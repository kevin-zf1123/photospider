// Photospider test: Scheduler Plugin Loader
// 测试调度器插件的动态加载功能

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <memory>
#include <new>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "photospider/core/graph_error.hpp"
#include "photospider/scheduler/scheduler_plugin_api.hpp"
#include "photospider/scheduler/scheduler_task_runtime.hpp"
#include "runtime/graph_runtime.hpp"
#include "scheduler/scheduler_factory.hpp"
#include "scheduler/scheduler_plugin_loader.hpp"
#include "scheduler/scheduler_worker_budget.hpp"

#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace scheduler_allocation_probe {

/** @brief Disabled allocation countdown sentinel. */
constexpr std::int64_t kDisabled = -1;
/** @brief Per-thread zero-based allocation countdown. */
thread_local std::int64_t countdown = kDisabled;
/** @brief Whether the armed one-shot allocation failure fired. */
thread_local bool fired = false;
/** @brief Whether current-thread allocations should be counted. */
thread_local bool counting = false;
/** @brief Number of allocations observed since counting began. */
thread_local std::size_t allocation_count = 0U;

/**
 * @brief Arms one deterministic allocation failure on the current thread.
 * @param allocation_index Zero-based allocation to reject.
 * @return Nothing.
 * @throws Nothing.
 */
void arm(std::int64_t allocation_index) noexcept {
  countdown = allocation_index;
  fired = false;
}

/**
 * @brief Disarms the current-thread allocation failure.
 * @return Nothing.
 * @throws Nothing.
 */
void disarm() noexcept {
  countdown = kDisabled;
}

/**
 * @brief Reports whether the most recently armed failure fired.
 * @return True after `maybe_fail()` rejected the selected allocation.
 * @throws Nothing.
 */
bool did_fire() noexcept {
  return fired;
}

/**
 * @brief Applies the current allocation failure decision.
 * @return Nothing when allocation may continue.
 * @throws std::bad_alloc at the selected allocation index.
 */
void maybe_fail() {
  if (counting) {
    ++allocation_count;
  }
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

/**
 * @brief Begins allocation counting without arming a failure.
 * @return Nothing.
 * @throws Nothing.
 */
void begin_counting() noexcept {
  allocation_count = 0U;
  counting = true;
}

/**
 * @brief Stops allocation counting and returns the observed total.
 * @return Allocations performed on the current thread while counting.
 * @throws Nothing.
 */
std::size_t end_counting() noexcept {
  counting = false;
  return allocation_count;
}

}  // namespace scheduler_allocation_probe

/**
 * @brief Test-executable allocation operator for scheduler failure probes.
 * @param size Requested allocation size.
 * @return malloc-backed storage.
 * @throws std::bad_alloc when injected or malloc fails.
 */
void* operator new(std::size_t size) {
  scheduler_allocation_probe::maybe_fail();
  if (void* memory = std::malloc(size == 0 ? 1 : size)) {
    return memory;
  }
  throw std::bad_alloc{};
}

/** @copydoc operator new(std::size_t) */
void* operator new[](std::size_t size) {
  return ::operator new(size);
}

/** @brief Releases scalar storage allocated by the test operator. */
void operator delete(void* memory) noexcept {
  std::free(memory);
}
/** @brief Releases array storage allocated by the test operator. */
void operator delete[](void* memory) noexcept {
  std::free(memory);
}
/** @brief Releases sized scalar storage allocated by the test operator. */
void operator delete(void* memory, std::size_t) noexcept {
  std::free(memory);
}
/** @brief Releases sized array storage allocated by the test operator. */
void operator delete[](void* memory, std::size_t) noexcept {
  std::free(memory);
}

namespace ps {
namespace {

/** @brief Short destroy-count fixture scheduler type. */
constexpr const char* kDestroyCountSchedulerType = "destroy_count_test";
constexpr const char* kLongDestroyCountSchedulerType =                 // NOLINT
    "destroy_count_scheduler_type_long_enough_to_force_owned_string_"  // NOLINT
    "storage";                                                         // NOLINT
/** @brief Fixture environment key selecting lifecycle trace output. */
constexpr const char* kDestroyCountTraceEnvironment =
    "PS_DESTROY_COUNT_SCHEDULER_TRACE";  // NOLINT
/** @brief Fixture environment key selecting injected lifecycle failures. */
constexpr const char* kDestroyCountFailureEnvironment =
    "PS_DESTROY_COUNT_SCHEDULER_FAILURE";  // NOLINT
/** @brief Fixture environment key enabling host calls during detach. */
constexpr const char* kDestroyCountDetachContextProbeEnvironment =
    "PS_DESTROY_COUNT_SCHEDULER_DETACH_CONTEXT_PROBE";  // NOLINT
/** @brief Fixture environment key enabling one duplicate discovery entry. */
constexpr const char* kDestroyCountDuplicateTypeEnvironment =
    "PS_DESTROY_COUNT_SCHEDULER_DUPLICATE_TYPE";  // NOLINT
/** @brief Fixture environment key selecting a discovery rejection mode. */
constexpr const char* kDestroyCountLoadFailureEnvironment =
    "PS_DESTROY_COUNT_SCHEDULER_LOAD_FAILURE";  // NOLINT

/**
 * @brief Mirrors the destroy-count plugin's fixture-only forwarding counters.
 * @note Values form a private test protocol consumed only through
 * `ps_test_scheduler_forwarding_count` while the fixture library is loaded.
 */
enum class SchedulerForwardingProbe : int {
  /** @brief Plugin runtime device-inventory calls. */
  AvailableDevices = 0,
  /** @brief Plugin runtime initial handle-batch calls. */
  InitialHandles,
  /** @brief Plugin runtime worker handle-batch calls. */
  WorkerHandles,
  /** @brief Plugin runtime any-thread callback calls. */
  AnyThreadTask,
};

/**
 * @brief Mirrors fixture-only scheduler discovery callback counters.
 * @note Values form a private protocol consumed through
 * `ps_test_scheduler_load_probe_count` while the fixture remains loaded.
 */
enum class SchedulerLoadProbe : int {
  /** @brief Calls to the mandatory ABI handshake export. */
  GetAbiVersion = 0,
  /** @brief Calls to the type-count export. */
  GetCount,
  /** @brief Calls to the indexed type-name export. */
  GetName,
  /** @brief Calls to the mandatory indexed description export. */
  GetDescription,
  /** @brief Calls to the mandatory implementation-version export. */
  GetVersion,
};

/**
 * @brief Fixture function signature that reads one discovery probe counter.
 * @note The ABI uses the enum's integer representation across the DSO boundary
 *       and its exported function pointer never throws into the test
 *       executable.
 */
using LoadProbeReader = int(int) noexcept;  // NOLINT(readability/casting)

/**
 * @brief Captures every caller-visible scheduler plugin registry container.
 * @note Exact equality before and after an injected load failure detects
 * phantom type mappings, partial metadata, leaked handles, and diagnostic
 * prefix changes.
 */
struct SchedulerPluginRegistrySnapshot {
  /** @brief Sorted built-in and plugin scheduler type names. */
  std::vector<std::string> registered_types;
  /** @brief Built-in and plugin metadata in loader iteration order. */
  std::vector<SchedulerPluginInfo> type_info;
  /** @brief Formatted retained-plugin handle records. */
  std::vector<std::string> loaded_plugins;
  /** @brief Recoverable load diagnostics in insertion order. */
  std::vector<std::string> load_errors;
};

/**
 * @brief Counts real TaskHandle callbacks executed by the plugin fixture.
 * @note The synchronous fixture invokes this executor on the calling test
 * thread, so plain integer fields are sufficient.
 */
class CountingTaskExecutor final : public TaskExecutor {
 public:
  /**
   * @brief Records one executed dense task id.
   * @param task_id Dense id forwarded by `TaskHandle::run()`.
   * @return Nothing.
   * @throws Nothing.
   * @note Count and sum together detect missing, duplicate, or unexpected test
   * callbacks without allocating trace storage.
   */
  void run_task(int task_id) override {
    ++callback_count_;
    task_id_sum_ += task_id;
  }

  /**
   * @brief Returns the number of callbacks observed so far.
   * @return Synchronous callback count.
   * @throws Nothing.
   */
  int callback_count() const noexcept { return callback_count_; }

  /**
   * @brief Returns the sum of executed dense task ids.
   * @return Accumulated task-id sum.
   * @throws Nothing.
   */
  int task_id_sum() const noexcept { return task_id_sum_; }

 private:
  /** @brief Number of real `run_task` calls. */
  int callback_count_ = 0;
  /** @brief Sum of real task ids used to detect duplicates or omissions. */
  int task_id_sum_ = 0;
};

/**
 * @brief Host-defined task failure used to verify exact exception relay.
 *
 * This type exists only in the test executable and cannot be constructed by
 * the scheduler DSO. Observing the same marker after plugin-side
 * `exception_ptr` storage proves that the host owner unwraps task failures
 * instead of applying plugin-origin normalization.
 *
 * @throws std::bad_alloc if construction of the diagnostic message fails.
 */
class HostTaskMarkerError final : public std::runtime_error {
 public:
  /**
   * @brief Creates one host-only task exception with a stable marker.
   * @param marker Payload used to prove the exact host exception survives.
   * @throws std::bad_alloc if base diagnostic storage cannot allocate.
   */
  explicit HostTaskMarkerError(int marker)
      : std::runtime_error("host task marker"), marker(marker) {}

  /**
   * @brief Releases the host-only diagnostic object.
   * @throws Nothing.
   */
  ~HostTaskMarkerError() noexcept override = default;

  /** @brief Stable payload proving the original exception object was rethrown.
   */
  int marker = 0;
};

/**
 * @brief Rethrows one prebuilt host exception through a plugin task handle.
 * @throws The exact retained host exception from `run_task`.
 * @note Optional failpoint arming occurs immediately before rethrow, proving
 * the host relay and exception identity lookup allocate no carrier state.
 */
class ThrowingTaskExecutor final : public TaskExecutor {
 public:
  /**
   * @brief Retains one host exception identity for deterministic rethrow.
   * @param error Non-null host exception pointer created before submission.
   * @param arm_allocation_failure Whether to reject the next allocation at the
   * throw boundary.
   * @throws Nothing.
   */
  ThrowingTaskExecutor(std::exception_ptr error,
                       bool arm_allocation_failure) noexcept
      : error_(std::move(error)),
        arm_allocation_failure_(arm_allocation_failure) {}

  /**
   * @brief Arms the optional allocation probe and rethrows the retained error.
   * @param task_id Ignored dense task id.
   * @return This method does not return.
   * @throws The exact exception retained at construction.
   */
  void run_task(int task_id) override {
    (void)task_id;
    if (arm_allocation_failure_) {
      scheduler_allocation_probe::arm(0);
    }
    std::rethrow_exception(error_);
  }

 private:
  /** @brief Exact host exception rethrown on every invocation. */
  std::exception_ptr error_;
  /** @brief Enables allocation-free relay verification. */
  bool arm_allocation_failure_ = false;
};

/**
 * @brief Reenters scheduler exception publication from task-error destruction.
 * @throws Nothing from destruction; nested publication failures are suppressed.
 * @note The test drops every other owner before registry clear, so this
 * destructor runs exactly where an in-lock `exception_ptr` release would
 * deadlock against the same host exception registry.
 */
class ReentrantHostTaskError final {
 public:
  /**
   * @brief Binds the live scheduler and one prebuilt nested exception.
   * @param scheduler Scheduler owner that outlives this exception.
   * @param reentered Flag published after nested `set_exception` returns.
   * @param nested_error Host exception passed during destructor reentry.
   * @throws Nothing.
   */
  ReentrantHostTaskError(IScheduler* scheduler, bool* reentered,
                         std::exception_ptr nested_error) noexcept
      : scheduler_(scheduler),
        reentered_(reentered),
        nested_error_(std::move(nested_error)) {}

  /**
   * @brief Transfers the single destructor-reentry responsibility.
   * @param other Exception value whose armed state moves here.
   * @throws Nothing.
   */
  ReentrantHostTaskError(ReentrantHostTaskError&& other) noexcept
      : scheduler_(std::exchange(other.scheduler_, nullptr)),
        reentered_(std::exchange(other.reentered_, nullptr)),
        nested_error_(std::move(other.nested_error_)),
        armed_(std::exchange(other.armed_, false)) {}

  /**
   * @brief Prevents duplicate destructor-reentry responsibility.
   * @param other Exception value that cannot be copied.
   * @throws Nothing because the operation is deleted.
   */
  ReentrantHostTaskError(const ReentrantHostTaskError& other) = delete;

  /**
   * @brief Prevents assignment across exception-object identities.
   * @param other Exception value that cannot replace this object.
   * @return No value because the operation is deleted.
   * @throws Nothing because the operation is deleted.
   */
  ReentrantHostTaskError& operator=(ReentrantHostTaskError&& other) = delete;

  /**
   * @brief Prevents copy assignment across exception-object identities.
   * @param other Exception value that cannot replace this object.
   * @return No value because the operation is deleted.
   * @throws Nothing because the operation is deleted.
   */
  ReentrantHostTaskError& operator=(const ReentrantHostTaskError& other) =
      delete;

  /**
   * @brief Reenters the same exception registry without surfacing failures.
   * @throws Nothing.
   */
  ~ReentrantHostTaskError() noexcept {
    if (!armed_) {
      return;
    }
    try {
      scheduler_->set_exception(nested_error_);
      *reentered_ = true;
    } catch (...) {
    }
  }

 private:
  /** @brief Borrowed scheduler owner retained by the enclosing test. */
  IScheduler* scheduler_ = nullptr;
  /** @brief Borrowed result flag retained by the enclosing test. */
  bool* reentered_ = nullptr;
  /** @brief Prebuilt nested exception avoids allocation in the destructor. */
  std::exception_ptr nested_error_;
  /** @brief True only for the single exception object owning reentry. */
  bool armed_ = true;
};

/**
 * @brief Captures one noncopyable reentrant error in an `exception_ptr`.
 * @param scheduler Scheduler reentered when the exception is finally destroyed.
 * @param reentered Test flag published after successful nested registration.
 * @param nested_error Prebuilt exception used by the destructor callback.
 * @return Exact pointer to the caught host exception object.
 * @throws std::bad_alloc if exception object allocation fails.
 * @note The throw expression transfers the armed destructor responsibility
 * without invoking the callback while this helper captures the pointer.
 */
std::exception_ptr make_reentrant_host_task_error(
    IScheduler* scheduler, bool* reentered, std::exception_ptr nested_error) {
  try {
    throw ReentrantHostTaskError(scheduler, reentered, std::move(nested_error));
  } catch (...) {
    return std::current_exception();
  }
}

/**
 * @brief Provides allocation-free host services for owner lifecycle tests.
 * @throws Nothing.
 * @note The fixture scheduler may retain this borrowed object only until the
 *       test completes explicit or destructor-fallback detach.
 */
class NoopSchedulerHostContext final : public SchedulerHostContext {
 public:
  /** @copydoc SchedulerHostContext::is_device_available */
  bool is_device_available(Device device) const noexcept override {
    return device == Device::CPU;
  }

