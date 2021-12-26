/*******************************************************************************
* Copyright 2020-2021 Intel Corporation
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

#ifndef BACKEND_DNNL_OPERATORS_BATCHNORM_HPP
#define BACKEND_DNNL_OPERATORS_BATCHNORM_HPP

#include <memory>
#include <string>
#include <vector>

#include "interface/backend.hpp"

#include "backend/dnnl/common.hpp"
#include "backend/dnnl/legacy.hpp"
#include "backend/dnnl/tensor.hpp"
#include "backend/dnnl/utils.hpp"
#include "sum.hpp"

#ifdef DNNL_GRAPH_WITH_SYCL
#include <CL/sycl.hpp>
#include <dnnl_sycl.hpp>
#endif

namespace dnnl {
namespace graph {
namespace impl {
namespace dnnl_impl {

namespace batch_normalization {
enum batch_normalization_inputs { kSrc, kScale, kShift, kMean, kVariance };
enum batch_normalization_outputs {
    kDst,
    kRunning_mean,
    kRunning_variance,
    kBatch_mean,
    kBatch_variance
};
} // namespace batch_normalization

namespace batch_normalization_bwd {
enum batch_normalization_bwd_inputs {
    kSrc,
    kDiff_dst,
    kScale,
    kMean,
    kVariance
};
enum batch_normalization_bwd_outputs { kDiff_src, kDiff_scale, kDiff_shift };
} // namespace batch_normalization_bwd

struct batch_normalization_forward_inference
    : public dnnl::batch_normalization_forward,
      public kernel_base {

    using super = dnnl::batch_normalization_forward;
    using dims_t = std::vector<dnnl::graph::impl::dim_t>;

private:
    primitive_desc pd_;
    dnnl::batch_normalization_forward prim_;
    float epsilon_;

    tensor scale_shift_;
    tensor expected_mean_;
    tensor expected_var_;

    dnnl::engine p_engine_;
    dnnl::stream p_stream_;

    // FIXME(qun) NOT well designed
    /// \note Currently we don't have enough information from framework to
    /// decide cache or not. Also we think that caching data in a library
    /// is not safe at this moment.
    bool disable_cache_data_ {true};

public:
    impl::status_t compile_impl(const impl::op_t *op,
            const impl::engine_t *g_engine,
            const std::vector<impl::logical_tensor_t> &inputs,
            const std::vector<impl::logical_tensor_t> &outputs) override {
        std::string data_format = op->get_attr<std::string>("data_format");
        // "NXC" format will be converted to "NCX" format
        impl::logical_tensor_t src_lt = inputs.at(batch_normalization::kSrc);

        // "NXC"
        if (data_format == "NXC") {
            src_lt = impl::logical_tensor_wrapper(
                    &inputs.at(batch_normalization::kSrc))
                             .reorder_data_dims_strides();
        }

        using desc = tensor::desc;
        // prepare the inputs and outputs tensors' descs
        desc src {src_lt};
        const bool use_stats = inputs.size() > batch_normalization::kMean;

        epsilon_ = op->get_attr<float>("epsilon");

        auto flags = normalization_flag::use_scale_shift;
        if (use_stats) flags |= normalization_flag::use_global_stats;
        if (op->get_kind() == op_kind::bn_relu)
            flags |= normalization_flag::fuse_norm_relu;

        // workaround: use src once issue intel/mkl-dnn#588 is
        // resolved
        src = src.is_4c_blocked() ? src.to_default_format() : src;

        p_engine_ = make_dnnl_engine(*g_engine);
        pd_ = primitive_desc(
                {prop_kind::forward_inference, src, epsilon_, flags},
                p_engine_);
        prim_ = super(pd_);
        const tensor::desc optimal_dst_desc {pd_.dst_desc()};
        impl::logical_tensor_t *ori_dst_lt
                = const_cast<impl::logical_tensor_t *>(
                        &outputs.at(batch_normalization::kDst));
        fill_layout_info(ori_dst_lt, optimal_dst_desc);
        return impl::status::success;
    }

    impl::status_t execute_impl(const impl::op_t *op,
            const impl::stream_t *g_stream,
            const std::vector<impl::tensor_t> &inputs,
            const std::vector<impl::tensor_t> &outputs) override {
        p_stream_ = make_dnnl_stream(p_engine_, *g_stream);
        std::string data_format = op->get_attr<std::string>("data_format");
        auto &src_lt = const_cast<impl::logical_tensor_t &>(
                inputs.at(batch_normalization::kSrc).get_logical_tensor());
        auto &dst_lt = const_cast<impl::logical_tensor_t &>(
                outputs.at(batch_normalization::kDst).get_logical_tensor());
        // "NXC"
        if (data_format == "NXC") {
            src_lt = impl::logical_tensor_wrapper(src_lt)
                             .reorder_data_dims_strides();
            dst_lt = impl::logical_tensor_wrapper(dst_lt)
                             .reorder_data_dims_strides();
        }

        impl::allocator_t *alc = g_stream->get_engine()->get_allocator();

        tensor x {src_lt, p_engine_, alc,
                inputs.at(batch_normalization::kSrc).get_data_handle()};
        tensor y {dst_lt, p_engine_, alc,
                outputs.at(batch_normalization::kDst).get_data_handle()};
        tensor w {inputs.at(batch_normalization::kScale), p_engine_, alc};
        tensor b {inputs.at(batch_normalization::kShift), p_engine_, alc};

        auto channels = x.get_dims()[1];
        if (channels != w.get_dims()[0]) {
            throw std::runtime_error("channel mismatch");
        }
        if (inputs.size() > batch_normalization::kMean) {
            tensor m {inputs.at(batch_normalization::kMean), p_engine_, alc};
            tensor v {
                    inputs.at(batch_normalization::kVariance), p_engine_, alc};
            compute(x, m, v, w, b, y, epsilon_, p_engine_, alc, p_stream_);
        } else {
            compute(x, w, b, y, epsilon_, p_engine_, alc, p_stream_);
        }

        return impl::status::success;
    }

private:
    void compute(const tensor &src, const tensor &scale, const tensor &shift,
            tensor &dst, float epsilon, const dnnl::engine &p_engine,
            impl::allocator_t *alc, const dnnl::stream &p_stream) {
        static tensor dummy;
        compute_impl</*use_stats=*/false>(src, dummy, dummy, scale, shift, dst,
                epsilon, p_engine, alc, p_stream);
    }

    void compute(const tensor &src, const tensor &mean, const tensor &variance,
            const tensor &scale, const tensor &shift, tensor &dst,
            float epsilon, const dnnl::engine &p_engine, impl::allocator_t *alc,
            const dnnl::stream &p_stream) {
        compute_impl</*use_stats=*/true>(src, mean, variance, scale, shift, dst,
                epsilon, p_engine, alc, p_stream);
    }

    template <bool use_stats>
    void compute_impl(const tensor &src, const tensor &mean,
            const tensor &variance, const tensor &scale, const tensor &shift,
            tensor &dst, float epsilon, const dnnl::engine &p_engine,
            impl::allocator_t *alc, const dnnl::stream &p_stream) {
        UNUSED(epsilon);
        // copy scale and shift to scale_shift tensor and cache it
        if (disable_cache_data_ || scale_shift_.is_empty()) {
            if (scale_shift_.is_empty())
                scale_shift_ = tensor {pd_.weights_desc(), p_engine, alc};
            auto *scale_shift_buf
                    = static_cast<char *>(scale_shift_.get_data_handle());
#if DNNL_GRAPH_WITH_SYCL
            cl::sycl::queue q = dnnl::sycl_interop::get_queue(p_stream);
            q.memcpy(
                    scale_shift_buf, scale.get_data_handle(), scale.get_size());
            q.memcpy(scale_shift_buf + scale.get_size(),
                    shift.get_data_handle(), shift.get_size());
#else
            std::memcpy(
                    scale_shift_buf, scale.get_data_handle(), scale.get_size());
            std::memcpy(scale_shift_buf + scale.get_size(),
                    shift.get_data_handle(), shift.get_size());
#endif
        }

        auto expected_src = src.reorder_if_differ_in(p_stream, pd_.src_desc());

        tensor expected_dst = dst;
        if (pd_.dst_desc() != dst.get_desc()) {
            expected_dst = tensor {pd_.dst_desc(), p_engine, alc};
        }

        if (use_stats) {
            // cache reordered mean and var
            if (disable_cache_data_ || expected_mean_.is_empty()
                    || expected_var_.is_empty()) {
                expected_mean_
                        = mean.reorder_if_differ_in(p_stream, pd_.mean_desc());
                expected_var_ = variance.reorder_if_differ_in(
                        p_stream, pd_.variance_desc());
            }
            prim_.execute(p_stream,
                    {{DNNL_ARG_SRC, expected_src},
                            {DNNL_ARG_SCALE_SHIFT, scale_shift_},
                            {DNNL_ARG_VARIANCE, expected_var_},
                            {DNNL_ARG_MEAN, expected_mean_},
                            {DNNL_ARG_DST, expected_dst}});
        } else {
            prim_.execute(p_stream,
                    {{DNNL_ARG_SRC, expected_src},
                            {DNNL_ARG_SCALE_SHIFT, scale_shift_},
                            {DNNL_ARG_DST, expected_dst}});
        }

        // if output layout has been set and different from optimal layout
        // we have to do reorder
        if (expected_dst.get_desc() != dst.get_desc()) {
            expected_dst.reorder_to(p_stream, dst);
        }
    }
};

