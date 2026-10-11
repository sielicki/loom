// SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only
#include <loom/loom.hpp>
#ifdef CHECK_EXECUTION
#    include <loom/execution.hpp>
#endif
#ifdef CHECK_ASIO
#    include <loom/asio.hpp>
#endif

int main() {
    return loom::get_fabric_version().get() < FI_VERSION(2, 7);
}
