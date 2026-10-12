// SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only
#include <loom/core/collective.hpp>
#include <loom/detail/impl/memory_impl.hpp>

#include <array>
#include <complex>
#include <limits>
#include <string>
#include <utility>

#include "mock_collective.hpp"
#include <catch2/catch_template_test_macros.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

namespace {
template <typename T>
concept can_broadcast =
    requires(loom::endpoint& ep, loom::collective::group& group, std::span<T> data) {
        loom::collective::broadcast(ep, group, data, nullptr, loom::fabric_addr{1});
    };

template <typename T>
concept can_all_reduce = requires(loom::endpoint& ep,
                                  loom::collective::group& group,
                                  std::span<const T> input,
                                  std::span<T> output) {
    loom::collective::all_reduce(
        ep, group, input, output, nullptr, nullptr, loom::atomic::operation::sum);
};
}  // namespace

TEST_CASE("collective overloads reject unmapped element types", "[collective][types]") {
    REQUIRE_FALSE(can_broadcast<bool>);
    REQUIRE_FALSE(can_broadcast<char>);
    REQUIRE_FALSE(can_broadcast<std::string>);
    REQUIRE_FALSE(can_broadcast<const std::uint32_t>);
    REQUIRE_FALSE(can_broadcast<volatile std::uint32_t>);
    REQUIRE_FALSE(can_all_reduce<bool>);
    REQUIRE_FALSE(can_all_reduce<char>);
    REQUIRE_FALSE(can_all_reduce<std::string>);
}

TEST_CASE("collective overloads accept mapped element types", "[collective][types]") {
    REQUIRE(can_broadcast<std::uint8_t>);
    REQUIRE(can_broadcast<std::int32_t>);
    REQUIRE(can_all_reduce<std::uint64_t>);
    REQUIRE(can_all_reduce<double>);
    REQUIRE(can_all_reduce<long double>);
    REQUIRE(can_all_reduce<std::complex<float>>);
    REQUIRE(can_all_reduce<std::complex<double>>);
    REQUIRE(can_all_reduce<std::complex<long double>>);
}

namespace {
using operation = loom::collective::operation;
using datatype = loom::atomic::datatype;

struct transfer_provider : loom::test::collective_provider {
    int calls{};
    ssize_t status{};
    operation last_operation{};
    fi_op reduction{};
    std::size_t count{};
    fi_datatype type{};
    const void* input{};
    void* output{};
    void* input_descriptor{};
    void* output_descriptor{};
    void* transfer_context{};
    fi_addr_t address{};

    static auto record(fid_ep* ep,
                       operation op,
                       const void* input,
                       void* output,
                       std::size_t count,
                       fi_datatype type,
                       fi_addr_t address,
                       void* context,
                       void* input_descriptor = nullptr,
                       void* output_descriptor = nullptr,
                       fi_op reduction = FI_NOOP) -> ssize_t {
        auto& self =
            static_cast<transfer_provider&>(*static_cast<collective_provider*>(ep->fid.context));
        ++self.calls;
        self.last_operation = op;
        self.reduction = reduction;
        self.input = input;
        self.output = output;
        self.input_descriptor = input_descriptor;
        self.output_descriptor = output_descriptor;
        self.count = count;
        self.type = type;
        self.address = address;
        self.transfer_context = context;
        return self.status;
    }

