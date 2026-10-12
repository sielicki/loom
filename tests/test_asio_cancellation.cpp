// SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only
#include <loom/asio/basic_endpoint.hpp>
#include <loom/detail/impl/completion_queue_impl.hpp>
#include <loom/detail/impl/endpoint_impl.hpp>

#include <array>
#include <vector>

#include <asio/bind_cancellation_slot.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

namespace {
struct cancellation_provider {
    std::vector<void*> pending;
    std::vector<void*> cancelled;
    int closed{};
    bool closed_while_pending{};
    bool fail_first_cancel{};
    bool fail_submission{};
    bool discard_on_close{};

    fi_ops ops{
        .size = sizeof(fi_ops),
        .close =
            [](fid* handle) {
                auto& self = *static_cast<cancellation_provider*>(handle->context);
                ++self.closed;
                self.closed_while_pending = !self.pending.empty();
                if (self.discard_on_close) {
                    self.pending.clear();
                    return 0;
                }
                return self.closed_while_pending ? -FI_EBUSY : 0;
            },
        .bind = [](fid*, fid*, std::uint64_t) { return 0; },
    };
    fi_ops cq_ops{.size = sizeof(fi_ops), .close = [](fid*) { return 0; }};
    fid_cq cq_handle{.fid = {.ops = &cq_ops}};
    loom::completion_queue queue{loom::detail::make_pmr_unique<loom::detail::completion_queue_impl>(
        loom::get_default_resource(), &cq_handle)};
    fi_ops_ep endpoint_ops{
        .size = sizeof(fi_ops_ep),
        .cancel = [](fid* handle, void* context) -> ssize_t {
            auto& self = *static_cast<cancellation_provider*>(handle->context);
            self.cancelled.push_back(context);
            return self.fail_first_cancel && self.cancelled.size() == 1 ? -FI_ENOSYS : 0;
        },
    };
    static auto submit(fid_ep* ep, const fi_msg* message, std::uint64_t) -> ssize_t {
        auto& self = *static_cast<cancellation_provider*>(ep->fid.context);
        if (self.fail_submission) {
            return -FI_EIO;
        }
        self.pending.push_back(message->context);
        return 0;
    }
    fi_ops_msg messages{
        .size = sizeof(fi_ops_msg),
        .recvmsg = submit,
        .sendmsg = submit,
    };
    fid_ep handle{.fid = {.context = this, .ops = &ops}, .ops = &endpoint_ops, .msg = &messages};

    auto endpoint() -> loom::endpoint {
        auto ep = loom::endpoint{loom::detail::make_pmr_unique<loom::detail::endpoint_impl>(
            loom::get_default_resource(),
            &handle,
            loom::endpoint_types::msg,
            loom::address_format::inet,
            0UZ,
            0UZ,
            loom::caps{},
            0,
            0,
            nullptr,
            loom::endpoint_iov_limits{1, 1, 1})};
        REQUIRE(ep.bind_cq(queue, loom::cq_bind::recv | loom::cq_bind::transmit));
        return ep;
    }

    void complete(void* context, std::error_code error = {}) {
        std::erase(pending, context);
        loom::completion_event event;
        event.error = error;
        event.context = loom::context_ptr<void>{context};
        REQUIRE(queue.dispatch(event));
    }

    void complete_all() {
        while (!pending.empty()) {
            complete(pending.back(), loom::make_error_code(loom::errc::canceled));
        }
    }
};
}  // namespace

