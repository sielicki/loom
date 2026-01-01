// SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only
#pragma once

#include <format>
#include <string_view>

#include "loom/core/address.hpp"
#include "loom/core/endpoint_types.hpp"
#include "loom/core/error.hpp"
#include "loom/core/memory.hpp"
#include "loom/core/result.hpp"
#include "loom/core/types.hpp"

/**
 * @struct formatter<loom::strong_type>
 * @brief Formatter specialization for strong_type wrapper.
 * @tparam T The underlying value type.
 * @tparam Tag The tag type for type safety.
 */
template <typename T, typename Tag>
struct std::formatter<loom::strong_type<T, Tag>> : std::formatter<T> {
    /**
     * @brief Formats the strong_type by delegating to the underlying type formatter.
     * @tparam FormatContext The format context type.
     * @param value The strong_type value to format.
     * @param ctx The format context.
     * @return Iterator past the formatted output.
     */
    template <typename FormatContext>
    auto format(const loom::strong_type<T, Tag>& value, FormatContext& ctx) const {
        return std::formatter<T>::format(value.get(), ctx);
    }
};

/**
 * @struct formatter<loom::context_ptr>
 * @brief Formatter specialization for context_ptr.
 * @tparam T The pointed-to type.
 */
template <typename T>
struct std::formatter<loom::context_ptr<T>> : std::formatter<const void*> {
    /**
     * @brief Formats the context_ptr as a raw pointer address.
     * @tparam FormatContext The format context type.
     * @param ptr The context pointer to format.
     * @param ctx The format context.
     * @return Iterator past the formatted output.
     */
    template <typename FormatContext>
    auto format(const loom::context_ptr<T>& ptr, FormatContext& ctx) const {
        return std::formatter<const void*>::format(ptr.raw(), ctx);
    }
};

/**
 * @struct formatter<loom::address_format>
 * @brief Formatter specialization for address_format enum.
 */
template <>
struct std::formatter<loom::address_format> : std::formatter<std::string_view> {
    /**
     * @brief Formats the address_format as its string name.
     * @tparam FormatContext The format context type.
     * @param fmt The address format to format.
     * @param ctx The format context.
     * @return Iterator past the formatted output.
     */
    template <typename FormatContext>
    auto format(loom::address_format fmt, FormatContext& ctx) const {
        std::string_view name = [fmt] {
            switch (fmt) {
                case loom::address_format::unspecified:
                    return "unspecified";
                case loom::address_format::inet:
                    return "inet";
                case loom::address_format::inet6:
                    return "inet6";
                case loom::address_format::ib:
                    return "ib";
                case loom::address_format::psmx:
                    return "psmx";
                case loom::address_format::psmx2:
                    return "psmx2";
                case loom::address_format::gni:
                    return "gni";
                case loom::address_format::bgq:
                    return "bgq";
                case loom::address_format::ethernet:
                    return "ethernet";
                case loom::address_format::str:
                    return "str";
                default:
                    return "unknown";
            }
        }();
        return std::formatter<std::string_view>::format(name, ctx);
    }
};

/**
 * @struct formatter<loom::atomic_op>
 * @brief Formatter specialization for atomic_op enum.
 */
