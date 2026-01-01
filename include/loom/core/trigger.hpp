// SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only
#pragma once

#include <rdma/fi_trigger.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>

#include "loom/core/allocator.hpp"
#include "loom/core/atomic.hpp"
#include "loom/core/concepts/strong_types.hpp"
#include "loom/core/counter.hpp"
#include "loom/core/crtp/fabric_object_base.hpp"
#include "loom/core/domain.hpp"
#include "loom/core/endpoint.hpp"
#include "loom/core/memory.hpp"
#include "loom/core/messaging.hpp"
#include "loom/core/result.hpp"
#include "loom/core/rma.hpp"
#include "loom/core/types.hpp"

/**
 * @namespace loom::trigger
 * @brief Triggered/deferred operation support.
 */
namespace loom::trigger {

/**
 * @enum event_type
 * @brief Types of trigger events.
 */
enum class event_type : std::uint32_t {
    threshold = FI_TRIGGER_THRESHOLD,  ///< Counter threshold trigger
    xpu = FI_TRIGGER_XPU,              ///< XPU-based trigger
};

/**
 * @enum op_type
 * @brief Types of deferred operations.
 */
enum class op_type : std::uint32_t {
    recv = FI_OP_RECV,                      ///< Receive operation
    send = FI_OP_SEND,                      ///< Send operation
    tagged_recv = FI_OP_TRECV,              ///< Tagged receive
    tagged_send = FI_OP_TSEND,              ///< Tagged send
    read = FI_OP_READ,                      ///< RMA read
    write = FI_OP_WRITE,                    ///< RMA write
    atomic = FI_OP_ATOMIC,                  ///< Atomic operation
    fetch_atomic = FI_OP_FETCH_ATOMIC,      ///< Fetch-atomic operation
    compare_atomic = FI_OP_COMPARE_ATOMIC,  ///< Compare-atomic operation
    counter_set = FI_OP_CNTR_SET,           ///< Set counter value
    counter_add = FI_OP_CNTR_ADD,           ///< Add to counter
};

/**
 * @struct threshold_condition
 * @brief Condition for triggering deferred work based on counter threshold.
 */
struct threshold_condition {
    counter* cntr{nullptr};    ///< Counter to monitor
    std::size_t threshold{0};  ///< Threshold value to trigger on

    /**
     * @brief Converts to libfabric trigger threshold structure.
     * @return The fi_trigger_threshold structure.
     */
    [[nodiscard]] auto to_fi_threshold() const noexcept -> ::fi_trigger_threshold;
};

/**
 * @namespace loom::trigger::detail
 * @brief Internal implementation details for triggers.
 */
namespace detail {
/**
 * @struct deferred_work_impl
 * @brief Internal implementation for deferred_work.
 */
struct deferred_work_impl;
}  // namespace detail

/**
 * @class deferred_work
 * @brief Represents a deferred/triggered operation.
 *
 * Deferred work allows operations to be queued and executed when
 * a trigger condition (such as a counter threshold) is met.
 */
class deferred_work : public fabric_object_base<deferred_work> {
public:
    /// Implementation pointer type
    using impl_ptr = ::loom::detail::pmr_unique_ptr<detail::deferred_work_impl>;

    /**
     * @brief Default constructor.
     */
    deferred_work();

    /**
     * @brief Constructs from an implementation pointer.
     * @param impl The implementation pointer.
     */
    explicit deferred_work(impl_ptr impl) noexcept;

    /**
     * @brief Destructor.
     */
    ~deferred_work();

    /**
     * @brief Deleted copy constructor.
     */
    deferred_work(const deferred_work&) = delete;
    /**
     * @brief Deleted copy assignment operator.
     */
    auto operator=(const deferred_work&) -> deferred_work& = delete;
    /**
     * @brief Move constructor.
     */
    deferred_work(deferred_work&&) noexcept;
    /**
     * @brief Move assignment operator.
     */
    auto operator=(deferred_work&&) noexcept -> deferred_work&;

    [[nodiscard]] static auto create_send(domain& dom,
                                          endpoint& ep,
                                          const threshold_condition& trigger,
                                          counter* completion_cntr,
                                          const msg::send_message<void*>& msg,
                                          msg::send_flags flags = msg::send_flag::none,
                                          memory_resource* resource = nullptr)
        -> result<deferred_work>;

    [[nodiscard]] static auto create_recv(domain& dom,
                                          endpoint& ep,
                                          const threshold_condition& trigger,
                                          counter* completion_cntr,
                                          const msg::recv_message<void*>& msg,
                                          msg::recv_flags flags = msg::recv_flag::none,
                                          memory_resource* resource = nullptr)
        -> result<deferred_work>;

