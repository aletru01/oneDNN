/*******************************************************************************
* Copyright 2019-2021 Intel Corporation
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

#ifndef CPU_X64_JIT_UNI_DW_CONV_KERNEL_UTILS_HPP
#define CPU_X64_JIT_UNI_DW_CONV_KERNEL_UTILS_HPP

#include "common/nstl.hpp"
#include "common/type_helpers.hpp"
#include "common/utils.hpp"
#include <type_traits>

#include "common/c_types_map.hpp"
#include "common/memory_tracking.hpp"

#include "cpu/x64/injectors/injector_utils.hpp"
#include "cpu/x64/injectors/jit_uni_binary_injector.hpp"
#include "cpu/x64/injectors/jit_uni_eltwise_injector.hpp"
#include "cpu/x64/injectors/jit_uni_postops_injector.hpp"
#include "cpu/x64/jit_generator.hpp"
#include "cpu/x64/jit_primitive_conf.hpp"

#include "cpu/x64/jit_avx512_core_bf16_dw_conv_kernel.hpp"
#include "cpu/x64/jit_uni_dw_conv_kernel_f32.hpp"

namespace dnnl {
namespace impl {
namespace cpu {
namespace x64 {

template <cpu_isa_t isa, data_type_t kernel_dt>
struct jit_uni_dw_conv_fwd_kernel {

    jit_uni_dw_conv_fwd_kernel(
            const jit_conv_conf_t &ajcp, const memory_desc_t &dst_md) {
        ker_ = new jit_kernel_t(ajcp, dst_md);
    }

    status_t create_kernel() { return ker_->create_kernel(); }
    ~jit_uni_dw_conv_fwd_kernel() { delete ker_; }

    static status_t init_conf(jit_conv_conf_t &jcp,
            const convolution_desc_t &cd, memory_desc_t &src_md,
            memory_desc_t &weights_md, memory_desc_t &bias_md,
            memory_desc_t &dst_md, const primitive_attr_t &attr);

    static void init_scratchpad(memory_tracking::registrar_t &scratchpad,
            const jit_conv_conf_t &jcp);

    jit_generator *ker() const { return ker_; }
    void operator()(const jit_conv_call_s *p) const { (*ker_)(p); }

private:
    constexpr static bool ker_condition_
            = isa == avx512_core && kernel_dt == data_type::bf16;
    using jit_kernel_t = typename utils::conditional<ker_condition_,
            jit_avx512_dw_conv_fwd_kernel_bf16,
            jit_uni_dw_conv_fwd_kernel_f32<isa>>::type;
    jit_kernel_t *ker_;
};

template <cpu_isa_t isa, data_type_t kernel_dt>
status_t jit_uni_dw_conv_fwd_kernel<isa, kernel_dt>::init_conf(
        jit_conv_conf_t &jcp, const convolution_desc_t &cd,
        memory_desc_t &src_md, memory_desc_t &weights_md,
        memory_desc_t &bias_md, memory_desc_t &dst_md,
        const primitive_attr_t &attr) {

    using namespace dnnl::impl::format_tag;
    using namespace dnnl::impl::utils;

    const memory_desc_wrapper src_d(&src_md);
    const memory_desc_wrapper weights_d(&weights_md);
    const memory_desc_wrapper dst_d(&dst_md);
    const memory_desc_wrapper bias_d(&bias_md);

    const int ndims = src_d.ndims();
    // Currently this kernel only supports 2D convolutions.
    if (ndims != 4) return status::unimplemented;

    jcp.prop_kind = cd.prop_kind;

    const auto blocked_tag
            = one_of(isa, avx512_common, avx512_core) ? nChw16c : nChw8c;
    const auto wei_tag
            = one_of(isa, avx512_common, avx512_core) ? Goihw16g : Goihw8g;
    const auto nxc_tag = nhwc;
    const auto def_tag
            = (mayiuse(avx512_core)
                      && jcp.prop_kind == prop_kind::forward_inference)
            ? nxc_tag
            : blocked_tag;

    jcp.with_bias = cd.bias_desc.format_kind != format_kind::undef;

    if (src_d.format_kind() == format_kind::any) {
        CHECK(memory_desc_init_by_tag(src_md, def_tag));
        jcp.src_tag = def_tag;
    } else {
        jcp.src_tag = src_d.matches_one_of_tag(blocked_tag, nxc_tag);
    }

    if (weights_d.format_kind() == format_kind::any) {
        CHECK(memory_desc_init_by_tag(weights_md, wei_tag));
        jcp.wei_tag = wei_tag;
    } else {
        jcp.wei_tag = weights_d.matches_one_of_tag(wei_tag);
    }

    if (dst_d.format_kind() == format_kind::any) {
        CHECK(memory_desc_init_by_tag(dst_md, def_tag));
        jcp.dst_tag = def_tag;
    } else {
        jcp.dst_tag = dst_d.matches_one_of_tag(blocked_tag, nxc_tag);
    }

    if (jcp.with_bias) {
        if (bias_d.format_kind() == format_kind::any)
            CHECK(memory_desc_init_by_tag(bias_md, format_tag::x));
    }

    if (jcp.dst_tag != jcp.src_tag) return status::unimplemented;
    const auto data_tag = jcp.src_tag;
    const bool is_data_layout_nxc = data_tag == nxc_tag;

    const bool is_bf16 = src_d.data_type() == data_type::bf16;

    jcp.dst_dt = cd.dst_desc.data_type;
    jcp.isa = (is_bf16 && mayiuse(avx512_core_bf16)) ? avx512_core_bf16 : isa;

    if (!mayiuse(isa) || (is_bf16 && !mayiuse(avx512_core)))
        return status::unimplemented;

    const int simd_w = one_of(isa, avx512_common, avx512_core) ? 16 : 8;

    const bool with_groups = weights_d.ndims() == src_d.ndims() + 1;
    if (!with_groups) return status::unimplemented;

    jcp.ngroups = weights_d.dims()[0];
    jcp.mb = src_d.dims()[0];

    jcp.oc = dst_d.dims()[1];
    jcp.oc_without_padding = jcp.oc;
    jcp.ic = src_d.dims()[1];

    jcp.ih = src_d.dims()[2];
    jcp.iw = src_d.dims()[3];
    jcp.oh = dst_d.dims()[2];
    jcp.ow = dst_d.dims()[3];

    jcp.kh = weights_d.dims()[3];
    jcp.kw = weights_d.dims()[4];

    jcp.t_pad = cd.padding[0][0];
    jcp.l_pad = cd.padding[0][1];

    jcp.stride_h = cd.strides[0];
    jcp.stride_w = cd.strides[1];

    jcp.dilate_h = cd.dilates[0];
    jcp.dilate_w = cd.dilates[1];

    jcp.typesize_out = types::data_type_size(dst_d.data_type());
    jcp.typesize_in = types::data_type_size(src_d.data_type());

    jcp.loop_order = loop_ngcw;

    jcp.ur_w = is_bf16 ? (isa_has_bf16(jcp.isa) ? 6 : 4)
                       : isa == avx512_common ? 6 : isa == avx2 ? 4 : 3;
    jcp.ur_w = nstl::min(jcp.ur_w, jcp.ow);

    jcp.ch_block = simd_w;
    jcp.nb_ch = div_up(jcp.oc, jcp.ch_block);
    jcp.nb_ch_blocking
            = one_of(isa, avx512_common, avx512_core) ? 4 : isa == avx2 ? 3 : 2;
    if (jcp.nb_ch < jcp.nb_ch_blocking) jcp.nb_ch_blocking = jcp.nb_ch;

    if (is_data_layout_nxc) {
        jcp.loop_order = loop_nhwcg;
        const int resrc_depthwise_ur_w = (31 - jcp.kw + jcp.stride_w)
                / (jcp.nb_ch_blocking + jcp.stride_w);
        jcp.is_resrc_depthwise = (!is_bf16)
                && one_of(isa, avx512_common, avx512_core)
                && jcp.stride_w < jcp.kw && jcp.kw <= 5 && jcp.dilate_w == 0
                && resrc_depthwise_ur_w >= 2;
        if (jcp.is_resrc_depthwise) {
            jcp.ur_w = nstl::min(jcp.ow, resrc_depthwise_ur_w);
        }
        bool cache_aliasing
                = (jcp.ngroups * jcp.iw * jcp.typesize_in) % 1024 == 0;
        if (cache_aliasing) {
            // currently only tuned for mobilenet-v1 shapes
            const int limit = jcp.ow > 7 ? 7 : 4;
            jcp.ur_w = nstl::min(jcp.ur_w, limit);
        }
    }

    jcp.ur_w_tail = jcp.ow % jcp.ur_w;

    int ext_kw = calculate_extended_filter_size(jcp.kw, jcp.dilate_w);
    int ext_kh = calculate_extended_filter_size(jcp.kh, jcp.dilate_h);
    jcp.r_pad = calculate_end_padding(
            jcp.l_pad, jcp.ow, jcp.iw, jcp.stride_w, ext_kw);
    jcp.b_pad = calculate_end_padding(
            jcp.t_pad, jcp.oh, jcp.ih, jcp.stride_h, ext_kh);
    bool kernel_outside_src = false || ext_kw <= jcp.l_pad
            || ext_kw <= jcp.r_pad || ext_kh <= jcp.t_pad
            || ext_kh <= jcp.b_pad;
    if (kernel_outside_src) return status::unimplemented;
    int r_pad_no_tail = nstl::max(0,
            calculate_end_padding(jcp.l_pad, jcp.ow - jcp.ur_w_tail, jcp.iw,
                    jcp.stride_w, ext_kw));
    if (jcp.l_pad > jcp.ur_w || r_pad_no_tail > jcp.ur_w)
        return status::unimplemented;

    const auto &post_ops = attr.post_ops_;

    jcp.with_sum = post_ops.find(primitive_kind::sum) != -1;
    const int eltwise_ind = post_ops.find(primitive_kind::eltwise);
    jcp.with_eltwise = eltwise_ind != -1;
    if (jcp.with_eltwise) jcp.eltwise = post_ops.entry_[eltwise_ind].eltwise;
    const int binary_ind = post_ops.find(primitive_kind::binary);
    jcp.with_binary = binary_ind != -1;
    if (jcp.with_binary) {
        using namespace dnnl::impl::cpu::binary_injector_utils;
        std::tie(jcp.with_binary_per_oc_bcast, jcp.with_binary_no_bcast)
                = bcast_strategies_present_tup(post_ops.entry_, dst_d,
                        broadcasting_strategy_t::per_oc,
                        broadcasting_strategy_t::no_broadcast);
    }

    jcp.post_ops = post_ops;

    using namespace injector;
    static constexpr bool sum_at_pos_0_only = true;
    static constexpr bool sum_requires_scale_one = true;
    const bool post_ops_ok_ = post_ops_ok({isa, {eltwise, binary, sum},
            jcp.post_ops, &dst_d, sum_at_pos_0_only, sum_requires_scale_one});
    if (!post_ops_ok_) return status::unimplemented;

    const bool ok_to_pad_channels = true && !is_data_layout_nxc
            && jcp.oc == jcp.ngroups && jcp.ic == jcp.ngroups
            && one_of(isa, avx512_common, avx512_core, avx2);
    if (ok_to_pad_channels) {
        jcp.oc = rnd_up(jcp.oc, simd_w);
        jcp.ic = rnd_up(jcp.oc, simd_w);
        jcp.ngroups = rnd_up(jcp.ngroups, simd_w);
    }

    const bool args_ok = true && jcp.oc == jcp.ngroups && jcp.ic == jcp.ngroups
            && IMPLICATION(!is_data_layout_nxc, jcp.ngroups % simd_w == 0)
            && jcp.wei_tag == wei_tag && data_tag != format_tag::undef
            && jcp.ic <= src_d.padded_dims()[1]
            && jcp.oc <= dst_d.padded_dims()[1]
            && jcp.ngroups <= weights_d.padded_dims()[0];
    if (!args_ok) return status::unimplemented;

    jcp.bia_dt = jcp.with_bias ? cd.bias_desc.data_type : data_type::undef;

    return status::success;
}

template <cpu_isa_t isa, data_type_t kernel_dt>
void jit_uni_dw_conv_fwd_kernel<isa, kernel_dt>::init_scratchpad(
        memory_tracking::registrar_t &scratchpad, const jit_conv_conf_t &jcp) {
    using namespace dnnl::impl::memory_tracking::names;
    if (jcp.bia_dt == data_type::bf16)
        scratchpad.book<float>(key_conv_bias_bf16_convert_wsp, jcp.oc);
    else if (jcp.with_bias && jcp.oc_without_padding != jcp.oc)
        scratchpad.book<float>(key_conv_padded_bias, jcp.oc);
}

template struct jit_uni_dw_conv_fwd_kernel<avx512_core, data_type::bf16>;
template struct jit_uni_dw_conv_fwd_kernel<avx512_common, data_type::f32>;
template struct jit_uni_dw_conv_fwd_kernel<avx2, data_type::f32>;
template struct jit_uni_dw_conv_fwd_kernel<sse41, data_type::f32>;

template <cpu_isa_t isa, data_type_t kernel_dt>
struct jit_uni_dw_conv_bwd_data_kernel {

    jit_uni_dw_conv_bwd_data_kernel(const jit_conv_conf_t &ajcp)
        : ker_(nullptr) {
        ker_ = new jit_kernel_t(ajcp);
    }

    status_t create_kernel() { return ker_->create_kernel(); }
    ~jit_uni_dw_conv_bwd_data_kernel() { delete ker_; }

    static status_t init_conf(jit_conv_conf_t &jcp,
            const convolution_desc_t &cd, const memory_desc_wrapper &diff_src_d,
            const memory_desc_wrapper &weights_d,
            const memory_desc_wrapper &diff_dst_d);

    static void init_scratchpad(memory_tracking::registrar_t &scratchpad,
            const jit_conv_conf_t &jcp);

    void operator()(const jit_conv_call_s *p) const { (*ker_)(p); }

private:
    using jit_kernel_t = typename utils::conditional<isa == avx512_core
                    && kernel_dt == data_type::bf16,
            jit_avx512_dw_conv_bwd_data_kernel_bf16,
            jit_uni_dw_conv_bwd_data_kernel_f32<isa>>::type;
    jit_kernel_t *ker_;

    DNNL_DISALLOW_COPY_AND_ASSIGN(jit_uni_dw_conv_bwd_data_kernel);
};

template <cpu_isa_t isa, data_type_t kernel_dt>
status_t jit_uni_dw_conv_bwd_data_kernel<isa, kernel_dt>::init_conf(
        jit_conv_conf_t &jcp, const convolution_desc_t &cd,
        const memory_desc_wrapper &diff_src_d,
        const memory_desc_wrapper &weights_d,
        const memory_desc_wrapper &diff_dst_d) {
    using namespace dnnl::impl::format_tag;
    using namespace dnnl::impl::utils;

    jcp.dsrc_dt = cd.diff_src_desc.data_type;
    const bool is_bf16 = diff_dst_d.data_type() == data_type::bf16;
    jcp.isa = (is_bf16 && mayiuse(avx512_core_bf16)) ? avx512_core_bf16 : isa;

    if (!mayiuse(isa) || (is_bf16 && !mayiuse(avx512_core)))
        return status::unimplemented;

    const int simd_w = one_of(isa, avx512_common, avx512_core) ? 16 : 8;

    const bool with_groups = weights_d.ndims() == diff_src_d.ndims() + 1;
    if (!with_groups) return status::unimplemented;

    jcp.ngroups = weights_d.dims()[0];
    jcp.mb = diff_src_d.dims()[0];

    jcp.oc = diff_dst_d.dims()[1];
    jcp.oc_without_padding = jcp.oc;
    jcp.ic = diff_src_d.dims()[1];

    jcp.ih = diff_src_d.dims()[2];
    jcp.iw = diff_src_d.dims()[3];
    jcp.oh = diff_dst_d.dims()[2];
    jcp.ow = diff_dst_d.dims()[3];

    jcp.kh = weights_d.dims()[3];
    jcp.kw = weights_d.dims()[4];

    jcp.t_pad = cd.padding[0][0];
    jcp.l_pad = cd.padding[0][1];

    jcp.stride_h = cd.strides[0];
    jcp.stride_w = cd.strides[1];

    jcp.dilate_h = cd.dilates[0];
    jcp.dilate_w = cd.dilates[1];

    const int ext_kw = calculate_extended_filter_size(jcp.kw, jcp.dilate_w);
    const int ext_kh = calculate_extended_filter_size(jcp.kh, jcp.dilate_h);
    jcp.r_pad = calculate_end_padding(
            jcp.l_pad, jcp.ow, jcp.iw, jcp.stride_w, ext_kw);
    jcp.b_pad = calculate_end_padding(
            jcp.t_pad, jcp.oh, jcp.ih, jcp.stride_h, ext_kh);

    jcp.ihp = jcp.ih + jcp.t_pad + jcp.b_pad;
    jcp.iwp = jcp.iw + jcp.l_pad + jcp.r_pad;

    bool ok_to_pad_channels = true && jcp.oc == jcp.ngroups
            && jcp.ic == jcp.ngroups
            && one_of(isa, avx512_common, avx512_core, avx2);
    if (ok_to_pad_channels) {
        jcp.oc = rnd_up(jcp.oc, simd_w);
        jcp.ic = rnd_up(jcp.oc, simd_w);
        jcp.ngroups = rnd_up(jcp.ngroups, simd_w);
    }

    auto dat_tag = one_of(isa, avx512_common, avx512_core) ? nChw16c : nChw8c;
    auto wei_tag = one_of(isa, avx512_common, avx512_core) ? Goihw16g : Goihw8g;

    jcp.src_tag = diff_src_d.matches_one_of_tag(dat_tag);
    jcp.wei_tag = weights_d.matches_one_of_tag(wei_tag);
    jcp.dst_tag = diff_dst_d.matches_one_of_tag(dat_tag);

    bool args_ok = true && jcp.oc == jcp.ngroups && jcp.ic == jcp.ngroups
            && jcp.ngroups % simd_w == 0 && jcp.dilate_h == 0
            && jcp.dilate_w == 0 && jcp.src_tag == dat_tag
            && jcp.wei_tag == wei_tag && jcp.dst_tag == dat_tag
            && jcp.oh == (jcp.ihp - jcp.kh) / jcp.stride_h + 1
            && jcp.ow == (jcp.iwp - jcp.kw) / jcp.stride_w + 1
            && jcp.ic <= diff_src_d.padded_dims()[1]
            && jcp.oc <= diff_dst_d.padded_dims()[1]
            && jcp.ngroups <= weights_d.padded_dims()[0];
    if (!args_ok) return status::unimplemented;

    jcp.typesize_out = types::data_type_size(diff_src_d.data_type());
    jcp.typesize_in = types::data_type_size(diff_dst_d.data_type());

    jcp.ur_w = is_bf16 ? (isa_has_bf16(jcp.isa) ? 6 : 4)
                       : isa == avx512_common ? 6 : isa == avx2 ? 4 : 3;

    jcp.ch_block = simd_w;
    jcp.nb_ch = jcp.ic / jcp.ch_block;
    jcp.nb_ch_blocking
            = one_of(isa, avx512_common, avx512_core) ? 4 : isa == avx2 ? 3 : 2;
    if (jcp.nb_ch < jcp.nb_ch_blocking) jcp.nb_ch_blocking = jcp.nb_ch;

    return status::success;
}

template <cpu_isa_t isa, data_type_t kernel_dt>
void jit_uni_dw_conv_bwd_data_kernel<isa, kernel_dt>::init_scratchpad(
        memory_tracking::registrar_t &scratchpad, const jit_conv_conf_t &jcp) {
    UNUSED(scratchpad);
    UNUSED(jcp);
}

template struct jit_uni_dw_conv_bwd_data_kernel<avx512_core, data_type::bf16>;
template struct jit_uni_dw_conv_bwd_data_kernel<avx512_common, data_type::f32>;
template struct jit_uni_dw_conv_bwd_data_kernel<avx2, data_type::f32>;
template struct jit_uni_dw_conv_bwd_data_kernel<sse41, data_type::f32>;

template <cpu_isa_t isa, data_type_t kernel_dt>
struct jit_uni_dw_conv_bwd_weights_kernel {

    jit_uni_dw_conv_bwd_weights_kernel(const jit_conv_conf_t &ajcp)
        : ker_(nullptr) {
        ker_ = new jit_kernel_t(ajcp);
    }

    status_t create_kernel() { return ker_->create_kernel(); }

    ~jit_uni_dw_conv_bwd_weights_kernel() { delete ker_; }

    static status_t init_conf(jit_conv_conf_t &jcp,
            const convolution_desc_t &cd, const memory_desc_wrapper &src_d,
            const memory_desc_wrapper &diff_weights_d,
            const memory_desc_wrapper &diff_dst_d, int nthreads);

    static void init_scratchpad(memory_tracking::registrar_t &scratchpad,
            const jit_conv_conf_t &jcp);

    static void balance(jit_conv_conf_t &jcp, int nthreads);

    void operator()(const jit_dw_conv_call_s *p) const { (*ker_)(p); }

private:
    using jit_kernel_t = typename utils::conditional<isa == avx512_core
                    && kernel_dt == data_type::bf16,
            jit_avx512_dw_conv_bwd_weights_kernel_bf16,
            jit_uni_dw_conv_bwd_weights_kernel_f32<isa>>::type;
    jit_kernel_t *ker_;
};

template <cpu_isa_t isa, data_type_t kernel_dt>
status_t jit_uni_dw_conv_bwd_weights_kernel<isa, kernel_dt>::init_conf(
        jit_conv_conf_t &jcp, const convolution_desc_t &cd,
        const memory_desc_wrapper &src_d,
        const memory_desc_wrapper &diff_weights_d,
        const memory_desc_wrapper &diff_dst_d, int nthreads) {
    using namespace dnnl::impl::format_tag;
    using namespace dnnl::impl::utils;

    jcp.dwei_dt = cd.diff_weights_desc.data_type;
    const bool is_bf16 = src_d.data_type() == data_type::bf16;
    jcp.isa = (is_bf16 && mayiuse(avx512_core_bf16)) ? avx512_core_bf16 : isa;

    if (!mayiuse(isa) || (is_bf16 && !mayiuse(avx512_core)))
        return status::unimplemented;

    jcp.ngroups = diff_weights_d.dims()[0];
    jcp.oc = diff_dst_d.dims()[1] / jcp.ngroups;
    jcp.ic = src_d.dims()[1] / jcp.ngroups;

    const bool with_groups = diff_weights_d.ndims() == src_d.ndims() + 1;

    jcp.is_depthwise = true && with_groups && everyone_is(1, jcp.oc, jcp.ic);

    if (!jcp.is_depthwise) return status::unimplemented;

    jcp.ch_block = one_of(isa, avx512_common, avx512_core) ? 16 : 8;

    jcp.mb = src_d.dims()[0];

    jcp.ih = src_d.dims()[2];
    jcp.iw = src_d.dims()[3];
    jcp.oh = diff_dst_d.dims()[2];
    jcp.ow = diff_dst_d.dims()[3];

    jcp.kh = diff_weights_d.dims()[3];
    jcp.kw = diff_weights_d.dims()[4];

    jcp.stride_h = cd.strides[0];
    jcp.stride_w = cd.strides[1];

    jcp.t_pad = cd.padding[0][0];
    jcp.l_pad = cd.padding[0][1];

    jcp.dilate_h = cd.dilates[0];
    jcp.dilate_w = cd.dilates[1];

    jcp.with_bias = cd.diff_bias_desc.format_kind != format_kind::undef;

    const int ext_kw = calculate_extended_filter_size(jcp.kw, jcp.dilate_w);
    const int ext_kh = calculate_extended_filter_size(jcp.kh, jcp.dilate_h);
    jcp.r_pad = nstl::max(0,
            calculate_end_padding(
                    jcp.l_pad, jcp.ow, jcp.iw, jcp.stride_w, ext_kw));
    jcp.b_pad = nstl::max(0,
            calculate_end_padding(
                    jcp.t_pad, jcp.oh, jcp.ih, jcp.stride_h, ext_kh));

    jcp.ihp = jcp.ih + jcp.t_pad + jcp.b_pad;
    jcp.iwp = jcp.iw + jcp.l_pad + jcp.r_pad;

    auto dat_tag = one_of(isa, avx512_common, avx512_core) ? nChw16c : nChw8c;
    auto wei_tag = one_of(isa, avx512_common, avx512_core) ? Goihw16g : Goihw8g;

    jcp.src_tag = src_d.matches_one_of_tag(dat_tag);
    jcp.wei_tag = diff_weights_d.matches_one_of_tag(wei_tag);
    jcp.dst_tag = diff_dst_d.matches_one_of_tag(dat_tag);

    bool args_ok = true && jcp.src_tag == dat_tag && jcp.wei_tag == wei_tag
            && jcp.dst_tag == dat_tag && jcp.ngroups % jcp.ch_block == 0
            && jcp.dilate_h == 0 && jcp.dilate_w == 0 && jcp.kw <= 3
            && jcp.stride_w <= jcp.kw // no gaps in kernel
            && jcp.oh == (jcp.ihp - jcp.kh) / jcp.stride_h + 1
            && jcp.ow == (jcp.iwp - jcp.kw) / jcp.stride_w + 1;
    if (!args_ok) return status::unimplemented;

    jcp.nb_ch = jcp.ngroups / jcp.ch_block;

    /* kernel applicability check wrt boundaries
     * the conditions are quite general across the kernels we have,
     * but ideally the check should belong to a specific kernel... */
    const int max_hpad = (jcp.kh - 1 + 1) / 2;
    const int max_wpad = (jcp.kw - 1 + 1) / 2;
    const int min_ih = jcp.kh + nstl::modulo(-jcp.t_pad, jcp.stride_h);
    const bool boundaries_ok = true && jcp.t_pad <= max_hpad
            && jcp.b_pad <= max_hpad && jcp.l_pad <= max_wpad
            && jcp.r_pad <= max_wpad
            // input must fully accommodate the filter
            && jcp.ih >= min_ih
            // non-unit padding must be a multiple of the stride
            && IMPLICATION(jcp.t_pad > 1, jcp.t_pad % jcp.stride_h == 0)
            && IMPLICATION(jcp.b_pad > 1, jcp.b_pad % jcp.stride_h == 0);
    if (!boundaries_ok) return status::unimplemented;

    /* BF16: accumulation of output happens in f32, down-conversion to bf16
     * happens during the reduction phase. */
    jcp.typesize_out = sizeof(float);
    jcp.typesize_in = types::data_type_size(src_d.data_type());
    jcp.bia_dt = jcp.with_bias ? cd.diff_bias_desc.data_type : data_type::undef;

    balance(jcp, nthreads);

    return status::success;
}

