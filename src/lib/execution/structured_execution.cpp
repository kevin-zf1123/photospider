#include "execution/structured_execution.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cstring>
#include <exception>
#include <functional>
#include <iterator>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
#include <utility>
#include <variant>
#include <vector>

#include "core/callback_environment.hpp"
#include "core/resource_observation.hpp"
#include "core/stored_failure.hpp"
#include "data/result_support.hpp"
#include "data/result_window_access.hpp"
#include "execution/cpu_range_context.hpp"
#include "execution/demand_query.hpp"
#include "execution/publication_diagnostics.hpp"
#include "execution/result_blocks.hpp"
#include "execution/result_cache.hpp"
#include "execution/result_cache_proof.hpp"
#include "execution/result_checkpoints.hpp"
#include "execution/result_fallback.hpp"
#include "execution/result_gpu_service.hpp"
#include "execution/result_native.hpp"
#include "execution/result_protocol.hpp"
#include "execution/result_publication_validation.hpp"
#include "execution/result_subscription.hpp"
#include "execution/structured_actor_lifecycle.hpp"
#include "execution/structured_need.hpp"
#include "execution/structured_result_cache.hpp"
#include "execution/tile_records.hpp"
#include "photospider/core/checked_math.hpp"
#include "photospider/core/content_digest.hpp"
#include "plugin/dependency_identity.hpp"
#include "plugin/failure_latch.hpp"
#include "plugin/operation_identity.hpp"
#include "plugin/port_validation.hpp"
#include "plugin/publication_visibility.hpp"
#include "plugin/result_need_validation.hpp"
#include "plugin/result_payload_bound.hpp"
#include "plugin/validation_dispatch.hpp"

#if defined(PHOTOSPIDER_ENABLE_EXECUTION_TEST_HOOKS)
#include "execution/execution_test_hooks.hpp"
#endif

namespace ps::execution_internal {
namespace {
// Boundary arrays retain the legacy vector ABI. Their declared element blocks
// and copied static members are admitted before construction; implementation-
// private STL nodes/headers remain in the documented legacy accounting scope.
Result<ResourceLease> legacy_capacity(const ResourceBudget& root,
                                      std::uint64_t bytes) {
  return root.reserve(ResourceCapacity::host(bytes, bytes));
}
}  // namespace

/** @brief Coordinator for compiler-visible structured edges and ordinary
 * inputs. Only this coordinator resolves DAG edges and performs mandatory I/O.
 * Required objects are retained independently of the optional completed cache;
 * failure never installs a complete object.
 * @details Each `Actor` retains its Start/Poll phase and pending submission
 * across driver traversals. The driver walks requested roots and producer
 * Actors, then submits ready callbacks in bounded waves. It waits for a wave to
 * retire before applying phase results and advancing the next frontier.
 * `execute_structured` allocates the coordinator and its
 * shared control block from the execution Root. The coordinator owns its copied
 * bindings and options and copies the snapshot identity into Root-managed
 * storage. It retains an optional plan owner and aliases that plan; without an
 * owner, it borrows the caller's plan. Queued work references coordinator-owned
 * state; every run drains its submitted callbacks before returning.
 *
 * The Root-owned `pending_tasks_` list retains queued and submitted Actors
 * until dispatch rejection or callback retirement. The driver fills a wave up
 * to `maximum_parallelism_`, waits for that wave to retire, then applies its
 * phase results serially before advancing the next dependency frontier. Queue
 * admission and stage leases remain separate from Root metadata used to build
 * Actors and pending records. CPU staged-tile coordination and GPU dispatch
 * still use their existing lane-specific paths rather than this Actor wave
 * scheduler.
 * Each poll's transfer, tile and native observations remain with its
 * submission until the driver retires it and merges them.
 * Poll timing starts before dispatch and ends at driver retirement,
 * so it includes queue and wave wait rather than measuring compute alone.
 *
 * Validated Needs can add computed producers to the pending frontier. A
 * completed tile Actor contributes to step retirement only after
 * its complete member tile is published; liveness then
 * follows requested roots and each step's selected input indices. The
 * coordinator clears a published object only when all
 * aliases have no remaining consumers or named-output pin and have no pending
 * work. Published Results and retained read windows keep their own
 * storage owners independently.
 *
 * `NeedPhase` retains a poll request and its supply state across advances. The
 * coordinator validates the complete envelope before supplying input, then
 * supplies at most one successful response per advance. Result object and
 * tensor requests use `need_object_input` and `result_object_step`; each input
 * has its own cursor and retains the producer Actor while waiting. If that
 * producer is not ready, the cursor pauses and resumes the same request later.
 * Tensor footprints are normalized once, and completed replies stay on the
 * consumer Actor for later requests in that Need. Cache replay uses the same
 * supply path and shares acquisition progress with ordinary consumers.
 * `Actor::driving` distinguishes active same-thread recursive dependency
 * servicing from an actor suspended on an upstream producer.
 */
class StructuredExecution final {
 public:
  StructuredExecution(const ExecutionPlan& plan,
                      std::vector<ExecutionBinding> bindings,
                      std::shared_ptr<OperationRegistry> operations,
                      ResourceBudget resources, const ExecutionOptions& options,
                      const CancellationToken& cancellation,
                      std::function<ErrorCode()> stop,
                      StructuredDispatch dispatch, std::string_view snapshot,
                      ResultCheckpoints* checkpoints, ResultCache* blocks,
                      std::shared_ptr<const ExecutionPlan> plan_owner,
                      std::uint32_t maximum_parallelism)
      : plan_owner_(std::move(plan_owner)),
        plan_(plan_owner_ ? *plan_owner_ : plan),
        bindings_(std::move(bindings)),
        operations_(std::move(operations)),
        resources_(std::move(resources)),
        payload_capture_(
            core_internal::ResourcePayloadScope::capture(resources_)),
        options_(options),
        maximum_parallelism_(maximum_parallelism),
        subscription_(resources_, cancellation,
                      std::move(options_.result_publication)),
        cancellation_(cancellation),
        stop_(std::move(stop)),
        dispatch_(std::move(dispatch)),
        templates_(plan_.structured_templates()),
        checkpoints_(snapshot.empty() ? nullptr : checkpoints),
        blocks_(blocks),
        block_epoch_(blocks ? blocks->epoch() : 0),
        snapshot_(ResourceAllocator<char>(resources_)),
        snapshot_input_(snapshot.data(), snapshot.size(),
                        ResourceAllocator<char>(resources_)),
        remaining_(options.maximum_dependency_work),
        cache_state_(resources_, options.maximum_dependency_cache_work),
        actors_(ResourceAllocator<std::weak_ptr<Actor>>(resources_)),
        actor_aliases_(ResourceAllocator<std::shared_ptr<Actor>>(resources_)),
        step_actors_(ResourceAllocator<ResourceVector<std::weak_ptr<Actor>>>(
            resources_)),
        open_units_(ResourceAllocator<std::uint64_t>(resources_)),
        actor_queries_(make_resource_map<ActorRegistration>(resources_)),
        acquisitions_(ResourceAllocator<AcquisitionMap>(resources_)),
        pending_tasks_(ResourceAllocator<PendingTask>(resources_)),
        remaining_consumers_(ResourceAllocator<std::uint64_t>(resources_)),
        named_pins_(ResourceAllocator<bool>(resources_)),
        finished_steps_(ResourceAllocator<bool>(resources_)),
        requested_roots_(ResourceAllocator<std::shared_ptr<Actor>>(resources_)),
        pending_root_requests_(ResourceAllocator<TileRequest>(resources_)),
        root_cursors_(ResourceAllocator<TileAddress>(resources_)),
        shared_actors_(ResourceAllocator<std::shared_ptr<Actor>>(resources_)),
        visibility_gates_(
            ResourceAllocator<std::shared_ptr<VisibilityGate>>(resources_)),
        complete_validation_admitted_(ResourceAllocator<bool>(resources_)),
        payload_bounds_(ResourceAllocator<
                        std::shared_ptr<plugin_internal::ResultPayloadBound>>(
            resources_)),
        pure_closure_(ResourceAllocator<bool>(resources_)),
        checkpoint_shareable_(ResourceAllocator<bool>(resources_)),
        result_cacheable_(ResourceAllocator<bool>(resources_)) {}

