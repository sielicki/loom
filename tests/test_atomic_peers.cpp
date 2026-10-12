// SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only
#include <loom/async/completion_queue.hpp>
#include <loom/detail/impl/endpoint_impl.hpp>
#include <loom/detail/impl/memory_impl.hpp>
#include <loom/loom.hpp>

#include <array>
#include <chrono>
#include <limits>
#include <thread>
#include <utility>

#include <catch2/catch_template_test_macros.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

namespace {
struct atomic_peer_provider {
    fid_domain domain_handle{};
    int calls{};
    ssize_t status{};
    std::uint64_t flags{};
    fi_addr_t peer{};
    const void* input{};
    const void* comparison{};
    void* output{};
    void* input_desc{};
    void* compare_desc{};
    void* output_desc{};
    void* context{};
    std::size_t count{};
    std::uint64_t address{24};
    fi_op operation{};
    fi_datatype datatype{FI_UINT64};
    fi_datatype queried_datatype{};

    static auto query(fid_ep* ep, fi_datatype type, fi_op op, std::size_t* count) -> int {
        auto& self = *static_cast<atomic_peer_provider*>(ep->fid.context);
        ++self.calls;
        self.queried_datatype = type;
        self.operation = op;
        *count = 16;
        return 0;
    }

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
        REQUIRE(address == self.address);
        REQUIRE(key == 0x123456789abcdef0ULL);
        REQUIRE(datatype == self.datatype);
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

