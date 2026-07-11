#include <gtest/gtest.h>

#if defined(_WIN32)
#include <windows.h>
#else
#include <dlfcn.h>
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <future>
#include <memory>
#include <mutex>
#include <new>
#include <sstream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include "adapter/buffer_adapter_opencv.hpp"
#include "kernel/scheduler/scheduler_task_runtime.hpp"  // NOLINT(build/include_subdir)
#include "node.hpp"  // NOLINT(build/include_subdir)
#include "photospider/host/host.hpp"
#include "ps_types.hpp"  // NOLINT(build/include_subdir)
#include "scheduler/scheduler_plugin_loader.hpp"  // NOLINT(build/include_subdir)

#ifndef PS_TEST_OP_PLUGIN_DIR
#define PS_TEST_OP_PLUGIN_DIR "build/test_plugins"
#endif

#ifndef PS_TEST_SCHEDULER_PLUGIN_PATH
#define PS_TEST_SCHEDULER_PLUGIN_PATH \
  "build/test_schedulers/libdestroy_count_scheduler_plugin.dylib"
#endif

namespace ps {

#if defined(PHOTOSPIDER_INTERNAL_BAD_ALLOC_TESTING)
/** @brief BUILD_TESTING close-coordination events mirrored from the Host. */
enum class EmbeddedCloseTestEvent {
  /** @brief One close caller has claimed the session marker. */
  MarkerClaimed,
  /** @brief A duplicate close caller is about to wait for marker release. */
  DuplicateAboutToWait,
};

/** @brief Borrowed callback installed for deterministic close synchronization.
 */
struct EmbeddedCloseTestHook {
  /** @brief Borrowed test context. */
  void* context = nullptr;
  /** @brief Non-throwing event callback. */
  void (*notify)(void* context,
                 EmbeddedCloseTestEvent event) noexcept = nullptr;
};

/**
 * @brief Installs or clears the embedded Host close test hook.
 * @param hook Hook that outlives concurrent close calls, or nullptr.
 * @return Nothing.
 * @throws Nothing.
 */
void set_embedded_host_close_test_hook(
    const EmbeddedCloseTestHook* hook) noexcept;
#endif

namespace {

/** @brief Serializes access to the Host blocking-operation release future. */
std::mutex g_host_blocking_source_mutex;

/** @brief Test-controlled release observed by the blocking Host operation. */
std::shared_future<void> g_host_blocking_source_release;

/** @brief Publishes entry into the blocking Host operation callback. */
std::atomic<bool> g_host_blocking_source_started{false};

#if defined(PHOTOSPIDER_INTERNAL_BAD_ALLOC_TESTING)
/** @brief Events published by the current embedded close coordination hook. */
struct EmbeddedCloseEventState {
  /** @brief Number of callers that have claimed the marker. */
  std::atomic<std::uint64_t> marker_claimed{0};
  /** @brief Number of duplicate callers that reached a condition wait. */
  std::atomic<std::uint64_t> duplicate_about_to_wait{0};
};

/**
 * @brief Records one embedded close event without allocating or blocking.
 * @param context Borrowed EmbeddedCloseEventState pointer.
 * @param event Coordination point reached by the Host.
 * @return Nothing.
 * @throws Nothing.
 */
void record_embedded_close_event(void* context,
                                 EmbeddedCloseTestEvent event) noexcept {
  auto* state = static_cast<EmbeddedCloseEventState*>(context);
  if (event == EmbeddedCloseTestEvent::MarkerClaimed) {
    state->marker_claimed.fetch_add(1, std::memory_order_release);
  } else {
    state->duplicate_about_to_wait.fetch_add(1, std::memory_order_release);
  }
}

/**
 * @brief Installs and assertion-safely clears one embedded close test hook.
 */
class ScopedEmbeddedCloseTestHook final {
 public:
  /**
   * @brief Installs a hook backed by the supplied event state.
   * @param state State that outlives this guard.
   * @throws Nothing.
   */
  explicit ScopedEmbeddedCloseTestHook(EmbeddedCloseEventState& state) noexcept
      : hook_{&state, &record_embedded_close_event} {
    set_embedded_host_close_test_hook(&hook_);
  }

  /** @brief Clears the borrowed hook before its state can be destroyed. */
  ~ScopedEmbeddedCloseTestHook() noexcept {
    set_embedded_host_close_test_hook(nullptr);
  }

  /**
   * @brief Prevents duplicate hook-installation ownership.
   * @param other Guard that remains installed.
   * @throws Nothing because construction is unavailable.
   */
  ScopedEmbeddedCloseTestHook(const ScopedEmbeddedCloseTestHook& other) =
      delete;

  /**
   * @brief Prevents replacing one installed hook.
   * @param other Guard whose hook remains installed.
   * @return No value because assignment is unavailable.
   * @throws Nothing because assignment is unavailable.
   */
  ScopedEmbeddedCloseTestHook& operator=(
      const ScopedEmbeddedCloseTestHook& other) = delete;

 private:
  /** @brief Hook object whose address remains stable while installed. */
  EmbeddedCloseTestHook hook_;
};

/**
 * @brief Waits until an atomic close event reaches one occurrence count.
 * @param event Event counter to observe.
 * @param expected Minimum count required for success.
 * @param timeout Maximum monotonic wait duration.
 * @return True when the count becomes visible before the deadline.
 * @throws Nothing.
 */
bool wait_for_embedded_close_event(const std::atomic<std::uint64_t>& event,
                                   std::uint64_t expected,
                                   std::chrono::milliseconds timeout) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    if (event.load(std::memory_order_acquire) >= expected) {
      return true;
    }
    std::this_thread::yield();
  }
  return event.load(std::memory_order_acquire) >= expected;
}
#endif

/** @brief Environment key selecting scheduler fixture lifecycle failures. */
constexpr const char* kSchedulerFailureEnvironment =  // NOLINT
    "PS_DESTROY_COUNT_SCHEDULER_FAILURE";             // NOLINT

/** @brief Scheduler type exported by the deterministic close-failure fixture.
 */
constexpr const char* kDestroyCountSchedulerType = "destroy_count_test";

/**
 * @brief Configures one deterministic blocking Host operation invocation.
 *
 * @param release Shared future whose readiness releases the operation.
 * @return Nothing.
 * @throws Nothing except implementation-defined mutex system errors.
 * @note The operation copies this future while holding the same mutex and then
 * waits without the mutex, so test cleanup can always release it.
 */
void configure_host_blocking_source(std::shared_future<void> release) {
  std::lock_guard<std::mutex> lock(g_host_blocking_source_mutex);
  g_host_blocking_source_started.store(false, std::memory_order_release);
  g_host_blocking_source_release = std::move(release);
}

/**
 * @brief Clears the deterministic blocking Host operation state.
 *
 * @return Nothing.
 * @throws Nothing except implementation-defined mutex system errors.
 * @note Tests call this only after every operation using the prior future has
 * completed.
 */
void reset_host_blocking_source() {
  std::lock_guard<std::mutex> lock(g_host_blocking_source_mutex);
  g_host_blocking_source_release = std::shared_future<void>();
  g_host_blocking_source_started.store(false, std::memory_order_release);
}

/**
 * @brief Waits for the blocking Host operation to enter its callback.
 *
 * @param timeout Maximum monotonic duration to poll.
 * @return True when callback entry is observed before the deadline.
 * @throws Nothing.
 * @note Five-millisecond polling bounds test latency without imposing a fixed
 * callback execution sleep.
 */
bool wait_for_host_blocking_source(std::chrono::milliseconds timeout) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    if (g_host_blocking_source_started.load(std::memory_order_acquire)) {
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  return g_host_blocking_source_started.load(std::memory_order_acquire);
}

/**
 * @brief Registers deterministic operations used by embedded Host tests.
 *
 * @throws std::bad_alloc if registry storage allocation fails.
 * @note The operation is intentionally tiny and CPU-only so Host seam tests
 *       exercise frontend behavior without depending on external plugins or
 *       GPU availability. The `resource_exhausted` operation deliberately
 *       throws std::bad_alloc from real node execution so the public Host
 *       exception contract is tested through the complete backend chain.
 */
void register_host_adapter_ops() {
  static std::once_flag once;
  std::call_once(once, [] {
    OpRegistry::instance().register_op_hp_monolithic(
        "host_adapter_test", "source",
        MonolithicOpFunc(
            [](const Node& node, const std::vector<const NodeOutput*>&) {
              const YAML::Node& params = node.runtime_parameters
                                             ? node.runtime_parameters
                                             : node.parameters;
              const int width = params["width"].as<int>(6);
              const int height = params["height"].as<int>(4);
              NodeOutput output;
              output.image_buffer = make_aligned_cpu_image_buffer(
                  width, height, 1, DataType::FLOAT32);
              cv::Mat mat = toCvMat(output.image_buffer);
              mat.setTo(7.0f);
              output.space.absolute_roi = cv::Rect(0, 0, width, height);
              output.debug.compute_device = "host-adapter-test";
              return output;
            }));
    OpRegistry::instance().register_op_hp_monolithic(
        "host_adapter_test", "slow_source",
        MonolithicOpFunc(
            [](const Node& node, const std::vector<const NodeOutput*>&) {
              const YAML::Node& params = node.runtime_parameters
                                             ? node.runtime_parameters
                                             : node.parameters;
              std::this_thread::sleep_for(
                  std::chrono::milliseconds(params["sleep_ms"].as<int>(50)));
              const int width = params["width"].as<int>(5);
              const int height = params["height"].as<int>(3);
              NodeOutput output;
              output.image_buffer = make_aligned_cpu_image_buffer(
                  width, height, 1, DataType::FLOAT32);
              cv::Mat mat = toCvMat(output.image_buffer);
              mat.setTo(3.0f);
              output.space.absolute_roi = cv::Rect(0, 0, width, height);
              output.debug.compute_device = "host-adapter-slow-test";
              return output;
            }));
    OpRegistry::instance().register_op_hp_monolithic(
        "host_adapter_test", "blocking_source",
        MonolithicOpFunc([](const Node& node,
                            const std::vector<const NodeOutput*>&) {
          std::shared_future<void> release;
          {
            std::lock_guard<std::mutex> lock(g_host_blocking_source_mutex);
            release = g_host_blocking_source_release;
          }
          g_host_blocking_source_started.store(true, std::memory_order_release);
          if (release.valid()) {
            release.wait();
          }
          const YAML::Node& params = node.runtime_parameters
                                         ? node.runtime_parameters
                                         : node.parameters;
          const int width = params["width"].as<int>(5);
          const int height = params["height"].as<int>(3);
          NodeOutput output;
          output.image_buffer = make_aligned_cpu_image_buffer(
              width, height, 1, DataType::FLOAT32);
          toCvMat(output.image_buffer).setTo(4.0f);
          output.space.absolute_roi = cv::Rect(0, 0, width, height);
          output.debug.compute_device = "host-adapter-blocking-test";
          return output;
        }));
    OpRegistry::instance().register_op_hp_monolithic(
        "host_adapter_test", "resized_extent",
        MonolithicOpFunc(
            [](const Node& node, const std::vector<const NodeOutput*>&) {
              const YAML::Node& params = node.runtime_parameters
                                             ? node.runtime_parameters
                                             : node.parameters;
              const int width = params["width"].as<int>(6);
              const int height = params["height"].as<int>(4);
              const int roi_width = params["roi_width"].as<int>(12);
              const int roi_height = params["roi_height"].as<int>(9);
              NodeOutput output;
              output.image_buffer = make_aligned_cpu_image_buffer(
                  width, height, 1, DataType::FLOAT32);
              cv::Mat mat = toCvMat(output.image_buffer);
              mat.setTo(5.0f);
              output.space.absolute_roi = cv::Rect(0, 0, roi_width, roi_height);
              output.space.inverse_matrix = {2.0, 0.0, 5.0, 0.0, 3.0,
                                             7.0, 0.0, 0.0, 1.0};
              output.space.local_inverse_matrix = {1.0,  0.0, 11.0, 0.0, 1.0,
                                                   13.0, 0.0, 0.0,  1.0};
              output.debug.compute_device = "host-adapter-resized-test";
              return output;
            }));
    OpRegistry::instance().register_op_hp_monolithic(
        "host_adapter_test", "no_image",
        MonolithicOpFunc(
            [](const Node&, const std::vector<const NodeOutput*>&) {
              return NodeOutput{};
            }));
    OpRegistry::instance().register_op_hp_monolithic(
        "host_adapter_test", "resource_exhausted",
        MonolithicOpFunc([](const Node&, const std::vector<const NodeOutput*>&)
                             -> NodeOutput { throw std::bad_alloc{}; }));
    OpRegistry::instance().register_op_hp_monolithic(
        "host_adapter_test", "identity",
        MonolithicOpFunc(
            [](const Node& node, const std::vector<const NodeOutput*>& inputs) {
              if (inputs.empty() || inputs.front() == nullptr) {
                throw GraphError(GraphErrc::InvalidParameter,
                                 "host adapter identity requires one input");
              }
              const NodeOutput& input = *inputs.front();
              NodeOutput output;
              output.image_buffer = make_aligned_cpu_image_buffer(
                  input.image_buffer.width, input.image_buffer.height,
                  input.image_buffer.channels, input.image_buffer.type);
              toCvMat(input.image_buffer).copyTo(toCvMat(output.image_buffer));
              output.space.absolute_roi = input.space.absolute_roi;
              output.debug.compute_device = "host-adapter-identity-test";
              (void)node;
              return output;
            }));
    OpRegistry::instance().register_dirty_propagator(
        "host_adapter_test", "identity",
        DirtyRoiPropFunc([](const Node&, const cv::Rect& roi,
                            const GraphModel&) { return roi; }));
    OpRegistry::instance().register_forward_propagator(
        "host_adapter_test", "identity",
        ForwardRoiPropFunc([](const Node&, const cv::Rect& roi,
                              const GraphModel&, const cv::Size&,
                              const cv::Size&) { return roi; }));
    OpRegistry::instance().register_op_hp_monolithic(
        "host_adapter_test", "offset_identity",
        MonolithicOpFunc([](const Node& node,
                            const std::vector<const NodeOutput*>& inputs) {
          if (inputs.empty() || inputs.front() == nullptr) {
            throw GraphError(GraphErrc::InvalidParameter,
                             "host adapter offset_identity requires one input");
          }
          const NodeOutput& input = *inputs.front();
          NodeOutput output;
          output.image_buffer = make_aligned_cpu_image_buffer(
              input.image_buffer.width, input.image_buffer.height,
              input.image_buffer.channels, input.image_buffer.type);
          toCvMat(input.image_buffer).copyTo(toCvMat(output.image_buffer));
          output.space.absolute_roi = input.space.absolute_roi;
          output.debug.compute_device = "host-adapter-offset-identity-test";
          (void)node;
          return output;
        }));
    OpRegistry::instance().register_dirty_propagator(
        "host_adapter_test", "offset_identity",
        DirtyRoiPropFunc(
            [](const Node&, const cv::Rect& roi, const GraphModel&) {
              return cv::Rect(roi.x + 64, roi.y, roi.width, roi.height);
            }));
  });
}

