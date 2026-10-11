// SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only

#include <loom/async.hpp>
#include <loom/execution.hpp>
#include <loom/loom.hpp>

#include <array>
#include <cstdint>
#include <execution>
#include <memory>
#include <ranges>
#include <stop_token>
#include <string_view>
#include <tuple>
#include <vector>

#include <catch2/catch_test_macros.hpp>

TEST_CASE("execution backend matches the requested implementation", "[execution]") {
#if defined(LOOM_EXECUTION_USE_STDEXEC)
    REQUIRE(std::string_view(loom::execution::execution_backend_name) == "stdexec");
#elif defined(LOOM_EXECUTION_USE_BEMAN)
    REQUIRE(std::string_view(loom::execution::execution_backend_name) == "beman::execution");
#elif defined(LOOM_EXECUTION_USE_STD)
    REQUIRE(std::string_view(loom::execution::execution_backend_name) == "std::execution");
#endif
}

TEST_CASE("portable algorithms deliver values and errors", "[execution]") {
    namespace ex = loom::execution;
    auto value = ex::sync_wait(ex::just(20) | ex::then([](int i) { return i + 1; }));
    REQUIRE(value.has_value());
    REQUIRE(std::get<0>(*value) == 21);

    loom::endpoint ep;
    loom::completion_queue cq;
    std::array<std::byte, 16> buffer{};
    auto error = ex::sync_wait(loom::async(ep).send(buffer, &cq) |
                               ex::then([]() noexcept { return std::error_code{}; }) |
                               ex::upon_error([](std::error_code ec) { return ec; }));
    REQUIRE(error.has_value());
    REQUIRE(std::get<0>(*error) == std::errc::bad_file_descriptor);
}

TEST_CASE("execution queries preserve the requested stop token", "[execution]") {
#if defined(LOOM_EXECUTION_BACKEND_STD)
    std::inplace_stop_source source;
#else
    loom::execution::impl::inplace_stop_source source;
#endif
    using stop_token = decltype(source.get_token());
    struct environment {
        stop_token token;
        auto query(decltype(loom::execution::get_stop_token)) const noexcept -> stop_token {
            return token;
        }
    };
    const auto token = loom::execution::get_stop_token(environment{source.get_token()});
    REQUIRE(token.stop_possible());
    REQUIRE_FALSE(token.stop_requested());
    source.request_stop();
    REQUIRE(token.stop_requested());
    REQUIRE_FALSE(loom::execution::get_stop_token(loom::execution::fabric_env{}).stop_possible());
}

TEST_CASE("schedule completes inline", "[execution]") {
    bool completed{};
    auto operation = loom::execution::schedule(loom::scheduler{}) |
                     loom::execution::then([&] { completed = true; });
    REQUIRE_FALSE(completed);
    REQUIRE(loom::execution::sync_wait(std::move(operation)).has_value());
    REQUIRE(completed);
}

TEST_CASE("scheduler concept", "[execution][senders]") {
    static_assert(loom::execution::scheduler<loom::scheduler>,
                  "loom::scheduler must satisfy scheduler");
}

TEST_CASE("send sender concept", "[execution][senders]") {
    static_assert(loom::execution::sender<loom::execution::send_sender>,
                  "send_sender must satisfy sender");
}

TEST_CASE("recv sender concept", "[execution][senders]") {
    static_assert(loom::execution::sender<loom::execution::recv_sender>,
                  "recv_sender must satisfy sender");
}

TEST_CASE("rma senders concept", "[execution][senders]") {
    static_assert(loom::execution::sender<loom::execution::read_sender>,
                  "read_sender must satisfy sender");
    static_assert(loom::execution::sender<loom::execution::write_sender>,
                  "write_sender must satisfy sender");
    static_assert(loom::execution::sender<loom::execution::fetch_add_sender<std::uint64_t>>,
                  "fetch_add_sender must satisfy sender");
    static_assert(loom::execution::sender<loom::execution::compare_swap_sender<std::uint64_t>>,
                  "compare_swap_sender must satisfy sender");
}

TEST_CASE("sender composition when_all", "[execution][senders]") {
    loom::endpoint ep{};
    loom::completion_queue cq{};
    std::array<std::byte, 64> buf1{};
    std::array<std::byte, 64> buf2{};

    auto composed =
        loom::execution::when_all(loom::async(ep).send(buf1, &cq), loom::async(ep).send(buf2, &cq));

    static_assert(loom::execution::sender<decltype(composed)>, "when_all result must be a sender");
}

TEST_CASE("sender composition then", "[execution][senders]") {
    loom::endpoint ep{};
    loom::completion_queue cq{};
    std::array<std::byte, 64> buffer{};

    auto composed = loom::async(ep).send(buffer, &cq) | loom::execution::then([] { return 42; });

    static_assert(loom::execution::sender<decltype(composed)>, "then result must be a sender");
}

