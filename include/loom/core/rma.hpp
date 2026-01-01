// SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only
#pragma once

#include <rdma/fi_rma.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include <sys/uio.h>

#include "loom/core/concepts/strong_types.hpp"
#include "loom/core/endpoint.hpp"
#include "loom/core/error.hpp"
#include "loom/core/memory.hpp"
#include "loom/core/result.hpp"
#include "loom/core/types.hpp"

/**
 * @namespace loom::rma
 * @brief Remote Memory Access (RMA) operations.
 */
namespace loom::rma {

/**
 * @struct rma_iov
 * @brief I/O vector for RMA operations.
 */
struct rma_iov {
    std::uint64_t addr;  ///< Remote address
    std::size_t len;     ///< Length in bytes
    std::uint64_t key;   ///< Remote memory key

    /**
     * @brief Default constructor.
     */
    constexpr rma_iov() noexcept = default;

    /**
     * @brief Constructs from address, length, and key.
     * @param a Remote address.
     * @param l Length in bytes.
     * @param k Remote memory key.
     */
    constexpr rma_iov(std::uint64_t a, std::size_t l, std::uint64_t k) noexcept
        : addr(a), len(l), key(k) {}

    /**
     * @brief Constructs from a remote_memory descriptor.
     * @param rm The remote memory descriptor.
     */
    constexpr explicit rma_iov(const remote_memory& rm) noexcept
        : addr(rm.addr), len(rm.length), key(rm.key) {}

