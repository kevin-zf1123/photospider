#pragma once

#include <algorithm>
#include <chrono>
#include <cstdint>

#include "core/checked_math.hpp"
#include "photospider/execution/execution.hpp"

namespace ps::execution_internal {
// The containing pool serializes updates and snapshots under its queue mutex.
// Storage is fixed context bootstrap state; observation allocates no memory.
class CallbackQueueMeter final {
 public:
  using Clock = std::chrono::steady_clock;
  explicit CallbackQueueMeter(bool enabled) : enabled_(enabled) {}
  bool enabled() const noexcept { return enabled_; }
  void published(Clock::time_point begin, Clock::time_point end,
                 std::uint64_t queued) noexcept {
    add(&statistics_.accepted_callbacks, 1);
    add(&statistics_.submission_ns, elapsed(begin, end));
    statistics_.maximum_queued_callbacks =
        std::max(statistics_.maximum_queued_callbacks, queued);
  }
  void started(Clock::time_point published,
               Clock::time_point claimed) noexcept {
    const auto wait = elapsed(published, claimed);
    add(&statistics_.started_callbacks, 1);
    add(&statistics_.queue_wait_ns, wait);
    statistics_.maximum_queue_wait_ns =
        std::max(statistics_.maximum_queue_wait_ns, wait);
  }
  CallbackQueueStatistics statistics() const noexcept { return statistics_; }

 private:
  static std::uint64_t elapsed(Clock::time_point begin,
                               Clock::time_point end) noexcept {
    const auto ns =
        std::chrono::duration_cast<std::chrono::nanoseconds>(end - begin)
            .count();
    return ns > 0 ? static_cast<std::uint64_t>(ns) : 0;
  }
  void add(std::uint64_t* value, std::uint64_t delta) noexcept {
    if (!core_internal::can_add(delta, *value)) {
      *value = UINT64_MAX;
      statistics_.saturated = true;
    } else {
      *value += delta;
    }
  }
  const bool enabled_;
  CallbackQueueStatistics statistics_;
};
}  // namespace ps::execution_internal
