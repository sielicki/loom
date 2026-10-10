// SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only
#pragma once

#include <optional>
#include <span>
#include <type_traits>
#include <utility>

#include "loom/core/atomic.hpp"
#include "loom/core/concepts/provider_traits.hpp"
#include "loom/core/memory.hpp"
#include "loom/core/result.hpp"

/**
 * @namespace loom
 * @brief Core namespace for the loom libfabric bindings library.
 */
namespace loom {

/**
 * @namespace loom::detail
 * @brief Implementation details for loom.
 */
namespace detail {

/**
 * @brief Gets the size of an atomic datatype.
 * @tparam T The atomic type.
 * @param dt The datatype (unused, size derived from T).
 * @return The size in bytes.
 */
template <typename T>
[[nodiscard]] constexpr auto datatype_size([[maybe_unused]] atomic::datatype dt) noexcept
    -> std::size_t {
    return sizeof(T);
}

/**
 * @brief Performs a local atomic operation for staged atomic emulation.
 * @tparam T The atomic operand type (must be arithmetic).
 * @param op The atomic operation to perform.
 * @param current The current value.
 * @param operand The operand for the operation.
 * @return The result of the atomic operation.
 */
template <typename T>
    requires std::is_arithmetic_v<T>
[[nodiscard]] auto perform_local_atomic_op(atomic::operation op, T current, T operand) noexcept
    -> T {
    switch (op) {
        case atomic::operation::sum:
            return current + operand;
        case atomic::operation::min:
            return current < operand ? current : operand;
        case atomic::operation::max:
            return current > operand ? current : operand;
        case atomic::operation::prod:
            return current * operand;
        case atomic::operation::bitwise_or:
            if constexpr (std::is_integral_v<T>) {
                return current | operand;
            }
            return current;
        case atomic::operation::bitwise_and:
            if constexpr (std::is_integral_v<T>) {
                return current & operand;
            }
            return current;
        case atomic::operation::bitwise_xor:
            if constexpr (std::is_integral_v<T>) {
                return current ^ operand;
            }
            return current;
        case atomic::operation::logical_or:
            return (current != T{0}) || (operand != T{0}) ? T{1} : T{0};
        case atomic::operation::logical_and:
            return (current != T{0}) && (operand != T{0}) ? T{1} : T{0};
        case atomic::operation::logical_xor:
            return ((current != T{0}) != (operand != T{0})) ? T{1} : T{0};
        case atomic::operation::atomic_write:
            return operand;
        case atomic::operation::atomic_read:
            return current;
        default:
            return current;
    }
}

}  // namespace detail

/**
 * @struct fetch_add_op
 * @brief Atomic fetch-and-add operation descriptor.
 * @tparam T The atomic operand type.
 */
template <atomic::atomic_type T>
struct fetch_add_op {
    endpoint* ep;              ///< Target endpoint
    T operand;                 ///< Value to add
    T* result;                 ///< Pointer to store fetched value
    memory_region* mr_result;  ///< Memory region for result
    remote_memory remote;      ///< Remote memory location
    void* context;             ///< Operation context

    /**
     * @brief Executes the operation using native atomics.
     * @return Success or error result.
     */
    [[nodiscard]] auto execute_native() const -> loom::result<void> {
        return atomic::fetch_add(*ep, operand, result, *mr_result, remote, context);
    }

    /**
     * @brief Submits through the provider atomic interface (legacy staged entry point).
     * @param staging_mr Retained for source compatibility; no application staging is needed.
     * @param staging_buffer Retained for source compatibility.
     * @return Success or error result.
     */
    [[nodiscard]] auto execute_staged([[maybe_unused]] memory_region& staging_mr,
                                      [[maybe_unused]] std::span<std::byte> staging_buffer) const
        -> loom::result<void> {
        return execute_native();
    }
};

/**
 * @struct atomic_add_op
 * @brief Atomic add operation descriptor (no fetch).
 * @tparam T The atomic operand type.
 */
template <atomic::atomic_type T>
struct atomic_add_op {
    endpoint* ep;          ///< Target endpoint
    T operand;             ///< Value to add
    memory_region* mr;     ///< Legacy staging registration (unused)
    remote_memory remote;  ///< Remote memory location
    void* context;         ///< Operation context

    /**
     * @brief Executes the operation using native atomics.
     * @return Success or error result.
     */
    [[nodiscard]] auto execute_native() const -> loom::result<void> {
        return atomic::add(*ep, operand, remote, context);
    }

