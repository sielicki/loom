// SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only
#include <loom/asio.hpp>
#include <loom/detail/impl/completion_queue_impl.hpp>
#include <loom/detail/impl/endpoint_impl.hpp>
#include <loom/detail/impl/memory_impl.hpp>
#include <loom/loom.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <limits>
#include <span>
#include <string>
#include <thread>

#include <asio/bind_cancellation_slot.hpp>
#include <asio/bind_executor.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

namespace {
struct vector_provider {
    fid_domain domain_handle{};
    loom::endpoint_iov_limits limits{64, 64, 64};
    std::span<const iovec> buffers;
    void* context{};
    void* attempted_context{};
    std::size_t remote_length{};
    bool throw_on_submit{};
    fi_addr_t peer{};
    fi_addr_t expected_message_peer{FI_ADDR_UNSPEC};
    std::uint64_t remote_address{};
    std::uint64_t remote_key{};
    std::vector<void*> descriptors;
    int submission_status{};
    std::uint64_t submission_flags{};
    bool completion_pending{};
    loom::completion_direction completion_direction{};

    static auto capture_rma(fid_ep* ep, const fi_msg_rma* msg, std::uint64_t flags) -> ssize_t {
        auto& self = *static_cast<vector_provider*>(ep->fid.context);
        self.submission_flags = flags;
        REQUIRE(msg->rma_iov_count == 1);
        self.remote_length = msg->rma_iov[0].len;
        self.remote_address = msg->rma_iov[0].addr;
        self.remote_key = msg->rma_iov[0].key;
        self.peer = msg->addr;
        if (msg->desc)
            self.descriptors.assign(msg->desc, msg->desc + msg->iov_count);
        return capture(ep, msg->msg_iov, msg->iov_count, msg->context);
    }

    static auto capture_message(fid_ep* ep,
                                const auto* msg,
                                std::uint64_t flags,
                                loom::completion_direction direction) -> ssize_t {
        auto& self = *static_cast<vector_provider*>(ep->fid.context);
        self.submission_flags = flags;
        REQUIRE(msg->addr == self.expected_message_peer);
        if (msg->desc)
            self.descriptors.assign(msg->desc, msg->desc + msg->iov_count);
        return capture(ep, msg->msg_iov, msg->iov_count, msg->context, direction);
    }

    static auto capture(fid_ep* ep,
                        const iovec* iov,
                        std::size_t count,
                        void* context,
                        loom::completion_direction direction = loom::completion_direction::transmit)
        -> ssize_t {
        auto& self = *static_cast<vector_provider*>(ep->fid.context);
        self.attempted_context = context;
        loom::completion_registration duplicate;
        auto registered =
            duplicate.bind(direction == loom::completion_direction::receive ? self.rx : self.tx,
                           loom::context_ptr<void>{context},
                           &self,
                           [](void*, const loom::completion_event&) {});
        REQUIRE_FALSE(registered);
        REQUIRE(registered.error() == loom::errc::address_in_use);
        if (self.throw_on_submit) {
            throw std::bad_alloc{};
        }
        if (self.submission_status) {
            return self.submission_status;
        }
        self.buffers = {iov, count};
        self.context = context;
        self.completion_pending = true;
        self.completion_direction = direction;
        return 0;
    }

    fi_ops ops{.size = sizeof(fi_ops),
               .close = [](fid*) { return 0; },
               .bind = [](fid*, fid*, std::uint64_t) { return 0; }};
    fi_ops_cq completions{
        .size = sizeof(fi_ops_cq),
        .read = [](fid_cq* cq, void* output, std::size_t) -> ssize_t {
            auto& self = *static_cast<vector_provider*>(cq->fid.context);
            const auto receive = self.completion_direction == loom::completion_direction::receive;
            if (!self.completion_pending || (self.submission_flags & FI_MORE) ||
                cq != (receive ? &self.rx_handle : &self.tx_handle))
                return -FI_EAGAIN;
            self.completion_pending = false;
            *static_cast<fi_cq_tagged_entry*>(output) = {.op_context = self.context, .len = 6};
            return 1;
        },
    };
    fid_cq tx_handle{.fid = {.context = this, .ops = &ops}, .ops = &completions},
        rx_handle{.fid = {.context = this, .ops = &ops}, .ops = &completions};
    loom::completion_queue tx{loom::detail::make_pmr_unique<loom::detail::completion_queue_impl>(
        loom::get_default_resource(), &tx_handle)};
    loom::completion_queue rx{loom::detail::make_pmr_unique<loom::detail::completion_queue_impl>(
        loom::get_default_resource(), &rx_handle)};
    fi_ops_msg messages{
        .size = sizeof(fi_ops_msg),
        .recvv =
            [](fid_ep* ep, const iovec* iov, void**, std::size_t count, fi_addr_t, void* context) {
                return capture(ep, iov, count, context, loom::completion_direction::receive);
            },
        .recvmsg =
            [](fid_ep* ep, const fi_msg* msg, std::uint64_t flags) {
                return capture_message(ep, msg, flags, loom::completion_direction::receive);
            },
        .sendv =
            [](fid_ep* ep, const iovec* iov, void**, std::size_t count, fi_addr_t, void* context) {
                return capture(ep, iov, count, context);
            },
        .sendmsg =
            [](fid_ep* ep, const fi_msg* msg, std::uint64_t flags) {
                return capture_message(ep, msg, flags, loom::completion_direction::transmit);
            },
    };
    fi_ops_tagged tags{
        .size = sizeof(fi_ops_tagged),
        .recvv =
            [](fid_ep* ep,
               const iovec* iov,
               void**,
               std::size_t count,
               fi_addr_t,
               std::uint64_t tag,
               std::uint64_t ignore,
               void* context) {
                REQUIRE(tag == 42);
                REQUIRE(ignore == 0x80);
                return capture(ep, iov, count, context, loom::completion_direction::receive);
            },
        .recvmsg =
            [](fid_ep* ep, const fi_msg_tagged* msg, std::uint64_t flags) {
                REQUIRE(msg->tag == 42);
                REQUIRE(msg->ignore == 0x80);
                return capture_message(ep, msg, flags, loom::completion_direction::receive);
            },
        .sendv =
            [](fid_ep* ep,
               const iovec* iov,
               void**,
               std::size_t count,
               fi_addr_t,
               std::uint64_t tag,
               void* context) {
                REQUIRE(tag == 42);
                return capture(ep, iov, count, context);
            },
        .sendmsg =
            [](fid_ep* ep, const fi_msg_tagged* msg, std::uint64_t flags) {
                REQUIRE(msg->tag == 42);
                return capture_message(ep, msg, flags, loom::completion_direction::transmit);
            },
    };
    fi_ops_rma rma{
        .size = sizeof(fi_ops_rma),
        .readmsg = [](fid_ep* ep,
                      const fi_msg_rma* msg,
                      std::uint64_t flags) { return capture_rma(ep, msg, flags); },
        .writemsg = [](fid_ep* ep,
                       const fi_msg_rma* msg,
                       std::uint64_t flags) { return capture_rma(ep, msg, flags); },
    };
    fid_ep handle{
        .fid = {.context = this, .ops = &ops}, .msg = &messages, .rma = &rma, .tagged = &tags};

