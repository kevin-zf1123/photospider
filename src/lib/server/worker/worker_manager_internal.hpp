#pragma once

/**
 * @file worker_manager_internal.hpp
 * @brief Aggregates source-private WorkerManager implementation boundaries.
 *
 * This header fixes one shared view of POSIX ownership and the opaque Impl
 * record registry for split implementation translation units. It is not
 * installed and adds no compatibility API.
 */

#include "server/worker/worker_manager_impl.hpp"
