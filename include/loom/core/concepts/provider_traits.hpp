// SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only
#pragma once

#include <concepts>
#include <cstdint>
#include <type_traits>

#include "loom/core/types.hpp"

/**
 * @namespace loom
 * @brief Core namespace for the loom libfabric bindings library.
 */
namespace loom {

/**
 * @namespace provider
 * @brief Provider tag types for compile-time provider selection.
 */
namespace provider {
/**
 * @struct verbs_tag
 * @brief Tag type for the RDMA Verbs provider (InfiniBand, RoCE).
 */
struct verbs_tag {};
/**
 * @struct efa_tag
 * @brief Tag type for the AWS Elastic Fabric Adapter provider.
 */
struct efa_tag {};
/**
 * @struct slingshot_tag
 * @brief Tag type for the HPE Slingshot (CXI) provider.
 */
struct slingshot_tag {};
/**
 * @struct shm_tag
 * @brief Tag type for the shared memory provider.
 */
struct shm_tag {};
/**
 * @struct tcp_tag
 * @brief Tag type for the TCP sockets provider.
 */
struct tcp_tag {};
/**
 * @struct ucx_tag
 * @brief Tag type for the UCX provider.
 */
struct ucx_tag {};
}  // namespace provider

/**
 * @struct provider_traits
 * @brief Primary template for provider-specific traits and capabilities.
 * @tparam Tag The provider tag type.
 */
template <typename Tag>
struct provider_traits;

/**
 * @struct provider_traits<provider::verbs_tag>
 * @brief Traits specialization for the RDMA Verbs provider.
 *
 * Verbs supports native atomics, manual progress, and requires local memory keys.
 */
template <>
struct provider_traits<provider::verbs_tag> {
    using tag_type = provider::verbs_tag;
    static constexpr bool supports_native_atomics = true;
    static constexpr bool uses_staged_atomics = false;
    static constexpr bool supports_inject = true;
    static constexpr bool supports_selective_completion = true;
    static constexpr bool uses_completion_queue_for_inject = false;
    static constexpr bool supports_rma_event = true;
    static constexpr bool supports_multi_recv = true;
    static constexpr mr_mode default_mr_mode =
        mr_mode_flags::basic | mr_mode_flags::local | mr_mode_flags::prov_key;
    static constexpr std::size_t max_inject_size = 64;

    static constexpr progress_mode default_control_progress = progress_mode::manual;
    static constexpr progress_mode default_data_progress = progress_mode::manual;
    static constexpr bool supports_auto_progress = false;

    /**
     * @brief Computes the remote address for RMA operations.
     * @param base The base address of the remote memory region.
     * @param offset The offset within the memory region.
     * @return The computed remote address.
     */
    [[nodiscard]] static constexpr auto compute_remote_addr(std::uint64_t base,
                                                            std::uint64_t offset) noexcept
        -> std::uint64_t {
        return base + offset;
    }

    /**
     * @brief Indicates whether this provider requires local memory keys.
     * @return True if local keys are required for memory registration.
     */
    [[nodiscard]] static constexpr auto requires_local_key() noexcept -> bool { return true; }

    /**
     * @brief Returns the provider name string.
     * @return The libfabric provider name.
     */
    [[nodiscard]] static constexpr auto provider_name() noexcept -> const char* { return "verbs"; }
};

/**
 * @struct provider_traits<provider::efa_tag>
 * @brief Traits specialization for the AWS Elastic Fabric Adapter provider.
 *
 * EFA uses staged atomics and does not support RMA events.
 */
template <>
struct provider_traits<provider::efa_tag> {
    using tag_type = provider::efa_tag;
    static constexpr bool supports_native_atomics = false;
    static constexpr bool uses_staged_atomics = true;
    static constexpr bool supports_inject = true;
    static constexpr bool supports_selective_completion = true;
    static constexpr bool uses_completion_queue_for_inject = false;
    static constexpr bool supports_rma_event = false;
    static constexpr bool supports_multi_recv = true;
    static constexpr mr_mode default_mr_mode = mr_mode_flags::basic | mr_mode_flags::prov_key;
    static constexpr std::size_t max_inject_size = 32;

