#pragma once

#include <array>
#include <chrono>
#include <cstdint>
#include <optional>
#include <vector>

#include "core/resource_observation.hpp"
#include "photospider/compiler/physical_program.hpp"

namespace ps::execution_internal {
enum class TaskState : std::uint8_t {
  Inactive,
  Waiting,
  Ready,
  Submitted,
  Succeeded,
  Failed,
  Skipped
};
enum class CallState : std::uint8_t { Succeeded, Failed, Elided };
enum class AdmissionWait : std::uint8_t { Coordinator, Lane, Context };
struct ReadyWait final {
  std::uint64_t lane_ns = 0, context_ns = 0;
};
struct CompletionKey final {
  std::uint64_t run = 0, activation = 0, submission = 0;
};
struct Submission final {
  CompletionKey key;
  physical::Id task = physical::none;
};
struct CallRetirement final {
  std::uint64_t sequence = 0, activation = 0, source_call = 0;
  physical::Id call = physical::none, failed_task = physical::none;
  CallState state = CallState::Elided;
};

/** @brief Coordinator-owned state for one validated block activation.
 * This class is deliberately single-threaded. Workers publish completion
 * records to the coordinator, which alone calls these methods. It owns no
 * payload and performs no backend calls. The surrounding Run releases holds
 * for completed and skipped tasks, and transfers exports at retirement.
 * Construction allocates all queues/counters before issue; allocation failure
 * throws std::bad_alloc. An optional parent metadata lease covers those arrays;
 * construction commits actual capacities and destruction withdraws them without
 * shrinking the parent's reservation. The program must remain immutable and
 * alive.
 */
class TaskSchedule final {
 public:
  TaskSchedule(const physical::Program& program, physical::Id block,
               std::uint64_t run, std::uint64_t activation,
               std::uint64_t first_submission = 0,
               std::uint64_t first_sequence = 0, ResourceLease metadata = {});
  /** @brief Capacity of scheduler arrays and bounded task failure messages.
   * Used by Run admission before constructing a block activation.
   */
  static std::uint64_t metadata_bytes(const physical::Program& program,
                                      physical::Id block);
  TaskSchedule(const TaskSchedule&) = delete;
  TaskSchedule& operator=(const TaskSchedule&) = delete;
  /** @brief Check eligibility without arming work; discard stale ready entries.
   * Coordinator-only, allocation-free. Effects remain gated by retirement.
   */
  bool has_ready(physical::Backend backend);
  /** @brief Arm one eligible task before calling the backend submit function.
   * Caller must already own its resource/queue credit and completion slot.
   * A rejected submit is completed locally with its failure status; accepted
   * work completes only after quiescence. Inline backend completion is legal.
   */
  std::optional<Submission> issue_next(physical::Backend backend);
  /** @brief Accept exactly one terminal event for an armed submission.
   * Invalid keys/state abort with Internal, without settling another task.
   * Status is copied into preallocated state; failure never enables successors.
   */
  Status complete(CompletionKey key, const Status& status);
  /** @brief Stop admission and retirement, then let accepted work drain.
   * Cancellation outranks Stale, which outranks other abort causes. The first
   * observed cause within a category wins. Task failures remain diagnostic.
   * Fixed fields and at most 256 diagnostic bytes are copied into reserved
   * storage without allocation; caller ownership remains unchanged.
   */
  void abort(const Status& status);
  /** @brief Retire the next settled call in IR order, without user callbacks.
   * Zero-task calls are Elided. A failure record follows the successful older
   * prefix and only after every submitted sibling completes. Caller reserves
   * delivery capacity before consuming the record.
   */
  std::optional<CallRetirement> retire_next();
  /** @brief Newly skipped IDs append once; caller tracks a consumption cursor.
   */
  const std::vector<physical::Id>& skipped() const noexcept { return skipped_; }
  TaskState state(physical::Id task) const { return tasks_.at(task).state; }
  // Integrate coordinator-observed blocking intervals once per backend.
  // A task snapshots these clocks on eligibility and differences them at issue.
  void waiting(physical::Backend backend, AdmissionWait reason) noexcept;
  // Called once after complete block admission and before the first issue.
  void admitted() noexcept;
  ReadyWait ready_wait(physical::Id task) const noexcept {
    return tasks_[task].wait;
  }
  std::chrono::steady_clock::time_point ready_at(physical::Id task) const {
    return tasks_[task].ready_at;
  }
  const Status& failure(physical::Id task) const {
    return tasks_.at(task).failure;
  }
  physical::Id failure_cut() const noexcept { return failure_cut_; }
  physical::Id retirement_head() const noexcept { return head_; }
  bool head_waiting() const noexcept {
    return !aborted() && head_ < block_.calls.size() && head_ <= failure_cut_ &&
           calls_[block_.calls[head_]].unsettled != 0;
  }
  std::uint64_t next_submission() const noexcept { return next_submission_; }
  std::uint64_t next_sequence() const noexcept { return next_sequence_; }
  std::size_t inflight() const noexcept { return inflight_; }
  bool aborted() const noexcept { return !abort_.ok(); }
  bool drained() const noexcept { return unsettled_ == 0; }
  /** @brief Status of the settled block, before Run delivery/finalization.
   * Calling before drain returns Internal. Success also requires every call
   * to have retired. An abort suppresses retirement but still requires drain.
   */
  Status settled_status() const;
  /** @brief Copy the settled diagnostic into caller-owned reserved storage.
   * target must have at least 256 message capacity. Performs no allocation;
   * scheduler storage remains owned and reusable through destruction.
   */
  void copy_settled_status(Status* target) const;

 private:
  struct Task {
    TaskState state = TaskState::Inactive;
    physical::Id pending = 0;
    Status failure;
    std::chrono::steady_clock::time_point ready_at;
    ReadyWait wait;
  };
  struct WaitClock {
    ReadyWait elapsed;
    AdmissionWait reason = AdmissionWait::Coordinator;
    std::chrono::steady_clock::time_point last =
        std::chrono::steady_clock::now();
    ReadyWait sample(std::chrono::steady_clock::time_point now) noexcept;
  };
  struct Call {
    physical::Id unsettled = 0, first_failed = physical::none;
  };
  void make_ready(physical::Id task);
  void skip_from(physical::Id ordinal);
  void activate_effect();
  void activate_boundary();
  bool lower_priority(physical::Id a, physical::Id b) const;

  const physical::Program& program_;
  const physical::BlockDesc& block_;
  std::uint64_t run_, activation_, first_submission_, next_submission_,
      next_sequence_;
  core_internal::MetadataAllocation allocation_;
  std::vector<Task> tasks_;
  std::vector<Call> calls_;
  std::array<std::vector<physical::Id>, 2> ready_;
  std::array<WaitClock, 2> waits_;
  std::vector<physical::Id> submissions_, skipped_;
  physical::Id head_ = 0, failure_cut_ = 0;
  physical::Id suppressed_from_ = 0, boundary_failure_ = physical::none;
  std::size_t unsettled_ = 0, inflight_ = 0;
  bool boundary_suppressed_ = false;
  Status abort_;
};
}  // namespace ps::execution_internal
