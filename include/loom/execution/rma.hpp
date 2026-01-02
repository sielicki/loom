// SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only
#pragma once

/**
 * @file rma.hpp
 * @brief P2300 senders for loom RMA (Remote Memory Access) operations.
 *
 * This header provides sender types for RMA operations including read, write,
 * and atomic operations. These senders enable one-sided communication where
 * data is transferred directly to/from remote memory without involving the
 * remote CPU.
 *
 * ## Usage
 *
 * ```cpp
 * #include <loom/execution.hpp>
 *
 * // Create an RMA sender factory
 * auto rma_ops = loom::rma_async(ep);
 *
 * // Read from remote memory
 * auto read_sender = rma_ops.read(local_buf, remote_addr, key, &cq);
 *
 * // Write to remote memory
 * auto write_sender = rma_ops.write(local_buf, remote_addr, key, &cq);
 *
 * // Atomic fetch-and-add
 * auto atomic_sender = rma_ops.fetch_add<uint64_t>(remote_addr, 1, key, &cq);
 * ```
 */

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <system_error>
#include <type_traits>
#include <utility>

#include "loom/async/completion_queue.hpp"
#include "loom/core/atomic.hpp"
#include "loom/core/endpoint.hpp"
#include "loom/core/memory.hpp"
#include "loom/core/types.hpp"
#include "loom/execution/concepts.hpp"
#include "loom/execution/operation_state.hpp"

namespace loom::execution {

// Forward declarations
class read_sender;
class write_sender;
template <typename T>
class fetch_add_sender;
template <typename T>
class compare_swap_sender;

}  // namespace loom::execution

namespace loom::execution {

/**
 * @brief Sender for RMA read operations.
 *
 * Reads data from remote memory into a local buffer.
 *
 * ## Completion Signatures
 * - set_value(std::size_t) - bytes read on success
 * - set_error(std::error_code) - on failure
 * - set_stopped() - on cancellation
 */
class read_sender {
public:
    using sender_concept = sender_t;
    using completion_signatures =
        loom::execution::completion_signatures<set_value_t(std::size_t),
                                               set_error_t(std::error_code),
                                               set_stopped_t()>;

    read_sender(endpoint* ep,
                std::span<std::byte> buffer,
                rma_addr remote_addr,
                mr_key key,
                completion_queue* cq) noexcept
        : ep_(ep), buffer_(buffer), remote_addr_(remote_addr), key_(key), cq_(cq) {}

    template <typename Receiver>
    [[nodiscard]] auto connect(Receiver receiver) && noexcept
        -> read_operation_state<std::decay_t<Receiver>> {
        return read_operation_state<std::decay_t<Receiver>>(
            ep_, buffer_, remote_addr_, key_, cq_, std::move(receiver));
    }

    [[nodiscard]] auto get_env() const noexcept -> fabric_env { return fabric_env{cq_}; }

private:
    endpoint* ep_;
    std::span<std::byte> buffer_;
    rma_addr remote_addr_;
    mr_key key_;
    completion_queue* cq_;
};

/**
 * @brief Sender for RMA write operations.
 *
 * Writes data from a local buffer to remote memory.
 *
 * ## Completion Signatures
 * - set_value() - on successful write
 * - set_error(std::error_code) - on failure
 * - set_stopped() - on cancellation
 */
class write_sender {
public:
    using sender_concept = sender_t;
    using completion_signatures = loom::execution::
        completion_signatures<set_value_t(), set_error_t(std::error_code), set_stopped_t()>;

    write_sender(endpoint* ep,
                 std::span<const std::byte> buffer,
                 rma_addr remote_addr,
                 mr_key key,
                 completion_queue* cq) noexcept
        : ep_(ep), buffer_(buffer), remote_addr_(remote_addr), key_(key), cq_(cq) {}

    template <typename Receiver>
    [[nodiscard]] auto connect(Receiver receiver) && noexcept
        -> write_operation_state<std::decay_t<Receiver>> {
        return write_operation_state<std::decay_t<Receiver>>(
            ep_, buffer_, remote_addr_, key_, cq_, std::move(receiver));
    }

