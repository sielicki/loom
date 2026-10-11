// SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only
#pragma once

#include <rdma/fi_atomic.h>

#include <concepts>
#include <span>
#include <type_traits>

#include "loom/core/concepts/strong_types.hpp"
#include "loom/core/endpoint.hpp"
#include "loom/core/error.hpp"
#include "loom/core/memory.hpp"
#include "loom/core/result.hpp"
#include "loom/core/types.hpp"
#include "loom/detail/conversions.hpp"

/**
 * @namespace loom::atomic
 * @brief Atomic remote memory operations.
 *
 * Ordinary submissions borrow operands, results, registrations, and context
 * until completion. Keep referenced values alive, including typed operands;
 * passing a temporary does not extend its lifetime. Injection releases only
 * its input on return. Query operation/type/count support before submission.
 * Element counts must fit the remote byte extent without address overflow.
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

// Check for __int128 support (GCC/Clang extension)
#if defined(__SIZEOF_INT128__)
#    define LOOM_HAS_INT128 1
// Suppress GCC's pedantic warning about __int128 not being ISO C++
#    pragma GCC diagnostic push
#    pragma GCC diagnostic ignored "-Wpedantic"
using int128_type = __int128;
using uint128_type = unsigned __int128;
#    pragma GCC diagnostic pop
#else
#    define LOOM_HAS_INT128 0
// Provide stub types when __int128 is not available
struct int128_type {};
struct uint128_type {};
#endif

namespace detail {
template <typename T>
struct datatype_traits {};

template <>
struct datatype_traits<std::int8_t> : std::integral_constant<datatype, datatype::int8> {};
template <>
struct datatype_traits<std::uint8_t> : std::integral_constant<datatype, datatype::uint8> {};
template <>
struct datatype_traits<std::int16_t> : std::integral_constant<datatype, datatype::int16> {};
template <>
struct datatype_traits<std::uint16_t> : std::integral_constant<datatype, datatype::uint16> {};
template <>
struct datatype_traits<std::int32_t> : std::integral_constant<datatype, datatype::int32> {};
template <>
struct datatype_traits<std::uint32_t> : std::integral_constant<datatype, datatype::uint32> {};
template <>
struct datatype_traits<std::int64_t> : std::integral_constant<datatype, datatype::int64> {};
template <>
struct datatype_traits<std::uint64_t> : std::integral_constant<datatype, datatype::uint64> {};
template <>
struct datatype_traits<float> : std::integral_constant<datatype, datatype::float32> {};
template <>
struct datatype_traits<double> : std::integral_constant<datatype, datatype::double64> {};
template <>
struct datatype_traits<long double> : std::integral_constant<datatype, datatype::long_double> {};
#if LOOM_HAS_INT128
template <>
struct datatype_traits<int128_type> : std::integral_constant<datatype, datatype::int128> {};
template <>
struct datatype_traits<uint128_type> : std::integral_constant<datatype, datatype::uint128> {};
#endif
}  // namespace detail

/**
 * @concept atomic_type
 * @brief Unqualified value types with an atomic datatype mapping.
 * Provider support for each operation, datatype, and count still requires a query.
 * @tparam T The type to check.
 */
template <typename T>
concept atomic_type = requires { detail::datatype_traits<T>::value; };

/**
 * @brief Gets the datatype enum for an atomic type.
 * @tparam T The C++ type to map to a datatype.
 * @return The corresponding datatype value.
 */
template <atomic_type T>
consteval auto get_datatype() noexcept -> datatype {
    return detail::datatype_traits<T>::value;
}

/**
 * @namespace loom::atomic::detail
 * @brief Internal implementation details for atomic operations.
 */
