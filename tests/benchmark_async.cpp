// SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only
#include <loom/async.hpp>

#include <algorithm>
#include <chrono>
#include <format>
#include <iostream>
#include <numeric>
#include <vector>

#include "benchmark_mpi_setup.hpp"
#include <stdexec/execution.hpp>

using namespace loom::benchmark;
using clock_type = std::chrono::high_resolution_clock;
using duration_type = std::chrono::nanoseconds;

template <typename Func>
auto benchmark_operation(Func&& func, std::size_t iterations) -> benchmark_result {
    std::vector<duration_type> timings;
    timings.reserve(iterations);

    for (std::size_t i = 0; i < iterations / 10; ++i) {
        func();
    }

    for (std::size_t i = 0; i < iterations; ++i) {
        auto start = clock_type::now();
        func();
        auto end = clock_type::now();
        timings.push_back(std::chrono::duration_cast<duration_type>(end - start));
    }

    return calculate_stats(timings);
}

void benchmark_send_latency(const mpi_context& mpi, fabric_setup& setup) {
    if (mpi.is_root()) {
        std::cout << "\n=== Send Operation Latency ===\n";
    }

    std::array<std::byte, 64> buffer{};
    std::ranges::fill(buffer, std::byte{0xAB});

    if (mpi.rank() == 0) {
        auto local_result = benchmark_operation(
            [&] {
                auto op = loom::async(*setup.endpoint).send(buffer, setup.cq.get());
                stdexec::sync_wait(std::move(op));
            },
            1000);

        auto global_result = benchmark_result::aggregate(mpi, local_result);
        global_result.print("send (64B)");

    } else if (mpi.rank() == 1) {
        auto local_result = benchmark_operation(
            [&] {
                auto op = loom::async(*setup.endpoint).recv(buffer, setup.cq.get());
                stdexec::sync_wait(std::move(op));
            },
            1000);

        benchmark_result::aggregate(mpi, local_result);
    }

    MPI_Barrier(MPI_COMM_WORLD);
}

void benchmark_recv_latency(const mpi_context& mpi, fabric_setup& setup) {
    if (mpi.is_root()) {
        std::cout << "\n=== Recv Operation Latency ===\n";
    }

    std::array<std::byte, 64> buffer{};

    if (mpi.rank() == 1) {
        std::ranges::fill(buffer, std::byte{0xCD});
        auto local_result = benchmark_operation(
            [&] {
                auto op = loom::async(*setup.endpoint).send(buffer, setup.cq.get());
                stdexec::sync_wait(std::move(op));
            },
            1000);

        benchmark_result::aggregate(mpi, local_result);

    } else if (mpi.rank() == 0) {
        auto local_result = benchmark_operation(
            [&] {
                auto op = loom::async(*setup.endpoint).recv(buffer, setup.cq.get());
                auto [bytes] = stdexec::sync_wait(std::move(op)).value();
                if (bytes != 64) {
                    throw std::runtime_error("Unexpected byte count");
                }
            },
            1000);

        auto global_result = benchmark_result::aggregate(mpi, local_result);
        global_result.print("recv (64B)");
    }

    MPI_Barrier(MPI_COMM_WORLD);
}

void benchmark_ping_pong(const mpi_context& mpi, fabric_setup& setup) {
    if (mpi.is_root()) {
        std::cout << "\n=== Ping-Pong Round-Trip Latency ===\n";
    }

    std::array<std::byte, 64> buffer{};
    std::ranges::fill(buffer, std::byte{0xEF});

    if (mpi.rank() == 0) {
        auto local_result = benchmark_operation(
            [&] {
                auto send_op = loom::async(*setup.endpoint).send(buffer, setup.cq.get());
                stdexec::sync_wait(std::move(send_op));

                auto recv_op = loom::async(*setup.endpoint).recv(buffer, setup.cq.get());
                stdexec::sync_wait(std::move(recv_op));
            },
            500);

        auto global_result = benchmark_result::aggregate(mpi, local_result);
        global_result.print("ping-pong roundtrip (64B)");

    } else if (mpi.rank() == 1) {
        auto local_result = benchmark_operation(
            [&] {
                auto recv_op = loom::async(*setup.endpoint).recv(buffer, setup.cq.get());
                stdexec::sync_wait(std::move(recv_op));

                auto send_op = loom::async(*setup.endpoint).send(buffer, setup.cq.get());
                stdexec::sync_wait(std::move(send_op));
            },
            500);

        benchmark_result::aggregate(mpi, local_result);
    }

    MPI_Barrier(MPI_COMM_WORLD);
}