template <>
struct std::formatter<loom::atomic_op> : std::formatter<std::string_view> {
    /**
     * @brief Formats the atomic_op as its string name.
     * @tparam FormatContext The format context type.
     * @param op The atomic operation to format.
     * @param ctx The format context.
     * @return Iterator past the formatted output.
     */
    template <typename FormatContext>
    auto format(loom::atomic_op op, FormatContext& ctx) const {
        std::string_view name = [op] {
            switch (op) {
                case loom::atomic_op::min:
                    return "min";
                case loom::atomic_op::max:
                    return "max";
                case loom::atomic_op::sum:
                    return "sum";
                case loom::atomic_op::prod:
                    return "prod";
                case loom::atomic_op::lor:
                    return "lor";
                case loom::atomic_op::land:
                    return "land";
                case loom::atomic_op::bor:
                    return "bor";
                case loom::atomic_op::band:
                    return "band";
                case loom::atomic_op::lxor:
                    return "lxor";
                case loom::atomic_op::bxor:
                    return "bxor";
                case loom::atomic_op::read:
                    return "read";
                case loom::atomic_op::write:
                    return "write";
                case loom::atomic_op::cswap:
                    return "cswap";
                case loom::atomic_op::cswap_ne:
                    return "cswap_ne";
                case loom::atomic_op::cswap_le:
                    return "cswap_le";
                case loom::atomic_op::cswap_lt:
                    return "cswap_lt";
                case loom::atomic_op::cswap_ge:
                    return "cswap_ge";
                case loom::atomic_op::cswap_gt:
                    return "cswap_gt";
                case loom::atomic_op::mswap:
                    return "mswap";
                default:
                    return "unknown";
            }
        }();
        return std::formatter<std::string_view>::format(name, ctx);
    }
};

/**
 * @struct formatter<loom::atomic_datatype>
 * @brief Formatter specialization for atomic_datatype enum.
 */
template <>
struct std::formatter<loom::atomic_datatype> : std::formatter<std::string_view> {
    /**
     * @brief Formats the atomic_datatype as its string name.
     * @tparam FormatContext The format context type.
     * @param dt The atomic datatype to format.
     * @param ctx The format context.
     * @return Iterator past the formatted output.
     */
    template <typename FormatContext>
    auto format(loom::atomic_datatype dt, FormatContext& ctx) const {
        std::string_view name = [dt] {
            switch (dt) {
                case loom::atomic_datatype::int8:
                    return "int8";
                case loom::atomic_datatype::uint8:
                    return "uint8";
                case loom::atomic_datatype::int16:
                    return "int16";
                case loom::atomic_datatype::uint16:
                    return "uint16";
                case loom::atomic_datatype::int32:
                    return "int32";
                case loom::atomic_datatype::uint32:
                    return "uint32";
                case loom::atomic_datatype::int64:
                    return "int64";
                case loom::atomic_datatype::uint64:
                    return "uint64";
                case loom::atomic_datatype::float32:
                    return "float32";
                case loom::atomic_datatype::float64:
                    return "float64";
                case loom::atomic_datatype::float128:
                    return "float128";
                case loom::atomic_datatype::float_complex:
                    return "float_complex";
                case loom::atomic_datatype::double_complex:
                    return "double_complex";
                case loom::atomic_datatype::long_double_complex:
                    return "long_double_complex";
                default:
                    return "unknown";
            }
        }();
        return std::formatter<std::string_view>::format(name, ctx);
    }
};

/**
 * @struct formatter<loom::threading_mode>
 * @brief Formatter specialization for threading_mode enum.
 */
template <>
struct std::formatter<loom::threading_mode> : std::formatter<std::string_view> {
    /**
     * @brief Formats the threading_mode as its string name.
     * @tparam FormatContext The format context type.
     * @param mode The threading mode to format.
     * @param ctx The format context.
     * @return Iterator past the formatted output.
     */
    template <typename FormatContext>
    auto format(loom::threading_mode mode, FormatContext& ctx) const {
        std::string_view name = [mode] {
            switch (mode) {
                case loom::threading_mode::unspecified:
                    return "unspecified";
                case loom::threading_mode::safe:
                    return "safe";
                case loom::threading_mode::fid:
                    return "fid";
                case loom::threading_mode::domain:
                    return "domain";
                case loom::threading_mode::completion:
                    return "completion";
                default:
                    return "unknown";
            }
        }();
        return std::formatter<std::string_view>::format(name, ctx);
    }
};

/**
 * @struct formatter<loom::progress_mode>
 * @brief Formatter specialization for progress_mode enum.
 */
