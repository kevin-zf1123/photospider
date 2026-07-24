/**
 * @file execution_lifecycle_telemetry.cpp
 * @brief Implements bounded source-private execution lifecycle telemetry.
 */
#include "compute/execution_lifecycle_telemetry.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <limits>
#include <memory>
#include <stdexcept>

namespace ps::compute {
namespace {

/**
 * @brief Mints one process-nonreused nonzero diagnostic identity.
 * @param counter Process-lifetime last-issued counter.
 * @param label Stable exhaustion diagnostic.
 * @return Fresh nonzero identity.
 * @throws std::overflow_error before wrap or reuse.
 * @note The relaxed atomic establishes uniqueness, not cross-object ordering.
 */
std::uint64_t mint_identity(std::atomic<std::uint64_t>& counter,
                            const char* label) {
  std::uint64_t observed = counter.load(std::memory_order_relaxed);
  for (;;) {
    if (observed == std::numeric_limits<std::uint64_t>::max()) {
      throw std::overflow_error(label);
    }
    const std::uint64_t candidate = observed + 1U;
    if (counter.compare_exchange_weak(observed, candidate,
                                      std::memory_order_relaxed,
                                      std::memory_order_relaxed)) {
      return candidate;
    }
  }
}

/**
 * @brief Returns the unique service identity counter.
 * @return Process-lifetime monotonic counter.
 * @throws Nothing.
 */
std::atomic<std::uint64_t>& service_identity_counter() noexcept {
  static std::atomic<std::uint64_t> counter{0U};
  return counter;
}

/**
 * @brief Returns the unique telemetry epoch counter.
 * @return Process-lifetime monotonic counter.
 * @throws Nothing.
 */
std::atomic<std::uint64_t>& telemetry_epoch_counter() noexcept {
  static std::atomic<std::uint64_t> counter{0U};
  return counter;
}

/**
 * @brief Mints one nonzero process-nonreused service instance identity.
 * @return Fresh service instance identity.
 * @throws std::overflow_error before wrap or reuse.
 */
std::uint64_t mint_service_identity() {
  return mint_identity(
      service_identity_counter(),
      "ExecutionService instance identity space is exhausted.");
}

/**
 * @brief Mints one nonzero process-nonreused telemetry epoch identity.
 * @return Fresh telemetry epoch identity.
 * @throws std::overflow_error before wrap or reuse.
 */
std::uint64_t mint_telemetry_epoch() {
  return mint_identity(
      telemetry_epoch_counter(),
      "Execution lifecycle telemetry epoch space is exhausted.");
}

/**
 * @brief Converts a steady duration to nonnegative saturating microseconds.
 * @param elapsed Duration since the telemetry origin.
 * @param saturated Receives whether conversion clamped to UINT64_MAX.
 * @return Nonnegative microsecond count.
 * @throws Nothing.
 */
std::uint64_t elapsed_microseconds(std::chrono::steady_clock::duration elapsed,
                                   bool* saturated) noexcept {
  *saturated = false;
  if (elapsed <= std::chrono::steady_clock::duration::zero()) {
    return 0U;
  }
  using Microseconds = std::chrono::microseconds;
  const long double candidate =
      std::chrono::duration<long double, std::micro>(elapsed).count();
  if (candidate >
      static_cast<long double>(std::numeric_limits<std::uint64_t>::max())) {
    *saturated = true;
    return std::numeric_limits<std::uint64_t>::max();
  }
  if (candidate <=
      static_cast<long double>(std::numeric_limits<Microseconds::rep>::max())) {
    const auto converted = std::chrono::duration_cast<Microseconds>(elapsed);
    return converted.count() < 0
               ? 0U
               : static_cast<std::uint64_t>(converted.count());
  }
  return static_cast<std::uint64_t>(candidate);
}

}  // namespace

/** @copydoc ExecutionLifecycleTelemetry::ExecutionLifecycleTelemetry */
ExecutionLifecycleTelemetry::ExecutionLifecycleTelemetry()
    : ring_(std::make_unique<ExecutionLifecycleEvent[]>(
          kExecutionLifecycleTelemetryCapacity)),
      origin_(std::chrono::steady_clock::now()),
      service_instance_id_(mint_service_identity()),
      telemetry_epoch_(mint_telemetry_epoch()) {}  // NOLINT

/** @copydoc ExecutionLifecycleTelemetry::~ExecutionLifecycleTelemetry */
ExecutionLifecycleTelemetry::~ExecutionLifecycleTelemetry() noexcept = default;

/** @copydoc ExecutionLifecycleTelemetry::publish */
std::uint64_t ExecutionLifecycleTelemetry::publish(
    ExecutionLifecycleEventKind kind, ExecutionLifecycleCategory category,
    std::uint64_t graph_instance_id, std::uint64_t run_id,
    std::uint64_t run_group_id, std::uint64_t generation,
    const ExecutionLifecycleCounters& counters) {
  if (kind == ExecutionLifecycleEventKind::ServiceStopped) {
    throw std::invalid_argument(
        "ServiceStopped requires publish_service_stopped().");
  }
  return publish_at(kind, category, graph_instance_id, run_id, run_group_id,
                    generation, counters,
                    std::chrono::steady_clock::now() - origin_, false);
}

/** @copydoc ExecutionLifecycleTelemetry::mark_stopping */
void ExecutionLifecycleTelemetry::mark_stopping(
    std::uint64_t shutdown_generation,
    const ExecutionLifecycleCounters& counters) {
  if (shutdown_generation == 0U) {
    throw std::invalid_argument(
        "Execution lifecycle shutdown generation must be nonzero.");
  }
  std::lock_guard<std::mutex> lock(mutex_);
  if (service_state_ == ExecutionLifecycleServiceState::Stopped) {
    throw std::logic_error("Execution lifecycle telemetry is stopped.");
  }
  if (shutdown_generation_ != 0U &&
      shutdown_generation_ != shutdown_generation) {
    throw std::invalid_argument(
        "Execution lifecycle shutdown generation changed.");
  }
  shutdown_generation_ = shutdown_generation;
  service_state_ = ExecutionLifecycleServiceState::Stopping;
  counters_ = complete_counters_locked(counters);
}

/** @copydoc ExecutionLifecycleTelemetry::publish_service_stopped */
std::uint64_t ExecutionLifecycleTelemetry::publish_service_stopped(
    std::uint64_t shutdown_generation,
    const ExecutionLifecycleCounters& counters) {
  if (shutdown_generation == 0U) {
    throw std::invalid_argument(
        "Execution lifecycle shutdown generation must be nonzero.");
  }
  bool timestamp_saturated = false;
  const std::uint64_t timestamp = elapsed_microseconds(
      std::chrono::steady_clock::now() - origin_, &timestamp_saturated);
  std::lock_guard<std::mutex> lock(mutex_);
  if (service_state_ == ExecutionLifecycleServiceState::Stopped) {
    if (shutdown_generation_ != shutdown_generation) {
      throw std::invalid_argument(
          "Execution lifecycle shutdown generation changed.");
    }
    return final_stop_sequence_;
  }
  if (service_state_ != ExecutionLifecycleServiceState::Stopping) {
    throw std::logic_error(
        "Execution lifecycle telemetry must be Stopping before Stopped.");
  }
  if (shutdown_generation_ != shutdown_generation) {
    throw std::invalid_argument(
        "Execution lifecycle shutdown generation changed.");
  }
  if (next_sequence_ == std::numeric_limits<std::uint64_t>::max()) {
    record_drop_locked();
    return 0U;
  }

  const std::uint64_t sequence = next_sequence_;
  ExecutionLifecycleEvent event;
  event.sequence = sequence;
  event.timestamp_us = timestamp;
  event.timestamp_saturated = timestamp_saturated;
  event.service_instance_id = service_instance_id_;
  event.telemetry_epoch = telemetry_epoch_;
  event.generation = shutdown_generation;
  event.kind = ExecutionLifecycleEventKind::ServiceStopped;
  event.category = ExecutionLifecycleCategory::None;
  event.counters = complete_counters_locked(counters);
  append_locked(event);
  counters_ = event.counters;
  service_state_ = ExecutionLifecycleServiceState::Stopped;
  final_stop_sequence_ = sequence;
  next_sequence_ = std::numeric_limits<std::uint64_t>::max();
  return sequence;
}

/** @copydoc ExecutionLifecycleTelemetry::publish_at */
std::uint64_t ExecutionLifecycleTelemetry::publish_at(
    ExecutionLifecycleEventKind kind, ExecutionLifecycleCategory category,
    std::uint64_t graph_instance_id, std::uint64_t run_id,
    std::uint64_t run_group_id, std::uint64_t generation,
    const ExecutionLifecycleCounters& counters,
    std::chrono::steady_clock::duration elapsed, bool final_stop) {
  bool timestamp_saturated = false;
  const std::uint64_t timestamp =
      elapsed_microseconds(elapsed, &timestamp_saturated);
  std::lock_guard<std::mutex> lock(mutex_);
  if (service_state_ == ExecutionLifecycleServiceState::Stopped) {
    record_drop_locked();
    return 0U;
  }
  if (!final_stop &&
      next_sequence_ >= std::numeric_limits<std::uint64_t>::max() - 1U) {
    record_drop_locked();
    return 0U;
  }
  if (final_stop &&
      next_sequence_ == std::numeric_limits<std::uint64_t>::max()) {
    record_drop_locked();
    return 0U;
  }

  const std::uint64_t sequence = next_sequence_;
  ExecutionLifecycleEvent event;
  event.sequence = sequence;
  event.timestamp_us = timestamp;
  event.timestamp_saturated = timestamp_saturated;
  event.service_instance_id = service_instance_id_;
  event.telemetry_epoch = telemetry_epoch_;
  event.graph_instance_id = graph_instance_id;
  event.run_id = run_id;
  event.run_group_id = run_group_id;
  event.generation = generation;
  event.kind = kind;
  event.category = category;
  event.counters = complete_counters_locked(counters);
  append_locked(event);
  counters_ = event.counters;

  if (final_stop) {
    service_state_ = ExecutionLifecycleServiceState::Stopped;
    final_stop_sequence_ = sequence;
    next_sequence_ = std::numeric_limits<std::uint64_t>::max();
  } else {
    ++next_sequence_;
  }
  return sequence;
}

/** @copydoc ExecutionLifecycleTelemetry::record_drop_locked */
void ExecutionLifecycleTelemetry::record_drop_locked() noexcept {
  if (global_dropped_total_ < std::numeric_limits<std::uint64_t>::max()) {
    ++global_dropped_total_;
    return;
  }
  global_dropped_saturated_ = true;
}

/** @copydoc ExecutionLifecycleTelemetry::append_locked */
void ExecutionLifecycleTelemetry::append_locked(
    const ExecutionLifecycleEvent& event) noexcept {
  if (retained_count_ < kExecutionLifecycleTelemetryCapacity) {
    const std::uint32_t index = static_cast<std::uint32_t>(
        (static_cast<std::uint64_t>(head_) + retained_count_) %
        kExecutionLifecycleTelemetryCapacity);
    ring_[index] = event;
    ++retained_count_;
    return;
  }
  ring_[head_] = event;
  head_ = static_cast<std::uint32_t>((static_cast<std::uint64_t>(head_) + 1U) %
                                     kExecutionLifecycleTelemetryCapacity);
  record_drop_locked();
}

/** @copydoc ExecutionLifecycleTelemetry::increment_physical_counter */
void ExecutionLifecycleTelemetry::increment_physical_counter(
    ExecutionLifecyclePhysicalCounter counter) noexcept {
  try {
    std::lock_guard<std::mutex> lock(mutex_);
    std::uint64_t& value = physical_counter_locked(counter);
    if (value == std::numeric_limits<std::uint64_t>::max()) {
      std::terminate();
    }
    ++value;
  } catch (...) {
    std::terminate();
  }
}

/** @copydoc ExecutionLifecycleTelemetry::decrement_physical_counter */
void ExecutionLifecycleTelemetry::decrement_physical_counter(
    ExecutionLifecyclePhysicalCounter counter) noexcept {
  try {
    std::lock_guard<std::mutex> lock(mutex_);
    std::uint64_t& value = physical_counter_locked(counter);
    if (value == 0U) {
      std::terminate();
    }
    --value;
  } catch (...) {
    std::terminate();
  }
}

/** @copydoc ExecutionLifecycleTelemetry::physical_counters_zero */
bool ExecutionLifecycleTelemetry::physical_counters_zero() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return counters_.ready_entry_count == 0U &&
         counters_.entered_callback_count == 0U &&
         counters_.live_root_reservation_count == 0U &&
         counters_.live_child_grant_count == 0U &&
         counters_.live_policy_invocation_count == 0U &&
         counters_.live_policy_binding_count == 0U;
}

