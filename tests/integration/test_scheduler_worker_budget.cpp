#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <future>
#include <memory>
#include <mutex>
#include <new>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include "core/ps_types.hpp"  // NOLINT(build/include_subdir)
#include "graph/node.hpp"     // NOLINT(build/include_subdir)
#include "photospider/core/graph_error.hpp"
#include "photospider/host/host.hpp"
#include "scheduler/scheduler_exception_test_hooks.hpp"
#include "scheduler/scheduler_plugin_loader.hpp"
#include "scheduler/serial_debug_scheduler.hpp"

namespace ps {
namespace {

/** @brief Registered zero-thread fixture charged through plugin-plan rules. */
constexpr char kChargedSerialSchedulerType[] = "issue_43_charged_serial";

/** @brief Registered fixture whose factory deliberately returns no instance. */
constexpr char kNullSchedulerType[] = "issue_43_null_factory";

/** @brief Registered fixture whose factory raises a candidate GraphError. */
constexpr char kGraphErrorSchedulerType[] = "issue_43_graph_error_factory";

/** @brief Operation type used by real public-Host scheduler probes. */
constexpr char kBudgetProbeOperationType[] = "issue_43_budget_probe";

/** @brief Operation subtype used by real public-Host scheduler probes. */
constexpr char kBudgetProbeOperationSubtype[] = "callback";

/** @brief Non-blocking sink subtype joining parallel callback sources. */
constexpr char kBudgetProbeSinkSubtype[] = "sink";

/** @brief Number of Graph Runs sharing one process service in the probe. */
constexpr std::size_t kWorkerProbeGraphCount = 4U;

/** @brief Fixed process CPU service grant shared by every probe Graph. */
constexpr unsigned int kWorkerProbeGrant = 4U;

/** @brief Independent ready callback sources placed in each probe Graph. */
constexpr std::size_t kWorkerProbeSources = 1U;

/** @brief Join-node id following every physical probe source. */
constexpr int kWorkerProbeSinkId = static_cast<int>(kWorkerProbeSources + 1U);

/** @brief Maximum simultaneous HP callbacks admitted across probe Graphs. */
constexpr std::size_t kWorkerProbeActive = kWorkerProbeGrant;

static_assert(kWorkerProbeActive == kWorkerProbeGraphCount,
              "each overlapping Graph must contribute one blocked callback");

/**
 * @brief Records real operation callbacks entered through scheduler execution.
 *
 * @throws std::system_error If mutex/condition-variable operations fail.
 * @throws std::bad_alloc If callback-observation storage cannot grow.
 * @note Each test owns one instance and keeps it published until every Host
 * compute and future that can reach the process-persistent callback has joined.
 */
class BudgetOperationProbe final {
 public:
  /**
   * @brief Records one callback thread and optionally blocks its completion.
   * @return Nothing.
   * @throws std::system_error If mutex/condition-variable operations fail.
   * @throws std::bad_alloc If callback-observation storage cannot grow.
   * @note Active and maximum counts bracket the real operation callback body;
   * blocking mode holds each scheduler worker until `release_callbacks()`.
   */
  void record_callback() {
    std::unique_lock<std::mutex> lock(mutex_);
    callback_threads_.push_back(std::this_thread::get_id());
    ++active_callbacks_;
    max_active_callbacks_ = std::max(max_active_callbacks_, active_callbacks_);
    cv_.notify_all();
    if (blocking_callbacks_) {
      cv_.wait(lock, [this] { return callbacks_released_; });
    }
    --active_callbacks_;
    cv_.notify_all();
  }

  /**
   * @brief Clears all observations before another forced compute.
   * @return Nothing.
   * @throws std::system_error If mutex locking fails.
   * @throws std::logic_error If any callback is active or blocked.
   * @note No callback may overlap reset in the owning test.
   */
  void reset() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (active_callbacks_ != 0U || blocking_callbacks_) {
      throw std::logic_error("cannot reset an active budget callback probe");
    }
    callback_threads_.clear();
    max_active_callbacks_ = 0U;
    callbacks_released_ = true;
  }

  /**
   * @brief Enables callback blocking and resets prior observations.
   * @return Nothing.
   * @throws std::system_error If mutex locking fails.
   * @throws std::logic_error If another blocking phase or callback is active.
   * @note Callers must arrange RAII release before destroying compute futures.
   */
  void begin_blocking_callbacks() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (active_callbacks_ != 0U || blocking_callbacks_) {
      throw std::logic_error("budget callback blocking is already active");
    }
    callback_threads_.clear();
    max_active_callbacks_ = 0U;
    callbacks_released_ = false;
    blocking_callbacks_ = true;
  }

  /**
   * @brief Releases every blocked and future callback in the current phase.
   * @return Nothing.
   * @throws Nothing; mutex failures terminate at this cleanup boundary.
   * @note The operation is idempotent so explicit and destructor cleanup may
   * both call it safely.
   */
  void release_callbacks() noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    callbacks_released_ = true;
    blocking_callbacks_ = false;
    cv_.notify_all();
  }

  /**
   * @brief Waits until at least a requested number of callbacks are active.
   * @param count Minimum simultaneous operation callbacks to observe.
   * @param timeout Maximum monotonic wait duration.
   * @return True when the active count reaches the requested value in time.
   * @throws std::system_error If mutex/condition-variable operations fail.
   * @note A request above the configured worker grant doubles as a bounded
   * oversubscription observation window while callbacks remain blocked.
   */
  bool wait_for_active_callbacks(std::size_t count,
                                 std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> lock(mutex_);
    return cv_.wait_for(lock, timeout,
                        [this, count] { return active_callbacks_ >= count; });
  }

  /**
   * @brief Copies all observed callback thread identities coherently.
   * @return Thread ids in callback-entry order.
   * @throws std::system_error If mutex locking fails.
   * @throws std::bad_alloc If result storage allocation fails.
   */
  std::vector<std::thread::id> callback_threads() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return callback_threads_;
  }

  /**
   * @brief Returns the number of callbacks entered in the current phase.
   * @return Exact callback-entry count.
   * @throws std::system_error If mutex locking fails.
   */
  std::size_t callback_count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return callback_threads_.size();
  }

  /**
   * @brief Returns callbacks currently inside the registered operation.
   * @return Exact active callback count.
   * @throws std::system_error If mutex locking fails.
   */
  std::size_t active_callback_count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return active_callbacks_;
  }

  /**
   * @brief Returns the maximum simultaneous real operation callbacks.
   * @return Peak active callback count since reset/blocking began.
   * @throws std::system_error If mutex locking fails.
   */
  std::size_t max_active_callback_count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return max_active_callbacks_;
  }

  /**
   * @brief Counts distinct physical threads observed inside real callbacks.
   * @return Number of unique `std::thread::id` values in this probe phase.
   * @throws std::system_error If mutex locking fails.
   * @throws std::bad_alloc If snapshot storage allocation fails.
   * @note Identities come from product operation callbacks, not scheduler
   * statistics, internal worker ids, or whole-process OS thread snapshots.
   */
  std::size_t unique_callback_thread_count() const {
    std::vector<std::thread::id> threads = callback_threads();
    std::sort(threads.begin(), threads.end());
    return static_cast<std::size_t>(std::distance(
        threads.begin(), std::unique(threads.begin(), threads.end())));
  }

 private:
  /** @brief Protects callback observations and snapshot copies. */
  mutable std::mutex mutex_;

  /** @brief Notifies tests and blocked callback workers of state changes. */
  std::condition_variable cv_;

  /** @brief Actual scheduler execution threads in callback-entry order. */
  std::vector<std::thread::id> callback_threads_;

  /** @brief Operation callbacks currently between entry and return. */
  std::size_t active_callbacks_ = 0U;

  /** @brief Largest simultaneous callback count in the current phase. */
  std::size_t max_active_callbacks_ = 0U;

  /** @brief Whether newly entered callbacks wait for explicit release. */
  bool blocking_callbacks_ = false;

  /** @brief Predicate that releases callbacks already waiting on `cv_`. */
  bool callbacks_released_ = true;
};

/** @brief Borrowed active operation probe, or null outside one test scope. */
std::atomic<BudgetOperationProbe*> g_budget_operation_probe{nullptr};

/**
 * @brief Publishes one borrowed operation probe for a lexical test scope.
 *
 * @throws std::logic_error If another probe is already installed.
 * @note Tests in this binary serialize process-global probe mutation.
 */
class ScopedBudgetOperationProbe final {
 public:
  /**
   * @brief Installs a probe that outlives this guard.
   * @param probe Test-owned observation state.
   * @throws std::logic_error If another probe is already installed.
   */
  explicit ScopedBudgetOperationProbe(BudgetOperationProbe& probe)
      : probe_(&probe) {
    BudgetOperationProbe* expected = nullptr;
    if (!g_budget_operation_probe.compare_exchange_strong(
            expected, probe_, std::memory_order_acq_rel,
            std::memory_order_acquire)) {
      throw std::logic_error("budget operation probe is already installed");
    }
  }

  /**
   * @brief Clears the borrowed probe after synchronous callback completion.
   * @throws Nothing.
   */
  ~ScopedBudgetOperationProbe() noexcept {
    BudgetOperationProbe* expected = probe_;
    (void)g_budget_operation_probe.compare_exchange_strong(
        expected, nullptr, std::memory_order_acq_rel,
        std::memory_order_acquire);
  }

  /**
   * @brief Prevents duplicate probe-publication ownership.
   * @param other Guard that retains clearing responsibility.
   * @throws Nothing because copying is unavailable.
   */
  ScopedBudgetOperationProbe(const ScopedBudgetOperationProbe& other) = delete;

  /**
   * @brief Prevents replacing probe-publication ownership.
   * @param other Guard that remains unchanged.
   * @return No value because assignment is unavailable.
   * @throws Nothing because assignment is unavailable.
   */
  ScopedBudgetOperationProbe& operator=(
      const ScopedBudgetOperationProbe& other) = delete;

 private:
  /** @brief Borrowed probe expected when this guard clears publication. */
  BudgetOperationProbe* probe_;
};

/**
 * @brief Enables callback blocking and guarantees release before future joins.
 *
 * @throws std::system_error If probe synchronization cannot begin.
 * @throws std::logic_error If another blocking phase is already active.
 * @note Declare this guard after compute-future storage so reverse destruction
 * releases scheduler workers before any unfinished `std::future` may block.
 */
