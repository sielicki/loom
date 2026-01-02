// SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only
/**
 * @file test_asio_registered_buffer.cpp
 * @brief Tests for loom::asio buffer registration.
 */

#include <loom/asio.hpp>
#include <loom/loom.hpp>

#include <array>

#include "ut.hpp"
#include <asio/io_context.hpp>

using namespace boost::ut;
using namespace std::literals;

suite registered_buffer_id_suite = [] {
    "registered_buffer_id default is invalid"_test = [] {
        loom::asio::registered_buffer_id id;
        expect(!id.is_valid());
    };

    "registered_buffer_id with value is valid"_test = [] {
        loom::asio::registered_buffer_id id{42};
        expect(id.is_valid());
        expect(id.native_handle() == 42_ul);
    };

    "registered_buffer_id equality"_test = [] {
        loom::asio::registered_buffer_id id1{1};
        loom::asio::registered_buffer_id id2{1};
        loom::asio::registered_buffer_id id3{2};

        expect(id1 == id2);
        expect(id1 != id3);
    };
};

suite mutable_registered_buffer_suite = [] {
    "mutable_registered_buffer default construction"_test = [] {
        loom::asio::mutable_registered_buffer buf;
        expect(buf.data() == nullptr);
        expect(buf.size() == 0_ul);
        expect(buf.memory_region() == nullptr);
        expect(!buf.id().is_valid());
    };

    "mutable_registered_buffer offset operator"_test = [] {
        std::array<std::byte, 64> storage{};
        ::asio::mutable_buffer asio_buf{storage.data(), storage.size()};

        loom::asio::mutable_registered_buffer buf{
            asio_buf, nullptr, loom::asio::registered_buffer_id{0}};

        auto offset_buf = buf + 16;
        expect(offset_buf.size() == 48_ul);
        expect(offset_buf.data() == storage.data() + 16);
    };

    "mutable_registered_buffer buffer_sequence interface"_test = [] {
        std::array<std::byte, 64> storage{};
        ::asio::mutable_buffer asio_buf{storage.data(), storage.size()};

        loom::asio::mutable_registered_buffer buf{
            asio_buf, nullptr, loom::asio::registered_buffer_id{0}};

        auto begin = loom::asio::buffer_sequence_begin(buf);
        auto end = loom::asio::buffer_sequence_end(buf);

        expect(end - begin == 1);
        expect(begin->data() == storage.data());
        expect(begin->size() == 64_ul);
    };
};

suite const_registered_buffer_suite = [] {
    "const_registered_buffer from mutable"_test = [] {
        std::array<std::byte, 64> storage{};
        ::asio::mutable_buffer asio_buf{storage.data(), storage.size()};

        loom::asio::mutable_registered_buffer mut_buf{
            asio_buf, nullptr, loom::asio::registered_buffer_id{5}};

        loom::asio::const_registered_buffer const_buf{mut_buf};

        expect(const_buf.data() == storage.data());
        expect(const_buf.size() == 64_ul);
        expect(const_buf.id().native_handle() == 5_ul);
    };

    "const_registered_buffer buffer_sequence interface"_test = [] {
        std::array<std::byte, 64> storage{};
        ::asio::const_buffer asio_buf{storage.data(), storage.size()};

        loom::asio::const_registered_buffer buf{
            asio_buf, nullptr, loom::asio::registered_buffer_id{0}};

        auto begin = loom::asio::buffer_sequence_begin(buf);
        auto end = loom::asio::buffer_sequence_end(buf);

        expect(end - begin == 1);
        expect(begin->data() == storage.data());
        expect(begin->size() == 64_ul);
    };
};

suite buffer_free_functions_suite = [] {
    "buffer() returns copy of mutable_registered_buffer"_test = [] {
        std::array<std::byte, 64> storage{};
        ::asio::mutable_buffer asio_buf{storage.data(), storage.size()};

        loom::asio::mutable_registered_buffer buf{
            asio_buf, nullptr, loom::asio::registered_buffer_id{3}};

        auto copy = loom::asio::buffer(buf);
        expect(copy.data() == storage.data());
        expect(copy.size() == 64_ul);
        expect(copy.id().native_handle() == 3_ul);
    };

    "buffer() with size limits mutable_registered_buffer"_test = [] {
        std::array<std::byte, 64> storage{};
        ::asio::mutable_buffer asio_buf{storage.data(), storage.size()};

        loom::asio::mutable_registered_buffer buf{
            asio_buf, nullptr, loom::asio::registered_buffer_id{4}};

        auto limited = loom::asio::buffer(buf, 32);
        expect(limited.data() == storage.data());
        expect(limited.size() == 32_ul);
        expect(limited.id().native_handle() == 4_ul);

        // Should not exceed original size
        auto overlimit = loom::asio::buffer(buf, 100);
        expect(overlimit.size() == 64_ul);
    };

    "buffer() returns copy of const_registered_buffer"_test = [] {
        std::array<std::byte, 64> storage{};
        ::asio::const_buffer asio_buf{storage.data(), storage.size()};

        loom::asio::const_registered_buffer buf{
            asio_buf, nullptr, loom::asio::registered_buffer_id{5}};

        auto copy = loom::asio::buffer(buf);
        expect(copy.data() == storage.data());
        expect(copy.size() == 64_ul);
        expect(copy.id().native_handle() == 5_ul);
    };

    "buffer() with size limits const_registered_buffer"_test = [] {
        std::array<std::byte, 64> storage{};
        ::asio::const_buffer asio_buf{storage.data(), storage.size()};

        loom::asio::const_registered_buffer buf{
            asio_buf, nullptr, loom::asio::registered_buffer_id{6}};

        auto limited = loom::asio::buffer(buf, 16);
        expect(limited.data() == storage.data());
        expect(limited.size() == 16_ul);
        expect(limited.id().native_handle() == 6_ul);
    };
};

