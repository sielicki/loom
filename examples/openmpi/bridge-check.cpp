// SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>

#include "bridge.h"

namespace {
void require(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "Open MPI Loom bridge: %s\n", message);
        std::exit(1);
    }
}

using native_info = std::unique_ptr<fi_info, decltype(&fi_freeinfo)>;
}  // namespace

int main(int argc, char** argv) {
    require(argc == 2, "provider filter required");
    native_info hints{fi_allocinfo(), fi_freeinfo};
    require(hints != nullptr, "allocate discovery hints");
    hints->caps = FI_MSG | FI_TAGGED | FI_DIRECTED_RECV;
    hints->mode = FI_CONTEXT | FI_CONTEXT2;
    hints->ep_attr->type = FI_EP_RDM;
    hints->addr_format = FI_SOCKADDR_IN;
    hints->fabric_attr->prov_name = ::strdup(argv[1]);
    require(hints->fabric_attr->prov_name != nullptr, "copy provider filter");
    fi_info* discovered{};
    require(fi_getinfo(FI_VERSION(2, 7), "127.0.0.1", "0", FI_SOURCE, hints.get(), &discovered) ==
                0,
            "discover software profile");
    native_info info{discovered, fi_freeinfo};
    ompi_mtl_ofi_loom* graph{};
    ompi_mtl_ofi_loom_handles handles{};

    const auto reject = [&](auto mutate) {
        auto candidate = *info;
        auto domain = *info->domain_attr;
        auto tx = *info->tx_attr;
        candidate.domain_attr = &domain;
        candidate.tx_attr = &tx;
        mutate(candidate);
        require(ompi_mtl_ofi_loom_open(&candidate, FI_AV_MAP, 2, 64, &graph, &handles) ==
                    -FI_ENOSYS,
                "reject unsupported requirements");
        require(!graph && ompi_mtl_ofi_loom_active() == 0, "rejection publishes no transport");
    };
    reject([](fi_info& candidate) { candidate.caps &= ~FI_TAGGED; });
    reject([](fi_info& candidate) { candidate.mode |= FI_MSG_PREFIX; });
    reject([](fi_info& candidate) { candidate.tx_attr->mode |= FI_MSG_PREFIX; });
    reject([](fi_info& candidate) { candidate.domain_attr->mr_mode = FI_MR_LOCAL; });
    reject([](fi_info& candidate) { candidate.domain_attr->mr_mode = FI_MR_BASIC; });
    reject([](fi_info& candidate) { candidate.domain_attr->mr_mode = FI_MR_ENDPOINT; });
    reject([](fi_info& candidate) { candidate.addr_format = FI_ADDR_STR; });

    require(ompi_mtl_ofi_loom_open(info.get(), FI_AV_MAP, 2, 64, &graph, &handles) == 0,
            "open supported graph");
    info.reset();
    hints.reset();
    require(graph && handles.ep && handles.cq && handles.av && handles.domain && handles.fabric,
            "publish borrowed handles");
    require(ompi_mtl_ofi_loom_active() == 1, "native discovery lifetime is independent");
    auto* original_ops = handles.ep->fid.ops;
    auto busy_ops = *original_ops;
    busy_ops.close = [](fid*) {
        return -FI_EBUSY;
    };
    handles.ep->fid.ops = &busy_ops;
    require(ompi_mtl_ofi_loom_close(&graph) == -FI_EBUSY, "report native close failure");
    require(graph && ompi_mtl_ofi_loom_active() == 1, "failed close retains ownership");
    handles.ep->fid.ops = original_ops;
    require(ompi_mtl_ofi_loom_close(&graph) == 0, "close graph");
    require(!graph && ompi_mtl_ofi_loom_active() == 0, "retire graph");
    require(ompi_mtl_ofi_loom_close(&graph) == 0, "close empty handle");
}
