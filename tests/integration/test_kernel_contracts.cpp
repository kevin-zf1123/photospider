#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <system_error>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

#include "adapters/opencv/buffer_adapter_opencv.hpp"
#include "compute/compute_service.hpp"
#include "core/param_utils.hpp"
#include "core/ps_types.hpp"  // NOLINT(build/include_subdir)
#include "graph/graph_cache_service.hpp"
#include "graph/graph_io_service.hpp"
#if defined(PHOTOSPIDER_INTERNAL_YAML_GRAPH_DOCUMENT_ADAPTER_TESTING)
#include "adapters/yaml/yaml_graph_document_adapter_test_access.hpp"
#endif
#include "graph/graph_model.hpp"  // NOLINT(build/include_subdir)
#include "graph/graph_state_executor_test_access.hpp"
#include "graph/graph_traversal_service.hpp"
#include "plugin/operation_host_adapter.hpp"
#include "providers/configured_image_artifact_codec.hpp"
#include "runtime/graph_event_service.hpp"
#include "runtime/interaction.hpp"
#include "runtime/kernel.hpp"
#include "support/fake_image_artifact_codec.hpp"
#include "support/kernel_test_access.hpp"
#include "support/kernel_test_dependencies.hpp"

namespace ps {
namespace {

static_assert(std::is_same_v<decltype(Node::parameters), plugin::ParameterMap>);
static_assert(
    std::is_same_v<decltype(Node::runtime_parameters), plugin::ParameterMap>);
static_assert(std::is_same_v<decltype(NodeOutput::data), plugin::ParameterMap>);

/** @brief Serializes the blocking contract operation's release future. */
std::mutex g_blocking_source_mutex;

/** @brief Test-controlled release copied by the blocking contract operation. */
std::shared_future<void> g_blocking_source_release;

/** @brief Publishes entry into the blocking contract operation callback. */
std::atomic<bool> g_blocking_source_started{false};

/** @brief Counts document-to-operation parameter producer invocations. */
std::atomic<int> g_parameter_value_source_calls{0};

/** @brief Counts effective-parameter consumer invocations. */
std::atomic<int> g_parameter_value_consumer_calls{0};

/**
 * @brief Configures the blocking contract op release signal for one test.
 *
 * @param release Future that the blocking source op waits on after signalling
 * that execution has started.
 * @throws std::bad_alloc if shared_future state copying allocates.
 * @note The op reads this state under g_blocking_source_mutex so tests can
 * safely install a fresh release future before submitting compute work.
 */
void configure_blocking_contract_source(std::shared_future<void> release) {
  std::lock_guard<std::mutex> lock(g_blocking_source_mutex);
  g_blocking_source_started.store(false, std::memory_order_release);
  g_blocking_source_release = std::move(release);
}

/**
 * @brief Clears the blocking contract op release signal after a test.
 *
 * @throws Nothing directly.
 * @note Leaving the future unset would make later blocking-source computes
 * wait on stale test state.
 */
void reset_blocking_contract_source() {
  std::lock_guard<std::mutex> lock(g_blocking_source_mutex);
  g_blocking_source_release = std::shared_future<void>();
  g_blocking_source_started.store(false, std::memory_order_release);
}

/**
 * @brief Waits until the blocking contract op reports that it is running.
 *
 * @param timeout Maximum time to wait for the op to start.
 * @return true when the op started before timeout, otherwise false.
 * @throws Nothing directly.
 * @note Tests use this to know the graph-state compute closure has entered
 * scheduler-backed work and is holding the graph-state executor boundary.
 */
bool wait_for_blocking_contract_source(std::chrono::milliseconds timeout) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    if (g_blocking_source_started.load(std::memory_order_acquire)) {
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  return g_blocking_source_started.load(std::memory_order_acquire);
}

/**
 * @brief Registers deterministic operations used by Kernel contract tests.
 *
 * @return Nothing.
 * @throws std::bad_alloc if registry keys, callbacks, or metadata cannot be
 * allocated during the one-time registration.
 * @note Registration is process-wide and idempotent through std::call_once.
 * The blocking operation borrows only the separately synchronized test future.
 */
void register_contract_ops() {
  static std::once_flag once;
  std::call_once(once, [] {
    auto& registry = OpRegistry::instance();

    registry.register_op_hp_monolithic(
        "kernel_contract_test", "source",
        MonolithicOpFunc(
            [](const Node& node, const std::vector<const NodeOutput*>&) {
              const int width =
                  as_int_flexible(node.runtime_parameters, "width", 17);
              const int height =
                  as_int_flexible(node.runtime_parameters, "height", 3);
              NodeOutput output;
              output.image_buffer = make_aligned_cpu_image_buffer(
                  width, height, 1, DataType::FLOAT32);
              cv::Mat mat = toCvMat(output.image_buffer);
              mat.setTo(1.0f);
              return output;
            }));

    registry.register_op_hp_monolithic(
        "kernel_contract_test", "process",
        MonolithicOpFunc(
            [](const Node&, const std::vector<const NodeOutput*>& inputs) {
              if (inputs.empty()) {
                throw GraphError(GraphErrc::MissingDependency,
                                 "process requires an input");
              }
              NodeOutput output;
              const auto& input = inputs.front()->image_buffer;
              output.image_buffer = make_aligned_cpu_image_buffer(
                  input.width, input.height, input.channels, input.type);
              cv::Mat src = toCvMat(input);
              cv::Mat dst = toCvMat(output.image_buffer);
              src.copyTo(dst);
              return output;
            }));

    registry.register_op_hp_monolithic(
        "kernel_contract_test", "blocking_source",
        MonolithicOpFunc(
            [](const Node& node, const std::vector<const NodeOutput*>&) {
              std::shared_future<void> release;
              {
                std::lock_guard<std::mutex> lock(g_blocking_source_mutex);
                release = g_blocking_source_release;
              }
              g_blocking_source_started.store(true, std::memory_order_release);
              if (release.valid()) {
                release.wait();
              }

              const int width =
                  as_int_flexible(node.runtime_parameters, "width", 17);
              const int height =
                  as_int_flexible(node.runtime_parameters, "height", 3);
              NodeOutput output;
              output.image_buffer = make_aligned_cpu_image_buffer(
                  width, height, 1, DataType::FLOAT32);
              cv::Mat mat = toCvMat(output.image_buffer);
              mat.setTo(3.0f);
              return output;
            }));

    registry.register_op_rt_tiled(
        "kernel_contract_test", "process",
        TileOpFunc([](const Node&, const OutputTile& output_tile,
                      const std::vector<InputTile>& input_tiles) {
          if (input_tiles.empty()) {
            throw GraphError(GraphErrc::MissingDependency,
                             "process requires input tiles");
          }
          cv::Mat src = toCvMat(input_tiles.front());
          cv::Mat dst = toCvMat(output_tile);
          src.copyTo(dst);
        }),
        OpMetadata{});

    registry.register_op_hp_monolithic(
        "kernel_contract_test", "parameter_value_source",
        plugin_host::adapt_monolithic_operation(plugin::MonolithicOperation(
            [](const plugin::NodeView& node,
               plugin::ArrayView<plugin::OperationInputView>) {
              g_parameter_value_source_calls.fetch_add(
                  1, std::memory_order_relaxed);
              const plugin::ParameterValue* enabled =
                  node.find_parameter("enabled");
              const plugin::ParameterValue* count =
                  node.find_parameter("count");
              const plugin::ParameterValue* ratio =
                  node.find_parameter("ratio");
              const plugin::ParameterValue* label =
                  node.find_parameter("label");
              if (enabled == nullptr || count == nullptr || ratio == nullptr ||
                  label == nullptr ||
                  node.find_parameter("optional_value") != nullptr) {
                throw GraphError(GraphErrc::InvalidParameter,
                                 "parameter source contract is incomplete");
              }
              if (!enabled->as_bool() || ratio->as_double() != 1.25 ||
                  label->as_string() != "007") {
                throw GraphError(GraphErrc::InvalidParameter,
                                 "parameter source contract values differ");
              }
              plugin::OperationOutput output;
              output.data["dynamic_count"] = count->as_int64() + 4;
              return output;
            })));

    registry.register_op_hp_monolithic(
        "kernel_contract_test", "parameter_value_consumer",
        plugin_host::adapt_monolithic_operation(plugin::MonolithicOperation(
            [](const plugin::NodeView& node,
               plugin::ArrayView<plugin::OperationInputView>) {
              g_parameter_value_consumer_calls.fetch_add(
                  1, std::memory_order_relaxed);
              const plugin::ParameterValue* enabled =
                  node.find_parameter("enabled");
              const plugin::ParameterValue* count =
                  node.find_parameter("count");
              const plugin::ParameterValue* ratio =
                  node.find_parameter("ratio");
              const plugin::ParameterValue* label =
                  node.find_parameter("label");
              if (enabled == nullptr || count == nullptr || ratio == nullptr ||
                  label == nullptr ||
                  node.find_parameter("optional_value") != nullptr) {
                throw GraphError(GraphErrc::InvalidParameter,
                                 "parameter consumer contract is incomplete");
              }
              if (enabled->as_bool() || count->as_int64() != 11 ||
                  ratio->as_double() != 2.5 ||
                  label->as_string() != "consumer") {
                throw GraphError(GraphErrc::InvalidParameter,
                                 "effective parameter contract values differ");
              }
              constexpr int kDefaultHeight = 3;
              plugin::OperationOutput output;
              output.image_buffer = make_aligned_cpu_image_buffer(
                  static_cast<int>(count->as_int64()), kDefaultHeight, 1,
                  DataType::FLOAT32);
              cv::Mat image = toCvMat(output.image_buffer);
              image.setTo(2.0f);
              return output;
            })));
  });
}

Node make_contract_node() {
  Node node;
  node.id = 1;
  node.name = "contract_source";
  node.type = "kernel_contract_test";
  node.subtype = "source";
  node.parameters["width"] = 17;
  node.parameters["height"] = 3;
  return node;
}

Node make_contract_process_node() {
  Node node;
  node.id = 2;
  node.name = "contract_process";
  node.type = "kernel_contract_test";
  node.subtype = "process";
  node.image_inputs.push_back(ImageInput{1, "image"});
  return node;
}

std::filesystem::path temp_path(const std::string& name) {
  return std::filesystem::temp_directory_path() / name;
}

/**
 * @brief Removes and returns a deterministic temporary cache root.
 *
 * @param name Directory name appended to the system temporary directory.
 * @return Clean root path ready for GraphModel construction.
 * @throws std::filesystem::filesystem_error if cleanup fails.
 * @note Tests use deterministic names so failed runs leave inspectable paths.
 */
std::filesystem::path clean_temp_path(const std::string& name) {
  auto root = temp_path(name);
  std::filesystem::remove_all(root);
  return root;
}

void write_text(const std::filesystem::path& path, const std::string& text) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream out(path);
  out << text;
}

/**
 * @brief Writes a single-node graph whose operation intentionally is missing.
 *
 * @param path YAML file path to create.
 * @throws std::filesystem::filesystem_error or std::ios_base::failure from
 * directory creation or file writing.
 * @note The graph is valid topology, so compute reaches operation resolution
 * and fails inside the compute request boundary.
 */
void write_missing_op_graph(const std::filesystem::path& path) {
  write_text(path, R"YAML(
- id: 1
  name: missing_op
  type: kernel_contract_test
  subtype: missing_op
  parameters:
    width: 8
    height: 8
)YAML");
}

/**
 * @brief Writes a graph that runs the blocking contract source operation.
 *
 * @param path YAML file path to create.
 * @throws std::filesystem::filesystem_error or std::ios_base::failure from
 * directory creation or file writing.
 * @note The source has explicit dimensions so planned parallel dispatch emits
 * a deterministic single monolithic scheduler task.
 */
void write_blocking_source_graph(const std::filesystem::path& path) {
  write_text(path, R"YAML(
- id: 1
  name: blocking_source
  type: kernel_contract_test
  subtype: blocking_source
  parameters:
    width: 8
    height: 8
)YAML");
}

/**
 * @brief Builds a process node with one image disk-cache entry.
 *
 * @param location CacheEntry location to attach to the process node.
 * @return Node configured for disk-cache service tests.
 * @throws std::bad_alloc if node strings or vectors cannot allocate.
 * @note The node does not need to be added to GraphModel for cache load tests
 * because GraphCacheService resolves disk paths from node id and cache entry.
 */
Node make_cached_process_node(const std::string& location) {
  Node node = make_contract_process_node();
  node.caches.push_back({"image", location});
  return node;
}

/**
 * @brief Observes when a graph-state close begins waiting for worker drainage.
 *
 * @note The callback performs one atomic store only, satisfying the executor
 * hook's no-allocation, nonblocking, and no-reentry contract. Tests install the
 * observer only after all setup work for the target runtime has completed.
 */
struct CloseWaitingObserver {
  /** @brief True after the target executor publishes CloseCallerWaiting. */
  std::atomic<bool> observed{false};