    static constexpr progress_mode default_control_progress = progress_mode::manual;
    static constexpr progress_mode default_data_progress = progress_mode::manual;
    static constexpr bool supports_auto_progress = false;

    /**
     * @brief Computes the remote address for RMA operations.
     * @param base The base address of the remote memory region.
     * @param offset The offset within the memory region.
     * @return The computed remote address.
     */
    [[nodiscard]] static constexpr auto compute_remote_addr(std::uint64_t base,
                                                            std::uint64_t offset) noexcept
        -> std::uint64_t {
        return base + offset;
    }

    /**
     * @brief Indicates whether this provider requires local memory keys.
     * @return True if local keys are required for memory registration.
     */
    [[nodiscard]] static constexpr auto requires_local_key() noexcept -> bool { return false; }

    /**
     * @brief Returns the provider name string.
     * @return The libfabric provider name.
     */
    [[nodiscard]] static constexpr auto provider_name() noexcept -> const char* { return "efa"; }
};

/**
 * @struct provider_traits<provider::slingshot_tag>
 * @brief Traits specialization for the HPE Slingshot (CXI) provider.
 *
 * Slingshot supports native atomics, automatic progress, and scalable memory registration.
 */
template <>
struct provider_traits<provider::slingshot_tag> {
    using tag_type = provider::slingshot_tag;
    static constexpr bool supports_native_atomics = true;
    static constexpr bool uses_staged_atomics = false;
    static constexpr bool supports_inject = true;
    static constexpr bool supports_selective_completion = true;
    static constexpr bool uses_completion_queue_for_inject = false;
    static constexpr bool supports_rma_event = true;
    static constexpr bool supports_multi_recv = true;
    static constexpr mr_mode default_mr_mode = mr_mode_flags::scalable | mr_mode_flags::virt_addr;
    static constexpr std::size_t max_inject_size = 64;

    static constexpr progress_mode default_control_progress = progress_mode::auto_progress;
    static constexpr progress_mode default_data_progress = progress_mode::auto_progress;
    static constexpr bool supports_auto_progress = true;

    /**
     * @brief Computes the remote address for RMA operations.
     * @param base The base address of the remote memory region.
     * @param offset The offset within the memory region.
     * @return The computed remote address.
     */
    [[nodiscard]] static constexpr auto compute_remote_addr(std::uint64_t base,
                                                            std::uint64_t offset) noexcept
        -> std::uint64_t {
        return base + offset;
    }

    /**
     * @brief Indicates whether this provider requires local memory keys.
     * @return True if local keys are required for memory registration.
     */
    [[nodiscard]] static constexpr auto requires_local_key() noexcept -> bool { return false; }

    /**
     * @brief Returns the provider name string.
     * @return The libfabric provider name.
     */
    [[nodiscard]] static constexpr auto provider_name() noexcept -> const char* { return "cxi"; }
};

/**
 * @struct provider_traits<provider::shm_tag>
 * @brief Traits specialization for the shared memory provider.
 *
 * SHM supports native atomics and automatic progress with virtual address mode.
 */
template <>
struct provider_traits<provider::shm_tag> {
    using tag_type = provider::shm_tag;
    static constexpr bool supports_native_atomics = true;
    static constexpr bool uses_staged_atomics = false;
    static constexpr bool supports_inject = true;
    static constexpr bool supports_selective_completion = true;
    static constexpr bool uses_completion_queue_for_inject = false;
    static constexpr bool supports_rma_event = true;
    static constexpr bool supports_multi_recv = true;
    static constexpr mr_mode default_mr_mode = mr_mode_flags::virt_addr;
    static constexpr std::size_t max_inject_size = 4096;

    static constexpr progress_mode default_control_progress = progress_mode::auto_progress;
    static constexpr progress_mode default_data_progress = progress_mode::auto_progress;
    static constexpr bool supports_auto_progress = true;

