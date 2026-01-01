// SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only
#include "loom/core/trigger.hpp"

#include <rdma/fabric.h>
#include <rdma/fi_domain.h>
#include <rdma/fi_endpoint.h>
#include <rdma/fi_errno.h>
#include <rdma/fi_rma.h>
#include <rdma/fi_tagged.h>
#include <rdma/fi_trigger.h>

#include <array>
#include <cstring>
#include <memory>
#include <vector>

#include "loom/core/allocator.hpp"
#include "loom/core/error.hpp"

namespace loom::trigger {

auto threshold_condition::to_fi_threshold() const noexcept -> ::fi_trigger_threshold {
    ::fi_trigger_threshold result{};
    result.cntr = cntr ? static_cast<::fid_cntr*>(cntr->impl_internal_ptr()) : nullptr;
    result.threshold = threshold;
    return result;
}

namespace detail {

/// @brief Internal implementation of deferred work for triggered operations.
struct deferred_work_impl {
    ::fid_domain* domain{nullptr};  ///< Domain for queuing deferred work.
    ::fi_deferred_work work{};      ///< Libfabric deferred work descriptor.

    ::fi_op_msg op_msg{};        ///< Message operation parameters.
    ::fi_op_tagged op_tagged{};  ///< Tagged message operation parameters.
    ::fi_op_rma op_rma{};        ///< RMA operation parameters.
    ::fi_op_cntr op_cntr{};      ///< Counter operation parameters.

    std::vector<::iovec> iov_storage;           ///< Storage for I/O vectors.
    std::vector<void*> desc_storage;            ///< Storage for memory descriptors.
    std::vector<::fi_rma_iov> rma_iov_storage;  ///< Storage for RMA I/O vectors.

    bool queued{false};  ///< Whether the work has been queued.

    /// @brief Default constructor.
    deferred_work_impl() = default;

    /// @brief Constructs deferred_work_impl with a domain.
    /// @param dom The libfabric domain pointer.
    explicit deferred_work_impl(::fid_domain* dom) : domain(dom) {}

    /// @brief Destructor that cancels queued work if necessary.
    ~deferred_work_impl() {
        if (queued && domain != nullptr) {
            ::fi_control(&domain->fid, FI_CANCEL_WORK, &work);
        }
    }