  /**
   * @brief Records the close-waiting checkpoint from a test-enabled executor.
   * @param context Borrowed observer supplied by the installed hook.
   * @param snapshot Allocation-free executor lifecycle snapshot.
   * @return Nothing.
   * @throws Nothing.
   */
  static void notify(
      void* context,
      const testing::GraphStateExecutorTestSnapshot& snapshot) noexcept {
    if (snapshot.event ==
        testing::GraphStateExecutorTestEvent::CloseCallerWaiting) {
      static_cast<CloseWaitingObserver*>(context)->observed.store(
          true, std::memory_order_release);
    }
  }
};

/**
 * @brief Installs one executor observer and clears it at scope exit.
 *
 * @param observer Observer retained by the scope owner.
 * @throws Nothing.
 * @note Tests using this guard must not run another executor-hook test in
 * parallel. Destruction clears the process-local borrowed hook before observer
 * storage leaves scope.
 */
class ScopedGraphStateExecutorHook {
 public:
  /**
   * @brief Installs a borrowed close-waiting observer for this scope.
   * @param observer Observer that remains alive through this guard's lifetime.
   * @throws Nothing.
   * @note Installation replaces the process-local executor test hook, so
   * callers must serialize guards that use this seam.
   */
  explicit ScopedGraphStateExecutorHook(CloseWaitingObserver& observer)
      : hook_{&observer, &CloseWaitingObserver::notify} {
    testing::set_graph_state_executor_test_hook(&hook_);
  }

  /**
   * @brief Clears the installed borrowed hook before observer storage expires.
   * @throws Nothing.
   * @note Every affected executor callback must have completed before this
   * guard leaves scope.
   */
  ~ScopedGraphStateExecutorHook() noexcept {
    testing::set_graph_state_executor_test_hook(nullptr);
  }

  /**
   * @brief Disables copying of process-local hook ownership.
   * @param other Guard whose borrowed hook must remain uniquely scoped.
   * @throws Nothing because construction is unavailable.
   * @note A copied guard could clear another guard's installed hook
   * prematurely.
   */
  ScopedGraphStateExecutorHook(const ScopedGraphStateExecutorHook& other) =
      delete;

  /**
   * @brief Disables copy assignment of process-local hook ownership.
   * @param other Guard whose borrowed hook must remain uniquely scoped.
   * @return No value because assignment is unavailable.
   * @throws Nothing because assignment is unavailable.
   * @note Assignment could invalidate the process-local hook lifetime boundary.
   */
  ScopedGraphStateExecutorHook& operator=(
      const ScopedGraphStateExecutorHook& other) = delete;

 private:
  /** @brief Borrowed hook installed for this scope. */
  testing::GraphStateExecutorTestHook hook_;
};

/**
 * @brief Waits a bounded interval for an atomic lifecycle checkpoint.
 * @param value Atomic flag published by the observed worker or close path.
 * @param timeout Maximum interval to wait.
 * @return True when the flag becomes set before the deadline.
 * @throws Nothing directly.
 * @note The helper is test-only synchronization fallback; ordering assertions
 * use explicit futures and executor checkpoints rather than operation sleeps.
 */
bool wait_for_atomic_true(const std::atomic<bool>& value,
                          std::chrono::milliseconds timeout) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    if (value.load(std::memory_order_acquire)) {
      return true;
    }
    std::this_thread::yield();
  }
  return value.load(std::memory_order_acquire);
}

/**
 * @brief Test-only work item that destroys a caller-retained Kernel owner.
 *
 * @return Nothing.
 * @throws Any exception raised while publishing the worker checkpoint or
 * destroying Kernel.
 * @note A launcher may move this callable into worker storage, but the callable
 * borrows the scenario-owned `std::unique_ptr`; it never owns Kernel itself.
 */
using KernelDestructionTask = std::function<void()>;

/**
 * @brief Injectable launcher for one Kernel-destruction work item.
 *
 * @param task Borrowing work item whose invocation is transferred to the
 * returned future.
 * @return Future that joins the launched work item.
 * @throws Any launch failure selected by the implementation or test.
 * @note On an exceptional return, the launcher must not retain or later invoke
 * `task`. This matches `std::async(std::launch::async, ...)` launch failure and
 * permits deterministic pre-launch failure injection without production hooks.
 */
using KernelDestructionLauncher = std::function<std::future<void>(
    KernelDestructionTask task)>;  // NOLINT(whitespace/indent_namespace)

/**
 * @brief Captures lifecycle and cleanup checkpoints from one teardown scenario.
 *
 * @note Fields are written only by the calling test thread after
 * synchronization with the relevant future or atomic publication.
 */
struct KernelCodecTeardownEvidence {
  /** @brief True after the first real graph-state work item starts blocking. */
  bool blocker_entered = false;
  /** @brief True when cache work is admitted and still queued behind blocker.
   */
  bool cache_work_pending = false;
  /** @brief True immediately before the injected launcher is invoked. */
  bool launcher_invoked = false;
  /** @brief True only when the launcher returns an owning future normally. */
  bool launcher_returned = false;
  /** @brief True after the successful destruction worker starts. */
  bool destruction_entered = false;
  /** @brief True when production teardown reaches its close-wait checkpoint. */
  bool close_waiting = false;
  /** @brief True when successful Kernel destruction remains blocked on work. */
  bool destruction_pending = false;
  /** @brief True when the codec remains retained before blocker release. */
  bool codec_alive_before_release = false;
  /** @brief True when encode has not run before blocker release. */
  bool encode_pending_before_release = false;
  /** @brief True after the test releases the graph-state blocker exactly once.
   */
  bool blocker_released = false;
  /** @brief True when the blocker future became ready within the bounded wait.
   */
  bool blocker_future_recovered = false;
  /** @brief True when admitted cache work became ready within the bounded wait.
   */
  bool cache_future_recovered = false;
  /** @brief True when a returned destruction future was joined successfully. */
  bool destruction_future_recovered = false;
  /** @brief True after the scenario no longer owns a Kernel. */
  bool kernel_destroyed = false;
  /** @brief True after the admitted fake-codec encode callback completes. */
  bool encode_finished = false;
  /** @brief True when the encode callback observed its codec owner alive. */
  bool codec_alive_during_encode = false;
  /** @brief True after Kernel teardown releases the last codec owner. */
  bool codec_released = false;
  /** @brief True after both scenario-owned temporary paths are absent. */
  bool temporary_paths_removed = false;
};

/**
 * @brief Runs the real Kernel/cache teardown lifecycle with injectable launch.
 *
 * The scenario creates a real Kernel, GraphRuntime, GraphStateExecutor, and
 * GraphCacheService with a fake codec. It blocks one graph-state work item,
 * admits a cache-save behind it, and invokes a launcher whose task borrows the
 * caller-retained Kernel owner. A normal launcher may destroy Kernel on its
 * worker. If launch throws before returning a future, the caller still owns
 * Kernel; cleanup releases the blocker, recovers admitted futures, destroys
 * Kernel and codec, removes temporary paths, then rethrows the original error.
 *
 * @note One instance is single-use and not thread-safe. Worker callbacks borrow
 * members only while `run()` is active; every returned future is joined before
 * `run()` returns or propagates an exception.
 */
class KernelCodecTeardownScenario final {
 public:
  /**
   * @brief Creates one single-use scenario with isolated temporary names.
   * @param suffix Stable suffix distinguishing concurrent or repeated tests.
   * @throws std::bad_alloc if path, string, promise, or future state allocation
   * fails.
   * @note Construction performs no filesystem mutation and creates no Kernel.
   */
  explicit KernelCodecTeardownScenario(const std::string& suffix)
      : graph_name_("contract_kernel_codec_lifetime_" + suffix),
        root_(temp_path("photospider-contract-kernel-codec-lifetime-root-" +
                        suffix)),
        yaml_path_(temp_path("photospider-contract-kernel-codec-lifetime-" +
                             suffix + ".yaml")),
        blocker_release_(release_blocker_.get_future().share()),
        blocker_entered_future_(blocker_entered_.get_future()),
        destruction_entered_future_(destruction_entered_.get_future()) {}

  /**
   * @brief Removes temporary paths after a fully recovered scenario.
   * @throws Nothing.
   * @note `run()` owns ordered blocker/future/Kernel recovery. The destructor
   * performs filesystem fallback only; destroying a live Kernel here would be
   * unsafe if an earlier cleanup invariant were broken.
   */
  ~KernelCodecTeardownScenario() noexcept { (void)cleanup_temporary_paths(); }

  /**
   * @brief Disables copying of futures, promises, and the unique Kernel owner.
   * @param other Scenario whose single-use synchronization state cannot be
   * shared.
   * @throws Nothing because construction is unavailable.
   */
  KernelCodecTeardownScenario(const KernelCodecTeardownScenario& other) =
      delete;

  /**
   * @brief Disables assignment of single-use teardown state.
   * @param other Scenario whose ownership state cannot replace this instance.
   * @return No value because assignment is unavailable.
   * @throws Nothing because assignment is unavailable.
   */
  KernelCodecTeardownScenario& operator=(
      const KernelCodecTeardownScenario& other) = delete;

  /**
   * @brief Executes one success or launcher-failure teardown scenario.
   *
   * Setup prepares the graph and admitted work. The launcher receives a task
   * that borrows `kernel_`; ownership therefore remains recoverable until the
   * launcher returns a future. Success observes real close/drain ordering
   * before releasing work. Failure first releases and joins all admitted work,
   * then destroys owners and temporary paths before propagating the original
   * error.
   *
   * @param launcher Callable that either returns one joining future or throws
   * before retaining/invoking its task.
   * @param evidence Caller-owned checkpoint record updated through cleanup.
   * @return Nothing.
   * @throws std::bad_alloc for setup/resource failure or injected launch
   * failure.
   * @throws std::runtime_error when a bounded synchronization checkpoint is not
   * reached.
   * @throws Any other launcher, future, or filesystem exception unchanged after
   * ordered cleanup.
   * @note The real success launcher uses `std::launch::async`; deterministic
   * failure launchers throw before task invocation and require no production
   * ABI or CMake test macro.
   */
  void run(const KernelDestructionLauncher& launcher,
           KernelCodecTeardownEvidence& evidence) {
    evidence = KernelCodecTeardownEvidence{};
    try {
      prepare_admitted_work(evidence);

      CloseWaitingObserver close_observer;
      ScopedGraphStateExecutorHook close_hook(close_observer);
      KernelDestructionTask task = [this] {
        destruction_entered_.set_value();
        kernel_.reset();
      };
      evidence.launcher_invoked = true;
      destruction_ = launcher(std::move(task));
      evidence.launcher_returned = true;
      if (!destruction_.valid()) {
        throw std::runtime_error(
            "Kernel destruction launcher returned an invalid future");
      }
      if (destruction_entered_future_.wait_for(kCheckpointTimeout) !=
          std::future_status::ready) {
        throw std::runtime_error("Kernel destruction worker did not start");
      }
      destruction_entered_future_.get();
      evidence.destruction_entered = true;
      evidence.close_waiting =
          wait_for_atomic_true(close_observer.observed, kCheckpointTimeout);
      evidence.destruction_pending =
          destruction_.wait_for(std::chrono::milliseconds(0)) ==
          std::future_status::timeout;
      evidence.codec_alive_before_release = !weak_codec_.expired();
      evidence.encode_pending_before_release =
          !encode_finished_.load(std::memory_order_acquire);

      release_blocker_once(evidence);
      consume_future(blocker_, evidence.blocker_future_recovered);
      consume_future(cache_work_, evidence.cache_future_recovered);
      consume_future(destruction_, evidence.destruction_future_recovered);
      finalize_owners_and_paths(evidence);
    } catch (...) {
      const std::exception_ptr original_failure = std::current_exception();
      recover_after_failure(&evidence);
      std::rethrow_exception(original_failure);
    }
  }

