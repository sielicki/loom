// SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only
#pragma once

#include <bit>
#include <cstdint>
#include <type_traits>

/**
 * @namespace loom
 * @brief Core namespace for the loom libfabric bindings library.
 */
namespace loom {

/**
 * @struct immediate_data
 * @brief Wrapper for 64-bit immediate data in RDMA operations.
 *
 * Provides type-safe access to immediate data passed with RDMA writes.
 */
struct immediate_data {
    std::uint64_t raw{0};  ///< Raw 64-bit value

    /**
     * @brief Default constructor.
     */
    constexpr immediate_data() noexcept = default;

    /**
     * @brief Constructs from a raw value.
     * @param value The raw 64-bit value.
     */
    constexpr explicit immediate_data(std::uint64_t value) noexcept : raw(value) {}

    /**
     * @brief Returns the raw value.
     * @return The 64-bit raw value.
     */
    [[nodiscard]] constexpr auto value() const noexcept -> std::uint64_t { return raw; }

    /**
     * @brief Explicit conversion to uint64_t.
     */
    [[nodiscard]] constexpr explicit operator std::uint64_t() const noexcept { return raw; }

    /**
     * @brief Equality comparison operator.
     */
    [[nodiscard]] constexpr auto operator==(const immediate_data&) const noexcept -> bool = default;
};

/**
 * @struct immediate_data_layout
 * @brief Compile-time layout for structured immediate data.
 * @tparam TypeBits Bits for message type field.
 * @tparam IndexBits Bits for index field.
 * @tparam IdBits Bits for ID field.
 * @tparam SeqBits Bits for sequence number field.
 *
 * Provides encoding/decoding of structured data within 64-bit immediate data.
 */
template <std::size_t TypeBits, std::size_t IndexBits, std::size_t IdBits, std::size_t SeqBits = 0>
struct immediate_data_layout {
    static_assert(TypeBits + IndexBits + IdBits + SeqBits <= 64, "Total bits must not exceed 64");

    static constexpr std::size_t type_bits = TypeBits;    ///< Type field bit count
    static constexpr std::size_t index_bits = IndexBits;  ///< Index field bit count
    static constexpr std::size_t id_bits = IdBits;        ///< ID field bit count
    static constexpr std::size_t seq_bits = SeqBits;      ///< Sequence field bit count

    static constexpr std::size_t seq_shift = 0;
    static constexpr std::size_t id_shift = seq_bits;
    static constexpr std::size_t index_shift = id_shift + id_bits;
    static constexpr std::size_t type_shift = index_shift + index_bits;

    static constexpr std::uint64_t seq_mask = (1ULL << seq_bits) - 1;
    static constexpr std::uint64_t id_mask = (1ULL << id_bits) - 1;
    static constexpr std::uint64_t index_mask = (1ULL << index_bits) - 1;
    static constexpr std::uint64_t type_mask = (1ULL << type_bits) - 1;

    /**
     * @brief Encodes fields into immediate data.
     * @param msg_type Message type.
     * @param index Index value.
     * @param id ID value.
     * @param seq Sequence number.
     * @return Encoded immediate data.
     */
    [[nodiscard]] static constexpr auto
    encode(std::uint32_t msg_type, std::uint32_t index, std::uint32_t id, std::uint32_t seq = 0)
        -> immediate_data {
        std::uint64_t value = 0;
        value |= (static_cast<std::uint64_t>(seq) & seq_mask) << seq_shift;
        value |= (static_cast<std::uint64_t>(id) & id_mask) << id_shift;
        value |= (static_cast<std::uint64_t>(index) & index_mask) << index_shift;
        value |= (static_cast<std::uint64_t>(msg_type) & type_mask) << type_shift;
        return immediate_data{value};
    }

    /**
     * @brief Decodes the type field.
     * @param data The immediate data to decode.
     * @return The type field value.
     */
    [[nodiscard]] static constexpr auto decode_type(immediate_data data) -> std::uint32_t {
        return static_cast<std::uint32_t>((data.raw >> type_shift) & type_mask);
    }

    /**
     * @brief Decodes the index field.
     * @param data The immediate data to decode.
     * @return The index field value.
     */
    [[nodiscard]] static constexpr auto decode_index(immediate_data data) -> std::uint32_t {
        return static_cast<std::uint32_t>((data.raw >> index_shift) & index_mask);
    }

    /**
     * @brief Decodes the ID field.
     * @param data The immediate data to decode.
     * @return The ID field value.
     */
    [[nodiscard]] static constexpr auto decode_id(immediate_data data) -> std::uint32_t {
        return static_cast<std::uint32_t>((data.raw >> id_shift) & id_mask);
    }

