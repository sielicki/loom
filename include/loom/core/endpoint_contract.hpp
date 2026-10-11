// SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only
#pragma once

#include <concepts>
#include <cstddef>
#include <span>
#include <type_traits>
#include <utility>

#include "loom/core/domain.hpp"
#include "loom/core/endpoint.hpp"
#include "loom/core/fabric.hpp"
#include "loom/detail/conversions.hpp"

namespace loom {

/// Requirements chosen by the application, independent of provider identity.
/// Derive a profile to request additional caps or exact domain policies.
/// Unspecified domain policies impose no requirement. Supported modes declare
/// obligations the caller can fulfill, not capabilities requested of a provider.
template <endpoint_tag Endpoint, std::size_t InjectBound = 0>
struct message_endpoint_profile {
    static constexpr endpoint_type ep_type{Endpoint{}};
    static constexpr caps required_caps = capability::msg | capability::send | capability::recv;
    static constexpr caps optional_caps{};
    static constexpr mode supported_modes{};
    static constexpr std::size_t inject_size = InjectBound;
    static constexpr threading_mode threading = threading_mode::unspecified;
    static constexpr progress_mode control_progress = progress_mode::unspecified;
    static constexpr progress_mode data_progress = progress_mode::unspecified;
};

/// All requirements must be typed constant expressions. Injection requires
/// untagged send capability; unsupported public bits cannot establish a proof.
template <typename T>
concept endpoint_profile = requires {
    { T::ep_type } -> std::same_as<const endpoint_type&>;
    { T::required_caps } -> std::same_as<const caps&>;
    { T::supported_modes } -> std::same_as<const mode&>;
    { T::inject_size } -> std::same_as<const std::size_t&>;
    { T::threading } -> std::same_as<const threading_mode&>;
    { T::control_progress } -> std::same_as<const progress_mode&>;
    { T::data_progress } -> std::same_as<const progress_mode&>;
    typename std::integral_constant<std::size_t, T::ep_type.index()>;
    typename std::integral_constant<std::size_t, T::inject_size>;
    typename std::integral_constant<threading_mode, T::threading>;
    typename std::integral_constant<progress_mode, T::control_progress>;
    typename std::integral_constant<progress_mode, T::data_progress>;
    requires(detail::from_fi_caps(detail::to_fi_caps(T::required_caps)) == T::required_caps);
    requires(detail::from_fi_mode(detail::to_fi_mode(T::supported_modes)) == T::supported_modes);
    requires(T::inject_size == 0 || T::required_caps.has(capability::msg | capability::send));
};

/// An endpoint whose selected configuration satisfies Profile.
/// Native-handle mutations must preserve the established contract.
template <endpoint_profile Profile>
class contract_endpoint {
public:
    using profile_type = Profile;
    static constexpr std::size_t inject_bound = Profile::inject_size;

    [[nodiscard]] static auto
    create(const domain& dom, const fabric_info& info, memory_resource* resource = nullptr)
        -> result<contract_endpoint> {
        if (!dom || !info) {
            return make_error_result<contract_endpoint>(errc::invalid_argument);
        }
        const auto* native = static_cast<const ::fi_info*>(info.internal_ptr());
        if (!native->ep_attr) {
            return make_error_result<contract_endpoint>(errc::invalid_argument);
        }
        // Validate the native enum and mode bits before lossy conversions can
        // turn an unknown value into a known type or erase an obligation.
        const auto modes = native->mode | (native->tx_attr ? native->tx_attr->mode : 0) |
                           (native->rx_attr ? native->rx_attr->mode : 0);
        if (native->ep_attr->type != detail::to_fi_ep_type(Profile::ep_type) ||
            !info.get_caps().has(Profile::required_caps) ||
            (modes & ~detail::to_fi_mode(Profile::supported_modes)) != 0 ||
            native->ep_attr->max_msg_size < inject_bound) {
            return make_error_result<contract_endpoint>(errc::not_supported);
        }
        // Domain requirements are checked against the opened domain, rather
        // than attributes from a possibly different discovery entry.
        if constexpr (Profile::threading != threading_mode::unspecified) {
            if (dom.get_threading() != Profile::threading)
                return make_error_result<contract_endpoint>(errc::not_supported);
        }
        if constexpr (Profile::control_progress != progress_mode::unspecified) {
            if (dom.get_control_progress() != Profile::control_progress)
                return make_error_result<contract_endpoint>(errc::not_supported);
        }
        if constexpr (Profile::data_progress != progress_mode::unspecified) {
            if (dom.get_data_progress() != Profile::data_progress)
                return make_error_result<contract_endpoint>(errc::not_supported);
        }
        auto ep = endpoint::create(dom, info, resource);
        if (!ep) {
            return make_error_result<contract_endpoint>(ep.error());
        }
        contract_endpoint checked;
        checked.ep_ = std::move(*ep);
        if (auto valid = checked.validate_inject_bound(); !valid) {
            return make_error_result<contract_endpoint>(valid.error());
        }
        return checked;
    }

