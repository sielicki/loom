// SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only
#include "loom/core/atomic.hpp"

#include <rdma/fabric.h>
#include <rdma/fi_atomic.h>

#include "loom/core/endpoint.hpp"
#include "loom/core/error.hpp"
#include "loom/core/memory.hpp"

namespace loom::atomic {

auto inject(endpoint& ep,
            operation op,
            datatype dt,
            const void* buf,
            std::size_t count,
            remote_memory remote) -> result<void> {
    auto* fid_ep = static_cast<struct fid_ep*>(ep.internal_ptr());

    auto ret = fi_inject_atomic(fid_ep,
                                buf,
                                count,
                                FI_ADDR_UNSPEC,
                                remote.addr,
                                remote.key,
                                detail::to_fi_datatype(dt),
                                detail::to_fi_op(op));

    if (ret != 0) {
        return make_error_result_from_fi_errno<void>(static_cast<int>(-ret));
    }

    return make_success();
}

auto is_valid(endpoint& ep, operation op, datatype dt, std::size_t* count_out) -> bool {
    auto* fid_ep = static_cast<struct fid_ep*>(ep.internal_ptr());

    auto ret = fi_atomicvalid(fid_ep, detail::to_fi_datatype(dt), detail::to_fi_op(op), count_out);

    return ret == 0;
}

auto is_fetch_valid(endpoint& ep, operation op, datatype dt, std::size_t* count_out) -> bool {
    auto* fid_ep = static_cast<struct fid_ep*>(ep.internal_ptr());

    auto ret =
        fi_fetch_atomicvalid(fid_ep, detail::to_fi_datatype(dt), detail::to_fi_op(op), count_out);

    return ret == 0;
}

auto is_compare_valid(endpoint& ep, operation op, datatype dt, std::size_t* count_out) -> bool {
    auto* fid_ep = static_cast<struct fid_ep*>(ep.internal_ptr());

    auto ret =
        fi_compare_atomicvalid(fid_ep, detail::to_fi_datatype(dt), detail::to_fi_op(op), count_out);

    return ret == 0;
}

}  // namespace loom::atomic
