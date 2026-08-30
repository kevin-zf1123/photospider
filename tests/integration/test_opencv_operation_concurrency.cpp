#include <fenv.h>  // NOLINT(build/c++11)
#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <future>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

#include "benchmark/benchmark_service.hpp"
#include "core/ps_types.hpp"  // NOLINT(build/include_subdir)
#include "graph/node.hpp"     // NOLINT(build/include_subdir)
#include "photospider/data/image_view.hpp"
#include "photospider/host/host.hpp"
#include "providers/opencv/opencv_operation_provider_test_access.hpp"
#include "support/host_spy.hpp"

namespace ps {
namespace {

/** @brief Operation type used by the benchmark worker-control fixture. */
constexpr char kBenchmarkProbeType[] = "benchmark_worker_probe";

/** @brief Blocking source subtype used to expose physical execution workers. */
constexpr char kBenchmarkProbeSourceSubtype[] = "source";

/** @brief Non-blocking sink subtype joining every independent source. */
constexpr char kBenchmarkProbeSinkSubtype[] = "sink";

/** @brief Number of independent sources available to the execution probe. */
constexpr int kBenchmarkProbeSourceCount = 16;

/** @brief Node id of the sink targeted by benchmark execution. */
constexpr int kBenchmarkProbeSinkId = kBenchmarkProbeSourceCount + 1;

/**
 * @brief Owns an isolated filesystem root for one benchmark integration test.
 *
 * @throws std::filesystem::filesystem_error if construction cannot create the
 *         temporary directory.
 * @note Destruction performs best-effort recursive cleanup and never throws.
 */
class ScopedBenchmarkTempDir final {
 public:
  /**
   * @brief Creates a process-unique temporary directory.
   * @param label Stable diagnostic prefix for the test fixture.
   * @throws std::filesystem::filesystem_error if directory creation fails.
   * @throws std::bad_alloc if path construction exhausts memory.
   */
  explicit ScopedBenchmarkTempDir(const std::string& label) {
    static std::atomic<unsigned int> sequence{0U};
    const unsigned int id = sequence.fetch_add(1U, std::memory_order_relaxed);
    root_ = std::filesystem::temp_directory_path() /
            (label + "_" + std::to_string(id));
    std::filesystem::remove_all(root_);
    std::filesystem::create_directories(root_);
  }

  /**
   * @brief Removes the owned temporary directory.
   * @throws Nothing; cleanup errors are deliberately ignored at this boundary.
   */
  ~ScopedBenchmarkTempDir() noexcept {
    std::error_code error;
    std::filesystem::remove_all(root_, error);
  }

  /**
   * @brief Prevents duplicate temporary-root ownership.
   * @param other Owner that retains its root.
   * @throws Nothing because copying is unavailable.
   */
  ScopedBenchmarkTempDir(const ScopedBenchmarkTempDir& other) = delete;

  /**
   * @brief Prevents replacing temporary-root ownership.
   * @param other Owner that remains unchanged.
   * @return No value because assignment is unavailable.
   * @throws Nothing because assignment is unavailable.
   */
  ScopedBenchmarkTempDir& operator=(const ScopedBenchmarkTempDir& other) =
      delete;

  /**
   * @brief Returns the owned directory path.
   * @return Immutable temporary root.
   * @throws Nothing.
   */
  const std::filesystem::path& root() const noexcept { return root_; }

 private:
  /** @brief Process-unique root removed by this owner. */
  std::filesystem::path root_;
};

/**
 * @brief Restores the calling test thread's complete floating-point state.
 *
 * @throws std::runtime_error when the initial environment cannot be captured.
 * @note Destruction restores rounding, sticky exception flags, and all other
 * environment controls. Restoration failure terminates because contaminating
 * a later test would make the suite result order-dependent.
 */
class ScopedTestFloatingPointEnvironment final {
 public:
  /**
   * @brief Captures the complete environment of the current test thread.
   * @throws std::runtime_error when `fegetenv` cannot capture that state.
   */
  ScopedTestFloatingPointEnvironment() {
    if (fegetenv(&initial_environment_) != 0) {
      throw std::runtime_error(
          "cannot capture the test thread floating-point environment");
    }
  }

  /**
   * @brief Restores the complete environment captured at construction.
   * @throws Nothing; restoration failure terminates the process.
   */
  ~ScopedTestFloatingPointEnvironment() noexcept {
    if (fesetenv(&initial_environment_) != 0) {
      std::terminate();
    }
  }

  /**
   * @brief Prevents duplicate ownership of test-thread restoration.
   * @param other Guard retaining restoration responsibility.
   * @throws Nothing because copying is unavailable.
   */
  ScopedTestFloatingPointEnvironment(
      const ScopedTestFloatingPointEnvironment& other) = delete;

  /**
   * @brief Prevents replacing test-thread restoration ownership.
   * @param other Guard that remains responsible for its captured environment.
   * @return No value because assignment is unavailable.
   * @throws Nothing because assignment is unavailable.
   */
  ScopedTestFloatingPointEnvironment& operator=(
      const ScopedTestFloatingPointEnvironment& other) = delete;

 private:
  /** @brief Complete calling-thread environment restored after the test. */
  fenv_t initial_environment_{};
};

/**
 * @brief Blocks real operation callbacks and records physical concurrency.
 *
 * @throws std::system_error if mutex or condition-variable operations fail.
 * @note The owning test must release every callback before destroying this
 *       object or clearing the published borrowed pointer.
 */
class CallbackConcurrencyGate final {
 public:
  /**
   * @brief Starts a fresh blocking observation phase.
   * @return Nothing.
   * @throws std::system_error if mutex locking fails.
   * @throws std::logic_error if callbacks from an earlier phase remain active.
   */
  void begin() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (active_callbacks_ != 0U) {
      throw std::logic_error("cannot restart an active callback gate");
    }
    callback_threads_.clear();
    max_active_callbacks_ = 0U;
    released_ = false;
  }

  /**
   * @brief Records and blocks one real callback until explicit release.
   * @return Nothing.
   * @throws std::system_error if synchronization fails.
   * @throws std::bad_alloc if thread-identity storage cannot grow.
   * @note Active counts bracket the registered callback body reached through
   *       the private product execution route.
   */
  void enter_and_wait() {
    enter_and_block();
    leave();
  }

