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
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>
#include <utility>

#include "benchmark/i2_profile.hpp"
#include "photospider/host/host.hpp"
#include "providers/opencv/opencv_operation_provider_test_access.hpp"

namespace ps::benchmark {
namespace {

using std::chrono_literals::operator""ms;
using std::chrono_literals::operator""s;

/**
 * @brief Owns one isolated filesystem root for real I2 product tests.
 * @throws std::filesystem::filesystem_error when creation fails.
 * @note Destruction performs best-effort cleanup and never throws.
 */
class ScopedI2TempDirectory final {
 public:
  /** @brief Creates one process-local unique temporary directory. */
  ScopedI2TempDirectory() {
    static std::atomic<unsigned int> sequence{0U};
    const unsigned int id = sequence.fetch_add(1U, std::memory_order_relaxed);
    root_ = std::filesystem::temp_directory_path() /
            ("photospider_i2_product_path_" + std::to_string(id));
    std::filesystem::remove_all(root_);
    std::filesystem::create_directories(root_);
  }

  /** @brief Removes the isolated directory recursively. */
  ~ScopedI2TempDirectory() noexcept {
    std::error_code error;
    std::filesystem::remove_all(root_, error);
  }

  /** @brief Prevents duplicate cleanup ownership. */
  ScopedI2TempDirectory(const ScopedI2TempDirectory&) = delete;

  /** @brief Prevents duplicate cleanup assignment. */
  ScopedI2TempDirectory& operator=(const ScopedI2TempDirectory&) = delete;

  /**
   * @brief Returns the owned root path.
   * @return Borrowed path valid for this guard lifetime.
   * @throws Nothing.
   */
  const std::filesystem::path& root() const noexcept { return root_; }

 private:
  /** @brief Exact recursively cleaned path. */
  std::filesystem::path root_;
};

/**
 * @brief Deterministically blocks tiled curve callbacks until released.
 * @throws Nothing from provider callbacks; synchronization failure terminates.
 * @note Tests release before Host teardown. The observer owns no product work.
 */
class I2CurveCallbackGate final
    : public providers::opencv::OpenCvOperationObserver {
 public:
  /**
   * @brief Creates one gate after a fixed number of pass-through entries.
   * @param pass_entries Matching entries allowed to return without waiting.
   * @throws Nothing.
   */
  explicit I2CurveCallbackGate(std::size_t pass_entries = 0U) noexcept
      : pass_entries_(pass_entries) {}

  /** @brief Releases defensive outstanding callbacks. */
  ~I2CurveCallbackGate() noexcept override { release(); }

  /** @brief Prevents duplicate synchronization ownership. */
  I2CurveCallbackGate(const I2CurveCallbackGate&) = delete;

  /** @brief Prevents duplicate synchronization assignment. */
  I2CurveCallbackGate& operator=(const I2CurveCallbackGate&) = delete;

  /** @copydoc providers::opencv::OpenCvOperationObserver::on_enter */
  void on_enter(const char* operation_key) noexcept override {
    if (std::strcmp(operation_key, "image_process:curve_transform") != 0) {
      return;
    }
    try {
      std::unique_lock<std::mutex> lock(mutex_);
      ++entered_;
      changed_.notify_all();
      if (entered_ <= pass_entries_) {
        return;
      }
      changed_.wait(lock, [this] { return released_; });
    } catch (...) {
      std::terminate();
    }
  }

  /** @copydoc providers::opencv::OpenCvOperationObserver::on_exit */
  void on_exit(const char* operation_key) noexcept override {
    if (std::strcmp(operation_key, "image_process:curve_transform") != 0) {
      return;
    }
    try {
      std::lock_guard<std::mutex> lock(mutex_);
      ++exited_;
      changed_.notify_all();
    } catch (...) {
      std::terminate();
    }
  }

  /**
   * @brief Waits for the first blocked curve callback.
   * @param timeout Bounded deadlock diagnostic timeout.
   * @return True after one matching callback enters.
   * @throws std::system_error from synchronization.
   */
  bool wait_for_entry(std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> lock(mutex_);
    return changed_.wait_for(lock, timeout,
                             [this] { return entered_ > pass_entries_; });
  }

  /** @brief Opens the gate idempotently and wakes all callbacks. */
  void release() noexcept {
    try {
      std::lock_guard<std::mutex> lock(mutex_);
      released_ = true;
      changed_.notify_all();
    } catch (...) {
      std::terminate();
    }
  }

  /**
   * @brief Returns completed matching callback exits.
   * @return Synchronized exact exit count.
   * @throws std::system_error from synchronization.
   */
  std::size_t exited() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return exited_;
  }

