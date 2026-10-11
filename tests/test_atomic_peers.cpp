// SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only
#include <loom/async/completion_queue.hpp>
#include <loom/detail/impl/endpoint_impl.hpp>
#include <loom/detail/impl/memory_impl.hpp>
#include <loom/loom.hpp>

#include <array>
#include <chrono>
#include <thread>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

namespace {
struct atomic_peer_provider {
    int calls{};
    ssize_t status{};
    fi_addr_t peer{};
    const void* input{};
    const void* comparison{};
    void* output{};
    void* input_desc{};
    void* compare_desc{};
    void* output_desc{};
    void* context{};
    std::size_t count{};
    fi_op operation{};

    static auto record(fid_ep* ep,
                       const void* input,
                       std::size_t count,
                       void* input_desc,
                       const void* comparison,
                       void* compare_desc,
                       void* output,
                       void* output_desc,
                       fi_addr_t peer,
                       std::uint64_t address,
                       std::uint64_t key,
                       fi_datatype datatype,
                       fi_op operation,
                       void* context) -> ssize_t {
        auto& self = *static_cast<atomic_peer_provider*>(ep->fid.context);
        ++self.calls;
        REQUIRE(address == 24);
        REQUIRE(key == 0x123456789abcdef0ULL);
        REQUIRE(datatype == FI_UINT64);
        self.peer = peer;
        self.input = input;
        self.comparison = comparison;
        self.output = output;
        self.input_desc = input_desc;
        self.compare_desc = compare_desc;
        self.output_desc = output_desc;
        self.context = context;
        self.count = count;
        self.operation = operation;
        return self.status;
    }

    fi_ops lifetime{.size = sizeof(fi_ops), .close = [](fid*) { return 0; }};
    fi_ops_atomic operations{
        .size = sizeof(fi_ops_atomic),
        .write =
            [](fid_ep* ep,
               const void* input,
               std::size_t count,
               void* desc,
               fi_addr_t peer,
               std::uint64_t address,
               std::uint64_t key,
               fi_datatype datatype,
               fi_op operation,
               void* context) {
                return record(ep,
                              input,
                              count,
                              desc,
                              nullptr,
                              nullptr,
                              nullptr,
                              nullptr,
                              peer,
                              address,
                              key,
                              datatype,
                              operation,
                              context);
            },
        .inject =
            [](fid_ep* ep,
               const void* input,
               std::size_t count,
               fi_addr_t peer,
               std::uint64_t address,
               std::uint64_t key,
               fi_datatype datatype,
               fi_op operation) {
                return record(ep,
                              input,
                              count,
                              nullptr,
                              nullptr,
                              nullptr,
                              nullptr,
                              nullptr,
                              peer,
                              address,
                              key,
                              datatype,
                              operation,
                              nullptr);
            },
        .readwrite =
            [](fid_ep* ep,
               const void* input,
               std::size_t count,
               void* desc,
               void* output,
               void* output_desc,
               fi_addr_t peer,
               std::uint64_t address,
               std::uint64_t key,
               fi_datatype datatype,
               fi_op operation,
               void* context) {
                return record(ep,
                              input,
                              count,
                              desc,
                              nullptr,
                              nullptr,
                              output,
                              output_desc,
                              peer,
                              address,
                              key,
                              datatype,
                              operation,
                              context);
            },
        .compwrite = record,
    };
    fid_ep handle{.fid = {.context = this, .ops = &lifetime}, .atomic = &operations};
    loom::endpoint ep{
        loom::detail::make_pmr_unique<loom::detail::endpoint_impl>(loom::get_default_resource(),
                                                                   &handle,
                                                                   loom::endpoint_types::rdm,
                                                                   loom::address_format::inet)};
};
}  // namespace

