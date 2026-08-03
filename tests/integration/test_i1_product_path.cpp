#include <gtest/gtest.h>

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

#include "benchmark/i1_profile.hpp"
#include "photospider/host/host.hpp"
#include "providers/opencv/opencv_operation_provider_test_access.hpp"

namespace ps::benchmark {
namespace {

using std::chrono_literals::operator""s;

/**
 * @brief Owns one isolated filesystem root for the embedded I1 integration.
 * @throws std::filesystem::filesystem_error when construction cannot create
 * the directory.
 * @note Destruction performs best-effort cleanup and never throws.
 */
class ScopedI1TempDirectory final {
 public:
  /**
   * @brief Creates one process-local unique temporary directory.
   * @throws std::filesystem::filesystem_error on path/query/create failures.
   * @throws std::bad_alloc when path text allocation fails.
   */
  ScopedI1TempDirectory() {
    static std::atomic<unsigned int> sequence{0U};
    const unsigned int id = sequence.fetch_add(1U, std::memory_order_relaxed);
    root_ = std::filesystem::temp_directory_path() /
            ("photospider_i1_product_path_" + std::to_string(id));
    std::filesystem::remove_all(root_);
    std::filesystem::create_directories(root_);
  }

  /** @brief Removes the isolated root. @throws Nothing. */
  ~ScopedI1TempDirectory() noexcept {
    std::error_code error;
    std::filesystem::remove_all(root_, error);
  }

  /**
   * @brief Prevents duplicate cleanup ownership.
   * @param other Owner that retains its directory.
   * @throws Nothing because copying is unavailable.
   */
  ScopedI1TempDirectory(const ScopedI1TempDirectory& other) = delete;

  /**
   * @brief Prevents replacing cleanup ownership.
   * @param other Owner that remains unchanged.
   * @return No value because assignment is unavailable.
   * @throws Nothing because assignment is unavailable.
   */
  ScopedI1TempDirectory& operator=(const ScopedI1TempDirectory& other) = delete;

  /**
   * @brief Returns the owned absolute-or-temporary root path.
   * @return Borrowed path valid for this guard lifetime.
   * @throws Nothing.
   */
  const std::filesystem::path& root() const noexcept { return root_; }

