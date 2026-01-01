// SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only
#include <loom/core/mr_cache.hpp>

#include "ut.hpp"

using namespace boost::ut;

suite mr_cache_suite = [] {
    "cache_traits alignment"_test = [] {
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
    };

    "cache_entry_base containment"_test = [] {
        loom::detail::mr_cache_entry_base entry{0x1000, 4096, loom::mr_access_flags::read};

        expect(entry.base_addr == 0x1000UL) << "base_addr mismatch";
        expect(entry.length == 4096UL) << "length mismatch";

        expect(entry.contains(0x1000, 100)) << "should contain 0x1000+100";
        expect(entry.contains(0x1500, 100)) << "should contain 0x1500+100";
        expect(entry.contains(0x1000, 4096)) << "should contain exact range";
        expect(!entry.contains(0x0F00, 100)) << "should not contain 0x0F00 (before start)";
        expect(!entry.contains(0x1000, 4097)) << "should not contain range extending past end";
    };

    "cache_entry_base overlap"_test = [] {
        loom::detail::mr_cache_entry_base entry{0x1000, 4096, loom::mr_access_flags::read};

        expect(entry.overlaps(0x0F00, 257)) << "should overlap (starts before, extends into)";

        expect(entry.overlaps(0x1F00, 512)) << "should overlap (starts inside, extends past)";

        expect(entry.overlaps(0x0800, 8192)) << "should overlap (completely contains)";

        expect(entry.overlaps(0x1500, 256)) << "should overlap (completely inside)";

        expect(!entry.overlaps(0x0500, 256)) << "should not overlap (completely before)";

        expect(!entry.overlaps(0x0F00, 256)) << "should not overlap (adjacent before)";

        expect(!entry.overlaps(0x2000, 256)) << "should not overlap (adjacent after)";

        expect(!entry.overlaps(0x3000, 256)) << "should not overlap (completely after)";
    };

    "handle default state"_test = [] {
        loom::mr_handle<loom::provider::verbs_tag> handle;

        expect(!handle.valid()) << "default handle should not be valid";
        expect(!handle) << "default handle should be false";
        expect(handle.mr() == nullptr) << "default handle mr() should be nullptr";
        expect(handle.key() == 0UL) << "default handle key() should be 0";
        expect(!handle.descriptor()) << "default handle descriptor() should be empty";
        expect(handle.base_address() == nullptr)
            << "default handle base_address() should be nullptr";
        expect(handle.length() == 0UL) << "default handle length() should be 0";
        expect(handle.refcount() == 0UL) << "default handle refcount() should be 0";

        auto remote = handle.to_remote_memory();
        expect(remote.addr == 0UL);
        expect(remote.key == 0UL);
        expect(remote.length == 0UL);
    };

    "handle copy and move"_test = [] {
        loom::mr_handle<loom::provider::efa_tag> h1;
        loom::mr_handle<loom::provider::efa_tag> h2 = h1;
        loom::mr_handle<loom::provider::efa_tag> h3 = std::move(h1);

        expect(!h2.valid()) << "copies of invalid handle should be invalid";
        expect(!h3.valid()) << "moved-from invalid handle should be invalid";

        h1 = h2;
        h1 = std::move(h3);

        expect(!h1.valid()) << "assigned handle should still be invalid";
    };

    "stats default"_test = [] {
        loom::mr_cache_stats stats;

        expect(stats.hits == 0UL);
        expect(stats.misses == 0UL);
        expect(stats.registrations == 0UL);
        expect(stats.evictions == 0UL);
        expect(stats.current_entries == 0UL);
        expect(stats.total_registered_bytes == 0UL);
    };

    "mr_cacheable concept"_test = [] {
        static_assert(loom::mr_cacheable<loom::mr_cache<loom::provider::verbs_tag>>);
        static_assert(loom::mr_cacheable<loom::mr_cache<loom::provider::efa_tag>>);
        static_assert(loom::mr_cacheable<loom::mr_cache<loom::provider::slingshot_tag>>);
        static_assert(loom::mr_cacheable<loom::mr_cache<loom::provider::tcp_tag>>);
        static_assert(loom::mr_cacheable<loom::mr_cache<loom::provider::shm_tag>>);
    };

    "provider specific traits"_test = [] {
        using verbs_traits = loom::mr_cache_traits<loom::provider::verbs_tag>;
        using efa_traits = loom::mr_cache_traits<loom::provider::efa_tag>;
        using cxi_traits = loom::mr_cache_traits<loom::provider::slingshot_tag>;

        static_assert(verbs_traits::page_size == 4096);
        static_assert(efa_traits::page_size == 4096);
        static_assert(cxi_traits::page_size == 4096);
    };

    "alignment edge cases"_test = [] {
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
    };
};

auto main() -> int {}