    fi_ops_collective transfers{
        .size = sizeof(fi_ops_collective),
        .barrier =
            [](fid_ep* ep, fi_addr_t address, void* context) {
                return record(
                    ep, operation::barrier, nullptr, nullptr, 0, FI_UINT8, address, context);
            },
        .broadcast =
            [](fid_ep* ep,
               void* input,
               std::size_t count,
               void* input_descriptor,
               fi_addr_t address,
               fi_addr_t,
               fi_datatype type,
               std::uint64_t,
               void* context) {
                return record(ep,
                              operation::broadcast,
                              input,
                              nullptr,
                              count,
                              type,
                              address,
                              context,
                              input_descriptor);
            },
        .alltoall =
            [](fid_ep* ep,
               const void* input,
               std::size_t count,
               void* input_descriptor,
               void* output,
               void* output_descriptor,
               fi_addr_t address,
               fi_datatype type,
               std::uint64_t,
               void* context) {
                return record(ep,
                              operation::all_to_all,
                              input,
                              output,
                              count,
                              type,
                              address,
                              context,
                              input_descriptor,
                              output_descriptor);
            },
        .allreduce =
            [](fid_ep* ep,
               const void* input,
               std::size_t count,
               void* input_descriptor,
               void* output,
               void* output_descriptor,
               fi_addr_t address,
               fi_datatype type,
               fi_op reduction,
               std::uint64_t,
               void* context) {
                return record(ep,
                              operation::all_reduce,
                              input,
                              output,
                              count,
                              type,
                              address,
                              context,
                              input_descriptor,
                              output_descriptor,
                              reduction);
            },
        .allgather =
            [](fid_ep* ep,
               const void* input,
               std::size_t count,
               void* input_descriptor,
               void* output,
               void* output_descriptor,
               fi_addr_t address,
               fi_datatype type,
               std::uint64_t,
               void* context) {
                return record(ep,
                              operation::all_gather,
                              input,
                              output,
                              count,
                              type,
                              address,
                              context,
                              input_descriptor,
                              output_descriptor);
            },
        .reduce_scatter =
            [](fid_ep* ep,
               const void* input,
               std::size_t count,
               void* input_descriptor,
               void* output,
               void* output_descriptor,
               fi_addr_t address,
               fi_datatype type,
               fi_op reduction,
               std::uint64_t,
               void* context) {
                return record(ep,
                              operation::reduce_scatter,
                              input,
                              output,
                              count,
                              type,
                              address,
                              context,
                              input_descriptor,
                              output_descriptor,
                              reduction);
            },
        .reduce =
            [](fid_ep* ep,
               const void* input,
               std::size_t count,
               void* input_descriptor,
               void* output,
               void* output_descriptor,
               fi_addr_t address,
               fi_addr_t,
               fi_datatype type,
               fi_op reduction,
               std::uint64_t,
               void* context) {
                return record(ep,
                              operation::reduce,
                              input,
                              output,
                              count,
                              type,
                              address,
                              context,
                              input_descriptor,
                              output_descriptor,
                              reduction);
            },
        .scatter =
            [](fid_ep* ep,
               const void* input,
               std::size_t count,
               void* input_descriptor,
               void* output,
               void* output_descriptor,
               fi_addr_t address,
               fi_addr_t,
               fi_datatype type,
               std::uint64_t,
               void* context) {
                return record(ep,
                              operation::scatter,
                              input,
                              output,
                              count,
                              type,
                              address,
                              context,
                              input_descriptor,
                              output_descriptor);
            },
        .gather =
            [](fid_ep* ep,
               const void* input,
               std::size_t count,
               void* input_descriptor,
               void* output,
               void* output_descriptor,
               fi_addr_t address,
               fi_addr_t,
               fi_datatype type,
               std::uint64_t,
               void* context) {
                return record(ep,
                              operation::gather,
                              input,
                              output,
                              count,
                              type,
                              address,
                              context,
                              input_descriptor,
                              output_descriptor);
            },
        .barrier2 =
            [](fid_ep* ep, fi_addr_t address, std::uint64_t, void* context) {
                return record(
                    ep, operation::barrier, nullptr, nullptr, 0, FI_UINT8, address, context);
            },
    };

    transfer_provider() { native_ep.collective = &transfers; }

    auto join(const loom::collective::group_attr& attr = {}) -> loom::collective::group {
        const std::array<loom::av_handle, 3> members{{{5}, {2}, {9}}};
        const auto opaque = !attr.communication_key.empty() || (attr.set_flags & FI_UNIVERSE);
        auto result = loom::collective::group::join(
            ep, av, opaque ? std::span<const loom::av_handle>{} : members, attr);
        REQUIRE(result);
        return std::move(*result);
    }
};

auto submit(operation op,
            transfer_provider& provider,
            loom::collective::group& group,
            std::span<std::byte> input,
            std::span<std::byte> output,
            datatype type,
            void* context = nullptr,
            loom::endpoint* target = nullptr,
            loom::memory_region* input_mr = nullptr,
            loom::memory_region* output_mr = nullptr) -> loom::result<void> {
    namespace coll = loom::collective;
    const auto root = loom::fabric_addr{5};
    const auto sum = loom::atomic::operation::sum;
    auto& ep = target ? *target : provider.ep;
    switch (op) {
        case operation::broadcast:
            return coll::broadcast(ep, group, input, input_mr, root, type, context);
        case operation::all_reduce:
            return coll::all_reduce(
                ep, group, input, output, input_mr, output_mr, sum, type, context);
        case operation::all_gather:
            return coll::all_gather(ep, group, input, output, input_mr, output_mr, type, context);
        case operation::all_to_all:
            return coll::all_to_all(ep, group, input, output, input_mr, output_mr, type, context);
        case operation::reduce:
            return coll::reduce(
                ep, group, input, output, input_mr, output_mr, root, sum, type, context);
        case operation::scatter:
            return coll::scatter(
                ep, group, input, output, input_mr, output_mr, root, type, context);
        case operation::gather:
            return coll::gather(ep, group, input, output, input_mr, output_mr, root, type, context);
        case operation::reduce_scatter:
            return coll::reduce_scatter(
                ep, group, input, output, input_mr, output_mr, sum, type, context);
        default:
            return loom::make_error_result<void>(loom::errc::invalid_argument);
    }
}

struct registered_buffer {
    fi_ops operations{
        .size = sizeof(fi_ops),
        .close = [](fid*) { return 0; },
        .bind = [](fid*, fid*, std::uint64_t) { return 0; },
        .control =
            [](fid*, int command, void*) {
                REQUIRE(command == FI_ENABLE);
                return 0;
            },
    };
    fid_mr handle{.fid = {.ops = &operations}, .mem_desc = &handle};
    loom::memory_region region;

