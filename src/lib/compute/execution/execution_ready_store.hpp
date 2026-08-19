#pragma once

/**
 * @file execution_ready_store.hpp
 * @brief Private policy-aware bounded ready-store implementation.
 *
 * The store owns ready-entry indexing, policy selection, and bounded staged
 * publication. It is source-private and does not extend the installed ABI.
 */

#include <algorithm>
#include <limits>
#include <list>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "compute/execution/execution_service_run_state.hpp"

namespace ps::compute {

/**
 * @brief Owns one policy-aware entry/byte-bounded service ready store.
 *
 * @throws std::bad_alloc when entry or policy-row storage grows.
 * @note Every method is called with `PoolState::mutex` held. The store owns
 * ready values and ordering history but no synchronization, Run admission,
 * dependency, resource-token, worker, completion, or lifecycle authority.
 */
class ExecutionService::BoundedReadyStore final {
 private:
  struct PolicyGraphState;
  struct PolicyRunState;

 public:
  /**
   * @brief Immutable ABI snapshot prepared while the ready store is locked.
   *
   * @throws std::bad_alloc when candidate storage cannot allocate.
   * @note The value contains copied scalar descriptors only. It retains no
   * ready entry, Run, grant, route, or mutation authority across a callback.
   */
  struct PolicySnapshot final {
    /** @brief Host-selected QoS class. */
    ComputeRunQosClass service_class = ComputeRunQosClass::Throughput;

    /** @brief Nonzero generation unique to this immutable snapshot. */
    std::uint64_t snapshot_generation = 0U;

    /** @brief Nonzero sequence unique to this callback attempt. */
    std::uint64_t selection_sequence = 0U;

    /** @brief Exact plugin-visible admissible frontier. */
    std::vector<ps_policy_candidate_v1> candidates;

    /** @brief Whether the bounded ABI callback may consume this value. */
    bool plugin_eligible = false;
  };

  /**
   * @brief Pinned current entry plus Host-authored commit values.
   *
   * @throws Nothing for move/copy operations after the shared pin exists.
   * @note `entry` keeps a purged object alive but does not keep it visible;
   * commit revalidates exact identity and store ownership under the lock.
   */
  struct SelectionPin final {
    /** @brief Exact selected object, or null when no candidate is selectable.
     */
    std::shared_ptr<QueueEntry> entry;

    /** @brief Selected class after current Host arbitration. */
    ComputeRunQosClass service_class = ComputeRunQosClass::Throughput;

    /** @brief Projected Graph charge committed only on successful start. */
    std::uint64_t graph_score = 0U;

    /** @brief Projected normalized Run charge committed only on start. */
    std::uint64_t run_score = 0U;

    /** @brief Captured nonreused candidate identity. */
    std::uint64_t candidate_id = 0U;

    /** @brief Captured nonreused entry version. */
    std::uint64_t entry_version = 0U;

    /** @brief Captured stable enqueue sequence. */
    std::uint64_t enqueue_sequence = 0U;

    /** @brief Captured immutable private route generation. */
    std::uint64_t route_generation = 0U;
  };

  /** @brief Outcome of one allocation-free reserved-start transaction. */
  enum class StartResult {
    /** @brief Entry/grants/fairness/in-flight ownership committed atomically.
     */
    Started,
    /** @brief The pin no longer denotes a current admissible entry. */
    Obsolete,
    /** @brief The admitted root could not mint the execution child grant. */
    GrantUnavailable,
    /** @brief A nonreused counter reached its terminal value. */
    IdentityExhausted,
  };

  /**
   * @brief Fixes aggregate ready-store limits for the service lifetime.
   * @param entry_limit Maximum stored entries across both service classes.
   * @param byte_limit Maximum accounted bytes across both service classes.
   * @param telemetry Stable physical-counter owner outliving this store.
   * @param operation_gate Stable execution-domain operation start gate.
   * @throws Nothing.
   */
  BoundedReadyStore(std::uint64_t entry_limit, std::uint64_t byte_limit,
                    ExecutionLifecycleTelemetry& telemetry,
                    OperationStartGate& operation_gate) noexcept
      : entry_limit_(entry_limit),
        byte_limit_(byte_limit),
        telemetry_(telemetry),
        operation_gate_(operation_gate) {}

  /**
   * @brief Returns conservative per-Run policy-map structural ownership.
   * @return One Run row, one conservatively charged Graph row, and linkage.
   * @throws GraphError when checked structural arithmetic overflows.
   * @note Graph rows are shared in storage but charged once per Run so
   * admission never depends on another Run retaining the allocation.
   */
  static std::uint64_t run_policy_envelope_bytes() {
    RetainedMemoryEstimator estimate("ExecutionService policy Run envelope");
    estimate.add_objects<std::pair<const std::uint64_t, PolicyRunState>>();
    estimate.add_objects<void*>(3U);
    estimate.add_objects<std::pair<const std::string, PolicyGraphState>>();
    estimate.add_objects<void*>(3U);
    return estimate.bytes();
  }

  /**
   * @brief Returns the stateless strategy for one explicit QoS class.
   * @param service_class Explicit immutable Run class.
   * @return Borrowed strategy owned for the store lifetime.
   * @throws Nothing; an invalid enum terminates as a trusted invariant breach.
   */
  const BuiltinPolicy& policy_for(
      ComputeRunQosClass service_class) const noexcept {
    switch (service_class) {
      case ComputeRunQosClass::Interactive:
        return interactive_policy_;
      case ComputeRunQosClass::Throughput:
        return throughput_policy_;
    }
    std::terminate();
  }

  /**
   * @brief Publishes one fully granted ready entry through policy accounting.
   * @param entry Owned entry with one active ready grant and checked cost.
   * @return True after publication, false without mutation on a local limit
   * or checked-counter/sequence violation.
   * @throws std::invalid_argument when identity, class, cost, or grant shape is
   * structurally invalid.
   * @throws std::bad_alloc when entry or policy-row ownership cannot allocate.
   * @note Successful publication assigns fresh nonreused identities and clears
   * every prior transient worker-cycle grant-block mark before visibility.
   */
  bool try_push(const std::shared_ptr<QueueEntry>& entry) {
    if (!entry) {
      throw std::invalid_argument(
          "Bounded ready store requires one owned entry.");
    }
    validate_entry(*entry);
    const ResourceVector charge = entry->ready_grant->resources();
    const std::optional<ResourceVector> next = checked_add_resources(
        ResourceVector{0U, 0U, 0U, entry_count_, byte_count_}, charge);
    if (!next.has_value() || next->ready_entries > entry_limit_ ||
        next->ready_bytes > byte_limit_ ||
        next_enqueue_sequence_ == std::numeric_limits<std::uint64_t>::max() ||
        next_candidate_id_ == std::numeric_limits<std::uint64_t>::max() ||
        next_entry_version_ == std::numeric_limits<std::uint64_t>::max()) {
      return false;
    }

    bool graph_inserted = false;
    auto graph_it = graph_states_.find(entry->run->graph);
    if (graph_it == graph_states_.end()) {
      if (next_graph_id_ == std::numeric_limits<std::uint64_t>::max()) {
        return false;
      }
      auto inserted = graph_states_.try_emplace(
          std::move(entry->run->available_graph_key), next_graph_id_ + 1U);
      graph_it = inserted.first;
      graph_inserted = inserted.second;
      if (!graph_inserted) {
        std::terminate();
      }
      ++next_graph_id_;
    }

    bool run_inserted = false;
    auto run_it = run_states_.end();
    try {
      run_it = run_states_.find(entry->run->id.value());
      if (run_it == run_states_.end()) {
        auto inserted = run_states_.try_emplace(
            entry->run->id.value(), entry->run.get(), &graph_it->second);
        run_it = inserted.first;
        run_inserted = inserted.second;
        if (!run_inserted) {
          std::terminate();
        }
        ++graph_it->second.active_runs;
      }
    } catch (...) {
      if (graph_inserted) {
        graph_states_.erase(graph_it);
      }
      throw;
    }

    PolicyRunState& run_state = run_it->second;
    if (run_state.run != entry->run.get() ||
        run_state.graph != &graph_it->second) {
      std::terminate();
    }

    try {
      entries_.push_back(entry);
    } catch (...) {
      if (run_inserted) {
        --graph_it->second.active_runs;
        run_states_.erase(run_it);
      }
      if (graph_inserted) {
        graph_states_.erase(graph_it);
      }
      throw;
    }

    entry->store_position = std::prev(entries_.end());
    entry->store_owned = true;
    entry->high_lane = entry->priority == ExecutionTaskPriority::High;
    entry->candidate_id = ++next_candidate_id_;
    entry->entry_version = ++next_entry_version_;
    entry->grant_blocked_worker_mask = 0U;
    entry->enqueued_class_dispatch_count =
        class_dispatch_count(entry->run->policy_qos.service_class);
    entry->enqueue_sequence = ++next_enqueue_sequence_;
    link_entry(run_state, *entry);
    entry_count_ = next->ready_entries;
    byte_count_ = next->ready_bytes;
    telemetry_.increment_physical_counter(
        ExecutionLifecyclePhysicalCounter::ReadyEntry);
    return true;
  }

