// SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only
#include "completion_fixture.hpp"

#include <loom/detail/impl/domain_impl.hpp>

#include <array>
#include <cstring>
#include <stdexcept>

#include "loom-fabric/src/ffi.rs.h"

namespace loom::cxx_bridge {
namespace {
struct completion_provider {
    TestCompletionKind kind;
    std::uintptr_t context;
    fi_addr_t source;
    bool buffer;
    bool zero_error_info;
    std::array<std::uint8_t, 4> diagnostic{0x52, 0, 0xff, 0x79};

    static auto owner(fid_cq* cq) -> completion_provider& {
        return *static_cast<completion_provider*>(cq->fid.context);
    }
    auto read(void* entries, std::size_t count, fi_addr_t* addresses = nullptr) -> ssize_t {
        if (!count)
            return -FI_EINVAL;
        switch (kind) {
            case TestCompletionKind::Error:
                return -FI_EAVAIL;
            case TestCompletionKind::QueueFailure:
                return -FI_EIO;
            case TestCompletionKind::Empty:
                return -FI_EAGAIN;
            case TestCompletionKind::Success:
                break;
            default:
                return -FI_EINVAL;
        }
        *static_cast<fi_cq_tagged_entry*>(entries) = {
            .op_context = reinterpret_cast<void*>(context),
            .flags = FI_RECV | FI_TAGGED | FI_REMOTE_CQ_DATA,
            .len = 5,
            .buf = buffer ? reinterpret_cast<void*>(std::uintptr_t{0x4321}) : nullptr,
            .data = 0xfedcba9876543210,
            .tag = 0xabcdef0123456789,
        };
        if (addresses)
            *addresses = source;
        return 1;
    }

    fi_ops lifetime_ops{.size = sizeof(fi_ops), .close = [](fid*) { return 0; }};
    fi_ops_cq cq_ops{
        .size = sizeof(fi_ops_cq),
        .read = [](fid_cq* cq,
                   void* entries,
                   std::size_t count) { return owner(cq).read(entries, count); },
        .readfrom =
            [](fid_cq* cq, void* entries, std::size_t count, fi_addr_t* addresses) {
                return owner(cq).read(entries, count, addresses);
            },
        .readerr = [](fid_cq* cq, fi_cq_err_entry* entry, std::uint64_t) -> ssize_t {
            auto& self = owner(cq);
            auto* output = entry->err_data;
            if (!output || entry->err_data_size < self.diagnostic.size())
                return -FI_ETOOSMALL;
            *entry = {
                .op_context = reinterpret_cast<void*>(self.context),
                .flags = FI_RECV | FI_TAGGED | FI_REMOTE_CQ_DATA,
                .len = 5,
                .buf = self.buffer ? reinterpret_cast<void*>(std::uintptr_t{0x4321}) : nullptr,
                .data = 0xfedcba9876543210,
                .tag = 0xabcdef0123456789,
                .olen = self.zero_error_info ? 0U : 17U,
                .err = FI_ETRUNC,
                .prov_errno = self.zero_error_info ? 0 : -37,
                .err_data = output,
                .err_data_size = self.zero_error_info ? 0 : self.diagnostic.size(),
                .src_addr = self.source,
            };
            if (entry->err_data_size)
                std::memcpy(output, self.diagnostic.data(), entry->err_data_size);
            return 1;
        },
        .sread =
            [](fid_cq* cq, void* entries, std::size_t count, const void*, int) {
                return owner(cq).read(entries, count);
            },
        .sreadfrom = [](fid_cq* cq,
                        void* entries,
                        std::size_t count,
                        fi_addr_t* addresses,
                        const void*,
                        int) { return owner(cq).read(entries, count, addresses); },
    };
    fid_cq native_cq{.fid = {.context = this, .ops = &lifetime_ops}, .ops = &cq_ops};
    fi_ops_domain domain_ops{
        .size = sizeof(fi_ops_domain),
        .cq_open =
            [](fid_domain* domain, fi_cq_attr* attr, fid_cq** output, void*) {
                if (attr->size != 16 || attr->format != FI_CQ_FORMAT_TAGGED)
                    return -FI_EINVAL;
                *output = &static_cast<completion_provider*>(domain->fid.context)->native_cq;
                return 0;
            },
    };
    fid_domain native_domain{.fid = {.context = this, .ops = &lifetime_ops}, .ops = &domain_ops};
};
}  // namespace

auto test_completion_read(TestCompletionKind kind,
                          const CqAttributes& attributes,
                          std::uintptr_t context,
                          std::uint64_t source,
                          bool buffer,
                          bool zero_error_info) -> CqReadResult {
    completion_provider provider{kind, context, source, buffer, zero_error_info};
    auto impl = loom::detail::make_pmr_unique<loom::detail::domain_impl>(
        loom::get_default_resource(), &provider.native_domain);
    impl->max_error_data = provider.diagnostic.size();
    DomainHandle domain{loom::domain{std::move(impl)}};
    auto queue = create_completion_queue(domain, attributes);
    auto read = [&] {
        return attributes.wait_obj ? cq_wait(*queue, 0) : cq_poll(*queue);
    };
    auto result = read();
    // Subsequent reads and teardown must not change the exported Rust bytes.
    provider.diagnostic.fill(0xcc);
    (void)read();
    return result;
}
}  // namespace loom::cxx_bridge
