// SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only
#include <loom/core/fabric.hpp>
#include <loom/core/progress_policy.hpp>
#include <loom/detail/conversions.hpp>
#include <loom/detail/impl/fabric_impl.hpp>

#include <utility>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

namespace {
auto wrap(fi_info& native) -> loom::fabric_info {
    return loom::fabric_info{loom::detail::make_pmr_unique<loom::detail::fabric_info_impl>(
        loom::get_default_resource(), &native, false)};
}
}  // namespace

TEST_CASE("endpoint completion ordering converts the native encoding", "[fabric][attributes]") {
    const auto native_order = GENERATE(std::uint64_t{0},
                                       std::uint64_t{FI_ORDER_STRICT},
                                       std::uint64_t{FI_ORDER_DATA},
                                       std::uint64_t{FI_ORDER_STRICT | FI_ORDER_DATA});
    fi_ep_attr endpoint{};
    fi_tx_attr transmit{};
    transmit.comp_order = native_order;
    fi_info native{};
    native.ep_attr = &endpoint;
    native.tx_attr = &transmit;
    const auto info = wrap(native);
    const auto attributes = info.get_endpoint_attr();
    REQUIRE(attributes);
    auto expected = loom::completion_ordering::none;
    if (native_order & FI_ORDER_DATA)
        expected |= loom::completion_ordering::data;
    if ((native_order & FI_ORDER_STRICT) == FI_ORDER_STRICT)
        expected |= loom::completion_ordering::strict;
    REQUIRE(attributes->comp_ordering == expected);
}

TEST_CASE("endpoint receive completion ordering remains independent of transmit",
          "[fabric][attributes]") {
    const auto missing = GENERATE(0, 1, 2, 3);
    fi_ep_attr endpoint{};
    fi_tx_attr transmit{};
    fi_rx_attr receive{};
    transmit.comp_order = FI_ORDER_STRICT;
    receive.comp_order = FI_ORDER_DATA;
    fi_info native{};
    native.ep_attr = &endpoint;
    native.tx_attr = missing & 1 ? nullptr : &transmit;
    native.rx_attr = missing & 2 ? nullptr : &receive;
    const auto info = wrap(native);
    const auto attributes = info.get_endpoint_attr();
    REQUIRE(attributes);
    REQUIRE(attributes->comp_ordering ==
            (missing & 1 ? loom::completion_ordering::none : loom::completion_ordering::strict));
    REQUIRE(attributes->rx_comp_ordering ==
            (missing & 2 ? loom::completion_ordering::none : loom::completion_ordering::data));
}

TEST_CASE("partial native masks do not invent completion ordering guarantees",
          "[fabric][attributes]") {
    fi_ep_attr endpoint{};
    fi_tx_attr transmit{};
    fi_rx_attr receive{};
    fi_info native{};
    native.ep_attr = &endpoint;
    native.tx_attr = &transmit;
    native.rx_attr = &receive;
    const auto info = wrap(native);
    for (std::uint64_t mask = 0; mask <= FI_ORDER_STRICT; ++mask) {
        INFO("native mask: " << mask);
        transmit.comp_order = mask;
        receive.comp_order = mask | FI_ORDER_DATA;
        const auto attributes = info.get_endpoint_attr();
        REQUIRE(attributes);
        REQUIRE(attributes->comp_ordering.has(loom::completion_ordering::strict) ==
                (mask == FI_ORDER_STRICT));
        REQUIRE_FALSE(attributes->comp_ordering.has(loom::completion_ordering::data));
        REQUIRE(attributes->rx_comp_ordering.has(loom::completion_ordering::strict) ==
                (mask == FI_ORDER_STRICT));
        REQUIRE(attributes->rx_comp_ordering.has(loom::completion_ordering::data));
    }
}

TEST_CASE("unified control progress survives negotiation and requires polling",
          "[fabric][attributes][progress]") {
    fi_domain_attr domain{};
    domain.control_progress = FI_PROGRESS_CONTROL_UNIFIED;
    domain.data_progress = FI_PROGRESS_MANUAL;
    fi_info native{};
    native.domain_attr = &domain;
    const auto info = wrap(native);
    const auto attributes = info.get_domain_attr();
    REQUIRE(attributes);
    REQUIRE(attributes->control_progress == loom::progress_mode::control_unified);
    CHECK(loom::detail::to_fi_progress(attributes->control_progress) ==
          FI_PROGRESS_CONTROL_UNIFIED);
    const loom::runtime_progress_policy policy{attributes->control_progress,
                                               attributes->data_progress};
    CHECK(policy.requires_manual_control_progress());
    CHECK(policy.requires_manual_data_progress());
    CHECK_FALSE(policy.supports_blocking_wait());
    using static_policy =
        loom::static_progress_policy<loom::detail::from_fi_progress(FI_PROGRESS_CONTROL_UNIFIED),
                                     loom::progress_mode::manual>;
    CHECK(static_policy::requires_manual_control_progress());
    CHECK(static_policy::requires_manual_data_progress());
    CHECK_FALSE(static_policy::supports_blocking_wait());
}

