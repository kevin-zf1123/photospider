#include <array>
#include <atomic>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <limits>
#include <new>
#include <optional>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#if !defined(_WIN32)
#include <fcntl.h>
#include <unistd.h>

#include <cerrno>
#endif

#include "photospider/core/graph_error.hpp"
#include "photospider/scheduler/scheduler_plugin_api.hpp"
#include "photospider/scheduler/scheduler_task_runtime.hpp"

namespace {

/** @brief Short plugin scheduler name used by the ordinary lifecycle case. */
constexpr const char* kShortSchedulerType = "destroy_count_test";
/** @brief Name long enough to force host-owned `std::string` allocation. */
constexpr const char* kLongSchedulerType =                             // NOLINT
    "destroy_count_scheduler_type_long_enough_to_force_owned_string_"  // NOLINT
    "storage";                                                         // NOLINT
/** @brief Type already provided by the production serial fixture. */
constexpr const char* kConflictingSchedulerType = "serial_debug_example";
/** @brief Host-only marker text used to prove plugin-visible exception type. */
constexpr const char* kHostTaskMarkerMessage = "host task marker";
/** @brief Environment key selecting a newline-delimited lifecycle trace. */
constexpr const char* kTraceEnvironment = "PS_DESTROY_COUNT_SCHEDULER_TRACE";
/** @brief Environment key selecting one or all fixture exception modes. */
constexpr const char* kFailureEnvironment =
    "PS_DESTROY_COUNT_SCHEDULER_FAILURE";  // NOLINT
/** @brief Environment key enabling one duplicate discovery type entry. */
constexpr const char* kDuplicateTypeEnvironment =
    "PS_DESTROY_COUNT_SCHEDULER_DUPLICATE_TYPE";  // NOLINT
/** @brief Environment key selecting a discovery rejection mode. */
constexpr const char* kLoadFailureEnvironment =
    "PS_DESTROY_COUNT_SCHEDULER_LOAD_FAILURE";  // NOLINT
/** @brief Environment key naming one fixture-only compute FIFO gate. */
constexpr const char* kComputeGateEnvironment =
    "PS_DESTROY_COUNT_SCHEDULER_COMPUTE_GATE";  // NOLINT
/** @brief Environment key enabling host-context calls during fixture detach. */
constexpr const char* kDetachContextProbeEnvironment =
    "PS_DESTROY_COUNT_SCHEDULER_DETACH_CONTEXT_PROBE";  // NOLINT

/** @brief Number of placement-created fixture instances still alive. */
std::atomic<int> g_active_count{0};
/** @brief Number of calls that reached the plugin destroy export. */
std::atomic<int> g_destroy_count{0};
/** @brief Number of host attach attempts since fixture reset. */
std::atomic<int> g_attach_count{0};
/** @brief Number of scheduler start attempts since fixture reset. */
std::atomic<int> g_start_count{0};
/** @brief Number of shutdown attempts made while a fixture was running. */
std::atomic<int> g_shutdown_count{0};
/** @brief Whether the process-scoped one-shot shutdown failure was consumed. */
std::atomic<bool> g_shutdown_failure_once_consumed{false};
/** @brief Whether the process-scoped compute synchronization gate was used. */
std::atomic<bool> g_compute_gate_consumed{false};

/**
 * @brief Stable counter slots for host-owner runtime forwarding probes.
 * @throws Nothing.
 * @note The numeric order is mirrored by `test_scheduler_plugin_loader` through
 * the fixture-only `ps_test_scheduler_forwarding_count` export.
 */
enum class ForwardingProbe : std::size_t {
  /** @brief Calls to the plugin runtime's device inventory override. */
  AvailableDevices = 0,
  /** @brief Calls to the plugin runtime's initial handle-batch override. */
  InitialHandles,
  /** @brief Calls to the plugin runtime's worker handle-batch override. */
  WorkerHandles,
  /** @brief Calls to the plugin runtime's any-thread callback override. */
  AnyThreadTask,
  /** @brief Number of valid fixture forwarding counters. */
  Count,
};

/** @brief Number of allocation-free forwarding probe counters. */
constexpr auto kProbeCount = static_cast<std::size_t>(ForwardingProbe::Count);
/** @brief Fixed allocation-free counter storage for forwarding probes. */
using ForwardingCounters = std::array<std::atomic<int>, kProbeCount>;
/** @brief Allocation-free counters for every forwarding probe slot. */
ForwardingCounters g_forwarding_counts = {0, 0, 0, 0};

/**
 * @brief Stable counter slots for scheduler-plugin discovery callbacks.
 * @throws Nothing.
 * @note The numeric order is mirrored by the loader transaction regression
 * through `ps_test_scheduler_load_probe_count`.
 */
enum class LoadProbe : std::size_t {
  /** @brief Calls to the mandatory numeric ABI handshake. */
  GetAbiVersion = 0,
  /** @brief Calls to the type-count export. */
  GetCount,
  /** @brief Calls to the indexed type-name export. */
  GetName,
  /** @brief Calls to the mandatory indexed description export. */
  GetDescription,
  /** @brief Calls to the mandatory implementation-version export. */
  GetVersion,
  /** @brief Number of valid discovery callback counters. */
  Count,
};

/** @brief Number of allocation-free discovery callback counters. */
constexpr auto kLoadProbeCount = static_cast<std::size_t>(LoadProbe::Count);
/** @brief Fixed counter storage for discovery callback probes. */
using LoadProbeCounters = std::array<std::atomic<int>, kLoadProbeCount>;
/** @brief Allocation-free counters for every discovery callback slot. */
LoadProbeCounters g_load_probe_counts = {0, 0, 0, 0, 0};

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
 * @brief DSO-defined exception used to verify host-side normalization order.
 *
 * Both virtual functions are emitted out of line in this fixture library, so
 * using or destroying an escaped instance after `library_unload` would be an
 * invalid access through the unloaded DSO.
 *
 * @throws Nothing.
 * @note `what()` and destruction append allocation-free trace events while the
 *       fixture is still mapped.
 */
class SchedulerPluginCustomError final : public std::exception {
 public:
  /**
   * @brief Records destruction of the DSO-owned exception object.
   * @throws Nothing.
   */
  ~SchedulerPluginCustomError() noexcept override;

