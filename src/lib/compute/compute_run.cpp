/**
 * @file compute_run.cpp
 * @brief Implements request-owned single-domain ComputeRun identity, state,
 * terminal arbitration, and temporary storage.
 */
#include "compute/compute_run.hpp"

#include <atomic>
#include <cstddef>
#include <functional>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "compute/compute_task_submission.hpp"
#include "compute/dirty_write_buffers.hpp"
#include "compute/resource_demand_estimator.hpp"

namespace ps::compute {
namespace {

/**
 * @brief Mints the next non-reused process-lifetime Run identity value.
 *
 * @return Fresh non-zero opaque id value.
 * @throws std::overflow_error before the atomic sequence would wrap or reuse a
 * value.
 * @note This ownership-neutral atomic owns no queue, worker, Graph, Run
 * lifecycle, or resource policy.
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
 * @param submission Candidate single-domain HP or RT child inputs.
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
  if (submission.supersession.key.target_node_id() !=
      submission.target_node_id) {
    throw std::invalid_argument(
        "ComputeRun supersession key must match its target node.");
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
 * @brief Serializes one registered cancellation callback with deactivation.
 *
 * @throws std::bad_alloc when callback ownership is created.
 * @note Invocation holds this slot's mutex but never the Run-control mutex.
 * Registration owners must not destroy their own slot recursively from inside
 * the callback. Production service and sibling callbacks satisfy that rule.
 */
class ComputeRunCancellationSlot final {
 public:
  /**
   * @brief Owns one non-empty cancellation cleanup callback.
   * @param callback Callback invoked with the stable accepted reason.
   * @throws std::bad_alloc when callable ownership allocates.
   */
  explicit ComputeRunCancellationSlot(
      std::function<void(ComputeRunCancellationReason)> callback)
      : callback_(std::move(callback)) {}

  /**
   * @brief Invokes the callback once when the slot remains active.
   * @param reason Stable Run cancellation reason.
   * @return Nothing.
   * @throws Any callback exception unchanged to the cancellation requester.
   */
  void invoke(ComputeRunCancellationReason reason) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (active_) {
      callback_(reason);
    }
  }

  /**
   * @brief Prevents future invocation and synchronizes with an active callback.
   * @return Nothing.
   * @throws std::system_error when mutex locking fails.
   */
  void deactivate() {
    std::lock_guard<std::mutex> lock(mutex_);
    active_ = false;
  }

 private:
  /** @brief Serializes invocation and registration destruction. */
  std::mutex mutex_;

  /** @brief True until the move-only registration is released. */
  bool active_ = true;

  /** @brief Cleanup callback retained independently from Run control lifetime.
   */
  std::function<void(ComputeRunCancellationReason)> callback_;
};

namespace {

/**
 * @brief Internal ownership state of the shared terminal/commit arbiter.
 * @throws Nothing for value operations.
 */
enum class ComputeRunArbiterState : std::uint8_t {
  /** @brief Cancellation, failure, or a commit contender may still claim. */
  Open,
  /** @brief One commit contender exclusively owns terminal resolution. */
  CommitClaimed,
  /** @brief Exactly one terminal outcome has been published. */
  Terminal,
};

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
   * @param monotonic_clock Non-empty private clock used for deadline
   * observation.
   * @throws std::invalid_argument for unsupported intent or invalid QoS.
   * @throws std::overflow_error when Run identity values are exhausted.
   * @throws std::bad_alloc when descriptor ownership cannot allocate.
   * @note No lease exists until ComputeRun::acquire_lease() is called.
   */
  ComputeRunControl(ComputeRunSubmission submission,
                    ComputeRunMonotonicClock monotonic_clock);

  /**
   * @brief Passively destroys Run-owned plan and staging storage.
   *
   * @throws Nothing.
   * @note Destruction publishes no outcome, requests no cancellation, and
   * creates no new lease.
   */
  ~ComputeRunControl() noexcept;

