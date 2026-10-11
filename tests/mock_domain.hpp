// SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only
#pragma once

#include <loom/detail/impl/domain_impl.hpp>
#include <loom/detail/impl/memory_impl.hpp>

#include <mutex>
#include <unordered_set>
#include <vector>

namespace loom::test {

// Exercise registration and ownership without requiring RDMA hardware.
struct mock_domain {
    std::mutex mutex;
    std::unordered_set<std::uint64_t> keys;
    std::vector<std::uint64_t> requested_keys;
    int registration_error{};
    int close_error{};
    bool provider_keys{};
    std::uint64_t access{};
    std::uint64_t flags{};
    fi_hmem_iface iface{};
    int device_id{};
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
                std::lock_guard lock{self.mutex};
                if (self.close_error)
                    return self.close_error;
                --self.live_regions;
                auto* region = reinterpret_cast<fid_mr*>(handle);
                self.keys.erase(region->key);
                delete region;
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
               std::uint64_t requested_key,
               std::uint64_t flags,
               fid_mr** region,
               void*) {
                return static_cast<mock_domain*>(handle->context)
                    ->register_region(access, flags, requested_key, region);
            },
        .regattr =
            [](fid* handle, const fi_mr_attr* attr, std::uint64_t flags, fid_mr** region) {
                auto& self = *static_cast<mock_domain*>(handle->context);
                self.iface = attr->iface;
                switch (attr->iface) {
                    case FI_HMEM_CUDA:
                        self.device_id = attr->device.cuda;
                        break;
                    case FI_HMEM_ZE:
                        self.device_id = attr->device.ze;
                        break;
                    case FI_HMEM_NEURON:
                        self.device_id = attr->device.neuron;
                        break;
                    case FI_HMEM_SYNAPSEAI:
                        self.device_id = attr->device.synapseai;
                        break;
#if FI_VERSION(FI_MAJOR_VERSION, FI_MINOR_VERSION) >= FI_VERSION(2, 5)
                    case FI_HMEM_ROCR:
                        self.device_id = attr->device.rocr;
                        break;
#endif
                    default:
                        self.device_id = 0;
                        break;
                }
                return self.register_region(attr->access, flags, attr->requested_key, region);
            },
    };
    fid_domain handle{
        .fid = {.context = this, .ops = &domain_ops},
        .mr = &mr_ops,
    };
    domain dom;

    explicit mock_domain(int mode = 0, std::size_t key_bytes = sizeof(std::uint64_t))
        : provider_keys((mode & FI_MR_PROV_KEY) || mode == FI_MR_BASIC),
          dom(detail::make_pmr_unique<detail::domain_impl>(get_default_resource(),
                                                           &handle,
                                                           threading_mode::safe,
                                                           progress_mode::manual,
                                                           progress_mode::manual,
                                                           address_format::unspecified,
                                                           std::string{},
                                                           mode,
                                                           key_bytes)) {}

    auto register_region(std::uint64_t permissions,
                         std::uint64_t options,
                         std::uint64_t requested_key,
                         fid_mr** region) -> int {
        std::lock_guard lock{mutex};
        requested_keys.push_back(requested_key);
        if (registration_error)
            return registration_error;
        const auto key = provider_keys ? registrations + 1 : requested_key;
        if (!keys.insert(key).second)
            return -FI_ENOKEY;
        access = permissions;
        flags = options;
        *region = new fid_mr{};
        (*region)->fid.context = this;
        (*region)->fid.ops = &region_ops;
        ++registrations;
        (*region)->key = key;
        (*region)->mem_desc = *region;
        ++live_regions;
        return 0;
    }
};

}  // namespace loom::test
