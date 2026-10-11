// SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only
#include <loom/async/completion_queue.hpp>
#include <loom/detail/impl/fabric_impl.hpp>

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

namespace {
struct diagnostic_resource : loom::memory_resource {
    bool fail{};
    int live{};
    auto do_allocate(std::size_t bytes, std::size_t alignment) -> void* override {
        if (fail)
            throw std::bad_alloc{};
        auto* result = std::pmr::new_delete_resource()->allocate(bytes, alignment);
        ++live;
        return result;
    }
    void do_deallocate(void* ptr, std::size_t bytes, std::size_t alignment) override {
        --live;
        std::pmr::new_delete_resource()->deallocate(ptr, bytes, alignment);
    }
    auto do_is_equal(const memory_resource& other) const noexcept -> bool override {
        return this == &other;
    }
};

struct diagnostic_provider {
    std::vector<std::byte> data;
    bool ready{true};
    bool oversized{};
    bool missing{};
    int error_status{1};
    int error_reads{};
    int error_code{FI_EIO};
    std::size_t overflow_length{};
    std::size_t supplied_capacity{};
    fi_ops lifetime{.size = sizeof(fi_ops), .close = [](fid*) { return 0; }};
    fi_ops_cq cq_ops{
        .size = sizeof(fi_ops_cq),
        .read = [](fid_cq* cq, void*, std::size_t) -> ssize_t {
            auto& self = *static_cast<diagnostic_provider*>(cq->fid.context);
            if (self.ready)
                return -FI_EAVAIL;
            std::ranges::fill(self.data, std::byte{0xff});
            return -FI_EAGAIN;
        },
        .readerr = [](fid_cq* cq, fi_cq_err_entry* entry, std::uint64_t) -> ssize_t {
            auto& self = *static_cast<diagnostic_provider*>(cq->fid.context);
            ++self.error_reads;
            if (self.error_status != 1)
                return self.error_status;
            auto* buffer = entry->err_data;
            self.supplied_capacity = entry->err_data_size;
            *entry = {.op_context = &self.ready,
                      .flags = FI_RECV,
                      .len = 7,
                      .olen = self.overflow_length,
                      .err = self.error_code,
                      .prov_errno = 37};
            if (self.supplied_capacity) {
                entry->err_data = buffer;
                entry->err_data_size = std::min(self.supplied_capacity, self.data.size());
                if (entry->err_data_size)
                    std::memcpy(buffer, self.data.data(), entry->err_data_size);
            } else {
                entry->err_data = self.data.data();
                entry->err_data_size = self.data.size();
            }
            if (self.oversized)
                entry->err_data_size = self.supplied_capacity + 1;
            if (self.missing)
                entry->err_data = nullptr;
            self.ready = false;
            return 1;
        },
        .sread =
            [](fid_cq* cq, void* buffer, std::size_t count, const void*, int) {
                return cq->ops->read(cq, buffer, count);
            },
    };
    fid_cq native_cq{.fid = {.context = this, .ops = &lifetime}, .ops = &cq_ops};
    fi_ops_domain domain_ops{
        .size = sizeof(fi_ops_domain),
        .cq_open =
            [](fid_domain* domain, fi_cq_attr*, fid_cq** output, void*) {
                *output = &static_cast<diagnostic_provider*>(domain->fid.context)->native_cq;
                return 0;
            },
    };
    fid_domain native_domain{.fid = {.context = this, .ops = &lifetime}, .ops = &domain_ops};
    fi_ops_fabric fabric_ops{
        .size = sizeof(fi_ops_fabric),
        .domain =
            [](fid_fabric* fabric, fi_info*, fid_domain** output, void*) {
                *output = &static_cast<diagnostic_provider*>(fabric->fid.context)->native_domain;
                return 0;
            },
    };
    fid_fabric native_fabric{.fid = {.context = this, .ops = &lifetime}, .ops = &fabric_ops};
    loom::fabric fabric{loom::detail::make_pmr_unique<loom::detail::fabric_impl>(
        loom::get_default_resource(), &native_fabric, nullptr)};

