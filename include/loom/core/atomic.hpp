// SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only
#pragma once

#include <rdma/fi_atomic.h>

#include <concepts>
#include <span>

#include "loom/core/concepts/strong_types.hpp"
#include "loom/core/endpoint.hpp"
#include "loom/core/error.hpp"
#include "loom/core/memory.hpp"
#include "loom/core/result.hpp"
#include "loom/core/types.hpp"

/**
 * @namespace loom::atomic
 * @brief Atomic remote memory operations.
 */
namespace loom::atomic {

/**
 * @enum operation
 * @brief Atomic operation types for remote memory.
 */
enum class operation {
    min,              ///< Minimum of remote and local value
    max,              ///< Maximum of remote and local value
    sum,              ///< Add local value to remote
    prod,             ///< Multiply remote by local value
    logical_or,       ///< Logical OR operation
    logical_and,      ///< Logical AND operation
    bitwise_or,       ///< Bitwise OR operation
    bitwise_and,      ///< Bitwise AND operation
    logical_xor,      ///< Logical XOR operation
    bitwise_xor,      ///< Bitwise XOR operation
    atomic_read,      ///< Atomic read of remote value
    atomic_write,     ///< Atomic write to remote location
    compare_swap,     ///< Compare and swap (equal)
    compare_swap_ne,  ///< Compare and swap (not equal)
    compare_swap_le,  ///< Compare and swap (less or equal)
    compare_swap_lt,  ///< Compare and swap (less than)
    compare_swap_ge,  ///< Compare and swap (greater or equal)
    compare_swap_gt,  ///< Compare and swap (greater than)
    masked_swap,      ///< Masked swap operation
};

/**
 * @enum datatype
 * @brief Data types for atomic operations.
 */
enum class datatype {
    int8,                 ///< Signed 8-bit integer
    uint8,                ///< Unsigned 8-bit integer
    int16,                ///< Signed 16-bit integer
    uint16,               ///< Unsigned 16-bit integer
    int32,                ///< Signed 32-bit integer
    uint32,               ///< Unsigned 32-bit integer
    int64,                ///< Signed 64-bit integer
    uint64,               ///< Unsigned 64-bit integer
    float32,              ///< 32-bit floating point
    double64,             ///< 64-bit floating point
    float_complex,        ///< Complex float
    double_complex,       ///< Complex double
    long_double,          ///< Long double
    long_double_complex,  ///< Complex long double
    int128,               ///< Signed 128-bit integer
    uint128,              ///< Unsigned 128-bit integer
};

/**
 * @concept atomic_type
 * @brief Concept for types that support atomic operations.
 * @tparam T The type to check.
 */
template <typename T>
concept atomic_type =
    std::is_arithmetic_v<T> || std::is_same_v<T, __int128> || std::is_same_v<T, unsigned __int128>;

/**
 * @brief Gets the datatype enum for an atomic type.
 * @tparam T The C++ type to map to a datatype.
 * @return The corresponding datatype value.
 */
template <typename T>
consteval auto get_datatype() -> datatype {
    if constexpr (std::is_same_v<T, std::int8_t>)
        return datatype::int8;
    else if constexpr (std::is_same_v<T, std::uint8_t>)
        return datatype::uint8;
    else if constexpr (std::is_same_v<T, std::int16_t>)
        return datatype::int16;
    else if constexpr (std::is_same_v<T, std::uint16_t>)
        return datatype::uint16;
    else if constexpr (std::is_same_v<T, std::int32_t>)
        return datatype::int32;
    else if constexpr (std::is_same_v<T, std::uint32_t>)
        return datatype::uint32;
    else if constexpr (std::is_same_v<T, std::int64_t>)
        return datatype::int64;
    else if constexpr (std::is_same_v<T, std::uint64_t>)
        return datatype::uint64;
    else if constexpr (std::is_same_v<T, float>)
        return datatype::float32;
    else if constexpr (std::is_same_v<T, double>)
        return datatype::double64;
    else if constexpr (std::is_same_v<T, __int128>)
        return datatype::int128;
    else if constexpr (std::is_same_v<T, unsigned __int128>)
        return datatype::uint128;
    else
        static_assert(sizeof(T) == 0, "Unsupported atomic datatype");
}

/**
 * @namespace loom::atomic::detail
 * @brief Internal implementation details for atomic operations.
 */
namespace detail {

/**
 * @brief Converts a loom operation to a libfabric fi_op.
 * @param op The loom atomic operation.
 * @return The corresponding fi_op value.
 */
[[nodiscard]] constexpr auto to_fi_op(operation op) noexcept -> ::fi_op {
    switch (op) {
        case operation::min:
            return FI_MIN;
        case operation::max:
            return FI_MAX;
        case operation::sum:
            return FI_SUM;
        case operation::prod:
            return FI_PROD;
        case operation::logical_or:
            return FI_LOR;
        case operation::logical_and:
            return FI_LAND;
        case operation::bitwise_or:
            return FI_BOR;
        case operation::bitwise_and:
            return FI_BAND;
        case operation::logical_xor:
            return FI_LXOR;
        case operation::bitwise_xor:
            return FI_BXOR;
        case operation::atomic_read:
            return FI_ATOMIC_READ;
        case operation::atomic_write:
            return FI_ATOMIC_WRITE;
        case operation::compare_swap:
            return FI_CSWAP;
        case operation::compare_swap_ne:
            return FI_CSWAP_NE;
        case operation::compare_swap_le:
            return FI_CSWAP_LE;
        case operation::compare_swap_lt:
            return FI_CSWAP_LT;
        case operation::compare_swap_ge:
            return FI_CSWAP_GE;
        case operation::compare_swap_gt:
            return FI_CSWAP_GT;
        case operation::masked_swap:
            return FI_MSWAP;
    }
    return FI_MIN;
}

/**
 * @brief Converts a loom datatype to a libfabric fi_datatype.
 * @param dt The loom datatype.
 * @return The corresponding fi_datatype value.
 */
[[nodiscard]] constexpr auto to_fi_datatype(datatype dt) noexcept -> ::fi_datatype {
    switch (dt) {
        case datatype::int8:
            return FI_INT8;
        case datatype::uint8:
            return FI_UINT8;
        case datatype::int16:
            return FI_INT16;
        case datatype::uint16:
            return FI_UINT16;
        case datatype::int32:
            return FI_INT32;
        case datatype::uint32:
            return FI_UINT32;
        case datatype::int64:
            return FI_INT64;
        case datatype::uint64:
            return FI_UINT64;
        case datatype::float32:
            return FI_FLOAT;
        case datatype::double64:
            return FI_DOUBLE;
        case datatype::float_complex:
            return FI_FLOAT_COMPLEX;
        case datatype::double_complex:
            return FI_DOUBLE_COMPLEX;
        case datatype::long_double:
            return FI_LONG_DOUBLE;
        case datatype::long_double_complex:
            return FI_LONG_DOUBLE_COMPLEX;
        case datatype::int128:
            return FI_INT128;
        case datatype::uint128:
            return FI_UINT128;
    }
    return FI_INT8;
}

}  // namespace detail

/**
 * @brief Executes an atomic operation on remote memory.
 * @tparam Ctx The context type.
 * @param ep The endpoint to use.
 * @param op The atomic operation to perform.
 * @param dt The data type of the operand.
 * @param buf Pointer to the local operand buffer.
 * @param count Number of elements.
 * @param mr Memory region for the local buffer (may be null).
 * @param remote Remote memory location.
 * @param context Optional user context.
 * @return Success or error result.
 */
template <context_like Ctx = void*>
auto execute(endpoint& ep,
             operation op,
             datatype dt,
             const void* buf,
             std::size_t count,
             memory_region* mr,
             remote_memory remote,
             Ctx context = {}) -> loom::result<void> {
    auto* fid_ep = static_cast<::fid_ep*>(ep.internal_ptr());
    void* desc = mr ? mr->descriptor().raw() : nullptr;

    auto ret = ::fi_atomic(fid_ep,
                           buf,
                           count,
                           desc,
                           FI_ADDR_UNSPEC,
                           remote.addr,
                           remote.key,
                           detail::to_fi_datatype(dt),
                           detail::to_fi_op(op),
                           to_raw_context(context));

    if (ret != 0) {
        return make_error_result_from_fi_errno<void>(static_cast<int>(-ret));
    }

    return make_success();
}

/**
 * @brief Injects an atomic operation without completion notification.
 * @param ep The endpoint to use.
 * @param op The atomic operation to perform.
 * @param dt The data type of the operand.
 * @param buf Pointer to the local operand buffer.
 * @param count Number of elements.
 * @param remote Remote memory location.
 * @return Success or error result.
 */
auto inject(endpoint& ep,
            operation op,
            datatype dt,
            const void* buf,
            std::size_t count,
            remote_memory remote) -> loom::result<void>;

/**
 * @brief Executes a fetch-atomic operation, returning the original value.
 * @tparam Ctx The context type.
 * @param ep The endpoint to use.
 * @param op The atomic operation to perform.
 * @param dt The data type of the operand.
 * @param buf Pointer to the local operand buffer.
 * @param result Pointer to receive the original remote value.
 * @param count Number of elements.
 * @param mr_buf Memory region for the operand buffer (may be null).
 * @param mr_result Memory region for the result buffer (may be null).
 * @param remote Remote memory location.
 * @param context Optional user context.
 * @return Success or error result.
 */
template <context_like Ctx = void*>
auto fetch(endpoint& ep,
           operation op,
           datatype dt,
           const void* buf,
           void* result,
           std::size_t count,
           memory_region* mr_buf,
           memory_region* mr_result,
           remote_memory remote,
           Ctx context = {}) -> loom::result<void> {
    auto* fid_ep = static_cast<::fid_ep*>(ep.internal_ptr());
    void* desc_buf = mr_buf ? mr_buf->descriptor().raw() : nullptr;
    void* desc_result = mr_result ? mr_result->descriptor().raw() : nullptr;

    auto ret = ::fi_fetch_atomic(fid_ep,
                                 buf,
                                 count,
                                 desc_buf,
                                 result,
                                 desc_result,
                                 FI_ADDR_UNSPEC,
                                 remote.addr,
                                 remote.key,
                                 detail::to_fi_datatype(dt),
                                 detail::to_fi_op(op),
                                 to_raw_context(context));

    if (ret != 0) {
        return make_error_result_from_fi_errno<void>(static_cast<int>(-ret));
    }

    return make_success();
}

/**
 * @brief Executes a compare-and-swap atomic operation.
 * @tparam Ctx The context type.
 * @param ep The endpoint to use.
 * @param dt The data type of the operands.
 * @param compare Pointer to the comparison value.
 * @param swap Pointer to the swap value.
 * @param result Pointer to receive the original remote value.
 * @param count Number of elements.
 * @param mr_compare Memory region for the compare buffer (may be null).
 * @param mr_result Memory region for the result buffer (may be null).
 * @param remote Remote memory location.
 * @param context Optional user context.
 * @return Success or error result.
 */
template <context_like Ctx = void*>
auto compare_swap(endpoint& ep,
                  datatype dt,
                  const void* compare,
                  const void* swap,
                  void* result,
                  std::size_t count,
                  memory_region* mr_compare,
                  memory_region* mr_result,
                  remote_memory remote,
                  Ctx context = {}) -> loom::result<void> {
    auto* fid_ep = static_cast<::fid_ep*>(ep.internal_ptr());
    void* desc_compare = mr_compare ? mr_compare->descriptor().raw() : nullptr;
    void* desc_result = mr_result ? mr_result->descriptor().raw() : nullptr;

    auto ret = ::fi_compare_atomic(fid_ep,
                                   compare,
                                   count,
                                   desc_compare,
                                   swap,
                                   nullptr,
                                   result,
                                   desc_result,
                                   FI_ADDR_UNSPEC,
                                   remote.addr,
                                   remote.key,
                                   detail::to_fi_datatype(dt),
                                   FI_CSWAP,
                                   to_raw_context(context));

    if (ret != 0) {
        return make_error_result_from_fi_errno<void>(static_cast<int>(-ret));
    }

    return make_success();
}

/**
 * @brief Atomically adds a value to remote memory.
 * @tparam T The atomic data type.
 * @tparam Ctx The context type.
 * @param ep The endpoint to use.
 * @param value The value to add.
 * @param remote Remote memory location.
 * @param context Optional user context.
 * @return Success or error result.
 */
template <atomic_type T, context_like Ctx = void*>
auto add(endpoint& ep, const T& value, remote_memory remote, Ctx context = {})
    -> loom::result<void> {
    return execute(ep, operation::sum, get_datatype<T>(), &value, 1, nullptr, remote, context);
}

/**
 * @brief Atomically adds a value and fetches the original.
 * @tparam T The atomic data type.
 * @tparam Ctx The context type.
 * @param ep The endpoint to use.
 * @param value The value to add.
 * @param result Pointer to receive the original remote value.
 * @param mr_result Memory region for the result buffer.
 * @param remote Remote memory location.
 * @param context Optional user context.
 * @return Success or error result.
 */
template <atomic_type T, context_like Ctx = void*>
auto fetch_add(endpoint& ep,
               const T& value,
               T* result,
               memory_region& mr_result,
               remote_memory remote,
               Ctx context = {}) -> loom::result<void> {
    return fetch(ep,
                 operation::sum,
                 get_datatype<T>(),
                 &value,
                 result,
                 1,
                 nullptr,
                 &mr_result,
                 remote,
                 context);
}

/**
 * @brief Typed compare-and-swap operation.
 * @tparam T The atomic data type.
 * @tparam Ctx The context type.
 * @param ep The endpoint to use.
 * @param compare_value The expected value to compare against.
 * @param swap_value The new value to store if comparison succeeds.
 * @param old_value Pointer to receive the original remote value.
 * @param mr_old Memory region for the result buffer.
 * @param remote Remote memory location.
 * @param context Optional user context.
 * @return Success or error result.
 */
template <atomic_type T, context_like Ctx = void*>
auto cas(endpoint& ep,
         const T& compare_value,
         const T& swap_value,
         T* old_value,
         memory_region& mr_old,
         remote_memory remote,
         Ctx context = {}) -> loom::result<void> {
    return compare_swap(ep,
                        get_datatype<T>(),
                        &compare_value,
                        &swap_value,
                        old_value,
                        1,
                        nullptr,
                        &mr_old,
                        remote,
                        context);
}

/**
 * @brief Checks if an atomic operation is valid for the endpoint.
 * @param ep The endpoint to query.
 * @param op The atomic operation to check.
 * @param dt The data type to check.
 * @param count_out Optional pointer to receive the max element count.
 * @return True if the operation is valid.
 */
[[nodiscard]] auto
is_valid(endpoint& ep, operation op, datatype dt, std::size_t* count_out = nullptr) -> bool;

/**
 * @brief Checks if a fetch-atomic operation is valid for the endpoint.
 * @param ep The endpoint to query.
 * @param op The atomic operation to check.
 * @param dt The data type to check.
 * @param count_out Optional pointer to receive the max element count.
 * @return True if the operation is valid.
 */
[[nodiscard]] auto
is_fetch_valid(endpoint& ep, operation op, datatype dt, std::size_t* count_out = nullptr) -> bool;

/**
 * @brief Checks if a compare-atomic operation is valid for the endpoint.
 * @param ep The endpoint to query.
 * @param op The atomic operation to check.
 * @param dt The data type to check.
 * @param count_out Optional pointer to receive the max element count.
 * @return True if the operation is valid.
 */
[[nodiscard]] auto
is_compare_valid(endpoint& ep, operation op, datatype dt, std::size_t* count_out = nullptr) -> bool;

}  // namespace loom::atomic
