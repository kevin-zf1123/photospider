#pragma once

/**
 * @file execution_service_internal.hpp
 * @brief Aggregates source-private ExecutionService implementation state.
 *
 * Translation units include this boundary to share one ODR-consistent view of
 * Run state, the bounded ready store, and the resource pool. The header is not
 * installed and adds no public or private source-tree compatibility surface.
 */

#include "compute/execution/execution_service_pool.hpp"