namespace detail {

[[nodiscard]] constexpr auto datatype_size(datatype type) noexcept -> std::size_t {
    switch (type) {
        case datatype::int8:
        case datatype::uint8:
            return sizeof(std::uint8_t);
        case datatype::int16:
        case datatype::uint16:
            return sizeof(std::uint16_t);
        case datatype::int32:
        case datatype::uint32:
            return sizeof(std::uint32_t);
        case datatype::int64:
        case datatype::uint64:
            return sizeof(std::uint64_t);
        case datatype::float32:
            return sizeof(float);
        case datatype::double64:
            return sizeof(double);
        case datatype::long_double:
            return sizeof(long double);
        case datatype::float_complex:
            return 2 * sizeof(float);
        case datatype::double_complex:
            return 2 * sizeof(double);
        case datatype::long_double_complex:
            return 2 * sizeof(long double);
        case datatype::int128:
        case datatype::uint128:
            return 2 * sizeof(std::uint64_t);
    }
    return 0;
}

[[nodiscard]] constexpr auto
contains_elements(remote_memory remote, datatype type, std::size_t count) noexcept -> bool {
    const auto size = datatype_size(type);
    return size != 0 && count <= remote.length / size && remote.contains(0, count * size);
}

/// Validate supplied registrations without imposing registration on other operands.
[[nodiscard]] inline auto registered_elements(const endpoint& ep,
                                              const memory_region* region,
                                              const void* address,
                                              datatype type,
                                              std::size_t count) noexcept -> bool {
    if (!region)
        return true;
    const auto size = datatype_size(type);
    return size != 0 && region->usable_with(ep) && count <= region->length() / size &&
           region->contains(address, count * size);
}

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
 * @param dest_addr Destination peer (ignored by connected endpoints).
 * @param remote Remote memory location.
 * @param context Optional user context.
 * @return Success or error result.
 */
/// Explicit flags use the message interface; inheritance extends creation defaults.
/// FI_INJECT releases constant inputs on return, while results remain borrowed.
template <operation_defaults Defaults = operation_defaults::replace, context_like Ctx = void*>
auto execute_to(endpoint& ep,
                operation op,
                datatype dt,
                const void* buf,
                std::size_t count,
                memory_region* mr,
                fabric_addr dest_addr,
                remote_memory remote,
                op_flags flags,
                Ctx context = {}) -> loom::result<void> {
    auto* fid_ep = static_cast<::fid_ep*>(ep.internal_ptr());
    if (!fid_ep || !detail::contains_elements(remote, dt, count) ||
        !detail::registered_elements(ep, mr, buf, dt, count))
        return make_error_result<void>(errc::invalid_argument);
    if (!FI_CHECK_OP(fid_ep->atomic, fi_ops_atomic, writemsg))
        return make_error_result<void>(errc::not_supported);
    void* input_desc = mr ? mr->descriptor().raw() : nullptr;
    const ::fi_ioc input{const_cast<void*>(buf), count};
    const ::fi_rma_ioc target{remote.addr, count, remote.key};
    const ::fi_msg_atomic message{
        .msg_iov = &input,
        .desc = input_desc ? &input_desc : nullptr,
        .iov_count = 1,
        .addr = dest_addr.get(),
        .rma_iov = &target,
        .rma_iov_count = 1,
        .datatype = detail::to_fi_datatype(dt),
        .op = detail::to_fi_op(op),
        .context = to_raw_context(context),
    };
    const auto native_flags = loom::detail::operation_flags<Defaults>(
        ep, completion_direction::transmit, loom::detail::to_fi_op_flags(flags));
    const auto status = ::fi_atomicmsg(fid_ep, &message, native_flags);
    if (status)
        return make_error_result_from_fi_errno<void>(static_cast<int>(-status));
    return make_success();
}

template <context_like Ctx = void*>
auto execute_to(endpoint& ep,
                operation op,
                datatype dt,
                const void* buf,
                std::size_t count,
                memory_region* mr,
                fabric_addr dest_addr,
                remote_memory remote,
                Ctx context = {}) -> loom::result<void> {
    auto* fid_ep = static_cast<::fid_ep*>(ep.internal_ptr());
    if (!fid_ep || !detail::contains_elements(remote, dt, count) ||
        !detail::registered_elements(ep, mr, buf, dt, count)) {
        return make_error_result<void>(errc::invalid_argument);
    }
    if (!FI_CHECK_OP(fid_ep->atomic, fi_ops_atomic, write)) {
        return make_error_result<void>(errc::not_supported);
    }
    void* desc = mr ? mr->descriptor().raw() : nullptr;

    auto ret = ::fi_atomic(fid_ep,
                           buf,
                           count,
                           desc,
                           dest_addr.get(),
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

/// Connected-endpoint convenience; buffers remain borrowed until completion.
template <context_like Ctx = void*>
auto execute(endpoint& ep,
             operation op,
             datatype dt,
             const void* buf,
             std::size_t count,
             memory_region* mr,
             remote_memory remote,
             Ctx context = {}) -> loom::result<void> {
    return execute_to(ep, op, dt, buf, count, mr, fabric_addrs::unspecified, remote, context);
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

/// Inject an atomic operand to an explicit peer, releasing the input on return.
auto inject_to(endpoint& ep,
               operation op,
               datatype dt,
               const void* buf,
               std::size_t count,
               fabric_addr dest_addr,
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
 * @param dest_addr Destination peer (ignored by connected endpoints).
 * @param remote Remote memory location.
 * @param context Optional user context.
 * @return Success or error result.
 */
/// Explicit flags use the message interface; inheritance extends creation defaults.
/// FI_INJECT releases constant inputs on return, while results remain borrowed.
template <operation_defaults Defaults = operation_defaults::replace, context_like Ctx = void*>
auto fetch_to(endpoint& ep,
              operation op,
              datatype dt,
              const void* buf,
              void* result,
              std::size_t count,
              const memory_region* mr_buf,
              const memory_region* mr_result,
              fabric_addr dest_addr,
              remote_memory remote,
              op_flags flags,
              Ctx context = {}) -> loom::result<void> {
    auto* fid_ep = static_cast<::fid_ep*>(ep.internal_ptr());
    if (!fid_ep || !detail::contains_elements(remote, dt, count) ||
        (op != operation::atomic_read &&
         !detail::registered_elements(ep, mr_buf, buf, dt, count)) ||
        !detail::registered_elements(ep, mr_result, result, dt, count))
        return make_error_result<void>(errc::invalid_argument);
    if (!FI_CHECK_OP(fid_ep->atomic, fi_ops_atomic, readwritemsg))
        return make_error_result<void>(errc::not_supported);
    void* input_desc = mr_buf ? mr_buf->descriptor().raw() : nullptr;
    void* result_desc = mr_result ? mr_result->descriptor().raw() : nullptr;
    const ::fi_ioc input{const_cast<void*>(buf), count};
    const ::fi_rma_ioc target{remote.addr, count, remote.key};
    ::fi_ioc output{result, count};
    const ::fi_msg_atomic message{
        .msg_iov = &input,
        .desc = input_desc ? &input_desc : nullptr,
        .iov_count = 1,
        .addr = dest_addr.get(),
        .rma_iov = &target,
        .rma_iov_count = 1,
        .datatype = detail::to_fi_datatype(dt),
        .op = detail::to_fi_op(op),
        .context = to_raw_context(context),
    };
    const auto native_flags = loom::detail::operation_flags<Defaults>(
        ep, completion_direction::transmit, loom::detail::to_fi_op_flags(flags));
    const auto status = ::fi_fetch_atomicmsg(
        fid_ep, &message, &output, result_desc ? &result_desc : nullptr, 1, native_flags);
    if (status)
        return make_error_result_from_fi_errno<void>(static_cast<int>(-status));
    return make_success();
}

template <context_like Ctx = void*>
auto fetch_to(endpoint& ep,
              operation op,
              datatype dt,
              const void* buf,
              void* result,
              std::size_t count,
              const memory_region* mr_buf,
              const memory_region* mr_result,
              fabric_addr dest_addr,
              remote_memory remote,
              Ctx context = {}) -> loom::result<void> {
    auto* fid_ep = static_cast<::fid_ep*>(ep.internal_ptr());
    if (!fid_ep || !detail::contains_elements(remote, dt, count) ||
        (op != operation::atomic_read &&
         !detail::registered_elements(ep, mr_buf, buf, dt, count)) ||
        !detail::registered_elements(ep, mr_result, result, dt, count)) {
        return make_error_result<void>(errc::invalid_argument);
    }
    if (!FI_CHECK_OP(fid_ep->atomic, fi_ops_atomic, readwrite)) {
        return make_error_result<void>(errc::not_supported);
    }
    void* desc_buf = mr_buf ? mr_buf->descriptor().raw() : nullptr;
    void* desc_result = mr_result ? mr_result->descriptor().raw() : nullptr;

    auto ret = ::fi_fetch_atomic(fid_ep,
                                 buf,
                                 count,
                                 desc_buf,
                                 result,
                                 desc_result,
                                 dest_addr.get(),
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

/// Connected-endpoint convenience; operands and result are borrowed until completion.
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
    return fetch_to(ep,
                    op,
                    dt,
                    buf,
                    result,
                    count,
                    mr_buf,
                    mr_result,
                    fabric_addrs::unspecified,
                    remote,
                    context);
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
 * @param mr_swap Memory region for the replacement buffer (may be null).
 * @param mr_result Memory region for the result buffer (may be null).
 * @param dest_addr Destination peer (ignored by connected endpoints).
 * @param remote Remote memory location.
 * @param context Optional user context.
 * @return Success or error result.
 */
/// Explicit flags use the message interface; inheritance extends creation defaults.
/// FI_INJECT releases constant inputs on return, while results remain borrowed.
template <operation_defaults Defaults = operation_defaults::replace, context_like Ctx = void*>
auto compare_swap_to(endpoint& ep,
                     datatype dt,
                     const void* compare,
                     const void* swap,
                     void* result,
                     std::size_t count,
                     const memory_region* mr_compare,
                     const memory_region* mr_swap,
                     const memory_region* mr_result,
                     fabric_addr dest_addr,
                     remote_memory remote,
                     op_flags flags,
                     Ctx context = {}) -> loom::result<void> {
    auto* fid_ep = static_cast<::fid_ep*>(ep.internal_ptr());
    if (!fid_ep || !detail::contains_elements(remote, dt, count) ||
        !detail::registered_elements(ep, mr_compare, compare, dt, count) ||
        !detail::registered_elements(ep, mr_swap, swap, dt, count) ||
        !detail::registered_elements(ep, mr_result, result, dt, count))
        return make_error_result<void>(errc::invalid_argument);
    if (!FI_CHECK_OP(fid_ep->atomic, fi_ops_atomic, compwritemsg))
        return make_error_result<void>(errc::not_supported);
    void* input_desc = mr_swap ? mr_swap->descriptor().raw() : nullptr;
    void* compare_desc = mr_compare ? mr_compare->descriptor().raw() : nullptr;
    void* result_desc = mr_result ? mr_result->descriptor().raw() : nullptr;
    const ::fi_ioc input{const_cast<void*>(swap), count};
    const ::fi_rma_ioc target{remote.addr, count, remote.key};
    const ::fi_ioc comparison{const_cast<void*>(compare), count};
    ::fi_ioc output{result, count};
    const ::fi_msg_atomic message{
        .msg_iov = &input,
        .desc = input_desc ? &input_desc : nullptr,
        .iov_count = 1,
        .addr = dest_addr.get(),
        .rma_iov = &target,
        .rma_iov_count = 1,
        .datatype = detail::to_fi_datatype(dt),
        .op = FI_CSWAP,
        .context = to_raw_context(context),
    };
    const auto native_flags = loom::detail::operation_flags<Defaults>(
        ep, completion_direction::transmit, loom::detail::to_fi_op_flags(flags));
    const auto status = ::fi_compare_atomicmsg(fid_ep,
                                               &message,
                                               &comparison,
                                               compare_desc ? &compare_desc : nullptr,
                                               1,
                                               &output,
                                               result_desc ? &result_desc : nullptr,
                                               1,
                                               native_flags);
    if (status)
        return make_error_result_from_fi_errno<void>(static_cast<int>(-status));
    return make_success();
}

template <context_like Ctx = void*>
auto compare_swap_to(endpoint& ep,
                     datatype dt,
                     const void* compare,
                     const void* swap,
                     void* result,
                     std::size_t count,
                     const memory_region* mr_compare,
                     const memory_region* mr_swap,
                     const memory_region* mr_result,
                     fabric_addr dest_addr,
                     remote_memory remote,
                     Ctx context = {}) -> loom::result<void> {
    auto* fid_ep = static_cast<::fid_ep*>(ep.internal_ptr());
    if (!fid_ep || !detail::contains_elements(remote, dt, count) ||
        !detail::registered_elements(ep, mr_compare, compare, dt, count) ||
        !detail::registered_elements(ep, mr_swap, swap, dt, count) ||
        !detail::registered_elements(ep, mr_result, result, dt, count)) {
        return make_error_result<void>(errc::invalid_argument);
    }
    if (!FI_CHECK_OP(fid_ep->atomic, fi_ops_atomic, compwrite)) {
        return make_error_result<void>(errc::not_supported);
    }
    void* desc_compare = mr_compare ? mr_compare->descriptor().raw() : nullptr;
    void* desc_swap = mr_swap ? mr_swap->descriptor().raw() : nullptr;
    void* desc_result = mr_result ? mr_result->descriptor().raw() : nullptr;

    auto ret = ::fi_compare_atomic(fid_ep,
                                   swap,
                                   count,
                                   desc_swap,
                                   compare,
                                   desc_compare,
                                   result,
                                   desc_result,
                                   dest_addr.get(),
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

/// Connected-endpoint convenience for replacement operands needing no local descriptor.
/// Use compare_swap_to when the replacement operand requires registration.
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
    return compare_swap_to(ep,
                           dt,
                           compare,
                           swap,
                           result,
                           count,
                           mr_compare,
                           nullptr,
                           mr_result,
                           fabric_addrs::unspecified,
                           remote,
                           context);
}

/// Typed addition with an explicit peer and optional operand registration.
template <atomic_type T, context_like Ctx = void*>
auto add_to(endpoint& ep,
            const T& value,
            memory_region* mr_value,
            fabric_addr dest_addr,
            remote_memory remote,
            Ctx context = {}) -> loom::result<void> {
    return execute_to(
        ep, operation::sum, get_datatype<T>(), &value, 1, mr_value, dest_addr, remote, context);
}

/// Typed fetch-add; both the operand and result remain borrowed until completion.
template <atomic_type T, context_like Ctx = void*>
auto fetch_add_to(endpoint& ep,
                  const T& value,
                  T* result,
                  memory_region* mr_value,
                  memory_region* mr_result,
                  fabric_addr dest_addr,
                  remote_memory remote,
                  Ctx context = {}) -> loom::result<void> {
    return fetch_to(ep,
                    operation::sum,
                    get_datatype<T>(),
                    &value,
                    result,
                    1,
                    mr_value,
                    mr_result,
                    dest_addr,
                    remote,
                    context);
}

/// Typed compare-and-swap with independent registrations for all three buffers.
template <atomic_type T, context_like Ctx = void*>
auto cas_to(endpoint& ep,
            const T& compare_value,
            const T& swap_value,
            T* old_value,
            memory_region* mr_compare,
            memory_region* mr_swap,
            memory_region* mr_old,
            fabric_addr dest_addr,
            remote_memory remote,
            Ctx context = {}) -> loom::result<void> {
    return compare_swap_to(ep,
                           get_datatype<T>(),
                           &compare_value,
                           &swap_value,
                           old_value,
                           1,
                           mr_compare,
                           mr_swap,
                           mr_old,
                           dest_addr,
                           remote,
                           context);
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
