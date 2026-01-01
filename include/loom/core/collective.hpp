// SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only
#pragma once

#include <rdma/fi_collective.h>

#include <cstddef>
#include <span>

#include "loom/core/atomic.hpp"
#include "loom/core/concepts/strong_types.hpp"
#include "loom/core/crtp/fabric_object_base.hpp"
#include "loom/core/domain.hpp"
#include "loom/core/endpoint.hpp"
#include "loom/core/memory.hpp"
#include "loom/core/result.hpp"
#include "loom/core/types.hpp"

/**
 * @namespace loom::collective
 * @brief Collective communication operations.
 */
namespace loom::collective {

/**
 * @enum operation
 * @brief Types of collective communication operations.
 */
enum class operation {
    barrier,         ///< Synchronization barrier
    broadcast,       ///< One-to-all broadcast
    all_to_all,      ///< All-to-all exchange
    all_reduce,      ///< Reduction with result to all
    all_gather,      ///< Gather with result to all
    reduce_scatter,  ///< Reduce and scatter result
    reduce,          ///< Reduction to root
    scatter,         ///< Scatter from root
    gather,          ///< Gather to root
};

/**
 * @namespace loom::collective::detail
 * @brief Implementation details for collective operations.
 */
namespace detail {

/**
 * @brief Converts loom atomic operation to libfabric operation.
 * @param op The loom atomic operation.
 * @return The corresponding libfabric operation.
 */
constexpr auto to_fi_op(atomic::operation op) noexcept -> ::fi_op {
    switch (op) {
        case atomic::operation::min:
            return FI_MIN;
        case atomic::operation::max:
            return FI_MAX;
        case atomic::operation::sum:
            return FI_SUM;
        case atomic::operation::prod:
            return FI_PROD;
        case atomic::operation::logical_or:
            return FI_LOR;
        case atomic::operation::logical_and:
            return FI_LAND;
        case atomic::operation::bitwise_or:
            return FI_BOR;
        case atomic::operation::bitwise_and:
            return FI_BAND;
        case atomic::operation::logical_xor:
            return FI_LXOR;
        case atomic::operation::bitwise_xor:
            return FI_BXOR;
        default:
            return FI_NOOP;
    }
}

/**
 * @brief Converts loom atomic datatype to libfabric datatype.
 * @param dt The loom atomic datatype.
 * @return The corresponding libfabric datatype.
 */
constexpr auto to_fi_datatype(atomic::datatype dt) noexcept -> ::fi_datatype {
    switch (dt) {
        case atomic::datatype::int8:
            return FI_INT8;
        case atomic::datatype::uint8:
            return FI_UINT8;
        case atomic::datatype::int16:
            return FI_INT16;
        case atomic::datatype::uint16:
            return FI_UINT16;
        case atomic::datatype::int32:
            return FI_INT32;
        case atomic::datatype::uint32:
            return FI_UINT32;
        case atomic::datatype::int64:
            return FI_INT64;
        case atomic::datatype::uint64:
            return FI_UINT64;
        case atomic::datatype::float32:
            return FI_FLOAT;
        case atomic::datatype::double64:
            return FI_DOUBLE;
        case atomic::datatype::float_complex:
            return FI_FLOAT_COMPLEX;
        case atomic::datatype::double_complex:
            return FI_DOUBLE_COMPLEX;
        case atomic::datatype::long_double:
            return FI_LONG_DOUBLE;
        case atomic::datatype::long_double_complex:
            return FI_LONG_DOUBLE_COMPLEX;
        case atomic::datatype::int128:
            return FI_INT128;
        case atomic::datatype::uint128:
            return FI_UINT128;
    }
    return FI_INT8;
}

}  // namespace detail

/**
 * @class group
 * @brief A collective communication group.
 *
 * Represents a set of endpoints participating in collective operations.
 */
class group : public fabric_object_base<group> {
public:
    /**
     * @brief Default constructor.
     */
    group() = default;

    /**
     * @brief Deleted copy constructor.
     */
    group(const group&) = delete;
    /**
     * @brief Deleted copy assignment operator.
     */
    auto operator=(const group&) -> group& = delete;
    /**
     * @brief Move constructor.
     */
    group(group&&) noexcept;
    /// Move assignment operator
    auto operator=(group&&) noexcept -> group&;

