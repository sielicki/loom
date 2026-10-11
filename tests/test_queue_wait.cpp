// SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only
#include <loom/async/completion_queue.hpp>
#include <loom/core/event_queue.hpp>
#include <loom/detail/impl/domain_impl.hpp>
#include <loom/detail/impl/fabric_impl.hpp>

#include <limits>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

namespace {
struct queue_provider {
    int calls{};
    int timeout{};
    int status{1};
    int error_status{1};
    int error_reads{};
    fi_wait_obj requested_wait{};
    fi_ops lifetime_ops{.size = sizeof(fi_ops), .close = [](fid*) { return 0; }};
    fi_ops_cq cq_ops{
        .size = sizeof(fi_ops_cq),
        .readerr = [](fid_cq* cq, fi_cq_err_entry* error, std::uint64_t) -> ssize_t {
            auto& self = *static_cast<queue_provider*>(cq->fid.context);
            ++self.error_reads;
            if (self.error_status != 1)
                return self.error_status;
            *error = {.len = 5, .err = FI_ECANCELED};
            return 1;
        },
        .sread = [](fid_cq* cq, void* buffer, std::size_t, const void*, int timeout) -> ssize_t {
            auto& self = *static_cast<queue_provider*>(cq->fid.context);
            ++self.calls;
            self.timeout = timeout;
            if (self.status != 1)
                return self.status;
            *static_cast<fi_cq_tagged_entry*>(buffer) = {.len = 17};
            return 1;
        },
    };
    fi_ops_eq eq_ops{
        .size = sizeof(fi_ops_eq),
        .read = [](fid_eq* eq, std::uint32_t* type, void* buffer, std::size_t, std::uint64_t)
            -> ssize_t {
            auto& self = *static_cast<queue_provider*>(eq->fid.context);
            ++self.calls;
            if (self.status != 1)
                return self.status;
            *type = FI_CONNECTED;
            *static_cast<fi_eq_cm_entry*>(buffer) = {.fid = &eq->fid};
            return sizeof(fi_eq_cm_entry);
        },
        .readerr = [](fid_eq* eq, fi_eq_err_entry* error, std::uint64_t) -> ssize_t {
            auto& self = *static_cast<queue_provider*>(eq->fid.context);
            ++self.error_reads;
            if (self.error_status != 1)
                return self.error_status;
            *error = {.fid = &eq->fid, .context = &self.calls, .data = 42, .err = FI_ECANCELED};
            return sizeof(fi_eq_err_entry);
        },
        .sread = [](fid_eq* eq,
                    std::uint32_t* type,
                    void* buffer,
                    std::size_t,
                    int timeout,
                    std::uint64_t) -> ssize_t {
            auto& self = *static_cast<queue_provider*>(eq->fid.context);
            ++self.calls;
            self.timeout = timeout;
            if (self.status != 1)
                return self.status;
            *type = FI_CONNECTED;
            *static_cast<fi_eq_cm_entry*>(buffer) = {.fid = &eq->fid};
            return sizeof(fi_eq_cm_entry);
        },
    };
    fid_cq native_cq{.fid = {.context = this, .ops = &lifetime_ops}, .ops = &cq_ops};
    fid_eq native_eq{.fid = {.context = this, .ops = &lifetime_ops}, .ops = &eq_ops};
    fi_ops_domain domain_ops{
        .size = sizeof(fi_ops_domain),
        .cq_open =
            [](fid_domain* dom, fi_cq_attr* attr, fid_cq** result, void*) {
                auto& self = *static_cast<queue_provider*>(dom->fid.context);
                self.requested_wait = attr->wait_obj;
                *result = &self.native_cq;
                return 0;
            },
    };
    fi_ops_fabric fabric_ops{
        .size = sizeof(fi_ops_fabric),
        .eq_open =
            [](fid_fabric* fab, fi_eq_attr* attr, fid_eq** result, void*) {
                auto& self = *static_cast<queue_provider*>(fab->fid.context);
                self.requested_wait = attr->wait_obj;
                *result = &self.native_eq;
                return 0;
            },
    };
    fid_domain native_domain{.fid = {.context = this, .ops = &lifetime_ops}, .ops = &domain_ops};
    fid_fabric native_fabric{.fid = {.context = this, .ops = &lifetime_ops}, .ops = &fabric_ops};
    loom::domain domain{loom::detail::make_pmr_unique<loom::detail::domain_impl>(
        loom::get_default_resource(), &native_domain)};
    loom::fabric fabric{loom::detail::make_pmr_unique<loom::detail::fabric_impl>(
        loom::get_default_resource(), &native_fabric, nullptr)};

    template <typename Action>
    void with_queue(bool completion, bool waitable, Action action) {
        if (completion) {
            auto created = loom::completion_queue::create(domain, {.wait_obj = waitable});
            REQUIRE(created);
            action(*created);
        } else {
            auto created = loom::event_queue::create(fabric, {.wait_obj = waitable});
            REQUIRE(created);
            action(*created);
        }
    }
};
}  // namespace

TEST_CASE("queue waits require a configured wait object", "[queue][wait]") {
    const auto completion = GENERATE(false, true);
    INFO("completion queue: " << completion);
    queue_provider provider;
    provider.with_queue(completion, false, [&](auto& queue) {
        const auto result = queue.wait(std::chrono::milliseconds{0});
        REQUIRE_FALSE(result);
        REQUIRE(result.error() == loom::errc::invalid_argument);
    });
    REQUIRE(provider.requested_wait == FI_WAIT_NONE);
    REQUIRE(provider.calls == 0);
}

