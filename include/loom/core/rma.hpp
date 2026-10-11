// SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only
#pragma once

#include <rdma/fi_rma.h>

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include <sys/uio.h>

#include "loom/core/concepts/strong_types.hpp"
#include "loom/core/endpoint.hpp"
#include "loom/core/error.hpp"
#include "loom/core/memory.hpp"
#include "loom/core/result.hpp"
#include "loom/core/types.hpp"
#include "loom/detail/conversions.hpp"

/**
 * @namespace loom::rma
 * @brief Remote Memory Access (RMA) operations.
 */
namespace loom::rma {

namespace detail {
template <operation_defaults Defaults>
[[nodiscard]] inline auto operation_flags(const endpoint& ep, op_flags flags) noexcept
    -> std::uint64_t {
    return loom::detail::operation_flags<Defaults>(
        ep, completion_direction::transmit, loom::detail::to_fi_op_flags(flags));
}
}  // namespace detail

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
    if (fid_ep == nullptr) {
        return make_error_result<void>(errc::invalid_argument);
    }
    if (!FI_CHECK_OP(fid_ep->rma, fi_ops_rma, read)) {
        return make_error_result<void>(errc::not_supported);
    }
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
 * @param dest_addr Destination peer (ignored by connected endpoints).
 * @param remote_iov Remote I/O vectors specifying source.
 * @param flags Per-call modifiers; Defaults selects replacement or inheritance.
 * @param context Optional operation context.
 * @return Success or error result.
 */
template <operation_defaults Defaults = operation_defaults::replace, context_like Ctx = void*>
auto readv_to(endpoint& ep,
              std::span<const iovec> local_iov,
              std::span<const mr_descriptor> desc,
              fabric_addr dest_addr,
              std::span<const rma_iov> remote_iov,
              op_flags flags,
              Ctx context = {}) -> result<void> {
    if (remote_iov.empty() || (!desc.empty() && desc.size() != local_iov.size())) {
        return make_error_result<void>(errc::invalid_argument);
    }

    auto* fid_ep = static_cast<::fid_ep*>(ep.internal_ptr());
    if (fid_ep == nullptr) {
        return make_error_result<void>(errc::invalid_argument);
    }

    if (!FI_CHECK_OP(fid_ep->rma, fi_ops_rma, readmsg)) {
        return make_error_result<void>(errc::not_supported);
    }

    std::vector<void*> raw_desc;
    raw_desc.reserve(desc.size());
    for (const auto descriptor : desc) {
        raw_desc.push_back(descriptor.raw());
    }
    std::vector<::fi_rma_iov> raw_remote;
    raw_remote.reserve(remote_iov.size());
    for (const auto& remote : remote_iov) {
        raw_remote.push_back({remote.addr, remote.len, remote.key});
    }
    const ::fi_msg_rma message{
        .msg_iov = local_iov.data(),
        .desc = raw_desc.empty() ? nullptr : raw_desc.data(),
        .iov_count = local_iov.size(),
        .addr = dest_addr.get(),
        .rma_iov = raw_remote.data(),
        .rma_iov_count = raw_remote.size(),
        .context = to_raw_context(context),
    };
    const auto ret = ::fi_readmsg(fid_ep, &message, detail::operation_flags<Defaults>(ep, flags));

    if (ret != 0) {
        return make_error_result_from_fi_errno<void>(static_cast<int>(-ret));
    }

    return make_success();
}

template <context_like Ctx = void*>
auto readv_to(endpoint& ep,
              std::span<const iovec> local_iov,
              std::span<const mr_descriptor> desc,
              fabric_addr dest_addr,
              std::span<const rma_iov> remote_iov,
              Ctx context = {}) -> result<void> {
    return readv_to(ep, local_iov, desc, dest_addr, remote_iov, op_flag::none, context);
}

/// Connected-endpoint convenience for vectored RMA.
template <context_like Ctx = void*>
auto readv(endpoint& ep,
           std::span<const iovec> local_iov,
           std::span<const mr_descriptor> desc,
           std::span<const rma_iov> remote_iov,
           Ctx context = {}) -> result<void> {
    return readv_to(ep, local_iov, desc, fabric_addrs::unspecified, remote_iov, context);
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
    if (fid_ep == nullptr) {
        return make_error_result<void>(errc::invalid_argument);
    }
    if (!FI_CHECK_OP(fid_ep->rma, fi_ops_rma, write)) {
        return make_error_result<void>(errc::not_supported);
    }
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
 * @param dest_addr Destination peer (ignored by connected endpoints).
 * @param remote_iov Remote I/O vectors specifying destination.
 * @param flags Per-call modifiers; Defaults selects replacement or inheritance.
 * @param context Optional operation context.
 * @return Success or error result.
 */
template <operation_defaults Defaults = operation_defaults::replace, context_like Ctx = void*>
auto writev_to(endpoint& ep,
               std::span<const iovec> local_iov,
               std::span<const mr_descriptor> desc,
               fabric_addr dest_addr,
               std::span<const rma_iov> remote_iov,
               op_flags flags,
               Ctx context = {}) -> result<void> {
    if (remote_iov.empty() || (!desc.empty() && desc.size() != local_iov.size())) {
        return make_error_result<void>(errc::invalid_argument);
    }

    auto* fid_ep = static_cast<::fid_ep*>(ep.internal_ptr());
    if (fid_ep == nullptr) {
        return make_error_result<void>(errc::invalid_argument);
    }

    if (!FI_CHECK_OP(fid_ep->rma, fi_ops_rma, writemsg)) {
        return make_error_result<void>(errc::not_supported);
    }

    std::vector<void*> raw_desc;
    raw_desc.reserve(desc.size());
    for (const auto descriptor : desc) {
        raw_desc.push_back(descriptor.raw());
    }
    std::vector<::fi_rma_iov> raw_remote;
    raw_remote.reserve(remote_iov.size());
    for (const auto& remote : remote_iov) {
        raw_remote.push_back({remote.addr, remote.len, remote.key});
    }
    const ::fi_msg_rma message{
        .msg_iov = local_iov.data(),
        .desc = raw_desc.empty() ? nullptr : raw_desc.data(),
        .iov_count = local_iov.size(),
        .addr = dest_addr.get(),
        .rma_iov = raw_remote.data(),
        .rma_iov_count = raw_remote.size(),
        .context = to_raw_context(context),
    };
    const auto ret = ::fi_writemsg(fid_ep, &message, detail::operation_flags<Defaults>(ep, flags));

    if (ret != 0) {
        return make_error_result_from_fi_errno<void>(static_cast<int>(-ret));
    }

    return make_success();
}

template <context_like Ctx = void*>
auto writev_to(endpoint& ep,
               std::span<const iovec> local_iov,
               std::span<const mr_descriptor> desc,
               fabric_addr dest_addr,
               std::span<const rma_iov> remote_iov,
               Ctx context = {}) -> result<void> {
    return writev_to(ep, local_iov, desc, dest_addr, remote_iov, op_flag::none, context);
}

/// Connected-endpoint convenience for vectored RMA.
template <context_like Ctx = void*>
auto writev(endpoint& ep,
            std::span<const iovec> local_iov,
            std::span<const mr_descriptor> desc,
            std::span<const rma_iov> remote_iov,
            Ctx context = {}) -> result<void> {
    return writev_to(ep, local_iov, desc, fabric_addrs::unspecified, remote_iov, context);
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
    if (fid_ep == nullptr) {
        return make_error_result<void>(errc::invalid_argument);
    }
    if (!FI_CHECK_OP(fid_ep->rma, fi_ops_rma, writedata)) {
        return make_error_result<void>(errc::not_supported);
    }
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
 * @param local_descriptor Descriptor for the local buffer, or empty when not required.
 * @param dest_addr Destination fabric address.
 * @param remote Remote memory descriptor.
 * @param context Optional operation context.
 * @return Success or error result.
 */
/// Per-call flags use the native message interface. Inheritance extends the
/// endpoint's creation defaults; raw native control changes are not tracked.
/// Buffers and registrations remain borrowed until the requested completion.
template <operation_defaults Defaults = operation_defaults::replace, context_like Ctx = void*>
auto read_to(endpoint& ep,
             std::span<std::byte> local_buffer,
             mr_descriptor local_descriptor,
             fabric_addr dest_addr,
             remote_memory remote,
             op_flags flags,
             Ctx context = {}) -> result<void> {
    auto* fid_ep = static_cast<::fid_ep*>(ep.internal_ptr());
    if (!fid_ep || local_buffer.size() > remote.length) {
        return make_error_result<void>(errc::invalid_argument);
    }
    if (!FI_CHECK_OP(fid_ep->rma, fi_ops_rma, readmsg)) {
        return make_error_result<void>(errc::not_supported);
    }
    const ::iovec local{local_buffer.data(), local_buffer.size()};
    auto* descriptor = local_descriptor.raw();
    const ::fi_rma_iov target{remote.addr, local_buffer.size(), remote.key};
    const ::fi_msg_rma message{
        .msg_iov = &local,
        .desc = descriptor ? &descriptor : nullptr,
        .iov_count = 1,
        .addr = dest_addr.get(),
        .rma_iov = &target,
        .rma_iov_count = 1,
        .context = to_raw_context(context),
    };
    const auto status =
        ::fi_readmsg(fid_ep, &message, detail::operation_flags<Defaults>(ep, flags));
    if (status) {
        return make_error_result_from_fi_errno<void>(static_cast<int>(-status));
    }
    return make_success();
}

template <context_like Ctx = void*>
auto read_to(endpoint& ep,
             std::span<std::byte> local_buffer,
             mr_descriptor local_descriptor,
             fabric_addr dest_addr,
             remote_memory remote,
             Ctx context = {}) -> result<void> {
    auto* fid_ep = static_cast<::fid_ep*>(ep.internal_ptr());
    if (fid_ep == nullptr) {
        return make_error_result<void>(errc::invalid_argument);
    }
    if (!FI_CHECK_OP(fid_ep->rma, fi_ops_rma, read)) {
        return make_error_result<void>(errc::not_supported);
    }
    auto* desc = local_descriptor.raw();

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

/// Use a registration's descriptor for the local buffer.
template <context_like Ctx = void*>
auto read_to(endpoint& ep,
             std::span<std::byte> local_buffer,
             memory_region& local_mr,
             fabric_addr dest_addr,
             remote_memory remote,
             Ctx context = {}) -> result<void> {
    return read_to(ep, local_buffer, local_mr.descriptor(), dest_addr, remote, context);
}

/// Use a registration's descriptor for the local buffer.
template <operation_defaults Defaults = operation_defaults::replace, context_like Ctx = void*>
auto read_to(endpoint& ep,
             std::span<std::byte> local_buffer,
             memory_region& local_mr,
             fabric_addr dest_addr,
             remote_memory remote,
             op_flags flags,
             Ctx context = {}) -> result<void> {
    return read_to<Defaults>(
        ep, local_buffer, local_mr.descriptor(), dest_addr, remote, flags, context);
}

/**
 * @brief Writes data from a local buffer to a specific remote address.
 * @tparam Ctx The context type (must satisfy context_like).
 * @param ep The endpoint for the operation.
 * @param local_buffer Local buffer to write from.
 * @param local_descriptor Descriptor for the local buffer, or empty when not required.
 * @param dest_addr Destination fabric address.
 * @param remote Remote memory descriptor.
 * @param context Optional operation context.
 * @return Success or error result.
 */
/// Per-call flags use the native message interface. Inheritance extends the
/// endpoint's creation defaults; raw native control changes are not tracked.
/// Buffers and registrations remain borrowed until the requested completion.
template <operation_defaults Defaults = operation_defaults::replace, context_like Ctx = void*>
auto write_to(endpoint& ep,
              std::span<const std::byte> local_buffer,
              mr_descriptor local_descriptor,
              fabric_addr dest_addr,
              remote_memory remote,
              op_flags flags,
              Ctx context = {}) -> result<void> {
    auto* fid_ep = static_cast<::fid_ep*>(ep.internal_ptr());
    if (!fid_ep || local_buffer.size() > remote.length) {
        return make_error_result<void>(errc::invalid_argument);
    }
    if (!FI_CHECK_OP(fid_ep->rma, fi_ops_rma, writemsg)) {
        return make_error_result<void>(errc::not_supported);
    }
    const ::iovec local{const_cast<std::byte*>(local_buffer.data()), local_buffer.size()};
    auto* descriptor = local_descriptor.raw();
    const ::fi_rma_iov target{remote.addr, local_buffer.size(), remote.key};
    const ::fi_msg_rma message{
        .msg_iov = &local,
        .desc = descriptor ? &descriptor : nullptr,
        .iov_count = 1,
        .addr = dest_addr.get(),
        .rma_iov = &target,
        .rma_iov_count = 1,
        .context = to_raw_context(context),
    };
    const auto status =
        ::fi_writemsg(fid_ep, &message, detail::operation_flags<Defaults>(ep, flags));
    if (status) {
        return make_error_result_from_fi_errno<void>(static_cast<int>(-status));
    }
    return make_success();
}

template <context_like Ctx = void*>
auto write_to(endpoint& ep,
              std::span<const std::byte> local_buffer,
              mr_descriptor local_descriptor,
              fabric_addr dest_addr,
              remote_memory remote,
              Ctx context = {}) -> result<void> {
    auto* fid_ep = static_cast<::fid_ep*>(ep.internal_ptr());
    if (fid_ep == nullptr) {
        return make_error_result<void>(errc::invalid_argument);
    }
    if (!FI_CHECK_OP(fid_ep->rma, fi_ops_rma, write)) {
        return make_error_result<void>(errc::not_supported);
    }
    auto* desc = local_descriptor.raw();

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

/// Use a registration's descriptor for the local buffer.
template <context_like Ctx = void*>
auto write_to(endpoint& ep,
              std::span<const std::byte> local_buffer,
              memory_region& local_mr,
              fabric_addr dest_addr,
              remote_memory remote,
              Ctx context = {}) -> result<void> {
    return write_to(ep, local_buffer, local_mr.descriptor(), dest_addr, remote, context);
}

/// Use a registration's descriptor for the local buffer.
template <operation_defaults Defaults = operation_defaults::replace, context_like Ctx = void*>
auto write_to(endpoint& ep,
              std::span<const std::byte> local_buffer,
              memory_region& local_mr,
              fabric_addr dest_addr,
              remote_memory remote,
              op_flags flags,
              Ctx context = {}) -> result<void> {
    return write_to<Defaults>(
        ep, local_buffer, local_mr.descriptor(), dest_addr, remote, flags, context);
}

/**
 * @brief Gets the maximum RMA transfer size for an endpoint.
 * @param ep The endpoint to query.
 * @return Maximum RMA transfer size in bytes, or zero if unavailable.
 * Uses the endpoint option, falling back to its creation attributes when
 * the provider does not implement the query.
 */
auto get_max_rma_size(const endpoint& ep) -> std::size_t;

/**
 * @brief Gets the maximum inject size for RMA operations.
 * @param ep The endpoint to query.
 * @return Maximum inject size in bytes, or zero if unavailable.
 * Uses the endpoint option, falling back to its creation attributes when
 * the provider does not implement the query.
 */
auto get_inject_size(const endpoint& ep) -> std::size_t;

}  // namespace loom::rma
