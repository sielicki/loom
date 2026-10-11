// SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only
#include <loom/core/provider_aware.hpp>
#include <loom/detail/impl/counter_impl.hpp>
#include <loom/detail/impl/domain_impl.hpp>
#include <loom/detail/impl/fabric_impl.hpp>

#include <array>
#include <concepts>
#include <span>
#include <utility>

#include <catch2/catch_template_test_macros.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

namespace {
using message_profile = loom::message_endpoint_profile<loom::rdm_endpoint_tag, 32>;
using checked_endpoint = loom::contract_endpoint<message_profile>;

struct progress_profile : message_profile {
    static constexpr loom::threading_mode threading = loom::threading_mode::safe;
    static constexpr loom::progress_mode control_progress = loom::progress_mode::manual;
    static constexpr loom::progress_mode data_progress = loom::progress_mode::auto_progress;
};

template <loom::threading_mode Threading>
struct threading_profile : message_profile {
    static constexpr loom::threading_mode threading = Threading;
};

struct context_profile : message_profile {
    static constexpr loom::mode supported_modes = loom::mode_bits::context2;
};

struct rma_profile : message_profile {
    static constexpr loom::caps required_caps =
        message_profile::required_caps | loom::capability::rma;
};

struct receive_profile : message_profile {
    static constexpr loom::caps required_caps = loom::capability::msg | loom::capability::recv;
    static constexpr std::size_t inject_size = 0;
};

struct invalid_inject_profile : receive_profile {
    static constexpr std::size_t inject_size = 32;
};

struct unknown_caps_profile : message_profile {
    static constexpr loom::caps required_caps{1ULL << 63};
};

struct unknown_modes_profile : message_profile {
    static constexpr loom::mode supported_modes{1ULL << 63};
};

auto runtime_inject_size() -> std::size_t {
    return 32;
}
struct runtime_profile : message_profile {
    static inline const std::size_t inject_size = runtime_inject_size();
};

template <typename Endpoint>
concept accepts_send = requires(Endpoint& ep, std::span<const std::byte> data) { ep.send(data); };

template <typename Endpoint, typename Buffer>
concept accepts_inject = requires(Endpoint& ep, Buffer buffer) { ep.inject(buffer); };

struct checked_provider {
    int opened{};
    int closed{};
    int queries{};
    int submitted{};
    int open_result{};
    int query_result{};
    int enable_result{};
    int submission_result{};
    std::size_t inject_limit{64};
    std::size_t enabled_limit{64};
    std::size_t returned_length{sizeof(std::size_t)};
    std::size_t submitted_length{};
    const void* submitted_buffer{};
    fi_addr_t destination{};

    fi_ops parent_ops{.size = sizeof(fi_ops), .close = [](fid*) { return 0; }};
    fi_ops lifetime{
        .size = sizeof(fi_ops),
        .close =
            [](fid* handle) {
                ++static_cast<checked_provider*>(handle->context)->closed;
                return 0;
            },
        .bind =
            [](fid* handle, fid*, std::uint64_t) {
                auto& self = *static_cast<checked_provider*>(handle->context);
                self.inject_limit = self.enabled_limit;
                return self.enable_result;
            },
        .control =
            [](fid* handle, int command, void*) {
                REQUIRE(command == FI_ENABLE);
                auto& self = *static_cast<checked_provider*>(handle->context);
                self.inject_limit = self.enabled_limit;
                return self.enable_result;
            },
    };
    fi_ops_ep endpoint_ops{
        .size = sizeof(fi_ops_ep),
        .getopt =
            [](fid* handle, int level, int option, void* output, std::size_t* length) {
                REQUIRE(level == FI_OPT_ENDPOINT);
                REQUIRE(option == FI_OPT_INJECT_MSG_SIZE);
                REQUIRE(*length == sizeof(std::size_t));
                auto& self = *static_cast<checked_provider*>(handle->context);
                ++self.queries;
                *static_cast<std::size_t*>(output) = self.inject_limit;
                *length = self.returned_length;
                return self.query_result;
            },
    };
    fi_ops_msg message_ops{
        .size = sizeof(fi_ops_msg),
        .inject =
            [](fid_ep* ep, const void* data, std::size_t length, fi_addr_t destination) -> ssize_t {
            auto& self = *static_cast<checked_provider*>(ep->fid.context);
            ++self.submitted;
            self.submitted_buffer = data;
            self.submitted_length = length;
            self.destination = destination;
            return self.submission_result;
        },
    };
    fid_ep endpoint_handle{
        .fid = {.context = this, .ops = &lifetime}, .ops = &endpoint_ops, .msg = &message_ops};
    fi_ops_domain domain_ops{
        .size = sizeof(fi_ops_domain),
        .endpoint =
            [](fid_domain* dom, fi_info*, fid_ep** output, void*) {
                auto& self = *static_cast<checked_provider*>(dom->fid.context);
                if (self.open_result)
                    return self.open_result;
                ++self.opened;
                *output = &self.endpoint_handle;
                return 0;
            },
    };
    fid_domain domain_handle{.fid = {.context = this, .ops = &parent_ops}, .ops = &domain_ops};
    loom::detail::domain_impl* domain_state{};
    loom::domain domain{[this] {
        auto state = loom::detail::make_pmr_unique<loom::detail::domain_impl>(
            loom::get_default_resource(),
            &domain_handle,
            loom::threading_mode::safe,
            loom::progress_mode::manual,
            loom::progress_mode::auto_progress);
        domain_state = state.get();
        return state;
    }()};
    fi_ep_attr ep_attr{.type = FI_EP_RDM, .max_msg_size = 4096};
    fi_tx_attr tx_attr{.inject_size = 48};
    fi_rx_attr rx_attr{};
    fi_info native_info{.caps = FI_MSG | FI_SEND | FI_RECV,
                        .tx_attr = &tx_attr,
                        .rx_attr = &rx_attr,
                        .ep_attr = &ep_attr};
    loom::fabric_info info{loom::detail::make_pmr_unique<loom::detail::fabric_info_impl>(
        loom::get_default_resource(), &native_info, false)};
};
}  // namespace