 private:
  /** @brief Exact recursively cleaned path. */
  std::filesystem::path root_;
};

/**
 * @brief Deterministically blocks only tiled curve callbacks in the provider.
 *
 * One old Run enters the gate before a newer same-key request is admitted.
 * This creates overlap without imposing an elapsed-time performance premise.
 *
 * @throws Nothing from provider callbacks; synchronization failure terminates.
 * @note The test must call release() before product teardown. The destructor
 * also releases defensively but does not own or join product workers.
 */
class CurveCallbackGate final
    : public providers::opencv::OpenCvOperationObserver {
 public:
  /** @brief Creates a closed gate with no observed callback. @throws Nothing.
   */
  CurveCallbackGate() = default;

  /** @brief Releases any defensive outstanding wait. @throws Nothing. */
  ~CurveCallbackGate() noexcept override { release(); }

  /**
   * @brief Prevents copying synchronization ownership.
   * @param other Gate that remains independent.
   * @throws Nothing because copying is unavailable.
   */
  CurveCallbackGate(const CurveCallbackGate& other) = delete;

  /**
   * @brief Prevents replacing synchronization ownership.
   * @param other Gate that remains unchanged.
   * @return No value because assignment is unavailable.
   * @throws Nothing because assignment is unavailable.
   */
  CurveCallbackGate& operator=(const CurveCallbackGate& other) = delete;

  /** @copydoc providers::opencv::OpenCvOperationObserver::on_enter */
  void on_enter(const char* operation_key) noexcept override {
    if (std::strcmp(operation_key, "image_process:curve_transform") != 0) {
      return;
    }
    std::unique_lock<std::mutex> lock(mutex_);
    ++entered_;
    changed_.notify_all();
    changed_.wait(lock, [this] { return released_; });
  }

  /** @copydoc providers::opencv::OpenCvOperationObserver::on_exit */
  void on_exit(const char* operation_key) noexcept override {
    if (std::strcmp(operation_key, "image_process:curve_transform") != 0) {
      return;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    ++exited_;
    changed_.notify_all();
  }

  /**
   * @brief Waits until at least one curve callback has entered the closed gate.
   * @param timeout Bounded diagnostic timeout, not a product SLO threshold.
   * @return True when an entry was observed before timeout.
   * @throws std::system_error from mutex/condition synchronization.
   */
  bool wait_for_entry(std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> lock(mutex_);
    return changed_.wait_for(lock, timeout, [this] { return entered_ > 0U; });
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

  /**
   * @brief Returns the number of curve callbacks that completed observer exit.
   * @return Exact synchronized exit count.
   * @throws std::system_error when mutex locking fails.
   */
  std::size_t exited() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return exited_;
  }

 private:
  /** @brief Serializes gate state and callback counters. */
  mutable std::mutex mutex_;

  /** @brief Wakes harness and provider waiters on state changes. */
  std::condition_variable changed_;

  /** @brief True after the harness permits callback completion. */
  bool released_ = false;

  /** @brief Number of matching callback entries. */
  std::size_t entered_ = 0U;

  /** @brief Number of matching callback exits. */
  std::size_t exited_ = 0U;
};

/**
 * @brief Publishes one borrowed OpenCV observer for a lexical test scope.
 * @throws Nothing.
 * @note Destruction releases the gate before clearing publication. The Host is
 * destroyed before the gate, so product teardown can join loaded callbacks.
 */
class ScopedCurveObserver final {
 public:
  /**
   * @brief Publishes the borrowed gate.
   * @param gate Gate that outlives this guard and the embedded Host.
   * @throws Nothing.
   */
  explicit ScopedCurveObserver(CurveCallbackGate& gate) noexcept : gate_(gate) {
    providers::opencv::set_opencv_operation_observer_for_testing(&gate_);
  }

  /** @brief Releases and unpublishes the borrowed gate. @throws Nothing. */
  ~ScopedCurveObserver() noexcept {
    gate_.release();
    providers::opencv::set_opencv_operation_observer_for_testing(nullptr);
  }

  /**
   * @brief Prevents duplicate unpublication ownership.
   * @param other Guard retaining publication responsibility.
   * @throws Nothing because copying is unavailable.
   */
  ScopedCurveObserver(const ScopedCurveObserver& other) = delete;

  /**
   * @brief Prevents replacing unpublication ownership.
   * @param other Guard that remains unchanged.
   * @return No value because assignment is unavailable.
   * @throws Nothing because assignment is unavailable.
   */
  ScopedCurveObserver& operator=(const ScopedCurveObserver& other) = delete;

 private:
  /** @brief Borrowed gate released before observer publication is cleared. */
  CurveCallbackGate& gate_;
};

/**
 * @brief Writes a reduced source with the exact serial I1 operation topology.
 * @param path YAML destination inside the test-owned temporary directory.
 * @return Nothing after complete flush/close.
 * @throws std::filesystem::filesystem_error when parent creation fails.
 * @throws std::runtime_error when the document cannot be written completely.
 * @note Only source dimensions are reduced. Provider identities, topology,
 * coefficients, target, intent, quality, and private QoS remain exact.
 */
void write_reduced_i1_graph(const std::filesystem::path& path) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream output(path);
  if (!output) {
    throw std::runtime_error("failed to open reduced I1 graph YAML");
  }
  output << R"YAML(- id: 0
  name: i1_coordinate_pattern
  type: image_generator
  subtype: coordinate_pattern
  parameters:
    width: 64
    height: 64
    channels: 4
    seed: 0
- id: 1
  name: i1_curve_one
  type: image_process
  subtype: curve_transform
  image_inputs:
    - from_node_id: 0
  parameters:
    k: 0.80
- id: 2
  name: i1_curve_two
  type: image_process
  subtype: curve_transform
  image_inputs:
    - from_node_id: 1
  parameters:
    k: 1.00
- id: 3
  name: i1_curve_three
  type: image_process
  subtype: curve_transform
  image_inputs:
    - from_node_id: 2
  parameters:
    k: 1.20
- id: 4
  name: i1_curve_four
  type: image_process
  subtype: curve_transform
  image_inputs:
    - from_node_id: 3
  parameters:
    k: 1.40
)YAML";
  output.close();
  if (!output) {
    throw std::runtime_error("failed to write reduced I1 graph YAML");
  }
}

/**
 * @brief Builds one reduced real-path request while retaining frozen controls.
 * @param session Loaded reduced session.
 * @param edit_index Frozen edit identity.
 * @return Exact I1 request with its Region clipped to the reduced source.
 * @throws The frozen request builder errors unchanged.
 */
HostComputeRequest make_reduced_request(const GraphSessionId& session,
                                        std::size_t edit_index) {
  HostComputeRequest request =
      make_i1_host_compute_request(session, edit_index);
  request.dirty_roi = PixelRect{0, 0, 64, 64};
  return request;
}

/**
 * @brief Waits until observation state proves supersession has been accepted.
 * @param collector Shared bounded episode collector.
 * @param timeout Bounded test-harness timeout, not an I1 latency verdict.
 * @return True after two generations and an older cancellation are visible.
 * @throws std::bad_alloc when a collector snapshot cannot allocate.
 */
bool wait_for_supersession(const I1EpisodeObservationCollector& collector,
                           std::chrono::milliseconds timeout) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    const I1EpisodeObservationSnapshot snapshot = collector.snapshot();
    if (snapshot.current_generations.size() >= 2U &&
        !snapshot.cancellations.empty()) {
      return true;
    }
    std::this_thread::yield();
  }
  return false;
}