    registered_buffer(std::span<std::byte> bytes, fid_domain* domain, int mode = 0)
        : region(loom::detail::make_pmr_unique<loom::detail::memory_region_impl>(
              loom::get_default_resource(),
              &handle,
              bytes.data(),
              bytes.size(),
              loom::detail::mr_key_lease{},
              0,
              mode,
              domain)) {}
};
}  // namespace

TEST_CASE("collectives reject incompatible and out-of-range registrations",
          "[collective][registration]") {
    const auto op = GENERATE(operation::broadcast,
                             operation::all_reduce,
                             operation::all_gather,
                             operation::all_to_all,
                             operation::reduce,
                             operation::scatter,
                             operation::gather,
                             operation::reduce_scatter);
    const auto check_output = GENERATE(false, true);
    if (op == operation::broadcast && check_output)
        return;
    enum class condition {
        valid,
        bound,
        foreign,
        unknown,
        disabled,
        other_endpoint,
        undersized,
        unrelated,
        empty
    };
    const auto state = GENERATE(condition::valid,
                                condition::bound,
                                condition::foreign,
                                condition::unknown,
                                condition::disabled,
                                condition::other_endpoint,
                                condition::undersized,
                                condition::unrelated,
                                condition::empty);
    const auto null_descriptor = GENERATE(false, true);
    const auto error = GENERATE(FI_EAGAIN, FI_EIO);
    CAPTURE(op, check_output, state, null_descriptor, error);
    transfer_provider provider;
    provider.status = -error;
    auto group = provider.join();
    std::array<std::uint32_t, 8> input{}, output{}, unrelated{};
    std::span<std::byte> in = std::as_writable_bytes(std::span{input});
    std::span<std::byte> out = std::as_writable_bytes(std::span{output});
    const auto input_count =
        op == operation::all_to_all || op == operation::scatter || op == operation::reduce_scatter
            ? 6u
            : 2u;
    const auto output_count =
        op == operation::all_to_all || op == operation::all_gather || op == operation::gather ? 6u
                                                                                              : 2u;
    const auto input_bytes = input_count * sizeof(std::uint32_t);
    const auto output_bytes = output_count * sizeof(std::uint32_t);
    auto registered_bytes = check_output ? out.first(output_bytes) : in.first(input_bytes);
    if (state == condition::undersized)
        registered_bytes = registered_bytes.first(registered_bytes.size() - 1);
    if (state == condition::unrelated)
        registered_bytes = std::as_writable_bytes(std::span{unrelated});
    fid_domain foreign_domain{};
    auto* domain = state == condition::foreign   ? &foreign_domain
                   : state == condition::unknown ? nullptr
                                                 : &provider.domain_handle;
    const bool endpoint_bound = state == condition::bound || state == condition::disabled ||
                                state == condition::other_endpoint;
    transfer_provider other;
    auto other_ep = loom::endpoint{
        loom::detail::make_pmr_unique<loom::detail::endpoint_impl>(loom::get_default_resource(),
                                                                   &other.native_ep,
                                                                   loom::endpoint_types::rdm,
                                                                   loom::address_format::inet,
                                                                   0,
                                                                   0,
                                                                   loom::caps{},
                                                                   0,
                                                                   0,
                                                                   &provider.domain_handle)};
    registered_buffer registration{registered_bytes, domain, endpoint_bound ? FI_MR_ENDPOINT : 0};
    if (endpoint_bound && state != condition::disabled) {
        REQUIRE(
            registration.region.bind(state == condition::other_endpoint ? other_ep : provider.ep));
        REQUIRE(registration.region.enable());
    }
    if (state == condition::empty)
        registration.region = {};
    if (null_descriptor)
        registration.handle.mem_desc = nullptr;
    // Spare capacity is legal: only the prefix accessed by libfabric needs registration.
    if (op != operation::scatter && op != operation::reduce_scatter)
        in = in.first(input_bytes);
    else
        out = out.first(output_bytes);
    const auto result = submit(op,
                               provider,
                               group,
                               in,
                               out,
                               datatype::uint32,
                               nullptr,
                               nullptr,
                               check_output ? nullptr : &registration.region,
                               check_output ? &registration.region : nullptr);
    const bool valid = state == condition::valid || state == condition::bound;
    REQUIRE_FALSE(result);
    CHECK(result.error() == (valid ? loom::make_error_code_from_fi_errno(error)
                                   : loom::make_error_code(loom::errc::invalid_argument)));
    CHECK(provider.calls == (valid ? 1 : 0));
    if (valid) {
        CHECK(provider.count == 2);
        CHECK(provider.input_descriptor == (check_output ? nullptr : registration.handle.mem_desc));
        CHECK(provider.output_descriptor ==
              (check_output ? registration.handle.mem_desc : nullptr));
    }
}

