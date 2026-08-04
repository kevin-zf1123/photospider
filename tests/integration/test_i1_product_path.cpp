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
#include "runtime/kernel_compute_test_access.hpp"

#if defined(PHOTOSPIDER_INTERNAL_HOST_ASYNC_ADMISSION_TESTING)
namespace ps {

/**
 * @brief Enables or clears deterministic prepared async-admission failure.
 * @param enabled True to fail after complete Host setup and before Kernel
 * entry.
 * @return Nothing.
 * @throws Nothing.
 */
void set_embedded_host_async_admission_failure_for_testing(
    bool enabled) noexcept;

}  // namespace ps
#endif

namespace ps::benchmark {
namespace {

using std::chrono_literals::operator""s;

#if defined(PHOTOSPIDER_INTERNAL_HOST_ASYNC_ADMISSION_TESTING)
/**
 * @brief Owns one lexical prepared-admission failure injection.
 * @throws Nothing.
 * @note Destruction restores the process-local switch before another I1 call or
 * embedded Host teardown.
 */
class ScopedEmbeddedAsyncAdmissionFailure final {
 public:
  /** @brief Enables deterministic pre-Kernel failure. @throws Nothing. */
  ScopedEmbeddedAsyncAdmissionFailure() noexcept {
    ps::set_embedded_host_async_admission_failure_for_testing(true);
  }

  /** @brief Clears deterministic pre-Kernel failure. @throws Nothing. */
  ~ScopedEmbeddedAsyncAdmissionFailure() noexcept {
    ps::set_embedded_host_async_admission_failure_for_testing(false);
  }

  /**
   * @brief Prevents duplicate switch ownership.
   * @param other Guard that retains the installed switch.
   * @throws Nothing because construction is unavailable.
   */
  ScopedEmbeddedAsyncAdmissionFailure(
      const ScopedEmbeddedAsyncAdmissionFailure& other) = delete;

  /**
   * @brief Prevents replacing switch ownership.
   * @param other Guard that retains the installed switch.
   * @return No value because assignment is unavailable.
   * @throws Nothing because assignment is unavailable.
   */
  ScopedEmbeddedAsyncAdmissionFailure& operator=(
      const ScopedEmbeddedAsyncAdmissionFailure& other) = delete;
};
#endif

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
 * @brief Blocks only the first Kernel candidate after generation preparation.
 *
 * The first caller owns its prepared generation but cannot pretrack or publish
 * currentness until release. Every later caller passes the same checkpoint,
 * allowing a higher generation to publish first deterministically.
 *
 * @throws Nothing from product callbacks; synchronization failure terminates.
 * @note The success path acquires both Host result futures before destruction.
 * Destruction opens provider work and joins any outstanding admission caller
 * before clearing the borrowed hook; backend result futures are joined later.
 */
class FirstPreparedCandidateGate final {
 public:
  /**
   * @brief Installs the borrowed Kernel admission hook.
   * @param callback_gate Provider gate released during every cleanup path.
   * @param admission_future Outer Host caller joined before hook removal.
   * @throws Nothing.
   */
  FirstPreparedCandidateGate(CurveCallbackGate& callback_gate,
                             std::future<Result<std::future<OperationStatus>>>&
                                 admission_future) noexcept
      : hook_{this, &FirstPreparedCandidateGate::notify},
        callback_gate_(callback_gate),
        admission_future_(admission_future) {
    testing::set_kernel_compute_admission_test_hook(&hook_);
  }

  /**
   * @brief Releases both product barriers and clears the borrowed hook.
   * @throws Nothing.
   * @note Provider release precedes async-future cleanup, preventing an
   * assertion failure from blocking while a caller waits inside real work.
   */
  ~FirstPreparedCandidateGate() noexcept {
    release();
    callback_gate_.release();
    try {
      if (admission_future_.valid()) {
        admission_future_.wait();
      }
    } catch (...) {
      std::terminate();
    }
    testing::set_kernel_compute_admission_test_hook(nullptr);
  }

  /**
   * @brief Prevents duplicate hook ownership.
   * @param other Guard retaining its borrowed hook.
   * @throws Nothing because copying is unavailable.
   */
  FirstPreparedCandidateGate(const FirstPreparedCandidateGate& other) = delete;

  /**
   * @brief Prevents replacement of hook ownership.
   * @param other Guard that remains unchanged.
   * @return No value because assignment is unavailable.
   * @throws Nothing because assignment is unavailable.
   */
  FirstPreparedCandidateGate& operator=(
      const FirstPreparedCandidateGate& other) = delete;