  /**
   * @brief Attempts a non-commit terminal publication.
   * @param outcome Fully formed success or exact failure value.
   * @return True only when the open arbiter accepted this outcome.
   * @throws std::system_error when Run/plan synchronization fails.
   * @note CommitClaimed can be resolved only by its one-shot contender.
   */
  bool publish_terminal(ComputeRunTerminalOutcome outcome);

  /**
   * @brief Attempts cancellation and performs plan/notification cleanup.
   * @param reason Stable reason proposed by a private source or deadline check.
   * @param request_child_cancellation_won Optional request-level latch to
   * publish when this cancellation wins the child terminal arbiter.
   * @return True only when cancellation claimed the open arbiter.
   * @throws Callback or synchronization exceptions after terminal publication.
   * @note The optional latch is released while `mutex` is held, after the
   * cancellation outcome is fixed and before Terminal becomes observable.
   * Cleanup then runs outside `mutex`.
   */
  bool request_cancellation(
      ComputeRunCancellationReason reason,
      std::atomic<bool>* request_child_cancellation_won = nullptr);

  /**
   * @brief Observes the immutable deadline and current cancellation outcome.
   * @return Stable cancellation reason when Cancelled, otherwise nullopt.
   * @throws Clock callback or synchronization exceptions.
   */
  std::optional<ComputeRunCancellationReason> observe_cancellation();

  /**
   * @brief Atomically observes deadline and reserves commit terminal ownership.
   * @return True only when phase is CommitPending and the arbiter was open.
   * @throws Clock callback or synchronization exceptions.
   * @note Deadline cancellation cleanup runs before a false return.
   */
  bool try_claim_commit();

  /**
   * @brief Resolves the sole accepted commit contender.
   * @param outcome Succeeded or exact Failed terminal value.
   * @return True only while CommitClaimed.
   * @throws std::system_error when Run/plan synchronization fails.
   */
  bool resolve_commit(ComputeRunTerminalOutcome outcome);

  /**
   * @brief Installs or immediately invokes a cancellation cleanup slot.
   * @param slot Shared non-null callback slot.
   * @return True when installed for a future cancellation, false when no future
   * cancellation can win or callback was invoked immediately.
   * @throws std::bad_alloc or synchronization/callback exceptions.
   */
  bool register_cancellation_slot(
      const std::shared_ptr<ComputeRunCancellationSlot>& slot);

  /** @brief Immutable identity and request inputs captured before planning. */
  const ComputeRunDescriptor descriptor;

  /** @brief Guards phase, terminal arbiter, and storage installation. */
  mutable std::mutex mutex;

  /** @brief Latest monotonic nonterminal phase before settlement. */
  ComputeRunPhase phase = ComputeRunPhase::Created;

  /** @brief Exactly-one terminal outcome, absent before settlement. */
  std::optional<ComputeRunTerminalOutcome> terminal_outcome;

  /** @brief Ownership state shared by cancellation, failure, and commit. */
  ComputeRunArbiterState arbiter_state = ComputeRunArbiterState::Open;

  /** @brief Injected steady-clock source used only at cooperative boundaries.
   */
  ComputeRunMonotonicClock monotonic_clock;

