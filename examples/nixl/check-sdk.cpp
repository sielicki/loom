// SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only
#include <iostream>
#include <type_traits>

#include <backend/backend_plugin.h>
#include <nixl.h>

static_assert(std::has_virtual_destructor_v<nixlBackendEngine>);

int main() {
    nixlAgent agent{"loom-sdk-check", nixlAgentConfig{}};
    nixl_mem_list_t memory_types;
    nixl_b_params_t options;
    // Exercise the installed plugin loader before adding the Loom plugin.
    const auto status = agent.getPluginParams("POSIX", memory_types, options);
    if (status != NIXL_SUCCESS || memory_types.empty()) {
        std::cerr << "Installed NIXL plugin loading failed: " << status << '\n';
        return 1;
    }
}