    auto endpoint(loom::cq_bind_flags bindings = loom::cq_bind::transmit | loom::cq_bind::recv,
                  std::uint64_t receive_defaults = FI_MORE,
                  std::uint64_t transmit_defaults = FI_FENCE) -> loom::endpoint {
        auto ep = loom::endpoint{
            loom::detail::make_pmr_unique<loom::detail::endpoint_impl>(loom::get_default_resource(),
                                                                       &handle,
                                                                       loom::endpoint_types::msg,
                                                                       loom::address_format::inet,
                                                                       0UZ,
                                                                       0UZ,
                                                                       loom::caps{},
                                                                       transmit_defaults,
                                                                       receive_defaults,
                                                                       &domain_handle,
                                                                       limits)};
        if (bindings.has(loom::cq_bind::transmit))
            REQUIRE(ep.bind_cq(tx, loom::cq_bind::transmit));
        if (bindings.has(loom::cq_bind::recv))
            REQUIRE(ep.bind_cq(rx, loom::cq_bind::recv));
        return ep;
    }
};
}  // namespace

TEST_CASE("Asio messages reject invalid per-buffer registrations", "[asio][registration]") {
    enum class defect { foreign, unknown, disabled, wrong_endpoint, short_range, unrelated, empty };
    const auto bad = GENERATE(defect::foreign,
                              defect::unknown,
                              defect::disabled,
                              defect::wrong_endpoint,
                              defect::short_range,
                              defect::unrelated,
                              defect::empty);
    const auto operand = GENERATE(0, 1);
    const auto receive = GENERATE(false, true);
    const auto tagged = GENERATE(false, true);
    const auto helper = GENERATE(false, true);
    vector_provider native;
    native.submission_status = -FI_EAGAIN;
    ::asio::io_context ioc;
    loom::asio::endpoint ep{ioc.get_executor(), native.endpoint()};
    fid_domain foreign{};
    fid_ep other_endpoint{};
    std::array<std::array<std::byte, 8>, 2> storage{};
    std::array<std::byte, 8> unrelated{};
    std::array<fid_mr, 2> handles{};
    std::array<loom::memory_region, 2> regions{};
    for (std::size_t i = 0; i < regions.size(); ++i) {
        handles[i] = {.fid = {.ops = &native.ops}};  // Null descriptors still require validation.
        auto impl = loom::detail::make_pmr_unique<loom::detail::memory_region_impl>(
            loom::get_default_resource(),
            i == operand && bad == defect::empty ? nullptr : &handles[i],
            i == operand && bad == defect::unrelated ? unrelated.data() : storage[i].data(),
            i == operand && bad == defect::short_range ? 7UZ : 8UZ,
            loom::detail::mr_key_lease{},
            0,
            i == operand && bad == defect::disabled ? FI_MR_ENDPOINT : 0,
            i == operand && bad == defect::foreign   ? &foreign
            : i == operand && bad == defect::unknown ? nullptr
                                                     : &native.domain_handle);
        if (i == operand && bad == defect::wrong_endpoint)
            impl->bound_endpoint = &other_endpoint;
        regions[i] = loom::memory_region{std::move(impl)};
    }
    const std::array buffers{
        loom::asio::mutable_registered_buffer{
            ::asio::buffer(storage[0]), &regions[0], loom::asio::registered_buffer_id{0}},
        loom::asio::mutable_registered_buffer{
            ::asio::buffer(storage[1]), &regions[1], loom::asio::registered_buffer_id{1}},
    };
    int calls{};
    const auto handler =
        ::asio::bind_executor(ioc.get_executor(), [&](std::error_code error, std::size_t bytes) {
            CHECK(error == loom::errc::invalid_argument);
            CHECK(bytes == 0);
            ++calls;
        });
    if (receive) {
        if (helper) {
            if (tagged)
                loom::asio::initiate_fabric_tagged_recv{ep.native_handle()}(
                    handler, buffers, 42, 0x80);
            else
                loom::asio::initiate_fabric_recv{ep.native_handle()}(handler, buffers);
        } else if (tagged)
            ep.async_tagged_receive(buffers, 42, 0x80, handler);
        else
            ep.async_receive(buffers, handler);
    } else if (helper) {
        if (tagged)
            loom::asio::initiate_fabric_tagged_send{ep.native_handle()}(handler, buffers, 42);
        else
            loom::asio::initiate_fabric_send{ep.native_handle()}(handler, buffers);
    } else if (tagged)
        ep.async_tagged_send(buffers, 42, handler);
    else
        ep.async_send(buffers, handler);
    CHECK(calls == 0);
    ioc.run();
    CHECK(calls == 1);
    CHECK(native.submission_flags == 0);
    CHECK(native.buffers.empty());
    CHECK_FALSE(native.completion_pending);
}

