// SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only
#include "memory_fixture.hpp"

#include <loom/detail/impl/memory_impl.hpp>

#include "loom-fabric/src/ffi.rs.h"

namespace loom::cxx_bridge {
namespace {
struct memory_fixture {
    fi_ops ops{.size = sizeof(fi_ops), .close = [](fid*) { return 0; }};
    fid_mr native{.fid = {.ops = &ops}};
    auto region(bool raw, std::uint64_t key) -> MemoryRegionHandle {
        native.key = key;
        return MemoryRegionHandle{
            loom::memory_region{loom::detail::make_pmr_unique<loom::detail::memory_region_impl>(
                loom::get_default_resource(),
                &native,
                nullptr,
                64,
                loom::detail::mr_key_lease{},
                0x1234,
                raw ? FI_MR_RAW : 0)}};
    }
};
}  // namespace

auto test_memory_key(bool raw, std::uint64_t key) -> std::uint64_t {
    memory_fixture provider;
    return mr_key(provider.region(raw, key));
}
auto test_memory_export(bool raw, std::uint64_t key) -> RemoteMemory {
    memory_fixture provider;
    return mr_remote_memory(provider.region(raw, key));
}
}  // namespace loom::cxx_bridge
