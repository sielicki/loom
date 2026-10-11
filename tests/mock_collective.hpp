// SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only
#pragma once

#include <loom/core/collective.hpp>
#include <loom/detail/impl/address_vector_impl.hpp>
#include <loom/detail/impl/endpoint_impl.hpp>

#include <vector>

namespace loom::test {
struct collective_provider {
    int failure{};
    int set_opens{};
    int address_queries{};
    int joins{};
    bool delayed{};
    fi_av_set_attr attributes{};
    fi_addr_t parent{};
    std::uint64_t join_flags{};
    void* context{};
    fid_mc** pending{};
    std::vector<fi_addr_t> members;
    std::vector<int> closed;
    fi_ops lifetime{.size = sizeof(fi_ops), .close = [](fid*) { return 0; }};
    fi_ops set_lifetime{
        .size = sizeof(fi_ops),
        .close =
            [](fid* handle) {
                static_cast<collective_provider*>(handle->context)->closed.push_back(2);
                return 0;
            },
    };
    fi_ops group_lifetime{
        .size = sizeof(fi_ops),
        .close =
            [](fid* handle) {
                static_cast<collective_provider*>(handle->context)->closed.push_back(1);
                return 0;
            },
    };
    fi_ops_av_set set_ops{
        .size = sizeof(fi_ops_av_set),
        .insert =
            [](fid_av_set* set, fi_addr_t address) {
                auto& self = *static_cast<collective_provider*>(set->fid.context);
                if (self.failure == 2)
                    return -FI_EIO;
                self.members.push_back(address);
                return 0;
            },
        .addr =
            [](fid_av_set* set, fi_addr_t* address) {
                auto& self = *static_cast<collective_provider*>(set->fid.context);
                ++self.address_queries;
                if (self.failure == 3)
                    return -FI_EIO;
                *address = 123;
                return 0;
            },
    };
    fid_av_set native_set{.fid = {.context = this, .ops = &set_lifetime}, .ops = &set_ops};
    fid_mc native_group{.fid = {.context = this, .ops = &group_lifetime}, .fi_addr = 999};
    fi_ops_av av_ops{
        .size = sizeof(fi_ops_av),
        .av_set =
            [](fid_av* av, fi_av_set_attr* attr, fid_av_set** result, void*) {
                auto& self = *static_cast<collective_provider*>(av->fid.context);
                ++self.set_opens;
                if (self.failure == 1)
                    return -FI_EIO;
                self.attributes = *attr;
                *result = &self.native_set;
                return 0;
            },
    };
    fi_ops_cm cm_ops{
        .size = sizeof(fi_ops_cm),
        .join =
            [](fid_ep* ep,
               const void* address,
               std::uint64_t flags,
               fid_mc** result,
               void* context) {
                auto& self = *static_cast<collective_provider*>(ep->fid.context);
                ++self.joins;
                const auto& collective = *static_cast<const fi_collective_addr*>(address);
                if (collective.set != &self.native_set)
                    return -FI_EINVAL;
                self.parent = collective.coll_addr;
                self.join_flags = flags;
                self.context = context;
                if (self.failure == 4)
                    return -FI_EIO;
                if (self.delayed) {
                    *result = nullptr;
                    self.pending = result;
                } else {
                    *result = &self.native_group;
                }
                return 0;
            },
    };
    fid_av native_av{.fid = {.context = this, .ops = &lifetime}, .ops = &av_ops};
    fid_domain domain_handle{};
    fid_ep native_ep{.fid = {.context = this, .ops = &lifetime}, .cm = &cm_ops};
    loom::address_vector av{loom::detail::make_pmr_unique<loom::detail::address_vector_impl>(
        loom::get_default_resource(), &native_av, loom::av_type::map, loom::address_format::inet)};
    loom::endpoint ep{
        loom::detail::make_pmr_unique<loom::detail::endpoint_impl>(loom::get_default_resource(),
                                                                   &native_ep,
                                                                   loom::endpoint_types::rdm,
                                                                   loom::address_format::inet,
                                                                   0,
                                                                   0,
                                                                   loom::caps{},
                                                                   0,
                                                                   0,
                                                                   &domain_handle)};
};
}  // namespace loom::test
