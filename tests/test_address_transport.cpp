// SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only
#include <loom/core/formatters.hpp>
#include <loom/detail/conversions.hpp>
#include <loom/detail/impl/address_vector_impl.hpp>
#include <loom/detail/impl/domain_impl.hpp>
#include <loom/detail/impl/endpoint_impl.hpp>
#include <loom/detail/impl/passive_endpoint_impl.hpp>

#include <algorithm>
#include <array>
#include <cstring>
#include <utility>
#include <vector>

#if __has_include(<infiniband/ib.h>)
#    include <infiniband/ib.h>
#endif

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
    int lookup_result{};
    int name_calls{};
    int name_result{};
    int cm_result{};
    bool silent_truncation{};
    bool string_addresses{};
    std::vector<std::string> received_strings;
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
        .getname =
            [](fid* handle, void* addr, std::size_t* length) {
                return static_cast<provider*>(handle->context)->copy_name(addr, length);
            },
        .getpeer =
            [](fid_ep* ep, void* addr, std::size_t* length) {
                return static_cast<provider*>(ep->fid.context)->copy_name(addr, length);
            },
        .connect =
            [](fid_ep* ep, const void* addr, const void*, std::size_t) {
                auto& self = *static_cast<provider*>(ep->fid.context);
                self.capture(addr, 1);
                return self.cm_result;
            },
        .listen = [](fid_pep* ep) { return record_cm(&ep->fid); },
        .accept = [](fid_ep* ep, const void*, std::size_t) { return record_cm(&ep->fid); },
        .reject = [](fid_pep* ep, fid*, const void*, std::size_t) { return record_cm(&ep->fid); },
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
                if (self.string_addresses) {
                    ++self.calls;
                    const auto* strings = static_cast<const char* const*>(addr);
                    for (std::size_t i = 0; i < count; ++i)
                        self.received_strings.emplace_back(strings[i]);
                } else {
                    self.capture(addr, count);
                }
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
                if (self.lookup_result)
                    return self.lookup_result;
                const auto capacity = *length;
                if (!self.stored_address.empty())
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
    fid_pep pep_handle{.fid = {.context = this, .ops = &ops}, .cm = &cm};
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

    static auto record_cm(fid* handle) -> int {
        auto& self = *static_cast<provider*>(handle->context);
        ++self.calls;
        return self.cm_result;
    }

    auto domain() -> loom::domain {
        return loom::domain{loom::detail::make_pmr_unique<loom::detail::domain_impl>(
            loom::get_default_resource(), &domain_handle)};
    }

    auto endpoint(loom::address_format format) -> loom::endpoint {
        return loom::endpoint{loom::detail::make_pmr_unique<loom::detail::endpoint_impl>(
            loom::get_default_resource(), &ep_handle, loom::endpoint_types::msg, format)};
    }
    auto passive(loom::address_format format) -> loom::passive_endpoint {
        return loom::passive_endpoint{
            loom::detail::make_pmr_unique<loom::detail::passive_endpoint_impl>(
                loom::get_default_resource(), &pep_handle, format)};
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
    auto copy_name(void* addr, std::size_t* length) -> int {
        ++name_calls;
        if (name_result)
            return name_result;
        const auto capacity = *length;
        *length = stored_address.size();
        if (!stored_address.empty())
            std::memcpy(addr, stored_address.data(), std::min(capacity, *length));
        return capacity < *length && !silent_truncation ? -FI_ETOOSMALL : 0;
    }
};

constexpr loom::ib_socket_address ib_socket{
    .gid = {0xa0,
            0xa1,
            0xa2,
            0xa3,
            0xa4,
            0xa5,
            0xa6,
            0xa7,
            0xa8,
            0xa9,
            0xaa,
            0xab,
            0xac,
            0xad,
            0xae,
            0xaf},
    .partition_key = 0x8123,
    .flow_info = 0x12345678,
    .service_id = 0x1122334455667788,
    .service_id_mask = 0xffff0000abcd1234,
    .scope_id = 0x0123456789abcdef,
};

