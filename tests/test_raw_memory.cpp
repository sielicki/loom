// SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only
#include <loom/core/concepts/memory.hpp>
#include <loom/core/mr_cache.hpp>
#include <loom/core/provider_aware.hpp>
#include <loom/core/rma.hpp>
#include <loom/detail/impl/endpoint_impl.hpp>

#include <array>
#include <cstring>
#include <limits>
#include <ranges>
#include <utility>

#include "mock_domain.hpp"
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

static_assert(loom::memory_region_type<loom::memory_region>);
static_assert(!loom::remote_memory_descriptor<loom::raw_remote_memory>);
static_assert(!loom::remote_memory_descriptor_duck<loom::raw_remote_memory>);

TEST_CASE("scalar exports reject raw and unavailable keys", "[memory][raw]") {
    const auto raw = GENERATE(false, true);
    const auto unavailable = GENERATE(false, true);
    const auto attributed = GENERATE(false, true);
    loom::test::mock_domain provider{raw ? FI_MR_RAW : 0};
    std::array<std::byte, 64> buffer{};
    auto region =
        attributed ? loom::memory_region::register_memory(
                         provider.dom,
                         buffer.data(),
                         buffer.size(),
                         loom::memory_registration{.access = loom::mr_access_flags::remote_write})
                   : loom::memory_region::register_memory(
                         provider.dom, buffer, loom::mr_access_flags::remote_write);
    REQUIRE(region);
    if (unavailable) {
        auto* native = static_cast<fid_mr*>(region->internal_ptr());
        provider.keys.erase(native->key);
        native->key = FI_KEY_NOTAVAIL;
        provider.keys.insert(native->key);
    }
    const auto key = region->key();
    const auto remote = loom::remote_memory::from_mr(*region);
    const auto specialized =
        loom::provider_remote_memory<loom::provider::tcp_tag>::from_mr(*region);
    if (raw || unavailable) {
        CHECK_FALSE(key);
        CHECK_FALSE(remote);
        CHECK_FALSE(specialized);
        if (!key && !remote && !specialized) {
            const auto expected = raw ? loom::errc::not_supported : loom::errc::again;
            CHECK(key.error() == expected);
            CHECK(remote.error() == expected);
            CHECK(specialized.error() == expected);
        }
    } else {
        REQUIRE(key);
        CHECK(*key == 0);
        REQUIRE(remote);
        CHECK(remote->key == *key);
        REQUIRE(specialized);
        CHECK(specialized->key == *key);
    }
}

TEST_CASE("empty and cached raw regions cannot export scalar descriptors", "[memory][raw]") {
    const loom::memory_region empty;
    CHECK_FALSE(empty.key());
    CHECK_FALSE(loom::remote_memory::from_mr(empty));
    loom::test::mock_domain provider{FI_MR_RAW};
    loom::mr_cache<loom::provider::tcp_tag> cache{provider.dom};
    std::array<std::byte, 64> buffer{};
    auto handle = cache.lookup(buffer.data(), buffer.size(), loom::mr_access_flags::send);
    REQUIRE(handle);
    CHECK_FALSE(handle->key());
    CHECK_FALSE(handle->to_remote_memory());
}

namespace {
struct raw_provider : loom::test::mock_domain {
    std::vector<std::byte> bytes{std::byte{0x52},
                                 std::byte{0},
                                 std::byte{0xff},
                                 std::byte{0x79},
                                 std::byte{1},
                                 std::byte{2},
                                 std::byte{3},
                                 std::byte{4},
                                 std::byte{5},
                                 std::byte{6},
                                 std::byte{7},
                                 std::byte{8},
                                 std::byte{9}};
    std::uint64_t base = 0xfedcba9876543210;
    std::uint64_t next_key = 0xabcdef0123456789;
    std::uint64_t mapped_base{};
    std::vector<std::byte> mapped_bytes;
    std::unordered_set<std::uint64_t> active;
    const std::uint8_t* retained_bytes{};
    int exports{};
    int maps{};
    int unmaps{};
    int probe_error{};
    int export_error{};
    int map_error{};
    int unmap_error{};
    bool oversized{};
    bool shrink{};

