// SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only
#pragma once

/**
 * @file registered_buffer.hpp
 * @brief Asio-compatible registered buffer types backed by RDMA memory regions.
 *
 * This file provides buffer registration that mirrors Asio's buffer_registration
 * interface but uses libfabric's fi_mr_regattr for RDMA memory registration
 * instead of io_uring's buffer registration.
 *
 * Usage:
 * @code
 * // Register buffers with a fabric domain via the execution context
 * std::array<std::byte, 4096> buf1, buf2;
 * std::array buffers = {asio::buffer(buf1), asio::buffer(buf2)};
 *
 * auto registration = loom::asio::register_buffers(ioc, domain, buffers, access);
 *
 * // Use registered buffers in operations
 * ep.async_send(registration[0], handler);
 * @endcode
 */

#include <cstddef>
#include <memory>
#include <span>
#include <vector>

#include "loom/core/domain.hpp"
#include "loom/core/memory.hpp"
#include "loom/core/types.hpp"
#include <asio/buffer.hpp>

namespace loom::asio {

class mutable_registered_buffer;
class const_registered_buffer;

/**
 * @brief Identifies a registered buffer within a registration.
 */
class registered_buffer_id {
public:
    using native_handle_type = std::size_t;

    constexpr registered_buffer_id() noexcept = default;
    constexpr explicit registered_buffer_id(native_handle_type id) noexcept : id_(id) {}

    [[nodiscard]] constexpr auto native_handle() const noexcept -> native_handle_type {
        return id_;
    }

    [[nodiscard]] constexpr auto is_valid() const noexcept -> bool { return id_ != invalid_id; }

    constexpr auto operator==(const registered_buffer_id&) const noexcept -> bool = default;

private:
    static constexpr native_handle_type invalid_id = static_cast<native_handle_type>(-1);
    native_handle_type id_{invalid_id};
};

/**
 * @brief A registered buffer over modifiable data.
 *
 * Wraps an RDMA-registered memory region and provides the MutableBufferSequence
 * interface. The buffer remains registered for the lifetime of the registration.
 */
class mutable_registered_buffer {
public:
    mutable_registered_buffer() noexcept = default;

    mutable_registered_buffer(::asio::mutable_buffer buffer,
                              memory_region* mr,
                              registered_buffer_id id) noexcept
        : buffer_(buffer), mr_(mr), id_(id) {}

    [[nodiscard]] auto data() const noexcept -> void* { return buffer_.data(); }

    [[nodiscard]] auto size() const noexcept -> std::size_t { return buffer_.size(); }

    [[nodiscard]] auto id() const noexcept -> registered_buffer_id { return id_; }

    [[nodiscard]] auto buffer() const noexcept -> const ::asio::mutable_buffer& { return buffer_; }

    [[nodiscard]] auto memory_region() const noexcept -> loom::memory_region* { return mr_; }

    [[nodiscard]] auto descriptor() const noexcept -> mr_descriptor {
        return mr_ ? mr_->descriptor() : mr_descriptor{};
    }

    [[nodiscard]] auto key() const noexcept -> std::uint64_t { return mr_ ? mr_->key() : 0; }

    auto operator+=(std::size_t n) noexcept -> mutable_registered_buffer& {
        buffer_ += n;
        return *this;
    }

private:
    friend mutable_registered_buffer buffer(const mutable_registered_buffer&, std::size_t) noexcept;

    ::asio::mutable_buffer buffer_;
    loom::memory_region* mr_{nullptr};
    registered_buffer_id id_;
};

/**
 * @brief A registered buffer over non-modifiable data.
 */
class const_registered_buffer {
public:
    const_registered_buffer() noexcept = default;

    const_registered_buffer(::asio::const_buffer buffer,
                            const memory_region* mr,
                            registered_buffer_id id) noexcept
        : buffer_(buffer), mr_(mr), id_(id) {}

    const_registered_buffer(const mutable_registered_buffer& other) noexcept
        : buffer_(other.buffer()), mr_(other.memory_region()), id_(other.id()) {}