template <cpu_isa_t isa, data_type_t kernel_dt>
void jit_uni_dw_conv_bwd_weights_kernel<isa, kernel_dt>::init_scratchpad(
        memory_tracking::registrar_t &scratchpad, const jit_conv_conf_t &jcp) {
    using namespace dnnl::impl::memory_tracking::names;
    /* Notes: if splitting thread work on 'mb', then a reduction has to take
     * place. Hence, book a per-thread, local weights-buffer for the
     * reduction */
    if (jcp.nthr_mb > 1) {
        const size_t mb = jcp.dwei_dt == data_type::bf16 ? jcp.nthr_mb
                                                         : jcp.nthr_mb - 1;
        const size_t wei_size = jcp.ngroups * jcp.kh * jcp.kw;
        scratchpad.book<float>(key_conv_wei_reduction, wei_size * mb);

        if (jcp.with_bias)
            scratchpad.book<float>(
                    key_conv_bia_reduction, jcp.ngroups * (jcp.nthr_mb - 1));
    } else if (jcp.nthr_mb == 1 && jcp.dwei_dt == data_type::bf16) {
        const size_t wei_size = jcp.ngroups * jcp.kh * jcp.kw;
        scratchpad.book<float>(key_conv_wei_reduction, wei_size);
    }
    if (jcp.bia_dt == data_type::bf16)
        scratchpad.book<float>(key_conv_bias_bf16_convert_wsp, jcp.ngroups);
}

