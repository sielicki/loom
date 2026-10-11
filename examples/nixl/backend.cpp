// SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only
#include <loom/async/completion_queue.hpp>
#include <loom/core/endpoint_contract.hpp>
#include <loom/loom.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <ranges>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

#include <backend/backend_plugin.h>

namespace {
using namespace std::chrono_literals;

template <typename T>
auto checked(loom::result<T> result) -> T {
    if (!result)
        throw std::system_error(result.error());
    if constexpr (!std::is_void_v<T>)
        return std::move(*result);
}

template <typename F>
auto protect(F&& function) noexcept -> nixl_status_t {
    try {
        return std::forward<F>(function)();
    } catch (const std::invalid_argument&) {
        return NIXL_ERR_INVALID_PARAM;
    } catch (const std::exception& error) {
        NIXL_ERROR << "Loom backend: " << error.what();
        return NIXL_ERR_BACKEND;
    }
}

auto option(const nixlBackendInitParams& params, const std::string& key, std::string fallback)
    -> std::string {
    if (params.customParams) {
        if (auto found = params.customParams->find(key); found != params.customParams->end())
            return found->second;
    }
    return fallback;
}

enum class wire_kind : std::uint64_t {
    connection_v1 = 0x4c4f4f4d0101,
    memory_v2 = 0x4c4f4f4d0202,
    notification_v1 = 0x4c4f4f4d0103
};

template <std::size_t N>
auto encode(const std::array<std::uint64_t, N>& words) -> std::string {
    std::string bytes(N * 8, '\0');
    for (std::size_t i = 0; i < N; ++i)
        for (std::size_t j = 0; j < 8; ++j)
            bytes[i * 8 + j] = static_cast<char>(words[i] >> (56 - 8 * j));
    return bytes;
}

template <std::size_t N>
auto decode(std::string_view bytes) -> std::optional<std::array<std::uint64_t, N>> {
    if (bytes.size() != N * 8)
        return std::nullopt;
    std::array<std::uint64_t, N> words{};
    for (std::size_t i = 0; i < N; ++i)
        for (std::size_t j = 0; j < 8; ++j)
            words[i] = (words[i] << 8) | static_cast<unsigned char>(bytes[i * 8 + j]);
    return words;
}

auto covers(std::uint64_t base,
            std::uint64_t size,
            std::uint64_t address,
            std::uint64_t length) noexcept -> bool {
    return address >= base && address - base <= size && length <= size - (address - base);
}

struct peer;
enum class message_phase { waiting, accepted, finished };
struct notification {
    std::array<std::byte, 8192> bytes{};
    std::size_t length{};
    loom::memory_region region;
    loom::request_context context;
    loom::completion_registration completion;
    std::shared_ptr<peer> destination;
    message_phase phase{message_phase::waiting};
    nixl_status_t error{NIXL_SUCCESS};
};

struct transport_profile : loom::message_endpoint_profile<loom::rdm_endpoint_tag> {
    static constexpr loom::caps required_caps =
        loom::message_endpoint_profile<loom::rdm_endpoint_tag>::required_caps |
        loom::capability::rma | loom::capability::read | loom::capability::write |
        loom::capability::remote_read | loom::capability::remote_write;
    static constexpr loom::mode supported_modes =
        loom::mode_bits::context | loom::mode_bits::context2;
    static constexpr loom::threading_mode threading = loom::threading_mode::safe;
};

struct resources {
    loom::fabric_info info;
    loom::fabric fabric;
    loom::domain domain;
    loom::completion_queue cq;
    loom::address_vector av;
    loom::endpoint endpoint;
    // The engine retires receives before releasing these registrations. Bound
    // MRs must be destroyed before their endpoint, including construction failure.
    std::vector<std::unique_ptr<notification>> receives;
    loom::ipv4_address address;
    loom::mr_mode registration_mode{};
    std::uint64_t protocol{};
    std::size_t max_transfer{};
    std::size_t max_message{};