template <>
struct std::formatter<loom::progress_mode> : std::formatter<std::string_view> {
    /**
     * @brief Formats the progress_mode as its string name.
     * @tparam FormatContext The format context type.
     * @param mode The progress mode to format.
     * @param ctx The format context.
     * @return Iterator past the formatted output.
     */
    template <typename FormatContext>
    auto format(loom::progress_mode mode, FormatContext& ctx) const {
        std::string_view name = [mode] {
            switch (mode) {
                case loom::progress_mode::unspecified:
                    return "unspecified";
                case loom::progress_mode::auto_progress:
                    return "auto";
                case loom::progress_mode::manual:
                    return "manual";
                case loom::progress_mode::manual_all:
                    return "manual_all";
                default:
                    return "unknown";
            }
        }();
        return std::formatter<std::string_view>::format(name, ctx);
    }
};

/**
 * @struct formatter<loom::errc>
 * @brief Formatter specialization for errc error codes.
 */
template <>
struct std::formatter<loom::errc> : std::formatter<std::string_view> {
    /**
     * @brief Formats the errc as its string name.
     * @tparam FormatContext The format context type.
     * @param e The error code to format.
     * @param ctx The format context.
     * @return Iterator past the formatted output.
     */
    template <typename FormatContext>
    auto format(loom::errc e, FormatContext& ctx) const {
        std::string_view name = [e] {
            switch (e) {
                case loom::errc::success:
                    return "success";
                case loom::errc::no_data:
                    return "no_data";
                case loom::errc::message_too_long:
                    return "message_too_long";
                case loom::errc::no_space:
                    return "no_space";
                case loom::errc::again:
                    return "again";
                case loom::errc::invalid_argument:
                    return "invalid_argument";
                case loom::errc::io_error:
                    return "io_error";
                case loom::errc::not_supported:
                    return "not_supported";
                case loom::errc::busy:
                    return "busy";
                case loom::errc::canceled:
                    return "canceled";
                case loom::errc::no_memory:
                    return "no_memory";
                case loom::errc::already:
                    return "already";
                case loom::errc::bad_flags:
                    return "bad_flags";
                case loom::errc::no_entry:
                    return "no_entry";
                case loom::errc::not_connected:
                    return "not_connected";
                case loom::errc::address_in_use:
                    return "address_in_use";
                case loom::errc::connection_refused:
                    return "connection_refused";
                case loom::errc::address_not_available:
                    return "address_not_available";
                case loom::errc::timeout:
                    return "timeout";
                default:
                    return "unknown";
            }
        }();
        return std::formatter<std::string_view>::format(name, ctx);
    }
};

/**
 * @struct formatter<loom::hmem_iface>
 * @brief Formatter specialization for hmem_iface enum.
 */
template <>
struct std::formatter<loom::hmem_iface> : std::formatter<std::string_view> {
    /**
     * @brief Formats the hmem_iface as its string name.
     * @tparam FormatContext The format context type.
     * @param iface The heterogeneous memory interface to format.
     * @param ctx The format context.
     * @return Iterator past the formatted output.
     */
    template <typename FormatContext>
    auto format(loom::hmem_iface iface, FormatContext& ctx) const {
        std::string_view name = [iface] {
            switch (iface) {
                case loom::hmem_iface::system:
                    return "system";
                case loom::hmem_iface::cuda:
                    return "cuda";
                case loom::hmem_iface::rocr:
                    return "rocr";
                case loom::hmem_iface::level_zero:
                    return "level_zero";
                case loom::hmem_iface::neuron:
                    return "neuron";
                case loom::hmem_iface::synapseai:
                    return "synapseai";
                default:
                    return "unknown";
            }
        }();
        return std::formatter<std::string_view>::format(name, ctx);
    }
};

/**
 * @struct formatter<loom::ipv4_address>
 * @brief Formatter specialization for IPv4 addresses.
 */
