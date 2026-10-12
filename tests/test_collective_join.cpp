// SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only
#include <loom/detail/impl/domain_impl.hpp>

#include <array>
#include <vector>

#include "mock_collective.hpp"
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

using loom::test::collective_provider;

TEST_CASE("collective queries validate operations and barrier attributes",
          "[collective][validation]") {
    using operation = loom::collective::operation;
    struct query_case {
        operation value;
        fi_collective_op native;
        bool valid{true};
    };
    constexpr std::array collectives{
        query_case{operation::barrier, FI_BARRIER},
        query_case{operation::broadcast, FI_BROADCAST},
        query_case{operation::all_to_all, FI_ALLTOALL},
        query_case{operation::all_reduce, FI_ALLREDUCE},
        query_case{operation::all_gather, FI_ALLGATHER},
        query_case{operation::reduce_scatter, FI_REDUCE_SCATTER},
        query_case{operation::reduce, FI_REDUCE},
        query_case{operation::scatter, FI_SCATTER},
        query_case{operation::gather, FI_GATHER},
        query_case{static_cast<operation>(-1), FI_BARRIER, false},
        query_case{static_cast<operation>(999), FI_BARRIER, false},
    };
    const auto status = GENERATE(0, -FI_EOPNOTSUPP);
    for (const auto& collective : collectives) {
        for (const auto& reduction : loom::test::collective_reductions) {
            CAPTURE(collective.value, reduction.operation, status);
            struct query_record {
                int calls{};
                int status;
                fi_collective_op collective{};
                fi_collective_attr attributes{};
            } record{.status = status};
            fi_ops lifetime{.size = sizeof(fi_ops), .close = [](fid*) { return 0; }};
            fi_ops_domain operations{
                .size = sizeof(fi_ops_domain),
                .query_collective =
                    [](fid_domain* dom,
                       fi_collective_op op,
                       fi_collective_attr* attr,
                       std::uint64_t flags) {
                        auto& record = *static_cast<query_record*>(dom->fid.context);
                        ++record.calls;
                        record.collective = op;
                        record.attributes = *attr;
                        CHECK(flags == 0);
                        return record.status;
                    },
            };
            fid_domain native{.fid = {.context = &record, .ops = &lifetime}, .ops = &operations};
            loom::domain dom{loom::detail::make_pmr_unique<loom::detail::domain_impl>(
                loom::get_default_resource(), &native)};
            const auto valid = collective.valid && reduction.valid;
            CHECK(reduction.query(dom, collective.value) == (valid && status == 0));
            CHECK(record.calls == (valid ? 1 : 0));
            if (valid) {
                CHECK(record.collective == collective.native);
                CHECK(record.attributes.op ==
                      (collective.value == operation::barrier ? FI_NOOP : reduction.native));
                CHECK(record.attributes.datatype ==
                      (collective.value == operation::barrier ? FI_VOID : FI_UINT64));
                CHECK(record.attributes.mode == 0);
            }
        }
    }
}

TEST_CASE("collective queries reject invalid datatypes", "[collective][validation]") {
    int queries{};
    fi_ops lifetime{.size = sizeof(fi_ops), .close = [](fid*) { return 0; }};
    fi_ops_domain operations{
        .size = sizeof(fi_ops_domain),
        .query_collective =
            [](fid_domain* dom,
               fi_collective_op op,
               fi_collective_attr* attr,
               std::uint64_t flags) {
                ++*static_cast<int*>(dom->fid.context);
                CHECK(op == FI_ALLREDUCE);
                CHECK(attr->datatype == FI_UINT64);
                CHECK(attr->op == FI_SUM);
                CHECK(flags == 0);
                return 0;
            },
    };
    fid_domain native{.fid = {.context = &queries, .ops = &lifetime}, .ops = &operations};
    loom::domain dom{loom::detail::make_pmr_unique<loom::detail::domain_impl>(
        loom::get_default_resource(), &native)};
    using enum loom::atomic::operation;
    CHECK_FALSE((loom::collective::is_supported<static_cast<loom::atomic::datatype>(-1), sum>(
        dom, loom::collective::operation::all_reduce)));
    CHECK_FALSE((loom::collective::is_supported<static_cast<loom::atomic::datatype>(999), sum>(
        dom, loom::collective::operation::all_reduce)));
    CHECK(queries == 0);
    CHECK((loom::collective::is_supported<std::uint64_t, sum>(
        dom, loom::collective::operation::all_reduce)));
    CHECK(queries == 1);
}

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
    REQUIRE(group->member_count() == 3);
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
        attr.member_count = 2;
        auto joined =
            loom::collective::group::join(provider.ep, provider.av, members, attr, &context);
        REQUIRE(joined);
        group = std::move(*joined);
        REQUIRE_FALSE(*joined);
    }
    REQUIRE(group);
    REQUIRE(group.member_count() == 2);
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
    REQUIRE_FALSE(group.member_count());
    REQUIRE(moved.member_count() == 2);
    // Models a provider publishing the multicast handle just before FI_JOIN_COMPLETE.
    *provider.pending = &provider.native_group;
    provider.pending = nullptr;
    REQUIRE(moved.internal_ptr() == &provider.native_group);
    REQUIRE(moved.address() == loom::fabric_addr{999});
    REQUIRE(provider.context == &context);
    moved = {};
    REQUIRE(provider.closed == std::vector<int>{1, 2});
}

TEST_CASE("collective joins reject inconsistent membership counts", "[collective][join]") {
    const auto count = GENERATE(std::size_t{0}, std::size_t{1}, std::size_t{3});
    collective_provider provider;
    const std::array<loom::av_handle, 2> members{{{5}, {9}}};
    loom::collective::group_attr attr{};
    attr.member_count = count;
    const auto result = loom::collective::group::join(provider.ep, provider.av, members, attr);
    REQUIRE_FALSE(result);
    REQUIRE(result.error() == loom::errc::invalid_argument);
    REQUIRE(provider.set_opens == 0);
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