    /**
     * @brief Submits through the provider atomic interface (legacy staged entry point).
     * @param staging_mr Retained for source compatibility; no application staging is needed.
     * @param staging_buffer Retained for source compatibility.
     * @return Success or error result.
     */
    [[nodiscard]] auto execute_staged([[maybe_unused]] memory_region& staging_mr,
                                      [[maybe_unused]] std::span<std::byte> staging_buffer) const
        -> loom::result<void> {
        return execute_native();
    }
};

/**
 * @struct compare_swap_op
 * @brief Compare-and-swap atomic operation descriptor.
 * @tparam T The atomic operand type.
 */
template <atomic::atomic_type T>
struct compare_swap_op {
    endpoint* ep;           ///< Target endpoint
    T compare_value;        ///< Expected value for comparison
    T swap_value;           ///< New value to write if comparison succeeds
    T* old_value;           ///< Pointer to store original value
    memory_region* mr_old;  ///< Memory region for old value
    remote_memory remote;   ///< Remote memory location
    void* context;          ///< Operation context

    /**
     * @brief Executes the operation using native atomics.
     * @return Success or error result.
     */
    [[nodiscard]] auto execute_native() const -> loom::result<void> {
        return atomic::cas(*ep, compare_value, swap_value, old_value, *mr_old, remote, context);
    }

    /**
     * @brief Submits through the provider atomic interface (legacy staged entry point).
     * @param staging_mr Retained for source compatibility; no application staging is needed.
     * @param staging_buffer Retained for source compatibility.
     * @return Success or error result.
     */
    [[nodiscard]] auto execute_staged([[maybe_unused]] memory_region& staging_mr,
                                      [[maybe_unused]] std::span<std::byte> staging_buffer) const
        -> loom::result<void> {
        return execute_native();
    }
};

/**
 * @struct generic_fetch_op
 * @brief Generic fetch-and-operate atomic operation descriptor.
 * @tparam T The atomic operand type.
 */
template <atomic::atomic_type T>
struct generic_fetch_op {
    endpoint* ep;              ///< Target endpoint
    atomic::operation op;      ///< The atomic operation to perform
    T operand;                 ///< Operand for the operation
    T* result;                 ///< Pointer to store fetched value
    memory_region* mr_result;  ///< Memory region for result
    remote_memory remote;      ///< Remote memory location
    void* context;             ///< Operation context

    /**
     * @brief Executes the operation using native atomics.
     * @return Success or error result.
     */
    [[nodiscard]] auto execute_native() const -> loom::result<void> {
        return atomic::fetch(*ep,
                             op,
                             atomic::get_datatype<T>(),
                             &operand,
                             result,
                             1,
                             nullptr,
                             mr_result,
                             remote,
                             context);
    }

    /**
     * @brief Submits through the provider atomic interface (legacy staged entry point).
     * @param staging_mr Retained for source compatibility; no application staging is needed.
     * @param staging_buffer Retained for source compatibility.
     * @return Success or error result.
     */
    [[nodiscard]] auto execute_staged([[maybe_unused]] memory_region& staging_mr,
                                      [[maybe_unused]] std::span<std::byte> staging_buffer) const
        -> loom::result<void> {
        return execute_native();
    }
};

/**
 * @class provider_atomic_context
 * @brief Provider-aware context for atomic operations.
 *
 * Uses libfabric's atomic interface for every provider. The provider supplies
 * the atomicity guarantees, whether implemented in hardware or software.
 * Query atomic support before submitting operations; unsupported operations
 * return the provider's error without attempting a non-atomic RMA fallback.
 *
 * @tparam P The provider tag type.
 */
template <provider_tag P>
class provider_atomic_context {
public:
    /**
     * @brief Constructs a context with a domain reference.
     * @param dom The fabric domain.
     */
    explicit provider_atomic_context(domain& dom) : domain_(&dom) {}

    /**
     * @brief Constructs a context with domain and staging memory region.
     * @param dom The fabric domain.
     * @param staging_mr Memory region for staging operations.
     */
    provider_atomic_context(domain& dom, memory_region staging_mr)
        : domain_(&dom), staging_mr_(std::move(staging_mr)) {}

    /**
     * @brief Executes an atomic operation using the appropriate method.
     * @tparam Op The operation type (must have execute_native/execute_staged).
     * @param op Borrowed operation descriptor; keep its storage alive until completion.
     * @return Success or error result.
     */
    template <typename Op>
    [[nodiscard]] auto execute(Op&& op) -> loom::result<void> {
        return std::forward<Op>(op).execute_native();
    }

    /**
     * @brief Sets the staging memory region.
     * @param mr The memory region to use for staging.
     */
    auto set_staging_mr(memory_region mr) -> void { staging_mr_ = std::move(mr); }