class ScopedBudgetCallbackBlock final {
 public:
  /**
   * @brief Starts a fresh blocking callback observation phase.
   * @param probe Published probe that outlives this guard and all futures.
   * @throws std::system_error If probe synchronization fails.
   * @throws std::logic_error If another blocking phase is already active.
   */
  explicit ScopedBudgetCallbackBlock(BudgetOperationProbe& probe)
      : probe_(&probe) {
    probe_->begin_blocking_callbacks();
  }

  /**
   * @brief Releases blocked callbacks on every lexical exit.
   * @throws Nothing.
   */
  ~ScopedBudgetCallbackBlock() noexcept { release(); }

  /**
   * @brief Releases callbacks idempotently before explicit future joins.
   * @return Nothing.
   * @throws Nothing.
   */
  void release() noexcept {
    if (probe_ == nullptr) {
      return;
    }
    probe_->release_callbacks();
    probe_ = nullptr;
  }

  /**
   * @brief Prevents duplicate callback-release ownership.
   * @param other Guard that retains release responsibility.
   * @throws Nothing because copying is unavailable.
   */
  ScopedBudgetCallbackBlock(const ScopedBudgetCallbackBlock& other) = delete;

  /**
   * @brief Prevents replacing callback-release ownership.
   * @param other Guard that remains unchanged.
   * @return No value because assignment is unavailable.
   * @throws Nothing because assignment is unavailable.
   */
  ScopedBudgetCallbackBlock& operator=(const ScopedBudgetCallbackBlock& other) =
      delete;

 private:
  /** @brief Borrowed probe to release, or null after explicit release. */
  BudgetOperationProbe* probe_;
};

/**
 * @brief Registers the real callback used by replacement and worker probes.
 * @return Nothing.
 * @throws std::bad_alloc If registry keys or callable storage cannot allocate.
 * @throws Any operation-registry synchronization failure unchanged.
 * @note Registration is process-persistent and idempotent. Source callbacks
 * record scheduler entry and both source/sink callbacks emit empty valid
 * outputs because tests observe execution, not pixels.
 */
void ensure_budget_probe_operation_registered() {
  static std::once_flag once;
  std::call_once(once, [] {
    OpRegistry::instance().register_op_hp_monolithic(
        kBudgetProbeOperationType, kBudgetProbeOperationSubtype,
        MonolithicOpFunc(
            [](const Node&, const std::vector<const NodeOutput*>&) {
              BudgetOperationProbe* probe =
                  g_budget_operation_probe.load(std::memory_order_acquire);
              if (probe != nullptr) {
                probe->record_callback();
              }
              return NodeOutput{};
            }));
    OpRegistry::instance().register_op_hp_monolithic(
        kBudgetProbeOperationType, kBudgetProbeSinkSubtype,
        MonolithicOpFunc(
            [](const Node&, const std::vector<const NodeOutput*>&) {
              return NodeOutput{};
            }));
  });
}

/**
 * @brief Returns process-lifetime construction evidence for the fixture.
 * @return Shared counter retained by both tests and registry callable state.
 * @throws std::bad_alloc On first access if shared allocation fails.
 * @note Tests compare relative snapshots and never assume execution order.
 */
std::shared_ptr<std::atomic<unsigned int>> charged_serial_constructions() {
  static const auto constructions =
      std::make_shared<std::atomic<unsigned int>>(0U);
  return constructions;
}

/**
 * @brief Returns process-lifetime null-factory invocation evidence.
 * @return Shared counter retained by tests and registered callable state.
 * @throws std::bad_alloc On first access if shared allocation fails.
 * @note Relative snapshots prove replacement reached construction only after
 * successful candidate admission without relying on global test order.
 */
std::shared_ptr<std::atomic<unsigned int>> null_scheduler_constructions() {
  static const auto constructions =
      std::make_shared<std::atomic<unsigned int>>(0U);
  return constructions;
}

/**
 * @brief Returns process-lifetime GraphError-factory invocation evidence.
 * @return Shared counter retained by tests and registered callable state.
 * @throws std::bad_alloc On first access if shared allocation fails.
 * @note Relative snapshots prove replacement reached candidate construction
 * before preserving the established handled Kernel bool boundary.
 */
std::shared_ptr<std::atomic<unsigned int>>
graph_error_scheduler_constructions() {
  static const auto constructions =
      std::make_shared<std::atomic<unsigned int>>(0U);
  return constructions;
}

/**
 * @brief Registers one workerless scheduler under ordinary plugin accounting.
 * @return Nothing.
 * @throws std::bad_alloc If registry or callable storage cannot allocate.
 * @note Registration is process-persistent. The factory records construction
 * before returning built-in serial behavior, so tests can fill process budget
 * without creating physical worker threads on a pre-fix implementation.
 */
void ensure_charged_serial_scheduler_registered() {
  static std::once_flag once;
  std::call_once(once, [] {
    auto constructions = charged_serial_constructions();
    SchedulerPluginLoader::instance().register_builtin(
        kChargedSerialSchedulerType,
        "workerless issue-43 Host aggregate admission fixture",
        [constructions](unsigned int worker_grant) {
          (void)worker_grant;
          constructions->fetch_add(1U, std::memory_order_relaxed);
          return std::make_unique<SerialDebugScheduler>();
        });
  });
}

/**
 * @brief Registers one type whose callable returns a null scheduler owner.
 * @return Nothing.
 * @throws std::bad_alloc If registry or callable storage cannot allocate.
 * @note Registration is process-persistent and models a supported plugin type
 * that becomes unavailable or declines construction after successful planning.
 */
void ensure_null_scheduler_registered() {
  static std::once_flag once;
  std::call_once(once, [] {
    auto constructions = null_scheduler_constructions();
    SchedulerPluginLoader::instance().register_builtin(
        kNullSchedulerType, "issue-43 null scheduler factory fixture",
        [constructions](
            unsigned int worker_grant) -> std::unique_ptr<IScheduler> {
          (void)worker_grant;
          constructions->fetch_add(1U, std::memory_order_relaxed);
          return nullptr;
        });
  });
}

/**
 * @brief Registers one type whose factory raises a categorized GraphError.
 * @return Nothing.
 * @throws std::bad_alloc If registry or callable storage cannot allocate.
 * @note The host-owned factory error models an existing candidate/plugin
 * lifecycle rejection that must remain a handled false/InvalidParameter result
 * rather than being newly exposed by budget-category propagation.
 */
void ensure_graph_error_scheduler_registered() {
  static std::once_flag once;
  std::call_once(once, [] {
    auto constructions = graph_error_scheduler_constructions();
    SchedulerPluginLoader::instance().register_builtin(
        kGraphErrorSchedulerType,
        "issue-43 handled candidate GraphError fixture",
        [constructions](
            unsigned int worker_grant) -> std::unique_ptr<IScheduler> {
          (void)worker_grant;
          constructions->fetch_add(1U, std::memory_order_relaxed);
          throw GraphError(GraphErrc::NotFound,
                           "issue-43 candidate factory rejection");
        });
  });
}

/**
 * @brief Owns one borrowed scheduler test hook for a lexical test scope.
 * @tparam Hook Immutable hook type accepted by the selected setter.
 * @throws Nothing after construction because setter operations are noexcept.
 * @note The guard clears the process-global test hook before borrowed hook or
 * context storage leaves scope. Tests remain single-threaded around mutation.
 */
template <typename Hook>
class ScopedBorrowedSchedulerHook final {
 public:
  /** @brief Scheduler-specific hook publication function. */
  using Setter = void (*)(const Hook*) noexcept;

  /**
   * @brief Publishes one borrowed hook until guard destruction.
   * @param setter Hook-specific publication function.
   * @param hook Borrowed immutable hook that outlives this guard.
   * @throws Nothing.
   */
  ScopedBorrowedSchedulerHook(Setter setter, const Hook* hook) noexcept
      : setter_(setter) {
    setter_(hook);
  }

  /**
   * @brief Clears the borrowed hook.
   * @throws Nothing.
   */
  ~ScopedBorrowedSchedulerHook() noexcept { setter_(nullptr); }

  /**
   * @brief Prevents duplicate clearing ownership.
   * @param other Guard that retains clearing responsibility.
   * @throws Nothing because copying is unavailable.
   */
  ScopedBorrowedSchedulerHook(const ScopedBorrowedSchedulerHook& other) =
      delete;

  /**
   * @brief Prevents replacing one clearing responsibility with another.
   * @param other Guard that remains unchanged.
   * @return No value because assignment is unavailable.
   * @throws Nothing because assignment is unavailable.
   */
  ScopedBorrowedSchedulerHook& operator=(
      const ScopedBorrowedSchedulerHook& other) = delete;

 private:
  /** @brief Non-null setter used to clear the borrowed hook. */
  Setter setter_;
};

/**
 * @brief Counts legacy CPU scheduler worker-creation hook entrances.
 * @throws Nothing for atomic construction and destruction.
 * @note The process ExecutionService does not invoke this legacy-owner hook.
 */
struct CpuWorkerCreationFailureProbe final {
  /** @brief Legacy scheduler hook entrances observed by the test. */
  std::atomic<unsigned int> entrances{0U};
};

/**
 * @brief Counts every first CPU worker-creation entrance without failing it.
 * @param context Borrowed CpuWorkerCreationFailureProbe owned by the test.
 * @param point Scheduler lifecycle point about to execute.
 * @param attempt One-based attempt within the current lifecycle operation.
 * @return Nothing.
 * @throws Nothing.
 * @note Saturated replacement must reject admission before this callback can
 * observe candidate worker creation.
 */
void count_cpu_worker_creation(void* context,
                               testing::SchedulerFailurePoint point,
                               std::size_t attempt) noexcept {
  if (point == testing::SchedulerFailurePoint::CpuThreadCreate &&
      attempt == 1U) {
    auto* probe = static_cast<CpuWorkerCreationFailureProbe*>(context);
    probe->entrances.fetch_add(1U, std::memory_order_relaxed);
  }
}

/**
 * @brief Injects allocation failure before a candidate's first CPU worker.
 * @param context Borrowed CpuWorkerCreationFailureProbe owned by the test.
 * @param point Scheduler lifecycle point about to execute.
 * @param attempt One-based attempt within the current lifecycle operation.
 * @return Nothing for unrelated lifecycle points.
 * @throws std::bad_alloc At the first CPU worker-creation entrance.
 * @note Ownerless process-service routing must never reach this legacy
 * scheduler hook.
 */