    /**
     * @brief Equality comparison operator.
     */
    [[nodiscard]] constexpr auto operator==(const rma_iov&) const noexcept -> bool = default;
};

/**
 * @namespace loom::rma::detail
 * @brief Implementation details for RMA operations.
 */
namespace detail {

/// Maximum number of I/O vectors supported
inline constexpr std::size_t max_iov_count = 16;

/**
 * @brief Converts memory region descriptors to raw pointers.
 * @tparam N Maximum output array size.
 * @param desc Span of memory region descriptors.
 * @param out Output array for raw descriptor pointers.
 * @return Pointer to raw descriptor array, or nullptr if empty.
 */
template <std::size_t N>
[[nodiscard]] inline auto descriptors_to_raw(std::span<const mr_descriptor> desc,
                                             std::array<void*, N>& out) noexcept -> void** {
    if (desc.empty()) {
        return nullptr;
    }
    for (std::size_t i = 0; i < desc.size() && i < N; ++i) {
        out[i] = desc[i].raw();
    }
    return out.data();
}

}  // namespace detail

/**
 * @brief Reads data from remote memory into a local buffer.
 * @tparam Ctx The context type (must satisfy context_like).
 * @param ep The endpoint for the operation.
 * @param local_buffer Local buffer to read into.
 * @param local_mr Memory region for the local buffer.
 * @param remote Remote memory descriptor.
 * @param context Optional operation context.
 * @return Success or error result.
 */
template <context_like Ctx = void*>
auto read(endpoint& ep,
          std::span<std::byte> local_buffer,
          memory_region& local_mr,
          remote_memory remote,
          Ctx context = {}) -> result<void> {
    auto* fid_ep = static_cast<::fid_ep*>(ep.internal_ptr());
    auto* desc = local_mr.descriptor().raw();

    auto ret = ::fi_read(fid_ep,
                         local_buffer.data(),
                         local_buffer.size(),
                         desc,
                         FI_ADDR_UNSPEC,
                         remote.addr,
                         remote.key,
                         to_raw_context(context));

    if (ret != 0) {
        return make_error_result_from_fi_errno<void>(static_cast<int>(-ret));
    }

    return make_success();
}

/**
 * @brief Reads data from remote memory using scatter/gather I/O.
 * @tparam Ctx The context type (must satisfy context_like).
 * @param ep The endpoint for the operation.
 * @param local_iov Local I/O vectors for scatter read.
 * @param desc Memory region descriptors for local buffers.
 * @param remote_iov Remote I/O vectors specifying source.
 * @param context Optional operation context.
 * @return Success or error result.
 */
template <context_like Ctx = void*>
auto readv(endpoint& ep,
           std::span<const iovec> local_iov,
           std::span<const mr_descriptor> desc,
           std::span<const rma_iov> remote_iov,
           Ctx context = {}) -> result<void> {
    if (remote_iov.empty()) {
        return make_error_result<void>(errc::invalid_argument);
    }

    auto* fid_ep = static_cast<::fid_ep*>(ep.internal_ptr());

    std::array<void*, detail::max_iov_count> raw_desc{};

    auto ret = ::fi_readv(fid_ep,
                          local_iov.data(),
                          detail::descriptors_to_raw(desc, raw_desc),
                          local_iov.size(),
                          FI_ADDR_UNSPEC,
                          remote_iov[0].addr,
                          remote_iov[0].key,
                          to_raw_context(context));

    if (ret != 0) {
        return make_error_result_from_fi_errno<void>(static_cast<int>(-ret));
    }

    return make_success();
}

/**
 * @brief Writes data from a local buffer to remote memory.
 * @tparam Ctx The context type (must satisfy context_like).
 * @param ep The endpoint for the operation.
 * @param local_buffer Local buffer to write from.
 * @param local_mr Memory region for the local buffer.
 * @param remote Remote memory descriptor.
 * @param context Optional operation context.
 * @return Success or error result.
 */
template <context_like Ctx = void*>
auto write(endpoint& ep,
           std::span<const std::byte> local_buffer,
           memory_region& local_mr,
           remote_memory remote,
           Ctx context = {}) -> result<void> {
    auto* fid_ep = static_cast<::fid_ep*>(ep.internal_ptr());
    auto* desc = local_mr.descriptor().raw();

    auto ret = ::fi_write(fid_ep,
                          local_buffer.data(),
                          local_buffer.size(),
                          desc,
                          FI_ADDR_UNSPEC,
                          remote.addr,
                          remote.key,
                          to_raw_context(context));

    if (ret != 0) {
        return make_error_result_from_fi_errno<void>(static_cast<int>(-ret));
    }

    return make_success();
}

/**
 * @brief Writes data to remote memory using gather I/O.
 * @tparam Ctx The context type (must satisfy context_like).
 * @param ep The endpoint for the operation.
 * @param local_iov Local I/O vectors for gather write.
 * @param desc Memory region descriptors for local buffers.
 * @param remote_iov Remote I/O vectors specifying destination.
 * @param context Optional operation context.
 * @return Success or error result.
 */
template <context_like Ctx = void*>
auto writev(endpoint& ep,
            std::span<const iovec> local_iov,
            std::span<const mr_descriptor> desc,
            std::span<const rma_iov> remote_iov,
            Ctx context = {}) -> result<void> {
    if (remote_iov.empty()) {
        return make_error_result<void>(errc::invalid_argument);
    }

    auto* fid_ep = static_cast<::fid_ep*>(ep.internal_ptr());

    std::array<void*, detail::max_iov_count> raw_desc{};

    auto ret = ::fi_writev(fid_ep,
                           local_iov.data(),
                           detail::descriptors_to_raw(desc, raw_desc),
                           local_iov.size(),
                           FI_ADDR_UNSPEC,
                           remote_iov[0].addr,
                           remote_iov[0].key,
                           to_raw_context(context));

    if (ret != 0) {
        return make_error_result_from_fi_errno<void>(static_cast<int>(-ret));
    }

    return make_success();
}

/**
 * @brief Injects a small write to remote memory without buffering.
 * @param ep The endpoint for the operation.
 * @param data Data to write (must fit in inject size limit).
 * @param remote Remote memory descriptor.
 * @return Success or error result.
 */
auto inject_write(endpoint& ep, std::span<const std::byte> data, remote_memory remote)
    -> result<void>;

/**
 * @brief Writes data to remote memory with immediate data.
 * @tparam Ctx The context type (must satisfy context_like).
 * @param ep The endpoint for the operation.
 * @param local_buffer Local buffer to write from.
 * @param local_mr Memory region for the local buffer.
 * @param remote Remote memory descriptor.
 * @param data Immediate data to send with the write.
 * @param context Optional operation context.
 * @return Success or error result.
 */
template <context_like Ctx = void*>
auto write_data(endpoint& ep,
                std::span<const std::byte> local_buffer,
                memory_region& local_mr,
                remote_memory remote,
                std::uint64_t data,
                Ctx context = {}) -> result<void> {
    auto* fid_ep = static_cast<::fid_ep*>(ep.internal_ptr());
    auto* desc = local_mr.descriptor().raw();

    auto ret = ::fi_writedata(fid_ep,
                              local_buffer.data(),
                              local_buffer.size(),
                              desc,
                              data,
                              FI_ADDR_UNSPEC,
                              remote.addr,
                              remote.key,
                              to_raw_context(context));

    if (ret != 0) {
        return make_error_result_from_fi_errno<void>(static_cast<int>(-ret));
    }

    return make_success();
}

/**
 * @brief Injects a small write with immediate data to remote memory.
 * @param ep The endpoint for the operation.
 * @param data Data to write (must fit in inject size limit).
 * @param remote Remote memory descriptor.
 * @param immediate_data Immediate data to send with the write.
 * @return Success or error result.
 */
auto inject_write_data(endpoint& ep,
                       std::span<const std::byte> data,
                       remote_memory remote,
                       std::uint64_t immediate_data) -> result<void>;

/**
 * @brief Reads data from a specific remote address to a local buffer.
 * @tparam Ctx The context type (must satisfy context_like).
 * @param ep The endpoint for the operation.
 * @param local_buffer Local buffer to read into.
 * @param local_mr Memory region for the local buffer.
 * @param dest_addr Destination fabric address.
 * @param remote Remote memory descriptor.
 * @param context Optional operation context.
 * @return Success or error result.
 */
template <context_like Ctx = void*>
auto read_to(endpoint& ep,
             std::span<std::byte> local_buffer,
             memory_region& local_mr,
             fabric_addr dest_addr,
             remote_memory remote,
             Ctx context = {}) -> result<void> {
    auto* fid_ep = static_cast<::fid_ep*>(ep.internal_ptr());
    auto* desc = local_mr.descriptor().raw();

    auto ret = ::fi_read(fid_ep,
                         local_buffer.data(),
                         local_buffer.size(),
                         desc,
                         dest_addr.get(),
                         remote.addr,
                         remote.key,
                         to_raw_context(context));

    if (ret != 0) {
        return make_error_result_from_fi_errno<void>(static_cast<int>(-ret));
    }

    return make_success();
}

/**
 * @brief Writes data from a local buffer to a specific remote address.
 * @tparam Ctx The context type (must satisfy context_like).
 * @param ep The endpoint for the operation.
 * @param local_buffer Local buffer to write from.
 * @param local_mr Memory region for the local buffer.
 * @param dest_addr Destination fabric address.
 * @param remote Remote memory descriptor.
 * @param context Optional operation context.
 * @return Success or error result.
 */
template <context_like Ctx = void*>
auto write_to(endpoint& ep,
              std::span<const std::byte> local_buffer,
              memory_region& local_mr,
              fabric_addr dest_addr,
              remote_memory remote,
              Ctx context = {}) -> result<void> {
    auto* fid_ep = static_cast<::fid_ep*>(ep.internal_ptr());
    auto* desc = local_mr.descriptor().raw();

    auto ret = ::fi_write(fid_ep,
                          local_buffer.data(),
                          local_buffer.size(),
                          desc,
                          dest_addr.get(),
                          remote.addr,
                          remote.key,
                          to_raw_context(context));

    if (ret != 0) {
        return make_error_result_from_fi_errno<void>(static_cast<int>(-ret));
    }

    return make_success();
}

/**
 * @brief Gets the maximum RMA transfer size for an endpoint.
 * @param ep The endpoint to query.
 * @return Maximum RMA transfer size in bytes.
 */
auto get_max_rma_size(const endpoint& ep) -> std::size_t;

/**
 * @brief Gets the maximum inject size for RMA operations.
 * @param ep The endpoint to query.
 * @return Maximum inject size in bytes.
 */
auto get_inject_size(const endpoint& ep) -> std::size_t;

}  // namespace loom::rma
