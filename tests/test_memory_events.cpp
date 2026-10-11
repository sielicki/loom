// SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only
#include <loom/detail/impl/counter_impl.hpp>
#include <loom/detail/impl/endpoint_impl.hpp>
#include <loom/loom.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <thread>
#include <utility>

#include "mock_domain.hpp"
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

namespace {
struct event_provider : loom::test::mock_domain {
    bool needs_endpoint{};
    bool enabled{};
    fid* bound_counter{};
    fid* bound_endpoint{};
    int bind_calls{};
    int enable_calls{};
    int bind_error{};
    int enable_error{};
    fi_ops object_ops{.size = sizeof(fi_ops), .close = [](fid*) { return 0; }};
    fid_cntr native_counter{.fid = {.fclass = FI_CLASS_CNTR, .ops = &object_ops}};
    fid_ep native_endpoint{.fid = {.fclass = FI_CLASS_EP, .ops = &object_ops}};

    static auto owner(fid* handle) -> event_provider& {
        return *static_cast<event_provider*>(
            static_cast<loom::test::mock_domain*>(handle->context));
    }

    explicit event_provider(bool endpoint_bound = false)
        : mock_domain(FI_MR_RMA_EVENT | (endpoint_bound ? FI_MR_ENDPOINT : 0)),
          needs_endpoint(endpoint_bound) {
        region_ops.bind = [](fid* mr, fid* resource, std::uint64_t flags) {
            auto& self = owner(mr);
            ++self.bind_calls;
            if (self.bind_error)
                return self.bind_error;
            if (self.enabled)
                return -FI_EOPBADSTATE;
            switch (resource->fclass) {
                case FI_CLASS_CNTR:
                    if (flags != FI_REMOTE_WRITE || !(self.flags & FI_RMA_EVENT))
                        return -FI_EINVAL;
                    self.bound_counter = resource;
                    return 0;
                case FI_CLASS_EP:
                    if (flags != 0 || !self.needs_endpoint)
                        return -FI_EINVAL;
                    self.bound_endpoint = resource;
                    return 0;
                default:
                    return -FI_EINVAL;
            }
        };
        region_ops.control = [](fid* mr, int command, void* arg) {
            auto& self = owner(mr);
            if (command != FI_ENABLE || arg)
                return -FI_EINVAL;
            ++self.enable_calls;
            if (self.enable_error)
                return self.enable_error;
            if (!self.bound_counter || (self.needs_endpoint && !self.bound_endpoint))
                return -FI_EOPBADSTATE;
            self.enabled = true;
            return 0;
        };
    }

    auto counter() -> loom::counter {
        return loom::counter{loom::detail::make_pmr_unique<loom::detail::counter_impl>(
            loom::get_default_resource(), &native_counter)};
    }
    auto endpoint() -> loom::endpoint {
        return loom::endpoint{loom::detail::make_pmr_unique<loom::detail::endpoint_impl>(
            loom::get_default_resource(),
            &native_endpoint,
            loom::endpoint_types::rdm,
            loom::address_format::unspecified,
            0UZ,
            0UZ,
            loom::caps{},
            0,
            0,
            &handle)};
    }
    auto region(std::span<std::byte> buffer) -> loom::memory_region {
        return loom::memory_region::register_memory(
                   dom,
                   buffer.data(),
                   buffer.size(),
                   loom::memory_registration{.access = loom::mr_access_flags::remote_write,
                                             .rma_events = true})
            .value();
    }
};
}  // namespace

TEST_CASE("remote-event regions bind all resources before enablement", "[memory][rma_event]") {
    const auto endpoint_bound = GENERATE(false, true);
    const auto counter_first = GENERATE(false, true);
    event_provider provider{endpoint_bound};
    auto counter = provider.counter();
    auto endpoint = provider.endpoint();
    std::array<std::byte, 64> buffer{};
    auto region = provider.region(buffer);
    CHECK(provider.flags == FI_RMA_EVENT);
    CHECK(provider.bind_calls == 0);
    CHECK(provider.enable_calls == 0);
    const auto early = region.enable();
    REQUIRE_FALSE(early);
    CHECK(early.error() == loom::make_error_code_from_fi_errno(FI_EOPBADSTATE));
    CHECK_FALSE(provider.enabled);
    if (endpoint_bound && !counter_first)
        REQUIRE(region.bind(endpoint));
    REQUIRE(region.bind(counter));
    if (endpoint_bound && counter_first) {
        CHECK_FALSE(region.enable());
        CHECK_FALSE(provider.enabled);
        REQUIRE(region.bind(endpoint));
    }
    CHECK(provider.bound_counter == &provider.native_counter.fid);
    CHECK(provider.bound_endpoint == (endpoint_bound ? &provider.native_endpoint.fid : nullptr));
    CHECK(provider.bind_calls == (endpoint_bound ? 2 : 1));
    REQUIRE(region.enable());
    CHECK(provider.enabled);
    const auto late = region.bind(counter);
    REQUIRE_FALSE(late);
    CHECK(late.error() == loom::make_error_code_from_fi_errno(FI_EOPBADSTATE));
}

