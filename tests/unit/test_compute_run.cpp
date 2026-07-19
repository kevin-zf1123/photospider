#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <exception>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "compute/compute_run.hpp"
#include "compute/compute_task_submission.hpp"
#include "compute/dirty_write_buffers.hpp"
#include "graph/graph_model.hpp"  // NOLINT(build/include_subdir)
#include "graph/graph_traversal_service.hpp"
#include "photospider/scheduler/scheduler_task_runtime.hpp"

namespace ps::compute {
namespace {

/**
 * @brief Builds deterministic descriptor input for ComputeRun unit tests.
 *
 * @param graph_identity Graph/session label to capture.
 * @param topology_generation Topology-only submission revision.
 * @param target_node_id Requested graph-local target.
 * @return Valid full-quality HP submission with explicit throughput QoS.
 * @throws std::bad_alloc if graph identity ownership cannot allocate.
 * @note The returned deadline is absent so test equality does not depend on
 * wall-clock timing.
 */
ComputeRunSubmission make_test_submission(std::string graph_identity,
                                          uint64_t topology_generation,
                                          int target_node_id) {
  return ComputeRunSubmission{
      std::move(graph_identity),
      ComputeRunSubmissionRevision{topology_generation},
      target_node_id,
      ComputeIntent::GlobalHighPrecision,
      ComputeRunQuality::Full,
      ComputeRunQos{ComputeRunQosClass::Throughput, std::nullopt, 3, 2}};
}

/**
 * @brief Builds a minimal valid graph node for Run-owned plan construction.
 *
 * @param id Stable graph-local node id.
 * @return Node with deterministic name and intentionally unresolved operation.
 * @throws std::bad_alloc if string assignment cannot allocate.
 * @note TaskSubmissionPlan preserves a missing operation for worker-time error,
 * so plan/storage construction does not require registry mutation.
 */
Node make_plan_node(int id) {
  Node node;
  node.id = id;
  node.name = "compute_run_node_" + std::to_string(id);
  node.type = "compute_run_test";
  node.subtype = "source";
  return node;
}

/**
 * @brief Deterministic owned-callback runtime for completion-isolation tests.
 *
 * The fixture accepts only the empty full-HP epoch marker, stores owned
 * callbacks until wait_for_completion(), and validates that every explicitly
 * added completion unit is released before the wait returns. All methods run
 * on the calling test thread, so a leaked unit fails deterministically instead
 * of hanging on a condition variable.
 *
 * @throws std::logic_error for borrowed work, missing batch initialization,
 * completion overflow/underflow, or a nonzero count after callback drainage.
 * @throws The exact first callback exception from wait_for_completion().
 * @note This fixture models scheduler completion ownership only; it owns no
 * worker, epoch filter, graph state, or operation implementation.
 */
class CompletionTrackingRuntime final : public SchedulerTaskRuntime {
 public:
  /**
   * @brief Starts a fresh completion batch from the required empty handle set.
   *
   * @param handles Borrowed handles, which must be empty.
   * @param total_task_count Initial count, which must be zero.
   * @param priority Ignored because the fixture has one deterministic lane.
   * @return Nothing.
   * @throws std::logic_error for nonempty handles or a nonzero initial count.
   * @note Starting a batch clears queued callbacks and all per-batch state.
   */
  void submit_initial_task_handles(
      std::vector<TaskHandle>&& handles, int total_task_count,
      SchedulerTaskPriority priority = SchedulerTaskPriority::Normal) override {
    (void)priority;
    if (!handles.empty() || total_task_count != 0) {
      throw std::logic_error(
          "CompletionTrackingRuntime requires an empty zero-count batch.");
    }
    callbacks_.clear();
    next_callback_ = 0;
    first_exception_ = nullptr;
    tasks_to_complete_ = 0;
    batch_initialized_ = true;
  }

