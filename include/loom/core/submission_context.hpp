// SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only
#pragma once

#include <rdma/fabric.h>

#include <concepts>
#include <coroutine>
#include <functional>
#include <future>
#include <system_error>
#include <type_traits>
#include <utility>
#include <variant>

namespace loom {

struct completion_event;

namespace detail {

template <typename T, typename Variant>
struct variant_index;

template <typename T, typename... Ts>
struct variant_index<T, std::variant<Ts...>> : std::integral_constant<std::size_t, [] {
    std::size_t idx = 0;
    ((std::same_as<T, Ts> ? false : (++idx, true)) && ...);
    return idx;
}()> {};

template <typename T, typename Variant>
inline constexpr std::size_t variant_index_v = variant_index<T, Variant>::value;

}  // namespace detail

template <typename T>
concept completion_receiver = requires(T& handler, completion_event& event) {
    { handler.set_value(event) } -> std::same_as<void>;
};

template <typename T>
concept error_receiver = requires(T& handler, std::error_code ec) {
    { handler.set_error(ec) } -> std::same_as<void>;
};

template <typename T>
concept stoppable_receiver = requires(T& handler) {
    { handler.set_stopped() } -> std::same_as<void>;
};

template <typename T>
concept full_receiver = completion_receiver<T> && error_receiver<T> && stoppable_receiver<T>;

template <full_receiver... Handlers>
class submission_context {
public:
    using handler_variant = std::variant<Handlers...>;

    template <typename H>
        requires(std::same_as<std::decay_t<H>, Handlers> || ...)
    explicit submission_context(H&& handler) : handler_(std::forward<H>(handler)) {}

    submission_context(const submission_context&) = delete;
    auto operator=(const submission_context&) -> submission_context& = delete;
    submission_context(submission_context&&) = default;
    auto operator=(submission_context&&) -> submission_context& = default;
    ~submission_context() = default;

    [[nodiscard]] auto as_context() noexcept -> void* { return &fi_ctx_; }

    [[nodiscard]] static auto from_context(void* ctx) noexcept -> submission_context* {
        return static_cast<submission_context*>(ctx);
    }

    void set_value(completion_event& event) {
        std::visit([&event](auto& h) { h.set_value(event); }, handler_);
    }

    void set_error(std::error_code ec) {
        std::visit([ec](auto& h) { h.set_error(ec); }, handler_);
    }

    void set_stopped() {
        std::visit([](auto& h) { h.set_stopped(); }, handler_);
    }

    template <typename H>
    [[nodiscard]] auto get_handler() noexcept -> H& {
        return std::get<H>(handler_);
    }

    template <typename H>
    [[nodiscard]] auto get_handler() const noexcept -> const H& {
        return std::get<H>(handler_);
    }

    [[nodiscard]] auto handler_index() const noexcept -> std::size_t { return handler_.index(); }

private:
    ::fi_context2 fi_ctx_{};
    handler_variant handler_;
};

namespace detail {

template <typename Signature>
using unique_function = std::move_only_function<Signature>;

}  // namespace detail

struct callback_receiver {
    using value_callback_type = detail::unique_function<void(completion_event&)>;
    using error_callback_type = detail::unique_function<void(std::error_code)>;
    using stopped_callback_type = detail::unique_function<void()>;

    value_callback_type on_complete;
    error_callback_type on_error;
    stopped_callback_type on_stopped;

    template <typename F>
        requires std::invocable<F, completion_event&>
    explicit callback_receiver(F&& complete_handler)
        : on_complete(std::forward<F>(complete_handler)), on_error([](std::error_code) {}),
          on_stopped([] {}) {}

    template <typename FComplete, typename FError>
        requires(std::invocable<FComplete, completion_event&> &&
                 std::invocable<FError, std::error_code>)
    callback_receiver(FComplete&& complete_handler, FError&& error_handler)
        : on_complete(std::forward<FComplete>(complete_handler)),
          on_error(std::forward<FError>(error_handler)), on_stopped([] {}) {}