    [[nodiscard]] auto get_env() const noexcept -> fabric_env { return fabric_env{cq_}; }

private:
    endpoint* ep_;
    std::span<const std::byte> buffer_;
    rma_addr remote_addr_;
    mr_key key_;
    completion_queue* cq_;
};

/**
 * @brief Operation state for fetch-and-add atomic operations.
 *
 * Stores the operand locally and performs a fetch-add on the remote memory,
 * returning the original value via set_value.
 *
 * @tparam Receiver The P2300 receiver type.
 * @tparam T The atomic value type.
 */
template <typename Receiver, typename T>
    requires atomic::atomic_type<T>
class fetch_add_operation_state final : public fabric_operation_state_base<Receiver, T> {
    using base = fabric_operation_state_base<Receiver, T>;

public:
    fetch_add_operation_state(endpoint* ep,
                              rma_addr remote_addr,
                              T operand,
                              mr_key key,
                              memory_region* result_mr,
                              completion_queue* cq,
                              Receiver receiver) noexcept
        : base(std::move(receiver)), ep_(ep), remote_addr_(remote_addr), operand_(operand),
          key_(key), result_mr_(result_mr), cq_(cq), result_value_{} {}

    void start() & noexcept {
        if (ep_ == nullptr || !ep_->impl_valid()) {
            this->complete_error(std::make_error_code(std::errc::bad_file_descriptor));
            return;
        }

        remote_memory remote{remote_addr_.get(), key_.get(), sizeof(T)};

        auto result = atomic::fetch(*ep_,
                                    atomic::operation::sum,
                                    atomic::get_datatype<T>(),
                                    &operand_,
                                    &result_value_,
                                    1,
                                    nullptr,
                                    result_mr_,
                                    remote,
                                    this->context());

        if (!result) {
            this->complete_error(result.error());
        }
        // On success, completion comes via the completion queue
    }

    [[nodiscard]] auto get_result() const noexcept -> T { return result_value_; }

private:
    endpoint* ep_;
    rma_addr remote_addr_;
    T operand_;
    mr_key key_;
    memory_region* result_mr_;
    completion_queue* cq_;
    T result_value_;
};

/**
 * @brief Sender for atomic fetch-and-add operations.
 *
 * Atomically adds a value to remote memory and returns the original value.
 *
 * ## Completion Signatures
 * - set_value(T) - the original value before the add
 * - set_error(std::error_code) - on failure
 * - set_stopped() - on cancellation
 *
 * @tparam T The atomic value type (uint32_t, uint64_t, etc.)
 */
template <typename T>
    requires atomic::atomic_type<T>
class fetch_add_sender {
public:
    using sender_concept = sender_t;
    using completion_signatures = loom::execution::
        completion_signatures<set_value_t(T), set_error_t(std::error_code), set_stopped_t()>;

    fetch_add_sender(endpoint* ep,
                     rma_addr remote_addr,
                     T operand,
                     mr_key key,
                     memory_region* result_mr,
                     completion_queue* cq) noexcept
        : ep_(ep), remote_addr_(remote_addr), operand_(operand), key_(key), result_mr_(result_mr),
          cq_(cq) {}

    template <typename Receiver>
    [[nodiscard]] auto connect(Receiver receiver) && noexcept
        -> fetch_add_operation_state<std::decay_t<Receiver>, T> {
        return fetch_add_operation_state<std::decay_t<Receiver>, T>(
            ep_, remote_addr_, operand_, key_, result_mr_, cq_, std::move(receiver));
    }

    [[nodiscard]] auto get_env() const noexcept -> fabric_env { return fabric_env{cq_}; }

private:
    endpoint* ep_;
    rma_addr remote_addr_;
    T operand_;
    mr_key key_;
    memory_region* result_mr_;
    completion_queue* cq_;
};

/**
 * @brief Operation state for compare-and-swap atomic operations.
 *
 * @tparam Receiver The P2300 receiver type.
 * @tparam T The atomic value type.
 */
template <typename Receiver, typename T>
    requires atomic::atomic_type<T>
class compare_swap_operation_state final : public fabric_operation_state_base<Receiver, T> {
    using base = fabric_operation_state_base<Receiver, T>;

public:
    compare_swap_operation_state(endpoint* ep,
                                 rma_addr remote_addr,
                                 T compare_value,
                                 T swap_value,
                                 mr_key key,
                                 memory_region* result_mr,
                                 completion_queue* cq,
                                 Receiver receiver) noexcept
        : base(std::move(receiver)), ep_(ep), remote_addr_(remote_addr),
          compare_value_(compare_value), swap_value_(swap_value), key_(key), result_mr_(result_mr),
          cq_(cq), result_value_{} {}