    static auto record_message(fid_ep* ep,
                               const fi_msg_atomic* message,
                               const fi_ioc* compare,
                               void** compare_desc,
                               fi_ioc* output,
                               void** output_desc,
                               std::uint64_t flags) -> ssize_t {
        static_cast<atomic_peer_provider*>(ep->fid.context)->flags = flags;
        REQUIRE(message->iov_count == 1);
        REQUIRE(message->rma_iov_count == 1);
        const auto count = message->msg_iov[0].count;
        REQUIRE(message->rma_iov[0].count == count);
        if (compare)
            REQUIRE(compare->count == count);
        if (output)
            REQUIRE(output->count == count);
        return record(ep,
                      message->msg_iov[0].addr,
                      count,
                      message->desc ? message->desc[0] : nullptr,
                      compare ? compare->addr : nullptr,
                      compare_desc ? compare_desc[0] : nullptr,
                      output ? output->addr : nullptr,
                      output_desc ? output_desc[0] : nullptr,
                      message->addr,
                      message->rma_iov[0].addr,
                      message->rma_iov[0].key,
                      message->datatype,
                      message->op,
                      message->context);
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
        .writemsg =
            [](fid_ep* ep, const fi_msg_atomic* message, std::uint64_t flags) {
                return record_message(ep, message, nullptr, nullptr, nullptr, nullptr, flags);
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
        .readwritemsg =
            [](fid_ep* ep,
               const fi_msg_atomic* message,
               fi_ioc* output,
               void** output_desc,
               std::size_t count,
               std::uint64_t flags) {
                REQUIRE(count == 1);
                return record_message(ep, message, nullptr, nullptr, output, output_desc, flags);
            },
        .compwrite = record,
        .compwritemsg =
            [](fid_ep* ep,
               const fi_msg_atomic* message,
               const fi_ioc* compare,
               void** compare_desc,
               std::size_t compare_count,
               fi_ioc* output,
               void** output_desc,
               std::size_t count,
               std::uint64_t flags) {
                REQUIRE(compare_count == 1);
                REQUIRE(count == 1);
                return record_message(
                    ep, message, compare, compare_desc, output, output_desc, flags);
            },
        .writevalid = query,
        .readwritevalid = query,
        .compwritevalid = query,
    };
    fid_ep handle{.fid = {.context = this, .ops = &lifetime}, .atomic = &operations};
    loom::endpoint ep{
        loom::detail::make_pmr_unique<loom::detail::endpoint_impl>(loom::get_default_resource(),
                                                                   &handle,
                                                                   loom::endpoint_types::rdm,
                                                                   loom::address_format::inet,
                                                                   0UZ,
                                                                   0UZ,
                                                                   loom::caps{},
                                                                   FI_FENCE | FI_DELIVERY_COMPLETE,
                                                                   0,
                                                                   &domain_handle)};
};

struct atomic_operation_case {
    loom::atomic::operation value;
    fi_op native;
    bool base;
    bool fetch;
    bool compare;
};

using enum loom::atomic::operation;
constexpr std::array atomic_operations{
    atomic_operation_case{min, FI_MIN, true, true, false},
    atomic_operation_case{max, FI_MAX, true, true, false},
    atomic_operation_case{sum, FI_SUM, true, true, false},
    atomic_operation_case{prod, FI_PROD, true, true, false},
    atomic_operation_case{logical_or, FI_LOR, true, true, false},
    atomic_operation_case{logical_and, FI_LAND, true, true, false},
    atomic_operation_case{bitwise_or, FI_BOR, true, true, false},
    atomic_operation_case{bitwise_and, FI_BAND, true, true, false},
    atomic_operation_case{logical_xor, FI_LXOR, true, true, false},
    atomic_operation_case{bitwise_xor, FI_BXOR, true, true, false},
    atomic_operation_case{atomic_read, FI_ATOMIC_READ, false, true, false},
    atomic_operation_case{atomic_write, FI_ATOMIC_WRITE, true, true, false},
    atomic_operation_case{compare_swap, FI_CSWAP, false, false, true},
    atomic_operation_case{compare_swap_ne, FI_CSWAP_NE, false, false, true},
    atomic_operation_case{compare_swap_le, FI_CSWAP_LE, false, false, true},
    atomic_operation_case{compare_swap_lt, FI_CSWAP_LT, false, false, true},
    atomic_operation_case{compare_swap_ge, FI_CSWAP_GE, false, false, true},
    atomic_operation_case{compare_swap_gt, FI_CSWAP_GT, false, false, true},
    atomic_operation_case{masked_swap, FI_MSWAP, false, false, true},
    atomic_operation_case{static_cast<loom::atomic::operation>(-1), FI_MIN, false, false, false},
    atomic_operation_case{static_cast<loom::atomic::operation>(999), FI_MIN, false, false, false},
};
}  // namespace

TEST_CASE("atomic submissions reject invalid operation families", "[atomic][validation]") {
    using namespace loom;
    enum class submission { base, base_message, injected, fetch, fetch_message };
    const auto path = GENERATE(submission::base,
                               submission::base_message,
                               submission::injected,
                               submission::fetch,
                               submission::fetch_message);
    for (const auto& op : atomic_operations) {
        CAPTURE(path, op.value);
        atomic_peer_provider provider;
        provider.status = -FI_EAGAIN;
        const std::uint64_t input{42};
        std::uint64_t output{};
        const fabric_addr peer{7};
        const remote_memory remote{24, 0x123456789abcdef0ULL, sizeof(input)};
        const auto result = [&]() -> loom::result<void> {
            switch (path) {
                case submission::base:
                    return atomic::execute_to(provider.ep,
                                              op.value,
                                              atomic::datatype::uint64,
                                              &input,
                                              1,
                                              nullptr,
                                              peer,
                                              remote);
                case submission::base_message:
                    return atomic::execute_to(provider.ep,
                                              op.value,
                                              atomic::datatype::uint64,
                                              &input,
                                              1,
                                              nullptr,
                                              peer,
                                              remote,
                                              op_flag::completion);
                case submission::injected:
                    return atomic::inject_to(
                        provider.ep, op.value, atomic::datatype::uint64, &input, 1, peer, remote);
                case submission::fetch:
                    return atomic::fetch_to(provider.ep,
                                            op.value,
                                            atomic::datatype::uint64,
                                            &input,
                                            &output,
                                            1,
                                            nullptr,
                                            nullptr,
                                            peer,
                                            remote);
                case submission::fetch_message:
                    return atomic::fetch_to(provider.ep,
                                            op.value,
                                            atomic::datatype::uint64,
                                            &input,
                                            &output,
                                            1,
                                            nullptr,
                                            nullptr,
                                            peer,
                                            remote,
                                            op_flag::completion);
            }
            std::unreachable();
        }();
        const auto accepted =
            path == submission::fetch || path == submission::fetch_message ? op.fetch : op.base;
        REQUIRE_FALSE(result);
        CHECK(result.error() == (accepted ? errc::again : errc::invalid_argument));
        CHECK(provider.calls == (accepted ? 1 : 0));
        if (accepted)
            CHECK(provider.operation == op.native);
    }
}

TEST_CASE("atomic queries reject invalid enums and operation families", "[atomic][validation]") {
    using namespace loom;
    const auto family = GENERATE(0UZ, 1UZ, 2UZ);
    const auto datatype = GENERATE(atomic::datatype::uint64,
                                   static_cast<atomic::datatype>(-1),
                                   static_cast<atomic::datatype>(999));
    constexpr std::array queries{
        &atomic::is_valid, &atomic::is_fetch_valid, &atomic::is_compare_valid};
    for (const auto& op : atomic_operations) {
        CAPTURE(family, datatype, op.value);
        atomic_peer_provider provider;
        const std::array families{op.base, op.fetch, op.compare};
        const auto accepted = datatype == atomic::datatype::uint64 && families[family];
        std::size_t count{99};
        CHECK(queries[family](provider.ep, op.value, datatype, &count) == accepted);
        CHECK(provider.calls == (accepted ? 1 : 0));
        CHECK(count == (accepted ? 16 : 99));
        if (accepted) {
            CHECK(provider.operation == op.native);
            CHECK(provider.queried_datatype == FI_UINT64);
        }
    }
}

TEST_CASE("atomic queries respect native operation table size", "[atomic][validation]") {
    using namespace loom;
    atomic_peer_provider provider;
    provider.operations.size = offsetof(fi_ops_atomic, writevalid);
    std::size_t count{99};
    CHECK_FALSE(
        atomic::is_valid(provider.ep, atomic::operation::sum, atomic::datatype::uint64, &count));
    CHECK_FALSE(atomic::is_fetch_valid(
        provider.ep, atomic::operation::sum, atomic::datatype::uint64, &count));
    CHECK_FALSE(atomic::is_compare_valid(
        provider.ep, atomic::operation::compare_swap, atomic::datatype::uint64, &count));
    CHECK(provider.calls == 0);
    CHECK(count == 99);
}

TEST_CASE("atomics reject incompatible or out-of-range registrations", "[atomic][registration]") {
    enum class defect { none, foreign, unknown, disabled, wrong_endpoint, short_range, unrelated };
    const auto bad = GENERATE(defect::none,
                              defect::foreign,
                              defect::unknown,
                              defect::disabled,
                              defect::wrong_endpoint,
                              defect::short_range,
                              defect::unrelated);
    const auto operand = GENERATE(0, 1, 2);
    const auto flagged = GENERATE(false, true);
    const auto count = GENERATE(1UZ, 2UZ);
    atomic_peer_provider native;
    native.status = -FI_EAGAIN;
    fid_domain foreign{};
    fid_ep other_endpoint{};
    std::array<std::array<std::uint64_t, 2>, 3> values{};
    std::array<fid_mr, 3> handles{};
    std::array<loom::memory_region, 3> regions{};
    std::array<std::uint64_t, 2> unrelated{};
    for (std::size_t i = 0; i < regions.size(); ++i) {
        handles[i] = {.fid = {.ops = &native.lifetime}, .mem_desc = &handles[i]};
        auto impl = loom::detail::make_pmr_unique<loom::detail::memory_region_impl>(
            loom::get_default_resource(),
            &handles[i],
            i == operand && bad == defect::unrelated ? unrelated.data() : values[i].data(),
            i == operand && bad == defect::short_range ? count * sizeof(std::uint64_t) - 1
                                                       : sizeof(values[i]),
            loom::detail::mr_key_lease{},
            0,
            i == operand && bad == defect::disabled ? FI_MR_ENDPOINT : 0,
            i == operand && bad == defect::foreign   ? &foreign
            : i == operand && bad == defect::unknown ? nullptr
                                                     : &native.domain_handle);
        if (i == operand && bad == defect::wrong_endpoint)
            impl->bound_endpoint = &other_endpoint;
        regions[i] = loom::memory_region{std::move(impl)};
    }
    using namespace loom;
    const remote_memory remote{24, 0x123456789abcdef0ULL, count * sizeof(std::uint64_t)};
    const fabric_addr peer{19};
    const auto type = atomic::datatype::uint64;
    const auto flags = op_flag::completion;
    for (int operation = 0; operation != 3; ++operation) {
        native.calls = 0;
        const auto result = [&] {
            if (operation == 0)
                return flagged ? atomic::execute_to(native.ep,
                                                    atomic::operation::sum,
                                                    type,
                                                    values[0].data(),
                                                    count,
                                                    &regions[0],
                                                    peer,
                                                    remote,
                                                    flags)
                               : atomic::execute_to(native.ep,
                                                    atomic::operation::sum,
                                                    type,
                                                    values[0].data(),
                                                    count,
                                                    &regions[0],
                                                    peer,
                                                    remote);
            if (operation == 1)
                return flagged ? atomic::fetch_to(native.ep,
                                                  atomic::operation::sum,
                                                  type,
                                                  values[0].data(),
                                                  values[2].data(),
                                                  count,
                                                  &regions[0],
                                                  &regions[2],
                                                  peer,
                                                  remote,
                                                  flags)
                               : atomic::fetch_to(native.ep,
                                                  atomic::operation::sum,
                                                  type,
                                                  values[0].data(),
                                                  values[2].data(),
                                                  count,
                                                  &regions[0],
                                                  &regions[2],
                                                  peer,
                                                  remote);
            return flagged ? atomic::compare_swap_to(native.ep,
                                                     type,
                                                     values[1].data(),
                                                     values[0].data(),
                                                     values[2].data(),
                                                     count,
                                                     &regions[1],
                                                     &regions[0],
                                                     &regions[2],
                                                     peer,
                                                     remote,
                                                     flags)
                           : atomic::compare_swap_to(native.ep,
                                                     type,
                                                     values[1].data(),
                                                     values[0].data(),
                                                     values[2].data(),
                                                     count,
                                                     &regions[1],
                                                     &regions[0],
                                                     &regions[2],
                                                     peer,
                                                     remote);
        }();
        const bool used = operation == 2 || operand == 0 || (operation == 1 && operand == 2);
        const bool valid = !used || bad == defect::none;
        CHECK(result.error() == (valid ? errc::again : errc::invalid_argument));
        CHECK(native.calls == (valid ? 1 : 0));
    }
    // FI_ATOMIC_READ ignores its input operand, including its descriptor.
    native.calls = 0;
    const auto result = flagged ? atomic::fetch_to(native.ep,
                                                   atomic::operation::atomic_read,
                                                   type,
                                                   nullptr,
                                                   values[2].data(),
                                                   count,
                                                   &regions[0],
                                                   &regions[2],
                                                   peer,
                                                   remote,
                                                   flags)
                                : atomic::fetch_to(native.ep,
                                                   atomic::operation::atomic_read,
                                                   type,
                                                   nullptr,
                                                   values[2].data(),
                                                   count,
                                                   &regions[0],
                                                   &regions[2],
                                                   peer,
                                                   remote);
    const bool valid = operand != 2 || bad == defect::none;
    CHECK(result.error() == (valid ? errc::again : errc::invalid_argument));
    CHECK(native.calls == (valid ? 1 : 0));
}

TEST_CASE("atomic submissions enforce remote element bounds", "[atomic][bounds]") {
    using namespace loom;
    struct element {
        atomic::datatype type;
        fi_datatype native;
        std::size_t bytes;
        bool valid{true};
    };
    constexpr std::array elements{
        element{atomic::datatype::int8, FI_INT8, 1},
        element{atomic::datatype::uint8, FI_UINT8, 1},
        element{atomic::datatype::int16, FI_INT16, 2},
        element{atomic::datatype::uint16, FI_UINT16, 2},
        element{atomic::datatype::int32, FI_INT32, 4},
        element{atomic::datatype::uint32, FI_UINT32, 4},
        element{atomic::datatype::int64, FI_INT64, 8},
        element{atomic::datatype::uint64, FI_UINT64, 8},
        element{atomic::datatype::float32, FI_FLOAT, sizeof(float)},
        element{atomic::datatype::double64, FI_DOUBLE, sizeof(double)},
        element{atomic::datatype::float_complex, FI_FLOAT_COMPLEX, 2 * sizeof(float)},
        element{atomic::datatype::double_complex, FI_DOUBLE_COMPLEX, 2 * sizeof(double)},
        element{atomic::datatype::long_double, FI_LONG_DOUBLE, sizeof(long double)},
        element{
            atomic::datatype::long_double_complex, FI_LONG_DOUBLE_COMPLEX, 2 * sizeof(long double)},
        element{atomic::datatype::int128, FI_INT128, 16},
        element{atomic::datatype::uint128, FI_UINT128, 16},
        element{static_cast<atomic::datatype>(-1), FI_INT8, 1, false},
        element{static_cast<atomic::datatype>(999), FI_INT8, 1, false},
    };
    constexpr auto max_address = std::numeric_limits<std::uint64_t>::max();
    constexpr auto max_count = std::numeric_limits<std::size_t>::max();
    struct transfer {
        std::uint64_t address;
        std::size_t length;
        std::size_t count;
        bool valid;
    };
    for (const auto& element : elements) {
        CAPTURE(element.native);
        const auto size = element.bytes;
        const std::array transfers{
            transfer{24, 0, 0, true},
            transfer{24, 0, 1, false},
            transfer{24, size - 1, 1, false},
            transfer{24, size, 1, true},
            transfer{24, size, 2, false},
            transfer{24, 2 * size, 2, true},
            transfer{max_address - size, size, 1, true},
            transfer{max_address - size + 1, size, 1, false},
            transfer{max_address, 0, 0, true},
            transfer{0, max_count, max_count / size, true},
            transfer{24, max_count, max_count / size + (size > 1), false},
        };
        for (const auto& test : transfers) {
            CAPTURE(test.address, test.length, test.count);
            atomic_peer_provider provider;
            provider.datatype = element.native;
            provider.address = test.address;
            provider.status = -FI_EAGAIN;  // Capture parameters without accessing any buffers.
            std::array<std::byte, 64> input{}, comparison{}, output{};
            const fabric_addr peer{19};
            const remote_memory remote{test.address, 0x123456789abcdef0ULL, test.length};
            const auto type = element.type;
            const auto count = test.count;
            const auto op = atomic::operation::sum;
            const std::array results{
                atomic::execute_to(
                    provider.ep, op, type, input.data(), count, nullptr, peer, remote),
                atomic::execute_to(provider.ep,
                                   op,
                                   type,
                                   input.data(),
                                   count,
                                   nullptr,
                                   peer,
                                   remote,
                                   op_flag::none),
                atomic::fetch_to(provider.ep,
                                 op,
                                 type,
                                 input.data(),
                                 output.data(),
                                 count,
                                 nullptr,
                                 nullptr,
                                 peer,
                                 remote),
                atomic::fetch_to(provider.ep,
                                 op,
                                 type,
                                 input.data(),
                                 output.data(),
                                 count,
                                 nullptr,
                                 nullptr,
                                 peer,
                                 remote,
                                 op_flag::none),
                atomic::compare_swap_to(provider.ep,
                                        type,
                                        comparison.data(),
                                        input.data(),
                                        output.data(),
                                        count,
                                        nullptr,
                                        nullptr,
                                        nullptr,
                                        peer,
                                        remote),
                atomic::compare_swap_to(provider.ep,
                                        type,
                                        comparison.data(),
                                        input.data(),
                                        output.data(),
                                        count,
                                        nullptr,
                                        nullptr,
                                        nullptr,
                                        peer,
                                        remote,
                                        op_flag::none),
                atomic::inject_to(provider.ep, op, type, input.data(), count, peer, remote),
            };
            for (const auto& result : results) {
                REQUIRE_FALSE(result);
                CHECK(result.error() ==
                      (test.valid && element.valid ? errc::again : errc::invalid_argument));
            }
            CHECK(provider.calls == (test.valid && element.valid ? results.size() : 0));
        }
    }
}

TEMPLATE_TEST_CASE("atomic peer submissions preserve registrations and deferred operands",
                   "[atomic][peer]",
                   std::uint64_t,
                   long double) {
    using namespace loom;
    const auto kind = GENERATE(0, 1, 2, 3);
    const auto typed = GENERATE(false, true);
    const auto peer = GENERATE(fabric_addr{7}, fabric_addr{19});
    const auto status = GENERATE(0, -FI_EAGAIN, -FI_EIO);
    atomic_peer_provider provider;
    provider.status = status;
    provider.datatype = std::same_as<TestType, long double> ? FI_LONG_DOUBLE : FI_UINT64;
    constexpr auto datatype = std::same_as<TestType, long double> ? atomic::datatype::long_double
                                                                  : atomic::datatype::uint64;
    std::array<TestType, 2> operand{3, 4}, comparison{42, 43}, result{};
    const auto& input = operand[0];
    const auto& expected = comparison[0];
    fid_mr operand_handle{.fid = {.ops = &provider.lifetime}, .mem_desc = &operand_handle};
    fid_mr compare_handle{.fid = {.ops = &provider.lifetime}, .mem_desc = &compare_handle};
    fid_mr result_handle{.fid = {.ops = &provider.lifetime}, .mem_desc = &result_handle};
    const auto make_mr = [&](fid_mr& mr, auto& data) {
        return memory_region{
            detail::make_pmr_unique<detail::memory_region_impl>(get_default_resource(),
                                                                &mr,
                                                                data.data(),
                                                                sizeof(data),
                                                                loom::detail::mr_key_lease{},
                                                                0,
                                                                0,
                                                                &provider.domain_handle)};
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
                return typed
                           ? atomic::add_to(provider.ep, input, &operand_mr, peer, remote, &context)
                           : atomic::execute_to(provider.ep,
                                                atomic::operation::sum,
                                                datatype,
                                                operand.data(),
                                                count,
                                                &operand_mr,
                                                peer,
                                                remote,
                                                &context);
            case 1:
                return typed ? atomic::fetch_add_to(provider.ep,
                                                    input,
                                                    result.data(),
                                                    &operand_mr,
                                                    &result_mr,
                                                    peer,
                                                    remote,
                                                    &context)
                             : atomic::fetch_to(provider.ep,
                                                atomic::operation::sum,
                                                datatype,
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
                                              expected,
                                              input,
                                              result.data(),
                                              &compare_mr,
                                              &operand_mr,
                                              &result_mr,
                                              peer,
                                              remote,
                                              &context)
                             : atomic::compare_swap_to(provider.ep,
                                                       datatype,
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
                                         datatype,
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
        auto* output = static_cast<TestType*>(provider.output);
        const auto* input = static_cast<const TestType*>(provider.input);
        for (std::size_t i = 0; i < count; ++i) {
            if (kind == 2)
                REQUIRE(static_cast<const TestType*>(provider.comparison)[i] == 42 + i);
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
        atomic::execute_to(
            provider.ep, op, type, &input, 1, nullptr, peer, remote, op_flag::completion),
        atomic::fetch_to(provider.ep,
                         op,
                         type,
                         &input,
                         &output,
                         1,
                         nullptr,
                         nullptr,
                         peer,
                         remote,
                         op_flag::completion),
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
                                remote,
                                op_flag::completion),
    };
    for (const auto& result : results) {
        REQUIRE_FALSE(result);
        REQUIRE(result.error() == (missing == 0 ? errc::invalid_argument : errc::not_supported));
    }
    REQUIRE(provider.calls == 0);
}

TEST_CASE("registered RDM atomics select the destination peer", "[atomic][peer][sockets]") {
    using namespace loom;
    enum class submission { scalar, message, injected };
    const auto mode = GENERATE(submission::scalar, submission::message, submission::injected);
    const bool selective = mode != submission::scalar;
    const auto flags =
        op_flag::completion | (mode == submission::injected ? op_flag::inject : op_flag::none);
    fabric_hints hints{};
    hints.provider_name = "sockets";
    hints.ep_type = endpoint_types::rdm;
    hints.capabilities = capability::msg | capability::atomic;
    auto info = query_fabric(hints);
    if (!info)
        SKIP("Sockets provider unavailable");
    auto* native_info = static_cast<fi_info*>(info->internal_ptr());
    native_info->tx_attr->op_flags = FI_DELIVERY_COMPLETE;
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
        REQUIRE(ep->bind_cq(*cq,
                            cq_bind::transmit | cq_bind::recv |
                                (selective ? cq_bind::selective_completion : cq_bind_flags{})));
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
    const auto remote = remote_memory::from_mr(*target_mr).value();
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
            if (selective) {
                submitted =
                    compare
                        ? atomic::compare_swap_to<operation_defaults::inherit>(
                              *source,
                              atomic::datatype::uint64,
                              &comparison,
                              &input,
                              &output,
                              1,
                              &*comparison_mr,
                              &*input_mr,
                              &*output_mr,
                              destination,
                              remote,
                              flags,
                              &context)
                        : atomic::fetch_to<operation_defaults::inherit>(*source,
                                                                        atomic::operation::sum,
                                                                        atomic::datatype::uint64,
                                                                        &input,
                                                                        &output,
                                                                        1,
                                                                        &*input_mr,
                                                                        &*output_mr,
                                                                        destination,
                                                                        remote,
                                                                        flags,
                                                                        &context);
            } else {
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
            }
            if (submitted || submitted.error() != errc::again)
                break;
            REQUIRE(cq->progress());
            std::this_thread::yield();
        } while (std::chrono::steady_clock::now() < deadline);
        REQUIRE(submitted);
        if (mode == submission::injected) {
            // Constant inputs may be reused, but output must survive until completion.
            input = 0;
            comparison = 0;
        }
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
        input = 3;
        comparison = 45;
    }
    REQUIRE(target_value == 3);
}