void benchmark_rdma_read(const mpi_context& mpi, fabric_setup& setup) {
    if (mpi.is_root()) {
        std::cout << "\n=== RDMA Read Latency ===\n";
    }

    std::vector<std::size_t> sizes = {64, 256, 1024, 4096};
    std::vector<std::byte> buffer(4096);

    auto local_addr = setup.mr->get_base_address();
    auto local_key = setup.mr->get_key();

    loom::rma_addr remote_addr;
    loom::mr_key remote_key;

    MPI_Sendrecv(&local_addr,
                 sizeof(local_addr),
                 MPI_BYTE,
                 (mpi.rank() + 1) % 2,
                 0,
                 &remote_addr,
                 sizeof(remote_addr),
                 MPI_BYTE,
                 (mpi.rank() + 1) % 2,
                 0,
                 MPI_COMM_WORLD,
                 MPI_STATUS_IGNORE);

    MPI_Sendrecv(&local_key,
                 sizeof(local_key),
                 MPI_BYTE,
                 (mpi.rank() + 1) % 2,
                 0,
                 &remote_key,
                 sizeof(remote_key),
                 MPI_BYTE,
                 (mpi.rank() + 1) % 2,
                 0,
                 MPI_COMM_WORLD,
                 MPI_STATUS_IGNORE);

    for (auto size : sizes) {
        if (mpi.rank() == 0) {
            auto local_result = benchmark_operation(
                [&] {
                    auto op = loom::rma_async(*setup.endpoint)
                                  .read(std::span{buffer.data(), size},
                                        remote_addr,
                                        remote_key,
                                        setup.cq.get());
                    stdexec::sync_wait(std::move(op));
                },
                500);

            auto global_result = benchmark_result::aggregate(mpi, local_result);
            global_result.print(std::format("rdma_read ({}B)", size));

        } else if (mpi.rank() == 1) {
            benchmark_result dummy{};
            benchmark_result::aggregate(mpi, dummy);
        }

        MPI_Barrier(MPI_COMM_WORLD);
    }
}

void benchmark_rdma_write(const mpi_context& mpi, fabric_setup& setup) {
    if (mpi.is_root()) {
        std::cout << "\n=== RDMA Write Latency ===\n";
    }

    std::vector<std::size_t> sizes = {64, 256, 1024, 4096};
    std::vector<std::byte> buffer(4096);
    std::ranges::fill(buffer, std::byte{0x42});

    auto local_addr = setup.mr->get_base_address();
    auto local_key = setup.mr->get_key();

    loom::rma_addr remote_addr;
    loom::mr_key remote_key;

    MPI_Sendrecv(&local_addr,
                 sizeof(local_addr),
                 MPI_BYTE,
                 (mpi.rank() + 1) % 2,
                 0,
                 &remote_addr,
                 sizeof(remote_addr),
                 MPI_BYTE,
                 (mpi.rank() + 1) % 2,
                 0,
                 MPI_COMM_WORLD,
                 MPI_STATUS_IGNORE);

    MPI_Sendrecv(&local_key,
                 sizeof(local_key),
                 MPI_BYTE,
                 (mpi.rank() + 1) % 2,
                 0,
                 &remote_key,
                 sizeof(remote_key),
                 MPI_BYTE,
                 (mpi.rank() + 1) % 2,
                 0,
                 MPI_COMM_WORLD,
                 MPI_STATUS_IGNORE);

    for (auto size : sizes) {
        if (mpi.rank() == 0) {
            auto local_result = benchmark_operation(
                [&] {
                    auto op = loom::rma_async(*setup.endpoint)
                                  .write(std::span{buffer.data(), size},
                                         remote_addr,
                                         remote_key,
                                         setup.cq.get());
                    stdexec::sync_wait(std::move(op));
                },
                500);

            auto global_result = benchmark_result::aggregate(mpi, local_result);
            global_result.print(std::format("rdma_write ({}B)", size));

        } else if (mpi.rank() == 1) {
            benchmark_result dummy{};
            benchmark_result::aggregate(mpi, dummy);
        }

        MPI_Barrier(MPI_COMM_WORLD);
    }
}

