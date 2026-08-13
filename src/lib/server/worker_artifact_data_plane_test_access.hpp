/**
 * @file worker_artifact_data_plane_test_access.hpp
 * @brief Declares deterministic source-private artifact data-plane fault seams.
 */
#pragma once

#include <filesystem>

namespace ps::server {

/**
 * @brief Exposes deterministic cleanup evidence without artifact authority.
 *
 * The seam exercises the production temporary-occurrence constructor at the
 * first point after `mkstemp` has created a named file. It exposes no path,
 * descriptor, reference, payload, or publication capability to the caller.
 *
 * @throws Exceptions are method-specific and documented below.
 * @note This source-private type is compiled only through the non-installed
 * single-tenant Job target and exists solely for maintained tests.
 */
class WorkerArtifactDataPlaneTestAccess final {
 public:
  /**
   * @brief Throws immediately after a private temporary file is created.
   * @param directory Existing test-owned directory in which to create it.
   * @return Nothing; successful `mkstemp` is followed by an injected throw.
   * @throws std::invalid_argument when `directory` is empty.
   * @throws std::filesystem::filesystem_error when path conversion fails.
   * @throws std::system_error when `mkstemp` cannot create the occurrence.
   * @throws std::bad_alloc deterministically after successful creation.
   * @note Stack unwinding must remove the exact temporary name and close its
   * descriptor without allocating after `mkstemp` succeeds.
   */
  static void throw_after_private_temporary_creation(
      const std::filesystem::path& directory);
};

}  // namespace ps::server