    explicit resources(const nixlBackendInitParams& params) {
        const auto provider = option(params, "provider", "sockets");
        const auto device = option(params, "domain", "lo");
        loom::fabric_hints hints;
        hints.provider_name = provider;
        hints.domain_name = device;
        hints.ep_type = transport_profile::ep_type;
        hints.addr_format = loom::address_format::inet;
        hints.threading = transport_profile::threading;
        hints.mode_bits = transport_profile::supported_modes;
        hints.capabilities = transport_profile::required_caps;
        info = checked(loom::query_fabric(hints));
        const auto dom = info.get_domain_attr();
        const auto ep = info.get_endpoint_attr();
        if (!dom || !ep || info.get_address_format() != loom::address_format::inet)
            throw std::invalid_argument("unsupported endpoint contract");
        registration_mode = dom->memory_region_mode;
        if (registration_mode.has_any(loom::mr_mode_flags::raw | loom::mr_mode_flags::mmu_notify |
                                      loom::mr_mode_flags::rma_event))
            throw std::invalid_argument("unsupported registration contract");
        protocol = ep->protocol.get();
        max_message = ep->max_msg_size.get();
        fabric = checked(loom::fabric::create(info));
        domain = checked(loom::domain::create(fabric, info));
        cq = checked(loom::completion_queue::create(domain, {.format = loom::cq_format::data}));
        av = checked(loom::address_vector::create(domain));
        // Retain the validated profile's guarantees while using the generic RMA
        // and registered-message APIs. This endpoint stays private.
        endpoint =
            checked(loom::contract_endpoint<transport_profile>::create(domain, info)).release();
        checked(endpoint.bind_cq(cq,
                                 loom::cq_bind::transmit | loom::cq_bind::recv |
                                     loom::cq_bind::selective_completion));
        checked(endpoint.bind_av(av));
        checked(endpoint.enable());
        const auto local = checked(endpoint.get_local_address());
        const auto* ipv4 = std::get_if<loom::ipv4_address>(&local);
        if (!ipv4)
            throw std::invalid_argument("expected an IPv4 endpoint address");
        address = *ipv4;
        max_transfer = loom::rma::get_max_rma_size(endpoint);
        if (!max_transfer || !max_message)
            throw std::invalid_argument("missing RMA size limit");
    }

    auto register_buffer(std::span<std::byte> buffer, loom::mr_access access)
        -> loom::memory_region {
        auto region = checked(loom::memory_region::register_memory(domain, buffer, access));
        if (registration_mode.has(loom::mr_mode_flags::endpoint)) {
            checked(region.bind(endpoint));
            checked(region.enable());
        }
        return region;
    }
};

enum class memory_space : std::uint64_t { host };
struct memory_identity {
    memory_space space;
    std::uint64_t device;
    auto operator==(const memory_identity&) const -> bool = default;
};

struct local_memory {
    std::shared_ptr<resources> fabric;
    loom::memory_region region;
    std::uint64_t base;
    memory_identity identity;
};

struct local_metadata final : nixlBackendMD {
    std::shared_ptr<local_memory> memory;
    explicit local_metadata(std::shared_ptr<local_memory> value)
        : nixlBackendMD(true), memory(std::move(value)) {}
};

struct peer {
    std::shared_ptr<resources> fabric;
    std::optional<loom::av_handle> handle;
    bool connected{true};
    ~peer() {
        if (handle)
            (void)fabric->av.remove(*handle);
    }
};

struct remote_metadata final : nixlBackendMD {
    std::shared_ptr<peer> destination;
    std::uint64_t base;
    loom::remote_memory region;
    memory_identity identity;
    remote_metadata(std::shared_ptr<peer> target,
                    std::uint64_t address,
                    loom::remote_memory memory,
                    memory_identity device)
        : nixlBackendMD(false), destination(std::move(target)), base(address), region(memory),
          identity(device) {}
};

struct request;
struct segment {
    request* owner{};
    std::shared_ptr<local_memory> memory;
    std::shared_ptr<peer> destination;
    std::span<std::byte> buffer;
    loom::remote_memory remote;
    loom::request_context context;
    loom::completion_registration completion;
};

enum class request_phase { prepared, active, finished };
struct request final : nixlBackendReqH {
    nixl_xfer_op_t operation;
    std::vector<std::unique_ptr<segment>> segments;
    request_phase phase{request_phase::prepared};
    std::size_t next{};
    std::size_t pending{};
    nixl_status_t error{NIXL_SUCCESS};
    std::shared_ptr<notification> message;
    explicit request(nixl_xfer_op_t op) : operation(op) {}

