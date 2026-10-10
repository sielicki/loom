// SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only
#include <loom/detail/impl/endpoint_impl.hpp>
#include <loom/loom.hpp>

#include <algorithm>
#include <array>
#include <cstring>
#include <vector>

#include <catch2/catch_test_macros.hpp>

using namespace loom;

TEST_CASE("RMA operations reject an empty endpoint", "[rma]") {
    endpoint ep;
    memory_region mr;
    std::array<std::byte, 1> storage{};
    remote_memory remote{0, 1, storage.size()};
    const std::array iov{iovec{storage.data(), storage.size()}};
    const std::array remote_iov{rma::rma_iov{remote}};
    REQUIRE(rma::read(ep, storage, mr, remote).error() == errc::invalid_argument);
    REQUIRE(rma::write(ep, storage, mr, remote).error() == errc::invalid_argument);
    REQUIRE(rma::write_data(ep, storage, mr, remote, 42).error() == errc::invalid_argument);
    REQUIRE(rma::readv(ep, iov, {}, remote_iov).error() == errc::invalid_argument);
    REQUIRE(rma::writev(ep, iov, {}, remote_iov).error() == errc::invalid_argument);
    REQUIRE(rma::read_to(ep, storage, mr, fabric_addrs::unspecified, remote).error() ==
            errc::invalid_argument);
    REQUIRE(rma::write_to(ep, storage, mr, fabric_addrs::unspecified, remote).error() ==
            errc::invalid_argument);
    REQUIRE(rma::inject_write(ep, storage, remote).error() == errc::invalid_argument);
    REQUIRE(rma::inject_write_data(ep, storage, remote, 42).error() == errc::invalid_argument);
}

namespace {
struct rma_provider {
    std::size_t calls{};
    void* context{};
    std::vector<void*> descriptors;
    std::vector<std::uint64_t> keys;
    fi_ops ops{.size = sizeof(fi_ops), .close = [](fid*) { return 0; }};
    fi_ops_rma rma_ops{
        .size = sizeof(fi_ops_rma),
        .readmsg = [](fid_ep* ep, const fi_msg_rma* msg, std::uint64_t) -> ssize_t {
            return static_cast<rma_provider*>(ep->fid.context)->transfer(*msg, false);
        },
        .writemsg = [](fid_ep* ep, const fi_msg_rma* msg, std::uint64_t) -> ssize_t {
            return static_cast<rma_provider*>(ep->fid.context)->transfer(*msg, true);
        },
    };
    fid_ep handle{.fid = {.context = this, .ops = &ops}, .rma = &rma_ops};

    auto endpoint() -> loom::endpoint {
        return loom::endpoint{
            loom::detail::make_pmr_unique<loom::detail::endpoint_impl>(loom::get_default_resource(),
                                                                       &handle,
                                                                       loom::endpoint_types::msg,
                                                                       loom::address_format::inet)};
    }

    auto transfer(const fi_msg_rma& msg, bool write) -> ssize_t {
        ++calls;
        context = msg.context;
        if (msg.desc) {
            descriptors.assign(msg.desc, msg.desc + msg.iov_count);
        }
        std::size_t local_index{}, local_offset{};
        for (std::size_t i = 0; i < msg.rma_iov_count; ++i) {
            const auto& remote = msg.rma_iov[i];
            keys.push_back(remote.key);
            auto* remote_data = reinterpret_cast<std::byte*>(remote.addr);
            for (std::size_t offset = 0; offset < remote.len;) {
                if (local_index == msg.iov_count) {
                    return -FI_EINVAL;
                }
                const auto& local = msg.msg_iov[local_index];
                const auto count = std::min(local.iov_len - local_offset, remote.len - offset);
                auto* local_data = static_cast<std::byte*>(local.iov_base) + local_offset;
                if (write) {
                    std::memcpy(remote_data + offset, local_data, count);
                } else {
                    std::memcpy(local_data, remote_data + offset, count);
                }
                offset += count;
                local_offset += count;
                if (local_offset == local.iov_len) {
                    ++local_index;
                    local_offset = 0;
                }
            }
        }
        return 0;
    }
};
}  // namespace

