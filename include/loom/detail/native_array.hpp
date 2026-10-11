// SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only
#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <memory>
#include <span>
#include <type_traits>

namespace loom::detail {

// Native submission metadata with inline storage for common small counts.
// Larger inputs retain every element. The storage lasts through the native
// call; providers retaining metadata require a separate ownership contract.
template <typename T>
    requires(std::is_trivially_default_constructible_v<T> && std::is_trivially_copyable_v<T>)
class native_array {
    static constexpr std::size_t inline_capacity = 16;

    // Only the input-sized prefix is initialized and read. Keep the object
    // immovable to avoid copying unused, uninitialized inline elements.
    std::array<T, inline_capacity> inline_storage_;
    std::unique_ptr<T[]> overflow_;
    std::size_t size_;

public:
    template <typename U, typename Projection>
    explicit native_array(std::span<const U> input, Projection project)
        : overflow_(input.size() > inline_capacity
                        ? std::make_unique_for_overwrite<T[]>(input.size())
                        : nullptr),
          size_(input.size()) {
        std::ranges::transform(input, data(), project);
    }

    native_array(const native_array&) = delete;
    native_array(native_array&&) = delete;
    auto operator=(const native_array&) -> native_array& = delete;
    auto operator=(native_array&&) -> native_array& = delete;

    [[nodiscard]] auto data() noexcept -> T* {
        return size_ > inline_capacity ? overflow_.get() : inline_storage_.data();
    }
    [[nodiscard]] auto size() const noexcept -> std::size_t { return size_; }
    [[nodiscard]] auto empty() const noexcept -> bool { return size_ == 0; }
};

}  // namespace loom::detail
