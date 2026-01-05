// SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only
/**
 * @file test_asio_registered_buffer.cpp
 * @brief Tests for loom::asio buffer registration.
 */

#include <loom/asio.hpp>
#include <loom/loom.hpp>

#include <array>

#include <catch2/catch_test_macros.hpp>
#include <asio/io_context.hpp>

using namespace std::literals;

// =============================================================================
// registered_buffer_id tests
// =============================================================================

TEST_CASE("registered_buffer_id default is invalid", "[asio][buffer]") {
    loom::asio::registered_buffer_id id;
    REQUIRE(!id.is_valid());
}

TEST_CASE("registered_buffer_id with value is valid", "[asio][buffer]") {
    loom::asio::registered_buffer_id id{42};
    REQUIRE(id.is_valid());
    REQUIRE(id.native_handle() == 42);
}

TEST_CASE("registered_buffer_id equality", "[asio][buffer]") {
    loom::asio::registered_buffer_id id1{1};
    loom::asio::registered_buffer_id id2{1};
    loom::asio::registered_buffer_id id3{2};

    REQUIRE(id1 == id2);
    REQUIRE(id1 != id3);
}

// =============================================================================
// mutable_registered_buffer tests
// =============================================================================

TEST_CASE("mutable_registered_buffer default construction", "[asio][buffer]") {
    loom::asio::mutable_registered_buffer buf;
    REQUIRE(buf.data() == nullptr);
    REQUIRE(buf.size() == 0);
    REQUIRE(buf.memory_region() == nullptr);
    REQUIRE(!buf.id().is_valid());
}

TEST_CASE("mutable_registered_buffer offset operator", "[asio][buffer]") {
    std::array<std::byte, 64> storage{};
    ::asio::mutable_buffer asio_buf{storage.data(), storage.size()};

    loom::asio::mutable_registered_buffer buf{
        asio_buf, nullptr, loom::asio::registered_buffer_id{0}};

    auto offset_buf = buf + 16;
    REQUIRE(offset_buf.size() == 48);
    REQUIRE(offset_buf.data() == storage.data() + 16);
}

TEST_CASE("mutable_registered_buffer buffer_sequence interface", "[asio][buffer]") {
    std::array<std::byte, 64> storage{};
    ::asio::mutable_buffer asio_buf{storage.data(), storage.size()};

    loom::asio::mutable_registered_buffer buf{
        asio_buf, nullptr, loom::asio::registered_buffer_id{0}};

    auto begin = loom::asio::buffer_sequence_begin(buf);
    auto end = loom::asio::buffer_sequence_end(buf);

    REQUIRE(end - begin == 1);
    REQUIRE(begin->data() == storage.data());
    REQUIRE(begin->size() == 64);
}

// =============================================================================
// const_registered_buffer tests
// =============================================================================

TEST_CASE("const_registered_buffer from mutable", "[asio][buffer]") {
    std::array<std::byte, 64> storage{};
    ::asio::mutable_buffer asio_buf{storage.data(), storage.size()};

    loom::asio::mutable_registered_buffer mut_buf{
        asio_buf, nullptr, loom::asio::registered_buffer_id{5}};

    loom::asio::const_registered_buffer const_buf{mut_buf};

    REQUIRE(const_buf.data() == storage.data());
    REQUIRE(const_buf.size() == 64);
    REQUIRE(const_buf.id().native_handle() == 5);
}

TEST_CASE("const_registered_buffer buffer_sequence interface", "[asio][buffer]") {
    std::array<std::byte, 64> storage{};
    ::asio::const_buffer asio_buf{storage.data(), storage.size()};

    loom::asio::const_registered_buffer buf{
        asio_buf, nullptr, loom::asio::registered_buffer_id{0}};

    auto begin = loom::asio::buffer_sequence_begin(buf);
    auto end = loom::asio::buffer_sequence_end(buf);

    REQUIRE(end - begin == 1);
    REQUIRE(begin->data() == storage.data());
    REQUIRE(begin->size() == 64);
}

// =============================================================================
// buffer free functions tests
// =============================================================================

TEST_CASE("buffer() returns copy of mutable_registered_buffer", "[asio][buffer]") {
    std::array<std::byte, 64> storage{};
    ::asio::mutable_buffer asio_buf{storage.data(), storage.size()};

    loom::asio::mutable_registered_buffer buf{
        asio_buf, nullptr, loom::asio::registered_buffer_id{3}};

    auto copy = loom::asio::buffer(buf);
    REQUIRE(copy.data() == storage.data());
    REQUIRE(copy.size() == 64);
    REQUIRE(copy.id().native_handle() == 3);
}

TEST_CASE("buffer() with size limits mutable_registered_buffer", "[asio][buffer]") {
    std::array<std::byte, 64> storage{};
    ::asio::mutable_buffer asio_buf{storage.data(), storage.size()};

    loom::asio::mutable_registered_buffer buf{
        asio_buf, nullptr, loom::asio::registered_buffer_id{4}};

    auto limited = loom::asio::buffer(buf, 32);
    REQUIRE(limited.data() == storage.data());
    REQUIRE(limited.size() == 32);
    REQUIRE(limited.id().native_handle() == 4);

    // Should not exceed original size
    auto overlimit = loom::asio::buffer(buf, 100);
    REQUIRE(overlimit.size() == 64);
}