TEST_CASE("Explicit Asio close retires discarded native operations", "[asio][cancellation]") {
    cancellation_provider native;
    native.discard_on_close = true;
    native.fail_first_cancel = GENERATE(false, true);
    const auto run_handlers = GENERATE(false, true);
    std::weak_ptr<std::array<std::byte, 8>> lifetime;
    int completions{};
    {
        ::asio::io_context ioc;
        ::asio::cancellation_signal signal;
        loom::asio::endpoint ep{ioc.get_executor(), native.endpoint()};
        auto storage = std::make_shared<std::array<std::byte, 8>>();
        lifetime = storage;
        auto handler = [&, storage](std::error_code error, std::size_t bytes) {
            CHECK(error == loom::errc::canceled);
            CHECK(bytes == 0);
            ++completions;
        };
        ep.async_receive(::asio::buffer(*storage), handler);
        ep.async_send(::asio::buffer(*storage),
                      ::asio::bind_cancellation_slot(signal.slot(), std::move(handler)));
        storage.reset();
        REQUIRE(native.pending.size() == 2);
        const auto contexts = native.pending;
        std::error_code error;
        ep.cancel(error);
        CHECK(static_cast<bool>(error) == native.fail_first_cancel);
        ep.close(error);
        CHECK_FALSE(error);
        CHECK_FALSE(ep.is_open());
        CHECK(native.closed == 1);
        CHECK(native.pending.empty());
        CHECK_FALSE(signal.slot().has_handler());
        CHECK(completions == 0);
        if (!native.pending.empty()) {
            // Keep the regression failure from leaving native work alive.
            native.complete_all();
        }
        for (auto* context : contexts) {
            loom::completion_event event;
            event.context = loom::context_ptr<void>{context};
            CHECK_FALSE(native.queue.dispatch(event));
        }
        ep.close(error);
        CHECK_FALSE(error);
        CHECK(native.closed == 1);
        if (run_handlers) {
            ioc.run();
            CHECK(completions == 2);
        }
    }
    CHECK(lifetime.expired());
    CHECK(completions == (run_handlers ? 2 : 0));
}

TEST_CASE("Failed Asio close preserves pending work for retry", "[asio][cancellation]") {
    cancellation_provider native;
    ::asio::io_context ioc;
    loom::asio::endpoint ep{ioc.get_executor(), native.endpoint()};
    std::array<std::byte, 1> storage{};
    int completions{};
    ep.async_receive(::asio::buffer(storage), [&](std::error_code, std::size_t) { ++completions; });
    std::error_code error;
    ep.close(error);
    CHECK(error == loom::errc::busy);
    CHECK(ep.is_open());
    CHECK(native.closed == 1);
    CHECK(native.pending.size() == 1);
    CHECK(native.cancelled.empty());
    CHECK(completions == 0);
    CHECK_THROWS_AS(ep.close(), std::system_error);
    CHECK(ep.is_open());
    ep.cancel(error);
    CHECK_FALSE(error);
    CHECK(native.cancelled == native.pending);
    native.complete_all();
    ioc.run();
    CHECK(completions == 1);
    ep.close(error);
    CHECK_FALSE(error);
    CHECK_FALSE(ep.is_open());
    CHECK(native.closed == 3);
}

TEST_CASE("Asio cancellation visits every pending operation", "[asio][cancellation]") {
    cancellation_provider native;
    native.fail_first_cancel = GENERATE(false, true);
    ::asio::io_context ioc;
    loom::asio::endpoint ep{ioc.get_executor(), native.endpoint()};
    std::array<std::byte, 1> storage{};
    int completions{};
    for (int i = 0; i < 3; ++i) {
        ep.async_receive(::asio::buffer(storage),
                         [&](std::error_code, std::size_t) { ++completions; });
    }
    REQUIRE(native.pending.size() == 3);
    native.complete(native.pending.front());
    std::error_code error;
    ep.cancel(error);
    REQUIRE(static_cast<bool>(error) == native.fail_first_cancel);
    REQUIRE(native.cancelled == native.pending);
    REQUIRE(native.cancelled.size() == 2);
    REQUIRE(completions == 0);
    native.complete_all();
    ioc.run();
    REQUIRE(completions == 3);
    native.cancelled.clear();
    ep.cancel(error);
    REQUIRE_FALSE(error);
    REQUIRE(native.cancelled.empty());
}