auto ib_socket_bytes(bool zero = false) -> std::array<std::uint8_t, 48> {
    // sockaddr_ib ABI: family and scope are native-endian; all other scalars
    // are network-endian. Distinct field bytes catch offset and width errors.
    std::array<std::uint8_t, 48> bytes{
        0,    0,    0x81, 0x23, 0x12, 0x34, 0x56, 0x78, 0xa0, 0xa1, 0xa2, 0xa3, 0xa4, 0xa5,
        0xa6, 0xa7, 0xa8, 0xa9, 0xaa, 0xab, 0xac, 0xad, 0xae, 0xaf, 0x11, 0x22, 0x33, 0x44,
        0x55, 0x66, 0x77, 0x88, 0xff, 0xff, 0,    0,    0xab, 0xcd, 0x12, 0x34,
    };
    if (zero)
        bytes.fill(0);
    const std::uint16_t family = 27;  // AF_IB, also used by libfabric's portable layout.
    std::memcpy(bytes.data(), &family, sizeof(family));
    const auto scope = zero ? std::uint64_t{} : ib_socket.scope_id;
    std::memcpy(bytes.data() + 40, &scope, sizeof(scope));
    return bytes;
}
}  // namespace

TEST_CASE("InfiniBand socket decoding preserves every native field", "[address][ib]") {
    const auto format = GENERATE(loom::address_format::ib, loom::address_format::sockaddr);
    const auto zero = GENERATE(false, true);
    const auto bytes = ib_socket_bytes(zero);
    const loom::address expected{zero ? loom::ib_socket_address{} : ib_socket};
    CHECK(loom::address_from_raw(bytes.data(), bytes.size(), format) == expected);
    std::array<std::uint8_t, 49> unaligned{};
    std::ranges::copy(bytes, unaligned.begin() + 1);
    CHECK(loom::address_from_raw(unaligned.data() + 1, bytes.size(), format) == expected);
    for (std::size_t length = 0; length < bytes.size(); ++length) {
        CAPTURE(length);
        CHECK(std::holds_alternative<loom::unspecified_address>(
            loom::address_from_raw(bytes.data(), length, format)));
    }
    auto wrong_family = bytes;
    const std::uint16_t family = AF_INET;
    std::memcpy(wrong_family.data(), &family, sizeof(family));
    CHECK(std::holds_alternative<loom::unspecified_address>(
        loom::address_from_raw(wrong_family.data(), bytes.size(), loom::address_format::ib)));
    CHECK(std::holds_alternative<loom::unspecified_address>(
        loom::address_from_raw(bytes.data(), bytes.size(), loom::address_format::sockaddr_ip)));
}

TEST_CASE("InfiniBand sockets survive connection and address vector operations", "[address][ib]") {
    enum class transport {
        connect,
        insert,
        batch,
        batch_with_context,
        lookup,
        local_name,
        peer_name
    };
    const auto format = GENERATE(loom::address_format::ib, loom::address_format::sockaddr);
    const auto via = GENERATE(transport::connect,
                              transport::insert,
                              transport::batch,
                              transport::batch_with_context,
                              transport::lookup,
                              transport::local_name,
                              transport::peer_name);
    const auto zero = GENERATE(false, true);
    const auto bytes = ib_socket_bytes(zero);
    provider native;
    native.address_size = bytes.size();
    const loom::address addr{zero ? loom::ib_socket_address{} : ib_socket};
    auto av = native.vector(format);
    if (via == transport::lookup || via == transport::local_name || via == transport::peer_name) {
        const auto storage = std::as_bytes(std::span{bytes});
        native.stored_address.assign(storage.begin(), storage.end());
        if (via == transport::lookup) {
            REQUIRE(av.lookup(loom::av_handle{100}).value() == addr);
        } else {
            auto ep = native.endpoint(format);
            const auto result =
                via == transport::local_name ? ep.get_local_address() : ep.get_peer_address();
            REQUIRE(result);
            CHECK(*result == addr);
        }
        return;
    }
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
        CHECK(*result == 2);
        CHECK(handles[0].value == 100);
        CHECK(handles[1].value == (with_contexts ? 100 : 101));
        if (with_contexts)
            CHECK(std::ranges::equal(native.contexts, contexts));
    }
    REQUIRE(native.received.size() % bytes.size() == 0);
    for (std::size_t offset = 0; offset < native.received.size(); offset += bytes.size())
        CHECK(std::memcmp(native.received.data() + offset, bytes.data(), bytes.size()) == 0);
}

