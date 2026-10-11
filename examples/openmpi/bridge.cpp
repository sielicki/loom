// SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only
#include "bridge.h"

#include <loom/async/completion_queue.hpp>
#include <loom/core/address_vector.hpp>
#include <loom/core/domain.hpp>
#include <loom/core/endpoint.hpp>
#include <loom/core/fabric.hpp>
#include <loom/core/messaging.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <memory>
#include <span>
#include <utility>

struct ompi_mtl_ofi_loom {
    loom::fabric_info info;
    loom::fabric fabric;
    loom::domain domain;
    loom::completion_queue cq;
    loom::address_vector av;
    loom::endpoint ep;
};

namespace {
std::atomic<int> active_transports{};

template <typename Function>
auto boundary(Function&& function) noexcept -> int {
    try {
        return std::forward<Function>(function)();
    } catch (const std::bad_alloc&) {
        return -FI_ENOMEM;
    } catch (...) {
        return -FI_EOTHER;
    }
}

auto status(const loom::result<void>& result) noexcept -> int {
    return result ? 0 : result.error().value();
}

auto bytes(const void* data, std::size_t size) noexcept -> std::span<const std::byte> {
    return {static_cast<const std::byte*>(data), size};
}

auto bytes(void* data, std::size_t size) noexcept -> std::span<std::byte> {
    return {static_cast<std::byte*>(data), size};
}

// The MTL packs datatypes before submission, including matched receives.
// Its only message-form sends are zero-byte synchronous acknowledgements.
auto valid_message(const fi_msg_tagged& message) noexcept -> bool {
    return message.iov_count <= 1 && (message.iov_count == 0 || message.msg_iov) &&
           (!message.desc || message.iov_count == 0 || !message.desc[0]);
}
}  // namespace

extern "C" int ompi_mtl_ofi_loom_open(const fi_info* native,
                                      fi_av_type av_type,
                                      std::size_t peers,
                                      std::size_t cq_size,
                                      ompi_mtl_ofi_loom** output,
                                      ompi_mtl_ofi_loom_handles* handles) {
    return boundary([&] {
        if (!native || !output || *output || !handles || !native->ep_attr || !native->domain_attr ||
            !native->tx_attr || !native->rx_attr || cq_size == 0) {
            return -FI_EINVAL;
        }
        constexpr auto required = FI_MSG | FI_TAGGED | FI_DIRECTED_RECV;
        constexpr auto supported_modes = FI_CONTEXT | FI_CONTEXT2 | FI_RX_CQ_DATA;
        // This consumer only registers device buffers. Reject host registration
        // requirements until its host MR policy is connected to Loom as well.
        if (native->ep_attr->type != FI_EP_RDM || (native->caps & required) != required ||
            ((native->mode | native->tx_attr->mode | native->rx_attr->mode) & ~supported_modes) !=
                0 ||
            native->domain_attr->mr_mode == FI_MR_BASIC ||
            (native->domain_attr->mr_mode & (FI_MR_LOCAL | FI_MR_ENDPOINT | FI_MR_RAW)) != 0 ||
            (native->addr_format != FI_SOCKADDR_IN && native->addr_format != FI_SOCKADDR_IN6 &&
             native->addr_format != FI_SOCKADDR && native->addr_format != FI_SOCKADDR_IP) ||
            (av_type != FI_AV_MAP && av_type != FI_AV_TABLE)) {
            return -FI_ENOSYS;
        }
        auto graph = std::make_unique<ompi_mtl_ofi_loom>();
        auto info = loom::fabric_info::copy_native(*native);
        if (!info) {
            return info.error().value();
        }
        graph->info = std::move(*info);
        auto fabric = loom::fabric::create(graph->info);
        if (!fabric) {
            return fabric.error().value();
        }
        graph->fabric = std::move(*fabric);
        auto domain = loom::domain::create(graph->fabric, graph->info);
        if (!domain) {
            return domain.error().value();
        }
        graph->domain = std::move(*domain);
        auto cq =
            loom::completion_queue::create(graph->domain, {.size = loom::queue_size{cq_size}});
        if (!cq) {
            return cq.error().value();
        }
        graph->cq = std::move(*cq);
        auto av = loom::address_vector::create(
            graph->domain,
            {.type = av_type == FI_AV_TABLE ? loom::av_type::table : loom::av_type::map,
             .count = peers,
             .addr_format = graph->info.get_domain_attr()->addr_format});
        if (!av) {
            return av.error().value();
        }
        graph->av = std::move(*av);
        auto ep = loom::endpoint::create(graph->domain, graph->info);
        if (!ep) {
            return ep.error().value();
        }
        graph->ep = std::move(*ep);
        if (auto result = graph->ep.bind_av(graph->av); !result) {
            return result.error().value();
        }
        if (auto result = graph->ep.bind_cq(graph->cq,
                                            loom::cq_bind::transmit | loom::cq_bind::recv |
                                                loom::cq_bind::selective_completion);
            !result) {
            return result.error().value();
        }
        if (auto result = graph->ep.enable(); !result) {
            return result.error().value();
        }
        *handles = {.fabric = static_cast<fid_fabric*>(graph->fabric.internal_ptr()),
                    .domain = static_cast<fid_domain*>(graph->domain.internal_ptr()),
                    .av = static_cast<fid_av*>(graph->av.internal_ptr()),
                    .cq = static_cast<fid_cq*>(graph->cq.internal_ptr()),
                    .ep = static_cast<fid_ep*>(graph->ep.internal_ptr())};
        *output = graph.release();
        active_transports.fetch_add(1, std::memory_order_relaxed);
        return 0;
    });
}

