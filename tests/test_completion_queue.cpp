// SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only
#include <loom/detail/impl/completion_queue_impl.hpp>
#include <loom/detail/impl/domain_impl.hpp>
#include <loom/detail/impl/fabric_impl.hpp>

#include <array>
#include <atomic>
#include <thread>
#include <utility>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/generators/catch_generators_range.hpp>

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

namespace {
constexpr std::array cq_formats{
    std::pair{loom::cq_format::context, FI_CQ_FORMAT_CONTEXT},
    std::pair{loom::cq_format::msg, FI_CQ_FORMAT_MSG},
    std::pair{loom::cq_format::data, FI_CQ_FORMAT_DATA},
    std::pair{loom::cq_format::tagged, FI_CQ_FORMAT_TAGGED},
    std::pair{loom::cq_format::unspecified, FI_CQ_FORMAT_TAGGED},
};

struct format_provider {
    fi_cq_format format{};
    bool tagged_supported{true};
    bool with_sources{};
    int source_reads{};
    int open_calls{};
    int read_calls{};
    int error_reads{};
    int status{};
    std::size_t cursor{};
    std::array<int, 70> contexts{};
    std::array<std::byte, 70> buffers{};

    auto read(void* output, std::size_t count) -> ssize_t {
        ++read_calls;
        if (status)
            return status;
        if (cursor == contexts.size())
            return -FI_EAGAIN;
        count = std::min(count, contexts.size() - cursor);
        for (std::size_t i = 0; i < count; ++i) {
            const auto index = cursor + i;
            constexpr auto flags = FI_RECV | FI_MULTI_RECV | FI_REMOTE_CQ_DATA;
            switch (format) {
                case FI_CQ_FORMAT_CONTEXT:
                    static_cast<fi_cq_entry*>(output)[i] = {&contexts[index]};
                    break;
                case FI_CQ_FORMAT_MSG:
                    static_cast<fi_cq_msg_entry*>(output)[i] = {
                        &contexts[index], flags, 10 + index};
                    break;
                case FI_CQ_FORMAT_DATA:
                    static_cast<fi_cq_data_entry*>(output)[i] = {
                        &contexts[index], flags, 10 + index, &buffers[index], 100 + index};
                    break;
                case FI_CQ_FORMAT_TAGGED:
                    static_cast<fi_cq_tagged_entry*>(output)[i] = {&contexts[index],
                                                                   flags,
                                                                   10 + index,
                                                                   &buffers[index],
                                                                   100 + index,
                                                                   200 + index};
                    break;
                default:
                    return -FI_EINVAL;
            }
        }
        cursor += count;
        return static_cast<ssize_t>(count);
    }
    auto read_from(void* output, std::size_t count, fi_addr_t* sources) -> ssize_t {
        ++source_reads;
        const auto start = cursor;
        const auto result = read(output, count);
        for (ssize_t i = 0; i < result; ++i) {
            const auto index = start + static_cast<std::size_t>(i);
            sources[i] = index % 2 ? FI_ADDR_NOTAVAIL : static_cast<fi_addr_t>(index);
        }
        return result;
    }
    fi_ops resource_ops{.size = sizeof(fi_ops), .close = [](fid*) { return 0; }};
    fi_ops_cq cq_ops{
        .size = sizeof(fi_ops_cq),
        .read =
            [](fid_cq* cq, void* output, std::size_t count) {
                return static_cast<format_provider*>(cq->fid.context)->read(output, count);
            },
        .readfrom =
            [](fid_cq* cq, void* output, std::size_t count, fi_addr_t* sources) {
                return static_cast<format_provider*>(cq->fid.context)
                    ->read_from(output, count, sources);
            },
        .readerr = [](fid_cq* cq, fi_cq_err_entry* output, std::uint64_t) -> ssize_t {
            auto& self = *static_cast<format_provider*>(cq->fid.context);
            ++self.error_reads;
            *output = {.op_context = &self.contexts[0],
                       .flags = FI_RECV,
                       .len = 3,
                       .buf = self.buffers.data(),
                       .data = 27,
                       .tag = 31,
                       .err = FI_ECANCELED,
                       .src_addr = 71};
            return 1;
        },
        .sread =
            [](fid_cq* cq, void* output, std::size_t count, const void*, int) {
                return static_cast<format_provider*>(cq->fid.context)->read(output, count);
            },
        .sreadfrom =
            [](fid_cq* cq, void* output, std::size_t count, fi_addr_t* sources, const void*, int) {
                return static_cast<format_provider*>(cq->fid.context)
                    ->read_from(output, count, sources);
            },
    };
    fid_cq native_cq{.fid = {.context = this, .ops = &resource_ops}, .ops = &cq_ops};
    fi_ops_domain domain_ops{
        .size = sizeof(fi_ops_domain),
        .cq_open =
            [](fid_domain* dom, fi_cq_attr* attr, fid_cq** output, void*) {
                auto& self = *static_cast<format_provider*>(dom->fid.context);
                ++self.open_calls;
                if (!self.tagged_supported && attr->format == FI_CQ_FORMAT_TAGGED)
                    return -FI_ENOSYS;
                self.format = attr->format;
                *output = &self.native_cq;
                return 0;
            },
    };
    fid_domain native_domain{.fid = {.context = this, .ops = &resource_ops}, .ops = &domain_ops};
    loom::domain domain{loom::detail::make_pmr_unique<loom::detail::domain_impl>(
        loom::get_default_resource(), &native_domain)};

