// SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only
#include <loom/async/completion_queue.hpp>
#include <loom/loom.hpp>

#include <array>
#include <thread>
#include <utility>
#include <vector>

#include "mock_domain.hpp"
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

using namespace loom;

TEST_CASE("memory binding rejects empty endpoints before calling the provider", "[memory]") {
    test::mock_domain provider;
    provider.region_ops.bind = [](fid*, fid*, std::uint64_t) {
        FAIL("invalid endpoint reached the memory provider");
        return 0;
    };
    std::array<std::byte, 64> buffer{};
    auto region = memory_region::register_memory(provider.dom, buffer, mr_access_flags::send);
    REQUIRE(region);
    endpoint empty;
    const auto result = region->bind(empty);
    REQUIRE_FALSE(result);
    REQUIRE(result.error() == errc::invalid_argument);
}

TEST_CASE("memory registration rejects empty domains", "[memory]") {
    domain empty;
    std::array<std::byte, 64> buffer{};
    const auto access = mr_access_flags::read;
    auto check = [](result<memory_region> region) {
        REQUIRE_FALSE(region);
        REQUIRE(region.error() == errc::invalid_argument);
    };
    check(memory_region::register_memory(empty, buffer, access));
    check(memory_region::register_hmem(
        empty, buffer.data(), buffer.size(), access, hmem_device::cuda(0)));
    check(memory_region::register_dmabuf(empty, buffer.data(), buffer.size(), access, 7, 0));
}

TEST_CASE("registration translates access permissions", "[memory]") {
    test::mock_domain provider;
    std::array<std::byte, 64> buffer{};
    constexpr std::array permissions{
        std::pair{mr_access_flags::read, FI_READ},
        std::pair{mr_access_flags::write, FI_WRITE},
        std::pair{mr_access_flags::remote_read, FI_REMOTE_READ},
        std::pair{mr_access_flags::remote_write, FI_REMOTE_WRITE},
        std::pair{mr_access_flags::send, FI_SEND},
        std::pair{mr_access_flags::recv, FI_RECV},
        std::pair{mr_access_flags::send | mr_access_flags::recv, FI_SEND | FI_RECV},
    };

    for (const auto& [access, expected] : permissions) {
        {
            auto region = memory_region::register_memory(provider.dom, buffer, access);
            REQUIRE(region);
            REQUIRE(provider.access == expected);
        }
        {
            auto region = memory_region::register_dmabuf(
                provider.dom, buffer.data(), buffer.size(), access, 7, 0);
            REQUIRE(region);
            REQUIRE(provider.access == expected);
            REQUIRE(provider.flags == FI_MR_DMABUF);
        }
        {
            auto region = memory_region::register_hmem(
                provider.dom, buffer.data(), buffer.size(), access, hmem_device::cuda(0));
            REQUIRE(region);
            REQUIRE(provider.access == expected);
        }
    }
}

TEST_CASE("basic memory registration", "[memory]") {
    SECTION("requires fabric provider") {
        fabric_hints hints{};
        hints.ep_type = endpoint_types::rdm;
        hints.capabilities = capability::msg;

        auto info_result = query_fabric(hints);
        if (!info_result) {
            SKIP("No providers found");
        }

        auto& info = *info_result;
        auto fab_result = fabric::create(info);
        REQUIRE(static_cast<bool>(fab_result));

        auto dom_result = domain::create(*fab_result, info);
        REQUIRE(static_cast<bool>(dom_result));

        std::vector<std::byte> buffer(4096);
        auto mr_result = memory_region::register_memory(
            *dom_result, std::span{buffer}, mr_access_flags::read | mr_access_flags::write);

        REQUIRE(static_cast<bool>(mr_result));

        auto& mr = *mr_result;
        REQUIRE(static_cast<bool>(mr));
        REQUIRE(mr.address() == buffer.data());
        REQUIRE(mr.length() == buffer.size());
    }
}

