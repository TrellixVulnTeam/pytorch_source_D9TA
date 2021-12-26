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

#ifndef BACKEND_DNNL_UTILS_HPP
#define BACKEND_DNNL_UTILS_HPP

#include <algorithm>
#include <cstring>
#include <stdlib.h>
#include <utility>
#include <vector>
namespace dnnl {
namespace graph {
namespace impl {
namespace dnnl_impl {
namespace utils {
template <typename F, typename T,
        typename U = decltype(std::declval<F>()(std::declval<T>()))>
std::vector<U> fmap(const std::vector<T> &vec, const F &f) {
    std::vector<U> result;
    std::transform(vec.begin(), vec.end(), std::back_inserter(result), f);
    return result;
}

/** sorts an array of values using @p comparator. While sorting the array
 * of value, the function permutes an array of @p keys accordingly.
 *
 * @note The arrays of @p keys can be omitted. In this case the function
 *       sorts the array of @vals only.
 */
template <typename T, typename U, typename F>
inline void simultaneous_sort(T *vals, U *keys, size_t size, F comparator) {
    if (size == 0) return;

    for (size_t i = 0; i < size - 1; ++i) {
        bool swapped = false;
        for (size_t j = 0; j < size - i - 1; j++) {
            if (comparator(vals[j], vals[j + 1]) > 0) {
                std::swap(vals[j], vals[j + 1]);
                if (keys) std::swap(keys[j], keys[j + 1]);
                swapped = true;
            }
        }

        if (swapped == false) break;
    }
}

/* Sorts an array of @p vals using @p comparator. Uses @p vals_2nd_level as a
 * second level comparing criteria in case comparator returns 0 (equal values)
 * for @p vals elements.
 * While sorting the array of @p vals, the function permutes an array of
 * @p vals_2nd_level and @p keys accordingly.
 */
template <typename T, typename U, typename F>
inline void simultaneous_sort(
        T *vals, T *vals_2nd_level, U *keys, size_t size, F comparator) {
    if (size == 0) return;

    for (size_t i = 0; i < size - 1; ++i) {
        bool swapped = false;

        for (size_t j = 0; j < size - i - 1; j++) {
            auto res = comparator(vals[j], vals[j + 1]);
            if (res == 0)
                res = comparator(vals_2nd_level[j], vals_2nd_level[j + 1]);

            if (res > 0) {
                std::swap(vals[j], vals[j + 1]);
                std::swap(vals_2nd_level[j], vals_2nd_level[j + 1]);
                std::swap(keys[j], keys[j + 1]);
                swapped = true;
            }
        }

        if (swapped == false) break;
    }
}

template <typename T>
inline T rnd_up(const T a, const T b) {
    return (a + b - 1) / b * b;
}

inline uintptr_t mod_ptr(void *ptr, size_t bytes) {
    return reinterpret_cast<uintptr_t>(ptr) & (bytes - 1);
}

inline bool is_aligned_ptr(void *ptr, size_t bytes) {
    return mod_ptr(ptr, bytes) == 0;
}

inline bool is_enable_constant_cache() {
    char *env = std::getenv("DNNL_GRAPH_CONSTANT_CACHE");
    return env != nullptr && std::strcmp(env, "1") == 0;
}

} // namespace utils
} // namespace dnnl_impl
} // namespace impl
} // namespace graph
} // namespace dnnl

#endif