TEST_CASE("Asio RMA preserves registered peers and remote bounds", "[asio][endpoint]") {
    const auto read = GENERATE(false, true);
    const auto helper = GENERATE(false, true);
    const auto status = GENERATE(0, -FI_EAGAIN, -FI_EIO);
    enum class remote_range { valid, short_extent, overflow };
    const auto range =
        GENERATE(remote_range::valid, remote_range::short_extent, remote_range::overflow);
    enum class local_range {
        valid,
        empty,
        short_extent,
        before_start,
        unrelated,
        unmapped,
        foreign_domain,
        unknown_domain,
        disabled,
        local_iov_limit,
        remote_iov_limit
    };
    const auto local = GENERATE(local_range::valid,
                                local_range::empty,
                                local_range::short_extent,
                                local_range::before_start,
                                local_range::unrelated,
                                local_range::unmapped,
                                local_range::foreign_domain,
                                local_range::unknown_domain,
                                local_range::disabled,
                                local_range::local_iov_limit,
                                local_range::remote_iov_limit);
    const bool invalid_range = range != remote_range::valid || local != local_range::valid;
    const auto null_descriptor = GENERATE(false, true);
    vector_provider native;
    native.submission_status = status;
    if (local == local_range::local_iov_limit)
        native.limits.transmit = 1;
    if (local == local_range::remote_iov_limit)
        native.limits.rma = 0;
    ::asio::io_context ioc;
    loom::asio::endpoint ep{ioc.get_executor(),
                            native.endpoint(loom::cq_bind::transmit | loom::cq_bind::recv,
                                            FI_MORE,
                                            FI_FENCE | FI_MORE | FI_DELIVERY_COMPLETE)};
    std::array<std::byte, 8> storage{}, unrelated{};
    fid_domain foreign_domain{};
    fid_mr handle{.fid = {.ops = &native.ops}, .mem_desc = null_descriptor ? nullptr : &handle};
    loom::memory_region mr{loom::detail::make_pmr_unique<loom::detail::memory_region_impl>(
        loom::get_default_resource(),
        local == local_range::empty ? nullptr : &handle,
        local == local_range::unmapped ? nullptr
        : local == local_range::unrelated
            ? unrelated.data()
            : storage.data() + (local == local_range::before_start ? 1 : 0),
        storage.size() - (local == local_range::short_extent ? 1 : 0),
        loom::detail::mr_key_lease{},
        0,
        local == local_range::disabled ? FI_MR_ENDPOINT : 0,
        local == local_range::foreign_domain   ? &foreign_domain
        : local == local_range::unknown_domain ? nullptr
                                               : &native.domain_handle)};
    const loom::fabric_addr peer{23};
    const loom::remote_memory remote{
        range == remote_range::overflow ? std::numeric_limits<std::uint64_t>::max() - 7 : 32,
        0x123456789abcdef0ULL,
        range == remote_range::short_extent ? 7u : 8u};
    // The explicit RMA registration takes precedence over descriptors carried
    // by the buffer views, including when its native descriptor is null.
    fid_mr view_handle{.fid = {.ops = &native.ops}, .mem_desc = &view_handle};
    loom::memory_region view_mr{loom::detail::make_pmr_unique<loom::detail::memory_region_impl>(
        loom::get_default_resource(),
        &view_handle,
        storage.data(),
        storage.size(),
        loom::detail::mr_key_lease{},
        0,
        0,
        &native.domain_handle)};
    std::array buffers{
        loom::asio::mutable_registered_buffer{
            ::asio::buffer(storage.data(), 3), &view_mr, loom::asio::registered_buffer_id{0}},
        loom::asio::mutable_registered_buffer{
            ::asio::buffer(storage.data() + 3, 5), &view_mr, loom::asio::registered_buffer_id{1}}};
    int completed{};
    auto handler =
        ::asio::bind_executor(ioc.get_executor(), [&](std::error_code error, std::size_t bytes) {
            ++completed;
            if (invalid_range)
                REQUIRE(error == loom::errc::invalid_argument);
            else if (status)
                REQUIRE(error == loom::make_error_code_from_fi_errno(-status));
            else
                REQUIRE_FALSE(error);
            REQUIRE(bytes == (invalid_range || status ? 0 : storage.size()));
        });
    if (helper) {
        if (read)
            loom::asio::initiate_fabric_read{ep.native_handle()}(
                handler, buffers, mr, remote, peer);
        else
            loom::asio::initiate_fabric_write{ep.native_handle()}(
                handler, buffers, mr, remote, peer);
    } else if (read) {
        ep.async_read_from(buffers, mr, peer, remote, handler);
    } else {
        ep.async_write_to(buffers, mr, peer, remote, handler);
    }
    REQUIRE(completed == 0);
    if (!invalid_range) {
        CHECK(native.submission_flags ==
              (FI_COMPLETION | FI_FENCE | (read ? 0 : FI_DELIVERY_COMPLETE)));
        REQUIRE(native.peer == peer.get());
        REQUIRE(native.remote_address == remote.addr);
        REQUIRE(native.remote_key == remote.key);
        REQUIRE(native.remote_length == storage.size());
        if (null_descriptor)
            REQUIRE(native.descriptors.empty());
        else
            REQUIRE(native.descriptors == std::vector<void*>{&handle, &handle});
    }
    buffers = {};
    if (!invalid_range && !status) {
        REQUIRE(native.buffers.size() == 2);
        REQUIRE(native.buffers[0].iov_base == storage.data());
        REQUIRE(native.buffers[1].iov_base == storage.data() + 3);
        loom::completion_event event;
        event.context = loom::context_ptr<void>{native.context};
        REQUIRE_FALSE(native.rx.dispatch(event));
        REQUIRE(native.tx.dispatch(event));
        REQUIRE_FALSE(native.tx.dispatch(event));
    } else {
        REQUIRE(native.context == nullptr);
    }
    ioc.run();
    REQUIRE(completed == 1);
}