  /**
   * @brief Records entry and blocks while retaining the active count.
   * @return Nothing.
   * @throws std::system_error if synchronization fails.
   * @throws std::bad_alloc if thread-identity storage cannot grow.
   * @note Observer clients pair this method with `leave()` after the real
   *       OpenCV callback body exits.
   */
  void enter_and_block() {
    std::unique_lock<std::mutex> lock(mutex_);
    callback_threads_.push_back(std::this_thread::get_id());
    ++active_callbacks_;
    max_active_callbacks_ = std::max(max_active_callbacks_, active_callbacks_);
    cv_.notify_all();
    cv_.wait(lock, [this] { return released_; });
  }

  /**
   * @brief Records exit from a callback previously admitted by this gate.
   * @return Nothing.
   * @throws std::system_error if mutex locking fails.
   * @note Exactly one exit must pair with every successful entry.
   */
  void leave() {
    std::lock_guard<std::mutex> lock(mutex_);
    --active_callbacks_;
    cv_.notify_all();
  }

  /**
   * @brief Releases all current and later callbacks in this phase.
   * @return Nothing.
   * @throws Nothing; synchronization failure terminates at the cleanup seam.
   * @note The operation is idempotent for explicit and RAII cleanup.
   */
  void release() noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    released_ = true;
    cv_.notify_all();
  }

  /**
   * @brief Waits for a minimum number of simultaneous callbacks.
   * @param count Required active callback count.
   * @param timeout Maximum monotonic wait duration.
   * @return True if the requested count is reached before timeout.
   * @throws std::system_error if synchronization fails.
   */
  bool wait_for_active(std::size_t count, std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> lock(mutex_);
    return cv_.wait_for(lock, timeout,
                        [this, count] { return active_callbacks_ >= count; });
  }

  /**
   * @brief Returns peak callback concurrency in the current phase.
   * @return Exact maximum simultaneous callback count.
   * @throws std::system_error if mutex locking fails.
   */
  std::size_t max_active_callbacks() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return max_active_callbacks_;
  }

  /**
   * @brief Counts distinct physical callback threads in the current phase.
   * @return Number of unique execution-worker thread identities.
   * @throws std::system_error if mutex locking fails.
   * @throws std::bad_alloc if snapshot storage cannot allocate.
   */
  std::size_t unique_callback_threads() const {
    std::vector<std::thread::id> snapshot;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      snapshot = callback_threads_;
    }
    std::sort(snapshot.begin(), snapshot.end());
    return static_cast<std::size_t>(std::distance(
        snapshot.begin(), std::unique(snapshot.begin(), snapshot.end())));
  }

 private:
  /** @brief Protects every observation and release predicate. */
  mutable std::mutex mutex_;

  /** @brief Notifies tests and callback workers about gate transitions. */
  std::condition_variable cv_;

  /** @brief Real callback thread identities in entry order. */
  std::vector<std::thread::id> callback_threads_;

  /** @brief Callbacks currently blocked inside the fixture operation. */
  std::size_t active_callbacks_ = 0U;

  /** @brief Largest simultaneous callback count since `begin()`. */
  std::size_t max_active_callbacks_ = 0U;

  /** @brief Predicate that permits callbacks to leave the gate. */
  bool released_ = true;
};

/** @brief Borrowed gate reached by the process-persistent fixture callback. */
std::atomic<CallbackConcurrencyGate*> g_callback_gate{nullptr};

/**
 * @brief Publishes one borrowed callback gate for a lexical test scope.
 *
 * @throws std::logic_error if another gate is already published.
 * @note The gate must outlive this guard and every callback that can load it.
 */
class ScopedCallbackGatePublication final {
 public:
  /**
   * @brief Publishes the borrowed gate.
   * @param gate Test-owned state retained beyond guard destruction.
   * @throws std::logic_error if another test already owns publication.
   */
  explicit ScopedCallbackGatePublication(CallbackConcurrencyGate& gate)
      : gate_(&gate) {
    CallbackConcurrencyGate* expected = nullptr;
    if (!g_callback_gate.compare_exchange_strong(expected, gate_,
                                                 std::memory_order_acq_rel,
                                                 std::memory_order_acquire)) {
      throw std::logic_error("callback gate is already published");
    }
  }

  /**
   * @brief Clears publication after all callback work has joined.
   * @throws Nothing.
   */
  ~ScopedCallbackGatePublication() noexcept {
    CallbackConcurrencyGate* expected = gate_;
    (void)g_callback_gate.compare_exchange_strong(expected, nullptr,
                                                  std::memory_order_acq_rel,
                                                  std::memory_order_acquire);
  }

  /**
   * @brief Prevents duplicate publication ownership.
   * @param other Guard retaining clearing responsibility.
   * @throws Nothing because copying is unavailable.
   */
  ScopedCallbackGatePublication(const ScopedCallbackGatePublication& other) =
      delete;

  /**
   * @brief Prevents replacing publication ownership.
   * @param other Guard that remains unchanged.
   * @return No value because assignment is unavailable.
   * @throws Nothing because assignment is unavailable.
   */
  ScopedCallbackGatePublication& operator=(
      const ScopedCallbackGatePublication& other) = delete;

 private:
  /** @brief Borrowed gate expected when this guard clears publication. */
  CallbackConcurrencyGate* gate_;
};

/**
 * @brief Releases a callback gate on every lexical exit.
 *
 * @throws Nothing after construction.
 * @note Declare the compute future before this guard so reverse destruction
 *       releases workers before a future destructor can wait for completion.
 */
class ScopedCallbackRelease final {
 public:
  /**
   * @brief Starts a blocking phase owned by this guard.
   * @param gate Gate to release during cleanup.
   * @throws std::system_error if phase initialization cannot lock.
   * @throws std::logic_error if another phase remains active.
   */
  explicit ScopedCallbackRelease(CallbackConcurrencyGate& gate) : gate_(&gate) {
    gate_->begin();
  }

  /** @brief Releases the gate idempotently. @throws Nothing. */
  ~ScopedCallbackRelease() noexcept { release(); }

  /**
   * @brief Releases the gate before an explicit future join.
   * @return Nothing.
   * @throws Nothing.
   */
  void release() noexcept {
    if (gate_ != nullptr) {
      gate_->release();
      gate_ = nullptr;
    }
  }

  /**
   * @brief Prevents duplicate release ownership.
   * @param other Guard retaining cleanup responsibility.
   * @throws Nothing because copying is unavailable.
   */
  ScopedCallbackRelease(const ScopedCallbackRelease& other) = delete;