TEST_CASE("endpoint profiles require typed constant guarantees", "[provider_contract]") {
    static_assert(loom::endpoint_profile<message_profile>);
    static_assert(loom::endpoint_profile<progress_profile>);
    static_assert(!loom::endpoint_profile<int>);
    static_assert(!loom::endpoint_profile<loom::provider::tcp_tag>);
    static_assert(!loom::endpoint_profile<invalid_inject_profile>);
    static_assert(!loom::endpoint_profile<unknown_caps_profile>);
    static_assert(!loom::endpoint_profile<unknown_modes_profile>);
    static_assert(!loom::endpoint_profile<runtime_profile>);
    static_assert(!accepts_send<loom::contract_endpoint<receive_profile>>);
    static_assert(
        std::same_as<checked_endpoint, loom::provider_endpoint<loom::provider::tcp_tag, 32>>);
    static_assert(!std::constructible_from<checked_endpoint, loom::endpoint>);
    static_assert(std::same_as<decltype(std::declval<checked_endpoint&>().underlying()),
                               const loom::endpoint&>);
    static_assert(checked_endpoint::max_inject_size() == 32);
    static_assert(checked_endpoint::can_inject(32));
    static_assert(!checked_endpoint::can_inject(33));
    static_assert(accepts_inject<checked_endpoint, std::span<const std::byte, 32>>);
    static_assert(accepts_inject<checked_endpoint, std::span<std::byte, 32>>);
    static_assert(!accepts_inject<checked_endpoint, std::span<const std::byte, 33>>);
    static_assert(!accepts_inject<checked_endpoint, std::span<std::byte, 33>>);
    using no_inject =
        loom::contract_endpoint<loom::message_endpoint_profile<loom::rdm_endpoint_tag>>;
    static_assert(!no_inject::can_inject(0));
    static_assert(!accepts_inject<no_inject, std::span<const std::byte>>);
    static_assert(!accepts_inject<no_inject, std::span<const std::byte, 0>>);
}

TEST_CASE("endpoint contracts depend on capabilities rather than branding", "[provider_contract]") {
    checked_provider native;
    // No fabric attributes or provider name are needed for a capability proof.
    REQUIRE(checked_endpoint::create(native.domain, native.info));
    REQUIRE(native.opened == 1);
    REQUIRE(native.closed == 1);
}

