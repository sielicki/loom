// SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only
#include "loom/core/endpoint_options.hpp"

#include <rdma/fi_endpoint.h>
#include <rdma/fi_ext.h>

#include <optional>

#include "loom/core/error.hpp"

namespace loom {

namespace {

/// @brief Translates loom endpoint option names to libfabric option names.
constexpr auto translate_optname(ep_opt::option optname) -> std::optional<int> {
    switch (optname) {
        case ep_opt::min_multi_recv:
            return FI_OPT_MIN_MULTI_RECV;
        case ep_opt::cm_data_size:
            return FI_OPT_CM_DATA_SIZE;
        case ep_opt::buffered_min:
            return FI_OPT_BUFFERED_MIN;
        case ep_opt::buffered_limit:
            return FI_OPT_BUFFERED_LIMIT;
        case ep_opt::shared_memory_permitted:
            return FI_OPT_SHARED_MEMORY_PERMITTED;
        case ep_opt::cuda_api_permitted:
            return FI_OPT_CUDA_API_PERMITTED;
        case ep_opt::efa_emulated_read:
            return FI_OPT_EFA_EMULATED_READ;
        case ep_opt::efa_emulated_write:
            return FI_OPT_EFA_EMULATED_WRITE;
        case ep_opt::efa_write_in_order_aligned_128_bytes:
            return FI_OPT_EFA_WRITE_IN_ORDER_ALIGNED_128_BYTES;
    }
    return std::nullopt;
}

}  // namespace

auto set_endpoint_option(endpoint& ep,
                         ep_opt::level level,
                         ep_opt::option optname,
                         const void* optval,
                         std::size_t optlen) -> result<void> {
    const auto native_option = translate_optname(optname);
    if (level != ep_opt::level_endpoint || !native_option)
        return make_error_result<void>(errc::invalid_argument);
    return set_native_endpoint_option(ep, FI_OPT_ENDPOINT, *native_option, optval, optlen);
}

auto get_endpoint_option(const endpoint& ep,
                         ep_opt::level level,
                         ep_opt::option optname,
                         void* optval,
                         std::size_t* optlen) -> result<void> {
    const auto native_option = translate_optname(optname);
    if (level != ep_opt::level_endpoint || !native_option)
        return make_error_result<void>(errc::invalid_argument);
    return get_native_endpoint_option(ep, FI_OPT_ENDPOINT, *native_option, optval, optlen);
}

auto set_native_endpoint_option(
    endpoint& ep, int level, int optname, const void* optval, std::size_t optlen) -> result<void> {
    if (!ep) {
        return make_error_result<void>(errc::invalid_argument);
    }

    auto* ep_ptr = static_cast<::fid_ep*>(ep.impl_internal_ptr());
    if (ep_ptr == nullptr) {
        return make_error_result<void>(errc::invalid_argument);
    }

    if (!FI_CHECK_OP(ep_ptr->ops, fi_ops_ep, setopt)) {
        return make_error_result<void>(errc::not_supported);
    }

    int ret = ::fi_setopt(&ep_ptr->fid, level, optname, optval, optlen);
    if (ret != 0) {
        return make_error_result_from_fi_errno<void>(-ret);
    }

    return make_success();
}

auto get_native_endpoint_option(
    const endpoint& ep, int level, int optname, void* optval, std::size_t* optlen) -> result<void> {
    if (!ep) {
        return make_error_result<void>(errc::invalid_argument);
    }

    auto* ep_ptr = static_cast<::fid_ep*>(ep.impl_internal_ptr());
    if (ep_ptr == nullptr) {
        return make_error_result<void>(errc::invalid_argument);
    }

    if (!FI_CHECK_OP(ep_ptr->ops, fi_ops_ep, getopt)) {
        return make_error_result<void>(errc::not_supported);
    }

    int ret = ::fi_getopt(&ep_ptr->fid, level, optname, optval, optlen);
    if (ret != 0) {
        return make_error_result_from_fi_errno<void>(-ret);
    }

    return make_success();
}

}  // namespace loom
