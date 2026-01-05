// SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only
#include <loom/core/mr_cache.hpp>

#include <catch2/catch_test_macros.hpp>

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