TEST_CASE("collectives pass element counts with the correct per-member extent",
          "[collective][count]") {
    const auto op = GENERATE(operation::broadcast,
                             operation::all_reduce,
                             operation::all_gather,
                             operation::all_to_all,
                             operation::reduce,
                             operation::scatter,
                             operation::gather,
                             operation::reduce_scatter);
    CAPTURE(op);
    transfer_provider provider;
    auto group = provider.join();
    std::array<std::uint32_t, 6> input{};
    std::array<std::uint32_t, 6> output{};
    auto in = std::as_writable_bytes(std::span{input});
    auto out = std::as_writable_bytes(std::span{output});
    const auto input_count =
        op == operation::all_to_all || op == operation::scatter || op == operation::reduce_scatter
            ? 6u
            : 2u;
    const auto output_count =
        op == operation::all_to_all || op == operation::all_gather || op == operation::gather ? 6u
                                                                                              : 2u;
    int context{};
    REQUIRE(submit(op,
                   provider,
                   group,
                   in.first(input_count * sizeof(std::uint32_t)),
                   out.first(output_count * sizeof(std::uint32_t)),
                   datatype::uint32,
                   &context));
    REQUIRE(provider.count == 2);
    REQUIRE(provider.calls == 1);
    REQUIRE(provider.last_operation == op);
    REQUIRE(provider.type == FI_UINT32);
    REQUIRE(provider.input == input.data());
    REQUIRE(provider.address == 999);
    REQUIRE(provider.transfer_context == &context);
}

TEST_CASE("collectives ignore registrations for omitted non-root buffers",
          "[collective][registration]") {
    const auto op = GENERATE(operation::reduce, operation::scatter, operation::gather);
    const auto opaque_membership = GENERATE(false, true);
    transfer_provider provider;
    loom::collective::group_attr attributes;
    if (opaque_membership)
        attributes.set_flags = FI_UNIVERSE;
    auto group = provider.join(attributes);
    std::array<std::uint32_t, 2> values{};
    auto buffer = std::as_writable_bytes(std::span{values});
    fid_domain foreign_domain{};
    registered_buffer active{buffer, &provider.domain_handle};
    registered_buffer omitted{buffer, &foreign_domain, FI_MR_ENDPOINT};
    const auto scatter = op == operation::scatter;
    REQUIRE(submit(op,
                   provider,
                   group,
                   scatter ? std::span<std::byte>{} : buffer,
                   scatter ? buffer : std::span<std::byte>{},
                   datatype::uint32,
                   nullptr,
                   nullptr,
                   scatter ? &omitted.region : &active.region,
                   scatter ? &active.region : &omitted.region));
    CHECK(provider.calls == 1);
    CHECK(provider.count == 2);
    CHECK(provider.input_descriptor == (scatter ? nullptr : active.handle.mem_desc));
    CHECK(provider.output_descriptor == (scatter ? active.handle.mem_desc : nullptr));
}

