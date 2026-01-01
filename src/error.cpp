// SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only
#include "loom/core/error.hpp"

#include <rdma/fi_errno.h>

namespace loom {

auto error_category::message(int ev) const -> std::string {
    const char* msg = ::fi_strerror(-ev);
    return msg ? msg : "Unknown loom error";
}

}  // namespace loom