    /**
     * @brief Checks if a staging memory region is configured.
     * @return True if staging memory region is present.
     */
    [[nodiscard]] auto has_staging_mr() const noexcept -> bool { return staging_mr_.has_value(); }

    /**
     * @brief Checks whether application staging is required (always false).
     * @return False; libfabric manages any provider-side software implementation.
     */
    [[nodiscard]] static constexpr auto requires_staging() noexcept -> bool { return false; }

private:
    domain* domain_;                                        ///< Fabric domain reference
    std::optional<memory_region> staging_mr_;               ///< Optional staging memory region
};

/**
 * @namespace loom::provider_atomic
 * @brief Provider-aware atomic operation functions.
 * @note Operand and result storage must remain alive until asynchronous completion.
 */
namespace provider_atomic {

/**
 * @brief Performs a provider-aware atomic fetch-and-add.
 * @tparam P The provider tag type.
 * @tparam T The atomic operand type.
 * @param ctx The provider atomic context.
 * @param ep The endpoint for the operation.
 * @param operand The value to add.
 * @param result Pointer to store the fetched value.
 * @param mr_result Memory region for the result.
 * @param remote Remote memory location.
 * @param context Optional operation context.
 * @return Success or error result.
 */
template <provider_tag P, atomic::atomic_type T>
[[nodiscard]] auto fetch_add([[maybe_unused]] provider_atomic_context<P>& ctx,
                             endpoint& ep,
                             const T& operand,
                             T* result,
                             memory_region& mr_result,
                             remote_memory remote,
                             void* context = nullptr) -> loom::result<void> {
    return atomic::fetch_add(ep, operand, result, mr_result, remote, context);
}

/**
 * @brief Performs a provider-aware atomic add (no fetch).
 * @tparam P The provider tag type.
 * @tparam T The atomic operand type.
 * @param ctx The provider atomic context.
 * @param ep The endpoint for the operation.
 * @param operand The value to add.
 * @param mr Legacy staging registration (unused).
 * @param remote Remote memory location.
 * @param context Optional operation context.
 * @return Success or error result.
 */
template <provider_tag P, atomic::atomic_type T>
[[nodiscard]] auto add([[maybe_unused]] provider_atomic_context<P>& ctx,
                       endpoint& ep,
                       const T& operand,
                       [[maybe_unused]] memory_region* mr,
                       remote_memory remote,
                       void* context = nullptr) -> loom::result<void> {
    return atomic::add(ep, operand, remote, context);
}

/**
 * @brief Performs a provider-aware compare-and-swap.
 * @tparam P The provider tag type.
 * @tparam T The atomic operand type.
 * @param ctx The provider atomic context.
 * @param ep The endpoint for the operation.
 * @param compare_value Expected value for comparison.
 * @param swap_value New value if comparison succeeds.
 * @param old_value Pointer to store original value.
 * @param mr_old Memory region for old value.
 * @param remote Remote memory location.
 * @param context Optional operation context.
 * @return Success or error result.
 */
template <provider_tag P, atomic::atomic_type T>
[[nodiscard]] auto compare_swap([[maybe_unused]] provider_atomic_context<P>& ctx,
                                endpoint& ep,
                                const T& compare_value,
                                const T& swap_value,
                                T* old_value,
                                memory_region& mr_old,
                                remote_memory remote,
                                void* context = nullptr) -> loom::result<void> {
    return atomic::cas(ep, compare_value, swap_value, old_value, mr_old, remote, context);
}

/**
 * @brief Performs a provider-aware generic fetch-and-operate.
 * @tparam P The provider tag type.
 * @tparam T The atomic operand type.
 * @param ctx The provider atomic context.
 * @param ep The endpoint for the operation.
 * @param op The atomic operation to perform.
 * @param operand The operand for the operation.
 * @param result Pointer to store the fetched value.
 * @param mr_result Memory region for the result.
 * @param remote Remote memory location.
 * @param context Optional operation context.
 * @return Success or error result.
 */
template <provider_tag P, atomic::atomic_type T>
[[nodiscard]] auto fetch_op([[maybe_unused]] provider_atomic_context<P>& ctx,
                            endpoint& ep,
                            atomic::operation op,
                            const T& operand,
                            T* result,
                            memory_region& mr_result,
                            remote_memory remote,
                            void* context = nullptr) -> loom::result<void> {
    return atomic::fetch(ep,
                         op,
                         atomic::get_datatype<T>(),
                         &operand,
                         result,
                         1,
                         nullptr,
                         &mr_result,
                         remote,
                         context);
}

}  // namespace provider_atomic

}  // namespace loom