TEST_CASE("Asio operations register and complete without later submissions",
          "[asio][endpoint][batch]") {
    const auto operation = GENERATE(0, 1, 2, 3, 4, 5);
    const auto initiation_helper = GENERATE(false, true);
    const auto bound = GENERATE(false, true);
    const auto status = GENERATE(0, -FI_EAGAIN, -FI_EIO);
    const bool receive = operation == 1 || operation == 3;
    CAPTURE(operation, initiation_helper, bound, status);
    vector_provider native;
    native.submission_status = status;
    ::asio::io_context ioc;
    loom::asio::endpoint ep{ioc.get_executor(),
                            native.endpoint(bound ? loom::cq_bind::transmit | loom::cq_bind::recv
                                            : receive ? loom::cq_bind::transmit
                                                      : loom::cq_bind::recv,
                                            FI_MORE,
                                            FI_MORE | FI_FENCE | FI_DELIVERY_COMPLETE)};
    const loom::remote_memory remote{0x1000, 1, 4096};
    std::array<char, 6> storage{'a', 'b', 'c', 'd', 'e', 'f'};
    const auto first = std::span{storage}.first(2);
    const auto second = std::span{storage}.subspan(2);
    fid_mr handle{.fid = {.ops = &native.ops}, .mem_desc = &handle};
    loom::memory_region mr{loom::detail::make_pmr_unique<loom::detail::memory_region_impl>(
        loom::get_default_resource(),
        &handle,
        storage.data(),
        storage.size(),
        loom::detail::mr_key_lease{},
        0,
        0,
        &native.domain_handle)};
    std::array<::asio::mutable_buffer, 2> buffers{::asio::buffer(first.data(), first.size()),
                                                  ::asio::buffer(second.data(), second.size())};
    std::array<::asio::const_buffer, 2> const_buffers{buffers[0], buffers[1]};
    int completions{};
    const auto expected = !bound   ? loom::make_error_code(loom::errc::invalid_argument)
                          : status ? loom::make_error_code_from_fi_errno(-status)
                                   : std::error_code{};
    auto handler = ::asio::bind_executor(ioc.get_executor(),
                                         [&](std::error_code error, std::size_t transferred) {
                                             REQUIRE(error == expected);
                                             REQUIRE(transferred == (expected ? 0 : 6));
                                             ++completions;
                                         });
    if (initiation_helper) {
        switch (operation) {
            case 0:
                loom::asio::initiate_fabric_send{ep.native_handle()}(handler, const_buffers);
                break;
            case 1:
                loom::asio::initiate_fabric_recv{ep.native_handle()}(handler, buffers);
                break;
            case 2:
                loom::asio::initiate_fabric_tagged_send{ep.native_handle()}(
                    handler, const_buffers, 42);
                break;
            case 3:
                loom::asio::initiate_fabric_tagged_recv{ep.native_handle()}(
                    handler, buffers, 42, 0x80);
                break;
            case 4:
                loom::asio::initiate_fabric_write{ep.native_handle()}(
                    handler, const_buffers, mr, remote);
                break;
            case 5:
                loom::asio::initiate_fabric_read{ep.native_handle()}(handler, buffers, mr, remote);
                break;
        }
    } else {
        switch (operation) {
            case 0:
                ep.async_send(const_buffers, handler);
                break;
            case 1:
                ep.async_receive(buffers, handler);
                break;
            case 2:
                ep.async_tagged_send(const_buffers, 42, handler);
                break;
            case 3:
                ep.async_tagged_receive(buffers, 42, 0x80, handler);
                break;
            case 4:
                ep.async_write_to(const_buffers, mr, remote, handler);
                break;
            case 5:
                ep.async_read_from(buffers, mr, remote, handler);
                break;
        }
    }
    buffers = {};
    const_buffers = {};
    REQUIRE(completions == 0);
    if (bound) {
        CHECK(native.submission_flags == (FI_COMPLETION | (receive ? 0 : FI_FENCE) |
                                          (!receive && operation != 5 ? FI_DELIVERY_COMPLETE : 0)));
    }
    if (bound && operation < 4) {
        CHECK(native.descriptors.empty());
    }
    if (expected) {
        REQUIRE(native.context == nullptr);
        REQUIRE((native.attempted_context != nullptr) == bound);
        loom::completion_event event;
        event.context = loom::context_ptr<void>{native.attempted_context};
        REQUIRE_FALSE(native.tx.dispatch(event));
        REQUIRE_FALSE(native.rx.dispatch(event));
        ioc.run();
        REQUIRE(completions == 1);
        return;
    }
    REQUIRE(native.context != nullptr);
    REQUIRE(native.buffers.size() == 2);
    std::string sent;
    for (const auto& buffer : native.buffers) {
        sent.append(static_cast<const char*>(buffer.iov_base), buffer.iov_len);
    }
    REQUIRE(sent == "abcdef");
    if (operation == 1 || operation == 3 || operation == 5) {
        for (const auto& buffer : native.buffers) {
            std::memset(buffer.iov_base, 'x', buffer.iov_len);
        }
        REQUIRE(std::string(first.data(), first.size()) == "xx");
        REQUIRE(std::string(second.data(), second.size()) == "xxxx");
    }
    if (operation >= 4) {
        REQUIRE(native.remote_length == 6);
    }
    auto& queue = receive ? native.rx : native.tx;
    auto& other = receive ? native.tx : native.rx;
    auto completion = queue.poll();
    CHECK(completion.has_value());
    // Flush deferred pre-fix work before releasing the operation storage.
    if (!completion) {
        native.submission_flags &= ~FI_MORE;
        completion = queue.poll();
        REQUIRE(completion);
    }
    const auto& event = *completion;
    // The provider owns every scratch slot; routing must not inspect them.
    auto& scratch = *static_cast<fi_context2*>(native.context);
    for (auto& slot : scratch.internal)
        slot = &scratch;
    REQUIRE_FALSE(other.dispatch(event));
    REQUIRE(queue.dispatch(event));
    REQUIRE_FALSE(queue.dispatch(event));
    ioc.run();
    REQUIRE(completions == 1);
}