  /**
   * @brief Builds one bounded immutable plugin snapshot from current state.
   * @param worker_id Worker attempting a start.
   * @param lane Fixed physical lane owned by the worker.
   * @param routes Host-owned route state used only for startability checks.
   * @return Current class/frontier and fresh callback generations; a frontier
   * larger than the ABI bound returns `plugin_eligible == false` and no copied
   * candidate array so the caller uses the full-state built-in path.
   * @throws std::bad_alloc when temporary or result storage cannot allocate.
   * @throws GraphError when a nonreused callback identity is exhausted.
   * @note Caller holds the service/store mutex. No pointer or authority enters
   * the returned ABI records.
   */
  PolicySnapshot make_policy_snapshot(
      int worker_id, execution::PhysicalExecutionLane lane,
      const execution::PhysicalExecutionRoutes& routes) {
    PolicySnapshot snapshot;
    const std::optional<ComputeRunQosClass> selected_class =
        choose_class(worker_id, lane, routes);
    if (!selected_class.has_value()) {
      return snapshot;
    }
    snapshot.service_class = *selected_class;
    std::vector<CandidateRecord> frontier =
        build_frontier(*selected_class, worker_id, lane, routes);
    if (frontier.empty()) {
      return snapshot;
    }
    if (frontier.size() > policy::kPolicyCandidateCountMax) {
      return snapshot;
    }
    if (next_snapshot_generation_ ==
            std::numeric_limits<std::uint64_t>::max() ||
        next_selection_sequence_ == std::numeric_limits<std::uint64_t>::max()) {
      throw GraphError(GraphErrc::ComputeError,
                       "ExecutionService policy identity exhausted.");
    }
    snapshot.candidates.reserve(frontier.size());
    for (const CandidateRecord& candidate : frontier) {
      snapshot.candidates.push_back(candidate.abi);
    }
    snapshot.snapshot_generation = ++next_snapshot_generation_;
    snapshot.selection_sequence = ++next_selection_sequence_;
    snapshot.plugin_eligible = true;
    return snapshot;
  }

  /**
   * @brief Recomputes current Host state and pins one returned candidate.
   * @param candidate_id Nonzero identity validated against the original call.
   * @param service_class Original Host-selected class.
   * @param worker_id Worker attempting the reserved start.
   * @param lane Fixed physical lane owned by the worker.
   * @param routes Host-owned route state used only for startability checks.
   * @return Exact current pin, or an empty value when Host state made the
   * decision obsolete.
   * @throws std::bad_alloc when recomputation storage cannot allocate.
   * @note Caller holds the service/store mutex. Revalidation does not advance
   * callback generations or mutate fairness, burst, ready, Run, or grants.
   */
  SelectionPin resolve_current(
      std::uint64_t candidate_id, ComputeRunQosClass service_class,
      int worker_id, execution::PhysicalExecutionLane lane,
      const execution::PhysicalExecutionRoutes& routes) {
    const std::optional<ComputeRunQosClass> current_class =
        choose_class(worker_id, lane, routes);
    if (!current_class.has_value() || *current_class != service_class) {
      return {};
    }
    std::vector<CandidateRecord> frontier =
        build_frontier(service_class, worker_id, lane, routes);
    const auto found =
        std::find_if(frontier.begin(), frontier.end(),
                     [candidate_id](const auto& candidate) {
                       return candidate.abi.candidate_id == candidate_id;
                     });
    return found == frontier.end() ? SelectionPin{} : pin_from(*found);
  }

  /**
   * @brief Chooses the deterministic built-in directly from full Host state.
   * @param worker_id Worker attempting a start.
   * @param lane Fixed physical lane owned by the worker.
   * @param routes Host-owned route state used only for startability checks.
   * @return Current exact pin or empty when no route is startable.
   * @throws Nothing.
   * @note This allocation-free path is used for sticky faults, oversized ABI
   * frontiers, and Host snapshot-allocation failure. The selected minimum is
   * identical to choosing from the reduced admissible frontier.
   */
  SelectionPin select_builtin_current(
      int worker_id, execution::PhysicalExecutionLane lane,
      const execution::PhysicalExecutionRoutes& routes) noexcept {
    const std::optional<ComputeRunQosClass> selected_class =
        choose_class(worker_id, lane, routes);
    if (!selected_class.has_value()) {
      return {};
    }
    const SelectedEntry selected =
        select_from_class(policy_for(*selected_class), worker_id, lane, routes);
    if (selected.entry == nullptr || selected.run_state == nullptr) {
      return {};
    }
    return pin_from(selected);
  }

  /**
   * @brief Commits one Host-owned reserved start without partial mutation.
   * @param pin Exact current object and projected fairness values.
   * @param worker_id Worker that will enter the callback after return.
   * @param lane Fixed physical lane owned by the calling worker.
   * @param routes Host-owned route state updated by the no-throw commit.
   * @return Started, obsolete, unavailable grant, or identity exhaustion.
   * @throws std::bad_alloc, std::logic_error, or std::overflow_error while
   * staging operation-gate ownership.
   * @throws std::system_error while staging the reservation child grant or
   * entering the Run-owned terminal arbiter.
   * @note Exceptional exits precede every ready/fairness/in-flight mutation.
   * After grant/gate staging, the Run terminal arbiter reserves an observation
   * coordinate and performs the irreversible route commit under one critical
   * section shared with cancellation acceptance. A rejected route explicitly
   * aborts the staged observation coordinate, publishes no callback, and rolls
   * back every staged owner.
   * Immediately before staging, scheduler-selectable Throughput competition
   * is recomputed from real ready entries, Run lifecycle, operation-gate, and
   * physical-route eligibility without treating transient child-grant
   * exhaustion as a policy filter. The separate evidence-startable class
   * facts add available child-grant capacity and are published only if this
   * exact selected start subsequently commits every owner and counter.
   * @note Caller holds `PoolState::mutex`; this method locks each probed
   * `RunState` separately and later holds the selected Run while staging its
   * child grant. `try_grant` acquires and releases the reservation mutex before
   * this method enters the Run terminal arbiter; no path holds reservation and
   * terminal-arbiter mutexes together.
   */
  StartResult commit_start(SelectionPin& pin, int worker_id,
                           execution::PhysicalExecutionLane lane,
                           execution::PhysicalExecutionRoutes& routes) {
    if (!pin.entry || !pin.entry->run) {
      return StartResult::Obsolete;
    }
    const bool throughput_selectable = has_scheduler_selectable(
        ComputeRunQosClass::Throughput, worker_id, lane, routes);
    const bool interactive_evidence_startable = has_evidence_startable(
        ComputeRunQosClass::Interactive, worker_id, lane, routes);
    const bool throughput_evidence_startable = has_evidence_startable(
        ComputeRunQosClass::Throughput, worker_id, lane, routes);
    const auto run_found = run_states_.find(pin.entry->run->id.value());
    if (run_found == run_states_.end()) {
      return StartResult::Obsolete;
    }
    PolicyRunState& run_state = run_found->second;
    if (run_state.run != pin.entry->run.get() || run_state.graph == nullptr ||
        !pin_matches(pin, run_state, worker_id, lane, routes)) {
      return StartResult::Obsolete;
    }

    std::lock_guard<std::mutex> run_lock(pin.entry->run->mutex);
    RunState& run = *pin.entry->run;
    if (!route_startable(run, *pin.entry, worker_id, lane, routes) ||
        run.route_generation != pin.route_generation ||
        class_dispatch_count(pin.service_class) ==
            std::numeric_limits<std::uint64_t>::max() ||
        run.committed_starts == std::numeric_limits<std::uint64_t>::max() ||
        (pin.service_class == ComputeRunQosClass::Interactive &&
         throughput_selectable &&
         consecutive_interactive_ ==
             std::numeric_limits<std::uint64_t>::max())) {
      return class_dispatch_count(pin.service_class) ==
                         std::numeric_limits<std::uint64_t>::max() ||
                     run.committed_starts ==
                         std::numeric_limits<std::uint64_t>::max()
                 ? StartResult::IdentityExhausted
                 : StartResult::Obsolete;
    }

    const ResourceVector execution_resources = task_execution_resources(run);
    if (!run.reservation.has_value()) {
      std::terminate();
    }
    std::optional<ResourceLedger::Grant> staged_grant =
        run.reservation->try_grant(execution_resources);
    if (!staged_grant.has_value()) {
      return StartResult::GrantUnavailable;
    }
#if defined(PHOTOSPIDER_INTERNAL_EXECUTION_SERVICE_TESTING)
    if (record_reserved_start_attempt_for_testing(
            pin.candidate_id, pin.entry_version, pin.route_generation,
            execution_resources)) {
      return StartResult::Obsolete;
    }
#endif
    if (!operation_gate_.try_start(
            pin.entry->submission.operation_constraints())) {
      return StartResult::Obsolete;
    }
    const DeviceBackend device = pin.entry->submission.metadata().device();
    /**
     * @brief Carries borrowed operands through synchronous Run arbitration.
     * @throws Nothing for aggregate initialization.
     * @note Every pointer remains valid until the local callback returns.
     */
    struct RouteCommitContext final {
      /** @brief Borrowed route inventory serialized by the service mutex. */
      execution::PhysicalExecutionRoutes* routes = nullptr;
      /** @brief Borrowed immutable route name retained by RunState. */
      const std::string* route = nullptr;
      /** @brief Immutable selected device for this callback. */
      DeviceBackend device = DeviceBackend::CPU;
    } route_context{&routes, &run.route, device};
#if defined(PHOTOSPIDER_INTERNAL_EXECUTION_SERVICE_TESTING)
    notify_service_start_arbitration_for_testing(
        testing::ServiceStartArbitrationPoint::BeforeRunArbitration);
#endif
    /**
     * @brief Commits one already-validated physical route irreversibly.
     * @param opaque_context Borrowed `RouteCommitContext` pointer.
     * @return True only when the route inventory accepts the start.
     * @throws Nothing; route commitment and the test checkpoint are no-throw.
     * @note The matching Run terminal arbiter is held for the complete call.
     */
    const auto commit_route = [](void* opaque_context) noexcept {
      auto* context = static_cast<RouteCommitContext*>(opaque_context);
#if defined(PHOTOSPIDER_INTERNAL_EXECUTION_SERVICE_TESTING)
      notify_service_start_arbitration_for_testing(
          testing::ServiceStartArbitrationPoint::BeforeRouteCommit);
      if (consume_route_commit_failure_for_testing()) {
        return false;
      }
#endif
      return context->routes->commit_start(*context->route, context->device);
    };
    bool route_committed = false;
    try {
      route_committed = pin.entry->submission.lease_.try_commit_service_start(
          commit_route, &route_context, &pin.entry->service_start_coordinate);
    } catch (...) {
      operation_gate_.finish(pin.entry->submission.operation_constraints());
      throw;
    }
    if (!route_committed) {
      operation_gate_.finish(pin.entry->submission.operation_constraints());
      return StartResult::Obsolete;
    }
    pin.entry->operation_gate_started = true;

    pin.entry->execution_grant.emplace(std::move(*staged_grant));
    remove_entry(*pin.entry, run_state);
    pin.entry->ready_grant.reset();
    run_state.charged_service = pin.run_score;
    run_state.graph->charged_service_for(pin.service_class) = pin.graph_score;
    ++class_dispatch_count(pin.service_class);
    ++run.committed_starts;
    ++run.in_flight;
    pin.entry->service_start_observation = ComputeRunServiceStartObservation{
        interactive_evidence_startable, throughput_evidence_startable, true};
    if (pin.service_class == ComputeRunQosClass::Interactive &&
        throughput_selectable) {
      ++consecutive_interactive_;
    } else {
      consecutive_interactive_ = 0U;
    }
    return StartResult::Started;
  }