/**
 * @brief Reports whether any authoritative Host high-water dimension advanced.
 * @param baseline Pre-row ledger snapshot.
 * @param final_snapshot Post-settlement ledger snapshot.
 * @return True when physical execution increased at least one lifetime peak.
 * @throws Nothing.
 */
bool host_high_water_advanced(
    const ResourceLedger::Snapshot& baseline,
    const ResourceLedger::Snapshot& final_snapshot) noexcept {
  return final_snapshot.high_water.cpu_slots > baseline.high_water.cpu_slots ||
         final_snapshot.high_water.retained_memory_bytes >
             baseline.high_water.retained_memory_bytes ||
         final_snapshot.high_water.scratch_bytes >
             baseline.high_water.scratch_bytes ||
         final_snapshot.high_water.ready_entries >
             baseline.high_water.ready_entries ||
         final_snapshot.high_water.ready_bytes >
             baseline.high_water.ready_bytes;
}

/**
 * @brief Exercises the full embedded latest-wins path with deterministic
 * overlap and checks every source-private observation/resource boundary.
 * @throws Nothing when product behavior matches the contract; setup/runtime
 * exceptions fail the GoogleTest case.
 * @note Timeouts only bound deadlock diagnosis. No elapsed duration is used as
 * a machine-dependent performance verdict.
 */