TEST_CASE("IP-only address formats reject InfiniBand socket submissions", "[address][ib]") {
    const auto format = GENERATE(
        loom::address_format::inet, loom::address_format::inet6, loom::address_format::sockaddr_ip);
    provider native;
    auto ep = native.endpoint(format);
    auto av = native.vector(format);
    const loom::address addr{ib_socket};
    const auto rejected = [](const auto& result) {
        REQUIRE_FALSE(result);
        CHECK(result.error() == loom::errc::invalid_argument);
    };
    rejected(ep.connect(addr));
    rejected(av.insert(addr));
    const std::array addresses{addr};
    std::array<loom::av_handle, 1> handles{};
    rejected(av.insert_batch(addresses, handles));
    CHECK(native.calls == 0);
}

#if __has_include(<infiniband/ib.h>)
TEST_CASE("InfiniBand socket layout agrees with rdma-core", "[address][ib][native]") {
    using native = loom::detail::native_ib_address;
    static_assert(loom::detail::ib_socket_family == AF_IB);
    static_assert(sizeof(native) == sizeof(sockaddr_ib));
    static_assert(alignof(native) == alignof(sockaddr_ib));
    static_assert(offsetof(native, family) == offsetof(sockaddr_ib, sib_family));
    static_assert(offsetof(native, partition_key) == offsetof(sockaddr_ib, sib_pkey));
    static_assert(offsetof(native, flow_info) == offsetof(sockaddr_ib, sib_flowinfo));
    static_assert(offsetof(native, gid) == offsetof(sockaddr_ib, sib_addr));
    static_assert(offsetof(native, service_id) == offsetof(sockaddr_ib, sib_sid));
    static_assert(offsetof(native, service_id_mask) == offsetof(sockaddr_ib, sib_sid_mask));
    static_assert(offsetof(native, scope_id) == offsetof(sockaddr_ib, sib_scope_id));
    const auto bytes = ib_socket_bytes();
    sockaddr_ib expected{};
    expected.sib_family = AF_IB;
    expected.sib_pkey = htobe16(ib_socket.partition_key);
    expected.sib_flowinfo = htobe32(ib_socket.flow_info);
    std::ranges::copy(ib_socket.gid, expected.sib_addr.sib_raw);
    expected.sib_sid = htobe64(ib_socket.service_id);
    expected.sib_sid_mask = htobe64(ib_socket.service_id_mask);
    expected.sib_scope_id = ib_socket.scope_id;
    CHECK(std::memcmp(&expected, bytes.data(), bytes.size()) == 0);
    CHECK(loom::address_from_raw(&expected, sizeof(expected), loom::address_format::ib) ==
          loom::address{ib_socket});
}
#endif

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
    const bool infiniband = GENERATE(false, true);
    std::array<loom::address, 2> addresses{loom::ipv4_address{0xC0000201U, 4321},
                                           infiniband ? loom::address{ib_socket}
                                                      : loom::address{loom::ipv6_address{}}};
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

