// SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only
#pragma once

#include <loom/cxx_bridge.hpp>

namespace loom::cxx_bridge {
enum class TestRegistrationFailure : std::uint8_t;
struct TestRegistrationState;

class RegistrationFixture {
public:
    struct state;
    std::unique_ptr<state> impl;
    explicit RegistrationFixture(bool endpoint_bound);
    ~RegistrationFixture();
};

auto test_registration_fixture(bool endpoint_bound) -> std::unique_ptr<RegistrationFixture>;
auto test_registration_fabric(RegistrationFixture& fixture) -> std::unique_ptr<FabricHandle>;
auto test_registration_info(const RegistrationFixture& fixture)
    -> std::unique_ptr<FabricInfoHandle>;
auto test_registration_state(const RegistrationFixture& fixture) -> TestRegistrationState;
void test_registration_fail(RegistrationFixture& fixture, TestRegistrationFailure failure);
void test_queue_schedule(RegistrationFixture& fixture,
                         std::uintptr_t context,
                         std::uint32_t delay_us);
}  // namespace loom::cxx_bridge
