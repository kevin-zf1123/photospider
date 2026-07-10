#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

#include "kernel/ops.hpp"
#include "kernel/plugin_manager.hpp"
#if defined(PHOTOSPIDER_INTERNAL_BAD_ALLOC_TESTING)
#include "kernel/op_registry_test_access.hpp"
#include "kernel/plugin_manage_module/plugin_loader_test_access.hpp"
#endif
#include "node.hpp"      // NOLINT(build/include_subdir)
#include "ps_types.hpp"  // NOLINT(build/include_subdir)

namespace plugin_cleanup_allocation_probe {

/** @brief Disabled current-thread allocation failure sentinel. */
constexpr std::int64_t kDisabled = -1;
/** @brief Zero-based allocation countdown for the current test thread. */
thread_local std::int64_t countdown = kDisabled;
/** @brief Whether an allocation was rejected after the most recent arm. */
thread_local bool fired = false;

/** @brief Arms one allocation failure at `allocation_index`. */
void arm(std::int64_t allocation_index) noexcept {
  countdown = allocation_index;
  fired = false;
}

/** @brief Disarms current-thread allocation injection. */
void disarm() noexcept {
  countdown = kDisabled;
}

/** @brief Returns whether the armed failure has fired. */
bool did_fire() noexcept {
  return fired;
}

/**
 * @brief Rejects the selected allocation.
 * @return Nothing when allocation may continue.
 * @throws std::bad_alloc at the armed allocation index.
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

}  // namespace plugin_cleanup_allocation_probe

/**
 * @brief Test-executable allocator used to audit no-allocation plugin cleanup.
 * @param size Requested allocation size.
 * @return malloc-backed storage.
 * @throws std::bad_alloc when injected or malloc fails.
 */
void* operator new(std::size_t size) {
  plugin_cleanup_allocation_probe::maybe_fail();
  if (void* memory = std::malloc(size == 0 ? 1 : size)) {
    return memory;
  }
  throw std::bad_alloc{};
}

/** @copydoc operator new(std::size_t) */
void* operator new[](std::size_t size) {
  return ::operator new(size);
}
/** @brief Releases scalar storage allocated by the test allocator. */
void operator delete(void* memory) noexcept {
  std::free(memory);
}
/** @brief Releases array storage allocated by the test allocator. */
void operator delete[](void* memory) noexcept {
  std::free(memory);
}
/** @brief Releases sized scalar storage allocated by the test allocator. */
void operator delete(void* memory, std::size_t) noexcept {
  std::free(memory);
}
/** @brief Releases sized array storage allocated by the test allocator. */
void operator delete[](void* memory, std::size_t) noexcept {
  std::free(memory);
}

namespace ps {
namespace {

constexpr const char* kLifecycleType = "plugin_lifecycle";
constexpr const char* kLifecycleSubtype = "op";
constexpr const char* kLifecycleKey = "plugin_lifecycle:op";
constexpr const char* kLifecycleTraceEnvironment = "PS_LIFECYCLE_PLUGIN_TRACE";
constexpr const char* kLifecycleThrowEnvironment =
    "PS_LIFECYCLE_PLUGIN_REGISTRAR_THROW";  // NOLINT(whitespace/indent_namespace)
constexpr const char* kLifecycleCallbackReleaseEnvironment =
    "PS_LIFECYCLE_PLUGIN_CALLBACK_RELEASE_FILE";  // NOLINT(whitespace/indent_namespace)
constexpr const char* kLifecycleRegistrarReleaseEnvironment =
    "PS_LIFECYCLE_PLUGIN_REGISTRAR_RELEASE_FILE";  // NOLINT(whitespace/indent_namespace)
constexpr const char* kLifecycleResultProbeEnvironment =
    "PS_LIFECYCLE_PLUGIN_RESULT_PROBE";  // NOLINT(whitespace/indent_namespace)
constexpr const char* kLifecycleDeviceRegistrarEnvironment =
    "PS_LIFECYCLE_PLUGIN_REGISTER_DEVICES";  // NOLINT(whitespace/indent_namespace)

#ifndef PS_STANDARD_OP_PLUGIN_DIR
#define PS_STANDARD_OP_PLUGIN_DIR "build/plugins"
#endif

#ifndef PS_TEST_OP_PLUGIN_DIR
#define PS_TEST_OP_PLUGIN_DIR "build/test_plugins"
#endif

/**
 * @brief Returns the CMake output root for test-only operation plugins.
 *
 * @return Filesystem path injected by CMake for this test target, or the legacy
 * `build/test_plugins` fallback when the source is compiled outside CMake.
 * @throws std::bad_alloc from path string construction.
 * @note The value is an absolute path in normal CMake builds, which lets the
 * test run from any working directory and from nested build trees such as
 * `build/ci`.
 */
std::filesystem::path test_op_plugin_dir() {
  return std::filesystem::path(PS_TEST_OP_PLUGIN_DIR);
}

/**
 * @brief Returns the CMake output directory for standard operation plugins.
 *
 * @return Filesystem path injected by CMake for standard plugin shared
 * libraries, or the legacy `build/plugins` fallback outside CMake.
 * @throws std::bad_alloc from path string construction.
 * @note Plugin manager lifecycle tests intentionally consume the build output
 * directory instead of a source-relative `build/` path so CI jobs can choose
 * any binary directory.
 */
std::filesystem::path standard_plugin_dir() {
  return std::filesystem::path(PS_STANDARD_OP_PLUGIN_DIR);
}

/**
 * @brief Returns the build output path for the lifecycle op plugin fixture.
 *
 * @return Platform-specific shared library path under `test_op_plugin_dir()`.
 * @throws std::bad_alloc from path string construction.
 * @note The plugin directory comes from the test target's CMake definitions so
 * the fixture follows the active binary directory rather than a hard-coded
 * `build/` tree.
 */
std::filesystem::path lifecycle_plugin_path() {
  const std::filesystem::path dir = test_op_plugin_dir() / "lifecycle";
#if defined(_WIN32)
  return dir / "lifecycle_op_plugin.dll";
#elif defined(__APPLE__)
  return dir / "liblifecycle_op_plugin.dylib";
#else
  return dir / "liblifecycle_op_plugin.so";
#endif
}

/**
 * @brief Returns the build output path for the replacement lifecycle plugin.
 *
 * @return Platform-specific shared library path under `test_op_plugin_dir()`.
 * @throws std::bad_alloc from path string construction.
 * @note The replacement plugin intentionally registers the same operation key
 * as `lifecycle_op_plugin`, and its directory is injected by CMake for the
 * active build tree.
 */
std::filesystem::path override_lifecycle_plugin_path() {
  const std::filesystem::path dir = test_op_plugin_dir() / "override";
#if defined(_WIN32)
  return dir / "override_lifecycle_op_plugin.dll";
#elif defined(__APPLE__)
  return dir / "liboverride_lifecycle_op_plugin.dylib";
#else
  return dir / "liboverride_lifecycle_op_plugin.so";
#endif
}

/**
 * @brief Formats plugin load errors for assertion messages.
 *
 * @param errors Structured plugin load errors returned by the manager.
 * @return Single-line summary suitable for GTest failure output.
 * @throws std::bad_alloc from string stream growth.
 * @note The helper keeps assertions readable without changing loader behavior.
 */
std::string describe_errors(const std::vector<PluginLoadError>& errors) {
  std::ostringstream out;
  for (const auto& error : errors) {
    out << "[" << error.path << "] " << error.message << "; ";
  }
  return out.str();
}

/**
 * @brief Checks whether a load result reports the lifecycle operation key.
 *
 * @param result Plugin load result to inspect.
 * @return True when reported keys contain `plugin_lifecycle:op`.
 * @throws Nothing.
 * @note The lifecycle plugin registers through `impl_table_`, so this also
 * proves loader key discovery observes multi-implementation registrations.
 */
bool result_contains_lifecycle_key(const PluginLoadResult& result) {
  return std::find(result.new_op_keys.begin(), result.new_op_keys.end(),
                   kLifecycleKey) != result.new_op_keys.end();
}

/**
 * @brief Sets one process environment variable for a bounded fixture load.
 *
 * @note Destruction restores the prior value or removes the key. Tests are
 * process-serial because environment variables are process-global.
 */
class ScopedEnvironmentVariable final {
 public:
  /**
   * @brief Installs a process environment value and preserves its predecessor.
   * @param name Environment key copied for the guard lifetime.
   * @param value New value visible to the dynamic plugin.
   * @throws std::runtime_error if the platform environment update fails.
   * @throws std::bad_alloc if key or previous-value storage cannot allocate.
   * @note Construction completes only after the replacement is installed.
   */
  ScopedEnvironmentVariable(const char* name, const std::string& value)
      : name_(name) {
    if (const char* previous = std::getenv(name)) {
      previous_ = std::string(previous);
    }
    set(value);
  }

  /**
   * @brief Restores the prior process environment without throwing.
   * @throws Nothing; platform restoration failures are suppressed.
   * @note The destructor never changes plugin/registry ownership state.
   */
  ~ScopedEnvironmentVariable() {
    try {
      if (previous_) {
        set(*previous_);
      } else {
        clear();
      }
    } catch (...) {
      // Test cleanup cannot safely surface an environment restoration failure.
    }
  }

  ScopedEnvironmentVariable(const ScopedEnvironmentVariable&) = delete;
  ScopedEnvironmentVariable& operator=(const ScopedEnvironmentVariable&) =
      delete;

 private:
  /**
   * @brief Replaces the owned environment key.
   *
   * @param value New value.
   * @return Nothing.
   * @throws std::runtime_error when the platform call fails.
   * @note The process-global key is the one copied during construction.
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
   *
   * @return Nothing.
   * @throws std::runtime_error when the platform call fails.
   * @note Removal affects process-global state and is used only by this guard.
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

  /** @brief Environment key owned for the guard lifetime. */
  std::string name_;
  /** @brief Previous value, or nullopt when the key was absent. */
  std::optional<std::string> previous_;
};

/**
 * @brief RAII owner for one current-thread plugin-cleanup allocation failure.
 * @note The scope owns no heap storage and always disarms before assertions.
 */
class ScopedPluginCleanupAllocationFailure final {
 public:
  /**
   * @brief Arms a zero-based allocation failure.
   * @param allocation_index Allocation to reject.
   * @throws Nothing.
   */
  explicit ScopedPluginCleanupAllocationFailure(
      std::int64_t allocation_index) noexcept {
    plugin_cleanup_allocation_probe::arm(allocation_index);
  }