TEST_CASE("vectored RMA preserves every remote segment", "[rma]") {
    rma_provider native;
    auto ep = native.endpoint();
    std::array<std::byte, 3> first{std::byte{1}, std::byte{2}, std::byte{3}};
    std::array<std::byte, 5> second{
        std::byte{4}, std::byte{5}, std::byte{6}, std::byte{7}, std::byte{8}};
    std::array<std::byte, 8> local{};
    std::array<iovec, 2> local_iov{{{local.data(), 4}, {local.data() + 4, 4}}};
    std::array<rma::rma_iov, 2> remote{{
        {reinterpret_cast<std::uintptr_t>(first.data()), first.size(), 11},
        {reinterpret_cast<std::uintptr_t>(second.data()), second.size(), 22},
    }};
    int context{};
    REQUIRE(rma::readv(ep, local_iov, {}, remote, &context));
    REQUIRE(local == std::array<std::byte, 8>{std::byte{1},
                                              std::byte{2},
                                              std::byte{3},
                                              std::byte{4},
                                              std::byte{5},
                                              std::byte{6},
                                              std::byte{7},
                                              std::byte{8}});
    REQUIRE(native.keys == std::vector<std::uint64_t>{11, 22});
    REQUIRE(native.context == &context);
    local.fill(std::byte{42});
    REQUIRE(rma::writev(ep, local_iov, {}, remote, &context));
    REQUIRE(std::ranges::all_of(first, [](auto byte) { return byte == std::byte{42}; }));
    REQUIRE(std::ranges::all_of(second, [](auto byte) { return byte == std::byte{42}; }));
}

TEST_CASE("vectored RMA retains all memory descriptors", "[rma]") {
    rma_provider native;
    auto ep = native.endpoint();
    std::array<std::byte, 20> local{}, remote{};
    std::array<iovec, 20> iov{};
    std::array<mr_descriptor, 20> descriptors{};
    std::vector<void*> expected;
    for (std::size_t i = 0; i < local.size(); ++i) {
        iov[i] = {local.data() + i, 1};
        descriptors[i] = mr_descriptor{local.data() + i};
        expected.push_back(local.data() + i);
    }
    std::array remote_iov{
        rma::rma_iov{reinterpret_cast<std::uintptr_t>(remote.data()), remote.size(), 1}};
    SECTION("read") {
        REQUIRE(rma::readv(ep, iov, descriptors, remote_iov));
    }
    SECTION("write") {
        REQUIRE(rma::writev(ep, iov, descriptors, remote_iov));
    }
    REQUIRE(native.descriptors == expected);
    REQUIRE(rma::readv(ep, iov, std::span{descriptors}.first(1), remote_iov).error() ==
            errc::invalid_argument);
    REQUIRE(rma::writev(ep, iov, std::span{descriptors}.first(1), remote_iov).error() ==
            errc::invalid_argument);
    REQUIRE(native.calls == 1);
}

TEST_CASE("rma type definitions", "[rma]") {
    rma::rma_iov iov{};
    iov.addr = 0x1000;
    iov.len = 4096;
    iov.key = 0xABCD;

    REQUIRE(iov.addr == 0x1000UL);
    REQUIRE(iov.len == 4096UL);
    REQUIRE(iov.key == 0xABCDUL);

    remote_memory remote{0x2000, 0x5678, 8192};
    REQUIRE(remote.addr == 0x2000UL);
    REQUIRE(remote.key == 0x5678UL);
    REQUIRE(remote.length == 8192UL);
}

TEST_CASE("rma size queries", "[rma][.provider]") {
    fabric_hints hints{};
    hints.ep_type = endpoint_types::rdm;
    hints.capabilities = capability::rma;

    auto info_result = query_fabric(hints);
    if (!info_result) {
        SKIP("No RMA providers found");
    }

    auto& info = *info_result;
    auto fab_result = fabric::create(info);
    REQUIRE(static_cast<bool>(fab_result));

    auto dom_result = domain::create(*fab_result, info);
    REQUIRE(static_cast<bool>(dom_result));

    auto ep_result = endpoint::create(*dom_result, info);
    REQUIRE(static_cast<bool>(ep_result));

    auto& ep = *ep_result;
    auto enable_result = ep.enable();
    REQUIRE(static_cast<bool>(enable_result));

    auto max_size = rma::get_max_rma_size(ep);
    REQUIRE(max_size > 0);

    auto inject_size = rma::get_inject_size(ep);
    REQUIRE(inject_size > 0);
}

TEST_CASE("inject write small data", "[rma][.provider]") {
    fabric_hints hints{};
    hints.ep_type = endpoint_types::rdm;
    hints.capabilities = capability::rma;

    auto info_result = query_fabric(hints);
    if (!info_result) {
        SKIP("No RMA providers found");
    }

    auto& info = *info_result;
    auto fab_result = fabric::create(info);
    REQUIRE(static_cast<bool>(fab_result));

    auto dom_result = domain::create(*fab_result, info);
    REQUIRE(static_cast<bool>(dom_result));

    auto ep_result = endpoint::create(*dom_result, info);
    REQUIRE(static_cast<bool>(ep_result));

    auto& ep = *ep_result;
    auto enable_result = ep.enable();
    REQUIRE(static_cast<bool>(enable_result));

    std::array<std::byte, 64> data{};
    for (std::size_t i = 0; i < data.size(); ++i) {
        data[i] = static_cast<std::byte>(i);
    }

    remote_memory remote{0x1000, 0xABCD, data.size()};

    [[maybe_unused]] auto result = rma::inject_write(ep, std::span{data}, remote);
}