TEST_CASE("provider addresses survive name lookup and reinsertion", "[address][provider]") {
    enum class source { local, peer, passive, lookup };
    const auto from = GENERATE(source::local, source::peer, source::passive, source::lookup);
    const auto format = GENERATE(loom::address_format::psmx,
                                 loom::address_format::psmx2,
                                 loom::address_format::gni,
                                 loom::address_format::bgq,
                                 loom::address_format::mlx,
                                 loom::address_format::ib_ud,
                                 loom::address_format::efa,
                                 loom::address_format::psmx3,
                                 loom::address_format::opx,
                                 loom::address_format::cxi,
                                 loom::address_format::ucx);
    const auto length = GENERATE(32u, 513u);
    const auto report_small_buffer = GENERATE(false, true);
    CAPTURE(from, format, length, report_small_buffer);
    provider native;
    native.report_small_buffer = report_small_buffer;
    native.silent_truncation = !report_small_buffer;
    native.address_size = length;
    native.stored_address.resize(length);
    for (std::size_t i = 0; i < length; ++i)
        native.stored_address[i] = static_cast<std::byte>(i);
    auto ep = native.endpoint(format);
    auto pep = native.passive(format);
    auto av = native.vector(format);
    const auto result = [&]() -> loom::result<loom::address> {
        switch (from) {
            case source::local:
                return ep.get_local_address();
            case source::peer:
                return ep.get_peer_address();
            case source::passive:
                return pep.get_local_address();
            case source::lookup:
                return av.lookup(loom::av_handle{100});
        }
        std::unreachable();
    }();
    REQUIRE(result);
    CHECK(loom::get_address_format(*result) == format);
    REQUIRE(std::holds_alternative<loom::opaque_address>(*result));
    const auto& opaque = std::get<loom::opaque_address>(*result);
    CHECK(std::ranges::equal(opaque.bytes(), native.stored_address));
    CHECK((from == source::lookup ? native.lookup_calls : native.name_calls) ==
          (length > 256 ? 2 : 1));
    REQUIRE(av.insert(*result));
    CHECK(native.received == native.stored_address);
}

TEST_CASE("opaque addresses own their bytes and retain their format", "[address][opaque]") {
    std::array storage{std::byte{0x17}, std::byte{0xa3}, std::byte{0xff}};
    const auto decoded =
        loom::address_from_raw(storage.data(), storage.size(), loom::address_format::efa);
    auto copy = decoded;
    auto moved = std::move(copy);
    storage.fill(std::byte{});
    REQUIRE(std::holds_alternative<loom::opaque_address>(moved));
    CHECK(moved == decoded);
    CHECK(loom::get_address_format(moved) == loom::address_format::efa);
    CHECK(std::format("{}", moved) == "opaque(14):17a3ff");
    CHECK(loom::get_port(moved) == 0);
    CHECK_FALSE(loom::address_ops::is_network_protocol(moved));
    CHECK_FALSE(loom::address_ops::is_rdma_capable(moved));
    CHECK(loom::address_ops::size(moved) == sizeof(loom::opaque_address));
    CHECK_THROWS_AS((loom::opaque_address{loom::address_format::inet, storage}),
                    std::invalid_argument);
    CHECK_THROWS_AS((loom::opaque_address{loom::address_format::efa, {}}), std::invalid_argument);
    storage.fill(std::byte{0x41});
    CHECK_THROWS_AS((loom::opaque_address{loom::address_format::str, storage}),
                    std::invalid_argument);
}

TEST_CASE("opaque binary addresses preserve native submission layout", "[address][opaque]") {
    enum class submission { connect, insert, batch, batch_with_context };
    const auto via = GENERATE(
        submission::connect, submission::insert, submission::batch, submission::batch_with_context);
    const auto mismatch = GENERATE(false, true);
    provider native;
    native.address_size = 32;
    std::array<std::byte, 32> first{}, second{};
    first.fill(std::byte{0x65});
    second.fill(std::byte{0x32});
    const std::array<loom::address, 2> addresses{
        loom::opaque_address{loom::address_format::efa, first},
        loom::opaque_address{loom::address_format::efa, second}};
    auto ep = native.endpoint(mismatch ? loom::address_format::cxi : loom::address_format::efa);
    auto av = native.vector(mismatch ? loom::address_format::cxi : loom::address_format::efa);
    std::array<loom::av_handle, 2> handles{};
    std::array<int, 2> tokens{};
    std::array<void*, 2> contexts{&tokens[0], &tokens[1]};
    const auto result = [&]() -> loom::result<void> {
        if (via == submission::connect)
            return ep.connect(addresses[0]);
        if (via == submission::insert)
            return av.insert(addresses[0]).transform([](auto) {});
        return av
            .insert_batch(addresses,
                          handles,
                          via == submission::batch_with_context ? std::span{contexts}
                                                                : std::span<void*>{})
            .transform([](auto) {});
    }();
    if (mismatch) {
        REQUIRE_FALSE(result);
        CHECK(result.error() == loom::errc::invalid_argument);
        CHECK(native.calls == 0);
    } else {
        REQUIRE(result);
        if (via == submission::batch) {
            REQUIRE(native.received.size() == first.size() + second.size());
            CHECK(std::ranges::equal(std::span{native.received}.first(first.size()), first));
            CHECK(std::ranges::equal(std::span{native.received}.last(second.size()), second));
            CHECK(handles[0].value == 100);
            CHECK(handles[1].value == 101);
        } else {
            CHECK(std::ranges::equal(native.received,
                                     via == submission::batch_with_context ? second : first));
            if (via == submission::batch_with_context)
                CHECK(std::ranges::equal(native.contexts, contexts));
        }
    }
}