    void check(const loom::completion_event& event, std::size_t index) {
        const bool message = format != FI_CQ_FORMAT_CONTEXT;
        const bool data = format == FI_CQ_FORMAT_DATA || format == FI_CQ_FORMAT_TAGGED;
        REQUIRE(event.context.get() == &contexts[index]);
        REQUIRE_FALSE(event.has_error());
        REQUIRE(event.flags == (message ? FI_RECV | FI_MULTI_RECV | FI_REMOTE_CQ_DATA : 0));
        REQUIRE(event.len == (message ? 10 + index : 0));
        REQUIRE(event.bytes_transferred == event.len);
        REQUIRE(event.buffer == (data ? &buffers[index] : nullptr));
        REQUIRE(event.data == (data ? 100 + index : 0));
        REQUIRE(event.tag == (format == FI_CQ_FORMAT_TAGGED ? 200 + index : 0));
        REQUIRE_FALSE(event.error_info.has_provider_error());
        REQUIRE(event.source == (with_sources && index % 2 == 0
                                     ? loom::fabric_addr{static_cast<std::uint64_t>(index)}
                                     : loom::fabric_addrs::unspecified));
    }
};
}  // namespace

TEST_CASE("completion formats select and decode native single entries",
          "[completion_queue][format]") {
    const auto [format, native_format] = GENERATE(Catch::Generators::from_range(cq_formats));
    format_provider provider;
    provider.tagged_supported = native_format == FI_CQ_FORMAT_TAGGED;
    provider.with_sources = GENERATE(false, true);
    auto created = loom::completion_queue::create(
        provider.domain,
        {.wait_obj = true, .format = format, .source_addresses = provider.with_sources});
    REQUIRE(created);
    REQUIRE(provider.format == native_format);
    auto queue = std::move(*created);
    REQUIRE(queue.format() ==
            (format == loom::cq_format::unspecified ? loom::cq_format::tagged : format));
    REQUIRE(created->format() == loom::cq_format::unspecified);
    const auto polled = queue.poll();
    REQUIRE(polled);
    provider.check(*polled, 0);
    const auto waited = queue.wait(std::chrono::milliseconds{0});
    REQUIRE(waited);
    provider.check(*waited, 1);
    const auto read = queue.read();
    REQUIRE(read);
    provider.check(*read, 2);
    REQUIRE(provider.source_reads == (provider.with_sources ? 3 : 0));
}

