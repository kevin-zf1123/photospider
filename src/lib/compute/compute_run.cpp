/**
 * @file compute_run.cpp
 * @brief Implements request-owned high-precision ComputeRun identity, state,
 * terminal arbitration, and temporary storage.
 */
#include "compute/compute_run.hpp"

#include <atomic>
#include <limits>
#include <memory>
#include <stdexcept>
#include <utility>
#include <vector>

#include "compute/compute_task_submission.hpp"
#include "compute/dirty_write_buffers.hpp"

namespace ps::compute {
namespace {

/**
 * @brief Mints the next non-reused process-lifetime Run identity value.
 *
 * @return Fresh non-zero opaque id value.
 * @throws std::overflow_error before the atomic sequence would wrap or reuse a
 * value.
 * @note This atomic owns no scheduler, worker, graph, Run, or resource. Issue
 * #68 may relocate value minting into ExecutionService.
 */
uint64_t mint_compute_run_id_value() {
  static std::atomic<uint64_t> next_id{1};
  uint64_t candidate = next_id.load(std::memory_order_relaxed);
  for (;;) {
    if (candidate == std::numeric_limits<uint64_t>::max()) {
      throw std::overflow_error("ComputeRunId sequence exhausted.");
    }
    if (next_id.compare_exchange_weak(candidate, candidate + 1,
                                      std::memory_order_relaxed,
                                      std::memory_order_relaxed)) {
      return candidate;
    }
  }
}

/**
 * @brief Validates issue #66 submission values before descriptor creation.
 *
 * @param submission Candidate HP Run inputs.
 * @return Nothing.
 * @throws std::invalid_argument for non-HP intent, zero QoS weight, or zero
 * maximum parallelism.
 * @note Empty graph identity remains valid for direct private service callers;
 * the Kernel product path supplies its stable session name.
 */
void validate_submission(const ComputeRunSubmission& submission) {
  if (submission.intent != ComputeIntent::GlobalHighPrecision) {
    throw std::invalid_argument(
        "Issue #66 ComputeRun requires GlobalHighPrecision intent.");
  }
  if (submission.qos.weight == 0) {
    throw std::invalid_argument("ComputeRun QoS weight must be positive.");
  }
  if (submission.qos.maximum_parallelism.has_value() &&
      *submission.qos.maximum_parallelism == 0) {
    throw std::invalid_argument(
        "ComputeRun maximum parallelism must be positive when set.");
  }
}

/**
 * @brief Returns the monotonic rank of a nonterminal Run phase.
 *
 * @param phase Phase to rank.
 * @return Increasing rank from Created through CommitPending.
 * @throws std::invalid_argument when phase is Terminal.
 * @note Terminal entry is controlled only by the terminal arbiter.
 */
int nonterminal_phase_rank(ComputeRunPhase phase) {
  switch (phase) {
    case ComputeRunPhase::Created:
      return 0;
    case ComputeRunPhase::Admitted:
      return 1;
    case ComputeRunPhase::Queued:
      return 2;
    case ComputeRunPhase::Running:
      return 3;
    case ComputeRunPhase::CommitPending:
      return 4;
    case ComputeRunPhase::Terminal:
      break;
  }
  throw std::invalid_argument(
      "ComputeRun terminal phase requires terminal publication.");
}

}  // namespace

/**
 * @brief Constructs an immutable descriptor from a fresh id and submission.
 *
 * @param id Fresh opaque Run identity.
 * @param submission Validated request inputs transferred into ownership.
 * @throws std::bad_alloc if graph identity transfer allocates.
 * @note No field is mutated after construction.
 */
ComputeRunDescriptor::ComputeRunDescriptor(ComputeRunId id,
                                           ComputeRunSubmission submission)
    : id_(id),
      graph_identity_(std::move(submission.graph_identity)),
      revision_(submission.revision),
      target_node_id_(submission.target_node_id),
      intent_(submission.intent),
      quality_(submission.quality),
      qos_(submission.qos) {}  // NOLINT(whitespace/indent_namespace)

/**
 * @brief Constructs one request-owned HP Run in Created phase.
 *
 * @param submission Descriptor inputs captured before HP planning.
 * @throws std::invalid_argument for unsupported intent or invalid QoS.
 * @throws std::overflow_error if Run identity allocation is exhausted.
 * @throws std::bad_alloc if descriptor ownership cannot allocate.
 * @note The fresh id is minted only after semantic validation succeeds.
 */
ComputeRun::ComputeRun(ComputeRunSubmission submission)
    : descriptor_([&submission]() {
        validate_submission(submission);
        return ComputeRunDescriptor(ComputeRunId(mint_compute_run_id_value()),
                                    std::move(submission));
      }()) {}

/**
 * @brief Passively destroys Run-owned plan and staging storage.
 *
 * @throws Nothing.
 * @note Destruction does not publish a terminal outcome or interact with the
 * borrowed scheduler runtime.
 */
ComputeRun::~ComputeRun() noexcept = default;

/**
 * @brief Returns the current phase under the Run mutex.
 *
 * @return Terminal when settled, otherwise latest nonterminal phase.
 * @throws std::system_error if mutex locking fails.
 */
ComputeRunPhase ComputeRun::phase() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return terminal_outcome_.has_value() ? ComputeRunPhase::Terminal : phase_;
}

/**
 * @brief Advances the Run to a later nonterminal phase.
 *
 * @param next Requested phase.
 * @return true when advanced; false for same phase or settled Run.
 * @throws std::invalid_argument when next is Terminal.
 * @throws std::logic_error when next moves backward.
 * @throws std::system_error if mutex locking fails.
 */