TEST_CASE("opaque binary batches reject inconsistent sizes before insertion", "[address][opaque]") {
    const auto contexts_requested = GENERATE(false, true);
    provider native;
    auto av = native.vector(loom::address_format::efa);
    std::array<std::byte, 32> storage{};
    const std::array<loom::address, 2> addresses{
        loom::opaque_address{loom::address_format::efa, storage},
        loom::opaque_address{loom::address_format::efa, std::span{storage}.first(31)}};
    std::array<loom::av_handle, 2> handles{};
    std::array<void*, 2> contexts{};
    const auto result = av.insert_batch(
        addresses, handles, contexts_requested ? std::span{contexts} : std::span<void*>{});
    REQUIRE_FALSE(result);
    CHECK(result.error() == loom::errc::invalid_argument);
    CHECK(native.calls == 0);
}

TEST_CASE("string addresses use native pointer arrays for AV insertion", "[address][opaque]") {
    const auto batched = GENERATE(false, true);
    const auto contexts_requested = GENERATE(false, true);
    provider native;
    native.string_addresses = true;
    std::string first = "fi_test://" + std::string(600, 'a');
    std::string second = "fi_test://b";
    const auto bytes = [](const std::string& text) {
        return std::as_bytes(std::span{text.c_str(), text.size() + 1});
    };
    const auto input = bytes(first);
    native.address_size = input.size();
    native.stored_address.assign(input.begin(), input.end());
    auto ep = native.endpoint(loom::address_format::str);
    auto av = native.vector(loom::address_format::str);
    const auto decoded = ep.get_local_address();
    REQUIRE(decoded);
    REQUIRE(std::holds_alternative<loom::opaque_address>(*decoded));
    CHECK(std::ranges::equal(std::get<loom::opaque_address>(*decoded).bytes(), input));
    REQUIRE(ep.connect(*decoded));
    CHECK(native.received == native.stored_address);
    std::array<int, 2> tokens{};
    std::array<void*, 2> contexts{&tokens[0], &tokens[1]};
    if (batched) {
        const std::array<loom::address, 2> addresses{
            *decoded, loom::opaque_address{loom::address_format::str, bytes(second)}};
        std::array<loom::av_handle, 2> handles{};
        REQUIRE(av.insert_batch(addresses,
                                handles,
                                contexts_requested ? std::span{contexts} : std::span<void*>{})
                    .value() == 2);
        CHECK(native.received_strings == std::vector{first, second});
        if (contexts_requested)
            CHECK(std::ranges::equal(native.contexts, contexts));
    } else {
        REQUIRE(av.insert(*decoded, contexts_requested ? contexts[0] : nullptr));
        CHECK(native.received_strings == std::vector{first});
        CHECK(native.contexts[0] == (contexts_requested ? contexts[0] : nullptr));
    }
}

