// SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only
#pragma once

/**
 * @file concepts.hpp
 * @brief P2300 std::execution concept detection and forwarding.
 *
 * This header provides a generic interface to P2300 sender/receiver concepts
 * that works with multiple implementations:
 * - stdexec (NVIDIA's reference implementation)
 * - beman::execution (Beman project implementation)
 * - std::execution (future C++26 standard)
 *
 * The detection is done at compile time through feature macros and header
 * availability. User code should use the types in the loom::execution
 * namespace which are aliased to the detected implementation.
 *
 * ## Usage
 *
 * ```cpp
 * #include <loom/execution/concepts.hpp>
 *
 * // Use loom::execution aliases - works with any backend
 * static_assert(loom::execution::sender<my_sender>);
 * loom::execution::set_value(receiver, args...);
 * ```
 *
 * ## Backend Selection
 *
 * The backend is selected in this priority order:
 * 1. std::execution (if available in C++26+)
 * 2. stdexec (if LOOM_USE_STDEXEC is defined or <stdexec/execution.hpp> exists)
 * 3. beman::execution (if <beman/execution/execution.hpp> exists)
 *
 * You can force a specific backend by defining one of:
 * - LOOM_EXECUTION_USE_STD
 * - LOOM_EXECUTION_USE_STDEXEC
 * - LOOM_EXECUTION_USE_BEMAN
 */

#include <concepts>
#include <system_error>
#include <type_traits>
#include <version>

// Detect available execution library implementations
// Priority: std::execution > stdexec > beman::execution

// Check for C++26 std::execution
#if defined(__cpp_lib_execution) && __cpp_lib_execution >= 202302L
#    define LOOM_HAS_STD_EXECUTION 1
#else
#    define LOOM_HAS_STD_EXECUTION 0
#endif

// Check for stdexec
#if __has_include(<stdexec/execution.hpp>)
#    define LOOM_HAS_STDEXEC 1
#else
#    define LOOM_HAS_STDEXEC 0
#endif

// Check for beman::execution
#if __has_include(<beman/execution/execution.hpp>)
#    define LOOM_HAS_BEMAN_EXECUTION 1
#else
#    define LOOM_HAS_BEMAN_EXECUTION 0
#endif

// Determine which backend to use
#if defined(LOOM_EXECUTION_USE_STD) && LOOM_HAS_STD_EXECUTION
#    define LOOM_EXECUTION_BACKEND_STD 1
#elif defined(LOOM_EXECUTION_USE_STDEXEC) && LOOM_HAS_STDEXEC
#    define LOOM_EXECUTION_BACKEND_STDEXEC 1
#elif defined(LOOM_EXECUTION_USE_BEMAN) && LOOM_HAS_BEMAN_EXECUTION
#    define LOOM_EXECUTION_BACKEND_BEMAN 1
#elif LOOM_HAS_STD_EXECUTION
#    define LOOM_EXECUTION_BACKEND_STD 1
#elif LOOM_HAS_STDEXEC
#    define LOOM_EXECUTION_BACKEND_STDEXEC 1
#elif LOOM_HAS_BEMAN_EXECUTION
#    define LOOM_EXECUTION_BACKEND_BEMAN 1
#else
#    define LOOM_EXECUTION_BACKEND_NONE 1
#endif

// Include the appropriate backend
#if defined(LOOM_EXECUTION_BACKEND_STD)
#    include <execution>
#elif defined(LOOM_EXECUTION_BACKEND_STDEXEC)
#    include <stdexec/execution.hpp>
#elif defined(LOOM_EXECUTION_BACKEND_BEMAN)
#    include <beman/execution/execution.hpp>
#endif

/**
 * @namespace loom::execution
 * @brief P2300-compatible execution primitives for loom.
 *
 * This namespace provides type aliases and concepts that forward to the
 * detected P2300 implementation (std::execution, stdexec, or beman::execution).
 */