TEST_CASE("device registrations retain nonzero ordinals", "[memory]") {
    test::mock_domain provider;
    std::array<std::byte, 64> buffer{};
    const std::array devices{
        hmem_device::cuda(2),
        hmem_device::level_zero(3),
        hmem_device::neuron(4),
        hmem_device::synapseai(5),
#if FI_VERSION(FI_MAJOR_VERSION, FI_MINOR_VERSION) >= FI_VERSION(2, 5)
        hmem_device::rocr(6),
#endif
    };
    for (const auto& device : devices) {
        auto region = memory_region::register_hmem(
            provider.dom, buffer.data(), buffer.size(), mr_access_flags::send, device);
        REQUIRE(region);
        REQUIRE(provider.iface == static_cast<fi_hmem_iface>(device.iface));
        REQUIRE(provider.device_id == device.device_id);
    }
}

TEST_CASE("device registration combines allocation and DMA-BUF metadata", "[memory][hmem]") {
    test::mock_domain provider(FI_MR_VIRT_ADDR);
    std::array<std::byte, 8192> buffer{};
    const auto allocation = GENERATE(hmem_allocation_kind::unspecified,
                                     hmem_allocation_kind::device,
                                     hmem_allocation_kind::host);
    const auto dma = GENERATE(false, true);
    memory_registration attributes{
        .access = mr_access_flags::send | mr_access_flags::recv,
        .device = hmem_device::cuda(3),
        .allocation = allocation,
    };
    if (dma)
        attributes.dmabuf = dmabuf_registration{17, 4096};
    auto region = memory_region::register_memory(provider.dom, buffer.data(), 128, attributes);
    REQUIRE(region);
    const auto expected = allocation == hmem_allocation_kind::device ? FI_HMEM_DEVICE_ONLY
                          : allocation == hmem_allocation_kind::host ? FI_HMEM_HOST_ALLOC
                                                                     : 0;
    REQUIRE(provider.flags == (expected | (dma ? FI_MR_DMABUF : 0)));
    REQUIRE(provider.iface == FI_HMEM_CUDA);
    REQUIRE(provider.device_id == 3);
    REQUIRE(provider.access == (FI_SEND | FI_RECV));
    REQUIRE(provider.hmem_data == nullptr);
    REQUIRE(provider.address == buffer.data());
    REQUIRE(provider.length == 128);
    REQUIRE(provider.registration_offset == 0);
    REQUIRE(region->remote_address() ==
            reinterpret_cast<std::uintptr_t>(buffer.data()) + (dma ? 4096 : 0));
    if (dma) {
        REQUIRE(provider.dmabuf_fd == 17);
        REQUIRE(provider.dmabuf_offset == 4096);
    }
}

TEST_CASE("invalid device registration metadata never reaches the provider", "[memory][hmem]") {
    test::mock_domain provider;
    std::array<std::byte, 64> buffer{};
    memory_registration attributes{.access = mr_access_flags::recv, .device = hmem_device::cuda(0)};
    SECTION("reserved HMEM data") {
        attributes.device.hmem_data = buffer.data();
    }
    SECTION("unknown interface") {
        attributes.device.iface = static_cast<hmem_iface>(99);
    }
    SECTION("out of range device") {
        attributes.device.device_id = std::int64_t{1} << 32;
    }
    SECTION("unknown allocation") {
        attributes.allocation = static_cast<hmem_allocation_kind>(99);
    }
    SECTION("device flag without a device API") {
        attributes.device = {};
        attributes.allocation = hmem_allocation_kind::device;
    }
    SECTION("host flag without a device API") {
        attributes.device = {};
        attributes.allocation = hmem_allocation_kind::host;
    }
    SECTION("invalid DMA-BUF descriptor") {
        attributes.dmabuf = dmabuf_registration{};
    }
    auto region =
        memory_region::register_memory(provider.dom, buffer.data(), buffer.size(), attributes);
    REQUIRE_FALSE(region);
    REQUIRE(region.error() == errc::invalid_argument);
    REQUIRE(provider.requested_keys.empty());
    REQUIRE(provider.live_regions == 0);
}

