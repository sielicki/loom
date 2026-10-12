// SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only
#include <loom/core/endpoint_options.hpp>
#include <loom/detail/impl/endpoint_impl.hpp>

#include <rdma/fi_ext.h>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

namespace {
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
