// SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only
#pragma once

#include <compare>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <type_traits>
#include <utility>

/**
 * @namespace loom
 * @brief Core namespace for the loom libfabric bindings library.
 */
namespace loom {

/**
 * @class strong_type
 * @brief A type-safe wrapper that prevents implicit conversions between semantically different
 * values.
 * @tparam T The underlying value type.
 * @tparam Tag A tag type to distinguish different strong types with the same underlying type.
 */
template <typename T, typename Tag>
class strong_type {
public:
    using value_type = T;  ///< The underlying value type
    using tag_type = Tag;  ///< The tag type for type distinction

    /**
     * @brief Default constructor.
     */
    constexpr strong_type() noexcept(std::is_nothrow_default_constructible_v<T>)
        requires std::is_default_constructible_v<T>
    = default;

    /**
     * @brief Constructs a strong_type from a compatible value.
     * @tparam U The type of the value to construct from.
     * @param value The value to wrap.
     */
    template <typename U>
        requires std::constructible_from<T, U> && (!std::same_as<std::decay_t<U>, strong_type>)
    constexpr explicit strong_type(U&& value) noexcept(std::is_nothrow_constructible_v<T, U>)
        : value_(std::forward<U>(value)) {}

    /**
     * @brief Returns a const reference to the underlying value.
     * @return Const reference to the wrapped value.
     */
    [[nodiscard]] constexpr auto get() const noexcept -> const T& { return value_; }
    /**
     * @brief Returns a mutable reference to the underlying value.
     * @return Mutable reference to the wrapped value.
     */
    [[nodiscard]] constexpr auto get() noexcept -> T& { return value_; }

    /**
     * @brief Explicit conversion to the underlying type.
     * @return A copy of the underlying value.
     */
    [[nodiscard]] constexpr explicit operator T() const
        noexcept(std::is_nothrow_copy_constructible_v<T>) {
        return value_;
    }

    /**
     * @brief Three-way comparison operator.
     */
    [[nodiscard]] constexpr auto operator<=>(const strong_type&) const noexcept = default;
    /**
     * @brief Equality comparison operator.
     */
    [[nodiscard]] constexpr auto operator==(const strong_type&) const noexcept -> bool = default;

    /**
     * @brief Bitwise OR operator for flag-like types.
     * @param other The other strong_type to OR with.
     * @return A new strong_type with the combined flags.
     */
    [[nodiscard]] constexpr auto operator|(const strong_type& other) const noexcept -> strong_type
        requires std::unsigned_integral<T>
    {
        return strong_type{value_ | other.value_};
    }

    /**
     * @brief Bitwise AND operator for flag-like types.
     * @param other The other strong_type to AND with.
     * @return A new strong_type with the masked flags.
     */
    [[nodiscard]] constexpr auto operator&(const strong_type& other) const noexcept -> strong_type
        requires std::unsigned_integral<T>
    {
        return strong_type{value_ & other.value_};
    }

    /**
     * @brief Bitwise XOR operator for flag-like types.
     * @param other The other strong_type to XOR with.
     * @return A new strong_type with the toggled flags.
     */
    [[nodiscard]] constexpr auto operator^(const strong_type& other) const noexcept -> strong_type
        requires std::unsigned_integral<T>
    {
        return strong_type{value_ ^ other.value_};
    }

    /**
     * @brief Bitwise NOT operator for flag-like types.
     * @return A new strong_type with all bits inverted.
     */
    [[nodiscard]] constexpr auto operator~() const noexcept -> strong_type
        requires std::unsigned_integral<T>
    {
        return strong_type{~value_};
    }

    /**
     * @brief Bitwise OR assignment operator.
     * @param other The other strong_type to OR with.
     * @return Reference to this strong_type after modification.
     */
    constexpr auto operator|=(const strong_type& other) noexcept -> strong_type&
        requires std::unsigned_integral<T>
    {
        value_ |= other.value_;
        return *this;
    }

