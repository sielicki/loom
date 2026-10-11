// SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only
#include <loom/core/fabric.hpp>
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