TEST_CASE("atomic peer submissions preserve registrations and deferred operands",
          "[atomic][peer]") {
    using namespace loom;
    const auto kind = GENERATE(0, 1, 2, 3);
    const auto typed = GENERATE(false, true);
    const auto peer = GENERATE(fabric_addr{7}, fabric_addr{19});
    const auto status = GENERATE(0, -FI_EAGAIN, -FI_EIO);
    atomic_peer_provider provider;
    provider.status = status;
    std::array<std::uint64_t, 2> operand{3, 4}, comparison{42, 43}, result{};
    fid_mr operand_handle{.fid = {.ops = &provider.lifetime}, .mem_desc = &operand_handle};
    fid_mr compare_handle{.fid = {.ops = &provider.lifetime}, .mem_desc = &compare_handle};
    fid_mr result_handle{.fid = {.ops = &provider.lifetime}, .mem_desc = &result_handle};
    const auto make_mr = [](fid_mr& mr, auto& data) {
        return memory_region{detail::make_pmr_unique<detail::memory_region_impl>(
            get_default_resource(), &mr, data.data(), sizeof(data))};
    };
    auto operand_mr = make_mr(operand_handle, operand);
    auto compare_mr = make_mr(compare_handle, comparison);
    auto result_mr = make_mr(result_handle, result);
    const remote_memory remote{24, 0x123456789abcdef0ULL, sizeof(result)};
    fi_context2 context{};
    const auto count = typed ? 1u : 2u;
    auto submitted = [&]() -> loom::result<void> {
        switch (kind) {
            case 0:
                return typed ? atomic::add_to(
                                   provider.ep, operand[0], &operand_mr, peer, remote, &context)
                             : atomic::execute_to(provider.ep,
                                                  atomic::operation::sum,
                                                  atomic::datatype::uint64,
                                                  operand.data(),
                                                  count,
                                                  &operand_mr,
                                                  peer,
                                                  remote,
                                                  &context);
            case 1:
                return typed ? atomic::fetch_add_to(provider.ep,
                                                    operand[0],
                                                    result.data(),
                                                    &operand_mr,
                                                    &result_mr,
                                                    peer,
                                                    remote,
                                                    &context)
                             : atomic::fetch_to(provider.ep,
                                                atomic::operation::sum,
                                                atomic::datatype::uint64,
                                                operand.data(),
                                                result.data(),
                                                count,
                                                &operand_mr,
                                                &result_mr,
                                                peer,
                                                remote,
                                                &context);
            case 2:
                return typed ? atomic::cas_to(provider.ep,
                                              comparison[0],
                                              operand[0],
                                              result.data(),
                                              &compare_mr,
                                              &operand_mr,
                                              &result_mr,
                                              peer,
                                              remote,
                                              &context)
                             : atomic::compare_swap_to(provider.ep,
                                                       atomic::datatype::uint64,
                                                       comparison.data(),
                                                       operand.data(),
                                                       result.data(),
                                                       count,
                                                       &compare_mr,
                                                       &operand_mr,
                                                       &result_mr,
                                                       peer,
                                                       remote,
                                                       &context);
            default:
                return atomic::inject_to(provider.ep,
                                         atomic::operation::sum,
                                         atomic::datatype::uint64,
                                         operand.data(),
                                         count,
                                         peer,
                                         remote);
        }
    }();
    REQUIRE(static_cast<bool>(submitted) == (status == 0));
    if (status != 0)
        REQUIRE(submitted.error() == make_error_code_from_fi_errno(-status));
    REQUIRE(provider.calls == 1);
    REQUIRE(provider.peer == peer.get());
    REQUIRE(provider.count == count);
    REQUIRE(provider.input == operand.data());
    REQUIRE(provider.input_desc == (kind == 3 ? nullptr : &operand_handle));
    REQUIRE(provider.comparison == (kind == 2 ? comparison.data() : nullptr));
    REQUIRE(provider.compare_desc == (kind == 2 ? &compare_handle : nullptr));
    REQUIRE(provider.output == (kind == 1 || kind == 2 ? result.data() : nullptr));
    REQUIRE(provider.output_desc == (kind == 1 || kind == 2 ? &result_handle : nullptr));
    REQUIRE(provider.context == (kind == 3 ? nullptr : &context));
    REQUIRE(provider.operation == (kind == 2 ? FI_CSWAP : FI_SUM));
    if (status == 0 && (kind == 1 || kind == 2)) {
        // The provider first touches borrowed operands after the wrapper returns.
        auto* output = static_cast<std::uint64_t*>(provider.output);
        const auto* input = static_cast<const std::uint64_t*>(provider.input);
        for (std::size_t i = 0; i < count; ++i) {
            if (kind == 2)
                REQUIRE(static_cast<const std::uint64_t*>(provider.comparison)[i] == 42 + i);
            output[i] = input[i] + 42;
            REQUIRE(result[i] == 45 + i);
        }
    }
}

