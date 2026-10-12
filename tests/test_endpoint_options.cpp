// SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only
#include <loom/core/endpoint_options.hpp>
#include <loom/detail/impl/endpoint_impl.hpp>

#include <rdma/fi_ext.h>

#include <array>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

namespace {
template <typename Level, typename Option>
concept loom_option_arguments =
    requires(loom::endpoint& ep, Level level, Option option, void* value, std::size_t length) {
        loom::get_endpoint_option(ep, level, option, value, &length);
        loom::set_endpoint_option(ep, level, option, value, length);
    };

template <typename Level, typename Option>
concept native_option_arguments =
    requires(loom::endpoint& ep, Level level, Option option, void* value, std::size_t length) {
        loom::get_native_endpoint_option(ep, level, option, value, &length);
        loom::set_native_endpoint_option(ep, level, option, value, length);
    };

static_assert(loom_option_arguments<loom::ep_opt::level, loom::ep_opt::option>);
static_assert(!loom_option_arguments<int, int>);
static_assert(!loom_option_arguments<decltype(FI_OPT_ENDPOINT), decltype(FI_OPT_CM_DATA_SIZE)>);
static_assert(!loom_option_arguments<loom::ep_opt::level, int>);
static_assert(!loom_option_arguments<int, loom::ep_opt::option>);
static_assert(!loom_option_arguments<loom::ep_opt::option, loom::ep_opt::level>);
static_assert(native_option_arguments<int, int>);
static_assert(!native_option_arguments<loom::ep_opt::level, loom::ep_opt::option>);
static_assert(!native_option_arguments<loom::ep_opt::level, int>);
static_assert(!native_option_arguments<int, loom::ep_opt::option>);

struct option_probe {
    int level{};
    int option{};
    int calls{};
    int status{};
    const void* value{};
    std::size_t length{};
    fi_ops ops{.size = sizeof(fi_ops), .close = [](fid*) { return 0; }};
    fi_ops_ep ep_ops{
        .size = sizeof(fi_ops_ep),
        .getopt =
            [](fid* ep, int level, int option, void* value, std::size_t* length) {
                return static_cast<option_probe*>(ep->context)
                    ->record(level, option, value, *length);
            },
        .setopt =
            [](fid* ep, int level, int option, const void* value, std::size_t length) {
                return static_cast<option_probe*>(ep->context)
                    ->record(level, option, value, length);
            },
    };
    fid_ep handle{.fid = {.context = this, .ops = &ops}, .ops = &ep_ops};

    auto record(int level, int option, const void* value, std::size_t length) -> int {
        ++calls;
        this->level = level;
        this->option = option;
        this->value = value;
        this->length = length;
        return status;
    }

    auto endpoint() -> loom::endpoint {
        return loom::endpoint{
            loom::detail::make_pmr_unique<loom::detail::endpoint_impl>(loom::get_default_resource(),
                                                                       &handle,
                                                                       loom::endpoint_types::rdm,
                                                                       loom::address_format::inet)};
    }
};

struct option_provider {
    int expected_option{};
    bool value{};
    int calls{};
    fi_ops ops{.size = sizeof(fi_ops), .close = [](fid*) { return 0; }};
    fi_ops_ep ep_ops{
        .size = sizeof(fi_ops_ep),
        .getopt =
            [](fid* ep, int level, int option, void* value, std::size_t* length) {
                auto& self = *static_cast<option_provider*>(ep->context);
                ++self.calls;
                if (level != FI_OPT_ENDPOINT || option != self.expected_option ||
                    *length != sizeof(bool)) {
                    return -FI_EINVAL;
                }
                *static_cast<bool*>(value) = self.value;
                return 0;
            },
        .setopt =
            [](fid* ep, int level, int option, const void* value, std::size_t length) {
                auto& self = *static_cast<option_provider*>(ep->context);
                ++self.calls;
                if (level != FI_OPT_ENDPOINT || option != self.expected_option ||
                    length != sizeof(bool)) {
                    return -FI_EINVAL;
                }
                self.value = *static_cast<const bool*>(value);
                return 0;
            },
    };
    fid_ep handle{.fid = {.context = this, .ops = &ops}, .ops = &ep_ops};

    auto endpoint() -> loom::endpoint {
        return loom::endpoint{
            loom::detail::make_pmr_unique<loom::detail::endpoint_impl>(loom::get_default_resource(),
                                                                       &handle,
                                                                       loom::endpoint_types::rdm,
                                                                       loom::address_format::inet)};
    }
};
}  // namespace