    [[nodiscard]] static auto create_tagged_send(domain& dom,
                                                 endpoint& ep,
                                                 const threshold_condition& trigger,
                                                 counter* completion_cntr,
                                                 const msg::tagged_send_message<void*>& msg,
                                                 msg::send_flags flags = msg::send_flag::none,
                                                 memory_resource* resource = nullptr)
        -> result<deferred_work>;

    [[nodiscard]] static auto create_tagged_recv(domain& dom,
                                                 endpoint& ep,
                                                 const threshold_condition& trigger,
                                                 counter* completion_cntr,
                                                 const msg::tagged_recv_message<void*>& msg,
                                                 msg::recv_flags flags = msg::recv_flag::none,
                                                 memory_resource* resource = nullptr)
        -> result<deferred_work>;

    [[nodiscard]] static auto create_read(domain& dom,
                                          endpoint& ep,
                                          const threshold_condition& trigger,
                                          counter* completion_cntr,
                                          std::span<const iovec> local_iov,
                                          std::span<const mr_descriptor> desc,
                                          std::span<const rma::rma_iov> remote_iov,
                                          fabric_addr dest_addr = fabric_addrs::unspecified,
                                          std::uint64_t flags = 0,
                                          memory_resource* resource = nullptr)
        -> result<deferred_work>;

    [[nodiscard]] static auto create_write(domain& dom,
                                           endpoint& ep,
                                           const threshold_condition& trigger,
                                           counter* completion_cntr,
                                           std::span<const iovec> local_iov,
                                           std::span<const mr_descriptor> desc,
                                           std::span<const rma::rma_iov> remote_iov,
                                           fabric_addr dest_addr = fabric_addrs::unspecified,
                                           std::uint64_t flags = 0,
                                           std::uint64_t data = 0,
                                           memory_resource* resource = nullptr)
        -> result<deferred_work>;

    [[nodiscard]] static auto create_counter_set(domain& dom,
                                                 const threshold_condition& trigger,
                                                 counter& target_cntr,
                                                 std::uint64_t value,
                                                 memory_resource* resource = nullptr)
        -> result<deferred_work>;

    [[nodiscard]] static auto create_counter_add(domain& dom,
                                                 const threshold_condition& trigger,
                                                 counter& target_cntr,
                                                 std::uint64_t value,
                                                 memory_resource* resource = nullptr)
        -> result<deferred_work>;

    auto cancel() -> void_result;

    [[nodiscard]] auto is_pending() const noexcept -> bool;