TEST_CASE("collectives ignore registrations for zero-byte transfers",
          "[collective][registration]") {
    const auto op = GENERATE(operation::broadcast,
                             operation::all_reduce,
                             operation::all_gather,
                             operation::all_to_all,
                             operation::reduce,
                             operation::scatter,
                             operation::gather,
                             operation::reduce_scatter);
    transfer_provider provider;
    auto group = provider.join();
    fid_domain foreign_domain{};
    std::array<std::byte, 8> storage{};
    registered_buffer unused{storage, &foreign_domain, FI_MR_ENDPOINT};
    REQUIRE(submit(op,
                   provider,
                   group,
                   std::span{storage}.first(0),
                   {},
                   datatype::uint32,
                   nullptr,
                   nullptr,
                   &unused.region,
                   &unused.region));
    CHECK(provider.calls == 1);
    CHECK(provider.count == 0);
    CHECK(provider.input_descriptor == nullptr);
    CHECK(provider.output_descriptor == nullptr);
}

TEST_CASE("collective byte spans use the native datatype width", "[collective][count]") {
    struct layout {
        datatype type;
        fi_datatype native;
        std::size_t width;
    };
    const auto value = GENERATE(
        layout{datatype::int8, FI_INT8, 1},
        layout{datatype::uint8, FI_UINT8, 1},
        layout{datatype::int16, FI_INT16, 2},
        layout{datatype::uint16, FI_UINT16, 2},
        layout{datatype::int32, FI_INT32, 4},
        layout{datatype::uint32, FI_UINT32, 4},
        layout{datatype::int64, FI_INT64, 8},
        layout{datatype::uint64, FI_UINT64, 8},
        layout{datatype::float32, FI_FLOAT, sizeof(float)},
        layout{datatype::double64, FI_DOUBLE, sizeof(double)},
        layout{datatype::long_double, FI_LONG_DOUBLE, sizeof(long double)},
        layout{datatype::float_complex, FI_FLOAT_COMPLEX, 2 * sizeof(float)},
        layout{datatype::double_complex, FI_DOUBLE_COMPLEX, 2 * sizeof(double)},
        layout{datatype::long_double_complex, FI_LONG_DOUBLE_COMPLEX, 2 * sizeof(long double)},
        layout{datatype::int128, FI_INT128, 16},
        layout{datatype::uint128, FI_UINT128, 16});
    transfer_provider provider;
    auto group = provider.join();
    std::array<std::byte, 2 * sizeof(std::complex<long double>)> storage{};
    REQUIRE(submit(operation::broadcast,
                   provider,
                   group,
                   std::span{storage}.first(2 * value.width),
                   {},
                   value.type));
    REQUIRE(provider.count == 2);
    REQUIRE(provider.type == value.native);
}

TEMPLATE_TEST_CASE("typed collective transfers preserve element counts",
                   "[collective][count]",
                   std::uint16_t,
                   std::uint64_t,
                   double,
                   long double,
                   std::complex<float>,
                   std::complex<double>,
                   std::complex<long double>) {
    namespace coll = loom::collective;
    const auto op = GENERATE(operation::broadcast,
                             operation::all_reduce,
                             operation::all_gather,
                             operation::all_to_all,
                             operation::reduce,
                             operation::reduce_scatter);
    transfer_provider provider;
    auto group = provider.join();
    std::array<TestType, 6> input{};
    std::array<TestType, 6> output{};
    std::span<const TestType> in{input};
    std::span<TestType> out{output};
    const auto root = loom::fabric_addr{5};
    const auto sum = loom::atomic::operation::sum;
    auto& ep = provider.ep;
    loom::result<void> result;
    switch (op) {
        case operation::broadcast:
            result = coll::broadcast(ep, group, out.first(2), nullptr, root);
            break;
        case operation::all_reduce:
            result = coll::all_reduce(ep, group, in.first(2), out.first(2), nullptr, nullptr, sum);
            break;
        case operation::all_gather:
            result = coll::all_gather(ep, group, in.first(2), out, nullptr, nullptr);
            break;
        case operation::all_to_all:
            result = coll::all_to_all(ep, group, in, out, nullptr, nullptr);
            break;
        case operation::reduce:
            result =
                coll::reduce(ep, group, in.first(2), out.first(2), nullptr, nullptr, root, sum);
            break;
        case operation::reduce_scatter:
            result = coll::reduce_scatter(ep, group, in, out.first(2), nullptr, nullptr, sum);
            break;
        default:
            FAIL("unexpected operation");
    }
    constexpr auto native = [] {
        if constexpr (std::same_as<TestType, std::uint16_t>)
            return FI_UINT16;
        else if constexpr (std::same_as<TestType, std::uint64_t>)
            return FI_UINT64;
        else if constexpr (std::same_as<TestType, double>)
            return FI_DOUBLE;
        else if constexpr (std::same_as<TestType, long double>)
            return FI_LONG_DOUBLE;
        else if constexpr (std::same_as<TestType, std::complex<float>>)
            return FI_FLOAT_COMPLEX;
        else if constexpr (std::same_as<TestType, std::complex<double>>)
            return FI_DOUBLE_COMPLEX;
        else
            return FI_LONG_DOUBLE_COMPLEX;
    }();
    REQUIRE(result);
    REQUIRE(provider.count == 2);
    REQUIRE(provider.type == native);
    REQUIRE(provider.last_operation == op);
    REQUIRE(coll::all_reduce_inplace(ep, group, out.first(2), nullptr, sum));
    REQUIRE(provider.count == 2);
    REQUIRE(provider.input == provider.output);
}

