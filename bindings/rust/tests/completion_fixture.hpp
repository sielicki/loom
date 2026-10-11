// SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only
#pragma once

#include <cstdint>

namespace loom::cxx_bridge {
struct CqAttributes;
struct CqReadResult;
enum class TestCompletionKind : std::uint8_t;

auto test_completion_read(TestCompletionKind kind,
                          const CqAttributes& attributes,
                          std::uintptr_t context,
                          std::uint64_t source,
                          bool buffer,
                          bool zero_error_info) -> CqReadResult;
}  // namespace loom::cxx_bridge
