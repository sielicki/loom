// SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only
#pragma once

#include <rdma/fi_endpoint.h>
#include <rdma/fi_tagged.h>

#include <array>
#include <cstdint>
#include <ranges>
#include <span>

#include <sys/uio.h>

#include "loom/core/concepts/data.hpp"
#include "loom/core/concepts/strong_types.hpp"
#include "loom/core/endpoint.hpp"
#include "loom/core/error.hpp"
#include "loom/core/memory.hpp"
#include "loom/core/result.hpp"
#include "loom/core/types.hpp"

/**
 * @namespace loom::msg
 * @brief Message passing operations for fabric endpoints.
 */
namespace loom::msg {

/**
 * @struct send_flags_tag
 * @brief Tag type for send_flags strong type.
 */
struct send_flags_tag {};

/// @brief Strong type for send operation flags.
using send_flags = strong_type<std::uint64_t, send_flags_tag>;

/**
 * @namespace loom::msg::send_flag
 * @brief Predefined send flag constants.
 */
namespace send_flag {
inline constexpr send_flags none{0ULL};                 ///< No flags
inline constexpr send_flags inject{1ULL << 0};          ///< Use inject path for small messages
inline constexpr send_flags completion{1ULL << 1};      ///< Generate completion event
inline constexpr send_flags remote_cq_data{1ULL << 2};  ///< Include immediate data
inline constexpr send_flags more{1ULL << 3};            ///< More operations to follow
inline constexpr send_flags fence{1ULL << 4};           ///< Fence operation ordering
}  // namespace send_flag

/**
 * @struct recv_flags_tag
 * @brief Tag type for recv_flags strong type.
 */
struct recv_flags_tag {};

/// @brief Strong type for receive operation flags.
using recv_flags = strong_type<std::uint64_t, recv_flags_tag>;

/**
 * @namespace loom::msg::recv_flag
 * @brief Predefined receive flag constants.
 */
namespace recv_flag {
inline constexpr recv_flags none{0ULL};              ///< No flags
inline constexpr recv_flags completion{1ULL << 1};   ///< Generate completion event
inline constexpr recv_flags more{1ULL << 3};         ///< More operations to follow
inline constexpr recv_flags multi_recv{1ULL << 16};  ///< Multi-receive buffer
}  // namespace recv_flag

/**
 * @namespace loom::msg::detail
 * @brief Implementation details for messaging operations.
 */
namespace detail {

/**
 * @brief Translates loom send flags to libfabric flags.
 * @param flags The loom send flags.
 * @return The corresponding libfabric flag bitmask.
 */
[[nodiscard]] inline auto translate_send_flags(send_flags flags) noexcept -> std::uint64_t {
    std::uint64_t result = 0;
    if (flags.has(send_flag::inject)) {
        result |= FI_INJECT;
    }
    if (flags.has(send_flag::completion)) {
        result |= FI_COMPLETION;
    }
    if (flags.has(send_flag::remote_cq_data)) {
        result |= FI_REMOTE_CQ_DATA;
    }
    if (flags.has(send_flag::more)) {
        result |= FI_MORE;
    }
    if (flags.has(send_flag::fence)) {
        result |= FI_FENCE;
    }
    return result;
}

/**
 * @brief Translates loom receive flags to libfabric flags.
 * @param flags The loom receive flags.
 * @return The corresponding libfabric flag bitmask.
 */
[[nodiscard]] inline auto translate_recv_flags(recv_flags flags) noexcept -> std::uint64_t {
    std::uint64_t result = 0;
    if (flags.has(recv_flag::completion)) {
        result |= FI_COMPLETION;
    }
    if (flags.has(recv_flag::more)) {
        result |= FI_MORE;
    }
    if (flags.has(recv_flag::multi_recv)) {
        result |= FI_MULTI_RECV;
    }
    return result;
}

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
 * @struct send_message
 * @brief Message descriptor for send operations.
 * @tparam Ctx The context type for completion handling.
 */
template <context_like Ctx = void*>
struct send_message {
    std::span<const iovec> iov{};                      ///< I/O vectors containing data to send
    std::span<const mr_descriptor> desc{};             ///< Memory region descriptors
    fabric_addr dest_addr{fabric_addrs::unspecified};  ///< Destination address
    Ctx context{};                                     ///< User context for completion
    std::uint64_t data{0};                             ///< Immediate data to include
};

/**
 * @struct recv_message
 * @brief Message descriptor for receive operations.
 * @tparam Ctx The context type for completion handling.
 */
template <context_like Ctx = void*>
struct recv_message {
    std::span<const iovec> iov{};                     ///< I/O vectors for receive buffers
    std::span<const mr_descriptor> desc{};            ///< Memory region descriptors
    fabric_addr src_addr{fabric_addrs::unspecified};  ///< Source address filter
    Ctx context{};                                    ///< User context for completion
};

/**
 * @struct tagged_send_message
 * @brief Message descriptor for tagged send operations.
 * @tparam Ctx The context type for completion handling.
 */
template <context_like Ctx = void*>
struct tagged_send_message {
    std::span<const iovec> iov{};                      ///< I/O vectors containing data to send
    std::span<const mr_descriptor> desc{};             ///< Memory region descriptors
    fabric_addr dest_addr{fabric_addrs::unspecified};  ///< Destination address
    std::uint64_t tag{0};                              ///< Message tag for matching
    Ctx context{};                                     ///< User context for completion
    std::uint64_t data{0};                             ///< Immediate data to include
};

/**
 * @struct tagged_recv_message
 * @brief Message descriptor for tagged receive operations.
 * @tparam Ctx The context type for completion handling.
 */
template <context_like Ctx = void*>
struct tagged_recv_message {
    std::span<const iovec> iov{};                     ///< I/O vectors for receive buffers
    std::span<const mr_descriptor> desc{};            ///< Memory region descriptors
    fabric_addr src_addr{fabric_addrs::unspecified};  ///< Source address filter
    std::uint64_t tag{0};                             ///< Tag to match against
    std::uint64_t ignore{0};                          ///< Bits to ignore in tag matching
    Ctx context{};                                    ///< User context for completion
};

/// @brief Maximum number of I/O vectors per message.
inline constexpr std::size_t max_iov_count = 16;

/**
 * @brief Sends a message using scatter/gather I/O.
 * @tparam Ctx The context type for completion handling.
 * @param ep The endpoint for the operation.
 * @param msg The message descriptor.
 * @param flags Optional send flags.
 * @return Success or error result.
 */
template <context_like Ctx>
[[nodiscard]] auto sendmsg(endpoint& ep,
                           const send_message<Ctx>& msg,
                           send_flags flags = send_flag::none) -> result<void> {
    auto* fid_ep = static_cast<::fid_ep*>(ep.impl_internal_ptr());
    if (fid_ep == nullptr) {
        return make_error_result<void>(errc::invalid_argument);
    }

    std::array<void*, max_iov_count> raw_desc{};

    ::fi_msg fi_m{};
    fi_m.msg_iov = msg.iov.data();
    fi_m.desc = detail::descriptors_to_raw(msg.desc, raw_desc);
    fi_m.iov_count = msg.iov.size();
    fi_m.addr = msg.dest_addr.get();
    fi_m.context = to_raw_context(msg.context);
    fi_m.data = msg.data;

    auto ret = ::fi_sendmsg(fid_ep, &fi_m, detail::translate_send_flags(flags));
    if (ret != 0) {
        return make_error_result_from_fi_errno<void>(static_cast<int>(-ret));
    }

    return make_success();
}

/**
 * @brief Receives a message using scatter/gather I/O.
 * @tparam Ctx The context type for completion handling.
 * @param ep The endpoint for the operation.
 * @param msg The message descriptor.
 * @param flags Optional receive flags.
 * @return Success or error result.
 */
template <context_like Ctx>
[[nodiscard]] auto recvmsg(endpoint& ep,
                           const recv_message<Ctx>& msg,
                           recv_flags flags = recv_flag::none) -> result<void> {
    auto* fid_ep = static_cast<::fid_ep*>(ep.impl_internal_ptr());
    if (fid_ep == nullptr) {
        return make_error_result<void>(errc::invalid_argument);
    }

    std::array<void*, max_iov_count> raw_desc{};

    ::fi_msg fi_m{};
    fi_m.msg_iov = msg.iov.data();
    fi_m.desc = detail::descriptors_to_raw(msg.desc, raw_desc);
    fi_m.iov_count = msg.iov.size();
    fi_m.addr = msg.src_addr.get();
    fi_m.context = to_raw_context(msg.context);
    fi_m.data = 0;

    auto ret = ::fi_recvmsg(fid_ep, &fi_m, detail::translate_recv_flags(flags));
    if (ret != 0) {
        return make_error_result_from_fi_errno<void>(static_cast<int>(-ret));
    }

    return make_success();
}

/**
 * @brief Sends a tagged message using scatter/gather I/O.
 * @tparam Ctx The context type for completion handling.
 * @param ep The endpoint for the operation.
 * @param msg The tagged message descriptor.
 * @param flags Optional send flags.
 * @return Success or error result.
 */
template <context_like Ctx>
[[nodiscard]] auto tagged_sendmsg(endpoint& ep,
                                  const tagged_send_message<Ctx>& msg,
                                  send_flags flags = send_flag::none) -> result<void> {
    auto* fid_ep = static_cast<::fid_ep*>(ep.impl_internal_ptr());
    if (fid_ep == nullptr) {
        return make_error_result<void>(errc::invalid_argument);
    }

    std::array<void*, max_iov_count> raw_desc{};

    ::fi_msg_tagged fi_m{};
    fi_m.msg_iov = msg.iov.data();
    fi_m.desc = detail::descriptors_to_raw(msg.desc, raw_desc);
    fi_m.iov_count = msg.iov.size();
    fi_m.addr = msg.dest_addr.get();
    fi_m.tag = msg.tag;
    fi_m.ignore = 0;
    fi_m.context = to_raw_context(msg.context);
    fi_m.data = msg.data;

    auto ret = ::fi_tsendmsg(fid_ep, &fi_m, detail::translate_send_flags(flags));
    if (ret != 0) {
        return make_error_result_from_fi_errno<void>(static_cast<int>(-ret));
    }

    return make_success();
}

/**
 * @brief Receives a tagged message using scatter/gather I/O.
 * @tparam Ctx The context type for completion handling.
 * @param ep The endpoint for the operation.
 * @param msg The tagged message descriptor.
 * @param flags Optional receive flags.
 * @return Success or error result.
 */
template <context_like Ctx>
[[nodiscard]] auto tagged_recvmsg(endpoint& ep,
                                  const tagged_recv_message<Ctx>& msg,
                                  recv_flags flags = recv_flag::none) -> result<void> {
    auto* fid_ep = static_cast<::fid_ep*>(ep.impl_internal_ptr());
    if (fid_ep == nullptr) {
        return make_error_result<void>(errc::invalid_argument);
    }

    std::array<void*, max_iov_count> raw_desc{};

    ::fi_msg_tagged fi_m{};
    fi_m.msg_iov = msg.iov.data();
    fi_m.desc = detail::descriptors_to_raw(msg.desc, raw_desc);
    fi_m.iov_count = msg.iov.size();
    fi_m.addr = msg.src_addr.get();
    fi_m.tag = msg.tag;
    fi_m.ignore = msg.ignore;
    fi_m.context = to_raw_context(msg.context);
    fi_m.data = 0;

    auto ret = ::fi_trecvmsg(fid_ep, &fi_m, detail::translate_recv_flags(flags));
    if (ret != 0) {
        return make_error_result_from_fi_errno<void>(static_cast<int>(-ret));
    }

    return make_success();
}

/**
 * @brief Sends data to a specific destination address.
 * @tparam Ctx The context type for completion handling.
 * @param ep The endpoint for the operation.
 * @param data Data to send.
 * @param dest_addr Destination fabric address.
 * @param ctx Optional user context.
 * @return Success or error result.
 */
template <context_like Ctx = void*>
[[nodiscard]] auto
send_to(endpoint& ep, std::span<const std::byte> data, fabric_addr dest_addr, Ctx ctx = {})
    -> result<void> {
    auto* fid_ep = static_cast<::fid_ep*>(ep.impl_internal_ptr());
    if (fid_ep == nullptr) {
        return make_error_result<void>(errc::invalid_argument);
    }

    auto ret =
        ::fi_send(fid_ep, data.data(), data.size(), nullptr, dest_addr.get(), to_raw_context(ctx));
    if (ret != 0) {
        return make_error_result_from_fi_errno<void>(static_cast<int>(-ret));
    }

    return make_success();
}

/**
 * @brief Receives data from a specific source address.
 * @tparam Ctx The context type for completion handling.
 * @param ep The endpoint for the operation.
 * @param buffer Buffer to receive into.
 * @param src_addr Source fabric address filter.
 * @param ctx Optional user context.
 * @return Success or error result.
 */
template <context_like Ctx = void*>
[[nodiscard]] auto
recv_from(endpoint& ep, std::span<std::byte> buffer, fabric_addr src_addr, Ctx ctx = {})
    -> result<void> {
    auto* fid_ep = static_cast<::fid_ep*>(ep.impl_internal_ptr());
    if (fid_ep == nullptr) {
        return make_error_result<void>(errc::invalid_argument);
    }

    auto ret = ::fi_recv(
        fid_ep, buffer.data(), buffer.size(), nullptr, src_addr.get(), to_raw_context(ctx));
    if (ret != 0) {
        return make_error_result_from_fi_errno<void>(static_cast<int>(-ret));
    }

    return make_success();
}

/**
 * @brief Sends data with immediate data to a specific destination.
 * @tparam Ctx The context type for completion handling.
 * @param ep The endpoint for the operation.
 * @param data Data to send.
 * @param immediate_data Immediate data to include with the message.
 * @param dest_addr Destination fabric address.
 * @param ctx Optional user context.
 * @return Success or error result.
 */
template <context_like Ctx = void*>
[[nodiscard]] auto send_data_to(endpoint& ep,
                                std::span<const std::byte> data,
                                std::uint64_t immediate_data,
                                fabric_addr dest_addr,
                                Ctx ctx = {}) -> result<void> {
    auto* fid_ep = static_cast<::fid_ep*>(ep.impl_internal_ptr());
    if (fid_ep == nullptr) {
        return make_error_result<void>(errc::invalid_argument);
    }

    auto ret = ::fi_senddata(fid_ep,
                             data.data(),
                             data.size(),
                             nullptr,
                             immediate_data,
                             dest_addr.get(),
                             to_raw_context(ctx));
    if (ret != 0) {
        return make_error_result_from_fi_errno<void>(static_cast<int>(-ret));
    }

    return make_success();
}

/**
 * @brief Posts a multi-receive buffer for multiple incoming messages.
 * @tparam Ctx The context type for completion handling.
 * @param ep The endpoint for the operation.
 * @param buffer Large buffer to receive multiple messages.
 * @param ctx Optional user context.
 * @return Success or error result.
 */
template <context_like Ctx = void*>
[[nodiscard]] auto post_multi_recv(endpoint& ep, std::span<std::byte> buffer, Ctx ctx = {})
    -> result<void> {
    auto* fid_ep = static_cast<::fid_ep*>(ep.impl_internal_ptr());
    if (fid_ep == nullptr) {
        return make_error_result<void>(errc::invalid_argument);
    }

    ::iovec iov{};
    iov.iov_base = buffer.data();
    iov.iov_len = buffer.size();

    ::fi_msg fi_m{};
    fi_m.msg_iov = &iov;
    fi_m.desc = nullptr;
    fi_m.iov_count = 1;
    fi_m.addr = FI_ADDR_UNSPEC;
    fi_m.context = to_raw_context(ctx);
    fi_m.data = 0;

    auto ret = ::fi_recvmsg(fid_ep, &fi_m, FI_MULTI_RECV);
    if (ret != 0) {
        return make_error_result_from_fi_errno<void>(static_cast<int>(-ret));
    }

    return make_success();
}

}  // namespace loom::msg

/**
 * @namespace loom
 * @brief Core loom namespace with messaging convenience functions.
 */
namespace loom {

/**
 * @brief Sends a tagged message with operation flags.
 * @tparam Buf Contiguous byte range type.
 * @tparam Ctx The context type for completion handling.
 * @param ep The endpoint for the operation.
 * @param data Data to send.
 * @param dest Destination fabric address.
 * @param tag Message tag for matching.
 * @param desc Memory region descriptor.
 * @param flags Operation flags.
 * @param ctx Optional user context.
 * @return Success or error result.
 */
template <contiguous_byte_range Buf, context_like Ctx = void*>
[[nodiscard]] auto tagged_send(endpoint& ep,
                               Buf&& data,
                               fabric_addr dest,
                               std::uint64_t tag,
                               mr_descriptor desc,
                               op_flags flags,
                               Ctx ctx = {}) -> result<void> {
    auto* fid_ep = static_cast<::fid_ep*>(ep.impl_internal_ptr());
    if (fid_ep == nullptr) {
        return make_error_result<void>(errc::invalid_argument);
    }

    ::iovec iov{};
    iov.iov_base = const_cast<void*>(static_cast<const void*>(std::data(data)));
    iov.iov_len = std::size(data);

    void* raw_desc = desc ? desc.raw() : nullptr;

    ::fi_msg_tagged fi_m{};
    fi_m.msg_iov = &iov;
    fi_m.desc = raw_desc ? &raw_desc : nullptr;
    fi_m.iov_count = 1;
    fi_m.addr = dest.get();
    fi_m.tag = tag;
    fi_m.ignore = 0;
    fi_m.context = to_raw_context(ctx);
    fi_m.data = 0;

    auto fi_flags = msg::detail::translate_send_flags(msg::send_flag::none);
    if (flags.has(op_flag::completion)) {
        fi_flags |= FI_COMPLETION;
    }
    if (flags.has(op_flag::fence)) {
        fi_flags |= FI_FENCE;
    }
    if (flags.has(op_flag::transmit_complete)) {
        fi_flags |= FI_TRANSMIT_COMPLETE;
    }
    if (flags.has(op_flag::delivery_complete)) {
        fi_flags |= FI_DELIVERY_COMPLETE;
    }

    auto ret = ::fi_tsendmsg(fid_ep, &fi_m, fi_flags);
    if (ret != 0) {
        return make_error_result_from_fi_errno<void>(static_cast<int>(-ret));
    }

    return make_success();
}

/**
 * @brief Sends a tagged message with simple interface.
 * @tparam Buf Contiguous byte range type.
 * @tparam Ctx The context type for completion handling.
 * @param ep The endpoint for the operation.
 * @param data Data to send.
 * @param dest Destination fabric address.
 * @param tag Message tag for matching.
 * @param desc Optional memory region descriptor.
 * @param ctx Optional user context.
 * @return Success or error result.
 */
template <contiguous_byte_range Buf, context_like Ctx = void*>
[[nodiscard]] auto tagged_send(endpoint& ep,
                               Buf&& data,
                               fabric_addr dest,
                               std::uint64_t tag,
                               mr_descriptor desc = {},
                               Ctx ctx = {}) -> result<void> {
    auto* fid_ep = static_cast<::fid_ep*>(ep.impl_internal_ptr());
    if (fid_ep == nullptr) {
        return make_error_result<void>(errc::invalid_argument);
    }

    void* raw_desc = desc ? desc.raw() : nullptr;
    auto ret = ::fi_tsend(
        fid_ep, std::data(data), std::size(data), raw_desc, dest.get(), tag, to_raw_context(ctx));
    if (ret != 0) {
        return make_error_result_from_fi_errno<void>(static_cast<int>(-ret));
    }

    return make_success();
}

/**
 * @brief Receives a tagged message with operation flags.
 * @tparam Buf Mutable byte range type.
 * @tparam Ctx The context type for completion handling.
 * @param ep The endpoint for the operation.
 * @param buffer Buffer to receive into.
 * @param src Source fabric address filter.
 * @param tag Tag to match against.
 * @param ignore Bits to ignore in tag matching.
 * @param desc Memory region descriptor.
 * @param flags Operation flags.
 * @param ctx Optional user context.
 * @return Success or error result.
 */
template <mutable_byte_range Buf, context_like Ctx = void*>
[[nodiscard]] auto tagged_recv(endpoint& ep,
                               Buf&& buffer,
                               fabric_addr src,
                               std::uint64_t tag,
                               std::uint64_t ignore,
                               mr_descriptor desc,
                               op_flags flags,
                               Ctx ctx = {}) -> result<void> {
    auto* fid_ep = static_cast<::fid_ep*>(ep.impl_internal_ptr());
    if (fid_ep == nullptr) {
        return make_error_result<void>(errc::invalid_argument);
    }

    ::iovec iov{};
    iov.iov_base = std::data(buffer);
    iov.iov_len = std::size(buffer);

    void* raw_desc = desc ? desc.raw() : nullptr;

    ::fi_msg_tagged fi_m{};
    fi_m.msg_iov = &iov;
    fi_m.desc = raw_desc ? &raw_desc : nullptr;
    fi_m.iov_count = 1;
    fi_m.addr = src.get();
    fi_m.tag = tag;
    fi_m.ignore = ignore;
    fi_m.context = to_raw_context(ctx);
    fi_m.data = 0;

    auto fi_flags = msg::detail::translate_recv_flags(msg::recv_flag::none);
    if (flags.has(op_flag::completion)) {
        fi_flags |= FI_COMPLETION;
    }

    auto ret = ::fi_trecvmsg(fid_ep, &fi_m, fi_flags);
    if (ret != 0) {
        return make_error_result_from_fi_errno<void>(static_cast<int>(-ret));
    }

    return make_success();
}

/**
 * @brief Receives a tagged message with simple interface.
 * @tparam Buf Mutable byte range type.
 * @tparam Ctx The context type for completion handling.
 * @param ep The endpoint for the operation.
 * @param buffer Buffer to receive into.
 * @param src Source fabric address filter.
 * @param tag Tag to match against.
 * @param ignore Bits to ignore in tag matching.
 * @param desc Optional memory region descriptor.
 * @param ctx Optional user context.
 * @return Success or error result.
 */
template <mutable_byte_range Buf, context_like Ctx = void*>
[[nodiscard]] auto tagged_recv(endpoint& ep,
                               Buf&& buffer,
                               fabric_addr src,
                               std::uint64_t tag,
                               std::uint64_t ignore = 0,
                               mr_descriptor desc = {},
                               Ctx ctx = {}) -> result<void> {
    auto* fid_ep = static_cast<::fid_ep*>(ep.impl_internal_ptr());
    if (fid_ep == nullptr) {
        return make_error_result<void>(errc::invalid_argument);
    }

    void* raw_desc = desc ? desc.raw() : nullptr;
    auto ret = ::fi_trecv(fid_ep,
                          std::data(buffer),
                          std::size(buffer),
                          raw_desc,
                          src.get(),
                          tag,
                          ignore,
                          to_raw_context(ctx));
    if (ret != 0) {
        return make_error_result_from_fi_errno<void>(static_cast<int>(-ret));
    }

    return make_success();
}

/**
 * @brief Sends a message with operation flags.
 * @tparam Buf Contiguous byte range type.
 * @tparam Ctx The context type for completion handling.
 * @param ep The endpoint for the operation.
 * @param data Data to send.
 * @param dest Destination fabric address.
 * @param desc Memory region descriptor.
 * @param flags Operation flags.
 * @param ctx Optional user context.
 * @return Success or error result.
 */
template <contiguous_byte_range Buf, context_like Ctx = void*>
[[nodiscard]] auto
send(endpoint& ep, Buf&& data, fabric_addr dest, mr_descriptor desc, op_flags flags, Ctx ctx = {})
    -> result<void> {
    auto* fid_ep = static_cast<::fid_ep*>(ep.impl_internal_ptr());
    if (fid_ep == nullptr) {
        return make_error_result<void>(errc::invalid_argument);
    }

    ::iovec iov{};
    iov.iov_base = const_cast<void*>(static_cast<const void*>(std::data(data)));
    iov.iov_len = std::size(data);

    void* raw_desc = desc ? desc.raw() : nullptr;

    ::fi_msg fi_m{};
    fi_m.msg_iov = &iov;
    fi_m.desc = raw_desc ? &raw_desc : nullptr;
    fi_m.iov_count = 1;
    fi_m.addr = dest.get();
    fi_m.context = to_raw_context(ctx);
    fi_m.data = 0;

    auto fi_flags = msg::detail::translate_send_flags(msg::send_flag::none);
    if (flags.has(op_flag::completion)) {
        fi_flags |= FI_COMPLETION;
    }
    if (flags.has(op_flag::fence)) {
        fi_flags |= FI_FENCE;
    }
    if (flags.has(op_flag::transmit_complete)) {
        fi_flags |= FI_TRANSMIT_COMPLETE;
    }
    if (flags.has(op_flag::delivery_complete)) {
        fi_flags |= FI_DELIVERY_COMPLETE;
    }

    auto ret = ::fi_sendmsg(fid_ep, &fi_m, fi_flags);
    if (ret != 0) {
        return make_error_result_from_fi_errno<void>(static_cast<int>(-ret));
    }

    return make_success();
}

/**
 * @brief Sends a message with simple interface.
 * @tparam Buf Contiguous byte range type.
 * @tparam Ctx The context type for completion handling.
 * @param ep The endpoint for the operation.
 * @param data Data to send.
 * @param dest Destination fabric address.
 * @param desc Optional memory region descriptor.
 * @param ctx Optional user context.
 * @return Success or error result.
 */
template <contiguous_byte_range Buf, context_like Ctx = void*>
[[nodiscard]] auto
send(endpoint& ep, Buf&& data, fabric_addr dest, mr_descriptor desc = {}, Ctx ctx = {})
    -> result<void> {
    auto* fid_ep = static_cast<::fid_ep*>(ep.impl_internal_ptr());
    if (fid_ep == nullptr) {
        return make_error_result<void>(errc::invalid_argument);
    }

    void* raw_desc = desc ? desc.raw() : nullptr;
    auto ret = ::fi_send(
        fid_ep, std::data(data), std::size(data), raw_desc, dest.get(), to_raw_context(ctx));
    if (ret != 0) {
        return make_error_result_from_fi_errno<void>(static_cast<int>(-ret));
    }

    return make_success();
}

/**
 * @brief Receives a message with operation flags.
 * @tparam Buf Mutable byte range type.
 * @tparam Ctx The context type for completion handling.
 * @param ep The endpoint for the operation.
 * @param buffer Buffer to receive into.
 * @param src Source fabric address filter.
 * @param desc Memory region descriptor.
 * @param flags Operation flags.
 * @param ctx Optional user context.
 * @return Success or error result.
 */
template <mutable_byte_range Buf, context_like Ctx = void*>
[[nodiscard]] auto
recv(endpoint& ep, Buf&& buffer, fabric_addr src, mr_descriptor desc, op_flags flags, Ctx ctx = {})
    -> result<void> {
    auto* fid_ep = static_cast<::fid_ep*>(ep.impl_internal_ptr());
    if (fid_ep == nullptr) {
        return make_error_result<void>(errc::invalid_argument);
    }

    ::iovec iov{};
    iov.iov_base = std::data(buffer);
    iov.iov_len = std::size(buffer);

    void* raw_desc = desc ? desc.raw() : nullptr;

    ::fi_msg fi_m{};
    fi_m.msg_iov = &iov;
    fi_m.desc = raw_desc ? &raw_desc : nullptr;
    fi_m.iov_count = 1;
    fi_m.addr = src.get();
    fi_m.context = to_raw_context(ctx);
    fi_m.data = 0;

    auto fi_flags = msg::detail::translate_recv_flags(msg::recv_flag::none);
    if (flags.has(op_flag::completion)) {
        fi_flags |= FI_COMPLETION;
    }

    auto ret = ::fi_recvmsg(fid_ep, &fi_m, fi_flags);
    if (ret != 0) {
        return make_error_result_from_fi_errno<void>(static_cast<int>(-ret));
    }

    return make_success();
}

/**
 * @brief Receives a message with simple interface.
 * @tparam Buf Mutable byte range type.
 * @tparam Ctx The context type for completion handling.
 * @param ep The endpoint for the operation.
 * @param buffer Buffer to receive into.
 * @param src Source fabric address filter.
 * @param desc Optional memory region descriptor.
 * @param ctx Optional user context.
 * @return Success or error result.
 */
template <mutable_byte_range Buf, context_like Ctx = void*>
[[nodiscard]] auto
recv(endpoint& ep, Buf&& buffer, fabric_addr src, mr_descriptor desc = {}, Ctx ctx = {})
    -> result<void> {
    auto* fid_ep = static_cast<::fid_ep*>(ep.impl_internal_ptr());
    if (fid_ep == nullptr) {
        return make_error_result<void>(errc::invalid_argument);
    }

    void* raw_desc = desc ? desc.raw() : nullptr;
    auto ret = ::fi_recv(
        fid_ep, std::data(buffer), std::size(buffer), raw_desc, src.get(), to_raw_context(ctx));
    if (ret != 0) {
        return make_error_result_from_fi_errno<void>(static_cast<int>(-ret));
    }

    return make_success();
}

/**
 * @brief Sends a tagged message using scatter/gather I/O vectors.
 * @tparam Iov I/O vector range type.
 * @tparam Desc Descriptor range type.
 * @tparam Ctx The context type for completion handling.
 * @param ep The endpoint for the operation.
 * @param iov I/O vectors containing data to send.
 * @param descriptors Memory region descriptors.
 * @param dest Destination fabric address.
 * @param tag Message tag for matching.
 * @param ctx Optional user context.
 * @return Success or error result.
 */
template <iov_range Iov, descriptor_range Desc, context_like Ctx = void*>
[[nodiscard]] auto tagged_sendv(
    endpoint& ep, Iov&& iov, Desc&& descriptors, fabric_addr dest, std::uint64_t tag, Ctx ctx = {})
    -> result<void> {
    auto* fid_ep = static_cast<::fid_ep*>(ep.impl_internal_ptr());
    if (fid_ep == nullptr) {
        return make_error_result<void>(errc::invalid_argument);
    }

    std::array<void*, msg::max_iov_count> raw_desc{};
    auto desc_span = std::span{std::ranges::data(descriptors), std::ranges::size(descriptors)};
    void** desc_ptr = msg::detail::descriptors_to_raw(desc_span, raw_desc);

    auto iov_data = std::ranges::data(iov);
    auto iov_size = std::ranges::size(iov);

    auto ret =
        ::fi_tsendv(fid_ep, iov_data, desc_ptr, iov_size, dest.get(), tag, to_raw_context(ctx));
    if (ret != 0) {
        return make_error_result_from_fi_errno<void>(static_cast<int>(-ret));
    }

    return make_success();
}

/**
 * @brief Receives a tagged message using scatter/gather I/O vectors.
 * @tparam Iov I/O vector range type.
 * @tparam Desc Descriptor range type.
 * @tparam Ctx The context type for completion handling.
 * @param ep The endpoint for the operation.
 * @param iov I/O vectors for receive buffers.
 * @param descriptors Memory region descriptors.
 * @param src Source fabric address filter.
 * @param tag Tag to match against.
 * @param ignore Bits to ignore in tag matching.
 * @param ctx Optional user context.
 * @return Success or error result.
 */
template <iov_range Iov, descriptor_range Desc, context_like Ctx = void*>
[[nodiscard]] auto tagged_recvv(endpoint& ep,
                                Iov&& iov,
                                Desc&& descriptors,
                                fabric_addr src,
                                std::uint64_t tag,
                                std::uint64_t ignore = 0,
                                Ctx ctx = {}) -> result<void> {
    auto* fid_ep = static_cast<::fid_ep*>(ep.impl_internal_ptr());
    if (fid_ep == nullptr) {
        return make_error_result<void>(errc::invalid_argument);
    }

    std::array<void*, msg::max_iov_count> raw_desc{};
    auto desc_span = std::span{std::ranges::data(descriptors), std::ranges::size(descriptors)};
    void** desc_ptr = msg::detail::descriptors_to_raw(desc_span, raw_desc);

    auto iov_data = std::ranges::data(iov);
    auto iov_size = std::ranges::size(iov);

    auto ret = ::fi_trecvv(
        fid_ep, iov_data, desc_ptr, iov_size, src.get(), tag, ignore, to_raw_context(ctx));
    if (ret != 0) {
        return make_error_result_from_fi_errno<void>(static_cast<int>(-ret));
    }

    return make_success();
}

/**
 * @brief Sends a message using scatter/gather I/O vectors.
 * @tparam Iov I/O vector range type.
 * @tparam Desc Descriptor range type.
 * @tparam Ctx The context type for completion handling.
 * @param ep The endpoint for the operation.
 * @param iov I/O vectors containing data to send.
 * @param descriptors Memory region descriptors.
 * @param dest Destination fabric address.
 * @param ctx Optional user context.
 * @return Success or error result.
 */
template <iov_range Iov, descriptor_range Desc, context_like Ctx = void*>
[[nodiscard]] auto
sendv(endpoint& ep, Iov&& iov, Desc&& descriptors, fabric_addr dest, Ctx ctx = {}) -> result<void> {
    auto* fid_ep = static_cast<::fid_ep*>(ep.impl_internal_ptr());
    if (fid_ep == nullptr) {
        return make_error_result<void>(errc::invalid_argument);
    }

    std::array<void*, msg::max_iov_count> raw_desc{};
    auto desc_span = std::span{std::ranges::data(descriptors), std::ranges::size(descriptors)};
    void** desc_ptr = msg::detail::descriptors_to_raw(desc_span, raw_desc);

    auto iov_data = std::ranges::data(iov);
    auto iov_size = std::ranges::size(iov);

    auto ret = ::fi_sendv(fid_ep, iov_data, desc_ptr, iov_size, dest.get(), to_raw_context(ctx));
    if (ret != 0) {
        return make_error_result_from_fi_errno<void>(static_cast<int>(-ret));
    }

    return make_success();
}

/**
 * @brief Receives a message using scatter/gather I/O vectors.
 * @tparam Iov I/O vector range type.
 * @tparam Desc Descriptor range type.
 * @tparam Ctx The context type for completion handling.
 * @param ep The endpoint for the operation.
 * @param iov I/O vectors for receive buffers.
 * @param descriptors Memory region descriptors.
 * @param src Source fabric address filter.
 * @param ctx Optional user context.
 * @return Success or error result.
 */
template <iov_range Iov, descriptor_range Desc, context_like Ctx = void*>
[[nodiscard]] auto recvv(endpoint& ep, Iov&& iov, Desc&& descriptors, fabric_addr src, Ctx ctx = {})
    -> result<void> {
    auto* fid_ep = static_cast<::fid_ep*>(ep.impl_internal_ptr());
    if (fid_ep == nullptr) {
        return make_error_result<void>(errc::invalid_argument);
    }

    std::array<void*, msg::max_iov_count> raw_desc{};
    auto desc_span = std::span{std::ranges::data(descriptors), std::ranges::size(descriptors)};
    void** desc_ptr = msg::detail::descriptors_to_raw(desc_span, raw_desc);

    auto iov_data = std::ranges::data(iov);
    auto iov_size = std::ranges::size(iov);

    auto ret = ::fi_recvv(fid_ep, iov_data, desc_ptr, iov_size, src.get(), to_raw_context(ctx));
    if (ret != 0) {
        return make_error_result_from_fi_errno<void>(static_cast<int>(-ret));
    }

    return make_success();
}

/**
 * @brief Sends a message with immediate data.
 * @tparam Buf Contiguous byte range type.
 * @tparam Ctx The context type for completion handling.
 * @param ep The endpoint for the operation.
 * @param data Data to send.
 * @param dest Destination fabric address.
 * @param immediate_data Immediate data to include with the message.
 * @param desc Optional memory region descriptor.
 * @param ctx Optional user context.
 * @return Success or error result.
 */
template <contiguous_byte_range Buf, context_like Ctx = void*>
[[nodiscard]] auto send_data(endpoint& ep,
                             Buf&& data,
                             fabric_addr dest,
                             std::uint64_t immediate_data,
                             mr_descriptor desc = {},
                             Ctx ctx = {}) -> result<void> {
    auto* fid_ep = static_cast<::fid_ep*>(ep.impl_internal_ptr());
    if (fid_ep == nullptr) {
        return make_error_result<void>(errc::invalid_argument);
    }

    void* raw_desc = desc ? desc.raw() : nullptr;
    auto ret = ::fi_senddata(fid_ep,
                             std::data(data),
                             std::size(data),
                             raw_desc,
                             immediate_data,
                             dest.get(),
                             to_raw_context(ctx));
    if (ret != 0) {
        return make_error_result_from_fi_errno<void>(static_cast<int>(-ret));
    }

    return make_success();
}

/**
 * @brief Sends a tagged message with immediate data.
 * @tparam Buf Contiguous byte range type.
 * @tparam Ctx The context type for completion handling.
 * @param ep The endpoint for the operation.
 * @param data Data to send.
 * @param dest Destination fabric address.
 * @param tag Message tag for matching.
 * @param immediate_data Immediate data to include with the message.
 * @param desc Optional memory region descriptor.
 * @param ctx Optional user context.
 * @return Success or error result.
 */
template <contiguous_byte_range Buf, context_like Ctx = void*>
[[nodiscard]] auto tagged_send_data(endpoint& ep,
                                    Buf&& data,
                                    fabric_addr dest,
                                    std::uint64_t tag,
                                    std::uint64_t immediate_data,
                                    mr_descriptor desc = {},
                                    Ctx ctx = {}) -> result<void> {
    auto* fid_ep = static_cast<::fid_ep*>(ep.impl_internal_ptr());
    if (fid_ep == nullptr) {
        return make_error_result<void>(errc::invalid_argument);
    }

    void* raw_desc = desc ? desc.raw() : nullptr;
    auto ret = ::fi_tsenddata(fid_ep,
                              std::data(data),
                              std::size(data),
                              raw_desc,
                              immediate_data,
                              dest.get(),
                              tag,
                              to_raw_context(ctx));
    if (ret != 0) {
        return make_error_result_from_fi_errno<void>(static_cast<int>(-ret));
    }

    return make_success();
}

}  // namespace loom
