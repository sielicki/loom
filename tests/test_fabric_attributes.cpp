// SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only
#include <loom/core/fabric.hpp>
#include <loom/core/progress_policy.hpp>
#include <loom/detail/conversions.hpp>
#include <loom/detail/impl/fabric_impl.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

namespace {
auto wrap(fi_info& native) -> loom::fabric_info {
    return loom::fabric_info{loom::detail::make_pmr_unique<loom::detail::fabric_info_impl>(
        loom::get_default_resource(), &native, false)};
}
}  // namespace

TEST_CASE("endpoint completion ordering converts the native encoding", "[fabric][attributes]") {
    const auto native_order = GENERATE(std::uint64_t{0},
                                       std::uint64_t{FI_ORDER_STRICT},
                                       std::uint64_t{FI_ORDER_DATA},
                                       std::uint64_t{FI_ORDER_STRICT | FI_ORDER_DATA});
    fi_ep_attr endpoint{};
    fi_tx_attr transmit{};
    transmit.comp_order = native_order;
    fi_info native{};
    native.ep_attr = &endpoint;
    native.tx_attr = &transmit;
    const auto info = wrap(native);
    const auto attributes = info.get_endpoint_attr();
    REQUIRE(attributes);
    auto expected = loom::completion_ordering::none;
    if (native_order & FI_ORDER_DATA)
        expected |= loom::completion_ordering::data;
    if ((native_order & FI_ORDER_STRICT) == FI_ORDER_STRICT)
        expected |= loom::completion_ordering::strict;
    REQUIRE(attributes->comp_ordering == expected);
}

TEST_CASE("endpoint receive completion ordering remains independent of transmit",
          "[fabric][attributes]") {
    const auto missing = GENERATE(0, 1, 2, 3);
    fi_ep_attr endpoint{};
    fi_tx_attr transmit{};
    fi_rx_attr receive{};
    transmit.comp_order = FI_ORDER_STRICT;
    receive.comp_order = FI_ORDER_DATA;
    fi_info native{};
    native.ep_attr = &endpoint;
    native.tx_attr = missing & 1 ? nullptr : &transmit;
    native.rx_attr = missing & 2 ? nullptr : &receive;
    const auto info = wrap(native);
    const auto attributes = info.get_endpoint_attr();
    REQUIRE(attributes);
    REQUIRE(attributes->comp_ordering ==
            (missing & 1 ? loom::completion_ordering::none : loom::completion_ordering::strict));
    REQUIRE(attributes->rx_comp_ordering ==
            (missing & 2 ? loom::completion_ordering::none : loom::completion_ordering::data));
}

TEST_CASE("partial native masks do not invent completion ordering guarantees",
          "[fabric][attributes]") {
    fi_ep_attr endpoint{};
    fi_tx_attr transmit{};
    fi_rx_attr receive{};
    fi_info native{};
    native.ep_attr = &endpoint;
    native.tx_attr = &transmit;
    native.rx_attr = &receive;
    const auto info = wrap(native);
    for (std::uint64_t mask = 0; mask <= FI_ORDER_STRICT; ++mask) {
        INFO("native mask: " << mask);
        transmit.comp_order = mask;
        receive.comp_order = mask | FI_ORDER_DATA;
        const auto attributes = info.get_endpoint_attr();
        REQUIRE(attributes);
        REQUIRE(attributes->comp_ordering.has(loom::completion_ordering::strict) ==
                (mask == FI_ORDER_STRICT));
        REQUIRE_FALSE(attributes->comp_ordering.has(loom::completion_ordering::data));
        REQUIRE(attributes->rx_comp_ordering.has(loom::completion_ordering::strict) ==
                (mask == FI_ORDER_STRICT));
        REQUIRE(attributes->rx_comp_ordering.has(loom::completion_ordering::data));
    }
}

TEST_CASE("unified control progress survives negotiation and requires polling",
          "[fabric][attributes][progress]") {
    fi_domain_attr domain{};
    domain.control_progress = FI_PROGRESS_CONTROL_UNIFIED;
    domain.data_progress = FI_PROGRESS_MANUAL;
    fi_info native{};
    native.domain_attr = &domain;
    const auto info = wrap(native);
    const auto attributes = info.get_domain_attr();
    REQUIRE(attributes);
    REQUIRE(attributes->control_progress == loom::progress_mode::control_unified);
    CHECK(loom::detail::to_fi_progress(attributes->control_progress) ==
          FI_PROGRESS_CONTROL_UNIFIED);
    const loom::runtime_progress_policy policy{attributes->control_progress,
                                               attributes->data_progress};
    CHECK(policy.requires_manual_control_progress());
    CHECK(policy.requires_manual_data_progress());
    CHECK_FALSE(policy.supports_blocking_wait());
    using static_policy =
        loom::static_progress_policy<loom::detail::from_fi_progress(FI_PROGRESS_CONTROL_UNIFIED),
                                     loom::progress_mode::manual>;
    CHECK(static_policy::requires_manual_control_progress());
    CHECK(static_policy::requires_manual_data_progress());
    CHECK_FALSE(static_policy::supports_blocking_wait());
}