    [[nodiscard]] auto impl_internal_ptr() const noexcept -> void*;
    [[nodiscard]] auto impl_valid() const noexcept -> bool;

private:
    impl_ptr impl_;
};

/**
 * @brief Flushes all pending deferred work in the domain.
 * @param dom The domain to flush.
 * @return Success or error result.
 */
auto flush_work(domain& dom) -> void_result;

/**
 * @brief Flushes deferred work triggered by a specific counter.
 * @param dom The domain to flush.
 * @param triggering_cntr The counter whose triggered work to flush.
 * @return Success or error result.
 */
auto flush_work(domain& dom, counter& triggering_cntr) -> void_result;

/**
 * @brief Posts a triggered send message operation.
 * @tparam Ctx The triggered context type.
 * @param ep The endpoint to send on.
 * @param m The send message descriptor.
 * @param trigger The threshold condition.
 * @param flags Optional send flags.
 * @return Success or error result.
 */
template <structured_triggered_context Ctx>
[[nodiscard]] auto sendmsg(endpoint& ep,
                           const msg::send_message<Ctx*>& m,
                           const threshold_condition& trigger,
                           msg::send_flags flags = msg::send_flag::none) -> result<void> {
    auto* fid_ep = static_cast<::fid_ep*>(ep.impl_internal_ptr());
    if (fid_ep == nullptr || m.context == nullptr || trigger.cntr == nullptr) {
        return make_error_result<void>(errc::invalid_argument);
    }

    m.context->set_threshold_trigger(static_cast<::fid_cntr*>(trigger.cntr->impl_internal_ptr()),
                                     trigger.threshold);

    std::array<void*, msg::max_iov_count> raw_desc{};

    ::fi_msg fi_m{};
    fi_m.msg_iov = m.iov.data();
    fi_m.desc = msg::detail::descriptors_to_raw(m.desc, raw_desc);
    fi_m.iov_count = m.iov.size();
    fi_m.addr = m.dest_addr.get();
    fi_m.context = m.context->raw();
    fi_m.data = m.data;

    auto ret = ::fi_sendmsg(fid_ep, &fi_m, msg::detail::translate_send_flags(flags) | FI_TRIGGER);
    if (ret != 0) {
        return make_error_result_from_fi_errno<void>(static_cast<int>(-ret));
    }

    return make_success();
}

/**
 * @brief Posts a triggered receive message operation.
 * @tparam Ctx The triggered context type.
 * @param ep The endpoint to receive on.
 * @param m The receive message descriptor.
 * @param trigger The threshold condition.
 * @param flags Optional receive flags.
 * @return Success or error result.
 */
template <structured_triggered_context Ctx>
[[nodiscard]] auto recvmsg(endpoint& ep,
                           const msg::recv_message<Ctx*>& m,
                           const threshold_condition& trigger,
                           msg::recv_flags flags = msg::recv_flag::none) -> result<void> {
    auto* fid_ep = static_cast<::fid_ep*>(ep.impl_internal_ptr());
    if (fid_ep == nullptr || m.context == nullptr || trigger.cntr == nullptr) {
        return make_error_result<void>(errc::invalid_argument);
    }

    m.context->set_threshold_trigger(static_cast<::fid_cntr*>(trigger.cntr->impl_internal_ptr()),
                                     trigger.threshold);

    std::array<void*, msg::max_iov_count> raw_desc{};

    ::fi_msg fi_m{};
    fi_m.msg_iov = m.iov.data();
    fi_m.desc = msg::detail::descriptors_to_raw(m.desc, raw_desc);
    fi_m.iov_count = m.iov.size();
    fi_m.addr = m.src_addr.get();
    fi_m.context = m.context->raw();
    fi_m.data = 0;

    auto ret = ::fi_recvmsg(fid_ep, &fi_m, msg::detail::translate_recv_flags(flags) | FI_TRIGGER);
    if (ret != 0) {
        return make_error_result_from_fi_errno<void>(static_cast<int>(-ret));
    }

    return make_success();
}

/**
 * @brief Posts a triggered tagged send message operation.
 * @tparam Ctx The triggered context type.
 * @param ep The endpoint to send on.
 * @param m The tagged send message descriptor.
 * @param trigger The threshold condition.
 * @param flags Optional send flags.
 * @return Success or error result.
 */
template <structured_triggered_context Ctx>
[[nodiscard]] auto tagged_sendmsg(endpoint& ep,
                                  const msg::tagged_send_message<Ctx*>& m,
                                  const threshold_condition& trigger,
                                  msg::send_flags flags = msg::send_flag::none) -> result<void> {
    auto* fid_ep = static_cast<::fid_ep*>(ep.impl_internal_ptr());
    if (fid_ep == nullptr || m.context == nullptr || trigger.cntr == nullptr) {
        return make_error_result<void>(errc::invalid_argument);
    }

    m.context->set_threshold_trigger(static_cast<::fid_cntr*>(trigger.cntr->impl_internal_ptr()),
                                     trigger.threshold);

    std::array<void*, msg::max_iov_count> raw_desc{};

    ::fi_msg_tagged fi_m{};
    fi_m.msg_iov = m.iov.data();
    fi_m.desc = msg::detail::descriptors_to_raw(m.desc, raw_desc);
    fi_m.iov_count = m.iov.size();
    fi_m.addr = m.dest_addr.get();
    fi_m.tag = m.tag;
    fi_m.ignore = 0;
    fi_m.context = m.context->raw();
    fi_m.data = m.data;

    auto ret = ::fi_tsendmsg(fid_ep, &fi_m, msg::detail::translate_send_flags(flags) | FI_TRIGGER);
    if (ret != 0) {
        return make_error_result_from_fi_errno<void>(static_cast<int>(-ret));
    }

    return make_success();
}

/**
 * @brief Posts a triggered tagged receive message operation.
 * @tparam Ctx The triggered context type.
 * @param ep The endpoint to receive on.
 * @param m The tagged receive message descriptor.
 * @param trigger The threshold condition.
 * @param flags Optional receive flags.
 * @return Success or error result.
 */
template <structured_triggered_context Ctx>
[[nodiscard]] auto tagged_recvmsg(endpoint& ep,
                                  const msg::tagged_recv_message<Ctx*>& m,
                                  const threshold_condition& trigger,
                                  msg::recv_flags flags = msg::recv_flag::none) -> result<void> {
    auto* fid_ep = static_cast<::fid_ep*>(ep.impl_internal_ptr());
    if (fid_ep == nullptr || m.context == nullptr || trigger.cntr == nullptr) {
        return make_error_result<void>(errc::invalid_argument);
    }

    m.context->set_threshold_trigger(static_cast<::fid_cntr*>(trigger.cntr->impl_internal_ptr()),
                                     trigger.threshold);

    std::array<void*, msg::max_iov_count> raw_desc{};

    ::fi_msg_tagged fi_m{};
    fi_m.msg_iov = m.iov.data();
    fi_m.desc = msg::detail::descriptors_to_raw(m.desc, raw_desc);
    fi_m.iov_count = m.iov.size();
    fi_m.addr = m.src_addr.get();
    fi_m.tag = m.tag;
    fi_m.ignore = m.ignore;
    fi_m.context = m.context->raw();
    fi_m.data = 0;

    auto ret = ::fi_trecvmsg(fid_ep, &fi_m, msg::detail::translate_recv_flags(flags) | FI_TRIGGER);
    if (ret != 0) {
        return make_error_result_from_fi_errno<void>(static_cast<int>(-ret));
    }

    return make_success();
}

/**
 * @brief Posts a triggered RMA read operation.
 * @tparam Ctx The triggered context type.
 * @param ep The endpoint to use.
 * @param local_buffer Local buffer to read into.
 * @param local_mr Memory region for the local buffer.
 * @param remote Remote memory location.
 * @param context The triggered context.
 * @param trigger The threshold condition.
 * @return Success or error result.
 */
template <structured_triggered_context Ctx>
[[nodiscard]] auto read(endpoint& ep,
                        std::span<std::byte> local_buffer,
                        memory_region& local_mr,
                        remote_memory remote,
                        Ctx* context,
                        const threshold_condition& trigger) -> result<void> {
    auto* fid_ep = static_cast<::fid_ep*>(ep.internal_ptr());
    if (fid_ep == nullptr || context == nullptr || trigger.cntr == nullptr) {
        return make_error_result<void>(errc::invalid_argument);
    }

    context->set_threshold_trigger(static_cast<::fid_cntr*>(trigger.cntr->impl_internal_ptr()),
                                   trigger.threshold);

    ::iovec iov{};
    iov.iov_base = local_buffer.data();
    iov.iov_len = local_buffer.size();

    auto* desc = local_mr.descriptor().raw();

    ::fi_rma_iov rma_iov{};
    rma_iov.addr = remote.addr;
    rma_iov.len = local_buffer.size();
    rma_iov.key = remote.key;

    ::fi_msg_rma fi_m{};
    fi_m.msg_iov = &iov;
    fi_m.desc = &desc;
    fi_m.iov_count = 1;
    fi_m.addr = FI_ADDR_UNSPEC;
    fi_m.rma_iov = &rma_iov;
    fi_m.rma_iov_count = 1;
    fi_m.context = context->raw();
    fi_m.data = 0;

    auto ret = ::fi_readmsg(fid_ep, &fi_m, FI_TRIGGER);
    if (ret != 0) {
        return make_error_result_from_fi_errno<void>(static_cast<int>(-ret));
    }

    return make_success();
}

/**
 * @brief Posts a triggered RMA write operation.
 * @tparam Ctx The triggered context type.
 * @param ep The endpoint to use.
 * @param local_buffer Local buffer to write from.
 * @param local_mr Memory region for the local buffer.
 * @param remote Remote memory location.
 * @param context The triggered context.
 * @param trigger The threshold condition.
 * @return Success or error result.
 */
template <structured_triggered_context Ctx>
[[nodiscard]] auto write(endpoint& ep,
                         std::span<const std::byte> local_buffer,
                         memory_region& local_mr,
                         remote_memory remote,
                         Ctx* context,
                         const threshold_condition& trigger) -> result<void> {
    auto* fid_ep = static_cast<::fid_ep*>(ep.internal_ptr());
    if (fid_ep == nullptr || context == nullptr || trigger.cntr == nullptr) {
        return make_error_result<void>(errc::invalid_argument);
    }

    context->set_threshold_trigger(static_cast<::fid_cntr*>(trigger.cntr->impl_internal_ptr()),
                                   trigger.threshold);

    ::iovec iov{};
    iov.iov_base = const_cast<std::byte*>(local_buffer.data());
    iov.iov_len = local_buffer.size();

    auto* desc = local_mr.descriptor().raw();

    ::fi_rma_iov rma_iov{};
    rma_iov.addr = remote.addr;
    rma_iov.len = local_buffer.size();
    rma_iov.key = remote.key;

    ::fi_msg_rma fi_m{};
    fi_m.msg_iov = &iov;
    fi_m.desc = &desc;
    fi_m.iov_count = 1;
    fi_m.addr = FI_ADDR_UNSPEC;
    fi_m.rma_iov = &rma_iov;
    fi_m.rma_iov_count = 1;
    fi_m.context = context->raw();
    fi_m.data = 0;

    auto ret = ::fi_writemsg(fid_ep, &fi_m, FI_TRIGGER);
    if (ret != 0) {
        return make_error_result_from_fi_errno<void>(static_cast<int>(-ret));
    }

    return make_success();
}

/**
 * @brief Checks if an endpoint supports triggered operations.
 * @param ep The endpoint to query.
 * @return True if triggered operations are supported.
 */
[[nodiscard]] auto supports_trigger(const endpoint& ep) -> bool;

}  // namespace loom::trigger