TEST(I1ProductPath, NewerGenerationIsSoleVisibleSettledOutput) {
  ScopedI1TempDirectory temp;
  CurveCallbackGate gate;
  std::unique_ptr<Host> host = create_embedded_host();
  ASSERT_NE(host, nullptr);
  ScopedCurveObserver observer(gate);

  const VoidResult seeded = host->seed_builtin_ops();
  ASSERT_TRUE(seeded.status.ok) << seeded.status.message;
  HostExecutionConfig execution_config;
  execution_config.worker_count = 8U;
  const VoidResult configured =
      host->configure_execution_defaults(execution_config);
  ASSERT_TRUE(configured.status.ok) << configured.status.message;

  const std::filesystem::path yaml_path = temp.root() / "i1-reduced.yaml";
  write_reduced_i1_graph(yaml_path);
  GraphLoadRequest load;
  load.session = GraphSessionId{"i1-product-path"};
  load.root_dir = (temp.root() / "sessions").string();
  load.yaml_path = yaml_path.string();
  load.cache_root_dir = (temp.root() / "cache").string();
  const Result<GraphSessionId> loaded = host->load_graph(load);
  ASSERT_TRUE(loaded.status.ok) << loaded.status.message;

  I1Host* const i1_host = as_i1_host(*host);
  ASSERT_NE(i1_host, nullptr);
  const I1ExecutionSnapshot baseline = i1_host->i1_execution_snapshot(0U, 1U);
  I1EpisodeObservationCollector observations;

  const auto first_deadline = std::chrono::steady_clock::now() + 5s;
  Result<std::future<OperationStatus>> first =
      i1_host->compute_i1_async(I1HostComputeRequest{
          make_reduced_request(load.session, 0U),
          compute::ComputeRunQos{compute::ComputeRunQosClass::Interactive,
                                 first_deadline, 1U, 8U},
          observations.make_edit_sink(0U)});
  ASSERT_TRUE(first.status.ok) << first.status.message;
  ASSERT_TRUE(first.value.valid());
  ASSERT_TRUE(gate.wait_for_entry(5s));

  const VoidResult mutated =
      host->set_node_yaml(load.session, NodeId{1}, i1_edit_node_one_yaml(11U));
  ASSERT_TRUE(mutated.status.ok) << mutated.status.message;
  const auto second_deadline = std::chrono::steady_clock::now() + 5s;
  Result<std::future<OperationStatus>> second =
      i1_host->compute_i1_async(I1HostComputeRequest{
          make_reduced_request(load.session, 11U),
          compute::ComputeRunQos{compute::ComputeRunQosClass::Interactive,
                                 second_deadline, 1U, 8U},
          observations.make_edit_sink(11U)});
  ASSERT_TRUE(second.status.ok) << second.status.message;
  ASSERT_TRUE(second.value.valid());
  EXPECT_TRUE(wait_for_supersession(observations, 5s));

  gate.release();
  ASSERT_EQ(first.value.wait_for(5s), std::future_status::ready);
  ASSERT_EQ(second.value.wait_for(5s), std::future_status::ready);
  const OperationStatus first_status = first.value.get();
  const OperationStatus second_status = second.value.get();
  EXPECT_FALSE(first_status.ok);
  EXPECT_TRUE(second_status.ok) << second_status.message;
  EXPECT_GT(gate.exited(), 0U);

  const I1EpisodeObservationSnapshot snapshot = observations.snapshot();
  ASSERT_FALSE(snapshot.overflowed);
  ASSERT_EQ(snapshot.current_generations.size(), 2U);
  EXPECT_EQ(snapshot.current_generations[0].edit_index, 0U);
  EXPECT_EQ(snapshot.current_generations[1].edit_index, 11U);
  EXPECT_LT(snapshot.current_generations[0].generation,
            snapshot.current_generations[1].generation);

  ASSERT_EQ(snapshot.cancellations.size(), 1U);
  const I1ObservedCancellation& cancellation = snapshot.cancellations.front();
  EXPECT_EQ(cancellation.edit_index, 0U);
  EXPECT_EQ(cancellation.reason,
            compute::ComputeRunCancellationReason::Superseded);

  ASSERT_EQ(snapshot.terminals.size(), 2U);
  std::optional<I1ObservedTerminal> old_terminal;
  std::optional<I1ObservedTerminal> new_terminal;
  for (const I1ObservedTerminal& terminal : snapshot.terminals) {
    if (terminal.edit_index == 0U) {
      old_terminal = terminal;
    } else if (terminal.edit_index == 11U) {
      new_terminal = terminal;
    }
  }
  ASSERT_TRUE(old_terminal.has_value());
  ASSERT_TRUE(new_terminal.has_value());
  EXPECT_EQ(old_terminal->kind, compute::ComputeRunTerminalKind::Cancelled);
  EXPECT_EQ(new_terminal->kind, compute::ComputeRunTerminalKind::Succeeded);
  EXPECT_EQ(old_terminal->run_id, cancellation.run_id);
  EXPECT_EQ(old_terminal->generation, cancellation.generation);

  ASSERT_EQ(snapshot.visible_outputs.size(), 1U);
  const I1ObservedVisibleOutput& visible = snapshot.visible_outputs.front();
  EXPECT_EQ(visible.edit_index, 11U);
  EXPECT_EQ(visible.run_id, new_terminal->run_id);
  EXPECT_EQ(visible.generation, new_terminal->generation);
  const ContentDigestResult digest = compute_content_digest(visible.output);
  ASSERT_EQ(digest.state, ContentDigestState::Available) << digest.diagnostic;
  ASSERT_TRUE(digest.digest.has_value());
  EXPECT_EQ(digest.digest->algorithm,
            CanonicalDigestAlgorithm::Sha256CanonicalV1);

  ASSERT_FALSE(snapshot.service_starts.empty());
  for (const I1ObservedServiceStart& start : snapshot.service_starts) {
    EXPECT_EQ(start.quality, compute::ComputeRunQuality::Full);
    EXPECT_EQ(start.qos.service_class,
              compute::ComputeRunQosClass::Interactive);
    EXPECT_EQ(start.qos.weight, 1U);
    EXPECT_EQ(start.qos.maximum_parallelism, std::optional<std::uint32_t>{8U});
    if (start.edit_index == 0U) {
      EXPECT_EQ(
          start.qos.deadline,
          std::optional<std::chrono::steady_clock::time_point>{first_deadline});
      EXPECT_LT(start.causal_sequence, cancellation.causal_sequence);
    } else {
      EXPECT_EQ(start.edit_index, 11U);
      EXPECT_EQ(start.qos.deadline,
                std::optional<std::chrono::steady_clock::time_point>{
                    second_deadline});
    }
  }

  const I1ExecutionSnapshot settled =
      i1_host->i1_execution_snapshot(baseline.lifecycle.snapshot_cut, 4096U);
  EXPECT_EQ(settled.lifecycle.cursor_gap, 0U);
  EXPECT_EQ(settled.lifecycle.global_dropped_total,
            baseline.lifecycle.global_dropped_total);
  EXPECT_EQ(settled.host_resources.reserved, baseline.host_resources.reserved);
  EXPECT_TRUE(host_high_water_advanced(baseline.host_resources,
                                       settled.host_resources));
  const compute::ExecutionLifecycleCounters& counters =
      settled.lifecycle.counters;
  EXPECT_EQ(counters.pending_candidate_count, 0U);
  EXPECT_EQ(counters.admitted_standalone_run_count, 0U);
  EXPECT_EQ(counters.admitted_run_group_count, 0U);
  EXPECT_EQ(counters.admitted_child_run_count, 0U);
  EXPECT_EQ(counters.terminal_not_quiescent_run_count, 0U);
  EXPECT_EQ(counters.finalizing_run_count, 0U);
  EXPECT_EQ(counters.ready_entry_count, 0U);
  EXPECT_EQ(counters.entered_callback_count, 0U);
  EXPECT_EQ(counters.live_root_reservation_count, 0U);
  EXPECT_EQ(counters.live_child_grant_count, 0U);
  EXPECT_EQ(counters.live_policy_invocation_count, 0U);

  const VoidResult closed = host->close_graph(load.session);
  EXPECT_TRUE(closed.status.ok) << closed.status.message;
}

}  // namespace
}  // namespace ps::benchmark
