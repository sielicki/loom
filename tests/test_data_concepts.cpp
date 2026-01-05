// SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only
#include <loom/core/concepts/data.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

TEST_CASE("trivially_serializable with primitives", "[data_concepts]") {
    static_assert(loom::trivially_serializable<int>);
    static_assert(loom::trivially_serializable<float>);
    static_assert(loom::trivially_serializable<double>);
    static_assert(loom::trivially_serializable<char>);
    static_assert(loom::trivially_serializable<std::uint8_t>);
    static_assert(loom::trivially_serializable<std::uint16_t>);
    static_assert(loom::trivially_serializable<std::uint32_t>);
    static_assert(loom::trivially_serializable<std::uint64_t>);
    static_assert(loom::trivially_serializable<std::int8_t>);
    static_assert(loom::trivially_serializable<std::int16_t>);
    static_assert(loom::trivially_serializable<std::int32_t>);
    static_assert(loom::trivially_serializable<std::int64_t>);
    static_assert(loom::trivially_serializable<std::byte>);
}

TEST_CASE("trivially_serializable with structs", "[data_concepts]") {
    struct trivial_struct {
        int a;
        float b;
        char c;
    };
    static_assert(loom::trivially_serializable<trivial_struct>);

    struct with_array {
        int data[10];
    };
    static_assert(loom::trivially_serializable<with_array>);

    struct nested_trivial {
        trivial_struct inner;
        int x;
    };
    static_assert(loom::trivially_serializable<nested_trivial>);
}

TEST_CASE("trivially_serializable rejects non-trivial types", "[data_concepts]") {
    static_assert(!loom::trivially_serializable<std::string>);
    static_assert(!loom::trivially_serializable<std::vector<int>>);

    struct non_trivial {
        std::string name;
    };
    static_assert(!loom::trivially_serializable<non_trivial>);

    struct with_virtual {
        virtual ~with_virtual() = default;
        int x;
    };
    static_assert(!loom::trivially_serializable<with_virtual>);
}

TEST_CASE("trivially_serializable rejects pointers", "[data_concepts]") {
    static_assert(!loom::trivially_serializable<int*>);
    static_assert(!loom::trivially_serializable<void*>);
    static_assert(!loom::trivially_serializable<const char*>);
}

TEST_CASE("contiguous_byte_range with byte arrays", "[data_concepts]") {
    static_assert(loom::contiguous_byte_range<std::array<std::byte, 64>>);
    static_assert(loom::contiguous_byte_range<std::vector<std::byte>>);
    static_assert(loom::contiguous_byte_range<std::span<std::byte>>);
    static_assert(loom::contiguous_byte_range<std::span<const std::byte>>);
}

TEST_CASE("contiguous_byte_range rejects char arrays", "[data_concepts]") {
    static_assert(!loom::contiguous_byte_range<std::array<char, 64>>);
    static_assert(!loom::contiguous_byte_range<std::vector<char>>);
    static_assert(!loom::contiguous_byte_range<std::span<char>>);
    static_assert(!loom::contiguous_byte_range<std::string>);
    static_assert(!loom::contiguous_byte_range<std::string_view>);
}

TEST_CASE("contiguous_byte_range rejects unsigned char", "[data_concepts]") {
    static_assert(!loom::contiguous_byte_range<std::array<unsigned char, 32>>);
    static_assert(!loom::contiguous_byte_range<std::vector<unsigned char>>);
    static_assert(!loom::contiguous_byte_range<std::span<unsigned char>>);
}

TEST_CASE("mutable_byte_range accepts mutable containers", "[data_concepts]") {
    static_assert(loom::mutable_byte_range<std::array<std::byte, 64>>);
    static_assert(loom::mutable_byte_range<std::vector<std::byte>>);
    static_assert(loom::mutable_byte_range<std::span<std::byte>>);
}

TEST_CASE("mutable_byte_range rejects char arrays", "[data_concepts]") {
    static_assert(!loom::mutable_byte_range<std::array<char, 64>>);
    static_assert(!loom::mutable_byte_range<std::vector<char>>);
    static_assert(!loom::mutable_byte_range<std::string>);
}

TEST_CASE("mutable_byte_range rejects const containers", "[data_concepts]") {
    static_assert(!loom::mutable_byte_range<std::span<const std::byte>>);
    static_assert(!loom::mutable_byte_range<std::string_view>);

    const std::array<std::byte, 64> const_arr{};
    static_assert(!loom::mutable_byte_range<decltype(const_arr)>);
}

TEST_CASE("buffer_sequence with vector of arrays", "[data_concepts]") {
    using buffer_type = std::array<std::byte, 64>;
    using sequence_type = std::vector<buffer_type>;
    static_assert(loom::buffer_sequence<sequence_type>);
}

TEST_CASE("buffer_sequence with array of arrays", "[data_concepts]") {
    using buffer_type = std::array<std::byte, 32>;
    using sequence_type = std::array<buffer_type, 4>;
    static_assert(loom::buffer_sequence<sequence_type>);
}

TEST_CASE("buffer_sequence with span of spans", "[data_concepts]") {
    using buffer_type = std::span<std::byte>;
    using sequence_type = std::span<buffer_type>;
    static_assert(loom::buffer_sequence<sequence_type>);
}

TEST_CASE("buffer_sequence with vector of vectors", "[data_concepts]") {
    using buffer_type = std::vector<std::byte>;
    using sequence_type = std::vector<buffer_type>;
    static_assert(loom::buffer_sequence<sequence_type>);
}
