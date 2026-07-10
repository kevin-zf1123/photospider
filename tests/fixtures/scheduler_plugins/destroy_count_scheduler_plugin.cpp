#include <array>
#include <atomic>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <new>
#include <optional>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "kernel/scheduler/scheduler_plugin_api.hpp"
#include "kernel/scheduler/scheduler_task_runtime.hpp"

namespace {

/** @brief Short plugin scheduler name used by the ordinary lifecycle case. */
constexpr const char* kShortSchedulerType = "destroy_count_test";
/** @brief Name long enough to force host-owned `std::string` allocation. */
constexpr const char* kLongSchedulerType =                             // NOLINT
    "destroy_count_scheduler_type_long_enough_to_force_owned_string_"  // NOLINT
    "storage";                                                         // NOLINT
/** @brief Environment key selecting a newline-delimited lifecycle trace. */
constexpr const char* kTraceEnvironment = "PS_DESTROY_COUNT_SCHEDULER_TRACE";
/** @brief Environment key selecting one or all fixture exception modes. */
constexpr const char* kFailureEnvironment =
    "PS_DESTROY_COUNT_SCHEDULER_FAILURE";  // NOLINT
/** @brief Environment key enabling one duplicate discovery type entry. */
constexpr const char* kDuplicateTypeEnvironment =
    "PS_DESTROY_COUNT_SCHEDULER_DUPLICATE_TYPE";  // NOLINT
/** @brief Environment key selecting a discovery-callback exception. */
constexpr const char* kLoadFailureEnvironment =
    "PS_DESTROY_COUNT_SCHEDULER_LOAD_FAILURE";  // NOLINT

/** @brief Number of placement-created fixture instances still alive. */
std::atomic<int> g_active_count{0};
/** @brief Number of calls that reached the plugin destroy export. */
std::atomic<int> g_destroy_count{0};

/**
 * @brief Stable counter slots for host-owner runtime forwarding probes.
 * @note The numeric order is mirrored by `test_scheduler_plugin_loader` through
 * the fixture-only `ps_test_scheduler_forwarding_count` export.
 */
enum class ForwardingProbe : std::size_t {
  /** @brief Calls to the plugin runtime's device inventory override. */
  AvailableDevices = 0,
  /** @brief Calls to the plugin runtime's initial handle-batch override. */
  InitialHandles,
  /** @brief Calls to the plugin runtime's single worker-handle override. */
  WorkerHandle,
  /** @brief Calls to the plugin runtime's worker handle-batch override. */
  WorkerHandles,
  /** @brief Calls to the plugin runtime's single any-thread handle override. */
  AnyThreadHandle,
  /** @brief Calls to the plugin runtime's any-thread handle-batch override. */
  AnyThreadHandles,
  /** @brief Calls to closure APIs used only by an incorrect host fallback. */
  ClosureFallback,
  /** @brief Number of valid fixture forwarding counters. */
  Count,
};

/** @brief Number of allocation-free forwarding probe counters. */
constexpr auto kProbeCount = static_cast<std::size_t>(ForwardingProbe::Count);
/** @brief Fixed allocation-free counter storage for forwarding probes. */
using ForwardingCounters = std::array<std::atomic<int>, kProbeCount>;
/** @brief Allocation-free counters for every forwarding probe slot. */
ForwardingCounters g_forwarding_counts = {0, 0, 0, 0, 0, 0, 0};

/**
 * @brief Stable counter slots for scheduler-plugin discovery callbacks.
 * @note The numeric order is mirrored by the loader transaction regression
 * through `ps_test_scheduler_load_probe_count`.
 */
enum class LoadProbe : std::size_t {
  /** @brief Calls to the type-count export. */
  GetCount = 0,
  /** @brief Calls to the indexed type-name export. */
  GetName,
  /** @brief Calls to the optional indexed description export. */
  GetDescription,
  /** @brief Calls to the optional version export. */
  GetVersion,
  /** @brief Number of valid discovery callback counters. */
  Count,
};

/** @brief Number of allocation-free discovery callback counters. */
constexpr auto kLoadProbeCount = static_cast<std::size_t>(LoadProbe::Count);
/** @brief Fixed counter storage for discovery callback probes. */
using LoadProbeCounters = std::array<std::atomic<int>, kLoadProbeCount>;
/** @brief Allocation-free counters for every discovery callback slot. */
LoadProbeCounters g_load_probe_counts = {0, 0, 0, 0};

/**
 * @brief Records one plugin runtime forwarding call.
 * @param probe Stable fixture slot associated with the called virtual method.
 * @return Nothing.
 * @throws Nothing.
 * @note Relaxed ordering is sufficient because tests invoke this synchronous
 * fixture on one thread and read counts only after each call returns.
 */
void record_forwarding_probe(ForwardingProbe probe) noexcept {
  g_forwarding_counts[static_cast<std::size_t>(probe)].fetch_add(
      1, std::memory_order_relaxed);
}

/**
 * @brief Records one scheduler-plugin discovery callback.
 * @param probe Stable fixture slot associated with the invoked ABI export.
 * @return Nothing.
 * @throws Nothing.
 * @note Relaxed atomics keep the probe allocation-free while the loader's
 * deterministic `operator new` failpoint is armed.
 */
void record_load_probe(LoadProbe probe) noexcept {
  g_load_probe_counts[static_cast<std::size_t>(probe)].fetch_add(
      1, std::memory_order_relaxed);
}

/**
 * @brief Appends one real scheduler/plugin lifetime event to the selected file.
 *
 * @param event Stable event label owned by the caller.
 * @return Nothing.
 * @throws Nothing; absent configuration and file failures are ignored.
 * @note C stdio keeps this probe usable from exception and library-unload
 * cleanup paths without introducing C++ allocation dependencies.
 */
void append_lifecycle_trace(const char* event) noexcept {
  const char* path = std::getenv(kTraceEnvironment);
  if (!path || path[0] == '\0') {
    return;
  }
  std::FILE* output = std::fopen(path, "a");
  if (!output) {
    return;
  }
  (void)std::fputs(event, output);
  (void)std::fputc('\n', output);
  (void)std::fclose(output);
}

/**
 * @brief Checks whether one deterministic fixture failure is enabled.
 *
 * @param mode Exact mode name to match; `all` enables every mode.
 * @return True when the process environment selects `mode` or `all`.
 * @throws Nothing.
 * @note String comparison is allocation-free so `bad_alloc` tests exercise the
 * host owner rather than fixture configuration.
 */
bool failure_enabled(const char* mode) noexcept {
  const char* configured = std::getenv(kFailureEnvironment);
  return configured && (std::strcmp(configured, mode) == 0 ||
                        std::strcmp(configured, "all") == 0);
}

/**
 * @brief Reports whether discovery should expose a duplicate third type.
 * @return True when the dedicated environment value is non-empty and not zero.
 * @throws Nothing.
 * @note The extra entry duplicates `kShortSchedulerType`, forcing host-side
 * conflict diagnostic staging without changing ordinary fixture behavior.
 */
bool duplicate_type_enabled() noexcept {
  const char* configured = std::getenv(kDuplicateTypeEnvironment);
  return configured && configured[0] != '\0' && configured[0] != '0';
}

/**
 * @brief Checks whether one discovery-callback failure mode is enabled.
 * @param mode Exact load-failure mode name to match.
 * @return True when the dedicated load-failure environment selects `mode`.
 * @throws Nothing.
 * @note This key is separate from lifecycle failure injection so existing
 * destructor tests may continue using their `all` lifecycle mode while loading.
 */
bool load_failure_enabled(const char* mode) noexcept {
  const char* configured = std::getenv(kLoadFailureEnvironment);
  return configured && std::strcmp(configured, mode) == 0;
}

/**
 * @brief Records actual dynamic-library teardown for ownership-order tests.
 *
 * @note The global instance emits after the final host/test handle is released.
 * A valid owner must therefore emit `destroy` before `library_unload`.
 */
struct LibraryLifetimeProbe {
  /**
   * @brief Records library teardown without surfacing trace I/O failures.
   * @throws Nothing.
   */
  ~LibraryLifetimeProbe() noexcept { append_lifecycle_trace("library_unload"); }
};

/** @brief Process-per-load probe whose destructor observes final handle
 * release. */
LibraryLifetimeProbe g_library_lifetime_probe;

/**
 * @brief Allocation-free scheduler fixture for host-owner failure injection.
 *
 * The implementation executes submitted tasks synchronously, publishes a
 * distinctive device list, and counts every native TaskHandle overload. Its
 * batch overrides can throw before callback execution, while its
 * constructor/destructor maintain the active-instance count used to prove that
 * host allocation failure cannot leak a raw plugin scheduler.
 *
 * @note Instances live in `g_scheduler_storage`; callers must pair successful
 * placement construction with `ps_scheduler_plugin_destroy` exactly once.
 */
class DestroyCountScheduler final : public ps::IScheduler,
                                    public ps::SchedulerTaskRuntime {
 public:
  /** @brief Marks one fixture instance active without allocating. */
  DestroyCountScheduler() { g_active_count.fetch_add(1); }
  /** @brief Marks the placement-owned fixture instance inactive. */
  ~DestroyCountScheduler() noexcept override { g_active_count.fetch_sub(1); }

  /**
   * @brief Accepts but does not retain a graph runtime.
   * @param runtime Borrowed runtime ignored by this synchronous fixture.
   * @return Nothing.
   * @throws Nothing.
   */
  void attach(ps::GraphRuntime* runtime) override { (void)runtime; }
  /**
   * @brief Detaches the ignored runtime and optionally injects resource
   * failure.
   * @return Nothing.
   * @throws std::bad_alloc when the `detach_bad_alloc` or `all` mode is active.
   * @note The trace is emitted before the configured exception.
   */
  void detach() override {
    append_lifecycle_trace("detach");
    if (failure_enabled("detach_bad_alloc")) {
      throw std::bad_alloc{};
    }
  }
  /** @brief Marks the fixture running; returns nothing and does not throw. */
  void start() override { running_ = true; }
  /**
   * @brief Stops the fixture and optionally injects an ordinary failure.
   * @return Nothing.
   * @throws std::runtime_error when `shutdown_runtime_error` or `all` is
   * active.
   * @note The state transition and trace precede the configured exception.
   */
  void shutdown() override {
    running_ = false;
    append_lifecycle_trace("shutdown");
    if (failure_enabled("shutdown_runtime_error")) {
      throw std::runtime_error("fixture shutdown failure");
    }
  }

  /**
   * @brief Returns the stable short fixture name.
   * @return Owned fixture name.
   * @throws std::bad_alloc if string construction cannot allocate.
   */
  std::string name() const override { return "destroy_count_test"; }
  /**
   * @brief Returns the stable fixture statistic string.
   * @return Owned statistic marker.
   * @throws std::bad_alloc if string construction cannot allocate.
   */
  std::string get_stats() const override { return "destroy-count-test"; }
  /** @brief Returns whether `start()` has not been balanced by shutdown. */
  bool is_running() const override { return running_; }
  /** @brief Mirrors `is_running()` for the task-runtime interface. */
  bool task_runtime_running() const override { return running_; }

  /**
   * @brief Reports a distinctive CPU/Metal inventory for owner forwarding.
   * @return CPU followed by Metal GPU.
   * @throws std::bad_alloc if vector result storage cannot allocate.
   * @note The distinctive two-device result detects an owner that accidentally
   * uses `SchedulerTaskRuntime`'s CPU-only default implementation.
   */
  std::vector<ps::Device> available_devices() const override {
    record_forwarding_probe(ForwardingProbe::AvailableDevices);
    return {ps::Device::CPU, ps::Device::GPU_METAL};
  }

  /**
   * @brief Executes each non-empty initial task synchronously.
   * @param tasks Tasks consumed in input order.
   * @param total_task_count Completion count recorded by the fixture.
   * @param priority Ignored scheduling hint.
   * @return Nothing.
   * @throws Any exception produced by a submitted task unchanged.
   */
  void submit_initial_tasks(std::vector<Task>&& tasks, int total_task_count,
                            ps::SchedulerTaskPriority priority =
                                ps::SchedulerTaskPriority::Normal) override {
    record_forwarding_probe(ForwardingProbe::ClosureFallback);
    (void)priority;
    tasks_to_complete_ = total_task_count;
    for (auto& task : tasks) {
      if (task) {
        task();
      }
    }
  }

  /**
   * @brief Executes one initial borrowed-handle batch through the native API.
   * @param handles Ready handles consumed synchronously in input order.
   * @param total_task_count Completion count recorded after failure injection.
   * @param priority Ignored scheduling hint.
   * @return Nothing.
   * @throws std::bad_alloc before any callback when the
   * `handle_batch_bad_alloc` mode is active.
   * @throws Any task-executor exception unchanged.
   * @note The call counter advances before failure so tests prove the host
   * reached this override rather than a base closure fallback.
   */
  void submit_initial_task_handles(
      std::vector<ps::TaskHandle>&& handles, int total_task_count,
      ps::SchedulerTaskPriority priority =
          ps::SchedulerTaskPriority::Normal) override {
    record_forwarding_probe(ForwardingProbe::InitialHandles);
    (void)priority;
    if (failure_enabled("handle_batch_bad_alloc")) {
      throw std::bad_alloc{};
    }
    tasks_to_complete_ = total_task_count;
    for (const auto& handle : handles) {
      if (handle) {
        handle.run();
      }
    }
  }

  /**
   * @brief Executes one worker-origin task through the common synchronous path.
   * @param task Task to consume.
   * @param priority Ignored scheduling hint.
   * @return Nothing.
   * @throws Any exception produced by `task` unchanged.
   */
  void submit_ready_task_from_worker(
      Task&& task, ps::SchedulerTaskPriority priority =
                       ps::SchedulerTaskPriority::Normal) override {
    record_forwarding_probe(ForwardingProbe::ClosureFallback);
    submit_ready_task_any_thread(std::move(task), priority, std::nullopt);
  }

  /**
   * @brief Executes one worker-origin borrowed handle through its native API.
   * @param handle Ready handle consumed synchronously when non-empty.
   * @param priority Ignored scheduling hint.
   * @return Nothing.
   * @throws Any task-executor exception unchanged.
   * @note The dedicated counter distinguishes this override from closure
   * conversion in the host owner.
   */
  void submit_ready_task_handle_from_worker(
      ps::TaskHandle handle, ps::SchedulerTaskPriority priority =
                                 ps::SchedulerTaskPriority::Normal) override {
    record_forwarding_probe(ForwardingProbe::WorkerHandle);
    (void)priority;
    if (handle) {
      handle.run();
    }
  }

  /**
   * @brief Executes one worker-origin borrowed-handle batch through its native
   * override.
   * @param handles Ready handles consumed synchronously in input order.
   * @param priority Ignored scheduling hint.
   * @return Nothing.
   * @throws std::bad_alloc before any callback when the
   * `handle_batch_bad_alloc` mode is active.
   * @throws Any task-executor exception unchanged.
   * @note Failure precedes handle iteration so zero callbacks are observable.
   */
  void submit_ready_task_handles_from_worker(
      std::vector<ps::TaskHandle>&& handles,
      ps::SchedulerTaskPriority priority =
          ps::SchedulerTaskPriority::Normal) override {
    record_forwarding_probe(ForwardingProbe::WorkerHandles);
    (void)priority;
    if (failure_enabled("handle_batch_bad_alloc")) {
      throw std::bad_alloc{};
    }
    for (const auto& handle : handles) {
      if (handle) {
        handle.run();
      }
    }
  }

  /**
   * @brief Executes one ready task synchronously on the caller.
   * @param task Task to consume when non-empty.
   * @param priority Ignored scheduling hint.
   * @param epoch Ignored batch epoch.
   * @return Nothing.
   * @throws Any exception produced by `task` unchanged.
   */
  void submit_ready_task_any_thread(
      Task&& task,
      ps::SchedulerTaskPriority priority = ps::SchedulerTaskPriority::Normal,
      std::optional<uint64_t> epoch = std::nullopt) override {
    record_forwarding_probe(ForwardingProbe::ClosureFallback);
    (void)priority;
    (void)epoch;
    if (task) {
      task();
    }
  }

  /**
   * @brief Executes one caller-thread borrowed handle through its native API.
   * @param handle Ready handle consumed synchronously when non-empty.
   * @param priority Ignored scheduling hint.
   * @param epoch Ignored batch epoch.
   * @return Nothing.
   * @throws Any task-executor exception unchanged.
   */
  void submit_ready_task_handle_any_thread(
      ps::TaskHandle handle,
      ps::SchedulerTaskPriority priority = ps::SchedulerTaskPriority::Normal,
      std::optional<uint64_t> epoch = std::nullopt) override {
    record_forwarding_probe(ForwardingProbe::AnyThreadHandle);
    (void)priority;
    (void)epoch;
    if (handle) {
      handle.run();
    }
  }

  /**
   * @brief Executes one caller-thread borrowed-handle batch through its native
   * override.
   * @param handles Ready handles consumed synchronously in input order.
   * @param priority Ignored scheduling hint.
   * @param epoch Ignored batch epoch.
   * @return Nothing.
   * @throws std::bad_alloc before any callback when the
   * `handle_batch_bad_alloc` mode is active.
   * @throws Any task-executor exception unchanged.
   * @note Failure precedes handle iteration so zero callbacks are observable.
   */
  void submit_ready_task_handles_any_thread(
      std::vector<ps::TaskHandle>&& handles,
      ps::SchedulerTaskPriority priority = ps::SchedulerTaskPriority::Normal,
      std::optional<uint64_t> epoch = std::nullopt) override {
    record_forwarding_probe(ForwardingProbe::AnyThreadHandles);
    (void)priority;
    (void)epoch;
    if (failure_enabled("handle_batch_bad_alloc")) {
      throw std::bad_alloc{};
    }
    for (const auto& handle : handles) {
      if (handle) {
        handle.run();
      }
    }
  }

  /** @brief Has no asynchronous work to await; returns without throwing. */
  void wait_for_completion() override {}
  /**
   * @brief Retains an exception pointer for fixture completeness.
   * @param e Exception identity supplied by the host.
   * @return Nothing.
   * @throws Nothing.
   */
  void set_exception(std::exception_ptr e) override { exception_ = e; }
  /**
   * @brief Increases the synchronous fixture completion counter.
   * @param delta Value to add.
   * @return Nothing.
   * @throws Nothing.
   */
  void inc_tasks_to_complete(int delta) override {
    tasks_to_complete_ += delta;
  }
  /** @brief Decrements a positive completion counter without throwing. */
  void dec_tasks_to_complete() override {
    if (tasks_to_complete_ > 0) {
      --tasks_to_complete_;
    }
  }
  /**
   * @brief Ignores trace events because this ownership fixture has no runtime.
   * @param action Scheduler event kind.
   * @param node_id Associated node identifier.
   * @return Nothing.
   * @throws Nothing.
   */
  void log_event(ps::SchedulerTraceAction action, int node_id) override {
    (void)action;
    (void)node_id;
  }

 private:
  /** @brief Local lifecycle flag exposed through both running queries. */
  bool running_ = false;
  /** @brief Synchronous completion counter retained for interface fidelity. */
  int tasks_to_complete_ = 0;
  /** @brief Last exception supplied through the runtime interface. */
  std::exception_ptr exception_;
};

/** @brief Allocation-free storage used to isolate host owner allocations. */
std::aligned_storage_t<sizeof(DestroyCountScheduler),   // NOLINT
                       alignof(DestroyCountScheduler)>  // NOLINT
    g_scheduler_storage;                                // NOLINT

}  // namespace

