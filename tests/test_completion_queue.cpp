// SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only
#include <loom/detail/impl/completion_queue_impl.hpp>

#include <array>
#include <catch2/catch_test_macros.hpp>

namespace {
struct mock_cq {
    int read_status{-FI_EAVAIL};
    int error_reads{};
    int context{};
    fi_ops ops{
        .size = sizeof(fi_ops),
        .close = [](fid*) { return 0; },
    };
    fi_ops_cq cq_ops{
        .size = sizeof(fi_ops_cq),
        .read = [](fid_cq* cq, void* buffer, std::size_t) -> ssize_t {
            auto& self = *static_cast<mock_cq*>(cq->fid.context);
            if (self.read_status != 1) {
                return self.read_status;
            }
            *static_cast<fi_cq_data_entry*>(buffer) = {.op_context = &self.context, .len = 7};
            self.read_status = -FI_EAGAIN;
            return 1;
        },
        .readerr = [](fid_cq* cq, fi_cq_err_entry* error, std::uint64_t) -> ssize_t {
            auto& self = *static_cast<mock_cq*>(cq->fid.context);
            ++self.error_reads;
            *error = {.op_context = &self.context, .len = 3, .tag = 42, .err = FI_ECANCELED};
            self.read_status = 1;
            return 1;
        },
    };
    fid_cq handle{.fid = {.context = this, .ops = &ops}, .ops = &cq_ops};
    loom::completion_queue queue{loom::detail::make_pmr_unique<loom::detail::completion_queue_impl>(
        loom::get_default_resource(), &handle)};
};
}  // namespace

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
    REQUIRE(events.front().tag == 0);
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