extern "C" int ompi_mtl_ofi_loom_close(ompi_mtl_ofi_loom** transport) {
    return boundary([&] {
        if (!transport) {
            return -FI_EINVAL;
        }
        if (*transport) {
            if (auto result = (*transport)->ep.close(); !result) {
                return result.error().value();
            }
            delete std::exchange(*transport, nullptr);
            active_transports.fetch_sub(1, std::memory_order_relaxed);
        }
        return 0;
    });
}

extern "C" int ompi_mtl_ofi_loom_active() {
    return active_transports.load(std::memory_order_relaxed);
}

extern "C" int ompi_mtl_ofi_loom_insert(ompi_mtl_ofi_loom* transport,
                                        const void* data,
                                        std::size_t size,
                                        fi_addr_t* output) {
    return boundary([&] {
        const auto address =
            loom::address_from_raw(data, size, transport->info.get_domain_attr()->addr_format);
        if (!output || std::holds_alternative<loom::unspecified_address>(address)) {
            return -FI_EINVAL;
        }
        auto result = transport->av.insert(address);
        if (!result) {
            return result.error().value();
        }
        *output = result->value;
        return 1;
    });
}

extern "C" int ompi_mtl_ofi_loom_remove(ompi_mtl_ofi_loom* transport, fi_addr_t address) {
    return boundary([&] { return status(transport->av.remove(loom::av_handle{address})); });
}

extern "C" int ompi_mtl_ofi_loom_progress(ompi_mtl_ofi_loom* transport,
                                          std::size_t limit,
                                          ompi_mtl_ofi_loom_completion callback) {
    return boundary([&] {
        std::array<loom::completion_event, 100> events{};
        const auto count =
            transport->cq.poll_batch(std::span{events}.first(std::min(limit, events.size())));
        for (const auto& event : std::span{events}.first(count)) {
            if (event.has_error()) {
                fi_cq_err_entry error{};
                error.op_context = event.context.get();
                error.flags = event.flags;
                error.len = event.len;
                error.buf = event.buffer;
                error.data = event.data;
                error.tag = event.tag;
                error.err = -event.error.value();
                error.prov_errno = event.error_info.prov_errno;
                error.err_data = const_cast<std::byte*>(event.error_info.err_data.get());
                error.err_data_size = event.error_info.err_data_size;
                error.src_addr = event.source.get();
                callback(nullptr, &error);
            } else {
                fi_cq_tagged_entry completion{.op_context = event.context.get(),
                                              .flags = event.flags,
                                              .len = event.len,
                                              .buf = event.buffer,
                                              .data = event.data,
                                              .tag = event.tag};
                callback(&completion, nullptr);
            }
        }
        return static_cast<int>(count);
    });
}

