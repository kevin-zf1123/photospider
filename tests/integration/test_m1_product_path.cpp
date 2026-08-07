/**
 * @file test_m1_product_path.cpp
 * @brief Verifies deterministic mixed fairness through the real embedded Host.
 */
#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include "benchmark/b1_host.hpp"     // NOLINT(build/include_subdir)
#include "benchmark/b1_profile.hpp"  // NOLINT(build/include_subdir)
#include "benchmark/i1_profile.hpp"  // NOLINT(build/include_subdir)
#include "benchmark/m1_host.hpp"     // NOLINT(build/include_subdir)
#include "benchmark/m1_profile.hpp"  // NOLINT(build/include_subdir)
#include "photospider/host/host.hpp"
#include "providers/opencv/opencv_operation_provider_test_access.hpp"

namespace ps::benchmark {
namespace {

using std::chrono_literals::operator""s;

/**
 * @brief Owns one isolated filesystem root for deterministic M1 product tests.
 * @throws std::filesystem::filesystem_error when construction cannot create
 * the directory.
 * @note Destruction performs best-effort cleanup and never throws.
 */
class ScopedM1TempDirectory final {
 public:
  /**
   * @brief Creates one process-local unique temporary directory.
   * @throws std::filesystem::filesystem_error on path/query/create failures.
   * @throws std::bad_alloc when path text allocation fails.
   */
  ScopedM1TempDirectory() {
    static std::atomic<std::uint64_t> sequence{0U};
    const auto timestamp =
        std::chrono::steady_clock::now().time_since_epoch().count();
    root_ = std::filesystem::temp_directory_path() /
            ("photospider_m1_product_path_" + std::to_string(timestamp) + "_" +
             std::to_string(sequence.fetch_add(1U, std::memory_order_relaxed)));
    std::filesystem::remove_all(root_);
    std::filesystem::create_directories(root_);
  }

  /** @brief Removes the exact owned root. @throws Nothing. */
  ~ScopedM1TempDirectory() noexcept {
    std::error_code error;
    std::filesystem::remove_all(root_, error);
  }

  /** @brief Prevents duplicate cleanup ownership. */
  ScopedM1TempDirectory(const ScopedM1TempDirectory&) = delete;

  /** @brief Prevents duplicate cleanup assignment. */
  ScopedM1TempDirectory& operator=(const ScopedM1TempDirectory&) = delete;

  /**
   * @brief Returns the exact owned root.
   * @return Borrowed path valid through this owner.
   * @throws Nothing.
   */
  const std::filesystem::path& root() const noexcept { return root_; }

