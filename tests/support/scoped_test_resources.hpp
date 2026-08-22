#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <ios>
#include <ostream>
#include <stdexcept>
#include <streambuf>
#include <string>
#include <system_error>

/**
 * @file scoped_test_resources.hpp
 * @brief Small RAII owners for test-only streams and temporary directories.
 */

namespace ps::test_support {

/**
 * @brief Owns one atomically created temporary directory until scope exit.
 *
 * @throws std::invalid_argument for an empty or non-leaf label.
 * @throws std::bad_alloc when path construction cannot allocate.
 * @throws std::filesystem::filesystem_error when the temporary root cannot be
 * created or a bounded collision retry is exhausted.
 * @note Construction never deletes a pre-existing path. The destructor removes
 * only the directory instance whose atomic creation this object observed.
 */
class ScopedTempDir final {
 public:
  /**
   * @brief Creates one unique empty directory below the platform temp root.
   * @param label Nonempty leaf-name prefix used only for diagnostics.
   * @throws std::invalid_argument when `label` is empty, `.`/`..`, or contains
   *         a parent path.
   * @throws std::bad_alloc when owned path construction cannot allocate.
   * @throws std::filesystem::filesystem_error when temp-root lookup or atomic
   *         directory creation fails, or all bounded candidates collide.
   * @note A process-local atomic sequence and monotonic timestamp reduce
   * collisions; `create_directory` is the final cross-process authority.
   */
  explicit ScopedTempDir(const std::string& label) {
    const std::filesystem::path label_path(label);
    if (label.empty() || label == "." || label == ".." ||
        label_path.has_parent_path()) {
      throw std::invalid_argument(
          "ScopedTempDir requires one nonempty leaf label.");
    }
    const std::filesystem::path temp_root =
        std::filesystem::temp_directory_path();
    constexpr std::size_t kMaximumAttempts = 64U;
    for (std::size_t attempt = 0U; attempt < kMaximumAttempts; ++attempt) {
      const std::uint64_t sequence =
          next_sequence_.fetch_add(1U, std::memory_order_relaxed);
      const auto timestamp =
          std::chrono::steady_clock::now().time_since_epoch().count();
      root_ = temp_root / (label + "-" + std::to_string(timestamp) + "-" +
                           std::to_string(sequence));
      std::error_code error;
      if (std::filesystem::create_directory(root_, error)) {
        return;
      }
      if (error) {
        throw std::filesystem::filesystem_error(
            "create temporary test directory", root_, error);
      }
    }
    throw std::filesystem::filesystem_error(
        "temporary test directory candidates collided", root_,
        std::make_error_code(std::errc::file_exists));
  }

  /**
   * @brief Prevents duplicate cleanup ownership.
   * @param other Unused source because copying is forbidden.
   * @throws Nothing because this operation is deleted.
   */
  ScopedTempDir(const ScopedTempDir& other) = delete;

  /**
   * @brief Prevents replacing cleanup ownership.
   * @param other Unused source because assignment is forbidden.
   * @return No value because this operation is deleted.
   * @throws Nothing because this operation is deleted.
   */
  ScopedTempDir& operator=(const ScopedTempDir& other) = delete;

  /**
   * @brief Removes the complete owned tree best-effort.
   * @throws Nothing.
   * @note Cleanup errors never replace a test assertion or primary exception.
   */
  ~ScopedTempDir() noexcept {
    std::error_code ignored;
    std::filesystem::remove_all(root_, ignored);
  }

  /**
   * @brief Returns the owned directory path.
   * @return Stable path reference valid until destruction.
   * @throws Nothing.
   */
  const std::filesystem::path& root() const noexcept { return root_; }

 private:
  /** @brief Process-local candidate suffix shared by test threads. */
  static inline std::atomic<std::uint64_t> next_sequence_{0U};

  /** @brief Exact directory created and later removed by this owner. */
  std::filesystem::path root_;
};

/**
 * @brief Restores one output stream's original buffer at scope exit.
 *
 * @throws std::invalid_argument when the replacement buffer is null.
 * @throws std::ios_base::failure when stream-state mutation is configured to
 * throw; a partial constructor mutation is rolled back before propagation.
 * @note The stream and both buffers must outlive this guard. One guard is
 * thread-affine and does not make a process-global stream safe for concurrent
 * redirection.
 */
class ScopedStreamBufferRedirect final {
 public:
  /**
   * @brief Installs a borrowed replacement stream buffer.
   * @param stream Stream whose buffer is temporarily replaced.
   * @param replacement Non-null buffer that outlives this guard.
   * @throws std::invalid_argument when `replacement` is null.
   * @throws std::ios_base::failure when configured stream exceptions reject
   *         replacement; the original buffer is restored before propagation.
   */
  ScopedStreamBufferRedirect(std::ostream& stream, std::streambuf* replacement)
      : stream_(stream) {
    if (replacement == nullptr) {
      throw std::invalid_argument(
          "Stream redirection requires a replacement buffer.");
    }
    original_ = stream_.rdbuf();
    try {
      stream_.rdbuf(replacement);
    } catch (...) {
      try {
        stream_.rdbuf(original_);
      } catch (...) {
        std::terminate();
      }
      throw;
    }
  }

  /**
   * @brief Prevents duplicate restoration ownership.
   * @param other Unused source because copying is forbidden.
   * @throws Nothing because this operation is deleted.
   */
  ScopedStreamBufferRedirect(const ScopedStreamBufferRedirect& other) = delete;

  /**
   * @brief Prevents replacing restoration ownership.
   * @param other Unused source because assignment is forbidden.
   * @return No value because this operation is deleted.
   * @throws Nothing because this operation is deleted.
   */
  ScopedStreamBufferRedirect& operator=(
      const ScopedStreamBufferRedirect& other) = delete;

  /**
   * @brief Restores the exact stream buffer captured at construction.
   * @throws Nothing; restoration failure terminates rather than leaving a
   *         dangling borrowed buffer installed.
   */
  ~ScopedStreamBufferRedirect() noexcept {
    try {
      stream_.rdbuf(original_);
    } catch (...) {
      std::terminate();
    }
  }

 private:
  /** @brief Stream whose buffer is restored by destruction. */
  std::ostream& stream_;

  /** @brief Original buffer borrowed from `stream_`. */
  std::streambuf* original_ = nullptr;
};

}  // namespace ps::test_support
