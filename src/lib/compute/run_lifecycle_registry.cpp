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

}  // namespace

/** @copydoc GraphCloseCoordinator::begin */
GraphCloseCoordinator::Role GraphCloseCoordinator::begin() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!started_) {
    started_ = true;
    return Role::Owner;
  }
  return Role::Joiner;
}

/** @copydoc GraphCloseCoordinator::complete_success */
void GraphCloseCoordinator::complete_success() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!started_ || completed_) {
      throw std::logic_error("Graph close completion requires one live owner.");
    }
    completed_ = true;
  }
  cv_.notify_all();
}

/** @copydoc GraphCloseCoordinator::wait_for_success */
void GraphCloseCoordinator::wait_for_success() {
  std::unique_lock<std::mutex> lock(mutex_);
  if (!started_) {
    throw std::logic_error("Graph close join requires a selected owner.");
  }
  cv_.wait(lock, [this]() { return completed_; });
}

/** @copydoc GraphCloseCoordinator::started */
bool GraphCloseCoordinator::started() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return started_;
}

/** @copydoc GraphCloseCoordinator::completed */
bool GraphCloseCoordinator::completed() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return completed_;
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
    /** @brief Request-level cancellation fan-out invoked outside the fence. */
    std::shared_ptr<ComputeRequestCancellationSource> cancellation;
    /** @brief Realtime pending gate denied before close fan-out. */
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
    /** @brief True after one owner captured and invoked cancellation sources.
     */
    bool cancellation_dispatched = false;
    /** @brief Indexed unresolved candidate controls. */
    std::list<std::shared_ptr<RunLifecycleAdmissionCandidateControl>>
        candidates;
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

/** @copydoc RunLifecycleAdmissionHandle::bundle_id */
std::uint64_t RunLifecycleAdmissionHandle::bundle_id() const {
  if (!active()) {
    throw std::logic_error("Run lifecycle admission handle is inactive.");
  }
  return bundle_id_;
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
                                  false,
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
  std::list<Impl::AdmissionRecord> staged;
  Impl::AdmissionRecord record;
  record.bundle_id = bundle_id;
  record.graph_instance_id = candidate.control_->graph_instance_id;
  record.runs.reserve(1U);
  record.runs.emplace_back(run_lease.descriptor().id(), std::move(run_lease));
  record.cancellation = std::move(cancellation);
  staged.push_back(std::move(record));

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
    impl_->telemetry.publish(ExecutionLifecycleEventKind::BundleAdmitted,
                             ExecutionLifecycleCategory::None,
                             control->graph_instance_id.value(), run_id.value(),
                             0U, bundle_id, impl_->counters_locked());
  }
  candidate.control_.reset();
  impl_->changed.notify_all();
  return RunLifecycleAdmissionHandle(this, bundle_id);
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
  std::list<Impl::AdmissionRecord> staged;
  Impl::AdmissionRecord record;
  record.bundle_id = bundle_id;
  record.graph_instance_id = candidate.control_->graph_instance_id;
  record.run_group_id = run_group_id.value();
  record.runs.reserve(2U);
  record.runs.emplace_back(hp_lease.descriptor().id(), std::move(hp_lease));
  record.runs.emplace_back(rt_lease.descriptor().id(), std::move(rt_lease));
  record.cancellation = std::move(cancellation);
  record.sibling_commit_gate = std::move(sibling_commit_gate);
  staged.push_back(std::move(record));

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
    impl_->telemetry.publish(ExecutionLifecycleEventKind::BundleAdmitted,
                             ExecutionLifecycleCategory::None,
                             control->graph_instance_id.value(),
                             first_run.value(), run_group_id.value(), bundle_id,
                             impl_->counters_locked());
  }
  candidate.control_.reset();
  impl_->changed.notify_all();
  return RunLifecycleAdmissionHandle(this, bundle_id);
}