    /**
     * @brief Decodes the sequence field.
     * @param data The immediate data to decode.
     * @return The sequence field value.
     */
    [[nodiscard]] static constexpr auto decode_seq(immediate_data data) -> std::uint32_t {
        return static_cast<std::uint32_t>((data.raw >> seq_shift) & seq_mask);
    }
};

using nixl_imm_layout = immediate_data_layout<4, 8, 16, 4>;  ///< NIXL immediate data layout

using nccl_imm_layout = immediate_data_layout<8, 8, 16, 0>;  ///< NCCL immediate data layout

/**
 * @namespace loom::imm
 * @brief Immediate data encoding/decoding utilities.
 */
namespace imm {

/**
 * @brief Encodes fields using a specific layout.
 * @tparam Layout The layout type to use.
 */
template <typename Layout>
[[nodiscard]] constexpr auto
encode(std::uint32_t msg_type, std::uint32_t index, std::uint32_t id, std::uint32_t seq = 0)
    -> immediate_data {
    return Layout::encode(msg_type, index, id, seq);
}

/**
 * @brief Decodes the type field using a specific layout.
 * @tparam Layout The layout type to use.
 */
template <typename Layout>
[[nodiscard]] constexpr auto decode_type(immediate_data data) -> std::uint32_t {
    return Layout::decode_type(data);
}

/**
 * @brief Decodes the index field using a specific layout.
 * @tparam Layout The layout type to use.
 */
template <typename Layout>
[[nodiscard]] constexpr auto decode_index(immediate_data data) -> std::uint32_t {
    return Layout::decode_index(data);
}

/**
 * @brief Decodes the ID field using a specific layout.
 * @tparam Layout The layout type to use.
 */
template <typename Layout>
[[nodiscard]] constexpr auto decode_id(immediate_data data) -> std::uint32_t {
    return Layout::decode_id(data);
}

/**
 * @brief Decodes the sequence field using a specific layout.
 * @tparam Layout The layout type to use.
 */
template <typename Layout>
[[nodiscard]] constexpr auto decode_seq(immediate_data data) -> std::uint32_t {
    return Layout::decode_seq(data);
}

/**
 * @brief Creates immediate data from a raw value.
 * @param value The raw 64-bit value.
 * @return The immediate data.
 */
[[nodiscard]] constexpr auto from_raw(std::uint64_t value) -> immediate_data {
    return immediate_data{value};
}

/**
 * @brief Extracts the raw value from immediate data.
 * @param data The immediate data.
 * @return The raw 64-bit value.
 */
[[nodiscard]] constexpr auto to_raw(immediate_data data) -> std::uint64_t {
    return data.raw;
}

/**
 * @brief Packs a trivially copyable value into immediate data.
 * @tparam T The value type (must fit in 64 bits).
 * @param value The value to pack.
 * @return The packed immediate data.
 */
template <typename T>
    requires std::is_trivially_copyable_v<T> && (sizeof(T) <= sizeof(std::uint64_t))
[[nodiscard]] constexpr auto pack(const T& value) -> immediate_data {
    std::uint64_t raw = 0;
    if constexpr (sizeof(T) == sizeof(std::uint64_t)) {
        raw = std::bit_cast<std::uint64_t>(value);
    } else if constexpr (sizeof(T) == sizeof(std::uint32_t)) {
        raw = std::bit_cast<std::uint32_t>(value);
    } else if constexpr (sizeof(T) == sizeof(std::uint16_t)) {
        raw = std::bit_cast<std::uint16_t>(value);
    } else if constexpr (sizeof(T) == sizeof(std::uint8_t)) {
        raw = std::bit_cast<std::uint8_t>(value);
    } else {
        static_assert(sizeof(T) == 0, "Unsupported type size for immediate data packing");
    }
    return immediate_data{raw};
}

/**
 * @brief Unpacks immediate data to a trivially copyable value.
 * @tparam T The target value type.
 * @param data The immediate data to unpack.
 * @return The unpacked value.
 */
template <typename T>
    requires std::is_trivially_copyable_v<T> && (sizeof(T) <= sizeof(std::uint64_t))
[[nodiscard]] constexpr auto unpack(immediate_data data) -> T {
    if constexpr (sizeof(T) == sizeof(std::uint64_t)) {
        return std::bit_cast<T>(data.raw);
    } else if constexpr (sizeof(T) == sizeof(std::uint32_t)) {
        return std::bit_cast<T>(static_cast<std::uint32_t>(data.raw));
    } else if constexpr (sizeof(T) == sizeof(std::uint16_t)) {
        return std::bit_cast<T>(static_cast<std::uint16_t>(data.raw));
    } else if constexpr (sizeof(T) == sizeof(std::uint8_t)) {
        return std::bit_cast<T>(static_cast<std::uint8_t>(data.raw));
    } else {
        static_assert(sizeof(T) == 0, "Unsupported type size for immediate data unpacking");
    }
}

}  // namespace imm

}  // namespace loom
