/**
 * @file run_lifecycle_registry.cpp
 * @brief Implements process-owned Graph/Run lifecycle admission and settlement.
 */
#include "compute/run_lifecycle_registry.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <exception>
#include <limits>
#include <list>
#include <memory>
#include <optional>
#include <stdexcept>
#include <utility>
#include <vector>

#include "compute/dirty_sibling_commit_gate.hpp"
#include "photospider/core/graph_error.hpp"

namespace ps::compute {

/**
 * @brief Shared indexed control for one pre-publication candidate.
 *
 * @throws Nothing after successful shared/list allocation.
 * @note Cancellation is an allocation-free atomic observation selected by
 * Graph close or process shutdown. The registry backlink remains valid because
 * service shutdown waits for every candidate resolution.
 */
struct RunLifecycleAdmissionCandidateControl final {
  /** @brief Stable owning registry. */
  RunLifecycleRegistry* registry = nullptr;
  /** @brief Nonzero process-nonreused candidate identity. */
  std::uint64_t candidate_id = 0U;
  /** @brief Exact Graph identity. */
  GraphInstanceId graph_instance_id{1U};
  /** @brief Strong Graph anchor lease retained through resolution. */
  GraphLifetimeLease graph_lease;
  /** @brief Zero, GraphClose, or ProcessShutdown encoded as enum+1. */
  std::atomic<std::uint16_t> cancellation{0U};
  /** @brief True after commit or rollback consumes the indexed candidate. */
  std::atomic<bool> resolved{false};
};

/**
 * @brief Persistent synchronized authority for one installed bundle.
 *
 * @throws std::system_error when finalization serialization fails.
 * @note The control is allocated before admission linearization. Active,
 * Finalizing, and Finalized form a retryable state machine shared by every
 * concurrent call on the same move-only handle.
 */
struct RunLifecycleAdmissionHandleControl final {
  /**
   * @brief Monotonic state except a failed owner returns Finalizing to Active.
   * @throws Nothing for value operations.
   */
  enum class State : std::uint8_t {
    /** @brief Installed bundle still requires a finalization owner. */
    Active,
    /** @brief Exactly one caller currently performs registry settlement. */
    Finalizing,
    /** @brief Registry retirement completed and repeat calls are no-ops. */
    Finalized,
  };

  /**
   * @brief Binds one preallocated authority to its registry and bundle.
   * @param registry_in Stable registry that outlives installed handles.
   * @param bundle_id_in Fresh nonzero installed bundle identity.
   * @throws Nothing.
   */
  RunLifecycleAdmissionHandleControl(RunLifecycleRegistry* registry_in,
                                     std::uint64_t bundle_id_in) noexcept
      : registry(registry_in), bundle_id(bundle_id_in) {}

