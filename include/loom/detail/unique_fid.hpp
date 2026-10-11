// SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only
#pragma once

#include <rdma/fabric.h>

#include <memory>

namespace loom::detail {

struct fid_deleter {
    template <typename T>
    void operator()(T* handle) const noexcept {
        ::fi_close(&handle->fid);
    }
};

// Retain native ownership until construction of the C++ owner succeeds.
template <typename T>
using unique_fid = std::unique_ptr<T, fid_deleter>;

}  // namespace loom::detail