void fail_first_cpu_worker_creation(void* context,
                                    testing::SchedulerFailurePoint point,
                                    std::size_t attempt) {
  if (point == testing::SchedulerFailurePoint::CpuThreadCreate &&
      attempt == 1U) {
    auto* probe = static_cast<CpuWorkerCreationFailureProbe*>(context);
    probe->entrances.fetch_add(1U, std::memory_order_relaxed);
    throw std::bad_alloc{};
  }
}

/**
 * @brief Owns one unique temporary root for scheduler-budget Host tests.
 *
 * @throws std::filesystem::filesystem_error if the temporary root cannot be
 *         prepared.
 * @note Destruction removes the root through a non-throwing error-code path so
 *       cleanup cannot replace a test assertion.
 */
class ScopedSchedulerBudgetTempDir {
 public:
  /**
   * @brief Creates an empty directory below the platform temporary root.
   * @param name Stable test label used as the directory prefix.
   * @throws std::filesystem::filesystem_error if setup fails.
   */
  explicit ScopedSchedulerBudgetTempDir(const std::string& name)
      : root_(std::filesystem::temp_directory_path() /
              (name + "_" +
               std::to_string(std::chrono::high_resolution_clock::now()
                                  .time_since_epoch()
                                  .count()))) {
    std::filesystem::remove_all(root_);
    std::filesystem::create_directories(root_);
  }

  /**
   * @brief Prevents duplicate ownership of one cleanup path.
   * @param other Owner that retains cleanup responsibility.
   * @throws Nothing because copying is unavailable.
   */
  ScopedSchedulerBudgetTempDir(const ScopedSchedulerBudgetTempDir& other) =
      delete;

  /**
   * @brief Prevents replacing cleanup ownership.
   * @param other Owner whose path remains unchanged.
   * @return No value because assignment is unavailable.
   * @throws Nothing because assignment is unavailable.
   */
  ScopedSchedulerBudgetTempDir& operator=(
      const ScopedSchedulerBudgetTempDir& other) = delete;

  /**
   * @brief Removes the owned temporary directory.
   * @throws Nothing.
   */
  ~ScopedSchedulerBudgetTempDir() {
    std::error_code error;
    std::filesystem::remove_all(root_, error);
  }

  /**
   * @brief Returns the owned temporary root.
   * @return Borrowed path valid for this helper's lifetime.
   * @throws Nothing.
   */
  const std::filesystem::path& root() const noexcept { return root_; }

 private:
  /** @brief Unique temporary directory removed at destruction. */
  std::filesystem::path root_;
};

/**
 * @brief Builds one empty-graph load request under a unique test root.
 * @param root Temporary root whose lifetime encloses the Host call.
 * @param session Unique session label for the current test.
 * @return Public request with no source YAML and an isolated session root.
 * @throws std::bad_alloc If copied path or session storage cannot allocate.
 * @note Empty Graphs still exercise complete scheduler construction/start and
 * publication without requiring operation fixtures or source assets.
 */
GraphLoadRequest make_budget_load_request(const std::filesystem::path& root,
                                          const std::string& session) {
  GraphLoadRequest request;
  request.session = GraphSessionId{session};
  request.root_dir = (root / "sessions").string();
  return request;
}

/**
 * @brief Writes one real single-node Graph for scheduler callback probes.
 * @param path Destination YAML file created with its parent directory.
 * @return Nothing.
 * @throws std::filesystem::filesystem_error If parent creation fails.
 * @throws std::bad_alloc If path or stream storage allocation fails.
 * @throws std::runtime_error If the destination cannot be written completely.
 * @note The only node resolves to the process-persistent budget probe callback.
 */
void write_budget_probe_graph(const std::filesystem::path& path) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream output(path);
  if (!output) {
    throw std::runtime_error("failed to open scheduler budget probe Graph");
  }
  output << "- id: 1\n"
         << "  name: scheduler_budget_probe\n"
         << "  type: " << kBudgetProbeOperationType << "\n"
         << "  subtype: " << kBudgetProbeOperationSubtype << "\n"
         << "  parameters: {}\n";
  output.close();
  if (!output) {
    throw std::runtime_error("failed to write scheduler budget probe Graph");
  }
}

/**
 * @brief Writes independent callback sources feeding one non-blocking sink.
 * @param path Destination YAML file created with its parent directory.
 * @return Nothing.
 * @throws std::filesystem::filesystem_error If parent creation fails.
 * @throws std::bad_alloc If path or stream storage allocation fails.
 * @throws std::runtime_error If the destination cannot be written completely.
 * @note All eight source nodes are initially ready for one HP scheduler batch;
 * the sink becomes ready only after every blocked source callback completes.
 */
void write_parallel_budget_probe_graph(const std::filesystem::path& path) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream output(path);
  if (!output) {
    throw std::runtime_error(
        "failed to open parallel scheduler budget probe Graph");
  }
  for (std::size_t index = 0U; index < kWorkerProbeSources; ++index) {
    const int node_id = static_cast<int>(index + 1U);
    output << "- id: " << node_id << "\n"
           << "  name: scheduler_budget_source_" << node_id << "\n"
           << "  type: " << kBudgetProbeOperationType << "\n"
           << "  subtype: " << kBudgetProbeOperationSubtype << "\n"
           << "  parameters: {}\n";
  }
  output << "- id: " << kWorkerProbeSinkId << "\n"
         << "  name: scheduler_budget_sink\n"
         << "  type: " << kBudgetProbeOperationType << "\n"
         << "  subtype: " << kBudgetProbeSinkSubtype << "\n"
         << "  image_inputs:\n";
  for (std::size_t index = 0U; index < kWorkerProbeSources; ++index) {
    output << "    - from_node_id: " << (index + 1U) << "\n";
  }
  output << "  parameters: {}\n";
  output.close();
  if (!output) {
    throw std::runtime_error(
        "failed to write parallel scheduler budget probe Graph");
  }
}

/**
 * @brief Builds a real callback Graph load request under an isolated root.
 * @param root Temporary root whose lifetime encloses the Host call.
 * @param session Unique session label for the current test.
 * @return Public request pointing at a newly written single-node YAML Graph.
 * @throws std::filesystem::filesystem_error If fixture directories cannot be
 * created.
 * @throws std::bad_alloc If path, request, or registered callback storage
 * allocation fails.
 * @throws std::runtime_error If the YAML fixture cannot be written completely.
 * @note The request shares only the registered operation key with other tests.
 */
GraphLoadRequest make_budget_probe_load_request(
    const std::filesystem::path& root, const std::string& session) {
  ensure_budget_probe_operation_registered();
  const std::filesystem::path yaml_path = root / "source" / (session + ".yaml");
  write_budget_probe_graph(yaml_path);

  GraphLoadRequest request = make_budget_load_request(root, session);
  request.yaml_path = yaml_path.string();
  request.cache_root_dir = (root / "cache").string();
  return request;
}

/**
 * @brief Builds one multi-source physical worker probe Graph request.
 * @param root Temporary root whose lifetime encloses the Host lifecycle.
 * @param session Unique session label for one saturated Graph.
 * @return Public request pointing at an eight-source plus sink YAML Graph.
 * @throws std::filesystem::filesystem_error If fixture directories cannot be
 * created.
 * @throws std::bad_alloc If path, request, or callback storage allocation
 * fails.
 * @throws std::runtime_error If the YAML fixture cannot be written completely.
 * @note Every returned Graph has identical topology but isolated session and
 * cache paths.
 */
GraphLoadRequest make_parallel_budget_probe_load_request(
    const std::filesystem::path& root, const std::string& session) {
  ensure_budget_probe_operation_registered();
  const std::filesystem::path yaml_path = root / "source" / (session + ".yaml");
  write_parallel_budget_probe_graph(yaml_path);

  GraphLoadRequest request = make_budget_load_request(root, session);
  request.yaml_path = yaml_path.string();
  request.cache_root_dir = (root / "cache").string();
  return request;
}

/**
 * @brief Builds one forced parallel compute through the Graph's HP scheduler.
 * @param session Loaded real-callback Graph session.
 * @return Public compute request targeting the probe node.
 * @throws std::bad_alloc If precision storage allocation fails.
 * @note Cache reads and saves are disabled so every call reaches the operation
 * callback instead of accepting cached output as scheduler evidence.
 */
HostComputeRequest make_budget_probe_compute_request(
    const GraphSessionId& session) {
  HostComputeRequest request;
  request.session = session;
  request.node = NodeId{1};
  request.cache.precision = "fp32";
  request.cache.force_recache = true;
  request.cache.disable_disk_cache = true;
  request.cache.nosave = true;
  request.execution.parallel = true;
  request.intent = ComputeIntent::GlobalHighPrecision;
  return request;
}

/**
 * @brief Builds one forced HP compute targeting the parallel probe sink.
 * @param session Loaded multi-source physical worker probe session.
 * @return Public parallel request that forces all source callbacks to execute.
 * @throws std::bad_alloc If precision storage allocation fails.
 * @note Explicit HP intent excludes the separately admitted RT scheduler from
 * callback execution while both scheduler worker sets remain live and charged.
 */
HostComputeRequest make_parallel_budget_probe_compute_request(
    const GraphSessionId& session) {
  HostComputeRequest request = make_budget_probe_compute_request(session);
  request.node = NodeId{kWorkerProbeSinkId};
  return request;
}

/**
 * @brief Applies one symmetric Host scheduler configuration for future Graphs.
 * @param host Public Host whose defaults are updated transactionally.
 * @param scheduler_type Registered or built-in HP/RT scheduler type.
 * @param worker_count Exact request shared by both intent plans.
 * @return Host configuration result for explicit test assertions.
 * @throws std::bad_alloc If request/status storage cannot allocate.
 * @note The helper mutates only future-Graph defaults; retained Graphs keep
 * their existing schedulers and reservations.
 */
VoidResult configure_budget_scheduler(Host& host,
                                      const std::string& scheduler_type,
                                      unsigned int worker_count) {
  HostSchedulerConfig config;
  config.hp_type = scheduler_type;
  config.rt_type = scheduler_type;
  config.worker_count = worker_count;
  return host.configure_scheduler_defaults(config);
}

/**
 * @brief Expects a public Host result to carry exact budget exhaustion status.
 * @param result Failed Graph load result to inspect.
 * @return Nothing.
 * @throws GoogleTest assertion failures only.
 * @note The message is diagnostic; assertions branch only on stable domain,
 * code, and name fields.
 */