TEST_CASE("atomic flags preserve peers counts and independent registrations", "[atomic][flags]") {
    using namespace loom;
    enum class kind { update, fetch, compare };
    const auto operation = GENERATE(kind::update, kind::fetch, kind::compare);
    const auto inherit = GENERATE(false, true);
    const auto count = GENERATE(1UZ, 2UZ);
    const auto status = GENERATE(0, -FI_EAGAIN, -FI_EIO);
    atomic_peer_provider native;
    native.status = status;
    std::array<std::uint64_t, 2> input{3, 4}, compare{42, 43}, output{};
    fid_mr input_handle{.fid = {.ops = &native.lifetime}, .mem_desc = &input_handle};
    fid_mr compare_handle{.fid = {.ops = &native.lifetime}, .mem_desc = &compare_handle};
    fid_mr output_handle{.fid = {.ops = &native.lifetime}, .mem_desc = &output_handle};
    const auto make_mr = [&](fid_mr& handle, auto& storage) {
        return memory_region{
            detail::make_pmr_unique<detail::memory_region_impl>(get_default_resource(),
                                                                &handle,
                                                                storage.data(),
                                                                sizeof(storage),
                                                                loom::detail::mr_key_lease{},
                                                                0,
                                                                0,
                                                                &native.domain_handle)};
    };
    auto input_mr = make_mr(input_handle, input);
    auto compare_mr = make_mr(compare_handle, compare);
    auto output_mr = make_mr(output_handle, output);
    const fabric_addr peer{19};
    const remote_memory remote{24, 0x123456789abcdef0ULL, sizeof(output)};
    fi_context2 context{};
    const auto flags = op_flag::completion | op_flag::inject;
    auto submit = [&]<operation_defaults Defaults>() -> void_result {
        switch (operation) {
            case kind::update:
                return atomic::execute_to<Defaults>(native.ep,
                                                    atomic::operation::sum,
                                                    atomic::datatype::uint64,
                                                    input.data(),
                                                    count,
                                                    &input_mr,
                                                    peer,
                                                    remote,
                                                    flags,
                                                    &context);
            case kind::fetch:
                return atomic::fetch_to<Defaults>(native.ep,
                                                  atomic::operation::sum,
                                                  atomic::datatype::uint64,
                                                  input.data(),
                                                  output.data(),
                                                  count,
                                                  &input_mr,
                                                  &output_mr,
                                                  peer,
                                                  remote,
                                                  flags,
                                                  &context);
            case kind::compare:
                return atomic::compare_swap_to<Defaults>(native.ep,
                                                         atomic::datatype::uint64,
                                                         compare.data(),
                                                         input.data(),
                                                         output.data(),
                                                         count,
                                                         &compare_mr,
                                                         &input_mr,
                                                         &output_mr,
                                                         peer,
                                                         remote,
                                                         flags,
                                                         &context);
        }
        std::unreachable();
    };
    const auto result = inherit ? submit.template operator()<operation_defaults::inherit>()
                                : submit.template operator()<operation_defaults::replace>();
    REQUIRE(native.calls == 1);
    REQUIRE(native.flags ==
            (FI_COMPLETION | FI_INJECT | (inherit ? FI_FENCE | FI_DELIVERY_COMPLETE : 0)));
    REQUIRE(native.peer == peer.get());
    REQUIRE(native.count == count);
    REQUIRE(native.input == input.data());
    REQUIRE(native.input_desc == &input_handle);
    REQUIRE(native.comparison == (operation == kind::compare ? compare.data() : nullptr));
    REQUIRE(native.compare_desc == (operation == kind::compare ? &compare_handle : nullptr));
    REQUIRE(native.output == (operation == kind::update ? nullptr : output.data()));
    REQUIRE(native.output_desc == (operation == kind::update ? nullptr : &output_handle));
    REQUIRE(native.context == &context);
    REQUIRE(native.operation == (operation == kind::compare ? FI_CSWAP : FI_SUM));
    if (status)
        REQUIRE(result.error() == make_error_code_from_fi_errno(-status));
    else
        REQUIRE(result);
}
