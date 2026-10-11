// SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only
#include <loom/core/collective.hpp>

#include <complex>
#include <string>

#include <catch2/catch_test_macros.hpp>

namespace {
template <typename T>
concept can_broadcast =
    requires(loom::endpoint& ep, loom::collective::group& group, std::span<T> data) {
        loom::collective::broadcast(ep, group, data, nullptr, loom::fabric_addr{1});
    };

template <typename T>
concept can_all_reduce = requires(loom::endpoint& ep,
                                  loom::collective::group& group,
                                  std::span<const T> input,
                                  std::span<T> output) {
    loom::collective::all_reduce(
        ep, group, input, output, nullptr, nullptr, loom::atomic::operation::sum);
};
}  // namespace

TEST_CASE("collective overloads reject unmapped element types", "[collective][types]") {
    REQUIRE_FALSE(can_broadcast<bool>);
    REQUIRE_FALSE(can_broadcast<char>);
    REQUIRE_FALSE(can_broadcast<std::string>);
    REQUIRE_FALSE(can_broadcast<const std::uint32_t>);
    REQUIRE_FALSE(can_broadcast<volatile std::uint32_t>);
    REQUIRE_FALSE(can_all_reduce<bool>);
    REQUIRE_FALSE(can_all_reduce<char>);
    REQUIRE_FALSE(can_all_reduce<std::string>);
}

TEST_CASE("collective overloads accept mapped element types", "[collective][types]") {
    REQUIRE(can_broadcast<std::uint8_t>);
    REQUIRE(can_broadcast<std::int32_t>);
    REQUIRE(can_all_reduce<std::uint64_t>);
    REQUIRE(can_all_reduce<double>);
    REQUIRE(can_all_reduce<long double>);
    REQUIRE(can_all_reduce<std::complex<float>>);
    REQUIRE(can_all_reduce<std::complex<double>>);
    REQUIRE(can_all_reduce<std::complex<long double>>);
}