  /**
   * @brief Tests whether one worker has scheduler-selectable work.
   * @param worker_id Worker attempting selection.
   * @param lane Fixed physical lane owned by the worker.
   * @param routes Host-owned route state used only for startability checks.
   * @return True when class arbitration can choose at least one entry without
   * considering transient execution child-grant capacity.
   * @throws Nothing.
   */
  bool has_scheduler_selectable_work(
      int worker_id, execution::PhysicalExecutionLane lane,
      const execution::PhysicalExecutionRoutes& routes) noexcept {
    return choose_class(worker_id, lane, routes).has_value();
  }

  /**
   * @brief Hides one exact grant-exhausted pin from one worker selection cycle.
   * @param pin Candidate/version/enqueue identity returned by current
   * selection.
   * @param worker_id CPU or GPU worker whose next selection must skip the pin.
   * @return True only when the identical store-owned entry was marked.
   * @throws Nothing.
   * @note Caller holds the service/store mutex. The mark is allocation-free,
   * preserves ready ownership and fairness state, and cannot transfer to a
   * replacement entry because all nonreused identities are revalidated.
   */
  bool mark_grant_blocked(const SelectionPin& pin, int worker_id) noexcept {
    if (!pin.entry || !pin.entry->store_owned ||
        pin.entry->candidate_id != pin.candidate_id ||
        pin.entry->entry_version != pin.entry_version ||
        pin.entry->enqueue_sequence != pin.enqueue_sequence ||
        *pin.entry->store_position != pin.entry) {
      return false;
    }
    pin.entry->grant_blocked_worker_mask |= worker_mask(worker_id);
    return true;
  }

  /**
   * @brief Restores every transiently blocked candidate for one worker.
   * @param worker_id CPU or GPU worker starting a fresh selection cycle.
   * @return Nothing.
   * @throws Nothing.
   * @note Caller holds the service/store mutex. Other workers' independent
   * block marks remain unchanged.
   */
  void clear_grant_blocked(int worker_id) noexcept {
    const std::uint16_t mask = worker_mask(worker_id);
    for (const std::shared_ptr<QueueEntry>& entry : entries_) {
      entry->grant_blocked_worker_mask &= static_cast<std::uint16_t>(~mask);
    }
  }

  /**
   * @brief Purges every queued entry belonging to one Run.
   * @param run Matching retained Run state.
   * @param retired_entries Empty caller-owned list receiving removed nodes.
   * @return Number of removed entries; the empty policy row remains active.
   * @throws Nothing; an accounting invariant violation terminates.
   * @note Nodes are spliced without allocation and MUST be destroyed only
   * after the caller releases the pool and Run locks. Their callbacks may
   * retire asynchronous fence-executor ownership that reacquires the Run.
   */
  std::size_t erase_run(
      const std::shared_ptr<RunState>& run,
      std::list<std::shared_ptr<QueueEntry>>& retired_entries) noexcept {
    if (!retired_entries.empty()) {
      std::terminate();
    }
    const auto found = run_states_.find(run->id.value());
    if (found == run_states_.end()) {
      return 0U;
    }
    PolicyRunState& state = found->second;
    if (state.run != run.get()) {
      std::terminate();
    }
    std::size_t removed = 0U;
    while (state.high.head != nullptr) {
      remove_entry(*state.high.head, state, &retired_entries);
      ++removed;
    }
    while (state.normal.head != nullptr) {
      remove_entry(*state.normal.head, state, &retired_entries);
      ++removed;
    }
    return removed;
  }

  /**
   * @brief Retires one settled Run's empty policy state.
   * @param run Matching settled Run removed from the active service index.
   * @return Nothing.
   * @throws Nothing; queued entries or identity mismatch terminate.
   * @note The shared Graph row is removed only after its last active Run.
   */
  void retire_run(const std::shared_ptr<RunState>& run) noexcept {
    const auto found = run_states_.find(run->id.value());
    if (found == run_states_.end()) {
      return;
    }
    PolicyRunState& state = found->second;
    if (state.run != run.get() || state.high.head != nullptr ||
        state.normal.head != nullptr || state.graph == nullptr ||
        state.graph->active_runs == 0U) {
      std::terminate();
    }
    PolicyGraphState* graph = state.graph;
    run_states_.erase(found);
    --graph->active_runs;
    if (graph->active_runs == 0U) {
      const auto graph_found = graph_states_.find(run->graph);
      if (graph_found == graph_states_.end() || &graph_found->second != graph) {
        std::terminate();
      }
      graph_states_.erase(graph_found);
    }
  }

  /**
   * @brief Tests exact physical policy-state ownership for one Run.
   * @param run Matching retained RunState.
   * @return True only when the policy row retains the same raw state.
   * @throws Nothing.
   * @note This is a physical ready-store predicate, not lifecycle admission or
   * close authority.
   */
  bool owns_run(const std::shared_ptr<RunState>& run) const noexcept {
    const auto found = run_states_.find(run->id.value());
    return found != run_states_.end() && found->second.run == run.get();
  }

  /**
   * @brief Reports whether any physical policy row uses one Run identity.
   * @param run_id Exact nonzero Run identity.
   * @return True while initial/dependent publication or settlement retains the
   * policy row.
   * @throws Nothing.
   * @note Used only to reject duplicate physical execution of one globally
   * unique Run; it does not replace RunLifecycleRegistry.
   */
  bool contains_run_id(ComputeRunId run_id) const noexcept {
    return run_states_.find(run_id.value()) != run_states_.end();
  }

  /**
   * @brief Returns current physical policy Run rows.
   * @return Exact row count.
   * @throws Nothing.
   */
  std::uint64_t run_count() const noexcept {
    return static_cast<std::uint64_t>(run_states_.size());
  }

  /**
   * @brief Drops all entries and policy rows during final service teardown.
   * @return Nothing.
   * @throws Nothing; owner destruction is noexcept.
   */
  void clear() noexcept {
    for (std::uint64_t count = 0U; count < entry_count_; ++count) {
      telemetry_.decrement_physical_counter(
          ExecutionLifecyclePhysicalCounter::ReadyEntry);
    }
    entries_.clear();
    run_states_.clear();
    graph_states_.clear();
    entry_count_ = 0U;
    byte_count_ = 0U;
    consecutive_interactive_ = 0U;
  }

  /**
   * @brief Reports whether no ready entry is stored.
   * @return True even if active empty Run policy rows remain.
   * @throws Nothing.
   */
  bool empty() const noexcept { return entry_count_ == 0U; }

  /**
   * @brief Returns current entries across both service classes.
   * @return Exact stored entry count.
   * @throws Nothing.
   */
  std::uint64_t entry_count() const noexcept { return entry_count_; }

