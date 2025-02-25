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

#include <algorithm>

#include "gpu/ocl/simple_reorder.hpp"

#include "common/utils.hpp"
#include "gpu/ocl/ocl_stream.hpp"
#include "gpu/ocl/ocl_utils.hpp"
namespace dnnl {
namespace impl {
namespace gpu {
namespace ocl {

using namespace dnnl::impl::memory_tracking::names;

using dimension = struct {
    int size;
    int idx;
};

// Returns size and index of dimension or block that's last or at given
// distance from end. Blocks, if exist, take precedence before dimensions.
// Order of dimensions is determined by sorting strides; smallest stride is
// last dimension.
dimension get_Nth_last_dim_or_block(
        const memory_desc_wrapper &md, int distance = 0) {
    int nblks = md.blocking_desc().inner_nblks;
    dimension ret;
    if (nblks >= distance + 1) {
        ret.idx = md.blocking_desc().inner_idxs[nblks - 1 - distance];
        ret.size = md.blocking_desc().inner_blks[nblks - 1 - distance];
        return ret;
    } else {
        int ndims = md.ndims();
        int dim_distance = distance - nblks;

        assert(dim_distance < ndims);

        std::vector<std::pair<int, int>> strides(ndims);
        for (int d = 0; d < ndims; ++d) {
            strides[d].first = md.blocking_desc().strides[d];
            strides[d].second = d;
        }
        std::sort(strides.begin(), strides.end());
        ret.idx = strides[dim_distance].second;
        ret.size = md.padded_dims()[ret.idx];
        // if a dimension has size=1 then two dimensions will have the same strides
        // and the above sort is not guaranteed to select the correct dimension
        if (dim_distance < ndims - 1) {
            if (strides[dim_distance].first
                    == strides[dim_distance + 1].first) {
                ret.size = 1;
            }
        }
        return ret;
    }
}

int innermost_block(dnnl_blocking_desc_t blk) {
    int last = blk.inner_nblks - 1;
    return blk.inner_blks[last];
}

bool is_alt_faster_than_ref(const memory_desc_wrapper &src_mdw,
        const memory_desc_wrapper &dst_mdw,
        const compute::device_info_t *dev_info) {
    using namespace format_tag;
    int ndims = src_mdw.ndims();
    int last = ndims - 1;
    if (!src_mdw.matches_one_of_tag(abcd, abc, ab)) { return false; }
    // on GPUs newer than gen9 reference implementation is usually faster
    if (dev_info->gpu_arch() != compute::gpu_arch_t::gen9) { return false; }
    // ensure reasonable work group size
    if (src_mdw.dims()[last] < 8) { return false; }
    // abcd->???b reorders are faster with reference implementation
    if (ndims == 4 && dst_mdw.blocking_desc().strides[1] == 1) { return false; }
    if (ndims == 3 && dst_mdw.blocking_desc().strides[0] == 1) { return false; }
    return true;
}

bool matches_one_NxN_layout(
        const memory_desc_wrapper &src, const memory_desc_wrapper &dst, int n) {
    if (dst.ndims() < 2) { return false; }
    if (!src.is_blocking_desc() || !dst.is_blocking_desc()) { return false; }
    auto dst_last = get_Nth_last_dim_or_block(dst, 0);
    auto src_last = get_Nth_last_dim_or_block(src, 0);

    if (dst_last.size % n != 0) { return false; }
    if (src_last.size % n != 0) { return false; }
    if (dst_last.idx == src_last.idx) { return false; }
    // no padding allowed on dimensions that are last in src or last in dst
    if (src.padded_dims()[src_last.idx] != src.dims()[src_last.idx]) {
        return false;
    }
    if (src.padded_dims()[dst_last.idx] != src.dims()[dst_last.idx]) {
        return false;
    }
    if (dst.padded_dims()[src_last.idx] != dst.dims()[src_last.idx]) {
        return false;
    }
    if (dst.padded_dims()[dst_last.idx] != dst.dims()[dst_last.idx]) {
        return false;
    }
    return true;
}

bool matches_ABxxxx8ayb_layout(dnnl_blocking_desc_t blk, int ndims) {
    if (ndims > 2) { return false; }
    int last = blk.inner_nblks - 1;
    // Don't allow this kernel when two adjacent blocks by b create
    // total block size smaller than 16 - in that situation macros
    // used for calculation of dst address return wrong values.
    for (int d = last - 2; d >= 0; d--) {
        if (blk.inner_idxs[d] == ndims - 1) {
            int double_block = blk.inner_blks[last] * blk.inner_blks[d];
            if (double_block < 16) {
                return false;
            } else {
                break;
            }
        }
    }
    return ((blk.inner_blks[last] == 4 || blk.inner_blks[last] == 2)
            && blk.inner_idxs[last] == ndims - 1
            && blk.inner_blks[last - 1] == 8
            && blk.inner_idxs[last - 1] == ndims - 2);
}

bool dim_is_div_by_16_or_less_than_16(
        const memory_desc_wrapper &src, int dim_index) {
    const auto &padded_dims = src.padded_dims();
    assert(dim_index < src.ndims());
    return (padded_dims[dim_index] % 16 == 0 || padded_dims[dim_index] < 16);
}

bool is_broadcast_by_strides(const memory_desc_wrapper &mdw) {
    if (mdw.is_blocking_desc()) {
        for (int i = 0; i < mdw.ndims(); i++) {
            if (mdw.blocking_desc().strides[i] == 0) { return true; }
        }
    }
    return false;
}

bool is_padded(const memory_desc_wrapper &mdw, int dim) {
    return (mdw.dims()[dim] != mdw.padded_dims()[dim]);
}

bool fits_3ch(const memory_desc_wrapper &src_mdw,
        const memory_desc_wrapper &dst_mdw, int scale_mask) {
    // TODO: make it more generic, now it works only for dense->padded case
    if (src_mdw.ndims() < 2 || dst_mdw.ndims() < 2) { return false; }

    auto last_dim_src = get_Nth_last_dim_or_block(src_mdw);
    auto nextlast_dim_src = get_Nth_last_dim_or_block(src_mdw, 1);
    auto last_dim_dst = get_Nth_last_dim_or_block(dst_mdw);
    auto nextlast_dim_dst = get_Nth_last_dim_or_block(dst_mdw, 1);

    if (last_dim_src.idx != last_dim_dst.idx) { return false; }
    if (last_dim_src.size > last_dim_dst.size) { return false; }
    if (last_dim_dst.size > 16 || last_dim_dst.size % 8 != 0) { return false; }
    if (last_dim_src.idx == nextlast_dim_src.idx) { return false; }
    if (last_dim_src.idx == nextlast_dim_dst.idx) { return false; }
    if (nextlast_dim_src.idx == nextlast_dim_dst.idx) { return false; }
    if (nextlast_dim_src.size % 2 != 0) { return false; }
    if (nextlast_dim_dst.size % 2 != 0) { return false; }
    if (is_padded(src_mdw, last_dim_src.idx)) { return false; }
    if (is_padded(src_mdw, nextlast_dim_src.idx)) { return false; }
    if (is_padded(src_mdw, nextlast_dim_dst.idx)) { return false; }
    if (is_padded(dst_mdw, nextlast_dim_src.idx)) { return false; }
    if (is_padded(dst_mdw, nextlast_dim_dst.idx)) { return false; }
    if (scale_mask & (1 << last_dim_src.idx)) { return false; }
    if (scale_mask & (1 << nextlast_dim_src.idx)) { return false; }
    if (scale_mask & (1 << nextlast_dim_dst.idx)) { return false; }
    // no 2nd layer of block on innermost dim in dst
    if (dst_mdw.padded_dims()[last_dim_src.idx] != last_dim_dst.size) {
        return false;
    }
    return true;
}

reorder_kernel_t select_kernel(const reorder_conf_t &conf,
        const memory_desc_wrapper &src_mdw, const memory_desc_wrapper &dst_mdw,
        const compute::device_info_t *dev_info) {
    using namespace format_tag;

    const auto &padded_dims = dst_mdw.padded_dims();

    int last = conf.ndims - 1;
    size_t last_dim = padded_dims[last];

    const bool has_padding_or_scale_quant
            = conf.has_padding || conf.scale_quant;

    const bool type_s8_u8 = utils::one_of(src_mdw.data_type(), dnnl_s8, dnnl_u8)
            || utils::one_of(dst_mdw.data_type(), dnnl_s8, dnnl_u8);

    const bool allow_unroll
            = !conf.has_padding && !conf.scale_quant && !type_s8_u8;

    if (is_broadcast_by_strides(src_mdw) || is_broadcast_by_strides(dst_mdw)) {
        return reorder_kernel_t::reorder_reference;
    }

    if (!conf.scale_quant) {
        if (matches_one_NxN_layout(src_mdw, dst_mdw, 16)) {
            // W/A for compiler bug: avoid using intel_sub_group_shuffle with
            // SIMD16 on gen12lp
            if (dev_info->gpu_arch() == compute::gpu_arch_t::gen12lp) {
                return reorder_kernel_t::transpose8x8;
            }
            return reorder_kernel_t::transpose16x16;
        }
        if (matches_one_NxN_layout(src_mdw, dst_mdw, 8)) {
            return reorder_kernel_t::transpose8x8;
        }
    }
    if (src_mdw.matches_one_of_tag(nhwc) && dst_mdw.matches_one_of_tag(nchw)
            && padded_dims[last] % 16 == 0
            && dim_is_div_by_16_or_less_than_16(dst_mdw, 1)) {
        return reorder_kernel_t::reorder_nchw;
    }
    if (src_mdw.matches_one_of_tag(nhwc) && dst_mdw.matches_one_of_tag(nchw)
            && dim_is_div_by_16_or_less_than_16(dst_mdw, 1)) {
        return reorder_kernel_t::unaligned_sizes;
    }

    if (!has_padding_or_scale_quant && (conf.nelems % 256 == 0)
            && src_mdw.similar_to(dst_mdw, true, false, 0)
            && !has_padding_or_scale_quant) {
        return reorder_kernel_t::dense_vector;
    }

    if (fits_3ch(src_mdw, dst_mdw, conf.scale_mask)) {
        return reorder_kernel_t::pad_innermost;
    }
    // This kernel works on tensors that have common innermost dim. Tries to
    // access mem using large enough chunks to utilize whole cache lines.
    auto src_last = get_Nth_last_dim_or_block(src_mdw);
    auto dst_last = get_Nth_last_dim_or_block(dst_mdw);
    auto inner_dim = dst_last.idx;
    if (!has_padding_or_scale_quant && src_last.idx == dst_last.idx
            && src_last.size % 8 == 0 && dst_last.size % 8 == 0
            && conf.ndims <= MAX_NDIMS
            && (src_last.size % (2 * dst_last.size) == 0
                    || dst_last.size % (2 * src_last.size) == 0)
            && !(conf.scale_mask & (1 << inner_dim))
            && dst_mdw.dims()[inner_dim] == dst_mdw.padded_dims()[inner_dim]
            && src_mdw.dims()[inner_dim] == src_mdw.padded_dims()[inner_dim]) {
        return reorder_kernel_t::vectorize_groups;
    }

    if (allow_unroll) {
        if (src_mdw.matches_one_of_tag(ABc16a16b, ABc16b16a, ABcd16a16b,
                    ABcd16b16a, ABcde16a16b, ABcde16b16a, BAc16a16b, BAc16b16a,
                    BAcd16a16b, BAcd16b16a, BAcde16b16a)
                || dst_mdw.matches_one_of_tag(ABc16a16b, ABc16b16a, ABcd16a16b,
                        ABcd16b16a, ABcde16a16b, ABcde16b16a, BAc16a16b,
                        BAc16b16a, BAcd16a16b, BAcd16b16a, BAcde16b16a)) {
            return reorder_kernel_t::unroll_16a16b;
        }
        if (src_mdw.matches_one_of_tag(aBc16b, aBcd16b, aBcde16b)
                || dst_mdw.matches_one_of_tag(aBc16b, aBcd16b, aBcde16b)) {
            return reorder_kernel_t::unroll_16b;
        }
        if (src_mdw.matches_one_of_tag(aBCd16b16c, aBCd16c16b, aBCde16b16c,
                    aBCde16c16b, aBCdef16b16c, aBCdef16c16b, aCBd16b16c,
                    aCBd16c16b, aCBde16b16c, aCBde16c16b, aCBdef16c16b)
                || dst_mdw.matches_one_of_tag(aBCd16b16c, aBCd16c16b,
                        aBCde16b16c, aBCde16c16b, aBCdef16b16c, aBCdef16c16b,
                        aCBd16b16c, aCBd16c16b, aCBde16b16c, aCBde16c16b,
                        aCBdef16c16b)) {
            return reorder_kernel_t::unroll_16b16c;
        }
    }

    if (src_mdw.matches_one_of_tag(abdfce) && dst_mdw.matches_one_of_tag(abcdef)
            && ((padded_dims[conf.ndims - 2] % 16) == 0)
            && dim_is_div_by_16_or_less_than_16(dst_mdw, last)) {
        return reorder_kernel_t::plain_xFxE_to_abcdef;
    }

    if ((src_mdw.matches_one_of_tag(abcd) || src_mdw.matches_one_of_tag(acdb))
            && dst_mdw.matches_one_of_tag(/*ABcd4a2b,*/ ABcd4a4b)
            && src_mdw.is_dense() && dst_mdw.is_dense(true)
            && padded_dims[3] % 16 == 0) {
        return reorder_kernel_t::plain_to_ABcd4axb;
    }

    // This kernel will be used where last dimension is not reordered.
    // It will vectorize that dimension.
    if (!has_padding_or_scale_quant && src_mdw.is_dense() && dst_mdw.is_dense()
            && last_dim % 8 == 0
            && dst_mdw.md_->format_desc.blocking.strides[last] == 1
            && src_mdw.md_->format_desc.blocking.strides[last] == 1
            && conf.ndims <= MAX_NDIMS) {
        return reorder_kernel_t::vectorize_last_dim;
    }

    // This kernel supports 2D reorders into blocked formats that
    // end in 8a4b or 8a2b, no matter how many block layers, but no padding.
    if (!has_padding_or_scale_quant && src_mdw.matches_one_of_tag(ab)
            && matches_ABxxxx8ayb_layout(
                    dst_mdw.md_->format_desc.blocking, conf.ndims)
            && padded_dims[last] % 16 == 0) {
        return reorder_kernel_t::plain_to_ABxx8ayb;
    }

    if (conf.ndims >= 2 && conf.ndims <= 4
            && src_mdw.md_->format_desc.blocking.inner_nblks == 0
            && dst_mdw.md_->format_desc.blocking.inner_nblks == 0
            && is_alt_faster_than_ref(src_mdw, dst_mdw, dev_info)
            && !has_padding_or_scale_quant) {
        return reorder_kernel_t::reorder_alt;
    }

    return reorder_kernel_t::reorder_reference;
}

void simple_reorder_t::pd_t::alt_defines(
        compute::kernel_ctx_t &kernel_ctx) const {
    const memory_desc_wrapper src_mdw(src_md());
    const memory_desc_wrapper dst_mdw(dst_md());
    size_t ndims = src_mdw.ndims();
    size_t last = ndims - 1;

    auto sdim = src_mdw.dims();
    auto sstr = src_mdw.blocking_desc().strides;
    auto dstr = dst_mdw.blocking_desc().strides;
    kernel_ctx.define_int("ALT_OFFSETS", 1);
    if (conf.dispatch.nd_range().global_range()[0] != (size_t)sdim[0]) {
        kernel_ctx.define_int("LIMIT_MAX_D0", sdim[last]);
    }
    kernel_ctx.define_int("S0", sstr[last]);
    kernel_ctx.define_int("S1", sstr[last - 1]);
    kernel_ctx.define_int("S2", ndims > 2 ? sstr[last - 2] : 1);
    kernel_ctx.define_int("SB", ndims > 3 ? sstr[last - 3] : 1);
    kernel_ctx.define_int("D0", dstr[last]);
    kernel_ctx.define_int("D1", dstr[last - 1]);
    kernel_ctx.define_int("D2", ndims > 2 ? dstr[last - 2] : 1);
    kernel_ctx.define_int("DB", ndims > 3 ? dstr[last - 3] : 1);
    kernel_ctx.define_int("BLK", ndims > 3 ? sdim[last - 3] : 1);
}

void simple_reorder_t::pd_t::alt_gen() {
    const memory_desc_wrapper src_mdw(src_md());
    const memory_desc_wrapper dst_mdw(dst_md());
    auto sdim = src_mdw.dims();

    size_t last = src_mdw.ndims() - 1;
    size_t gws3 = src_mdw.ndims() > 2 ? sdim[last - 2] : 1;
    size_t gws[3] = {(size_t)sdim[last], (size_t)sdim[last - 1], gws3};
    size_t work_group_size = 32;
    if (sdim[last] <= 16) { work_group_size = 16; }
    if (sdim[last] <= 8) { work_group_size = 8; }
    const size_t lws[3] = {work_group_size, 1, 1};
    // Don't use nonuniform work groups, round up number work items if needed.
    size_t mod = gws[0] % lws[0];
    if (mod != 0) { gws[0] += lws[0] - mod; }
    conf.dispatch.generate_override(gws, lws);
}

status_t simple_reorder_t::pd_t::init_conf(engine_t *engine) {
    using namespace format_tag;

    const memory_desc_wrapper src_mdw(src_md());
    const memory_desc_wrapper dst_mdw(dst_md());

    conf.src_md_info = memory_desc_info_t::create(src_mdw);
    conf.dst_md_info = memory_desc_info_t::create(dst_mdw);

    status_t status = status::success;

    const auto &padded_dims = dst_mdw.padded_dims();
    conf.with_sum_ab = (alpha() != 1.f || beta() != 0.f);
    conf.scale_quant = attr()->output_scales_.mask_ != 0;
    conf.scale_mask = conf.scale_quant ? attr()->output_scales_.mask_ : 0;
    conf.scales_num = conf.scale_quant ? attr()->output_scales_.count_ : 0;
    conf.with_sum_a = conf.with_sum_ab && beta() == 0.f;
    conf.has_padding = !src_mdw.is_dense() || !dst_mdw.is_dense();
    conf.ndims = src_mdw.ndims();
    conf.nelems = utils::array_product(padded_dims, conf.ndims);

    conf.sub_group_size = 1;

    if (conf.nelems == 0) return status::success;

    int last = conf.ndims - 1;
    size_t last_dim = padded_dims[last];

    auto *compute_engine = utils::downcast<compute::compute_engine_t *>(engine);

    conf.implementation = select_kernel(
            conf, src_mdw, dst_mdw, compute_engine->device_info());

    dim_t blocks[MAX_NDIMS] = {1, 1, 1, 1, 1, 1};
    int vect_size = 1;
    int vect_dim = 0;

    conf.dispatch = compute_engine->create_dispatch(dst_mdw.md_);
    int temp_block = 1;

    switch (conf.implementation) {
        case reorder_reference:
            blocks[2] = blocks[3] = blocks[4] = blocks[5] = 0;
            break;
        case reorder_alt:
            // special handling with dispatcher override
            conf.sub_group_size = 16;
            break;
        case dense_vector:
            // see special handling below
            conf.sub_group_size = 16;
            break;
        case unroll_16b:
            conf.sub_group_size = 16;
            vect_dim = 1;
            vect_size = 16;
            break;
        case unroll_16b16c:
            conf.sub_group_size = 16;
            blocks[2] = 16;
            vect_dim = 1;
            vect_size = 16;
            break;
        case unroll_16a16b:
            conf.sub_group_size = 16;
            blocks[0] = 16;
            vect_dim = 1;
            vect_size = 16;
            break;
        case plain_to_ABcd4axb: {
            auto &blk = dst_mdw.blocking_desc();
            int b_block = blk.inner_blks[blk.inner_nblks - 1];
            conf.sub_group_size = (b_block == 2 ? 8 : 16);
            blocks[0] = 4;
            blocks[1] = b_block;
            vect_dim = 3;
            vect_size = conf.sub_group_size;
        } break;
        case vectorize_last_dim:
            for (int dim = last - 1;
                    dim >= 0 && dim < MAX_NDIMS && temp_block == 1; dim--) {
                if (padded_dims[dim] % 4 == 0) { temp_block = 4; }
                if (padded_dims[dim] % 8 == 0) { temp_block = 8; }
                if (padded_dims[dim] % 16 == 0) { temp_block = 16; }
                blocks[dim] = temp_block;
            }
            vect_dim = last;
            vect_size = (last_dim % 16 == 0) ? 16 : 8;
            break;
        case pad_innermost: {
            auto last_dim_src = get_Nth_last_dim_or_block(src_mdw);
            auto nextlast_dim_src = get_Nth_last_dim_or_block(src_mdw, 1);
            auto last_dim_dst = get_Nth_last_dim_or_block(dst_mdw);
            auto nextlast_dim_dst = get_Nth_last_dim_or_block(dst_mdw, 1);

            int min_common_size
                    = std::min(last_dim_src.size, last_dim_dst.size);
            int max_common_size
                    = std::max(last_dim_src.size, last_dim_dst.size);

            // Group size bigger than 4 would need too much private mem;
            // group size 1 will give worse perf than reference kernel.
            int max_group_size = 4;
            while (nextlast_dim_src.size % max_group_size != 0
                    || nextlast_dim_dst.size % max_group_size != 0) {
                max_group_size--;
            }

            conf.aux_data.vg.vector_dim = last_dim_src.idx;
            conf.aux_data.vg.group_size = max_group_size;
            conf.aux_data.vg.src_loop_dim = nextlast_dim_dst.idx;
            conf.aux_data.vg.dst_loop_dim = nextlast_dim_src.idx;
            conf.aux_data.vg.innermost_size = min_common_size;
            conf.sub_group_size = max_common_size;

            blocks[conf.aux_data.vg.src_loop_dim] = conf.aux_data.vg.group_size;
            blocks[conf.aux_data.vg.dst_loop_dim] = conf.aux_data.vg.group_size;
            vect_dim = conf.aux_data.vg.vector_dim;
            vect_size = conf.sub_group_size;
        } break;

        case vectorize_groups: {
            auto last_dim_src = get_Nth_last_dim_or_block(src_mdw);
            auto nextlast_dim_src = get_Nth_last_dim_or_block(src_mdw, 1);
            auto last_dim_dst = get_Nth_last_dim_or_block(dst_mdw);
            auto nextlast_dim_dst = get_Nth_last_dim_or_block(dst_mdw, 1);
            int min_common_size
                    = std::min(last_dim_src.size, last_dim_dst.size);
            vect_dim = last_dim_src.idx;
            vect_size = (min_common_size % 16 == 0) ? 16 : 8;
            assert(last_dim_src.size % vect_size == 0
                    && last_dim_dst.size % vect_size == 0);
            assert(last_dim_src.idx == last_dim_dst.idx);
            int src_chunks;
            if (last_dim_src.size / vect_size > 1) {
                src_chunks = last_dim_src.size / vect_size;
                conf.aux_data.vg.dst_loop_dim = last_dim_src.idx;
            } else {
                src_chunks = nextlast_dim_src.size;
                conf.aux_data.vg.dst_loop_dim = nextlast_dim_src.idx;
            }
            int dst_chunks;
            if (last_dim_dst.size / vect_size > 1) {
                dst_chunks = last_dim_dst.size / vect_size;
                conf.aux_data.vg.src_loop_dim = last_dim_dst.idx;
            } else {
                dst_chunks = nextlast_dim_dst.size;
                conf.aux_data.vg.src_loop_dim = nextlast_dim_dst.idx;
            }
            // TODO:
            // Final algorithm for selecting group size should consider:
            // 1. Group size must be small enough to guarantee no spill.
            // 2. Group size should be large enough to fill whole cache lines
            //    on both reads and writes, with line size determined by HW
            // 3. If there's not enough data to feed all EUs, ignore (2) and
            //    decrease group size.
            int max_data_size = (int)std::max(
                    src_mdw.data_type_size(), dst_mdw.data_type_size());

            int group = 16 / max_data_size;
            while (group > 1) {
                if (src_chunks % group == 0 && dst_chunks % group == 0) {
                    break;
                }
                group--;
            }
            assert(group >= 1);

            conf.aux_data.vg.vector_dim = last_dim_src.idx;
            conf.aux_data.vg.group_size = group;
            conf.sub_group_size = vect_size;

            blocks[conf.aux_data.vg.src_loop_dim] = group;
            blocks[conf.aux_data.vg.dst_loop_dim] = group;
        } break;
        case plain_to_ABxx8ayb:
            conf.sub_group_size = 16;
            blocks[0] = 8;
            vect_dim = last;
            vect_size = 16;
            break;
        case plain_xFxE_to_abcdef:
            conf.sub_group_size = 16;
            blocks[5] = nstl::min(padded_dims[conf.ndims - 1], dnnl_dim_t(16));
            vect_dim = 4;
            vect_size = 16;
            break;
        case transpose8x8:
            conf.sub_group_size = 8;
            blocks[get_Nth_last_dim_or_block(dst_mdw).idx] = 8;
            vect_dim = get_Nth_last_dim_or_block(src_mdw).idx;
            vect_size = 8;
            break;
        case transpose16x16:
            conf.sub_group_size = 16;
            blocks[get_Nth_last_dim_or_block(dst_mdw).idx] = 16;
            vect_dim = get_Nth_last_dim_or_block(src_mdw).idx;
            vect_size = 16;
            break;
        case reorder_nchw:
            conf.sub_group_size = 16;
            blocks[1] = nstl::min(padded_dims[1], dnnl_dim_t(16));
            vect_dim = 3;
            vect_size = 16;
            break;
        case unaligned_sizes: blocks[1] = padded_dims[1]; break;
    }

    // special case for dense_vector kernel - treat tensors as flat 1D vectors
    if (conf.implementation == dense_vector) {
        conf.dispatch.define_dim("D0", 0, conf.nelems, 16);
        conf.dispatch.vectorize_dim("D0", 16);
    } else {
        for (int i = 0; i < MAX_NDIMS; ++i) {
            auto dim_str = utils::format("D%d", i);
            if (i < dst_mdw.ndims()) {
                conf.dispatch.define_dim(dim_str, i, padded_dims[i], blocks[i]);
            } else {
                conf.dispatch.define_dim(dim_str, 1);
            }
        }
        if (vect_size != 1) {
            auto dim_str = utils::format("D%d", vect_dim);
            conf.dispatch.vectorize_dim(dim_str, vect_size);
        }
    }

    if (conf.implementation == reorder_alt) {
        alt_gen();
    } else {
        conf.dispatch.generate();
    }
    return status;
}

status_t simple_reorder_t::pd_t::init_kernel_ctx(
        compute::kernel_ctx_t &kernel_ctx) const {
    using namespace format_tag;

    const memory_desc_wrapper src_mdw(src_md());
    const memory_desc_wrapper dst_mdw(dst_md());

    if (conf.nelems == 0) return status::success;

    kernel_ctx.define_int("NDIMS", conf.ndims);
    kernel_ctx.add_option("-cl-std=CL2.0");

    if (conf.with_sum_a)
        kernel_ctx.define_int("WITH_SUM_A", 1);
    else if (conf.with_sum_ab)
        kernel_ctx.define_int("WITH_SUM_AB", 1);

    if (conf.scale_quant) {
        kernel_ctx.define_int("SCALE_QUANT", 1);
        kernel_ctx.define_int("SCALE_MASK", conf.scale_mask);
    }

    def_dispatch(kernel_ctx, conf.dispatch);

    // the 'unaligned_sizes' kernel uses the same implementation in .cl
    // the difference is in sizes of blocks[]
    kernel_ctx.define_int("REF_REORDER",
            conf.implementation == reorder_reference
                    || conf.implementation == unaligned_sizes);
    kernel_ctx.define_int("SUB_GROUP_SIZE", conf.sub_group_size);

    kernel_ctx.define_int("PAD_FILL_ZERO", conf.has_padding);
    if (conf.implementation == dense_vector) {
        kernel_ctx.add_option("-Dcl_intel_subgroups_char");
        kernel_ctx.define_int("USE_DENSE_VECT", 1);
    }

    def_memory_desc_info(kernel_ctx, conf.src_md_info, "SRC");
    def_memory_desc_info(kernel_ctx, conf.dst_md_info, "DST");

    // distinguish between various flavors of unroll kernel
    if (src_mdw.matches_one_of_tag(
                ABc16a16b, ABcd16a16b, ABcde16a16b, BAc16a16b, BAcd16a16b)) {
        kernel_ctx.define_int("SRC_16A16B", 1);
    } else if (src_mdw.matches_one_of_tag(ABc16b16a, ABcd16b16a, ABcde16b16a,
                       BAc16b16a, BAcd16b16a, BAcde16b16a)) {
        kernel_ctx.define_int("SRC_16B16A", 1);
    } else if (src_mdw.matches_one_of_tag(aBc16b, aBcd16b, aBcde16b)) {
        kernel_ctx.define_int("SRC_16B", 1);
    } else if (src_mdw.matches_one_of_tag(aBCd16b16c, aBCde16b16c, aBCdef16b16c,
                       aCBd16b16c, aCBde16b16c)) {
        kernel_ctx.define_int("SRC_16B16C", 1);
    } else if (src_mdw.matches_one_of_tag(aBCd16c16b, aBCde16c16b, aBCdef16c16b,
                       aCBd16c16b, aCBde16c16b, aCBdef16c16b)) {
        kernel_ctx.define_int("SRC_16C16B", 1);
    }
    if (dst_mdw.matches_one_of_tag(
                ABc16a16b, ABcd16a16b, ABcde16a16b, BAc16a16b, BAcd16a16b)) {
        kernel_ctx.define_int("DST_16A16B", 1);
    } else if (dst_mdw.matches_one_of_tag(ABc16b16a, ABcd16b16a, ABcde16b16a,
                       BAc16b16a, BAcd16b16a, BAcde16b16a)) {
        kernel_ctx.define_int("DST_16B16A", 1);
    } else if (dst_mdw.matches_one_of_tag(aBc16b, aBcd16b, aBcde16b)) {
        kernel_ctx.define_int("DST_16B", 1);
    } else if (dst_mdw.matches_one_of_tag(aBCd16b16c, aBCde16b16c, aBCdef16b16c,
                       aCBd16b16c, aCBde16b16c)) {
        kernel_ctx.define_int("DST_16B16C", 1);
    } else if (dst_mdw.matches_one_of_tag(aBCd16c16b, aBCde16c16b, aBCdef16c16b,
                       aCBd16c16b, aCBde16c16b, aCBdef16c16b)) {
        kernel_ctx.define_int("DST_16C16B", 1);
    }

    if (conf.implementation == reorder_alt) { alt_defines(kernel_ctx); }
    if (conf.implementation == plain_xFxE_to_abcdef)
        kernel_ctx.define_int("PLAIN_xFxE_TO_ABCDEF", 1);

    if (conf.implementation == plain_to_ABcd4axb)
        kernel_ctx.define_int("PLAIN_TO_ABCD4AXB", 1);

    if (conf.implementation == vectorize_last_dim) {
        kernel_ctx.define_int("VECTORIZE_LAST_DIM", 1);
    }
    if (conf.implementation == pad_innermost) {
        kernel_ctx.define_int("PAD_INNERMOST", 1);
        kernel_ctx.define_int(
                "VECT_DIM", conf.aux_data.vg.vector_dim); //useless
        kernel_ctx.define_int("SRC_LOOP_DIM", conf.aux_data.vg.src_loop_dim);
        kernel_ctx.define_int("DST_LOOP_DIM", conf.aux_data.vg.dst_loop_dim);
        kernel_ctx.define_int("GROUP", conf.aux_data.vg.group_size);
        auto lr = conf.dispatch.nd_range().local_range();
        kernel_ctx.define_int(
                "SG_PER_WG", (lr[0] * lr[1] * lr[2]) / conf.sub_group_size);
        kernel_ctx.define_int(
                "INNERMOST_SIZE", conf.aux_data.vg.innermost_size);
        kernel_ctx.define_int("VECT_SIZE", conf.sub_group_size);
    }
    if (conf.implementation == vectorize_groups) {
        kernel_ctx.define_int("VECTORIZE_GROUPS", 1);
        kernel_ctx.define_int("VECT_DIM", conf.aux_data.vg.vector_dim);
        kernel_ctx.define_int("SRC_LOOP_DIM", conf.aux_data.vg.src_loop_dim);
        kernel_ctx.define_int("DST_LOOP_DIM", conf.aux_data.vg.dst_loop_dim);
        kernel_ctx.define_int("GROUP", conf.aux_data.vg.group_size);
    }
    if (conf.implementation == plain_to_ABxx8ayb) {
        kernel_ctx.define_int("PLAIN_TO_AB_XX_8AYB", 1);
        kernel_ctx.define_int(
                "BLK_L", innermost_block(dst_mdw.md_->format_desc.blocking));
    }

    if (conf.implementation == transpose8x8
            || conf.implementation == transpose16x16) {
        kernel_ctx.define_int("TRANSPOSE_NXN", 1);
        kernel_ctx.define_int(
                "DST_BLOCK_DIM", get_Nth_last_dim_or_block(src_mdw).idx);
    }

    if (conf.implementation == reorder_nchw) {
        kernel_ctx.define_int("REORDER_NCHW", 1);
    }

    kernel_ctx.print_options();
    return status::success;
}

void simple_reorder_t::pd_t::init_scratchpad() {
    if (conf.scales_num > 0) {
        auto scratchpad = scratchpad_registry().registrar();
        scratchpad.book(memory_tracking::names::key_reorder_scales,
                conf.scales_num, sizeof(float), OCL_BUFFER_ALIGNMENT);
    }
}

status_t simple_reorder_t::execute(const exec_ctx_t &ctx) const {

    status_t status = status::success;

    auto &src = CTX_IN_STORAGE(DNNL_ARG_FROM);
    auto &dst = CTX_OUT_STORAGE(DNNL_ARG_TO);
    CHECK(status);

    const auto &conf = pd()->conf;
    if (conf.nelems == 0) return status::success;

    float alpha = pd()->alpha();
    float beta = pd()->beta();

    std::unique_ptr<memory_storage_t> scales;
    if (conf.scale_quant) {
        scales = ctx.get_scratchpad_grantor().get_memory_storage(
                key_reorder_scales);

        void *tmp_ptr = nullptr;
        status = scales->map_data(&tmp_ptr, ctx.stream(),
                sizeof(float) * pd()->attr()->output_scales_.count_);
        if (status != status::success) return status;
        utils::array_copy((float *)tmp_ptr,
                pd()->attr()->output_scales_.scales_,
                pd()->attr()->output_scales_.count_);
        status = scales->unmap_data(tmp_ptr, ctx.stream());
        if (status != status::success) return status;
    }

    compute::kernel_arg_list_t arg_list;
    arg_list.set(0, src);
    arg_list.set(1, dst);
    arg_list.set(2, alpha);
    arg_list.set(3, beta);
    arg_list.set(4, scales ? *scales : memory_storage_t::empty_storage());

    auto nd_range = conf.dispatch.nd_range();
    status = parallel_for(ctx, nd_range, kernel_, arg_list);

    return status;
}

} // namespace ocl
} // namespace gpu
} // namespace impl
} // namespace dnnl
