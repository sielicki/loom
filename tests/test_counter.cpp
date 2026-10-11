// SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only
#include <loom/core/counter.hpp>
#include <loom/detail/impl/domain_impl.hpp>
#include <loom/detail/impl/fabric_impl.hpp>

#include <limits>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

namespace {
struct counter_provider {
    std::uint64_t value{};
    std::uint64_t waited_threshold{};
    int waited_timeout{};
    int wait_calls{};
    int closes{};
    int set_result{};
    int set_calls{};
    int add_calls{};
    int open_calls{};
    fi_wait_obj wait_object{};
    fi_ops resource_ops{
        .size = sizeof(fi_ops),
        .close =
            [](fid* handle) {
                ++static_cast<counter_provider*>(handle->context)->closes;
                return 0;
            },
    };
    fi_ops_cntr counter_ops{
        .size = sizeof(fi_ops_cntr),
        .read =
            [](fid_cntr* cntr) { return static_cast<counter_provider*>(cntr->fid.context)->value; },
        .add =
            [](fid_cntr* cntr, std::uint64_t value) {
                auto& self = *static_cast<counter_provider*>(cntr->fid.context);
                ++self.add_calls;
                if (value > std::numeric_limits<std::uint64_t>::max() - self.value)
                    return -FI_EOVERFLOW;
                self.value += value;
                return 0;
            },
        .set =
            [](fid_cntr* cntr, std::uint64_t value) {
                auto& self = *static_cast<counter_provider*>(cntr->fid.context);
                ++self.set_calls;
                if (self.set_result != 0)
                    return self.set_result;
                self.value = value;
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
                ++self.open_calls;
                self.wait_object = attr->wait_obj;
                *result = &self.counter_handle;
                return 0;
            },
    };
    fid_domain domain_handle{.fid = {.context = this, .ops = &resource_ops}, .ops = &domain_ops};
    fi_ops_fabric fabric_ops{
        .size = sizeof(fi_ops_fabric),
        .domain =
            [](fid_fabric* fabric, fi_info*, fid_domain** output, void*) {
                *output = &static_cast<counter_provider*>(fabric->fid.context)->domain_handle;
                return 0;
            },
    };
    fid_fabric native_fabric{.fid = {.context = this, .ops = &resource_ops}, .ops = &fabric_ops};
    loom::fabric fabric{loom::detail::make_pmr_unique<loom::detail::fabric_impl>(
        loom::get_default_resource(), &native_fabric, nullptr)};
    loom::domain domain{loom::detail::make_pmr_unique<loom::detail::domain_impl>(
        loom::get_default_resource(), &domain_handle)};

    auto bounded_domain(std::uint64_t maximum,
                        std::uint64_t error_maximum,
                        loom::memory_resource* resource = nullptr) -> loom::domain {
        fi_domain_attr attributes{};
        attributes.mr_mode = FI_MR_PROV_KEY;
        attributes.max_cntr_value = maximum;
        attributes.max_err_cntr_value = error_maximum;
        fi_info native_info{};
        native_info.domain_attr = &attributes;
        loom::fabric_info info{loom::detail::make_pmr_unique<loom::detail::fabric_info_impl>(
            loom::get_default_resource(), &native_info, false)};
        auto result = loom::domain::create(fabric, info, resource);
        REQUIRE(result);
        return std::move(*result);
    }
};
}  // namespace

TEST_CASE("counter creation rejects values outside the negotiated range", "[counter][limits]") {
    const auto initial = GENERATE(false, true);
    counter_provider provider;
    auto domain = provider.bounded_domain(7, 3);
    const auto counter = loom::counter::create(
        domain,
        {.initial = initial ? 8UL : 0UL, .threshold = initial ? 1UL : 8UL, .wait_obj = true});
    REQUIRE_FALSE(counter);
    REQUIRE(counter.error() == loom::errc::invalid_argument);
    REQUIRE(provider.open_calls == 0);
    REQUIRE(provider.set_calls == 0);
}

TEST_CASE("counter operations reject unreachable values before native calls", "[counter][limits]") {
    const auto operation = GENERATE(0, 1, 2);
    counter_provider provider;
    auto domain = provider.bounded_domain(7, 3);
    auto counter = loom::counter::create(domain, {.wait_obj = true});
    REQUIRE(counter);
    const auto result = operation == 0   ? counter->wait_until(8, std::chrono::milliseconds{0})
                        : operation == 1 ? counter->set(8)
                                         : counter->add(8);
    REQUIRE_FALSE(result);
    REQUIRE(result.error() == loom::errc::invalid_argument);
    REQUIRE(provider.wait_calls == 0);
    REQUIRE(provider.set_calls == 0);
    REQUIRE(provider.add_calls == 0);
}