TEST_CASE("sender composition let_value", "[execution][senders]") {
    loom::endpoint ep{};
    loom::completion_queue cq{};
    std::array<std::byte, 64> send_buf{};
    std::array<std::byte, 64> recv_buf{};

    auto composed = loom::async(ep).send(send_buf, &cq) |
                    loom::execution::let_value([&] { return loom::async(ep).recv(recv_buf, &cq); });

    static_assert(loom::execution::sender<decltype(composed)>, "let_value result must be a sender");
}

TEST_CASE("error handling", "[execution][senders]") {
    loom::endpoint ep{};
    loom::completion_queue cq{};
    std::array<std::byte, 64> buffer{};

    auto with_error_handler =
        loom::async(ep).send(buffer, &cq) |
        loom::execution::upon_error([](std::error_code ec) { return ec.value(); });

    static_assert(loom::execution::sender<decltype(with_error_handler)>,
                  "upon_error result must be a sender");
}

TEST_CASE("scheduler schedule", "[execution][senders]") {
    loom::completion_queue cq{};
    loom::scheduler sched{std::shared_ptr<loom::completion_queue>(&cq, [](auto*) {})};

    auto schedule_sender = loom::execution::schedule(sched);

    static_assert(loom::execution::sender<decltype(schedule_sender)>,
                  "schedule result must be sender");
}

TEST_CASE("bulk operations", "[execution][senders]") {
    loom::endpoint ep{};
    loom::completion_queue cq{};

    std::vector<std::array<std::byte, 64>> buffers(10);

    auto bulk_send =
        loom::execution::just() | loom::execution::bulk(std::execution::seq, 10, [&](int i) {
            (void)loom::async(ep).send(buffers[static_cast<std::size_t>(i)], &cq);
        });

    static_assert(loom::execution::sender<decltype(bulk_send)>, "bulk result must be a sender");
}

TEST_CASE("rma read-modify-write chain", "[execution][senders]") {
    loom::endpoint ep{};
    loom::completion_queue cq{};
    std::array<std::byte, 128> buffer{};
    loom::rma_addr addr{0x1000ULL};
    loom::mr_key key{42ULL};

    auto rmw = loom::rma_async(ep).read(buffer, addr, key, &cq) |
               loom::execution::then([&](std::size_t) { return buffer; }) |
               loom::execution::let_value(
                   [&](auto& buf) { return loom::rma_async(ep).write(buf, addr, key, &cq); });

    static_assert(loom::execution::sender<decltype(rmw)>, "RMW chain must be a sender");
}

TEST_CASE("atomic fetch_add operations", "[execution][senders]") {
    loom::endpoint ep{};
    loom::completion_queue cq{};
    loom::rma_addr addr{0x2000ULL};
    loom::mr_key key{99ULL};
    loom::memory_region* result_mr = nullptr;

    auto atomic_op = loom::rma_async(ep).fetch_add<std::uint64_t>(addr, 1, key, result_mr, &cq) |
                     loom::execution::then([](std::uint64_t old_val) { return old_val + 1; });

    static_assert(loom::execution::sender<decltype(atomic_op)>, "fetch_add must be a sender");
}

TEST_CASE("atomic compare_swap operations", "[execution][senders]") {
    loom::endpoint ep{};
    loom::completion_queue cq{};
    loom::rma_addr addr{0x3000ULL};
    loom::mr_key key{100ULL};
    loom::memory_region* result_mr = nullptr;

    auto cas_op = loom::rma_async(ep).compare_swap<std::uint64_t>(addr, 0, 1, key, result_mr, &cq) |
                  loom::execution::then([](std::uint64_t old_val) { return old_val == 0; });

    static_assert(loom::execution::sender<decltype(cas_op)>, "compare_swap must be a sender");
}

TEST_CASE("recv return type", "[execution][senders]") {
    loom::endpoint ep{};
    loom::completion_queue cq{};
    std::array<std::byte, 64> buffer{};

    auto recv_op = loom::async(ep).recv(buffer, &cq) | loom::execution::then([](std::size_t bytes) {
                       static_assert(std::same_as<decltype(bytes), std::size_t>,
                                     "recv must return std::size_t");
                       return bytes;
                   });

    static_assert(loom::execution::sender<decltype(recv_op)>, "recv_op must be a sender");
}

TEST_CASE("tagged operations", "[execution][senders]") {
    loom::endpoint ep{};
    loom::completion_queue cq{};
    std::array<std::byte, 64> buffer{};

    auto tagged = loom::async(ep).tagged_send(buffer, 0x1234, &cq);

    static_assert(loom::execution::sender<decltype(tagged)>, "tagged_send must be a sender");
}

TEST_CASE("constexpr properties", "[execution][senders]") {
    constexpr auto msg_reliable = loom::is_reliable(loom::endpoint_types::msg);
    static_assert(msg_reliable, "MSG endpoints must be reliable");

    constexpr auto dgram_unreliable = !loom::is_reliable(loom::endpoint_types::dgram);
    static_assert(dgram_unreliable, "DGRAM endpoints must be unreliable");
}