TEST_CASE("selective reads preserve other completions in order", "[completion_queue][selective]") {
    const auto [format, native_format] = GENERATE(Catch::Generators::from_range(cq_formats));
    format_provider provider;
    provider.with_sources = GENERATE(false, true);
    auto created = loom::completion_queue::create(
        provider.domain,
        {.wait_obj = true, .format = format, .source_addresses = provider.with_sources});
    REQUIRE(created);
    auto& queue = *created;
    const loom::context_ptr<void> target{&provider.contexts[5]};
    for (int i = 0; i != 5; ++i) {
        auto event = queue.read_for(target);
        REQUIRE_FALSE(event);
        REQUIRE(event.error() == loom::errc::again);
    }
    const auto selected = queue.read_for(target);
    REQUIRE(selected);
    provider.check(*selected, 5);
    const auto reads = provider.read_calls;
    // Select an entry already preserved without consuming another native one.
    const auto earlier = queue.read_for(loom::context_ptr<void>{&provider.contexts[2]});
    REQUIRE(earlier);
    provider.check(*earlier, 2);
    REQUIRE(provider.read_calls == reads);
    auto moved = std::move(queue);
    provider.check(moved.poll().value(), 0);
    provider.check(moved.wait(std::chrono::milliseconds{0}).value(), 1);
    std::array<loom::completion_event, 70> batch;
    REQUIRE(moved.poll_batch(batch) == 2);
    provider.check(batch[0], 3);
    provider.check(batch[1], 4);
    REQUIRE(provider.read_calls == reads);
    provider.check(moved.read().value(), 6);
    REQUIRE(moved.read_for({}).error() == loom::errc::invalid_argument);
    REQUIRE(queue.read_for(target).error() == loom::errc::invalid_argument);
}

TEST_CASE("selective reads do not block behind a native waiter", "[completion_queue][selective]") {
    struct provider {
        std::atomic<bool> entered{}, release{};
        int reads{};
        int context{};
        fi_ops resource_ops{.size = sizeof(fi_ops), .close = [](fid*) { return 0; }};
        fi_ops_cq cq_ops{
            .size = sizeof(fi_ops_cq),
            .read = [](fid_cq* cq, void*, std::size_t) -> ssize_t {
                ++static_cast<provider*>(cq->fid.context)->reads;
                return -FI_EAGAIN;
            },
            .sread = [](fid_cq* cq, void* output, std::size_t, const void*, int) -> ssize_t {
                auto& self = *static_cast<provider*>(cq->fid.context);
                self.entered.store(true);
                self.entered.notify_one();
                self.release.wait(false);
                *static_cast<fi_cq_tagged_entry*>(output) = {.op_context = &self.context, .len = 7};
                return 1;
            },
        };
        fid_cq handle{.fid = {.context = this, .ops = &resource_ops}, .ops = &cq_ops};
    } native;
    loom::completion_queue queue{loom::detail::make_pmr_unique<loom::detail::completion_queue_impl>(
        loom::get_default_resource(), &native.handle, loom::runtime_progress_policy{}, true)};
    std::optional<loom::completion_event> completed;
    std::jthread waiter{[&] { completed = queue.wait().value(); }};
    native.entered.wait(false);
    const auto selected = queue.read_for(loom::context_ptr<void>{&native.context});
    CHECK(!selected);
    CHECK(selected.error() == loom::errc::again);
    CHECK_FALSE(queue.poll());
    std::array<loom::completion_event, 2> batch;
    CHECK(queue.poll_batch(batch) == 0);
    CHECK(queue.progress().error() == loom::errc::again);
    CHECK(queue.wait(std::chrono::milliseconds{0}).error() == loom::errc::again);
    CHECK(native.reads == 0);
    native.release.store(true);
    native.release.notify_one();
    waiter.join();
    REQUIRE(completed);
    REQUIRE(completed->context.get() == &native.context);
    REQUIRE(completed->bytes_transferred == 7);
}