 private:
  /** @brief Serializes the provider gate and counters. */
  mutable std::mutex mutex_;
  /** @brief Wakes provider and harness waiters. */
  std::condition_variable changed_;
  /** @brief True after callbacks may complete. */
  bool released_ = false;
  /** @brief Matching entries deliberately passed before blocking. */
  std::size_t pass_entries_ = 0U;
  /** @brief Matching callback entries. */
  std::size_t entered_ = 0U;
  /** @brief Matching callback exits. */
  std::size_t exited_ = 0U;
};

/**
 * @brief Publishes one borrowed provider observer for a lexical scope.
 * @throws Nothing.
 * @note Destruction releases waiters before clearing observer publication.
 */
class ScopedI2CurveObserver final {
 public:
  /**
   * @brief Publishes one gate that outlives this owner.
   * @param gate Borrowed provider gate.
   */
  explicit ScopedI2CurveObserver(I2CurveCallbackGate& gate) noexcept
      : gate_(gate) {
    providers::opencv::set_opencv_operation_observer_for_testing(&gate_);
  }

  /** @brief Releases callbacks and removes borrowed publication. */
  ~ScopedI2CurveObserver() noexcept {
    gate_.release();
    providers::opencv::set_opencv_operation_observer_for_testing(nullptr);
  }

  /** @brief Prevents duplicate observer-publication ownership. */
  ScopedI2CurveObserver(const ScopedI2CurveObserver&) = delete;

  /** @brief Prevents duplicate observer-publication assignment. */
  ScopedI2CurveObserver& operator=(const ScopedI2CurveObserver&) = delete;

 private:
  /** @brief Borrowed gate released before unpublication. */
  I2CurveCallbackGate& gate_;
};

/**
 * @brief Writes one tiny exact-topology coordinate-pattern graph.
 * @param path Destination inside the test-owned temporary directory.
 * @return Nothing after complete close.
 * @throws Filesystem or stream failures when creation/write fails.
 * @note Only extent is reduced to 16x16. Provider identities, topology,
 * coefficient sequence, RT scale factor, target, and execution path are real.
 */
void write_reduced_i2_graph(const std::filesystem::path& path) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream output(path);
  if (!output) {
    throw std::runtime_error("failed to open reduced I2 graph YAML");
  }
  output << R"YAML(- id: 0
  name: i2_coordinate_pattern
  type: image_generator
  subtype: coordinate_pattern
  parameters:
    width: 16
    height: 16
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
    throw std::runtime_error("failed to write reduced I2 graph YAML");
  }
}

/**
 * @brief Creates, configures, seeds, and loads one reduced real Host graph.
 * @param temp Test-owned filesystem scope.
 * @param session Stable session label.
 * @return Embedded Host owning a loaded graph and process execution service.
 * @throws Product and filesystem failures unchanged.
 */
std::unique_ptr<Host> make_loaded_i2_host(const ScopedI2TempDirectory& temp,
                                          const GraphSessionId& session) {
  std::unique_ptr<Host> host = create_embedded_host();
  if (!host) {
    throw std::runtime_error("embedded I2 Host creation returned null");
  }
  const VoidResult seeded = host->seed_builtin_ops();
  if (!seeded.status.ok) {
    throw std::runtime_error(seeded.status.message);
  }
  HostExecutionConfig execution_config;
  execution_config.worker_count = 8U;
  const VoidResult configured =
      host->configure_execution_defaults(execution_config);
  if (!configured.status.ok) {
    throw std::runtime_error(configured.status.message);
  }
  const std::filesystem::path yaml_path = temp.root() / "i2-reduced.yaml";
  write_reduced_i2_graph(yaml_path);
  GraphLoadRequest load;
  load.session = session;
  load.root_dir = (temp.root() / "sessions").string();
  load.yaml_path = yaml_path.string();
  load.cache_root_dir = (temp.root() / "cache").string();
  const Result<GraphSessionId> loaded = host->load_graph(load);
  if (!loaded.status.ok) {
    throw std::runtime_error(loaded.status.message);
  }
  HostComputeRequest baseline = make_i1_host_compute_request(session, 0U);
  baseline.dirty_roi = PixelRect{0, 0, 16, 16};
  const VoidResult baseline_result = host->compute(baseline);
  if (!baseline_result.status.ok) {
    throw std::runtime_error(baseline_result.status.message);
  }
  return host;
}