    deferred_work_impl(const deferred_work_impl&) = delete;
    auto operator=(const deferred_work_impl&) -> deferred_work_impl& = delete;
    deferred_work_impl(deferred_work_impl&&) = delete;
    auto operator=(deferred_work_impl&&) -> deferred_work_impl& = delete;
};

}  // namespace detail

deferred_work::deferred_work() = default;

deferred_work::deferred_work(impl_ptr impl) noexcept : impl_(std::move(impl)) {}

deferred_work::~deferred_work() = default;

deferred_work::deferred_work(deferred_work&&) noexcept = default;

auto deferred_work::operator=(deferred_work&&) noexcept -> deferred_work& = default;

auto deferred_work::impl_valid() const noexcept -> bool {
    return impl_ && impl_->domain != nullptr;
}

auto deferred_work::impl_internal_ptr() const noexcept -> void* {
    return impl_ ? &impl_->work : nullptr;
}

auto deferred_work::is_pending() const noexcept -> bool {
    return impl_ && impl_->queued;
}

auto deferred_work::cancel() -> void_result {
    if (!impl_ || !impl_->domain || !impl_->queued) {
        return make_error_result<void>(errc::invalid_argument);
    }

    int ret = ::fi_control(&impl_->domain->fid, FI_CANCEL_WORK, &impl_->work);
    if (ret != 0) {
        return make_error_result_from_fi_errno<void>(-ret);
    }

    impl_->queued = false;
    return make_success();
}

auto deferred_work::create_send(domain& dom,
                                endpoint& ep,
                                const threshold_condition& trigger,
                                counter* completion_cntr,
                                const msg::send_message<void*>& msg,
                                msg::send_flags flags,
                                memory_resource* resource) -> result<deferred_work> {
    if (!dom || !ep) {
        return make_error_result<deferred_work>(errc::invalid_argument);
    }

    auto* domain_ptr = static_cast<::fid_domain*>(dom.internal_ptr());
    auto* ep_ptr = static_cast<::fid_ep*>(ep.impl_internal_ptr());

    auto* res = resource ? resource : std::pmr::get_default_resource();
    auto impl = loom::detail::make_pmr_unique<detail::deferred_work_impl>(res, domain_ptr);

    impl->iov_storage.assign(msg.iov.begin(), msg.iov.end());
    impl->desc_storage.reserve(msg.desc.size());
    for (const auto& d : msg.desc) {
        impl->desc_storage.push_back(d.raw());
    }

    impl->op_msg.ep = ep_ptr;
    impl->op_msg.msg.msg_iov = impl->iov_storage.data();
    impl->op_msg.msg.desc = impl->desc_storage.empty() ? nullptr : impl->desc_storage.data();
    impl->op_msg.msg.iov_count = impl->iov_storage.size();
    impl->op_msg.msg.addr = msg.dest_addr.get();
    impl->op_msg.msg.context = msg.context;
    impl->op_msg.msg.data = msg.data;
    impl->op_msg.flags = loom::msg::detail::translate_send_flags(flags);

    impl->work.threshold = trigger.threshold;
    impl->work.triggering_cntr =
        trigger.cntr ? static_cast<::fid_cntr*>(trigger.cntr->impl_internal_ptr()) : nullptr;
    impl->work.completion_cntr =
        completion_cntr ? static_cast<::fid_cntr*>(completion_cntr->impl_internal_ptr()) : nullptr;
    impl->work.op_type = FI_OP_SEND;
    impl->work.op.msg = &impl->op_msg;

    int ret = ::fi_control(&domain_ptr->fid, FI_QUEUE_WORK, &impl->work);
    if (ret != 0) {
        return make_error_result_from_fi_errno<deferred_work>(-ret);
    }

    impl->queued = true;
    return deferred_work{std::move(impl)};
}

auto deferred_work::create_recv(domain& dom,
                                endpoint& ep,
                                const threshold_condition& trigger,
                                counter* completion_cntr,
                                const msg::recv_message<void*>& msg,
                                msg::recv_flags flags,
                                memory_resource* resource) -> result<deferred_work> {
    if (!dom || !ep) {
        return make_error_result<deferred_work>(errc::invalid_argument);
    }

    auto* domain_ptr = static_cast<::fid_domain*>(dom.internal_ptr());
    auto* ep_ptr = static_cast<::fid_ep*>(ep.impl_internal_ptr());

    auto* res = resource ? resource : std::pmr::get_default_resource();
    auto impl = loom::detail::make_pmr_unique<detail::deferred_work_impl>(res, domain_ptr);

    impl->iov_storage.assign(msg.iov.begin(), msg.iov.end());
    impl->desc_storage.reserve(msg.desc.size());
    for (const auto& d : msg.desc) {
        impl->desc_storage.push_back(d.raw());
    }

    impl->op_msg.ep = ep_ptr;
    impl->op_msg.msg.msg_iov = impl->iov_storage.data();
    impl->op_msg.msg.desc = impl->desc_storage.empty() ? nullptr : impl->desc_storage.data();
    impl->op_msg.msg.iov_count = impl->iov_storage.size();
    impl->op_msg.msg.addr = msg.src_addr.get();
    impl->op_msg.msg.context = msg.context;
    impl->op_msg.msg.data = 0;
    impl->op_msg.flags = loom::msg::detail::translate_recv_flags(flags);

    impl->work.threshold = trigger.threshold;
    impl->work.triggering_cntr =
        trigger.cntr ? static_cast<::fid_cntr*>(trigger.cntr->impl_internal_ptr()) : nullptr;
    impl->work.completion_cntr =
        completion_cntr ? static_cast<::fid_cntr*>(completion_cntr->impl_internal_ptr()) : nullptr;
    impl->work.op_type = FI_OP_RECV;
    impl->work.op.msg = &impl->op_msg;

    int ret = ::fi_control(&domain_ptr->fid, FI_QUEUE_WORK, &impl->work);
    if (ret != 0) {
        return make_error_result_from_fi_errno<deferred_work>(-ret);
    }

    impl->queued = true;
    return deferred_work{std::move(impl)};
}

auto deferred_work::create_tagged_send(domain& dom,
                                       endpoint& ep,
                                       const threshold_condition& trigger,
                                       counter* completion_cntr,
                                       const msg::tagged_send_message<void*>& msg,
                                       msg::send_flags flags,
                                       memory_resource* resource) -> result<deferred_work> {
    if (!dom || !ep) {
        return make_error_result<deferred_work>(errc::invalid_argument);
    }

    auto* domain_ptr = static_cast<::fid_domain*>(dom.internal_ptr());
    auto* ep_ptr = static_cast<::fid_ep*>(ep.impl_internal_ptr());

    auto* res = resource ? resource : std::pmr::get_default_resource();
    auto impl = loom::detail::make_pmr_unique<detail::deferred_work_impl>(res, domain_ptr);

    impl->iov_storage.assign(msg.iov.begin(), msg.iov.end());
    impl->desc_storage.reserve(msg.desc.size());
    for (const auto& d : msg.desc) {
        impl->desc_storage.push_back(d.raw());
    }

    impl->op_tagged.ep = ep_ptr;
    impl->op_tagged.msg.msg_iov = impl->iov_storage.data();
    impl->op_tagged.msg.desc = impl->desc_storage.empty() ? nullptr : impl->desc_storage.data();
    impl->op_tagged.msg.iov_count = impl->iov_storage.size();
    impl->op_tagged.msg.addr = msg.dest_addr.get();
    impl->op_tagged.msg.tag = msg.tag;
    impl->op_tagged.msg.ignore = 0;
    impl->op_tagged.msg.context = msg.context;
    impl->op_tagged.msg.data = msg.data;
    impl->op_tagged.flags = loom::msg::detail::translate_send_flags(flags);

    impl->work.threshold = trigger.threshold;
    impl->work.triggering_cntr =
        trigger.cntr ? static_cast<::fid_cntr*>(trigger.cntr->impl_internal_ptr()) : nullptr;
    impl->work.completion_cntr =
        completion_cntr ? static_cast<::fid_cntr*>(completion_cntr->impl_internal_ptr()) : nullptr;
    impl->work.op_type = FI_OP_TSEND;
    impl->work.op.tagged = &impl->op_tagged;

    int ret = ::fi_control(&domain_ptr->fid, FI_QUEUE_WORK, &impl->work);
    if (ret != 0) {
        return make_error_result_from_fi_errno<deferred_work>(-ret);
    }

    impl->queued = true;
    return deferred_work{std::move(impl)};
}

auto deferred_work::create_tagged_recv(domain& dom,
                                       endpoint& ep,
                                       const threshold_condition& trigger,
                                       counter* completion_cntr,
                                       const msg::tagged_recv_message<void*>& msg,
                                       msg::recv_flags flags,
                                       memory_resource* resource) -> result<deferred_work> {
    if (!dom || !ep) {
        return make_error_result<deferred_work>(errc::invalid_argument);
    }

    auto* domain_ptr = static_cast<::fid_domain*>(dom.internal_ptr());
    auto* ep_ptr = static_cast<::fid_ep*>(ep.impl_internal_ptr());

    auto* res = resource ? resource : std::pmr::get_default_resource();
    auto impl = loom::detail::make_pmr_unique<detail::deferred_work_impl>(res, domain_ptr);

    impl->iov_storage.assign(msg.iov.begin(), msg.iov.end());
    impl->desc_storage.reserve(msg.desc.size());
    for (const auto& d : msg.desc) {
        impl->desc_storage.push_back(d.raw());
    }

    impl->op_tagged.ep = ep_ptr;
    impl->op_tagged.msg.msg_iov = impl->iov_storage.data();
    impl->op_tagged.msg.desc = impl->desc_storage.empty() ? nullptr : impl->desc_storage.data();
    impl->op_tagged.msg.iov_count = impl->iov_storage.size();
    impl->op_tagged.msg.addr = msg.src_addr.get();
    impl->op_tagged.msg.tag = msg.tag;
    impl->op_tagged.msg.ignore = msg.ignore;
    impl->op_tagged.msg.context = msg.context;
    impl->op_tagged.msg.data = 0;
    impl->op_tagged.flags = loom::msg::detail::translate_recv_flags(flags);

    impl->work.threshold = trigger.threshold;
    impl->work.triggering_cntr =
        trigger.cntr ? static_cast<::fid_cntr*>(trigger.cntr->impl_internal_ptr()) : nullptr;
    impl->work.completion_cntr =
        completion_cntr ? static_cast<::fid_cntr*>(completion_cntr->impl_internal_ptr()) : nullptr;
    impl->work.op_type = FI_OP_TRECV;
    impl->work.op.tagged = &impl->op_tagged;

    int ret = ::fi_control(&domain_ptr->fid, FI_QUEUE_WORK, &impl->work);
    if (ret != 0) {
        return make_error_result_from_fi_errno<deferred_work>(-ret);
    }

    impl->queued = true;
    return deferred_work{std::move(impl)};
}

auto deferred_work::create_read(domain& dom,
                                endpoint& ep,
                                const threshold_condition& trigger,
                                counter* completion_cntr,
                                std::span<const iovec> local_iov,
                                std::span<const mr_descriptor> desc,
                                std::span<const rma::rma_iov> remote_iov,
                                fabric_addr dest_addr,
                                std::uint64_t flags,
                                memory_resource* resource) -> result<deferred_work> {
    if (!dom || !ep) {
        return make_error_result<deferred_work>(errc::invalid_argument);
    }

    auto* domain_ptr = static_cast<::fid_domain*>(dom.internal_ptr());
    auto* ep_ptr = static_cast<::fid_ep*>(ep.impl_internal_ptr());

    auto* res = resource ? resource : std::pmr::get_default_resource();
    auto impl = loom::detail::make_pmr_unique<detail::deferred_work_impl>(res, domain_ptr);

    impl->iov_storage.assign(local_iov.begin(), local_iov.end());
    impl->desc_storage.reserve(desc.size());
    for (const auto& d : desc) {
        impl->desc_storage.push_back(d.raw());
    }
    impl->rma_iov_storage.reserve(remote_iov.size());
    for (const auto& r : remote_iov) {
        ::fi_rma_iov fi_r{};
        fi_r.addr = r.addr;
        fi_r.len = r.len;
        fi_r.key = r.key;
        impl->rma_iov_storage.push_back(fi_r);
    }

    impl->op_rma.ep = ep_ptr;
    impl->op_rma.msg.msg_iov = impl->iov_storage.data();
    impl->op_rma.msg.desc = impl->desc_storage.empty() ? nullptr : impl->desc_storage.data();
    impl->op_rma.msg.iov_count = impl->iov_storage.size();
    impl->op_rma.msg.addr = dest_addr.get();
    impl->op_rma.msg.rma_iov = impl->rma_iov_storage.data();
    impl->op_rma.msg.rma_iov_count = impl->rma_iov_storage.size();
    impl->op_rma.msg.context = nullptr;
    impl->op_rma.msg.data = 0;
    impl->op_rma.flags = flags;

    impl->work.threshold = trigger.threshold;
    impl->work.triggering_cntr =
        trigger.cntr ? static_cast<::fid_cntr*>(trigger.cntr->impl_internal_ptr()) : nullptr;
    impl->work.completion_cntr =
        completion_cntr ? static_cast<::fid_cntr*>(completion_cntr->impl_internal_ptr()) : nullptr;
    impl->work.op_type = FI_OP_READ;
    impl->work.op.rma = &impl->op_rma;

    int ret = ::fi_control(&domain_ptr->fid, FI_QUEUE_WORK, &impl->work);
    if (ret != 0) {
        return make_error_result_from_fi_errno<deferred_work>(-ret);
    }

    impl->queued = true;
    return deferred_work{std::move(impl)};
}

auto deferred_work::create_write(domain& dom,
                                 endpoint& ep,
                                 const threshold_condition& trigger,
                                 counter* completion_cntr,
                                 std::span<const iovec> local_iov,
                                 std::span<const mr_descriptor> desc,
                                 std::span<const rma::rma_iov> remote_iov,
                                 fabric_addr dest_addr,
                                 std::uint64_t flags,
                                 std::uint64_t data,
                                 memory_resource* resource) -> result<deferred_work> {
    if (!dom || !ep) {
        return make_error_result<deferred_work>(errc::invalid_argument);
    }

    auto* domain_ptr = static_cast<::fid_domain*>(dom.internal_ptr());
    auto* ep_ptr = static_cast<::fid_ep*>(ep.impl_internal_ptr());

    auto* res = resource ? resource : std::pmr::get_default_resource();
    auto impl = loom::detail::make_pmr_unique<detail::deferred_work_impl>(res, domain_ptr);

    impl->iov_storage.assign(local_iov.begin(), local_iov.end());
    impl->desc_storage.reserve(desc.size());
    for (const auto& d : desc) {
        impl->desc_storage.push_back(d.raw());
    }
    impl->rma_iov_storage.reserve(remote_iov.size());
    for (const auto& r : remote_iov) {
        ::fi_rma_iov fi_r{};
        fi_r.addr = r.addr;
        fi_r.len = r.len;
        fi_r.key = r.key;
        impl->rma_iov_storage.push_back(fi_r);
    }

    impl->op_rma.ep = ep_ptr;
    impl->op_rma.msg.msg_iov = impl->iov_storage.data();
    impl->op_rma.msg.desc = impl->desc_storage.empty() ? nullptr : impl->desc_storage.data();
    impl->op_rma.msg.iov_count = impl->iov_storage.size();
    impl->op_rma.msg.addr = dest_addr.get();
    impl->op_rma.msg.rma_iov = impl->rma_iov_storage.data();
    impl->op_rma.msg.rma_iov_count = impl->rma_iov_storage.size();
    impl->op_rma.msg.context = nullptr;
    impl->op_rma.msg.data = data;
    impl->op_rma.flags = flags;

    impl->work.threshold = trigger.threshold;
    impl->work.triggering_cntr =
        trigger.cntr ? static_cast<::fid_cntr*>(trigger.cntr->impl_internal_ptr()) : nullptr;
    impl->work.completion_cntr =
        completion_cntr ? static_cast<::fid_cntr*>(completion_cntr->impl_internal_ptr()) : nullptr;
    impl->work.op_type = FI_OP_WRITE;
    impl->work.op.rma = &impl->op_rma;

    int ret = ::fi_control(&domain_ptr->fid, FI_QUEUE_WORK, &impl->work);
    if (ret != 0) {
        return make_error_result_from_fi_errno<deferred_work>(-ret);
    }

    impl->queued = true;
    return deferred_work{std::move(impl)};
}

auto deferred_work::create_counter_set(domain& dom,
                                       const threshold_condition& trigger,
                                       counter& target_cntr,
                                       std::uint64_t value,
                                       memory_resource* resource) -> result<deferred_work> {
    if (!dom || !target_cntr) {
        return make_error_result<deferred_work>(errc::invalid_argument);
    }

    auto* domain_ptr = static_cast<::fid_domain*>(dom.internal_ptr());

    auto* res = resource ? resource : std::pmr::get_default_resource();
    auto impl = loom::detail::make_pmr_unique<detail::deferred_work_impl>(res, domain_ptr);

    impl->op_cntr.cntr = static_cast<::fid_cntr*>(target_cntr.impl_internal_ptr());
    impl->op_cntr.value = value;

    impl->work.threshold = trigger.threshold;
    impl->work.triggering_cntr =
        trigger.cntr ? static_cast<::fid_cntr*>(trigger.cntr->impl_internal_ptr()) : nullptr;
    impl->work.completion_cntr = nullptr;
    impl->work.op_type = FI_OP_CNTR_SET;
    impl->work.op.cntr = &impl->op_cntr;

    int ret = ::fi_control(&domain_ptr->fid, FI_QUEUE_WORK, &impl->work);
    if (ret != 0) {
        return make_error_result_from_fi_errno<deferred_work>(-ret);
    }

    impl->queued = true;
    return deferred_work{std::move(impl)};
}

auto deferred_work::create_counter_add(domain& dom,
                                       const threshold_condition& trigger,
                                       counter& target_cntr,
                                       std::uint64_t value,
                                       memory_resource* resource) -> result<deferred_work> {
    if (!dom || !target_cntr) {
        return make_error_result<deferred_work>(errc::invalid_argument);
    }

    auto* domain_ptr = static_cast<::fid_domain*>(dom.internal_ptr());

    auto* res = resource ? resource : std::pmr::get_default_resource();
    auto impl = loom::detail::make_pmr_unique<detail::deferred_work_impl>(res, domain_ptr);

    impl->op_cntr.cntr = static_cast<::fid_cntr*>(target_cntr.impl_internal_ptr());
    impl->op_cntr.value = value;

    impl->work.threshold = trigger.threshold;
    impl->work.triggering_cntr =
        trigger.cntr ? static_cast<::fid_cntr*>(trigger.cntr->impl_internal_ptr()) : nullptr;
    impl->work.completion_cntr = nullptr;
    impl->work.op_type = FI_OP_CNTR_ADD;
    impl->work.op.cntr = &impl->op_cntr;

    int ret = ::fi_control(&domain_ptr->fid, FI_QUEUE_WORK, &impl->work);
    if (ret != 0) {
        return make_error_result_from_fi_errno<deferred_work>(-ret);
    }

    impl->queued = true;
    return deferred_work{std::move(impl)};
}

auto flush_work(domain& dom) -> void_result {
    if (!dom) {
        return make_error_result<void>(errc::invalid_argument);
    }

    auto* domain_ptr = static_cast<::fid_domain*>(dom.internal_ptr());

    int ret = ::fi_control(&domain_ptr->fid, FI_FLUSH_WORK, nullptr);
    if (ret != 0) {
        return make_error_result_from_fi_errno<void>(-ret);
    }

    return make_success();
}

auto flush_work(domain& dom, counter& triggering_cntr) -> void_result {
    if (!dom || !triggering_cntr) {
        return make_error_result<void>(errc::invalid_argument);
    }

    auto* domain_ptr = static_cast<::fid_domain*>(dom.internal_ptr());
    auto* cntr_ptr = static_cast<::fid_cntr*>(triggering_cntr.impl_internal_ptr());

    int ret = ::fi_control(&domain_ptr->fid, FI_FLUSH_WORK, cntr_ptr);
    if (ret != 0) {
        return make_error_result_from_fi_errno<void>(-ret);
    }

    return make_success();
}

auto supports_trigger(const endpoint& ep) -> bool {
    if (!ep) {
        return false;
    }

    auto* fid_ep = static_cast<::fid_ep*>(ep.impl_internal_ptr());
    if (fid_ep == nullptr) {
        return false;
    }

    return true;
}

}  // namespace loom::trigger
