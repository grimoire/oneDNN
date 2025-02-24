/*******************************************************************************
* Copyright 2017-2022 Intel Corporation
*
* Licensed under the Apache License, Version 2.0 (the "License");
* you may not use this file except in compliance with the License.
* You may obtain a copy of the License at
*
*     http://www.apache.org/licenses/LICENSE-2.0
*
* Unless required by applicable law or agreed to in writing, software
* distributed under the License is distributed on an "AS IS" BASIS,
* WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
* See the License for the specific language governing permissions and
* limitations under the License.
*******************************************************************************/

#include "utils/parallel.hpp"

#include "conv/conv_common.hpp"
#include "conv/deconv.hpp"

namespace conv {

void compute_ref_direct_fwd(const prb_t *prb, const args_t &args) {
    const dnn_mem_t &src_m = args.find(DNNL_ARG_SRC);
    const dnn_mem_t &wei_m = args.find(DNNL_ARG_WEIGHTS);
    const dnn_mem_t &bia_m = args.find(DNNL_ARG_BIAS);
    const dnn_mem_t &dst_m = args.find(DNNL_ARG_DST);
    /* help compiler optimize the code */
    const int64_t MB = prb->mb, G = prb->g, OC = prb->oc, IC = prb->ic;
    const int64_t OCG = OC / G, ICG = IC / G;
    const int64_t OD = prb->od, OH = prb->oh, OW = prb->ow;
    const int64_t ID = prb->id, IH = prb->ih, IW = prb->iw;
    const int64_t SD = prb->sd, SH = prb->sh, SW = prb->sw;
    const int64_t PD = prb->pd, PH = prb->ph, PW = prb->pw;
    const int64_t KD = prb->kd, KH = prb->kh, KW = prb->kw;
    const int64_t DD = prb->dd + 1;
    const int64_t DH = prb->dh + 1;
    const int64_t DW = prb->dw + 1;

    auto ker = [&](float &d, int64_t g, int64_t mb, int64_t oc, int64_t od,
                       int64_t oh, int64_t ow) {
        const float *__restrict src_loc
                = (const float *)src_m + (mb * IC + g * ICG) * ID * IH * IW;
        const float *__restrict wei_loc
                = (const float *)wei_m + (g * OCG + oc) * ICG * KD * KH * KW;

        for (int64_t kd = 0; kd < KD; ++kd) {
            const int64_t id = od * SD - PD + kd * DD;
            if (id < 0 || id >= ID) continue;
            for (int64_t kh = 0; kh < KH; ++kh) {
                const int64_t ih = oh * SH - PH + kh * DH;
                if (ih < 0 || ih >= IH) continue;
                for (int64_t kw = 0; kw < KW; ++kw) {
                    const int64_t iw = ow * SW - PW + kw * DW;
                    if (iw < 0 || iw >= IW) continue;

                    for (int64_t ic = 0; ic < ICG; ++ic) {
                        int64_t src_off = ((ic * ID + id) * IH + ih) * IW + iw;
                        int64_t wei_off = ((ic * KD + kd) * KH + kh) * KW + kw;
                        float s = src_loc[src_off];
                        maybe_zero_point(prb->attr, s, prb->src_zp,
                                g * ICG + ic, DNNL_ARG_SRC);
                        d += s * wei_loc[wei_off];
                    }
                }
            }
        }
    };

    auto v_po_masks = prb->attr.post_ops.get_po_masks();
    benchdnn_parallel_nd(G, MB, OCG, OD, OH, OW,
            [&](int64_t g, int64_t mb, int64_t oc, int64_t od, int64_t oh,
                    int64_t ow) {
                const size_t dst_off = dst_off_f(prb, mb, g, oc, od, oh, ow);
                float &dst = ((float *)dst_m)[dst_off];

                float conv_res = 0;
                ker(conv_res, g, mb, oc, od, oh, ow);

                if (prb->dir & FLAG_BIA) {
                    const size_t bia_off = bia_off_f(prb, g, oc);
                    conv_res += ((float *)bia_m)[bia_off];
                }

                maybe_oscale(prb->attr, conv_res, prb->scales, g * OCG + oc);

                const auto v_po_vals
                        = prepare_po_vals(dst_m, args, v_po_masks, dst_off);

                maybe_post_ops(prb->attr, conv_res, dst, v_po_vals);

                maybe_zero_point(prb->attr, conv_res, prb->dst_zp, g * OCG + oc,
                        DNNL_ARG_DST, true);

                dst = conv_res;
            });
}

void compute_ref_direct_bwd_d(const prb_t *prb, const args_t &args) {
    const dnn_mem_t &diff_src_m = args.find(DNNL_ARG_DIFF_SRC);
    const dnn_mem_t &wei_m = args.find(DNNL_ARG_WEIGHTS);
    const dnn_mem_t &bia_m = args.find(DNNL_ARG_BIAS);
    const dnn_mem_t &diff_dst_m = args.find(DNNL_ARG_DIFF_DST);
    /* help compiler optimize the code */
    const int64_t MB = prb->mb, G = prb->g, OC = prb->oc, IC = prb->ic;
    const int64_t OCG = OC / G, ICG = IC / G;
    const int64_t OD = prb->od, OH = prb->oh, OW = prb->ow;
    const int64_t ID = prb->id, IH = prb->ih, IW = prb->iw;
    const int64_t SD = prb->sd, SH = prb->sh, SW = prb->sw;
    const int64_t PD = prb->pd, PH = prb->ph, PW = prb->pw;
    const int64_t KD = prb->kd, KH = prb->kh, KW = prb->kw;
    const int64_t DD = prb->dd + 1;
    const int64_t DH = prb->dh + 1;
    const int64_t DW = prb->dw + 1;

    enum { precompute_size = 16 };
    const bool fast = MAX3(KD, KH, KW) <= precompute_size;

    // from bwd pov zp src from fwd is zp diff dst and
    // zp dst is zp dst is zp diff_src
    const auto map_arg_to_zp_arg = [](int num) {
        switch (num) {
            case DNNL_ARG_DIFF_DST: return DNNL_ARG_SRC;
            case DNNL_ARG_DIFF_SRC: return DNNL_ARG_DST;
            default: assert(false && "map_arg_to_zp_arg unsupported arg");
        }

        return -1;
    };

    /* pre-computes arrays of oh(ow) and kh(kw) for traversing in kernel */
    auto precompute_ok
            = [](int64_t i, int64_t O, int64_t K, int64_t S, int64_t P,
                      int64_t D, int64_t &num, int64_t *_o, int64_t *_k) {
                  assert(K <= precompute_size);
                  num = 0;
                  for (int64_t k = 0; k < K; ++k) {
                      int64_t o = i - k * D + P;
                      if (o < 0 || o % S) continue;
                      o /= S;
                      if (o >= O) continue;
                      _k[num] = k;
                      _o[num] = o;
                      ++num;
                  }
              };

    auto ker_fast = [&](float &ds, int64_t g, int64_t mb, int64_t ic,
                            int64_t id, int64_t ih, int64_t iw) {
        int64_t kd[precompute_size], od[precompute_size], num_d;
        int64_t kh[precompute_size], oh[precompute_size], num_h;
        int64_t kw[precompute_size], ow[precompute_size], num_w;
        precompute_ok(id, OD, KD, SD, PD, DD, num_d, od, kd);
        precompute_ok(ih, OH, KH, SH, PH, DH, num_h, oh, kh);
        precompute_ok(iw, OW, KW, SW, PW, DW, num_w, ow, kw);

        const float *__restrict diff_dst_loc = (const float *)diff_dst_m
                + (mb * OC + g * OCG) * OD * OH * OW;
        const float *__restrict wei_loc
                = (const float *)wei_m + ((g * OCG) * ICG + ic) * KD * KH * KW;

        for_(int64_t d = 0; d < num_d; ++d)
        for_(int64_t h = 0; h < num_h; ++h)
        for (int64_t w = 0; w < num_w; ++w) {
            for (int64_t oc = 0; oc < OCG; ++oc) {
                const int64_t diff_dst_off
                        = ((oc * OD + od[d]) * OH + oh[h]) * OW + ow[w];
                const int64_t wei_off
                        = ((oc * ICG * KD + kd[d]) * KH + kh[h]) * KW + kw[w];
                float diff_dst_val = diff_dst_loc[diff_dst_off];
                maybe_zero_point(prb->attr, diff_dst_val, prb->src_zp,
                        g * OCG + oc, map_arg_to_zp_arg(DNNL_ARG_DIFF_DST));
                ds += diff_dst_val * wei_loc[wei_off];
            }
        }
    };

    auto ker = [&](float &ds, int64_t g, int64_t mb, int64_t ic, int64_t id,
                       int64_t ih, int64_t iw) {
        const float *__restrict diff_dst_loc = (const float *)diff_dst_m
                + (mb * OC + g * OCG) * OD * OH * OW;
        const float *__restrict wei_loc
                = (const float *)wei_m + ((g * OCG) * ICG + ic) * KD * KH * KW;

        for (int64_t kd = 0; kd < KD; ++kd) {
            int64_t od = id - kd * DD + PD;
            if (od < 0 || od % SD || od >= OD * SD) continue;
            od /= SD;
            for (int64_t kh = 0; kh < KH; ++kh) {
                int64_t oh = ih - kh * DH + PH;
                if (oh < 0 || oh % SH || oh >= OH * SH) continue;
                oh /= SH;
                for (int64_t kw = 0; kw < KW; ++kw) {
                    int64_t ow = iw - kw * DW + PW;
                    if (ow < 0 || ow % SW || ow >= OW * SW) continue;
                    ow /= SW;
                    for (int64_t oc = 0; oc < OCG; ++oc) {
                        const int64_t diff_dst_off
                                = ((oc * OD + od) * OH + oh) * OW + ow;
                        const int64_t wei_off
                                = ((oc * ICG * KD + kd) * KH + kh) * KW + kw;
                        float diff_dst_val = diff_dst_loc[diff_dst_off];
                        maybe_zero_point(prb->attr, diff_dst_val, prb->src_zp,
                                g * OCG + oc,
                                map_arg_to_zp_arg(DNNL_ARG_DIFF_DST));

                        ds += diff_dst_val * wei_loc[wei_off];
                    }
                }
            }
        }
    };

    auto v_po_masks = prb->attr.post_ops.get_po_masks();
    benchdnn_parallel_nd(G, MB, ICG, ID, IH, IW,
            [&](int64_t g, int64_t mb, int64_t ic, int64_t id, int64_t ih,
                    int64_t iw) {
                size_t src_off = src_off_f(prb, mb, g, ic, id, ih, iw);
                float &ds = ((float *)diff_src_m)[src_off];
                float conv_res = 0;
                if (fast)
                    ker_fast(conv_res, g, mb, ic, id, ih, iw);
                else
                    ker(conv_res, g, mb, ic, id, ih, iw);

                if (prb->dir & FLAG_BIA) {
                    const size_t bia_off = (size_t)g * ICG + ic;
                    conv_res += ((float *)bia_m)[bia_off];
                }
                maybe_oscale(prb->attr, conv_res, prb->scales, g * ICG + ic);

                const auto v_po_vals = prepare_po_vals(
                        diff_src_m, args, v_po_masks, src_off);

                maybe_post_ops(prb->attr, conv_res, ds, v_po_vals);
                maybe_zero_point(prb->attr, conv_res, prb->dst_zp, g * ICG + ic,
                        map_arg_to_zp_arg(DNNL_ARG_DIFF_SRC), true);

                ds = conv_res;
            });
}

void compute_ref_bwd_weights(const prb_t *prb, const args_t &args) {
    const dnn_mem_t &src_m = args.find(DNNL_ARG_SRC);
    const dnn_mem_t &diff_wei_m = args.find(DNNL_ARG_DIFF_WEIGHTS);
    const dnn_mem_t &diff_dst_m = args.find(DNNL_ARG_DIFF_DST);
    /* help compiler optimize the code */
    const int64_t MB = prb->mb, G = prb->g, OC = prb->oc, IC = prb->ic;
    const int64_t OCG = OC / G, ICG = IC / G;
    const int64_t OD = prb->od, OH = prb->oh, OW = prb->ow;
    const int64_t ID = prb->id, IH = prb->ih, IW = prb->iw;
    const int64_t SD = prb->sd, SH = prb->sh, SW = prb->sw;
    const int64_t PD = prb->pd, PH = prb->ph, PW = prb->pw;
    const int64_t KD = prb->kd, KH = prb->kh, KW = prb->kw;
    const int64_t DD = prb->dd + 1;
    const int64_t DH = prb->dh + 1;
    const int64_t DW = prb->dw + 1;

    auto compute_bounds
            = [](int64_t I, int64_t O, int64_t k, int64_t S, int64_t P,
                      int64_t D, int64_t &o_s, int64_t &o_e) {
                  const float tmp = P - k * D;
                  o_s = MAX2(0, ceilf(tmp / S));
                  o_e = MIN2(O, ceilf((I + tmp) / S));
              };

    auto ker = [&](float &dw, int64_t g, int64_t oc, int64_t ic, int64_t kd,
                       int64_t kh, int64_t kw) {
        int64_t od_s, od_e, oh_s, oh_e, ow_s, ow_e;
        compute_bounds(ID, OD, kd, SD, PD, DD, od_s, od_e);
        compute_bounds(IH, OH, kh, SH, PH, DH, oh_s, oh_e);
        compute_bounds(IW, OW, kw, SW, PW, DW, ow_s, ow_e);
        const int64_t id_s = kd * DD - PD;
        const int64_t ih_s = kh * DH - PH;
        const int64_t iw_s = kw * DW - PW;

        for (int64_t mb = 0; mb < MB; ++mb) {
            const float *__restrict diff_dst_loc = (const float *)diff_dst_m
                    + (mb * OC + g * OCG + oc) * OD * OH * OW;
            const float *__restrict src_loc = (const float *)src_m
                    + (mb * IC + g * ICG + ic) * ID * IH * IW;

            for_(int64_t od = od_s; od < od_e; ++od)
            for_(int64_t oh = oh_s; oh < oh_e; ++oh)
            for (int64_t ow = ow_s; ow < ow_e; ++ow) {
                const int64_t id = od * SD + id_s;
                const int64_t ih = oh * SH + ih_s;
                const int64_t iw = ow * SW + iw_s;

                size_t diff_dst_off = (od * OH + oh) * OW + ow;
                size_t src_off = (id * IH + ih) * IW + iw;
                dw += diff_dst_loc[diff_dst_off] * src_loc[src_off];
            }
        }
    };

    benchdnn_parallel_nd(G, OCG, ICG, KD, KH, KW,
            [&](int64_t g, int64_t oc, int64_t ic, int64_t kd, int64_t kh,
                    int64_t kw) {
                size_t wei_off = wei_off_f(prb, g, oc, ic, kd, kh, kw);
                float &dw = ((float *)diff_wei_m)[wei_off];
                dw = 0;
                ker(dw, g, oc, ic, kd, kh, kw);
            });
}

void compute_ref_bwd_bias(const prb_t *prb, const args_t &args) {
    const dnn_mem_t &diff_bia_m = args.find(DNNL_ARG_DIFF_BIAS);
    const dnn_mem_t &diff_dst_m = args.find(DNNL_ARG_DIFF_DST);
    /* help compiler optimize the code */
    const int64_t MB = prb->mb, G = prb->g, OC = prb->oc;
    const int64_t OCG = OC / G;
    const int64_t OD = prb->od, OH = prb->oh, OW = prb->ow;

    benchdnn_parallel_nd(G, OCG, [&](int64_t g, int64_t oc) {
        size_t bia_off = bia_off_f(prb, g, oc);
        double sum = 0;

        for_(int64_t mb = 0; mb < MB; ++mb)
        for_(int64_t od = 0; od < OD; ++od)
        for_(int64_t oh = 0; oh < OH; ++oh)
        for (int64_t ow = 0; ow < OW; ++ow) {
            size_t dst_off = dst_off_f(prb, mb, g, oc, od, oh, ow);
            sum += ((float *)diff_dst_m)[dst_off];
        }
        ((float *)diff_bia_m)[bia_off] = (float)sum;
    });
}

void compute_ref_direct_bwd_w(const prb_t *prb, const args_t &args) {
    compute_ref_bwd_weights(prb, args);
    if (!(prb->dir & FLAG_BIA)) return;
    compute_ref_bwd_bias(prb, args);
}

void compute_ref_fwd(
        const prb_t *prb, const args_t &args, dnnl_primitive_t prim_ref) {
    if (prim_ref) {
        SAFE_V(execute_and_wait(prim_ref, args));
        return;
    }

    if (prb->alg == WINO && prb->cfg[SRC].dt == dnnl_f32) {
        compute_wino_ref_fwd(prb, args);
    } else {
        compute_ref_direct_fwd(prb, args);
    }
}

void compute_ref_bwd_d(
        const prb_t *prb, const args_t &args, dnnl_primitive_t prim_ref) {
    if (prim_ref) {
        SAFE_V(execute_and_wait(prim_ref, args));
        return;
    }

    if (prb->alg == WINO && prb->cfg[SRC].dt == dnnl_f32) {
        compute_wino_ref_bwd_d(prb, args);
    } else {
        compute_ref_direct_bwd_d(prb, args);
    }
}

void compute_ref_bwd_w(
        const prb_t *prb, const args_t &args, dnnl_primitive_t prim_ref) {
    if (prim_ref) {
        SAFE_V(execute_and_wait(prim_ref, args));
        return;
    }

    if (prb->alg == WINO && prb->cfg[SRC].dt == dnnl_f32) {
        compute_wino_ref_bwd_w(prb, args);
    } else {
        compute_ref_direct_bwd_w(prb, args);
    }
}

void compute_ref(
        const prb_t *prb, const args_t &args, dnnl_primitive_t prim_ref) {
    // Since deconv uses conv::prb_t, using a common templated interface for
    // correctness validation requires compute_ref function to be in the same
    // namespace, thus, we need to dispatch to deconv::compute_ref here. The
    // alternative solution is to separate deconv and conv drivers completely.
    if (prb->is_deconv) {
        deconv::compute_ref(prb, args, prim_ref);
        return;
    }

    if (prb->dir & FLAG_FWD)
        compute_ref_fwd(prb, args, prim_ref);
    else if (prb->dir == BWD_D)
        compute_ref_bwd_d(prb, args, prim_ref);
    else if (prb->dir & FLAG_BWD && prb->dir & FLAG_WEI)
        compute_ref_bwd_w(prb, args, prim_ref);
}

} // namespace conv