void expect_compute_budget_failure(const Result<GraphSessionId>& result) {
  EXPECT_FALSE(result.status.ok);
  EXPECT_EQ(result.status.domain, OperationErrorDomain::Graph);
  EXPECT_EQ(checked_graph_error_code(result.status), GraphErrc::ComputeError);
  EXPECT_EQ(result.status.name, "compute_error");
}

/**
 * @brief Expects scheduler replacement to preserve exact budget status.
 * @param result Failed public Host replacement result to inspect.
 * @return Nothing.
 * @throws GoogleTest assertion failures only.
 * @note Stable domain/code/name fields drive assertions; messages remain
 * diagnostic text and are never parsed.
 */
void expect_compute_budget_failure(const VoidResult& result) {
  EXPECT_FALSE(result.status.ok);
  EXPECT_EQ(result.status.domain, OperationErrorDomain::Graph);
  EXPECT_EQ(checked_graph_error_code(result.status), GraphErrc::ComputeError);
  EXPECT_EQ(result.status.name, "compute_error");
}

/**
 * @brief Expects one rejected session to be absent from public Host state.
 * @param host Public Host queried after a failed Graph load transaction.
 * @param session Rejected session label that must not be published.
 * @return Nothing.
 * @throws GoogleTest assertion failures only.
 * @note The helper inspects only copied public values and never reaches into
 * Kernel or GraphRuntime ownership.
 */
void expect_graph_absent(Host& host, const GraphSessionId& session) {
  const Result<std::vector<GraphSessionId>> listed = host.list_graphs();
  ASSERT_TRUE(listed.status.ok) << listed.status.message;
  EXPECT_TRUE(std::none_of(
      listed.value.begin(), listed.value.end(),
      [&session](const auto& item) { return item.value == session.value; }));
}

/**
 * @brief Writes syntactically valid YAML with an invalid Graph root shape.
 * @param path Destination source path created with its parent directory.
 * @return Nothing.
 * @throws std::filesystem::filesystem_error If parent creation fails.
 * @throws std::bad_alloc If path or stream state allocation fails.
 * @throws std::runtime_error If the destination cannot be written completely.
 * @note A mapping root deterministically reaches GraphIO validation and raises
 * GraphErrc::InvalidYaml after execution ownership has been configured.
 */
void write_invalid_graph_yaml(const std::filesystem::path& path) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream output(path);
  if (!output) {
    throw std::runtime_error("failed to open invalid Graph YAML fixture");
  }
  output << "nodes: []\n";
  output.close();
  if (!output) {
    throw std::runtime_error("failed to write invalid Graph YAML fixture");
  }
}

/**
 * @brief Proves the process ledger is neither leaked nor over-released.
 * @param host Public Host used for recovery and over-admission probes.
 * @param root Temporary root whose lifetime encloses every probe Graph.
 * @param prefix Unique label prefix for this failure scenario.
 * @return Nothing.
 * @throws std::bad_alloc If fixture registration, path/request construction,
 * or Host result allocation fails.
 * @throws GoogleTest assertion failures for contract mismatches.
 * @note Two workerless charged pairs first consume all thirty-two slots. A
 * subsequent one-slot HP plus zero-slot RT candidate must fail: inability to
 * fill detects leaks, while extra admission detects any duplicate release.
 */
void expect_exact_process_budget_recovery(Host& host,
                                          const std::filesystem::path& root,
                                          const std::string& prefix) {
  ensure_charged_serial_scheduler_registered();
  const VoidResult fill_config = configure_budget_scheduler(
      host, kChargedSerialSchedulerType, kSchedulerWorkerRequestMax);
  ASSERT_TRUE(fill_config.status.ok) << fill_config.status.message;

  const GraphSessionId first_id{prefix + "_fill_first"};
  const GraphSessionId second_id{prefix + "_fill_second"};
  const Result<GraphSessionId> first =
      host.load_graph(make_budget_load_request(root, first_id.value));
  ASSERT_TRUE(first.status.ok) << first.status.message;
  const Result<GraphSessionId> second =
      host.load_graph(make_budget_load_request(root, second_id.value));
  ASSERT_TRUE(second.status.ok) << second.status.message;

  HostSchedulerConfig one_slot_config;
  one_slot_config.hp_type = kChargedSerialSchedulerType;
  one_slot_config.rt_type = "serial_debug";
  one_slot_config.worker_count = 1U;
  const VoidResult probe_config =
      host.configure_scheduler_defaults(one_slot_config);
  ASSERT_TRUE(probe_config.status.ok) << probe_config.status.message;
  const GraphSessionId probe_id{prefix + "_over_release_probe"};
  const Result<GraphSessionId> probe =
      host.load_graph(make_budget_load_request(root, probe_id.value));
  expect_compute_budget_failure(probe);
  expect_graph_absent(host, probe_id);
  if (probe.status.ok) {
    EXPECT_TRUE(host.close_graph(probe_id).status.ok);
  }

  EXPECT_TRUE(host.close_graph(first_id).status.ok);
  EXPECT_TRUE(host.close_graph(second_id).status.ok);
}

/**
 * @brief Proves exactly two process worker slots remain available.
 * @param host Public Host used for exact-fill and over-admission probes.
 * @param root Temporary root whose lifetime encloses both probe Graphs.
 * @param prefix Unique label prefix for the current replacement scenario.
 * @return Nothing.
 * @throws std::bad_alloc If registration, request, or result allocation fails.
 * @throws GoogleTest assertion failures when the ledger is not exactly thirty.
 * @note The caller retains old Graphs expected to own thirty slots. Successful
 * two-slot admission detects a leaked candidate reservation; rejection of a
 * subsequent one-slot admission detects old-owner or duplicate release.
 */
void expect_exact_two_slots_remaining(Host& host,
                                      const std::filesystem::path& root,
                                      const std::string& prefix) {
  ensure_charged_serial_scheduler_registered();
  HostSchedulerConfig two_slot_config;
  two_slot_config.hp_type = kChargedSerialSchedulerType;
  two_slot_config.rt_type = "serial_debug";
  two_slot_config.worker_count = 2U;
  const VoidResult two_slot_configured =
      host.configure_scheduler_defaults(two_slot_config);
  ASSERT_TRUE(two_slot_configured.status.ok)
      << two_slot_configured.status.message;

  const GraphSessionId exact_fill_id{prefix + "_exact_fill"};
  const Result<GraphSessionId> exact_fill =
      host.load_graph(make_budget_load_request(root, exact_fill_id.value));
  ASSERT_TRUE(exact_fill.status.ok) << exact_fill.status.message;

  HostSchedulerConfig one_slot_config = two_slot_config;
  one_slot_config.worker_count = 1U;
  const VoidResult one_slot_configured =
      host.configure_scheduler_defaults(one_slot_config);
  ASSERT_TRUE(one_slot_configured.status.ok)
      << one_slot_configured.status.message;
  const GraphSessionId rejected_id{prefix + "_over_release_probe"};
  const Result<GraphSessionId> rejected =
      host.load_graph(make_budget_load_request(root, rejected_id.value));
  expect_compute_budget_failure(rejected);
  expect_graph_absent(host, rejected_id);
  if (rejected.status.ok) {
    EXPECT_TRUE(host.close_graph(rejected_id).status.ok);
  }

  EXPECT_TRUE(host.close_graph(exact_fill_id).status.ok);
}

/**
 * @brief Proves an invalid worker request cannot replace accepted defaults.
 *
 * @throws Nothing when Host status and scheduler ownership match the contract;
 *         GoogleTest records any mismatch.
 * @note The failing assertion precedes Graph load so the pre-fix implementation
 *       cannot attempt to create the deliberately oversized worker request.
 */
TEST(EmbeddedHostSchedulerLimits,
     RejectsAboveInstanceLimitWithoutChangingFutureDefaults) {
  ScopedSchedulerBudgetTempDir temp("photospider_scheduler_worker_limit_test");
  std::unique_ptr<Host> host = create_embedded_host();
  ASSERT_NE(host, nullptr);

  HostSchedulerConfig accepted;
  accepted.hp_type = "serial_debug";
  accepted.rt_type = "serial_debug";
  accepted.worker_count = 0U;
  const VoidResult accepted_result =
      host->configure_scheduler_defaults(accepted);
  ASSERT_TRUE(accepted_result.status.ok) << accepted_result.status.message;

  HostSchedulerConfig rejected;
  rejected.hp_type = "cpu_work_stealing";
  rejected.rt_type = "cpu_work_stealing";
  rejected.worker_count = kSchedulerWorkerRequestMax + 1U;
  const VoidResult rejected_result =
      host->configure_scheduler_defaults(rejected);
  ASSERT_FALSE(rejected_result.status.ok);
  EXPECT_EQ(rejected_result.status.domain, OperationErrorDomain::Graph);
  EXPECT_EQ(checked_graph_error_code(rejected_result.status),
            GraphErrc::InvalidParameter);
  EXPECT_EQ(rejected_result.status.name, "invalid_parameter");

  GraphLoadRequest load;
  load.session = GraphSessionId{"preserved_serial_defaults"};
  load.root_dir = (temp.root() / "sessions").string();
  const Result<GraphSessionId> loaded = host->load_graph(load);
  ASSERT_TRUE(loaded.status.ok) << loaded.status.message;

  const Result<SchedulerInfoSnapshot> hp =
      host->scheduler_info(load.session, ComputeIntent::GlobalHighPrecision);
  ASSERT_TRUE(hp.status.ok) << hp.status.message;
  EXPECT_EQ(hp.value.scheduler_name, "serial_debug");

  const Result<SchedulerInfoSnapshot> rt =
      host->scheduler_info(load.session, ComputeIntent::RealTimeUpdate);
  ASSERT_TRUE(rt.status.ok) << rt.status.message;
  EXPECT_EQ(rt.value.scheduler_name, "serial_debug");
}

/**
 * @brief Proves HP+RT capacity rejection happens before either construction.
 * @return Nothing.
 * @throws Nothing when public Host status and construction evidence agree;
 * GoogleTest records mismatches.
 * @note Charged workerless fixtures leave eight slots available before a
 * sixteen-slot candidate. This makes the current sequential reservation path
 * visibly construct only HP while an atomic pair implementation constructs
 * neither, without ever starting excess physical workers.
 */