  /**
   * @brief Returns current accounted bytes across both service classes.
   * @return Exact stored byte count.
   * @throws Nothing.
   */
  std::uint64_t byte_count() const noexcept { return byte_count_; }

  /**
   * @brief Counts every physical ready entry by immutable Run QoS class.
   * @return One complete store-local diagnostic cut.
   * @throws Nothing; caller holds `PoolState::mutex`.
   * @note Unknown class values make the snapshot invalid while preserving its
   * total count; they are never silently assigned to either known class.
   */
  ExecutionReadyClassSnapshot class_snapshot() const noexcept {
    ExecutionReadyClassSnapshot snapshot;
    for (const std::shared_ptr<QueueEntry>& entry : entries_) {
      ++snapshot.total_entries;
      switch (entry->run->policy_qos.service_class) {
        case ComputeRunQosClass::Interactive:
          ++snapshot.interactive_entries;
          break;
        case ComputeRunQosClass::Throughput:
          ++snapshot.throughput_entries;
          break;
        default:
          snapshot.valid = false;
          break;
      }
    }
    if (snapshot.total_entries != entry_count_) {
      std::terminate();
    }
    return snapshot;
  }

 private:
  /**
   * @brief Returns the fixed bit assigned to one configured physical worker.
   * @param worker_id CPU id in `[0,7]` or the GPU id in `[1,8]`.
   * @return Nonzero bit in the 16-bit per-entry transient mask.
   * @throws Nothing; an out-of-domain trusted id terminates.
   */
  static std::uint16_t worker_mask(int worker_id) noexcept {
    if (worker_id < 0 ||
        worker_id > static_cast<int>(kExecutionWorkerRequestMax)) {
      std::terminate();
    }
    return static_cast<std::uint16_t>(1U << worker_id);
  }

  /**
   * @brief Tests whether one entry is hidden in the current worker cycle.
   * @param entry Current store-owned ready entry.
   * @param worker_id Worker attempting route selection.
   * @return True only after that worker observed grant exhaustion for `entry`.
   * @throws Nothing; invalid internal worker ids terminate.
   */
  static bool is_grant_blocked(const QueueEntry& entry,
                               int worker_id) noexcept {
    return (entry.grant_blocked_worker_mask & worker_mask(worker_id)) != 0U;
  }

  /**
   * @brief Intrusive same-Run FIFO endpoints for one priority hint.
   * @throws Nothing for value construction.
   */
  struct LaneEndpoints final {
    /** @brief Oldest entry in this Run/lane, or null. */
    QueueEntry* head = nullptr;

    /** @brief Newest entry in this Run/lane, or null. */
    QueueEntry* tail = nullptr;
  };

  /**
   * @brief Shared fairness accounting for one stable Graph identity.
   * @throws Nothing for value construction.
   */
  struct PolicyGraphState final {
    /**
     * @brief Creates one Graph fairness row with a nonreused opaque id.
     * @param graph_identity Nonzero service-lifetime Graph identity.
     * @throws Nothing.
     */
    explicit PolicyGraphState(std::uint64_t graph_identity) noexcept
        : graph_id(graph_identity) {}

    /**
     * @brief Returns the class-local raw service accumulator.
     * @param service_class Class already chosen by inter-class arbitration.
     * @return Mutable raw Graph service for only that class.
     * @throws Nothing; an invalid trusted enum terminates.
     * @note Interactive and Throughput history never share this scalar.
     */
    std::uint64_t& charged_service_for(
        ComputeRunQosClass service_class) noexcept {
      switch (service_class) {
        case ComputeRunQosClass::Interactive:
          return interactive_charged_service;
        case ComputeRunQosClass::Throughput:
          return throughput_charged_service;
      }
      std::terminate();
    }

    /** @brief Raw work/byte service charged to Interactive selections. */
    std::uint64_t interactive_charged_service = 0U;

    /** @brief Raw work/byte service charged to Throughput selections. */
    std::uint64_t throughput_charged_service = 0U;

    /** @brief Nonzero opaque identity exposed only as policy snapshot data. */
    const std::uint64_t graph_id;

    /** @brief Active Run policy rows currently sharing this Graph. */
    std::uint64_t active_runs = 0U;
  };

  /**
   * @brief Persistent policy accounting and ready lanes for one active Run.
   * @throws Nothing for scalar construction.
   * @note This row outlives temporary ready emptiness until Run settlement.
   */
  struct PolicyRunState final {
    /**
     * @brief Binds one active Run to its stable Graph policy row.
     * @param active_run Borrowed Run retained by active/entry ownership.
     * @param graph_state Stable map-owned Graph row.
     * @throws Nothing.
     */
    PolicyRunState(RunState* active_run, PolicyGraphState* graph_state) noexcept
        : run(active_run), graph(graph_state) {}

    /** @brief Borrowed active Run; never retained beyond service settlement. */
    RunState* run = nullptr;

    /** @brief Stable shared Graph accounting row. */
    PolicyGraphState* graph = nullptr;

    /**
     * @brief Weight-normalized service charged within this Run's fixed class.
     */
    std::uint64_t charged_service = 0U;

    /** @brief Same-Run high-hint FIFO. */
    LaneEndpoints high;

    /** @brief Same-Run normal-hint FIFO. */
    LaneEndpoints normal;
  };

 public:
  /**
   * @brief Complete detached initial-publication storage for one Run.
   *
   * @throws Nothing from movement/destruction after staging allocations finish.
   * @note Map/list nodes, ready grants, queue entries, and nonreused identities
   * are all owned here before lifecycle installation. No node is visible in the
   * live store until try_publish_prepared_batch().
   */
  struct PreparedBatch final {
    /** @brief Matching Run retained independently from staged entries. */
    std::shared_ptr<RunState> run;
    /** @brief Detached queue-list nodes in initial publication order. */
    std::list<std::shared_ptr<QueueEntry>> entries;
    /** @brief Detached candidate Graph policy row. */
    std::map<std::string, PolicyGraphState> graph_states;
    /** @brief Detached exact Run policy row. */
    std::map<std::uint64_t, PolicyRunState> run_states;
    /** @brief Aggregate ready-entry/byte charge for the initial set. */
    ResourceVector ready_charge;
  };

  /**
   * @brief Allocates every store node for one unpublished initial batch.
   * @param run Matching prepared Run state.
   * @param entries Fully granted queue entries.
   * @return Detached batch with reserved nonreused publication identities.
   * @throws std::invalid_argument for empty/mismatched entries.
   * @throws std::overflow_error when store identities are exhausted.
   * @throws GraphError when aggregate ready charge overflows.
   * @throws std::bad_alloc when detached map/list nodes cannot allocate.
   * @note Caller holds the pool mutex. Identity gaps after staging failure are
   * intentional and preserve non-reuse without publishing any store state.
   */
  PreparedBatch prepare_initial_batch(
      const std::shared_ptr<RunState>& run,
      const std::vector<std::shared_ptr<QueueEntry>>& entries) {
    if (!run || entries.empty()) {
      throw std::invalid_argument(
          "ExecutionService prepared batch requires Run and entries.");
    }
    const std::uint64_t count = static_cast<std::uint64_t>(entries.size());
    if (count >
            std::numeric_limits<std::uint64_t>::max() - next_candidate_id_ ||
        count >
            std::numeric_limits<std::uint64_t>::max() - next_entry_version_ ||
        count > std::numeric_limits<std::uint64_t>::max() -
                    next_enqueue_sequence_ ||
        next_graph_id_ == std::numeric_limits<std::uint64_t>::max()) {
      throw std::overflow_error(
          "ExecutionService ready publication identity space is exhausted.");
    }

    PreparedBatch batch;
    batch.run = run;
    batch.graph_states.try_emplace(std::move(run->available_graph_key),
                                   ++next_graph_id_);
    PolicyGraphState* staged_graph = &batch.graph_states.begin()->second;
    batch.run_states.try_emplace(run->id.value(), run.get(), staged_graph);
    for (const std::shared_ptr<QueueEntry>& entry : entries) {
      if (!entry || entry->run.get() != run.get()) {
        throw std::invalid_argument(
            "ExecutionService prepared batch mixes Run ownership.");
      }
      validate_entry(*entry);
      const std::optional<ResourceVector> next_charge = checked_add_resources(
          batch.ready_charge, entry->ready_grant->resources());
      if (!next_charge.has_value()) {
        throw GraphError(
            GraphErrc::ComputeError,
            "ExecutionService prepared ready charge exceeds representation.");
      }
      batch.ready_charge = *next_charge;
      entry->candidate_id = ++next_candidate_id_;
      entry->entry_version = ++next_entry_version_;
      entry->enqueue_sequence = ++next_enqueue_sequence_;
      batch.entries.push_back(entry);
    }
    return batch;
  }

