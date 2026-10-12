// SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only
#include <loom/core/fabric.hpp>
#include <loom/core/mr_cache.hpp>

#include <barrier>
#include <numeric>
#include <random>
#include <thread>
#include <unordered_map>
#include <vector>

#include "mock_domain.hpp"
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

TEST_CASE("endpoint-bound caches require an endpoint before registering",
          "[mr_cache][endpoint_mr]") {
    using namespace loom;
    test::mock_domain provider{FI_MR_ENDPOINT};
    mr_cache<provider::tcp_tag> cache{provider.dom};
    alignas(4096) std::array<std::byte, 4096> buffer{};
    const auto operation = GENERATE(0, 1, 2);
    auto handle =
        operation == 0 ? cache.lookup(buffer, mr_access_flags::send)
        : operation == 1
            ? cache.lookup_hmem(
                  buffer.data(), buffer.size(), mr_access_flags::send, hmem_device::cuda(0))
            : cache.lookup_dmabuf(buffer.data(), buffer.size(), mr_access_flags::send, 7);
    CHECK_FALSE(handle);
    CHECK(provider.registrations == 0);
    CHECK(cache.size() == 0);
}

TEST_CASE("cache rejects wrapping ranges before lookup or registration", "[mr_cache]") {
    using namespace loom;
    test::mock_domain provider;
    mr_cache<provider::tcp_tag> cache(provider.dom);
    alignas(4096) std::array<std::byte, 4096> buffer{};
    auto existing = cache.lookup(buffer, mr_access_flags::send);
    REQUIRE(existing);
    const auto maximum = std::numeric_limits<std::uintptr_t>::max();
    REQUIRE_FALSE(cache.lookup(buffer.data(), maximum, mr_access_flags::send));
    auto* last_byte = std::bit_cast<void*>(maximum);
    REQUIRE_FALSE(cache.lookup(last_byte, 1, mr_access_flags::send));
    REQUIRE_FALSE(cache.lookup_hmem(last_byte, 2, mr_access_flags::send, hmem_device::cuda(0)));
    REQUIRE_FALSE(cache.lookup_dmabuf(last_byte, 2, mr_access_flags::send, 7, 0));
    REQUIRE(provider.registrations == 1);
    REQUIRE(cache.stats().hits == 0);
    REQUIRE_FALSE(cache.lookup(buffer.data(), 0, mr_access_flags::send));
}

TEST_CASE("cache range comparisons do not wrap", "[mr_cache]") {
    const auto maximum = std::numeric_limits<std::uintptr_t>::max();
    loom::detail::mr_cache_entry_base entry{4096, 4096, loom::mr_access_flags::send};
    REQUIRE_FALSE(entry.contains(4097, maximum));
    REQUIRE_FALSE(entry.overlaps(4097, 0));
    REQUIRE(entry.overlaps(4095, maximum));
    REQUIRE_FALSE(entry.overlaps(maximum - 2, 1));
}

TEST_CASE("concurrent cache hits retain exact statistics", "[mr_cache]") {
    using namespace loom;
    enum class memory { host, hmem, dmabuf };
    const auto kind = GENERATE(memory::host, memory::hmem, memory::dmabuf);
    test::mock_domain provider;
    mr_cache<provider::tcp_tag> cache(provider.dom);
    alignas(4096) std::array<std::byte, 4096> buffer{};
    const auto lookup = [&]() {
        switch (kind) {
            case memory::host:
                return cache.lookup(buffer, mr_access_flags::send);
            case memory::hmem:
                return cache.lookup_hmem(
                    buffer.data(), buffer.size(), mr_access_flags::send, hmem_device::cuda(0));
            case memory::dmabuf:
                return cache.lookup_dmabuf(buffer.data(), buffer.size(), mr_access_flags::send, 7);
        }
        std::unreachable();
    };
    auto registration = lookup();
    REQUIRE(registration);
    {
        std::vector<std::jthread> threads;
        for (int i = 0; i < 4; ++i) {
            threads.emplace_back([&] {
                for (int j = 0; j < 1000; ++j) {
                    auto handle = lookup();
                }
            });
        }
    }
    REQUIRE(cache.stats().hits == 4000);
    REQUIRE(provider.registrations == 1);
    REQUIRE(registration->refcount() == 1);
}

TEST_CASE("concurrent cache misses publish one registration", "[mr_cache][concurrent]") {
    using namespace loom;
    enum class memory { host, hmem, dmabuf };
    const auto kind = GENERATE(memory::host, memory::hmem, memory::dmabuf);
    test::mock_domain provider;
    mr_cache<provider::tcp_tag> cache{provider.dom};
    alignas(4096) std::array<std::byte, 4096> buffer{};
    constexpr std::size_t count = 16;
    std::array<mr_handle<provider::tcp_tag>, count> handles;
    std::barrier ready{static_cast<std::ptrdiff_t>(count)};
    std::atomic<bool> failed{};
    {
        std::vector<std::jthread> threads;
        for (std::size_t index = 0; index < count; ++index) {
            threads.emplace_back([&, index] {
                ready.arrive_and_wait();
                auto result = [&] {
                    switch (kind) {
                        case memory::host:
                            return cache.lookup(buffer, mr_access_flags::send);
                        case memory::hmem:
                            return cache.lookup_hmem(buffer.data(),
                                                     buffer.size(),
                                                     mr_access_flags::send,
                                                     hmem_device::cuda(0));
                        case memory::dmabuf:
                            return cache.lookup_dmabuf(
                                buffer.data(), buffer.size(), mr_access_flags::send, 7);
                    }
                    std::unreachable();
                }();
                if (result)
                    handles[index] = std::move(*result);
                else
                    failed.store(true);
            });
        }
    }
    REQUIRE_FALSE(failed.load());
    REQUIRE(provider.registrations == 1);
    REQUIRE(cache.stats().misses == 1);
    REQUIRE(cache.stats().hits == count - 1);
    REQUIRE(cache.hit_rate() == static_cast<double>(count - 1) / count);
    for (const auto& handle : handles) {
        REQUIRE(handle.key() == handles[0].key());
        REQUIRE(handle.refcount() == count);
    }
}