TEST_CASE("endpoint contracts reject incompatible typed attributes", "[provider_contract]") {
    checked_provider native;
    SECTION("wrong endpoint type") {
        native.ep_attr.type = FI_EP_MSG;
    }
    SECTION("unknown endpoint type") {
        native.ep_attr.type = FI_EP_UNSPEC;
    }
    SECTION("receive-only capabilities") {
        native.native_info.caps = FI_MSG | FI_RECV;
    }
    SECTION("send-only capabilities") {
        native.native_info.caps = FI_MSG | FI_SEND;
    }
    SECTION("unsupported mode") {
        native.native_info.mode = FI_MSG_PREFIX;
    }
    SECTION("unsupported transmit mode") {
        native.tx_attr.mode = FI_CONTEXT2;
    }
    SECTION("unsupported receive mode") {
        native.rx_attr.mode = FI_CONTEXT2;
    }
    SECTION("unrepresented native mode") {
        native.native_info.mode = FI_ASYNC_IOV;
    }
    SECTION("unknown native mode") {
        native.native_info.mode = 1ULL << 63;
    }
    SECTION("message size below inject bound") {
        native.ep_attr.max_msg_size = 31;
    }
    auto result = checked_endpoint::create(native.domain, native.info);
    REQUIRE_FALSE(result);
    REQUIRE(result.error() == loom::errc::not_supported);
    REQUIRE(native.opened == 0);
    REQUIRE(native.queries == 0);
}

TEST_CASE("extended profiles validate every required capability", "[provider_contract]") {
    checked_provider native;
    const bool supported = GENERATE(false, true);
    if (supported)
        native.native_info.caps |= FI_RMA;
    auto result = loom::contract_endpoint<rma_profile>::create(native.domain, native.info);
    REQUIRE(static_cast<bool>(result) == supported);
    REQUIRE(native.opened == (supported ? 1 : 0));
    if (!result)
        REQUIRE(result.error() == loom::errc::not_supported);
}

TEST_CASE("unknown native endpoint types cannot become message endpoints", "[provider_contract]") {
    checked_provider native;
    native.ep_attr.type = FI_EP_UNSPEC;
    using message_endpoint =
        loom::contract_endpoint<loom::message_endpoint_profile<loom::msg_endpoint_tag, 32>>;
    auto result = message_endpoint::create(native.domain, native.info);
    REQUIRE_FALSE(result);
    REQUIRE(result.error() == loom::errc::not_supported);
    REQUIRE(native.opened == 0);
}

TEST_CASE("profiles declare supported mode obligations explicitly", "[provider_contract]") {
    checked_provider native;
    native.native_info.mode = FI_CONTEXT2;
    native.tx_attr.mode = FI_CONTEXT2;
    native.rx_attr.mode = FI_CONTEXT2;
    REQUIRE(loom::contract_endpoint<context_profile>::create(native.domain, native.info));
}

TEST_CASE("domain policy requirements use the opened domain", "[provider_contract]") {
    checked_provider native;
    fi_domain_attr attributes{.threading = FI_THREAD_SAFE,
                              .control_progress = FI_PROGRESS_MANUAL,
                              .data_progress = FI_PROGRESS_AUTO};
    native.native_info.domain_attr = &attributes;
    auto* domain = native.domain_state;
    bool supported = false;
    SECTION("matching policies") {
        supported = true;
    }
    SECTION("wrong threading") {
        domain->threading = loom::threading_mode::domain;
    }
    SECTION("wrong control progress") {
        domain->control_progress = loom::progress_mode::auto_progress;
    }
    SECTION("wrong data progress") {
        domain->data_progress = loom::progress_mode::manual;
    }
    SECTION("unknown data progress") {
        domain->data_progress = loom::progress_mode::unspecified;
    }
    auto result = loom::contract_endpoint<progress_profile>::create(native.domain, native.info);
    REQUIRE(static_cast<bool>(result) == supported);
    REQUIRE(native.opened == (supported ? 1 : 0));
    if (!result)
        REQUIRE(result.error() == loom::errc::not_supported);
}

TEMPLATE_TEST_CASE("endpoint contracts distinguish endpoint and FID serialization",
                   "[provider_contract][threading]",
                   threading_profile<loom::threading_mode::fid>,
                   threading_profile<loom::threading_mode::endpoint>) {
    const auto native_mode = GENERATE(FI_THREAD_FID, FI_THREAD_ENDPOINT);
    checked_provider native;
    native.domain_state->threading = loom::detail::from_fi_threading(native_mode);
    const bool matches = TestType::threading == loom::threading_mode::endpoint
                             ? native_mode == FI_THREAD_ENDPOINT
                             : native_mode == FI_THREAD_FID;
    auto result = loom::contract_endpoint<TestType>::create(native.domain, native.info);
    CHECK(result.has_value() == matches);
    CHECK(native.opened == (matches ? 1 : 0));
    if (!result)
        CHECK(result.error() == loom::errc::not_supported);
}

