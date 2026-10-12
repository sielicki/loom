// SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only
#include <loom/core/endpoint.hpp>
#include <loom/detail/impl/endpoint_impl.hpp>

#include <array>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

namespace {
enum class support { no_table, no_entry, short_table, available };
struct submission_provider {
    int calls{};
    int status{-FI_EAGAIN};
    fi_ops ops{.size = sizeof(fi_ops), .close = [](fid*) { return 0; }};
    fi_ops_msg messages{.size = sizeof(fi_ops_msg)};
    fi_ops_tagged tagged{.size = sizeof(fi_ops_tagged)};
    fi_ops_rma rma{.size = sizeof(fi_ops_rma)};
    fi_ops_ep endpoint_ops{.size = sizeof(fi_ops_ep)};
    fid_ep handle{.fid = {.context = this, .ops = &ops},
                  .ops = &endpoint_ops,
                  .msg = &messages,
                  .rma = &rma,
                  .tagged = &tagged};
    loom::endpoint endpoint{loom::detail::make_pmr_unique<loom::detail::endpoint_impl>(
        loom::get_default_resource(),
        &handle,
        loom::endpoint_types::msg,
        loom::address_format::unspecified,
        0UZ,
        0UZ,
        loom::caps{},
        0,
        0,
        nullptr,
        loom::endpoint_iov_limits{1, 1, 1})};

    explicit submission_provider(support state) {
        if (state == support::no_table) {
            handle.msg = nullptr;
            handle.tagged = nullptr;
            handle.rma = nullptr;
            handle.ops = nullptr;
            return;
        }
        if (state == support::no_entry)
            return;
        auto submit = [](fid_ep* endpoint, auto...) -> ssize_t {
            auto& self = *static_cast<submission_provider*>(endpoint->fid.context);
            ++self.calls;
            return self.status;
        };
        messages.send = submit;
        messages.recv = submit;
        messages.sendv = submit;
        messages.recvv = submit;
        messages.inject = submit;
        messages.injectdata = submit;
        tagged.send = submit;
        tagged.recv = submit;
        tagged.inject = submit;
        tagged.injectdata = submit;
        rma.read = submit;
        rma.write = submit;
        endpoint_ops.cancel = [](fid* endpoint, void*) -> ssize_t {
            auto& self = *static_cast<submission_provider*>(endpoint->context);
            ++self.calls;
            return self.status;
        };
        if (state == support::short_table) {
            messages.size = tagged.size = rma.size = endpoint_ops.size = 0;
        }
    }
};
}  // namespace

TEST_CASE("endpoint submissions require native operation entries", "[endpoint][submission]") {
    const auto state =
        GENERATE(support::no_table, support::no_entry, support::short_table, support::available);
    const auto status = GENERATE(0, -FI_EAGAIN, -FI_EIO);
    submission_provider native{state};
    native.status = status;
    auto& ep = native.endpoint;
    std::array<std::byte, 8> bytes{};
    std::array buffers{iovec{bytes.data(), bytes.size()}};
    fi_context2 storage{};
    const loom::context_ptr<void> context{&storage};
    const loom::fabric_addr peer{7};
    const loom::rma_addr address{11};
    const loom::mr_key key{13};
    const std::array results{
        ep.send(bytes, context),
        ep.recv(bytes, context),
        ep.sendv(buffers, context),
        ep.recvv(buffers, context),
        ep.tagged_send(bytes, 17, context),
        ep.tagged_recv(bytes, 17, 0, context),
        ep.inject(bytes, peer),
        ep.inject_data(bytes, 19, peer),
        ep.tagged_inject(bytes, 17, peer),
        ep.tagged_inject_data(bytes, 17, 19, peer),
        ep.cancel(context),
        ep.read(bytes, address, key, context),
        ep.write(bytes, address, key, context),
    };
    for (const auto& result : results) {
        if (state != support::available) {
            REQUIRE_FALSE(result);
            REQUIRE(result.error() == loom::errc::not_supported);
        } else if (status) {
            REQUIRE_FALSE(result);
            REQUIRE(result.error() == loom::make_error_code_from_fi_errno(-status));
        } else {
            REQUIRE(result);
        }
    }
    REQUIRE(native.calls == (state == support::available ? results.size() : 0));
}