/** @copydoc ExecutionLifecycleTelemetry::physical_counter_locked */
std::uint64_t& ExecutionLifecycleTelemetry::physical_counter_locked(
    ExecutionLifecyclePhysicalCounter counter) noexcept {
  switch (counter) {
    case ExecutionLifecyclePhysicalCounter::ReadyEntry:
      return counters_.ready_entry_count;
    case ExecutionLifecyclePhysicalCounter::EnteredCallback:
      return counters_.entered_callback_count;
    case ExecutionLifecyclePhysicalCounter::LiveRootReservation:
      return counters_.live_root_reservation_count;
    case ExecutionLifecyclePhysicalCounter::LiveChildGrant:
      return counters_.live_child_grant_count;
    case ExecutionLifecyclePhysicalCounter::LivePolicyInvocation:
      return counters_.live_policy_invocation_count;
    case ExecutionLifecyclePhysicalCounter::LivePolicyBinding:
      return counters_.live_policy_binding_count;
  }
  std::terminate();
}

/** @copydoc ExecutionLifecycleTelemetry::complete_counters_locked */
ExecutionLifecycleCounters
ExecutionLifecycleTelemetry::complete_counters_locked(
    const ExecutionLifecycleCounters& counters) const noexcept {
  ExecutionLifecycleCounters complete = counters;
  complete.ready_entry_count = counters_.ready_entry_count;
  complete.entered_callback_count = counters_.entered_callback_count;
  complete.live_root_reservation_count = counters_.live_root_reservation_count;
  complete.live_child_grant_count = counters_.live_child_grant_count;
  complete.live_policy_invocation_count =
      counters_.live_policy_invocation_count;
  complete.live_policy_binding_count = counters_.live_policy_binding_count;
  return complete;
}