 private:
  /** @brief Exact recursively cleaned path. */
  std::filesystem::path root_;
};

/**
 * @brief Blocks matching real OpenCV operation callbacks until release.
 *
 * The gate establishes queued product work without using elapsed completion
 * time as evidence. Once released, every later matching callback passes.
 *
 * @throws Nothing from provider callbacks; synchronization failure terminates.
 * @note The matching operation remains borrowed immutable text for this owner.
 */
class OperationCallbackGate final
    : public providers::opencv::OpenCvOperationObserver {
 public:
  /**
   * @brief Creates one closed gate for an exact provider operation key.
   * @param operation_key Nonempty exact built-in operation identity.
   * @throws std::invalid_argument for an empty key.
   * @throws std::bad_alloc when key ownership allocates.
   */
  explicit OperationCallbackGate(std::string operation_key)
      : operation_key_(std::move(operation_key)) {
    if (operation_key_.empty()) {
      throw std::invalid_argument(
          "M1 callback gate requires an operation key.");
    }
  }

  /** @brief Releases every defensive outstanding wait. @throws Nothing. */
  ~OperationCallbackGate() noexcept override { release(); }

  /** @brief Prevents duplicate synchronization ownership. */
  OperationCallbackGate(const OperationCallbackGate&) = delete;

  /** @brief Prevents duplicate synchronization assignment. */
  OperationCallbackGate& operator=(const OperationCallbackGate&) = delete;

  /** @copydoc providers::opencv::OpenCvOperationObserver::on_enter */
  void on_enter(const char* operation_key) noexcept override {
    if (operation_key == nullptr || operation_key_ != operation_key) {
      return;
    }
    std::unique_lock<std::mutex> lock(mutex_);
    ++entered_;
    changed_.notify_all();
    changed_.wait(lock, [this] { return released_; });
  }

  /** @copydoc providers::opencv::OpenCvOperationObserver::on_exit */
  void on_exit(const char* operation_key) noexcept override {
    if (operation_key == nullptr || operation_key_ != operation_key) {
      return;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    ++exited_;
    changed_.notify_all();
  }

  /**
   * @brief Waits for a minimum number of matching callback entries.
   * @param count Positive required entry count.
   * @param timeout Bounded deadlock diagnostic, not an M1 SLO threshold.
   * @return True when at least `count` entries arrived before timeout.
   * @throws std::system_error from synchronization.
   */
  bool wait_for_entries(std::size_t count, std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> lock(mutex_);
    return changed_.wait_for(lock, timeout,
                             [this, count] { return entered_ >= count; });
  }

  /**
   * @brief Opens the gate idempotently and wakes every blocked callback.
   * @return Nothing.
   * @throws Nothing; synchronization failures terminate.
   */
  void release() noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    released_ = true;
    changed_.notify_all();
  }

 private:
  /** @brief Exact provider operation key selected for blocking. */
  const std::string operation_key_;

  /** @brief Serializes gate state and callback counters. */
  std::mutex mutex_;

  /** @brief Wakes provider callbacks and bounded harness waits. */
  std::condition_variable changed_;

  /** @brief True after matching callbacks may complete. */
  bool released_ = false;

  /** @brief Number of matching callbacks that entered. */
  std::size_t entered_ = 0U;

  /** @brief Number of matching callbacks that exited. */
  std::size_t exited_ = 0U;
};

/**
 * @brief Publishes one borrowed OpenCV observer for a lexical test scope.
 * @throws Nothing.
 * @note Destruction releases the gate before clearing process publication.
 */
class ScopedOperationObserver final {
 public:
  /**
   * @brief Publishes one borrowed callback gate.
   * @param gate Gate that outlives this guard and blocked product work.
   * @throws Nothing.
   */
  explicit ScopedOperationObserver(OperationCallbackGate& gate) noexcept
      : gate_(gate) {
    providers::opencv::set_opencv_operation_observer_for_testing(&gate_);
  }

  /** @brief Releases and unpublishes the borrowed observer. @throws Nothing. */
  ~ScopedOperationObserver() noexcept {
    gate_.release();
    providers::opencv::set_opencv_operation_observer_for_testing(nullptr);
  }

  /** @brief Prevents duplicate unpublication ownership. */
  ScopedOperationObserver(const ScopedOperationObserver&) = delete;

  /** @brief Prevents duplicate unpublication assignment. */
  ScopedOperationObserver& operator=(const ScopedOperationObserver&) = delete;

 private:
  /** @brief Borrowed gate released before observer publication is cleared. */
  OperationCallbackGate& gate_;
};

/**
 * @brief Writes one single-task target-four coordinate-pattern Graph.
 * @param path Test-owned YAML destination.
 * @param seed Deterministic source seed.
 * @return Nothing after complete flush and close.
 * @throws Filesystem, stream, or allocation failures unchanged.
 * @note Target id four lets the existing I1/B1 request builders traverse the
 * real Host path while reducing this mechanism-only fixture to one task.
 */
void write_single_task_graph(const std::filesystem::path& path,
                             std::uint64_t seed) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream output(path);
  if (!output) {
    throw std::runtime_error("failed to open reduced M1 graph YAML");
  }
  output << "- id: 4\n"
            "  name: m1_coordinate_pattern\n"
            "  type: image_generator\n"
            "  subtype: coordinate_pattern\n"
            "  parameters:\n"
            "    width: 64\n"
            "    height: 64\n"
            "    channels: 4\n"
            "    seed: "
         << seed << '\n';
  output.close();
  if (!output) {
    throw std::runtime_error("failed to write reduced M1 graph YAML");
  }
}

/**
 * @brief Writes one tiled target-four Graph with at least eight real tasks.
 * @param path Test-owned YAML destination.
 * @param seed Deterministic source seed.
 * @return Nothing after complete flush and close.
 * @throws Filesystem, stream, or allocation failures unchanged.
 * @note A 1024-by-1024 curve target produces enough real tiles for cap eight;
 * no fake task or direct execution-service path is introduced.
 */
void write_cap_eight_graph(const std::filesystem::path& path,
                           std::uint64_t seed) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream output(path);
  if (!output) {
    throw std::runtime_error("failed to open cap-eight M1 graph YAML");
  }
  output << "- id: 0\n"
            "  name: m1_cap_eight_source\n"
            "  type: image_generator\n"
            "  subtype: coordinate_pattern\n"
            "  parameters:\n"
            "    width: 1024\n"
            "    height: 1024\n"
            "    channels: 4\n"
            "    seed: "
         << seed
         << "\n"
            "- id: 4\n"
            "  name: m1_cap_eight_curve\n"
            "  type: image_process\n"
            "  subtype: curve_transform\n"
            "  image_inputs:\n"
            "    - from_node_id: 0\n"
            "  parameters:\n"
            "    k: 1.00\n";
  output.close();
  if (!output) {
    throw std::runtime_error("failed to write cap-eight M1 graph YAML");
  }
}

