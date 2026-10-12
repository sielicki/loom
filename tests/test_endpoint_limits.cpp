// SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only
#include <loom/core/rma.hpp>
#include <loom/core/trigger.hpp>
#include <loom/detail/impl/domain_impl.hpp>
#include <loom/detail/impl/fabric_impl.hpp>

#include <cstdlib>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

namespace {
struct limit_provider {
    std::uint64_t capabilities{};
    std::uint64_t tx_flags{};
    std::uint64_t rx_flags{};
    loom::endpoint_iov_limits limits{};
    bool tx_attributes = true, rx_attributes = true;
    int query_result = -FI_ENOPROTOOPT;
    std::size_t rma_limit = 8192;
    std::size_t inject_limit = 24;
    std::size_t returned_length = sizeof(std::size_t);
    fi_ops ops{.size = sizeof(fi_ops), .close = [](fid*) { return 0; }};
    fi_ops_ep endpoint_ops{
        .size = sizeof(fi_ops_ep),
        .getopt =
            [](fid* endpoint, int level, int option, void* output, std::size_t* length) {
                auto& self = *static_cast<limit_provider*>(endpoint->context);
                REQUIRE(level == FI_OPT_ENDPOINT);
                REQUIRE(*length == sizeof(std::size_t));
                REQUIRE((option == FI_OPT_MAX_RMA_SIZE || option == FI_OPT_INJECT_RMA_SIZE ||
                         option == FI_OPT_INJECT_MSG_SIZE));
                *static_cast<std::size_t*>(output) =
                    option == FI_OPT_MAX_RMA_SIZE ? self.rma_limit : self.inject_limit;
                *length = self.returned_length;
                return self.query_result;
            },
    };
    fid_ep handle{.fid = {.context = this, .ops = &ops}, .ops = &endpoint_ops};
    fi_ops_domain domain_ops{
        .size = sizeof(fi_ops_domain),
        .endpoint =
            [](fid_domain* domain, fi_info*, fid_ep** output, void*) {
                *output = &static_cast<limit_provider*>(domain->fid.context)->handle;
                return 0;
            },
    };
    fid_domain domain_handle{.fid = {.context = this, .ops = &ops}, .ops = &domain_ops};
    loom::domain domain{loom::detail::make_pmr_unique<loom::detail::domain_impl>(
        loom::get_default_resource(), &domain_handle)};

    auto endpoint(loom::memory_resource* resource = nullptr) -> loom::endpoint {
        auto* raw = fi_allocinfo();
        REQUIRE(raw);
        raw->caps = capabilities;
        raw->ep_attr->type = FI_EP_RDM;
        raw->ep_attr->max_msg_size = 65536;
        raw->tx_attr->inject_size = 48;
        raw->tx_attr->op_flags = tx_flags;
        raw->rx_attr->op_flags = rx_flags;
        raw->tx_attr->iov_limit = limits.transmit;
        raw->tx_attr->rma_iov_limit = limits.rma;
        raw->rx_attr->iov_limit = limits.receive;
        if (!tx_attributes) {
            std::free(raw->tx_attr);
            raw->tx_attr = nullptr;
        }
        if (!rx_attributes) {
            std::free(raw->rx_attr);
            raw->rx_attr = nullptr;
        }
        loom::fabric_info info{loom::detail::make_pmr_unique<loom::detail::fabric_info_impl>(
            loom::get_default_resource(), raw)};
        auto result = loom::endpoint::create(domain, info, resource);
        REQUIRE(result);
        return std::move(*result);
    }
};
}  // namespace

TEST_CASE("trigger support reflects negotiated endpoint capabilities", "[trigger][endpoint]") {
    static_assert(loom::detail::to_fi_caps(loom::capability::trigger) == FI_TRIGGER);
    static_assert(loom::detail::from_fi_caps(FI_TRIGGER) == loom::capability::trigger);
    const bool enabled = GENERATE(false, true);
    limit_provider native;
    native.capabilities = FI_MSG | (enabled ? FI_TRIGGER : 0);
    auto original = native.endpoint();
    REQUIRE(loom::trigger::supports_trigger(original) == enabled);
    auto moved = std::move(original);
    REQUIRE_FALSE(loom::trigger::supports_trigger(original));
    REQUIRE(loom::trigger::supports_trigger(moved) == enabled);
}

TEST_CASE("RMA size queries use current endpoint limits", "[rma][endpoint]") {
    limit_provider native;
    native.query_result = 0;
    auto ep = native.endpoint();
    REQUIRE(loom::rma::get_max_rma_size(ep) == 8192);
    REQUIRE(loom::rma::get_inject_size(ep) == 24);
    native.rma_limit = 2048;
    native.inject_limit = 0;
    REQUIRE(loom::rma::get_max_rma_size(ep) == 2048);
    REQUIRE(loom::rma::get_inject_size(ep) == 0);
}