/**
 * @brief Owns a unique temporary directory for one Host adapter test.
 *
 * @throws std::filesystem::filesystem_error if setup cleanup or directory
 *         creation fails.
 * @note The destructor uses an error_code cleanup path so test assertions are
 *       not masked by best-effort removal failures.
 */
class ScopedTempDir {
 public:
  /**
   * @brief Creates an empty unique temporary directory.
   *
   * @param name Directory name below the platform temporary directory.
   * @throws std::filesystem::filesystem_error if directory creation fails.
   */
  explicit ScopedTempDir(const std::string& name)
      : root_(std::filesystem::temp_directory_path() /
              (name + "_" +
               std::to_string(std::chrono::high_resolution_clock::now()
                                  .time_since_epoch()
                                  .count()))) {
    std::filesystem::remove_all(root_);
    std::filesystem::create_directories(root_);
  }

  /**
   * @brief Prevents two owners from deleting the same temporary directory.
   * @param other Owner that retains cleanup responsibility.
   * @throws Nothing because construction is unavailable.
   */
  ScopedTempDir(const ScopedTempDir& other) = delete;

  /**
   * @brief Prevents replacing temporary-directory cleanup ownership.
   * @param other Owner whose root remains unchanged.
   * @return No value because assignment is unavailable.
   * @throws Nothing because assignment is unavailable.
   */
  ScopedTempDir& operator=(const ScopedTempDir& other) = delete;

  /**
   * @brief Removes the temporary directory.
   *
   * @throws Nothing.
   * @note Cleanup is best-effort so it cannot hide a test failure.
   */
  ~ScopedTempDir() {
    std::error_code ec;
    std::filesystem::remove_all(root_, ec);
  }

  /**
   * @brief Returns the root path for the temporary directory.
   *
   * @return Temporary root path.
   * @throws Nothing.
   */
  const std::filesystem::path& root() const noexcept { return root_; }

 private:
  /** @brief Temporary directory root owned by this helper. */
  std::filesystem::path root_;
};

/**
 * @brief Temporarily sets one scheduler-fixture environment value.
 *
 * @throws std::bad_alloc if the key or previous value cannot be copied.
 * @throws std::runtime_error if the platform environment update fails.
 * @note Tests using this helper are process-serial because environment values
 *       are global. Destruction restores the exact prior value best-effort.
 */
class ScopedEnvironmentValue final {
 public:
  /**
   * @brief Saves the current value and installs one fixture selection.
   * @param name Environment key copied for this guard's lifetime.
   * @param value New value visible to the scheduler plugin.
   * @throws std::bad_alloc if owned strings cannot be allocated.
   * @throws std::runtime_error if the environment cannot be updated.
   */
  ScopedEnvironmentValue(const char* name, const std::string& value)
      : name_(name) {
    if (const char* previous = std::getenv(name)) {
      previous_ = std::string(previous);
    }
    set(value);
  }

  /**
   * @brief Restores the saved environment state without hiding test failures.
   * @throws Nothing; platform restoration failures are suppressed.
   */
  ~ScopedEnvironmentValue() noexcept {
    try {
      if (previous_) {
        set(*previous_);
      } else {
        clear();
      }
    } catch (...) {
    }
  }

  /**
   * @brief Prevents duplicate restoration ownership.
   * @param other Guard that remains the sole restoration owner.
   * @throws Nothing because construction is unavailable.
   */
  ScopedEnvironmentValue(const ScopedEnvironmentValue& other) = delete;

  /**
   * @brief Prevents replacing one active environment guard.
   * @param other Guard whose environment key remains unchanged.
   * @return No value because assignment is unavailable.
   * @throws Nothing because assignment is unavailable.
   */
  ScopedEnvironmentValue& operator=(const ScopedEnvironmentValue& other) =
      delete;

 private:
  /**
   * @brief Installs a new value for the owned key.
   * @param value Value to publish process-wide.
   * @return Nothing.
   * @throws std::runtime_error if the platform call fails.
   */
  void set(const std::string& value) {
#if defined(_WIN32)
    if (_putenv_s(name_.c_str(), value.c_str()) != 0) {
      throw std::runtime_error("_putenv_s failed");
    }
#else
    if (setenv(name_.c_str(), value.c_str(), 1) != 0) {
      throw std::runtime_error("setenv failed");
    }
#endif
  }

  /**
   * @brief Removes the owned environment key.
   * @return Nothing.
   * @throws std::runtime_error if the platform call fails.
   */
  void clear() {
#if defined(_WIN32)
    if (_putenv_s(name_.c_str(), "") != 0) {
      throw std::runtime_error("_putenv_s clear failed");
    }
#else
    if (unsetenv(name_.c_str()) != 0) {
      throw std::runtime_error("unsetenv failed");
    }
#endif
  }

  /** @brief Environment key retained through restoration. */
  std::string name_;
  /** @brief Previous value, or nullopt when the key was absent. */
  std::optional<std::string> previous_;
};

/**
 * @brief Writes a single-node Host adapter test graph.
 *
 * @param path YAML file path to create.
 * @throws std::filesystem::filesystem_error or std::ios_base::failure if file
 *         creation fails.
 * @note The graph has one ending node so traversal, dependency-tree, compute,
 *       and image-returning Host APIs all observe the same deterministic node.
 */
void write_host_adapter_graph(const std::filesystem::path& path, int width = 6,
                              int height = 4,
                              const std::string& subtype = "source",
                              int slow_sleep_ms = 75) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream out(path);
  out << "- id: 1\n"
      << "  name: host_source\n"
      << "  type: host_adapter_test\n"
      << "  subtype: " << subtype << "\n"
      << "  parameters:\n"
      << "    width: " << width << "\n"
      << "    height: " << height << "\n";
  if (subtype == "slow_source") {
    out << "    sleep_ms: " << slow_sleep_ms << "\n";
  }
  if (subtype == "resized_extent") {
    out << "    roi_width: 12\n"
        << "    roi_height: 9\n";
  }
}

/**
 * @brief Writes a graph whose node has no registered operation.
 *
 * @param path YAML file path to create.
 * @throws std::filesystem::filesystem_error or std::ios_base::failure if file
 *         creation fails.
 * @note Loading succeeds because operation lookup is deferred to compute, so
 *       Host image-compute failure mapping can verify GraphErrc::NoOperation.
 */
void write_host_adapter_unregistered_op_graph(
    const std::filesystem::path& path) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream out(path);
  out << "- id: 1\n"
      << "  name: missing_op_source\n"
      << "  type: host_adapter_missing\n"
      << "  subtype: source\n"
      << "  parameters:\n"
      << "    width: 6\n"
      << "    height: 4\n";
}

/**
 * @brief Writes a two-node graph with identity ROI propagation.
 *
 * @param path YAML file path to create.
 * @throws std::filesystem::filesystem_error or std::ios_base::failure if file
 *         creation fails.
 * @note Node 2 depends on node 1 through an image input and uses an explicit
 *       identity propagator, giving Host ROI tests deterministic forward and
 *       backward rectangles.
 */
void write_host_adapter_roi_graph(const std::filesystem::path& path) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream out(path);
  out << "- id: 1\n"
      << "  name: roi_source\n"
      << "  type: host_adapter_test\n"
      << "  subtype: source\n"
      << "  parameters:\n"
      << "    width: 8\n"
      << "    height: 6\n"
      << "- id: 2\n"
      << "  name: roi_identity\n"
      << "  type: host_adapter_test\n"
      << "  subtype: identity\n"
      << "  image_inputs:\n"
      << "    - from_node_id: 1\n";
}

/**
 * @brief Writes a two-node graph whose backward dirty ROI differs by edge side.
 *
 * @param path YAML file path to create.
 * @throws std::filesystem::filesystem_error or std::ios_base::failure if file
 *         creation fails.
 * @note Node 2 uses a deterministic test-only dirty propagator that shifts the
 *       upstream demand by one HP micro-tile, so Host conversion tests can
 *       catch accidental swaps of `from_roi` and `to_roi`.
 */
void write_host_adapter_offset_roi_graph(const std::filesystem::path& path) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream out(path);
  out << "- id: 1\n"
      << "  name: roi_source\n"
      << "  type: host_adapter_test\n"
      << "  subtype: source\n"
      << "  parameters:\n"
      << "    width: 256\n"
      << "    height: 128\n"
      << "- id: 2\n"
      << "  name: roi_offset_identity\n"
      << "  type: host_adapter_test\n"
      << "  subtype: offset_identity\n"
      << "  image_inputs:\n"
      << "    - from_node_id: 1\n";
}

/**
 * @brief Returns the lifecycle operation plugin fixture directory.
 *
 * @return Directory containing the platform-specific lifecycle plugin library.
 * @throws std::bad_alloc if path construction allocates and fails.
 * @note CMake injects `PS_TEST_OP_PLUGIN_DIR` so the path follows the active
 *       binary directory instead of assuming a source-relative `build/` tree.
 */
std::filesystem::path lifecycle_plugin_dir() {
  return std::filesystem::path(PS_TEST_OP_PLUGIN_DIR) / "lifecycle";
}

/**
 * @brief Returns the replacement lifecycle plugin fixture directory.
 *
 * @return Directory containing the platform-specific override plugin library.
 * @throws std::bad_alloc if path construction allocates and fails.
 * @note The fixture replaces the same canonical operation key as the lifecycle
 *       plugin so multiple Host instances can exercise one restoration chain.
 */
std::filesystem::path override_lifecycle_plugin_dir() {
  return std::filesystem::path(PS_TEST_OP_PLUGIN_DIR) / "override";
}

/**
 * @brief Returns the deterministic scheduler lifecycle fixture library path.
 *
 * @return Platform-specific path below the CMake test scheduler directory.
 * @throws std::bad_alloc if path or filename construction cannot allocate.
 * @note The existing fixture can throw from real scheduler shutdown while
 *       retaining running state, allowing a later close retry to prove that
 *       shutdown is attempted again.
 */
std::filesystem::path destroy_count_scheduler_plugin_path() {
  return std::filesystem::path(PS_TEST_SCHEDULER_PLUGIN_PATH);
}

/**
 * @brief Owns fixture lifecycle exports resolved from the exact plugin path.
 *
 * @throws std::runtime_error when the library or a required export cannot be
 *         opened.
 * @note The diagnostic handle remains open while the Host loader owns its own
 *       mapping, then closes independently after all counter reads finish.
 */
class SchedulerFixtureExports final {
 public:
  /**
   * @brief Opens one scheduler fixture and resolves reset/shutdown counters.
   * @param path Complete platform-specific library path injected by CMake.
   * @throws std::runtime_error when opening or symbol lookup fails.
   */
  explicit SchedulerFixtureExports(const std::filesystem::path& path) {
#if defined(_WIN32)
    handle_ = LoadLibrary(path.string().c_str());
    if (handle_ != nullptr) {
      reset_counts_ = reinterpret_cast<void (*)()>(
          GetProcAddress(handle_, "ps_test_scheduler_reset_counts"));
      shutdown_count_ = reinterpret_cast<int (*)()>(
          GetProcAddress(handle_, "ps_test_scheduler_shutdown_count"));
    }
#else
    handle_ = dlopen(path.string().c_str(), RTLD_LAZY);
    if (handle_ != nullptr) {
      reset_counts_ = reinterpret_cast<void (*)()>(
          dlsym(handle_, "ps_test_scheduler_reset_counts"));
      shutdown_count_ = reinterpret_cast<int (*)()>(
          dlsym(handle_, "ps_test_scheduler_shutdown_count"));
    }
#endif
    if (handle_ == nullptr || reset_counts_ == nullptr ||
        shutdown_count_ == nullptr) {
      close();
      throw std::runtime_error(
          "failed to resolve scheduler lifecycle fixture exports: " +
          path.string());
    }
  }

  /** @brief Closes the diagnostic library handle. @throws Nothing. */
  ~SchedulerFixtureExports() noexcept { close(); }

  /**
   * @brief Prevents two owners from closing the same diagnostic handle.
   * @param other Owner that retains the native library handle.
   * @throws Nothing because construction is unavailable.
   */
  SchedulerFixtureExports(const SchedulerFixtureExports& other) = delete;

  /**
   * @brief Prevents replacing diagnostic-handle ownership.
   * @param other Owner whose handle remains unchanged.
   * @return No value because assignment is unavailable.
   * @throws Nothing because assignment is unavailable.
   */
  SchedulerFixtureExports& operator=(const SchedulerFixtureExports& other) =
      delete;

  /**
   * @brief Resets fixture counters while no scheduler instance is active.
   * @return Nothing.
   * @throws Nothing.
   */
  void reset_counts() const noexcept { reset_counts_(); }

  /**
   * @brief Returns the number of explicit scheduler shutdown calls.
   * @return Current fixture shutdown count.
   * @throws Nothing.
   */
  int shutdown_count() const noexcept { return shutdown_count_(); }

 private:
  /** @brief Releases the native diagnostic handle when present. */
  void close() noexcept {
#if defined(_WIN32)
    if (handle_ != nullptr) {
      FreeLibrary(handle_);
      handle_ = nullptr;
    }
#else
    if (handle_ != nullptr) {
      dlclose(handle_);
      handle_ = nullptr;
    }
#endif
  }

#if defined(_WIN32)
  /** @brief Native Windows dynamic-library handle. */
  HMODULE handle_ = nullptr;
#else
  /** @brief Native POSIX dynamic-library handle. */
  void* handle_ = nullptr;
#endif
  /** @brief Fixture counter reset export. */
  void (*reset_counts_)() = nullptr;
  /** @brief Fixture shutdown counter export. */
  int (*shutdown_count_)() = nullptr;
};

/**
 * @brief Clears process-global scheduler plugins on every test exit.
 *
 * @throws Nothing.
 * @note Declare this guard before the Host owner. Reverse destruction then
 *       destroys Host graph runtimes first and clears loader mappings second.
 */