TEST_CASE("Asio messages report vector limit errors asynchronously", "[asio][endpoint][iov]") {
    const auto receive = GENERATE(false, true);
    const auto tagged = GENERATE(false, true);
    const auto helper = GENERATE(false, true);
    const auto limit = GENERATE(0UZ, 1UZ);
    vector_provider native;
    native.limits =
        receive ? loom::endpoint_iov_limits{64, limit, 1} : loom::endpoint_iov_limits{limit, 64, 1};
    ::asio::io_context ioc;
    loom::asio::endpoint ep{ioc.get_executor(), native.endpoint()};
    std::array<std::byte, 2> storage{};
    const std::array buffers{::asio::buffer(storage.data(), 1),
                             ::asio::buffer(storage.data() + 1, 1)};
    int completed{};
    const auto handler =
        ::asio::bind_executor(ioc.get_executor(), [&](std::error_code error, std::size_t bytes) {
            ++completed;
            CHECK(error == loom::errc::invalid_argument);
            CHECK(bytes == 0);
        });
    const loom::fabric_addr peer{19};
    if (receive) {
        if (helper) {
            if (tagged)
                loom::asio::initiate_fabric_tagged_recv{ep.native_handle()}(
                    handler, buffers, 42, 0, peer);
            else
                loom::asio::initiate_fabric_recv{ep.native_handle()}(handler, buffers, peer);
        } else if (tagged)
            ep.async_tagged_receive_from(buffers, peer, 42, 0, handler);
        else
            ep.async_receive_from(buffers, peer, handler);
    } else {
        if (helper) {
            if (tagged)
                loom::asio::initiate_fabric_tagged_send{ep.native_handle()}(
                    handler, buffers, 42, peer);
            else
                loom::asio::initiate_fabric_send{ep.native_handle()}(handler, buffers, peer);
        } else if (tagged)
            ep.async_tagged_send_to(buffers, peer, 42, handler);
        else
            ep.async_send_to(buffers, peer, handler);
    }
    CHECK(completed == 0);
    CHECK(native.attempted_context == nullptr);
    CHECK_FALSE(native.completion_pending);
    ioc.run();
    CHECK(completed == 1);
}