  /**
   * @brief Prevents replacing release ownership.
   * @param other Guard that remains unchanged.
   * @return No value because assignment is unavailable.
   * @throws Nothing because assignment is unavailable.
   */
  ScopedCallbackRelease& operator=(const ScopedCallbackRelease& other) = delete;

 private:
  /** @brief Borrowed gate, or null after explicit release. */
  CallbackConcurrencyGate* gate_;
};

/**
 * @brief Adapts the built-in callback observer to one curve-operation gate.
 *
 * @throws Nothing from observer methods; synchronization failures terminate
 *         the test process rather than perturbing product callback semantics.
 * @note Other built-in keys pass through without changing gate state.
 */
class CurveOperationObserver final
    : public providers::opencv::OpenCvOperationObserver {
 public:
  /**
   * @brief Binds the observer to test-owned blocking state.
   * @param gate Gate that outlives observer publication and all callbacks.
   * @throws Nothing.
   */
  explicit CurveOperationObserver(CallbackConcurrencyGate& gate) noexcept
      : gate_(&gate) {}

  /** @copydoc providers::opencv::OpenCvOperationObserver::on_enter */
  void on_enter(const char* operation_key) noexcept override {
    if (std::strcmp(operation_key, "image_process:curve_transform") == 0) {
      gate_->enter_and_block();
    }
  }

  /** @copydoc providers::opencv::OpenCvOperationObserver::on_exit */
  void on_exit(const char* operation_key) noexcept override {
    if (std::strcmp(operation_key, "image_process:curve_transform") == 0) {
      gate_->leave();
    }
  }

 private:
  /** @brief Borrowed gate retained for the observer lifetime. */
  CallbackConcurrencyGate* gate_;
};

/**
 * @brief Publishes one built-in OpenCV observer for a lexical scope.
 *
 * @throws Nothing.
 * @note Tests serialize use and join all computes before guard destruction.
 */
class ScopedOpenCvObserverPublication final {
 public:
  /**
   * @brief Publishes a borrowed observer.
   * @param observer Observer that outlives this guard.
   * @throws Nothing.
   */
  explicit ScopedOpenCvObserverPublication(
      providers::opencv::OpenCvOperationObserver& observer) noexcept {
    providers::opencv::set_opencv_operation_observer_for_testing(&observer);
  }

  /** @brief Clears observer publication. @throws Nothing. */
  ~ScopedOpenCvObserverPublication() noexcept {
    providers::opencv::set_opencv_operation_observer_for_testing(nullptr);
  }

  /**
   * @brief Prevents duplicate clearing ownership.
   * @param other Guard retaining publication responsibility.
   * @throws Nothing because copying is unavailable.
   */
  ScopedOpenCvObserverPublication(
      const ScopedOpenCvObserverPublication& other) = delete;

  /**
   * @brief Prevents replacing observer publication ownership.
   * @param other Guard that remains unchanged.
   * @return No value because assignment is unavailable.
   * @throws Nothing because assignment is unavailable.
   */
  ScopedOpenCvObserverPublication& operator=(
      const ScopedOpenCvObserverPublication& other) = delete;
};

/**
 * @brief Registers blocking sources and a non-blocking join operation.
 * @return Nothing.
 * @throws std::bad_alloc if registry key or callback storage allocation fails.
 * @note Registration is process-persistent and idempotent; the borrowed gate
 *       controls observation lifetime separately. Both callbacks explicitly
 *       declare an empty output schema because they observe scheduling only.
 */
void ensure_benchmark_probe_registered() {
  static std::once_flag once;
  std::call_once(once, [] {
    OpMetadata no_output_metadata;
    no_output_metadata.produces_image = false;
    OpRegistry::instance().register_op_hp_monolithic(
        kBenchmarkProbeType, kBenchmarkProbeSourceSubtype,
        MonolithicOpFunc(
            [](const Node&, const std::vector<const NodeOutput*>&) {
              CallbackConcurrencyGate* gate =
                  g_callback_gate.load(std::memory_order_acquire);
              if (gate != nullptr) {
                gate->enter_and_wait();
              }
              return NodeOutput{};
            }),
        no_output_metadata);
    OpRegistry::instance().register_op_hp_monolithic(
        kBenchmarkProbeType, kBenchmarkProbeSinkSubtype,
        MonolithicOpFunc(
            [](const Node&, const std::vector<const NodeOutput*>&) {
              return NodeOutput{};
            }),
        no_output_metadata);
  });
}

/**
 * @brief Writes sixteen independent sources feeding one benchmark target.
 * @param path YAML destination whose parent directory is created.
 * @return Nothing.
 * @throws std::filesystem::filesystem_error if directory creation fails.
 * @throws std::runtime_error if the file cannot be written completely.
 * @throws std::bad_alloc if path or stream storage exhausts memory.
 */
void write_benchmark_probe_graph(const std::filesystem::path& path) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream output(path);
  if (!output) {
    throw std::runtime_error("failed to open benchmark worker probe YAML");
  }
  for (int node_id = 1; node_id <= kBenchmarkProbeSourceCount; ++node_id) {
    output << "- id: " << node_id << "\n"
           << "  name: benchmark_worker_source_" << node_id << "\n"
           << "  type: " << kBenchmarkProbeType << "\n"
           << "  subtype: " << kBenchmarkProbeSourceSubtype << "\n"
           << "  parameters: {}\n";
  }
  output << "- id: " << kBenchmarkProbeSinkId << "\n"
         << "  name: benchmark_worker_sink\n"
         << "  type: " << kBenchmarkProbeType << "\n"
         << "  subtype: " << kBenchmarkProbeSinkSubtype << "\n"
         << "  image_inputs:\n";
  for (int node_id = 1; node_id <= kBenchmarkProbeSourceCount; ++node_id) {
    output << "    - from_node_id: " << node_id << "\n";
  }
  output << "  parameters: {}\n";
  output.close();
  if (!output) {
    throw std::runtime_error("failed to write benchmark worker probe YAML");
  }
}

/**
 * @brief Writes mixed enabled, disabled, valid, invalid, and automatic
 * sessions.
 * @param path Benchmark configuration destination.
 * @return Nothing.
 * @throws std::runtime_error if the file cannot be written completely.
 * @throws std::bad_alloc if path or stream storage exhausts memory.
 * @note Every enabled valid session uses one repetition of the same custom
 *       probe Graph. The disabled out-of-range numeric session proves its
 *       thread range is outside RunAll preflight; the enabled invalid session
 *       exercises skip diagnostics without blocking later valid sessions.
 */