template <>
struct std::formatter<loom::ipv4_address> {
    /**
     * @brief Parses the format specification.
     * @param ctx The parse context.
     * @return Iterator to end of format spec.
     */
    constexpr auto parse(std::format_parse_context& ctx) { return ctx.begin(); }

    /**
     * @brief Formats the IPv4 address as dotted decimal notation.
     * @tparam FormatContext The format context type.
     * @param addr The IPv4 address to format.
     * @param ctx The format context.
     * @return Iterator past the formatted output.
     */
    template <typename FormatContext>
    auto format(const loom::ipv4_address& addr, FormatContext& ctx) const {
        auto octets = addr.octets();
        if (addr.port() != 0) {
            return std::format_to(ctx.out(),
                                  "{}.{}.{}.{}:{}",
                                  octets[0],
                                  octets[1],
                                  octets[2],
                                  octets[3],
                                  addr.port());
        }
        return std::format_to(ctx.out(), "{}.{}.{}.{}", octets[0], octets[1], octets[2], octets[3]);
    }
};

/**
 * @struct formatter<loom::ipv6_address>
 * @brief Formatter specialization for IPv6 addresses.
 */
template <>
struct std::formatter<loom::ipv6_address> {
    /**
     * @brief Parses the format specification.
     * @param ctx The parse context.
     * @return Iterator to end of format spec.
     */
    constexpr auto parse(std::format_parse_context& ctx) { return ctx.begin(); }

    /**
     * @brief Formats the IPv6 address as colon-separated hex segments.
     * @tparam FormatContext The format context type.
     * @param addr The IPv6 address to format.
     * @param ctx The format context.
     * @return Iterator past the formatted output.
     */
    template <typename FormatContext>
    auto format(const loom::ipv6_address& addr, FormatContext& ctx) const {
        auto segs = addr.segments();
        if (addr.port() != 0) {
            return std::format_to(ctx.out(),
                                  "[{:x}:{:x}:{:x}:{:x}:{:x}:{:x}:{:x}:{:x}]:{}",
                                  segs[0],
                                  segs[1],
                                  segs[2],
                                  segs[3],
                                  segs[4],
                                  segs[5],
                                  segs[6],
                                  segs[7],
                                  addr.port());
        }
        return std::format_to(ctx.out(),
                              "{:x}:{:x}:{:x}:{:x}:{:x}:{:x}:{:x}:{:x}",
                              segs[0],
                              segs[1],
                              segs[2],
                              segs[3],
                              segs[4],
                              segs[5],
                              segs[6],
                              segs[7]);
    }
};

/**
 * @struct formatter<loom::ib_address>
 * @brief Formatter specialization for InfiniBand addresses.
 */
template <>
struct std::formatter<loom::ib_address> {
    /**
     * @brief Parses the format specification.
     * @param ctx The parse context.
     * @return Iterator to end of format spec.
     */
    constexpr auto parse(std::format_parse_context& ctx) { return ctx.begin(); }

    /**
     * @brief Formats the IB address with LID, QPN, and GID prefix.
     * @tparam FormatContext The format context type.
     * @param addr The InfiniBand address to format.
     * @param ctx The format context.
     * @return Iterator past the formatted output.
     */
    template <typename FormatContext>
    auto format(const loom::ib_address& addr, FormatContext& ctx) const {
        auto gid = addr.gid();
        return std::format_to(ctx.out(),
                              "ib[lid={},qpn={},gid={:02x}:{:02x}:{:02x}:{:02x}:...]",
                              addr.lid(),
                              addr.qpn(),
                              gid[0],
                              gid[1],
                              gid[2],
                              gid[3]);
    }
};

/**
 * @struct formatter<loom::ethernet_address>
 * @brief Formatter specialization for Ethernet MAC addresses.
 */