/**
 * @brief Builds one reduced real I2 request retaining frozen private controls.
 * @param session Loaded reduced session.
 * @param edit_index Frozen edit identity.
 * @return RT request with full reduced source Region.
 * @throws Frozen request construction failures unchanged.
 */
HostComputeRequest make_reduced_i2_request(const GraphSessionId& session,
                                           std::size_t edit_index) {
  HostComputeRequest request =
      make_i2_host_compute_request(session, edit_index);
  request.dirty_roi = PixelRect{0, 0, 16, 16};
  return request;
}

/**
 * @brief Admits one real progressive request using exact anchored deadlines.
 * @param host Borrowed private I2 Host.
 * @param request Ordinary reduced RT request.
 * @param sink Preallocated edit-scoped observer.
 * @param admission Exact pre-call coordinate sample.
 * @param sequence Nonzero row-local sequence.
 * @return Scheduling result and future.
 * @throws Host allocation and synchronization failures unchanged.
 */
Result<std::future<OperationStatus>> admit_reduced_i2(
    I2Host& host, HostComputeRequest request,
    std::shared_ptr<compute::ComputeRunObservationSink> sink,
    std::chrono::steady_clock::time_point admission, std::uint64_t sequence) {
  return host.compute_i2_async(I2HostComputeRequest{
      std::move(request),
      compute::ComputeRunQos{compute::ComputeRunQosClass::Interactive,
                             admission + kI2PreviewDeadlineBudget, 1U, 8U},
      compute::ComputeRunQos{compute::ComputeRunQosClass::Interactive,
                             admission + kI2FinalDeadlineBudget, 1U, 8U},
      std::move(sink),
      compute::AcceptedBoundaryCoordinate{admission, sequence}});
}

/**
 * @brief Waits for complete two-child and Host settlement observation.
 * @param collector Shared bounded I2 collector.
 * @param timeout Bounded deadlock diagnostic timeout.
 * @return True after two terminals/quiescences/resource returns and one Host
 * settlement are release-published.
 * @throws std::bad_alloc when snapshots allocate.
 */
bool wait_for_i2_settlement(const I2EpisodeObservationCollector& collector,
                            std::chrono::milliseconds timeout) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    const I2EpisodeObservationSnapshot snapshot = collector.snapshot();
    if (snapshot.terminals.size() >= 2U &&
        snapshot.run_quiescences.size() >= 2U &&
        snapshot.resource_settlements.size() >= 2U &&
        snapshot.host_settlements.size() >= 1U) {
      return true;
    }
    std::this_thread::yield();
  }
  return false;
}

/**
 * @brief Waits for one older edit cancellation before final triggering.
 * @param collector Shared two-edit collector.
 * @param old_edit Older edit identity.
 * @param timeout Bounded deadlock diagnostic timeout.
 * @return True when the old edit has cancellation and no final trigger.
 * @throws std::bad_alloc when snapshots allocate.
 */
bool wait_for_i2_pretrigger_cancellation(
    const I2EpisodeObservationCollector& collector, std::size_t old_edit,
    std::chrono::milliseconds timeout) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    const I2EpisodeObservationSnapshot snapshot = collector.snapshot();
    bool cancelled = false;
    bool triggered = false;
    for (const I2ObservedCancellation& cancellation : snapshot.cancellations) {
      cancelled = cancelled || cancellation.child.edit_index == old_edit;
    }
    for (const I2ObservedFinalTrigger& trigger : snapshot.final_triggers) {
      triggered = triggered || trigger.child.edit_index == old_edit;
    }
    if (cancelled && !triggered) {
      return true;
    }
    std::this_thread::yield();
  }
  return false;
}