  /**
   * @brief Rejects borrowed worker-ready work from the full-HP test path.
   *
   * @param handles Borrowed handles, which must be empty.
   * @param priority Ignored because no borrowed work is accepted.
   * @return Nothing.
   * @throws std::logic_error when any borrowed handle is supplied.
   * @note Empty input is accepted as a no-op for interface completeness.
   */
  void submit_ready_task_handles_from_worker(
      std::vector<TaskHandle>&& handles,
      SchedulerTaskPriority priority = SchedulerTaskPriority::Normal) override {
    (void)priority;
    if (!handles.empty()) {
      throw std::logic_error(
          "CompletionTrackingRuntime rejects borrowed ready handles.");
    }
  }

  /**
   * @brief Retains one owned callback until deterministic batch drainage.
   *
   * @param task Callback whose ownership transfers to this fixture.
   * @param priority Ignored deterministic priority.
   * @param epoch Ignored because the fixture has one active batch.
   * @return Nothing.
   * @throws std::logic_error when no batch has been initialized.
   * @throws std::bad_alloc when callback storage cannot grow.
   * @note Empty callbacks are ignored and do not change observation counters.
   */
  void submit_ready_task_any_thread(
      Task&& task,
      SchedulerTaskPriority priority = SchedulerTaskPriority::Normal,
      std::optional<std::uint64_t> epoch = std::nullopt) override {
    (void)priority;
    (void)epoch;
    if (!task) {
      return;
    }
    if (!batch_initialized_) {
      throw std::logic_error(
          "CompletionTrackingRuntime has no initialized batch.");
    }
    callbacks_.push_back(std::move(task));
    ++callbacks_submitted_;
  }

  /**
   * @brief Drains accepted callbacks and validates exact completion settlement.
   *
   * @return Nothing when callbacks finish and the count reaches zero.
   * @throws The exact first callback exception.
   * @throws std::logic_error when completion units remain after drainage.
   * @note Callbacks may append dependent callbacks while this loop is active.
   */
  void wait_for_completion() override {
    while (!first_exception_ && next_callback_ < callbacks_.size()) {
      Task callback = std::move(callbacks_.at(next_callback_));
      ++next_callback_;
      ++callbacks_invoked_;
      try {
        callback();
      } catch (...) {
        set_exception(std::current_exception());
      }
    }
    callbacks_.clear();
    next_callback_ = 0;
    if (first_exception_) {
      std::rethrow_exception(first_exception_);
    }
    if (tasks_to_complete_ != 0) {
      throw std::logic_error(
          "CompletionTrackingRuntime retained an unsettled completion unit.");
    }
  }

  /**
   * @brief Retains the first callback exception for wait-time rethrow.
   *
   * @param error Exact callback exception; null input is ignored.
   * @return Nothing.
   * @throws Nothing.
   * @note Later exceptions do not replace the first retained identity.
   */
  void set_exception(std::exception_ptr error) noexcept override {
    if (error && !first_exception_) {
      first_exception_ = std::move(error);
    }
  }

  /**
   * @brief Adds positive units to the current completion count.
   *
   * @param delta Positive unit count.
   * @return Nothing.
   * @throws std::logic_error for a missing batch or integer overflow.
   * @note Nonpositive input is a no-op, matching current scheduler behavior.
   */
  void inc_tasks_to_complete(int delta) override {
    if (delta <= 0) {
      return;
    }
    if (!batch_initialized_) {
      throw std::logic_error(
          "CompletionTrackingRuntime has no initialized batch.");
    }
    if (tasks_to_complete_ > std::numeric_limits<int>::max() - delta) {
      throw std::logic_error(
          "CompletionTrackingRuntime completion count overflow.");
    }
    tasks_to_complete_ += delta;
    total_units_added_ += delta;
  }

  /**
   * @brief Releases exactly one unit from the current completion count.
   *
   * @return Nothing.
   * @throws std::logic_error when no completion unit remains.
   * @note The release counter increments only after a valid decrement.
   */
  void dec_tasks_to_complete() override {
    if (tasks_to_complete_ <= 0) {
      throw std::logic_error(
          "CompletionTrackingRuntime completion count underflow.");
    }
    --tasks_to_complete_;
    ++completion_releases_;
  }