TEST_CASE("completion batches use the selected native stride", "[completion_queue][format]") {
    const auto [format, native_format] = GENERATE(Catch::Generators::from_range(cq_formats));
    format_provider provider;
    provider.with_sources = GENERATE(false, true);
    auto queue = loom::completion_queue::create(
        provider.domain, {.format = format, .source_addresses = provider.with_sources});
    REQUIRE(queue);
    REQUIRE(provider.format == native_format);
    std::array<loom::completion_event, 72> events{};
    for (auto& event : events) {
        event.error = loom::make_error_code(loom::errc::io_error);
        event.flags = event.data = event.tag = UINT64_MAX;
    }
    REQUIRE(queue->poll_batch(events) == 64);
    for (std::size_t i = 0; i < 64; ++i)
        provider.check(events[i], i);
    REQUIRE(events[64].has_error());
    REQUIRE(queue->poll_batch(events) == 6);
    for (std::size_t i = 0; i < 6; ++i)
        provider.check(events[i], i + 64);
    REQUIRE(queue->poll_batch(events) == 0);
    REQUIRE(provider.read_calls == 3);
    REQUIRE(queue->poll_batch({}) == 0);
    REQUIRE(provider.read_calls == 3);
    REQUIRE(provider.source_reads == (provider.with_sources ? 3 : 0));
}

TEST_CASE("completion error layout is independent of the success format",
          "[completion_queue][format]") {
    const auto [format, native_format] = GENERATE(Catch::Generators::from_range(cq_formats));
    const auto operation = GENERATE(0, 1, 2);
    format_provider provider;
    provider.status = -FI_EAVAIL;
    provider.with_sources = GENERATE(false, true);
    auto queue = loom::completion_queue::create(
        provider.domain,
        {.wait_obj = true, .format = format, .source_addresses = provider.with_sources});
    REQUIRE(queue);
    REQUIRE(provider.format == native_format);
    loom::completion_event event;
    if (operation == 0) {
        const auto read = queue->poll();
        REQUIRE(read);
        event = *read;
    } else if (operation == 1) {
        const auto read = queue->wait(std::chrono::milliseconds{0});
        REQUIRE(read);
        event = *read;
    } else {
        REQUIRE(queue->poll_batch(std::span{&event, 1}) == 1);
    }
    REQUIRE(event.error == loom::errc::canceled);
    REQUIRE(event.context.get() == &provider.contexts[0]);
    REQUIRE(event.buffer == provider.buffers.data());
    REQUIRE(event.flags == FI_RECV);
    REQUIRE(event.len == 3);
    REQUIRE(event.data == 27);
    REQUIRE(event.tag == 31);
    REQUIRE(event.source ==
            (provider.with_sources ? loom::fabric_addr{71} : loom::fabric_addrs::unspecified));
}

TEST_CASE("source address reads reject absent native operations", "[completion_queue][source]") {
    const auto method = GENERATE(0, 1, 2);
    const auto truncated = GENERATE(false, true);
    format_provider provider;
    if (truncated) {
        provider.cq_ops.size = offsetof(fi_ops_cq, readfrom);
    } else {
        provider.cq_ops.readfrom = nullptr;
        provider.cq_ops.sreadfrom = nullptr;
    }
    auto queue = loom::completion_queue::create(provider.domain,
                                                {.wait_obj = true, .source_addresses = true});
    REQUIRE(queue);
    if (method == 1) {
        const auto result = queue->wait();
        REQUIRE_FALSE(result);
        REQUIRE(result.error() == loom::make_error_code_from_fi_errno(FI_ENOSYS));
    } else {
        loom::completion_event event;
        if (method == 0) {
            const auto result = queue->poll();
            REQUIRE(result);
            event = *result;
        } else {
            REQUIRE(queue->poll_batch(std::span{&event, 1}) == 1);
        }
        REQUIRE(event.error == loom::make_error_code_from_fi_errno(FI_ENOSYS));
        REQUIRE(event.source == loom::fabric_addrs::unspecified);
    }
    REQUIRE(provider.read_calls == 0);
    REQUIRE(provider.source_reads == 0);
}