template <>
struct std::formatter<loom::ethernet_address> {
    /**
     * @brief Parses the format specification.
     * @param ctx The parse context.
     * @return Iterator to end of format spec.
     */
    constexpr auto parse(std::format_parse_context& ctx) { return ctx.begin(); }

    /**
     * @brief Formats the Ethernet address as colon-separated hex bytes.
     * @tparam FormatContext The format context type.
     * @param addr The Ethernet address to format.
     * @param ctx The format context.
     * @return Iterator past the formatted output.
     */
    template <typename FormatContext>
    auto format(const loom::ethernet_address& addr, FormatContext& ctx) const {
        auto mac = addr.mac();
        return std::format_to(ctx.out(),
                              "{:02x}:{:02x}:{:02x}:{:02x}:{:02x}:{:02x}",
                              mac[0],
                              mac[1],
                              mac[2],
                              mac[3],
                              mac[4],
                              mac[5]);
    }
};

/**
 * @struct formatter<loom::unspecified_address>
 * @brief Formatter specialization for unspecified addresses.
 */
template <>
struct std::formatter<loom::unspecified_address> : std::formatter<std::string_view> {
    /**
     * @brief Formats the unspecified address as a placeholder string.
     * @tparam FormatContext The format context type.
     * @param ctx The format context.
     * @return Iterator past the formatted output.
     */
    template <typename FormatContext>
    auto format(const loom::unspecified_address&, FormatContext& ctx) const {
        return std::formatter<std::string_view>::format("<unspecified>", ctx);
    }
};

/**
 * @struct formatter<loom::address>
 * @brief Formatter specialization for the address variant type.
 */
template <>
struct std::formatter<loom::address> {
    /**
     * @brief Parses the format specification.
     * @param ctx The parse context.
     * @return Iterator to end of format spec.
     */
    constexpr auto parse(std::format_parse_context& ctx) { return ctx.begin(); }

    /**
     * @brief Formats the address by visiting the variant and formatting the contained type.
     * @tparam FormatContext The format context type.
     * @param addr The address variant to format.
     * @param ctx The format context.
     * @return Iterator past the formatted output.
     */
    template <typename FormatContext>
    auto format(const loom::address& addr, FormatContext& ctx) const {
        return std::visit([&ctx](const auto& a) { return std::format_to(ctx.out(), "{}", a); },
                          addr);
    }
};

/**
 * @struct formatter<loom::remote_memory>
 * @brief Formatter specialization for remote memory descriptors.
 */
template <>
struct std::formatter<loom::remote_memory> {
    /**
     * @brief Parses the format specification.
     * @param ctx The parse context.
     * @return Iterator to end of format spec.
     */
    constexpr auto parse(std::format_parse_context& ctx) { return ctx.begin(); }

    /**
     * @brief Formats the remote memory as address, key, and length.
     * @tparam FormatContext The format context type.
     * @param rm The remote memory descriptor to format.
     * @param ctx The format context.
     * @return Iterator past the formatted output.
     */
    template <typename FormatContext>
    auto format(const loom::remote_memory& rm, FormatContext& ctx) const {
        return std::format_to(ctx.out(),
                              "remote_memory{{addr=0x{:x}, key=0x{:x}, len={}}}",
                              rm.addr,
                              rm.key,
                              rm.length);
    }
};

/**
 * @struct formatter<loom::endpoint_type>
 * @brief Formatter specialization for endpoint types.
 */
template <>
struct std::formatter<loom::endpoint_type> {
    /**
     * @brief Parses the format specification.
     * @param ctx The parse context.
     * @return Iterator to end of format spec.
     */
    constexpr auto parse(std::format_parse_context& ctx) { return ctx.begin(); }

    /**
     * @brief Formats the endpoint type using its type name.
     * @tparam FormatContext The format context type.
     * @param type The endpoint type to format.
     * @param ctx The format context.
     * @return Iterator past the formatted output.
     */
    template <typename FormatContext>
    auto format(const loom::endpoint_type& type, FormatContext& ctx) const {
        return std::format_to(ctx.out(), "{}", loom::type_name(type));
    }
};

