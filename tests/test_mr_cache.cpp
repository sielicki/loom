// SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only
#include <loom/core/mr_cache.hpp>

#include <thread>
#include <vector>

#include "mock_domain.hpp"
#include <catch2/catch_test_macros.hpp>

TEST_CASE("concurrent cache hits retain exact statistics", "[mr_cache]") {
    using namespace loom;
    test::mock_domain provider;
    mr_cache<provider::tcp_tag> cache(provider.dom);
    alignas(4096) std::array<std::byte, 4096> buffer{};
    auto registration = cache.lookup(buffer, mr_access_flags::send);
    REQUIRE(registration);
    {
        std::vector<std::jthread> threads;
        for (int i = 0; i < 4; ++i) {
            threads.emplace_back([&] {
                for (int j = 0; j < 1000; ++j) {
                    auto handle = cache.lookup(buffer, mr_access_flags::send);
                }
            });
        }
    }
    REQUIRE(cache.stats().hits == 4000);
    REQUIRE(provider.registrations == 1);
    REQUIRE(registration->refcount() == 1);
}

TEST_CASE("cache retains registrations sharing a page", "[mr_cache]") {
    using namespace loom;
    test::mock_domain provider;
    mr_cache<provider::tcp_tag> cache(provider.dom);
    alignas(4096) std::array<std::byte, 8192> buffer{};

    auto first = cache.lookup(buffer.data(), 32, mr_access_flags::send);
    REQUIRE(first);

    SECTION("larger range") {
        auto larger = cache.lookup(buffer, mr_access_flags::send);
        REQUIRE(larger);
        REQUIRE(larger->length() == buffer.size());
        REQUIRE(larger->key() != first->key());
    }
    SECTION("different permissions") {
        auto receiver = cache.lookup(buffer.data(), 32, mr_access_flags::recv);
        REQUIRE(receiver);
        REQUIRE(receiver->key() != first->key());
        auto sender = cache.lookup(buffer.data(), 32, mr_access_flags::send);
        REQUIRE(sender);
        REQUIRE(sender->key() == first->key());
    }
    SECTION("DMA buffer") {
        auto dma = cache.lookup_dmabuf(buffer.data(), 32, mr_access_flags::send, 1);
        REQUIRE(dma);
        REQUIRE(dma->key() != first->key());
        auto host = cache.lookup(buffer.data(), 32, mr_access_flags::send);
        REQUIRE(host);
        REQUIRE(host->key() == first->key());
    }
    SECTION("heterogeneous memory") {
        auto device =
            cache.lookup_hmem(buffer.data(), 32, mr_access_flags::send, hmem_device::cuda(0));
        REQUIRE(device);
        REQUIRE(device->key() != first->key());
        auto host = cache.lookup(buffer.data(), 32, mr_access_flags::send);
        REQUIRE(host);
        REQUIRE(host->key() == first->key());
    }

    REQUIRE(first->length() == 4096);
    REQUIRE(first->refcount() == 1);
    REQUIRE(cache.size() == 2);
    REQUIRE(provider.live_regions == 2);
}

TEST_CASE("cache finds an earlier overlapping registration", "[mr_cache]") {
    using namespace loom;
    test::mock_domain provider;
    mr_cache<provider::tcp_tag> cache(provider.dom);
    alignas(4096) std::array<std::byte, 8192> buffer{};

    auto large = cache.lookup(buffer, mr_access_flags::send);
    auto small = cache.lookup(buffer.data() + 4096, 32, mr_access_flags::recv);
    REQUIRE(large);
    REQUIRE(small);
    auto found = cache.lookup(buffer.data() + 4096, 4096, mr_access_flags::send);
    REQUIRE(found);
    REQUIRE(found->key() == large->key());
    REQUIRE(provider.registrations == 2);
}

