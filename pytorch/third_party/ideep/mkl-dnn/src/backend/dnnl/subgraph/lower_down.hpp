/*******************************************************************************
 * Copyright 2021 Intel Corporation
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
#ifndef BACKEND_DNNL_SUBGRAPH_LOWER_DOWN_HPP
#define BACKEND_DNNL_SUBGRAPH_LOWER_DOWN_HPP

#include <memory>
#include <vector>
#include <unordered_map>

#include "interface/c_types_map.hpp"

#include "utils.hpp"

namespace dnnl {
namespace graph {
namespace impl {
namespace dnnl_impl {

class primitive_attr_mgr {
public:
    primitive_attr_mgr() = default;

    // Disable assignment and copy
    primitive_attr_mgr(const primitive_attr_mgr &) = delete;
    primitive_attr_mgr(primitive_attr_mgr &&) = delete;
    primitive_attr_mgr &operator=(const primitive_attr_mgr &) = delete;
    primitive_attr_mgr &operator=(primitive_attr_mgr &&) = delete;

    int64_t init_attr() {
        auto ret = data_.insert({counter++, dnnl::primitive_attr()});
        return ret.first->first;
    }

    dnnl::primitive_attr &get_attr(int64_t key) { return data_[key]; }

private:
    std::unordered_map<int64_t, dnnl::primitive_attr> data_;
    int64_t counter {0};
};

void check_with_bias(std::vector<std::shared_ptr<op_t>> &subgraph);

void fuse_bias_add(std::vector<std::shared_ptr<op_t>> &subgraph);

void split_quant_dequant(std::vector<std::shared_ptr<op_t>> &subgraph);

void folding_mul_scales(std::vector<std::shared_ptr<op_t>> &subgraph);

void fuse_to_int8_conv(std::vector<std::shared_ptr<op_t>> &subgraph);

void fuse_to_int8_matmul(std::vector<std::shared_ptr<op_t>> &subgraph);

void fuse_output_scales(std::vector<std::shared_ptr<op_t>> &subgraph,
        primitive_attr_mgr &prm_attr_mgr);

status_t fuse_post_ops(std::vector<std::shared_ptr<op_t>> &subgraph,
        primitive_attr_mgr &prm_attr_mgr);

void fuse_zero_points(std::vector<std::shared_ptr<op_t>> &subgraph,
        primitive_attr_mgr &prm_attr_mgr);

} // namespace dnnl_impl
} // namespace impl
} // namespace graph
} // namespace dnnl

#endif