TEST_CASE("connection management rejects unavailable native operations", "[address][invalid]") {
    enum class operation { local_name, peer_name, connect, accept, passive_name, listen, reject };
    enum class support { available, missing_table, missing_member, truncated, empty };
    const auto call = GENERATE(operation::local_name,
                               operation::peer_name,
                               operation::connect,
                               operation::accept,
                               operation::passive_name,
                               operation::listen,
                               operation::reject);
    const auto state = GENERATE(support::available,
                                support::missing_table,
                                support::missing_member,
                                support::truncated,
                                support::empty);
    CAPTURE(call, state);
    provider native;
    native.cm_result = native.name_result = -FI_EAGAIN;
    auto ep = native.endpoint(loom::address_format::inet);
    auto pep = native.passive(loom::address_format::inet);
    if (state == support::missing_table)
        native.ep_handle.cm = native.pep_handle.cm = nullptr;
    if (state == support::empty) {
        ep = {};
        pep = {};
    }
    const auto configure = [&](auto member, std::size_t offset) {
        if (state == support::missing_member)
            native.cm.*member = nullptr;
        if (state == support::truncated)
            native.cm.size = offset;
    };
    const auto result = [&]() -> loom::void_result {
        switch (call) {
            case operation::local_name:
                configure(&fi_ops_cm::getname, offsetof(fi_ops_cm, getname));
                return ep.get_local_address().transform([](auto&&) {});
            case operation::peer_name:
                configure(&fi_ops_cm::getpeer, offsetof(fi_ops_cm, getpeer));
                return ep.get_peer_address().transform([](auto&&) {});
            case operation::connect:
                configure(&fi_ops_cm::connect, offsetof(fi_ops_cm, connect));
                return ep.connect(loom::ipv4_address{0x7f000001U, 4321});
            case operation::accept:
                configure(&fi_ops_cm::accept, offsetof(fi_ops_cm, accept));
                return ep.accept();
            case operation::passive_name:
                configure(&fi_ops_cm::getname, offsetof(fi_ops_cm, getname));
                return pep.get_local_address().transform([](auto&&) {});
            case operation::listen:
                configure(&fi_ops_cm::listen, offsetof(fi_ops_cm, listen));
                return pep.listen();
            case operation::reject:
                configure(&fi_ops_cm::reject, offsetof(fi_ops_cm, reject));
                return pep.reject(&native.ep_handle.fid);
        }
        std::unreachable();
    }();
    REQUIRE_FALSE(result);
    CHECK(result.error() == (state == support::available ? loom::errc::again
                             : state == support::empty   ? loom::errc::invalid_argument
                                                         : loom::errc::not_supported));
    CHECK(native.calls + native.name_calls == (state == support::available ? 1 : 0));
}

TEST_CASE("address queries reject malformed and unsupported representations",
          "[address][invalid]") {
    const auto format = GENERATE(loom::address_format::inet,
                                 loom::address_format::inet6,
                                 loom::address_format::ib,
                                 loom::address_format::str,
                                 loom::address_format::efa,
                                 loom::address_format::unspecified,
                                 static_cast<loom::address_format>(0xffffffff));
    provider native;
    // Empty provider addresses and short/malformed typed addresses cannot be used.
    if (format != loom::address_format::efa)
        native.stored_address.assign(4, std::byte{0x55});
    auto ep = native.endpoint(format);
    auto pep = native.passive(format);
    auto av = native.vector(format);
    const std::array results{ep.get_local_address(),
                             ep.get_peer_address(),
                             pep.get_local_address(),
                             av.lookup(loom::av_handle{100})};
    for (const auto& result : results) {
        REQUIRE_FALSE(result);
        const auto expected = format == loom::address_format::unspecified ||
                                      format == static_cast<loom::address_format>(0xffffffff)
                                  ? loom::errc::not_supported
                                  : loom::errc::invalid_argument;
        CHECK(result.error() == expected);
    }
}

TEST_CASE("address queries propagate native failures without retry loops", "[address][invalid]") {
    const auto error = GENERATE(FI_EIO, FI_ETOOSMALL);
    provider native;
    native.name_result = native.lookup_result = -error;
    auto ep = native.endpoint(loom::address_format::efa);
    auto pep = native.passive(loom::address_format::efa);
    auto av = native.vector(loom::address_format::efa);
    const std::array results{ep.get_local_address(),
                             ep.get_peer_address(),
                             pep.get_local_address(),
                             av.lookup(loom::av_handle{100})};
    for (const auto& result : results) {
        REQUIRE_FALSE(result);
        CHECK(result.error() == loom::make_error_code_from_fi_errno(error));
    }
    CHECK(native.name_calls == 3);
    CHECK(native.lookup_calls == 1);
}