  /** @brief Disarms allocation injection. */
  ~ScopedPluginCleanupAllocationFailure() {
    plugin_cleanup_allocation_probe::disarm();
  }

  ScopedPluginCleanupAllocationFailure(
      const ScopedPluginCleanupAllocationFailure&) = delete;
  ScopedPluginCleanupAllocationFailure& operator=(
      const ScopedPluginCleanupAllocationFailure&) = delete;
};

/**
 * @brief Owns one deterministic observer for the registry contention slow path.
 *
 * @throws Nothing.
 * @note The observer publishes a pointer to its inline atomic counter and
 * clears that pointer before the counter is destroyed. The surrounding test
 * must join every contending thread before this scope ends.
 */
class ScopedOpRegistryContentionCounter final {
 public:
  /**
   * @brief Installs this scope's inline counter as the active test observer.
   *
   * @throws Nothing.
   * @note Installation allocates nothing and begins from a zero observation
   *       count.
   */
  ScopedOpRegistryContentionCounter() noexcept {
    testing::set_op_registry_contention_counter(&counter_);
  }

  /**
   * @brief Clears the observer before its inline counter leaves scope.
   *
   * @throws Nothing.
   * @note All threads that may enter the registry lock must already be joined.
   *       Clearing is an atomic pointer publication, not a synchronization
   * join.
   */
  ~ScopedOpRegistryContentionCounter() noexcept {
    testing::set_op_registry_contention_counter(nullptr);
  }

  /**
   * @brief Prevents two scopes from claiming one installed counter lifetime.
   *
   * @param other Scope that remains the unique installed observer.
   * @note Deletion prevents either destructor from clearing another counter.
   */
  ScopedOpRegistryContentionCounter(
      const ScopedOpRegistryContentionCounter& other) = delete;

  /**
   * @brief Prevents retargeting a live installed observer.
   *
   * @param other Scope that must retain its own counter lifetime.
   * @return No value because this operation is deleted.
   * @note The installed pointer remains paired with lexical scope lifetime.
   */
  ScopedOpRegistryContentionCounter& operator=(
      const ScopedOpRegistryContentionCounter& other) = delete;

  /**
   * @brief Prevents moving the installed counter to a different address.
   *
   * @param other Scope whose inline counter address remains installed.
   * @note Deletion keeps the published observer pointer stable.
   */
  ScopedOpRegistryContentionCounter(ScopedOpRegistryContentionCounter&& other) =
      delete;

  /**
   * @brief Prevents transferring an installed observer into another scope.
   *
   * @param other Scope that retains its own observer lifetime.
   * @return No value because this operation is deleted.
   * @note The active pointer cannot be retargeted through move assignment.
   */
  ScopedOpRegistryContentionCounter& operator=(
      ScopedOpRegistryContentionCounter&& other) = delete;

  /**
   * @brief Waits for a lock attempt to observe a different thread as owner.
   *
   * @param timeout Maximum diagnostic bound before reporting no observation.
   * @return True only after the lock slow path increments the counter.
   * @throws Nothing; polling uses atomic loads and thread yields only.
   * @note Time does not establish correctness: a successful return is backed by
   *       the compare-exchange path observing a real non-null foreign owner.
   */
  bool wait_until_observed(std::chrono::milliseconds timeout) const noexcept {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (counter_.load(std::memory_order_acquire) == 0) {
      if (std::chrono::steady_clock::now() >= deadline) {
        return false;
      }
      std::this_thread::yield();
    }
    return true;
  }

  /**
   * @brief Returns the number of lock attempts that entered contention.
   *
   * @return Current observation count.
   * @throws Nothing.
   * @note Each acquisition reports at most once even if it yields repeatedly.
   */
  std::uint64_t observed_count() const noexcept {
    return counter_.load(std::memory_order_acquire);
  }

 private:
  /** @brief Inline counter whose address remains stable for this scope. */
  std::atomic<std::uint64_t> counter_{0};
};

/**
 * @brief Returns a unique trace path for one lifecycle transaction scenario.
 *
 * @param label Stable scenario label used in the filename.
 * @return Path in GTest's temporary directory.
 * @throws std::bad_alloc from path/string construction.
 * @note Existing files are removed by the caller before installing the path in
 * the dynamic plugin's environment.
 */
std::filesystem::path lifecycle_trace_path(const std::string& label) {
  static std::atomic<unsigned int> sequence{0};
  return std::filesystem::path(::testing::TempDir()) /
         ("photospider-lifecycle-" + label + "-" +
          std::to_string(sequence.fetch_add(1, std::memory_order_relaxed)) +
          ".log");
}

/**
 * @brief Reads newline-delimited lifecycle events emitted by the real plugin.
 *
 * @param path Trace file selected through the fixture environment.
 * @return Events in actual destructor/registrar order.
 * @throws std::bad_alloc if line/vector storage cannot allocate.
 * @note A missing file yields an empty vector so the caller reports a focused
 * assertion rather than an unrelated stream exception.
 */
std::vector<std::string> read_lifecycle_trace(
    const std::filesystem::path& path) {
  std::vector<std::string> events;
  std::ifstream input(path);
  std::string line;
  while (std::getline(input, line)) {
    events.push_back(line);
  }
  return events;
}

/**
 * @brief Waits until a lifecycle trace contains one event.
 *
 * @param path Trace file written by the lifecycle plugin.
 * @param event Event label to observe.
 * @param timeout Maximum wait duration.
 * @return True when the event appears before timeout.
 * @throws std::bad_alloc if trace parsing or event copying exhausts memory.
 * @note Polling is bounded and used only to synchronize a real callback with
 *       explicit unload; it does not infer callback completion from timing.
 */
bool wait_for_lifecycle_event(const std::filesystem::path& path,
                              const std::string& event,
                              std::chrono::milliseconds timeout) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    const auto events = read_lifecycle_trace(path);
    if (std::find(events.begin(), events.end(), event) != events.end()) {
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  return false;
}

/**
 * @brief Checks global registry key presence without copying plugin callbacks.
 *
 * @return True when the lifecycle key remains in the canonical key inventory.
 * @throws std::bad_alloc if registry key enumeration allocation fails.
 * @note Avoiding `resolve_for_intent` is intentional: a defective loader may
 * already have unloaded the library behind the stale callable.
 */
bool lifecycle_key_is_registered() {
  const auto keys = OpRegistry::instance().get_combined_keys();
  return std::find(keys.begin(), keys.end(), kLifecycleKey) != keys.end();
}

/**
 * @brief Consumes the currently armed allocation failure after cleanup.
 *
 * @return True when the probe still rejected this explicit allocation.
 * @throws Nothing; `std::bad_alloc` is converted to the boolean result.
 * @note A true result proves the preceding cleanup performed no C++ dynamic
 * allocation and therefore did not consume the zero-count failpoint.
 */
bool consume_armed_cleanup_allocation_failure() noexcept {
  try {
    void* memory = ::operator new(1);
    ::operator delete(memory);
  } catch (const std::bad_alloc&) {
    return true;
  }
  return false;
}

/**
 * @brief Installs a host-owned callback at the lifecycle plugin's target key.
 *
 * @return Nothing.
 * @throws std::bad_alloc if registry callback or metadata storage cannot grow.
 * @note A failed candidate load must preserve this exact active callback,
 * proving strong guarantee for replacement as well as insertion registration.
 */
void register_host_lifecycle_sentinel() {
  OpMetadata metadata;
  metadata.cost_score = 99;
  OpRegistry::instance().register_op_hp_monolithic(
      kLifecycleType, kLifecycleSubtype,
      [](const Node&, const std::vector<const NodeOutput*>&) {
        NodeOutput output;
        output.debug.compute_device = "HOST_LIFECYCLE_SENTINEL";
        return output;
      },
      metadata);
}

/**
 * @brief Replaces the lifecycle operation through the direct registry API.
 *
 * @return Nothing.
 * @throws std::bad_alloc if callback or registry storage cannot allocate.
 * @note This intentionally bypasses `PluginManager` and mutates the exact key
 *       published by the plugin. The new callback must own the HP callback and
 *       metadata slots while plugin-owned propagation slots remain separable.
 */
void register_direct_lifecycle_replacement() {
  OpRegistry::instance().register_op_hp_monolithic(
      kLifecycleType, kLifecycleSubtype,
      [](const Node&, const std::vector<const NodeOutput*>&) {
        NodeOutput output;
        output.debug.compute_device = "DIRECT_LIFECYCLE_MUTATION";
        return output;
      });
}

/**
 * @brief Appends one host-owned device implementation at the lifecycle key.
 *
 * @return Nothing.
 * @throws std::bad_alloc if callback, metadata, or registry storage cannot
 *         allocate.
 * @note The distinct device and cost identify this entry after plugin-owned
 *       device elements have been compacted during unload.
 */
void register_direct_lifecycle_device() {
  OpMetadata metadata;
  metadata.cost_score = 41;
  OpRegistry::instance().register_impl(
      kLifecycleType, kLifecycleSubtype, Device::ASIC_NPU,
      [](const Node&, const std::vector<const NodeOutput*>&) {
        NodeOutput output;
        output.debug.compute_device = "DIRECT_DEVICE_MONOLITHIC";
        return output;
      },
      metadata);
}

/**
 * @brief Checks for one device implementation with exact shape and cost.
 *
 * @param device Required device label.
 * @param tiled Required callback shape.
 * @param cost_score Required deterministic fixture cost.
 * @return True when exactly matching active implementation state is present.
 * @throws std::bad_alloc if registry callback snapshots cannot be copied.
 * @note The copied callback vector is destroyed before this helper returns, so
 *       it never delays a later library-unload assertion.
 */
bool has_lifecycle_device_implementation(Device device, bool tiled,
                                         int cost_score) {
  const auto implementations = OpRegistry::instance().get_all_implementations(
      kLifecycleType, kLifecycleSubtype);
  return std::any_of(
      implementations.begin(), implementations.end(),
      [device, tiled, cost_score](const OpImplementation& implementation) {
        return implementation.metadata.device_preference == device &&
               implementation.is_tiled() == tiled &&
               implementation.metadata.cost_score == cost_score;
      });
}

/**
 * @brief Asserts parallel device implementation and revision-vector state.
 *
 * @param expected_count Expected live count in both vectors.
 * @return Nothing.
 * @throws GTest assertion failures when counts diverge or a token is zero.
 * @note The BUILD_TESTING seam returns counts only and does not copy callbacks.
 */
void expect_lifecycle_device_ownership_alignment(std::size_t expected_count) {
  const auto inspection =
      testing::inspect_op_registry_device_ownership_for_testing(
          OpRegistry::instance(), kLifecycleKey);
  EXPECT_EQ(inspection.implementation_count, expected_count);
  EXPECT_EQ(inspection.revision_count, expected_count);
  EXPECT_TRUE(inspection.all_revisions_nonzero);
}

/**
 * @brief Records whether callback-state destruction occurs under registry lock.
 *
 * @throws Nothing; read failures during the reentrant probe are converted to a
 *         false completion signal.
 * @note The final callback copy owns this object. Its destructor both inspects
 *       lock ownership and performs a real registry read, making replacement
 *       and unregister retirement order deterministic without timeout guesses.
 */
struct ReentrantRegistryCallbackProbe {
  /** @brief Registry re-entered during final callback-state destruction. */
  OpRegistry* registry = nullptr;
  /** @brief Set after the destructor itself has run. */
  std::atomic<bool>* destroyed = nullptr;
  /** @brief Captures current-thread lock ownership at destructor entry. */
  std::atomic<bool>* destroyed_under_lock = nullptr;
  /** @brief Set only when the reentrant registry read completes. */
  std::atomic<bool>* reentered = nullptr;