/**
 * @brief Creates and configures one embedded Host for deterministic M1 work.
 * @param worker_count Exact worker count in the supported range `[1,8]`.
 * @return Seeded configured embedded Host.
 * @throws Product, allocation, or configuration failures unchanged.
 */
std::unique_ptr<Host> make_m1_host(unsigned int worker_count) {
  std::unique_ptr<Host> host = create_embedded_host();
  if (!host) {
    throw std::runtime_error("embedded M1 Host creation returned null");
  }
  const VoidResult seeded = host->seed_builtin_ops();
  if (!seeded.status.ok) {
    throw std::runtime_error(seeded.status.message);
  }
  HostExecutionConfig config;
  config.worker_count = worker_count;
  const VoidResult configured = host->configure_execution_defaults(config);
  if (!configured.status.ok) {
    throw std::runtime_error(configured.status.message);
  }
  return host;
}

/**
 * @brief Loads one reduced single-task session below a unique owned subtree.
 * @param host Configured embedded Host.
 * @param root Test-owned filesystem root.
 * @param session Unique stable session identity.
 * @param seed Deterministic source seed.
 * @return Nothing after successful publication.
 * @throws Product, filesystem, stream, or allocation failures unchanged.
 */
void load_single_task_session(Host& host, const std::filesystem::path& root,
                              const GraphSessionId& session,
                              std::uint64_t seed) {
  const std::filesystem::path session_root = root / session.value;
  const std::filesystem::path yaml = session_root / "graph.yaml";
  write_single_task_graph(yaml, seed);
  GraphLoadRequest load;
  load.session = session;
  load.root_dir = (session_root / "session").string();
  load.yaml_path = yaml.string();
  load.cache_root_dir = (session_root / "cache").string();
  const Result<GraphSessionId> loaded = host.load_graph(load);
  if (!loaded.status.ok) {
    throw std::runtime_error(loaded.status.message);
  }
}

/**
 * @brief Loads one real tiled target-four session for cap-eight reservation.
 * @param host Configured embedded Host.
 * @param root Test-owned filesystem root.
 * @param session Unique stable session identity.
 * @param seed Deterministic source seed.
 * @return Nothing after successful publication.
 * @throws Product, filesystem, stream, or allocation failures unchanged.
 */
void load_cap_eight_session(Host& host, const std::filesystem::path& root,
                            const GraphSessionId& session, std::uint64_t seed) {
  const std::filesystem::path session_root = root / session.value;
  const std::filesystem::path yaml = session_root / "graph.yaml";
  write_cap_eight_graph(yaml, seed);
  GraphLoadRequest load;
  load.session = session;
  load.root_dir = (session_root / "session").string();
  load.yaml_path = yaml.string();
  load.cache_root_dir = (session_root / "cache").string();
  const Result<GraphSessionId> loaded = host.load_graph(load);
  if (!loaded.status.ok) {
    throw std::runtime_error(loaded.status.message);
  }
}

/**
 * @brief Builds one source-private Throughput request for a reduced session.
 * @param session Loaded target-four Graph.
 * @param cap Exact supported B1 Run cap one or eight.
 * @param sink Request-local observation-only sink.
 * @return Complete request accepted by the existing B1 Host seam.
 * @throws Profile validation or allocation failures unchanged.
 */
B1HostComputeRequest make_reduced_throughput_request(
    const GraphSessionId& session, std::uint64_t cap,
    std::shared_ptr<compute::ComputeRunObservationSink> sink) {
  return B1HostComputeRequest{
      make_b1_host_compute_request(session, cap),
      compute::ComputeRunQos{compute::ComputeRunQosClass::Throughput,
                             std::nullopt, 1U, static_cast<std::uint32_t>(cap)},
      std::move(sink)};
}

