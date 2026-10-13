#pragma once

#include <cfenv>  // NOLINT(build/c++11)

namespace ps::plugin_internal {
/** @brief Thread-local nearest/gradual-underflow scope for CPU algorithms.
 * Restores rounding, denormal controls and exception flags after any return.
 * A failed environment setup is reported through active().
 */
class FloatingEnvironment final {
 public:
  FloatingEnvironment() noexcept {
    saved_ = std::fegetenv(&previous_) == 0;
    active_ = saved_ && std::fesetenv(FE_DFL_ENV) == 0;
  }
  ~FloatingEnvironment() noexcept {
    if (saved_)
      std::fesetenv(&previous_);
  }
  FloatingEnvironment(const FloatingEnvironment&) = delete;
  FloatingEnvironment& operator=(const FloatingEnvironment&) = delete;
  bool active() const noexcept { return active_; }

 private:
  std::fenv_t previous_{};
  bool saved_ = false, active_ = false;
};
}  // namespace ps::plugin_internal