/** @copydoc RunLifecycleRegistry::finalize_admission */
void RunLifecycleRegistry::finalize_admission(
    RunLifecycleAdmissionHandle handle) {
  if (!handle.active() || handle.registry_ != this) {
    throw std::invalid_argument(
        "Run lifecycle finalization requires a live local handle.");
  }
  const std::uint64_t bundle_id = handle.bundle_id_;
  std::array<ComputeRunLease*, 2U> registry_leases{nullptr, nullptr};
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
        admission->runs.size() > registry_leases.size()) {
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
      registry_leases[run_count++] = &*run.lease;
    }
  }

  for (std::size_t index = 0U; index < run_count; ++index) {
    const std::optional<ComputeRunTerminalOutcome> outcome =
        registry_leases[index]->terminal_outcome();
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
      run.finalizing = true;
      impl_->telemetry.publish(ExecutionLifecycleEventKind::RunTerminal,
                               terminal_category(outcomes[index]),
                               admission->graph_instance_id.value(),
                               run.run_id.value(), admission->run_group_id,
                               bundle_id, impl_->counters_locked());
      ++index;
    }
  }

  for (std::size_t index = 0U; index < run_count; ++index) {
    registry_leases[index]->wait_until_only_lease();
  }

  {
    std::lock_guard<std::mutex> lock(impl_->fence);
    const auto admission = impl_->find_admission(bundle_id);
    if (admission == impl_->admissions.end()) {
      throw std::logic_error(
          "Run lifecycle bundle disappeared during finalization.");
    }
    for (Impl::RunRecord& run : admission->runs) {
      run.quiescent = true;
      impl_->telemetry.publish(ExecutionLifecycleEventKind::RunQuiescent,
                               ExecutionLifecycleCategory::None,
                               admission->graph_instance_id.value(),
                               run.run_id.value(), admission->run_group_id,
                               bundle_id, impl_->counters_locked());
    }
  }

  for (std::size_t index = 0U; index < run_count; ++index) {
    registry_leases[index]->wait_for_resource_settlement();
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
      run.resource_settled = true;
      impl_->telemetry.publish(ExecutionLifecycleEventKind::ResourceSettled,
                               ExecutionLifecycleCategory::None,
                               admission->graph_instance_id.value(),
                               run.run_id.value(), admission->run_group_id,
                               bundle_id, impl_->counters_locked());
    }
    std::array<std::uint64_t, 2U> run_ids{0U, 0U};
    std::size_t run_id_count = 0U;
    for (const Impl::RunRecord& run : admission->runs) {
      run_ids[run_id_count++] = run.run_id.value();
    }
    impl_->admissions.erase(admission);
    for (std::size_t index = 0U; index < run_id_count; ++index) {
      impl_->telemetry.publish(
          ExecutionLifecycleEventKind::RunUnregistered,
          ExecutionLifecycleCategory::None, graph_instance_id.value(),
          run_ids[index], run_group_id, bundle_id, impl_->counters_locked());
    }
  }
  handle.registry_ = nullptr;
  handle.bundle_id_ = 0U;
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
  std::size_t source_count = 0U;
  std::uint64_t close_generation = 0U;
  bool dispatch_cancellation = false;
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
      impl_->telemetry.publish(ExecutionLifecycleEventKind::GraphClosing,
                               cancellation_category(reason),
                               graph_instance_id.value(), 0U, 0U,
                               row->close_generation, impl_->counters_locked());
    }
    close_generation = row->close_generation;
    if (!row->cancellation_dispatched) {
      row->cancellation_dispatched = true;
      dispatch_cancellation = true;
      source_count = static_cast<std::size_t>(std::count_if(
          impl_->admissions.cbegin(), impl_->admissions.cend(),
          [graph_instance_id](const Impl::AdmissionRecord& admission) {
            return admission.graph_instance_id == graph_instance_id;
          }));
    }
  }

  if (!dispatch_cancellation) {
    return close_generation;
  }
  std::vector<std::shared_ptr<ComputeRequestCancellationSource>> sources;
  std::vector<std::shared_ptr<DirtySiblingCommitGate>> gates;
  sources.reserve(source_count);
  gates.reserve(source_count);
  {
    std::lock_guard<std::mutex> lock(impl_->fence);
    for (const Impl::AdmissionRecord& admission : impl_->admissions) {
      if (admission.graph_instance_id == graph_instance_id) {
        sources.push_back(admission.cancellation);
        gates.push_back(admission.sibling_commit_gate);
      }
    }
  }
  for (std::size_t index = 0U; index < sources.size(); ++index) {
    if (gates[index]) {
      gates[index]->abort_hp_commit();
    }
    try {
      (void)sources[index]->request_cancellation(reason);
    } catch (...) {
      // Terminal cancellation is published before callback failure; close is
      // monotonic and must continue settlement.
    }
    std::lock_guard<std::mutex> lock(impl_->fence);
    impl_->telemetry.publish(ExecutionLifecycleEventKind::CancellationRequested,
                             cancellation_category(reason),
                             graph_instance_id.value(), 0U, 0U,
                             close_generation, impl_->counters_locked());
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
  std::size_t source_count = 0U;
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
      row.cancellation_dispatched = true;
      for (const auto& candidate : row.candidates) {
        candidate->cancellation.store(encoded, std::memory_order_release);
      }
    }
    source_count = impl_->admissions.size();
    impl_->telemetry.mark_stopping(generation, impl_->counters_locked());
    for (const Impl::GraphRow& row : impl_->rows) {
      impl_->telemetry.publish(ExecutionLifecycleEventKind::GraphClosing,
                               ExecutionLifecycleCategory::ProcessShutdown,
                               row.graph_instance_id.value(), 0U, 0U,
                               row.close_generation, impl_->counters_locked());
    }
  }

  std::vector<std::shared_ptr<ComputeRequestCancellationSource>> sources;
  std::vector<std::shared_ptr<DirtySiblingCommitGate>> gates;
  sources.reserve(source_count);
  gates.reserve(source_count);
  {
    std::lock_guard<std::mutex> lock(impl_->fence);
    for (const Impl::AdmissionRecord& admission : impl_->admissions) {
      sources.push_back(admission.cancellation);
      gates.push_back(admission.sibling_commit_gate);
    }
  }
  for (std::size_t index = 0U; index < sources.size(); ++index) {
    if (gates[index]) {
      gates[index]->abort_hp_commit();
    }
    try {
      (void)sources[index]->request_cancellation(
          ComputeRunCancellationReason::ProcessShutdown);
    } catch (...) {
      // The accepted terminal reason is stable before any cleanup callback
      // exception. Process shutdown remains monotonic.
    }
    std::lock_guard<std::mutex> lock(impl_->fence);
    impl_->telemetry.publish(ExecutionLifecycleEventKind::CancellationRequested,
                             ExecutionLifecycleCategory::ProcessShutdown, 0U,
                             0U, 0U, generation, impl_->counters_locked());
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
