// SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only
#pragma once

#include <loom/async/completion_queue.hpp>
#include <loom/loom.hpp>

#include <algorithm>
#include <cmath>
#include <format>
#include <memory>
#include <numeric>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <mpi.h>

namespace loom::benchmark {

inline constexpr std::size_t max_address_size = 256;

class mpi_context {
public:
    mpi_context(int* argc, char*** argv) {
        MPI_Init(argc, argv);
        MPI_Comm_rank(MPI_COMM_WORLD, &rank_);
        MPI_Comm_size(MPI_COMM_WORLD, &size_);
    }

    ~mpi_context() noexcept { MPI_Finalize(); }

    mpi_context(const mpi_context&) = delete;
    mpi_context(mpi_context&&) = delete;
    auto operator=(const mpi_context&) -> mpi_context& = delete;
    auto operator=(mpi_context&&) -> mpi_context& = delete;

    [[nodiscard]] auto rank() const noexcept -> int { return rank_; }
    [[nodiscard]] auto size() const noexcept -> int { return size_; }
    [[nodiscard]] auto is_root() const noexcept -> bool { return rank_ == 0; }

private:
    int rank_{0};
    int size_{0};
};

struct fabric_setup {
    std::unique_ptr<loom::fabric> fabric;
    std::unique_ptr<loom::domain> domain;
    std::shared_ptr<loom::completion_queue> cq;
    std::unique_ptr<loom::endpoint> endpoint;
    std::vector<loom::address> peer_addresses;
    std::unique_ptr<loom::memory_region> mr;

    [[nodiscard]] static auto exchange_addresses(const mpi_context& mpi, const loom::endpoint& ep)
        -> std::vector<loom::address> {
        auto local_addr = ep.get_local_address();

        std::vector<char> local_addr_data(max_address_size);
        std::size_t local_size = serialize_address(local_addr, local_addr_data);

        std::vector<int> sizes(static_cast<std::size_t>(mpi.size()));
        int local_size_int = static_cast<int>(local_size);
        MPI_Allgather(&local_size_int, 1, MPI_INT, sizes.data(), 1, MPI_INT, MPI_COMM_WORLD);

        std::vector<int> displacements(static_cast<std::size_t>(mpi.size()));
        std::exclusive_scan(sizes.begin(), sizes.end(), displacements.begin(), 0);
        int total_size = displacements.back() + sizes.back();

        std::vector<char> all_addr_data(static_cast<std::size_t>(total_size));
        MPI_Allgatherv(local_addr_data.data(),
                       local_size_int,
                       MPI_BYTE,
                       all_addr_data.data(),
                       sizes.data(),
                       displacements.data(),
                       MPI_BYTE,
                       MPI_COMM_WORLD);

        std::vector<loom::address> peer_addrs;
        peer_addrs.reserve(static_cast<std::size_t>(mpi.size() - 1));

        for (int i = 0; i < mpi.size(); ++i) {
            if (i != mpi.rank()) {
                auto addr = deserialize_address(
                    all_addr_data.data() + displacements[static_cast<std::size_t>(i)],
                    static_cast<std::size_t>(sizes[static_cast<std::size_t>(i)]));
                peer_addrs.push_back(addr);
            }
        }

        return peer_addrs;
    }

    [[nodiscard]] static auto create(const mpi_context& mpi,
                                     std::string_view provider_hint = {},
                                     std::size_t mr_size = 1024 * 1024) -> fabric_setup {
        fabric_setup setup;

        auto hints = loom::fabric_info::hints();
        if (!provider_hint.empty()) {
            hints.set_provider_name(std::string{provider_hint});
        }
        hints.set_endpoint_type(loom::endpoint_types::msg);
        hints.set_caps(loom::capability::msg | loom::capability::rma | loom::capability::atomic);

        auto infos = loom::fabric_info::query(hints);
        if (infos.empty()) {
            throw std::runtime_error("No suitable fabric providers found");
        }

        if (mpi.is_root()) {
            std::cout << std::format("Using provider: {}\n", infos[0].get_provider_name());
        }

        setup.fabric = loom::fabric::create(infos[0]);
        setup.domain = loom::domain::create(*setup.fabric, infos[0]);

        setup.cq = loom::completion_queue::create(*setup.domain);

        setup.endpoint = loom::endpoint::create(*setup.domain, infos[0]);
        setup.endpoint->bind_cq(*setup.cq, loom::cq_bind::transmit | loom::cq_bind::recv);
        setup.endpoint->enable();

        setup.peer_addresses = exchange_addresses(mpi, *setup.endpoint);

        setup.mr = loom::memory_region::register_region(
            *setup.domain,
            mr_size,
            loom::mr_access::read | loom::mr_access::write | loom::mr_access::remote_read |
                loom::mr_access::remote_write);

        if (mpi.size() >= 2 && !setup.peer_addresses.empty()) {
            int peer_rank = (mpi.rank() + 1) % mpi.size();
            if (static_cast<std::size_t>(peer_rank) < setup.peer_addresses.size()) {
                setup.endpoint->connect(setup.peer_addresses[static_cast<std::size_t>(peer_rank)]);
            }
        }

        MPI_Barrier(MPI_COMM_WORLD);

        return setup;
    }

private:
    [[nodiscard]] static auto serialize_address(const loom::address& addr,
                                                std::vector<char>& buffer) -> std::size_t {
        return std::visit(
            [&buffer](const auto& a) -> std::size_t {
                using T = std::decay_t<decltype(a)>;

                static_assert(std::is_trivially_copyable_v<T>,
                              "Address type must be trivially copyable for serialization");

                if constexpr (std::is_same_v<T, loom::ipv4_address>) {
                    std::memcpy(buffer.data(), &a, sizeof(a));
                    return sizeof(a);
                } else if constexpr (std::is_same_v<T, loom::ipv6_address>) {
                    std::memcpy(buffer.data(), &a, sizeof(a));
                    return sizeof(a);
                } else {
                    return 0;
                }
            },
            addr);
    }