TEST_CASE("Asio messages preserve registered peers and descriptors", "[asio][endpoint][buffer]") {
    const auto receive = GENERATE(false, true);
    const auto tagged = GENERATE(false, true);
    const auto helper = GENERATE(false, true);
    const auto single = GENERATE(false, true);
    const auto count = single ? 1UZ : GENERATE(3UZ, 4UZ, 5UZ, 17UZ, 64UZ);
    const auto second_index = single ? 0UZ : GENERATE_COPY(1UZ, count - 1);
    const auto first_registered = GENERATE(false, true);
    vector_provider native;
    native.expected_message_peer = GENERATE(fi_addr_t{19}, FI_ADDR_UNSPEC);
    const loom::fabric_addr peer{native.expected_message_peer};
    ::asio::io_context ioc;
    loom::asio::endpoint ep{ioc.get_executor(), native.endpoint()};
    std::array<std::byte, 8> first{}, second{};
    fid_mr first_handle{.fid = {.ops = &native.ops}, .mem_desc = &first_handle};
    fid_mr second_handle{.fid = {.ops = &native.ops}, .mem_desc = &second_handle};
    loom::memory_region first_mr{loom::detail::make_pmr_unique<loom::detail::memory_region_impl>(
        loom::get_default_resource(),
        &first_handle,
        first.data(),
        first.size(),
        loom::detail::mr_key_lease{},
        0,
        0,
        &native.domain_handle)};
    loom::memory_region second_mr{loom::detail::make_pmr_unique<loom::detail::memory_region_impl>(
        loom::get_default_resource(),
        &second_handle,
        second.data(),
        second.size(),
        loom::detail::mr_key_lease{},
        0,
        0,
        &native.domain_handle)};
    const auto first_buffer = loom::asio::buffer(
        loom::asio::mutable_registered_buffer{::asio::buffer(first),
                                              first_registered ? &first_mr : nullptr,
                                              loom::asio::registered_buffer_id{0}} +
            1,
        2);
    const auto second_buffer = loom::asio::buffer(
        loom::asio::mutable_registered_buffer{
            ::asio::buffer(second), &second_mr, loom::asio::registered_buffer_id{1}} +
            2,
        4);
    std::vector buffers(count, first_buffer);
    if (!single)
        buffers[second_index] = second_buffer;
    int calls{};
    const auto handler =
        ::asio::bind_executor(ioc.get_executor(), [&](std::error_code error, std::size_t bytes) {
            ++calls;
            REQUIRE_FALSE(error);
            REQUIRE(bytes == (single ? 2 : receive ? 6 : 2 * count + 2));
        });
    const auto submit = [&](const auto& buffers) {
        if constexpr (::asio::is_mutable_buffer_sequence<
                          std::remove_cvref_t<decltype(buffers)>>::value) {
            if (receive) {
                if (helper) {
                    if (tagged)
                        loom::asio::initiate_fabric_tagged_recv{ep.native_handle()}(
                            handler, buffers, 42, 0x80, peer);
                    else
                        loom::asio::initiate_fabric_recv{ep.native_handle()}(
                            handler, buffers, peer);
                } else if (tagged)
                    ep.async_tagged_receive_from(buffers, peer, 42, 0x80, handler);
                else
                    ep.async_receive_from(buffers, peer, handler);
                return;
            }
        }
        if (helper) {
            if (tagged)
                loom::asio::initiate_fabric_tagged_send{ep.native_handle()}(
                    handler, buffers, 42, peer);
            else
                loom::asio::initiate_fabric_send{ep.native_handle()}(handler, buffers, peer);
        } else if (tagged)
            ep.async_tagged_send_to(buffers, peer, 42, handler);
        else
            ep.async_send_to(buffers, peer, handler);
    };
    if (receive) {
        if (single)
            submit(first_buffer);
        else
            submit(buffers);
    } else {
        // Both const and mutable registered buffers must model ConstBufferSequence.
        const auto as_const = GENERATE(false, true);
        if (as_const) {
            if (single)
                submit(loom::asio::const_registered_buffer{first_buffer});
            else
                submit(std::vector<loom::asio::const_registered_buffer>{buffers.begin(),
                                                                        buffers.end()});
        } else if (single)
            submit(first_buffer);
        else
            submit(buffers);
    }
    REQUIRE(calls == 0);
    REQUIRE(native.buffers.size() == count);
    for (std::size_t i = 0; i < count; ++i) {
        const bool is_second = !single && i == second_index;
        CHECK(native.buffers[i].iov_base == (is_second ? second.data() + 2 : first.data() + 1));
        CHECK(native.buffers[i].iov_len == (is_second ? 4 : 2));
    }
    const auto first_descriptor = first_registered ? first_mr.descriptor().raw() : nullptr;
    if (single && !first_registered) {
        CHECK(native.descriptors.empty());
    } else {
        REQUIRE(native.descriptors.size() == native.buffers.size());
        for (std::size_t i = 0; i < count; ++i)
            CHECK(native.descriptors[i] ==
                  (!single && i == second_index ? second_mr.descriptor().raw() : first_descriptor));
    }
    loom::completion_event event{.context = loom::context_ptr<void>{native.context},
                                 .bytes_transferred = single ? 2UZ : 6UZ};
    REQUIRE((receive ? native.rx : native.tx).dispatch(event));
    ioc.run();
    REQUIRE(calls == 1);
}

