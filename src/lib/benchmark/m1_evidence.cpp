/**
 * @file m1_evidence.cpp
 * @brief Implements the fail-closed five-axis M1 inner-row evaluator.
 */
#include "benchmark/m1_evidence.hpp"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace ps::benchmark {
namespace {

// NOLINTBEGIN(whitespace/indent_namespace)

/** @brief Frozen process Host limits from the execution-profile contract. */
constexpr ResourceVector kM1HostLimits{32U, 1073741824U, 536870912U, 65536U,
                                       268435456U};

/** @brief Frozen Throughput capacity after Interactive headroom subtraction. */
constexpr ResourceVector kM1ThroughputCapacity{31U, 1006632960U, 503316480U,
                                               64512U, 251658240U};

/** @brief Frozen configured-Metal memory and scratch limits. */
constexpr DeviceResourceVector kM1MetalLimits{536870912U, 268435456U};

// NOLINTEND

/**
 * @brief Appends one stable diagnostic once.
 * @param reasons Mutable diagnostic collection.
 * @param reason Complete stable reason.
 * @return Nothing.
 * @throws std::bad_alloc when reason ownership allocates.
 */
void invalidate_m1(std::vector<std::string>* reasons, std::string reason) {
  if (std::find(reasons->begin(), reasons->end(), reason) == reasons->end()) {
    reasons->push_back(std::move(reason));
  }
}

/**
 * @brief Adds one uint64 contribution without wraparound.
 * @param value Mutable aggregate.
 * @param contribution Nonnegative contribution.
 * @return True when the exact sum was stored.
 * @throws Nothing.
 */
bool checked_accumulate(std::uint64_t* value,
                        std::uint64_t contribution) noexcept {
  if (*value > std::numeric_limits<std::uint64_t>::max() - contribution) {
    return false;
  }
  *value += contribution;
  return true;
}

/**
 * @brief Tests whether a Host resource vector is exactly zero.
 * @param value Complete resource vector.
 * @return True only when every dimension is zero.
 * @throws Nothing.
 */
bool zero_resources(const ResourceVector& value) noexcept {
  return value == ResourceVector{};
}

/**
 * @brief Tests component-wise high-water monotonicity.
 * @param prior Earlier lifetime high-water.
 * @param current Later lifetime high-water.
 * @return True when no dimension decreased.
 * @throws Nothing.
 */
bool resource_high_water_nondecreasing(const ResourceVector& prior,
                                       const ResourceVector& current) noexcept {
  return prior.cpu_slots <= current.cpu_slots &&
         prior.retained_memory_bytes <= current.retained_memory_bytes &&
         prior.scratch_bytes <= current.scratch_bytes &&
         prior.ready_entries <= current.ready_entries &&
         prior.ready_bytes <= current.ready_bytes;
}

/**
 * @brief Tests whether all lifecycle ownership counters are zero.
 * @param counters Complete final counter set.
 * @return True only for exact process settlement.
 * @throws Nothing.
 */
bool lifecycle_settled(
    const compute::ExecutionLifecycleCounters& counters) noexcept {
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
 * @brief Tests one closed lifecycle service-state representation.
 * @param state Candidate enum.
 * @return True for Accepting, Stopping, or Stopped.
 * @throws Nothing.
 */
bool known_service_state(
    compute::ExecutionLifecycleServiceState state) noexcept {
  switch (state) {
    case compute::ExecutionLifecycleServiceState::Accepting:
    case compute::ExecutionLifecycleServiceState::Stopping:
    case compute::ExecutionLifecycleServiceState::Stopped:
      return true;
  }
  return false;
}

/**
 * @brief Tests one closed lifecycle event-kind representation.
 * @param kind Candidate enum.
 * @return True only for one declared version-1 event kind.
 * @throws Nothing.
 */
bool known_lifecycle_event_kind(
    compute::ExecutionLifecycleEventKind kind) noexcept {
  switch (kind) {
    case compute::ExecutionLifecycleEventKind::ServiceStarted:
    case compute::ExecutionLifecycleEventKind::GraphRegistered:
    case compute::ExecutionLifecycleEventKind::GraphClosing:
    case compute::ExecutionLifecycleEventKind::CandidateBegan:
    case compute::ExecutionLifecycleEventKind::CandidateRolledBack:
    case compute::ExecutionLifecycleEventKind::BundleAdmitted:
    case compute::ExecutionLifecycleEventKind::CancellationRequested:
    case compute::ExecutionLifecycleEventKind::RunTerminal:
    case compute::ExecutionLifecycleEventKind::RunQuiescent:
    case compute::ExecutionLifecycleEventKind::ResourceSettled:
    case compute::ExecutionLifecycleEventKind::RunUnregistered:
    case compute::ExecutionLifecycleEventKind::GraphRowRemoved:
    case compute::ExecutionLifecycleEventKind::WorkerJoined:
    case compute::ExecutionLifecycleEventKind::BindingRetired:
    case compute::ExecutionLifecycleEventKind::ServiceStopped:
      return true;
  }
  return false;
}

/**
 * @brief Tests one closed lifecycle event-category representation.
 * @param category Candidate enum.
 * @return True only for one declared version-1 category.
 * @throws Nothing.
 */
bool known_lifecycle_category(
    compute::ExecutionLifecycleCategory category) noexcept {
  switch (category) {
    case compute::ExecutionLifecycleCategory::None:
    case compute::ExecutionLifecycleCategory::ExplicitRequest:
    case compute::ExecutionLifecycleCategory::Deadline:
    case compute::ExecutionLifecycleCategory::Superseded:
    case compute::ExecutionLifecycleCategory::GraphClose:
    case compute::ExecutionLifecycleCategory::ProcessShutdown:
    case compute::ExecutionLifecycleCategory::Succeeded:
    case compute::ExecutionLifecycleCategory::Cancelled:
    case compute::ExecutionLifecycleCategory::FailureResourceExhausted:
    case compute::ExecutionLifecycleCategory::FailureOther:
      return true;
  }
  return false;
}

/**
 * @brief Validates the identity/category shape authored for one event kind.
 * @param event Candidate closed lifecycle record.
 * @return True only when required and absent identities match the producer.
 * @throws Nothing.
 * @note This validates scalar shape, not cross-record lifecycle order; sequence
 * and cursor replay provide that independent boundary.
 */
bool valid_lifecycle_event_identity(
    const compute::ExecutionLifecycleEvent& event) noexcept {
  using Category = compute::ExecutionLifecycleCategory;
  using Kind = compute::ExecutionLifecycleEventKind;
  const bool graph = event.graph_instance_id != 0U;
  const bool run = event.run_id != 0U;
  const bool group = event.run_group_id != 0U;
  const bool generation = event.generation != 0U;
  const bool none = event.category == Category::None;
  const bool terminal = event.category == Category::Succeeded ||
                        event.category == Category::Cancelled ||
                        event.category == Category::FailureResourceExhausted ||
                        event.category == Category::FailureOther;
  switch (event.kind) {
    case Kind::ServiceStarted:
      return !graph && !run && !group && !generation && none;
    case Kind::GraphRegistered:
      return graph && !run && !group && !generation && none;
    case Kind::GraphClosing:
      return graph && !run && !group && generation &&
             (event.category == Category::GraphClose ||
              event.category == Category::ProcessShutdown);
    case Kind::CandidateBegan:
      return graph && !run && !group && generation && none;
    case Kind::CandidateRolledBack:
      return graph && !run && !group && generation &&
             (none || event.category == Category::GraphClose ||
              event.category == Category::ProcessShutdown);
    case Kind::BundleAdmitted:
      return graph && run && generation && none;
    case Kind::CancellationRequested:
      return graph && !run && !group && generation &&
             (event.category == Category::GraphClose ||
              event.category == Category::ProcessShutdown);
    case Kind::RunTerminal:
      return graph && run && generation && terminal;
    case Kind::RunQuiescent:
    case Kind::ResourceSettled:
    case Kind::RunUnregistered:
      return graph && run && generation && none;
    case Kind::GraphRowRemoved:
      return graph && !run && !group && none;
    case Kind::WorkerJoined:
      return !graph && !run && !group && generation && none;
    case Kind::BindingRetired:
      return !graph && !run && !group && generation &&
             (none || event.category == Category::FailureOther);
    case Kind::ServiceStopped:
      return !graph && !run && !group && generation && none;
  }
  return false;
}

/**
 * @brief Invalid-priority conjunction of independent SLO verdicts.
 * @param verdicts Complete axis verdicts.
 * @param reasons Diagnostics receiving unknown enum evidence.
 * @return Invalid, Fail, or Pass without substitution.
 * @throws std::bad_alloc when an unknown-enum reason allocates.
 */
I1Verdict compose_m1_row(const std::vector<I1Verdict>& verdicts,
                         std::vector<std::string>* reasons) {
  bool failed = false;
  for (const I1Verdict verdict : verdicts) {
    switch (verdict) {
      case I1Verdict::Pass:
        break;
      case I1Verdict::Fail:
        failed = true;
        break;
      case I1Verdict::Invalid:
        return I1Verdict::Invalid;
      default:
        invalidate_m1(reasons, "M1 inner row contains an unknown verdict");
        return I1Verdict::Invalid;
    }
  }
  return failed ? I1Verdict::Fail : I1Verdict::Pass;
}

/**
 * @brief Memory validation classification separating shape from SLO failures.
 * @throws Nothing for value construction.
 */
struct M1MemoryValidation final {
  /** @brief False for missing, malformed, lossy, or contradictory evidence. */
  bool valid = true;
  /** @brief False when an authoritative high-water exceeds its fixed limit. */
  bool within_limits = true;
  /** @brief False when final ownership or Compute I/O remains nonzero. */
  bool settled = true;
};

/** @brief Exact number of policy bindings published before ServiceStarted. */
constexpr std::uint64_t kM1InitialPolicyBindingCount = 2U;

/**
 * @brief Monotonic state of one replayed Graph registry row.
 * @throws Nothing for value construction and comparison.
 */
enum class M1ReplayGraphState : std::uint8_t {
  /** @brief Candidate and bundle admission remains legal. */
  Open,
  /** @brief Admission is closed while candidates and bundles settle. */
  Closing,
};

/**
 * @brief Monotonic state of one replayed child Run.
 * @throws Nothing for value construction and comparison.
 */
enum class M1ReplayRunState : std::uint8_t {
  /** @brief The child is installed and has not published terminal state. */
  Admitted,
  /** @brief Terminal state is published but non-registry leases may remain. */
  Terminal,
  /** @brief Only the registry lease remains. */
  Quiescent,
  /** @brief Root reservation and every child grant have returned. */
  ResourceSettled,
  /** @brief The child has left every registry index. */
  Unregistered,
};

/**
 * @brief Replay state for one live Graph row and its anonymous candidates.
 *
 * BundleAdmitted does not carry the consumed candidate id. The replay
 * therefore proves the only producer-observable relation: every explicit
 * rollback names a unique begun candidate and the number of commits plus
 * rollbacks never exceeds the number begun on this Graph.
 *
 * @throws std::bad_alloc when candidate identities are retained.
 */
struct M1ReplayGraph final {
  /** @brief Current monotonic row state. */
  M1ReplayGraphState state = M1ReplayGraphState::Open;
  /** @brief Nonzero close generation after Open-to-Closing. */
  std::uint64_t close_generation = 0U;
  /** @brief Cancellation category currently visible to pending candidates. */
  compute::ExecutionLifecycleCategory candidate_cancellation =
      compute::ExecutionLifecycleCategory::None;
  /** @brief Every unique candidate identity begun on this row. */
  std::set<std::uint64_t> candidate_ids;
  /** @brief Candidate identities that explicitly rolled back. */
  std::set<std::uint64_t> rolled_back_candidate_ids;
  /** @brief Number of anonymous candidates consumed by BundleAdmitted. */
  std::uint64_t committed_candidate_count = 0U;
};

/**
 * @brief Replay state for one child Run identity.
 * @throws Nothing for value construction and movement.
 */
struct M1ReplayRun final {
  /** @brief Exact globally non-reused child identity. */
  std::uint64_t run_id = 0U;
  /** @brief Current monotonic settlement phase. */
  M1ReplayRunState state = M1ReplayRunState::Admitted;
};

/**
 * @brief Replay state for one standalone or two-child realtime bundle.
 * @throws std::bad_alloc when the second realtime child identity is learned.
 */
struct M1ReplayBundle final {
  /** @brief Exact registry-private bundle identity. */
  std::uint64_t bundle_id = 0U;
  /** @brief Exact owning Graph identity. */
  std::uint64_t graph_instance_id = 0U;
  /** @brief Zero for standalone or the exact realtime group identity. */
  std::uint64_t run_group_id = 0U;
  /** @brief One for standalone or two for realtime. */
  std::size_t expected_run_count = 0U;
  /** @brief Children in producer order; realtime child two is learned later. */
  std::vector<M1ReplayRun> runs;
  /** @brief True after the admission was erased before unregister events. */
  bool detached = false;
  /** @brief True after close/shutdown captured its cancellation record. */
  bool cancellation_captured = false;
};

/**
 * @brief One pending second child event emitted under the registry fence.
 * @throws Nothing for value construction.
 */
struct M1ReplayGroupStep final {
  /** @brief Exact bundle whose second child must be emitted next. */
  std::uint64_t bundle_id = 0U;
  /** @brief Exact child transition kind that must complete the pair. */
  compute::ExecutionLifecycleEventKind kind =
      compute::ExecutionLifecycleEventKind::RunTerminal;
};

/** @brief Comparable key for one captured cancellation publication count. */
using M1CancelKey = std::tuple<std::uint64_t, std::uint16_t, std::uint64_t>;

/**
 * @brief Tests exact equality of the nine registry-derived counter fields.
 * @param observed Event/page counter view supplied by telemetry.
 * @param expected Independently replayed registry counter view.
 * @return True only when every registry-derived field is identical.
 * @throws Nothing.
 */
bool equal_registry_counters(
    const compute::ExecutionLifecycleCounters& observed,
    const compute::ExecutionLifecycleCounters& expected) noexcept {
  return observed.registered_graph_count == expected.registered_graph_count &&
         observed.open_graph_count == expected.open_graph_count &&
         observed.closing_graph_count == expected.closing_graph_count &&
         observed.pending_candidate_count == expected.pending_candidate_count &&
         observed.admitted_standalone_run_count ==
             expected.admitted_standalone_run_count &&
         observed.admitted_run_group_count ==
             expected.admitted_run_group_count &&
         observed.admitted_child_run_count ==
             expected.admitted_child_run_count &&
         observed.terminal_not_quiescent_run_count ==
             expected.terminal_not_quiescent_run_count &&
         observed.finalizing_run_count == expected.finalizing_run_count;
}

/**
 * @brief Replays one complete lossless lifecycle stream as producer state.
 *
 * The replay owns no product authority. It reconstructs Graph rows,
 * anonymous candidate consumption, standalone/group admission, ordered child
 * settlement, cancellation fan-out, Graph removal, and the no-event service
 * Stopping transition. Every event and page counter view is checked against
 * the resulting registry state. Physical counters remain independently
 * sampled facts and are checked only for producer-guaranteed capacity,
 * ownership reachability, origin, and final-zero constraints.
 *
 * @throws std::bad_alloc when replay maps, sets, vectors, or diagnostics grow.
 */
class M1LifecycleReplay final {
 public:
  /**
   * @brief Binds stable invalidation output for the complete replay.
   * @param reasons Mutable row diagnostics that outlive this replay.
   * @throws Nothing.
   */
  explicit M1LifecycleReplay(std::vector<std::string>* reasons) noexcept
      : reasons_(reasons) {}

  /**
   * @brief Reports whether the no-event service Stopping transition occurred.
   * @return True after begin_shutdown() accepts one generation.
   * @throws Nothing.
   */
  bool shutdown_started() const noexcept { return shutdown_started_; }

  /**
   * @brief Applies producer-atomic Accepting-to-Stopping state.
   *
   * Every live row becomes Closing before the first ProcessShutdown
   * GraphClosing event, candidate cancellation changes to ProcessShutdown,
   * and every not-yet-captured admission cancellation record is captured.
   *
   * @param generation Exact nonzero page shutdown generation.
   * @return True when the transition is new or idempotently identical.
   * @throws std::bad_alloc when shutdown enumeration/cancellation state grows.
   */
  bool begin_shutdown(std::uint64_t generation) {
    if (generation == 0U || service_stopped_) {
      return reject("shutdown transition has an invalid generation or state");
    }
    if (shutdown_started_) {
      if (shutdown_generation_ != generation) {
        return reject("shutdown generation changed during replay");
      }
      return true;
    }
    shutdown_started_ = true;
    shutdown_generation_ = generation;
    for (const std::uint64_t graph_id : graph_registration_order_) {
      const auto found = graphs_.find(graph_id);
      if (found == graphs_.end()) {
        continue;
      }
      M1ReplayGraph& graph = found->second;
      if (graph.state == M1ReplayGraphState::Open) {
        graph.state = M1ReplayGraphState::Closing;
        graph.close_generation = 1U;
      }
      graph.candidate_cancellation =
          compute::ExecutionLifecycleCategory::ProcessShutdown;
      shutdown_graph_closing_order_.push_back(graph_id);
      capture_cancellations(
          graph_id, compute::ExecutionLifecycleCategory::ProcessShutdown,
          generation);
    }
    return true;
  }

  /**
   * @brief Applies and validates one exact lifecycle event.
   * @param event Next globally sequenced producer record.
   * @return True when its transition and complete counter view are valid.
   * @throws std::bad_alloc when state or diagnostics grow.
   */
  bool apply(const compute::ExecutionLifecycleEvent& event) {
    if (service_stopped_) {
      return reject("ordinary event appears after ServiceStopped");
    }
    if (!expected_atomic_event(event)) {
      return false;
    }

    bool transition_valid = true;
    using Kind = compute::ExecutionLifecycleEventKind;
    switch (event.kind) {
      case Kind::ServiceStarted:
        if (service_started_ || event.sequence != 1U || !graphs_.empty() ||
            !bundles_.empty()) {
          transition_valid = reject(
              "ServiceStarted is duplicated or has prior producer state");
        } else {
          service_started_ = true;
        }
        break;
      case Kind::GraphRegistered:
        transition_valid = register_graph(event);
        break;
      case Kind::GraphClosing:
        transition_valid = close_graph(event);
        break;
      case Kind::CandidateBegan:
        transition_valid = begin_candidate(event);
        break;
      case Kind::CandidateRolledBack:
        transition_valid = rollback_candidate(event);
        break;
      case Kind::BundleAdmitted:
        transition_valid = admit_bundle(event);
        break;
      case Kind::CancellationRequested:
        transition_valid = consume_cancellation(event);
        break;
      case Kind::RunTerminal:
      case Kind::RunQuiescent:
      case Kind::ResourceSettled:
      case Kind::RunUnregistered:
        transition_valid = advance_run(event);
        break;
      case Kind::GraphRowRemoved:
        transition_valid = remove_graph(event);
        break;
      case Kind::WorkerJoined:
        if (!shutdown_started_ || event.generation != shutdown_generation_) {
          transition_valid =
              reject("WorkerJoined is outside its shutdown generation");
        }
        break;
      case Kind::BindingRetired:
        break;
      case Kind::ServiceStopped:
        transition_valid = stop_service(event);
        break;
    }

    const bool counters_valid = validate_counter_view(
        event.counters, event.kind == Kind::ServiceStarted,
        event.kind == Kind::ServiceStopped, "event");
    return transition_valid && counters_valid;
  }

  /**
   * @brief Validates the complete counter view at one atomic page cut.
   * @param page Exact copied telemetry page after all returned records.
   * @return True when registry and physical counter constraints hold.
   * @throws std::bad_alloc when diagnostics grow.
   */
  bool validate_page(const compute::ExecutionLifecyclePage& page) {
    bool valid = validate_counter_view(
        page.counters, false,
        page.service_state == compute::ExecutionLifecycleServiceState::Stopped,
        "page");
    if (shutdown_started_ && page.shutdown_generation != shutdown_generation_) {
      valid = reject("page shutdown generation differs from replay") && valid;
    }
    if (!shutdown_started_ && page.shutdown_generation != 0U) {
      valid = reject("Accepting replay has a shutdown generation") && valid;
    }
    if (page.service_state ==
            compute::ExecutionLifecycleServiceState::Accepting &&
        shutdown_started_) {
      valid = reject("page service state moved behind replay") && valid;
    }
    if (page.service_state ==
            compute::ExecutionLifecycleServiceState::Stopping &&
        (!shutdown_started_ || service_stopped_)) {
      valid =
          reject("Stopping page contradicts replayed service state") && valid;
    }
    if (page.service_state ==
            compute::ExecutionLifecycleServiceState::Stopped &&
        !service_stopped_) {
      valid = reject("Stopped page lacks replayed ServiceStopped") && valid;
    }
    return valid;
  }

  /**
   * @brief Verifies no producer-atomic transition or owned row remains open.
   * @return True only for a complete M1 final capture.
   * @throws std::bad_alloc when diagnostics grow.
   */
  bool complete() {
    bool valid = true;
    if (!service_started_) {
      valid = reject("history lacks the unique ServiceStarted origin") && valid;
    }
    if (!shutdown_started_ || !service_stopped_) {
      valid =
          reject("history lacks the final ServiceStopped settlement") && valid;
    }
    if (pending_group_step_.has_value() ||
        shutdown_graph_closing_index_ != shutdown_graph_closing_order_.size()) {
      valid = reject("history ends inside one producer-atomic event batch") &&
              valid;
    }
    if (!graphs_.empty() || !bundles_.empty() ||
        !pending_cancellations_.empty()) {
      valid = reject("history ends with live Graph, bundle, or cancellation") &&
              valid;
    }
    return valid;
  }

 private:
  /**
   * @brief Appends one stable lifecycle replay invalidation.
   * @param reason Detail without the common M1 prefix.
   * @return Always false for direct guard composition.
   * @throws std::bad_alloc when diagnostics allocate.
   */
  bool reject(std::string reason) {
    invalidate_m1(reasons_, "M1 memory evidence: lifecycle " + reason);
    return false;
  }

  /**
   * @brief Returns one Graph's current producer-visible candidate count.
   * @param graph Exact retained Graph state.
   * @return Begun minus rolled-back minus anonymously committed candidates.
   * @throws Nothing; impossible arithmetic terminates replay through caller
   * validation before this helper is used.
   */
  static std::uint64_t pending_candidates(const M1ReplayGraph& graph) noexcept {
    const std::uint64_t begun =
        static_cast<std::uint64_t>(graph.candidate_ids.size());
    const std::uint64_t rolled_back =
        static_cast<std::uint64_t>(graph.rolled_back_candidate_ids.size());
    return begun - rolled_back - graph.committed_candidate_count;
  }

  /**
   * @brief Recomputes the exact nine registry-derived counters.
   * @return Fresh authority-free replay view with physical fields left zero.
   * @throws Nothing.
   */
  compute::ExecutionLifecycleCounters counters() const noexcept {
    compute::ExecutionLifecycleCounters result;
    result.registered_graph_count = static_cast<std::uint64_t>(graphs_.size());
    for (const auto& entry : graphs_) {
      const M1ReplayGraph& graph = entry.second;
      if (graph.state == M1ReplayGraphState::Open) {
        ++result.open_graph_count;
      } else {
        ++result.closing_graph_count;
      }
      result.pending_candidate_count += pending_candidates(graph);
    }
    for (const auto& entry : bundles_) {
      const M1ReplayBundle& bundle = entry.second;
      if (bundle.detached) {
        continue;
      }
      if (bundle.run_group_id == 0U) {
        ++result.admitted_standalone_run_count;
      } else {
        ++result.admitted_run_group_count;
        result.admitted_child_run_count +=
            static_cast<std::uint64_t>(bundle.expected_run_count);
      }
      for (const M1ReplayRun& run : bundle.runs) {
        if (run.state == M1ReplayRunState::Terminal) {
          ++result.terminal_not_quiescent_run_count;
        }
        if (run.state == M1ReplayRunState::Terminal ||
            run.state == M1ReplayRunState::Quiescent ||
            run.state == M1ReplayRunState::ResourceSettled) {
          ++result.finalizing_run_count;
        }
      }
    }
    return result;
  }

  /**
   * @brief Validates exact registry counters and conservative physical facts.
   * @param observed Complete event/page counter view.
   * @param service_origin Whether this is the unique ServiceStarted record.
   * @param final_stop Whether this cut is the final ServiceStopped state.
   * @param context Stable `event` or `page` diagnostic label.
   * @return True only when every producer-guaranteed relation holds.
   * @throws std::bad_alloc when diagnostics allocate.
   * @note Physical current counters may rise or fall between lifecycle events;
   * this method deliberately derives no event-kind delta from them.
   */
  bool validate_counter_view(
      const compute::ExecutionLifecycleCounters& observed, bool service_origin,
      bool final_stop, const char* context) {
    bool valid = true;
    const compute::ExecutionLifecycleCounters expected = counters();
    if (!equal_registry_counters(observed, expected)) {
      valid = reject(std::string(context) +
                     " registry-derived counters differ from replay") &&
              valid;
    }

    if (observed.ready_entry_count > kM1HostLimits.ready_entries ||
        observed.ready_entry_count > std::numeric_limits<std::uint64_t>::max() -
                                         observed.entered_callback_count ||
        observed.ready_entry_count + observed.entered_callback_count >
            observed.live_child_grant_count) {
      valid = reject(std::string(context) +
                     " physical ready/callback/grant bounds are impossible") &&
              valid;
    }
    if (observed.live_child_grant_count != 0U &&
        observed.live_root_reservation_count == 0U) {
      valid = reject(std::string(context) +
                     " child grant has no reachable root reservation") &&
              valid;
    }
    if (observed.live_policy_invocation_count != 0U &&
        observed.live_policy_binding_count == 0U) {
      valid = reject(std::string(context) +
                     " policy invocation has no live binding") &&
              valid;
    }

    const std::uint64_t admitted_run_count =
        expected.admitted_standalone_run_count +
        expected.admitted_child_run_count;
    const bool execution_owner = observed.ready_entry_count != 0U ||
                                 observed.entered_callback_count != 0U ||
                                 observed.live_policy_invocation_count != 0U;
    if (execution_owner && admitted_run_count == 0U) {
      valid = reject(std::string(context) +
                     " executing Run owner has no admitted child") &&
              valid;
    }
    const bool resource_owner = observed.live_root_reservation_count != 0U ||
                                observed.live_child_grant_count != 0U;
    if (resource_owner && admitted_run_count == 0U &&
        expected.pending_candidate_count == 0U) {
      valid = reject(std::string(context) +
                     " resource owner has no admitted child or staged "
                     "candidate") &&
              valid;
    }

    if (service_origin &&
        (execution_owner || resource_owner ||
         observed.live_policy_binding_count != kM1InitialPolicyBindingCount)) {
      valid =
          reject("ServiceStarted physical origin is not producer-reachable") &&
          valid;
    }
    if (final_stop && !lifecycle_settled(observed)) {
      valid = reject("ServiceStopped counters are not exactly zero") && valid;
    }
    return valid;
  }

  /**
   * @brief Enforces multi-record batches published under one registry fence.
   * @param event Next event candidate.
   * @return True when no batch is open or this is its required next record.
   * @throws std::bad_alloc when diagnostics allocate.
   */
  bool expected_atomic_event(const compute::ExecutionLifecycleEvent& event) {
    if (pending_group_step_.has_value() &&
        (event.generation != pending_group_step_->bundle_id ||
         event.kind != pending_group_step_->kind)) {
      return reject("another event split one realtime child transition pair");
    }
    if (shutdown_graph_closing_index_ < shutdown_graph_closing_order_.size()) {
      const std::uint64_t expected_graph =
          shutdown_graph_closing_order_[shutdown_graph_closing_index_];
      if (event.kind != compute::ExecutionLifecycleEventKind::GraphClosing ||
          event.category !=
              compute::ExecutionLifecycleCategory::ProcessShutdown ||
          event.graph_instance_id != expected_graph) {
        return reject(
            "another event split the process-shutdown GraphClosing batch");
      }
    }
    return true;
  }

  /**
   * @brief Applies one GraphRegistered event.
   * @param event Exact candidate event.
   * @return True only for a fresh Graph during Accepting.
   * @throws std::bad_alloc when Graph/order storage grows.
   */
  bool register_graph(const compute::ExecutionLifecycleEvent& event) {
    if (!service_started_ || shutdown_started_ ||
        all_graph_ids_.count(event.graph_instance_id) != 0U) {
      return reject("GraphRegistered is not a fresh Accepting row");
    }
    graphs_.emplace(event.graph_instance_id, M1ReplayGraph{});
    all_graph_ids_.insert(event.graph_instance_id);
    graph_registration_order_.push_back(event.graph_instance_id);
    return true;
  }

  /**
   * @brief Captures every still-indexed cancellation record for one Graph.
   * @param graph_id Exact Graph identity.
   * @param category GraphClose or ProcessShutdown publication category.
   * @param generation Close or shutdown generation carried by cancellation.
   * @return Nothing.
   * @throws std::bad_alloc when a new expectation key is inserted.
   */
  void capture_cancellations(std::uint64_t graph_id,
                             compute::ExecutionLifecycleCategory category,
                             std::uint64_t generation) {
    const M1CancelKey key{graph_id, static_cast<std::uint16_t>(category),
                          generation};
    for (auto& entry : bundles_) {
      M1ReplayBundle& bundle = entry.second;
      if (bundle.graph_instance_id != graph_id || bundle.detached ||
          bundle.cancellation_captured) {
        continue;
      }
      bundle.cancellation_captured = true;
      ++pending_cancellations_[key];
    }
  }

  /**
   * @brief Applies explicit or process-shutdown GraphClosing publication.
   * @param event Exact next GraphClosing event.
   * @return True only when row state, generation, and shutdown order match.
   * @throws std::bad_alloc when cancellation expectations grow.
   */
  bool close_graph(const compute::ExecutionLifecycleEvent& event) {
    const auto found = graphs_.find(event.graph_instance_id);
    if (found == graphs_.end()) {
      return reject("GraphClosing names no live row");
    }
    M1ReplayGraph& graph = found->second;
    if (shutdown_started_) {
      if (event.category !=
              compute::ExecutionLifecycleCategory::ProcessShutdown ||
          shutdown_graph_closing_index_ >=
              shutdown_graph_closing_order_.size() ||
          shutdown_graph_closing_order_[shutdown_graph_closing_index_] !=
              event.graph_instance_id ||
          event.generation != graph.close_generation) {
        return reject("process-shutdown GraphClosing identity drifted");
      }
      ++shutdown_graph_closing_index_;
      return true;
    }
    if (graph.state != M1ReplayGraphState::Open || event.generation != 1U) {
      return reject("explicit GraphClosing did not perform Open-to-Closing");
    }
    graph.state = M1ReplayGraphState::Closing;
    graph.close_generation = event.generation;
    graph.candidate_cancellation = event.category;
    capture_cancellations(event.graph_instance_id, event.category,
                          event.generation);
    return true;
  }

  /**
   * @brief Applies one CandidateBegan event.
   * @param event Exact candidate identity publication.
   * @return True only for a fresh candidate on an Open Accepting row.
   * @throws std::bad_alloc when candidate identity sets grow.
   */
  bool begin_candidate(const compute::ExecutionLifecycleEvent& event) {
    const auto found = graphs_.find(event.graph_instance_id);
    if (shutdown_started_ || found == graphs_.end() ||
        found->second.state != M1ReplayGraphState::Open ||
        !all_candidate_ids_.insert(event.generation).second) {
      return reject("CandidateBegan is stale, reused, or not admissible");
    }
    found->second.candidate_ids.insert(event.generation);
    return true;
  }

  /**
   * @brief Applies one identity-bearing candidate rollback.
   * @param event Exact rollback publication.
   * @return True only for one unresolved candidate with the current reason.
   * @throws std::bad_alloc when rollback identity storage grows.
   */
  bool rollback_candidate(const compute::ExecutionLifecycleEvent& event) {
    const auto found = graphs_.find(event.graph_instance_id);
    if (found == graphs_.end()) {
      return reject("CandidateRolledBack names no live Graph");
    }
    M1ReplayGraph& graph = found->second;
    if (graph.candidate_ids.count(event.generation) == 0U ||
        graph.rolled_back_candidate_ids.count(event.generation) != 0U ||
        event.category != graph.candidate_cancellation) {
      return reject("CandidateRolledBack identity or cancellation drifted");
    }
    const std::uint64_t begun =
        static_cast<std::uint64_t>(graph.candidate_ids.size());
    const std::uint64_t next_rolled_back =
        static_cast<std::uint64_t>(graph.rolled_back_candidate_ids.size() + 1U);
    if (graph.committed_candidate_count > begun - next_rolled_back) {
      return reject("candidate rollback conflicts with anonymous commit");
    }
    graph.rolled_back_candidate_ids.insert(event.generation);
    return true;
  }

  /**
   * @brief Applies one standalone or realtime bundle admission.
   * @param event Bundle identity plus first child and optional group identity.
   * @return True only when one pending candidate can be consumed.
   * @throws std::bad_alloc when bundle/run/group identity storage grows.
   */
  bool admit_bundle(const compute::ExecutionLifecycleEvent& event) {
    const auto graph_found = graphs_.find(event.graph_instance_id);
    if (shutdown_started_ || graph_found == graphs_.end() ||
        graph_found->second.state != M1ReplayGraphState::Open ||
        pending_candidates(graph_found->second) == 0U ||
        !all_bundle_ids_.insert(event.generation).second ||
        !all_run_ids_.insert(event.run_id).second ||
        (event.run_group_id != 0U &&
         !all_run_group_ids_.insert(event.run_group_id).second)) {
      return reject("BundleAdmitted identity or candidate consumption drifted");
    }
    ++graph_found->second.committed_candidate_count;
    M1ReplayBundle bundle;
    bundle.bundle_id = event.generation;
    bundle.graph_instance_id = event.graph_instance_id;
    bundle.run_group_id = event.run_group_id;
    bundle.expected_run_count = event.run_group_id == 0U ? 1U : 2U;
    bundle.runs.push_back(
        M1ReplayRun{event.run_id, M1ReplayRunState::Admitted});
    bundles_.emplace(bundle.bundle_id, std::move(bundle));
    return true;
  }

  /**
   * @brief Consumes one previously captured cancellation publication.
   * @param event Graph/category/generation correlation record.
   * @return True only when one captured bundle record remains.
   * @throws std::bad_alloc when diagnostics allocate.
   */
  bool consume_cancellation(const compute::ExecutionLifecycleEvent& event) {
    const M1CancelKey key{event.graph_instance_id,
                          static_cast<std::uint16_t>(event.category),
                          event.generation};
    const auto found = pending_cancellations_.find(key);
    if (found == pending_cancellations_.end() || found->second == 0U) {
      return reject("CancellationRequested has no captured bundle");
    }
    if (--found->second == 0U) {
      pending_cancellations_.erase(found);
    }
    return true;
  }

  /**
   * @brief Maps one Run event kind to its required prior and next state.
   * @param kind RunTerminal, RunQuiescent, ResourceSettled, or RunUnregistered.
   * @return Exact prior/next pair.
   * @throws std::logic_error for a non-Run kind.
   */
  static std::pair<M1ReplayRunState, M1ReplayRunState> run_transition(
      compute::ExecutionLifecycleEventKind kind) {
    using Kind = compute::ExecutionLifecycleEventKind;
    switch (kind) {
      case Kind::RunTerminal:
        return {M1ReplayRunState::Admitted, M1ReplayRunState::Terminal};
      case Kind::RunQuiescent:
        return {M1ReplayRunState::Terminal, M1ReplayRunState::Quiescent};
      case Kind::ResourceSettled:
        return {M1ReplayRunState::Quiescent, M1ReplayRunState::ResourceSettled};
      case Kind::RunUnregistered:
        return {M1ReplayRunState::ResourceSettled,
                M1ReplayRunState::Unregistered};
      default:
        throw std::logic_error("M1 lifecycle replay received a non-Run kind");
    }
  }

  /**
   * @brief Applies one child transition with bundle-wide phase barriers.
   * @param event Exact child/bundle/Graph/group identity publication.
   * @return True only for terminal-to-quiescent-to-settled-to-unregistered.
   * @throws std::bad_alloc when the second realtime child is retained.
   */
  bool advance_run(const compute::ExecutionLifecycleEvent& event) {
    const auto bundle_found = bundles_.find(event.generation);
    if (bundle_found == bundles_.end()) {
      return reject("Run transition names no live or unregistering bundle");
    }
    M1ReplayBundle& bundle = bundle_found->second;
    if (event.graph_instance_id != bundle.graph_instance_id ||
        event.run_group_id != bundle.run_group_id) {
      return reject("Run transition crosses Graph or group identity");
    }

    auto run_found = std::find_if(bundle.runs.begin(), bundle.runs.end(),
                                  [&event](const M1ReplayRun& run) {
                                    return run.run_id == event.run_id;
                                  });
    if (run_found == bundle.runs.end()) {
      const bool learns_second_child =
          bundle.expected_run_count == 2U && bundle.runs.size() == 1U &&
          event.kind == compute::ExecutionLifecycleEventKind::RunTerminal &&
          pending_group_step_.has_value() &&
          pending_group_step_->bundle_id == bundle.bundle_id &&
          all_run_ids_.insert(event.run_id).second;
      if (!learns_second_child) {
        return reject(
            "Run transition uses an unknown or reused child identity");
      }
      bundle.runs.push_back(
          M1ReplayRun{event.run_id, M1ReplayRunState::Admitted});
      run_found = std::prev(bundle.runs.end());
    }

    const std::size_t run_index =
        static_cast<std::size_t>(std::distance(bundle.runs.begin(), run_found));
    const auto transition = run_transition(event.kind);
    if (run_found->state != transition.first) {
      return reject(
          "Run settlement phase is duplicated, skipped, or reordered");
    }
    if (bundle.expected_run_count == 2U) {
      if (run_index == 0U) {
        if (bundle.runs.size() != 2U &&
            event.kind != compute::ExecutionLifecycleEventKind::RunTerminal) {
          return reject("realtime second child is absent before later phases");
        }
        if (event.kind != compute::ExecutionLifecycleEventKind::RunTerminal &&
            bundle.runs[1U].state != transition.first) {
          return reject("realtime phase began before both children arrived");
        }
      } else if (run_index != 1U ||
                 bundle.runs[0U].state != transition.second) {
        return reject("realtime children are not in producer order");
      }
    }

    run_found->state = transition.second;
    if (event.kind == compute::ExecutionLifecycleEventKind::RunUnregistered &&
        run_index == 0U) {
      bundle.detached = true;
    }
    if (bundle.expected_run_count == 2U) {
      if (run_index == 0U) {
        pending_group_step_ = M1ReplayGroupStep{bundle.bundle_id, event.kind};
      } else {
        pending_group_step_.reset();
      }
    }

    const bool complete = std::all_of(
        bundle.runs.begin(), bundle.runs.end(), [](const M1ReplayRun& run) {
          return run.state == M1ReplayRunState::Unregistered;
        });
    if (complete) {
      bundles_.erase(bundle_found);
    }
    return true;
  }

  /**
   * @brief Applies registration rollback or empty Closing-row removal.
   * @param event Exact GraphRowRemoved publication.
   * @return True only after candidate/bundle settlement and valid generation.
   * @throws std::bad_alloc when diagnostics allocate.
   */
  bool remove_graph(const compute::ExecutionLifecycleEvent& event) {
    const auto found = graphs_.find(event.graph_instance_id);
    if (found == graphs_.end() || pending_candidates(found->second) != 0U) {
      return reject("GraphRowRemoved names no empty live row");
    }
    const bool has_bundle = std::any_of(
        bundles_.begin(), bundles_.end(), [&event](const auto& entry) {
          return entry.second.graph_instance_id == event.graph_instance_id;
        });
    if (has_bundle) {
      return reject("GraphRowRemoved precedes complete bundle unregistration");
    }
    if (found->second.state == M1ReplayGraphState::Open) {
      if (event.generation != 0U || !found->second.candidate_ids.empty()) {
        return reject("Graph registration rollback is not pristine");
      }
    } else if (event.generation != found->second.close_generation) {
      return reject("GraphRowRemoved close generation drifted");
    }
    graphs_.erase(found);
    return true;
  }

  /**
   * @brief Applies the final service event after complete logical settlement.
   * @param event Exact ServiceStopped record.
   * @return True only for matching generation and empty replay ownership.
   * @throws std::bad_alloc when diagnostics allocate.
   */
  bool stop_service(const compute::ExecutionLifecycleEvent& event) {
    if (!shutdown_started_ || event.generation != shutdown_generation_ ||
        !graphs_.empty() || !bundles_.empty() ||
        !pending_cancellations_.empty() || pending_group_step_.has_value() ||
        shutdown_graph_closing_index_ != shutdown_graph_closing_order_.size()) {
      return reject("ServiceStopped precedes complete lifecycle settlement");
    }
    service_stopped_ = true;
    return true;
  }

  /** @brief Stable mutable diagnostic target supplied by the evaluator. */
  std::vector<std::string>* reasons_ = nullptr;
  /** @brief True after the unique sequence-one service origin. */
  bool service_started_ = false;
  /** @brief True after the no-event Accepting-to-Stopping transition. */
  bool shutdown_started_ = false;
  /** @brief True after the unique final ServiceStopped record. */
  bool service_stopped_ = false;
  /** @brief Stable process-shutdown generation after Stopping. */
  std::uint64_t shutdown_generation_ = 0U;
  /** @brief Live Graph rows keyed by exact GraphInstanceId scalar. */
  std::map<std::uint64_t, M1ReplayGraph> graphs_;
  /** @brief Process-nonreused Graph identities observed in this service. */
  std::set<std::uint64_t> all_graph_ids_;
  /** @brief Live-row insertion order used by shutdown enumeration. */
  std::vector<std::uint64_t> graph_registration_order_;
  /** @brief Process-nonreused candidate identities observed in this service. */
  std::set<std::uint64_t> all_candidate_ids_;
  /** @brief Live or mid-unregistration bundles keyed by bundle identity. */
  std::map<std::uint64_t, M1ReplayBundle> bundles_;
  /** @brief Process-nonreused bundle identities observed in this service. */
  std::set<std::uint64_t> all_bundle_ids_;
  /** @brief Process-nonreused child Run identities observed in this service. */
  std::set<std::uint64_t> all_run_ids_;
  /** @brief Process-nonreused realtime group identities in this service. */
  std::set<std::uint64_t> all_run_group_ids_;
  /** @brief Captured cancellation publications remaining by correlation. */
  std::map<M1CancelKey, std::uint64_t> pending_cancellations_;
  /** @brief Required second child transition in one fence-held group phase. */
  std::optional<M1ReplayGroupStep> pending_group_step_;
  /** @brief Exact row order emitted by begin_service_shutdown(). */
  std::vector<std::uint64_t> shutdown_graph_closing_order_;
  /** @brief Next required row in the shutdown GraphClosing batch. */
  std::size_t shutdown_graph_closing_index_ = 0U;
};

/**
 * @brief Replays every retained lifecycle page as one lossless cursor chain.
 *
 * The replay requires exact chronological capture ordinals, preserves each
 * request cursor, proves every sequence from cursor+1 through the atomic cut,
 * checks closed enums/identities/timestamps, and requires the lifecycle effects
 * necessarily produced by a nonempty mixed workload.
 *
 * @param snapshots Chronological M1 execution snapshots starting at cursor 0.
 * @param require_workload_effects True when retained protocol work exists.
 * @param reasons Mutable row diagnostics receiving stable invalidations.
 * @return True only for one complete continuous same-service history.
 * @throws std::bad_alloc when event-kind tracking or diagnostics allocate.
 */
bool validate_m1_lifecycle_history(
    const std::vector<M1ExecutionSnapshot>& snapshots,
    bool require_workload_effects, std::vector<std::string>* reasons) {
  bool valid = true;
  const auto invalid = [&valid, reasons](std::string reason) {
    valid = false;
    invalidate_m1(reasons, "M1 memory evidence: lifecycle " + reason);
  };
  if (snapshots.empty()) {
    invalid("history is empty");
    return false;
  }

  const std::uint64_t service_id =
      snapshots.front().lifecycle.service_instance_id;
  const std::uint64_t telemetry_epoch =
      snapshots.front().lifecycle.telemetry_epoch;
  std::uint64_t expected_cursor = 0U;
  std::uint64_t previous_timestamp_us = 0U;
  std::uint64_t shutdown_generation = 0U;
  compute::ExecutionLifecycleServiceState previous_state =
      compute::ExecutionLifecycleServiceState::Accepting;
  std::set<compute::ExecutionLifecycleEventKind> observed_kinds;
  bool observed_any_event = false;
  bool observed_service_stopped = false;
  M1LifecycleReplay replay(reasons);

  for (std::size_t index = 0U; index < snapshots.size(); ++index) {
    const M1ExecutionSnapshot& snapshot = snapshots[index];
    const compute::ExecutionLifecyclePage& page = snapshot.lifecycle;
    if (snapshot.temporal_capture_ordinal != index) {
      invalid("capture ordinal is missing, duplicated, or reordered");
    }
    if (snapshot.lifecycle_after_cursor != expected_cursor) {
      invalid("request cursor does not continue the prior page cut");
    }
    if (page.schema_version !=
            compute::kExecutionLifecycleTelemetrySchemaVersion ||
        page.capacity != compute::kExecutionLifecycleTelemetryCapacity ||
        service_id == 0U || page.service_instance_id != service_id ||
        telemetry_epoch == 0U || page.telemetry_epoch != telemetry_epoch ||
        !known_service_state(page.service_state) ||
        page.global_dropped_total != 0U || page.global_dropped_saturated ||
        page.cursor_gap != 0U || page.has_more) {
      invalid("schema, identity, enum, or losslessness drifted");
    }

    if (known_service_state(page.service_state) &&
        static_cast<std::uint16_t>(page.service_state) <
            static_cast<std::uint16_t>(previous_state)) {
      invalid("service state moved backwards");
    }
    previous_state = page.service_state;
    if (page.service_state ==
        compute::ExecutionLifecycleServiceState::Accepting) {
      if (page.shutdown_generation != 0U) {
        invalid("Accepting page carries a shutdown generation");
      }
    } else {
      if (page.shutdown_generation == 0U) {
        invalid("Stopping or Stopped page lacks a shutdown generation");
      } else if (shutdown_generation == 0U) {
        shutdown_generation = page.shutdown_generation;
      } else if (shutdown_generation != page.shutdown_generation) {
        invalid("shutdown generation changed between pages");
      }
    }

    if (page.snapshot_cut == 0U) {
      if (snapshot.lifecycle_after_cursor != 0U ||
          page.first_retained_sequence != 0U || page.next_sequence != 1U ||
          !page.records.empty() || page.next_cursor != 0U ||
          page.service_state !=
              compute::ExecutionLifecycleServiceState::Accepting) {
        invalid("empty-ring page contradicts the producer contract");
      }
      expected_cursor = 0U;
      continue;
    }

    if (page.first_retained_sequence != 1U) {
      invalid("lossless nonempty ring does not retain sequence one");
    }
    if (snapshot.lifecycle_after_cursor > page.snapshot_cut ||
        page.next_cursor != page.snapshot_cut) {
      invalid("request cursor, next cursor, and atomic cut are inconsistent");
    }
    std::uint64_t expected_record_count = 0U;
    if (snapshot.lifecycle_after_cursor <= page.snapshot_cut) {
      expected_record_count =
          page.snapshot_cut - snapshot.lifecycle_after_cursor;
    }
    if (expected_record_count >
            compute::kExecutionLifecycleTelemetryMaxPageSize ||
        page.records.size() != expected_record_count) {
      invalid("page omits, duplicates, or truncates sequenced records");
    }

    for (std::size_t record_index = 0U; record_index < page.records.size();
         ++record_index) {
      const compute::ExecutionLifecycleEvent& event =
          page.records[record_index];
      const std::uint64_t expected_sequence =
          snapshot.lifecycle_after_cursor + record_index + 1U;
      if (event.schema_version !=
              compute::kExecutionLifecycleTelemetrySchemaVersion ||
          event.sequence != expected_sequence ||
          event.service_instance_id != service_id ||
          event.telemetry_epoch != telemetry_epoch ||
          event.timestamp_saturated ||
          !known_lifecycle_event_kind(event.kind) ||
          !known_lifecycle_category(event.category) ||
          !valid_lifecycle_event_identity(event)) {
        invalid(
            "record schema, sequence, identity shape, timestamp, or enum "
            "drifted");
      }
      if (observed_any_event && event.timestamp_us < previous_timestamp_us) {
        invalid("record timestamps moved backwards");
      }
      previous_timestamp_us = event.timestamp_us;
      observed_any_event = true;
      observed_kinds.insert(event.kind);

      const bool shutdown_event =
          (event.kind == compute::ExecutionLifecycleEventKind::GraphClosing &&
           event.category ==
               compute::ExecutionLifecycleCategory::ProcessShutdown) ||
          event.kind == compute::ExecutionLifecycleEventKind::WorkerJoined ||
          event.kind == compute::ExecutionLifecycleEventKind::ServiceStopped;
      if (!replay.shutdown_started() &&
          page.service_state !=
              compute::ExecutionLifecycleServiceState::Accepting &&
          shutdown_event && !replay.begin_shutdown(page.shutdown_generation)) {
        valid = false;
      }
      if (!replay.apply(event)) {
        valid = false;
      }

      if (event.sequence == 1U &&
          (event.kind != compute::ExecutionLifecycleEventKind::ServiceStarted ||
           event.category != compute::ExecutionLifecycleCategory::None ||
           event.graph_instance_id != 0U || event.run_id != 0U ||
           event.run_group_id != 0U || event.generation != 0U)) {
        invalid("sequence one is not the canonical ServiceStarted event");
      }
      if (event.kind == compute::ExecutionLifecycleEventKind::ServiceStarted) {
        if (event.sequence != 1U ||
            event.category != compute::ExecutionLifecycleCategory::None) {
          invalid("ServiceStarted is duplicated or malformed");
        }
      }
      if (event.kind == compute::ExecutionLifecycleEventKind::ServiceStopped) {
        observed_service_stopped = true;
        if (page.service_state !=
                compute::ExecutionLifecycleServiceState::Stopped ||
            event.category != compute::ExecutionLifecycleCategory::None ||
            event.graph_instance_id != 0U || event.run_id != 0U ||
            event.run_group_id != 0U || event.generation == 0U ||
            event.generation != page.shutdown_generation) {
          invalid("ServiceStopped event is malformed or precedes Stopped");
        }
      } else if (observed_service_stopped) {
        invalid("ordinary event appears after ServiceStopped");
      }
    }

    if (!replay.shutdown_started() &&
        page.service_state !=
            compute::ExecutionLifecycleServiceState::Accepting &&
        !replay.begin_shutdown(page.shutdown_generation)) {
      valid = false;
    }
    if (!replay.validate_page(page)) {
      valid = false;
    }

    if (page.service_state ==
        compute::ExecutionLifecycleServiceState::Stopped) {
      if (page.next_sequence != std::numeric_limits<std::uint64_t>::max() ||
          !observed_service_stopped) {
        invalid("Stopped page lacks the final event or exhausted sentinel");
      }
    } else if (page.snapshot_cut == std::numeric_limits<std::uint64_t>::max() ||
               page.next_sequence != page.snapshot_cut + 1U) {
      invalid("next sequence does not immediately follow the atomic cut");
    }
    expected_cursor = page.snapshot_cut;
  }

  if (observed_kinds.count(
          compute::ExecutionLifecycleEventKind::ServiceStarted) != 1U) {
    invalid("history lacks the unique ServiceStarted origin");
  }
  if (require_workload_effects) {
    for (const compute::ExecutionLifecycleEventKind required :
         {compute::ExecutionLifecycleEventKind::GraphRegistered,
          compute::ExecutionLifecycleEventKind::CandidateBegan,
          compute::ExecutionLifecycleEventKind::BundleAdmitted,
          compute::ExecutionLifecycleEventKind::RunTerminal,
          compute::ExecutionLifecycleEventKind::RunQuiescent,
          compute::ExecutionLifecycleEventKind::ResourceSettled,
          compute::ExecutionLifecycleEventKind::RunUnregistered,
          compute::ExecutionLifecycleEventKind::GraphClosing,
          compute::ExecutionLifecycleEventKind::GraphRowRemoved}) {
      if (observed_kinds.count(required) == 0U) {
        invalid("history lacks one or more required mixed-workload effects");
        break;
      }
    }
  }
  if (!replay.complete()) {
    valid = false;
  }
  return valid;
}

/**
 * @brief Validates temporal snapshots and event-derived Compute I/O evidence.
 * @param snapshots Chronological same-service samples including final cut.
 * @param offers Complete immutable B1 protocol offer set.
 * @param jobs Exact-one complete B1 evidence for every offer.
 * @param row Mutable result receiving high-water values and diagnostics.
 * @return Structural validity plus independently complete limit/settlement
 * outcomes.
 * @throws std::bad_alloc when diagnostics allocate.
 */
M1MemoryValidation validate_m1_memory(
    const std::vector<M1ExecutionSnapshot>& snapshots,
    const std::vector<M1BatchOfferEvidence>& offers,
    const std::vector<B1JobEvidence>& jobs, M1InnerRow* row) {
  M1MemoryValidation result;
  const auto invalid = [&result, row](std::string reason) {
    result.valid = false;
    invalidate_m1(&row->validity_reasons, "M1 memory evidence: " + reason);
  };
  if (snapshots.size() < 4U) {
    invalid("fewer than four boundary/final snapshots");
    return result;
  }

  std::map<B1JobInstance, const B1JobEvidence*> indexed_jobs;
  for (const B1JobEvidence& job : jobs) {
    if (!indexed_jobs.emplace(job.job, &job).second) {
      invalid("multiple B1 I/O streams claim the same occurrence");
    }
  }
  if (jobs.size() != offers.size() || indexed_jobs.size() != offers.size()) {
    invalid("B1 I/O stream cardinality does not match protocol offers");
  }
  std::set<std::uint64_t> accounting_sequences;
  for (const M1BatchOfferEvidence& offer : offers) {
    const auto found = indexed_jobs.find(offer.job);
    if (found == indexed_jobs.end()) {
      invalid("a protocol B1 offer lacks its complete I/O stream");
      continue;
    }
    const B1JobEvidence& job = *found->second;
    if (job.producer_offer_ordinal != offer.producer_offer_ordinal ||
        job.offered_at != offer.offered.timestamp ||
        !offer.endpoint.has_value() ||
        job.endpoint_at != offer.endpoint->timestamp) {
      invalid("B1 I/O stream identity or endpoint differs from its offer");
    }
    const B1ComputeIoEvaluation io = evaluate_b1_compute_io_evidence(job);
    if (!io.structurally_valid || !io.fault_free_complete) {
      invalid("B1 I/O stream is malformed or not fault-free complete");
      for (const std::string& reason : io.validity_reasons) {
        invalidate_m1(&row->validity_reasons, "M1 memory evidence: " + reason);
      }
    }
    row->compute_io_task_high_water =
        std::max(row->compute_io_task_high_water, io.task_high_water);
    row->compute_io_planned_byte_high_water = std::max(
        row->compute_io_planned_byte_high_water, io.planned_byte_high_water);
    for (const B1ComputeIoObservation& observation :
         job.output.io_observations) {
      std::optional<std::uint64_t> sequence;
      if (observation.point == B1IoObservationPoint::AcceptedAdmission ||
          observation.point == B1IoObservationPoint::OfferRejected) {
        if (observation.admission_event.has_value()) {
          sequence = observation.admission_event->sequence;
        }
      } else if (observation.point == B1IoObservationPoint::Settlement &&
                 observation.settlement_event.has_value()) {
        sequence = observation.settlement_event->sequence;
      }
      if (sequence.has_value() &&
          !accounting_sequences.insert(*sequence).second) {
        invalid("Compute I/O accounting sequence is duplicated across jobs");
      }
    }
  }

  if (!validate_m1_lifecycle_history(snapshots, !jobs.empty(),
                                     &row->validity_reasons)) {
    result.valid = false;
  }
  ResourceVector prior_high_water = snapshots.front().host_resources.high_water;
  const std::vector<ResourceLedger::DeviceSnapshot>& initial_devices =
      snapshots.front().device_resources;

  for (const M1ExecutionSnapshot& snapshot : snapshots) {
    if (snapshot.host_resources.limits != kM1HostLimits ||
        snapshot.throughput.capacity != kM1ThroughputCapacity) {
      invalid("Host limits or Throughput headroom capacity drifted");
    }
    if (!resources_fit(snapshot.host_resources.reserved,
                       snapshot.host_resources.limits) ||
        !resource_high_water_nondecreasing(
            prior_high_water, snapshot.host_resources.high_water)) {
      invalid("Host reservation or lifetime high-water is contradictory");
    }
    if (!resources_fit(snapshot.host_resources.high_water,
                       snapshot.host_resources.limits)) {
      result.within_limits = false;
    }
    prior_high_water = snapshot.host_resources.high_water;
    if (!resources_fit(snapshot.throughput.reserved,
                       snapshot.throughput.capacity)) {
      invalid("Throughput reservation exceeds general capacity");
    }
    if (!snapshot.ready_classes.valid ||
        snapshot.ready_classes.interactive_entries >
            std::numeric_limits<std::uint64_t>::max() -
                snapshot.ready_classes.throughput_entries ||
        snapshot.ready_classes.interactive_entries +
                snapshot.ready_classes.throughput_entries !=
            snapshot.ready_classes.total_entries) {
      invalid("ready-store class partition is malformed");
    }
    const execution::ComputeIoExecutorSnapshot& io = snapshot.compute_io;
    if (io.task_limit != kB1ComputeIoTaskLimit ||
        io.planned_bytes_limit != kB1ComputeIoPlannedByteLimit ||
        io.constructing_tasks >
            std::numeric_limits<std::uint64_t>::max() - io.queued_tasks ||
        io.constructing_tasks + io.queued_tasks >
            std::numeric_limits<std::uint64_t>::max() - io.running_tasks ||
        io.constructing_tasks + io.queued_tasks + io.running_tasks !=
            io.active_tasks ||
        io.active_tasks > io.task_limit ||
        io.active_planned_bytes > io.planned_bytes_limit) {
      invalid("Compute I/O limits, phase partition, or active state drifted");
    }
    if (snapshot.device_resources.size() != initial_devices.size()) {
      invalid("configured device inventory cardinality changed");
    } else {
      for (std::size_t index = 0U; index < initial_devices.size(); ++index) {
        const ResourceLedger::DeviceSnapshot& initial = initial_devices[index];
        const ResourceLedger::DeviceSnapshot& current =
            snapshot.device_resources[index];
        if (current.device != initial.device ||
            current.limits != initial.limits ||
            current.available.device_memory_bytes >
                current.limits.device_memory_bytes ||
            current.available.device_scratch_bytes >
                current.limits.device_scratch_bytes ||
            current.available.device_memory_bytes +
                    current.reserved.device_memory_bytes !=
                current.limits.device_memory_bytes ||
            current.available.device_scratch_bytes +
                    current.reserved.device_scratch_bytes !=
                current.limits.device_scratch_bytes) {
          invalid("device identity, limits, reservation, or available drifted");
        }
        if (!device_resources_fit(current.reserved, current.limits) ||
            !device_resources_fit(current.high_water, current.limits)) {
          result.within_limits = false;
        }
        if (current.device.backend() == DeviceBackend::Metal &&
            current.limits != kM1MetalLimits) {
          invalid("configured Metal limits differ from the frozen profile");
        }
      }
    }
  }

  const M1ExecutionSnapshot& initial = snapshots.front();
  if (initial.compute_io.active_tasks != 0U ||
      initial.compute_io.active_planned_bytes != 0U ||
      initial.compute_io.constructing_tasks != 0U ||
      initial.compute_io.queued_tasks != 0U ||
      initial.compute_io.running_tasks != 0U) {
    invalid("initial Compute I/O boundary is not zero");
  }
  const M1ExecutionSnapshot& final = snapshots.back();
  if (!zero_resources(final.host_resources.reserved) ||
      !zero_resources(final.throughput.reserved) ||
      final.ready_classes.total_entries != 0U ||
      final.compute_io.active_tasks != 0U ||
      final.compute_io.active_planned_bytes != 0U ||
      final.compute_io.constructing_tasks != 0U ||
      final.compute_io.queued_tasks != 0U ||
      final.compute_io.running_tasks != 0U ||
      !lifecycle_settled(final.lifecycle.counters)) {
    result.settled = false;
    invalid("final Host, ready, I/O, or lifecycle ownership is not zero");
  }
  for (const ResourceLedger::DeviceSnapshot& device : final.device_resources) {
    if (device.reserved != DeviceResourceVector{}) {
      result.settled = false;
      invalid("final device ownership is not zero");
    }
  }
  return result;
}

}  // namespace

/** @copydoc evaluate_m1_inner_row */
M1InnerRow evaluate_m1_inner_row(M1InnerRowInput input) {
  M1InnerRow row;
  row.evidence = std::move(input);
  row.protocol = evaluate_m1_protocol(row.evidence.protocol);
  row.validity_reasons = row.protocol.validity_reasons;

  if (row.evidence.replicate_ordinal == 0U ||
      row.evidence.replicate_ordinal > 3U ||
      row.evidence.protocol.replicate_ordinal !=
          row.evidence.replicate_ordinal) {
    invalidate_m1(&row.validity_reasons,
                  "M1 inner row replicate identity drifted");
  }
  const bool protocol_valid =
      row.protocol.verdict == I1Verdict::Pass &&
      row.evidence.replicate_ordinal != 0U &&
      row.evidence.replicate_ordinal <= 3U &&
      row.evidence.protocol.replicate_ordinal == row.evidence.replicate_ordinal;
  if (!protocol_valid) {
    return row;
  }

  std::vector<std::chrono::nanoseconds> latency_samples;
  bool every_latency_complete = true;
  bool every_output_passed = true;
  bool every_interactive_waste_complete = true;
  bool interactive_sum_valid = true;
  for (const M1InteractiveOccurrenceEvidence& occurrence :
       row.evidence.protocol.interactive_occurrences) {
    if (occurrence.phase != B1JobPhase::Measured) {
      continue;
    }
    if (!occurrence.final_latency.has_value()) {
      every_latency_complete = false;
    } else {
      latency_samples.push_back(*occurrence.final_latency);
    }
    every_latency_complete = every_latency_complete &&
                             occurrence.latency_verdict != I1Verdict::Invalid;
    every_output_passed =
        every_output_passed && occurrence.output_verdict == I1Verdict::Pass;
    every_interactive_waste_complete =
        every_interactive_waste_complete &&
        occurrence.waste_verdict != I1Verdict::Invalid;
    interactive_sum_valid =
        interactive_sum_valid &&
        checked_accumulate(&row.interactive_all_started_service,
                           occurrence.service.all_started_service) &&
        checked_accumulate(&row.interactive_discarded_started_service,
                           occurrence.service.discarded_started_service) &&
        checked_accumulate(&row.interactive_post_cancellation_started_service,
                           occurrence.service.post_cancel_started_service);
  }

  bool latency_valid = every_latency_complete &&
                       latency_samples.size() == kM1MeasuredI1OriginCount &&
                       row.evidence.paired_isolated_i1_p99.has_value() &&
                       row.evidence.paired_isolated_i1_p99->count() > 0 &&
                       row.evidence.occurrence_attribution_proved;
  if (latency_valid) {
    row.latency =
        I1LatencyPercentiles{i1_nearest_rank(latency_samples, 50U, 100U),
                             i1_nearest_rank(latency_samples, 95U, 100U),
                             i1_nearest_rank(latency_samples, 99U, 100U)};
    row.relative_latency_p99 =
        static_cast<double>(row.latency->p99.count()) /
        static_cast<double>(row.evidence.paired_isolated_i1_p99->count());
    const bool every_episode_passed =
        std::all_of(row.evidence.protocol.interactive_occurrences.begin(),
                    row.evidence.protocol.interactive_occurrences.end(),
                    [](const M1InteractiveOccurrenceEvidence& occurrence) {
                      return occurrence.phase != B1JobPhase::Measured ||
                             occurrence.latency_verdict == I1Verdict::Pass;
                    });
    row.latency_verdict =
        every_episode_passed && every_output_passed &&
                row.latency->p50 <= kI1LatencyP50Limit &&
                row.latency->p95 <= kI1LatencyP95Limit &&
                row.latency->p99 <= kI1LatencyP99Limit &&
                std::isfinite(*row.relative_latency_p99) &&
                *row.relative_latency_p99 <= kM1RelativeLatencyP99Limit
            ? I1Verdict::Pass
            : I1Verdict::Fail;
  } else {
    invalidate_m1(&row.validity_reasons,
                  "M1 latency evidence or paired isolated p99 is incomplete");
  }

  M1FairnessEvidenceInput fairness_input = row.evidence.fairness;
  fairness_input.interactive_latency_verdict = row.latency_verdict;
  row.fairness = evaluate_m1_fairness(std::move(fairness_input));
  row.throughput_progress_verdict = row.fairness.throughput_progress_verdict;
  row.fairness_verdict = row.fairness.composite_fairness_verdict;
  for (const std::string& reason : row.fairness.validity_reasons) {
    invalidate_m1(&row.validity_reasons, "M1 fairness evidence: " + reason);
  }
  if (!row.evidence.occurrence_attribution_proved) {
    row.throughput_progress_verdict = I1Verdict::Invalid;
    row.fairness_verdict = I1Verdict::Invalid;
    invalidate_m1(&row.validity_reasons,
                  "M1 occurrence-owned phase attribution is unproved");
  }
  if (!row.evidence.temporal_effects_complete) {
    row.fairness_verdict = I1Verdict::Invalid;
    invalidate_m1(&row.validity_reasons,
                  "M1 measured-window carryover physical effects are missing");
  }

  const M1BatchWasteEvidence& batch = row.evidence.batch_waste;
  bool waste_valid =
      every_interactive_waste_complete && interactive_sum_valid &&
      row.evidence.occurrence_attribution_proved &&
      row.interactive_all_started_service > 0U &&
      row.interactive_discarded_started_service <=
          row.interactive_all_started_service &&
      batch.all_started_service > 0U &&
      batch.discarded_started_service <= batch.all_started_service;
  if (waste_valid) {
    row.interactive_discarded_ratio =
        static_cast<double>(row.interactive_discarded_started_service) /
        static_cast<double>(row.interactive_all_started_service);
    const bool each_interactive_passed =
        std::all_of(row.evidence.protocol.interactive_occurrences.begin(),
                    row.evidence.protocol.interactive_occurrences.end(),
                    [](const M1InteractiveOccurrenceEvidence& occurrence) {
                      return occurrence.phase != B1JobPhase::Measured ||
                             occurrence.waste_verdict == I1Verdict::Pass;
                    });
    row.waste_verdict =
        each_interactive_passed &&
                *row.interactive_discarded_ratio <=
                    kI1DiscardedServiceRatioLimit &&
                row.interactive_post_cancellation_started_service == 0U &&
                batch.discarded_started_service == 0U &&
                batch.post_cancellation_started_service == 0U &&
                batch.duplicate_service_starts == 0U &&
                batch.retry_service_starts == 0U
            ? I1Verdict::Pass
            : I1Verdict::Fail;
  } else {
    invalidate_m1(&row.validity_reasons,
                  "M1 measured Interactive/B1 waste evidence is incomplete");
  }

  const M1MemoryValidation memory = validate_m1_memory(
      row.evidence.temporal_snapshots, row.evidence.protocol.batch_offers,
      row.evidence.batch_jobs, &row);
  if (memory.valid && row.evidence.temporal_effects_complete) {
    row.memory_verdict = memory.within_limits && memory.settled
                             ? I1Verdict::Pass
                             : I1Verdict::Fail;
  } else if (!row.evidence.temporal_effects_complete) {
    invalidate_m1(&row.validity_reasons,
                  "M1 temporal resource evidence is incomplete");
  }

  row.overall_verdict = compose_m1_row(
      {row.latency_verdict, row.throughput_progress_verdict,
       row.fairness_verdict, row.waste_verdict, row.memory_verdict},
      &row.validity_reasons);
  return row;
}

}  // namespace ps::benchmark
