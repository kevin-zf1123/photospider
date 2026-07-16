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

#include "compute/task_graph_planning.hpp"
#include "core/ops.hpp"
#include "graph/graph_model.hpp"
#include "graph/roi_propagation_service.hpp"
#include "plugin/plugin_manager.hpp"
#if defined(PHOTOSPIDER_INTERNAL_BAD_ALLOC_TESTING)
#include "core/op_registry_test_access.hpp"
#include "plugin/plugin_loader_test_access.hpp"
#endif
#include "core/ps_types.hpp"  // NOLINT(build/include_subdir)
#include "graph/node.hpp"     // NOLINT(build/include_subdir)

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
constexpr const char* kLifecycleCpuDeviceSubtype = "cpu_device";
constexpr const char* kLifecycleCpuDeviceKey = "plugin_lifecycle:cpu_device";
constexpr const char* kLifecycleTaskShapeSubtype = "task_shape_override";
constexpr const char* kLifecycleTaskShapeKey =
    "plugin_lifecycle:task_shape_override";  // NOLINT(whitespace/indent_namespace)
constexpr const char* kDirectCpuOwnerType = "direct_cpu_stable_owner";
constexpr const char* kDirectCpuMonolithicSubtype = "monolithic";
constexpr const char* kDirectCpuMonolithicKey =
    "direct_cpu_stable_owner:monolithic";  // NOLINT(whitespace/indent_namespace)
constexpr const char* kDirectCpuTiledSubtype = "tiled";
constexpr const char* kDirectCpuTiledKey = "direct_cpu_stable_owner:tiled";
constexpr const char* kLifecycleTraceEnvironment = "PS_LIFECYCLE_PLUGIN_TRACE";
constexpr const char* kLifecycleThrowEnvironment =
    "PS_LIFECYCLE_PLUGIN_REGISTRAR_THROW";  // NOLINT(whitespace/indent_namespace)
constexpr const char* kLifecycleCallbackReleaseEnvironment =
    "PS_LIFECYCLE_PLUGIN_CALLBACK_RELEASE_FILE";  // NOLINT(whitespace/indent_namespace)
constexpr const char* kLifecycleCallbackThrowEnvironment =
    "PS_LIFECYCLE_PLUGIN_CALLBACK_THROW";  // NOLINT(whitespace/indent_namespace)
constexpr const char* kLifecycleRegistrarReleaseEnvironment =
    "PS_LIFECYCLE_PLUGIN_REGISTRAR_RELEASE_FILE";  // NOLINT(whitespace/indent_namespace)
constexpr const char* kLifecycleResultProbeEnvironment =
    "PS_LIFECYCLE_PLUGIN_RESULT_PROBE";  // NOLINT(whitespace/indent_namespace)
constexpr const char* kLifecycleInvalidResultEnvironment =
    "PS_LIFECYCLE_PLUGIN_INVALID_RESULT";  // NOLINT(whitespace/indent_namespace)
constexpr const char* kLifecycleDeviceRegistrarEnvironment =
    "PS_LIFECYCLE_PLUGIN_REGISTER_DEVICES";  // NOLINT(whitespace/indent_namespace)
constexpr const char* kLifecycleCpuDeviceRegistrarEnvironment =
    "PS_LIFECYCLE_PLUGIN_REGISTER_CPU_DEVICE";  // NOLINT(whitespace/indent_namespace)
constexpr const char* kLifecycleTaskShapeOverrideEnvironment =
    "PS_LIFECYCLE_PLUGIN_REGISTER_TASK_SHAPE_OVERRIDE";  // NOLINT(whitespace/indent_namespace)
constexpr const char* kLifecycleDataDependentEnvironment =
    "PS_LIFECYCLE_PLUGIN_DATA_DEPENDENT_LUT";  // NOLINT(whitespace/indent_namespace)
constexpr const char* kLifecycleInvalidRoiEnvironment =
    "PS_LIFECYCLE_PLUGIN_INVALID_ROI";  // NOLINT(whitespace/indent_namespace)
constexpr const char* kLifecycleInvalidNameEnvironment =
    "PS_LIFECYCLE_PLUGIN_INVALID_NAME";  // NOLINT(whitespace/indent_namespace)
constexpr const char* kLifecycleEmptyCallbackEnvironment =
    "PS_LIFECYCLE_PLUGIN_EMPTY_CALLBACK";  // NOLINT(whitespace/indent_namespace)
constexpr const char* kV1OnlyMarkerEnvironment =
    "PS_V1_ONLY_PLUGIN_MARKER";  // NOLINT(whitespace/indent_namespace)

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
 * @brief Returns the build output path for the unsupported v1-only fixture.
 * @return Platform-specific shared library path under test_op_plugin_dir().
 * @throws std::bad_alloc from path string construction.
 * @note CMake places this fixture alone in its directory so a retry attempts
 * exactly one rejected candidate.
 */
