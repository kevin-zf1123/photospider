#pragma once

#include <vector>

#include "compute/execution_service.hpp"

namespace ps::testing {

/**
 * @brief Test-only access to deterministic ExecutionService staging cleanup.
 *
 * @note This private support header is compiled only by repository tests. It
 * exposes no Host, scheduler, plugin, or installed-library API.
 */
class ExecutionServiceTestAccess final {
 public:
  /**
   * @brief Releases one initial-submission vector through the production seam.
   * @param submissions Initial values whose storage should be retired.
   * @return Nothing.
   * @throws Nothing.
   * @note Production calls the same boundary only after moving every value
   * into a staged queue entry and before publishing or waiting for the Run.
   */
  static void release_initial_submission_storage(
      std::vector<compute::ReadyTaskSubmission>& submissions) noexcept {
    compute::ExecutionService::release_initial_submission_storage(submissions);
  }
};

}  // namespace ps::testing