  /**
   * @brief Atomically links one detached initial batch into the live store.
   * @param batch Active detached batch from prepare_initial_batch().
   * @return True after publication, false when current bounded capacity rejects
   * the complete batch without mutation.
   * @throws std::invalid_argument for inactive/foreign/duplicate Run state.
   * @throws std::system_error only if standard container internals violate
   * their allocator/comparator contract.
   * @note Caller holds the pool and matching Run mutexes. All allocating nodes
   * already exist. Graph-row insertion rollback preserves an empty live store
   * if the Run node cannot link.
   */
  bool try_publish_prepared_batch(PreparedBatch& batch) {
    if (!batch.run || batch.entries.empty() ||
        batch.graph_states.size() != 1U || batch.run_states.size() != 1U ||
        batch.run_states.begin()->first != batch.run->id.value()) {
      throw std::invalid_argument(
          "ExecutionService prepared batch is inactive or malformed.");
    }
    if (contains_run_id(batch.run->id)) {
      throw std::invalid_argument(
          "ExecutionService prepared Run id is already active.");
    }
    const std::optional<ResourceVector> next = checked_add_resources(
        ResourceVector{0U, 0U, 0U, entry_count_, byte_count_},
        batch.ready_charge);
    if (!next.has_value() || next->ready_entries > entry_limit_ ||
        next->ready_bytes > byte_limit_) {
      return false;
    }

    bool graph_inserted = false;
    auto graph_it = graph_states_.find(batch.run->graph);
    if (graph_it == graph_states_.end()) {
      graph_states_.merge(batch.graph_states);
      graph_it = graph_states_.find(batch.run->graph);
      if (graph_it == graph_states_.end()) {
        std::terminate();
      }
      graph_inserted = true;
    }

    batch.run_states.begin()->second.graph = &graph_it->second;
    try {
      run_states_.merge(batch.run_states);
    } catch (...) {
      if (graph_inserted) {
        auto graph_node = graph_states_.extract(graph_it);
        batch.graph_states.insert(std::move(graph_node));
      }
      throw;
    }
    const auto run_it = run_states_.find(batch.run->id.value());
    if (run_it == run_states_.end() || !batch.run_states.empty()) {
      std::terminate();
    }
    ++graph_it->second.active_runs;

    const auto first = batch.entries.begin();
    entries_.splice(entries_.end(), batch.entries);
    for (auto entry_it = first; entry_it != entries_.end(); ++entry_it) {
      QueueEntry& entry = **entry_it;
      entry.store_position = entry_it;
      entry.store_owned = true;
      entry.high_lane = entry.priority == ExecutionTaskPriority::High;
      entry.grant_blocked_worker_mask = 0U;
      entry.enqueued_class_dispatch_count =
          class_dispatch_count(entry.run->policy_qos.service_class);
      link_entry(run_it->second, entry);
      telemetry_.increment_physical_counter(
          ExecutionLifecyclePhysicalCounter::ReadyEntry);
    }
    entry_count_ = next->ready_entries;
    byte_count_ = next->ready_bytes;
    batch.run.reset();
    return true;
  }

 private:
  /**
   * @brief Complete mutable selection retained only during one pop call.
   * @throws Nothing for value construction.
   */
  struct SelectedEntry final {
    /** @brief Chosen persistent Run policy row. */
    PolicyRunState* run_state = nullptr;

    /** @brief Chosen store-owned entry. */
    QueueEntry* entry = nullptr;

    /** @brief Graph charged service after this selection. */
    std::uint64_t graph_score = 0U;

    /** @brief Run normalized service after this selection. */
    std::uint64_t run_score = 0U;

    /** @brief Whether dispatch-count aging selected this entry. */
    bool aged = false;
  };

  /**
   * @brief Host-private candidate owner used only while forming a frontier.
   * @throws Nothing after the shared pin and ABI scalar record exist.
   * @note The ABI field is safe to copy to foreign code; pointer fields never
   * leave trusted Host control or survive the current locked operation.
   */
  struct CandidateRecord final {
    /** @brief Matching persistent Run policy row. */
    PolicyRunState* run_state = nullptr;

    /** @brief Exact store-owned entry retained during local computation. */
    std::shared_ptr<QueueEntry> entry;

    /** @brief Complete authority-free ABI descriptor. */
    ps_policy_candidate_v1 abi{};

    /** @brief Raw positive cost used by the Graph admissibility band. */
    std::uint64_t raw_cost = 0U;

    /** @brief Normalized positive cost used by the Run admissibility band. */
    std::uint64_t normalized_cost = 0U;
  };

  /**
   * @brief Adds an ordering-only counter without unsigned wraparound.
   * @param lhs Existing charged service.
   * @param rhs Positive candidate cost.
   * @return Exact sum or the representable ceiling.
   * @throws Nothing.
   * @note Saturation affects ordering only; aged selection preserves progress.
   */
  static std::uint64_t saturating_add(std::uint64_t lhs,
                                      std::uint64_t rhs) noexcept {
    if (rhs > std::numeric_limits<std::uint64_t>::max() - lhs) {
      return std::numeric_limits<std::uint64_t>::max();
    }
    return lhs + rhs;
  }

  /**
   * @brief Converts one optional steady-clock deadline into ABI nanoseconds.
   * @param deadline Optional absolute monotonic time point.
   * @return Finite nanoseconds when present, otherwise the ABI sentinel.
   * @throws Nothing; negative times clamp to zero and large values clamp below
   * the reserved no-deadline sentinel.
   */
  static std::uint64_t deadline_nanoseconds(
      const std::optional<std::chrono::steady_clock::time_point>&
          deadline) noexcept {
    if (!deadline.has_value()) {
      return PS_POLICY_NO_DEADLINE_NS;
    }
    const auto raw = std::chrono::duration_cast<std::chrono::nanoseconds>(
                         deadline->time_since_epoch())
                         .count();
    if (raw <= 0) {
      return 0U;
    }
    return std::min<std::uint64_t>(static_cast<std::uint64_t>(raw),
                                   PS_POLICY_NO_DEADLINE_NS - 1U);
  }

  /**
   * @brief Tests immutable and Run-local conditions for one physical start.
   * @param run Run whose mutex is held by the caller.
   * @param entry Ready entry whose selected device fixes the physical lane.
   * @param worker_id Stable worker id.
   * @param lane Fixed physical lane owned by the worker.
   * @param routes Host-owned route state used only for startability checks.
   * @return True only for a live, uncancelled, class/route-capable Run.
   * @throws Nothing.
   */
  bool route_startable(
      const RunState& run, const QueueEntry& entry, int worker_id,
      execution::PhysicalExecutionLane lane,
      const execution::PhysicalExecutionRoutes& routes) const noexcept {
    if (!run.accepting || run.cancelled || run.first_exception != nullptr) {
      return false;
    }
    if (run.policy_qos.maximum_parallelism.has_value() &&
        static_cast<std::uint64_t>(run.in_flight) >=
            *run.policy_qos.maximum_parallelism) {
      return false;
    }
    if (!operation_gate_.can_start(entry.submission.operation_constraints())) {
      return false;
    }
    const DeviceBackend device = entry.submission.metadata().device();
    if (!run.exposes_device(device)) {
      return false;
    }
    return routes.can_start(run.route, device, lane, worker_id,
                            static_cast<std::uint64_t>(run.in_flight));
  }

  /**
   * @brief Builds the exact child-grant vector for one physical task start.
   * @param run Run whose immutable demand and retained bytes are authoritative.
   * @return Complete per-task Host resource demand.
   * @throws Nothing.
   */
  static ResourceVector task_execution_resources(const RunState& run) noexcept {
    return ResourceVector{1U, run.execution_retained_bytes_per_task,
                          run.resource_demand.scratch_bytes, 0U, 0U};
  }

  /**
   * @brief Tests whether a Run can mint its next exact execution child grant.
   * @param run Run whose mutex is held by the caller.
   * @return True only when live reservation capacity covers every dimension.
   * @throws Nothing; a reservation-state locking failure terminates because
   * this helper is a no-throw evidence probe.
   * @note This copies authority-free capacity and never mints or consumes a
   * grant. A later `try_grant()` remains the sole commit linearization point.
   */
  static bool has_execution_capacity(const RunState& run) noexcept {
    return run.reservation.has_value() &&
           resources_fit(task_execution_resources(run),
                         run.reservation->available());
  }

  /**
   * @brief Tests whether one class contains scheduler-selectable ready work.
   * @param service_class Class already independent from intent and route.
   * @param worker_id Worker attempting a start.
   * @param lane Fixed physical lane owned by the worker.
   * @param routes Host-owned route state used only for startability checks.
   * @return True when at least one live Run exposes one route-, operation-,
   * and lifecycle-eligible lane head for policy selection.
   * @throws Nothing.
   * @note Execution child-grant capacity is deliberately excluded. A selected
   * but grant-exhausted entry reaches `commit_start()`, receives a
   * worker-local grant-block mark, and leaves the same selection cycle free to
   * search other candidates without advancing dispatch or fairness state.
   */
  bool has_scheduler_selectable(
      ComputeRunQosClass service_class, int worker_id,
      execution::PhysicalExecutionLane lane,
      const execution::PhysicalExecutionRoutes& routes) noexcept {
    for (auto& row : run_states_) {
      PolicyRunState& state = row.second;
      if (state.run->policy_qos.service_class != service_class) {
        continue;
      }
      std::lock_guard<std::mutex> run_lock(state.run->mutex);
      if (candidate_entry_for_lane(state, worker_id, lane, routes) != nullptr) {
        return true;
      }
    }
    return false;
  }