TEST_CASE("queue waits preserve valid timeouts after moving", "[queue][wait]") {
    const auto completion = GENERATE(false, true);
    const auto timeout = GENERATE(std::chrono::milliseconds{0},
                                  std::chrono::milliseconds{37},
                                  std::chrono::milliseconds{std::numeric_limits<int>::max()},
                                  std::chrono::milliseconds::min());
    queue_provider provider;
    provider.with_queue(completion, true, [&](auto& queue) {
        auto moved = std::move(queue);
        REQUIRE(moved.wait(timeout));
        REQUIRE(provider.timeout == (timeout.count() < 0 ? -1 : timeout.count()));
        REQUIRE(moved.wait());
        REQUIRE(provider.timeout == -1);
        REQUIRE_FALSE(queue.wait());
    });
    REQUIRE(provider.requested_wait == FI_WAIT_UNSPEC);
    REQUIRE(provider.calls == 2);
}

TEST_CASE("queue waits reject overflowing finite timeouts", "[queue][wait]") {
    const auto completion = GENERATE(false, true);
    using namespace std::chrono;
    const auto timeout =
        GENERATE(milliseconds{std::numeric_limits<int>::max()} + 1ms, milliseconds::max());
    queue_provider provider;
    provider.with_queue(completion, true, [&](auto& queue) {
        const auto result = queue.wait(timeout);
        REQUIRE_FALSE(result);
        REQUIRE(result.error() == loom::errc::invalid_argument);
    });
    REQUIRE(provider.calls == 0);
}

TEST_CASE("queue waits do not invent events from failed or empty reads", "[queue][wait]") {
    const auto completion = GENERATE(false, true);
    const auto status = GENERATE(0, -FI_EAGAIN, -FI_EINTR, -FI_EINVAL, -FI_ETIMEDOUT);
    queue_provider provider;
    provider.status = status;
    provider.with_queue(completion, true, [&](auto& queue) {
        const auto result = queue.wait(std::chrono::milliseconds{0});
        REQUIRE_FALSE(result);
        REQUIRE(result.error() ==
                loom::make_error_code_from_fi_errno(status == 0 ? FI_EAGAIN : -status));
    });
    REQUIRE(provider.calls == 1);
    REQUIRE(provider.error_reads == 0);
}

TEST_CASE("queue waits preserve actual asynchronous error events", "[queue][wait]") {
    const auto completion = GENERATE(false, true);
    queue_provider provider;
    provider.status = -FI_EAVAIL;
    provider.with_queue(completion, true, [&](auto& queue) {
        const auto result = queue.wait(std::chrono::milliseconds{0});
        REQUIRE(result);
        REQUIRE(result->error == loom::errc::canceled);
    });
    REQUIRE(provider.error_reads == 1);
}

TEST_CASE("queue waits propagate error queue read failures", "[queue][wait]") {
    const auto completion = GENERATE(false, true);
    const auto status = GENERATE(0, -FI_EAGAIN, -FI_EIO);
    queue_provider provider;
    provider.status = -FI_EAVAIL;
    provider.error_status = status;
    provider.with_queue(completion, true, [&](auto& queue) {
        const auto result = queue.wait(std::chrono::milliseconds{0});
        REQUIRE_FALSE(result);
        REQUIRE(result.error() ==
                loom::make_error_code_from_fi_errno(status == 0 ? FI_EAGAIN : -status));
    });
    REQUIRE(provider.error_reads == 1);
}

TEST_CASE("event polling returns no event after an empty read", "[queue][poll]") {
    const auto status = GENERATE(0, -FI_EAGAIN);
    queue_provider provider;
    provider.status = status;
    auto queue = loom::event_queue::create(provider.fabric);
    REQUIRE(queue);
    REQUIRE_FALSE(queue->poll());
    REQUIRE(provider.error_reads == 0);
}

TEST_CASE("event polling preserves read failures without consuming operation errors",
          "[queue][poll]") {
    const auto status = GENERATE(-FI_EINTR, -FI_EINVAL, -FI_EIO);
    queue_provider provider;
    provider.status = status;
    auto queue = loom::event_queue::create(provider.fabric);
    REQUIRE(queue);
    const auto event = queue->poll();
    REQUIRE(event);
    REQUIRE(event->error == loom::make_error_code_from_fi_errno(-status));
    REQUIRE(event->fid == nullptr);
    REQUIRE(provider.error_reads == 0);
}

TEST_CASE("event polling preserves error entry metadata and read failures", "[queue][poll]") {
    const auto error_status = GENERATE(1, 0, -FI_EAGAIN, -FI_EIO);
    queue_provider provider;
    provider.status = -FI_EAVAIL;
    provider.error_status = error_status;
    auto queue = loom::event_queue::create(provider.fabric);
    REQUIRE(queue);
    const auto event = queue->poll();
    REQUIRE(event);
    if (error_status == 1) {
        REQUIRE(event->error == loom::errc::canceled);
        REQUIRE(event->fid == &provider.native_eq.fid);
        REQUIRE(event->context == &provider.calls);
        REQUIRE(event->data == 42);
    } else {
        REQUIRE(event->error ==
                loom::make_error_code_from_fi_errno(error_status == 0 ? FI_EAGAIN : -error_status));
    }
    REQUIRE(provider.error_reads == 1);
}