  ~StructuredExecution() { drain(); }
  /** @brief Waits for pending callback retirement before clearing actor state.
   * @note Used by the coordinator destructor, including exception cleanup.
   */
  void drain() noexcept {
    core_internal::ResourcePayloadScope payload_scope(resources_,
                                                      payload_capture_);
    wait_pending();
    for (const auto& weak : actors_)
      if (auto actor = weak.lock())
        drain_actor(*actor);
    for (const auto& weak : actors_)
      if (auto actor = weak.lock();
          actor && !actor->complete && actor->terminal == ErrorCode::Ok)
        retire(*actor, Status{ErrorCode::Cancelled, {}});
  }
  Result<ExecutionResult> run(const DemandQuery* requested) {
    const auto started = std::chrono::steady_clock::now();
    ResourceAllocationScope coordinator_scope(resources_);
    core_internal::ResourcePayloadScope payload_scope(resources_,
                                                      payload_capture_);
    Result<ExecutionResult> result(Status{ErrorCode::Internal, {}});
    try {
      auto admitted = plan_.resources().reference(resources_);
      if (!admitted.ok())
        return Result<ExecutionResult>(admitted.status());
      bindings_resources_ = admitted.take_value();
      result = run_body(requested);
    } catch (const std::bad_alloc&) {
      result =
          Result<ExecutionResult>(Status{ErrorCode::ResourceExhausted, {}});
    } catch (...) {
      result = Result<ExecutionResult>(Status{ErrorCode::OperationFailed, {}});
    }
    subscription_.close();
    wait_pending();
    const auto observer_failure = subscription_.status();
    if (!observer_failure.ok())
      return Result<ExecutionResult>(observer_failure);
    if (!result.ok())
      return result;
    diagnostics_.execute_us = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - started)
            .count());
    diagnostics_.managed_resources = resources_.statistics();
    auto completed = result.take_value();
    completed.diagnostics = std::move(diagnostics_);
#if defined(PHOTOSPIDER_ENABLE_EXECUTION_TEST_HOOKS)
    execution_testing::notify_final_result_ready();
#endif
    // Draining callbacks and retiring their captures can outlive run_body's
    // stop check. Recheck after complete assembly before publishing success.
    const auto stopped = active_stop();
    if (stopped != ErrorCode::Ok)
      return Result<ExecutionResult>(Status{stopped, {}});
    return Result<ExecutionResult>(std::move(completed));
  }

  bool empty_planned_request(const std::string& name) const {
    const auto& request = plan_.output_requests().at(name);
    return request.empty();
  }
  Result<ResultRef> empty_named_output(std::size_t index) {
    using Answer = Result<ResultRef>;
    const auto& step = plan_.steps()[index];
    auto charged = consume(templates_[index].size() + snapshot_.size() + 1);
    if (!charged.ok())
      return Answer(charged);
    content_internal::Sha256 identity;
    identity.text("photospider.empty-output.v1");
    identity.text(templates_[index]);
    identity.text(snapshot_);
    identity.integer(index);
    const auto key = identity.finish();
    ResultGrowthLimits empty_limits;
    auto started =
        ResultBuilder::start(resources_, *step.output_result_schema, key,
                             empty_limits, {}, plan_.tile_extent());
    if (!started.ok())
      return Answer(started.status());
    auto builder = started.take_value();
    auto empty = builder.seal_empty();
    if (!empty.ok())
      return empty;
    empty.value().bind_producer(step.node_id);
    auto ready = consume(1);
    if (!ready.ok())
      return Answer(ready);
    auto notified =
        subscription_.notify(index, step.result_ref(), empty.value(),
                             empty.value().descriptor().value().revision());
    return notified.ok() ? empty : Answer(notified);
  }
  Result<ExecutionResult> run_body(const DemandQuery* requested) {
    using Answer = Result<ExecutionResult>;
    auto status = initialize_execution_state(requested);
    if (!status.ok())
      return Answer(status);
    status = initialize_pure_closure();
    if (!status.ok())
      return Answer(status);
    status = initialize_cache_eligibility();
    if (!status.ok())
      return Answer(status);
    status = initialize_run_identity(requested);
    if (!status.ok())
      return Answer(status);
    status = collect_requested_roots(requested);
    if (!status.ok())
      return Answer(status);
    status = run_effect_roots(requested);
    if (!status.ok())
      return Answer(status);
    ExecutionResult result;
    status = collect_named_outputs(requested, result);
    if (!status.ok())
      return Answer(status);
    status = record_result_digest(result);
    if (!status.ok())
      return Answer(status);
    auto final_status = consume(0);
    if (!final_status.ok())
      return Answer(final_status);
    auto retired = tile_records_->retire_payloads();
    if (!retired.ok())
      return Answer(retired);
    result.dependencies.tiles_ = tile_records_;
    return Answer(std::move(result));
  }

 private:
  using StartPhase = StructuredStartPhase;
  using PollPhase = StructuredPollPhase;
  struct Actor;
  using Acquisition = StructuredInputAcquisition<Actor>;
  using AcquisitionMap = ResourceMap<std::shared_ptr<Acquisition>>;
  using NeedCursor = StructuredNeedCursor<Actor>;
  using NeedPhase = StructuredNeedPhase<Actor>;
  /** @brief One operation instance and its resumable execution state.
   * @details The Root-owned pending-task list retains this Actor while its
   * callback is queued or submitted. `waiting` retains an upstream Actor for
   * an incomplete dependency; `driving` guards active advancement, including
   * same-thread recursive dependency servicing. Aliases share the Actor's
   * publication and release accounting.
   */
  struct SharedCommit {
    TileRequest tiles;
    ResultRef result;
    std::shared_ptr<const TileEvidence> evidence;
    // Frozen with this delta; later producer work cannot change its provenance.
    bool fallback_taint = false;
  };
  struct SharedGrant {
    ResultRef result;
    std::shared_ptr<const TileEvidence> evidence;
    bool fallback_taint = false;
  };
  struct Actor final : StructuredActorLifecycle<NeedPhase> {
    explicit Actor(const PlanStep& step, const ResourceBudget& budget)
        : contract_step(&step),
          shared_results(
              std::less<std::uint32_t>{},
              ResourceAllocator<ResultObjectInputs::value_type>(budget)),
          query(*step.structured_metadata, step.parameters),
          key(ResourceAllocator<char>(budget)),
          aliases(ResourceAllocator<std::size_t>(budget)),
          tensors(std::less<std::pair<std::uint32_t, std::uint32_t>>{},
                  ResourceAllocator<ResultTensorInputs::value_type>(budget)),
          input_facts(budget),
          shared_commits(ResourceAllocator<SharedCommit>(budget)),
          shared_grants(make_resource_map<SharedGrant>(budget)),
          tile_record(budget),
          results(std::less<std::uint32_t>{},
                  ResourceAllocator<ResultObjectInputs::value_type>(budget)),
          descriptors(
              std::less<std::uint32_t>{},
              ResourceAllocator<ResultDescriptorInputs::value_type>(budget)),
          cache(budget),
          failure(std::allocate_shared<std::atomic<ErrorCode>>(
              ResourceAllocator<std::atomic<ErrorCode>>(budget),
              ErrorCode::Ok)),
          service_failure(std::allocate_shared<plugin_internal::FailureLatch>(
              ResourceAllocator<plugin_internal::FailureLatch>(budget))),
          node_id(step.node_id) {}
    ~Actor() { retire_owned_storage(); }
    ResourceLease lease;
    const PlanStep* contract_step;
    const PlanStep& contract() const { return *contract_step; }
    bool is_shared() const { return query.shared_stage != UINT32_MAX; }
    bool core_started = false, has_shared_dependencies = false;
    std::atomic<std::uint64_t> shared_work_remaining{0};
    ResultObjectInputs shared_results;
    ResultProgramQuery query;
    ResourceString key;
    ResourceVector<std::size_t> aliases;
    std::weak_ptr<Actor> self;
    std::size_t index = 0, registry_slot = 0;
    bool deferred_start = false;
    std::atomic<std::uint64_t> discovery_work_remaining{0};
    std::shared_ptr<Actor> waiting;
    Actor* wait_scan_next = nullptr;
    std::uint64_t wait_scan_epoch = 0;
    std::thread::id driver;
    ResultTensorInputs tensors;
    using InputFactsMap = ResultInputFacts;
    InputFactsMap input_facts;
    ResourceVector<SharedCommit> shared_commits;
    ResourceMap<SharedGrant> shared_grants;
    TileRequest shared_visible;

    TileRecord tile_record;
    std::shared_ptr<const TileEvidence> shared_evidence;
    std::shared_ptr<const TilePublication> tile_publication;
    ResultObjectInputs results;
    ResultDescriptorInputs descriptors;
    ResultRef published;
    StructuredActorCache cache;
    ResultFallbackState fallback;
    std::shared_ptr<std::atomic<ErrorCode>> failure;
    std::shared_ptr<plugin_internal::FailureLatch> service_failure;
    ErrorCode terminal = ErrorCode::Ok;
    std::uint64_t node_id = 0;
    std::uint64_t published_revision = 0, native_dispatches = 0;
    bool complete = false, liveness_complete = false, named_root = false;
    bool visibility_ready = false, gated = false;
  };
  struct VisibilityGate {
    explicit VisibilityGate(const ResourceBudget& root)
        : units(ResourceAllocator<std::shared_ptr<Actor>>(root)) {}
    std::size_t index = 0;
    std::uint32_t key = 0;
    TileRequest request;
    ResourceVector<std::shared_ptr<Actor>> units;
    std::uint64_t cursor = 0;
    bool prefix = false, busy = false, visible = false;
    Status failure;
  };
  struct PendingTask {
    std::shared_ptr<Actor> actor;
    Backend backend;
    bool tiles;
    CancellationToken token;
    std::function<Status(const StructuredServices&)> task;
    std::shared_ptr<ExecutionDiagnostics> observations;
  };
  struct ActorRegistration {
    std::weak_ptr<Actor> observed;
    std::shared_ptr<Actor> pending;
  };
  // Nested inline callbacks restore the matching coordinator services.
  struct CallbackContext {
    StructuredExecution* owner;
    Actor* actor;
    const StructuredServices* services;
    bool optional_replay;
    CallbackContext* previous;
    CallbackContext(StructuredExecution* execution, Actor* current,
                    const StructuredServices* supplied, bool optional)
        : owner(execution),
          actor(current),
          services(supplied),
          optional_replay(optional),
          previous(callback_) {
      callback_ = this;
    }
    ~CallbackContext() { callback_ = previous; }
  };
  inline static thread_local CallbackContext* callback_ = nullptr;
  CallbackContext* callback_context() const {
    for (auto* context = callback_; context; context = context->previous)
      if (context->owner == this)
        return context;
    return nullptr;
  }
  const StructuredServices* active_services() const {
    auto* context = callback_context();
    return context ? context->services : nullptr;
  }
  bool& work_mode() {
    auto* context = callback_context();
    return context ? context->optional_replay : optional_replay_;
  }
  bool recursively_busy(const Actor& actor) const {
    if (!actor.busy)
      return false;
    if (actor.driver != std::this_thread::get_id())
      return true;
    for (auto* context = callback_; context; context = context->previous)
      if (context->owner == this && context->actor == &actor)
        return true;
    return actor.driving || (!actor.pending && !actor.waiting);
  }
  std::shared_ptr<Actor> first_pending_actor() const {
    return pending_tasks_.empty() ? nullptr : pending_tasks_.front().actor;
  }
  bool running_pending() const {
    return std::any_of(pending_tasks_.begin(), pending_tasks_.end(),
                       [](const auto& task) {
                         return !task.actor->queued && task.actor->pending &&
                                !task.actor->pending->ready();
                       });
  }
  void forget_pending(Actor& actor) {
    pending_tasks_.erase(
        std::remove_if(
            pending_tasks_.begin(), pending_tasks_.end(),
            [&](const auto& task) { return task.actor.get() == &actor; }),
        pending_tasks_.end());
  }
  void wait_pending() {
    for (const auto& task : pending_tasks_)
      if (!task.actor->queued && task.actor->pending) {
        task.actor->pending->wait();
      }
  }
  Status external_stop_status(ErrorCode code) const noexcept {
    const auto reason = code == ErrorCode::Cancelled ? FailureReason::Cancelled
                        : code == ErrorCode::Stale ? FailureReason::StaleVersion
                                                   : FailureReason::None;
    const FailureDetail detail =
        reason == FailureReason::None
            ? FailureDetail{}
            : FailureDetail{FailureOrigin::Cancellation, FailureScope::Run};
    try {
#if defined(PHOTOSPIDER_ENABLE_EXECUTION_TEST_HOOKS)
      if (execution_testing::fail_failure_status_construction(
              execution_testing::FailureStatusConstructionPoint::ExternalStop))
        return Status{code, {}, reason, detail};
#endif
      return Status{code,
                    code == ErrorCode::Cancelled
                        ? "structured execution cancelled"
                        : "structured execution stopped before publication",
                    reason, detail};
    } catch (...) {
      return Status{code, {}, reason, detail};
    }
  }
  Status pending_protocol_failure() const {
    const auto bind_protocol = [](const Status& failure, std::uint64_t node) {
      if (failure.detail.origin != FailureOrigin::Protocol)
        return Status::success();
      core_internal::StoredFailure recorded;
      recorded.bind_producer(node);
      recorded.record(failure);
      return recorded.status();
    };
    const auto phase_failure = [&bind_protocol](const Actor& actor) {
      const Status* failure = nullptr;
      if (const auto* phase = std::get_if<StartPhase>(&actor.phase))
        failure = &phase->result.status();
      else if (const auto* phase = std::get_if<PollPhase>(&actor.phase))
        failure = &phase->result.status();
      return failure ? bind_protocol(*failure, actor.node_id)
                     : Status::success();
    };
    for (const auto& task : pending_tasks_) {
      auto failure = task.actor->service_failure->snapshot();
      if (failure.detail.origin == FailureOrigin::Protocol)
        return bind_protocol(failure, task.actor->node_id);
      // Phase results belong to workers until the submission retires. Sticky
      // service failures above are the only records safe to inspect earlier.
      if (task.actor->queued || !task.actor->pending ||
          !task.actor->pending->ready())
        continue;
      failure = task.actor->pending->finish();
      if (failure.detail.origin == FailureOrigin::Protocol)
        return bind_protocol(failure, task.actor->node_id);
      failure = phase_failure(*task.actor);
      if (failure.detail.origin == FailureOrigin::Protocol)
        return failure;
    }
    return Status::success();
  }
  Status pump_pending(bool wait) {
    Status stopped;
    if (!running_pending()) {
      std::uint32_t submitted = 0;
      for (auto& task : pending_tasks_) {
        if (!task.actor->queued)
          continue;
        if (submitted == maximum_parallelism_)
          break;
        auto accepted = dispatch_(task.backend, task.tiles, task.token,
                                  std::move(task.task));
        task.actor->queued = false;
        if (!accepted.ok()) {
          task.actor->pending->immediate = accepted.status();
          break;
        }
        auto stage = accepted.take_value();
        stage.observations = task.observations;
        task.actor->pending = std::move(stage);
        ++submitted;
      }
      diagnostics_.peak_active_tasks =
          std::max(diagnostics_.peak_active_tasks, submitted);
      const auto code = active_stop();
      if (code != ErrorCode::Ok)
        stopped = external_stop_status(code);
#if defined(PHOTOSPIDER_ENABLE_EXECUTION_TEST_HOOKS)
      if (submitted)
        execution_testing::notify_post_submit_observation();
#endif
    }
    while (running_pending()) {
      const auto code = active_stop();
      if (code != ErrorCode::Ok && stopped.code != code)
        stopped = external_stop_status(code);
      for (const auto& task : pending_tasks_)
        if (!task.actor->queued && task.actor->pending) {
          if (!task.actor->pending->ready())
            task.actor->pending->completion.wait_for(
                std::chrono::milliseconds(2));
        }
      if (!wait)
        break;
    }
    if (!stopped.ok() && !running_pending()) {
      auto protocol_failure = pending_protocol_failure();
      return protocol_failure.ok() ? stopped : protocol_failure;
    }
    return Status::success();
  }
  void drain_actor(Actor& actor) {
    if (actor.pending && !actor.queued) {
      actor.pending->wait();
      actor.pending.reset();
    }
    actor.pending.reset();
    actor.queued = false;
    actor.phase.emplace<std::monostate>();
    actor.waiting.reset();
    actor.busy = false;
    forget_pending(actor);
  }
  ErrorCode execution_stop(const CancellationToken& token) const {
    if (token.cancelled())
      return ErrorCode::Cancelled;
    if (!plan_.current())
      return ErrorCode::Stale;
    const auto observer_failure = subscription_.code();
    return observer_failure == ErrorCode::Ok ? stop_() : observer_failure;
  }
  ErrorCode active_stop() const { return execution_stop(cancellation_); }
  std::function<Status(const StructuredServices&)> prepare_task(
      std::function<Status()> task, Actor* actor,
      const std::shared_ptr<ExecutionDiagnostics>& observations) {
    const auto optional = work_mode();
    auto payload = core_internal::ResourcePayloadScope::capture(resources_);
    return [this, task = std::move(task), optional, actor, payload,
            observations](const StructuredServices& services) {
      core_internal::ResourcePayloadScope payload_scope(resources_, payload);
      CallbackContext context(this, actor, &services, optional);
      ResultNativeScope native_scope(
          services.allocator, services.native_input, resources_,
          options_.dependencies.sets,
          options_.dependencies.maximum_gpu_requests, services.native_storage,
          services.gpu == nullptr, [this](std::uint64_t units) {
            ResourceAllocationScope optional_scope(resources_);
            return checkpoint_cache_work(units);
          });
      auto status = task();
      observations->transfer_count += native_scope.transfers;
      observations->transfer_bytes += native_scope.transferred_bytes;
      observations->host_access_count += native_scope.host_accesses;
      if (services.observe)
        services.observe(*observations);
      return status;
    };
  }
  Result<StructuredSubmission> submit(std::function<Status()> task,
                                      Backend backend, bool tiles,
                                      Actor* actor = nullptr) {
    auto observations = std::allocate_shared<ExecutionDiagnostics>(
        ResourceAllocator<ExecutionDiagnostics>(resources_));
    auto submitted =
        dispatch_(backend, tiles, active_token(),
                  prepare_task(std::move(task), actor, observations));
    if (!submitted.ok())
      return submitted;
    auto stage = submitted.take_value();
    stage.observations = std::move(observations);
    return Result<StructuredSubmission>(std::move(stage));
  }
  Status merge_observations(const StructuredSubmission& pending) {
    if (!pending.observations)
      return Status::success();
    auto& source = *pending.observations;
    const auto add = [](std::uint64_t& target, std::uint64_t& value) {
      if (!core_internal::can_add(value, target))
        return false;
      target += std::exchange(value, 0);
      return true;
    };
    if (!add(diagnostics_.transfer_count, source.transfer_count) ||
        !add(diagnostics_.transfer_bytes, source.transfer_bytes) ||
        !add(diagnostics_.cpu_stage_count, source.cpu_stage_count) ||
        !add(diagnostics_.cpu_tile_callback_count,
             source.cpu_tile_callback_count) ||
        !add(diagnostics_.native_dispatch_count,
             source.native_dispatch_count) ||
        !add(diagnostics_.native_submission_count,
             source.native_submission_count) ||
        !add(diagnostics_.native_compute_us, source.native_compute_us) ||
        !add(diagnostics_.native_constant_bytes,
             source.native_constant_bytes) ||
        !add(diagnostics_.native_upload_hits, source.native_upload_hits) ||
        !add(diagnostics_.host_access_count, source.host_access_count))
      return Status{ErrorCode::ResourceExhausted,
                    "structured diagnostic overflow"};
    return Status::success();
  }
  Status await(const StructuredSubmission& pending) {
    struct Drain {
      const StructuredSubmission& pending;
      ~Drain() { pending.wait(); }
    } drain{pending};
    while (!pending.ready()) {
      pending.completion.wait_for(std::chrono::milliseconds(2));
    }
    auto finished = pending.finish();
    auto merged = merge_observations(pending);
    return finished.ok() ? merged : finished;
  }
  Status dispatch(std::function<Status()> task, Backend backend = Backend::Cpu,
                  bool tiles = false) {
    auto submitted = submit(std::move(task), backend, tiles);
    return submitted.ok() ? await(submitted.value()) : submitted.status();
  }
  Status admit_poll_diagnostics(const Actor& actor) {
    const auto output = plan_.steps()[actor.index].result_ref();
    const auto found = std::find_if(
        diagnostics_.operation_timings.begin(),
        diagnostics_.operation_timings.end(), [&](const auto& item) {
          return item.output == output && item.backend == actor.query.backend &&
                 item.shared_stage == actor.query.shared_stage;
        });
    if (found == diagnostics_.operation_timings.end()) {
      diagnostics_.operation_timings.push_back(
          {output, actor.query.backend, 0, ErrorCode::Ok, 0, 0});
      diagnostics_.operation_timings.back().shared_stage =
          actor.query.shared_stage;
      if (actor.query.prepared &&
          !actor.query.prepared->diagnostic_definitions().empty()) {
        auto& timing = diagnostics_.operation_timings.back();
        timing.reports.emplace(resources_);
        auto status = timing.reports->initialize(
            actor.query.prepared->diagnostic_definitions());
        if (!status.ok()) {
          diagnostics_.operation_timings.pop_back();
          return status;
        }
      }
    }
    return Status::success();
  }
  Status record_poll_diagnostics(const Actor& actor, const PollPhase& completed,
                                 ErrorCode outcome) {
    if (!completed.invoked)
      return Status::success();
    auto timing = std::find_if(
        diagnostics_.operation_timings.begin(),
        diagnostics_.operation_timings.end(), [&](const auto& item) {
          return item.output == plan_.steps()[actor.index].result_ref() &&
                 item.backend == actor.query.backend &&
                 item.shared_stage == actor.query.shared_stage;
        });
    if (timing == diagnostics_.operation_timings.end())
      return protocol_failure("missing admitted Result poll diagnostics");
    const auto elapsed = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - completed.started)
            .count());
    if (timing->invocation_count == UINT64_MAX ||
        !core_internal::can_add(elapsed, timing->duration_us) ||
        !core_internal::can_add(completed.native_dispatches,
                                timing->native_dispatch_count))
      return Status{ErrorCode::ResourceExhausted,
                    "Result poll diagnostic counter overflow"};
    if (completed.reports) {
      if (!timing->reports)
        return protocol_failure("missing admitted diagnostic accumulator");
      auto reported = timing->reports->merge(*completed.reports);
      if (!reported.ok())
        return reported;
    }
    timing->duration_us += elapsed;
    ++timing->invocation_count;
    timing->native_dispatch_count += completed.native_dispatches;
    timing->outcome = outcome;
    return Status::success();
  }
  void record_failed_poll(Actor& actor, const PollPhase& completed,
                          const Status& failure) {
    // Latch the callback's first cause before any fallible diagnostic merge.
    // The timing envelope was admitted before dispatch; recording cannot grow
    // its container after a callback has exhausted Root capacity.
    const auto first = actor.service_failure->record(failure);
    try {
      static_cast<void>(record_poll_diagnostics(actor, completed, first.code));
    } catch (...) {
      // Diagnostics cannot replace an already returned execution failure.
    }
  }
  Status submit_actor(const std::shared_ptr<Actor>& actor,
                      std::function<Status()> task,
                      Backend backend = Backend::Cpu, bool tiles = false) {
    if (actor->pending)
      return protocol_failure("structured actor already has a pending stage");
    if (std::holds_alternative<PollPhase>(actor->phase)) {
      auto admitted = admit_poll_diagnostics(*actor);
      if (!admitted.ok())
        return admitted;
    }
    if (!callback_context()) {
      auto observations = std::allocate_shared<ExecutionDiagnostics>(
          ResourceAllocator<ExecutionDiagnostics>(resources_));
      auto prepared = prepare_task(std::move(task), actor.get(), observations);
      pending_tasks_.push_back({actor, backend, tiles, active_token(),
                                std::move(prepared), observations});
      actor->pending.emplace();
      actor->queued = true;
      ++progress_;
      return Status::success();
    }
    auto submitted = submit(std::move(task), backend, tiles, actor.get());
    if (!submitted.ok())
      return submitted.status();
    actor->pending = submitted.take_value();
    return Status::success();
  }
  Status finish_actor(const std::shared_ptr<Actor>& actor) {
    if (!actor->has_submitted_stage())
      return protocol_failure("structured actor has no submitted stage");
    struct Reset {
      StructuredExecution& owner;
      Actor& actor;
      ~Reset() {
        actor.release_stage();
        if (!owner.callback_context())
          owner.forget_pending(actor);
      }
    } reset{*this, *actor};
    ++progress_;
    auto finished =
        actor->await_stage([this](const auto& stage) { return await(stage); });
    return finished;
  }
  Status start_actor(std::size_t, const std::shared_ptr<Actor>& current) {
    current->deferred_start = false;
    current->phase.emplace<StartPhase>();
    current->busy = true;
    current->driver = std::this_thread::get_id();
    struct Reset {
      Actor& actor;
      ~Reset() {
        if (!actor.pending) {
          actor.phase.emplace<std::monostate>();
          actor.busy = false;
        }
      }
    } reset{*current};
    return submit_actor(current, [this, current] {
      auto& actor = *current;
      auto& phase = std::get<StartPhase>(actor.phase);
      const auto& step = actor.contract();
      if (actor.query.backend == Backend::Gpu &&
          (!active_services() || !active_services()->native_gpu_available)) {
#if defined(PHOTOSPIDER_ENABLE_EXECUTION_TEST_HOOKS)
        execution_testing::notify_before_scheduler_failure(
            execution_testing::SchedulerFailurePoint::GpuBackendUnavailable);
#endif
        phase.result = Result<ResultContinuation>(
            Status{ErrorCode::BackendUnavailable,
                   "native GPU Result lane is unavailable"});
        return Status::success();
      }
      ResourceAllocationScope metadata_scope(resources_, &phase.sticky);
      auto allocator = resources_.allocator().limited(
          std::min(step.traits.outputs[0].continuation_bytes,
                   options_.dependencies.maximum_state_bytes),
          [full = actor.service_failure](const Status& status) {
            full->record(status);
          });
      actor.core_started = true;
      phase.result = operations_->start_result_compiled(
          step.operation, actor.query, allocator, actor.failure);
      return phase.sticky == ErrorCode::Ok ? Status::success()
                                           : Status{phase.sticky, {}};
    });
  }
  struct WorkMode final {
    bool& target;
    bool previous;
    WorkMode(bool& mode, bool optional) : target(mode), previous(mode) {
      target = optional;
    }
    ~WorkMode() { target = previous; }
  };
  struct CachePublicationTransaction final {
    explicit CachePublicationTransaction(std::recursive_mutex& gate)
        : lock(gate) {}
    std::unique_lock<std::recursive_mutex> lock;
  };
  Status consume(std::uint64_t count) {
    const auto stopped = active_stop();
    if (stopped != ErrorCode::Ok)
      return Status{stopped, {}};
    if (work_mode())
      return checkpoint_cache_work(count);
    auto admitted = admit_run_work(count);
    return admitted.ok() ? resources_.consume({count}) : admitted;
  }
  Status admit_run_work(std::uint64_t count) {
    auto available = remaining_.load(std::memory_order_relaxed);
    do {
      if (count > available)
        return Status{ErrorCode::ResourceExhausted,
                      "structured Run work exhausted",
                      FailureReason::WorkLimit,
                      {FailureOrigin::Resource, FailureScope::Run}};
    } while (!remaining_.compare_exchange_weak(available, available - count,
                                               std::memory_order_relaxed));
    return Status::success();
  }
  Status admit_actor_work(Actor& actor, std::uint64_t count) {
    if (!actor.is_shared())
      return Status::success();
    auto available =
        actor.shared_work_remaining.load(std::memory_order_relaxed);
    do {
      if (count > available)
        return Status{ErrorCode::ResourceExhausted, "shared stage work limit",
                      FailureReason::WorkLimit};
    } while (!actor.shared_work_remaining.compare_exchange_weak(
        available, available - count, std::memory_order_relaxed));
    return Status::success();
  }
  Status operation_work(Actor& actor, std::uint64_t count) {
    const auto stopped = active_stop();
    if (stopped != ErrorCode::Ok)
      return Status{stopped, {}};
    auto admitted = admit_actor_work(actor, count);
    return admitted.ok() ? resources_.consume({count}) : admitted;
  }
  CancellationToken active_token() const { return cancellation_; }
  FootprintLimits set_limits() {
    auto limits = options_.dependencies.sets;
    limits.cancellation = active_token();
    limits.consume_work = [&](std::uint64_t count) { return consume(count); };
    return limits;
  }
  Status initialize_execution_state(const DemandQuery* requested) {
    if (!options_.maximum_result_window_bytes ||
        options_.maximum_result_window_bytes > INT64_MAX)
      return protocol_failure("invalid structured I/O window");
    auto traversal = consume(plan_.steps().size() + 1);
    if (!traversal.ok())
      return traversal;

    auto live = initialize_liveness(requested);
    if (!live.ok())
      return live;
    actor_aliases_.resize(plan_.steps().size());
    open_units_.resize(plan_.steps().size());
    for (std::size_t i = 0; i < plan_.steps().size(); ++i) {
      step_actors_.emplace_back(
          ResourceAllocator<std::weak_ptr<Actor>>(resources_));
      acquisitions_.push_back(
          make_resource_map<std::shared_ptr<Acquisition>>(resources_));
    }
    return Status::success();
  }
  Status initialize_pure_closure() {
    pure_closure_.resize(plan_.steps().size());
    for (std::size_t i = 0; i < plan_.steps().size(); ++i) {
      const auto& step = plan_.steps()[i];
      const auto& included = step.traits.outputs[0].input_indices;
      auto purity_work = consume(
          1 + step.inputs.size() * (1 + (included ? included->size() : 0)));
      if (!purity_work.ok())
        return purity_work;
      pure_closure_[i] =
          step.traits.deterministic && step.traits.side_effect_free;
      for (std::uint32_t port = 0; port < step.inputs.size(); ++port) {
        if (included && std::find(included->begin(), included->end(), port) ==
                            included->end())
          continue;
        if (const auto* producer =
                std::get_if<PlanStepInput>(&step.inputs[port]))
          pure_closure_[i] =
              pure_closure_[i] && pure_closure_[producer->step_index];
      }
    }
    return Status::success();
  }
  Status initialize_cache_eligibility() {
    checkpoint_shareable_.resize(plan_.steps().size());
    result_cacheable_.resize(plan_.steps().size());
    for (std::size_t i = 0; i < plan_.steps().size(); ++i) {
      const auto& step = plan_.steps()[i];
      const auto& included = step.traits.outputs[0].input_indices;
      auto purity_work = checkpoint_cache_work(
          1 + step.inputs.size() * (1 + (included ? included->size() : 0)));
      if (!purity_work.ok()) {
        if (purity_work.code != ErrorCode::ResourceExhausted)
          return purity_work;
        std::fill(checkpoint_shareable_.begin(), checkpoint_shareable_.end(),
                  false);
        break;
      }
      checkpoint_shareable_[i] =
          step.traits.deterministic && step.traits.side_effect_free;
      result_cacheable_[i] = checkpoint_shareable_[i] && step.traits.cacheable;
      for (std::uint32_t port = 0; port < step.inputs.size(); ++port) {
        if (included && std::find(included->begin(), included->end(), port) ==
                            included->end())
          continue;
        if (const auto* producer =
                std::get_if<PlanStepInput>(&step.inputs[port])) {
          checkpoint_shareable_[i] =
              checkpoint_shareable_[i] &&
              checkpoint_shareable_[producer->step_index];
          result_cacheable_[i] =
              result_cacheable_[i] && result_cacheable_[producer->step_index];
        }
      }
    }
    return Status::success();
  }
  Status initialize_run_identity(const DemandQuery* requested) {
    subscription_.initialize(plan_.steps().size());
    diagnostics_.operation_timings = decltype(diagnostics_.operation_timings)(
        ResourceAllocator<OperationTiming>(resources_));
    diagnostics_.operation_timings.reserve(plan_.steps().size());
    if (templates_.size() != plan_.steps().size() ||
        std::any_of(templates_.begin(), templates_.end(),
                    [](const auto& key) { return key.empty(); }))
      return Status{ErrorCode::ResourceExhausted,
                    "structured identity work exhausted"};
    auto snapshot_work = consume(snapshot_input_.size() + 48);
    if (!snapshot_work.ok())
      return snapshot_work;
    if (!snapshot_input_.empty())
      snapshot_.assign(snapshot_input_.data(), snapshot_input_.size());
    if (snapshot_.empty()) {
      static std::atomic<std::uint64_t> sequence{1};
      auto id = sequence.load();
      do {
        if (id == UINT64_MAX)
          return Status{ErrorCode::ResourceExhausted, {}};
      } while (!sequence.compare_exchange_weak(id, id + 1));
      char digits[24];
      auto converted = std::to_chars(digits, digits + sizeof(digits), id);
      snapshot_.append("/structured-run-");
      snapshot_.append(digits,
                       static_cast<std::size_t>(converted.ptr - digits));
    }
    tile_records_ = std::allocate_shared<TileRecords>(
        ResourceAllocator<TileRecords>(resources_), resources_);
    if (requested) {
      auto key =
          demand_key(*requested, plan_,
                     options_.dependencies.sets.maximum_boxes, resources_);
      if (!key.ok())
        return key.status();
    }
    diagnostics_.plan_digest =
        ResourceString(plan_.digest().value.data(), plan_.digest().value.size(),
                       ResourceAllocator<char>(resources_));
    return Status::success();
  }
  Status collect_requested_roots(const DemandQuery* requested) {
    payload_bounds_.resize(plan_.steps().size());
    complete_validation_admitted_.resize(plan_.steps().size(), false);
    for (std::size_t i = 0; i < plan_.steps().size(); ++i) {
      const auto& bound =
          plan_.steps()[i].traits.outputs[0].maximum_output_payload_bytes;
      if (bound)
        payload_bounds_[i] =
            std::allocate_shared<plugin_internal::ResultPayloadBound>(
                ResourceAllocator<plugin_internal::ResultPayloadBound>(
                    resources_),
                *bound, true);
    }
    pending_root_requests_.resize(plan_.steps().size());
    root_cursors_.resize(plan_.steps().size());
    for (const auto& named : plan_.outputs()) {
      if (requested && !requested->count(named.first))
        continue;
      const auto index = named.second;
      auto selected = (requested ? requested->at(named.first)
                                 : plan_.output_requests().at(named.first))
                          .resolve(plan_.steps()[index].tile_grids);
      if (!selected.ok())
        return selected.status();
      auto joined =
          pending_root_requests_[index].unite(selected.value(), resources_);
      if (!joined.ok())
        return joined.status();
      pending_root_requests_[index] = joined.take_value();
    }
    // Root aliases jointly authorize work for their producer. Admit the union
    // before collecting any alias; each returned Result keeps its own
    // selection.
    for (std::size_t index = 0; index < pending_root_requests_.size();
         ++index) {
      if (pending_root_requests_[index].empty())
        continue;
      auto admitted =
          admit_validation_scope(index, pending_root_requests_[index]);
      if (!admitted.ok())
        return admitted;
    }
    liveness_ready_ = true;
    return Status::success();
  }
  Status collect_named_outputs(const DemandQuery* requested,
                               ExecutionResult& result) {
    result.results = make_resource_map<ResultRef>(resources_);
    for (const auto& named : plan_.outputs()) {
      if (requested && !requested->count(named.first))
        continue;
      auto status = consume(1);
      if (!status.ok())
        return status;
      const auto& step = plan_.steps()[named.second];
      if (!step.output_result_schema)
        return protocol_failure("operation output requires Result schema");
      const bool empty_request = requested ? requested->at(named.first).empty()
                                           : empty_planned_request(named.first);
      auto object =
          empty_request
              ? empty_named_output(named.second)
              : result_object(named.second,
                              requested
                                  ? requested->at(named.first)
                                  : plan_.output_requests().at(named.first));
      if (!object.ok())
        return object.status();
      result.results.emplace(named.first, object.take_value());
    }
    return Status::success();
  }
  Result<bool> admit_result_digest(const ExecutionResult& result) {
    if (subscription_.has_callback() || !options_.maximum_result_digest_samples)
      return Result<bool>(false);
    auto digest_samples = options_.maximum_result_digest_samples;
    auto digest_work = remaining_.load(std::memory_order_relaxed);
    for (const auto& named : result.results) {
      auto facts = named.second.descriptor();
      if (!facts.ok())
        return Result<bool>(facts.status());
      const auto& schema = named.second.schema();

      for (std::uint32_t slot = 0; slot < schema.tensors.size(); ++slot) {
        auto count = facts.value().tensor_coverage(slot).element_count();
        if (!count.ok() || count.value() > digest_samples ||
            count.value() > digest_work)
          return Result<bool>(false);
        digest_samples -= count.value();
        digest_work -= count.value();
      }
    }
    return Result<bool>(true);
  }
  Status record_result_digest(const ExecutionResult& result) {
    auto admitted = admit_result_digest(result);
    if (!admitted.ok())
      return admitted.status();
    if (!admitted.value())
      return Status::success();
    content_internal::Sha256 digest;
    digest.text("photospider.result-content.v1");
    for (const auto& named : result.results) {
      digest.text(named.first);
      const auto& object = named.second;
      auto schema = object.schema().managed_canonical(resources_);
      if (!schema.ok())
        return schema.status();
      digest.text(schema.value());
      auto facts = object.descriptor();
      if (!facts.ok())
        return facts.status();

      for (std::uint32_t slot = 0; slot < object.schema().tensors.size();
           ++slot) {
        const auto& coverage = facts.value().tensor_coverage(slot);
        digest.integer(coverage.boxes().size());
        for (const auto& box : coverage.boxes())
          for (const auto& axis : box.dimensions()) {
            digest.integer(axis.offset);
            digest.integer(axis.extent);
          }
        const auto width = TensorSpan::element_size(
            object.schema().tensors[slot].descriptor.element_type);
        for (const auto& box : coverage.boxes()) {
          auto window =
              object.acquire_tensor(facts.value(), slot, box, active_token());
          if (!window.ok())
            return window.status();
          auto part =
              Footprint::from_regions(coverage.shape(), {box}, set_limits());
          if (!part.ok())
            return part.status();
          auto scanned = part.value().visit(
              [&](const auto& coordinate) {
                auto active = consume(1);
                if (!active.ok())
                  return active;
                auto run = window.value().row_run(coordinate);
                if (!run.ok())
                  return run.status();
                digest.bytes(run.value().data, width);
                return Status::success();
              },
              remaining_, active_token());
          if (!scanned.ok())
            return scanned;
        }
      }
    }
    auto active = consume(0);
    if (!active.ok())
      return active;
    diagnostics_.result_digest =
        ResourceString(digest.finish(), ResourceAllocator<char>(resources_));
    return Status::success();
  }
  Status initialize_liveness(const DemandQuery* requested) {
    const auto count = plan_.steps().size();
    remaining_consumers_.assign(count, 0);
    named_pins_.assign(count, false);
    finished_steps_.assign(count, false);
    ResourceVector<bool> reachable(count, false,
                                   ResourceAllocator<bool>(resources_));
    ResourceVector<std::size_t> pending{
        ResourceAllocator<std::size_t>(resources_)};
    for (const auto& named : plan_.outputs())
      if (!requested || requested->count(named.first)) {
        named_pins_[named.second] = true;
        pending.push_back(named.second);
      }
    for (std::size_t i = 0; i < count; ++i)
      if (wants_effects(requested) &&
          (!plan_.steps()[i].traits.side_effect_free ||
           std::any_of(
               plan_.steps()[i].prepared->traits().shared_stages.begin(),
               plan_.steps()[i].prepared->traits().shared_stages.end(),
               [](const auto& stage) { return stage.mandatory; }))) {
        named_pins_[i] = true;
        pending.push_back(i);
      }
    while (!pending.empty()) {
      auto index = pending.back();
      pending.pop_back();
      if (reachable[index])
        continue;
      reachable[index] = true;
      const auto& step = plan_.steps()[index];
      const auto& included = step.traits.outputs[0].input_indices;
      auto charged = consume(1 + step.inputs.size() *
                                     (1 + (included ? included->size() : 0)));
      if (!charged.ok())
        return charged;
      for (std::uint32_t port = 0; port < step.inputs.size(); ++port) {
        if (included && std::find(included->begin(), included->end(), port) ==
                            included->end())
          continue;
        if (const auto* producer =
                std::get_if<PlanStepInput>(&step.inputs[port])) {
          ++remaining_consumers_[producer->step_index];
          pending.push_back(producer->step_index);
        }
      }
    }
    return Status::success();
  }
  Status release_unused_result(std::size_t index) {
    if (remaining_consumers_[index] || named_pins_[index])
      return Status::success();
    auto charged = consume(step_actors_[index].size());
    if (!charged.ok())
      return charged;
    for (const auto& weak : step_actors_[index]) {
      auto current = weak.lock();
      if (!current || !current->complete || current->pending)
        continue;
      if (current->tile_publication) {
        auto retired = tile_records_->retire_payload(current->tile_record.unit,
                                                     current->tile_publication);
        if (!retired.ok())
          return retired;
        current->tile_publication.reset();
      }
      release_actor_registration(*current);
      current->published = {};
    }
    actor_aliases_[index].reset();
    acquisitions_[index].clear();
    return Status::success();
  }
  Status complete_liveness(Actor& actor) {
    if (!actor.is_shared() && !actor.visibility_ready)
      return Status::success();
    if (!actor.complete || actor.is_shared())
      return Status::success();
    if (!actor.liveness_complete) {
      actor.liveness_complete = true;
      --open_units_[actor.index];
    }
    return finish_steps(actor.index);
  }
  Status finish_steps(std::size_t first) {
    if (!liveness_ready_)
      return Status::success();
    ResourceVector<std::size_t> ready{
        ResourceAllocator<std::size_t>(resources_)};
    ready.push_back(first);
    while (!ready.empty()) {
      const auto index = ready.back();
      ready.pop_back();
      if (finished_steps_[index] || open_units_[index] ||
          !pending_root_requests_[index].empty() || remaining_consumers_[index])
        continue;
      finished_steps_[index] = true;
      const auto& step = plan_.steps()[index];
      const auto& included = step.traits.outputs[0].input_indices;
      auto charged = consume(1 + step.inputs.size() *
                                     (1 + (included ? included->size() : 0)));
      if (!charged.ok())
        return charged;
      auto released = release_unused_result(index);
      if (!released.ok())
        return released;
      for (std::uint32_t port = 0; port < step.inputs.size(); ++port) {
        if (included && std::find(included->begin(), included->end(), port) ==
                            included->end())
          continue;
        if (const auto* producer =
                std::get_if<PlanStepInput>(&step.inputs[port])) {
          auto& remaining = remaining_consumers_[producer->step_index];
          if (remaining && !--remaining)
            ready.push_back(producer->step_index);
        }
      }
    }
    return Status::success();
  }
  void release_actor_registration(Actor& actor) {
    if (actor.is_shared())
      return;
    // Completed units remain available for later adaptive Needs in this Run.
    // A step retires only after every possible consumer step has finished.
    if (actor.terminal == ErrorCode::Ok &&
        (!finished_steps_[actor.index] || named_pins_[actor.index]))
      return;
    auto found = actor_queries_.find(actor.key);
    if (found != actor_queries_.end())
      found->second.pending.reset();
  }
  Status retire(Actor& actor, const Status& incoming) {
    auto failure = actor.service_failure->record(incoming);
    if (!failure.detail.node_id && !failure.detail.input_id)
      failure.detail.node_id = actor.node_id;
    if (failure.detail.scope == FailureScope::Unspecified)
      failure.detail.scope = FailureScope::Group;
    actor.service_failure->enrich(failure);
    actor.terminal = failure.code;
    if (actor.published.valid())
      actor.published.retire_producer(failure);
    actor.continuation = {};
    actor.waiting.reset();
    actor.results.clear();
    actor.shared_results.clear();
    actor.descriptors.clear();
    actor.tensors.clear();
    actor.input_facts.clear();

    release_actor_registration(actor);
    return failure;
  }
  void append_actor_scope(ResourceString& key, std::size_t index,
                          TileAddress address) const {
    content_internal::Sha256 demand_key;
    demand_key.text("structured-unit.v1");
    demand_key.integer(index);
    demand_key.integer(plan_.steps()[index].output_index);
    demand_key.integer(address.tensor_slot);
    demand_key.integer(address.tile);
    demand_key.integer(plan_.tile_extent());
    const auto suffix = demand_key.finish();
    key.push_back(':');
    key.append(suffix.data(), suffix.size());
  }
  Result<std::shared_ptr<Actor>> shared_actor(std::size_t index,
                                              std::uint32_t stage) {
    using Answer = Result<std::shared_ptr<Actor>>;
    if (index >= plan_.steps().size() ||
        stage >= plan_.steps()[index].shared_stages.size())
      return Answer(protocol_failure("unknown shared stage"));
    const auto node = plan_.steps()[index].node_id;
    auto charged = consume(shared_actors_.size() + plan_.steps().size() + 1);
    if (!charged.ok())
      return Answer(charged);
    for (const auto& existing : shared_actors_)
      if (existing->node_id == node && existing->query.shared_stage == stage)
        return Answer(existing);
    // Original-node identity is independent of the requesting selected output.
    for (std::size_t i = 0; i < index; ++i)
      if (plan_.steps()[i].node_id == node) {
        index = i;
        break;
      }
    const auto& contract = *plan_.steps()[index].shared_stages[stage];
    const auto& declared = contract.prepared->traits().shared_stages[stage];
    if (contract.backend == Backend::Gpu &&
        plan_.execution_mode() == ExecutionMode::CpuExact)
      return Answer(
          Status{ErrorCode::BackendUnavailable,
                 "GPU shared stage is unavailable in CPU execution mode"});
    auto capacity = ResourceCapacity::host(sizeof(Actor), sizeof(Actor));
    capacity[ResourceKind::Entries] = 1;
    auto admitted = resources_.reserve(capacity);
    if (!admitted.ok())
      return Answer(admitted.status());
    auto created = std::shared_ptr<Actor>(new Actor(contract, resources_));
    created->lease = admitted.take_value();
    created->index = index;
    created->self = created;
    created->aliases.push_back(index);
    created->query.shared_stage = stage;
    created->tile_record.unit = {index, contract.output_index, 0, 0};
    created->query.output_index = contract.output_index;
    created->query.backend = contract.backend;
    created->query.prepared = contract.prepared;
    created->query.resources = bindings_resources_;
    created->query.tile_extent = plan_.tile_extent();
    created->query.page_bytes = options_.maximum_result_window_bytes;
    created->query.cancellation = active_token();
    created->query.snapshot_identity = snapshot_;
    created->shared_work_remaining = declared.maximum_work;
    created->discovery_work_remaining = options_.dependencies.maximum_work;
    content_internal::Sha256 key;
    key.text("photospider.shared-stage.v1");
    key.text(snapshot_);
    key.integer(node);
    key.text(declared.key);
    created->key =
        ResourceString(key.finish(), ResourceAllocator<char>(resources_));
    created->query.semantic_key = created->key;
    created->cache.disabled = true;
    created->registry_slot = actors_.size();
    actors_.push_back(created);
    shared_actors_.push_back(
        created);  // Insert before start, including failure.
    auto started = initialize_actor(index, created);
    return started.ok() ? Answer(created) : Answer(started);
  }
  Status connect_wait(Actor& consumer, const std::shared_ptr<Actor>& producer) {
    std::lock_guard<std::recursive_mutex> lock(callback_metadata_mutex_);
    if (wait_scan_epoch_ == UINT64_MAX)
      return Status{ErrorCode::ResourceExhausted,
                    "wait graph traversal identity exhausted"};
    const auto epoch = ++wait_scan_epoch_;
    Actor* pending = nullptr;
    // No scheduling or callbacks occur during this scan. Intrusive scratch is
    // owned by admitted Actors, so waiting on a producer needs no allocation.
    const auto enqueue = [&](Actor* node) -> Status {
      if (!node)
        return Status::success();
      auto work = consume(1);
      if (!work.ok())
        return work;
      if (node == &consumer)
        return Status{ErrorCode::Cycle, "cyclic tile/shared-stage dependency"};
      if (node->wait_scan_epoch != epoch) {
        node->wait_scan_epoch = epoch;
        node->wait_scan_next = pending;
        pending = node;
      }
      return Status::success();
    };
    auto status = enqueue(producer.get());
    if (!status.ok())
      return status;
    while (pending) {
      auto* node = pending;
      pending = node->wait_scan_next;
      status = consume(1);
      if (!status.ok())
        return status;
      status = enqueue(node->waiting.get());
      if (!status.ok())
        return status;
      if (const auto* phase = std::get_if<NeedPhase>(&node->phase)) {
        const auto append = [&](const NeedCursor& cursor) -> Status {
          auto added = enqueue(cursor.producer.get());
          if (!added.ok())
            return added;
          return cursor.acquisition
                     ? enqueue(cursor.acquisition->producer.get())
                     : Status::success();
        };
        status = append(phase->cursor);
        if (!status.ok())
          return status;
        for (const auto& request : phase->cursor.requests) {
          status = append(*request);
          if (!status.ok())
            return status;
        }
      }
    }
    consumer.waiting = producer;
    return Status::success();
  }
  Result<std::shared_ptr<Actor>> actor(std::size_t index,
                                       TileAddress address = {},
                                       bool defer_start = false) {
    using Answer = Result<std::shared_ptr<Actor>>;
    if (index >= plan_.steps().size())
      return Answer(protocol_failure("invalid result step"));
    const auto& step = plan_.steps()[index];
    const auto slot = address.tensor_slot;
    const auto tile = address.tile;
    if (slot >= step.tile_grids.size() ||
        tile >= step.tile_grids[slot].tile_count())
      return Answer(protocol_failure("invalid member computation unit"));
    auto region = step.tile_grids[slot].region(tile);
    if (!region.ok())
      return Answer(region.status());
    auto samples = Footprint::from_regions(
        step.output_result_schema->tensors[slot].sample_shape(),
        {region.take_value()}, set_limits());
    if (!samples.ok())
      return Answer(samples.status());
    std::optional<Footprint> tensor_outputs(samples.take_value());
    auto key_work = consume(templates_[index].size() + snapshot_.size() + 1);
    if (!key_work.ok())
      return Answer(key_work);
    ResourceString key{ResourceAllocator<char>(resources_)};
    key.reserve(templates_[index].size() + snapshot_.size() + 1);
    key.append(templates_[index].data(), templates_[index].size());
    key.push_back(':');
    key.append(snapshot_);
    append_actor_scope(key, index, address);
    auto existing = actor_queries_.find(key);
    if (existing != actor_queries_.end()) {
      auto shared = existing->second.observed.lock();
      if (!shared) {
        actor_queries_.erase(existing);
      } else {
        if (std::find(shared->aliases.begin(), shared->aliases.end(), index) ==
            shared->aliases.end())
          shared->aliases.push_back(index);
        actor_aliases_[index] = shared;
        return Answer(std::move(shared));
      }
    }
    WorkMode computation(work_mode(), false);
    const std::uint64_t bytes = sizeof(Actor);
    auto capacity = ResourceCapacity::host(bytes, bytes);
    capacity[ResourceKind::Entries] = 1;
    auto lease = resources_.reserve(capacity);
    if (!lease.ok())
      return Answer(lease.status());
    if (!step.structured_metadata)
      return Answer(protocol_failure("missing compiled stage metadata"));
    auto created = std::shared_ptr<Actor>(new Actor(step, resources_));
    created->discovery_work_remaining = options_.dependencies.maximum_work;
    created->lease = lease.take_value();
    created->index = index;
    created->aliases.push_back(index);
    created->self = created;
    created->deferred_start = defer_start;
    created->query.snapshot_identity = snapshot_;
    created->query.tensor_outputs = std::move(tensor_outputs);
    created->query.tensor_slot = slot;
    created->query.tile_unit = tile;
    created->tile_record.unit = {index, step.output_index, slot, tile};
    created->query.tile_extent = plan_.tile_extent();
    created->query.backend = step.backend;
    created->query.output_index = step.output_index;
    created->query.resources = bindings_resources_;
    created->query.prepared = step.prepared;
    const auto* visibility = step.output_result_schema->publication_scope(slot);
    created->gated =
        (visibility &&
         visibility->policy != ResultPublicationPolicy::IndependentChunks) ||
        step.prepared->requires_complete_validation(true, step.output_index);
    created->cache.disabled = created->gated;

    created->key = std::move(key);
    created->query.semantic_key = created->key;
    created->query.page_bytes = options_.maximum_result_window_bytes;
    created->query.cancellation = active_token();
    created->registry_slot = actors_.size();
    actors_.push_back(created);
    step_actors_[index].push_back(created);
    ++open_units_[index];
    actor_queries_.emplace(created->key, ActorRegistration{created, created});
    actor_aliases_[index] = created;
    created->busy = true;
    created->driver = std::this_thread::get_id();
    struct Initialized {
      Actor& actor;
      ~Initialized() {
        if (!actor.pending && !actor.waiting)
          actor.busy = false;
      }
    } initialized{*created};
    actor_aliases_[index] = created;
    auto started = initialize_actor(index, created);
    return started.ok() ? Answer(std::move(created)) : Answer(started);
  }
  /** @brief Restores or starts a prepared actor on the current driver thread.
   * @details Reuses an eligible completed result or submits the actor's initial
   * callback; its phase remains attached to the actor for later advancement.
   */
  Status initialize_actor(std::size_t index,
                          const std::shared_ptr<Actor>& created) {
    auto reused = (created->is_shared() || created->gated)
                      ? Result<bool>(false)
                      : reuse_cached_result(index, *created);
    if (!reused.ok())
      return retire(*created, reused.status());
    created->initialized = true;
    if (reused.value())
      return Status::success();
    auto charged = consume(1);
    if (!charged.ok())
      return retire(*created, charged);
    if (created->deferred_start)
      return Status::success();
    auto submitted = start_actor(index, created);
    return submitted.ok() ? submitted : retire(*created, submitted);
  }
  Status complete_start(std::size_t index,
                        const std::shared_ptr<Actor>& current,
                        StartPhase started) {
    const auto& step = current->contract();
    if ((!current->is_shared() || !current->core_started) &&
        ResultFallbackState::can_retry_start(
            step.traits, current->query.backend, started, *current->failure,
            *current->service_failure) &&
        active_stop() == ErrorCode::Ok) {
      auto capacity = legacy_capacity(
          resources_, 2 * (step.operation.size() +
                           started.result.status().message.size() + 64));
      if (!capacity.ok())
        return retire(*current, capacity.status());
      diagnostics_.fallback_reasons.push_back(step.operation + ": " +
                                              started.result.status().message);
      const auto elapsed = static_cast<std::uint64_t>(
          std::chrono::duration_cast<std::chrono::microseconds>(
              std::chrono::steady_clock::now() - started.started)
              .count());
      diagnostics_.operation_timings.push_back(
          {step.result_ref(), Backend::Gpu, elapsed,
           ErrorCode::BackendUnavailable, 1, 0});
      diagnostics_.operation_timings.back().shared_stage =
          current->query.shared_stage;
      current->fallback.taint = true;
      current->query.backend = Backend::Cpu;
      auto retry = consume(1);
      if (!retry.ok())
        return retire(*current, retry);
      auto submitted = start_actor(index, current);
      return submitted.ok() ? submitted : retire(*current, submitted);
    }
    if (started.sticky == ErrorCode::InvalidArgument)
      return retire(*current,
                    Status{started.sticky,
                           {},
                           FailureReason::UnauthorizedRead,
                           {FailureOrigin::Protocol, FailureScope::Group}});
    if (!started.dispatched.ok())
      return retire(*current, started.dispatched);
    if (!started.result.ok())
      return retire(*current, started.result.status());
    current->continuation = started.result.take_value();
    return Status::success();
  }
  Status input_failure(const PlanInput& input, Status status) const {
    if (status.ok())
      return status;
    if (status.detail.origin == FailureOrigin::Unspecified &&
        (status.code == ErrorCode::TypeMismatch ||
         status.code == ErrorCode::InvalidArgument ||
         status.code == ErrorCode::OperationFailed))
      status.detail.origin = status.code == ErrorCode::TypeMismatch
                                 ? FailureOrigin::Schema
                                 : FailureOrigin::Domain;
    if (!status.detail.input_id && !status.detail.node_id) {
      if (const auto* external = std::get_if<PlanWorkflowInput>(&input))
        status.detail.input_id =
            plan_.input_declarations()[external->declaration_index].id;
      else
        status.detail.node_id =
            plan_.steps()[std::get<PlanStepInput>(input).step_index].node_id;
    }
    return status;
  }
  Status checkpoint_allowed(std::size_t index, Actor& actor) {
    auto work = consume(0);
    if (!work.ok())
      return work;
    const auto failure = actor.service_failure->snapshot();
    if (!failure.ok())
      return failure;
    if (actor.failure->load() != ErrorCode::Ok)
      return Status{actor.failure->load(), {}};
    if (actor.is_shared())
      return protocol_failure(
          "shared stages do not expose optional checkpoint or block caches");
    const auto& traits = plan_.steps()[index].traits;
    return traits.deterministic && traits.side_effect_free
               ? Status::success()
               : protocol_failure("checkpoint requires a pure Result program");
  }
  Result<ResourceString> checkpoint_key(std::size_t, const Actor& actor) {
    auto charged = checkpoint_cache_work(actor.key.size() + 16);
    if (!charged.ok())
      return Result<ResourceString>(charged);
    ResourceString key(actor.key);
    key.append("/checkpoint");
    return Result<ResourceString>(std::move(key));
  }
  std::shared_ptr<ResultCheckpointScope> checkpoint_scope(
      const ResourceString& key) {
    return cache_state_.scope(key, checkpoints_,
                              options_.dependencies.sets.maximum_boxes);
  }
  Status checkpoint_cache_work(std::uint64_t units) {
    CacheWork work(*this);
    return cache_state_.charge(units, callback_metadata_mutex_, work,
                               diagnostics_.dependency_cache_work);
  }
  Result<std::optional<ResultCheckpoint>> checkpoint_before(
      std::size_t index, Actor& actor, std::uint32_t phase,
      std::uint64_t before, ResourceVector<std::uint64_t>& association) {
    using Answer = Result<std::optional<ResultCheckpoint>>;
    std::unique_lock<std::recursive_mutex> lock(callback_metadata_mutex_);
    auto status = checkpoint_allowed(index, actor);
    if (!status.ok())
      return Answer(status);
    if (!phase)
      return Answer(protocol_failure("zero checkpoint phase"));
    if (!cache_state_.remaining() || !checkpoint_shareable_[index] ||
        actor.has_shared_dependencies || actor.fallback.taint)
      return Answer(std::optional<ResultCheckpoint>{});
    Result<ResourceString> key(Status{ErrorCode::Internal, {}});
    ResultCheckpoint found;
    {
      ResourceAllocationScope optional_scope(resources_);
      try {
        key = checkpoint_key(index, actor);
        if (!key.ok())
          return key.status().code == ErrorCode::ResourceExhausted
                     ? Answer(std::optional<ResultCheckpoint>{})
                     : Answer(key.status());
        auto scope = checkpoint_scope(key.value());
        if (scope)
          found = scope->find(phase, before);
      } catch (const std::bad_alloc&) {
        return Answer(std::optional<ResultCheckpoint>{});
      }
    }
    if (!found.valid())
      return Answer(std::optional<ResultCheckpoint>{});
    auto witness =
        std::static_pointer_cast<const ResultCheckpointWitness>(found.witness_);
    if (witness->scope != key.value() || found.phase() != phase ||
        found.sequence() > before || !found.state().owned_by(resources_))
      return Answer(protocol_failure("Result checkpoint scope mismatch"));
    if (witness->weight > cache_state_.remaining())
      return Answer(std::optional<ResultCheckpoint>{});
    status = checkpoint_cache_work(witness->weight);
    if (!status.ok())
      return Answer(status);
    if (!(witness->tiles.unit == actor.tile_record.unit))
      return Answer(protocol_failure("checkpoint computation unit changed"));
    // Restore is optional until every fallible allocation is admitted. Keep
    // the actor and the callback's association unchanged on a capacity miss.
    ResourceAllocationScope optional_restore(resources_);
    const auto refused = [](const Status& failed) -> Answer {
      if (failed.code == ErrorCode::ResourceExhausted &&
          (failed.reason == FailureReason::None ||
           failed.reason == FailureReason::CapacityLimit))
        return Answer(std::optional<ResultCheckpoint>{});
      return Answer(failed);
    };
    try {
      auto record_work = consume(actor.tile_record.inputs.size());
      if (!record_work.ok())
        return Answer(record_work);
      TileRecord restored = actor.tile_record;
      for (const auto& dependency : witness->tiles.inputs) {
        status = restored.include(dependency, resources_);
        if (!status.ok())
          return refused(status);
      }
      for (const auto& evidence : witness->tiles.shared) {
        status = restored.inherit(evidence, resources_);
        if (!status.ok())
          return refused(status);
      }
      status = consume(actor.input_facts.size());
      if (!status.ok())
        return Answer(status);
      auto restored_facts = actor.input_facts;
      for (const auto& entry : witness->facts) {
        auto& revision = restored_facts[entry.first];
        revision = std::max(revision, entry.second);
      }
      status = operation_work(actor, restored_facts.size());
      if (!status.ok())
        return Answer(status);
      ResourceVector<std::uint64_t> refreshed{
          ResourceAllocator<std::uint64_t>(resources_)};
      refreshed.reserve(restored_facts.size());
      for (const auto& input : restored_facts)
        refreshed.push_back(input.first.second);
      // The table stages its own allocations and commits atomically. Once it
      // succeeds, only same-Root moves remain before exposing the checkpoint.
      status = tile_records_->restore_ancestry(
          0, witness->tile_ancestry,
          [&](std::uint64_t n) { return consume(n); });
      if (!status.ok())
        return refused(status);
      actor.tile_record = std::move(restored);
      actor.input_facts = std::move(restored_facts);
      association = std::move(refreshed);
      lock.unlock();
#if defined(PHOTOSPIDER_ENABLE_EXECUTION_TEST_HOOKS)
      execution_testing::notify_checkpoint_borrowed();
#endif
      return Answer(std::optional<ResultCheckpoint>{std::move(found)});
    } catch (const std::bad_alloc&) {
      return Answer(std::optional<ResultCheckpoint>{});
    }
  }

  Status checkpoint_publish(std::size_t index, Actor& actor,
                            std::uint32_t phase, std::uint64_t sequence,
                            const ResultRef& state) {
    std::unique_lock<std::recursive_mutex> lock(callback_metadata_mutex_);
    auto status = checkpoint_allowed(index, actor);
    if (!status.ok())
      return status;
    if (!phase || !state.owned_by(resources_))
      return protocol_failure("invalid Result checkpoint state");
    auto descriptor = state.descriptor();
    if (!descriptor.ok())
      return descriptor.status();
    if (!cache_state_.remaining() || !checkpoint_shareable_[index] ||
        actor.has_shared_dependencies || actor.fallback.taint)
      return Status::success();
    // Optional retention has its own allocation fence: a missed cache
    // admission cannot replace a successfully computed state with failure.
    ResourceAllocationScope optional_scope(resources_);
    try {
      auto key = checkpoint_key(index, actor);
      if (!key.ok())
        return key.status().code == ErrorCode::ResourceExhausted
                   ? Status::success()
                   : key.status();
      auto scope = checkpoint_scope(key.value());
      if (!scope)
        return Status::success();
      const auto maximum = options_.dependencies.sets.maximum_boxes;
      std::uint64_t weight = 1 + key.value().size();
      const auto add = [&](std::uint64_t units) {
        return core_internal::checked_add(weight, units, &weight, maximum);
      };
      if (!add(actor.input_facts.size() * 3))
        return Status::success();
      status = checkpoint_cache_work(weight);
      if (!status.ok())
        return status.code == ErrorCode::ResourceExhausted ? Status::success()
                                                           : status;
      auto tile_ancestry = tile_records_->capture_ancestry(
          actor.tile_record,
          [&](const PlanStepInput& input, std::uint32_t output,
              std::uint32_t slot) -> Result<TileRecords::ProducerUnits> {
            if (input.step_index >= plan_.steps().size())
              return Result<TileRecords::ProducerUnits>(
                  protocol_failure("invalid checkpoint producer"));
            const auto& source = plan_.steps()[input.step_index];
            if (source.output_index != output ||
                slot >= source.tile_grids.size() ||
                !source.output_result_schema)
              return Result<TileRecords::ProducerUnits>(
                  protocol_failure("checkpoint producer output changed"));
            return Result<TileRecords::ProducerUnits>(
                TileRecords::ProducerUnits{
                    source.tile_grids[slot].tile_count()});
          },
          [&](std::uint64_t n) { return checkpoint_cache_work(n); });
      if (!tile_ancestry.ok())
        return tile_ancestry.status().code == ErrorCode::ResourceExhausted
                   ? Status::success()
                   : tile_ancestry.status();
      ResourceVector<std::shared_ptr<const TileEvidence>> evidence_queue{
          ResourceAllocator<std::shared_ptr<const TileEvidence>>(resources_)};
      std::set<std::uint64_t, std::less<std::uint64_t>,
               ResourceAllocator<std::uint64_t>>
          evidence_seen{std::less<std::uint64_t>{},
                        ResourceAllocator<std::uint64_t>(resources_)};
      const auto add_record = [&](const TileRecord& record) {
        if (!add(1 + record.inputs.size() + record.shared.size()) ||
            !checkpoint_cache_work(1 + record.inputs.size() +
                                   record.shared.size())
                 .ok())
          return false;
        for (const auto& dependency : record.inputs)
          if (!add(dependency.tiles.intervals().size()) ||
              !checkpoint_cache_work(dependency.tiles.intervals().size()).ok())
            return false;
        for (const auto& node : record.shared)
          if (node && evidence_seen.insert(node->identity).second)
            evidence_queue.push_back(node);
        return true;
      };
      if (!add_record(actor.tile_record))
        return Status::success();
      for (const auto& record : tile_ancestry.value())
        if (!add_record(*record))
          return Status::success();
      for (std::size_t i = 0; i < evidence_queue.size(); ++i) {
        const auto node = evidence_queue[i];
        if (!add(1 + node->inputs.size() + node->inherited.size()) ||
            !checkpoint_cache_work(1 + node->inputs.size() +
                                   node->inherited.size())
                 .ok())
          return Status::success();
        for (const auto& input : node->inputs)
          if (!add(input.tiles.intervals().size()) ||
              !checkpoint_cache_work(input.tiles.intervals().size()).ok())
            return Status::success();
        for (const auto& child : node->inherited)
          if (child && evidence_seen.insert(child->identity).second)
            evidence_queue.push_back(child);
      }
      auto witness = std::allocate_shared<ResultCheckpointWitness>(
          ResourceAllocator<ResultCheckpointWitness>(resources_), resources_);
      witness->tiles = actor.tile_record;
      witness->tile_ancestry = tile_ancestry.take_value();
      witness->scope = key.take_value();
      witness->facts = actor.input_facts;
      witness->weight = weight;
      ResultCheckpoint checkpoint;
      checkpoint.phase_ = phase;
      checkpoint.sequence_ = sequence;
      checkpoint.state_ = state;
      checkpoint.witness_ = std::move(witness);
      if (scope->put(checkpoint, weight)) {
#if defined(PHOTOSPIDER_ENABLE_EXECUTION_TEST_HOOKS)
        lock.unlock();
        execution_testing::notify_checkpoint_published();
#endif
      }
      return Status::success();
    } catch (const std::bad_alloc&) {
      return Status::success();
    }
  }
  Result<std::string> shared_block_contract(std::size_t index,
                                            const ResultProgramPhase& phase) {
    auto available = cache_state_.remaining();
    Status admission;
    contract_internal::DependencyTemplateDigest hash(
        &available, [&](std::uint64_t count) {
          // Admit each hash operation before performing it. A refused optional
          // allocation or work reservation leaves no uncharged digest work.
          try {
            admission = checkpoint_cache_work(count);
          } catch (const std::bad_alloc&) {
            admission = Status{ErrorCode::ResourceExhausted, {}};
          }
          return admission.ok();
        });
    hash.text("photospider.result-shared-block-contract.v2");
    hash.integer(static_cast<std::uint32_t>(plan_.execution_mode()));
    hash.integer(static_cast<std::uint32_t>(phase.query.backend));
    hash.text(plan_.steps()[index].operation);
    contract_internal::append_traits(&hash, phase.query.prepared->traits());
    hash.integer(phase.query.prepared->resource_ids().size());
    for (const auto& id : phase.query.prepared->resource_ids()) {
      hash.integer(id.byte_length);
      for (auto byte : id.sha256)
        hash.integer(byte);
    }
    hash.integer(phase.query.parameters.size());
    for (const auto& parameter : phase.query.parameters) {
      hash.text(parameter.first);
      hash.integer(parameter.second.index());
      if (const auto* value = std::get_if<int64_t>(&parameter.second)) {
        hash.integer(static_cast<uint64_t>(*value));
      } else if (const auto* value = std::get_if<double>(&parameter.second)) {
        uint64_t bits;
        std::memcpy(&bits, value, sizeof(bits));
        hash.integer(bits);
      } else if (const auto* value = std::get_if<bool>(&parameter.second)) {
        hash.integer(*value);
      } else {
        hash.text(std::get<std::string>(parameter.second));
      }
    }
    hash.integer(phase.query.inputs.size());
    for (const auto& input : phase.query.inputs) {
      hash.integer(input.result_schema != nullptr);
      if (input.result_schema)
        hash.text(input.result_schema->canonical());
    }
    auto key = hash.finish();
    if (!admission.ok() && admission.code != ErrorCode::ResourceExhausted)
      return Result<std::string>(admission);
    if (key.empty())
      cache_state_.disable();
    return Result<std::string>(std::move(key));
  }
  struct BlockCacheProbe {
    std::string key;
    ResourceLease key_lease;
    TensorSpan cached;
  };
  Status probe_block_cache(std::size_t index, Actor& actor,
                           const ResultProgramPhase& phase, std::uint32_t kind,
                           std::uint64_t begin, std::uint64_t end,
                           std::uint64_t mode, const ResultRef& incoming,
                           BlockCacheProbe& probe) {
    Status status;
    // Object fields remain mandatory coordinator access. Optional
    // content hashing never loads them or starts production inside a callback.
    if (!(blocks_ && checkpoint_shareable_[index] &&
          !actor.has_shared_dependencies && !actor.fallback.taint &&
          plan_.steps()[index].traits.cacheable && cache_state_.remaining() &&
          actor.results.empty() && actor.descriptors.empty()))
      return Status::success();
    ResourceAllocationScope optional_scope(resources_);
    try {
      // Only the block-state namespace is shared. Actor, checkpoint and
      // completed-result identities remain selected-output contracts.
      const bool shared =
          plan_.steps()[index].traits.share_blocks_across_outputs;
      std::string common;
      if (shared) {
        auto identity = shared_block_contract(index, phase);
        if (!identity.ok())
          return identity.status();
        common = identity.take_value();
      }
      const auto& contract = shared ? common : templates_[index];
      std::uint64_t cost = 1 + contract.size();
      const auto add = [&](std::uint64_t n) {
        return core_internal::checked_add(cost, n, &cost);
      };
      const auto set_cost = [&](const Footprint& samples,
                                std::uint64_t lookup = 0) {
        status = checkpoint_cache_work(1 + samples.shape().size() +
                                       samples.boxes().size());
        if (!status.ok())
          return false;
        auto count = samples.element_count();
        std::uint64_t scale = 0;
        return count.ok() &&
               core_internal::checked_add(lookup, 9 + samples.shape().size(),
                                          &scale) &&
               core_internal::can_multiply(count.value(), scale) &&
               add(count.value() * scale) &&
               add(1 + samples.shape().size() +
                   samples.boxes().size() * (1 + 2 * samples.shape().size()));
      };
      const auto schema_cost = [&](const SchemaTemplate& schema) {
        status = checkpoint_cache_work(1 + schema.tensors.size() +
                                       schema.domain.size());
        if (!status.ok())
          return false;
        return add(schema.canonical_size());
      };
      auto incoming_facts = incoming.descriptor();
      status = checkpoint_cache_work(1 + actor.tensors.size());
      bool admitted = !contract.empty() && status.ok() && incoming_facts.ok() &&
                      schema_cost(incoming.schema()) &&
                      set_cost(incoming_facts.value().tensor_coverage(0));
      for (const auto& tensor : actor.tensors) {
        admitted = admitted && tensor.second.payload_authorized_ &&
                   schema_cost(tensor.second.result_.schema()) &&
                   set_cost(tensor.second.coverage());
      }
      if (!admitted)
        return Status::success();
      status = checkpoint_cache_work(cost);
      if (!status.ok())
        return status.code == ErrorCode::ResourceExhausted ? Status::success()
                                                           : status;
      content_internal::Sha256 hash;
      hash.text("photospider.result-block.v1");
      hash.text(contract);
      hash.integer(kind);
      hash.integer(begin);
      hash.integer(end);
      hash.integer(mode);
      auto schema = incoming.schema().managed_canonical(resources_);
      if (!schema.ok()) {
        status = schema.status();
      } else {
        hash.text(schema.value());
        status = append_block_samples(
            &hash, incoming_facts.value().tensor_coverage(0),
            incoming.schema().tensors[0].descriptor.element_type,
            [&](const auto& at, void* bytes, std::size_t width) {
              return incoming.read_tensor(incoming_facts.value(), 0, at, bytes,
                                          width, active_token());
            },
            phase.consume_work, active_token());
      }
      hash.integer(actor.tensors.size());
      for (const auto& tensor : actor.tensors) {
        if (!status.ok())
          break;
        hash.integer(tensor.first.first);
        hash.integer(tensor.first.second);
        auto schema =
            tensor.second.result_.schema().managed_canonical(resources_);
        if (!schema.ok()) {
          status = schema.status();
          break;
        }
        hash.text(schema.value());
        status = append_block_samples(
            &hash, tensor.second.coverage(),
            tensor.second.spec().descriptor.element_type,
            [&](const auto& at, void* bytes, std::size_t width) {
              return tensor.second.read_granted(at, bytes, width,
                                                active_token());
            },
            phase.consume_work, active_token());
      }
      if (status.ok()) {
        auto capacity = legacy_capacity(resources_, 256);
        if (!capacity.ok()) {
          status = capacity.status();
        } else {
          probe.key_lease = capacity.take_value();
          probe.key = "result-block/" + hash.finish();
          probe.cached = blocks_->get(probe.key, block_epoch_);
        }
        if (probe.cached.valid())
          ++diagnostics_.block_cache_hits;
        else
          ++diagnostics_.block_cache_misses;
      }
      if (!status.ok() && status.code != ErrorCode::ResourceExhausted)
        return status;
    } catch (const std::bad_alloc&) {
      probe.key.clear();
    }
    return Status::success();
  }
  Status retain_block_cache(const BlockCacheProbe& probe,
                            const ResultRef& result,
                            const ResultProgramPhase& phase,
                            bool native_computed) {
    if (probe.key.empty() || !native_computed)
      return Status::success();
    ResourceAllocationScope optional_scope(resources_);
    try {
      auto count = result.schema().tensors[0].sample_count();
      if (!count.ok() || !core_internal::can_multiply(count.value(), 9))
        return Status::success();
      auto status = checkpoint_cache_work(count.value() * 9);
      if (!status.ok())
        return status.code == ErrorCode::ResourceExhausted ? Status::success()
                                                           : status;
      auto packed = pack_block_state(result, phase);
      if (!packed.ok())
        return packed.status().code == ErrorCode::ResourceExhausted
                   ? Status::success()
                   : packed.status();
      blocks_->put(probe.key, packed.value(), block_epoch_,
                   phase.query.backend == Backend::Gpu);
    } catch (const std::bad_alloc&) {
    }
    return Status::success();
  }
  Result<ResultRef> evaluate_block(
      std::size_t index, Actor& actor, const ResultProgramPhase& phase,
      std::uint32_t kind, std::uint64_t begin, std::uint64_t end,
      std::uint64_t mode, const ResultRef& incoming,
      const std::function<Result<ResultRef>()>& compute) {
    std::unique_lock<std::recursive_mutex> lock(callback_metadata_mutex_);
    using Answer = Result<ResultRef>;
    auto status = checkpoint_allowed(index, actor);
    if (!status.ok())
      return Answer(status);
    status = consume(1);
    if (!status.ok())
      return Answer(status);
    if (!kind || begin >= end || !compute)
      return Answer(protocol_failure("invalid Result block request"));
    status = result_block_state(incoming, resources_);
    if (!status.ok())
      return Answer(status);
    BlockCacheProbe probe;
    status = probe_block_cache(index, actor, phase, kind, begin, end, mode,
                               incoming, probe);
    if (!status.ok())
      return Answer(status);
    if (probe.cached.valid()) {
      const auto& spec = incoming.schema().tensors[0];
      if (probe.cached.descriptor().element_type !=
              spec.descriptor.element_type ||
          probe.cached.descriptor().shape != spec.sample_shape())
        return Answer(protocol_failure("cached Result block state mismatch"));
      // The copy contains no old source owners, associations or witness. The
      // current actor retains its current successful Needs independently.
      auto unpacked = [&]() -> Answer {
        ResourceAllocationScope optional_restore(resources_);
        try {
          return unpack_block_state(probe.cached, incoming.schema(), phase);
        } catch (const std::bad_alloc&) {
          return Answer(Status{ErrorCode::ResourceExhausted, {}});
        }
      }();
      if (unpacked.ok())
        return unpacked;
      if (unpacked.status().code != ErrorCode::ResourceExhausted ||
          (unpacked.status().reason != FailureReason::None &&
           unpacked.status().reason != FailureReason::CapacityLimit))
        return unpacked;
      // Optional reconstruction may need more metadata than compute(). Drop
      // the candidate before computing, without poisoning the actor's latch.
      probe = {};
      --diagnostics_.block_cache_hits;
      ++diagnostics_.block_cache_misses;
    }
    status = checkpoint_allowed(index, actor);
    if (!status.ok())
      return Answer(status);
    const auto native_before =
        active_services() && active_services()->gpu_dispatches
            ? active_services()->gpu_dispatches()
            : 0;
    lock.unlock();
    auto result = core_internal::invoke_callback([&] { return compute(); });
    lock.lock();
    if (!result.ok())
      return result;
    status = checkpoint_allowed(index, actor);
    if (!status.ok())
      return Answer(status);
    status = result_block_state(result.value(), resources_);
    if (!status.ok() || !result.value().schema().same_schema(incoming.schema()))
      return Answer(
          status.ok() ? protocol_failure("computed Result block state mismatch")
                      : status);
    const bool native_computed =
        actor.query.backend != Backend::Gpu ||
        (active_services() && active_services()->gpu_dispatches &&
         active_services()->gpu_dispatches() > native_before);
    status = retain_block_cache(probe, result.value(), phase, native_computed);
    if (!status.ok())
      return Answer(status);
    return result;
  }
  Status admit_validation_scope(std::size_t index, const TileRequest& request) {
    const auto& step = plan_.steps()[index];
    if (complete_validation_admitted_[index])
      return Status::success();
    if (!step.prepared->requires_complete_validation(true, step.output_index))
      return Status::success();
    // Completeness is measured after the schema's declared publication scope
    // expands the work, not after adding any validator-driven input Need.
    std::array<bool, 16> complete{};
    for (std::uint32_t slot = 0; slot < step.tile_grids.size(); ++slot) {
      const auto found = request.members.find(slot);
      complete[slot] =
          !step.tile_grids[slot].tile_count() ||
          (found != request.members.end() &&
           found->second.size() == step.tile_grids[slot].tile_count());
    }
    for (const auto& scope : step.output_result_schema->publication_scopes) {
      if (scope.policy == ResultPublicationPolicy::CompleteBundle) {
        const bool triggered = std::any_of(
            scope.members.begin(), scope.members.end(), [&](auto slot) {
              const auto found = request.members.find(slot);
              return found != request.members.end() && !found->second.empty();
            });
        if (triggered)
          for (auto slot : scope.members)
            complete[slot] = true;
      } else if (scope.policy == ResultPublicationPolicy::StablePrefix) {
        const auto slot = scope.members.front();
        const auto found = request.members.find(slot);
        if (found != request.members.end() && !found->second.empty() &&
            found->second.intervals().back().end ==
                step.tile_grids[slot].tile_count())
          complete[slot] = true;
      }
    }
    for (std::uint32_t slot = 0; slot < step.tile_grids.size(); ++slot) {
      if (!complete[slot])
        return {ErrorCode::TypeMismatch,
                "complete output validator requires complete requested or "
                "declared publication coverage"};
    }
    complete_validation_admitted_[index] = true;
    return Status::success();
  }
  Result<bool> advance_visibility(Actor& requested) {
    using Answer = Result<bool>;
    const auto& step = requested.contract();
    const auto& schema = *step.output_result_schema;
    const bool complete_validator =
        step.prepared->requires_complete_validation(true, step.output_index);
    const auto* scope = schema.publication_scope(requested.query.tensor_slot);
    if (complete_validator && !complete_validation_admitted_[requested.index])
      return Answer(Status{
          ErrorCode::TypeMismatch,
          "complete output validator requires complete publication coverage"});
    const auto key = complete_validator ? 16U : scope->members.front();
    std::shared_ptr<VisibilityGate> gate;
    for (const auto& found : visibility_gates_)
      if (found->index == requested.index && found->key == key) {
        gate = found;
        break;
      }
    if (!gate) {
      gate = std::allocate_shared<VisibilityGate>(
          ResourceAllocator<VisibilityGate>(resources_), resources_);
      gate->index = requested.index;
      gate->key = key;
      gate->prefix = !complete_validator &&
                     scope->policy == ResultPublicationPolicy::StablePrefix;
      for (std::uint32_t slot = 0; slot < schema.tensors.size(); ++slot) {
        if (!complete_validator &&
            std::find(scope->members.begin(), scope->members.end(), slot) ==
                scope->members.end())
          continue;
        auto tiles =
            TileSet::range(resources_, 0, step.tile_grids[slot].tile_count());
        if (!tiles.ok())
          return Answer(tiles.status());
        gate->request.members.emplace(slot, tiles.take_value());
      }
      visibility_gates_.push_back(gate);
    }
    if (!gate->failure.ok())
      return Answer(gate->failure);
    if (gate->visible || requested.visibility_ready)
      return Answer(true);
    if (gate->busy)
      return Answer(
          Status{ErrorCode::Cycle, "cyclic publication scope dependency"});
    gate->busy = true;
    struct Reset {
      bool& value;
      ~Reset() { value = false; }
    } reset{gate->busy};
    const auto fail = [&](Status status) -> Answer {
      gate->failure = status;
      return Answer(status);
    };
    const auto count = gate->prefix ? requested.query.tile_unit + 1
                                    : unit_count(step, gate->request);
    if (gate->cursor < count) {
      const auto address = unit_index(step, gate->request, gate->cursor);
      auto producer = actor(requested.index, address);
      if (!producer.ok())
        return fail(producer.status());
      auto current = producer.take_value();
      if (current->terminal != ErrorCode::Ok)
        return fail(current->service_failure->snapshot());
      if (!current->complete) {
        auto connected = connect_wait(requested, current);
        if (!connected.ok())
          return fail(connected);
        auto advanced = advance(requested.index, *current);
        if (!advanced.ok())
          return fail(advanced);
        if (!current->complete)
          return Answer(false);
      }
      requested.waiting.reset();
      if (gate->prefix) {
        auto installed =
            tile_records_->install(0, current->tile_record, current->published,
                                   &current->tile_publication);
        if (!installed.ok())
          return fail(installed);
        current->visibility_ready = true;
        auto notified = notify(*current);
        if (notified.ok())
          notified = complete_liveness(*current);
        if (!notified.ok())
          return fail(notified);
      }
      // Prefix units must remain discoverable after later units replace the
      // per-output alias; a backwards Need must reuse its certified actor.
      gate->units.push_back(current);
      ++gate->cursor;
      ++progress_;
      if (gate->cursor < count)
        return Answer(false);
    }
    if (gate->prefix)
      return Answer(requested.visibility_ready);
    ResourceVector<ResultRef> payloads{
        ResourceAllocator<ResultRef>(resources_)};
    ResourceVector<TileRecords::PendingPublication> entries{
        ResourceAllocator<TileRecords::PendingPublication>(resources_)};
    for (const auto& unit : gate->units) {
      payloads.push_back(unit->published);
      entries.push_back(
          {&unit->tile_record, unit->published, &unit->tile_publication});
    }
    auto aggregate = assemble_units(requested.index, gate->request, payloads);
    if (!aggregate.ok())
      return fail(aggregate.status());
    auto descriptor = aggregate.value().descriptor();
    if (!descriptor.ok())
      return fail(descriptor.status());
    auto checked = plugin_internal::validate_result_contract(
        *step.prepared, true, step.output_index, aggregate.value(),
        descriptor.value(), nullptr, resources_, active_token(),
        [&](auto n) { return operation_work(requested, n); },
        options_.maximum_result_window_bytes, ErrorCode::OperationFailed);
    if (!checked.ok())
      return fail(checked);
    // Every member is certified by the same aggregate validator. Retain its
    // union once, so any input invalidating the bundle invalidates all members.
    TileRecord bundle(resources_);
    bundle.unit = requested.tile_record.unit;
    bool fallback_taint = false;
    for (const auto& unit : gate->units) {
      fallback_taint |= unit->fallback.taint ||
                        unit->query.backend != unit->contract().backend;
      for (const auto& input : unit->tile_record.inputs) {
        checked = bundle.include(input, resources_);
        if (!checked.ok())
          return fail(checked);
      }
      for (const auto& evidence : unit->tile_record.shared) {
        checked = bundle.inherit(evidence, resources_);
        if (!checked.ok())
          return fail(checked);
      }
    }
    auto evidence = bundle.freeze(resources_);
    if (!evidence.ok())
      return fail(evidence.status());
    for (const auto& unit : gate->units) {
      TileRecord record(resources_);
      record.unit = unit->tile_record.unit;
      checked = record.inherit(evidence.value(), resources_);
      if (!checked.ok())
        return fail(checked);
      unit->tile_record = std::move(record);
      unit->fallback.taint |= fallback_taint;
    }
    checked = active_token().cancelled() ? Status{ErrorCode::Cancelled, {}}
                                         : Status::success();
    if (!checked.ok())
      return fail(checked);
    checked = tile_records_->install_batch(0, entries.data(), entries.size());
    if (!checked.ok())
      return fail(checked);
    gate->visible = true;
    for (const auto& unit : gate->units)
      unit->visibility_ready = true;
    for (const auto& unit : gate->units) {
      checked = notify(*unit);
      if (checked.ok())
        checked = complete_liveness(*unit);
      if (!checked.ok())
        return fail(checked);
    }
    return Answer(true);
  }
  bool satisfied(const Actor& actor) const {
    return actor.published.valid() && actor.complete;
  }
  Result<std::optional<ResultRef>> result_object_step(
      std::size_t index, const std::shared_ptr<Actor>& current) {
    using Answer = Result<std::optional<ResultRef>>;
    if (recursively_busy(*current))
      return Answer(Status{ErrorCode::Cycle, {}});
    if (!satisfied(*current)) {
      if (current->terminal != ErrorCode::Ok) {
        release_actor_registration(*current);
        return Answer(current->service_failure->snapshot());
      }
      if (current->complete)
        return Answer(protocol_failure("requested result field is absent"));
      auto status = [&] {
        WorkMode computation(work_mode(), false);
        return advance(index, *current);
      }();
      if (!status.ok())
        return Answer(status);
      if (current->terminal != ErrorCode::Ok) {
        release_actor_registration(*current);
        return Answer(current->service_failure->snapshot());
      }
      if (!satisfied(*current))
        return Answer(std::optional<ResultRef>{});
    }
    if (!current->is_shared() && !current->visibility_ready) {
      auto visible = advance_visibility(*current);
      if (!visible.ok())
        return Answer(visible.status());
      if (!visible.value())
        return Answer(std::optional<ResultRef>{});
    }
    auto notified = notify(*current);
    if (current->complete)
      release_actor_registration(*current);
    return notified.ok() ? Answer(std::optional<ResultRef>(current->published))
                         : Answer(notified);
  }
  bool wants_effects(const DemandQuery* requested) const {
    if (requested) {
      return std::any_of(
          requested->begin(), requested->end(),
          [](const auto& named) { return !named.second.empty(); });
    }
    return true;
  }
  Status run_effect_roots(const DemandQuery* requested) {
    if (!wants_effects(requested))
      return Status::success();
    for (std::size_t i = 0; i < plan_.steps().size(); ++i) {
      const auto& declarations =
          plan_.steps()[i].prepared->traits().shared_stages;
      bool ran = false;
      for (std::uint32_t ordinal = 0; ordinal < declarations.size();
           ++ordinal) {
        if (!declarations[ordinal].mandatory)
          continue;
        ran = true;
        auto acquired = shared_actor(i, ordinal);
        if (!acquired.ok())
          return acquired.status();
        auto current = acquired.take_value();
        for (;;) {
          const auto before = progress_;
          ++traversal_;
          auto ready = result_object_step(current->index, current);
          if (!ready.ok())
            return ready.status();
          if (ready.value())
            break;
          if (progress_ == before) {
            auto pumped = pump_pending(true);
            if (!pumped.ok())
              return pumped;
          }
        }
      }
      if (ran) {
        auto retired = finish_steps(i);
        if (!retired.ok())
          return retired;
      }
    }
    return Status::success();
  }
  static std::uint64_t unit_count(const PlanStep&, const TileRequest& request) {
    return request.size();
  }
  static TileAddress unit_index(const PlanStep&, const TileRequest& request,
                                std::uint64_t position) {
    return request.at(position);
  }
  Status append_unit_view(ResultBuilder& builder, const ResultRef& source,
                          const ResultDescriptor& facts, const Region& region,
                          std::uint32_t slot, unsigned depth = 0) {
    return plugin_internal::append_publication_view(
        builder, source, facts, slot, region, resources_, active_token(),
        [&](auto n) { return consume(n); }, depth);
  }

  Result<ResultRef> assemble_units(std::size_t index,
                                   const TileRequest& request,
                                   const ResourceVector<ResultRef>& units) {
    using Answer = Result<ResultRef>;
    if (units.empty())
      return empty_named_output(index);
    if (units.size() == 1)
      return Answer(units.front());
    const auto& step = plan_.steps()[index];
    content_internal::Sha256 identity;
    identity.text("photospider.tile-assembly.v1");
    identity.text(templates_[index]);
    identity.text(snapshot_);
    ResourceVector<std::uint64_t> association{
        ResourceAllocator<std::uint64_t>(resources_)};
    for (std::size_t i = 0; i < units.size(); ++i) {
      auto charged = consume(1);
      if (!charged.ok())
        return Answer(charged);
      const auto& object = units[i];
      if (!object.valid() ||
          !object.schema().same_schema(*step.output_result_schema))
        return Answer(protocol_failure("incompatible tile assembly schema"));
      auto ids = object.association();
      association.insert(association.end(), ids.begin(), ids.end());
      const auto address = unit_index(step, request, i);
      identity.integer(address.tensor_slot);
      identity.integer(address.tile);
    }
    auto scratch = resources_.reserve(
        ResourceCapacity::host(association.size() * sizeof(std::uint64_t),
                               association.size() * sizeof(std::uint64_t)));
    if (!scratch.ok())
      return Answer(scratch.status());
    auto made = ResultBuilder::start(
        resources_, *step.output_result_schema, identity.finish(), {},
        std::vector<std::uint64_t>(association.begin(), association.end()),
        plan_.tile_extent());
    if (!made.ok())
      return Answer(made.status());
    auto builder = made.take_value();
    for (std::size_t i = 0; i < units.size(); ++i) {
      auto facts = units[i].descriptor();
      const auto address = unit_index(step, request, i);
      auto region = step.tile_grids[address.tensor_slot].region(address.tile);
      if (!facts.ok() || !region.ok())
        return Answer(!facts.ok() ? facts.status() : region.status());
      auto appended = append_unit_view(builder, units[i], facts.value(),
                                       region.value(), address.tensor_slot);
      if (!appended.ok())
        return Answer(appended);
    }
    auto result = builder.seal();
    if (!result.ok())
      return result;
    for (std::uint32_t slot = 0; slot < step.tile_grids.size(); ++slot) {
      auto expected = demand_samples(request, step, slot, set_limits());
      if (!expected.ok())
        return Answer(expected.status());
      if (result.value().descriptor().value().tensor_coverage(slot) !=
          expected.value())
        return Answer(
            protocol_failure("assembled member tile coverage mismatch"));
    }
    result.value().bind_producer(step.node_id);
    return result;
  }
  Result<ResultRef> result_object(std::size_t index, TileRequest selected,
                                  Actor* consumer = nullptr) {
    using Answer = Result<ResultRef>;
    const auto& step = plan_.steps()[index];
    auto resolved = selected.resolve(step.tile_grids);
    if (!resolved.ok())
      return Answer(resolved.status());
    selected = resolved.take_value();
    auto permitted = admit_validation_scope(index, selected);
    if (!permitted.ok())
      return Answer(permitted);
    auto shared = acquisition(index, selected);
    if (!shared.ok())
      return Answer(shared.status());
    auto acquired = shared.take_value();
    if (consumer && acquired->result.valid()) {
      consumer->fallback.taint |= acquired->fallback_taint;
      return Answer(acquired->result);
    }
    const auto count = unit_count(step, selected);
    while (!acquired->result.valid() && acquired->position < count) {
      const auto i = acquired->position;
      bool fallback_taint = false;
      if (!consumer) {
        const auto address = unit_index(step, selected, i);
        const auto limit = std::max(1U, maximum_parallelism_);
        // Retain prepared siblings until their named output is collected.
        // Only consumed units of this output leave the bounded root window.
        requested_roots_.erase(
            std::remove_if(requested_roots_.begin(), requested_roots_.end(),
                           [&](const auto& root) {
                             const auto& unit = root->tile_record.unit;
                             const auto member =
                                 selected.members.find(unit.tensor_slot);
                             return root->index == index &&
                                    member != selected.members.end() &&
                                    member->second.contains(unit.tile) &&
                                    (unit.tensor_slot < address.tensor_slot ||
                                     (unit.tensor_slot == address.tensor_slot &&
                                      unit.tile < address.tile));
                           }),
            requested_roots_.end());
        const auto append = [&](std::size_t owner, TileAddress unit) -> Status {
          const auto found = std::find_if(
              requested_roots_.begin(), requested_roots_.end(),
              [&](const auto& root) {
                return root->index == owner &&
                       root->tile_record.unit.tensor_slot == unit.tensor_slot &&
                       root->tile_record.unit.tile == unit.tile;
              });
          if (found != requested_roots_.end())
            return Status::success();
          auto created = actor(owner, unit, true);
          if (!created.ok())
            return created.status();
          created.value()->named_root = true;
          requested_roots_.push_back(created.take_value());
          return Status::success();
        };
        auto seeded = append(index, address);
        if (!seeded.ok())
          return Answer(seeded);
        const auto append_next = [&](std::size_t owner) -> Result<bool> {
          auto& cursor = root_cursors_[owner];
          const auto& members = pending_root_requests_[owner].members;
          for (auto member = members.lower_bound(cursor.tensor_slot);
               member != members.end(); ++member) {
            for (const auto& interval : member->second.intervals()) {
              auto work = consume(1);
              if (!work.ok())
                return Result<bool>(work);
              const auto begin = member->first == cursor.tensor_slot
                                     ? std::max(interval.begin, cursor.tile)
                                     : interval.begin;
              if (begin >= interval.end)
                continue;
              cursor = {member->first, begin + 1};
              auto status = append(owner, {member->first, begin});
              return status.ok() ? Result<bool>(true) : Result<bool>(status);
            }
          }
          return Result<bool>(false);
        };
        // Give each named output a slot before filling this output's wave.
        for (std::size_t peer = 0; peer < pending_root_requests_.size() &&
                                   requested_roots_.size() < limit;
             ++peer) {
          if (peer == index || pending_root_requests_[peer].empty() ||
              std::any_of(
                  requested_roots_.begin(), requested_roots_.end(),
                  [&](const auto& root) { return root->index == peer; }))
            continue;
          auto next = append_next(peer);
          if (!next.ok())
            return Answer(next.status());
        }
        // The pending union also includes other names selecting this step.
        // Advancing its compact cursor does not enumerate unrequested tiles.
        while (requested_roots_.size() < limit) {
          auto next = append_next(index);
          if (!next.ok())
            return Answer(next.status());
          if (!next.value())
            break;
        }
      }
      if (!acquired->producer) {
        auto producer = actor(index, unit_index(step, selected, i));
        if (!producer.ok())
          return Answer(producer.status());
        acquired->producer = producer.take_value();
      }
      auto ready = unit_object(index, unit_index(step, selected, i), consumer,
                               &fallback_taint);
      if (!ready.ok())
        return ready;
      acquired->fallback_taint |= fallback_taint;
      // unit_object may advance another named root while waiting. That root
      // can finish this same acquisition, so only commit unchanged progress.
      if (!acquired->result.valid() && acquired->position == i) {
        acquired->units.push_back(ready.take_value());
        ++acquired->position;
        acquired->producer.reset();
      }
    }
    if (!consumer) {
      for (const auto& member : selected.members) {
        auto found = pending_root_requests_[index].members.find(member.first);
        if (found == pending_root_requests_[index].members.end())
          continue;
        auto left = found->second.subtract(member.second, resources_);
        if (!left.ok())
          return Answer(left.status());
        if (left.value().empty())
          pending_root_requests_[index].members.erase(found);
        else
          found->second = left.take_value();
      }
      for (const auto& root : requested_roots_) {
        if (root->index != index)
          continue;
        auto closed = complete_liveness(*root);
        if (!closed.ok())
          return Answer(closed);
      }
      requested_roots_.erase(
          std::remove_if(requested_roots_.begin(), requested_roots_.end(),
                         [&](const auto& root) {
                           const auto& unit = root->tile_record.unit;
                           const auto member =
                               selected.members.find(unit.tensor_slot);
                           return root->index == index &&
                                  member != selected.members.end() &&
                                  member->second.contains(unit.tile);
                         }),
          requested_roots_.end());
      auto finished = finish_steps(index);
      if (!finished.ok())
        return Answer(finished);
    }
    if (!acquired->result.valid()) {
      auto assembled = assemble_units(index, selected, acquired->units);
      if (!assembled.ok())
        return assembled;
      acquired->result = assembled.take_value();
      acquired->units.clear();
      acquired->units.shrink_to_fit();
    }
    if (consumer)
      consumer->fallback.taint |= acquired->fallback_taint;
    return Answer(acquired->result);
  }
  Result<ResultRef> unit_object(std::size_t index, TileAddress tile,
                                Actor* consumer = nullptr,
                                bool* fallback_taint = nullptr) {
    auto acquired = actor(index, tile);
    if (!acquired.ok())
      return Result<ResultRef>(acquired.status());
    auto current = acquired.take_value();
    if (consumer) {
      auto connected = connect_wait(*consumer, current);
      if (!connected.ok())
        return Result<ResultRef>(connected);
    }
    for (;;) {
      const auto before = progress_;
      ++traversal_;
      auto ready = result_object_step(index, current);
      if (!ready.ok())
        return Result<ResultRef>(ready.status());
      if (ready.value()) {
        const bool tainted =
            current->fallback.taint ||
            current->query.backend != plan_.steps()[index].backend;
        if (consumer) {
          consumer->fallback.taint |= tainted;
          consumer->waiting.reset();
        }
        if (fallback_taint)
          *fallback_taint |= tainted;
        return Result<ResultRef>(std::move(*ready.value()));
      }
      // Synchronous replay can be inside an upstream Actor initializer.
      // Advancing peer roots here could reenter that unfinished initializer.
      if (!callback_context() && !consumer) {
        for (const auto& root : requested_roots_) {
          if (root == current || root->complete ||
              root->terminal != ErrorCode::Ok || !root->initialized ||
              recursively_busy(*root))
            continue;
          auto advanced = result_object_step(root->index, root);
          if (!advanced.ok())
            return Result<ResultRef>(advanced.status());
        }
      }
      if (!callback_context() && progress_ == before) {
        auto pumped = pump_pending(true);
        if (!pumped.ok())
          return Result<ResultRef>(pumped);
      }
    }
  }
  Result<ResultRef> object_input(const PlanInput& input,
                                 const TileRequest& selected,
                                 Actor* consumer = nullptr) {
    if (const auto* source = std::get_if<PlanWorkflowInput>(&input)) {
      const auto& object = bindings_.at(source->declaration_index).result;
      if (!object.owned_by(resources_))
        return Result<ResultRef>(protocol_failure(
            "Result binding belongs to a different resource root"));
      return Result<ResultRef>(object);
    }
    auto ready = result_object(std::get<PlanStepInput>(input).step_index,
                               selected, consumer);
    return ready;
  }
  Result<std::shared_ptr<Acquisition>> acquisition(
      std::size_t index, const TileRequest& selected) {
    using Answer = Result<std::shared_ptr<Acquisition>>;
    auto work =
        consume(1 + selected.interval_count() + selected.members.size());
    if (!work.ok())
      return Answer(work);
    content_internal::Sha256 digest;
    digest.text("photospider.input-acquisition.v1");
    for (const auto& member : selected.members) {
      digest.integer(member.first);
      digest.integer(member.second.intervals().size());
      for (const auto& interval : member.second.intervals()) {
        digest.integer(interval.begin);
        digest.integer(interval.end);
      }
    }
    auto key = digest.finish();
    auto& entries = acquisitions_[index];
    auto found = entries.find(std::string_view(key));
    if (found != entries.end())
      return Answer(found->second);
    auto made = std::allocate_shared<Acquisition>(
        ResourceAllocator<Acquisition>(resources_), resources_);
    entries.emplace(ResourceString(key, ResourceAllocator<char>(resources_)),
                    made);
    return Answer(std::move(made));
  }
  Result<std::optional<ResultRef>> need_object_input(
      Actor& consumer, NeedCursor& cursor, const PlanInput& input,
      const TileRequest& selected, bool replay) {
    using Answer = Result<std::optional<ResultRef>>;
    if (replay || std::holds_alternative<PlanWorkflowInput>(input)) {
      auto ready = object_input(input, selected, &consumer);
      if (!ready.ok())
        return Answer(ready.status());
      return Answer(std::optional<ResultRef>(ready.take_value()));
    }
    const auto index = std::get<PlanStepInput>(input).step_index;
    const auto& step = plan_.steps()[index];
    const auto count = unit_count(step, selected);
    if (!cursor.acquisition) {
      auto shared = acquisition(index, selected);
      if (!shared.ok())
        return Answer(shared.status());
      cursor.acquisition = shared.take_value();
    }
    auto& acquired = *cursor.acquisition;
    if (!acquired.result.valid() && acquired.position < count) {
      if (!acquired.producer) {
        auto producer =
            actor(index, unit_index(step, selected, acquired.position));
        if (!producer.ok())
          return Answer(producer.status());
        acquired.producer = producer.take_value();
      }
      cursor.producer = acquired.producer;
      auto connected = connect_wait(consumer, cursor.producer);
      if (!connected.ok())
        return Answer(connected);
      auto ready = result_object_step(index, cursor.producer);
      if (!ready.ok() || !ready.value())
        return ready;
      acquired.fallback_taint |= cursor.producer->fallback.taint ||
                                 cursor.producer->query.backend != step.backend;
      acquired.units.push_back(std::move(*ready.value()));
      ++acquired.position;
      ++progress_;
      consumer.waiting.reset();
      cursor.producer.reset();
      acquired.producer.reset();
      if (acquired.position < count)
        return Answer(std::optional<ResultRef>{});
    }
    if (!acquired.result.valid()) {
      auto assembled = assemble_units(index, selected, acquired.units);
      if (!assembled.ok())
        return Answer(assembled.status());
      acquired.result = assembled.take_value();
      acquired.units.clear();
      acquired.units.shrink_to_fit();
    }
    consumer.fallback.taint |= acquired.fallback_taint;
    auto result = acquired.result;
    consumer.waiting.reset();
    cursor.producer.reset();
    cursor.acquisition.reset();
    return Answer(std::optional<ResultRef>(std::move(result)));
  }

  Result<std::optional<ResultTensorInput>> need_tensor_input(
      Actor& consumer, NeedCursor& cursor, const PlanInput& input,
      const Footprint& samples, const TileRequest& selected, std::uint32_t slot,
      bool replay) {
    using Answer = Result<std::optional<ResultTensorInput>>;
    auto ready = need_object_input(consumer, cursor, input, selected, replay);
    if (!ready.ok())
      return Answer(ready.status());
    if (!ready.value())
      return Answer(std::optional<ResultTensorInput>{});
    auto descriptor = ready.value()->descriptor();
    if (!descriptor.ok())
      return Answer(descriptor.status());
    auto outside = samples.subtract(descriptor.value().tensor_coverage(slot),
                                    set_limits());
    if (!outside.ok())
      return Answer(outside.status());
    if (!outside.value().empty())
      return Answer(
          protocol_failure("tensor Need is outside published coverage"));
    ResultTensorInput grant;
    grant.result_ = std::move(*ready.value());
    grant.descriptor_ = descriptor.take_value();
    grant.slot_ = slot;
    grant.samples_ = samples;
    grant.resources_ = resources_;
    return Answer(std::optional<ResultTensorInput>{std::move(grant)});
  }
  Status notify(Actor& actor) {
    if (!actor.is_shared() && !actor.visibility_ready)
      return Status::success();
    if (!actor.complete || actor.is_shared())
      return Status::success();
    if (!subscription_.enabled())
      return Status::success();
    auto descriptor = actor.published.descriptor(false);
    if (!descriptor.ok())
      return descriptor.status();
    auto charged = consume(actor.aliases.size());
    if (!charged.ok())
      return charged;
    Status delivered;
    for (const auto index : actor.aliases) {
      delivered =
          subscription_.notify(index, plan_.steps()[index].result_ref(),
                               actor.published, descriptor.value().revision());
      if (!delivered.ok())
        break;
    }
    return delivered;
  }
  void note_cached_input_backend(Actor& actor, const PlanInput& input,
                                 const ResultRef* object = nullptr) {
    const auto* producer = std::get_if<PlanStepInput>(&input);
    if (!producer)
      return;
    auto source = actor_aliases_[producer->step_index];
    if (object) {
      source.reset();
      for (const auto& weak : actors_)
        if (auto candidate = weak.lock();
            candidate && candidate->published.valid() &&
            candidate->published.object_id() == object->object_id()) {
          source = candidate;
          break;
        }
    }
    if (source &&
        (source->fallback.taint ||
         source->query.backend != plan_.steps()[producer->step_index].backend))
      actor.fallback.taint = true;
  }

  struct CacheWork final : StructuredCacheWorkServices {
    explicit CacheWork(StructuredExecution& owner) : owner(owner) {}
    Status charge(std::uint64_t n) override {
      return owner.checkpoint_cache_work(n);
    }
    CancellationToken cancellation() override { return owner.active_token(); }
    ErrorCode stop() override { return owner.active_stop(); }
    StructuredExecution& owner;
  };
  StructuredCacheContext cache_context() {
    return {plan_,
            bindings_,
            templates_,
            snapshot_,
            options_.dependencies.sets,
            result_cacheable_,
            blocks_,
            *tile_records_,
            block_epoch_,
            options_.maximum_result_window_bytes,
            diagnostics_.dependency_cache_records_visited};
  }
  static StructuredCacheActorView cache_actor(Actor& actor) {
    return {actor.query,       actor.cache,       actor.tensors,
            actor.results,     actor.descriptors, actor.tile_record,
            actor.input_facts, actor.published,   actor.fallback.taint,
            actor.busy};
  }
  Result<ResourceString> supplied_cache_facts(const Actor& actor) {
    CacheWork work(*this);
    return cache_state_.supplied_facts(actor.results, actor.tensors,
                                       options_.maximum_result_window_bytes,
                                       work);
  }
  void retain_cache_need(std::size_t index, Actor& actor,
                         const ResultProgramNeed& need) noexcept {
    CacheWork work(*this);
    cache_state_.record_need(index, cache_actor(actor), cache_context(), need,
                             work);
  }
  void retain_cached_result(std::size_t index, Actor& actor) noexcept {
    std::lock_guard<std::recursive_mutex> metadata(callback_metadata_mutex_);
    CacheWork work(*this);
    cache_state_.store_completed(index, cache_actor(actor), cache_context(),
                                 work);
  }
  Result<bool> reuse_cached_result(std::size_t index, Actor& actor) {
    struct Host final : StructuredCacheReplayHost {
      Host(StructuredExecution& owner, std::size_t index, Actor& actor)
          : owner(owner), index(index), actor(actor) {}
      Status charge(std::uint64_t n) override {
        return owner.checkpoint_cache_work(n);
      }
      CancellationToken cancellation() override { return owner.active_token(); }
      ErrorCode stop() override { return owner.active_stop(); }
      Status supply(const ResultProgramNeed& need) override {
        auto status = owner.replay_need(index, actor, need);
        if (!status.ok() || !owner.payload_bounds_[index])
          return status;
        if (!provenance)
          provenance =
              std::allocate_shared<plugin_internal::ResultPayloadBound>(
                  ResourceAllocator<plugin_internal::ResultPayloadBound>(
                      owner.resources_),
                  UINT64_MAX);
        ResultProgramPhase phase{actor.query,
                                 actor.results,
                                 owner.resources_.allocator(),
                                 owner.resources_,
                                 [&](std::uint64_t n) { return charge(n); },
                                 actor.failure,
                                 {}};
        phase.tensors = &actor.tensors;
        phase.shared_stages = &actor.shared_results;
        return provenance->capture(phase);
      }
      Result<bool> adopt(const StructuredCacheManifest& candidate,
                         const ResourceVector<std::uint64_t>& association,
                         const ReboundTileCache& tiles) override {
        auto rebound = candidate.result.rebind_cached(
            actor.query.semantic_key, association, owner.active_token(),
            [&](std::uint64_t n) { return charge(n); });
        if (!rebound.ok()) {
          if (rebound.status().code == ErrorCode::Cancelled ||
              rebound.status().code == ErrorCode::Stale)
            return Result<bool>(rebound.status());
          return Result<bool>(false);
        }
        auto charged = charge(actor.tile_record.inputs.size() +
                              tiles.task.inputs.size() + 1);
        if (!charged.ok())
          return charged.code == ErrorCode::ResourceExhausted
                     ? Result<bool>(false)
                     : Result<bool>(charged);
        TileRecord merged = actor.tile_record;
        for (const auto& dependency : tiles.task.inputs) {
          charged = charge(dependency.tiles.intervals().size() + 1);
          if (charged.ok())
            charged = merged.include(dependency, owner.resources_);
          if (!charged.ok())
            return charged.code == ErrorCode::ResourceExhausted
                       ? Result<bool>(false)
                       : Result<bool>(charged);
        }
        actor.tile_record = std::move(merged);
        actor.query.backend = candidate.backend;
        CachePublicationTransaction transaction(owner.callback_metadata_mutex_);
        auto captured = [&]() -> Result<ResultRef> {
          WorkMode optional(owner.work_mode(), true);
          auto checked =
              owner.validate_publication(index, actor, {rebound.value(), true});
          if (!checked.ok())
            return Result<ResultRef>(checked);
          return rebound.value().capture();
        }();
        if (!captured.ok())
          return captured.status().code == ErrorCode::ResourceExhausted ||
                         captured.status().code == ErrorCode::NotFound
                     ? Result<bool>(false)
                     : Result<bool>(captured.status());
        std::shared_ptr<plugin_internal::ResultPayloadBound> accounted_payload;
        if (owner.payload_bounds_[index]) {
          ResultProgramPhase phase{actor.query,
                                   actor.results,
                                   owner.resources_.allocator(),
                                   owner.resources_,
                                   [&](std::uint64_t n) { return charge(n); },
                                   actor.failure,
                                   {}};
          phase.tensors = &actor.tensors;
          phase.shared_stages = &actor.shared_results;
          auto ledger = owner.payload_bounds_[index]->candidate(phase);
          if (!ledger.ok())
            return ledger.status().code == ErrorCode::ResourceExhausted
                       ? Result<bool>(false)
                       : Result<bool>(ledger.status());
          accounted_payload = ledger.take_value();
          auto admitted = accounted_payload->capture(phase);
          if (admitted.ok() && provenance)
            admitted = accounted_payload->import_sources(*provenance, phase);
          if (admitted.ok())
            admitted = accounted_payload->check(captured.value(), phase);
          if (!admitted.ok())
            return admitted.code == ErrorCode::ResourceExhausted
                       ? Result<bool>(false)
                       : Result<bool>(admitted);
        }
        auto status = owner.commit_publication(
            index, actor, captured.take_value(), true, true, true,
            &tiles.ancestry, [&](std::uint64_t n) { return charge(n); },
            &transaction, std::move(accounted_payload));
        if (!status.ok())
          return actor.terminal == ErrorCode::Ok && !actor.complete &&
                         status.code == ErrorCode::ResourceExhausted
                     ? Result<bool>(false)
                     : Result<bool>(status);
        ++owner.diagnostics_.cache_hits;
        owner.diagnostics_
            .selected_backends[owner.plan_.steps()[index].result_ref()] =
            actor.query.backend;
        return Result<bool>(true);
      }
      std::shared_ptr<plugin_internal::ResultPayloadBound> provenance;
      StructuredExecution& owner;
      std::size_t index;
      Actor& actor;
    } host(*this, index, actor);
    return cache_state_.try_reuse(index, cache_actor(actor), cache_context(),
                                  host);
  }
  Result<std::optional<ResultDescriptor>> static_descriptor(
      const SchemaTemplate& schema) {
    using Answer = Result<std::optional<ResultDescriptor>>;
    auto charged = consume(1 + schema.tensors.size());
    if (!charged.ok())
      return Answer(charged);
    ResultDescriptor facts;
    facts.tensor_count_ = schema.tensors.size();

    for (std::uint32_t slot = 0; slot < schema.tensors.size(); ++slot) {
      auto empty =
          Footprint::none(schema.tensors[slot].sample_shape(), set_limits());
      if (!empty.ok())
        return Answer(empty.status());
      facts.tensors_[slot] = empty.take_value();
    }
    return Answer(std::optional<ResultDescriptor>(std::move(facts)));
  }
  Result<std::shared_ptr<const StructuredNeedGrants>> tile_grants(
      std::size_t, const Actor& actor, const ResultProgramNeed& need) {
    using Answer = Result<std::shared_ptr<const StructuredNeedGrants>>;
    const auto& step = actor.contract();
    auto valid = plugin_internal::validate_result_need(
        need, actor.query, step.traits.outputs[0], resources_,
        std::min<std::uint64_t>(65536,
                                options_.dependencies.sets.maximum_boxes),
        false, [&](std::uint64_t n) { return consume(n); });
    if (!valid.ok())
      return Answer(valid);
    auto grants = std::allocate_shared<StructuredNeedGrants>(
        ResourceAllocator<StructuredNeedGrants>(resources_));
    grants->shared_stages = ResourceVector<ResultSharedNeed>(
        need.shared_stages.begin(), need.shared_stages.end(),
        ResourceAllocator<ResultSharedNeed>(resources_));
    for (auto& shared : grants->shared_stages) {
      if (!shared.tiles)
        continue;
      auto grids = TileGrid::members(
          step.prepared->traits().shared_stages[shared.stage].result_schema,
          plan_.tile_extent());
      if (!grids.ok())
        return Answer(grids.status());
      auto normalized = shared.tiles->resolve(grids.value());
      if (!normalized.ok())
        return Answer(normalized.status());
      shared.tiles = normalized.take_value();
    }
    grants->dependencies = ResourceVector<TileDependency>(
        ResourceAllocator<TileDependency>(resources_));
    grants->static_descriptors = decltype(grants->static_descriptors)(
        ResourceAllocator<std::pair<std::uint32_t, ResultDescriptor>>(
            resources_));
    grants->tiles = ResourceVector<StructuredTensorGrant>(
        ResourceAllocator<StructuredTensorGrant>(resources_));
    for (const auto& request : need.tiles) {
      const auto& source = step.inputs[request.input];
      const auto& grids = actor.query.input_tile_grids[request.input];
      auto resolved = request.resolve(grids, resources_);
      if (!resolved.ok())
        return Answer(resolved.status());
      const auto& normalized = resolved.value();
      if (!normalized.descriptor_only && normalized.tiles.empty())
        continue;
      const auto* producer = std::get_if<PlanStepInput>(&source);
      grants->dependencies.push_back(
          {source,
           producer ? plan_.steps()[producer->step_index].output_index : 0,
           normalized.tensor_slot, normalized.tiles});
      const auto& schema = *actor.query.inputs[request.input].result_schema;
      if (normalized.descriptor_only) {
        auto facts = static_descriptor(schema);
        if (!facts.ok())
          return Answer(facts.status());
        if (facts.value())
          grants->static_descriptors.emplace_back(request.input,
                                                  *facts.value());
        continue;
      }
      const auto slot = normalized.tensor_slot;
      auto samples = grids[slot].coverage(normalized.tiles, set_limits());
      if (!samples.ok())
        return Answer(samples.status());
      TileRequest selected;
      selected.members.emplace(slot, normalized.tiles);
      grants->tiles.push_back({request.input, slot, samples.take_value(), false,
                               std::move(selected)});
    }
    // One envelope may split a producer's complete scope across input ports.
    // Merge only for validator admission; each port retains its original grant.
    // A plan step identifies one specialized output, including its member
    // grids.
    using ProducerRequest = std::pair<const std::size_t, TileRequest>;
    std::map<std::size_t, TileRequest, std::less<std::size_t>,
             ResourceAllocator<ProducerRequest>>
        producer_requests{std::less<std::size_t>{},
                          ResourceAllocator<ProducerRequest>(resources_)};
    for (const auto& grant : grants->tiles) {
      const auto* producer =
          std::get_if<PlanStepInput>(&step.inputs[grant.input]);
      if (!producer)
        continue;
      const auto& source = plan_.steps()[producer->step_index];
      if (!source.prepared->requires_complete_validation(true,
                                                         source.output_index))
        continue;
      auto& requested = producer_requests[producer->step_index];
      auto joined = requested.unite(grant.producer_request, resources_);
      if (!joined.ok())
        return Answer(joined.status());
      requested = joined.take_value();
    }
    for (const auto& requested : producer_requests) {
      if (requested.second.empty())
        continue;
      auto admitted = admit_validation_scope(requested.first, requested.second);
      if (!admitted.ok())
        return Answer(admitted);
    }
    return Answer(std::move(grants));
  }
  Status supply_need_step(std::size_t index, Actor& actor,
                          const ResultProgramNeed& need, NeedCursor& cursor,
                          bool replay = false) {
    WorkMode work(work_mode(), replay);
    if (!cursor.grants) {
      auto grants = tile_grants(index, actor, need);
      if (!grants.ok())
        return grants.status();
      auto record_work = consume(actor.tile_record.inputs.size());
      if (!record_work.ok())
        return record_work;
      TileRecord merged = actor.tile_record;
      for (const auto& dependency : grants.value()->dependencies) {
        auto included = merged.include(dependency, resources_);
        if (!included.ok())
          return included;
      }
      actor.tile_record = std::move(merged);
      cursor.grants = grants.take_value();
    }
    return supply_need_step(index, actor, *cursor.grants, cursor, replay);
  }
  Status supply_need_step(std::size_t index, Actor& actor,
                          const StructuredNeedGrants& need, NeedCursor& cursor,
                          bool replay = false) {
    WorkMode work(work_mode(), replay);
    if (!cursor.initialized) {
      for (const auto& entry : need.static_descriptors) {
        actor.descriptors[entry.first] = entry.second;
      }

      actor.tensors.clear();
      actor.shared_results.clear();
      if (!need.shared_stages.empty()) {
        actor.has_shared_dependencies = true;
        actor.cache.disabled = true;
        actor.cache.replay.clear();
      }
      const auto count = need.tiles.size() + need.shared_stages.size();
      cursor.total = count;
      cursor.initialized = true;
      ++progress_;
    }
    const auto inputs = need.tiles.size() + need.shared_stages.size();
    if (!replay && inputs > 1 && cursor.next < inputs) {
      auto advanced =
          cursor.advance_inputs(inputs, resources_, [&](NeedCursor& progress) {
            return supply_one_need_step(index, actor, need, progress, false);
          });
      if (!advanced.ok())
        return advanced.status();
      if (advanced.value()) {
        ++progress_;
        if (cursor.supplied == inputs)
          actor.waiting.reset();
      }
      return cursor.complete() ? validate_supplied_inputs(actor)
                               : Status::success();
    }
    const auto before = cursor.next;
    auto status = supply_one_need_step(index, actor, need, cursor, replay);
    if (cursor.next != before)
      ++progress_;
    return status.ok() && cursor.complete() ? validate_supplied_inputs(actor)
                                            : status;
  }
  Status validate_supplied_inputs(Actor& actor) {
    if (!actor.query.prepared)
      return Status::success();
    std::uint32_t previous = UINT32_MAX;
    for (const auto& input : actor.tensors) {
      const auto port = input.first.first;
      if (port == previous)
        continue;
      previous = port;
      const auto& source = actor.contract().inputs[port];
      auto checked = plugin_internal::validate_tensor_contracts(
          *actor.query.prepared, port, *actor.query.inputs[port].result_schema,
          actor.tensors, resources_, active_token(),
          [&](auto n) { return operation_work(actor, n); },
          std::holds_alternative<PlanWorkflowInput>(source)
              ? ErrorCode::InvalidArgument
              : ErrorCode::OperationFailed);
      if (!checked.ok())
        return input_failure(source, checked);
    }
    return Status::success();
  }
  Status prepare_tensor_supply(Actor& actor, const StructuredNeedGrants& need,
                               const StructuredTensorGrant& input,
                               const Footprint& closed, NeedCursor& cursor) {
    auto charged = consume(need.tiles.size());
    if (!charged.ok())
      return charged;
    cursor.tensor_supply_samples = closed;
    cursor.tensor_request = input.producer_request;
    cursor.tensor_payload = !input.descriptor_only;
    for (const auto& other : need.tiles) {
      if (&other == &input || other.input != input.input ||
          other.slot != input.slot || other.descriptor_only)
        continue;
      cursor.tensor_payload = true;
      auto united =
          cursor.tensor_request.members.at(input.slot)
              .unite(other.producer_request.members.at(input.slot), resources_);
      if (!united.ok())
        return united.status();
      cursor.tensor_request.members[input.slot] = united.take_value();
      auto part = actor.query.inputs[input.input]
                      .result_schema->tensors[input.slot]
                      .close_samples(other.samples, set_limits());
      if (!part.ok())
        return part.status();
      auto joined =
          cursor.tensor_supply_samples->unite(part.value(), set_limits());
      if (!joined.ok())
        return joined.status();
      cursor.tensor_supply_samples = joined.take_value();
    }
    return Status::success();
  }
  Status validate_tensor_supply(Actor& actor, const PlanStep& step,
                                const StructuredTensorGrant& input,
                                const Footprint& closed,
                                const ResultTensorInput& capability) {
    const auto& result = capability.result_;
    const auto& granted = capability.samples_;
    note_cached_input_backend(actor, step.inputs[input.input], &result);
    auto selected = closed.intersect(granted, set_limits());
    if (!selected.ok())
      return selected.status();
    auto& revision = actor.input_facts[{input.input, result.object_id()}];
    revision = std::max(revision, capability.descriptor_.revision());
    return Status::success();
  }
  Status supply_tensor_need(std::size_t, Actor& actor,
                            const StructuredNeedGrants& need,
                            const StructuredTensorGrant& input,
                            NeedCursor& cursor, bool replay) {
    const auto& step = actor.contract();
    const auto fail = [&](Status status) {
      return replay ? status : retire(actor, status);
    };
    if (!cursor.tensor_samples) {
      auto closed = actor.query.inputs[input.input]
                        .result_schema->tensors[input.slot]
                        .close_samples(input.samples, set_limits());
      if (!closed.ok())
        return fail(closed.status());
      if (input.descriptor_only) {
        auto empty = Footprint::none(closed.value().shape(), set_limits());
        if (!empty.ok())
          return fail(empty.status());
        closed = std::move(empty);
      }
      cursor.tensor_samples = closed.take_value();
    }
    const auto& closed = *cursor.tensor_samples;
    const auto existing = actor.tensors.find({input.input, input.slot});
    ResultTensorInput capability;
    if (existing != actor.tensors.end()) {
      capability = existing->second;
    } else {
      if (!cursor.tensor_supply_samples) {
        auto prepared =
            prepare_tensor_supply(actor, need, input, closed, cursor);
        if (!prepared.ok())
          return fail(prepared);
      }
      if (!cursor.tensor_payload) {
        capability.static_schema_ =
            actor.query.inputs[input.input].result_schema;
        capability.slot_ = input.slot;
        capability.samples_ = *cursor.tensor_supply_samples;
        capability.resources_ = resources_;
      } else {
        auto observed =
            need_tensor_input(actor, cursor, step.inputs[input.input],
                              *cursor.tensor_supply_samples,
                              cursor.tensor_request, input.slot, replay);
        if (!observed.ok())
          return fail(observed.status());
        if (!observed.value())
          return Status::success();
        capability = std::move(*observed.value());
        capability.payload_authorized_ = true;
      }
    }
    capability.failure_ = actor.failure;
    capability.observer_ = actor.service_failure;
    auto outside = closed.subtract(capability.samples_, set_limits());
    if (!outside.ok())
      return fail(outside.status());
    if (!outside.value().empty())
      return fail(protocol_failure("tensor Need is outside supplied coverage"));
    auto checked =
        capability.static_schema_
            ? Status::success()
            : validate_tensor_supply(actor, step, input, closed, capability);
    if (!checked.ok())
      return fail(checked);
    if (existing == actor.tensors.end())
      actor.tensors.emplace(std::make_pair(input.input, input.slot),
                            std::move(capability));
    cursor.tensor_samples.reset();
    cursor.tensor_supply_samples.reset();
    cursor.tensor_payload = false;
    ++cursor.next;
    return Status::success();
  }
  Result<ResultRef> project_shared(const Actor& actor, const ResultRef& source,
                                   const TileRequest& selected) {
    using Answer = Result<ResultRef>;
    const auto& schema = *actor.query.output.result_schema;
    auto association = source.association();
    auto made = ResultBuilder::start(
        resources_, schema, actor.key, {},
        std::vector<std::uint64_t>(association.begin(), association.end()),
        plan_.tile_extent());
    if (!made.ok())
      return Answer(made.status());
    auto builder = made.take_value();
    auto facts = source.descriptor(false);
    if (!facts.ok())
      return Answer(facts.status());
    for (const auto& member : selected.members) {
      auto samples = actor.contract().tile_grids[member.first].coverage(
          member.second, set_limits());
      if (!samples.ok())
        return Answer(samples.status());
      for (const auto& box : samples.value().boxes()) {
        auto appended =
            append_unit_view(builder, source, facts.value(), box, member.first);
        if (!appended.ok())
          return Answer(appended);
      }
    }
    return builder.seal();
  }
  Status commit_shared(Actor& actor, const ResultRef& source, bool complete) {
    auto facts = source.descriptor(false);
    if (!facts.ok())
      return facts.status();
    auto produced = plugin_internal::published_tiles(
        source.schema(), facts.value(), plan_.tile_extent(), resources_,
        set_limits());
    if (!produced.ok())
      return produced.status();
    auto visible = plugin_internal::visible_tiles(
        source.schema(), produced.value(), plan_.tile_extent(), resources_);
    if (!visible.ok())
      return visible.status();
    bool requires_complete = false;
    // Stage validators with complete requirements wait for the static full
    // scope.
    requires_complete =
        actor.query.prepared->requires_complete_stage_validation(
            actor.query.shared_stage);
    auto all = TileRequest::all(actor.contract().tile_grids, resources_);
    if (!all.ok())
      return all.status();
    if (requires_complete && !produced.value().contains(all.value())) {
      if (complete)
        return protocol_failure(
            "shared stage omitted complete validator coverage");
      return Status::success();
    }
    TileRequest delta;
    for (const auto& member : visible.value().members) {
      const auto prior = actor.shared_visible.members.find(member.first);
      auto remaining = prior == actor.shared_visible.members.end()
                           ? Result<TileSet>(member.second)
                           : member.second.subtract(prior->second, resources_);
      if (!remaining.ok())
        return remaining.status();
      if (!remaining.value().empty())
        delta.members.emplace(member.first, remaining.take_value());
    }
    if (complete && visible.value() != produced.value())
      return protocol_failure(
          "shared stage ended with an incomplete publication scope");
    if (delta.empty())
      return Status::success();
    auto payload = project_shared(actor, source, delta);
    if (!payload.ok())
      return payload.status();
    auto checked = plugin_internal::validate_result_contract(
        *actor.query.prepared, true, actor.query.output_index, payload.value(),
        payload.value().descriptor().value(), nullptr, resources_,
        active_token(), [&](auto n) { return operation_work(actor, n); },
        options_.maximum_result_window_bytes, ErrorCode::OperationFailed,
        actor.query.shared_stage);
    if (!checked.ok())
      return checked;
    auto evidence = actor.tile_record.freeze(resources_);
    if (!evidence.ok())
      return evidence.status();
    auto frozen = evidence.take_value();
    TileRecord next(resources_);
    next.unit = actor.tile_record.unit;
    checked = next.inherit(frozen, resources_);
    if (!checked.ok())
      return checked;
    actor.shared_commits.push_back(
        {std::move(delta), payload.take_value(), frozen,
         actor.fallback.taint ||
             actor.query.backend != actor.contract().backend});
    actor.shared_visible = visible.take_value();
    actor.shared_evidence = std::move(frozen);
    actor.tile_record = std::move(next);
    return Status::success();
  }
  Result<std::optional<SharedGrant>> shared_grant(
      Actor& producer, const ResultSharedNeed& need) {
    using Answer = Result<std::optional<SharedGrant>>;
    auto request =
        need.tiles
            ? need.tiles->resolve(producer.contract().tile_grids)
            : TileRequest::all(producer.contract().tile_grids, resources_);
    if (!request.ok())
      return Answer(request.status());
    if (!producer.shared_visible.contains(request.value())) {
      if (producer.terminal != ErrorCode::Ok)
        return Answer(producer.service_failure->snapshot());
      if (producer.complete)
        return Answer(protocol_failure("shared stage omitted requested tiles"));
      return Answer(std::optional<SharedGrant>{});
    }
    content_internal::Sha256 identity;
    identity.text("shared-grant.v2");
    identity.integer(request.value().members.size());
    for (const auto& member : request.value().members) {
      identity.integer(member.first);
      identity.integer(member.second.intervals().size());
      for (auto interval : member.second.intervals()) {
        identity.integer(interval.begin);
        identity.integer(interval.end);
      }
    }
    const auto key = identity.finish();
    const auto cached = producer.shared_grants.find(std::string_view(key));
    if (cached != producer.shared_grants.end())
      return Answer(std::optional<SharedGrant>(cached->second));
    auto made =
        ResultBuilder::start(resources_, *producer.query.output.result_schema,
                             key, {}, {}, plan_.tile_extent());
    if (!made.ok())
      return Answer(made.status());
    auto builder = made.take_value();
    std::shared_ptr<const TileEvidence> evidence;
    bool fallback_taint = false;
    for (const auto& commit : producer.shared_commits) {
      auto facts = commit.result.descriptor();
      if (!facts.ok())
        return Answer(facts.status());
      for (const auto& member : request.value().members) {
        const auto produced = commit.tiles.members.find(member.first);
        if (produced == commit.tiles.members.end())
          continue;
        auto selected = member.second.intersect(produced->second, resources_);
        if (!selected.ok())
          return Answer(selected.status());
        if (selected.value().empty())
          continue;
        evidence = commit.evidence;
        fallback_taint |= commit.fallback_taint;
        auto retained = builder.reference().retain_association(
            commit.result.association(), [&](auto n) { return consume(n); });
        if (!retained.ok())
          return Answer(retained);
        auto samples = producer.contract().tile_grids[member.first].coverage(
            selected.value(), set_limits());
        if (!samples.ok())
          return Answer(samples.status());
        for (const auto& box : samples.value().boxes()) {
          auto appended = append_unit_view(builder, commit.result,
                                           facts.value(), box, member.first);
          if (!appended.ok())
            return Answer(appended);
        }
      }
    }
    auto result = builder.seal();
    if (!result.ok())
      return Answer(result.status());
    SharedGrant grant{result.take_value(), std::move(evidence), fallback_taint};
    producer.shared_grants.emplace(
        ResourceString(key, ResourceAllocator<char>(resources_)), grant);
    return Answer(std::optional<SharedGrant>(std::move(grant)));
  }
  Status supply_one_need_step(std::size_t index, Actor& actor,
                              const StructuredNeedGrants& need,
                              NeedCursor& cursor, bool replay) {
    if (cursor.next < need.shared_stages.size()) {
      const auto& selected = need.shared_stages[cursor.next];
      const auto stage = selected.stage;
      if (selected.tiles && selected.tiles->empty()) {
        auto empty = ResultBuilder::start(
            resources_,
            *plan_.steps()[index].shared_stages[stage]->output_result_schema,
            "empty-stage-grant", {}, {}, plan_.tile_extent());
        if (!empty.ok())
          return empty.status();
        auto empty_builder = empty.take_value();
        auto sealed = empty_builder.seal();
        if (!sealed.ok())
          return sealed.status();
        actor.shared_results[stage] = sealed.take_value();
        ++cursor.next;
        return Status::success();
      }

      if (!cursor.producer) {
        auto acquired = shared_actor(index, stage);
        if (!acquired.ok())
          return acquired.status();
        cursor.producer = acquired.take_value();
      }
      auto ready = shared_grant(*cursor.producer, selected);
      if (!ready.ok())
        return ready.status();
      if (!ready.value()) {
        // A committed tile is immutable and needs no edge to the producer's
        // unfinished continuation. Only missing coverage can block this actor.
        auto connected = connect_wait(actor, cursor.producer);
        if (!connected.ok())
          return connected;
        return advance(cursor.producer->index, *cursor.producer);
      }
      if (ready.value()->evidence) {
        auto inherited =
            actor.tile_record.inherit(ready.value()->evidence, resources_);
        if (!inherited.ok())
          return inherited;
      }
      actor.fallback.taint |= ready.value()->fallback_taint;
      actor.shared_results[stage] = ready.value()->result;
      actor.input_facts[{UINT32_MAX - stage,
                         ready.value()->result.object_id()}] =
          ready.value()->result.descriptor().value().revision();
      actor.waiting.reset();
      cursor.producer.reset();
      ++cursor.next;
      return Status::success();
    }
    const auto tensor = cursor.next - need.shared_stages.size();
    if (tensor >= need.tiles.size())
      return Status::success();
    return supply_tensor_need(index, actor, need, need.tiles[tensor], cursor,
                              replay);
  }
  Status replay_need(std::size_t index, Actor& actor,
                     const ResultProgramNeed& need) {
    NeedCursor cursor;
    while (!cursor.complete()) {
      auto status = supply_need_step(index, actor, need, cursor, true);
      if (!status.ok())
        return status;
    }
    return Status::success();
  }
  Status validate_publication(std::size_t, Actor& actor,
                              const ResultPublication& published) {
    struct Services final : PublicationValidationServices {
      Services(StructuredExecution& owner, Actor& actor)
          : owner(owner), actor(actor) {}
      Status consume(std::uint64_t n) override { return owner.consume(n); }
      Status operation_work(std::uint64_t n) override {
        return owner.work_mode() ? owner.checkpoint_cache_work(n)
                                 : owner.operation_work(actor, n);
      }
      Status run_work(std::uint64_t n) override {
        return owner.work_mode() ? owner.checkpoint_cache_work(n)
                                 : owner.admit_run_work(n);
      }
      ErrorCode stop() override { return owner.active_stop(); }
      CancellationToken cancellation() override { return owner.active_token(); }
      FootprintLimits set_limits() override { return owner.set_limits(); }
      StructuredExecution& owner;
      Actor& actor;
    } services(*this, actor);
    PublicationValidationView view{
        actor.contract(),
        actor.query,
        actor.published,
        actor.published_revision,
        actor.node_id,
        actor.input_facts,
        resources_,
        options_.maximum_result_window_bytes,
        services,
        actor.is_shared() ||
            (actor.gated &&
             (actor.query.prepared->requires_complete_validation(
                  true, actor.query.output_index) ||
              (actor.query.output.result_schema->publication_scope(
                   actor.query.tensor_slot) &&
               actor.query.output.result_schema
                       ->publication_scope(actor.query.tensor_slot)
                       ->policy == ResultPublicationPolicy::CompleteBundle)))};
    return ResultPublicationValidator::validate(view, published);
  }
  Result<ResultRef> stage_publication(std::size_t index, Actor& actor,
                                      const ResultPublication& published) {
    using Answer = Result<ResultRef>;
    auto checked = validate_publication(index, actor, published);
    if (!checked.ok())
      return Answer(checked);
    auto descriptor = published.result.descriptor(published.complete);
    if (!descriptor.ok())
      return Answer(descriptor.status());
    auto captured = published.result.capture();
    return captured;
  }
  Status commit_publication(
      std::size_t index, Actor& actor, ResultRef captured, bool complete,
      bool from_cache, bool send_notification,
      const TileRecords::Snapshot* ancestry = nullptr,
      const std::function<Status(std::uint64_t)>& cache_charge = {},
      CachePublicationTransaction* transaction = nullptr,
      std::shared_ptr<plugin_internal::ResultPayloadBound> accounted_payload =
          {}) {
    auto descriptor = captured.descriptor(complete);
    if (!descriptor.ok())
      return retire(actor, descriptor.status());
    if (!from_cache) {
      std::optional<ResultDescriptor> previous;
      if (actor.published.valid()) {
        auto old = actor.published.descriptor(false);
        if (!old.ok())
          return retire(actor, old.status());
        previous = old.take_value();
      }
      const auto& facts = descriptor.value();
      auto charged = consume(facts.tensor_count());
      if (!charged.ok())
        return retire(actor, charged);
      std::uint64_t elements = 0;
      bool saturated = false;
      const auto add = [&](std::uint64_t count) {
        add_computed_elements(&elements, &saturated, count);
      };

      for (std::uint32_t slot = 0; slot < facts.tensor_count(); ++slot) {
        const auto& covered = facts.tensor_coverage(slot);
        auto counted = consume(covered.boxes().size());
        if (!counted.ok())
          return retire(actor, counted);
        auto increment = covered;
        if (previous) {
          auto added =
              covered.subtract(previous->tensor_coverage(slot), set_limits());
          if (!added.ok())
            return retire(actor, added.status());
          counted = consume(added.value().boxes().size());
          if (!counted.ok())
            return retire(actor, counted);
          increment = added.take_value();
        }
        auto count = increment.element_count();
        if (!count.ok()) {
          // element_count only reports ResourceExhausted for unrepresentable
          // cardinality; it performs no resource admission or payload reads.
          if (count.status().code != ErrorCode::ResourceExhausted)
            return retire(actor, count.status());
          add_computed_elements(&elements, &saturated, UINT64_MAX, true);
        } else {
          add(count.value());
        }
      }
      if (elements) {
        auto timing = std::find_if(
            diagnostics_.operation_timings.begin(),
            diagnostics_.operation_timings.end(), [&](const auto& item) {
              return item.output == plan_.steps()[actor.index].result_ref() &&
                     item.backend == actor.query.backend &&
                     item.shared_stage == actor.query.shared_stage;
            });
        if (timing == diagnostics_.operation_timings.end())
          return retire(actor, protocol_failure(
                                   "missing Result publication diagnostics"));
        add_computed_elements(&timing->computed_elements,
                              &timing->computed_elements_saturated, elements,
                              saturated);
      }
    }
    Status status;
    if (complete && !actor.is_shared() && !actor.gated) {
      status = tile_records_->install(0, actor.tile_record, captured,
                                      &actor.tile_publication, ancestry,
                                      cache_charge);
      if (!status.ok())
        return from_cache && status.code == ErrorCode::ResourceExhausted
                   ? status
                   : retire(actor, status);
    }
    if (accounted_payload) {
      payload_bounds_[index]->commit_candidate(*accounted_payload);
    } else if (!actor.is_shared() && payload_bounds_[index]) {
      ResultProgramPhase phase{actor.query,
                               actor.results,
                               resources_.allocator(),
                               resources_,
                               [&](std::uint64_t n) { return consume(n); },
                               actor.failure,
                               {}};
      phase.tensors = &actor.tensors;
      phase.shared_stages = &actor.shared_results;
      auto accounted = payload_bounds_[index]->capture(phase);
      if (accounted.ok())
        accounted = payload_bounds_[index]->check(captured, phase);
      if (!accounted.ok())
        return retire(actor, accounted);
    }
    if (actor.is_shared()) {
      auto committed = commit_shared(actor, captured, complete);
      if (!committed.ok())
        return retire(actor, committed);
    }
    actor.published = std::move(captured);
    actor.published_revision = descriptor.value().revision();
    actor.complete = complete;
    if (!actor.is_shared())
      actor.visibility_ready = !actor.gated;
    if (transaction) {
      transaction->lock.unlock();
    }
    if (send_notification && !actor.is_shared() && actor.visibility_ready)
      status = notify(actor);
    if (!status.ok())
      return status;
    if (actor.complete) {
      if (!from_cache && !actor.is_shared() && !actor.gated)
        retain_cached_result(index, actor);
      actor.cache.replay.clear();
      actor.continuation = {};
      actor.results.clear();
      actor.shared_results.clear();
      actor.descriptors.clear();
      actor.tensors.clear();

      if (!actor.is_shared())
        actor.input_facts.clear();
      release_actor_registration(actor);
    }
    return actor.is_shared() || actor.visibility_ready
               ? complete_liveness(actor)
               : Status::success();
  }
  Status publish_object(std::size_t index, Actor& actor,
                        const ResultPublication& published,
                        bool from_cache = false) {
    auto captured = stage_publication(index, actor, published);
    return captured.ok()
               ? commit_publication(index, actor, captured.take_value(),
                                    published.complete, from_cache, true)
               : retire(actor, captured.status());
  }
  // One synchronous poll's borrowed services and Root-managed scratch state.
  // The phase and its callbacks are destroyed before this stack object; no
  // callback may retain an invocation or phase pointer after poll retirement.
  struct PollInvocation final {
    StructuredExecution& owner;
    std::size_t index;
    Actor& actor;
    const PlanStep& step;
    PollPhase& stage;
    ErrorCode& sticky;
    Result<ResultProgramPoll>& polled;
    const StructuredServices* services;
    bool optional;
    ResourceBudget& resources_;
    const ExecutionOptions& options_;
    CancellationToken token;
    bool in_block = false, in_discovery = false;
    ResourceVector<std::uint64_t> association;
    ResourceVector<std::shared_ptr<const ResultDiscoveryReceipt>> discovered;
    std::uint64_t discovered_count = 0;
    PollInvocation(StructuredExecution& execution, std::size_t step_index,
                   Actor& current, const StructuredServices* supplied,
                   bool replay)
        : owner(execution),
          index(step_index),
          actor(current),
          step(current.contract()),
          stage(std::get<PollPhase>(actor.phase)),
          sticky(stage.sticky),
          polled(stage.result),
          services(supplied),
          optional(replay),
          resources_(execution.resources_),
          options_(execution.options_),
          token(execution.active_token()),
          association(ResourceAllocator<std::uint64_t>(resources_)),
          discovered(
              ResourceAllocator<std::shared_ptr<const ResultDiscoveryReceipt>>(
                  resources_)) {}
    Result<std::uint64_t> workspace_limit() {
      auto limit = step.traits.workspace_bytes;
      if (step.traits.workspace_input_multiplier) {
        const auto add_input = [&](const Footprint& samples, ElementType type) {
          auto count = samples.element_count();
          const auto factor = TensorSpan::element_size(type) *
                              static_cast<std::uint64_t>(
                                  step.traits.workspace_input_multiplier);
          if (!count.ok() ||
              !core_internal::can_multiply_add(count.value(), factor, limit))
            return false;
          limit += count.value() * factor;
          return true;
        };
        for (const auto& input : actor.tensors)
          if (!add_input(input.second.coverage(),
                         input.second.spec().descriptor.element_type))
            return Result<std::uint64_t>(
                Status{ErrorCode::ResourceExhausted, {}});
      }
      return Result<std::uint64_t>(limit);
    }
    void observe_failure(const Status& failed) {
      if (sticky != ErrorCode::Ok)
        actor.service_failure->record(
            sticky == ErrorCode::InvalidArgument
                ? Status{sticky,
                         {},
                         FailureReason::UnauthorizedRead,
                         {FailureOrigin::Protocol, FailureScope::Group}}
                : Status{sticky, {}});
      auto first = actor.service_failure->record(failed);
      auto expected = ErrorCode::Ok;
      actor.failure->compare_exchange_strong(expected, first.code);
    }
    Status work(std::uint64_t count) {
      auto admitted = owner.admit_actor_work(actor, count);
      if (!admitted.ok()) {
        observe_failure(admitted);
        return admitted;
      }
      const auto previous = sticky;
      const auto stopped = owner.execution_stop(token);
      auto result = Status{stopped, {}};
      if (result.ok() && in_discovery) {
        auto available =
            actor.discovery_work_remaining.load(std::memory_order_relaxed);
        do {
          if (count > available) {
            result = {ErrorCode::ResourceExhausted,
                      "GPU discovery work limit",
                      FailureReason::WorkLimit,
                      {FailureOrigin::Resource, FailureScope::Run}};
            break;
          }
        } while (!actor.discovery_work_remaining.compare_exchange_weak(
            available, available - count, std::memory_order_relaxed));
        if (result.ok())
          result = owner.admit_run_work(count);
      }
      if (result.ok()) {
        result = resources_.consume({count});
      }
      if (!result.ok()) {
        // Root admission also raises the code-only allocator flag.
        // This service owns that first cause and records its complete
        // status before the generic flag can discard reason/scope.
        if (previous == ErrorCode::Ok)
          actor.service_failure->record(result);
        observe_failure(result);
      }
      return result;
    }
    Status initialize_association() {
      auto facts_work = work(actor.input_facts.size());
      if (!facts_work.ok())
        return facts_work;
      association.reserve(actor.input_facts.size());
      for (const auto& input : actor.input_facts)
        association.push_back(input.first.second);
      return Status::success();
    }
    Status report_diagnostic(std::uint32_t index, ByteView report) {
      auto status = work(1);
      if (status.ok())
        status = stage.reports ? stage.reports->report(index, report)
                               : Status{ErrorCode::InvalidArgument,
                                        "undeclared diagnostic"};
      if (!status.ok())
        observe_failure(status);
      return status;
    }
    Result<std::optional<ResultCheckpoint>> borrow_checkpoint(
        std::uint32_t kind, std::uint64_t before) {
      using Answer = Result<std::optional<ResultCheckpoint>>;
      try {
        CallbackContext scope(&owner, &actor, services, optional);
        if (in_discovery) {
          auto failed = protocol_failure("checkpoint inside GPU discovery");
          observe_failure(failed);
          return Answer(failed);
        }
        actor.fallback.retry_safe = false;
        auto found =
            owner.checkpoint_before(index, actor, kind, before, association);
        if (!found.ok())
          observe_failure(found.status());
        return found;
      } catch (const std::bad_alloc&) {
        Status failed{ErrorCode::ResourceExhausted, {}};
        observe_failure(failed);
        return Answer(failed);
      } catch (...) {
        Status failed{ErrorCode::OperationFailed, {}};
        observe_failure(failed);
        return Answer(failed);
      }
    }
    Status publish_checkpoint(std::uint32_t kind, std::uint64_t sequence,
                              const ResultRef& state) {
      Status published;
      try {
        CallbackContext scope(&owner, &actor, services, optional);
        if (in_discovery) {
          auto failed = protocol_failure("checkpoint inside GPU discovery");
          observe_failure(failed);
          return failed;
        }
        actor.fallback.retry_safe = false;
        published =
            owner.checkpoint_publish(index, actor, kind, sequence, state);
      } catch (const std::bad_alloc&) {
        published = Status{ErrorCode::ResourceExhausted, {}};
      } catch (...) {
        published = Status{ErrorCode::OperationFailed, {}};
      }
      if (!published.ok())
        observe_failure(published);
      return published;
    }
    Result<ResultRef> block(const ResultProgramPhase& phase, std::uint32_t kind,
                            std::uint64_t begin, std::uint64_t end,
                            std::uint64_t mode, const ResultRef& incoming,
                            const std::function<Result<ResultRef>()>& compute) {
      using Answer = Result<ResultRef>;
      try {
        CallbackContext context(&owner, &actor, services, optional);
        if (in_discovery || in_block) {
          auto failed =
              protocol_failure("nested Result pure block or GPU discovery");
          observe_failure(failed);
          return Answer(failed);
        }
        struct Scope {
          bool& value;
          ~Scope() { value = false; }
        } scope{in_block};
        in_block = true;
        actor.fallback.retry_safe = false;
        auto result = owner.evaluate_block(index, actor, phase, kind, begin,
                                           end, mode, incoming, compute);
        if (!result.ok())
          observe_failure(result.status());
        return result;
      } catch (const std::bad_alloc&) {
        Status failed{ErrorCode::ResourceExhausted, {}};
        observe_failure(failed);
        return Answer(failed);
      } catch (...) {
        Status failed{ErrorCode::OperationFailed, {}};
        observe_failure(failed);
        return Answer(failed);
      }
    }
    Result<std::shared_ptr<const ResultDiscoveryReceipt>> discover(
        const ResultProgramPhase& phase, std::uint32_t capacity,
        std::uint32_t candidates,
        const std::function<Status(const ResultGpuRequestTable&)>& compute) {
      using Answer = Result<std::shared_ptr<const ResultDiscoveryReceipt>>;
      auto result = [&]() -> Answer {
        try {
          if (in_block || in_discovery || !phase.gpu)
            return Answer(
                protocol_failure("invalid Result GPU discovery scope"));
          struct Scope {
            bool& value;
            ~Scope() { value = false; }
          } scope{in_discovery};
          in_discovery = true;
          actor.fallback.retry_safe = false;
          const auto before = owner.active_services()->gpu_dispatches();
          auto needs = ResultNativeScope::discover(
              phase.query, capacity, candidates, compute,
              [&](std::uint64_t n) { return work(n); });
          if (!needs.ok())
            return Answer(needs.status());
          if (owner.active_services()->gpu_dispatches() == before)
            return Answer(
                Status{ErrorCode::OperationFailed,
                       "Result GPU discovery submitted no native work"});
          const auto maximum = std::min<std::uint64_t>(
              65536, options_.dependencies.sets.maximum_boxes);
          if (!core_internal::can_add(discovered_count, needs.value().size(),
                                      maximum))
            return Answer(Status{ErrorCode::ResourceExhausted,
                                 "GPU discovery need limit"});
          auto receipt = std::allocate_shared<ResultDiscoveryReceipt>(
              ResourceAllocator<ResultDiscoveryReceipt>(resources_));
          receipt->tiles = needs.take_value();
          discovered_count += receipt->tiles.size();
          discovered.push_back(receipt);
          return Answer(std::move(receipt));
        } catch (const std::bad_alloc&) {
          return Answer(Status{ErrorCode::ResourceExhausted, {}});
        } catch (...) {
          return Answer(Status{ErrorCode::OperationFailed, {}});
        }
      }();
      if (!result.ok())
        observe_failure(result.status());
      return result;
    }
    Status append_discoveries(ResultProgramNeed& need) {
      auto charged = work(discovered_count + need.tiles.size());
      const auto maximum = std::min<std::uint64_t>(
          65536, options_.dependencies.sets.maximum_boxes);
      if (charged.ok() &&
          !core_internal::can_add(need.tiles.size(), discovered_count, maximum))
        charged = {ErrorCode::ResourceExhausted,
                   "GPU discovery attachment limit"};
      if (!charged.ok())
        return charged;
      for (const auto& receipt : discovered)
        for (const auto& tensor : receipt->tiles)
          need.tiles.push_back(tensor);
      return Status::success();
    }
    void attach_discoveries() {
      if (!polled.ok() || !discovered_count)
        return;
      auto response = polled.take_value();
      auto* need = std::get_if<ResultProgramNeed>(&response);
      if (!need) {
        observe_failure(protocol_failure(
            "GPU discovery requires supply before completion"));
      } else {
        auto charged = append_discoveries(*need);
        if (!charged.ok())
          observe_failure(charged);
      }
      polled = Result<ResultProgramPoll>(std::move(response));
    }
    void check_native_status() {
      if (!owner.active_services() || !owner.active_services()->gpu_status)
        return;
      auto native_status = owner.active_services()->gpu_status();
      if (native_status.code == ErrorCode::InvalidArgument) {
        native_status.reason = FailureReason::UnauthorizedRead;
        native_status.detail.origin = FailureOrigin::Protocol;
        native_status.detail.scope = FailureScope::Group;
      }
      if (!native_status.ok())
        observe_failure(native_status);
    }
    void record_native_dispatches() {
      if (actor.query.backend != Backend::Gpu || !owner.active_services() ||
          !owner.active_services()->gpu_dispatches)
        return;
      const auto count = owner.active_services()->gpu_dispatches();
      stage.native_dispatches = count;
      if (!core_internal::can_add(count, actor.native_dispatches))
        observe_failure(Status{ErrorCode::ResourceExhausted,
                               "native dispatch counter overflow"});
      else
        actor.native_dispatches += count;
    }

    Status finish() {
      if (sticky != ErrorCode::Ok) {
        auto expected = ErrorCode::Ok;
        actor.failure->compare_exchange_strong(expected, sticky);
      }
      auto failure = actor.service_failure->snapshot();
      if (!failure.ok())
        return failure;
      const auto code = actor.failure->load();
      if (code == ErrorCode::InvalidArgument)
        return Status{code,
                      {},
                      FailureReason::UnauthorizedRead,
                      {FailureOrigin::Protocol, FailureScope::Group}};
      return Status{code, {}};
    }
  };
  Status poll_actor_phase(
      std::size_t index, const std::shared_ptr<Actor>& current,
      const std::function<Result<ResultProgramPoll>(const ResultProgramPhase&)>&
          poll) {
    const auto* services = active_services();
    const auto optional = work_mode();
    CallbackContext member_context(this, current.get(), services, optional);
    auto& actor = *current;
    auto& stage = std::get<PollPhase>(actor.phase);
    ResourceAllocationScope metadata_scope(resources_, &stage.sticky);
    PollInvocation invocation(*this, index, actor, services, optional);
    auto limit = invocation.workspace_limit();
    if (!limit.ok())
      return limit.status();
    auto allocator =
        (active_services() ? active_services()->allocator
                           : resources_.allocator())
            .limited(limit.value(),
                     [full = actor.service_failure](const Status& status) {
                       full->record(status);
                     });
    auto observe_failure = [&](const Status& failed) {
      invocation.observe_failure(failed);
    };
    auto work = [&](std::uint64_t count) { return invocation.work(count); };
#if defined(PHOTOSPIDER_ENABLE_EXECUTION_TEST_HOOKS)
    const auto& step = plan_.steps()[index];
    bool in_poll = false;
    std::uint64_t phase_work_ordinal = 0;
    const auto poll_thread = std::this_thread::get_id();
    auto phase_work = [&](std::uint64_t count) {
      if (std::this_thread::get_id() != poll_thread || !in_poll || !count)
        return work(count);
      execution_testing::PhaseWorkPoint point{
          step.operation, actor.query.backend,  &actor,
          actor.polls,    ++phase_work_ordinal, count};
      execution_testing::notify_phase_work_started(point);
      auto status = work(count);
      execution_testing::notify_phase_work_finished(point, status.code);
      return status;
    };
#else
    auto& phase_work = work;
#endif
    ResultProgramPhase phase{actor.query,    actor.results, allocator,
                             resources_,     phase_work,    actor.failure,
                             observe_failure};
    auto facts_work = invocation.initialize_association();
    if (!facts_work.ok())
      return facts_work;
    phase.report_diagnostic = [&](std::uint32_t type, ByteView report) {
      return invocation.report_diagnostic(type, report);
    };
    phase.failure_latch = actor.service_failure;
    phase.association = &invocation.association;
    phase.tensors = &actor.tensors;
    phase.shared_stages = &actor.shared_results;
    if (!actor.is_shared() && payload_bounds_[index]) {
      auto captured = payload_bounds_[index]->capture(phase);
      if (!captured.ok())
        return captured;
    }
    phase.descriptors = &actor.descriptors;
    if (!actor.is_shared()) {
      phase.checkpoint_before = [&](std::uint32_t kind, std::uint64_t before) {
        return invocation.borrow_checkpoint(kind, before);
      };
      phase.checkpoint_publish = [&](std::uint32_t kind, std::uint64_t sequence,
                                     const ResultRef& state) {
        return invocation.publish_checkpoint(kind, sequence, state);
      };
      phase.block = [&](std::uint32_t kind, std::uint64_t begin,
                        std::uint64_t end, std::uint64_t mode,
                        const ResultRef& incoming,
                        const std::function<Result<ResultRef>()>& compute) {
        return invocation.block(phase, kind, begin, end, mode, incoming,
                                compute);
      };
    }
    phase.discover =
        [&, entry_thread = std::this_thread::get_id(), failure = actor.failure,
         first = actor.service_failure](
            std::uint32_t capacity, std::uint32_t candidates,
            const std::function<Status(const ResultGpuRequestTable&)>&
                compute) {
          using Answer = Result<std::shared_ptr<const ResultDiscoveryReceipt>>;
          if (std::this_thread::get_id() != entry_thread || in_cpu_range ||
              !active_services()) {
            Status failed{ErrorCode::InvalidArgument,
                          {},
                          FailureReason::UnauthorizedRead,
                          {FailureOrigin::Protocol, FailureScope::Group}};
            auto recorded = first->record(failed);
            auto expected = ErrorCode::Ok;
            failure->compare_exchange_strong(expected, recorded.code);
            return Answer(std::move(failed));
          }
          return invocation.discover(phase, capacity, candidates, compute);
        };
    if (active_services()) {
      phase.cpu_parallel = active_services()->cpu_parallel;
      phase.cpu_tiles = active_services()->cpu_tiles;
      phase.gpu = active_services()->gpu;
      phase.gpu_status = active_services()->gpu_status;
    }
    ResultGpuService gpu(phase.gpu, phase.gpu_status, observe_failure);
    phase.gpu = gpu.get();
    stage.invoked = true;
#if defined(PHOTOSPIDER_ENABLE_EXECUTION_TEST_HOOKS)
    struct PollObservationScope {
      bool& active;
      ~PollObservationScope() { active = false; }
    };
    {
      PollObservationScope observation{in_poll};
      in_poll = true;
      stage.result = poll(phase);
    }
#else
    stage.result = poll(phase);
#endif
    invocation.attach_discoveries();
    invocation.check_native_status();
    invocation.record_native_dispatches();
    return invocation.finish();
  }
  Status advance_actor_need(std::size_t index, Actor& actor) {
    if (!actor.busy) {
      actor.busy = true;
      actor.driver = std::this_thread::get_id();
    }
    Status supplied;
    try {
      supplied = actor.advance_need(
          [&](const auto& request, auto& cursor) {
            return supply_need_step(index, actor, request, cursor);
          },
          [&](const auto& request) {
            retain_cache_need(index, actor, request);
          });
    } catch (const std::bad_alloc&) {
      return retire(actor, Status{ErrorCode::ResourceExhausted, {}});
    } catch (...) {
      return retire(actor, Status{ErrorCode::OperationFailed, {}});
    }
    return supplied.ok() ? supplied : retire(actor, supplied);
  }
  Status submit_actor_poll(std::size_t index,
                           const std::shared_ptr<Actor>& current) {
    auto& actor = *current;
    const auto& step = actor.contract();
    actor.busy = true;
    actor.driver = std::this_thread::get_id();
    return submit_actor(
        current,
        [this, index, current] {
          return poll_actor_phase(index, current,
                                  [&](const ResultProgramPhase& phase) {
                                    return current->continuation.poll(phase);
                                  });
        },
        actor.query.backend, step.traits.cpu_staged_tiles);
  }
  Status advance(std::size_t index, Actor& actor) {
    if (actor.driving)
      return Status{ErrorCode::Cycle, {}};
    if (!callback_context() && actor.visit == traversal_)
      return Status::success();
    actor.visit = traversal_;
    actor.driving = true;
    struct Driving {
      bool& active;
      ~Driving() { active = false; }
    } driving{actor.driving};
    auto current = actor.self.lock();
    if (!current)
      return protocol_failure("structured Actor owner is absent");
    struct ExceptionDrain {
      Actor& actor;
      int exceptions = std::uncaught_exceptions();
      ~ExceptionDrain() {
        if (std::uncaught_exceptions() > exceptions && actor.pending)
          actor.pending->wait();
      }
    } exception_drain{actor};
    struct Idle {
      Actor& actor;
      ~Idle() {
        if (!actor.pending && !actor.waiting)
          actor.busy = false;
      }
    } idle{actor};
    struct ResetPhase {
      Actor& actor;
      ~ResetPhase() {
        if (!actor.pending && (actor.terminal != ErrorCode::Ok ||
                               !std::holds_alternative<NeedPhase>(actor.phase)))
          actor.phase.emplace<std::monostate>();
      }
    } reset_phase{actor};
    if (actor.deferred_start) {
      auto submitted = start_actor(index, current);
      return submitted.ok() ? submitted : retire(actor, submitted);
    }
    if (actor.queued)
      return Status::success();
    if (actor.pending && !actor.pending->ready()) {
      actor.pending->completion.wait_for(std::chrono::milliseconds(2));
      if (!actor.pending->ready()) {
        return Status::success();
      }
    }
    if (std::holds_alternative<StartPhase>(actor.phase)) {
      auto dispatched = finish_actor(current);
      auto started = actor.take_start(std::move(dispatched));
      return complete_start(index, current, std::move(started));
    }
    if (std::holds_alternative<NeedPhase>(actor.phase))
      return advance_actor_need(index, actor);
    Status status;
    if (!actor.pending) {
      const auto& step = actor.contract();
      auto charged = consume(1);
      if (!charged.ok())
        return retire(actor, charged);
      auto admitted = actor.begin_poll(
          std::min(step.traits.outputs[0].maximum_dependency_stages,
                   options_.dependencies.maximum_stages),
          actor.failure->load(), "structured stage limit");
      if (!admitted.ok())
        return retire(actor, admitted);
      if (actor.query.prepared &&
          !actor.query.prepared->diagnostic_definitions().empty()) {
        auto& reports = std::get<PollPhase>(actor.phase).reports;
        reports.emplace(resources_);
        auto status =
            reports->initialize(actor.query.prepared->diagnostic_definitions());
        if (!status.ok())
          return retire(actor, status);
      }
      status = submit_actor_poll(index, current);
    }
    if (status.ok() && actor.queued)
      return Status::success();
    if (status.ok() && !actor.pending->ready()) {
      actor.pending->completion.wait_for(std::chrono::milliseconds(2));
      if (!actor.pending->ready())
        return Status::success();
    }
    if (status.ok())
      status = finish_actor(current);
    auto completed = actor.take_poll();
    return complete_poll(index, current, std::move(status),
                         std::move(completed));
  }
  Status complete_poll(std::size_t index, const std::shared_ptr<Actor>& current,
                       Status status, PollPhase completed,
                       const ResultRef* certified = nullptr) {
    auto& actor = *current;
    const auto& step = actor.contract();
    auto& polled = completed.result;
    auto sticky = completed.sticky;
    const auto host_failure = actor.service_failure->snapshot();
    if (host_failure.detail.origin == FailureOrigin::Protocol)
      status = host_failure;
    else if (sticky == ErrorCode::InvalidArgument ||
             actor.failure->load() == ErrorCode::InvalidArgument)
      status = Status{ErrorCode::InvalidArgument,
                      {},
                      FailureReason::UnauthorizedRead,
                      {FailureOrigin::Protocol, FailureScope::Group}};
    const auto& failed = status.ok() ? polled.status() : status;
    const auto operation_failure = actor.failure->load();
    if (!actor.is_shared() &&
        actor.fallback.can_retry_poll(
            step.traits, actor.query.backend, failed, host_failure,
            *actor.service_failure, actor.native_dispatches,
            actor.published.valid(), sticky, operation_failure) &&
        active_stop() == ErrorCode::Ok) {
      // The retry is a new attempt. Escaped old capabilities retain their old
      // failure owners, while CPU preparation receives fresh sticky records.
      auto retry_failure = std::allocate_shared<std::atomic<ErrorCode>>(
          ResourceAllocator<std::atomic<ErrorCode>>(resources_), ErrorCode::Ok);
      auto retry_host_failure =
          std::allocate_shared<plugin_internal::FailureLatch>(
              ResourceAllocator<plugin_internal::FailureLatch>(resources_));
      actor.failure = std::move(retry_failure);
      actor.service_failure = std::move(retry_host_failure);
      auto capacity = legacy_capacity(
          resources_, 2 * (step.operation.size() + failed.message.size() + 64));
      if (!capacity.ok())
        return retire(actor, capacity.status());
      diagnostics_.fallback_reasons.push_back(step.operation + ": " +
                                              failed.message);
      auto recorded = record_poll_diagnostics(actor, completed,
                                              ErrorCode::BackendUnavailable);
      if (!recorded.ok())
        return retire(actor, recorded);
      // A failed attempt owns no public output or mandatory I/O. Drop its
      // capabilities before restarting; Root work and total stages remain
      // spent.
      actor.continuation = {};
      actor.tensors.clear();
      actor.results.clear();
      actor.shared_results.clear();
      actor.descriptors.clear();

      actor.tile_record.inputs.clear();
      if (!actor.is_shared())
        actor.input_facts.clear();
      actor.cache.replay.clear();
      actor.cache.disabled = true;
      actor.fallback.taint = true;
      actor.query.backend = Backend::Cpu;
      auto retry = consume(1);
      if (!retry.ok())
        return retire(actor, retry);
      auto submitted = start_actor(index, current);
      return submitted.ok() ? submitted : retire(actor, submitted);
    }
    if (!status.ok() || !polled.ok()) {
      const auto first = status.ok() ? polled.status() : status;
      record_failed_poll(actor, completed, first);
      return retire(actor, first);
    }

    actor.tensors.clear();
    auto recorded = record_poll_diagnostics(actor, completed, ErrorCode::Ok);
    if (!recorded.ok())
      return retire(actor, recorded);
    if (!actor.is_shared())
      diagnostics_.selected_backends[step.result_ref()] = actor.query.backend;
    diagnostics_.peak_active_tasks =
        std::max(diagnostics_.peak_active_tasks, 1U);
    auto event = polled.take_value();
    if (auto* need = std::get_if<ResultProgramNeed>(&event)) {
      actor.phase.emplace<NeedPhase>(std::move(*need));
      return Status::success();
    }
    const auto& published = std::get<ResultPublication>(event);
    return certified ? commit_publication(index, actor, *certified,
                                          published.complete, false, false)
                     : publish_object(index, actor, published);
  }
  std::shared_ptr<const ExecutionPlan> plan_owner_;
  const ExecutionPlan& plan_;
  std::vector<ExecutionBinding> bindings_;
  std::shared_ptr<OperationRegistry> operations_;
  ResourceBudget resources_;
  core_internal::PayloadCapture payload_capture_;
  ResourceSet bindings_resources_;
  ExecutionOptions options_;
  const std::uint32_t maximum_parallelism_;
  ResultSubscription subscription_;
  CancellationToken cancellation_;
  std::function<ErrorCode()> stop_;
  StructuredDispatch dispatch_;
  const std::vector<std::string>& templates_;
  ResultCheckpoints* checkpoints_ = nullptr;
  ResultCache* blocks_ = nullptr;
  std::uint64_t block_epoch_ = 0;
  ResourceString snapshot_;
  ResourceString snapshot_input_;
  std::atomic<std::uint64_t> remaining_;
  StructuredResultCache cache_state_;
  bool optional_replay_ = false;
  ResourceVector<std::weak_ptr<Actor>> actors_;
  ResourceVector<std::shared_ptr<Actor>> actor_aliases_;
  ResourceVector<ResourceVector<std::weak_ptr<Actor>>> step_actors_;
  ResourceVector<std::uint64_t> open_units_;
  bool liveness_ready_ = false;
  ResourceMap<ActorRegistration> actor_queries_;
  ResourceVector<AcquisitionMap> acquisitions_;
  ResourceVector<PendingTask> pending_tasks_;
  ResourceVector<std::uint64_t> remaining_consumers_;
  ResourceVector<bool> named_pins_, finished_steps_;
  std::uint64_t wait_scan_epoch_ = 0;
  ResourceVector<std::shared_ptr<Actor>> requested_roots_;
  ResourceVector<TileRequest> pending_root_requests_;
  ResourceVector<TileAddress> root_cursors_;
  ResourceVector<std::shared_ptr<Actor>> shared_actors_;
  ResourceVector<std::shared_ptr<VisibilityGate>> visibility_gates_;
  ResourceVector<bool> complete_validation_admitted_;
  std::uint64_t progress_ = 0, traversal_ = 1;
  std::recursive_mutex callback_metadata_mutex_;
  ResourceVector<std::shared_ptr<plugin_internal::ResultPayloadBound>>
      payload_bounds_;
  ResourceVector<bool> pure_closure_, checkpoint_shareable_, result_cacheable_;

  ExecutionDiagnostics diagnostics_;
  std::shared_ptr<TileRecords> tile_records_;
};
Result<ExecutionResult> execute_structured(
    const ExecutionPlan& plan, std::vector<ExecutionBinding> bindings,
    std::shared_ptr<OperationRegistry> operations, ResourceBudget resources,
    const ExecutionOptions& options, const CancellationToken& cancellation,
    const std::function<ErrorCode()>& stop, const StructuredDispatch& dispatch,
    const DemandQuery* requested, const std::string& snapshot_identity,
    ResultCheckpoints* checkpoints, ResultCache* blocks,
    std::shared_ptr<const ExecutionPlan> plan_owner,
    std::uint32_t maximum_parallelism) {
  try {
    auto execution = std::allocate_shared<StructuredExecution>(
        ResourceAllocator<StructuredExecution>(resources), plan,
        std::move(bindings), std::move(operations), resources, options,
        cancellation, stop, dispatch, snapshot_identity, checkpoints, blocks,
        std::move(plan_owner), maximum_parallelism);
    return execution->run(requested);
  } catch (const std::bad_alloc&) {
    return Result<ExecutionResult>(Status{ErrorCode::ResourceExhausted, {}});
  } catch (...) {
    return Result<ExecutionResult>(Status{ErrorCode::OperationFailed, {}});
  }
}
}  // namespace ps::execution_internal