 private:
  /** @brief Maximum wait used to prove a lifecycle checkpoint is bounded. */
  static constexpr std::chrono::milliseconds kCheckpointTimeout{2000};

  /**
   * @brief Creates the real graph, blocker, and admitted cache-save work.
   * @param evidence Record receiving setup and admission checkpoints.
   * @return Nothing.
   * @throws std::bad_alloc or filesystem/runtime exceptions from real setup.
   * @throws std::runtime_error when the blocker does not start by the deadline.
   * @note The external codec owner is released before graph-state work begins;
   * only Kernel's GraphCacheService retains it afterward.
   */
  void prepare_admitted_work(KernelCodecTeardownEvidence& evidence) {
    if (!cleanup_temporary_paths()) {
      throw std::runtime_error("Kernel teardown temporary cleanup failed");
    }
    write_text(yaml_path_, R"YAML(
- id: 1
  name: cached_source
  type: kernel_contract_test
  subtype: source
  parameters:
    width: 2
    height: 1
)YAML");

    auto codec = std::make_shared<testing::FakeImageArtifactCodec>(
        testing::FakeImageArtifactCodec::DecodeCallback{},
        [this](const std::filesystem::path&, const ImageBuffer&,
               ImageArtifactPrecision) {
          codec_alive_during_encode_.store(!weak_codec_.expired(),
                                           std::memory_order_release);
          encode_finished_.store(true, std::memory_order_release);
        });
    weak_codec_ = codec;
    kernel_ = ps::testing::make_unique_kernel_with_yaml_graph_documents(codec);
    codec.reset();

    const auto loaded =
        kernel_->load_graph(graph_name_, root_.string(), yaml_path_.string());
    if (!loaded.has_value()) {
      throw std::runtime_error("Kernel teardown graph did not load");
    }
    testing::KernelTestAccess::submit_graph_state(
        *kernel_, graph_name_,
        [](GraphModel& graph) {
          graph.mutate_node_runtime_state(
              1, [](GraphModel::NodeRuntimeState& state) {
                state.caches.push_back({"image", "output.png"});
                state.cached_output_high_precision = NodeOutput{};
                state.cached_output_high_precision->image_buffer =
                    make_aligned_cpu_image_buffer(2, 1, 1, DataType::FLOAT32);
              });
        })
        .get();

    blocker_ = testing::KernelTestAccess::submit_graph_state(
        *kernel_, graph_name_, [this](GraphModel&) {
          blocker_entered_.set_value();
          blocker_release_.wait();
        });
    if (blocker_entered_future_.wait_for(kCheckpointTimeout) !=
        std::future_status::ready) {
      throw std::runtime_error("graph-state lifetime blocker did not start");
    }
    blocker_entered_future_.get();
    evidence.blocker_entered = true;

    cache_work_ = testing::KernelTestAccess::submit_cache_save(
        *kernel_, graph_name_, 1, "int16");
    evidence.cache_work_pending =
        cache_work_.wait_for(std::chrono::milliseconds(0)) ==
        std::future_status::timeout;
  }

  /**
   * @brief Releases the graph-state blocker exactly once.
   * @param evidence Record updated after promise publication succeeds.
   * @return Nothing.
   * @throws std::future_error if the promise state is unexpectedly invalid.
   * @note Callers release before waiting on blocker, cache, or destruction
   * futures so Kernel teardown can never wait on a caller-held gate.
   */
  void release_blocker_once(KernelCodecTeardownEvidence& evidence) {
    if (!blocker_released_) {
      release_blocker_.set_value();
      blocker_released_ = true;
    }
    evidence.blocker_released = true;
  }

  /**
   * @brief Waits for and consumes one admitted future.
   * @param future Future whose task must complete before owner destruction.
   * @param recovered Set true only when readiness occurs within the deadline
   * and `get()` completes normally.
   * @return Nothing.
   * @throws std::runtime_error when readiness misses the bounded deadline.
   * @throws Any exception stored in the future.
   * @note Consumption is ordered blocker, cache work, then destruction worker.
   */
  static void consume_future(std::future<void>& future, bool& recovered) {
    if (!future.valid()) {
      throw std::runtime_error("required teardown future is invalid");
    }
    if (future.wait_for(kCheckpointTimeout) != std::future_status::ready) {
      throw std::runtime_error("teardown future missed its bounded deadline");
    }
    future.get();
    recovered = true;
  }

  /**
   * @brief Consumes one future during exception recovery without replacing the
   * original failure.
   * @param future Future whose admitted task must be joined when valid.
   * @param recovered Optional evidence field set only for bounded, successful
   * recovery.
   * @return Nothing.
   * @throws Nothing; future exceptions are suppressed after admission cleanup.
   * @note After recording bounded readiness, the helper still calls `get()` so
   * no admitted task outlives borrowed scenario state.
   */
  static void recover_future_noexcept(std::future<void>& future,
                                      bool* recovered) noexcept {
    if (!future.valid()) {
      return;
    }
    bool ready = false;
    try {
      ready = future.wait_for(kCheckpointTimeout) == std::future_status::ready;
      if (ready) {
        future.get();
      }
      if (recovered != nullptr) {
        *recovered = ready;
      }
    } catch (...) {
      if (recovered != nullptr) {
        *recovered = false;
      }
    }
  }

  /**
   * @brief Completes owner release and temporary-path cleanup.
   * @param evidence Record receiving final codec and filesystem checkpoints.
   * @return Nothing.
   * @throws Nothing.
   * @note Callers invoke this only after every admitted future is consumed.
   */
  void finalize_owners_and_paths(
      KernelCodecTeardownEvidence& evidence) noexcept {
    kernel_.reset();
    evidence.kernel_destroyed = kernel_ == nullptr;
    evidence.encode_finished = encode_finished_.load(std::memory_order_acquire);
    evidence.codec_alive_during_encode =
        codec_alive_during_encode_.load(std::memory_order_acquire);
    evidence.codec_released = weak_codec_.expired();
    evidence.temporary_paths_removed = cleanup_temporary_paths();
  }

  /**
   * @brief Recovers every admitted resource before an exception escapes.
   * @param evidence Optional record updated with successful cleanup
   * checkpoints.
   * @return Nothing.
   * @throws Nothing; the original setup or launcher exception remains primary.
   * @note Recovery order is blocker release, blocker/cache/destruction future
   * consumption, Kernel/codec destruction, then filesystem cleanup. Missing the
   * bounded recovery deadline terminates the test process rather than allowing
   * Kernel destruction to re-enter the same blocked lane.
   */
  void recover_after_failure(KernelCodecTeardownEvidence* evidence) noexcept {
    const bool has_admitted_work = blocker_.valid() || cache_work_.valid();
    if (has_admitted_work || destruction_.valid()) {
      try {
        if (!blocker_released_) {
          release_blocker_.set_value();
          blocker_released_ = true;
        }
        if (evidence != nullptr) {
          evidence->blocker_released = true;
        }
      } catch (...) {
        if (evidence != nullptr) {
          evidence->blocker_released = false;
        }
      }
    }

    recover_future_noexcept(
        blocker_,
        evidence == nullptr ? nullptr : &evidence->blocker_future_recovered);
    recover_future_noexcept(
        cache_work_,
        evidence == nullptr ? nullptr : &evidence->cache_future_recovered);
    recover_future_noexcept(destruction_,
                            evidence == nullptr
                                ? nullptr
                                : &evidence->destruction_future_recovered);
    if (destruction_.valid() || blocker_.valid() || cache_work_.valid()) {
      std::terminate();
    }
    if (evidence != nullptr) {
      finalize_owners_and_paths(*evidence);
    } else if (kernel_ != nullptr || has_admitted_work) {
      kernel_.reset();
      (void)cleanup_temporary_paths();
    }
  }

  /**
   * @brief Removes both deterministic temporary paths without throwing.
   * @return True when neither path exists after cleanup and no filesystem query
   * failed.
   * @throws Nothing.
   * @note Cleanup uses `std::error_code` so it never masks launcher exceptions.
   */
  bool cleanup_temporary_paths() noexcept {
    std::error_code root_remove_error;
    std::error_code yaml_remove_error;
    std::filesystem::remove_all(root_, root_remove_error);
    std::filesystem::remove(yaml_path_, yaml_remove_error);
    std::error_code root_exists_error;
    std::error_code yaml_exists_error;
    const bool root_exists = std::filesystem::exists(root_, root_exists_error);
    const bool yaml_exists =
        std::filesystem::exists(yaml_path_, yaml_exists_error);
    return !root_remove_error && !yaml_remove_error && !root_exists_error &&
           !yaml_exists_error && !root_exists && !yaml_exists;
  }

  /** @brief Loaded graph name unique to this scenario instance. */
  const std::string graph_name_;
  /** @brief Temporary session root removed after every outcome. */
  const std::filesystem::path root_;
  /** @brief Temporary source YAML removed after every outcome. */
  const std::filesystem::path yaml_path_;
  /**
   * @brief Caller-retained Kernel owner borrowed by a launched task.
   * @note Declared before blocker synchronization members so unexpected stack
   * unwinding destroys the blocker state first; `run()` must recover every
   * admitted future before allowing normal owner teardown.
   */
  std::unique_ptr<Kernel> kernel_;
  /** @brief Weak observer proving final codec release. */
  std::weak_ptr<testing::FakeImageArtifactCodec> weak_codec_;
  /** @brief True when encode observes its weak codec owner as live. */
  std::atomic<bool> codec_alive_during_encode_{false};
  /** @brief True after the admitted fake encode callback completes. */
  std::atomic<bool> encode_finished_{false};
  /** @brief Promise releasing the real graph-state blocker once. */
  std::promise<void> release_blocker_;
  /** @brief Shared release signal borrowed by the blocker callback. */
  std::shared_future<void> blocker_release_;
  /** @brief Promise publishing entry into the blocker callback. */
  std::promise<void> blocker_entered_;
  /** @brief Caller-side future for blocker entry. */
  std::future<void> blocker_entered_future_;
  /** @brief Future owning the admitted blocker work item. */
  std::future<void> blocker_;
  /** @brief Future owning the admitted cache-save work item. */
  std::future<void> cache_work_;
  /** @brief Promise publishing entry into successful Kernel destruction. */
  std::promise<void> destruction_entered_;
  /** @brief Caller-side future for destruction-worker entry. */
  std::future<void> destruction_entered_future_;
  /** @brief Future returned only after destruction launch succeeds. */
  std::future<void> destruction_;
  /** @brief Guards the single blocker promise publication. */
  bool blocker_released_ = false;
};

/**
 * @brief Owns common disk-cache diagnostic test state.
 *
 * The context prepares a clean cache root, constructs a GraphModel, creates one
 * cached process node, and removes the root in the destructor with
 * `std::error_code` so cleanup never masks assertion failures.
 *
 * @note The helper keeps the four disk-cache diagnostic tests focused on their
 * distinct miss/hit/error assertions instead of repeating filesystem setup.
 */
struct DiskCacheDiagnosticContext {
  /** @brief Fake codec owner retained for call and failure assertions. */
  std::shared_ptr<testing::FakeImageArtifactCodec> codec;
  /** @brief Cache service under test, injected with `codec`. */
  GraphCacheService cache;
  std::filesystem::path root;
  GraphModel graph;
  Node node;

