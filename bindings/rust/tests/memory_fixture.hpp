// SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only
#pragma once
#include <cstdint>

namespace loom::cxx_bridge {
struct RemoteMemory;
auto test_memory_key(bool raw, std::uint64_t key) -> std::uint64_t;
auto test_memory_export(bool raw, std::uint64_t key) -> RemoteMemory;
}  // namespace loom::cxx_bridge