  /** @copydoc SchedulerHostContext::set_task_context */
  void set_task_context(int worker_id, std::uint64_t epoch) noexcept override {
    (void)worker_id;
    (void)epoch;
  }

  /** @copydoc SchedulerHostContext::clear_task_context */
  void clear_task_context() noexcept override {}

  /** @copydoc SchedulerHostContext::log_event */
  void log_event(SchedulerTraceAction action, int node_id, int worker_id,
                 std::uint64_t epoch) noexcept override {
    (void)action;
    (void)node_id;
    (void)worker_id;
    (void)epoch;
  }

  /**
   * @brief Releases the concrete no-op host after every detach completes.
   * @throws Nothing.
   * @note Destruction occurs only after the scheduler owner is gone.
   */
  ~NoopSchedulerHostContext() noexcept override = default;
};

/**
 * @brief Reads one named forwarding counter through the fixture C export.
 * @param read_count Resolved fixture counter function.
 * @param probe Counter slot to read.
 * @return Current fixture call count for `probe`.
 * @throws Nothing.
 */
int forwarding_probe_count(int (*read_count)(int),
                           SchedulerForwardingProbe probe) noexcept {
  return read_count(static_cast<int>(probe));
}

/**
 * @brief Reads one fixture scheduler-discovery callback counter.
 * @param read_count Resolved fixture counter export.
 * @param probe Discovery callback slot to read.
 * @return Current fixture call count for `probe`.
 * @throws Nothing.
 */
int load_probe_count(int (*read_count)(int),
                     SchedulerLoadProbe probe) noexcept {
  return read_count(static_cast<int>(probe));
}

/**
 * @brief Captures the loader's complete observable plugin registry state.
 * @param loader Loader queried while no allocation failpoint is armed.
 * @return Owned snapshot suitable for exact post-failure comparison.
 * @throws std::bad_alloc if result vectors or copied metadata cannot allocate.
 * @note Every loader query already returns an independently owned snapshot.
 */
SchedulerPluginRegistrySnapshot capture_plugin_registry(
    const SchedulerPluginLoader& loader) {
  SchedulerPluginRegistrySnapshot snapshot;
  snapshot.registered_types = loader.get_registered_types();
  snapshot.type_info = loader.get_all_info();
  snapshot.loaded_plugins = loader.list_loaded_plugins();
  snapshot.load_errors = loader.get_load_errors();
  return snapshot;
}

/**
 * @brief Compares every field in two scheduler plugin registry snapshots.
 * @param expected State captured before a transactional load attempt.
 * @param actual State observed after that attempt.
 * @return Nothing.
 * @throws std::bad_alloc if GoogleTest diagnostic formatting cannot allocate.
 * @note Metadata fields are compared individually because
 * `SchedulerPluginInfo` intentionally has no public equality operator.
 */
void expect_plugin_registry_equal(
    const SchedulerPluginRegistrySnapshot& expected,
    const SchedulerPluginRegistrySnapshot& actual) {
  EXPECT_EQ(actual.registered_types, expected.registered_types);
  EXPECT_EQ(actual.loaded_plugins, expected.loaded_plugins);
  EXPECT_EQ(actual.load_errors, expected.load_errors);
  ASSERT_EQ(actual.type_info.size(), expected.type_info.size());
  for (std::size_t index = 0; index < expected.type_info.size(); ++index) {
    EXPECT_EQ(actual.type_info[index].type_name,
              expected.type_info[index].type_name);
    EXPECT_EQ(actual.type_info[index].description,
              expected.type_info[index].description);
    EXPECT_EQ(actual.type_info[index].plugin_path,
              expected.type_info[index].plugin_path);
    EXPECT_EQ(actual.type_info[index].version,
              expected.type_info[index].version);
    EXPECT_EQ(actual.type_info[index].is_builtin,
              expected.type_info[index].is_builtin);
  }
}

/**
 * @brief Sets one process environment value for a bounded fixture scenario.
 *
 * @note Destruction restores the prior value or removes the key. Tests using
 * this guard are process-serial because environment variables are global.
 */
class ScopedSchedulerFixtureEnvironment final {
 public:
  /**
   * @brief Installs one fixture environment value and saves its predecessor.
   * @param name Environment key copied for the guard lifetime.
   * @param value New value visible to the dynamic scheduler plugin.
   * @throws std::runtime_error when the platform environment update fails.
   * @throws std::bad_alloc if owned key or previous-value storage cannot grow.
   */
  ScopedSchedulerFixtureEnvironment(const char* name, const std::string& value)
      : name_(name) {
    if (const char* previous = std::getenv(name)) {
      previous_ = std::string(previous);
    }
    set(value);
  }

  /**
   * @brief Restores the predecessor without throwing from test cleanup.
   * @throws Nothing; restoration failures are intentionally suppressed.
   */
  ~ScopedSchedulerFixtureEnvironment() noexcept {
    try {
      if (previous_) {
        set(*previous_);
      } else {
        clear();
      }
    } catch (...) {
      // Process-global test cleanup cannot safely surface restoration failure.
    }
  }

  ScopedSchedulerFixtureEnvironment(const ScopedSchedulerFixtureEnvironment&) =
      delete;
  ScopedSchedulerFixtureEnvironment& operator=(
      const ScopedSchedulerFixtureEnvironment&) = delete;

