// SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only
#pragma once

#include <cstring>
#include <span>
#include <type_traits>

#include "loom/core/atomic.hpp"
#include "loom/core/concepts/provider_traits.hpp"
#include "loom/core/memory.hpp"
#include "loom/core/result.hpp"
#include "loom/core/rma.hpp"

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
     * @brief Executes the operation using staged RMA emulation.
     * @param staging_mr Memory region for staging buffer.
     * @param staging_buffer Staging buffer for read-modify-write.
     * @return Success or error result.
     */
    [[nodiscard]] auto execute_staged(memory_region& staging_mr,
                                      std::span<std::byte> staging_buffer) const
        -> loom::result<void> {
        static_assert(sizeof(T) <= 64, "Staging buffer must fit atomic type");

        auto read_result =
            rma::read(*ep, staging_buffer.subspan(0, sizeof(T)), staging_mr, remote, nullptr);
        if (!read_result) {
            return read_result;
        }

        T current_value{};
        std::memcpy(&current_value, staging_buffer.data(), sizeof(T));

        if (result != nullptr) {
            *result = current_value;
        }

        T new_value = current_value + operand;
        std::memcpy(staging_buffer.data(), &new_value, sizeof(T));

        return rma::write(*ep,
                          std::span<const std::byte>(staging_buffer.data(), sizeof(T)),
                          staging_mr,
                          remote,
                          context);
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
    memory_region* mr;     ///< Memory region (for staged operations)
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
     * @brief Executes the operation using staged RMA emulation.
     * @param staging_mr Memory region for staging buffer.
     * @param staging_buffer Staging buffer for read-modify-write.
     * @return Success or error result.
     */
    [[nodiscard]] auto execute_staged(memory_region& staging_mr,
                                      std::span<std::byte> staging_buffer) const
        -> loom::result<void> {
        auto read_result =
            rma::read(*ep, staging_buffer.subspan(0, sizeof(T)), staging_mr, remote, nullptr);
        if (!read_result) {
            return read_result;
        }

        T current_value{};
        std::memcpy(&current_value, staging_buffer.data(), sizeof(T));

        T new_value = current_value + operand;
        std::memcpy(staging_buffer.data(), &new_value, sizeof(T));

        return rma::write(*ep,
                          std::span<const std::byte>(staging_buffer.data(), sizeof(T)),
                          staging_mr,
                          remote,
                          context);
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
     * @brief Executes the operation using staged RMA emulation.
     * @param staging_mr Memory region for staging buffer.
     * @param staging_buffer Staging buffer for read-modify-write.
     * @return Success or error result.
     */
    [[nodiscard]] auto execute_staged(memory_region& staging_mr,
                                      std::span<std::byte> staging_buffer) const
        -> loom::result<void> {
        auto read_result =
            rma::read(*ep, staging_buffer.subspan(0, sizeof(T)), staging_mr, remote, nullptr);
        if (!read_result) {
            return read_result;
        }

        T current_value{};
        std::memcpy(&current_value, staging_buffer.data(), sizeof(T));

        if (old_value != nullptr) {
            *old_value = current_value;
        }

        if (current_value == compare_value) {
            std::memcpy(staging_buffer.data(), &swap_value, sizeof(T));
            return rma::write(*ep,
                              std::span<const std::byte>(staging_buffer.data(), sizeof(T)),
                              staging_mr,
                              remote,
                              context);
        }

        return make_success();
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
     * @brief Executes the operation using staged RMA emulation.
     * @param staging_mr Memory region for staging buffer.
     * @param staging_buffer Staging buffer for read-modify-write.
     * @return Success or error result.
     */
    [[nodiscard]] auto execute_staged(memory_region& staging_mr,
                                      std::span<std::byte> staging_buffer) const
        -> loom::result<void> {
        auto read_result =
            rma::read(*ep, staging_buffer.subspan(0, sizeof(T)), staging_mr, remote, nullptr);
        if (!read_result) {
            return read_result;
        }

        T current_value{};
        std::memcpy(&current_value, staging_buffer.data(), sizeof(T));

        if (result != nullptr) {
            *result = current_value;
        }

        T new_value = detail::perform_local_atomic_op(op, current_value, operand);
        std::memcpy(staging_buffer.data(), &new_value, sizeof(T));

        return rma::write(*ep,
                          std::span<const std::byte>(staging_buffer.data(), sizeof(T)),
                          staging_mr,
                          remote,
                          context);
    }
};

/**
 * @class provider_atomic_context
 * @brief Provider-aware context for atomic operations.
 *
 * Automatically selects between native and staged atomic implementations
 * based on provider capabilities.
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
     * @param op The operation to execute.
     * @return Success or error result.
     */
    template <typename Op>
    [[nodiscard]] auto execute(Op&& op) -> loom::result<void> {
        if constexpr (native_atomic_provider<P>) {
            return std::forward<Op>(op).execute_native();
        } else if constexpr (staged_atomic_provider<P>) {
            if (!staging_mr_) {
                return make_error_result<void>(errc::not_supported);
            }
            return std::forward<Op>(op).execute_staged(
                *staging_mr_, std::span<std::byte>(staging_buffer_.data(), staging_buffer_.size()));
        } else {
            static_assert(native_atomic_provider<P> || staged_atomic_provider<P>,
                          "Provider must support either native or staged atomics");
        }
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
     * @brief Checks if this provider requires staged atomics.
     * @return True if the provider uses staged atomics.
     */
    [[nodiscard]] static constexpr auto requires_staging() noexcept -> bool {
        return staged_atomic_provider<P>;
    }

private:
    domain* domain_;                                        ///< Fabric domain reference
    std::optional<memory_region> staging_mr_;               ///< Optional staging memory region
    alignas(64) std::array<std::byte, 64> staging_buffer_;  ///< Cache-aligned staging buffer
};

/**
 * @namespace loom::provider_atomic
 * @brief Provider-aware atomic operation functions.
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
[[nodiscard]] auto fetch_add(provider_atomic_context<P>& ctx,
                             endpoint& ep,
                             const T& operand,
                             T* result,
                             memory_region& mr_result,
                             remote_memory remote,
                             void* context = nullptr) -> loom::result<void> {
    return ctx.execute(fetch_add_op<T>{&ep, operand, result, &mr_result, remote, context});
}

/**
 * @brief Performs a provider-aware atomic add (no fetch).
 * @tparam P The provider tag type.
 * @tparam T The atomic operand type.
 * @param ctx The provider atomic context.
 * @param ep The endpoint for the operation.
 * @param operand The value to add.
 * @param mr Memory region (for staged operations).
 * @param remote Remote memory location.
 * @param context Optional operation context.
 * @return Success or error result.
 */
template <provider_tag P, atomic::atomic_type T>
[[nodiscard]] auto add(provider_atomic_context<P>& ctx,
                       endpoint& ep,
                       const T& operand,
                       memory_region* mr,
                       remote_memory remote,
                       void* context = nullptr) -> loom::result<void> {
    return ctx.execute(atomic_add_op<T>{&ep, operand, mr, remote, context});
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
[[nodiscard]] auto compare_swap(provider_atomic_context<P>& ctx,
                                endpoint& ep,
                                const T& compare_value,
                                const T& swap_value,
                                T* old_value,
                                memory_region& mr_old,
                                remote_memory remote,
                                void* context = nullptr) -> loom::result<void> {
    return ctx.execute(
        compare_swap_op<T>{&ep, compare_value, swap_value, old_value, &mr_old, remote, context});
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
[[nodiscard]] auto fetch_op(provider_atomic_context<P>& ctx,
                            endpoint& ep,
                            atomic::operation op,
                            const T& operand,
                            T* result,
                            memory_region& mr_result,
                            remote_memory remote,
                            void* context = nullptr) -> loom::result<void> {
    return ctx.execute(generic_fetch_op<T>{&ep, op, operand, result, &mr_result, remote, context});
}

}  // namespace provider_atomic

}  // namespace loom