  /**
   * @brief Records whether planned operation execution was entered.
   *
   * @param action Stable scheduler trace action.
   * @param node_id Backend node id, unused by this fixture.
   * @return Nothing.
   * @throws Nothing.
   * @note Assignment traces are intentionally excluded from the execution
   * count.
   */
  void log_event(SchedulerTraceAction action, int node_id) noexcept override {
    (void)node_id;
    if (action == SchedulerTraceAction::Execute ||
        action == SchedulerTraceAction::ExecuteTile) {
      ++planned_execution_events_;
    }
  }

  /**
   * @brief Returns the current logical completion count.
   *
   * @return Outstanding units in the active batch.
   * @throws Nothing.
   */
  int tasks_to_complete() const noexcept { return tasks_to_complete_; }

  /**
   * @brief Returns all units added across the fixture lifetime.
   *
   * @return Sum of positive completion increments.
   * @throws Nothing.
   */
  int total_units_added() const noexcept { return total_units_added_; }

  /**
   * @brief Returns successful completion decrement calls.
   *
   * @return Number of exactly-one unit releases.
   * @throws Nothing.
   */
  int completion_releases() const noexcept { return completion_releases_; }

  /**
   * @brief Returns owned callbacks accepted by the fixture.
   *
   * @return Number of nonempty callback submissions.
   * @throws Nothing.
   */
  int callbacks_submitted() const noexcept { return callbacks_submitted_; }

  /**
   * @brief Returns owned callbacks invoked by batch drainage.
   *
   * @return Number of callbacks whose callable body was entered.
   * @throws Nothing.
   */
  int callbacks_invoked() const noexcept { return callbacks_invoked_; }

  /**
   * @brief Returns planned execution trace entries.
   *
   * @return Number of Execute or ExecuteTile actions.
   * @throws Nothing.
   * @note Zero proves the accepted callback did not enter NodeTaskRunner work.
   */
  int planned_execution_events() const noexcept {
    return planned_execution_events_;
  }

 private:
  /** @brief Owned callbacks retained until wait_for_completion(). */
  std::vector<Task> callbacks_;

  /** @brief Index of the next callback selected for deterministic drainage. */
  std::size_t next_callback_ = 0;

  /** @brief Exact first callback failure retained for wait-time rethrow. */
  std::exception_ptr first_exception_;

  /** @brief Outstanding logical completion units in the active batch. */
  int tasks_to_complete_ = 0;

  /** @brief Sum of positive units added across completed test batches. */
  int total_units_added_ = 0;

  /** @brief Number of successful exactly-one completion releases. */
  int completion_releases_ = 0;

  /** @brief Number of accepted nonempty owned callbacks. */
  int callbacks_submitted_ = 0;

  /** @brief Number of accepted callback bodies entered during drainage. */
  int callbacks_invoked_ = 0;

  /** @brief Number of planned Execute or ExecuteTile trace actions. */
  int planned_execution_events_ = 0;