TEST_CASE("counter limits survive query destruction and counter moves", "[counter][limits]") {
    constexpr auto full_range = std::numeric_limits<std::uint64_t>::max();
    const auto maximum =
        GENERATE_COPY(std::uint64_t{0}, std::uint64_t{7}, full_range - 1, full_range);
    const auto errors = GENERATE_COPY(std::uint64_t{0}, std::uint64_t{3}, full_range);
    const auto custom_resource = GENERATE(false, true);
    counter_provider provider;
    std::pmr::monotonic_buffer_resource resource;
    auto domain = provider.bounded_domain(maximum, errors, custom_resource ? &resource : nullptr);
    const auto bound = maximum ? maximum : full_range;
    auto created =
        loom::counter::create(domain, {.initial = bound, .threshold = bound, .wait_obj = true});
    REQUIRE(created);
    auto counter = std::move(*created);
    REQUIRE(counter.max_value() == bound);
    REQUIRE(counter.max_error_value() == (errors ? errors : full_range));
    REQUIRE(created->max_value() == 0);
    REQUIRE(created->max_error_value() == 0);
    REQUIRE(counter.read() == bound);
    REQUIRE(counter.wait(std::chrono::milliseconds{0}));
    REQUIRE(counter.set(0));
    REQUIRE(counter.add(bound));
    REQUIRE(counter.wait_until(bound, std::chrono::milliseconds{0}));
    REQUIRE(counter.read() == bound);
}

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

TEST_CASE("counter creation closes each acquired native handle exactly once", "[counter]") {
    counter_provider provider;
    SECTION("allocation failure") {
        REQUIRE_THROWS_AS(
            loom::counter::create(provider.domain, {}, std::pmr::null_memory_resource()),
            std::bad_alloc);
    }
    SECTION("initialization failure") {
        provider.set_result = -FI_EIO;
        auto created = loom::counter::create(
            provider.domain, {.initial = 1}, std::pmr::null_memory_resource());
        REQUIRE_FALSE(created);
        REQUIRE(created.error() == loom::errc::io_error);
    }
    SECTION("successful ownership transfer") {
        auto created = loom::counter::create(provider.domain);
        REQUIRE(created);
        REQUIRE(provider.closes == 0);
    }
    REQUIRE(provider.closes == 1);
}

TEST_CASE("counter waits can advance cumulative targets without rewriting completions",
          "[counter]") {
    const auto count = GENERATE(
        std::uint64_t{1}, std::uint64_t{1} << 63, std::numeric_limits<std::uint64_t>::max() - 1);
    counter_provider provider;
    auto created = loom::counter::create(provider.domain, {.initial = count, .wait_obj = true});
    REQUIRE(created);
    REQUIRE(created->wait_until(count, std::chrono::milliseconds{0}));
    const auto pending = created->wait_until(count + 1, std::chrono::milliseconds{0});
    REQUIRE_FALSE(pending);
    REQUIRE(pending.error() == loom::errc::timeout);
    REQUIRE(provider.waited_threshold == count + 1);
    ++provider.value;  // Model another completion arriving at the provider.
    auto moved = std::move(*created);
    REQUIRE(moved.wait_until(count + 1, std::chrono::milliseconds{0}));
    REQUIRE(moved.read() == count + 1);
    REQUIRE(moved.wait(std::chrono::milliseconds{0}));
    REQUIRE(provider.waited_threshold == 1);  // Explicit targets leave the default unchanged.
    REQUIRE(provider.add_calls == 0);
    REQUIRE(provider.set_calls == 1);  // Only counter initialization wrote to the provider.
    REQUIRE_FALSE(created->wait_until(count));
}

TEST_CASE("explicit counter targets retain wait object and timeout checks", "[counter]") {
    using namespace std::chrono;
    counter_provider provider;
    auto polling = loom::counter::create(provider.domain);
    REQUIRE(polling);
    REQUIRE(polling->wait_until(0).error() == loom::errc::invalid_argument);
    auto waitable = loom::counter::create(provider.domain, {.wait_obj = true});
    REQUIRE(waitable);
    REQUIRE(waitable->wait_until(0, milliseconds::min()));
    REQUIRE(provider.waited_timeout == -1);
    const auto invalid =
        waitable->wait_until(0, milliseconds{std::numeric_limits<int>::max()} + 1ms);
    REQUIRE_FALSE(invalid);
    REQUIRE(invalid.error() == loom::errc::invalid_argument);
    REQUIRE(provider.wait_calls == 1);
}