    /**
     * @brief Bitwise AND assignment operator.
     * @param other The other strong_type to AND with.
     * @return Reference to this strong_type after modification.
     */
    constexpr auto operator&=(const strong_type& other) noexcept -> strong_type&
        requires std::unsigned_integral<T>
    {
        value_ &= other.value_;
        return *this;
    }

    /**
     * @brief Bitwise XOR assignment operator.
     * @param other The other strong_type to XOR with.
     * @return Reference to this strong_type after modification.
     */
    constexpr auto operator^=(const strong_type& other) noexcept -> strong_type&
        requires std::unsigned_integral<T>
    {
        value_ ^= other.value_;
        return *this;
    }

    /**
     * @brief Checks if all specified flags are set.
     * @param flags The flags to check for.
     * @return True if all flags are set.
     */
    [[nodiscard]] constexpr auto has(const strong_type& flags) const noexcept -> bool
        requires std::unsigned_integral<T>
    {
        return (value_ & flags.value_) == flags.value_;
    }

    /**
     * @brief Checks if any of the specified flags are set.
     * @param flags The flags to check for.
     * @return True if any flag is set.
     */
    [[nodiscard]] constexpr auto has_any(const strong_type& flags) const noexcept -> bool
        requires std::unsigned_integral<T>
    {
        return (value_ & flags.value_) != T{0};
    }

    /**
     * @brief Boolean conversion operator.
     * @return True if any flags are set.
     */
    [[nodiscard]] constexpr explicit operator bool() const noexcept
        requires std::unsigned_integral<T>
    {
        return value_ != T{0};
    }

private:
    T value_{};
};

/**
 * @class context_ptr
 * @brief A type-safe pointer wrapper for operation contexts.
 * @tparam T The pointed-to type.
 */
template <typename T>
class context_ptr {
public:
    /**
     * @brief Default constructor creating a null pointer.
     */
    constexpr context_ptr() noexcept : ptr_(nullptr) {}
    /**
     * @brief Constructs from a typed pointer.
     * @param ptr The pointer to wrap.
     */
    constexpr explicit context_ptr(T* ptr) noexcept : ptr_(ptr) {}
    /**
     * @brief Constructs from a void pointer.
     * @param ptr The void pointer to cast and wrap.
     */
    constexpr explicit context_ptr(void* ptr) noexcept : ptr_(static_cast<T*>(ptr)) {}

    /**
     * @brief Returns the pointer as the template type.
     * @return The wrapped pointer.
     */
    [[nodiscard]] constexpr auto as() const noexcept -> T* { return ptr_; }

    /**
     * @brief Casts the pointer to a different type.
     * @tparam U The target type.
     * @return The pointer cast to the target type.
     */
    template <typename U>
    [[nodiscard]] constexpr auto as() const noexcept -> U* {
        return static_cast<U*>(static_cast<void*>(ptr_));
    }

    /**
     * @brief Returns the wrapped pointer.
     * @return The wrapped pointer.
     */
    [[nodiscard]] constexpr auto get() const noexcept -> T* { return ptr_; }
    /**
     * @brief Returns the pointer as a void pointer.
     * @return The wrapped pointer as void*.
     */
    [[nodiscard]] constexpr auto raw() const noexcept -> void* { return static_cast<void*>(ptr_); }

    /**
     * @brief Boolean conversion operator.
     * @return True if the pointer is not null.
     */
    [[nodiscard]] constexpr explicit operator bool() const noexcept { return ptr_ != nullptr; }
    /**
     * @brief Dereference operator.
     * @return Reference to the pointed-to object.
     */
    [[nodiscard]] constexpr auto operator*() const noexcept -> T&
        requires(!std::is_void_v<T>)
    {
        return *ptr_;
    }
    /**
     * @brief Member access operator.
     * @return The wrapped pointer.
     */
    [[nodiscard]] constexpr auto operator->() const noexcept -> T* { return ptr_; }

private:
    T* ptr_;
};

/**
 * @class context_ptr<void>
 * @brief Specialization of context_ptr for void pointers.
 */
template <>
class context_ptr<void> {
public:
    /**
     * @brief Default constructor creating a null pointer.
     */
    constexpr context_ptr() noexcept : ptr_(nullptr) {}
    /**
     * @brief Constructs from a void pointer.
     * @param ptr The void pointer to wrap.
     */
    constexpr explicit context_ptr(void* ptr) noexcept : ptr_(ptr) {}