  /** @brief Whether the required empty epoch marker initialized a batch. */
  bool batch_initialized_ = false;
};

/**
 * @brief Verifies immutable Run request identity and explicit QoS.
 *
 * @note Two live Runs must never receive the same process-lifetime identity.
 */
TEST(ComputeRunDescriptor, CapturesIdRevisionIntentQualityAndQosWithoutReuse) {
  ComputeRun first(make_test_submission("session-a", 17, 42));
  ComputeRun second(make_test_submission("session-b", 18, 43));

  EXPECT_NE(first.descriptor().id().value(), 0U);
  EXPECT_NE(first.descriptor().id(), second.descriptor().id());
  EXPECT_EQ(first.descriptor().graph_identity(), "session-a");
  EXPECT_EQ(first.descriptor().revision().topology_generation, 17U);
  EXPECT_EQ(first.descriptor().target_node_id(), 42);
  EXPECT_EQ(first.descriptor().intent(), ComputeIntent::GlobalHighPrecision);
  EXPECT_EQ(first.descriptor().quality(), ComputeRunQuality::Full);
  EXPECT_EQ(first.descriptor().qos().service_class,
            ComputeRunQosClass::Throughput);
  EXPECT_FALSE(first.descriptor().qos().deadline.has_value());
  EXPECT_EQ(first.descriptor().qos().weight, 3U);
  ASSERT_TRUE(first.descriptor().qos().maximum_parallelism.has_value());
  EXPECT_EQ(*first.descriptor().qos().maximum_parallelism, 2U);
}

/**
 * @brief Verifies invalid domain/QoS values are rejected before Run admission.
 *
 * @note Validation happens before a Run id is minted, avoiding identities for
 * descriptors that never become Runs.
 */
TEST(ComputeRunDescriptor, RejectsRealtimeAndNonpositiveQosValues) {
  ComputeRunSubmission realtime = make_test_submission("rt", 1, 1);
  realtime.intent = ComputeIntent::RealTimeUpdate;
  EXPECT_THROW((void)ComputeRun(std::move(realtime)), std::invalid_argument);

  ComputeRunSubmission zero_weight = make_test_submission("weight", 1, 1);
  zero_weight.qos.weight = 0;
  EXPECT_THROW((void)ComputeRun(std::move(zero_weight)), std::invalid_argument);

  ComputeRunSubmission zero_parallelism =
      make_test_submission("parallelism", 1, 1);
  zero_parallelism.qos.maximum_parallelism = 0;
  EXPECT_THROW((void)ComputeRun(std::move(zero_parallelism)),
               std::invalid_argument);
}

/**
 * @brief Verifies forward skips, duplicate phases, and backward rejection.
 *
 * @note Terminal entry is accepted only through the terminal arbiter.
 */
TEST(ComputeRunLifecycle, AdvancesMonotonicallyAndSettlesSuccessOnce) {
  ComputeRun run(make_test_submission("phase", 9, 3));

  EXPECT_EQ(run.phase(), ComputeRunPhase::Created);
  EXPECT_TRUE(run.advance_to(ComputeRunPhase::Admitted));
  EXPECT_FALSE(run.advance_to(ComputeRunPhase::Admitted));
  EXPECT_TRUE(run.advance_to(ComputeRunPhase::Running));
  EXPECT_THROW((void)run.advance_to(ComputeRunPhase::Queued), std::logic_error);
  EXPECT_THROW((void)run.advance_to(ComputeRunPhase::Terminal),
               std::invalid_argument);

  EXPECT_TRUE(run.publish_succeeded());
  EXPECT_EQ(run.phase(), ComputeRunPhase::Terminal);
  EXPECT_FALSE(run.advance_to(ComputeRunPhase::CommitPending));
  EXPECT_FALSE(run.publish_succeeded());
  EXPECT_FALSE(
      run.publish_cancelled(ComputeRunCancellationReason::ExplicitRequest));
  EXPECT_FALSE(
      run.publish_failed(std::make_exception_ptr(std::runtime_error("late"))));

  const auto terminal = run.terminal_outcome();
  ASSERT_TRUE(terminal.has_value());
  EXPECT_EQ(terminal->kind, ComputeRunTerminalKind::Succeeded);
  EXPECT_FALSE(terminal->failure);
  EXPECT_FALSE(terminal->cancellation_reason.has_value());
}

/**
 * @brief Verifies failure outcome preserves the original exception object.
 *
 * @note Rethrowing the copied exception_ptr exercises identity-preserving
 * storage instead of reconstructing an error from text.
 */
TEST(ComputeRunLifecycle, PreservesOriginalFailureException) {
  ComputeRun run(make_test_submission("failure", 4, 5));
  const std::exception_ptr failure =
      std::make_exception_ptr(std::runtime_error("compute-run-failure"));

  ASSERT_TRUE(run.publish_failed(failure));
  const auto terminal = run.terminal_outcome();
  ASSERT_TRUE(terminal.has_value());
  ASSERT_EQ(terminal->kind, ComputeRunTerminalKind::Failed);
  ASSERT_TRUE(terminal->failure);
  EXPECT_TRUE(terminal->failure == failure);
  try {
    std::rethrow_exception(terminal->failure);
    FAIL() << "Expected stored ComputeRun failure.";
  } catch (const std::runtime_error& error) {
    EXPECT_STREQ(error.what(), "compute-run-failure");
  }
}

/**
 * @brief Verifies concurrent success/failure/cancellation have one winner.
 *
 * @note Threads share only the Run terminal arbiter and an allocation-free
 * start flag. Every contender is joined before the outcome is inspected.
 */
TEST(ComputeRunLifecycle, ConcurrentTerminalContendersSelectExactlyOne) {
  ComputeRun run(make_test_submission("terminal-race", 6, 7));
  std::atomic_bool start{false};
  std::atomic_int accepted{0};
  std::vector<std::thread> contenders;
  contenders.reserve(12);

  for (int index = 0; index < 12; ++index) {
    contenders.emplace_back([&, index]() {
      while (!start.load(std::memory_order_acquire)) {
        std::this_thread::yield();
      }
      bool won = false;
      switch (index % 3) {
        case 0:
          won = run.publish_succeeded();
          break;
        case 1:
          won = run.publish_failed(
              std::make_exception_ptr(std::runtime_error("race failure")));
          break;
        default:
          won = run.publish_cancelled(
              ComputeRunCancellationReason::ExplicitRequest);
          break;
      }
      if (won) {
        accepted.fetch_add(1, std::memory_order_relaxed);
      }
    });
  }

  start.store(true, std::memory_order_release);
  for (auto& contender : contenders) {
    contender.join();
  }

  EXPECT_EQ(accepted.load(std::memory_order_relaxed), 1);
  EXPECT_TRUE(run.is_terminal());
  EXPECT_TRUE(run.terminal_outcome().has_value());
}

/**
 * @brief Verifies full HP plan, dependency, identity, and temp ownership.
 *
 * @note The plan is constructed after admission and remains address-stable
 * until passive Run destruction.
 */
TEST(ComputeRunStorage, OwnsOneFullHpSubmissionPlanAndTemporarySlots) {
  GraphModel graph("cache/compute-run-plan-storage");
  graph.add_node(make_plan_node(11));
  graph.validate_topology();
  GraphTraversalService traversal;
  ComputeRun run(
      make_test_submission("plan-storage", graph.topology_generation(), 11));
  ASSERT_TRUE(run.advance_to(ComputeRunPhase::Admitted));

  TaskSubmissionPlan& plan = run.emplace_submission_plan(
      graph, traversal, 11, std::vector<Device>{Device::CPU});

  EXPECT_EQ(run.submission_plan(), &plan);
  ASSERT_EQ(plan.execution_order().size(), 1U);
  EXPECT_EQ(plan.execution_order().front(), 11);
  EXPECT_EQ(plan.temp_results().size(), plan.execution_order().size());
  EXPECT_THROW((void)run.emplace_submission_plan(
                   graph, traversal, 11, std::vector<Device>{Device::CPU}),
               std::logic_error);
}

/**
 * @brief Verifies standalone dirty HP staging is installed once under the Run.
 *
 * @note Terminal Runs reject late storage creation so cleanup cannot revive
 * request-local output state.
 */
TEST(ComputeRunStorage, OwnsOneStandaloneDirtyHpWriteBuffer) {
  ComputeRun run(make_test_submission("dirty-storage", 2, 8));
  ASSERT_TRUE(run.advance_to(ComputeRunPhase::Admitted));

  HighPrecisionDirtyWriteBuffer& buffer =
      run.emplace_dirty_hp_write_buffer(false);
  EXPECT_EQ(run.dirty_hp_write_buffer(), &buffer);
  EXPECT_THROW((void)run.emplace_dirty_hp_write_buffer(true), std::logic_error);

  ComputeRun terminal_run(make_test_submission("terminal-storage", 2, 8));
  ASSERT_TRUE(terminal_run.publish_succeeded());
  EXPECT_THROW((void)terminal_run.emplace_dirty_hp_write_buffer(true),
               std::logic_error);
}

/**
 * @brief Verifies a lease retains Run state after its request observer leaves.
 *
 * @note Observer destruction is intentionally not a cancellation signal. The
 * surviving lease proves the descriptor and terminal arbiter remain alive
 * after the original stack frame returns.
 */
TEST(ComputeRunLease,
     RetainsControlAfterObserverDestructionWithoutImplicitCancellation) {
  std::optional<ComputeRunLease> retained_lease;
  std::uint64_t retained_run_id = 0;
  {
    ComputeRun run(make_test_submission("lease-retained", 12, 9));
    retained_run_id = run.descriptor().id().value();
    retained_lease.emplace(run.acquire_lease());
    EXPECT_FALSE(run.is_quiescent());
  }

  ASSERT_TRUE(retained_lease.has_value());
  EXPECT_EQ(retained_lease->descriptor().id().value(), retained_run_id);
  EXPECT_EQ(retained_lease->descriptor().graph_identity(), "lease-retained");
  EXPECT_FALSE(retained_lease->terminal_outcome().has_value());
  retained_lease.reset();
}

/**
 * @brief Verifies terminal publication does not imply physical quiescence.
 *
 * @note The failure is visible through both observer and active lease before
 * the lease releases; quiescence changes only when active lease count reaches
 * zero.
 */
TEST(ComputeRunLease, TerminalStateRemainsNonquiescentUntilLeaseRelease) {
  ComputeRun run(make_test_submission("terminal-nonquiescent", 13, 10));
  {
    ComputeRunLease lease = run.acquire_lease();
    const std::exception_ptr failure =
        std::make_exception_ptr(std::runtime_error("leased failure"));
    ASSERT_TRUE(run.publish_failed(failure));
    EXPECT_TRUE(run.is_terminal());
    EXPECT_FALSE(run.is_quiescent());
    EXPECT_THROW((void)run.acquire_lease(), std::logic_error);
    const auto leased_outcome = lease.terminal_outcome();
    ASSERT_TRUE(leased_outcome.has_value());
    EXPECT_EQ(leased_outcome->kind, ComputeRunTerminalKind::Failed);
    EXPECT_TRUE(leased_outcome->failure == failure);
  }
  EXPECT_TRUE(run.is_quiescent());
  ASSERT_TRUE(run.terminal_outcome().has_value());
  EXPECT_EQ(run.terminal_outcome()->kind, ComputeRunTerminalKind::Failed);
}

/**
 * @brief Verifies lease copy and move operations preserve exact accounting.
 *
 * @note Copies add active ownership, moves transfer it, and replacing a lease
 * releases the previous Run without making the retained Run quiescent.
 */
TEST(ComputeRunLease, CopyMoveAndAssignmentPreserveQuiescenceAccounting) {
  ComputeRun first(make_test_submission("lease-copy-first", 14, 11));
  ComputeRun second(make_test_submission("lease-copy-second", 15, 12));
  {
    ComputeRunLease first_lease = first.acquire_lease();
    ComputeRunLease first_copy = first_lease;
    ComputeRunLease moved_copy = std::move(first_copy);
    ComputeRunLease second_lease = second.acquire_lease();

    EXPECT_FALSE(first.is_quiescent());
    EXPECT_FALSE(second.is_quiescent());
    second_lease = moved_copy;
    EXPECT_TRUE(second.is_quiescent());
    EXPECT_EQ(second_lease.descriptor().id(), first.descriptor().id());

    first_copy = std::move(second_lease);
    EXPECT_EQ(first_copy.descriptor().id(), first.descriptor().id());
    EXPECT_FALSE(first.is_quiescent());
  }
  EXPECT_TRUE(first.is_quiescent());
  EXPECT_TRUE(second.is_quiescent());
}

/**
 * @brief Verifies identical local ids are isolated by Run id and lease route.
 *
 * @note A mismatched or unregistered identity cannot publish failure. The
 * matching Run retains the exact original exception pointer.
 */
TEST(ComputeRunTaskIdentity,
     RepeatedLocalIdsAndFailurePublicationRemainRunScoped) {
  GraphModel first_graph("cache/compute-run-identity-first");
  first_graph.add_node(make_plan_node(21));
  first_graph.validate_topology();
  GraphModel second_graph("cache/compute-run-identity-second");
  second_graph.add_node(make_plan_node(22));
  second_graph.validate_topology();
  GraphTraversalService traversal;
  ComputeRun first(make_test_submission("identity-first",
                                        first_graph.topology_generation(), 21));
  ComputeRun second(make_test_submission(
      "identity-second", second_graph.topology_generation(), 22));
  first.advance_to(ComputeRunPhase::Admitted);
  second.advance_to(ComputeRunPhase::Admitted);
  first.emplace_submission_plan(first_graph, traversal, 21,
                                std::vector<Device>{Device::CPU});
  second.emplace_submission_plan(second_graph, traversal, 22,
                                 std::vector<Device>{Device::CPU});
  ComputeRunLease first_lease = first.acquire_lease();
  ComputeRunLease second_lease = second.acquire_lease();
  const ComputeRunTaskIdentity first_zero = first_lease.task_identity(0);
  const ComputeRunTaskIdentity second_zero = second_lease.task_identity(0);

  EXPECT_NE(first_zero, second_zero);
  EXPECT_TRUE(first_lease.accepts_task_identity(first_zero));
  EXPECT_FALSE(first_lease.accepts_task_identity(second_zero));
  EXPECT_FALSE(
      first_lease.accepts_task_identity(first_lease.task_identity(999)));

  const std::exception_ptr failure =
      std::make_exception_ptr(std::runtime_error("second run failure"));
  EXPECT_FALSE(first_lease.publish_task_failure(second_zero, failure));
  EXPECT_FALSE(first_lease.publish_task_failure(first_lease.task_identity(999),
                                                failure));
  EXPECT_FALSE(first.terminal_outcome().has_value());
  EXPECT_FALSE(second.terminal_outcome().has_value());

  EXPECT_TRUE(second_lease.publish_task_failure(second_zero, failure));
  EXPECT_FALSE(first.terminal_outcome().has_value());
  const auto second_outcome = second.terminal_outcome();
  ASSERT_TRUE(second_outcome.has_value());
  EXPECT_EQ(second_outcome->kind, ComputeRunTerminalKind::Failed);
  EXPECT_TRUE(second_outcome->failure == failure);
}

/**
 * @brief Verifies a terminal accepted task retires its counted scheduler unit.
 *
 * @note The owned callback is accepted before failure publication and invoked
 * by the runtime afterward. Zero execution traces and an untouched temporary
 * slot prove that terminal retirement does not enter planned node work.
 */
TEST(ComputeRunCompletion,
     TerminalAcceptedTaskSettlesUnitWithoutExecutingPlannedWork) {
  GraphModel graph("cache/compute-run-terminal-task");
  graph.add_node(make_plan_node(31));
  graph.validate_topology();
  GraphTraversalService traversal;
  ComputeRun run(
      make_test_submission("terminal-task", graph.topology_generation(), 31));
  ASSERT_TRUE(run.advance_to(ComputeRunPhase::Admitted));
  TaskSubmissionPlan& plan = run.emplace_submission_plan(
      graph, traversal, 31, std::vector<Device>{Device::CPU});
  ASSERT_EQ(plan.size(), 1U);
  ASSERT_EQ(plan.temp_results().size(), 1U);

  ComputeRunLease lease = run.acquire_lease();
  CompletionTrackingRuntime runtime;
  runtime.submit_initial_task_handles({}, 0);
  plan.submit_initial_ready_tasks(lease, runtime);
  ASSERT_EQ(runtime.callbacks_submitted(), 1);
  ASSERT_EQ(runtime.total_units_added(), 1);
  ASSERT_EQ(runtime.tasks_to_complete(), 1);
  EXPECT_THROW(lease.execute_task(lease.task_identity(999), runtime),
               std::invalid_argument);
  EXPECT_EQ(runtime.completion_releases(), 0);
  EXPECT_EQ(runtime.tasks_to_complete(), 1);

  const std::exception_ptr failure =
      std::make_exception_ptr(std::runtime_error("another worker failed"));
  ASSERT_TRUE(run.publish_failed(failure));
  EXPECT_NO_THROW(runtime.wait_for_completion());

  EXPECT_EQ(runtime.callbacks_invoked(), 1);
  EXPECT_EQ(runtime.completion_releases(), 1);
  EXPECT_EQ(runtime.tasks_to_complete(), 0);
  EXPECT_EQ(runtime.planned_execution_events(), 0);
  EXPECT_FALSE(plan.temp_results().front().has_value());
  ASSERT_TRUE(run.terminal_outcome().has_value());
  EXPECT_TRUE(run.terminal_outcome()->failure == failure);
}

/**
 * @brief Verifies a terminal accepted bootstrap retires its counted unit.
 *
 * @note The bootstrap callback is queued before failure publication. Its
 * terminal path must release only the bootstrap unit and must not add planned
 * task units or publish any planned callback.
 */
TEST(ComputeRunCompletion,
     TerminalAcceptedBootstrapSettlesUnitWithoutPublishingPlannedWork) {
  GraphModel graph("cache/compute-run-terminal-bootstrap");
  graph.add_node(make_plan_node(32));
  graph.validate_topology();
  GraphTraversalService traversal;
  ComputeRun run(make_test_submission("terminal-bootstrap",
                                      graph.topology_generation(), 32));
  ASSERT_TRUE(run.advance_to(ComputeRunPhase::Admitted));
  TaskSubmissionPlan& plan = run.emplace_submission_plan(
      graph, traversal, 32, std::vector<Device>{Device::CPU});
  ASSERT_EQ(plan.size(), 1U);

  ComputeRunLease dispatcher_lease = run.acquire_lease();
  CompletionTrackingRuntime runtime;
  runtime.submit_initial_task_handles({}, 0);
  runtime.inc_tasks_to_complete(1);
  ComputeRunLease bootstrap_lease = dispatcher_lease;
  runtime.submit_ready_task_any_thread(
      [lease = std::move(bootstrap_lease), &runtime]() mutable {
        lease.execute_bootstrap(runtime);
      });
  ASSERT_EQ(runtime.callbacks_submitted(), 1);
  ASSERT_EQ(runtime.total_units_added(), 1);
  ASSERT_EQ(runtime.tasks_to_complete(), 1);

  const std::exception_ptr failure = std::make_exception_ptr(
      std::runtime_error("run failed before bootstrap"));
  ASSERT_TRUE(run.publish_failed(failure));
  EXPECT_NO_THROW(runtime.wait_for_completion());

  EXPECT_EQ(runtime.callbacks_submitted(), 1);
  EXPECT_EQ(runtime.callbacks_invoked(), 1);
  EXPECT_EQ(runtime.total_units_added(), 1);
  EXPECT_EQ(runtime.completion_releases(), 1);
  EXPECT_EQ(runtime.tasks_to_complete(), 0);
  EXPECT_EQ(runtime.planned_execution_events(), 0);
  EXPECT_FALSE(plan.temp_results().front().has_value());
  ASSERT_TRUE(run.terminal_outcome().has_value());
  EXPECT_TRUE(run.terminal_outcome()->failure == failure);
}

}  // namespace
}  // namespace ps::compute