TEST_CASE("device registrations retain their exact range", "[mr_cache]") {
    using namespace loom;
    test::mock_domain provider;
    mr_cache<provider::tcp_tag> cache(provider.dom);
    alignas(4096) std::array<std::byte, 4096> buffer{};
    auto* address = buffer.data() + 17;
    SECTION("DMA buffer") {
        auto handle = cache.lookup_dmabuf(address, 31, mr_access_flags::send, 1, 17);
        REQUIRE(handle);
        REQUIRE(handle->mr()->address() == address);
        REQUIRE(handle->mr()->length() == 31);
    }
    SECTION("device allocation") {
        auto handle = cache.lookup_hmem(address, 31, mr_access_flags::send, hmem_device::cuda(2));
        REQUIRE(handle);
        REQUIRE(handle->mr()->address() == address);
        REQUIRE(handle->mr()->length() == 31);
    }
    REQUIRE(cache.stats().total_registered_bytes == 31);
}

TEST_CASE("clearing a cache preserves live handles", "[mr_cache]") {
    using namespace loom;
    test::mock_domain provider;
    mr_cache<provider::tcp_tag> cache(provider.dom);
    alignas(4096) std::array<std::byte, 8192> buffer{};

    {
        auto live = cache.lookup(buffer.data(), 32, mr_access_flags::send);
        REQUIRE(live);
        {
            auto unused = cache.lookup(buffer.data() + 4096, 32, mr_access_flags::send);
            REQUIRE(unused);
        }
        cache.clear();
        REQUIRE(live->refcount() == 1);
        REQUIRE(live->mr()->address() == buffer.data());
        REQUIRE(provider.live_regions == 1);
        REQUIRE(cache.size() == 1);
        REQUIRE(cache.stats().current_entries == 1);
        REQUIRE(cache.stats().total_registered_bytes == 4096);
        REQUIRE(cache.stats().evictions == 1);
    }

    cache.clear();
    REQUIRE(provider.live_regions == 0);
    REQUIRE(cache.size() == 0);
    REQUIRE(cache.stats().current_entries == 0);
    REQUIRE(cache.stats().total_registered_bytes == 0);
    REQUIRE(cache.stats().evictions == 2);
}

TEST_CASE("cache_traits alignment", "[mr_cache]") {
    using traits = loom::mr_cache_traits<loom::provider::verbs_tag>;

    static_assert(traits::page_size == 4096);

    constexpr auto addr1 = std::uintptr_t{0x1000};
    constexpr auto addr2 = std::uintptr_t{0x1234};
    constexpr auto addr3 = std::uintptr_t{0x1FFF};

    static_assert(traits::align_down(addr1) == 0x1000);
    static_assert(traits::align_down(addr2) == 0x1000);
    static_assert(traits::align_down(addr3) == 0x1000);

    static_assert(traits::align_up(addr1) == 0x1000);
    static_assert(traits::align_up(addr2) == 0x2000);
    static_assert(traits::align_up(addr3) == 0x2000);

    constexpr auto len1 = traits::aligned_length(0x1000, 100);
    static_assert(len1 == 4096);

    constexpr auto len2 = traits::aligned_length(0x1234, 100);
    static_assert(len2 == 4096);

    constexpr auto len3 = traits::aligned_length(0x1F00, 512);
    static_assert(len3 == 8192);
}

TEST_CASE("cache_entry_base containment", "[mr_cache]") {
    loom::detail::mr_cache_entry_base entry{0x1000, 4096, loom::mr_access_flags::read};

    REQUIRE(entry.base_addr == 0x1000UL);
    REQUIRE(entry.length == 4096UL);

    REQUIRE(entry.contains(0x1000, 100));
    REQUIRE(entry.contains(0x1500, 100));
    REQUIRE(entry.contains(0x1000, 4096));
    REQUIRE_FALSE(entry.contains(0x0F00, 100));
    REQUIRE_FALSE(entry.contains(0x1000, 4097));
}

TEST_CASE("cache_entry_base overlap", "[mr_cache]") {
    loom::detail::mr_cache_entry_base entry{0x1000, 4096, loom::mr_access_flags::read};

    REQUIRE(entry.overlaps(0x0F00, 257));
    REQUIRE(entry.overlaps(0x1F00, 512));
    REQUIRE(entry.overlaps(0x0800, 8192));
    REQUIRE(entry.overlaps(0x1500, 256));
    REQUIRE_FALSE(entry.overlaps(0x0500, 256));
    REQUIRE_FALSE(entry.overlaps(0x0F00, 256));
    REQUIRE_FALSE(entry.overlaps(0x2000, 256));
    REQUIRE_FALSE(entry.overlaps(0x3000, 256));
}