  /**
   * @brief Creates a clean graph cache root and one cached process node.
   *
   * @param root_name Temporary directory name for this test case.
   * @param cache_location CacheEntry location to configure on the node.
   * @param decode Optional fake decode behavior for an existing image file.
   * @throws std::filesystem::filesystem_error if root cleanup or creation
   * fails.
   * @throws std::bad_alloc if fake codec ownership allocation fails.
   */
  DiskCacheDiagnosticContext(
      const std::string& root_name, const std::string& cache_location,
      testing::FakeImageArtifactCodec::DecodeCallback decode = {})
      : codec(std::make_shared<testing::FakeImageArtifactCodec>(
            std::move(decode))),
        cache(codec),
        root(clean_temp_path(root_name)),
        graph(root),
        node(make_cached_process_node(cache_location)) {}

  /**
   * @brief Removes the temporary cache root without throwing.
   *
   * @note Cleanup errors are intentionally ignored at teardown because the test
   * assertions already captured the behavior under validation.
   */
  ~DiskCacheDiagnosticContext() {
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
  }

  /**
   * @brief Returns the image cache path for the configured node.
   *
   * @return Resolved path for the node's first cache entry.
   * @throws std::bad_alloc if path construction cannot allocate.
   */
  std::filesystem::path cache_file() const {
    return cache.node_cache_dir(graph, node.id) / node.caches.front().location;
  }

  /**
   * @brief Returns the YAML metadata path for the configured node.
   *
   * @return Resolved `.yml` path paired with `cache_file()`.
   * @throws std::bad_alloc if path construction cannot allocate.
   */
  std::filesystem::path metadata_file() const {
    auto path = cache_file();
    path.replace_extension(".yml");
    return path;
  }
};

}  // namespace

TEST(ImageBufferContract, AlignedCpuRowsAndPaddedStep) {
  ImageBuffer buffer =
      make_aligned_cpu_image_buffer(17, 5, 3, DataType::FLOAT32);

  const auto base = reinterpret_cast<std::uintptr_t>(buffer.data.get());
  EXPECT_EQ(base % 64, 0u);
  EXPECT_EQ(buffer.step % 64, 0u);
  EXPECT_GT(buffer.step, 17u * 3u * sizeof(float));

  for (int y = 0; y < buffer.height; ++y) {
    const auto row = base + static_cast<std::uintptr_t>(y) * buffer.step;
    EXPECT_EQ(row % 64, 0u);
  }
}

TEST(ImageBufferContract, OpenCvAndTileAccessRespectPaddedStep) {
  ImageBuffer buffer =
      make_aligned_cpu_image_buffer(17, 4, 1, DataType::FLOAT32);
  cv::Mat mat = toCvMat(buffer);
  ASSERT_EQ(mat.step, buffer.step);
  ASSERT_FALSE(mat.isContinuous());
  mat.setTo(0.0f);

  Node node;
  OutputTile output_tile{&buffer, PixelRect{3, 1, 5, 2}};
  TileOpFunc write_tile = [](const Node&, const OutputTile& tile,
                             const std::vector<InputTile>&) {
    cv::Mat tile_mat = toCvMat(tile);
    tile_mat.setTo(7.0f);
  };
  write_tile(node, output_tile, {});

  EXPECT_FLOAT_EQ(mat.at<float>(1, 3), 7.0f);
  EXPECT_FLOAT_EQ(mat.at<float>(2, 7), 7.0f);
  EXPECT_FLOAT_EQ(mat.at<float>(0, 3), 0.0f);
  EXPECT_FLOAT_EQ(mat.at<float>(1, 8), 0.0f);
}

TEST(InteractionInspectionContracts,
     ReturnsStructuredDependencyTreeAndInspection) {
  auto root = temp_path("photospider-interaction-inspect-contract");
  std::filesystem::remove_all(root);
  const auto yaml_path = root / "graph.yaml";
  write_text(yaml_path, R"YAML(
- id: 1
  name: source
  type: kernel_contract_test
  subtype: source
  parameters:
    width: 17
    height: 3
- id: 2
  name: process
  type: kernel_contract_test
  subtype: process
  image_inputs:
    - from_node_id: 1
      from_output_name: image
)YAML");

  Kernel kernel = ps::testing::make_kernel_with_yaml_graph_documents();
  InteractionService svc(kernel);
  auto loaded =
      svc.cmd_load_graph("inspect_contract", root.string(), yaml_path.string());
  ASSERT_TRUE(loaded.has_value());

  auto tree = svc.cmd_dependency_tree(*loaded, 2, true);
  ASSERT_TRUE(tree.has_value());
  EXPECT_EQ(tree->scope, DependencyTree::Scope::StartNode);
  ASSERT_TRUE(tree->start_node_id.has_value());
  EXPECT_EQ(*tree->start_node_id, 2);
  EXPECT_TRUE(tree->start_node_found);
  ASSERT_EQ(tree->root_node_ids, std::vector<int>({2}));
  ASSERT_EQ(tree->entries.size(), 2u);

  EXPECT_EQ(tree->entries[0].depth, 0);
  EXPECT_FALSE(tree->entries[0].incoming_edge.has_value());
  EXPECT_EQ(tree->entries[0].node.id, 2);
  EXPECT_EQ(tree->entries[0].node.name, "process");
  ASSERT_TRUE(tree->entries[0].node.metadata.has_value());
  EXPECT_FALSE(tree->entries[0].node.metadata->has_cached_output);

  EXPECT_EQ(tree->entries[1].depth, 2);
  ASSERT_TRUE(tree->entries[1].incoming_edge.has_value());
  EXPECT_EQ(tree->entries[1].incoming_edge->kind,
            GraphTopologyEdgeKind::ImageInput);
  EXPECT_EQ(tree->entries[1].incoming_edge->from_node_id, 1);
  EXPECT_EQ(tree->entries[1].incoming_edge->to_node_id, 2);
  EXPECT_EQ(tree->entries[1].incoming_edge->from_output_name, "image");
  EXPECT_EQ(tree->entries[1].node.id, 1);
  EXPECT_EQ(tree->entries[1].node.parameters.at("width").as_int64(), 17);

  auto graph = svc.cmd_inspect_graph(*loaded);
  ASSERT_TRUE(graph.has_value());
  ASSERT_EQ(graph->nodes.size(), 2u);
  EXPECT_EQ(graph->nodes[0].id, 1);
  EXPECT_EQ(graph->nodes[1].id, 2);
  ASSERT_TRUE(graph->nodes[0].metadata.has_value());
  EXPECT_FALSE(graph->nodes[0].metadata->has_cached_output);

  std::filesystem::remove_all(root);
}

/**
 * @brief Verifies one document-to-Graph-to-operation ParameterValue path.
 *
 * @return Nothing; GoogleTest assertions report kind, value, merge, and output
 * mismatches.
 * @throws std::bad_alloc or filesystem exceptions if fixture setup fails.
 * @note The producer emits a named Int64 value which overrides the consumer's
 * static Int64 parameter without passing through YAML storage.
 */
TEST(ParameterValuePath, DocumentGraphAndOperationsStayFormatNeutral) {
  register_contract_ops();
  g_parameter_value_source_calls.store(0, std::memory_order_relaxed);
  g_parameter_value_consumer_calls.store(0, std::memory_order_relaxed);

  const std::string graph_name = "parameter_value_vertical_path";
  const auto root = clean_temp_path("photospider-parameter-value-root");
  const auto yaml_path = temp_path("photospider-parameter-value.yaml");
  write_text(yaml_path, R"YAML(
- id: 1
  name: parameter_source
  type: kernel_contract_test
  subtype: parameter_value_source
  parameters:
    enabled: true
    count: 7
    ratio: 1.25
    label: "007"
- id: 2
  name: parameter_consumer
  type: kernel_contract_test
  subtype: parameter_value_consumer
  parameter_inputs:
    - from_node_id: 1
      from_output_name: dynamic_count
      to_parameter_name: count
  parameters:
    enabled: false
    count: 2
    ratio: 2.5
    label: consumer
)YAML");

  Kernel kernel = ps::testing::make_kernel_with_yaml_graph_documents();
  const auto loaded =
      kernel.load_graph(graph_name, root.string(), yaml_path.string());
  ASSERT_TRUE(loaded.has_value());

  Kernel::ComputeRequest request;
  request.name = graph_name;
  request.node_id = 2;
  request.cache.precision = "int8";
  request.cache.force_recache = true;
  request.cache.disable_disk_cache = true;
  request.cache.nosave = true;
  request.execution.parallel = false;
  ASSERT_TRUE(kernel.compute(request));
  EXPECT_FALSE(kernel.last_error(graph_name).has_value());
  EXPECT_EQ(g_parameter_value_source_calls.load(std::memory_order_relaxed), 1);
  EXPECT_EQ(g_parameter_value_consumer_calls.load(std::memory_order_relaxed),
            1);

  const plugin::ParameterMap evidence =
      testing::KernelTestAccess::submit_graph_state(
          kernel, graph_name,
          [](GraphModel& graph) {
            const Node& source = graph.node(1);
            const Node& consumer = graph.node(2);
            plugin::ParameterMap snapshot;
            snapshot["enabled"] = source.parameters.at("enabled");
            snapshot["source_count"] = source.parameters.at("count");
            snapshot["ratio"] = source.parameters.at("ratio");
            snapshot["label"] = source.parameters.at("label");
            snapshot["consumer_static_count"] = consumer.parameters.at("count");
            snapshot["dynamic_count"] =
                source.cached_output_high_precision->data.at("dynamic_count");
            snapshot["final_width"] =
                consumer.cached_output_high_precision->image_buffer.width;
            snapshot["final_height"] =
                consumer.cached_output_high_precision->image_buffer.height;
            return snapshot;
          })
          .get();

  EXPECT_TRUE(evidence.at("enabled").as_bool());
  EXPECT_EQ(evidence.at("source_count").as_int64(), 7);
  EXPECT_DOUBLE_EQ(evidence.at("ratio").as_double(), 1.25);
  EXPECT_EQ(evidence.at("label").as_string(), "007");
  EXPECT_EQ(evidence.at("consumer_static_count").as_int64(), 2);
  EXPECT_EQ(evidence.at("dynamic_count").as_int64(), 11);
  EXPECT_EQ(evidence.at("final_width").as_int64(), 11);
  EXPECT_EQ(evidence.at("final_height").as_int64(), 3);

  kernel.close_graph(graph_name);
  std::filesystem::remove_all(root);
  std::filesystem::remove(yaml_path);
}

/**
 * @brief Verifies quoted numeric text fails an exact Int64 plugin accessor.
 *
 * @return Nothing; GoogleTest assertions report callback and error mismatch.
 * @throws std::bad_alloc or filesystem exceptions if fixture setup fails.
 * @note Loading succeeds because string is a valid parameter kind; failure
 * occurs only when the operation requires Int64.
 */
TEST(ParameterValuePath, ExactTypeMismatchFailsInsideOperation) {
  register_contract_ops();
  g_parameter_value_source_calls.store(0, std::memory_order_relaxed);
  g_parameter_value_consumer_calls.store(0, std::memory_order_relaxed);

  const std::string graph_name = "parameter_value_type_mismatch";
  const auto root =
      clean_temp_path("photospider-parameter-value-mismatch-root");
  const auto yaml_path = temp_path("photospider-parameter-value-mismatch.yaml");
  write_text(yaml_path, R"YAML(
- id: 1
  name: parameter_source
  type: kernel_contract_test
  subtype: parameter_value_source
  parameters:
    enabled: true
    count: "7"
    ratio: 1.25
    label: "007"
)YAML");

  Kernel kernel = ps::testing::make_kernel_with_yaml_graph_documents();
  const auto loaded =
      kernel.load_graph(graph_name, root.string(), yaml_path.string());
  ASSERT_TRUE(loaded.has_value());

  Kernel::ComputeRequest request;
  request.name = graph_name;
  request.node_id = 1;
  request.cache.precision = "int8";
  request.cache.force_recache = true;
  request.cache.disable_disk_cache = true;
  request.cache.nosave = true;
  EXPECT_FALSE(kernel.compute(request));
  const auto error = kernel.last_error(graph_name);
  ASSERT_TRUE(error.has_value());
  EXPECT_EQ(error->code, GraphErrc::ComputeError);
  EXPECT_EQ(g_parameter_value_source_calls.load(std::memory_order_relaxed), 1);
  EXPECT_EQ(g_parameter_value_consumer_calls.load(std::memory_order_relaxed),
            0);

  kernel.close_graph(graph_name);
  std::filesystem::remove_all(root);
  std::filesystem::remove(yaml_path);
}