    static auto owner(fid* handle) -> raw_provider& {
        return *static_cast<raw_provider*>(static_cast<loom::test::mock_domain*>(handle->context));
    }
    explicit raw_provider(int mode = FI_MR_RAW | FI_MR_VIRT_ADDR) : mock_domain(mode) {
        region_ops.control = [](fid* mr, int command, void* value) {
            if (command != FI_GET_RAW_MR)
                return -FI_ENOSYS;
            auto& self = owner(mr);
            auto& attr = *static_cast<struct fi_mr_raw_attr*>(value);
            ++self.exports;
            if (attr.flags)
                return -FI_EINVAL;
            if (self.exports == 1 && self.probe_error)
                return self.probe_error;
            if (self.exports == 2 && self.export_error)
                return self.export_error;
            if (*attr.key_size < self.bytes.size()) {
                *attr.key_size = self.bytes.size();
                return -FI_ETOOSMALL;
            }
            if (!self.bytes.empty())
                std::memcpy(attr.raw_key, self.bytes.data(), self.bytes.size());
            *attr.base_addr = self.base;
            *attr.key_size = self.oversized ? self.bytes.size() + 1
                             : self.shrink  ? self.bytes.size() - 1
                                            : self.bytes.size();
            return 0;
        };
        domain_ops.control = [](fid* domain, int command, void* value) {
            auto& self = owner(domain);
            if (command == FI_MAP_RAW_MR) {
                ++self.maps;
                if (self.map_error)
                    return self.map_error;
                auto& attr = *static_cast<struct fi_mr_map_raw*>(value);
                if (attr.flags)
                    return -FI_EINVAL;
                self.mapped_base = attr.base_addr;
                self.mapped_bytes.resize(attr.key_size);
                if (attr.key_size)
                    std::memcpy(self.mapped_bytes.data(), attr.raw_key, attr.key_size);
                self.retained_bytes = attr.raw_key;
                *attr.key = self.next_key++;
                self.active.insert(*attr.key);
                return 0;
            }
            if (command == FI_UNMAP_KEY) {
                ++self.unmaps;
                if (self.unmap_error)
                    return self.unmap_error;
                return self.active.erase(*static_cast<std::uint64_t*>(value)) ? 0 : -FI_EINVAL;
            }
            return -FI_ENOSYS;
        };
    }
    auto region(std::span<std::byte> data) -> loom::memory_region {
        return loom::memory_region::register_memory(dom, data, loom::mr_access_flags::remote_write)
            .value();
    }
};

struct counted_resource : loom::memory_resource {
    int remaining = 100;
    int live{};
    auto do_allocate(std::size_t size, std::size_t alignment) -> void* override {
        if (remaining-- <= 0)
            throw std::bad_alloc{};
        auto* result = std::pmr::new_delete_resource()->allocate(size, alignment);
        ++live;
        return result;
    }
    void do_deallocate(void* ptr, std::size_t size, std::size_t alignment) override {
        --live;
        std::pmr::new_delete_resource()->deallocate(ptr, size, alignment);
    }
    auto do_is_equal(const std::pmr::memory_resource& other) const noexcept -> bool override {
        return this == &other;
    }
};
}  // namespace

TEST_CASE("raw exports own bytes and preserve both address models", "[memory][raw]") {
    enum class registration { host, cuda, dmabuf };
    const auto interface = GENERATE(registration::host, registration::cuda, registration::dmabuf);
    const auto mode = GENERATE(0, FI_MR_RAW, FI_MR_RAW | FI_MR_VIRT_ADDR);
    const auto empty_key = GENERATE(false, true);
    const auto shrink = GENERATE(false, true);
    raw_provider provider{mode};
    if (empty_key)
        provider.bytes.clear();
    provider.shrink = shrink && !empty_key;
    std::array<std::byte, 80> buffer{};
    auto region = [&] {
        if (interface == registration::host)
            return provider.region(std::span{buffer}.first(64));
        loom::memory_registration attributes{.access = loom::mr_access_flags::remote_write};
        if (interface == registration::cuda) {
            attributes.device = loom::hmem_device::cuda(3);
            attributes.allocation = loom::hmem_allocation_kind::device;
        } else {
            attributes.dmabuf = loom::dmabuf_registration{17, 8};
        }
        return loom::memory_region::register_memory(provider.dom, buffer.data(), 64, attributes)
            .value();
    }();
    counted_resource resource;
    auto raw = region.export_raw(&resource);
    REQUIRE(raw);
    CHECK(provider.exports == 2);
    CHECK(raw->key.get_allocator().resource() == &resource);
    CHECK(raw->base_addr == provider.base);
    CHECK(raw->addr == ((mode & FI_MR_VIRT_ADDR) ? reinterpret_cast<std::uintptr_t>(buffer.data()) +
                                                       (interface == registration::dmabuf ? 8 : 0)
                                                 : 0));
    CHECK(raw->length == 64);
    CHECK(raw->key.size() == provider.bytes.size() - provider.shrink);
    CHECK(std::ranges::equal(raw->key, std::span{provider.bytes}.first(raw->key.size())));
    provider.bytes.assign(provider.bytes.size(), std::byte{0xcc});
    region = {};
    CHECK(provider.live_regions == 0);
    if (!raw->key.empty())
        CHECK(raw->key.front() == std::byte{0x52});
    raw = std::unexpected(loom::make_error_code(loom::errc::io_error));
    CHECK(resource.live == 0);
}