    /**
     * @brief Default constructor.
     */
    contract_endpoint() = default;
    /**
     * @brief Destructor.
     */
    ~contract_endpoint() = default;
    /**
     * @brief Move constructor.
     */
    contract_endpoint(contract_endpoint&&) noexcept = default;
    /**
     * @brief Move assignment operator.
     */
    auto operator=(contract_endpoint&&) noexcept -> contract_endpoint& = default;
    /**
     * @brief Deleted copy constructor.
     */
    contract_endpoint(const contract_endpoint&) = delete;
    /**
     * @brief Deleted copy assignment operator.
     */
    auto operator=(const contract_endpoint&) -> contract_endpoint& = delete;

    /**
     * @brief Enables the endpoint for communication.
     * @return Success or error result.
     */
    [[nodiscard]] auto enable() -> void_result {
        return revalidate_after([this] { return ep_.enable(); });
    }

    /// Retain the native endpoint on close failure, disabling new submissions.
    [[nodiscard]] auto close() noexcept -> void_result {
        contract_valid_ = false;
        return ep_.close();
    }

    /**
     * @brief Sends data on the endpoint.
     * @param data Data to send.
     * @param ctx Optional context pointer.
     * @return Success or error result.
     */
    [[nodiscard]] auto send(std::span<const std::byte> data, context_ptr<void> ctx = {})
        -> void_result
        requires(Profile::required_caps.has(capability::msg | capability::send))
    {
        if (!*this) {
            return make_error_result<void>(errc::invalid_argument);
        }
        return ep_.send(data, ctx);
    }

    /**
     * @brief Receives data on the endpoint.
     * @param buffer Buffer to receive into.
     * @param ctx Optional context pointer.
     * @return Success or error result.
     */
    [[nodiscard]] auto recv(std::span<std::byte> buffer, context_ptr<void> ctx = {}) -> void_result
        requires(Profile::required_caps.has(capability::msg | capability::recv))
    {
        if (!*this) {
            return make_error_result<void>(errc::invalid_argument);
        }
        return ep_.recv(buffer, ctx);
    }

    /**
     * @brief Injects a small message.
     * @param data Data to inject.
     * @param dest_addr Destination address.
     * @return Success or error result.
     */
    [[nodiscard]] auto inject(std::span<const std::byte> data,
                              fabric_addr dest_addr = fabric_addrs::unspecified) -> void_result
        requires(inject_bound != 0)
    {
        if (data.size() > inject_bound) {
            return make_error_result<void>(errc::message_too_long);
        }
        return submit_inject(data, dest_addr);
    }

    /// Fixed extents establish the per-message bound without a runtime size check.
    template <typename Byte, std::size_t Extent>
        requires((std::same_as<Byte, std::byte> || std::same_as<Byte, const std::byte>) &&
                 Extent != std::dynamic_extent && Extent <= inject_bound && inject_bound != 0)
    [[nodiscard]] auto inject(std::span<Byte, Extent> data,
                              fabric_addr dest_addr = fabric_addrs::unspecified) -> void_result {
        return submit_inject(data, dest_addr);
    }

    // Prevent an oversized fixed span from falling back to dynamic conversion.
    template <typename Byte, std::size_t Extent>
        requires((std::same_as<Byte, std::byte> || std::same_as<Byte, const std::byte>) &&
                 Extent != std::dynamic_extent && Extent > inject_bound)
    auto inject(std::span<Byte, Extent>, fabric_addr = fabric_addrs::unspecified)
        -> void_result = delete;

    /**
     * @brief Checks if a message size can be injected.
     * @param size Size to check.
     * @return True if the size can be injected.
     */
    [[nodiscard]] static constexpr auto can_inject(std::size_t size) noexcept -> bool {
        return inject_bound != 0 && size <= inject_bound;
    }

