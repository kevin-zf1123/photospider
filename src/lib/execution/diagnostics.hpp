#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

#include "core/resource_observation.hpp"
#include "photospider/execution/execution.hpp"

namespace ps::execution_internal {
// Counters never retain a Run, Result, allocation or context. Storage slices
// may retain an alias to this state after Run destruction.
struct MemoryObservation final {
  mutable std::mutex mutex;
  ExecutionMemoryStatistics statistics;
  ExecutionMemoryStatistics snapshot() const noexcept {
    std::lock_guard<std::mutex> lock(mutex);
    return statistics;
  }
  void reservation(const std::array<std::uint64_t, 3>& amounts,
                   bool add) noexcept {
    std::lock_guard<std::mutex> lock(mutex);
    for (unsigned d = 0; d < 3; ++d) {
      auto& reserved = statistics.reserved_bytes_by_domain[d];
      if (add)
        reserved += amounts[d];
      else
        reserved -= amounts[d];
      statistics.peak_reserved_bytes_by_domain[d] =
          std::max(statistics.peak_reserved_bytes_by_domain[d], reserved);
    }
  }
  void change(unsigned domain, std::uint64_t reserved_add,
              std::uint64_t reserved_remove, std::uint64_t live_add,
              std::uint64_t live_remove) noexcept {
    std::lock_guard<std::mutex> lock(mutex);
    auto& reserved = statistics.reserved_bytes_by_domain[domain];
    auto& live = statistics.live_bytes_by_domain[domain];
    reserved += reserved_add;
    reserved -= reserved_remove;
    live += live_add;
    live -= live_remove;
    statistics.peak_reserved_bytes_by_domain[domain] =
        std::max(statistics.peak_reserved_bytes_by_domain[domain], reserved);
    statistics.peak_live_bytes_by_domain[domain] =
        std::max(statistics.peak_live_bytes_by_domain[domain], live);
  }
};
struct DiagnosticsState final {
  ResourceLease metadata;
  std::shared_ptr<core_internal::MetadataObservation> metadata_observation;
  mutable std::mutex mutex;
  ExecutionDiagnostics snapshot;
  MemoryObservation memory;
  std::vector<ExecutionTaskTiming> tasks;
  std::vector<ExecutionCallTiming> calls;
  std::size_t task_count = 0, call_count = 0;
};
}  // namespace ps::execution_internal