TEST_CASE("typed endpoint options preserve every native mapping", "[endpoint][options]") {
    struct mapping {
        loom::ep_opt::option option;
        int native;
        std::size_t width;
    };
    using enum loom::ep_opt::option;
    constexpr std::array mappings{
        mapping{min_multi_recv, FI_OPT_MIN_MULTI_RECV, sizeof(std::size_t)},
        mapping{cm_data_size, FI_OPT_CM_DATA_SIZE, sizeof(std::size_t)},
        mapping{buffered_min, FI_OPT_BUFFERED_MIN, sizeof(std::size_t)},
        mapping{buffered_limit, FI_OPT_BUFFERED_LIMIT, sizeof(std::size_t)},
        mapping{shared_memory_permitted, FI_OPT_SHARED_MEMORY_PERMITTED, sizeof(bool)},
        mapping{cuda_api_permitted, FI_OPT_CUDA_API_PERMITTED, sizeof(bool)},
        mapping{efa_emulated_read, FI_OPT_EFA_EMULATED_READ, sizeof(bool)},
        mapping{efa_emulated_write, FI_OPT_EFA_EMULATED_WRITE, sizeof(bool)},
        mapping{efa_write_in_order_aligned_128_bytes,
                FI_OPT_EFA_WRITE_IN_ORDER_ALIGNED_128_BYTES,
                sizeof(bool)},
    };
    const auto setting = GENERATE(false, true);
    for (const auto& mapped : mappings) {
        CAPTURE(mapped.option, setting);
        option_probe native;
        auto ep = native.endpoint();
        bool boolean{};
        std::size_t extent{};
        void* value = mapped.width == sizeof(bool) ? static_cast<void*>(&boolean) : &extent;
        auto length = mapped.width;
        const auto result =
            setting ? loom::set_endpoint_option(
                          ep, loom::ep_opt::level_endpoint, mapped.option, value, length)
                    : loom::get_endpoint_option(
                          ep, loom::ep_opt::level_endpoint, mapped.option, value, &length);
        REQUIRE(result);
        CHECK(native.calls == 1);
        CHECK(native.level == FI_OPT_ENDPOINT);
        CHECK(native.option == mapped.native);
        CHECK(native.value == value);
        CHECK(native.length == mapped.width);
    }
}

TEST_CASE("native endpoint options bypass Loom identifier translation", "[endpoint][options]") {
    const auto level = GENERATE(static_cast<int>(FI_OPT_ENDPOINT), 91);
    const auto status = GENERATE(0, -FI_EIO);
    const auto setting = GENERATE(false, true);
    constexpr std::array<int, 9> options{FI_OPT_CM_DATA_SIZE,
                                         FI_OPT_BUFFERED_MIN,
                                         FI_OPT_BUFFERED_LIMIT,
                                         FI_OPT_SEND_BUF_SIZE,
                                         64,
                                         65,
                                         128,
                                         129,
                                         130};
    for (const auto option : options) {
        CAPTURE(level, option, status, setting);
        option_probe native;
        native.status = status;
        auto ep = native.endpoint();
        std::size_t value{}, length{sizeof(value)};
        const auto result =
            setting ? loom::set_native_endpoint_option(ep, level, option, &value, length)
                    : loom::get_native_endpoint_option(ep, level, option, &value, &length);
        REQUIRE(static_cast<bool>(result) == (status == 0));
        if (!result)
            CHECK(result.error() == loom::errc::io_error);
        CHECK(native.calls == 1);
        CHECK(native.level == level);
        CHECK(native.option == option);
        CHECK(native.value == &value);
        CHECK(native.length == sizeof(value));
    }
}