void write_mixed_benchmark_config(const std::filesystem::path& path) {
  std::ofstream output(path);
  if (!output) {
    throw std::runtime_error("failed to open mixed benchmark config");
  }
  output << "sessions:\n"
         << "  - name: one_thread\n"
         << "    enabled: true\n"
         << "    auto_generate: false\n"
         << "    yaml_path: probe.yaml\n"
         << "    execution: {runs: 1, threads: 1, parallel: true}\n"
         << "  - name: disabled_invalid\n"
         << "    enabled: false\n"
         << "    auto_generate: false\n"
         << "    yaml_path: probe.yaml\n"
         << "    execution: {runs: 1, threads: 9, parallel: true}\n"
         << "  - name: enabled_invalid\n"
         << "    enabled: true\n"
         << "    auto_generate: false\n"
         << "    yaml_path: probe.yaml\n"
         << "    execution: {runs: 1, threads: 9, parallel: true}\n"
         << "  - name: two_threads\n"
         << "    enabled: true\n"
         << "    auto_generate: false\n"
         << "    yaml_path: probe.yaml\n"
         << "    execution: {runs: 1, threads: 2, parallel: true}\n"
         << "  - name: automatic_threads\n"
         << "    enabled: true\n"
         << "    auto_generate: false\n"
         << "    yaml_path: probe.yaml\n"
         << "    execution: {runs: 1, parallel: true}\n";
  output.close();
  if (!output) {
    throw std::runtime_error("failed to write mixed benchmark config");
  }
}

/**
 * @brief Builds one custom benchmark config targeting the probe sink.
 * @param yaml_path Absolute source Graph path.
 * @param worker_count Exact benchmark execution-worker request.
 * @return Enabled single-run parallel benchmark configuration.
 * @throws std::bad_alloc if string copies allocate.
 */
BenchmarkSessionConfig make_probe_benchmark_config(
    const std::filesystem::path& yaml_path, int worker_count) {
  BenchmarkSessionConfig config;
  config.name = "benchmark_worker_control_" + std::to_string(worker_count);
  config.enabled = true;
  config.auto_generate = false;
  config.yaml_path = yaml_path.string();
  config.execution.runs = 1;
  config.execution.threads = worker_count;
  config.execution.parallel = true;
  return config;
}

/**
 * @brief Builds an auto-generated tiled curve benchmark configuration.
 * @param worker_count Exact benchmark execution-worker request.
 * @return Single-run configuration with enough 256-pixel tiles for eight
 *         workers.
 * @throws std::bad_alloc if string assignment exhausts memory.
 */
BenchmarkSessionConfig make_curve_benchmark_config(int worker_count) {
  BenchmarkSessionConfig config;
  config.name = "opencv_curve_concurrency_" + std::to_string(worker_count);
  config.enabled = true;
  config.auto_generate = true;
  config.generator_config.input_op_type = "image_generator:constant";
  config.generator_config.main_op_type = "image_process:curve_transform";
  config.generator_config.output_op_type = "analyzer:get_dimensions";
  config.generator_config.width = 1024;
  config.generator_config.height = 1024;
  config.generator_config.chain_length = 1;
  config.generator_config.num_outputs = 1;
  config.execution.runs = 1;
  config.execution.threads = worker_count;
  config.execution.parallel = true;
  return config;
}

/**
 * @brief Resolves a test benchmark request by the public bounded-auto rule.
 * @param configured_threads Zero for automatic selection or an exact grant.
 * @return Expected nonzero worker grant no greater than eight.
 * @throws Nothing.
 */
std::size_t expected_benchmark_workers(int configured_threads) noexcept {
  if (configured_threads > 0) {
    return static_cast<std::size_t>(configured_threads);
  }
  return static_cast<std::size_t>(
      std::max(1U, std::min(std::thread::hardware_concurrency(), 8U)));
}

/**
 * @brief Writes a deterministic constant-to-curve image Graph.
 * @param path YAML destination whose parent directory is created.
 * @return Nothing.
 * @throws std::filesystem::filesystem_error if directory creation fails.
 * @throws std::runtime_error if the destination cannot be written completely.
 * @throws std::bad_alloc if path or stream storage exhausts memory.
 */
void write_curve_output_graph(const std::filesystem::path& path) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream output(path);
  if (!output) {
    throw std::runtime_error("failed to open deterministic curve Graph");
  }
  output << "- id: 0\n"
         << "  name: deterministic_constant\n"
         << "  type: image_generator\n"
         << "  subtype: constant\n"
         << "  parameters:\n"
         << "    width: 512\n"
         << "    height: 512\n"
         << "    value: 64\n"
         << "    channels: 1\n"
         << "- id: 1\n"
         << "  name: deterministic_curve\n"
         << "  type: image_process\n"
         << "  subtype: curve_transform\n"
         << "  image_inputs:\n"
         << "    - from_node_id: 0\n"
         << "  parameters:\n"
         << "    k: 1.75\n";
  output.close();
  if (!output) {
    throw std::runtime_error("failed to write deterministic curve Graph");
  }
}

/**
 * @brief Publishes one tightly packed single-channel FP32 image Value.
 * @param width Positive image width.
 * @param height Positive image height.
 * @param samples Exact row-major sample payload.
 * @return Fresh immutable zero-origin ordinary image Value.
 * @throws std::invalid_argument for inconsistent shape or payload.
 * @throws std::overflow_error when Value envelope arithmetic overflows.
 * @throws std::bad_alloc when metadata or payload allocation fails.
 * @note Samples retain their native FP32 domain without scaling.
 */
Value make_float_image_value(std::size_t width, std::size_t height,
                             const std::vector<float>& samples) {
  if (width == 0U || height == 0U || samples.size() != width * height) {
    throw std::invalid_argument("OpenCV concurrency image fixture is invalid");
  }
  std::vector<std::byte> storage(samples.size() * sizeof(float));
  std::memcpy(storage.data(), samples.data(), storage.size());
  DenseTensorDescriptor descriptor{{height, width, 1U},
                                   ElementSemantics::FloatingPoint,
                                   StorageEncoding{32U}};
  ImageFacet facet = make_zero_origin_image_facet(descriptor, 1U, 0U, 2U);
  return Value::from_cpu_dense_tensor(
      std::move(descriptor), std::move(facet),
      StridedLayout{{static_cast<std::ptrdiff_t>(width * sizeof(float)),
                     static_cast<std::ptrdiff_t>(sizeof(float)),
                     static_cast<std::ptrdiff_t>(sizeof(float))}},
      std::move(storage));
}