  /** @brief Stable registry that must perform settlement. */
  RunLifecycleRegistry* const registry;
  /** @brief Stable nonzero installed bundle identity. */
  const std::uint64_t bundle_id;
  /** @brief Lock-free destructor-visible obligation state. */
  std::atomic<State> state{State::Active};
  /** @brief Serializes owner selection and failed-owner retry. */
  std::mutex mutex;
  /** @brief Wakes concurrent callers after owner success or failure. */
  std::condition_variable changed;
};

namespace {

/**
 * @brief Mints one nonzero non-reused identity from a process counter.
 * @param counter Last-issued counter.
 * @param diagnostic Stable exhaustion message.
 * @return Fresh scalar.
 * @throws std::overflow_error before wrap or reuse.
 */
std::uint64_t mint_registry_identity(std::atomic<std::uint64_t>& counter,
                                     const char* diagnostic) {
  std::uint64_t observed = counter.load(std::memory_order_relaxed);
  for (;;) {
    if (observed == std::numeric_limits<std::uint64_t>::max()) {
      throw std::overflow_error(diagnostic);
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
 * @brief Allocates one idle Graph close coordinator.
 * @return Shared coordinator retained by a Graph lifetime anchor and claims.
 * @throws std::bad_alloc when coordinator allocation fails.
 */
std::shared_ptr<GraphCloseCoordinator> make_graph_close_coordinator() {
  return std::make_shared<GraphCloseCoordinator>();
}

/**
 * @brief Returns the process-lifetime candidate identity counter.
 * @return Stable monotonic counter.
 * @throws Nothing.
 */
std::atomic<std::uint64_t>& candidate_identity_counter() noexcept {
  static std::atomic<std::uint64_t> counter{0U};
  return counter;
}

/**
 * @brief Returns the process-lifetime installed-bundle identity counter.
 * @return Stable monotonic counter.
 * @throws Nothing.
 */
std::atomic<std::uint64_t>& bundle_identity_counter() noexcept {
  static std::atomic<std::uint64_t> counter{0U};
  return counter;
}

/**
 * @brief Returns the process-lifetime shutdown generation counter.
 * @return Stable monotonic counter.
 * @throws Nothing.
 */
std::atomic<std::uint64_t>& shutdown_generation_counter() noexcept {
  static std::atomic<std::uint64_t> counter{0U};
  return counter;
}

/**
 * @brief Encodes supported candidate cancellation into a nonzero atomic value.
 * @param reason GraphClose or ProcessShutdown.
 * @return Stable compact representation.
 * @throws std::invalid_argument for any other reason.
 */
std::uint16_t encode_candidate_cancellation(
    ComputeRunCancellationReason reason) {
  switch (reason) {
    case ComputeRunCancellationReason::GraphClose:
      return 1U;
    case ComputeRunCancellationReason::ProcessShutdown:
      return 2U;
    case ComputeRunCancellationReason::ExplicitRequest:
    case ComputeRunCancellationReason::Superseded:
    case ComputeRunCancellationReason::DeadlineExceeded:
      break;
  }
  throw std::invalid_argument(
      "Lifecycle candidate cancellation requires GraphClose or "
      "ProcessShutdown.");
}

/**
 * @brief Decodes one candidate cancellation observation.
 * @param encoded Zero or supported compact representation.
 * @return Matching optional reason.
 * @throws Nothing; invalid internal storage terminates.
 */
std::optional<ComputeRunCancellationReason> decode_candidate_cancellation(
    std::uint16_t encoded) noexcept {
  switch (encoded) {
    case 0U:
      return std::nullopt;
    case 1U:
      return ComputeRunCancellationReason::GraphClose;
    case 2U:
      return ComputeRunCancellationReason::ProcessShutdown;
    default:
      std::terminate();
  }
}

/**
 * @brief Maps one trusted cancellation reason to telemetry category.
 * @param reason Exact Run cancellation reason.
 * @return Frozen version-1 category.
 * @throws Nothing.
 */
ExecutionLifecycleCategory cancellation_category(
    ComputeRunCancellationReason reason) noexcept {
  switch (reason) {
    case ComputeRunCancellationReason::ExplicitRequest:
      return ExecutionLifecycleCategory::ExplicitRequest;
    case ComputeRunCancellationReason::DeadlineExceeded:
      return ExecutionLifecycleCategory::Deadline;
    case ComputeRunCancellationReason::Superseded:
      return ExecutionLifecycleCategory::Superseded;
    case ComputeRunCancellationReason::GraphClose:
      return ExecutionLifecycleCategory::GraphClose;
    case ComputeRunCancellationReason::ProcessShutdown:
      return ExecutionLifecycleCategory::ProcessShutdown;
  }
  std::terminate();
}

/**
 * @brief Maps one exact terminal outcome to telemetry category.
 * @param outcome Published child Run outcome.
 * @return Frozen version-1 category.
 * @throws Nothing.
 */
ExecutionLifecycleCategory terminal_category(
    const ComputeRunTerminalOutcome& outcome) noexcept {
  switch (outcome.kind) {
    case ComputeRunTerminalKind::Succeeded:
      return ExecutionLifecycleCategory::Succeeded;
    case ComputeRunTerminalKind::Cancelled:
      return ExecutionLifecycleCategory::Cancelled;
    case ComputeRunTerminalKind::Failed:
      break;
  }
  if (outcome.failure) {
    try {
      std::rethrow_exception(outcome.failure);
    } catch (const std::bad_alloc&) {
      return ExecutionLifecycleCategory::FailureResourceExhausted;
    } catch (...) {
    }
  }
  return ExecutionLifecycleCategory::FailureOther;
}

/**
 * @brief Tests whether every version-1 counter is zero.
 * @param counters Candidate final view.
 * @return True only when every field is exactly zero.
 * @throws Nothing.
 */
bool all_counters_zero(const ExecutionLifecycleCounters& counters) noexcept {
  return counters.registered_graph_count == 0U &&
         counters.open_graph_count == 0U &&
         counters.closing_graph_count == 0U &&
         counters.pending_candidate_count == 0U &&
         counters.admitted_standalone_run_count == 0U &&
         counters.admitted_run_group_count == 0U &&
         counters.admitted_child_run_count == 0U &&
         counters.terminal_not_quiescent_run_count == 0U &&
         counters.finalizing_run_count == 0U &&
         counters.ready_entry_count == 0U &&
         counters.entered_callback_count == 0U &&
         counters.live_root_reservation_count == 0U &&
         counters.live_child_grant_count == 0U &&
         counters.live_policy_invocation_count == 0U &&
         counters.live_policy_binding_count == 0U;
}

/**
 * @brief Completes trusted telemetry after an irreversible state transition.
 * @tparam Publish Allocation-free telemetry operation.
 * @param publish Callable that records the already committed transition.
 * @return Nothing.
 * @throws Nothing; telemetry synchronization failure terminates because the
 * transition cannot be rolled back or safely reported as recoverable.
 * @note Callers may hold the lifecycle fence. The callable must not wait or
 * enter plugin, policy, route, ledger, or Run code.
 */
template <typename Publish>
void publish_committed_transition(Publish&& publish) noexcept {
  try {
    std::forward<Publish>(publish)();
  } catch (...) {
    std::terminate();
  }
}

}  // namespace

/** @copydoc GraphCloseCoordinator::begin */
GraphCloseCoordinator::Claim GraphCloseCoordinator::begin() {
  std::unique_lock<std::mutex> lock(mutex_);
  cv_.wait(lock, [this]() { return state_ != State::Failed; });
  if (state_ == State::Idle) {
    if (generation_ == std::numeric_limits<std::uint64_t>::max()) {
      throw std::overflow_error("Graph close generation exhausted.");
    }
    ++generation_;
    state_ = State::InProgress;
    return Claim{Role::Owner, generation_};
  }
  if (state_ == State::InProgress) {
    if (pending_joiners_ == std::numeric_limits<std::uint64_t>::max()) {
      throw std::overflow_error("Graph close joiner count exhausted.");
    }
    ++pending_joiners_;
  }
  return Claim{Role::Joiner, generation_};
}

/** @copydoc GraphCloseCoordinator::complete_success */
void GraphCloseCoordinator::complete_success(const Claim& owner) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (owner.role != Role::Owner || owner.generation != generation_ ||
        state_ != State::InProgress) {
      throw std::logic_error("Graph close completion requires one live owner.");
    }
    state_ = State::Succeeded;
  }
  cv_.notify_all();
}

/** @copydoc GraphCloseCoordinator::complete_failure */
void GraphCloseCoordinator::complete_failure(
    const Claim& owner, std::exception_ptr failure) noexcept {
  try {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (owner.role != Role::Owner || owner.generation != generation_ ||
          state_ != State::InProgress || !failure) {
        std::terminate();
      }
      failure_ = std::move(failure);
      state_ = pending_joiners_ == 0U ? State::Idle : State::Failed;
      if (state_ == State::Idle) {
        failure_ = nullptr;
      }
    }
    cv_.notify_all();
  } catch (...) {
    std::terminate();
  }
}

/** @copydoc GraphCloseCoordinator::wait_for_completion */
void GraphCloseCoordinator::wait_for_completion(const Claim& joiner) {
  if (joiner.role != Role::Joiner || joiner.generation == 0U) {
    throw std::logic_error("Graph close wait requires one Joiner claim.");
  }
  std::exception_ptr failure;
  bool retry_ready = false;
  try {
    std::unique_lock<std::mutex> lock(mutex_);
    if (joiner.generation != generation_) {
      std::terminate();
    }
    cv_.wait(lock, [this]() { return state_ != State::InProgress; });
    if (state_ == State::Succeeded) {
      return;
    }
    if (state_ != State::Failed || pending_joiners_ == 0U || !failure_) {
      std::terminate();
    }
    failure = failure_;
    --pending_joiners_;
    if (pending_joiners_ == 0U) {
      failure_ = nullptr;
      state_ = State::Idle;
      retry_ready = true;
    }
  } catch (...) {
    std::terminate();
  }
  if (retry_ready) {
    cv_.notify_all();
  }
  std::rethrow_exception(failure);
}

/** @copydoc GraphCloseCoordinator::started */
bool GraphCloseCoordinator::started() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return generation_ != 0U;
}

/** @copydoc GraphCloseCoordinator::completed */
bool GraphCloseCoordinator::completed() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return state_ == State::Succeeded;
}

/** @copydoc GraphLifetimeAnchor::GraphLifetimeAnchor */
GraphLifetimeAnchor::GraphLifetimeAnchor(GraphInstanceId graph_instance_id)
    : graph_instance_id_(graph_instance_id),
      close_coordinator_(make_graph_close_coordinator()) {}  // NOLINT

/** @copydoc GraphLifetimeLease::graph_instance_id */
GraphInstanceId GraphLifetimeLease::graph_instance_id() const {
  if (!anchor_) {
    throw std::logic_error("Graph lifetime lease is inactive.");
  }
  return anchor_->graph_instance_id();
}

/**
 * @brief Complete private lifecycle state behind RunLifecycleRegistry.
 *
 * @throws std::system_error when the lifecycle fence or condition fails.
 * @note Every list insertion is staged off-fence and spliced without
 * allocation. Linear scans intentionally avoid a second allocating index and
 * keep complete realtime bundle visibility under one fence mutation.
 */