TEST_CASE("buffer() returns copy of const_registered_buffer", "[asio][buffer]") {
    std::array<std::byte, 64> storage{};
    ::asio::const_buffer asio_buf{storage.data(), storage.size()};

    loom::asio::const_registered_buffer buf{
        asio_buf, nullptr, loom::asio::registered_buffer_id{5}};

    auto copy = loom::asio::buffer(buf);
    REQUIRE(copy.data() == storage.data());
    REQUIRE(copy.size() == 64);
    REQUIRE(copy.id().native_handle() == 5);
}

TEST_CASE("buffer() with size limits const_registered_buffer", "[asio][buffer]") {
    std::array<std::byte, 64> storage{};
    ::asio::const_buffer asio_buf{storage.data(), storage.size()};

    loom::asio::const_registered_buffer buf{
        asio_buf, nullptr, loom::asio::registered_buffer_id{6}};

    auto limited = loom::asio::buffer(buf, 16);
    REQUIRE(limited.data() == storage.data());
    REQUIRE(limited.size() == 16);
    REQUIRE(limited.id().native_handle() == 6);
}

// =============================================================================
// buffer_registration tests (require fabric provider)
// =============================================================================

TEST_CASE("buffer_registration with fabric", "[asio][buffer][.integration]") {
    loom::fabric_hints hints{};
    hints.ep_type = loom::endpoint_types::rdm;

    auto info_result = loom::query_fabric(hints);
    REQUIRE(info_result.has_value());

    auto fabric_result = loom::fabric::create(*info_result);
    REQUIRE(fabric_result.has_value());

    auto domain_result = loom::domain::create(*fabric_result, *info_result);
    REQUIRE(domain_result.has_value());

    ::asio::io_context ioc;

    std::array<std::byte, 1024> buf1{};
    std::array<std::byte, 2048> buf2{};
    std::array buffers = {::asio::buffer(buf1), ::asio::buffer(buf2)};

    auto reg_result =
        loom::asio::register_buffers(ioc,
                                     *domain_result,
                                     buffers,
                                     loom::mr_access_flags::send | loom::mr_access_flags::recv);

    REQUIRE(reg_result.has_value());

    auto& reg = *reg_result;
    REQUIRE(reg.size() == 2);

    REQUIRE(reg[0].data() == buf1.data());
    REQUIRE(reg[0].size() == 1024);
    REQUIRE(reg[0].memory_region() != nullptr);
    REQUIRE(reg[0].id().native_handle() == 0);

    REQUIRE(reg[1].data() == buf2.data());
    REQUIRE(reg[1].size() == 2048);
    REQUIRE(reg[1].memory_region() != nullptr);
    REQUIRE(reg[1].id().native_handle() == 1);
}

TEST_CASE("buffer_registration iteration", "[asio][buffer][.integration]") {
    loom::fabric_hints hints{};
    hints.ep_type = loom::endpoint_types::rdm;

    auto info_result = loom::query_fabric(hints);
    REQUIRE(info_result.has_value());

    auto fabric_result = loom::fabric::create(*info_result);
    REQUIRE(fabric_result.has_value());

    auto domain_result = loom::domain::create(*fabric_result, *info_result);
    REQUIRE(domain_result.has_value());

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

    REQUIRE(reg_result.has_value());

    auto& reg = *reg_result;
    std::size_t count = 0;
    for (const auto& buf : reg) {
        REQUIRE(buf.size() == 512);
        REQUIRE(buf.id().native_handle() == count);
        ++count;
    }
    REQUIRE(count == 3);
}

TEST_CASE("buffer_registration move semantics", "[asio][buffer][.integration]") {
    loom::fabric_hints hints{};
    hints.ep_type = loom::endpoint_types::rdm;

    auto info_result = loom::query_fabric(hints);
    REQUIRE(info_result.has_value());

    auto fabric_result = loom::fabric::create(*info_result);
    REQUIRE(fabric_result.has_value());

    auto domain_result = loom::domain::create(*fabric_result, *info_result);
    REQUIRE(domain_result.has_value());

    ::asio::io_context ioc;

    std::array<std::byte, 256> buf{};
    std::array buffers = {::asio::buffer(buf)};

    auto reg_result =
        loom::asio::register_buffers(ioc,
                                     *domain_result,
                                     buffers,
                                     loom::mr_access_flags::send | loom::mr_access_flags::recv);

    REQUIRE(reg_result.has_value());

    auto reg1 = std::move(*reg_result);
    REQUIRE(reg1.size() == 1);

    auto reg2 = std::move(reg1);
    REQUIRE(reg2.size() == 1);
    REQUIRE(reg2[0].data() == buf.data());
}