TEST_CASE("manual-all progress requests retain the native manual requirement",
          "[fabric][attributes][progress]") {
    REQUIRE(loom::detail::to_fi_progress(loom::progress_mode::manual_all) == FI_PROGRESS_MANUAL);
}

TEST_CASE("discovery preserves distinct native threading requirements",
          "[fabric][attributes][threading]") {
    const auto [native_mode, expected] =
        GENERATE(std::pair{FI_THREAD_UNSPEC, loom::threading_mode::unspecified},
                 std::pair{FI_THREAD_SAFE, loom::threading_mode::safe},
                 std::pair{FI_THREAD_FID, loom::threading_mode::fid},
                 std::pair{FI_THREAD_DOMAIN, loom::threading_mode::domain},
                 std::pair{FI_THREAD_COMPLETION, loom::threading_mode::completion},
                 std::pair{FI_THREAD_ENDPOINT, loom::threading_mode::endpoint});
    CAPTURE(native_mode);
    fi_domain_attr domain{.threading = native_mode};
    fi_fabric_attr fabric{};
    fi_info native{.domain_attr = &domain, .fabric_attr = &fabric};
    const auto info = wrap(native);
    const auto domain_attributes = info.get_domain_attr();
    const auto fabric_attributes = info.get_fabric_attr();
    REQUIRE(domain_attributes);
    REQUIRE(fabric_attributes);
    CHECK(domain_attributes->threading == expected);
    CHECK(fabric_attributes->threading == expected);
    CHECK(loom::detail::to_fi_threading(expected) == native_mode);
}

TEST_CASE("native progress modes round trip without changing manual requirements",
          "[fabric][attributes][progress]") {
    const auto native = GENERATE(
        FI_PROGRESS_UNSPEC, FI_PROGRESS_AUTO, FI_PROGRESS_MANUAL, FI_PROGRESS_CONTROL_UNIFIED);
    const auto converted = loom::detail::from_fi_progress(native);
    REQUIRE(loom::detail::to_fi_progress(converted) == native);
    const loom::runtime_progress_policy policy{converted, converted};
    const bool manual = native == FI_PROGRESS_MANUAL || native == FI_PROGRESS_CONTROL_UNIFIED;
    REQUIRE(policy.requires_manual_control_progress() == manual);
    REQUIRE(policy.requires_manual_data_progress() == manual);
    REQUIRE(policy.supports_blocking_wait() == (native == FI_PROGRESS_AUTO));
}

TEST_CASE("fabric attributes report the selected domain's concurrency requirements",
          "[fabric][attributes][progress]") {
    const auto profile = GENERATE(0, 1, 2);
    fi_domain_attr domain{};
    domain.threading = profile == 0   ? FI_THREAD_DOMAIN
                       : profile == 1 ? FI_THREAD_COMPLETION
                                      : FI_THREAD_SAFE;
    domain.control_progress = profile == 0   ? FI_PROGRESS_CONTROL_UNIFIED
                              : profile == 1 ? FI_PROGRESS_MANUAL
                                             : FI_PROGRESS_AUTO;
    domain.data_progress = profile == 2 ? FI_PROGRESS_AUTO : FI_PROGRESS_MANUAL;
    char name[] = "test_fabric";
    char provider[] = "test_provider";
    fi_fabric_attr fabric{};
    fabric.name = name;
    fabric.prov_name = provider;
    fabric.prov_version = 42;
    fi_info native{};
    native.fabric_attr = &fabric;
    native.domain_attr = &domain;
    const auto info = wrap(native);
    const auto attributes = info.get_fabric_attr();
    const auto selected_domain = info.get_domain_attr();
    REQUIRE(attributes);
    REQUIRE(selected_domain);
    CHECK(attributes->threading == selected_domain->threading);
    CHECK(attributes->control_progress == selected_domain->control_progress);
    CHECK(attributes->data_progress == selected_domain->data_progress);
    REQUIRE(attributes->name == "test_fabric");
    REQUIRE(attributes->provider_name == "test_provider");
    REQUIRE(attributes->provider_version == loom::fabric_version{42U});
}