TEST_CASE("invalid completion formats fail before opening the provider",
          "[completion_queue][format]") {
    format_provider provider;
    const auto queue = loom::completion_queue::create(provider.domain,
                                                      {.format = static_cast<loom::cq_format>(-1)});
    REQUIRE_FALSE(queue);
    REQUIRE(queue.error() == loom::errc::invalid_argument);
    REQUIRE(provider.open_calls == 0);
}

TEST_CASE("immediate data uses the remote CQ data flag", "[completion_queue]") {
    loom::completion_event event;
    event.flags = FI_WRITE;
    REQUIRE_FALSE(event.has_immediate_data());
    event.flags = FI_REMOTE_CQ_DATA;
    REQUIRE(event.has_immediate_data());
}

TEST_CASE("completion progress leaves entries queued and reports failures",
          "[completion_queue][progress]") {
    const auto status = GENERATE(0, -FI_EAGAIN, -FI_EAVAIL, -FI_EIO, -FI_EINVAL, -FI_ENOSYS);
    format_provider provider;
    provider.status = status;
    auto queue = loom::completion_queue::create(provider.domain);
    REQUIRE(queue);
    const auto progressed = queue->progress();
    if (status == 0 || status == -FI_EAGAIN) {
        REQUIRE(progressed);
    } else {
        REQUIRE_FALSE(progressed);
        REQUIRE(progressed.error() == loom::make_error_code_from_fi_errno(-status));
    }
    REQUIRE(provider.read_calls == 1);
    REQUIRE(provider.cursor == 0);
    REQUIRE(provider.error_reads == 0);
    if (status == -FI_EAVAIL) {
        const auto error = queue->poll();
        REQUIRE(error);
        REQUIRE(error->error == loom::errc::canceled);
        REQUIRE(error->context.get() == &provider.contexts[0]);
        REQUIRE(provider.error_reads == 1);
    }
    provider.status = 0;
    const auto completion = queue->poll();
    REQUIRE(completion);
    provider.check(*completion, 0);
}

