// SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only
#include "loom/core/collective.hpp"

#include <rdma/fabric.h>
#include <rdma/fi_collective.h>

#include "loom/core/domain.hpp"
#include "loom/core/endpoint.hpp"
#include "loom/core/error.hpp"
#include "loom/detail/impl/collective_impl.hpp"

namespace loom::collective {

group::group() = default;

group::group(group&&) noexcept = default;
auto group::operator=(group&&) noexcept -> group& = default;
group::~group() = default;

auto group::impl_valid() const noexcept -> bool {
    return impl_ != nullptr;
}

auto group::address() const noexcept -> fabric_addr {
    return impl_ && impl_->mc_group ? fabric_addr{::fi_mc_addr(impl_->mc_group)}
                                    : fabric_addrs::unspecified;
}

auto group::member_count() const noexcept -> std::optional<std::size_t> {
    return impl_ ? impl_->member_count : std::nullopt;
}

auto group::impl_internal_ptr() const noexcept -> void* {
    return impl_ ? impl_->mc_group : nullptr;
}

auto create_group_impl(endpoint& ep,
                       address_vector& av,
                       std::span<const av_handle> members,
                       const group_attr& attr,
                       void* context) -> result<group> {
    if (!ep || !av) {
        return make_error_result<group>(errc::invalid_argument);
    }
    for (const auto member : members) {
        if (!member.valid()) {
            return make_error_result<group>(errc::invalid_argument);
        }
    }
    const bool explicit_members = attr.communication_key.empty() && !(attr.set_flags & FI_UNIVERSE);
    if (attr.member_count &&
        (*attr.member_count == 0 || (explicit_members && *attr.member_count != members.size()))) {
        return make_error_result<group>(errc::invalid_argument);
    }
    auto* native_ep = static_cast<::fid_ep*>(ep.internal_ptr());
    auto* native_av = static_cast<::fid_av*>(av.internal_ptr());
    if (!native_ep->cm || !FI_CHECK_OP(native_ep->cm, fi_ops_cm, join) || !native_av->ops ||
        !FI_CHECK_OP(native_av->ops, fi_ops_av, av_set)) {
        return make_error_result_from_fi_errno<group>(FI_ENOSYS);
    }

    auto impl = std::make_unique<group::impl>();
    impl->member_count = explicit_members ? std::optional{members.size()} : attr.member_count;
    if (!attr.communication_key.empty()) {
        impl->communication_key.assign(attr.communication_key.begin(),
                                       attr.communication_key.end());
    }
    ::fi_av_set_attr set_attr{};
    set_attr.count = attr.member_count.value_or(members.size());
    set_attr.start_addr = FI_ADDR_NOTAVAIL;
    set_attr.end_addr = FI_ADDR_NOTAVAIL;
    set_attr.comm_key_size = impl->communication_key.size();
    set_attr.comm_key = impl->communication_key.empty()
                            ? nullptr
                            : reinterpret_cast<std::uint8_t*>(impl->communication_key.data());
    set_attr.flags = attr.set_flags;
    ::fid_av_set* set = nullptr;
    auto ret = ::fi_av_set(native_av, &set_attr, &set, nullptr);
    if (ret != 0) {
        return make_error_result_from_fi_errno<group>(-ret);
    }
    impl->members.reset(set);
    if (!set || !set->ops) {
        return make_error_result_from_fi_errno<group>(FI_EIO);
    }
    for (const auto member : members) {
        if (!FI_CHECK_OP(set->ops, fi_ops_av_set, insert)) {
            return make_error_result_from_fi_errno<group>(FI_ENOSYS);
        }
        ret = ::fi_av_set_insert(set, member.value);
        if (ret != 0) {
            return make_error_result_from_fi_errno<group>(-ret);
        }
    }

    auto parent = attr.parent_address.value_or(fabric_addrs::unspecified).get();
    if (!attr.parent_address) {
        if (!FI_CHECK_OP(set->ops, fi_ops_av_set, addr)) {
            return make_error_result_from_fi_errno<group>(FI_ENOSYS);
        }
        ret = ::fi_av_set_addr(set, &parent);
        if (ret != 0) {
            return make_error_result_from_fi_errno<group>(-ret);
        }
    }
    ret = ::fi_join_collective(native_ep, parent, set, attr.join_flags, &impl->mc_group, context);
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