TEST_CASE("hmem device creation", "[memory]") {
    auto cuda_dev = hmem_device::cuda(0);
    REQUIRE(cuda_dev.iface == hmem_iface::cuda);
    REQUIRE(cuda_dev.device_id == 0);

    auto rocr_dev = hmem_device::rocr(1);
    REQUIRE(rocr_dev.iface == hmem_iface::rocr);
    REQUIRE(rocr_dev.device_id == 1);

    auto ze_dev = hmem_device::level_zero(2);
    REQUIRE(ze_dev.iface == hmem_iface::level_zero);
    REQUIRE(ze_dev.device_id == 2);

    auto neuron_dev = hmem_device::neuron(3);
    REQUIRE(neuron_dev.iface == hmem_iface::neuron);
    REQUIRE(neuron_dev.device_id == 3);

    auto synapse_dev = hmem_device::synapseai(4);
    REQUIRE(synapse_dev.iface == hmem_iface::synapseai);
    REQUIRE(synapse_dev.device_id == 4);
}

TEST_CASE("remote memory descriptor", "[memory]") {
    std::uint64_t addr = 0x12345678;
    std::uint64_t key = 0xABCDEF;
    std::size_t length = 4096;

    remote_memory remote{addr, key, length};

    REQUIRE(remote.addr == addr);
    REQUIRE(remote.key == key);
    REQUIRE(remote.length == length);
}

TEST_CASE("simultaneous software memory registrations receive distinct keys", "[memory][mr_keys]") {
    const auto provider = GENERATE("tcp", "sockets");
    fabric_hints hints{};
    hints.ep_type = endpoint_types::rdm;
    hints.capabilities = capability::rma;
    hints.provider_name = provider;
    auto info = query_fabric(hints);
    if (!info) {
        SKIP("Requested software provider unavailable");
    }
    auto fab = fabric::create(*info);
    REQUIRE(fab);
    auto dom = domain::create(*fab, *info);
    REQUIRE(dom);
    std::array<std::byte, 64> first_buffer{}, second_buffer{};
    auto first = memory_region::register_memory(*dom, first_buffer, mr_access_flags::remote_write);
    REQUIRE(first);
    auto second =
        memory_region::register_memory(*dom, second_buffer, mr_access_flags::remote_write);
    REQUIRE(second);
    REQUIRE(first->key() != second->key());
}

TEST_CASE("registration keys span all memory interfaces and follow region lifetimes",
          "[memory][mr_keys]") {
    test::mock_domain provider{0, 1};
    std::array<std::byte, 64> buffer{};
    std::vector<memory_region> regions;
    const auto access = mr_access_flags::remote_write;
    for (int i = 0; i < 256; ++i) {
        auto region =
            i % 3 == 0 ? memory_region::register_memory(provider.dom, buffer, access)
            : i % 3 == 1
                ? memory_region::register_hmem(
                      provider.dom, buffer.data(), buffer.size(), access, hmem_device::cuda(0))
                : memory_region::register_dmabuf(
                      provider.dom, buffer.data(), buffer.size(), access, 7);
        REQUIRE(region);
        REQUIRE(region->key() < 256);
        regions.push_back(std::move(*region));
    }
    REQUIRE(provider.keys.size() == 256);
    auto full = memory_region::register_memory(provider.dom, buffer, access);
    REQUIRE_FALSE(full);
    REQUIRE(full.error() == make_error_code_from_fi_errno(FI_ENOKEY));
    REQUIRE(provider.registrations == 256);
    auto retained = std::move(regions[17]);
    REQUIRE_FALSE(memory_region::register_memory(provider.dom, buffer, access));
    const auto released = retained.key();
    retained = {};
    auto replacement = memory_region::register_memory(provider.dom, buffer, access);
    REQUIRE(replacement);
    REQUIRE(replacement->key() == released);
    auto moved_domain = std::move(provider.dom);
    regions.clear();
    replacement = memory_region{};
    REQUIRE(provider.live_regions == 0);
    auto reused = memory_region::register_memory(moved_domain, buffer, access);
    REQUIRE(reused);
}