class RunLifecycleRegistry::Impl final {
 public:
  /**
   * @brief Monotonic registry service state.
   * @throws Nothing for value construction and comparison.
   */
  enum class ServiceState {
    /** @brief Graph/candidate/bundle admission remains open. */
    Accepting,
    /** @brief Admission closed while ownership settles. */
    Stopping,
    /** @brief Registry and physical execution retired. */
    Stopped,
  };

  /**
   * @brief Monotonic per-Graph admission state.
   * @throws Nothing for value construction and comparison.
   */
  enum class GraphState {
    /** @brief Candidate and bundle installation may linearize. */
    Open,
    /** @brief No new bundle may install; existing ownership settles. */
    Closing,
  };

  /**
   * @brief One indexed child Run with the registry's minimum lease.
   * @throws Nothing after lease staging succeeds.
   */
  struct RunRecord final {
    /**
     * @brief Stages one child identity and minimum registry lease.
     * @param id Exact nonzero child Run identity.
     * @param run_lease Sole lease transferred into registry ownership.
     * @throws Nothing after argument evaluation.
     */
    RunRecord(ComputeRunId id, ComputeRunLease run_lease) noexcept
        : run_id(id), lease(std::move(run_lease)) {}

    /** @brief Exact child Run identity. */
    ComputeRunId run_id;
    /** @brief Sole registry-owned lifetime lease. */
    std::optional<ComputeRunLease> lease;
    /** @brief True after terminal/finalization observation begins. */
    bool finalizing = false;
    /** @brief True after all non-registry leases retire. */
    bool quiescent = false;
    /** @brief True after exact ledger root/grant settlement observation. */
    bool resource_settled = false;
  };

  /**
   * @brief One atomically installed standalone or realtime bundle.
   * @throws std::bad_alloc only while staging child storage off-fence.
   */
  struct AdmissionRecord final {
    /** @brief Nonzero registry-private bundle identity. */
    std::uint64_t bundle_id = 0U;
    /** @brief Exact owning Graph identity. */
    GraphInstanceId graph_instance_id{1U};
    /** @brief Optional nonzero realtime group identity. */
    std::uint64_t run_group_id = 0U;
    /** @brief One standalone child or exact HP/RT pair. */
    std::vector<RunRecord> runs;
  };

  /**
   * @brief Preallocated cancellation fan-out node for one installed bundle.
   *
   * @throws Nothing after off-fence list-node and shared-owner staging.
   * @note Graph close or process shutdown splices these nodes out under the
   * lifecycle fence, then invokes their owners without the fence. Finalization
   * erases an undispatched node by bundle identity.
   */
  struct CancellationDispatchRecord final {
    /** @brief Matching installed bundle identity. */
    std::uint64_t bundle_id = 0U;
    /** @brief Exact Graph correlation used by close telemetry. */
    GraphInstanceId graph_instance_id{1U};
    /** @brief Request-level cancellation fan-out owner. */
    std::shared_ptr<ComputeRequestCancellationSource> cancellation;
    /** @brief Optional realtime gate denied before fan-out. */
    std::shared_ptr<DirtySiblingCommitGate> sibling_commit_gate;
  };

  /**
   * @brief One complete live Graph lifecycle row.
   * @throws Nothing after off-fence staging succeeds.
   */
  struct GraphRow final {
    /** @brief Exact non-reused Graph identity. */
    GraphInstanceId graph_instance_id{1U};
    /** @brief Runtime lifetime root retained through row removal. */
    std::shared_ptr<GraphLifetimeAnchor> anchor;
    /** @brief Open or monotonic Closing state. */
    GraphState state = GraphState::Open;
    /** @brief One nonzero Graph close generation after Closing. */
    std::uint64_t close_generation = 0U;
    /** @brief Indexed unresolved candidate controls. */
    std::list<std::shared_ptr<RunLifecycleAdmissionCandidateControl>>
        candidates;
    /** @brief Installed-bundle cancellation nodes not yet captured by close. */
    std::list<CancellationDispatchRecord> cancellation_records;
  };

  /**
   * @brief Binds one registry to its stable telemetry owner.
   * @param telemetry_owner Service-owned fixed ring.
   * @throws Nothing.
   */
  explicit Impl(ExecutionLifecycleTelemetry& telemetry_owner) noexcept
      : telemetry(telemetry_owner) {}

  /**
   * @brief Finds an exact Graph row while the fence is held.
   * @param graph_instance_id Exact identity.
   * @return Iterator or rows.end().
   * @throws Nothing.
   */
  std::list<GraphRow>::iterator find_graph(
      GraphInstanceId graph_instance_id) noexcept {
    return std::find_if(rows.begin(), rows.end(),
                        [graph_instance_id](const GraphRow& row) {
                          return row.graph_instance_id == graph_instance_id;
                        });
  }

  /**
   * @brief Finds an exact Graph row while a const fence is held.
   * @param graph_instance_id Exact identity.
   * @return Iterator or rows.end().
   * @throws Nothing.
   */
  std::list<GraphRow>::const_iterator find_graph(
      GraphInstanceId graph_instance_id) const noexcept {
    return std::find_if(rows.cbegin(), rows.cend(),
                        [graph_instance_id](const GraphRow& row) {
                          return row.graph_instance_id == graph_instance_id;
                        });
  }

  /**
   * @brief Finds one installed admission while the fence is held.
   * @param bundle_id Exact bundle identity.
   * @return Iterator or admissions.end().
   * @throws Nothing.
   */
  std::list<AdmissionRecord>::iterator find_admission(
      std::uint64_t bundle_id) noexcept {
    return std::find_if(admissions.begin(), admissions.end(),
                        [bundle_id](const AdmissionRecord& admission) {
                          return admission.bundle_id == bundle_id;
                        });
  }

  /**
   * @brief Reports whether one Graph still has an installed bundle.
   * @param graph_instance_id Exact identity.
   * @return True when any admission belongs to the Graph.
   * @throws Nothing.
   */
  bool graph_has_admission(GraphInstanceId graph_instance_id) const noexcept {
    return std::any_of(admissions.cbegin(), admissions.cend(),
                       [graph_instance_id](const AdmissionRecord& admission) {
                         return admission.graph_instance_id ==
                                graph_instance_id;
                       });
  }

  /**
   * @brief Calculates registry-derived post-transition counters.
   * @return Exact Graph/candidate/admission/finalization view.
   * @throws Nothing.
   * @note Caller holds fence.
   */
  ExecutionLifecycleCounters counters_locked() const noexcept {
    ExecutionLifecycleCounters result;
    result.registered_graph_count = static_cast<std::uint64_t>(rows.size());
    for (const GraphRow& row : rows) {
      if (row.state == GraphState::Open) {
        ++result.open_graph_count;
      } else {
        ++result.closing_graph_count;
      }
      result.pending_candidate_count +=
          static_cast<std::uint64_t>(row.candidates.size());
    }
    for (const AdmissionRecord& admission : admissions) {
      if (admission.run_group_id == 0U) {
        ++result.admitted_standalone_run_count;
      } else {
        ++result.admitted_run_group_count;
        result.admitted_child_run_count +=
            static_cast<std::uint64_t>(admission.runs.size());
      }
      for (const RunRecord& run : admission.runs) {
        if (run.finalizing && !run.quiescent) {
          ++result.terminal_not_quiescent_run_count;
        }
        if (run.finalizing) {
          ++result.finalizing_run_count;
        }
      }
    }
    return result;
  }