    auto native_pending() const noexcept -> bool {
        return pending || (message && message->phase == message_phase::accepted);
    }

    auto status() const noexcept -> nixl_status_t {
        if (phase == request_phase::prepared)
            return NIXL_ERR_NOT_POSTED;
        if (native_pending() ||
            (error == NIXL_SUCCESS &&
             (next != segments.size() || (message && message->phase != message_phase::finished))))
            return NIXL_IN_PROG;
        return error != NIXL_SUCCESS ? error : message ? message->error : NIXL_SUCCESS;
    }
};

class engine final : public nixlBackendEngine {
    static constexpr std::size_t request_limit = 64;
    static constexpr std::size_t segment_limit = 4096;
    static constexpr std::size_t progress_budget = 64;
    std::shared_ptr<resources> fabric_;
    mutable std::mutex mutex_;
    mutable std::condition_variable changed_;
    std::unordered_map<std::string, std::shared_ptr<peer>> peers_;
    mutable std::unordered_map<nixlBackendReqH*, std::unique_ptr<request>> requests_;
    mutable bool queue_failed_{};
    mutable bool notification_failed_{};
    bool stop_receives_{};
    mutable std::vector<std::shared_ptr<notification>> outgoing_;
    mutable notif_list_t notifications_;
    std::jthread progress_thread_;

    static void complete(void* object, const loom::completion_event& event) noexcept {
        auto& work = *static_cast<segment*>(object)->owner;
        --work.pending;
        if (event.has_error() && work.error == NIXL_SUCCESS)
            work.error = NIXL_ERR_BACKEND;
    }

    static void complete_message(void* object, const loom::completion_event& event) noexcept {
        auto& message = *static_cast<notification*>(object);
        message.phase = message_phase::finished;
        message.error = event.has_error() ? NIXL_ERR_BACKEND : NIXL_SUCCESS;
        if (!message.destination)
            message.length = event.bytes_transferred;
    }

    auto make_notification(std::shared_ptr<peer> destination, std::string_view payload) const
        -> std::shared_ptr<notification> {
        auto message = std::make_shared<notification>();
        constexpr std::size_t header_size = 24;
        const auto limit = std::min(message->bytes.size(), fabric_->max_message);
        if (limit < header_size || localAgent.size() > limit - header_size ||
            payload.size() > limit - header_size - localAgent.size())
            throw std::invalid_argument("notification exceeds the message limit");
        const auto header = encode(std::array{std::to_underlying(wire_kind::notification_v1),
                                              std::uint64_t{localAgent.size()},
                                              std::uint64_t{payload.size()}});
        auto* output = reinterpret_cast<char*>(message->bytes.data());
        std::ranges::copy(header, output);
        std::ranges::copy(localAgent, output + header_size);
        std::ranges::copy(payload, output + header_size + localAgent.size());
        message->length = header_size + localAgent.size() + payload.size();
        message->destination = std::move(destination);
        message->region = fabric_->register_buffer(message->bytes, loom::mr_access_flags::send);
        return message;
    }

