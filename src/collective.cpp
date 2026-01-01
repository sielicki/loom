// SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only
#include "loom/core/collective.hpp"

#include <rdma/fabric.h>
#include <rdma/fi_collective.h>

#include "loom/core/domain.hpp"
#include "loom/core/endpoint.hpp"
#include "loom/core/error.hpp"

namespace loom::collective {

/// @brief Internal implementation of the collective group.
struct group::impl {
    fabric_addr coll_addr{fabric_addrs::unspecified};  ///< Collective address for the group.
    fid_mc* mc_group{nullptr};                         ///< Multicast group handle.

    /// @brief Destructor that closes the multicast group.
    ~impl() {
        if (mc_group) {
            fi_close(&mc_group->fid);
        }
    }
};

group::group(group&&) noexcept = default;
auto group::operator=(group&&) noexcept -> group& = default;
group::~group() = default;

auto group::impl_valid() const noexcept -> bool {
    return impl_ && impl_->coll_addr != fabric_addrs::unspecified;
}

auto group::address() const noexcept -> fabric_addr {
    return impl_ ? impl_->coll_addr : fabric_addrs::unspecified;
}

auto group::impl_internal_ptr() const noexcept -> void* {
    return impl_ ? impl_->mc_group : nullptr;
}

auto create_group_impl(endpoint& ep, fabric_addr coll_addr, void* context) -> result<group> {
    auto impl = std::make_unique<group::impl>();
    impl->coll_addr = coll_addr;

    auto* fid_ep = static_cast<struct fid_ep*>(ep.internal_ptr());

    auto ret = fi_join_collective(fid_ep, coll_addr.get(), nullptr, 0, &impl->mc_group, context);

    if (ret != 0) {
        return make_error_result_from_fi_errno<group>(-ret);
    }

    group grp;
    grp.impl_ = std::move(impl);
    return grp;
}

namespace {

/// @brief Converts loom collective operation to libfabric collective operation.
constexpr auto to_fi_coll(operation op) noexcept -> ::fi_collective_op {
    switch (op) {
        case operation::barrier:
            return FI_BARRIER;
        case operation::broadcast:
            return FI_BROADCAST;
        case operation::all_to_all:
            return FI_ALLTOALL;
        case operation::all_reduce:
            return FI_ALLREDUCE;
        case operation::all_gather:
            return FI_ALLGATHER;
        case operation::reduce_scatter:
            return FI_REDUCE_SCATTER;
        case operation::reduce:
            return FI_REDUCE;
        case operation::scatter:
            return FI_SCATTER;
        case operation::gather:
            return FI_GATHER;
    }
    return FI_BARRIER;
}

}  // namespace

namespace detail {

auto is_supported_impl(domain& dom, operation op, atomic::datatype dt, atomic::operation aop)
    -> bool {
    auto* fid_dom = static_cast<::fid_domain*>(dom.internal_ptr());
    if (fid_dom == nullptr) {
        return false;
    }

    ::fi_collective_attr attr{};
    attr.op = to_fi_op(aop);
    attr.datatype = to_fi_datatype(dt);

    auto ret = ::fi_query_collective(fid_dom, to_fi_coll(op), &attr, 0);
    return ret == 0;
}

}  // namespace detail

}  // namespace loom::collective
