// SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only
/**
 * @file test_asio_fabric_service.cpp
 * @brief Tests for loom::asio::fabric_service.
 */

#include <loom/asio.hpp>
#include <loom/loom.hpp>

#include <chrono>
#include <thread>

#include "ut.hpp"
#include <asio/io_context.hpp>
#include <asio/steady_timer.hpp>

using namespace boost::ut;
using namespace std::literals;

suite fabric_service_suite = [] {
    "fabric_service creation"_test = [] {
        ::asio::io_context ioc;

        expect(!loom::asio::fabric_service::has_service(ioc));

        [[maybe_unused]] auto& service = loom::asio::fabric_service::use(ioc);

        expect(loom::asio::fabric_service::has_service(ioc));
    };

    "fabric_service use returns same instance"_test = [] {
        ::asio::io_context ioc;

        auto& service1 = loom::asio::fabric_service::use(ioc);
        auto& service2 = loom::asio::fabric_service::use(ioc);

        expect(&service1 == &service2);
    };

    "fabric_service start and stop"_test = [] {
        ::asio::io_context ioc;
        auto& service = loom::asio::fabric_service::use(ioc);

        expect(!service.is_running());

        service.start();
        expect(service.is_running());

        service.stop();
        expect(!service.is_running());
    };

    skip / "fabric_service register and deregister cq"_test = [] {
        loom::fabric_hints hints{};
        hints.ep_type = loom::endpoint_types::rdm;

        auto info_result = loom::query_fabric(hints);
        expect(static_cast<bool>(info_result)) << "No fabric provider available";

        auto fabric_result = loom::fabric::create(*info_result);
        expect(static_cast<bool>(fabric_result)) << "Failed to create fabric";

        auto domain_result = loom::domain::create(*fabric_result, *info_result);
        expect(static_cast<bool>(domain_result)) << "Failed to create domain";

        auto cq_result =
            loom::completion_queue::create(*domain_result, {.size = loom::queue_size{64UL}});
        expect(static_cast<bool>(cq_result)) << "Failed to create completion queue";

        ::asio::io_context ioc;
        auto& service = loom::asio::fabric_service::use(ioc);

        service.register_cq(*cq_result);
        service.deregister_cq(*cq_result);
    };

    skip / "fabric_service poll_once with no completions"_test = [] {
        loom::fabric_hints hints{};
        hints.ep_type = loom::endpoint_types::rdm;

        auto info_result = loom::query_fabric(hints);
        expect(static_cast<bool>(info_result)) << "No fabric provider available";

        auto fabric_result = loom::fabric::create(*info_result);
        expect(static_cast<bool>(fabric_result)) << "Failed to create fabric";

        auto domain_result = loom::domain::create(*fabric_result, *info_result);
        expect(static_cast<bool>(domain_result)) << "Failed to create domain";

        auto cq_result =
            loom::completion_queue::create(*domain_result, {.size = loom::queue_size{64UL}});
        expect(static_cast<bool>(cq_result)) << "Failed to create completion queue";

        ::asio::io_context ioc;
        auto& service = loom::asio::fabric_service::use(ioc);

        service.register_cq(*cq_result);

        auto count = service.poll_once();
        expect(count == 0_ul);

        service.deregister_cq(*cq_result);
    };

    skip / "fabric_service timer-based polling runs"_test = [] {
        loom::fabric_hints hints{};
        hints.ep_type = loom::endpoint_types::rdm;

        auto info_result = loom::query_fabric(hints);
        expect(static_cast<bool>(info_result)) << "No fabric provider available";

        auto fabric_result = loom::fabric::create(*info_result);
        expect(static_cast<bool>(fabric_result)) << "Failed to create fabric";

        auto domain_result = loom::domain::create(*fabric_result, *info_result);
        expect(static_cast<bool>(domain_result)) << "Failed to create domain";

        auto cq_result =
            loom::completion_queue::create(*domain_result, {.size = loom::queue_size{64UL}});
        expect(static_cast<bool>(cq_result)) << "Failed to create completion queue";

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

        expect(!service.is_running());
        service.deregister_cq(*cq_result);
    };

    skip / "fabric_service with multiple completion queues"_test = [] {
        loom::fabric_hints hints{};
        hints.ep_type = loom::endpoint_types::rdm;

        auto info_result = loom::query_fabric(hints);
        expect(static_cast<bool>(info_result)) << "No fabric provider available";

        auto fabric_result = loom::fabric::create(*info_result);
        expect(static_cast<bool>(fabric_result)) << "Failed to create fabric";

        auto domain_result = loom::domain::create(*fabric_result, *info_result);
        expect(static_cast<bool>(domain_result)) << "Failed to create domain";

        auto cq1_result =
            loom::completion_queue::create(*domain_result, {.size = loom::queue_size{32UL}});
        auto cq2_result =
            loom::completion_queue::create(*domain_result, {.size = loom::queue_size{32UL}});
        expect(static_cast<bool>(cq1_result) && static_cast<bool>(cq2_result))
            << "Failed to create completion queues";

        ::asio::io_context ioc;
        auto& service = loom::asio::fabric_service::use(ioc);

        service.register_cq(*cq1_result);
        service.register_cq(*cq2_result);

        auto count = service.poll_once();
        expect(count == 0_ul);

        service.deregister_cq(*cq1_result);
        service.deregister_cq(*cq2_result);
    };
};

auto main() -> int {
    return 0;
}