TEST_CASE("provider construction requires discovery evidence", "[provider_contract]") {
    checked_provider native;
    auto expected = loom::errc::not_supported;
    SECTION("empty domain") {
        native.domain = {};
        expected = loom::errc::invalid_argument;
    }
    SECTION("empty discovery") {
        native.info = {};
        expected = loom::errc::invalid_argument;
    }
    SECTION("missing endpoint attributes") {
        native.native_info.ep_attr = nullptr;
        expected = loom::errc::invalid_argument;
    }
    SECTION("missing messaging capability") {
        native.native_info.caps = FI_RMA;
    }
    auto result = checked_endpoint::create(native.domain, native.info);
    REQUIRE_FALSE(result);
    REQUIRE(result.error() == expected);
    REQUIRE(native.opened == 0);
}

TEST_CASE("chosen inject bounds are established before returning an endpoint",
          "[provider_contract]") {
    checked_provider native;
    native.inject_limit = GENERATE(0, 16, 31, 32, 64);
    {
        auto result =
            loom::create_provider_endpoint<loom::provider::tcp_tag, 32>(native.domain, native.info);
        REQUIRE(static_cast<bool>(result) == (native.inject_limit >= 32));
        REQUIRE(native.opened == 1);
        if (result) {
            REQUIRE(static_cast<bool>(*result));
            REQUIRE(native.closed == 0);
        } else {
            REQUIRE(result.error() == loom::errc::not_supported);
            REQUIRE(native.closed == 1);
        }
    }
    REQUIRE(native.closed == 1);
}

TEST_CASE("provider construction preserves native failures", "[provider_contract]") {
    checked_provider native;
    auto expected = loom::errc::io_error;
    SECTION("endpoint creation failure") {
        native.open_result = -FI_EIO;
    }
    SECTION("size query failure") {
        native.query_result = -FI_EIO;
    }
    SECTION("size query malformed result") {
        native.returned_length = 1;
    }
    SECTION("unsupported option uses insufficient creation bound") {
        native.query_result = -FI_ENOPROTOOPT;
        native.tx_attr.inject_size = 16;
        expected = loom::errc::not_supported;
    }
    const auto result = checked_endpoint::create(native.domain, native.info);
    REQUIRE_FALSE(result);
    REQUIRE(result.error() == expected);
    REQUIRE(native.closed == native.opened);
}

TEST_CASE("unsupported inject queries use the selected creation bound", "[provider_contract]") {
    checked_provider native;
    native.query_result = GENERATE(-FI_ENOPROTOOPT, -FI_ENOSYS);
    {
        auto result = checked_endpoint::create(native.domain, native.info);
        REQUIRE(result);
        REQUIRE(result->max_inject_size() == 32);
    }
    auto too_large =
        loom::create_provider_endpoint<loom::provider::tcp_tag>(native.domain, native.info);
    REQUIRE_FALSE(too_large);
    REQUIRE(too_large.error() == loom::errc::not_supported);
}

TEST_CASE("checked injection retains peers and native backpressure", "[provider_contract]") {
    checked_provider native;
    native.submission_result = GENERATE(0, -FI_EAGAIN, -FI_EIO);
    auto result = checked_endpoint::create(native.domain, native.info);
    REQUIRE(result);
    REQUIRE(result->enable());
    const auto queries = native.queries;
    std::array<std::byte, 33> data{};
    const bool fixed_extent = GENERATE(false, true);
    auto submitted =
        fixed_extent
            ? result->inject(std::span{data}.first<32>(), loom::fabric_addr{91})
            : result->inject(std::span<const std::byte>{data.data(), 32}, loom::fabric_addr{91});
    REQUIRE(static_cast<bool>(submitted) == (native.submission_result == 0));
    if (!submitted)
        REQUIRE(submitted.error().value() == native.submission_result);
    REQUIRE(native.submitted == 1);
    REQUIRE(native.submitted_buffer == data.data());
    REQUIRE(native.submitted_length == 32);
    REQUIRE(native.destination == 91);
    REQUIRE(native.queries == queries);
    auto oversized = result->inject(std::span<const std::byte>{data});
    REQUIRE_FALSE(oversized);
    REQUIRE(oversized.error() == loom::errc::message_too_long);
    REQUIRE(native.submitted == 1);
}