TEST(EmbeddedHostSchedulerBudget,
     CombinedIntentRejectionConstructsNeitherScheduler) {
  ensure_charged_serial_scheduler_registered();
  ScopedSchedulerBudgetTempDir temp(
      "photospider_scheduler_atomic_pair_admission_test");
  std::unique_ptr<Host> host = create_embedded_host();
  ASSERT_NE(host, nullptr);

  VoidResult configured = configure_budget_scheduler(
      *host, kChargedSerialSchedulerType, kSchedulerWorkerRequestMax);
  ASSERT_TRUE(configured.status.ok) << configured.status.message;
  Result<GraphSessionId> first =
      host->load_graph(make_budget_load_request(temp.root(), "atomic_fill_16"));
  ASSERT_TRUE(first.status.ok) << first.status.message;

  configured =
      configure_budget_scheduler(*host, kChargedSerialSchedulerType, 4U);
  ASSERT_TRUE(configured.status.ok) << configured.status.message;
  Result<GraphSessionId> second =
      host->load_graph(make_budget_load_request(temp.root(), "atomic_fill_8"));
  ASSERT_TRUE(second.status.ok) << second.status.message;

  configured = configure_budget_scheduler(*host, kChargedSerialSchedulerType,
                                          kSchedulerWorkerRequestMax);
  ASSERT_TRUE(configured.status.ok) << configured.status.message;
  const unsigned int before =
      charged_serial_constructions()->load(std::memory_order_relaxed);
  const GraphSessionId rejected_id{"atomic_rejected_16"};
  const Result<GraphSessionId> rejected = host->load_graph(
      make_budget_load_request(temp.root(), rejected_id.value));
  expect_compute_budget_failure(rejected);
  EXPECT_EQ(charged_serial_constructions()->load(std::memory_order_relaxed),
            before);
  expect_graph_absent(*host, rejected_id);

  EXPECT_TRUE(host->close_graph(first.value).status.ok);
  EXPECT_TRUE(host->close_graph(second.value).status.ok);
}

/**
 * @brief Proves both plans resolve before either scheduler is constructed.
 * @return Nothing.
 * @throws Nothing when invalid type classification and publication rollback
 * agree with the contract; GoogleTest records mismatches.
 * @note A valid observable HP fixture paired with an unknown RT type exposes
 * the legacy sequential path, which constructs HP and publishes a partial
 * runtime instead of rejecting the complete intent pair.
 */
TEST(EmbeddedHostSchedulerBudget,
     UnknownRtTypeConstructsNeitherSchedulerAndPublishesNoGraph) {
  ensure_charged_serial_scheduler_registered();
  ScopedSchedulerBudgetTempDir temp(
      "photospider_scheduler_unknown_pair_plan_test");
  std::unique_ptr<Host> host = create_embedded_host();
  ASSERT_NE(host, nullptr);

  HostSchedulerConfig config;
  config.hp_type = kChargedSerialSchedulerType;
  config.rt_type = "issue_43_unknown_scheduler_type";
  config.worker_count = kSchedulerWorkerRequestMax;
  const VoidResult configured = host->configure_scheduler_defaults(config);
  ASSERT_TRUE(configured.status.ok) << configured.status.message;

  const GraphSessionId rejected_id{"unknown_rt_rejected"};
  const unsigned int before =
      charged_serial_constructions()->load(std::memory_order_relaxed);
  const Result<GraphSessionId> rejected = host->load_graph(
      make_budget_load_request(temp.root(), rejected_id.value));
  EXPECT_FALSE(rejected.status.ok);
  EXPECT_EQ(rejected.status.domain, OperationErrorDomain::Graph);
  EXPECT_EQ(checked_graph_error_code(rejected.status),
            GraphErrc::InvalidParameter);
  EXPECT_EQ(rejected.status.name, "invalid_parameter");
  EXPECT_EQ(charged_serial_constructions()->load(std::memory_order_relaxed),
            before);
  expect_graph_absent(*host, rejected_id);

  if (rejected.status.ok) {
    EXPECT_TRUE(host->close_graph(rejected_id).status.ok);
  }
}

/**
 * @brief Proves a planned factory null result rolls back the complete pair.
 * @return Nothing.
 * @throws Nothing when Host classification, publication, and capacity reuse
 * agree with the contract; GoogleTest records mismatches.
 * @note HP returns null before RT construction. Two subsequent sixteen-slot
 * Graphs must fill all thirty-two slots, proving both rejected reservations
 * were released rather than only the consumed HP share.
 */
TEST(EmbeddedHostSchedulerBudget,
     NullFactoryRejectsWithoutPublicationAndReturnsPairCapacity) {
  ensure_charged_serial_scheduler_registered();
  ensure_null_scheduler_registered();
  ScopedSchedulerBudgetTempDir temp(
      "photospider_scheduler_null_factory_pair_test");
  std::unique_ptr<Host> host = create_embedded_host();
  ASSERT_NE(host, nullptr);

  HostSchedulerConfig config;
  config.hp_type = kNullSchedulerType;
  config.rt_type = kChargedSerialSchedulerType;
  config.worker_count = kSchedulerWorkerRequestMax;
  const VoidResult configured = host->configure_scheduler_defaults(config);
  ASSERT_TRUE(configured.status.ok) << configured.status.message;

  const GraphSessionId rejected_id{"null_factory_rejected"};
  const unsigned int before =
      charged_serial_constructions()->load(std::memory_order_relaxed);
  const Result<GraphSessionId> rejected = host->load_graph(
      make_budget_load_request(temp.root(), rejected_id.value));
  EXPECT_FALSE(rejected.status.ok);
  EXPECT_EQ(rejected.status.domain, OperationErrorDomain::Graph);
  EXPECT_EQ(checked_graph_error_code(rejected.status),
            GraphErrc::InvalidParameter);
  EXPECT_EQ(rejected.status.name, "invalid_parameter");
  EXPECT_EQ(charged_serial_constructions()->load(std::memory_order_relaxed),
            before);
  expect_graph_absent(*host, rejected_id);

  const VoidResult charged = configure_budget_scheduler(
      *host, kChargedSerialSchedulerType, kSchedulerWorkerRequestMax);
  ASSERT_TRUE(charged.status.ok) << charged.status.message;
  const Result<GraphSessionId> first = host->load_graph(
      make_budget_load_request(temp.root(), "null_factory_recovery_first"));
  ASSERT_TRUE(first.status.ok) << first.status.message;
  const Result<GraphSessionId> second = host->load_graph(
      make_budget_load_request(temp.root(), "null_factory_recovery_second"));
  ASSERT_TRUE(second.status.ok) << second.status.message;

  EXPECT_TRUE(host->close_graph(first.value).status.ok);
  EXPECT_TRUE(host->close_graph(second.value).status.ok);
}

/**
 * @brief Proves CPU Graph loads create no per-Graph scheduler workers.
 * @return Nothing.
 * @throws Nothing when public load, route inspection, and construction probes
 * agree; GoogleTest records mismatches.
 * @note The legacy CPU scheduler hook would throw on the first owner worker.
 * Two successful Graph loads with zero hook entries therefore prove both HP
 * and RT bindings reuse the already configured process service.
 */
TEST(EmbeddedHostSchedulerBudget,
     BuiltInCpuGraphLoadsCreateNoPerGraphSchedulers) {
  ScopedSchedulerBudgetTempDir temp("photospider_shared_cpu_graph_load_test");
  std::unique_ptr<Host> host = create_embedded_host();
  ASSERT_NE(host, nullptr);

  const VoidResult configured =
      configure_budget_scheduler(*host, "cpu_work_stealing", 1U);
  ASSERT_TRUE(configured.status.ok) << configured.status.message;
  CpuWorkerCreationFailureProbe failure_probe;
  const testing::SchedulerFailureInjectionHook failure_hook{
      &failure_probe, fail_first_cpu_worker_creation};
  Result<GraphSessionId> first;
  Result<GraphSessionId> second;
  {
    ScopedBorrowedSchedulerHook<testing::SchedulerFailureInjectionHook>
        failure_guard(testing::set_cpu_scheduler_failure_injection_hook,
                      &failure_hook);
    first = host->load_graph(
        make_budget_load_request(temp.root(), "shared_cpu_first"));
    second = host->load_graph(
        make_budget_load_request(temp.root(), "shared_cpu_second"));
  }

  ASSERT_TRUE(first.status.ok) << first.status.message;
  ASSERT_TRUE(second.status.ok) << second.status.message;
  EXPECT_EQ(failure_probe.entrances.load(std::memory_order_relaxed), 0U);
  for (const GraphSessionId& session : {first.value, second.value}) {
    const Result<SchedulerInfoSnapshot> hp =
        host->scheduler_info(session, ComputeIntent::GlobalHighPrecision);
    ASSERT_TRUE(hp.status.ok) << hp.status.message;
    EXPECT_EQ(hp.value.scheduler_name, "CpuWorkStealingScheduler");
    const Result<SchedulerInfoSnapshot> rt =
        host->scheduler_info(session, ComputeIntent::RealTimeUpdate);
    ASSERT_TRUE(rt.status.ok) << rt.status.message;
    EXPECT_EQ(rt.value.scheduler_name, "CpuWorkStealingScheduler");
    EXPECT_TRUE(host->close_graph(session).status.ok);
  }
}

/**
 * @brief Proves a fresh invalid load retains only the first fixed CPU pool.
 * @return Nothing.
 * @throws std::bad_alloc If fixture, Host, path, scheduler, or result storage
 * allocation fails.
 * @throws std::filesystem::filesystem_error If fixture directory creation
 * fails.
 * @throws std::runtime_error If the deterministic YAML fixture cannot be
 * written completely.
 * @note The invalid document is the first CPU-selecting load on a default Host.
 * Zero reconfiguration must remain reusable, while at least one unequal
 * positive request must conflict with the resolved fixed count. A legacy CPU
 * construction hook guards both failed and valid loads, proving neither owns a
 * per-Graph built-in CPU scheduler.
 */
