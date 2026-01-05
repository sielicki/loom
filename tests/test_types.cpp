// SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only
#include <loom/core/types.hpp>

#include <catch2/catch_test_macros.hpp>

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
