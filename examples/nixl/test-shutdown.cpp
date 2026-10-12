// SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only
#include <rdma/fi_endpoint.h>
#include <rdma/fi_tagged.h>

#include <atomic>
#include <cstdio>

#include "backend.cpp"

namespace {
enum class cancellation { supported, unsupported, partial, unsupported_close_busy };

// Preserve real provider operations while injecting unsupported cancellation
// and a retriable close failure. The tables outlive the endpoint and worker.
struct fault_operations : fi_ops, fi_ops_ep, fi_ops_tagged {
    decltype(fi_ops::close) native_close;
    decltype(fi_ops_ep::cancel) native_cancel;
    decltype(fi_ops_tagged::recvmsg) native_receive;
    std::weak_ptr<resources> transport;
    cancellation policy;
    std::atomic<unsigned> receives{}, cancels{}, closes{};
    bool refuse_close_once;
    bool storage_retained{true};

    fault_operations(fid_ep& ep, const std::shared_ptr<resources>& owner, cancellation selected)
        : fi_ops(*ep.fid.ops), fi_ops_ep(*ep.ops), fi_ops_tagged(*ep.tagged),
          native_close(fi_ops::close), native_cancel(fi_ops_ep::cancel),
          native_receive(fi_ops_tagged::recvmsg), transport(owner), policy(selected),
          refuse_close_once(selected == cancellation::unsupported_close_busy) {
        fi_ops::close = [](fid* handle) -> int {
            auto& self = *static_cast<fault_operations*>(handle->ops);
            ++self.closes;
            if (const auto transport = self.transport.lock();
                transport && self.policy != cancellation::supported) {
                self.storage_retained &=
                    transport->receives.size() == 24 &&
                    std::ranges::all_of(transport->receives, [](const auto& slot) {
                        return slot && bool(slot->region);
                    });
            }
            if (std::exchange(self.refuse_close_once, false))
                return -FI_EBUSY;
            return self.native_close(handle);
        };
        fi_ops_ep::cancel = [](fid* handle, void* context) -> ssize_t {
            auto& self = *static_cast<fault_operations*>(reinterpret_cast<fid_ep*>(handle)->ops);
            const auto call = self.cancels.fetch_add(1);
            if (self.policy == cancellation::unsupported ||
                self.policy == cancellation::unsupported_close_busy ||
                (self.policy == cancellation::partial && call == 0))
                return -FI_ENOSYS;
            return self.native_cancel(handle, context);
        };
        fi_ops_tagged::recvmsg =
            [](fid_ep* handle, const fi_msg_tagged* message, std::uint64_t flags) -> ssize_t {
            auto& self = *static_cast<fault_operations*>(handle->tagged);
            const auto result = self.native_receive(handle, message, flags);
            if (result == 0)
                ++self.receives;
            return result;
        };
        ep.fid.ops = this;
        ep.ops = this;
        ep.tagged = this;
    }
};

void require(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}

void shutdown(const char* provider, cancellation policy) {
    nixl_b_params_t settings{{"provider", provider}, {"domain", "lo"}, {"cuda", "0"}};
    nixlBackendInitParams params{};
    params.localAgent = "shutdown";
    params.type = "LOOM";
    params.customParams = &settings;
    std::optional<fault_operations> faults;
    auto transport = std::make_shared<resources>(params);
    faults.emplace(*static_cast<fid_ep*>(transport->endpoint.internal_ptr()), transport, policy);
    {
        engine backend{&params, transport};
        const auto deadline = std::chrono::steady_clock::now() + 5s;
        while (faults->receives.load() != 24) {
            require(std::chrono::steady_clock::now() < deadline, "receives were not posted");
            std::this_thread::yield();
        }
        // Destruction must retire every accepted receive even when its cancel
        // fails. The test timeout detects waiting for nonexistent CQ events.
    }
    require(faults->cancels >= 24, "not all pending receives were offered for cancellation");
    require(faults->closes >= (policy == cancellation::unsupported_close_busy ? 2U : 1U),
            "native close was not completed");
    require(faults->storage_retained, "receive storage was released before native close");
    require(!transport->endpoint && transport->receives.empty(), "live receive storage remains");
}
}  // namespace

int main(int argc, char** argv) try {
    require(argc == 2, "expected provider");
    for (const auto policy : {cancellation::supported,
                              cancellation::unsupported,
                              cancellation::partial,
                              cancellation::unsupported_close_busy}) {
        std::printf("shutdown scenario %d\n", std::to_underlying(policy));
        std::fflush(stdout);
        shutdown(argv[1], policy);
    }
    return 0;
} catch (const std::exception& error) {
    std::fprintf(stderr, "%s\n", error.what());
    return 1;
}