TEST(EmbeddedHostSchedulerBudget,
     FreshInvalidYamlLoadRetainsFirstFixedPoolWithoutPerGraphOwner) {
  ScopedSchedulerBudgetTempDir temp(
      "photospider_invalid_yaml_shared_pool_test");
  std::unique_ptr<Host> host = create_embedded_host();
  ASSERT_NE(host, nullptr);

  const GraphSessionId rejected_id{"invalid_yaml_rejected"};
  GraphLoadRequest request =
      make_budget_load_request(temp.root(), rejected_id.value);
  const std::filesystem::path yaml_path =
      temp.root() / "source" / "invalid_graph.yaml";
  write_invalid_graph_yaml(yaml_path);
  request.yaml_path = yaml_path.string();

  CpuWorkerCreationFailureProbe worker_probe;
  const testing::SchedulerFailureInjectionHook worker_hook{
      &worker_probe, fail_first_cpu_worker_creation};
  {
    ScopedBorrowedSchedulerHook<testing::SchedulerFailureInjectionHook> guard(
        testing::set_cpu_scheduler_failure_injection_hook, &worker_hook);
    const Result<GraphSessionId> rejected = host->load_graph(request);

    EXPECT_FALSE(rejected.status.ok);
    EXPECT_EQ(rejected.status.domain, OperationErrorDomain::Graph);
    EXPECT_EQ(checked_graph_error_code(rejected.status),
              GraphErrc::InvalidYaml);
    EXPECT_EQ(rejected.status.name, "invalid_yaml");
    expect_graph_absent(*host, rejected_id);

    const VoidResult first_same =
        configure_budget_scheduler(*host, "cpu_work_stealing", 0U);
    ASSERT_TRUE(first_same.status.ok) << first_same.status.message;
    const VoidResult second_same =
        configure_budget_scheduler(*host, "cpu_work_stealing", 0U);
    ASSERT_TRUE(second_same.status.ok) << second_same.status.message;

    const VoidResult positive_one =
        configure_budget_scheduler(*host, "cpu_work_stealing", 1U);
    if (positive_one.status.ok) {
      const VoidResult repeated_one =
          configure_budget_scheduler(*host, "cpu_work_stealing", 1U);
      ASSERT_TRUE(repeated_one.status.ok) << repeated_one.status.message;
      const VoidResult conflicting_two =
          configure_budget_scheduler(*host, "cpu_work_stealing", 2U);
      EXPECT_FALSE(conflicting_two.status.ok);
      EXPECT_EQ(conflicting_two.status.domain, OperationErrorDomain::Graph);
      EXPECT_EQ(checked_graph_error_code(conflicting_two.status),
                GraphErrc::InvalidParameter);
      EXPECT_EQ(conflicting_two.status.name, "invalid_parameter");
    } else {
      EXPECT_EQ(positive_one.status.domain, OperationErrorDomain::Graph);
      EXPECT_EQ(checked_graph_error_code(positive_one.status),
                GraphErrc::InvalidParameter);
      EXPECT_EQ(positive_one.status.name, "invalid_parameter");
    }

    const GraphSessionId valid_id{"valid_after_invalid_yaml"};
    const Result<GraphSessionId> valid =
        host->load_graph(make_budget_load_request(temp.root(), valid_id.value));
    ASSERT_TRUE(valid.status.ok) << valid.status.message;
    EXPECT_TRUE(host->close_graph(valid_id).status.ok);
  }
  EXPECT_EQ(worker_probe.entrances.load(std::memory_order_relaxed), 0U);
}

/**
 * @brief Proves saturation, close recovery, and zero-slot serial admission.
 * @return Nothing.
 * @throws Nothing when public Host lifecycle results match the fixed budget.
 * @note The charged fixture fills all thirty-two slots without physical
 * workers, preventing a pre-fix test from creating over-budget threads.
 */
TEST(EmbeddedHostSchedulerBudget,
     SaturationRejectsThenCloseRecoversWhileSerialRemainsAdmissible) {
  ensure_charged_serial_scheduler_registered();
  ScopedSchedulerBudgetTempDir temp(
      "photospider_scheduler_saturation_recovery_test");
  std::unique_ptr<Host> host = create_embedded_host();
  ASSERT_NE(host, nullptr);

  VoidResult configured = configure_budget_scheduler(
      *host, kChargedSerialSchedulerType, kSchedulerWorkerRequestMax);
  ASSERT_TRUE(configured.status.ok) << configured.status.message;
  const GraphSessionId first_id{"saturation_fill_first"};
  const GraphSessionId second_id{"saturation_fill_second"};
  Result<GraphSessionId> first =
      host->load_graph(make_budget_load_request(temp.root(), first_id.value));
  ASSERT_TRUE(first.status.ok) << first.status.message;
  Result<GraphSessionId> second =
      host->load_graph(make_budget_load_request(temp.root(), second_id.value));
  ASSERT_TRUE(second.status.ok) << second.status.message;

  configured =
      configure_budget_scheduler(*host, kChargedSerialSchedulerType, 1U);
  ASSERT_TRUE(configured.status.ok) << configured.status.message;
  const GraphSessionId retry_id{"saturation_positive_retry"};
  Result<GraphSessionId> saturated =
      host->load_graph(make_budget_load_request(temp.root(), retry_id.value));
  expect_compute_budget_failure(saturated);

  configured = configure_budget_scheduler(*host, "serial_debug", 0U);
  ASSERT_TRUE(configured.status.ok) << configured.status.message;
  const GraphSessionId serial_id{"saturation_zero_slot_serial"};
  Result<GraphSessionId> serial =
      host->load_graph(make_budget_load_request(temp.root(), serial_id.value));
  ASSERT_TRUE(serial.status.ok) << serial.status.message;

  const VoidResult closed_first = host->close_graph(first_id);
  ASSERT_TRUE(closed_first.status.ok) << closed_first.status.message;
  configured = configure_budget_scheduler(*host, kChargedSerialSchedulerType,
                                          kSchedulerWorkerRequestMax);
  ASSERT_TRUE(configured.status.ok) << configured.status.message;
  Result<GraphSessionId> recovered =
      host->load_graph(make_budget_load_request(temp.root(), retry_id.value));
  ASSERT_TRUE(recovered.status.ok) << recovered.status.message;

  EXPECT_TRUE(host->close_graph(second_id).status.ok);
  EXPECT_TRUE(host->close_graph(serial_id).status.ok);
  EXPECT_TRUE(host->close_graph(retry_id).status.ok);
}

/**
 * @brief Proves independent embedded Hosts share one process admission ledger.
 * @return Nothing.
 * @throws std::bad_alloc If Host, path, request, or result allocation fails.
 * @throws GoogleTest assertion failures when cross-Host rejection or
 * destructor-time release violates the contract.
 * @note Each Host first owns one sixteen-slot Graph. The observer can still
 * admit serial work but not one positive slot. Destroying the owner lets the
 * observer immediately replace its sixteen slots; after explicit observer
 * closes, exact-ledger probes require all thirty-two slots to be reusable and
 * reject any thirty-third slot.
 */
TEST(EmbeddedHostSchedulerBudget,
     IndependentHostsShareLimitAndDestructionReturnsAllCapacity) {
  ensure_charged_serial_scheduler_registered();
  ScopedSchedulerBudgetTempDir temp(
      "photospider_multi_host_process_budget_test");
  std::unique_ptr<Host> owner = create_embedded_host();
  std::unique_ptr<Host> observer = create_embedded_host();
  ASSERT_NE(owner, nullptr);
  ASSERT_NE(observer, nullptr);

  const VoidResult owner_config = configure_budget_scheduler(
      *owner, kChargedSerialSchedulerType, kSchedulerWorkerRequestMax);
  ASSERT_TRUE(owner_config.status.ok) << owner_config.status.message;
  const Result<GraphSessionId> owner_first = owner->load_graph(
      make_budget_load_request(temp.root(), "multi_host_owner_first"));
  ASSERT_TRUE(owner_first.status.ok) << owner_first.status.message;

  const VoidResult observer_fill_config = configure_budget_scheduler(
      *observer, kChargedSerialSchedulerType, kSchedulerWorkerRequestMax);
  ASSERT_TRUE(observer_fill_config.status.ok)
      << observer_fill_config.status.message;
  const GraphSessionId observer_first_id{"multi_host_observer_first"};
  const Result<GraphSessionId> observer_first = observer->load_graph(
      make_budget_load_request(temp.root(), observer_first_id.value));
  ASSERT_TRUE(observer_first.status.ok) << observer_first.status.message;

  HostSchedulerConfig positive_probe_config;
  positive_probe_config.hp_type = kChargedSerialSchedulerType;
  positive_probe_config.rt_type = "serial_debug";
  positive_probe_config.worker_count = 1U;
  const VoidResult positive_config =
      observer->configure_scheduler_defaults(positive_probe_config);
  ASSERT_TRUE(positive_config.status.ok) << positive_config.status.message;
  const GraphSessionId rejected_id{"multi_host_observer_rejected"};
  const Result<GraphSessionId> rejected = observer->load_graph(
      make_budget_load_request(temp.root(), rejected_id.value));
  expect_compute_budget_failure(rejected);
  expect_graph_absent(*observer, rejected_id);
  if (rejected.status.ok) {
    EXPECT_TRUE(observer->close_graph(rejected_id).status.ok);
  }

  const VoidResult serial_config =
      configure_budget_scheduler(*observer, "serial_debug", 0U);
  ASSERT_TRUE(serial_config.status.ok) << serial_config.status.message;
  const GraphSessionId serial_id{"multi_host_observer_serial"};
  const Result<GraphSessionId> serial = observer->load_graph(
      make_budget_load_request(temp.root(), serial_id.value));
  ASSERT_TRUE(serial.status.ok) << serial.status.message;

  owner.reset();
  const VoidResult recovery_config = configure_budget_scheduler(
      *observer, kChargedSerialSchedulerType, kSchedulerWorkerRequestMax);
  ASSERT_TRUE(recovery_config.status.ok) << recovery_config.status.message;
  const GraphSessionId observer_recovered_id{
      "multi_host_observer_after_owner_destroy"};
  const Result<GraphSessionId> observer_recovered = observer->load_graph(
      make_budget_load_request(temp.root(), observer_recovered_id.value));
  ASSERT_TRUE(observer_recovered.status.ok)
      << observer_recovered.status.message;

  EXPECT_TRUE(observer->close_graph(observer_first_id).status.ok);
  EXPECT_TRUE(observer->close_graph(observer_recovered_id).status.ok);
  ASSERT_NO_FATAL_FAILURE(expect_exact_process_budget_recovery(
      *observer, temp.root(), "multi_host_destructor_recovery"));
  EXPECT_TRUE(observer->close_graph(serial_id).status.ok);
}

