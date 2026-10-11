// SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only
#include <loom/core/endpoint.hpp>
#include <loom/detail/impl/endpoint_impl.hpp>

#include <utility>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

namespace {
struct closing_provider {
    int result{};
    int attempts{};
    fi_ops ops{
        .size = sizeof(fi_ops),
        .close =
            [](fid* handle) {
                auto& self = *static_cast<closing_provider*>(handle->context);
                ++self.attempts;
                return self.result;
            },
    };
    fid_ep handle{.fid = {.context = this, .ops = &ops}};

    auto endpoint() -> loom::endpoint {
        return loom::endpoint{loom::detail::make_pmr_unique<loom::detail::endpoint_impl>(
            loom::get_default_resource(),
            &handle,
            loom::endpoint_types::rdm,
            loom::address_format::unspecified)};
    }
};
}  // namespace

TEST_CASE("explicit endpoint close releases ownership exactly once", "[endpoint][close]") {
    closing_provider native;
    {
        auto ep = native.endpoint();
        REQUIRE(ep.close());
        REQUIRE_FALSE(ep);
        REQUIRE(ep.internal_ptr() == nullptr);
        REQUIRE(ep.get_type().error() == loom::errc::invalid_argument);
        REQUIRE(ep.send({}).error() == loom::errc::invalid_argument);
        REQUIRE(ep.close());
        REQUIRE(native.attempts == 1);
    }
    REQUIRE(native.attempts == 1);
}

TEST_CASE("failed endpoint close preserves the handle for retry", "[endpoint][close]") {
    closing_provider native;
    native.result = GENERATE(-FI_EBUSY, -FI_EIO, -FI_EAGAIN);
    {
        auto ep = native.endpoint();
        auto result = ep.close();
        REQUIRE_FALSE(result);
        REQUIRE(result.error().value() == native.result);
        REQUIRE(ep);
        REQUIRE(ep.internal_ptr() == &native.handle);
        REQUIRE(native.attempts == 1);
        native.result = 0;
        auto moved = std::move(ep);
        REQUIRE(ep.close());
        REQUIRE(native.attempts == 1);
        REQUIRE(moved.close());
        REQUIRE(native.attempts == 2);
        REQUIRE_FALSE(moved);
    }
    REQUIRE(native.attempts == 2);
}

TEST_CASE("destruction can retry a previously failed endpoint close", "[endpoint][close]") {
    closing_provider native;
    {
        auto ep = native.endpoint();
        native.result = -FI_EBUSY;
        REQUIRE_FALSE(ep.close());
        native.result = 0;
    }
    REQUIRE(native.attempts == 2);
}

TEST_CASE("empty endpoint close is idempotent", "[endpoint][close]") {
    loom::endpoint ep;
    REQUIRE(ep.close());
    REQUIRE(ep.close());
}
