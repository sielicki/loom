// SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only
#pragma once

#include <concepts>
#include <optional>
#include <string_view>

#include "loom/core/endpoint_types.hpp"
#include "loom/core/fabric.hpp"
#include "loom/core/result.hpp"
#include "loom/core/types.hpp"
#include "loom/provider_range.hpp"

/**
 * @namespace loom
 * @brief Core namespace for the loom libfabric bindings library.
 */
namespace loom {

/**
 * @concept fabric_capability
 * @brief Concept for types describing fabric capability requirements.
 * @tparam T The type to check.
 */
template <typename T>
concept fabric_capability = requires {
    { T::required_caps } -> std::convertible_to<caps>;
    { T::endpoint_type } -> std::convertible_to<endpoint_type>;
    { T::optional_caps } -> std::convertible_to<caps>;
};

/**
 * @struct rdm_tagged_messaging
 * @brief Capability descriptor for RDM with tagged messaging.
 */
struct rdm_tagged_messaging {
    static constexpr caps required_caps = capability::msg | capability::tagged;   ///< Required caps
    static constexpr caps optional_caps = capability::fence;                      ///< Optional caps
    static constexpr endpoint_type endpoint_type = endpoint_types::rdm;           ///< Endpoint type
    static constexpr std::string_view description = "RDM with tagged messaging";  ///< Description
};

/**
 * @struct rdma_write_ops
 * @brief Capability descriptor for RDMA write operations.
 */
struct rdma_write_ops {
    static constexpr caps required_caps =
        capability::rma | capability::remote_write;                           ///< Required caps
    static constexpr caps optional_caps = capability::remote_read;            ///< Optional caps
    static constexpr endpoint_type endpoint_type = endpoint_types::rdm;       ///< Endpoint type
    static constexpr std::string_view description = "RDMA write operations";  ///< Description
};

/**
 * @struct rdma_read_ops
 * @brief Capability descriptor for RDMA read operations.
 */
struct rdma_read_ops {
    static constexpr caps required_caps =
        capability::rma | capability::remote_read;                           ///< Required caps
    static constexpr caps optional_caps = capability::remote_write;          ///< Optional caps
    static constexpr endpoint_type endpoint_type = endpoint_types::rdm;      ///< Endpoint type
    static constexpr std::string_view description = "RDMA read operations";  ///< Description
};

/**
 * @struct gpu_direct_rdma
 * @brief Capability descriptor for GPUDirect RDMA.
 */
struct gpu_direct_rdma {
    static constexpr caps required_caps = capability::hmem;                    ///< Required caps
    static constexpr caps optional_caps{0ULL};                                 ///< Optional caps
    static constexpr endpoint_type endpoint_type = endpoint_types::rdm;        ///< Endpoint type
    static constexpr std::string_view description = "GPUDirect RDMA support";  ///< Description
};

/**
 * @struct atomic_ops
 * @brief Capability descriptor for atomic operations.
 */
struct atomic_ops {
    static constexpr caps required_caps = capability::atomic;             ///< Required caps
    static constexpr caps optional_caps{0ULL};                            ///< Optional caps
    static constexpr endpoint_type endpoint_type = endpoint_types::rdm;   ///< Endpoint type
    static constexpr std::string_view description = "Atomic operations";  ///< Description
};

/**
 * @struct connection_oriented_messaging
 * @brief Capability descriptor for connection-oriented messaging.
 */
struct connection_oriented_messaging {
    static constexpr caps required_caps = capability::msg;               ///< Required caps
    static constexpr caps optional_caps = capability::rma;               ///< Optional caps
    static constexpr endpoint_type endpoint_type = endpoint_types::msg;  ///< Endpoint type
    static constexpr std::string_view description =
        "Connection-oriented messaging";  ///< Description
};

/**
 * @struct datagram_messaging
 * @brief Capability descriptor for datagram messaging.
 */
struct datagram_messaging {
    static constexpr caps required_caps = capability::msg;                 ///< Required caps
    static constexpr caps optional_caps{0ULL};                             ///< Optional caps
    static constexpr endpoint_type endpoint_type = endpoint_types::dgram;  ///< Endpoint type
    static constexpr std::string_view description = "Datagram messaging";  ///< Description
};

/**
 * @struct combined_capabilities
 * @brief Combines multiple capability descriptors into one.
 * @tparam Caps The capability types to combine.
 */
template <fabric_capability... Caps>
struct combined_capabilities {
    static constexpr caps required_caps = (Caps::required_caps | ...);  ///< Combined required caps

    static constexpr caps optional_caps = (Caps::optional_caps | ...);  ///< Combined optional caps