TEST_CASE("typed endpoint options reject unknown identifiers", "[endpoint][options]") {
    const auto level = GENERATE(loom::ep_opt::level_endpoint,
                                static_cast<loom::ep_opt::level>(-1),
                                static_cast<loom::ep_opt::level>(1));
    const auto option = GENERATE(loom::ep_opt::cuda_api_permitted,
                                 static_cast<loom::ep_opt::option>(-1),
                                 static_cast<loom::ep_opt::option>(0),
                                 static_cast<loom::ep_opt::option>(999));
    const auto setting = GENERATE(false, true);
    CAPTURE(level, option, setting);
    option_probe native;
    auto ep = native.endpoint();
    bool value{};
    auto length = sizeof(value);
    const auto result = setting ? loom::set_endpoint_option(ep, level, option, &value, length)
                                : loom::get_endpoint_option(ep, level, option, &value, &length);
    const auto valid =
        level == loom::ep_opt::level_endpoint && option == loom::ep_opt::cuda_api_permitted;
    REQUIRE(static_cast<bool>(result) == valid);
    if (!result)
        CHECK(result.error() == loom::errc::invalid_argument);
    CHECK(native.calls == (valid ? 1 : 0));
}

TEST_CASE("boolean endpoint options use native identifiers and widths", "[endpoint]") {
    option_provider native;
    auto ep = native.endpoint();
    SECTION("shared memory") {
        native.expected_option = FI_OPT_SHARED_MEMORY_PERMITTED;
        REQUIRE(loom::set_shared_memory_permitted(ep, true));
        REQUIRE(loom::get_shared_memory_permitted(ep).value());
        REQUIRE(loom::set_shared_memory_permitted(ep, false));
        REQUIRE_FALSE(loom::get_shared_memory_permitted(ep).value());
    }
    SECTION("CUDA") {
        native.expected_option = FI_OPT_CUDA_API_PERMITTED;
        REQUIRE(loom::set_cuda_api_permitted(ep, true));
        REQUIRE(loom::get_cuda_api_permitted(ep).value());
        REQUIRE(loom::set_cuda_api_permitted(ep, false));
        REQUIRE_FALSE(loom::get_cuda_api_permitted(ep).value());
    }
    SECTION("EFA emulated reads") {
        native.expected_option = FI_OPT_EFA_EMULATED_READ;
        native.value = true;
        REQUIRE(loom::efa::get_emulated_read(ep).value());
        native.value = false;
        REQUIRE_FALSE(loom::efa::get_emulated_read(ep).value());
    }
    SECTION("EFA emulated writes") {
        native.expected_option = FI_OPT_EFA_EMULATED_WRITE;
        native.value = true;
        REQUIRE(loom::efa::get_emulated_write(ep).value());
        native.value = false;
        REQUIRE_FALSE(loom::efa::get_emulated_write(ep).value());
    }
    SECTION("EFA ordered writes") {
        native.expected_option = FI_OPT_EFA_WRITE_IN_ORDER_ALIGNED_128_BYTES;
        REQUIRE(loom::efa::set_write_in_order_aligned_128_bytes(ep, true));
        REQUIRE(loom::efa::get_write_in_order_aligned_128_bytes(ep).value());
        REQUIRE(loom::efa::set_write_in_order_aligned_128_bytes(ep, false));
        REQUIRE_FALSE(loom::efa::get_write_in_order_aligned_128_bytes(ep).value());
    }
    REQUIRE(native.calls >= 2);
}

TEST_CASE("endpoint options reject unavailable native operations", "[endpoint]") {
    enum class support { available, missing_table, missing_member, truncated, empty };
    const auto state = GENERATE(support::available,
                                support::missing_table,
                                support::missing_member,
                                support::truncated,
                                support::empty);
    const auto setting = GENERATE(false, true);
    CAPTURE(state, setting);
    option_provider native;
    native.expected_option = FI_OPT_CUDA_API_PERMITTED;
    auto ep = native.endpoint();
    switch (state) {
        case support::available:
            break;
        case support::missing_table:
            native.handle.ops = nullptr;
            break;
        case support::missing_member:
            if (setting)
                native.ep_ops.setopt = nullptr;
            else
                native.ep_ops.getopt = nullptr;
            break;
        case support::truncated:
            native.ep_ops.size =
                setting ? offsetof(fi_ops_ep, setopt) : offsetof(fi_ops_ep, getopt);
            break;
        case support::empty:
            ep = {};
            break;
    }
    const auto result = setting ? loom::set_cuda_api_permitted(ep, true)
                                : loom::get_cuda_api_permitted(ep).transform([](bool) {});
    REQUIRE(static_cast<bool>(result) == (state == support::available));
    if (!result)
        REQUIRE(result.error() == (state == support::empty ? loom::errc::invalid_argument
                                                           : loom::errc::not_supported));
    REQUIRE(native.calls == (state == support::available ? 1 : 0));
}