  /** @brief Non-waiting lifecycle/index fence. */
  mutable std::mutex fence;
  /** @brief Wakes close/shutdown when candidate/bundle predicates change. */
  std::condition_variable changed;
  /** @brief Stable service-owned telemetry ring. */
  ExecutionLifecycleTelemetry& telemetry;
  /** @brief Monotonic service state. */
  ServiceState service_state = ServiceState::Accepting;
  /** @brief One process shutdown generation, or zero before Stopping. */
  std::uint64_t shutdown_generation = 0U;
  /** @brief Complete live Graph rows. */
  std::list<GraphRow> rows;
  /** @brief Atomically visible installed standalone/group bundles. */
  std::list<AdmissionRecord> admissions;
};

/** @copydoc RunLifecycleAdmissionCandidate::RunLifecycleAdmissionCandidate */
RunLifecycleAdmissionCandidate::RunLifecycleAdmissionCandidate(
    RunLifecycleAdmissionCandidate&& other) noexcept
    : control_(std::move(other.control_)) {}

/** @copydoc RunLifecycleAdmissionCandidate::operator= */
RunLifecycleAdmissionCandidate& RunLifecycleAdmissionCandidate::operator=(
    RunLifecycleAdmissionCandidate&& other) noexcept {
  if (this != &other) {
    reset();
    control_ = std::move(other.control_);
  }
  return *this;
}

/** @copydoc RunLifecycleAdmissionCandidate::~RunLifecycleAdmissionCandidate */
RunLifecycleAdmissionCandidate::~RunLifecycleAdmissionCandidate() noexcept {
  reset();
}

/** @copydoc RunLifecycleAdmissionCandidate::id */
std::uint64_t RunLifecycleAdmissionCandidate::id() const {
  if (!control_) {
    throw std::logic_error("Run lifecycle candidate is inactive.");
  }
  return control_->candidate_id;
}

/** @copydoc RunLifecycleAdmissionCandidate::graph_instance_id */
GraphInstanceId RunLifecycleAdmissionCandidate::graph_instance_id() const {
  if (!control_) {
    throw std::logic_error("Run lifecycle candidate is inactive.");
  }
  return control_->graph_instance_id;
}

/** @copydoc RunLifecycleAdmissionCandidate::cancellation_reason */
std::optional<ComputeRunCancellationReason>
RunLifecycleAdmissionCandidate::cancellation_reason() const {
  if (!control_) {
    throw std::logic_error("Run lifecycle candidate is inactive.");
  }
  return decode_candidate_cancellation(
      control_->cancellation.load(std::memory_order_acquire));
}

/** @copydoc RunLifecycleAdmissionCandidate::reset */
void RunLifecycleAdmissionCandidate::reset() noexcept {
  if (!control_) {
    return;
  }
  std::shared_ptr<RunLifecycleAdmissionCandidateControl> control =
      std::move(control_);
  control->registry->rollback_candidate(control);
}

/** @copydoc RunLifecycleAdmissionHandle::RunLifecycleAdmissionHandle */
RunLifecycleAdmissionHandle::RunLifecycleAdmissionHandle(
    RunLifecycleAdmissionHandle&& other) noexcept
    : control_(std::move(other.control_)) {}  // NOLINT

/** @copydoc RunLifecycleAdmissionHandle::~RunLifecycleAdmissionHandle */
RunLifecycleAdmissionHandle::~RunLifecycleAdmissionHandle() noexcept {
  if (control_ != nullptr &&
      control_->state.load(std::memory_order_acquire) !=
          RunLifecycleAdmissionHandleControl::State::Finalized) {
    std::terminate();
  }
}

/** @copydoc RunLifecycleAdmissionHandle::active */
bool RunLifecycleAdmissionHandle::active() const noexcept {
  return control_ != nullptr &&
         control_->state.load(std::memory_order_acquire) !=
             RunLifecycleAdmissionHandleControl::State::Finalized;
}

/** @copydoc RunLifecycleAdmissionHandle::bundle_id */
std::uint64_t RunLifecycleAdmissionHandle::bundle_id() const {
  if (!active()) {
    throw std::logic_error("Run lifecycle admission handle is inactive.");
  }
  return control_->bundle_id;
}

/** @copydoc RunLifecycleRegistry::RunLifecycleRegistry */
RunLifecycleRegistry::RunLifecycleRegistry(
    ExecutionLifecycleTelemetry& telemetry)
    : impl_(std::make_unique<Impl>(telemetry)) {}

/** @copydoc RunLifecycleRegistry::~RunLifecycleRegistry */
RunLifecycleRegistry::~RunLifecycleRegistry() noexcept {
  if (!impl_) {
    return;
  }
  try {
    std::lock_guard<std::mutex> lock(impl_->fence);
    const bool empty = impl_->rows.empty() && impl_->admissions.empty();
    const bool valid_partial_construction =
        empty && impl_->service_state == Impl::ServiceState::Accepting;
    const bool valid_stopped =
        empty && impl_->service_state == Impl::ServiceState::Stopped;
    if (!valid_partial_construction && !valid_stopped) {
      std::terminate();
    }
  } catch (...) {
    std::terminate();
  }
}

/** @copydoc RunLifecycleRegistry::register_graph */
void RunLifecycleRegistry::register_graph(
    std::shared_ptr<GraphLifetimeAnchor> anchor) {
  if (!anchor || anchor->retired()) {
    throw std::invalid_argument(
        "Run lifecycle Graph registration requires a live anchor.");
  }
  std::list<Impl::GraphRow> staged;
  staged.push_back(Impl::GraphRow{anchor->graph_instance_id(),
                                  std::move(anchor),
                                  Impl::GraphState::Open,
                                  0U,
                                  {},
                                  {}});
  const GraphInstanceId graph_instance_id = staged.front().graph_instance_id;
  std::lock_guard<std::mutex> lock(impl_->fence);
  if (impl_->service_state != Impl::ServiceState::Accepting) {
    throw std::logic_error("Run lifecycle registry no longer accepts Graphs.");
  }
  if (impl_->find_graph(graph_instance_id) != impl_->rows.end()) {
    throw std::invalid_argument(
        "Run lifecycle Graph identity is already registered.");
  }
  impl_->rows.splice(impl_->rows.end(), staged);
  impl_->telemetry.publish(ExecutionLifecycleEventKind::GraphRegistered,
                           ExecutionLifecycleCategory::None,
                           graph_instance_id.value(), 0U, 0U, 0U,
                           impl_->counters_locked());
}