suite buffer_registration_suite = [] {
    skip / "buffer_registration with fabric"_test = [] {
        loom::fabric_hints hints{};
        hints.ep_type = loom::endpoint_types::rdm;

        auto info_result = loom::query_fabric(hints);
        expect(static_cast<bool>(info_result)) << "No fabric provider available";

        auto fabric_result = loom::fabric::create(*info_result);
        expect(static_cast<bool>(fabric_result)) << "Failed to create fabric";

        auto domain_result = loom::domain::create(*fabric_result, *info_result);
        expect(static_cast<bool>(domain_result)) << "Failed to create domain";

        ::asio::io_context ioc;

        std::array<std::byte, 1024> buf1{};
        std::array<std::byte, 2048> buf2{};
        std::array buffers = {::asio::buffer(buf1), ::asio::buffer(buf2)};

        auto reg_result =
            loom::asio::register_buffers(ioc,
                                         *domain_result,
                                         buffers,
                                         loom::mr_access_flags::send | loom::mr_access_flags::recv);

        expect(static_cast<bool>(reg_result)) << "Failed to register buffers";

        auto& reg = *reg_result;
        expect(reg.size() == 2_ul);

        expect(reg[0].data() == buf1.data());
        expect(reg[0].size() == 1024_ul);
        expect(reg[0].memory_region() != nullptr);
        expect(reg[0].id().native_handle() == 0_ul);

        expect(reg[1].data() == buf2.data());
        expect(reg[1].size() == 2048_ul);
        expect(reg[1].memory_region() != nullptr);
        expect(reg[1].id().native_handle() == 1_ul);
    };

    skip / "buffer_registration iteration"_test = [] {
        loom::fabric_hints hints{};
        hints.ep_type = loom::endpoint_types::rdm;

        auto info_result = loom::query_fabric(hints);
        expect(static_cast<bool>(info_result)) << "No fabric provider available";

        auto fabric_result = loom::fabric::create(*info_result);
        expect(static_cast<bool>(fabric_result)) << "Failed to create fabric";

        auto domain_result = loom::domain::create(*fabric_result, *info_result);
        expect(static_cast<bool>(domain_result)) << "Failed to create domain";

        ::asio::io_context ioc;

        std::array<std::byte, 512> buf1{};
        std::array<std::byte, 512> buf2{};
        std::array<std::byte, 512> buf3{};
        std::array buffers = {::asio::buffer(buf1), ::asio::buffer(buf2), ::asio::buffer(buf3)};

        auto reg_result =
            loom::asio::register_buffers(ioc,
                                         *domain_result,
                                         buffers,
                                         loom::mr_access_flags::send | loom::mr_access_flags::recv);

        expect(static_cast<bool>(reg_result)) << "Failed to register buffers";

        auto& reg = *reg_result;
        std::size_t count = 0;
        for (const auto& buf : reg) {
            expect(buf.size() == 512_ul);
            expect(buf.id().native_handle() == count);
            ++count;
        }
        expect(count == 3_ul);
    };

    skip / "buffer_registration move semantics"_test = [] {
        loom::fabric_hints hints{};
        hints.ep_type = loom::endpoint_types::rdm;

        auto info_result = loom::query_fabric(hints);
        expect(static_cast<bool>(info_result)) << "No fabric provider available";

        auto fabric_result = loom::fabric::create(*info_result);
        expect(static_cast<bool>(fabric_result)) << "Failed to create fabric";

        auto domain_result = loom::domain::create(*fabric_result, *info_result);
        expect(static_cast<bool>(domain_result)) << "Failed to create domain";

        ::asio::io_context ioc;

        std::array<std::byte, 256> buf{};
        std::array buffers = {::asio::buffer(buf)};

        auto reg_result =
            loom::asio::register_buffers(ioc,
                                         *domain_result,
                                         buffers,
                                         loom::mr_access_flags::send | loom::mr_access_flags::recv);

        expect(static_cast<bool>(reg_result)) << "Failed to register buffers";

        auto reg1 = std::move(*reg_result);
        expect(reg1.size() == 1_ul);

        auto reg2 = std::move(reg1);
        expect(reg2.size() == 1_ul);
        expect(reg2[0].data() == buf.data());
    };
};

auto main() -> int {
    return 0;
}