  /**
   * @brief Waits until the first candidate owns its prepared generation.
   * @param timeout Bounded diagnostic timeout.
   * @return True when the first hook callback entered.
   * @throws std::system_error from synchronization.
   */
  bool wait_for_first(std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> lock(mutex_);
    return changed_.wait_for(lock, timeout, [this] { return entered_ != 0U; });
  }

  /**
   * @brief Releases the first prepared candidate idempotently.
   * @return Nothing.
   * @throws Nothing; synchronization failure terminates.
   */
  void release() noexcept {
    try {
      std::lock_guard<std::mutex> lock(mutex_);
      released_ = true;
      changed_.notify_all();
    } catch (...) {
      std::terminate();
    }
  }

 private:
  /**
   * @brief Handles one exact post-prepare Kernel checkpoint.
   * @param context Borrowed gate owner.
   * @param event Exact candidate-admission checkpoint.
   * @return Nothing after non-first callers pass or first-caller release.
   * @throws Nothing; invalid context or synchronization failure terminates.
   */
  static void notify(void* context,
                     testing::KernelComputeAdmissionTestEvent event) noexcept {
    if (event != testing::KernelComputeAdmissionTestEvent::
                     ProductCandidatePreparedBeforeLineageTracking) {
      return;
    }
    auto* gate = static_cast<FirstPreparedCandidateGate*>(context);
    if (gate == nullptr) {
      std::terminate();
    }
    try {
      std::unique_lock<std::mutex> lock(gate->mutex_);
      ++gate->entered_;
      gate->changed_.notify_all();
      if (gate->entered_ == 1U) {
        gate->changed_.wait(lock, [gate] { return gate->released_; });
      }
    } catch (...) {
      std::terminate();
    }
  }

