// SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only
#include <loom/detail/conversions.hpp>
#include <loom/detail/impl/address_vector_impl.hpp>
#include <loom/detail/impl/domain_impl.hpp>
#include <loom/detail/impl/endpoint_impl.hpp>

#include <algorithm>
#include <array>
#include <cstring>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

namespace {
struct provider {
    std::vector<std::byte> received;
    std::size_t address_size = sizeof(sockaddr_in);
    std::size_t calls{};
    bool fail_middle{};
    fid* bound_address_vector{};
    int bind_result{};
    std::uint64_t open_flags{};
    int open_calls{};
    std::vector<std::byte> stored_address;
    std::string printable_address;
    bool report_small_buffer{};
    int lookup_calls{};
    int straddr_calls{};
    std::vector<void*> contexts;
    fi_ops ops{
        .size = sizeof(fi_ops),
        .close = [](fid*) { return 0; },
        .bind =
            [](fid* ep, fid* av, std::uint64_t) {
                auto& self = *static_cast<provider*>(ep->context);
                self.bound_address_vector = av;
                return self.bind_result;
            },
    };
    fi_ops_cm cm{
        .size = sizeof(fi_ops_cm),
        .connect =
            [](fid_ep* ep, const void* addr, const void*, std::size_t) {
                auto& self = *static_cast<provider*>(ep->fid.context);
                self.capture(addr, 1);
                return 0;
            },
    };
    fi_ops_av av_ops{
        .size = sizeof(fi_ops_av),
        .insert =
            [](fid_av* av,
               const void* addr,
               std::size_t count,
               fi_addr_t* output,
               std::uint64_t,
               void* context) {
                auto& self = *static_cast<provider*>(av->fid.context);
                self.capture(addr, count);
                self.contexts.push_back(context);
                for (std::size_t i = 0; i < count; ++i) {
                    output[i] = self.fail_middle && i == 1 ? FI_ADDR_NOTAVAIL : 100 + i;
                }
                return static_cast<int>(count - (self.fail_middle && count > 1 ? 1 : 0));
            },
        .lookup =
            [](fid_av* av, fi_addr_t, void* address, std::size_t* length) {
                auto& self = *static_cast<provider*>(av->fid.context);
                ++self.lookup_calls;
                const auto capacity = *length;
                std::memcpy(address,
                            self.stored_address.data(),
                            std::min(capacity, self.stored_address.size()));
                *length = self.stored_address.size();
                return self.report_small_buffer && capacity < *length ? -FI_ETOOSMALL : 0;
            },
        .straddr =
            [](fid_av* av, const void* address, char* output, std::size_t* length) -> const char* {
            auto& self = *static_cast<provider*>(av->fid.context);
            ++self.straddr_calls;
            REQUIRE(std::memcmp(address, self.stored_address.data(), self.stored_address.size()) ==
                    0);
            const auto count = std::min(*length - 1, self.printable_address.size());
            std::memcpy(output, self.printable_address.data(), count);
            output[count] = '\0';
            *length = self.printable_address.size() + 1;
            return output;
        },
    };
    fid_ep ep_handle{.fid = {.context = this, .ops = &ops}, .cm = &cm};
    fid_av av_handle{.fid = {.context = this, .ops = &ops}, .ops = &av_ops};
    fi_ops_domain domain_ops{
        .size = sizeof(fi_ops_domain),
        .av_open =
            [](fid_domain* dom, fi_av_attr* attr, fid_av** av, void*) {
                auto& self = *static_cast<provider*>(dom->fid.context);
                ++self.open_calls;
                self.open_flags = attr->flags;
                attr->type = FI_AV_TABLE;
                *av = &self.av_handle;
                return 0;
            },
    };
    fid_domain domain_handle{.fid = {.context = this, .ops = &ops}, .ops = &domain_ops};

    auto domain() -> loom::domain {
        return loom::domain{loom::detail::make_pmr_unique<loom::detail::domain_impl>(
            loom::get_default_resource(), &domain_handle)};
    }

    auto endpoint(loom::address_format format) -> loom::endpoint {
        return loom::endpoint{loom::detail::make_pmr_unique<loom::detail::endpoint_impl>(
            loom::get_default_resource(), &ep_handle, loom::endpoint_types::msg, format)};
    }
    auto vector(loom::address_format format) -> loom::address_vector {
        return loom::address_vector{
            loom::detail::make_pmr_unique<loom::detail::address_vector_impl>(
                loom::get_default_resource(), &av_handle, loom::av_type::table, format)};
    }
    void capture(const void* address, std::size_t count) {
        ++calls;
        const auto* bytes = static_cast<const std::byte*>(address);
        received.assign(bytes, bytes + count * address_size);
    }
};
}  // namespace

