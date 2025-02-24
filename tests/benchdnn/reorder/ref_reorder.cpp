/*******************************************************************************
* Copyright 2022 Intel Corporation
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

#include "reorder/reorder.hpp"

namespace reorder {

void compute_ref(
        const prb_t *prb, const args_t &args, dnnl_primitive_t prim_ref) {
    const dnn_mem_t &src = args.find(DNNL_ARG_FROM);
    const dnn_mem_t &dst = args.find(DNNL_ARG_TO);
    const dnn_mem_t &s8_comp = args.find(DNNL_ARG_SRC_1);
    const dnn_mem_t &zp_comp = args.find(DNNL_ARG_SRC_2);

    float *dst_ptr = (float *)dst;

    auto dst_dt = prb->conf_out->dt;
    const auto nelems = src.nelems();
    const int scale_mask = attr_t::get_default_mask(prb->attr.oscale.policy);
    // This is native to reorder zero point which comes from reorder attributes.
    const int src_zero_point = prb->src_zp ? prb->src_zp[0] : 0;
    const int dst_zero_point = prb->dst_zp ? prb->dst_zp[0] : 0;

    float beta = 0;
    const auto &po = prb->attr.post_ops;
    const int beta_idx = po.find(attr_t::post_ops_t::kind_t::SUM);
    if (beta_idx >= 0) beta = po.entry[beta_idx].sum.scale;

    // These are non-native compensations coming from other primitives with
    // s8s8 or zero-points support to pre-compute compensated part and apply it
    // at the end of computations.
    const bool need_s8_comp = s8_comp.dt() == dnnl_s32;
    const bool need_zp_comp = zp_comp.dt() == dnnl_s32;
    const bool need_comp = need_s8_comp || need_zp_comp;
    // `adjust_scale` participates only with s8s8 compensation.
    const float s8_scale_factor = need_s8_comp ? reorder_rescale_factor() : 1.f;

    benchdnn_parallel_nd(nelems, [&](int64_t idx) {
        float s = src.get_elem(idx) - src_zero_point;
        float d = 0;
        if (beta_idx >= 0) d = dst.get_elem(idx) - dst_zero_point;

        const int64_t scale_idx = dst.get_scale_idx(idx, scale_mask);
        const float alpha = prb->scales[scale_idx];
        float value = s8_scale_factor * alpha * s + beta * d + dst_zero_point;
        value = maybe_saturate(dst_dt, value);
        if (dst_dt == dnnl_s32 && value >= (float)INT_MAX)
            value = BENCHDNN_S32_TO_F32_SAT_CONST;

        dst_ptr[idx] = round_to_nearest_representable(dst_dt, value);
    });

    if (!need_comp) return;

    int *s8_comp_ptr = (int *)s8_comp;
    int *zp_comp_ptr = (int *)zp_comp;

    // mostly following benchdnn/ref_reduction.cpp/compute_ref
    const auto nelems_s8_comp = s8_comp.nelems();
    const auto nelems_zp_comp = zp_comp.nelems();
    const auto nelems_comp = MAX2(nelems_s8_comp, nelems_zp_comp);
    const auto &ndims = src.ndims();
    const auto &src_dims = src.md_.dims;
    assert(nelems_comp > 0);
    assert(IMPLICATION(
            need_s8_comp && need_zp_comp, nelems_s8_comp == nelems_zp_comp));

    int comp_mask = 0;
    for (const auto &i_oflag : prb->oflag) {
        if ((i_oflag.first == FLAG_S8S8_COMP || i_oflag.first == FLAG_ZP_COMP)
                && i_oflag.second != FLAG_NONE) {
            comp_mask = i_oflag.second;
            break;
        }
    }

    dims_t comp_dims(ndims, 1); // src_dims with '1' at non-masked dims.
    dims_t reduce_dims(ndims, 1); // complementary to above.
    for (int i = 0; i < ndims; ++i) {
        if (comp_mask & (1 << i)) {
            comp_dims[i] = src_dims[i];
            reduce_dims[i] = 1;
        } else {
            comp_dims[i] = 1;
            reduce_dims[i] = src_dims[i];
        }
    }

    const auto nelems_reduce = nelems / nelems_comp;
    benchdnn_parallel_nd(nelems_comp, [&](int64_t f) {
        dims_t idle_pos = off2dims_idx(comp_dims, f);
        const int64_t src_idle_off = md_off_v(src.md_, idle_pos.data());
        int comp_val = 0;
        for (int64_t r = 0; r < nelems_reduce; ++r) {
            dims_t reduce_pos = off2dims_idx(reduce_dims, r);
            const int64_t src_reduce_off = md_off_v(src.md_, reduce_pos.data());
            const int64_t src_off = src_idle_off + src_reduce_off;
            const int64_t scale_idx = dst.get_scale_idx(src_off, scale_mask);
            const float alpha = prb->scales[scale_idx];
            const float value = src.get_elem(src_off) * alpha * s8_scale_factor;
            comp_val -= maybe_saturate(dst_dt, value);
        }
        if (need_zp_comp) zp_comp_ptr[f] = comp_val;
        comp_val *= 128;
        if (need_s8_comp) s8_comp_ptr[f] = comp_val;
    });
}

} // namespace reorder