  /** @brief Borrowed callback aggregate published to Kernel. */
  testing::KernelComputeAdmissionTestHook hook_;
  /** @brief Provider gate released before any async caller is joined. */
  CurveCallbackGate& callback_gate_;
  /** @brief Outer admission caller joined before the hook is cleared. */
  std::future<Result<std::future<OperationStatus>>>& admission_future_;
  /** @brief Serializes callback count and release state. */
  std::mutex mutex_;
  /** @brief Wakes the harness and the first blocked caller. */
  std::condition_variable changed_;
  /** @brief Number of candidate-prepared checkpoints observed. */
  std::size_t entered_ = 0U;
  /** @brief True after the first candidate may continue. */
  bool released_ = false;
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

#if defined(PHOTOSPIDER_INTERNAL_HOST_ASYNC_ADMISSION_TESTING)
/**
 * @brief Proves failed prepared I1 admission binds no product identity or
 * output.
 *
 * @return Nothing; GoogleTest assertions report admission, observation, or
 * recovery mismatches.
 * @throws std::bad_alloc, filesystem, or synchronization exceptions from the
 * real embedded product fixture.
 * @note The first call fails after every Host-side resource is prepared but
 * before Kernel entry. The second call then proves the same common path remains
 * usable and receives generation one with only its own accepted coordinate.
 */
TEST(I1ProductPath, PreparedAdmissionFailureLeavesNoProductBindingAndRecovers) {
  ScopedI1TempDirectory temp;
  std::unique_ptr<Host> host = create_embedded_host();
  ASSERT_NE(host, nullptr);

  const VoidResult seeded = host->seed_builtin_ops();
  ASSERT_TRUE(seeded.status.ok) << seeded.status.message;
  HostExecutionConfig execution_config;
  execution_config.worker_count = 8U;
  const VoidResult configured =
      host->configure_execution_defaults(execution_config);
  ASSERT_TRUE(configured.status.ok) << configured.status.message;

  const std::filesystem::path yaml_path = temp.root() / "i1-rejection.yaml";
  write_reduced_i1_graph(yaml_path);
  GraphLoadRequest load;
  load.session = GraphSessionId{"i1-prepared-admission-rejection"};
  load.root_dir = (temp.root() / "sessions").string();
  load.yaml_path = yaml_path.string();
  load.cache_root_dir = (temp.root() / "cache").string();
  const Result<GraphSessionId> loaded = host->load_graph(load);
  ASSERT_TRUE(loaded.status.ok) << loaded.status.message;

  I1Host* const i1_host = as_i1_host(*host);
  ASSERT_NE(i1_host, nullptr);
  I1EpisodeObservationCollector observations;
  const auto shared_admission_time = std::chrono::steady_clock::now();

  Result<std::future<OperationStatus>> rejected;
  {
    ScopedEmbeddedAsyncAdmissionFailure injected_failure;
    rejected = i1_host->compute_i1_async(I1HostComputeRequest{
        make_reduced_request(load.session, 0U),
        compute::ComputeRunQos{compute::ComputeRunQosClass::Interactive,
                               std::chrono::steady_clock::now() + 5s, 1U, 8U},
        observations.make_edit_sink(0U),
        I1AcceptedCoordinate{shared_admission_time, 1U}});
  }
  EXPECT_FALSE(rejected.status.ok);
  EXPECT_EQ(checked_graph_error_code(rejected.status), GraphErrc::ComputeError);
  EXPECT_NE(rejected.status.message.find(
                "injected embedded Host async admission preparation failure"),
            std::string::npos);
  EXPECT_FALSE(rejected.value.valid());

  const I1EpisodeObservationSnapshot rejected_snapshot =
      observations.snapshot();
  EXPECT_FALSE(rejected_snapshot.overflowed);
  EXPECT_TRUE(rejected_snapshot.current_generations.empty());
  EXPECT_TRUE(rejected_snapshot.service_starts.empty());
  EXPECT_TRUE(rejected_snapshot.cancellations.empty());
  EXPECT_TRUE(rejected_snapshot.terminals.empty());
  EXPECT_TRUE(rejected_snapshot.visible_outputs.empty());
  EXPECT_TRUE(rejected_snapshot.run_quiescences.empty());
  EXPECT_TRUE(rejected_snapshot.resource_settlements.empty());
  EXPECT_TRUE(rejected_snapshot.host_settlements.empty());

  Result<std::future<OperationStatus>> recovered =
      i1_host->compute_i1_async(I1HostComputeRequest{
          make_reduced_request(load.session, 1U),
          compute::ComputeRunQos{compute::ComputeRunQosClass::Interactive,
                                 std::chrono::steady_clock::now() + 5s, 1U, 8U},
          observations.make_edit_sink(1U),
          I1AcceptedCoordinate{shared_admission_time, 2U}});
  ASSERT_TRUE(recovered.status.ok) << recovered.status.message;
  ASSERT_TRUE(recovered.value.valid());
  ASSERT_EQ(recovered.value.wait_for(5s), std::future_status::ready);
  const OperationStatus recovered_status = recovered.value.get();
  EXPECT_TRUE(recovered_status.ok) << recovered_status.message;

  const VoidResult closed = host->close_graph(load.session);
  ASSERT_TRUE(closed.status.ok) << closed.status.message;
  const I1EpisodeObservationSnapshot recovered_snapshot =
      observations.snapshot();
  ASSERT_FALSE(recovered_snapshot.overflowed);
  ASSERT_EQ(recovered_snapshot.current_generations.size(), 1U);
  const I1ObservedCurrentGeneration& current =
      recovered_snapshot.current_generations.front();
  EXPECT_EQ(current.edit_index, 1U);
  EXPECT_EQ(current.generation, 1U);
  ASSERT_TRUE(current.accepted_coordinate.has_value());
  EXPECT_EQ(current.accepted_coordinate->admission_time(),
            shared_admission_time);
  EXPECT_EQ(current.accepted_coordinate->event_sequence(), 2U);
  EXPECT_TRUE(recovered_snapshot.cancellations.empty());
  ASSERT_EQ(recovered_snapshot.terminals.size(), 1U);
  EXPECT_EQ(recovered_snapshot.terminals.front().edit_index, 1U);
  EXPECT_EQ(recovered_snapshot.terminals.front().kind,
            compute::ComputeRunTerminalKind::Succeeded);
  ASSERT_EQ(recovered_snapshot.visible_outputs.size(), 1U);
  EXPECT_EQ(recovered_snapshot.visible_outputs.front().edit_index, 1U);
  ASSERT_EQ(recovered_snapshot.host_settlements.size(), 1U);
  EXPECT_EQ(recovered_snapshot.host_settlements.front().edit_index, 1U);
  ASSERT_FALSE(recovered_snapshot.service_starts.empty());
  for (const I1ObservedServiceStart& start :
       recovered_snapshot.service_starts) {
    EXPECT_EQ(start.edit_index, 1U);
  }
  for (const I1ObservedRunLifecycleTransition& transition :
       recovered_snapshot.run_quiescences) {
    EXPECT_EQ(transition.edit_index, 1U);
  }
  for (const I1ObservedRunLifecycleTransition& transition :
       recovered_snapshot.resource_settlements) {
    EXPECT_EQ(transition.edit_index, 1U);
  }
}
#endif

/**
 * @brief Reproduces inverse generation/accepted-coordinate publication through
 * the real Host and Kernel path.
 *
 * @return Nothing; GoogleTest reports currentness, cancellation, or settlement
 * regressions.
 * @throws Embedded product, filesystem, allocation, and synchronization
 * failures unchanged to GoogleTest.
 * @note The later coordinate prepares generation one and pauses at a Kernel
 * barrier. The earlier coordinate then prepares generation two, becomes
 * current, and blocks in real provider work. Releasing generation one must
 * make the later coordinate current despite its lower scalar generation.
 */
TEST(I1ProductPath,
     LaterAcceptedCoordinateWinsWhenGenerationPreparationOrderIsInverse) {
  ScopedI1TempDirectory temp;
  CurveCallbackGate curve_gate;
  std::unique_ptr<Host> host = create_embedded_host();
  ASSERT_NE(host, nullptr);
  ScopedCurveObserver curve_observer(curve_gate);

  const VoidResult seeded = host->seed_builtin_ops();
  ASSERT_TRUE(seeded.status.ok) << seeded.status.message;
  HostExecutionConfig execution_config;
  execution_config.worker_count = 8U;
  const VoidResult configured =
      host->configure_execution_defaults(execution_config);
  ASSERT_TRUE(configured.status.ok) << configured.status.message;

  const std::filesystem::path yaml_path = temp.root() / "i1-inverse.yaml";
  write_reduced_i1_graph(yaml_path);
  GraphLoadRequest load;
  load.session = GraphSessionId{"i1-inverse-publication-order"};
  load.root_dir = (temp.root() / "sessions").string();
  load.yaml_path = yaml_path.string();
  load.cache_root_dir = (temp.root() / "cache").string();
  const Result<GraphSessionId> loaded = host->load_graph(load);
  ASSERT_TRUE(loaded.status.ok) << loaded.status.message;

  I1Host* const i1_host = as_i1_host(*host);
  ASSERT_NE(i1_host, nullptr);
  I1EpisodeObservationCollector observations;
  const auto shared_admission_time = std::chrono::steady_clock::now();
  const auto deadline = std::chrono::steady_clock::now() + 5s;
  std::future<Result<std::future<OperationStatus>>> later_submission;

  const VoidResult final_mutation =
      host->set_node_yaml(load.session, NodeId{1}, i1_edit_node_one_yaml(11U));
  ASSERT_TRUE(final_mutation.status.ok) << final_mutation.status.message;

  Result<std::future<OperationStatus>> later_coordinate;
  Result<std::future<OperationStatus>> earlier_coordinate;
  {
    FirstPreparedCandidateGate admission_gate(curve_gate, later_submission);
    later_submission = std::async(std::launch::async, [&] {
      return i1_host->compute_i1_async(I1HostComputeRequest{
          make_reduced_request(load.session, 11U),
          compute::ComputeRunQos{compute::ComputeRunQosClass::Interactive,
                                 deadline, 1U, 8U},
          observations.make_edit_sink(11U),
          I1AcceptedCoordinate{shared_admission_time, 2U}});
    });
    ASSERT_TRUE(admission_gate.wait_for_first(std::chrono::seconds(5)));

    const VoidResult earlier_mutation =
        host->set_node_yaml(load.session, NodeId{1}, i1_edit_node_one_yaml(0U));
    ASSERT_TRUE(earlier_mutation.status.ok) << earlier_mutation.status.message;
    earlier_coordinate = i1_host->compute_i1_async(I1HostComputeRequest{
        make_reduced_request(load.session, 0U),
        compute::ComputeRunQos{compute::ComputeRunQosClass::Interactive,
                               deadline, 1U, 8U},
        observations.make_edit_sink(0U),
        I1AcceptedCoordinate{shared_admission_time, 1U}});
    ASSERT_TRUE(earlier_coordinate.status.ok)
        << earlier_coordinate.status.message;
    ASSERT_TRUE(earlier_coordinate.value.valid());
    ASSERT_TRUE(curve_gate.wait_for_entry(std::chrono::seconds(5)));

    const VoidResult restored_final = host->set_node_yaml(
        load.session, NodeId{1}, i1_edit_node_one_yaml(11U));
    ASSERT_TRUE(restored_final.status.ok) << restored_final.status.message;
    admission_gate.release();
    ASSERT_EQ(later_submission.wait_for(std::chrono::seconds(5)),
              std::future_status::ready);
    later_coordinate = later_submission.get();
    ASSERT_TRUE(later_coordinate.status.ok) << later_coordinate.status.message;
    ASSERT_TRUE(later_coordinate.value.valid());
    ASSERT_TRUE(wait_for_supersession(observations, std::chrono::seconds(5)));
  }

  curve_gate.release();
  ASSERT_EQ(earlier_coordinate.value.wait_for(std::chrono::seconds(5)),
            std::future_status::ready);
  ASSERT_EQ(later_coordinate.value.wait_for(std::chrono::seconds(5)),
            std::future_status::ready);
  const OperationStatus earlier_status = earlier_coordinate.value.get();
  const OperationStatus later_status = later_coordinate.value.get();
  EXPECT_FALSE(earlier_status.ok);
  EXPECT_TRUE(later_status.ok) << later_status.message;

  const I1EpisodeObservationSnapshot snapshot = observations.snapshot();
  ASSERT_FALSE(snapshot.overflowed);
  ASSERT_EQ(snapshot.current_generations.size(), 2U);
  const I1ObservedCurrentGeneration& first_current =
      snapshot.current_generations[0U];
  const I1ObservedCurrentGeneration& second_current =
      snapshot.current_generations[1U];
  EXPECT_EQ(first_current.edit_index, 0U);
  EXPECT_EQ(first_current.generation, 2U);
  EXPECT_EQ(second_current.edit_index, 11U);
  EXPECT_EQ(second_current.generation, 1U);
  ASSERT_TRUE(first_current.accepted_coordinate.has_value());
  ASSERT_TRUE(second_current.accepted_coordinate.has_value());
  EXPECT_EQ(first_current.accepted_coordinate->admission_time(),
            shared_admission_time);
  EXPECT_EQ(second_current.accepted_coordinate->admission_time(),
            shared_admission_time);
  EXPECT_EQ(first_current.accepted_coordinate->event_sequence(), 1U);
  EXPECT_EQ(second_current.accepted_coordinate->event_sequence(), 2U);
  EXPECT_LT(*first_current.accepted_coordinate,
            *second_current.accepted_coordinate);

  ASSERT_EQ(snapshot.cancellations.size(), 1U);
  EXPECT_EQ(snapshot.cancellations.front().edit_index, 0U);
  EXPECT_EQ(snapshot.cancellations.front().generation, 2U);
  ASSERT_EQ(snapshot.visible_outputs.size(), 1U);
  EXPECT_EQ(snapshot.visible_outputs.front().edit_index, 11U);
  EXPECT_EQ(snapshot.visible_outputs.front().generation, 1U);

  const VoidResult closed = host->close_graph(load.session);
  EXPECT_TRUE(closed.status.ok) << closed.status.message;
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
  const auto shared_admission_time = std::chrono::steady_clock::now();

  const auto first_deadline = std::chrono::steady_clock::now() + 5s;
  Result<std::future<OperationStatus>> first =
      i1_host->compute_i1_async(I1HostComputeRequest{
          make_reduced_request(load.session, 0U),
          compute::ComputeRunQos{compute::ComputeRunQosClass::Interactive,
                                 first_deadline, 1U, 8U},
          observations.make_edit_sink(0U),
          I1AcceptedCoordinate{shared_admission_time, 1U}});
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
          observations.make_edit_sink(11U),
          I1AcceptedCoordinate{shared_admission_time, 2U}});
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
  ASSERT_TRUE(snapshot.current_generations[0].accepted_coordinate.has_value());
  ASSERT_TRUE(snapshot.current_generations[1].accepted_coordinate.has_value());
  EXPECT_EQ(
      snapshot.current_generations[0].accepted_coordinate->admission_time(),
      shared_admission_time);
  EXPECT_EQ(
      snapshot.current_generations[1].accepted_coordinate->admission_time(),
      shared_admission_time);
  EXPECT_EQ(
      snapshot.current_generations[0].accepted_coordinate->event_sequence(),
      1U);
  EXPECT_EQ(
      snapshot.current_generations[1].accepted_coordinate->event_sequence(),
      2U);
  EXPECT_EQ(snapshot.current_generations[0].causal_sequence, 1U);
  EXPECT_NE(
      snapshot.current_generations[1].causal_sequence,
      snapshot.current_generations[1].accepted_coordinate->event_sequence());

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
