// SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only
#include <loom/detail/impl/endpoint_impl.hpp>
#include <loom/loom.hpp>

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <vector>

#include "mock_domain.hpp"
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

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
    std::uint64_t flags{};
    std::uint64_t defaults{};
    fi_addr_t peer{};
    int status{};
    void* context{};
    std::vector<void*> descriptors;
    std::vector<std::uint64_t> keys;
    static auto record(fid_ep* ep) -> ssize_t {
        auto& self = *static_cast<rma_provider*>(ep->fid.context);
        ++self.calls;
        return self.status;
    }
    fi_ops ops{.size = sizeof(fi_ops), .close = [](fid*) { return 0; }};
    fi_ops_rma rma_ops{
        .size = sizeof(fi_ops_rma),
        .read = [](fid_ep* ep, auto...) -> ssize_t { return record(ep); },
        .readmsg = [](fid_ep* ep, const fi_msg_rma* msg, std::uint64_t flags) -> ssize_t {
            return static_cast<rma_provider*>(ep->fid.context)->transfer(*msg, false, flags);
        },
        .write = [](fid_ep* ep, auto...) -> ssize_t { return record(ep); },
        .writemsg = [](fid_ep* ep, const fi_msg_rma* msg, std::uint64_t flags) -> ssize_t {
            return static_cast<rma_provider*>(ep->fid.context)->transfer(*msg, true, flags);
        },
        .inject = [](fid_ep* ep, auto...) -> ssize_t { return record(ep); },
        .writedata = [](fid_ep* ep, auto...) -> ssize_t { return record(ep); },
        .injectdata = [](fid_ep* ep, auto...) -> ssize_t { return record(ep); },
    };
    fid_ep handle{.fid = {.context = this, .ops = &ops}, .rma = &rma_ops};

    auto endpoint() -> loom::endpoint {
        return loom::endpoint{
            loom::detail::make_pmr_unique<loom::detail::endpoint_impl>(loom::get_default_resource(),
                                                                       &handle,
                                                                       loom::endpoint_types::msg,
                                                                       loom::address_format::inet,
                                                                       0UZ,
                                                                       0UZ,
                                                                       loom::caps{},
                                                                       defaults)};
    }

    auto transfer(const fi_msg_rma& msg, bool write, std::uint64_t requested_flags) -> ssize_t {
        ++calls;
        flags = requested_flags;
        peer = msg.addr;
        if (status)
            return status;
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

TEST_CASE("RMA scalar submissions enforce remote extents", "[rma][bounds]") {
    constexpr auto maximum = std::numeric_limits<std::uint64_t>::max();
    struct transfer {
        std::uint64_t address;
        std::size_t extent;
        std::size_t length;
        bool valid;
    };
    constexpr std::array cases{
        transfer{0, 0, 0, true},
        transfer{0, 0, 1, false},
        transfer{0x1000, 8, 7, true},
        transfer{0x1000, 8, 8, true},
        transfer{0x1000, 8, 9, false},
        transfer{0x1000, 8, 16, false},
        transfer{maximum - 7, 8, 7, true},
        transfer{maximum - 7, 8, 8, false},
        transfer{maximum, 0, 0, true},
        transfer{maximum, 8, 1, false},
        transfer{0x1000, 16, 16, true},
    };
    test::mock_domain domain;
    std::array<std::byte, 16> storage{};
    auto mr = memory_region::register_memory(
                  domain.dom, storage, mr_access_flags::read | mr_access_flags::write)
                  .value();
    rma_provider native;
    auto ep = native.endpoint();
    native.status = -FI_EAGAIN;  // Capture submission without accessing a remote address.
    for (const auto& test : cases) {
        CAPTURE(test.address, test.extent, test.length);
        native.calls = 0;
        const auto buffer = std::span{storage}.first(test.length);
        const remote_memory remote{test.address, maximum - 1, test.extent};
        const fabric_addr peer{19};
        const std::array results{
            rma::read(ep, buffer, mr, remote),
            rma::write(ep, buffer, mr, remote),
            rma::write_data(ep, buffer, mr, remote, 7),
            rma::inject_write(ep, buffer, remote),
            rma::inject_write_data(ep, buffer, remote, 7),
            rma::read_to(ep, buffer, mr.descriptor(), peer, remote),
            rma::write_to(ep, buffer, mr.descriptor(), peer, remote),
            rma::read_to(ep, buffer, mr.descriptor(), peer, remote, op_flag::none),
            rma::write_to(ep, buffer, mr.descriptor(), peer, remote, op_flag::none),
            rma::read_to(ep, buffer, mr, peer, remote),
            rma::write_to(ep, buffer, mr, peer, remote),
            rma::read_to(ep, buffer, mr, peer, remote, op_flag::none),
            rma::write_to(ep, buffer, mr, peer, remote, op_flag::none),
        };
        for (const auto& result : results) {
            REQUIRE_FALSE(result);
            CHECK(result.error() == (test.valid ? errc::again : errc::invalid_argument));
        }
        CHECK(native.calls == (test.valid ? results.size() : 0));
    }
}

TEST_CASE("RMA rejects unavailable native operations", "[rma]") {
    const auto missing = GENERATE(0, 1, 2, 3);
    rma_provider native;
    auto ep = native.endpoint();
    if (missing == 0)
        ep = {};
    else if (missing == 1)
        native.handle.rma = nullptr;
    else if (missing == 2)
        native.rma_ops = {.size = sizeof(fi_ops_rma)};
    else
        native.rma_ops.size = sizeof(std::size_t);
    std::array<std::byte, 8> buffer{};
    memory_region memory;
    const remote_memory remote{0, 1, buffer.size()};
    const fabric_addr peer{19};
    const std::array iov{iovec{buffer.data(), buffer.size()}};
    const std::array remote_iov{rma::rma_iov{remote}};
    const std::array results{
        rma::read(ep, buffer, memory, remote),
        rma::write(ep, buffer, memory, remote),
        rma::write_data(ep, buffer, memory, remote, 7),
        rma::inject_write(ep, buffer, remote),
        rma::inject_write_data(ep, buffer, remote, 7),
        rma::read_to(ep, buffer, mr_descriptor{}, peer, remote),
        rma::write_to(ep, buffer, mr_descriptor{}, peer, remote),
        rma::readv_to(ep, iov, {}, peer, remote_iov),
        rma::writev_to(ep, iov, {}, peer, remote_iov),
        rma::read_to(ep, buffer, mr_descriptor{}, peer, remote, op_flag::completion),
        rma::write_to(ep, buffer, mr_descriptor{}, peer, remote, op_flag::completion),
        rma::readv_to(ep, iov, {}, peer, remote_iov, op_flag::completion),
        rma::writev_to(ep, iov, {}, peer, remote_iov, op_flag::completion),
    };
    for (const auto& result : results) {
        REQUIRE_FALSE(result);
        REQUIRE(result.error() == (missing == 0 ? errc::invalid_argument : errc::not_supported));
    }
    REQUIRE(native.calls == 0);
}

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

TEST_CASE("RMA flags preserve explicit overrides and inherited defaults", "[rma][flags]") {
    const auto write = GENERATE(false, true);
    const auto vector = GENERATE(false, true);
    const auto inherit = GENERATE(false, true);
    const auto status = GENERATE(0, -FI_EAGAIN, -FI_EIO);
    CAPTURE(write, vector, inherit, status);
    rma_provider native;
    native.defaults = FI_FENCE;
    native.status = status;
    auto ep = native.endpoint();
    std::array<std::byte, 8> local{}, target{};
    local.fill(std::byte{23});
    target.fill(std::byte{42});
    const remote_memory remote{reinterpret_cast<std::uintptr_t>(target.data()), 17, target.size()};
    const fabric_addr peer{19};
    const std::array iov{iovec{local.data(), 3}, iovec{local.data() + 3, 5}};
    const std::array remote_iov{rma::rma_iov{remote}};
    int descriptor{}, context{};
    const mr_descriptor local_descriptor{&descriptor};
    const std::array descriptors{local_descriptor, local_descriptor};
    const auto flags = op_flag::completion | (write ? op_flag::delivery_complete : op_flag::none);
    auto submit = [&]<operation_defaults Defaults>() -> void_result {
        if (vector) {
            return write ? rma::writev_to<Defaults>(
                               ep, iov, descriptors, peer, remote_iov, flags, &context)
                         : rma::readv_to<Defaults>(
                               ep, iov, descriptors, peer, remote_iov, flags, &context);
        }
        return write ? rma::write_to<Defaults>(
                           ep, local, local_descriptor, peer, remote, flags, &context)
                     : rma::read_to<Defaults>(
                           ep, local, local_descriptor, peer, remote, flags, &context);
    };
    const auto result = inherit ? submit.template operator()<operation_defaults::inherit>()
                                : submit.template operator()<operation_defaults::replace>();
    REQUIRE(native.calls == 1);
    REQUIRE(native.peer == peer.get());
    REQUIRE(native.flags ==
            (FI_COMPLETION | (inherit ? FI_FENCE : 0) | (write ? FI_DELIVERY_COMPLETE : 0)));
    if (status) {
        REQUIRE_FALSE(result);
        REQUIRE(result.error() == make_error_code_from_fi_errno(-status));
    } else {
        REQUIRE(result);
        REQUIRE(native.context == &context);
        REQUIRE(native.keys == std::vector<std::uint64_t>{remote.key});
        REQUIRE(native.descriptors == std::vector<void*>(vector ? 2 : 1, &descriptor));
        REQUIRE(local == target);
    }
}

TEST_CASE("RMA reads reject explicit write-only flags", "[rma][flags]") {
    const auto flag =
        GENERATE(op_flag::inject, op_flag::transmit_complete, op_flag::delivery_complete);
    const auto vector = GENERATE(false, true);
    rma_provider native;
    native.status = -FI_EAGAIN;
    auto ep = native.endpoint();
    std::array<std::byte, 8> local{};
    const remote_memory remote{0x1000, 7, local.size()};
    const std::array iov{iovec{local.data(), local.size()}};
    const std::array remote_iov{rma::rma_iov{remote}};
    const auto flags = flag | op_flag::completion;
    const auto result =
        vector ? rma::readv_to(ep, iov, {}, fabric_addr{19}, remote_iov, flags)
               : rma::read_to(ep, local, mr_descriptor{}, fabric_addr{19}, remote, flags);
    REQUIRE_FALSE(result);
    CHECK(result.error() == errc::invalid_argument);
    CHECK(native.calls == 0);
}

TEST_CASE("RMA reads filter write-only defaults", "[rma][flags]") {
    const auto write_flag = GENERATE(FI_REMOTE_CQ_DATA,
                                     FI_INJECT,
                                     FI_INJECT_COMPLETE,
                                     FI_TRANSMIT_COMPLETE,
                                     FI_DELIVERY_COMPLETE,
                                     FI_COMMIT_COMPLETE);
    const auto defaults = GENERATE(operation_defaults::replace,
                                   operation_defaults::inherit,
                                   operation_defaults::inherit_without_more);
    const auto vector = GENERATE(false, true);
    rma_provider native;
    native.status = -FI_EAGAIN;
    native.defaults = FI_FENCE | FI_MORE | write_flag;
    auto ep = native.endpoint();
    std::array<std::byte, 8> local{};
    const remote_memory remote{0x1000, 7, local.size()};
    const std::array iov{iovec{local.data(), local.size()}};
    const std::array remote_iov{rma::rma_iov{remote}};
    const auto submit = [&]<operation_defaults Defaults>() {
        return vector
                   ? rma::readv_to<Defaults>(
                         ep, iov, {}, fabric_addr{19}, remote_iov, op_flag::completion)
                   : rma::read_to<Defaults>(
                         ep, local, mr_descriptor{}, fabric_addr{19}, remote, op_flag::completion);
    };
    const auto result = [&] {
        switch (defaults) {
            case operation_defaults::replace:
                return submit.template operator()<operation_defaults::replace>();
            case operation_defaults::inherit:
                return submit.template operator()<operation_defaults::inherit>();
            case operation_defaults::inherit_without_more:
                return submit.template operator()<operation_defaults::inherit_without_more>();
        }
        std::unreachable();
    }();
    REQUIRE_FALSE(result);
    CHECK(result.error() == errc::again);
    CHECK(native.calls == 1);
    CHECK(native.flags ==
          (FI_COMPLETION | (defaults == operation_defaults::replace ? 0 : FI_FENCE) |
           (defaults == operation_defaults::inherit ? FI_MORE : 0)));

    test::mock_domain domain;
    auto mr = memory_region::register_memory(
                  domain.dom, local, mr_access_flags::read | mr_access_flags::write)
                  .value();
    const auto check = [&](const auto& result) {
        REQUIRE_FALSE(result);
        CHECK(result.error() == errc::again);
        CHECK(native.flags == (FI_FENCE | FI_MORE));
    };
    check(rma::read(ep, local, mr, remote));
    check(rma::read_to(ep, local, mr, fabric_addr{19}, remote));
    check(ep.read(local, rma_addr{remote.addr}, mr_key{remote.key}));
    CHECK(native.calls == 4);
    native.rma_ops.readmsg = nullptr;
    CHECK(rma::read_to(ep, local, mr, fabric_addr{19}, remote).error() == errc::not_supported);
    CHECK(native.calls == 4);
}