/** @copydoc RunLifecycleRegistry::rollback_graph_registration */
void RunLifecycleRegistry::rollback_graph_registration(
    GraphInstanceId graph_instance_id) {
  std::lock_guard<std::mutex> lock(impl_->fence);
  const auto row = impl_->find_graph(graph_instance_id);
  if (row == impl_->rows.end() || row->state != Impl::GraphState::Open ||
      !row->candidates.empty() ||
      impl_->graph_has_admission(graph_instance_id)) {
    throw std::logic_error(
        "Run lifecycle Graph registration cannot be rolled back.");
  }
  impl_->rows.erase(row);
  impl_->telemetry.publish(ExecutionLifecycleEventKind::GraphRowRemoved,
                           ExecutionLifecycleCategory::None,
                           graph_instance_id.value(), 0U, 0U, 0U,
                           impl_->counters_locked());
  impl_->changed.notify_all();
}

/** @copydoc RunLifecycleRegistry::begin_graph_admission */
RunLifecycleAdmissionCandidate RunLifecycleRegistry::begin_graph_admission(
    GraphInstanceId graph_instance_id) {
  const std::uint64_t candidate_id = mint_registry_identity(
      candidate_identity_counter(),
      "Run lifecycle candidate identity space is exhausted.");
  auto control = std::make_shared<RunLifecycleAdmissionCandidateControl>();
  std::list<std::shared_ptr<RunLifecycleAdmissionCandidateControl>> staged;
  staged.push_back(control);

  std::lock_guard<std::mutex> lock(impl_->fence);
  const auto row = impl_->find_graph(graph_instance_id);
  if (impl_->service_state != Impl::ServiceState::Accepting ||
      row == impl_->rows.end() || row->state != Impl::GraphState::Open) {
    throw GraphError(GraphErrc::NotFound,
                     "Graph is not open for Run admission.");
  }
  control->registry = this;
  control->candidate_id = candidate_id;
  control->graph_instance_id = graph_instance_id;
  control->graph_lease = GraphLifetimeLease(row->anchor);
  row->candidates.splice(row->candidates.end(), staged);
  impl_->telemetry.publish(ExecutionLifecycleEventKind::CandidateBegan,
                           ExecutionLifecycleCategory::None,
                           graph_instance_id.value(), 0U, 0U, candidate_id,
                           impl_->counters_locked());
  return RunLifecycleAdmissionCandidate(std::move(control));
}

namespace {

/**
 * @brief Validates staged Run identity against one unresolved candidate.
 * @param candidate Exact candidate control.
 * @param lease Candidate Run lease.
 * @return Nothing.
 * @throws std::invalid_argument on Graph identity mismatch.
 */
void validate_candidate_run(
    const RunLifecycleAdmissionCandidateControl& candidate,
    const ComputeRunLease& lease) {
  if (lease.descriptor().graph_instance_id() != candidate.graph_instance_id) {
    throw std::invalid_argument(
        "Run lifecycle bundle Graph identity does not match its candidate.");
  }
}

}  // namespace

/** @copydoc RunLifecycleRegistry::commit_standalone */
RunLifecycleAdmissionHandle RunLifecycleRegistry::commit_standalone(
    RunLifecycleAdmissionCandidate candidate, ComputeRunLease run_lease,
    std::shared_ptr<ComputeRequestCancellationSource> cancellation) {
  if (!candidate.control_ || candidate.control_->registry != this) {
    throw std::logic_error(
        "Run lifecycle standalone commit requires a live local candidate.");
  }
  if (!cancellation) {
    throw std::invalid_argument(
        "Run lifecycle standalone commit requires cancellation ownership.");
  }
  validate_candidate_run(*candidate.control_, run_lease);
  const std::uint64_t bundle_id = mint_registry_identity(
      bundle_identity_counter(),
      "Run lifecycle bundle identity space is exhausted.");
  auto handle_control =
      std::make_shared<RunLifecycleAdmissionHandleControl>(this, bundle_id);
  std::list<Impl::AdmissionRecord> staged;
  Impl::AdmissionRecord record;
  record.bundle_id = bundle_id;
  record.graph_instance_id = candidate.control_->graph_instance_id;
  record.runs.reserve(1U);
  record.runs.emplace_back(run_lease.descriptor().id(), std::move(run_lease));
  staged.push_back(std::move(record));
  std::list<Impl::CancellationDispatchRecord> staged_cancellation;
  staged_cancellation.push_back(Impl::CancellationDispatchRecord{
      bundle_id, candidate.control_->graph_instance_id, std::move(cancellation),
      nullptr});

  std::shared_ptr<RunLifecycleAdmissionCandidateControl> control =
      candidate.control_;
  {
    std::lock_guard<std::mutex> lock(impl_->fence);
    const auto row = impl_->find_graph(control->graph_instance_id);
    const bool may_commit =
        impl_->service_state == Impl::ServiceState::Accepting &&
        row != impl_->rows.end() && row->state == Impl::GraphState::Open &&
        !control->resolved.load(std::memory_order_acquire) &&
        std::find(row->candidates.begin(), row->candidates.end(), control) !=
            row->candidates.end();
    if (!may_commit) {
      throw GraphError(GraphErrc::NotFound, "Graph close won Run admission.");
    }
    const ComputeRunId run_id = staged.front().runs.front().run_id;
    for (const Impl::AdmissionRecord& active : impl_->admissions) {
      for (const Impl::RunRecord& run : active.runs) {
        if (run.run_id == run_id) {
          throw std::logic_error(
              "Run lifecycle Run identity is already installed.");
        }
      }
    }
    row->candidates.remove(control);
    control->resolved.store(true, std::memory_order_release);
    control->graph_lease = GraphLifetimeLease();
    impl_->admissions.splice(impl_->admissions.end(), staged);
    row->cancellation_records.splice(row->cancellation_records.end(),
                                     staged_cancellation);
    publish_committed_transition([&]() {
      impl_->telemetry.publish(
          ExecutionLifecycleEventKind::BundleAdmitted,
          ExecutionLifecycleCategory::None, control->graph_instance_id.value(),
          run_id.value(), 0U, bundle_id, impl_->counters_locked());
    });
  }
  candidate.control_.reset();
  impl_->changed.notify_all();
  return RunLifecycleAdmissionHandle(std::move(handle_control));
}

