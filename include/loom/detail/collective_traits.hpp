// SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only
#pragma once

#include <complex>
#include <cstddef>
#include <cstdint>

#include "loom/core/atomic.hpp"

namespace loom::collective::detail {

template <typename T, atomic::datatype Datatype>
struct element_description {
    using value_type = T;
    static constexpr auto datatype = Datatype;
    static constexpr auto native_datatype = atomic::detail::to_fi_datatype(Datatype);
    static constexpr auto size = sizeof(T);
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

}  // namespace loom::collective::detail
