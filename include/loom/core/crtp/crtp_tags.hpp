// SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only
#pragma once

/**
 * @namespace loom::crtp
 * @brief CRTP tag types for compile-time type detection.
 */
namespace loom::crtp {

/**
 * @struct fabric_object_tag
 * @brief Tag type for fabric object CRTP base classes.
 */
struct fabric_object_tag {};
/**
 * @struct event_tag
 * @brief Tag type for event CRTP base classes.
 */
struct event_tag {};
/**
 * @struct remote_memory_tag
 * @brief Tag type for remote memory CRTP base classes.
 */
struct remote_memory_tag {};
/**
 * @struct asio_context_tag
 * @brief Tag type for asio context CRTP base classes.
 */
struct asio_context_tag {};

}  // namespace loom::crtp