  /**
   * @brief Inspects and re-enters the registry during callback retirement.
   * @throws Nothing; allocation failures from the diagnostic read are caught.
   * @note Lock ownership is observed before the reentrant read.
   */
  ~ReentrantRegistryCallbackProbe() {
    destroyed_under_lock->store(
        testing::op_registry_lock_held_by_current_thread_for_testing(*registry),
        std::memory_order_release);
    try {
      (void)registry->get_keys();
      reentered->store(true, std::memory_order_release);
    } catch (...) {
      reentered->store(false, std::memory_order_release);
    }
    destroyed->store(true, std::memory_order_release);
  }
};

/**
 * @brief Creates one callback whose final state destructor re-enters registry.
 *
 * @param registry Registry inspected by the probe.
 * @param destroyed Output flag for final probe destruction.
 * @param destroyed_under_lock Output flag for lock ownership at destruction.
 * @param reentered Output flag for successful reentrant registry inspection.
 * @return Monolithic callback owning the sole shared probe reference.
 * @throws std::bad_alloc if shared state or callback storage cannot allocate.
 * @note The caller must move the returned callback directly into the registry
 *       so no external copy extends the probe lifetime.
 */
MonolithicOpFunc make_reentrant_registry_callback(
    OpRegistry& registry, std::atomic<bool>& destroyed,
    std::atomic<bool>& destroyed_under_lock, std::atomic<bool>& reentered) {
  auto probe = std::make_shared<ReentrantRegistryCallbackProbe>();
  probe->registry = &registry;
  probe->destroyed = &destroyed;
  probe->destroyed_under_lock = &destroyed_under_lock;
  probe->reentered = &reentered;
  return [probe = std::move(probe)](const Node&,
                                    const std::vector<const NodeOutput*>&) {
    (void)probe;
    return NodeOutput{};
  };
}

/**
 * @brief Executes the currently registered lifecycle operation marker.
 *
 * @return `NodeOutput::debug.compute_device` emitted by the resolved
 * implementation.
 * @throws GTest assertion failures when the key is missing or not monolithic.
 * @note Invoking the callback proves the active registry entry points to the
 * expected loaded library implementation.
 */
std::string current_lifecycle_compute_device() {
  auto op = OpRegistry::instance().resolve_for_intent(
      kLifecycleType, kLifecycleSubtype, ComputeIntent::GlobalHighPrecision);
  EXPECT_TRUE(op.has_value());
  if (!op || !std::holds_alternative<MonolithicOpFunc>(*op)) {
    return {};
  }

  Node node;
  node.id = 1;
  node.type = kLifecycleType;
  node.subtype = kLifecycleSubtype;
  const std::vector<const NodeOutput*> inputs;
  NodeOutput output = std::get<MonolithicOpFunc>(*op)(node, inputs);
  return output.debug.compute_device;
}

/**
 * @brief Invokes the active lifecycle plugin and returns its complete result.
 *
 * @return Plugin-produced output, including its host-attached library lease.
 * @throws std::bad_alloc if callback snapshot or output construction allocates.
 * @note The resolved callback copy is destroyed before this helper returns, so
 *       after explicit manager unload the returned value can be the final
 *       dynamic-library owner exercised by assignment tests.
 */
NodeOutput invoke_lifecycle_output() {
  auto op = OpRegistry::instance().resolve_for_intent(
      kLifecycleType, kLifecycleSubtype, ComputeIntent::GlobalHighPrecision);
  EXPECT_TRUE(op.has_value());
  if (!op || !std::holds_alternative<MonolithicOpFunc>(*op)) {
    return {};
  }

  Node node;
  node.id = 1;
  node.type = kLifecycleType;
  node.subtype = kLifecycleSubtype;
  return std::get<MonolithicOpFunc>(*op)(node, {});
}

/**
 * @brief Checks whether a load result reports a standard plugin operation key.
 *
 * @param result Plugin load result to inspect.
 * @param key Canonical `type:subtype` operation key.
 * @return True when reported keys contain `key`.
 * @throws Nothing.
 * @note Standard plugins are loaded from `standard_plugin_dir()`, and this
 * helper keeps test assertions independent from platform-specific shared
 * library suffixes.
 */
bool result_contains_key(const PluginLoadResult& result,
                         const std::string& key) {
  return std::find(result.new_op_keys.begin(), result.new_op_keys.end(), key) !=
         result.new_op_keys.end();
}

/**
 * @brief Asserts that one operation uses explicit dirty and forward contracts.
 *
 * @param type Operation type registered in OpRegistry.
 * @param subtype Operation subtype registered in OpRegistry.
 * @return Nothing.
 * @throws GTest assertion failures when either contract is missing.
 * @note The helper checks contract status rather than callback return values so
 * side-effecting plugins such as `io:save` can document explicit pass-through
 * behavior without pretending to produce an image output.
 */
void expect_explicit_roi_contract(const std::string& type,
                                  const std::string& subtype) {
  auto& registry = OpRegistry::instance();
  EXPECT_EQ(registry.dirty_propagation_contract_status(type, subtype),
            PropagationContractStatus::Explicit)
      << type << ":" << subtype << " dirty ROI contract";
  EXPECT_EQ(registry.forward_propagation_contract_status(type, subtype),
            PropagationContractStatus::Explicit)
      << type << ":" << subtype << " forward ROI contract";
}

/**
 * @brief Test fixture that isolates the global operation registry key.
 *
 * Each test removes the lifecycle key before and after execution because
 * `OpRegistry` and `PluginManager` are process-global singletons shared by all
 * embedded Hosts.
 */
class PluginManagerLifecycleTest : public ::testing::Test {
 protected:
  /**
   * @brief Removes stale lifecycle operation state before each test.
   *
   * @return Nothing.
   * @throws Nothing under current registry behavior.
   * @note This protects the tests from previous failed runs in the same
   * process.
   */
  void SetUp() override {
    (void)PluginManager::process_instance().unload_all_plugins();
    OpRegistry::instance().unregister_key(kLifecycleKey);
  }