/**
 * @brief Builds one reduced ordinary request for the accepted I1 collector.
 * @param session Loaded target-four Graph.
 * @return Frozen I1 request with Region clipped to the 64-by-64 fixture.
 * @throws Frozen request-builder failures unchanged.
 */
HostComputeRequest make_reduced_interactive_request(
    const GraphSessionId& session) {
  HostComputeRequest request = make_i1_host_compute_request(session, 0U);
  request.dirty_roi = PixelRect{0, 0, 64, 64};
  return request;
}

/**
 * @brief Waits for exact Host and Throughput CPU root-reservation totals.
 * @param host Private M1 diagnostic seam.
 * @param host_cpu Exact all-class ledger CPU total.
 * @param throughput_cpu Exact Throughput-account CPU total.
 * @param timeout Bounded deadlock diagnostic, not an SLO threshold.
 * @return Latest snapshot once both totals match.
 * @throws std::runtime_error when the bounded wait expires.
 * @throws Snapshot allocation or synchronization failures unchanged.
 */
M1ExecutionSnapshot wait_for_cpu_reservations(
    const M1Host& host, std::uint64_t host_cpu, std::uint64_t throughput_cpu,
    std::chrono::milliseconds timeout) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  do {
    M1ExecutionSnapshot snapshot = host.m1_execution_snapshot(0U, 1U);
    if (snapshot.host_resources.reserved.cpu_slots == host_cpu &&
        snapshot.throughput.reserved.cpu_slots == throughput_cpu) {
      return snapshot;
    }
    std::this_thread::yield();
  } while (std::chrono::steady_clock::now() < deadline);
  throw std::runtime_error("timed out waiting for exact M1 CPU reservations");
}

/**
 * @brief Waits for exact real ready-store counts in both product classes.
 * @param host Private observation-only M1 diagnostic seam.
 * @param interactive_entries Required physical Interactive ready count.
 * @param throughput_entries Required physical Throughput ready count.
 * @param timeout Bounded deadlock diagnostic, not an M1 timing threshold.
 * @return Latest complete snapshot at the matching real ready cut.
 * @throws std::runtime_error when the bounded wait expires or the ready class
 * snapshot contains an unknown closed QoS value.
 * @throws Snapshot allocation or synchronization failures unchanged.
 */
M1ExecutionSnapshot wait_for_ready_entries(const M1Host& host,
                                           std::uint64_t interactive_entries,
                                           std::uint64_t throughput_entries,
                                           std::chrono::milliseconds timeout) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  do {
    M1ExecutionSnapshot snapshot = host.m1_execution_snapshot(0U, 1U);
    if (!snapshot.ready_classes.valid) {
      throw std::runtime_error("M1 ready snapshot contains unknown QoS");
    }
    if (snapshot.ready_classes.interactive_entries == interactive_entries &&
        snapshot.ready_classes.throughput_entries == throughput_entries &&
        snapshot.ready_classes.total_entries ==
            interactive_entries + throughput_entries) {
      return snapshot;
    }
    std::this_thread::yield();
  } while (std::chrono::steady_clock::now() < deadline);
  throw std::runtime_error("timed out waiting for exact M1 ready entries");
}

/**
 * @brief Closes every test-owned session after all product work has settled.
 * @param host Host owning every session.
 * @param sessions Complete loaded session list.
 * @return Nothing after every close succeeds.
 * @throws std::runtime_error on the first close failure.
 */
void close_sessions(Host& host, const std::vector<GraphSessionId>& sessions) {
  for (auto it = sessions.rbegin(); it != sessions.rend(); ++it) {
    const VoidResult closed = host.close_graph(*it);
    if (!closed.status.ok) {
      throw std::runtime_error(closed.status.message);
    }
  }
}

/**
 * @brief Builds complete non-timing fairness input around observed starts.
 * @param snapshot Settled real mixed observation snapshot.
 * @return Passing synthetic threshold inputs plus actual class-start order.
 * @throws std::bad_alloc when vector storage allocates.
 * @note Only class-start evidence comes from this reduced product fixture;
 * machine progress/Jain/latency thresholds remain deliberately synthetic.
 */