    void post_message(notification& message) const {
        if (message.phase != message_phase::waiting)
            return;
        if (message.destination && !message.destination->connected) {
            message.error = NIXL_ERR_REMOTE_DISCONNECT;
            message.phase = message_phase::finished;
            return;
        }
        message.context = loom::request_context{};
        checked(message.completion.bind(fabric_->cq,
                                        loom::context_ptr<void>{message.context.context_ptr()},
                                        &message,
                                        complete_message));
        const iovec buffer{message.bytes.data(),
                           message.destination
                               ? message.length
                               : std::min(message.bytes.size(), fabric_->max_message)};
        const auto descriptor = message.region.descriptor();
        const auto posted =
            message.destination
                ? loom::msg::sendmsg(
                      fabric_->endpoint,
                      loom::msg::send_message<>{
                          .iov = {&buffer, 1},
                          .desc = {&descriptor, 1},
                          .dest_addr = loom::fabric_addr{message.destination->handle->value},
                          .context = message.context.context_ptr()},
                      loom::msg::send_flag::completion)
                : loom::msg::recvmsg(
                      fabric_->endpoint,
                      loom::msg::recv_message<>{.iov = {&buffer, 1},
                                                .desc = {&descriptor, 1},
                                                .context = message.context.context_ptr()},
                      loom::msg::recv_flag::completion);
        if (posted) {
            message.phase = message_phase::accepted;
        } else {
            message.completion.reset();
            if (posted.error() != loom::errc::again) {
                message.error = NIXL_ERR_BACKEND;
                message.phase = message_phase::finished;
                if (!message.destination)
                    queue_failed_ = true;
            }
        }
    }

    void receive_notifications() const {
        if (stop_receives_)
            return;
        for (auto& slot : fabric_->receives) {
            if (slot->phase == message_phase::finished) {
                if (notifications_.size() >= request_limit)
                    continue;
                const auto size = slot->length;
                std::string_view bytes{reinterpret_cast<const char*>(slot->bytes.data()),
                                       std::min(size, slot->bytes.size())};
                const auto header =
                    bytes.size() >= 24 ? decode<3>(bytes.substr(0, 24)) : std::nullopt;
                if (slot->error != NIXL_SUCCESS || !header || size > slot->bytes.size() ||
                    (*header)[0] != std::to_underlying(wire_kind::notification_v1) ||
                    (*header)[1] > bytes.size() - 24 ||
                    (*header)[2] != bytes.size() - 24 - (*header)[1]) {
                    notification_failed_ = true;
                } else {
                    notifications_.emplace_back(bytes.substr(24, (*header)[1]),
                                                bytes.substr(24 + (*header)[1], (*header)[2]));
                }
                slot->phase = message_phase::waiting;
                slot->error = NIXL_SUCCESS;
            }
            post_message(*slot);
            if (queue_failed_)
                return;
        }
    }

