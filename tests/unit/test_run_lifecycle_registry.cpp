#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <future>
#include <memory>
#include <new>
#include <optional>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>

#include "compute/dirty_sibling_commit_gate.hpp"
#include "compute/run_lifecycle_registry.hpp"
#include "photospider/core/graph_error.hpp"

namespace allocation_probe {

/** @brief Disabled current-thread allocation-failure sentinel. */
constexpr std::int64_t kDisabled = -1;

/** @brief Current-thread one-shot allocation countdown. */
thread_local std::int64_t countdown = kDisabled;

/** @brief Whether the armed probe intercepted one allocation. */
thread_local bool fired = false;

/**
 * @brief Arms one current-thread allocation failure.
 * @param allocation_index Zero-based allocation that must fail.
 * @return Nothing.
 * @throws Nothing.
 */
void arm(std::int64_t allocation_index) noexcept {
  countdown = allocation_index;
  fired = false;
}

/**
 * @brief Restores ordinary allocation for the current thread.
 * @return Nothing.
 * @throws Nothing.
 */
void disarm() noexcept {
  countdown = kDisabled;
}

/**
 * @brief Reports whether one armed allocation was attempted.
 * @return True only after the one-shot probe fired.
 * @throws Nothing.
 */
bool did_fire() noexcept {
  return fired;
}

/**
 * @brief Applies the current-thread allocation failure decision.
 * @return Nothing.
 * @throws std::bad_alloc when the countdown reaches zero.
 */
void maybe_fail() {
  if (countdown < 0) {
    return;
  }
  if (countdown == 0) {
    countdown = kDisabled;
    fired = true;
    throw std::bad_alloc{};
  }
  --countdown;
}

}  // namespace allocation_probe

/**
 * @brief Supplies scalar allocation with deterministic test injection.
 * @param size Requested byte count.
 * @return malloc-compatible storage.
 * @throws std::bad_alloc when injected or allocation fails.
 */
void* operator new(std::size_t size) {
  allocation_probe::maybe_fail();
  if (void* memory = std::malloc(size == 0U ? 1U : size)) {
    return memory;
  }
  throw std::bad_alloc{};
}

/**
 * @brief Supplies array allocation through the scalar test operator.
 * @param size Requested byte count.
 * @return malloc-compatible storage.
 * @throws std::bad_alloc when injected or allocation fails.
 */
void* operator new[](std::size_t size) {
  return ::operator new(size);
}

/**
 * @brief Releases scalar storage from the test allocation operator.
 * @param memory Nullable storage.
 * @return Nothing.
 * @throws Nothing.
 */
void operator delete(void* memory) noexcept {
  std::free(memory);
}

/**
 * @brief Releases array storage from the test allocation operator.
 * @param memory Nullable storage.
 * @return Nothing.
 * @throws Nothing.
 */
void operator delete[](void* memory) noexcept {
  std::free(memory);
}

/**
 * @brief Releases sized scalar storage from the test allocation operator.
 * @param memory Nullable storage.
 * @param size Original byte count, unused by free().
 * @return Nothing.
 * @throws Nothing.
 */
void operator delete(void* memory, std::size_t size) noexcept {
  (void)size;
  std::free(memory);
}

/**
 * @brief Releases sized array storage from the test allocation operator.
 * @param memory Nullable storage.
 * @param size Original byte count, unused by free().
 * @return Nothing.
 * @throws Nothing.
 */
void operator delete[](void* memory, std::size_t size) noexcept {
  (void)size;
  std::free(memory);
}

