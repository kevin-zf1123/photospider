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

#include "core/numeric_diagnostics.hpp"
#include "core/stored_failure.hpp"
#include "data/content_digest.hpp"
#include "data/input_validation.hpp"
#include "data/whole_input_view.hpp"
#include "execution/cpu_range_context.hpp"
#include "execution/dependency_records.hpp"
#include "execution/publication_diagnostics.hpp"
#include "execution/resource_observation.hpp"
#include "execution/result_blocks.hpp"
#include "execution/result_cache.hpp"
#include "execution/result_callback_scope.hpp"
#include "execution/result_checkpoints.hpp"
#include "execution/result_native.hpp"
#include "execution/result_subscription.hpp"
#include "execution/shared_results.hpp"
#include "photospider/data/representation.hpp"
#include "plugin/dependency_identity.hpp"
#include "plugin/failure_latch.hpp"
#include "plugin/operation_identity.hpp"
#include "plugin/result_need_validation.hpp"

#if defined(PHOTOSPIDER_ENABLE_EXECUTION_TEST_HOOKS)
#include "execution/execution_test_hooks.hpp"
#endif

namespace ps::execution_internal {
namespace {
// Native failures must enter the shared first-error latch before the caller
// can invoke another host service. The table and its context live for one poll.
template <class Observe>
class ResultGpuService final {
 public:
  ResultGpuService(const ps_gpu_service_v1* source,
                   const std::function<Status()>& status,
                   const Observe& observe)
      : source_(source), status_(status), observe_(observe) {
    if (!source_)
      return;
    service_ = *source_;
    service_.context = this;
    service_.buffer = [](void* context, const std::uint8_t* bytes,
                         std::uint64_t size, std::uint32_t writable,
                         std::uint64_t* token) noexcept {
      if (!context)
        return static_cast<int>(PS_GPU_RESULT_FAILURE_V1);
      auto& self = *static_cast<ResultGpuService*>(context);
      return self.finish(self.source_->buffer(self.source_->context, bytes,
                                              size, writable, token));
    };
    service_.execute = [](void* context, const ps_gpu_dispatch_v1* commands,
                          std::uint32_t count) noexcept {
      if (!context)
        return static_cast<int>(PS_GPU_RESULT_FAILURE_V1);
      auto& self = *static_cast<ResultGpuService*>(context);
      return self.finish(
          self.source_->execute(self.source_->context, commands, count));
    };
    service_.release = [](void* context, std::uint64_t token) noexcept {
      if (!context)
        return static_cast<int>(PS_GPU_RESULT_FAILURE_V1);
      auto& self = *static_cast<ResultGpuService*>(context);
      return self.finish(self.source_->release(self.source_->context, token));
    };
  }
  const ps_gpu_service_v1* get() const { return source_ ? &service_ : nullptr; }