  /**
   * @brief Tests whether one class has evidence-startable ready work.
   * @param service_class Class already independent from intent and route.
   * @param worker_id Worker whose lane-local candidate visibility is sampled.
   * @param lane Fixed physical lane owned by the worker.
   * @param routes Host-owned route state used only for eligibility checks.
   * @return True when one scheduler-selectable lane head can also mint its
   * exact execution child grant from live reservation capacity.
   * @throws Nothing.
   * @note This observation-only probe never filters the policy frontier and
   * never mints authority. `try_grant()` remains the sole commit linearization
   * point, and a failed commit publishes no service-start observation.
   */
  bool has_evidence_startable(
      ComputeRunQosClass service_class, int worker_id,
      execution::PhysicalExecutionLane lane,
      const execution::PhysicalExecutionRoutes& routes) noexcept {
    for (auto& row : run_states_) {
      PolicyRunState& state = row.second;
      if (state.run->policy_qos.service_class != service_class) {
        continue;
      }
      std::lock_guard<std::mutex> run_lock(state.run->mutex);
      if (candidate_entry_for_lane(state, worker_id, lane, routes) != nullptr &&
          has_execution_capacity(*state.run)) {
        return true;
      }
    }
    return false;
  }

  /**
   * @brief Applies the fixed three-to-one inter-class arbitration rule.
   * @param worker_id Worker attempting a start.
   * @param lane Fixed physical lane owned by the worker.
   * @param routes Host-owned route state used only for startability checks.
   * @return Selected class, or null when this worker has no selectable route.
   * @throws Nothing.
   * @note Class competition is intentionally independent of transient child-
   * grant capacity. Grant exhaustion is handled only after policy selection.
   */
  std::optional<ComputeRunQosClass> choose_class(
      int worker_id, execution::PhysicalExecutionLane lane,
      const execution::PhysicalExecutionRoutes& routes) noexcept {
    const bool interactive_selectable = has_scheduler_selectable(
        ComputeRunQosClass::Interactive, worker_id, lane, routes);
    const bool throughput_selectable = has_scheduler_selectable(
        ComputeRunQosClass::Throughput, worker_id, lane, routes);
    if (!interactive_selectable && !throughput_selectable) {
      return std::nullopt;
    }
    if (interactive_selectable &&
        (!throughput_selectable ||
         consecutive_interactive_ < kInteractiveBurstLimit)) {
      return ComputeRunQosClass::Interactive;
    }
    return ComputeRunQosClass::Throughput;
  }

  /**
   * @brief Creates one trusted ABI record from a current lane head.
   * @param state Matching Run/Graph fairness row.
   * @param entry Current store-owned lane head.
   * @return Complete local candidate with shared pin and scalar ABI bytes.
   * @throws Nothing after copying the existing shared owner.
   */
  CandidateRecord make_candidate(PolicyRunState& state,
                                 QueueEntry& entry) const noexcept {
    const ComputeRunQosClass service_class =
        state.run->policy_qos.service_class;
    const std::uint64_t dispatch_count = class_dispatch_count(service_class);
    const std::uint64_t age =
        dispatch_count >= entry.enqueued_class_dispatch_count
            ? dispatch_count - entry.enqueued_class_dispatch_count
            : 0U;
    const std::uint64_t normalized =
        policy_for(service_class)
            .normalized_cost(entry.policy_service_cost,
                             state.run->policy_qos.weight);
    ps_policy_candidate_v1 abi{};
    abi.struct_size = sizeof(abi);
    abi.struct_kind = PS_POLICY_STRUCT_CANDIDATE;
    abi.candidate_id = entry.candidate_id;
    abi.graph_id = state.graph->graph_id;
    abi.run_id = state.run->id.value();
    abi.deadline_ns = deadline_nanoseconds(state.run->policy_qos.deadline);
    abi.weight = state.run->policy_qos.weight;
    abi.work_units = entry.submission.resource_demand().work_units;
    abi.ready_bytes = entry.ready_grant->resources().ready_bytes;
    abi.graph_service_score =
        saturating_add(state.graph->charged_service_for(service_class),
                       entry.policy_service_cost);
    abi.run_service_score = saturating_add(state.charged_service, normalized);
    abi.dispatch_age = age;
    abi.enqueue_sequence = entry.enqueue_sequence;
    if (entry.high_lane) {
      abi.flags |= PS_POLICY_CANDIDATE_FLAG_HIGH_PRIORITY_HINT;
    }
    if (state.run->policy_qos.deadline.has_value()) {
      abi.flags |= PS_POLICY_CANDIDATE_FLAG_DEADLINE_PRESENT;
    }
    return CandidateRecord{&state, *entry.store_position, abi,
                           entry.policy_service_cost, normalized};
  }

  /**
   * @brief Reduces current base candidates to the exact admissible frontier.
   * @param service_class Class chosen before aging.
   * @param worker_id Worker attempting a start.
   * @param lane Fixed physical lane owned by the worker.
   * @param routes Host-owned route state used only for startability checks.
   * @return At most one current candidate per live Run after age/deadline and
   * one-quantum Graph/Run bands.
   * @throws std::bad_alloc when temporary ownership cannot allocate.
   * @note Caller holds the service/store mutex. Each Run mutex is held only
   * while copying its current startability facts.
   */
  std::vector<CandidateRecord> build_frontier(
      ComputeRunQosClass service_class, int worker_id,
      execution::PhysicalExecutionLane lane,
      const execution::PhysicalExecutionRoutes& routes) {
    std::vector<CandidateRecord> candidates;
    candidates.reserve(run_states_.size());
    for (auto& row : run_states_) {
      PolicyRunState& state = row.second;
      if (state.run->policy_qos.service_class != service_class) {
        continue;
      }
      std::lock_guard<std::mutex> run_lock(state.run->mutex);
      QueueEntry* entry =
          candidate_entry_for_lane(state, worker_id, lane, routes);
      if (entry != nullptr) {
        candidates.push_back(make_candidate(state, *entry));
      }
    }
    if (candidates.empty()) {
      return candidates;
    }

    const auto erase_unless = [&candidates](const auto& predicate) {
      candidates.erase(std::remove_if(candidates.begin(), candidates.end(),
                                      [&predicate](const auto& candidate) {
                                        return !predicate(candidate);
                                      }),
                       candidates.end());
    };
    const std::uint64_t maximum_age =
        std::max_element(candidates.begin(), candidates.end(),
                         [](const auto& lhs, const auto& rhs) {
                           return lhs.abi.dispatch_age < rhs.abi.dispatch_age;
                         })
            ->abi.dispatch_age;
    if (maximum_age >= kPolicyAgingDispatches) {
      erase_unless([maximum_age](const auto& candidate) {
        return candidate.abi.dispatch_age == maximum_age;
      });
    } else if (service_class == ComputeRunQosClass::Interactive) {
      std::uint64_t minimum_deadline = PS_POLICY_NO_DEADLINE_NS;
      for (const CandidateRecord& candidate : candidates) {
        if ((candidate.abi.flags & PS_POLICY_CANDIDATE_FLAG_DEADLINE_PRESENT) !=
            0U) {
          minimum_deadline =
              std::min(minimum_deadline, candidate.abi.deadline_ns);
        }
      }
      if (minimum_deadline != PS_POLICY_NO_DEADLINE_NS) {
        erase_unless([minimum_deadline](const auto& candidate) {
          return candidate.abi.deadline_ns == minimum_deadline;
        });
      }
    }

    const bool graph_scores_saturated = std::all_of(
        candidates.begin(), candidates.end(), [](const auto& value) {
          return value.abi.graph_service_score ==
                 std::numeric_limits<std::uint64_t>::max();
        });
    if (graph_scores_saturated) {
      const auto oldest = std::min_element(
          candidates.begin(), candidates.end(),
          [](const auto& lhs, const auto& rhs) {
            return lhs.abi.enqueue_sequence < rhs.abi.enqueue_sequence;
          });
      const std::uint64_t graph_id = oldest->abi.graph_id;
      erase_unless([graph_id](const auto& candidate) {
        return candidate.abi.graph_id == graph_id;
      });
    } else {
      std::uint64_t minimum_score = std::numeric_limits<std::uint64_t>::max();
      std::uint64_t maximum_cost = 0U;
      for (const CandidateRecord& candidate : candidates) {
        minimum_score =
            std::min(minimum_score, candidate.abi.graph_service_score);
        maximum_cost = std::max(maximum_cost, candidate.raw_cost);
      }
      const std::uint64_t limit = saturating_add(minimum_score, maximum_cost);
      erase_unless([limit](const auto& candidate) {
        return candidate.abi.graph_service_score <= limit;
      });
    }

    std::map<std::uint64_t, std::pair<std::uint64_t, std::uint64_t>> run_bands;
    for (const CandidateRecord& candidate : candidates) {
      auto [found, inserted] = run_bands.try_emplace(
          candidate.abi.graph_id, candidate.abi.run_service_score,
          candidate.normalized_cost);
      if (!inserted) {
        found->second.first =
            std::min(found->second.first, candidate.abi.run_service_score);
        found->second.second =
            std::max(found->second.second, candidate.normalized_cost);
      }
    }
    erase_unless([&run_bands](const auto& candidate) {
      const auto found = run_bands.find(candidate.abi.graph_id);
      if (found == run_bands.end()) {
        return false;
      }
      const std::uint64_t limit =
          saturating_add(found->second.first, found->second.second);
      return candidate.abi.run_service_score <= limit;
    });
    return candidates;
  }