 private:
  /**
   * @brief Replaces the owned environment key with `value`.
   * @param value New process-global value.
   * @return Nothing.
   * @throws std::runtime_error when the platform update fails.
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
   * @throws std::runtime_error when the platform update fails.
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

  /** @brief Environment key retained for restoration. */
  std::string name_;
  /** @brief Previous value, or nullopt when the key was initially absent. */
  std::optional<std::string> previous_;
};

/**
 * @brief Returns a unique path for one scheduler-owner lifecycle trace.
 * @param label Stable scenario label included in the filename.
 * @return Path in GoogleTest's temporary directory.
 * @throws std::bad_alloc from path/string construction.
 */
std::filesystem::path scheduler_owner_trace_path(const std::string& label) {
  static std::atomic<unsigned int> sequence{0};
  return std::filesystem::path(::testing::TempDir()) /
         ("photospider-scheduler-owner-" + label + "-" +
          std::to_string(sequence.fetch_add(1, std::memory_order_relaxed)) +
          ".log");
}

/**
 * @brief Reads newline-delimited events emitted by the real scheduler fixture.
 * @param path Trace file selected through the fixture environment.
 * @return Events in actual lifecycle and final-unload order.
 * @throws std::bad_alloc if line/vector storage cannot allocate.
 * @note A missing trace returns an empty vector for a focused assertion.
 */
std::vector<std::string> read_scheduler_owner_trace(
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
 * @brief RAII owner for one current-thread allocation failpoint.
 * @note Assertions run after destruction so GoogleTest allocation is normal.
 */
class ScopedSchedulerAllocationFailure final {
 public:
  /**
   * @brief Arms a zero-based allocation failure.
   * @param allocation_index Allocation to reject.
   * @throws Nothing.
   */
  explicit ScopedSchedulerAllocationFailure(
      std::int64_t allocation_index) noexcept {
    scheduler_allocation_probe::arm(allocation_index);
  }

  /**
   * @brief Disarms the current-thread failpoint before assertions allocate.
   * @throws Nothing.
   */
  ~ScopedSchedulerAllocationFailure() noexcept {
    scheduler_allocation_probe::disarm();
  }

  ScopedSchedulerAllocationFailure(const ScopedSchedulerAllocationFailure&) =
      delete;
  ScopedSchedulerAllocationFailure& operator=(
      const ScopedSchedulerAllocationFailure&) = delete;
};

#ifndef PS_SCHEDULER_PLUGIN_DIR
#define PS_SCHEDULER_PLUGIN_DIR "build/schedulers"
#endif

#ifndef PS_TEST_SCHEDULER_PLUGIN_DIR
#define PS_TEST_SCHEDULER_PLUGIN_DIR "build/test_schedulers"
#endif

#ifndef PS_TEST_SCHEDULER_MISSING_HANDSHAKE_PATH
#define PS_TEST_SCHEDULER_MISSING_HANDSHAKE_PATH \
  "build/test_scheduler_incompatible/missing/"   \
  "missing_handshake_scheduler_plugin"
#endif

#ifndef PS_TEST_SCHEDULER_MISMATCHED_ABI_PATH
#define PS_TEST_SCHEDULER_MISMATCHED_ABI_PATH \
  "build/test_scheduler_incompatible/mismatch/mismatched_abi_scheduler_plugin"
#endif

/**
 * @brief Returns the CMake output directory for scheduler plugins.
 *
 * @param test_fixture True selects the test-only scheduler fixture output
 * directory; false selects the production/demo scheduler plugin output
 * directory.
 * @return Filesystem path injected by CMake for this test target, or the
 * legacy source-root-relative `build/schedulers` tree when compiled outside
 * CMake.
 * @throws std::bad_alloc from path string construction.
 * @note CMake injects absolute paths during normal builds so the loader tests
 * can execute from the source tree, the binary tree, or nested CI build
 * directories without assuming a literal `build/` directory name.
 */
std::filesystem::path scheduler_plugin_dir(bool test_fixture = false) {
  return std::filesystem::path(test_fixture ? PS_TEST_SCHEDULER_PLUGIN_DIR
                                            : PS_SCHEDULER_PLUGIN_DIR);
}

/**
 * @brief Builds the expected shared library path for a scheduler plugin.
 *
 * @param stem Platform-independent library target stem without prefix or
 * extension.
 * @param test_fixture True when the plugin lives in the test-only scheduler
 * output directory.
 * @return Platform-specific plugin path inside `scheduler_plugin_dir()`.
 * @throws std::bad_alloc from path and filename string construction.
 * @note The helper mirrors CMake's target output directories instead of using
 * source-root-relative paths, preserving CI compatibility with arbitrary
 * `BUILD_DIR` values.
 */
std::filesystem::path scheduler_plugin_path(const std::string& stem,
                                            bool test_fixture = false) {
  const std::filesystem::path dir = scheduler_plugin_dir(test_fixture);
#if defined(_WIN32)
  return dir / (stem + ".dll");
#elif defined(__APPLE__)
  return dir / ("lib" + stem + ".dylib");
#else
  return dir / ("lib" + stem + ".so");
#endif
}

/**
 * @brief Owns one test-only native handle used to inspect fixture exports.
 *
 * @throws std::bad_alloc if platform path-string construction fails.
 * @note The production loader retains its own independent DSO lifetime. Tests
 *       declare scheduler owners after this handle so those owners are
 *       destroyed before the final test handle can close.
 */
class ScopedSchedulerPluginHandle final {
 public:
  /**
   * @brief Opens one existing scheduler DSO for fixture-only symbol probes.
   * @param path Exact platform library path.
   * @throws std::bad_alloc if converting the path to owned text fails.
   */
  explicit ScopedSchedulerPluginHandle(const std::filesystem::path& path) {
#ifdef _WIN32
    handle_ = LoadLibrary(path.string().c_str());
#else
    handle_ = dlopen(path.string().c_str(), RTLD_LAZY);
#endif
  }

  /**
   * @brief Closes the test-owned native handle when open.
   * @throws Nothing.
   */
  ~ScopedSchedulerPluginHandle() noexcept {
#ifdef _WIN32
    if (handle_ != nullptr) {
      FreeLibrary(handle_);
    }
#else
    if (handle_ != nullptr) {
      dlclose(handle_);
    }
#endif
  }

  /**
   * @brief Prevents duplicate native-handle ownership.
   * @param other Handle owner that remains unchanged.
   * @throws Nothing because the operation is deleted.
   */
  ScopedSchedulerPluginHandle(const ScopedSchedulerPluginHandle& other) =
      delete;

  /**
   * @brief Prevents replacing one native-handle owner.
   * @param other Handle owner that remains unchanged.
   * @return No value because the operation is deleted.
   * @throws Nothing because the operation is deleted.
   */
  ScopedSchedulerPluginHandle& operator=(
      const ScopedSchedulerPluginHandle& other) = delete;

  /**
   * @brief Reports whether the native library opened successfully.
   * @return True when symbol resolution may be attempted.
   * @throws Nothing.
   */
  bool valid() const noexcept { return handle_ != nullptr; }

  /**
   * @brief Resolves one fixture-only function export.
   * @tparam Function Exact function-pointer type expected by the caller.
   * @param symbol Null-terminated export name.
   * @return Typed function pointer, or null when absent/unopened.
   * @throws Nothing.
   */
  template <typename Function>
  Function resolve(const char* symbol) const noexcept {
    if (handle_ == nullptr) {
      return nullptr;
    }
#ifdef _WIN32
    return reinterpret_cast<Function>(GetProcAddress(handle_, symbol));
#else
    return reinterpret_cast<Function>(dlsym(handle_, symbol));
#endif
  }

 private:
#ifdef _WIN32
  /** @brief Test-owned Windows library handle. */
  HMODULE handle_ = nullptr;
#else
  /** @brief Test-owned POSIX dynamic-library handle. */
  void* handle_ = nullptr;
#endif
};

bool contains_type(const std::vector<std::string>& types,
                   const std::string& type) {
  return std::find(types.begin(), types.end(), type) != types.end();
}

}  // namespace

class SchedulerPluginLoaderTest : public ::testing::Test {
 protected:
  void SetUp() override {
    // 清除之前的状态
    auto& loader = SchedulerPluginLoader::instance();
    loader.clear_plugins();
    loader.clear_errors();
  }
};

// 测试单例模式
TEST_F(SchedulerPluginLoaderTest, SingletonInstance) {
  auto& loader1 = SchedulerPluginLoader::instance();
  auto& loader2 = SchedulerPluginLoader::instance();
  EXPECT_EQ(&loader1, &loader2);
}

// 测试初始状态
TEST_F(SchedulerPluginLoaderTest, InitialState) {
  auto& loader = SchedulerPluginLoader::instance();

  // 初始时应该没有已注册的类型（插件还未加载）
  auto types = loader.get_registered_types();
  EXPECT_TRUE(types.empty());

  // 加载错误列表应该为空
  auto errors = loader.get_load_errors();
  EXPECT_TRUE(errors.empty());
}

/**
 * @brief Locks the current scheduler plugin ABI to version two.
 * @return Nothing; GoogleTest records an obsolete public SDK generation.
 * @throws Nothing.
 * @note Exact equality is intentional: scheduler ABI v1 has no compatible
 *       resolved-worker-grant contract and must not remain loadable.
 */
TEST_F(SchedulerPluginLoaderTest, PublicSchedulerAbiIsExactlyVersionTwo) {
  EXPECT_EQ(PS_SCHEDULER_PLUGIN_ABI_VERSION, 2U);
}

/**
 * @brief Rejects zero and above-limit plugin grants before construction.
 * @return Nothing; GoogleTest records any accepted non-grant value.
 * @throws Nothing when the loader validates before invoking the fixture
 *         factory; unexpected exceptions are reported by GoogleTest.
 * @note SchedulerFactory resolves automatic requests before this private
 *       boundary, so every compatible plugin create call must receive one
 *       exact hard grant in `[1,8]`.
 */
TEST_F(SchedulerPluginLoaderTest,
       PluginCreateRejectsEveryValueOutsideResolvedGrantRange) {
  auto& loader = SchedulerPluginLoader::instance();
  ASSERT_TRUE(loader.load_plugin(
      scheduler_plugin_path("destroy_count_scheduler_plugin", true)));

  EXPECT_THROW((void)loader.create(kDestroyCountSchedulerType, 0U),
               std::invalid_argument);
  EXPECT_THROW((void)loader.create(kDestroyCountSchedulerType,
                                   kSchedulerWorkerRequestMax + 1U),
               std::invalid_argument);
}

/**
 * @brief Makes the repository fixture itself reject and record invalid grants.
 * @return Nothing; GoogleTest records raw export or probe mismatches.
 * @throws std::bad_alloc if native path or assertion storage cannot allocate.
 * @note This direct ABI call bypasses SchedulerPluginLoader deliberately. It
 *       proves the fixture independently adopts ABI v2 instead of relying only
 *       on the Host's pre-export validation.
 */
TEST_F(SchedulerPluginLoaderTest,
       FixtureCreateRejectsAndRecordsInvalidAbiV2Grants) {
  const std::filesystem::path plugin_path =
      scheduler_plugin_path("destroy_count_scheduler_plugin", true);
  ASSERT_TRUE(std::filesystem::exists(plugin_path));
  ScopedSchedulerPluginHandle probe_handle(plugin_path);
  ASSERT_TRUE(probe_handle.valid());
  const auto create_scheduler = probe_handle.resolve<SchedulerPluginCreateFunc>(
      kSchedulerPluginCreateSymbol);
  const auto reset_counts = probe_handle.resolve<void (*)() noexcept>(
      "ps_test_scheduler_reset_counts");
  const auto create_count = probe_handle.resolve<int (*)() noexcept>(
      "ps_test_scheduler_create_count");
  const auto last_worker_grant =
      probe_handle.resolve<std::uint32_t (*)() noexcept>(
          "ps_test_scheduler_last_worker_grant");
  const auto active_count = probe_handle.resolve<int (*)() noexcept>(
      "ps_test_scheduler_active_count");
  const auto destroy_count = probe_handle.resolve<int (*)() noexcept>(
      "ps_test_scheduler_destroy_count");
  ASSERT_NE(create_scheduler, nullptr);
  ASSERT_NE(reset_counts, nullptr);
  ASSERT_NE(create_count, nullptr);
  ASSERT_NE(last_worker_grant, nullptr);
  ASSERT_NE(active_count, nullptr);
  ASSERT_NE(destroy_count, nullptr);

  for (const std::uint32_t grant :
       std::array<std::uint32_t, 2>{0U, kSchedulerWorkerRequestMax + 1U}) {
    reset_counts();
    EXPECT_EQ(create_scheduler(kDestroyCountSchedulerType, grant), nullptr);
    EXPECT_EQ(create_count(), 1);
    EXPECT_EQ(last_worker_grant(), grant);
    EXPECT_EQ(active_count(), 0);
    EXPECT_EQ(destroy_count(), 0);
  }
}

/**
 * @brief Passes resolved automatic and exact worker grants to an ABI v2 DSO.
 * @return Nothing; GoogleTest records planning, export, or lifecycle mismatch.
 * @throws std::bad_alloc or std::system_error if fixture loading, planning,
 *         reservation, owner construction, or synchronization fails.
 * @note Deterministic hardware inputs cover unavailable and above-ceiling
 *       detection without replacing production platform detection. Count nine
 *       must fail during planning without invoking the create export.
 */
TEST_F(SchedulerPluginLoaderTest,
       FactoryPassesResolvedAutomaticAndExactPluginGrants) {
  const std::filesystem::path plugin_path =
      scheduler_plugin_path("destroy_count_scheduler_plugin", true);
  ASSERT_TRUE(std::filesystem::exists(plugin_path));
  ScopedSchedulerPluginHandle probe_handle(plugin_path);
  ASSERT_TRUE(probe_handle.valid());
  const auto reset_counts = probe_handle.resolve<void (*)() noexcept>(
      "ps_test_scheduler_reset_counts");
  const auto create_count = probe_handle.resolve<int (*)() noexcept>(
      "ps_test_scheduler_create_count");
  const auto last_worker_grant =
      probe_handle.resolve<std::uint32_t (*)() noexcept>(
          "ps_test_scheduler_last_worker_grant");
  const auto active_count = probe_handle.resolve<int (*)() noexcept>(
      "ps_test_scheduler_active_count");
  const auto destroy_count = probe_handle.resolve<int (*)() noexcept>(
      "ps_test_scheduler_destroy_count");
  ASSERT_NE(reset_counts, nullptr);
  ASSERT_NE(create_count, nullptr);
  ASSERT_NE(last_worker_grant, nullptr);
  ASSERT_NE(active_count, nullptr);
  ASSERT_NE(destroy_count, nullptr);

  auto& loader = SchedulerPluginLoader::instance();
  ASSERT_TRUE(loader.load_plugin(plugin_path));
  /**
   * @brief One deterministic configured/detected/resolved planning scenario.
   * @throws Nothing.
   */
  struct GrantCase {
    /** @brief Configured request supplied to deterministic planning. */
    unsigned int configured;
    /** @brief Simulated platform hardware concurrency. */
    unsigned int detected;
    /** @brief Exact resolved plugin grant expected at the DSO. */
    unsigned int expected;
  };
  const std::array<GrantCase, 4> cases = {
      GrantCase{0U, 0U, 1U},
      GrantCase{0U, kSchedulerWorkerRequestMax + 19U,
                kSchedulerWorkerRequestMax},
      GrantCase{1U, kSchedulerWorkerRequestMax, 1U},
      GrantCase{kSchedulerWorkerRequestMax, 1U, kSchedulerWorkerRequestMax}};
  SchedulerWorkerBudget budget(kSchedulerWorkerRequestMax);

  for (const GrantCase& grant_case : cases) {
    reset_counts();
    const std::optional<SchedulerPlan> plan =
        SchedulerFactory::plan_for_hardware(kDestroyCountSchedulerType,
                                            grant_case.configured,
                                            grant_case.detected);
    ASSERT_TRUE(plan.has_value());
    EXPECT_EQ(plan->worker_grant(), grant_case.expected);
    EXPECT_EQ(plan->reservation_slots(), grant_case.expected);
    std::optional<SchedulerWorkerBudget::Reservation> reservation =
        budget.try_reserve(plan->reservation_slots());
    ASSERT_TRUE(reservation.has_value());
    std::unique_ptr<IScheduler> scheduler =
        SchedulerFactory::create(*plan, std::move(*reservation));
    ASSERT_NE(scheduler, nullptr);
    EXPECT_EQ(create_count(), 1);
    EXPECT_EQ(last_worker_grant(), grant_case.expected);
    EXPECT_EQ(active_count(), 1);
    EXPECT_EQ(destroy_count(), 0);
    scheduler.reset();
    EXPECT_EQ(active_count(), 0);
    EXPECT_EQ(destroy_count(), 1);
  }

  reset_counts();
  EXPECT_THROW((void)SchedulerFactory::plan_for_hardware(
                   kDestroyCountSchedulerType, kSchedulerWorkerRequestMax + 1U,
                   kSchedulerWorkerRequestMax),
               std::invalid_argument);
  EXPECT_EQ(create_count(), 0);
  EXPECT_EQ(last_worker_grant(), 0U);
  EXPECT_EQ(active_count(), 0);
  EXPECT_EQ(destroy_count(), 0);
}

/**
 * @brief Retains the grant until concrete plugin-instance destruction.
 * @return Nothing; GoogleTest records capacity, grant, or lifecycle mismatch.
 * @throws std::bad_alloc or std::system_error if fixture loading, planning,
 *         reservation, construction, or synchronization fails.
 * @note An isolated eight-slot budget avoids global process-state coupling.
 *       Capacity must remain unavailable while the plugin owner is alive and
 *       become exactly reusable only after its instance destroy export has
 *       completed; loader and probe handles may keep the DSO mapped.
 */
TEST_F(SchedulerPluginLoaderTest,
       PluginOwnerRetainsGrantUntilConcreteDestruction) {
  const std::filesystem::path plugin_path =
      scheduler_plugin_path("destroy_count_scheduler_plugin", true);
  ASSERT_TRUE(std::filesystem::exists(plugin_path));
  ScopedSchedulerPluginHandle probe_handle(plugin_path);
  ASSERT_TRUE(probe_handle.valid());
  const auto reset_counts = probe_handle.resolve<void (*)() noexcept>(
      "ps_test_scheduler_reset_counts");
  const auto create_count = probe_handle.resolve<int (*)() noexcept>(
      "ps_test_scheduler_create_count");
  const auto last_worker_grant =
      probe_handle.resolve<std::uint32_t (*)() noexcept>(
          "ps_test_scheduler_last_worker_grant");
  const auto active_count = probe_handle.resolve<int (*)() noexcept>(
      "ps_test_scheduler_active_count");
  const auto destroy_count = probe_handle.resolve<int (*)() noexcept>(
      "ps_test_scheduler_destroy_count");
  ASSERT_NE(reset_counts, nullptr);
  ASSERT_NE(create_count, nullptr);
  ASSERT_NE(last_worker_grant, nullptr);
  ASSERT_NE(active_count, nullptr);
  ASSERT_NE(destroy_count, nullptr);

  auto& loader = SchedulerPluginLoader::instance();
  ASSERT_TRUE(loader.load_plugin(plugin_path));
  reset_counts();
  const std::optional<SchedulerPlan> plan = SchedulerFactory::plan_for_hardware(
      kDestroyCountSchedulerType, kSchedulerWorkerRequestMax, 1U);
  ASSERT_TRUE(plan.has_value());
  SchedulerWorkerBudget budget(kSchedulerWorkerRequestMax);
  std::optional<SchedulerWorkerBudget::Reservation> reservation =
      budget.try_reserve(plan->reservation_slots());
  ASSERT_TRUE(reservation.has_value());
  std::unique_ptr<IScheduler> scheduler =
      SchedulerFactory::create(*plan, std::move(*reservation));
  ASSERT_NE(scheduler, nullptr);
  EXPECT_EQ(create_count(), 1);
  EXPECT_EQ(last_worker_grant(), kSchedulerWorkerRequestMax);
  EXPECT_EQ(active_count(), 1);
  EXPECT_EQ(destroy_count(), 0);
  EXPECT_FALSE(budget.try_reserve(1U).has_value());

  scheduler.reset();
  EXPECT_EQ(active_count(), 0);
  EXPECT_EQ(destroy_count(), 1);
  std::optional<SchedulerWorkerBudget::Reservation> recovered =
      budget.try_reserve(kSchedulerWorkerRequestMax);
  ASSERT_TRUE(recovered.has_value());
  EXPECT_FALSE(budget.try_reserve(1U).has_value());
  recovered.reset();
  EXPECT_TRUE(budget.try_reserve(kSchedulerWorkerRequestMax).has_value());
}

/**
 * @brief Checks completion-counter boundaries through the real plugin owner.
 * @return Nothing.
 * @throws Nothing when nonpositive deltas are ignored and overflow preserves
 * the fixture counter.
 */
TEST_F(SchedulerPluginLoaderTest, FixtureCompletionCounterIsChecked) {
  auto& loader = SchedulerPluginLoader::instance();
  ASSERT_TRUE(loader.load_plugin(
      scheduler_plugin_path("destroy_count_scheduler_plugin", true)));
  auto scheduler = loader.create(kDestroyCountSchedulerType, 1U);
  ASSERT_NE(scheduler, nullptr);

  scheduler->inc_tasks_to_complete(0);
  scheduler->inc_tasks_to_complete(-1);
  EXPECT_EQ(scheduler->get_stats(), "completion=0");
  scheduler->inc_tasks_to_complete(std::numeric_limits<int>::max());
  EXPECT_EQ(scheduler->get_stats(),
            "completion=" + std::to_string(std::numeric_limits<int>::max()));
  try {
    scheduler->inc_tasks_to_complete(1);
    FAIL() << "plugin overflow did not cross the owner boundary";
  } catch (const GraphError& error) {
    EXPECT_EQ(error.code(), GraphErrc::ComputeError);
    EXPECT_STREQ(error.what(), "fixture completion counter overflow");
  }
  EXPECT_EQ(scheduler->get_stats(),
            "completion=" + std::to_string(std::numeric_limits<int>::max()));
  scheduler->dec_tasks_to_complete();
  EXPECT_EQ(
      scheduler->get_stats(),
      "completion=" + std::to_string(std::numeric_limits<int>::max() - 1));
}

// 测试扫描不存在的目录
TEST_F(SchedulerPluginLoaderTest, ScanNonExistentDirectory) {
  auto& loader = SchedulerPluginLoader::instance();

  size_t count = loader.scan_and_load("/nonexistent/directory");
  EXPECT_EQ(count, 0);
}

// 测试扫描实际的调度器插件目录
TEST_F(SchedulerPluginLoaderTest, ScanSchedulerDirectory) {
  auto& loader = SchedulerPluginLoader::instance();

  const auto plugin_dir = scheduler_plugin_dir();
  size_t count = loader.scan_and_load(plugin_dir.string());

  EXPECT_GE(count, 3u);

  // 如果加载成功，验证类型
  if (count > 0) {
    auto types = loader.get_registered_types();
    EXPECT_FALSE(types.empty());
    EXPECT_TRUE(contains_type(types, "cpu_work_stealing_example"));
    EXPECT_TRUE(contains_type(types, "serial_debug_example"));
    EXPECT_TRUE(contains_type(types, "gpu_pipeline_example"));
    EXPECT_TRUE(contains_type(types, "heterogeneous_example"));
    EXPECT_FALSE(contains_type(types, "destroy_count_test"))
        << "test-only scheduler fixture must not be exposed in " << plugin_dir;

    // 打印已加载的类型
    std::cout << "Loaded scheduler types from plugins:\n";
    for (const auto& type : types) {
      std::cout << "  - " << type << "\n";
    }
  }
}

// 测试加载单个插件
TEST_F(SchedulerPluginLoaderTest, LoadSinglePlugin) {
  auto& loader = SchedulerPluginLoader::instance();

  bool result = loader.load_plugin(
      scheduler_plugin_path("cpu_work_stealing_example_plugin").string());

  if (result) {
    EXPECT_TRUE(loader.is_registered("cpu_work_stealing_example"));
  }
}

/**
 * @brief Rejects a DSO with no numeric ABI handshake before discovery.
 * @return Nothing.
 * @throws Nothing when only one recoverable diagnostic is published and every
 * plugin callback probe remains zero.
 * @note A test-owned DSO handle keeps the probe exports mapped after the
 * loader releases its rejected candidate lifetime.
 */
TEST_F(SchedulerPluginLoaderTest,
       MissingHandshakeInvokesNoDiscoveryOrFactoryExport) {
  auto& loader = SchedulerPluginLoader::instance();
  const std::filesystem::path plugin_path(
      PS_TEST_SCHEDULER_MISSING_HANDSHAKE_PATH);
  ASSERT_TRUE(std::filesystem::exists(plugin_path));

#ifdef _WIN32
  HMODULE test_handle = LoadLibrary(plugin_path.string().c_str());
  ASSERT_NE(test_handle, nullptr);
  auto reset_counts = reinterpret_cast<void (*)()>(
      GetProcAddress(test_handle, "ps_test_scheduler_reset_counts"));
  auto read_load_probe = reinterpret_cast<int (*)(int)>(
      GetProcAddress(test_handle, "ps_test_scheduler_load_probe_count"));
#else
  void* test_handle = dlopen(plugin_path.string().c_str(), RTLD_LAZY);
  ASSERT_NE(test_handle, nullptr) << dlerror();
  auto reset_counts = reinterpret_cast<void (*)()>(
      dlsym(test_handle, "ps_test_scheduler_reset_counts"));
  auto read_load_probe = reinterpret_cast<int (*)(int)>(
      dlsym(test_handle, "ps_test_scheduler_load_probe_count"));
#endif
  ASSERT_NE(reset_counts, nullptr);
  ASSERT_NE(read_load_probe, nullptr);
  reset_counts();
  const SchedulerPluginRegistrySnapshot before =
      capture_plugin_registry(loader);

  EXPECT_FALSE(loader.load_plugin(plugin_path));

  const SchedulerPluginRegistrySnapshot after = capture_plugin_registry(loader);
  SchedulerPluginRegistrySnapshot expected = before;
  expected.load_errors = after.load_errors;
  expect_plugin_registry_equal(expected, after);
  ASSERT_EQ(after.load_errors.size(), before.load_errors.size() + 1U);
  EXPECT_NE(after.load_errors.back().find("missing ABI handshake"),
            std::string::npos);
  for (int probe = static_cast<int>(SchedulerLoadProbe::GetAbiVersion);
       probe <= static_cast<int>(SchedulerLoadProbe::GetVersion); ++probe) {
    EXPECT_EQ(read_load_probe(probe), 0);
  }

#ifdef _WIN32
  FreeLibrary(test_handle);
#else
  dlclose(test_handle);
#endif
}

/**
 * @brief Rejects ABI v1 after exactly one numeric handshake callback.
 * @return Nothing.
 * @throws Nothing when all later discovery/factory/lifecycle probes remain
 * zero and the type/metadata/handle registries remain unchanged.
 * @note The exact diagnostic proves the fixture is specifically old ABI v1,
 *       not an arbitrary future mismatch or missing-export candidate.
 */
TEST_F(SchedulerPluginLoaderTest,
       AbiV1InvokesOnlyHandshakeAndPublishesNoRegistration) {
  auto& loader = SchedulerPluginLoader::instance();
  const std::filesystem::path plugin_path(
      PS_TEST_SCHEDULER_MISMATCHED_ABI_PATH);
  ASSERT_TRUE(std::filesystem::exists(plugin_path));
  ScopedSchedulerPluginHandle probe_handle(plugin_path);
  ASSERT_TRUE(probe_handle.valid());
  const auto reset_counts = probe_handle.resolve<void (*)() noexcept>(
      "ps_test_scheduler_reset_counts");
  const auto read_load_probe = probe_handle.resolve<LoadProbeReader*>(
      "ps_test_scheduler_load_probe_count");
  const auto create_count = probe_handle.resolve<int (*)() noexcept>(
      "ps_test_scheduler_create_count");
  const auto destroy_count = probe_handle.resolve<int (*)() noexcept>(
      "ps_test_scheduler_destroy_count");
  ASSERT_NE(reset_counts, nullptr);
  ASSERT_NE(read_load_probe, nullptr);
  ASSERT_NE(create_count, nullptr);
  ASSERT_NE(destroy_count, nullptr);
  reset_counts();
  const SchedulerPluginRegistrySnapshot before =
      capture_plugin_registry(loader);

  EXPECT_FALSE(loader.load_plugin(plugin_path));

  const SchedulerPluginRegistrySnapshot after = capture_plugin_registry(loader);
  SchedulerPluginRegistrySnapshot expected = before;
  expected.load_errors = after.load_errors;
  expect_plugin_registry_equal(expected, after);
  ASSERT_EQ(after.load_errors.size(), before.load_errors.size() + 1U);
  EXPECT_NE(after.load_errors.back().find("ABI mismatch"), std::string::npos);
  EXPECT_NE(after.load_errors.back().find("expected 2, got 1"),
            std::string::npos);
  EXPECT_EQ(
      load_probe_count(read_load_probe, SchedulerLoadProbe::GetAbiVersion), 1);
  EXPECT_EQ(load_probe_count(read_load_probe, SchedulerLoadProbe::GetCount), 0);
  EXPECT_EQ(load_probe_count(read_load_probe, SchedulerLoadProbe::GetName), 0);
  EXPECT_EQ(
      load_probe_count(read_load_probe, SchedulerLoadProbe::GetDescription), 0);
  EXPECT_EQ(load_probe_count(read_load_probe, SchedulerLoadProbe::GetVersion),
            0);
  EXPECT_EQ(create_count(), 0);
  EXPECT_EQ(destroy_count(), 0);
}

/**
 * @brief Verifies the human version export is called once and then cached.
 * @return Nothing.
 * @throws Nothing when repeated metadata/list snapshots do not re-enter the
 * plugin DSO.
 */
TEST_F(SchedulerPluginLoaderTest, HumanVersionIsCachedAtLoadTime) {
  auto& loader = SchedulerPluginLoader::instance();
  const auto plugin_path =
      scheduler_plugin_path("destroy_count_scheduler_plugin", true);
  ASSERT_TRUE(std::filesystem::exists(plugin_path));

#ifdef _WIN32
  HMODULE test_handle = LoadLibrary(plugin_path.string().c_str());
  ASSERT_NE(test_handle, nullptr);
  auto reset_counts = reinterpret_cast<void (*)()>(
      GetProcAddress(test_handle, "ps_test_scheduler_reset_counts"));
  auto read_load_probe = reinterpret_cast<int (*)(int)>(
      GetProcAddress(test_handle, "ps_test_scheduler_load_probe_count"));
#else
  void* test_handle = dlopen(plugin_path.string().c_str(), RTLD_LAZY);
  ASSERT_NE(test_handle, nullptr) << dlerror();
  auto reset_counts = reinterpret_cast<void (*)()>(
      dlsym(test_handle, "ps_test_scheduler_reset_counts"));
  auto read_load_probe = reinterpret_cast<int (*)(int)>(
      dlsym(test_handle, "ps_test_scheduler_load_probe_count"));
#endif
  ASSERT_NE(reset_counts, nullptr);
  ASSERT_NE(read_load_probe, nullptr);
  reset_counts();

  ASSERT_TRUE(loader.load_plugin(plugin_path));
  EXPECT_EQ(
      load_probe_count(read_load_probe, SchedulerLoadProbe::GetAbiVersion), 1);
  EXPECT_EQ(load_probe_count(read_load_probe, SchedulerLoadProbe::GetVersion),
            1);
  for (int iteration = 0; iteration < 3; ++iteration) {
    EXPECT_FALSE(loader.list_loaded_plugins().empty());
    EXPECT_FALSE(loader.get_all_info().empty());
    EXPECT_TRUE(loader.get_info(kDestroyCountSchedulerType).has_value());
  }
  EXPECT_EQ(load_probe_count(read_load_probe, SchedulerLoadProbe::GetVersion),
            1);

  loader.clear_plugins();
#ifdef _WIN32
  FreeLibrary(test_handle);
#else
  dlclose(test_handle);
#endif
}

// 测试获取描述
TEST_F(SchedulerPluginLoaderTest, GetDescription) {
  auto& loader = SchedulerPluginLoader::instance();

  loader.scan_and_load(scheduler_plugin_dir().string());

  auto types = loader.get_registered_types();
  for (const auto& type : types) {
    auto desc = loader.get_description(type);
    // 描述应该非空
    EXPECT_FALSE(desc.empty()) << "Type: " << type;
  }
}

// 测试创建调度器实例
TEST_F(SchedulerPluginLoaderTest, CreateSchedulerFromPlugin) {
  auto& loader = SchedulerPluginLoader::instance();

  const auto plugin_dir = scheduler_plugin_dir();
  size_t count = loader.scan_and_load(plugin_dir.string());
  if (count == 0) {
    GTEST_SKIP() << "No plugins found in " << plugin_dir;
  }

  auto types = loader.get_registered_types();
  for (const auto& type : types) {
    auto scheduler = loader.create(type, 4);
    EXPECT_NE(scheduler, nullptr)
        << "Failed to create scheduler of type: " << type;

    if (scheduler) {
      // 验证基本功能
      scheduler->start();
      auto stats = scheduler->get_stats();
      EXPECT_FALSE(stats.empty());
      scheduler->shutdown();
    }
  }
}

// 测试列出已加载的插件
TEST_F(SchedulerPluginLoaderTest, ListLoadedPlugins) {
  auto& loader = SchedulerPluginLoader::instance();

  loader.scan_and_load(scheduler_plugin_dir().string());

  auto plugins = loader.list_loaded_plugins();
  std::cout << "Loaded plugins:\n";
  for (const auto& info : plugins) {
    std::cout << "  " << info << "\n";
  }
}

// 测试 SchedulerFactory 集成
TEST_F(SchedulerPluginLoaderTest, FactoryIntegration) {
  auto& loader = SchedulerPluginLoader::instance();

  size_t count = loader.scan_and_load(scheduler_plugin_dir().string());
  if (count == 0) {
    GTEST_SKIP() << "No plugins found";
  }

  // 通过 SchedulerFactory 创建插件调度器
  auto types = loader.get_registered_types();
  for (const auto& type : types) {
    // Factory 应该能够创建插件类型
    auto scheduler = SchedulerFactory::create(type, 2);
    EXPECT_NE(scheduler, nullptr) << "Factory failed to create: " << type;
  }
}

/**
 * @brief Rejects a null heterogeneous example type with one legal ABI grant.
 * @return Nothing; GoogleTest records loading, symbol, or result mismatch.
 * @throws std::bad_alloc if plugin-path construction or diagnostics allocate.
 * @note The legal grant isolates type validation from ABI v2 worker-grant
 *       validation; no scheduler ownership is transferred for the null type.
 */
TEST_F(SchedulerPluginLoaderTest, GpuPipelineExampleCreateRejectsNullTypeName) {
  const auto plugin_path = scheduler_plugin_path("gpu_pipeline_example_plugin");

  if (!std::filesystem::exists(plugin_path)) {
    GTEST_SKIP() << "gpu pipeline scheduler plugin was not built";
  }

#ifdef _WIN32
  HMODULE test_handle = LoadLibrary(plugin_path.string().c_str());
  ASSERT_NE(test_handle, nullptr);
  auto create_scheduler = reinterpret_cast<SchedulerPluginCreateFunc>(
      GetProcAddress(test_handle, kSchedulerPluginCreateSymbol));
#else
  void* test_handle = dlopen(plugin_path.string().c_str(), RTLD_LAZY);
  ASSERT_NE(test_handle, nullptr) << dlerror();
  auto create_scheduler = reinterpret_cast<SchedulerPluginCreateFunc>(
      dlsym(test_handle, kSchedulerPluginCreateSymbol));
#endif
  ASSERT_NE(create_scheduler, nullptr);
  EXPECT_EQ(create_scheduler(nullptr, 1U), nullptr);

#ifdef _WIN32
  FreeLibrary(test_handle);
#else
  dlclose(test_handle);
#endif
}

// 测试清除插件
TEST_F(SchedulerPluginLoaderTest, ClearPlugins) {
  auto& loader = SchedulerPluginLoader::instance();

  const auto plugin_dir = scheduler_plugin_dir();
  loader.scan_and_load(plugin_dir.string());

  auto before = loader.get_registered_types();
  size_t before_count = before.size();

  // 清除
  loader.clear_plugins();

  auto after = loader.get_registered_types();
  EXPECT_TRUE(after.empty());

  // 可以重新加载
  loader.scan_and_load(plugin_dir.string());
  auto reloaded = loader.get_registered_types();
  EXPECT_EQ(reloaded.size(), before_count);
}

TEST_F(SchedulerPluginLoaderTest, PluginSchedulerUsesPluginDestroyAfterClear) {
  auto& loader = SchedulerPluginLoader::instance();

  const auto plugin_path =
      scheduler_plugin_path("destroy_count_scheduler_plugin", true);

  if (!std::filesystem::exists(plugin_path)) {
    GTEST_SKIP() << "destroy-count scheduler plugin was not built";
  }

#ifdef _WIN32
  HMODULE test_handle = LoadLibrary(plugin_path.string().c_str());
  ASSERT_NE(test_handle, nullptr);
  auto reset_counts = reinterpret_cast<void (*)()>(
      GetProcAddress(test_handle, "ps_test_scheduler_reset_counts"));
  auto active_count = reinterpret_cast<int (*)()>(
      GetProcAddress(test_handle, "ps_test_scheduler_active_count"));
  auto destroy_count = reinterpret_cast<int (*)()>(
      GetProcAddress(test_handle, "ps_test_scheduler_destroy_count"));
#else
  void* test_handle = dlopen(plugin_path.string().c_str(), RTLD_LAZY);
  ASSERT_NE(test_handle, nullptr) << dlerror();
  auto reset_counts = reinterpret_cast<void (*)()>(
      dlsym(test_handle, "ps_test_scheduler_reset_counts"));
  auto active_count = reinterpret_cast<int (*)()>(
      dlsym(test_handle, "ps_test_scheduler_active_count"));
  auto destroy_count = reinterpret_cast<int (*)()>(
      dlsym(test_handle, "ps_test_scheduler_destroy_count"));
#endif
  ASSERT_NE(reset_counts, nullptr);
  ASSERT_NE(active_count, nullptr);
  ASSERT_NE(destroy_count, nullptr);
  reset_counts();

  ASSERT_TRUE(loader.load_plugin(plugin_path));
  auto scheduler = loader.create("destroy_count_test", 1U);
  ASSERT_NE(scheduler, nullptr);
  EXPECT_EQ(active_count(), 1);
  EXPECT_EQ(destroy_count(), 0);

  loader.clear_plugins();
  EXPECT_EQ(active_count(), 1);
  scheduler.reset();
  EXPECT_EQ(active_count(), 0);
  EXPECT_EQ(destroy_count(), 1);

#ifdef _WIN32
  FreeLibrary(test_handle);
#else
  dlclose(test_handle);
#endif
}

/**
 * @brief Verifies the owner forwards the complete minimal public runtime API.
 * @return Nothing.
 * @throws Nothing when device discovery, both handle batches, and one
 * any-thread callback reach the real plugin overrides.
 * @note Fixture counters prove which dynamic virtual functions ran; callback
 * count and task-id sum prove the supplied handles executed exactly once.
 */
TEST_F(SchedulerPluginLoaderTest,
       OwnerForwardsDevicesAndEveryTaskHandleOverride) {
  auto& loader = SchedulerPluginLoader::instance();
  const auto plugin_path =
      scheduler_plugin_path("destroy_count_scheduler_plugin", true);
  ASSERT_TRUE(std::filesystem::exists(plugin_path));

#ifdef _WIN32
  HMODULE test_handle = LoadLibrary(plugin_path.string().c_str());
  ASSERT_NE(test_handle, nullptr);
  auto reset_counts = reinterpret_cast<void (*)()>(
      GetProcAddress(test_handle, "ps_test_scheduler_reset_counts"));
  auto read_forwarding_count = reinterpret_cast<int (*)(int)>(
      GetProcAddress(test_handle, "ps_test_scheduler_forwarding_count"));
#else
  void* test_handle = dlopen(plugin_path.string().c_str(), RTLD_LAZY);
  ASSERT_NE(test_handle, nullptr) << dlerror();
  auto reset_counts = reinterpret_cast<void (*)()>(
      dlsym(test_handle, "ps_test_scheduler_reset_counts"));
  auto read_forwarding_count = reinterpret_cast<int (*)(int)>(
      dlsym(test_handle, "ps_test_scheduler_forwarding_count"));
#endif
  ASSERT_NE(reset_counts, nullptr);
  ASSERT_NE(read_forwarding_count, nullptr);
  ASSERT_TRUE(loader.load_plugin(plugin_path));
  reset_counts();

  auto scheduler = loader.create("destroy_count_test", 1U);
  ASSERT_NE(scheduler, nullptr);
  IScheduler* runtime = scheduler.get();

  EXPECT_EQ(runtime->available_devices(),
            (std::vector<Device>{Device::CPU, Device::GPU_METAL}));

  CountingTaskExecutor executor;
  std::vector<TaskHandle> initial_handles{TaskHandle{&executor, 1, 101},
                                          TaskHandle{&executor, 2, 102}};
  runtime->submit_initial_task_handles(std::move(initial_handles), 2,
                                       SchedulerTaskPriority::High);
  std::vector<TaskHandle> worker_handles{TaskHandle{&executor, 3, 103},
                                         TaskHandle{&executor, 4, 104}};
  runtime->submit_ready_task_handles_from_worker(std::move(worker_handles),
                                                 SchedulerTaskPriority::High);
  runtime->submit_ready_task_any_thread([&executor]() { executor.run_task(5); },
                                        SchedulerTaskPriority::Normal,
                                        std::uint64_t{41});

  EXPECT_EQ(executor.callback_count(), 5);
  EXPECT_EQ(executor.task_id_sum(), 15);
  EXPECT_EQ(forwarding_probe_count(read_forwarding_count,
                                   SchedulerForwardingProbe::AvailableDevices),
            1);
  EXPECT_EQ(forwarding_probe_count(read_forwarding_count,
                                   SchedulerForwardingProbe::InitialHandles),
            1);
  EXPECT_EQ(forwarding_probe_count(read_forwarding_count,
                                   SchedulerForwardingProbe::WorkerHandles),
            1);
  EXPECT_EQ(forwarding_probe_count(read_forwarding_count,
                                   SchedulerForwardingProbe::AnyThreadTask),
            1);

  scheduler.reset();
  loader.clear_plugins();
#ifdef _WIN32
  FreeLibrary(test_handle);
#else
  dlclose(test_handle);
#endif
}

/**
 * @brief Verifies plugin batch bad_alloc crosses the owner before callbacks.
 * @return Nothing.
 * @throws Nothing when both public handle-batch overrides propagate
 * `std::bad_alloc` and executes zero borrowed handles.
 * @note Per-method fixture counters prove neither batch was decomposed.
 */
TEST_F(SchedulerPluginLoaderTest,
       OwnerPreservesBatchBadAllocBeforeAnyTaskHandleCallback) {
  auto& loader = SchedulerPluginLoader::instance();
  const auto plugin_path =
      scheduler_plugin_path("destroy_count_scheduler_plugin", true);
  ASSERT_TRUE(std::filesystem::exists(plugin_path));

#ifdef _WIN32
  HMODULE test_handle = LoadLibrary(plugin_path.string().c_str());
  ASSERT_NE(test_handle, nullptr);
  auto reset_counts = reinterpret_cast<void (*)()>(
      GetProcAddress(test_handle, "ps_test_scheduler_reset_counts"));
  auto read_forwarding_count = reinterpret_cast<int (*)(int)>(
      GetProcAddress(test_handle, "ps_test_scheduler_forwarding_count"));
#else
  void* test_handle = dlopen(plugin_path.string().c_str(), RTLD_LAZY);
  ASSERT_NE(test_handle, nullptr) << dlerror();
  auto reset_counts = reinterpret_cast<void (*)()>(
      dlsym(test_handle, "ps_test_scheduler_reset_counts"));
  auto read_forwarding_count = reinterpret_cast<int (*)(int)>(
      dlsym(test_handle, "ps_test_scheduler_forwarding_count"));
#endif
  ASSERT_NE(reset_counts, nullptr);
  ASSERT_NE(read_forwarding_count, nullptr);
  ASSERT_TRUE(loader.load_plugin(plugin_path));
  reset_counts();

  auto scheduler = loader.create("destroy_count_test", 1U);
  ASSERT_NE(scheduler, nullptr);
  IScheduler* runtime = scheduler.get();
  CountingTaskExecutor executor;

  {
    ScopedSchedulerFixtureEnvironment failures(kDestroyCountFailureEnvironment,
                                               "handle_batch_bad_alloc");
    std::vector<TaskHandle> initial_handles{TaskHandle{&executor, 1, 101},
                                            TaskHandle{&executor, 2, 102}};
    EXPECT_THROW(
        runtime->submit_initial_task_handles(std::move(initial_handles), 2,
                                             SchedulerTaskPriority::High),
        std::bad_alloc);

    std::vector<TaskHandle> worker_handles{TaskHandle{&executor, 3, 103},
                                           TaskHandle{&executor, 4, 104}};
    EXPECT_THROW(runtime->submit_ready_task_handles_from_worker(
                     std::move(worker_handles), SchedulerTaskPriority::Normal),
                 std::bad_alloc);
  }

  EXPECT_EQ(executor.callback_count(), 0);
  EXPECT_EQ(executor.task_id_sum(), 0);
  EXPECT_EQ(forwarding_probe_count(read_forwarding_count,
                                   SchedulerForwardingProbe::InitialHandles),
            1);
  EXPECT_EQ(forwarding_probe_count(read_forwarding_count,
                                   SchedulerForwardingProbe::WorkerHandles),
            1);
  EXPECT_EQ(forwarding_probe_count(read_forwarding_count,
                                   SchedulerForwardingProbe::AnyThreadTask),
            0);

  scheduler.reset();
  loader.clear_plugins();
#ifdef _WIN32
  FreeLibrary(test_handle);
#else
  dlclose(test_handle);
#endif
}

/**
 * @brief Bounds host relay allocations independently of handle-batch size.
 * @return Nothing.
 * @throws Nothing when two 4096-handle batches execute with only constant-count
 * host allocations and registry storage is reusable after each wait.
 * @note This protects the index-slot design from regressing to one heap/control
 * block allocation per task handle.
 */
TEST_F(SchedulerPluginLoaderTest,
       LargeHandleBatchUsesConstantCountRelayAllocationsAndReusesSlots) {
  auto& loader = SchedulerPluginLoader::instance();
  const auto plugin_path =
      scheduler_plugin_path("destroy_count_scheduler_plugin", true);
  ASSERT_TRUE(loader.load_plugin(plugin_path));
  auto scheduler = loader.create(kDestroyCountSchedulerType, 1U);
  ASSERT_NE(scheduler, nullptr);

  constexpr std::size_t kHandleCount = 4096U;
  CountingTaskExecutor executor;
  auto make_handles = [&]() {
    return std::vector<TaskHandle>(kHandleCount, TaskHandle{&executor, 1, 101});
  };

  std::vector<TaskHandle> first_handles = make_handles();
  std::size_t first_allocations = 0U;
  scheduler_allocation_probe::begin_counting();
  try {
    scheduler->submit_initial_task_handles(std::move(first_handles),
                                           static_cast<int>(kHandleCount));
    first_allocations = scheduler_allocation_probe::end_counting();
  } catch (...) {
    (void)scheduler_allocation_probe::end_counting();
    throw;
  }
  scheduler->wait_for_completion();

  std::vector<TaskHandle> second_handles = make_handles();
  std::size_t reused_allocations = 0U;
  scheduler_allocation_probe::begin_counting();
  try {
    scheduler->submit_initial_task_handles(std::move(second_handles),
                                           static_cast<int>(kHandleCount));
    reused_allocations = scheduler_allocation_probe::end_counting();
  } catch (...) {
    (void)scheduler_allocation_probe::end_counting();
    throw;
  }
  scheduler->wait_for_completion();

  EXPECT_LE(first_allocations, 16U);
  EXPECT_LE(reused_allocations, 12U);
  EXPECT_EQ(executor.callback_count(), static_cast<int>(2U * kHandleCount));
  EXPECT_EQ(executor.task_id_sum(), static_cast<int>(2U * kHandleCount));
}

/**
 * @brief Verifies explicit lifecycle calls normalize plugin-origin failures.
 * @throws Nothing when ordinary plugin exceptions become host GraphError while
 *         resource exhaustion remains a fresh host `std::bad_alloc`.
 * @note The failure environment is restored before owner destruction so this
 * test isolates the ordinary caller-visible API path from fallback cleanup.
 */
TEST_F(SchedulerPluginLoaderTest,
       ExplicitLifecycleCallsNormalizePluginExceptions) {
  auto& loader = SchedulerPluginLoader::instance();
  const auto plugin_path =
      scheduler_plugin_path("destroy_count_scheduler_plugin", true);
  ASSERT_TRUE(std::filesystem::exists(plugin_path));
  ASSERT_TRUE(loader.load_plugin(plugin_path));
  auto scheduler = loader.create("destroy_count_test", 1U);
  ASSERT_NE(scheduler, nullptr);
  scheduler->start();

  {
    ScopedSchedulerFixtureEnvironment failures(kDestroyCountFailureEnvironment,
                                               "all");
    try {
      scheduler->shutdown();
      FAIL() << "plugin shutdown failure did not cross the owner boundary";
    } catch (const GraphError& error) {
      EXPECT_EQ(error.code(), GraphErrc::ComputeError);
      EXPECT_STREQ(error.what(), "fixture shutdown failure");
    }
    EXPECT_THROW(scheduler->detach(), std::bad_alloc);
  }

  scheduler.reset();
  loader.clear_plugins();
}

/**
 * @brief Maps a plugin invalid argument to the stable public graph category.
 * @return Nothing.
 * @throws Nothing when the owner returns host-owned InvalidParameter text.
 */
TEST_F(SchedulerPluginLoaderTest,
       OwnerMapsPluginInvalidArgumentToInvalidParameter) {
  auto& loader = SchedulerPluginLoader::instance();
  const auto plugin_path =
      scheduler_plugin_path("destroy_count_scheduler_plugin", true);
  ASSERT_TRUE(loader.load_plugin(plugin_path));
  auto scheduler = loader.create(kDestroyCountSchedulerType, 1U);
  ASSERT_NE(scheduler, nullptr);

  ScopedSchedulerFixtureEnvironment failure(kDestroyCountFailureEnvironment,
                                            "stats_invalid_argument");
  try {
    (void)scheduler->get_stats();
    FAIL() << "fixture invalid argument did not cross the owner boundary";
  } catch (const GraphError& error) {
    EXPECT_EQ(error.code(), GraphErrc::InvalidParameter);
    EXPECT_STREQ(error.what(), "fixture scheduler invalid argument");
  }
}

/**
 * @brief Rejects a DSO-provided device value outside the fixed public enum.
 * @return Nothing.
 * @throws Nothing when host validation returns an owned InvalidParameter error
 * before the unknown value can enter compute planning.
 */
TEST_F(SchedulerPluginLoaderTest,
       OwnerRejectsUnknownPluginDeviceBeforePlanning) {
  auto& loader = SchedulerPluginLoader::instance();
  const auto plugin_path =
      scheduler_plugin_path("destroy_count_scheduler_plugin", true);
  ASSERT_TRUE(loader.load_plugin(plugin_path));
  auto scheduler = loader.create(kDestroyCountSchedulerType, 1U);
  ASSERT_NE(scheduler, nullptr);

  ScopedSchedulerFixtureEnvironment failure(kDestroyCountFailureEnvironment,
                                            "available_devices_invalid");
  try {
    (void)scheduler->available_devices();
    FAIL() << "fixture invalid device entered host planning";
  } catch (const GraphError& error) {
    EXPECT_EQ(error.code(), GraphErrc::InvalidParameter);
    EXPECT_STREQ(error.what(),
                 "Scheduler plugin returned an unknown device value");
  }
}

/**
 * @brief Normalizes a DSO-defined owner exception before final plugin unload.
 * @return Nothing.
 * @throws Nothing when `what()` and the custom exception destructor run before
 *         detach, plugin destroy, and the final library-unload probe.
 * @note Clearing the loader first makes the scheduler owner the sole remaining
 * DSO lease when the custom statistics callback throws.
 */
TEST_F(SchedulerPluginLoaderTest,
       OwnerCustomExceptionIsDestroyedBeforeLibraryUnload) {
  auto& loader = SchedulerPluginLoader::instance();
  const auto plugin_path =
      scheduler_plugin_path("destroy_count_scheduler_plugin", true);
  ASSERT_TRUE(std::filesystem::exists(plugin_path));
  const auto trace_path = scheduler_owner_trace_path("custom-exception");
  std::filesystem::remove(trace_path);

  {
    ScopedSchedulerFixtureEnvironment trace(kDestroyCountTraceEnvironment,
                                            trace_path.string());
    ASSERT_TRUE(loader.load_plugin(plugin_path));
    auto scheduler = loader.create(kDestroyCountSchedulerType, 1U);
    ASSERT_NE(scheduler, nullptr);
    loader.clear_plugins();

    {
      ScopedSchedulerFixtureEnvironment failure(kDestroyCountFailureEnvironment,
                                                "stats_custom_exception");
      try {
        (void)scheduler->get_stats();
        FAIL() << "fixture custom exception did not cross the owner boundary";
      } catch (const GraphError& error) {
        EXPECT_EQ(error.code(), GraphErrc::ComputeError);
        EXPECT_STREQ(error.what(), "fixture scheduler custom exception");
      }
    }

    scheduler.reset();
    EXPECT_EQ(read_scheduler_owner_trace(trace_path),
              (std::vector<std::string>{"custom_exception_what",
                                        "custom_exception_destroy", "detach",
                                        "destroy", "library_unload"}));
  }
  std::filesystem::remove(trace_path);
}

/**
 * @brief Keeps host handle/callback exceptions unchanged inside the plugin.
 * @return Nothing.
 * @throws Nothing when both plugin-side `std::exception` catches observe the
 * host message, the caller receives the same `exception_ptr` identity, and the
 * armed next-allocation failpoint never fires between task throw and host
 * catch.
 * @note A carrier implementation would fail the DSO visibility trace; an
 * allocating relay would replace the marker with `std::bad_alloc`.
 */
TEST_F(SchedulerPluginLoaderTest,
       HostHandleAndCallbackExceptionsStayExactAndAllocationFree) {
  auto& loader = SchedulerPluginLoader::instance();
  const auto plugin_path =
      scheduler_plugin_path("destroy_count_scheduler_plugin", true);
  ASSERT_TRUE(std::filesystem::exists(plugin_path));
  const auto trace_path = scheduler_owner_trace_path("host-task-identity");
  std::filesystem::remove(trace_path);
  ScopedSchedulerFixtureEnvironment trace(kDestroyCountTraceEnvironment,
                                          trace_path.string());
  ASSERT_TRUE(loader.load_plugin(plugin_path));
  auto scheduler = loader.create(kDestroyCountSchedulerType, 1U);
  ASSERT_NE(scheduler, nullptr);

  const std::exception_ptr handle_error =
      std::make_exception_ptr(HostTaskMarkerError{0x39});
  ThrowingTaskExecutor executor(handle_error, true);
  bool handle_caught = false;
  bool handle_identity_matches = false;
  int handle_marker = 0;
  bool handle_allocation_fired = false;
  try {
    std::vector<TaskHandle> handles{TaskHandle{&executor, 1, 101}};
    scheduler->submit_initial_task_handles(std::move(handles), 1);
  } catch (const HostTaskMarkerError& error) {
    handle_caught = true;
    handle_identity_matches = std::current_exception() == handle_error;
    handle_marker = error.marker;
    handle_allocation_fired = scheduler_allocation_probe::did_fire();
    scheduler_allocation_probe::disarm();
  } catch (...) {
    handle_allocation_fired = scheduler_allocation_probe::did_fire();
    scheduler_allocation_probe::disarm();
    throw;
  }
  scheduler_allocation_probe::disarm();

  const std::exception_ptr callback_error =
      std::make_exception_ptr(HostTaskMarkerError{0x3A});
  SchedulerTaskRuntime::Task callback = [callback_error]() {
    scheduler_allocation_probe::arm(0);
    std::rethrow_exception(callback_error);
  };
  bool callback_caught = false;
  bool callback_identity_matches = false;
  int callback_marker = 0;
  bool callback_allocation_fired = false;
  try {
    scheduler->submit_ready_task_any_thread(std::move(callback));
  } catch (const HostTaskMarkerError& error) {
    callback_caught = true;
    callback_identity_matches = std::current_exception() == callback_error;
    callback_marker = error.marker;
    callback_allocation_fired = scheduler_allocation_probe::did_fire();
    scheduler_allocation_probe::disarm();
  } catch (...) {
    callback_allocation_fired = scheduler_allocation_probe::did_fire();
    scheduler_allocation_probe::disarm();
    throw;
  }
  scheduler_allocation_probe::disarm();

  EXPECT_TRUE(handle_caught);
  EXPECT_TRUE(handle_identity_matches);
  EXPECT_EQ(handle_marker, 0x39);
  EXPECT_FALSE(handle_allocation_fired);
  EXPECT_TRUE(callback_caught);
  EXPECT_TRUE(callback_identity_matches);
  EXPECT_EQ(callback_marker, 0x3A);
  EXPECT_FALSE(callback_allocation_fired);
  EXPECT_EQ(read_scheduler_owner_trace(trace_path),
            (std::vector<std::string>{"host_handle_std_exception_visible",
                                      "host_callback_std_exception_visible"}));

  scheduler.reset();
  loader.clear_plugins();
  std::filesystem::remove(trace_path);
}

/**
 * @brief Preserves a host task exception stored and rethrown by the plugin.
 * @return Nothing.
 * @throws Nothing when the exact host-only type and payload survive
 *         `set_exception` plus `wait_for_completion` after loader unload.
 * @note The scheduler DSO cannot construct `HostTaskMarkerError`; observing it
 * proves that the owner distinguished task transport from plugin failures.
 */
TEST_F(SchedulerPluginLoaderTest,
       HostTaskExceptionRemainsExactAcrossPluginWait) {
  auto& loader = SchedulerPluginLoader::instance();
  const auto plugin_path =
      scheduler_plugin_path("destroy_count_scheduler_plugin", true);
  ASSERT_TRUE(std::filesystem::exists(plugin_path));
  const auto trace_path = scheduler_owner_trace_path("host-stored-identity");
  std::filesystem::remove(trace_path);
  ScopedSchedulerFixtureEnvironment trace(kDestroyCountTraceEnvironment,
                                          trace_path.string());
  ASSERT_TRUE(loader.load_plugin(plugin_path));
  auto scheduler = loader.create(kDestroyCountSchedulerType, 1U);
  ASSERT_NE(scheduler, nullptr);

  const std::exception_ptr expected =
      std::make_exception_ptr(HostTaskMarkerError{0x38});
  scheduler->set_exception(expected);
  loader.clear_plugins();
  bool identity_matches = false;
  try {
    scheduler->wait_for_completion();
    FAIL() << "fixture did not rethrow the stored host task exception";
  } catch (const HostTaskMarkerError& error) {
    EXPECT_EQ(error.marker, 0x38);
    identity_matches = std::current_exception() == expected;
  }
  EXPECT_TRUE(identity_matches);
  scheduler.reset();
  EXPECT_EQ(read_scheduler_owner_trace(trace_path),
            (std::vector<std::string>{"host_stored_std_exception_visible",
                                      "detach", "destroy", "library_unload"}));
  std::filesystem::remove(trace_path);
}

/**
 * @brief Destroys retired host exceptions outside the registry spin guard.
 * @return Nothing.
 * @throws Nothing when the final exception destructor reenters `set_exception`
 * and a second wait retires the nested registration without deadlock.
 * @note The fixture ignores both publications, making the host registry the
 * sole owner before each wait-driven clear.
 */
TEST_F(SchedulerPluginLoaderTest,
       HostExceptionDestructorCanReenterRegistryDuringWaitClear) {
  auto& loader = SchedulerPluginLoader::instance();
  const auto plugin_path =
      scheduler_plugin_path("destroy_count_scheduler_plugin", true);
  ASSERT_TRUE(loader.load_plugin(plugin_path));
  auto scheduler = loader.create(kDestroyCountSchedulerType, 1U);
  ASSERT_NE(scheduler, nullptr);

  bool reentered = false;
  std::exception_ptr error = make_reentrant_host_task_error(
      scheduler.get(), &reentered,
      std::make_exception_ptr(std::runtime_error("nested host task error")));
  {
    ScopedSchedulerFixtureEnvironment failure(kDestroyCountFailureEnvironment,
                                              "ignore_host_exception");
    scheduler->set_exception(error);
    error = nullptr;
    scheduler->wait_for_completion();
    EXPECT_TRUE(reentered);
    scheduler->wait_for_completion();
  }
}

/**
 * @brief Proves a failed second attach re-arms destructor detach fallback.
 * @return Nothing.
 * @throws Nothing when the owner normalizes the attach error and later performs
 *         one additional best-effort detach before plugin destroy.
 * @note The fixture publishes its host pointer before throwing, so skipping
 *       fallback would leave plugin state borrowing the test host at destroy.
 */
TEST_F(SchedulerPluginLoaderTest,
       FailedReattachStillReceivesDestructorDetachFallback) {
  auto& loader = SchedulerPluginLoader::instance();
  const auto plugin_path =
      scheduler_plugin_path("destroy_count_scheduler_plugin", true);
  ASSERT_TRUE(std::filesystem::exists(plugin_path));
  const auto trace_path = scheduler_owner_trace_path("failed-reattach");
  std::filesystem::remove(trace_path);
  NoopSchedulerHostContext host;

  {
    ScopedSchedulerFixtureEnvironment trace(kDestroyCountTraceEnvironment,
                                            trace_path.string());
    ScopedSchedulerFixtureEnvironment failures(
        kDestroyCountFailureEnvironment, "attach_runtime_error_on_second");
#ifdef _WIN32
    HMODULE test_handle = LoadLibrary(plugin_path.string().c_str());
    ASSERT_NE(test_handle, nullptr);
    std::shared_ptr<void> test_library(test_handle, [](void* handle) {
      FreeLibrary(static_cast<HMODULE>(handle));
    });
    auto reset_counts = reinterpret_cast<void (*)()>(
        GetProcAddress(test_handle, "ps_test_scheduler_reset_counts"));
#else
    void* test_handle = dlopen(plugin_path.string().c_str(), RTLD_LAZY);
    ASSERT_NE(test_handle, nullptr) << dlerror();
    std::shared_ptr<void> test_library(test_handle,
                                       [](void* handle) { dlclose(handle); });
    auto reset_counts = reinterpret_cast<void (*)()>(
        dlsym(test_handle, "ps_test_scheduler_reset_counts"));
#endif
    ASSERT_NE(reset_counts, nullptr);
    reset_counts();
    ASSERT_TRUE(loader.load_plugin(plugin_path));
    auto scheduler = loader.create(kDestroyCountSchedulerType, 1U);
    ASSERT_NE(scheduler, nullptr);

    scheduler->attach(host);
    scheduler->start();
    scheduler->shutdown();
    scheduler->detach();
    EXPECT_THROW(scheduler->attach(host), GraphError);

    loader.clear_plugins();
    scheduler.reset();
    test_library.reset();
    EXPECT_EQ(
        read_scheduler_owner_trace(trace_path),
        (std::vector<std::string>{"shutdown", "detach", "attach_second_failure",
                                  "detach", "destroy", "library_unload"}));
  }
  std::filesystem::remove(trace_path);
}

/**
 * @brief Proves a failed second start re-arms destructor shutdown fallback.
 * @return Nothing.
 * @throws Nothing when the owner normalizes the restart error and later
 *         performs shutdown and detach before plugin destruction.
 * @note The fixture publishes running state before throwing, modeling a
 *       partially started scheduler that would leak resources if fallback were
 *       still marked complete from the first lifecycle.
 */
TEST_F(SchedulerPluginLoaderTest,
       FailedRestartStillReceivesDestructorShutdownFallback) {
  auto& loader = SchedulerPluginLoader::instance();
  const auto plugin_path =
      scheduler_plugin_path("destroy_count_scheduler_plugin", true);
  ASSERT_TRUE(std::filesystem::exists(plugin_path));
  const auto trace_path = scheduler_owner_trace_path("failed-restart");
  std::filesystem::remove(trace_path);
  NoopSchedulerHostContext host;

  {
    ScopedSchedulerFixtureEnvironment trace(kDestroyCountTraceEnvironment,
                                            trace_path.string());
    ScopedSchedulerFixtureEnvironment failures(kDestroyCountFailureEnvironment,
                                               "start_runtime_error_on_second");
#ifdef _WIN32
    HMODULE test_handle = LoadLibrary(plugin_path.string().c_str());
    ASSERT_NE(test_handle, nullptr);
    std::shared_ptr<void> test_library(test_handle, [](void* handle) {
      FreeLibrary(static_cast<HMODULE>(handle));
    });
    auto reset_counts = reinterpret_cast<void (*)()>(
        GetProcAddress(test_handle, "ps_test_scheduler_reset_counts"));
#else
    void* test_handle = dlopen(plugin_path.string().c_str(), RTLD_LAZY);
    ASSERT_NE(test_handle, nullptr) << dlerror();
    std::shared_ptr<void> test_library(test_handle,
                                       [](void* handle) { dlclose(handle); });
    auto reset_counts = reinterpret_cast<void (*)()>(
        dlsym(test_handle, "ps_test_scheduler_reset_counts"));
#endif
    ASSERT_NE(reset_counts, nullptr);
    reset_counts();
    ASSERT_TRUE(loader.load_plugin(plugin_path));
    auto scheduler = loader.create(kDestroyCountSchedulerType, 1U);
    ASSERT_NE(scheduler, nullptr);

    scheduler->attach(host);
    scheduler->start();
    scheduler->shutdown();
    scheduler->detach();
    scheduler->attach(host);
    EXPECT_THROW(scheduler->start(), GraphError);

    loader.clear_plugins();
    scheduler.reset();
    test_library.reset();
    EXPECT_EQ(read_scheduler_owner_trace(trace_path),
              (std::vector<std::string>{
                  "shutdown", "detach", "start_second_failure", "shutdown",
                  "detach", "destroy", "library_unload"}));
  }
  std::filesystem::remove(trace_path);
}

/**
 * @brief Exercises hostile lifecycle and destroy exceptions during destruction.
 * @throws Nothing when fallback cleanup fences every plugin call independently.
 * @note The real library-unload probe must run only after shutdown, detach, and
 * exactly one destroy call, with no extra test-owned dynamic-library handle.
 */
TEST_F(SchedulerPluginLoaderTest,
       DestructorFallbackFencesEachExceptionAndUnloadsAfterDestroy) {
  auto& loader = SchedulerPluginLoader::instance();
  const auto plugin_path =
      scheduler_plugin_path("destroy_count_scheduler_plugin", true);
  ASSERT_TRUE(std::filesystem::exists(plugin_path));
  const auto trace_path = scheduler_owner_trace_path("fallback-exceptions");
  std::filesystem::remove(trace_path);

  {
    ScopedSchedulerFixtureEnvironment trace(kDestroyCountTraceEnvironment,
                                            trace_path.string());
    ScopedSchedulerFixtureEnvironment failures(kDestroyCountFailureEnvironment,
                                               "all");
    ASSERT_TRUE(loader.load_plugin(plugin_path));
    auto scheduler = loader.create("destroy_count_test", 1U);
    ASSERT_NE(scheduler, nullptr);
    scheduler->start();

    loader.clear_plugins();
    scheduler.reset();

    EXPECT_EQ(read_scheduler_owner_trace(trace_path),
              (std::vector<std::string>{"shutdown", "detach", "destroy",
                                        "library_unload"}));
  }

  std::filesystem::remove(trace_path);
}

/**
 * @brief Keeps the plugin host context valid through runtime destructor detach.
 *
 * The test first stops and restarts the real plugin scheduler to prove ordinary
 * `GraphRuntime::stop()` performs shutdown without detach. It then releases the
 * loader registry so the runtime becomes the final scheduler and DSO owner.
 * Runtime destruction must shut down the restarted scheduler, allow detach to
 * call both public host-context services, destroy the plugin instance, and only
 * then release the dynamic library.
 *
 * @return Nothing.
 * @throws Nothing when stop/restart and the complete destruction order match
 *         the long-lived runtime ownership contract.
 * @note The fixture context probe is enabled only for this test. Its CPU marker
 *       proves `is_device_available()` reached the still-live GraphRuntime,
 *       while the following marker proves `log_event()` returned before the
 *       scheduler cleared its borrowed context.
 */
TEST_F(SchedulerPluginLoaderTest,
       GraphRuntimeDestructorKeepsPluginHostContextAliveUntilDetach) {
  auto& loader = SchedulerPluginLoader::instance();
  const auto plugin_path =
      scheduler_plugin_path("destroy_count_scheduler_plugin", true);
  ASSERT_TRUE(std::filesystem::exists(plugin_path));
  const auto trace_path = scheduler_owner_trace_path("runtime-context-detach");
  const auto runtime_root =
      trace_path.parent_path() / (trace_path.stem().string() + "-runtime");
  std::filesystem::remove(trace_path);
  std::filesystem::remove_all(runtime_root);

  {
    ScopedSchedulerFixtureEnvironment trace(kDestroyCountTraceEnvironment,
                                            trace_path.string());
    ScopedSchedulerFixtureEnvironment context_probe(
        kDestroyCountDetachContextProbeEnvironment, "1");
    ASSERT_TRUE(loader.load_plugin(plugin_path));

    {
      GraphRuntime::Info info{"scheduler_runtime_context_lifetime",
                              runtime_root, "", "", runtime_root / "cache"};
      GraphRuntime runtime(info);
      auto scheduler = loader.create(kDestroyCountSchedulerType, 1U);
      ASSERT_NE(scheduler, nullptr);
      runtime.set_scheduler(ComputeIntent::GlobalHighPrecision,
                            std::move(scheduler));
      loader.clear_plugins();

      runtime.start();
      runtime.stop();
      EXPECT_FALSE(runtime.running());
      IScheduler* retained =
          runtime.get_scheduler(ComputeIntent::GlobalHighPrecision);
      ASSERT_NE(retained, nullptr);
      EXPECT_FALSE(retained->is_running());
      EXPECT_EQ(read_scheduler_owner_trace(trace_path),
                (std::vector<std::string>{"shutdown"}));

      runtime.start();
      EXPECT_TRUE(runtime.running());
      EXPECT_TRUE(retained->is_running());
      EXPECT_EQ(read_scheduler_owner_trace(trace_path),
                (std::vector<std::string>{"shutdown"}));
    }

    EXPECT_EQ(read_scheduler_owner_trace(trace_path),
              (std::vector<std::string>{"shutdown", "shutdown",
                                        "detach_context_cpu_available",
                                        "detach_context_log_event", "detach",
                                        "destroy", "library_unload"}));
  }

  std::filesystem::remove(trace_path);
  std::filesystem::remove_all(runtime_root);
}

/**
 * @brief Combines owner allocation failure with a throwing destroy export.
 * @throws Nothing when the raw guard preserves the original `std::bad_alloc`.
 * @note Active/destroy counts are read while the test-owned library handle is
 * still mapped; destroy must end the raw object exactly once before throwing.
 */
TEST_F(SchedulerPluginLoaderTest,
       OwnerAllocationFailureFencesDestroyAndPreservesBadAlloc) {
  auto& loader = SchedulerPluginLoader::instance();
  const auto plugin_path =
      scheduler_plugin_path("destroy_count_scheduler_plugin", true);
  ASSERT_TRUE(std::filesystem::exists(plugin_path));

#ifdef _WIN32
  HMODULE test_handle = LoadLibrary(plugin_path.string().c_str());
  ASSERT_NE(test_handle, nullptr);
  auto reset_counts = reinterpret_cast<void (*)()>(
      GetProcAddress(test_handle, "ps_test_scheduler_reset_counts"));
  auto active_count = reinterpret_cast<int (*)()>(
      GetProcAddress(test_handle, "ps_test_scheduler_active_count"));
  auto destroy_count = reinterpret_cast<int (*)()>(
      GetProcAddress(test_handle, "ps_test_scheduler_destroy_count"));
#else
  void* test_handle = dlopen(plugin_path.string().c_str(), RTLD_LAZY);
  ASSERT_NE(test_handle, nullptr) << dlerror();
  auto reset_counts = reinterpret_cast<void (*)()>(
      dlsym(test_handle, "ps_test_scheduler_reset_counts"));
  auto active_count = reinterpret_cast<int (*)()>(
      dlsym(test_handle, "ps_test_scheduler_active_count"));
  auto destroy_count = reinterpret_cast<int (*)()>(
      dlsym(test_handle, "ps_test_scheduler_destroy_count"));
#endif
  ASSERT_NE(reset_counts, nullptr);
  ASSERT_NE(active_count, nullptr);
  ASSERT_NE(destroy_count, nullptr);
  ASSERT_TRUE(loader.load_plugin(plugin_path));
  reset_counts();
  const std::string type_name = kLongDestroyCountSchedulerType;
  ScopedSchedulerFixtureEnvironment failures(kDestroyCountFailureEnvironment,
                                             "destroy_runtime_error");

  bool caught_bad_alloc = false;
  bool failpoint_fired = false;
  {
    ScopedSchedulerAllocationFailure failure(0);
    try {
      (void)loader.create(type_name, 1U);
    } catch (const std::bad_alloc&) {
      caught_bad_alloc = true;
    }
    failpoint_fired = scheduler_allocation_probe::did_fire();
  }

  EXPECT_TRUE(failpoint_fired);
  EXPECT_TRUE(caught_bad_alloc);
  EXPECT_EQ(active_count(), 0);
  EXPECT_EQ(destroy_count(), 1);
  loader.clear_plugins();

#ifdef _WIN32
  FreeLibrary(test_handle);
#else
  dlclose(test_handle);
#endif
}

/**
 * @brief Injects failure into host owner allocation after plugin creation.
 * @throws Nothing when the raw guard propagates bad_alloc and destroys once.
 * @note The fixture create export itself performs no heap allocation.
 */
TEST_F(SchedulerPluginLoaderTest,
       OwnerAllocationFailureDestroysRawPluginInstanceExactlyOnce) {
  auto& loader = SchedulerPluginLoader::instance();
  const auto plugin_path =
      scheduler_plugin_path("destroy_count_scheduler_plugin", true);
  ASSERT_TRUE(std::filesystem::exists(plugin_path));

#ifdef _WIN32
  HMODULE test_handle = LoadLibrary(plugin_path.string().c_str());
  ASSERT_NE(test_handle, nullptr);
  auto reset_counts = reinterpret_cast<void (*)()>(
      GetProcAddress(test_handle, "ps_test_scheduler_reset_counts"));
  auto active_count = reinterpret_cast<int (*)()>(
      GetProcAddress(test_handle, "ps_test_scheduler_active_count"));
  auto destroy_count = reinterpret_cast<int (*)()>(
      GetProcAddress(test_handle, "ps_test_scheduler_destroy_count"));
#else
  void* test_handle = dlopen(plugin_path.string().c_str(), RTLD_LAZY);
  ASSERT_NE(test_handle, nullptr) << dlerror();
  auto reset_counts = reinterpret_cast<void (*)()>(
      dlsym(test_handle, "ps_test_scheduler_reset_counts"));
  auto active_count = reinterpret_cast<int (*)()>(
      dlsym(test_handle, "ps_test_scheduler_active_count"));
  auto destroy_count = reinterpret_cast<int (*)()>(
      dlsym(test_handle, "ps_test_scheduler_destroy_count"));
#endif
  ASSERT_NE(reset_counts, nullptr);
  ASSERT_NE(active_count, nullptr);
  ASSERT_NE(destroy_count, nullptr);
  ASSERT_TRUE(loader.load_plugin(plugin_path));
  reset_counts();
  const std::string type_name = kLongDestroyCountSchedulerType;

  bool caught_bad_alloc = false;
  bool failpoint_fired = false;
  {
    ScopedSchedulerAllocationFailure failure(0);
    try {
      (void)loader.create(type_name, 1U);
    } catch (const std::bad_alloc&) {
      caught_bad_alloc = true;
    }
    failpoint_fired = scheduler_allocation_probe::did_fire();
  }

  EXPECT_TRUE(failpoint_fired);
  EXPECT_TRUE(caught_bad_alloc);
  EXPECT_EQ(active_count(), 0);
  EXPECT_EQ(destroy_count(), 1);
  loader.clear_plugins();

#ifdef _WIN32
  FreeLibrary(test_handle);
#else
  dlclose(test_handle);
#endif
}

/**
 * @brief Injects failure into the host owner's long type-name copy.
 * @throws Nothing when the raw guard destroys the instance exactly once.
 * @note Allocation index one follows successful owner storage allocation.
 */
TEST_F(SchedulerPluginLoaderTest,
       TypeNameCopyFailureDestroysRawPluginInstanceExactlyOnce) {
  auto& loader = SchedulerPluginLoader::instance();
  const auto plugin_path =
      scheduler_plugin_path("destroy_count_scheduler_plugin", true);
  ASSERT_TRUE(std::filesystem::exists(plugin_path));

#ifdef _WIN32
  HMODULE test_handle = LoadLibrary(plugin_path.string().c_str());
  ASSERT_NE(test_handle, nullptr);
  auto reset_counts = reinterpret_cast<void (*)()>(
      GetProcAddress(test_handle, "ps_test_scheduler_reset_counts"));
  auto active_count = reinterpret_cast<int (*)()>(
      GetProcAddress(test_handle, "ps_test_scheduler_active_count"));
  auto destroy_count = reinterpret_cast<int (*)()>(
      GetProcAddress(test_handle, "ps_test_scheduler_destroy_count"));
#else
  void* test_handle = dlopen(plugin_path.string().c_str(), RTLD_LAZY);
  ASSERT_NE(test_handle, nullptr) << dlerror();
  auto reset_counts = reinterpret_cast<void (*)()>(
      dlsym(test_handle, "ps_test_scheduler_reset_counts"));
  auto active_count = reinterpret_cast<int (*)()>(
      dlsym(test_handle, "ps_test_scheduler_active_count"));
  auto destroy_count = reinterpret_cast<int (*)()>(
      dlsym(test_handle, "ps_test_scheduler_destroy_count"));
#endif
  ASSERT_NE(reset_counts, nullptr);
  ASSERT_NE(active_count, nullptr);
  ASSERT_NE(destroy_count, nullptr);
  ASSERT_TRUE(loader.load_plugin(plugin_path));
  reset_counts();
  const std::string type_name = kLongDestroyCountSchedulerType;

  bool caught_bad_alloc = false;
  bool failpoint_fired = false;
  {
    ScopedSchedulerAllocationFailure failure(1);
    try {
      (void)loader.create(type_name, 1U);
    } catch (const std::bad_alloc&) {
      caught_bad_alloc = true;
    }
    failpoint_fired = scheduler_allocation_probe::did_fire();
  }

  EXPECT_TRUE(failpoint_fired);
  EXPECT_TRUE(caught_bad_alloc);
  EXPECT_EQ(active_count(), 0);
  EXPECT_EQ(destroy_count(), 1);
  loader.clear_plugins();

#ifdef _WIN32
  FreeLibrary(test_handle);
#else
  dlclose(test_handle);
#endif
}

/**
 * @brief Exhausts every allocation along one post-open plugin load path.
 *
 * Each injected `std::bad_alloc` must leave registered types, metadata,
 * retained handles, and the pre-existing error prefix exactly unchanged. The
 * same candidate is then retried immediately without the failpoint and must
 * commit both real types plus its intentional duplicate-type diagnostic.
 *
 * @return Nothing.
 * @throws Nothing when every allocation failure preserves the strong
 * transaction guarantee and the first non-failing index completes the load.
 * @note Fixture callback counters prove the sweep reaches shadow construction,
 * type/metadata staging, conflict-error staging, and final handle bookkeeping
 * after the candidate library has opened. A fixed upper bound prevents a
 * broken allocation probe from hanging the test.
 */
TEST_F(SchedulerPluginLoaderTest,
       LoadAllocationFailuresPreserveRegistryAndAllowImmediateRetry) {
  auto& loader = SchedulerPluginLoader::instance();
  const auto baseline_plugin_path =
      scheduler_plugin_path("serial_debug_example_plugin");
  const auto candidate_plugin_path =
      scheduler_plugin_path("destroy_count_scheduler_plugin", true);
  const auto missing_plugin_path =
      scheduler_plugin_path("missing_load_transaction_plugin", true);
  ASSERT_TRUE(std::filesystem::exists(baseline_plugin_path));
  ASSERT_TRUE(std::filesystem::exists(candidate_plugin_path));
  ASSERT_FALSE(std::filesystem::exists(missing_plugin_path));
  ASSERT_TRUE(loader.load_plugin(baseline_plugin_path));

#ifdef _WIN32
  HMODULE test_handle = LoadLibrary(candidate_plugin_path.string().c_str());
  ASSERT_NE(test_handle, nullptr);
  auto reset_counts = reinterpret_cast<void (*)()>(
      GetProcAddress(test_handle, "ps_test_scheduler_reset_counts"));
  auto read_load_probe = reinterpret_cast<int (*)(int)>(
      GetProcAddress(test_handle, "ps_test_scheduler_load_probe_count"));
#else
  void* test_handle = dlopen(candidate_plugin_path.string().c_str(), RTLD_LAZY);
  ASSERT_NE(test_handle, nullptr) << dlerror();
  auto reset_counts = reinterpret_cast<void (*)()>(
      dlsym(test_handle, "ps_test_scheduler_reset_counts"));
  auto read_load_probe = reinterpret_cast<int (*)(int)>(
      dlsym(test_handle, "ps_test_scheduler_load_probe_count"));
#endif
  ASSERT_NE(reset_counts, nullptr);
  ASSERT_NE(read_load_probe, nullptr);

  ScopedSchedulerFixtureEnvironment duplicate_type(
      kDestroyCountDuplicateTypeEnvironment, "1");
  const std::string candidate_abs_path =
      std::filesystem::absolute(candidate_plugin_path).string();
  constexpr std::int64_t kMaximumAllocationIndex = 512;
  int injected_failures = 0;
  int failures_after_open = 0;
  int failures_during_shadow_copy = 0;
  int failures_during_type_staging = 0;
  int failures_after_complete_metadata = 0;
  int failures_after_duplicate_callback = 0;
  bool completed_without_failure = false;

  for (std::int64_t allocation_index = 0;
       allocation_index < kMaximumAllocationIndex; ++allocation_index) {
    SCOPED_TRACE(::testing::Message()
                 << "allocation_index=" << allocation_index);
    loader.clear_errors();
    ASSERT_FALSE(loader.load_plugin(missing_plugin_path));
    const SchedulerPluginRegistrySnapshot before =
        capture_plugin_registry(loader);
    ASSERT_EQ(before.load_errors.size(), 1u);
    reset_counts();

    bool load_result = false;
    bool caught_bad_alloc = false;
    bool failpoint_fired = false;
    {
      ScopedSchedulerAllocationFailure failure(allocation_index);
      try {
        load_result = loader.load_plugin(candidate_plugin_path);
      } catch (const std::bad_alloc&) {
        caught_bad_alloc = true;
      }
      failpoint_fired = scheduler_allocation_probe::did_fire();
    }

    const int count_calls =
        load_probe_count(read_load_probe, SchedulerLoadProbe::GetCount);
    const int name_calls =
        load_probe_count(read_load_probe, SchedulerLoadProbe::GetName);
    const int description_calls =
        load_probe_count(read_load_probe, SchedulerLoadProbe::GetDescription);
    const int version_calls =
        load_probe_count(read_load_probe, SchedulerLoadProbe::GetVersion);

    if (failpoint_fired) {
      ++injected_failures;
      failures_after_open += version_calls > 0 ? 1 : 0;
      failures_during_shadow_copy +=
          version_calls > 0 && count_calls == 0 ? 1 : 0;
      failures_during_type_staging +=
          count_calls > 0 && name_calls > 0 && description_calls < 2 ? 1 : 0;
      failures_after_complete_metadata += description_calls >= 2 ? 1 : 0;
      failures_after_duplicate_callback += name_calls >= 3 ? 1 : 0;

      EXPECT_TRUE(caught_bad_alloc);
      EXPECT_FALSE(load_result);
      expect_plugin_registry_equal(before, capture_plugin_registry(loader));

      ASSERT_TRUE(loader.load_plugin(candidate_plugin_path))
          << "same-path retry failed after allocation index "
          << allocation_index;
    } else {
      EXPECT_FALSE(caught_bad_alloc);
      ASSERT_TRUE(load_result);
      completed_without_failure = true;
    }

    const SchedulerPluginRegistrySnapshot committed =
        capture_plugin_registry(loader);
    EXPECT_EQ(committed.registered_types.size(),
              before.registered_types.size() + 2);
    EXPECT_TRUE(
        contains_type(committed.registered_types, kDestroyCountSchedulerType));
    EXPECT_TRUE(contains_type(committed.registered_types,
                              kLongDestroyCountSchedulerType));
    ASSERT_EQ(committed.loaded_plugins.size(),
              before.loaded_plugins.size() + 1);
    EXPECT_TRUE(std::any_of(
        committed.loaded_plugins.begin(), committed.loaded_plugins.end(),
        [&](const std::string& plugin) {
          return plugin.find(candidate_abs_path) != std::string::npos;
        }));
    ASSERT_EQ(committed.load_errors.size(), before.load_errors.size() + 1);
    EXPECT_TRUE(std::equal(before.load_errors.begin(), before.load_errors.end(),
                           committed.load_errors.begin()));
    EXPECT_NE(committed.load_errors.back().find("already registered by plugin"),
              std::string::npos);

    const auto short_info = loader.get_info(kDestroyCountSchedulerType);
    const auto long_info = loader.get_info(kLongDestroyCountSchedulerType);
    ASSERT_TRUE(short_info.has_value());
    ASSERT_TRUE(long_info.has_value());
    EXPECT_EQ(short_info->plugin_path, candidate_abs_path);
    EXPECT_EQ(long_info->plugin_path, candidate_abs_path);
    EXPECT_EQ(short_info->version, "test");
    EXPECT_EQ(long_info->version, "test");

    ASSERT_TRUE(loader.unload_plugin(candidate_plugin_path));
    SchedulerPluginRegistrySnapshot expected_after_unload = before;
    expected_after_unload.load_errors = committed.load_errors;
    expect_plugin_registry_equal(expected_after_unload,
                                 capture_plugin_registry(loader));

    if (completed_without_failure) {
      break;
    }
  }

  EXPECT_TRUE(completed_without_failure);
  EXPECT_GT(injected_failures, 0);
  EXPECT_GT(failures_after_open, 0);
  EXPECT_GT(failures_during_shadow_copy, 0);
  EXPECT_GT(failures_during_type_staging, 0);
  EXPECT_GT(failures_after_complete_metadata, 0);
  EXPECT_GT(failures_after_duplicate_callback, 0);

  loader.clear_plugins();
  loader.clear_errors();
#ifdef _WIN32
  FreeLibrary(test_handle);
#else
  dlclose(test_handle);
#endif
}

/**
 * @brief Rejects compatible scheduler DSOs without a publishable type identity.
 *
 * The real fixture reports zero types, a null or empty second in-range name,
 * or one type already owned by the baseline plugin. Each rejection must discard
 * partial shadow metadata, preserve the prior registry and diagnostic prefix,
 * release the candidate handle, and permit an immediate ordinary retry.
 *
 * @return Nothing.
 * @throws Nothing when every invalid discovery shape fails transactionally and
 * the same candidate path remains reusable.
 * @note A separate duplicate fixture path already proves that conflicts remain
 * recoverable when at least one valid non-conflicting type can be published.
 */
TEST_F(SchedulerPluginLoaderTest,
       InvalidTypeIdentityRejectsCandidateAndAllowsImmediateRetry) {
  auto& loader = SchedulerPluginLoader::instance();
  const auto baseline_plugin_path =
      scheduler_plugin_path("serial_debug_example_plugin");
  const auto candidate_plugin_path =
      scheduler_plugin_path("destroy_count_scheduler_plugin", true);
  ASSERT_TRUE(std::filesystem::exists(baseline_plugin_path));
  ASSERT_TRUE(std::filesystem::exists(candidate_plugin_path));
  ASSERT_TRUE(loader.load_plugin(baseline_plugin_path));

  const std::array<std::pair<const char*, const char*>, 4> rejection_cases{{
      {"count_zero", "reported zero scheduler types"},
      {"name_null", "invalid scheduler type name at index 1"},
      {"name_empty", "invalid scheduler type name at index 1"},
      {"all_conflicting_types", "no non-conflicting scheduler types"},
  }};

  for (const auto& [mode, expected_diagnostic] : rejection_cases) {
    SCOPED_TRACE(::testing::Message() << "mode=" << mode);
    const SchedulerPluginRegistrySnapshot before =
        capture_plugin_registry(loader);

    {
      ScopedSchedulerFixtureEnvironment rejection(
          kDestroyCountLoadFailureEnvironment, mode);
      EXPECT_FALSE(loader.load_plugin(candidate_plugin_path));
    }

    const SchedulerPluginRegistrySnapshot rejected =
        capture_plugin_registry(loader);
    SchedulerPluginRegistrySnapshot expected_rejected = before;
    expected_rejected.load_errors = rejected.load_errors;
    expect_plugin_registry_equal(expected_rejected, rejected);
    ASSERT_EQ(rejected.load_errors.size(), before.load_errors.size() + 1U);
    EXPECT_TRUE(std::equal(before.load_errors.begin(), before.load_errors.end(),
                           rejected.load_errors.begin()));
    EXPECT_NE(rejected.load_errors.back().find(expected_diagnostic),
              std::string::npos);
    EXPECT_FALSE(loader.is_registered(kDestroyCountSchedulerType));
    EXPECT_FALSE(loader.is_registered(kLongDestroyCountSchedulerType));

    ASSERT_TRUE(loader.load_plugin(candidate_plugin_path));
    EXPECT_TRUE(loader.is_registered(kDestroyCountSchedulerType));
    EXPECT_TRUE(loader.is_registered(kLongDestroyCountSchedulerType));
    ASSERT_TRUE(loader.unload_plugin(candidate_plugin_path));

    SchedulerPluginRegistrySnapshot expected_after_retry = before;
    expected_after_retry.load_errors = rejected.load_errors;
    expect_plugin_registry_equal(expected_after_retry,
                                 capture_plugin_registry(loader));
  }
}

/**
 * @brief A discovery callback exception rolls back partial candidate metadata.
 * @return Nothing.
 * @throws Nothing when a host-owned GraphError carries the callback diagnostic,
 * the live registry remains exact, and the same path succeeds immediately
 * afterward.
 * @note The fixture throws from the second description callback, after its
 * first type has been fully staged but before any shadow state is committed.
 */
TEST_F(SchedulerPluginLoaderTest,
       LoadCallbackExceptionPreservesRegistryAndAllowsImmediateRetry) {
  auto& loader = SchedulerPluginLoader::instance();
  const auto baseline_plugin_path =
      scheduler_plugin_path("serial_debug_example_plugin");
  const auto candidate_plugin_path =
      scheduler_plugin_path("destroy_count_scheduler_plugin", true);
  const auto missing_plugin_path =
      scheduler_plugin_path("missing_callback_transaction_plugin", true);
  ASSERT_TRUE(std::filesystem::exists(baseline_plugin_path));
  ASSERT_TRUE(std::filesystem::exists(candidate_plugin_path));
  ASSERT_FALSE(std::filesystem::exists(missing_plugin_path));
  ASSERT_TRUE(loader.load_plugin(baseline_plugin_path));
  ASSERT_FALSE(loader.load_plugin(missing_plugin_path));
  const SchedulerPluginRegistrySnapshot before =
      capture_plugin_registry(loader);

  {
    ScopedSchedulerFixtureEnvironment failure(
        kDestroyCountLoadFailureEnvironment, "description_runtime_error");
    try {
      (void)loader.load_plugin(candidate_plugin_path);
      FAIL() << "fixture metadata callback failure did not propagate";
    } catch (const GraphError& error) {
      EXPECT_EQ(error.code(), GraphErrc::ComputeError);
      EXPECT_STREQ(error.what(), "fixture scheduler description failure");
    }
  }

  expect_plugin_registry_equal(before, capture_plugin_registry(loader));
  EXPECT_FALSE(loader.is_registered(kDestroyCountSchedulerType));
  EXPECT_FALSE(loader.is_registered(kLongDestroyCountSchedulerType));

  ASSERT_TRUE(loader.load_plugin(candidate_plugin_path));
  EXPECT_TRUE(loader.is_registered(kDestroyCountSchedulerType));
  EXPECT_TRUE(loader.is_registered(kLongDestroyCountSchedulerType));
  ASSERT_TRUE(loader.unload_plugin(candidate_plugin_path));
  expect_plugin_registry_equal(before, capture_plugin_registry(loader));
  loader.clear_plugins();
  loader.clear_errors();
}

/**
 * @brief Destroys a DSO-defined discovery exception before candidate unload.
 * @return Nothing.
 * @throws Nothing when normalization preserves registry state and the trace
 *         orders `what`, custom destruction, then final DSO teardown.
 * @note No test-owned native handle is opened, so the failed candidate's
 * transaction lease is the only reference capable of delaying unload.
 */
TEST_F(SchedulerPluginLoaderTest,
       DiscoveryCustomExceptionIsDestroyedBeforeCandidateUnload) {
  auto& loader = SchedulerPluginLoader::instance();
  const auto plugin_path =
      scheduler_plugin_path("destroy_count_scheduler_plugin", true);
  ASSERT_TRUE(std::filesystem::exists(plugin_path));
  const auto trace_path = scheduler_owner_trace_path("discovery-exception");
  std::filesystem::remove(trace_path);
  const SchedulerPluginRegistrySnapshot before =
      capture_plugin_registry(loader);

  {
    ScopedSchedulerFixtureEnvironment trace(kDestroyCountTraceEnvironment,
                                            trace_path.string());
    ScopedSchedulerFixtureEnvironment failure(
        kDestroyCountLoadFailureEnvironment, "description_custom_exception");
    try {
      (void)loader.load_plugin(plugin_path);
      FAIL() << "fixture custom discovery exception did not propagate";
    } catch (const GraphError& error) {
      EXPECT_EQ(error.code(), GraphErrc::ComputeError);
      EXPECT_STREQ(error.what(), "fixture scheduler custom exception");
    }
    EXPECT_EQ(read_scheduler_owner_trace(trace_path),
              (std::vector<std::string>{"custom_exception_what",
                                        "custom_exception_destroy",
                                        "library_unload"}));
  }

  expect_plugin_registry_equal(before, capture_plugin_registry(loader));
  EXPECT_FALSE(loader.is_registered(kDestroyCountSchedulerType));
  ASSERT_TRUE(loader.load_plugin(plugin_path));
  ASSERT_TRUE(loader.unload_plugin(plugin_path));
  expect_plugin_registry_equal(before, capture_plugin_registry(loader));
  std::filesystem::remove(trace_path);
}

}  // namespace ps