void benchmark_atomic_ops(const mpi_context& mpi, fabric_setup& setup) {
    if (mpi.is_root()) {
        std::cout << "\n=== Atomic Operation Latency ===\n";
    }

    auto local_addr = setup.mr->get_base_address();
    auto local_key = setup.mr->get_key();

    loom::rma_addr remote_addr;
    loom::mr_key remote_key;

    MPI_Sendrecv(&local_addr,
                 sizeof(local_addr),
                 MPI_BYTE,
                 (mpi.rank() + 1) % 2,
                 0,
                 &remote_addr,
                 sizeof(remote_addr),
                 MPI_BYTE,
                 (mpi.rank() + 1) % 2,
                 0,
                 MPI_COMM_WORLD,
                 MPI_STATUS_IGNORE);

    MPI_Sendrecv(&local_key,
                 sizeof(local_key),
                 MPI_BYTE,
                 (mpi.rank() + 1) % 2,
                 0,
                 &remote_key,
                 sizeof(remote_key),
                 MPI_BYTE,
                 (mpi.rank() + 1) % 2,
                 0,
                 MPI_COMM_WORLD,
                 MPI_STATUS_IGNORE);

    if (mpi.rank() == 1) {
        auto* counter_ptr = reinterpret_cast<std::uint64_t*>(setup.mr->get_base_address().get());
        *counter_ptr = 0;
    }
    MPI_Barrier(MPI_COMM_WORLD);

    if (mpi.rank() == 0) {
        auto local_result = benchmark_operation(
            [&] {
                auto op = loom::rma_async(*setup.endpoint)
                              .fetch_add<std::uint64_t>(remote_addr, 1, remote_key, setup.cq.get());
                auto [old_val] = stdexec::sync_wait(std::move(op)).value();
            },
            500);

        auto global_result = benchmark_result::aggregate(mpi, local_result);
        global_result.print("atomic_fetch_add");

    } else if (mpi.rank() == 1) {
        benchmark_result dummy{};
        benchmark_result::aggregate(mpi, dummy);
    }

    MPI_Barrier(MPI_COMM_WORLD);

    if (mpi.rank() == 1) {
        auto* counter_ptr = reinterpret_cast<std::uint64_t*>(setup.mr->get_base_address().get());
        *counter_ptr = 0;
    }
    MPI_Barrier(MPI_COMM_WORLD);

    if (mpi.rank() == 0) {
        auto local_result = benchmark_operation(
            [&] {
                auto op =
                    loom::rma_async(*setup.endpoint)
                        .compare_swap<std::uint64_t>(remote_addr, 0, 1, remote_key, setup.cq.get());
                auto [old_val] = stdexec::sync_wait(std::move(op)).value();

                auto reset_op =
                    loom::rma_async(*setup.endpoint)
                        .compare_swap<std::uint64_t>(remote_addr, 1, 0, remote_key, setup.cq.get());
                stdexec::sync_wait(std::move(reset_op));
            },
            500);

        auto global_result = benchmark_result::aggregate(mpi, local_result);
        global_result.print("atomic_compare_swap");

    } else if (mpi.rank() == 1) {
        benchmark_result dummy{};
        benchmark_result::aggregate(mpi, dummy);
    }

    MPI_Barrier(MPI_COMM_WORLD);
}