M1FairnessEvidenceInput make_product_class_start_input(
    const M1FairnessObservationSnapshot& snapshot) {
  M1FairnessEvidenceInput input;
  input.paired_isolated_b1 =
      M1PairedB1RateEvidence{1000000U, std::chrono::seconds(1)};
  for (std::size_t window = 0U; window < kM1MeasuredWindowCount; ++window) {
    input.progress_windows.push_back(
        M1ThroughputProgressSample{window, 200000U, std::chrono::seconds(1)});
    input.graph_service_windows.push_back(
        M1GraphServiceWindow{window, true, 1U, 1U});
  }
  for (const M1FairnessObservation& event : snapshot.events) {
    if (event.kind == M1ObservationKind::ServiceStart) {
      input.class_starts.push_back(
          M1ClassStartSample{event.causal_sequence, event.service_class,
                             event.interactive_candidate_startable,
                             event.throughput_candidate_startable,
                             event.execution_grant_committed});
    }
  }
  input.headroom_admissions = M1HeadroomAdmissionEvidence{
      kM1MeasuredI1AttemptCount, kM1MeasuredI1AttemptCount, 0U};
  for (std::size_t origin = 0U; origin < kM1MeasuredI1OriginCount; ++origin) {
    for (std::size_t edit = 0U; edit < kI1EditCount; ++edit) {
      input.headroom_outcomes.push_back(M1HeadroomAdmissionOutcome{
          origin, edit, true, OperationStatus{}, false});
    }
  }
  input.interactive_latency_verdict = I1Verdict::Pass;
  input.observation_overflowed = snapshot.overflowed;
  input.observation_sequence_exhausted = snapshot.sequence_exhausted;
  input.observation_qos_mismatch = snapshot.qos_mismatch;
  return input;
}

/**
 * @brief Proves real mixed backlog preserves batch settlement and 3:1 starts.
 * @throws Product, filesystem, observer, future, or synchronization failures.
 * @note The fixture has one real task per request. Provider gating establishes
 * simultaneous backlog; no elapsed completion time is compared to an SLO.
 */