TEST_CASE("unsupported RMA size queries retain creation limits", "[rma][endpoint]") {
    limit_provider native;
    native.query_result = GENERATE(-FI_ENOPROTOOPT, -FI_ENOSYS);
    SECTION("unsupported option") {}
    SECTION("missing query callback") {
        native.endpoint_ops.getopt = nullptr;
    }
    SECTION("missing query interface") {
        native.handle.ops = nullptr;
    }
    auto original = native.endpoint();
    auto ep = std::move(original);
    REQUIRE(loom::rma::get_max_rma_size(ep) == 65536);
    REQUIRE(loom::rma::get_inject_size(ep) == 48);
    REQUIRE(loom::rma::get_max_rma_size(original) == 0);
    REQUIRE(loom::rma::get_inject_size(original) == 0);
}

TEST_CASE("unavailable RMA size limits return zero", "[rma][endpoint]") {
    limit_provider native;
    auto ep = native.endpoint();
    SECTION("empty endpoint") {
        ep = {};
    }
    SECTION("query error") {
        native.query_result = -FI_EIO;
    }
    SECTION("invalid result size") {
        native.query_result = 0;
        native.returned_length = 1;
    }
    REQUIRE(loom::rma::get_max_rma_size(ep) == 0);
    REQUIRE(loom::rma::get_inject_size(ep) == 0);
}

TEST_CASE("message inject bounds preserve native query failures", "[endpoint][inject]") {
    limit_provider native;
    auto original = native.endpoint();
    auto ep = std::move(original);
    REQUIRE(original.message_inject_size().error() == loom::errc::invalid_argument);
    REQUIRE(ep.message_inject_size().value() == 48);
    native.query_result = 0;
    REQUIRE(ep.message_inject_size().value() == 24);
    native.inject_limit = 0;
    REQUIRE(ep.message_inject_size().value() == 0);
    native.query_result = -FI_EIO;
    REQUIRE(ep.message_inject_size().error() == loom::errc::io_error);
    native.query_result = 0;
    native.returned_length = 1;
    REQUIRE(ep.message_inject_size().error() == loom::errc::io_error);
}

TEST_CASE("message inject queries handle unavailable operation tables", "[endpoint][inject]") {
    limit_provider native;
    auto ep = native.endpoint();
    SECTION("unsupported option") {
        native.query_result = GENERATE(-FI_ENOPROTOOPT, -FI_ENOSYS);
    }
    SECTION("absent operation table") {
        native.handle.ops = nullptr;
    }
    SECTION("absent callback") {
        native.endpoint_ops.getopt = nullptr;
    }
    SECTION("truncated operation table") {
        native.endpoint_ops.size = offsetof(fi_ops_ep, getopt);
    }
    REQUIRE(ep.message_inject_size().value() == 48);
}

TEST_CASE("endpoint operation defaults survive discovery and wrapper moves", "[endpoint][flags]") {
    limit_provider native;
    native.tx_flags = FI_FENCE | FI_DELIVERY_COMPLETE;
    native.rx_flags = FI_COMPLETION;
    auto original = native.endpoint();
    auto moved = std::move(original);
    REQUIRE(moved.impl_operation_flags(loom::completion_direction::transmit) == native.tx_flags);
    REQUIRE(moved.impl_operation_flags(loom::completion_direction::receive) == native.rx_flags);
    REQUIRE(original.impl_operation_flags(loom::completion_direction::transmit) == 0);
}

TEST_CASE("endpoint IOV limits preserve negotiated counts", "[endpoint][iov]") {
    const auto limit = GENERATE(0UZ, 1UZ, 16UZ, 17UZ);
    const auto tx_attributes = GENERATE(false, true);
    const auto rx_attributes = GENERATE(false, true);
    const auto explicit_resource = GENERATE(false, true);
    limit_provider native;
    native.limits = {limit, limit + 1, limit + 2};
    native.tx_attributes = tx_attributes;
    native.rx_attributes = rx_attributes;
    auto original = native.endpoint(explicit_resource ? loom::get_default_resource() : nullptr);
    const loom::endpoint_iov_limits expected{
        tx_attributes ? limit : 0, rx_attributes ? limit + 1 : 0, tx_attributes ? limit + 2 : 0};
    CHECK(original.iov_limits() == expected);
    auto moved = std::move(original);
    CHECK(original.iov_limits() == loom::endpoint_iov_limits{});
    CHECK(moved.iov_limits() == expected);
    REQUIRE(moved.close());
    CHECK(moved.iov_limits() == loom::endpoint_iov_limits{});
}
