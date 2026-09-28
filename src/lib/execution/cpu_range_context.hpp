#pragma once

namespace ps::execution_internal {
/** @brief True only while this thread executes a host-assigned range block. */
inline thread_local bool in_cpu_range = false;
/** @brief True throughout a context-owned CPU or GPU pool worker's lifetime. */
inline thread_local bool in_kernel_worker = false;
}  // namespace ps::execution_internal