    [[nodiscard]] static auto deserialize_address(const char* data, std::size_t size)
        -> loom::address {
        if (size == sizeof(loom::ipv4_address)) {
            loom::ipv4_address addr;
            static_assert(std::is_trivially_copyable_v<loom::ipv4_address>);
            std::memcpy(&addr, data, size);
            return addr;
        }
        if (size == sizeof(loom::ipv6_address)) {
            loom::ipv6_address addr;
            static_assert(std::is_trivially_copyable_v<loom::ipv6_address>);
            std::memcpy(&addr, data, size);
            return addr;
        }
        return loom::unspecified_address{};
    }
};

struct benchmark_result {
    std::chrono::nanoseconds mean;
    std::chrono::nanoseconds median;
    std::chrono::nanoseconds p99;
    std::chrono::nanoseconds min;
    std::chrono::nanoseconds max;
    std::chrono::nanoseconds stddev;

    [[nodiscard]] static auto aggregate(const mpi_context& mpi, const benchmark_result& local)
        -> benchmark_result {
        struct alignas(8) mpi_data {
            std::int64_t mean;
            std::int64_t median;
            std::int64_t p99;
            std::int64_t min;
            std::int64_t max;
            std::int64_t stddev;
        };

        mpi_data local_data{local.mean.count(),
                            local.median.count(),
                            local.p99.count(),
                            local.min.count(),
                            local.max.count(),
                            local.stddev.count()};

        mpi_data global_sum{};
        mpi_data global_min{};
        mpi_data global_max{};

        MPI_Reduce(&local_data, &global_sum, 6, MPI_INT64_T, MPI_SUM, 0, MPI_COMM_WORLD);
        MPI_Reduce(&local_data, &global_min, 6, MPI_INT64_T, MPI_MIN, 0, MPI_COMM_WORLD);
        MPI_Reduce(&local_data, &global_max, 6, MPI_INT64_T, MPI_MAX, 0, MPI_COMM_WORLD);

        if (mpi.is_root()) {
            return {std::chrono::nanoseconds{global_sum.mean / mpi.size()},
                    std::chrono::nanoseconds{global_sum.median / mpi.size()},
                    std::chrono::nanoseconds{global_sum.p99 / mpi.size()},
                    std::chrono::nanoseconds{global_min.min},
                    std::chrono::nanoseconds{global_max.max},
                    std::chrono::nanoseconds{global_sum.stddev / mpi.size()}};
        }

        return local;
    }

    void print(std::string_view name) const {
        std::cout << std::format("{:50s}: mean={:8d}ns, median={:8d}ns, "
                                 "p99={:8d}ns, min={:8d}ns, max={:8d}ns, stddev={:8d}ns\n",
                                 name,
                                 mean.count(),
                                 median.count(),
                                 p99.count(),
                                 min.count(),
                                 max.count(),
                                 stddev.count());
    }
};

[[nodiscard]] inline auto calculate_stats(std::vector<std::chrono::nanoseconds>& timings)
    -> benchmark_result {
    const std::chrono::nanoseconds sum =
        std::accumulate(timings.begin(), timings.end(), std::chrono::nanoseconds{0});
    const std::chrono::nanoseconds mean = sum / static_cast<std::int64_t>(timings.size());

    std::ranges::sort(timings);

    const std::chrono::nanoseconds median = timings[timings.size() / 2];
    const std::chrono::nanoseconds p99 = timings[static_cast<std::size_t>(timings.size() * 0.99)];
    const std::chrono::nanoseconds min = timings.front();
    const std::chrono::nanoseconds max = timings.back();

    std::chrono::nanoseconds variance_sum{0};
    for (const auto& t : timings) {
        const auto diff = t - mean;
        variance_sum += std::chrono::nanoseconds{diff.count() * diff.count()};
    }
    const auto variance = variance_sum / static_cast<std::int64_t>(timings.size());
    const std::chrono::nanoseconds stddev{
        static_cast<std::int64_t>(std::sqrt(static_cast<double>(variance.count())))};

    return {mean, median, p99, min, max, stddev};
}

}  // namespace loom::benchmark
