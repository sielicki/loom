// SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only
#include "registration_fixture.hpp"

#include <loom/detail/impl/fabric_impl.hpp>

#include <rdma/fi_rma.h>

#include <chrono>
#include <optional>

#include "loom-fabric/src/ffi.rs.h"

namespace loom::cxx_bridge {
struct RegistrationFixture::state {
    bool endpoint_bound;
    TestRegistrationState counts{};
    TestRegistrationFailure failure{TestRegistrationFailure::None};
    std::optional<std::uintptr_t> completion;
    std::chrono::steady_clock::time_point completion_ready;

    struct region {
        fid_mr native{};
        state* owner;
        fid_ep* endpoint{};
        bool enabled{};
    };

    static auto owner(fid* handle) -> state& { return *static_cast<state*>(handle->context); }
    static auto memory(fid* handle) -> region& { return *static_cast<region*>(handle->context); }

    fi_ops fabric_lifetime{
        .size = sizeof(fi_ops),
        .close =
            [](fid* handle) {
                auto& self = owner(handle);
                self.counts.bad_close_order += self.counts.domains != 0;
                --self.counts.fabrics;
                delete reinterpret_cast<fid_fabric*>(handle);
                return 0;
            },
    };
    fi_ops domain_lifetime{
        .size = sizeof(fi_ops),
        .close =
            [](fid* handle) {
                auto& self = owner(handle);
                self.counts.bad_close_order += self.counts.endpoints != 0 ||
                                               self.counts.regions != 0 ||
                                               self.counts.queues != 0 || self.counts.vectors != 0;
                --self.counts.domains;
                delete reinterpret_cast<fid_domain*>(handle);
                return 0;
            },
    };
    fi_ops endpoint_lifetime{
        .size = sizeof(fi_ops),
        .close =
            [](fid* handle) {
                auto& self = owner(handle);
                ++self.counts.endpoint_close_calls;
                if (self.failure == TestRegistrationFailure::Close)
                    return -FI_EBUSY;
                // Tests close all registrations before either endpoint in endpoint mode.
                self.counts.bad_close_order += self.endpoint_bound && self.counts.regions != 0;
                --self.counts.endpoints;
                delete reinterpret_cast<fid_ep*>(handle);
                return 0;
            },
        .bind = [](fid*, fid*, std::uint64_t) { return 0; },
        .control = [](fid*, int command, void*) { return command == FI_ENABLE ? 0 : -FI_ENOSYS; },
    };
    fi_ops queue_lifetime{
        .size = sizeof(fi_ops),
        .close =
            [](fid* handle) {
                auto& self = owner(handle);
                self.counts.bad_close_order += self.counts.endpoints != 0;
                --self.counts.queues;
                delete reinterpret_cast<fid_cq*>(handle);
                return 0;
            },
    };
    fi_ops vector_lifetime{
        .size = sizeof(fi_ops),
        .close =
            [](fid* handle) {
                auto& self = owner(handle);
                self.counts.bad_close_order += self.counts.endpoints != 0;
                --self.counts.vectors;
                delete reinterpret_cast<fid_av*>(handle);
                return 0;
            },
    };
    fi_ops memory_lifetime{
        .size = sizeof(fi_ops),
        .close =
            [](fid* handle) {
                auto& value = memory(handle);
                --value.owner->counts.regions;
                delete &value;
                return 0;
            },
        .bind =
            [](fid* handle, fid* endpoint, std::uint64_t flags) {
                auto& value = memory(handle);
                auto& self = *value.owner;
                ++self.counts.bind_calls;
                if (self.failure == TestRegistrationFailure::Bind)
                    return -FI_EIO;
                if (flags || endpoint->fclass != FI_CLASS_EP || value.enabled)
                    return -FI_EINVAL;
                value.endpoint = reinterpret_cast<fid_ep*>(endpoint);
                return 0;
            },
        .control =
            [](fid* handle, int command, void*) {
                auto& value = memory(handle);
                auto& self = *value.owner;
                ++self.counts.enable_calls;
                if (self.failure == TestRegistrationFailure::Enable)
                    return -FI_EIO;
                if (command != FI_ENABLE || !value.endpoint)
                    return -FI_EINVAL;
                value.enabled = true;
                value.native.key = 42;
                return 0;
            },
    };

    auto register_memory(fid_mr** output) -> int {
        ++counts.registration_calls;
        if (failure == TestRegistrationFailure::Register)
            return -FI_ENOMEM;
        auto value = std::make_unique<region>();
        value->owner = this;
        value->enabled = !endpoint_bound;
        value->native = {
            .fid = {.fclass = FI_CLASS_MR, .context = value.get(), .ops = &memory_lifetime},
            .mem_desc = value.get(),
            .key = endpoint_bound ? FI_KEY_NOTAVAIL : 42};
        ++counts.regions;
        *output = &value.release()->native;
        return 0;
    }

    fi_ops_mr memory_ops{
        .size = sizeof(fi_ops_mr),
        .reg = [](fid* handle,
                  const void*,
                  std::size_t,
                  std::uint64_t,
                  std::uint64_t,
                  std::uint64_t,
                  std::uint64_t,
                  fid_mr** output,
                  void*) { return owner(handle).register_memory(output); },
        .regattr =
            [](fid* handle, const fi_mr_attr* attr, std::uint64_t flags, fid_mr** output) {
                auto& self = owner(handle);
                self.counts.cuda = attr->iface == FI_HMEM_CUDA;
                self.counts.device = self.counts.cuda ? attr->device.cuda : 0;
                self.counts.device_allocation = (flags & FI_HMEM_DEVICE_ONLY) != 0;
                return self.register_memory(output);
            },
    };