TEST_CASE("missing domain metadata does not imply fabric concurrency guarantees",
          "[fabric][attributes][progress]") {
    fi_fabric_attr fabric{};
    fi_info native{};
    native.fabric_attr = &fabric;
    const auto info = wrap(native);
    const auto attributes = info.get_fabric_attr();
    REQUIRE(attributes);
    CHECK(attributes->threading == loom::threading_mode::unspecified);
    CHECK(attributes->control_progress == loom::progress_mode::unspecified);
    CHECK(attributes->data_progress == loom::progress_mode::unspecified);
}

TEST_CASE("named receive contexts and message prefixes use independent namespaces",
          "[fabric][attributes][capabilities]") {
    const auto requirements = GENERATE(0, 1, 2, 3);
    fi_info native{};
    native.caps = FI_MSG | (requirements & 1 ? FI_NAMED_RX_CTX : 0);
    native.mode = requirements & 2 ? FI_MSG_PREFIX : 0;
    const auto info = wrap(native);
    const auto expected_caps =
        loom::capability::msg | (requirements & 1 ? loom::capability::named_rx_ctx : loom::caps{});
    const auto expected_mode = requirements & 2 ? loom::mode_bits::msg_prefix : loom::mode{};
    CHECK(info.get_caps() == expected_caps);
    CHECK(info.get_mode() == expected_mode);
    CHECK(loom::detail::to_fi_caps(info.get_caps()) == native.caps);
    CHECK(loom::detail::to_fi_mode(info.get_mode()) == native.mode);
}

TEST_CASE("negotiated capability flags survive conversion independently",
          "[fabric][attributes][capabilities]") {
    const auto [requested, extra] = GENERATE(
        std::pair{loom::capability::multicast, std::uint64_t{FI_MULTICAST}},
        std::pair{loom::capability::av_user_id, std::uint64_t{FI_AV_USER_ID}},
        std::pair{loom::capability::peer, std::uint64_t{FI_PEER}},
        std::pair{loom::capability::xpu, std::uint64_t{FI_XPU}},
        std::pair{loom::capability::tagged_directed_recv, std::uint64_t{FI_TAGGED_DIRECTED_RECV}},
        std::pair{loom::capability::tagged_multi_recv, std::uint64_t{FI_TAGGED_MULTI_RECV}},
        std::pair{loom::capability::exact_directed_recv, std::uint64_t{FI_EXACT_DIRECTED_RECV}},
        std::pair{loom::capability::rma_pmem, std::uint64_t{FI_RMA_PMEM}},
        std::pair{loom::capability::source_err, std::uint64_t{FI_SOURCE_ERR}},
        std::pair{loom::capability::shared_av, std::uint64_t{FI_SHARED_AV}},
        std::pair{loom::capability::rma_event, std::uint64_t{FI_RMA_EVENT}},
        std::pair{loom::capability::source, std::uint64_t{FI_SOURCE}},
        std::pair{loom::capability::directed_recv, std::uint64_t{FI_DIRECTED_RECV}});
    const auto base =
        GENERATE(std::uint64_t{0},
                 std::uint64_t{FI_MSG | FI_TAGGED | FI_SEND | FI_RECV},
                 std::uint64_t{FI_RMA | FI_ATOMIC | FI_REMOTE_READ | FI_REMOTE_WRITE});
    fi_info native{};
    native.caps = base | extra;
    const auto info = wrap(native);
    CHECK(info.get_caps() == (loom::detail::from_fi_caps(base) | requested));
    CHECK(loom::detail::to_fi_caps(info.get_caps()) == native.caps);
}

TEST_CASE("negotiated address formats survive discovery", "[fabric][attributes][address]") {
    const auto format = GENERATE(std::uint32_t{FI_FORMAT_UNSPEC},
                                 std::uint32_t{FI_SOCKADDR},
                                 std::uint32_t{FI_SOCKADDR_IN},
                                 std::uint32_t{FI_SOCKADDR_IN6},
                                 std::uint32_t{FI_SOCKADDR_IB},
                                 std::uint32_t{FI_ADDR_PSMX},
                                 std::uint32_t{FI_ADDR_GNI},
                                 std::uint32_t{FI_ADDR_BGQ},
                                 std::uint32_t{FI_ADDR_MLX},
                                 std::uint32_t{FI_ADDR_STR},
                                 std::uint32_t{FI_ADDR_PSMX2},
                                 std::uint32_t{FI_ADDR_IB_UD},
                                 std::uint32_t{FI_ADDR_EFA},
                                 std::uint32_t{FI_ADDR_PSMX3},
                                 std::uint32_t{FI_ADDR_OPX},
                                 std::uint32_t{FI_ADDR_CXI},
                                 std::uint32_t{FI_ADDR_UCX},
                                 std::uint32_t{FI_SOCKADDR_IP});
    fi_domain_attr domain{};
    fi_info native{};
    native.addr_format = format;
    native.domain_attr = &domain;
    const auto info = wrap(native);
    const auto attributes = info.get_domain_attr();
    REQUIRE(attributes);
    CHECK(loom::detail::to_fi_addr_format(info.get_address_format()) == format);
    CHECK(loom::detail::to_fi_addr_format(attributes->addr_format) == format);
}