TEST_CASE("raw mapping retains metadata until unmapping", "[memory][raw]") {
    const auto key = GENERATE(std::uint64_t{0},
                              std::uint64_t{0xabcdef0123456789},
                              std::numeric_limits<std::uint64_t>::max());
    raw_provider provider;
    provider.next_key = key;
    std::array<std::byte, 64> buffer{};
    auto region = provider.region(buffer);
    auto raw = region.export_raw().value();
    const auto expected_bytes = provider.bytes;
    const auto expected_address = raw.addr;
    auto mapping = loom::mapped_remote_memory::map(provider.dom, raw);
    REQUIRE(mapping);
    REQUIRE(*mapping);
    raw.key.assign(raw.key.size(), std::byte{0xcc});
    CHECK(provider.mapped_base == provider.base);
    CHECK(provider.mapped_bytes == expected_bytes);
    CHECK(std::memcmp(provider.retained_bytes, expected_bytes.data(), expected_bytes.size()) == 0);
    auto remote = mapping->remote();
    REQUIRE(remote);
    CHECK(remote->key == key);
    CHECK(remote->addr == expected_address);
    CHECK(remote->length == buffer.size());
    auto moved = std::move(*mapping);
    CHECK_FALSE(*mapping);
    CHECK_FALSE(mapping->remote());
    CHECK(provider.unmaps == 0);
    REQUIRE(moved.close());
    CHECK_FALSE(moved);
    CHECK_FALSE(moved.remote());
    CHECK(provider.active.empty());
    CHECK(provider.unmaps == 1);
    REQUIRE(moved.close());
    CHECK(provider.unmaps == 1);
}

TEST_CASE("mapping move assignment exchanges live ownership", "[memory][raw]") {
    raw_provider provider;
    std::array<std::byte, 64> buffer{};
    auto region = provider.region(buffer);
    auto raw = region.export_raw().value();
    counted_resource first_resource, second_resource;
    {
        auto first = loom::mapped_remote_memory::map(provider.dom, raw, &first_resource).value();
        auto second = loom::mapped_remote_memory::map(provider.dom, raw, &second_resource).value();
        const auto first_key = first.remote()->key;
        const auto second_key = second.remote()->key;
        first = std::move(second);
        CHECK(first.remote()->key == second_key);
        CHECK(second.remote()->key == first_key);
        CHECK(provider.unmaps == 0);
        auto& same = first;
        first = std::move(same);
        CHECK(first.remote()->key == second_key);
    }
    CHECK(provider.unmaps == 2);
    CHECK(provider.active.empty());
    CHECK(first_resource.live == 0);
    CHECK(second_resource.live == 0);
}

TEST_CASE("raw control failures preserve errors and mapping ownership", "[memory][raw]") {
    raw_provider provider;
    std::array<std::byte, 64> buffer{};
    auto region = provider.region(buffer);
    SECTION("probe failure") {
        provider.probe_error = -FI_EACCES;
        CHECK(region.export_raw().error() == loom::make_error_code_from_fi_errno(FI_EACCES));
        CHECK(provider.exports == 1);
    }
    SECTION("read failure") {
        provider.export_error = -FI_EAGAIN;
        CHECK(region.export_raw().error() == loom::errc::again);
    }
    SECTION("changed key size") {
        provider.export_error = -FI_ETOOSMALL;
        CHECK(region.export_raw().error() == loom::make_error_code_from_fi_errno(FI_ETOOSMALL));
    }
    SECTION("invalid output size") {
        provider.oversized = true;
        CHECK(region.export_raw().error() == loom::errc::io_error);
    }
    SECTION("map failure") {
        provider.map_error = -FI_ENOMEM;
        CHECK(loom::mapped_remote_memory::map(provider.dom, region.export_raw().value()).error() ==
              loom::errc::no_memory);
        CHECK(provider.unmaps == 0);
        CHECK(provider.active.empty());
    }
    SECTION("unmap failure can be retried") {
        auto mapping =
            loom::mapped_remote_memory::map(provider.dom, region.export_raw().value()).value();
        const auto key = mapping.remote()->key;
        provider.unmap_error = -FI_EBUSY;
        CHECK(mapping.close().error() == loom::errc::busy);
        CHECK(mapping);
        CHECK(mapping.remote()->key == key);
        CHECK(provider.active.contains(key));
        provider.unmap_error = 0;
        REQUIRE(mapping.close());
        CHECK(provider.active.empty());
        CHECK(provider.unmaps == 2);
    }
}