    /**
     * @brief Casts the pointer to a typed pointer.
     * @tparam U The target type.
     * @return The pointer cast to the target type.
     */
    template <typename U>
    [[nodiscard]] constexpr auto as() const noexcept -> U* {
        return static_cast<U*>(ptr_);
    }

    /**
     * @brief Returns the wrapped void pointer.
     * @return The wrapped void pointer.
     */
    [[nodiscard]] constexpr auto get() const noexcept -> void* { return ptr_; }
    /**
     * @brief Returns the wrapped void pointer.
     * @return The wrapped void pointer.
     */
    [[nodiscard]] constexpr auto raw() const noexcept -> void* { return ptr_; }

    /**
     * @brief Boolean conversion operator.
     * @return True if the pointer is not null.
     */
    [[nodiscard]] constexpr explicit operator bool() const noexcept { return ptr_ != nullptr; }

private:
    void* ptr_;
};

/**
 * @struct fabric_version_tag
 * @brief Tag type for fabric version strong type.
 */
struct fabric_version_tag {};
/**
 * @typedef fabric_version
 * @brief Strongly-typed fabric interface version number.
 */
using fabric_version = strong_type<std::uint32_t, fabric_version_tag>;

/**
 * @struct protocol_version_tag
 * @brief Tag type for protocol version strong type.
 */
struct protocol_version_tag {};
/**
 * @typedef protocol_version
 * @brief Strongly-typed endpoint protocol version number.
 */
using protocol_version = strong_type<std::uint32_t, protocol_version_tag>;

/**
 * @struct caps_tag
 * @brief Tag type for capabilities strong type.
 */
struct caps_tag {};
/**
 * @typedef caps
 * @brief Strongly-typed capability flags for fabric operations.
 */
using caps = strong_type<std::uint64_t, caps_tag>;

/**
 * @namespace capability
 * @brief Capability flag constants for fabric operations.
 */
namespace capability {
inline constexpr caps msg{0x1ULL};
inline constexpr caps rma{0x2ULL};
inline constexpr caps tagged{0x4ULL};
inline constexpr caps atomic{0x8ULL};
inline constexpr caps read{0x10ULL};
inline constexpr caps write{0x20ULL};
inline constexpr caps collective{0x40ULL};
inline constexpr caps recv{0x80ULL};
inline constexpr caps send{0x100ULL};
inline constexpr caps remote_read{0x200ULL};
inline constexpr caps remote_write{0x400ULL};
inline constexpr caps multi_recv{0x800ULL};
inline constexpr caps remote_comm{0x1000ULL};
inline constexpr caps fence{0x2000ULL};
inline constexpr caps local_comm{0x4000ULL};
inline constexpr caps msg_prefix{0x8000ULL};
inline constexpr caps hmem{0x10000ULL};
}  // namespace capability

/**
 * @struct mode_tag
 * @brief Tag type for mode strong type.
 */
struct mode_tag {};
/**
 * @typedef mode
 * @brief Strongly-typed mode flags indicating application behavior requirements.
 */
using mode = strong_type<std::uint64_t, mode_tag>;

/**
 * @namespace mode_bits
 * @brief Mode flag constants specifying application behavior requirements.
 */
namespace mode_bits {
inline constexpr mode context{0x1ULL};
inline constexpr mode msg_prefix{0x2ULL};
inline constexpr mode rx_cq_data{0x4ULL};
inline constexpr mode local_mr{0x8ULL};
}  // namespace mode_bits

/**
 * @struct msg_order_tag
 * @brief Tag type for message ordering strong type.
 */
struct msg_order_tag {};
/**
 * @typedef msg_order
 * @brief Strongly-typed message ordering guarantees for transmit operations.
 */
using msg_order = strong_type<std::uint64_t, msg_order_tag>;

/**
 * @namespace ordering
 * @brief Message ordering constants for transmit operations.
 */
namespace ordering {
inline constexpr msg_order none{0ULL};
inline constexpr msg_order strict{0x1ULL};
inline constexpr msg_order data{0x2ULL};
inline constexpr msg_order raw{0x100ULL};
inline constexpr msg_order war{0x200ULL};
inline constexpr msg_order waw{0x400ULL};
}  // namespace ordering

/**
 * @struct comp_order_tag
 * @brief Tag type for completion ordering strong type.
 */
struct comp_order_tag {};
/**
 * @typedef comp_order
 * @brief Strongly-typed completion ordering guarantees for operations.
 */
using comp_order = strong_type<std::uint64_t, comp_order_tag>;

/**
 * @namespace completion_ordering
 * @brief Completion ordering constants for operation completions.
 */
namespace completion_ordering {
inline constexpr comp_order none{0ULL};
inline constexpr comp_order strict{0x1ULL};
inline constexpr comp_order data{0x2ULL};
}  // namespace completion_ordering

/**
 * @enum address_format
 * @brief Network address format types supported by fabric providers.
 */
enum class address_format : std::uint32_t {
    unspecified = 0,  ///< No specific format required
    inet = 1,         ///< IPv4 socket address
    inet6 = 2,        ///< IPv6 socket address
    ib = 3,           ///< InfiniBand address
    psmx = 4,         ///< PSM address (deprecated)
    psmx2 = 5,        ///< PSM2 address
    gni = 6,          ///< Cray GNI address
    bgq = 7,          ///< IBM Blue Gene/Q address
    ethernet = 8,     ///< Ethernet MAC address
    str = 9,          ///< String-formatted address
};

/**
 * @struct size_tag
 * @brief Tag type for wrapped size strong type.
 */
struct size_tag {};
/**
 * @typedef size_t_wrapped
 * @brief Strongly-typed size value for type-safe size parameters.
 */
using size_t_wrapped = strong_type<std::size_t, size_tag>;

/**
 * @struct rma_addr_tag
 * @brief Tag type for RMA address strong type.
 */
struct rma_addr_tag {};
/**
 * @typedef rma_addr
 * @brief Strongly-typed remote memory address for RMA operations.
 */
using rma_addr = strong_type<std::uint64_t, rma_addr_tag>;

/**
 * @struct mr_key_tag
 * @brief Tag type for memory region key strong type.
 */
struct mr_key_tag {};
/**
 * @typedef mr_key
 * @brief Strongly-typed memory region key for remote memory access.
 */
using mr_key = strong_type<std::uint64_t, mr_key_tag>;

/**
 * @struct mr_access_tag
 * @brief Tag type for memory region access flags strong type.
 */
struct mr_access_tag {};
/**
 * @typedef mr_access
 * @brief Strongly-typed memory region access permission flags.
 */
using mr_access = strong_type<std::uint64_t, mr_access_tag>;

/**
 * @namespace mr_access_flags
 * @brief Memory region access permission flag constants.
 */
namespace mr_access_flags {
inline constexpr mr_access read{0x1ULL};
inline constexpr mr_access write{0x2ULL};
inline constexpr mr_access remote_read{0x4ULL};
inline constexpr mr_access remote_write{0x8ULL};
inline constexpr mr_access send{0x10ULL};
inline constexpr mr_access recv{0x20ULL};
}  // namespace mr_access_flags

/**
 * @struct mr_mode_tag
 * @brief Tag type for memory region mode strong type.
 */
struct mr_mode_tag {};
/**
 * @typedef mr_mode
 * @brief Strongly-typed memory region mode flags for registration behavior.
 */
using mr_mode = strong_type<std::uint64_t, mr_mode_tag>;

/**
 * @namespace mr_mode_flags
 * @brief Memory region mode flag constants for registration behavior.
 */
namespace mr_mode_flags {
inline constexpr mr_mode basic{0ULL};
inline constexpr mr_mode scalable{1ULL << 0};
inline constexpr mr_mode local{1ULL << 1};
inline constexpr mr_mode virt_addr{1ULL << 2};
inline constexpr mr_mode allocated{1ULL << 3};
inline constexpr mr_mode prov_key{1ULL << 4};
inline constexpr mr_mode raw{1ULL << 5};
inline constexpr mr_mode hmem{1ULL << 6};
inline constexpr mr_mode endpoint{1ULL << 7};
inline constexpr mr_mode collective{1ULL << 8};
}  // namespace mr_mode_flags

/**
 * @struct tag_type_tag
 * @brief Tag type for message tag strong type.
 */
struct tag_type_tag {};
/**
 * @typedef tag_type
 * @brief Strongly-typed message tag for tagged messaging operations.
 */
using tag_type = strong_type<std::uint64_t, tag_type_tag>;

/**
 * @enum atomic_op
 * @brief Atomic operation types for remote memory atomics.
 */
enum class atomic_op : std::uint32_t {
    min = 0,        ///< Minimum of operands
    max = 1,        ///< Maximum of operands
    sum = 2,        ///< Sum of operands
    prod = 3,       ///< Product of operands
    lor = 4,        ///< Logical OR
    land = 5,       ///< Logical AND
    bor = 6,        ///< Bitwise OR
    band = 7,       ///< Bitwise AND
    lxor = 8,       ///< Logical XOR
    bxor = 9,       ///< Bitwise XOR
    read = 10,      ///< Atomic read
    write = 11,     ///< Atomic write
    cswap = 12,     ///< Compare and swap if equal
    cswap_ne = 13,  ///< Compare and swap if not equal
    cswap_le = 14,  ///< Compare and swap if less or equal
    cswap_lt = 15,  ///< Compare and swap if less than
    cswap_ge = 16,  ///< Compare and swap if greater or equal
    cswap_gt = 17,  ///< Compare and swap if greater than
    mswap = 18,     ///< Masked swap
};

/**
 * @enum atomic_datatype
 * @brief Data types supported for atomic operations.
 */
enum class atomic_datatype : std::uint32_t {
    int8 = 0,                  ///< Signed 8-bit integer
    uint8 = 1,                 ///< Unsigned 8-bit integer
    int16 = 2,                 ///< Signed 16-bit integer
    uint16 = 3,                ///< Unsigned 16-bit integer
    int32 = 4,                 ///< Signed 32-bit integer
    uint32 = 5,                ///< Unsigned 32-bit integer
    int64 = 6,                 ///< Signed 64-bit integer
    uint64 = 7,                ///< Unsigned 64-bit integer
    float32 = 8,               ///< 32-bit floating point
    float64 = 9,               ///< 64-bit floating point
    float128 = 10,             ///< 128-bit floating point
    float_complex = 11,        ///< Single-precision complex
    double_complex = 12,       ///< Double-precision complex
    long_double_complex = 13,  ///< Extended-precision complex
};

/**
 * @struct queue_size_tag
 * @brief Tag type for queue size strong type.
 */
struct queue_size_tag {};
/**
 * @typedef queue_size
 * @brief Strongly-typed queue depth or message size limit.
 */
using queue_size = strong_type<std::size_t, queue_size_tag>;

/**
 * @enum threading_mode
 * @brief Threading safety levels supported by fabric providers.
 */
enum class threading_mode : std::uint32_t {
    unspecified = 0,  ///< No threading requirements specified
    safe = 1,         ///< Thread-safe, concurrent access allowed
    fid = 2,          ///< Per-object serialization required
    domain = 3,       ///< Per-domain serialization required
    completion = 4,   ///< Completion serialization required
};

/**
 * @enum progress_mode
 * @brief Progress engine modes for driving communication.
 */
enum class progress_mode : std::uint32_t {
    unspecified = 0,    ///< No progress mode specified
    auto_progress = 1,  ///< Provider drives progress automatically
    manual = 2,         ///< Application must poll for progress
    manual_all = 3,     ///< Application controls all progress
};

/**
 * @struct fabric_addr_tag
 * @brief Tag type for fabric address strong type.
 */
struct fabric_addr_tag {};
/**
 * @typedef fabric_addr
 * @brief Strongly-typed fabric address for peer addressing.
 */
using fabric_addr = strong_type<std::uint64_t, fabric_addr_tag>;

/**
 * @namespace fabric_addrs
 * @brief Predefined fabric address constants.
 */
namespace fabric_addrs {
/**
 * @brief Special value indicating no specific address.
 */
inline constexpr fabric_addr unspecified{static_cast<std::uint64_t>(-1)};
}  // namespace fabric_addrs

/**
 * @struct cq_bind_flags_tag
 * @brief Tag type for completion queue binding flags strong type.
 */
struct cq_bind_flags_tag {};
/**
 * @typedef cq_bind_flags
 * @brief Strongly-typed flags for binding completion queues to endpoints.
 */
using cq_bind_flags = strong_type<std::uint64_t, cq_bind_flags_tag>;

/**
 * @namespace cq_bind
 * @brief Completion queue binding flag constants.
 */
namespace cq_bind {
inline constexpr cq_bind_flags transmit{1ULL << 0};
inline constexpr cq_bind_flags recv{1ULL << 1};
inline constexpr cq_bind_flags selective_completion{1ULL << 2};
}  // namespace cq_bind

/**
 * @struct op_flags_tag
 * @brief Tag type for operation flags strong type.
 */
struct op_flags_tag {};
/**
 * @typedef op_flags
 * @brief Strongly-typed operation modifier flags.
 */
using op_flags = strong_type<std::uint64_t, op_flags_tag>;

/**
 * @namespace op_flag
 * @brief Operation modifier flag constants.
 */
namespace op_flag {
inline constexpr op_flags none{0ULL};
inline constexpr op_flags completion{1ULL << 0};
inline constexpr op_flags inject{1ULL << 1};
inline constexpr op_flags fence{1ULL << 2};
inline constexpr op_flags transmit_complete{1ULL << 3};
inline constexpr op_flags delivery_complete{1ULL << 4};
}  // namespace op_flag

/**
 * @class mr_descriptor
 * @brief Opaque memory region descriptor for data operations.
 *
 * Memory region descriptors are provider-specific handles used to reference
 * registered memory during data transfer operations. They must be passed
 * to send, receive, and RMA operations when using registered buffers.
 */
class mr_descriptor {
public:
    /**
     * @brief Default constructor creating a null descriptor.
     */
    constexpr mr_descriptor() noexcept = default;
    /**
     * @brief Constructs a descriptor from a raw pointer.
     * @param desc The provider-specific descriptor pointer.
     */
    constexpr explicit mr_descriptor(void* desc) noexcept : desc_(desc) {}

    /**
     * @brief Returns the raw descriptor pointer.
     * @return The provider-specific descriptor pointer.
     */
    [[nodiscard]] constexpr auto raw() const noexcept -> void* { return desc_; }
    /**
     * @brief Boolean conversion to check if descriptor is valid.
     * @return True if the descriptor is not null.
     */
    [[nodiscard]] constexpr explicit operator bool() const noexcept { return desc_ != nullptr; }
    /**
     * @brief Equality comparison operator.
     * @return True if descriptors are equal.
     */
    [[nodiscard]] constexpr auto operator==(const mr_descriptor&) const noexcept -> bool = default;

private:
    void* desc_{nullptr};
};

}  // namespace loom