TEST_CASE("address lookup retries provider-sized buffers", "[address]") {
    provider native;
    native.stored_address.resize(512, std::byte{0x42});
    sockaddr_in expected{};
    expected.sin_family = AF_INET;
    expected.sin_port = htons(4321);
    expected.sin_addr.s_addr = htonl(0xC0000201U);
    std::memcpy(native.stored_address.data(), &expected, sizeof(expected));
    auto av = native.vector(loom::address_format::inet);
    SECTION("truncated success") {
        native.report_small_buffer = false;
    }
    SECTION("buffer-size error") {
        native.report_small_buffer = true;
    }
    REQUIRE(av.lookup(loom::av_handle{100}).value() ==
            loom::address{loom::ipv4_address{0xC0000201U, 4321}});
    REQUIRE(native.lookup_calls == 2);
}

TEST_CASE("address formatting resizes and omits the terminator", "[address]") {
    provider native;
    native.stored_address.resize(512, std::byte{0x42});
    auto av = native.vector(loom::address_format::inet);
    SECTION("short text") {
        native.printable_address = "192.0.2.1:4321";
    }
    SECTION("long text") {
        native.printable_address.assign(600, 'x');
    }
    REQUIRE(av.address_to_string(loom::av_handle{100}).value() == native.printable_address);
    REQUIRE(native.lookup_calls == 2);
    REQUIRE(native.straddr_calls == (native.printable_address.size() > 256 ? 2 : 1));
}

TEST_CASE("address vector creation translates flags", "[address]") {
    provider native;
    auto dom = native.domain();
    loom::address_vector_attr attr;
    attr.flags = loom::av_flags::read_only | loom::av_flags::symmetric;
    REQUIRE(loom::address_vector::create(dom, attr));
    REQUIRE(native.open_flags == (FI_READ | FI_SYMMETRIC));
}

TEST_CASE("address vectors retain the provider-selected organization", "[address]") {
    provider native;
    auto dom = native.domain();
    loom::address_vector_attr attr;
    attr.type = loom::av_type::unspecified;
    auto av = loom::address_vector::create(dom, attr);
    REQUIRE(av);
    REQUIRE(av->get_type() == loom::av_type::table);
}

TEST_CASE("synchronous address vectors reject asynchronous creation", "[address]") {
    provider native;
    auto dom = native.domain();
    loom::address_vector_attr attr;
    SECTION("loom flag") {
        attr.flags = loom::av_flags::async;
    }
    SECTION("native flag") {
        attr.flags = FI_EVENT;
    }
    REQUIRE(loom::address_vector::create(dom, attr).error() == loom::errc::not_supported);
    REQUIRE(native.open_calls == 0);
}

TEST_CASE("address vector binding reaches the provider", "[address]") {
    provider native;
    auto av = native.vector(loom::address_format::inet);
    auto ep = native.endpoint(loom::address_format::inet);
    REQUIRE(av.bind(ep));
    REQUIRE(native.bound_address_vector == &native.av_handle.fid);
    native.bind_result = -FI_EINVAL;
    REQUIRE(av.bind(ep).error() == loom::errc::invalid_argument);
    loom::endpoint empty;
    REQUIRE(av.bind(empty).error() == loom::errc::invalid_argument);
}

TEST_CASE("connection and AV insertion encode native IPv4 addresses", "[address]") {
    provider native;
    const loom::address addr = loom::ipv4_address{0xC0000201U, 4321};
    SECTION("connection") {
        auto ep = native.endpoint(loom::address_format::inet);
        REQUIRE(ep.connect(addr));
    }
    SECTION("address vector") {
        auto av = native.vector(loom::address_format::inet);
        auto result = av.insert(addr);
        REQUIRE(result);
        REQUIRE(result->value == 100);
    }
    sockaddr_in received{};
    REQUIRE(native.received.size() == sizeof(received));
    std::memcpy(&received, native.received.data(), sizeof(received));
    REQUIRE(received.sin_family == AF_INET);
    REQUIRE(ntohs(received.sin_port) == 4321);
    REQUIRE(ntohl(received.sin_addr.s_addr) == 0xC0000201U);
}