TEST_CASE("collectives reject invalid datatypes and partial elements before submission",
          "[collective][count]") {
    const auto op = GENERATE(operation::broadcast,
                             operation::all_reduce,
                             operation::all_gather,
                             operation::all_to_all,
                             operation::reduce,
                             operation::scatter,
                             operation::gather,
                             operation::reduce_scatter);
    const auto invalid_type = GENERATE(false, true);
    transfer_provider provider;
    auto group = provider.join();
    std::array<std::byte, 3> input{};
    std::array<std::byte, 3> output{};
    const auto result = submit(op,
                               provider,
                               group,
                               input,
                               output,
                               invalid_type ? static_cast<datatype>(-1) : datatype::uint32);
    REQUIRE_FALSE(result);
    REQUIRE(result.error() == loom::errc::invalid_argument);
    REQUIRE(provider.calls == 0);
}

TEST_CASE("collectives reject undersized local and group buffers", "[collective][count]") {
    const auto op = GENERATE(operation::all_reduce,
                             operation::all_gather,
                             operation::all_to_all,
                             operation::reduce,
                             operation::scatter,
                             operation::gather,
                             operation::reduce_scatter);
    transfer_provider provider;
    auto group = provider.join();
    std::array<std::byte, 24> input{};
    std::array<std::byte, 24> output{};
    std::span<std::byte> in{input};
    std::span<std::byte> out{output};
    if (op == operation::scatter || op == operation::reduce_scatter) {
        in = in.first(23);
        out = out.first(8);
    } else {
        in = in.first(op == operation::all_to_all ? 24 : 8);
        out = out.first(op == operation::all_reduce || op == operation::reduce ? 7 : 23);
    }
    const auto result = submit(op, provider, group, in, out, datatype::uint32);
    REQUIRE_FALSE(result);
    REQUIRE(result.error() == loom::errc::invalid_argument);
    REQUIRE(provider.calls == 0);
}

TEST_CASE("all-to-all input must partition into whole elements per member", "[collective][count]") {
    transfer_provider provider;
    auto group = provider.join();
    std::array<std::byte, 20> input{};
    std::array<std::byte, 24> output{};
    REQUIRE_FALSE(submit(operation::all_to_all, provider, group, input, output, datatype::uint32));
    REQUIRE(provider.calls == 0);
}

TEST_CASE("collectives accept omitted non-root buffers", "[collective][count]") {
    const auto op = GENERATE(operation::reduce, operation::gather, operation::scatter);
    transfer_provider provider;
    auto group = provider.join();
    std::array<std::byte, 8> storage{};
    const auto result = op == operation::scatter
                            ? submit(op, provider, group, {}, storage, datatype::uint32)
                            : submit(op, provider, group, storage, {}, datatype::uint32);
    REQUIRE(result);
    REQUIRE(provider.count == 2);
}

TEST_CASE("collective group extents require a known nonoverflowing membership",
          "[collective][count]") {
    const auto op =
        GENERATE(operation::all_gather, operation::all_to_all, operation::reduce_scatter);
    const auto size = GENERATE(std::optional<std::size_t>{},
                               std::optional<std::size_t>{3},
                               std::optional{std::numeric_limits<std::size_t>::max()});
    transfer_provider provider;
    loom::collective::group_attr attr{};
    attr.set_flags = FI_UNIVERSE;
    attr.member_count = size;
    auto group = provider.join(attr);
    REQUIRE(group.member_count() == size);
    std::array<std::byte, 24> input{};
    std::array<std::byte, 24> output{};
    const auto result = submit(op,
                               provider,
                               group,
                               std::span{input}.first(op == operation::all_gather ? 8 : 24),
                               std::span{output}.first(op == operation::reduce_scatter ? 8 : 24),
                               datatype::uint32);
    if (size == 3) {
        REQUIRE(result);
        REQUIRE(provider.count == 2);
    } else {
        REQUIRE_FALSE(result);
        REQUIRE(provider.calls == 0);
    }
}