/**
 * @brief Proves saturated replacement requires candidate headroom.
 * @return Nothing.
 * @throws std::bad_alloc If fixture, Host, scheduler, or result allocation
 * fails.
 * @throws GoogleTest assertion failures when status, construction, ownership,
 * or real compute behavior violates the strong replacement contract.
 * @note Two charged workerless Graphs fill all thirty-two slots. The one-slot
 * CPU candidate must be rejected before worker creation while the target's
 * prior serial scheduler remains published and executes one forced callback
 * both before and after the failed replacement. Closing the filler then makes
 * the same CPU replacement succeed, distinguishing exhaustion from bad type.
 */
TEST(EmbeddedHostSchedulerBudget,
     SaturatedReplacementRequiresHeadroomAndPreservesOldCompute) {
  ensure_charged_serial_scheduler_registered();
  ScopedSchedulerBudgetTempDir temp(
      "photospider_saturated_scheduler_replacement_test");
  std::unique_ptr<Host> host = create_embedded_host();
  ASSERT_NE(host, nullptr);

  const VoidResult charged_config = configure_budget_scheduler(
      *host, kChargedSerialSchedulerType, kSchedulerWorkerRequestMax);
  ASSERT_TRUE(charged_config.status.ok) << charged_config.status.message;
  const GraphSessionId target_id{"saturated_replacement_target"};
  const Result<GraphSessionId> target = host->load_graph(
      make_budget_probe_load_request(temp.root(), target_id.value));
  ASSERT_TRUE(target.status.ok) << target.status.message;
  const GraphSessionId filler_id{"saturated_replacement_filler"};
  const Result<GraphSessionId> filler =
      host->load_graph(make_budget_load_request(temp.root(), filler_id.value));
  ASSERT_TRUE(filler.status.ok) << filler.status.message;

  BudgetOperationProbe operation_probe;
  ScopedBudgetOperationProbe operation_guard(operation_probe);
  const HostComputeRequest compute_request =
      make_budget_probe_compute_request(target_id);
  const VoidResult before_compute = host->compute(compute_request);
  ASSERT_TRUE(before_compute.status.ok) << before_compute.status.message;
  const std::vector<std::thread::id> before_threads =
      operation_probe.callback_threads();
  ASSERT_EQ(before_threads.size(), 1U);

  const VoidResult unknown =
      host->replace_scheduler(target_id, ComputeIntent::GlobalHighPrecision,
                              "issue_43_missing_replacement_type");
  EXPECT_FALSE(unknown.status.ok);
  EXPECT_EQ(unknown.status.domain, OperationErrorDomain::Graph);
  EXPECT_EQ(checked_graph_error_code(unknown.status),
            GraphErrc::InvalidParameter);
  EXPECT_EQ(unknown.status.name, "invalid_parameter");

  HostSchedulerConfig replacement_defaults;
  replacement_defaults.hp_type = kChargedSerialSchedulerType;
  replacement_defaults.rt_type = "serial_debug";
  replacement_defaults.worker_count = 1U;
  const VoidResult replacement_config =
      host->configure_scheduler_defaults(replacement_defaults);
  ASSERT_TRUE(replacement_config.status.ok)
      << replacement_config.status.message;

  CpuWorkerCreationFailureProbe worker_probe;
  const testing::SchedulerFailureInjectionHook worker_hook{
      &worker_probe, count_cpu_worker_creation};
  VoidResult replaced;
  {
    ScopedBorrowedSchedulerHook<testing::SchedulerFailureInjectionHook> guard(
        testing::set_cpu_scheduler_failure_injection_hook, &worker_hook);
    replaced = host->replace_scheduler(
        target_id, ComputeIntent::GlobalHighPrecision, "cpu_work_stealing");
  }

  expect_compute_budget_failure(replaced);
  EXPECT_EQ(worker_probe.entrances.load(std::memory_order_relaxed), 0U);
  const Result<SchedulerInfoSnapshot> retained =
      host->scheduler_info(target_id, ComputeIntent::GlobalHighPrecision);
  ASSERT_TRUE(retained.status.ok) << retained.status.message;
  EXPECT_EQ(retained.value.scheduler_name, "serial_debug");

  operation_probe.reset();
  const VoidResult after_compute = host->compute(compute_request);
  ASSERT_TRUE(after_compute.status.ok) << after_compute.status.message;
  const std::vector<std::thread::id> after_threads =
      operation_probe.callback_threads();
  ASSERT_EQ(after_threads.size(), 1U);

  const VoidResult closed_filler = host->close_graph(filler_id);
  ASSERT_TRUE(closed_filler.status.ok) << closed_filler.status.message;
  const VoidResult retried = host->replace_scheduler(
      target_id, ComputeIntent::GlobalHighPrecision, "cpu_work_stealing");
  ASSERT_TRUE(retried.status.ok) << retried.status.message;
  const Result<SchedulerInfoSnapshot> replaced_info =
      host->scheduler_info(target_id, ComputeIntent::GlobalHighPrecision);
  ASSERT_TRUE(replaced_info.status.ok) << replaced_info.status.message;
  EXPECT_EQ(replaced_info.value.scheduler_name, "CpuWorkStealingScheduler");
  EXPECT_TRUE(host->close_graph(target_id).status.ok);
}

/**
 * @brief Preserves the established handled category for candidate GraphError.
 * @return Nothing.
 * @throws std::bad_alloc If fixture, Host, scheduler, or result allocation
 * fails.
 * @throws GoogleTest assertion failures when issue #43 budget propagation
 * accidentally exposes an unrelated candidate GraphError category.
 * @note A registered one-slot factory raises Graph NotFound after admission.
 * Kernel must retain its historical false result so Host reports
 * InvalidParameter, while the old serial scheduler stays usable and complete
 * process capacity is immediately recoverable.
 */
TEST(EmbeddedHostSchedulerBudget,
     CandidateGraphErrorRetainsHandledInvalidParameterMapping) {
  ensure_graph_error_scheduler_registered();
  ScopedSchedulerBudgetTempDir temp(
      "photospider_candidate_graph_error_replacement_test");
  std::unique_ptr<Host> host = create_embedded_host();
  ASSERT_NE(host, nullptr);

  const VoidResult serial_config =
      configure_budget_scheduler(*host, "serial_debug", 0U);
  ASSERT_TRUE(serial_config.status.ok) << serial_config.status.message;
  const GraphSessionId target_id{"candidate_graph_error_target"};
  const Result<GraphSessionId> target = host->load_graph(
      make_budget_probe_load_request(temp.root(), target_id.value));
  ASSERT_TRUE(target.status.ok) << target.status.message;

  HostSchedulerConfig replacement_defaults;
  replacement_defaults.hp_type = kGraphErrorSchedulerType;
  replacement_defaults.rt_type = "serial_debug";
  replacement_defaults.worker_count = 1U;
  const VoidResult configured =
      host->configure_scheduler_defaults(replacement_defaults);
  ASSERT_TRUE(configured.status.ok) << configured.status.message;
  const unsigned int before_constructions =
      graph_error_scheduler_constructions()->load(std::memory_order_relaxed);
  const VoidResult replaced = host->replace_scheduler(
      target_id, ComputeIntent::GlobalHighPrecision, kGraphErrorSchedulerType);
  EXPECT_FALSE(replaced.status.ok);
  EXPECT_EQ(replaced.status.domain, OperationErrorDomain::Graph);
  EXPECT_EQ(checked_graph_error_code(replaced.status),
            GraphErrc::InvalidParameter);
  EXPECT_EQ(replaced.status.name, "invalid_parameter");
  EXPECT_EQ(
      graph_error_scheduler_constructions()->load(std::memory_order_relaxed),
      before_constructions + 1U);

  const Result<SchedulerInfoSnapshot> retained =
      host->scheduler_info(target_id, ComputeIntent::GlobalHighPrecision);
  ASSERT_TRUE(retained.status.ok) << retained.status.message;
  EXPECT_EQ(retained.value.scheduler_name, "serial_debug");
  BudgetOperationProbe operation_probe;
  ScopedBudgetOperationProbe operation_guard(operation_probe);
  const VoidResult retained_compute =
      host->compute(make_budget_probe_compute_request(target_id));
  ASSERT_TRUE(retained_compute.status.ok) << retained_compute.status.message;
  ASSERT_EQ(operation_probe.callback_threads().size(), 1U);

  ASSERT_NO_FATAL_FAILURE(expect_exact_process_budget_recovery(
      *host, temp.root(), "candidate_graph_error_recovery"));
  EXPECT_TRUE(host->close_graph(target_id).status.ok);
}

/**
 * @brief Proves a null replacement candidate returns only its admitted slot.
 * @return Nothing.
 * @throws std::bad_alloc If fixture, Host, scheduler, or result allocation
 * fails.
 * @throws GoogleTest assertion failures when category, construction evidence,
 * old behavior, or exact remaining capacity violates the contract.
 * @note Retained Graphs charge thirty slots. The registered factory invocation
 * proves one candidate slot was admitted before it returned null; exact-fill
 * probes then distinguish candidate leakage from any old-owner over-release.
 */
