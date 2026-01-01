// SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only
#include <loom/core/concepts/data.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "ut.hpp"

using namespace boost::ut;

suite data_concepts_suite = [] {
    "trivially_serializable with primitives"_test = [] {
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
    };

    "trivially_serializable with structs"_test = [] {
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
    };

    "trivially_serializable rejects non-trivial types"_test = [] {
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
    };

    "trivially_serializable rejects pointers"_test = [] {
        static_assert(!loom::trivially_serializable<int*>);
        static_assert(!loom::trivially_serializable<void*>);
        static_assert(!loom::trivially_serializable<const char*>);
    };

    "contiguous_byte_range with byte arrays"_test = [] {
        static_assert(loom::contiguous_byte_range<std::array<std::byte, 64>>);
        static_assert(loom::contiguous_byte_range<std::vector<std::byte>>);
        static_assert(loom::contiguous_byte_range<std::span<std::byte>>);
        static_assert(loom::contiguous_byte_range<std::span<const std::byte>>);
    };

    "contiguous_byte_range rejects char arrays"_test = [] {
        static_assert(!loom::contiguous_byte_range<std::array<char, 64>>);
        static_assert(!loom::contiguous_byte_range<std::vector<char>>);
        static_assert(!loom::contiguous_byte_range<std::span<char>>);
        static_assert(!loom::contiguous_byte_range<std::string>);
        static_assert(!loom::contiguous_byte_range<std::string_view>);
    };

    "contiguous_byte_range rejects unsigned char"_test = [] {
        static_assert(!loom::contiguous_byte_range<std::array<unsigned char, 32>>);
        static_assert(!loom::contiguous_byte_range<std::vector<unsigned char>>);
        static_assert(!loom::contiguous_byte_range<std::span<unsigned char>>);
    };

    "mutable_byte_range accepts mutable containers"_test = [] {
        static_assert(loom::mutable_byte_range<std::array<std::byte, 64>>);
        static_assert(loom::mutable_byte_range<std::vector<std::byte>>);
        static_assert(loom::mutable_byte_range<std::span<std::byte>>);
    };

    "mutable_byte_range rejects char arrays"_test = [] {
        static_assert(!loom::mutable_byte_range<std::array<char, 64>>);
        static_assert(!loom::mutable_byte_range<std::vector<char>>);
        static_assert(!loom::mutable_byte_range<std::string>);
    };

    "mutable_byte_range rejects const containers"_test = [] {
        static_assert(!loom::mutable_byte_range<std::span<const std::byte>>);
        static_assert(!loom::mutable_byte_range<std::string_view>);

        const std::array<std::byte, 64> const_arr{};
        static_assert(!loom::mutable_byte_range<decltype(const_arr)>);
    };

    "buffer_sequence with vector of arrays"_test = [] {
        using buffer_type = std::array<std::byte, 64>;
        using sequence_type = std::vector<buffer_type>;
        static_assert(loom::buffer_sequence<sequence_type>);
    };

    "buffer_sequence with array of arrays"_test = [] {
        using buffer_type = std::array<std::byte, 32>;
        using sequence_type = std::array<buffer_type, 4>;
        static_assert(loom::buffer_sequence<sequence_type>);
    };

    "buffer_sequence with span of spans"_test = [] {
        using buffer_type = std::span<std::byte>;
        using sequence_type = std::span<buffer_type>;
        static_assert(loom::buffer_sequence<sequence_type>);
    };

    "buffer_sequence with vector of vectors"_test = [] {
        using buffer_type = std::vector<std::byte>;
        using sequence_type = std::vector<buffer_type>;
        static_assert(loom::buffer_sequence<sequence_type>);
    };
};

auto main() -> int {}