    [[nodiscard]] auto data() const noexcept -> const void* { return buffer_.data(); }

    [[nodiscard]] auto size() const noexcept -> std::size_t { return buffer_.size(); }

    [[nodiscard]] auto id() const noexcept -> registered_buffer_id { return id_; }

    [[nodiscard]] auto buffer() const noexcept -> const ::asio::const_buffer& { return buffer_; }

    [[nodiscard]] auto memory_region() const noexcept -> const loom::memory_region* { return mr_; }

    [[nodiscard]] auto descriptor() const noexcept -> mr_descriptor {
        return mr_ ? mr_->descriptor() : mr_descriptor{};
    }

    [[nodiscard]] auto key() const noexcept -> std::uint64_t { return mr_ ? mr_->key() : 0; }

    auto operator+=(std::size_t n) noexcept -> const_registered_buffer& {
        buffer_ += n;
        return *this;
    }

private:
    friend const_registered_buffer buffer(const const_registered_buffer&, std::size_t) noexcept;

    ::asio::const_buffer buffer_;
    const loom::memory_region* mr_{nullptr};
    registered_buffer_id id_;
};

[[nodiscard]] inline auto operator+(const mutable_registered_buffer& b, std::size_t n) noexcept
    -> mutable_registered_buffer {
    auto result = b;
    result += n;
    return result;
}

[[nodiscard]] inline auto operator+(std::size_t n, const mutable_registered_buffer& b) noexcept
    -> mutable_registered_buffer {
    return b + n;
}

[[nodiscard]] inline auto operator+(const const_registered_buffer& b, std::size_t n) noexcept
    -> const_registered_buffer {
    auto result = b;
    result += n;
    return result;
}

[[nodiscard]] inline auto operator+(std::size_t n, const const_registered_buffer& b) noexcept
    -> const_registered_buffer {
    return b + n;
}

[[nodiscard]] inline auto buffer_sequence_begin(const mutable_registered_buffer& b) noexcept
    -> const ::asio::mutable_buffer* {
    return &b.buffer();
}

[[nodiscard]] inline auto buffer_sequence_end(const mutable_registered_buffer& b) noexcept
    -> const ::asio::mutable_buffer* {
    return &b.buffer() + 1;
}

[[nodiscard]] inline auto buffer_sequence_begin(const const_registered_buffer& b) noexcept
    -> const ::asio::const_buffer* {
    return &b.buffer();
}

[[nodiscard]] inline auto buffer_sequence_end(const const_registered_buffer& b) noexcept
    -> const ::asio::const_buffer* {
    return &b.buffer() + 1;
}

/**
 * @brief Obtain a buffer representing the entire registered buffer.
 * @param b The registered buffer.
 * @return A copy of the registered buffer.
 */
[[nodiscard]] inline auto buffer(const mutable_registered_buffer& b) noexcept
    -> mutable_registered_buffer {
    return b;
}

/**
 * @brief Obtain a buffer representing the entire registered buffer.
 * @param b The registered buffer.
 * @return A copy of the registered buffer.
 */
[[nodiscard]] inline auto buffer(const const_registered_buffer& b) noexcept
    -> const_registered_buffer {
    return b;
}

/**
 * @brief Obtain a buffer representing part of a registered buffer.
 * @param b The registered buffer.
 * @param n The maximum size of the returned buffer.
 * @return A registered buffer with size limited to n bytes.
 */
[[nodiscard]] inline auto buffer(const mutable_registered_buffer& b, std::size_t n) noexcept
    -> mutable_registered_buffer {
    return mutable_registered_buffer(::asio::buffer(b.buffer(), n), b.memory_region(), b.id());
}

/**
 * @brief Obtain a buffer representing part of a registered buffer.
 * @param b The registered buffer.
 * @param n The maximum size of the returned buffer.
 * @return A registered buffer with size limited to n bytes.
 */
[[nodiscard]] inline auto buffer(const const_registered_buffer& b, std::size_t n) noexcept
    -> const_registered_buffer {
    return const_registered_buffer(::asio::buffer(b.buffer(), n), b.memory_region(), b.id());
}

/**
 * @brief Manages RDMA registration for a sequence of buffers.
 *
 * This class registers buffers with a libfabric domain using fi_mr_regattr,
 * making them suitable for zero-copy RDMA operations. The registration
 * persists until this object is destroyed.
 *
 * @tparam MutableBufferSequence The buffer sequence type.
 * @tparam Allocator The allocator for internal storage.
 */
template <typename MutableBufferSequence, typename Allocator = std::allocator<void>>
class buffer_registration {
public:
    using allocator_type = Allocator;
    using iterator = typename std::vector<mutable_registered_buffer>::const_iterator;
    using const_iterator = iterator;