    static constexpr loom::endpoint_type endpoint_type = []() consteval {
        if constexpr (sizeof...(Caps) == 0) {
            return endpoint_types::msg;
        } else {
            constexpr auto get_first = []<typename First, typename... Rest>() consteval {
                return First::endpoint_type;
            };
            constexpr loom::endpoint_type first_type = get_first.template operator()<Caps...>();

            static_assert(((Caps::endpoint_type == first_type) && ...),
                          "All combined capabilities must use the same endpoint type");

            return first_type;
        }
    }();

    static constexpr std::string_view description = "Combined capabilities";  ///< Description
};

static_assert(fabric_capability<rdm_tagged_messaging>);
static_assert(fabric_capability<rdma_write_ops>);
static_assert(fabric_capability<gpu_direct_rdma>);
static_assert(fabric_capability<atomic_ops>);

/**
 * @class fabric_query
 * @brief Query builder for finding providers with specific capabilities.
 * @tparam RequiredCaps The required capability descriptors.
 */
template <fabric_capability... RequiredCaps>
class fabric_query {
public:
    using capabilities = combined_capabilities<RequiredCaps...>;  ///< Combined capabilities

    /**
     * @brief Builds fabric hints from the required capabilities.
     * @return The fabric hints structure.
     */
    [[nodiscard]] static constexpr auto build_hints() -> fabric_hints {
        fabric_hints hints{};

        if constexpr (sizeof...(RequiredCaps) > 0) {
            hints.capabilities = capabilities::required_caps;
            hints.ep_type = capabilities::endpoint_type;
        }

        return hints;
    }

    /**
     * @brief Queries for matching providers.
     * @return A range of matching providers or error.
     */
    [[nodiscard]] static auto query() -> result<provider_range> {
        auto hints = build_hints();
        auto info_result = query_fabric(hints);

        if (!info_result) {
            return std::unexpected(info_result.error());
        }

        return provider_range{std::move(*info_result)};
    }

    /**
     * @brief Queries for providers with additional filtering.
     * @tparam Predicate The filter predicate type.
     * @param pred The filter predicate.
     * @return Filtered view of matching providers.
     */
    template <typename Predicate>
    [[nodiscard]] static auto query_filtered(Predicate&& pred) {
        auto range_result = query();
        if (!range_result) {
            return range_result;
        }

        return std::ranges::views::filter(*range_result, std::forward<Predicate>(pred));
    }

    /**
     * @brief Queries for the first matching provider.
     * @return The first matching provider or nullopt if none found.
     */
    [[nodiscard]] static auto query_first() -> result<std::optional<fabric_info>> {
        auto range_result = query();
        if (!range_result) {
            return std::unexpected(range_result.error());
        }

        auto& range = *range_result;
        if (range.empty()) {
            return std::nullopt;
        }

        return std::nullopt;
    }

    /**
     * @brief Gets the required capabilities at compile time.
     * @return The required capabilities.
     */
    [[nodiscard]] static consteval auto get_required_caps() -> caps {
        if constexpr (sizeof...(RequiredCaps) > 0) {
            return capabilities::required_caps;
        } else {
            return caps{0ULL};
        }
    }

    /**
     * @brief Gets the optional capabilities at compile time.
     * @return The optional capabilities.
     */
    [[nodiscard]] static consteval auto get_optional_caps() -> caps {
        if constexpr (sizeof...(RequiredCaps) > 0) {
            return capabilities::optional_caps;
        } else {
            return caps{0ULL};
        }
    }

    /**
     * @brief Gets the endpoint type at compile time.
     * @return The required endpoint type.
     */
    [[nodiscard]] static consteval auto get_endpoint_type() -> endpoint_type {
        if constexpr (sizeof...(RequiredCaps) > 0) {
            return capabilities::endpoint_type;
        } else {
            return endpoint_types::msg;
        }
    }
};

/**
 * @brief Queries for providers with specific capabilities.
 * @tparam Caps The required capability descriptors.
 * @return A range of matching providers or error.
 */
template <fabric_capability... Caps>
[[nodiscard]] auto query_providers() -> result<provider_range> {
    return fabric_query<Caps...>::query();
}

/**
 * @brief Queries for providers with filtering.
 * @tparam Caps The required capability descriptors.
 * @tparam Predicate The filter predicate type.
 * @param pred The filter predicate to apply.
 * @return Filtered view of matching providers.
 */
template <fabric_capability... Caps, typename Predicate>
[[nodiscard]] auto query_providers(Predicate&& pred) {
    return fabric_query<Caps...>::query_filtered(std::forward<Predicate>(pred));
}

}  // namespace loom