class ScopedSchedulerPluginCleanup final {
 public:
  /**
   * @brief Clears stale scheduler plugin state before a fixture test begins.
   * @throws Nothing; cleanup failures are suppressed for assertion safety.
   */
  ScopedSchedulerPluginCleanup() noexcept { clear(); }

  /** @brief Clears scheduler state after later-declared Host destruction. */
  ~ScopedSchedulerPluginCleanup() noexcept { clear(); }

 private:
  /** @brief Clears plugin mappings and diagnostics behind a no-throw fence. */
  static void clear() noexcept {
    try {
      SchedulerPluginLoader::instance().clear_plugins();
      SchedulerPluginLoader::instance().clear_errors();
    } catch (...) {
    }
  }

 public:
  /**
   * @brief Prevents duplicate process-global cleanup ownership.
   * @param other Guard that remains responsible for cleanup.
   * @throws Nothing because construction is unavailable.
   */
  ScopedSchedulerPluginCleanup(const ScopedSchedulerPluginCleanup& other) =
      delete;

  /**
   * @brief Prevents replacing process-global cleanup ownership.
   * @param other Guard whose cleanup responsibility remains unchanged.
   * @return No value because assignment is unavailable.
   * @throws Nothing because assignment is unavailable.
   */
  ScopedSchedulerPluginCleanup& operator=(
      const ScopedSchedulerPluginCleanup& other) = delete;
};

/**
 * @brief Writes a graph backed by the dynamically loaded lifecycle operation.
 *
 * @param path YAML file path to create.
 * @return Nothing.
 * @throws std::filesystem::filesystem_error if parent directory creation fails.
 * @throws std::ios_base::failure if opening or writing the graph file fails.
 * @note The operation returns debug metadata without requiring image inputs, so
 *       Host inspection can distinguish original and replacement callbacks.
 */
void write_lifecycle_plugin_graph(const std::filesystem::path& path) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream out;
  out.exceptions(std::ios::failbit | std::ios::badbit);
  out.open(path);
  out << "- id: 1\n"
      << "  name: lifecycle_plugin_node\n"
      << "  type: plugin_lifecycle\n"
      << "  subtype: op\n";
}

/**
 * @brief Cleans process-global operation plugins through one public Host.
 *
 * Construction removes stale plugins and converts a non-OK public status into
 * `std::runtime_error`. Destruction retries global cleanup but suppresses all
 * exceptions so assertion unwinding remains visible.
 *
 * @throws std::bad_alloc when construction cannot copy Host status storage.
 * @throws std::runtime_error when initial public cleanup reports failure.
 * @note The referenced Host must outlive this guard. Cleanup deliberately uses
 *       the public global-unload surface exercised by the test; every Host sees
 *       the same process-owner state.
 */
class ScopedHostPluginCleanup final {
 public:
  /**
   * @brief Removes stale process plugins before a multi-Host scenario.
   *
   * @param host Long-lived Host used for cleanup.
   * @throws std::bad_alloc if the Host boundary cannot construct a status.
   * @throws std::runtime_error if the public cleanup status is not OK; the
   *         exception copies that status message for test diagnostics.
   * @note The borrowed Host is retained by reference and must remain alive
   * until this guard's destructor has finished.
   */
  explicit ScopedHostPluginCleanup(Host& host) : host_(host) {
    const auto cleanup = host_.plugins_unload_all();
    if (!cleanup.status.ok) {
      throw std::runtime_error(cleanup.status.message);
    }
  }

  /**
   * @brief Performs best-effort public global cleanup after assertions.
   *
   * @throws Nothing; Host exceptions are caught and suppressed.
   * @note Cleanup runs while `host_` is still alive and does not replace an
   *       exception already unwinding from the test body.
   */
  ~ScopedHostPluginCleanup() noexcept {
    try {
      (void)host_.plugins_unload_all();
    } catch (...) {
      // Test teardown must not hide the assertion that triggered unwinding.
    }
  }

  /**
   * @brief Prevents duplicating cleanup ownership for one borrowed Host.
   *
   * @param other Guard that remains the sole cleanup owner.
   * @note Deletion prevents two destructors from racing global cleanup.
   */
  ScopedHostPluginCleanup(const ScopedHostPluginCleanup& other) = delete;

  /**
   * @brief Prevents retargeting an active cleanup guard.
   *
   * @param other Guard whose borrowed Host must remain unchanged.
   * @return No value because this operation is deleted.
   * @note Lexical lifetime remains paired with the Host supplied at
   *       construction.
   */
  ScopedHostPluginCleanup& operator=(const ScopedHostPluginCleanup& other) =
      delete;

  /**
   * @brief Prevents transferring cleanup ownership away from its lexical Host.
   *
   * @param other Guard that remains paired with its borrowed Host.
   * @note Deletion keeps one deterministic destructor cleanup point.
   */
  ScopedHostPluginCleanup(ScopedHostPluginCleanup&& other) = delete;

  /**
   * @brief Prevents replacing cleanup ownership through move assignment.
   *
   * @param other Guard whose borrowed Host remains unchanged.
   * @return No value because this operation is deleted.
   * @note Neither guard can become responsible for a different Host.
   */
  ScopedHostPluginCleanup& operator=(ScopedHostPluginCleanup&& other) = delete;

 private:
  /**
   * @brief Public Host borrowed for initial and final process-global cleanup.
   * @note The surrounding test owns the Host and keeps it alive past this
   * guard.
   */
  Host& host_;
};

/**
 * @brief Returns YAML text for replacing the single Host test node.
 *
 * @param name Replacement node name.
 * @param width Replacement width parameter.
 * @param height Replacement height parameter.
 * @return YAML text accepted by set_node_yaml().
 * @throws std::bad_alloc if string construction allocates and fails.
 * @note The backend preserves the target node id supplied separately.
 */
std::string replacement_node_yaml(const std::string& name, int width,
                                  int height) {
  std::ostringstream out;
  out << R"YAML(
id: 1
name: )YAML"
      << name << R"YAML(
type: host_adapter_test
subtype: source
parameters:
  width: )YAML"
      << width << R"YAML(
  height: )YAML"
      << height << "\n";
  return out.str();
}

/**
 * @brief Loads a deterministic Host adapter graph.
 *
 * @param host Host under test.
 * @param root Temporary root containing source and session folders.
 * @param session Session label to load.
 * @param subtype Operation subtype to write into the graph YAML.
 * @param slow_sleep_ms Milliseconds used by the slow_source fixture op.
 * @return Loaded session id.
 * @throws std::bad_alloc if path or diagnostic strings allocate and fail.
 * @note Test assertions fail immediately if loading is rejected.
 */
GraphSessionId load_test_graph(Host& host, const std::filesystem::path& root,
                               const std::string& session,
                               const std::string& subtype = "source",
                               int slow_sleep_ms = 75) {
  GraphLoadRequest request;
  request.session = GraphSessionId{session};
  request.root_dir = (root / "sessions").string();
  request.yaml_path = (root / "source" / (session + ".yaml")).string();
  request.cache_root_dir = (root / "cache").string();
  write_host_adapter_graph(request.yaml_path, 6, 4, subtype, slow_sleep_ms);
  auto loaded = host.load_graph(request);
  EXPECT_TRUE(loaded.status.ok) << loaded.status.message;
  EXPECT_EQ(loaded.value.value, session);
  return loaded.value;
}

/**
 * @brief Builds the graph load request used by Host adapter tests.
 *
 * @param root Temporary root containing source, sessions, and cache folders.
 * @return GraphLoadRequest pointing at a deterministic single-node graph.
 * @throws std::bad_alloc if path string conversion allocates and fails.
 * @note The request exercises the embedded adapter's copy/load path by
 *       providing an explicit YAML source file.
 */
GraphLoadRequest make_load_request(const std::filesystem::path& root) {
  const auto yaml_path = root / "source" / "host_graph.yaml";
  write_host_adapter_graph(yaml_path);

  GraphLoadRequest request;
  request.session = GraphSessionId{"host_adapter_graph"};
  request.root_dir = (root / "sessions").string();
  request.yaml_path = yaml_path.string();
  request.cache_root_dir = (root / "cache").string();
  return request;
}

/**
 * @brief Builds a compute request for the Host adapter test graph.
 *
 * @param session Session to compute.
 * @return HostComputeRequest for node 1 with timing enabled.
 * @throws std::bad_alloc if precision string allocation fails.
 */
HostComputeRequest make_compute_request(const GraphSessionId& session) {
  HostComputeRequest request;
  request.session = session;
  request.node = NodeId{1};
  request.cache.precision = "fp32";
  request.telemetry.enable_timing = true;
  return request;
}

/**
 * @brief Reports whether a string vector contains a value.
 *
 * @param values Values to search.
 * @param needle String to find.
 * @return True when needle is present.
 * @throws Nothing directly.
 */
bool contains_string(const std::vector<std::string>& values,
                     const std::string& needle) {
  return std::find(values.begin(), values.end(), needle) != values.end();
}

TEST(EmbeddedHostAdapter,
     CoversInteractionCoreWithPublicSnapshotsAndNoKernelExposure) {
  register_host_adapter_ops();
  ScopedTempDir temp("photospider_host_adapter_test");
  auto host = create_embedded_host();
  ASSERT_NE(host, nullptr);

  const GraphLoadRequest load_request = make_load_request(temp.root());
  const GraphSessionId session = load_request.session;
  auto loaded = host->load_graph(load_request);
  ASSERT_TRUE(loaded.status.ok) << loaded.status.message;
  EXPECT_EQ(loaded.value.value, session.value);

  auto graphs = host->list_graphs();
  ASSERT_TRUE(graphs.status.ok) << graphs.status.message;
  ASSERT_EQ(graphs.value.size(), 1u);
  EXPECT_EQ(graphs.value.front().value, session.value);

  auto ids = host->list_node_ids(session);
  ASSERT_TRUE(ids.status.ok) << ids.status.message;
  ASSERT_EQ(ids.value.size(), 1u);
  EXPECT_EQ(ids.value.front().value, 1);

  auto ending = host->ending_nodes(session);
  ASSERT_TRUE(ending.status.ok) << ending.status.message;
  ASSERT_EQ(ending.value.size(), 1u);
  EXPECT_EQ(ending.value.front().value, 1);

  auto graph_view = host->inspect_graph(session);
  ASSERT_TRUE(graph_view.status.ok) << graph_view.status.message;
  ASSERT_EQ(graph_view.value.nodes.size(), 1u);
  EXPECT_EQ(graph_view.value.session.value, session.value);
  EXPECT_EQ(graph_view.value.nodes.front().name, "host_source");
  EXPECT_EQ(graph_view.value.nodes.front().parameters.at("width"), "6");

  auto node_view = host->inspect_node(session, NodeId{1});
  ASSERT_TRUE(node_view.status.ok) << node_view.status.message;
  EXPECT_EQ(node_view.value.type, "host_adapter_test");
  EXPECT_EQ(node_view.value.subtype, "source");

  auto tree = host->dependency_tree(session, std::nullopt, true);
  ASSERT_TRUE(tree.status.ok) << tree.status.message;
  EXPECT_EQ(tree.value.scope, HostDependencyTreeScope::EndingNodes);
  ASSERT_EQ(tree.value.entries.size(), 1u);
  EXPECT_EQ(tree.value.entries.front().node.id.value, 1);

  auto traversal = host->traversal_orders(session);
  ASSERT_TRUE(traversal.status.ok) << traversal.status.message;
  ASSERT_EQ(traversal.value.size(), 1u);
  ASSERT_EQ(traversal.value.at(1).size(), 1u);
  EXPECT_EQ(traversal.value.at(1).front().value, 1);

  const HostComputeRequest compute_request = make_compute_request(session);
  auto compute_status = host->compute(compute_request);
  ASSERT_TRUE(compute_status.status.ok) << compute_status.status.message;

  auto image = host->compute_and_get_image(compute_request);
  ASSERT_TRUE(image.status.ok) << image.status.message;
  EXPECT_EQ(image.value.width, 6);
  EXPECT_EQ(image.value.height, 4);
  EXPECT_EQ(image.value.channels, 1);
  EXPECT_EQ(image.value.device, Device::CPU);
  ASSERT_NE(image.value.data, nullptr);

  auto async_compute = host->compute_async(compute_request);
  ASSERT_TRUE(async_compute.status.ok) << async_compute.status.message;
  OperationStatus async_status = async_compute.value.get();
  EXPECT_TRUE(async_status.ok) << async_status.message;

  auto timing = host->timing(session);
  ASSERT_TRUE(timing.status.ok) << timing.status.message;
  EXPECT_FALSE(timing.value.node_timings.empty());

  auto io_time = host->last_io_time(session);
  ASSERT_TRUE(io_time.status.ok) << io_time.status.message;
  EXPECT_GE(io_time.value, 0.0);

  auto invalid_event_limit = host->drain_compute_events(session, 0);
  EXPECT_FALSE(invalid_event_limit.status.ok);
  EXPECT_EQ(checked_graph_error_code(invalid_event_limit.status),
            GraphErrc::InvalidParameter);

  auto missing_events = host->drain_compute_events(
      GraphSessionId{"missing-event-session"}, kComputeEventDrainMaxLimit);
  EXPECT_FALSE(missing_events.status.ok);
  EXPECT_EQ(missing_events.status.domain, OperationErrorDomain::Graph);
  EXPECT_EQ(checked_graph_error_code(missing_events.status),
            GraphErrc::NotFound);

  auto events = host->drain_compute_events(session, kComputeEventDrainMaxLimit);
  ASSERT_TRUE(events.status.ok) << events.status.message;
  ASSERT_FALSE(events.value.events.empty());
  EXPECT_GT(events.value.events.front().sequence, 0u);
  EXPECT_LT(events.value.events.back().sequence, kObservationSequenceExhausted);

  auto dirty = host->dirty_region_snapshot(session);
  ASSERT_TRUE(dirty.status.ok) << dirty.status.message;

  auto scheduler_types = host->scheduler_available_types();
  ASSERT_TRUE(scheduler_types.status.ok) << scheduler_types.status.message;
  EXPECT_TRUE(contains_string(scheduler_types.value, "serial_debug"));

  auto replaced = host->replace_scheduler(
      session, ComputeIntent::GlobalHighPrecision, "serial_debug");
  ASSERT_TRUE(replaced.status.ok) << replaced.status.message;

  auto scheduler_info =
      host->scheduler_info(session, ComputeIntent::GlobalHighPrecision);
  ASSERT_TRUE(scheduler_info.status.ok) << scheduler_info.status.message;
  EXPECT_EQ(scheduler_info.value.scheduler_name, "serial_debug");
  EXPECT_NE(scheduler_info.value.stats.find("SerialDebugScheduler"),
            std::string::npos);

  HostComputeRequest trace_request = make_compute_request(session);
  trace_request.cache.force_recache = true;
  trace_request.execution.parallel = true;
  trace_request.telemetry.enable_timing = false;
  auto trace_compute = host->compute(trace_request);
  ASSERT_TRUE(trace_compute.status.ok) << trace_compute.status.message;

  auto invalid_trace_limit = host->scheduler_trace(session, 0, 0);
  EXPECT_FALSE(invalid_trace_limit.status.ok);
  EXPECT_EQ(checked_graph_error_code(invalid_trace_limit.status),
            GraphErrc::InvalidParameter);

  auto missing_trace = host->scheduler_trace(
      GraphSessionId{"missing-trace-session"}, 0, kSchedulerTraceMaxLimit);
  EXPECT_FALSE(missing_trace.status.ok);
  EXPECT_EQ(missing_trace.status.domain, OperationErrorDomain::Graph);
  EXPECT_EQ(checked_graph_error_code(missing_trace.status),
            GraphErrc::NotFound);

  auto scheduler_trace =
      host->scheduler_trace(session, 0, kSchedulerTraceMaxLimit);
  ASSERT_TRUE(scheduler_trace.status.ok) << scheduler_trace.status.message;
  ASSERT_FALSE(scheduler_trace.value.events.empty());
  EXPECT_GT(scheduler_trace.value.events.front().sequence, 0u);
  EXPECT_LT(scheduler_trace.value.events.back().sequence,
            kObservationSequenceExhausted);

  auto repeated_scheduler_trace =
      host->scheduler_trace(session, 0, kSchedulerTraceMaxLimit);
  ASSERT_TRUE(repeated_scheduler_trace.status.ok)
      << repeated_scheduler_trace.status.message;
  ASSERT_EQ(repeated_scheduler_trace.value.events.size(),
            scheduler_trace.value.events.size());
  EXPECT_EQ(repeated_scheduler_trace.value.events.front().sequence,
            scheduler_trace.value.events.front().sequence);

  auto future_trace = host->scheduler_trace(
      session, kObservationSequenceExhausted - 1, kSchedulerTraceMaxLimit);
  EXPECT_FALSE(future_trace.status.ok);
  EXPECT_EQ(future_trace.status.domain, OperationErrorDomain::Graph);
  EXPECT_EQ(checked_graph_error_code(future_trace.status),
            GraphErrc::InvalidParameter);

  auto premature_terminal_trace = host->scheduler_trace(
      session, kObservationSequenceExhausted, kSchedulerTraceMaxLimit);
  EXPECT_FALSE(premature_terminal_trace.status.ok);
  EXPECT_EQ(premature_terminal_trace.status.domain,
            OperationErrorDomain::Graph);
  EXPECT_EQ(checked_graph_error_code(premature_terminal_trace.status),
            GraphErrc::InvalidParameter);

  auto description = host->scheduler_description("serial_debug");
  ASSERT_TRUE(description.status.ok) << description.status.message;
  EXPECT_NE(description.value.find("Single-threaded"), std::string::npos);

  auto missing_description =
      host->scheduler_description("missing_scheduler_type");
  EXPECT_FALSE(missing_description.status.ok);
  EXPECT_EQ(missing_description.status.domain, OperationErrorDomain::Graph);
  EXPECT_EQ(checked_graph_error_code(missing_description.status),
            GraphErrc::NotFound);
  EXPECT_EQ(missing_description.status.name, "not_found");

  auto plugins = host->plugins_load_report({});
  ASSERT_TRUE(plugins.status.ok) << plugins.status.message;
  EXPECT_EQ(plugins.value.loaded, 0);

  auto seed = host->seed_builtin_ops();
  ASSERT_TRUE(seed.status.ok) << seed.status.message;

  auto op_sources = host->ops_combined_sources();
  ASSERT_TRUE(op_sources.status.ok) << op_sources.status.message;
  EXPECT_EQ(op_sources.value.at("host_adapter_test:source"), "built-in");

  auto yaml = host->get_node_yaml(session, NodeId{1});
  ASSERT_TRUE(yaml.status.ok) << yaml.status.message;
  EXPECT_NE(yaml.value.find("host_source"), std::string::npos);

  auto clear_memory = host->clear_memory_cache(session);
  ASSERT_TRUE(clear_memory.status.ok) << clear_memory.status.message;

  auto close = host->close_graph(session);
  EXPECT_TRUE(close.status.ok) << close.status.message;
}

