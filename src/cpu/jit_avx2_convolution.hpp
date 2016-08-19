/*******************************************************************************
* Copyright 2016 Intel Corporation
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

#ifndef CPU_JIT_CONVOLUTION_HPP
#define CPU_JIT_CONVOLUTION_HPP

#include "c_types_map.hpp"
#include "convolution.hpp"
#include "jit_avx2_conv_generator_f32.hpp"

namespace mkldnn {
namespace impl {
namespace cpu {

template <impl::precision_t prec>
class jit_avx2_convolution:
    public convolution<jit_avx2_convolution<prec>> {
public:
    typedef typename prec_trait<prec>::type data_t;
    using convolution<jit_avx2_convolution<prec>>::convolution;
    jit_avx2_convolution(const convolution_primitive_desc_t &cpd,
            const primitive_at_t *inputs, const primitive *outputs[]);

    static status_t set_default_parameters(convolution_desc_t &conv_d);
    static status_t constraint(const convolution_desc_t &conv_d);

    static const primitive_impl implementation;
private:
    jit_convolution_param_t jcp;
    jit_avx2_conv_generator_f32 *generator;
    void (*jit_ker)(void *);

    status_t execute_forward();
};

}
}
}

#endif

// vim: et ts=4 sw=4 cindent cino^=l0,\:0,N-s