  /**
   * @brief Converts one private candidate into an exact nonmutating pin.
   * @param candidate Current frontier member.
   * @return Complete shared pin and projected charges.
   * @throws Nothing.
   */
  static SelectionPin pin_from(const CandidateRecord& candidate) noexcept {
    return SelectionPin{candidate.entry,
                        candidate.entry->run->policy_qos.service_class,
                        candidate.abi.graph_service_score,
                        candidate.abi.run_service_score,
                        candidate.abi.candidate_id,
                        candidate.entry->entry_version,
                        candidate.abi.enqueue_sequence,
                        candidate.entry->run->route_generation};
  }

  /**
   * @brief Converts one allocation-free built-in choice into a shared pin.
   * @param selected Current store-owned selection.
   * @return Complete shared pin.
   * @throws Nothing.
   */
  static SelectionPin pin_from(const SelectedEntry& selected) noexcept {
    std::shared_ptr<QueueEntry> entry = *selected.entry->store_position;
    return SelectionPin{entry,
                        entry->run->policy_qos.service_class,
                        selected.graph_score,
                        selected.run_score,
                        entry->candidate_id,
                        entry->entry_version,
                        entry->enqueue_sequence,
                        entry->run->route_generation};
  }

  /**
   * @brief Revalidates exact object, nonreused identities, lane head, and row.
   * @param pin Candidate retained after policy return.
   * @param state Matching current Run policy row.
   * @param worker_id Stable worker id.
   * @param lane Fixed physical lane owned by the worker.
   * @param routes Host-owned route state used for startability checks.
   * @return True only while the identical visible object remains eligible.
   * @throws Nothing.
   */
  bool pin_matches(
      const SelectionPin& pin, PolicyRunState& state, int worker_id,
      execution::PhysicalExecutionLane lane,
      const execution::PhysicalExecutionRoutes& routes) const noexcept {
    return pin.entry->store_owned &&
           pin.entry->candidate_id == pin.candidate_id &&
           pin.entry->entry_version == pin.entry_version &&
           pin.entry->enqueue_sequence == pin.enqueue_sequence &&
           pin.entry->run->route_generation == pin.route_generation &&
           pin.entry->run.get() == state.run &&
           candidate_entry_for_lane(state, worker_id, lane, routes) ==
               pin.entry.get() &&
           *pin.entry->store_position == pin.entry;
  }

  /**
   * @brief Returns the successful-start counter for one policy class.
   * @param service_class Explicit class selected before aging.
   * @return Mutable class-local counter.
   * @throws Nothing; an invalid trusted enum terminates.
   */
  std::uint64_t& class_dispatch_count(
      ComputeRunQosClass service_class) noexcept {
    switch (service_class) {
      case ComputeRunQosClass::Interactive:
        return interactive_dispatch_count_;
      case ComputeRunQosClass::Throughput:
        return throughput_dispatch_count_;
    }
    std::terminate();
  }

  /**
   * @brief Returns the successful-start counter for one policy class.
   * @param service_class Explicit class selected before aging.
   * @return Immutable class-local counter.
   * @throws Nothing; an invalid trusted enum terminates.
   */
  const std::uint64_t& class_dispatch_count(
      ComputeRunQosClass service_class) const noexcept {
    switch (service_class) {
      case ComputeRunQosClass::Interactive:
        return interactive_dispatch_count_;
      case ComputeRunQosClass::Throughput:
        return throughput_dispatch_count_;
    }
    std::terminate();
  }

  /**
   * @brief Validates one entry before any store or policy mutation.
   * @param entry Candidate owned by its staging caller.
   * @return Nothing.
   * @throws std::invalid_argument for malformed ownership, grant, identity,
   * class, or checked-cost state.
   */
  void validate_entry(const QueueEntry& entry) const {
    if (!entry.run || !entry.ready_grant.has_value() ||
        !entry.ready_grant->active() || entry.store_owned ||
        entry.policy_service_cost == 0U ||
        entry.submission.metadata().run_id() != entry.run->id ||
        entry.submission.metadata().graph_identity() != entry.run->graph ||
        entry.submission.metadata().qos().service_class !=
            entry.run->policy_qos.service_class) {
      throw std::invalid_argument(
          "Bounded ready store received invalid policy entry ownership.");
    }
    const ResourceVector charge = entry.ready_grant->resources();
    if (charge.cpu_slots != 0U || charge.retained_memory_bytes != 0U ||
        charge.scratch_bytes != 0U || charge.ready_entries != 1U ||
        charge.ready_bytes == 0U) {
      throw std::invalid_argument(
          "Bounded ready store received an invalid ready grant.");
    }
  }

  /**
   * @brief Links one published entry to its same-Run priority FIFO.
   * @param state Matching persistent Run policy row.
   * @param entry Newly list-owned entry with no intrusive neighbors.
   * @return Nothing.
   * @throws Nothing; invalid linkage terminates.
   */
  static void link_entry(PolicyRunState& state, QueueEntry& entry) noexcept {
    if (entry.run_previous != nullptr || entry.run_next != nullptr) {
      std::terminate();
    }
    LaneEndpoints& lane = entry.high_lane ? state.high : state.normal;
    entry.run_previous = lane.tail;
    if (lane.tail != nullptr) {
      lane.tail->run_next = &entry;
    } else {
      lane.head = &entry;
    }
    lane.tail = &entry;
  }

  /**
   * @brief Returns the same-Run entry eligible for global policy ranking.
   * @param state Active Run state.
   * @return Oldest aged lane head, otherwise the high-hint head when present.
   * @throws Nothing.
   * @note Aging overrides the same-Run priority hint so a stream of newly
   * released high work cannot indefinitely hide an older normal candidate.
   */
  QueueEntry* candidate_entry(PolicyRunState& state) const noexcept {
    if (state.high.head == nullptr) {
      return state.normal.head;
    }
    if (state.normal.head == nullptr) {
      return state.high.head;
    }

    const bool high_aged = is_aged(*state.high.head);
    const bool normal_aged = is_aged(*state.normal.head);
    if (high_aged != normal_aged) {
      return normal_aged ? state.normal.head : state.high.head;
    }
    if (high_aged) {
      return state.normal.head->enqueue_sequence <
                     state.high.head->enqueue_sequence
                 ? state.normal.head
                 : state.high.head;
    }
    return state.high.head;
  }

  /**
   * @brief Returns the first entry in one priority FIFO usable by a worker.
   * @param state Active Run policy row whose Run mutex is held.
   * @param head Oldest entry in one intrusive priority FIFO.
   * @param worker_id Stable worker id.
   * @param lane Fixed physical lane owned by the worker.
   * @param routes Host-owned route state used for startability checks.
   * @return First startable entry, or null when this FIFO belongs to another
   * physical lane.
   * @throws Nothing.
   * @note Skipping an opposite-device head does not mutate FIFO order; each
   * physical lane independently observes its oldest compatible entry.
   */
  QueueEntry* first_startable_entry(
      PolicyRunState& state, QueueEntry* head, int worker_id,
      execution::PhysicalExecutionLane lane,
      const execution::PhysicalExecutionRoutes& routes) const noexcept {
    for (QueueEntry* entry = head; entry != nullptr; entry = entry->run_next) {
      if (!is_grant_blocked(*entry, worker_id) &&
          route_startable(*state.run, *entry, worker_id, lane, routes)) {
        return entry;
      }
    }
    return nullptr;
  }

  /**
   * @brief Returns the same-Run candidate for one fixed physical lane.
   * @param state Active Run policy row whose Run mutex is held.
   * @param worker_id Stable worker id.
   * @param lane Fixed physical lane owned by the worker.
   * @param routes Host-owned route state used for startability checks.
   * @return Oldest compatible aged head, otherwise compatible high work first.
   * @throws Nothing.
   * @note Device filtering happens before the existing high/normal aging rule;
   * opposite-lane entries remain published for their owning worker.
   */
  QueueEntry* candidate_entry_for_lane(
      PolicyRunState& state, int worker_id,
      execution::PhysicalExecutionLane lane,
      const execution::PhysicalExecutionRoutes& routes) const noexcept {
    QueueEntry* high =
        first_startable_entry(state, state.high.head, worker_id, lane, routes);
    QueueEntry* normal = first_startable_entry(state, state.normal.head,
                                               worker_id, lane, routes);
    if (high == nullptr) {
      return normal;
    }
    if (normal == nullptr) {
      return high;
    }
    const bool high_aged = is_aged(*high);
    const bool normal_aged = is_aged(*normal);
    if (high_aged != normal_aged) {
      return normal_aged ? normal : high;
    }
    if (high_aged) {
      return normal->enqueue_sequence < high->enqueue_sequence ? normal : high;
    }
    return high;
  }