TEST_CASE("Asio cancellation survives moves and source destruction", "[asio][cancellation]") {
    cancellation_provider original;
    cancellation_provider replaced;
    ::asio::io_context ioc;
    ::asio::cancellation_signal signal;
    std::array<std::byte, 1> storage{};
    int completions{};
    auto handler = [&](std::error_code error, std::size_t bytes) {
        REQUIRE(error == loom::errc::canceled);
        REQUIRE(bytes == 0);
        ++completions;
    };
    loom::asio::endpoint destination{ioc.get_executor(), replaced.endpoint()};
    destination.async_receive(::asio::buffer(storage), handler);
    {
        loom::asio::endpoint source{ioc.get_executor(), original.endpoint()};
        auto* address = source.native_handle();
        source.async_receive(::asio::buffer(storage),
                             ::asio::bind_cancellation_slot(signal.slot(), handler));
        loom::asio::endpoint moved{std::move(source)};
        REQUIRE_FALSE(source.is_open());
        REQUIRE(moved.native_handle() == address);
        destination = std::move(moved);
        REQUIRE_FALSE(moved.is_open());
        REQUIRE(destination.native_handle() == address);
    }
    REQUIRE(original.cancelled.empty());
    REQUIRE(replaced.cancelled == replaced.pending);
    REQUIRE(replaced.closed == 0);
    signal.emit(::asio::cancellation_type::terminal);
    REQUIRE(original.cancelled == original.pending);
    std::error_code error;
    destination.close(error);
    REQUIRE(error == loom::errc::busy);
    REQUIRE(destination.is_open());
    REQUIRE(original.closed == 1);
    original.complete_all();
    replaced.complete_all();
    destination.close();
    REQUIRE_FALSE(destination.is_open());
    REQUIRE(original.closed == 2);
    REQUIRE(replaced.closed == 1);
    REQUIRE_FALSE(original.closed_while_pending);
    REQUIRE_FALSE(replaced.closed_while_pending);
    REQUIRE_FALSE(signal.slot().has_handler());
    ioc.run();
    REQUIRE(completions == 2);
}

TEST_CASE("Asio destruction retains the endpoint until completion", "[asio][cancellation]") {
    cancellation_provider native;
    ::asio::io_context ioc;
    std::array<std::byte, 1> storage{};
    bool completed{};
    {
        loom::asio::endpoint ep{ioc.get_executor(), native.endpoint()};
        ep.async_receive(::asio::buffer(storage), [&](std::error_code error, std::size_t) {
            REQUIRE(error == loom::errc::canceled);
            completed = true;
        });
    }
    REQUIRE(native.cancelled == native.pending);
    REQUIRE(native.closed == 0);
    native.complete_all();
    REQUIRE(native.closed == 1);
    REQUIRE_FALSE(native.closed_while_pending);
    ioc.run();
    REQUIRE(completed);
}

TEST_CASE("Failed Asio submissions leave no pending cancellation", "[asio][cancellation]") {
    cancellation_provider native;
    native.fail_submission = true;
    ::asio::io_context ioc;
    std::array<std::byte, 1> storage{};
    bool completed{};
    {
        loom::asio::endpoint ep{ioc.get_executor()};
        ep.close();
        // The native accessor remains usable after close, including for reuse.
        ep.get() = native.endpoint();
        ep.async_receive(::asio::buffer(storage), [&](std::error_code error, std::size_t) {
            REQUIRE(error == loom::errc::io_error);
            completed = true;
        });
        ep.cancel();
        REQUIRE(native.cancelled.empty());
    }
    REQUIRE(native.closed == 1);
    ioc.run();
    REQUIRE(completed);
}

TEST_CASE("Asio cancellation respects the requested guarantee", "[asio][cancellation]") {
    using cancellation_type = ::asio::cancellation_type;
    const auto type = GENERATE(cancellation_type::none,
                               cancellation_type::terminal,
                               cancellation_type::partial,
                               cancellation_type::total,
                               cancellation_type::partial | cancellation_type::total,
                               cancellation_type::all);
    const bool terminal = (type & cancellation_type::terminal) != cancellation_type::none;
    CAPTURE(type);
    cancellation_provider native;
    ::asio::io_context ioc;
    ::asio::cancellation_signal signal;
    loom::asio::endpoint ep{ioc.get_executor(), native.endpoint()};
    std::array<std::byte, 1> storage{};
    bool completed{};
    ep.async_receive(
        ::asio::buffer(storage),
        ::asio::bind_cancellation_slot(signal.slot(), [&](std::error_code error, std::size_t) {
            REQUIRE(static_cast<bool>(error) == terminal);
            completed = true;
        }));
    signal.emit(type);
    REQUIRE(native.cancelled.size() == (terminal ? 1 : 0));
    native.complete(native.pending.front(),
                    terminal ? loom::make_error_code(loom::errc::canceled) : std::error_code{});
    ioc.run();
    REQUIRE(completed);
}
