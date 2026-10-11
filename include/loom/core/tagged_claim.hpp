// SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only
#pragma once

#include <cstdint>
#include <exception>
#include <optional>
#include <span>
#include <utility>

#include "loom/async/completion_queue.hpp"
#include "loom/core/messaging.hpp"

namespace loom::msg {

enum class tagged_claim_state { idle, probing, reserved, receiving, discarding };
enum class tagged_claim_result { no_match, reserved, received, discarded };

/// Owns stable native context storage for one tagged message reservation.
///
/// The endpoint wrapper must outlive this object and must not be moved or
/// replaced while it is borrowed. Do not access this object concurrently.
/// Serialize endpoint access and progress as required by its threading mode.
/// Feed each native completion for context() to observe() exactly once.
///
/// Successful probe submission is asynchronous. A reserved result must be
/// followed by receive() or discard(), then its terminal completion. Failed
/// submissions retain the previous state, including a live reservation.
/// Buffers and registrations remain borrowed until receive completion or
/// successful endpoint close; cancellation acceptance does not retire them.
///
/// Destruction with outstanding work or a reservation terminates unless the
/// borrowed endpoint has been successfully closed. There is no implicit
/// discard or blocking progress. After close, retire_after_close() releases
/// the local state even if the provider discarded its CQ entries.
class tagged_claim {
public:
    explicit tagged_claim(endpoint& ep) noexcept : endpoint_(ep) {}

    tagged_claim(const tagged_claim&) = delete;
    auto operator=(const tagged_claim&) -> tagged_claim& = delete;
    tagged_claim(tagged_claim&&) = delete;
    auto operator=(tagged_claim&&) -> tagged_claim& = delete;

    ~tagged_claim() {
        if (state_ != tagged_claim_state::idle && endpoint_) {
            std::terminate();
        }
    }

    [[nodiscard]] auto state() const noexcept -> tagged_claim_state { return state_; }

    /// For routing CQ events and native cancellation, not separate submissions.
    [[nodiscard]] auto context() noexcept -> context_ptr<void> {
        return context_ptr<void>{&context_};
    }

    /// Full metadata from the last observed completion, including error details.
    [[nodiscard]] auto completion() const noexcept -> const std::optional<completion_event>& {
        return completion_;
    }

    [[nodiscard]] auto probe(std::uint64_t tag,
                             std::uint64_t ignore = 0,
                             fabric_addr source = fabric_addrs::unspecified) -> result<void> {
        if (state_ != tagged_claim_state::idle) {
            return make_error_result<void>(errc::busy);
        }
        message_ = {.msg_iov = nullptr,
                    .desc = nullptr,
                    .iov_count = 0,
                    .addr = source.get(),
                    .tag = tag,
                    .ignore = ignore,
                    .context = &context_,
                    .data = 0};
        return submit(tagged_claim_state::probing, FI_PEEK | FI_CLAIM | FI_COMPLETION);
    }

    /// Consume the reservation into a borrowed buffer. Zero-length receives are
    /// permitted; a message exceeding the buffer completes with truncation.
    [[nodiscard]] auto receive(std::span<std::byte> buffer, mr_descriptor descriptor = {})
        -> result<void> {
        if (state_ != tagged_claim_state::reserved) {
            return make_error_result<void>(errc::invalid_argument);
        }
        iov_ = {buffer.data(), buffer.size()};
        descriptor_ = descriptor.raw();
        message_.msg_iov = &iov_;
        message_.iov_count = 1;
        message_.desc = descriptor_ ? &descriptor_ : nullptr;
        return submit(tagged_claim_state::receiving, FI_CLAIM | FI_COMPLETION);
    }

    [[nodiscard]] auto discard() -> result<void> {
        if (state_ != tagged_claim_state::reserved) {
            return make_error_result<void>(errc::invalid_argument);
        }
        message_.msg_iov = nullptr;
        message_.iov_count = 0;
        message_.desc = nullptr;
        return submit(tagged_claim_state::discarding, FI_CLAIM | FI_DISCARD | FI_COMPLETION);
    }

    /// No-match is ordinary probe output. Other errors, including no-message on
    /// a claim receive, remain errors. A terminal CQ event retires the submitted
    /// operation; only a successful probe leaves a reservation behind.
    [[nodiscard]] auto observe(const completion_event& event) -> result<tagged_claim_result> {
        if (event.context.get() != context().get() || state_ == tagged_claim_state::idle ||
            state_ == tagged_claim_state::reserved) {
            return make_error_result<tagged_claim_result>(errc::invalid_argument);
        }
        const auto previous = state_;
        completion_ = event;
        state_ = tagged_claim_state::idle;
        if (event.has_error()) {
            if (previous == tagged_claim_state::probing && event.error == errc::no_message) {
                return tagged_claim_result::no_match;
            }
            return std::unexpected(event.error);
        }
        switch (previous) {
            case tagged_claim_state::probing:
                state_ = tagged_claim_state::reserved;
                return tagged_claim_result::reserved;
            case tagged_claim_state::receiving:
                return tagged_claim_result::received;
            case tagged_claim_state::discarding:
                return tagged_claim_result::discarded;
            default:
                std::unreachable();
        }
    }

    /// Successful endpoint close guarantees that native context use has ended.
    /// Failed close leaves the endpoint open and cannot retire this object.
    [[nodiscard]] auto retire_after_close() -> result<void> {
        if (endpoint_) {
            return make_error_result<void>(errc::busy);
        }
        state_ = tagged_claim_state::idle;
        return make_success();
    }

private:
    endpoint& endpoint_;
    ::fi_context2 context_{};
    ::iovec iov_{};
    void* descriptor_{};
    ::fi_msg_tagged message_{};
    tagged_claim_state state_{tagged_claim_state::idle};
    std::optional<completion_event> completion_;

    [[nodiscard]] auto submit(tagged_claim_state next, std::uint64_t flags) -> result<void> {
        auto* ep = static_cast<::fid_ep*>(endpoint_.impl_internal_ptr());
        if (!ep) {
            return make_error_result<void>(errc::invalid_argument);
        }
        if (!FI_CHECK_OP(ep->tagged, fi_ops_tagged, recvmsg)) {
            return make_error_result<void>(errc::not_supported);
        }
        // Explicit flags replace endpoint defaults and always request a CQ entry.
        auto result = detail::submit_tagged_receive(*ep, message_, flags);
        if (result) {
            state_ = next;
        }
        return result;
    }
};

}  // namespace loom::msg