TEST_CASE("manual-all progress requests retain the native manual requirement",
          "[fabric][attributes][progress]") {
    REQUIRE(loom::detail::to_fi_progress(loom::progress_mode::manual_all) == FI_PROGRESS_MANUAL);
}

TEST_CASE("native progress modes round trip without changing manual requirements",
          "[fabric][attributes][progress]") {
    const auto native = GENERATE(
        FI_PROGRESS_UNSPEC, FI_PROGRESS_AUTO, FI_PROGRESS_MANUAL, FI_PROGRESS_CONTROL_UNIFIED);
    const auto converted = loom::detail::from_fi_progress(native);
    REQUIRE(loom::detail::to_fi_progress(converted) == native);
    const loom::runtime_progress_policy policy{converted, converted};
    const bool manual = native == FI_PROGRESS_MANUAL || native == FI_PROGRESS_CONTROL_UNIFIED;
    REQUIRE(policy.requires_manual_control_progress() == manual);
    REQUIRE(policy.requires_manual_data_progress() == manual);
    REQUIRE(policy.supports_blocking_wait() == (native == FI_PROGRESS_AUTO));
}

TEST_CASE("fabric attributes report the selected domain's concurrency requirements",
          "[fabric][attributes][progress]") {
    const auto profile = GENERATE(0, 1, 2);
    fi_domain_attr domain{};
    domain.threading = profile == 0   ? FI_THREAD_DOMAIN
                       : profile == 1 ? FI_THREAD_COMPLETION
                                      : FI_THREAD_SAFE;
    domain.control_progress = profile == 0   ? FI_PROGRESS_CONTROL_UNIFIED
                              : profile == 1 ? FI_PROGRESS_MANUAL
                                             : FI_PROGRESS_AUTO;
    domain.data_progress = profile == 2 ? FI_PROGRESS_AUTO : FI_PROGRESS_MANUAL;
    char name[] = "test_fabric";
    char provider[] = "test_provider";
    fi_fabric_attr fabric{};
    fabric.name = name;
    fabric.prov_name = provider;
    fabric.prov_version = 42;
    fi_info native{};
    native.fabric_attr = &fabric;
    native.domain_attr = &domain;
    const auto info = wrap(native);
    const auto attributes = info.get_fabric_attr();
    const auto selected_domain = info.get_domain_attr();
    REQUIRE(attributes);
    REQUIRE(selected_domain);
    CHECK(attributes->threading == selected_domain->threading);
    CHECK(attributes->control_progress == selected_domain->control_progress);
    CHECK(attributes->data_progress == selected_domain->data_progress);
    REQUIRE(attributes->name == "test_fabric");
    REQUIRE(attributes->provider_name == "test_provider");
    REQUIRE(attributes->provider_version == loom::fabric_version{42U});
}

TEST_CASE("missing domain metadata does not imply fabric concurrency guarantees",
          "[fabric][attributes][progress]") {
    fi_fabric_attr fabric{};
    fi_info native{};
    native.fabric_attr = &fabric;
    const auto info = wrap(native);
    const auto attributes = info.get_fabric_attr();
    REQUIRE(attributes);
    CHECK(attributes->threading == loom::threading_mode::unspecified);
    CHECK(attributes->control_progress == loom::progress_mode::unspecified);
    CHECK(attributes->data_progress == loom::progress_mode::unspecified);
}

TEST_CASE("named receive contexts and message prefixes use independent namespaces",
          "[fabric][attributes][capabilities]") {
    const auto requirements = GENERATE(0, 1, 2, 3);
    fi_info native{};
    native.caps = FI_MSG | (requirements & 1 ? FI_NAMED_RX_CTX : 0);
    native.mode = requirements & 2 ? FI_MSG_PREFIX : 0;
    const auto info = wrap(native);
    const auto expected_caps =
        loom::capability::msg | (requirements & 1 ? loom::capability::named_rx_ctx : loom::caps{});
    const auto expected_mode = requirements & 2 ? loom::mode_bits::msg_prefix : loom::mode{};
    CHECK(info.get_caps() == expected_caps);
    CHECK(info.get_mode() == expected_mode);
    CHECK(loom::detail::to_fi_caps(info.get_caps()) == native.caps);
    CHECK(loom::detail::to_fi_mode(info.get_mode()) == native.mode);
}