TEST_CASE("address insertion encodes IPv6 and rejects format mismatches", "[address]") {
    provider native;
    native.address_size = sizeof(sockaddr_in6);
    const std::array<std::uint16_t, 8> segments{0x2001, 0xdb8, 0, 0, 0, 0, 0, 42};
    const loom::address addr = loom::ipv6_address{segments, 9876, 7, 0x12345};
    auto av = native.vector(loom::address_format::inet6);
    REQUIRE(av.insert(addr));
    sockaddr_in6 received{};
    std::memcpy(&received, native.received.data(), sizeof(received));
    REQUIRE(received.sin6_family == AF_INET6);
    REQUIRE(ntohs(received.sin6_port) == 9876);
    REQUIRE(received.sin6_addr.s6_addr[0] == 0x20);
    REQUIRE(received.sin6_addr.s6_addr[1] == 0x01);
    REQUIRE(received.sin6_addr.s6_addr[15] == 42);
    REQUIRE(received.sin6_scope_id == 7);
    REQUIRE(ntohl(received.sin6_flowinfo) == 0x12345);
    REQUIRE(loom::address_from_raw(&received, sizeof(received), loom::address_format::inet6) ==
            addr);
    auto mismatched = av.insert(loom::ipv4_address{0x7F000001U, 9876});
    REQUIRE_FALSE(mismatched);
    REQUIRE(mismatched.error() == loom::errc::invalid_argument);
    REQUIRE(native.calls == 1);
}

TEST_CASE("generic socket formats retain typed IP transport", "[address]") {
    enum class transport { connect, insert, batch, batch_with_context };
    const auto format = GENERATE(loom::address_format::sockaddr, loom::address_format::sockaddr_ip);
    const bool ipv6 = GENERATE(false, true);
    const auto via = GENERATE(
        transport::connect, transport::insert, transport::batch, transport::batch_with_context);
    const std::array<std::uint16_t, 8> segments{0x2001, 0xdb8, 0, 0, 0, 0, 0, 42};
    const loom::address addr = ipv6 ? loom::address{loom::ipv6_address{segments, 9876, 7, 0x12345}}
                                    : loom::address{loom::ipv4_address{0xC0000201U, 4321}};
    provider native;
    native.address_size = ipv6 ? sizeof(sockaddr_in6) : sizeof(sockaddr_in);
    auto av = native.vector(format);
    if (via == transport::connect) {
        auto ep = native.endpoint(format);
        REQUIRE(ep.connect(addr));
    } else if (via == transport::insert) {
        REQUIRE(av.insert(addr));
    } else {
        const bool with_contexts = via == transport::batch_with_context;
        const std::array addresses{addr, addr};
        std::array<loom::av_handle, 2> handles{};
        std::array<int, 2> tokens{};
        std::array<void*, 2> contexts{&tokens[0], &tokens[1]};
        const auto result = av.insert_batch(
            addresses, handles, with_contexts ? std::span{contexts} : std::span<void*>{});
        REQUIRE(result);
        REQUIRE(*result == 2);
        REQUIRE(native.calls == (with_contexts ? 2 : 1));
        REQUIRE(native.received.size() == native.address_size * (with_contexts ? 1 : 2));
        if (with_contexts) {
            REQUIRE(std::ranges::equal(native.contexts, contexts));
        } else {
            REQUIRE(loom::address_from_raw(native.received.data() + native.address_size,
                                           native.address_size,
                                           format) == addr);
        }
    }
    REQUIRE(loom::address_from_raw(native.received.data(), native.address_size, format) == addr);
    // Decoding must tolerate unaligned storage and reject incomplete native addresses.
    std::vector<std::byte> unaligned(native.address_size + 1);
    std::memcpy(unaligned.data() + 1, native.received.data(), native.address_size);
    REQUIRE(loom::address_from_raw(unaligned.data() + 1, native.address_size, format) == addr);
    REQUIRE(std::holds_alternative<loom::unspecified_address>(
        loom::address_from_raw(unaligned.data() + 1, native.address_size - 1, format)));
    REQUIRE(std::holds_alternative<loom::unspecified_address>(
        loom::address_from_raw(unaligned.data() + 1, 1, format)));
    sockaddr unknown{};
    REQUIRE(std::holds_alternative<loom::unspecified_address>(
        loom::address_from_raw(&unknown, sizeof(unknown), format)));
}

TEST_CASE("generic socket batches reject mixed address strides before submission", "[address]") {
    const auto format = GENERATE(loom::address_format::sockaddr, loom::address_format::sockaddr_ip);
    const bool reverse = GENERATE(false, true);
    const bool with_contexts = GENERATE(false, true);
    std::array<loom::address, 2> addresses{loom::ipv4_address{0xC0000201U, 4321},
                                           loom::ipv6_address{}};
    if (reverse) {
        std::swap(addresses[0], addresses[1]);
    }
    std::array<loom::av_handle, 2> handles{};
    std::array<void*, 2> contexts{};
    provider native;
    auto av = native.vector(format);
    const auto result = av.insert_batch(
        addresses, handles, with_contexts ? std::span{contexts} : std::span<void*>{});
    REQUIRE_FALSE(result);
    REQUIRE(result.error() == loom::errc::invalid_argument);
    REQUIRE(native.calls == 0);
}