    auto open_domain(loom::memory_resource* resource = nullptr) -> loom::domain {
        fi_domain_attr attributes{.mr_mode = FI_MR_PROV_KEY, .max_err_data = data.size()};
        fi_info info{.domain_attr = &attributes};
        loom::fabric_info query{loom::detail::make_pmr_unique<loom::detail::fabric_info_impl>(
            loom::get_default_resource(), &info, false)};
        auto domain = loom::domain::create(fabric, query, resource);
        REQUIRE(domain);
        // The copied limit must not depend on the query's lifetime.
        return std::move(*domain);
    }
};

auto read_diagnostic(loom::completion_queue& queue, int method) -> loom::completion_event {
    if (method == 0) {
        auto event = queue.poll();
        REQUIRE(event);
        return std::move(*event);
    }
    if (method == 1) {
        auto event = queue.read();
        REQUIRE(event);
        return std::move(*event);
    }
    if (method == 2) {
        auto event = queue.wait(std::chrono::milliseconds{0});
        REQUIRE(event);
        return std::move(*event);
    }
    std::array<loom::completion_event, 2> events;
    const auto count = queue.poll_batch(events);
    REQUIRE(count == 1);
    return std::move(events.front());
}

auto bytes(const loom::completion_event& event) -> std::span<const std::byte> {
    return {event.error_info.err_data.get(), event.error_info.err_data_size};
}
}  // namespace

TEST_CASE("completion diagnostics survive later reads and queue destruction",
          "[completion_queue][diagnostic]") {
    const auto method = GENERATE(0, 1, 2, 3);
    const auto size = GENERATE(0UL, 1UL, 7UL, 4096UL);
    diagnostic_provider provider;
    provider.data.resize(size, std::byte{0x52});
    const auto expected = provider.data;
    auto domain = provider.open_domain();
    auto queue = loom::completion_queue::create(domain, {.wait_obj = true});
    REQUIRE(queue);
    auto event = read_diagnostic(*queue, method);
    REQUIRE(event.error == loom::errc::io_error);
    REQUIRE(event.context.get() == &provider.ready);
    REQUIRE(event.error_info.prov_errno == 37);
    REQUIRE(event.error_info.available);
    REQUIRE(std::ranges::equal(bytes(event), expected));
    auto copy = event;
    event = {};
    REQUIRE_FALSE(queue->poll());
    *queue = {};
    REQUIRE(std::ranges::equal(bytes(copy), expected));
    REQUIRE(provider.supplied_capacity == std::max(size, 1UL));
}

TEST_CASE("completion diagnostic allocation precedes error consumption",
          "[completion_queue][diagnostic]") {
    const auto method = GENERATE(0, 1, 2, 3);
    diagnostic_provider provider;
    provider.data.resize(7, std::byte{0x63});
    diagnostic_resource resource;
    auto domain = provider.open_domain(&resource);
    auto queue = loom::completion_queue::create(domain, {.wait_obj = true}, &resource);
    REQUIRE(queue);
    const auto before = resource.live;
    resource.fail = true;
    REQUIRE_THROWS_AS(read_diagnostic(*queue, method), std::bad_alloc);
    REQUIRE(provider.error_reads == 0);
    REQUIRE(resource.live == before);
    resource.fail = false;
    auto event = read_diagnostic(*queue, method);
    REQUIRE(resource.live > before);
    resource.fail = true;
    auto copy = event;
    event = {};
    *queue = {};
    domain = {};
    REQUIRE(resource.live > 0);
    REQUIRE(std::ranges::equal(bytes(copy), provider.data));
    copy = {};
    REQUIRE(resource.live == 0);
}