    /**
     * @brief Gets the chosen, checked inject bound for this endpoint type.
     * @return Maximum inject size in bytes.
     */
    [[nodiscard]] static constexpr auto max_inject_size() noexcept -> std::size_t {
        return inject_bound;
    }

    /**
     * @brief Cancels an operation.
     * @param ctx Context of the operation to cancel.
     * @return Success or error result.
     */
    [[nodiscard]] auto cancel(context_ptr<void> ctx) -> void_result { return ep_.cancel(ctx); }

    /**
     * @brief Gets the local address of the endpoint.
     * @return The local address or error.
     */
    [[nodiscard]] auto get_local_address() const -> result<address> {
        return ep_.get_local_address();
    }

    /**
     * @brief Gets the peer address of the endpoint.
     * @return The peer address or error.
     */
    [[nodiscard]] auto get_peer_address() const -> result<address> {
        return ep_.get_peer_address();
    }

    /**
     * @brief Binds a completion queue to the endpoint.
     * @param cq The completion queue.
     * @param flags Binding flags.
     * @return Success or error result.
     */
    [[nodiscard]] auto bind_cq(completion_queue& cq, cq_bind_flags flags) -> void_result {
        return revalidate_after([&] { return ep_.bind_cq(cq, flags); });
    }

    /**
     * @brief Binds a transmit completion queue.
     * @param cq The completion queue.
     * @return Success or error result.
     */
    [[nodiscard]] auto bind_tx_cq(completion_queue& cq) -> void_result {
        return revalidate_after([&] { return ep_.bind_tx_cq(cq); });
    }

    /**
     * @brief Binds a receive completion queue.
     * @param cq The completion queue.
     * @return Success or error result.
     */
    [[nodiscard]] auto bind_rx_cq(completion_queue& cq) -> void_result {
        return revalidate_after([&] { return ep_.bind_rx_cq(cq); });
    }

    /**
     * @brief Binds an event queue to the endpoint.
     * @param eq The event queue.
     * @param flags Binding flags.
     * @return Success or error result.
     */
    [[nodiscard]] auto bind_eq(event_queue& eq, std::uint64_t flags = 0) -> void_result {
        return revalidate_after([&] { return ep_.bind_eq(eq, flags); });
    }

    /**
     * @brief Binds an address vector to the endpoint.
     * @param av The address vector.
     * @param flags Binding flags.
     * @return Success or error result.
     */
    [[nodiscard]] auto bind_av(address_vector& av, std::uint64_t flags = 0) -> void_result {
        return revalidate_after([&] { return ep_.bind_av(av, flags); });
    }

    /**
     * @brief Binds a counter to the endpoint.
     * @param cntr The counter.
     * @param flags Binding flags.
     * @return Success or error result.
     */
    [[nodiscard]] auto bind_counter(counter& cntr, std::uint64_t flags = 0) -> void_result {
        return revalidate_after([&] { return ep_.bind_counter(cntr, flags); });
    }

    /// Const access cannot replace the endpoint or change its options.
    [[nodiscard]] auto underlying() const noexcept -> const endpoint& { return ep_; }

    /// Explicitly leave the checked contract to reconfigure or use the generic API.
    [[nodiscard]] auto release() && noexcept -> endpoint {
        contract_valid_ = false;
        return std::move(ep_);
    }

    [[nodiscard]] explicit operator bool() const noexcept {
        return contract_valid_ && static_cast<bool>(ep_);
    }

private:
    [[nodiscard]] auto revalidate_after(auto&& configure) -> void_result {
        contract_valid_ = false;
        if (auto configured = std::forward<decltype(configure)>(configure)(); !configured) {
            return configured;
        }
        return validate_inject_bound();
    }

    [[nodiscard]] auto validate_inject_bound() -> void_result {
        contract_valid_ = false;
        if constexpr (inject_bound != 0) {
            const auto limit = ep_.message_inject_size();
            if (!limit) {
                return make_error_result<void>(limit.error());
            }
            if (*limit < inject_bound) {
                return make_error_result<void>(errc::not_supported);
            }
        }
        contract_valid_ = true;
        return make_success();
    }

    [[nodiscard]] auto submit_inject(std::span<const std::byte> data, fabric_addr destination)
        -> void_result {
        if (!*this) {
            return make_error_result<void>(errc::invalid_argument);
        }
        return ep_.inject(data, destination);
    }

    endpoint ep_;
    bool contract_valid_{};
};

}  // namespace loom