std::filesystem::path v1_only_plugin_path() {
  const std::filesystem::path dir = test_op_plugin_dir() / "v1_only";
#if defined(_WIN32)
  return dir / "v1_only_op_plugin.dll";
#elif defined(__APPLE__)
  return dir / "libv1_only_op_plugin.dylib";
#else
  return dir / "libv1_only_op_plugin.so";
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

  /**
   * @brief Prevents two guards from restoring the same environment key.
   * @param other Guard that remains responsible for its installed value.
   * @throws Nothing; operation is deleted.
   * @note Unique lexical ownership preserves one deterministic restoration.
   */
  ScopedEnvironmentVariable(const ScopedEnvironmentVariable& other) = delete;

  /**
   * @brief Prevents replacing one active environment guard by assignment.
   * @param other Guard that retains responsibility for its installed value.
   * @return No value because this operation is deleted.
   * @throws Nothing; operation is deleted.
   * @note Assignment cannot transfer process-global restoration ownership.
   */
  ScopedEnvironmentVariable& operator=(const ScopedEnvironmentVariable& other) =
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

  /**
   * @brief Prevents duplicating ownership of one armed allocation failpoint.
   * @param other Scope that remains responsible for disarming the failpoint.
   * @throws Nothing; operation is deleted.
   * @note Exactly one lexical owner must disarm the thread-local probe.
   */
  ScopedPluginCleanupAllocationFailure(
      const ScopedPluginCleanupAllocationFailure& other) = delete;

  /**
   * @brief Prevents replacing an active allocation-failure scope.
   * @param other Scope retaining responsibility for its armed failpoint.
   * @return No value because this operation is deleted.
   * @throws Nothing; operation is deleted.
   * @note Assignment cannot transfer the thread-local disarm obligation.
   */
  ScopedPluginCleanupAllocationFailure& operator=(
      const ScopedPluginCleanupAllocationFailure& other) = delete;
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
   * @throws Nothing; operation is deleted.
   * @note Deletion prevents either destructor from clearing another counter.
   */
  ScopedOpRegistryContentionCounter(
      const ScopedOpRegistryContentionCounter& other) = delete;

  /**
   * @brief Prevents retargeting a live installed observer.
   *
   * @param other Scope that must retain its own counter lifetime.
   * @return No value because this operation is deleted.
   * @throws Nothing; operation is deleted.
   * @note The installed pointer remains paired with lexical scope lifetime.
   */
  ScopedOpRegistryContentionCounter& operator=(
      const ScopedOpRegistryContentionCounter& other) = delete;

  /**
   * @brief Prevents moving the installed counter to a different address.
   *
   * @param other Scope whose inline counter address remains installed.
   * @throws Nothing; operation is deleted.
   * @note Deletion keeps the published observer pointer stable.
   */
  ScopedOpRegistryContentionCounter(ScopedOpRegistryContentionCounter&& other) =
      delete;

  /**
   * @brief Prevents transferring an installed observer into another scope.
   *
   * @param other Scope that retains its own observer lifetime.
   * @return No value because this operation is deleted.
   * @throws Nothing; operation is deleted.
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
 * @brief Owns one BUILD_TESTING observer for final device-wrapper retirement.
 *
 * @throws Nothing.
 * @note The inline counters outlive the installed observer pointer, and the
 *       destructor clears that pointer before their storage leaves scope.
 */
class ScopedDeviceCallbackRetirementInspection final {
 public:
  /**
   * @brief Installs this scope's inline retirement counters.
   * @throws Nothing.
   * @note A test must not overlap two instances because the observer is
   *       process-global while plugin unload is process-serialized.
   */
  ScopedDeviceCallbackRetirementInspection() noexcept {
    testing::set_op_registry_device_callback_retirement_inspection(
        &inspection_);
  }

  /**
   * @brief Clears the observer before inline counter destruction.
   * @throws Nothing.
   * @note Every callback expected to report must already have retired.
   */
  ~ScopedDeviceCallbackRetirementInspection() noexcept {
    testing::set_op_registry_device_callback_retirement_inspection(nullptr);
  }

  /**
   * @brief Prevents two scopes from sharing one installed observer lifetime.
   * @param other Scope that remains the unique observer owner.
   * @throws Nothing; operation is deleted.
   * @note Deletion prevents either destructor from clearing another observer.
   */
  ScopedDeviceCallbackRetirementInspection(
      const ScopedDeviceCallbackRetirementInspection& other) = delete;

  /**
   * @brief Prevents retargeting a live retirement observer by copy assignment.
   * @param other Scope retaining its own inline counters.
   * @return No value because this operation is deleted.
   * @throws Nothing; operation is deleted.
   * @note The installed pointer remains paired with lexical scope lifetime.
   */
  ScopedDeviceCallbackRetirementInspection& operator=(
      const ScopedDeviceCallbackRetirementInspection& other) = delete;

  /**
   * @brief Prevents moving installed counters to a different address.
   * @param other Scope whose inline counter address remains installed.
   * @throws Nothing; operation is deleted.
   * @note Deletion keeps the observer pointer stable.
   */
  ScopedDeviceCallbackRetirementInspection(
      ScopedDeviceCallbackRetirementInspection&& other) = delete;

  /**
   * @brief Prevents transferring an installed observer by move assignment.
   * @param other Scope retaining its own observer lifetime.
   * @return No value because this operation is deleted.
   * @throws Nothing; operation is deleted.
   * @note The active pointer cannot be rebound after construction.
   */
  ScopedDeviceCallbackRetirementInspection& operator=(
      ScopedDeviceCallbackRetirementInspection&& other) = delete;

  /**
   * @brief Returns final host device-wrapper destructions observed so far.
   * @return Monotonic destruction count.
   * @throws Nothing.
   * @note Reader copies share wrapper state and therefore do not increment this
   *       value until the last retained state is released.
   */
  std::uint64_t destructions() const noexcept {
    return inspection_.destructions.load(std::memory_order_acquire);
  }

  /**
   * @brief Returns wrapper destructions that began under the registry lock.
   * @return Monotonic lock-violation count.
   * @throws Nothing.
   * @note Correct retirement keeps this value at zero.
   */
  std::uint64_t destructions_under_lock() const noexcept {
    return inspection_.destructions_under_lock.load(std::memory_order_acquire);
  }

 private:
  /** @brief Inline counters published only for this lexical scope. */
  testing::OpRegistryDeviceCallbackRetirementInspection inspection_;
};

/**
 * @brief Removes one registry key before its borrowed callback state expires.
 *
 * @throws std::bad_alloc if canonical key construction cannot allocate.
 * @note Declare this guard immediately after the stack state borrowed by a
 *       registered callback. Reverse destruction then unregisters the callback
 *       before that state expires on fatal assertions or exceptions. Registry
 *       lookup and node extraction do not allocate, but retired callback
 *       destructors may execute allocating user code and must remain noexcept.
 */
class ScopedOpRegistryKeyCleanup final {
 public:
  /**
   * @brief Prepares noexcept cleanup for one operation type and subtype.
   * @param registry Registry whose key may receive a borrowed callback.
   * @param type Operation type used to build the canonical key.
   * @param subtype Operation subtype used to build the canonical key.
   * @throws std::bad_alloc if canonical key construction cannot allocate.
   * @note Canonical-key allocation completes before registration.
   *       `OpRegistry::unregister_key` lookup and node extraction do not
   *       allocate; retired callback destructors may execute allocating user
   *       code, but they must honor noexcept destruction.
   */
  ScopedOpRegistryKeyCleanup(OpRegistry& registry, const std::string& type,
                             const std::string& subtype)
      : registry_(registry), key_(make_key(type, subtype)) {}

  /**
   * @brief Removes the guarded key while borrowed callback state is alive.
   * @throws Nothing; the destructor is noexcept and cannot propagate an
   *         exception.
   * @note `OpRegistry::unregister_key` is idempotent; its documented lookup
   *       and node-extraction path does not allocate or throw, and callback
   *       retirement occurs outside the state lock. Retired callback
   *       destructors may allocate while running user code but must be
   *       noexcept; a violation terminates instead of escaping this guard.
   *       The guard therefore does not promise allocation-free teardown. A
   *       disarmed guard performs no operation.
   */
  ~ScopedOpRegistryKeyCleanup() noexcept {
    if (armed_) {
      (void)registry_.unregister_key(key_);
    }
  }

  /**
   * @brief Prevents two guards from cleaning the same borrowed callback.
   * @param other Guard that remains responsible for its registry key.
   * @throws Nothing; operation is deleted.
   * @note Unique lexical ownership fixes cleanup ordering relative to the
   *       callback's borrowed state.
   */
  ScopedOpRegistryKeyCleanup(const ScopedOpRegistryKeyCleanup& other) = delete;

  /**
   * @brief Prevents replacing one active cleanup obligation by assignment.
   * @param other Guard retaining responsibility for its registry key.
   * @return No value because this operation is deleted.
   * @throws Nothing; operation is deleted.
   * @note Assignment cannot preserve both guards' declaration ordering.
   */
  ScopedOpRegistryKeyCleanup& operator=(
      const ScopedOpRegistryKeyCleanup& other) = delete;

  /**
   * @brief Prevents moving cleanup past the borrowed state it protects.
   * @param other Guard whose lexical position remains authoritative.
   * @throws Nothing; operation is deleted.
   * @note Stable scope placement guarantees cleanup precedes tracker expiry.
   */
  ScopedOpRegistryKeyCleanup(ScopedOpRegistryKeyCleanup&& other) = delete;

  /**
   * @brief Prevents move assignment from changing cleanup order.
   * @param other Guard retaining its original lexical lifetime.
   * @return No value because this operation is deleted.
   * @throws Nothing; operation is deleted.
   * @note Cleanup ownership cannot move after callback registration.
   */
  ScopedOpRegistryKeyCleanup& operator=(ScopedOpRegistryKeyCleanup&& other) =
      delete;

  /**
   * @brief Disarms cleanup after explicit whole-key unregister succeeds.
   * @return Nothing.
   * @throws Nothing.
   * @note Call only after `unregister_key` has returned; retained callback
   *       snapshots still destruct before the tracker by lexical ordering.
   */
  void disarm() noexcept { armed_ = false; }

 private:
  /** @brief Registry that outlives this test-local cleanup guard. */
  OpRegistry& registry_;
  /** @brief Preallocated canonical key used by noexcept destruction. */
  std::string key_;
  /**
   * @brief True until normal-path unregister transfers cleanup responsibility.
   */
  bool armed_ = true;
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
 * @brief Counts one exact event in a parsed lifecycle trace.
 *
 * @param events Ordered fixture events to inspect.
 * @param expected Exact event label to count.
 * @return Number of matching entries.
 * @throws Nothing.
 * @note CPU bridge tests compare counts before and after reader copies so
 *       registration-time target copies remain distinguishable from
 *       regressions.
 */
std::size_t count_lifecycle_event(const std::vector<std::string>& events,
                                  const std::string& expected) noexcept {
  return static_cast<std::size_t>(
      std::count(events.begin(), events.end(), expected));
}

/**
 * @brief Canonicalizes the unspecified destruction order of two vector
 * elements.
 *
 * @param events Ordered lifecycle trace copied for comparison.
 * @return Trace with each adjacent monolithic/tiled device-destruction pair in
 *         tiled-then-monolithic order.
 * @throws Nothing beyond the already completed input-vector move.
 * @note C++ does not make tests depend on one standard library's vector element
 *       destruction direction; all ordering relative to callback/library
 *       retirement remains unchanged and is still asserted exactly.
 */
std::vector<std::string> normalize_device_target_trace(
    std::vector<std::string> events) noexcept {
  for (std::size_t index = 0; index + 1 < events.size(); ++index) {
    if (events[index] == "device_monolithic_target_destroy" &&
        events[index + 1] == "device_tiled_target_destroy") {
      events[index].swap(events[index + 1]);
      ++index;
    }
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
 * @brief Asserts exact equality of one key's internal ownership inspection.
 *
 * @param before Coherent baseline captured before one precise failpoint.
 * @param after Coherent state captured after the injected exception.
 * @return Nothing.
 * @throws GTest assertion failures when key presence, values, or revisions
 * vary.
 * @note Failpoint tests use a single baseline device slot, so equality of its
 *       exact first revision plus count proves the complete device token vector
 *       remains unchanged.
 */
void expect_device_ownership_inspection_equal(
    const testing::OpRegistryDeviceOwnershipInspection& before,
    const testing::OpRegistryDeviceOwnershipInspection& after) {
  EXPECT_EQ(after.implementation_entry_present,
            before.implementation_entry_present);
  EXPECT_EQ(after.ownership_entry_present, before.ownership_entry_present);
  EXPECT_EQ(after.implementation_count, before.implementation_count);
  EXPECT_EQ(after.revision_count, before.revision_count);
  EXPECT_EQ(after.all_revisions_nonzero, before.all_revisions_nonzero);
  EXPECT_EQ(after.first_device_revision, before.first_device_revision);
  EXPECT_EQ(after.monolithic_hp_revision, before.monolithic_hp_revision);
  EXPECT_EQ(after.tiled_hp_revision, before.tiled_hp_revision);
  EXPECT_EQ(after.meta_hp_revision, before.meta_hp_revision);
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
 * @brief Aggregates lifecycle observations for stateful device callback
 * targets.
 *
 * Each target construction, copy, move, and destruction inspects the actual
 * current-thread registry lock token and performs one reentrant registry read.
 * The test can therefore distinguish a genuine lock-bound target operation
 * from mere absence of deadlock.
 *
 * @throws Nothing directly; construction and destruction operate only on
 *         atomics and borrowed pointers.
 * @note The tracker has stack lifetime spanning every callback snapshot and
 *       registry retirement in its test. Atomic fields keep observations safe
 *       if implementation details later move snapshot copying to another
 *       thread.
 */
struct ReentrantDeviceCallbackTracker {
  /** @brief Registry inspected and re-entered by every target operation. */
  OpRegistry* registry = nullptr;
  /** @brief Number of currently live functor target objects. */
  std::atomic<int> live_targets{0};
  /** @brief Number of directly constructed original target objects. */
  std::atomic<int> constructions{0};
  /** @brief Number of original-target copy-constructor calls. */
  std::atomic<int> copies{0};
  /** @brief Number of original-target move-constructor calls. */
  std::atomic<int> moves{0};
  /** @brief Total observed construction/copy/move/destruction operations. */
  std::atomic<int> observations{0};
  /** @brief Number of target destructors that completed. */
  std::atomic<int> destructions{0};
  /** @brief Number of callback invocations through any copied path. */
  std::atomic<int> invocations{0};
  /** @brief Last deterministic tiled-callback result observed by the test. */
  std::atomic<int> last_tiled_result{-1};
  /** @brief Sticky signal that any target operation held the registry lock. */
  std::atomic<bool> observed_under_lock{false};
  /** @brief Sticky signal that any reentrant registry read failed. */
  std::atomic<bool> reentry_failed{false};

  /**
   * @brief Inspects lock ownership and performs a real registry re-entry.
   *
   * @return Nothing.
   * @throws Nothing; registry read failures become `reentry_failed`.
   * @note The owner-token inspection occurs before the read, so same-thread
   *       recursive lock support cannot hide a lock-bound target operation.
   */
  void observe() noexcept {
    observations.fetch_add(1, std::memory_order_relaxed);
    if (testing::op_registry_lock_held_by_current_thread_for_testing(
            *registry)) {
      observed_under_lock.store(true, std::memory_order_release);
    }
    try {
      (void)registry->get_keys();
    } catch (...) {
      reentry_failed.store(true, std::memory_order_release);
    }
  }
};

/**
 * @brief Common state whose special members audit callback-target relocation.
 *
 * @throws Nothing after construction; all observations suppress registry read
 *         failures.
 * @note Move transfers the tracker identity without changing the live-target
 *       count. A moved-from destructor is inert.
 */
struct ReentrantDeviceCallbackState {
  /** @brief Tracker owned by the surrounding deterministic test. */
  ReentrantDeviceCallbackTracker* tracker = nullptr;

  /**
   * @brief Creates one observed callback target.
   * @param target_tracker Tracker that outlives this target.
   * @throws Nothing.
   */
  explicit ReentrantDeviceCallbackState(
      ReentrantDeviceCallbackTracker& target_tracker) noexcept
      : tracker(&target_tracker) {
    tracker->constructions.fetch_add(1, std::memory_order_relaxed);
    tracker->live_targets.fetch_add(1, std::memory_order_relaxed);
    tracker->observe();
  }

  /**
   * @brief Copies one callback target and records the copy context.
   * @param other Live target whose tracker is shared.
   * @throws Nothing.
   */
  ReentrantDeviceCallbackState(
      const ReentrantDeviceCallbackState& other) noexcept
      : tracker(other.tracker) {
    if (tracker) {
      tracker->copies.fetch_add(1, std::memory_order_relaxed);
      tracker->live_targets.fetch_add(1, std::memory_order_relaxed);
      tracker->observe();
    }
  }

  /**
   * @brief Transfers one callback target and records the move context.
   * @param other Target relinquishing its tracker identity.
   * @throws Nothing.
   */
  ReentrantDeviceCallbackState(ReentrantDeviceCallbackState&& other) noexcept
      : tracker(std::exchange(other.tracker, nullptr)) {
    if (tracker) {
      tracker->moves.fetch_add(1, std::memory_order_relaxed);
      tracker->observe();
    }
  }

  /**
   * @brief Prevents replacing an observed callback target by copy assignment.
   * @param other Target that retains its existing tracker identity.
   * @return No value because this operation is deleted.
   * @throws Nothing; operation is deleted.
   * @note Immutable target identity keeps live/destruction counts balanced.
   */
  ReentrantDeviceCallbackState& operator=(
      const ReentrantDeviceCallbackState& other) = delete;

  /**
   * @brief Prevents replacing an observed callback target by move assignment.
   * @param other Target that retains its existing tracker identity.
   * @return No value because this operation is deleted.
   * @throws Nothing; operation is deleted.
   * @note Stable-owner publication moves the surrounding callback value
   * instead.
   */
  ReentrantDeviceCallbackState& operator=(
      ReentrantDeviceCallbackState&& other) = delete;

  /**
   * @brief Records target destruction and releases one live-target count.
   * @throws Nothing.
   * @note The observation includes the final target destruction after registry
   *       unregister, which must re-enter only after lock release.
   */
  ~ReentrantDeviceCallbackState() noexcept {
    if (tracker) {
      tracker->observe();
      tracker->destructions.fetch_add(1, std::memory_order_relaxed);
      tracker->live_targets.fetch_sub(1, std::memory_order_release);
    }
  }
};

/**
 * @brief Stateful monolithic device callback used by relocation tests.
 * @throws Nothing after construction.
 * @note Every copied target reports through the inherited tracker; the tracker
 *       must outlive all callback snapshots.
 */
struct ReentrantDeviceMonolithicCallback : ReentrantDeviceCallbackState {
  using ReentrantDeviceCallbackState::ReentrantDeviceCallbackState;

  /**
   * @brief Produces one deterministic result and records invocation.
   * @param node Borrowed operation node; unused.
   * @param inputs Borrowed input results; unused.
   * @return Result carrying the direct CPU monolithic marker.
   * @throws std::bad_alloc if diagnostic string storage cannot allocate.
   * @note Device snapshots and the HP bridge must produce the same marker.
   */
  NodeOutput operator()(const Node& node,
                        const std::vector<const NodeOutput*>& inputs) const {
    (void)node;
    (void)inputs;
    tracker->invocations.fetch_add(1, std::memory_order_relaxed);
    NodeOutput output;
    output.debug.compute_device = "DIRECT_STATEFUL_CPU_MONOLITHIC";
    return output;
  }
};

/**
 * @brief Stateful tiled device callback used by relocation tests.
 * @throws Nothing after construction.
 * @note Every copied target reports through the inherited tracker; the tracker
 *       must outlive all callback snapshots.
 */
struct ReentrantDeviceTiledCallback : ReentrantDeviceCallbackState {
  using ReentrantDeviceCallbackState::ReentrantDeviceCallbackState;

  /**
   * @brief Computes one deterministic observation from borrowed tile views.
   * @param node Borrowed operation node; unused.
   * @param output Borrowed output tile; unused.
   * @param inputs Borrowed input tiles; unused.
   * @return Nothing.
   * @throws Nothing.
   * @note Device snapshots and the HP bridge must publish the same observation.
   */
  void operator()(const Node& node, const OutputTile& output,
                  const std::vector<InputTile>& inputs) const {
    const int result = node.id + output.roi.x + output.roi.y +
                       output.roi.width + output.roi.height +
                       static_cast<int>(inputs.size());
    tracker->last_tiled_result.store(result, std::memory_order_release);
    tracker->invocations.fetch_add(1, std::memory_order_relaxed);
  }
};

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
    OpRegistry::instance().unregister_key(kLifecycleCpuDeviceKey);
    OpRegistry::instance().unregister_key(kLifecycleTaskShapeKey);
    OpRegistry::instance().unregister_key(kDirectCpuMonolithicKey);
    OpRegistry::instance().unregister_key(kDirectCpuTiledKey);
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
    OpRegistry::instance().unregister_key(kLifecycleCpuDeviceKey);
    OpRegistry::instance().unregister_key(kLifecycleTaskShapeKey);
    OpRegistry::instance().unregister_key(kDirectCpuMonolithicKey);
    OpRegistry::instance().unregister_key(kDirectCpuTiledKey);
  }
};

}  // namespace

TEST_F(PluginManagerLifecycleTest,
       RejectsV1OnlyPluginWithoutInvocationOrPublication) {
  const auto plugin_path = v1_only_plugin_path();
  ASSERT_TRUE(std::filesystem::exists(plugin_path))
      << "v1-only operation fixture was not built: " << plugin_path;
  const auto marker_path = lifecycle_trace_path("v1-only-invocation");
  std::filesystem::remove(marker_path);
  ScopedEnvironmentVariable marker_environment(kV1OnlyMarkerEnvironment,
                                               marker_path.string());

  auto& manager = PluginManager::process_instance();
  manager.seed_builtins_from_registry();
  const auto registry_keys_before = OpRegistry::instance().get_combined_keys();
  const auto sources_before = manager.op_sources();
  const std::size_t handles_before = manager.loaded_plugin_count();
  for (int attempt = 0; attempt < 2; ++attempt) {
    SCOPED_TRACE(attempt);
    const PluginLoadResult result =
        manager.load_from_dirs_report({plugin_path.parent_path().string()});
    EXPECT_EQ(result.attempted, 1);
    EXPECT_EQ(result.loaded, 0);
    ASSERT_EQ(result.errors.size(), 1u);
    EXPECT_EQ(result.errors.front().code, GraphErrc::InvalidParameter);
    EXPECT_NE(result.errors.front().message.find("register_photospider_ops_v2"),
              std::string::npos);
    EXPECT_TRUE(result.new_op_keys.empty());
    EXPECT_EQ(OpRegistry::instance().get_combined_keys(), registry_keys_before);
    EXPECT_EQ(manager.loaded_plugin_count(), handles_before);
    EXPECT_EQ(manager.op_sources(), sources_before);
    EXPECT_FALSE(std::filesystem::exists(marker_path));
  }
}

/**
 * @brief Proves a real plugin shape override and unload invalidate cached
 * FullTaskGraph tasks without a topology change.
 *
 * @throws Nothing when fixture loading, graph expansion, and unload complete.
 * @note The host first owns a four-tile implementation. The DSO adds a
 *       preferred monolithic HP slot for the same key while retaining the
 *       tiled predecessor in its independent slot, then unload restores the
 *       tiled predecessor. Execution resolution and task-shape planning must
 *       both honor monolithic HP precedence while both slots coexist. Every
 *       phase must receive a separately expanded task graph keyed by the
 *       registry task-shape generation.
 */
TEST_F(PluginManagerLifecycleTest,
       TaskGraphCacheTracksPluginShapeOverrideAndUnloadRestoration) {
  const auto plugin_path = lifecycle_plugin_path();
  ASSERT_TRUE(std::filesystem::exists(plugin_path));
  ScopedEnvironmentVariable shape_override_environment(
      kLifecycleTaskShapeOverrideEnvironment, "1");

  auto& registry = OpRegistry::instance();
  OpMetadata tiled_metadata;
  tiled_metadata.tile_preference = TileSizePreference::MICRO;
  registry.register_op_hp_tiled(
      kLifecycleType, kLifecycleTaskShapeSubtype,
      TileOpFunc(
          [](const Node&, const OutputTile&, const std::vector<InputTile>&) {}),
      tiled_metadata);

  GraphModel graph("cache/plugin-task-shape-generation");
  Node node;
  node.id = 1;
  node.name = "plugin_task_shape_generation";
  node.type = kLifecycleType;
  node.subtype = kLifecycleTaskShapeSubtype;
  node.parameters = YAML::Node(YAML::NodeType::Map);
  node.parameters["width"] = 64;
  node.parameters["height"] = 16;
  graph.add_node(node);

  const auto tiled_before = compute::get_or_expand_full_task_graph(
      graph, ComputeIntent::GlobalHighPrecision);
  ASSERT_EQ(tiled_before->task_graph.tasks.size(), 4u);
  EXPECT_TRUE(std::all_of(tiled_before->task_graph.tasks.begin(),
                          tiled_before->task_graph.tasks.end(),
                          [](const auto& task) {
                            return task.kind == compute::PlannedTaskKind::Tile;
                          }));
  const std::uint64_t tiled_generation = registry.task_shape_generation();

  auto& manager = PluginManager::process_instance();
  const PluginLoadResult result =
      manager.load_from_dirs_report({plugin_path.parent_path().string()});
  ASSERT_EQ(result.loaded, 1) << describe_errors(result.errors);
  EXPECT_NE(std::find(result.new_op_keys.begin(), result.new_op_keys.end(),
                      kLifecycleTaskShapeKey),
            result.new_op_keys.end());
  EXPECT_NE(registry.task_shape_generation(), tiled_generation);
  const auto merged_implementations =
      registry.get_implementations(kLifecycleType, kLifecycleTaskShapeSubtype);
  ASSERT_TRUE(merged_implementations.has_value());
  EXPECT_TRUE(merged_implementations->monolithic_hp.has_value());
  EXPECT_TRUE(merged_implementations->tiled_hp.has_value());
  const std::uint64_t monolithic_generation = registry.task_shape_generation();
  const auto resolved_override =
      registry.resolve_for_intent(kLifecycleType, kLifecycleTaskShapeSubtype,
                                  ComputeIntent::GlobalHighPrecision);
  ASSERT_TRUE(resolved_override.has_value());
  EXPECT_TRUE(std::holds_alternative<MonolithicOpFunc>(*resolved_override));

  const auto monolithic_override = compute::get_or_expand_full_task_graph(
      graph, ComputeIntent::GlobalHighPrecision);
  EXPECT_NE(monolithic_override.get(), tiled_before.get());
  ASSERT_EQ(monolithic_override->task_graph.tasks.size(), 1u);
  EXPECT_EQ(monolithic_override->task_graph.tasks.front().kind,
            compute::PlannedTaskKind::Monolithic);

  EXPECT_EQ(manager.unload_all_plugins(), 2);
  EXPECT_NE(registry.task_shape_generation(), monolithic_generation);
  const auto restored_implementations =
      registry.get_implementations(kLifecycleType, kLifecycleTaskShapeSubtype);
  ASSERT_TRUE(restored_implementations.has_value());
  EXPECT_FALSE(restored_implementations->monolithic_hp.has_value());
  EXPECT_TRUE(restored_implementations->tiled_hp.has_value());
  const auto resolved_predecessor =
      registry.resolve_for_intent(kLifecycleType, kLifecycleTaskShapeSubtype,
                                  ComputeIntent::GlobalHighPrecision);
  ASSERT_TRUE(resolved_predecessor.has_value());
  EXPECT_TRUE(std::holds_alternative<TileOpFunc>(*resolved_predecessor));
  const auto tiled_after_unload = compute::get_or_expand_full_task_graph(
      graph, ComputeIntent::GlobalHighPrecision);
  EXPECT_NE(tiled_after_unload.get(), monolithic_override.get());
  ASSERT_EQ(tiled_after_unload->task_graph.tasks.size(), 4u);
  EXPECT_TRUE(std::all_of(tiled_after_unload->task_graph.tasks.begin(),
                          tiled_after_unload->task_graph.tasks.end(),
                          [](const auto& task) {
                            return task.kind == compute::PlannedTaskKind::Tile;
                          }));
}

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
  ScopedDeviceCallbackRetirementInspection retirement_inspection;

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
    EXPECT_EQ(retirement_inspection.destructions(),
              static_cast<std::uint64_t>((cycle + 1) * 2));
    EXPECT_EQ(retirement_inspection.destructions_under_lock(), 0u);
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

  EXPECT_EQ(
      normalize_device_target_trace(read_lifecycle_trace(trace_path)),
      (std::vector<std::string>{
          "registrar_return", "device_tiled_target_destroy",
          "device_monolithic_target_destroy", "callback_destroy",
          "library_unload", "registrar_return", "device_tiled_target_destroy",
          "device_monolithic_target_destroy", "callback_destroy",
          "library_unload"}));
  std::filesystem::remove(trace_path);
}

/**
 * @brief Forces repeated stable-slot growth and mixed plugin/direct compaction.
 *
 * @throws Nothing when monolithic and tiled stateful targets observe every
 *         move/copy/destruction outside the registry lock.
 * @note The real lifecycle plugin contributes two leading device slots and a
 *       retained library. Twenty-four later direct slots force several vector
 *       growth steps; unloading the plugin then compacts those direct survivors
 *       across the removed prefix while preserving source and revision state.
 */
TEST_F(PluginManagerLifecycleTest,
       StatefulDeviceTargetsRetireOutsideLockAcrossGrowthAndCompaction) {
  constexpr int kCallbacksPerShape = 12;
  const auto plugin_path = lifecycle_plugin_path();
  ASSERT_TRUE(std::filesystem::exists(plugin_path));
  const auto trace_path = lifecycle_trace_path("stateful-device-slots");
  std::filesystem::remove(trace_path);
  ScopedEnvironmentVariable trace_environment(kLifecycleTraceEnvironment,
                                              trace_path.string());
  ScopedEnvironmentVariable device_environment(
      kLifecycleDeviceRegistrarEnvironment, "1");
  auto& manager = PluginManager::process_instance();
  auto& registry = OpRegistry::instance();

  const auto result =
      manager.load_from_dirs_report({plugin_path.parent_path().string()});
  ASSERT_EQ(result.loaded, 1) << describe_errors(result.errors);
  ASSERT_TRUE(result.errors.empty()) << describe_errors(result.errors);
  expect_lifecycle_device_ownership_alignment(2);

  ReentrantDeviceCallbackTracker monolithic_tracker;
  monolithic_tracker.registry = &registry;
  ScopedOpRegistryKeyCleanup monolithic_callback_cleanup(
      registry, kLifecycleType, kLifecycleSubtype);
  ReentrantDeviceCallbackTracker tiled_tracker;
  tiled_tracker.registry = &registry;
  ScopedOpRegistryKeyCleanup tiled_callback_cleanup(registry, kLifecycleType,
                                                    kLifecycleSubtype);
  for (int index = 0; index < kCallbacksPerShape; ++index) {
    OpMetadata monolithic_metadata;
    monolithic_metadata.cost_score = 100 + index;
    MonolithicOpFunc monolithic =
        ReentrantDeviceMonolithicCallback(monolithic_tracker);
    registry.register_impl(kLifecycleType, kLifecycleSubtype, Device::ASIC_NPU,
                           std::move(monolithic), monolithic_metadata);

    OpMetadata tiled_metadata;
    tiled_metadata.cost_score = 200 + index;
    tiled_metadata.tile_preference = TileSizePreference::MICRO;
    TileOpFunc tiled = ReentrantDeviceTiledCallback(tiled_tracker);
    registry.register_impl(kLifecycleType, kLifecycleSubtype, Device::GPU_CUDA,
                           std::move(tiled), tiled_metadata);
  }

  expect_lifecycle_device_ownership_alignment(2 + 2 * kCallbacksPerShape);
  EXPECT_EQ(monolithic_tracker.live_targets.load(std::memory_order_acquire),
            kCallbacksPerShape);
  EXPECT_EQ(tiled_tracker.live_targets.load(std::memory_order_acquire),
            kCallbacksPerShape);
  EXPECT_FALSE(
      monolithic_tracker.observed_under_lock.load(std::memory_order_acquire));
  EXPECT_FALSE(
      tiled_tracker.observed_under_lock.load(std::memory_order_acquire));
  EXPECT_FALSE(
      monolithic_tracker.reentry_failed.load(std::memory_order_acquire));
  EXPECT_FALSE(tiled_tracker.reentry_failed.load(std::memory_order_acquire));
  EXPECT_EQ(manager.op_sources().at(kLifecycleKey), "mixed");

  EXPECT_EQ(manager.unload_all_plugins(), 1);
  EXPECT_EQ(manager.loaded_plugin_count(), 0u);
  EXPECT_EQ(manager.op_sources().at(kLifecycleKey), "built-in");
  expect_lifecycle_device_ownership_alignment(2 * kCallbacksPerShape);
  EXPECT_EQ(normalize_device_target_trace(read_lifecycle_trace(trace_path)),
            (std::vector<std::string>{"registrar_return",
                                      "device_tiled_target_destroy",
                                      "device_monolithic_target_destroy",
                                      "callback_destroy", "library_unload"}));

  {
    const auto implementations =
        registry.get_all_implementations(kLifecycleType, kLifecycleSubtype);
    EXPECT_EQ(implementations.size(), 2u * kCallbacksPerShape);
    EXPECT_EQ(std::count_if(implementations.begin(), implementations.end(),
                            [](const OpImplementation& implementation) {
                              return implementation.is_monolithic();
                            }),
              kCallbacksPerShape);
    EXPECT_EQ(std::count_if(implementations.begin(), implementations.end(),
                            [](const OpImplementation& implementation) {
                              return implementation.is_tiled();
                            }),
              kCallbacksPerShape);
  }
  EXPECT_EQ(monolithic_tracker.live_targets.load(std::memory_order_acquire),
            kCallbacksPerShape);
  EXPECT_EQ(tiled_tracker.live_targets.load(std::memory_order_acquire),
            kCallbacksPerShape);

  const bool callbacks_removed = registry.unregister_key(kLifecycleKey);
  if (callbacks_removed) {
    monolithic_callback_cleanup.disarm();
    tiled_callback_cleanup.disarm();
  }
  EXPECT_TRUE(callbacks_removed);
  EXPECT_EQ(monolithic_tracker.live_targets.load(std::memory_order_acquire), 0);
  EXPECT_EQ(tiled_tracker.live_targets.load(std::memory_order_acquire), 0);
  EXPECT_GE(monolithic_tracker.destructions.load(std::memory_order_acquire),
            kCallbacksPerShape);
  EXPECT_GE(tiled_tracker.destructions.load(std::memory_order_acquire),
            kCallbacksPerShape);
  EXPECT_FALSE(
      monolithic_tracker.observed_under_lock.load(std::memory_order_acquire));
  EXPECT_FALSE(
      tiled_tracker.observed_under_lock.load(std::memory_order_acquire));
  EXPECT_FALSE(
      monolithic_tracker.reentry_failed.load(std::memory_order_acquire));
  EXPECT_FALSE(tiled_tracker.reentry_failed.load(std::memory_order_acquire));
  std::filesystem::remove(trace_path);
}

/**
 * @brief Proves direct stateful CPU monolithic registration shares one owner.
 *
 * @throws Nothing when CPU registration adds no target transfer beyond the
 *         same-shape non-CPU baseline and device snapshots copy outside lock.
 * @note This path calls `OpRegistry::register_impl` directly and therefore
 *       exercises no plugin-retained wrapper. The transfer baseline makes the
 *       proof independent of `std::function` move internals; the final bridge
 *       release retires the original owner after whole-key unregister and
 *       outside the registry lock.
 */
TEST_F(PluginManagerLifecycleTest,
       DirectCpuMonolithicBridgeDoesNotCopyOriginalTarget) {
  auto& registry = OpRegistry::instance();
  constexpr const char* kTransferControlSubtype = "monolithic_transfer_control";
  const std::string transfer_control_key =
      make_key(kDirectCpuOwnerType, kTransferControlSubtype);
  (void)registry.unregister_key(transfer_control_key);
  ReentrantDeviceCallbackTracker transfer_control;
  transfer_control.registry = &registry;
  ScopedOpRegistryKeyCleanup transfer_control_cleanup(
      registry, kDirectCpuOwnerType, kTransferControlSubtype);
  MonolithicOpFunc transfer_control_candidate =
      ReentrantDeviceMonolithicCallback(transfer_control);
  const int control_copies_before =
      transfer_control.copies.load(std::memory_order_acquire);
  const int control_moves_before =
      transfer_control.moves.load(std::memory_order_acquire);
  const int control_live_before =
      transfer_control.live_targets.load(std::memory_order_acquire);
  registry.register_impl(kDirectCpuOwnerType, kTransferControlSubtype,
                         Device::GPU_METAL,
                         std::move(transfer_control_candidate), OpMetadata{});
  const int control_copy_delta =
      transfer_control.copies.load(std::memory_order_acquire) -
      control_copies_before;
  const int control_move_delta =
      transfer_control.moves.load(std::memory_order_acquire) -
      control_moves_before;
  const int control_live_delta =
      transfer_control.live_targets.load(std::memory_order_acquire) -
      control_live_before;
  transfer_control_candidate = MonolithicOpFunc{};
  const bool transfer_control_removed =
      registry.unregister_key(transfer_control_key);
  if (transfer_control_removed) {
    transfer_control_cleanup.disarm();
  }
  EXPECT_TRUE(transfer_control_removed);
  EXPECT_EQ(transfer_control.live_targets.load(std::memory_order_acquire), 0);
  EXPECT_FALSE(
      transfer_control.observed_under_lock.load(std::memory_order_acquire));
  EXPECT_FALSE(transfer_control.reentry_failed.load(std::memory_order_acquire));

  ReentrantDeviceCallbackTracker tracker;
  tracker.registry = &registry;
  ScopedOpRegistryKeyCleanup registered_callback_cleanup(
      registry, kDirectCpuOwnerType, kDirectCpuMonolithicSubtype);

  MonolithicOpFunc candidate = ReentrantDeviceMonolithicCallback(tracker);
  ASSERT_EQ(tracker.constructions.load(std::memory_order_acquire), 1);
  ASSERT_EQ(tracker.copies.load(std::memory_order_acquire), 0);
  ASSERT_EQ(tracker.live_targets.load(std::memory_order_acquire), 1);
  const int copies_before_registration =
      tracker.copies.load(std::memory_order_acquire);
  const int moves_before_registration =
      tracker.moves.load(std::memory_order_acquire);
  const int live_before_registration =
      tracker.live_targets.load(std::memory_order_acquire);

  registry.register_impl(kDirectCpuOwnerType, kDirectCpuMonolithicSubtype,
                         Device::CPU, std::move(candidate), OpMetadata{});
  const int copies_after_registration =
      tracker.copies.load(std::memory_order_acquire);
  const int moves_after_registration =
      tracker.moves.load(std::memory_order_acquire);
  const int live_after_registration =
      tracker.live_targets.load(std::memory_order_acquire);
  EXPECT_EQ(copies_after_registration - copies_before_registration,
            control_copy_delta);
  EXPECT_EQ(moves_after_registration - moves_before_registration,
            control_move_delta);
  EXPECT_EQ(live_after_registration - live_before_registration,
            control_live_delta);
  candidate = MonolithicOpFunc{};
  EXPECT_FALSE(candidate);
  EXPECT_EQ(tracker.live_targets.load(std::memory_order_acquire), 1);
  const int destructions_before_snapshots =
      tracker.destructions.load(std::memory_order_acquire);

  const auto ownership =
      testing::inspect_op_registry_device_ownership_for_testing(
          registry, kDirectCpuMonolithicKey);
  ASSERT_TRUE(ownership.implementation_entry_present);
  ASSERT_TRUE(ownership.ownership_entry_present);
  ASSERT_EQ(ownership.implementation_count, 1u);
  ASSERT_EQ(ownership.revision_count, 1u);
  ASSERT_NE(ownership.first_device_revision, 0u);
  EXPECT_EQ(ownership.monolithic_hp_revision, ownership.first_device_revision);
  EXPECT_EQ(ownership.meta_hp_revision, ownership.first_device_revision);

  auto hp_bridge = registry.resolve_for_intent(
      kDirectCpuOwnerType, kDirectCpuMonolithicSubtype,
      ComputeIntent::GlobalHighPrecision);
  ASSERT_TRUE(hp_bridge.has_value());
  ASSERT_TRUE(std::holds_alternative<MonolithicOpFunc>(*hp_bridge));
  EXPECT_EQ(tracker.copies.load(std::memory_order_acquire),
            copies_after_registration);
  std::optional<OpRegistry::OpVariant> hp_bridge_copy;
  hp_bridge_copy.emplace(*hp_bridge);
  EXPECT_EQ(tracker.copies.load(std::memory_order_acquire),
            copies_after_registration);

  auto device_snapshots = registry.get_implementations_by_device(
      kDirectCpuOwnerType, kDirectCpuMonolithicSubtype, Device::CPU);
  ASSERT_EQ(device_snapshots.size(), 1u);
  ASSERT_TRUE(device_snapshots.front().is_monolithic());
  EXPECT_EQ(tracker.copies.load(std::memory_order_acquire),
            copies_after_registration + 1);
  std::optional<OpImplementation> device_snapshot_copy;
  device_snapshot_copy.emplace(device_snapshots.front());
  EXPECT_EQ(tracker.copies.load(std::memory_order_acquire),
            copies_after_registration + 2);
  EXPECT_EQ(tracker.live_targets.load(std::memory_order_acquire), 3);

  Node node;
  node.id = 17;
  node.type = kDirectCpuOwnerType;
  node.subtype = kDirectCpuMonolithicSubtype;
  const NodeOutput device_output =
      std::get<MonolithicOpFunc>(device_snapshot_copy->func)(node, {});
  const NodeOutput bridge_output =
      std::get<MonolithicOpFunc>(*hp_bridge_copy)(node, {});
  EXPECT_EQ(device_output.debug.compute_device,
            "DIRECT_STATEFUL_CPU_MONOLITHIC");
  EXPECT_EQ(bridge_output.debug.compute_device,
            device_output.debug.compute_device);
  EXPECT_EQ(tracker.invocations.load(std::memory_order_acquire), 2);

  const bool registered_callback_removed =
      registry.unregister_key(kDirectCpuMonolithicKey);
  if (registered_callback_removed) {
    registered_callback_cleanup.disarm();
  }
  EXPECT_TRUE(registered_callback_removed);
  const auto removed =
      testing::inspect_op_registry_device_ownership_for_testing(
          registry, kDirectCpuMonolithicKey);
  EXPECT_FALSE(removed.implementation_entry_present);
  EXPECT_FALSE(removed.ownership_entry_present);
  EXPECT_EQ(removed.implementation_count, 0u);
  EXPECT_EQ(removed.revision_count, 0u);
  EXPECT_EQ(tracker.live_targets.load(std::memory_order_acquire), 3);

  device_snapshot_copy.reset();
  EXPECT_EQ(tracker.live_targets.load(std::memory_order_acquire), 2);
  device_snapshots.clear();
  EXPECT_EQ(tracker.live_targets.load(std::memory_order_acquire), 1);
  hp_bridge_copy.reset();
  EXPECT_EQ(tracker.live_targets.load(std::memory_order_acquire), 1);
  hp_bridge.reset();
  EXPECT_EQ(tracker.live_targets.load(std::memory_order_acquire), 0);
  EXPECT_EQ(tracker.destructions.load(std::memory_order_acquire),
            destructions_before_snapshots + 3);
  EXPECT_EQ(tracker.copies.load(std::memory_order_acquire),
            copies_after_registration + 2);
  EXPECT_FALSE(tracker.observed_under_lock.load(std::memory_order_acquire));
  EXPECT_FALSE(tracker.reentry_failed.load(std::memory_order_acquire));
}

/**
 * @brief Proves direct stateful CPU tiled registration shares one owner.
 *
 * @throws Nothing when CPU registration adds no target transfer beyond the
 *         same-shape non-CPU baseline and device snapshots copy outside lock.
 * @note The device snapshot and HP tiled bridge receive identical borrowed
 *       views and must report the same deterministic observation. A non-CPU
 *       transfer control isolates standard-library move behavior before final
 *       bridge release retires the original target outside lock.
 */
TEST_F(PluginManagerLifecycleTest,
       DirectCpuTiledBridgeDoesNotCopyOriginalTarget) {
  auto& registry = OpRegistry::instance();
  constexpr const char* kTransferControlSubtype = "tiled_transfer_control";
  const std::string transfer_control_key =
      make_key(kDirectCpuOwnerType, kTransferControlSubtype);
  (void)registry.unregister_key(transfer_control_key);
  ReentrantDeviceCallbackTracker transfer_control;
  transfer_control.registry = &registry;
  ScopedOpRegistryKeyCleanup transfer_control_cleanup(
      registry, kDirectCpuOwnerType, kTransferControlSubtype);
  TileOpFunc transfer_control_candidate =
      ReentrantDeviceTiledCallback(transfer_control);
  const int control_copies_before =
      transfer_control.copies.load(std::memory_order_acquire);
  const int control_moves_before =
      transfer_control.moves.load(std::memory_order_acquire);
  const int control_live_before =
      transfer_control.live_targets.load(std::memory_order_acquire);
  registry.register_impl(kDirectCpuOwnerType, kTransferControlSubtype,
                         Device::GPU_METAL,
                         std::move(transfer_control_candidate), OpMetadata{});
  const int control_copy_delta =
      transfer_control.copies.load(std::memory_order_acquire) -
      control_copies_before;
  const int control_move_delta =
      transfer_control.moves.load(std::memory_order_acquire) -
      control_moves_before;
  const int control_live_delta =
      transfer_control.live_targets.load(std::memory_order_acquire) -
      control_live_before;
  transfer_control_candidate = TileOpFunc{};
  const bool transfer_control_removed =
      registry.unregister_key(transfer_control_key);
  if (transfer_control_removed) {
    transfer_control_cleanup.disarm();
  }
  EXPECT_TRUE(transfer_control_removed);
  EXPECT_EQ(transfer_control.live_targets.load(std::memory_order_acquire), 0);
  EXPECT_FALSE(
      transfer_control.observed_under_lock.load(std::memory_order_acquire));
  EXPECT_FALSE(transfer_control.reentry_failed.load(std::memory_order_acquire));

  ReentrantDeviceCallbackTracker tracker;
  tracker.registry = &registry;
  ScopedOpRegistryKeyCleanup registered_callback_cleanup(
      registry, kDirectCpuOwnerType, kDirectCpuTiledSubtype);

  TileOpFunc candidate = ReentrantDeviceTiledCallback(tracker);
  ASSERT_EQ(tracker.constructions.load(std::memory_order_acquire), 1);
  ASSERT_EQ(tracker.copies.load(std::memory_order_acquire), 0);
  ASSERT_EQ(tracker.live_targets.load(std::memory_order_acquire), 1);
  const int copies_before_registration =
      tracker.copies.load(std::memory_order_acquire);
  const int moves_before_registration =
      tracker.moves.load(std::memory_order_acquire);
  const int live_before_registration =
      tracker.live_targets.load(std::memory_order_acquire);

  OpMetadata metadata;
  metadata.tile_preference = TileSizePreference::MICRO;
  registry.register_impl(kDirectCpuOwnerType, kDirectCpuTiledSubtype,
                         Device::CPU, std::move(candidate), metadata);
  const int copies_after_registration =
      tracker.copies.load(std::memory_order_acquire);
  const int moves_after_registration =
      tracker.moves.load(std::memory_order_acquire);
  const int live_after_registration =
      tracker.live_targets.load(std::memory_order_acquire);
  EXPECT_EQ(copies_after_registration - copies_before_registration,
            control_copy_delta);
  EXPECT_EQ(moves_after_registration - moves_before_registration,
            control_move_delta);
  EXPECT_EQ(live_after_registration - live_before_registration,
            control_live_delta);
  candidate = TileOpFunc{};
  EXPECT_FALSE(candidate);
  EXPECT_EQ(tracker.live_targets.load(std::memory_order_acquire), 1);
  const int destructions_before_snapshots =
      tracker.destructions.load(std::memory_order_acquire);

  const auto ownership =
      testing::inspect_op_registry_device_ownership_for_testing(
          registry, kDirectCpuTiledKey);
  ASSERT_TRUE(ownership.implementation_entry_present);
  ASSERT_TRUE(ownership.ownership_entry_present);
  ASSERT_EQ(ownership.implementation_count, 1u);
  ASSERT_EQ(ownership.revision_count, 1u);
  ASSERT_NE(ownership.first_device_revision, 0u);
  EXPECT_EQ(ownership.tiled_hp_revision, ownership.first_device_revision);
  EXPECT_EQ(ownership.meta_hp_revision, ownership.first_device_revision);

  auto hp_bridge =
      registry.resolve_for_intent(kDirectCpuOwnerType, kDirectCpuTiledSubtype,
                                  ComputeIntent::GlobalHighPrecision);
  ASSERT_TRUE(hp_bridge.has_value());
  ASSERT_TRUE(std::holds_alternative<TileOpFunc>(*hp_bridge));
  EXPECT_EQ(tracker.copies.load(std::memory_order_acquire),
            copies_after_registration);
  std::optional<OpRegistry::OpVariant> hp_bridge_copy;
  hp_bridge_copy.emplace(*hp_bridge);
  EXPECT_EQ(tracker.copies.load(std::memory_order_acquire),
            copies_after_registration);

  auto device_snapshots = registry.get_implementations_by_device(
      kDirectCpuOwnerType, kDirectCpuTiledSubtype, Device::CPU);
  ASSERT_EQ(device_snapshots.size(), 1u);
  ASSERT_TRUE(device_snapshots.front().is_tiled());
  EXPECT_EQ(tracker.copies.load(std::memory_order_acquire),
            copies_after_registration + 1);
  std::optional<OpImplementation> device_snapshot_copy;
  device_snapshot_copy.emplace(device_snapshots.front());
  EXPECT_EQ(tracker.copies.load(std::memory_order_acquire),
            copies_after_registration + 2);
  EXPECT_EQ(tracker.live_targets.load(std::memory_order_acquire), 3);

  Node node;
  node.id = 29;
  node.type = kDirectCpuOwnerType;
  node.subtype = kDirectCpuTiledSubtype;
  OutputTile output;
  output.roi = cv::Rect(2, 3, 5, 7);
  const std::vector<InputTile> inputs(4);
  tracker.last_tiled_result.store(-1, std::memory_order_release);
  std::get<TileOpFunc>(device_snapshot_copy->func)(node, output, inputs);
  const int device_result =
      tracker.last_tiled_result.load(std::memory_order_acquire);
  tracker.last_tiled_result.store(-1, std::memory_order_release);
  std::get<TileOpFunc> (*hp_bridge_copy)(node, output, inputs);
  const int bridge_result =
      tracker.last_tiled_result.load(std::memory_order_acquire);
  EXPECT_EQ(device_result, 50);
  EXPECT_EQ(bridge_result, device_result);
  EXPECT_EQ(tracker.invocations.load(std::memory_order_acquire), 2);

  const bool registered_callback_removed =
      registry.unregister_key(kDirectCpuTiledKey);
  if (registered_callback_removed) {
    registered_callback_cleanup.disarm();
  }
  EXPECT_TRUE(registered_callback_removed);
  const auto removed =
      testing::inspect_op_registry_device_ownership_for_testing(
          registry, kDirectCpuTiledKey);
  EXPECT_FALSE(removed.implementation_entry_present);
  EXPECT_FALSE(removed.ownership_entry_present);
  EXPECT_EQ(removed.implementation_count, 0u);
  EXPECT_EQ(removed.revision_count, 0u);
  EXPECT_EQ(tracker.live_targets.load(std::memory_order_acquire), 3);

  device_snapshot_copy.reset();
  EXPECT_EQ(tracker.live_targets.load(std::memory_order_acquire), 2);
  device_snapshots.clear();
  EXPECT_EQ(tracker.live_targets.load(std::memory_order_acquire), 1);
  hp_bridge_copy.reset();
  EXPECT_EQ(tracker.live_targets.load(std::memory_order_acquire), 1);
  hp_bridge.reset();
  EXPECT_EQ(tracker.live_targets.load(std::memory_order_acquire), 0);
  EXPECT_EQ(tracker.destructions.load(std::memory_order_acquire),
            destructions_before_snapshots + 3);
  EXPECT_EQ(tracker.copies.load(std::memory_order_acquire),
            copies_after_registration + 2);
  EXPECT_FALSE(tracker.observed_under_lock.load(std::memory_order_acquire));
  EXPECT_FALSE(tracker.reentry_failed.load(std::memory_order_acquire));
}

/**
 * @brief Retains one stateful CPU device reader and its HP bridge across
 * unload.
 *
 * @throws Nothing when both copied paths remain callable and share one original
 *         plugin target until the final bridge owner is released.
 * @note The fixture traces every genuine original-target copy. Reader
 * snapshots, bridge copying, unload, and post-unload invocation must not
 * increase that count. The host observer proves final target retirement begins
 * outside the registry lock, and the DSO trace proves it precedes library
 * unload.
 */
TEST_F(PluginManagerLifecycleTest,
       CpuDeviceBridgeSharesStableOwnerAcrossReaderCopyAndUnload) {
  const auto plugin_path = lifecycle_plugin_path();
  ASSERT_TRUE(std::filesystem::exists(plugin_path));
  const auto trace_path = lifecycle_trace_path("cpu-device-stable-owner");
  std::filesystem::remove(trace_path);
  ScopedEnvironmentVariable trace_environment(kLifecycleTraceEnvironment,
                                              trace_path.string());
  ScopedEnvironmentVariable cpu_device_environment(
      kLifecycleCpuDeviceRegistrarEnvironment, "1");
  auto& manager = PluginManager::process_instance();
  auto& registry = OpRegistry::instance();
  ScopedDeviceCallbackRetirementInspection retirement_inspection;

  const auto result =
      manager.load_from_dirs_report({plugin_path.parent_path().string()});
  ASSERT_EQ(result.loaded, 1) << describe_errors(result.errors);
  ASSERT_TRUE(result.errors.empty()) << describe_errors(result.errors);
  EXPECT_NE(std::find(result.new_op_keys.begin(), result.new_op_keys.end(),
                      kLifecycleCpuDeviceKey),
            result.new_op_keys.end());
  const std::string absolute_path =
      std::filesystem::absolute(plugin_path).string();
  ASSERT_EQ(manager.op_sources().at(kLifecycleCpuDeviceKey), absolute_path);

  const auto ownership =
      testing::inspect_op_registry_device_ownership_for_testing(
          registry, kLifecycleCpuDeviceKey);
  ASSERT_TRUE(ownership.implementation_entry_present);
  ASSERT_TRUE(ownership.ownership_entry_present);
  ASSERT_EQ(ownership.implementation_count, 1u);
  ASSERT_EQ(ownership.revision_count, 1u);
  ASSERT_NE(ownership.first_device_revision, 0u);
  EXPECT_EQ(ownership.monolithic_hp_revision, ownership.first_device_revision);
  EXPECT_EQ(ownership.meta_hp_revision, ownership.first_device_revision);

  const std::size_t registration_target_copies = count_lifecycle_event(
      read_lifecycle_trace(trace_path), "cpu_device_target_copy");
  std::optional<OpImplementation> device_reader;
  std::optional<OpRegistry::OpVariant> hp_bridge_reader;
  {
    auto implementations = registry.get_implementations_by_device(
        kLifecycleType, kLifecycleCpuDeviceSubtype, Device::CPU);
    ASSERT_EQ(implementations.size(), 1u);
    ASSERT_TRUE(implementations.front().is_monolithic());
    device_reader = implementations.front();

    auto resolved =
        registry.resolve_for_intent(kLifecycleType, kLifecycleCpuDeviceSubtype,
                                    ComputeIntent::GlobalHighPrecision);
    ASSERT_TRUE(resolved.has_value());
    ASSERT_TRUE(std::holds_alternative<MonolithicOpFunc>(*resolved));
    hp_bridge_reader = *resolved;
  }
  EXPECT_EQ(count_lifecycle_event(read_lifecycle_trace(trace_path),
                                  "cpu_device_target_copy"),
            registration_target_copies);

  Node node;
  node.type = kLifecycleType;
  node.subtype = kLifecycleCpuDeviceSubtype;
  {
    NodeOutput device_output =
        std::get<MonolithicOpFunc>(device_reader->func)(node, {});
    NodeOutput bridge_output =
        std::get<MonolithicOpFunc>(*hp_bridge_reader)(node, {});
    EXPECT_EQ(device_output.debug.compute_device,
              "PLUGIN_CPU_DEVICE_MONOLITHIC");
    EXPECT_EQ(bridge_output.debug.compute_device,
              device_output.debug.compute_device);
  }

  EXPECT_EQ(manager.unload_all_plugins(), 2);
  EXPECT_EQ(manager.loaded_plugin_count(), 0u);
  EXPECT_EQ(manager.op_sources().count(kLifecycleCpuDeviceKey), 0u);
  EXPECT_FALSE(
      registry.get_implementations(kLifecycleType, kLifecycleCpuDeviceSubtype));
  EXPECT_EQ(retirement_inspection.destructions(), 0u);
  EXPECT_EQ(retirement_inspection.destructions_under_lock(), 0u);
  EXPECT_EQ(count_lifecycle_event(read_lifecycle_trace(trace_path),
                                  "cpu_device_target_destroy"),
            0u);
  EXPECT_EQ(
      count_lifecycle_event(read_lifecycle_trace(trace_path), "library_unload"),
      0u);

  {
    NodeOutput device_output =
        std::get<MonolithicOpFunc>(device_reader->func)(node, {});
    NodeOutput bridge_output =
        std::get<MonolithicOpFunc>(*hp_bridge_reader)(node, {});
    EXPECT_EQ(device_output.debug.compute_device,
              "PLUGIN_CPU_DEVICE_MONOLITHIC");
    EXPECT_EQ(bridge_output.debug.compute_device,
              device_output.debug.compute_device);
  }
  EXPECT_EQ(count_lifecycle_event(read_lifecycle_trace(trace_path),
                                  "cpu_device_target_copy"),
            registration_target_copies);

  device_reader.reset();
  EXPECT_EQ(retirement_inspection.destructions(), 0u);
  EXPECT_EQ(count_lifecycle_event(read_lifecycle_trace(trace_path),
                                  "cpu_device_target_destroy"),
            0u);
  hp_bridge_reader.reset();

  EXPECT_EQ(retirement_inspection.destructions(), 1u);
  EXPECT_EQ(retirement_inspection.destructions_under_lock(), 0u);
  const auto final_trace = read_lifecycle_trace(trace_path);
  EXPECT_EQ(count_lifecycle_event(final_trace, "cpu_device_target_copy"),
            registration_target_copies);
  EXPECT_EQ(count_lifecycle_event(final_trace, "cpu_device_target_destroy"),
            1u);
  EXPECT_EQ(count_lifecycle_event(final_trace, "library_unload"), 1u);
  const auto target_destroy = std::find(final_trace.begin(), final_trace.end(),
                                        "cpu_device_target_destroy");
  const auto library_unload =
      std::find(final_trace.begin(), final_trace.end(), "library_unload");
  ASSERT_NE(target_destroy, final_trace.end());
  ASSERT_NE(library_unload, final_trace.end());
  EXPECT_LT(target_destroy, library_unload);
  std::filesystem::remove(trace_path);
}

/**
 * @brief Injects exact stable-owner and CPU-bridge construction failures.
 *
 * @throws Nothing when all four deterministic failures preserve registry state.
 * @note Monolithic and tiled baseline keys each retain one callable value, one
 *       exact device revision, built-in source attribution, and the complete
 *       process registry inventory. Each failpoint must report one real hit;
 *       this test never relies on the test executable's global `operator new`.
 */
TEST_F(PluginManagerLifecycleTest,
       DeviceRegistrationFailpointsPreserveValuesRevisionsAndSources) {
  using Failpoint = testing::OpRegistryDeviceRegistrationFailpoint;
  auto& registry = OpRegistry::instance();
  auto& manager = PluginManager::process_instance();
  constexpr const char* kType = "device_registration_failure";
  constexpr const char* kMonolithicSubtype = "monolithic";
  constexpr const char* kTiledSubtype = "tiled";
  const std::string monolithic_key = make_key(kType, kMonolithicSubtype);
  const std::string tiled_key = make_key(kType, kTiledSubtype);
  registry.unregister_key(monolithic_key);
  ScopedOpRegistryKeyCleanup monolithic_callback_cleanup(registry, kType,
                                                         kMonolithicSubtype);
  registry.unregister_key(tiled_key);

  OpMetadata monolithic_metadata;
  monolithic_metadata.cost_score = 101;
  registry.register_impl(
      kType, kMonolithicSubtype, Device::ASIC_NPU,
      [](const Node&, const std::vector<const NodeOutput*>&) {
        NodeOutput output;
        output.debug.compute_device = "BASELINE_DEVICE_MONOLITHIC";
        return output;
      },
      monolithic_metadata);

  std::atomic<int> tiled_invocations{0};
  ScopedOpRegistryKeyCleanup tiled_callback_cleanup(registry, kType,
                                                    kTiledSubtype);
  OpMetadata tiled_metadata;
  tiled_metadata.cost_score = 202;
  tiled_metadata.tile_preference = TileSizePreference::MICRO;
  registry.register_impl(
      kType, kTiledSubtype, Device::GPU_CUDA,
      [&tiled_invocations](const Node&, const OutputTile&,
                           const std::vector<InputTile>&) {
        tiled_invocations.fetch_add(1, std::memory_order_relaxed);
      },
      tiled_metadata);
  manager.seed_builtins_from_registry();

  const auto baseline_keys = registry.get_combined_keys();
  const auto baseline_sources = manager.op_sources();
  const auto baseline_monolithic =
      testing::inspect_op_registry_device_ownership_for_testing(registry,
                                                                monolithic_key);
  const auto baseline_tiled =
      testing::inspect_op_registry_device_ownership_for_testing(registry,
                                                                tiled_key);
  ASSERT_TRUE(baseline_monolithic.implementation_entry_present);
  ASSERT_TRUE(baseline_monolithic.ownership_entry_present);
  ASSERT_EQ(baseline_monolithic.implementation_count, 1u);
  ASSERT_EQ(baseline_monolithic.revision_count, 1u);
  ASSERT_NE(baseline_monolithic.first_device_revision, 0u);
  ASSERT_TRUE(baseline_tiled.implementation_entry_present);
  ASSERT_TRUE(baseline_tiled.ownership_entry_present);
  ASSERT_EQ(baseline_tiled.implementation_count, 1u);
  ASSERT_EQ(baseline_tiled.revision_count, 1u);
  ASSERT_NE(baseline_tiled.first_device_revision, 0u);
  ASSERT_EQ(baseline_sources.at(monolithic_key), "built-in");
  ASSERT_EQ(baseline_sources.at(tiled_key), "built-in");

  testing::set_op_registry_device_registration_failpoint(
      Failpoint::StableOwner);
  EXPECT_THROW(registry.register_impl(
                   kType, kMonolithicSubtype, Device::GPU_METAL,
                   [](const Node&, const std::vector<const NodeOutput*>&) {
                     return NodeOutput{};
                   }),
               std::bad_alloc);
  EXPECT_EQ(testing::op_registry_device_registration_failpoint_hits(), 1u);
  testing::set_op_registry_device_registration_failpoint(Failpoint::None);
  EXPECT_EQ(registry.get_combined_keys(), baseline_keys);
  EXPECT_EQ(manager.op_sources(), baseline_sources);
  expect_device_ownership_inspection_equal(
      baseline_monolithic,
      testing::inspect_op_registry_device_ownership_for_testing(
          registry, monolithic_key));

  testing::set_op_registry_device_registration_failpoint(
      Failpoint::CpuCompatibilityBridge);
  EXPECT_THROW(registry.register_impl(
                   kType, kMonolithicSubtype, Device::CPU,
                   [](const Node&, const std::vector<const NodeOutput*>&) {
                     return NodeOutput{};
                   }),
               std::bad_alloc);
  EXPECT_EQ(testing::op_registry_device_registration_failpoint_hits(), 1u);
  testing::set_op_registry_device_registration_failpoint(Failpoint::None);
  EXPECT_EQ(registry.get_combined_keys(), baseline_keys);
  EXPECT_EQ(manager.op_sources(), baseline_sources);
  expect_device_ownership_inspection_equal(
      baseline_monolithic,
      testing::inspect_op_registry_device_ownership_for_testing(
          registry, monolithic_key));

  auto monolithic_values =
      registry.get_all_implementations(kType, kMonolithicSubtype);
  ASSERT_EQ(monolithic_values.size(), 1u);
  EXPECT_TRUE(monolithic_values.front().is_monolithic());
  EXPECT_EQ(monolithic_values.front().metadata.device_preference,
            Device::ASIC_NPU);
  EXPECT_EQ(monolithic_values.front().metadata.cost_score, 101);
  Node monolithic_node;
  const auto monolithic_output = std::get<MonolithicOpFunc>(
      monolithic_values.front().func)(monolithic_node, {});
  EXPECT_EQ(monolithic_output.debug.compute_device,
            "BASELINE_DEVICE_MONOLITHIC");
  EXPECT_FALSE(registry.resolve_for_intent(kType, kMonolithicSubtype,
                                           ComputeIntent::GlobalHighPrecision));

  testing::set_op_registry_device_registration_failpoint(
      Failpoint::StableOwner);
  EXPECT_THROW(
      registry.register_impl(
          kType, kTiledSubtype, Device::GPU_METAL,
          [](const Node&, const OutputTile&, const std::vector<InputTile>&) {},
          OpMetadata{}),
      std::bad_alloc);
  EXPECT_EQ(testing::op_registry_device_registration_failpoint_hits(), 1u);
  testing::set_op_registry_device_registration_failpoint(Failpoint::None);
  EXPECT_EQ(registry.get_combined_keys(), baseline_keys);
  EXPECT_EQ(manager.op_sources(), baseline_sources);
  expect_device_ownership_inspection_equal(
      baseline_tiled, testing::inspect_op_registry_device_ownership_for_testing(
                          registry, tiled_key));

  testing::set_op_registry_device_registration_failpoint(
      Failpoint::CpuCompatibilityBridge);
  EXPECT_THROW(
      registry.register_impl(
          kType, kTiledSubtype, Device::CPU,
          [](const Node&, const OutputTile&, const std::vector<InputTile>&) {},
          OpMetadata{}),
      std::bad_alloc);
  EXPECT_EQ(testing::op_registry_device_registration_failpoint_hits(), 1u);
  testing::set_op_registry_device_registration_failpoint(Failpoint::None);
  EXPECT_EQ(registry.get_combined_keys(), baseline_keys);
  EXPECT_EQ(manager.op_sources(), baseline_sources);
  expect_device_ownership_inspection_equal(
      baseline_tiled, testing::inspect_op_registry_device_ownership_for_testing(
                          registry, tiled_key));

  auto tiled_values = registry.get_all_implementations(kType, kTiledSubtype);
  ASSERT_EQ(tiled_values.size(), 1u);
  EXPECT_TRUE(tiled_values.front().is_tiled());
  EXPECT_EQ(tiled_values.front().metadata.device_preference, Device::GPU_CUDA);
  EXPECT_EQ(tiled_values.front().metadata.cost_score, 202);
  Node tiled_node;
  std::get<TileOpFunc>(tiled_values.front().func)(tiled_node, OutputTile{}, {});
  EXPECT_EQ(tiled_invocations.load(std::memory_order_acquire), 1);
  EXPECT_FALSE(registry.resolve_for_intent(kType, kTiledSubtype,
                                           ComputeIntent::GlobalHighPrecision));

  monolithic_values.clear();
  tiled_values.clear();
  const bool monolithic_removed = registry.unregister_key(monolithic_key);
  if (monolithic_removed) {
    monolithic_callback_cleanup.disarm();
  }
  EXPECT_TRUE(monolithic_removed);
  const bool tiled_removed = registry.unregister_key(tiled_key);
  if (tiled_removed) {
    tiled_callback_cleanup.disarm();
  }
  EXPECT_TRUE(tiled_removed);
}

/**
 * @brief Exercises every device-registration failpoint against a fresh key.
 *
 * @throws Nothing when no failed candidate publishes a value, ownership row,
 *         key, source, or count and the identical key remains reusable.
 * @note The 2x2 matrix covers monolithic and tiled candidates at both stable
 *       owner and CPU compatibility-bridge construction. Every retry invokes
 *       both the device snapshot and its HP forwarding bridge before cleanup.
 */
TEST_F(PluginManagerLifecycleTest,
       DeviceRegistrationFailpointsDoNotPublishFreshKeysAndAllowRetry) {
  using Failpoint = testing::OpRegistryDeviceRegistrationFailpoint;
  auto& registry = OpRegistry::instance();
  auto& manager = PluginManager::process_instance();
  constexpr const char* kType = "fresh_device_registration_failure";

  const auto expect_failed_key_absent =
      [&](const std::string& key,
          const std::vector<std::string>& baseline_raw_keys,
          const std::vector<std::string>& baseline_combined_keys,
          const std::map<std::string, std::string>& baseline_sources,
          std::size_t baseline_loaded_plugins) {
        const auto inspection =
            testing::inspect_op_registry_device_ownership_for_testing(registry,
                                                                      key);
        EXPECT_FALSE(inspection.implementation_entry_present);
        EXPECT_FALSE(inspection.ownership_entry_present);
        EXPECT_EQ(inspection.implementation_count, 0u);
        EXPECT_EQ(inspection.revision_count, 0u);
        EXPECT_EQ(inspection.first_device_revision, 0u);
        EXPECT_EQ(inspection.monolithic_hp_revision, 0u);
        EXPECT_EQ(inspection.tiled_hp_revision, 0u);
        EXPECT_EQ(inspection.meta_hp_revision, 0u);
        EXPECT_EQ(registry.get_keys(), baseline_raw_keys);
        EXPECT_EQ(registry.get_keys().size(), baseline_raw_keys.size());
        EXPECT_EQ(registry.get_combined_keys(), baseline_combined_keys);
        EXPECT_EQ(registry.get_combined_keys().size(),
                  baseline_combined_keys.size());
        EXPECT_EQ(manager.op_sources(), baseline_sources);
        EXPECT_EQ(manager.op_sources().size(), baseline_sources.size());
        EXPECT_EQ(manager.loaded_plugin_count(), baseline_loaded_plugins);
      };

  const auto exercise_monolithic = [&](const std::string& subtype,
                                       Failpoint failpoint) {
    SCOPED_TRACE(subtype);
    const std::string key = make_key(kType, subtype);
    (void)registry.unregister_key(key);
    ScopedOpRegistryKeyCleanup registered_callback_cleanup(registry, kType,
                                                           subtype);
    const auto baseline_raw_keys = registry.get_keys();
    const auto baseline_combined_keys = registry.get_combined_keys();
    const auto baseline_sources = manager.op_sources();
    const std::size_t baseline_loaded_plugins = manager.loaded_plugin_count();
    ASSERT_EQ(
        std::count(baseline_raw_keys.begin(), baseline_raw_keys.end(), key), 0);
    ASSERT_EQ(baseline_sources.count(key), 0u);

    testing::set_op_registry_device_registration_failpoint(failpoint);
    EXPECT_THROW(registry.register_impl(
                     kType, subtype, Device::CPU,
                     [](const Node&, const std::vector<const NodeOutput*>&) {
                       NodeOutput output;
                       output.debug.compute_device =
                           "UNPUBLISHED_FRESH_MONOLITHIC";
                       return output;
                     }),
                 std::bad_alloc);
    EXPECT_EQ(testing::op_registry_device_registration_failpoint_hits(), 1u);
    testing::set_op_registry_device_registration_failpoint(Failpoint::None);
    expect_failed_key_absent(key, baseline_raw_keys, baseline_combined_keys,
                             baseline_sources, baseline_loaded_plugins);
    EXPECT_FALSE(registry.get_implementations(kType, subtype).has_value());

    registry.register_impl(
        kType, subtype, Device::CPU,
        [](const Node&, const std::vector<const NodeOutput*>&) {
          NodeOutput output;
          output.debug.compute_device = "RETRIED_FRESH_MONOLITHIC";
          return output;
        });
    const auto ownership =
        testing::inspect_op_registry_device_ownership_for_testing(registry,
                                                                  key);
    ASSERT_TRUE(ownership.implementation_entry_present);
    ASSERT_TRUE(ownership.ownership_entry_present);
    ASSERT_EQ(ownership.implementation_count, 1u);
    ASSERT_EQ(ownership.revision_count, 1u);
    ASSERT_NE(ownership.first_device_revision, 0u);
    EXPECT_EQ(ownership.monolithic_hp_revision,
              ownership.first_device_revision);
    EXPECT_EQ(ownership.meta_hp_revision, ownership.first_device_revision);

    auto device =
        registry.get_implementations_by_device(kType, subtype, Device::CPU);
    ASSERT_EQ(device.size(), 1u);
    ASSERT_TRUE(device.front().is_monolithic());
    auto bridge = registry.resolve_for_intent(
        kType, subtype, ComputeIntent::GlobalHighPrecision);
    ASSERT_TRUE(bridge.has_value());
    ASSERT_TRUE(std::holds_alternative<MonolithicOpFunc>(*bridge));
    Node node;
    node.type = kType;
    node.subtype = subtype;
    const auto device_output =
        std::get<MonolithicOpFunc>(device.front().func)(node, {});
    const auto bridge_output = std::get<MonolithicOpFunc>(*bridge)(node, {});
    EXPECT_EQ(device_output.debug.compute_device, "RETRIED_FRESH_MONOLITHIC");
    EXPECT_EQ(bridge_output.debug.compute_device,
              device_output.debug.compute_device);

    device.clear();
    bridge.reset();
    const bool key_removed = registry.unregister_key(key);
    if (key_removed) {
      registered_callback_cleanup.disarm();
    }
    EXPECT_TRUE(key_removed);
    expect_failed_key_absent(key, baseline_raw_keys, baseline_combined_keys,
                             baseline_sources, baseline_loaded_plugins);
  };

  const auto exercise_tiled = [&](const std::string& subtype,
                                  Failpoint failpoint) {
    SCOPED_TRACE(subtype);
    const std::string key = make_key(kType, subtype);
    (void)registry.unregister_key(key);
    std::atomic<int> invocations{0};
    ScopedOpRegistryKeyCleanup registered_callback_cleanup(registry, kType,
                                                           subtype);
    const auto baseline_raw_keys = registry.get_keys();
    const auto baseline_combined_keys = registry.get_combined_keys();
    const auto baseline_sources = manager.op_sources();
    const std::size_t baseline_loaded_plugins = manager.loaded_plugin_count();
    ASSERT_EQ(
        std::count(baseline_raw_keys.begin(), baseline_raw_keys.end(), key), 0);
    ASSERT_EQ(baseline_sources.count(key), 0u);

    testing::set_op_registry_device_registration_failpoint(failpoint);
    EXPECT_THROW(registry.register_impl(
                     kType, subtype, Device::CPU,
                     [](const Node&, const OutputTile&,
                        const std::vector<InputTile>&) {},
                     OpMetadata{}),
                 std::bad_alloc);
    EXPECT_EQ(testing::op_registry_device_registration_failpoint_hits(), 1u);
    testing::set_op_registry_device_registration_failpoint(Failpoint::None);
    expect_failed_key_absent(key, baseline_raw_keys, baseline_combined_keys,
                             baseline_sources, baseline_loaded_plugins);
    EXPECT_FALSE(registry.get_implementations(kType, subtype).has_value());

    registry.register_impl(
        kType, subtype, Device::CPU,
        [&invocations](const Node&, const OutputTile&,
                       const std::vector<InputTile>&) {
          invocations.fetch_add(1, std::memory_order_relaxed);
        },
        OpMetadata{});
    const auto ownership =
        testing::inspect_op_registry_device_ownership_for_testing(registry,
                                                                  key);
    ASSERT_TRUE(ownership.implementation_entry_present);
    ASSERT_TRUE(ownership.ownership_entry_present);
    ASSERT_EQ(ownership.implementation_count, 1u);
    ASSERT_EQ(ownership.revision_count, 1u);
    ASSERT_NE(ownership.first_device_revision, 0u);
    EXPECT_EQ(ownership.tiled_hp_revision, ownership.first_device_revision);
    EXPECT_EQ(ownership.meta_hp_revision, ownership.first_device_revision);

    auto device =
        registry.get_implementations_by_device(kType, subtype, Device::CPU);
    ASSERT_EQ(device.size(), 1u);
    ASSERT_TRUE(device.front().is_tiled());
    auto bridge = registry.resolve_for_intent(
        kType, subtype, ComputeIntent::GlobalHighPrecision);
    ASSERT_TRUE(bridge.has_value());
    ASSERT_TRUE(std::holds_alternative<TileOpFunc>(*bridge));
    Node node;
    node.type = kType;
    node.subtype = subtype;
    const OutputTile output;
    const std::vector<InputTile> inputs;
    std::get<TileOpFunc>(device.front().func)(node, output, inputs);
    std::get<TileOpFunc> (*bridge)(node, output, inputs);
    EXPECT_EQ(invocations.load(std::memory_order_acquire), 2);

    device.clear();
    bridge.reset();
    const bool key_removed = registry.unregister_key(key);
    if (key_removed) {
      registered_callback_cleanup.disarm();
    }
    EXPECT_TRUE(key_removed);
    expect_failed_key_absent(key, baseline_raw_keys, baseline_combined_keys,
                             baseline_sources, baseline_loaded_plugins);
  };

  exercise_monolithic("monolithic_stable_owner", Failpoint::StableOwner);
  exercise_monolithic("monolithic_cpu_bridge",
                      Failpoint::CpuCompatibilityBridge);
  exercise_tiled("tiled_stable_owner", Failpoint::StableOwner);
  exercise_tiled("tiled_cpu_bridge", Failpoint::CpuCompatibilityBridge);
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
  EXPECT_EQ(normalize_device_target_trace(read_lifecycle_trace(trace_path)),
            (std::vector<std::string>{
                "registrar_return", "device_tiled_target_destroy",
                "device_monolithic_target_destroy", "callback_destroy"}));

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
  EXPECT_EQ(normalize_device_target_trace(read_lifecycle_trace(trace_path)),
            (std::vector<std::string>{"registrar_return",
                                      "device_tiled_target_destroy",
                                      "device_monolithic_target_destroy",
                                      "callback_destroy", "library_unload"}));
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

TEST_F(PluginManagerLifecycleTest,
       InFlightPluginExceptionsBecomeHostOwnedBeforeConcurrentUnload) {
  const auto plugin_path = lifecycle_plugin_path();
  ASSERT_TRUE(std::filesystem::exists(plugin_path));
  /**
   * @brief Describes one plugin-origin exception normalization case.
   * @throws Nothing.
   * @note String pointers borrow static literals for the enclosing test. Each
   * case selects a fixture throw mode, its expected DSO destruction event, and
   * the optional host-owned GraphError code checked after concurrent unload.
   */
  struct ExceptionCase {
    /** @brief Fixture mode selecting the plugin exception type to throw. */
    const char* mode;
    /** @brief Trace event proving destruction of the plugin exception. */
    const char* destroy_event;
    /** @brief Expected GraphError code, or nullopt for std::bad_alloc. */
    std::optional<GraphErrc> expected_graph_code;
  };
  const ExceptionCase cases[] = {
      {"custom", "exception_destroy", GraphErrc::ComputeError},
      {"invalid_argument", "invalid_argument_exception_destroy",
       GraphErrc::InvalidParameter},
      {"bad_alloc", "bad_alloc_exception_destroy", std::nullopt},
  };

  for (const ExceptionCase& test_case : cases) {
    SCOPED_TRACE(test_case.mode);
    const auto trace_path =
        lifecycle_trace_path(std::string("exception-") + test_case.mode);
    const auto release_path = lifecycle_trace_path(
        std::string("exception-release-") + test_case.mode);
    std::filesystem::remove(trace_path);
    std::filesystem::remove(release_path);
    ScopedEnvironmentVariable trace_environment(kLifecycleTraceEnvironment,
                                                trace_path.string());
    ScopedEnvironmentVariable release_environment(
        kLifecycleCallbackReleaseEnvironment, release_path.string());
    ScopedEnvironmentVariable throw_environment(
        kLifecycleCallbackThrowEnvironment, test_case.mode);

    auto& manager = PluginManager::process_instance();
    const auto result =
        manager.load_from_dirs_report({plugin_path.parent_path().string()});
    ASSERT_EQ(result.loaded, 1) << describe_errors(result.errors);
    auto resolved = OpRegistry::instance().resolve_for_intent(
        kLifecycleType, kLifecycleSubtype, ComputeIntent::GlobalHighPrecision);
    ASSERT_TRUE(resolved.has_value());
    ASSERT_TRUE(std::holds_alternative<MonolithicOpFunc>(*resolved));
    MonolithicOpFunc callback =
        std::move(std::get<MonolithicOpFunc>(*resolved));
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
    if (!wait_for_lifecycle_event(trace_path, "callback_enter",
                                  std::chrono::seconds(2))) {
      std::ofstream(release_path).put('\n');
      invocation.wait();
      manager.unload_all_plugins();
      ADD_FAILURE() << "exception callback did not reach release barrier";
      continue;
    }
    EXPECT_EQ(manager.unload_all_plugins(), 1);
    std::ofstream(release_path).put('\n');
    ASSERT_EQ(invocation.wait_for(std::chrono::seconds(2)),
              std::future_status::ready);

    if (test_case.expected_graph_code) {
      try {
        (void)invocation.get();
        ADD_FAILURE() << "plugin exception escaped normalization";
      } catch (const GraphError& error) {
        EXPECT_EQ(error.code(), *test_case.expected_graph_code);
        EXPECT_NE(std::string(error.what()).find("lifecycle plugin"),
                  std::string::npos);
      }
    } else {
      try {
        (void)invocation.get();
        ADD_FAILURE() << "plugin bad_alloc did not preserve its category";
      } catch (const std::bad_alloc& error) {
        EXPECT_EQ(typeid(error), typeid(std::bad_alloc));
      }
    }

    const auto trace = read_lifecycle_trace(trace_path);
    const auto exception_destroy =
        std::find(trace.begin(), trace.end(), test_case.destroy_event);
    const auto callback_destroy =
        std::find(trace.begin(), trace.end(), "callback_destroy");
    const auto library_unload =
        std::find(trace.begin(), trace.end(), "library_unload");
    ASSERT_NE(exception_destroy, trace.end());
    ASSERT_NE(callback_destroy, trace.end());
    ASSERT_NE(library_unload, trace.end());
    EXPECT_LT(exception_destroy, callback_destroy);
    EXPECT_LT(callback_destroy, library_unload);
    std::filesystem::remove(trace_path);
    std::filesystem::remove(release_path);
  }
}

/**
 * @brief Proves loaded callbacks fence only the actual DSO invocation frame.
 *
 * @throws Nothing when host pre-entry YAML conversion and post-return output
 * validation preserve their original host-owned exception types.
 * @note The release file already exists, so any accidental callback entry is
 * visible in the lifecycle trace without blocking the test.
 */
TEST_F(PluginManagerLifecycleTest,
       LoadedCallbackPreservesHostAdapterExceptionTypesOutsideDsoFence) {
  const auto plugin_path = lifecycle_plugin_path();
  ASSERT_TRUE(std::filesystem::exists(plugin_path));
  const auto trace_path = lifecycle_trace_path("host-adapter-exceptions");
  const auto release_path =
      lifecycle_trace_path("host-adapter-exceptions-release");
  std::filesystem::remove(trace_path);
  std::ofstream(release_path).put('\n');
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

  Node node;
  node.id = 1;
  node.type = kLifecycleType;
  node.subtype = kLifecycleSubtype;
  node.parameters = YAML::Load("{bad: !unsupported value}");
  EXPECT_THROW((void)callback(node, {}), YAML::Exception);
  EXPECT_EQ(read_lifecycle_trace(trace_path),
            (std::vector<std::string>{"registrar_return"}));

  node.parameters = YAML::Node(YAML::NodeType::Map);
  {
    ScopedEnvironmentVariable invalid_result_environment(
        kLifecycleInvalidResultEnvironment, "1");
    EXPECT_THROW((void)callback(node, {}), std::invalid_argument);
  }
  EXPECT_EQ(read_lifecycle_trace(trace_path),
            (std::vector<std::string>{"registrar_return", "callback_enter",
                                      "callback_return"}));

  callback = MonolithicOpFunc{};
  EXPECT_EQ(manager.unload_all_plugins(), 1);
  EXPECT_EQ(read_lifecycle_trace(trace_path),
            (std::vector<std::string>{"registrar_return", "callback_enter",
                                      "callback_return", "callback_destroy",
                                      "library_unload"}));
  std::filesystem::remove(trace_path);
  std::filesystem::remove(release_path);
}

/**
 * @brief Proves malformed ROI return values are rejected after real DSO
 * callback return.
 *
 * @throws Nothing when host-side validation preserves std::invalid_argument,
 * graph state, cache state, and loaded-plugin lifetime ordering.
 * @note Negative origins remain legal; this test selects a negative dimension
 * and a signed-int endpoint overflow, which are the two invalid categories.
 */
TEST_F(PluginManagerLifecycleTest,
       LoadedRoiCallbacksRejectInvalidReturnedGeometryOutsideDsoFence) {
  const auto plugin_path = lifecycle_plugin_path();
  ASSERT_TRUE(std::filesystem::exists(plugin_path));
  const auto trace_path = lifecycle_trace_path("host-roi-validation");
  std::filesystem::remove(trace_path);
  ScopedEnvironmentVariable trace_environment(kLifecycleTraceEnvironment,
                                              trace_path.string());

  auto& manager = PluginManager::process_instance();
  const auto result =
      manager.load_from_dirs_report({plugin_path.parent_path().string()});
  ASSERT_EQ(result.loaded, 1) << describe_errors(result.errors);

  auto& registry = OpRegistry::instance();
  DirtyRoiPropFunc dirty =
      registry.get_dirty_propagator(kLifecycleType, kLifecycleSubtype);
  ForwardRoiPropFunc forward =
      registry.get_forward_propagator(kLifecycleType, kLifecycleSubtype);

  GraphModel graph("cache/plugin-roi-validation");
  Node source;
  source.id = 1;
  source.name = "roi_source";
  source.type = "test";
  source.subtype = "source";
  source.parameters = YAML::Node(YAML::NodeType::Map);
  graph.add_node(source);
  Node child;
  child.id = 2;
  child.name = "roi_child";
  child.type = kLifecycleType;
  child.subtype = kLifecycleSubtype;
  child.parameters = YAML::Node(YAML::NodeType::Map);
  child.image_inputs = {ImageInput{1, "image"}};
  graph.add_node(child);
  graph.validate_topology();
  graph.mutate_node_runtime_state(2, [](auto& state) {
    NodeOutput sentinel;
    sentinel.debug.compute_device = "ROI_CACHE_SENTINEL";
    state.cached_output_high_precision = std::move(sentinel);
    state.hp_version = 17;
  });
  const std::uint64_t topology_generation = graph.topology_generation();
  const std::uint64_t dirty_generation = graph.dirty_generation_counter;
  const plugin::ParameterMap parameters;

  {
    ScopedEnvironmentVariable invalid_roi_environment(
        kLifecycleInvalidRoiEnvironment, "negative");
    EXPECT_THROW(
        (void)dirty(graph.node(2), cv::Rect(1, 2, 3, 4), graph, cv::Size(8, 8),
                    {cv::Size(8, 8)}, parameters, nullptr),
        std::invalid_argument);
  }
  {
    ScopedEnvironmentVariable invalid_roi_environment(
        kLifecycleInvalidRoiEnvironment, "overflow");
    EXPECT_THROW((void)forward(graph.node(2), cv::Rect(1, 2, 3, 4), graph,
                               cv::Size(8, 8), cv::Size(8, 8), 0,
                               {cv::Size(8, 8)}, parameters),
                 std::invalid_argument);
  }

  ASSERT_TRUE(graph.node(2).cached_output_high_precision.has_value());
  EXPECT_EQ(graph.node(2).cached_output_high_precision->debug.compute_device,
            "ROI_CACHE_SENTINEL");
  EXPECT_EQ(graph.node(2).hp_version, 17U);
  EXPECT_EQ(graph.topology_generation(), topology_generation);
  EXPECT_EQ(graph.dirty_generation_counter, dirty_generation);
  EXPECT_EQ(read_lifecycle_trace(trace_path),
            (std::vector<std::string>{"registrar_return", "dirty_roi_return",
                                      "forward_roi_return"}));

  EXPECT_EQ(manager.unload_all_plugins(), 1);
  dirty = DirtyRoiPropFunc{};
  forward = ForwardRoiPropFunc{};
  std::filesystem::remove(trace_path);
}

/**
 * @brief Proves reserved key separators abort a real DSO registration
 * transaction without replacing prior host state.
 *
 * @return Nothing.
 * @throws Nothing when both type and subtype rejection cases preserve registry
 * keys, source ownership, retained-handle count, and the predecessor callback.
 * @note The fixture stages its ordinary callbacks before attempting the invalid
 * name, so absence of publication exercises complete transaction rollback.
 * Built-ins are seeded before the baseline snapshot so the test remains
 * hermetic when CTest runs this case in its own process.
 */
TEST_F(PluginManagerLifecycleTest,
       InvalidDsoNameSegmentsRollBackWithoutIdentityCollision) {
  const auto plugin_path = lifecycle_plugin_path();
  ASSERT_TRUE(std::filesystem::exists(plugin_path));
  auto& manager = PluginManager::process_instance();
  auto& registry = OpRegistry::instance();
  manager.seed_builtins_from_registry();

  for (const std::string mode : {"type", "subtype"}) {
    SCOPED_TRACE(mode);
    registry.unregister_key(kLifecycleKey);
    register_host_lifecycle_sentinel();
    const auto baseline_keys = registry.get_combined_keys();
    const auto baseline_sources = manager.op_sources();
    const std::size_t baseline_handles = manager.loaded_plugin_count();
    const auto trace_path = lifecycle_trace_path("invalid-name-" + mode);
    std::filesystem::remove(trace_path);
    ScopedEnvironmentVariable trace_environment(kLifecycleTraceEnvironment,
                                                trace_path.string());
    ScopedEnvironmentVariable invalid_name_environment(
        kLifecycleInvalidNameEnvironment, mode);

    const auto result =
        manager.load_from_dirs_report({plugin_path.parent_path().string()});
    EXPECT_EQ(result.loaded, 0);
    ASSERT_EQ(result.errors.size(), 1u);
    EXPECT_EQ(result.errors.front().code, GraphErrc::InvalidParameter);
    EXPECT_EQ(registry.get_combined_keys(), baseline_keys);
    EXPECT_EQ(manager.op_sources(), baseline_sources);
    EXPECT_EQ(manager.loaded_plugin_count(), baseline_handles);
    EXPECT_EQ(current_lifecycle_compute_device(), "HOST_LIFECYCLE_SENTINEL");
    EXPECT_EQ(
        read_lifecycle_trace(trace_path),
        (std::vector<std::string>{mode == "type" ? "registrar_invalid_type"
                                                 : "registrar_invalid_subtype",
                                  "callback_destroy", "library_unload"}));

    registry.unregister_key(kLifecycleKey);
    std::filesystem::remove(trace_path);
  }
}

/**
 * @brief Proves a raw DSO registrar call cannot stage an empty operation.
 *
 * @return Nothing.
 * @throws Nothing when host validation reports InvalidParameter, preserves all
 * registry/source/handle state, and retires already staged callable state
 * before the rejected candidate library unloads.
 * @note The fixture bypasses the typed SDK helper deliberately, so this test
 * exercises the host's independent raw-boundary validation and transaction
 * rollback rather than the public helper precondition. Built-ins are seeded
 * before the baseline snapshot so isolated and whole-executable runs observe
 * the same process-owner state.
 */
TEST_F(PluginManagerLifecycleTest,
       EmptyRawDsoCallbackRollsBackBeforeCandidateUnload) {
  const auto plugin_path = lifecycle_plugin_path();
  ASSERT_TRUE(std::filesystem::exists(plugin_path));
  auto& manager = PluginManager::process_instance();
  auto& registry = OpRegistry::instance();
  manager.seed_builtins_from_registry();

  registry.unregister_key(kLifecycleKey);
  register_host_lifecycle_sentinel();
  const auto baseline_keys = registry.get_combined_keys();
  const auto baseline_sources = manager.op_sources();
  const std::size_t baseline_handles = manager.loaded_plugin_count();
  const auto trace_path = lifecycle_trace_path("empty-raw-callback");
  std::filesystem::remove(trace_path);
  ScopedEnvironmentVariable trace_environment(kLifecycleTraceEnvironment,
                                              trace_path.string());
  ScopedEnvironmentVariable empty_callback_environment(
      kLifecycleEmptyCallbackEnvironment, "1");

  const auto result =
      manager.load_from_dirs_report({plugin_path.parent_path().string()});
  EXPECT_EQ(result.loaded, 0);
  ASSERT_EQ(result.errors.size(), 1u);
  EXPECT_EQ(result.errors.front().code, GraphErrc::InvalidParameter);
  EXPECT_EQ(registry.get_combined_keys(), baseline_keys);
  EXPECT_EQ(manager.op_sources(), baseline_sources);
  EXPECT_EQ(manager.loaded_plugin_count(), baseline_handles);
  EXPECT_EQ(current_lifecycle_compute_device(), "HOST_LIFECYCLE_SENTINEL");
  EXPECT_EQ(read_lifecycle_trace(trace_path),
            (std::vector<std::string>{"registrar_empty_callback",
                                      "callback_destroy", "library_unload"}));

  registry.unregister_key(kLifecycleKey);
  std::filesystem::remove(trace_path);
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
 * @brief Proves an ImageBuffer copy independently retains plugin deleter code.
 *
 * @throws Nothing when real plugin load, invocation, unload, and trace cleanup
 * complete successfully.
 * @note The enclosing NodeOutput and registry callback lease are retired first;
 * only the copied image context remains. Its final reset must run the
 * plugin-defined payload destructor before the real library-unload event.
 */
TEST_F(PluginManagerLifecycleTest,
       ImageBufferCopyRetainsPluginPayloadLeaseAfterOutputRetires) {
  const auto plugin_path = lifecycle_plugin_path();
  ASSERT_TRUE(std::filesystem::exists(plugin_path));
  const auto trace_path = lifecycle_trace_path("image-buffer-copy-lease");
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
  ImageBuffer retained_buffer = plugin_output.image_buffer;

  EXPECT_EQ(manager.unload_all_plugins(), 1);
  plugin_output = NodeOutput{};
  EXPECT_EQ(read_lifecycle_trace(trace_path),
            (std::vector<std::string>{"registrar_return", "callback_destroy"}));

  retained_buffer = ImageBuffer{};
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

  Node save_node;
  save_node.id = 701;
  save_node.name = "save_failure_contract";
  save_node.type = "io";
  save_node.subtype = "save";
  const std::filesystem::path rejected_path =
      std::filesystem::temp_directory_path() /
      "photospider-save-rejection.unsupported_extension";
  save_node.parameters["path"] = rejected_path.string();
  NodeOutput save_input;
  save_input.image_buffer =
      make_aligned_cpu_image_buffer(1, 1, 1, DataType::FLOAT32);
  const std::vector<const NodeOutput*> save_inputs{&save_input};
  const auto save_operation = OpRegistry::instance().resolve_for_intent(
      "io", "save", ComputeIntent::GlobalHighPrecision);
  ASSERT_TRUE(save_operation.has_value());
  ASSERT_TRUE(std::holds_alternative<MonolithicOpFunc>(*save_operation));
  try {
    (void)std::get<MonolithicOpFunc> (*save_operation)(save_node, save_inputs);
    ADD_FAILURE() << "save accepted an output path without an image writer";
  } catch (const GraphError& error) {
    EXPECT_EQ(error.code(), GraphErrc::Io);
  }
  std::filesystem::remove(rejected_path);

  EXPECT_GE(manager.unload_all_plugins(), 3);
  EXPECT_EQ(OpRegistry::instance().dirty_propagation_contract_status(
                "image_process", "invert"),
            PropagationContractStatus::LegacyIdentityFallback);
}

TEST_F(PluginManagerLifecycleTest,
       DependencyCacheTracksPluginOverrideAndRestoredBuilderRevision) {
  const auto original_path = lifecycle_plugin_path();
  const auto replacement_path = override_lifecycle_plugin_path();
  ASSERT_TRUE(std::filesystem::exists(original_path));
  ASSERT_TRUE(std::filesystem::exists(replacement_path));

  auto& manager = PluginManager::process_instance();
  ASSERT_EQ(
      manager.load_from_dirs_report({original_path.parent_path().string()})
          .loaded,
      1);

  GraphModel graph;
  Node source;
  source.id = 1;
  source.name = "dependency_source";
  source.type = "image_generator";
  source.subtype = "constant";
  source.parameters = YAML::Load("{width: 8, height: 8, value: 0}");
  graph.add_node(source);
  Node child;
  child.id = 2;
  child.name = "plugin_dependency";
  child.type = "plugin_lifecycle";
  child.subtype = "op";
  child.parameters = YAML::Load("{width: 8, height: 8}");
  child.image_inputs.push_back(ImageInput{1, "image"});
  graph.add_node(child);
  graph.validate_topology();
  graph.mutate_node_runtime_state(1, [](auto& state) {
    state.cached_output_high_precision = NodeOutput{};
    state.cached_output_high_precision->image_buffer =
        make_aligned_cpu_image_buffer(8, 8, 1, DataType::FLOAT32);
    state.hp_version = 1;
  });

  RoiPropagationService propagation;
  const auto build_and_read_marker = [&]() {
    (void)propagation.project_roi_backward(graph, 2, cv::Rect(0, 0, 1, 1), 1);
    const Node& cached_node = graph.node(2);
    EXPECT_TRUE(cached_node.dependency_lut_cache.has_value());
    return cached_node.dependency_lut_cache->lut.cell_to_upstream_roi.front().x;
  };
  EXPECT_EQ(build_and_read_marker(), 1);
  const std::uint64_t original_revision =
      graph.node(2).dependency_lut_cache->identity.dependency_builder_revision;
  EXPECT_EQ(graph.node(2).dependency_lut_version, 1u);

  ASSERT_EQ(
      manager.load_from_dirs_report({replacement_path.parent_path().string()})
          .loaded,
      1);
  EXPECT_EQ(build_and_read_marker(), 2);
  const std::uint64_t replacement_revision =
      graph.node(2).dependency_lut_cache->identity.dependency_builder_revision;
  EXPECT_NE(replacement_revision, original_revision);
  EXPECT_EQ(graph.node(2).dependency_lut_version, 2u);

  EXPECT_EQ(manager.unload_by_plugin_path(
                std::filesystem::absolute(replacement_path).string()),
            1);
  EXPECT_EQ(build_and_read_marker(), 1);
  EXPECT_EQ(
      graph.node(2).dependency_lut_cache->identity.dependency_builder_revision,
      original_revision);
  EXPECT_EQ(graph.node(2).dependency_lut_version, 3u);
  EXPECT_EQ(manager.unload_all_plugins(), 1);
}

TEST_F(PluginManagerLifecycleTest,
       PublicDependencyFlagControlsImageVersionCacheIdentity) {
  const auto plugin_path = lifecycle_plugin_path();
  ASSERT_TRUE(std::filesystem::exists(plugin_path));
  auto& manager = PluginManager::process_instance();

  GraphModel graph;
  Node source;
  source.id = 1;
  source.name = "dependency_source";
  source.type = "image_generator";
  source.subtype = "constant";
  source.parameters = YAML::Load("{width: 8, height: 8, value: 0}");
  graph.add_node(source);
  Node child;
  child.id = 2;
  child.name = "plugin_dependency";
  child.type = kLifecycleType;
  child.subtype = kLifecycleSubtype;
  child.parameters = YAML::Load("{width: 8, height: 8}");
  child.image_inputs.push_back(ImageInput{1, "image"});
  graph.add_node(child);
  graph.validate_topology();
  graph.mutate_node_runtime_state(1, [](auto& state) {
    state.cached_output_high_precision = NodeOutput{};
    state.cached_output_high_precision->image_buffer =
        make_aligned_cpu_image_buffer(8, 8, 1, DataType::FLOAT32);
    state.hp_version = 1;
  });

  RoiPropagationService propagation;
  const auto project = [&]() {
    return propagation.project_roi_backward(graph, 2, cv::Rect(0, 0, 1, 1), 1);
  };

  ASSERT_EQ(manager.load_from_dirs_report({plugin_path.parent_path().string()})
                .loaded,
            1);
  ASSERT_TRUE(project().has_value());
  EXPECT_FALSE(graph.node(2).dependency_lut_cache->identity.data_dependent);
  EXPECT_EQ(graph.node(2).dependency_lut_version, 1u);
  graph.mutate_node_runtime_state(1, [](auto& state) { state.hp_version = 2; });
  ASSERT_TRUE(project().has_value());
  EXPECT_EQ(graph.node(2).dependency_lut_version, 1u)
      << "static dependency builders ignore pixel-only version changes";
  EXPECT_EQ(manager.unload_all_plugins(), 1);

  ScopedEnvironmentVariable data_dependent_environment(
      kLifecycleDataDependentEnvironment, "1");
  ASSERT_EQ(manager.load_from_dirs_report({plugin_path.parent_path().string()})
                .loaded,
            1);
  ASSERT_TRUE(project().has_value());
  EXPECT_TRUE(graph.node(2).dependency_lut_cache->identity.data_dependent);
  EXPECT_EQ(graph.node(2).dependency_lut_version, 2u);
  graph.mutate_node_runtime_state(1, [](auto& state) { state.hp_version = 3; });
  ASSERT_TRUE(project().has_value());
  EXPECT_EQ(graph.node(2).dependency_lut_version, 3u)
      << "data-dependent builders include upstream image versions";
  EXPECT_EQ(manager.unload_all_plugins(), 1);
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
