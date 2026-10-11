// SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <memory_resource>
#include <optional>
#include <span>

#include "loom/core/allocator.hpp"
#include "loom/core/crtp/fabric_object_base.hpp"
#include "loom/core/crtp/remote_memory_base.hpp"
#include "loom/core/domain.hpp"
#include "loom/core/result.hpp"
#include "loom/core/types.hpp"

/**
 * @namespace loom
 * @brief Core namespace for the loom libfabric bindings library.
 */
namespace loom {

/**
 * @namespace detail
 * @brief Internal implementation details.
 */
namespace detail {
/**
 * @struct memory_region_impl
 * @brief Internal implementation for memory_region.
 */
struct memory_region_impl;
}  // namespace detail

/**
 * @enum hmem_iface
 * @brief Heterogeneous memory interface types.
 *
 * Identifies the type of accelerator memory for registration.
 */
enum class hmem_iface : std::uint32_t {
    system = 0,      ///< System (host) memory
    cuda = 1,        ///< NVIDIA CUDA device memory
    rocr = 2,        ///< AMD ROCm device memory
    level_zero = 3,  ///< Intel Level Zero device memory
    neuron = 4,      ///< AWS Neuron accelerator memory
    synapseai = 5,   ///< Habana SynapseAI accelerator memory
};

/**
 * @struct hmem_device
 * @brief Describes a heterogeneous memory device.
 *
 * Used to specify the accelerator device when registering device memory
 * for RDMA operations.
 */
struct hmem_device {
    hmem_iface iface{hmem_iface::system};  ///< Memory interface type
    std::int64_t device_id{0};             ///< Device identifier
    void* hmem_data{nullptr};              ///< Reserved; must remain null

    /**
     * @brief Default constructor creating a system memory device.
     */
    constexpr hmem_device() noexcept = default;

    /**
     * @brief Creates an hmem_device for NVIDIA CUDA memory.
     * @param device The CUDA device ordinal.
     * @return An hmem_device configured for CUDA.
     */
    static constexpr auto cuda(int device) noexcept -> hmem_device {
        return hmem_device{hmem_iface::cuda, device, nullptr};
    }

    /**
     * @brief Creates an hmem_device for AMD ROCm memory.
     * @param device The ROCm device ordinal.
     * @return An hmem_device configured for ROCm.
     */
    static constexpr auto rocr(int device) noexcept -> hmem_device {
        return hmem_device{hmem_iface::rocr, device, nullptr};
    }

    /**
     * @brief Creates an hmem_device for Intel Level Zero memory.
     * @param device The Level Zero device ordinal.
     * @return An hmem_device configured for Level Zero.
     */
    static constexpr auto level_zero(int device) noexcept -> hmem_device {
        return hmem_device{hmem_iface::level_zero, device, nullptr};
    }

    /**
     * @brief Creates an hmem_device for AWS Neuron memory.
     * @param device The Neuron device ordinal.
     * @return An hmem_device configured for Neuron.
     */
    static constexpr auto neuron(int device) noexcept -> hmem_device {
        return hmem_device{hmem_iface::neuron, device, nullptr};
    }

    /**
     * @brief Creates an hmem_device for Habana SynapseAI memory.
     * @param device The SynapseAI device ordinal.
     * @return An hmem_device configured for SynapseAI.
     */
    static constexpr auto synapseai(int device) noexcept -> hmem_device {
        return hmem_device{hmem_iface::synapseai, device, nullptr};
    }

private:
    /**
     * @brief Private constructor for creating hmem_device with all fields.
     * @param iface_ The memory interface type.
     * @param id_ The device identifier.
     * @param data_ Provider-specific device data.
     */
    constexpr hmem_device(hmem_iface iface_, std::int64_t id_, void* data_) noexcept
        : iface(iface_), device_id(id_), hmem_data(data_) {}
};

/// Allocation made through the selected device API. Unspecified also covers
/// managed/shared allocations; device and host select exclusive native flags.
enum class hmem_allocation_kind { unspecified, device, host };

/// Borrowed DMA-BUF backing. The descriptor and allocation must outlive the MR.
/// The registered range starts at base address + offset in virtual-address mode.
struct dmabuf_registration {
    int fd{-1};
    std::uint64_t offset{};
};

/// Registration attributes kept together for host, device, and DMA-BUF memory.
/// Device allocations require a domain negotiated with capability::hmem.
/// Registration does not take allocation ownership or establish GPU visibility.
struct memory_registration {
    mr_access access{};
    hmem_device device{};
    hmem_allocation_kind allocation{hmem_allocation_kind::unspecified};
    std::optional<dmabuf_registration> dmabuf{};
    /// Configure this MR for remote RMA events. Bind its counter before
    /// enable() when the domain requires mr_mode_flags::rma_event.
    bool rma_events{};
};

/**
 * @class memory_region
 * @brief Represents a registered memory region for RDMA operations.
 *
 * Memory regions must be registered with a domain before they can be used
 * for remote memory access operations. Registration pins the memory and
 * makes it accessible to the fabric hardware. Application-selected keys are
 * allocated uniquely among live Loom registrations in the domain. The domain's
 * memory resource must outlive all registrations. Native registrations created
 * outside Loom must coordinate their keys separately.
 * Collective buffers require mr_access_flags::collective together with send
 * and/or recv access when the domain requires mr_mode_flags::collective.
 */
class memory_region : public fabric_object_base<memory_region> {
public:
    using impl_ptr = detail::pmr_unique_ptr<detail::memory_region_impl>;