TEST_CASE("completion errors retain native truncation counts", "[completion_queue][diagnostic]") {
    const auto method = GENERATE(0, 1, 2, 3);
    const auto selective = GENERATE(false, true);
    diagnostic_provider provider;
    provider.error_code = FI_ETRUNC;
    provider.overflow_length = GENERATE(0UZ, 13UZ, std::numeric_limits<std::size_t>::max());
    auto domain = provider.open_domain();
    auto queue = loom::completion_queue::create(domain, {.wait_obj = true}).value();
    if (selective) {
        int other{};
        REQUIRE(queue.read_for(loom::context_ptr<void>{&other}).error() == loom::errc::again);
    }
    auto event = read_diagnostic(queue, method);
    REQUIRE(event.error == loom::make_error_code_from_fi_errno(FI_ETRUNC));
    REQUIRE(event.bytes_transferred == 7);
    REQUIRE(event.error_info.available);
    REQUIRE(event.error_info.overflow_length == provider.overflow_length);
    const auto copy = event;
    event = {};
    queue = {};
    REQUIRE_FALSE(event.error_info.available);
    REQUIRE(copy.error_info.available);
    REQUIRE(copy.error_info.overflow_length == provider.overflow_length);
}

TEST_CASE("invalid completion diagnostic payloads do not escape",
          "[completion_queue][diagnostic]") {
    const auto method = GENERATE(0, 1, 2, 3);
    const auto oversized = GENERATE(false, true);
    diagnostic_provider provider;
    provider.data.resize(7, std::byte{0x52});
    provider.oversized = oversized;
    provider.missing = !oversized;
    auto domain = provider.open_domain();
    auto queue = loom::completion_queue::create(domain, {.wait_obj = true});
    REQUIRE(queue);
    if (method == 2) {
        const auto event = queue->wait(std::chrono::milliseconds{0});
        REQUIRE_FALSE(event);
        REQUIRE(event.error() == loom::errc::io_error);
    } else {
        const auto event = read_diagnostic(*queue, method);
        REQUIRE(event.error == loom::errc::io_error);
        REQUIRE(event.error_info.err_data_size == 0);
        REQUIRE_FALSE(event.error_info.available);
    }
}

TEST_CASE("failed completion error reads release diagnostic storage",
          "[completion_queue][diagnostic]") {
    const auto status = GENERATE(0, -FI_EAGAIN, -FI_EINVAL);
    diagnostic_provider provider;
    provider.data.resize(7);
    provider.error_status = status;
    diagnostic_resource resource;
    auto domain = provider.open_domain();
    auto queue = loom::completion_queue::create(domain, {.wait_obj = true}, &resource);
    REQUIRE(queue);
    const auto before = resource.live;
    const auto event = queue->wait(std::chrono::milliseconds{0});
    REQUIRE_FALSE(event);
    REQUIRE(event.error() ==
            (status == -FI_EINVAL ? loom::errc::invalid_argument : loom::errc::again));
    REQUIRE(resource.live == before);
}

TEST_CASE("selective reads retain foreign error diagnostics", "[completion_queue][diagnostic]") {
    const auto method = GENERATE(0, 1, 2, 3);
    diagnostic_resource resource;
    diagnostic_provider provider;
    provider.data.resize(7, std::byte{0x63});
    const auto expected = provider.data;
    auto domain = provider.open_domain();
    auto queue = loom::completion_queue::create(domain, {.wait_obj = true}, &resource).value();
    int target{};
    resource.fail = true;
    REQUIRE_THROWS_AS(queue.read_for(loom::context_ptr<void>{&target}), std::bad_alloc);
    REQUIRE(provider.error_reads == 0);
    resource.fail = false;
    REQUIRE(queue.read_for(loom::context_ptr<void>{&target}).error() == loom::errc::again);
    REQUIRE(provider.error_reads == 1);
    // Reading again invalidates the provider's previous diagnostic storage.
    REQUIRE(queue.read_for(loom::context_ptr<void>{&target}).error() == loom::errc::again);
    auto event = read_diagnostic(queue, method);
    REQUIRE(event.context.get() == &provider.ready);
    REQUIRE(event.error == loom::errc::io_error);
    REQUIRE(event.error_info.prov_errno == 37);
    queue = {};
    REQUIRE(std::ranges::equal(bytes(event), expected));
    event = {};
    REQUIRE(resource.live == 0);
}