void benchmark_composition_overhead(const mpi_context& mpi, fabric_setup& setup) {
    if (mpi.is_root()) {
        std::cout << "\n=== Sender Composition Overhead ===\n";
    }

    std::array<std::byte, 64> buffer{};

    if (mpi.rank() == 0) {
        auto bare_result = benchmark_operation(
            [&] {
                auto op = loom::async(*setup.endpoint).send(buffer, setup.cq.get());
                stdexec::sync_wait(std::move(op));
            },
            1000);

        auto then_result = benchmark_operation(
            [&] {
                auto op = loom::async(*setup.endpoint).send(buffer, setup.cq.get()) |
                          stdexec::then([] {});
                stdexec::sync_wait(std::move(op));
            },
            1000);

        auto let_value_result = benchmark_operation(
            [&] {
                auto op = loom::async(*setup.endpoint).send(buffer, setup.cq.get()) |
                          stdexec::let_value([&] { return stdexec::just(); });
                stdexec::sync_wait(std::move(op));
            },
            1000);

        auto global_bare = benchmark_result::aggregate(mpi, bare_result);
        auto global_then = benchmark_result::aggregate(mpi, then_result);
        auto global_let_value = benchmark_result::aggregate(mpi, let_value_result);

        global_bare.print("bare send");
        global_then.print("send | then");
        global_let_value.print("send | let_value");

        std::cout << std::format("  then overhead: +{:d}ns\n",
                                 (global_then.mean - global_bare.mean).count());
        std::cout << std::format("  let_value overhead: +{:d}ns\n",
                                 (global_let_value.mean - global_bare.mean).count());

    } else if (mpi.rank() == 1) {
        for (int i = 0; i < 3; ++i) {
            auto local_result = benchmark_operation(
                [&] {
                    auto op = loom::async(*setup.endpoint).recv(buffer, setup.cq.get());
                    stdexec::sync_wait(std::move(op));
                },
                1000);
            benchmark_result::aggregate(mpi, local_result);
        }
    }

    MPI_Barrier(MPI_COMM_WORLD);
}

void benchmark_throughput(const mpi_context& mpi, fabric_setup& setup) {
    if (mpi.is_root()) {
        std::cout << "\n=== Throughput Benchmark ===\n";
    }

    constexpr std::size_t num_ops = 10000;
    std::array<std::byte, 64> buffer{};

    if (mpi.rank() == 0) {
        auto start = clock_type::now();

        for (std::size_t i = 0; i < num_ops; ++i) {
            auto op = loom::async(*setup.endpoint).send(buffer, setup.cq.get());
            stdexec::sync_wait(std::move(op));
        }

        auto end = clock_type::now();
        auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end - start);
        auto ops_per_sec = (num_ops * 1'000'000) / duration.count();

        std::cout << std::format(
            "Throughput: {} ops/sec ({} us for {} ops)\n", ops_per_sec, duration.count(), num_ops);
        std::cout << std::format("Per-op: {} ns\n", duration.count() * 1000 / num_ops);

    } else if (mpi.rank() == 1) {
        for (std::size_t i = 0; i < num_ops; ++i) {
            auto op = loom::async(*setup.endpoint).recv(buffer, setup.cq.get());
            stdexec::sync_wait(std::move(op));
        }
    }

    MPI_Barrier(MPI_COMM_WORLD);
}