TEST_CASE("handle default state", "[mr_cache]") {
    loom::mr_handle<loom::provider::verbs_tag> handle;

    REQUIRE_FALSE(handle.valid());
    REQUIRE_FALSE(handle);
    REQUIRE(handle.mr() == nullptr);
    REQUIRE(handle.key() == 0UL);
    REQUIRE_FALSE(handle.descriptor());
    REQUIRE(handle.base_address() == nullptr);
    REQUIRE(handle.length() == 0UL);
    REQUIRE(handle.refcount() == 0UL);

    auto remote = handle.to_remote_memory();
    REQUIRE(remote.addr == 0UL);
    REQUIRE(remote.key == 0UL);
    REQUIRE(remote.length == 0UL);
}

TEST_CASE("handle copy and move", "[mr_cache]") {
    loom::mr_handle<loom::provider::efa_tag> h1;
    loom::mr_handle<loom::provider::efa_tag> h2 = h1;
    loom::mr_handle<loom::provider::efa_tag> h3 = std::move(h1);

    REQUIRE_FALSE(h2.valid());
    REQUIRE_FALSE(h3.valid());

    h1 = h2;
    h1 = std::move(h3);

    REQUIRE_FALSE(h1.valid());
}

TEST_CASE("stats default", "[mr_cache]") {
    loom::mr_cache_stats stats;

    REQUIRE(stats.hits == 0UL);
    REQUIRE(stats.misses == 0UL);
    REQUIRE(stats.registrations == 0UL);
    REQUIRE(stats.evictions == 0UL);
    REQUIRE(stats.current_entries == 0UL);
    REQUIRE(stats.total_registered_bytes == 0UL);
}

TEST_CASE("mr_cacheable concept", "[mr_cache]") {
    static_assert(loom::mr_cacheable<loom::mr_cache<loom::provider::verbs_tag>>);
    static_assert(loom::mr_cacheable<loom::mr_cache<loom::provider::efa_tag>>);
    static_assert(loom::mr_cacheable<loom::mr_cache<loom::provider::slingshot_tag>>);
    static_assert(loom::mr_cacheable<loom::mr_cache<loom::provider::tcp_tag>>);
    static_assert(loom::mr_cacheable<loom::mr_cache<loom::provider::shm_tag>>);
}

TEST_CASE("provider specific traits", "[mr_cache]") {
    using verbs_traits = loom::mr_cache_traits<loom::provider::verbs_tag>;
    using efa_traits = loom::mr_cache_traits<loom::provider::efa_tag>;
    using cxi_traits = loom::mr_cache_traits<loom::provider::slingshot_tag>;

    static_assert(verbs_traits::page_size == 4096);
    static_assert(efa_traits::page_size == 4096);
    static_assert(cxi_traits::page_size == 4096);
}

TEST_CASE("alignment edge cases", "[mr_cache]") {
    using traits = loom::mr_cache_traits<loom::provider::verbs_tag>;

    static_assert(traits::align_down(0) == 0);
    static_assert(traits::align_up(0) == 0);

    static_assert(traits::align_down(1) == 0);
    static_assert(traits::align_up(1) == 4096);

    static_assert(traits::align_down(4095) == 0);
    static_assert(traits::align_up(4095) == 4096);

    static_assert(traits::align_down(4096) == 4096);
    static_assert(traits::align_up(4096) == 4096);

    static_assert(traits::align_down(4097) == 4096);
    static_assert(traits::align_up(4097) == 8192);

    constexpr std::uintptr_t large_addr = 0xFFFF'FFFF'FFFF'F000ULL;
    static_assert(traits::align_down(large_addr) == large_addr);

    static_assert(traits::aligned_length(0, 1) == 4096);
    static_assert(traits::aligned_length(0, 4096) == 4096);
    static_assert(traits::aligned_length(0, 4097) == 8192);
    static_assert(traits::aligned_length(1, 4095) == 4096);
    static_assert(traits::aligned_length(1, 4096) == 8192);
}
