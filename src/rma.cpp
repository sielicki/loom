// SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only
#include "loom/core/rma.hpp"

#include <rdma/fabric.h>
#include <rdma/fi_endpoint.h>
#include <rdma/fi_rma.h>

#include "loom/core/endpoint.hpp"
#include "loom/core/error.hpp"
#include "loom/core/memory.hpp"

namespace loom::rma {

auto inject_write(endpoint& ep, std::span<const std::byte> data, remote_memory remote)
    -> result<void> {
    auto* fid_ep = static_cast<struct fid_ep*>(ep.internal_ptr());

    auto ret =
        fi_inject_write(fid_ep, data.data(), data.size(), FI_ADDR_UNSPEC, remote.addr, remote.key);

    if (ret != 0) {
        return make_error_result_from_fi_errno<void>(static_cast<int>(-ret));
    }

    return make_success();
}

auto inject_write_data(endpoint& ep,
                       std::span<const std::byte> data,
                       remote_memory remote,
                       std::uint64_t immediate_data) -> result<void> {
    auto* fid_ep = static_cast<struct fid_ep*>(ep.internal_ptr());

    auto ret = fi_inject_writedata(
        fid_ep, data.data(), data.size(), immediate_data, FI_ADDR_UNSPEC, remote.addr, remote.key);

    if (ret != 0) {
        return make_error_result_from_fi_errno<void>(static_cast<int>(-ret));
    }

    return make_success();
}

auto get_max_rma_size(const endpoint&) -> std::size_t {
    return 1ULL << 30;
}

auto get_inject_size(const endpoint&) -> std::size_t {
    return 4096;
}

}  // namespace loom::rma
