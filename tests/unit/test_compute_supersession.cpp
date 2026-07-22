#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <future>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "compute/compute_request_coordinator.hpp"
#include "compute/run_group.hpp"
#include "graph/graph_model.hpp"           // NOLINT(build/include_subdir)
#include "graph/graph_state_executor.hpp"  // NOLINT(build/include_subdir)
#include "runtime/graph_runtime.hpp"       // NOLINT(build/include_subdir)

namespace ps::compute {
namespace {

/** @brief Bounded diagnostic wait used without ordering sleeps. */
constexpr auto kTestTimeout = std::chrono::seconds(10);

/**
 * @brief Waits for an atomic predicate with a bounded diagnostic deadline.
 * @tparam Predicate Nonblocking predicate callable.
 * @param predicate Condition published by an executor callback.
 * @return True when the condition becomes true before the deadline.
 * @throws Nothing directly.
 * @note Ordering comes from promises/executor FIFO; this helper only prevents
 * a regression from hanging the maintained test process indefinitely.
 */
template <typename Predicate>
bool wait_for_predicate(Predicate&& predicate) {
  const auto deadline = std::chrono::steady_clock::now() + kTestTimeout;
  while (std::chrono::steady_clock::now() < deadline) {
    if (predicate()) {
      return true;
    }
    std::this_thread::yield();
  }
  return predicate();
}

/**
 * @brief Deterministic test clock representing one publication tick.
 * @throws Nothing for construction and advancement.
 * @note Production supersession is event ordered and does not read wall time;
 * this injected clock proves the exact fixed-duration storm cardinality.
 */
class VirtualMonotonicClock final {
 public:
  /**
   * @brief Advances one deterministic publication tick.
   * @return Nothing.
   * @throws Nothing.
   */
  void advance() noexcept { ++ticks_; }

  /**
   * @brief Returns the exact number of simulated publication ticks.
   * @return Monotonic tick count.
   * @throws Nothing.
   */
  std::uint64_t ticks() const noexcept { return ticks_; }

