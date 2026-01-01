// SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only
#pragma once

#include <concepts>
#include <iterator>
#include <ranges>

#include "loom/core/endpoint_types.hpp"
#include "loom/core/fabric.hpp"
#include "loom/core/types.hpp"

/**
 * @namespace loom
 * @brief Core namespace for the loom libfabric bindings library.
 */
namespace loom {

/**
 * @class provider_iterator
 * @brief Forward iterator for traversing fabric provider information.
 */
class provider_iterator {
public:
    using iterator_category = std::forward_iterator_tag;  ///< Iterator category
    using value_type = fabric_info;                       ///< Value type
    using difference_type = std::ptrdiff_t;               ///< Difference type
    using pointer = const fabric_info*;                   ///< Pointer type
    using reference = const fabric_info&;                 ///< Reference type

    /**
     * @brief Default constructor creating a sentinel iterator.
     */
    constexpr provider_iterator() noexcept = default;

    /**
     * @brief Constructs an iterator pointing to a fabric_info.
     * @param info Pointer to the fabric_info to iterate from.
     */
    explicit provider_iterator(const fabric_info* info) noexcept : current_(info) {}

    /**
     * @brief Dereferences the iterator.
     * @return Reference to the current fabric_info.
     */
    auto operator*() const -> reference { return *current_; }

    /**
     * @brief Member access operator.
     * @return Pointer to the current fabric_info.
     */
    auto operator->() const -> pointer { return current_; }

    /**
     * @brief Pre-increment operator.
     * @return Reference to this iterator after advancing.
     */
    auto operator++() -> provider_iterator& {
        if (current_) {
            auto it = current_->begin();
            ++it;
            current_ = (it != current_->end()) ? &(*it) : nullptr;
        }
        return *this;
    }

    /**
     * @brief Post-increment operator.
     * @return Copy of the iterator before advancing.
     */
    auto operator++(int) -> provider_iterator {
        auto tmp = *this;
        ++(*this);
        return tmp;
    }

    /**
     * @brief Equality comparison operator.
     * @param other The iterator to compare with.
     * @return True if iterators are equal.
     */
    auto operator==(const provider_iterator& other) const noexcept -> bool {
        return current_ == other.current_;
    }

private:
    const fabric_info* current_{nullptr};  ///< Current fabric_info pointer
};

/**
 * @class provider_range
 * @brief Range of fabric provider information for range-based iteration.
 *
 * Provides a std::ranges-compatible view over available fabric providers.
 */
class provider_range {
public:
    using iterator = provider_iterator;        ///< Iterator type
    using const_iterator = provider_iterator;  ///< Const iterator type

    /**
     * @brief Constructs from a fabric_info chain.
     * @param info The fabric_info to iterate over.
     */
    explicit provider_range(fabric_info&& info) noexcept : info_(std::move(info)) {}

    /**
     * @brief Gets an iterator to the beginning.
     * @return Iterator to the first provider.
     */
    [[nodiscard]] auto begin() const -> iterator {
        if (!info_) {
            return iterator{nullptr};
        }
        return iterator{&info_};
    }

    /**
     * @brief Gets the end sentinel iterator.
     * @return Sentinel iterator.
     */
    [[nodiscard]] auto end() const -> iterator { return iterator{nullptr}; }

    /**
     * @brief Gets a const iterator to the beginning.
     * @return Const iterator to the first provider.
     */
    [[nodiscard]] auto cbegin() const -> const_iterator { return begin(); }

    /**
     * @brief Gets the const end sentinel iterator.
     * @return Const sentinel iterator.
     */
    [[nodiscard]] auto cend() const -> const_iterator { return end(); }

    /**
     * @brief Gets the number of providers in the range.
     * @return Number of providers.
     */
    [[nodiscard]] auto size() const -> std::size_t {
        return static_cast<std::size_t>(std::ranges::distance(begin(), end()));
    }

    /**
     * @brief Checks if the range is empty.
     * @return True if no providers are available.
     */
    [[nodiscard]] auto empty() const noexcept -> bool { return !info_; }

    /**
     * @brief Deleted copy constructor.
     */
    provider_range(const provider_range&) = delete;
    /**
     * @brief Deleted copy assignment operator.
     */
    auto operator=(const provider_range&) -> provider_range& = delete;
    /**
     * @brief Move constructor.
     */
    provider_range(provider_range&&) noexcept = default;
    /**
     * @brief Move assignment operator.
     */
    auto operator=(provider_range&&) noexcept -> provider_range& = default;

private:
    fabric_info info_;  ///< The underlying fabric_info chain
};

static_assert(std::ranges::forward_range<provider_range>);
static_assert(std::ranges::range<provider_range>);

}  // namespace loom