TEST(M1ProductPath,
     ThroughputSettlesInsideInteractiveBacklogWithBoundedStarts) {
  ScopedM1TempDirectory temp;
  std::unique_ptr<Host> host = make_m1_host(1U);
  std::future<Result<ImageBuffer>> blocker_future;
  std::vector<std::future<Result<ImageBuffer>>> throughput_futures;
  OperationCallbackGate gate("image_generator:coordinate_pattern");
  ScopedOperationObserver observer(gate);

  B1Host* const b1_host = as_b1_host(*host);
  I1Host* const i1_host = as_i1_host(*host);
  M1Host* const m1_host = as_m1_host(*host);
  ASSERT_NE(b1_host, nullptr);
  ASSERT_NE(i1_host, nullptr);
  ASSERT_NE(m1_host, nullptr);

  std::vector<GraphSessionId> sessions;
  const GraphSessionId blocker{"m1-class-blocker"};
  sessions.push_back(blocker);
  load_single_task_session(*host, temp.root(), blocker, 100U);
  std::vector<GraphSessionId> interactive_sessions;
  std::vector<GraphSessionId> throughput_sessions;
  for (std::size_t index = 0U; index < 8U; ++index) {
    GraphSessionId session{"m1-interactive-" + std::to_string(index)};
    interactive_sessions.push_back(session);
    sessions.push_back(session);
    load_single_task_session(*host, temp.root(), session, index);
  }
  for (std::size_t index = 0U; index < 3U; ++index) {
    GraphSessionId session{"m1-throughput-" + std::to_string(index)};
    throughput_sessions.push_back(session);
    sessions.push_back(session);
    load_single_task_session(*host, temp.root(), session, index + 20U);
  }

  M1FairnessObservationCollector blocker_observations(16U);
  blocker_future = std::async(std::launch::async, [&] {
    return b1_host->compute_b1_image(make_reduced_throughput_request(
        blocker, 1U,
        blocker_observations.make_sink(
            M1ObservedRequestTag::ThroughputGraphA)));
  });
  ASSERT_TRUE(gate.wait_for_entries(1U, 5s));

  M1FairnessObservationCollector observations(128U);
  const auto admission_sample = std::chrono::steady_clock::now();
  I1AcceptedBoundaryCollector admissions(
      *i1_host, [admission_sample] { return admission_sample; },
      [](std::chrono::steady_clock::time_point) {});
  std::vector<I1EditAdmissionResult> interactive_admissions;
  interactive_admissions.reserve(interactive_sessions.size());
  for (const GraphSessionId& session : interactive_sessions) {
    interactive_admissions.push_back(admissions.admit_edit(
        admission_sample, 0U, make_reduced_interactive_request(session),
        observations.make_sink(M1ObservedRequestTag::Interactive)));
    EXPECT_EQ(interactive_admissions.back().edit_index, 0U);
    EXPECT_EQ(interactive_admissions.back().admission_sample, admission_sample);
    ASSERT_TRUE(
        interactive_admissions.back().reserved_event_sequence.has_value());
    ASSERT_TRUE(interactive_admissions.back().accepted_coordinate.has_value());
    EXPECT_EQ(
        interactive_admissions.back().accepted_coordinate->admission_time(),
        admission_sample);
    EXPECT_EQ(
        interactive_admissions.back().accepted_coordinate->event_sequence(),
        *interactive_admissions.back().reserved_event_sequence);
    ASSERT_TRUE(interactive_admissions.back().settlement.valid());
  }
  throughput_futures.reserve(throughput_sessions.size());
  for (std::size_t index = 0U; index < throughput_sessions.size(); ++index) {
    throughput_futures.push_back(std::async(std::launch::async, [&, index] {
      return b1_host->compute_b1_image(make_reduced_throughput_request(
          throughput_sessions[index], 1U,
          observations.make_sink(
              index % 2U == 0U ? M1ObservedRequestTag::ThroughputGraphA
                               : M1ObservedRequestTag::ThroughputGraphB)));
    }));
  }
  const M1ExecutionSnapshot queued =
      wait_for_cpu_reservations(*m1_host, 12U, 4U, 5s);
  EXPECT_EQ(queued.throughput.capacity.cpu_slots, 31U);
  const M1ExecutionSnapshot ready = wait_for_ready_entries(
      *m1_host, interactive_sessions.size(), throughput_sessions.size(), 5s);
  ASSERT_TRUE(ready.ready_classes.valid);

  gate.release();
  ASSERT_EQ(blocker_future.wait_for(5s), std::future_status::ready);
  const Result<ImageBuffer> blocker_result = blocker_future.get();
  ASSERT_TRUE(blocker_result.status.ok) << blocker_result.status.message;
  for (std::future<Result<ImageBuffer>>& future : throughput_futures) {
    ASSERT_EQ(future.wait_for(5s), std::future_status::ready);
    const Result<ImageBuffer> result = future.get();
    ASSERT_TRUE(result.status.ok) << result.status.message;
  }
  for (I1EditAdmissionResult& admission : interactive_admissions) {
    ASSERT_EQ(admission.settlement.wait_for(5s), std::future_status::ready);
    const OperationStatus status = admission.settlement.get();
    ASSERT_TRUE(status.ok) << status.message;
  }

  const M1FairnessObservationSnapshot snapshot = observations.snapshot();
  ASSERT_FALSE(snapshot.overflowed);
  ASSERT_FALSE(snapshot.sequence_exhausted);
  ASSERT_FALSE(snapshot.qos_mismatch);
  const M1FairnessSummary fairness =
      evaluate_m1_fairness(make_product_class_start_input(snapshot));
  EXPECT_EQ(fairness.class_start_verdict, I1Verdict::Pass);
  EXPECT_LE(fairness.maximum_interactive_burst, kM1InteractiveBurstLimit);

  std::uint64_t first_throughput_settlement = 0U;
  std::uint64_t final_interactive_settlement = 0U;
  std::size_t interactive_starts = 0U;
  std::size_t throughput_starts = 0U;
  std::size_t dual_startable_starts = 0U;
  std::vector<std::uint64_t> successful_throughput_runs;
  for (const M1FairnessObservation& event : snapshot.events) {
    if (event.kind == M1ObservationKind::ServiceStart) {
      EXPECT_TRUE(event.execution_grant_committed);
      if (event.interactive_candidate_startable &&
          event.throughput_candidate_startable) {
        ++dual_startable_starts;
      }
      if (event.service_class == compute::ComputeRunQosClass::Interactive) {
        ++interactive_starts;
      } else {
        ++throughput_starts;
      }
    }
    if (event.kind == M1ObservationKind::RunTerminal &&
        event.service_class == compute::ComputeRunQosClass::Throughput &&
        event.run_terminal_kind == compute::ComputeRunTerminalKind::Succeeded) {
      successful_throughput_runs.push_back(event.run_id);
    }
    if (event.kind != M1ObservationKind::RunResourceSettled) {
      continue;
    }
    if (event.service_class == compute::ComputeRunQosClass::Throughput &&
        first_throughput_settlement == 0U) {
      first_throughput_settlement = event.causal_sequence;
    }
    if (event.service_class == compute::ComputeRunQosClass::Interactive) {
      final_interactive_settlement =
          std::max(final_interactive_settlement, event.causal_sequence);
    }
  }
  EXPECT_EQ(interactive_starts, interactive_sessions.size());
  EXPECT_EQ(throughput_starts, throughput_sessions.size());
  EXPECT_GT(dual_startable_starts, 0U);
  EXPECT_FALSE(successful_throughput_runs.empty());
  EXPECT_NE(first_throughput_settlement, 0U);
  EXPECT_NE(final_interactive_settlement, 0U);
  EXPECT_LT(first_throughput_settlement, final_interactive_settlement);

  bool successful_throughput_settled_while_interactive_outstanding = false;
  for (const std::uint64_t throughput_run : successful_throughput_runs) {
    const auto settlement = std::find_if(
        snapshot.events.begin(), snapshot.events.end(),
        [throughput_run](const M1FairnessObservation& event) {
          return event.kind == M1ObservationKind::RunResourceSettled &&
                 event.run_id == throughput_run;
        });
    if (settlement == snapshot.events.end()) {
      continue;
    }
    std::uint64_t interactive_started = 0U;
    std::uint64_t interactive_settled = 0U;
    for (const M1FairnessObservation& event : snapshot.events) {
      if (event.causal_sequence > settlement->causal_sequence) {
        break;
      }
      if (event.service_class != compute::ComputeRunQosClass::Interactive) {
        continue;
      }
      if (event.kind == M1ObservationKind::ServiceStart) {
        ++interactive_started;
      } else if (event.kind == M1ObservationKind::RunResourceSettled) {
        ++interactive_settled;
      }
    }
    const bool queued_interactive_remains =
        interactive_started < ready.ready_classes.interactive_entries;
    const bool started_interactive_remains =
        interactive_settled < interactive_started;
    if (queued_interactive_remains || started_interactive_remains) {
      successful_throughput_settled_while_interactive_outstanding = true;
      break;
    }
  }
  EXPECT_TRUE(successful_throughput_settled_while_interactive_outstanding);

  const M1ExecutionSnapshot settled =
      wait_for_cpu_reservations(*m1_host, 0U, 0U, 5s);
  EXPECT_EQ(settled.host_resources.reserved, ResourceVector{});
  close_sessions(*host, sessions);
}

