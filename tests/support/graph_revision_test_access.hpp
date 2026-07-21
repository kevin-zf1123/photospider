#pragma once

#include <atomic>
#include <cstdint>

#include "graph/graph_revision.hpp"  // NOLINT(build/include_subdir)

namespace ps::testing {

/**
 * @brief Private bridge for exercising the production identity CAS algorithm.
 *
 * @note The bridge accepts only caller-owned counters. It cannot observe,
 * reset, or otherwise modify the process-lifetime production counter.
 */
class GraphInstanceIdTestAccess {
 public:
  /**
   * @brief Mints through the exact production reservation loop.
   * @param last_issued Isolated counter owned by the calling test.
   * @return Newly reserved identity.
   * @throws std::overflow_error when the supplied counter is exhausted.
   * @note Concurrent callers may share the supplied atomic exactly as
   * production callers share the hidden process counter.
   */
  static GraphInstanceId mint_from(std::atomic<uint64_t>& last_issued) {
    return GraphInstanceId::mint_from_counter(last_issued);
  }
};

}  // namespace ps::testing
