#pragma once

#include <chrono>
#include <cstdint>

#include "execution/execution_test_hooks.hpp"

namespace ps::execution_testing {
// Instrumented only in the noninstalled test-kernel. A profile retains its
// observer until every invocation has retired; normal builds emit no clocks.
class ExecutionTiming final {
 public:
  ExecutionTiming(const ExecutionTiming&) = delete;
  ExecutionTiming& operator=(const ExecutionTiming&) = delete;
#ifdef PHOTOSPIDER_ENABLE_EXECUTION_TEST_HOOKS
  explicit ExecutionTiming(TimingKind kind, bool enabled = true) noexcept
      : hook_(enabled ? execution_timing_hook() : nullptr), kind_(kind) {
    if (hook_)
      start_ = Clock::now();
  }
  ~ExecutionTiming() noexcept {
    if (hook_) {
      const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
                               Clock::now() - start_)
                               .count();
      hook_(kind_, bytes_, elapsed > 0 ? elapsed : 0, successful_);
    }
  }
  void success(std::uint64_t bytes) noexcept {
    bytes_ = bytes;
    successful_ = true;
  }

 private:
  using Clock = std::chrono::steady_clock;
  ExecutionTimingHook hook_;
  TimingKind kind_;
  Clock::time_point start_{};
  std::uint64_t bytes_ = 0;
  bool successful_ = false;
#else
  explicit ExecutionTiming(TimingKind, bool = true) noexcept {}
  void success(std::uint64_t) noexcept {}
#endif
};
}  // namespace ps::execution_testing