TEST_CASE("cache reuse and release race invalidation without losing registrations",
          "[mr_cache][concurrent]") {
    using namespace loom;
    enum class memory { host, hmem, dmabuf };
    const auto kind = GENERATE(memory::host, memory::hmem, memory::dmabuf);
    test::mock_domain provider;
    mr_cache<provider::tcp_tag> cache{provider.dom};
    alignas(4096) std::array<std::byte, 32768> buffer{};
    constexpr int workers = 4, iterations = 2000;
    std::barrier ready{workers + 1};
    std::atomic<bool> failed{};
    {
        std::vector<std::jthread> threads;
        for (int worker = 0; worker < workers; ++worker) {
            threads.emplace_back([&, worker] {
                ready.arrive_and_wait();
                for (int iteration = 0; iteration < iterations; ++iteration) {
                    const auto offset = ((iteration + worker) % 8) * 4096;
                    auto result = [&] {
                        switch (kind) {
                            case memory::host:
                                return cache.lookup(
                                    buffer.data() + offset, 32, mr_access_flags::send);
                            case memory::hmem:
                                return cache.lookup_hmem(buffer.data() + offset,
                                                         32,
                                                         mr_access_flags::send,
                                                         hmem_device::cuda(0));
                            case memory::dmabuf:
                                return cache.lookup_dmabuf(
                                    buffer.data(), 32, mr_access_flags::send, 7, offset);
                        }
                        std::unreachable();
                    }();
                    if (!result) {
                        failed.store(true);
                        continue;
                    }
                    auto copy = *result;
                    const auto key = copy.key();
                    // The native registration must remain alive while either
                    // handle exists, even if its range was just invalidated.
                    const std::lock_guard lock{provider.mutex};
                    if (!key || !provider.keys.contains(*key))
                        failed.store(true);
                }
            });
        }
        threads.emplace_back([&] {
            ready.arrive_and_wait();
            for (int iteration = 0; iteration < iterations; ++iteration) {
                cache.invalidate(buffer.data(), buffer.size());
                cache.evict_unreferenced();
            }
        });
    }
    REQUIRE_FALSE(failed.load());
    const auto stats = cache.stats();
    REQUIRE(stats.hits + stats.misses == workers * iterations);
    REQUIRE(stats.registrations == stats.misses);
    REQUIRE(provider.registrations == stats.registrations);
    cache.clear();
    REQUIRE(cache.size() == 0);
    REQUIRE(provider.live_regions == 0);
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

TEST_CASE("cache range indexing survives varied insertion and removal orders",
          "[mr_cache][index]") {
    using namespace loom;
    enum class order { ascending, descending, shuffled };
    const auto insertion = GENERATE(order::ascending, order::descending, order::shuffled);
    const auto removal = GENERATE(order::ascending, order::descending, order::shuffled);
    constexpr std::size_t count = 2048;
    test::mock_domain provider;
    mr_cache<provider::tcp_tag> cache{provider.dom};
    std::vector<std::size_t> indices(count);
    std::iota(indices.begin(), indices.end(), 0);
    auto arrange = [&](order selected) {
        std::ranges::sort(indices);
        if (selected == order::descending)
            std::ranges::reverse(indices);
        else if (selected == order::shuffled)
            std::shuffle(indices.begin(), indices.end(), std::mt19937{1729});
    };
    const auto address = [](std::size_t index) {
        return reinterpret_cast<void*>((2 * index + 1) * 4096);
    };
    std::vector<mr_handle<provider::tcp_tag>> handles(count);
    arrange(insertion);
    for (const auto index : indices) {
        auto handle = cache.lookup(address(index), 4096, mr_access_flags::send);
        REQUIRE(handle);
        handles[index] = std::move(*handle);
    }
    REQUIRE(provider.registrations == count);
    arrange(removal);
    for (const auto index : indices) {
        const auto key = handles[index].key();
        REQUIRE(cache.lookup(address(index), 4096, mr_access_flags::send)->key() == key);
        cache.invalidate(address(index), 4096);
        handles[index] = {};
        // Reinserting a deleted range must not resurrect its retired MR or
        // lose another subtree while replacing a node with two children.
        auto replacement = cache.lookup(address(index), 4096, mr_access_flags::send);
        REQUIRE(replacement);
        REQUIRE(replacement->key() != key);
        handles[index] = std::move(*replacement);
    }
    REQUIRE(cache.size() == count);
    REQUIRE(provider.registrations == 2 * count);
    for (const auto index : indices)
        REQUIRE(cache.lookup(address(index), 4096, mr_access_flags::send)->key() ==
                handles[index].key());
    // Erase every reusable node without immediately replacing it.
    for (const auto index : indices) {
        cache.invalidate(address(index), 4096);
        handles[index] = {};
    }
    REQUIRE(cache.size() == 0);
    REQUIRE(provider.live_regions == 0);
}

TEST_CASE("cache interval reuse matches a linear ownership model", "[mr_cache][index]") {
    using namespace loom;
    enum class memory { host, hmem, mapped, unmapped };
    struct request {
        memory kind;
        std::uintptr_t base;
        std::size_t length;
        mr_access access;
        int device_or_fd;
        std::uint64_t offset;
    };
    struct record {
        request range;
        std::uint64_t key;
        bool reusable{true};
        bool alive{true};
        mr_handle<provider::tcp_tag> handle;
    };
    test::mock_domain provider;
    mr_cache<provider::tcp_tag> cache{provider.dom};
    std::vector<record> model;
    std::mt19937 random{GENERATE(7U, 1729U, 65537U)};
    constexpr std::uintptr_t allocation = 0x10000000;
    const auto contains =
        [](std::uint64_t base, std::size_t size, std::uint64_t start, std::size_t length) {
            return start >= base && start - base <= size && length <= size - (start - base);
        };
    const auto overlaps =
        [](std::uintptr_t base, std::size_t size, std::uintptr_t start, std::size_t length) {
            return start < base ? base - start < length : start - base < size;
        };
    const auto lookup = [&](request requested) {
        auto result = [&]() -> loom::result<mr_handle<provider::tcp_tag>> {
            auto* address = reinterpret_cast<void*>(requested.base);
            switch (requested.kind) {
                case memory::host:
                    return cache.lookup(address, requested.length, requested.access);
                case memory::hmem:
                    return cache.lookup_hmem(address,
                                             requested.length,
                                             requested.access,
                                             hmem_device::cuda(requested.device_or_fd));
                case memory::mapped:
                case memory::unmapped:
                    return cache.lookup_dmabuf(requested.kind == memory::mapped
                                                   ? reinterpret_cast<void*>(allocation)
                                                   : nullptr,
                                               requested.length,
                                               requested.access,
                                               requested.device_or_fd,
                                               requested.offset);
            }
            std::unreachable();
        }();
        REQUIRE(result);
        const auto key = result->key();
        REQUIRE(key);
        const auto matching = [&](const record& cached) {
            const auto& range = cached.range;
            return cached.alive && cached.reusable && range.kind == requested.kind &&
                   range.device_or_fd == requested.device_or_fd &&
                   range.access.has(requested.access) &&
                   contains(range.base, range.length, requested.base, requested.length) &&
                   ((range.kind != memory::mapped && range.kind != memory::unmapped) ||
                    contains(range.offset, range.length, requested.offset, requested.length));
        };
        if (std::ranges::any_of(model, matching)) {
            REQUIRE(std::ranges::any_of(model, [&](const record& cached) {
                return matching(cached) && cached.key == *key;
            }));
        } else {
            REQUIRE(provider.registrations == model.size() + 1);
            model.push_back({requested, *key, true, true, std::move(*result)});
        }
        REQUIRE(provider.registrations == model.size());
    };
    for (std::size_t index = 0; index < 512; ++index)
        lookup({memory::host, allocation + index * 8192, 4096, mr_access_flags::send, 0, 0});

    for (int iteration = 0; iteration < 3000; ++iteration) {
        const auto action = random() % 10;
        if (action < 7) {
            const auto kind = static_cast<memory>(random() % 4);
            const auto offset = static_cast<std::uint64_t>(random() % 512) * 4096;
            const std::size_t length = (1 + random() % 64) * 4096;
            const std::array permissions{mr_access_flags::send,
                                         mr_access_flags::recv,
                                         mr_access_flags::send | mr_access_flags::recv};
            lookup({kind,
                    kind == memory::unmapped ? 0 : allocation + offset,
                    length,
                    permissions[random() % permissions.size()],
                    kind == memory::host ? 0 : static_cast<int>(random() % 3),
                    offset});
        } else if (action == 7) {
            const auto start = random() % 8 == 0 ? 0 : allocation + (random() % 512) * 4096;
            const std::size_t length = (1 + random() % 4) * 4096;
            cache.invalidate(reinterpret_cast<void*>(start), length);
            for (auto& cached : model) {
                if (cached.alive &&
                    overlaps(cached.range.base, cached.range.length, start, length)) {
                    cached.reusable = false;
                    if (!cached.handle)
                        cached.alive = false;
                }
            }
        } else if (action == 8) {
            auto& cached = model[random() % model.size()];
            cached.handle = {};
            if (!cached.reusable)
                cached.alive = false;
        } else {
            cache.evict_unreferenced();
            for (auto& cached : model)
                if (!cached.handle)
                    cached.alive = false;
        }
        std::size_t entries{}, bytes{};
        for (const auto& cached : model) {
            if (cached.alive) {
                ++entries;
                bytes += cached.range.length;
            }
        }
        REQUIRE(cache.size() == entries);
        REQUIRE(cache.stats().total_registered_bytes == bytes);
        REQUIRE(provider.live_regions == entries);
    }
    model.clear();
    cache.clear();
    REQUIRE(provider.live_regions == 0);
}

TEST_CASE("device registrations retain their exact range", "[mr_cache]") {
    using namespace loom;
    test::mock_domain provider;
    mr_cache<provider::tcp_tag> cache(provider.dom);
    alignas(4096) std::array<std::byte, 4096> buffer{};
    auto* address = buffer.data() + 17;
    SECTION("DMA buffer") {
        auto handle = cache.lookup_dmabuf(buffer.data(), 31, mr_access_flags::send, 1, 17);
        REQUIRE(handle);
        REQUIRE(provider.address == buffer.data());
        REQUIRE(provider.dmabuf_offset == 17);
        REQUIRE(handle->base_address() == address);
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

TEST_CASE("DMA-BUF cache invalidation follows the registered subrange", "[mr_cache][dmabuf]") {
    using namespace loom;
    test::mock_domain provider;
    mr_cache<provider::tcp_tag> cache(provider.dom);
    alignas(4096) std::array<std::byte, 8192> storage{};
    {
        auto handle = cache.lookup_dmabuf(storage.data(), 32, mr_access_flags::send, 7, 4096);
        REQUIRE(handle);
        CHECK(handle->base_address() == storage.data() + 4096);
        CHECK(handle->mr()->address() == handle->base_address());
        cache.invalidate(storage.data() + 4096 + 8, 1);
        CHECK(provider.live_regions == 1);
    }
    CHECK(provider.live_regions == 0);
    CHECK(cache.size() == 0);
    CHECK(cache.stats().current_entries == 0);
    CHECK(cache.stats().total_registered_bytes == 0);
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

TEST_CASE("invalidated registrations retire after their last handle", "[mr_cache][invalidate]") {
    using namespace loom;
    test::mock_domain provider;
    mr_cache<provider::tcp_tag> cache(provider.dom);
    alignas(4096) std::array<std::byte, 4096> buffer{};
    auto old = cache.lookup(buffer, mr_access_flags::send);
    REQUIRE(old);
    auto copy = *old;
    const auto old_key = old->key();
    cache.invalidate(buffer.data() + 17, 1);
    cache.invalidate(buffer.data(), buffer.size());
    REQUIRE(old->valid());
    REQUIRE(old->key() == old_key);
    REQUIRE(copy.key() == old_key);
    REQUIRE(provider.live_regions == 1);

    auto fresh = cache.lookup(buffer, mr_access_flags::send);
    REQUIRE(fresh);
    REQUIRE(fresh->key() != old_key);
    REQUIRE(provider.live_regions == 2);
    *old = {};
    REQUIRE(provider.live_regions == 2);
    copy = {};
    REQUIRE(provider.live_regions == 1);
    REQUIRE(cache.size() == 1);
    REQUIRE(cache.stats().current_entries == 1);
    REQUIRE(cache.stats().total_registered_bytes == buffer.size());
    REQUIRE(cache.stats().evictions == 1);

    auto hit = cache.lookup(buffer, mr_access_flags::send);
    REQUIRE(hit);
    REQUIRE(hit->key() == fresh->key());
    REQUIRE(cache.stats().hits == 1);
    cache.clear();
    REQUIRE(provider.live_regions == 1);
    *fresh = {};
    *hit = {};
    cache.clear();
    REQUIRE(provider.live_regions == 0);
    REQUIRE(cache.stats().total_registered_bytes == 0);
    REQUIRE(cache.stats().evictions == 2);
}

TEST_CASE("invalidation retires each overlapping registration independently",
          "[mr_cache][invalidate]") {
    using namespace loom;
    test::mock_domain provider;
    mr_cache<provider::tcp_tag> cache(provider.dom);
    alignas(4096) std::array<std::byte, 8192> buffer{};
    auto sender = cache.lookup(buffer.data(), 32, mr_access_flags::send);
    auto receiver = cache.lookup(buffer.data(), 32, mr_access_flags::recv);
    auto device = cache.lookup_hmem(buffer.data(), 32, mr_access_flags::send, hmem_device::cuda(0));
    auto dma = cache.lookup_dmabuf(buffer.data(), 32, mr_access_flags::send, 7);
    auto separate = cache.lookup(buffer.data() + 4096, 32, mr_access_flags::send);
    REQUIRE(sender);
    REQUIRE(receiver);
    REQUIRE(device);
    REQUIRE(dma);
    REQUIRE(separate);
    const auto separate_key = separate->key();
    cache.invalidate(buffer.data() + 4096, 0);
    cache.invalidate(buffer.data(), 1);
    *sender = {};
    *receiver = {};
    *device = {};
    *dma = {};
    *separate = {};
    REQUIRE(provider.live_regions == 1);
    REQUIRE(cache.stats().evictions == 4);
    auto reused = cache.lookup(buffer.data() + 4096, 32, mr_access_flags::send);
    REQUIRE(reused);
    REQUIRE(reused->key() == separate_key);
}

TEST_CASE("last handle release can race cache invalidation and eviction",
          "[mr_cache][invalidate]") {
    using namespace loom;
    test::mock_domain provider;
    mr_cache<provider::tcp_tag> cache(provider.dom);
    alignas(4096) std::array<std::byte, 4096> buffer{};
    for (int iteration = 0; iteration < 100; ++iteration) {
        auto registration = cache.lookup(buffer, mr_access_flags::send);
        REQUIRE(registration);
        std::barrier ready{2};
        std::jthread release([handle = std::move(*registration), &ready]() mutable {
            ready.arrive_and_wait();
            handle = {};
        });
        ready.arrive_and_wait();
        cache.invalidate(buffer.data(), buffer.size());
        cache.evict_unreferenced();
        release.join();
        REQUIRE(provider.live_regions == 0);
        REQUIRE(cache.size() == 0);
        REQUIRE(cache.stats().total_registered_bytes == 0);
    }
    REQUIRE(cache.stats().evictions == 100);
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
    REQUIRE(handle.key().error() == loom::errc::invalid_argument);
    REQUIRE_FALSE(handle.descriptor());
    REQUIRE(handle.base_address() == nullptr);
    REQUIRE(handle.length() == 0UL);
    REQUIRE(handle.refcount() == 0UL);

    auto remote = handle.to_remote_memory();
    REQUIRE_FALSE(remote);
    REQUIRE(remote.error() == loom::errc::invalid_argument);
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

namespace {
enum class cache_memory { host, hmem, dmabuf };

auto endpoint_cache_lookup(auto& cache, std::span<std::byte> buffer, cache_memory kind)
    -> decltype(cache.lookup(buffer, loom::mr_access_flags::send)) {
    switch (kind) {
        case cache_memory::host:
            return cache.lookup(buffer, loom::mr_access_flags::send);
        case cache_memory::hmem:
            return cache.lookup_hmem(buffer.data(),
                                     buffer.size(),
                                     loom::mr_access_flags::send,
                                     loom::hmem_device::cuda(0));
        case cache_memory::dmabuf:
            return cache.lookup_dmabuf(
                buffer.data(), buffer.size(), loom::mr_access_flags::send, 7);
    }
    std::unreachable();
}

struct endpoint_cache_provider : loom::test::mock_domain {
    struct binding {
        fid* endpoint{};
        bool enabled{};
    };
    std::unordered_map<fid_mr*, binding> bindings;
    int binds{}, enables{}, endpoint_closes{};
    int bind_error{}, enable_error{};
    const decltype(fi_ops::close) release_region = region_ops.close;

    static auto owner(fid* handle) -> endpoint_cache_provider& {
        return *static_cast<endpoint_cache_provider*>(
            static_cast<loom::test::mock_domain*>(handle->context));
    }
    fi_ops endpoint_ops{
        .size = sizeof(fi_ops),
        .close =
            [](fid* endpoint) {
                auto& self = owner(endpoint);
                for (const auto& [region, binding] : self.bindings)
                    if (binding.endpoint == endpoint)
                        return -FI_EBUSY;
                ++self.endpoint_closes;
                delete reinterpret_cast<fid_ep*>(endpoint);
                return 0;
            },
    };
    fi_ops_domain operations{
        .size = sizeof(fi_ops_domain),
        .endpoint =
            [](fid_domain* domain, fi_info*, fid_ep** output, void*) {
                auto& self = owner(&domain->fid);
                *output = new fid_ep{.fid = {.fclass = FI_CLASS_EP,
                                             .context = domain->fid.context,
                                             .ops = &self.endpoint_ops}};
                return 0;
            },
    };
    fi_ep_attr endpoint_attributes{.type = FI_EP_RDM};
    loom::fabric_info info =
        loom::fabric_info::copy_native(fi_info{.ep_attr = &endpoint_attributes}).value();

    explicit endpoint_cache_provider(int mode = FI_MR_ENDPOINT,
                                     std::size_t key_bytes = sizeof(std::uint64_t))
        : mock_domain(mode, key_bytes) {
        handle.ops = &operations;
        region_ops.bind = [](fid* region, fid* endpoint, std::uint64_t flags) {
            auto& self = owner(region);
            ++self.binds;
            CHECK(flags == 0);
            CHECK(endpoint->fclass == FI_CLASS_EP);
            CHECK(endpoint->context == region->context);
            if (self.bind_error)
                return self.bind_error;
            auto* mr = reinterpret_cast<fid_mr*>(region);
            if (self.bindings.contains(mr))
                return -FI_EOPBADSTATE;
            self.bindings.emplace(mr, binding{endpoint, false});
            return 0;
        };
        region_ops.control = [](fid* region, int command, void* argument) {
            auto& self = owner(region);
            ++self.enables;
            CHECK(command == FI_ENABLE);
            CHECK(argument == nullptr);
            auto binding = self.bindings.find(reinterpret_cast<fid_mr*>(region));
            if (binding == self.bindings.end() || binding->second.enabled)
                return -FI_EOPBADSTATE;
            if (self.enable_error)
                return self.enable_error;
            binding->second.enabled = true;
            return 0;
        };
        region_ops.close = [](fid* region) {
            auto& self = owner(region);
            self.bindings.erase(reinterpret_cast<fid_mr*>(region));
            return self.release_region(region);
        };
    }
    auto endpoint(loom::memory_resource* resource = nullptr) -> loom::endpoint {
        return loom::endpoint::create(dom, info, resource).value();
    }
};
}  // namespace

TEST_CASE("endpoint caches publish enabled host and device registrations",
          "[mr_cache][endpoint_mr]") {
    using namespace loom;
    const auto kind = GENERATE(cache_memory::host, cache_memory::hmem, cache_memory::dmabuf);
    const auto mode = GENERATE(0, FI_MR_ENDPOINT, FI_MR_ENDPOINT | FI_MR_PROV_KEY);
    const auto explicit_resource = GENERATE(false, true);
    endpoint_cache_provider provider{mode};
    alignas(4096) std::array<std::byte, 4096> buffer{};
    auto endpoint = provider.endpoint(explicit_resource ? get_default_resource() : nullptr);
    REQUIRE(endpoint.belongs_to(provider.dom));
    CHECK(provider.dom.get_memory_region_mode().has(mr_mode_flags::endpoint) ==
          bool(mode & FI_MR_ENDPOINT));
    auto cache = make_mr_cache<provider::tcp_tag>(provider.dom, endpoint);
    auto handle = endpoint_cache_lookup(cache, buffer, kind);
    REQUIRE(handle);
    CHECK(provider.registrations == 1);
    CHECK(cache.stats().registrations == 1);
    CHECK(cache.size() == 1);
    if (mode & FI_MR_ENDPOINT) {
        REQUIRE(provider.bindings.size() == 1);
        auto* native = static_cast<fid_mr*>(handle->mr()->internal_ptr());
        CHECK(provider.bindings.at(native).endpoint == endpoint.internal_ptr());
        CHECK(provider.bindings.at(native).enabled);
        CHECK(provider.binds == 1);
        CHECK(provider.enables == 1);
    } else {
        CHECK(provider.bindings.empty());
        CHECK(provider.binds == 0);
        CHECK(provider.enables == 0);
    }
    REQUIRE(handle->key());
    {
        const auto again = endpoint_cache_lookup(cache, buffer, kind);
        REQUIRE(again);
        CHECK(again->key() == handle->key());
        CHECK(provider.registrations == 1);
        CHECK(cache.stats().hits == 1);
    }
    cache.clear();
    CHECK(provider.live_regions == 1);
    if (mode & FI_MR_ENDPOINT)
        CHECK(endpoint.close().error() == errc::busy);
    handle = std::unexpected(make_error_code(errc::io_error));
    CHECK(provider.live_regions == 1);
    cache.clear();
    CHECK(provider.live_regions == 0);
    CHECK(provider.bindings.empty());
    REQUIRE(endpoint.close());
    CHECK(provider.endpoint_closes == 1);
    CHECK(endpoint_cache_lookup(cache, buffer, kind).error() == errc::invalid_argument);
}

TEST_CASE("endpoint caches never reuse another endpoint registration", "[mr_cache][endpoint_mr]") {
    using namespace loom;
    endpoint_cache_provider provider;
    alignas(4096) std::array<std::byte, 4096> buffer{};
    auto first = provider.endpoint();
    auto second = provider.endpoint();
    mr_cache<provider::tcp_tag> first_cache{provider.dom, first};
    mr_cache<provider::tcp_tag> second_cache{provider.dom, second};
    auto first_region = first_cache.lookup(buffer, mr_access_flags::send).value();
    auto second_region = second_cache.lookup(buffer, mr_access_flags::send).value();
    CHECK(first_region.mr()->internal_ptr() != second_region.mr()->internal_ptr());
    CHECK(first_region.key() != second_region.key());
    CHECK(provider.bindings.at(static_cast<fid_mr*>(first_region.mr()->internal_ptr())).endpoint ==
          first.internal_ptr());
    CHECK(provider.bindings.at(static_cast<fid_mr*>(second_region.mr()->internal_ptr())).endpoint ==
          second.internal_ptr());
    auto moved = std::move(first);
    CHECK(first_cache.lookup(buffer, mr_access_flags::send).error() == errc::invalid_argument);
    CHECK(moved.belongs_to(provider.dom));
    first = std::move(moved);
    REQUIRE(first_cache.lookup(buffer, mr_access_flags::send));
    CHECK(provider.registrations == 2);
}

TEST_CASE("endpoint caches reject missing or foreign native scopes before registering",
          "[mr_cache][endpoint_mr]") {
    using namespace loom;
    const auto kind = GENERATE(cache_memory::host, cache_memory::hmem, cache_memory::dmabuf);
    endpoint_cache_provider provider;
    endpoint_cache_provider foreign;
    alignas(4096) std::array<std::byte, 4096> buffer{};
    auto endpoint = foreign.endpoint();
    mr_cache<provider::tcp_tag> cache{provider.dom, endpoint};
    CHECK(endpoint_cache_lookup(cache, buffer, kind).error() == errc::invalid_argument);
    CHECK(provider.registrations == 0);
    CHECK(foreign.registrations == 0);
    CHECK_FALSE(endpoint.belongs_to(provider.dom));
    loom::endpoint empty_endpoint;
    mr_cache<provider::tcp_tag> empty_cache{provider.dom, empty_endpoint};
    CHECK(endpoint_cache_lookup(empty_cache, buffer, kind).error() == errc::invalid_argument);
    domain empty_domain;
    mr_cache<provider::tcp_tag> invalid{empty_domain};
    CHECK(endpoint_cache_lookup(invalid, buffer, kind).error() == errc::invalid_argument);
    CHECK_FALSE(endpoint.belongs_to(empty_domain));
    CHECK_FALSE(empty_endpoint.belongs_to(provider.dom));
    auto moved_domain = std::move(provider.dom);
    mr_cache<provider::tcp_tag> moved_cache{provider.dom};
    CHECK(endpoint_cache_lookup(moved_cache, buffer, kind).error() == errc::invalid_argument);
    provider.dom = std::move(moved_domain);
}

TEST_CASE("failed endpoint registration is retired before cache publication",
          "[mr_cache][endpoint_mr]") {
    using namespace loom;
    enum class failure { registration, bind, enable };
    const auto fail = GENERATE(failure::registration, failure::bind, failure::enable);
    const auto kind = GENERATE(cache_memory::host, cache_memory::hmem, cache_memory::dmabuf);
    // A one-key pool proves failed registration/bind/enable releases the lease.
    endpoint_cache_provider provider{FI_MR_ENDPOINT, 0};
    alignas(4096) std::array<std::byte, 4096> buffer{};
    auto endpoint = provider.endpoint();
    mr_cache<provider::tcp_tag> cache{provider.dom, endpoint};
    switch (fail) {
        case failure::registration:
            provider.registration_error = -FI_EAGAIN;
            break;
        case failure::bind:
            provider.bind_error = -FI_EAGAIN;
            break;
        case failure::enable:
            provider.enable_error = -FI_EAGAIN;
            break;
    }
    const auto rejected = endpoint_cache_lookup(cache, buffer, kind);
    REQUIRE_FALSE(rejected);
    CHECK(rejected.error() == errc::again);
    CHECK(cache.size() == 0);
    CHECK(cache.stats().registrations == 0);
    CHECK(cache.stats().current_entries == 0);
    CHECK(cache.stats().total_registered_bytes == 0);
    CHECK(provider.live_regions == 0);
    CHECK(provider.keys.empty());
    CHECK(provider.bindings.empty());
    CHECK(provider.binds == (fail == failure::registration ? 0 : 1));
    CHECK(provider.enables == (fail == failure::enable ? 1 : 0));
    provider.registration_error = provider.bind_error = provider.enable_error = 0;
    const auto accepted = endpoint_cache_lookup(cache, buffer, kind);
    REQUIRE(accepted);
    CHECK(provider.live_regions == 1);
    CHECK(cache.size() == 1);
    CHECK(provider.requested_keys.back() == 0);
    const auto native = static_cast<fid_mr*>(accepted->mr()->internal_ptr());
    CHECK(provider.bindings.at(native).enabled);
}

TEST_CASE("native endpoint caches preserve ordinary domain registration",
          "[mr_cache][integration]") {
    using namespace loom;
    fabric_hints hints;
    hints.provider_name = "sockets";
    hints.ep_type = endpoint_types::rdm;
    hints.capabilities = capability::msg;
    auto info = query_fabric(hints);
    if (!info)
        SKIP("Sockets provider unavailable");
    auto fabric = fabric::create(*info).value();
    auto domain = domain::create(fabric, *info).value();
    auto foreign = domain::create(fabric, *info).value();
    auto endpoint = endpoint::create(domain, *info).value();
    CHECK(endpoint.belongs_to(domain));
    CHECK_FALSE(endpoint.belongs_to(foreign));
    CHECK(domain.get_memory_region_mode() == info->get_domain_attr()->memory_region_mode);
    CHECK_FALSE(domain.get_memory_region_mode().has(mr_mode_flags::endpoint));
    alignas(4096) std::array<std::byte, 4096> buffer{};
    mr_cache<provider::tcp_tag> cache{domain, endpoint};
    auto region = cache.lookup(buffer, mr_access_flags::send);
    REQUIRE(region);
    REQUIRE(region->key());
    CHECK(region->base_address() == buffer.data());
    mr_cache<provider::tcp_tag> invalid{foreign, endpoint};
    CHECK(invalid.lookup(buffer, mr_access_flags::send).error() == errc::invalid_argument);
    CHECK(invalid.size() == 0);
}

TEST_CASE("device cache lookups reuse registrations and count hits", "[mr_cache][reuse]") {
    using namespace loom;
    const auto dma = GENERATE(false, true);
    test::mock_domain provider;
    mr_cache<provider::tcp_tag> cache(provider.dom);
    std::array<std::byte, 128> allocation{};
    for (int i = 0; i < 100; ++i) {
        const auto handle =
            dma ? cache.lookup_dmabuf(allocation.data(), 64, mr_access_flags::send, 7, 17)
                : cache.lookup_hmem(
                      allocation.data() + 17, 64, mr_access_flags::send, hmem_device::cuda(0));
        REQUIRE(handle);
        CHECK(handle->base_address() == allocation.data() + 17);
    }
    CHECK(provider.registrations == 1);
    CHECK(provider.live_regions == 1);
    CHECK(cache.size() == 1);
    CHECK(cache.stats().hits == 99);
    CHECK(cache.stats().misses == 1);
    CHECK(cache.stats().registrations == 1);
    CHECK(cache.stats().total_registered_bytes == 64);
    cache.clear();
    CHECK(provider.live_regions == 0);
}

TEST_CASE("device cache lookups reuse contained ranges and permissions", "[mr_cache][reuse]") {
    using namespace loom;
    const auto dma = GENERATE(false, true);
    test::mock_domain provider;
    mr_cache<provider::tcp_tag> cache(provider.dom);
    std::array<std::byte, 128> allocation{};
    const auto original =
        dma ? cache.lookup_dmabuf(
                  allocation.data(), 64, mr_access_flags::send | mr_access_flags::recv, 7, 17)
            : cache.lookup_hmem(allocation.data() + 17,
                                64,
                                mr_access_flags::send | mr_access_flags::recv,
                                hmem_device::cuda(0));
    REQUIRE(original);
    const auto contained =
        dma ? cache.lookup_dmabuf(allocation.data(), 32, mr_access_flags::recv, 7, 25)
            : cache.lookup_hmem(
                  allocation.data() + 25, 32, mr_access_flags::recv, hmem_device::cuda(0));
    REQUIRE(contained);
    CHECK(contained->mr()->internal_ptr() == original->mr()->internal_ptr());
    CHECK(original->refcount() == 2);
    CHECK(provider.registrations == 1);
    CHECK(cache.stats().hits == 1);
    CHECK(cache.stats().misses == 1);
}

TEST_CASE("HMEM cache identity separates devices interfaces and permissions", "[mr_cache][hmem]") {
    using namespace loom;
    enum class change {
        none,
        device,
        interface,
        address,
        length,
        permissions,
        reserved,
        invalid_device,
        invalid_interface
    };
    const auto changed = GENERATE(change::none,
                                  change::device,
                                  change::interface,
                                  change::address,
                                  change::length,
                                  change::permissions,
                                  change::reserved,
                                  change::invalid_device,
                                  change::invalid_interface);
    test::mock_domain provider;
    mr_cache<provider::tcp_tag> cache(provider.dom);
    std::array<std::byte, 128> allocation{};
    const auto original = cache.lookup_hmem(
        allocation.data(), 32, mr_access_flags::send | mr_access_flags::recv, hmem_device::cuda(0));
    REQUIRE(original);
    auto device = hmem_device::cuda(0);
    auto* address = allocation.data();
    std::size_t length = 32;
    auto access = mr_access_flags::send;
    switch (changed) {
        case change::none:
            break;
        case change::device:
            device.device_id = 1;
            break;
        case change::interface:
            device.iface = hmem_iface::rocr;
            break;
        case change::address:
            address += 64;
            break;
        case change::length:
            length = 64;
            break;
        case change::permissions:
            access = mr_access_flags::read;
            break;
        case change::reserved:
            device.hmem_data = allocation.data();
            break;
        case change::invalid_device:
            device.device_id = std::numeric_limits<std::int64_t>::max();
            break;
        case change::invalid_interface:
            device.iface = static_cast<hmem_iface>(99);
            break;
    }
    const auto found = cache.lookup_hmem(address, length, access, device);
    const bool invalid = changed == change::reserved || changed == change::invalid_device ||
                         changed == change::invalid_interface;
    CAPTURE(changed);
    REQUIRE(static_cast<bool>(found) == !invalid);
    if (invalid) {
        CHECK(found.error() == errc::invalid_argument);
        CHECK(provider.registrations == 1);
    } else {
        CHECK((found->mr()->internal_ptr() == original->mr()->internal_ptr()) ==
              (changed == change::none));
        CHECK(provider.registrations == (changed == change::none ? 1 : 2));
    }
    provider.registration_error = -FI_EIO;
    const auto again =
        cache.lookup_hmem(allocation.data(), 16, mr_access_flags::recv, hmem_device::cuda(0));
    REQUIRE(again);
    CHECK(again->mr()->internal_ptr() == original->mr()->internal_ptr());
}

TEST_CASE("DMA-BUF cache identity checks file and virtual ranges", "[mr_cache][dmabuf]") {
    using namespace loom;
    enum class change {
        same,
        inside,
        before,
        beyond,
        fd,
        mapping,
        permissions,
        overflow,
        invalid_fd
    };
    const auto changed = GENERATE(change::same,
                                  change::inside,
                                  change::before,
                                  change::beyond,
                                  change::fd,
                                  change::mapping,
                                  change::permissions,
                                  change::overflow,
                                  change::invalid_fd);
    const auto mapped = GENERATE(false, true);
    CAPTURE(changed, mapped);
    test::mock_domain provider;
    mr_cache<provider::tcp_tag> cache(provider.dom);
    std::array<std::byte, 128> allocation{}, other{};
    auto* base = mapped ? allocation.data() : nullptr;
    const auto original =
        cache.lookup_dmabuf(base, 64, mr_access_flags::send | mr_access_flags::recv, 7, 17);
    REQUIRE(original);
    std::uint64_t offset = 17;
    int fd = 7;
    std::size_t length = 32;
    auto access = mr_access_flags::send;
    switch (changed) {
        case change::same:
            break;
        case change::inside:
            offset = 25;
            break;
        case change::before:
            offset = 16;
            break;
        case change::beyond:
            offset = 73;
            break;
        case change::fd:
            fd = 8;
            break;
        case change::mapping:
            base = other.data();
            break;
        case change::permissions:
            access = mr_access_flags::read;
            break;
        case change::overflow:
            offset = std::numeric_limits<std::uint64_t>::max();
            break;
        case change::invalid_fd:
            fd = -1;
            break;
    }
    const auto found = cache.lookup_dmabuf(base, length, access, fd, offset);
    const bool invalid = changed == change::overflow || changed == change::invalid_fd;
    const bool hit = changed == change::same || changed == change::inside;
    REQUIRE(static_cast<bool>(found) == !invalid);
    if (invalid) {
        CHECK(found.error() == errc::invalid_argument);
        CHECK(provider.registrations == 1);
    } else {
        CHECK((found->mr()->internal_ptr() == original->mr()->internal_ptr()) == hit);
        CHECK(provider.registrations == (hit ? 1 : 2));
    }
    CHECK(cache.stats().hits == (hit ? 1 : 0));
    CHECK(cache.stats().misses == (hit || invalid ? 1 : 2));
    provider.registration_error = -FI_EIO;
    const auto again =
        cache.lookup_dmabuf(mapped ? allocation.data() : nullptr, 16, mr_access_flags::recv, 7, 17);
    REQUIRE(again);
    CHECK(again->mr()->internal_ptr() == original->mr()->internal_ptr());
}

TEST_CASE("invalidated device allocations cannot be reused by new handles",
          "[mr_cache][invalidate]") {
    using namespace loom;
    const auto kind = GENERATE(cache_memory::hmem, cache_memory::dmabuf);
    const auto mapped = GENERATE(false, true);
    test::mock_domain provider;
    mr_cache<provider::tcp_tag> cache(provider.dom);
    std::array<std::byte, 128> allocation{};
    auto* base = mapped ? allocation.data() : nullptr;
    const auto lookup = [&]() {
        return kind == cache_memory::dmabuf
                   ? cache.lookup_dmabuf(base, 64, mr_access_flags::send, 7, 17)
                   : cache.lookup_hmem(base, 64, mr_access_flags::send, hmem_device::cuda(0));
    };
    auto original = lookup();
    REQUIRE(original);
    cache.invalidate(original->base_address(), original->length());
    auto replacement = lookup();
    REQUIRE(replacement);
    CHECK(replacement->mr()->internal_ptr() != original->mr()->internal_ptr());
    CHECK(provider.live_regions == 2);
    CHECK(cache.stats().total_registered_bytes == 128);
    *original = {};
    CHECK(provider.live_regions == 1);
    CHECK(cache.stats().total_registered_bytes == 64);
    CHECK(cache.stats().evictions == 1);
    cache.clear();
    CHECK(provider.live_regions == 1);
    *replacement = {};
    cache.clear();
    CHECK(provider.live_regions == 0);
    CHECK(cache.size() == 0);
}
