// SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only
#include <loom/async/completion_queue.hpp>
#include <loom/core/address_vector.hpp>
#include <loom/core/endpoint.hpp>
#include <loom/core/fabric.hpp>

#include <rdma/fabric.h>
#include <rdma/fi_domain.h>

#include <array>
#include <cstring>
#include <memory_resource>
#include <ranges>
#include <string_view>
#include <utility>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

namespace {
struct nic_provider {
    int error{};
    int source_closes{};
    int copy_closes{};
    fi_ops operations{
        .size = sizeof(fi_ops),
        .close =
            [](fid* handle) {
                auto& self = *static_cast<nic_provider*>(handle->context);
                if (handle == &self.source.fid) {
                    ++self.source_closes;
                } else {
                    REQUIRE(handle == &self.copy.fid);
                    ++self.copy_closes;
                }
                return 0;
            },
        .control =
            [](fid* handle, int command, void* output) {
                auto& self = *static_cast<nic_provider*>(handle->context);
                REQUIRE(command == FI_DUP);
                if (self.error) {
                    return self.error;
                }
                *static_cast<fid_nic**>(output) = &self.copy;
                return 0;
            },
    };
    fid_nic source{.fid = {.context = this, .ops = &operations}};
    fid_nic copy{.fid = {.context = this, .ops = &operations}};
    fi_info info{.nic = &source};
};
}  // namespace

TEST_CASE("native discovery copies retain one complete independent entry", "[fabric][import]") {
    loom::fabric_info info;
    fid context{};
    fid_fabric fabric{};
    fid_domain domain{};
    constexpr std::uint64_t tag_format = 0xffff0000ffffffff;
    {
        std::array source{'s', 'r', 'c', '\0'};
        std::array destination{'d', 's', 't', '\0'};
        std::array name{'n', 'a', 'm', 'e', '\0'};
        std::array<std::uint8_t, 4> key{1, 2, 3, 4};
        fi_tx_attr tx{.op_flags = FI_DELIVERY_COMPLETE, .msg_order = FI_ORDER_SAS, .iov_limit = 3};
        fi_rx_attr rx{.op_flags = FI_COMPLETION, .msg_order = FI_ORDER_SAS, .iov_limit = 5};
        fi_ep_attr ep{.type = FI_EP_RDM,
                      .msg_prefix_size = 32,
                      .mem_tag_format = tag_format,
                      .auth_key_size = key.size(),
                      .auth_key = key.data()};
        fi_domain_attr dom{.domain = &domain,
                           .name = name.data(),
                           .threading = FI_THREAD_DOMAIN,
                           .control_progress = FI_PROGRESS_CONTROL_UNIFIED,
                           .data_progress = FI_PROGRESS_CONTROL_UNIFIED,
                           .mr_key_size = 8,
                           .cq_data_size = 4,
                           .auth_key = key.data(),
                           .auth_key_size = key.size()};
        fi_fabric_attr fab{.fabric = &fabric, .name = name.data(), .prov_name = name.data()};
        fi_info following{.caps = FI_RMA};
        fi_info native{.next = &following,
                       .caps = FI_MSG | FI_TAGGED,
                       .mode = FI_CONTEXT2,
                       .addr_format = FI_ADDR_STR,
                       .src_addrlen = source.size(),
                       .dest_addrlen = destination.size(),
                       .src_addr = source.data(),
                       .dest_addr = destination.data(),
                       .handle = &context,
                       .tx_attr = &tx,
                       .rx_attr = &rx,
                       .ep_attr = &ep,
                       .domain_attr = &dom,
                       .fabric_attr = &fab};
        auto copied = loom::fabric_info::copy_native(native);
        REQUIRE(copied);
        info = std::move(*copied);
        source.fill('x');
        destination.fill('x');
        name.fill('x');
        key.fill(0);
    }

    REQUIRE(std::ranges::distance(info) == 1);
    REQUIRE(info.get_caps() == (loom::capability::msg | loom::capability::tagged));
    auto& native = *static_cast<const fi_info*>(info.internal_ptr());
    REQUIRE(native.next == nullptr);
    REQUIRE(native.mode == FI_CONTEXT2);
    REQUIRE(native.addr_format == FI_ADDR_STR);
    REQUIRE(native.src_addrlen == 4);
    REQUIRE(native.dest_addrlen == 4);
    REQUIRE(std::memcmp(native.src_addr, "src", 4) == 0);
    REQUIRE(std::memcmp(native.dest_addr, "dst", 4) == 0);
    REQUIRE(native.handle == &context);
    REQUIRE(native.fabric_attr->fabric == &fabric);
    REQUIRE(native.domain_attr->domain == &domain);
    REQUIRE(std::string_view{native.fabric_attr->name} == "name");
    REQUIRE(std::string_view{native.fabric_attr->prov_name} == "name");
    REQUIRE(std::string_view{native.domain_attr->name} == "name");
    REQUIRE(native.domain_attr->cq_data_size == 4);
    REQUIRE(native.domain_attr->mr_key_size == 8);
    REQUIRE(native.domain_attr->threading == FI_THREAD_DOMAIN);
    REQUIRE(native.domain_attr->data_progress == FI_PROGRESS_CONTROL_UNIFIED);
    REQUIRE(native.tx_attr->op_flags == FI_DELIVERY_COMPLETE);
    REQUIRE(native.rx_attr->op_flags == FI_COMPLETION);
    REQUIRE(native.tx_attr->msg_order == FI_ORDER_SAS);
    REQUIRE(native.rx_attr->msg_order == FI_ORDER_SAS);
    REQUIRE(native.tx_attr->iov_limit == 3);
    REQUIRE(native.rx_attr->iov_limit == 5);
    REQUIRE(native.ep_attr->mem_tag_format == tag_format);
    REQUIRE(native.ep_attr->msg_prefix_size == 32);
    const std::array<std::uint8_t, 4> key{1, 2, 3, 4};
    REQUIRE(native.ep_attr->auth_key_size == key.size());
    REQUIRE(native.domain_attr->auth_key_size == key.size());
    REQUIRE(std::memcmp(native.ep_attr->auth_key, key.data(), key.size()) == 0);
    REQUIRE(std::memcmp(native.domain_attr->auth_key, key.data(), key.size()) == 0);
}