/**
 * @brief Computes one deterministic curve image with an exact Run cap.
 * @param host Seeded public Host whose process execution pool is already fixed.
 * @param root Temporary root owning source, session, and cache paths.
 * @param yaml_path Deterministic Graph source path.
 * @param maximum_parallelism Exact positive Run callback cap.
 * @param session Unique graph session label.
 * @return Immutable public image Value retained after graph close.
 * @throws std::bad_alloc if request, Host, or image storage exhausts memory.
 * @throws std::runtime_error if load, compute, or close fails.
 * @note Compute uses the real public Host and tiled built-in callback path with
 *       cache reads, disk writes, and output saves disabled. The cap changes
 *       Run QoS without changing the process pool.
 */
Value compute_curve_value(Host& host, const std::filesystem::path& root,
                          const std::filesystem::path& yaml_path,
                          unsigned int maximum_parallelism,
                          const std::string& session) {
  GraphLoadRequest load;
  load.session = GraphSessionId{session};
  load.root_dir = (root / "sessions").string();
  load.yaml_path = yaml_path.string();
  load.cache_root_dir = (root / "cache").string();
  const Result<GraphSessionId> loaded = host.load_graph(load);
  if (!loaded.status.ok) {
    throw std::runtime_error("failed to load deterministic curve Graph: " +
                             loaded.status.message);
  }

  HostComputeRequest request;
  request.session = load.session;
  request.node = NodeId{1};
  request.cache.precision = "fp32";
  request.cache.force_recache = true;
  request.cache.disable_disk_cache = true;
  request.cache.nosave = true;
  request.execution.parallel = true;
  request.execution.maximum_parallelism = maximum_parallelism;
  request.intent = ComputeIntent::GlobalHighPrecision;
  const Result<NamedValueResult> computed =
      host.compute_and_get_values(request);
  const VoidResult closed = host.close_graph(load.session);
  if (!computed.status.ok) {
    throw std::runtime_error("failed to compute deterministic curve Graph: " +
                             computed.status.message);
  }
  if (!closed.status.ok) {
    throw std::runtime_error("failed to close deterministic curve Graph: " +
                             closed.status.message);
  }
  const Value* image =
      computed.value.find(std::string(NodeOutput::kImageOutputName));
  if (image == nullptr) {
    throw std::runtime_error(
        "deterministic curve Graph omitted its named image Value");
  }
  return *image;
}

/**
 * @brief Proves automatic benchmark threads become one Run cap.
 *
 * @throws Nothing when the benchmark publishes its resolved nonzero cap;
 *         GoogleTest records mismatches and setup exceptions fail the test.
 * @note Process preparation uses the zero idempotent worker request, while the
 *       compute request and benchmark result share one positive cap resolved
 *       at the benchmark boundary.
 */
TEST(OpenCvOperationConcurrency,
     BenchmarkAutoThreadsPublishRunCapAndPreserveFixedPool) {
  ScopedBenchmarkTempDir temp("photospider_benchmark_auto_worker_grant");
  const std::filesystem::path yaml_path = temp.root() / "probe.yaml";
  write_benchmark_probe_graph(yaml_path);

  testing::HostSpy host;
  BenchmarkService service(host);
  const BenchmarkSessionConfig config =
      make_probe_benchmark_config(yaml_path, 0);

  const BenchmarkResult result = service.Run(temp.root().string(), config, 1);
  const std::vector<testing::HostInvocation> invocations = host.invocations();
  const auto configured = std::find_if(
      invocations.begin(), invocations.end(), [](const auto& call) {
        return call.method == "execution.configure_defaults";
      });
  const auto loaded = std::find_if(
      invocations.begin(), invocations.end(),
      [](const auto& call) { return call.method == "graph.load"; });
  const auto computed = std::find_if(
      invocations.begin(), invocations.end(),
      [](const auto& call) { return call.method == "compute.submit"; });

  ASSERT_NE(configured, invocations.end());
  ASSERT_NE(loaded, invocations.end());
  ASSERT_NE(computed, invocations.end());
  EXPECT_LT(configured, loaded);
  EXPECT_EQ(std::count_if(invocations.begin(), invocations.end(),
                          [](const auto& call) {
                            return call.method ==
                                   "execution.configure_defaults";
                          }),
            1);
  EXPECT_EQ(configured->text, "cpu\ncpu");
  EXPECT_EQ(configured->worker_count, 0U);
  ASSERT_TRUE(computed->compute_request.has_value());
  ASSERT_TRUE(
      computed->compute_request->execution.maximum_parallelism.has_value());
  EXPECT_EQ(*computed->compute_request->execution.maximum_parallelism,
            static_cast<unsigned int>(result.num_threads));
  EXPECT_GT(result.num_threads, 0);
  EXPECT_LE(result.num_threads, 8);
}

/**
 * @brief Proves mixed benchmark sessions share one process preparation.
 *
 * @throws Nothing when valid sessions publish their own caps, invalid enabled
 *         input is diagnosed/skipped, and a disabled out-of-range numeric cap
 *         is not preflighted.
 * @note Configuration parsing still validates YAML structure and field types.
 *       The Host spy makes process preparation count, per-Run QoS, result
 *       order, and range-validation scope deterministic without backend
 *       timing.
 */
