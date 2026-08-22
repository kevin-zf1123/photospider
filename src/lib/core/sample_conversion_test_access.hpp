#pragma once

#if !defined(PHOTOSPIDER_INTERNAL_SAMPLE_CONVERSION_TESTING)
#error \
    "sample_conversion_test_access.hpp is available only in BUILD_TESTING builds"
#endif

namespace ps::testing {

/**
 * @brief Forces sample affine arithmetic to use binary64 on the current thread.
 *
 * Construction saves the calling thread's prior test mode and selects
 * binary64 for every finite affine forward/reverse map reached through the
 * public sample-conversion path. Destruction restores the saved mode.
 *
 * @throws Nothing.
 * @note This source-private scope exists only in BUILD_TESTING runtime images.
 *       Nested scopes restore correctly, and concurrent threads remain
 *       independent. It changes no endpoint, storage, or policy contract.
 */
class ScopedBinary64AffineForTesting final {
 public:
  /**
   * @brief Saves the current thread's mode and selects binary64 arithmetic.
   * @throws Nothing.
   */
  ScopedBinary64AffineForTesting() noexcept;

  /**
   * @brief Restores the current thread's mode captured at construction.
   * @throws Nothing.
   */
  ~ScopedBinary64AffineForTesting() noexcept;

  /** @brief Prevents copying a thread-local test scope. */
  ScopedBinary64AffineForTesting(const ScopedBinary64AffineForTesting&) =
      delete;
  /** @brief Prevents copy assignment of a thread-local test scope. */
  ScopedBinary64AffineForTesting& operator=(
      const ScopedBinary64AffineForTesting&) = delete;
  /** @brief Prevents moving a thread-local test scope between owners. */
  ScopedBinary64AffineForTesting(ScopedBinary64AffineForTesting&&) = delete;
  /** @brief Prevents move assignment of a thread-local test scope. */
  ScopedBinary64AffineForTesting& operator=(ScopedBinary64AffineForTesting&&) =
      delete;

 private:
  /** @brief Calling thread's mode before this scope was constructed. */
  bool previous_mode_ = false;
};

}  // namespace ps::testing
