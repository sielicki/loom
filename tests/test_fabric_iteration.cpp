// SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only
#include <loom/detail/impl/fabric_impl.hpp>
#include <loom/provider_range.hpp>

#include <array>
#include <barrier>
#include <thread>

#include <catch2/catch_test_macros.hpp>

namespace {
struct failing_resource : std::pmr::memory_resource {
    bool fail{};
    int outstanding{};

    auto do_allocate(std::size_t bytes, std::size_t alignment) -> void* override {
        if (fail) {
            throw std::bad_alloc{};
        }
        auto* storage = std::pmr::new_delete_resource()->allocate(bytes, alignment);
        ++outstanding;
        return storage;
    }
    void do_deallocate(void* pointer, std::size_t bytes, std::size_t alignment) override {
        --outstanding;
        std::pmr::new_delete_resource()->deallocate(pointer, bytes, alignment);
    }
    auto do_is_equal(const memory_resource& other) const noexcept -> bool override {
        return this == &other;
    }
};

auto providers(loom::memory_resource* resource = loom::get_default_resource())
    -> loom::fabric_info {
    auto* first = fi_allocinfo();
    REQUIRE(first);
    first->caps = FI_MSG;
    first->next = fi_allocinfo();
    REQUIRE(first->next);
    first->next->caps = FI_RMA;
    first->next->next = fi_allocinfo();
    REQUIRE(first->next->next);
    first->next->next->caps = FI_TAGGED;
    return loom::fabric_info{
        loom::detail::make_pmr_unique<loom::detail::fabric_info_impl>(resource, first)};
}
}  // namespace

TEST_CASE("fabric iteration retries failed allocations without losing position",
          "[fabric][iterator]") {
    failing_resource resource;
    {
        const auto info = providers(&resource);
        REQUIRE(resource.outstanding == 1);
        auto cursor = info.begin();
        resource.fail = true;
        REQUIRE_THROWS_AS(++cursor, std::bad_alloc);
        REQUIRE(cursor == info.begin());
        REQUIRE(cursor->get_caps() == loom::capability::msg);
        resource.fail = false;
        ++cursor;
        REQUIRE(cursor->get_caps() == loom::capability::rma);
        REQUIRE(resource.outstanding == 2);
    }
    REQUIRE(resource.outstanding == 0);
}

TEST_CASE("fabric enumeration visits every provider once", "[fabric][iterator]") {
    static_assert(std::forward_iterator<loom::fabric_info::iterator>);
    const auto info = providers();
    auto cursor = info.begin();
    auto* native = static_cast<fi_info*>(info.internal_ptr());
    for (const auto expected :
         {loom::capability::msg, loom::capability::rma, loom::capability::tagged}) {
        REQUIRE(cursor != info.end());
        REQUIRE(cursor->get_caps() == expected);
        REQUIRE(cursor->internal_ptr() == native);
        const auto previous = cursor++;
        REQUIRE(previous->get_caps() == expected);
        native = native->next;
    }
    REQUIRE(cursor == info.end());
    REQUIRE(info.get_caps() == loom::capability::msg);
}

TEST_CASE("provider iterators retain independent positions and references", "[fabric][iterator]") {
    const loom::provider_range range{providers()};
    const auto first = range.begin();
    auto cursor = first;
    ++cursor;
    REQUIRE(cursor->get_caps() == loom::capability::rma);
    const auto& second = *cursor;
    ++cursor;
    REQUIRE(cursor->get_caps() == loom::capability::tagged);
    REQUIRE(second.get_caps() == loom::capability::rma);
    REQUIRE(first->get_caps() == loom::capability::msg);
    REQUIRE(range.size() == 3);
    auto again = range.begin();
    ++again;
    REQUIRE(&*again == &second);
    auto filtered = range | std::views::filter([](const auto& info) {
                        return info.get_caps().has(loom::capability::rma);
                    });
    REQUIRE(std::ranges::distance(filtered) == 1);
}

TEST_CASE("const fabric iterators share stable entries across threads", "[fabric][iterator]") {
    const auto info = providers();
    std::array<std::array<const loom::fabric_info*, 3>, 8> entries{};
    std::barrier ready{8};
    {
        std::array<std::jthread, 8> threads;
        for (std::size_t i = 0; i < threads.size(); ++i) {
            threads[i] = std::jthread([&, i] {
                ready.arrive_and_wait();
                auto cursor = info.begin();
                for (auto& entry : entries[i]) {
                    if (cursor != info.end()) {
                        entry = &*cursor;
                        ++cursor;
                    }
                }
            });
        }
    }
    for (const auto& sequence : entries) {
        REQUIRE(sequence == entries.front());
        REQUIRE(sequence[0] != sequence[1]);
        REQUIRE(sequence[1] != sequence[2]);
        REQUIRE(sequence[2]);
        REQUIRE(sequence[2]->get_caps() == loom::capability::tagged);
    }
}

TEST_CASE("fabric iteration handles empty and single results", "[fabric][iterator]") {
    const loom::fabric_info empty;
    REQUIRE(empty.begin() == empty.end());
    const loom::provider_range empty_range{loom::fabric_info{}};
    REQUIRE(empty_range.empty());
    REQUIRE(empty_range.size() == 0);
    auto* raw = fi_allocinfo();
    REQUIRE(raw);
    const loom::fabric_info single{loom::detail::make_pmr_unique<loom::detail::fabric_info_impl>(
        loom::get_default_resource(), raw)};
    auto cursor = single.begin();
    REQUIRE(cursor != single.end());
    REQUIRE(++cursor == single.end());
}
