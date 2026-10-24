// SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only
#pragma once

#include <thread>
#include <utility>

namespace loom::test {

// libc++ on the macOS Nix runner does not provide std::jthread.
// These tests only need a thread that joins before its scope ends.
class joining_thread : public std::thread {
public:
    using std::thread::thread;
    joining_thread() noexcept = default;
    joining_thread(joining_thread&&) noexcept = default;

    joining_thread& operator=(joining_thread&& other) noexcept {
        if (joinable())
            join();
        std::thread::operator=(std::move(other));
        return *this;
    }

    ~joining_thread() {
        if (joinable())
            join();
    }
};

}  // namespace loom::test