void benchmark_parallel_ops(const mpi_context& mpi, fabric_setup& setup) {
    if (mpi.is_root()) {
        std::cout << "\n=== Parallel Operations (when_all) ===\n";
    }

    std::array<std::byte, 64> buf1{}, buf2{}, buf3{}, buf4{};

    if (mpi.rank() == 0) {
        auto seq_result = benchmark_operation(
            [&] {
                auto op1 = loom::async(*setup.endpoint).send(buf1, setup.cq.get());
                stdexec::sync_wait(std::move(op1));
                auto op2 = loom::async(*setup.endpoint).send(buf2, setup.cq.get());
                stdexec::sync_wait(std::move(op2));
                auto op3 = loom::async(*setup.endpoint).send(buf3, setup.cq.get());
                stdexec::sync_wait(std::move(op3));
                auto op4 = loom::async(*setup.endpoint).send(buf4, setup.cq.get());
                stdexec::sync_wait(std::move(op4));
            },
            200);

        auto par_result = benchmark_operation(
            [&] {
                auto op =
                    stdexec::when_all(loom::async(*setup.endpoint).send(buf1, setup.cq.get()),
                                      loom::async(*setup.endpoint).send(buf2, setup.cq.get()),
                                      loom::async(*setup.endpoint).send(buf3, setup.cq.get()),
                                      loom::async(*setup.endpoint).send(buf4, setup.cq.get()));
                stdexec::sync_wait(std::move(op));
            },
            200);

        auto global_seq = benchmark_result::aggregate(mpi, seq_result);
        auto global_par = benchmark_result::aggregate(mpi, par_result);

        global_seq.print("4 sends (sequential)");
        global_par.print("4 sends (when_all)");

        auto speedup = static_cast<double>(global_seq.mean.count()) / global_par.mean.count();
        std::cout << std::format("  Speedup: {:.2f}x\n", speedup);

    } else if (mpi.rank() == 1) {
        for (int bench = 0; bench < 2; ++bench) {
            auto local_result = benchmark_operation(
                [&] {
                    for (int i = 0; i < 4; ++i) {
                        auto op = loom::async(*setup.endpoint).recv(buf1, setup.cq.get());
                        stdexec::sync_wait(std::move(op));
                    }
                },
                200);
            benchmark_result::aggregate(mpi, local_result);
        }
    }

    MPI_Barrier(MPI_COMM_WORLD);
}

void benchmark_memory_overhead(const mpi_context& mpi) {
    if (mpi.is_root()) {
        std::cout << "\n=== Memory Overhead ===\n";

        std::cout << std::format("sizeof(send_sender):          {} bytes\n",
                                 sizeof(loom::async::send_sender));
        std::cout << std::format("sizeof(recv_sender):          {} bytes\n",
                                 sizeof(loom::async::recv_sender));
        std::cout << std::format("sizeof(read_sender):          {} bytes\n",
                                 sizeof(loom::async::read_sender));
        std::cout << std::format("sizeof(write_sender):         {} bytes\n",
                                 sizeof(loom::async::write_sender));
        std::cout << std::format("sizeof(atomic_sender<u64>):   {} bytes\n",
                                 sizeof(loom::async::atomic_sender<std::uint64_t>));
        std::cout << std::format("sizeof(scheduler):            {} bytes\n",
                                 sizeof(loom::scheduler));
    }
}

auto main(int argc, char** argv) -> int {
    mpi_context mpi(&argc, &argv);

    if (mpi.size() < 2) {
        if (mpi.is_root()) {
            std::cerr << "This benchmark requires at least 2 MPI ranks\n";
            std::cerr << "Run with: mpirun -np 2 ./benchmark_async\n";
        }
        return 1;
    }

    try {
        auto setup = fabric_setup::create(mpi);

        if (mpi.is_root()) {
            std::cout << "=== Loom Async Performance Benchmarks ===\n";
            std::cout << std::format("Running on {} MPI ranks\n", mpi.size());
            std::cout << "Measuring actual network operations\n\n";
        }

        benchmark_send_latency(mpi, setup);
        benchmark_recv_latency(mpi, setup);
        benchmark_ping_pong(mpi, setup);
        benchmark_rdma_read(mpi, setup);
        benchmark_rdma_write(mpi, setup);
        benchmark_atomic_ops(mpi, setup);
        benchmark_composition_overhead(mpi, setup);
        benchmark_throughput(mpi, setup);
        benchmark_parallel_ops(mpi, setup);
        benchmark_memory_overhead(mpi);

        if (mpi.is_root()) {
            std::cout << "\n=== Benchmarks Complete ===\n";
            std::cout << "\nKey Observations:\n";
            std::cout << "- Send/recv latency depends on fabric provider and network\n";
            std::cout << "- RDMA operations bypass CPU on target (one-sided)\n";
            std::cout << "- Atomics provide lock-free synchronization\n";
            std::cout << "- Sender composition adds minimal overhead\n";
            std::cout << "- when_all enables parallel operation submission\n";
        }

    } catch (const std::exception& e) {
        std::cerr << "Rank " << mpi.rank() << " error: " << e.what() << "\n";
        MPI_Abort(MPI_COMM_WORLD, 1);
    }

    return 0;
}
