// SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only
#include <loom/core/safe_context.hpp>

#include <algorithm>
#include <catch2/catch_test_macros.hpp>

TEST_CASE("provider context writes preserve application metadata", "[context]") {
    loom::safe_context context;
    int operation{};
    loom::store_safe_context(context, &operation);

    // FI_CONTEXT2 providers may overwrite every internal slot.
    std::ranges::fill(context.provider.internal, nullptr);
    REQUIRE(loom::is_context_valid(&context.provider));
    REQUIRE(loom::recover_safe_context<int>(&context.provider) == &operation);

    loom::invalidate_safe_context(context);
    REQUIRE_FALSE(loom::is_context_valid(&context.provider));
    REQUIRE(loom::recover_safe_context<int>(nullptr) == nullptr);
}