TEST_CASE("native discovery copies own only their duplicated NIC", "[fabric][import]") {
    const auto fail_allocation = GENERATE(false, true);
    nic_provider provider;
    if (fail_allocation) {
        REQUIRE_THROWS_AS(
            loom::fabric_info::copy_native(provider.info, std::pmr::null_memory_resource()),
            std::bad_alloc);
    } else {
        auto copied = loom::fabric_info::copy_native(provider.info);
        REQUIRE(copied);
        auto moved = std::move(*copied);
        REQUIRE_FALSE(*copied);
        REQUIRE(static_cast<const fi_info*>(moved.internal_ptr())->nic == &provider.copy);
        REQUIRE(provider.copy_closes == 0);
        moved = {};
    }
    REQUIRE(provider.source_closes == 0);
    REQUIRE(provider.copy_closes == 1);
    REQUIRE(provider.info.nic == &provider.source);
}

TEST_CASE("native discovery copy failure does not invent a cause", "[fabric][import]") {
    nic_provider provider;
    provider.error = GENERATE(-FI_EIO, -FI_ENOMEM);
    auto copied = loom::fabric_info::copy_native(provider.info);
    REQUIRE_FALSE(copied);
    REQUIRE(copied.error() == loom::errc::other);
    REQUIRE(provider.source_closes == 0);
    REQUIRE(provider.copy_closes == 0);
}

TEST_CASE("imported discovery creates resources after its source is released",
          "[fabric][import][integration]") {
    using namespace loom;
    fabric_hints hints{};
    hints.provider_name = "sockets";
    hints.ep_type = endpoint_types::rdm;
    hints.capabilities = capability::tagged;
    auto discovered = query_fabric(hints);
    if (!discovered) {
        SKIP("Sockets provider unavailable");
    }
    auto info = fabric_info::copy_native(*static_cast<const fi_info*>(discovered->internal_ptr()));
    REQUIRE(info);
    *discovered = {};
    auto fab = fabric::create(*info);
    REQUIRE(fab);
    auto dom = domain::create(*fab, *info);
    REQUIRE(dom);
    auto cq = completion_queue::create(*dom);
    REQUIRE(cq);
    auto av = address_vector::create(*dom);
    REQUIRE(av);
    auto ep = endpoint::create(*dom, *info);
    REQUIRE(ep);
    REQUIRE(ep->bind_cq(*cq, cq_bind::transmit | cq_bind::recv));
    REQUIRE(ep->bind_av(*av));
    REQUIRE(ep->enable());
    REQUIRE(ep->close());
}