/** @copydoc RunLifecycleRegistry::commit_realtime_group */
RunLifecycleAdmissionHandle RunLifecycleRegistry::commit_realtime_group(
    RunLifecycleAdmissionCandidate candidate, RunGroupId run_group_id,
    ComputeRunLease hp_lease, ComputeRunLease rt_lease,
    std::shared_ptr<ComputeRequestCancellationSource> cancellation,
    std::shared_ptr<DirtySiblingCommitGate> sibling_commit_gate) {
  if (!candidate.control_ || candidate.control_->registry != this) {
    throw std::logic_error(
        "Run lifecycle group commit requires a live local candidate.");
  }
  if (!cancellation || !sibling_commit_gate || run_group_id.value() == 0U) {
    throw std::invalid_argument(
        "Run lifecycle group commit requires source, gate, and group id.");
  }
  validate_candidate_run(*candidate.control_, hp_lease);
  validate_candidate_run(*candidate.control_, rt_lease);
  if (hp_lease.descriptor().id() == rt_lease.descriptor().id()) {
    throw std::invalid_argument(
        "Run lifecycle realtime children require distinct Run identities.");
  }
  const std::uint64_t bundle_id = mint_registry_identity(
      bundle_identity_counter(),
      "Run lifecycle bundle identity space is exhausted.");
  auto handle_control =
      std::make_shared<RunLifecycleAdmissionHandleControl>(this, bundle_id);
  std::list<Impl::AdmissionRecord> staged;
  Impl::AdmissionRecord record;
  record.bundle_id = bundle_id;
  record.graph_instance_id = candidate.control_->graph_instance_id;
  record.run_group_id = run_group_id.value();
  record.runs.reserve(2U);
  record.runs.emplace_back(hp_lease.descriptor().id(), std::move(hp_lease));
  record.runs.emplace_back(rt_lease.descriptor().id(), std::move(rt_lease));
  staged.push_back(std::move(record));
  std::list<Impl::CancellationDispatchRecord> staged_cancellation;
  staged_cancellation.push_back(Impl::CancellationDispatchRecord{
      bundle_id, candidate.control_->graph_instance_id, std::move(cancellation),
      std::move(sibling_commit_gate)});

  std::shared_ptr<RunLifecycleAdmissionCandidateControl> control =
      candidate.control_;
  {
    std::lock_guard<std::mutex> lock(impl_->fence);
    const auto row = impl_->find_graph(control->graph_instance_id);
    const bool may_commit =
        impl_->service_state == Impl::ServiceState::Accepting &&
        row != impl_->rows.end() && row->state == Impl::GraphState::Open &&
        !control->resolved.load(std::memory_order_acquire) &&
        std::find(row->candidates.begin(), row->candidates.end(), control) !=
            row->candidates.end();
    if (!may_commit) {
      throw GraphError(GraphErrc::NotFound,
                       "Graph close won realtime bundle admission.");
    }
    for (const Impl::AdmissionRecord& active : impl_->admissions) {
      if (active.run_group_id == run_group_id.value()) {
        throw std::logic_error(
            "Run lifecycle RunGroup identity is already installed.");
      }
      for (const Impl::RunRecord& existing : active.runs) {
        for (const Impl::RunRecord& staged_run : staged.front().runs) {
          if (existing.run_id == staged_run.run_id) {
            throw std::logic_error(
                "Run lifecycle child Run identity is already installed.");
          }
        }
      }
    }
    row->candidates.remove(control);
    control->resolved.store(true, std::memory_order_release);
    control->graph_lease = GraphLifetimeLease();
    const ComputeRunId first_run = staged.front().runs.front().run_id;
    impl_->admissions.splice(impl_->admissions.end(), staged);
    row->cancellation_records.splice(row->cancellation_records.end(),
                                     staged_cancellation);
    publish_committed_transition([&]() {
      impl_->telemetry.publish(ExecutionLifecycleEventKind::BundleAdmitted,
                               ExecutionLifecycleCategory::None,
                               control->graph_instance_id.value(),
                               first_run.value(), run_group_id.value(),
                               bundle_id, impl_->counters_locked());
    });
  }
  candidate.control_.reset();
  impl_->changed.notify_all();
  return RunLifecycleAdmissionHandle(std::move(handle_control));
}

/** @copydoc RunLifecycleRegistry::finalize_admission */
void RunLifecycleRegistry::finalize_admission(
    RunLifecycleAdmissionHandle& handle) {
  const std::shared_ptr<RunLifecycleAdmissionHandleControl> control =
      handle.control_;
  if (control == nullptr || control->registry != this) {
    throw std::invalid_argument(
        "Run lifecycle finalization requires a live local handle.");
  }
  {
    std::unique_lock<std::mutex> lock(control->mutex);
    for (;;) {
      const RunLifecycleAdmissionHandleControl::State state =
          control->state.load(std::memory_order_acquire);
      if (state == RunLifecycleAdmissionHandleControl::State::Finalized) {
        return;
      }
      if (state == RunLifecycleAdmissionHandleControl::State::Active) {
        control->state.store(
            RunLifecycleAdmissionHandleControl::State::Finalizing,
            std::memory_order_release);
        break;
      }
      control->changed.wait(lock);
    }
  }

  const std::uint64_t bundle_id = control->bundle_id;
  try {
    std::array<ComputeRunSettlementObserver, 2U> settlement_observers;
    std::array<ComputeRunTerminalOutcome, 2U> outcomes;
    std::size_t run_count = 0U;
    GraphInstanceId graph_instance_id{1U};
    std::uint64_t run_group_id = 0U;
    {
      std::lock_guard<std::mutex> lock(impl_->fence);
      const auto admission = impl_->find_admission(bundle_id);
      if (admission == impl_->admissions.end()) {
        throw std::invalid_argument(
            "Run lifecycle finalization names no installed bundle.");
      }
      if (admission->runs.empty() ||
          admission->runs.size() > settlement_observers.size()) {
        throw std::logic_error(
            "Run lifecycle bundle has an invalid child count.");
      }
      graph_instance_id = admission->graph_instance_id;
      run_group_id = admission->run_group_id;
      for (Impl::RunRecord& run : admission->runs) {
        if (!run.lease.has_value()) {
          throw std::logic_error(
              "Run lifecycle installed record has no registry lease.");
        }
        settlement_observers[run_count++] = run.lease->settlement_observer();
      }
    }

    for (std::size_t index = 0U; index < run_count; ++index) {
      const std::optional<ComputeRunTerminalOutcome> outcome =
          settlement_observers[index].terminal_outcome();
      if (!outcome.has_value()) {
        throw std::logic_error(
            "Run lifecycle finalization requires terminal child Runs.");
      }
      outcomes[index] = *outcome;
    }

    {
      std::lock_guard<std::mutex> lock(impl_->fence);
      const auto admission = impl_->find_admission(bundle_id);
      if (admission == impl_->admissions.end() ||
          admission->graph_instance_id != graph_instance_id ||
          admission->run_group_id != run_group_id ||
          admission->runs.size() != run_count) {
        throw std::logic_error(
            "Run lifecycle bundle changed during terminal observation.");
      }
      std::size_t index = 0U;
      for (Impl::RunRecord& run : admission->runs) {
        if (!run.finalizing) {
          run.finalizing = true;
          publish_committed_transition([&]() {
            impl_->telemetry.publish(
                ExecutionLifecycleEventKind::RunTerminal,
                terminal_category(outcomes[index]),
                admission->graph_instance_id.value(), run.run_id.value(),
                admission->run_group_id, bundle_id, impl_->counters_locked());
          });
        }
        ++index;
      }
    }

    if (finalization_wait_observer_ != nullptr) {
      finalization_wait_observer_(finalization_wait_observer_context_,
                                  bundle_id, false);
    }
    for (std::size_t index = 0U; index < run_count; ++index) {
      settlement_observers[index].wait_until_registry_lease();
    }

    {
      std::lock_guard<std::mutex> lock(impl_->fence);
      const auto admission = impl_->find_admission(bundle_id);
      if (admission == impl_->admissions.end()) {
        throw std::logic_error(
            "Run lifecycle bundle disappeared during finalization.");
      }
      for (Impl::RunRecord& run : admission->runs) {
        if (!run.quiescent) {
          run.quiescent = true;
          publish_committed_transition([&]() {
            impl_->telemetry.publish(
                ExecutionLifecycleEventKind::RunQuiescent,
                ExecutionLifecycleCategory::None,
                admission->graph_instance_id.value(), run.run_id.value(),
                admission->run_group_id, bundle_id, impl_->counters_locked());
          });
        }
      }
    }

    if (finalization_wait_observer_ != nullptr) {
      finalization_wait_observer_(finalization_wait_observer_context_,
                                  bundle_id, true);
    }
    for (std::size_t index = 0U; index < run_count; ++index) {
      settlement_observers[index].wait_for_resource_settlement();
    }

    {
      std::lock_guard<std::mutex> lock(impl_->fence);
      const auto admission = impl_->find_admission(bundle_id);
      if (admission == impl_->admissions.end()) {
        throw std::logic_error(
            "Run lifecycle bundle disappeared during resource settlement.");
      }
      for (Impl::RunRecord& run : admission->runs) {
        if (!run.quiescent) {
          throw std::logic_error(
              "Run lifecycle resource settlement preceded quiescence.");
        }
        if (!run.resource_settled) {
          run.resource_settled = true;
          publish_committed_transition([&]() {
            impl_->telemetry.publish(
                ExecutionLifecycleEventKind::ResourceSettled,
                ExecutionLifecycleCategory::None,
                admission->graph_instance_id.value(), run.run_id.value(),
                admission->run_group_id, bundle_id, impl_->counters_locked());
          });
        }
      }
      std::array<std::uint64_t, 2U> run_ids{0U, 0U};
      std::size_t run_id_count = 0U;
      for (const Impl::RunRecord& run : admission->runs) {
        run_ids[run_id_count++] = run.run_id.value();
      }
      const auto row = impl_->find_graph(graph_instance_id);
      if (row == impl_->rows.end()) {
        throw std::logic_error(
            "Run lifecycle Graph row disappeared before finalization.");
      }
      row->cancellation_records.remove_if(
          [bundle_id](const Impl::CancellationDispatchRecord& record) {
            return record.bundle_id == bundle_id;
          });
      impl_->admissions.erase(admission);
      for (std::size_t index = 0U; index < run_id_count; ++index) {
        publish_committed_transition([&]() {
          impl_->telemetry.publish(ExecutionLifecycleEventKind::RunUnregistered,
                                   ExecutionLifecycleCategory::None,
                                   graph_instance_id.value(), run_ids[index],
                                   run_group_id, bundle_id,
                                   impl_->counters_locked());
        });
      }
    }
  } catch (...) {
    try {
      std::lock_guard<std::mutex> lock(control->mutex);
      control->state.store(RunLifecycleAdmissionHandleControl::State::Active,
                           std::memory_order_release);
    } catch (...) {
      std::terminate();
    }
    control->changed.notify_all();
    throw;
  }
  try {
    std::lock_guard<std::mutex> lock(control->mutex);
    control->state.store(RunLifecycleAdmissionHandleControl::State::Finalized,
                         std::memory_order_release);
  } catch (...) {
    std::terminate();
  }
  control->changed.notify_all();
  impl_->changed.notify_all();
}