TEST_CASE("Asio receives require completion lengths", "[asio][endpoint]") {
    const auto tagged = GENERATE(false, true);
    const auto helper = GENERATE(false, true);
    const auto format =
        GENERATE(FI_CQ_FORMAT_CONTEXT, FI_CQ_FORMAT_MSG, FI_CQ_FORMAT_DATA, FI_CQ_FORMAT_TAGGED);
    vector_provider native;
    native.rx =
        loom::completion_queue{loom::detail::make_pmr_unique<loom::detail::completion_queue_impl>(
            loom::get_default_resource(),
            &native.rx_handle,
            loom::runtime_progress_policy{},
            false,
            format)};
    native.tx =
        loom::completion_queue{loom::detail::make_pmr_unique<loom::detail::completion_queue_impl>(
            loom::get_default_resource(),
            &native.tx_handle,
            loom::runtime_progress_policy{},
            false,
            format)};
    ::asio::io_context ioc;
    loom::asio::endpoint ep{ioc.get_executor(), native.endpoint()};
    std::array<std::byte, 16> storage{};
    int calls{};
    std::error_code result;
    std::size_t bytes{};
    auto handler =
        ::asio::bind_executor(ioc.get_executor(), [&](std::error_code error, std::size_t count) {
            ++calls;
            result = error;
            bytes = count;
        });
    if (helper) {
        if (tagged)
            loom::asio::initiate_fabric_tagged_recv{ep.native_handle()}(
                handler, ::asio::buffer(storage), 42, 0x80);
        else
            loom::asio::initiate_fabric_recv{ep.native_handle()}(handler, ::asio::buffer(storage));
    } else if (tagged)
        ep.async_tagged_receive(::asio::buffer(storage), 42, 0x80, handler);
    else
        ep.async_receive(::asio::buffer(storage), handler);
    const auto rejected = format == FI_CQ_FORMAT_CONTEXT;
    CHECK((native.attempted_context == nullptr) == rejected);
    REQUIRE(calls == 0);
    if (native.context) {
        loom::completion_event event{.context = loom::context_ptr<void>{native.context},
                                     .bytes_transferred = 7};
        REQUIRE(native.rx.dispatch(event));
    }
    ioc.run();
    REQUIRE(calls == 1);
    CHECK(result ==
          (rejected ? loom::make_error_code(loom::errc::not_supported) : std::error_code{}));
    CHECK(bytes == (rejected ? 0 : 7));

    calls = 0;
    ioc.restart();
    ep.async_send(::asio::buffer(storage), handler);
    REQUIRE(calls == 0);
    loom::completion_event sent{.context = loom::context_ptr<void>{native.context}};
    REQUIRE(native.tx.dispatch(sent));
    ioc.run();
    REQUIRE(calls == 1);
    REQUIRE_FALSE(result);
    REQUIRE(bytes == storage.size());
}

TEST_CASE("Asio messages transfer registered buffers to RDM peers", "[asio][endpoint][loopback]") {
    using namespace loom;
    const auto tagged = GENERATE(false, true);
    const auto source_filter = GENERATE(false, true);
    fabric_hints hints{};
    hints.provider_name = "sockets";
    hints.ep_type = endpoint_types::rdm;
    hints.capabilities = capability::msg | capability::tagged | capability::directed_recv;
    auto info = query_fabric(hints);
    if (!info)
        SKIP("Sockets provider unavailable");
    auto fab = fabric::create(*info).value();
    auto dom = domain::create(fab, *info).value();
    auto cq = completion_queue::create(dom).value();
    auto av = address_vector::create(dom).value();
    std::array<endpoint, 2> endpoints;
    std::array<fabric_addr, 2> peers;
    for (std::size_t i = 0; i < endpoints.size(); ++i) {
        auto ep = endpoint::create(dom, *info).value();
        REQUIRE(ep.bind_cq(cq, cq_bind::transmit | cq_bind::recv | cq_bind::selective_completion));
        REQUIRE(ep.bind_av(av));
        REQUIRE(ep.enable());
        peers[i] = fabric_addr{av.insert(ep.get_local_address().value()).value().value};
        endpoints[i] = std::move(ep);
    }
    ::asio::io_context ioc;
    loom::asio::endpoint sender{ioc.get_executor(), std::move(endpoints[0])};
    loom::asio::endpoint receiver{ioc.get_executor(), std::move(endpoints[1])};
    std::array<std::byte, 7> outgoing;
    outgoing.fill(std::byte{0x5a});
    std::array<std::byte, 16> incoming{};
    const std::array send_buffers{::asio::buffer(outgoing.data(), 3),
                                  ::asio::buffer(outgoing.data() + 3, 4)};
    const std::array recv_buffers{::asio::buffer(incoming.data(), 4),
                                  ::asio::buffer(incoming.data() + 4, 12)};
    auto send_mrs =
        loom::asio::register_buffers(ioc, dom, send_buffers, mr_access_flags::send).value();
    auto recv_mrs =
        loom::asio::register_buffers(ioc, dom, recv_buffers, mr_access_flags::recv).value();
    const std::array sends{send_mrs[0], send_mrs[1]};
    const std::array recvs{recv_mrs[0], recv_mrs[1]};
    std::array<int, 2> calls{};
    std::array<std::error_code, 2> errors{};
    std::array<std::size_t, 2> sizes{};
    const auto handler = [&](std::size_t i) {
        return [&, i](std::error_code error, std::size_t bytes) {
            ++calls[i];
            errors[i] = error;
            sizes[i] = bytes;
        };
    };
    const auto source = source_filter ? peers[0] : fabric_addrs::unspecified;
    if (tagged) {
        receiver.async_tagged_receive_from(recvs, source, 42, 0, handler(1));
        sender.async_tagged_send_to(sends, peers[1], 42, handler(0));
    } else {
        receiver.async_receive_from(recvs, source, handler(1));
        sender.async_send_to(sends, peers[1], handler(0));
    }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
    bool unhandled{};
    while ((calls[0] == 0 || calls[1] == 0) && std::chrono::steady_clock::now() < deadline) {
        while (auto event = cq.poll()) {
            if (!cq.dispatch(*event)) {
                unhandled = true;
                break;
            }
        }
        ioc.poll();
        if (unhandled)
            break;
        std::this_thread::yield();
    }
    if (calls[0] == 0 || calls[1] == 0 || unhandled) {
        REQUIRE(sender.native_handle()->close());
        REQUIRE(receiver.native_handle()->close());
        FAIL("Asio RDM message did not complete");
    }
    REQUIRE(calls == std::array{1, 1});
    REQUIRE_FALSE(errors[0]);
    REQUIRE_FALSE(errors[1]);
    REQUIRE(sizes == std::array{outgoing.size(), outgoing.size()});
    REQUIRE(std::ranges::equal(outgoing, std::span{incoming}.first(outgoing.size())));
    REQUIRE(std::ranges::all_of(std::span{incoming}.subspan(outgoing.size()),
                                [](std::byte value) { return value == std::byte{}; }));
}

