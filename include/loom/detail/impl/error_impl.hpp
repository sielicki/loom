// SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only
#pragma once

#include <rdma/fi_errno.h>

#include <string>

#include "loom/core/error.hpp"

#ifdef LOOM_IMPLEMENTATION

namespace loom {

inline auto error_category::message(int ev) const -> std::string {
    const char* msg = ::fi_strerror(-ev);
    return msg ? msg : "Unknown loom error";
}

}  // namespace loom

#endif  // LOOM_IMPLEMENTATION