  /**
   * @brief Returns the fixed custom scheduler-plugin diagnostic.
   * @return Process-lifetime text owned by this fixture DSO.
   * @throws Nothing.
   */
  const char* what() const noexcept override;
};

/** @copydoc SchedulerPluginCustomError::~SchedulerPluginCustomError */
SchedulerPluginCustomError::~SchedulerPluginCustomError() noexcept {
  append_lifecycle_trace("custom_exception_destroy");
}

/** @copydoc SchedulerPluginCustomError::what */
const char* SchedulerPluginCustomError::what() const noexcept {
  append_lifecycle_trace("custom_exception_what");
  return "fixture scheduler custom exception";
}

/**
 * @brief Waits once on the optional fixture-only compute synchronization FIFO.
 *
 * @return Nothing after one byte is received or when no gate is configured.
 * @throws std::runtime_error if a configured POSIX gate cannot be opened or
 *         does not deliver one byte.
 * @note This test-fixture control runs inside the real plugin scheduler after
 *       daemon Host admission. It is not compiled into a product scheduler,
 *       exposed as a daemon flag, or represented on the IPC wire. A one-shot
 *       atomic prevents later batches and graph cleanup from waiting again.
 */
void wait_for_compute_gate() {
  const char* path = std::getenv(kComputeGateEnvironment);
  if (path == nullptr || path[0] == '\0' ||
      g_compute_gate_consumed.exchange(true, std::memory_order_relaxed)) {
    return;
  }
#if defined(_WIN32)
  (void)path;
#else
  append_lifecycle_trace("compute_gate_wait");
  int descriptor = -1;
  do {
    descriptor = ::open(path, O_RDONLY | O_CLOEXEC);
  } while (descriptor < 0 && errno == EINTR);
  if (descriptor < 0) {
    throw std::runtime_error("fixture compute gate open failed");
  }
  char token = 0;
  ssize_t received = -1;
  do {
    received = ::read(descriptor, &token, 1);
  } while (received < 0 && errno == EINTR);
  const int close_result = ::close(descriptor);
  if (received != 1 || close_result != 0) {
    throw std::runtime_error("fixture compute gate read failed");
  }
  append_lifecycle_trace("compute_gate_release");
#endif
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
 * @brief Checks one exact lifecycle failure mode without the `all` wildcard.
 * @param mode Exact specialized mode name to match.
 * @return True only when the fixture environment equals `mode`.
 * @throws Nothing.
 * @note Second-attempt attach/start failures use exact matching so the ordinary
 *       `all` cleanup-fence scenario retains its established first lifecycle.
 */
bool exact_failure_enabled(const char* mode) noexcept {
  const char* configured = std::getenv(kFailureEnvironment);
  return configured != nullptr && std::strcmp(configured, mode) == 0;
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
 * @brief Checks whether one discovery rejection mode is enabled.
 * @param mode Exact load-failure mode name to match.
 * @return True when the dedicated load-failure environment selects `mode`.
 * @throws Nothing.
 * @note This key is separate from lifecycle failure injection so callback
 * exceptions and invalid identity metadata can be selected independently from
 * existing destructor modes.
 */
bool load_failure_enabled(const char* mode) noexcept {
  const char* configured = std::getenv(kLoadFailureEnvironment);
  return configured && std::strcmp(configured, mode) == 0;
}

/**
 * @brief Records actual dynamic-library teardown for ownership-order tests.
 *
 * @throws Nothing.
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

/**
 * @brief Process-per-load probe whose destructor observes final handle release.
 * @note Construction is static and allocation-free; destruction appends the
 *       final lifecycle trace after all scheduler owners release the DSO.
 */
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
 * @throws Configured fixture failures and synchronous task exceptions according
 *         to the individual method contract.
 * @note Instances live in `g_scheduler_storage`; callers must pair successful
 * placement construction with `ps_scheduler_plugin_destroy` exactly once.
 */
class DestroyCountScheduler final : public ps::IScheduler {
 public:
  /**
   * @brief Marks one placement-created fixture instance active.
   * @throws Nothing.
   * @note Construction allocates no memory and publishes no host ownership.
   */
  DestroyCountScheduler() noexcept { g_active_count.fetch_add(1); }

  /**
   * @brief Marks the placement-owned fixture instance inactive.
   * @throws Nothing.
   * @note Storage remains static for the matching destroy export to reuse.
   */
  ~DestroyCountScheduler() noexcept override { g_active_count.fetch_sub(1); }

  /**
   * @brief Retains a borrowed host context and optionally fails a reattach.
   * @param host Borrowed context that outlives shutdown and detach.
   * @return Nothing.
   * @throws std::runtime_error on the second attempt when the
   *         `attach_runtime_error_on_second` mode is active.
   * @note Failure occurs after retaining `host` and emits
   *       `attach_second_failure`, modeling a plugin that requires destructor
   *       detach fallback after partial attach state publication.
   */
  void attach(ps::SchedulerHostContext& host) override {
    host_ = &host;
    const int attempt =
        g_attach_count.fetch_add(1, std::memory_order_relaxed) + 1;
    if (attempt == 2 &&
        exact_failure_enabled("attach_runtime_error_on_second")) {
      append_lifecycle_trace("attach_second_failure");
      throw std::runtime_error("fixture second attach failure");
    }
  }
  /**
   * @brief Probes and clears the borrowed host before optional failure.
   * @return Nothing.
   * @throws std::bad_alloc when the `detach_bad_alloc` or `all` mode is active.
   * @note When the dedicated context-probe environment key equals `1`, the
   *       fixture calls both host services and records their successful return
   *       before the ordinary detach marker. Trace and pointer clearing precede
   *       the configured exception, so even hostile lifecycle failure cannot
   *       retain the host context.
   */
  void detach() override {
    const char* context_probe = std::getenv(kDetachContextProbeEnvironment);
    if (host_ != nullptr && context_probe != nullptr &&
        std::strcmp(context_probe, "1") == 0) {
      append_lifecycle_trace(host_->is_device_available(ps::Device::CPU)
                                 ? "detach_context_cpu_available"
                                 : "detach_context_cpu_unavailable");
      host_->log_event(ps::SchedulerTraceAction::Execute, 38, -1, 38U);
      append_lifecycle_trace("detach_context_log_event");
    }
    append_lifecycle_trace("detach");
    host_ = nullptr;
    if (failure_enabled("detach_bad_alloc")) {
      throw std::bad_alloc{};
    }
  }
  /**
   * @brief Marks the fixture running and optionally fails a restart.
   * @return Nothing.
   * @throws std::runtime_error on the second attempt when the
   *         `start_runtime_error_on_second` mode is active.
   * @note Failure occurs after publishing `running_` and emits
   *       `start_second_failure`, modeling partial worker/resource ownership
   *       that requires destructor shutdown fallback.
   */
  void start() override {
    running_ = true;
    const int attempt =
        g_start_count.fetch_add(1, std::memory_order_relaxed) + 1;
    if (attempt == 2 &&
        exact_failure_enabled("start_runtime_error_on_second")) {
      append_lifecycle_trace("start_second_failure");
      throw std::runtime_error("fixture second start failure");
    }
  }
  /**
   * @brief Stops the fixture and optionally injects a retryable lifecycle
   * error.
   * @return Nothing.
   * @throws ps::GraphError when `shutdown_graph_not_found` or
   *         `shutdown_graph_io` is active.
   * @throws std::runtime_error when `shutdown_runtime_error`,
   * `shutdown_runtime_error_once`, or `all` is active.
   * @note The counter and trace precede failure injection. Failed attempts keep
   *       the scheduler running so a later close must execute shutdown again.
   */
  void shutdown() override {
    if (!running_) {
      return;
    }
    g_shutdown_count.fetch_add(1, std::memory_order_relaxed);
    append_lifecycle_trace("shutdown");
    const char* failure_mode = std::getenv(kFailureEnvironment);
    if (failure_mode != nullptr &&
        std::strcmp(failure_mode, "shutdown_graph_not_found") == 0) {
      throw ps::GraphError(ps::GraphErrc::NotFound,
                           "fixture shutdown graph-not-found failure");
    }
    if (failure_mode != nullptr &&
        std::strcmp(failure_mode, "shutdown_graph_io") == 0) {
      throw ps::GraphError(ps::GraphErrc::Io,
                           "fixture shutdown graph-io failure");
    }
    if (failure_enabled("shutdown_runtime_error")) {
      throw std::runtime_error("fixture shutdown failure");
    }
    if (failure_enabled("shutdown_runtime_error_once") &&
        !g_shutdown_failure_once_consumed.exchange(true,
                                                   std::memory_order_relaxed)) {
      throw std::runtime_error("fixture shutdown failure");
    }
    running_ = false;
  }

  /**
   * @brief Returns the stable short fixture name.
   * @return Owned fixture name.
   * @throws std::bad_alloc if string construction cannot allocate.
   * @note The returned copy does not alias plugin metadata storage.
   */
  std::string name() const override { return "destroy_count_test"; }
  /**
   * @brief Returns the current fixture completion count as diagnostic text.
   * @return Owned `completion=<count>` snapshot.
   * @throws std::bad_alloc if string construction cannot allocate.
   * @throws std::invalid_argument when the `stats_invalid_argument` mode is
   *         active.
   * @throws SchedulerPluginCustomError when the `stats_custom_exception` mode
   *         is active.
   * @note Tests use this copied value to verify counter-boundary behavior
   * through the real plugin owner without exposing mutable fixture state.
   */
  std::string get_stats() const override {
    if (exact_failure_enabled("stats_invalid_argument")) {
      throw std::invalid_argument("fixture scheduler invalid argument");
    }
    if (exact_failure_enabled("stats_custom_exception")) {
      throw SchedulerPluginCustomError{};
    }
    return "completion=" + std::to_string(tasks_to_complete_);
  }
  /**
   * @brief Returns whether `start()` has not been balanced by shutdown.
   * @return True while the fixture publishes its running lifecycle.
   * @throws Nothing.
   * @note Tests call this only through externally serialized lifecycle paths.
   */
  bool is_running() const noexcept override { return running_; }
  /**
   * @brief Reports a distinctive CPU/Metal inventory for owner forwarding.
   * @return CPU followed by Metal GPU, or one invalid value in the dedicated
   * fixture failure mode.
   * @throws std::bad_alloc if vector result storage cannot allocate.
   * @note The distinctive two-device result detects an owner that accidentally
   * uses `SchedulerTaskRuntime`'s CPU-only default implementation. The invalid
   * mode proves host planning validates DSO-provided public enum values.
   */
  std::vector<ps::Device> available_devices() const override {
    record_forwarding_probe(ForwardingProbe::AvailableDevices);
    if (exact_failure_enabled("available_devices_invalid")) {
      return {
          static_cast<ps::Device>(std::numeric_limits<std::uint32_t>::max())};
    }
    return {ps::Device::CPU, ps::Device::GPU_METAL};
  }

  /**
   * @brief Executes one initial borrowed-handle batch through the native API.
   * @param handles Ready handles consumed synchronously in input order.
   * @param total_task_count Completion count recorded after failure injection.
   * @param priority Ignored scheduling hint.
   * @return Nothing.
   * @throws std::bad_alloc before any callback when the
   * `handle_batch_bad_alloc` mode is active.
   * @throws std::runtime_error after the bad-allocation failpoint and before
   *         any callback if the configured fixture compute gate cannot be
   *         opened, read, or closed.
   * @throws Any task-executor exception unchanged.
   * @note The call counter advances before failure so tests prove the host
   * reached this override rather than a base closure fallback. Successful
   * admission also resets fixture-local first-exception state for the batch.
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
    wait_for_compute_gate();
    tasks_to_complete_ = total_task_count;
    exception_ = nullptr;
    for (const auto& handle : handles) {
      if (handle) {
        try {
          handle.run();
        } catch (const std::exception& error) {
          if (std::strcmp(error.what(), kHostTaskMarkerMessage) == 0) {
            append_lifecycle_trace("host_handle_std_exception_visible");
          }
          throw;
        }
      }
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
   * @note The callback is never retained after this method returns.
   */
  void submit_ready_task_any_thread(
      Task&& task,
      ps::SchedulerTaskPriority priority = ps::SchedulerTaskPriority::Normal,
      std::optional<uint64_t> epoch = std::nullopt) override {
    record_forwarding_probe(ForwardingProbe::AnyThreadTask);
    (void)priority;
    (void)epoch;
    if (task) {
      try {
        task();
      } catch (const std::exception& error) {
        if (std::strcmp(error.what(), kHostTaskMarkerMessage) == 0) {
          append_lifecycle_trace("host_callback_std_exception_visible");
        }
        throw;
      }
    }
  }

  /**
   * @brief Completes inline and rethrows the first host-published exception.
   * @return Nothing.
   * @throws The exact exception retained by `set_exception`.
   * @note No callback or borrowed task handle remains after synchronous
   *       submission returns. The rethrow lets the host owner prove that task
   *       exceptions remain exact while plugin-origin failures are normalized.
   */
  void wait_for_completion() override {
    if (exception_ != nullptr) {
      try {
        std::rethrow_exception(exception_);
      } catch (const std::exception& error) {
        if (std::strcmp(error.what(), kHostTaskMarkerMessage) == 0) {
          append_lifecycle_trace("host_stored_std_exception_visible");
        }
        throw;
      }
    }
  }
  /**
   * @brief Retains an exception pointer for fixture completeness.
   * @param e Exception identity supplied by the host.
   * @return Nothing.
   * @throws Nothing.
   * @note Null and duplicate input are ignored. The first non-null identity is
   * retained until the next initial batch resets fixture-local state. The
   * `ignore_host_exception` mode deliberately retains nothing so the host
   * registry becomes the final owner for destructor-reentry coverage.
   */
  void set_exception(std::exception_ptr e) noexcept override {
    if (exact_failure_enabled("ignore_host_exception")) {
      return;
    }
    if (e != nullptr && exception_ == nullptr) {
      exception_ = std::move(e);
    }
  }
  /**
   * @brief Increases the synchronous fixture completion counter.
   * @param delta Positive value to add.
   * @return Nothing.
   * @throws std::overflow_error if the addition would exceed `INT_MAX`.
   * @note Nonpositive values are ignored and rejected overflow leaves the
   * counter unchanged.
   */
  void inc_tasks_to_complete(int delta) override {
    if (delta <= 0) {
      return;
    }
    if (tasks_to_complete_ > std::numeric_limits<int>::max() - delta) {
      throw std::overflow_error("fixture completion counter overflow");
    }
    tasks_to_complete_ += delta;
  }
  /**
   * @brief Decrements the synchronous completion counter with a zero floor.
   * @return Nothing.
   * @throws Nothing.
   * @note Repeated calls after settlement leave the counter at zero.
   */
  void dec_tasks_to_complete() noexcept override {
    if (tasks_to_complete_ > 0) {
      --tasks_to_complete_;
    }
  }
  /**
   * @brief Forwards trace events through the borrowed public host context.
   * @param action Scheduler event kind.
   * @param node_id Associated node identifier.
   * @return Nothing.
   * @throws Nothing.
   * @note Detached calls are ignored and the host retains no supplied scalar.
   */
  void log_event(ps::SchedulerTraceAction action,
                 int node_id) noexcept override {
    if (host_ != nullptr) {
      host_->log_event(action, node_id, -1, 0U);
    }
  }

 private:
  /** @brief Local lifecycle flag exposed through `is_running`. */
  bool running_ = false;
  /** @brief Borrowed public host context cleared by detach. */
  ps::SchedulerHostContext* host_ = nullptr;
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

#if !defined(PHOTOSPIDER_SCHEDULER_FIXTURE_OMIT_HANDSHAKE)
/**
 * @brief Reports the scheduler SDK generation selected for this fixture DSO.
 * @return Configured fixture ABI value, or the current public value by default.
 * @throws Nothing.
 * @note The loader must invoke this before every other fixture ABI export.
 */
PHOTOSPIDER_SCHEDULER_PLUGIN_EXPORT std::uint32_t
ps_scheduler_plugin_get_abi_version() noexcept {
  record_load_probe(LoadProbe::GetAbiVersion);
#if defined(PHOTOSPIDER_SCHEDULER_FIXTURE_ABI_VERSION)
  return PHOTOSPIDER_SCHEDULER_FIXTURE_ABI_VERSION;
#else
  return ps::PS_SCHEDULER_PLUGIN_ABI_VERSION;
#endif
}
#endif

/**
 * @brief Reports ordinary types or one deterministic rejection shape.
 * @return Zero for `count_zero`, one for `all_conflicting_types`, two
 * ordinarily, or three while the duplicate-type fixture mode is enabled.
 * @throws Nothing.
 * @note The optional duplicate entry exercises transactional conflict rollback.
 */
PHOTOSPIDER_SCHEDULER_PLUGIN_EXPORT std::uint32_t
ps_scheduler_plugin_get_count() noexcept {
  record_load_probe(LoadProbe::GetCount);
  if (load_failure_enabled("count_zero")) {
    return 0U;
  }
  if (load_failure_enabled("all_conflicting_types")) {
    return 1U;
  }
  return duplicate_type_enabled() ? 3U : 2U;
}

/**
 * @brief Returns a stable scheduler name for one fixture entry.
 * @param index Zero-based fixture entry.
 * @return Process-lifetime name; fixture rejection modes return nullptr or an
 * empty string at the second in-range index, and ordinary out-of-range indices
 * return nullptr.
 * @throws Nothing.
 * @note The loader copies valid text before releasing the fixture DSO.
 */
PHOTOSPIDER_SCHEDULER_PLUGIN_EXPORT const char* ps_scheduler_plugin_get_name(
    std::uint32_t index) noexcept {
  record_load_probe(LoadProbe::GetName);
  if (index == 0 && load_failure_enabled("all_conflicting_types")) {
    return kConflictingSchedulerType;
  }
  if (index == 0) {
    return kShortSchedulerType;
  }
  if (index == 1) {
    if (load_failure_enabled("name_null")) {
      return nullptr;
    }
    if (load_failure_enabled("name_empty")) {
      return "";
    }
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
 * @throws SchedulerPluginCustomError for the second ordinary type when the
 * `description_custom_exception` mode is enabled.
 * @note The exception occurs after the first type has been fully staged, which
 * exercises rollback of partial candidate metadata.
 */
PHOTOSPIDER_SCHEDULER_PLUGIN_EXPORT const char*
ps_scheduler_plugin_get_description(std::uint32_t index) {
  record_load_probe(LoadProbe::GetDescription);
  if (index == 1 && load_failure_enabled("description_runtime_error")) {
    throw std::runtime_error("fixture scheduler description failure");
  }
  if (index == 1 && load_failure_enabled("description_custom_exception")) {
    throw SchedulerPluginCustomError{};
  }
  return index < 2U ? "Destroy-count scheduler lifecycle test" : nullptr;
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
PHOTOSPIDER_SCHEDULER_PLUGIN_EXPORT ps::IScheduler* ps_scheduler_plugin_create(
    const char* type_name, std::uint32_t worker_count) noexcept {
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
PHOTOSPIDER_SCHEDULER_PLUGIN_EXPORT void ps_scheduler_plugin_destroy(
    ps::IScheduler* scheduler) {
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
 * @note The value is diagnostic and does not participate in ABI gating.
 */
PHOTOSPIDER_SCHEDULER_PLUGIN_EXPORT const char*
ps_scheduler_plugin_get_version() noexcept {
  record_load_probe(LoadProbe::GetVersion);
  return "test";
}

/**
 * @brief Reads the number of currently live fixture schedulers.
 * @return Active instance count.
 * @throws Nothing.
 * @note Callers read this only while the fixture DSO remains loaded.
 */
int ps_test_scheduler_active_count() noexcept {
  return g_active_count.load();
}

/**
 * @brief Reads the number of plugin destroy-export invocations.
 * @return Destroy invocation count since the last reset.
 * @throws Nothing.
 * @note The counter advances before optional hostile destroy failure.
 */
int ps_test_scheduler_destroy_count() noexcept {
  return g_destroy_count.load();
}

/**
 * @brief Reads the number of shutdown attempts made while running.
 * @return Counted shutdown attempts since the last fixture reset.
 * @throws Nothing.
 * @note The count advances before failure injection, allowing retry tests to
 *       prove that a later close reached the scheduler again.
 */
int ps_test_scheduler_shutdown_count() noexcept {
  return g_shutdown_count.load(std::memory_order_relaxed);
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
int ps_test_scheduler_forwarding_count(int probe) noexcept {
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
int ps_test_scheduler_load_probe_count(int probe) noexcept {
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
void ps_test_scheduler_reset_counts() noexcept {
  g_active_count.store(0);
  g_destroy_count.store(0);
  g_attach_count.store(0, std::memory_order_relaxed);
  g_start_count.store(0, std::memory_order_relaxed);
  g_shutdown_count.store(0, std::memory_order_relaxed);
  g_shutdown_failure_once_consumed.store(false, std::memory_order_relaxed);
  g_compute_gate_consumed.store(false, std::memory_order_relaxed);
  for (auto& count : g_forwarding_counts) {
    count.store(0, std::memory_order_relaxed);
  }
  for (auto& count : g_load_probe_counts) {
    count.store(0, std::memory_order_relaxed);
  }
}
}
