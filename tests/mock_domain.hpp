// SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only
#pragma once

#include <loom/detail/impl/domain_impl.hpp>
#include <loom/detail/impl/memory_impl.hpp>

namespace loom::test {

// Exercise registration and ownership without requiring RDMA hardware.
struct mock_domain {
    std::uint64_t access{};
    std::uint64_t flags{};
    std::size_t registrations{};
    std::size_t live_regions{};

    fi_ops domain_ops{
        .size = sizeof(fi_ops),
        .close = [](fid*) { return 0; },
    };
    fi_ops region_ops{
        .size = sizeof(fi_ops),
        .close =
            [](fid* handle) {
                auto& self = *static_cast<mock_domain*>(handle->context);
                --self.live_regions;
                delete reinterpret_cast<fid_mr*>(handle);
                return 0;
            },
    };
    fi_ops_mr mr_ops{
        .size = sizeof(fi_ops_mr),
        .reg =
            [](fid* handle,
               const void*,
               std::size_t,
               std::uint64_t access,
               std::uint64_t,
               std::uint64_t,
               std::uint64_t flags,
               fid_mr** region,
               void*) {
                return static_cast<mock_domain*>(handle->context)
                    ->register_region(access, flags, region);
            },
        .regattr =
            [](fid* handle, const fi_mr_attr* attr, std::uint64_t flags, fid_mr** region) {
                return static_cast<mock_domain*>(handle->context)
                    ->register_region(attr->access, flags, region);
            },
    };
    fid_domain handle{
        .fid = {.context = this, .ops = &domain_ops},
        .mr = &mr_ops,
    };
    domain dom{detail::make_pmr_unique<detail::domain_impl>(get_default_resource(), &handle)};

    auto register_region(std::uint64_t permissions, std::uint64_t options, fid_mr** region) -> int {
        access = permissions;
        flags = options;
        *region = new fid_mr{};
        (*region)->fid.context = this;
        (*region)->fid.ops = &region_ops;
        (*region)->key = ++registrations;
        (*region)->mem_desc = *region;
        ++live_regions;
        return 0;
    }
};

}  // namespace loom::test