/**
 * @brief Waits until one edit-scoped child cancellation is observed.
 * @param collector Shared two-edit collector.
 * @param edit_index Edit whose cancellation must become visible.
 * @param timeout Bounded deadlock diagnostic timeout.
 * @return True after any child of the edit accepts cancellation.
 * @throws std::bad_alloc when snapshots allocate.
 */
bool wait_for_i2_cancellation(const I2EpisodeObservationCollector& collector,
                              std::size_t edit_index,
                              std::chrono::milliseconds timeout) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    const I2EpisodeObservationSnapshot snapshot = collector.snapshot();
    for (const I2ObservedCancellation& cancellation : snapshot.cancellations) {
      if (cancellation.child.edit_index == edit_index) {
        return true;
      }
    }
    std::this_thread::yield();
  }
  return false;
}

/**
 * @brief Proves one real progressive request publishes preview before trigger,
 * then final, with exact child QoS and closed Host/Metal/no-I/O evidence.
 * @throws Product, filesystem, native Metal, digest, and synchronization
 * failures unchanged to GoogleTest.
 * @note The 16x16 fixture validates long-lived behavior only; it is not the
 * exact 111-slot workload and asserts no machine SLO percentile. Conditional
 * Metal acquisition must return every configured device reservation to its
 * exact pre-acquisition value before this focused row continues.
 */