/**
 * @brief Verifies scheduler shutdown failure preserves a retryable Host
 * session.
 *
 * @throws Nothing when the fixture, Host status mapping, and cleanup behave as
 *         specified; GoogleTest records any mismatch.
 * @note The first close throws while the fixture remains running. The Host must
 *       return Unknown, reopen admission, retain the graph, and invoke shutdown
 *       again when closing the same session after injection is removed.
 */
TEST(EmbeddedHostAdapter, CloseShutdownFailureRetainsSessionAndAllowsRetry) {
  register_host_adapter_ops();
  ScopedTempDir temp("photospider_host_adapter_close_failure_test");
  ScopedSchedulerPluginCleanup scheduler_cleanup;
  auto host = create_embedded_host();
  ASSERT_NE(host, nullptr);

  const std::filesystem::path plugin_path =
      destroy_count_scheduler_plugin_path();
  ASSERT_TRUE(std::filesystem::exists(plugin_path))
      << "scheduler close-failure fixture was not built: " << plugin_path;
  SchedulerFixtureExports fixture(plugin_path);
  fixture.reset_counts();
  const VoidResult plugin_load = host->scheduler_load(plugin_path.string());
  ASSERT_TRUE(plugin_load.status.ok) << plugin_load.status.message;

  HostSchedulerConfig scheduler_config;
  scheduler_config.hp_type = kDestroyCountSchedulerType;
  scheduler_config.rt_type = "serial_debug";
  const VoidResult configured =
      host->configure_scheduler_defaults(scheduler_config);
  ASSERT_TRUE(configured.status.ok) << configured.status.message;

  const GraphSessionId session =
      load_test_graph(*host, temp.root(), "close_failure_retry_graph");
  HostComputeRequest stale_error_request = make_compute_request(session);
  stale_error_request.node = NodeId{99};
  const Result<ImageBuffer> stale_compute =
      host->compute_and_get_image(stale_error_request);
  ASSERT_FALSE(stale_compute.status.ok);
  const OperationStatus last_error_before_failure = host->last_error(session);
  ASSERT_FALSE(last_error_before_failure.ok);

  {
    ScopedEnvironmentValue failure(kSchedulerFailureEnvironment,
                                   "shutdown_runtime_error");
    const VoidResult failed_close = host->close_graph(session);
    EXPECT_FALSE(failed_close.status.ok);
    EXPECT_EQ(failed_close.status.domain, OperationErrorDomain::Graph);
    EXPECT_EQ(checked_graph_error_code(failed_close.status),
              GraphErrc::Unknown);
    EXPECT_EQ(failed_close.status.name, "unknown");
    EXPECT_NE(failed_close.status.message.find("fixture shutdown failure"),
              std::string::npos);
    EXPECT_EQ(fixture.shutdown_count(), 1);
  }

  const Result<std::vector<GraphSessionId>> after_failure = host->list_graphs();
  ASSERT_TRUE(after_failure.status.ok) << after_failure.status.message;
  ASSERT_EQ(after_failure.value.size(), 1u);
  EXPECT_EQ(after_failure.value.front().value, session.value);

  const Result<SchedulerInfoSnapshot> admitted_after_failure =
      host->scheduler_info(session, ComputeIntent::GlobalHighPrecision);
  ASSERT_TRUE(admitted_after_failure.status.ok)
      << admitted_after_failure.status.message;
  EXPECT_EQ(admitted_after_failure.value.scheduler_name,
            kDestroyCountSchedulerType);
  const OperationStatus last_error_after_failure = host->last_error(session);
  EXPECT_EQ(last_error_after_failure.ok, last_error_before_failure.ok);
  EXPECT_EQ(last_error_after_failure.domain, last_error_before_failure.domain);
  EXPECT_EQ(last_error_after_failure.code, last_error_before_failure.code);
  EXPECT_EQ(last_error_after_failure.name, last_error_before_failure.name);
  EXPECT_EQ(last_error_after_failure.message,
            last_error_before_failure.message);

  const VoidResult retry_close = host->close_graph(session);
  EXPECT_TRUE(retry_close.status.ok) << retry_close.status.message;
  EXPECT_EQ(fixture.shutdown_count(), 2);

  const Result<std::vector<GraphSessionId>> after_retry = host->list_graphs();
  ASSERT_TRUE(after_retry.status.ok) << after_retry.status.message;
  EXPECT_TRUE(after_retry.value.empty());
  const OperationStatus last_error_after_retry = host->last_error(session);
  EXPECT_TRUE(last_error_after_retry.ok) << last_error_after_retry.message;

  const VoidResult missing_close = host->close_graph(session);
  EXPECT_FALSE(missing_close.status.ok);
  EXPECT_EQ(checked_graph_error_code(missing_close.status),
            GraphErrc::NotFound);
}

/**
 * @brief Verifies a shutdown GraphError::NotFound cannot masquerade as absence.
 *
 * @throws Nothing when close remapping, graph retention, and retry hold;
 *         GoogleTest records any mismatch.
 * @note Only Kernel's explicit false result denotes an absent graph. A
 * scheduler GraphError::NotFound is remapped to Unknown and keeps the session
 * loaded.
 */
TEST(EmbeddedHostAdapter,
     CloseShutdownGraphNotFoundMapsUnknownAndRetainsGraph) {
  register_host_adapter_ops();
  ScopedTempDir temp("photospider_host_adapter_close_graph_error_test");
  ScopedSchedulerPluginCleanup scheduler_cleanup;
  auto host = create_embedded_host();
  ASSERT_NE(host, nullptr);

  const std::filesystem::path plugin_path =
      destroy_count_scheduler_plugin_path();
  ASSERT_TRUE(std::filesystem::exists(plugin_path))
      << "scheduler close-failure fixture was not built: " << plugin_path;
  SchedulerFixtureExports fixture(plugin_path);
  fixture.reset_counts();
  const VoidResult plugin_load = host->scheduler_load(plugin_path.string());
  ASSERT_TRUE(plugin_load.status.ok) << plugin_load.status.message;

  HostSchedulerConfig scheduler_config;
  scheduler_config.hp_type = kDestroyCountSchedulerType;
  scheduler_config.rt_type = "serial_debug";
  const VoidResult configured =
      host->configure_scheduler_defaults(scheduler_config);
  ASSERT_TRUE(configured.status.ok) << configured.status.message;

  const GraphSessionId session =
      load_test_graph(*host, temp.root(), "close_graph_error_retry_graph");
  {
    ScopedEnvironmentValue failure(kSchedulerFailureEnvironment,
                                   "shutdown_graph_not_found");
    const VoidResult failed_close = host->close_graph(session);
    EXPECT_FALSE(failed_close.status.ok);
    EXPECT_EQ(checked_graph_error_code(failed_close.status),
              GraphErrc::Unknown);
    EXPECT_EQ(failed_close.status.name, "unknown");
    EXPECT_NE(failed_close.status.message.find(
                  "fixture shutdown graph-not-found failure"),
              std::string::npos);
    EXPECT_EQ(fixture.shutdown_count(), 1);
  }

  const Result<std::vector<GraphSessionId>> retained = host->list_graphs();
  ASSERT_TRUE(retained.status.ok) << retained.status.message;
  ASSERT_EQ(retained.value.size(), 1u);
  EXPECT_EQ(retained.value.front().value, session.value);

  const Result<SchedulerInfoSnapshot> admitted =
      host->scheduler_info(session, ComputeIntent::GlobalHighPrecision);
  ASSERT_TRUE(admitted.status.ok) << admitted.status.message;
  EXPECT_EQ(admitted.value.scheduler_name, kDestroyCountSchedulerType);

  const VoidResult retry = host->close_graph(session);
  EXPECT_TRUE(retry.status.ok) << retry.status.message;
  EXPECT_EQ(fixture.shutdown_count(), 2);
}

TEST(EmbeddedHostAdapter,
     SpatialSnapshotPreservesOutputExtentSeparatelyFromRoi) {
  register_host_adapter_ops();
  ScopedTempDir temp("photospider_host_adapter_spatial_extent_test");
  auto host = create_embedded_host();
  ASSERT_NE(host, nullptr);

  const GraphSessionId session =
      load_test_graph(*host, temp.root(), "spatial_extent", "resized_extent");
  const HostComputeRequest compute_request = make_compute_request(session);
  auto compute_status = host->compute(compute_request);
  ASSERT_TRUE(compute_status.status.ok) << compute_status.status.message;

  auto node_view = host->inspect_node(session, NodeId{1});
  ASSERT_TRUE(node_view.status.ok) << node_view.status.message;
  ASSERT_TRUE(node_view.value.space.has_value());
  EXPECT_EQ(node_view.value.space->extent.width, 6);
  EXPECT_EQ(node_view.value.space->extent.height, 4);
  EXPECT_EQ(node_view.value.space->absolute_roi.width, 12);
  EXPECT_EQ(node_view.value.space->absolute_roi.height, 9);
  EXPECT_EQ(node_view.value.space->inverse_matrix[2], 5.0);
  EXPECT_EQ(node_view.value.space->inverse_matrix[5], 7.0);
  EXPECT_EQ(node_view.value.space->local_inverse_matrix[2], 11.0);
  EXPECT_EQ(node_view.value.space->local_inverse_matrix[5], 13.0);

  auto graph_view = host->inspect_graph(session);
  ASSERT_TRUE(graph_view.status.ok) << graph_view.status.message;
  ASSERT_EQ(graph_view.value.nodes.size(), 1u);
  ASSERT_TRUE(graph_view.value.nodes.front().space.has_value());
  EXPECT_EQ(graph_view.value.nodes.front().space->extent.width, 6);
  EXPECT_EQ(graph_view.value.nodes.front().space->extent.height, 4);
  EXPECT_EQ(graph_view.value.nodes.front().space->absolute_roi.width, 12);
  EXPECT_EQ(graph_view.value.nodes.front().space->absolute_roi.height, 9);
  EXPECT_EQ(graph_view.value.nodes.front().space->local_inverse_matrix[2],
            11.0);
  EXPECT_EQ(graph_view.value.nodes.front().space->local_inverse_matrix[5],
            13.0);

  auto tree = host->dependency_tree(session, std::nullopt, true);
  ASSERT_TRUE(tree.status.ok) << tree.status.message;
  ASSERT_EQ(tree.value.entries.size(), 1u);
  ASSERT_TRUE(tree.value.entries.front().node.space.has_value());
  EXPECT_EQ(tree.value.entries.front().node.space->extent.width, 6);
  EXPECT_EQ(tree.value.entries.front().node.space->extent.height, 4);
  EXPECT_EQ(tree.value.entries.front().node.space->absolute_roi.width, 12);
  EXPECT_EQ(tree.value.entries.front().node.space->absolute_roi.height, 9);
  EXPECT_EQ(tree.value.entries.front().node.space->local_inverse_matrix[2],
            11.0);
  EXPECT_EQ(tree.value.entries.front().node.space->local_inverse_matrix[5],
            13.0);
}