    void progress_locked() const {
        for (std::size_t i = 0; i < progress_budget; ++i) {
            auto event = fabric_->cq.poll();
            if (!event)
                break;
            if (!fabric_->cq.dispatch(*event)) {
                // This CQ belongs exclusively to this backend. An unattributed
                // error cannot be assigned to an arbitrary request.
                queue_failed_ = true;
                break;
            }
        }
        if (queue_failed_) {
            // Native close must succeed before retiring contexts without CQ events.
            if (auto closed = fabric_->endpoint.close(); closed) {
                for (auto& [_, work] : requests_) {
                    work->error = NIXL_ERR_BACKEND;
                    work->pending = 0;
                    work->phase = request_phase::finished;
                    for (auto& part : work->segments)
                        part->completion.reset();
                    if (work->message) {
                        work->message->completion.reset();
                        work->message->phase = message_phase::finished;
                        work->message->error = NIXL_ERR_BACKEND;
                    }
                }
                for (auto& message : outgoing_) {
                    message->completion.reset();
                    message->phase = message_phase::finished;
                    message->error = NIXL_ERR_BACKEND;
                }
                for (auto& slot : fabric_->receives) {
                    slot->completion.reset();
                    slot->phase = message_phase::finished;
                }
            }
        } else {
            receive_notifications();
            if (queue_failed_)
                return;
            for (auto& message : outgoing_)
                post_message(*message);
            std::erase_if(outgoing_, [this](const auto& message) {
                if (message->phase != message_phase::finished)
                    return false;
                if (message->error != NIXL_SUCCESS)
                    notification_failed_ = true;
                return true;
            });
            for (auto& [_, work] : requests_) {
                if (work->phase != request_phase::active)
                    continue;
                if (work->error == NIXL_SUCCESS && work->next < work->segments.size()) {
                    auto& part = *work->segments[work->next];
                    if (!part.destination->connected) {
                        work->error = NIXL_ERR_REMOTE_DISCONNECT;
                        continue;
                    }
                    part.context = loom::request_context{};
                    const auto bound =
                        part.completion.bind(fabric_->cq,
                                             loom::context_ptr<void>{part.context.context_ptr()},
                                             &part,
                                             complete);
                    if (!bound) {
                        work->error = NIXL_ERR_BACKEND;
                        continue;
                    }
                    const loom::fabric_addr destination{part.destination->handle->value};
                    const auto submitted =
                        work->operation == NIXL_WRITE
                            ? loom::rma::write_to(fabric_->endpoint,
                                                  part.buffer,
                                                  part.memory->region,
                                                  destination,
                                                  part.remote,
                                                  loom::op_flag::completion |
                                                      loom::op_flag::delivery_complete,
                                                  part.context.context_ptr())
                            : loom::rma::read_to(fabric_->endpoint,
                                                 part.buffer,
                                                 part.memory->region,
                                                 destination,
                                                 part.remote,
                                                 loom::op_flag::completion,
                                                 part.context.context_ptr());
                    if (submitted) {
                        ++work->next;
                        ++work->pending;
                    } else {
                        part.completion.reset();
                        if (submitted.error() != loom::errc::again)
                            work->error = NIXL_ERR_BACKEND;
                    }
                }
                if (work->error == NIXL_SUCCESS && work->next == work->segments.size() &&
                    work->pending == 0 && work->message)
                    post_message(*work->message);
                if (work->status() != NIXL_IN_PROG)
                    work->phase = request_phase::finished;
            }
        }
        changed_.notify_all();
    }

public:
    explicit engine(const nixlBackendInitParams* params)
        : nixlBackendEngine(params), fabric_(std::make_shared<resources>(*params)) {
        for (unsigned i = 0; i < 8; ++i) {
            auto slot = std::make_unique<notification>();
            slot->region = fabric_->register_buffer(slot->bytes, loom::mr_access_flags::recv);
            fabric_->receives.push_back(std::move(slot));
        }
        progress_thread_ = std::jthread([this](std::stop_token stop) {
            std::unique_lock lock(mutex_);
            while (!stop.stop_requested()) {
                try {
                    progress_locked();
                } catch (...) {
                    queue_failed_ = true;
                }
                changed_.wait_for(lock, 1ms);
            }
        });
    }

    ~engine() override {
        // Stop queued work and drain accepted operations while the worker and
        // all native contexts, registrations and endpoints remain alive.
        {
            std::unique_lock lock(mutex_);
            for (auto& [_, work] : requests_)
                if (work->phase == request_phase::active)
                    work->error = NIXL_ERR_CANCELED;
            for (auto& message : outgoing_)
                if (message->phase == message_phase::waiting)
                    message->phase = message_phase::finished;
            changed_.wait(lock, [this] {
                for (const auto& [_, work] : requests_)
                    if (work->native_pending())
                        return false;
                for (const auto& message : outgoing_)
                    if (message->phase == message_phase::accepted)
                        return false;
                return true;
            });
            stop_receives_ = true;
            for (const auto& slot : fabric_->receives)
                if (slot->phase == message_phase::accepted)
                    (void)fabric_->endpoint.cancel(
                        loom::context_ptr<void>{slot->context.context_ptr()});
            changed_.wait(lock, [this] {
                return std::ranges::none_of(fabric_->receives, [](const auto& slot) {
                    return slot->phase == message_phase::accepted;
                });
            });
            requests_.clear();
            outgoing_.clear();
            fabric_->receives.clear();
            // Failed close retains the endpoint; keep driving progress until
            // native ownership can be released without abandoning live storage.
            while (!fabric_->endpoint.close())
                changed_.wait_for(lock, 1ms);
        }
        progress_thread_.request_stop();
        changed_.notify_all();
        progress_thread_.join();
    }