TEST(EmbeddedHostSchedulerBudget,
     NullReplacementCandidateReturnsOnlyCandidateCapacity) {
  ensure_charged_serial_scheduler_registered();
  ensure_null_scheduler_registered();
  ScopedSchedulerBudgetTempDir temp(
      "photospider_null_scheduler_replacement_candidate_test");
  std::unique_ptr<Host> host = create_embedded_host();
  ASSERT_NE(host, nullptr);

  VoidResult configured = configure_budget_scheduler(
      *host, kChargedSerialSchedulerType, kSchedulerWorkerRequestMax);
  ASSERT_TRUE(configured.status.ok) << configured.status.message;
  const GraphSessionId target_id{"null_candidate_target"};
  const Result<GraphSessionId> target = host->load_graph(
      make_budget_probe_load_request(temp.root(), target_id.value));
  ASSERT_TRUE(target.status.ok) << target.status.message;

  configured =
      configure_budget_scheduler(*host, kChargedSerialSchedulerType, 7U);
  ASSERT_TRUE(configured.status.ok) << configured.status.message;
  const GraphSessionId filler_id{"null_candidate_filler"};
  const Result<GraphSessionId> filler =
      host->load_graph(make_budget_load_request(temp.root(), filler_id.value));
  ASSERT_TRUE(filler.status.ok) << filler.status.message;

  BudgetOperationProbe operation_probe;
  ScopedBudgetOperationProbe operation_guard(operation_probe);
  const HostComputeRequest compute_request =
      make_budget_probe_compute_request(target_id);
  const VoidResult before_compute = host->compute(compute_request);
  ASSERT_TRUE(before_compute.status.ok) << before_compute.status.message;
  ASSERT_EQ(operation_probe.callback_threads().size(), 1U);

  HostSchedulerConfig replacement_defaults;
  replacement_defaults.hp_type = kChargedSerialSchedulerType;
  replacement_defaults.rt_type = "serial_debug";
  replacement_defaults.worker_count = 1U;
  configured = host->configure_scheduler_defaults(replacement_defaults);
  ASSERT_TRUE(configured.status.ok) << configured.status.message;
  const unsigned int before_constructions =
      null_scheduler_constructions()->load(std::memory_order_relaxed);
  const VoidResult replaced = host->replace_scheduler(
      target_id, ComputeIntent::GlobalHighPrecision, kNullSchedulerType);
  EXPECT_FALSE(replaced.status.ok);
  EXPECT_EQ(replaced.status.domain, OperationErrorDomain::Graph);
  EXPECT_EQ(checked_graph_error_code(replaced.status),
            GraphErrc::InvalidParameter);
  EXPECT_EQ(replaced.status.name, "invalid_parameter");
  EXPECT_EQ(null_scheduler_constructions()->load(std::memory_order_relaxed),
            before_constructions + 1U);

  const Result<SchedulerInfoSnapshot> retained =
      host->scheduler_info(target_id, ComputeIntent::GlobalHighPrecision);
  ASSERT_TRUE(retained.status.ok) << retained.status.message;
  EXPECT_EQ(retained.value.scheduler_name, "serial_debug");
  operation_probe.reset();
  const VoidResult retained_compute = host->compute(compute_request);
  ASSERT_TRUE(retained_compute.status.ok) << retained_compute.status.message;
  ASSERT_EQ(operation_probe.callback_threads().size(), 1U);

  ASSERT_NO_FATAL_FAILURE(expect_exact_two_slots_remaining(
      *host, temp.root(), "null_candidate_recovery"));
  EXPECT_TRUE(host->close_graph(filler_id).status.ok);
  EXPECT_TRUE(host->close_graph(target_id).status.ok);
}

/**
 * @brief Proves CPU replacement publishes an ownerless service binding.
 * @return Nothing.
 * @throws std::bad_alloc From ordinary fixture, Host, callback, or result
 * allocation.
 * @throws GoogleTest assertion failures when replacement constructs a legacy
 * CPU owner, fails to execute through the service, or permits pool resizing.
 * @note The legacy CPU worker hook throws on first construction. Successful
 * replacement with zero hook entries proves the existing serial owner is
 * replaced by a process-service route rather than another scheduler object.
 */
TEST(EmbeddedHostSchedulerBudget,
     ReplacementPublishesOwnerlessFixedCpuServiceBinding) {
  ScopedSchedulerBudgetTempDir temp(
      "photospider_ownerless_cpu_replacement_test");
  std::unique_ptr<Host> host = create_embedded_host();
  ASSERT_NE(host, nullptr);

  VoidResult configured = configure_budget_scheduler(*host, "serial_debug", 0U);
  ASSERT_TRUE(configured.status.ok) << configured.status.message;
  const GraphSessionId target_id{"ownerless_cpu_replacement_target"};
  const Result<GraphSessionId> target = host->load_graph(
      make_budget_probe_load_request(temp.root(), target_id.value));
  ASSERT_TRUE(target.status.ok) << target.status.message;

  BudgetOperationProbe operation_probe;
  ScopedBudgetOperationProbe operation_guard(operation_probe);
  const HostComputeRequest compute_request =
      make_budget_probe_compute_request(target_id);
  const VoidResult before_compute = host->compute(compute_request);
  ASSERT_TRUE(before_compute.status.ok) << before_compute.status.message;
  ASSERT_EQ(operation_probe.callback_threads().size(), 1U);

  HostSchedulerConfig replacement_defaults;
  replacement_defaults.hp_type = "serial_debug";
  replacement_defaults.rt_type = "serial_debug";
  replacement_defaults.worker_count = 1U;
  configured = host->configure_scheduler_defaults(replacement_defaults);
  ASSERT_TRUE(configured.status.ok) << configured.status.message;

  CpuWorkerCreationFailureProbe worker_probe;
  const testing::SchedulerFailureInjectionHook worker_hook{
      &worker_probe, fail_first_cpu_worker_creation};
  {
    ScopedBorrowedSchedulerHook<testing::SchedulerFailureInjectionHook> guard(
        testing::set_cpu_scheduler_failure_injection_hook, &worker_hook);
    const VoidResult replaced = host->replace_scheduler(
        target_id, ComputeIntent::GlobalHighPrecision, "cpu_work_stealing");
    ASSERT_TRUE(replaced.status.ok) << replaced.status.message;
  }
  EXPECT_EQ(worker_probe.entrances.load(std::memory_order_relaxed), 0U);

  const Result<SchedulerInfoSnapshot> replaced_info =
      host->scheduler_info(target_id, ComputeIntent::GlobalHighPrecision);
  ASSERT_TRUE(replaced_info.status.ok) << replaced_info.status.message;
  EXPECT_EQ(replaced_info.value.scheduler_name, "CpuWorkStealingScheduler");
  operation_probe.reset();
  const std::thread::id caller_thread = std::this_thread::get_id();
  const VoidResult service_compute = host->compute(compute_request);
  ASSERT_TRUE(service_compute.status.ok) << service_compute.status.message;
  const std::vector<std::thread::id> service_threads =
      operation_probe.callback_threads();
  ASSERT_EQ(service_threads.size(), 1U);
  EXPECT_NE(service_threads.front(), caller_thread);

  const VoidResult conflicting =
      configure_budget_scheduler(*host, "cpu_work_stealing", 2U);
  EXPECT_FALSE(conflicting.status.ok);
  EXPECT_EQ(conflicting.status.domain, OperationErrorDomain::Graph);
  EXPECT_EQ(checked_graph_error_code(conflicting.status),
            GraphErrc::InvalidParameter);
  EXPECT_TRUE(host->close_graph(target_id).status.ok);
}

/**
 * @brief Proves multiple Graph Runs overlap on one fixed built-in CPU pool.
 * @return Nothing.
 * @throws std::bad_alloc If Host, Graph, scheduler, callback, future, or result
 * storage allocation fails.
 * @throws std::system_error If probe or scheduler synchronization fails.
 * @throws std::future_error If asynchronous result ownership is invalid.
 * @throws GoogleTest assertion failures when public product execution exceeds
 * its exact fixed process worker grant.
 * @note Four Graphs belong to one Host and each exposes one blocking source.
 * Observing four simultaneous callbacks proves four independent Runs share the
 * same four-worker service without a complete-Run gate. A bounded half-second
 * observation rejects a fifth callback while all workers remain blocked.
 * Evidence is collected only inside registered operation callbacks; scheduler
 * statistics, internal worker ids, and OS thread snapshots are never consulted.
 */
TEST(EmbeddedHostSchedulerBudget,
     SharedBuiltInWorkersOverlapMultipleGraphRunsWithinFixedGrant) {
  ScopedSchedulerBudgetTempDir temp(
      "photospider_physical_scheduler_worker_bound_test");
  BudgetOperationProbe operation_probe;
  ScopedBudgetOperationProbe operation_guard(operation_probe);
  std::unique_ptr<Host> host = create_embedded_host();
  ASSERT_NE(host, nullptr);
  const VoidResult configured =
      configure_budget_scheduler(*host, "cpu_work_stealing", kWorkerProbeGrant);
  ASSERT_TRUE(configured.status.ok) << configured.status.message;
  std::vector<GraphSessionId> sessions;
  sessions.reserve(kWorkerProbeGraphCount);
  for (std::size_t index = 0U; index < kWorkerProbeGraphCount; ++index) {
    const GraphSessionId session{"physical_worker_probe_" +
                                 std::to_string(index)};
    const Result<GraphSessionId> loaded = host->load_graph(
        make_parallel_budget_probe_load_request(temp.root(), session.value));
    ASSERT_TRUE(loaded.status.ok) << loaded.status.message;
    sessions.push_back(session);
  }

  std::vector<std::future<OperationStatus>> compute_futures;
  compute_futures.reserve(kWorkerProbeGraphCount);
  ScopedBudgetCallbackBlock callback_block(operation_probe);
  bool every_compute_scheduled = true;
  for (std::size_t index = 0U; index < sessions.size(); ++index) {
    const HostComputeRequest request =
        make_parallel_budget_probe_compute_request(sessions[index]);
    Result<std::future<OperationStatus>> scheduled =
        host->compute_async(request);
    if (!scheduled.status.ok) {
      ADD_FAILURE() << scheduled.status.message;
      every_compute_scheduled = false;
      break;
    }
    compute_futures.push_back(std::move(scheduled.value));
  }
  if (!every_compute_scheduled) {
    callback_block.release();
    return;
  }

  const bool reached_exact_grant = operation_probe.wait_for_active_callbacks(
      kWorkerProbeActive, std::chrono::seconds(5));
  if (!reached_exact_grant) {
    ADD_FAILURE() << "admitted HP workers did not enter all blocked callbacks";
  } else {
    const bool exceeded_grant = operation_probe.wait_for_active_callbacks(
        kWorkerProbeActive + 1U, std::chrono::milliseconds(500));
    EXPECT_FALSE(exceeded_grant);
    EXPECT_EQ(operation_probe.active_callback_count(), kWorkerProbeActive);
    EXPECT_EQ(operation_probe.max_active_callback_count(), kWorkerProbeActive);
    EXPECT_EQ(operation_probe.callback_count(), kWorkerProbeActive);
  }

  callback_block.release();
  for (std::future<OperationStatus>& compute_future : compute_futures) {
    const OperationStatus computed = compute_future.get();
    EXPECT_TRUE(computed.ok) << computed.message;
  }

  EXPECT_EQ(operation_probe.active_callback_count(), 0U);
  EXPECT_EQ(operation_probe.max_active_callback_count(), kWorkerProbeActive);
  EXPECT_EQ(operation_probe.callback_count(),
            kWorkerProbeGraphCount * kWorkerProbeSources);
  EXPECT_EQ(operation_probe.unique_callback_thread_count(), kWorkerProbeActive);

  for (const GraphSessionId& session : sessions) {
    const VoidResult closed = host->close_graph(session);
    EXPECT_TRUE(closed.status.ok) << closed.status.message;
  }
}

}  // namespace
}  // namespace ps