bool ComputeRun::advance_to(ComputeRunPhase next) {
  const int next_rank = nonterminal_phase_rank(next);
  std::lock_guard<std::mutex> lock(mutex_);
  if (terminal_outcome_.has_value()) {
    return false;
  }
  const int current_rank = nonterminal_phase_rank(phase_);
  if (next_rank < current_rank) {
    throw std::logic_error("ComputeRun phase cannot move backward.");
  }
  if (next_rank == current_rank) {
    return false;
  }
  phase_ = next;
  return true;
}

/**
 * @brief Publishes success through the exact-once terminal arbiter.
 *
 * @return true only when this call wins.
 * @throws std::system_error if mutex locking fails.
 */
bool ComputeRun::publish_succeeded() {
  return publish_terminal(ComputeRunTerminalOutcome{
      ComputeRunTerminalKind::Succeeded, nullptr, std::nullopt});
}

/**
 * @brief Publishes failure with the original exception pointer.
 *
 * @param failure Non-null caught exception identity.
 * @return true only when this call wins.
 * @throws std::invalid_argument when failure is null.
 * @throws std::system_error if mutex locking fails.
 */
bool ComputeRun::publish_failed(std::exception_ptr failure) {
  if (!failure) {
    throw std::invalid_argument(
        "ComputeRun failure publication requires an exception.");
  }
  return publish_terminal(ComputeRunTerminalOutcome{
      ComputeRunTerminalKind::Failed, std::move(failure), std::nullopt});
}

/**
 * @brief Publishes cancellation with a stable reason.
 *
 * @param reason Cancellation cause.
 * @return true only when this call wins.
 * @throws std::system_error if mutex locking fails.
 */
bool ComputeRun::publish_cancelled(ComputeRunCancellationReason reason) {
  return publish_terminal(ComputeRunTerminalOutcome{
      ComputeRunTerminalKind::Cancelled, nullptr, reason});
}

/**
 * @brief Copies the terminal outcome under the Run mutex.
 *
 * @return Outcome snapshot or nullopt before settlement.
 * @throws std::system_error if mutex locking fails.
 */
std::optional<ComputeRunTerminalOutcome> ComputeRun::terminal_outcome() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return terminal_outcome_;
}

/**
 * @brief Reports whether the terminal arbiter has been claimed.
 *
 * @return true after one terminal publication.
 * @throws std::system_error if mutex locking fails.
 */
bool ComputeRun::is_terminal() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return terminal_outcome_.has_value();
}

/**
 * @brief Constructs the Run-owned full HP scheduler submission plan.
 *
 * @param graph Graph used for plan construction and operation resolution.
 * @param traversal Traversal service used by the dispatch-plan builder.
 * @param node_id Requested target node.
 * @param available_devices Scheduler-exposed devices transferred into plan.
 * @return Mutable Run-owned submission plan.
 * @throws std::logic_error for duplicate storage or terminal Run.
 * @throws GraphError or standard exceptions from TaskSubmissionPlan.
 * @note The Run must outlive the synchronous scheduler drain.
 */
TaskSubmissionPlan& ComputeRun::emplace_submission_plan(
    GraphModel& graph, GraphTraversalService& traversal, int node_id,
    std::vector<Device> available_devices) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (terminal_outcome_.has_value()) {
    throw std::logic_error(
        "Cannot install submission plan on a terminal ComputeRun.");
  }
  if (submission_plan_) {
    throw std::logic_error("ComputeRun already owns a task submission plan.");
  }
  submission_plan_ = std::make_unique<TaskSubmissionPlan>(
      graph, traversal, node_id, std::move(available_devices));
  return *submission_plan_;
}

/**
 * @brief Returns the installed full HP submission plan.
 *
 * @return Borrowed plan pointer or nullptr.
 * @throws std::system_error if mutex locking fails.
 */
TaskSubmissionPlan* ComputeRun::submission_plan() {
  std::lock_guard<std::mutex> lock(mutex_);
  return submission_plan_.get();
}

/**
 * @brief Constructs the Run-owned standalone dirty HP staging buffer.
 *
 * @param seed_existing_outputs Whether staged nodes seed current HP output.
 * @return Mutable Run-owned staging buffer.
 * @throws std::logic_error for duplicate storage or terminal Run.
 * @throws std::bad_alloc when allocation fails.
 */
HighPrecisionDirtyWriteBuffer& ComputeRun::emplace_dirty_hp_write_buffer(
    bool seed_existing_outputs) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (terminal_outcome_.has_value()) {
    throw std::logic_error(
        "Cannot install dirty HP storage on a terminal ComputeRun.");
  }
  if (dirty_hp_write_buffer_) {
    throw std::logic_error("ComputeRun already owns a dirty HP write buffer.");
  }
  dirty_hp_write_buffer_ =
      std::make_unique<HighPrecisionDirtyWriteBuffer>(seed_existing_outputs);
  return *dirty_hp_write_buffer_;
}

/**
 * @brief Returns the installed standalone dirty HP staging buffer.
 *
 * @return Borrowed buffer pointer or nullptr.
 * @throws std::system_error if mutex locking fails.
 */
HighPrecisionDirtyWriteBuffer* ComputeRun::dirty_hp_write_buffer() {
  std::lock_guard<std::mutex> lock(mutex_);
  return dirty_hp_write_buffer_.get();
}

/**
 * @brief Claims the exact-once terminal outcome under one mutex.
 *
 * @param outcome Fully formed success, failure, or cancellation value.
 * @return true when accepted, false after any earlier outcome.
 * @throws std::system_error if mutex locking fails.
 */
bool ComputeRun::publish_terminal(ComputeRunTerminalOutcome outcome) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (terminal_outcome_.has_value()) {
    return false;
  }
  terminal_outcome_ = std::move(outcome);
  return true;
}

}  // namespace ps::compute