    auto supportsRemote() const -> bool override { return true; }
    auto supportsLocal() const -> bool override { return false; }
    auto supportsNotif() const -> bool override { return true; }
    auto getSupportedMems() const -> nixl_mem_list_t override { return {DRAM_SEG}; }

    auto registerMem(const nixlBlobDesc& memory, const nixl_mem_t& type, nixlBackendMD*& output)
        -> nixl_status_t override {
        output = nullptr;
        if (type != DRAM_SEG)
            return NIXL_ERR_NOT_SUPPORTED;
        if (!memory.addr || !memory.len ||
            memory.len > std::numeric_limits<std::uintptr_t>::max() - memory.addr)
            return NIXL_ERR_INVALID_PARAM;
        return protect([&] {
            std::lock_guard lock(mutex_);
            auto region = fabric_->register_buffer(
                {reinterpret_cast<std::byte*>(memory.addr), memory.len},
                loom::mr_access_flags::read | loom::mr_access_flags::write |
                    loom::mr_access_flags::remote_read | loom::mr_access_flags::remote_write);
            auto local =
                std::make_shared<local_memory>(fabric_,
                                               std::move(region),
                                               memory.addr,
                                               memory_identity{memory_space::host, memory.devId});
            output = new local_metadata(std::move(local));
            return NIXL_SUCCESS;
        });
    }

    auto deregisterMem(nixlBackendMD* metadata) -> nixl_status_t override {
        std::lock_guard lock(mutex_);
        delete metadata;
        return NIXL_SUCCESS;
    }
    auto unloadMD(nixlBackendMD* metadata) -> nixl_status_t override {
        return deregisterMem(metadata);
    }

    auto getPublicData(const nixlBackendMD* metadata, std::string& output) const
        -> nixl_status_t override {
        const auto* local = dynamic_cast<const local_metadata*>(metadata);
        if (!local)
            return NIXL_ERR_INVALID_PARAM;
        return protect([&] {
            const auto& region = local->memory->region;
            output = encode(std::array{std::to_underlying(wire_kind::memory_v2),
                                       local->memory->base,
                                       std::uint64_t{region.length()},
                                       region.remote_address(),
                                       region.key(),
                                       std::to_underlying(local->memory->identity.space),
                                       local->memory->identity.device});
            return NIXL_SUCCESS;
        });
    }

    auto getConnInfo(std::string& output) const -> nixl_status_t override {
        return protect([&] {
            output = encode(std::array{std::to_underlying(wire_kind::connection_v1),
                                       fabric_->protocol,
                                       std::uint64_t{fabric_->address.to_uint32()},
                                       std::uint64_t{fabric_->address.port()}});
            return NIXL_SUCCESS;
        });
    }

    auto loadRemoteConnInfo(const std::string& agent, const std::string& bytes)
        -> nixl_status_t override {
        const auto data = decode<4>(bytes);
        if (!data || (*data)[0] != std::to_underlying(wire_kind::connection_v1) ||
            (*data)[1] != fabric_->protocol || !std::in_range<std::uint32_t>((*data)[2]) ||
            !std::in_range<std::uint16_t>((*data)[3]) || (*data)[3] == 0)
            return NIXL_ERR_MISMATCH;
        return protect([&] {
            std::lock_guard lock(mutex_);
            const loom::ipv4_address address{static_cast<std::uint32_t>((*data)[2]),
                                             static_cast<std::uint16_t>((*data)[3])};
            auto target = std::make_shared<peer>();
            target->fabric = fabric_;
            target->handle = checked(fabric_->av.insert(loom::address{address}));
            if (auto old = peers_.find(agent); old != peers_.end())
                old->second->connected = false;
            peers_.insert_or_assign(agent, std::move(target));
            return NIXL_SUCCESS;
        });
    }