/** @copydoc RunLifecycleRegistry::permits_visible_commit */
bool RunLifecycleRegistry::permits_visible_commit(
    GraphInstanceId graph_instance_id, ComputeRunId run_id) const {
  std::lock_guard<std::mutex> lock(impl_->fence);
  const auto row = impl_->find_graph(graph_instance_id);
  if (row == impl_->rows.cend() || row->state != Impl::GraphState::Open) {
    return false;
  }
  for (const Impl::AdmissionRecord& admission : impl_->admissions) {
    if (admission.graph_instance_id != graph_instance_id) {
      continue;
    }
    for (const Impl::RunRecord& run : admission.runs) {
      if (run.run_id == run_id) {
        return true;
      }
    }
  }
  return false;
}

/** @copydoc RunLifecycleRegistry::begin_graph_close */
std::uint64_t RunLifecycleRegistry::begin_graph_close(
    GraphInstanceId graph_instance_id, ComputeRunCancellationReason reason) {
  const std::uint16_t encoded_reason = encode_candidate_cancellation(reason);
  std::list<Impl::CancellationDispatchRecord> cancellation_records;
  std::uint64_t close_generation = 0U;
  {
    std::lock_guard<std::mutex> lock(impl_->fence);
    const auto row = impl_->find_graph(graph_instance_id);
    if (row == impl_->rows.end()) {
      throw GraphError(GraphErrc::NotFound,
                       "Graph lifecycle row was not found.");
    }
    if (row->state == Impl::GraphState::Open) {
      row->state = Impl::GraphState::Closing;
      row->close_generation = 1U;
      for (const auto& candidate : row->candidates) {
        candidate->cancellation.store(encoded_reason,
                                      std::memory_order_release);
      }
      publish_committed_transition([&]() {
        impl_->telemetry.publish(
            ExecutionLifecycleEventKind::GraphClosing,
            cancellation_category(reason), graph_instance_id.value(), 0U, 0U,
            row->close_generation, impl_->counters_locked());
      });
    }
    close_generation = row->close_generation;
    cancellation_records.splice(cancellation_records.end(),
                                row->cancellation_records);
  }

  for (const Impl::CancellationDispatchRecord& record : cancellation_records) {
    if (record.sibling_commit_gate) {
      record.sibling_commit_gate->abort_hp_commit();
    }
    (void)record.cancellation->request_cancellation_after_linearization(reason);
    std::lock_guard<std::mutex> lock(impl_->fence);
    publish_committed_transition([&]() {
      impl_->telemetry.publish(
          ExecutionLifecycleEventKind::CancellationRequested,
          cancellation_category(reason), graph_instance_id.value(), 0U, 0U,
          close_generation, impl_->counters_locked());
    });
  }

  return close_generation;
}

/** @copydoc RunLifecycleRegistry::finish_graph_close */
void RunLifecycleRegistry::finish_graph_close(
    GraphInstanceId graph_instance_id) {
  std::unique_lock<std::mutex> lock(impl_->fence);
  const auto initial_row = impl_->find_graph(graph_instance_id);
  if (initial_row == impl_->rows.end()) {
    throw GraphError(GraphErrc::NotFound, "Graph lifecycle row was not found.");
  }
  if (initial_row->state != Impl::GraphState::Closing) {
    throw std::logic_error(
        "Graph lifecycle row must be Closing before settlement.");
  }
  const std::uint64_t close_generation = initial_row->close_generation;
  impl_->changed.wait(lock, [this, graph_instance_id]() {
    const auto row = impl_->find_graph(graph_instance_id);
    return row == impl_->rows.end() ||
           (row->candidates.empty() &&
            !impl_->graph_has_admission(graph_instance_id));
  });
  const auto row = impl_->find_graph(graph_instance_id);
  if (row != impl_->rows.end()) {
    impl_->rows.erase(row);
    impl_->telemetry.publish(ExecutionLifecycleEventKind::GraphRowRemoved,
                             ExecutionLifecycleCategory::None,
                             graph_instance_id.value(), 0U, 0U,
                             close_generation, impl_->counters_locked());
  }
  lock.unlock();
  impl_->changed.notify_all();
}