TEST(CacheSemantics, HpAndRtComputePopulateFormalCaches) {
  register_contract_ops();
  GraphTraversalService traversal;
  GraphCacheService cache{providers::make_configured_image_artifact_codec()};
  GraphEventService events;
  ComputeService compute(traversal, cache, events);

  GraphModel graph(temp_path("photospider-contract-cache"));
  graph.add_node(make_contract_node());
  graph.add_node(make_contract_process_node());
  graph.validate_topology();

  // HP compute populates cached_output_high_precision only
  ComputeService::Request hp_request;
  hp_request.node_id = 2;
  hp_request.cache.precision = "int8";
  hp_request.cache.disable_disk_cache = true;
  hp_request.intent = ComputeIntent::GlobalHighPrecision;
  NodeOutput& hp = compute.compute(graph, hp_request);
  EXPECT_TRUE(graph.node(2).cached_output_high_precision.has_value());
  EXPECT_EQ(hp.image_buffer.width,
            graph.node(2).cached_output_high_precision->image_buffer.width);

  // Snapshot HP cache dimensions before RT compute
  int hp_w_before =
      graph.node(2).cached_output_high_precision->image_buffer.width;
  int hp_h_before =
      graph.node(2).cached_output_high_precision->image_buffer.height;

  // RT compute returns proxy output; cached_output_high_precision must remain
  // unchanged.
  ComputeService::Request rt_request = hp_request;
  rt_request.intent = ComputeIntent::RealTimeUpdate;
  rt_request.dirty_roi = PixelRect{0, 0, 8, 8};
  NodeOutput& rt = compute.compute(graph, rt_request);

  // Key contract: RT compute must NOT alter the formal HP cache
  ASSERT_TRUE(graph.node(2).cached_output_high_precision.has_value());
  EXPECT_EQ(graph.node(2).cached_output_high_precision->image_buffer.width,
            hp_w_before)
      << "RT compute must not change HP cache width";
  EXPECT_EQ(graph.node(2).cached_output_high_precision->image_buffer.height,
            hp_h_before)
      << "RT compute must not change HP cache height";

  // RT output should be downscaled relative to HP
  EXPECT_LE(rt.image_buffer.width, hp_w_before)
      << "RT output should be <= HP output width";
}

TEST(CacheSemantics, DiskSaveAndSyncIgnoreNodesWithoutHpState) {
  register_contract_ops();
  GraphTraversalService traversal;
  GraphCacheService cache{providers::make_configured_image_artifact_codec()};
  GraphEventService events;
  ComputeService compute(traversal, cache, events);

  auto root = temp_path("photospider-contract-disk-cache");
  std::filesystem::remove_all(root);
  GraphModel graph(root);

  // Node 1: source (no caches entry → won't be persisted to disk)
  graph.add_node(make_contract_node());

  // Node 2: process with HP cache + caches entry → should be saved to disk
  graph.add_node(make_contract_process_node());
  graph.mutate_node_runtime_state(
      2, [](auto& state) { state.caches.push_back({"image", "output.png"}); });

  // Node 3: process with caches entry but no HP state. RT proxy state is not
  // stored on GraphModel and therefore cannot protect disk cache files.
  Node rt_only_node;
  rt_only_node.id = 3;
  rt_only_node.name = "rt_only";
  rt_only_node.type = "kernel_contract_test";
  rt_only_node.subtype = "process";
  rt_only_node.image_inputs.push_back(ImageInput{1, "image"});
  rt_only_node.caches.push_back({"image", "rt_output.png"});
  graph.add_node(rt_only_node);

  graph.validate_topology();

  // HP compute for node 2 — also computes node 1 as dependency
  ComputeService::Request hp_request;
  hp_request.node_id = 2;
  hp_request.cache.precision = "int8";
  hp_request.cache.disable_disk_cache = true;
  hp_request.intent = ComputeIntent::GlobalHighPrecision;
  compute.compute(graph, hp_request);
  ASSERT_TRUE(graph.node(2).cached_output_high_precision.has_value());

  // Create stale disk file for node 3 (simulating leftover from a previous HP
  // run that no longer has valid HP cache).
  auto dir3 = cache.node_cache_dir(graph, 3);
  std::filesystem::create_directories(dir3);
  auto stale_file = dir3 / "rt_output.png";
  {
    std::ofstream out(stale_file, std::ios::binary);
    out << "stale";
  }
  ASSERT_TRUE(std::filesystem::exists(stale_file));

  // --- Perform sync ---
  auto sync_result = cache.synchronize_disk_cache(graph, "int8");

  // Contract 1: Node 2 has HP cache → should be saved to disk
  EXPECT_GE(sync_result.saved_nodes, 1)
      << "Nodes with HP cache should be saved to disk";

  // Contract 2: node 3 has NO HP cache, so stale disk files must be cleaned
  // up. RT proxy state is outside GraphModel and cannot protect stale files.
  EXPECT_FALSE(std::filesystem::exists(stale_file))
      << "Stale disk files for nodes without HP cache should be removed";
  EXPECT_GE(sync_result.removed_files, 1)
      << "Sync should report removed stale files for nodes without HP cache";

  // Contract 3: Node 2 has HP cache, so its configured artifact is encoded.
  auto dir2 = cache.node_cache_dir(graph, 2);
  EXPECT_TRUE(std::filesystem::exists(dir2))
      << "HP cache directory should exist after sync";

  // Clean up
  std::filesystem::remove_all(root);
}

TEST(CacheSemantics, DiskCacheMissRecordsDiagnostic) {
  DiskCacheDiagnosticContext ctx("photospider-contract-disk-cache-miss",
                                 "missing.png");

  NodeOutput out;
  EXPECT_FALSE(
      ctx.cache.try_load_from_disk_cache_into(ctx.graph, ctx.node, out));

  const auto result = ctx.graph.last_disk_cache_load_result_snapshot();
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result->status, GraphModel::DiskCacheLoadStatus::Miss);
  EXPECT_EQ(result->code, GraphErrc::Unknown);
  EXPECT_EQ(result->node_id, ctx.node.id);
  EXPECT_EQ(result->location, "missing.png");
  EXPECT_NE(result->message.find("No disk cache files"), std::string::npos);
}

TEST(CacheSemantics, DiskCacheMetadataHitPreservesTryLoadBehavior) {
  DiskCacheDiagnosticContext ctx("photospider-contract-disk-cache-hit",
                                 "output.png");
  auto metadata_file = ctx.metadata_file();
  write_text(metadata_file, "answer: 42\nlabel: cached\n");

  NodeOutput out;
  EXPECT_TRUE(
      ctx.cache.try_load_from_disk_cache_into(ctx.graph, ctx.node, out));
  ASSERT_NE(out.data.find("answer"), out.data.end());
  ASSERT_NE(out.data.find("label"), out.data.end());
  EXPECT_EQ(out.data.at("answer").as_int64(), 42);
  EXPECT_EQ(out.data.at("label").as_string(), "cached");

  const auto result = ctx.graph.last_disk_cache_load_result_snapshot();
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result->status, GraphModel::DiskCacheLoadStatus::Hit);
  EXPECT_EQ(result->code, GraphErrc::Unknown);
  EXPECT_EQ(result->metadata_file, metadata_file);
}

TEST(CacheSemantics, DiskCacheInvalidMetadataRecordsErrorDiagnostic) {
  DiskCacheDiagnosticContext ctx("photospider-contract-disk-cache-bad-yaml",
                                 "output.png");
  auto metadata_file = ctx.metadata_file();
  write_text(metadata_file, "answer: [1, 2\n");

  NodeOutput out;
  EXPECT_FALSE(
      ctx.cache.try_load_from_disk_cache_into(ctx.graph, ctx.node, out));

  const auto result = ctx.graph.last_disk_cache_load_result_snapshot();
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result->status, GraphModel::DiskCacheLoadStatus::Error);
  EXPECT_EQ(result->code, GraphErrc::InvalidYaml);
  EXPECT_EQ(result->metadata_file, metadata_file);
  EXPECT_NE(result->message.find("Failed to parse disk cache metadata"),
            std::string::npos);
}

TEST(CacheSemantics, InjectedCodecIoErrorLeavesHpCacheUnchanged) {
  DiskCacheDiagnosticContext ctx(
      "photospider-contract-disk-cache-codec-io", "output.png",
      [](const std::filesystem::path& path) -> ImageBuffer {
        throw GraphError(
            GraphErrc::Io,
            "fake decode rejected image artifact: " + path.string());
      });
  auto image_file = ctx.cache_file();
  write_text(image_file, "fake image bytes");

  EXPECT_FALSE(ctx.cache.try_load_from_disk_cache(ctx.graph, ctx.node));
  EXPECT_FALSE(ctx.node.cached_output_high_precision.has_value());

  const auto calls = ctx.codec->calls();
  ASSERT_EQ(calls.size(), 1u);
  EXPECT_EQ(calls.front().kind,
            testing::FakeImageArtifactCodec::Call::Kind::Decode);
  EXPECT_EQ(calls.front().path, image_file);

  const auto result = ctx.graph.last_disk_cache_load_result_snapshot();
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result->status, GraphModel::DiskCacheLoadStatus::Error);
  EXPECT_EQ(result->code, GraphErrc::Io);
  EXPECT_EQ(result->cache_file, image_file);
  EXPECT_NE(result->message.find("fake decode rejected image artifact"),
            std::string::npos);
}

TEST(CacheSemantics, InjectedCodecBadAllocPropagatesWithoutHpMutation) {
  DiskCacheDiagnosticContext ctx(
      "photospider-contract-disk-cache-codec-bad-alloc", "output.png",
      [](const std::filesystem::path&) -> ImageBuffer {
        throw std::bad_alloc();
      });
  write_text(ctx.cache_file(), "fake image bytes");

  EXPECT_THROW(ctx.cache.try_load_from_disk_cache(ctx.graph, ctx.node),
               std::bad_alloc);
  EXPECT_FALSE(ctx.node.cached_output_high_precision.has_value());
  ASSERT_EQ(ctx.codec->calls().size(), 1u);
}

TEST(CacheSemantics, InjectedCodecLifetimeAndPrecisionFollowCacheService) {
  const auto root = clean_temp_path("photospider-contract-codec-lifetime");
  GraphModel graph(root);
  Node node = make_cached_process_node("output.png");
  node.cached_output_high_precision = NodeOutput{};
  node.cached_output_high_precision->image_buffer =
      make_aligned_cpu_image_buffer(2, 1, 1, DataType::FLOAT32);
  const std::filesystem::path expected_path =
      root / std::to_string(node.id) / node.caches.front().location;

  std::weak_ptr<testing::FakeImageArtifactCodec> weak_codec;
  {
    auto codec = std::make_shared<testing::FakeImageArtifactCodec>();
    weak_codec = codec;
    GraphCacheService cache(codec);
    codec.reset();
    ASSERT_FALSE(weak_codec.expired());

    cache.save_cache_if_configured(graph, node, "int16");
    const auto retained = weak_codec.lock();
    ASSERT_TRUE(retained);
    const auto calls = retained->calls();
    ASSERT_EQ(calls.size(), 1u);
    EXPECT_EQ(calls.front().kind,
              testing::FakeImageArtifactCodec::Call::Kind::Encode);
    EXPECT_EQ(calls.front().path, expected_path);
    ASSERT_TRUE(calls.front().precision.has_value());
    EXPECT_EQ(*calls.front().precision, ImageArtifactPrecision::UInt16);
  }

  EXPECT_TRUE(weak_codec.expired());
  std::filesystem::remove_all(root);
}

