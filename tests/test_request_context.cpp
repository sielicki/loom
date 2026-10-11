// SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only
#include <loom/core/request_context.hpp>

#include <array>

#include <catch2/catch_test_macros.hpp>

namespace {
struct test_resource : std::pmr::memory_resource {
    bool reject{};
    std::size_t live_allocations{};

    auto do_allocate(std::size_t bytes, std::size_t alignment) -> void* override {
        if (reject)
            throw std::bad_alloc{};
        auto* result = std::pmr::new_delete_resource()->allocate(bytes, alignment);
        ++live_allocations;
        return result;
    }
    void do_deallocate(void* pointer, std::size_t bytes, std::size_t alignment) override {
        --live_allocations;
        std::pmr::new_delete_resource()->deallocate(pointer, bytes, alignment);
    }
    auto do_is_equal(const memory_resource& other) const noexcept -> bool override {
        return this == &other;
    }
};
}  // namespace

TEST_CASE("request pool growth preserves active context addresses", "[context][pool]") {
    loom::request_context_pool pool{1};
    auto* first = pool.acquire();
    int payload = 42;
    first->set_user_data(&payload);
    auto* native_context = first->context_ptr();
    std::array<loom::request_context*, 128> others;
    for (auto& context : others)
        context = pool.acquire();
    pool.release(first);
    REQUIRE(pool.in_use_count() == others.size());
    REQUIRE(pool.acquire() == first);
    REQUIRE(first->context_ptr() == native_context);
    REQUIRE(first->user_data<int>() == nullptr);
    first->set_user_data(&payload);
    static_cast<fi_context2*>(native_context)->internal[7] = &payload;
    REQUIRE(loom::request_context::from_fi_context(native_context)->user_data<int>() == &payload);
    pool.release(first);
    for (auto* context : others)
        pool.release(context);
    REQUIRE(pool.in_use_count() == 0);
}

TEST_CASE("moving request pools preserves acquired addresses across allocators",
          "[context][pool]") {
    test_resource source_resource, destination_resource;
    {
        loom::request_context_pool source{1, &source_resource};
        auto* context = source.acquire();
        int payload = 7;
        context->set_user_data(&payload);
        loom::request_context_pool moved{std::move(source)};
        REQUIRE(moved.in_use_count() == 1);
        loom::request_context_pool destination{1, &destination_resource};
        destination = std::move(moved);
        destination.release(context);
        REQUIRE(destination.in_use_count() == 0);
        REQUIRE(destination.acquire() == context);
        REQUIRE(destination.get_allocator().resource() == &source_resource);
        destination.release(context);
        REQUIRE(source.size() == 0);
        REQUIRE(moved.size() == 0);
        REQUIRE(source.acquire());
        REQUIRE(moved.acquire());
    }
    REQUIRE(source_resource.live_allocations == 0);
    REQUIRE(destination_resource.live_allocations == 0);
}

TEST_CASE("request pools use the default resource for null allocators", "[context][pool]") {
    loom::request_context_pool pool{static_cast<std::pmr::memory_resource*>(nullptr)};
    REQUIRE(pool.get_allocator().resource() == std::pmr::get_default_resource());
    REQUIRE(pool.acquire());
}

TEST_CASE("preallocated request contexts need no allocation to acquire", "[context][pool]") {
    test_resource resource;
    {
        loom::request_context_pool pool{64, &resource};
        resource.reject = true;
        std::array<loom::request_context*, 64> contexts;
        for (auto& context : contexts)
            context = pool.acquire();
        REQUIRE(pool.in_use_count() == contexts.size());
        for (auto* context : contexts)
            pool.release(context);
        REQUIRE(pool.in_use_count() == 0);
    }
    REQUIRE(resource.live_allocations == 0);
}