TEST(I2ProductPath, PreviewTriggersFinalAndAcquisitionsReuseExactBindings) {
  ScopedI2TempDirectory temp;
  const GraphSessionId session{"i2-positive"};
  std::unique_ptr<Host> host = make_loaded_i2_host(temp, session);
  I2Host* const i2_host = as_i2_host(*host);
  ASSERT_NE(i2_host, nullptr);

  const VoidResult mutated = host->set_node_yaml(
      session, NodeId{1}, i1_edit_node_one_yaml(kI1EditCount - 1U));
  ASSERT_TRUE(mutated.status.ok) << mutated.status.message;
  I2EpisodeObservationCollector observations;
  const auto admission = std::chrono::steady_clock::now();
  Result<std::future<OperationStatus>> result =
      admit_reduced_i2(*i2_host, make_reduced_i2_request(session, 11U),
                       observations.make_edit_sink(11U), admission, 1U);
  ASSERT_TRUE(result.status.ok) << result.status.message;
  ASSERT_TRUE(result.value.valid());
  ASSERT_EQ(result.value.wait_for(5s), std::future_status::ready);
  const OperationStatus settled = result.value.get();
  ASSERT_TRUE(settled.ok) << settled.message;
  ASSERT_TRUE(wait_for_i2_settlement(observations, 5s));
  const I1ExecutionSnapshot before_acquisitions =
      i2_host->i2_execution_snapshot(0U, 4096U);
  const auto capture_deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(5);
  ASSERT_EQ(observations.freeze_visible_outputs(*i2_host, capture_deadline),
            2U);
  const I1ExecutionSnapshot after_acquisitions = i2_host->i2_execution_snapshot(
      before_acquisitions.lifecycle.snapshot_cut, 4096U);
  ASSERT_EQ(before_acquisitions.device_resources.size(),
            after_acquisitions.device_resources.size());
  for (std::size_t index = 0U;
       index < before_acquisitions.device_resources.size(); ++index) {
    EXPECT_EQ(before_acquisitions.device_resources[index].device,
              after_acquisitions.device_resources[index].device);
    EXPECT_EQ(before_acquisitions.device_resources[index].reserved,
              after_acquisitions.device_resources[index].reserved);
  }

  const I2EpisodeObservationSnapshot snapshot = observations.snapshot();
  ASSERT_FALSE(snapshot.overflowed);
  ASSERT_EQ(snapshot.current_generations.size(), 1U);
  ASSERT_EQ(snapshot.final_triggers.size(), 1U);
  ASSERT_EQ(snapshot.visible_outputs.size(), 2U);
  ASSERT_EQ(snapshot.terminals.size(), 2U);
  const I2ObservedVisibleOutput* preview = nullptr;
  const I2ObservedVisibleOutput* final = nullptr;
  for (const I2ObservedVisibleOutput& visible : snapshot.visible_outputs) {
    if (visible.child.quality == compute::ComputeRunQuality::Interactive) {
      preview = &visible;
    } else {
      final = &visible;
    }
  }
  ASSERT_NE(preview, nullptr);
  ASSERT_NE(final, nullptr);
  const I2ObservedFinalTrigger& trigger = snapshot.final_triggers.front();
  EXPECT_LT(preview->causal_sequence, trigger.causal_sequence);
  EXPECT_LT(trigger.causal_sequence, final->causal_sequence);
  EXPECT_NE(preview->child.run_id, final->child.run_id);
  EXPECT_EQ(preview->child.generation, final->child.generation);
  EXPECT_EQ(preview->child.accepted_coordinate,
            final->child.accepted_coordinate);
  EXPECT_EQ(preview->child.qos.deadline,
            std::optional<std::chrono::steady_clock::time_point>{
                admission + kI2PreviewDeadlineBudget});
  EXPECT_EQ(final->child.qos.deadline,
            std::optional<std::chrono::steady_clock::time_point>{
                admission + kI2FinalDeadlineBudget});
  for (const I2ObservedServiceStart& start : snapshot.service_starts) {
    if (start.child.quality == compute::ComputeRunQuality::Full) {
      EXPECT_LT(trigger.causal_sequence, start.causal_sequence);
    }
  }

  for (const I2ObservedVisibleOutput* visible : {preview, final}) {
    ASSERT_TRUE(visible->value_valid_at_capture);
    ASSERT_TRUE(visible->content_digest.has_value());
    EXPECT_EQ(visible->content_digest->state, ContentDigestState::Available)
        << visible->content_digest->diagnostic;
    ASSERT_TRUE(visible->acquisition.has_value());
    const I2ValueAcquisitionEvidence& access = *visible->acquisition;
    ASSERT_TRUE(access.host_first.plan.has_value());
    ASSERT_TRUE(access.host_second.plan.has_value());
    EXPECT_EQ(access.host_first.plan->kind(), AccessPlanKind::Direct);
    EXPECT_EQ(access.host_second.plan->kind(), AccessPlanKind::Direct);
    EXPECT_EQ(access.host_first.revision, access.host_second.revision);
    EXPECT_EQ(access.host_first.binding, access.host_second.binding);
    EXPECT_EQ(access.host_first.allocation, access.host_second.allocation);
    EXPECT_EQ(access.host_first.storage_bytes,
              access.host_second.storage_bytes);
    EXPECT_EQ(access.host_first.plan->transfer_bytes(), 0U);
    EXPECT_EQ(access.host_second.plan->transfer_bytes(), 0U);
    EXPECT_EQ(access.io_before.active_tasks, access.io_after.active_tasks);
    EXPECT_EQ(access.io_before.active_planned_bytes,
              access.io_after.active_planned_bytes);
    if (access.metal.available) {
      ASSERT_TRUE(access.metal.first.has_value());
      ASSERT_TRUE(access.metal.second.has_value());
      EXPECT_TRUE(access.metal.first->executor_submitted);
      EXPECT_FALSE(access.metal.second->executor_submitted);
      EXPECT_EQ(access.metal.first->revision, access.host_first.revision);
      EXPECT_EQ(access.metal.first->binding, access.metal.second->binding);
      EXPECT_NE(access.metal.first->binding, access.host_first.binding);
    } else {
      EXPECT_EQ(access.metal.unavailable_reason,
                "not-applicable: process Metal executor unavailable");
    }
  }
  const VoidResult closed = host->close_graph(session);
  EXPECT_TRUE(closed.status.ok) << closed.status.message;
}

/**
 * @brief Proves equal-time newer acceptance cancels an older blocked preview
 * before final trigger, so both older child cancellations precede their
 * matching terminals and the older HP child performs no service work.
 * @throws Product, filesystem, allocation, and synchronization failures
 * unchanged to GoogleTest.
 * @note The provider gate establishes the cancellation-before-trigger race
 * without using elapsed performance as a correctness oracle.
 */
