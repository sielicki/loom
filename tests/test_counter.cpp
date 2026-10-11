// SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only
#include <loom/core/counter.hpp>
#include <loom/detail/impl/domain_impl.hpp>

#include <limits>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

namespace {
struct counter_provider {
    std::uint64_t value{};
    std::uint64_t waited_threshold{};
    int waited_timeout{};
    int wait_calls{};
    fi_wait_obj wait_object{};
    fi_ops resource_ops{
        .size = sizeof(fi_ops),
        .close = [](fid*) { return 0; },
    };
    fi_ops_cntr counter_ops{
        .size = sizeof(fi_ops_cntr),
        .read =
            [](fid_cntr* cntr) { return static_cast<counter_provider*>(cntr->fid.context)->value; },
        .set =
            [](fid_cntr* cntr, std::uint64_t value) {
                static_cast<counter_provider*>(cntr->fid.context)->value = value;
                return 0;
            },
        .wait =
            [](fid_cntr* cntr, std::uint64_t threshold, int timeout) {
                auto& self = *static_cast<counter_provider*>(cntr->fid.context);
                self.waited_threshold = threshold;
                self.waited_timeout = timeout;
                ++self.wait_calls;
                return self.value >= threshold ? 0 : -FI_ETIMEDOUT;
            },
    };
    fid_cntr counter_handle{.fid = {.context = this, .ops = &resource_ops}, .ops = &counter_ops};
    fi_ops_domain domain_ops{
        .size = sizeof(fi_ops_domain),
        .cntr_open =
            [](fid_domain* dom, fi_cntr_attr* attr, fid_cntr** result, void*) {
                auto& self = *static_cast<counter_provider*>(dom->fid.context);
                self.wait_object = attr->wait_obj;
                *result = &self.counter_handle;
                return 0;
            },
    };
    fid_domain domain_handle{.fid = {.context = this, .ops = &resource_ops}, .ops = &domain_ops};
    loom::domain domain{loom::detail::make_pmr_unique<loom::detail::domain_impl>(
        loom::get_default_resource(), &domain_handle)};
};
}  // namespace

TEST_CASE("counter waits and checks respect the configured threshold", "[counter]") {
    const auto threshold =
        GENERATE(std::uint64_t{0}, std::uint64_t{7}, std::numeric_limits<std::uint64_t>::max());
    counter_provider provider;
    auto created = loom::counter::create(
        provider.domain,
        {.initial = threshold == 0 ? 0 : threshold - 1, .threshold = threshold, .wait_obj = true});
    REQUIRE(created);
    REQUIRE(created->check_threshold() == (threshold == 0));
    auto wait = created->wait(std::chrono::milliseconds{0});
    REQUIRE(wait.has_value() == (threshold == 0));
    if (!wait)
        REQUIRE(wait.error() == loom::errc::timeout);
    REQUIRE(provider.waited_threshold == threshold);
    REQUIRE(provider.waited_timeout == 0);
    REQUIRE(provider.wait_object == FI_WAIT_UNSPEC);

    auto moved = std::move(*created);
    REQUIRE_FALSE(created->check_threshold());
    REQUIRE(moved.set(threshold));
    REQUIRE(moved.check_threshold());
    REQUIRE(moved.wait());
    REQUIRE(provider.waited_threshold == threshold);
    REQUIRE(provider.waited_timeout == -1);
}

TEST_CASE("counter waits reject counters without wait support", "[counter]") {
    counter_provider provider;
    auto created = loom::counter::create(provider.domain);
    REQUIRE(created);
    REQUIRE(provider.wait_object == FI_WAIT_NONE);
    auto wait = created->wait(std::chrono::milliseconds{0});
    REQUIRE_FALSE(wait);
    REQUIRE(wait.error() == loom::errc::invalid_argument);
    REQUIRE(provider.wait_calls == 0);
}

TEST_CASE("counter waits preserve finite and infinite timeout values", "[counter]") {
    using namespace std::chrono;
    counter_provider provider;
    auto created = loom::counter::create(provider.domain, {.initial = 1, .wait_obj = true});
    REQUIRE(created);
    SECTION("largest representable finite timeout") {
        REQUIRE(created->wait(milliseconds{std::numeric_limits<int>::max()}));
        REQUIRE(provider.waited_timeout == std::numeric_limits<int>::max());
    }
    SECTION("negative timeouts keep native infinite semantics") {
        REQUIRE(created->wait(milliseconds::min()));
        REQUIRE(provider.waited_timeout == -1);
    }
    SECTION("unrepresentable positive timeouts cannot become infinite") {
        const auto timeout =
            GENERATE(milliseconds{std::numeric_limits<int>::max()} + 1ms, milliseconds::max());
        const auto wait = created->wait(timeout);
        REQUIRE_FALSE(wait);
        REQUIRE(wait.error() == loom::errc::invalid_argument);
        REQUIRE(provider.wait_calls == 0);
    }
}
