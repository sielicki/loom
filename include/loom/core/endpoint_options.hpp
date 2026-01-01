// SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only
#pragma once

#include <cstddef>
#include <cstdint>

#include "loom/core/endpoint.hpp"
#include "loom/core/result.hpp"

/**
 * @namespace loom
 * @brief Core namespace for the loom libfabric bindings library.
 */
namespace loom {

/**
 * @namespace loom::ep_opt
 * @brief Endpoint option constants for get/set operations.
 */
namespace ep_opt {

inline constexpr int level_endpoint = 0;  ///< Endpoint-level option

inline constexpr int min_multi_recv = 1;  ///< Minimum multi-receive buffer size
inline constexpr int cm_data_size = 2;    ///< Connection manager data size
inline constexpr int buffered_min = 3;    ///< Minimum buffered receive size
inline constexpr int buffered_limit = 4;  ///< Maximum buffered receive limit

inline constexpr int shared_memory_permitted = 64;  ///< Allow shared memory optimization
inline constexpr int cuda_api_permitted = 65;       ///< Allow CUDA API usage

inline constexpr int efa_emulated_read = 128;                     ///< EFA emulated read status
inline constexpr int efa_emulated_write = 129;                    ///< EFA emulated write status
inline constexpr int efa_write_in_order_aligned_128_bytes = 130;  ///< EFA 128-byte aligned ordering

}  // namespace ep_opt

/**
 * @brief Sets an endpoint option.
 * @param ep The endpoint to configure.
 * @param level The option level (use ep_opt::level_endpoint).
 * @param optname The option name constant.
 * @param optval Pointer to the option value.
 * @param optlen Size of the option value.
 * @return Success or error result.
 */
[[nodiscard]] auto
set_endpoint_option(endpoint& ep, int level, int optname, const void* optval, std::size_t optlen)
    -> result<void>;

/**
 * @brief Gets an endpoint option.
 * @param ep The endpoint to query.
 * @param level The option level (use ep_opt::level_endpoint).
 * @param optname The option name constant.
 * @param optval Pointer to receive the option value.
 * @param optlen Pointer to the value size (updated on return).
 * @return Success or error result.
 */
[[nodiscard]] auto
get_endpoint_option(const endpoint& ep, int level, int optname, void* optval, std::size_t* optlen)
    -> result<void>;

/**
 * @brief Sets the minimum multi-receive buffer size.
 * @param ep The endpoint to configure.
 * @param size The minimum buffer size in bytes.
 * @return Success or error result.
 */
[[nodiscard]] inline auto set_min_multi_recv(endpoint& ep, std::size_t size) -> result<void> {
    return set_endpoint_option(
        ep, ep_opt::level_endpoint, ep_opt::min_multi_recv, &size, sizeof(size));
}

/**
 * @brief Gets the minimum multi-receive buffer size.
 * @param ep The endpoint to query.
 * @return The minimum buffer size or error.
 */
[[nodiscard]] inline auto get_min_multi_recv(const endpoint& ep) -> result<std::size_t> {
    std::size_t size{0};
    std::size_t optlen = sizeof(size);
    auto res =
        get_endpoint_option(ep, ep_opt::level_endpoint, ep_opt::min_multi_recv, &size, &optlen);
    if (!res) {
        return std::unexpected(res.error());
    }
    return size;
}

/**
 * @brief Sets the minimum buffered receive size.
 * @param ep The endpoint to configure.
 * @param size The minimum buffered size in bytes.
 * @return Success or error result.
 */
[[nodiscard]] inline auto set_buffered_min(endpoint& ep, std::size_t size) -> result<void> {
    return set_endpoint_option(
        ep, ep_opt::level_endpoint, ep_opt::buffered_min, &size, sizeof(size));
}

/**
 * @brief Gets the minimum buffered receive size.
 * @param ep The endpoint to query.
 * @return The minimum buffered size or error.
 */
[[nodiscard]] inline auto get_buffered_min(const endpoint& ep) -> result<std::size_t> {
    std::size_t size{0};
    std::size_t optlen = sizeof(size);
    auto res =
        get_endpoint_option(ep, ep_opt::level_endpoint, ep_opt::buffered_min, &size, &optlen);
    if (!res) {
        return std::unexpected(res.error());
    }
    return size;
}

/**
 * @brief Sets the maximum buffered receive limit.
 * @param ep The endpoint to configure.
 * @param size The maximum buffered limit in bytes.
 * @return Success or error result.
 */
[[nodiscard]] inline auto set_buffered_limit(endpoint& ep, std::size_t size) -> result<void> {
    return set_endpoint_option(
        ep, ep_opt::level_endpoint, ep_opt::buffered_limit, &size, sizeof(size));
}

/**
 * @brief Gets the maximum buffered receive limit.
 * @param ep The endpoint to query.
 * @return The maximum buffered limit or error.
 */
[[nodiscard]] inline auto get_buffered_limit(const endpoint& ep) -> result<std::size_t> {
    std::size_t size{0};
    std::size_t optlen = sizeof(size);
    auto res =
        get_endpoint_option(ep, ep_opt::level_endpoint, ep_opt::buffered_limit, &size, &optlen);
    if (!res) {
        return std::unexpected(res.error());
    }
    return size;
}

/**
 * @brief Sets whether shared memory optimization is permitted.
 * @param ep The endpoint to configure.
 * @param permitted True to allow shared memory optimization.
 * @return Success or error result.
 */
[[nodiscard]] inline auto set_shared_memory_permitted(endpoint& ep, bool permitted)
    -> result<void> {
    int val = permitted ? 1 : 0;
    return set_endpoint_option(
        ep, ep_opt::level_endpoint, ep_opt::shared_memory_permitted, &val, sizeof(val));
}

/**
 * @brief Gets whether shared memory optimization is permitted.
 * @param ep The endpoint to query.
 * @return True if permitted, or error.
 */
[[nodiscard]] inline auto get_shared_memory_permitted(const endpoint& ep) -> result<bool> {
    int val{0};
    std::size_t optlen = sizeof(val);
    auto res = get_endpoint_option(
        ep, ep_opt::level_endpoint, ep_opt::shared_memory_permitted, &val, &optlen);
    if (!res) {
        return std::unexpected(res.error());
    }
    return val != 0;
}

/**
 * @brief Sets whether CUDA API usage is permitted.
 * @param ep The endpoint to configure.
 * @param permitted True to allow CUDA API usage.
 * @return Success or error result.
 */
[[nodiscard]] inline auto set_cuda_api_permitted(endpoint& ep, bool permitted) -> result<void> {
    int val = permitted ? 1 : 0;
    return set_endpoint_option(
        ep, ep_opt::level_endpoint, ep_opt::cuda_api_permitted, &val, sizeof(val));
}

/**
 * @brief Gets whether CUDA API usage is permitted.
 * @param ep The endpoint to query.
 * @return True if permitted, or error.
 */
[[nodiscard]] inline auto get_cuda_api_permitted(const endpoint& ep) -> result<bool> {
    int val{0};
    std::size_t optlen = sizeof(val);
    auto res =
        get_endpoint_option(ep, ep_opt::level_endpoint, ep_opt::cuda_api_permitted, &val, &optlen);
    if (!res) {
        return std::unexpected(res.error());
    }
    return val != 0;
}

/**
 * @namespace loom::efa
 * @brief EFA (Elastic Fabric Adapter) specific endpoint options.
 */
namespace efa {

/**
 * @brief Gets whether read operations are emulated.
 * @param ep The endpoint to query.
 * @return True if reads are emulated, or error.
 */
[[nodiscard]] inline auto get_emulated_read(const endpoint& ep) -> result<bool> {
    int val{0};
    std::size_t optlen = sizeof(val);
    auto res =
        get_endpoint_option(ep, ep_opt::level_endpoint, ep_opt::efa_emulated_read, &val, &optlen);
    if (!res) {
        return std::unexpected(res.error());
    }
    return val != 0;
}

/**
 * @brief Gets whether write operations are emulated.
 * @param ep The endpoint to query.
 * @return True if writes are emulated, or error.
 */
[[nodiscard]] inline auto get_emulated_write(const endpoint& ep) -> result<bool> {
    int val{0};
    std::size_t optlen = sizeof(val);
    auto res =
        get_endpoint_option(ep, ep_opt::level_endpoint, ep_opt::efa_emulated_write, &val, &optlen);
    if (!res) {
        return std::unexpected(res.error());
    }
    return val != 0;
}

/**
 * @brief Sets whether 128-byte aligned in-order writes are enabled.
 * @param ep The endpoint to configure.
 * @param enable True to enable this feature.
 * @return Success or error result.
 */
[[nodiscard]] inline auto set_write_in_order_aligned_128_bytes(endpoint& ep, bool enable)
    -> result<void> {
    int val = enable ? 1 : 0;
    return set_endpoint_option(ep,
                               ep_opt::level_endpoint,
                               ep_opt::efa_write_in_order_aligned_128_bytes,
                               &val,
                               sizeof(val));
}

/**
 * @brief Gets whether 128-byte aligned in-order writes are enabled.
 * @param ep The endpoint to query.
 * @return True if enabled, or error.
 */
[[nodiscard]] inline auto get_write_in_order_aligned_128_bytes(const endpoint& ep) -> result<bool> {
    int val{0};
    std::size_t optlen = sizeof(val);
    auto res = get_endpoint_option(
        ep, ep_opt::level_endpoint, ep_opt::efa_write_in_order_aligned_128_bytes, &val, &optlen);
    if (!res) {
        return std::unexpected(res.error());
    }
    return val != 0;
}

}  // namespace efa

}  // namespace loom