  /**
   * @brief Tests one entry's deterministic dispatch-count age.
   * @param entry Published entry.
   * @return True after at least eight later successful dispatches.
   * @throws Nothing.
   */
  bool is_aged(const QueueEntry& entry) const noexcept {
    const std::uint64_t dispatch_count =
        class_dispatch_count(entry.run->policy_qos.service_class);
    return dispatch_count >= entry.enqueued_class_dispatch_count &&
           dispatch_count - entry.enqueued_class_dispatch_count >=
               kPolicyAgingDispatches;
  }

  /**
   * @brief Reports whether one explicit service class has ready work.
   * @param service_class QoS class to inspect.
   * @return True when any matching active Run has an eligible entry.
   * @throws Nothing.
   */
  bool has_ready(ComputeRunQosClass service_class) noexcept {
    for (auto& row : run_states_) {
      PolicyRunState& state = row.second;
      if (state.run->policy_qos.service_class == service_class &&
          candidate_entry(state) != nullptr) {
        return true;
      }
    }
    return false;
  }

  /**
   * @brief Selects one entry within a policy's explicit service class.
   * @param policy Stateless built-in ranking strategy.
   * @param worker_id Stable worker id.
   * @param lane Fixed physical lane owned by the worker.
   * @param routes Host-owned route state used for startability checks.
   * @return Complete chosen row/entry and next accounting scores.
   * @throws Nothing.
   * @note Aged candidates precede ordinary comparison only within the class
   * already selected by inter-class arbitration.
   */
  SelectedEntry select_from_class(
      const BuiltinPolicy& policy, int worker_id,
      execution::PhysicalExecutionLane lane,
      const execution::PhysicalExecutionRoutes& routes) noexcept {
    SelectedEntry best;
    BuiltinPolicy::Candidate best_snapshot;
    std::uint64_t maximum_age = 0U;
    std::optional<std::chrono::steady_clock::time_point> minimum_deadline;
    bool any_deadline = false;

    for (auto& row : run_states_) {
      PolicyRunState& state = row.second;
      if (state.run->policy_qos.service_class != policy.service_class()) {
        continue;
      }
      std::lock_guard<std::mutex> run_lock(state.run->mutex);
      QueueEntry* entry =
          candidate_entry_for_lane(state, worker_id, lane, routes);
      if (entry == nullptr) {
        continue;
      }
      const std::uint64_t dispatch_count =
          class_dispatch_count(policy.service_class());
      const std::uint64_t age =
          dispatch_count >= entry->enqueued_class_dispatch_count
              ? dispatch_count - entry->enqueued_class_dispatch_count
              : 0U;
      maximum_age = std::max(maximum_age, age);
      if (state.run->policy_qos.deadline.has_value()) {
        any_deadline = true;
        if (!minimum_deadline.has_value() ||
            *state.run->policy_qos.deadline < *minimum_deadline) {
          minimum_deadline = state.run->policy_qos.deadline;
        }
      }
    }

    for (auto& row : run_states_) {
      PolicyRunState& state = row.second;
      if (state.run->policy_qos.service_class != policy.service_class()) {
        continue;
      }
      std::lock_guard<std::mutex> run_lock(state.run->mutex);
      QueueEntry* entry =
          candidate_entry_for_lane(state, worker_id, lane, routes);
      if (entry == nullptr) {
        continue;
      }
      const std::uint64_t graph_score = saturating_add(
          state.graph->charged_service_for(policy.service_class()),
          entry->policy_service_cost);
      const std::uint64_t run_score =
          saturating_add(state.charged_service,
                         policy.normalized_cost(entry->policy_service_cost,
                                                state.run->policy_qos.weight));
      const std::uint64_t dispatch_count =
          class_dispatch_count(policy.service_class());
      const std::uint64_t age =
          dispatch_count >= entry->enqueued_class_dispatch_count
              ? dispatch_count - entry->enqueued_class_dispatch_count
              : 0U;
      const bool aged =
          maximum_age >= kPolicyAgingDispatches && age == maximum_age;
      if (maximum_age >= kPolicyAgingDispatches && !aged) {
        continue;
      }
      if (maximum_age < kPolicyAgingDispatches &&
          policy.service_class() == ComputeRunQosClass::Interactive &&
          any_deadline && state.run->policy_qos.deadline != minimum_deadline) {
        continue;
      }
      const BuiltinPolicy::Candidate snapshot{
          state.run->policy_qos.deadline,
          graph_score,
          run_score,
          entry->enqueue_sequence,
      };

      bool replace = best.entry == nullptr;
      if (!replace) {
        replace = policy.precedes(snapshot, best_snapshot);
      }
      if (replace) {
        best = SelectedEntry{&state, entry, graph_score, run_score, aged};
        best_snapshot = snapshot;
      }
    }
    return best;
  }

  /**
   * @brief Unlinks and destroys one store list node with exact accounting.
   * @param entry Store-owned entry selected or purged.
   * @param state Matching Run policy row retained after removal.
   * @param retired_entries Optional list receiving the unlinked node without
   * destruction while the caller owns service locks.
   * @return Nothing.
   * @throws Nothing; invalid linkage/accounting terminates.
   * @note A non-null destination uses allocation-free list splicing. The
   * caller destroys those nodes only after releasing pool and Run locks.
   */
  void remove_entry(QueueEntry& entry, PolicyRunState& state,
                    std::list<std::shared_ptr<QueueEntry>>* retired_entries =
                        nullptr) noexcept {
    if (!entry.store_owned || entry.run.get() != state.run ||
        !entry.ready_grant.has_value() || !entry.ready_grant->active()) {
      std::terminate();
    }
    LaneEndpoints& lane = entry.high_lane ? state.high : state.normal;
    if (entry.run_previous != nullptr) {
      entry.run_previous->run_next = entry.run_next;
    } else if (lane.head == &entry) {
      lane.head = entry.run_next;
    } else {
      std::terminate();
    }
    if (entry.run_next != nullptr) {
      entry.run_next->run_previous = entry.run_previous;
    } else if (lane.tail == &entry) {
      lane.tail = entry.run_previous;
    } else {
      std::terminate();
    }

    const ResourceVector charge = entry.ready_grant->resources();
    if (charge.ready_entries != 1U || charge.ready_entries > entry_count_ ||
        charge.ready_bytes > byte_count_) {
      std::terminate();
    }
    entry_count_ -= charge.ready_entries;
    byte_count_ -= charge.ready_bytes;
    entry.store_owned = false;
    entry.run_previous = nullptr;
    entry.run_next = nullptr;
    if (retired_entries == nullptr) {
      entries_.erase(entry.store_position);
    } else {
      retired_entries->splice(retired_entries->end(), entries_,
                              entry.store_position);
    }
    telemetry_.decrement_physical_counter(
        ExecutionLifecyclePhysicalCounter::ReadyEntry);
  }

  /** @brief Immutable maximum entries across all classes and lanes. */
  const std::uint64_t entry_limit_;

  /** @brief Immutable maximum accounted bytes across all classes and lanes. */
  const std::uint64_t byte_limit_;

  /** @brief Stable non-owning physical lifecycle counter owner. */
  ExecutionLifecycleTelemetry& telemetry_;

  /** @brief Stable execution-domain exact-identity and exclusion gate. */
  OperationStartGate& operation_gate_;

  /** @brief Store-owned list node for every currently ready value. */
  std::list<std::shared_ptr<QueueEntry>> entries_;

  /** @brief Persistent active-Run policy rows keyed by opaque Run id. */
  std::map<std::uint64_t, PolicyRunState> run_states_;

  /** @brief Shared Graph fairness rows keyed by stable Graph identity. */
  std::map<std::string, PolicyGraphState> graph_states_;

  /** @brief Stateless deadline-aware interactive built-in strategy. */
  InteractiveBuiltinPolicy interactive_policy_;

  /** @brief Stateless weighted throughput built-in strategy. */
  ThroughputBuiltinPolicy throughput_policy_;

  /** @brief Current entries across all classes and lanes. */
  std::uint64_t entry_count_ = 0U;

  /** @brief Current exact ready-grant bytes across all entries. */
  std::uint64_t byte_count_ = 0U;

  /** @brief Successful Interactive starts used only for class-local aging. */
  std::uint64_t interactive_dispatch_count_ = 0U;

  /** @brief Successful Throughput starts used only for class-local aging. */
  std::uint64_t throughput_dispatch_count_ = 0U;

  /** @brief Stable publication sequence assigned without reuse. */
  std::uint64_t next_enqueue_sequence_ = 0U;

  /** @brief Stable candidate identity assigned without reuse. */
  std::uint64_t next_candidate_id_ = 0U;

  /** @brief Stable private entry version assigned without reuse. */
  std::uint64_t next_entry_version_ = 0U;

  /** @brief Stable opaque Graph identity assigned without reuse. */
  std::uint64_t next_graph_id_ = 0U;

  /** @brief Stable immutable snapshot generation assigned without reuse. */
  std::uint64_t next_snapshot_generation_ = 0U;

  /** @brief Stable callback-attempt sequence assigned without reuse. */
  std::uint64_t next_selection_sequence_ = 0U;

  /** @brief Current interactive burst while throughput remains ready. */
  std::uint64_t consecutive_interactive_ = 0U;
};

}  // namespace ps::compute