struct batch_normalization_forward_training
    : public dnnl::batch_normalization_forward,
      public kernel_base {

    using super = dnnl::batch_normalization_forward;

private:
    float epsilon_;
    float mom_;
    primitive_desc pd_;

    tensor scale_shift_;
    tensor original_dst_;

    dnnl::engine p_engine_;
    dnnl::stream p_stream_;

public:
    impl::status_t compile_impl(const impl::op_t *op,
            const impl::engine_t *g_engine,
            const std::vector<impl::logical_tensor_t> &inputs,
            const std::vector<impl::logical_tensor_t> &outputs) override {
        using desc = tensor::desc;
        epsilon_ = op->get_attr<float>("epsilon");
        mom_ = op->get_attr<float>("momentum");
        std::string data_format = op->get_attr<std::string>("data_format");

        impl::logical_tensor_t src_lt = inputs.at(batch_normalization::kSrc);
        // "NXC"
        if (data_format == "NXC") {
            src_lt = impl::logical_tensor_wrapper(
                    &inputs.at(batch_normalization::kSrc))
                             .reorder_data_dims_strides();
        }

        // prepare the inputs and outputs tensors' descs
        desc src {src_lt};
        const bool use_stats = inputs.size() > batch_normalization::kMean;
        impl::logical_tensor_t dst_lt = outputs.at(batch_normalization::kDst);
        impl::logical_tensor_t rm_lt
                = outputs.at(batch_normalization::kRunning_mean);
        impl::logical_tensor_t rv_lt
                = outputs.at(batch_normalization::kRunning_variance);

        auto flags = normalization_flag::use_scale_shift;
        if (use_stats) flags |= normalization_flag::use_global_stats;

        // workaround: use src once issue intel/mkl-dnn#588 is
        // resolved
        src = src.is_4c_blocked() ? src.to_default_format() : src;

        p_engine_ = make_dnnl_engine(*g_engine);
        pd_ = primitive_desc(
                {prop_kind::forward_training, src, epsilon_, flags}, p_engine_);
        const tensor::desc optimal_dst_desc {pd_.dst_desc()};
        const tensor::desc optimal_rm_desc {pd_.mean_desc()};
        const tensor::desc optimal_rv_desc {pd_.variance_desc()};
        fill_layout_info(&dst_lt, optimal_dst_desc);
        fill_layout_info(&rm_lt, optimal_rm_desc);
        fill_layout_info(&rv_lt, optimal_rv_desc);
        return impl::status::success;
    }

    impl::status_t execute_impl(const impl::op_t *op,
            const impl::stream_t *g_stream,
            const std::vector<impl::tensor_t> &inputs,
            const std::vector<impl::tensor_t> &outputs) override {
        p_stream_ = make_dnnl_stream(p_engine_, *g_stream);
        impl::allocator_t *alc = g_stream->get_engine()->get_allocator();

        std::string data_format = op->get_attr<std::string>("data_format");
        auto &src_lt = const_cast<impl::logical_tensor_t &>(
                inputs.at(batch_normalization::kSrc).get_logical_tensor());
        auto &dst_lt = const_cast<impl::logical_tensor_t &>(
                outputs.at(batch_normalization::kDst).get_logical_tensor());
        // "NXC"
        if (data_format == "NXC") {
            src_lt = impl::logical_tensor_wrapper(src_lt)
                             .reorder_data_dims_strides();
            dst_lt = impl::logical_tensor_wrapper(dst_lt)
                             .reorder_data_dims_strides();
        }
        tensor x {src_lt, p_engine_, alc,
                inputs.at(batch_normalization::kSrc).get_data_handle()};
        tensor w {inputs.at(batch_normalization::kScale), p_engine_, alc};
        tensor b {inputs.at(batch_normalization::kShift), p_engine_, alc};
        tensor y {dst_lt, p_engine_, alc,
                outputs.at(batch_normalization::kDst).get_data_handle()};
        tensor m {inputs.at(batch_normalization::kMean), p_engine_, alc};
        tensor v {inputs.at(batch_normalization::kVariance), p_engine_, alc};
        tensor rm {
                outputs.at(batch_normalization::kRunning_mean), p_engine_, alc};
        tensor rv {outputs.at(batch_normalization::kRunning_variance),
                p_engine_, alc};
        tensor bm {
                outputs.at(batch_normalization::kBatch_mean), p_engine_, alc};
        tensor bv {outputs.at(batch_normalization::kBatch_variance), p_engine_,
                alc};
        compute(x, w, b, y, m, v, rm, rv, bm, bv, mom_, epsilon_, alc,
                p_stream_);
        return impl::status::success;
    }

private:
    void compute_impl(tensor &src, const tensor &scale, const tensor &shift,
            tensor &dst, tensor &mean, tensor &variance, float momentum,
            float epsilon, impl::allocator_t *alc,
            const dnnl::stream &p_stream) {
        UNUSED(momentum);
        UNUSED(epsilon);

        original_dst_ = dst;
        scale_shift_ = tensor {pd_.weights_desc(), p_engine_, alc};
        auto *scale_shift_buf
                = static_cast<char *>(scale_shift_.get_data_handle());
#if DNNL_GRAPH_WITH_SYCL
        cl::sycl::queue q = dnnl::sycl_interop::get_queue(p_stream);
        q.memcpy(scale_shift_buf, scale.get_data_handle(), scale.get_size());
        q.memcpy(scale_shift_buf + scale.get_size(), shift.get_data_handle(),
                shift.get_size());
#else
        std::memcpy(scale_shift_buf, scale.get_data_handle(), scale.get_size());
        std::memcpy(scale_shift_buf + scale.get_size(), shift.get_data_handle(),
                shift.get_size());
#endif

        mean.reinit_if_possible(p_stream, pd_.mean_desc());
        variance.reinit_if_possible(p_stream, pd_.variance_desc());
        dst.reinit_if_possible(p_stream, pd_.dst_desc());
        super(pd_).execute(p_stream,
                {{DNNL_ARG_SRC, src}, {DNNL_ARG_SCALE_SHIFT, scale_shift_},
                        {DNNL_ARG_MEAN, mean}, {DNNL_ARG_VARIANCE, variance},
                        {DNNL_ARG_DST, dst}});

        if (original_dst_.get_desc() != dst.get_desc()) {
            dst.reorder_to(p_stream, original_dst_);
        }
    }

    void compute(tensor &src, const tensor &scale, const tensor &shift,
            tensor &dst, tensor &mean, tensor &variance, tensor &running_mean,
            tensor &running_var, tensor &batch_mean, tensor &batch_var,
            float momentum, float epsilon, impl::allocator_t *alc,
            const dnnl::stream &p_stream) {
        compute_impl(src, scale, shift, dst, mean, variance, momentum, epsilon,
                alc, p_stream);
        // running_mean, running_mean's buffer can be empty
        sum::compute({momentum, 1 - momentum}, {running_mean, mean},
                running_mean, p_engine_, alc, p_stream);
        sum::compute({momentum, 1 - momentum}, {running_var, variance},
                running_var, p_engine_, alc, p_stream);
        // copy data
        batch_mean.reinit_if_possible(p_stream, mean.get_desc());
        batch_var.reinit_if_possible(p_stream, variance.get_desc());
#if DNNL_GRAPH_WITH_SYCL
        cl::sycl::queue q = dnnl::sycl_interop::get_queue(p_stream);
        q.memcpy(batch_mean.get_data_handle(), mean.get_data_handle(),
                batch_mean.get_size());
        q.memcpy(batch_var.get_data_handle(), variance.get_data_handle(),
                batch_var.get_size());
#else
        std::memcpy(batch_mean.get_data_handle(), mean.get_data_handle(),
                batch_mean.get_size());
        std::memcpy(batch_var.get_data_handle(), variance.get_data_handle(),
                batch_var.get_size());
#endif
    }
};

