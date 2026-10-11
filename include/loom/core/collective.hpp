// SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only
#pragma once

#include <rdma/fi_collective.h>

#include <cstddef>
#include <optional>
#include <span>

#include "loom/core/address_vector.hpp"
#include "loom/core/atomic.hpp"
#include "loom/core/concepts/strong_types.hpp"
#include "loom/core/crtp/fabric_object_base.hpp"
#include "loom/core/domain.hpp"
#include "loom/core/endpoint.hpp"
#include "loom/core/memory.hpp"
#include "loom/core/result.hpp"
#include "loom/core/types.hpp"
#include "loom/detail/collective_traits.hpp"

/**
 * @namespace loom::collective
 * @brief Collective communication operations.
 */
namespace loom::collective {

/// A mutable, unqualified element type with an explicit libfabric datatype mapping.
/// This describes representation, not a provider's support for a particular operation.
template <typename T>
concept element = requires { typename detail::element_traits<T>::value_type; };

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

using atomic::detail::to_fi_datatype;

}  // namespace detail

/// Options for constructing a collective membership set and joining it.
struct group_attr {
    std::span<const std::byte> communication_key{};  ///< Provider-specific membership key; copied.
    std::uint64_t set_flags{};                       ///< Native AV-set creation flags.
    std::uint64_t join_flags{};                      ///< Native collective join flags.
    /// Defaults to the new set's address. Use fabric_addrs::unspecified for
    /// application-managed membership, or an existing group's address for a subgroup.
    std::optional<fabric_addr> parent_address{};
    /// Required for group-sized buffers when a communication key or FI_UNIVERSE
    /// supplies membership. Otherwise the explicit member list determines the size.
    std::optional<std::size_t> member_count{};
};

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
    group();

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

    /// Creates an owned AV set from ordered members and initiates an asynchronous join.
    /// The endpoint must be bound to this AV and an EQ. Every participant must
    /// supply matching membership and observe FI_JOIN_COMPLETE before transfers.
    /// The AV, endpoint, and context must outlive the join and subsequent group
    /// operations. Keep the group alive until these operations finish; it may be moved.
    template <context_like Ctx = void*>
    [[nodiscard]] static auto join(endpoint& ep,
                                   address_vector& av,
                                   std::span<const av_handle> members,
                                   const group_attr& attr = {},
                                   Ctx context = {}) -> result<group>;

    /// Gets the joined group's address after FI_JOIN_COMPLETE. Before completion,
    /// the provider may not have supplied a group handle or a usable address.
    [[nodiscard]] auto address() const noexcept -> fabric_addr;

    /// Known membership size, or nullopt for opaque membership without a supplied count.
    [[nodiscard]] auto member_count() const noexcept -> std::optional<std::size_t>;

    /// Gets the internal pointer for fabric object interface
    [[nodiscard]] auto impl_internal_ptr() const noexcept -> void*;
    /// Reports an accepted join, including one whose completion is still pending.
    [[nodiscard]] auto impl_valid() const noexcept -> bool;

private:
    /// Implementation struct (pimpl)
    struct impl;
    std::unique_ptr<impl> impl_;

    friend auto create_group_impl(endpoint& ep,
                                  address_vector& av,
                                  std::span<const av_handle> members,
                                  const group_attr& attr,
                                  void* context) -> result<group>;
};

auto create_group_impl(endpoint& ep,
                       address_vector& av,
                       std::span<const av_handle> members,
                       const group_attr& attr,
                       void* context) -> result<group>;

template <context_like Ctx>
auto group::join(endpoint& ep,
                 address_vector& av,
                 std::span<const av_handle> members,
                 const group_attr& attr,
                 Ctx context) -> result<group> {
    return create_group_impl(ep, av, members, attr, to_raw_context(context));
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
    const auto native = detail::checked_endpoint<FI_BARRIER>(ep, coll_addr);
    if (!native) {
        return std::unexpected(native.error());
    }
    auto* fid_ep = *native;

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
    const auto native = detail::checked_endpoint<FI_BARRIER>(ep, grp.address(), flags);
    if (!native) {
        return std::unexpected(native.error());
    }
    auto* fid_ep = *native;

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
    const auto native = detail::checked_endpoint<FI_BROADCAST>(ep, grp.address());
    if (!native) {
        return std::unexpected(native.error());
    }
    const auto transfer =
        detail::checked_transfer<FI_BROADCAST>(buffer.size(), 0, dt, grp.member_count());
    if (!transfer) {
        return std::unexpected(transfer.error());
    }
    auto* fid_ep = *native;
    void* desc = mr ? mr->descriptor().raw() : nullptr;

    auto ret = ::fi_broadcast(fid_ep,
                              buffer.data(),
                              transfer->count.value,
                              desc,
                              grp.address().get(),
                              root_addr.get(),
                              transfer->datatype,
                              0,
                              to_raw_context(context));

    if (ret != 0) {
        return make_error_result_from_fi_errno<void>(static_cast<int>(-ret));
    }

    return make_success();
}