/** @copydoc RunLifecycleRegistry::close_graph */
void RunLifecycleRegistry::close_graph(GraphInstanceId graph_instance_id,
                                       ComputeRunCancellationReason reason) {
  (void)begin_graph_close(graph_instance_id, reason);
  finish_graph_close(graph_instance_id);
}

/** @copydoc RunLifecycleRegistry::begin_service_shutdown */
std::uint64_t RunLifecycleRegistry::begin_service_shutdown() {
  std::list<Impl::CancellationDispatchRecord> cancellation_records;
  std::uint64_t generation = 0U;
  {
    std::lock_guard<std::mutex> lock(impl_->fence);
    if (impl_->service_state == Impl::ServiceState::Stopped) {
      return impl_->shutdown_generation;
    }
    if (impl_->service_state == Impl::ServiceState::Stopping) {
      return impl_->shutdown_generation;
    }
    generation = mint_registry_identity(
        shutdown_generation_counter(),
        "Execution lifecycle shutdown generation space is exhausted.");
    impl_->service_state = Impl::ServiceState::Stopping;
    impl_->shutdown_generation = generation;
    const std::uint16_t encoded = encode_candidate_cancellation(
        ComputeRunCancellationReason::ProcessShutdown);
    for (Impl::GraphRow& row : impl_->rows) {
      if (row.state == Impl::GraphState::Open) {
        row.state = Impl::GraphState::Closing;
        row.close_generation = 1U;
      }
      for (const auto& candidate : row.candidates) {
        candidate->cancellation.store(encoded, std::memory_order_release);
      }
      cancellation_records.splice(cancellation_records.end(),
                                  row.cancellation_records);
    }
    publish_committed_transition([&]() {
      impl_->telemetry.mark_stopping(generation, impl_->counters_locked());
    });
    for (const Impl::GraphRow& row : impl_->rows) {
      publish_committed_transition([&]() {
        impl_->telemetry.publish(ExecutionLifecycleEventKind::GraphClosing,
                                 ExecutionLifecycleCategory::ProcessShutdown,
                                 row.graph_instance_id.value(), 0U, 0U,
                                 row.close_generation,
                                 impl_->counters_locked());
      });
    }
  }

  for (const Impl::CancellationDispatchRecord& record : cancellation_records) {
    if (record.sibling_commit_gate) {
      record.sibling_commit_gate->abort_hp_commit();
    }
    (void)record.cancellation->request_cancellation_after_linearization(
        ComputeRunCancellationReason::ProcessShutdown);
    std::lock_guard<std::mutex> lock(impl_->fence);
    publish_committed_transition([&]() {
      impl_->telemetry.publish(
          ExecutionLifecycleEventKind::CancellationRequested,
          ExecutionLifecycleCategory::ProcessShutdown,
          record.graph_instance_id.value(), 0U, 0U, generation,
          impl_->counters_locked());
    });
  }
  return generation;
}

/** @copydoc RunLifecycleRegistry::wait_until_empty */
void RunLifecycleRegistry::wait_until_empty() {
  std::unique_lock<std::mutex> lock(impl_->fence);
  if (impl_->service_state == Impl::ServiceState::Accepting) {
    throw std::logic_error(
        "Run lifecycle empty wait requires service shutdown.");
  }
  impl_->changed.wait(lock, [this]() {
    return impl_->rows.empty() && impl_->admissions.empty();
  });
}

/** @copydoc RunLifecycleRegistry::mark_service_stopped */
std::uint64_t RunLifecycleRegistry::mark_service_stopped(
    const ExecutionLifecycleCounters& final_counters) {
  if (!all_counters_zero(final_counters)) {
    throw std::invalid_argument(
        "Execution lifecycle final counters must be exactly zero.");
  }
  std::lock_guard<std::mutex> lock(impl_->fence);
  if (impl_->service_state == Impl::ServiceState::Stopped) {
    return impl_->telemetry.publish_service_stopped(impl_->shutdown_generation,
                                                    final_counters);
  }
  if (impl_->service_state != Impl::ServiceState::Stopping ||
      !impl_->rows.empty() || !impl_->admissions.empty()) {
    throw std::logic_error(
        "Execution lifecycle cannot stop before registry settlement.");
  }
  const std::uint64_t sequence = impl_->telemetry.publish_service_stopped(
      impl_->shutdown_generation, final_counters);
  impl_->service_state = Impl::ServiceState::Stopped;
  return sequence;
}

/** @copydoc RunLifecycleRegistry::counters */
ExecutionLifecycleCounters RunLifecycleRegistry::counters() const {
  std::lock_guard<std::mutex> lock(impl_->fence);
  return impl_->counters_locked();
}

/** @copydoc RunLifecycleRegistry::accepting */
bool RunLifecycleRegistry::accepting() const {
  std::lock_guard<std::mutex> lock(impl_->fence);
  return impl_->service_state == Impl::ServiceState::Accepting;
}

/** @copydoc RunLifecycleRegistry::shutdown_generation */
std::uint64_t RunLifecycleRegistry::shutdown_generation() const {
  std::lock_guard<std::mutex> lock(impl_->fence);
  return impl_->shutdown_generation;
}

/** @copydoc RunLifecycleRegistry::rollback_candidate */
void RunLifecycleRegistry::rollback_candidate(
    const std::shared_ptr<RunLifecycleAdmissionCandidateControl>&
        control) noexcept {
  try {
    std::lock_guard<std::mutex> lock(impl_->fence);
    if (control->resolved.exchange(true, std::memory_order_acq_rel)) {
      return;
    }
    const auto row = impl_->find_graph(control->graph_instance_id);
    if (row == impl_->rows.end()) {
      std::terminate();
    }
    const auto candidate =
        std::find(row->candidates.begin(), row->candidates.end(), control);
    if (candidate == row->candidates.end()) {
      std::terminate();
    }
    row->candidates.erase(candidate);
    control->graph_lease = GraphLifetimeLease();
    const auto reason = decode_candidate_cancellation(
        control->cancellation.load(std::memory_order_acquire));
    impl_->telemetry.publish(ExecutionLifecycleEventKind::CandidateRolledBack,
                             reason.has_value()
                                 ? cancellation_category(*reason)
                                 : ExecutionLifecycleCategory::None,
                             control->graph_instance_id.value(), 0U, 0U,
                             control->candidate_id, impl_->counters_locked());
    impl_->changed.notify_all();
  } catch (...) {
    std::terminate();
  }
}

}  // namespace ps::compute