TEST_CASE("unsupported address hints cannot become unconstrained discovery",
          "[fabric][attributes][address]") {
    loom::fabric_hints hints;
    hints.addr_format =
        GENERATE(loom::address_format::ethernet, static_cast<loom::address_format>(UINT32_MAX));
    hints.provider_name = "loom-unsupported-address-test";
    const auto result = loom::query_fabric(hints);
    REQUIRE_FALSE(result);
    CHECK(result.error() == loom::errc::not_supported);
}

TEST_CASE("legacy basic registration stays distinct from default registration",
          "[fabric][attributes][mr_mode]") {
    const auto mode = GENERATE(0, FI_MR_BASIC, FI_MR_SCALABLE, FI_MR_LOCAL | FI_MR_PROV_KEY);
    fi_domain_attr domain{};
    domain.mr_mode = mode;
    fi_info native{};
    native.domain_attr = &domain;
    const auto info = wrap(native);
    const auto attributes = info.get_domain_attr();
    REQUIRE(attributes);
    const auto mapped = attributes->memory_region_mode;
    CHECK(mapped.has(loom::mr_mode_flags::basic) == (mode == FI_MR_BASIC));
    CHECK(loom::detail::to_fi_mr_mode(mapped) == mode);
    CHECK(loom::detail::to_fi_mr_mode(loom::mr_mode_flags::basic) == FI_MR_BASIC);
    CHECK(loom::detail::to_fi_mr_mode(loom::mr_mode{}) == 0);
}

TEST_CASE("legacy registration hints cannot combine with other modes",
          "[fabric][attributes][mr_mode]") {
    const auto legacy = GENERATE(loom::mr_mode_flags::basic, loom::mr_mode_flags::scalable);
    const auto extra = GENERATE(loom::mr_mode_flags::local,
                                loom::mr_mode_flags::virt_addr,
                                loom::mr_mode_flags::basic | loom::mr_mode_flags::scalable);
    loom::fabric_hints hints;
    hints.provider_name = "loom-invalid-registration-mode-test";
    hints.memory_region_mode = legacy | extra;
    const auto result = loom::query_fabric(hints);
    REQUIRE_FALSE(result);
    CHECK(result.error() == loom::errc::invalid_argument);
}

TEST_CASE("negotiated memory registration requirements survive discovery",
          "[fabric][attributes][mr_mode]") {
    const auto [flag, required] =
        GENERATE(std::pair{loom::mr_mode_flags::mmu_notify, FI_MR_MMU_NOTIFY},
                 std::pair{loom::mr_mode_flags::rma_event, FI_MR_RMA_EVENT},
                 std::pair{loom::mr_mode_flags::xpu_desc, FI_MR_XPU_DESC});
    const auto base = GENERATE(0,
                               FI_MR_LOCAL | FI_MR_VIRT_ADDR | FI_MR_PROV_KEY,
                               FI_MR_MMU_NOTIFY | FI_MR_RMA_EVENT | FI_MR_XPU_DESC);
    fi_domain_attr domain{};
    domain.mr_mode = required | base;
    fi_info native{};
    native.domain_attr = &domain;
    const auto info = wrap(native);
    const auto attributes = info.get_domain_attr();
    REQUIRE(attributes);
    CHECK(attributes->memory_region_mode.has(flag));
    CHECK(loom::detail::to_fi_mr_mode(flag) == required);
    CHECK(loom::detail::to_fi_mr_mode(attributes->memory_region_mode) == domain.mr_mode);
}

TEST_CASE("domain attributes preserve independent counter limits", "[fabric][attributes][limits]") {
    constexpr auto full_range = std::numeric_limits<std::uint64_t>::max();
    const auto maximum =
        GENERATE_COPY(std::uint64_t{0}, std::uint64_t{7}, full_range - 1, full_range);
    const auto errors = GENERATE_COPY(std::uint64_t{0}, std::uint64_t{3}, full_range);
    fi_domain_attr domain{};
    domain.max_cntr_value = maximum;
    domain.max_err_cntr_value = errors;
    fi_info native{};
    native.domain_attr = &domain;
    const auto info = wrap(native);
    const auto attributes = info.get_domain_attr();
    REQUIRE(attributes);
    REQUIRE(attributes->max_counter_value == (maximum ? maximum : full_range));
    REQUIRE(attributes->max_error_counter_value == (errors ? errors : full_range));
}