 private:
  int finish(int result) noexcept {
    // The underlying service records illegal cross-thread calls atomically.
    // Only the owner may read poll-local flags; the final poll check collects
    // thread violations after the operator's workers have joined.
    if (std::this_thread::get_id() != owner_ || !status_)
      return result;
    try {
      auto failed = status_();
      if (failed.code == ErrorCode::InvalidArgument) {
        failed.reason = FailureReason::UnauthorizedRead;
        failed.detail = {FailureOrigin::Protocol, FailureScope::Group};
      }
      if (!failed.ok())
        observe_(failed);
    } catch (...) {
      observe_(Status{ErrorCode::ResourceExhausted, {}});
      return PS_GPU_RESULT_FAILURE_V1;
    }
    return result;
  }
  const ps_gpu_service_v1* source_;
  const std::function<Status()>& status_;
  const Observe& observe_;
  const std::thread::id owner_ = std::this_thread::get_id();
  ps_gpu_service_v1 service_{};
};
Status protocol(const char* message) {
  return Status{ErrorCode::InvalidArgument,
                message,
                FailureReason::MalformedEnvelope,
                {FailureOrigin::Protocol, FailureScope::Group}};
}
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
 * `service_peers` uses the same driver to progress producers needed by waiters.
 * `execute_structured` allocates the coordinator and its
 * shared control block from the execution Root. The coordinator owns its copied
 * bindings and options and copies the snapshot identity into Root-managed
 * storage. It retains an optional plan owner and aliases that plan; without an
 * owner, it borrows the caller's plan. Queued work may still reference
 * coordinator-owned run state. Callbacks install services and the effective
 * producer lease in `CallbackContext`. `run()` closes this caller's
 * subscription before deciding whether to hand off or drain. For a
 * frozen-plan Result producer, cancellation may arrive while its top-level
 * callback is still pending and a peer still needs that producer. In that
 * case `run()` closes the caller subscription and offers a strong coordinator
 * owner to `SharedResults`; it returns early only if adoption succeeds and the
 * peer still needs the producer. A failed adoption or vanished peer withdraws
 * that owner and keeps the synchronous drain; the same run does not retry the
 * handoff.
 *
 * Explicitly requested named Result roots enter the Actor frontier. A
 * potential CPU contract-1 joint candidate can remain deferred while its
 * consuming Actor validates and registers the full Need, so peer producer
 * inputs are present before the candidate is started for grouping.
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
 * submission until the driver retires it and merges them. A joint worker marks
 * entry and runs the callback; the driver accounts for the joint group once at
 * retirement. Poll timing starts before dispatch and ends at driver retirement,
 * so it includes queue and wave wait rather than measuring compute alone.
 * During joint Need preflight, member-local Cancelled/Stale status outside
 * Protocol and Run/Group scope retires only that member. Protocol-origin
 * errors and Run/Group-scoped failures remain enclosing joint failures.
 *
 * A peer's Result wait pumps the registry on that peer's calling thread. The
 * registry releases its table lock before driving. `driver_mutex_` serializes
 * the original run, peer driving, and draining; a contended ordinary pump
 * returns without inspecting actors. A driver waits for the pending callback
 * to retire before inspecting actor state or changing actor driver ids. Context
 * shutdown cancels shared work and drains registered coordinators outside the
 * registry lock. Only the pinned frozen-plan path can hand off; ordinary plans
 * and runs without a remaining peer drain synchronously. This does not make
 * synchronous cache replay or worker-internal paths cancellably detachable.
 *
 * Validated Needs can add computed producers to the pending frontier. A
 * completed non-contract-2 Whole Actor is eligible to mark aliased steps
 * finished only after every tensor slot has full coverage; liveness then
 * follows requested roots and each step's selected input indices. The
 * coordinator clears a published object and its producer lease only when all
 * aliases have no remaining consumers or named-output pin and have no pending
 * or joint work. Published Results and retained read windows keep their own
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
 * supply path with a local cursor. Legacy Value requests and synchronous scalar
 * preparation remain separate paths.
 * `Actor::driving` distinguishes active same-thread recursive dependency
 * servicing from an actor suspended on an upstream producer.
 * A run that does not hand off, and any destruction or explicit drain, waits
 * for the top-level pending callback, including nested work, to retire before
 * traversing actors for cleanup. A peer driver observes the same retirement
 * boundary. The shared registry owns a parked coordinator strongly, while
 * producer entries do not own their drivers. This prevents an entry/driver
 * ownership cycle.
 */
class StructuredExecution final
    : public SharedResults::Driver,
      public std::enable_shared_from_this<StructuredExecution> {
 public:
  /** @brief Marks an active coordinator driving scope. */
  struct DriverScope {
    bool& active;
    explicit DriverScope(bool& flag) : active(flag) { active = true; }
    ~DriverScope() { active = false; }
  };
  StructuredExecution(const ExecutionPlan& plan,
                      std::vector<ExecutionBinding> bindings,
                      std::shared_ptr<OperationRegistry> operations,
                      ResourceBudget resources, const ExecutionOptions& options,
                      const CancellationToken& cancellation,
                      std::function<ErrorCode()> stop,
                      StructuredDispatch dispatch, std::string_view snapshot,
                      SharedResults* shared_results,
                      ResultCheckpoints* checkpoints, ResultCache* blocks,
                      std::shared_ptr<const ExecutionPlan> plan_owner,
                      bool atom_outcomes, std::uint32_t maximum_parallelism)
      : plan_owner_(std::move(plan_owner)),
        plan_(plan_owner_ ? *plan_owner_ : plan),
        bindings_(std::move(bindings)),
        operations_(std::move(operations)),
        resources_(std::move(resources)),
        payload_capture_(ResourcePayloadScope::capture(resources_)),
        options_(options),
        atom_outcomes_(atom_outcomes),
        maximum_parallelism_(maximum_parallelism),
        subscription_(resources_, cancellation,
                      std::move(options_.result_publication)),
        cancellation_(cancellation),
        stop_(std::move(stop)),
        dispatch_(std::move(dispatch)),
        templates_(plan_.structured_templates()),
        shared_(snapshot.empty() ? nullptr : shared_results),
        checkpoints_(snapshot.empty() ? nullptr : checkpoints),
        blocks_(blocks),
        block_epoch_(blocks ? blocks->epoch() : 0),
        snapshot_(ResourceAllocator<char>(resources_)),
        snapshot_input_(snapshot.data(), snapshot.size(),
                        ResourceAllocator<char>(resources_)),
        remaining_(options.maximum_dependency_work),
        cache_remaining_(options.maximum_dependency_cache_work),
        actors_(ResourceAllocator<std::weak_ptr<Actor>>(resources_)),
        actor_aliases_(ResourceAllocator<std::shared_ptr<Actor>>(resources_)),
        actor_queries_(make_resource_map<ActorRegistration>(resources_)),
        validation_domains_(std::less<DomainKey>{},
                            ResourceAllocator<DomainEntry>(resources_)),
        pending_tasks_(ResourceAllocator<PendingTask>(resources_)),
        shared_payloads_(
            ResourceAllocator<std::shared_ptr<SharedResults::ProducerPayload>>(
                resources_)),
        remaining_consumers_(ResourceAllocator<std::uint64_t>(resources_)),
        named_pins_(ResourceAllocator<bool>(resources_)),
        finished_steps_(ResourceAllocator<bool>(resources_)),
        requested_roots_(ResourceAllocator<std::shared_ptr<Actor>>(resources_)),
        shareable_closure_(ResourceAllocator<bool>(resources_)),
        checkpoint_shareable_(ResourceAllocator<bool>(resources_)),
        result_cacheable_(ResourceAllocator<bool>(resources_)),
        checkpoint_scopes_(
            make_resource_map<std::shared_ptr<ResultCheckpointScope>>(
                resources_)) {}

  ~StructuredExecution() override { drain(); }
  /** @brief Waits for pending callback retirement before clearing actor state.
   * @note Used by registry shutdown and the coordinator destructor.
   */
  void drain() noexcept override {
    std::lock_guard<std::recursive_mutex> lock(driver_mutex_);
    detaching_ = false;
    ResourcePayloadScope payload_scope(resources_, payload_capture_);
    wait_pending();
    for (const auto& weak : actors_)
      if (auto actor = weak.lock())
        drain_actor(*actor);
    for (const auto& weak : actors_)
      if (auto actor = weak.lock();
          actor && !actor->complete && actor->terminal == ErrorCode::Ok)
        retire(*actor, Status{ErrorCode::Cancelled, {}});
  }
  /** @brief Advances parked producers on the caller's thread.
   * @details Ordinary peer pumps try-lock and return while the original driver
   * or another peer is active. Drain mode waits for the driver lock. Both modes
   * wait for submitted callbacks to retire before reading Actors or changing
   * their driver ids. A traversal can advance several eligible producers, at
   * most once per Actor, subject to the configured callback wave limit.
   * @return True when no retained producer work remains; false when work is
   * pending, active, contended, or still needs a later advance.
   */
  bool drive(bool drain_unneeded) override {
    std::unique_lock<std::recursive_mutex> lock(driver_mutex_, std::defer_lock);
    if (drain_unneeded)
      lock.lock();
    else if (!lock.try_lock())
      return false;
    if (driver_active_)
      return false;
    DriverScope driving(driver_active_);
    detaching_ = false;
    ResourceAllocationScope coordinator_scope(resources_);
    ResourcePayloadScope payload_scope(resources_, payload_capture_);
    if (running_pending()) {
      if (!drain_unneeded || pending_peers())
        return false;
      wait_pending();
    }
    const auto before = progress_;
    ++traversal_;
    // Callback retirement is the boundary for reading actor state or changing
    // its driver. Partially prepared consumers have no submitted continuation.
    for (auto& weak : actors_) {
      auto actor = weak.lock();
      if (actor && !actor->initialized && !actor->shared.valid()) {
        actor_queries_.erase(actor->key);
        for (auto index : actor->aliases)
          if (actor_aliases_[index] == actor)
            actor_aliases_[index].reset();
        weak.reset();
      } else if (actor) {
        actor->driver = std::this_thread::get_id();
      }
    }
    for (std::size_t position = actors_.size(); position > 0; --position) {
      auto current = actors_[position - 1].lock();
      if (!current || !current->shared.valid() || !current->shared.producer() ||
          current->complete || current->terminal != ErrorCode::Ok)
        continue;
      if (!(current->joint ? current->joint->peers()
                           : current->shared.continue_for_peers())) {
        drain_actor(*current);
        retire(*current, Status{ErrorCode::Cancelled, {}});
        continue;
      }
      try {
        auto status = current->initialized
                          ? advance(current->index, *current)
                          : initialize_actor(current->index, current);
        if (!status.ok())
          retire(*current, status);
      } catch (const std::bad_alloc&) {
        drain_actor(*current);
        retire(*current, Status{ErrorCode::ResourceExhausted, {}});
      } catch (...) {
        drain_actor(*current);
        retire(*current, Status{ErrorCode::OperationFailed, {}});
      }
    }
    if (progress_ == before && !pending_tasks_.empty()) {
      auto pumped = pump_pending(false);
      if (!pumped.ok()) {
        for (const auto& weak : actors_)
          if (auto actor = weak.lock();
              actor && !actor->complete && actor->terminal == ErrorCode::Ok)
            retire(*actor, pumped);
      }
    }
    return pending_tasks_.empty() &&
           std::all_of(actors_.begin(), actors_.end(), [](const auto& weak) {
             auto actor = weak.lock();
             return !actor || actor->complete ||
                    actor->terminal != ErrorCode::Ok;
           });
  }
  Result<ExecutionResult> run(const DemandQuery* requested) {
    std::unique_lock<std::recursive_mutex> driver_lock(driver_mutex_);
    DriverScope driving(driver_active_);
    const auto started = std::chrono::steady_clock::now();
    ResourceAllocationScope coordinator_scope(resources_);
    ResourcePayloadScope payload_scope(resources_, payload_capture_);
    Result<ExecutionResult> result(Status{ErrorCode::Internal, {}});
    try {
      auto admitted = plan_.resources().reference(resources_);
      if (!admitted.ok())
        return Result<ExecutionResult>(admitted.status());
      bindings_resources_ = admitted.take_value();
      for (const auto& binding : bindings_) {
        const auto* supplied = &binding.result.resources();
        auto reowned = supplied->reference(resources_);
        if (!reowned.ok())
          return Result<ExecutionResult>(reowned.status());
        auto joined = bindings_resources_.unite(reowned.value());
        if (!joined.ok())
          return Result<ExecutionResult>(joined.status());
        bindings_resources_ = joined.take_value();
      }
      result = run_body(requested);
    } catch (const std::bad_alloc&) {
      result =
          Result<ExecutionResult>(Status{ErrorCode::ResourceExhausted, {}});
    } catch (...) {
      result = Result<ExecutionResult>(Status{ErrorCode::OperationFailed, {}});
    }
    subscription_.close();
    call_.retire_user();
    handoff_allowed_ = false;
    if (detaching_) {
      try {
#if defined(PHOTOSPIDER_ENABLE_EXECUTION_TEST_HOOKS)
        execution_testing::notify_structured_handoff_ready();
#endif
        if (shared_->adopt(shared_from_this())) {
          if (pending_peers()) {
            return result;
          }
          shared_->withdraw(this);
        }
      } catch (...) {
        // Failed handoff admission preserves the synchronous drain boundary.
      }
      detaching_ = false;
    }
    wait_pending();
    if (first_pending_actor() && !first_pending_actor()->queued &&
        first_pending_actor()->joint && first_pending_actor()->joint->pending) {
      auto carrier = first_pending_actor();
      auto group = carrier->joint;
      try {
        finish_joint(group);
      } catch (...) {
        drain_actor(*carrier);
      }
    }
    // A producer may have returned a prefix to this Run while another frozen
    // waiter still needs the complete object. Drain that shared obligation
    // before retiring its original coordinator and borrowed bindings.
    for (std::size_t position = actors_.size(); position > 0; --position) {
      const auto i = position - 1;
      auto current = actors_[i].lock();
      if (!current || !current->shared.valid() || !current->shared.producer())
        continue;
      while (!current->complete && current->terminal == ErrorCode::Ok) {
        if (!(current->joint ? current->joint->peers()
                             : current->shared.continue_for_peers())) {
          drain_actor(*current);
          retire(*current, Status{ErrorCode::Cancelled, {}});
          break;
        }
        try {
          const auto before = progress_;
          ++traversal_;
          auto status = current->initialized
                            ? advance(current->index, *current)
                            : initialize_actor(current->index, current);
          if (!status.ok())
            break;
          if (progress_ == before) {
            status = pump_pending(true);
            if (!status.ok())
              break;
          }
        } catch (const std::bad_alloc&) {
          drain_actor(*current);
          retire(*current, Status{ErrorCode::ResourceExhausted, {}});
        } catch (...) {
          drain_actor(*current);
          retire(*current, Status{ErrorCode::OperationFailed, {}});
        }
      }
    }
    if (shared_ && !callback_)
      shared_->pump(true);
    const auto observer_failure = subscription_.status();
    if (!observer_failure.ok())
      return Result<ExecutionResult>(observer_failure);
    if (result.ok()) {
      diagnostics_.execute_us = static_cast<std::uint64_t>(
          std::chrono::duration_cast<std::chrono::microseconds>(
              std::chrono::steady_clock::now() - started)
              .count());
      diagnostics_.managed_resources = resources_.statistics();
      for (const auto& payload : shared_payloads_)
        diagnostics_.shared_peak_live_bytes =
            std::max(diagnostics_.shared_peak_live_bytes, payload->peak());
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
    return result;
  }
  Result<ExecutionResult> run_body(const DemandQuery* requested) {
    using Answer = Result<ExecutionResult>;
    if (!options_.maximum_result_window_bytes ||
        options_.maximum_result_window_bytes > INT64_MAX)
      return Answer(protocol("invalid structured I/O window"));
    auto traversal = consume(plan_.steps().size() + 1);
    if (!traversal.ok())
      return Answer(traversal);

    rollback_possible_ = std::any_of(
        plan_.steps().begin(), plan_.steps().end(), [](const auto& step) {
          return (step.backend == Backend::Gpu && step.traits.supports_cpu &&
                  step.traits.allows_cpu_fallback &&
                  step.traits.deterministic && step.traits.side_effect_free) ||
                 result_joint_candidate(step);
        });
    auto live = initialize_liveness(requested);
    if (!live.ok())
      return Answer(live);
    actor_aliases_.resize(plan_.steps().size());
    shareable_closure_.resize(plan_.steps().size());
    for (std::size_t i = 0; i < plan_.steps().size(); ++i) {
      const auto& step = plan_.steps()[i];
      const auto& included = step.traits.outputs[0].input_indices;
      auto purity_work = consume(
          1 + step.inputs.size() * (1 + (included ? included->size() : 0)));
      if (!purity_work.ok())
        return Answer(purity_work);
      shareable_closure_[i] =
          step.traits.deterministic && step.traits.side_effect_free;
      for (std::uint32_t port = 0; port < step.inputs.size(); ++port) {
        if (included && std::find(included->begin(), included->end(), port) ==
                            included->end())
          continue;
        if (const auto* producer =
                std::get_if<PlanStepInput>(&step.inputs[port]))
          shareable_closure_[i] =
              shareable_closure_[i] && shareable_closure_[producer->step_index];
      }
    }
    checkpoint_shareable_.resize(plan_.steps().size());
    result_cacheable_.resize(plan_.steps().size());
    for (std::size_t i = 0; i < plan_.steps().size(); ++i) {
      const auto& step = plan_.steps()[i];
      const auto& included = step.traits.outputs[0].input_indices;
      auto purity_work = checkpoint_cache_work(
          1 + step.inputs.size() * (1 + (included ? included->size() : 0)));
      if (!purity_work.ok()) {
        if (purity_work.code != ErrorCode::ResourceExhausted)
          return Answer(purity_work);
        std::fill(checkpoint_shareable_.begin(), checkpoint_shareable_.end(),
                  false);
        break;
      }
      checkpoint_shareable_[i] =
          step.traits.deterministic && step.traits.side_effect_free;
      result_cacheable_[i] = checkpoint_shareable_[i] && step.traits.cacheable;
      if (step.output_result_schema &&
          step.output_result_schema->id == "photospider.path_set")
        result_cacheable_[i] = false;
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
    subscription_.initialize(plan_.steps().size());
    diagnostics_.operation_timings = decltype(diagnostics_.operation_timings)(
        ResourceAllocator<OperationTiming>(resources_));
    diagnostics_.operation_timings.reserve(plan_.steps().size());
    if (templates_.size() != plan_.steps().size() ||
        std::any_of(templates_.begin(), templates_.end(),
                    [](const auto& key) { return key.empty(); }))
      return Answer(Status{ErrorCode::ResourceExhausted,
                           "structured identity work exhausted"});
    auto snapshot_work = consume(snapshot_input_.size() + 48);
    if (!snapshot_work.ok())
      return Answer(snapshot_work);
    if (!snapshot_input_.empty())
      snapshot_.assign(snapshot_input_.data(), snapshot_input_.size());
    if (snapshot_.empty()) {
      static std::atomic<std::uint64_t> sequence{1};
      auto id = sequence.load();
      do {
        if (id == UINT64_MAX)
          return Answer(Status{ErrorCode::ResourceExhausted, {}});
      } while (!sequence.compare_exchange_weak(id, id + 1));
      char digits[24];
      auto converted = std::to_chars(digits, digits + sizeof(digits), id);
      snapshot_ = "structured-run-";
      snapshot_.append(digits,
                       static_cast<std::size_t>(converted.ptr - digits));
    }
    records_ = std::make_unique<DependencyRecords>(
        plan_, std::string(snapshot_), set_limits());
    if (!records_->status().ok())
      return Answer(records_->status());
    for (std::size_t i = 0; i < bindings_.size(); ++i)
      if (bindings_[i].result.valid()) {
        auto status =
            records_->bind_result(PlanWorkflowInput{i}, bindings_[i].result);
        if (!status.ok())
          return Answer(status);
      }
    if (shared_) {
      ResourceVector<bool> reachable(plan_.steps().size(), false,
                                     ResourceAllocator<bool>(resources_));
      for (const auto& named : plan_.outputs())
        if (!requested || requested->count(named.first))
          reachable[named.second] = true;
      for (std::size_t i = 0; i < plan_.steps().size(); ++i)
        if (wants_effects(requested) &&
            !plan_.steps()[i].traits.side_effect_free)
          reachable[i] = true;
      for (std::size_t position = reachable.size(); position > 0; --position) {
        const auto i = position - 1;
        if (!reachable[i])
          continue;
        const auto& step = plan_.steps()[i];
        for (std::uint32_t port = 0; port < step.inputs.size(); ++port) {
          const auto& selected = step.traits.outputs[0].input_indices;
          if (selected && std::find(selected->begin(), selected->end(), port) ==
                              selected->end())
            continue;
          if (const auto* input =
                  std::get_if<PlanStepInput>(&step.inputs[port]))
            reachable[input->step_index] = true;
        }
      }
      ResourceVector<ResourceString> keys{
          ResourceAllocator<ResourceString>(resources_)};
      for (std::size_t i = 0; i < reachable.size(); ++i) {
        if (!reachable[i] || !shareable_closure_[i] ||
            !plan_.steps()[i].output_result_schema)
          continue;
        auto charged = consume(templates_[i].size() + snapshot_.size() + 1);
        if (!charged.ok())
          return Answer(charged);
        ResourceString key{ResourceAllocator<char>(resources_)};
        key.reserve(templates_[i].size() + snapshot_.size() + 1);
        key.append(templates_[i].data(), templates_[i].size());
        key.push_back(':');
        key.append(snapshot_);
        append_actor_scope(key, i, {}, 0);
        keys.push_back(std::move(key));
      }
      std::sort(keys.begin(), keys.end());
      keys.erase(std::unique(keys.begin(), keys.end()), keys.end());
      auto joined = shared_->join_call(snapshot_, resources_, cancellation_,
                                       std::move(keys));
      if (!joined.ok())
        return Answer(joined.status());
      call_ = joined.take_value();
    }
    if (requested) {
      for (const auto& item : *requested) {
        const auto found = plan_.outputs().find(item.first);
        if (found == plan_.outputs().end() || !item.second.valid())
          return Answer(protocol("invalid structured named query"));
        const auto& output = plan_.steps()[found->second];
        const auto shape =
            output.output_result_schema &&
                    !output.output_result_schema->tensors.empty()
                ? output.output_result_schema->tensors[0].sample_shape()
            : atom_outcomes_ && output.output_result_schema
                ? std::vector<std::uint64_t>{1}
                : output.output_descriptor.shape;
        if (item.second.shape() != shape)
          return Answer(protocol("structured query domain mismatch"));
        auto allowed = atom_outcomes_ && output.output_result_schema &&
                               output.output_result_schema->tensors.empty()
                           ? Footprint::all({1}, set_limits())
                           : Footprint::from_regions(
                                 shape, {plan_.output_regions().at(item.first)},
                                 set_limits());
        if (!allowed.ok())
          return Answer(allowed.status());
        auto outside = item.second.subtract(allowed.value(), set_limits());
        if (!outside.ok())
          return Answer(outside.status());
        if (!outside.value().empty())
          return Answer(protocol("structured query exceeds plan"));
      }
    }
    diagnostics_.plan_digest =
        ResourceString(plan_.digest().value.data(), plan_.digest().value.size(),
                       ResourceAllocator<char>(resources_));
    if (atom_outcomes_)
      return run_atoms(requested);
    auto effects = run_effect_roots(requested);
    if (!effects.ok())
      return Answer(effects);
    ResourceVector<bool> registered(plan_.steps().size(), false,
                                    ResourceAllocator<bool>(resources_));
    for (const auto& named : plan_.outputs()) {
      const auto index = named.second;
      const auto& step = plan_.steps()[index];
      if ((requested && !requested->count(named.first)) ||
          (registered[index] && step.traits.joint_contract != 2) ||
          !step.output_result_schema)
        continue;
      registered[index] = true;
      std::optional<Footprint> demand;
      if (!step.output_result_schema->tensors.empty()) {
        auto wanted =
            requested
                ? Result<Footprint>(requested->at(named.first))
                : Footprint::from_regions(
                      step.output_result_schema->tensors[0].sample_shape(),
                      {plan_.output_regions().at(named.first)}, set_limits());
        if (!wanted.ok())
          return Answer(wanted.status());
        auto closed = step.output_result_schema->tensors[0].close_samples(
            wanted.value(), set_limits());
        if (!closed.ok())
          return Answer(closed.status());
        demand = closed.take_value();
      }
      auto created = actor(index, std::move(demand), 0, true);
      if (!created.ok())
        return Answer(created.status());
      requested_roots_.push_back(created.take_value());
    }
    ExecutionResult result;
    result.results = make_resource_map<ResultRef>(resources_);
    for (const auto& named : plan_.outputs()) {
      if (requested && !requested->count(named.first))
        continue;
      auto status = consume(1);
      if (!status.ok())
        return Answer(status);
      const auto& step = plan_.steps()[named.second];
      if (step.output_result_schema) {
        std::optional<Footprint> demand;
        if (!step.output_result_schema->tensors.empty()) {
          auto wanted =
              requested
                  ? Result<Footprint>(requested->at(named.first))
                  : Footprint::from_regions(
                        step.output_result_schema->tensors[0].sample_shape(),
                        {plan_.output_regions().at(named.first)}, set_limits());
          if (!wanted.ok())
            return Answer(wanted.status());
          auto closed = step.output_result_schema->tensors[0].close_samples(
              wanted.value(), set_limits());
          if (!closed.ok())
            return Answer(closed.status());
          demand = closed.take_value();
        }
        auto object = result_object(named.second, ResultObjectNeed{}, demand);
        if (!object.ok())
          return Answer(object.status());
        auto object_coverage =
            demand ? *demand : Footprint::all({1}, set_limits()).take_value();
        auto evidence = record_object(named.second, object.value());
        if (!evidence.ok())
          return Answer(evidence);
        const auto ancestry = object.value().dependencies();
        if (!ancestry || ancestry->roots.empty())
          return Answer(protocol("Result has no publication ancestry"));
        evidence = records_->output(named.first, named.second, object_coverage,
                                    ancestry->roots[0]->scope);
        if (!evidence.ok())
          return Answer(evidence);
        result.results.emplace(named.first, object.take_value());
        continue;
      }
      return Answer(protocol("operation output requires Result schema"));
    }
    auto status = consume(0);
    if (!status.ok())
      return Answer(status);
    const bool finished =
        !first_pending_actor() &&
        std::all_of(actors_.begin(), actors_.end(), [](const auto& weak) {
          auto actor = weak.lock();
          return !actor || actor->complete || actor->terminal != ErrorCode::Ok;
        });
    auto snapshot =
        finished ? Result<ExecutionDependencies>(std::move(*records_).finish())
                 : records_->snapshot();
    if (!snapshot.ok())
      return Answer(snapshot.status());
    bool hash_outputs = !subscription_.has_callback() &&
                        options_.maximum_result_digest_samples != 0;
    auto digest_samples = options_.maximum_result_digest_samples;
    auto digest_work = remaining_.load(std::memory_order_relaxed);
    if (hash_outputs) {
      for (const auto& named : result.results) {
        auto facts = named.second.descriptor();
        if (!facts.ok())
          return Answer(facts.status());
        const auto& schema = named.second.schema();
        for (std::uint32_t slot = 0; slot < schema.fields.size(); ++slot) {
          auto width = schema.row_bytes(slot);
          if (!width.ok())
            return Answer(width.status());
          const auto rows = facts.value().rows(slot);
          const auto elements =
              width.value() /
              Value::element_size(schema.fields[slot].element_type);
          if (width.value() > options_.maximum_result_window_bytes ||
              (elements && rows > digest_samples / elements) ||
              rows > digest_work) {
            hash_outputs = false;
            break;
          }
          digest_samples -= rows * elements;
          digest_work -= rows;
        }
        if (!hash_outputs)
          break;
        for (std::uint32_t slot = 0; slot < schema.tensors.size(); ++slot) {
          auto count = facts.value().tensor_coverage(slot).element_count();
          if (!count.ok() || count.value() > digest_samples ||
              count.value() > digest_work) {
            hash_outputs = false;
            break;
          }
          digest_samples -= count.value();
          digest_work -= count.value();
        }
        if (!hash_outputs)
          break;
      }
    }
    if (hash_outputs) {
      content_internal::Sha256 digest;
      digest.text("photospider.result-content.v1");
      for (const auto& named : result.results) {
        digest.text(named.first);
        const auto& object = named.second;
        auto schema = object.schema().managed_canonical(resources_);
        if (!schema.ok())
          return Answer(schema.status());
        digest.text(schema.value());
        auto facts = object.descriptor();
        if (!facts.ok())
          return Answer(facts.status());
        for (std::uint32_t slot = 0; slot < object.schema().fields.size();
             ++slot) {
          auto width = object.schema().row_bytes(slot);
          if (!width.ok())
            return Answer(width.status());
          if (width.value() > options_.maximum_result_window_bytes)
            return Answer(Status{ErrorCode::ResourceExhausted,
                                 "digest field exceeds read window"});
          const auto rows = facts.value().rows(slot);
          digest.integer(rows);
          for (std::uint64_t row = 0; row < rows; ++row) {
            auto active = consume(1);
            if (!active.ok())
              return Answer(active);
            auto plan = object.prepare_read(facts.value(), slot, row, 1);
            if (!plan.ok())
              return Answer(plan.status());
            auto bytes = plan.value().load(width.value(), active_token());
            if (!bytes.ok())
              return Answer(bytes.status());
            digest.bytes(bytes.value()->bytes().data(), width.value());
          }
        }
        for (std::uint32_t slot = 0; slot < object.schema().tensors.size();
             ++slot) {
          const auto& coverage = facts.value().tensor_coverage(slot);
          digest.integer(coverage.boxes().size());
          for (const auto& box : coverage.boxes())
            for (const auto& axis : box.dimensions()) {
              digest.integer(axis.offset);
              digest.integer(axis.extent);
            }
          const auto width = Value::element_size(
              object.schema().tensors[slot].descriptor.element_type);
          for (const auto& box : coverage.boxes()) {
            auto window =
                object.acquire_tensor(facts.value(), slot, box, active_token());
            if (!window.ok())
              return Answer(window.status());
            auto part =
                Footprint::from_regions(coverage.shape(), {box}, set_limits());
            if (!part.ok())
              return Answer(part.status());
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
              return Answer(scanned);
          }
        }
      }
      auto active = consume(0);
      if (!active.ok())
        return Answer(active);
      diagnostics_.result_digest =
          ResourceString(digest.finish(), ResourceAllocator<char>(resources_));
    }
    auto final_status = consume(0);
    if (!final_status.ok())
      return Answer(final_status);
    result.dependencies = snapshot.take_value();
    return Answer(std::move(result));
  }

  Result<ExecutionResult> run_atoms(const DemandQuery* requested) {
    using Answer = Result<ExecutionResult>;
    if (!requested)
      return Answer(
          protocol("Result atom collection requires explicit demand"));
    struct Observation {
      std::string_view name;
      std::size_t step;
      AtomKey key;
      std::optional<Footprint> samples;
      std::shared_ptr<Actor> actor;
      core_internal::StoredFailure failure;
    };
    ResourceVector<Observation> observations{
        ResourceAllocator<Observation>(resources_)};
    const auto maximum =
        std::min<std::uint64_t>(65536, options_.maximum_atom_observations);
    // Validate every named output and the full observation count before any
    // actor preparation, source work, start or poll callback.
    for (const auto& named : *requested) {
      const auto index = plan_.outputs().at(named.first);
      const auto& step = plan_.steps()[index];
      const auto& traits = step.traits.outputs[0];
      if (!step.output_result_schema || step.backend != Backend::Cpu ||
          traits.observation_kind != ObservationKind::Atomic ||
          traits.failure_delivery != FailureDelivery::PerAtomOutcome ||
          step.traits.joint_contract != 2)
        return Answer(Status{
            ErrorCode::InvalidArgument,
            "requested output does not declare CPU Result atom outcomes"});
      const auto& schema = *step.output_result_schema;
      const ResultTensorSpec* tensor =
          schema.tensors.empty() ? nullptr : &schema.tensors[0];
      auto closed = tensor ? tensor->close_samples(named.second, set_limits())
                           : Result<Footprint>(named.second);
      if (!closed.ok())
        return Answer(closed.status());
      std::vector<std::uint64_t> domain;
      std::vector<bool> grouped;
      if (tensor) {
        const auto shape = tensor->sample_shape();
        const auto tuple = input_internal::tuple_channel_axis(
            tensor->descriptor, tensor->facets);
        for (std::size_t axis = 0; axis < shape.size(); ++axis) {
          const bool is_grouped =
              axis >= shape.size() - tensor->atomic_trailing_axes ||
              (tuple && axis == *tuple + tensor->batch_axes.size());
          grouped.push_back(is_grouped);
          if (!is_grouped)
            domain.push_back(shape[axis]);
        }
      }
      if (domain.empty())
        domain.push_back(1);
      std::vector<Region> boxes;
      for (const auto& box : closed.value().boxes()) {
        std::vector<RegionDimension> dimensions;
        if (tensor) {
          for (std::size_t axis = 0; axis < grouped.size(); ++axis)
            if (!grouped[axis])
              dimensions.push_back(box.dimensions()[axis]);
        }
        if (dimensions.empty())
          dimensions.push_back({0, 1});
        boxes.emplace_back(std::move(dimensions));
      }
      auto atoms = Footprint::from_regions(domain, boxes, set_limits());
      if (!atoms.ok())
        return Answer(atoms.status());
      auto count = atoms.value().element_count();
      if (!count.ok() || count.value() > maximum - observations.size())
        return Answer(Status{ErrorCode::ResourceExhausted,
                             "atom observation count limit"});
      if (count.value() && (!options_.dependencies.maximum_stages ||
                            !traits.maximum_dependency_stages))
        return Answer(
            Status{ErrorCode::ResourceExhausted, "Result atom stage limit"});
      auto visited = atoms.value().visit(
          [&](const auto& coordinate) {
            auto charged = consume(1);
            if (!charged.ok())
              return charged;
            std::optional<Footprint> samples;
            if (tensor) {
              const auto shape = tensor->sample_shape();
              std::vector<RegionDimension> dimensions;
              std::size_t at = 0;
              for (std::size_t axis = 0; axis < shape.size(); ++axis)
                dimensions.push_back(
                    grouped[axis] ? RegionDimension{0, shape[axis]}
                                  : RegionDimension{coordinate[at++], 1});
              auto made = Footprint::from_regions(shape, {Region(dimensions)},
                                                  set_limits());
              if (!made.ok())
                return made.status();
              samples = made.take_value();
            }
            ResultProgramQuery query(*step.structured_metadata,
                                     step.parameters);
            query.output_index = step.output_index;
            query.tensor_outputs = samples;
            auto key = result_atom_key(query);
            if (!key.ok())
              return key.status();
            observations.push_back({named.first,
                                    index,
                                    key.take_value(),
                                    std::move(samples),
                                    {},
                                    {}});
            return Status::success();
          },
          maximum, active_token());
      if (!visited.ok())
        return Answer(visited);
    }
    auto effects = run_effect_roots(requested);
    if (!effects.ok())
      return Answer(effects);
    ExecutionResult result;
    result.atoms = ResourceVector<AtomObservation>{
        ResourceAllocator<AtomObservation>(resources_)};
    result.atoms.reserve(observations.size());
    result.results = make_resource_map<ResultRef>(resources_);
    const auto enclosing = [&](const Status& status) {
      return status.detail.origin == FailureOrigin::Protocol ||
             status.detail.scope == FailureScope::Run ||
             status.detail.scope == FailureScope::Waiter ||
             stop_() != ErrorCode::Ok;
    };
    for (auto& item : observations) {
      auto created = actor(item.step, item.samples, 0, true);
      if (!created.ok()) {
        if (enclosing(created.status()))
          return Answer(created.status());
        item.failure.record(created.status());
      } else {
        item.actor = created.take_value();
      }
    }
    for (auto& item : observations) {
      Result<ResultRef> outcome(Status{ErrorCode::Internal, {}});
      std::optional<QualityReport> quality;
      if (!item.failure.ok()) {
        outcome = Result<ResultRef>(item.failure.status());
      } else {
        for (;;) {
          const auto before = progress_;
          ++traversal_;
          auto ready = result_object_step(item.step, item.actor, {});
          if (!ready.ok()) {
            outcome = Result<ResultRef>(ready.status());
            break;
          }
          if (ready.value()) {
            outcome = Result<ResultRef>(std::move(*ready.value()));
            break;
          }
          if (progress_ == before) {
            auto pumped = pump_pending(true);
            if (!pumped.ok()) {
              outcome = Result<ResultRef>(pumped);
              break;
            }
          }
        }
        quality = item.actor->quality;
      }
      if (!outcome.ok()) {
        if (enclosing(outcome.status()))
          return Answer(outcome.status());
        if (!quality) {
          for (const auto& entry : actor_queries_) {
            const auto& failed = entry.second.failed;
            if (!failed || failed->failure.ok() || !failed->quality)
              continue;
            const auto& detail = outcome.status().detail;
            const auto cause = failed->failure.fixed_status();
            const bool same_atom = detail.atom && cause.detail.atom &&
                                   *detail.atom == *cause.detail.atom;
            const bool same_domain =
                detail.domain && cause.detail.domain &&
                detail.domain->first == cause.detail.domain->first &&
                detail.domain->extent == cause.detail.domain->extent;
            if (detail.node_id == cause.detail.node_id &&
                (same_atom || same_domain)) {
              quality = failed->quality;
              break;
            }
          }
        }
      } else {
        auto evidence =
            record_object(item.step, outcome.value(), item.actor.get());
        if (!evidence.ok())
          return Answer(evidence);
        const auto bundle = outcome.value().dependencies();
        if (!bundle || bundle->roots.empty())
          return Answer(protocol("Result atom has no publication ancestry"));
        auto samples = item.samples;
        if (!samples) {
          auto singleton = Footprint::all({1}, set_limits());
          if (!singleton.ok())
            return Answer(singleton.status());
          samples = singleton.take_value();
        }
        evidence = records_->output(std::string(item.name), item.step, *samples,
                                    bundle->roots[0]->scope);
        if (!evidence.ok())
          return Answer(evidence);
      }
      result.atoms.push_back(
          {ResourceString(item.name.data(), item.name.size(),
                          ResourceAllocator<char>(resources_)),
           plan_.steps()[item.step].result_ref(), item.key, std::move(outcome),
           std::move(quality)});
      ++diagnostics_.tile_count;
      item.actor.reset();
    }
    const auto active = consume(0);
    if (!active.ok())
      return Answer(active);
    const bool finished =
        !first_pending_actor() &&
        std::all_of(actors_.begin(), actors_.end(), [](const auto& weak) {
          auto actor = weak.lock();
          return !actor || actor->complete || actor->terminal != ErrorCode::Ok;
        });
    auto snapshot =
        finished ? Result<ExecutionDependencies>(std::move(*records_).finish())
                 : records_->snapshot();
    if (!snapshot.ok())
      return Answer(snapshot.status());
    result.dependencies = snapshot.take_value();
    return Answer(std::move(result));
  }

 private:
  struct StartPhase {
    std::chrono::steady_clock::time_point started =
        std::chrono::steady_clock::now();
    Status dispatched;
    Result<ResultContinuation> result{Status{ErrorCode::Internal, {}}};
    ErrorCode sticky = ErrorCode::Ok;
  };
  struct PollPhase {
    std::chrono::steady_clock::time_point started =
        std::chrono::steady_clock::now();
    NumericDiagnostics numeric;
    std::uint64_t native_dispatches = 0;
    bool invoked = false;
    Result<ResultProgramPoll> result{Status{ErrorCode::Internal, {}}};
    ErrorCode sticky = ErrorCode::Ok;
  };
  struct Actor;
  struct JointActor;
  /** @brief Progress through one retained input in a Need request.
   * @details Each input has an independent cursor. `producer` keeps its
   * requested upstream Actor alive while the consumer waits and remains set
   * until the requested object is available. The Need phase rotates among
   * inputs that can make progress; a traversal visit prevents a deep Actor
   * chain from being recursively revisited within that traversal.
   */
  struct NeedCursor {
    std::size_t next = 0, total = 0;
    bool initialized = false;
    std::optional<Footprint> tensor_samples;
    std::optional<Footprint> tensor_supply_samples;
    bool tensor_payload = false;
    std::shared_ptr<Actor> producer;
    bool tensor_atoms_initialized = false;
    std::size_t tensor_atom_next = 0;
    ResourceVector<Footprint> tensor_atoms;
    ResourceVector<std::shared_ptr<Actor>> tensor_producers;
    ResourceVector<ResultTensorInput::Piece> tensor_pieces;
    ResourceVector<std::shared_ptr<NeedCursor>> requests;
    std::size_t supplied = 0, turn = 0;
    bool complete() const { return initialized && next == total; }
  };
  /** @brief Poll response retained until all requested inputs are supplied.
   * @details The coordinator validates the full envelope before progressing
   * independent input cursors. A supply exception terminates the producer's
   * Need phase. A completed I/O request is not replayed if retaining its reply
   * fails.
   */
  struct NeedPhase {
    explicit NeedPhase(ResultProgramNeed value) : request(std::move(value)) {}
    ResultProgramNeed request;
    NeedCursor cursor;
  };
  /** @brief One operation instance and its resumable execution state.
   * @details The Root-owned pending-task list retains this Actor while its
   * callback is queued or submitted. `waiting` retains an upstream Actor for
   * an incomplete dependency; `driving` guards active advancement, including
   * same-thread recursive dependency servicing. Aliases share the Actor's
   * publication and release accounting.
   */
  struct Actor {
    explicit Actor(const PlanStep& step, const ResourceBudget& budget)
        : query(*step.structured_metadata, step.parameters),
          key(ResourceAllocator<char>(budget)),
          dependency_scope(ResourceAllocator<char>(budget)),
          aliases(ResourceAllocator<std::size_t>(budget)),
          tensors(std::less<std::pair<std::uint32_t, std::uint32_t>>{},
                  ResourceAllocator<ResultTensorInputs::value_type>(budget)),
          input_facts(budget),
          history(std::less<ResultNeedKey>{},
                  ResourceAllocator<ResultNeedHistory::value_type>(budget)),
          results(std::less<std::uint32_t>{},
                  ResourceAllocator<ResultObjectInputs::value_type>(budget)),
          io(ResourceAllocator<ResultIoReply>(budget)),
          cache_replay(ResourceAllocator<StructuredCacheNeed>(budget)),
          input_bundles(ResourceAllocator<InputBundle>(budget)),
          failure(std::allocate_shared<std::atomic<ErrorCode>>(
              ResourceAllocator<std::atomic<ErrorCode>>(budget),
              ErrorCode::Ok)),
          service_failure(std::allocate_shared<plugin_internal::FailureLatch>(
              ResourceAllocator<plugin_internal::FailureLatch>(budget))),
          node_id(step.node_id) {}
    ResourceLease lease;
    ResultProgramQuery query;
    AtomKey observation;
    ResourceString key, dependency_scope;
    ResourceVector<std::size_t> aliases;
    std::weak_ptr<Actor> self;
    ResultContinuation continuation;
    std::shared_ptr<JointActor> joint;
    std::size_t index = 0, joint_slot = 0, registry_slot = 0;
    bool deferred_start = false, joint_disabled = false, scalar_restart = false;
    std::atomic<std::uint64_t> discovery_work_remaining{0};
    std::variant<std::monostate, StartPhase, PollPhase, NeedPhase> phase;
    std::optional<StructuredSubmission> pending;
    std::shared_ptr<Actor> waiting;
    std::thread::id driver;
    ResultTensorInputs tensors;
    using InputFactsMap = ResultInputFacts;
    InputFactsMap input_facts;
    ResultNeedHistory history;
    ResultObjectInputs results;
    ResourceVector<ResultIoReply> io;
    ResultRef published;
    std::optional<QualityReport> quality;
    ResourceVector<StructuredCacheNeed> cache_replay;
    struct InputBundle {
      InputBundle(std::size_t producer,
                  std::shared_ptr<const DependencyBundle> bundle,
                  std::uint32_t input_port = UINT32_MAX,
                  std::uint64_t object = 0, std::uint64_t revision = 0)
          : first(producer),
            second(std::move(bundle)),
            port(input_port),
            object_id(object),
            revision(revision) {}
      std::size_t first;
      std::shared_ptr<const DependencyBundle> second;
      std::uint32_t port;
      std::uint64_t object_id, revision;
    };
    ResourceVector<InputBundle> input_bundles;
    std::unique_ptr<DependencyRecords> attempt_records;
    bool cache_disabled = false, fallback_taint = false;
    SharedResults::Lease shared;
    ResultRelation input_obligations;
    std::shared_ptr<std::atomic<ErrorCode>> failure;
    std::shared_ptr<plugin_internal::FailureLatch> service_failure;
    ErrorCode terminal = ErrorCode::Ok;
    std::uint64_t node_id = 0;
    std::uint32_t polls = 0;
    bool retry_safe = true, native_block_reuse = false;
    std::uint64_t published_revision = 0, native_dispatches = 0;
    bool complete = false, busy = false, driving = false, initialized = false;
    bool queued = false;
    std::uint64_t visit = 0;
  };
  struct PendingTask {
    std::shared_ptr<Actor> actor;
    Backend backend;
    bool whole, tiles, frozen_producer;
    CancellationToken token;
    std::function<Status(const StructuredServices&)> task;
    std::shared_ptr<ExecutionDiagnostics> observations;
  };
  struct FailedObservation {
    core_internal::StoredFailure failure;
    std::optional<QualityReport> quality;
  };
  struct ActorRegistration {
    std::weak_ptr<Actor> observed;
    std::shared_ptr<Actor> pending;
    std::shared_ptr<FailedObservation> failed;
  };
  struct JointActor {
    struct Member {
      std::size_t index = 0;
      std::weak_ptr<Actor> actor;
      SharedResults::Lease shared;
      CancellationToken token;
      std::shared_ptr<plugin_internal::FailureLatch> failure;
      std::shared_ptr<std::atomic<bool>> done;
    };
    explicit JointActor(const ResourceBudget& root)
        : payload(std::make_shared<PayloadObservation>()),
          members(ResourceAllocator<Member>(root)),
          pending_members(ResourceAllocator<std::weak_ptr<Actor>>(root)),
          failure(std::allocate_shared<plugin_internal::FailureLatch>(
              ResourceAllocator<plugin_internal::FailureLatch>(root))) {}
    std::shared_ptr<PayloadObservation> payload;
    ResourceVector<Member> members;
    ResourceVector<std::weak_ptr<Actor>> pending_members;
    std::shared_ptr<plugin_internal::FailureLatch> failure;
    mutable CancellationSource cancellation;
    std::unique_ptr<DependencyRecords> saved_records;
    ResultJointContinuation continuation;
    Result<ResultJointContinuation> started{Status{ErrorCode::Internal, {}}};
    Result<ResourceVector<ResultJointOutcome>> polled{
        Status{ErrorCode::Internal, {}}};
    bool pending = false, starting = false, entered = false;
    std::uint64_t visit = 0;
    bool contract2 = false;
    std::atomic<bool> driving{false};
    void refresh() const {
      bool active = false;
      for (const auto& member : members) {
        if (member.done->load())
          continue;
        member.shared.refresh();
        active |= !member.token.cancelled();
      }
      if (!active)
        cancellation.cancel();
    }
    bool peers() const {
      for (const auto& member : members)
        if (!member.done->load() && !member.token.cancelled() &&
            member.shared.continue_for_peers())
          return true;
      return false;
    }
    bool complete() const {
      return std::all_of(
          members.begin(), members.end(),
          [](const auto& member) { return member.done->load(); });
    }
  };
  /** @brief Per-thread scope for one callback's structured execution state.
   * @details Installs services, effective producer state, replay mode, service
   * depth, and the callback's Actor. Lookup selects the matching execution
   * owner; nested inline work restores its predecessor. Actor identity and the
   * driver thread id distinguish same-driver re-entry from a competing advance
   * and identify same-actor recursive dependency checks.
   * @note CPU range and tile work uses the phase's explicitly captured producer
   * pointer and token, not this thread-local stack. The lease remains valid
   * through callback and barrier retirement.
   */
  struct CallbackContext {
    StructuredExecution* owner;
    Actor* actor;
    const StructuredServices* services;
    SharedResults::Lease retained;
    const SharedResults::Lease* active;
    bool optional_replay;
    unsigned depth;
    CallbackContext* previous;
    std::shared_ptr<JointActor> joint;
    CallbackContext(StructuredExecution* execution, Actor* current,
                    const StructuredServices* supplied,
                    SharedResults::Lease shared, bool optional,
                    unsigned service_depth,
                    std::shared_ptr<JointActor> shared_joint = {})
        : owner(execution),
          actor(current),
          services(supplied),
          retained(std::move(shared)),
          active(retained.valid() ? &retained : nullptr),
          optional_replay(optional),
          depth(service_depth),
          previous(callback_),
          joint(std::move(shared_joint)) {
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
  std::shared_ptr<JointActor>& active_joint() {
    auto* context = callback_context();
    return context ? context->joint : active_joint_;
  }
  std::shared_ptr<JointActor> active_joint() const {
    auto* context = callback_context();
    return context ? context->joint : active_joint_;
  }
  struct JointScope {
    std::shared_ptr<JointActor>& target;
    std::shared_ptr<JointActor> previous;
    const SharedResults::Lease*& producer;
    const SharedResults::Lease* prior_producer;
    ResourcePayloadScope payload_scope;
    JointScope(StructuredExecution& owner, std::shared_ptr<JointActor> group)
        : target(owner.active_joint()),
          previous(target),
          producer(owner.active_shared()),
          prior_producer(producer),
          payload_scope(owner.resources_,
                        owner.producer_capture(group->payload)) {
      target = std::move(group);
      producer = nullptr;
    }
    ~JointScope() {
      target = std::move(previous);
      producer = prior_producer;
    }
  };
  bool task_peers(const Actor& actor) const {
    return actor.joint && actor.joint->pending
               ? actor.joint->peers()
               : actor.shared.continue_for_peers();
  }
  void refresh_task(const Actor& actor) const {
    if (actor.joint && actor.joint->pending)
      actor.joint->refresh();
    else
      actor.shared.refresh();
  }
  const StructuredServices* active_services() const {
    auto* context = callback_context();
    return context ? context->services : nullptr;
  }
  const SharedResults::Lease* active_shared() const {
    auto* context = callback_context();
    return context ? context->active : active_shared_;
  }
  const SharedResults::Lease*& active_shared() {
    auto* context = callback_context();
    return context ? context->active : active_shared_;
  }
  bool& work_mode() {
    auto* context = callback_context();
    return context ? context->optional_replay : optional_replay_;
  }
  unsigned& service_depth() {
    auto* context = callback_context();
    return context ? context->depth : service_depth_;
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
  bool pending_peers() const {
    return std::any_of(
        pending_tasks_.begin(), pending_tasks_.end(),
        [&](const auto& task) { return task_peers(*task.actor); });
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
        refresh_task(*task.actor);
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
      if (auto group = task.actor->joint) {
        failure = group->failure->snapshot();
        if (failure.detail.origin == FailureOrigin::Protocol)
          return bind_protocol(failure, task.actor->node_id);
        for (const auto& member : group->members) {
          failure = member.failure->snapshot();
          if (failure.detail.origin == FailureOrigin::Protocol)
            return bind_protocol(failure, plan_.steps()[member.index].node_id);
        }
      }
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
      if (auto group = task.actor->joint) {
        for (const auto& weak : group->pending_members)
          if (auto member = weak.lock()) {
            failure = phase_failure(*member);
            if (!failure.ok())
              return failure;
          }
        failure =
            group->starting ? group->started.status() : group->polled.status();
        if (failure.detail.origin == FailureOrigin::Protocol)
          return bind_protocol(failure, task.actor->node_id);
      }
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
        auto accepted =
            dispatch_(task.backend, task.whole, task.tiles,
                      task.frozen_producer, task.token, std::move(task.task));
        task.actor->queued = false;
        if (!accepted.ok()) {
          task.actor->pending->immediate = accepted.status();
          if (task.actor->joint) {
            for (const auto& weak : task.actor->joint->pending_members)
              if (auto member = weak.lock()) {
                member->queued = false;
                member->pending = task.actor->pending;
              }
          }
          break;
        }
        auto stage = accepted.take_value();
        stage.observations = task.observations;
        task.actor->pending = std::move(stage);
        if (task.actor->joint) {
          for (const auto& weak : task.actor->joint->pending_members)
            if (auto member = weak.lock()) {
              member->queued = false;
              member->pending = task.actor->pending;
            }
        }
        ++submitted;
      }
      diagnostics_.peak_active_tasks =
          std::max(diagnostics_.peak_active_tasks, submitted);
      // A parked coordinator continues under producer leases after retiring
      // its original caller. That caller's stop cannot fail healthy peers.
      const auto code = handoff_allowed_ ? active_stop() : ErrorCode::Ok;
      if (code != ErrorCode::Ok)
        stopped = external_stop_status(code);
#if defined(PHOTOSPIDER_ENABLE_EXECUTION_TEST_HOOKS)
      if (submitted)
        execution_testing::notify_post_submit_observation();
#endif
    }
    while (running_pending()) {
      const auto code = handoff_allowed_ ? active_stop() : ErrorCode::Ok;
      if (code != ErrorCode::Ok && stopped.code != code)
        stopped = external_stop_status(code);
      for (const auto& task : pending_tasks_)
        if (!task.actor->queued && task.actor->pending) {
          refresh_task(*task.actor);
          if (!task.actor->pending->ready())
            task.actor->pending->completion.wait_for(
                std::chrono::milliseconds(2));
        }
      if (plan_owner_ && shared_ && !callback_ && handoff_allowed_ &&
          caller_external_stop() != ErrorCode::Ok && pending_peers()) {
        auto protocol_failure = pending_protocol_failure();
        if (!protocol_failure.ok()) {
          wait_pending();
          return protocol_failure;
        }
        detaching_ = true;
        return Status{caller_external_stop(), {}};
      }
      if (!wait)
        break;
    }
    // A wave may retire before the caller observes its own stop. Preserve
    // its producer phases for peer driving even at that retirement boundary.
    if (plan_owner_ && shared_ && !callback_ && handoff_allowed_ &&
        caller_external_stop() != ErrorCode::Ok && pending_peers()) {
      auto protocol_failure = pending_protocol_failure();
      if (!protocol_failure.ok()) {
        wait_pending();
        return protocol_failure;
      }
      detaching_ = true;
      return Status{caller_external_stop(), {}};
    }
    if (!stopped.ok() && !running_pending()) {
      auto protocol_failure = pending_protocol_failure();
      return protocol_failure.ok() ? stopped : protocol_failure;
    }
    return Status::success();
  }
  void retire_joint_submission(const std::shared_ptr<JointActor>& group) {
    if (auto carrier = group->pending_members.front().lock();
        carrier && carrier->pending) {
      group->refresh();
      if (!carrier->queued) {
        carrier->pending->wait();
        finish_actor(carrier);
      } else {
        forget_pending(*carrier);
        carrier->pending.reset();
        carrier->queued = false;
      }
    }
    for (const auto& weak : group->pending_members)
      if (auto member = weak.lock()) {
        member->pending.reset();
        member->queued = false;
        member->busy = false;
      }
    group->pending_members.clear();
    group->pending = false;
  }
  void drain_actor(Actor& actor) {
    if (actor.joint && actor.joint->pending) {
      auto group = actor.joint;
      retire_joint_submission(group);
      fail_joint(group, Status{ErrorCode::Cancelled, {}});
    }
    if (actor.pending && !actor.queued) {
      refresh_task(actor);
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
  void refresh_shared() const {
    call_.refresh();
    if (active_shared())
      active_shared()->refresh();
    if (auto group = active_joint())
      group->refresh();
  }
  ErrorCode execution_stop(const SharedResults::Lease* producer,
                           const CancellationToken& token) const {
    if (producer)
      producer->refresh();
    if (token.cancelled())
      return ErrorCode::Cancelled;
    // A pinned frozen producer owns the captured graph and inputs. Its plan
    // predicate may describe only the originating demand's generation.
    if (!(producer && producer->producer() && plan_owner_) && !plan_.current())
      return ErrorCode::Stale;
    if (producer)
      return ErrorCode::Ok;
    const auto observer_failure = subscription_.code();
    return observer_failure == ErrorCode::Ok ? stop_() : observer_failure;
  }
  ErrorCode active_stop() const {
    refresh_shared();
    const auto* context = callback_context();
    if (auto group = active_joint();
        group && !active_shared() && (!context || !context->actor)) {
      if (group->cancellation.token().cancelled())
        return ErrorCode::Cancelled;
      const bool shared_producer = std::any_of(
          group->members.begin(), group->members.end(), [](const auto& member) {
            return member.shared.valid() && member.shared.producer();
          });
      if (!(shared_producer && plan_owner_) && !plan_.current())
        return ErrorCode::Stale;
      const auto observer = subscription_.code();
      return shared_producer             ? ErrorCode::Ok
             : observer != ErrorCode::Ok ? observer
                                         : stop_();
    }
    return execution_stop(active_shared(), active_token());
  }
  ErrorCode caller_external_stop() const {
    return cancellation_.cancelled() ? ErrorCode::Cancelled
           : !plan_.current()        ? ErrorCode::Stale
                                     : ErrorCode::Ok;
  }
  bool frozen_producer() const {
    if (!plan_owner_)
      return false;
    if (const auto* producer = active_shared())
      return producer->valid() && producer->producer();
    if (const auto group = active_joint()) {
      return std::any_of(
          group->members.begin(), group->members.end(), [](const auto& member) {
            return member.shared.valid() && member.shared.producer();
          });
    }
    return false;
  }
  struct ActiveScope {
    const SharedResults::Lease*& target;
    const SharedResults::Lease* previous;
    ResourcePayloadScope payload_scope;
    ActiveScope(StructuredExecution& owner, const SharedResults::Lease& lease)
        : target(owner.active_shared()),
          previous(target),
          payload_scope(owner.resources_,
                        owner.producer_capture(lease.payload_observation())) {
      if (lease.valid() && lease.producer())
        target = &lease;
    }
    ~ActiveScope() { target = previous; }
  };
  PayloadCapture producer_capture(
      std::shared_ptr<PayloadObservation> producer) const {
    auto capture = ResourcePayloadScope::capture(resources_);
    if (producer)
      capture.producer = std::move(producer);
    return capture;
  }
  std::function<Status(const StructuredServices&)> prepare_task(
      std::function<Status()> task, Actor* actor,
      const std::shared_ptr<ExecutionDiagnostics>& observations) {
    const auto* current = active_shared();
    auto shared = current ? *current : SharedResults::Lease{};
    const auto optional = work_mode();
    const auto depth = service_depth();
    auto joint = active_joint();
    auto payload = ResourcePayloadScope::capture(resources_);
    return [this, task = std::move(task), shared = std::move(shared), optional,
            depth, actor, joint, payload,
            observations](const StructuredServices& services) {
      ResourcePayloadScope payload_scope(resources_, payload);
      CallbackContext context(this, actor, &services, shared, optional, depth,
                              joint);
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
                                      Backend backend, bool whole, bool tiles,
                                      Actor* actor = nullptr) {
    auto observations = std::allocate_shared<ExecutionDiagnostics>(
        ResourceAllocator<ExecutionDiagnostics>(resources_));
    auto submitted =
        dispatch_(backend, whole, tiles, frozen_producer(), active_token(),
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
      if (value > UINT64_MAX - target)
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
      refresh_shared();
      pending.completion.wait_for(std::chrono::milliseconds(2));
    }
    refresh_shared();
    auto finished = pending.finish();
    auto merged = merge_observations(pending);
    return finished.ok() ? merged : finished;
  }
  Status dispatch(std::function<Status()> task, Backend backend = Backend::Cpu,
                  bool whole = false, bool tiles = false) {
    auto submitted = submit(std::move(task), backend, whole, tiles);
    return submitted.ok() ? await(submitted.value()) : submitted.status();
  }
  Status admit_poll_diagnostics(const Actor& actor) {
    const auto output = plan_.steps()[actor.index].result_ref();
    const auto found = std::find_if(
        diagnostics_.operation_timings.begin(),
        diagnostics_.operation_timings.end(), [&](const auto& item) {
          return item.output == output && item.backend == actor.query.backend;
        });
    if (found == diagnostics_.operation_timings.end())
      diagnostics_.operation_timings.push_back(
          {output, actor.query.backend, 0, ErrorCode::Ok, 0, 0});
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
                 item.backend == actor.query.backend;
        });
    if (timing == diagnostics_.operation_timings.end())
      return protocol("missing admitted Result poll diagnostics");
    const auto elapsed = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - completed.started)
            .count());
    if (timing->invocation_count == UINT64_MAX ||
        elapsed > UINT64_MAX - timing->duration_us ||
        completed.native_dispatches >
            UINT64_MAX - timing->native_dispatch_count)
      return Status{ErrorCode::ResourceExhausted,
                    "Result poll diagnostic counter overflow"};
    auto merged =
        merge_numeric_diagnostics(&timing->numeric, completed.numeric);
    if (!merged.ok())
      return merged;
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
                      Backend backend = Backend::Cpu, bool whole = false,
                      bool tiles = false, bool joint_task = false) {
    if (actor->pending)
      return protocol("structured actor already has a pending stage");
    if (!joint_task && std::holds_alternative<PollPhase>(actor->phase)) {
      auto admitted = admit_poll_diagnostics(*actor);
      if (!admitted.ok())
        return admitted;
    }
    if (!callback_context()) {
      auto observations = std::allocate_shared<ExecutionDiagnostics>(
          ResourceAllocator<ExecutionDiagnostics>(resources_));
      auto prepared = prepare_task(
          std::move(task), joint_task ? nullptr : actor.get(), observations);
      pending_tasks_.push_back({actor, backend, whole, tiles, frozen_producer(),
                                active_token(), std::move(prepared),
                                observations});
      actor->pending.emplace();
      actor->queued = true;
      ++progress_;
      return Status::success();
    }
    auto submitted = submit(std::move(task), backend, whole, tiles,
                            joint_task ? nullptr : actor.get());
    if (!submitted.ok())
      return submitted.status();
    actor->pending = submitted.take_value();
    return Status::success();
  }
  Status finish_actor(const std::shared_ptr<Actor>& actor) {
    if (!actor->pending || actor->queued)
      return protocol("structured actor has no submitted stage");
    struct Reset {
      StructuredExecution& owner;
      Actor& actor;
      ~Reset() {
        actor.pending.reset();
        if (!owner.callback_context())
          owner.forget_pending(actor);
      }
    } reset{*this, *actor};
    ++progress_;
    auto finished = await(*actor->pending);
    if (actor->joint && actor->joint->starting && actor->joint->entered) {
      ++diagnostics_.joint_groups;
      actor->joint->entered = false;
    }
    return finished;
  }
  static bool result_joint_candidate(const PlanStep& step) {
    return step.backend == Backend::Cpu && step.output_result_schema &&
           (step.traits.joint_contract == 1 ||
            step.traits.joint_contract == 2) &&
           step.traits.outputs[0].dependency_version == 2 &&
           step.traits.outputs[0].observation_kind == ObservationKind::Atomic;
  }
  bool joint_observation(const Actor& actor) const {
    const auto& tensors = actor.query.output.result_schema->tensors;
    if (tensors.empty())
      return true;
    const auto& tensor = tensors[actor.query.tensor_slot];
    const auto shape = tensor.sample_shape();
    auto dimensions = Region::whole(shape).dimensions();
    if (actor.query.tensor_outputs) {
      if (actor.query.tensor_outputs->empty() ||
          actor.query.tensor_outputs->boxes().size() != 1)
        return false;
      dimensions = actor.query.tensor_outputs->boxes()[0].dimensions();
    }
    const auto tuple =
        input_internal::tuple_channel_axis(tensor.descriptor, tensor.facets);
    for (std::size_t axis = 0; axis < shape.size(); ++axis) {
      const bool grouped = axis >= shape.size() - tensor.atomic_trailing_axes ||
                           (tuple && axis == *tuple + tensor.batch_axes.size());
      if (!grouped && dimensions[axis].extent != 1)
        return false;
    }
    return true;
  }
  void release_joint(const std::shared_ptr<JointActor>& group, bool fallback) {
    if (fallback)
      ++diagnostics_.joint_fallbacks;
    Status restored = Status::success();
    if (fallback && group->saved_records) {
      records_ = std::move(group->saved_records);
      for (const auto& member : group->members)
        if (auto actor = member.actor.lock();
            actor && actor->complete && actor->published.valid()) {
          auto bundle = actor->published.dependencies();
          if (bundle && restored.ok())
            restored = records_->import_bundle(*bundle, actor->index);
        }
    }
    group->continuation = {};
    group->started =
        Result<ResultJointContinuation>(Status{ErrorCode::Stale, {}});
    for (auto& member : group->members)
      if (auto actor = member.actor.lock()) {
        actor->joint.reset();
        actor->joint_disabled = true;
        if (!restored.ok() && !actor->complete &&
            actor->terminal == ErrorCode::Ok)
          retire(*actor, restored);
        if (fallback && !actor->complete && actor->terminal == ErrorCode::Ok) {
          if (const auto* completed = std::get_if<PollPhase>(&actor->phase)) {
            auto recorded = record_poll_diagnostics(
                *actor, *completed, completed->result.status().code);
            if (!recorded.ok()) {
              retire(*actor, recorded);
              continue;
            }
          }
          actor->phase.emplace<std::monostate>();
          actor->waiting.reset();
          actor->results.clear();
          actor->tensors.clear();
          actor->io.clear();
          actor->history.clear();
          actor->input_facts.clear();
          actor->input_bundles.clear();
          actor->input_obligations = {};
          actor->cache_replay.clear();
        }
        if (fallback && !actor->complete && actor->terminal == ErrorCode::Ok) {
          actor->deferred_start = true;
          actor->scalar_restart = true;
        }
      }
  }
  Status fail_joint(const std::shared_ptr<JointActor>& group,
                    const Status& failure) {
    for (auto& member : group->members)
      if (auto actor = member.actor.lock();
          actor && !actor->complete && actor->terminal == ErrorCode::Ok) {
        if (const auto* completed = std::get_if<PollPhase>(&actor->phase);
            completed && !actor->pending)
          record_failed_poll(*actor, *completed, failure);
        retire(*actor, failure);
      }
    release_joint(group, false);
    return Status::success();
  }
  bool optional_joint_failure(const Status& failure) const {
    return failure.code != ErrorCode::Cancelled &&
           failure.code != ErrorCode::Stale &&
           failure.detail.origin != FailureOrigin::Protocol &&
           failure.detail.origin != FailureOrigin::Domain &&
           failure.detail.origin != FailureOrigin::Schema &&
           failure.detail.scope == FailureScope::Unspecified;
  }
  Status submit_joint(const std::shared_ptr<JointActor>& group,
                      ResourceVector<std::shared_ptr<Actor>> ready,
                      bool starting) {
    if (ready.empty())
      return protocol("empty structured joint task");
    if (!starting) {
      for (const auto& actor : ready) {
        auto admitted = admit_poll_diagnostics(*actor);
        if (!admitted.ok())
          return admitted;
      }
    }
    JointScope scope(*this, group);
    group->refresh();
    group->pending_members.reserve(ready.size());
    group->starting = starting;
    group->pending = true;
    group->pending_members.clear();
    for (const auto& actor : ready) {
      actor->busy = true;
      actor->driver = std::this_thread::get_id();
      group->pending_members.push_back(actor);
    }
    auto carrier = ready.front();
    const bool tiles =
        std::any_of(ready.begin(), ready.end(), [&](const auto& actor) {
          return plan_.steps()[actor->index].traits.cpu_staged_tiles;
        });
    if (!starting)
      ++diagnostics_.joint_polls;
    auto submitted = submit_actor(
        carrier,
        [this, group, ready = std::move(ready), starting] {
          if (starting) {
            group->entered = true;
            ResourceVector<ResultProgramQuery> queries{
                ResourceAllocator<ResultProgramQuery>(resources_)};
            for (const auto& actor : ready)
              queries.push_back(actor->query);
            auto allocator = resources_.allocator().limited(
                options_.dependencies.maximum_state_bytes,
                [first = group->failure](ErrorCode code) {
                  first->record(Status{code, {}});
                });
            group->started = operations_->start_result_joint_compiled(
                plan_.steps()[ready.front()->index].operation, queries,
                resources_, allocator);
            return Status::success();
          }
          ResourceVector<const ResultProgramPhase*> borrowed{
              ResourceAllocator<const ResultProgramPhase*>(resources_)};
          borrowed.reserve(ready.size());
          std::function<void(std::size_t)> build = [&](std::size_t position) {
            if (position == ready.size()) {
              if (borrowed.empty()) {
                group->polled = Result<ResourceVector<ResultJointOutcome>>(
                    ResourceVector<ResultJointOutcome>{});
              } else {
                CallbackContext group_context(this, nullptr, active_services(),
                                              {}, false, service_depth(),
                                              group);
                JointScope scope(*this, group);
                auto allocator = active_services()
                                     ? active_services()->allocator
                                     : resources_.allocator();
                group->polled = group->continuation.poll(
                    {borrowed, allocator,
                     [&](std::uint64_t count) { return consume(count); }});
              }
              return;
            }
            auto actor = ready[position];
            bool visited = false;
            auto status = poll_actor_phase(
                actor->index, actor, [&](const ResultProgramPhase& phase) {
                  visited = true;
                  borrowed.push_back(&phase);
                  build(position + 1);
                  borrowed.pop_back();
                  if (!group->polled.ok())
                    return Result<ResultProgramPoll>(group->polled.status());
                  auto& results = group->polled.value();
                  auto found = std::find_if(
                      results.begin(), results.end(), [&](const auto& result) {
                        return result.key == actor->observation;
                      });
                  if (found != results.end())
                    actor->quality = found->quality;
                  return found == results.end()
                             ? Result<ResultProgramPoll>(
                                   protocol("missing structured joint outcome"))
                             : std::move(found->outcome);
                });
            if (!status.ok())
              std::get<PollPhase>(actor->phase).result =
                  Result<ResultProgramPoll>(status);
            if (!visited)
              build(position + 1);
          };
          build(0);
          // A retained capability belonging to a previously completed member
          // must still make ignored protocol errors visible to the shared
          // continuation.
          for (const auto& member : group->members)
            if (member.done->load()) {
              auto failed = member.failure->snapshot();
              if (failed.detail.origin == FailureOrigin::Protocol)
                group->polled =
                    Result<ResourceVector<ResultJointOutcome>>(failed);
            }
          return Status::success();
        },
        Backend::Cpu, false, tiles, true);
    if (!submitted.ok()) {
      group->pending = false;
      for (const auto& weak : group->pending_members)
        if (auto actor = weak.lock())
          actor->busy = false;
      group->pending_members.clear();
      return submitted;
    }
    for (const auto& weak : group->pending_members)
      if (auto actor = weak.lock(); actor && actor != carrier) {
        actor->pending = carrier->pending;
        actor->queued = carrier->queued;
      }
    return Status::success();
  }
  Result<bool> start_joint(std::size_t index,
                           const std::shared_ptr<Actor>& current) {
    const bool contract2 = plan_.steps()[index].traits.joint_contract == 2;
    if ((!options_.enable_joint && !contract2) ||
        !result_joint_candidate(plan_.steps()[index]) ||
        current->joint_disabled || joint_depth_ >= 16 ||
        current->query.prepared->traits().joint_continuation_bytes >
            options_.dependencies.maximum_state_bytes)
      return contract2 ? Result<bool>(Status{
                             ErrorCode::ResourceExhausted,
                             "Result atom joint admission limit",
                             FailureReason::WorkLimit,
                             {FailureOrigin::Resource, FailureScope::Group}})
                       : Result<bool>(false);
    std::shared_ptr<JointActor> group;
    ResourceAllocationScope admission_scope(resources_);
    try {
      ResourceVector<std::shared_ptr<Actor>> candidates{
          ResourceAllocator<std::shared_ptr<Actor>>(resources_)};
      const auto admit = [&](const std::shared_ptr<Actor>& actor) {
        if (!actor || actor->joint || actor->joint_disabled ||
            !actor->deferred_start || !actor->initialized || actor->complete ||
            actor->terminal != ErrorCode::Ok || actor->pending ||
            actor->waiting ||
            (actor->shared.valid() && !actor->shared.producer()) ||
            !joint_observation(*actor))
          return;
        if (contract2 && std::any_of(candidates.begin(), candidates.end(),
                                     [&](const auto& other) {
                                       return other->query.output_index ==
                                                  actor->query.output_index &&
                                              other->query.tensor_slot !=
                                                  actor->query.tensor_slot;
                                     }))
          return;
        if (std::none_of(candidates.begin(), candidates.end(),
                         [&](const auto& other) { return other == actor; }))
          candidates.push_back(actor);
      };
      if (contract2) {
        // Contract 2 is required even for a singleton: scalar starts cannot
        // transport its typed failure or quality evidence.
        admit(current);
        if (options_.enable_joint) {
          for (const auto& weak : actors_) {
            if (candidates.size() == 64)
              break;
            auto charged = consume(1);
            if (!charged.ok())
              return Result<bool>(charged);
            auto candidate = weak.lock();
            if (candidate && candidate->node_id == current->node_id &&
                candidates.size() < 64)
              admit(candidate);
          }
        }
      } else {
        for (const auto& planned : plan_.execution_groups()) {
          if (planned.node_id != current->node_id)
            continue;
          for (auto member : planned.members) {
            auto actor = actor_aliases_[member];
            admit(actor);
          }
          break;
        }
      }
      if (candidates.size() < (contract2 ? 1U : 2U) || candidates.size() > 64 ||
          std::find(candidates.begin(), candidates.end(), current) ==
              candidates.end())
        return Result<bool>(false);
      group = std::allocate_shared<JointActor>(
          ResourceAllocator<JointActor>(resources_), resources_);
      group->contract2 = contract2;
      if (!contract2) {
        // Members are not attached yet, so use the existing caller/producer
        // token to admit the initial rollback record before creating the shared
        // task.
        records_->set_cancellation(current->query.cancellation);
        auto saved = records_->fork_result_attempt();
        if (!saved.ok()) {
          if (contract2)
            return Result<bool>(saved.status());
          current->joint_disabled = true;
          return Result<bool>(false);
        }
        group->saved_records = saved.take_value();
      }
      group->members.reserve(candidates.size());
      group->pending_members.reserve(candidates.size());
      for (const auto& actor : candidates) {
        JointActor::Member member;
        member.index = actor->index;
        member.actor = actor;
        member.shared = actor->shared;
        member.token = actor->query.cancellation;
        member.failure = actor->service_failure;
        member.done = std::allocate_shared<std::atomic<bool>>(
            ResourceAllocator<std::atomic<bool>>(resources_), false);
        group->members.push_back(std::move(member));
      }
      for (const auto& member : group->members)
        member.shared.observe_joint_payload(group->payload);
      for (std::size_t slot = 0; slot < candidates.size(); ++slot) {
        candidates[slot]->joint = group;
        candidates[slot]->joint_slot = slot;
        candidates[slot]->deferred_start = false;
      }
      auto status = submit_joint(group, std::move(candidates), true);
      if (!status.ok()) {
        if (!contract2 && optional_joint_failure(status))
          release_joint(group, true);
        else
          fail_joint(group, status);
      }
      return Result<bool>(true);
    } catch (const std::bad_alloc&) {
      if (group && group->pending && !group->pending_members.empty()) {
        auto carrier = group->pending_members.front().lock();
        if (carrier && carrier->pending)
          throw;
        group->pending = false;
        for (const auto& weak : group->pending_members)
          if (auto actor = weak.lock())
            actor->busy = false;
        group->pending_members.clear();
      }
      const bool attached = static_cast<bool>(current->joint);
      if (contract2) {
        if (attached)
          fail_joint(group, Status{ErrorCode::ResourceExhausted, {}});
        return Result<bool>(Status{ErrorCode::ResourceExhausted, {}});
      }
      if (attached)
        release_joint(group, true);
      current->joint_disabled = true;
      return Result<bool>(attached);
    }
  }
  Status register_joint_need(std::size_t index, Actor& consumer,
                             const ResultProgramNeed& need) {
    const auto& step = plan_.steps()[index];
    ActiveScope producer_scope(*this, consumer.shared);
    auto valid = plugin_internal::validate_result_need(
        need, consumer.query, step.traits.outputs[0], resources_,
        std::min<std::uint64_t>(65536,
                                options_.dependencies.sets.maximum_boxes),
        options_.maximum_result_window_bytes, false,
        [&](std::uint64_t count) { return consume(count); });
    if (!valid.ok())
      return valid;
    return register_need_inputs(index, consumer, need, false);
  }
  Status register_need_inputs(std::size_t index, Actor& consumer,
                              const ResultProgramNeed& need,
                              bool only_contract_one) {
    const auto& step = plan_.steps()[index];
    ActiveScope producer_scope(*this, consumer.shared);
    const auto eligible = [&](std::uint32_t port) {
      const auto* input = std::get_if<PlanStepInput>(&step.inputs[port]);
      return input &&
             (!only_contract_one ||
              (plan_.steps()[input->step_index].traits.joint_contract == 1 &&
               plan_.steps()[input->step_index].backend == Backend::Cpu));
    };
    ResourceVector<std::size_t> registering_steps{
        ResourceAllocator<std::size_t>(resources_)};
    const auto mark = [&](std::uint32_t port) {
      if (!eligible(port))
        return;
      const auto source = std::get<PlanStepInput>(step.inputs[port]).step_index;
      if (plan_.steps()[source].traits.joint_contract == 1)
        registering_steps.push_back(source);
    };
    for (const auto& request : need.results)
      mark(request.input);
    for (const auto& request : need.tensors)
      mark(request.input);
    unsigned sort_levels = 1;
    for (auto count = registering_steps.size(); count > 1; count >>= 1)
      ++sort_levels;
    auto registration_work = consume(registering_steps.size() * sort_levels);
    if (!registration_work.ok())
      return registration_work;
    std::sort(registering_steps.begin(), registering_steps.end());
    registering_steps.erase(
        std::unique(registering_steps.begin(), registering_steps.end()),
        registering_steps.end());
    auto previous = std::atomic_load(&c1_need_registration_);
    if (!registering_steps.empty()) {
      auto current = std::allocate_shared<C1NeedRegistration>(
          ResourceAllocator<C1NeedRegistration>(resources_), previous,
          std::move(registering_steps));
      std::atomic_store(
          &c1_need_registration_,
          std::shared_ptr<const C1NeedRegistration>(std::move(current)));
    }
    struct Registration {
      std::shared_ptr<const C1NeedRegistration>& slot;
      std::shared_ptr<const C1NeedRegistration> previous;
      ~Registration() { std::atomic_store(&slot, std::move(previous)); }
    } registering{c1_need_registration_, std::move(previous)};
    const auto register_input = [&](std::uint32_t port,
                                    std::optional<Footprint> samples,
                                    std::uint32_t slot) -> Status {
      const auto* input = std::get_if<PlanStepInput>(&step.inputs[port]);
      if (!eligible(port))
        return Status::success();
      auto acquired = actor(input->step_index, std::move(samples), slot, true);
      return acquired.ok() ? Status::success() : acquired.status();
    };
    for (const auto& request : need.results) {
      auto registered = register_input(request.input, {}, 0);
      if (!registered.ok())
        return registered;
    }
    using TensorKey = std::pair<std::uint32_t, std::uint32_t>;
    std::map<TensorKey, Footprint, std::less<TensorKey>,
             ResourceAllocator<std::pair<const TensorKey, Footprint>>>
        demands{std::less<TensorKey>{},
                ResourceAllocator<std::pair<const TensorKey, Footprint>>(
                    resources_)};
    for (const auto& request : need.tensors) {
      if (!eligible(request.input))
        continue;
      auto charged = consume(1);
      if (!charged.ok())
        return charged;
      auto samples =
          request.roles & 7U
              ? consumer.query.inputs[request.input]
                    .result_schema->tensors[request.slot]
                    .close_samples(request.samples, set_limits())
              : Footprint::none(request.samples.shape(), set_limits());
      if (!samples.ok())
        return samples.status();
      auto key = TensorKey{request.input, request.slot};
      auto found = demands.find(key);
      if (found == demands.end()) {
        demands.emplace(key, samples.take_value());
      } else {
        auto joined = found->second.unite(samples.value(), set_limits());
        if (!joined.ok())
          return joined.status();
        found->second = joined.take_value();
      }
    }
    for (auto& demand : demands) {
      auto registered = register_input(
          demand.first.first, std::move(demand.second), demand.first.second);
      if (!registered.ok())
        return registered;
    }
    return Status::success();
  }
  Status finish_joint(const std::shared_ptr<JointActor>& group) {
    JointScope scope(*this, group);
    const bool previously_driving = group->driving;
    group->driving = true;
    struct RestoreDriving {
      JointActor& group;
      bool previous;
      ~RestoreDriving() { group.driving = previous; }
    } restore{*group, previously_driving};
    try {
      return finish_joint_impl(group);
    } catch (const std::bad_alloc&) {
      if (group->pending)
        retire_joint_submission(group);
      return fail_joint(group, Status{ErrorCode::ResourceExhausted, {}});
    } catch (...) {
      if (group->pending)
        retire_joint_submission(group);
      return fail_joint(group, Status{ErrorCode::OperationFailed, {}});
    }
  }
  Status finish_joint_impl(const std::shared_ptr<JointActor>& group) {
    if (group->pending_members.empty())
      return protocol("missing structured joint retirement owner");
    auto carrier = group->pending_members.front().lock();
    if (!carrier || !carrier->pending)
      return protocol("missing structured joint submission");
    std::array<std::shared_ptr<Actor>, 64> storage;
    struct Ready {
      std::shared_ptr<Actor>* items;
      std::size_t count = 0;
      auto begin() { return items; }
      auto end() { return items + count; }
      auto size() const { return count; }
      auto& operator[](std::size_t i) { return items[i]; }
    } ready{storage.data()};
    for (const auto& weak : group->pending_members)
      if (auto actor = weak.lock())
        storage[ready.count++] = std::move(actor);
    auto status = finish_actor(carrier);
    for (const auto& actor : ready) {
      actor->pending.reset();
      actor->busy = false;
    }
    group->pending_members.clear();
    group->pending = false;
    if (!status.ok()) {
      if (!group->contract2 && optional_joint_failure(status))
        release_joint(group, true);
      else
        fail_joint(group, status);
      return Status::success();
    }
    if (group->starting) {
      auto first = group->failure->snapshot();
      if (first.ok() && !group->started.ok())
        first = group->started.status();
      if (!first.ok()) {
        if (!group->contract2 && optional_joint_failure(first))
          release_joint(group, true);
        else
          fail_joint(group, first);
      } else {
        group->continuation = group->started.take_value();
      }
      group->starting = false;
      return Status::success();
    }
    if (!group->polled.ok()) {
      const auto first = group->polled.status();
      if (!group->contract2 && optional_joint_failure(first))
        release_joint(group, true);
      else
        fail_joint(group, first);
      return Status::success();
    }
    // A contract-2 domain reply may include a member whose Need is still
    // Waiting. Such a reply must retire that exact Actor, not the latest alias
    // for its output and not only the Ready subset of this poll.
    if (group->contract2) {
      for (auto& reply : group->polled.value()) {
        if (std::any_of(ready.begin(), ready.end(), [&](const auto& actor) {
              return actor->observation == reply.key;
            }))
          continue;
        auto member =
            std::find_if(group->members.begin(), group->members.end(),
                         [&](const auto& item) {
                           auto actor = item.actor.lock();
                           return actor && actor->observation == reply.key;
                         });
        if (member == group->members.end() || ready.count == storage.size())
          return fail_joint(group, protocol("unknown structured atom reply"));
        auto actor = member->actor.lock();
        if (reply.outcome.ok() || actor->complete ||
            actor->terminal != ErrorCode::Ok)
          return fail_joint(group, protocol("invalid Waiting atom reply"));
        actor->waiting.reset();
        actor->phase.emplace<PollPhase>();
        std::get<PollPhase>(actor->phase).result = std::move(reply.outcome);
        actor->quality = reply.quality;
        storage[ready.count++] = std::move(actor);
      }
      // Domain finality covers all cohorts and cache hits in this Run. Check
      // the complete envelope before recording any newly terminal success.
      for (const auto& actor : ready) {
        const auto& result = std::get<PollPhase>(actor->phase).result;
        if (result.ok() ||
            result.status().detail.scope != FailureScope::ValidationDomain)
          continue;
        auto& domain = validation_domains_.at(domain_key(*actor));
        if (domain.semantic_terminal)
          return fail_joint(
              group, protocol("Result validation domain revoked a Run atom"));
      }
      for (const auto& actor : ready) {
        const auto& result = std::get<PollPhase>(actor->phase).result;
        if (result.ok() ||
            result.status().detail.scope != FailureScope::ValidationDomain)
          continue;
        auto& domain = validation_domains_.at(domain_key(*actor));
        auto failure = result.status();
        failure.detail.node_id = actor->node_id;
        domain.failure.record(failure);
        domain.quality = actor->quality;
      }
    }
    // Validate every returned Need against host limits before publishing any
    // success or creating its upstream producer.
    for (const auto& actor : ready) {
      auto& result = std::get<PollPhase>(actor->phase).result;
      if (result.ok()) {
        if (const auto* need =
                std::get_if<ResultProgramNeed>(&result.value())) {
          auto valid = plugin_internal::validate_result_need(
              *need, actor->query,
              plan_.steps()[actor->index].traits.outputs[0], resources_,
              std::min<std::uint64_t>(65536,
                                      options_.dependencies.sets.maximum_boxes),
              options_.maximum_result_window_bytes, false,
              [&](std::uint64_t count) { return consume(count); });
          if (!valid.ok())
            return fail_joint(group, valid);
        }
      }
    }
    ResourceVector<ResultRef> certified(
        ready.size(), ResultRef{}, ResourceAllocator<ResultRef>(resources_));
    records_->set_cancellation(group->cancellation.token());
    auto transaction = records_->fork_result_attempt();
    if (!transaction.ok())
      return fail_joint(group, transaction.status());
    auto original = std::move(records_);
    records_ = transaction.take_value();
    for (std::size_t i = 0; i < ready.size(); ++i) {
      auto& actor = *ready[i];
      auto& result = std::get<PollPhase>(actor.phase).result;
      if (!result.ok() ||
          !std::holds_alternative<ResultPublication>(result.value()))
        continue;
      ActiveScope producer_scope(*this, actor.shared);
      if (actor.query.cancellation.cancelled()) {
        result = Result<ResultProgramPoll>(Status{ErrorCode::Cancelled, {}});
        continue;
      }
      records_->set_cancellation(actor.query.cancellation);
      auto candidate = records_->fork_result_attempt();
      if (!candidate.ok()) {
        if (candidate.status().code == ErrorCode::Cancelled &&
            !group->cancellation.token().cancelled()) {
          result = Result<ResultProgramPoll>(candidate.status());
          continue;
        }
        records_ = std::move(original);
        return fail_joint(group, candidate.status());
      }
      auto preceding = std::move(records_);
      records_ = candidate.take_value();
      auto captured = stage_publication(
          actor.index, actor, std::get<ResultPublication>(result.value()));
      if (!captured.ok()) {
        auto failed = captured.status();
        records_ = std::move(preceding);
        if (failed.detail.origin == FailureOrigin::Protocol ||
            failed.code == ErrorCode::InvalidArgument ||
            failed.code == ErrorCode::TypeMismatch) {
          if (failed.detail.origin != FailureOrigin::Protocol)
            failed = protocol("invalid structured joint publication evidence");
          records_ = std::move(original);
          return fail_joint(group, failed);
        }
        result = Result<ResultProgramPoll>(failed);
      } else {
        certified[i] = captured.take_value();
      }
    }
    original.reset();
    for (std::size_t i = 0; i < ready.size(); ++i) {
      auto actor = ready[i];
      ActiveScope producer_scope(*this, actor->shared);
      auto completed = std::move(std::get<PollPhase>(actor->phase));
      actor->phase.emplace<std::monostate>();
      auto applied = complete_poll(
          actor->index, actor, Status::success(), std::move(completed),
          certified[i].valid() ? &certified[i] : nullptr);
      if (!applied.ok() && actor->terminal == ErrorCode::Ok)
        retire(*actor, applied);
    }
    // Register all newly declared upstream demands before driving the first.
    for (const auto& actor : ready)
      if (const auto* need = std::get_if<NeedPhase>(&actor->phase)) {
        auto registered =
            register_joint_need(actor->index, *actor, need->request);
        if (!registered.ok())
          retire(*actor, registered);
      }
    if (group->complete())
      release_joint(group, false);
    return Status::success();
  }
  Status advance_joint(const std::shared_ptr<JointActor>& requested_group) {
    auto group = requested_group;
    if (group->driving)
      return Status{ErrorCode::Cycle, {}};
    if (!callback_context() && group->visit == traversal_)
      return Status::success();
    group->visit = traversal_;
    group->driving = true;
    ++joint_depth_;
    struct Reset {
      JointActor& group;
      std::uint32_t& depth;
      ~Reset() {
        group.driving = false;
        --depth;
      }
    } reset{*group, joint_depth_};
    JointScope scope(*this, group);
    group->refresh();
    if (group->pending) {
      auto carrier = group->pending_members.front().lock();
      if (!carrier || !carrier->pending)
        return fail_joint(group, protocol("missing structured joint task"));
      if (carrier->queued)
        return Status::success();
      if (!carrier->pending->ready()) {
        carrier->pending->completion.wait_for(std::chrono::milliseconds(2));
        if (!carrier->pending->ready()) {
          if (plan_owner_ && shared_ && !callback_ && handoff_allowed_ &&
              caller_external_stop() != ErrorCode::Ok && group->peers()) {
            auto protocol_failure = pending_protocol_failure();
            if (!protocol_failure.ok()) {
              wait_pending();
              return protocol_failure;
            }
            detaching_ = true;
            return Status{caller_external_stop(), {}};
          }
          return Status::success();
        }
      }
      return finish_joint(group);
    }
    if (group->complete()) {
      release_joint(group, false);
      return Status::success();
    }
    // Retire an upstream submission before another member can submit its next
    // phase. Distinct queries of the same output have independent Actors.
    if (!callback_context() && first_pending_actor() &&
        !first_pending_actor()->queued) {
      auto pending = first_pending_actor();
      auto progressed = advance(pending->index, *pending);
      if (!progressed.ok() && detaching_)
        return progressed;
      if (!progressed.ok() && pending->terminal == ErrorCode::Ok)
        retire(*pending, progressed);
      return Status::success();
    }
    for (auto& member : group->members) {
      auto actor = member.actor.lock();
      if (!actor || member.done->load())
        continue;
      if (auto* need = std::get_if<NeedPhase>(&actor->phase)) {
        ActiveScope producer_scope(*this, actor->shared);
        auto supplied =
            supply_need_step(member.index, *actor, need->request, need->cursor);
        if (!supplied.ok()) {
          retire(*actor, supplied);
        } else if (need->cursor.complete()) {
          retain_cache_need(member.index, *actor, need->request);
          actor->phase.emplace<std::monostate>();
        }
        // Retire the submitted wave before advancing the joint frontier.
        if (running_pending())
          return Status::success();
      }
    }
    ResourceVector<std::shared_ptr<Actor>> ready{
        ResourceAllocator<std::shared_ptr<Actor>>(resources_)};
    for (auto& member : group->members) {
      auto actor = member.actor.lock();
      if (!actor || member.done->load() || actor->waiting || actor->pending ||
          !std::holds_alternative<std::monostate>(actor->phase))
        continue;
      ActiveScope producer_scope(*this, actor->shared);
      auto charged = consume(1);
      if (!charged.ok()) {
        retire(*actor, charged);
        continue;
      }
      if (actor->polls >= std::min(plan_.steps()[member.index]
                                       .traits.outputs[0]
                                       .maximum_dependency_stages,
                                   options_.dependencies.maximum_stages)) {
        retire(*actor, Status{ErrorCode::ResourceExhausted,
                              "structured joint stage limit"});
        continue;
      }
      ++actor->polls;
      actor->phase.emplace<PollPhase>();
      std::get<PollPhase>(actor->phase).sticky = actor->failure->load();
      ready.push_back(std::move(actor));
    }
    if (!ready.empty()) {
      auto status = submit_joint(group, std::move(ready), false);
      if (!status.ok()) {
        if (!group->contract2 && optional_joint_failure(status))
          release_joint(group, true);
        else
          fail_joint(group, status);
      }
    } else if (group->complete()) {
      release_joint(group, false);
    }
    return Status::success();
  }
  Status start_actor(std::size_t index, const std::shared_ptr<Actor>& current) {
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
    return submit_actor(current, [this, index, current] {
      auto& actor = *current;
      auto& phase = std::get<StartPhase>(actor.phase);
      const auto& step = plan_.steps()[index];
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
      ResultCallbackScope scope(&phase.sticky, actor.service_failure.get());
      ResourceAllocationScope metadata_scope(resources_, &phase.sticky);
      auto allocator = resources_.allocator().limited(
          std::min(step.traits.outputs[0].continuation_bytes,
                   options_.dependencies.maximum_state_bytes),
          [full = actor.service_failure](ErrorCode code) {
            full->record(Status{code, {}});
          });
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
  Status operation_work(uint64_t count) {
    const auto stopped = active_stop();
    return stopped == ErrorCode::Ok ? resources_.consume({count})
                                    : Status{stopped, {}};
  }
  CancellationToken active_token() const {
    if (active_shared())
      return active_shared()->token();
    if (const auto* context = callback_context();
        context && context->actor && context->actor->joint)
      return context->actor->query.cancellation;
    if (auto group = active_joint())
      return group->cancellation.token();
    return cancellation_;
  }
  FootprintLimits set_limits() {
    auto limits = options_.dependencies.sets;
    limits.cancellation = active_token();
    limits.consume_work = [&](std::uint64_t count) { return consume(count); };
    return limits;
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
          !plan_.steps()[i].traits.side_effect_free) {
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
    auto actor = actor_aliases_[index];
    if (!actor || !actor->complete || actor->pending || actor->joint)
      return Status::success();
    auto charged = consume(1 + actor->aliases.size());
    if (!charged.ok())
      return charged;
    if (std::any_of(actor->aliases.begin(), actor->aliases.end(),
                    [&](auto alias) {
                      return remaining_consumers_[alias] || named_pins_[alias];
                    }))
      return Status::success();
    for (auto alias : actor->aliases)
      if (actor_aliases_[alias] == actor)
        actor_aliases_[alias].reset();
    release_actor_registration(*actor, true);
    actor->published = {};
    actor->shared = {};
    return Status::success();
  }
  Status complete_liveness(Actor& actor) {
    if (remaining_consumers_.empty() || !actor.complete ||
        plan_.steps()[actor.index].traits.joint_contract == 2 ||
        plan_.steps()[actor.index].traits.outputs[0].region_rule !=
            OperationRegionRule::Whole)
      return Status::success();
    auto facts = actor.published.descriptor(false);
    if (!facts.ok())
      return facts.status();
    for (std::uint32_t slot = 0; slot < facts.value().tensor_count(); ++slot) {
      auto missing = Footprint::all(
          actor.published.schema().tensors[slot].sample_shape(), set_limits());
      if (!missing.ok())
        return missing.status();
      missing = missing.value().subtract(facts.value().tensor_coverage(slot),
                                         set_limits());
      if (!missing.ok())
        return missing.status();
      if (!missing.value().empty())
        return Status::success();
    }
    for (auto index : actor.aliases) {
      if (finished_steps_[index])
        continue;
      finished_steps_[index] = true;
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
                std::get_if<PlanStepInput>(&step.inputs[port]);
            producer && remaining_consumers_[producer->step_index]) {
          auto& remaining = remaining_consumers_[producer->step_index];
          if (!--remaining) {
            auto released = release_unused_result(producer->step_index);
            if (!released.ok())
              return released;
          }
        }
      }
    }
    return Status::success();
  }
  void release_actor_registration(Actor& actor, bool consumed = false) {
    if (!consumed && plan_.steps()[actor.index].traits.joint_contract == 2)
      return;
    auto found = actor_queries_.find(actor.key);
    if (found != actor_queries_.end())
      found->second.pending.reset();
  }
  Status retire(Actor& actor, const Status& incoming) {
    if (!callback_context() && detaching_)
      return incoming;
    auto failure = actor.service_failure->record(incoming);
    if (!failure.detail.node_id && !failure.detail.input_id)
      failure.detail.node_id = actor.node_id;
    if (failure.detail.scope == FailureScope::Unspecified)
      failure.detail.scope = FailureScope::Group;
    actor.service_failure->enrich(failure);
    actor.terminal = failure.code;
    if (failure.detail.origin != FailureOrigin::Domain) {
      actor.quality.reset();
    } else {
      for (const auto& entry : actor_queries_) {
        const auto& old = entry.second.failed;
        if (!old || !old->quality || old->failure.ok())
          continue;
        const auto cause = old->failure.fixed_status();
        const bool atom = cause.detail.atom && failure.detail.atom &&
                          *cause.detail.atom == *failure.detail.atom;
        const bool domain =
            cause.detail.domain && failure.detail.domain &&
            cause.detail.domain->first == failure.detail.domain->first &&
            cause.detail.domain->extent == failure.detail.domain->extent;
        if (cause.detail.node_id == failure.detail.node_id &&
            (atom || domain)) {
          actor.quality = old->quality;
          break;
        }
      }
    }
    auto own = actor_queries_.find(actor.key);
    if (own != actor_queries_.end() && own->second.failed &&
        failure.code != ErrorCode::Cancelled &&
        failure.code != ErrorCode::Stale) {
      own->second.failed->failure.record(failure);
      own->second.failed->quality = actor.quality;
    }
    if (plan_.steps()[actor.index].traits.joint_contract == 2 &&
        actor.observation.rank && failure.detail.node_id == actor.node_id &&
        (failure.detail.origin == FailureOrigin::Domain ||
         failure.detail.origin == FailureOrigin::Schema)) {
      auto& domain = validation_domains_.at(domain_key(actor));
      if (failure.detail.scope == FailureScope::ValidationDomain) {
        domain.failure.record(failure);
        domain.quality = actor.quality;
      } else if (failure.detail.scope == FailureScope::Atom) {
        domain.semantic_terminal = true;
      }
    }
    if (actor.joint)
      actor.joint->members[actor.joint_slot].done->store(true);
    actor.shared.fail(failure, actor.quality);
    if (actor.published.valid() &&
        (!actor.shared.valid() || actor.shared.producer()))
      actor.published.retire_producer(failure);
    actor.continuation = {};
    actor.waiting.reset();
    actor.results.clear();
    actor.tensors.clear();
    actor.input_facts.clear();
    actor.history.clear();
    actor.io.clear();
    release_actor_registration(actor);
    return failure;
  }
  void append_actor_scope(ResourceString& key, std::size_t index,
                          const std::optional<Footprint>& tensors,
                          std::uint32_t slot) const {
    content_internal::Sha256 demand_key;
    demand_key.text("structured-request.v3");
    demand_key.integer(slot);
    demand_key.integer(tensors ? 2U : 0U);
    if (!plan_.steps()[index].output_result_schema ||
        !shareable_closure_[index])
      demand_key.integer(index);
    if (tensors) {
      const auto& demand = *tensors;
      for (auto n : demand.shape())
        demand_key.integer(n);
      for (const auto& box : demand.boxes())
        for (const auto& axis : box.dimensions()) {
          demand_key.integer(axis.offset);
          demand_key.integer(axis.extent);
        }
    }
    auto suffix = demand_key.finish();
    key.push_back(':');
    key.append(suffix.data(), suffix.size());
  }
  Result<std::shared_ptr<Actor>> actor(
      std::size_t index, std::optional<Footprint> tensor_outputs = {},
      std::uint32_t tensor_slot = 0, bool defer_start = false) {
    using Answer = Result<std::shared_ptr<Actor>>;
    if (index >= plan_.steps().size())
      return Answer(protocol("invalid result step"));
    const auto& step = plan_.steps()[index];
    if (step.traits.outputs[0].dependency_version != 2)
      return Answer(protocol("structured continuation required"));
    if (step.traits.joint_contract == 2 && step.backend != Backend::Cpu)
      return Answer(Status{ErrorCode::BackendUnavailable,
                           "Result atom joints require the CPU lane"});
    if (!step.traits.side_effect_free && step.output_result_schema &&
        !step.output_result_schema->tensors.empty()) {
      if (tensor_slot >= step.output_result_schema->tensors.size())
        return Answer(protocol("invalid effect tensor slot"));
      tensor_slot = 0;
      auto full = Footprint::all(
          step.output_result_schema->tensors[tensor_slot].sample_shape(),
          set_limits());
      if (!full.ok())
        return Answer(full.status());
      tensor_outputs = full.take_value();
    }
    if (tensor_outputs &&
        step.traits.outputs[0].region_rule == OperationRegionRule::Whole &&
        step.traits.outputs[0].observation_kind !=
            ObservationKind::RequestRecord &&
        step.traits.outputs[0].failure_delivery !=
            FailureDelivery::PerAtomOutcome &&
        !tensor_outputs->empty()) {
      auto full = Footprint::all(tensor_outputs->shape(), set_limits());
      if (!full.ok())
        return Answer(full.status());
      tensor_outputs = full.take_value();
    }
    auto key_work = consume(templates_[index].size() + snapshot_.size() + 1);
    if (!key_work.ok())
      return Answer(key_work);
    ResourceString key{ResourceAllocator<char>(resources_)};
    key.reserve(templates_[index].size() + snapshot_.size() + 1);
    key.append(templates_[index].data(), templates_[index].size());
    key.push_back(':');
    key.append(snapshot_);
    append_actor_scope(key, index, tensor_outputs, tensor_slot);
    if (shareable_closure_[index]) {
      auto interested = call_.add_interest(key, resources_);
      if (!interested.ok())
        return Answer(interested);
      if (active_shared()) {
        auto linked = active_shared()->add_dependency(key);
        if (!linked.ok())
          return Answer(linked);
      }
    }
    auto existing = actor_queries_.find(key);
    if (existing != actor_queries_.end()) {
      if (existing->second.failed && !existing->second.failed->failure.ok())
        return Answer(existing->second.failed->failure.status());
      auto shared = existing->second.observed.lock();
      if (!shared) {
        actor_queries_.erase(existing);
      } else {
        if (std::find(shared->aliases.begin(), shared->aliases.end(), index) ==
            shared->aliases.end())
          shared->aliases.push_back(index);
        actor_aliases_[index] = shared;
        ++diagnostics_.shared_computations;
        return Answer(std::move(shared));
      }
    }
    const std::uint64_t bytes = sizeof(Actor);
    auto capacity = ResourceCapacity::host(bytes, bytes);
    capacity[ResourceKind::Entries] = 1;
    auto lease = resources_.reserve(capacity);
    if (!lease.ok())
      return Answer(lease.status());
    if (!step.structured_metadata)
      return Answer(protocol("missing compiled stage metadata"));
    auto created = std::shared_ptr<Actor>(new Actor(step, resources_));
    created->discovery_work_remaining = options_.dependencies.maximum_work;
    created->lease = lease.take_value();
    created->index = index;
    created->aliases.push_back(index);
    created->self = created;
    created->deferred_start = defer_start || result_joint_candidate(step);
    if (step.traits.joint_contract == 2 && step.output_result_schema &&
        step.output_result_schema->fields.empty() && tensor_outputs &&
        tensor_outputs->empty())
      created->deferred_start = false;
    created->query.snapshot_identity = snapshot_;
    created->query.tensor_outputs = std::move(tensor_outputs);
    created->query.tensor_slot = tensor_slot;
    created->query.tile_height = plan_.tile_height();
    created->query.tile_width = plan_.tile_width();
    created->query.backend = step.backend;
    created->query.output_index = step.output_index;
    created->query.resources = bindings_resources_;
    created->query.prepared = step.prepared;
    created->key = std::move(key);
    created->query.semantic_key = created->key;
    auto scope_work = consume(created->key.size() + 1);
    if (!scope_work.ok())
      return Answer(scope_work);
    content_internal::Sha256 dependency_scope;
    dependency_scope.text("photospider.result-dependency-query.v1");
    dependency_scope.text(created->key);
    created->dependency_scope.assign(dependency_scope.finish());
    created->query.page_bytes = options_.maximum_result_window_bytes;
    created->query.cancellation = active_token();
    if (result_joint_candidate(step) && joint_observation(*created)) {
      auto observation = result_atom_key(created->query);
      if (!observation.ok())
        return Answer(observation.status());
      created->observation = observation.take_value();
    }
    if (step.traits.joint_contract == 2 && created->observation.rank) {
      auto& domain = validation_domains_[domain_key(*created)];
      if (!domain.failure.ok()) {
        created->quality = domain.quality;
        return Answer(retire(*created, domain.failure.status()));
      }
    }
    created->registry_slot = actors_.size();
    actors_.push_back(created);
    std::shared_ptr<FailedObservation> failed;
    if (step.traits.joint_contract == 2)
      failed = std::allocate_shared<FailedObservation>(
          ResourceAllocator<FailedObservation>(resources_));
    actor_queries_.emplace(
        created->key, ActorRegistration{created, created, std::move(failed)});
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
    auto scalar_ready = prepare_scalars(index, *created);
    if (!scalar_ready.ok())
      return Answer(retire(*created, scalar_ready));
    if (shared_ && shareable_closure_[index] && step.output_result_schema) {
      auto joined = shared_->acquire(created->query.semantic_key, resources_,
                                     cancellation_, call_);
      if (!joined.ok())
        return Answer(joined.status());
      created->shared = joined.take_value();
      if (auto payload = created->shared.active_payload()) {
        std::lock_guard<std::recursive_mutex> lock(callback_metadata_mutex_);
        shared_payloads_.push_back(std::move(payload));
      }
      created->query.cancellation = created->shared.token();
      if (!created->shared.producer()) {
        created->initialized = true;
        actor_aliases_[index] = created;
        ++diagnostics_.shared_computations;
        return Answer(std::move(created));
      }
    }
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
    ActiveScope producer_scope(*this, created->shared);
    auto reused = reuse_cached_result(index, *created);
    if (!reused.ok())
      return retire(*created, reused.status());
    created->initialized = true;
    if (reused.value())
      return Status::success();
    auto charged = consume(1);
    if (!charged.ok())
      return retire(*created, charged);
    const auto& traits = plan_.steps()[index].traits;
    if (created->query.backend == Backend::Gpu && traits.supports_cpu &&
        traits.allows_cpu_fallback && traits.deterministic &&
        traits.side_effect_free) {
      records_->set_cancellation(active_token());
      auto saved = records_->fork_result_attempt();
      if (!saved.ok())
        return retire(*created, saved.status());
      created->attempt_records = saved.take_value();
    }
    if (created->deferred_start)
      return Status::success();
    auto submitted = start_actor(index, created);
    return submitted.ok() ? submitted : retire(*created, submitted);
  }
  Status complete_start(std::size_t index,
                        const std::shared_ptr<Actor>& current,
                        StartPhase started) {
    const auto& step = plan_.steps()[index];
    if (started.dispatched.ok() && !started.result.ok() &&
        plugin_internal::FailureLatch::retryable_backend_failure(
            started.result.status()) &&
        current->query.backend == Backend::Gpu && step.traits.supports_cpu &&
        step.traits.allows_cpu_fallback && step.traits.deterministic &&
        step.traits.side_effect_free && started.sticky == ErrorCode::Ok &&
        current->failure->load() == ErrorCode::Ok &&
        current->service_failure->snapshot().ok() &&
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
      current->attempt_records.reset();
      current->fallback_taint = true;
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
  Status retain_success(Actor& actor, std::uint32_t port,
                        ResultSupportTarget target, std::uint32_t slot,
                        std::uint32_t roles, const Footprint& samples) {
    if (!roles)
      return Status::success();
    auto work = consume(1 + samples.shape().size());
    if (!work.ok())
      return work;
    ResultNeedKey key{port, target, slot, roles};
    auto found = actor.history.find(key);
    if (found == actor.history.end()) {
      if (actor.history.size() >= options_.dependencies.sets.maximum_boxes)
        return Status{ErrorCode::ResourceExhausted, {}};
      actor.history.emplace(key, samples);
      return Status::success();
    }
    auto old = Result<Footprint>(found->second);
    auto next = Result<Footprint>(samples);
    if (target == ResultSupportTarget::Field &&
        found->second.shape() != samples.shape()) {
      const std::vector<std::uint64_t> shape{
          std::max(found->second.shape()[0], samples.shape()[0])};
      old = Footprint::from_regions(shape, found->second.boxes(), set_limits());
      next = Footprint::from_regions(shape, samples.boxes(), set_limits());
    }
    if (!old.ok() || !next.ok())
      return !old.ok() ? old.status() : next.status();
    auto merged = old.value().unite(next.value(), set_limits());
    if (!merged.ok())
      return merged.status();
    found->second = merged.take_value();
    return Status::success();
  }
  Status retain_object_success(Actor& actor, std::uint32_t port,
                               const ResultRef& object,
                               const ResultDescriptor& facts) {
    auto descriptor = Footprint::all({1}, set_limits());
    if (!descriptor.ok())
      return descriptor.status();
    auto status = retain_success(actor, port, ResultSupportTarget::Descriptor,
                                 0, 8, descriptor.value());
    if (!status.ok())
      return status;
    for (std::uint32_t field = 0; field < facts.field_count(); ++field) {
      auto samples = facts.rows(field)
                         ? Footprint::all({facts.rows(field)}, set_limits())
                         : Footprint::none({1}, set_limits());
      if (!samples.ok())
        return samples.status();
      status = retain_success(actor, port, ResultSupportTarget::Field, field, 7,
                              samples.value());
      if (!status.ok())
        return status;
    }
    for (std::uint32_t slot = 0; slot < object.schema().tensors.size();
         ++slot) {
      status = retain_success(actor, port, ResultSupportTarget::Tensor, slot, 7,
                              facts.tensor_coverage(slot));
      if (!status.ok())
        return status;
    }
    return Status::success();
  }
  Status checkpoint_allowed(std::size_t index, Actor& actor,
                            bool block = false) {
    auto work = consume(0);
    if (!work.ok())
      return work;
    const auto failure = actor.service_failure->snapshot();
    if (!failure.ok())
      return failure;
    if (actor.failure->load() != ErrorCode::Ok)
      return Status{actor.failure->load(), {}};
    const auto& traits = plan_.steps()[index].traits;
    if (!block &&
        traits.outputs[0].observation_kind == ObservationKind::RequestRecord)
      return protocol("terminal Result cannot use checkpoint services");
    return traits.deterministic && traits.side_effect_free
               ? Status::success()
               : protocol("checkpoint requires a pure Result program");
  }
  Result<ResourceString> checkpoint_key(std::size_t index, const Actor& actor) {
    auto charged =
        checkpoint_cache_work(templates_[index].size() + snapshot_.size() + 24);
    if (!charged.ok())
      return Result<ResourceString>(charged);
    ResourceString key{ResourceAllocator<char>(resources_)};
    key.reserve(templates_[index].size() + snapshot_.size() + 24);
    key.append(templates_[index].data(), templates_[index].size());
    key.push_back(':');
    key.append(snapshot_);
    key.append("/tensor-slot/");
    char digits[12];
    const auto converted =
        std::to_chars(digits, digits + sizeof(digits), actor.query.tensor_slot);
    key.append(digits, static_cast<std::size_t>(converted.ptr - digits));
    return Result<ResourceString>(std::move(key));
  }
  std::shared_ptr<ResultCheckpointScope> checkpoint_scope(
      const ResourceString& key) {
    auto found = checkpoint_scopes_.find(key);
    if (found != checkpoint_scopes_.end())
      return found->second;
    const auto maximum = options_.dependencies.sets.maximum_boxes;
    auto scope = checkpoints_
                     ? checkpoints_->acquire(key, resources_, maximum)
                     : std::allocate_shared<ResultCheckpointScope>(
                           ResourceAllocator<ResultCheckpointScope>(resources_),
                           resources_, maximum);
    if (scope)
      checkpoint_scopes_.emplace(key, scope);
    return scope;
  }
  Status checkpoint_cache_work(std::uint64_t units) {
    std::lock_guard<std::recursive_mutex> lock(callback_metadata_mutex_);
    const auto stopped = active_stop();
    if (stopped != ErrorCode::Ok)
      return Status{stopped, {}};
    if (units > cache_remaining_)
      return Status{ErrorCode::ResourceExhausted,
                    "Result checkpoint cache work exhausted",
                    FailureReason::WorkLimit,
                    {FailureOrigin::Resource, FailureScope::Run}};
    auto status = resources_.consume({units});
    if (!status.ok())
      return status;
    cache_remaining_ -= units;
    diagnostics_.dependency_cache_work += units;
    return Status::success();
  }
  Result<std::optional<ResultCheckpoint>> checkpoint_before(
      std::size_t index, Actor& actor, std::uint32_t phase,
      std::uint64_t before) {
    using Answer = Result<std::optional<ResultCheckpoint>>;
    std::unique_lock<std::recursive_mutex> lock(callback_metadata_mutex_);
    auto status = checkpoint_allowed(index, actor);
    if (!status.ok())
      return Answer(status);
    if (!phase)
      return Answer(protocol("zero checkpoint phase"));
    if (!cache_remaining_ || !checkpoint_shareable_[index] ||
        actor.fallback_taint)
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
      return Answer(protocol("Result checkpoint scope mismatch"));
    if (witness->weight > cache_remaining_)
      return Answer(std::optional<ResultCheckpoint>{});
    status = checkpoint_cache_work(witness->weight);
    if (!status.ok())
      return Answer(status);
    const auto& inputs = plan_.steps()[index].inputs;
    for (const auto& entry : witness->needs) {
      const auto& [port, target, slot, roles] = entry.first;
      if (port >= inputs.size())
        return Answer(protocol("invalid checkpoint input port"));
      status = records_->bind_domain(inputs[port], target, slot,
                                     entry.second.shape());
      if (!status.ok())
        return Answer(status);
      status = retain_success(actor, port, target, slot, roles, entry.second);
      if (!status.ok())
        return Answer(status);
      for (const auto& box : entry.second.boxes()) {
        auto scratch = resources_.reserve(ResourceCapacity::host(
            8 * sizeof(ResultMappedAxis), 8 * sizeof(ResultMappedAxis)));
        if (!scratch.ok())
          return Answer(scratch.status());
        std::vector<ResultMappedAxis> axes;
        axes.reserve(box.rank());
        for (const auto& dimension : box.dimensions())
          axes.push_back({-1, dimension.offset, 0, dimension.extent});
        auto relation = ResultRelation::mapped(
            resources_, {1}, Region::whole({1}), entry.second.shape(), axes,
            {port, roles, 0, 0, target, slot});
        if (!relation.ok())
          return Answer(relation.status());
        status = retain_obligation(actor, relation.take_value());
        if (!status.ok())
          return Answer(status);
      }
    }
    for (const auto& entry : witness->facts) {
      auto& revision = actor.input_facts[entry.first];
      revision = std::max(revision, entry.second);
    }
    for (const auto& [port, bundle] : witness->ancestry) {
      if (port >= inputs.size() || !bundle)
        return Answer(protocol("invalid checkpoint ancestry"));
      const auto* producer = std::get_if<PlanStepInput>(&inputs[port]);
      if (!producer)
        return Answer(protocol("checkpoint producer became a source"));
      status = records_->import_bundle(*bundle, producer->step_index);
      if (!status.ok())
        return Answer(status);
      status = retain_input_bundle(actor, producer->step_index, bundle, port);
      if (!status.ok())
        return Answer(status);
    }
    lock.unlock();
#if defined(PHOTOSPIDER_ENABLE_EXECUTION_TEST_HOOKS)
    execution_testing::notify_checkpoint_borrowed();
#endif
    return Answer(std::optional<ResultCheckpoint>{std::move(found)});
  }
  Status checkpoint_publish(std::size_t index, Actor& actor,
                            std::uint32_t phase, std::uint64_t sequence,
                            const ResultRef& state) {
    std::unique_lock<std::recursive_mutex> lock(callback_metadata_mutex_);
    auto status = checkpoint_allowed(index, actor);
    if (!status.ok())
      return status;
    if (!phase || state.request_record_ || !state.owned_by(resources_))
      return protocol("invalid Result checkpoint state");
    auto descriptor = state.descriptor();
    if (!descriptor.ok())
      return descriptor.status();
    if (!cache_remaining_ || !checkpoint_shareable_[index] ||
        actor.fallback_taint)
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
        if (units > maximum || weight > maximum - units)
          return false;
        weight += units;
        return true;
      };
      if (!add(actor.input_facts.size() * 3))
        return Status::success();
      const auto preflight = actor.history.size() + 1;
      status = checkpoint_cache_work(preflight);
      if (!status.ok())
        return status.code == ErrorCode::ResourceExhausted ? Status::success()
                                                           : status;
      ResourceVector<std::uint32_t> ports{
          ResourceAllocator<std::uint32_t>(resources_)};
      ports.reserve(
          std::min(actor.history.size(), plan_.steps()[index].inputs.size()));
      for (const auto& need : actor.history) {
        if (!add(1 + need.second.shape().size() +
                 need.second.boxes().size() *
                     (1 + 2 * need.second.shape().size())))
          return Status::success();
        const auto port = std::get<0>(need.first);
        if (ports.empty() || ports.back() != port)
          ports.push_back(port);
      }
      status = checkpoint_cache_work(weight - preflight);
      if (!status.ok())
        return status.code == ErrorCode::ResourceExhausted ? Status::success()
                                                           : status;
      auto witness = std::allocate_shared<ResultCheckpointWitness>(
          ResourceAllocator<ResultCheckpointWitness>(resources_), resources_);
      witness->scope = key.take_value();
      witness->needs = actor.history;
      witness->facts = actor.input_facts;
      std::set<const DependencyRecord*, std::less<const DependencyRecord*>,
               ResourceAllocator<const DependencyRecord*>>
          recorded;
      ResourceVector<const DependencyRecord*> pending;
      ResourceVector<const void*> relation_owners;
      for (auto port : ports) {
        const auto* producer =
            std::get_if<PlanStepInput>(&plan_.steps()[index].inputs[port]);
        if (!producer)
          continue;
        for (const auto& input : actor.input_bundles) {
          if (input.port != port)
            continue;
          if (!add(1 + input.second->roots.size()) ||
              !checkpoint_cache_work(1 + input.second->roots.size()).ok())
            return Status::success();
          for (const auto& root : input.second->roots)
            pending.push_back(root.get());
          while (!pending.empty()) {
            const auto* record = pending.back();
            pending.pop_back();
            if (!checkpoint_cache_work(1).ok())
              return Status::success();
            if (!recorded.insert(record).second)
              continue;
            const auto units =
                1 + record->identity.size() + record->scope.size() +
                record->input_queries.size() + record->samples.shape().size() +
                record->samples.boxes().size() *
                    (1 + 2 * record->samples.shape().size()) +
                record->routes.size() + record->upstream.size() +
                record->upstream_ports.size() + record->domains.size() +
                record->manifest.size();
            if (!add(units) || !checkpoint_cache_work(units).ok())
              return Status::success();
            for (const auto& query : record->input_queries)
              if (!add(query.identity.size()) ||
                  !checkpoint_cache_work(query.identity.size()).ok())
                return Status::success();
            for (const auto& domain : record->domains)
              if (!add(domain.shape.size()) ||
                  !checkpoint_cache_work(domain.shape.size()).ok())
                return Status::success();
            for (const auto& need : record->manifest) {
              const auto count = need.tags.size() +
                                 need.samples.shape().size() +
                                 need.samples.boxes().size() *
                                     (1 + 2 * need.samples.shape().size());
              if (!add(count) || !checkpoint_cache_work(count).ok())
                return Status::success();
            }
            if (record->certificate) {
              const auto count = record->certificate->storage_entries();
              if (!add(count) || !checkpoint_cache_work(count).ok())
                return Status::success();
            }
            if (record->request) {
              const auto count = 1 + record->request->identity.size() +
                                 record->request->manifest.size();
              if (!add(count) || !checkpoint_cache_work(count).ok())
                return Status::success();
              for (const auto& need : record->request->manifest) {
                const auto entries = need.tags.size() +
                                     need.samples.shape().size() +
                                     need.samples.boxes().size() *
                                         (1 + 2 * need.samples.shape().size());
                if (!add(entries) || !checkpoint_cache_work(entries).ok())
                  return Status::success();
              }
            }
            for (const auto& relation :
                 {record->relation, record->descriptor}) {
              auto size = relation.cache_metadata(
                  relation_owners, maximum - weight,
                  [&](std::uint64_t n) { return checkpoint_cache_work(n); });
              if (!size.ok() || !add(size.value()))
                return Status::success();
            }
            for (const auto& child : record->upstream)
              pending.push_back(child.get());
          }
          witness->ancestry.emplace_back(port, input.second);
        }
      }
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
    auto available = cache_remaining_;
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
    hash.text("photospider.result-shared-block-contract.v1");
    hash.integer(static_cast<std::uint32_t>(plan_.execution_mode()));
    hash.integer(static_cast<std::uint32_t>(phase.query.backend));
    hash.text(plan_.steps()[index].operation);
    contract_internal::append_traits(&hash, phase.query.prepared->traits());
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
      hash.metadata(input.descriptor, input.facets);
      hash.integer(input.atomic_trailing_axes);
      hash.integer(input.result_schema != nullptr);
      if (input.result_schema)
        hash.text(input.result_schema->canonical());
    }
    auto key = hash.finish();
    if (!admission.ok() && admission.code != ErrorCode::ResourceExhausted)
      return Result<std::string>(admission);
    if (key.empty())
      cache_remaining_ = 0;
    return Result<std::string>(std::move(key));
  }
  Result<ResultRef> evaluate_block(
      std::size_t index, Actor& actor, const ResultProgramPhase& phase,
      std::uint32_t kind, std::uint64_t begin, std::uint64_t end,
      std::uint64_t mode, const ResultRef& incoming,
      const std::function<Result<ResultRef>()>& compute) {
    std::unique_lock<std::recursive_mutex> lock(callback_metadata_mutex_);
    using Answer = Result<ResultRef>;
    auto status = checkpoint_allowed(index, actor, true);
    if (!status.ok())
      return Answer(status);
    status = consume(1);
    if (!status.ok())
      return Answer(status);
    if (!kind || begin >= end || !compute)
      return Answer(protocol("invalid Result block request"));
    if (incoming.request_record_)
      return Answer(protocol("terminal Result cannot be block state"));
    status = result_block_state(incoming, resources_);
    if (!status.ok())
      return Answer(status);
    std::string key;
    ResourceLease key_lease;
    Value cached;
    // Object fields and I/O remain mandatory coordinator access. Optional
    // content hashing never loads them or starts production inside a callback.
    if (blocks_ && checkpoint_shareable_[index] && !actor.fallback_taint &&
        plan_.steps()[index].traits.cacheable && cache_remaining_ &&
        actor.results.empty() && actor.io.empty() &&
        incoming.resources().size() == 0) {
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
            return Answer(identity.status());
          common = identity.take_value();
        }
        const auto& contract = shared ? common : templates_[index];
        std::uint64_t cost = 1 + contract.size();
        const auto add = [&](std::uint64_t n) {
          if (n > UINT64_MAX - cost)
            return false;
          cost += n;
          return true;
        };
        const auto set_cost = [&](const Footprint& samples,
                                  std::uint64_t lookup = 0) {
          status = checkpoint_cache_work(1 + samples.shape().size() +
                                         samples.boxes().size());
          if (!status.ok())
            return false;
          auto count = samples.element_count();
          const auto scale = 9 + samples.shape().size() + lookup;
          return count.ok() && count.value() <= UINT64_MAX / scale &&
                 add(count.value() * scale) &&
                 add(1 + samples.shape().size() +
                     samples.boxes().size() * (1 + 2 * samples.shape().size()));
        };
        const auto schema_cost = [&](const SchemaTemplate& schema) {
          status = checkpoint_cache_work(
              1 + schema.fields.size() + schema.tensors.size() +
              schema.domain.size() + schema.metadata.size());
          if (!status.ok())
            return false;
          for (const auto& tensor : schema.tensors) {
            status = checkpoint_cache_work(tensor.layout.groups.size() +
                                           tensor.facets.size());
            if (!status.ok())
              return false;
          }
          return add(schema.canonical_size());
        };
        auto incoming_facts = incoming.descriptor();
        status = checkpoint_cache_work(1 + actor.tensors.size());
        bool admitted = !contract.empty() && status.ok() &&
                        incoming_facts.ok() && schema_cost(incoming.schema()) &&
                        set_cost(incoming_facts.value().tensor_coverage(0));
        for (const auto& tensor : actor.tensors) {
          admitted =
              admitted && tensor.second.payload_authorized_ &&
              tensor.second.result_.resources().size() == 0 &&
              schema_cost(tensor.second.result_.schema()) &&
              set_cost(tensor.second.coverage(),
                       tensor.second.pieces_.size() *
                           (1 + tensor.second.coverage().shape().size()));
          for (const auto& piece : tensor.second.pieces_) {
            status = checkpoint_cache_work(1);
            admitted =
                admitted && status.ok() && piece.result.resources().size() == 0;
          }
        }
        if (admitted) {
          status = checkpoint_cache_work(cost);
          if (status.ok()) {
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
                    return incoming.read_tensor(incoming_facts.value(), 0, at,
                                                bytes, width, active_token());
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
                key_lease = capacity.take_value();
                key = "result-block/" + hash.finish();
                cached = blocks_->get(key, block_epoch_);
              }
              if (cached.valid())
                ++diagnostics_.block_cache_hits;
              else
                ++diagnostics_.block_cache_misses;
            }
          }
          if (!status.ok() && status.code != ErrorCode::ResourceExhausted)
            return Answer(status);
        }
      } catch (const std::bad_alloc&) {
        key.clear();
      }
    }
    if (cached.valid()) {
      if (actor.query.backend == Backend::Gpu)
        actor.native_block_reuse = true;
      const auto& spec = incoming.schema().tensors[0];
      if (cached.descriptor().element_type != spec.descriptor.element_type ||
          cached.descriptor().shape != spec.sample_shape())
        return Answer(protocol("cached Result block state mismatch"));
      // The copy contains no old source owners, associations or witness. The
      // current actor retains its current successful Needs independently.
      return unpack_block_state(cached, incoming.schema(), phase);
    }
    status = checkpoint_allowed(index, actor, true);
    if (!status.ok())
      return Answer(status);
    const auto native_before =
        active_services() && active_services()->gpu_dispatches
            ? active_services()->gpu_dispatches()
            : 0;
    lock.unlock();
    auto result = compute();
    lock.lock();
    if (!result.ok())
      return result;
    status = checkpoint_allowed(index, actor, true);
    if (!status.ok())
      return Answer(status);
    if (result.value().request_record_)
      return Answer(protocol("terminal Result cannot be block state"));
    status = result_block_state(result.value(), resources_);
    if (!status.ok() || !result.value().schema().same_schema(incoming.schema()))
      return Answer(status.ok()
                        ? protocol("computed Result block state mismatch")
                        : status);
    const bool native_computed =
        actor.query.backend != Backend::Gpu ||
        (active_services() && active_services()->gpu_dispatches &&
         active_services()->gpu_dispatches() > native_before);
    if (!key.empty() && native_computed &&
        result.value().resources().size() == 0) {
      ResourceAllocationScope optional_scope(resources_);
      try {
        auto count = result.value().schema().tensors[0].sample_count();
        if (count.ok() && count.value() <= UINT64_MAX / 9) {
          status = checkpoint_cache_work(count.value() * 9);
          if (status.ok()) {
            auto packed = pack_block_state(result.value(), phase);
            if (packed.ok())
              blocks_->put(key, packed.value(), block_epoch_,
                           phase.query.backend == Backend::Gpu);
            else if (packed.status().code != ErrorCode::ResourceExhausted)
              return Answer(packed.status());
          } else if (status.code != ErrorCode::ResourceExhausted) {
            return Answer(status);
          }
        }
      } catch (const std::bad_alloc&) {
      }
    }
    return result;
  }
  Status retain_input_bundle(Actor& actor, std::size_t producer,
                             std::shared_ptr<const DependencyBundle> bundle,
                             std::uint32_t port = UINT32_MAX,
                             std::uint64_t object = 0,
                             std::uint64_t revision = 0) {
    if (!bundle)
      return protocol("input Result has no dependency ancestry");
    auto charged = consume(1 + actor.input_bundles.size());
    if (!charged.ok())
      return charged;
    for (auto& prior : actor.input_bundles) {
      if (prior.first != producer || prior.port != port)
        continue;
      if (prior.second == bundle)
        return Status::success();
      if (object && prior.object_id == object) {
        if (revision >= prior.revision) {
          prior.second = std::move(bundle);
          prior.revision = revision;
        }
        return Status::success();
      }
    }
    if (actor.input_bundles.size() >= options_.dependencies.sets.maximum_boxes)
      return Status{ErrorCode::ResourceExhausted, {}};
    actor.input_bundles.emplace_back(producer, std::move(bundle), port, object,
                                     revision);
    return Status::success();
  }
  Status retain_input_bundle(Actor& actor, const PlanInput& input,
                             const ResultRef& object, std::uint32_t port) {
    const auto* producer = std::get_if<PlanStepInput>(&input);
    return producer ? retain_input_bundle(
                          actor, producer->step_index, object.dependencies(),
                          port, object.object_id(),
                          object.descriptor(false).value().revision())
                    : Status::success();
  }
  Status import_input_bundles(const Actor& actor) {
    records_->set_cancellation(active_token());
    for (const auto& input : actor.input_bundles) {
      auto imported =
          records_->import_bundle(*input.second, input.first,
                                  [&](std::uint64_t n) { return consume(n); });
      if (!imported.ok())
        return imported;
    }
    return Status::success();
  }
  Status prepare_scalars(std::size_t index, Actor& actor) {
    const auto& step = plan_.steps()[index];
    const auto& selected = step.traits.outputs[0].input_indices;
    ResourceVector<std::pair<uint32_t, uint32_t>> scalars{
        ResourceAllocator<std::pair<uint32_t, uint32_t>>(resources_)};
    // Validate the entire selected envelope before producing any scalar input.
    for (uint32_t port = 0; port < step.inputs.size(); ++port) {
      if (selected && std::find(selected->begin(), selected->end(), port) ==
                          selected->end())
        continue;
      if (const auto* source =
              std::get_if<PlanWorkflowInput>(&step.inputs[port]);
          source && bindings_[source->declaration_index].result.request_record_)
        return protocol("terminal Result cannot be an operation input");
      const auto& constraint = step.traits.input_schema[port];
      if (!constraint.scalar_bounds ||
          (actor.query.tensor_outputs && actor.query.tensor_outputs->empty() &&
           actor.query.output.result_schema->fields.empty() &&
           (step.traits.outputs[0].observation_kind ==
                ObservationKind::RequestRecord ||
            step.traits.joint_contract == 2)))
        continue;
      auto slot = input_internal::resolve_tensor_member(
          constraint, actor.query.inputs[port]);
      if (!slot.ok())
        return input_failure(step.inputs[port], slot.status());
      scalars.emplace_back(port, slot.value());
    }
    ResourceVector<ResultRelation> obligations{
        ResourceAllocator<ResultRelation>(resources_)};
    for (const auto& scalar : scalars) {
      const auto port = scalar.first, slot = scalar.second;
      auto samples = Footprint::all({1}, set_limits());
      if (!samples.ok())
        return samples.status();
      auto ready =
          object_input(step.inputs[port], {}, samples.value(), slot, &actor);
      if (!ready.ok())
        return input_failure(step.inputs[port], ready.status());
      auto ancestry =
          retain_input_bundle(actor, step.inputs[port], ready.value(), port);
      if (!ancestry.ok())
        return ancestry;
      note_cached_input_backend(actor, step.inputs[port], &ready.value());
      auto facts = ready.value().descriptor();
      if (!facts.ok())
        return input_failure(step.inputs[port], facts.status());
      const auto numeric =
          std::holds_alternative<PlanWorkflowInput>(step.inputs[port])
              ? ErrorCode::InvalidArgument
              : ErrorCode::OperationFailed;
      auto checked = input_internal::validate_port_tensor(
          step.traits.input_schema[port], ready.value(), facts.value(),
          actor.query.inputs[port], numeric, active_token(),
          [&] { return active_stop(); });
      if (!checked.ok())
        return input_failure(step.inputs[port], checked);
      ResultTensorInput capability;
      capability.result_ = ready.value();
      capability.descriptor_ = facts.take_value();
      capability.slot_ = slot;
      capability.resources_ = resources_;
      capability.payload_authorized_ = true;
      capability.samples_ = samples.take_value();
      capability.failure_ = actor.failure;
      capability.observer_ = actor.service_failure;
      auto& revision = actor.input_facts[{port, ready.value().object_id()}];
      revision = std::max(revision, capability.descriptor_.revision());
      auto retained = retain_success(actor, port, ResultSupportTarget::Tensor,
                                     slot, 5, capability.samples_);
      if (!retained.ok())
        return retained;
      auto metadata = Footprint::all({1}, set_limits());
      if (!metadata.ok())
        return metadata.status();
      retained = retain_success(actor, port, ResultSupportTarget::Descriptor, 0,
                                8, metadata.value());
      if (!retained.ok())
        return retained;
      actor.tensors.emplace(std::make_pair(port, slot), std::move(capability));
      for (const auto target :
           {ResultSupportTarget::Tensor, ResultSupportTarget::Descriptor}) {
        auto witness = ResultRelation::cartesian(
            resources_, 1,
            {port, target == ResultSupportTarget::Tensor ? 4U : 8U, 0, 1,
             target, target == ResultSupportTarget::Tensor ? slot : 0U});
        if (!witness.ok())
          return witness.status();
        obligations.push_back(witness.take_value());
      }
    }
    // A balanced expression keeps the witness depth bounded for large
    // signatures.
    while (obligations.size() > 1) {
      ResourceVector<ResultRelation> next{
          ResourceAllocator<ResultRelation>(resources_)};
      next.reserve((obligations.size() + 1) / 2);
      for (size_t i = 0; i < obligations.size(); i += 2) {
        if (i + 1 == obligations.size()) {
          next.push_back(obligations[i]);
          continue;
        }
        auto joined = ResultRelation::unite(
            resources_, {obligations[i], obligations[i + 1]});
        if (!joined.ok())
          return joined.status();
        next.push_back(joined.take_value());
      }
      obligations = std::move(next);
    }
    if (!obligations.empty())
      actor.input_obligations = obligations.front();
    return Status::success();
  }
  Status retain_obligation(Actor& actor, ResultRelation relation) {
    if (!actor.input_obligations.valid()) {
      actor.input_obligations = std::move(relation);
      return Status::success();
    }
    auto merged =
        ResultRelation::unite(resources_, {actor.input_obligations, relation});
    if (!merged.ok())
      return merged.status();
    actor.input_obligations = merged.take_value();
    return Status::success();
  }
  Status retain_tensor_need(Actor& actor, const ResultTensorNeed& need,
                            const Footprint& samples, bool whole) {
    if (need.roles & 8U) {
      auto descriptor = ResultRelation::cartesian(
          resources_, 1,
          {need.input, 8, 0, 1, ResultSupportTarget::Descriptor, 0});
      if (!descriptor.ok())
        return descriptor.status();
      auto retained = retain_obligation(actor, descriptor.take_value());
      if (!retained.ok())
        return retained;
    }
    // Whole validation is one prerequisite of the complete result. Regional
    // programs publish their registered per-observation Validation mapping;
    // widening it to the whole Need would erase exact regional dirty support.
    if ((need.roles & 4U) && whole) {
      for (const auto& box : samples.boxes()) {
        std::vector<ResultMappedAxis> axes;
        auto scratch = resources_.reserve(ResourceCapacity::host(
            8 * sizeof(ResultMappedAxis), 8 * sizeof(ResultMappedAxis)));
        if (!scratch.ok())
          return scratch.status();
        axes.reserve(8);
        for (auto d : box.dimensions())
          axes.push_back({-1, d.offset, 0, d.extent});
        auto relation = ResultRelation::mapped(
            resources_, {1}, Region::whole({1}), samples.shape(), axes,
            {need.input, 4, 0, 0, ResultSupportTarget::Tensor, need.slot});
        if (!relation.ok())
          return relation.status();
        auto retained = retain_obligation(actor, relation.take_value());
        if (!retained.ok())
          return retained;
      }
    }
    return Status::success();
  }
  ResourceVector<DependencyInputQuery> input_queries(const Actor& actor) {
    ResourceVector<DependencyInputQuery> inputs{
        ResourceAllocator<DependencyInputQuery>(resources_)};
    for (const auto& input : actor.input_bundles) {
      if (input.port == UINT32_MAX)
        continue;
      for (const auto& root : input.second->roots) {
        if (root->scope.empty())
          continue;
        if (std::none_of(inputs.begin(), inputs.end(), [&](const auto& old) {
              return old.port == input.port && old.identity == root->scope &&
                     old.kind == root->kind && old.slot == root->slot;
            }))
          inputs.push_back({input.port, root->scope, root->kind, root->slot});
      }
    }
    return inputs;
  }
  Status record_object(std::size_t index, const ResultRef& object,
                       const Actor* producer = nullptr) {
    records_->set_cancellation(active_token());
    auto facts = object.descriptor(false);
    if (!facts.ok())
      return facts.status();
    auto descriptor = object.descriptor_relation();
    if (!descriptor.ok())
      return descriptor.status();
    if (!producer) {
      for (const auto& weak : actors_)
        if (auto candidate = weak.lock();
            candidate && candidate->published.valid() &&
            candidate->published.object_id() == object.object_id() &&
            std::find(candidate->aliases.begin(), candidate->aliases.end(),
                      index) != candidate->aliases.end()) {
          producer = candidate.get();
          break;
        }
    }
    if (producer && producer->input_obligations.valid()) {
      auto joined = ResultRelation::unite(
          resources_, {descriptor.value(), producer->input_obligations});
      if (!joined.ok())
        return joined.status();
      descriptor = std::move(joined);
    }
    if (auto bundle = object.dependencies();
        bundle && (!producer ||
                   (producer->published.object_id() == object.object_id() &&
                    producer->published_revision >= facts.value().revision())))
      return records_->import_bundle(*bundle, index);
    auto inputs = producer ? input_queries(*producer)
                           : ResourceVector<DependencyInputQuery>{};
    return records_->append_result(
        index, object, descriptor.take_value(),
        producer ? std::string_view(producer->dependency_scope)
                 : object.semantic_key(),
        inputs);
  }