TEST_CASE("MR counter binding preserves provider errors and rejects empty handles",
          "[memory][rma_event]") {
    event_provider provider;
    auto counter = provider.counter();
    std::array<std::byte, 64> buffer{};
    auto region = provider.region(buffer);
    loom::memory_region empty_region;
    loom::counter empty_counter;
    CHECK(empty_region.bind(counter).error() == loom::errc::invalid_argument);
    CHECK(region.bind(empty_counter).error() == loom::errc::invalid_argument);
    CHECK(provider.bind_calls == 0);
    provider.bind_error = -FI_ENOMEM;
    CHECK(region.bind(counter).error() == loom::errc::no_memory);
    CHECK(provider.bound_counter == nullptr);
    provider.bind_error = 0;
    auto moved_counter = std::move(counter);
    REQUIRE(region.bind(moved_counter));
    provider.enable_error = -FI_EBUSY;
    CHECK(region.enable().error() == loom::errc::busy);
    CHECK_FALSE(provider.enabled);
    provider.enable_error = 0;
    auto moved_region = std::move(region);
    REQUIRE(moved_region.enable());
    CHECK(provider.enabled);
}

TEST_CASE("MR controls reject unavailable native operations", "[memory][rma_event]") {
    enum class operation { counter, endpoint, enable, refresh };
    enum class missing { table, function, prefix };
    const auto op =
        GENERATE(operation::counter, operation::endpoint, operation::enable, operation::refresh);
    const auto absence = GENERATE(missing::table, missing::function, missing::prefix);
    event_provider provider{true};
    auto counter = provider.counter();
    auto endpoint = provider.endpoint();
    std::array<std::byte, 64> buffer{};
    auto region = provider.region(buffer);
    auto* native = static_cast<fid_mr*>(region.internal_ptr());
    if (absence == missing::table) {
        native->fid.ops = nullptr;
    } else if (absence == missing::function) {
        provider.region_ops.bind = nullptr;
        provider.region_ops.control = nullptr;
    } else {
        provider.region_ops.size = offsetof(fi_ops, bind);
    }
    const auto result = [&]() -> loom::void_result {
        switch (op) {
            case operation::counter:
                return region.bind(counter);
            case operation::endpoint:
                return region.bind(endpoint);
            case operation::enable:
                return region.enable();
            case operation::refresh:
                return region.refresh();
        }
        std::unreachable();
    }();
    native->fid.ops = &provider.region_ops;
    provider.region_ops.size = sizeof(fi_ops);
    REQUIRE_FALSE(result);
    CHECK(result.error() == loom::errc::not_supported);
    CHECK(provider.bind_calls == 0);
    CHECK(provider.enable_calls == 0);
}

TEST_CASE("sockets counts remote writes only to the bound MR", "[memory][rma_event][integration]") {
    using namespace loom;
    fabric_hints hints{};
    hints.provider_name = "sockets";
    hints.ep_type = endpoint_types::rdm;
    hints.capabilities = capability::msg | capability::rma | capability::rma_event;
    const auto info = query_fabric(hints);
    if (!info)
        SKIP("Sockets provider with RMA events unavailable");
    const auto attributes = info->get_domain_attr();
    REQUIRE(attributes);
    REQUIRE(attributes->memory_region_mode == mr_mode_flags::none);
    auto fabric = fabric::create(*info).value();
    auto domain = domain::create(fabric, *info).value();
    auto counter = counter::create(domain).value();
    std::array<std::byte, 8> source{}, target{}, other{};
    auto local = memory_region::register_memory(domain, source, mr_access_flags::write).value();
    auto counted =
        memory_region::register_memory(
            domain,
            target.data(),
            target.size(),
            memory_registration{.access = mr_access_flags::remote_write, .rma_events = true})
            .value();
    auto uncounted =
        memory_region::register_memory(domain, other, mr_access_flags::remote_write).value();
    REQUIRE(counted.bind(counter));
    // This provider registers enabled MRs. The mock tests exercise enablement
    // when FI_MR_RMA_EVENT or FI_MR_ENDPOINT requires it.
    auto cq = completion_queue::create(domain).value();
    auto av = address_vector::create(domain).value();
    auto endpoint = endpoint::create(domain, *info).value();
    REQUIRE(endpoint.bind_cq(cq, cq_bind::transmit | cq_bind::recv));
    REQUIRE(endpoint.bind_av(av));
    REQUIRE(endpoint.enable());
    const auto peer = av.insert(endpoint.get_local_address().value()).value();
    REQUIRE(counter.read().value() == 0);
    std::uint64_t expected{};
    int context{};
    for (const auto count_write : {true, false, true}) {
        source.fill(std::byte{static_cast<unsigned char>(++context)});
        const auto remote = remote_memory::from_mr(count_write ? counted : uncounted).value();
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{3};
        void_result submitted;
        do {
            submitted = rma::write_to(endpoint,
                                      std::span<const std::byte>{source},
                                      local,
                                      fabric_addr{peer.value},
                                      remote,
                                      op_flag::delivery_complete,
                                      &context);
            if (submitted || submitted.error() != errc::again)
                break;
            (void)cq.poll();
            std::this_thread::yield();
        } while (std::chrono::steady_clock::now() < deadline);
        REQUIRE(submitted);
        expected += count_write;
        std::optional<completion_event> completion;
        do {
            if (auto event = cq.poll())
                completion = *event;
            if (completion && counter.read().value() >= expected)
                break;
            std::this_thread::yield();
        } while (std::chrono::steady_clock::now() < deadline);
        REQUIRE(completion);
        REQUIRE_FALSE(completion->error);
        CHECK(completion->context.get() == &context);
        CHECK(counter.read().value() == expected);
        CHECK((count_write ? target : other) == source);
    }
    REQUIRE(endpoint.close());
}