namespace ps::compute {
namespace {

/**
 * @brief Builds one valid standalone child submission for a Graph identity.
 * @param graph_instance_id Exact registered Graph identity.
 * @param target_node_id Distinct request target.
 * @return Valid full HP submission.
 * @throws std::bad_alloc when copied identity storage allocates.
 */
ComputeRunSubmission make_standalone_submission(
    GraphInstanceId graph_instance_id, int target_node_id) {
  return ComputeRunSubmission{
      "registry-graph",
      graph_instance_id,
      GraphRevision::initial(),
      target_node_id,
      ComputeIntent::GlobalHighPrecision,
      ComputeRunQuality::Full,
      ComputeRunQos{ComputeRunQosClass::Throughput, std::nullopt, 1U,
                    std::nullopt},
      SupersessionIdentity{
          SupersessionKey(target_node_id, ComputeIntent::GlobalHighPrecision),
          SupersessionGeneration(1U)}};
}

/**
 * @brief Builds one valid realtime child submission.
 * @param graph_instance_id Exact registered Graph identity.
 * @param target_node_id Shared request target.
 * @param child_intent HP or RT child domain.
 * @param quality Matching Full or Interactive quality.
 * @return Valid child carrying one shared realtime lineage.
 * @throws std::bad_alloc when copied identity storage allocates.
 */
ComputeRunSubmission make_group_submission(GraphInstanceId graph_instance_id,
                                           int target_node_id,
                                           ComputeIntent child_intent,
                                           ComputeRunQuality quality) {
  const SupersessionIdentity identity{
      SupersessionKey(target_node_id, ComputeIntent::RealTimeUpdate),
      SupersessionGeneration(1U)};
  return ComputeRunSubmission{"registry-realtime-graph",
                              graph_instance_id,
                              GraphRevision::initial(),
                              target_node_id,
                              child_intent,
                              quality,
                              ComputeRunQos{ComputeRunQosClass::Throughput,
                                            std::nullopt, 1U, std::nullopt},
                              identity};
}

/**
 * @brief Registers one fresh anchor and returns its retained test owner.
 * @param registry Target lifecycle registry.
 * @param graph_instance_id Exact Graph identity.
 * @return Shared preallocated lifetime anchor.
 * @throws Registry or allocation exceptions unchanged.
 */
std::shared_ptr<GraphLifetimeAnchor> register_graph(
    RunLifecycleRegistry& registry, GraphInstanceId graph_instance_id) {
  auto anchor = std::make_shared<GraphLifetimeAnchor>(graph_instance_id);
  registry.register_graph(anchor);
  return anchor;
}

/**
 * @brief Closes one empty/settled Graph and marks its synthetic lanes retired.
 * @param registry Target lifecycle registry.
 * @param anchor Matching test lifetime anchor.
 * @return Nothing.
 * @throws Registry close exceptions unchanged.
 */
void close_graph(RunLifecycleRegistry& registry,
                 const std::shared_ptr<GraphLifetimeAnchor>& anchor) {
  registry.close_graph(anchor->graph_instance_id(),
                       ComputeRunCancellationReason::GraphClose);
  anchor->mark_retired();
}

TEST(GraphCloseCoordinator, SelectsOneOwnerAndJoinsOneGeneration) {
  GraphCloseCoordinator coordinator;
  EXPECT_EQ(coordinator.begin(), GraphCloseCoordinator::Role::Owner);
  EXPECT_EQ(coordinator.begin(), GraphCloseCoordinator::Role::Joiner);
  EXPECT_TRUE(coordinator.started());
  EXPECT_FALSE(coordinator.completed());

  auto waiter = std::async(std::launch::async, [&coordinator]() {
    coordinator.wait_for_success();
    return true;
  });
  EXPECT_EQ(waiter.wait_for(std::chrono::milliseconds(25)),
            std::future_status::timeout);
  coordinator.complete_success();
  EXPECT_TRUE(waiter.get());
  EXPECT_TRUE(coordinator.completed());
  EXPECT_THROW(coordinator.complete_success(), std::logic_error);
}

TEST(RunLifecycleRegistry, RollsBackCandidatesAndNeverReusesIdentity) {
  ExecutionLifecycleTelemetry telemetry;
  RunLifecycleRegistry registry(telemetry);
  auto first_anchor = register_graph(registry, GraphInstanceId{101U});

  std::uint64_t first_candidate_id = 0U;
  {
    RunLifecycleAdmissionCandidate candidate =
        registry.begin_graph_admission(first_anchor->graph_instance_id());
    first_candidate_id = candidate.id();
    EXPECT_TRUE(candidate.active());
    EXPECT_EQ(registry.counters().pending_candidate_count, 1U);
  }
  EXPECT_EQ(registry.counters().pending_candidate_count, 0U);
  close_graph(registry, first_anchor);

  auto second_anchor = register_graph(registry, GraphInstanceId{102U});
  RunLifecycleAdmissionCandidate second =
      registry.begin_graph_admission(second_anchor->graph_instance_id());
  EXPECT_GT(second.id(), first_candidate_id);
  second = RunLifecycleAdmissionCandidate{};
  close_graph(registry, second_anchor);
}

TEST(RunLifecycleRegistry, InstallsAndFinalizesStandaloneBeforeRowRemoval) {
  static_assert(
      !std::is_move_assignable_v<RunLifecycleAdmissionHandle>,
      "an unresolved finalization obligation must not be overwritten");
  ExecutionLifecycleTelemetry telemetry;
  RunLifecycleRegistry registry(telemetry);
  auto anchor = register_graph(registry, GraphInstanceId{201U});
  ComputeRun run(make_standalone_submission(anchor->graph_instance_id(), 7));
  auto cancellation = std::make_shared<ComputeRequestCancellationSource>();
  cancellation->attach(run);

  RunLifecycleAdmissionHandle handle = registry.commit_standalone(
      registry.begin_graph_admission(anchor->graph_instance_id()),
      run.acquire_lease(), cancellation);
  EXPECT_TRUE(handle.active());
  EXPECT_TRUE(registry.permits_visible_commit(anchor->graph_instance_id(),
                                              run.descriptor().id()));
  EXPECT_EQ(registry.counters().admitted_standalone_run_count, 1U);

  const std::uint64_t bundle_id = handle.bundle_id();
  RunLifecycleAdmissionHandle moved_handle(std::move(handle));
  EXPECT_FALSE(handle.active());
  EXPECT_THROW((void)handle.bundle_id(), std::logic_error);
  EXPECT_TRUE(moved_handle.active());
  EXPECT_EQ(moved_handle.bundle_id(), bundle_id);

  ASSERT_TRUE(run.publish_succeeded());
  registry.finalize_admission(moved_handle);
  EXPECT_FALSE(moved_handle.active());
  EXPECT_EQ(registry.counters().admitted_standalone_run_count, 0U);
  registry.close_graph(anchor->graph_instance_id(),
                       ComputeRunCancellationReason::GraphClose);
  EXPECT_FALSE(anchor->retired())
      << "registry row removal must precede lane/runtime retirement";
  anchor->mark_retired();

  const ExecutionLifecyclePage page = telemetry.snapshot(0U, 64U);
  EXPECT_TRUE(std::any_of(page.records.begin(), page.records.end(),
                          [](const ExecutionLifecycleEvent& event) {
                            return event.kind ==
                                   ExecutionLifecycleEventKind::GraphRowRemoved;
                          }));
}

TEST(RunLifecycleRegistry, RealtimeBundleAppearsAndRetiresAtomically) {
  ExecutionLifecycleTelemetry telemetry;
  RunLifecycleRegistry registry(telemetry);
  auto anchor = register_graph(registry, GraphInstanceId{301U});
  auto cancellation = std::make_shared<ComputeRequestCancellationSource>();
  RunGroup group(make_group_submission(anchor->graph_instance_id(), 9,
                                       ComputeIntent::GlobalHighPrecision,
                                       ComputeRunQuality::Full),
                 make_group_submission(anchor->graph_instance_id(), 9,
                                       ComputeIntent::RealTimeUpdate,
                                       ComputeRunQuality::Interactive),
                 cancellation);
  cancellation->attach(group.hp_run());
  cancellation->attach(group.rt_run());

  RunLifecycleAdmissionHandle handle = registry.commit_realtime_group(
      registry.begin_graph_admission(anchor->graph_instance_id()), group.id(),
      group.hp_run().acquire_lease(), group.rt_run().acquire_lease(),
      cancellation, group.sibling_commit_gate());
  const ExecutionLifecycleCounters admitted = registry.counters();
  EXPECT_EQ(admitted.admitted_standalone_run_count, 0U);
  EXPECT_EQ(admitted.admitted_run_group_count, 1U);
  EXPECT_EQ(admitted.admitted_child_run_count, 2U);
  EXPECT_TRUE(registry.permits_visible_commit(
      anchor->graph_instance_id(), group.hp_run().descriptor().id()));
  EXPECT_TRUE(registry.permits_visible_commit(
      anchor->graph_instance_id(), group.rt_run().descriptor().id()));

  ASSERT_TRUE(group.hp_run().publish_succeeded());
  ASSERT_TRUE(group.rt_run().publish_succeeded());
  group.release_lifecycle_leases();
  registry.finalize_admission(handle);
  const ExecutionLifecycleCounters settled = registry.counters();
  EXPECT_EQ(settled.admitted_run_group_count, 0U);
  EXPECT_EQ(settled.admitted_child_run_count, 0U);
  close_graph(registry, anchor);
}

TEST(RunLifecycleRegistry, AdmissionAndCloseHaveExactTwoRaceOutcomes) {
  {
    ExecutionLifecycleTelemetry telemetry;
    RunLifecycleRegistry registry(telemetry);
    auto anchor = register_graph(registry, GraphInstanceId{401U});
    ComputeRun run(make_standalone_submission(anchor->graph_instance_id(), 11));
    auto cancellation = std::make_shared<ComputeRequestCancellationSource>();
    cancellation->attach(run);
    RunLifecycleAdmissionHandle handle = registry.commit_standalone(
        registry.begin_graph_admission(anchor->graph_instance_id()),
        run.acquire_lease(), cancellation);

    const std::uint64_t generation = registry.begin_graph_close(
        anchor->graph_instance_id(), ComputeRunCancellationReason::GraphClose);
    EXPECT_NE(generation, 0U);
    ASSERT_TRUE(run.terminal_outcome().has_value());
    EXPECT_EQ(run.terminal_outcome()->kind, ComputeRunTerminalKind::Cancelled);
    EXPECT_FALSE(registry.permits_visible_commit(anchor->graph_instance_id(),
                                                 run.descriptor().id()));
    registry.finalize_admission(handle);
    registry.finish_graph_close(anchor->graph_instance_id());
    anchor->mark_retired();
  }

  {
    ExecutionLifecycleTelemetry telemetry;
    RunLifecycleRegistry registry(telemetry);
    auto anchor = register_graph(registry, GraphInstanceId{402U});
    ComputeRun run(make_standalone_submission(anchor->graph_instance_id(), 12));
    auto cancellation = std::make_shared<ComputeRequestCancellationSource>();
    cancellation->attach(run);
    RunLifecycleAdmissionCandidate candidate =
        registry.begin_graph_admission(anchor->graph_instance_id());
    const std::uint64_t generation = registry.begin_graph_close(
        anchor->graph_instance_id(), ComputeRunCancellationReason::GraphClose);
    EXPECT_NE(generation, 0U);
    ASSERT_TRUE(candidate.cancellation_reason().has_value());
    EXPECT_EQ(*candidate.cancellation_reason(),
              ComputeRunCancellationReason::GraphClose);
    EXPECT_THROW((void)registry.commit_standalone(
                     std::move(candidate), run.acquire_lease(), cancellation),
                 GraphError);
    EXPECT_FALSE(run.terminal_outcome().has_value());
    registry.finish_graph_close(anchor->graph_instance_id());
    anchor->mark_retired();
  }
}

TEST(RunLifecycleRegistry,
     GraphCloseUsesPreallocatedFanoutAndContainsCallbackFailure) {
  ExecutionLifecycleTelemetry telemetry;
  RunLifecycleRegistry registry(telemetry);
  auto anchor = register_graph(registry, GraphInstanceId{451U});
  ComputeRun first(make_standalone_submission(anchor->graph_instance_id(), 21));
  ComputeRun second(
      make_standalone_submission(anchor->graph_instance_id(), 22));
  auto first_cancellation =
      std::make_shared<ComputeRequestCancellationSource>();
  auto second_cancellation =
      std::make_shared<ComputeRequestCancellationSource>();
  first_cancellation->attach(first);
  second_cancellation->attach(second);

  const std::exception_ptr callback_failure =
      std::make_exception_ptr(std::runtime_error("injected close cleanup"));
  ComputeRunCancellationRegistration failing_registration;
  {
    ComputeRunLease lease = first.acquire_lease();
    failing_registration = lease.register_cancellation_notification(
        [callback_failure](ComputeRunCancellationReason) {
          std::rethrow_exception(callback_failure);
        });
  }

  RunLifecycleAdmissionHandle first_handle = registry.commit_standalone(
      registry.begin_graph_admission(anchor->graph_instance_id()),
      first.acquire_lease(), first_cancellation);
  RunLifecycleAdmissionHandle second_handle = registry.commit_standalone(
      registry.begin_graph_admission(anchor->graph_instance_id()),
      second.acquire_lease(), second_cancellation);

  std::exception_ptr transition_failure;
  std::uint64_t generation = 0U;
  allocation_probe::arm(0);
  try {
    generation = registry.begin_graph_close(
        anchor->graph_instance_id(), ComputeRunCancellationReason::GraphClose);
  } catch (...) {
    transition_failure = std::current_exception();
  }
  const bool attempted_allocation = allocation_probe::did_fire();
  allocation_probe::disarm();

  EXPECT_FALSE(attempted_allocation);
  ASSERT_FALSE(transition_failure);
  EXPECT_NE(generation, 0U);
  ASSERT_TRUE(first.terminal_outcome().has_value());
  ASSERT_TRUE(second.terminal_outcome().has_value());
  EXPECT_EQ(first.terminal_outcome()->kind, ComputeRunTerminalKind::Cancelled);
  EXPECT_EQ(second.terminal_outcome()->kind, ComputeRunTerminalKind::Cancelled);

  registry.finalize_admission(first_handle);
  registry.finalize_admission(second_handle);
  registry.finish_graph_close(anchor->graph_instance_id());
  anchor->mark_retired();

  const ExecutionLifecyclePage page = telemetry.snapshot(0U, 64U);
  EXPECT_EQ(
      std::count_if(page.records.begin(), page.records.end(),
                    [](const ExecutionLifecycleEvent& event) {
                      return event.kind ==
                             ExecutionLifecycleEventKind::CancellationRequested;
                    }),
      2);
}

TEST(RunLifecycleRegistry,
     ProcessShutdownUsesPreallocatedFanoutAfterStoppingTransition) {
  ExecutionLifecycleTelemetry telemetry;
  RunLifecycleRegistry registry(telemetry);
  auto anchor = register_graph(registry, GraphInstanceId{452U});
  ComputeRun run(make_standalone_submission(anchor->graph_instance_id(), 23));
  auto cancellation = std::make_shared<ComputeRequestCancellationSource>();
  cancellation->attach(run);
  RunLifecycleAdmissionHandle handle = registry.commit_standalone(
      registry.begin_graph_admission(anchor->graph_instance_id()),
      run.acquire_lease(), cancellation);

  std::exception_ptr transition_failure;
  std::uint64_t generation = 0U;
  allocation_probe::arm(0);
  try {
    generation = registry.begin_service_shutdown();
  } catch (...) {
    transition_failure = std::current_exception();
  }
  const bool attempted_allocation = allocation_probe::did_fire();
  allocation_probe::disarm();

  EXPECT_FALSE(attempted_allocation);
  ASSERT_FALSE(transition_failure);
  EXPECT_NE(generation, 0U);
  ASSERT_TRUE(run.terminal_outcome().has_value());
  EXPECT_EQ(run.terminal_outcome()->kind, ComputeRunTerminalKind::Cancelled);
  EXPECT_EQ(run.terminal_outcome()->cancellation_reason,
            ComputeRunCancellationReason::ProcessShutdown);

  registry.finalize_admission(handle);
  registry.finish_graph_close(anchor->graph_instance_id());
  anchor->mark_retired();
  registry.wait_until_empty();
  EXPECT_NE(registry.mark_service_stopped({}), 0U);
}

TEST(RunLifecycleRegistry, ProcessShutdownIsIdempotentAndStopsAfterEmptyRows) {
  ExecutionLifecycleTelemetry telemetry;
  RunLifecycleRegistry registry(telemetry);
  auto first = register_graph(registry, GraphInstanceId{501U});
  auto second = register_graph(registry, GraphInstanceId{502U});

  const std::uint64_t generation = registry.begin_service_shutdown();
  EXPECT_NE(generation, 0U);
  EXPECT_EQ(registry.begin_service_shutdown(), generation);
  EXPECT_FALSE(registry.accepting());
  EXPECT_EQ(registry.counters().closing_graph_count, 2U);
  registry.finish_graph_close(first->graph_instance_id());
  first->mark_retired();
  registry.finish_graph_close(second->graph_instance_id());
  second->mark_retired();
  registry.wait_until_empty();

  const std::uint64_t stopped = registry.mark_service_stopped({});
  EXPECT_NE(stopped, 0U);
  EXPECT_EQ(registry.mark_service_stopped({}), stopped);
  const ExecutionLifecyclePage page = telemetry.snapshot(0U, 64U);
  EXPECT_EQ(page.service_state, ExecutionLifecycleServiceState::Stopped);
  EXPECT_EQ(page.shutdown_generation, generation);
  ASSERT_FALSE(page.records.empty());
  EXPECT_EQ(page.records.back().kind,
            ExecutionLifecycleEventKind::ServiceStopped);
}

}  // namespace
}  // namespace ps::compute