/**
 * @struct formatter<loom::msg_endpoint_tag>
 * @brief Formatter specialization for message endpoint tag.
 */
template <>
struct std::formatter<loom::msg_endpoint_tag> : std::formatter<std::string_view> {
    /**
     * @brief Formats the message endpoint tag as "message".
     * @tparam FormatContext The format context type.
     * @param ctx The format context.
     * @return Iterator past the formatted output.
     */
    template <typename FormatContext>
    auto format(const loom::msg_endpoint_tag&, FormatContext& ctx) const {
        return std::formatter<std::string_view>::format("message", ctx);
    }
};

/**
 * @struct formatter<loom::rdm_endpoint_tag>
 * @brief Formatter specialization for reliable datagram endpoint tag.
 */
template <>
struct std::formatter<loom::rdm_endpoint_tag> : std::formatter<std::string_view> {
    /**
     * @brief Formats the RDM endpoint tag as "reliable_datagram".
     * @tparam FormatContext The format context type.
     * @param ctx The format context.
     * @return Iterator past the formatted output.
     */
    template <typename FormatContext>
    auto format(const loom::rdm_endpoint_tag&, FormatContext& ctx) const {
        return std::formatter<std::string_view>::format("reliable_datagram", ctx);
    }
};

/**
 * @struct formatter<loom::dgram_endpoint_tag>
 * @brief Formatter specialization for datagram endpoint tag.
 */
template <>
struct std::formatter<loom::dgram_endpoint_tag> : std::formatter<std::string_view> {
    /**
     * @brief Formats the datagram endpoint tag as "datagram".
     * @tparam FormatContext The format context type.
     * @param ctx The format context.
     * @return Iterator past the formatted output.
     */
    template <typename FormatContext>
    auto format(const loom::dgram_endpoint_tag&, FormatContext& ctx) const {
        return std::formatter<std::string_view>::format("datagram", ctx);
    }
};

/**
 * @struct formatter<loom::result>
 * @brief Formatter specialization for result types.
 * @tparam T The value type of the result.
 */
template <typename T>
struct std::formatter<loom::result<T>> {
    /**
     * @brief Parses the format specification.
     * @param ctx The parse context.
     * @return Iterator to end of format spec.
     */
    constexpr auto parse(std::format_parse_context& ctx) { return ctx.begin(); }

    /**
     * @brief Formats the result as "ok" or "ok(value)" on success, "error(message)" on failure.
     * @tparam FormatContext The format context type.
     * @param res The result to format.
     * @param ctx The format context.
     * @return Iterator past the formatted output.
     */
    template <typename FormatContext>
    auto format(const loom::result<T>& res, FormatContext& ctx) const {
        if (res.has_value()) {
            if constexpr (std::is_void_v<T>) {
                return std::format_to(ctx.out(), "ok");
            } else {
                return std::format_to(ctx.out(), "ok({})", *res);
            }
        }
        return std::format_to(ctx.out(), "error({})", res.error().message());
    }
};

/**
 * @struct formatter<std::error_code>
 * @brief Formatter specialization for std::error_code.
 */
template <>
struct std::formatter<std::error_code> {
    /**
     * @brief Parses the format specification.
     * @param ctx The parse context.
     * @return Iterator to end of format spec.
     */
    constexpr auto parse(std::format_parse_context& ctx) { return ctx.begin(); }

    /**
     * @brief Formats the error code as "category:message".
     * @tparam FormatContext The format context type.
     * @param ec The error code to format.
     * @param ctx The format context.
     * @return Iterator past the formatted output.
     */
    template <typename FormatContext>
    auto format(const std::error_code& ec, FormatContext& ctx) const {
        return std::format_to(ctx.out(), "{}:{}", ec.category().name(), ec.message());
    }
};