  /**
   * @brief Weak registered cleanup slots within pre-accounted product capacity.
   * @note Registrations own slots strongly; expired positions are reused so
   * sequential service phases do not grow retained Run storage after admission.
   */
  std::vector<std::weak_ptr<ComputeRunCancellationSlot>> cancellation_slots;

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
      graph_instance_id_(submission.graph_instance_id),
      revision_(submission.revision),
      target_node_id_(submission.target_node_id),
      intent_(submission.intent),
      quality_(submission.quality),
      qos_(submission.qos),
      supersession_(std::move(submission.supersession)) {
}  // NOLINT(whitespace/indent_namespace)

/**
 * @brief Constructs shared Run state after validating immutable submission.
 *
 * @param submission Candidate request values transferred into the descriptor.
 * @throws std::invalid_argument for unsupported intent or invalid QoS.
 * @throws std::overflow_error when Run id allocation is exhausted.
 * @throws std::bad_alloc when descriptor ownership cannot allocate.
 */
ComputeRunControl::ComputeRunControl(
    ComputeRunSubmission submission,
    ComputeRunMonotonicClock monotonic_clock_in)
    : descriptor([&submission]() {
        validate_submission(submission);
        return ComputeRunDescriptor(ComputeRunId(mint_compute_run_id_value()),
                                    std::move(submission));
      }()),  // NOLINT(whitespace/indent_namespace)
      monotonic_clock(std::move(monotonic_clock_in)) {
  if (!monotonic_clock) {
    throw std::invalid_argument(
        "ComputeRun requires a non-empty monotonic clock.");
  }
  cancellation_slots.reserve(4U);
}

/**
 * @brief Passively releases Run-local storage after all owners disappear.
 *
 * @throws Nothing.
 */
ComputeRunControl::~ComputeRunControl() noexcept = default;

/** @copydoc ComputeRunControl::publish_terminal */
bool ComputeRunControl::publish_terminal(ComputeRunTerminalOutcome outcome) {
  TaskSubmissionPlan* plan = nullptr;
  {
    std::lock_guard<std::mutex> lock(mutex);
    if (arbiter_state != ComputeRunArbiterState::Open) {
      return false;
    }
    terminal_outcome = std::move(outcome);
    arbiter_state = ComputeRunArbiterState::Terminal;
    plan = submission_plan.get();
  }
  if (plan != nullptr) {
    plan->close_publication();
  }
  return true;
}

/** @copydoc ComputeRunControl::request_cancellation */
bool ComputeRunControl::request_cancellation(
    ComputeRunCancellationReason reason,
    std::atomic<bool>* request_child_cancellation_won) {
  TaskSubmissionPlan* plan = nullptr;
  {
    std::lock_guard<std::mutex> lock(mutex);
    if (arbiter_state != ComputeRunArbiterState::Open) {
      return false;
    }
    terminal_outcome = ComputeRunTerminalOutcome{
        ComputeRunTerminalKind::Cancelled, nullptr, reason};
    if (request_child_cancellation_won != nullptr) {
      request_child_cancellation_won->store(true, std::memory_order_release);
    }
    arbiter_state = ComputeRunArbiterState::Terminal;
    plan = submission_plan.get();
  }
  if (plan != nullptr) {
    plan->close_publication();
  }

  std::size_t slot_index = 0U;
  for (;;) {
    std::shared_ptr<ComputeRunCancellationSlot> slot;
    {
      std::lock_guard<std::mutex> lock(mutex);
      if (slot_index >= cancellation_slots.size()) {
        break;
      }
      slot = cancellation_slots[slot_index++].lock();
    }
    if (slot != nullptr) {
      slot->invoke(reason);
    }
  }
  return true;
}

/** @copydoc ComputeRunControl::observe_cancellation */
std::optional<ComputeRunCancellationReason>
ComputeRunControl::observe_cancellation() {
  const std::chrono::steady_clock::time_point now = monotonic_clock();
  bool deadline_expired = false;
  {
    std::lock_guard<std::mutex> lock(mutex);
    deadline_expired = arbiter_state == ComputeRunArbiterState::Open &&
                       descriptor.qos().deadline.has_value() &&
                       now >= *descriptor.qos().deadline;
  }
  if (deadline_expired) {
    (void)request_cancellation(ComputeRunCancellationReason::DeadlineExceeded);
  }

  std::lock_guard<std::mutex> lock(mutex);
  if (terminal_outcome.has_value() &&
      terminal_outcome->kind == ComputeRunTerminalKind::Cancelled) {
    return terminal_outcome->cancellation_reason;
  }
  return std::nullopt;
}

/** @copydoc ComputeRunControl::try_claim_commit */
bool ComputeRunControl::try_claim_commit() {
  const std::chrono::steady_clock::time_point now = monotonic_clock();
  TaskSubmissionPlan* plan = nullptr;
  bool deadline_cancelled = false;
  {
    std::lock_guard<std::mutex> lock(mutex);
    if (arbiter_state != ComputeRunArbiterState::Open) {
      return false;
    }
    if (descriptor.qos().deadline.has_value() &&
        now >= *descriptor.qos().deadline) {
      terminal_outcome = ComputeRunTerminalOutcome{
          ComputeRunTerminalKind::Cancelled, nullptr,
          ComputeRunCancellationReason::DeadlineExceeded};
      arbiter_state = ComputeRunArbiterState::Terminal;
      plan = submission_plan.get();
      deadline_cancelled = true;
    } else if (phase != ComputeRunPhase::CommitPending) {
      return false;
    } else {
      arbiter_state = ComputeRunArbiterState::CommitClaimed;
      return true;
    }
  }

  if (plan != nullptr) {
    plan->close_publication();
  }
  if (deadline_cancelled) {
    std::size_t slot_index = 0U;
    for (;;) {
      std::shared_ptr<ComputeRunCancellationSlot> slot;
      {
        std::lock_guard<std::mutex> lock(mutex);
        if (slot_index >= cancellation_slots.size()) {
          break;
        }
        slot = cancellation_slots[slot_index++].lock();
      }
      if (slot != nullptr) {
        slot->invoke(ComputeRunCancellationReason::DeadlineExceeded);
      }
    }
  }
  return false;
}

/** @copydoc ComputeRunControl::resolve_commit */
bool ComputeRunControl::resolve_commit(ComputeRunTerminalOutcome outcome) {
  TaskSubmissionPlan* plan = nullptr;
  {
    std::lock_guard<std::mutex> lock(mutex);
    if (arbiter_state != ComputeRunArbiterState::CommitClaimed) {
      return false;
    }
    terminal_outcome = std::move(outcome);
    arbiter_state = ComputeRunArbiterState::Terminal;
    plan = submission_plan.get();
  }
  if (plan != nullptr) {
    plan->close_publication();
  }
  return true;
}

/** @copydoc ComputeRunControl::register_cancellation_slot */
bool ComputeRunControl::register_cancellation_slot(
    const std::shared_ptr<ComputeRunCancellationSlot>& slot) {
  std::optional<ComputeRunCancellationReason> immediate_reason;
  {
    std::lock_guard<std::mutex> lock(mutex);
    if (arbiter_state == ComputeRunArbiterState::Open) {
      for (std::weak_ptr<ComputeRunCancellationSlot>& existing :
           cancellation_slots) {
        if (existing.expired()) {
          existing = slot;
          return true;
        }
      }
      cancellation_slots.push_back(slot);
      return true;
    }
    if (terminal_outcome.has_value() &&
        terminal_outcome->kind == ComputeRunTerminalKind::Cancelled) {
      immediate_reason = terminal_outcome->cancellation_reason;
    }
  }
  if (immediate_reason.has_value()) {
    slot->invoke(*immediate_reason);
  }
  return false;
}

/**
 * @brief Shared child-source list and distinct request/child-win state.
 * @throws std::bad_alloc when source storage grows.
 */
class ComputeRequestCancellationControl final {
 public:
  /** @brief Serializes attachment and first request acceptance. */
  mutable std::mutex mutex;