TEST(EmbeddedHostAdapter, ComputePlanningSnapshotsUsePublicValues) {
  register_host_adapter_ops();
  ScopedTempDir temp("photospider_host_adapter_planning_snapshot_test");
  auto host = create_embedded_host();
  ASSERT_NE(host, nullptr);

  const GraphSessionId session =
      load_test_graph(*host, temp.root(), "planning_snapshot");

  auto before_compute = host->compute_planning_snapshot(session);
  ASSERT_TRUE(before_compute.status.ok) << before_compute.status.message;
  EXPECT_FALSE(before_compute.value.has_value());

  auto before_history = host->recent_compute_planning_snapshots(session);
  ASSERT_TRUE(before_history.status.ok) << before_history.status.message;
  EXPECT_TRUE(before_history.value.empty());

  HostComputeRequest compute_request = make_compute_request(session);
  compute_request.execution.parallel = true;
  auto compute_status = host->compute(compute_request);
  ASSERT_TRUE(compute_status.status.ok) << compute_status.status.message;

  auto latest = host->compute_planning_snapshot(session);
  ASSERT_TRUE(latest.status.ok) << latest.status.message;
  ASSERT_TRUE(latest.value.has_value());
  EXPECT_EQ(latest.value->intent, ComputeIntent::GlobalHighPrecision);
  EXPECT_EQ(latest.value->target_node.value, 1);
  EXPECT_TRUE(latest.value->parallel);
  EXPECT_EQ(latest.value->planned_node_count, 1u);
  EXPECT_GE(latest.value->task_count, 1u);
  EXPECT_GE(latest.value->active_task_count, 1u);
  ASSERT_FALSE(latest.value->planned_node_sample.empty());
  EXPECT_EQ(latest.value->planned_node_sample.front().value, 1);
  ASSERT_FALSE(latest.value->task_sample.empty());
  EXPECT_EQ(latest.value->task_sample.front().node.value, 1);
  EXPECT_FALSE(latest.value->task_sample.front().kind.empty());

  auto history = host->recent_compute_planning_snapshots(session);
  ASSERT_TRUE(history.status.ok) << history.status.message;
  ASSERT_FALSE(history.value.empty());
  EXPECT_EQ(history.value.back().target_node.value, 1);

  auto missing = host->compute_planning_snapshot(GraphSessionId{"missing"});
  EXPECT_FALSE(missing.status.ok);
  EXPECT_EQ(checked_graph_error_code(missing.status), GraphErrc::NotFound);
}

TEST(EmbeddedHostAdapter, AsyncComputeCanFinishAfterCloseGraphRequest) {
  register_host_adapter_ops();
  ScopedTempDir temp("photospider_host_adapter_async_close_test");
  auto host = create_embedded_host();
  ASSERT_NE(host, nullptr);

  const GraphSessionId session =
      load_test_graph(*host, temp.root(), "async_close_graph", "slow_source");
  HostComputeRequest request = make_compute_request(session);
  request.execution.parallel = true;

  auto async_compute = host->compute_async(request);
  ASSERT_TRUE(async_compute.status.ok) << async_compute.status.message;

  auto close = host->close_graph(session);
  EXPECT_TRUE(close.status.ok) << close.status.message;
  EXPECT_EQ(async_compute.value.wait_for(std::chrono::milliseconds(0)),
            std::future_status::ready);

  OperationStatus async_status = async_compute.value.get();
  EXPECT_TRUE(async_status.ok) << async_status.message;

  auto ids_after_close = host->list_node_ids(session);
  EXPECT_FALSE(ids_after_close.status.ok);
  EXPECT_EQ(checked_graph_error_code(ids_after_close.status),
            GraphErrc::NotFound);
}

TEST(EmbeddedHostAdapter, AsyncComputeRejectsNewWorkWhileCloseIsWaiting) {
  register_host_adapter_ops();
  ScopedTempDir temp("photospider_host_adapter_async_close_gate_test");
  auto host = create_embedded_host();
  ASSERT_NE(host, nullptr);

  const GraphSessionId session = load_test_graph(
      *host, temp.root(), "async_close_gate_graph", "slow_source", 250);
  HostComputeRequest request = make_compute_request(session);
  request.execution.parallel = true;

  auto initial_async = host->compute_async(request);
  ASSERT_TRUE(initial_async.status.ok) << initial_async.status.message;

  auto close_future = std::async(std::launch::async, [&host, session]() {
    return host->close_graph(session);
  });
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  ASSERT_EQ(close_future.wait_for(std::chrono::milliseconds(0)),
            std::future_status::timeout);

  auto rejected_async = host->compute_async(request);
  EXPECT_FALSE(rejected_async.status.ok);
  EXPECT_EQ(checked_graph_error_code(rejected_async.status),
            GraphErrc::NotFound);

  OperationStatus initial_status = initial_async.value.get();
  EXPECT_TRUE(initial_status.ok) << initial_status.message;

  auto close = close_future.get();
  EXPECT_TRUE(close.status.ok) << close.status.message;
}

TEST(EmbeddedHostAdapter, AsyncComputeFailureStatusSurvivesCloseGraph) {
  register_host_adapter_ops();
  ScopedTempDir temp("photospider_host_adapter_async_failure_close_test");
  auto host = create_embedded_host();
  ASSERT_NE(host, nullptr);

  GraphLoadRequest missing_op_load;
  missing_op_load.session = GraphSessionId{"async_missing_op_graph"};
  missing_op_load.root_dir = (temp.root() / "sessions").string();
  missing_op_load.yaml_path =
      (temp.root() / "source" / "async_missing_op_graph.yaml").string();
  missing_op_load.cache_root_dir = (temp.root() / "cache").string();
  write_host_adapter_unregistered_op_graph(missing_op_load.yaml_path);
  auto loaded_missing_op = host->load_graph(missing_op_load);
  ASSERT_TRUE(loaded_missing_op.status.ok) << loaded_missing_op.status.message;

  auto async_compute =
      host->compute_async(make_compute_request(missing_op_load.session));
  ASSERT_TRUE(async_compute.status.ok) << async_compute.status.message;

  auto close = host->close_graph(missing_op_load.session);
  ASSERT_TRUE(close.status.ok) << close.status.message;
  EXPECT_EQ(async_compute.value.wait_for(std::chrono::milliseconds(0)),
            std::future_status::ready);

  OperationStatus async_status = async_compute.value.get();
  EXPECT_FALSE(async_status.ok);
  EXPECT_EQ(checked_graph_error_code(async_status), GraphErrc::NoOperation);
  EXPECT_NE(async_status.message.find("No op"), std::string::npos);

  auto closed_error = host->last_error(missing_op_load.session);
  EXPECT_TRUE(closed_error.ok) << closed_error.message;
}

/**
 * @brief Verifies close waits for an admitted synchronous Host compute.
 *
 * @throws Nothing when close remains pending while the deterministic operation
 * holds graph-state execution, then both calls finish without runtime lifetime
 * overlap.
 * @note This exercises the embedded admission gate and Kernel close
 * serialization through public Host methods rather than direct runtime access.
 */
TEST(EmbeddedHostAdapter, CloseWaitsForAdmittedSynchronousCompute) {
  register_host_adapter_ops();
  ScopedTempDir temp("photospider_host_adapter_sync_close_gate_test");
  auto host = create_embedded_host();
  ASSERT_NE(host, nullptr);
  const GraphSessionId session = load_test_graph(
      *host, temp.root(), "sync_close_gate_graph", "blocking_source");

  std::promise<void> release_compute;
  configure_host_blocking_source(release_compute.get_future().share());
  HostComputeRequest request = make_compute_request(session);
  request.execution.parallel = true;
  request.cache.force_recache = true;
  auto compute_future = std::async(
      std::launch::async, [&host, request] { return host->compute(request); });

  if (!wait_for_host_blocking_source(std::chrono::seconds(2))) {
    release_compute.set_value();
    (void)compute_future.get();
    reset_host_blocking_source();
    (void)host->close_graph(session);
    FAIL() << "blocking synchronous Host compute did not start";
  }

  std::promise<void> close_entered;
  auto close_entered_future = close_entered.get_future();
  auto close_future = std::async(std::launch::async, [&] {
    close_entered.set_value();
    return host->close_graph(session);
  });
  EXPECT_EQ(close_entered_future.wait_for(std::chrono::seconds(2)),
            std::future_status::ready);
  EXPECT_EQ(close_future.wait_for(std::chrono::milliseconds(100)),
            std::future_status::timeout);

  release_compute.set_value();
  const VoidResult compute = compute_future.get();
  EXPECT_TRUE(compute.status.ok) << compute.status.message;
  const VoidResult close = close_future.get();
  EXPECT_TRUE(close.status.ok) << close.status.message;

  reset_host_blocking_source();
}

/**
 * @brief Verifies concurrent closes settle as owner success then waiter absent.
 *
 * @throws Nothing when deterministic admission ordering and close results hold;
 *         GoogleTest records any mismatch.
 * @note A blocking admitted compute keeps the first close pending after it has
 *       claimed the close marker. A BUILD_TESTING callback proves the second
 *       close reaches duplicate-marker wait before the compute is released.
 */
TEST(EmbeddedHostAdapter, ConcurrentCloseOwnerSuccessMakesWaiterNotFound) {
  register_host_adapter_ops();
  ScopedTempDir temp("photospider_host_adapter_concurrent_close_success_test");
  auto host = create_embedded_host();
  ASSERT_NE(host, nullptr);
  const GraphSessionId session = load_test_graph(
      *host, temp.root(), "concurrent_close_success_graph", "blocking_source");

  std::promise<void> release_compute;
  configure_host_blocking_source(release_compute.get_future().share());
  HostComputeRequest request = make_compute_request(session);
  request.execution.parallel = true;
  request.cache.force_recache = true;
  auto compute_future = std::async(
      std::launch::async, [&host, request] { return host->compute(request); });

  if (!wait_for_host_blocking_source(std::chrono::seconds(2))) {
    release_compute.set_value();
    (void)compute_future.get();
    reset_host_blocking_source();
    (void)host->close_graph(session);
    FAIL() << "blocking synchronous Host compute did not start";
  }

#if defined(PHOTOSPIDER_INTERNAL_BAD_ALLOC_TESTING)
  EmbeddedCloseEventState close_events;
  ScopedEmbeddedCloseTestHook close_hook(close_events);
#endif
  auto owner = std::async(std::launch::async, [&host, session] {
    return host->close_graph(session);
  });
#if defined(PHOTOSPIDER_INTERNAL_BAD_ALLOC_TESTING)
  if (!wait_for_embedded_close_event(close_events.marker_claimed, 1,
                                     std::chrono::seconds(2))) {
    release_compute.set_value();
    (void)compute_future.get();
    (void)owner.get();
    reset_host_blocking_source();
    FAIL() << "owner close did not claim the session marker";
  }
#endif

  auto waiter = std::async(std::launch::async,
                           [&] { return host->close_graph(session); });
#if defined(PHOTOSPIDER_INTERNAL_BAD_ALLOC_TESTING)
  if (!wait_for_embedded_close_event(close_events.duplicate_about_to_wait, 1,
                                     std::chrono::seconds(2))) {
    release_compute.set_value();
    (void)compute_future.get();
    (void)owner.get();
    (void)waiter.get();
    reset_host_blocking_source();
    FAIL() << "second close did not enter duplicate-marker wait";
  }
#endif

  release_compute.set_value();
  const VoidResult compute = compute_future.get();
  EXPECT_TRUE(compute.status.ok) << compute.status.message;
  const VoidResult owner_result = owner.get();
  EXPECT_TRUE(owner_result.status.ok) << owner_result.status.message;
  const VoidResult waiter_result = waiter.get();
  EXPECT_FALSE(waiter_result.status.ok);
  EXPECT_EQ(checked_graph_error_code(waiter_result.status),
            GraphErrc::NotFound);
  reset_host_blocking_source();
}

/**
 * @brief Verifies every duplicate close waiter claims the marker exclusively.
 *
 * @throws Nothing when one owner succeeds and two waiters serialize to
 *         NotFound; GoogleTest records any mismatch.
 * @note Both waiters are observed inside the marker wait before compute is
 *       released. This catches a single-check implementation that wakes both
 *       waiters and lets them enter Kernel close concurrently.
 */