/**
 * @brief Proves exact general-capacity saturation preserves one Interactive
 * slot.
 * @throws Product, filesystem, observer, future, or synchronization failures.
 * @note Three cap-eight plus seven cap-one Throughput Runs reserve 31 CPU slots
 * while eight workers are gated. Another cap-one Throughput Run is rejected;
 * one single-task Interactive Run then owns the protected thirty-second slot.
 */
TEST(M1ProductPath, ThroughputGeneralCapacityPreservesInteractiveHeadroom) {
  ScopedM1TempDirectory temp;
  std::unique_ptr<Host> host = make_m1_host(8U);
  std::vector<std::future<Result<ImageBuffer>>> throughput_futures;
  OperationCallbackGate gate("image_generator:coordinate_pattern");
  ScopedOperationObserver observer(gate);

  B1Host* const b1_host = as_b1_host(*host);
  I1Host* const i1_host = as_i1_host(*host);
  M1Host* const m1_host = as_m1_host(*host);
  ASSERT_NE(b1_host, nullptr);
  ASSERT_NE(i1_host, nullptr);
  ASSERT_NE(m1_host, nullptr);

  std::vector<GraphSessionId> sessions;
  std::vector<GraphSessionId> throughput_sessions;
  for (std::size_t index = 0U; index < 11U; ++index) {
    GraphSessionId session{"m1-headroom-throughput-" + std::to_string(index)};
    throughput_sessions.push_back(session);
    sessions.push_back(session);
    load_cap_eight_session(*host, temp.root(), session, index + 40U);
  }
  const GraphSessionId interactive_session{"m1-headroom-interactive"};
  sessions.push_back(interactive_session);
  load_single_task_session(*host, temp.root(), interactive_session, 80U);

  M1FairnessObservationCollector observations(1024U);
  const std::vector<std::uint64_t> active_caps{8U, 8U, 8U, 1U, 1U,
                                               1U, 1U, 1U, 1U, 1U};
  throughput_futures.reserve(active_caps.size());
  for (std::size_t index = 0U; index < active_caps.size(); ++index) {
    throughput_futures.push_back(std::async(std::launch::async, [&, index] {
      return b1_host->compute_b1_image(make_reduced_throughput_request(
          throughput_sessions[index], active_caps[index],
          observations.make_sink(
              index % 2U == 0U ? M1ObservedRequestTag::ThroughputGraphA
                               : M1ObservedRequestTag::ThroughputGraphB)));
    }));
  }
  ASSERT_TRUE(gate.wait_for_entries(8U, 5s));
  const M1ExecutionSnapshot saturated =
      wait_for_cpu_reservations(*m1_host, 31U, 31U, 5s);
  ASSERT_EQ(saturated.host_resources.limits.cpu_slots, 32U);
  ASSERT_EQ(saturated.throughput.capacity.cpu_slots, 31U);

  const Result<ImageBuffer> rejected =
      b1_host->compute_b1_image(make_reduced_throughput_request(
          throughput_sessions.back(), 1U,
          observations.make_sink(M1ObservedRequestTag::ThroughputGraphA)));
  EXPECT_FALSE(rejected.status.ok);
  EXPECT_EQ(checked_graph_error_code(rejected.status), GraphErrc::ComputeError);
  const M1ExecutionSnapshot after_rejection =
      m1_host->m1_execution_snapshot(0U, 1U);
  EXPECT_EQ(after_rejection.host_resources.reserved.cpu_slots, 31U);
  EXPECT_EQ(after_rejection.throughput.reserved.cpu_slots, 31U);

  const auto admission_sample = std::chrono::steady_clock::now();
  I1AcceptedBoundaryCollector admissions(
      *i1_host, [admission_sample] { return admission_sample; },
      [](std::chrono::steady_clock::time_point) {});
  I1EditAdmissionResult interactive = admissions.admit_edit(
      admission_sample, 0U,
      make_reduced_interactive_request(interactive_session),
      observations.make_sink(M1ObservedRequestTag::Interactive));
  EXPECT_EQ(interactive.edit_index, 0U);
  EXPECT_EQ(interactive.admission_sample, admission_sample);
  ASSERT_TRUE(interactive.reserved_event_sequence.has_value());
  ASSERT_TRUE(interactive.accepted_coordinate.has_value());
  EXPECT_EQ(interactive.accepted_coordinate->admission_time(),
            admission_sample);
  EXPECT_EQ(interactive.accepted_coordinate->event_sequence(),
            *interactive.reserved_event_sequence);
  ASSERT_TRUE(interactive.settlement.valid());
  const M1ExecutionSnapshot with_interactive =
      wait_for_cpu_reservations(*m1_host, 32U, 31U, 5s);
  EXPECT_EQ(with_interactive.throughput.capacity.cpu_slots, 31U);
  EXPECT_EQ(with_interactive.throughput.reserved.cpu_slots, 31U);

  gate.release();
  for (std::future<Result<ImageBuffer>>& future : throughput_futures) {
    ASSERT_EQ(future.wait_for(5s), std::future_status::ready);
    const Result<ImageBuffer> result = future.get();
    ASSERT_TRUE(result.status.ok) << result.status.message;
  }
  ASSERT_EQ(interactive.settlement.wait_for(5s), std::future_status::ready);
  const OperationStatus interactive_status = interactive.settlement.get();
  ASSERT_TRUE(interactive_status.ok) << interactive_status.message;

  const M1FairnessObservationSnapshot snapshot = observations.snapshot();
  EXPECT_FALSE(snapshot.overflowed);
  EXPECT_FALSE(snapshot.sequence_exhausted);
  EXPECT_FALSE(snapshot.qos_mismatch);
  const M1ExecutionSnapshot settled =
      wait_for_cpu_reservations(*m1_host, 0U, 0U, 5s);
  EXPECT_EQ(settled.host_resources.reserved, ResourceVector{});
  close_sessions(*host, sessions);
}

}  // namespace
}  // namespace ps::benchmark