/** @copydoc ExecutionLifecycleTelemetry::snapshot */
ExecutionLifecyclePage ExecutionLifecycleTelemetry::snapshot(
    std::uint64_t after_cursor, std::uint32_t limit) const {
  if (limit < kExecutionLifecycleTelemetryMinPageSize ||
      limit > kExecutionLifecycleTelemetryMaxPageSize) {
    throw std::invalid_argument(
        "Execution lifecycle page limit must be in [1,4096].");
  }

  ExecutionLifecyclePage page;
  page.records.reserve(limit);
  std::lock_guard<std::mutex> lock(mutex_);
  page.service_instance_id = service_instance_id_;
  page.telemetry_epoch = telemetry_epoch_;
  page.service_state = service_state_;
  page.shutdown_generation = shutdown_generation_;
  page.next_sequence = next_sequence_;
  page.global_dropped_total = global_dropped_total_;
  page.global_dropped_saturated = global_dropped_saturated_;
  page.counters = counters_;
  if (retained_count_ == 0U) {
    if (after_cursor != 0U &&
        !(after_cursor == std::numeric_limits<std::uint64_t>::max() &&
          next_sequence_ == std::numeric_limits<std::uint64_t>::max())) {
      throw std::invalid_argument(
          "Execution lifecycle cursor exceeds the empty snapshot cut.");
    }
    page.next_cursor = after_cursor == std::numeric_limits<std::uint64_t>::max()
                           ? after_cursor
                           : 0U;
    return page;
  }

  const ExecutionLifecycleEvent& first = ring_[head_];
  const std::uint32_t last_index = static_cast<std::uint32_t>(
      (static_cast<std::uint64_t>(head_) + retained_count_ - 1U) %
      kExecutionLifecycleTelemetryCapacity);
  const ExecutionLifecycleEvent& last = ring_[last_index];
  page.first_retained_sequence = first.sequence;
  page.snapshot_cut = last.sequence;

  if (after_cursor == std::numeric_limits<std::uint64_t>::max()) {
    if (next_sequence_ != std::numeric_limits<std::uint64_t>::max()) {
      throw std::invalid_argument(
          "Execution lifecycle exhausted sentinel is not yet valid.");
    }
    page.next_cursor = after_cursor;
    return page;
  }
  if (after_cursor > page.snapshot_cut) {
    throw std::invalid_argument(
        "Execution lifecycle cursor exceeds the snapshot cut.");
  }

  std::uint64_t effective_cursor = after_cursor;
  if (after_cursor != 0U && after_cursor < page.first_retained_sequence - 1U) {
    page.cursor_gap = (page.first_retained_sequence - 1U) - after_cursor;
    effective_cursor = page.first_retained_sequence - 1U;
  }

  std::uint32_t visited = 0U;
  std::uint32_t matching_after_page = 0U;
  while (visited < retained_count_) {
    const std::uint32_t index = static_cast<std::uint32_t>(
        (static_cast<std::uint64_t>(head_) + visited) %
        kExecutionLifecycleTelemetryCapacity);
    const ExecutionLifecycleEvent& event = ring_[index];
    if (event.sequence > effective_cursor &&
        event.sequence <= page.snapshot_cut) {
      if (page.records.size() < limit) {
        page.records.push_back(event);
      } else {
        ++matching_after_page;
      }
    }
    ++visited;
  }
  page.has_more = matching_after_page != 0U;
  page.next_cursor =
      page.records.empty() ? page.snapshot_cut : page.records.back().sequence;
  return page;
}

}  // namespace ps::compute
