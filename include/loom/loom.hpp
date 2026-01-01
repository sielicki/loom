// SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only
#pragma once

#include "loom/core/atomic.hpp"
#include "loom/core/collective.hpp"
#include "loom/core/concepts/capabilities.hpp"
#include "loom/core/concepts/protocol_concepts.hpp"
#include "loom/core/concepts/provider_traits.hpp"
#include "loom/core/domain.hpp"
#include "loom/core/endpoint.hpp"
#include "loom/core/endpoint_options.hpp"
#include "loom/core/error.hpp"
#include "loom/core/fabric.hpp"
#include "loom/core/formatters.hpp"
#include "loom/core/immediate_data.hpp"
#include "loom/core/memory.hpp"
#include "loom/core/messaging.hpp"
#include "loom/core/mr_cache.hpp"
#include "loom/core/progress_policy.hpp"
#include "loom/core/provider_atomic.hpp"
#include "loom/core/provider_aware.hpp"
#include "loom/core/request_context.hpp"
#include "loom/core/result.hpp"
#include "loom/core/rma.hpp"
#include "loom/core/scalable_endpoint.hpp"
#include "loom/core/shared_context.hpp"
#include "loom/core/trigger.hpp"
#include "loom/core/types.hpp"
#include "loom/fabric_query.hpp"
#include "loom/protocol/protocol.hpp"
#include "loom/provider_range.hpp"

/**
 * @namespace loom
 * @brief Core namespace for the loom libfabric bindings library.
 */
namespace loom {

/**
 * @struct version_t
 * @brief Version information functor.
 */
inline constexpr struct version_t {
    /**
     * @brief Returns the library version string.
     * @return Version string.
     */
    constexpr auto operator()() const noexcept -> const char* { return "0.1.0"; }
} version;  ///< Global version instance

}  // namespace loom