    auto connect(const std::string& agent) -> nixl_status_t override {
        std::lock_guard lock(mutex_);
        return peers_.contains(agent) ? NIXL_SUCCESS : NIXL_ERR_NOT_FOUND;
    }

    auto genNotif(const std::string& agent, const std::string& message) const
        -> nixl_status_t override {
        return protect([&] {
            std::lock_guard lock(mutex_);
            const auto destination = peers_.find(agent);
            if (destination == peers_.end())
                return NIXL_ERR_NOT_FOUND;
            if (queue_failed_ || notification_failed_)
                return NIXL_ERR_BACKEND;
            if (outgoing_.size() >= request_limit)
                return NIXL_ERR_NOT_ALLOWED;
            outgoing_.push_back(make_notification(destination->second, message));
            changed_.notify_all();
            return NIXL_SUCCESS;
        });
    }

    auto getNotifs(notif_list_t& output) -> nixl_status_t override {
        std::lock_guard lock(mutex_);
        output.swap(notifications_);
        changed_.notify_all();
        return queue_failed_ || notification_failed_ ? NIXL_ERR_BACKEND : NIXL_SUCCESS;
    }
    auto disconnect(const std::string& agent) -> nixl_status_t override {
        std::lock_guard lock(mutex_);
        if (auto found = peers_.find(agent); found != peers_.end()) {
            found->second->connected = false;
            peers_.erase(found);
        }
        return NIXL_SUCCESS;
    }

    auto loadRemoteMD(const nixlBlobDesc& memory,
                      const nixl_mem_t& type,
                      const std::string& agent,
                      nixlBackendMD*& output) -> nixl_status_t override {
        output = nullptr;
        if (type != DRAM_SEG)
            return NIXL_ERR_NOT_SUPPORTED;
        const auto data = decode<7>(memory.metaInfo);
        if (!data || (*data)[0] != std::to_underlying(wire_kind::memory_v2) ||
            (*data)[1] != memory.addr || (*data)[2] != memory.len || memory.len == 0 ||
            memory.len > std::numeric_limits<std::uint64_t>::max() - (*data)[3] ||
            (*data)[5] != std::to_underlying(memory_space::host) || (*data)[6] != memory.devId)
            return NIXL_ERR_MISMATCH;
        return protect([&] {
            std::lock_guard lock(mutex_);
            const auto target = peers_.find(agent);
            if (target == peers_.end())
                return NIXL_ERR_NOT_FOUND;
            output = new remote_metadata(target->second,
                                         memory.addr,
                                         loom::remote_memory{(*data)[3], (*data)[4], memory.len},
                                         memory_identity{memory_space::host, memory.devId});
            return NIXL_SUCCESS;
        });
    }

    auto prepXfer(const nixl_xfer_op_t& operation,
                  const nixl_meta_dlist_t& local,
                  const nixl_meta_dlist_t& remote,
                  const std::string& agent,
                  nixlBackendReqH*& output,
                  const nixl_opt_b_args_t*) const -> nixl_status_t override {
        output = nullptr;
        if (local.getType() != DRAM_SEG || remote.getType() != DRAM_SEG)
            return NIXL_ERR_NOT_SUPPORTED;
        const auto count = std::ranges::size(local);
        if ((operation != NIXL_READ && operation != NIXL_WRITE) || count == 0 ||
            count != std::ranges::size(remote) || count > segment_limit)
            return NIXL_ERR_INVALID_PARAM;
        return protect([&] {
            std::lock_guard lock(mutex_);
            if (requests_.size() >= request_limit || queue_failed_)
                return NIXL_ERR_NOT_ALLOWED;
            const auto destination = peers_.find(agent);
            if (destination == peers_.end())
                return NIXL_ERR_NOT_FOUND;
            auto work = std::make_unique<request>(operation);
            work->segments.reserve(count);
            for (std::size_t i = 0; i < count; ++i) {
                auto* source = dynamic_cast<local_metadata*>(local[i].metadataP);
                auto* target = dynamic_cast<remote_metadata*>(remote[i].metadataP);
                const auto size = operation == NIXL_WRITE ? local[i].len : remote[i].len;
                if (!source || !target || !size || size > fabric_->max_transfer ||
                    size > local[i].len || size > remote[i].len ||
                    source->memory->fabric != fabric_ ||
                    target->destination != destination->second ||
                    source->memory->identity !=
                        memory_identity{memory_space::host, local[i].devId} ||
                    target->identity != memory_identity{memory_space::host, remote[i].devId} ||
                    !covers(source->memory->base,
                            source->memory->region.length(),
                            local[i].addr,
                            size) ||
                    !covers(target->base, target->region.length, remote[i].addr, size))
                    return NIXL_ERR_INVALID_PARAM;
                auto part = std::make_unique<segment>();
                part->owner = work.get();
                part->memory = source->memory;
                part->destination = target->destination;
                part->buffer = {reinterpret_cast<std::byte*>(local[i].addr), size};
                part->remote = target->region.subregion(remote[i].addr - target->base, size);
                work->segments.push_back(std::move(part));
            }
            auto* handle = work.get();
            requests_.emplace(handle, std::move(work));
            output = handle;
            return NIXL_SUCCESS;
        });
    }

