// SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only
#include <loom/core/types.hpp>

#include "ut.hpp"

using namespace boost::ut;

suite types_suite = [] {
    "fabric_version equality"_test = [] {
        loom::fabric_version ver1{0x12345678U};
        loom::fabric_version ver2{0x12345678U};
        loom::fabric_version ver3{0x87654321U};

        expect(ver1 == ver2) << "Equal versions should compare equal";
        expect(ver1 != ver3) << "Different versions should not compare equal";
    };

    "fabric_version get()"_test = [] {
        loom::fabric_version ver{0x12345678U};
        expect(ver.get() == 0x12345678U) << "get() should return correct value";
    };
};

auto main() -> int {}