    static auto submit(fid_ep* endpoint, void* descriptor) -> ssize_t {
        auto* value = static_cast<region*>(descriptor);
        if (!value || !value->enabled || (value->endpoint && value->endpoint != endpoint))
            return -FI_EINVAL;
        return 0;
    }
    fi_ops_rma rma_ops{
        .size = sizeof(fi_ops_rma),
        .readmsg = [](fid_ep* endpoint, const fi_msg_rma* message, std::uint64_t) -> ssize_t {
            ++owner(&endpoint->fid).counts.read_calls;
            owner(&endpoint->fid).counts.last_context =
                reinterpret_cast<std::uintptr_t>(message->context);
            return submit(endpoint, message->desc[0]);
        },
        .writemsg = [](fid_ep* endpoint, const fi_msg_rma* message, std::uint64_t) -> ssize_t {
            ++owner(&endpoint->fid).counts.write_calls;
            owner(&endpoint->fid).counts.last_context =
                reinterpret_cast<std::uintptr_t>(message->context);
            return submit(endpoint, message->desc[0]);
        },
    };
    fi_ops_cq queue_ops{
        .size = sizeof(fi_ops_cq),
        .read = [](fid_cq* queue, void* output, std::size_t count) -> ssize_t {
            auto& self = owner(&queue->fid);
            ++self.counts.queue_reads;
            if (!count || !self.completion ||
                std::chrono::steady_clock::now() < self.completion_ready)
                return -FI_EAGAIN;
            *static_cast<fi_cq_tagged_entry*>(output) = {
                .op_context = reinterpret_cast<void*>(*self.completion), .len = 7};
            self.completion.reset();
            return 1;
        },
    };
    fi_ops_domain domain_ops{
        .size = sizeof(fi_ops_domain),
        .av_open =
            [](fid_domain* domain, fi_av_attr*, fid_av** output, void*) {
                auto& self = owner(&domain->fid);
                *output = new fid_av{
                    .fid = {.fclass = FI_CLASS_AV, .context = &self, .ops = &self.vector_lifetime}};
                ++self.counts.vectors;
                return 0;
            },
        .cq_open =
            [](fid_domain* domain, fi_cq_attr*, fid_cq** output, void*) {
                auto& self = owner(&domain->fid);
                *output = new fid_cq{
                    .fid = {.fclass = FI_CLASS_CQ, .context = &self, .ops = &self.queue_lifetime},
                    .ops = &self.queue_ops};
                ++self.counts.queues;
                return 0;
            },
        .endpoint =
            [](fid_domain* domain, fi_info*, fid_ep** output, void*) {
                auto& self = owner(&domain->fid);
                *output = new fid_ep{.fid = {.fclass = FI_CLASS_EP,
                                             .context = &self,
                                             .ops = &self.endpoint_lifetime},
                                     .rma = &self.rma_ops};
                ++self.counts.endpoints;
                return 0;
            },
    };
    fi_ops_fabric fabric_ops{
        .size = sizeof(fi_ops_fabric),
        .domain =
            [](fid_fabric* fabric, fi_info*, fid_domain** output, void*) {
                auto& self = owner(&fabric->fid);
                *output = new fid_domain{.fid = {.fclass = FI_CLASS_DOMAIN,
                                                 .context = &self,
                                                 .ops = &self.domain_lifetime},
                                         .ops = &self.domain_ops,
                                         .mr = &self.memory_ops};
                ++self.counts.domains;
                return 0;
            },
    };
};

RegistrationFixture::RegistrationFixture(bool endpoint_bound)
    : impl(std::make_unique<state>(endpoint_bound)) {}
RegistrationFixture::~RegistrationFixture() = default;

auto test_registration_fixture(bool endpoint_bound) -> std::unique_ptr<RegistrationFixture> {
    return std::make_unique<RegistrationFixture>(endpoint_bound);
}
auto test_registration_fabric(RegistrationFixture& fixture) -> std::unique_ptr<FabricHandle> {
    auto& self = *fixture.impl;
    auto* native = new fid_fabric{
        .fid = {.fclass = FI_CLASS_FABRIC, .context = &self, .ops = &self.fabric_lifetime},
        .ops = &self.fabric_ops};
    ++self.counts.fabrics;
    return std::make_unique<FabricHandle>(
        loom::fabric{loom::detail::make_pmr_unique<loom::detail::fabric_impl>(
            loom::get_default_resource(), native, nullptr)});
}
auto test_registration_info(const RegistrationFixture& fixture)
    -> std::unique_ptr<FabricInfoHandle> {
    fi_domain_attr domain{.mr_mode = FI_MR_PROV_KEY | FI_MR_VIRT_ADDR |
                                     (fixture.impl->endpoint_bound ? FI_MR_ENDPOINT : 0),
                          .mr_key_size = sizeof(std::uint64_t)};
    fi_ep_attr endpoint{.type = FI_EP_RDM};
    fi_info info{.caps = FI_RMA | FI_READ | FI_WRITE | FI_HMEM,
                 .ep_attr = &endpoint,
                 .domain_attr = &domain};
    return std::make_unique<FabricInfoHandle>(
        unwrap_or_throw(loom::fabric_info::copy_native(info)));
}
auto test_registration_state(const RegistrationFixture& fixture) -> TestRegistrationState {
    return fixture.impl->counts;
}
void test_registration_fail(RegistrationFixture& fixture, TestRegistrationFailure failure) {
    fixture.impl->failure = failure;
}
void test_queue_schedule(RegistrationFixture& fixture,
                         std::uintptr_t context,
                         std::uint32_t delay_us) {
    fixture.impl->completion = context;
    fixture.impl->completion_ready =
        std::chrono::steady_clock::now() + std::chrono::microseconds{delay_us};
}
}  // namespace loom::cxx_bridge