TEST(OpenCvOperationConcurrency,
     BenchmarkRunAllSharesPoolAndPreservesMixedSessionCaps) {
  ScopedBenchmarkTempDir temp("photospider_benchmark_mixed_run_caps");
  const std::filesystem::path yaml_path = temp.root() / "probe.yaml";
  write_benchmark_probe_graph(yaml_path);
  write_mixed_benchmark_config(temp.root() / "benchmark_config.yaml");

  testing::HostSpy host;
  BenchmarkService service(host);
  ::testing::internal::CaptureStderr();
  const std::vector<BenchmarkResult> results =
      service.RunAll(temp.root().string());
  const std::string diagnostics = ::testing::internal::GetCapturedStderr();
  const std::vector<testing::HostInvocation> invocations = host.invocations();

  ASSERT_EQ(results.size(), 3U);
  EXPECT_EQ(results[0].benchmark_name, "one_thread");
  EXPECT_EQ(results[0].num_threads, 1);
  EXPECT_EQ(results[1].benchmark_name, "two_threads");
  EXPECT_EQ(results[1].num_threads, 2);
  EXPECT_EQ(results[2].benchmark_name, "automatic_threads");
  EXPECT_EQ(results[2].num_threads,
            static_cast<int>(expected_benchmark_workers(0)));
  EXPECT_NE(diagnostics.find("Error running benchmark 'enabled_invalid': "
                             "benchmark execution.threads must be between "
                             "zero and eight"),
            std::string::npos);
  EXPECT_EQ(diagnostics.find("disabled_invalid"), std::string::npos);

  EXPECT_EQ(host.call_count("execution.configure_defaults"), 1U);
  std::vector<unsigned int> caps;
  for (const auto& invocation : invocations) {
    if (invocation.method != "compute.submit") {
      continue;
    }
    ASSERT_TRUE(invocation.compute_request.has_value());
    ASSERT_TRUE(
        invocation.compute_request->execution.maximum_parallelism.has_value());
    caps.push_back(*invocation.compute_request->execution.maximum_parallelism);
  }
  EXPECT_EQ(
      caps,
      (std::vector<unsigned int>{
          1U, 2U, static_cast<unsigned int>(expected_benchmark_workers(0))}));
}

/**
 * @brief Proves failed process preparation is global, retryable, and early.
 *
 * @throws Nothing when the exact Host diagnostic escapes before graph load
 *         and a later call retries the once-only initialization successfully.
 * @note `std::call_once` must not latch an exception as successful
 *       preparation; the session QoS cap remains unchanged across the retry.
 */
TEST(OpenCvOperationConcurrency,
     BenchmarkProcessPreparationFailureRetainsDiagnosticAndCanRetry) {
  ScopedBenchmarkTempDir temp("photospider_benchmark_prepare_failure");
  const std::filesystem::path yaml_path = temp.root() / "probe.yaml";
  write_benchmark_probe_graph(yaml_path);

  testing::HostSpy host;
  OperationStatus rejected;
  rejected.ok = false;
  rejected.message = "benchmark process setup rejected";
  host.set_status("execution.configure_defaults", rejected);
  BenchmarkService service(host);
  const BenchmarkSessionConfig config =
      make_probe_benchmark_config(yaml_path, 2);

  try {
    (void)service.Run(temp.root().string(), config, 1);
    FAIL() << "expected process execution preparation to fail";
  } catch (const std::runtime_error& error) {
    EXPECT_STREQ(error.what(), "benchmark process setup rejected");
  }
  EXPECT_EQ(host.call_count("execution.configure_defaults"), 1U);
  EXPECT_EQ(host.call_count("graph.load"), 0U);

  host.set_status("execution.configure_defaults", OperationStatus{});
  const BenchmarkResult result = service.Run(temp.root().string(), config, 1);
  EXPECT_EQ(result.num_threads, 2);
  EXPECT_EQ(host.call_count("execution.configure_defaults"), 2U);
  EXPECT_EQ(host.call_count("graph.load"), 1U);
}

/**
 * @brief Proves benchmark thread input caps real execution callbacks.
 *
 * @throws Nothing when product behavior satisfies the contract; GoogleTest
 *         records mismatches and C++ setup exceptions fail the test.
 * @note One Host pool is fixed at eight before the service starts. Sequential
 *       benchmark Runs then vary only Run QoS; each cap is reached exactly
 *       while cap-plus-one remains bounded, without elapsed-time assertions.
 *       Callback identities may roam across up to eight fixed lanes over the
 *       full Run because the cap limits simultaneous work, not lane selection.
 */
TEST(OpenCvOperationConcurrency,
     BenchmarkThreadsCapCallbacksOnOneFixedExecutionPool) {
  ensure_benchmark_probe_registered();
  ScopedBenchmarkTempDir temp("photospider_benchmark_worker_control");
  const std::filesystem::path yaml_path = temp.root() / "probe.yaml";
  write_benchmark_probe_graph(yaml_path);

  CallbackConcurrencyGate gate;
  ScopedCallbackGatePublication publication(gate);
  std::unique_ptr<Host> host = create_embedded_host();
  ASSERT_NE(host, nullptr);
  HostExecutionConfig execution_config;
  execution_config.worker_count = 8U;
  const VoidResult configured =
      host->configure_execution_defaults(execution_config);
  ASSERT_TRUE(configured.status.ok) << configured.status.message;
  BenchmarkService service(*host);
  for (const int configured_threads : {0, 1, 2, 4, 8}) {
    const std::size_t expected_workers =
        expected_benchmark_workers(configured_threads);
    const BenchmarkSessionConfig config =
        make_probe_benchmark_config(yaml_path, configured_threads);

    std::future<BenchmarkResult> run;
    ScopedCallbackRelease release(gate);
    run = std::async(std::launch::async, [&service, &temp, &config] {
      return service.Run(temp.root().string(), config, 1);
    });

    EXPECT_TRUE(
        gate.wait_for_active(expected_workers, std::chrono::seconds(10)))
        << "configured_threads=" << configured_threads;
    EXPECT_FALSE(gate.wait_for_active(expected_workers + 1U,
                                      std::chrono::milliseconds(250)))
        << "configured_threads=" << configured_threads;
    release.release();

    ASSERT_EQ(run.wait_for(std::chrono::seconds(20)), std::future_status::ready)
        << "configured_threads=" << configured_threads;
    const BenchmarkResult result = run.get();
    EXPECT_EQ(result.num_threads, static_cast<int>(expected_workers));
    EXPECT_EQ(gate.max_active_callbacks(), expected_workers);
    EXPECT_GE(gate.unique_callback_threads(), expected_workers);
    EXPECT_LE(gate.unique_callback_threads(), 8U);
  }
}

/**
 * @brief Proves benchmark worker inputs reject signed and oversized values.
 *
 * @throws Nothing when validation precedes graph load; GoogleTest records any
 *         mismatch.
 * @note Both cases reuse a valid Graph so only worker-domain validation can
 *       explain the exception and the absence of a published session.
 */
