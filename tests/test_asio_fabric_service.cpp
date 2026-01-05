// SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only
/**
 * @file test_asio_fabric_service.cpp
 * @brief Tests for loom::asio::fabric_service.
 */

#include <loom/asio.hpp>
#include <loom/loom.hpp>

#include <chrono>
#include <thread>

#include <asio/io_context.hpp>
#include <asio/steady_timer.hpp>
#include <catch2/catch_test_macros.hpp>

using namespace std::literals;

TEST_CASE("fabric_service creation", "[asio][fabric_service]") {
    ::asio::io_context ioc;

    REQUIRE_FALSE(loom::asio::fabric_service::has_service(ioc));

    [[maybe_unused]] auto& service = loom::asio::fabric_service::use(ioc);

    REQUIRE(loom::asio::fabric_service::has_service(ioc));
}

TEST_CASE("fabric_service use returns same instance", "[asio][fabric_service]") {
    ::asio::io_context ioc;

    auto& service1 = loom::asio::fabric_service::use(ioc);
    auto& service2 = loom::asio::fabric_service::use(ioc);

    REQUIRE(&service1 == &service2);
}

TEST_CASE("fabric_service start and stop", "[asio][fabric_service]") {
    ::asio::io_context ioc;
    auto& service = loom::asio::fabric_service::use(ioc);

    REQUIRE_FALSE(service.is_running());

    service.start();
    REQUIRE(service.is_running());

    service.stop();
    REQUIRE_FALSE(service.is_running());
}

TEST_CASE("fabric_service register and deregister cq", "[asio][fabric_service][.provider]") {
    loom::fabric_hints hints{};
    hints.ep_type = loom::endpoint_types::rdm;

    auto info_result = loom::query_fabric(hints);
    if (!info_result) {
        SKIP("No fabric provider available");
    }

    auto fabric_result = loom::fabric::create(*info_result);
    REQUIRE(fabric_result);

    auto domain_result = loom::domain::create(*fabric_result, *info_result);
    REQUIRE(domain_result);

    auto cq_result =
        loom::completion_queue::create(*domain_result, {.size = loom::queue_size{64UL}});
    REQUIRE(cq_result);

    ::asio::io_context ioc;
    auto& service = loom::asio::fabric_service::use(ioc);

    service.register_cq(*cq_result);
    service.deregister_cq(*cq_result);
}

TEST_CASE("fabric_service poll_once with no completions", "[asio][fabric_service][.provider]") {
    loom::fabric_hints hints{};
    hints.ep_type = loom::endpoint_types::rdm;

    auto info_result = loom::query_fabric(hints);
    if (!info_result) {
        SKIP("No fabric provider available");
    }

    auto fabric_result = loom::fabric::create(*info_result);
    REQUIRE(fabric_result);

    auto domain_result = loom::domain::create(*fabric_result, *info_result);
    REQUIRE(domain_result);

    auto cq_result =
        loom::completion_queue::create(*domain_result, {.size = loom::queue_size{64UL}});
    REQUIRE(cq_result);

    ::asio::io_context ioc;
    auto& service = loom::asio::fabric_service::use(ioc);

    service.register_cq(*cq_result);

    auto count = service.poll_once();
    REQUIRE(count == 0UL);

    service.deregister_cq(*cq_result);
}

TEST_CASE("fabric_service timer-based polling runs", "[asio][fabric_service][.provider]") {
    loom::fabric_hints hints{};
    hints.ep_type = loom::endpoint_types::rdm;

    auto info_result = loom::query_fabric(hints);
    if (!info_result) {
        SKIP("No fabric provider available");
    }

    auto fabric_result = loom::fabric::create(*info_result);
    REQUIRE(fabric_result);

    auto domain_result = loom::domain::create(*fabric_result, *info_result);
    REQUIRE(domain_result);

    auto cq_result =
        loom::completion_queue::create(*domain_result, {.size = loom::queue_size{64UL}});
    REQUIRE(cq_result);

    ::asio::io_context ioc;
    auto& service = loom::asio::fabric_service::use(ioc);

    loom::asio::fabric_service::options opts{
        .poll_interval = std::chrono::microseconds{1000},
        .max_completions_per_poll = 16,
    };
    service.register_cq(*cq_result, opts);
    service.start();

    ::asio::steady_timer timer(ioc);
    timer.expires_after(5ms);
    timer.async_wait([&service](const std::error_code&) { service.stop(); });

    ioc.run();

    REQUIRE_FALSE(service.is_running());
    service.deregister_cq(*cq_result);
}

TEST_CASE("fabric_service with multiple completion queues", "[asio][fabric_service][.provider]") {
    loom::fabric_hints hints{};
    hints.ep_type = loom::endpoint_types::rdm;

    auto info_result = loom::query_fabric(hints);
    if (!info_result) {
        SKIP("No fabric provider available");
    }

    auto fabric_result = loom::fabric::create(*info_result);
    REQUIRE(fabric_result);

    auto domain_result = loom::domain::create(*fabric_result, *info_result);
    REQUIRE(domain_result);

    auto cq1_result =
        loom::completion_queue::create(*domain_result, {.size = loom::queue_size{32UL}});
    auto cq2_result =
        loom::completion_queue::create(*domain_result, {.size = loom::queue_size{32UL}});
    REQUIRE(cq1_result);
    REQUIRE(cq2_result);

    ::asio::io_context ioc;
    auto& service = loom::asio::fabric_service::use(ioc);

    service.register_cq(*cq1_result);
    service.register_cq(*cq2_result);

    auto count = service.poll_once();
    REQUIRE(count == 0UL);

    service.deregister_cq(*cq1_result);
    service.deregister_cq(*cq2_result);
}