  /**
   * @brief Removes lifecycle operation state after each test.
   *
   * @return Nothing.
   * @throws Nothing under current registry behavior.
   * @note The fixture clears the unique process owner so failed assertions do
   *       not leak plugin state into the next test.
   */
  void TearDown() override {
    (void)PluginManager::process_instance().unload_all_plugins();
    OpRegistry::instance().unregister_key(kLifecycleKey);
  }
};

}  // namespace

TEST_F(PluginManagerLifecycleTest,
       LoadRetainsHandleAndUnloadRemovesMultiImplKey) {
  const auto plugin_path = lifecycle_plugin_path();
  ASSERT_TRUE(std::filesystem::exists(plugin_path))
      << "lifecycle op plugin was not built: " << plugin_path;

  auto& manager = PluginManager::process_instance();
  const auto result =
      manager.load_from_dirs_report({plugin_path.parent_path().string()});

  EXPECT_EQ(result.attempted, 1);
  EXPECT_EQ(result.loaded, 1) << describe_errors(result.errors);
  EXPECT_TRUE(result.errors.empty()) << describe_errors(result.errors);
  EXPECT_TRUE(result_contains_lifecycle_key(result));
  EXPECT_EQ(manager.loaded_plugin_count(), 1u);

  const std::string absolute_path =
      std::filesystem::absolute(plugin_path).string();
  const auto sources = manager.op_sources();
  auto source_it = sources.find(kLifecycleKey);
  ASSERT_NE(source_it, sources.end());
  EXPECT_EQ(source_it->second, absolute_path);

  EXPECT_TRUE(OpRegistry::instance()
                  .resolve_for_intent(kLifecycleType, kLifecycleSubtype,
                                      ComputeIntent::GlobalHighPrecision)
                  .has_value());

  EXPECT_EQ(manager.unload_by_plugin_path(absolute_path), 1);
  EXPECT_EQ(manager.loaded_plugin_count(), 0u);
  EXPECT_EQ(manager.op_sources().count(kLifecycleKey), 0u);
  EXPECT_FALSE(OpRegistry::instance()
                   .resolve_for_intent(kLifecycleType, kLifecycleSubtype,
                                       ComputeIntent::GlobalHighPrecision)
                   .has_value());
}

/**
 * @brief Exercises real device registrar callbacks beside one direct element.
 *
 * @throws Nothing when two load/unload cycles compact only plugin-owned device
 *         values and their parallel revisions.
 * @note Source inspection must report mixed ownership while loaded, then the
 *       direct implementation and its aligned token must survive final library
 *       release. The second cycle catches stale indices left by compaction.
 */
TEST_F(PluginManagerLifecycleTest,
       DeviceRegistrarMixedOwnershipCompactsParallelTokens) {
  const auto plugin_path = lifecycle_plugin_path();
  ASSERT_TRUE(std::filesystem::exists(plugin_path));
  const auto trace_path = lifecycle_trace_path("device-mixed-compaction");
  std::filesystem::remove(trace_path);
  ScopedEnvironmentVariable trace_environment(kLifecycleTraceEnvironment,
                                              trace_path.string());
  ScopedEnvironmentVariable device_environment(
      kLifecycleDeviceRegistrarEnvironment, "1");
  auto& manager = PluginManager::process_instance();

  register_direct_lifecycle_device();
  expect_lifecycle_device_ownership_alignment(1);
  ASSERT_TRUE(has_lifecycle_device_implementation(Device::ASIC_NPU, false, 41));

  for (int cycle = 0; cycle < 2; ++cycle) {
    SCOPED_TRACE(cycle);
    const auto result =
        manager.load_from_dirs_report({plugin_path.parent_path().string()});
    ASSERT_EQ(result.loaded, 1) << describe_errors(result.errors);
    ASSERT_TRUE(result.errors.empty()) << describe_errors(result.errors);
    EXPECT_EQ(manager.op_sources().at(kLifecycleKey), "mixed");
    expect_lifecycle_device_ownership_alignment(3);
    EXPECT_TRUE(
        has_lifecycle_device_implementation(Device::ASIC_NPU, false, 41));
    EXPECT_TRUE(
        has_lifecycle_device_implementation(Device::GPU_METAL, false, 3));
    EXPECT_TRUE(has_lifecycle_device_implementation(Device::GPU_CUDA, true, 4));

    EXPECT_EQ(manager.unload_all_plugins(), 1);
    EXPECT_EQ(manager.loaded_plugin_count(), 0u);
    EXPECT_EQ(manager.op_sources().at(kLifecycleKey), "built-in");
    expect_lifecycle_device_ownership_alignment(1);
    EXPECT_TRUE(
        has_lifecycle_device_implementation(Device::ASIC_NPU, false, 41));
    EXPECT_FALSE(
        has_lifecycle_device_implementation(Device::GPU_METAL, false, 3));
    EXPECT_FALSE(
        has_lifecycle_device_implementation(Device::GPU_CUDA, true, 4));
  }

  EXPECT_EQ(read_lifecycle_trace(trace_path),
            (std::vector<std::string>{"registrar_return", "callback_destroy",
                                      "library_unload", "registrar_return",
                                      "callback_destroy", "library_unload"}));
  std::filesystem::remove(trace_path);
}

/**
 * @brief Clears plugin device ownership before direct same-key re-registration.
 *
 * @throws Nothing when whole-key unregister removes both device values and
 *         revision tokens, and later plugin unload preserves the direct entry.
 * @note This sequence fails if old plugin revisions remain at the front of the
 *       ownership vector and are incorrectly applied to the new direct value.
 */
TEST_F(PluginManagerLifecycleTest,
       WholeKeyUnregisterClearsDeviceTokensBeforeDirectReregister) {
  const auto plugin_path = lifecycle_plugin_path();
  ASSERT_TRUE(std::filesystem::exists(plugin_path));
  const auto trace_path = lifecycle_trace_path("device-unregister-reregister");
  std::filesystem::remove(trace_path);
  ScopedEnvironmentVariable trace_environment(kLifecycleTraceEnvironment,
                                              trace_path.string());
  ScopedEnvironmentVariable device_environment(
      kLifecycleDeviceRegistrarEnvironment, "1");
  auto& manager = PluginManager::process_instance();
  const auto result =
      manager.load_from_dirs_report({plugin_path.parent_path().string()});
  ASSERT_EQ(result.loaded, 1) << describe_errors(result.errors);
  expect_lifecycle_device_ownership_alignment(2);

  EXPECT_TRUE(OpRegistry::instance().unregister_key(kLifecycleKey));
  expect_lifecycle_device_ownership_alignment(0);
  EXPECT_EQ(read_lifecycle_trace(trace_path),
            (std::vector<std::string>{"registrar_return", "callback_destroy"}));

  register_direct_lifecycle_device();
  expect_lifecycle_device_ownership_alignment(1);
  EXPECT_EQ(manager.op_sources().at(kLifecycleKey), "built-in");
  EXPECT_EQ(manager.unload_all_plugins(), 0);
  EXPECT_EQ(manager.loaded_plugin_count(), 0u);
  EXPECT_EQ(manager.op_sources().at(kLifecycleKey), "built-in");
  expect_lifecycle_device_ownership_alignment(1);
  EXPECT_TRUE(has_lifecycle_device_implementation(Device::ASIC_NPU, false, 41));
  EXPECT_FALSE(
      has_lifecycle_device_implementation(Device::GPU_METAL, false, 3));
  EXPECT_FALSE(has_lifecycle_device_implementation(Device::GPU_CUDA, true, 4));
  EXPECT_EQ(read_lifecycle_trace(trace_path),
            (std::vector<std::string>{"registrar_return", "callback_destroy",
                                      "library_unload"}));
  std::filesystem::remove(trace_path);
}

/**
 * @brief Proves direct replacement and unregister retire callbacks outside
 * lock.
 *
 * @throws Nothing when both callback-state destructors observe no registry lock
 *         and complete a real reentrant registry read.
 * @note The test reads the actual current-thread owner token; it does not treat
 *       a timeout or absence of deadlock as evidence of correct lock ordering.
 */
TEST_F(PluginManagerLifecycleTest,
       DirectCallbackReplacementAndUnregisterDestroyOutsideRegistryLock) {
  constexpr const char* kType = "registry_retirement";
  constexpr const char* kSubtype = "reentrant_callback";
  const std::string key = make_key(kType, kSubtype);
  auto& registry = OpRegistry::instance();
  registry.unregister_key(key);

  std::atomic<bool> replacement_destroyed{false};
  std::atomic<bool> replacement_destroyed_under_lock{false};
  std::atomic<bool> replacement_reentered{false};
  registry.register_op_hp_monolithic(
      kType, kSubtype,
      make_reentrant_registry_callback(registry, replacement_destroyed,
                                       replacement_destroyed_under_lock,
                                       replacement_reentered));
  registry.register_op_hp_monolithic(
      kType, kSubtype, [](const Node&, const std::vector<const NodeOutput*>&) {
        return NodeOutput{};
      });
  EXPECT_TRUE(replacement_destroyed.load(std::memory_order_acquire));
  EXPECT_FALSE(
      replacement_destroyed_under_lock.load(std::memory_order_acquire));
  EXPECT_TRUE(replacement_reentered.load(std::memory_order_acquire));

  std::atomic<bool> unregister_destroyed{false};
  std::atomic<bool> unregister_destroyed_under_lock{false};
  std::atomic<bool> unregister_reentered{false};
  registry.register_op_hp_monolithic(
      kType, kSubtype,
      make_reentrant_registry_callback(registry, unregister_destroyed,
                                       unregister_destroyed_under_lock,
                                       unregister_reentered));
  EXPECT_TRUE(registry.unregister_key(key));
  EXPECT_TRUE(unregister_destroyed.load(std::memory_order_acquire));
  EXPECT_FALSE(unregister_destroyed_under_lock.load(std::memory_order_acquire));
  EXPECT_TRUE(unregister_reentered.load(std::memory_order_acquire));
}

TEST_F(PluginManagerLifecycleTest, UnloadAllPluginsReleasesRetainedHandles) {
  const auto plugin_path = lifecycle_plugin_path();
  ASSERT_TRUE(std::filesystem::exists(plugin_path))
      << "lifecycle op plugin was not built: " << plugin_path;

  auto& manager = PluginManager::process_instance();
  const auto result =
      manager.load_from_dirs_report({plugin_path.parent_path().string()});

  ASSERT_EQ(result.loaded, 1) << describe_errors(result.errors);
  ASSERT_TRUE(result.errors.empty()) << describe_errors(result.errors);
  ASSERT_EQ(manager.loaded_plugin_count(), 1u);

  EXPECT_EQ(manager.unload_all_plugins(), 1);
  EXPECT_EQ(manager.loaded_plugin_count(), 0u);
  EXPECT_EQ(manager.op_sources().count(kLifecycleKey), 0u);
  EXPECT_FALSE(OpRegistry::instance()
                   .resolve_for_intent(kLifecycleType, kLifecycleSubtype,
                                       ComputeIntent::GlobalHighPrecision)
                   .has_value());
}

TEST_F(PluginManagerLifecycleTest,
       UnloadReplacementPluginRestoresOverwrittenOperation) {
  const auto plugin_path = lifecycle_plugin_path();
  const auto override_path = override_lifecycle_plugin_path();
  ASSERT_TRUE(std::filesystem::exists(plugin_path))
      << "lifecycle op plugin was not built: " << plugin_path;
  ASSERT_TRUE(std::filesystem::exists(override_path))
      << "override lifecycle op plugin was not built: " << override_path;

  auto& manager = PluginManager::process_instance();
  const auto first_result =
      manager.load_from_dirs_report({plugin_path.parent_path().string()});
  ASSERT_EQ(first_result.loaded, 1) << describe_errors(first_result.errors);
  ASSERT_TRUE(first_result.errors.empty())
      << describe_errors(first_result.errors);
  EXPECT_TRUE(result_contains_lifecycle_key(first_result));
  EXPECT_EQ(current_lifecycle_compute_device(), "PLUGIN_LIFECYCLE_TEST");

  const std::string original_path =
      std::filesystem::absolute(plugin_path).string();
  ASSERT_EQ(manager.op_sources().at(kLifecycleKey), original_path);

  const auto replacement_result =
      manager.load_from_dirs_report({override_path.parent_path().string()});
  ASSERT_EQ(replacement_result.loaded, 1)
      << describe_errors(replacement_result.errors);
  ASSERT_TRUE(replacement_result.errors.empty())
      << describe_errors(replacement_result.errors);
  EXPECT_TRUE(result_contains_lifecycle_key(replacement_result));
  ASSERT_EQ(manager.loaded_plugin_count(), 2u);
  EXPECT_EQ(current_lifecycle_compute_device(), "PLUGIN_OVERRIDE_TEST");

  const std::string replacement_path =
      std::filesystem::absolute(override_path).string();
  ASSERT_EQ(manager.op_sources().at(kLifecycleKey), replacement_path);

  EXPECT_EQ(manager.unload_by_plugin_path(replacement_path), 1);
  EXPECT_EQ(manager.loaded_plugin_count(), 1u);
  ASSERT_EQ(manager.op_sources().at(kLifecycleKey), original_path);
  EXPECT_EQ(current_lifecycle_compute_device(), "PLUGIN_LIFECYCLE_TEST");

  EXPECT_EQ(manager.unload_by_plugin_path(original_path), 1);
  EXPECT_EQ(manager.loaded_plugin_count(), 0u);
  EXPECT_EQ(manager.op_sources().count(kLifecycleKey), 0u);
  EXPECT_FALSE(OpRegistry::instance()
                   .resolve_for_intent(kLifecycleType, kLifecycleSubtype,
                                       ComputeIntent::GlobalHighPrecision)
                   .has_value());
}

/**
 * @brief Proves unload-all unwinds a two-plugin override chain in load order.
 * @throws Nothing when the built-in callback/source are restored exactly.
 * @note Plugin filenames intentionally sort differently from dependency order.
 */
TEST_F(PluginManagerLifecycleTest,
       UnloadAllRestoresBuiltinThroughReverseLoadOrder) {
  const auto plugin_path = lifecycle_plugin_path();
  const auto override_path = override_lifecycle_plugin_path();
  ASSERT_TRUE(std::filesystem::exists(plugin_path));
  ASSERT_TRUE(std::filesystem::exists(override_path));

  register_host_lifecycle_sentinel();
  auto& manager = PluginManager::process_instance();
  manager.seed_builtins_from_registry();

  const auto first_result =
      manager.load_from_dirs_report({plugin_path.parent_path().string()});
  ASSERT_EQ(first_result.loaded, 1) << describe_errors(first_result.errors);
  ASSERT_EQ(current_lifecycle_compute_device(), "PLUGIN_LIFECYCLE_TEST");

  const auto second_result =
      manager.load_from_dirs_report({override_path.parent_path().string()});
  ASSERT_EQ(second_result.loaded, 1) << describe_errors(second_result.errors);
  ASSERT_EQ(current_lifecycle_compute_device(), "PLUGIN_OVERRIDE_TEST");

  EXPECT_EQ(manager.unload_all_plugins(), 2);
  EXPECT_EQ(manager.loaded_plugin_count(), 0u);
  ASSERT_EQ(manager.op_sources().at(kLifecycleKey), "built-in");
  EXPECT_EQ(current_lifecycle_compute_device(), "HOST_LIFECYCLE_SENTINEL");
}

/**
 * @brief Proves repeated public seed calls never replay built-ins over a later
 * registry replacement.
 * @throws Nothing when the replacement remains active until explicit cleanup.
 * @note `ops::register_builtin()` is called directly only after the assertion
 * to restore the test's core-operation callback for subsequent tests.
 */
TEST_F(PluginManagerLifecycleTest,
       RepeatedSeedDoesNotOverwritePostSeedRegistryReplacement) {
  auto& manager = PluginManager::process_instance();
  manager.seed_builtins_from_registry();

  OpRegistry::instance().register_op_hp_monolithic(
      "image_generator", "constant",
      [](const Node&, const std::vector<const NodeOutput*>&) {
        NodeOutput output;
        output.debug.compute_device = "POST_SEED_REPLACEMENT";
        return output;
      });

  manager.seed_builtins_from_registry();
  auto resolved = OpRegistry::instance().resolve_for_intent(
      "image_generator", "constant", ComputeIntent::GlobalHighPrecision);
  ASSERT_TRUE(resolved.has_value());
  ASSERT_TRUE(std::holds_alternative<MonolithicOpFunc>(*resolved));
  Node node;
  node.type = "image_generator";
  node.subtype = "constant";
  const NodeOutput output = std::get<MonolithicOpFunc>(*resolved)(node, {});
  EXPECT_EQ(output.debug.compute_device, "POST_SEED_REPLACEMENT");

  ops::register_builtin();
}

/**
 * @brief Proves explicit unload performs no allocation and preserves lifetime.
 * @throws Nothing when the armed allocation remains unconsumed.
 * @note The trace must destroy plugin callable state before library unload.
 */
TEST_F(PluginManagerLifecycleTest,
       UnloadAllAllocatesNothingAndDestroysCallbacksBeforeLibrary) {
  const auto plugin_path = lifecycle_plugin_path();
  ASSERT_TRUE(std::filesystem::exists(plugin_path));
  const auto trace_path = lifecycle_trace_path("unload-all-no-allocation");
  std::filesystem::remove(trace_path);
  ScopedEnvironmentVariable trace_environment(kLifecycleTraceEnvironment,
                                              trace_path.string());
  auto& manager = PluginManager::process_instance();
  const auto result =
      manager.load_from_dirs_report({plugin_path.parent_path().string()});
  ASSERT_EQ(result.loaded, 1) << describe_errors(result.errors);

  int removed = -1;
  bool fired_during_cleanup = false;
  bool failure_remained_armed = false;
  {
    ScopedPluginCleanupAllocationFailure failure(0);
    removed = manager.unload_all_plugins();
    fired_during_cleanup = plugin_cleanup_allocation_probe::did_fire();
    failure_remained_armed = consume_armed_cleanup_allocation_failure();
  }

  EXPECT_EQ(removed, 1);
  EXPECT_FALSE(fired_during_cleanup);
  EXPECT_TRUE(failure_remained_armed);
  EXPECT_EQ(manager.loaded_plugin_count(), 0u);
  EXPECT_FALSE(lifecycle_key_is_registered());
  EXPECT_EQ(read_lifecycle_trace(trace_path),
            (std::vector<std::string>{"registrar_return", "callback_destroy",
                                      "library_unload"}));
  std::filesystem::remove(trace_path);
}

/**
 * @brief Proves exact-key single-plugin unload is allocation-independent.
 * @throws Nothing when cleanup leaves the allocation failpoint armed.
 * @note The exact absolute key is computed before injection and the lifecycle
 * trace must destroy callable state before releasing the dynamic library.
 */
TEST_F(PluginManagerLifecycleTest,
       ExactKeyUnloadAllocatesNothingAndDestroysCallbacksBeforeLibrary) {
  const auto plugin_path = lifecycle_plugin_path();
  ASSERT_TRUE(std::filesystem::exists(plugin_path));
  const std::string load_key = std::filesystem::absolute(plugin_path).string();
  const auto trace_path = lifecycle_trace_path("exact-key-no-allocation");
  std::filesystem::remove(trace_path);
  ScopedEnvironmentVariable trace_environment(kLifecycleTraceEnvironment,
                                              trace_path.string());
  auto& manager = PluginManager::process_instance();
  const auto result =
      manager.load_from_dirs_report({plugin_path.parent_path().string()});
  ASSERT_EQ(result.loaded, 1) << describe_errors(result.errors);

  int removed = -1;
  bool fired_during_cleanup = false;
  bool failure_remained_armed = false;
  {
    ScopedPluginCleanupAllocationFailure failure(0);
    removed = manager.unload_by_plugin_path(load_key);
    fired_during_cleanup = plugin_cleanup_allocation_probe::did_fire();
    failure_remained_armed = consume_armed_cleanup_allocation_failure();
  }

  EXPECT_EQ(removed, 1);
  EXPECT_FALSE(fired_during_cleanup);
  EXPECT_TRUE(failure_remained_armed);
  EXPECT_EQ(manager.loaded_plugin_count(), 0u);
  EXPECT_FALSE(lifecycle_key_is_registered());
  EXPECT_EQ(read_lifecycle_trace(trace_path),
            (std::vector<std::string>{"registrar_return", "callback_destroy",
                                      "library_unload"}));
  std::filesystem::remove(trace_path);
}

/**
 * @brief Proves relative-path normalization failure has a strong guarantee.
 * @throws Nothing when the injected allocation failure is propagated exactly.
 * @note The allocation is injected before normalization; registry, source,
 * result, and retained handle state must remain unchanged.
 */
TEST_F(PluginManagerLifecycleTest,
       RelativePathNormalizationBadAllocLeavesPluginStateUnchanged) {
  const auto plugin_path = lifecycle_plugin_path();
  ASSERT_TRUE(std::filesystem::exists(plugin_path));
  const std::string load_key = std::filesystem::absolute(plugin_path).string();
  const std::string relative_path =
      std::filesystem::relative(plugin_path).string();
  ASSERT_NE(relative_path, load_key);
  auto& manager = PluginManager::process_instance();
  const auto result =
      manager.load_from_dirs_report({plugin_path.parent_path().string()});
  ASSERT_EQ(result.loaded, 1) << describe_errors(result.errors);
  ASSERT_EQ(manager.op_sources().at(kLifecycleKey), load_key);

  bool caught_bad_alloc = false;
  bool allocation_failed = false;
  {
    ScopedPluginCleanupAllocationFailure failure(0);
    try {
      (void)manager.unload_by_plugin_path(relative_path);
    } catch (const std::bad_alloc&) {
      caught_bad_alloc = true;
    }
    allocation_failed = plugin_cleanup_allocation_probe::did_fire();
  }

  EXPECT_TRUE(caught_bad_alloc);
  EXPECT_TRUE(allocation_failed);
  EXPECT_EQ(manager.loaded_plugin_count(), 1u);
  ASSERT_EQ(manager.op_sources().at(kLifecycleKey), load_key);
  EXPECT_EQ(current_lifecycle_compute_device(), "PLUGIN_LIFECYCLE_TEST");
  EXPECT_EQ(manager.unload_by_plugin_path(load_key), 1);
  EXPECT_EQ(manager.loaded_plugin_count(), 0u);
  EXPECT_FALSE(lifecycle_key_is_registered());
}

/**
 * @brief Proves repeated access returns one process owner and explicit unload
 * remains allocation-independent.
 * @throws Nothing when cleanup leaves the failpoint armed.
 * @note The process manager is intentionally never destroyed; explicit unload
 *       owns callback-before-library ordering.
 */
TEST_F(PluginManagerLifecycleTest,
       ProcessOwnerIsStableAndExplicitUnloadPreservesLifetimeOrder) {
  const auto plugin_path = lifecycle_plugin_path();
  ASSERT_TRUE(std::filesystem::exists(plugin_path));
  const auto trace_path = lifecycle_trace_path("destructor-no-allocation");
  std::filesystem::remove(trace_path);
  ScopedEnvironmentVariable trace_environment(kLifecycleTraceEnvironment,
                                              trace_path.string());
  auto& manager = PluginManager::process_instance();
  EXPECT_EQ(&manager, &PluginManager::process_instance());
  const auto result =
      manager.load_from_dirs_report({plugin_path.parent_path().string()});
  ASSERT_EQ(result.loaded, 1) << describe_errors(result.errors);

  bool fired_during_cleanup = false;
  bool failure_remained_armed = false;
  int removed = -1;
  {
    ScopedPluginCleanupAllocationFailure failure(0);
    removed = manager.unload_all_plugins();
    fired_during_cleanup = plugin_cleanup_allocation_probe::did_fire();
    failure_remained_armed = consume_armed_cleanup_allocation_failure();
  }

  EXPECT_EQ(removed, 1);
  EXPECT_FALSE(fired_during_cleanup);
  EXPECT_TRUE(failure_remained_armed);
  EXPECT_FALSE(lifecycle_key_is_registered());
  EXPECT_EQ(read_lifecycle_trace(trace_path),
            (std::vector<std::string>{"registrar_return", "callback_destroy",
                                      "library_unload"}));
  std::filesystem::remove(trace_path);
}

/**
 * @brief Keeps a real plugin callback mapped across explicit global unload.
 *
 * @throws Nothing when the callback lease preserves the dynamic library until
 *         invocation completion.
 * @note Registry visibility disappears synchronously at unload, while the
 *       copied in-flight callback defers callback destruction and library
 *       unmapping until the release-file barrier lets it return.
 */
TEST_F(PluginManagerLifecycleTest,
       ExplicitUnloadDefersLibraryReleaseUntilInFlightCallbackReturns) {
  const auto plugin_path = lifecycle_plugin_path();
  ASSERT_TRUE(std::filesystem::exists(plugin_path));
  const auto trace_path = lifecycle_trace_path("in-flight-unload");
  const auto release_path = lifecycle_trace_path("in-flight-release");
  std::filesystem::remove(trace_path);
  std::filesystem::remove(release_path);
  ScopedEnvironmentVariable trace_environment(kLifecycleTraceEnvironment,
                                              trace_path.string());
  ScopedEnvironmentVariable release_environment(
      kLifecycleCallbackReleaseEnvironment, release_path.string());

  auto& manager = PluginManager::process_instance();
  const auto result =
      manager.load_from_dirs_report({plugin_path.parent_path().string()});
  ASSERT_EQ(result.loaded, 1) << describe_errors(result.errors);
  auto resolved = OpRegistry::instance().resolve_for_intent(
      kLifecycleType, kLifecycleSubtype, ComputeIntent::GlobalHighPrecision);
  ASSERT_TRUE(resolved.has_value());
  ASSERT_TRUE(std::holds_alternative<MonolithicOpFunc>(*resolved));
  MonolithicOpFunc callback = std::move(std::get<MonolithicOpFunc>(*resolved));
  resolved.reset();

  auto invocation = std::async(std::launch::async,
                               [callback = std::move(callback)]() mutable {
                                 Node node;
                                 node.id = 1;
                                 node.type = kLifecycleType;
                                 node.subtype = kLifecycleSubtype;
                                 return callback(node, {});
                               });
  callback = MonolithicOpFunc{};
  const bool callback_entered = wait_for_lifecycle_event(
      trace_path, "callback_enter", std::chrono::seconds(2));
  if (!callback_entered) {
    std::ofstream(release_path).put('\n');
    invocation.wait();
    ADD_FAILURE() << "lifecycle callback did not reach release barrier";
    return;
  }

  EXPECT_EQ(manager.unload_all_plugins(), 1);
  EXPECT_FALSE(lifecycle_key_is_registered());
  EXPECT_EQ(read_lifecycle_trace(trace_path),
            (std::vector<std::string>{"registrar_return", "callback_enter"}));

  std::ofstream(release_path).put('\n');
  std::optional<NodeOutput> output{invocation.get()};
  invocation = std::future<NodeOutput>{};
  EXPECT_EQ(output->debug.compute_device, "PLUGIN_LIFECYCLE_TEST");
  EXPECT_EQ(read_lifecycle_trace(trace_path),
            (std::vector<std::string>{"registrar_return", "callback_enter",
                                      "callback_return", "callback_destroy"}));
  output.reset();
  EXPECT_EQ(read_lifecycle_trace(trace_path),
            (std::vector<std::string>{"registrar_return", "callback_enter",
                                      "callback_return", "callback_destroy",
                                      "library_unload"}));
  std::filesystem::remove(trace_path);
  std::filesystem::remove(release_path);
}

/**
 * @brief Proves copy construction and copy assignment retain an old result
 * lease until every plugin-instantiated payload member is retired.
 *
 * @throws Nothing when staged assignment preserves its strong guarantee and the
 *         real plugin trace remains correctly ordered.
 * @note A forced allocation failure first proves the destination stays intact;
 *       the final successful overwrite must emit `result_payload_destroy`
 *       before `library_unload`.
 */
TEST_F(PluginManagerLifecycleTest,
       NodeOutputCopyAssignmentRetiresPayloadBeforeLibrary) {
  const auto plugin_path = lifecycle_plugin_path();
  ASSERT_TRUE(std::filesystem::exists(plugin_path));
  const auto trace_path = lifecycle_trace_path("node-output-copy-assignment");
  std::filesystem::remove(trace_path);
  ScopedEnvironmentVariable trace_environment(kLifecycleTraceEnvironment,
                                              trace_path.string());
  ScopedEnvironmentVariable result_probe_environment(
      kLifecycleResultProbeEnvironment, "1");

  auto& manager = PluginManager::process_instance();
  const auto result =
      manager.load_from_dirs_report({plugin_path.parent_path().string()});
  ASSERT_EQ(result.loaded, 1) << describe_errors(result.errors);
  NodeOutput plugin_output = invoke_lifecycle_output();
  ASSERT_TRUE(plugin_output.image_buffer.context);
  EXPECT_EQ(manager.unload_all_plugins(), 1);
  EXPECT_EQ(read_lifecycle_trace(trace_path),
            (std::vector<std::string>{"registrar_return", "callback_destroy"}));

  NodeOutput host_replacement;
  host_replacement.data["host"] = "replacement";
  bool caught_bad_alloc = false;
  {
    ScopedPluginCleanupAllocationFailure failure(0);
    try {
      plugin_output = host_replacement;
    } catch (const std::bad_alloc&) {
      caught_bad_alloc = true;
    }
  }
  EXPECT_TRUE(caught_bad_alloc);
  EXPECT_EQ(plugin_output.debug.compute_device, "PLUGIN_LIFECYCLE_TEST");
  EXPECT_TRUE(plugin_output.image_buffer.context);
  EXPECT_EQ(read_lifecycle_trace(trace_path),
            (std::vector<std::string>{"registrar_return", "callback_destroy"}));

  NodeOutput copied(plugin_output);
  NodeOutput moved(std::move(copied));
  NodeOutput copy_assigned;
  copy_assigned = moved;
  plugin_output = NodeOutput{};
  moved = NodeOutput{};
  EXPECT_EQ(read_lifecycle_trace(trace_path),
            (std::vector<std::string>{"registrar_return", "callback_destroy"}));

  copy_assigned = host_replacement;
  EXPECT_EQ(
      read_lifecycle_trace(trace_path),
      (std::vector<std::string>{"registrar_return", "callback_destroy",
                                "result_payload_destroy", "library_unload"}));
  std::filesystem::remove(trace_path);
}

/**
 * @brief Proves move construction and move assignment transfer complete result
 * states without releasing the old lease ahead of its payload.
 *
 * @throws Nothing when final overwrite retires plugin state in trace order.
 * @note The source is moved twice before a final rvalue overwrite makes the
 * destination's retired state the last library owner.
 */
TEST_F(PluginManagerLifecycleTest,
       NodeOutputMoveAssignmentRetiresPayloadBeforeLibrary) {
  const auto plugin_path = lifecycle_plugin_path();
  ASSERT_TRUE(std::filesystem::exists(plugin_path));
  const auto trace_path = lifecycle_trace_path("node-output-move-assignment");
  std::filesystem::remove(trace_path);
  ScopedEnvironmentVariable trace_environment(kLifecycleTraceEnvironment,
                                              trace_path.string());
  ScopedEnvironmentVariable result_probe_environment(
      kLifecycleResultProbeEnvironment, "1");

  auto& manager = PluginManager::process_instance();
  const auto result =
      manager.load_from_dirs_report({plugin_path.parent_path().string()});
  ASSERT_EQ(result.loaded, 1) << describe_errors(result.errors);
  NodeOutput plugin_output = invoke_lifecycle_output();
  ASSERT_TRUE(plugin_output.image_buffer.context);
  EXPECT_EQ(manager.unload_all_plugins(), 1);
  EXPECT_EQ(read_lifecycle_trace(trace_path),
            (std::vector<std::string>{"registrar_return", "callback_destroy"}));

  NodeOutput moved(std::move(plugin_output));
  EXPECT_FALSE(plugin_output.plugin_library_lifetime);
  EXPECT_FALSE(plugin_output.image_buffer.context);
  NodeOutput move_assigned;
  move_assigned = std::move(moved);
  EXPECT_FALSE(moved.plugin_library_lifetime);
  EXPECT_FALSE(moved.image_buffer.context);
  EXPECT_EQ(read_lifecycle_trace(trace_path),
            (std::vector<std::string>{"registrar_return", "callback_destroy"}));

  move_assigned = NodeOutput{};
  EXPECT_EQ(
      read_lifecycle_trace(trace_path),
      (std::vector<std::string>{"registrar_return", "callback_destroy",
                                "result_payload_destroy", "library_unload"}));
  std::filesystem::remove(trace_path);
}

/**
 * @brief Overlaps plugin staging with a direct registry mutation
 * deterministically.
 *
 * @throws Nothing when the registrar barrier, both futures, and final callbacks
 *         complete within their bounded waits.
 * @note The registry slow path signals only after the mutation observes the
 *       loader thread as the actual lock owner. The registrar barrier is
 *       released after that signal; the mutation must then survive both plugin
 *       commit and unload.
 */
TEST_F(PluginManagerLifecycleTest,
       DirectSameKeyMutationSurvivesPluginPublicationAndUnload) {
  const auto plugin_path = lifecycle_plugin_path();
  ASSERT_TRUE(std::filesystem::exists(plugin_path));
  const auto trace_path = lifecycle_trace_path("registry-overlap");
  const auto release_path = lifecycle_trace_path("registrar-release");
  std::filesystem::remove(trace_path);
  std::filesystem::remove(release_path);
  ScopedEnvironmentVariable trace_environment(kLifecycleTraceEnvironment,
                                              trace_path.string());
  ScopedEnvironmentVariable release_environment(
      kLifecycleRegistrarReleaseEnvironment, release_path.string());

  auto& manager = PluginManager::process_instance();
  auto load_future = std::async(std::launch::async, [&]() {
    return manager.load_from_dirs_report({plugin_path.parent_path().string()});
  });
  const bool registrar_waiting = wait_for_lifecycle_event(
      trace_path, "registrar_wait_enter", std::chrono::seconds(2));
  if (!registrar_waiting) {
    std::ofstream(release_path).put('\n');
    load_future.wait();
    ADD_FAILURE() << "lifecycle registrar did not reach publication barrier";
    return;
  }

  ScopedOpRegistryContentionCounter contention;
  auto mutation_future =
      std::async(std::launch::async, register_direct_lifecycle_replacement);
  if (!contention.wait_until_observed(std::chrono::seconds(2))) {
    std::ofstream(release_path).put('\n');
    load_future.wait();
    mutation_future.wait();
    ADD_FAILURE()
        << "direct registry mutation did not enter the lock slow path";
    return;
  }
  EXPECT_EQ(contention.observed_count(), 1u);

  std::ofstream(release_path).put('\n');
  ASSERT_EQ(load_future.wait_for(std::chrono::seconds(2)),
            std::future_status::ready);
  const auto result = load_future.get();
  ASSERT_EQ(result.loaded, 1) << describe_errors(result.errors);
  ASSERT_EQ(mutation_future.wait_for(std::chrono::seconds(2)),
            std::future_status::ready);
  mutation_future.get();

  EXPECT_TRUE(lifecycle_key_is_registered());
  EXPECT_EQ(current_lifecycle_compute_device(), "DIRECT_LIFECYCLE_MUTATION");
  EXPECT_EQ(manager.op_sources().at(kLifecycleKey), "mixed");
  EXPECT_EQ(OpRegistry::instance().dirty_propagation_contract_status(
                kLifecycleType, kLifecycleSubtype),
            PropagationContractStatus::Explicit);
  EXPECT_EQ(OpRegistry::instance().forward_propagation_contract_status(
                kLifecycleType, kLifecycleSubtype),
            PropagationContractStatus::Explicit);
  EXPECT_EQ(manager.unload_all_plugins(), 1);
  EXPECT_TRUE(lifecycle_key_is_registered());
  EXPECT_EQ(current_lifecycle_compute_device(), "DIRECT_LIFECYCLE_MUTATION");
  EXPECT_EQ(manager.op_sources().at(kLifecycleKey), "built-in");
  EXPECT_EQ(OpRegistry::instance().dirty_propagation_contract_status(
                kLifecycleType, kLifecycleSubtype),
            PropagationContractStatus::LegacyIdentityFallback);
  EXPECT_EQ(OpRegistry::instance().forward_propagation_contract_status(
                kLifecycleType, kLifecycleSubtype),
            PropagationContractStatus::LegacyIdentityFallback);
  EXPECT_EQ(read_lifecycle_trace(trace_path),
            (std::vector<std::string>{"registrar_wait_enter",
                                      "registrar_wait_exit", "registrar_return",
                                      "callback_destroy", "library_unload"}));
  std::filesystem::remove(trace_path);
  std::filesystem::remove(release_path);
}

/**
 * @brief Reuses one manager across repeated real dynamic load/unload cycles.
 * @throws Nothing when registry/source/handle state returns to baseline.
 * @note Twenty iterations expose retained-handle and stale-source leakage.
 */
TEST_F(PluginManagerLifecycleTest, RepeatedLoadUnloadPreservesLifecycleState) {
  constexpr int kIterations = 20;
  const auto plugin_path = lifecycle_plugin_path();
  ASSERT_TRUE(std::filesystem::exists(plugin_path));
  auto& manager = PluginManager::process_instance();

  for (int iteration = 0; iteration < kIterations; ++iteration) {
    SCOPED_TRACE(iteration);
    const auto result =
        manager.load_from_dirs_report({plugin_path.parent_path().string()});
    ASSERT_EQ(result.loaded, 1) << describe_errors(result.errors);
    EXPECT_EQ(manager.loaded_plugin_count(), 1u);
    EXPECT_EQ(current_lifecycle_compute_device(), "PLUGIN_LIFECYCLE_TEST");
    EXPECT_EQ(manager.unload_all_plugins(), 1);
    EXPECT_EQ(manager.loaded_plugin_count(), 0u);
    EXPECT_FALSE(lifecycle_key_is_registered());
  }
}

/**
 * @brief Proves unloading a shadowed middle plugin splices its real
 * predecessor.
 *
 * @throws Nothing when sentinel, P1, and P2 behavior transitions are exact.
 * @note The shared trace proves P1 and P2 callable state is destroyed before
 *       each matching library unload, while the final P2 unload restores the
 *       host sentinel rather than removed P1 code.
 */
TEST_F(PluginManagerLifecycleTest,
       UnloadShadowedMiddlePluginSplicesPredecessorAndRetiresInOrder) {
  const auto plugin_path = lifecycle_plugin_path();
  const auto override_path = override_lifecycle_plugin_path();
  ASSERT_TRUE(std::filesystem::exists(plugin_path))
      << "lifecycle op plugin was not built: " << plugin_path;
  ASSERT_TRUE(std::filesystem::exists(override_path))
      << "override lifecycle op plugin was not built: " << override_path;
  const auto trace_path = lifecycle_trace_path("shadowed-middle-splice");
  std::filesystem::remove(trace_path);
  ScopedEnvironmentVariable trace_environment(kLifecycleTraceEnvironment,
                                              trace_path.string());

  auto& manager = PluginManager::process_instance();
  register_host_lifecycle_sentinel();
  manager.seed_builtins_from_registry();
  ASSERT_EQ(manager.op_sources().at(kLifecycleKey), "built-in");
  ASSERT_EQ(current_lifecycle_compute_device(), "HOST_LIFECYCLE_SENTINEL");

  const auto first_result =
      manager.load_from_dirs_report({plugin_path.parent_path().string()});
  ASSERT_EQ(first_result.loaded, 1) << describe_errors(first_result.errors);
  ASSERT_TRUE(first_result.errors.empty())
      << describe_errors(first_result.errors);

  const std::string original_path =
      std::filesystem::absolute(plugin_path).string();
  ASSERT_EQ(manager.op_sources().at(kLifecycleKey), original_path);
  EXPECT_EQ(current_lifecycle_compute_device(), "PLUGIN_LIFECYCLE_TEST");

  const auto replacement_result =
      manager.load_from_dirs_report({override_path.parent_path().string()});
  ASSERT_EQ(replacement_result.loaded, 1)
      << describe_errors(replacement_result.errors);
  ASSERT_TRUE(replacement_result.errors.empty())
      << describe_errors(replacement_result.errors);

  const std::string replacement_path =
      std::filesystem::absolute(override_path).string();
  ASSERT_EQ(manager.loaded_plugin_count(), 2u);
  ASSERT_EQ(manager.op_sources().at(kLifecycleKey), replacement_path);
  EXPECT_EQ(current_lifecycle_compute_device(), "PLUGIN_OVERRIDE_TEST");
  EXPECT_EQ(read_lifecycle_trace(trace_path),
            (std::vector<std::string>{"registrar_return",
                                      "override_registrar_return"}));

  EXPECT_EQ(manager.unload_by_plugin_path(original_path), 0);
  EXPECT_EQ(manager.loaded_plugin_count(), 1u);
  ASSERT_EQ(manager.op_sources().at(kLifecycleKey), replacement_path);
  EXPECT_EQ(current_lifecycle_compute_device(), "PLUGIN_OVERRIDE_TEST");
  EXPECT_EQ(
      read_lifecycle_trace(trace_path),
      (std::vector<std::string>{"registrar_return", "override_registrar_return",
                                "callback_destroy", "library_unload"}));

  EXPECT_EQ(manager.unload_by_plugin_path(replacement_path), 1);
  EXPECT_EQ(manager.loaded_plugin_count(), 0u);
  ASSERT_EQ(manager.op_sources().at(kLifecycleKey), "built-in");
  EXPECT_EQ(current_lifecycle_compute_device(), "HOST_LIFECYCLE_SENTINEL");
  EXPECT_EQ(read_lifecycle_trace(trace_path),
            (std::vector<std::string>{
                "registrar_return", "override_registrar_return",
                "callback_destroy", "library_unload",
                "override_callback_destroy", "override_library_unload"}));
  std::filesystem::remove(trace_path);
}

TEST_F(PluginManagerLifecycleTest,
       StandardOperationPluginsRegisterExplicitRoiContracts) {
  OpRegistry::instance().unregister_key("image_process:invert");
  OpRegistry::instance().unregister_key("image_process:threshold");
  OpRegistry::instance().unregister_key("io:save");
  OpRegistry::instance().unregister_key("image_generator:perlin_noise_metal");

  const auto plugin_dir = standard_plugin_dir();
  ASSERT_TRUE(std::filesystem::exists(plugin_dir))
      << "standard operation plugin directory was not built: " << plugin_dir;

  auto& manager = PluginManager::process_instance();
  const auto result = manager.load_from_dirs_report({plugin_dir.string()});

  EXPECT_GE(result.loaded, 3) << describe_errors(result.errors);
  EXPECT_TRUE(result.errors.empty()) << describe_errors(result.errors);
  EXPECT_TRUE(result_contains_key(result, "image_process:invert"));
  EXPECT_TRUE(result_contains_key(result, "image_process:threshold"));
  EXPECT_TRUE(result_contains_key(result, "io:save"));
  const bool metal_perlin_loaded =
      result_contains_key(result, "image_generator:perlin_noise_metal");

  expect_explicit_roi_contract("image_process", "invert");
  expect_explicit_roi_contract("image_process", "threshold");
  expect_explicit_roi_contract("io", "save");
  if (metal_perlin_loaded) {
    expect_explicit_roi_contract("image_generator", "perlin_noise_metal");
  }

  EXPECT_GE(manager.unload_all_plugins(), 3);
  EXPECT_EQ(OpRegistry::instance().dirty_propagation_contract_status(
                "image_process", "invert"),
            PropagationContractStatus::LegacyIdentityFallback);
}

#if defined(PHOTOSPIDER_INTERNAL_BAD_ALLOC_TESTING)
TEST_F(PluginManagerLifecycleTest,
       PostRegistrationBadAllocHasStrongTransactionGuarantee) {
  using Failpoint = testing::OperationPluginLoadFailpoint;
  const std::vector<std::pair<Failpoint, std::string>> cases = {
      {Failpoint::PreviousSource, "previous-source"},
      {Failpoint::SourceAndResult, "source-result"},
      {Failpoint::Snapshot, "snapshot"},
      {Failpoint::HandleCommit, "handle-commit"},
  };
  const auto plugin_path = lifecycle_plugin_path();
  ASSERT_TRUE(std::filesystem::exists(plugin_path));

  for (const auto& [failpoint, label] : cases) {
    SCOPED_TRACE(label);
    OpRegistry::instance().unregister_key(kLifecycleKey);
    register_host_lifecycle_sentinel();
    std::map<std::string, std::string> op_sources = {
        {"sentinel:key", "sentinel-source"}};
    LoadedOpPluginMap loaded_plugins;
    LoadedOpPlugin sentinel_plugin;
    sentinel_plugin.registered_keys.push_back("sentinel:key");
    loaded_plugins.emplace("sentinel-plugin", std::move(sentinel_plugin));
    PluginLoadResult result;
    result.attempted = 7;
    result.loaded = 3;
    result.errors.push_back(
        {"sentinel-path", GraphErrc::Unknown, "sentinel-error"});
    result.new_op_keys.push_back("sentinel:key");

    const auto trace_path = lifecycle_trace_path(label);
    std::filesystem::remove(trace_path);
    ScopedEnvironmentVariable trace_environment(kLifecycleTraceEnvironment,
                                                trace_path.string());
    testing::set_operation_plugin_load_failpoint(failpoint);

    bool caught_bad_alloc = false;
    try {
      testing::load_one_operation_plugin_for_testing(plugin_path, op_sources,
                                                     loaded_plugins, result);
    } catch (const std::bad_alloc&) {
      caught_bad_alloc = true;
    }
    const auto failpoint_hits = testing::operation_plugin_load_failpoint_hits();
    testing::set_operation_plugin_load_failpoint(Failpoint::None);

    const auto trace = read_lifecycle_trace(trace_path);
    EXPECT_TRUE(caught_bad_alloc);
    EXPECT_EQ(failpoint_hits, 1u);
    EXPECT_EQ(op_sources, (std::map<std::string, std::string>{
                              {"sentinel:key", "sentinel-source"}}));
    ASSERT_EQ(loaded_plugins.size(), 1u);
    ASSERT_EQ(loaded_plugins.count("sentinel-plugin"), 1u);
    EXPECT_EQ(loaded_plugins.at("sentinel-plugin").registered_keys,
              (std::vector<std::string>{"sentinel:key"}));
    EXPECT_EQ(result.attempted, 7);
    EXPECT_EQ(result.loaded, 3);
    ASSERT_EQ(result.errors.size(), 1u);
    EXPECT_EQ(result.errors[0].path, "sentinel-path");
    EXPECT_EQ(result.errors[0].message, "sentinel-error");
    EXPECT_EQ(result.new_op_keys, (std::vector<std::string>{"sentinel:key"}));
    EXPECT_TRUE(lifecycle_key_is_registered());
    EXPECT_EQ(current_lifecycle_compute_device(), "HOST_LIFECYCLE_SENTINEL");
    EXPECT_EQ(trace,
              (std::vector<std::string>{"registrar_return", "callback_destroy",
                                        "library_unload"}));
    std::cout << "plugin_transaction_trace failpoint=" << label
              << " bad_alloc=" << caught_bad_alloc << " hits=" << failpoint_hits
              << " attempted=" << result.attempted
              << " sources=" << op_sources.size()
              << " handles=" << loaded_plugins.size()
              << " active_callback=" << current_lifecycle_compute_device()
              << " lifecycle=";
    for (const auto& event : trace) {
      std::cout << event << ',';
    }
    std::cout << '\n';
    std::filesystem::remove(trace_path);
    OpRegistry::instance().unregister_key(kLifecycleKey);
  }
}

TEST_F(PluginManagerLifecycleTest,
       ManagerBadAllocDoesNotRetainOrPublishCandidatePlugin) {
  using Failpoint = testing::OperationPluginLoadFailpoint;
  const auto plugin_path = lifecycle_plugin_path();
  ASSERT_TRUE(std::filesystem::exists(plugin_path));
  const auto trace_path = lifecycle_trace_path("manager-handle-commit");
  std::filesystem::remove(trace_path);
  ScopedEnvironmentVariable trace_environment(kLifecycleTraceEnvironment,
                                              trace_path.string());
  testing::set_operation_plugin_load_failpoint(Failpoint::HandleCommit);

  auto& manager = PluginManager::process_instance();
  EXPECT_THROW(
      manager.load_from_dirs_report({plugin_path.parent_path().string()}),
      std::bad_alloc);
  const auto hits = testing::operation_plugin_load_failpoint_hits();
  testing::set_operation_plugin_load_failpoint(Failpoint::None);

  EXPECT_EQ(hits, 1u);
  EXPECT_EQ(manager.loaded_plugin_count(), 0u);
  EXPECT_EQ(manager.op_sources().count(kLifecycleKey), 0u);
  EXPECT_FALSE(lifecycle_key_is_registered());
  EXPECT_EQ(read_lifecycle_trace(trace_path),
            (std::vector<std::string>{"registrar_return", "callback_destroy",
                                      "library_unload"}));
  std::cout << "plugin_manager_transaction_trace failpoint=handle-commit"
            << " hits=" << hits
            << " retained_handles=" << manager.loaded_plugin_count()
            << " active_key=" << lifecycle_key_is_registered() << '\n';
  std::filesystem::remove(trace_path);
}

TEST_F(PluginManagerLifecycleTest,
       RegistrarFailuresPreserveOriginalPolicyAndSafeUnloadOrder) {
  const auto plugin_path = lifecycle_plugin_path();
  ASSERT_TRUE(std::filesystem::exists(plugin_path));

  {
    const auto trace_path = lifecycle_trace_path("registrar-runtime");
    std::filesystem::remove(trace_path);
    ScopedEnvironmentVariable trace_environment(kLifecycleTraceEnvironment,
                                                trace_path.string());
    ScopedEnvironmentVariable throw_environment(kLifecycleThrowEnvironment,
                                                "runtime_error");
    auto& manager = PluginManager::process_instance();
    const auto result =
        manager.load_from_dirs_report({plugin_path.parent_path().string()});
    ASSERT_EQ(result.errors.size(), 1u);
    EXPECT_EQ(result.errors[0].message, "lifecycle registrar runtime failure");
    EXPECT_EQ(result.loaded, 0);
    EXPECT_EQ(manager.loaded_plugin_count(), 0u);
    EXPECT_FALSE(lifecycle_key_is_registered());
    EXPECT_EQ(read_lifecycle_trace(trace_path),
              (std::vector<std::string>{"registrar_throw_runtime_error",
                                        "callback_destroy", "library_unload"}));
    std::cout << "plugin_registrar_failure_trace mode=runtime_error"
              << " errors=" << result.errors.size()
              << " retained_handles=" << manager.loaded_plugin_count()
              << " active_key=" << lifecycle_key_is_registered() << '\n';
    std::filesystem::remove(trace_path);
  }

  {
    const auto trace_path = lifecycle_trace_path("registrar-bad-alloc");
    std::filesystem::remove(trace_path);
    ScopedEnvironmentVariable trace_environment(kLifecycleTraceEnvironment,
                                                trace_path.string());
    ScopedEnvironmentVariable throw_environment(kLifecycleThrowEnvironment,
                                                "bad_alloc");
    auto& manager = PluginManager::process_instance();
    EXPECT_THROW(
        manager.load_from_dirs_report({plugin_path.parent_path().string()}),
        std::bad_alloc);
    EXPECT_EQ(manager.loaded_plugin_count(), 0u);
    EXPECT_FALSE(lifecycle_key_is_registered());
    EXPECT_EQ(read_lifecycle_trace(trace_path),
              (std::vector<std::string>{"registrar_throw_bad_alloc",
                                        "callback_destroy", "library_unload"}));
    std::cout << "plugin_registrar_failure_trace mode=bad_alloc"
              << " retained_handles=" << manager.loaded_plugin_count()
              << " active_key=" << lifecycle_key_is_registered() << '\n';
    std::filesystem::remove(trace_path);
  }
}
#endif

}  // namespace ps