/**
 * @brief Proves Kernel drains admitted cache work before releasing its codec.
 *
 * @return Nothing; GoogleTest assertions report lifecycle-ordering failures.
 * @throws std::bad_alloc or filesystem/runtime exceptions if setup or launch
 * fails.
 * @note The production-shaped launcher moves only a borrowing task into
 * `std::async`; the calling thread retains the unique Kernel owner until launch
 * returns successfully. Executor checkpoints prove destruction waits for real
 * runtime drainage, encode observes a live codec, and codec release follows
 * Kernel destruction.
 */
TEST(CacheSemantics,
     InjectedCodecKernelDestructionDrainsWorkBeforeCodecRelease) {
  KernelCodecTeardownScenario scenario("success");
  KernelCodecTeardownEvidence evidence;
  scenario.run(
      [](KernelDestructionTask task) {
        return std::async(std::launch::async, std::move(task));
      },
      evidence);

  EXPECT_TRUE(evidence.blocker_entered);
  EXPECT_TRUE(evidence.cache_work_pending);
  EXPECT_TRUE(evidence.launcher_invoked);
  EXPECT_TRUE(evidence.launcher_returned);
  EXPECT_TRUE(evidence.destruction_entered);
  EXPECT_TRUE(evidence.close_waiting);
  EXPECT_TRUE(evidence.destruction_pending);
  EXPECT_TRUE(evidence.codec_alive_before_release);
  EXPECT_TRUE(evidence.encode_pending_before_release);
  EXPECT_TRUE(evidence.blocker_released);
  EXPECT_TRUE(evidence.blocker_future_recovered);
  EXPECT_TRUE(evidence.cache_future_recovered);
  EXPECT_TRUE(evidence.destruction_future_recovered);
  EXPECT_TRUE(evidence.kernel_destroyed);
  EXPECT_TRUE(evidence.encode_finished);
  EXPECT_TRUE(evidence.codec_alive_during_encode);
  EXPECT_TRUE(evidence.codec_released);
  EXPECT_TRUE(evidence.temporary_paths_removed);
}

/**
 * @brief Proves launcher allocation failure cannot deadlock Kernel teardown.
 *
 * @return Nothing; GoogleTest assertions report propagation or cleanup failure.
 * @throws std::bad_alloc only when the scenario fails to catch the
 * deterministic injected launcher error.
 * @note Failure is injected after a real blocker starts and real cache work is
 * admitted, but before the borrowing task is invoked or retained. The scenario
 * must release the blocker, recover both admitted futures, destroy Kernel and
 * codec, remove temporary paths, and only then propagate the original
 * `std::bad_alloc` to this test.
 */
TEST(CacheSemantics,
     InjectedCodecKernelDestructionLaunchBadAllocRecoversWithoutHang) {
  KernelCodecTeardownScenario scenario("launcher-bad-alloc");
  KernelCodecTeardownEvidence evidence;
  bool caught_bad_alloc = false;
  try {
    scenario.run(
        [](KernelDestructionTask) -> std::future<void> {
          throw std::bad_alloc{};
        },
        evidence);
  } catch (const std::bad_alloc&) {
    caught_bad_alloc = true;
  }

  EXPECT_TRUE(caught_bad_alloc);
  EXPECT_TRUE(evidence.blocker_entered);
  EXPECT_TRUE(evidence.cache_work_pending);
  EXPECT_TRUE(evidence.launcher_invoked);
  EXPECT_FALSE(evidence.launcher_returned);
  EXPECT_FALSE(evidence.destruction_entered);
  EXPECT_TRUE(evidence.blocker_released);
  EXPECT_TRUE(evidence.blocker_future_recovered);
  EXPECT_TRUE(evidence.cache_future_recovered);
  EXPECT_FALSE(evidence.destruction_future_recovered);
  EXPECT_TRUE(evidence.kernel_destroyed);
  EXPECT_TRUE(evidence.encode_finished);
  EXPECT_TRUE(evidence.codec_alive_during_encode);
  EXPECT_TRUE(evidence.codec_released);
  EXPECT_TRUE(evidence.temporary_paths_removed);
}

TEST(CacheSemantics, DiskCacheDiagnosticSnapshotSupportsConcurrentWriters) {
  GraphModel graph(
      temp_path("photospider-contract-disk-cache-diagnostic-lock"));
  std::promise<void> release;
  auto ready = release.get_future().share();
  std::vector<std::future<void>> writers;
  constexpr int kWriterCount = 32;

  for (int i = 0; i < kWriterCount; ++i) {
    writers.push_back(std::async(std::launch::async, [&, i]() {
      ready.wait();
      GraphModel::DiskCacheLoadResult result;
      result.node_id = i;
      result.location = "entry-" + std::to_string(i) + ".png";
      result.status = GraphModel::DiskCacheLoadStatus::Miss;
      result.message = "concurrent diagnostic " + std::to_string(i);
      graph.record_disk_cache_load_result(std::move(result));

      const auto snapshot = graph.last_disk_cache_load_result_snapshot();
      ASSERT_TRUE(snapshot.has_value());
      EXPECT_FALSE(snapshot->message.empty());
    }));
  }

  release.set_value();
  for (auto& writer : writers) {
    writer.get();
  }

  const auto final_snapshot = graph.last_disk_cache_load_result_snapshot();
  ASSERT_TRUE(final_snapshot.has_value());
  EXPECT_GE(final_snapshot->node_id, 0);
  EXPECT_LT(final_snapshot->node_id, kWriterCount);
  EXPECT_EQ(final_snapshot->status, GraphModel::DiskCacheLoadStatus::Miss);
}

TEST(ComputeContracts, RealTimeUpdateWithoutDirtyRoiFailsClearly) {
  register_contract_ops();
  GraphTraversalService traversal;
  GraphCacheService cache{providers::make_configured_image_artifact_codec()};
  GraphEventService events;
  ComputeService compute(traversal, cache, events);

  GraphModel graph(temp_path("photospider-contract-rt-error"));
  graph.add_node(make_contract_node());

  ComputeService::Request request;
  request.node_id = 1;
  request.cache.precision = "int8";
  request.cache.disable_disk_cache = true;
  request.intent = ComputeIntent::RealTimeUpdate;
  EXPECT_THROW(compute.compute(graph, request), GraphError);
}

TEST(KernelExceptionContracts, BadAllocEscapesRuntimeAndGraphStateWrappers) {
  const std::string graph_name = "contract_bad_alloc_wrappers";
  const auto root = clean_temp_path("photospider-contract-bad-alloc-root");
  const auto yaml_path = temp_path("photospider-contract-bad-alloc.yaml");
  write_missing_op_graph(yaml_path);

  Kernel kernel = ps::testing::make_kernel_with_yaml_graph_documents();
  const auto loaded =
      kernel.load_graph(graph_name, root.string(), yaml_path.string());
  ASSERT_TRUE(loaded.has_value());

  EXPECT_THROW(
      (void)testing::KernelTestAccess::inject_bad_alloc_through_runtime(
          kernel, graph_name),
      std::bad_alloc);
  EXPECT_THROW(
      (void)testing::KernelTestAccess::inject_bad_alloc_through_graph_state(
          kernel, graph_name),
      std::bad_alloc);
  EXPECT_THROW(
      (void)testing::KernelTestAccess::
          inject_bad_alloc_through_last_error_graph_state(kernel, graph_name),
      std::bad_alloc);

  kernel.close_graph(graph_name);
  std::filesystem::remove_all(root);
}

TEST(ComputeContracts, SyncFailureRestoresRequestScopedGraphState) {
  register_contract_ops();
  const std::string graph_name = "contract_sync_state_restore";
  const auto root = clean_temp_path("photospider-contract-sync-state-root");
  const auto yaml_path = temp_path("photospider-contract-sync-state.yaml");
  write_missing_op_graph(yaml_path);

  Kernel kernel = ps::testing::make_kernel_with_yaml_graph_documents();
  auto loaded =
      kernel.load_graph(graph_name, root.string(), yaml_path.string());
  ASSERT_TRUE(loaded.has_value());
  testing::KernelTestAccess::submit_graph_state(
      kernel, graph_name,
      [](GraphModel& graph) {
        graph.set_quiet(false);
        graph.set_skip_save_cache(true);
        return 0;
      })
      .get();

  Kernel::ComputeRequest request;
  request.name = graph_name;
  request.node_id = 1;
  request.cache.precision = "int8";
  request.cache.disable_disk_cache = true;
  request.cache.nosave = false;
  request.execution.quiet = true;

  EXPECT_FALSE(kernel.compute(request));
  auto restored =
      testing::KernelTestAccess::submit_graph_state(
          kernel, graph_name,
          [](GraphModel& graph) {
            return std::make_pair(graph.is_quiet(), graph.skip_save_cache());
          })
          .get();
  EXPECT_FALSE(restored.first);
  EXPECT_TRUE(restored.second);
  kernel.close_graph(graph_name);
  std::filesystem::remove_all(root);
}

TEST(ComputeContracts, AsyncParallelFailureRestoresRequestScopedGraphState) {
  register_contract_ops();
  const std::string graph_name = "contract_async_state_restore";
  const auto root = clean_temp_path("photospider-contract-async-state-root");
  const auto yaml_path = temp_path("photospider-contract-async-state.yaml");
  write_missing_op_graph(yaml_path);

  Kernel kernel = ps::testing::make_kernel_with_yaml_graph_documents();
  auto loaded =
      kernel.load_graph(graph_name, root.string(), yaml_path.string());
  ASSERT_TRUE(loaded.has_value());
  testing::KernelTestAccess::submit_graph_state(
      kernel, graph_name,
      [](GraphModel& graph) {
        graph.set_quiet(false);
        graph.set_skip_save_cache(true);
        return 0;
      })
      .get();

  Kernel::ComputeRequest request;
  request.name = graph_name;
  request.node_id = 1;
  request.cache.precision = "int8";
  request.cache.disable_disk_cache = true;
  request.cache.nosave = false;
  request.execution.parallel = true;
  request.execution.quiet = true;

  auto future = kernel.compute_async(request);
  ASSERT_TRUE(future.has_value());
  const Kernel::AsyncComputeResult outcome = future->get();
  EXPECT_FALSE(outcome.ok);
  ASSERT_TRUE(outcome.error.has_value());
  EXPECT_EQ(outcome.error->code, GraphErrc::ComputeError);
  EXPECT_FALSE(outcome.error->message.empty());
  auto restored =
      testing::KernelTestAccess::submit_graph_state(
          kernel, graph_name,
          [](GraphModel& graph) {
            return std::make_pair(graph.is_quiet(), graph.skip_save_cache());
          })
          .get();
  EXPECT_FALSE(restored.first);
  EXPECT_TRUE(restored.second);
  kernel.close_graph(graph_name);
  std::filesystem::remove_all(root);
}

/**
 * @brief Proves queued asynchronous failures retain work-item-owned errors.
 *
 * @throws Nothing when each future carries the exact request failure after both
 * work items have completed and the shared LastError has changed.
 * @note The two requests target one session and are submitted before either
 * result is consumed. One reaches operation lookup and the other fails node
 * lookup, making their GraphErrc values observably distinct.
 */