extern "C" {

/**
 * @brief Reports ordinary types plus an optional duplicate conflict entry.
 * @return Two scheduler types normally, or three while the duplicate-type
 * fixture mode is enabled.
 * @throws Nothing.
 */
int ps_scheduler_plugin_get_count() {
  record_load_probe(LoadProbe::GetCount);
  return duplicate_type_enabled() ? 3 : 2;
}

/**
 * @brief Returns a stable scheduler name for one fixture entry.
 * @param index Zero-based fixture entry.
 * @return Process-lifetime name, or nullptr when `index` is out of range.
 * @throws Nothing.
 */
const char* ps_scheduler_plugin_get_name(int index) {
  record_load_probe(LoadProbe::GetName);
  if (index == 0) {
    return kShortSchedulerType;
  }
  if (index == 1) {
    return kLongSchedulerType;
  }
  return index == 2 && duplicate_type_enabled() ? kShortSchedulerType : nullptr;
}

/**
 * @brief Returns the shared lifecycle-fixture description.
 * @param index Zero-based fixture entry.
 * @return Process-lifetime description, or nullptr when out of range.
 * @throws std::runtime_error for the second ordinary type when the
 * `description_runtime_error` load-failure mode is enabled.
 * @note The exception occurs after the first type has been fully staged, which
 * exercises rollback of partial candidate metadata.
 */
const char* ps_scheduler_plugin_get_description(int index) {
  record_load_probe(LoadProbe::GetDescription);
  if (index == 1 && load_failure_enabled("description_runtime_error")) {
    throw std::runtime_error("fixture scheduler description failure");
  }
  return index >= 0 && index < 2 ? "Destroy-count scheduler lifecycle test"
                                 : nullptr;
}

/**
 * @brief Placement-constructs one allocation-free scheduler instance.
 * @param type_name Requested fixture type name.
 * @param worker_count Ignored worker count from the plugin ABI.
 * @return Raw plugin scheduler, or nullptr for an unknown type or while the
 * single fixture storage slot is occupied.
 * @throws Nothing.
 * @note The host must destroy a non-null result through
 * `ps_scheduler_plugin_destroy`; this function performs no heap allocation.
 */
ps::IScheduler* ps_scheduler_plugin_create(const char* type_name,
                                           unsigned int worker_count) {
  (void)worker_count;
  if (!type_name || (std::strcmp(type_name, kShortSchedulerType) != 0 &&
                     std::strcmp(type_name, kLongSchedulerType) != 0)) {
    return nullptr;
  }
  if (g_active_count.load() != 0) {
    return nullptr;
  }
  return new (&g_scheduler_storage) DestroyCountScheduler();
}

/**
 * @brief Destroys a placement-owned fixture scheduler exactly once.
 * @param scheduler Non-null pointer returned by the matching create export.
 * @return Nothing.
 * @throws std::runtime_error after object destruction when the
 * `destroy_runtime_error` or `all` fixture mode is active.
 * @note Storage is static and is not deallocated; only the object lifetime is
 * ended after incrementing the externally inspectable destroy count. Throwing
 * is deliberately hostile ABI-fixture behavior used to verify the host fence.
 */
void ps_scheduler_plugin_destroy(ps::IScheduler* scheduler) {
  const bool throw_after_destroy = failure_enabled("destroy_runtime_error");
  g_destroy_count.fetch_add(1);
  append_lifecycle_trace("destroy");
  static_cast<DestroyCountScheduler*>(scheduler)->~DestroyCountScheduler();
  if (throw_after_destroy) {
    throw std::runtime_error("fixture destroy failure");
  }
}

/**
 * @brief Returns the fixture plugin version marker.
 * @return Process-lifetime version string.
 * @throws Nothing.
 */
const char* ps_scheduler_plugin_get_version() {
  record_load_probe(LoadProbe::GetVersion);
  return "test";
}

/**
 * @brief Reads the number of currently live fixture schedulers.
 * @return Active instance count.
 * @throws Nothing.
 */
int ps_test_scheduler_active_count() {
  return g_active_count.load();
}

/**
 * @brief Reads the number of plugin destroy-export invocations.
 * @return Destroy invocation count since the last reset.
 * @throws Nothing.
 */
int ps_test_scheduler_destroy_count() {
  return g_destroy_count.load();
}

/**
 * @brief Reads one host-owner runtime forwarding counter.
 * @param probe Stable zero-based `ForwardingProbe` numeric value.
 * @return Recorded call count, or -1 when `probe` is outside the fixture
 * counter range.
 * @throws Nothing.
 * @note This fixture-only C export avoids sharing plugin-owned C++ objects with
 * the test executable while the plugin remains loaded.
 */
int ps_test_scheduler_forwarding_count(int probe) {
  if (probe < 0 || probe >= static_cast<int>(ForwardingProbe::Count)) {
    return -1;
  }
  return g_forwarding_counts[static_cast<std::size_t>(probe)].load(
      std::memory_order_relaxed);
}

/**
 * @brief Reads one scheduler-plugin discovery callback counter.
 * @param probe Stable zero-based `LoadProbe` numeric value.
 * @return Recorded call count, or -1 when `probe` is outside the fixture
 * counter range.
 * @throws Nothing.
 * @note The export remains fixture-only and performs no allocation while the
 * host failpoint is armed or immediately after it fires.
 */
int ps_test_scheduler_load_probe_count(int probe) {
  if (probe < 0 || probe >= static_cast<int>(LoadProbe::Count)) {
    return -1;
  }
  return g_load_probe_counts[static_cast<std::size_t>(probe)].load(
      std::memory_order_relaxed);
}

/**
 * @brief Resets lifecycle counters before a test creates an instance.
 * @return Nothing.
 * @throws Nothing.
 * @note Call only while no fixture instance is active.
 */
void ps_test_scheduler_reset_counts() {
  g_active_count.store(0);
  g_destroy_count.store(0);
  for (auto& count : g_forwarding_counts) {
    count.store(0, std::memory_order_relaxed);
  }
  for (auto& count : g_load_probe_counts) {
    count.store(0, std::memory_order_relaxed);
  }
}
}
