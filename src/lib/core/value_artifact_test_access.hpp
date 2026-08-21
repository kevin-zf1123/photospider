#pragma once

#if !defined(PHOTOSPIDER_INTERNAL_VALUE_ARTIFACT_TESTING)
#error \
    "value_artifact_test_access.hpp is available only in BUILD_TESTING builds"
#endif

#include <cstdint>

namespace ps::testing {

/**
 * @brief Snapshot of the production named-artifact archive sizing pass.
 *
 * @throws Nothing for ordinary aggregate operations.
 * @note This source-private observation exists only in BUILD_TESTING runtime
 *       images. Sizes are recorded by the real encoder after canonical
 *       metadata and every aligned payload span have been calculated.
 */
struct ValueArtifactArchiveSizingObservationForTesting final {
  /** @brief Whether the encoder completed its checked sizing pass. */
  bool observed = false;
  /** @brief Exact encoded metadata prefix bytes. */
  std::uint64_t metadata_bytes = 0U;
  /** @brief Exact aggregate raw payload bytes without alignment padding. */
  std::uint64_t payload_bytes = 0U;
  /** @brief Exact complete archive bytes including alignment padding. */
  std::uint64_t archive_bytes = 0U;
};

/**
 * @brief Scales frozen named-artifact archive limits on the current thread.
 *
 * Construction saves the calling thread's prior source-private limits and
 * observation, installs smaller positive metadata/payload limits, and clears
 * the current observation. Destruction restores the saved state.
 *
 * @throws std::invalid_argument when either limit is zero or exceeds its
 *         production counterpart.
 * @note This scope exists only in BUILD_TESTING runtime images. Nested scopes
 *       restore correctly, concurrent threads remain independent, and no
 *       installed header, public request, wire field, or production branch is
 *       introduced.
 */
class ScopedValueArtifactArchiveLimitsForTesting final {
 public:
  /**
   * @brief Installs smaller positive archive component limits.
   * @param metadata_bytes Maximum canonical metadata-prefix bytes.
   * @param payload_bytes Maximum aggregate raw payload bytes.
   * @throws std::invalid_argument when a limit is zero or exceeds production.
   */
  ScopedValueArtifactArchiveLimitsForTesting(std::uint64_t metadata_bytes,
                                             std::uint64_t payload_bytes);

  /**
   * @brief Restores the calling thread's prior limits and observation.
   * @throws Nothing.
   */
  ~ScopedValueArtifactArchiveLimitsForTesting() noexcept;

  /** @brief Prevents copying a thread-local test scope. */
  ScopedValueArtifactArchiveLimitsForTesting(
      const ScopedValueArtifactArchiveLimitsForTesting&) = delete;
  /** @brief Prevents copy assignment of a thread-local test scope. */
  ScopedValueArtifactArchiveLimitsForTesting& operator=(
      const ScopedValueArtifactArchiveLimitsForTesting&) = delete;
  /** @brief Prevents moving a thread-local test scope between owners. */
  ScopedValueArtifactArchiveLimitsForTesting(
      ScopedValueArtifactArchiveLimitsForTesting&&) = delete;
  /** @brief Prevents move assignment of a thread-local test scope. */
  ScopedValueArtifactArchiveLimitsForTesting& operator=(
      ScopedValueArtifactArchiveLimitsForTesting&&) = delete;

  /**
   * @brief Returns the calling thread's latest production sizing observation.
   * @return Snapshot recorded by the real named-artifact encoder.
   * @throws Nothing.
   */
  ValueArtifactArchiveSizingObservationForTesting observation() const noexcept;

 private:
  /** @brief Calling thread's metadata limit before construction. */
  std::uint64_t previous_metadata_bytes_ = 0U;
  /** @brief Calling thread's payload limit before construction. */
  std::uint64_t previous_payload_bytes_ = 0U;
  /** @brief Calling thread's observation before construction. */
  ValueArtifactArchiveSizingObservationForTesting previous_observation_;
};

}  // namespace ps::testing