TEST(OpenCvOperationConcurrency,
     BenchmarkThreadsRejectOutOfDomainValuesBeforeGraphLoad) {
  ensure_benchmark_probe_registered();
  ScopedBenchmarkTempDir temp("photospider_benchmark_worker_rejection");
  const std::filesystem::path yaml_path = temp.root() / "probe.yaml";
  write_benchmark_probe_graph(yaml_path);

  std::unique_ptr<Host> host = create_embedded_host();
  ASSERT_NE(host, nullptr);
  BenchmarkService service(*host);

  BenchmarkSessionConfig negative = make_probe_benchmark_config(yaml_path, -1);
  EXPECT_THROW((void)service.Run(temp.root().string(), negative, 1),
               std::invalid_argument);
  BenchmarkSessionConfig oversized = make_probe_benchmark_config(yaml_path, 9);
  EXPECT_THROW((void)service.Run(temp.root().string(), oversized, 1),
               std::invalid_argument);

  const Result<std::vector<GraphSessionId>> sessions = host->list_graphs();
  ASSERT_TRUE(sessions.status.ok) << sessions.status.message;
  EXPECT_TRUE(sessions.value.empty());
}

/**
 * @brief Proves zero is not a valid public Run maximum-parallelism value.
 *
 * @throws Nothing when the embedded Host rejects the request before callback
 *         execution and leaves the loaded Graph closable.
 * @note Absence, not zero, represents an uncapped Run. This keeps public Host
 *       and Host validation aligned with `ComputeRunQos`.
 */
TEST(OpenCvOperationConcurrency,
     HostComputeSurfacesRejectZeroMaximumParallelismAsInvalidParameter) {
  ensure_benchmark_probe_registered();
  ScopedBenchmarkTempDir temp("photospider_zero_run_parallelism");
  const std::filesystem::path yaml_path = temp.root() / "probe.yaml";
  write_benchmark_probe_graph(yaml_path);

  std::unique_ptr<Host> host = create_embedded_host();
  ASSERT_NE(host, nullptr);
  GraphLoadRequest load;
  load.session = GraphSessionId{"zero_run_parallelism"};
  load.root_dir = (temp.root() / "sessions").string();
  load.yaml_path = yaml_path.string();
  const Result<GraphSessionId> loaded = host->load_graph(load);
  ASSERT_TRUE(loaded.status.ok) << loaded.status.message;

  HostComputeRequest request;
  request.session = load.session;
  request.node = NodeId{kBenchmarkProbeSinkId};
  request.execution.parallel = true;
  request.execution.maximum_parallelism = 0U;
  const VoidResult computed = host->compute(request);
  EXPECT_FALSE(computed.status.ok);
  EXPECT_EQ(computed.status.domain, OperationErrorDomain::Graph);
  EXPECT_EQ(computed.status.code,
            static_cast<std::int32_t>(GraphErrc::InvalidParameter));
  EXPECT_EQ(computed.status.name, "invalid_parameter");

  const Result<std::future<OperationStatus>> asynchronous =
      host->compute_async(request);
  EXPECT_FALSE(asynchronous.status.ok);
  EXPECT_EQ(asynchronous.status.domain, OperationErrorDomain::Graph);
  EXPECT_EQ(asynchronous.status.code,
            static_cast<std::int32_t>(GraphErrc::InvalidParameter));
  EXPECT_EQ(asynchronous.status.name, "invalid_parameter");

  const Result<NamedValueResult> values = host->compute_and_get_values(request);
  EXPECT_FALSE(values.status.ok);
  EXPECT_EQ(values.status.domain, OperationErrorDomain::Graph);
  EXPECT_EQ(values.status.code,
            static_cast<std::int32_t>(GraphErrc::InvalidParameter));
  EXPECT_EQ(values.status.name, "invalid_parameter");

  const VoidResult closed = host->close_graph(load.session);
  EXPECT_TRUE(closed.status.ok) << closed.status.message;
}

/**
 * @brief Proves the real curve provider restores the complete calling-thread
 * floating-point environment.
 *
 * @throws Provider, registry, Host output planning/grant, Value view,
 * allocation, or floating-point setup failures unchanged to GoogleTest.
 * @note The callback runs synchronously on this thread after a non-default
 * rounding mode and two sticky exceptions are installed. The test snapshots
 * rounding and every `FE_ALL_EXCEPT` flag immediately after provider return,
 * then retires the sole Host grant and inspects the sealed Value. The
 * `ScopedTestFloatingPointEnvironment` restores the thread's original
 * environment on every later exit.
 */
TEST(OpenCvOperationConcurrency,
     BuiltinCurveRestoresCompleteCallingThreadEnvironment) {
  ScopedTestFloatingPointEnvironment restore_initial_environment;
  std::unique_ptr<Host> host = create_embedded_host();
  ASSERT_NE(host, nullptr);
  const VoidResult seeded = host->seed_builtin_ops();
  ASSERT_TRUE(seeded.status.ok) << seeded.status.message;

  const auto resolved = OpRegistry::instance().resolve_for_intent(
      "image_process", "curve_transform", ComputeIntent::GlobalHighPrecision);
  ASSERT_TRUE(resolved.has_value());
  ASSERT_TRUE(std::holds_alternative<TileOpFunc>(*resolved));
  const TileOpFunc curve_callback = std::get<TileOpFunc>(*resolved);

  Node node;
  node.id = 1;
  node.type = "image_process";
  node.subtype = "curve_transform";
  node.runtime_parameters["k"] = 1.75;

  const Value input_value = make_float_image_value(2U, 1U, {0.25F, 0.5F});
  ASSERT_TRUE(input_value.image_facet().has_value());
  HostOutputBinding output_binding =
      HostOutputBinding::allocate(DenseImageOutputPlan::create(
          std::string(NodeOutput::kImageOutputName),
          input_value.dense_tensor_descriptor(), *input_value.image_facet(),
          input_value.strided_layout(), input_value.storage_size(), 64U));
  const PixelRect tile_roi{0, 0, 2, 1};
  HostOutputWriteGrant output_grant =
      output_binding.grant_tile({image_region_domain(), 0, 2, 0, 1});
  const OutputTile output_tile{&output_binding.plan(), &output_grant, tile_roi};
  const std::vector<InputTile> input_tiles{{&input_value, tile_roi, nullptr}};

  ASSERT_EQ(fesetenv(FE_DFL_ENV), 0);
  ASSERT_EQ(fesetround(FE_DOWNWARD), 0);
  ASSERT_EQ(feclearexcept(FE_ALL_EXCEPT), 0);
  constexpr int kPresetExceptions = FE_INVALID | FE_DIVBYZERO;
  ASSERT_EQ(feraiseexcept(kPresetExceptions), 0);
  ASSERT_EQ(fegetround(), FE_DOWNWARD);
  ASSERT_EQ(fetestexcept(FE_ALL_EXCEPT), kPresetExceptions);

  curve_callback(node, output_tile, input_tiles);
  const int restored_rounding = fegetround();
  const int restored_exceptions = fetestexcept(FE_ALL_EXCEPT);

  EXPECT_EQ(restored_rounding, FE_DOWNWARD);
  EXPECT_EQ(restored_exceptions, kPresetExceptions);
  output_grant.retire_success();
  const ImageView output_view(output_binding.seal());
  float first_output = 0.0F;
  float second_output = 0.0F;
  std::memcpy(&first_output, output_view.channel_data(0U, 0U, 0U),
              sizeof(first_output));
  std::memcpy(&second_output, output_view.channel_data(1U, 0U, 0U),
              sizeof(second_output));
  EXPECT_GT(first_output, 0.0F);
  EXPECT_GT(second_output, 0.0F);
}