TEST(I2ProductPath, NewerEqualTimeAcceptanceSuppressesOlderFinalTrigger) {
  ScopedI2TempDirectory temp;
  I2CurveCallbackGate gate;
  const GraphSessionId session{"i2-pretrigger-cancel"};
  std::unique_ptr<Host> host = make_loaded_i2_host(temp, session);
  ScopedI2CurveObserver observer(gate);
  I2Host* const i2_host = as_i2_host(*host);
  ASSERT_NE(i2_host, nullptr);
  I2EpisodeObservationCollector observations;
  const auto shared_admission = std::chrono::steady_clock::now();

  Result<std::future<OperationStatus>> older =
      admit_reduced_i2(*i2_host, make_reduced_i2_request(session, 0U),
                       observations.make_edit_sink(0U), shared_admission, 1U);
  ASSERT_TRUE(older.status.ok) << older.status.message;
  ASSERT_TRUE(older.value.valid());
  ASSERT_TRUE(gate.wait_for_entry(5s));

  const VoidResult mutated =
      host->set_node_yaml(session, NodeId{1}, i1_edit_node_one_yaml(1U));
  ASSERT_TRUE(mutated.status.ok) << mutated.status.message;
  Result<std::future<OperationStatus>> newer =
      admit_reduced_i2(*i2_host, make_reduced_i2_request(session, 1U),
                       observations.make_edit_sink(1U), shared_admission, 2U);
  ASSERT_TRUE(newer.status.ok) << newer.status.message;
  ASSERT_TRUE(newer.value.valid());
  EXPECT_TRUE(wait_for_i2_pretrigger_cancellation(observations, 0U, 5s));
  gate.release();

  ASSERT_EQ(older.value.wait_for(5s), std::future_status::ready);
  ASSERT_EQ(newer.value.wait_for(5s), std::future_status::ready);
  EXPECT_FALSE(older.value.get().ok);
  EXPECT_TRUE(newer.value.get().ok);
  EXPECT_GT(gate.exited(), 0U);
  const I2EpisodeObservationSnapshot snapshot = observations.snapshot();
  bool old_trigger = false;
  bool old_full_service = false;
  bool old_preview_visible = false;
  bool old_final_visible = false;
  std::map<std::uint64_t, std::uint64_t> old_cancellation_sequences;
  std::map<std::uint64_t, std::uint64_t> old_terminal_sequences;
  for (const I2ObservedFinalTrigger& trigger : snapshot.final_triggers) {
    old_trigger = old_trigger || trigger.child.edit_index == 0U;
  }
  for (const I2ObservedServiceStart& start : snapshot.service_starts) {
    old_full_service =
        old_full_service ||
        (start.child.edit_index == 0U &&
         start.child.quality == compute::ComputeRunQuality::Full);
  }
  for (const I2ObservedVisibleOutput& visible : snapshot.visible_outputs) {
    old_preview_visible =
        old_preview_visible ||
        (visible.child.edit_index == 0U &&
         visible.child.quality == compute::ComputeRunQuality::Interactive);
    old_final_visible =
        old_final_visible ||
        (visible.child.edit_index == 0U &&
         visible.child.quality == compute::ComputeRunQuality::Full);
  }
  for (const I2ObservedCancellation& cancellation : snapshot.cancellations) {
    if (cancellation.child.edit_index == 0U) {
      EXPECT_TRUE(
          old_cancellation_sequences
              .emplace(cancellation.child.run_id, cancellation.causal_sequence)
              .second);
    }
  }
  for (const I2ObservedTerminal& terminal : snapshot.terminals) {
    if (terminal.child.edit_index == 0U &&
        terminal.kind == compute::ComputeRunTerminalKind::Cancelled) {
      EXPECT_TRUE(old_terminal_sequences
                      .emplace(terminal.child.run_id, terminal.causal_sequence)
                      .second);
    }
  }
  EXPECT_FALSE(old_trigger);
  EXPECT_FALSE(old_full_service);
  EXPECT_FALSE(old_preview_visible);
  EXPECT_FALSE(old_final_visible);
  ASSERT_EQ(old_cancellation_sequences.size(), 2U);
  ASSERT_EQ(old_terminal_sequences.size(), 2U);
  for (const auto& [run_id, cancellation_sequence] :
       old_cancellation_sequences) {
    const auto terminal = old_terminal_sequences.find(run_id);
    ASSERT_NE(terminal, old_terminal_sequences.end());
    EXPECT_LT(cancellation_sequence, terminal->second);
  }
  ASSERT_GE(snapshot.current_generations.size(), 2U);
  EXPECT_EQ(
      snapshot.current_generations[0].accepted_coordinate->event_sequence(),
      1U);
  EXPECT_EQ(
      snapshot.current_generations[1].accepted_coordinate->event_sequence(),
      2U);
  const VoidResult closed = host->close_graph(session);
  EXPECT_TRUE(closed.status.ok) << closed.status.message;
}