  bool satisfied(const Actor& actor, const ResultObjectNeed& request) const {
    if (!actor.published.valid())
      return false;
    if (request.complete)
      return actor.complete;
    auto facts = actor.published.descriptor(false);
    return facts.ok() && request.field < facts.value().field_count() &&
           (facts.value().sealed() ||
            facts.value().rows(request.field) >= request.minimum_rows);
  }
  Result<std::optional<ResultRef>> result_object_step(
      std::size_t index, const std::shared_ptr<Actor>& current,
      const ResultObjectNeed& request) {
    using Answer = Result<std::optional<ResultRef>>;
    WorkMode work(work_mode(), false);
    if (plan_.steps()[index].traits.joint_contract == 2 &&
        current->observation.rank) {
      const auto& domain = validation_domains_.at(domain_key(*current));
      if (!domain.failure.ok() && current->terminal == ErrorCode::Ok) {
        current->quality = domain.quality;
        return Answer(retire(*current, domain.failure.status()));
      }
    }
    if (current->shared.valid() && !current->shared.producer()) {
      auto ready = current->shared.poll(request.complete, request.field,
                                        request.minimum_rows, active_token());
      current->quality = current->shared.quality();
      if (!ready.ok())
        return Answer(retire(*current, ready.status()));
      if (!ready.value()) {
        if (shared_ && !callback_)
          shared_->pump();
        auto pumped = service_peers();
        return pumped.ok() ? Answer(std::optional<ResultRef>{})
                           : Answer(pumped);
      }
      auto object = std::move(*ready.value());
      auto bundle = object.dependencies();
      if (!bundle)
        return Answer(
            protocol("shared Result has no captured dependency ancestry"));
      records_->set_cancellation(active_token());
      auto bound = records_->bind_result(PlanStepInput{index}, object);
      if (!bound.ok())
        return Answer(bound);
      auto imported = records_->import_bundle(*bundle, index);
      if (!imported.ok())
        return Answer(imported);
      current->query.backend = current->shared.backend();
      current->fallback_taint = current->shared.fallback_taint();
      diagnostics_.selected_backends[plan_.steps()[index].result_ref()] =
          current->query.backend;
      current->published = object;
      auto facts = current->published.descriptor(false);
      if (!facts.ok())
        return Answer(retire(*current, facts.status()));
      current->published_revision = facts.value().revision();
      current->complete = facts.value().sealed();
      if (current->complete &&
          plan_.steps()[index].traits.joint_contract == 2 &&
          current->observation.rank)
        validation_domains_.at(domain_key(*current)).semantic_terminal = true;
      if (current->complete)
        release_actor_registration(*current);
      auto notified = notify(*current);
      release_actor_registration(*current, true);
      return notified.ok() ? Answer(std::optional<ResultRef>(std::move(object)))
                           : Answer(notified);
    }
    if (recursively_busy(*current))
      return Answer(Status{ErrorCode::Cycle, {}});
    if (!satisfied(*current, request)) {
      if (current->terminal != ErrorCode::Ok &&
          (!current->joint || current->joint->complete())) {
        release_actor_registration(*current, true);
        return Answer(current->service_failure->snapshot());
      }
      if (current->complete)
        return Answer(protocol("requested result field is absent"));
      auto status = advance(index, *current);
      if (!status.ok())
        return Answer(status);
      if (current->terminal != ErrorCode::Ok &&
          (!current->joint || current->joint->complete())) {
        release_actor_registration(*current, true);
        return Answer(current->service_failure->snapshot());
      }
      if (!satisfied(*current, request))
        return Answer(std::optional<ResultRef>{});
    }
    if (rollback_possible_ ||
        &current->query.inputs !=
            &plan_.steps()[index].structured_metadata->inputs) {
      auto bundle = current->published.dependencies();
      if (!bundle)
        return Answer(protocol("completed Result has no ancestry"));
      records_->set_cancellation(active_token());
      auto imported = records_->import_bundle(*bundle, index);
      if (!imported.ok())
        return Answer(imported);
    }
    auto notified = notify(*current);
    if (current->complete)
      release_actor_registration(*current, true);
    return notified.ok() ? Answer(std::optional<ResultRef>(current->published))
                         : Answer(notified);
  }
  bool wants_effects(const DemandQuery* requested) const {
    return !requested ||
           std::any_of(requested->begin(), requested->end(),
                       [](const auto& named) { return !named.second.empty(); });
  }
  Status run_effect_roots(const DemandQuery* requested) {
    if (!wants_effects(requested))
      return Status::success();
    for (std::size_t i = 0; i < plan_.steps().size(); ++i) {
      if (plan_.steps()[i].traits.side_effect_free)
        continue;
      auto effect = result_object(i, ResultObjectNeed{});
      if (!effect.ok())
        return effect.status();
    }
    return Status::success();
  }
  Result<ResultRef> result_object(std::size_t index,
                                  const ResultObjectNeed& request,
                                  std::optional<Footprint> tensor_outputs = {},
                                  std::uint32_t tensor_slot = 0,
                                  Actor* consumer = nullptr) {
    WorkMode work(work_mode(), false);
    auto acquired = actor(index, std::move(tensor_outputs), tensor_slot);
    if (!acquired.ok())
      return Result<ResultRef>(acquired.status());
    auto current = acquired.take_value();
    for (;;) {
      const auto before = progress_;
      ++traversal_;
      auto ready = result_object_step(index, current, request);
      if (!ready.ok())
        return Result<ResultRef>(ready.status());
      if (ready.value()) {
        if (consumer)
          consumer->fallback_taint |=
              current->fallback_taint ||
              current->query.backend != plan_.steps()[index].backend;
        return Result<ResultRef>(std::move(*ready.value()));
      }
      if (!callback_context()) {
        for (const auto& root : requested_roots_) {
          if (root == current || root->complete ||
              root->terminal != ErrorCode::Ok || !root->initialized ||
              recursively_busy(*root) ||
              (root->deferred_start &&
               result_joint_candidate(plan_.steps()[root->index])))
            continue;
          auto peer = result_object_step(root->index, root, {});
          if (!peer.ok() && detaching_)
            return Result<ResultRef>(peer.status());
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
                                 const ResultObjectNeed& need,
                                 std::optional<Footprint> samples,
                                 std::uint32_t slot = 0,
                                 Actor* consumer = nullptr) {
    if (const auto* source = std::get_if<PlanWorkflowInput>(&input)) {
      const auto& object = bindings_.at(source->declaration_index).result;
      if (object.request_record_)
        return Result<ResultRef>(
            protocol("terminal Result cannot be an operation input"));
      if (!object.owned_by(resources_))
        return Result<ResultRef>(
            protocol("Result binding belongs to a different resource root"));
      return Result<ResultRef>(object);
    }
    auto ready = result_object(std::get<PlanStepInput>(input).step_index, need,
                               std::move(samples), slot, consumer);
    if (!ready.ok())
      return ready;
    auto bound = records_->bind_result(input, ready.value());
    return bound.ok() ? ready : Result<ResultRef>(bound);
  }
  Result<std::optional<ResultRef>> need_object_input(
      Actor& consumer, NeedCursor& cursor, const PlanInput& input,
      const ResultObjectNeed& need, std::optional<Footprint> samples,
      std::uint32_t slot, bool replay, std::uint32_t port) {
    using Answer = Result<std::optional<ResultRef>>;
    if (replay || std::holds_alternative<PlanWorkflowInput>(input)) {
      auto ready =
          object_input(input, need, std::move(samples), slot, &consumer);
      if (!ready.ok())
        return Answer(ready.status());
      auto retained = retain_input_bundle(consumer, input, ready.value(), port);
      return retained.ok()
                 ? Answer(std::optional<ResultRef>(ready.take_value()))
                 : Answer(retained);
    }
    const auto index = std::get<PlanStepInput>(input).step_index;
    if (!cursor.producer) {
      auto acquired = actor(index, std::move(samples), slot);
      if (!acquired.ok())
        return Answer(acquired.status());
      cursor.producer = acquired.take_value();
    }
    if (!consumer.waiting)
      consumer.waiting = cursor.producer;
    auto ready = result_object_step(index, cursor.producer, need);
    if (!ready.ok() || !ready.value())
      return ready;
    auto bound = records_->bind_result(input, *ready.value());
    if (!bound.ok())
      return Answer(bound);
    auto retained = retain_input_bundle(consumer, input, *ready.value(), port);
    if (!retained.ok())
      return Answer(retained);
    consumer.fallback_taint |=
        cursor.producer->fallback_taint ||
        cursor.producer->query.backend != plan_.steps()[index].backend;
    consumer.waiting.reset();
    cursor.producer.reset();
    return ready;
  }
  Result<std::optional<ResultTensorInput>> need_tensor_input(
      Actor& consumer, NeedCursor& cursor, const PlanInput& input,
      const Footprint& samples, std::uint32_t slot, bool replay,
      std::uint32_t port) {
    using Answer = Result<std::optional<ResultTensorInput>>;
    const auto* producer = std::get_if<PlanStepInput>(&input);
    const bool atoms =
        producer &&
        plan_.steps()[producer->step_index].traits.joint_contract == 2;
    // A field-free Empty query already has a sealed metadata-only Result.
    // Use that ordinary path for Empty grants and Descriptor-only Needs;
    // neither one names an atom or grants a payload sample.
    const bool metadata_only = atoms && samples.empty() &&
                               plan_.steps()[producer->step_index]
                                   .output_result_schema->fields.empty();
    if (!atoms || metadata_only) {
      auto ready = need_object_input(consumer, cursor, input, {}, samples, slot,
                                     replay, port);
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
        return Answer(protocol("tensor Need is outside published coverage"));
      ResultTensorInput grant;
      grant.result_ = std::move(*ready.value());
      grant.descriptor_ = descriptor.take_value();
      grant.slot_ = slot;
      grant.samples_ = samples;
      grant.resources_ = resources_;
      return Answer(std::optional<ResultTensorInput>{std::move(grant)});
    }
    const auto index = producer->step_index;
    if (!cursor.tensor_atoms_initialized) {
      const auto& spec =
          plan_.steps()[index].output_result_schema->tensors.at(slot);
      const auto shape = spec.sample_shape();
      const auto tuple =
          input_internal::tuple_channel_axis(spec.descriptor, spec.facets);
      std::array<bool, 8> grouped{};
      std::vector<std::uint64_t> domain;
      for (std::size_t axis = 0; axis < shape.size(); ++axis) {
        grouped[axis] = axis >= shape.size() - spec.atomic_trailing_axes ||
                        (tuple && axis == *tuple + spec.batch_axes.size());
        if (!grouped[axis])
          domain.push_back(shape[axis]);
      }
      if (domain.empty())
        domain.push_back(1);
      std::vector<Region> boxes;
      for (const auto& box : samples.boxes()) {
        std::vector<RegionDimension> dimensions;
        for (std::size_t axis = 0; axis < shape.size(); ++axis)
          if (!grouped[axis])
            dimensions.push_back(box.dimensions()[axis]);
        if (dimensions.empty())
          dimensions.push_back({0, 1});
        boxes.emplace_back(std::move(dimensions));
      }
      auto observations = Footprint::from_regions(domain, boxes, set_limits());
      if (!observations.ok())
        return Answer(observations.status());
      auto count = observations.value().element_count();
      if (!count.ok() || !count.value() ||
          count.value() > std::min<std::uint64_t>(
                              65536, options_.dependencies.sets.maximum_boxes))
        return Answer(
            count.ok() && !count.value()
                ? protocol("empty Result atom tensor Need has no source object")
                : Status{ErrorCode::ResourceExhausted,
                         "tensor Need atom count limit"});
      cursor.tensor_atoms.reserve(count.value());
      cursor.tensor_producers.reserve(count.value());
      cursor.tensor_pieces.reserve(count.value());
      auto visited = observations.value().visit(
          [&](const auto& coordinate) {
            auto charged = consume(1);
            if (!charged.ok())
              return charged;
            std::vector<RegionDimension> dimensions;
            std::size_t at = 0;
            for (std::size_t axis = 0; axis < shape.size(); ++axis)
              dimensions.push_back(grouped[axis]
                                       ? RegionDimension{0, shape[axis]}
                                       : RegionDimension{coordinate[at++], 1});
            auto query = Footprint::from_regions(shape, {Region(dimensions)},
                                                 set_limits());
            if (!query.ok())
              return query.status();
            auto source = actor(index, query.value(), slot, true);
            if (!source.ok())
              return source.status();
            cursor.tensor_atoms.push_back(query.take_value());
            cursor.tensor_producers.push_back(source.take_value());
            return Status::success();
          },
          count.value(), active_token());
      if (!visited.ok())
        return Answer(visited);
      cursor.tensor_atoms_initialized = true;
    }
    if (cursor.tensor_atom_next < cursor.tensor_producers.size()) {
      auto& source = cursor.tensor_producers[cursor.tensor_atom_next];
      if (!consumer.waiting)
        consumer.waiting = source;
      auto ready = result_object_step(index, source, {});
      if (!ready.ok())
        return Answer(ready.status());
      if (!ready.value())
        return Answer(std::optional<ResultTensorInput>{});
      auto bound = records_->bind_result(input, *ready.value());
      if (!bound.ok())
        return Answer(bound);
      auto retained =
          retain_input_bundle(consumer, input, *ready.value(), port);
      if (!retained.ok())
        return Answer(retained);
      auto descriptor = ready.value()->descriptor();
      if (!descriptor.ok())
        return Answer(descriptor.status());
      consumer.fallback_taint |=
          source->fallback_taint ||
          source->query.backend != plan_.steps()[index].backend;
      cursor.tensor_pieces.push_back(
          {std::move(*ready.value()), descriptor.take_value(),
           cursor.tensor_atoms[cursor.tensor_atom_next]});
      source.reset();
      consumer.waiting.reset();
      ++cursor.tensor_atom_next;
      if (cursor.tensor_atom_next < cursor.tensor_producers.size())
        return Answer(std::optional<ResultTensorInput>{});
    }
    ResultTensorInput grant;
    grant.result_ = cursor.tensor_pieces.front().result;
    grant.descriptor_ = cursor.tensor_pieces.front().descriptor;
    grant.slot_ = slot;
    grant.samples_ = samples;
    grant.resources_ = resources_;
    if (cursor.tensor_pieces.size() > 1)
      grant.pieces_ = std::move(cursor.tensor_pieces);
    cursor.tensor_atoms.clear();
    cursor.tensor_producers.clear();
    cursor.tensor_pieces.clear();
    cursor.tensor_atoms_initialized = false;
    cursor.tensor_atom_next = 0;
    return Answer(std::optional<ResultTensorInput>{std::move(grant)});
  }
  Status service_peers() {
    if (service_depth() >= 64)
      return Status{ErrorCode::ResourceExhausted, "shared service depth"};
    ++service_depth();
    struct Leave {
      unsigned& depth;
      ~Leave() { --depth; }
    } leave{service_depth()};
    refresh_shared();
    for (std::size_t position = actors_.size(); position > 0; --position) {
      auto current = actors_[position - 1].lock();
      if (!current || (current->joint && current->joint == active_joint()) ||
          recursively_busy(*current) || current->complete ||
          current->terminal != ErrorCode::Ok ||
          (current->joint && current->joint->driving.load()) ||
          !current->shared.valid() || !current->shared.producer() ||
          !current->shared.has_other_waiters())
        continue;
      if (current->deferred_start) {
        auto registering = c1_registering(*current);
        if (!registering.ok())
          return registering.status();
        if (registering.value())
          continue;
      }
      auto status = advance(current->index, *current);
      if (callback_context())
        while (status.ok() && (current->pending || current->waiting))
          status = advance(current->index, *current);
      if (!status.ok() && current->terminal == ErrorCode::Ok)
        return status;
    }
    return Status::success();
  }
  Result<ResultIoReply> io(const ResultIoRequest& request) {
    using Answer = Result<ResultIoReply>;
    auto charged = consume(1);
    if (!charged.ok())
      return Answer(charged);
    if (const auto* read = std::get_if<ResultReadPlan>(&request)) {
      auto ready =
          read->load(options_.maximum_result_window_bytes, active_token());
      return ready.ok() ? Answer(ResultIoReply{ready.take_value()})
                        : Answer(ready.status());
    }
    if (const auto* write = std::get_if<ResultWritePlan>(&request)) {
      auto applied = write->apply(active_token());
      return applied.ok() ? Answer(ResultIoReply{std::monostate{}})
                          : Answer(applied);
    }
    if (std::holds_alternative<ResultCreateTemporary>(request)) {
      auto file = TemporaryStorage::create(resources_);
      return file.ok() ? Answer(ResultIoReply{file.take_value()})
                       : Answer(file.status());
    }
    if (const auto* read = std::get_if<ResultReadTemporary>(&request)) {
      auto ready = read->storage.read(read->offset, read->bytes,
                                      options_.maximum_result_window_bytes,
                                      active_token());
      return ready.ok() ? Answer(ResultIoReply{ready.take_value()})
                        : Answer(ready.status());
    }
    if (const auto* write = std::get_if<ResultWriteTemporary>(&request)) {
      if (!write->bytes)
        return Answer(protocol("missing temporary write payload"));
      auto storage = write->storage;
      auto retained = resources_.reference(write->bytes);
      if (!retained.ok())
        return Answer(retained.status());
      auto applied = storage.write(write->offset, retained.value()->bytes(),
                                   active_token());
      return applied.ok() ? Answer(ResultIoReply{std::monostate{}})
                          : Answer(applied);
    }
    const auto& extend = std::get<ResultExtendTemporary>(request);
    auto storage = extend.storage;
    auto applied = storage.append_zeroed(extend.bytes, active_token());
    return applied.ok() ? Answer(ResultIoReply{applied.value()})
                        : Answer(applied.status());
  }
  Status notify(Actor& actor) {
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
    if (!delivered.ok())
      call_.retire_user();
    if (!delivered.ok() &&
        ((actor.shared.valid() && actor.shared.producer() &&
          actor.shared.has_other_waiters()) ||
         (active_shared() && active_shared()->has_other_waiters())))
      return Status::success();
    return delivered;
  }
  void note_cached_input_backend(Actor& actor, const PlanInput& input,
                                 const ResultRef* object = nullptr) {
    if (const auto* producer = std::get_if<PlanStepInput>(&input)) {
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
      if (source && (source->fallback_taint ||
                     source->query.backend !=
                         plan_.steps()[producer->step_index].backend))
        actor.fallback_taint = true;
    }
  }
  Status cache_facts(content_internal::Sha256& hash, const ResultRef& object,
                     const ResultDescriptor& facts) {
    auto charged =
        checkpoint_cache_work(1 + object.schema().id.size() +
                              facts.field_count() + facts.tensor_count());
    if (!charged.ok())
      return charged;
    for (std::uint32_t slot = 0; slot < facts.tensor_count(); ++slot) {
      const auto& coverage = facts.tensor_coverage(slot);
      charged = checkpoint_cache_work(coverage.shape().size() +
                                      coverage.boxes().size() *
                                          (1 + 2 * coverage.shape().size()));
      if (!charged.ok())
        return charged;
    }
    hash.integer(facts.sealed());
    hash.integer(facts.field_count());
    for (std::uint32_t field = 0; field < facts.field_count(); ++field)
      hash.integer(facts.rows(field));
    hash.integer(facts.tensor_count());
    for (std::uint32_t slot = 0; slot < facts.tensor_count(); ++slot)
      append_block_footprint(&hash, facts.tensor_coverage(slot));
    hash.text(object.schema().id);
    hash.integer(object.schema().version);
    return Status::success();
  }
  Result<ResourceString> supplied_cache_facts(const Actor& actor) {
    content_internal::Sha256 hash;
    hash.text("photospider.result-cache-direct-facts.v2");
    for (const auto& input : actor.results) {
      auto status = checkpoint_cache_work(1);
      if (!status.ok())
        return Result<ResourceString>(status);
      auto facts = input.second.descriptor(false);
      if (!facts.ok())
        return Result<ResourceString>(facts.status());
      hash.integer(input.first);
      status = cache_facts(hash, input.second, facts.value());
      if (!status.ok())
        return Result<ResourceString>(status);
    }
    for (const auto& input : actor.tensors) {
      auto status =
          checkpoint_cache_work(1 + input.second.samples_.boxes().size());
      if (!status.ok())
        return Result<ResourceString>(status);
      hash.integer(input.first.first);
      hash.integer(input.first.second);
      hash.integer(input.second.pieces_.size());
      if (input.second.pieces_.empty()) {
        status =
            cache_facts(hash, input.second.result_, input.second.descriptor_);
        if (!status.ok())
          return Result<ResourceString>(status);
      } else {
        for (const auto& piece : input.second.pieces_) {
          status = cache_facts(hash, piece.result, piece.descriptor);
          if (!status.ok())
            return Result<ResourceString>(status);
          status = checkpoint_cache_work(
              1 + piece.samples.boxes().size() *
                      (1 + 2 * piece.samples.shape().size()));
          if (!status.ok())
            return Result<ResourceString>(status);
          append_block_footprint(&hash, piece.samples);
        }
      }
      append_block_footprint(&hash, input.second.samples_);
    }
    return Result<ResourceString>(
        ResourceString(hash.finish(), ResourceAllocator<char>(resources_)));
  }
  Result<ResourceString> source_cache_digest(
      const ResourceVector<SourceObservation>& sources) {
    content_internal::Sha256 hash;
    hash.text("photospider.result-cache-sources.v1");
    auto initial = checkpoint_cache_work(1 + sources.size());
    if (!initial.ok())
      return Result<ResourceString>(initial);
    for (const auto& source : sources) {
      auto status =
          checkpoint_cache_work(bindings_.size() + source.input.size() +
                                source.samples.boxes().size());
      if (!status.ok())
        return Result<ResourceString>(status);
      auto found = std::find_if(bindings_.begin(), bindings_.end(),
                                [&](const auto& binding) {
                                  return std::string_view(binding.name) ==
                                         std::string_view(source.input);
                                });
      if (found == bindings_.end())
        return Result<ResourceString>(Status{ErrorCode::NotFound, {}});
      hash.text(source.input);
      hash.integer(static_cast<std::uint32_t>(source.target));
      hash.integer(source.slot);
      hash.integer(source.roles);
      append_block_footprint(&hash, source.samples);
      if (!found->result.valid())
        return Result<ResourceString>(Status{ErrorCode::NotFound, {}});
      auto descriptor = found->result.descriptor(false);
      if (!descriptor.ok())
        return Result<ResourceString>(descriptor.status());
      if (source.target == ResultSupportTarget::Descriptor) {
        auto charged =
            checkpoint_cache_work(found->result.schema().canonical_size() + 1 +
                                  descriptor.value().field_count() +
                                  descriptor.value().tensor_count());
        if (!charged.ok())
          return Result<ResourceString>(charged);
        hash.text(found->result.schema().canonical());
        charged = cache_facts(hash, found->result, descriptor.value());
        if (!charged.ok())
          return Result<ResourceString>(charged);
        continue;
      }
      auto count = source.samples.element_count();
      if (!count.ok())
        return Result<ResourceString>(count.status());
      if (source.target == ResultSupportTarget::Field) {
        auto width = found->result.schema().row_bytes(source.slot);
        if (!width.ok() ||
            width.value() > options_.maximum_result_window_bytes ||
            (width.value() && count.value() > UINT64_MAX / width.value()))
          return Result<ResourceString>(
              Status{ErrorCode::ResourceExhausted, {}});
        status = checkpoint_cache_work(count.value() * width.value());
        if (!status.ok())
          return Result<ResourceString>(status);
        status = source.samples.visit(
            [&](const auto& at) {
              auto active = checkpoint_cache_work(0);
              if (!active.ok())
                return active;
              auto plan = found->result.prepare_read(descriptor.value(),
                                                     source.slot, at[0], 1);
              if (!plan.ok())
                return plan.status();
              auto bytes = plan.value().load(
                  options_.maximum_result_window_bytes, active_token());
              if (!bytes.ok())
                return bytes.status();
              hash.bytes(bytes.value()->bytes().data(),
                         bytes.value()->bytes().size());
              return Status::success();
            },
            count.value(), active_token());
      } else if (source.target == ResultSupportTarget::Tensor) {
        if (source.slot >= found->result.schema().tensors.size())
          return Result<ResourceString>(Status{ErrorCode::NotFound, {}});
        const auto width = Value::element_size(found->result.schema()
                                                   .tensors[source.slot]
                                                   .descriptor.element_type);
        status = checkpoint_cache_work(count.value());
        if (!status.ok())
          return Result<ResourceString>(status);
        for (const auto& box : source.samples.boxes()) {
          auto window = found->result.acquire_tensor(
              descriptor.value(), source.slot, box, active_token());
          if (!window.ok())
            return Result<ResourceString>(window.status());
          auto samples = Footprint::from_regions(source.samples.shape(), {box});
          if (!samples.ok())
            return Result<ResourceString>(samples.status());
          status = samples.value().visit(
              [&](const auto& at) {
                auto active = checkpoint_cache_work(0);
                if (!active.ok())
                  return active;
                auto run = window.value().row_run(at);
                if (!run.ok())
                  return run.status();
                hash.bytes(run.value().data, width);
                return Status::success();
              },
              count.value(), active_token());
          if (!status.ok())
            break;
        }
      } else {
        return Result<ResourceString>(Status{ErrorCode::NotFound, {}});
      }
      if (!status.ok())
        return Result<ResourceString>(status);
    }
    auto active = checkpoint_cache_work(0);
    if (!active.ok())
      return Result<ResourceString>(active);
    return Result<ResourceString>(
        ResourceString(hash.finish(), ResourceAllocator<char>(resources_)));
  }
  std::string completed_cache_key(std::size_t index, const Actor& actor) {
    content_internal::Sha256 hash;
    hash.text("photospider.completed-result.v1");
    hash.text(templates_[index]);
    hash.integer(actor.query.output_index);
    hash.integer(static_cast<std::uint32_t>(plan_.steps()[index].backend));
    hash.integer(actor.query.tensor_slot);
    hash.integer(actor.query.tensor_outputs.has_value());
    if (actor.query.tensor_outputs)
      append_block_footprint(&hash, *actor.query.tensor_outputs);
    return hash.finish();
  }
  void retain_cache_need(std::size_t index, Actor& actor,
                         const ResultProgramNeed& need) noexcept {
    if (!blocks_ || !result_cacheable_[index] || actor.cache_disabled)
      return;
    try {
      auto count = 1 + need.tensors.size() + need.results.size();
      if (actor.cache_replay.size() >= blocks_->dependency_metadata_limit() ||
          !checkpoint_cache_work(count).ok()) {
        actor.cache_disabled = true;
        actor.cache_replay.clear();
        return;
      }
      auto facts = supplied_cache_facts(actor);
      if (!facts.ok()) {
        actor.cache_disabled = true;
        actor.cache_replay.clear();
        return;
      }
      StructuredCacheNeed saved;
      saved.request.tensors = need.tensors;
      saved.request.results = need.results;
      saved.facts = facts.take_value();
      actor.cache_replay.push_back(std::move(saved));
    } catch (...) {
      actor.cache_disabled = true;
      actor.cache_replay.clear();
    }
  }
  Result<ResourceVector<SourceObservation>> completed_source_proof(
      std::size_t index, const ResultRef& result) {
    auto bundle = result.dependencies();
    if (!bundle)
      return Result<ResourceVector<SourceObservation>>(
          Status{ErrorCode::NotFound, {}});
    auto limits = options_.dependencies.sets;
    limits.maximum_work = std::min(limits.maximum_work, cache_remaining_);
    limits.consume_work = [&](std::uint64_t n) {
      return checkpoint_cache_work(n);
    };
    limits.cancellation = active_token();
    auto charged = checkpoint_cache_work(1 + plan_.input_declarations().size() +
                                         snapshot_.size() + bindings_.size());
    if (!charged.ok())
      return Result<ResourceVector<SourceObservation>>(charged);
    for (const auto& declaration : plan_.input_declarations()) {
      const auto rank =
          declaration.result_schema &&
                  !declaration.result_schema->tensors.empty()
              ? declaration.result_schema->tensors[0].batch_axes.size() +
                    declaration.result_schema->tensors[0]
                        .descriptor.shape.size()
              : 0;
      charged = checkpoint_cache_work(1 + declaration.name.size() + rank);
      if (!charged.ok())
        return Result<ResourceVector<SourceObservation>>(charged);
    }
    DependencyRecords proof(plan_, std::string(snapshot_), limits);
    for (std::size_t i = 0; i < bindings_.size(); ++i)
      if (bindings_[i].result.valid()) {
        const auto& schema = bindings_[i].result.schema();
        charged = checkpoint_cache_work(1 + schema.fields.size() +
                                        schema.tensors.size());
        if (!charged.ok())
          return Result<ResourceVector<SourceObservation>>(charged);
        for (const auto& tensor : schema.tensors) {
          charged = checkpoint_cache_work(2 + tensor.descriptor.shape.size() +
                                          tensor.batch_axes.size());
          if (!charged.ok())
            return Result<ResourceVector<SourceObservation>>(charged);
        }
        auto status =
            proof.bind_result(PlanWorkflowInput{i}, bindings_[i].result);
        if (!status.ok())
          return Result<ResourceVector<SourceObservation>>(status);
      }
    auto imported = proof.import_bundle(*bundle, index, [&](std::uint64_t n) {
      return checkpoint_cache_work(n);
    });
    if (!imported.ok())
      return Result<ResourceVector<SourceObservation>>(imported);
    return proof.source_observations();
  }
  void retain_cached_result(std::size_t index, Actor& actor) noexcept {
    if (!blocks_ || !result_cacheable_[index] || actor.cache_disabled ||
        actor.fallback_taint || actor.quality ||
        actor.query.backend != plan_.steps()[index].backend ||
        !cache_remaining_ || blocks_->epoch() != block_epoch_)
      return;
    try {
      auto sources = completed_source_proof(index, actor.published);
      if (!sources.ok())
        return;
      auto content = source_cache_digest(sources.value());
      if (!content.ok())
        return;
      auto charged = checkpoint_cache_work(1 + actor.cache_replay.size() +
                                           sources.value().size());
      if (!charged.ok())
        return;
      auto manifest = std::allocate_shared<StructuredCacheManifest>(
          ResourceAllocator<StructuredCacheManifest>(resources_));
      manifest->content = content.take_value();
      manifest->sources = sources.take_value();
      manifest->replay = actor.cache_replay;
      manifest->obligations = actor.input_obligations;
      manifest->bundle = actor.published.dependencies();
      manifest->result = actor.published;
      manifest->backend = actor.query.backend;
      manifest->epoch = block_epoch_;
      manifest->metadata =
          1 + manifest->replay.size() + manifest->sources.size();
      const auto maximum = blocks_->dependency_metadata_limit();
      const auto add_metadata = [&](std::uint64_t count) {
        if (count > maximum || manifest->metadata > maximum - count)
          return false;
        manifest->metadata += count;
        return checkpoint_cache_work(count).ok();
      };
      ResourceVector<const void*> relation_owners;
      ResourceVector<const DependencyRecord*> pending;
      std::set<const DependencyRecord*, std::less<const DependencyRecord*>,
               ResourceAllocator<const DependencyRecord*>>
          seen;
      if (!add_metadata(manifest->bundle->roots.size()))
        return;
      std::set<const TerminalResultRequest*,
               std::less<const TerminalResultRequest*>,
               ResourceAllocator<const TerminalResultRequest*>>
          requests;
      for (const auto& record : manifest->bundle->roots)
        pending.push_back(record.get());
      while (!pending.empty()) {
        if (!checkpoint_cache_work(1).ok())
          return;
        const auto* record = pending.back();
        pending.pop_back();
        if (!seen.insert(record).second)
          continue;
        ++diagnostics_.dependency_cache_records_visited;
        if (record->request && requests.insert(record->request.get()).second) {
          if (!add_metadata(1 + record->request->identity.size() +
                            record->request->manifest.size()))
            return;
          for (const auto& need : record->request->manifest)
            if (!add_metadata(need.tags.size() * 3 +
                              need.samples.shape().size() +
                              need.samples.boxes().size() *
                                  (1 + 2 * need.samples.shape().size())))
              return;
        }

        if (!add_metadata(1 + record->identity.size() + record->scope.size() +
                          record->input_queries.size() +
                          record->samples.shape().size() +
                          record->samples.boxes().size() *
                              (1 + 2 * record->samples.shape().size()) +
                          record->routes.size() + record->upstream.size() +
                          record->upstream_ports.size() +
                          record->domains.size() + record->manifest.size()))
          return;
        for (const auto& input : record->input_queries)
          if (!add_metadata(input.identity.size()))
            return;
        for (const auto& domain : record->domains)
          if (!add_metadata(domain.shape.size()))
            return;
        for (const auto& need : record->manifest)
          if (!add_metadata(need.tags.size() * 3 + need.samples.shape().size() +
                            need.samples.boxes().size() *
                                (1 + 2 * need.samples.shape().size())))
            return;
        if (record->certificate &&
            !add_metadata(record->certificate->storage_entries()))
          return;
        for (const auto& relation : {record->relation, record->descriptor}) {
          auto weight = relation.cache_metadata(
              relation_owners, maximum - manifest->metadata,
              [&](std::uint64_t n) { return checkpoint_cache_work(n); });
          if (!weight.ok())
            return;
          manifest->metadata += weight.value();
        }
        for (const auto& child : record->upstream)
          pending.push_back(child.get());
      }
      for (const auto& source : manifest->sources)
        if (!add_metadata(source.input.size() + source.samples.shape().size() +
                          source.samples.boxes().size() *
                              (1 + 2 * source.samples.shape().size())))
          return;
      auto obligations = manifest->obligations.cache_metadata(
          relation_owners, maximum - manifest->metadata,
          [&](std::uint64_t n) { return checkpoint_cache_work(n); });
      if (!obligations.ok())
        return;
      manifest->metadata += obligations.value();
      for (const auto& log : manifest->replay) {
        manifest->metadata += log.facts.size() + log.request.results.size() +
                              log.request.tensors.size();
        for (const auto& tensor : log.request.tensors)
          manifest->metadata +=
              tensor.samples.boxes().size() + tensor.samples.shape().size();
      }
      if (manifest->metadata > blocks_->dependency_metadata_limit() ||
          !checkpoint_cache_work(manifest->metadata).ok())
        return;
      const auto key = completed_cache_key(index, actor);
      content_internal::Sha256 storage;
      storage.text("photospider.completed-result-payload.v1");
      storage.text(key);
      storage.text(manifest->content);
      for (const auto& log : manifest->replay)
        storage.text(log.facts);
      manifest->key = ResourceString("result/" + storage.finish(),
                                     ResourceAllocator<char>(resources_));
      blocks_->put_structured(key, std::move(manifest), [&](std::uint64_t n) {
        return checkpoint_cache_work(n);
      });
    } catch (...) {
    }
  }
  Result<bool> reuse_cached_result(std::size_t index, Actor& actor) {
    if (!blocks_ || !plan_.steps()[index].output_result_schema ||
        !result_cacheable_[index] || !cache_remaining_ ||
        actor.query.output.result_schema->id == "photospider.path_set")
      return Result<bool>(false);
    bool replayed = false;
    struct InitialState {
      ResultTensorInputs tensors;
      ResultObjectInputs results;
      ResultNeedHistory history;
      ResultInputFacts facts;
      ResultRelation obligations;
      ResourceVector<Actor::InputBundle> bundles;
    };
    std::optional<InitialState> initial;
    const auto reset = [&]() {
      actor.busy = false;
      actor.cache_replay.clear();
      actor.io.clear();
      actor.tensors.clear();
      actor.results.clear();
      actor.history.clear();
      actor.input_facts.clear();
      actor.input_bundles.clear();
      actor.input_obligations = {};
      if (initial) {
        actor.tensors = std::move(initial->tensors);
        actor.results = std::move(initial->results);
        actor.history = std::move(initial->history);
        actor.input_facts = std::move(initial->facts);
        actor.input_obligations = std::move(initial->obligations);
        actor.input_bundles = std::move(initial->bundles);
      }
      return Status::success();
    };
    try {
      auto charged = checkpoint_cache_work(templates_[index].size() + 1);
      if (!charged.ok())
        return charged.code == ErrorCode::ResourceExhausted
                   ? Result<bool>(false)
                   : Result<bool>(charged);
      auto candidates = blocks_->structured_candidates(
          completed_cache_key(index, actor), block_epoch_);
      if (candidates.empty())
        return Result<bool>(false);
      for (const auto& candidate : candidates) {
        auto content = source_cache_digest(candidate->sources);
        if (!content.ok()) {
          if (content.status().code == ErrorCode::Cancelled ||
              content.status().code == ErrorCode::Stale)
            return Result<bool>(content.status());
          continue;
        }
        if (content.value() != candidate->content)
          continue;
        charged = checkpoint_cache_work(
            1 + 64 * actor.tensors.size() + actor.results.size() +
            actor.history.size() + actor.input_facts.size());
        if (!charged.ok())
          return charged.code == ErrorCode::ResourceExhausted
                     ? Result<bool>(false)
                     : Result<bool>(charged);
        initial.emplace(InitialState{
            actor.tensors, actor.results, actor.history, actor.input_facts,
            actor.input_obligations, actor.input_bundles});
        bool matched = true;
        replayed = true;
        actor.busy = true;
        for (const auto& log : candidate->replay) {
          auto status = replay_need(index, actor, log.request);
          if (!status.ok()) {
            actor.busy = false;
            if (detaching_)
              reset();
            if (status.code == ErrorCode::Cancelled ||
                status.code == ErrorCode::Stale)
              return Result<bool>(status);
            matched = false;
            break;
          }
          auto facts = supplied_cache_facts(actor);
          if (!facts.ok() || facts.value() != log.facts) {
            matched = false;
            break;
          }
        }
        actor.busy = false;
        if (actor.fallback_taint) {
          actor.cache_disabled = true;
          matched = false;
        }
        if (!matched) {
          break;
        }
        if (!blocks_->structured_verified(candidate))
          break;
        ResourceVector<std::uint64_t> association{
            ResourceAllocator<std::uint64_t>(resources_)};
        for (const auto& facts : actor.input_facts)
          association.push_back(facts.first.second);
        auto rebound = candidate->result.rebind_cached(
            actor.query.semantic_key, association, active_token(),
            [&](std::uint64_t n) { return checkpoint_cache_work(n); });
        if (!rebound.ok()) {
          if (rebound.status().code == ErrorCode::Cancelled ||
              rebound.status().code == ErrorCode::Stale)
            return Result<bool>(rebound.status());
          break;
        }
        actor.input_obligations = candidate->obligations;
        actor.query.backend = candidate->backend;
        auto status =
            publish_object(index, actor, {rebound.take_value(), true}, true);
        if (!status.ok())
          return Result<bool>(status);
        ++diagnostics_.cache_hits;
        diagnostics_.selected_backends[plan_.steps()[index].result_ref()] =
            actor.query.backend;
        return Result<bool>(true);
      }
      if (!replayed)
        return Result<bool>(false);
      auto scalar = reset();
      return scalar.ok() ? Result<bool>(false) : Result<bool>(scalar);
    } catch (const std::bad_alloc&) {
      if (!replayed)
        return Result<bool>(false);
      try {
        auto status = reset();
        return status.ok() ? Result<bool>(false) : Result<bool>(status);
      } catch (const std::bad_alloc&) {
        return Result<bool>(Status{ErrorCode::ResourceExhausted, {}});
      }
    }
  }
  Status supply_need_step(std::size_t index, Actor& actor,
                          const ResultProgramNeed& need, NeedCursor& cursor,
                          bool replay = false) {
    WorkMode work(work_mode(), replay);
    records_->set_cancellation(active_token());
    const auto& step = plan_.steps()[index];
    const auto fail = [&](Status status) {
      return replay ? status : retire(actor, status);
    };
    if (!cursor.initialized) {
      actor.io.clear();
      actor.tensors.clear();
      const auto count =
          need.results.size() + need.tensors.size() + need.io.size();
      const auto maximum = std::min<std::uint64_t>(
          65536, options_.dependencies.sets.maximum_boxes);
      auto valid = plugin_internal::validate_result_need(
          need, actor.query, step.traits.outputs[0], resources_, maximum,
          options_.maximum_result_window_bytes, replay,
          [&](std::uint64_t amount) { return consume(amount); });
      if (!valid.ok())
        return fail(valid);
      if (!replay) {
        auto registered = register_need_inputs(index, actor, need, true);
        if (!registered.ok())
          return fail(registered);
      }
      cursor.total = count;
      cursor.initialized = true;
      ++progress_;
    }
    const auto inputs = need.results.size() + need.tensors.size();
    if (!replay && inputs > 1 && cursor.next < inputs) {
      if (cursor.requests.empty()) {
        cursor.requests = ResourceVector<std::shared_ptr<NeedCursor>>(
            ResourceAllocator<std::shared_ptr<NeedCursor>>(resources_));
        cursor.requests.reserve(inputs);
        for (std::size_t i = 0; i < inputs; ++i) {
          auto progress = std::allocate_shared<NeedCursor>(
              ResourceAllocator<NeedCursor>(resources_));
          progress->initialized = true;
          progress->next = i;
          progress->total = cursor.total;
          cursor.requests.push_back(std::move(progress));
        }
      }
      for (std::size_t tried = 0; tried < inputs; ++tried) {
        const auto position = cursor.turn++ % inputs;
        auto& progress = *cursor.requests[position];
        if (progress.next != position)
          continue;
        // Supplying one entry reuses its own producer/footprint/atom state.
        auto status = supply_one_need_step(index, actor, need, progress, false);
        if (!status.ok())
          return status;
        if (progress.next != position) {
          ++cursor.supplied;
          ++progress_;
          if (cursor.supplied == inputs) {
            cursor.next = inputs;
            cursor.requests.clear();
            actor.waiting.reset();
          }
          return Status::success();
        }
      }
      return Status::success();
    }
    const auto before = cursor.next;
    auto status = supply_one_need_step(index, actor, need, cursor, replay);
    if (cursor.next != before)
      ++progress_;
    return status;
  }
  Status supply_one_need_step(std::size_t index, Actor& actor,
                              const ResultProgramNeed& need, NeedCursor& cursor,
                              bool replay) {
    const auto& step = plan_.steps()[index];
    const auto fail = [&](Status status) {
      return replay ? status : retire(actor, status);
    };
    const auto results_end = need.results.size();
    if (cursor.next < results_end) {
      const auto& input = need.results[cursor.next];
      auto observed = need_object_input(actor, cursor, step.inputs[input.input],
                                        input, {}, 0, replay, input.input);
      if (!observed.ok())
        return fail(observed.status());
      if (!observed.value())
        return Status::success();
      auto ready = Result<ResultRef>(std::move(*observed.value()));
      note_cached_input_backend(actor, step.inputs[input.input],
                                &ready.value());
      auto bound =
          records_->bind_result(step.inputs[input.input], ready.value());
      if (!bound.ok())
        return fail(bound);
      auto facts = ready.value().descriptor(false);
      if (!facts.ok())
        return fail(facts.status());
      auto history = retain_object_success(actor, input.input, ready.value(),
                                           facts.value());
      if (!history.ok())
        return fail(history);
      auto& revision =
          actor.input_facts[{input.input, ready.value().object_id()}];
      revision = std::max(revision, facts.value().revision());
      actor.results[input.input] = ready.take_value();
      ++cursor.next;
      return Status::success();
    }
    const auto tensors_end = results_end + need.tensors.size();
    if (cursor.next < tensors_end) {
      const auto& input = need.tensors[cursor.next - results_end];
      if (!cursor.tensor_samples) {
        auto closed = actor.query.inputs[input.input]
                          .result_schema->tensors[input.slot]
                          .close_samples(input.samples, set_limits());
        if (!closed.ok())
          return fail(closed.status());
        if (!(input.roles & 7U)) {
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
          auto charged = consume(need.tensors.size());
          if (!charged.ok())
            return fail(charged);
          cursor.tensor_supply_samples = closed;
          cursor.tensor_payload = (input.roles & 7U) != 0;
          for (const auto& other : need.tensors) {
            if (&other == &input || other.input != input.input ||
                other.slot != input.slot || !(other.roles & 7U))
              continue;
            cursor.tensor_payload = true;
            auto part = actor.query.inputs[input.input]
                            .result_schema->tensors[input.slot]
                            .close_samples(other.samples, set_limits());
            if (!part.ok())
              return fail(part.status());
            auto joined =
                cursor.tensor_supply_samples->unite(part.value(), set_limits());
            if (!joined.ok())
              return fail(joined.status());
            cursor.tensor_supply_samples = joined.take_value();
          }
        }
        auto observed = need_tensor_input(
            actor, cursor, step.inputs[input.input],
            *cursor.tensor_supply_samples, input.slot, replay, input.input);
        if (!observed.ok())
          return fail(observed.status());
        if (!observed.value())
          return Status::success();
        capability = std::move(*observed.value());
        capability.payload_authorized_ = cursor.tensor_payload;
      }
      capability.failure_ = actor.failure;
      capability.observer_ = actor.service_failure;
      const auto& constraint = step.traits.input_schema[input.input];
      const auto validate = [&](const ResultRef& result,
                                const ResultDescriptor& descriptor,
                                const Footprint& granted) -> Status {
        note_cached_input_backend(actor, step.inputs[input.input], &result);
        auto selected = closed.intersect(granted, set_limits());
        if (!selected.ok())
          return selected.status();
        if ((input.roles & 7U) && constraint.scalar_bounds &&
            !selected.value().empty()) {
          auto slot = input_internal::resolve_tensor_member(
              constraint, actor.query.inputs[input.input]);
          if (!slot.ok() || slot.value() != input.slot)
            return input_failure(
                step.inputs[input.input],
                slot.ok()
                    ? Status{ErrorCode::TypeMismatch,
                             "scalar Need must select the constrained member"}
                    : slot.status());
          auto checked = input_internal::validate_port_tensor(
              constraint, result, descriptor, actor.query.inputs[input.input],
              std::holds_alternative<PlanWorkflowInput>(
                  step.inputs[input.input])
                  ? ErrorCode::InvalidArgument
                  : ErrorCode::OperationFailed,
              active_token(), [&] { return active_stop(); });
          if (!checked.ok())
            return input_failure(step.inputs[input.input], checked);
        }
        if (input.roles & 4U) {
          auto checked = input_internal::validate_tensor_samples(
              result, descriptor, input.slot, selected.value(), resources_,
              std::holds_alternative<PlanWorkflowInput>(
                  step.inputs[input.input])
                  ? ErrorCode::InvalidArgument
                  : ErrorCode::OperationFailed,
              active_token(), [&] { return active_stop(); },
              [&](uint64_t n) { return operation_work(n); });
          if (!checked.ok())
            return input_failure(step.inputs[input.input], checked);
        }
        auto& revision = actor.input_facts[{input.input, result.object_id()}];
        revision = std::max(revision, descriptor.revision());
        return Status::success();
      };
      auto outside = closed.subtract(capability.samples_, set_limits());
      if (!outside.ok() || !outside.value().empty())
        return fail(protocol("tensor Need is outside supplied coverage"));
      if (capability.pieces_.empty()) {
        auto checked = validate(capability.result_, capability.descriptor_,
                                capability.samples_);
        if (!checked.ok())
          return fail(checked);
      } else {
        for (const auto& piece : capability.pieces_) {
          auto checked =
              validate(piece.result, piece.descriptor, piece.samples);
          if (!checked.ok())
            return fail(checked);
        }
      }
      auto witness = retain_tensor_need(
          actor, input, closed,
          step.traits.outputs[0].region_rule == OperationRegionRule::Whole);
      if (!witness.ok())
        return fail(witness);
      auto history =
          retain_success(actor, input.input, ResultSupportTarget::Tensor,
                         input.slot, input.roles & 7U, closed);
      if (!history.ok())
        return fail(history);
      if (input.roles & 8U) {
        auto metadata = Footprint::all({1}, set_limits());
        if (!metadata.ok())
          return fail(metadata.status());
        history =
            retain_success(actor, input.input, ResultSupportTarget::Descriptor,
                           0, 8, metadata.value());
        if (!history.ok())
          return fail(history);
      }
      if (!replay && existing == actor.tensors.end() &&
          step.traits.outputs[0].region_rule == OperationRegionRule::Whole &&
          step.traits.outputs[0].preserve_output_views) {
        auto prepared = capability.prepare_whole_view(
            step.traits.outputs[0].requires_input_views, active_token(),
            [&](uint64_t n) { return consume(n); });
        if (!prepared.ok())
          return fail(prepared);
      }
      if (existing == actor.tensors.end())
        actor.tensors.emplace(std::make_pair(input.input, input.slot),
                              std::move(capability));
      cursor.tensor_samples.reset();
      cursor.tensor_supply_samples.reset();
      cursor.tensor_payload = false;
      ++cursor.next;
      return Status::success();
    }
    if (cursor.next < cursor.total) {
      const auto& request = need.io[cursor.next - tensors_end];
      actor.retry_safe = false;
      auto ready = io(request);
      if (!ready.ok())
        return fail(ready.status());
#if defined(PHOTOSPIDER_ENABLE_EXECUTION_TEST_HOOKS)
      execution_testing::notify_structured_io_completed();
#endif
      actor.io.push_back(ready.take_value());
      ++cursor.next;
    }
    return Status::success();
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
  Status validate_publication(std::size_t index, Actor& actor,
                              const ResultPublication& published) {
    const auto& step = plan_.steps()[index];
    if (!step.output_result_schema || !published.result.owned_by(resources_) ||
        !published.result.schema().same_schema(*step.output_result_schema) ||
        !published.result.matches_scope(actor.query.semantic_key) ||
        (actor.published.valid() &&
         actor.published.object_id() != published.result.object_id()))
      return protocol("structured publication identity mismatch");
    published.result.bind_producer(actor.node_id);
    auto descriptor = published.result.descriptor(published.complete);
    if (!descriptor.ok() ||
        descriptor.value().revision() <= actor.published_revision ||
        descriptor.value().sealed() != published.complete)
      return protocol("invalid descriptor publication");
    auto facts_work = consume(actor.input_facts.size());
    if (!facts_work.ok())
      return facts_work;
    ResourceVector<std::uint64_t> association{
        ResourceAllocator<std::uint64_t>(resources_)};
    association.reserve(actor.input_facts.size());
    for (const auto& input : actor.input_facts)
      association.push_back(input.first.second);
    auto retained = published.result.retain_association(
        association, [&](auto n) { return consume(n); });
    if (!retained.ok())
      return retained;
    for (uint32_t slot = 0; slot < published.result.schema().tensors.size();
         ++slot) {
      auto relation = published.result.tensor_relation(slot);
      if (!relation.ok())
        return relation.status();
      for (uint32_t port = 0; port < actor.query.inputs.size(); ++port) {
        const auto& schema = actor.query.inputs[port].result_schema;
        if (!schema)
          continue;
        for (uint32_t member = 0; member < schema->tensors.size(); ++member) {
          const auto& tensor = schema->tensors[member];
          const auto channel = input_internal::tuple_channel_axis(
              tensor.descriptor, tensor.facets);
          if (!channel)
            continue;
          auto witness = relation.value();
          if (step.traits.outputs[0].region_rule ==
              OperationRegionRule::Whole) {
            ResourceVector<ResultRelation> obligations{
                ResourceAllocator<ResultRelation>(resources_)};
            obligations.push_back(witness);
            for (const auto& history : actor.history) {
              auto charged = consume(1);
              if (!charged.ok())
                return charged;
              if (std::get<0>(history.first) != port ||
                  std::get<1>(history.first) != ResultSupportTarget::Tensor ||
                  std::get<2>(history.first) != member ||
                  !(std::get<3>(history.first) & 4U))
                continue;
              for (const auto& box : history.second.boxes()) {
                auto scratch = resources_.reserve(
                    ResourceCapacity::host(8 * sizeof(ResultMappedAxis),
                                           8 * sizeof(ResultMappedAxis)));
                if (!scratch.ok())
                  return scratch.status();
                auto lease = scratch.take_value();
                std::vector<ResultMappedAxis> axes;
                axes.reserve(8);
                for (const auto dimension : box.dimensions())
                  axes.push_back({-1, dimension.offset, 0, dimension.extent});
                const auto& shape =
                    published.result.schema().tensors[slot].sample_shape();
                auto obligation = ResultRelation::mapped(
                    resources_, shape, Region::whole(shape),
                    tensor.sample_shape(), axes,
                    {port, 4, 0, 0, ResultSupportTarget::Tensor, member});
                if (!obligation.ok())
                  return obligation.status();
                obligations.push_back(obligation.take_value());
              }
            }
            while (obligations.size() > 1) {
              ResourceVector<ResultRelation> next{
                  ResourceAllocator<ResultRelation>(resources_)};
              for (size_t i = 0; i < obligations.size(); i += 2) {
                if (i + 1 == obligations.size()) {
                  next.push_back(obligations[i]);
                  continue;
                }
                auto joined = ResultRelation::unite(
                    resources_, {obligations[i], obligations[i + 1]});
                if (!joined.ok())
                  return joined.status();
                next.push_back(joined.take_value());
              }
              obligations = std::move(next);
            }
            witness = obligations.front();
          }
          const auto& output_tensor = published.result.schema().tensors[slot];
          const auto output_shape = output_tensor.sample_shape();
          uint32_t grouped_axes = 0;
          for (size_t axis =
                   output_shape.size() - output_tensor.atomic_trailing_axes;
               axis < output_shape.size(); ++axis)
            grouped_axes |= 1U << axis;
          const auto output_channel = input_internal::tuple_channel_axis(
              output_tensor.descriptor, output_tensor.facets);
          if (output_channel)
            grouped_axes |=
                1U << (*output_channel + output_tensor.batch_axes.size());
          auto checked = witness.validate_tuple_closure(
              descriptor.value().tensor_coverage(slot), port, member,
              tensor.sample_shape(), *channel + tensor.batch_axes.size(),
              grouped_axes, set_limits());
          if (!checked.ok())
            return checked;
        }
      }
    }
    if (step.traits.outputs[0].output_schema.scalar_bounds) {
      auto checked = input_internal::validate_port_tensor(
          step.traits.outputs[0].output_schema, published.result,
          descriptor.value(), actor.query.output, ErrorCode::OperationFailed,
          active_token(), [&] { return active_stop(); });
      if (!checked.ok())
        return checked;
    }
    for (uint32_t slot = 0; slot < published.result.schema().tensors.size();
         ++slot) {
      auto checked = input_internal::validate_tensor_samples(
          published.result, descriptor.value(), slot,
          descriptor.value().tensor_coverage(slot), resources_,
          ErrorCode::OperationFailed, active_token(),
          [&] { return active_stop(); },
          [&](uint64_t n) { return operation_work(n); });
      if (!checked.ok()) {
        if (checked.detail.origin == FailureOrigin::Unspecified &&
            checked.code == ErrorCode::OperationFailed)
          checked.detail.origin = FailureOrigin::Domain;
        checked.detail.node_id = actor.node_id;
        return checked;
      }
    }
    auto validated = validate_representation(
        published.result, resources_, options_.maximum_result_window_bytes,
        active_token(), [&](std::uint64_t count) {
          const auto stopped = active_stop();
          if (stopped != ErrorCode::Ok)
            return Status{stopped, {}};
          return admit_run_work(count);
        });
    if (!validated.ok()) {
      if ((validated.detail.origin == FailureOrigin::Unspecified ||
           validated.detail.origin == FailureOrigin::Domain ||
           validated.detail.origin == FailureOrigin::Schema) &&
          validated.code != ErrorCode::ResourceExhausted &&
          validated.code != ErrorCode::Cancelled &&
          validated.code != ErrorCode::Stale) {
        if (validated.detail.origin == FailureOrigin::Unspecified)
          validated.detail.origin = FailureOrigin::Schema;
        validated.detail.scope = FailureScope::Association;
        validated.detail.association = published.result.object_id();
      }
      return validated;
    }
    const bool request_record =
        plan_.steps()[index].traits.outputs[0].observation_kind ==
        ObservationKind::RequestRecord;
    if (request_record && !published.complete)
      return protocol("terminal Result requires complete publication");
    if (request_record && actor.query.tensor_outputs &&
        descriptor.value().tensor_coverage(actor.query.tensor_slot) !=
            *actor.query.tensor_outputs)
      return retire(
          actor,
          protocol("terminal Result coverage differs from captured query"));
    if (published.complete && actor.query.tensor_outputs) {
      auto outside = actor.query.tensor_outputs->subtract(
          descriptor.value().tensor_coverage(actor.query.tensor_slot),
          set_limits());
      if (!outside.ok() || !outside.value().empty())
        return protocol("image publication omitted captured demand");
    }
    return Status::success();
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
    auto evidence = import_input_bundles(actor);
    if (evidence.ok())
      evidence = record_object(index, published.result, &actor);
    if (!evidence.ok())
      return Answer(evidence);
    auto bundle = records_->capture_bundle(index, {}, actor.dependency_scope);
    if (!bundle.ok())
      return Answer(bundle.status());
    published.result.bind_dependencies(bundle.take_value());
    auto captured = published.result.capture();
    return captured;
  }
  Status commit_publication(std::size_t index, Actor& actor, ResultRef captured,
                            bool complete, bool from_cache,
                            bool send_notification) {
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
      auto charged = consume(facts.field_count() + facts.tensor_count());
      if (!charged.ok())
        return retire(actor, charged);
      std::uint64_t elements = 0;
      bool saturated = false;
      const auto add = [&](std::uint64_t count) {
        add_computed_elements(&elements, &saturated, count);
      };
      for (std::uint32_t field = 0; field < facts.field_count(); ++field) {
        const auto before = previous ? previous->rows(field) : 0;
        if (facts.rows(field) < before)
          return retire(actor, protocol("Result rows regressed"));
        add(facts.rows(field) - before);
      }
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
                     item.backend == actor.query.backend;
            });
        if (timing == diagnostics_.operation_timings.end())
          return retire(actor,
                        protocol("missing Result publication diagnostics"));
        add_computed_elements(&timing->computed_elements,
                              &timing->computed_elements_saturated, elements,
                              saturated);
      }
    }
    Status status;
    actor.published = std::move(captured);
    actor.published_revision = descriptor.value().revision();
    actor.complete = complete;
    if (complete && plan_.steps()[index].traits.joint_contract == 2 &&
        actor.observation.rank)
      validation_domains_.at(domain_key(actor)).semantic_terminal = true;
    actor.shared.publish(actor.published, actor.complete, actor.query.backend,
                         actor.fallback_taint, actor.quality);
    if (send_notification)
      status = notify(actor);
    if (!status.ok())
      return status;
    if (actor.complete) {
      actor.attempt_records.reset();
      actor.input_bundles.clear();
      if (!from_cache)
        retain_cached_result(index, actor);
      if (actor.joint)
        actor.joint->members[actor.joint_slot].done->store(true);
      actor.cache_replay.clear();
      actor.continuation = {};
      actor.results.clear();
      actor.tensors.clear();
      actor.io.clear();
      actor.input_facts.clear();
      actor.history.clear();
      release_actor_registration(actor);
    }
    return complete_liveness(actor);
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
  Status poll_actor_phase(
      std::size_t index, const std::shared_ptr<Actor>& current,
      const std::function<Result<ResultProgramPoll>(const ResultProgramPhase&)>&
          poll) {
    const auto* services = active_services();
    const auto optional = work_mode();
    const auto depth = service_depth();
    auto joint = active_joint();
    CallbackContext member_context(this, current.get(), services,
                                   current->shared, optional, depth, joint);
    auto& actor = *current;
    const auto& step = plan_.steps()[index];
    auto& stage = std::get<PollPhase>(actor.phase);
    auto& sticky = stage.sticky;
    auto& numeric = stage.numeric;
    auto& polled = stage.result;
    ResultCallbackScope scope(&sticky, actor.service_failure.get());
    ResourceAllocationScope metadata_scope(resources_, &sticky);
    auto limit = step.traits.workspace_bytes;
    if (step.traits.workspace_input_multiplier) {
      const auto add_input = [&](const Footprint& samples, ElementType type) {
        auto count = samples.element_count();
        const auto factor =
            Value::element_size(type) *
            static_cast<std::uint64_t>(step.traits.workspace_input_multiplier);
        if (!count.ok() || count.value() > (UINT64_MAX - limit) / factor)
          return false;
        limit += count.value() * factor;
        return true;
      };
      for (const auto& input : actor.tensors)
        if (!add_input(input.second.coverage(),
                       input.second.spec().descriptor.element_type))
          return Status{ErrorCode::ResourceExhausted, {}};
    }
    auto allocator =
        (active_services() ? active_services()->allocator
                           : resources_.allocator())
            .limited(limit, [full = actor.service_failure](ErrorCode code) {
              full->record(Status{code, {}});
            });
    auto observe_failure = [&actor, &sticky](const Status& failed) {
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
    };
    const auto* producer = active_shared();
    const auto token = active_token();
    bool in_block = false, in_discovery = false;
    auto work = [&, producer, token](std::uint64_t count) {
      const auto previous = sticky;
      const auto stopped = execution_stop(producer, token);
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
          result = admit_run_work(count);
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
    };
    ResultProgramPhase phase{actor.query,   actor.results,  actor.io,
                             allocator,     resources_,     work,
                             actor.failure, observe_failure};
    ResourceVector<std::uint64_t> association{
        ResourceAllocator<std::uint64_t>(resources_)};
    auto facts_work = work(actor.input_facts.size());
    if (!facts_work.ok())
      return facts_work;
    association.reserve(actor.input_facts.size());
    for (const auto& input : actor.input_facts)
      association.push_back(input.first.second);
    phase.report_numeric = [&](const NumericDiagnostics& report) {
      auto status = work(1);
      if (status.ok())
        status = merge_numeric_diagnostics(&numeric, report);
      if (!status.ok())
        observe_failure(status);
      return status;
    };
    phase.failure_latch = actor.service_failure;
    phase.association = &association;
    phase.tensors = &actor.tensors;
    ResourceVector<std::shared_ptr<const ResultDiscoveryReceipt>> discovered{
        ResourceAllocator<std::shared_ptr<const ResultDiscoveryReceipt>>(
            resources_)};
    std::uint64_t discovered_count = 0;
    phase.checkpoint_before = [&](std::uint32_t kind, std::uint64_t before) {
      using Answer = Result<std::optional<ResultCheckpoint>>;
      try {
        CallbackContext scope(this, current.get(), services, current->shared,
                              optional, depth, joint);
        if (in_discovery) {
          auto failed = protocol("checkpoint inside GPU discovery");
          observe_failure(failed);
          return Answer(failed);
        }
        actor.retry_safe = false;
        auto found = checkpoint_before(index, actor, kind, before);
        if (found.ok() && found.value()) {
          auto charged = work(actor.input_facts.size());
          if (!charged.ok()) {
            observe_failure(charged);
            return Answer(charged);
          }
          ResourceVector<std::uint64_t> refreshed{
              ResourceAllocator<std::uint64_t>(resources_)};
          refreshed.reserve(actor.input_facts.size());
          for (const auto& input : actor.input_facts)
            refreshed.push_back(input.first.second);
          association = std::move(refreshed);
        }
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
    };
    phase.checkpoint_publish = [&](std::uint32_t kind, std::uint64_t sequence,
                                   const ResultRef& state) {
      Status published;
      try {
        CallbackContext scope(this, current.get(), services, current->shared,
                              optional, depth, joint);
        if (in_discovery) {
          auto failed = protocol("checkpoint inside GPU discovery");
          observe_failure(failed);
          return failed;
        }
        actor.retry_safe = false;
        published = checkpoint_publish(index, actor, kind, sequence, state);
      } catch (const std::bad_alloc&) {
        published = Status{ErrorCode::ResourceExhausted, {}};
      } catch (...) {
        published = Status{ErrorCode::OperationFailed, {}};
      }
      if (!published.ok())
        observe_failure(published);
      return published;
    };
    phase.block = [&](std::uint32_t kind, std::uint64_t begin,
                      std::uint64_t end, std::uint64_t mode,
                      const ResultRef& incoming,
                      const std::function<Result<ResultRef>()>& compute) {
      using Answer = Result<ResultRef>;
      try {
        CallbackContext context(this, current.get(), services, current->shared,
                                optional, depth, joint);
        if (in_discovery || in_block) {
          auto failed = protocol("nested Result pure block or GPU discovery");
          observe_failure(failed);
          return Answer(failed);
        }
        struct Scope {
          bool& value;
          ~Scope() { value = false; }
        } scope{in_block};
        in_block = true;
        actor.retry_safe = false;
        auto result = evaluate_block(index, actor, phase, kind, begin, end,
                                     mode, incoming, compute);
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
    };
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
          auto result = [&]() -> Answer {
            try {
              if (in_block || in_discovery || !phase.gpu)
                return Answer(protocol("invalid Result GPU discovery scope"));
              struct Scope {
                bool& value;
                ~Scope() { value = false; }
              } scope{in_discovery};
              in_discovery = true;
              actor.retry_safe = false;
              const auto before = active_services()->gpu_dispatches();
              auto needs = ResultNativeScope::discover(
                  phase.query, capacity, candidates, compute, work);
              if (!needs.ok())
                return Answer(needs.status());
              if (active_services()->gpu_dispatches() == before)
                return Answer(
                    Status{ErrorCode::OperationFailed,
                           "Result GPU discovery submitted no native work"});
              const auto maximum = std::min<std::uint64_t>(
                  65536, options_.dependencies.sets.maximum_boxes);
              if (discovered_count > maximum ||
                  needs.value().size() > maximum - discovered_count)
                return Answer(Status{ErrorCode::ResourceExhausted,
                                     "GPU discovery need limit"});
              auto receipt = std::allocate_shared<ResultDiscoveryReceipt>(
                  ResourceAllocator<ResultDiscoveryReceipt>(resources_));
              receipt->tensors = needs.take_value();
              discovered_count += receipt->tensors.size();
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
    polled = poll(phase);
    if (polled.ok() && discovered_count) {
      auto response = polled.take_value();
      auto* need = std::get_if<ResultProgramNeed>(&response);
      if (!need) {
        observe_failure(
            protocol("GPU discovery requires supply before completion"));
      } else {
        auto charged = work(discovered_count + need->tensors.size());
        const auto maximum = std::min<std::uint64_t>(
            65536, options_.dependencies.sets.maximum_boxes);
        if (charged.ok() && (need->tensors.size() > maximum ||
                             discovered_count > maximum - need->tensors.size()))
          charged = {ErrorCode::ResourceExhausted,
                     "GPU discovery attachment limit"};
        if (!charged.ok()) {
          observe_failure(charged);
        } else {
          for (const auto& receipt : discovered)
            for (const auto& tensor : receipt->tensors)
              need->tensors.push_back(tensor);
        }
      }
      polled = Result<ResultProgramPoll>(std::move(response));
    }
    if (active_services() && active_services()->gpu_status) {
      auto native_status = active_services()->gpu_status();
      if (native_status.code == ErrorCode::InvalidArgument) {
        native_status.reason = FailureReason::UnauthorizedRead;
        native_status.detail.origin = FailureOrigin::Protocol;
        native_status.detail.scope = FailureScope::Group;
      }
      if (!native_status.ok())
        observe_failure(native_status);
    }
    if (actor.query.backend == Backend::Gpu && active_services() &&
        active_services()->gpu_dispatches) {
      const auto count = active_services()->gpu_dispatches();
      stage.native_dispatches = count;
      if (count > UINT64_MAX - actor.native_dispatches)
        observe_failure(Status{ErrorCode::ResourceExhausted,
                               "native dispatch counter overflow"});
      else
        actor.native_dispatches += count;
    }
    bool require_native_dispatch = true;
#if defined(PHOTOSPIDER_ENABLE_EXECUTION_TEST_HOOKS)
    // A constructed scheduler fixture can supply a logical GPU lane without
    // native services. An actual device callback always keeps its GPU table.
    require_native_dispatch = !active_services() || active_services()->gpu ||
                              !active_services()->native_gpu_available;
#endif
    if (polled.ok() && actor.query.backend == Backend::Gpu &&
        require_native_dispatch && !actor.native_dispatches &&
        !actor.native_block_reuse &&
        step.traits.outputs[0].data_movement !=
            DataMovementKind::BitwiseMapped) {
      if (const auto* publication =
              std::get_if<ResultPublication>(&polled.value())) {
        auto facts = publication->result.descriptor(false);
        if (!facts.ok()) {
          observe_failure(facts.status());
        } else {
          bool nonempty = false;
          for (std::uint32_t i = 0; i < facts.value().tensor_count(); ++i)
            nonempty |= !facts.value().tensor_coverage(i).empty();
          for (std::uint32_t i = 0; i < facts.value().field_count(); ++i)
            nonempty |= facts.value().rows(i) != 0;
          if (nonempty)
            observe_failure(
                Status{ErrorCode::OperationFailed,
                       "GPU Result publication performed no native dispatch"});
        }
      }
    }

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
    const auto& step = plan_.steps()[index];
    ActiveScope producer_scope(*this, actor.shared);
    auto current = actor.self.lock();
    if (!current)
      return protocol("structured Actor owner is absent");
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
    if (actor.joint)
      return advance_joint(actor.joint);
    if (actor.deferred_start) {
      auto grouped = start_joint(index, current);
      if (!grouped.ok())
        return retire(actor, grouped.status());
      if (grouped.value())
        return Status::success();
      if (step.traits.joint_contract == 2)
        return retire(actor, protocol("Result atom has no admitted joint"));
      if (actor.scalar_restart) {
        auto ready = prepare_scalars(index, actor);
        if (!ready.ok())
          return retire(actor, ready);
        actor.scalar_restart = false;
      }
      auto submitted = start_actor(index, current);
      return submitted.ok() ? submitted : retire(actor, submitted);
    }
    if (actor.queued)
      return Status::success();
    if (actor.pending && !actor.pending->ready()) {
      refresh_shared();
      actor.pending->completion.wait_for(std::chrono::milliseconds(2));
      if (!actor.pending->ready()) {
        if (plan_owner_ && shared_ && !callback_ && handoff_allowed_ &&
            caller_external_stop() != ErrorCode::Ok && actor.shared.valid() &&
            actor.shared.producer() && actor.shared.continue_for_peers()) {
          auto protocol_failure = pending_protocol_failure();
          if (!protocol_failure.ok()) {
            wait_pending();
            return protocol_failure;
          }
          detaching_ = true;
          return Status{caller_external_stop(), {}};
        }
        return Status::success();
      }
    }
    if (std::holds_alternative<StartPhase>(actor.phase)) {
      auto dispatched = finish_actor(current);
      auto started = std::move(std::get<StartPhase>(actor.phase));
      actor.phase.emplace<std::monostate>();
      started.dispatched = std::move(dispatched);
      return complete_start(index, current, std::move(started));
    }
    if (auto* need = std::get_if<NeedPhase>(&actor.phase)) {
      if (!actor.busy) {
        actor.busy = true;
        actor.driver = std::this_thread::get_id();
      }
      Status supplied;
      try {
        supplied = supply_need_step(index, actor, need->request, need->cursor);
      } catch (const std::bad_alloc&) {
        return retire(actor, Status{ErrorCode::ResourceExhausted, {}});
      } catch (...) {
        return retire(actor, Status{ErrorCode::OperationFailed, {}});
      }
      if (!supplied.ok())
        return supplied;
      if (need->cursor.complete()) {
        retain_cache_need(index, actor, need->request);
        actor.phase.emplace<std::monostate>();
      }
      return Status::success();
    }
    Status status;
    if (!actor.pending) {
      auto charged = consume(1);
      if (!charged.ok())
        return retire(actor, charged);
      if (actor.polls >=
          std::min(step.traits.outputs[0].maximum_dependency_stages,
                   options_.dependencies.maximum_stages))
        return retire(actor, Status{ErrorCode::ResourceExhausted,
                                    "structured stage limit"});
      ++actor.polls;
      actor.busy = true;
      actor.driver = std::this_thread::get_id();
      actor.phase.emplace<PollPhase>();
      std::get<PollPhase>(actor.phase).sticky = actor.failure->load();
      status = submit_actor(
          current,
          [this, index, current] {
            return poll_actor_phase(index, current,
                                    [&](const ResultProgramPhase& phase) {
                                      return current->continuation.poll(phase);
                                    });
          },
          actor.query.backend,
          step.traits.outputs[0].region_rule == OperationRegionRule::Whole,
          step.traits.cpu_staged_tiles);
    }
    if (status.ok() && actor.queued)
      return Status::success();
    if (status.ok() && !actor.pending->ready()) {
      refresh_shared();
      actor.pending->completion.wait_for(std::chrono::milliseconds(2));
      if (!actor.pending->ready())
        return Status::success();
    }
    if (status.ok())
      status = finish_actor(current);
    auto completed = std::move(std::get<PollPhase>(actor.phase));
    actor.phase.emplace<std::monostate>();
    return complete_poll(index, current, std::move(status),
                         std::move(completed));
  }
  Status complete_poll(std::size_t index, const std::shared_ptr<Actor>& current,
                       Status status, PollPhase completed,
                       const ResultRef* certified = nullptr) {
    auto& actor = *current;
    const auto& step = plan_.steps()[index];
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
    const bool retryable_host_failure =
        host_failure.ok() ||
        plugin_internal::FailureLatch::retryable_backend_failure(host_failure);
    if (plugin_internal::FailureLatch::retryable_backend_failure(failed) &&
        actor.query.backend == Backend::Gpu && step.traits.supports_cpu &&
        step.traits.allows_cpu_fallback && step.traits.deterministic &&
        step.traits.side_effect_free && actor.retry_safe &&
        !actor.native_dispatches && !actor.published.valid() &&
        sticky == ErrorCode::Ok &&
        (operation_failure == ErrorCode::Ok ||
         operation_failure == ErrorCode::BackendUnavailable) &&
        retryable_host_failure &&
        actor.service_failure->backend_retry_allowed() &&
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
      actor.io.clear();
      actor.history.clear();
      actor.input_facts.clear();
      actor.input_bundles.clear();
      if (!actor.attempt_records)
        return retire(actor, protocol("missing Result attempt records"));
      auto restore_work = consume(1 + actors_.size());
      if (!restore_work.ok())
        return retire(actor, restore_work);
      records_ = std::move(actor.attempt_records);
      for (const auto& weak : actors_) {
        auto peer = weak.lock();
        if (!peer || peer.get() == &actor || peer->complete ||
            peer->terminal != ErrorCode::Ok)
          continue;
        auto imported = import_input_bundles(*peer);
        if (!imported.ok())
          return retire(actor, imported);
      }
      actor.input_obligations = {};
      actor.cache_replay.clear();
      actor.cache_disabled = true;
      actor.fallback_taint = true;
      actor.query.backend = Backend::Cpu;
      auto ready = prepare_scalars(index, actor);
      if (!ready.ok())
        return retire(actor, ready);
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
    actor.io.clear();
    actor.tensors.clear();
    auto recorded = record_poll_diagnostics(actor, completed, ErrorCode::Ok);
    if (!recorded.ok())
      return retire(actor, recorded);
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
  std::recursive_mutex driver_mutex_;
  bool detaching_ = false, handoff_allowed_ = true, driver_active_ = false;
  const ExecutionPlan& plan_;
  std::vector<ExecutionBinding> bindings_;
  std::shared_ptr<OperationRegistry> operations_;
  ResourceBudget resources_;
  PayloadCapture payload_capture_;
  ResourceBindings bindings_resources_;
  ExecutionOptions options_;
  bool atom_outcomes_;
  const std::uint32_t maximum_parallelism_;
  ResultSubscription subscription_;
  CancellationToken cancellation_;
  std::function<ErrorCode()> stop_;
  StructuredDispatch dispatch_;
  const std::vector<std::string>& templates_;
  SharedResults* shared_ = nullptr;
  ResultCheckpoints* checkpoints_ = nullptr;
  ResultCache* blocks_ = nullptr;
  std::uint64_t block_epoch_ = 0;
  SharedResults::RunLease call_;
  unsigned service_depth_ = 0;
  struct C1NeedRegistration {
    std::shared_ptr<const C1NeedRegistration> previous;
    ResourceVector<std::size_t> steps;
    C1NeedRegistration(std::shared_ptr<const C1NeedRegistration> previous,
                       ResourceVector<std::size_t> steps)
        : previous(std::move(previous)), steps(std::move(steps)) {}
  };
  std::shared_ptr<const C1NeedRegistration> c1_need_registration_;
  Result<bool> c1_registering(const Actor& actor) {
    for (auto scope = std::atomic_load(&c1_need_registration_); scope;
         scope = scope->previous) {
      unsigned comparisons = 1;
      for (auto count = scope->steps.size(); count > 1; count >>= 1)
        ++comparisons;
      auto charged = consume((actor.aliases.size() + 1) * comparisons);
      if (!charged.ok())
        return Result<bool>(charged);
      if (std::binary_search(scope->steps.begin(), scope->steps.end(),
                             actor.index))
        return Result<bool>(true);
      for (auto index : actor.aliases)
        if (std::binary_search(scope->steps.begin(), scope->steps.end(), index))
          return Result<bool>(true);
    }
    return Result<bool>(false);
  }
  const SharedResults::Lease* active_shared_ = nullptr;
  ResourceString snapshot_;
  ResourceString snapshot_input_;
  std::atomic<std::uint64_t> remaining_;
  std::uint64_t cache_remaining_;
  bool optional_replay_ = false;
  bool rollback_possible_ = false;
  ResourceVector<std::weak_ptr<Actor>> actors_;
  ResourceVector<std::shared_ptr<Actor>> actor_aliases_;
  ResourceMap<ActorRegistration> actor_queries_;
  using DomainKey = std::tuple<std::string_view, std::uint32_t, std::uint32_t>;
  struct ValidationDomainRecord {
    core_internal::StoredFailure failure;
    std::optional<QualityReport> quality;
    bool semantic_terminal = false;
  };
  using DomainEntry = std::pair<const DomainKey, ValidationDomainRecord>;
  std::map<DomainKey, ValidationDomainRecord, std::less<DomainKey>,
           ResourceAllocator<DomainEntry>>
      validation_domains_;
  DomainKey domain_key(const Actor& actor) const {
    return {templates_[actor.index], actor.query.output_index,
            actor.query.tensor_slot};
  }
  ResourceVector<PendingTask> pending_tasks_;
  ResourceVector<std::shared_ptr<SharedResults::ProducerPayload>>
      shared_payloads_;
  ResourceVector<std::uint64_t> remaining_consumers_;
  ResourceVector<bool> named_pins_, finished_steps_;
  ResourceVector<std::shared_ptr<Actor>> requested_roots_;
  std::uint64_t progress_ = 0, traversal_ = 1;
  std::recursive_mutex callback_metadata_mutex_;
  std::shared_ptr<JointActor> active_joint_;
  std::uint32_t joint_depth_ = 0;
  ResourceVector<bool> shareable_closure_, checkpoint_shareable_,
      result_cacheable_;

  ResourceMap<std::shared_ptr<ResultCheckpointScope>> checkpoint_scopes_;
  ExecutionDiagnostics diagnostics_;
  std::unique_ptr<DependencyRecords> records_;
};
Result<ExecutionResult> execute_structured(
    const ExecutionPlan& plan, std::vector<ExecutionBinding> bindings,
    std::shared_ptr<OperationRegistry> operations, ResourceBudget resources,
    const ExecutionOptions& options, const CancellationToken& cancellation,
    const std::function<ErrorCode()>& stop, const StructuredDispatch& dispatch,
    const DemandQuery* requested, const std::string& snapshot_identity,
    SharedResults* shared_results, ResultCheckpoints* checkpoints,
    ResultCache* blocks, std::shared_ptr<const ExecutionPlan> plan_owner,
    bool atom_outcomes, std::uint32_t maximum_parallelism) {
  try {
    auto execution = std::allocate_shared<StructuredExecution>(
        ResourceAllocator<StructuredExecution>(resources), plan,
        std::move(bindings), std::move(operations), resources, options,
        cancellation, stop, dispatch, snapshot_identity, shared_results,
        checkpoints, blocks, std::move(plan_owner), atom_outcomes,
        maximum_parallelism);
    return execution->run(requested);
  } catch (const std::bad_alloc&) {
    return Result<ExecutionResult>(Status{ErrorCode::ResourceExhausted, {}});
  } catch (...) {
    return Result<ExecutionResult>(Status{ErrorCode::OperationFailed, {}});
  }
}
}  // namespace ps::execution_internal