TEST_CASE("failed registrations return their application keys", "[memory][mr_keys]") {
    const auto interface = GENERATE(0, 1, 2);
    test::mock_domain provider{0, 0};  // A one-key space forces immediate reuse.
    std::array<std::byte, 64> buffer{};
    auto create = [&](memory_resource* resource = nullptr) -> result<memory_region> {
        if (interface == 0)
            return memory_region::register_memory(
                provider.dom, buffer, mr_access_flags::read, resource);
        if (interface == 1)
            return memory_region::register_hmem(provider.dom,
                                                buffer.data(),
                                                buffer.size(),
                                                mr_access_flags::read,
                                                hmem_device::cuda(0),
                                                resource);
        return memory_region::register_dmabuf(
            provider.dom, buffer.data(), buffer.size(), mr_access_flags::read, 7, 0, resource);
    };
    provider.registration_error = -FI_EIO;
    REQUIRE_FALSE(create());
    provider.registration_error = 0;
    REQUIRE_THROWS_AS(create(std::pmr::null_memory_resource()), std::bad_alloc);
    REQUIRE(provider.live_regions == 0);
    auto success = create();
    REQUIRE(success);
    REQUIRE(success->key() == 0);
}

TEST_CASE("provider-selected registration keys bypass the application pool", "[memory][mr_keys]") {
    const auto mode = GENERATE(int{FI_MR_PROV_KEY}, int{FI_MR_BASIC});
    test::mock_domain provider{mode, 0};
    std::array<std::byte, 64> buffer{};
    auto first = memory_region::register_memory(provider.dom, buffer, mr_access_flags::read);
    auto second = memory_region::register_memory(provider.dom, buffer, mr_access_flags::read);
    REQUIRE(first);
    REQUIRE(second);
    REQUIRE(first->key() != second->key());
    REQUIRE(provider.requested_keys == std::vector<std::uint64_t>{0, 0});
}

TEST_CASE("parallel registrations reserve distinct live keys", "[memory][mr_keys]") {
    test::mock_domain provider{0, 1};
    std::array<std::byte, 64> buffer{};
    std::array<result<memory_region>, 64> regions;
    {
        std::vector<std::jthread> workers;
        for (auto& region : regions) {
            workers.emplace_back([&provider, &buffer, &region] {
                region =
                    memory_region::register_memory(provider.dom, buffer, mr_access_flags::read);
            });
        }
    }
    for (const auto& region : regions)
        REQUIRE(region);
    REQUIRE(provider.keys.size() == regions.size());
}

TEST_CASE("failed native closes do not release live registration keys", "[memory][mr_keys]") {
    test::mock_domain provider{0, 0};
    std::array<std::byte, 64> buffer{};
    auto region = memory_region::register_memory(provider.dom, buffer, mr_access_flags::read);
    REQUIRE(region);
    auto* native = static_cast<fid_mr*>(region->internal_ptr());
    provider.close_error = -FI_EBUSY;
    *region = {};
    auto next = memory_region::register_memory(provider.dom, buffer, mr_access_flags::read);
    REQUIRE_FALSE(next);
    REQUIRE(next.error() == make_error_code_from_fi_errno(FI_ENOKEY));
    REQUIRE(provider.live_regions == 1);
    provider.close_error = 0;
    REQUIRE(fi_close(&native->fid) == 0);
}

TEST_CASE("DMA-BUF registration preserves its offset without setting reserved fields",
          "[memory][dmabuf]") {
    test::mock_domain provider;
    std::array<std::byte, 64> buffer{};
    auto region = memory_region::register_dmabuf(
        provider.dom, buffer.data(), buffer.size(), mr_access_flags::remote_write, 7, 4096);
    REQUIRE(region);
    REQUIRE(provider.registration_offset == 0);
    REQUIRE(provider.dmabuf_offset == 4096);
}

