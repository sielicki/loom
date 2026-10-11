// SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only
#include <loom/detail/impl/completion_queue_impl.hpp>
#include <loom/detail/impl/fabric_impl.hpp>

#include <array>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

namespace {
struct mock_cq {
    int read_status{-FI_EAVAIL};
    int error_reads{};
    int context{};
    std::array<std::byte, 64> receive_buffer{};
    fi_wait_obj wait_object{FI_WAIT_FD};
    bool wait_requested{};
    fi_ops ops{
        .size = sizeof(fi_ops),
        .close = [](fid*) { return 0; },
        .control =
            [](fid* handle, int command, void* output) {
                auto& self = *static_cast<mock_cq*>(handle->context);
                if (command == FI_GETWAITOBJ) {
                    *static_cast<fi_wait_obj*>(output) = self.wait_object;
                    return 0;
                }
                if (command == FI_GETWAIT) {
                    self.wait_requested = true;
                    *static_cast<int*>(output) = 23;
                    return 0;
                }
                return -FI_ENOSYS;
            },
    };
    fi_ops_cq cq_ops{
        .size = sizeof(fi_ops_cq),
        .read = [](fid_cq* cq, void* buffer, std::size_t) -> ssize_t {
            auto& self = *static_cast<mock_cq*>(cq->fid.context);
            if (self.read_status != 1) {
                return self.read_status;
            }
            *static_cast<fi_cq_tagged_entry*>(buffer) = {.op_context = &self.context,
                                                         .flags = FI_RECV | FI_MULTI_RECV,
                                                         .len = 7,
                                                         .buf = self.receive_buffer.data() + 16,
                                                         .tag = 99};
            self.read_status = -FI_EAGAIN;
            return 1;
        },
        .readerr = [](fid_cq* cq, fi_cq_err_entry* error, std::uint64_t) -> ssize_t {
            auto& self = *static_cast<mock_cq*>(cq->fid.context);
            ++self.error_reads;
            *error = {.op_context = &self.context,
                      .flags = FI_RECV,
                      .len = 3,
                      .buf = self.receive_buffer.data() + 32,
                      .tag = 42,
                      .err = FI_ECANCELED};
            self.read_status = 1;
            return 1;
        },
        .sread = [](fid_cq* cq, void* buffer, std::size_t count, const void*, int) -> ssize_t {
            return cq->ops->read(cq, buffer, count);
        },
    };
    fid_cq handle{.fid = {.context = this, .ops = &ops}, .ops = &cq_ops};
    loom::completion_queue queue{loom::detail::make_pmr_unique<loom::detail::completion_queue_impl>(
        loom::get_default_resource(), &handle, loom::runtime_progress_policy{}, true)};
};
}  // namespace

TEST_CASE("immediate data uses the remote CQ data flag", "[completion_queue]") {
    loom::completion_event event;
    event.flags = FI_WRITE;
    REQUIRE_FALSE(event.has_immediate_data());
    event.flags = FI_REMOTE_CQ_DATA;
    REQUIRE(event.has_immediate_data());
}

TEST_CASE("batch polling drains errors and resumes completions", "[completion_queue]") {
    mock_cq provider;
    std::array<loom::completion_event, 2> events;
    REQUIRE(provider.queue.poll_batch(events) == 1);
    REQUIRE(provider.error_reads == 1);
    REQUIRE(events.front().error == loom::make_error_code_from_fi_errno(FI_ECANCELED));
    REQUIRE(events.front().context.get() == &provider.context);
    REQUIRE(events.front().tag == 42);
    REQUIRE(provider.queue.poll_batch(events) == 1);
    REQUIRE_FALSE(events.front().has_error());
    REQUIRE(events.front().bytes_transferred == 7);
    REQUIRE(events.front().tag == 99);
    REQUIRE(provider.queue.poll_batch(events) == 0);
}

TEST_CASE("queue read failures do not consume operation errors", "[completion_queue]") {
    mock_cq provider;
    provider.read_status = -FI_EINVAL;
    const auto event = provider.queue.poll();
    REQUIRE(event);
    REQUIRE(event->error == loom::make_error_code_from_fi_errno(FI_EINVAL));
    REQUIRE(provider.error_reads == 0);
}

TEST_CASE("wait descriptors require a file descriptor wait object", "[completion_queue]") {
    mock_cq provider;
    provider.wait_object = FI_WAIT_POLLFD;
    REQUIRE_FALSE(provider.queue.get_wait_fd());
    REQUIRE_FALSE(provider.wait_requested);
    REQUIRE(provider.queue.supports_blocking_wait());

    provider.wait_object = FI_WAIT_NONE;
    REQUIRE_FALSE(provider.queue.supports_blocking_wait());
    provider.wait_object = FI_WAIT_FD;
    REQUIRE(provider.queue.get_wait_fd().value() == 23);
    REQUIRE(provider.wait_requested);
}

TEST_CASE("native waits consult the fabric before blocking", "[completion_queue]") {
    mock_cq provider;
    struct wait_state {
        int status{};
        fid* expected{};
    } state{0, &provider.handle.fid};
    fi_ops ops{.size = sizeof(fi_ops), .close = [](fid*) { return 0; }};
    fi_ops_fabric fabric_ops{
        .size = sizeof(fi_ops_fabric),
        .trywait =
            [](fid_fabric* fabric, fid** descriptors, int count) {
                auto& self = *static_cast<wait_state*>(fabric->fid.context);
                CHECK(count == 1);
                CHECK(descriptors[0] == self.expected);
                return self.status;
            },
    };
    fid_fabric handle{.fid = {.context = &state, .ops = &ops}, .ops = &fabric_ops};
    loom::fabric fabric{loom::detail::make_pmr_unique<loom::detail::fabric_impl>(
        loom::get_default_resource(), &handle, nullptr)};
    REQUIRE(provider.queue.try_wait(fabric).value());
    state.status = -FI_EAGAIN;
    REQUIRE_FALSE(provider.queue.try_wait(fabric).value());
    state.status = -FI_EINVAL;
    REQUIRE(provider.queue.try_wait(fabric).error() == loom::errc::invalid_argument);
    REQUIRE_FALSE(provider.queue.try_wait(loom::fabric{}));
}

TEST_CASE("completion reads preserve received message locations", "[completion_queue][buffer]") {
    const auto method = GENERATE(0, 1, 2);
    const auto error = GENERATE(false, true);
    mock_cq provider;
    provider.read_status = error ? -FI_EAVAIL : 1;
    loom::completion_event event;
    if (method == 0) {
        auto result = provider.queue.poll();
        REQUIRE(result);
        event = *result;
    } else if (method == 1) {
        auto result = provider.queue.wait(std::chrono::milliseconds{0});
        REQUIRE(result);
        event = *result;
    } else {
        std::array<loom::completion_event, 2> results;
        REQUIRE(provider.queue.poll_batch(results) == 1);
        event = results.front();
    }
    REQUIRE(event.buffer == provider.receive_buffer.data() + (error ? 32 : 16));
    REQUIRE(event.bytes_transferred == (error ? 3 : 7));
    REQUIRE(event.has_error() == error);
    REQUIRE((event.flags & FI_RECV) != 0);
}