TEST_CASE("collectives preserve zero counts spare capacity and provider failures",
          "[collective][count]") {
    transfer_provider provider;
    auto group = provider.join();
    const auto op = GENERATE(operation::broadcast,
                             operation::all_reduce,
                             operation::all_gather,
                             operation::all_to_all,
                             operation::reduce,
                             operation::scatter,
                             operation::gather,
                             operation::reduce_scatter);
    REQUIRE(submit(op, provider, group, {}, {}, datatype::uint32));
    REQUIRE(provider.count == 0);
    provider.status = -FI_EAGAIN;
    REQUIRE(submit(op, provider, group, {}, {}, datatype::uint32).error() ==
            loom::make_error_code_from_fi_errno(FI_EAGAIN));
    provider.status = 0;
    std::array<std::byte, 8> input{};
    std::array<std::byte, 9> output{};
    REQUIRE(submit(operation::all_reduce, provider, group, input, output, datatype::uint32));
    REQUIRE(provider.count == 2);
}

TEST_CASE("collective transfers reject empty endpoints", "[collective][invalid]") {
    const auto op = GENERATE(operation::broadcast,
                             operation::all_reduce,
                             operation::all_gather,
                             operation::all_to_all,
                             operation::reduce,
                             operation::scatter,
                             operation::gather,
                             operation::reduce_scatter);
    transfer_provider provider;
    auto group = provider.join();
    loom::endpoint empty;
    const auto result = submit(op, provider, group, {}, {}, datatype::uint32, nullptr, &empty);
    REQUIRE_FALSE(result);
    REQUIRE(result.error() == loom::errc::invalid_argument);
    REQUIRE(provider.calls == 0);
}

TEST_CASE("collectives reject missing native operations", "[collective][invalid]") {
    const auto op = GENERATE(operation::broadcast,
                             operation::all_reduce,
                             operation::all_gather,
                             operation::all_to_all,
                             operation::reduce,
                             operation::scatter,
                             operation::gather,
                             operation::reduce_scatter);
    const auto missing = GENERATE(0, 1, 2);
    transfer_provider provider;
    auto group = provider.join();
    if (missing == 0) {
        provider.native_ep.collective = nullptr;
    } else if (missing == 1) {
        provider.transfers.size = sizeof(std::size_t);
    } else {
        switch (op) {
            case operation::broadcast:
                provider.transfers.broadcast = nullptr;
                break;
            case operation::all_reduce:
                provider.transfers.allreduce = nullptr;
                break;
            case operation::all_gather:
                provider.transfers.allgather = nullptr;
                break;
            case operation::all_to_all:
                provider.transfers.alltoall = nullptr;
                break;
            case operation::reduce:
                provider.transfers.reduce = nullptr;
                break;
            case operation::scatter:
                provider.transfers.scatter = nullptr;
                break;
            case operation::gather:
                provider.transfers.gather = nullptr;
                break;
            case operation::reduce_scatter:
                provider.transfers.reduce_scatter = nullptr;
                break;
            default:
                FAIL("unexpected operation");
        }
    }
    const auto result = submit(op, provider, group, {}, {}, datatype::uint32);
    REQUIRE_FALSE(result);
    REQUIRE(result.error() == loom::make_error_code_from_fi_errno(FI_ENOSYS));
    REQUIRE(provider.calls == 0);
}

TEST_CASE("collectives reject groups without published addresses", "[collective][invalid]") {
    const auto op = GENERATE(operation::broadcast,
                             operation::all_reduce,
                             operation::all_gather,
                             operation::all_to_all,
                             operation::reduce,
                             operation::scatter,
                             operation::gather,
                             operation::reduce_scatter);
    const auto delayed = GENERATE(false, true);
    transfer_provider provider;
    provider.delayed = delayed;
    auto group = provider.join();
    if (!delayed)
        group = {};
    const auto result = submit(op, provider, group, {}, {}, datatype::uint32);
    if (delayed) {
        // Complete the mock join before releasing its borrowed output slot.
        *provider.pending = &provider.native_group;
        provider.pending = nullptr;
    }
    REQUIRE_FALSE(result);
    REQUIRE(result.error() == loom::errc::invalid_argument);
    REQUIRE(provider.calls == 0);
}

