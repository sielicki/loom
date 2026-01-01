// SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only
#pragma once

#include <bit>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <memory_resource>
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
    void* hmem_data{nullptr};              ///< Provider-specific device data

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

/**
 * @class memory_region
 * @brief Represents a registered memory region for RDMA operations.
 *
 * Memory regions must be registered with a domain before they can be used
 * for remote memory access operations. Registration pins the memory and
 * makes it accessible to the fabric hardware.
 */
class memory_region : public fabric_object_base<memory_region> {
public:
    using impl_ptr = detail::pmr_unique_ptr<detail::memory_region_impl>;

    /**
     * @brief Default constructor creating an empty memory region.
     */
    memory_region() = default;

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

    [[nodiscard]] static auto register_memory(domain& dom,
                                              std::span<std::byte> buffer,
                                              mr_access access,
                                              memory_resource* resource = nullptr)
        -> result<memory_region>;

    [[nodiscard]] static auto register_memory(domain& dom,
                                              void* addr,
                                              std::size_t length,
                                              mr_access access,
                                              memory_resource* resource = nullptr)
        -> result<memory_region>;

    [[nodiscard]] static auto register_hmem(domain& dom,
                                            void* addr,
                                            std::size_t length,
                                            mr_access access,
                                            hmem_device device,
                                            memory_resource* resource = nullptr)
        -> result<memory_region>;

    [[nodiscard]] static auto register_dmabuf(domain& dom,
                                              void* addr,
                                              std::size_t length,
                                              mr_access access,
                                              int fd,
                                              std::uint64_t offset = 0,
                                              memory_resource* resource = nullptr)
        -> result<memory_region>;

    [[nodiscard]] auto descriptor() const noexcept -> mr_descriptor;

    [[nodiscard]] auto key() const noexcept -> std::uint64_t;

    [[nodiscard]] auto address() const noexcept -> void*;

    [[nodiscard]] auto length() const noexcept -> std::size_t;

    /**
     * @brief Binds this memory region to an endpoint.
     * @param ep The endpoint to bind to.
     * @return Result indicating success or failure.
     */
    auto bind(class endpoint& ep) -> result<void>;

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
    std::uint64_t addr{0};  ///< Remote virtual address
    std::uint64_t key{0};   ///< Remote memory key
    std::size_t length{0};  ///< Length of the remote region in bytes

    /**
     * @brief Default constructor creating an empty remote_memory.
     */
    constexpr remote_memory() noexcept = default;

    /**
     * @brief Constructs a remote_memory with explicit values.
     * @param addr_ The remote virtual address.
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
        return remote_memory{std::bit_cast<std::uint64_t>(mr.address()), mr.key(), mr.length()};
    }
};

}  // namespace loom