struct batch_normalization_backward : public dnnl::batch_normalization_backward,
                                      public kernel_base {
    using super = dnnl::batch_normalization_backward;

private:
    primitive_desc pd_;
    float epsilon_;

    tensor diff_scale_shift_;
    tensor original_diff_src_;

    dnnl::engine p_engine_;
    dnnl::stream p_stream_;

public:
    impl::status_t compile_impl(const impl::op_t *op,
            const impl::engine_t *g_engine,
            const std::vector<impl::logical_tensor_t> &inputs,
            const std::vector<impl::logical_tensor_t> &outputs) {
        using desc = tensor::desc;
        epsilon_ = op->get_attr<float>("epsilon");
        std::string data_format = op->get_attr<std::string>("data_format");

        impl::logical_tensor_t src_lt = inputs.at(batch_normalization::kSrc);
        impl::logical_tensor_t diff_src_lt
                = outputs.at(batch_normalization_bwd::kDiff_src);
        // "NXC"
        if (data_format == "NXC") {
            src_lt = impl::logical_tensor_wrapper(&src_lt)
                             .reorder_data_dims_strides();
        }
        // prepare the inputs and outputs tensors' descs
        desc src {src_lt};

        const bool use_stats = inputs.size() > batch_normalization::kMean;

        auto flags = normalization_flag::use_scale_shift;
        if (use_stats) flags |= normalization_flag::use_global_stats;

        // workaround: use src once issue intel/mkl-dnn#588 is
        // resolved
        src = src.is_4c_blocked() ? src.to_default_format() : src;

        p_engine_ = make_dnnl_engine(*g_engine);
        impl::allocator_t *alc = g_engine->get_allocator();

        auto forward_hints = dnnl::batch_normalization_forward::primitive_desc(
                {prop_kind::forward_training, src, epsilon_, flags}, p_engine_);
        // cache diff_dst in order to compute
        pd_ = primitive_desc({prop_kind::backward, forward_hints.dst_desc(),
                                     src, epsilon_, flags},
                p_engine_, forward_hints);

        diff_scale_shift_ = tensor(pd_.diff_weights_desc(), p_engine_, alc);
        const tensor::desc optimal_diff_src_desc {pd_.diff_src_desc()};
        fill_layout_info(&diff_src_lt, optimal_diff_src_desc);
        return impl::status::success;
    }

    impl::status_t execute_impl(const impl::op_t *op,
            const impl::stream_t *g_stream,
            const std::vector<impl::tensor_t> &inputs,
            const std::vector<impl::tensor_t> &outputs) {
        p_stream_ = make_dnnl_stream(p_engine_, *g_stream);
        impl::allocator_t *alc = g_stream->get_engine()->get_allocator();

        std::string data_format = op->get_attr<std::string>("data_format");
        auto &src_lt = const_cast<impl::logical_tensor_t &>(
                inputs.at(batch_normalization_bwd::kSrc).get_logical_tensor());
        auto &diff_dst_lt = const_cast<impl::logical_tensor_t &>(
                inputs.at(batch_normalization_bwd::kDiff_dst)
                        .get_logical_tensor());
        auto &diff_src_lt = const_cast<impl::logical_tensor_t &>(
                outputs.at(batch_normalization_bwd::kDiff_src)
                        .get_logical_tensor());
        // "NXC"
        if (data_format == "NXC") {
            src_lt = impl::logical_tensor_wrapper(src_lt)
                             .reorder_data_dims_strides();
            diff_dst_lt = impl::logical_tensor_wrapper(diff_dst_lt)
                                  .reorder_data_dims_strides();
            diff_src_lt = impl::logical_tensor_wrapper(diff_src_lt)
                                  .reorder_data_dims_strides();
        }
        tensor x {src_lt, p_engine_, alc,
                inputs.at(batch_normalization_bwd::kSrc).get_data_handle()};
        tensor w {inputs.at(batch_normalization_bwd::kScale), p_engine_, alc};
        tensor m {inputs.at(batch_normalization_bwd::kMean), p_engine_, alc};
        tensor v {
                inputs.at(batch_normalization_bwd::kVariance), p_engine_, alc};
        tensor diff_dst {diff_dst_lt, p_engine_, alc,
                inputs.at(batch_normalization_bwd::kDiff_dst)
                        .get_data_handle()};
        tensor diff_scale {outputs.at(batch_normalization_bwd::kDiff_scale),
                p_engine_, alc};
        tensor diff_shift {outputs.at(batch_normalization_bwd::kDiff_shift),
                p_engine_, alc};
        tensor diff_src {diff_src_lt, p_engine_, alc,
                outputs.at(batch_normalization_bwd::kDiff_src)
                        .get_data_handle()};
        compute(x, m, v, diff_dst, w, diff_src, diff_scale, diff_shift,
                p_engine_, alc, p_stream_);
        return impl::status::success;
    }

private:
    void compute_impl(tensor &src, tensor &mean, tensor &variance,
            tensor &diff_dst, const tensor &scale, tensor &diff_src,
            const dnnl::engine &p_engine, impl::allocator_t *alc,
            const dnnl::stream &p_stream) {
        UNUSED(alc);
        UNUSED(p_engine);
        // TODO(xxx): support no-affine model
        original_diff_src_ = diff_src;

        diff_dst.reinit_if_possible(p_stream, pd_.diff_dst_desc());
        src.reinit_if_possible(p_stream, pd_.src_desc());
        diff_src.reinit_if_possible(p_stream, pd_.diff_src_desc());
        diff_scale_shift_.reinit_if_possible(p_stream, pd_.diff_weights_desc());

        mean.reinit_if_possible(p_stream, pd_.mean_desc());
        variance.reinit_if_possible(p_stream, pd_.variance_desc());
        super(pd_).execute(p_stream,
                {{DNNL_ARG_SRC, src}, {DNNL_ARG_DIFF_DST, diff_dst},
                        {DNNL_ARG_SCALE_SHIFT, scale}, // only need scale
                        {DNNL_ARG_MEAN, mean}, {DNNL_ARG_VARIANCE, variance},
                        {DNNL_ARG_DIFF_SRC, diff_src},
                        {DNNL_ARG_DIFF_SCALE_SHIFT, diff_scale_shift_}});

        if (diff_src.get_desc() != original_diff_src_.get_desc()) {
            diff_src.reorder_to(p_stream, original_diff_src_);
        }
    }

    void compute(tensor &src, tensor &mean, tensor &variance, tensor &diff_dst,
            const tensor &scale, tensor &diff_src, tensor &diff_scale,
            tensor &diff_shift, const dnnl::engine &p_engine,
            impl::allocator_t *alc, const dnnl::stream &p_stream) {
        compute_impl(src, mean, variance, diff_dst, scale, diff_src, p_engine,
                alc, p_stream);
        diff_scale.reinit_if_possible(p_stream, scale.get_desc());
        diff_shift.reinit_if_possible(p_stream, scale.get_desc());
        auto *diff_scale_shift_buf
                = static_cast<char *>(diff_scale_shift_.get_data_handle());
#if DNNL_GRAPH_WITH_SYCL
        cl::sycl::queue q = dnnl::sycl_interop::get_queue(p_stream);
        q.memcpy(diff_scale.get_data_handle(), diff_scale_shift_buf,
                diff_scale.get_size());
        q.memcpy(diff_shift.get_data_handle(),
                diff_scale_shift_buf + diff_scale.get_size(),
                diff_shift.get_size());
#else
        std::memcpy(diff_scale.get_data_handle(), diff_scale_shift_buf,
                diff_scale.get_size());
        std::memcpy(diff_shift.get_data_handle(),
                diff_scale_shift_buf + diff_scale.get_size(),
                diff_shift.get_size());
#endif
    }
};

} // namespace dnnl_impl
} // namespace impl
} // namespace graph
} // namespace dnnl

#endif