    /**
     * @brief Computes the remote address for RMA operations.
     * @param base The base address of the remote memory region.
     * @param offset The offset within the memory region.
     * @return The computed remote address.
     */
    [[nodiscard]] static constexpr auto compute_remote_addr(std::uint64_t base,
                                                            std::uint64_t offset) noexcept
        -> std::uint64_t {
        return base + offset;
    }

    /**
     * @brief Indicates whether this provider requires local memory keys.
     * @return True if local keys are required for memory registration.
     */
    [[nodiscard]] static constexpr auto requires_local_key() noexcept -> bool { return false; }

    /**
     * @brief Returns the provider name string.
     * @return The libfabric provider name.
     */
    [[nodiscard]] static constexpr auto provider_name() noexcept -> const char* { return "shm"; }
};

/**
 * @struct provider_traits<provider::tcp_tag>
 * @brief Traits specialization for the TCP sockets provider.
 *
 * TCP uses staged atomics and does not support RMA events.
 */
template <>
struct provider_traits<provider::tcp_tag> {
    using tag_type = provider::tcp_tag;
    static constexpr bool supports_native_atomics = false;
    static constexpr bool uses_staged_atomics = true;
    static constexpr bool supports_inject = true;
    static constexpr bool supports_selective_completion = true;
    static constexpr bool uses_completion_queue_for_inject = false;
    static constexpr bool supports_rma_event = false;
    static constexpr bool supports_multi_recv = true;
    static constexpr mr_mode default_mr_mode = mr_mode_flags::basic;
    static constexpr std::size_t max_inject_size = 64;

    static constexpr progress_mode default_control_progress = progress_mode::manual;
    static constexpr progress_mode default_data_progress = progress_mode::manual;
    static constexpr bool supports_auto_progress = false;

    /**
     * @brief Computes the remote address for RMA operations.
     * @param base The base address of the remote memory region.
     * @param offset The offset within the memory region.
     * @return The computed remote address.
     */
    [[nodiscard]] static constexpr auto compute_remote_addr(std::uint64_t base,
                                                            std::uint64_t offset) noexcept
        -> std::uint64_t {
        return base + offset;
    }

    /**
     * @brief Indicates whether this provider requires local memory keys.
     * @return True if local keys are required for memory registration.
     */
    [[nodiscard]] static constexpr auto requires_local_key() noexcept -> bool { return false; }

    /**
     * @brief Returns the provider name string.
     * @return The libfabric provider name.
     */
    [[nodiscard]] static constexpr auto provider_name() noexcept -> const char* { return "tcp"; }
};

/**
 * @struct provider_traits<provider::ucx_tag>
 * @brief Traits specialization for the UCX provider.
 *
 * UCX supports native atomics with optional automatic progress.
 */
template <>
struct provider_traits<provider::ucx_tag> {
    using tag_type = provider::ucx_tag;
    static constexpr bool supports_native_atomics = true;
    static constexpr bool uses_staged_atomics = false;
    static constexpr bool supports_inject = true;
    static constexpr bool supports_selective_completion = true;
    static constexpr bool uses_completion_queue_for_inject = false;
    static constexpr bool supports_rma_event = true;
    static constexpr bool supports_multi_recv = true;
    static constexpr mr_mode default_mr_mode = mr_mode_flags::basic;
    static constexpr std::size_t max_inject_size = 128;

    static constexpr progress_mode default_control_progress = progress_mode::manual;
    static constexpr progress_mode default_data_progress = progress_mode::manual;
    static constexpr bool supports_auto_progress = true;

    /**
     * @brief Computes the remote address for RMA operations.
     * @param base The base address of the remote memory region.
     * @param offset The offset within the memory region.
     * @return The computed remote address.
     */
    [[nodiscard]] static constexpr auto compute_remote_addr(std::uint64_t base,
                                                            std::uint64_t offset) noexcept
        -> std::uint64_t {
        return base + offset;
    }

    /**
     * @brief Indicates whether this provider requires local memory keys.
     * @return True if local keys are required for memory registration.
     */
    [[nodiscard]] static constexpr auto requires_local_key() noexcept -> bool { return false; }

