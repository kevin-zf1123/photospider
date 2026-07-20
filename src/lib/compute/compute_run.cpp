/**
 * @file compute_run.cpp
 * @brief Implements request-owned single-domain ComputeRun identity, state,
 * terminal arbitration, and temporary storage.
 */
#include "compute/compute_run.hpp"

#include <atomic>
#include <cstddef>
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
 * @brief Validates current single-domain Run values before descriptor creation.
 *
 * @param submission Candidate HP Run inputs.
 * @return Nothing.
 * @throws std::invalid_argument for unsupported intent, mismatched quality,
 * zero QoS weight, or zero maximum parallelism.
 * @note Empty graph identity remains valid for direct private service callers;
 * the Kernel product path supplies its stable session name.
 */
void validate_submission(const ComputeRunSubmission& submission) {
  if (submission.intent != ComputeIntent::GlobalHighPrecision &&
      submission.intent != ComputeIntent::RealTimeUpdate) {
    throw std::invalid_argument(
        "ComputeRun requires one supported compute intent.");
  }
  if (submission.intent == ComputeIntent::GlobalHighPrecision &&
      submission.quality != ComputeRunQuality::Full) {
    throw std::invalid_argument(
        "GlobalHighPrecision ComputeRun requires full quality.");
  }
  if (submission.intent == ComputeIntent::RealTimeUpdate &&
      submission.quality != ComputeRunQuality::Interactive) {
    throw std::invalid_argument(
        "RealTimeUpdate ComputeRun requires interactive quality.");
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
 * @brief Shared private lifetime and mutable state for one ComputeRun.
 *
 * The request observer and every active ComputeRunLease retain this control
 * block. It owns all Run-local execution storage and tracks lease-count
 * quiescence independently from terminal publication.
 *
 * @throws std::invalid_argument, std::overflow_error, or std::bad_alloc from
 * validated descriptor construction.
 * @note The class is defined only in this translation unit and is not an ABI
 * surface.
 */
class ComputeRunControl {
 public:
  /**
   * @brief Constructs a new control block with a fresh Run identity.
   *
   * @param submission Immutable request inputs transferred into the descriptor.
   * @throws std::invalid_argument for unsupported intent or invalid QoS.
   * @throws std::overflow_error when Run identity values are exhausted.
   * @throws std::bad_alloc when descriptor ownership cannot allocate.
   * @note No lease exists until ComputeRun::acquire_lease() is called.
   */
  explicit ComputeRunControl(ComputeRunSubmission submission);

  /**
   * @brief Passively destroys Run-owned plan and staging storage.
   *
   * @throws Nothing.
   * @note Destruction publishes no outcome, requests no cancellation, and
   * creates no new lease.
   */
  ~ComputeRunControl() noexcept;

  /** @brief Immutable identity and request inputs captured before planning. */
  const ComputeRunDescriptor descriptor;

  /** @brief Guards phase, terminal arbiter, and storage installation. */
  mutable std::mutex mutex;

  /** @brief Latest monotonic nonterminal phase before settlement. */
  ComputeRunPhase phase = ComputeRunPhase::Created;

  /** @brief Exactly-one terminal outcome, absent before settlement. */
  std::optional<ComputeRunTerminalOutcome> terminal_outcome;

  /** @brief Optional full HP plan, dependency state, runner, and temp output.
   */
  std::unique_ptr<TaskSubmissionPlan> submission_plan;

  /** @brief Optional standalone dirty HP staging output. */
  std::unique_ptr<HighPrecisionDirtyWriteBuffer> dirty_hp_write_buffer;

  /** @brief Number of live non-forgeable leases retaining this control. */
  std::atomic<std::size_t> active_leases{0};
};

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
 * @brief Constructs shared Run state after validating immutable submission.
 *
 * @param submission Candidate request values transferred into the descriptor.
 * @throws std::invalid_argument for unsupported intent or invalid QoS.
 * @throws std::overflow_error when Run id allocation is exhausted.
 * @throws std::bad_alloc when descriptor ownership cannot allocate.
 */
ComputeRunControl::ComputeRunControl(ComputeRunSubmission submission)
    : descriptor([&submission]() {
        validate_submission(submission);
        return ComputeRunDescriptor(ComputeRunId(mint_compute_run_id_value()),
                                    std::move(submission));
      }()) {}

/**
 * @brief Passively releases Run-local storage after all owners disappear.
 *
 * @throws Nothing.
 */
ComputeRunControl::~ComputeRunControl() noexcept = default;

/**
 * @brief Constructs one request observer and shared domain Run control block.
 *
 * @param submission Descriptor inputs captured before HP planning.
 * @throws std::invalid_argument for unsupported intent or invalid QoS.
 * @throws std::overflow_error if Run identity allocation is exhausted.
 * @throws std::bad_alloc if descriptor ownership cannot allocate.
 * @note The fresh id is minted only after semantic validation succeeds; no
 * active lease exists yet.
 */
ComputeRun::ComputeRun(ComputeRunSubmission submission)
    : control_(std::make_shared<ComputeRunControl>(std::move(submission))) {}

/**
 * @brief Passively releases the request observer.
 *
 * @throws Nothing.
 * @note Active leases retain the control block; observer destruction publishes
 * no terminal outcome and requests no cancellation.
 */
ComputeRun::~ComputeRun() noexcept = default;

/**
 * @brief Returns the immutable descriptor from shared control.
 *
 * @return Borrowed descriptor retained by this observer.
 * @throws Nothing.
 */
const ComputeRunDescriptor& ComputeRun::descriptor_ref() const noexcept {
  return control_->descriptor;
}

/**
 * @brief Acquires one active non-forgeable lease before accepting work.
 *
 * @return Strong lease bound to this Run control block.
 * @throws std::logic_error when terminal state is already published.
 * @throws std::system_error if mutex locking fails.
 */
ComputeRunLease ComputeRun::acquire_lease() {
  std::lock_guard<std::mutex> lock(control_->mutex);
  if (control_->terminal_outcome.has_value()) {
    throw std::logic_error("Cannot acquire a lease for a terminal ComputeRun.");
  }
  return ComputeRunLease(control_);
}

/**
 * @brief Reports whether no active Run lease remains.
 *
 * @return true only when the active lease count is zero.
 * @throws Nothing.
 */
bool ComputeRun::is_quiescent() const noexcept {
  return control_->active_leases.load(std::memory_order_acquire) == 0U;
}

/**
 * @brief Returns the current phase under the Run mutex.
 *
 * @return Terminal when settled, otherwise latest nonterminal phase.
 * @throws std::system_error if mutex locking fails.
 */
ComputeRunPhase ComputeRun::phase() const {
  std::lock_guard<std::mutex> lock(control_->mutex);
  return control_->terminal_outcome.has_value() ? ComputeRunPhase::Terminal
                                                : control_->phase;
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
  std::lock_guard<std::mutex> lock(control_->mutex);
  if (control_->terminal_outcome.has_value()) {
    return false;
  }
  const int current_rank = nonterminal_phase_rank(control_->phase);
  if (next_rank < current_rank) {
    throw std::logic_error("ComputeRun phase cannot move backward.");
  }
  if (next_rank == current_rank) {
    return false;
  }
  control_->phase = next;
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
  std::lock_guard<std::mutex> lock(control_->mutex);
  return control_->terminal_outcome;
}

/**
 * @brief Reports whether the terminal arbiter has been claimed.
 *
 * @return true after one terminal publication.
 * @throws std::system_error if mutex locking fails.
 */
bool ComputeRun::is_terminal() const {
  std::lock_guard<std::mutex> lock(control_->mutex);
  return control_->terminal_outcome.has_value();
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
 * @note Shared control retains the plan through every accepted full-HP lease.
 */
TaskSubmissionPlan& ComputeRun::emplace_submission_plan(
    GraphModel& graph, GraphTraversalService& traversal, int node_id,
    std::vector<Device> available_devices) {
  std::lock_guard<std::mutex> lock(control_->mutex);
  if (control_->terminal_outcome.has_value()) {
    throw std::logic_error(
        "Cannot install submission plan on a terminal ComputeRun.");
  }
  if (control_->submission_plan) {
    throw std::logic_error("ComputeRun already owns a task submission plan.");
  }
  control_->submission_plan = std::make_unique<TaskSubmissionPlan>(
      control_->descriptor.id(), graph, traversal, node_id,
      std::move(available_devices));
  return *control_->submission_plan;
}

/**
 * @brief Returns the installed full HP submission plan.
 *
 * @return Borrowed plan pointer or nullptr.
 * @throws std::system_error if mutex locking fails.
 */
TaskSubmissionPlan* ComputeRun::submission_plan() {
  std::lock_guard<std::mutex> lock(control_->mutex);
  return control_->submission_plan.get();
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
  std::lock_guard<std::mutex> lock(control_->mutex);
  if (control_->terminal_outcome.has_value()) {
    throw std::logic_error(
        "Cannot install dirty HP storage on a terminal ComputeRun.");
  }
  if (control_->dirty_hp_write_buffer) {
    throw std::logic_error("ComputeRun already owns a dirty HP write buffer.");
  }
  control_->dirty_hp_write_buffer =
      std::make_unique<HighPrecisionDirtyWriteBuffer>(seed_existing_outputs);
  return *control_->dirty_hp_write_buffer;
}

/**
 * @brief Returns the installed standalone dirty HP staging buffer.
 *
 * @return Borrowed buffer pointer or nullptr.
 * @throws std::system_error if mutex locking fails.
 */
HighPrecisionDirtyWriteBuffer* ComputeRun::dirty_hp_write_buffer() {
  std::lock_guard<std::mutex> lock(control_->mutex);
  return control_->dirty_hp_write_buffer.get();
}

/**
 * @brief Claims the exact-once terminal outcome under one mutex.
 *
 * @param outcome Fully formed success, failure, or cancellation value.
 * @return true when accepted, false after any earlier outcome.
 * @throws std::system_error if mutex locking fails.
 */
bool ComputeRun::publish_terminal(ComputeRunTerminalOutcome outcome) {
  std::lock_guard<std::mutex> lock(control_->mutex);
  if (control_->terminal_outcome.has_value()) {
    return false;
  }
  control_->terminal_outcome = std::move(outcome);
  return true;
}

/**
 * @brief Mints the first active lease for one shared Run control block.
 *
 * @param control Control block retained by the new lease.
 * @throws Nothing.
 */
ComputeRunLease::ComputeRunLease(
    std::shared_ptr<ComputeRunControl> control) noexcept
    : control_(std::move(control)) {
  retain();
}

/**
 * @brief Copies one active lease and increments quiescence accounting.
 *
 * @param other Existing lease to the same or another Run.
 * @throws Nothing.
 */
ComputeRunLease::ComputeRunLease(const ComputeRunLease& other) noexcept
    : control_(other.control_) {
  retain();
}

/**
 * @brief Replaces this lease with a retained copy.
 *
 * @param other Existing lease to retain.
 * @return Reference to this lease.
 * @throws Nothing.
 */
ComputeRunLease& ComputeRunLease::operator=(
    const ComputeRunLease& other) noexcept {
  if (this == &other) {
    return *this;
  }
  release();
  control_ = other.control_;
  retain();
  return *this;
}

/**
 * @brief Transfers one active lease without changing its count.
 *
 * @param other Lease left empty.
 * @throws Nothing.
 */
ComputeRunLease::ComputeRunLease(ComputeRunLease&& other) noexcept
    : control_(std::move(other.control_)) {}

/**
 * @brief Replaces this lease by transferring another lease.
 *
 * @param other Lease left empty.
 * @return Reference to this lease.
 * @throws Nothing.
 */
ComputeRunLease& ComputeRunLease::operator=(ComputeRunLease&& other) noexcept {
  if (this == &other) {
    return *this;
  }
  release();
  control_ = std::move(other.control_);
  return *this;
}

/**
 * @brief Passively releases one active lease.
 *
 * @throws Nothing.
 */
ComputeRunLease::~ComputeRunLease() noexcept {
  release();
}

/**
 * @brief Increments active lease accounting for the retained control.
 *
 * @return Nothing.
 * @throws Nothing.
 */
void ComputeRunLease::retain() noexcept {
  if (control_) {
    control_->active_leases.fetch_add(1U, std::memory_order_acq_rel);
  }
}

/**
 * @brief Decrements active lease accounting and releases shared ownership.
 *
 * @return Nothing.
 * @throws Nothing.
 * @note Counter underflow indicates an internal ownership defect and
 * terminates instead of silently corrupting quiescence.
 */
void ComputeRunLease::release() noexcept {
  if (!control_) {
    return;
  }
  const std::size_t previous =
      control_->active_leases.fetch_sub(1U, std::memory_order_acq_rel);
  if (previous == 0U) {
    std::terminate();
  }
  control_.reset();
}

/**
 * @brief Returns the immutable descriptor retained by this lease.
 *
 * @return Borrowed descriptor valid for the lease lifetime.
 * @throws Nothing.
 */
const ComputeRunDescriptor& ComputeRunLease::descriptor() const noexcept {
  return control_->descriptor;
}

/**
 * @brief Creates a composite identity in the retained Run namespace.
 *
 * @param local_task_id Dense Run-local task value.
 * @return Composite Run/local identity.
 * @throws Nothing.
 */
ComputeRunTaskIdentity ComputeRunLease::task_identity(
    uint64_t local_task_id) const noexcept {
  return ComputeRunTaskIdentity(control_->descriptor.id(),
                                ComputeRunLocalTaskId(local_task_id));
}

/**
 * @brief Tests whether a composite identity is registered by this Run plan.
 *
 * @param identity Candidate Run/local identity.
 * @return true only for a matching Run and registered local task.
 * @throws std::system_error if mutex locking fails.
 */
bool ComputeRunLease::accepts_task_identity(
    const ComputeRunTaskIdentity& identity) const {
  std::lock_guard<std::mutex> lock(control_->mutex);
  return control_->submission_plan != nullptr &&
         control_->submission_plan->contains_task_identity(identity);
}

/**
 * @brief Publishes one matching task failure through the Run terminal arbiter.
 *
 * @param identity Identity of the registered failing task.
 * @param failure Exact non-null worker exception.
 * @return true only when the identity matched and failure won.
 * @throws std::invalid_argument when failure is null.
 * @throws std::system_error if mutex locking fails.
 */
bool ComputeRunLease::publish_task_failure(
    const ComputeRunTaskIdentity& identity, std::exception_ptr failure) {
  if (!failure) {
    throw std::invalid_argument(
        "ComputeRun task failure requires an exception.");
  }
  std::lock_guard<std::mutex> lock(control_->mutex);
  if (control_->submission_plan == nullptr ||
      !control_->submission_plan->contains_task_identity(identity) ||
      control_->terminal_outcome.has_value()) {
    return false;
  }
  control_->terminal_outcome = ComputeRunTerminalOutcome{
      ComputeRunTerminalKind::Failed, std::move(failure), std::nullopt};
  return true;
}

/**
 * @brief Copies terminal outcome while this lease retains the Run.
 *
 * @return Outcome snapshot or nullopt.
 * @throws std::system_error if mutex locking fails.
 */
std::optional<ComputeRunTerminalOutcome> ComputeRunLease::terminal_outcome()
    const {
  std::lock_guard<std::mutex> lock(control_->mutex);
  return control_->terminal_outcome;
}

/**
 * @brief Routes one accepted callback into its matching Run-owned plan.
 *
 * @param identity Composite task identity carried by the callback.
 * @param task_runtime Active scheduler runtime.
 * @return Nothing.
 * @throws std::invalid_argument for mismatched or unregistered identity.
 * @throws std::system_error if the control mutex cannot be locked.
 * @throws Exceptions propagated by Run-owned plan execution, scheduler
 * completion accounting, dependent-callback submission, or matching failure
 * publication.
 * @note A matching accepted callback observed after terminal publication
 * releases its own completion unit without entering the plan. Active valid
 * failures are passed to this Run's failure publisher before unchanged
 * rethrow; an exception from that publication propagates instead.
 */
void ComputeRunLease::execute_task(const ComputeRunTaskIdentity& identity,
                                   SchedulerTaskRuntime& task_runtime) {
  TaskSubmissionPlan* plan = nullptr;
  bool skip_terminal_run = false;
  {
    std::lock_guard<std::mutex> lock(control_->mutex);
    if (control_->submission_plan == nullptr ||
        !control_->submission_plan->contains_task_identity(identity)) {
      throw std::invalid_argument(
          "ComputeRun task identity does not match its retaining lease.");
    }
    if (control_->terminal_outcome.has_value()) {
      skip_terminal_run = true;
    } else {
      plan = control_->submission_plan.get();
    }
  }
  if (skip_terminal_run) {
    task_runtime.dec_tasks_to_complete();
    return;
  }

  try {
    plan->execute_task(identity, *this, task_runtime);
  } catch (...) {
    const std::exception_ptr failure = std::current_exception();
    (void)publish_task_failure(identity, failure);
    std::rethrow_exception(failure);
  }
}

/**
 * @brief Executes initial-ready discovery and submission under this lease.
 *
 * @param task_runtime Active scheduler batch initialized by the dispatcher.
 * @return Nothing.
 * @throws GraphError or standard exceptions from plan bootstrap and runtime.
 * @note An accepted bootstrap observed after terminal publication releases its
 * own completion unit without submitting planned work. Active bootstrap
 * failures publish directly to this Run because bootstrap has no planned local
 * task identity.
 */
void ComputeRunLease::execute_bootstrap(SchedulerTaskRuntime& task_runtime) {
  TaskSubmissionPlan* plan = nullptr;
  bool skip_terminal_run = false;
  {
    std::lock_guard<std::mutex> lock(control_->mutex);
    if (control_->submission_plan == nullptr) {
      throw std::logic_error(
          "ComputeRun bootstrap requires an installed submission plan.");
    }
    if (control_->terminal_outcome.has_value()) {
      skip_terminal_run = true;
    } else {
      plan = control_->submission_plan.get();
    }
  }
  if (skip_terminal_run) {
    task_runtime.dec_tasks_to_complete();
    return;
  }

  try {
    plan->submit_initial_ready_tasks(*this, task_runtime);
    task_runtime.dec_tasks_to_complete();
  } catch (...) {
    const std::exception_ptr failure = std::current_exception();
    {
      std::lock_guard<std::mutex> lock(control_->mutex);
      if (!control_->terminal_outcome.has_value()) {
        control_->terminal_outcome = ComputeRunTerminalOutcome{
            ComputeRunTerminalKind::Failed, failure, std::nullopt};
      }
    }
    std::rethrow_exception(failure);
  }
}

}  // namespace ps::compute