    /**
     * @brief Default constructor creating an empty memory region.
     */
    memory_region();

    /**
     * @brief Constructs a memory_region from an implementation pointer.
     * @param impl The implementation pointer.
     */
    explicit memory_region(impl_ptr impl) noexcept;

    /**
     * @brief Destructor.
     */
    ~memory_region();

    /**
     * @brief Deleted copy constructor.
     */
    memory_region(const memory_region&) = delete;
    /**
     * @brief Deleted copy assignment operator.
     */
    auto operator=(const memory_region&) -> memory_region& = delete;
    /**
     * @brief Move constructor.
     */
    memory_region(memory_region&&) noexcept;
    auto operator=(memory_region&&) noexcept -> memory_region&;

    [[nodiscard("memory registration result must be checked for errors")]]
    static auto register_memory(const domain& dom,
                                std::span<std::byte> buffer,
                                mr_access access,
                                memory_resource* resource = nullptr) -> result<memory_region>;

    [[nodiscard("memory registration result must be checked for errors")]]
    static auto register_memory(const domain& dom,
                                void* addr,
                                std::size_t length,
                                mr_access access,
                                memory_resource* resource = nullptr) -> result<memory_region>;

    [[nodiscard("memory registration result must be checked for errors")]]
    static auto register_memory(const domain& dom,
                                void* addr,
                                std::size_t length,
                                const memory_registration& attributes,
                                memory_resource* resource = nullptr) -> result<memory_region>;

    [[nodiscard("hmem registration result must be checked for errors")]]
    static auto register_hmem(const domain& dom,
                              void* addr,
                              std::size_t length,
                              mr_access access,
                              hmem_device device,
                              memory_resource* resource = nullptr) -> result<memory_region>;

    [[nodiscard("dmabuf registration result must be checked for errors")]]
    static auto register_dmabuf(const domain& dom,
                                void* addr,
                                std::size_t length,
                                mr_access access,
                                int fd,
                                std::uint64_t offset = 0,
                                memory_resource* resource = nullptr) -> result<memory_region>;

    [[nodiscard]] auto descriptor() const noexcept -> mr_descriptor;

    [[nodiscard]] auto key() const noexcept -> std::uint64_t;

    /// Start of the registered local span, including any DMA-BUF offset.
    /// Null when a DMA-BUF has no local mapping, or this region is empty.
    [[nodiscard]] auto address() const noexcept -> void*;

    /// Address to exchange with peers: zero for offset-based registration,
    /// otherwise the registered virtual address (including a DMA-BUF offset).
    [[nodiscard]] auto remote_address() const noexcept -> std::uint64_t;

    [[nodiscard]] auto length() const noexcept -> std::size_t;

    /// Check a local byte range without accessing its contents. Requires a
    /// valid, locally mapped MR; empty ranges at its end are permitted.
    [[nodiscard]] auto contains(const void* buffer, std::size_t size) const noexcept -> bool;

    /**
     * @brief Binds this memory region to an endpoint.
     * @param ep The endpoint to bind to.
     * @return Result indicating success or failure.
     */
    auto bind(class endpoint& ep) -> result<void>;

    /// Count remote writes and modifying atomics to this MR. The counter must
    /// outlive the MR, and accessing endpoints need capability::rma_event.
    /// Set memory_registration::rma_events when required by the domain, bind
    /// every resource, then enable the MR before exposing it to peers.
    auto bind(class counter& cntr) -> result<void>;

    /// Enable an MR created disabled by its registration mode. Complete all
    /// endpoint and counter bindings first; no further bindings are permitted.
    auto enable() -> result<void>;

    auto refresh() -> result<void>;

    [[nodiscard]] auto impl_internal_ptr() const noexcept -> void*;
    [[nodiscard]] auto impl_valid() const noexcept -> bool;

private:
    impl_ptr impl_;
    friend class endpoint;
    friend class domain;
};

/**
 * @struct remote_memory
 * @brief Describes a remote memory region for RMA operations.
 *
 * Contains the address, key, and length needed to perform remote memory
 * access operations on a peer's registered memory region.
 */
struct remote_memory : remote_memory_base<remote_memory> {
    std::uint64_t addr{0};  ///< Remote virtual address or region offset
    std::uint64_t key{0};   ///< Remote memory key
    std::size_t length{0};  ///< Length of the remote region in bytes

    /**
     * @brief Default constructor creating an empty remote_memory.
     */
    constexpr remote_memory() noexcept = default;

    /**
     * @brief Constructs a remote_memory with explicit values.
     * @param addr_ The remote virtual address or region offset.
     * @param key_ The remote memory key.
     * @param len_ The length of the region in bytes.
     */
    constexpr remote_memory(std::uint64_t addr_, std::uint64_t key_, std::size_t len_) noexcept
        : addr(addr_), key(key_), length(len_) {}

    /**
     * @brief Creates a remote_memory descriptor from a local memory region.
     * @param mr The local memory region to describe.
     * @return A remote_memory descriptor for the given region.
     */
    static auto from_mr(const memory_region& mr) -> remote_memory {
        return remote_memory{mr.remote_address(), mr.key(), mr.length()};
    }
};

}  // namespace loom