 private:
  /** @brief Monotonic simulated publication count. */
  std::uint64_t ticks_ = 0;
};

/**
 * @brief Records exact-once terminal settlement for one storm generation.
 * @param settlements Per-generation atomic claimant counts.
 * @param terminal_count Total first terminal claimants.
 * @param duplicates Duplicate claimant diagnostic count.
 * @param index Generation-local zero-based slot.
 * @return Nothing.
 * @throws Nothing.
 */
void settle_generation(std::vector<std::atomic<unsigned int>>& settlements,
                       std::atomic<std::size_t>& terminal_count,
                       std::atomic<std::size_t>& duplicates,
                       std::size_t index) noexcept {
  if (settlements[index].fetch_add(1, std::memory_order_acq_rel) == 0) {
    terminal_count.fetch_add(1, std::memory_order_relaxed);
  } else {
    duplicates.fetch_add(1, std::memory_order_relaxed);
  }
}

/**
 * @brief Runs one fixed ten-minute-equivalent same-key latest-wins storm.
 * @param updates_per_second Injected 30 Hz or 60 Hz rate.
 * @return Nothing; GoogleTest assertions report bound or settlement failures.
 * @throws Allocation, executor, or synchronization failures unchanged.
 * @note Generation zero materializes and blocks. Every later publication is
 * linearized before release, so only the final mailbox value may materialize.
 */
void run_fixed_same_key_storm(std::size_t updates_per_second) {
  constexpr std::size_t kSimulatedSeconds = 10U * 60U;
  const std::size_t generation_count = updates_per_second * kSimulatedSeconds;
  ASSERT_TRUE(updates_per_second == 30U || updates_per_second == 60U);

  GraphModel model(std::filesystem::path{});
  GraphStateExecutor graph_state(model);
  GraphStateExecutor compute_lane(
      model, GraphStateExecutor::kDefaultQueueCapacity,
      GraphStateExecutor::CapacityMode::TotalAdmission);
  ComputeRequestCoordinator coordinator(graph_state, compute_lane);
  const SupersessionKey key(7, ComputeIntent::GlobalHighPrecision);

  std::vector<std::atomic<unsigned int>> settlements(generation_count);
  for (auto& settlement : settlements) {
    settlement.store(0, std::memory_order_relaxed);
  }
  std::atomic<std::size_t> terminal_count{0};
  std::atomic<std::size_t> duplicate_settlements{0};
  std::atomic<std::size_t> materialized_count{0};
  std::atomic<bool> final_was_current{false};
  std::atomic<std::uint64_t> visible_generation{0};
  std::promise<void> first_entered;
  std::future<void> first_entered_future = first_entered.get_future();
  std::promise<void> release_first;
  const std::shared_future<void> first_release =
      release_first.get_future().share();
  std::promise<void> final_settled;
  std::future<void> final_settled_future = final_settled.get_future();
  auto first_source = std::make_shared<ComputeRequestCancellationSource>();
  VirtualMonotonicClock clock;

  for (std::size_t index = 0; index < generation_count; ++index) {
    ComputeRequestCoordinator::PreparedCandidate prepared =
        coordinator.prepare(key);
    const SupersessionIdentity identity = prepared.identity();
    const bool first = index == 0;
    const bool last = index + 1 == generation_count;
    std::shared_ptr<ComputeRequestCancellationSource> source =
        first ? first_source
              : std::make_shared<ComputeRequestCancellationSource>();
    coordinator.publish(
        std::move(prepared), std::move(source),
        [&, index, identity, first, last] {
          materialized_count.fetch_add(1, std::memory_order_relaxed);
          if (first) {
            first_entered.set_value();
            first_release.wait();
          }
          const bool current = coordinator.is_current(identity);
          if (current) {
            visible_generation.store(identity.generation.value(),
                                     std::memory_order_release);
          }
          if (last) {
            final_was_current.store(current, std::memory_order_release);
          }
          settle_generation(settlements, terminal_count, duplicate_settlements,
                            index);
          if (last) {
            final_settled.set_value();
          }
        },
        [&, index] {
          settle_generation(settlements, terminal_count, duplicate_settlements,
                            index);
        },
        [&, index](std::exception_ptr) {
          settle_generation(settlements, terminal_count, duplicate_settlements,
                            index);
        });
    clock.advance();
    if (first) {
      graph_state.submit([](GraphModel&) {}).get();
      ASSERT_EQ(first_entered_future.wait_for(kTestTimeout),
                std::future_status::ready);
    }
  }

  graph_state.submit([](GraphModel&) {}).get();
  const ComputeRequestCoordinator::Snapshot blocked = coordinator.snapshot();
  EXPECT_EQ(clock.ticks(), generation_count);
  EXPECT_EQ(blocked.lineage_rows, 1U);
  EXPECT_EQ(blocked.reserved_tickets, 1U);
  EXPECT_EQ(blocked.active_candidates, 1U);
  EXPECT_EQ(blocked.pending_candidates, 1U);
  EXPECT_EQ(blocked.provisional_adopters, 0U);
  EXPECT_EQ(blocked.lane_admitted_units, 1U);
  EXPECT_EQ(terminal_count.load(std::memory_order_acquire),
            generation_count - 2U);

  release_first.set_value();
  ASSERT_EQ(final_settled_future.wait_for(kTestTimeout),
            std::future_status::ready);
  ASSERT_TRUE(wait_for_predicate(
      [&] { return coordinator.snapshot().lineage_rows == 0U; }));

  EXPECT_EQ(materialized_count.load(std::memory_order_acquire), 2U);
  EXPECT_EQ(terminal_count.load(std::memory_order_acquire), generation_count);
  EXPECT_EQ(duplicate_settlements.load(std::memory_order_acquire), 0U);
  EXPECT_TRUE(final_was_current.load(std::memory_order_acquire));
  EXPECT_EQ(visible_generation.load(std::memory_order_acquire),
            generation_count);
  ASSERT_TRUE(first_source->accepted_reason().has_value());
  EXPECT_EQ(*first_source->accepted_reason(),
            ComputeRunCancellationReason::Superseded);
  for (const auto& settlement : settlements) {
    EXPECT_EQ(settlement.load(std::memory_order_acquire), 1U);
  }
  EXPECT_EQ(coordinator.snapshot().lane_admitted_units, 0U);

  coordinator.stop_admission();
  compute_lane.close_and_drain();
  graph_state.close_and_drain();
}

/**
 * @brief Builds one valid RunGroup child submission.
 * @param child_intent HP or RT child domain.
 * @param quality Matching full or interactive child quality.
 * @param identity Shared realtime request lineage.
 * @return Valid child submission.
 * @throws std::bad_alloc when graph identity storage allocates.
 */
ComputeRunSubmission make_group_submission(
    ComputeIntent child_intent, ComputeRunQuality quality,
    const SupersessionIdentity& identity) {
  return ComputeRunSubmission{"run-group-test",
                              GraphInstanceId(1),
                              GraphRevision(1),
                              identity.key.target_node_id(),
                              child_intent,
                              quality,
                              ComputeRunQos{ComputeRunQosClass::Throughput,
                                            std::nullopt, 1, std::nullopt},
                              identity};
}

/**
 * @brief Verifies intent canonicalization and checked maximum generation use.
 * @return Nothing; GoogleTest assertions report identity/overflow failures.
 * @throws Standard construction failures unchanged to GoogleTest.
 */
TEST(SupersessionIdentity,
     NormalizesLegacyHpAndChecksGenerationExhaustionWithoutWrap) {
  EXPECT_EQ(normalize_supersession_intent(std::nullopt),
            ComputeIntent::GlobalHighPrecision);
  EXPECT_EQ(normalize_supersession_intent(ComputeIntent::GlobalHighPrecision),
            ComputeIntent::GlobalHighPrecision);
  EXPECT_EQ(normalize_supersession_intent(ComputeIntent::RealTimeUpdate),
            ComputeIntent::RealTimeUpdate);

  const SupersessionKey legacy_key(3,
                                   normalize_supersession_intent(std::nullopt));
  const SupersessionKey explicit_key(3, ComputeIntent::GlobalHighPrecision);
  const SupersessionKey realtime_key(3, ComputeIntent::RealTimeUpdate);
  EXPECT_EQ(legacy_key, explicit_key);
  EXPECT_FALSE(legacy_key == realtime_key);
  EXPECT_THROW((void)SupersessionKey(-1, ComputeIntent::GlobalHighPrecision),
               std::invalid_argument);
  EXPECT_THROW((void)SupersessionGeneration(0), std::invalid_argument);

  SupersessionGenerationAllocator allocator(
      std::numeric_limits<std::uint64_t>::max());
  EXPECT_EQ(allocator.allocate().value(),
            std::numeric_limits<std::uint64_t>::max());
  EXPECT_THROW((void)allocator.allocate(), std::overflow_error);
}

/**
 * @brief Verifies wake-pending reuses one node and preserves peer FIFO order.
 * @return Nothing; GoogleTest assertions report capacity or order failures.
 * @throws Executor and synchronization failures unchanged to GoogleTest.
 */
TEST(GraphStateExecutorContinuation,
     WakePendingReusesOneNodeAndYieldsToQueuedPeerAtTotalCapacity) {
  GraphModel model(std::filesystem::path{});
  GraphStateExecutor lane(model, 2,
                          GraphStateExecutor::CapacityMode::TotalAdmission);
  std::mutex order_mutex;
  std::vector<int> order;
  std::atomic<int> turn{0};
  std::promise<void> first_entered;
  auto first_entered_future = first_entered.get_future();
  std::promise<void> release_first;
  const std::shared_future<void> first_release =
      release_first.get_future().share();
  std::promise<void> second_finished;
  auto second_finished_future = second_finished.get_future();

  GraphStateExecutor::ContinuationTicket ticket = lane.reserve_continuation(
      [&](const GraphStateExecutor::ContinuationTicket&) {
        const int current = turn.fetch_add(1, std::memory_order_relaxed);
        {
          std::lock_guard<std::mutex> lock(order_mutex);
          order.push_back(current == 0 ? 1 : 3);
        }
        if (current == 0) {
          first_entered.set_value();
          first_release.wait();
          return GraphStateExecutor::ContinuationAction::Park;
        }
        second_finished.set_value();
        return GraphStateExecutor::ContinuationAction::Retire;
      });
  ASSERT_TRUE(ticket.wake());
  ASSERT_EQ(first_entered_future.wait_for(kTestTimeout),
            std::future_status::ready);

  auto peer = lane.submit([&](GraphModel&) {
    std::lock_guard<std::mutex> lock(order_mutex);
    order.push_back(2);
  });
  for (int wake = 0; wake < 128; ++wake) {
    EXPECT_TRUE(ticket.wake());
  }
  EXPECT_EQ(lane.admitted_units(), 2U);
  release_first.set_value();
  peer.get();
  ASSERT_EQ(second_finished_future.wait_for(kTestTimeout),
            std::future_status::ready);
  ASSERT_TRUE(wait_for_predicate([&] { return lane.admitted_units() == 0U; }));

  EXPECT_EQ(order, (std::vector<int>{1, 2, 3}));
  EXPECT_EQ(turn.load(std::memory_order_acquire), 2);
  EXPECT_FALSE(ticket.wake());
  EXPECT_FALSE(ticket.retire());
  lane.close_and_drain();
}

/**
 * @brief Verifies the exact 64-unit bound and close-owned parked retirement.
 * @return Nothing; GoogleTest assertions report oversell or drainage failures.
 * @throws Executor and synchronization failures unchanged to GoogleTest.
 */
TEST(GraphStateExecutorContinuation,
     AllSixtyFourAdmissionsRejectBlockedSixtyFifthAndDrainParkedTickets) {
  GraphModel model(std::filesystem::path{});
  GraphStateExecutor lane(model, GraphStateExecutor::kDefaultQueueCapacity,
                          GraphStateExecutor::CapacityMode::TotalAdmission);
  std::promise<void> active_entered;
  auto active_entered_future = active_entered.get_future();
  std::promise<void> release_active;
  const std::shared_future<void> active_release =
      release_active.get_future().share();
  auto active = lane.submit([&](GraphModel&) {
    active_entered.set_value();
    active_release.wait();
  });
  ASSERT_EQ(active_entered_future.wait_for(kTestTimeout),
            std::future_status::ready);

  std::atomic<std::size_t> close_turns{0};
  std::vector<GraphStateExecutor::ContinuationTicket> tickets;
  tickets.reserve(GraphStateExecutor::kDefaultQueueCapacity - 1U);
  for (std::size_t index = 1; index < GraphStateExecutor::kDefaultQueueCapacity;
       ++index) {
    tickets.push_back(lane.reserve_continuation(
        [&](const GraphStateExecutor::ContinuationTicket&) {
          close_turns.fetch_add(1, std::memory_order_relaxed);
          return GraphStateExecutor::ContinuationAction::Retire;
        }));
  }
  EXPECT_EQ(lane.admitted_units(), GraphStateExecutor::kDefaultQueueCapacity);

  std::promise<void> blocked_entered;
  auto blocked_entered_future = blocked_entered.get_future();
  auto blocked = std::async(std::launch::async, [&] {
    blocked_entered.set_value();
    return lane.reserve_continuation(
        [](const GraphStateExecutor::ContinuationTicket&) {
          return GraphStateExecutor::ContinuationAction::Retire;
        });
  });
  ASSERT_EQ(blocked_entered_future.wait_for(kTestTimeout),
            std::future_status::ready);
  EXPECT_EQ(blocked.wait_for(std::chrono::milliseconds(0)),
            std::future_status::timeout);

  lane.stop_admission();
  ASSERT_EQ(blocked.wait_for(kTestTimeout), std::future_status::ready);
  EXPECT_THROW((void)blocked.get(), std::runtime_error);
  release_active.set_value();
  active.get();
  lane.close_and_drain();
  EXPECT_EQ(close_turns.load(std::memory_order_acquire), tickets.size());
  EXPECT_EQ(lane.admitted_units(), 0U);
  for (const auto& ticket : tickets) {
    EXPECT_FALSE(ticket.wake());
  }
}

/**
 * @brief Verifies concurrent same-key adopters reserve one shared ticket.
 * @return Nothing; GoogleTest assertions report ticket/generation failures.
 * @throws Allocation and synchronization failures unchanged to GoogleTest.
 */
TEST(ComputeRequestCoordinator,
     ConcurrentSameKeyPreparationSharesExactlyOneReservedTicket) {
  GraphModel model(std::filesystem::path{});
  GraphStateExecutor graph_state(model);
  GraphStateExecutor compute_lane(
      model, 1, GraphStateExecutor::CapacityMode::TotalAdmission);
  ComputeRequestCoordinator coordinator(graph_state, compute_lane);
  const SupersessionKey key(9, ComputeIntent::GlobalHighPrecision);
  constexpr std::size_t kPreparerCount = 16;
  std::promise<void> release;
  const std::shared_future<void> start = release.get_future().share();
  std::vector<std::future<ComputeRequestCoordinator::PreparedCandidate>>
      workers;
  workers.reserve(kPreparerCount);
  for (std::size_t index = 0; index < kPreparerCount; ++index) {
    workers.push_back(std::async(std::launch::async, [&, start] {
      start.wait();
      return coordinator.prepare(key);
    }));
  }
  release.set_value();

  std::vector<ComputeRequestCoordinator::PreparedCandidate> prepared;
  prepared.reserve(kPreparerCount);
  std::set<std::uint64_t> generations;
  for (auto& worker : workers) {
    prepared.push_back(worker.get());
    generations.insert(prepared.back().identity().generation.value());
  }
  const ComputeRequestCoordinator::Snapshot snapshot = coordinator.snapshot();
  EXPECT_EQ(snapshot.lineage_rows, 1U);
  EXPECT_EQ(snapshot.reserved_tickets, 1U);
  EXPECT_EQ(snapshot.provisional_adopters, kPreparerCount);
  EXPECT_EQ(snapshot.lane_admitted_units, 1U);
  EXPECT_EQ(generations.size(), kPreparerCount);

  prepared.clear();
  ASSERT_TRUE(wait_for_predicate([&] {
    const auto current = coordinator.snapshot();
    return current.lineage_rows == 0U && current.lane_admitted_units == 0U;
  }));
  EXPECT_EQ(coordinator.snapshot().lane_admitted_units, 0U);
  coordinator.stop_admission();
  compute_lane.close_and_drain();
  graph_state.close_and_drain();
}

/**
 * @brief Verifies same-key wake storms yield to peer and ordinary FIFO work.
 * @return Nothing; GoogleTest assertions report fairness or ledger failures.
 * @throws Allocation and synchronization failures unchanged to GoogleTest.
 */
TEST(ComputeRequestCoordinator,
     PeerAndOrdinaryFifoProgressSurviveUnboundedSameKeyWake) {
  GraphModel model(std::filesystem::path{});
  GraphStateExecutor graph_state(model);
  GraphStateExecutor compute_lane(
      model, 3, GraphStateExecutor::CapacityMode::TotalAdmission);
  ComputeRequestCoordinator coordinator(graph_state, compute_lane);
  const SupersessionKey first_key(1, ComputeIntent::GlobalHighPrecision);
  const SupersessionKey peer_key(2, ComputeIntent::GlobalHighPrecision);
  std::mutex order_mutex;
  std::vector<int> order;
  std::promise<void> first_entered;
  auto first_entered_future = first_entered.get_future();
  std::promise<void> release_first;
  const std::shared_future<void> first_release =
      release_first.get_future().share();
  std::promise<void> latest_finished;
  auto latest_finished_future = latest_finished.get_future();

  auto publish = [&](const SupersessionKey& key, int marker, bool block,
                     bool final) {
    auto prepared = coordinator.prepare(key);
    coordinator.publish(
        std::move(prepared),
        std::make_shared<ComputeRequestCancellationSource>(),
        [&, marker, block, final] {
          {
            std::lock_guard<std::mutex> lock(order_mutex);
            order.push_back(marker);
          }
          if (block) {
            first_entered.set_value();
            first_release.wait();
          }
          if (final) {
            latest_finished.set_value();
          }
        },
        [] {}, [](std::exception_ptr) {});
  };

  publish(first_key, 1, true, false);
  graph_state.submit([](GraphModel&) {}).get();
  ASSERT_EQ(first_entered_future.wait_for(kTestTimeout),
            std::future_status::ready);
  for (int replacement = 0; replacement < 1024; ++replacement) {
    publish(first_key, 4, false, replacement == 1023);
  }
  publish(peer_key, 2, false, false);
  graph_state.submit([](GraphModel&) {}).get();
  auto ordinary = compute_lane.submit([&](GraphModel&) {
    std::lock_guard<std::mutex> lock(order_mutex);
    order.push_back(3);
  });
  EXPECT_EQ(coordinator.snapshot().lane_admitted_units, 3U);

  release_first.set_value();
  ordinary.get();
  ASSERT_EQ(latest_finished_future.wait_for(kTestTimeout),
            std::future_status::ready);
  ASSERT_TRUE(wait_for_predicate(
      [&] { return coordinator.snapshot().lineage_rows == 0U; }));
  EXPECT_EQ(order, (std::vector<int>{1, 2, 3, 4}));
  EXPECT_EQ(coordinator.snapshot().lane_admitted_units, 0U);
  coordinator.stop_admission();
  compute_lane.close_and_drain();
  graph_state.close_and_drain();
}

/**
 * @brief Verifies target and canonical-intent lineages cancel independently.
 * @return Nothing; GoogleTest assertions report cross-lineage interference.
 * @throws Allocation and synchronization failures unchanged to GoogleTest.
 */
TEST(ComputeRequestCoordinator,
     PeerTargetsAndCanonicalIntentsDoNotCrossCancel) {
  GraphModel model(std::filesystem::path{});
  GraphStateExecutor graph_state(model);
  GraphStateExecutor compute_lane(
      model, 3, GraphStateExecutor::CapacityMode::TotalAdmission);
  ComputeRequestCoordinator coordinator(graph_state, compute_lane);
  const SupersessionKey target_key(1, ComputeIntent::GlobalHighPrecision);
  const SupersessionKey peer_target_key(2, ComputeIntent::GlobalHighPrecision);
  const SupersessionKey peer_intent_key(1, ComputeIntent::RealTimeUpdate);
  auto old_source = std::make_shared<ComputeRequestCancellationSource>();
  auto peer_target_source =
      std::make_shared<ComputeRequestCancellationSource>();
  auto peer_intent_source =
      std::make_shared<ComputeRequestCancellationSource>();
  auto latest_source = std::make_shared<ComputeRequestCancellationSource>();
  std::promise<void> old_entered;
  auto old_entered_future = old_entered.get_future();
  std::promise<void> release_old;
  const std::shared_future<void> old_release = release_old.get_future().share();
  std::atomic<std::size_t> materialized{0};

  auto old = coordinator.prepare(target_key);
  coordinator.publish(
      std::move(old), old_source,
      [&] {
        old_entered.set_value();
        old_release.wait();
        materialized.fetch_add(1, std::memory_order_relaxed);
      },
      [] {}, [](std::exception_ptr) {});
  graph_state.submit([](GraphModel&) {}).get();
  ASSERT_EQ(old_entered_future.wait_for(kTestTimeout),
            std::future_status::ready);

  auto publish_peer =
      [&](const SupersessionKey& key,
          std::shared_ptr<ComputeRequestCancellationSource> source) {
        auto prepared = coordinator.prepare(key);
        coordinator.publish(
            std::move(prepared), std::move(source),
            [&] { materialized.fetch_add(1, std::memory_order_relaxed); },
            [] {}, [](std::exception_ptr) {});
      };
  publish_peer(peer_target_key, peer_target_source);
  publish_peer(peer_intent_key, peer_intent_source);
  publish_peer(target_key, latest_source);
  graph_state.submit([](GraphModel&) {}).get();

  const ComputeRequestCoordinator::Snapshot blocked = coordinator.snapshot();
  EXPECT_EQ(blocked.lineage_rows, 3U);
  EXPECT_EQ(blocked.reserved_tickets, 3U);
  EXPECT_EQ(blocked.active_candidates, 1U);
  EXPECT_EQ(blocked.pending_candidates, 3U);
  ASSERT_TRUE(old_source->accepted_reason().has_value());
  EXPECT_EQ(*old_source->accepted_reason(),
            ComputeRunCancellationReason::Superseded);
  EXPECT_FALSE(peer_target_source->accepted_reason().has_value());
  EXPECT_FALSE(peer_intent_source->accepted_reason().has_value());
  EXPECT_FALSE(latest_source->accepted_reason().has_value());

  release_old.set_value();
  ASSERT_TRUE(wait_for_predicate(
      [&] { return coordinator.snapshot().lineage_rows == 0U; }));
  EXPECT_EQ(materialized.load(std::memory_order_acquire), 4U);
  coordinator.stop_admission();
  compute_lane.close_and_drain();
  graph_state.close_and_drain();
}

/**
 * @brief Verifies equal labels on distinct live Graphs have separate domains.
 * @return Nothing; GoogleTest assertions report cross-instance cancellation.
 * @throws Runtime, filesystem, or synchronization failures to GoogleTest.
 */
TEST(ComputeRequestCoordinator,
     EqualExternalLabelsOnDistinctLiveGraphsKeepIndependentDomains) {
  const auto first_root = std::filesystem::temp_directory_path() /
                          "photospider-supersession-domain-first";
  const auto second_root = std::filesystem::temp_directory_path() /
                           "photospider-supersession-domain-second";
  std::filesystem::remove_all(first_root);
  std::filesystem::remove_all(second_root);
  {
    GraphRuntime::Info first_info;
    first_info.name = "equal-external-label";
    first_info.root = first_root;
    GraphRuntime::Info second_info;
    second_info.name = first_info.name;
    second_info.root = second_root;
    GraphRuntime first(first_info);
    GraphRuntime second(second_info);
    const GraphInstanceId first_instance =
        first.graph_state()
            .submit([](GraphModel& graph) { return graph.instance_id(); })
            .get();
    const GraphInstanceId second_instance =
        second.graph_state()
            .submit([](GraphModel& graph) { return graph.instance_id(); })
            .get();
    ASSERT_NE(first_instance, second_instance);

    const SupersessionKey key(4, ComputeIntent::GlobalHighPrecision);
    auto first_source = std::make_shared<ComputeRequestCancellationSource>();
    auto second_source = std::make_shared<ComputeRequestCancellationSource>();
    std::promise<void> first_entered;
    auto first_entered_future = first_entered.get_future();
    std::promise<void> second_entered;
    auto second_entered_future = second_entered.get_future();
    std::promise<void> release;
    const std::shared_future<void> release_future =
        release.get_future().share();

    auto first_prepared = first.prepare_compute_request(key);
    first.publish_compute_request(
        std::move(first_prepared), first_source,
        [&] {
          first_entered.set_value();
          release_future.wait();
        },
        [] {}, [](std::exception_ptr) {});
    auto second_prepared = second.prepare_compute_request(key);
    second.publish_compute_request(
        std::move(second_prepared), second_source,
        [&] {
          second_entered.set_value();
          release_future.wait();
        },
        [] {}, [](std::exception_ptr) {});
    first.graph_state().submit([](GraphModel&) {}).get();
    second.graph_state().submit([](GraphModel&) {}).get();
    ASSERT_EQ(first_entered_future.wait_for(kTestTimeout),
              std::future_status::ready);
    ASSERT_EQ(second_entered_future.wait_for(kTestTimeout),
              std::future_status::ready);

    auto replacement = first.prepare_compute_request(key);
    first.publish_compute_request(
        std::move(replacement),
        std::make_shared<ComputeRequestCancellationSource>(), [] {}, [] {},
        [](std::exception_ptr) {});
    first.graph_state().submit([](GraphModel&) {}).get();
    ASSERT_TRUE(first_source->accepted_reason().has_value());
    EXPECT_EQ(*first_source->accepted_reason(),
              ComputeRunCancellationReason::Superseded);
    EXPECT_FALSE(second_source->accepted_reason().has_value());

    release.set_value();
    ASSERT_TRUE(wait_for_predicate([&] {
      return first.compute_request_snapshot().lineage_rows == 0U &&
             second.compute_request_snapshot().lineage_rows == 0U;
    }));
  }
  std::filesystem::remove_all(first_root);
  std::filesystem::remove_all(second_root);
}

/**
 * @brief Verifies generation exhaustion preserves already current work.
 * @return Nothing; GoogleTest assertions report cancellation or wrap failures.
 * @throws Allocation and synchronization failures unchanged to GoogleTest.
 */
TEST(ComputeRequestCoordinator,
     GenerationOverflowRejectsPreparationWithoutDisplacingCurrentWork) {
  GraphModel model(std::filesystem::path{});
  GraphStateExecutor graph_state(model);
  GraphStateExecutor compute_lane(
      model, 1, GraphStateExecutor::CapacityMode::TotalAdmission);
  ComputeRequestCoordinator coordinator(
      graph_state, compute_lane, std::numeric_limits<std::uint64_t>::max());
  const SupersessionKey key(11, ComputeIntent::GlobalHighPrecision);
  auto source = std::make_shared<ComputeRequestCancellationSource>();
  auto prepared = coordinator.prepare(key);
  const SupersessionIdentity identity = prepared.identity();
  std::promise<void> entered;
  auto entered_future = entered.get_future();
  std::promise<void> release;
  const std::shared_future<void> release_future = release.get_future().share();
  std::promise<void> finished;
  auto finished_future = finished.get_future();
  std::atomic<bool> remained_current{false};
  coordinator.publish(
      std::move(prepared), source,
      [&] {
        entered.set_value();
        release_future.wait();
        remained_current.store(coordinator.is_current(identity),
                               std::memory_order_release);
        finished.set_value();
      },
      [] {}, [](std::exception_ptr) {});
  graph_state.submit([](GraphModel&) {}).get();
  ASSERT_EQ(entered_future.wait_for(kTestTimeout), std::future_status::ready);
  EXPECT_THROW((void)coordinator.prepare(key), std::overflow_error);
  EXPECT_FALSE(source->accepted_reason().has_value());
  release.set_value();
  ASSERT_EQ(finished_future.wait_for(kTestTimeout), std::future_status::ready);
  ASSERT_TRUE(wait_for_predicate(
      [&] { return coordinator.snapshot().lineage_rows == 0U; }));
  EXPECT_TRUE(remained_current.load(std::memory_order_acquire));
  coordinator.stop_admission();
  compute_lane.close_and_drain();
  graph_state.close_and_drain();
}

/**
 * @brief Verifies close rejects queued publication and retires its ticket once.
 * @return Nothing; GoogleTest assertions report settlement or ledger failures.
 * @throws Allocation and synchronization failures unchanged to GoogleTest.
 */
TEST(ComputeRequestCoordinator,
     CloseRejectsQueuedPublicationAndRetiresItsParkedTicketExactlyOnce) {
  GraphModel model(std::filesystem::path{});
  GraphStateExecutor graph_state(model);
  GraphStateExecutor compute_lane(
      model, 1, GraphStateExecutor::CapacityMode::TotalAdmission);
  ComputeRequestCoordinator coordinator(graph_state, compute_lane);
  std::promise<void> graph_blocked;
  auto graph_blocked_future = graph_blocked.get_future();
  std::promise<void> release_graph;
  const std::shared_future<void> graph_release =
      release_graph.get_future().share();
  auto blocker = graph_state.submit([&](GraphModel&) {
    graph_blocked.set_value();
    graph_release.wait();
  });
  ASSERT_EQ(graph_blocked_future.wait_for(kTestTimeout),
            std::future_status::ready);

  std::atomic<int> executed{0};
  std::atomic<int> superseded{0};
  std::atomic<int> failed{0};
  std::atomic<bool> failure_was_nonnull{false};
  std::promise<void> failure_settled;
  auto failure_settled_future = failure_settled.get_future();
  auto prepared = coordinator.prepare(
      SupersessionKey(13, ComputeIntent::GlobalHighPrecision));
  coordinator.publish(
      std::move(prepared), std::make_shared<ComputeRequestCancellationSource>(),
      [&] { executed.fetch_add(1, std::memory_order_relaxed); },
      [&] { superseded.fetch_add(1, std::memory_order_relaxed); },
      [&](std::exception_ptr failure) {
        failed.fetch_add(1, std::memory_order_relaxed);
        failure_was_nonnull.store(failure != nullptr,
                                  std::memory_order_release);
        failure_settled.set_value();
      });
  EXPECT_EQ(coordinator.snapshot().provisional_adopters, 1U);
  coordinator.stop_admission();
  compute_lane.stop_admission();
  auto closing =
      std::async(std::launch::async, [&] { compute_lane.close_and_drain(); });
  release_graph.set_value();
  blocker.get();

  ASSERT_EQ(failure_settled_future.wait_for(kTestTimeout),
            std::future_status::ready);
  ASSERT_EQ(closing.wait_for(kTestTimeout), std::future_status::ready);
  closing.get();
  graph_state.close_and_drain();
  EXPECT_EQ(executed.load(std::memory_order_acquire), 0);
  EXPECT_EQ(superseded.load(std::memory_order_acquire), 0);
  EXPECT_EQ(failed.load(std::memory_order_acquire), 1);
  EXPECT_TRUE(failure_was_nonnull.load(std::memory_order_acquire));
  EXPECT_EQ(coordinator.snapshot().lineage_rows, 0U);
  EXPECT_EQ(coordinator.snapshot().lane_admitted_units, 0U);
}

/**
 * @brief Runs the deterministic 18,000-generation 30 Hz equivalent storm.
 * @return Nothing; GoogleTest assertions report bound/settlement failures.
 * @throws Allocation and synchronization failures unchanged to GoogleTest.
 */
TEST(ComputeRequestCoordinatorStorm,
     TenMinutesAtThirtyHertzKeepsOneTicketAndLatestMailbox) {
  run_fixed_same_key_storm(30U);
}

/**
 * @brief Runs the deterministic 36,000-generation 60 Hz equivalent storm.
 * @return Nothing; GoogleTest assertions report bound/settlement failures.
 * @throws Allocation and synchronization failures unchanged to GoogleTest.
 */
TEST(ComputeRequestCoordinatorStorm,
     TenMinutesAtSixtyHertzKeepsOneTicketAndLatestMailbox) {
  run_fixed_same_key_storm(60U);
}

/**
 * @brief Verifies stable group cancellation and child-local HP isolation.
 * @return Nothing; GoogleTest assertions report fan-out/aggregate failures.
 * @throws Run construction and synchronization failures to GoogleTest.
 */
TEST(RunGroup,
     GroupCancellationFansStableReasonWhileChildOnlyCancellationStaysLocal) {
  const SupersessionIdentity first_identity{
      SupersessionKey(5, ComputeIntent::RealTimeUpdate),
      SupersessionGeneration(17)};
  auto first_source = std::make_shared<ComputeRequestCancellationSource>();
  RunGroup cancelled_group(
      make_group_submission(ComputeIntent::GlobalHighPrecision,
                            ComputeRunQuality::Full, first_identity),
      make_group_submission(ComputeIntent::RealTimeUpdate,
                            ComputeRunQuality::Interactive, first_identity),
      first_source);
  const ComputeRunLease cancelled_hp = cancelled_group.hp_run().acquire_lease();
  const ComputeRunLease cancelled_rt = cancelled_group.rt_run().acquire_lease();
  first_source->attach(cancelled_group.hp_run());
  first_source->attach(cancelled_group.rt_run());
  EXPECT_TRUE(cancelled_group.request_cancellation(
      ComputeRunCancellationReason::Superseded));
  EXPECT_FALSE(cancelled_group.request_cancellation(
      ComputeRunCancellationReason::ExplicitRequest));
  ASSERT_TRUE(cancelled_hp.terminal_outcome().has_value());
  ASSERT_TRUE(cancelled_rt.terminal_outcome().has_value());
  EXPECT_EQ(cancelled_hp.terminal_outcome()->cancellation_reason,
            ComputeRunCancellationReason::Superseded);
  EXPECT_EQ(cancelled_rt.terminal_outcome()->cancellation_reason,
            ComputeRunCancellationReason::Superseded);
  EXPECT_THROW(
      cancelled_group.sibling_commit_gate()->wait_for_rt_commit_or_throw(),
      GraphError);
  EXPECT_EQ(cancelled_group.aggregate_terminal_outcome().cancellation_reason,
            ComputeRunCancellationReason::Superseded);

  const SupersessionIdentity second_identity{
      SupersessionKey(6, ComputeIntent::RealTimeUpdate),
      SupersessionGeneration(18)};
  RunGroup child_local(
      make_group_submission(ComputeIntent::GlobalHighPrecision,
                            ComputeRunQuality::Full, second_identity),
      make_group_submission(ComputeIntent::RealTimeUpdate,
                            ComputeRunQuality::Interactive, second_identity));
  const ComputeRunLease local_hp = child_local.hp_run().acquire_lease();
  const ComputeRunLease local_rt = child_local.rt_run().acquire_lease();
  EXPECT_TRUE(child_local.hp_run().cancellation_source().request_cancellation(
      ComputeRunCancellationReason::ExplicitRequest));
  EXPECT_TRUE(local_hp.terminal_outcome().has_value());
  EXPECT_FALSE(local_rt.terminal_outcome().has_value());
  EXPECT_TRUE(child_local.rt_run().publish_succeeded());
  EXPECT_EQ(child_local.aggregate_terminal_outcome().cancellation_reason,
            ComputeRunCancellationReason::ExplicitRequest);
}

/**
 * @brief Verifies aggregate failure priority and same-domain validation.
 * @return Nothing; GoogleTest assertions report ordering/validation failures.
 * @throws Run construction and synchronization failures to GoogleTest.
 */
TEST(RunGroup, AggregatePrioritizesResourceFailureAndRejectsMixedDomains) {
  const SupersessionIdentity identity{
      SupersessionKey(8, ComputeIntent::RealTimeUpdate),
      SupersessionGeneration(19)};
  RunGroup group(
      make_group_submission(ComputeIntent::GlobalHighPrecision,
                            ComputeRunQuality::Full, identity),
      make_group_submission(ComputeIntent::RealTimeUpdate,
                            ComputeRunQuality::Interactive, identity));
  const ComputeRunLease hp = group.hp_run().acquire_lease();
  const ComputeRunLease rt = group.rt_run().acquire_lease();
  EXPECT_TRUE(
      group.hp_run().publish_failed(std::make_exception_ptr(std::bad_alloc())));
  EXPECT_TRUE(group.rt_run().publish_failed(
      std::make_exception_ptr(std::runtime_error("rt failure"))));
  const ComputeRunTerminalOutcome aggregate =
      group.aggregate_terminal_outcome();
  EXPECT_EQ(aggregate.kind, ComputeRunTerminalKind::Failed);
  EXPECT_THROW(std::rethrow_exception(aggregate.failure), std::bad_alloc);
  EXPECT_TRUE(hp.terminal_outcome().has_value());
  EXPECT_TRUE(rt.terminal_outcome().has_value());

  ComputeRunSubmission mismatched_hp = make_group_submission(
      ComputeIntent::GlobalHighPrecision, ComputeRunQuality::Full, identity);
  ComputeRunSubmission mismatched_rt = make_group_submission(
      ComputeIntent::RealTimeUpdate, ComputeRunQuality::Interactive, identity);
  mismatched_rt.graph_identity = "different-live-graph";
  EXPECT_THROW(
      (void)RunGroup(std::move(mismatched_hp), std::move(mismatched_rt)),
      std::invalid_argument);
}

}  // namespace
}  // namespace ps::compute