TEST(ComputeContracts, OverlappingAsyncFailuresOwnExactKernelResults) {
  register_contract_ops();
  const std::string graph_name = "contract_async_exact_errors";
  const auto root = clean_temp_path("photospider-contract-async-exact-root");
  const auto yaml_path = temp_path("photospider-contract-async-exact.yaml");
  write_missing_op_graph(yaml_path);

  Kernel kernel = ps::testing::make_kernel_with_yaml_graph_documents();
  auto loaded =
      kernel.load_graph(graph_name, root.string(), yaml_path.string());
  ASSERT_TRUE(loaded.has_value());

  Kernel::ComputeRequest missing_op_request;
  missing_op_request.name = graph_name;
  missing_op_request.node_id = 1;
  missing_op_request.cache.precision = "int8";
  missing_op_request.cache.force_recache = true;
  missing_op_request.cache.disable_disk_cache = true;
  missing_op_request.execution.parallel = true;
  Kernel::ComputeRequest missing_node_request = missing_op_request;
  missing_node_request.node_id = 99;

  auto missing_op_future = kernel.compute_async(missing_op_request);
  auto missing_node_future = kernel.compute_async(missing_node_request);
  ASSERT_TRUE(missing_op_future.has_value());
  ASSERT_TRUE(missing_node_future.has_value());
  ASSERT_EQ(missing_op_future->wait_for(std::chrono::seconds(2)),
            std::future_status::ready);
  ASSERT_EQ(missing_node_future->wait_for(std::chrono::seconds(2)),
            std::future_status::ready);

  const Kernel::AsyncComputeResult missing_op = missing_op_future->get();
  const Kernel::AsyncComputeResult missing_node = missing_node_future->get();
  EXPECT_FALSE(missing_op.ok);
  ASSERT_TRUE(missing_op.error.has_value());
  EXPECT_EQ(missing_op.error->code, GraphErrc::ComputeError);
  EXPECT_FALSE(missing_op.error->message.empty());
  EXPECT_FALSE(missing_node.ok);
  ASSERT_TRUE(missing_node.error.has_value());
  EXPECT_EQ(missing_node.error->code, GraphErrc::NotFound);
  EXPECT_FALSE(missing_node.error->message.empty());
  EXPECT_NE(missing_op.error->message, missing_node.error->message);

  const auto shared_error = kernel.last_error(graph_name);
  ASSERT_TRUE(shared_error.has_value());
  EXPECT_TRUE(shared_error->code == GraphErrc::NoOperation ||
              shared_error->code == GraphErrc::NotFound);

  EXPECT_TRUE(kernel.close_graph(graph_name));
  std::filesystem::remove_all(root);
}

TEST(ComputeContracts, ParallelComputeSerializesGraphStateOperations) {
  register_contract_ops();
  const std::string graph_name = "contract_parallel_graph_state";
  const auto root = clean_temp_path("photospider-contract-parallel-state-root");
  const auto yaml_path = temp_path("photospider-contract-parallel-state.yaml");
  write_blocking_source_graph(yaml_path);

  Kernel kernel = ps::testing::make_kernel_with_yaml_graph_documents();
  auto loaded =
      kernel.load_graph(graph_name, root.string(), yaml_path.string());
  ASSERT_TRUE(loaded.has_value());

  std::promise<void> release_compute;
  configure_blocking_contract_source(release_compute.get_future().share());

  Kernel::ComputeRequest request;
  request.name = graph_name;
  request.node_id = 1;
  request.cache.precision = "int8";
  request.cache.force_recache = true;
  request.cache.disable_disk_cache = true;
  request.execution.parallel = true;

  auto compute_future = kernel.compute_async(request);
  ASSERT_TRUE(compute_future.has_value());
  EXPECT_TRUE(
      wait_for_blocking_contract_source(std::chrono::milliseconds(2000)));

  std::atomic<bool> post_ran{false};
  auto post_future = testing::KernelTestAccess::submit_graph_state(
      kernel, graph_name, [&post_ran](GraphModel& graph) {
        post_ran.store(true, std::memory_order_release);
        return graph.node_count();
      });

  EXPECT_EQ(post_future.wait_for(std::chrono::milliseconds(100)),
            std::future_status::timeout);
  EXPECT_FALSE(post_ran.load(std::memory_order_acquire));

  release_compute.set_value();
  const Kernel::AsyncComputeResult outcome = compute_future->get();
  EXPECT_TRUE(outcome.ok);
  EXPECT_FALSE(outcome.error.has_value());
  EXPECT_EQ(post_future.wait_for(std::chrono::milliseconds(2000)),
            std::future_status::ready);
  EXPECT_EQ(post_future.get(), 1u);
  EXPECT_TRUE(post_ran.load(std::memory_order_acquire));

  reset_blocking_contract_source();
  kernel.close_graph(graph_name);
  std::filesystem::remove_all(root);
}

/**
 * @brief Verifies scheduler info and replacement wait for active compute.
 *
 * @throws Nothing when both scheduler calls remain pending until the blocking
 * compute releases the graph-state serialization boundary.
 * @note SerialDebugScheduler executes the blocking operation on its caller.
 * Without graph-state serialization, replacement could destroy that scheduler
 * while one of its member calls is still on the stack.
 */
TEST(ComputeContracts, SchedulerObservationAndReplacementWaitForCompute) {
  register_contract_ops();
  const std::string graph_name = "contract_scheduler_lifetime";
  const auto root = clean_temp_path("photospider-contract-scheduler-life-root");
  const auto yaml_path = temp_path("photospider-contract-scheduler-life.yaml");
  write_blocking_source_graph(yaml_path);

  Kernel kernel = ps::testing::make_kernel_with_yaml_graph_documents();
  auto loaded =
      kernel.load_graph(graph_name, root.string(), yaml_path.string());
  ASSERT_TRUE(loaded.has_value());
  ASSERT_TRUE(kernel.replace_scheduler(
      graph_name, ComputeIntent::GlobalHighPrecision, "serial_debug"));

  std::promise<void> release_compute;
  configure_blocking_contract_source(release_compute.get_future().share());

  Kernel::ComputeRequest request;
  request.name = graph_name;
  request.node_id = 1;
  request.cache.precision = "int8";
  request.cache.force_recache = true;
  request.cache.disable_disk_cache = true;
  request.execution.parallel = true;
  auto compute_future = kernel.compute_async(request);
  ASSERT_TRUE(compute_future.has_value());

  if (!wait_for_blocking_contract_source(std::chrono::seconds(2))) {
    release_compute.set_value();
    (void)compute_future->get();
    reset_blocking_contract_source();
    (void)kernel.close_graph(graph_name);
    std::filesystem::remove_all(root);
    FAIL() << "blocking scheduler compute did not start";
  }

  std::promise<void> info_entered;
  auto info_entered_future = info_entered.get_future();
  auto info_future = std::async(std::launch::async, [&] {
    info_entered.set_value();
    return kernel.get_scheduler_info(graph_name,
                                     ComputeIntent::GlobalHighPrecision);
  });
  std::promise<void> replace_entered;
  auto replace_entered_future = replace_entered.get_future();
  auto replace_future = std::async(std::launch::async, [&] {
    replace_entered.set_value();
    return kernel.replace_scheduler(
        graph_name, ComputeIntent::GlobalHighPrecision, "cpu_work_stealing");
  });

  EXPECT_EQ(info_entered_future.wait_for(std::chrono::seconds(2)),
            std::future_status::ready);
  EXPECT_EQ(replace_entered_future.wait_for(std::chrono::seconds(2)),
            std::future_status::ready);
  EXPECT_EQ(info_future.wait_for(std::chrono::milliseconds(100)),
            std::future_status::timeout);
  EXPECT_EQ(replace_future.wait_for(std::chrono::milliseconds(100)),
            std::future_status::timeout);

  release_compute.set_value();
  const Kernel::AsyncComputeResult compute_outcome = compute_future->get();
  EXPECT_TRUE(compute_outcome.ok);
  EXPECT_FALSE(compute_outcome.error.has_value());

  const auto observed_info = info_future.get();
  ASSERT_TRUE(observed_info.has_value());
  EXPECT_TRUE(observed_info->first == "serial_debug" ||
              observed_info->first == "CpuWorkStealingScheduler");
  EXPECT_FALSE(observed_info->second.empty());
  EXPECT_TRUE(replace_future.get());

  const auto final_info =
      kernel.get_scheduler_info(graph_name, ComputeIntent::GlobalHighPrecision);
  ASSERT_TRUE(final_info.has_value());
  EXPECT_EQ(final_info->first, "CpuWorkStealingScheduler");

  reset_blocking_contract_source();
  EXPECT_TRUE(kernel.close_graph(graph_name));
  std::filesystem::remove_all(root);
}

/**
 * @brief Verifies graph close waits behind accepted asynchronous compute.
 *
 * @throws Nothing when close remains pending until the graph-state work item
 * completes, then removes the runtime without invalidating its owned outcome.
 * @note The blocking operation creates a deterministic close/compute race and
 * avoids relying on a fixed operation sleep duration.
 */
TEST(ComputeContracts, CloseWaitsForAcceptedAsyncGraphStateWork) {
  register_contract_ops();
  const std::string graph_name = "contract_close_async_lifetime";
  const auto root = clean_temp_path("photospider-contract-close-life-root");
  const auto yaml_path = temp_path("photospider-contract-close-life.yaml");
  write_blocking_source_graph(yaml_path);

  Kernel kernel = ps::testing::make_kernel_with_yaml_graph_documents();
  auto loaded =
      kernel.load_graph(graph_name, root.string(), yaml_path.string());
  ASSERT_TRUE(loaded.has_value());

  std::promise<void> release_compute;
  configure_blocking_contract_source(release_compute.get_future().share());

  Kernel::ComputeRequest request;
  request.name = graph_name;
  request.node_id = 1;
  request.cache.precision = "int8";
  request.cache.force_recache = true;
  request.cache.disable_disk_cache = true;
  request.execution.parallel = true;
  auto compute_future = kernel.compute_async(request);
  ASSERT_TRUE(compute_future.has_value());

  if (!wait_for_blocking_contract_source(std::chrono::seconds(2))) {
    release_compute.set_value();
    (void)compute_future->get();
    reset_blocking_contract_source();
    (void)kernel.close_graph(graph_name);
    std::filesystem::remove_all(root);
    FAIL() << "blocking compute did not start before close";
  }

  std::promise<void> close_entered;
  auto close_entered_future = close_entered.get_future();
  auto close_future = std::async(std::launch::async, [&] {
    close_entered.set_value();
    return kernel.close_graph(graph_name);
  });
  EXPECT_EQ(close_entered_future.wait_for(std::chrono::seconds(2)),
            std::future_status::ready);
  EXPECT_EQ(close_future.wait_for(std::chrono::milliseconds(100)),
            std::future_status::timeout);

  release_compute.set_value();
  const Kernel::AsyncComputeResult outcome = compute_future->get();
  EXPECT_TRUE(outcome.ok);
  EXPECT_FALSE(outcome.error.has_value());
  EXPECT_TRUE(close_future.get());
  EXPECT_FALSE(kernel.last_error(graph_name).has_value());

  reset_blocking_contract_source();
  std::filesystem::remove_all(root);
}

/**
 * @brief Verifies a stopped runtime restarts only inside graph-state execution.
 *
 * @throws Nothing when an accepted compute stays queued with the runtime
 * stopped until the preceding graph-state task releases, then starts and
 * completes normally.
 * @note This directly guards the scheduler start/info/replace/close lifetime
 * rule: compute submission itself must not call GraphRuntime::start() outside
 * the serialization boundary.
 */
TEST(ComputeContracts, RuntimeRestartWaitsForGraphStateSerialization) {
  register_contract_ops();
  const std::string graph_name = "contract_serialized_runtime_restart";
  const auto root =
      clean_temp_path("photospider-contract-serialized-restart-root");
  const auto yaml_path =
      temp_path("photospider-contract-serialized-restart.yaml");
  write_blocking_source_graph(yaml_path);

  Kernel kernel = ps::testing::make_kernel_with_yaml_graph_documents();
  auto loaded =
      kernel.load_graph(graph_name, root.string(), yaml_path.string());
  ASSERT_TRUE(loaded.has_value());
  GraphRuntime& runtime =
      testing::KernelTestAccess::runtime(kernel, graph_name);
  runtime.stop();
  ASSERT_FALSE(runtime.running());

  std::promise<void> release_blocker;
  const std::shared_future<void> blocker_release =
      release_blocker.get_future().share();
  std::promise<void> blocker_entered;
  auto blocker_entered_future = blocker_entered.get_future();
  auto blocker = testing::KernelTestAccess::submit_graph_state(
      kernel, graph_name, [&blocker_entered, blocker_release](GraphModel&) {
        blocker_entered.set_value();
        blocker_release.wait();
        return 0;
      });
  if (blocker_entered_future.wait_for(std::chrono::seconds(2)) !=
      std::future_status::ready) {
    release_blocker.set_value();
    (void)blocker.get();
    (void)kernel.close_graph(graph_name);
    std::filesystem::remove_all(root);
    FAIL() << "graph-state blocker did not start";
  }

  Kernel::ComputeRequest request;
  request.name = graph_name;
  request.node_id = 1;
  request.cache.precision = "int8";
  request.cache.force_recache = true;
  request.cache.disable_disk_cache = true;
  request.execution.parallel = true;
  auto compute_future = kernel.compute_async(request);
  if (!compute_future) {
    release_blocker.set_value();
    (void)blocker.get();
    (void)kernel.close_graph(graph_name);
    std::filesystem::remove_all(root);
    FAIL() << "serialized restart compute was not accepted";
  }
  EXPECT_FALSE(runtime.running());
  EXPECT_EQ(compute_future->wait_for(std::chrono::milliseconds(100)),
            std::future_status::timeout);

  release_blocker.set_value();
  EXPECT_EQ(blocker.get(), 0);
  const Kernel::AsyncComputeResult outcome = compute_future->get();
  EXPECT_TRUE(outcome.ok);
  EXPECT_FALSE(outcome.error.has_value());
  EXPECT_TRUE(runtime.running());

  EXPECT_TRUE(kernel.close_graph(graph_name));
  std::filesystem::remove_all(root);
}