TEST_CASE("collective reductions validate operation identifiers", "[collective][validation]") {
    namespace coll = loom::collective;
    enum class path {
        all_reduce,
        typed_all_reduce,
        inplace,
        reduce,
        typed_reduce,
        reduce_scatter,
        typed_reduce_scatter
    };
    const auto route = GENERATE(path::all_reduce,
                                path::typed_all_reduce,
                                path::inplace,
                                path::reduce,
                                path::typed_reduce,
                                path::reduce_scatter,
                                path::typed_reduce_scatter);
    const auto status = GENERATE(0, -FI_EAGAIN);
    for (const auto& reduction : loom::test::collective_reductions) {
        CAPTURE(route, status, reduction.operation);
        transfer_provider provider;
        provider.status = status;
        auto group = provider.join();
        std::array<std::uint64_t, 3> input{}, output{};
        const std::span<const std::uint64_t> in{input};
        const std::span<std::uint64_t> out{output};
        const auto op = reduction.operation;
        const loom::fabric_addr root{5};
        const auto result = [&]() -> loom::result<void> {
            switch (route) {
                case path::all_reduce:
                    return coll::all_reduce(provider.ep,
                                            group,
                                            std::as_bytes(in),
                                            std::as_writable_bytes(out),
                                            nullptr,
                                            nullptr,
                                            op,
                                            datatype::uint64);
                case path::typed_all_reduce:
                    return coll::all_reduce(provider.ep, group, in, out, nullptr, nullptr, op);
                case path::inplace:
                    return coll::all_reduce_inplace(provider.ep, group, out, nullptr, op);
                case path::reduce:
                    return coll::reduce(provider.ep,
                                        group,
                                        std::as_bytes(in),
                                        std::as_writable_bytes(out),
                                        nullptr,
                                        nullptr,
                                        root,
                                        op,
                                        datatype::uint64);
                case path::typed_reduce:
                    return coll::reduce(provider.ep, group, in, out, nullptr, nullptr, root, op);
                case path::reduce_scatter:
                    return coll::reduce_scatter(provider.ep,
                                                group,
                                                std::as_bytes(in),
                                                std::as_writable_bytes(out.first(1)),
                                                nullptr,
                                                nullptr,
                                                op,
                                                datatype::uint64);
                case path::typed_reduce_scatter:
                    return coll::reduce_scatter(
                        provider.ep, group, in, out.first(1), nullptr, nullptr, op);
            }
            std::unreachable();
        }();
        REQUIRE(static_cast<bool>(result) == (reduction.valid && status == 0));
        if (!result)
            CHECK(result.error() ==
                  (reduction.valid ? loom::errc::again : loom::errc::invalid_argument));
        CHECK(provider.calls == (reduction.valid ? 1 : 0));
        if (reduction.valid)
            CHECK(provider.reduction == reduction.native);
    }
}

TEST_CASE("collective barriers check their selected native operation", "[collective][invalid]") {
    const auto flags = GENERATE(std::uint64_t{0}, std::uint64_t{FI_FENCE});
    const auto missing = GENERATE(0, 1, 2, 3);
    transfer_provider provider;
    auto group = provider.join();
    if (missing == 1)
        provider.native_ep.collective = nullptr;
    if (missing == 2)
        provider.transfers.barrier = nullptr;
    if (missing == 3)
        provider.transfers.size = offsetof(fi_ops_collective, barrier2);
    int context{};
    const auto result = loom::collective::barrier(provider.ep, group, flags, &context);
    const bool supported =
        missing == 0 || (missing == 2 && flags != 0) || (missing == 3 && flags == 0);
    REQUIRE(static_cast<bool>(result) == supported);
    if (supported) {
        REQUIRE(provider.calls == 1);
        REQUIRE(provider.transfer_context == &context);
        REQUIRE(provider.address == 999);
    } else {
        REQUIRE(provider.calls == 0);
        REQUIRE(result.error() == loom::make_error_code_from_fi_errno(FI_ENOSYS));
    }
    const auto direct = loom::collective::barrier(provider.ep, group.address());
    REQUIRE(static_cast<bool>(direct) == (missing == 0 || missing == 3));
    loom::endpoint empty;
    REQUIRE(loom::collective::barrier(empty, group.address()).error() ==
            loom::errc::invalid_argument);
    REQUIRE(loom::collective::barrier(provider.ep, loom::fabric_addrs::unspecified).error() ==
            loom::errc::invalid_argument);
}