TEST_CASE("enabling cannot leave an invalid inject contract usable", "[provider_contract]") {
    checked_provider native;
    auto result = checked_endpoint::create(native.domain, native.info);
    REQUIRE(result);
    auto expected = loom::errc::not_supported;
    SECTION("enable reduces the message inject limit") {
        native.enabled_limit = 16;
    }
    SECTION("enable fails") {
        native.enable_result = -FI_EIO;
        expected = loom::errc::io_error;
    }
    SECTION("post-enable query fails") {
        native.query_result = -FI_EIO;
        expected = loom::errc::io_error;
    }
    REQUIRE(result->enable().error() == expected);
    REQUIRE_FALSE(static_cast<bool>(*result));
    std::array<std::byte, 8> payload{};
    REQUIRE(result->inject(std::span{payload}).error() == loom::errc::invalid_argument);
    REQUIRE(result->send(payload).error() == loom::errc::invalid_argument);
    REQUIRE(result->recv(payload).error() == loom::errc::invalid_argument);
    REQUIRE(native.submitted == 0);
}

TEST_CASE("resource binding revalidates the inject contract", "[provider_contract]") {
    checked_provider native;
    fid_cntr handle{.fid = {.ops = &native.parent_ops}};
    loom::counter counter{loom::detail::make_pmr_unique<loom::detail::counter_impl>(
        loom::get_default_resource(), &handle)};
    auto result = checked_endpoint::create(native.domain, native.info);
    REQUIRE(result);
    native.enabled_limit = GENERATE(16, 64);
    auto bound = result->bind_counter(counter, FI_SEND);
    REQUIRE(static_cast<bool>(bound) == (native.enabled_limit >= 32));
    REQUIRE(static_cast<bool>(*result) == static_cast<bool>(bound));
    std::array<std::byte, 8> payload{};
    REQUIRE(static_cast<bool>(result->inject(std::span{payload})) == static_cast<bool>(bound));
    REQUIRE(native.submitted == (bound ? 1 : 0));
}

TEST_CASE("moving and releasing provider endpoints preserves ownership", "[provider_contract]") {
    checked_provider native;
    auto result = checked_endpoint::create(native.domain, native.info);
    REQUIRE(result);
    auto moved = std::move(*result);
    REQUIRE_FALSE(static_cast<bool>(*result));
    REQUIRE(static_cast<bool>(moved));
    std::array<std::byte, 8> payload{};
    REQUIRE(result->inject(std::span{payload}).error() == loom::errc::invalid_argument);
    {
        auto generic = std::move(moved).release();
        REQUIRE(generic);
        REQUIRE_FALSE(static_cast<bool>(moved));
        REQUIRE(generic.inject(payload));
        REQUIRE(native.closed == 0);
    }
    REQUIRE(native.closed == 1);
}

TEST_CASE("zero inject contracts do not invent zero-byte injection", "[provider_contract]") {
    checked_provider native;
    native.inject_limit = 0;
    native.query_result = -FI_EIO;
    auto result =
        loom::create_provider_endpoint<loom::provider::tcp_tag, 0>(native.domain, native.info);
    REQUIRE(result);
    REQUIRE(result->enable());
    REQUIRE(native.queries == 0);
}

TEST_CASE("checked endpoint profiles use real discovery limits",
          "[provider_contract][integration]") {
    loom::fabric_hints hints;
    hints.provider_name = "tcp";  // A discovery hint, not evidence of any guarantee.
    hints.ep_type = message_profile::ep_type;
    hints.capabilities = message_profile::required_caps;
    auto discovered = loom::query_fabric(hints);
    if (!discovered)
        SKIP("TCP provider unavailable");
    for (const auto& info : *discovered) {
        auto fabric = loom::fabric::create(info);
        REQUIRE(fabric);
        auto domain = loom::domain::create(*fabric, info);
        REQUIRE(domain);
        auto ep = checked_endpoint::create(*domain, info);
        REQUIRE(ep);
        REQUIRE(ep->underlying().message_inject_size().value() >= 32);
        using wrong_type =
            loom::contract_endpoint<loom::message_endpoint_profile<loom::dgram_endpoint_tag, 32>>;
        auto wrong = wrong_type::create(*domain, info);
        REQUIRE_FALSE(wrong);
        REQUIRE(wrong.error() == loom::errc::not_supported);
        return;
    }
    SKIP("No matching discovery entries");
}