extern "C" int ompi_mtl_ofi_loom_send(ompi_mtl_ofi_loom* t,
                                      const void* data,
                                      size_t size,
                                      void* desc,
                                      fi_addr_t peer,
                                      void* context) {
    return boundary([&] {
        return status(loom::send(
            t->ep, bytes(data, size), loom::fabric_addr{peer}, loom::mr_descriptor{desc}, context));
    });
}
extern "C" int ompi_mtl_ofi_loom_recv(
    ompi_mtl_ofi_loom* t, void* data, size_t size, void* desc, fi_addr_t peer, void* context) {
    return boundary([&] {
        return status(loom::recv(
            t->ep, bytes(data, size), loom::fabric_addr{peer}, loom::mr_descriptor{desc}, context));
    });
}
extern "C" int ompi_mtl_ofi_loom_senddata(ompi_mtl_ofi_loom* t,
                                          const void* data,
                                          size_t size,
                                          void* desc,
                                          uint64_t immediate,
                                          fi_addr_t peer,
                                          void* context) {
    return boundary([&] {
        return status(loom::send_data(t->ep,
                                      bytes(data, size),
                                      loom::fabric_addr{peer},
                                      immediate,
                                      loom::mr_descriptor{desc},
                                      context));
    });
}
extern "C" int
ompi_mtl_ofi_loom_inject(ompi_mtl_ofi_loom* t, const void* data, size_t size, fi_addr_t peer) {
    return boundary(
        [&] { return status(t->ep.inject(bytes(data, size), loom::fabric_addr{peer})); });
}
extern "C" int ompi_mtl_ofi_loom_injectdata(
    ompi_mtl_ofi_loom* t, const void* data, size_t size, uint64_t immediate, fi_addr_t peer) {
    return boundary([&] {
        return status(t->ep.inject_data(bytes(data, size), immediate, loom::fabric_addr{peer}));
    });
}
extern "C" int ompi_mtl_ofi_loom_tsend(ompi_mtl_ofi_loom* t,
                                       const void* data,
                                       size_t size,
                                       void* desc,
                                       fi_addr_t peer,
                                       uint64_t tag,
                                       void* context) {
    return boundary([&] {
        return status(loom::tagged_send(t->ep,
                                        bytes(data, size),
                                        loom::fabric_addr{peer},
                                        tag,
                                        loom::mr_descriptor{desc},
                                        context));
    });
}
extern "C" int ompi_mtl_ofi_loom_trecv(ompi_mtl_ofi_loom* t,
                                       void* data,
                                       size_t size,
                                       void* desc,
                                       fi_addr_t peer,
                                       uint64_t tag,
                                       uint64_t ignore,
                                       void* context) {
    return boundary([&] {
        return status(loom::tagged_recv(t->ep,
                                        bytes(data, size),
                                        loom::fabric_addr{peer},
                                        tag,
                                        ignore,
                                        loom::mr_descriptor{desc},
                                        context));
    });
}
extern "C" int ompi_mtl_ofi_loom_tsenddata(ompi_mtl_ofi_loom* t,
                                           const void* data,
                                           size_t size,
                                           void* desc,
                                           uint64_t immediate,
                                           fi_addr_t peer,
                                           uint64_t tag,
                                           void* context) {
    return boundary([&] {
        return status(loom::tagged_send_data(t->ep,
                                             bytes(data, size),
                                             loom::fabric_addr{peer},
                                             tag,
                                             immediate,
                                             loom::mr_descriptor{desc},
                                             context));
    });
}
extern "C" int ompi_mtl_ofi_loom_tinject(
    ompi_mtl_ofi_loom* t, const void* data, size_t size, fi_addr_t peer, uint64_t tag) {
    return boundary([&] {
        return status(t->ep.tagged_inject(bytes(data, size), tag, loom::fabric_addr{peer}));
    });
}
extern "C" int ompi_mtl_ofi_loom_tinjectdata(ompi_mtl_ofi_loom* t,
                                             const void* data,
                                             size_t size,
                                             uint64_t immediate,
                                             fi_addr_t peer,
                                             uint64_t tag) {
    return boundary([&] {
        return status(
            t->ep.tagged_inject_data(bytes(data, size), tag, immediate, loom::fabric_addr{peer}));
    });
}

extern "C" int
ompi_mtl_ofi_loom_tsendmsg(ompi_mtl_ofi_loom* t, const fi_msg_tagged* message, uint64_t flags) {
    return boundary([&] {
        if (!message || !valid_message(*message) || flags != 0) {
            return -FI_EINVAL;
        }
        return status(loom::msg::tagged_sendmsg(
            t->ep,
            loom::msg::tagged_send_message<>{.iov = {message->msg_iov, message->iov_count},
                                             .dest_addr = loom::fabric_addr{message->addr},
                                             .tag = message->tag,
                                             .context = message->context,
                                             .data = message->data}));
    });
}

extern "C" int
ompi_mtl_ofi_loom_trecvmsg(ompi_mtl_ofi_loom* t, const fi_msg_tagged* message, uint64_t flags) {
    return boundary([&] {
        if (!message || !valid_message(*message) ||
            (flags & ~(FI_COMPLETION | FI_PEEK | FI_CLAIM | FI_DISCARD)) != 0) {
            return -FI_EINVAL;
        }
        auto translated = loom::msg::recv_flag::none;
        if (flags & FI_COMPLETION) {
            translated |= loom::msg::recv_flag::completion;
        }
        if (flags & FI_PEEK) {
            translated |= loom::msg::recv_flag::peek;
        }
        if (flags & FI_CLAIM) {
            translated |= loom::msg::recv_flag::claim;
        }
        if (flags & FI_DISCARD) {
            translated |= loom::msg::recv_flag::discard;
        }
        return status(loom::msg::tagged_recvmsg(
            t->ep,
            loom::msg::tagged_recv_message<>{.iov = {message->msg_iov, message->iov_count},
                                             .src_addr = loom::fabric_addr{message->addr},
                                             .tag = message->tag,
                                             .ignore = message->ignore,
                                             .context = message->context},
            translated));
    });
}

extern "C" int ompi_mtl_ofi_loom_cancel(ompi_mtl_ofi_loom* t, void* context) {
    return boundary([&] { return status(t->ep.cancel(loom::context_ptr<void>{context})); });
}