TEST_CASE("atomic peer submissions reject unavailable endpoints and operations", "[atomic][peer]") {
    using namespace loom;
    const auto missing = GENERATE(0, 1, 2, 3);
    atomic_peer_provider provider;
    if (missing == 0)
        provider.ep = {};
    else if (missing == 1)
        provider.handle.atomic = nullptr;
    else if (missing == 2)
        provider.operations = {.size = sizeof(fi_ops_atomic)};
    else
        provider.operations.size = sizeof(std::size_t);
    std::uint64_t input{}, output{}, comparison{};
    const auto type = atomic::datatype::uint64;
    const auto op = atomic::operation::sum;
    const fabric_addr peer{7};
    const remote_memory remote{24, 0x123456789abcdef0ULL, sizeof(input)};
    const std::array results{
        atomic::execute_to(provider.ep, op, type, &input, 1, nullptr, peer, remote),
        atomic::fetch_to(provider.ep, op, type, &input, &output, 1, nullptr, nullptr, peer, remote),
        atomic::compare_swap_to(provider.ep,
                                type,
                                &comparison,
                                &input,
                                &output,
                                1,
                                nullptr,
                                nullptr,
                                nullptr,
                                peer,
                                remote),
        atomic::inject_to(provider.ep, op, type, &input, 1, peer, remote),
    };
    for (const auto& result : results) {
        REQUIRE_FALSE(result);
        REQUIRE(result.error() == (missing == 0 ? errc::invalid_argument : errc::not_supported));
    }
    REQUIRE(provider.calls == 0);
}

TEST_CASE("registered RDM atomics select the destination peer", "[atomic][peer][sockets]") {
    using namespace loom;
    fabric_hints hints{};
    hints.provider_name = "sockets";
    hints.ep_type = endpoint_types::rdm;
    hints.capabilities = capability::msg | capability::atomic;
    auto info = query_fabric(hints);
    if (!info)
        SKIP("Sockets provider unavailable");
    auto fabric = fabric::create(*info);
    REQUIRE(fabric);
    auto dom = domain::create(*fabric, *info);
    REQUIRE(dom);
    auto cq = completion_queue::create(*dom);
    REQUIRE(cq);
    auto av = address_vector::create(*dom);
    REQUIRE(av);
    auto source = endpoint::create(*dom, *info);
    auto target = endpoint::create(*dom, *info);
    REQUIRE(source);
    REQUIRE(target);
    for (auto* ep : {&*source, &*target}) {
        REQUIRE(ep->bind_cq(*cq, cq_bind::transmit | cq_bind::recv));
        REQUIRE(ep->bind_av(*av));
        REQUIRE(ep->enable());
    }
    auto target_address = target->get_local_address();
    REQUIRE(target_address);
    auto peer = av->insert(*target_address);
    REQUIRE(peer);
    const fabric_addr destination{peer->value};
    std::uint64_t input{3}, comparison{45}, output{}, target_value{42};
    const auto register_value = [&](std::uint64_t& value) {
        return memory_region::register_memory(*dom,
                                              std::as_writable_bytes(std::span{&value, 1}),
                                              mr_access_flags::read | mr_access_flags::write |
                                                  mr_access_flags::remote_read |
                                                  mr_access_flags::remote_write);
    };
    auto input_mr = register_value(input);
    auto comparison_mr = register_value(comparison);
    auto output_mr = register_value(output);
    auto target_mr = register_value(target_value);
    REQUIRE(input_mr);
    REQUIRE(comparison_mr);
    REQUIRE(output_mr);
    REQUIRE(target_mr);
    const auto remote = remote_memory::from_mr(*target_mr);
    std::size_t limit{};
    REQUIRE(
        atomic::is_fetch_valid(*source, atomic::operation::sum, atomic::datatype::uint64, &limit));
    REQUIRE(limit >= 1);
    REQUIRE(atomic::is_compare_valid(
        *source, atomic::operation::compare_swap, atomic::datatype::uint64, &limit));
    REQUIRE(limit >= 1);
    for (bool compare : {false, true}) {
        fi_context2 context{};
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{3};
        result<void> submitted;
        do {
            submitted = compare ? atomic::cas_to(*source,
                                                 comparison,
                                                 input,
                                                 &output,
                                                 &*comparison_mr,
                                                 &*input_mr,
                                                 &*output_mr,
                                                 destination,
                                                 remote,
                                                 &context)
                                : atomic::fetch_add_to(*source,
                                                       input,
                                                       &output,
                                                       &*input_mr,
                                                       &*output_mr,
                                                       destination,
                                                       remote,
                                                       &context);
            if (submitted || submitted.error() != errc::again)
                break;
            REQUIRE(cq->progress());
            std::this_thread::yield();
        } while (std::chrono::steady_clock::now() < deadline);
        REQUIRE(submitted);
        std::optional<completion_event> completion;
        do {
            completion = cq->poll();
            if (completion)
                break;
            std::this_thread::yield();
        } while (std::chrono::steady_clock::now() < deadline);
        REQUIRE(completion);
        REQUIRE_FALSE(completion->error);
        REQUIRE(completion->context.get() == &context);
        REQUIRE(output == (compare ? 45 : 42));
    }
    REQUIRE(target_value == 3);
}