TEST(GraphModelContract, ClearResetsModelRuntimeState) {
  GraphModel graph(temp_path("photospider-contract-clear"));
  graph.add_node(make_contract_node());
  graph.timing_results.node_timings.push_back({1, "node", 1.0, "computed"});
  graph.timing_results.total_ms = 10.0;
  graph.total_io_time_ms.store(4.0);
  graph.set_skip_save_cache(true);
  graph.set_quiet(false);

  graph.clear();

  EXPECT_TRUE(graph.empty());
  EXPECT_TRUE(graph.timing_results.node_timings.empty());
  EXPECT_DOUBLE_EQ(graph.timing_results.total_ms, 0.0);
  EXPECT_DOUBLE_EQ(graph.total_io_time_ms.load(), 0.0);
  EXPECT_FALSE(graph.skip_save_cache());
  EXPECT_TRUE(graph.is_quiet());
}

TEST(GraphIoContract, FailedReloadPreservesPreviousGraph) {
  const auto valid_path = temp_path("photospider-contract-valid.yaml");
  const auto invalid_path = temp_path("photospider-contract-invalid.yaml");
  write_text(valid_path,
             "- id: 1\n"
             "  name: valid\n"
             "  type: kernel_contract_test\n"
             "  subtype: source\n");
  write_text(invalid_path,
             "- id: 1\n"
             "  name: invalid\n"
             "  type: kernel_contract_test\n"
             "  subtype: source\n"
             "  image_inputs:\n"
             "    - from_node_id: 99\n");

  GraphModel graph(temp_path("photospider-contract-reload-cache"));
  GraphIOService io = ps::testing::make_yaml_graph_io_service();
  io.load(graph, valid_path);
  ASSERT_EQ(graph.node(1).name, "valid");

  EXPECT_THROW(io.load(graph, invalid_path), GraphError);
  ASSERT_TRUE(graph.has_node(1));
  EXPECT_EQ(graph.node(1).name, "valid");
}

TEST(GraphIoContract, SuccessfulReloadResetsRuntimeMetadata) {
  const auto valid_path = temp_path("photospider-contract-runtime-old.yaml");
  const auto replacement_path =
      temp_path("photospider-contract-runtime-new.yaml");
  write_text(valid_path,
             "- id: 1\n"
             "  name: old_graph\n"
             "  type: kernel_contract_test\n"
             "  subtype: source\n");
  write_text(replacement_path,
             "- id: 3\n"
             "  name: new_graph\n"
             "  type: kernel_contract_test\n"
             "  subtype: source\n");

  GraphModel graph(temp_path("photospider-contract-reload-runtime-cache"));
  GraphIOService io = ps::testing::make_yaml_graph_io_service();
  io.load(graph, valid_path);
  ASSERT_EQ(graph.node(1).name, "old_graph");

  graph.timing_results.node_timings.push_back(
      {1, "old_graph", 9.0, "computed"});
  graph.timing_results.total_ms = 9.0;
  graph.total_io_time_ms.store(7.0);
  graph.set_skip_save_cache(true);
  graph.dirty_generation_counter = 42;
  graph.dirty_source_hp_commit_generation[1] = 42;
  graph.last_dirty_region_snapshot_debug = "stale dirty snapshot";
  compute::DirtyRegionSnapshot snapshot;
  snapshot.graph_generation = 42;
  snapshot.dirty_source_nodes.push_back(1);
  graph.last_dirty_region_snapshot = snapshot;
  graph.recent_dirty_region_snapshots.push_back(snapshot);
  compute::ComputePlan plan;
  plan.target_node_id = 1;
  graph.last_compute_plan = plan;
  graph.recent_compute_plans.push_back(plan);

  io.load(graph, replacement_path);

  ASSERT_FALSE(graph.has_node(1));
  ASSERT_TRUE(graph.has_node(3));
  EXPECT_EQ(graph.node(3).name, "new_graph");
  EXPECT_TRUE(graph.timing_results.node_timings.empty());
  EXPECT_DOUBLE_EQ(graph.timing_results.total_ms, 0.0);
  EXPECT_DOUBLE_EQ(graph.total_io_time_ms.load(), 0.0);
  EXPECT_FALSE(graph.skip_save_cache());
  EXPECT_EQ(graph.dirty_generation_counter, 0u);
  EXPECT_TRUE(graph.dirty_source_hp_commit_generation.empty());
  EXPECT_FALSE(graph.last_dirty_region_snapshot_debug.has_value());
  EXPECT_FALSE(graph.last_dirty_region_snapshot.has_value());
  EXPECT_TRUE(graph.recent_dirty_region_snapshots.empty());
  EXPECT_FALSE(graph.last_compute_plan.has_value());
  EXPECT_TRUE(graph.recent_compute_plans.empty());
}

#if defined(PHOTOSPIDER_INTERNAL_YAML_GRAPH_DOCUMENT_ADAPTER_TESTING)
/**
 * @brief Reports a stream failure that occurs after the destination opens.
 *
 * @return Nothing; GoogleTest assertions report exception-category mismatch.
 * @throws std::bad_alloc or filesystem exceptions if fixture setup fails.
 * @note The private failpoint marks the real stream bad only after its YAML
 * write. It does not throw or replace the writer, so this test remains red
 * unless the configured YAML adapter observes the late stream state itself.
 */
TEST(GraphIoContract, SaveReportsPostOpenWriteFailureAsIo) {
  GraphModel graph(temp_path("photospider-contract-save-late-write-cache"));
  graph.add_node(make_contract_node());
  GraphIOService io = ps::testing::make_yaml_graph_io_service();
  const auto output_path =
      temp_path("photospider-contract-save-late-write.yaml");
  std::filesystem::remove(output_path);

  testing::arm_yaml_graph_document_save_failure(
      output_path, testing::YamlGraphDocumentSaveFailureStage::AfterWrite);
  bool caught_io = false;
  try {
    io.save(graph, output_path);
  } catch (const GraphError& error) {
    caught_io = error.code() == GraphErrc::Io;
  }
  const std::size_t hit_count =
      testing::yaml_graph_document_save_failure_hit_count();
  testing::clear_yaml_graph_document_save_failure();

  EXPECT_TRUE(caught_io);
  EXPECT_EQ(hit_count, 1u);
  EXPECT_TRUE(std::filesystem::exists(output_path));
  std::filesystem::remove(output_path);
}

/**
 * @brief Reports a destination flush failure after YAML bytes are emitted.
 *
 * @return Nothing; GoogleTest assertions report exception-category mismatch.
 * @throws std::bad_alloc or filesystem exceptions if fixture setup fails.
 * @note The private failpoint changes only the real stream state after flush,
 * so a passing test proves the configured YAML adapter observes that status.
 */
TEST(GraphIoContract, SaveReportsPostWriteFlushFailureAsIo) {
  GraphModel graph(temp_path("photospider-contract-save-flush-cache"));
  graph.add_node(make_contract_node());
  GraphIOService io = ps::testing::make_yaml_graph_io_service();
  const auto output_path = temp_path("photospider-contract-save-flush.yaml");
  std::filesystem::remove(output_path);

  testing::arm_yaml_graph_document_save_failure(
      output_path, testing::YamlGraphDocumentSaveFailureStage::AfterFlush);
  bool caught_io = false;
  try {
    io.save(graph, output_path);
  } catch (const GraphError& error) {
    caught_io = error.code() == GraphErrc::Io;
  }
  const std::size_t hit_count =
      testing::yaml_graph_document_save_failure_hit_count();
  testing::clear_yaml_graph_document_save_failure();

  EXPECT_TRUE(caught_io);
  EXPECT_EQ(hit_count, 1u);
  EXPECT_TRUE(std::filesystem::exists(output_path));
  std::filesystem::remove(output_path);
}

/**
 * @brief Reports a destination close failure after successful flushing.
 *
 * @return Nothing; GoogleTest assertions report exception-category mismatch.
 * @throws std::bad_alloc or filesystem exceptions if fixture setup fails.
 * @note Explicit close makes the final stream state observable before the
 * ofstream destructor; the failpoint only marks that real stream failed.
 */
TEST(GraphIoContract, SaveReportsPostFlushCloseFailureAsIo) {
  GraphModel graph(temp_path("photospider-contract-save-close-cache"));
  graph.add_node(make_contract_node());
  GraphIOService io = ps::testing::make_yaml_graph_io_service();
  const auto output_path = temp_path("photospider-contract-save-close.yaml");
  std::filesystem::remove(output_path);

  testing::arm_yaml_graph_document_save_failure(
      output_path, testing::YamlGraphDocumentSaveFailureStage::AfterClose);
  bool caught_io = false;
  try {
    io.save(graph, output_path);
  } catch (const GraphError& error) {
    caught_io = error.code() == GraphErrc::Io;
  }
  const std::size_t hit_count =
      testing::yaml_graph_document_save_failure_hit_count();
  testing::clear_yaml_graph_document_save_failure();

  EXPECT_TRUE(caught_io);
  EXPECT_EQ(hit_count, 1u);
  EXPECT_TRUE(std::filesystem::exists(output_path));
  std::filesystem::remove(output_path);
}
#endif

/**
 * @brief Preserves the previous node when exact YAML replacement validation
 *        fails.
 *
 * @return Nothing; GoogleTest assertions report error-category or model-state
 *         mismatches.
 * @throws std::bad_alloc or filesystem exceptions if fixture setup cannot
 *         allocate or create its deterministic graph inputs.
 * @note The required-node Kernel boundary reports InvalidYaml while the
 *       candidate-map validation keeps the visible node unchanged.
 */
TEST(GraphMutationContract, InvalidNodeReplacementPreservesPreviousNode) {
  const auto root = temp_path("photospider-contract-kernel-root");
  const auto yaml_path = temp_path("photospider-contract-kernel.yaml");
  std::filesystem::remove_all(root);
  write_text(yaml_path,
             "- id: 1\n"
             "  name: valid\n"
             "  type: kernel_contract_test\n"
             "  subtype: source\n");

  Kernel kernel = ps::testing::make_kernel_with_yaml_graph_documents();
  auto loaded =
      kernel.load_graph("contract_graph", root.string(), yaml_path.string());
  ASSERT_TRUE(loaded.has_value());

  const std::string invalid_replacement =
      "id: 1\n"
      "name: invalid\n"
      "type: kernel_contract_test\n"
      "subtype: source\n"
      "image_inputs:\n"
      "  - from_node_id: 99\n";
  try {
    kernel.set_node_document("contract_graph", 1, invalid_replacement);
    FAIL() << "invalid node replacement unexpectedly succeeded";
  } catch (const GraphError& error) {
    EXPECT_EQ(error.code(), GraphErrc::InvalidYaml);
  }

  auto node_yaml = kernel.get_node_document("contract_graph", 1);
  ASSERT_TRUE(node_yaml.has_value());
  EXPECT_NE(node_yaml->find("valid"), std::string::npos);
  kernel.close_graph("contract_graph");
}

}  // namespace ps
