// SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only
#include <loom/core/collective.hpp>
#include <loom/detail/impl/address_vector_impl.hpp>
#include <loom/detail/impl/endpoint_impl.hpp>

#include <array>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

namespace {
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
    fid_ep native_ep{.fid = {.context = this, .ops = &lifetime}, .cm = &cm_ops};
    loom::address_vector av{loom::detail::make_pmr_unique<loom::detail::address_vector_impl>(
        loom::get_default_resource(), &native_av, loom::av_type::map, loom::address_format::inet)};
    loom::endpoint ep{
        loom::detail::make_pmr_unique<loom::detail::endpoint_impl>(loom::get_default_resource(),
                                                                   &native_ep,
                                                                   loom::endpoint_types::rdm,
                                                                   loom::address_format::inet)};
};
}  // namespace

TEST_CASE("collective joins provide membership and report the joined address",
          "[collective][join]") {
    collective_provider provider;
    int context{};
    const std::array<loom::av_handle, 3> members{{{5}, {2}, {9}}};
    auto group = loom::collective::group::join(provider.ep, provider.av, members, {}, &context);
    REQUIRE(group);
    REQUIRE(provider.set_opens == 1);
    REQUIRE(provider.members == std::vector<fi_addr_t>{5, 2, 9});
    REQUIRE(provider.attributes.count == 3);
    REQUIRE(provider.attributes.start_addr == FI_ADDR_NOTAVAIL);
    REQUIRE(provider.attributes.end_addr == FI_ADDR_NOTAVAIL);
    REQUIRE(provider.attributes.stride == 0);
    REQUIRE(provider.parent == 123);
    REQUIRE(provider.join_flags == FI_COLLECTIVE);
    REQUIRE(provider.context == &context);
    REQUIRE(group->address() == loom::fabric_addr{999});
    REQUIRE(provider.closed.empty());
    *group = {};
    REQUIRE(provider.closed == std::vector<int>{1, 2});
}

TEST_CASE("collective membership survives delayed joins and moves", "[collective][join]") {
    collective_provider provider;
    provider.delayed = true;
    int context{};
    loom::collective::group group;
    {
        const std::array<loom::av_handle, 2> members{{{5}, {9}}};
        std::array key{std::byte{7}, std::byte{8}, std::byte{9}};
        loom::collective::group_attr attr{};
        attr.communication_key = key;
        attr.set_flags = FI_BARRIER_SET;
        attr.join_flags = FI_SEND;
        attr.parent_address = loom::fabric_addrs::unspecified;
        auto joined =
            loom::collective::group::join(provider.ep, provider.av, members, attr, &context);
        REQUIRE(joined);
        group = std::move(*joined);
        REQUIRE_FALSE(*joined);
    }
    REQUIRE(group);
    REQUIRE(group.internal_ptr() == nullptr);
    REQUIRE(group.address() == loom::fabric_addrs::unspecified);
    REQUIRE(provider.pending);
    REQUIRE(provider.closed.empty());
    REQUIRE(provider.parent == FI_ADDR_NOTAVAIL);
    REQUIRE(provider.address_queries == 0);
    REQUIRE(provider.join_flags == (FI_COLLECTIVE | FI_SEND));
    REQUIRE(provider.attributes.flags == FI_BARRIER_SET);
    REQUIRE(provider.attributes.comm_key_size == 3);
    REQUIRE(provider.attributes.comm_key[0] == 7);
    REQUIRE(provider.attributes.comm_key[2] == 9);
    auto moved = std::move(group);
    REQUIRE_FALSE(group);
    // Models a provider publishing the multicast handle just before FI_JOIN_COMPLETE.
    *provider.pending = &provider.native_group;
    provider.pending = nullptr;
    REQUIRE(moved.internal_ptr() == &provider.native_group);
    REQUIRE(moved.address() == loom::fabric_addr{999});
    REQUIRE(provider.context == &context);
    moved = {};
    REQUIRE(provider.closed == std::vector<int>{1, 2});
}

TEST_CASE("failed collective setup releases its membership set", "[collective][join]") {
    const auto step = GENERATE(1, 2, 3, 4);
    collective_provider provider;
    provider.failure = step;
    const std::array<loom::av_handle, 2> members{{{5}, {9}}};
    auto group = loom::collective::group::join(provider.ep, provider.av, members);
    REQUIRE_FALSE(group);
    REQUIRE(group.error() == loom::make_error_code_from_fi_errno(FI_EIO));
    REQUIRE(provider.closed == (step == 1 ? std::vector<int>{} : std::vector<int>{2}));
}

TEST_CASE("collective joins reject invalid endpoints vectors and members", "[collective][join]") {
    collective_provider provider;
    loom::endpoint empty_endpoint;
    loom::address_vector empty_vector;
    const std::array<loom::av_handle, 1> members{{{5}}};
    const std::array<loom::av_handle, 1> invalid{{{FI_ADDR_NOTAVAIL}}};
    REQUIRE(loom::collective::group::join(empty_endpoint, provider.av, members).error() ==
            loom::errc::invalid_argument);
    REQUIRE(loom::collective::group::join(provider.ep, empty_vector, members).error() ==
            loom::errc::invalid_argument);
    REQUIRE(loom::collective::group::join(provider.ep, provider.av, invalid).error() ==
            loom::errc::invalid_argument);
    REQUIRE(provider.set_opens == 0);
    REQUIRE(provider.joins == 0);
}

TEST_CASE("missing collective setup operations return errors", "[collective][join]") {
    const auto missing = GENERATE(0, 1, 2, 3, 4);
    collective_provider provider;
    if (missing == 0)
        provider.native_ep.cm = nullptr;
    else if (missing == 1)
        provider.cm_ops.join = nullptr;
    else if (missing == 2)
        provider.av_ops.av_set = nullptr;
    else if (missing == 3)
        provider.set_ops.insert = nullptr;
    else
        provider.set_ops.addr = nullptr;
    const std::array<loom::av_handle, 1> members{{{5}}};
    auto group = loom::collective::group::join(provider.ep, provider.av, members);
    REQUIRE_FALSE(group);
    REQUIRE(group.error() == loom::make_error_code_from_fi_errno(FI_ENOSYS));
    REQUIRE(provider.joins == 0);
    REQUIRE(provider.closed == (missing < 3 ? std::vector<int>{} : std::vector<int>{2}));
}
