// SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only
#pragma once

#include <atomic>
#include <exception>
#include <thread>

#include "loom/asio/asio_context.hpp"
#include "loom/core/messaging.hpp"

namespace loom::asio::detail {

// Stack-owned native state stays at a stable address through retirement, even
// when a different CQ reader dispatches its completion. No executor is needed.
class blocking_context {
public:
    explicit blocking_context(std::shared_ptr<endpoint_state> endpoint)
        : endpoint_(std::move(endpoint)) {
        endpoint_->add(context());
    }
    ~blocking_context() { endpoint_->remove(context()); }
    blocking_context(const blocking_context&) = delete;
    auto operator=(const blocking_context&) -> blocking_context& = delete;

    [[nodiscard]] auto context() noexcept -> void* { return &native_; }
    [[nodiscard]] auto completed() const noexcept -> bool {
        return completed_.load(std::memory_order_acquire);
    }
    [[nodiscard]] auto bind(completion_direction direction) -> void_result {
        return registration_.bind(endpoint_->endpoint,
                                  direction,
                                  context_ptr<void>{context()},
                                  this,
                                  [](void* object, const completion_event& event) noexcept {
                                      auto& self = *static_cast<blocking_context*>(object);
                                      self.event = event;
                                      // The waiting thread may destroy the context immediately.
                                      self.completed_.store(true, std::memory_order_release);
                                  });
    }
    void request_cancel() { (void)endpoint_->endpoint.cancel(context_ptr<void>{context()}); }

    asio_buffer_storage<> storage;
    completion_event event;

private:
    std::shared_ptr<endpoint_state> endpoint_;
    fi_context2 native_{};
    completion_registration registration_;
    std::atomic<bool> completed_{};
};

template <bool Receive, typename BufferSequence>
auto blocking_message(std::shared_ptr<endpoint_state> endpoint, const BufferSequence& buffers)
    -> std::pair<std::error_code, std::size_t> {
    constexpr auto direction =
        Receive ? completion_direction::receive : completion_direction::transmit;
    auto& ep = endpoint->endpoint;
    const auto* queue = ep.bound_cq(direction);
    if (!ep || !queue)
        return {make_error_code(errc::invalid_argument), 0};
    if constexpr (Receive) {
        if (queue->format() == cq_format::context ||
            (ep.impl_operation_flags(direction) & FI_MULTI_RECV))
            return {make_error_code(errc::not_supported), 0};
    }
    blocking_context operation{std::move(endpoint)};
    operation.storage.template prepare_buffers<Receive>(buffers, std::nullopt);
    const auto prepared = operation.storage.buffers();
    auto submitted = operation.bind(direction);
    if (submitted) {
        if constexpr (Receive) {
            const msg::recv_message<> message{
                .iov = prepared.iov, .desc = prepared.descriptors, .context = operation.context()};
            submitted = msg::recvmsg<operation_defaults::inherit_without_more>(
                ep, message, msg::recv_flag::completion);
        } else {
            const msg::send_message<> message{
                .iov = prepared.iov, .desc = prepared.descriptors, .context = operation.context()};
            submitted = msg::sendmsg<operation_defaults::inherit_without_more>(
                ep, message, msg::send_flag::completion);
        }
    }
    if (!submitted)
        return {submitted.error(), 0};

    std::error_code read_error;
    std::exception_ptr exception;
    while (!operation.completed()) {
        try {
            auto event = queue->read_for(context_ptr<void>{operation.context()});
            if (event) {
                (void)queue->dispatch(*event);
            } else if (event.error() != errc::again && !read_error) {
                read_error = event.error();
                operation.request_cancel();
            }
        } catch (...) {
            // Metadata allocation precedes CQ consumption. Cancellation alone
            // does not release native ownership of the buffer or the context.
            if (!exception) {
                exception = std::current_exception();
                operation.request_cancel();
            }
        }
        if (!operation.completed())
            std::this_thread::yield();
    }
    if (exception)
        std::rethrow_exception(exception);
    const auto error = read_error ? read_error : operation.event.error;
    const auto bytes = Receive ? operation.event.bytes_transferred : (error ? 0 : prepared.bytes);
    return {error, bytes};
}

}  // namespace loom::asio::detail