TEST(EmbeddedHostAdapter, ThreeConcurrentClosesSerializeEveryWaiter) {
  register_host_adapter_ops();
  ScopedTempDir temp("photospider_host_adapter_three_close_test");
  auto host = create_embedded_host();
  ASSERT_NE(host, nullptr);
  const GraphSessionId session = load_test_graph(
      *host, temp.root(), "three_close_graph", "blocking_source");

  std::promise<void> release_compute;
  configure_host_blocking_source(release_compute.get_future().share());
  HostComputeRequest request = make_compute_request(session);
  request.execution.parallel = true;
  request.cache.force_recache = true;
  auto compute_future = std::async(
      std::launch::async, [&host, request] { return host->compute(request); });
  if (!wait_for_host_blocking_source(std::chrono::seconds(2))) {
    release_compute.set_value();
    (void)compute_future.get();
    reset_host_blocking_source();
    (void)host->close_graph(session);
    FAIL() << "blocking synchronous Host compute did not start";
  }

#if defined(PHOTOSPIDER_INTERNAL_BAD_ALLOC_TESTING)
  EmbeddedCloseEventState close_events;
  ScopedEmbeddedCloseTestHook close_hook(close_events);
#endif
  auto owner = std::async(std::launch::async, [&host, session] {
    return host->close_graph(session);
  });
#if defined(PHOTOSPIDER_INTERNAL_BAD_ALLOC_TESTING)
  if (!wait_for_embedded_close_event(close_events.marker_claimed, 1,
                                     std::chrono::seconds(2))) {
    release_compute.set_value();
    (void)compute_future.get();
    (void)owner.get();
    reset_host_blocking_source();
    FAIL() << "owner close did not claim the session marker";
  }
#endif

  auto first_waiter = std::async(std::launch::async, [&host, session] {
    return host->close_graph(session);
  });
  auto second_waiter = std::async(std::launch::async, [&host, session] {
    return host->close_graph(session);
  });
#if defined(PHOTOSPIDER_INTERNAL_BAD_ALLOC_TESTING)
  if (!wait_for_embedded_close_event(close_events.duplicate_about_to_wait, 2,
                                     std::chrono::seconds(2))) {
    release_compute.set_value();
    (void)compute_future.get();
    (void)owner.get();
    (void)first_waiter.get();
    (void)second_waiter.get();
    reset_host_blocking_source();
    FAIL() << "both duplicate closes did not enter marker waits";
  }
#endif

  release_compute.set_value();
  EXPECT_TRUE(compute_future.get().status.ok);
  const VoidResult owner_result = owner.get();
  EXPECT_TRUE(owner_result.status.ok) << owner_result.status.message;
  const VoidResult first_result = first_waiter.get();
  const VoidResult second_result = second_waiter.get();
  EXPECT_FALSE(first_result.status.ok);
  EXPECT_EQ(checked_graph_error_code(first_result.status), GraphErrc::NotFound);
  EXPECT_FALSE(second_result.status.ok);
  EXPECT_EQ(checked_graph_error_code(second_result.status),
            GraphErrc::NotFound);
  reset_host_blocking_source();
}

/**
 * @brief Verifies a waiter retries after the owner consumes a one-shot failure.
 *
 * @throws Nothing when the owner returns Unknown and the waiter closes the
 *         retained graph; GoogleTest records any mismatch.
 * @note The process-scoped fixture fails only its first shutdown invocation, so
 *       no environment mutation races the two close attempts.
 */
TEST(EmbeddedHostAdapter, ConcurrentCloseRetriesAfterEarlierShutdownFailure) {
  register_host_adapter_ops();
  ScopedTempDir temp("photospider_host_adapter_concurrent_close_failure_test");
  ScopedSchedulerPluginCleanup scheduler_cleanup;
  auto host = create_embedded_host();
  ASSERT_NE(host, nullptr);

  const std::filesystem::path plugin_path =
      destroy_count_scheduler_plugin_path();
  ASSERT_TRUE(std::filesystem::exists(plugin_path));
  SchedulerFixtureExports fixture(plugin_path);
  fixture.reset_counts();
  const VoidResult plugin_load = host->scheduler_load(plugin_path.string());
  ASSERT_TRUE(plugin_load.status.ok) << plugin_load.status.message;

  HostSchedulerConfig scheduler_config;
  scheduler_config.hp_type = kDestroyCountSchedulerType;
  scheduler_config.rt_type = "serial_debug";
  ASSERT_TRUE(host->configure_scheduler_defaults(scheduler_config).status.ok);

  const GraphSessionId session = load_test_graph(
      *host, temp.root(), "concurrent_close_failure_graph", "blocking_source");
  std::promise<void> release_compute;
  configure_host_blocking_source(release_compute.get_future().share());
  HostComputeRequest request = make_compute_request(session);
  request.execution.parallel = true;
  request.cache.force_recache = true;
  auto compute_future = std::async(
      std::launch::async, [&host, request] { return host->compute(request); });
  if (!wait_for_host_blocking_source(std::chrono::seconds(2))) {
    release_compute.set_value();
    (void)compute_future.get();
    reset_host_blocking_source();
    FAIL() << "blocking synchronous Host compute did not start";
  }

  ScopedEnvironmentValue failure(kSchedulerFailureEnvironment,
                                 "shutdown_runtime_error_once");
#if defined(PHOTOSPIDER_INTERNAL_BAD_ALLOC_TESTING)
  EmbeddedCloseEventState close_events;
  ScopedEmbeddedCloseTestHook close_hook(close_events);
#endif
  auto owner = std::async(std::launch::async, [&host, session] {
    return host->close_graph(session);
  });
#if defined(PHOTOSPIDER_INTERNAL_BAD_ALLOC_TESTING)
  if (!wait_for_embedded_close_event(close_events.marker_claimed, 1,
                                     std::chrono::seconds(2))) {
    release_compute.set_value();
    (void)compute_future.get();
    (void)owner.get();
    reset_host_blocking_source();
    FAIL() << "owner close did not claim the session marker";
  }
#endif

  auto waiter = std::async(std::launch::async,
                           [&] { return host->close_graph(session); });
#if defined(PHOTOSPIDER_INTERNAL_BAD_ALLOC_TESTING)
  if (!wait_for_embedded_close_event(close_events.duplicate_about_to_wait, 1,
                                     std::chrono::seconds(2))) {
    release_compute.set_value();
    (void)compute_future.get();
    (void)owner.get();
    (void)waiter.get();
    reset_host_blocking_source();
    FAIL() << "second close did not enter duplicate-marker wait";
  }
#endif

  release_compute.set_value();
  EXPECT_TRUE(compute_future.get().status.ok);
  const VoidResult owner_result = owner.get();
  EXPECT_FALSE(owner_result.status.ok);
  EXPECT_EQ(checked_graph_error_code(owner_result.status), GraphErrc::Unknown);
  const VoidResult waiter_result = waiter.get();
  EXPECT_TRUE(waiter_result.status.ok) << waiter_result.status.message;
  EXPECT_EQ(fixture.shutdown_count(), 2);
  reset_host_blocking_source();
}

/**
 * @brief Verifies overlapping failures for one session keep distinct statuses.
 *
 * @throws Nothing when both public futures preserve their work-item-owned
 * status after the shared Kernel diagnostic has changed.
 * @note Both requests are accepted before either future is consumed. They use
 * one graph-state executor but fail with different stable GraphErrc values, so
 * reconstructing either result from the final shared LastError is invalid.
 */
TEST(EmbeddedHostAdapter, OverlappingAsyncFailuresOwnTheirExactStatus) {
  register_host_adapter_ops();
  ScopedTempDir temp("photospider_host_adapter_async_exact_status_test");
  auto host = create_embedded_host();
  ASSERT_NE(host, nullptr);

  GraphLoadRequest load;
  load.session = GraphSessionId{"async_exact_status_graph"};
  load.root_dir = (temp.root() / "sessions").string();
  load.yaml_path =
      (temp.root() / "source" / "async_exact_status_graph.yaml").string();
  load.cache_root_dir = (temp.root() / "cache").string();
  write_host_adapter_unregistered_op_graph(load.yaml_path);
  auto loaded = host->load_graph(load);
  ASSERT_TRUE(loaded.status.ok) << loaded.status.message;

  HostComputeRequest missing_op_request = make_compute_request(load.session);
  HostComputeRequest missing_node_request = missing_op_request;
  missing_node_request.node = NodeId{99};

  auto missing_op = host->compute_async(missing_op_request);
  auto missing_node = host->compute_async(missing_node_request);
  ASSERT_TRUE(missing_op.status.ok) << missing_op.status.message;
  ASSERT_TRUE(missing_node.status.ok) << missing_node.status.message;
  ASSERT_EQ(missing_op.value.wait_for(std::chrono::seconds(2)),
            std::future_status::ready);
  ASSERT_EQ(missing_node.value.wait_for(std::chrono::seconds(2)),
            std::future_status::ready);

  const OperationStatus missing_op_status = missing_op.value.get();
  const OperationStatus missing_node_status = missing_node.value.get();
  EXPECT_FALSE(missing_op_status.ok);
  EXPECT_EQ(missing_op_status.domain, OperationErrorDomain::Graph);
  EXPECT_EQ(checked_graph_error_code(missing_op_status),
            GraphErrc::NoOperation);
  EXPECT_EQ(missing_op_status.name, "no_operation");
  EXPECT_FALSE(missing_op_status.message.empty());
  EXPECT_FALSE(missing_node_status.ok);
  EXPECT_EQ(missing_node_status.domain, OperationErrorDomain::Graph);
  EXPECT_EQ(checked_graph_error_code(missing_node_status), GraphErrc::NotFound);
  EXPECT_EQ(missing_node_status.name, "not_found");
  EXPECT_FALSE(missing_node_status.message.empty());
  EXPECT_NE(missing_op_status.message, missing_node_status.message);

  auto close = host->close_graph(load.session);
  EXPECT_TRUE(close.status.ok) << close.status.message;
}

TEST(EmbeddedHostAdapter, SyncComputePropagatesNodeExecutionBadAlloc) {
  register_host_adapter_ops();
  ScopedTempDir temp("photospider_host_bad_alloc_sync");
  auto host = create_embedded_host();
  ASSERT_NE(host, nullptr);

  const GraphSessionId session = load_test_graph(
      *host, temp.root(), "bad_alloc_sync", "resource_exhausted");
  const HostComputeRequest request = make_compute_request(session);

  try {
    const VoidResult result = host->compute(request);
    FAIL() << "std::bad_alloc was converted to Host status: code="
           << static_cast<int>(result.status.code)
           << " message=" << result.status.message;
  } catch (const std::bad_alloc&) {
    SUCCEED();
  }
}

TEST(EmbeddedHostAdapter, AsyncComputeFuturePropagatesNodeExecutionBadAlloc) {
  register_host_adapter_ops();
  ScopedTempDir temp("photospider_host_bad_alloc_async");
  auto host = create_embedded_host();
  ASSERT_NE(host, nullptr);

  const GraphSessionId session = load_test_graph(
      *host, temp.root(), "bad_alloc_async", "resource_exhausted");
  auto scheduled = host->compute_async(make_compute_request(session));
  ASSERT_TRUE(scheduled.status.ok) << scheduled.status.message;
  ASSERT_TRUE(scheduled.value.valid());

  try {
    const OperationStatus status = scheduled.value.get();
    FAIL() << "std::bad_alloc was converted by async Host path: code="
           << static_cast<int>(status.code) << " message=" << status.message;
  } catch (const std::bad_alloc&) {
    SUCCEED();
  }
}

TEST(EmbeddedHostAdapter, ComputeReturnsNotFoundForMissingSession) {
  register_host_adapter_ops();
  ScopedTempDir temp("photospider_host_adapter_compute_missing_test");
  auto host = create_embedded_host();
  ASSERT_NE(host, nullptr);

  HostComputeRequest missing_request =
      make_compute_request(GraphSessionId{"missing_compute_graph"});
  auto missing = host->compute(missing_request);
  EXPECT_FALSE(missing.status.ok);
  EXPECT_EQ(checked_graph_error_code(missing.status), GraphErrc::NotFound);
  auto missing_image = host->compute_and_get_image(missing_request);
  EXPECT_FALSE(missing_image.status.ok);
  EXPECT_EQ(checked_graph_error_code(missing_image.status),
            GraphErrc::NotFound);

  const GraphSessionId session =
      load_test_graph(*host, temp.root(), "closed_compute_graph");
  HostComputeRequest closed_request = make_compute_request(session);
  auto initial = host->compute(closed_request);
  ASSERT_TRUE(initial.status.ok) << initial.status.message;

  auto close = host->close_graph(session);
  ASSERT_TRUE(close.status.ok) << close.status.message;

  auto closed = host->compute(closed_request);
  EXPECT_FALSE(closed.status.ok);
  EXPECT_EQ(checked_graph_error_code(closed.status), GraphErrc::NotFound);
  auto closed_image = host->compute_and_get_image(closed_request);
  EXPECT_FALSE(closed_image.status.ok);
  EXPECT_EQ(checked_graph_error_code(closed_image.status), GraphErrc::NotFound);
}

TEST(EmbeddedHostAdapter, CloseGraphClearsStaleLastErrorBeforeImageCompute) {
  register_host_adapter_ops();
  ScopedTempDir temp("photospider_host_adapter_close_clears_error_test");
  auto host = create_embedded_host();
  ASSERT_NE(host, nullptr);

  const GraphSessionId session =
      load_test_graph(*host, temp.root(), "close_clears_error_graph");
  HostComputeRequest missing_node_request = make_compute_request(session);
  missing_node_request.node = NodeId{99};

  auto missing_node_image = host->compute_and_get_image(missing_node_request);
  ASSERT_FALSE(missing_node_image.status.ok);
  ASSERT_EQ(checked_graph_error_code(missing_node_image.status),
            GraphErrc::NotFound);

  auto stale_error = host->last_error(session);
  ASSERT_FALSE(stale_error.ok);
  ASSERT_EQ(checked_graph_error_code(stale_error), GraphErrc::NotFound);

  auto close = host->close_graph(session);
  ASSERT_TRUE(close.status.ok) << close.status.message;

  auto closed_error = host->last_error(session);
  EXPECT_TRUE(closed_error.ok) << closed_error.message;

  auto closed_image =
      host->compute_and_get_image(make_compute_request(session));
  EXPECT_FALSE(closed_image.status.ok);
  EXPECT_EQ(checked_graph_error_code(closed_image.status), GraphErrc::NotFound);
}