/**
 * @brief Proves a newer accepted generation revokes an older final after its
 * trigger and physical HP entry but before visible commit.
 * @throws Product, filesystem, allocation, and synchronization failures
 * unchanged to GoogleTest.
 * @note Four preview curve callbacks pass; the first HP curve callback blocks
 * deterministically. This exercises stale-final denial without timing sleeps.
 */
TEST(I2ProductPath, PostTriggerSupersessionRejectsOlderFinalVisibility) {
  ScopedI2TempDirectory temp;
  I2CurveCallbackGate gate(4U);
  const GraphSessionId session{"i2-posttrigger-stale"};
  std::unique_ptr<Host> host = make_loaded_i2_host(temp, session);
  ScopedI2CurveObserver observer(gate);
  I2Host* const i2_host = as_i2_host(*host);
  ASSERT_NE(i2_host, nullptr);
  I2EpisodeObservationCollector observations;

  const auto older_admission = std::chrono::steady_clock::now();
  Result<std::future<OperationStatus>> older =
      admit_reduced_i2(*i2_host, make_reduced_i2_request(session, 0U),
                       observations.make_edit_sink(0U), older_admission, 1U);
  ASSERT_TRUE(older.status.ok) << older.status.message;
  ASSERT_TRUE(older.value.valid());
  ASSERT_TRUE(gate.wait_for_entry(5s));
  {
    const I2EpisodeObservationSnapshot blocked = observations.snapshot();
    bool older_triggered = false;
    bool older_full_started = false;
    for (const I2ObservedFinalTrigger& trigger : blocked.final_triggers) {
      older_triggered = older_triggered || trigger.child.edit_index == 0U;
    }
    for (const I2ObservedServiceStart& start : blocked.service_starts) {
      older_full_started =
          older_full_started ||
          (start.child.edit_index == 0U &&
           start.child.quality == compute::ComputeRunQuality::Full);
    }
    ASSERT_TRUE(older_triggered);
    ASSERT_TRUE(older_full_started);
  }

  const VoidResult mutated =
      host->set_node_yaml(session, NodeId{1}, i1_edit_node_one_yaml(1U));
  ASSERT_TRUE(mutated.status.ok) << mutated.status.message;
  const auto newer_admission = std::chrono::steady_clock::now();
  Result<std::future<OperationStatus>> newer =
      admit_reduced_i2(*i2_host, make_reduced_i2_request(session, 1U),
                       observations.make_edit_sink(1U), newer_admission, 2U);
  ASSERT_TRUE(newer.status.ok) << newer.status.message;
  ASSERT_TRUE(newer.value.valid());
  EXPECT_TRUE(wait_for_i2_cancellation(observations, 0U, 5s));
  gate.release();

  ASSERT_EQ(older.value.wait_for(5s), std::future_status::ready);
  ASSERT_EQ(newer.value.wait_for(5s), std::future_status::ready);
  EXPECT_FALSE(older.value.get().ok);
  EXPECT_TRUE(newer.value.get().ok);
  const I2EpisodeObservationSnapshot settled = observations.snapshot();
  bool older_final_visible = false;
  bool newer_preview_visible = false;
  bool newer_final_visible = false;
  for (const I2ObservedVisibleOutput& visible : settled.visible_outputs) {
    older_final_visible =
        older_final_visible ||
        (visible.child.edit_index == 0U &&
         visible.child.quality == compute::ComputeRunQuality::Full);
    newer_preview_visible =
        newer_preview_visible ||
        (visible.child.edit_index == 1U &&
         visible.child.quality == compute::ComputeRunQuality::Interactive);
    newer_final_visible =
        newer_final_visible ||
        (visible.child.edit_index == 1U &&
         visible.child.quality == compute::ComputeRunQuality::Full);
  }
  EXPECT_FALSE(older_final_visible);
  EXPECT_TRUE(newer_preview_visible);
  EXPECT_TRUE(newer_final_visible);
  const VoidResult closed = host->close_graph(session);
  EXPECT_TRUE(closed.status.ok) << closed.status.message;
}

}  // namespace
}  // namespace ps::benchmark