    /**
     * @brief Returns the provider name string.
     * @return The libfabric provider name.
     */
    [[nodiscard]] static constexpr auto provider_name() noexcept -> const char* { return "ucx"; }
};

/**
 * @concept provider_tag
 * @brief Concept that validates a type has the required provider_traits interface.
 * @tparam P The provider tag type to validate.
 */
template <typename P>
concept provider_tag = requires {
    typename provider_traits<P>;
    typename provider_traits<P>::tag_type;
    { provider_traits<P>::supports_native_atomics } -> std::same_as<const bool&>;
    { provider_traits<P>::uses_staged_atomics } -> std::same_as<const bool&>;
    { provider_traits<P>::supports_inject } -> std::same_as<const bool&>;
    { provider_traits<P>::default_mr_mode } -> std::same_as<const mr_mode&>;
    { provider_traits<P>::max_inject_size } -> std::same_as<const std::size_t&>;
    { provider_traits<P>::default_control_progress } -> std::same_as<const progress_mode&>;
    { provider_traits<P>::default_data_progress } -> std::same_as<const progress_mode&>;
    { provider_traits<P>::supports_auto_progress } -> std::same_as<const bool&>;
    { provider_traits<P>::compute_remote_addr(std::uint64_t{}, std::uint64_t{}) } noexcept;
    { provider_traits<P>::requires_local_key() } noexcept -> std::same_as<bool>;
    { provider_traits<P>::provider_name() } noexcept -> std::same_as<const char*>;
};

/**
 * @concept native_atomic_provider
 * @brief Concept for providers that support native hardware atomics.
 * @tparam P The provider tag type.
 */
template <typename P>
concept native_atomic_provider = provider_tag<P> && provider_traits<P>::supports_native_atomics &&
                                 !provider_traits<P>::uses_staged_atomics;

/**
 * @concept staged_atomic_provider
 * @brief Concept for providers that use software-staged atomics.
 * @tparam P The provider tag type.
 */
template <typename P>
concept staged_atomic_provider = provider_tag<P> && provider_traits<P>::uses_staged_atomics &&
                                 !provider_traits<P>::supports_native_atomics;

/**
 * @concept inject_capable_provider
 * @brief Concept for providers that support inject operations.
 * @tparam P The provider tag type.
 */
template <typename P>
concept inject_capable_provider = provider_tag<P> && provider_traits<P>::supports_inject;

/**
 * @concept selective_completion_provider
 * @brief Concept for providers that support selective completion.
 * @tparam P The provider tag type.
 */
template <typename P>
concept selective_completion_provider =
    provider_tag<P> && provider_traits<P>::supports_selective_completion;

/**
 * @concept rma_event_provider
 * @brief Concept for providers that support RMA event notifications.
 * @tparam P The provider tag type.
 */
template <typename P>
concept rma_event_provider = provider_tag<P> && provider_traits<P>::supports_rma_event;

/**
 * @concept multi_recv_provider
 * @brief Concept for providers that support multi-receive buffers.
 * @tparam P The provider tag type.
 */
template <typename P>
concept multi_recv_provider = provider_tag<P> && provider_traits<P>::supports_multi_recv;

/**
 * @concept local_key_required_provider
 * @brief Concept for providers that require local memory keys.
 * @tparam P The provider tag type.
 */
template <typename P>
concept local_key_required_provider = provider_tag<P> && provider_traits<P>::requires_local_key();

/**
 * @concept auto_progress_provider
 * @brief Concept for providers that support automatic progress.
 * @tparam P The provider tag type.
 */
template <typename P>
concept auto_progress_provider = provider_tag<P> && provider_traits<P>::supports_auto_progress;

/**
 * @concept manual_progress_provider
 * @brief Concept for providers that require manual progress.
 * @tparam P The provider tag type.
 */
template <typename P>
concept manual_progress_provider = provider_tag<P> && !provider_traits<P>::supports_auto_progress;

/**
 * @concept auto_data_progress_provider
 * @brief Concept for providers with automatic data progress by default.
 * @tparam P The provider tag type.
 */
template <typename P>
concept auto_data_progress_provider =
    provider_tag<P> && provider_traits<P>::default_data_progress == progress_mode::auto_progress;

/**
 * @concept manual_data_progress_provider
 * @brief Concept for providers with manual data progress by default.
 * @tparam P The provider tag type.
 */
template <typename P>
concept manual_data_progress_provider =
    provider_tag<P> && provider_traits<P>::default_data_progress == progress_mode::manual;

/**
 * @struct atomic_dispatch
 * @brief Dispatches atomic operations based on provider capabilities.
 * @tparam P The provider tag type.
 */
template <provider_tag P>
struct atomic_dispatch {
    /**
     * @brief Executes an atomic operation using the appropriate method for the provider.
     * @tparam Op The operation type.
     * @param op The operation to execute.
     * @return The result of the atomic operation.
     */
    template <typename Op>
    [[nodiscard]] static constexpr auto execute(Op&& op) {
        if constexpr (native_atomic_provider<P>) {
            return std::forward<Op>(op).execute_native();
        } else if constexpr (staged_atomic_provider<P>) {
            return std::forward<Op>(op).execute_staged();
        } else {
            static_assert(native_atomic_provider<P> || staged_atomic_provider<P>,
                          "Provider must support either native or staged atomics");
        }
    }
};

/**
 * @brief Checks if a message size can use inject for the given provider.
 * @tparam P The provider tag type.
 * @param size The message size in bytes.
 * @return True if the size is within the inject limit.
 */
template <provider_tag P>
[[nodiscard]] constexpr auto can_inject(std::size_t size) noexcept -> bool {
    return provider_traits<P>::supports_inject && size <= provider_traits<P>::max_inject_size;
}

/**
 * @brief Gets the default memory region mode for the provider.
 * @tparam P The provider tag type.
 * @return The default mr_mode flags.
 */
template <provider_tag P>
[[nodiscard]] constexpr auto get_mr_mode() noexcept -> mr_mode {
    return provider_traits<P>::default_mr_mode;
}

/**
 * @brief Computes an RMA address for the given provider.
 * @tparam P The provider tag type.
 * @param base The base address.
 * @param offset The offset from base.
 * @return The computed remote address.
 */
template <provider_tag P>
[[nodiscard]] constexpr auto compute_rma_addr(std::uint64_t base, std::uint64_t offset) noexcept
    -> std::uint64_t {
    return provider_traits<P>::compute_remote_addr(base, offset);
}

/**
 * @brief Gets the default control progress mode for the provider.
 * @tparam P The provider tag type.
 * @return The default control progress mode.
 */
template <provider_tag P>
[[nodiscard]] constexpr auto get_default_control_progress() noexcept -> progress_mode {
    return provider_traits<P>::default_control_progress;
}

/**
 * @brief Gets the default data progress mode for the provider.
 * @tparam P The provider tag type.
 * @return The default data progress mode.
 */
template <provider_tag P>
[[nodiscard]] constexpr auto get_default_data_progress() noexcept -> progress_mode {
    return provider_traits<P>::default_data_progress;
}

/**
 * @brief Checks if the provider requires manual progress.
 * @tparam P The provider tag type.
 * @return True if manual progress is required.
 */
template <provider_tag P>
[[nodiscard]] constexpr auto requires_manual_progress() noexcept -> bool {
    return provider_traits<P>::default_data_progress == progress_mode::manual;
}

static_assert(native_atomic_provider<provider::verbs_tag>);
static_assert(native_atomic_provider<provider::slingshot_tag>);
static_assert(staged_atomic_provider<provider::efa_tag>);
static_assert(staged_atomic_provider<provider::tcp_tag>);
static_assert(inject_capable_provider<provider::verbs_tag>);
static_assert(inject_capable_provider<provider::efa_tag>);
static_assert(local_key_required_provider<provider::verbs_tag>);
static_assert(!local_key_required_provider<provider::efa_tag>);

static_assert(auto_progress_provider<provider::slingshot_tag>);
static_assert(auto_progress_provider<provider::shm_tag>);
static_assert(manual_progress_provider<provider::verbs_tag>);
static_assert(manual_progress_provider<provider::efa_tag>);
static_assert(manual_progress_provider<provider::tcp_tag>);

}  // namespace loom