    /**
     * @brief Destructor.
     */
    ~group();

    /**
     * @brief Joins an endpoint to a collective group.
     * @tparam Ctx The context type (must satisfy context_like).
     * @param ep The endpoint to join.
     * @param coll_addr The collective address for the group.
     * @param context Optional operation context.
     * @return The group or error result.
     */
    template <context_like Ctx = void*>
    [[nodiscard]] static auto join(endpoint& ep, fabric_addr coll_addr, Ctx context = {})
        -> result<group>;

    /// Gets the fabric address of this group
    [[nodiscard]] auto address() const noexcept -> fabric_addr;

    /// Gets the internal pointer for fabric object interface
    [[nodiscard]] auto impl_internal_ptr() const noexcept -> void*;
    /// Checks if the group is valid
    [[nodiscard]] auto impl_valid() const noexcept -> bool;

private:
    /// Implementation struct (pimpl)
    struct impl;
    std::unique_ptr<impl> impl_;

    /**
     * @brief Factory function for creating group implementation.
     * @param ep The endpoint.
     * @param coll_addr The collective address.
     * @param context The operation context.
     * @return The group or error result.
     */
    friend auto create_group_impl(endpoint& ep, fabric_addr coll_addr, void* context)
        -> result<group>;
};

/**
 * @brief Creates a group implementation (internal).
 * @param ep The endpoint.
 * @param coll_addr The collective address.
 * @param context The operation context.
 * @return The group or error result.
 */
auto create_group_impl(endpoint& ep, fabric_addr coll_addr, void* context) -> result<group>;

/**
 * @brief Joins an endpoint to a collective group.
 * @tparam Ctx The context type.
 * @param ep The endpoint.
 * @param coll_addr The collective address.
 * @param context The operation context.
 * @return The group or error result.
 */
template <context_like Ctx>
auto group::join(endpoint& ep, fabric_addr coll_addr, Ctx context) -> result<group> {
    return create_group_impl(ep, coll_addr, to_raw_context(context));
}

/**
 * @brief Performs a barrier synchronization.
 * @tparam Ctx The context type (must satisfy context_like).
 * @param ep The endpoint for the operation.
 * @param coll_addr The collective address.
 * @param context Optional operation context.
 * @return Success or error result.
 */
template <context_like Ctx = void*>
auto barrier(endpoint& ep, fabric_addr coll_addr, Ctx context = {}) -> result<void> {
    auto* fid_ep = static_cast<::fid_ep*>(ep.internal_ptr());

    auto ret = ::fi_barrier(fid_ep, coll_addr.get(), to_raw_context(context));

    if (ret != 0) {
        return make_error_result_from_fi_errno<void>(static_cast<int>(-ret));
    }

    return make_success();
}

/**
 * @brief Performs a barrier synchronization with flags.
 * @tparam Ctx The context type (must satisfy context_like).
 * @param ep The endpoint for the operation.
 * @param grp The collective group.
 * @param flags Operation flags.
 * @param context Optional operation context.
 * @return Success or error result.
 */
template <context_like Ctx = void*>
auto barrier(endpoint& ep, group& grp, std::uint64_t flags, Ctx context = {}) -> result<void> {
    auto* fid_ep = static_cast<::fid_ep*>(ep.internal_ptr());

    auto ret = ::fi_barrier2(fid_ep, grp.address().get(), flags, to_raw_context(context));

    if (ret != 0) {
        return make_error_result_from_fi_errno<void>(static_cast<int>(-ret));
    }

    return make_success();
}

/**
 * @brief Broadcasts data from root to all group members.
 * @tparam Ctx The context type (must satisfy context_like).
 * @param ep The endpoint for the operation.
 * @param grp The collective group.
 * @param buffer Buffer to broadcast (source at root, destination elsewhere).
 * @param mr Optional memory region for the buffer.
 * @param root_addr Address of the root node.
 * @param dt Data type of the buffer elements.
 * @param context Optional operation context.
 * @return Success or error result.
 */
template <context_like Ctx = void*>
auto broadcast(endpoint& ep,
               group& grp,
               std::span<std::byte> buffer,
               memory_region* mr,
               fabric_addr root_addr,
               atomic::datatype dt,
               Ctx context = {}) -> result<void> {
    auto* fid_ep = static_cast<::fid_ep*>(ep.internal_ptr());
    void* desc = mr ? mr->descriptor().raw() : nullptr;

    auto ret = ::fi_broadcast(fid_ep,
                              buffer.data(),
                              buffer.size(),
                              desc,
                              grp.address().get(),
                              root_addr.get(),
                              detail::to_fi_datatype(dt),
                              0,
                              to_raw_context(context));

    if (ret != 0) {
        return make_error_result_from_fi_errno<void>(static_cast<int>(-ret));
    }

    return make_success();
}

/**
 * @brief Broadcasts typed data from root to all group members.
 * @tparam T The element type.
 * @tparam Ctx The context type (must satisfy context_like).
 * @param ep The endpoint for the operation.
 * @param grp The collective group.
 * @param data Data span to broadcast.
 * @param mr Optional memory region for the data.
 * @param root_addr Address of the root node.
 * @param context Optional operation context.
 * @return Success or error result.
 */
template <typename T, context_like Ctx = void*>
auto broadcast(endpoint& ep,
               group& grp,
               std::span<T> data,
               memory_region* mr,
               fabric_addr root_addr,
               Ctx context = {}) -> result<void> {
    return broadcast(
        ep, grp, std::as_writable_bytes(data), mr, root_addr, atomic::get_datatype<T>(), context);
}

/**
 * @brief Performs an all-reduce operation across group members.
 * @tparam Ctx The context type (must satisfy context_like).
 * @param ep The endpoint for the operation.
 * @param grp The collective group.
 * @param send_buf Data to reduce.
 * @param recv_buf Buffer to receive reduced result.
 * @param send_mr Optional memory region for send buffer.
 * @param recv_mr Optional memory region for receive buffer.
 * @param op The reduction operation.
 * @param dt Data type of the elements.
 * @param context Optional operation context.
 * @return Success or error result.
 */
template <context_like Ctx = void*>
auto all_reduce(endpoint& ep,
                group& grp,
                std::span<const std::byte> send_buf,
                std::span<std::byte> recv_buf,
                memory_region* send_mr,
                memory_region* recv_mr,
                atomic::operation op,
                atomic::datatype dt,
                Ctx context = {}) -> result<void> {
    auto* fid_ep = static_cast<::fid_ep*>(ep.internal_ptr());
    void* send_desc = send_mr ? send_mr->descriptor().raw() : nullptr;
    void* recv_desc = recv_mr ? recv_mr->descriptor().raw() : nullptr;

    auto ret = ::fi_allreduce(fid_ep,
                              send_buf.data(),
                              send_buf.size(),
                              send_desc,
                              recv_buf.data(),
                              recv_desc,
                              grp.address().get(),
                              detail::to_fi_datatype(dt),
                              detail::to_fi_op(op),
                              0,
                              to_raw_context(context));

    if (ret != 0) {
        return make_error_result_from_fi_errno<void>(static_cast<int>(-ret));
    }

    return make_success();
}

/**
 * @brief Performs a typed all-reduce operation.
 * @tparam T The element type.
 * @tparam Ctx The context type (must satisfy context_like).
 * @param ep The endpoint for the operation.
 * @param grp The collective group.
 * @param send_data Data to reduce.
 * @param recv_data Buffer to receive reduced result.
 * @param send_mr Optional memory region for send data.
 * @param recv_mr Optional memory region for receive data.
 * @param op The reduction operation.
 * @param context Optional operation context.
 * @return Success or error result.
 */
template <typename T, context_like Ctx = void*>
auto all_reduce(endpoint& ep,
                group& grp,
                std::span<const T> send_data,
                std::span<T> recv_data,
                memory_region* send_mr,
                memory_region* recv_mr,
                atomic::operation op,
                Ctx context = {}) -> result<void> {
    return all_reduce(ep,
                      grp,
                      std::as_bytes(send_data),
                      std::as_writable_bytes(recv_data),
                      send_mr,
                      recv_mr,
                      op,
                      atomic::get_datatype<T>(),
                      context);
}

/**
 * @brief Performs an in-place all-reduce operation.
 * @tparam T The element type.
 * @tparam Ctx The context type (must satisfy context_like).
 * @param ep The endpoint for the operation.
 * @param grp The collective group.
 * @param data Data to reduce (result stored in place).
 * @param mr Optional memory region for the data.
 * @param op The reduction operation.
 * @param context Optional operation context.
 * @return Success or error result.
 */
template <typename T, context_like Ctx = void*>
auto all_reduce_inplace(endpoint& ep,
                        group& grp,
                        std::span<T> data,
                        memory_region* mr,
                        atomic::operation op,
                        Ctx context = {}) -> result<void> {
    return all_reduce(ep,
                      grp,
                      std::as_bytes(data),
                      std::as_writable_bytes(data),
                      mr,
                      mr,
                      op,
                      atomic::get_datatype<T>(),
                      context);
}

/**
 * @brief Gathers data from all group members to all members.
 * @tparam Ctx The context type (must satisfy context_like).
 * @param ep The endpoint for the operation.
 * @param grp The collective group.
 * @param send_buf Data to contribute.
 * @param recv_buf Buffer to receive gathered data from all members.
 * @param send_mr Optional memory region for send buffer.
 * @param recv_mr Optional memory region for receive buffer.
 * @param dt Data type of the elements.
 * @param context Optional operation context.
 * @return Success or error result.
 */
template <context_like Ctx = void*>
auto all_gather(endpoint& ep,
                group& grp,
                std::span<const std::byte> send_buf,
                std::span<std::byte> recv_buf,
                memory_region* send_mr,
                memory_region* recv_mr,
                atomic::datatype dt,
                Ctx context = {}) -> result<void> {
    auto* fid_ep = static_cast<::fid_ep*>(ep.internal_ptr());
    void* send_desc = send_mr ? send_mr->descriptor().raw() : nullptr;
    void* recv_desc = recv_mr ? recv_mr->descriptor().raw() : nullptr;

    auto ret = ::fi_allgather(fid_ep,
                              send_buf.data(),
                              send_buf.size(),
                              send_desc,
                              recv_buf.data(),
                              recv_desc,
                              grp.address().get(),
                              detail::to_fi_datatype(dt),
                              0,
                              to_raw_context(context));

    if (ret != 0) {
        return make_error_result_from_fi_errno<void>(static_cast<int>(-ret));
    }

    return make_success();
}

/**
 * @brief Gathers typed data from all group members to all members.
 * @tparam T The element type.
 * @tparam Ctx The context type (must satisfy context_like).
 * @param ep The endpoint for the operation.
 * @param grp The collective group.
 * @param send_data Data to contribute.
 * @param recv_data Buffer to receive gathered data.
 * @param send_mr Optional memory region for send data.
 * @param recv_mr Optional memory region for receive data.
 * @param context Optional operation context.
 * @return Success or error result.
 */
template <typename T, context_like Ctx = void*>
auto all_gather(endpoint& ep,
                group& grp,
                std::span<const T> send_data,
                std::span<T> recv_data,
                memory_region* send_mr,
                memory_region* recv_mr,
                Ctx context = {}) -> result<void> {
    return all_gather(ep,
                      grp,
                      std::as_bytes(send_data),
                      std::as_writable_bytes(recv_data),
                      send_mr,
                      recv_mr,
                      atomic::get_datatype<T>(),
                      context);
}

/**
 * @brief Exchanges data between all group members.
 * @tparam Ctx The context type (must satisfy context_like).
 * @param ep The endpoint for the operation.
 * @param grp The collective group.
 * @param send_buf Data to send to each member.
 * @param recv_buf Buffer to receive data from each member.
 * @param send_mr Optional memory region for send buffer.
 * @param recv_mr Optional memory region for receive buffer.
 * @param dt Data type of the elements.
 * @param context Optional operation context.
 * @return Success or error result.
 */
template <context_like Ctx = void*>
auto all_to_all(endpoint& ep,
                group& grp,
                std::span<const std::byte> send_buf,
                std::span<std::byte> recv_buf,
                memory_region* send_mr,
                memory_region* recv_mr,
                atomic::datatype dt,
                Ctx context = {}) -> result<void> {
    auto* fid_ep = static_cast<::fid_ep*>(ep.internal_ptr());
    void* send_desc = send_mr ? send_mr->descriptor().raw() : nullptr;
    void* recv_desc = recv_mr ? recv_mr->descriptor().raw() : nullptr;

    auto ret = ::fi_alltoall(fid_ep,
                             send_buf.data(),
                             send_buf.size(),
                             send_desc,
                             recv_buf.data(),
                             recv_desc,
                             grp.address().get(),
                             detail::to_fi_datatype(dt),
                             0,
                             to_raw_context(context));

    if (ret != 0) {
        return make_error_result_from_fi_errno<void>(static_cast<int>(-ret));
    }

    return make_success();
}

/**
 * @brief Exchanges typed data between all group members.
 * @tparam T The element type.
 * @tparam Ctx The context type (must satisfy context_like).
 * @param ep The endpoint for the operation.
 * @param grp The collective group.
 * @param send_data Data to send to each member.
 * @param recv_data Buffer to receive data from each member.
 * @param send_mr Optional memory region for send data.
 * @param recv_mr Optional memory region for receive data.
 * @param context Optional operation context.
 * @return Success or error result.
 */
template <typename T, context_like Ctx = void*>
auto all_to_all(endpoint& ep,
                group& grp,
                std::span<const T> send_data,
                std::span<T> recv_data,
                memory_region* send_mr,
                memory_region* recv_mr,
                Ctx context = {}) -> result<void> {
    return all_to_all(ep,
                      grp,
                      std::as_bytes(send_data),
                      std::as_writable_bytes(recv_data),
                      send_mr,
                      recv_mr,
                      atomic::get_datatype<T>(),
                      context);
}

/**
 * @brief Reduces data from all group members to a root node.
 * @tparam Ctx The context type (must satisfy context_like).
 * @param ep The endpoint for the operation.
 * @param grp The collective group.
 * @param send_buf Data to reduce.
 * @param recv_buf Buffer for reduced result (only valid at root).
 * @param send_mr Optional memory region for send buffer.
 * @param recv_mr Optional memory region for receive buffer.
 * @param root_addr Address of the root node.
 * @param op The reduction operation.
 * @param dt Data type of the elements.
 * @param context Optional operation context.
 * @return Success or error result.
 */
template <context_like Ctx = void*>
auto reduce(endpoint& ep,
            group& grp,
            std::span<const std::byte> send_buf,
            std::span<std::byte> recv_buf,
            memory_region* send_mr,
            memory_region* recv_mr,
            fabric_addr root_addr,
            atomic::operation op,
            atomic::datatype dt,
            Ctx context = {}) -> result<void> {
    auto* fid_ep = static_cast<::fid_ep*>(ep.internal_ptr());
    void* send_desc = send_mr ? send_mr->descriptor().raw() : nullptr;
    void* recv_desc = recv_mr ? recv_mr->descriptor().raw() : nullptr;

    auto ret = ::fi_reduce(fid_ep,
                           send_buf.data(),
                           send_buf.size(),
                           send_desc,
                           recv_buf.data(),
                           recv_desc,
                           grp.address().get(),
                           root_addr.get(),
                           detail::to_fi_datatype(dt),
                           detail::to_fi_op(op),
                           0,
                           to_raw_context(context));

    if (ret != 0) {
        return make_error_result_from_fi_errno<void>(static_cast<int>(-ret));
    }

    return make_success();
}

/**
 * @brief Reduces typed data from all group members to a root node.
 * @tparam T The element type.
 * @tparam Ctx The context type (must satisfy context_like).
 * @param ep The endpoint for the operation.
 * @param grp The collective group.
 * @param send_data Data to reduce.
 * @param recv_data Buffer for reduced result (only valid at root).
 * @param send_mr Optional memory region for send data.
 * @param recv_mr Optional memory region for receive data.
 * @param root_addr Address of the root node.
 * @param op The reduction operation.
 * @param context Optional operation context.
 * @return Success or error result.
 */
template <typename T, context_like Ctx = void*>
auto reduce(endpoint& ep,
            group& grp,
            std::span<const T> send_data,
            std::span<T> recv_data,
            memory_region* send_mr,
            memory_region* recv_mr,
            fabric_addr root_addr,
            atomic::operation op,
            Ctx context = {}) -> result<void> {
    return reduce(ep,
                  grp,
                  std::as_bytes(send_data),
                  std::as_writable_bytes(recv_data),
                  send_mr,
                  recv_mr,
                  root_addr,
                  op,
                  atomic::get_datatype<T>(),
                  context);
}

/**
 * @brief Scatters data from root to all group members.
 * @tparam Ctx The context type (must satisfy context_like).
 * @param ep The endpoint for the operation.
 * @param grp The collective group.
 * @param send_buf Data to scatter (only at root).
 * @param recv_buf Buffer to receive portion of scattered data.
 * @param send_mr Optional memory region for send buffer.
 * @param recv_mr Optional memory region for receive buffer.
 * @param root_addr Address of the root node.
 * @param dt Data type of the elements.
 * @param context Optional operation context.
 * @return Success or error result.
 */
template <context_like Ctx = void*>
auto scatter(endpoint& ep,
             group& grp,
             std::span<const std::byte> send_buf,
             std::span<std::byte> recv_buf,
             memory_region* send_mr,
             memory_region* recv_mr,
             fabric_addr root_addr,
             atomic::datatype dt,
             Ctx context = {}) -> result<void> {
    auto* fid_ep = static_cast<::fid_ep*>(ep.internal_ptr());
    void* send_desc = send_mr ? send_mr->descriptor().raw() : nullptr;
    void* recv_desc = recv_mr ? recv_mr->descriptor().raw() : nullptr;

    auto ret = ::fi_scatter(fid_ep,
                            send_buf.data(),
                            send_buf.size(),
                            send_desc,
                            recv_buf.data(),
                            recv_desc,
                            grp.address().get(),
                            root_addr.get(),
                            detail::to_fi_datatype(dt),
                            0,
                            to_raw_context(context));

    if (ret != 0) {
        return make_error_result_from_fi_errno<void>(static_cast<int>(-ret));
    }

    return make_success();
}

/**
 * @brief Gathers data from all group members to a root node.
 * @tparam Ctx The context type (must satisfy context_like).
 * @param ep The endpoint for the operation.
 * @param grp The collective group.
 * @param send_buf Data to contribute.
 * @param recv_buf Buffer to receive all gathered data (only at root).
 * @param send_mr Optional memory region for send buffer.
 * @param recv_mr Optional memory region for receive buffer.
 * @param root_addr Address of the root node.
 * @param dt Data type of the elements.
 * @param context Optional operation context.
 * @return Success or error result.
 */
template <context_like Ctx = void*>
auto gather(endpoint& ep,
            group& grp,
            std::span<const std::byte> send_buf,
            std::span<std::byte> recv_buf,
            memory_region* send_mr,
            memory_region* recv_mr,
            fabric_addr root_addr,
            atomic::datatype dt,
            Ctx context = {}) -> result<void> {
    auto* fid_ep = static_cast<::fid_ep*>(ep.internal_ptr());
    void* send_desc = send_mr ? send_mr->descriptor().raw() : nullptr;
    void* recv_desc = recv_mr ? recv_mr->descriptor().raw() : nullptr;

    auto ret = ::fi_gather(fid_ep,
                           send_buf.data(),
                           send_buf.size(),
                           send_desc,
                           recv_buf.data(),
                           recv_desc,
                           grp.address().get(),
                           root_addr.get(),
                           detail::to_fi_datatype(dt),
                           0,
                           to_raw_context(context));

    if (ret != 0) {
        return make_error_result_from_fi_errno<void>(static_cast<int>(-ret));
    }

    return make_success();
}

/**
 * @brief Reduces data and scatters the result to all members.
 * @tparam Ctx The context type (must satisfy context_like).
 * @param ep The endpoint for the operation.
 * @param grp The collective group.
 * @param send_buf Data to reduce.
 * @param recv_buf Buffer for portion of reduced result.
 * @param send_mr Optional memory region for send buffer.
 * @param recv_mr Optional memory region for receive buffer.
 * @param op The reduction operation.
 * @param dt Data type of the elements.
 * @param context Optional operation context.
 * @return Success or error result.
 */
template <context_like Ctx = void*>
auto reduce_scatter(endpoint& ep,
                    group& grp,
                    std::span<const std::byte> send_buf,
                    std::span<std::byte> recv_buf,
                    memory_region* send_mr,
                    memory_region* recv_mr,
                    atomic::operation op,
                    atomic::datatype dt,
                    Ctx context = {}) -> result<void> {
    auto* fid_ep = static_cast<::fid_ep*>(ep.internal_ptr());
    void* send_desc = send_mr ? send_mr->descriptor().raw() : nullptr;
    void* recv_desc = recv_mr ? recv_mr->descriptor().raw() : nullptr;

    auto ret = ::fi_reduce_scatter(fid_ep,
                                   send_buf.data(),
                                   send_buf.size(),
                                   send_desc,
                                   recv_buf.data(),
                                   recv_desc,
                                   grp.address().get(),
                                   detail::to_fi_datatype(dt),
                                   detail::to_fi_op(op),
                                   0,
                                   to_raw_context(context));

    if (ret != 0) {
        return make_error_result_from_fi_errno<void>(static_cast<int>(-ret));
    }

    return make_success();
}

/**
 * @brief Reduces typed data and scatters the result to all members.
 * @tparam T The element type.
 * @tparam Ctx The context type (must satisfy context_like).
 * @param ep The endpoint for the operation.
 * @param grp The collective group.
 * @param send_data Data to reduce.
 * @param recv_data Buffer for portion of reduced result.
 * @param send_mr Optional memory region for send data.
 * @param recv_mr Optional memory region for receive data.
 * @param op The reduction operation.
 * @param context Optional operation context.
 * @return Success or error result.
 */
template <typename T, context_like Ctx = void*>
auto reduce_scatter(endpoint& ep,
                    group& grp,
                    std::span<const T> send_data,
                    std::span<T> recv_data,
                    memory_region* send_mr,
                    memory_region* recv_mr,
                    atomic::operation op,
                    Ctx context = {}) -> result<void> {
    return reduce_scatter(ep,
                          grp,
                          std::as_bytes(send_data),
                          std::as_writable_bytes(recv_data),
                          send_mr,
                          recv_mr,
                          op,
                          atomic::get_datatype<T>(),
                          context);
}

/**
 * @namespace loom::collective::detail
 * @brief Implementation details for collective support checking.
 */
namespace detail {

/**
 * @brief Checks if a collective operation is supported (internal).
 * @param dom The fabric domain.
 * @param op The collective operation type.
 * @param dt The data type.
 * @param aop The atomic operation.
 * @return True if supported.
 */
auto is_supported_impl(domain& dom, operation op, atomic::datatype dt, atomic::operation aop)
    -> bool;

}  // namespace detail

/**
 * @brief Checks if a collective operation is supported for a datatype.
 * @tparam Dt The atomic datatype.
 * @tparam Op The atomic operation.
 * @param dom The fabric domain to query.
 * @param op The collective operation type.
 * @return True if the operation is supported.
 */
template <atomic::datatype Dt, atomic::operation Op>
[[nodiscard]] auto is_supported(domain& dom, operation op) -> bool {
    return detail::is_supported_impl(dom, op, Dt, Op);
}

/**
 * @brief Checks if a collective operation is supported for a type.
 * @tparam T The element type (must satisfy atomic::atomic_type).
 * @tparam Op The atomic operation.
 * @param dom The fabric domain to query.
 * @param op The collective operation type.
 * @return True if the operation is supported.
 */
template <typename T, atomic::operation Op>
    requires atomic::atomic_type<T>
[[nodiscard]] auto is_supported(domain& dom, operation op) -> bool {
    return detail::is_supported_impl(dom, op, atomic::get_datatype<T>(), Op);
}

}  // namespace loom::collective