TEST(EmbeddedHostAdapter, ComputeImagePreservesBackendFailureStatus) {
  register_host_adapter_ops();
  ScopedTempDir temp("photospider_host_adapter_image_status_test");
  auto host = create_embedded_host();
  ASSERT_NE(host, nullptr);

  const GraphSessionId session =
      load_test_graph(*host, temp.root(), "image_status_graph");
  HostComputeRequest missing_node_request = make_compute_request(session);
  missing_node_request.node = NodeId{99};

  auto missing_node_image = host->compute_and_get_image(missing_node_request);
  EXPECT_FALSE(missing_node_image.status.ok);
  EXPECT_EQ(checked_graph_error_code(missing_node_image.status),
            GraphErrc::NotFound);
  EXPECT_FALSE(missing_node_image.status.message.empty());

  auto missing_node_error = host->last_error(session);
  EXPECT_FALSE(missing_node_error.ok);
  EXPECT_EQ(checked_graph_error_code(missing_node_error), GraphErrc::NotFound);
  EXPECT_FALSE(missing_node_error.message.empty());

  auto recovered_image =
      host->compute_and_get_image(make_compute_request(session));
  ASSERT_TRUE(recovered_image.status.ok) << recovered_image.status.message;
  auto cleared_error = host->last_error(session);
  EXPECT_TRUE(cleared_error.ok) << cleared_error.message;

  GraphLoadRequest missing_op_load;
  missing_op_load.session = GraphSessionId{"image_missing_op_graph"};
  missing_op_load.root_dir = (temp.root() / "sessions").string();
  missing_op_load.yaml_path =
      (temp.root() / "source" / "image_missing_op_graph.yaml").string();
  missing_op_load.cache_root_dir = (temp.root() / "cache").string();
  write_host_adapter_unregistered_op_graph(missing_op_load.yaml_path);
  auto loaded_missing_op = host->load_graph(missing_op_load);
  ASSERT_TRUE(loaded_missing_op.status.ok) << loaded_missing_op.status.message;

  auto missing_op_image = host->compute_and_get_image(
      make_compute_request(missing_op_load.session));
  EXPECT_FALSE(missing_op_image.status.ok);
  EXPECT_EQ(checked_graph_error_code(missing_op_image.status),
            GraphErrc::NoOperation);
  EXPECT_NE(missing_op_image.status.message.find("No op"), std::string::npos);

  auto missing_op_error = host->last_error(missing_op_load.session);
  EXPECT_FALSE(missing_op_error.ok);
  EXPECT_EQ(checked_graph_error_code(missing_op_error), GraphErrc::NoOperation);
  EXPECT_NE(missing_op_error.message.find("No op"), std::string::npos);
}

TEST(EmbeddedHostAdapter, ComputeImagePreservesSuccessfulEmptyOutput) {
  register_host_adapter_ops();
  ScopedTempDir temp("photospider_host_adapter_empty_image_test");
  auto host = create_embedded_host();
  ASSERT_NE(host, nullptr);

  const GraphSessionId session =
      load_test_graph(*host, temp.root(), "empty_image_graph", "no_image");
  HostComputeRequest missing_node_request = make_compute_request(session);
  missing_node_request.node = NodeId{99};
  auto missing_node_image = host->compute_and_get_image(missing_node_request);
  ASSERT_FALSE(missing_node_image.status.ok);
  ASSERT_EQ(checked_graph_error_code(missing_node_image.status),
            GraphErrc::NotFound);

  auto empty_image = host->compute_and_get_image(make_compute_request(session));
  ASSERT_TRUE(empty_image.status.ok) << empty_image.status.message;
  EXPECT_EQ(empty_image.value.width, 0);
  EXPECT_EQ(empty_image.value.height, 0);
  EXPECT_EQ(empty_image.value.data, nullptr);

  auto cleared_error = host->last_error(session);
  EXPECT_TRUE(cleared_error.ok) << cleared_error.message;

  auto closed = host->close_graph(session);
  ASSERT_TRUE(closed.status.ok) << closed.status.message;

  auto closed_image =
      host->compute_and_get_image(make_compute_request(session));
  EXPECT_FALSE(closed_image.status.ok);
  EXPECT_EQ(checked_graph_error_code(closed_image.status), GraphErrc::NotFound);
}

TEST(EmbeddedHostAdapter, ReplaceSchedulerReturnsNotFoundForMissingSession) {
  register_host_adapter_ops();
  ScopedTempDir temp("photospider_host_adapter_scheduler_missing_test");
  auto host = create_embedded_host();
  ASSERT_NE(host, nullptr);

  auto missing = host->replace_scheduler(
      GraphSessionId{"missing_scheduler_graph"},
      ComputeIntent::GlobalHighPrecision, "serial_debug");
  EXPECT_FALSE(missing.status.ok);
  EXPECT_EQ(checked_graph_error_code(missing.status), GraphErrc::NotFound);

  const GraphSessionId session =
      load_test_graph(*host, temp.root(), "closed_scheduler_graph");
  auto invalid_type = host->replace_scheduler(
      session, ComputeIntent::GlobalHighPrecision, "missing_scheduler_type");
  EXPECT_FALSE(invalid_type.status.ok);
  EXPECT_EQ(checked_graph_error_code(invalid_type.status),
            GraphErrc::InvalidParameter);

  auto close = host->close_graph(session);
  ASSERT_TRUE(close.status.ok) << close.status.message;

  auto closed = host->replace_scheduler(
      session, ComputeIntent::GlobalHighPrecision, "serial_debug");
  EXPECT_FALSE(closed.status.ok);
  EXPECT_EQ(checked_graph_error_code(closed.status), GraphErrc::NotFound);
}

TEST(EmbeddedHostAdapter, ReloadSaveSetNodeAndClearGraphReturnStatuses) {
  register_host_adapter_ops();
  ScopedTempDir temp("photospider_host_adapter_graph_mutation_test");
  auto host = create_embedded_host();
  ASSERT_NE(host, nullptr);

  const GraphSessionId session =
      load_test_graph(*host, temp.root(), "graph_mutation");

  auto set = host->set_node_yaml(session, NodeId{1},
                                 replacement_node_yaml("host_replaced", 9, 2));
  ASSERT_TRUE(set.status.ok) << set.status.message;

  auto node = host->inspect_node(session, NodeId{1});
  ASSERT_TRUE(node.status.ok) << node.status.message;
  EXPECT_EQ(node.value.name, "host_replaced");
  EXPECT_EQ(node.value.parameters.at("width"), "9");

  const auto saved_path = temp.root() / "saved" / "saved_graph.yaml";
  std::filesystem::create_directories(saved_path.parent_path());
  auto save = host->save_graph(session, saved_path.string());
  ASSERT_TRUE(save.status.ok) << save.status.message;
  EXPECT_TRUE(std::filesystem::exists(saved_path));

  const auto reload_path = temp.root() / "source" / "reload_graph.yaml";
  write_host_adapter_graph(reload_path, 11, 5);
  auto reload = host->reload_graph(session, reload_path.string());
  ASSERT_TRUE(reload.status.ok) << reload.status.message;

  auto reloaded = host->inspect_node(session, NodeId{1});
  ASSERT_TRUE(reloaded.status.ok) << reloaded.status.message;
  EXPECT_EQ(reloaded.value.parameters.at("width"), "11");

  auto missing_reload =
      host->reload_graph(GraphSessionId{"missing_graph"}, reload_path.string());
  EXPECT_FALSE(missing_reload.status.ok);
  EXPECT_EQ(checked_graph_error_code(missing_reload.status),
            GraphErrc::NotFound);

  const auto missing_file_path =
      temp.root() / "source" / "missing_reload_graph.yaml";
  auto io_reload = host->reload_graph(session, missing_file_path.string());
  EXPECT_FALSE(io_reload.status.ok);
  EXPECT_EQ(checked_graph_error_code(io_reload.status), GraphErrc::Io);

  auto after_io_reload = host->inspect_node(session, NodeId{1});
  ASSERT_TRUE(after_io_reload.status.ok) << after_io_reload.status.message;
  EXPECT_EQ(after_io_reload.value.parameters.at("width"), "11");

  const auto invalid_yaml_path =
      temp.root() / "source" / "invalid_reload_graph.yaml";
  {
    std::ofstream invalid_yaml(invalid_yaml_path);
    invalid_yaml << "not: a sequence\n";
  }
  auto invalid_reload = host->reload_graph(session, invalid_yaml_path.string());
  EXPECT_FALSE(invalid_reload.status.ok);
  EXPECT_EQ(checked_graph_error_code(invalid_reload.status),
            GraphErrc::InvalidYaml);

  auto after_invalid_reload = host->inspect_node(session, NodeId{1});
  ASSERT_TRUE(after_invalid_reload.status.ok)
      << after_invalid_reload.status.message;
  EXPECT_EQ(after_invalid_reload.value.parameters.at("width"), "11");

  auto clear = host->clear_graph(session);
  ASSERT_TRUE(clear.status.ok) << clear.status.message;

  auto ids = host->list_node_ids(session);
  ASSERT_TRUE(ids.status.ok) << ids.status.message;
  EXPECT_TRUE(ids.value.empty());
}

TEST(EmbeddedHostAdapter, RoiProjectionUsesPublicPixelRectValues) {
  register_host_adapter_ops();
  ScopedTempDir temp("photospider_host_adapter_roi_projection_test");
  auto host = create_embedded_host();
  ASSERT_NE(host, nullptr);

  const auto yaml_path = temp.root() / "source" / "roi_graph.yaml";
  write_host_adapter_roi_graph(yaml_path);

  GraphLoadRequest request;
  request.session = GraphSessionId{"roi_projection"};
  request.root_dir = (temp.root() / "sessions").string();
  request.yaml_path = yaml_path.string();
  request.cache_root_dir = (temp.root() / "cache").string();

  auto loaded = host->load_graph(request);
  ASSERT_TRUE(loaded.status.ok) << loaded.status.message;

  const PixelRect roi{1, 2, 3, 2};
  auto projected =
      host->project_roi(request.session, NodeId{1}, roi, NodeId{2});
  ASSERT_TRUE(projected.status.ok) << projected.status.message;
  EXPECT_EQ(projected.value.x, roi.x);
  EXPECT_EQ(projected.value.y, roi.y);
  EXPECT_EQ(projected.value.width, roi.width);
  EXPECT_EQ(projected.value.height, roi.height);

  auto back_projected =
      host->project_roi_backward(request.session, NodeId{2}, roi, NodeId{1});
  ASSERT_TRUE(back_projected.status.ok) << back_projected.status.message;
  EXPECT_EQ(back_projected.value.x, roi.x);
  EXPECT_EQ(back_projected.value.y, roi.y);
  EXPECT_EQ(back_projected.value.width, roi.width);
  EXPECT_EQ(back_projected.value.height, roi.height);

  auto missing_target =
      host->project_roi(request.session, NodeId{1}, roi, NodeId{99});
  EXPECT_FALSE(missing_target.status.ok);
  EXPECT_EQ(checked_graph_error_code(missing_target.status),
            GraphErrc::InvalidParameter);
}

TEST(EmbeddedHostAdapter, DirtySnapshotPreservesMonolithicAndEdgeDetails) {
  register_host_adapter_ops();
  ScopedTempDir temp("photospider_host_adapter_dirty_snapshot_details_test");
  auto host = create_embedded_host();
  ASSERT_NE(host, nullptr);

  const auto yaml_path = temp.root() / "source" / "dirty_roi_graph.yaml";
  write_host_adapter_offset_roi_graph(yaml_path);

  GraphLoadRequest request;
  request.session = GraphSessionId{"dirty_snapshot_details"};
  request.root_dir = (temp.root() / "sessions").string();
  request.yaml_path = yaml_path.string();
  request.cache_root_dir = (temp.root() / "cache").string();

  auto loaded = host->load_graph(request);
  ASSERT_TRUE(loaded.status.ok) << loaded.status.message;

  HostComputeRequest full_request;
  full_request.session = request.session;
  full_request.node = NodeId{2};
  full_request.cache.precision = "fp32";
  auto initial_compute = host->compute(full_request);
  ASSERT_TRUE(initial_compute.status.ok) << initial_compute.status.message;

  HostComputeRequest dirty_request = full_request;
  dirty_request.intent = ComputeIntent::GlobalHighPrecision;
  dirty_request.dirty_roi = PixelRect{70, 10, 20, 20};
  auto dirty_compute = host->compute(dirty_request);
  ASSERT_TRUE(dirty_compute.status.ok) << dirty_compute.status.message;

  auto snapshot = host->dirty_region_snapshot(request.session);
  ASSERT_TRUE(snapshot.status.ok) << snapshot.status.message;
  EXPECT_FALSE(snapshot.value.dirty_monolithic_nodes.empty());
  EXPECT_FALSE(snapshot.value.edge_mappings.empty());

  const auto monolithic_node =
      std::find_if(snapshot.value.dirty_monolithic_nodes.begin(),
                   snapshot.value.dirty_monolithic_nodes.end(),
                   [](const DirtyMonolithicRegionSnapshot& region) {
                     return region.node.value == 2 &&
                            region.domain == DirtyDomain::HighPrecision;
                   });
  ASSERT_NE(monolithic_node, snapshot.value.dirty_monolithic_nodes.end());
  EXPECT_TRUE(monolithic_node->whole_output);
  EXPECT_EQ(monolithic_node->pixel_roi.x, 0);
  EXPECT_EQ(monolithic_node->pixel_roi.y, 0);
  EXPECT_EQ(monolithic_node->pixel_roi.width, 256);
  EXPECT_EQ(monolithic_node->pixel_roi.height, 128);

  const auto edge_mapping = std::find_if(
      snapshot.value.edge_mappings.begin(), snapshot.value.edge_mappings.end(),
      [](const DirtyEdgeMappingSnapshot& mapping) {
        return mapping.from_node.value == 1 && mapping.to_node.value == 2 &&
               mapping.domain == DirtyDomain::HighPrecision;
      });
  ASSERT_NE(edge_mapping, snapshot.value.edge_mappings.end());
  EXPECT_EQ(edge_mapping->direction, DirtyEdgeDirection::BackwardDemand);
  EXPECT_EQ(edge_mapping->from_roi.x, 64);
  EXPECT_EQ(edge_mapping->from_roi.y, 0);
  EXPECT_EQ(edge_mapping->from_roi.width, 128);
  EXPECT_EQ(edge_mapping->from_roi.height, 64);
  EXPECT_EQ(edge_mapping->to_roi.x, 64);
  EXPECT_EQ(edge_mapping->to_roi.y, 0);
  EXPECT_EQ(edge_mapping->to_roi.width, 64);
  EXPECT_EQ(edge_mapping->to_roi.height, 64);
}