    buffer_registration(const buffer_registration&) = delete;
    auto operator=(const buffer_registration&) -> buffer_registration& = delete;

    buffer_registration(buffer_registration&& other) noexcept
        : domain_(other.domain_), regions_(std::move(other.regions_)),
          buffers_(std::move(other.buffers_)), alloc_(std::move(other.alloc_)) {
        other.domain_ = nullptr;
    }

    auto operator=(buffer_registration&& other) noexcept -> buffer_registration& {
        if (this != &other) {
            domain_ = other.domain_;
            regions_ = std::move(other.regions_);
            buffers_ = std::move(other.buffers_);
            alloc_ = std::move(other.alloc_);
            other.domain_ = nullptr;
        }
        return *this;
    }

    ~buffer_registration() = default;

    [[nodiscard]] auto size() const noexcept -> std::size_t { return buffers_.size(); }

    [[nodiscard]] auto begin() const noexcept -> const_iterator { return buffers_.begin(); }

    [[nodiscard]] auto cbegin() const noexcept -> const_iterator { return buffers_.cbegin(); }

    [[nodiscard]] auto end() const noexcept -> const_iterator { return buffers_.end(); }

    [[nodiscard]] auto cend() const noexcept -> const_iterator { return buffers_.cend(); }

    [[nodiscard]] auto operator[](std::size_t i) const noexcept
        -> const mutable_registered_buffer& {
        return buffers_[i];
    }

    [[nodiscard]] auto at(std::size_t i) const -> const mutable_registered_buffer& {
        return buffers_.at(i);
    }

    [[nodiscard]] auto get_allocator() const noexcept -> allocator_type { return alloc_; }

private:
    template <typename Executor, typename MBS, typename Alloc>
    friend auto
    register_buffers(const Executor&, loom::domain&, const MBS&, mr_access, const Alloc&)
        -> result<buffer_registration<MBS, Alloc>>;

    template <typename Executor, typename MBS>
    friend auto register_buffers(const Executor&, loom::domain&, const MBS&, mr_access)
        -> result<buffer_registration<MBS>>;

    buffer_registration(loom::domain* domain,
                        std::vector<memory_region> regions,
                        std::vector<mutable_registered_buffer> buffers,
                        const Allocator& alloc)
        : domain_(domain), regions_(std::move(regions)), buffers_(std::move(buffers)),
          alloc_(alloc) {}