TEST_CASE("provider address formats cannot silently accept an IP encoding", "[address]") {
    const auto format = GENERATE(std::uint32_t{FI_ADDR_PSMX},
                                 std::uint32_t{FI_ADDR_PSMX2},
                                 std::uint32_t{FI_ADDR_PSMX3},
                                 std::uint32_t{FI_ADDR_GNI},
                                 std::uint32_t{FI_ADDR_BGQ},
                                 std::uint32_t{FI_ADDR_MLX},
                                 std::uint32_t{FI_ADDR_STR},
                                 std::uint32_t{FI_ADDR_IB_UD},
                                 std::uint32_t{FI_ADDR_EFA},
                                 std::uint32_t{FI_ADDR_OPX},
                                 std::uint32_t{FI_ADDR_CXI},
                                 std::uint32_t{FI_ADDR_UCX});
    provider native;
    auto av = native.vector(loom::detail::from_fi_addr_format(format));
    auto ep = native.endpoint(loom::detail::from_fi_addr_format(format));
    const loom::address addr = loom::ipv4_address{0xC0000201U, 4321};
    const auto inserted = av.insert(addr);
    const auto connected = ep.connect(addr);
    REQUIRE_FALSE(inserted);
    REQUIRE_FALSE(connected);
    REQUIRE(inserted.error() == loom::errc::invalid_argument);
    REQUIRE(connected.error() == loom::errc::invalid_argument);
    REQUIRE(native.calls == 0);
}

TEST_CASE("address vectors inherit the domain format for round trips", "[address][integration]") {
    loom::fabric_hints hints;
    hints.ep_type = loom::endpoint_types::rdm;
    hints.capabilities = loom::capability::msg;
    hints.addr_format = loom::address_format::inet;
    auto info = loom::query_fabric(hints);
    if (!info) {
        SKIP("No IPv4 RDM provider available");
    }
    auto fabric = loom::fabric::create(*info);
    REQUIRE(fabric);
    auto domain = loom::domain::create(*fabric, *info);
    REQUIRE(domain);
    auto av = loom::address_vector::create(*domain);
    REQUIRE(av);
    const loom::address addr = loom::ipv4_address{0x7F000001U, 12345};
    auto handle = av->insert(addr);
    REQUIRE(handle);
    auto found = av->lookup(*handle);
    REQUIRE(found);
    REQUIRE(*found == addr);
}

TEST_CASE("batch insertion packs native addresses and preserves failed positions", "[address]") {
    provider native;
    native.fail_middle = true;
    auto av = native.vector(loom::address_format::inet);
    const std::array<loom::address, 3> addresses{loom::ipv4_address{0xC0000201U, 1000},
                                                 loom::ipv4_address{0xC0000202U, 2000},
                                                 loom::ipv4_address{0xC0000203U, 3000}};
    std::array<loom::av_handle, 3> handles{};
    REQUIRE(av.insert_batch(addresses, handles).value() == 2);
    REQUIRE(native.calls == 1);
    REQUIRE(native.received.size() == 3 * sizeof(sockaddr_in));
    for (std::size_t i = 0; i < addresses.size(); ++i) {
        sockaddr_in addr{};
        std::memcpy(&addr, native.received.data() + i * sizeof(addr), sizeof(addr));
        REQUIRE(addr.sin_family == AF_INET);
        REQUIRE(ntohs(addr.sin_port) == (i + 1) * 1000);
        REQUIRE(ntohl(addr.sin_addr.s_addr) == 0xC0000201U + i);
    }
    REQUIRE(handles[0].value == 100);
    REQUIRE_FALSE(handles[1].valid());
    REQUIRE(handles[2].value == 102);
    REQUIRE(av.count() == 2);
}

TEST_CASE("batch insertion forwards individual contexts", "[address]") {
    provider native;
    auto av = native.vector(loom::address_format::inet);
    const std::array<loom::address, 2> addresses{loom::ipv4_address{0xC0000201U, 1000},
                                                 loom::ipv4_address{0xC0000202U, 2000}};
    std::array<loom::av_handle, 2> handles{};
    int first{}, second{};
    std::array<void*, 2> contexts{&first, &second};
    REQUIRE(av.insert_batch(addresses, handles, contexts).value() == 2);
    REQUIRE(native.contexts == std::vector<void*>{&first, &second});
    REQUIRE_FALSE(av.insert_batch(addresses, handles, std::span{contexts}.first(1)));
    REQUIRE(native.calls == 2);
}