TEST(EmbeddedHostAdapter, DirtySourceAndCacheControlsExposeFrontendStatus) {
  register_host_adapter_ops();
  ScopedTempDir temp("photospider_host_adapter_dirty_cache_test");
  auto host = create_embedded_host();
  ASSERT_NE(host, nullptr);

  const GraphSessionId session =
      load_test_graph(*host, temp.root(), "dirty_cache");

  HostComputeRequest request = make_compute_request(session);
  auto compute = host->compute(request);
  ASSERT_TRUE(compute.status.ok) << compute.status.message;

  const PixelRect roi{1, 1, 2, 2};
  auto begin = host->begin_dirty_source(session, NodeId{1},
                                        DirtyDomain::HighPrecision, roi);
  ASSERT_TRUE(begin.status.ok) << begin.status.message;
  ASSERT_FALSE(begin.value.sources.empty());
  EXPECT_EQ(begin.value.sources.front().node.value, 1);
  EXPECT_EQ(begin.value.sources.front().lifecycle,
            DirtySourceLifecycleState::Updating);

  auto update = host->update_dirty_source(
      session, NodeId{1}, DirtyDomain::HighPrecision, PixelRect{2, 2, 1, 1});
  ASSERT_TRUE(update.status.ok) << update.status.message;

  auto end =
      host->end_dirty_source(session, NodeId{1}, DirtyDomain::HighPrecision);
  ASSERT_TRUE(end.status.ok) << end.status.message;
  ASSERT_FALSE(end.value.sources.empty());
  EXPECT_EQ(end.value.sources.front().lifecycle,
            DirtySourceLifecycleState::Settled);

  auto cache_all = host->cache_all_nodes(session, "fp32");
  EXPECT_TRUE(cache_all.status.ok) << cache_all.status.message;

  auto sync = host->synchronize_disk_cache(session, "fp32");
  EXPECT_TRUE(sync.status.ok) << sync.status.message;

  auto clear_memory = host->clear_memory_cache(session);
  EXPECT_TRUE(clear_memory.status.ok) << clear_memory.status.message;

  auto clear_drive = host->clear_drive_cache(session);
  EXPECT_TRUE(clear_drive.status.ok) << clear_drive.status.message;

  auto clear_all = host->clear_cache(session);
  EXPECT_TRUE(clear_all.status.ok) << clear_all.status.message;

  auto free_memory = host->free_transient_memory(session);
  EXPECT_TRUE(free_memory.status.ok) << free_memory.status.message;
}

TEST(EmbeddedHostAdapter, DirtySourceFailuresPreserveStatusCodes) {
  register_host_adapter_ops();
  ScopedTempDir temp("photospider_host_adapter_dirty_status_test");
  auto host = create_embedded_host();
  ASSERT_NE(host, nullptr);

  const GraphSessionId session =
      load_test_graph(*host, temp.root(), "dirty_status");
  const PixelRect roi{1, 1, 2, 2};
  const PixelRect empty_roi{1, 1, 0, 2};

  auto missing_session_begin =
      host->begin_dirty_source(GraphSessionId{"missing_dirty_session"},
                               NodeId{1}, DirtyDomain::HighPrecision, roi);
  EXPECT_FALSE(missing_session_begin.status.ok);
  EXPECT_EQ(checked_graph_error_code(missing_session_begin.status),
            GraphErrc::NotFound);

  auto missing_session_update =
      host->update_dirty_source(GraphSessionId{"missing_dirty_session"},
                                NodeId{1}, DirtyDomain::HighPrecision, roi);
  EXPECT_FALSE(missing_session_update.status.ok);
  EXPECT_EQ(checked_graph_error_code(missing_session_update.status),
            GraphErrc::NotFound);

  auto missing_session_end =
      host->end_dirty_source(GraphSessionId{"missing_dirty_session"}, NodeId{1},
                             DirtyDomain::HighPrecision);
  EXPECT_FALSE(missing_session_end.status.ok);
  EXPECT_EQ(checked_graph_error_code(missing_session_end.status),
            GraphErrc::NotFound);

  auto missing_node_begin = host->begin_dirty_source(
      session, NodeId{99}, DirtyDomain::HighPrecision, roi);
  EXPECT_FALSE(missing_node_begin.status.ok);
  EXPECT_EQ(checked_graph_error_code(missing_node_begin.status),
            GraphErrc::NotFound);
  EXPECT_NE(missing_node_begin.status.message.find("Dirty source node 99"),
            std::string::npos);
  auto missing_node_begin_error = host->last_error(session);
  EXPECT_FALSE(missing_node_begin_error.ok);
  EXPECT_EQ(checked_graph_error_code(missing_node_begin_error),
            GraphErrc::NotFound);

  auto missing_node_update = host->update_dirty_source(
      session, NodeId{99}, DirtyDomain::HighPrecision, roi);
  EXPECT_FALSE(missing_node_update.status.ok);
  EXPECT_EQ(checked_graph_error_code(missing_node_update.status),
            GraphErrc::NotFound);
  EXPECT_NE(missing_node_update.status.message.find("Dirty source node 99"),
            std::string::npos);
  auto missing_node_update_error = host->last_error(session);
  EXPECT_FALSE(missing_node_update_error.ok);
  EXPECT_EQ(checked_graph_error_code(missing_node_update_error),
            GraphErrc::NotFound);

  auto missing_node_end =
      host->end_dirty_source(session, NodeId{99}, DirtyDomain::HighPrecision);
  EXPECT_FALSE(missing_node_end.status.ok);
  EXPECT_EQ(checked_graph_error_code(missing_node_end.status),
            GraphErrc::NotFound);
  EXPECT_NE(missing_node_end.status.message.find("Dirty source node 99"),
            std::string::npos);
  auto missing_node_end_error = host->last_error(session);
  EXPECT_FALSE(missing_node_end_error.ok);
  EXPECT_EQ(checked_graph_error_code(missing_node_end_error),
            GraphErrc::NotFound);

  auto invalid_begin = host->begin_dirty_source(
      session, NodeId{1}, DirtyDomain::HighPrecision, empty_roi);
  EXPECT_FALSE(invalid_begin.status.ok);
  EXPECT_EQ(checked_graph_error_code(invalid_begin.status),
            GraphErrc::InvalidParameter);
  EXPECT_NE(invalid_begin.status.message.find("Dirty source ROI is empty"),
            std::string::npos);
  auto invalid_begin_error = host->last_error(session);
  EXPECT_FALSE(invalid_begin_error.ok);
  EXPECT_EQ(checked_graph_error_code(invalid_begin_error),
            GraphErrc::InvalidParameter);

  auto invalid_update = host->update_dirty_source(
      session, NodeId{1}, DirtyDomain::HighPrecision, empty_roi);
  EXPECT_FALSE(invalid_update.status.ok);
  EXPECT_EQ(checked_graph_error_code(invalid_update.status),
            GraphErrc::InvalidParameter);
  EXPECT_NE(invalid_update.status.message.find("Dirty source ROI is empty"),
            std::string::npos);
  auto invalid_update_error = host->last_error(session);
  EXPECT_FALSE(invalid_update_error.ok);
  EXPECT_EQ(checked_graph_error_code(invalid_update_error),
            GraphErrc::InvalidParameter);
}

TEST(EmbeddedHostAdapter, SchedulerScanLoadAndPluginUnloadUseStatusValues) {
  register_host_adapter_ops();
  ScopedTempDir temp("photospider_host_adapter_scheduler_test");
  auto host = create_embedded_host();
  ASSERT_NE(host, nullptr);

  const auto scheduler_dir = temp.root() / "schedulers";
  std::filesystem::create_directories(scheduler_dir);

  auto scan = host->scheduler_scan({scheduler_dir.string()});
  ASSERT_TRUE(scan.status.ok) << scan.status.message;

  auto bad_load =
      host->scheduler_load((temp.root() / "missing_scheduler.so").string());
  EXPECT_FALSE(bad_load.status.ok);
  EXPECT_EQ(checked_graph_error_code(bad_load.status), GraphErrc::Io);

  auto loaded = host->scheduler_loaded_plugins();
  ASSERT_TRUE(loaded.status.ok) << loaded.status.message;

  const auto plugin_dir = lifecycle_plugin_dir();
  ASSERT_TRUE(std::filesystem::exists(plugin_dir))
      << "lifecycle op plugin directory was not built: " << plugin_dir;

  auto plugin_report = host->plugins_load_report({plugin_dir.string()});
  ASSERT_TRUE(plugin_report.status.ok) << plugin_report.status.message;
  EXPECT_EQ(plugin_report.value.loaded, 1);
  EXPECT_TRUE(plugin_report.value.errors.empty());
  EXPECT_TRUE(
      contains_string(plugin_report.value.new_op_keys, "plugin_lifecycle:op"));

  auto plugin_sources = host->ops_combined_sources();
  ASSERT_TRUE(plugin_sources.status.ok) << plugin_sources.status.message;
  ASSERT_NE(plugin_sources.value.find("plugin_lifecycle:op"),
            plugin_sources.value.end());

  auto unload_ops = host->plugins_unload_all();
  ASSERT_TRUE(unload_ops.status.ok) << unload_ops.status.message;
  EXPECT_GE(unload_ops.value, 1);

  auto plugin_sources_after_unload = host->ops_combined_sources();
  ASSERT_TRUE(plugin_sources_after_unload.status.ok)
      << plugin_sources_after_unload.status.message;
  EXPECT_EQ(plugin_sources_after_unload.value.count("plugin_lifecycle:op"), 0u);

  auto status_only_load = host->plugins_load({plugin_dir.string()});
  ASSERT_TRUE(status_only_load.status.ok) << status_only_load.status.message;

  auto unload_status_only = host->plugins_unload_all();
  ASSERT_TRUE(unload_status_only.status.ok)
      << unload_status_only.status.message;
  EXPECT_GE(unload_status_only.value, 1);

  auto empty_scan_plugins =
      host->plugins_load({(temp.root() / "missing_plugins").string()});
  EXPECT_TRUE(empty_scan_plugins.status.ok)
      << empty_scan_plugins.status.message;
}

/**
 * @brief Proves every embedded Host shares one process plugin owner.
 *
 * @throws Nothing when public Host status mapping and fixture IO succeed.
 * @note One Host loads P1, another loads P2 and executes it, both loading Hosts
 *       are destroyed, and a third Host performs the global unload. The
 *       surviving and newly created Hosts must observe the same state.
 */
TEST(EmbeddedHostAdapter,
     OperationPluginsAreProcessGlobalAcrossHostDestructionAndUnload) {
  ScopedTempDir temp("photospider_host_process_plugin_owner_test");
  auto observer = create_embedded_host();
  ASSERT_NE(observer, nullptr);
  ScopedHostPluginCleanup cleanup(*observer);

  const auto original_dir = lifecycle_plugin_dir();
  const auto replacement_dir = override_lifecycle_plugin_dir();
  ASSERT_TRUE(std::filesystem::exists(original_dir));
  ASSERT_TRUE(std::filesystem::exists(replacement_dir));

  auto original_loader = create_embedded_host();
  ASSERT_NE(original_loader, nullptr);
  const auto original_report =
      original_loader->plugins_load_report({original_dir.string()});
  ASSERT_TRUE(original_report.status.ok) << original_report.status.message;
  ASSERT_EQ(original_report.value.loaded, 1);

  auto observer_sources = observer->ops_sources();
  ASSERT_TRUE(observer_sources.status.ok) << observer_sources.status.message;
  ASSERT_EQ(observer_sources.value.count("plugin_lifecycle:op"), 1u);

  GraphLoadRequest load_request;
  load_request.session = GraphSessionId{"process_plugin_graph"};
  load_request.root_dir = (temp.root() / "sessions").string();
  load_request.yaml_path = (temp.root() / "source" / "plugin.yaml").string();
  load_request.cache_root_dir = (temp.root() / "cache").string();
  write_lifecycle_plugin_graph(load_request.yaml_path);
  const auto loaded = observer->load_graph(load_request);
  ASSERT_TRUE(loaded.status.ok) << loaded.status.message;

  HostComputeRequest request = make_compute_request(load_request.session);
  request.cache.force_recache = true;
  auto computed = observer->compute(request);
  ASSERT_TRUE(computed.status.ok) << computed.status.message;
  auto original_view = observer->inspect_node(load_request.session, NodeId{1});
  ASSERT_TRUE(original_view.status.ok) << original_view.status.message;
  ASSERT_TRUE(original_view.value.space.has_value());
  EXPECT_EQ(original_view.value.space->absolute_roi.width, 11);
  EXPECT_EQ(original_view.value.space->absolute_roi.height, 7);

  auto replacement_loader = create_embedded_host();
  ASSERT_NE(replacement_loader, nullptr);
  const auto replacement_report =
      replacement_loader->plugins_load_report({replacement_dir.string()});
  ASSERT_TRUE(replacement_report.status.ok)
      << replacement_report.status.message;
  ASSERT_EQ(replacement_report.value.loaded, 1);

  const auto repeated_seed = observer->seed_builtin_ops();
  ASSERT_TRUE(repeated_seed.status.ok) << repeated_seed.status.message;
  computed = observer->compute(request);
  ASSERT_TRUE(computed.status.ok) << computed.status.message;
  auto replacement_view =
      observer->inspect_node(load_request.session, NodeId{1});
  ASSERT_TRUE(replacement_view.status.ok) << replacement_view.status.message;
  ASSERT_TRUE(replacement_view.value.space.has_value());
  EXPECT_EQ(replacement_view.value.space->absolute_roi.width, 22);
  EXPECT_EQ(replacement_view.value.space->absolute_roi.height, 9);

  original_loader.reset();
  replacement_loader.reset();
  computed = observer->compute(request);
  ASSERT_TRUE(computed.status.ok) << computed.status.message;
  auto after_loader_destruction =
      observer->inspect_node(load_request.session, NodeId{1});
  ASSERT_TRUE(after_loader_destruction.status.ok)
      << after_loader_destruction.status.message;
  ASSERT_TRUE(after_loader_destruction.value.space.has_value());
  EXPECT_EQ(after_loader_destruction.value.space->absolute_roi.width, 22);
  EXPECT_EQ(after_loader_destruction.value.space->absolute_roi.height, 9);

  auto unloading_host = create_embedded_host();
  ASSERT_NE(unloading_host, nullptr);
  const auto unloaded = unloading_host->plugins_unload_all();
  ASSERT_TRUE(unloaded.status.ok) << unloaded.status.message;
  EXPECT_EQ(unloaded.value, 2);

  observer_sources = observer->ops_sources();
  ASSERT_TRUE(observer_sources.status.ok) << observer_sources.status.message;
  EXPECT_EQ(observer_sources.value.count("plugin_lifecycle:op"), 0u);
  const auto unloading_sources = unloading_host->ops_sources();
  ASSERT_TRUE(unloading_sources.status.ok) << unloading_sources.status.message;
  EXPECT_EQ(unloading_sources.value.count("plugin_lifecycle:op"), 0u);

  auto fresh_host = create_embedded_host();
  ASSERT_NE(fresh_host, nullptr);
  const auto fresh_sources = fresh_host->ops_sources();
  ASSERT_TRUE(fresh_sources.status.ok) << fresh_sources.status.message;
  EXPECT_EQ(fresh_sources.value.count("plugin_lifecycle:op"), 0u);
}

}  // namespace
}  // namespace ps