  /** @brief Stable first request reason, independent of child arbitration.
   */
  std::optional<ComputeRunCancellationReason> accepted_reason;

  /**
   * @brief True after the accepted request reason wins any child arbiter.
   * @note The winning child publishes this latch with release ordering while
   * holding its terminal mutex, before that Cancelled outcome is observable.
   */
  std::atomic<bool> child_cancellation_won{false};

  /** @brief Child Run ids already attached to this request. */
  std::vector<std::uint64_t> child_ids;

  /** @brief Independent weak-lifetime child cancellation authorities. */
  std::vector<ComputeRunCancellationSource> child_sources;
};

/** @copydoc ComputeRunCancellationSource::request_cancellation */
bool ComputeRunCancellationSource::request_cancellation(
    ComputeRunCancellationReason reason) const {
  const std::shared_ptr<ComputeRunControl> control = control_.lock();
  return control != nullptr && control->request_cancellation(reason);
}

/** @copydoc ComputeRequestCancellationSource::ComputeRequestCancellationSource
 */
ComputeRequestCancellationSource::ComputeRequestCancellationSource()
    : control_(std::make_shared<ComputeRequestCancellationControl>()) {}

/** @copydoc ComputeRequestCancellationSource::attach */
void ComputeRequestCancellationSource::attach(ComputeRun& run) {
  const ComputeRunCancellationSource child = run.cancellation_source();
  const std::uint64_t child_id = run.descriptor().id().value();
  std::optional<ComputeRunCancellationReason> immediate_reason;
  {
    std::lock_guard<std::mutex> lock(control_->mutex);
    for (std::uint64_t existing_id : control_->child_ids) {
      if (existing_id == child_id) {
        return;
      }
    }
    control_->child_ids.push_back(child_id);
    try {
      control_->child_sources.push_back(child);
    } catch (...) {
      control_->child_ids.pop_back();
      throw;
    }
    immediate_reason = control_->accepted_reason;
  }
  if (immediate_reason.has_value()) {
    const std::shared_ptr<ComputeRunControl> child_control =
        child.control_.lock();
    if (child_control != nullptr) {
      (void)child_control->request_cancellation(
          *immediate_reason, &control_->child_cancellation_won);
    }
  }
}

/** @copydoc ComputeRequestCancellationSource::request_cancellation */
bool ComputeRequestCancellationSource::request_cancellation(
    ComputeRunCancellationReason reason) {
  std::vector<ComputeRunCancellationSource> children;
  {
    std::lock_guard<std::mutex> lock(control_->mutex);
    if (control_->accepted_reason.has_value()) {
      return false;
    }
    children = control_->child_sources;
    control_->accepted_reason = reason;
  }
  for (const ComputeRunCancellationSource& child : children) {
    const std::shared_ptr<ComputeRunControl> child_control =
        child.control_.lock();
    if (child_control != nullptr) {
      (void)child_control->request_cancellation(
          reason, &control_->child_cancellation_won);
    }
  }
  return true;
}

/** @copydoc ComputeRequestCancellationSource::accepted_reason */
std::optional<ComputeRunCancellationReason>
ComputeRequestCancellationSource::accepted_reason() const {
  std::lock_guard<std::mutex> lock(control_->mutex);
  return control_->accepted_reason;
}

/** @copydoc
 * ComputeRequestCancellationSource::accepted_child_cancellation_reason */
std::optional<ComputeRunCancellationReason>
ComputeRequestCancellationSource::accepted_child_cancellation_reason() const {
  if (!control_->child_cancellation_won.load(std::memory_order_acquire)) {
    return std::nullopt;
  }
  std::lock_guard<std::mutex> lock(control_->mutex);
  return control_->accepted_reason;
}

/** @copydoc
 * ComputeRunCancellationRegistration::ComputeRunCancellationRegistration */
ComputeRunCancellationRegistration::ComputeRunCancellationRegistration(
    ComputeRunCancellationRegistration&& other) noexcept
    : slot_(std::move(other.slot_)) {}

/** @copydoc ComputeRunCancellationRegistration::operator= */
ComputeRunCancellationRegistration&
ComputeRunCancellationRegistration::operator=(
    ComputeRunCancellationRegistration&& other) noexcept {
  if (this == &other) {
    return *this;
  }
  reset();
  slot_ = std::move(other.slot_);
  return *this;
}

/** @copydoc
 * ComputeRunCancellationRegistration::~ComputeRunCancellationRegistration */
ComputeRunCancellationRegistration::
    ~ComputeRunCancellationRegistration() noexcept {
  reset();
}

/** @copydoc ComputeRunCancellationRegistration::reset */
void ComputeRunCancellationRegistration::reset() noexcept {
  if (slot_ == nullptr) {
    return;
  }
  try {
    slot_->deactivate();
  } catch (...) {
    std::terminate();
  }
  slot_.reset();
}

/** @copydoc ComputeRunCommitContender::ComputeRunCommitContender */
ComputeRunCommitContender::ComputeRunCommitContender(
    ComputeRunCommitContender&& other) noexcept
    : control_(std::move(other.control_)) {}

/** @copydoc ComputeRunCommitContender::operator= */
ComputeRunCommitContender& ComputeRunCommitContender::operator=(
    ComputeRunCommitContender&& other) noexcept {
  if (this == &other) {
    return *this;
  }
  abandon();
  control_ = std::move(other.control_);
  return *this;
}

/** @copydoc ComputeRunCommitContender::~ComputeRunCommitContender */
ComputeRunCommitContender::~ComputeRunCommitContender() noexcept {
  abandon();
}

/** @copydoc ComputeRunCommitContender::publish_succeeded */
bool ComputeRunCommitContender::publish_succeeded() {
  if (control_ == nullptr) {
    return false;
  }
  const bool published = control_->resolve_commit(ComputeRunTerminalOutcome{
      ComputeRunTerminalKind::Succeeded, nullptr, std::nullopt});
  if (published) {
    control_.reset();
  }
  return published;
}

/** @copydoc ComputeRunCommitContender::publish_failed */
bool ComputeRunCommitContender::publish_failed(std::exception_ptr failure) {
  if (!failure) {
    throw std::invalid_argument(
        "ComputeRun commit failure publication requires an exception.");
  }
  if (control_ == nullptr) {
    return false;
  }
  const bool published = control_->resolve_commit(ComputeRunTerminalOutcome{
      ComputeRunTerminalKind::Failed, std::move(failure), std::nullopt});
  if (published) {
    control_.reset();
  }
  return published;
}

/** @copydoc ComputeRunCommitContender::abandon */
void ComputeRunCommitContender::abandon() noexcept {
  if (control_ == nullptr) {
    return;
  }
  try {
    const std::exception_ptr failure = std::make_exception_ptr(std::logic_error(
        "ComputeRun commit contender was abandoned without resolution."));
    if (!control_->resolve_commit(ComputeRunTerminalOutcome{
            ComputeRunTerminalKind::Failed, failure, std::nullopt})) {
      std::terminate();
    }
    control_.reset();
  } catch (...) {
    std::terminate();
  }
}

/**
 * @brief Constructs one request observer and shared domain Run control block.
 *
 * @param submission Descriptor inputs captured before single-domain planning
 * and preflight.
 * @param monotonic_clock Non-empty injected steady-clock source.
 * @throws std::invalid_argument for unsupported intent, intent/quality
 * mismatch, or invalid QoS.
 * @throws std::overflow_error if Run identity allocation is exhausted.
 * @throws std::bad_alloc if descriptor ownership cannot allocate.
 * @note The fresh id is minted only after semantic validation succeeds; no
 * active lease exists yet.
 */
ComputeRun::ComputeRun(ComputeRunSubmission submission,
                       ComputeRunMonotonicClock monotonic_clock)
    : control_(std::make_shared<ComputeRunControl>(
          std::move(submission), std::move(monotonic_clock))) {
}  // NOLINT(whitespace/indent_namespace)

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
ComputeRunLease ComputeRun::acquire_lease() const {
  std::lock_guard<std::mutex> lock(control_->mutex);
  if (control_->terminal_outcome.has_value()) {
    throw std::logic_error("Cannot acquire a lease for a terminal ComputeRun.");
  }
  return ComputeRunLease(control_);
}

/** @copydoc ComputeRun::cancellation_source */
ComputeRunCancellationSource ComputeRun::cancellation_source() const noexcept {
  return ComputeRunCancellationSource(control_);
}

/** @copydoc ComputeRun::observe_cancellation */
std::optional<ComputeRunCancellationReason> ComputeRun::observe_cancellation()
    const {
  return control_->observe_cancellation();
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
  return control_->arbiter_state == ComputeRunArbiterState::Terminal
             ? ComputeRunPhase::Terminal
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
  if (control_->arbiter_state != ComputeRunArbiterState::Open) {
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
  return control_->arbiter_state == ComputeRunArbiterState::Terminal;
}

/**
 * @brief Constructs the Run-owned full HP task submission plan.
 *
 * @param graph Graph used for plan construction and operation resolution.
 * @param traversal Traversal service used by the dispatch-plan builder.
 * @param node_id Requested target node.
 * @param available_devices Active-route devices transferred into plan
 * ownership.
 * @return Mutable Run-owned submission plan.
 * @throws std::logic_error for duplicate storage or terminal Run.
 * @throws GraphError or standard exceptions from TaskSubmissionPlan.
 * @note Shared control retains the plan through every accepted full-HP lease.
 */
TaskSubmissionPlan& ComputeRun::emplace_submission_plan(
    GraphModel& graph, GraphTraversalService& traversal, int node_id,
    std::vector<Device> available_devices) {
  std::lock_guard<std::mutex> lock(control_->mutex);
  if (control_->arbiter_state != ComputeRunArbiterState::Open) {
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
  if (control_->arbiter_state != ComputeRunArbiterState::Open) {
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
  return control_->publish_terminal(std::move(outcome));
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

/** @copydoc ComputeRunLease::retained_memory_bytes */
std::uint64_t ComputeRunLease::retained_memory_bytes() const {
  std::lock_guard<std::mutex> lock(control_->mutex);
  RetainedMemoryEstimator estimate("ComputeRunControl");
  estimate.add_objects<ComputeRunControl>();
  estimate.add_shared_control_block();
  estimate.add_bytes(static_cast<std::uint64_t>(
      control_->descriptor.graph_identity().capacity()));
  estimate.add_bytes(1U);
  estimate.add_objects<std::weak_ptr<ComputeRunCancellationSlot>>(
      static_cast<std::uint64_t>(control_->cancellation_slots.capacity()));
  if (control_->submission_plan) {
    estimate.add_bytes(control_->submission_plan->retained_memory_bytes());
  }
  if (control_->dirty_hp_write_buffer) {
    estimate.add_bytes(
        control_->dirty_hp_write_buffer->retained_memory_bytes());
  }
  return estimate.bytes();
}

/**
 * @brief Estimates one heap-owned cancellation notification registration.
 *
 * @param callback_capture_bytes Structural callback-capture bytes supplied by
 * the registration owner.
 * @return Checked slot, shared-control, and conservative capture bytes.
 * @throws GraphError when checked structural arithmetic overflows.
 * @note The complete slot type is private to this translation unit, keeping the
 * estimator accurate without exposing cancellation internals in the header.
 */
std::uint64_t ComputeRunLease::cancellation_notification_retained_memory_bytes(
    std::uint64_t callback_capture_bytes) {
  RetainedMemoryEstimator estimate("ComputeRun cancellation notification");
  estimate.add_objects<ComputeRunCancellationSlot>();
  estimate.add_shared_control_block();
  estimate.add_bytes(callback_capture_bytes);
  return estimate.bytes();
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
  {
    std::lock_guard<std::mutex> lock(control_->mutex);
    if (control_->submission_plan == nullptr ||
        !control_->submission_plan->contains_task_identity(identity)) {
      return false;
    }
  }
  return control_->publish_terminal(ComputeRunTerminalOutcome{
      ComputeRunTerminalKind::Failed, std::move(failure), std::nullopt});
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

/** @copydoc ComputeRunLease::observe_cancellation */
std::optional<ComputeRunCancellationReason>
ComputeRunLease::observe_cancellation() const {
  return control_->observe_cancellation();
}

/** @copydoc ComputeRunLease::register_cancellation_notification */
ComputeRunCancellationRegistration
ComputeRunLease::register_cancellation_notification(
    std::function<void(ComputeRunCancellationReason)> callback) const {
  if (!callback) {
    throw std::invalid_argument(
        "ComputeRun cancellation notification requires a callback.");
  }
  auto slot = std::make_shared<ComputeRunCancellationSlot>(std::move(callback));
  if (!control_->register_cancellation_slot(slot)) {
    slot->deactivate();
    return ComputeRunCancellationRegistration();
  }
  return ComputeRunCancellationRegistration(std::move(slot));
}

/** @copydoc ComputeRunLease::try_claim_commit */
std::optional<ComputeRunCommitContender> ComputeRunLease::try_claim_commit()
    const {
  if (!control_->try_claim_commit()) {
    return std::nullopt;
  }
  return ComputeRunCommitContender(control_);
}

/**
 * @brief Routes one accepted callback into its matching Run-owned plan.
 *
 * @param identity Composite task identity carried by the callback.
 * @param task_runtime Active execution runtime.
 * @param callback_owns_completion Whether a runtime exact-once token owns the
 * pre-counted callback unit instead of this lease route.
 * @return Nothing.
 * @throws std::invalid_argument for mismatched or unregistered identity.
 * @throws std::system_error if the control mutex cannot be locked.
 * @throws Exceptions propagated by Run-owned plan execution, execution
 * completion accounting, dependent-callback submission, or matching failure
 * publication.
 * @note A matching accepted callback observed after terminal publication
 * releases its own completion unit without entering the plan. Active valid
 * failures are passed to this Run's failure publisher before unchanged
 * rethrow; an exception from that publication propagates instead.
 */
void ComputeRunLease::execute_task(const ComputeRunTaskIdentity& identity,
                                   ExecutionTaskRuntime& task_runtime,
                                   bool callback_owns_completion) {
  (void)observe_cancellation();
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
    if (!callback_owns_completion) {
      task_runtime.dec_tasks_to_complete();
    }
    return;
  }

  try {
    plan->execute_task(identity, *this, task_runtime);
    if (!callback_owns_completion) {
      task_runtime.dec_tasks_to_complete();
    }
  } catch (...) {
    const std::exception_ptr failure = std::current_exception();
    (void)publish_task_failure(identity, failure);
    std::rethrow_exception(failure);
  }
}

/**
 * @brief Executes initial-ready discovery and submission under this lease.
 *
 * @param task_runtime Active execution batch initialized by the dispatcher.
 * @return Nothing.
 * @throws GraphError or standard exceptions from plan bootstrap and runtime.
 * @note An accepted bootstrap observed after terminal publication releases its
 * own completion unit without submitting planned work. Active bootstrap
 * failures publish directly to this Run because bootstrap has no planned local
 * task identity.
 */
void ComputeRunLease::execute_bootstrap(ExecutionTaskRuntime& task_runtime) {
  (void)observe_cancellation();
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
    try {
      task_runtime.dec_tasks_to_complete();
    } catch (...) {
    }
    (void)control_->publish_terminal(ComputeRunTerminalOutcome{
        ComputeRunTerminalKind::Failed, failure, std::nullopt});
    std::rethrow_exception(failure);
  }
}

}  // namespace ps::compute
