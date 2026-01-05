// SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only
/**
 * @file loom_impl.hpp
 * @brief Single-header implementation for loom (stb-style).
 *
 * This header contains all implementation code for loom. To use loom as a
 * header-only library, include this file in exactly ONE .cpp file in your
 * project with LOOM_IMPLEMENTATION defined:
 *
 * @code
 * // In exactly one .cpp file:
 * #define LOOM_IMPLEMENTATION
 * #include <loom/loom_impl.hpp>
 * @endcode
 *
 * All other files should just include <loom/loom.hpp> as normal.
 *
 * Alternatively, link against the pre-built loom library and don't use this
 * header at all.
 */
#pragma once

// First include the public API
#include "loom/loom.hpp"

// Define LOOM_IMPLEMENTATION before including impl headers
#ifndef LOOM_IMPLEMENTATION
#    define LOOM_IMPLEMENTATION
#    define LOOM_IMPLEMENTATION_DEFINED_HERE
#endif

// Include all implementation headers
#include "loom/detail/impl/address_impl.hpp"
#include "loom/detail/impl/address_vector_impl.hpp"
#include "loom/detail/impl/atomic_impl.hpp"
#include "loom/detail/impl/collective_impl.hpp"
#include "loom/detail/impl/completion_queue_impl.hpp"
#include "loom/detail/impl/counter_impl.hpp"
#include "loom/detail/impl/domain_impl.hpp"
#include "loom/detail/impl/endpoint_impl.hpp"
#include "loom/detail/impl/error_impl.hpp"
#include "loom/detail/impl/event_queue_impl.hpp"
#include "loom/detail/impl/fabric_impl.hpp"
#include "loom/detail/impl/memory_impl.hpp"
#include "loom/detail/impl/passive_endpoint_impl.hpp"
#include "loom/detail/impl/rma_impl.hpp"
#include "loom/detail/impl/scalable_endpoint_impl.hpp"
#include "loom/detail/impl/shared_context_impl.hpp"
#include "loom/detail/impl/trigger_impl.hpp"

#ifdef LOOM_IMPLEMENTATION_DEFINED_HERE
#    undef LOOM_IMPLEMENTATION
#    undef LOOM_IMPLEMENTATION_DEFINED_HERE
#endif