template <cpu_isa_t isa, data_type_t kernel_dt>
void jit_uni_dw_conv_bwd_weights_kernel<isa, kernel_dt>::balance(
        jit_conv_conf_t &jcp, int nthreads) {
    jcp.nthr = nthreads;
    jcp.nthr_g = jcp.nthr_mb = 1;

    /* Basic-Heuristics for parallel strategy:
     * 1) Tries to parallel on the number of Groups (g) where tasks are
     * independent. Otherwise,
     * 2) Tries to split the work across g and MiniBatch (mb).
     * Parallelizing on mb requires computing a reduction for weights.
     *
     * NOTE: because of 'task partitioning' scheme, there will be unbalanced
     * per-thread load when the number of threads is high (e.g. > 16).
     */
    jcp.nthr_g = nstl::min(jcp.nb_ch, jcp.nthr);
    jcp.nthr_mb = nstl::min(nstl::max(1, jcp.nthr / jcp.nthr_g), jcp.mb);

    jcp.nthr = jcp.nthr_g * jcp.nthr_mb;
}

template struct jit_uni_dw_conv_bwd_weights_kernel<avx512_core,
        data_type::bf16>;
template struct jit_uni_dw_conv_bwd_weights_kernel<avx512_common,
        data_type::f32>;
template struct jit_uni_dw_conv_bwd_weights_kernel<avx2, data_type::f32>;
template struct jit_uni_dw_conv_bwd_weights_kernel<sse41, data_type::f32>;
} // namespace x64
} // namespace cpu
} // namespace impl
} // namespace dnnl
#endif /* CPU_X64_JIT_UNI_DW_CONV_KERNEL_UTILS_HPP */