/**
 * @brief Broadcasts typed data from root to all group members.
 * @tparam T The element type (must satisfy collective::element).
 * @tparam Ctx The context type (must satisfy context_like).
 * @param ep The endpoint for the operation.
 * @param grp The collective group.
 * @param data Data span to broadcast.
 * @param mr Optional memory region for the data.
 * @param root_addr Address of the root node.
 * @param context Optional operation context.
 * @return Success or error result.
 */
template <element T, context_like Ctx = void*>
auto broadcast(endpoint& ep,
               group& grp,
               std::span<T> data,
               memory_region* mr,
               fabric_addr root_addr,
               Ctx context = {}) -> result<void> {
    return broadcast(ep,
                     grp,
                     std::as_writable_bytes(data),
                     mr,
                     root_addr,
                     detail::element_traits<T>::datatype,
                     context);
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
    const auto native = detail::checked_endpoint<FI_ALLREDUCE>(ep, grp.address());
    if (!native) {
        return std::unexpected(native.error());
    }
    const auto transfer = detail::checked_transfer<FI_ALLREDUCE>(
        send_buf.size(), recv_buf.size(), dt, grp.member_count());
    if (!transfer) {
        return std::unexpected(transfer.error());
    }
    auto* fid_ep = *native;
    void* send_desc = send_mr ? send_mr->descriptor().raw() : nullptr;
    void* recv_desc = recv_mr ? recv_mr->descriptor().raw() : nullptr;

    auto ret = ::fi_allreduce(fid_ep,
                              send_buf.data(),
                              transfer->count.value,
                              send_desc,
                              recv_buf.data(),
                              recv_desc,
                              grp.address().get(),
                              transfer->datatype,
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
 * @tparam T The element type (must satisfy collective::element).
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
template <element T, context_like Ctx = void*>
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
                      detail::element_traits<T>::datatype,
                      context);
}

/**
 * @brief Performs an in-place all-reduce operation.
 * @tparam T The element type (must satisfy collective::element).
 * @tparam Ctx The context type (must satisfy context_like).
 * @param ep The endpoint for the operation.
 * @param grp The collective group.
 * @param data Data to reduce (result stored in place).
 * @param mr Optional memory region for the data.
 * @param op The reduction operation.
 * @param context Optional operation context.
 * @return Success or error result.
 */
template <element T, context_like Ctx = void*>
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
                      detail::element_traits<T>::datatype,
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
    const auto native = detail::checked_endpoint<FI_ALLGATHER>(ep, grp.address());
    if (!native) {
        return std::unexpected(native.error());
    }
    const auto transfer = detail::checked_transfer<FI_ALLGATHER>(
        send_buf.size(), recv_buf.size(), dt, grp.member_count());
    if (!transfer) {
        return std::unexpected(transfer.error());
    }
    auto* fid_ep = *native;
    void* send_desc = send_mr ? send_mr->descriptor().raw() : nullptr;
    void* recv_desc = recv_mr ? recv_mr->descriptor().raw() : nullptr;

    auto ret = ::fi_allgather(fid_ep,
                              send_buf.data(),
                              transfer->count.value,
                              send_desc,
                              recv_buf.data(),
                              recv_desc,
                              grp.address().get(),
                              transfer->datatype,
                              0,
                              to_raw_context(context));

    if (ret != 0) {
        return make_error_result_from_fi_errno<void>(static_cast<int>(-ret));
    }

    return make_success();
}

/**
 * @brief Gathers typed data from all group members to all members.
 * @tparam T The element type (must satisfy collective::element).
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
template <element T, context_like Ctx = void*>
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
                      detail::element_traits<T>::datatype,
                      context);
}

/**
 * @brief Exchanges data between all group members.
 * @tparam Ctx The context type (must satisfy context_like).
 * @param ep The endpoint for the operation.
 * @param grp The collective group.
 * @param send_buf Complete input, partitioned equally among all members.
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
    const auto native = detail::checked_endpoint<FI_ALLTOALL>(ep, grp.address());
    if (!native) {
        return std::unexpected(native.error());
    }
    const auto transfer = detail::checked_transfer<FI_ALLTOALL>(
        send_buf.size(), recv_buf.size(), dt, grp.member_count());
    if (!transfer) {
        return std::unexpected(transfer.error());
    }
    auto* fid_ep = *native;
    void* send_desc = send_mr ? send_mr->descriptor().raw() : nullptr;
    void* recv_desc = recv_mr ? recv_mr->descriptor().raw() : nullptr;

    auto ret = ::fi_alltoall(fid_ep,
                             send_buf.data(),
                             transfer->count.value,
                             send_desc,
                             recv_buf.data(),
                             recv_desc,
                             grp.address().get(),
                             transfer->datatype,
                             0,
                             to_raw_context(context));

    if (ret != 0) {
        return make_error_result_from_fi_errno<void>(static_cast<int>(-ret));
    }

    return make_success();
}

/**
 * @brief Exchanges typed data between all group members.
 * @tparam T The element type (must satisfy collective::element).
 * @tparam Ctx The context type (must satisfy context_like).
 * @param ep The endpoint for the operation.
 * @param grp The collective group.
 * @param send_data Complete input, partitioned equally among all members.
 * @param recv_data Buffer to receive data from each member.
 * @param send_mr Optional memory region for send data.
 * @param recv_mr Optional memory region for receive data.
 * @param context Optional operation context.
 * @return Success or error result.
 */
