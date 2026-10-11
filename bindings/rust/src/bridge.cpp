// SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only
#include <rdma/fi_cm.h>

#include <array>
#include <limits>
#include <utility>
#include <vector>

#include "loom-fabric/src/ffi.rs.h"
#include "loom/core/rma.hpp"
#include "loom/detail/conversions.hpp"

namespace loom::cxx_bridge {

namespace {

template <typename Bridge, typename Native, std::size_t N>
auto native_value(Bridge value, const std::array<std::pair<Bridge, Native>, N>& entries) -> Native {
    for (const auto& [bridge, native] : entries)
        if (bridge == value)
            return native;
    throw std::invalid_argument("unsupported bridge enum");
}

template <typename Bridge, typename Native, std::size_t N>
auto bridge_value(Native value, const std::array<std::pair<Bridge, Native>, N>& entries) -> Bridge {
    for (const auto& [bridge, native] : entries)
        if (native == value)
            return bridge;
    throw std::invalid_argument("unsupported native discovery enum");
}

constexpr std::array threading_values{
    std::pair{ThreadingMode::Unspecified, FI_THREAD_UNSPEC},
    std::pair{ThreadingMode::Safe, FI_THREAD_SAFE},
    std::pair{ThreadingMode::Fid, FI_THREAD_FID},
    std::pair{ThreadingMode::Domain, FI_THREAD_DOMAIN},
    std::pair{ThreadingMode::Completion, FI_THREAD_COMPLETION},
    std::pair{ThreadingMode::Endpoint, FI_THREAD_ENDPOINT},
};

constexpr std::array progress_values{
    std::pair{ProgressMode::Unspecified, FI_PROGRESS_UNSPEC},
    std::pair{ProgressMode::Automatic, FI_PROGRESS_AUTO},
    std::pair{ProgressMode::Manual, FI_PROGRESS_MANUAL},
    std::pair{ProgressMode::ControlUnified, FI_PROGRESS_CONTROL_UNIFIED},
};

constexpr std::array address_values{
    std::pair{AddressFormat::Unspecified, loom::address_format::unspecified},
    std::pair{AddressFormat::Inet, loom::address_format::inet},
    std::pair{AddressFormat::Inet6, loom::address_format::inet6},
    std::pair{AddressFormat::Ib, loom::address_format::ib},
    std::pair{AddressFormat::Psmx, loom::address_format::psmx},
    std::pair{AddressFormat::Psmx2, loom::address_format::psmx2},
    std::pair{AddressFormat::Gni, loom::address_format::gni},
    std::pair{AddressFormat::Bgq, loom::address_format::bgq},
    std::pair{AddressFormat::String, loom::address_format::str},
    std::pair{AddressFormat::Sockaddr, loom::address_format::sockaddr},
    std::pair{AddressFormat::SockaddrIp, loom::address_format::sockaddr_ip},
    std::pair{AddressFormat::Mlx, loom::address_format::mlx},
    std::pair{AddressFormat::IbUd, loom::address_format::ib_ud},
    std::pair{AddressFormat::Efa, loom::address_format::efa},
    std::pair{AddressFormat::Psmx3, loom::address_format::psmx3},
    std::pair{AddressFormat::Opx, loom::address_format::opx},
    std::pair{AddressFormat::Cxi, loom::address_format::cxi},
    std::pair{AddressFormat::Ucx, loom::address_format::ucx},
};

static_assert(std::to_underlying(ModeBit::Context) == loom::mode_bits::context.get());
static_assert(std::to_underlying(ModeBit::MsgPrefix) == loom::mode_bits::msg_prefix.get());
static_assert(std::to_underlying(ModeBit::RxCqData) == loom::mode_bits::rx_cq_data.get());
static_assert(std::to_underlying(ModeBit::LocalMr) == loom::mode_bits::local_mr.get());
static_assert(std::to_underlying(ModeBit::Context2) == loom::mode_bits::context2.get());
static_assert(std::to_underlying(MrModeBit::Scalable) == loom::mr_mode_flags::scalable.get());
static_assert(std::to_underlying(MrModeBit::Local) == loom::mr_mode_flags::local.get());
static_assert(std::to_underlying(MrModeBit::VirtAddr) == loom::mr_mode_flags::virt_addr.get());
static_assert(std::to_underlying(MrModeBit::Allocated) == loom::mr_mode_flags::allocated.get());
static_assert(std::to_underlying(MrModeBit::ProviderKey) == loom::mr_mode_flags::prov_key.get());
static_assert(std::to_underlying(MrModeBit::Raw) == loom::mr_mode_flags::raw.get());
static_assert(std::to_underlying(MrModeBit::Hmem) == loom::mr_mode_flags::hmem.get());
static_assert(std::to_underlying(MrModeBit::Endpoint) == loom::mr_mode_flags::endpoint.get());
static_assert(std::to_underlying(MrModeBit::Collective) == loom::mr_mode_flags::collective.get());
static_assert(std::to_underlying(MrModeBit::Basic) == loom::mr_mode_flags::basic.get());
static_assert(std::to_underlying(MrModeBit::MmuNotify) == loom::mr_mode_flags::mmu_notify.get());
static_assert(std::to_underlying(MrModeBit::RmaEvent) == loom::mr_mode_flags::rma_event.get());
static_assert(std::to_underlying(MrModeBit::XpuDesc) == loom::mr_mode_flags::xpu_desc.get());
constexpr std::array endpoint_values{
    std::pair{EndpointKind::Unspec, FI_EP_UNSPEC},
    std::pair{EndpointKind::Msg, FI_EP_MSG},
    std::pair{EndpointKind::Rdm, FI_EP_RDM},
    std::pair{EndpointKind::Dgram, FI_EP_DGRAM},
};

constexpr std::array hmem_interface_values{
    std::pair{HmemInterface::System, loom::hmem_iface::system},
    std::pair{HmemInterface::Cuda, loom::hmem_iface::cuda},
    std::pair{HmemInterface::Rocr, loom::hmem_iface::rocr},
    std::pair{HmemInterface::LevelZero, loom::hmem_iface::level_zero},
    std::pair{HmemInterface::Neuron, loom::hmem_iface::neuron},
    std::pair{HmemInterface::SynapseAi, loom::hmem_iface::synapseai},
};

constexpr std::array hmem_allocation_values{
    std::pair{HmemAllocation::Unspecified, loom::hmem_allocation_kind::unspecified},
    std::pair{HmemAllocation::Device, loom::hmem_allocation_kind::device},
    std::pair{HmemAllocation::Host, loom::hmem_allocation_kind::host},
};

}  // namespace

auto query_fabric(const FabricHints& hints) -> std::unique_ptr<FabricInfoHandle> {
    loom::fabric_hints loom_hints{};

    const auto provider_name = std::string(hints.provider_name);
    const auto fabric_name = std::string(hints.fabric_name);
    const auto domain_name = std::string(hints.domain_name);
    if (!provider_name.empty()) {
        loom_hints.provider_name = provider_name;
    }
    if (!fabric_name.empty())
        loom_hints.fabric_name = fabric_name;
    if (!domain_name.empty())
        loom_hints.domain_name = domain_name;

    loom_hints.capabilities = loom::caps{hints.capabilities};
    loom_hints.mode_bits = loom::mode{hints.supported_modes};
    loom_hints.addr_format = native_value(hints.address_format, address_values);
    const auto threading = native_value(hints.threading, threading_values);
    loom_hints.threading = loom::detail::from_fi_threading(threading);
    if (loom::detail::to_fi_threading(*loom_hints.threading) != threading)
        throw std::invalid_argument("unsupported threading hint");
    loom_hints.control_progress =
        loom::detail::from_fi_progress(native_value(hints.control_progress, progress_values));
    loom_hints.data_progress =
        loom::detail::from_fi_progress(native_value(hints.data_progress, progress_values));
    if (hints.has_mr_mode)
        loom_hints.memory_region_mode = loom::mr_mode{hints.mr_mode};

    // Unknown public bits must not silently weaken a requested contract.
    if (loom::detail::from_fi_caps(loom::detail::to_fi_caps(loom_hints.capabilities)) !=
            loom_hints.capabilities ||
        loom::detail::from_fi_mode(loom::detail::to_fi_mode(loom_hints.mode_bits)) !=
            loom_hints.mode_bits ||
        (loom_hints.memory_region_mode &&
         loom::detail::from_fi_mr_mode(loom::detail::to_fi_mr_mode(
             *loom_hints.memory_region_mode)) != *loom_hints.memory_region_mode))
        throw std::invalid_argument("unsupported discovery flag");

    switch (hints.ep_type) {
        case EndpointKind::Unspec:
            loom_hints.ep_type = std::nullopt;
            break;
        case EndpointKind::Msg:
            loom_hints.ep_type = loom::endpoint_types::msg;
            break;
        case EndpointKind::Rdm:
            loom_hints.ep_type = loom::endpoint_types::rdm;
            break;
        case EndpointKind::Dgram:
            loom_hints.ep_type = loom::endpoint_types::dgram;
            break;
        default:
            throw std::invalid_argument("unknown endpoint type");
    }

    auto result = loom::query_fabric(loom_hints);
    if (!result) {
        throw std::runtime_error(result.error().message());
    }

    return std::make_unique<FabricInfoHandle>(std::move(*result));
}

auto fabric_info_next(const FabricInfoHandle& info) -> std::unique_ptr<FabricInfoHandle> {
    return info.next();
}

auto fabric_info_attributes(const FabricInfoHandle& info) -> FabricAttributesData {
    const auto* native = static_cast<const ::fi_info*>(info.get().internal_ptr());
    if (!native || !native->fabric_attr || !native->domain_attr || !native->ep_attr)
        throw std::invalid_argument("incomplete discovery attributes");
    const auto required = native->mode | (native->tx_attr ? native->tx_attr->mode : 0) |
                          (native->rx_attr ? native->rx_attr->mode : 0);
    const auto modes = loom::detail::from_fi_mode(required);
    const auto mr_mode = loom::detail::from_fi_mr_mode(native->domain_attr->mr_mode);
    const auto format = info.get().get_address_format();
    // Losing an unknown obligation would let the consumer accept an invalid
    // configuration. Reject this entry while leaving other entries usable.
    if (loom::detail::to_fi_mode(modes) != required ||
        loom::detail::to_fi_mr_mode(mr_mode) != native->domain_attr->mr_mode ||
        loom::detail::to_fi_addr_format(format) != native->addr_format)
        throw std::invalid_argument("unsupported native discovery requirements");
    const auto own_name = [](const char* value) {
        return rust::String{value ? value : ""};
    };
    return {
        .provider_name = own_name(native->fabric_attr->prov_name),
        .fabric_name = own_name(native->fabric_attr->name),
        .domain_name = own_name(native->domain_attr->name),
        .provider_version = native->fabric_attr->prov_version,
        .endpoint_type = bridge_value(native->ep_attr->type, endpoint_values),
        .protocol = native->ep_attr->protocol,
        .protocol_version = native->ep_attr->protocol_version,
        .address_format = bridge_value(format, address_values),
        .threading = bridge_value(native->domain_attr->threading, threading_values),
        .control_progress = bridge_value(native->domain_attr->control_progress, progress_values),
        .data_progress = bridge_value(native->domain_attr->data_progress, progress_values),
        .capabilities = info.get().get_caps().get(),
        .required_modes = modes.get(),
        .mr_mode = mr_mode.get(),
        .max_message_size = native->ep_attr->max_msg_size,
    };
}

auto create_fabric(const FabricInfoHandle& info) -> std::unique_ptr<FabricHandle> {
    auto result = loom::fabric::create(info.get());
    return std::make_unique<FabricHandle>(unwrap_or_throw(std::move(result)));
}

auto create_domain(const FabricHandle& fab, const FabricInfoHandle& info)
    -> std::unique_ptr<DomainHandle> {
    auto result = loom::domain::create(fab.get(), info.get());
    return std::make_unique<DomainHandle>(unwrap_or_throw(std::move(result)));
}

auto create_endpoint(const DomainHandle& dom, const FabricInfoHandle& info)
    -> std::unique_ptr<EndpointHandle> {
    auto result = loom::endpoint::create(dom.get(), info.get());
    return std::make_unique<EndpointHandle>(unwrap_or_throw(std::move(result)));
}

auto create_completion_queue(const DomainHandle& dom, const CqAttributes& attr)
    -> std::unique_ptr<CompletionQueueHandle> {
    loom::completion_queue_attr cq_attr{};
    cq_attr.size = loom::queue_size{attr.size};
    cq_attr.wait_obj = attr.wait_obj;

    auto result = loom::completion_queue::create(dom.get(), cq_attr);
    return std::make_unique<CompletionQueueHandle>(unwrap_or_throw(std::move(result)));
}

auto create_address_vector(const DomainHandle& dom) -> std::unique_ptr<AddressVectorHandle> {
    auto result = loom::address_vector::create(dom.get());
    return std::make_unique<AddressVectorHandle>(unwrap_or_throw(std::move(result)));
}

auto register_memory(const DomainHandle& dom,
                     std::uintptr_t ptr,
                     std::size_t len,
                     std::uint64_t access) -> std::unique_ptr<MemoryRegionHandle> {
    auto span = std::span{reinterpret_cast<std::byte*>(ptr), len};
    auto result = loom::memory_region::register_memory(dom.get(), span, loom::mr_access{access});
    return std::make_unique<MemoryRegionHandle>(unwrap_or_throw(std::move(result)));
}

auto register_memory_with_options(const DomainHandle& dom,
                                  std::uintptr_t ptr,
                                  std::size_t len,
                                  const MemoryRegistrationAttributes& attributes)
    -> std::unique_ptr<MemoryRegionHandle> {
    loom::memory_registration registration{
        .access = loom::mr_access{attributes.access},
        .allocation = native_value(attributes.allocation, hmem_allocation_values),
    };
    registration.device.iface = native_value(attributes.interface, hmem_interface_values);
    registration.device.device_id = attributes.device;
    auto result = loom::memory_region::register_memory(
        dom.get(), reinterpret_cast<void*>(ptr), len, registration);
    return std::make_unique<MemoryRegionHandle>(unwrap_or_throw(std::move(result)));
}

void endpoint_enable(EndpointHandle& ep) {
    unwrap_void_or_throw(ep.get().enable());
}

auto endpoint_close(EndpointHandle& ep) noexcept -> std::int32_t {
    const auto result = ep.get().close();
    return result ? 0 : result.error().value();
}

void endpoint_bind_cq(EndpointHandle& ep, const CompletionQueueHandle& cq, std::uint64_t flags) {
    unwrap_void_or_throw(ep.get().bind_cq(cq.get(), loom::cq_bind_flags{flags}));
}

void endpoint_bind_av(EndpointHandle& ep, const AddressVectorHandle& av) {
    unwrap_void_or_throw(ep.get().bind_av(av.get()));
}

auto endpoint_send(EndpointHandle& ep, rust::Slice<const std::uint8_t> data, std::uintptr_t ctx)
    -> std::int32_t {
    auto span = std::span{reinterpret_cast<const std::byte*>(data.data()), data.size()};
    auto context = loom::context_ptr<void>{reinterpret_cast<void*>(ctx)};
    return submission_status(ep.get().send(span, context));
}

auto endpoint_recv(EndpointHandle& ep, rust::Slice<std::uint8_t> buf, std::uintptr_t ctx)
    -> std::int32_t {
    auto span = std::span{reinterpret_cast<std::byte*>(buf.data()), buf.size()};
    auto context = loom::context_ptr<void>{reinterpret_cast<void*>(ctx)};
    return submission_status(ep.get().recv(span, context));
}

auto endpoint_tagged_send(EndpointHandle& ep,
                          rust::Slice<const std::uint8_t> data,
                          std::uint64_t tag,
                          std::uintptr_t ctx) -> std::int32_t {
    auto span = std::span{reinterpret_cast<const std::byte*>(data.data()), data.size()};
    auto context = loom::context_ptr<void>{reinterpret_cast<void*>(ctx)};
    return submission_status(ep.get().tagged_send(span, tag, context));
}

auto endpoint_tagged_recv(EndpointHandle& ep,
                          rust::Slice<std::uint8_t> buf,
                          std::uint64_t tag,
                          std::uint64_t ignore,
                          std::uintptr_t ctx) -> std::int32_t {
    auto span = std::span{reinterpret_cast<std::byte*>(buf.data()), buf.size()};
    auto context = loom::context_ptr<void>{reinterpret_cast<void*>(ctx)};
    return submission_status(ep.get().tagged_recv(span, tag, ignore, context));
}

auto endpoint_inject(EndpointHandle& ep, rust::Slice<const std::uint8_t> data) -> std::int32_t {
    auto span = std::span{reinterpret_cast<const std::byte*>(data.data()), data.size()};
    return submission_status(ep.get().inject(span));
}

namespace {
auto rma_flags(const RmaOptions& options) -> std::optional<loom::op_flags> {
    auto flags = loom::op_flag::completion;
    switch (options.completion) {
        case RmaCompletion::Local:
            break;
        case RmaCompletion::Transmit:
            flags |= loom::op_flag::transmit_complete;
            break;
        case RmaCompletion::Delivery:
            flags |= loom::op_flag::delivery_complete;
            break;
        default:
            return std::nullopt;
    }
    if (options.fence)
        flags |= loom::op_flag::fence;
    return flags;
}

auto registered_range(const MemoryRegionHandle& mr,
                      const void* buffer,
                      std::size_t length,
                      const RemoteMemory& remote) noexcept -> bool {
    const auto base = reinterpret_cast<std::uintptr_t>(mr.get().address());
    const auto address = reinterpret_cast<std::uintptr_t>(buffer);
    return address >= base && address - base <= mr.get().length() &&
           length <= mr.get().length() - (address - base) && length <= remote.length &&
           length <= std::numeric_limits<std::uint64_t>::max() - remote.addr;
}
}  // namespace

auto endpoint_read_raw(EndpointHandle& ep,
                       std::uint8_t* buffer,
                       std::size_t length,
                       const MemoryRegionHandle& mr,
                       const RmaTarget& target,
                       const RmaOptions& options,
                       std::uintptr_t context) -> std::int32_t {
    const auto flags = rma_flags(options);
    if (!flags || !registered_range(mr, buffer, length, target.memory))
        return std::to_underlying(loom::errc::invalid_argument);
    return submission_status(loom::rma::read_to(
        ep.get(),
        std::as_writable_bytes(std::span{buffer, length}),
        mr.get().descriptor(),
        loom::fabric_addr{target.peer},
        loom::remote_memory{target.memory.addr, target.memory.key, target.memory.length},
        *flags,
        reinterpret_cast<void*>(context)));
}

auto endpoint_write_raw(EndpointHandle& ep,
                        const std::uint8_t* buffer,
                        std::size_t length,
                        const MemoryRegionHandle& mr,
                        const RmaTarget& target,
                        const RmaOptions& options,
                        std::uintptr_t context) -> std::int32_t {
    const auto flags = rma_flags(options);
    if (!flags || !registered_range(mr, buffer, length, target.memory))
        return std::to_underlying(loom::errc::invalid_argument);
    return submission_status(loom::rma::write_to(
        ep.get(),
        std::as_bytes(std::span{buffer, length}),
        mr.get().descriptor(),
        loom::fabric_addr{target.peer},
        loom::remote_memory{target.memory.addr, target.memory.key, target.memory.length},
        *flags,
        reinterpret_cast<void*>(context)));
}

auto endpoint_get_name(const EndpointHandle& ep) -> rust::Vec<std::uint8_t> {
    if (!ep.get()) {
        throw std::runtime_error("invalid endpoint");
    }
    auto* handle = static_cast<fid_ep*>(ep.get().internal_ptr());
    std::size_t length{};
    auto ret = fi_getname(&handle->fid, nullptr, &length);
    if (ret != -FI_ETOOSMALL && ret != 0) {
        throw std::runtime_error(loom::make_error_code_from_fi_errno(-ret).message());
    }
    std::vector<std::uint8_t> address(length);
    ret = fi_getname(&handle->fid, address.data(), &length);
    if (ret != 0) {
        throw std::runtime_error(loom::make_error_code_from_fi_errno(-ret).message());
    }
    rust::Vec<std::uint8_t> vec;
    vec.reserve(length);
    for (const auto byte : std::span{address}.first(length)) {
        vec.push_back(byte);
    }
    return vec;
}

namespace {
auto encode_completion(const loom::completion_event& event) -> CqReadResult {
    return {
        .kind = CqReadKind::Event,
        .event = {
            .context = std::bit_cast<std::uintptr_t>(event.context.raw()),
            .bytes_transferred = event.bytes_transferred,
            .flags = event.flags,
            .tag = event.tag,
            .data = event.data,
            .has_error = event.has_error(),
            .error_code = event.error ? event.error.value() : 0,
        },
    };
}
}  // namespace

auto cq_poll(const CompletionQueueHandle& cq) -> CqReadResult {
    const auto event = cq.get().poll();
    return event ? encode_completion(*event) : CqReadResult{.kind = CqReadKind::Empty};
}

auto cq_wait(const CompletionQueueHandle& cq, std::int32_t timeout_ms) -> CqReadResult {
    std::optional<std::chrono::milliseconds> timeout;
    if (timeout_ms >= 0) {
        timeout = std::chrono::milliseconds{timeout_ms};
    }
    const auto result = cq.get().wait(timeout);
    if (!result) {
        return {.kind = CqReadKind::Failure, .error_code = result.error().value()};
    }
    return encode_completion(*result);
}

auto cq_try_wait(const CompletionQueueHandle& cq, const FabricHandle& fabric) -> bool {
    auto ready = cq.get().try_wait(fabric.get());
    if (!ready) {
        throw std::system_error(ready.error());
    }
    return *ready;
}

auto cq_get_wait_fd(const CompletionQueueHandle& cq) -> std::int32_t {
    auto result = cq.get().get_wait_fd();
    if (!result) {
        throw std::runtime_error(result.error().message());
    }
    return *result;
}

auto cq_supports_wait(const CompletionQueueHandle& cq) -> bool {
    return cq.get().supports_blocking_wait();
}

auto av_insert(const AddressVectorHandle& av, rust::Slice<const std::uint8_t> addr)
    -> std::uint64_t {
    if (!av.get() || addr.empty()) {
        throw std::runtime_error("invalid address vector or empty address");
    }
    auto* handle = static_cast<fid_av*>(av.get().internal_ptr());
    fi_addr_t address = FI_ADDR_NOTAVAIL;
    const auto ret = fi_av_insert(handle, addr.data(), 1, &address, 0, nullptr);
    if (ret != 1) {
        throw std::runtime_error(
            loom::make_error_code_from_fi_errno(ret < 0 ? -ret : FI_EINVAL).message());
    }
    return address;
}

auto mr_key(const MemoryRegionHandle& mr) -> std::uint64_t {
    return mr.get().key();
}

auto mr_descriptor(const MemoryRegionHandle& mr) -> std::uintptr_t {
    return std::bit_cast<std::uintptr_t>(mr.get().descriptor().raw());
}

auto mr_remote_memory(const MemoryRegionHandle& mr) -> RemoteMemory {
    return {.addr = mr.get().remote_address(), .key = mr.get().key(), .length = mr.get().length()};
}

auto cap_msg() -> std::uint64_t {
    return loom::capability::msg.get();
}

auto cap_rma() -> std::uint64_t {
    return loom::capability::rma.get();
}

auto cap_tagged() -> std::uint64_t {
    return loom::capability::tagged.get();
}

auto cap_atomic() -> std::uint64_t {
    return loom::capability::atomic.get();
}

auto cap_read() -> std::uint64_t {
    return loom::capability::read.get();
}

auto cap_write() -> std::uint64_t {
    return loom::capability::write.get();
}

auto cap_recv() -> std::uint64_t {
    return loom::capability::recv.get();
}

auto cap_send() -> std::uint64_t {
    return loom::capability::send.get();
}

auto cap_remote_read() -> std::uint64_t {
    return loom::capability::remote_read.get();
}

auto cap_remote_write() -> std::uint64_t {
    return loom::capability::remote_write.get();
}

auto cq_bind_transmit() -> std::uint64_t {
    return loom::cq_bind::transmit.get();
}

auto cq_bind_recv() -> std::uint64_t {
    return loom::cq_bind::recv.get();
}

auto mr_access_read() -> std::uint64_t {
    return loom::mr_access_flags::read.get();
}

auto mr_access_write() -> std::uint64_t {
    return loom::mr_access_flags::write.get();
}

auto mr_access_remote_read() -> std::uint64_t {
    return loom::mr_access_flags::remote_read.get();
}

auto mr_access_remote_write() -> std::uint64_t {
    return loom::mr_access_flags::remote_write.get();
}

auto mr_access_send() -> std::uint64_t {
    return loom::mr_access_flags::send.get();
}

auto mr_access_recv() -> std::uint64_t {
    return loom::mr_access_flags::recv.get();
}

}  // namespace loom::cxx_bridge