TEST_CASE("Asio receives reject multi-receive defaults", "[asio][endpoint]") {
    const auto tagged = GENERATE(false, true);
    const auto helper = GENERATE(false, true);
    const auto defaults = GENERATE(FI_MULTI_RECV, FI_MULTI_RECV | FI_MORE);
    vector_provider native;
    ::asio::io_context ioc;
    loom::asio::endpoint ep{
        ioc.get_executor(),
        native.endpoint(loom::cq_bind::transmit | loom::cq_bind::recv, defaults)};
    std::array<std::byte, 16> storage{};
    int calls{};
    std::error_code result;
    std::size_t bytes{};
    auto handler =
        ::asio::bind_executor(ioc.get_executor(), [&](std::error_code error, std::size_t count) {
            ++calls;
            result = error;
            bytes = count;
        });
    if (helper) {
        if (tagged)
            loom::asio::initiate_fabric_tagged_recv{ep.native_handle()}(
                handler, ::asio::buffer(storage), 42, 0x80);
        else
            loom::asio::initiate_fabric_recv{ep.native_handle()}(handler, ::asio::buffer(storage));
    } else if (tagged) {
        ep.async_tagged_receive(::asio::buffer(storage), 42, 0x80, handler);
    } else {
        ep.async_receive(::asio::buffer(storage), handler);
    }
    CHECK(native.attempted_context == nullptr);
    REQUIRE(calls == 0);
    // Clean up an erroneous pre-fix submission with its first receive event.
    // That event must not release a real multi-receive buffer or its context.
    if (native.context) {
        loom::completion_event event{.context = loom::context_ptr<void>{native.context},
                                     .bytes_transferred = 7};
        REQUIRE(native.rx.dispatch(event));
    }
    ioc.run();
    REQUIRE(calls == 1);
    CHECK(result == loom::errc::not_supported);
    CHECK(bytes == 0);
    REQUIRE(ioc.poll() == 0);

    calls = 0;
    ioc.restart();
    ep.async_send(::asio::buffer(storage), handler);
    REQUIRE(native.submission_flags == (FI_COMPLETION | FI_FENCE));
    REQUIRE(calls == 0);
    loom::completion_event sent{.context = loom::context_ptr<void>{native.context}};
    REQUIRE(native.tx.dispatch(sent));
    ioc.run();
    REQUIRE(calls == 1);
    REQUIRE_FALSE(result);
    REQUIRE(bytes == storage.size());
}

TEST_CASE("Asio initiation exceptions release the pending context", "[asio][endpoint]") {
    vector_provider native;
    native.throw_on_submit = true;
    ::asio::io_context ioc;
    loom::asio::endpoint ep{ioc.get_executor(), native.endpoint()};
    ::asio::cancellation_signal cancellation;
    std::array<std::byte, 1> storage{};
    auto handler =
        ::asio::bind_cancellation_slot(cancellation.slot(), [](std::error_code, std::size_t) {
            FAIL("No operation was submitted");
        });
    REQUIRE_THROWS_AS(ep.async_send(::asio::buffer(storage), handler), std::bad_alloc);
    REQUIRE_FALSE(cancellation.slot().has_handler());
    REQUIRE(native.attempted_context != nullptr);
    loom::completion_event event;
    event.context = loom::context_ptr<void>{native.attempted_context};
    REQUIRE_FALSE(native.tx.dispatch(event));
    REQUIRE(ioc.run() == 0);
}

TEST_CASE("Asio initiation rejects missing native endpoints asynchronously", "[asio][endpoint]") {
    ::asio::io_context ioc;
    std::array<std::byte, 1> storage{};
    int completions{};
    auto handler =
        ::asio::bind_executor(ioc.get_executor(), [&](std::error_code error, std::size_t bytes) {
            REQUIRE(error == loom::errc::invalid_argument);
            REQUIRE(bytes == 0);
            ++completions;
        });
    loom::asio::initiate_fabric_send{nullptr}(handler, ::asio::buffer(storage));
    REQUIRE(completions == 0);
    ioc.run();
    REQUIRE(completions == 1);
}