    template <typename FComplete, typename FError, typename FStopped>
        requires(std::invocable<FComplete, completion_event&> &&
                 std::invocable<FError, std::error_code> && std::invocable<FStopped>)
    callback_receiver(FComplete&& complete_handler,
                      FError&& error_handler,
                      FStopped&& stopped_handler)
        : on_complete(std::forward<FComplete>(complete_handler)),
          on_error(std::forward<FError>(error_handler)),
          on_stopped(std::forward<FStopped>(stopped_handler)) {}

    void set_value(completion_event& event) {
        if (on_complete) {
            on_complete(event);
        }
    }

    void set_error(std::error_code ec) {
        if (on_error) {
            on_error(ec);
        }
    }

    void set_stopped() {
        if (on_stopped) {
            on_stopped();
        }
    }
};

struct coroutine_receiver {
    std::coroutine_handle<> handle;
    completion_event* result_storage;
    std::error_code* error_storage;
    bool* stopped_flag;

    coroutine_receiver(std::coroutine_handle<> h,
                       completion_event* result,
                       std::error_code* error = nullptr,
                       bool* stopped = nullptr)
        : handle(h), result_storage(result), error_storage(error), stopped_flag(stopped) {}

    void set_value(completion_event& event) {
        if (result_storage != nullptr) {
            *result_storage = event;
        }
        if (handle) {
            handle.resume();
        }
    }

    void set_error(std::error_code ec) {
        if (error_storage != nullptr) {
            *error_storage = ec;
        }
        if (handle) {
            handle.resume();
        }
    }

    void set_stopped() {
        if (stopped_flag != nullptr) {
            *stopped_flag = true;
        }
        if (handle) {
            handle.resume();
        }
    }
};

struct promise_receiver {
    std::promise<completion_event> promise;

    promise_receiver() = default;
    explicit promise_receiver(std::promise<completion_event>&& p) : promise(std::move(p)) {}

    void set_value(completion_event& event) { promise.set_value(event); }

    void set_error(std::error_code ec) {
        promise.set_exception(std::make_exception_ptr(std::system_error(ec)));
    }

    void set_stopped() {
        promise.set_exception(std::make_exception_ptr(
            std::system_error(std::make_error_code(std::errc::operation_canceled))));
    }

    [[nodiscard]] auto get_future() -> std::future<completion_event> {
        return promise.get_future();
    }
};

using default_submission_context =
    submission_context<callback_receiver, coroutine_receiver, promise_receiver>;

template <typename F>
[[nodiscard]] auto make_callback_context(F&& handler) -> default_submission_context* {
    return new default_submission_context(callback_receiver(std::forward<F>(handler)));
}

[[nodiscard]] inline auto make_coroutine_context(std::coroutine_handle<> handle,
                                                 completion_event* result,
                                                 std::error_code* error = nullptr,
                                                 bool* stopped = nullptr)
    -> default_submission_context* {
    return new default_submission_context(coroutine_receiver(handle, result, error, stopped));
}

[[nodiscard]] inline auto make_promise_context()
    -> std::pair<default_submission_context*, std::future<completion_event>> {
    auto* ctx = new default_submission_context(promise_receiver{});
    auto future = ctx->get_handler<promise_receiver>().get_future();
    return {ctx, std::move(future)};
}

template <full_receiver... Handlers>
void dispatch_completion(submission_context<Handlers...>* ctx, completion_event& event) {
    if (ctx == nullptr) {
        return;
    }

    if (event.has_error()) {
        ctx->set_error(event.error);
    } else {
        ctx->set_value(event);
    }
}

template <full_receiver... Handlers>
void dispatch_stopped(submission_context<Handlers...>* ctx) {
    if (ctx == nullptr) {
        return;
    }

    ctx->set_stopped();
}

}  // namespace loom