    void start() & noexcept {
        if (ep_ == nullptr || !ep_->impl_valid()) {
            this->complete_error(std::make_error_code(std::errc::bad_file_descriptor));
            return;
        }

        remote_memory remote{remote_addr_.get(), key_.get(), sizeof(T)};

        auto result = atomic::compare_swap(*ep_,
                                           atomic::get_datatype<T>(),
                                           &compare_value_,
                                           &swap_value_,
                                           &result_value_,
                                           1,
                                           nullptr,
                                           result_mr_,
                                           remote,
                                           this->context());

        if (!result) {
            this->complete_error(result.error());
        }
        // On success, completion comes via the completion queue
    }

    [[nodiscard]] auto get_result() const noexcept -> T { return result_value_; }

private:
    endpoint* ep_;
    rma_addr remote_addr_;
    T compare_value_;
    T swap_value_;
    mr_key key_;
    memory_region* result_mr_;
    completion_queue* cq_;
    T result_value_;
};

/**
 * @brief Sender for atomic compare-and-swap operations.
 *
 * Atomically compares the remote value to an expected value and, if equal,
 * replaces it with a new value. Returns the original remote value.
 *
 * ## Completion Signatures
 * - set_value(T) - the original value (before swap, regardless of success)
 * - set_error(std::error_code) - on failure
 * - set_stopped() - on cancellation
 *
 * @tparam T The atomic value type (uint32_t, uint64_t, etc.)
 */
template <typename T>
    requires atomic::atomic_type<T>
class compare_swap_sender {
public:
    using sender_concept = sender_t;
    using completion_signatures = loom::execution::
        completion_signatures<set_value_t(T), set_error_t(std::error_code), set_stopped_t()>;

    compare_swap_sender(endpoint* ep,
                        rma_addr remote_addr,
                        T compare_value,
                        T swap_value,
                        mr_key key,
                        memory_region* result_mr,
                        completion_queue* cq) noexcept
        : ep_(ep), remote_addr_(remote_addr), compare_value_(compare_value),
          swap_value_(swap_value), key_(key), result_mr_(result_mr), cq_(cq) {}

    template <typename Receiver>
    [[nodiscard]] auto connect(Receiver receiver) && noexcept
        -> compare_swap_operation_state<std::decay_t<Receiver>, T> {
        return compare_swap_operation_state<std::decay_t<Receiver>, T>(ep_,
                                                                       remote_addr_,
                                                                       compare_value_,
                                                                       swap_value_,
                                                                       key_,
                                                                       result_mr_,
                                                                       cq_,
                                                                       std::move(receiver));
    }

    [[nodiscard]] auto get_env() const noexcept -> fabric_env { return fabric_env{cq_}; }

private:
    endpoint* ep_;
    rma_addr remote_addr_;
    T compare_value_;
    T swap_value_;
    mr_key key_;
    memory_region* result_mr_;
    completion_queue* cq_;
};

// Backwards compatibility alias
template <typename T>
using atomic_sender = fetch_add_sender<T>;

/**
 * @brief Factory for creating RMA senders for an endpoint.
 *
 * This class provides a convenient interface for creating senders
 * for various RMA operations on a specific endpoint.
 *
 * ## Usage
 *
 * ```cpp
 * auto rma_ops = loom::rma_async(ep);
 * auto read_s = rma_ops.read(buffer, addr, key, &cq);
 * auto write_s = rma_ops.write(buffer, addr, key, &cq);
 * auto atomic_s = rma_ops.fetch_add<uint64_t>(addr, 1, key, &result_mr, &cq);
 * ```
 */
class rma_sender_factory {
public:
    explicit rma_sender_factory(endpoint& ep) noexcept : ep_(&ep) {}

    /**
     * @brief Creates a read sender.
     * @param buffer Local buffer to read into.
     * @param remote_addr Remote memory address.
     * @param key Remote memory region key.
     * @param cq Pointer to the completion queue.
     * @return A read_sender.
     */
    [[nodiscard]] auto read(std::span<std::byte> buffer,
                            rma_addr remote_addr,
                            mr_key key,
                            completion_queue* cq) const noexcept -> read_sender {
        return read_sender(ep_, buffer, remote_addr, key, cq);
    }

