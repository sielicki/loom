// SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only
#pragma once

#include <rdma/fi_errno.h>

#include <climits>
#include <cstdint>
#include <limits>
#include <memory>
#include <mutex>
#include <unordered_set>
#include <utility>

#include "loom/core/allocator.hpp"
#include "loom/core/result.hpp"

namespace loom::detail {

// Shared by a domain and its registrations; the resource must outlive both.
struct mr_key_pool {
    std::mutex mutex;
    std::pmr::unordered_set<std::uint64_t> live;
    std::uint64_t next{};
    std::uint64_t maximum;

    mr_key_pool(std::size_t bytes, memory_resource* resource)
        : live(resource),
          maximum(bytes >= sizeof(std::uint64_t) ? std::numeric_limits<std::uint64_t>::max() - 1
                                                 : (std::uint64_t{1} << (bytes * CHAR_BIT)) - 1) {}
};

class mr_key_lease {
public:
    mr_key_lease() = default;
    mr_key_lease(const mr_key_lease&) = delete;
    auto operator=(const mr_key_lease&) -> mr_key_lease& = delete;
    mr_key_lease(mr_key_lease&&) noexcept = default;
    auto operator=(mr_key_lease&& other) noexcept -> mr_key_lease& {
        if (this != &other) {
            reset();
            pool_ = std::move(other.pool_);
            key_ = other.key_;
        }
        return *this;
    }
    ~mr_key_lease() { reset(); }

    static auto acquire(std::shared_ptr<mr_key_pool> pool) -> result<mr_key_lease> {
        if (!pool) {
            return mr_key_lease{};
        }
        std::lock_guard lock{pool->mutex};
        if (pool->live.size() > pool->maximum) {
            return make_error_result_from_fi_errno<mr_key_lease>(FI_ENOKEY);
        }
        auto key = pool->next;
        while (pool->live.contains(key)) {
            key = key == pool->maximum ? 0 : key + 1;
        }
        pool->live.insert(key);
        pool->next = key == pool->maximum ? 0 : key + 1;
        return mr_key_lease{std::move(pool), key};
    }

    [[nodiscard]] auto get() const noexcept -> std::uint64_t { return key_; }

    // A failed native close leaves the registration live: do not reuse its key.
    void abandon() noexcept { pool_.reset(); }

private:
    mr_key_lease(std::shared_ptr<mr_key_pool> pool, std::uint64_t key) noexcept
        : pool_(std::move(pool)), key_(key) {}

    void reset() noexcept {
        if (pool_) {
            {
                std::lock_guard lock{pool_->mutex};
                pool_->live.erase(key_);
            }
            pool_.reset();
        }
    }

    std::shared_ptr<mr_key_pool> pool_;
    std::uint64_t key_{};
};

}  // namespace loom::detail
