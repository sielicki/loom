// SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only

#include <loom/execution.hpp>
#include <loom/loom.hpp>

#include <array>
#include <cstdint>
#include <memory>
#include <ranges>
#include <vector>

#include "ut.hpp"
#include <stdexec/execution.hpp>

using namespace boost::ut;

suite async_senders_suite = [] {
    "scheduler concept"_test = [] {
        static_assert(stdexec::scheduler<loom::scheduler>,
                      "loom::scheduler must satisfy scheduler");
    };

    "send sender concept"_test = [] {
        static_assert(stdexec::sender<loom::execution::send_sender>,
                      "send_sender must satisfy sender");
    };

    "recv sender concept"_test = [] {
        static_assert(stdexec::sender<loom::execution::recv_sender>,
                      "recv_sender must satisfy sender");
    };

    "rma senders concept"_test = [] {
        static_assert(stdexec::sender<loom::execution::read_sender>,
                      "read_sender must satisfy sender");
        static_assert(stdexec::sender<loom::execution::write_sender>,
                      "write_sender must satisfy sender");
        static_assert(stdexec::sender<loom::execution::fetch_add_sender<std::uint64_t>>,
                      "fetch_add_sender must satisfy sender");
        static_assert(stdexec::sender<loom::execution::compare_swap_sender<std::uint64_t>>,
                      "compare_swap_sender must satisfy sender");
    };

    "sender composition when_all"_test = [] {
        loom::endpoint ep{nullptr};
        loom::completion_queue cq{nullptr};
        std::array<std::byte, 64> buf1{};
        std::array<std::byte, 64> buf2{};

        auto composed =
            stdexec::when_all(loom::async(ep).send(buf1, &cq), loom::async(ep).send(buf2, &cq));

        static_assert(stdexec::sender<decltype(composed)>, "when_all result must be a sender");
    };

    "sender composition then"_test = [] {
        loom::endpoint ep{nullptr};
        loom::completion_queue cq{nullptr};
        std::array<std::byte, 64> buffer{};

        auto composed = loom::async(ep).send(buffer, &cq) | stdexec::then([] { return 42; });

        static_assert(stdexec::sender<decltype(composed)>, "then result must be a sender");
    };

    "sender composition let_value"_test = [] {
        loom::endpoint ep{nullptr};
        loom::completion_queue cq{nullptr};
        std::array<std::byte, 64> send_buf{};
        std::array<std::byte, 64> recv_buf{};

        auto composed = loom::async(ep).send(send_buf, &cq) |
                        stdexec::let_value([&] { return loom::async(ep).recv(recv_buf, &cq); });

        static_assert(stdexec::sender<decltype(composed)>, "let_value result must be a sender");
    };

    "error handling"_test = [] {
        loom::endpoint ep{nullptr};
        loom::completion_queue cq{nullptr};
        std::array<std::byte, 64> buffer{};

        auto with_error_handler =
            loom::async(ep).send(buffer, &cq) |
            stdexec::upon_error([](std::error_code ec) { return ec.value(); });

        static_assert(stdexec::sender<decltype(with_error_handler)>,
                      "upon_error result must be a sender");
    };

    "scheduler schedule"_test = [] {
        loom::completion_queue cq{nullptr};
        loom::scheduler sched{std::shared_ptr<loom::completion_queue>(&cq, [](auto*) {})};

        auto schedule_sender = stdexec::schedule(sched);

        static_assert(stdexec::sender<decltype(schedule_sender)>, "schedule result must be sender");
    };

    "bulk operations"_test = [] {
        loom::endpoint ep{nullptr};
        loom::completion_queue cq{nullptr};

        std::vector<std::array<std::byte, 64>> buffers(10);

        auto bulk_send = stdexec::just(std::views::iota(0, 10)) | stdexec::bulk(10, [&](int i) {
                             return loom::async(ep).send(buffers[i], &cq);
                         });

        static_assert(stdexec::sender<decltype(bulk_send)>, "bulk result must be a sender");
    };

    "rma read-modify-write chain"_test = [] {
        loom::endpoint ep{nullptr};
        loom::completion_queue cq{nullptr};
        std::array<std::byte, 128> buffer{};
        loom::rma_addr addr{0x1000ULL};
        loom::mr_key key{42ULL};

        auto rmw = loom::rma_async(ep).read(buffer, addr, key, &cq) |
                   stdexec::then([&] { return buffer; }) | stdexec::let_value([&](auto& buf) {
                       return loom::rma_async(ep).write(buf, addr, key, &cq);
                   });

        static_assert(stdexec::sender<decltype(rmw)>, "RMW chain must be a sender");
    };

    "atomic fetch_add operations"_test = [] {
        loom::endpoint ep{nullptr};
        loom::completion_queue cq{nullptr};
        loom::rma_addr addr{0x2000ULL};
        loom::mr_key key{99ULL};
        loom::memory_region* result_mr = nullptr;

        auto atomic_op =
            loom::rma_async(ep).fetch_add<std::uint64_t>(addr, 1, key, result_mr, &cq) |
            stdexec::then([](std::uint64_t old_val) { return old_val + 1; });

        static_assert(stdexec::sender<decltype(atomic_op)>, "fetch_add must be a sender");
    };

    "atomic compare_swap operations"_test = [] {
        loom::endpoint ep{nullptr};
        loom::completion_queue cq{nullptr};
        loom::rma_addr addr{0x3000ULL};
        loom::mr_key key{100ULL};
        loom::memory_region* result_mr = nullptr;

        auto cas_op =
            loom::rma_async(ep).compare_swap<std::uint64_t>(addr, 0, 1, key, result_mr, &cq) |
            stdexec::then([](std::uint64_t old_val) { return old_val == 0; });

        static_assert(stdexec::sender<decltype(cas_op)>, "compare_swap must be a sender");
    };

    "recv return type"_test = [] {
        loom::endpoint ep{nullptr};
        loom::completion_queue cq{nullptr};
        std::array<std::byte, 64> buffer{};

        auto recv_op = loom::async(ep).recv(buffer, &cq) | stdexec::then([](std::size_t bytes) {
                           static_assert(std::same_as<decltype(bytes), std::size_t>,
                                         "recv must return std::size_t");
                           return bytes;
                       });

        static_assert(stdexec::sender<decltype(recv_op)>, "recv_op must be a sender");
    };

    "tagged operations"_test = [] {
        loom::endpoint ep{nullptr};
        loom::completion_queue cq{nullptr};
        std::array<std::byte, 64> buffer{};

        auto tagged = loom::async(ep).tagged_send(buffer, 0x1234, &cq);

        static_assert(stdexec::sender<decltype(tagged)>, "tagged_send must be a sender");
    };

    "constexpr properties"_test = [] {
        constexpr auto msg_reliable = loom::is_reliable(loom::endpoint_types::msg);
        static_assert(msg_reliable, "MSG endpoints must be reliable");

        constexpr auto dgram_unreliable = !loom::is_reliable(loom::endpoint_types::dgram);
        static_assert(dgram_unreliable, "DGRAM endpoints must be unreliable");
    };
};

auto main() -> int {}
