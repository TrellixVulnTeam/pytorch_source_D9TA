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

#ifndef INTERFACE_VALUE_HPP
#define INTERFACE_VALUE_HPP

#include <algorithm>
#include <limits>
#include <memory>
#include <utility>
#include <vector>

#include "c_types_map.hpp"
#include "logical_tensor.hpp"

namespace dnnl {
namespace graph {
namespace impl {

class value_t {
public:
    // A value connected to an op
    value_t(op_t &producer, size_t offset, const logical_tensor_t &lt,
            bool internal = false)
        : val_(lt)
        , producer_(&producer)
        , offset_(offset)
        , internal_(internal) {}

    // A value not associated with an op
    value_t(const logical_tensor_t &lt, bool internal = false)
        : val_(lt), internal_(internal) {}

    logical_tensor_t get_logical_tensor() const { return val_; }

    void set_dims(const std::vector<dim_t> &dims) {
        val_.ndims = static_cast<int>(dims.size());
        for (size_t d = 0; d < dims.size(); ++d) {
            val_.dims[d] = dims[d];
        }
    }

    void set_layout_type(layout_type_t new_type) {
        val_.layout_type = new_type;
    }

    void set_data_type(data_type_t new_dtype) { val_.data_type = new_dtype; }

    op_t &get_producer() const { return *producer_; }
    void set_producer(op_t &producer) { producer_ = &producer; }
    void reset_producer() { producer_ = nullptr; }
    bool has_producer() const { return producer_ != nullptr; }

    size_t get_offset() const { return offset_; }
    void set_offset(size_t offset) { offset_ = offset; }

    bool is_internal() const { return internal_; }

    bool operator==(const value_t &rhs) const {
        bool equal = logical_tensor_wrapper(this->val_)
                == logical_tensor_wrapper(rhs.val_);

        return equal && (this->producer_ == rhs.producer_)
                && (this->offset_ == rhs.offset_)
                && (this->consumers_ == rhs.consumers_)
                && (this->internal_ == rhs.internal_);
    }

    bool operator!=(const value_t &rhs) const { return !operator==(rhs); }

    // member class: A user of a value
    class consumer_t {
    public:
        consumer_t(op_t &op, size_t offset) : op_(&op), offset_(offset) {}

        consumer_t(const consumer_t &c) = default;

        bool operator==(const consumer_t &c) const {
            return op_ == c.op_ && offset_ == c.offset_;
        };

        op_t &get_op() const { return *op_; }

        size_t get_offset() const { return offset_; }

    private:
        op_t *op_ {nullptr};
        size_t offset_;
    };

    const std::vector<consumer_t> get_consumers() const { return consumers_; }

    void add_consumer(op_t &op, size_t offset) {
        const consumer_t c {op, offset};
        if (std::find(consumers_.begin(), consumers_.end(), c)
                == consumers_.end()) {
            consumers_.push_back(c);
        }
    }

    bool find_consumer(const op_kind_t &kind, size_t &offset);

    void swap_consumer(const size_t offset1, const size_t offset2) {
        std::swap(consumers_[offset1], consumers_[offset2]);
    }

    void remove_consumer(op_t &op, size_t offset) {
        const consumer_t c {op, offset};
        auto pos = std::find(consumers_.begin(), consumers_.end(), c);
        if (pos != consumers_.end()) {
            // Not all ops have been added through build_graph
            consumers_.erase(pos);
        }
    }

private:
    logical_tensor_t val_;

    op_t *producer_ {nullptr};
    size_t offset_ {std::numeric_limits<size_t>::max()};
    std::vector<consumer_t> consumers_;
    bool internal_ {false};
};

} // namespace impl
} // namespace graph
} // namespace dnnl

#endif