    loom::domain* domain_{nullptr};
    std::vector<memory_region> regions_;
    std::vector<mutable_registered_buffer> buffers_;
    [[no_unique_address]] Allocator alloc_;
};

/**
 * @brief Registers buffers with a fabric domain for RDMA operations.
 *
 * @tparam Executor The executor type.
 * @tparam MutableBufferSequence The buffer sequence type.
 * @tparam Allocator The allocator type.
 * @param ex The executor (used for consistency with Asio interface).
 * @param domain The fabric domain to register with.
 * @param buffer_sequence The buffers to register.
 * @param access The access flags for the memory regions.
 * @param alloc The allocator for internal storage.
 * @return A result containing the buffer_registration or an error.
 */
template <typename Executor, typename MutableBufferSequence, typename Allocator>
[[nodiscard]] auto register_buffers(const Executor& /*ex*/,
                                    loom::domain& domain,
                                    const MutableBufferSequence& buffer_sequence,
                                    mr_access access,
                                    const Allocator& alloc)
    -> result<buffer_registration<MutableBufferSequence, Allocator>> {
    std::vector<memory_region> regions;
    std::vector<mutable_registered_buffer> buffers;

    std::size_t index = 0;
    for (auto it = ::asio::buffer_sequence_begin(buffer_sequence);
         it != ::asio::buffer_sequence_end(buffer_sequence);
         ++it, ++index) {
        auto buf = ::asio::mutable_buffer(*it);

        auto mr_result = memory_region::register_memory(
            domain, static_cast<std::byte*>(buf.data()), buf.size(), access);

        if (!mr_result) {
            return make_error_result<buffer_registration<MutableBufferSequence, Allocator>>(
                mr_result.error());
        }

        regions.push_back(std::move(*mr_result));
    }

    index = 0;
    for (auto it = ::asio::buffer_sequence_begin(buffer_sequence);
         it != ::asio::buffer_sequence_end(buffer_sequence);
         ++it, ++index) {
        auto buf = ::asio::mutable_buffer(*it);
        buffers.emplace_back(buf, &regions[index], registered_buffer_id{index});
    }

    return buffer_registration<MutableBufferSequence, Allocator>(
        &domain, std::move(regions), std::move(buffers), alloc);
}

/**
 * @brief Registers buffers with a fabric domain for RDMA operations.
 *
 * @tparam Executor The executor type.
 * @tparam MutableBufferSequence The buffer sequence type.
 * @param ex The executor (used for consistency with Asio interface).
 * @param domain The fabric domain to register with.
 * @param buffer_sequence The buffers to register.
 * @param access The access flags for the memory regions.
 * @return A result containing the buffer_registration or an error.
 */
template <typename Executor, typename MutableBufferSequence>
[[nodiscard]] auto register_buffers(const Executor& ex,
                                    loom::domain& domain,
                                    const MutableBufferSequence& buffer_sequence,
                                    mr_access access)
    -> result<buffer_registration<MutableBufferSequence>> {
    return register_buffers(ex, domain, buffer_sequence, access, std::allocator<void>{});
}

/**
 * @brief Registers buffers using an execution context.
 *
 * @tparam ExecutionContext The execution context type.
 * @tparam MutableBufferSequence The buffer sequence type.
 * @param ctx The execution context.
 * @param domain The fabric domain to register with.
 * @param buffer_sequence The buffers to register.
 * @param access The access flags for the memory regions.
 * @return A result containing the buffer_registration or an error.
 */
template <typename ExecutionContext, typename MutableBufferSequence>
[[nodiscard]] auto register_buffers(ExecutionContext& ctx,
                                    loom::domain& domain,
                                    const MutableBufferSequence& buffer_sequence,
                                    mr_access access)
    -> result<buffer_registration<MutableBufferSequence>> {
    return register_buffers(ctx.get_executor(), domain, buffer_sequence, access);
}

/**
 * @brief Registers buffers using an execution context with a custom allocator.
 *
 * @tparam ExecutionContext The execution context type.
 * @tparam MutableBufferSequence The buffer sequence type.
 * @tparam Allocator The allocator type.
 * @param ctx The execution context.
 * @param domain The fabric domain to register with.
 * @param buffer_sequence The buffers to register.
 * @param access The access flags for the memory regions.
 * @param alloc The allocator for internal storage.
 * @return A result containing the buffer_registration or an error.
 */
template <typename ExecutionContext, typename MutableBufferSequence, typename Allocator>
[[nodiscard]] auto register_buffers(ExecutionContext& ctx,
                                    loom::domain& domain,
                                    const MutableBufferSequence& buffer_sequence,
                                    mr_access access,
                                    const Allocator& alloc)
    -> result<buffer_registration<MutableBufferSequence, Allocator>> {
    return register_buffers(ctx.get_executor(), domain, buffer_sequence, access, alloc);
}

}  // namespace loom::asio
