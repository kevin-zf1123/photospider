/**
 * @file scoped_descriptor_pressure.hpp
 * @brief Provides Linux descriptor-allocation pressure for DSO identity tests.
 */
#pragma once

#include <cstddef>
#include <system_error>
#include <vector>

#if defined(__linux__)
#include <fcntl.h>
#include <unistd.h>

#include <cerrno>
#endif

namespace ps::test {

/**
 * @brief Retains a bounded set of inert descriptors for one test scope.
 *
 * Linux trust regression tests use this owner to move sealed snapshot
 * descriptors away from incidental low-number allocations while preserving
 * deterministic lowest-free-descriptor reuse between sequential admissions.
 *
 * @throws std::system_error if Linux cannot open `/dev/null`.
 * @throws std::bad_alloc if descriptor bookkeeping cannot allocate.
 * @note This is observation pressure only. It does not create trust authority,
 * inspect production descriptors, or alter the native loader contract.
 */
class ScopedDescriptorPressure final {
 public:
  /**
   * @brief Opens and retains `descriptor_count` inert Linux descriptors.
   * @param descriptor_count Exact bounded pressure count selected by the test.
   * @throws std::system_error when one `/dev/null` open fails.
   * @throws std::bad_alloc when descriptor storage cannot allocate.
   * @note Non-Linux builds retain no descriptors because native identity tests
   * are not registered there.
   */
  explicit ScopedDescriptorPressure(std::size_t descriptor_count) {
#if defined(__linux__)
    try {
      descriptors_.reserve(descriptor_count);
      for (std::size_t index = 0U; index < descriptor_count; ++index) {
        const int descriptor = ::open("/dev/null", O_RDONLY | O_CLOEXEC);
        if (descriptor < 0) {
          throw std::system_error(errno, std::generic_category(),
                                  "open descriptor-pressure source");
        }
        try {
          descriptors_.push_back(descriptor);
        } catch (...) {
          (void)::close(descriptor);
          throw;
        }
      }
    } catch (...) {
      close_all();
      throw;
    }
#else
    static_cast<void>(descriptor_count);
#endif
  }

  /**
   * @brief Closes every retained descriptor in reverse allocation order.
   * @throws Nothing; close failures are intentionally ignored in test cleanup.
   */
  ~ScopedDescriptorPressure() noexcept { close_all(); }

  /** @brief Prevents duplicating descriptor ownership. */
  ScopedDescriptorPressure(const ScopedDescriptorPressure&) = delete;

  /** @brief Prevents assigning duplicate descriptor ownership. */
  ScopedDescriptorPressure& operator=(const ScopedDescriptorPressure&) = delete;

  /** @brief Prevents moving descriptors away from their lexical test scope. */
  ScopedDescriptorPressure(ScopedDescriptorPressure&&) = delete;

  /** @brief Prevents move-assigning lexical descriptor pressure. */
  ScopedDescriptorPressure& operator=(ScopedDescriptorPressure&&) = delete;

  /**
   * @brief Returns the number of currently retained pressure descriptors.
   * @return Exact Linux count, or zero on unsupported platforms.
   * @throws Nothing.
   */
  std::size_t size() const noexcept { return descriptors_.size(); }

 private:
  /**
   * @brief Closes and clears all retained Linux descriptors.
   * @return Nothing.
   * @throws Nothing.
   */
  void close_all() noexcept {
#if defined(__linux__)
    for (auto iterator = descriptors_.rbegin(); iterator != descriptors_.rend();
         ++iterator) {
      (void)::close(*iterator);
    }
#endif
    descriptors_.clear();
  }

  /** @brief Owned inert Linux descriptors in allocation order. */
  std::vector<int> descriptors_;
};

}  // namespace ps::test