    auto postXfer(const nixl_xfer_op_t& operation,
                  const nixl_meta_dlist_t&,
                  const nixl_meta_dlist_t&,
                  const std::string&,
                  nixlBackendReqH*& handle,
                  const nixl_opt_b_args_t* options) const -> nixl_status_t override try {
        std::lock_guard lock(mutex_);
        const auto found = requests_.find(handle);
        if (found == requests_.end() || found->second->operation != operation)
            return NIXL_ERR_INVALID_PARAM;
        auto& work = *found->second;
        if (work.phase == request_phase::active && work.status() == NIXL_IN_PROG)
            return NIXL_ERR_REPOST_ACTIVE;
        if (queue_failed_)
            return NIXL_ERR_BACKEND;
        for (auto& part : work.segments)
            if (!part->destination->connected)
                return NIXL_ERR_REMOTE_DISCONNECT;
        work.message =
            options && options->hasNotif
                ? make_notification(work.segments.front()->destination, options->notifMsg)
                : nullptr;
        work.next = 0;
        work.pending = 0;
        work.error = NIXL_SUCCESS;
        work.phase = request_phase::active;
        changed_.notify_all();
        return NIXL_IN_PROG;
    } catch (const std::invalid_argument&) {
        return NIXL_ERR_INVALID_PARAM;
    } catch (const std::exception&) {
        return NIXL_ERR_BACKEND;
    }

    auto checkXfer(nixlBackendReqH* handle) const -> nixl_status_t override {
        std::lock_guard lock(mutex_);
        const auto found = requests_.find(handle);
        return found == requests_.end() ? NIXL_ERR_INVALID_PARAM : found->second->status();
    }

    auto releaseReqH(nixlBackendReqH* handle) const -> nixl_status_t override {
        std::unique_lock lock(mutex_);
        const auto found = requests_.find(handle);
        if (found == requests_.end())
            return NIXL_ERR_INVALID_PARAM;
        auto& work = *found->second;
        if (work.phase == request_phase::active) {
            work.error = NIXL_ERR_CANCELED;
            // Native RMA may not support cancellation. Stop queued segments;
            // wait for accepted DMA before allowing the caller to reuse buffers.
            changed_.wait(lock, [&work] { return !work.native_pending(); });
        }
        requests_.erase(handle);
        return NIXL_SUCCESS;
    }
};
}  // namespace

extern "C" NIXL_PLUGIN_EXPORT auto nixl_plugin_init() -> nixlBackendPlugin* {
    return nixlBackendPluginCreator<engine>::create(NIXL_PLUGIN_API_VERSION,
                                                    "LOOM",
                                                    "0.1.0",
                                                    {{"provider", "sockets"}, {"domain", "lo"}},
                                                    {DRAM_SEG});
}

extern "C" NIXL_PLUGIN_EXPORT void nixl_plugin_fini() {}
