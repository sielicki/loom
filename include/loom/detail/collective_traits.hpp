// SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only
#pragma once

#include <rdma/fi_collective.h>

#include <complex>
#include <cstddef>
#include <cstdint>
#include <optional>

#include "loom/core/atomic.hpp"

namespace loom::collective::detail {

struct element_layout {
    fi_datatype datatype;
    std::size_t size;
};

template <typename T, atomic::datatype Datatype>
struct element_description {
    using value_type = T;
    static constexpr auto datatype = Datatype;
    static constexpr auto native_datatype = atomic::detail::to_fi_datatype(Datatype);
    static constexpr auto size = sizeof(T);
    static constexpr element_layout layout{native_datatype, size};
};

// Only explicitly mapped types may enter typed collective operations.
template <typename T>
struct element_traits {};

template <>
struct element_traits<std::int8_t> : element_description<std::int8_t, atomic::datatype::int8> {};
template <>
struct element_traits<std::uint8_t> : element_description<std::uint8_t, atomic::datatype::uint8> {};
template <>
struct element_traits<std::int16_t> : element_description<std::int16_t, atomic::datatype::int16> {};
template <>
struct element_traits<std::uint16_t>
    : element_description<std::uint16_t, atomic::datatype::uint16> {};
template <>
struct element_traits<std::int32_t> : element_description<std::int32_t, atomic::datatype::int32> {};
template <>
struct element_traits<std::uint32_t>
    : element_description<std::uint32_t, atomic::datatype::uint32> {};
template <>
struct element_traits<std::int64_t> : element_description<std::int64_t, atomic::datatype::int64> {};
template <>
struct element_traits<std::uint64_t>
    : element_description<std::uint64_t, atomic::datatype::uint64> {};
template <>
struct element_traits<float> : element_description<float, atomic::datatype::float32> {};
template <>
struct element_traits<double> : element_description<double, atomic::datatype::double64> {};
template <>
struct element_traits<long double>
    : element_description<long double, atomic::datatype::long_double> {};
template <>
struct element_traits<std::complex<float>>
    : element_description<std::complex<float>, atomic::datatype::float_complex> {};
template <>
struct element_traits<std::complex<double>>
    : element_description<std::complex<double>, atomic::datatype::double_complex> {};
template <>
struct element_traits<std::complex<long double>>
    : element_description<std::complex<long double>, atomic::datatype::long_double_complex> {};
#if LOOM_HAS_INT128
template <>
struct element_traits<atomic::int128_type>
    : element_description<atomic::int128_type, atomic::datatype::int128> {};
template <>
struct element_traits<atomic::uint128_type>
    : element_description<atomic::uint128_type, atomic::datatype::uint128> {};
#endif

constexpr auto describe_element(atomic::datatype type) noexcept -> std::optional<element_layout> {
    switch (type) {
        case atomic::datatype::int8:
            return element_traits<std::int8_t>::layout;
        case atomic::datatype::uint8:
            return element_traits<std::uint8_t>::layout;
        case atomic::datatype::int16:
            return element_traits<std::int16_t>::layout;
        case atomic::datatype::uint16:
            return element_traits<std::uint16_t>::layout;
        case atomic::datatype::int32:
            return element_traits<std::int32_t>::layout;
        case atomic::datatype::uint32:
            return element_traits<std::uint32_t>::layout;
        case atomic::datatype::int64:
            return element_traits<std::int64_t>::layout;
        case atomic::datatype::uint64:
            return element_traits<std::uint64_t>::layout;
        case atomic::datatype::float32:
            return element_traits<float>::layout;
        case atomic::datatype::double64:
            return element_traits<double>::layout;
        case atomic::datatype::float_complex:
            return element_traits<std::complex<float>>::layout;
        case atomic::datatype::double_complex:
            return element_traits<std::complex<double>>::layout;
        case atomic::datatype::long_double:
            return element_traits<long double>::layout;
        case atomic::datatype::long_double_complex:
            return element_traits<std::complex<long double>>::layout;
        case atomic::datatype::int128:
#if LOOM_HAS_INT128
            return element_traits<atomic::int128_type>::layout;
#else
            return element_layout{FI_INT128, 2 * sizeof(std::uint64_t)};
#endif
        case atomic::datatype::uint128:
#if LOOM_HAS_INT128
            return element_traits<atomic::uint128_type>::layout;
#else
            return element_layout{FI_UINT128, 2 * sizeof(std::uint64_t)};
#endif
    }
    return std::nullopt;
}

enum class count_basis { input, output, partitioned_input };
enum class buffer_extent { unused, single, group, root_single, root_group };

template <count_basis Basis, buffer_extent Input, buffer_extent Output>
struct transfer_shape {
    static constexpr auto basis = Basis;
    static constexpr auto input = Input;
    static constexpr auto output = Output;
};

template <fi_collective_op Operation>
struct transfer_traits;

template <>
struct transfer_traits<FI_BROADCAST>
    : transfer_shape<count_basis::input, buffer_extent::single, buffer_extent::unused> {};
template <>
struct transfer_traits<FI_ALLREDUCE>
    : transfer_shape<count_basis::input, buffer_extent::single, buffer_extent::single> {};
template <>
struct transfer_traits<FI_ALLGATHER>
    : transfer_shape<count_basis::input, buffer_extent::single, buffer_extent::group> {};
template <>
struct transfer_traits<FI_ALLTOALL>
    : transfer_shape<count_basis::partitioned_input, buffer_extent::group, buffer_extent::group> {};
template <>
struct transfer_traits<FI_REDUCE>
    : transfer_shape<count_basis::input, buffer_extent::single, buffer_extent::root_single> {};
template <>
struct transfer_traits<FI_SCATTER>
    : transfer_shape<count_basis::output, buffer_extent::root_group, buffer_extent::single> {};
template <>
struct transfer_traits<FI_GATHER>
    : transfer_shape<count_basis::input, buffer_extent::single, buffer_extent::root_group> {};
template <>
struct transfer_traits<FI_REDUCE_SCATTER>
    : transfer_shape<count_basis::output, buffer_extent::group, buffer_extent::single> {};

struct element_count {
    std::size_t value;
};

struct transfer_layout {
    element_count count;
    fi_datatype datatype;
};

template <buffer_extent Extent>
constexpr auto fits_buffer(std::size_t bytes,
                           element_layout element,
                           element_count count,
                           std::optional<std::size_t> members) noexcept -> bool {
    if constexpr (Extent == buffer_extent::unused) {
        return true;
    }
    if constexpr (Extent == buffer_extent::root_single || Extent == buffer_extent::root_group) {
        // Root-only buffers may be omitted by non-root participants.
        if (bytes == 0) {
            return true;
        }
    }
    auto capacity = bytes / element.size;
    if constexpr (Extent == buffer_extent::group || Extent == buffer_extent::root_group) {
        if (!members || *members == 0) {
            return false;
        }
        capacity /= *members;
    }
    return count.value <= capacity;
}

template <fi_collective_op Operation>
auto checked_transfer(std::size_t input_bytes,
                      std::size_t output_bytes,
                      atomic::datatype type,
                      std::optional<std::size_t> members) -> result<transfer_layout> {
    using traits = transfer_traits<Operation>;
    const auto element = describe_element(type);
    if (!element) {
        return make_error_result<transfer_layout>(errc::invalid_argument);
    }
    const auto bytes = traits::basis == count_basis::output ? output_bytes : input_bytes;
    if (bytes % element->size != 0) {
        return make_error_result<transfer_layout>(errc::invalid_argument);
    }
    element_count count{bytes / element->size};
    if constexpr (traits::basis == count_basis::partitioned_input) {
        if (!members || *members == 0 || count.value % *members != 0) {
            return make_error_result<transfer_layout>(errc::invalid_argument);
        }
        count.value /= *members;
    }
    // Division checks the full group extent without overflowing a byte-count product.
    if (!fits_buffer<traits::input>(input_bytes, *element, count, members) ||
        !fits_buffer<traits::output>(output_bytes, *element, count, members)) {
        return make_error_result<transfer_layout>(errc::invalid_argument);
    }
    return transfer_layout{count, element->datatype};
}

}  // namespace loom::collective::detail