TEST_CASE("raw setup allocates before acquiring a native mapping", "[memory][raw]") {
    raw_provider provider;
    std::array<std::byte, 64> buffer{};
    auto region = provider.region(buffer);
    counted_resource resource;
    resource.remaining = 0;
    CHECK_THROWS_AS(region.export_raw(&resource), std::bad_alloc);
    CHECK(resource.live == 0);
    CHECK(provider.exports == 1);
    auto raw = region.export_raw().value();
    CHECK_THROWS_AS(loom::mapped_remote_memory::map(provider.dom, raw, &resource), std::bad_alloc);
    CHECK(provider.maps == 0);
    CHECK(resource.live == 0);
}

TEST_CASE("raw metadata rejects invalid ranges and unavailable controls", "[memory][raw]") {
    enum class missing { table, prefix, function };
    const auto absent = GENERATE(missing::table, missing::prefix, missing::function);
    raw_provider provider;
    std::array<std::byte, 64> buffer{};
    auto region = provider.region(buffer);
    auto raw = region.export_raw().value();
    auto* native = static_cast<fid_mr*>(region.internal_ptr());
    if (absent == missing::table) {
        native->fid.ops = nullptr;
        provider.handle.fid.ops = nullptr;
    } else if (absent == missing::prefix) {
        provider.region_ops.size = offsetof(fi_ops, control);
        provider.domain_ops.size = offsetof(fi_ops, control);
    } else {
        provider.region_ops.control = nullptr;
        provider.domain_ops.control = nullptr;
    }
    const auto exported = region.export_raw();
    const auto mapped = loom::mapped_remote_memory::map(provider.dom, raw);
    native->fid.ops = &provider.region_ops;
    provider.handle.fid.ops = &provider.domain_ops;
    REQUIRE_FALSE(exported);
    CHECK(exported.error() == loom::errc::not_supported);
    REQUIRE_FALSE(mapped);
    CHECK(mapped.error() == loom::errc::not_supported);
    CHECK(provider.exports == 2);
    CHECK(provider.maps == 0);
    loom::domain empty_domain;
    CHECK(loom::mapped_remote_memory::map(empty_domain, raw).error() ==
          loom::errc::invalid_argument);
    loom::memory_region empty_region;
    CHECK(empty_region.export_raw().error() == loom::errc::invalid_argument);
    raw.addr = std::numeric_limits<std::uint64_t>::max();
    raw.length = 1;
    CHECK(loom::mapped_remote_memory::map(provider.dom, raw).error() ==
          loom::errc::invalid_argument);
    loom::mapped_remote_memory empty_mapping;
    CHECK_FALSE(empty_mapping);
    CHECK(empty_mapping.remote().error() == loom::errc::invalid_argument);
    CHECK(empty_mapping.close());
}

TEST_CASE("mapped raw keys reach RMA and survive domain wrapper moves", "[memory][raw][rma]") {
    raw_provider provider;
    std::optional<loom::domain> moved_domain;
    std::array<std::byte, 64> target{};
    std::array<std::byte, 8> source{};
    source.fill(std::byte{0x52});
    auto region = provider.region(target);
    auto mapping =
        loom::mapped_remote_memory::map(provider.dom, region.export_raw().value()).value();
    moved_domain.emplace(std::move(provider.dom));
    fi_ops ops{.size = sizeof(fi_ops), .close = [](fid*) { return 0; }};
    fi_ops_rma rma{
        .size = sizeof(fi_ops_rma),
        .writemsg = [](fid_ep* ep, const fi_msg_rma* message, std::uint64_t flags) -> ssize_t {
            auto& provider = *static_cast<raw_provider*>(ep->fid.context);
            if (flags || message->addr != 7 || message->iov_count != 1 ||
                message->rma_iov_count != 1 || !provider.active.contains(message->rma_iov[0].key))
                return -FI_EACCES;
            std::memcpy(reinterpret_cast<void*>(message->rma_iov[0].addr),
                        message->msg_iov[0].iov_base,
                        message->msg_iov[0].iov_len);
            return 0;
        },
    };
    fid_ep native{.fid = {.context = &provider, .ops = &ops}, .rma = &rma};
    auto endpoint = loom::endpoint{
        loom::detail::make_pmr_unique<loom::detail::endpoint_impl>(loom::get_default_resource(),
                                                                   &native,
                                                                   loom::endpoint_types::rdm,
                                                                   loom::address_format::inet)};
    auto remote = mapping.remote().value().subregion(13, source.size()).value();
    REQUIRE(loom::rma::write_to(endpoint,
                                std::span<const std::byte>{source},
                                loom::mr_descriptor{},
                                loom::fabric_addr{7},
                                remote,
                                loom::op_flag::none));
    CHECK(std::ranges::equal(source, std::span{target}.subspan(13, source.size())));
    CHECK(target[12] == std::byte{});
    CHECK(target[21] == std::byte{});
    REQUIRE(endpoint.close());
    REQUIRE(mapping.close());
    CHECK(provider.active.empty());
}
