// SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only
#include <loom/core/types.hpp>
#include <loom/detail/conversions.hpp>

#include <catch2/catch_test_macros.hpp>

TEST_CASE("extended context mode survives provider negotiation", "[types]") {
    const auto modes = loom::mode_bits::context2 | loom::mode_bits::rx_cq_data;
    const auto provider_modes = FI_CONTEXT2 | FI_RX_CQ_DATA;
    REQUIRE(loom::detail::to_fi_mode(modes) == provider_modes);
    REQUIRE(loom::detail::from_fi_mode(provider_modes) == modes);
}

TEST_CASE("device memory and collective capabilities reach providers", "[types]") {
    const auto capabilities = loom::capability::hmem | loom::capability::collective;
    REQUIRE(loom::detail::to_fi_caps(capabilities) == (FI_HMEM | FI_COLLECTIVE));
    REQUIRE(loom::detail::from_fi_caps(FI_HMEM | FI_COLLECTIVE) == capabilities);
}

TEST_CASE("fabric_version equality", "[types]") {
    loom::fabric_version ver1{0x12345678U};
    loom::fabric_version ver2{0x12345678U};
    loom::fabric_version ver3{0x87654321U};

    REQUIRE(ver1 == ver2);
    REQUIRE(ver1 != ver3);
}

TEST_CASE("fabric_version get()", "[types]") {
    loom::fabric_version ver{0x12345678U};
    REQUIRE(ver.get() == 0x12345678U);
}