/**
 * @brief Proves tiled built-in callbacks reach the requested worker overlap.
 *
 * @throws Nothing when product behavior satisfies the reentrant provider
 *         contract; GoogleTest records any mismatch.
 * @note The observer blocks inside the built-in callback body so every Run cap
 *       is reached exactly; elapsed operation performance is not part of the
 *       verdict. One eight-worker pool remains fixed across every
 *       cap/repetition pair, and sequential callbacks may use different lanes
 *       while never exceeding the active cap.
 */
TEST(OpenCvOperationConcurrency,
     BuiltinCurveCallbacksReachRequestedWorkerConcurrency) {
  ScopedBenchmarkTempDir temp("photospider_opencv_curve_concurrency");
  CallbackConcurrencyGate gate;
  CurveOperationObserver observer(gate);
  ScopedOpenCvObserverPublication publication(observer);
  std::unique_ptr<Host> host = create_embedded_host();
  ASSERT_NE(host, nullptr);
  const VoidResult seeded = host->seed_builtin_ops();
  ASSERT_TRUE(seeded.status.ok) << seeded.status.message;
  HostExecutionConfig execution_config;
  execution_config.worker_count = 8U;
  const VoidResult configured =
      host->configure_execution_defaults(execution_config);
  ASSERT_TRUE(configured.status.ok) << configured.status.message;
  BenchmarkService service(*host);
  for (int repetition = 0; repetition < 3; ++repetition) {
    for (const int worker_count : {1, 2, 4, 8}) {
      const BenchmarkSessionConfig config =
          make_curve_benchmark_config(worker_count);
      const std::size_t expected_workers =
          static_cast<std::size_t>(worker_count);

      std::future<BenchmarkResult> run;
      ScopedCallbackRelease release(gate);
      run = std::async(std::launch::async, [&service, &temp, &config] {
        return service.Run(temp.root().string(), config, 1);
      });

      EXPECT_TRUE(
          gate.wait_for_active(expected_workers, std::chrono::seconds(10)))
          << "repetition=" << repetition << ", workers=" << worker_count;
      EXPECT_FALSE(gate.wait_for_active(expected_workers + 1U,
                                        std::chrono::milliseconds(250)))
          << "repetition=" << repetition << ", workers=" << worker_count;
      release.release();

      ASSERT_EQ(run.wait_for(std::chrono::seconds(20)),
                std::future_status::ready)
          << "repetition=" << repetition << ", workers=" << worker_count;
      const BenchmarkResult result = run.get();
      EXPECT_EQ(result.num_threads, worker_count);
      EXPECT_EQ(gate.max_active_callbacks(), expected_workers);
      EXPECT_GE(gate.unique_callback_threads(), expected_workers);
      EXPECT_LE(gate.unique_callback_threads(), 8U);
    }
  }
}

/**
 * @brief Proves one- and eight-cap tiled curve execution is bitwise equal.
 *
 * @throws Nothing when deterministic output is preserved; setup exceptions
 *         fail the test and GoogleTest records descriptor or pixel mismatches.
 * @note Comparison checks every logical channel element retained by the
 *       public Host Values. Both Runs share one
 *       fixed eight-lane pool and vary only public Run QoS.
 */
TEST(OpenCvOperationConcurrency,
     BuiltinCurveOutputMatchesBetweenOneAndEightRunCaps) {
  ScopedBenchmarkTempDir temp("photospider_opencv_curve_output");
  const std::filesystem::path yaml_path = temp.root() / "curve.yaml";
  write_curve_output_graph(yaml_path);

  std::unique_ptr<Host> host = create_embedded_host();
  ASSERT_NE(host, nullptr);
  const VoidResult seeded = host->seed_builtin_ops();
  ASSERT_TRUE(seeded.status.ok) << seeded.status.message;
  HostExecutionConfig execution_config;
  execution_config.worker_count = 8U;
  const VoidResult configured =
      host->configure_execution_defaults(execution_config);
  ASSERT_TRUE(configured.status.ok) << configured.status.message;

  const ImageView serial(compute_curve_value(*host, temp.root(), yaml_path, 1U,
                                             "curve_output_serial"));
  const ImageView parallel(compute_curve_value(*host, temp.root(), yaml_path,
                                               8U, "curve_output_parallel"));

  ASSERT_EQ(serial.width(), parallel.width());
  ASSERT_EQ(serial.height(), parallel.height());
  ASSERT_EQ(serial.channels(), parallel.channels());
  ASSERT_EQ(serial.descriptor().element_semantics,
            parallel.descriptor().element_semantics);
  ASSERT_EQ(serial.descriptor().storage_encoding.bit_width,
            parallel.descriptor().storage_encoding.bit_width);
  ASSERT_EQ(serial.element_bytes(), parallel.element_bytes());
  for (std::size_t row = 0U; row < serial.height(); ++row) {
    for (std::size_t column = 0U; column < serial.width(); ++column) {
      for (std::size_t channel = 0U; channel < serial.channels(); ++channel) {
        EXPECT_EQ(std::memcmp(serial.channel_data(column, row, channel),
                              parallel.channel_data(column, row, channel),
                              serial.element_bytes()),
                  0)
            << "row=" << row << ", column=" << column
            << ", channel=" << channel;
      }
    }
  }
}

}  // namespace
}  // namespace ps