TEST_CASE("remote descriptors honor registration addressing modes", "[memory][remote_address]") {
    const auto mode = GENERATE(0, int{FI_MR_VIRT_ADDR}, int{FI_MR_BASIC});
    const auto interface = GENERATE(0, 1, 2);
    test::mock_domain provider{mode};
    std::array<std::byte, 64> buffer{};
    auto region =
        interface == 0
            ? memory_region::register_memory(provider.dom, buffer, mr_access_flags::remote_write)
        : interface == 1
            ? memory_region::register_hmem(provider.dom,
                                           buffer.data(),
                                           buffer.size(),
                                           mr_access_flags::remote_write,
                                           hmem_device::cuda(0))
            : memory_region::register_dmabuf(
                  provider.dom, buffer.data(), buffer.size(), mr_access_flags::remote_write, 7, 16);
    REQUIRE(region);
    auto moved = std::move(*region);
    const auto expected =
        mode == 0 ? 0
                  : static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(buffer.data())) +
                        (interface == 2 ? 16 : 0);
    REQUIRE(moved.remote_address() == expected);
    REQUIRE(region->remote_address() == 0);
    const auto generic = remote_memory::from_mr(moved);
    REQUIRE(generic.addr == expected);
    REQUIRE(generic.key == moved.key());
    REQUIRE(generic.length == buffer.size());
    const auto specialized = provider_remote_memory<provider::tcp_tag>::from_mr(moved);
    REQUIRE(specialized.addr == expected);
    REQUIRE(specialized.key == moved.key());
    REQUIRE(specialized.length == buffer.size());
    REQUIRE(remote_memory::from_mr(*region).addr == 0);
}

TEST_CASE("cached memory exports registration-relative remote addresses",
          "[memory][remote_address]") {
    const auto mode = GENERATE(0, int{FI_MR_VIRT_ADDR});
    test::mock_domain provider{mode};
    mr_cache<provider::tcp_tag> cache{provider.dom};
    std::array<std::byte, 64> buffer{};
    auto handle = cache.lookup(buffer, mr_access_flags::remote_write);
    REQUIRE(handle);
    const auto remote = handle->to_remote_memory();
    const auto expected =
        mode == 0
            ? 0
            : static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(handle->base_address()));
    REQUIRE(remote.addr == expected);
    REQUIRE(remote.key == handle->key());
    REQUIRE(remote.length == handle->length());
}

TEST_CASE("exported remote addresses support sockets loopback writes", "[memory][remote_address]") {
    fabric_hints hints{};
    hints.ep_type = endpoint_types::rdm;
    hints.provider_name = "sockets";
    hints.capabilities = capability::msg | capability::rma;
    auto info = query_fabric(hints);
    if (!info)
        SKIP("Sockets provider unavailable");
    auto* native_info = static_cast<fi_info*>(info->internal_ptr());
    REQUIRE(native_info->domain_attr->mr_mode == 0);
    native_info->tx_attr->op_flags |= FI_DELIVERY_COMPLETE;
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
    auto address = ep->get_local_address();
    REQUIRE(address);
    auto peer = av->insert(*address);
    REQUIRE(peer);
    std::array<std::byte, 64> buffer{};
    buffer[0] = std::byte{42};
    auto mr = memory_region::register_memory(*dom,
                                             buffer,
                                             mr_access_flags::remote_write | mr_access_flags::read |
                                                 mr_access_flags::write);
    REQUIRE(mr);
    auto remote = remote_memory::from_mr(*mr);
    remote.addr += 16;
    remote.length = 1;
    int context{};
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{3};
    result<void> submitted;
    do {
        submitted = rma::write_to(*ep,
                                  std::span<const std::byte>{buffer}.first(1),
                                  *mr,
                                  fabric_addr{peer->value},
                                  remote,
                                  &context);
        if (submitted || submitted.error() != errc::again)
            break;
        (void)cq->poll();
        std::this_thread::yield();
    } while (std::chrono::steady_clock::now() < deadline);
    REQUIRE(submitted);
    std::optional<completion_event> completion;
    do {
        completion = cq->poll();
        if (completion)
            break;
        std::this_thread::yield();
    } while (std::chrono::steady_clock::now() < deadline);
    REQUIRE(completion);
    REQUIRE_FALSE(completion->error);
    REQUIRE(buffer[16] == std::byte{42});
}

TEST_CASE("DMA-BUF remote addresses reject overflowing virtual offsets",
          "[memory][remote_address]") {
    test::mock_domain provider{FI_MR_VIRT_ADDR};
    std::array<std::byte, 64> buffer{};
    auto region = memory_region::register_dmabuf(provider.dom,
                                                 buffer.data(),
                                                 buffer.size(),
                                                 mr_access_flags::remote_write,
                                                 7,
                                                 std::numeric_limits<std::uint64_t>::max());
    REQUIRE_FALSE(region);
    REQUIRE(region.error() == errc::invalid_argument);
    REQUIRE(provider.registrations == 0);
}
