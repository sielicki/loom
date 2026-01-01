// SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only
#include "loom/core/endpoint_options.hpp"

#include <rdma/fi_endpoint.h>

#include "loom/core/error.hpp"

namespace loom {

namespace {

/// @brief Translates loom endpoint option names to libfabric option names.
auto translate_optname(int optname) -> int {
    switch (optname) {
        case ep_opt::min_multi_recv:
            return FI_OPT_MIN_MULTI_RECV;
        case ep_opt::cm_data_size:
            return FI_OPT_CM_DATA_SIZE;
        case ep_opt::buffered_min:
            return FI_OPT_BUFFERED_MIN;
        case ep_opt::buffered_limit:
            return FI_OPT_BUFFERED_LIMIT;
#ifdef FI_OPT_SHARED_MEMORY_PERMITTED
        case ep_opt::shared_memory_permitted:
            return FI_OPT_SHARED_MEMORY_PERMITTED;
#endif
#ifdef FI_OPT_CUDA_API_PERMITTED
        case ep_opt::cuda_api_permitted:
            return FI_OPT_CUDA_API_PERMITTED;
#endif
#ifdef FI_OPT_EFA_EMULATED_READ
        case ep_opt::efa_emulated_read:
            return FI_OPT_EFA_EMULATED_READ;
#endif
#ifdef FI_OPT_EFA_EMULATED_WRITE
        case ep_opt::efa_emulated_write:
            return FI_OPT_EFA_EMULATED_WRITE;
#endif
#ifdef FI_OPT_EFA_WRITE_IN_ORDER_ALIGNED_128_BYTES
        case ep_opt::efa_write_in_order_aligned_128_bytes:
            return FI_OPT_EFA_WRITE_IN_ORDER_ALIGNED_128_BYTES;
#endif
        default:
            return optname;
    }
}

}  // namespace

auto set_endpoint_option(
    endpoint& ep, int level, int optname, const void* optval, std::size_t optlen) -> result<void> {
    if (!ep) {
        return make_error_result<void>(errc::invalid_argument);
    }

    auto* ep_ptr = static_cast<::fid_ep*>(ep.impl_internal_ptr());
    if (ep_ptr == nullptr) {
        return make_error_result<void>(errc::invalid_argument);
    }

    int fi_level = (level == ep_opt::level_endpoint) ? FI_OPT_ENDPOINT : level;
    int fi_optname = translate_optname(optname);

    int ret = ::fi_setopt(&ep_ptr->fid, fi_level, fi_optname, optval, optlen);
    if (ret != 0) {
        return make_error_result_from_fi_errno<void>(-ret);
    }

    return make_success();
}

auto get_endpoint_option(
    const endpoint& ep, int level, int optname, void* optval, std::size_t* optlen) -> result<void> {
    if (!ep) {
        return make_error_result<void>(errc::invalid_argument);
    }

    auto* ep_ptr = static_cast<::fid_ep*>(ep.impl_internal_ptr());
    if (ep_ptr == nullptr) {
        return make_error_result<void>(errc::invalid_argument);
    }

    int fi_level = (level == ep_opt::level_endpoint) ? FI_OPT_ENDPOINT : level;
    int fi_optname = translate_optname(optname);

    int ret = ::fi_getopt(&ep_ptr->fid, fi_level, fi_optname, optval, optlen);
    if (ret != 0) {
        return make_error_result_from_fi_errno<void>(-ret);
    }

    return make_success();
}

}  // namespace loom