    template <typename T, std::size_t N>
    [[nodiscard]] auto read(std::array<T, N>& buffer,
                            rma_addr remote_addr,
                            mr_key key,
                            completion_queue* cq) const noexcept -> read_sender {
        return read_sender(ep_,
                           std::span<std::byte>(reinterpret_cast<std::byte*>(buffer.data()),
                                                buffer.size() * sizeof(T)),
                           remote_addr,
                           key,
                           cq);
    }

    /**
     * @brief Creates a write sender.
     * @param buffer Local buffer to write from.
     * @param remote_addr Remote memory address.
     * @param key Remote memory region key.
     * @param cq Pointer to the completion queue.
     * @return A write_sender.
     */
    [[nodiscard]] auto write(std::span<const std::byte> buffer,
                             rma_addr remote_addr,
                             mr_key key,
                             completion_queue* cq) const noexcept -> write_sender {
        return write_sender(ep_, buffer, remote_addr, key, cq);
    }

    template <typename T, std::size_t N>
    [[nodiscard]] auto write(const std::array<T, N>& buffer,
                             rma_addr remote_addr,
                             mr_key key,
                             completion_queue* cq) const noexcept -> write_sender {
        return write_sender(
            ep_,
            std::span<const std::byte>(reinterpret_cast<const std::byte*>(buffer.data()),
                                       buffer.size() * sizeof(T)),
            remote_addr,
            key,
            cq);
    }

    /**
     * @brief Creates an atomic fetch-and-add sender.
     *
     * Atomically adds @p operand to the value at @p remote_addr and returns
     * the original value. The result is stored in memory backed by @p result_mr.
     *
     * @tparam T The atomic value type (int32_t, uint32_t, int64_t, uint64_t, etc.)
     * @param remote_addr Remote memory address of the atomic variable.
     * @param operand The value to add to the remote value.
     * @param key Remote memory region key.
     * @param result_mr Memory region for storing the result (original value).
     * @param cq Pointer to the completion queue.
     * @return A fetch_add_sender that completes with the original remote value.
     */
    template <typename T>
        requires atomic::atomic_type<T>
    [[nodiscard]] auto fetch_add(rma_addr remote_addr,
                                 T operand,
                                 mr_key key,
                                 memory_region* result_mr,
                                 completion_queue* cq) const noexcept -> fetch_add_sender<T> {
        return fetch_add_sender<T>(ep_, remote_addr, operand, key, result_mr, cq);
    }

    /**
     * @brief Creates an atomic compare-and-swap sender.
     *
     * Atomically compares the value at @p remote_addr to @p compare_value.
     * If they are equal, replaces the remote value with @p swap_value.
     * Returns the original remote value (before any swap) in both cases.
     *
     * @tparam T The atomic value type (int32_t, uint32_t, int64_t, uint64_t, etc.)
     * @param remote_addr Remote memory address of the atomic variable.
     * @param compare_value The expected value to compare against.
     * @param swap_value The new value to store if comparison succeeds.
     * @param key Remote memory region key.
     * @param result_mr Memory region for storing the result (original value).
     * @param cq Pointer to the completion queue.
     * @return A compare_swap_sender that completes with the original remote value.
     */
    template <typename T>
        requires atomic::atomic_type<T>
    [[nodiscard]] auto compare_swap(rma_addr remote_addr,
                                    T compare_value,
                                    T swap_value,
                                    mr_key key,
                                    memory_region* result_mr,
                                    completion_queue* cq) const noexcept -> compare_swap_sender<T> {
        return compare_swap_sender<T>(
            ep_, remote_addr, compare_value, swap_value, key, result_mr, cq);
    }

private:
    endpoint* ep_;
};

}  // namespace loom::execution

namespace loom {

/**
 * @brief Creates an RMA sender factory for the given endpoint.
 * @param ep Reference to the endpoint.
 * @return An rma_sender_factory.
 */
[[nodiscard]] inline auto rma_async(endpoint& ep) noexcept -> execution::rma_sender_factory {
    return execution::rma_sender_factory(ep);
}

}  // namespace loom