template <element T, context_like Ctx = void*>
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
                      detail::element_traits<T>::datatype,
                      context);
}

/**
 * @brief Reduces data from all group members to a root node.
 * @tparam Ctx The context type (must satisfy context_like).
 * @param ep The endpoint for the operation.
 * @param grp The collective group.
 * @param send_buf Data to reduce.
 * @param recv_buf Result buffer at root; may be empty only at non-root participants.
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
    const auto native = detail::checked_endpoint<FI_REDUCE>(ep, grp.address());
    if (!native) {
        return std::unexpected(native.error());
    }
    const auto transfer = detail::checked_transfer<FI_REDUCE>(
        send_buf.size(), recv_buf.size(), dt, grp.member_count());
    if (!transfer) {
        return std::unexpected(transfer.error());
    }
    auto* fid_ep = *native;
    void* send_desc = send_mr ? send_mr->descriptor().raw() : nullptr;
    void* recv_desc = recv_mr ? recv_mr->descriptor().raw() : nullptr;

    auto ret = ::fi_reduce(fid_ep,
                           send_buf.data(),
                           transfer->count.value,
                           send_desc,
                           recv_buf.data(),
                           recv_desc,
                           grp.address().get(),
                           root_addr.get(),
                           transfer->datatype,
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
 * @tparam T The element type (must satisfy collective::element).
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
template <element T, context_like Ctx = void*>
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
                  detail::element_traits<T>::datatype,
                  context);
}

/**
 * @brief Scatters data from root to all group members.
 * @tparam Ctx The context type (must satisfy context_like).
 * @param ep The endpoint for the operation.
 * @param grp The collective group.
 * @param send_buf Complete input at root; may be empty only at non-root participants.
 * @param recv_buf Local portion; its extent determines the count received by each member.
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
    const auto native = detail::checked_endpoint<FI_SCATTER>(ep, grp.address());
    if (!native) {
        return std::unexpected(native.error());
    }
    const auto transfer = detail::checked_transfer<FI_SCATTER>(
        send_buf.size(), recv_buf.size(), dt, grp.member_count());
    if (!transfer) {
        return std::unexpected(transfer.error());
    }
    auto* fid_ep = *native;
    void* send_desc = send_mr ? send_mr->descriptor().raw() : nullptr;
    void* recv_desc = recv_mr ? recv_mr->descriptor().raw() : nullptr;

    auto ret = ::fi_scatter(fid_ep,
                            send_buf.data(),
                            transfer->count.value,
                            send_desc,
                            recv_buf.data(),
                            recv_desc,
                            grp.address().get(),
                            root_addr.get(),
                            transfer->datatype,
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
 * @param recv_buf Full result at root; may be empty only at non-root participants.
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
    const auto native = detail::checked_endpoint<FI_GATHER>(ep, grp.address());
    if (!native) {
        return std::unexpected(native.error());
    }
    const auto transfer = detail::checked_transfer<FI_GATHER>(
        send_buf.size(), recv_buf.size(), dt, grp.member_count());
    if (!transfer) {
        return std::unexpected(transfer.error());
    }
    auto* fid_ep = *native;
    void* send_desc = send_mr ? send_mr->descriptor().raw() : nullptr;
    void* recv_desc = recv_mr ? recv_mr->descriptor().raw() : nullptr;

    auto ret = ::fi_gather(fid_ep,
                           send_buf.data(),
                           transfer->count.value,
                           send_desc,
                           recv_buf.data(),
                           recv_desc,
                           grp.address().get(),
                           root_addr.get(),
                           transfer->datatype,
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
 * @param recv_buf Local portion; its extent determines the count received by each member.
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
    const auto native = detail::checked_endpoint<FI_REDUCE_SCATTER>(ep, grp.address());
    if (!native) {
        return std::unexpected(native.error());
    }
    const auto transfer = detail::checked_transfer<FI_REDUCE_SCATTER>(
        send_buf.size(), recv_buf.size(), dt, grp.member_count());
    if (!transfer) {
        return std::unexpected(transfer.error());
    }
    auto* fid_ep = *native;
    void* send_desc = send_mr ? send_mr->descriptor().raw() : nullptr;
    void* recv_desc = recv_mr ? recv_mr->descriptor().raw() : nullptr;

    auto ret = ::fi_reduce_scatter(fid_ep,
                                   send_buf.data(),
                                   transfer->count.value,
                                   send_desc,
                                   recv_buf.data(),
                                   recv_desc,
                                   grp.address().get(),
                                   transfer->datatype,
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
 * @tparam T The element type (must satisfy collective::element).
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
template <element T, context_like Ctx = void*>
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
                          detail::element_traits<T>::datatype,
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
template <element T, atomic::operation Op>
[[nodiscard]] auto is_supported(domain& dom, operation op) -> bool {
    return detail::is_supported_impl(dom, op, detail::element_traits<T>::datatype, Op);
}

}  // namespace loom::collective
