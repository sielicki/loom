// SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only
#pragma once

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#pragma GCC diagnostic ignored "-W#pragma-messages"

#include <rdma/fabric.h>

#include "loom/core/address.hpp"
#include "loom/core/endpoint_types.hpp"
#include "loom/core/types.hpp"

/**
 * @namespace loom::detail
 * @brief Internal conversion utilities between loom types and libfabric types.
 */
namespace loom::detail {

/**
 * @brief Converts loom capabilities to libfabric capability flags.
 * @param c The loom caps to convert.
 * @return The libfabric FI_* capability flags.
 */
[[nodiscard]] constexpr auto to_fi_caps(caps c) noexcept -> std::uint64_t {
    std::uint64_t fi_caps = 0;

    if (c.has(capability::msg))
        fi_caps |= FI_MSG;
    if (c.has(capability::rma))
        fi_caps |= FI_RMA;
    if (c.has(capability::tagged))
        fi_caps |= FI_TAGGED;
    if (c.has(capability::atomic))
        fi_caps |= FI_ATOMIC;
    if (c.has(capability::read))
        fi_caps |= FI_READ;
    if (c.has(capability::write))
        fi_caps |= FI_WRITE;
    if (c.has(capability::recv))
        fi_caps |= FI_RECV;
    if (c.has(capability::send))
        fi_caps |= FI_SEND;
    if (c.has(capability::remote_read))
        fi_caps |= FI_REMOTE_READ;
    if (c.has(capability::remote_write))
        fi_caps |= FI_REMOTE_WRITE;
    if (c.has(capability::multi_recv))
        fi_caps |= FI_MULTI_RECV;
    if (c.has(capability::remote_comm))
        fi_caps |= FI_REMOTE_COMM;
    if (c.has(capability::fence))
        fi_caps |= FI_FENCE;
    if (c.has(capability::local_comm))
        fi_caps |= FI_LOCAL_COMM;
    if (c.has(capability::msg_prefix))
        fi_caps |= FI_MSG_PREFIX;

    return fi_caps;
}

/**
 * @brief Converts libfabric capability flags to loom capabilities.
 * @param fi_caps The libfabric FI_* capability flags.
 * @return The loom caps.
 */
[[nodiscard]] constexpr auto from_fi_caps(std::uint64_t fi_caps) noexcept -> caps {
    caps result{0ULL};

    if (fi_caps & FI_MSG)
        result |= capability::msg;
    if (fi_caps & FI_RMA)
        result |= capability::rma;
    if (fi_caps & FI_TAGGED)
        result |= capability::tagged;
    if (fi_caps & FI_ATOMIC)
        result |= capability::atomic;
    if (fi_caps & FI_READ)
        result |= capability::read;
    if (fi_caps & FI_WRITE)
        result |= capability::write;
    if (fi_caps & FI_RECV)
        result |= capability::recv;
    if (fi_caps & FI_SEND)
        result |= capability::send;
    if (fi_caps & FI_REMOTE_READ)
        result |= capability::remote_read;
    if (fi_caps & FI_REMOTE_WRITE)
        result |= capability::remote_write;
    if (fi_caps & FI_MULTI_RECV)
        result |= capability::multi_recv;
    if (fi_caps & FI_REMOTE_COMM)
        result |= capability::remote_comm;
    if (fi_caps & FI_FENCE)
        result |= capability::fence;
    if (fi_caps & FI_LOCAL_COMM)
        result |= capability::local_comm;
    if (fi_caps & FI_MSG_PREFIX)
        result |= capability::msg_prefix;

    return result;
}

/**
 * @brief Converts loom mode to libfabric mode flags.
 * @param m The loom mode to convert.
 * @return The libfabric FI_* mode flags.
 */
[[nodiscard]] constexpr auto to_fi_mode(mode m) noexcept -> std::uint64_t {
    std::uint64_t fi_mode = 0;

    if (m.has(mode_bits::context))
        fi_mode |= FI_CONTEXT;
    if (m.has(mode_bits::msg_prefix))
        fi_mode |= FI_MSG_PREFIX;
    if (m.has(mode_bits::rx_cq_data))
        fi_mode |= FI_RX_CQ_DATA;
    if (m.has(mode_bits::local_mr))
        fi_mode |= FI_LOCAL_MR;

    return fi_mode;
}

/**
 * @brief Converts libfabric mode flags to loom mode.
 * @param fi_mode The libfabric FI_* mode flags.
 * @return The loom mode.
 */
[[nodiscard]] constexpr auto from_fi_mode(std::uint64_t fi_mode) noexcept -> mode {
    mode result{0ULL};

    if (fi_mode & FI_CONTEXT)
        result |= mode_bits::context;
    if (fi_mode & FI_MSG_PREFIX)
        result |= mode_bits::msg_prefix;
    if (fi_mode & FI_RX_CQ_DATA)
        result |= mode_bits::rx_cq_data;
    if (fi_mode & FI_LOCAL_MR)
        result |= mode_bits::local_mr;

    return result;
}

/**
 * @brief Converts loom endpoint_type to libfabric fi_ep_type.
 * @param type The loom endpoint type.
 * @return The libfabric endpoint type.
 */
[[nodiscard]] inline auto to_fi_ep_type(const endpoint_type& type) noexcept -> ::fi_ep_type {
    return std::visit(
        [](auto tag) -> ::fi_ep_type {
            using T = decltype(tag);
            if constexpr (std::is_same_v<T, msg_endpoint_tag>) {
                return FI_EP_MSG;
            } else if constexpr (std::is_same_v<T, rdm_endpoint_tag>) {
                return FI_EP_RDM;
            } else if constexpr (std::is_same_v<T, dgram_endpoint_tag>) {
                return FI_EP_DGRAM;
            } else {
                return FI_EP_UNSPEC;
            }
        },
        type);
}

/**
 * @brief Converts libfabric fi_ep_type to loom endpoint_type.
 * @param type The libfabric endpoint type.
 * @return The loom endpoint type.
 */
[[nodiscard]] inline auto from_fi_ep_type(::fi_ep_type type) noexcept -> endpoint_type {
    switch (type) {
        case FI_EP_MSG:
            return endpoint_types::msg;
        case FI_EP_RDM:
            return endpoint_types::rdm;
        case FI_EP_DGRAM:
            return endpoint_types::dgram;
        default:
            return endpoint_types::msg;
    }
}

/**
 * @brief Converts loom address_format to libfabric address format.
 * @param fmt The loom address format.
 * @return The libfabric address format constant.
 */
[[nodiscard]] constexpr auto to_fi_addr_format(address_format fmt) noexcept -> std::uint32_t {
    switch (fmt) {
        case address_format::inet:
            return FI_SOCKADDR_IN;
        case address_format::inet6:
            return FI_SOCKADDR_IN6;
        case address_format::ib:
            return FI_SOCKADDR_IB;
        case address_format::ethernet:
            return FI_ADDR_EFA;
        case address_format::unspecified:
        default:
            return FI_FORMAT_UNSPEC;
    }
}

/**
 * @brief Converts libfabric address format to loom address_format.
 * @param fmt The libfabric address format constant.
 * @return The loom address format.
 */
[[nodiscard]] constexpr auto from_fi_addr_format(std::uint32_t fmt) noexcept -> address_format {
    switch (fmt) {
        case FI_SOCKADDR_IN:
            return address_format::inet;
        case FI_SOCKADDR_IN6:
            return address_format::inet6;
        case FI_SOCKADDR_IB:
            return address_format::ib;
        default:
            return address_format::unspecified;
    }
}

/**
 * @brief Converts loom progress_mode to libfabric fi_progress.
 * @param mode The loom progress mode.
 * @return The libfabric progress mode.
 */
[[nodiscard]] constexpr auto to_fi_progress(progress_mode mode) noexcept -> ::fi_progress {
    switch (mode) {
        case progress_mode::auto_progress:
            return FI_PROGRESS_AUTO;
        case progress_mode::manual:
            return FI_PROGRESS_MANUAL;
        default:
            return FI_PROGRESS_UNSPEC;
    }
}

/**
 * @brief Converts libfabric fi_progress to loom progress_mode.
 * @param prog The libfabric progress mode.
 * @return The loom progress mode.
 */
[[nodiscard]] constexpr auto from_fi_progress(::fi_progress prog) noexcept -> progress_mode {
    switch (prog) {
        case FI_PROGRESS_AUTO:
            return progress_mode::auto_progress;
        case FI_PROGRESS_MANUAL:
            return progress_mode::manual;
        default:
            return progress_mode::unspecified;
    }
}

/**
 * @brief Converts loom threading_mode to libfabric fi_threading.
 * @param mode The loom threading mode.
 * @return The libfabric threading mode.
 */
[[nodiscard]] constexpr auto to_fi_threading(threading_mode mode) noexcept -> ::fi_threading {
    switch (mode) {
        case threading_mode::safe:
            return FI_THREAD_SAFE;
        case threading_mode::fid:
            return FI_THREAD_FID;
        case threading_mode::domain:
            return FI_THREAD_DOMAIN;
        case threading_mode::completion:
            return FI_THREAD_COMPLETION;
        default:
            return FI_THREAD_UNSPEC;
    }
}

/**
 * @brief Converts libfabric fi_threading to loom threading_mode.
 * @param thread The libfabric threading mode.
 * @return The loom threading mode.
 */
[[nodiscard]] constexpr auto from_fi_threading(::fi_threading thread) noexcept -> threading_mode {
    switch (thread) {
        case FI_THREAD_SAFE:
            return threading_mode::safe;
        case FI_THREAD_FID:
        case FI_THREAD_ENDPOINT:
            return threading_mode::fid;
        case FI_THREAD_DOMAIN:
            return threading_mode::domain;
        case FI_THREAD_COMPLETION:
            return threading_mode::completion;
        default:
            return threading_mode::unspecified;
    }
}

/**
 * @brief Converts loom msg_order to libfabric ordering flags.
 * @param order The loom message ordering flags.
 * @return The libfabric FI_ORDER_* flags.
 */
[[nodiscard]] constexpr auto to_fi_order(msg_order order) noexcept -> std::uint64_t {
    std::uint64_t fi_order = 0;

    if (order.has(ordering::strict))
        fi_order |= FI_ORDER_STRICT;
    if (order.has(ordering::data))
        fi_order |= FI_ORDER_DATA;
    if (order.has(ordering::raw))
        fi_order |= FI_ORDER_RAW;
    if (order.has(ordering::war))
        fi_order |= FI_ORDER_WAR;
    if (order.has(ordering::waw))
        fi_order |= FI_ORDER_WAW;

    return fi_order;
}

/**
 * @brief Converts libfabric ordering flags to loom msg_order.
 * @param fi_order The libfabric FI_ORDER_* flags.
 * @return The loom message ordering flags.
 */
[[nodiscard]] constexpr auto from_fi_order(std::uint64_t fi_order) noexcept -> msg_order {
    msg_order result{0ULL};

    if (fi_order & FI_ORDER_STRICT)
        result |= ordering::strict;
    if (fi_order & FI_ORDER_DATA)
        result |= ordering::data;
    if (fi_order & FI_ORDER_RAW)
        result |= ordering::raw;
    if (fi_order & FI_ORDER_WAR)
        result |= ordering::war;
    if (fi_order & FI_ORDER_WAW)
        result |= ordering::waw;

    return result;
}

/**
 * @brief Converts loom cq_bind_flags to libfabric CQ bind flags.
 * @param flags The loom CQ bind flags.
 * @return The libfabric FI_TRANSMIT/FI_RECV/FI_SELECTIVE_COMPLETION flags.
 */
[[nodiscard]] constexpr auto to_fi_cq_bind_flags(cq_bind_flags flags) noexcept -> std::uint64_t {
    std::uint64_t fi_flags = 0;

    if (flags.has(cq_bind::transmit))
        fi_flags |= FI_TRANSMIT;
    if (flags.has(cq_bind::recv))
        fi_flags |= FI_RECV;
    if (flags.has(cq_bind::selective_completion))
        fi_flags |= FI_SELECTIVE_COMPLETION;

    return fi_flags;
}

/**
 * @brief Converts loom op_flags to libfabric operation flags.
 * @param flags The loom operation flags.
 * @return The libfabric operation flags.
 */
[[nodiscard]] constexpr auto to_fi_op_flags(op_flags flags) noexcept -> std::uint64_t {
    std::uint64_t fi_flags = 0;

    if (flags.has(op_flag::completion))
        fi_flags |= FI_COMPLETION;
    if (flags.has(op_flag::inject))
        fi_flags |= FI_INJECT;
    if (flags.has(op_flag::fence))
        fi_flags |= FI_FENCE;
    if (flags.has(op_flag::transmit_complete))
        fi_flags |= FI_TRANSMIT_COMPLETE;
    if (flags.has(op_flag::delivery_complete))
        fi_flags |= FI_DELIVERY_COMPLETE;

    return fi_flags;
}

/**
 * @brief Converts loom mr_mode to libfabric memory region mode.
 * @param mode The loom memory region mode flags.
 * @return The libfabric FI_MR_* mode flags.
 */
[[nodiscard]] constexpr auto to_fi_mr_mode(mr_mode mode) noexcept -> int {
    int fi_mode = 0;

    if (mode.has(mr_mode_flags::scalable))
        fi_mode |= FI_MR_SCALABLE;
    if (mode.has(mr_mode_flags::local))
        fi_mode |= FI_MR_LOCAL;
    if (mode.has(mr_mode_flags::virt_addr))
        fi_mode |= FI_MR_VIRT_ADDR;
    if (mode.has(mr_mode_flags::allocated))
        fi_mode |= FI_MR_ALLOCATED;
    if (mode.has(mr_mode_flags::prov_key))
        fi_mode |= FI_MR_PROV_KEY;
    if (mode.has(mr_mode_flags::raw))
        fi_mode |= FI_MR_RAW;
    if (mode.has(mr_mode_flags::hmem))
        fi_mode |= FI_MR_HMEM;
    if (mode.has(mr_mode_flags::endpoint))
        fi_mode |= FI_MR_ENDPOINT;
    if (mode.has(mr_mode_flags::collective))
        fi_mode |= FI_MR_COLLECTIVE;

    return fi_mode;
}

/**
 * @brief Converts libfabric memory region mode to loom mr_mode.
 * @param fi_mode The libfabric FI_MR_* mode flags.
 * @return The loom memory region mode flags.
 */
[[nodiscard]] constexpr auto from_fi_mr_mode(int fi_mode) noexcept -> mr_mode {
    mr_mode result{0ULL};

    if (fi_mode & FI_MR_SCALABLE)
        result |= mr_mode_flags::scalable;
    if (fi_mode & FI_MR_LOCAL)
        result |= mr_mode_flags::local;
    if (fi_mode & FI_MR_VIRT_ADDR)
        result |= mr_mode_flags::virt_addr;
    if (fi_mode & FI_MR_ALLOCATED)
        result |= mr_mode_flags::allocated;
    if (fi_mode & FI_MR_PROV_KEY)
        result |= mr_mode_flags::prov_key;
    if (fi_mode & FI_MR_RAW)
        result |= mr_mode_flags::raw;
    if (fi_mode & FI_MR_HMEM)
        result |= mr_mode_flags::hmem;
    if (fi_mode & FI_MR_ENDPOINT)
        result |= mr_mode_flags::endpoint;
    if (fi_mode & FI_MR_COLLECTIVE)
        result |= mr_mode_flags::collective;

    return result;
}

#pragma GCC diagnostic pop

}  // namespace loom::detail