TEST_CASE("completion progress rejects an empty queue", "[completion_queue][progress]") {
    const loom::completion_queue queue;
    const auto result = queue.progress();
    REQUIRE_FALSE(result);
    REQUIRE(result.error() == loom::errc::invalid_argument);
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

TEST_CASE("wait descriptor queries reject unavailable control operations", "[completion_queue]") {
    enum class unavailable { table, operation, version };
    const auto missing = GENERATE(unavailable::table, unavailable::operation, unavailable::version);
    mock_cq provider;
    if (missing == unavailable::table)
        provider.handle.fid.ops = nullptr;
    else if (missing == unavailable::operation)
        provider.ops.control = nullptr;
    else
        provider.ops.size = offsetof(fi_ops, control);
    const auto descriptor = provider.queue.get_wait_fd();
    const auto blocking = provider.queue.supports_blocking_wait();
    provider.handle.fid.ops = &provider.ops;
    REQUIRE_FALSE(descriptor);
    REQUIRE(descriptor.error() == loom::errc::not_supported);
    REQUIRE_FALSE(blocking);
    REQUIRE_FALSE(provider.wait_requested);
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
    REQUIRE(provider.queue.try_wait().error() == loom::errc::not_supported);
    REQUIRE(loom::completion_queue{}.try_wait().error() == loom::errc::invalid_argument);
    SECTION("missing wait operation") {
        fabric_ops.trywait = nullptr;
        REQUIRE(provider.queue.try_wait(fabric).error() == loom::errc::not_supported);
    }
    SECTION("older operation table") {
        fabric_ops.size = offsetof(fi_ops_fabric, trywait);
        REQUIRE(provider.queue.try_wait(fabric).error() == loom::errc::not_supported);
    }
    SECTION("missing fabric operation table") {
        handle.ops = nullptr;
        REQUIRE(provider.queue.try_wait(fabric).error() == loom::errc::not_supported);
    }
}

TEST_CASE("created completion queues retain their native wait fabric", "[completion_queue]") {
    const auto custom_resource = GENERATE(false, true);
    std::pmr::monotonic_buffer_resource arena;
    auto* resource = custom_resource ? &arena : nullptr;
    mock_cq provider;
    provider.queue = {};
    struct wait_state {
        fid_domain* domain{};
        fid_cq* cq{};
        int status{};
        int calls{};
    } state;
    fi_ops lifetime{.size = sizeof(fi_ops), .close = [](fid*) { return 0; }};
    fi_ops_domain domain_ops{
        .size = sizeof(fi_ops_domain),
        .cq_open =
            [](fid_domain* domain, fi_cq_attr*, fid_cq** output, void*) {
                *output = static_cast<wait_state*>(domain->fid.context)->cq;
                return 0;
            },
    };
    fid_domain native_domain{.fid = {.context = &state, .ops = &lifetime}, .ops = &domain_ops};
    state.domain = &native_domain;
    state.cq = &provider.handle;
    fi_ops_fabric fabric_ops{
        .size = sizeof(fi_ops_fabric),
        .domain =
            [](fid_fabric* fabric, fi_info*, fid_domain** output, void*) {
                *output = static_cast<wait_state*>(fabric->fid.context)->domain;
                return 0;
            },
        .trywait =
            [](fid_fabric* fabric, fid** descriptors, int count) {
                auto& state = *static_cast<wait_state*>(fabric->fid.context);
                ++state.calls;
                CHECK(count == 1);
                CHECK(descriptors[0] == &state.cq->fid);
                return state.status;
            },
    };
    fid_fabric native_fabric{.fid = {.context = &state, .ops = &lifetime}, .ops = &fabric_ops};
    loom::fabric fabric{loom::detail::make_pmr_unique<loom::detail::fabric_impl>(
        loom::get_default_resource(), &native_fabric, nullptr)};
    auto* native_info = fi_allocinfo();
    REQUIRE(native_info);
    native_info->domain_attr->mr_mode = FI_MR_PROV_KEY;
    loom::fabric_info info{loom::detail::make_pmr_unique<loom::detail::fabric_info_impl>(
        loom::get_default_resource(), native_info)};
    auto domain = loom::domain::create(fabric, info, resource);
    REQUIRE(domain);
    auto created = loom::completion_queue::create(*domain, {.wait_obj = true}, resource);
    REQUIRE(created);
    auto queue = std::move(*created);
    auto moved_fabric = std::move(fabric);
    auto moved_domain = std::move(*domain);
    REQUIRE(queue.try_wait().value());
    REQUIRE(queue.try_wait(moved_fabric).value());
    REQUIRE(state.calls == 2);
    fid_fabric unrelated{.fid = {.context = &state, .ops = &lifetime}, .ops = &fabric_ops};
    loom::fabric other{loom::detail::make_pmr_unique<loom::detail::fabric_impl>(
        loom::get_default_resource(), &unrelated, nullptr)};
    REQUIRE(queue.try_wait(other).error() == loom::errc::invalid_argument);
    REQUIRE(state.calls == 2);
    state.status = -FI_EAGAIN;
    REQUIRE_FALSE(queue.try_wait().value());
    state.status = -FI_EIO;
    REQUIRE(queue.try_wait().error() == loom::errc::io_error);
    REQUIRE(state.calls == 4);
    provider.read_status = 1;
    int other_context{};
    REQUIRE(queue.read_for(loom::context_ptr<void>{&other_context}).error() == loom::errc::again);
    REQUIRE_FALSE(queue.try_wait().value());
    REQUIRE(state.calls == 4);
    REQUIRE(queue.poll());
    state.status = 0;
    REQUIRE(queue.try_wait().value());
    REQUIRE(state.calls == 5);
    queue = {};
    moved_domain = {};
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
