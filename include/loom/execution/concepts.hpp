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
 * - std::execution (C++26 standard)
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
 * 2. stdexec (if <stdexec/execution.hpp> exists)
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
#if defined(__cpp_lib_senders) && __cpp_lib_senders >= 202406L
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

// An explicit selection must never silently choose another implementation.
#if (defined(LOOM_EXECUTION_USE_STD) + defined(LOOM_EXECUTION_USE_STDEXEC) +                       \
     defined(LOOM_EXECUTION_USE_BEMAN)) > 1
#    error "Select only one loom execution backend"
#endif

#if defined(LOOM_EXECUTION_USE_STD)
#    if !LOOM_HAS_STD_EXECUTION
#        error "Selected std::execution requires __cpp_lib_senders >= 202406L"
#    endif
#    define LOOM_EXECUTION_BACKEND_STD 1
#elif defined(LOOM_EXECUTION_USE_STDEXEC)
#    if !LOOM_HAS_STDEXEC
#        error "Selected stdexec backend requires <stdexec/execution.hpp>"
#    endif
#    define LOOM_EXECUTION_BACKEND_STDEXEC 1
#elif defined(LOOM_EXECUTION_USE_BEMAN)
#    if !LOOM_HAS_BEMAN_EXECUTION
#        error "Selected Beman backend requires <beman/execution/execution.hpp>"
#    endif
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
namespace impl = std::execution;
#elif defined(LOOM_EXECUTION_BACKEND_STDEXEC)
namespace impl = stdexec;
#elif defined(LOOM_EXECUTION_BACKEND_BEMAN)
namespace impl = beman::execution;
#endif

#if !defined(LOOM_EXECUTION_BACKEND_NONE)

// The pinned stdexec predates the C++26 concept-tag renames (P4154).
#    if defined(LOOM_EXECUTION_BACKEND_STDEXEC)
using sender_tag = impl::sender_t;
using receiver_tag = impl::receiver_t;
using scheduler_tag = impl::scheduler_t;
using operation_state_tag = impl::operation_state_t;
#    else
using impl::operation_state_tag;
using impl::receiver_tag;
using impl::scheduler_tag;
using impl::sender_tag;
#    endif

// Preserve the original loom aliases while using standard tag names internally.
using sender_t = sender_tag;
using receiver_t = receiver_tag;

// Keep the public surface identical across P2300 implementations.
using impl::bulk;
using impl::completion_signatures;
using impl::connect;
using impl::continues_on;
using impl::forward_progress_guarantee;
using impl::get_completion_scheduler;
using impl::get_env;
using impl::get_forward_progress_guarantee;
using impl::get_scheduler;
using impl::just;
using impl::just_error;
using impl::just_stopped;
using impl::let_error;
using impl::let_stopped;
using impl::let_value;
using impl::operation_state;
using impl::receiver;
using impl::schedule;
using impl::scheduler;
using impl::sender;
using impl::set_error;
using impl::set_error_t;
using impl::set_stopped;
using impl::set_stopped_t;
using impl::set_value;
using impl::set_value_t;
using impl::start;
using impl::starts_on;
using impl::then;
using impl::upon_error;
using impl::upon_stopped;
using impl::when_all;

#    if defined(LOOM_EXECUTION_BACKEND_STD)
using std::get_stop_token;
using std::this_thread::sync_wait;
#    else
using impl::get_stop_token;
using impl::sync_wait;
#    endif

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
