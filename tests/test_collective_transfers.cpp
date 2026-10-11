// SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only
#include <loom/core/collective.hpp>

#include <array>
#include <complex>
#include <limits>
#include <string>

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
    std::size_t count{};
    fi_datatype type{};
    const void* input{};
    void* output{};
    void* transfer_context{};
    fi_addr_t address{};

    static auto record(fid_ep* ep,
                       operation op,
                       const void* input,
                       void* output,
                       std::size_t count,
                       fi_datatype type,
                       fi_addr_t address,
                       void* context) -> ssize_t {
        auto& self =
            static_cast<transfer_provider&>(*static_cast<collective_provider*>(ep->fid.context));
        ++self.calls;
        self.last_operation = op;
        self.input = input;
        self.output = output;
        self.count = count;
        self.type = type;
        self.address = address;
        self.transfer_context = context;
        return self.status;
    }

    fi_ops_collective transfers{
        .size = sizeof(fi_ops_collective),
        .broadcast =
            [](fid_ep* ep,
               void* input,
               std::size_t count,
               void*,
               fi_addr_t address,
               fi_addr_t,
               fi_datatype type,
               std::uint64_t,
               void* context) {
                return record(
                    ep, operation::broadcast, input, nullptr, count, type, address, context);
            },
        .alltoall =
            [](fid_ep* ep,
               const void* input,
               std::size_t count,
               void*,
               void* output,
               void*,
               fi_addr_t address,
               fi_datatype type,
               std::uint64_t,
               void* context) {
                return record(
                    ep, operation::all_to_all, input, output, count, type, address, context);
            },
        .allreduce =
            [](fid_ep* ep,
               const void* input,
               std::size_t count,
               void*,
               void* output,
               void*,
               fi_addr_t address,
               fi_datatype type,
               fi_op,
               std::uint64_t,
               void* context) {
                return record(
                    ep, operation::all_reduce, input, output, count, type, address, context);
            },
        .allgather =
            [](fid_ep* ep,
               const void* input,
               std::size_t count,
               void*,
               void* output,
               void*,
               fi_addr_t address,
               fi_datatype type,
               std::uint64_t,
               void* context) {
                return record(
                    ep, operation::all_gather, input, output, count, type, address, context);
            },
        .reduce_scatter =
            [](fid_ep* ep,
               const void* input,
               std::size_t count,
               void*,
               void* output,
               void*,
               fi_addr_t address,
               fi_datatype type,
               fi_op,
               std::uint64_t,
               void* context) {
                return record(
                    ep, operation::reduce_scatter, input, output, count, type, address, context);
            },
        .reduce =
            [](fid_ep* ep,
               const void* input,
               std::size_t count,
               void*,
               void* output,
               void*,
               fi_addr_t address,
               fi_addr_t,
               fi_datatype type,
               fi_op,
               std::uint64_t,
               void* context) {
                return record(ep, operation::reduce, input, output, count, type, address, context);
            },
        .scatter =
            [](fid_ep* ep,
               const void* input,
               std::size_t count,
               void*,
               void* output,
               void*,
               fi_addr_t address,
               fi_addr_t,
               fi_datatype type,
               std::uint64_t,
               void* context) {
                return record(ep, operation::scatter, input, output, count, type, address, context);
            },
        .gather =
            [](fid_ep* ep,
               const void* input,
               std::size_t count,
               void*,
               void* output,
               void*,
               fi_addr_t address,
               fi_addr_t,
               fi_datatype type,
               std::uint64_t,
               void* context) {
                return record(ep, operation::gather, input, output, count, type, address, context);
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
            void* context = nullptr) -> loom::result<void> {
    namespace coll = loom::collective;
    const auto root = loom::fabric_addr{5};
    const auto sum = loom::atomic::operation::sum;
    auto& ep = provider.ep;
    switch (op) {
        case operation::broadcast:
            return coll::broadcast(ep, group, input, nullptr, root, type, context);
        case operation::all_reduce:
            return coll::all_reduce(ep, group, input, output, nullptr, nullptr, sum, type, context);
        case operation::all_gather:
            return coll::all_gather(ep, group, input, output, nullptr, nullptr, type, context);
        case operation::all_to_all:
            return coll::all_to_all(ep, group, input, output, nullptr, nullptr, type, context);
        case operation::reduce:
            return coll::reduce(
                ep, group, input, output, nullptr, nullptr, root, sum, type, context);
        case operation::scatter:
            return coll::scatter(ep, group, input, output, nullptr, nullptr, root, type, context);
        case operation::gather:
            return coll::gather(ep, group, input, output, nullptr, nullptr, root, type, context);
        case operation::reduce_scatter:
            return coll::reduce_scatter(
                ep, group, input, output, nullptr, nullptr, sum, type, context);
        default:
            return loom::make_error_result<void>(loom::errc::invalid_argument);
    }
}
}  // namespace

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