namespace loom::execution {

#if defined(LOOM_EXECUTION_BACKEND_STD)

// Forward std::execution types
namespace impl = std::execution;

using std::execution::operation_state;
using std::execution::receiver;
using std::execution::scheduler;
using std::execution::sender;

using std::execution::connect;
using std::execution::schedule;
using std::execution::set_error;
using std::execution::set_stopped;
using std::execution::set_value;
using std::execution::start;

using std::execution::get_completion_scheduler;
using std::execution::get_env;
using std::execution::get_scheduler;
using std::execution::get_stop_token;

using std::execution::receiver_t;
using std::execution::sender_t;

template <typename... Sigs>
using completion_signatures = std::execution::completion_signatures<Sigs...>;

using std::execution::set_error_t;
using std::execution::set_stopped_t;
using std::execution::set_value_t;

#elif defined(LOOM_EXECUTION_BACKEND_STDEXEC)

// Forward stdexec types
namespace impl = stdexec;

template <typename T>
concept sender = stdexec::sender<T>;

template <typename T>
concept receiver = stdexec::receiver<T>;

template <typename T>
concept scheduler = stdexec::scheduler<T>;

template <typename T>
concept operation_state = stdexec::operation_state<T>;

using stdexec::connect;
using stdexec::schedule;
using stdexec::set_error;
using stdexec::set_stopped;
using stdexec::set_value;
using stdexec::start;

using stdexec::get_completion_scheduler;
using stdexec::get_env;
using stdexec::get_scheduler;
using stdexec::get_stop_token;

using sender_t = stdexec::sender_t;
using receiver_t = stdexec::receiver_t;

template <typename... Sigs>
using completion_signatures = stdexec::completion_signatures<Sigs...>;

using set_value_t = stdexec::set_value_t;
using set_error_t = stdexec::set_error_t;
using set_stopped_t = stdexec::set_stopped_t;

// Algorithms
using stdexec::bulk;
using stdexec::continues_on;
using stdexec::just;
using stdexec::just_error;
using stdexec::just_stopped;
using stdexec::let_error;
using stdexec::let_stopped;
using stdexec::let_value;
using stdexec::split;
using stdexec::starts_on;
using stdexec::sync_wait;
using stdexec::then;
using stdexec::upon_error;
using stdexec::upon_stopped;
using stdexec::when_all;

#elif defined(LOOM_EXECUTION_BACKEND_BEMAN)

// Forward beman::execution types
namespace impl = beman::execution;

template <typename T>
concept sender = beman::execution::sender<T>;

template <typename T>
concept receiver = beman::execution::receiver<T>;

template <typename T>
concept scheduler = beman::execution::scheduler<T>;

template <typename T>
concept operation_state = beman::execution::operation_state<T>;

using beman::execution::connect;
using beman::execution::schedule;
using beman::execution::set_error;
using beman::execution::set_stopped;
using beman::execution::set_value;
using beman::execution::start;

using beman::execution::get_completion_scheduler;
using beman::execution::get_env;
using beman::execution::get_scheduler;
using beman::execution::get_stop_token;

using sender_t = beman::execution::sender_t;
using receiver_t = beman::execution::receiver_t;

template <typename... Sigs>
using completion_signatures = beman::execution::completion_signatures<Sigs...>;

using set_value_t = beman::execution::set_value_t;
using set_error_t = beman::execution::set_error_t;
using set_stopped_t = beman::execution::set_stopped_t;

// Algorithms
using beman::execution::bulk;
using beman::execution::continues_on;
using beman::execution::just;
using beman::execution::let_error;
using beman::execution::let_stopped;
using beman::execution::let_value;
using beman::execution::starts_on;
using beman::execution::sync_wait;
using beman::execution::then;
using beman::execution::when_all;

#else

// No backend available - provide stub concepts that always fail
namespace impl {
struct no_execution_backend {};
}  // namespace impl

template <typename T>
concept sender = false;

template <typename T>
concept receiver = false;

template <typename T>
concept scheduler = false;

template <typename T>
concept operation_state = false;

struct sender_t {};
struct receiver_t {};

template <typename... Sigs>
struct completion_signatures {};

struct set_value_t {
    template <typename R, typename... Args>
    void operator()(R&&, Args&&...) const noexcept = delete;
};
struct set_error_t {
    template <typename R, typename E>
    void operator()(R&&, E&&) const noexcept = delete;
};
struct set_stopped_t {
    template <typename R>
    void operator()(R&&) const noexcept = delete;
};

inline constexpr set_value_t set_value{};
inline constexpr set_error_t set_error{};
inline constexpr set_stopped_t set_stopped{};

#endif

/**
 * @brief Check if a P2300 execution backend is available.
 */
inline constexpr bool has_execution_backend =
#if defined(LOOM_EXECUTION_BACKEND_NONE)
    false;
#else
    true;
#endif

/**
 * @brief String describing the active execution backend.
 */
inline constexpr const char* execution_backend_name =
#if defined(LOOM_EXECUTION_BACKEND_STD)
    "std::execution";
#elif defined(LOOM_EXECUTION_BACKEND_STDEXEC)
    "stdexec";
#elif defined(LOOM_EXECUTION_BACKEND_BEMAN)
        "beman::execution";
#else
        "none";
#endif

}  // namespace loom::execution
