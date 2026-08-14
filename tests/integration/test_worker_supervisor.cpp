/**
 * @file test_worker_supervisor.cpp
 * @brief Verifies Issue #100 real-process crash isolation and bounded
 * lifecycle.
 */
#include <fcntl.h>
#include <gtest/gtest.h>
#include <signal.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include "server/single_tenant_job_service.hpp"  // NOLINT(build/include_subdir)
#include "server/single_tenant_job_service_test_access.hpp"  // NOLINT(build/include_subdir)
#include "server/worker_manager_test_access.hpp"  // NOLINT(build/include_subdir)
#include "server/worker_process_launch.hpp"  // NOLINT(build/include_subdir)
#include "server/worker_protocol.hpp"        // NOLINT(build/include_subdir)

#ifndef PS_TEST_WORKER_FIXTURE_PATH
#error "PS_TEST_WORKER_FIXTURE_PATH must name the real-process fixture"
#endif

namespace ps::server {
namespace {

using std::chrono_literals::operator""ms;
using std::chrono_literals::operator""s;

/** @brief High descriptor floor that rejects a small fixed closure cap. */
constexpr int kHighDescriptorSentinelFloor = 100000;
/** @brief Probe exit when requested `RLIMIT_NOFILE` setup unexpectedly fails.
 */
constexpr int kDescriptorLimitSetupFailed = 71;
/** @brief Probe exit when a high inheritable sentinel cannot be created. */
constexpr int kHighDescriptorSentinelSetupFailed = 72;
/** @brief Probe exit when the worker did not finish successfully in time. */
constexpr int kDescriptorWorkerExecutionFailed = 73;
/** @brief Probe exit when child closure changed the authority's descriptor. */
constexpr int kParentDescriptorOwnershipChanged = 74;
/** @brief Probe exit for an unexpected C++ setup or service exception. */
constexpr int kDescriptorProbeRaised = 75;
/** @brief Probe exit if allocation failure fabricates a completion callback. */
constexpr int kCompletionCallbackInvoked = 76;
/** @brief Probe exit if the failed record is ordinarily marked and deleted. */
constexpr int kCompletionRecordDeleted = 77;
/** @brief Probe exit for unexpected allocation-fault test setup failure. */
constexpr int kCompletionProbeRaised = 78;
/** @brief Probe exit when the isolated hard file-size limit cannot be set. */
constexpr int kFileSizeLimitSetupFailed = 79;
/** @brief Probe exit when the low hard limit does not yield WorkerStartup. */
constexpr int kFileSizeEnvelopeAccepted = 80;
/** @brief Probe exit when rejected startup leaves service-owned residue. */
constexpr int kFileSizeEnvelopeResidue = 81;
/** @brief Probe exit for an unexpected file-size-envelope probe exception. */
constexpr int kFileSizeProbeRaised = 82;
/** @brief One bulk byte beyond the former aggregate worker-frame ceiling. */
constexpr std::size_t kBulkPayloadAboveFormerControlBytes = (64U << 20U) + 1U;
/** @brief Tight liveness bound crossed by the held real bulk transfer. */
constexpr std::chrono::milliseconds kBulkHeartbeatEvidenceTimeout{2000};
/** @brief Fixed finite margin beyond one complete Heartbeat timeout. */
constexpr std::chrono::milliseconds kBulkHeartbeatEvidenceMargin{100};
/** @brief Isolated hard file-size limit below the normal one-MiB envelope. */
constexpr rlim_t kLowHardFileSizeLimit = static_cast<rlim_t>(64U << 10U);

/**
 * @brief Process-global descriptor-limit shape exercised by an isolated probe.
 * @throws Nothing for value operations.
 */
enum class DescriptorLimitProbeMode : std::uint8_t {
  /** @brief Sets the soft descriptor limit to `RLIM_INFINITY`. */
  Unlimited,
  /** @brief Lowers the soft limit only after opening the high sentinel. */
  LoweredAfterSentinel,
};

/**
 * @brief Owns one unique durable root through all service restarts in a test.
 * @throws Filesystem failures when creation fails.
 */
class ScopedSupervisorRoot final {
 public:
  /** @brief Creates one unique empty directory. */
  ScopedSupervisorRoot() {
    static std::atomic<std::uint64_t> sequence{0U};
    const auto ticks =
        std::chrono::steady_clock::now().time_since_epoch().count();
    path_ = std::filesystem::temp_directory_path() /
            ("photospider-issue100-supervisor-" + std::to_string(ticks) + "-" +
             std::to_string(sequence.fetch_add(1U)));
    if (!std::filesystem::create_directory(path_)) {
      throw std::runtime_error("failed to create supervisor test root");
    }
  }

  /** @brief Best-effort removes the exact root after all services close. */
  ~ScopedSupervisorRoot() noexcept {
    std::error_code error;
    std::filesystem::remove_all(path_, error);
  }

  /** @brief Prevents duplicate root ownership. */
  ScopedSupervisorRoot(const ScopedSupervisorRoot& other) = delete;
  /** @brief Prevents duplicate root assignment. */
  ScopedSupervisorRoot& operator=(const ScopedSupervisorRoot& other) = delete;

  /**
   * @brief Returns the exact existing root path.
   * @return Borrowed path valid for this owner lifetime.
   * @throws Nothing.
   */
  const std::filesystem::path& path() const noexcept { return path_; }

 private:
  /** @brief Sole recursively cleaned durable root. */
  std::filesystem::path path_;
};

/**
 * @brief Installs and restores one process-global `SIGCHLD` disposition.
 * @throws std::system_error when installation fails.
 * @note Tests use this owner only inside an isolated death-test child. Calling
 * `restore()` before child exit proves the disposition did not leak even
 * within that process.
 */
class ScopedSigchldDisposition final {
 public:
  /**
   * @brief Saves the current action and installs one auto-reaping candidate.
   * @param ignore Whether the handler is `SIG_IGN` rather than `SIG_DFL`.
   * @param flags Additional `sigaction` flags such as `SA_NOCLDWAIT`.
   * @throws std::system_error when mask initialization or installation fails.
   */
  ScopedSigchldDisposition(bool ignore, int flags) {
    struct sigaction replacement{};
    replacement.sa_handler = ignore ? SIG_IGN : SIG_DFL;
    replacement.sa_flags = flags;
    if (sigemptyset(&replacement.sa_mask) != 0 ||
        ::sigaction(SIGCHLD, &replacement, &previous_) != 0) {
      throw std::system_error(errno, std::generic_category(),
                              "install test SIGCHLD disposition");
    }
    installed_ = true;
  }

  /**
   * @brief Best-effort restores the prior action when still installed.
   * @throws Nothing.
   * @note Death-test children that intentionally abort cannot run this tail;
   * process isolation preserves the parent action in those cases.
   */
  ~ScopedSigchldDisposition() noexcept { static_cast<void>(restore()); }

  /**
   * @brief Prevents duplicate process-global disposition ownership.
   * @param other Existing owner that remains unchanged.
   * @throws Nothing because the operation is deleted.
   */
  ScopedSigchldDisposition(const ScopedSigchldDisposition& other) = delete;
  /**
   * @brief Prevents duplicate process-global disposition assignment.
   * @param other Existing owner that remains unchanged.
   * @return No assignment result because the operation is deleted.
   * @throws Nothing because the operation is deleted.
   */
  ScopedSigchldDisposition& operator=(const ScopedSigchldDisposition& other) =
      delete;

  /**
   * @brief Restores the exact saved action once.
   * @return True after restoration or when already restored.
   * @throws Nothing.
   */
  bool restore() noexcept {
    if (!installed_) {
      return true;
    }
    if (::sigaction(SIGCHLD, &previous_, nullptr) != 0) {
      return false;
    }
    installed_ = false;
    return true;
  }

 private:
  /** @brief Exact action observed before test installation. */
  struct sigaction previous_{};
  /** @brief Whether this owner still owes one restoration. */
  bool installed_ = false;
};

/**
 * @brief Externalizable fixture factory that cannot execute in control plane.
 * @throws std::bad_alloc when allocating its empty prepared catalog fails.
 */
class FixtureWorkerFactory final : public JobAttemptWorkerFactory {
 public:
  /**
   * @brief Supplies an empty immutable external graph catalog.
   * @throws std::bad_alloc when catalog allocation fails.
   * @note The process fixture keys only off JobSpec and intentionally ignores
   * the catalog's closed missing-graph diagnostic.
   */
  FixtureWorkerFactory()
      : FixtureWorkerFactory(
            std::make_shared<const PreparedExternalGraphCatalog>(
                std::vector<PreparedExternalGraphEntry>{})) {}

  /**
   * @brief Supplies one caller-prepared immutable external graph catalog.
   * @param external_graphs Non-null catalog transported to the fixture.
   * @throws std::invalid_argument when `external_graphs` is null.
   */
  explicit FixtureWorkerFactory(
      std::shared_ptr<const PreparedExternalGraphCatalog> external_graphs)
      : JobAttemptWorkerFactory(std::move(external_graphs)) {}

  /**
   * @brief Fails if the product path ever invokes an in-process worker.
   * @param assignment Exact assignment, intentionally unused.
   * @return Never returns.
   * @throws std::logic_error unconditionally.
   */
  std::unique_ptr<JobAttemptWorker> create(
      const JobAssignment& assignment) override {
    static_cast<void>(assignment);
    throw std::logic_error(
        "fixture factory must not execute in the control-plane process");
  }
};

/**
 * @brief Unmarked in-process-only factory used to prove product rejection.
 * @throws Nothing for construction.
 */
class UnmarkedWorkerFactory final : public JobAttemptWorkerFactory {
 public:
  /**
   * @brief Returns null if reached; construction must reject first.
   * @param assignment Exact assignment, unused.
   * @return Null worker.
   * @throws Nothing.
   */
  std::unique_ptr<JobAttemptWorker> create(
      const JobAssignment& assignment) override {
    static_cast<void>(assignment);
    return nullptr;
  }
};

/**
 * @brief Returns one minimal report for the explicit in-process completion
 * construction probe.
 * @throws Nothing for construction; report identity copies may allocate.
 */
class InProcessCompletionProbeWorker final : public JobAttemptWorker {
 public:
  /**
   * @brief Produces one worker-owned failed report without an artifact.
   * @param assignment Exact current assignment to echo.
   * @param cancellation_requested Unused monotonic observer.
   * @return Complete candidate report for the manager's first `Report` fact.
   * @throws std::bad_alloc when identity or message retention fails.
   */
  JobAttemptReport execute(
      const JobAssignment& assignment,
      const std::function<bool()>& cancellation_requested) override {
    static_cast<void>(cancellation_requested);
    JobAttemptReport report;
    report.identity = assignment.identity;
    report.outcome = JobAttemptOutcome::Failed;
    report.settled = false;
    report.failure = JobAttemptFailure::Unexpected;
    report.message = "in-process completion construction probe";
    return report;
  }
};

/**
 * @brief Creates one explicit source-private in-process completion probe.
 * @throws Nothing for construction; `create()` may allocate.
 */
class InProcessCompletionProbeFactory final
    : public InProcessJobAttemptWorkerFactoryForTest {
 public:
  /**
   * @brief Creates one fresh deterministic probe worker.
   * @param assignment Exact assignment, unused until worker execution.
   * @return Non-null fresh worker owner.
   * @throws std::bad_alloc when worker allocation fails.
   */
  std::unique_ptr<JobAttemptWorker> create(
      const JobAssignment& assignment) override {
    static_cast<void>(assignment);
    return std::make_unique<InProcessCompletionProbeWorker>();
  }
};

/**
 * @brief Builds complete finite resources realistic for a freshly execed image.
 * @return 512-GiB Darwin-compatible address-space envelope and one-MiB
 * artifact bounds.
 * @throws Nothing.
 */
JobResourceRequest supervisor_resources() {
  JobResourceRequest request;
  request.cpu_slots = 1U;
  request.host_memory_bytes = 512ULL << 30U;
  request.output_bytes = 1U << 20U;
  request.staging_bytes = 1U << 20U;
  request.retention_bytes = 1U << 20U;
  return request;
}

/**
 * @brief Builds resources for a candidate at the former aggregate frame cap.
 * @return Valid request whose image limits are one byte above 64 MiB.
 * @throws Nothing.
 * @note The candidate cannot fit the former aggregate control frame once
 * report metadata is included, but remains valid bulk data-plane content.
 */
JobResourceRequest bulk_data_plane_resources() {
  JobResourceRequest request = supervisor_resources();
  request.output_bytes = kBulkPayloadAboveFormerControlBytes;
  request.staging_bytes = kBulkPayloadAboveFormerControlBytes;
  request.retention_bytes = kBulkPayloadAboveFormerControlBytes;
  return request;
}

/**
 * @brief Builds permissive finite capacity for concurrent process tests.
 * @return Capacity for eight active 512-GiB attempts and retained outputs.
 * @throws Nothing.
 */
TenantQuotaLimits supervisor_quota() {
  TenantQuotaLimits limits;
  limits.maximum_active_attempts = 8U;
  limits.capacity.cpu_slots = 8U;
  limits.capacity.host_memory_bytes = 4ULL << 40U;
  limits.capacity.output_bytes = 8U << 20U;
  limits.capacity.staging_bytes = 8U << 20U;
  limits.capacity.retention_bytes = 8U << 20U;
  return limits;
}

/**
 * @brief Builds finite quota for bulk output followed by checkpoint reuse.
 * @return Output/staging capacity one byte above 64 MiB and retention for two
 * such resource envelopes.
 * @throws Nothing.
 */
TenantQuotaLimits bulk_data_plane_quota() {
  TenantQuotaLimits limits = supervisor_quota();
  limits.capacity.output_bytes = kBulkPayloadAboveFormerControlBytes;
  limits.capacity.staging_bytes = kBulkPayloadAboveFormerControlBytes;
  limits.capacity.retention_bytes = 2U * kBulkPayloadAboveFormerControlBytes;
  return limits;
}

/**
 * @brief Builds short deterministic manager bounds for the fixture.
 * @return Valid process options using the configured fixture executable.
 * @throws Path allocation failures unchanged.
 */
WorkerManagerOptions supervisor_options() {
  WorkerManagerOptions options;
  options.worker_executable = PS_TEST_WORKER_FIXTURE_PATH;
  options.startup_timeout = 2s;
  options.heartbeat_interval = 25ms;
  options.heartbeat_timeout = 180ms;
  options.attempt_runtime_timeout = 3s;
  options.post_report_timeout = 150ms;
  options.cooperative_cancel_timeout = 100ms;
  options.terminate_timeout = 100ms;
  options.kill_reap_timeout = 500ms;
  options.io_timeout = 500ms;
  return options;
}

/**
 * @brief Deterministic monotonic state for one exec-status acceptance test.
 * @throws Nothing for value construction and atomic operations.
 */
struct ExecStatusDeadlineHookState final {
  /** @brief Current synthetic monotonic clock ticks. */
  std::atomic<std::chrono::steady_clock::duration::rep> now_ticks{0};
  /** @brief Exact strict deadline reached at the observed result boundary. */
  std::chrono::steady_clock::time_point deadline;
  /** @brief Number of complete errno or clean EOF observations. */
  std::atomic<std::size_t> observations{0U};
};

/**
 * @brief Retains one synthetic clock state with its immutable hook contract.
 * @throws Nothing for value construction and ownership moves.
 */
struct ExecStatusDeadlineHooks final {
  /** @brief Mutable state retained for terminal assertions. */
  std::shared_ptr<ExecStatusDeadlineHookState> state;
  /** @brief Immutable callback contract installed in manager options. */
  std::shared_ptr<const WorkerManagerExecStatusDeadlineTestHooks> callbacks;
};

/**
 * @brief Returns one test's synthetic exec-status-only monotonic time.
 * @param context Non-null `ExecStatusDeadlineHookState` owner.
 * @return Exact atomically retained synthetic time.
 * @throws Nothing.
 */
std::chrono::steady_clock::time_point exec_status_test_now(
    void* context) noexcept {
  const auto* state = static_cast<const ExecStatusDeadlineHookState*>(context);
  return std::chrono::steady_clock::time_point{
      std::chrono::steady_clock::duration{
          state->now_ticks.load(std::memory_order_acquire)}};
}

/**
 * @brief Advances synthetic time to equality after one terminal pipe result.
 * @param context Non-null `ExecStatusDeadlineHookState` owner.
 * @param point Exact non-authorizing exec-status observation point.
 * @return Nothing.
 * @throws Nothing.
 * @note Equality is deliberately late under the strict `now < deadline`
 * contract. The callback performs no descriptor or process operation.
 */
void cross_exec_status_test_deadline(
    void* context, WorkerManagerExecStatusDeadlineTestPoint point) noexcept {
  auto* state = static_cast<ExecStatusDeadlineHookState*>(context);
  if (point ==
      WorkerManagerExecStatusDeadlineTestPoint::ResultReadyBeforeAcceptance) {
    state->observations.fetch_add(1U, std::memory_order_acq_rel);
    state->now_ticks.store(state->deadline.time_since_epoch().count(),
                           std::memory_order_release);
  }
}

/**
 * @brief Builds one retained exec-status hook and its deterministic state.
 * @param startup_timeout Positive manager startup duration.
 * @return Shared state and immutable callbacks suitable for manager options.
 * @throws Duration validation, deadline arithmetic, or allocation failures.
 */
ExecStatusDeadlineHooks exec_status_deadline_hooks(
    std::chrono::milliseconds startup_timeout) {
  auto state = std::make_shared<ExecStatusDeadlineHookState>();
  state->deadline = checked_worker_deadline(
      std::chrono::steady_clock::time_point{}, startup_timeout);
  auto hooks = std::make_shared<WorkerManagerExecStatusDeadlineTestHooks>();
  hooks->context = state;
  hooks->now = exec_status_test_now;
  hooks->observe = cross_exec_status_test_deadline;
  return {std::move(state), std::move(hooks)};
}

/**
 * @brief Describes one independently bounded WorkerManager duration field.
 * @throws Nothing for value construction.
 * @note The pointer-to-member keeps the rejection matrix tied directly to the
 * nine public source-private configuration fields.
 */
struct WorkerDurationFieldCase final {
  /** @brief Stable field spelling expected in rejection diagnostics. */
  std::string_view name;
  /** @brief Exact duration member selected by this matrix row. */
  std::chrono::milliseconds WorkerManagerOptions::* member;
  /** @brief Inclusive field-specific accepted maximum. */
  std::chrono::milliseconds maximum;
};

/** @brief Complete nine-field WorkerManager duration-bound matrix. */
constexpr std::array<WorkerDurationFieldCase, 9U> kWorkerDurationFieldCases{{
    {"startup_timeout", &WorkerManagerOptions::startup_timeout,
     kMaximumWorkerDuration},
    {"heartbeat_interval", &WorkerManagerOptions::heartbeat_interval,
     kMaximumWorkerHeartbeatInterval},
    {"heartbeat_timeout", &WorkerManagerOptions::heartbeat_timeout,
     kMaximumWorkerDuration},
    {"attempt_runtime_timeout", &WorkerManagerOptions::attempt_runtime_timeout,
     kMaximumWorkerDuration},
    {"post_report_timeout", &WorkerManagerOptions::post_report_timeout,
     kMaximumWorkerDuration},
    {"cooperative_cancel_timeout",
     &WorkerManagerOptions::cooperative_cancel_timeout, kMaximumWorkerDuration},
    {"terminate_timeout", &WorkerManagerOptions::terminate_timeout,
     kMaximumWorkerDuration},
    {"kill_reap_timeout", &WorkerManagerOptions::kill_reap_timeout,
     kMaximumWorkerDuration},
    {"io_timeout", &WorkerManagerOptions::io_timeout, kMaximumWorkerDuration},
}};

/**
 * @brief Closes one probe-owned descriptor through interrupted syscalls.
 * @param fd Descriptor or negative sentinel.
 * @return Nothing.
 * @throws Nothing.
 */
void close_probe_descriptor(int fd) noexcept {
  if (fd < 0) {
    return;
  }
  while (::close(fd) < 0 && errno == EINTR) {
  }
}

/**
 * @brief Owns one exact supervisor-test descriptor until reset/destruction.
 * @throws Nothing for construction and destruction.
 */
class ScopedProbeDescriptor final {
 public:
  /**
   * @brief Takes ownership of one descriptor or negative sentinel.
   * @param descriptor Descriptor to close exactly once.
   * @throws Nothing.
   */
  explicit ScopedProbeDescriptor(int descriptor) noexcept
      : descriptor_(descriptor) {}

  /** @brief Closes the exact descriptor if ownership remains armed. */
  ~ScopedProbeDescriptor() noexcept { reset(); }

  /** @brief Prevents duplicate descriptor ownership. */
  ScopedProbeDescriptor(const ScopedProbeDescriptor&) = delete;
  /** @brief Prevents duplicate descriptor assignment. */
  ScopedProbeDescriptor& operator=(const ScopedProbeDescriptor&) = delete;

  /**
   * @brief Closes and clears the exact owned descriptor.
   * @return Nothing.
   * @throws Nothing.
   */
  void reset() noexcept {
    close_probe_descriptor(descriptor_);
    descriptor_ = -1;
  }

  /**
   * @brief Borrows the currently owned descriptor.
   * @return Nonnegative descriptor while ownership remains armed.
   * @throws Nothing.
   */
  int get() const noexcept { return descriptor_; }

 private:
  /** @brief Exact owned descriptor, or negative after reset. */
  int descriptor_ = -1;
};

/**
 * @brief Builds one exact prepared binding for the filesystem-block fixture.
 * @param graph_id Exact fixture mode identity.
 * @param path Test-owned FIFO or regular-file path.
 * @return Non-null immutable one-entry external graph catalog.
 * @throws Path conversion, contract, or allocation failures unchanged.
 */
std::shared_ptr<const PreparedExternalGraphCatalog> filesystem_fixture_catalog(
    GraphArtifactId graph_id, const std::filesystem::path& path) {
  ResolvedGraphArtifact graph;
  graph.ok = true;
  graph.yaml_path = path.string();
  std::vector<PreparedExternalGraphEntry> entries;
  entries.push_back(
      PreparedExternalGraphEntry{std::move(graph_id), std::move(graph)});
  return std::make_shared<const PreparedExternalGraphCatalog>(
      std::move(entries));
}

/**
 * @brief Creates one named pipe used by the worker-side filesystem probe.
 * @param path Fresh exact path.
 * @return Nothing after successful creation.
 * @throws std::system_error when `mkfifo` fails.
 */
void create_worker_filesystem_fifo(const std::filesystem::path& path) {
  if (::mkfifo(path.c_str(), 0600) != 0) {
    throw std::system_error(errno, std::generic_category(),
                            "create worker filesystem FIFO");
  }
}

/**
 * @brief Opens a FIFO writer after the execed fixture waits on its read side.
 * @param path Existing exact FIFO.
 * @param timeout Maximum deterministic observer duration.
 * @return Writer descriptor that must remain open without data.
 * @throws std::runtime_error when no reader appears by the deadline.
 * @throws std::system_error for an unexpected `open` failure.
 */
int wait_for_worker_filesystem_reader(const std::filesystem::path& path,
                                      std::chrono::milliseconds timeout) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    const int descriptor = ::open(path.c_str(), O_WRONLY | O_NONBLOCK);
    if (descriptor >= 0) {
      return descriptor;
    }
    if (errno != ENXIO && errno != EINTR) {
      throw std::system_error(errno, std::generic_category(),
                              "open worker filesystem FIFO writer");
    }
    std::this_thread::sleep_for(10ms);
  }
  throw std::runtime_error("worker did not enter filesystem FIFO read");
}

/**
 * @brief Releases one worker blocked on the exact filesystem FIFO reader.
 * @param descriptor Connected nonnegative FIFO writer descriptor.
 * @return Nothing after exactly one byte is written.
 * @throws std::invalid_argument for a negative descriptor.
 * @throws std::system_error for an unexpected write failure.
 */
void release_worker_filesystem_reader(int descriptor) {
  if (descriptor < 0) {
    throw std::invalid_argument("worker filesystem FIFO writer is invalid");
  }
  const char release = 'x';
  for (;;) {
    const ssize_t written = ::write(descriptor, &release, sizeof(release));
    if (written == static_cast<ssize_t>(sizeof(release))) {
      return;
    }
    if (written < 0 && errno == EINTR) {
      continue;
    }
    throw std::system_error(errno, std::generic_category(),
                            "release worker filesystem FIFO reader");
  }
}

/**
 * @brief Replaces a drained FIFO with one regular one-byte recovery input.
 * @param path Exact test-owned path.
 * @return Nothing after a complete close.
 * @throws std::runtime_error for remove, open, write, or close failure.
 */
void replace_fifo_with_recovery_input(const std::filesystem::path& path) {
  if (!std::filesystem::remove(path)) {
    throw std::runtime_error("worker filesystem FIFO was not removed");
  }
  std::ofstream output(path, std::ios::binary);
  if (!output) {
    throw std::runtime_error("worker recovery input open failed");
  }
  output.put('x');
  output.close();
  if (!output) {
    throw std::runtime_error("worker recovery input write failed");
  }
}

/**
 * @brief Builds one fixture-selected immutable JobSpec.
 * @param mode Exact graph id interpreted only by the process fixture.
 * @param checkpoint Optional durable checkpoint identity.
 * @return Complete supported JobSpec.
 * @throws Contract/allocation failures unchanged.
 */
JobSpec fixture_spec(std::string mode,
                     std::optional<ArtifactId> checkpoint = std::nullopt) {
  return JobSpec(GraphArtifactId(std::move(mode)), 0,
                 OutputSlotId("image.final"), supervisor_resources(),
                 std::move(checkpoint));
}

/**
 * @brief Builds one valid assignment for a completion allocation-fault probe.
 * @param mode Exact fixture graph mode or in-process diagnostic selector.
 * @return Complete immutable exact assignment.
 * @throws Contract or allocation failures unchanged.
 */
JobAssignment completion_failure_assignment(
    std::string mode = "fixture.completion-allocation-failure") {
  auto spec = std::make_shared<const JobSpec>(fixture_spec(std::move(mode)));
  AttemptIdentity identity;
  identity.tenant_id = TenantId("tenant.supervisor.completion-failure");
  identity.job_id = JobId("job.supervisor.completion-failure");
  identity.job_spec_digest = spec->digest();
  identity.attempt_id = JobAttemptId("attempt.supervisor.completion-failure");
  identity.worker_instance_id =
      WorkerInstanceId("worker.supervisor.completion-failure");
  identity.worker_lease_generation = WorkerLeaseGeneration{1U};
  return JobAssignment{std::move(identity), std::move(spec), nullptr};
}

/**
 * @brief Creates one real product-mode service using the fixture executable.
 * @param root Existing durable root.
 * @param options Valid manager options.
 * @param factory Optional externalizable factory; null selects the default
 * process fixture catalog.
 * @param quota_limits Complete tenant capacity for this service instance.
 * @return Unique service owner.
 * @throws Service construction failures unchanged.
 */
std::unique_ptr<SingleTenantJobService> make_service(
    const std::filesystem::path& root, WorkerManagerOptions options,
    std::shared_ptr<JobAttemptWorkerFactory> factory = nullptr,
    TenantQuotaLimits quota_limits = supervisor_quota()) {
  if (factory == nullptr) {
    factory = std::make_shared<FixtureWorkerFactory>();
  }
  return std::make_unique<SingleTenantJobService>(
      TenantId("tenant.supervisor"), std::move(quota_limits), root,
      std::move(factory), DurableServerStateOptions{},
      TenantQuotaAuthorityOptions{}, std::move(options));
}

/**
 * @brief Probes construction rejection under one auto-reaping policy.
 * @param root Fresh service root that must remain unopened on rejection.
 * @param ignore Whether to install `SIG_IGN`.
 * @param flags Additional signal-action flags.
 * @return Zero only for an explicit `SIGCHLD` rejection plus restoration.
 * @throws Nothing; distinct nonzero codes preserve child-process diagnostics.
 */
int probe_sigchld_construction_rejection(const std::filesystem::path& root,
                                         bool ignore, int flags) noexcept {
  try {
    ScopedSigchldDisposition disposition(ignore, flags);
    bool rejected = false;
    try {
      auto service = make_service(root, supervisor_options());
      static_cast<void>(service);
    } catch (const std::invalid_argument& error) {
      rejected = std::string(error.what()).find("SIGCHLD") != std::string::npos;
    } catch (...) {
      return 2;
    }
    const bool root_untouched = !std::filesystem::exists(root);
    if (!disposition.restore()) {
      return 3;
    }
    if (!rejected) {
      return 4;
    }
    return root_untouched ? 0 : 5;
  } catch (...) {
    return 6;
  }
}

/**
 * @brief Waits for one terminal Job and asserts presence.
 * @param service Live service.
 * @param job_id Exact accepted Job identity.
 * @return Copied terminal snapshot.
 * @throws Test assertion failure as runtime error when unexpectedly absent.
 */
JobSnapshot wait_terminal(SingleTenantJobService& service,
                          const JobId& job_id) {
  std::optional<JobSnapshot> snapshot = service.wait_for(job_id, 5s);
  if (!snapshot.has_value()) {
    throw std::runtime_error("supervisor Job did not become terminal");
  }
  return *snapshot;
}

/**
 * @brief Verifies one strict exec-status deadline terminal and full cleanup.
 * @param service Live service after one deterministic deadline crossing.
 * @param submitted Exact accepted Job receipt.
 * @param hook_state Non-null retained synthetic-clock observation state.
 * @return Nothing after terminal, process, thread, quota, and artifact checks.
 * @throws Test assertion failures and observer wait failures through
 * GoogleTest.
 */
void expect_exec_status_deadline_failure_without_residue(
    SingleTenantJobService& service, const JobSubmission& submitted,
    const std::shared_ptr<ExecStatusDeadlineHookState>& hook_state) {
  const JobSnapshot terminal = wait_terminal(service, submitted.job_id);

  EXPECT_EQ(hook_state->observations.load(std::memory_order_acquire), 1U);
  EXPECT_EQ(terminal.state, JobState::Failed) << terminal.message;
  EXPECT_TRUE(terminal.attempt_settled);
  EXPECT_EQ(terminal.attempt_outcome, JobAttemptOutcome::Failed);
  EXPECT_EQ(terminal.failure, JobAttemptFailure::WorkerStartup);
  EXPECT_EQ(terminal.message, "worker exec-status deadline expired");
  EXPECT_FALSE(terminal.output_receipt.has_value());
  EXPECT_EQ(service.find_artifact(terminal.output_artifact_id), nullptr);
  EXPECT_TRUE(SingleTenantJobServiceTestAccess::
                  wait_for_owned_worker_thread_count_at_most(service, 0U, 2s));
  EXPECT_EQ(
      SingleTenantJobServiceTestAccess::live_worker_process_count(service), 0U);
  const TenantQuotaSnapshot quota = service.quota_snapshot();
  EXPECT_EQ(quota.active_attempts, 0U);
  EXPECT_EQ(quota.cpu_slots, 0U);
  EXPECT_EQ(quota.host_memory_bytes, 0U);
  EXPECT_EQ(quota.output_bytes, 0U);
  EXPECT_EQ(quota.staging_bytes, 0U);
  EXPECT_EQ(quota.retention_bytes, 0U);
  EXPECT_EQ(quota.retained_artifacts, 0U);
  EXPECT_TRUE(quota.device_bytes.empty());
}

/**
 * @brief Exercises real worker exec across one descriptor-limit boundary.
 * @param mode Process-global soft-limit sequence to install around the high fd.
 * @return Zero only when the worker succeeds in under two seconds, the execed
 * fixture observes the high sentinel as closed, and the authority still owns
 * its original sentinel; otherwise one stable nonzero probe code.
 * @throws Nothing; exceptions are converted to a stable probe exit.
 * @note The caller runs this function only in an isolated death-test child.
 * Its process-global `RLIMIT_NOFILE` mutation therefore cannot affect another
 * test, while the service still exercises the real fork/exec product path.
 */
int run_descriptor_closure_probe(DescriptorLimitProbeMode mode) noexcept {
  int source_descriptor = -1;
  int sentinel_descriptor = -1;
  try {
    rlimit descriptor_limit{};
    if (::getrlimit(RLIMIT_NOFILE, &descriptor_limit) != 0) {
      return kDescriptorLimitSetupFailed;
    }
    if (mode == DescriptorLimitProbeMode::Unlimited) {
      if (descriptor_limit.rlim_max != RLIM_INFINITY) {
        return kDescriptorLimitSetupFailed;
      }
      descriptor_limit.rlim_cur = RLIM_INFINITY;
    } else {
      const rlim_t required =
          static_cast<rlim_t>(kHighDescriptorSentinelFloor + 1);
      if (descriptor_limit.rlim_max != RLIM_INFINITY &&
          descriptor_limit.rlim_max < required) {
        return kDescriptorLimitSetupFailed;
      }
      descriptor_limit.rlim_cur =
          descriptor_limit.rlim_max == RLIM_INFINITY
              ? required
              : std::max(descriptor_limit.rlim_cur, required);
    }
    if (::setrlimit(RLIMIT_NOFILE, &descriptor_limit) != 0) {
      return kDescriptorLimitSetupFailed;
    }

    source_descriptor = ::open("/dev/null", O_RDONLY);
    if (source_descriptor < 0) {
      return kHighDescriptorSentinelSetupFailed;
    }
    sentinel_descriptor =
        ::fcntl(source_descriptor, F_DUPFD, kHighDescriptorSentinelFloor);
    close_probe_descriptor(source_descriptor);
    source_descriptor = -1;
    if (sentinel_descriptor < kHighDescriptorSentinelFloor) {
      close_probe_descriptor(sentinel_descriptor);
      return kHighDescriptorSentinelSetupFailed;
    }
    if (mode == DescriptorLimitProbeMode::LoweredAfterSentinel) {
      descriptor_limit.rlim_cur = 1024;
      if (::setrlimit(RLIMIT_NOFILE, &descriptor_limit) != 0 ||
          ::fcntl(sentinel_descriptor, F_GETFD) < 0) {
        close_probe_descriptor(sentinel_descriptor);
        return kDescriptorLimitSetupFailed;
      }
    }

    ScopedSupervisorRoot root;
    WorkerManagerOptions options = supervisor_options();
    options.startup_timeout = 1s;
    auto service = make_service(root.path(), std::move(options));
    const std::string mode =
        "fixture.fd.closed." + std::to_string(sentinel_descriptor);
    const auto started = std::chrono::steady_clock::now();
    const JobSubmission submitted = service->submit(fixture_spec(mode));
    const JobSnapshot terminal = wait_terminal(*service, submitted.job_id);
    const auto elapsed = std::chrono::steady_clock::now() - started;
    if (terminal.state != JobState::Succeeded || elapsed >= 2s) {
      service.reset();
      close_probe_descriptor(sentinel_descriptor);
      return kDescriptorWorkerExecutionFailed;
    }
    if (::fcntl(sentinel_descriptor, F_GETFD) < 0) {
      service.reset();
      close_probe_descriptor(sentinel_descriptor);
      return kParentDescriptorOwnershipChanged;
    }
    service.reset();
    close_probe_descriptor(sentinel_descriptor);
    return 0;
  } catch (...) {
    close_probe_descriptor(source_descriptor);
    close_probe_descriptor(sentinel_descriptor);
    return kDescriptorProbeRaised;
  }
}

/**
 * @brief Proves a finite hard file-size limit cannot narrow accepted metadata.
 * @return Zero only when the attempt fails as `WorkerStartup` before any child
 * process exists and all active quota/worker/artifact ownership is released;
 * otherwise one stable nonzero probe code.
 * @throws Nothing; setup and service exceptions become stable probe exits.
 * @note The caller runs this function only in an isolated death-test child.
 * Lowering its hard `RLIMIT_FSIZE` is irreversible within that child and can
 * therefore never contaminate the parent test process.
 */
int run_file_size_envelope_rejection_probe() noexcept {
  try {
    rlimit file_size_limit{};
    if (::getrlimit(RLIMIT_FSIZE, &file_size_limit) != 0 ||
        (file_size_limit.rlim_max != RLIM_INFINITY &&
         file_size_limit.rlim_max < kLowHardFileSizeLimit)) {
      return kFileSizeLimitSetupFailed;
    }
    file_size_limit.rlim_cur = kLowHardFileSizeLimit;
    file_size_limit.rlim_max = kLowHardFileSizeLimit;
    if (::setrlimit(RLIMIT_FSIZE, &file_size_limit) != 0) {
      return kFileSizeLimitSetupFailed;
    }

    ScopedSupervisorRoot root;
    auto service = make_service(root.path(), supervisor_options());
    const JobSubmission submitted =
        service->submit(fixture_spec("fixture.file-size-envelope"));
    const JobSnapshot terminal = wait_terminal(*service, submitted.job_id);
    if (terminal.state != JobState::Failed ||
        terminal.failure != JobAttemptFailure::WorkerStartup ||
        !terminal.attempt_settled || terminal.output_receipt.has_value() ||
        service->find_artifact(terminal.output_artifact_id) != nullptr) {
      return kFileSizeEnvelopeAccepted;
    }
    if (!SingleTenantJobServiceTestAccess::
            wait_for_owned_worker_thread_count_at_most(*service, 0U, 2s) ||
        SingleTenantJobServiceTestAccess::live_worker_process_count(*service) !=
            0U) {
      return kFileSizeEnvelopeResidue;
    }
    const TenantQuotaSnapshot quota = service->quota_snapshot();
    if (quota.active_attempts != 0U || quota.cpu_slots != 0U ||
        quota.host_memory_bytes != 0U || quota.output_bytes != 0U ||
        quota.staging_bytes != 0U || quota.retention_bytes != 0U ||
        quota.retained_artifacts != 0U || !quota.device_bytes.empty()) {
      return kFileSizeEnvelopeResidue;
    }
    service.reset();
    return 0;
  } catch (...) {
    return kFileSizeProbeRaised;
  }
}

/**
 * @brief Polls a predicate within one short deterministic observer bound.
 * @param predicate Nonempty read-only predicate.
 * @param timeout Maximum observer duration.
 * @return True once predicate succeeds, otherwise false at timeout.
 * @throws Predicate exceptions unchanged.
 */
bool wait_until(const std::function<bool()>& predicate,
                std::chrono::milliseconds timeout) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    if (predicate()) {
      return true;
    }
    std::this_thread::sleep_for(10ms);
  }
  return predicate();
}

/**
 * @brief Drives one actual first terminal-completion constructor under a
 * deterministic allocation fault.
 * @param point Exact completion kind whose first construction must fail-stop.
 * @param in_process_test_mode Whether to use the explicit in-process marker.
 * @param mode External fixture mode or in-process diagnostic selector.
 * @return Never returns normally; stable nonzero exits diagnose an invalid
 * callback, ordinary record retirement, or setup exception.
 * @throws Nothing; all setup exceptions become `kCompletionProbeRaised`.
 * @note The conforming path aborts before the callback and before completed-
 * record retirement. The death-test parent owns process isolation.
 */
[[noreturn]] void provoke_initial_completion_construction_failure(
    WorkerManagerCompletionConstructionPointForTest point,
    bool in_process_test_mode, std::string mode) noexcept {
  try {
    WorkerManagerOptions options = supervisor_options();
    options.fail_initial_completion_construction_for_test = std::make_shared<
        std::atomic<WorkerManagerCompletionConstructionPointForTest>>(point);
    WorkerManagerCallbacks callbacks;
    callbacks.begin_assignment = [](const AttemptIdentity&) { return true; };
    callbacks.cancellation_requested = [](const AttemptIdentity&) {
      return false;
    };
    callbacks.complete_assignment = [](WorkerManagerCompletion) {
      ::_exit(kCompletionCallbackInvoked);
    };
    std::shared_ptr<JobAttemptWorkerFactory> factory;
    if (in_process_test_mode) {
      factory = std::make_shared<InProcessCompletionProbeFactory>();
    } else {
      factory = std::make_shared<FixtureWorkerFactory>();
    }
    JobAssignment assignment = completion_failure_assignment(std::move(mode));
    const AttemptIdentity identity = assignment.identity;
    WorkerManager manager(std::move(factory), std::move(callbacks),
                          std::move(options), in_process_test_mode);
    manager.start(std::move(assignment));
    if (point ==
        WorkerManagerCompletionConstructionPointForTest::ForcedCancellation) {
      if (!wait_until(
              [&] { return manager.ownership_snapshot().live_processes == 1U; },
              2s) ||
          !manager.request_cancel(identity)) {
        ::_exit(kCompletionProbeRaised);
      }
    }
    if (!manager.wait_for_owned_count_at_most(0U, 5s)) {
      ::_exit(kCompletionProbeRaised);
    }
    ::_exit(kCompletionRecordDeleted);
  } catch (...) {
    ::_exit(kCompletionProbeRaised);
  }
}

/**
 * @brief Provokes exact-reaping authority loss after a worker is live.
 * @param root Fresh service root used only by the isolated death-test child.
 * @param ignore Whether to install `SIG_IGN`.
 * @param flags Additional signal-action flags.
 * @return Nothing; a conforming manager fail-stops during service drainage.
 * @throws Setup failures unchanged so the death-test regex cannot hide them.
 * @note The signal action is process-local to the death-test child; expected
 * `SIGABRT` ends that child, while the parent test process remains unchanged.
 */
void provoke_reap_authority_loss(const std::filesystem::path& root, bool ignore,
                                 int flags) {
  auto service = make_service(root, supervisor_options());
  static_cast<void>(service->submit(fixture_spec("fixture.ignore")));
  if (!wait_until(
          [&] {
            return SingleTenantJobServiceTestAccess::live_worker_process_count(
                       *service) == 1U;
          },
          2s)) {
    throw std::runtime_error("authority-loss worker did not become live");
  }
  ScopedSigchldDisposition disposition(ignore, flags);
  service.reset();
  throw std::runtime_error("authority loss returned ordinary settlement");
}

/**
 * @brief Reads the PID encoded in one fixture artifact's four tight bytes.
 * @param service Live artifact authority.
 * @param snapshot Successful Job snapshot with receipt.
 * @return Encoded positive child PID.
 * @throws std::runtime_error for absent or malformed artifact truth.
 */
std::uint32_t artifact_pid(const SingleTenantJobService& service,
                           const JobSnapshot& snapshot) {
  if (!snapshot.output_receipt.has_value()) {
    throw std::runtime_error("successful fixture Job has no receipt");
  }
  const std::shared_ptr<const ArtifactRecord> artifact =
      service.find_artifact(snapshot.output_receipt->artifact_id);
  if (artifact == nullptr ||
      artifact->payload.size() != sizeof(std::uint32_t)) {
    throw std::runtime_error("fixture artifact has no encoded PID");
  }
  std::uint32_t pid = 0U;
  std::memcpy(&pid, artifact->payload.data(), sizeof(pid));
  return pid;
}

TEST(WorkerSupervisor, ConcurrentAttemptsUseFreshProcessesAndReapCompletely) {
  ScopedSupervisorRoot root;
  auto service = make_service(root.path(), supervisor_options());
  const JobSubmission first =
      service->submit(fixture_spec("fixture.slow.success"));
  const JobSubmission second =
      service->submit(fixture_spec("fixture.slow.success"));

  ASSERT_TRUE(wait_until(
      [&] {
        return SingleTenantJobServiceTestAccess::live_worker_process_count(
                   *service) == 2U;
      },
      2s));
  const JobSnapshot first_terminal = wait_terminal(*service, first.job_id);
  const JobSnapshot second_terminal = wait_terminal(*service, second.job_id);
  ASSERT_EQ(first_terminal.state, JobState::Succeeded);
  ASSERT_EQ(second_terminal.state, JobState::Succeeded);
  EXPECT_NE(artifact_pid(*service, first_terminal),
            artifact_pid(*service, second_terminal));
  EXPECT_TRUE(SingleTenantJobServiceTestAccess::
                  wait_for_owned_worker_thread_count_at_most(*service, 0U, 2s));
  EXPECT_EQ(
      SingleTenantJobServiceTestAccess::live_worker_process_count(*service),
      0U);
}

TEST(WorkerSupervisor,
     LaunchDeadlinesExactlyMatchManagerPolicyAboveLegacyCaps) {
  ScopedSupervisorRoot root;
  WorkerManagerOptions options = supervisor_options();
  options.startup_timeout = 12s;
  options.io_timeout = 3500ms;
  auto service = make_service(root.path(), std::move(options));

  const JobSubmission submitted =
      service->submit(fixture_spec("fixture.launch.deadlines.12000.3500"));
  const JobSnapshot terminal = wait_terminal(*service, submitted.job_id);

  EXPECT_EQ(terminal.state, JobState::Succeeded) << terminal.message;
  EXPECT_EQ(terminal.failure, JobAttemptFailure::None);
}

TEST(WorkerSupervisor, ExecStatusCleanEofCrossingDeadlineFailsClosedAndReaps) {
  ScopedSupervisorRoot root;
  WorkerManagerOptions options = supervisor_options();
  auto hooks = exec_status_deadline_hooks(options.startup_timeout);
  options.exec_status_deadline_hooks_for_test = hooks.callbacks;
  auto service = make_service(root.path(), std::move(options));

  const JobSubmission submitted =
      service->submit(fixture_spec("fixture.exec-status.clean-eof"));

  expect_exec_status_deadline_failure_without_residue(*service, submitted,
                                                      hooks.state);
}

TEST(WorkerSupervisor, ExecStatusErrnoCrossingDeadlineReportsDeadlineAndReaps) {
  ScopedSupervisorRoot root;
  const std::filesystem::path transient_executable =
      root.path() / "exec-status-errno-fixture";
  std::filesystem::create_symlink(PS_TEST_WORKER_FIXTURE_PATH,
                                  transient_executable);
  WorkerManagerOptions options = supervisor_options();
  options.worker_executable = transient_executable;
  auto hooks = exec_status_deadline_hooks(options.startup_timeout);
  options.exec_status_deadline_hooks_for_test = hooks.callbacks;
  auto service = make_service(root.path(), std::move(options));
  ASSERT_TRUE(std::filesystem::remove(transient_executable));

  const JobSubmission submitted =
      service->submit(fixture_spec("fixture.exec-status.errno"));

  expect_exec_status_deadline_failure_without_residue(*service, submitted,
                                                      hooks.state);
}

TEST(WorkerSupervisor,
     PreparedGraphTextBoundsFailBeforeServiceOwnershipAndExactValuesExecute) {
  /**
   * @brief Selects one transported prepared-graph text field for rejection.
   * @throws Nothing for aggregate initialization and value operations.
   */
  struct GraphTextFieldCase final {
    /** @brief Human-readable field name used in assertion diagnostics. */
    const char* name;
    /** @brief Exact `ResolvedGraphArtifact` string member under test. */
    std::string ResolvedGraphArtifact::* member;
    /** @brief Distinct byte repeated through the boundary condition. */
    char padding;
  };
  const std::array<GraphTextFieldCase, 5U> cases{{
      {"root_dir", &ResolvedGraphArtifact::root_dir, 'r'},
      {"yaml_path", &ResolvedGraphArtifact::yaml_path, 'y'},
      {"config_path", &ResolvedGraphArtifact::config_path, 'c'},
      {"cache_root_dir", &ResolvedGraphArtifact::cache_root_dir, 'h'},
      {"message", &ResolvedGraphArtifact::message, 'm'},
  }};

  ScopedSupervisorRoot root;
  const GraphArtifactId graph_id("fixture.graph-text-bound");
  ResolvedGraphArtifact exact_graph;
  exact_graph.ok = true;
  for (const GraphTextFieldCase& test_case : cases) {
    exact_graph.*(test_case.member) =
        std::string(kMaximumWorkerTextFieldBytes, test_case.padding);
  }
  auto exact_catalog = std::make_shared<const PreparedExternalGraphCatalog>(
      std::vector<PreparedExternalGraphEntry>{{graph_id, exact_graph}});
  auto exact_factory =
      std::make_shared<FixtureWorkerFactory>(std::move(exact_catalog));
  auto exact_service =
      make_service(root.path() / "exact-bound", supervisor_options(),
                   std::move(exact_factory));
  const JobSubmission submitted =
      exact_service->submit(fixture_spec(graph_id.value()));
  const JobSnapshot terminal = wait_terminal(*exact_service, submitted.job_id);
  EXPECT_EQ(terminal.state, JobState::Succeeded) << terminal.message;
  EXPECT_EQ(terminal.failure, JobAttemptFailure::None);
  EXPECT_NE(terminal.failure, JobAttemptFailure::WorkerStartup);
  EXPECT_TRUE(
      SingleTenantJobServiceTestAccess::
          wait_for_owned_worker_thread_count_at_most(*exact_service, 0U, 2s));
  EXPECT_EQ(SingleTenantJobServiceTestAccess::live_worker_process_count(
                *exact_service),
            0U);
  exact_service.reset();

  for (const GraphTextFieldCase& test_case : cases) {
    const std::filesystem::path rejected_root =
        root.path() / (std::string("rejected-") + test_case.name);
    std::shared_ptr<const PreparedExternalGraphCatalog> rejected_catalog;
    std::shared_ptr<FixtureWorkerFactory> rejected_factory;
    std::unique_ptr<SingleTenantJobService> rejected_service;
    ResolvedGraphArtifact oversized_graph;
    oversized_graph.ok = true;
    oversized_graph.*(test_case.member) =
        std::string(kMaximumWorkerTextFieldBytes + 1U, test_case.padding);
    try {
      rejected_catalog = std::make_shared<const PreparedExternalGraphCatalog>(
          std::vector<PreparedExternalGraphEntry>{
              {graph_id, std::move(oversized_graph)}});
      rejected_factory =
          std::make_shared<FixtureWorkerFactory>(rejected_catalog);
      rejected_service =
          make_service(rejected_root, supervisor_options(), rejected_factory);
      ADD_FAILURE() << test_case.name
                    << " reached service construction one byte over";
    } catch (const std::length_error& error) {
      const std::string diagnostic(error.what());
      EXPECT_NE(diagnostic.find(test_case.name), std::string::npos)
          << diagnostic;
      EXPECT_NE(
          diagnostic.find(std::to_string(kMaximumWorkerTextFieldBytes + 1U)),
          std::string::npos)
          << diagnostic;
      EXPECT_NE(diagnostic.find(std::to_string(kMaximumWorkerTextFieldBytes)),
                std::string::npos)
          << diagnostic;
    } catch (const std::exception& error) {
      ADD_FAILURE() << test_case.name
                    << " raised wrong exception: " << error.what();
    }
    EXPECT_EQ(rejected_catalog, nullptr) << test_case.name;
    EXPECT_EQ(rejected_factory, nullptr) << test_case.name;
    EXPECT_EQ(rejected_service, nullptr) << test_case.name;
    EXPECT_FALSE(std::filesystem::exists(rejected_root)) << test_case.name;
  }
}

TEST(WorkerSupervisor,
     BlockingTrustedFilesystemIoCanBeCancelledReapedAndRecovered) {
  ScopedSupervisorRoot root;
  const std::filesystem::path fifo = root.path() / "worker-filesystem.fifo";
  create_worker_filesystem_fifo(fifo);
  const GraphArtifactId graph_id("fixture.fs.block");
  auto factory = std::make_shared<FixtureWorkerFactory>(
      filesystem_fixture_catalog(graph_id, fifo));
  WorkerManagerOptions options = supervisor_options();
  options.startup_timeout = 12s;
  options.io_timeout = 3500ms;
  auto service =
      make_service(root.path(), std::move(options), std::move(factory));
  const JobSubmission submitted =
      service->submit(fixture_spec(graph_id.value()));
  ScopedProbeDescriptor writer(wait_for_worker_filesystem_reader(fifo, 2s));

  const auto cancel_started = std::chrono::steady_clock::now();
  ASSERT_TRUE(service->cancel(submitted.job_id));
  const JobSnapshot cancelled = wait_terminal(*service, submitted.job_id);
  const auto cancel_elapsed = std::chrono::steady_clock::now() - cancel_started;

  EXPECT_EQ(cancelled.state, JobState::Cancelled) << cancelled.message;
  EXPECT_TRUE(cancelled.attempt_settled);
  EXPECT_EQ(cancelled.failure, JobAttemptFailure::WorkerCancellationForced);
  EXPECT_LT(cancel_elapsed, 2s);

  writer.reset();
  replace_fifo_with_recovery_input(fifo);
  const JobSubmission recovery =
      service->submit(fixture_spec(graph_id.value()));
  const JobSnapshot recovered = wait_terminal(*service, recovery.job_id);
  EXPECT_EQ(recovered.state, JobState::Succeeded) << recovered.message;
  EXPECT_TRUE(recovered.attempt_settled);
  EXPECT_EQ(recovered.failure, JobAttemptFailure::None);
  EXPECT_TRUE(recovered.output_receipt.has_value());
  EXPECT_NE(recovered.assignment.worker_instance_id,
            cancelled.assignment.worker_instance_id);
}

TEST(WorkerSupervisor,
     BlockingTrustedFilesystemIoCannotMakeServiceDestructionUnbounded) {
  ScopedSupervisorRoot root;
  const std::filesystem::path fifo = root.path() / "worker-shutdown.fifo";
  create_worker_filesystem_fifo(fifo);
  const GraphArtifactId graph_id("fixture.fs.block");
  auto factory = std::make_shared<FixtureWorkerFactory>(
      filesystem_fixture_catalog(graph_id, fifo));
  auto service =
      make_service(root.path(), supervisor_options(), std::move(factory));
  static_cast<void>(service->submit(fixture_spec(graph_id.value())));
  ScopedProbeDescriptor writer(wait_for_worker_filesystem_reader(fifo, 2s));

  const auto shutdown_started = std::chrono::steady_clock::now();
  service.reset();
  const auto shutdown_elapsed =
      std::chrono::steady_clock::now() - shutdown_started;

  EXPECT_LT(shutdown_elapsed, 2s);
}

TEST(WorkerSupervisor,
     PausedCheckpointTransferLeavesServiceMutexResponsiveAndCancellable) {
  ScopedSupervisorRoot root;
  auto defer_checkpoint = std::make_shared<std::atomic<bool>>(false);
  auto checkpoint_paused = std::make_shared<std::atomic<bool>>(false);
  WorkerManagerOptions options = supervisor_options();
  options.defer_checkpoint_transfer_for_test = defer_checkpoint;
  options.checkpoint_transfer_paused_for_test = checkpoint_paused;
  auto service = make_service(root.path(), std::move(options));

  const JobSubmission source =
      service->submit(fixture_spec("fixture.checkpoint-source"));
  const JobSnapshot source_terminal = wait_terminal(*service, source.job_id);
  ASSERT_EQ(source_terminal.state, JobState::Succeeded)
      << source_terminal.message;
  ASSERT_TRUE(source_terminal.output_receipt.has_value());
  const ArtifactId checkpoint_id = source_terminal.output_receipt->artifact_id;
  defer_checkpoint->store(true, std::memory_order_release);

  const auto submit_started = std::chrono::steady_clock::now();
  const JobSubmission consumer =
      service->submit(fixture_spec("fixture.checkpoint", checkpoint_id));
  const auto submit_elapsed = std::chrono::steady_clock::now() - submit_started;
  EXPECT_LT(submit_elapsed, 500ms);
  ASSERT_TRUE(wait_until(
      [&] { return checkpoint_paused->load(std::memory_order_acquire); }, 2s));

  const auto query_started = std::chrono::steady_clock::now();
  const std::optional<JobSnapshot> running = service->query(consumer.job_id);
  const auto query_elapsed = std::chrono::steady_clock::now() - query_started;
  ASSERT_TRUE(running.has_value());
  EXPECT_FALSE(is_terminal_job_state(running->state));
  EXPECT_LT(query_elapsed, 200ms);
  ASSERT_TRUE(service->cancel(consumer.job_id));
  const JobSnapshot cancelled = wait_terminal(*service, consumer.job_id);

  EXPECT_EQ(cancelled.state, JobState::Cancelled) << cancelled.message;
  EXPECT_EQ(cancelled.failure, JobAttemptFailure::WorkerCancellationForced);
  EXPECT_FALSE(cancelled.output_receipt.has_value());
  EXPECT_EQ(service->find_artifact(cancelled.output_artifact_id), nullptr);
  EXPECT_NE(service->find_artifact(checkpoint_id), nullptr);
  const TenantQuotaSnapshot quota = service->quota_snapshot();
  EXPECT_EQ(quota.active_attempts, 0U);
  EXPECT_EQ(quota.retained_artifacts, 1U);
  EXPECT_EQ(quota.retention_bytes,
            source_terminal.output_receipt->descriptor.payload_bytes);
  EXPECT_TRUE(SingleTenantJobServiceTestAccess::
                  wait_for_owned_worker_thread_count_at_most(*service, 0U, 2s));
  EXPECT_EQ(
      SingleTenantJobServiceTestAccess::live_worker_process_count(*service),
      0U);
}

TEST(WorkerSupervisor,
     PausedOutputTransferCannotMakeServiceDestructionUnboundedOrPublish) {
  ScopedSupervisorRoot root;
  auto defer_output = std::make_shared<std::atomic<bool>>(true);
  auto output_paused = std::make_shared<std::atomic<bool>>(false);
  WorkerManagerOptions options = supervisor_options();
  options.heartbeat_timeout = 3s;
  options.attempt_runtime_timeout = 10s;
  options.defer_output_drain_for_test = defer_output;
  options.output_transfer_paused_for_test = output_paused;
  auto service = make_service(root.path(), std::move(options), nullptr,
                              bulk_data_plane_quota());
  const JobSpec spec(GraphArtifactId("fixture.former-control-bound-output"), 0,
                     OutputSlotId("image.final"), bulk_data_plane_resources());
  const JobSubmission submitted = service->submit(spec);
  ASSERT_TRUE(wait_until(
      [&] { return output_paused->load(std::memory_order_acquire); }, 3s));
  const std::optional<JobSnapshot> active = service->query(submitted.job_id);
  ASSERT_TRUE(active.has_value());
  const ArtifactId candidate_artifact_id = active->output_artifact_id;

  const auto shutdown_started = std::chrono::steady_clock::now();
  service.reset();
  const auto shutdown_elapsed =
      std::chrono::steady_clock::now() - shutdown_started;
  EXPECT_LT(shutdown_elapsed, 2s);

  auto recovered = make_service(root.path(), supervisor_options(), nullptr,
                                bulk_data_plane_quota());
  const std::optional<JobSnapshot> terminal =
      recovered->query(submitted.job_id);
  ASSERT_TRUE(terminal.has_value());
  EXPECT_EQ(terminal->state, JobState::Cancelled) << terminal->message;
  EXPECT_EQ(terminal->failure, JobAttemptFailure::WorkerCancellationForced);
  EXPECT_FALSE(terminal->output_receipt.has_value());
  EXPECT_EQ(recovered->find_artifact(candidate_artifact_id), nullptr);
  const TenantQuotaSnapshot quota = recovered->quota_snapshot();
  EXPECT_EQ(quota.active_attempts, 0U);
  EXPECT_EQ(quota.retained_artifacts, 0U);
  EXPECT_EQ(quota.retention_bytes, 0U);
  EXPECT_EQ(
      SingleTenantJobServiceTestAccess::live_worker_process_count(*recovered),
      0U);
}

TEST(WorkerSupervisor,
     UnlimitedNoFileLimitClosesHighDescriptorAndExecsPromptly) {
  rlimit descriptor_limit{};
  ASSERT_EQ(::getrlimit(RLIMIT_NOFILE, &descriptor_limit), 0);
  if (descriptor_limit.rlim_max != RLIM_INFINITY) {
    GTEST_SKIP() << "hard RLIMIT_NOFILE cannot represent unlimited";
  }

  EXPECT_EXIT(
      {
        ::_exit(
            run_descriptor_closure_probe(DescriptorLimitProbeMode::Unlimited));
      },
      ::testing::ExitedWithCode(0), "");
}

TEST(WorkerSupervisor, LoweredNoFileLimitStillClosesPreexistingHighDescriptor) {
  rlimit descriptor_limit{};
  ASSERT_EQ(::getrlimit(RLIMIT_NOFILE, &descriptor_limit), 0);
  if (descriptor_limit.rlim_max != RLIM_INFINITY &&
      descriptor_limit.rlim_max <=
          static_cast<rlim_t>(kHighDescriptorSentinelFloor)) {
    GTEST_SKIP() << "hard RLIMIT_NOFILE cannot allocate the high sentinel";
  }

  EXPECT_EXIT(
      {
        ::_exit(run_descriptor_closure_probe(
            DescriptorLimitProbeMode::LoweredAfterSentinel));
      },
      ::testing::ExitedWithCode(0), "");
}

TEST(WorkerSupervisor,
     LowHardFileSizeLimitFailsAcceptedEnvelopeBeforeForkWithoutResidue) {
  rlimit file_size_limit{};
  ASSERT_EQ(::getrlimit(RLIMIT_FSIZE, &file_size_limit), 0);
  if (file_size_limit.rlim_max != RLIM_INFINITY &&
      file_size_limit.rlim_max < kLowHardFileSizeLimit) {
    GTEST_SKIP() << "hard RLIMIT_FSIZE is already below the probe limit";
  }

  EXPECT_EXIT(
      { ::_exit(run_file_size_envelope_rejection_probe()); },
      ::testing::ExitedWithCode(0), "");
}

TEST(WorkerSupervisor, CrashAndProtocolFaultsFailOnlyOwningAttempt) {
  ScopedSupervisorRoot root;
  auto service = make_service(root.path(), supervisor_options());
  const JobSubmission unrelated =
      service->submit(fixture_spec("fixture.slow.success"));
  const std::vector<std::pair<std::string, JobAttemptFailure>> cases{
      {"fixture.preaccept.nonzero", JobAttemptFailure::WorkerExit},
      {"fixture.nonzero", JobAttemptFailure::WorkerExit},
      {"fixture.signal", JobAttemptFailure::WorkerSignal},
      {"fixture.channel", JobAttemptFailure::WorkerChannel},
      {"fixture.malformed", JobAttemptFailure::WorkerProtocol},
      {"fixture.data.digest-mismatch", JobAttemptFailure::WorkerProtocol},
      {"fixture.stall", JobAttemptFailure::WorkerHeartbeatTimeout},
      {"fixture.report.hang", JobAttemptFailure::WorkerProtocol}};
  for (const auto& test_case : cases) {
    const JobSubmission submitted =
        service->submit(fixture_spec(test_case.first));
    const JobSnapshot terminal = wait_terminal(*service, submitted.job_id);
    EXPECT_EQ(terminal.state, JobState::Failed) << test_case.first;
    EXPECT_EQ(terminal.failure, test_case.second)
        << test_case.first << ": " << terminal.message;
    EXPECT_TRUE(terminal.attempt_settled) << test_case.first;
    EXPECT_EQ(terminal.attempt_outcome, JobAttemptOutcome::Failed)
        << test_case.first;
    EXPECT_FALSE(terminal.output_receipt.has_value()) << test_case.first;
    EXPECT_EQ(service->find_artifact(terminal.output_artifact_id), nullptr)
        << test_case.first;
  }
  EXPECT_EQ(wait_terminal(*service, unrelated.job_id).state,
            JobState::Succeeded);
}

TEST(WorkerSupervisor,
     DataPlaneJoinMismatchesFailWithoutWorkerQuotaOrArtifactResidue) {
  constexpr std::array<std::pair<std::string_view, std::string_view>, 3U> cases{
      {
          {"fixture.data.reference-mismatch",
           "worker report output metadata exceeds its assigned stage"},
          {"fixture.data.descriptor-mismatch",
           "worker output-stage size exceeds or differs from metadata"},
          {"fixture.data.stale-attempt",
           "worker report identity does not match its exact lease"},
      }};

  for (const auto& test_case : cases) {
    SCOPED_TRACE(std::string(test_case.first));
    ScopedSupervisorRoot root;
    auto service = make_service(root.path(), supervisor_options());
    const JobSubmission submitted =
        service->submit(fixture_spec(std::string(test_case.first)));
    const JobSnapshot terminal = wait_terminal(*service, submitted.job_id);

    EXPECT_EQ(terminal.state, JobState::Failed) << terminal.message;
    EXPECT_TRUE(terminal.attempt_settled);
    EXPECT_EQ(terminal.attempt_outcome, JobAttemptOutcome::Failed);
    EXPECT_EQ(terminal.failure, JobAttemptFailure::WorkerProtocol);
    EXPECT_NE(terminal.message.find(test_case.second), std::string::npos)
        << terminal.message;
    EXPECT_FALSE(terminal.output_receipt.has_value());
    EXPECT_EQ(service->find_artifact(terminal.output_artifact_id), nullptr);
    EXPECT_TRUE(
        SingleTenantJobServiceTestAccess::
            wait_for_owned_worker_thread_count_at_most(*service, 0U, 2s));
    EXPECT_EQ(
        SingleTenantJobServiceTestAccess::live_worker_process_count(*service),
        0U);
    const TenantQuotaSnapshot quota = service->quota_snapshot();
    EXPECT_EQ(quota.active_attempts, 0U);
    EXPECT_EQ(quota.cpu_slots, 0U);
    EXPECT_EQ(quota.host_memory_bytes, 0U);
    EXPECT_EQ(quota.output_bytes, 0U);
    EXPECT_EQ(quota.staging_bytes, 0U);
    EXPECT_EQ(quota.retention_bytes, 0U);
    EXPECT_EQ(quota.retained_artifacts, 0U);
    EXPECT_TRUE(quota.device_bytes.empty());
  }
}

TEST(WorkerSupervisor, ReassemblesReportAcrossMultiplePollSlices) {
  ScopedSupervisorRoot root;
  auto service = make_service(root.path(), supervisor_options());
  const JobSubmission submitted =
      service->submit(fixture_spec("fixture.fragmented.report"));

  const JobSnapshot terminal = wait_terminal(*service, submitted.job_id);

  EXPECT_EQ(terminal.state, JobState::Succeeded) << terminal.message;
  EXPECT_EQ(terminal.attempt_outcome, JobAttemptOutcome::Succeeded);
  EXPECT_EQ(terminal.failure, JobAttemptFailure::None);
}

/**
 * @brief Reassembles one fragmented Cancel across multiple receiver slices.
 * @return Nothing; GoogleTest reports lifecycle or identity drift.
 * @throws Fixture, service, protocol, process, and allocation failures.
 * @note The relay emits one exact Cancel header and payload in fragments whose
 * gaps cross receiver poll slices. The contract requires the complete frame to
 * remain decoder-owned through semantic identity interpretation and fresh
 * strict-before acceptance; successful acceptance must still publish
 * Cancelled/Cancelled/CancellationObserved. This end-to-end wall-clock case is
 * paired with a deterministic protocol test for the post-interpretation
 * deadline interleave.
 */
TEST(WorkerSupervisor, ReassemblesCancelAcrossMultiplePollSlices) {
  ScopedSupervisorRoot root;
  WorkerManagerOptions options = supervisor_options();
  options.cooperative_cancel_timeout = 300ms;
  auto service = make_service(root.path(), std::move(options));
  const JobSubmission submitted =
      service->submit(fixture_spec("fixture.fragmented.cancel"));
  ASSERT_TRUE(wait_until(
      [&] {
        const auto snapshot = service->query(submitted.job_id);
        return snapshot.has_value() && snapshot->state == JobState::Running;
      },
      2s));

  ASSERT_TRUE(service->cancel(submitted.job_id));
  const JobSnapshot terminal = wait_terminal(*service, submitted.job_id);

  EXPECT_EQ(terminal.state, JobState::Cancelled) << terminal.message;
  EXPECT_EQ(terminal.attempt_outcome, JobAttemptOutcome::Cancelled);
  EXPECT_EQ(terminal.failure, JobAttemptFailure::CancellationObserved);
}

TEST(WorkerSupervisor, RuntimeTimeoutTerminatesHeartbeatingWorker) {
  ScopedSupervisorRoot root;
  WorkerManagerOptions options = supervisor_options();
  options.attempt_runtime_timeout = 160ms;
  auto service = make_service(root.path(), std::move(options));
  const JobSubmission submitted =
      service->submit(fixture_spec("fixture.runtime"));
  const JobSnapshot terminal = wait_terminal(*service, submitted.job_id);
  EXPECT_EQ(terminal.state, JobState::Failed);
  EXPECT_EQ(terminal.failure, JobAttemptFailure::WorkerRuntimeTimeout)
      << terminal.message;
  EXPECT_EQ(
      SingleTenantJobServiceTestAccess::live_worker_process_count(*service),
      0U);
}

TEST(WorkerSupervisor,
     OutputProgressCannotRenewAnExpiredAuthenticatedHeartbeatDeadline) {
  ScopedSupervisorRoot root;
  WorkerManagerOptions options = supervisor_options();
  options.heartbeat_timeout = 150ms;
  options.attempt_runtime_timeout = 3s;
  options.post_report_timeout = 2s;
  options.io_timeout = 2s;
  auto service = make_service(root.path(), std::move(options));

  const JobSubmission submitted =
      service->submit(fixture_spec("fixture.output.no-heartbeat"));
  const JobSnapshot terminal = wait_terminal(*service, submitted.job_id);

  EXPECT_EQ(terminal.state, JobState::Failed) << terminal.message;
  EXPECT_TRUE(terminal.attempt_settled);
  EXPECT_EQ(terminal.attempt_outcome, JobAttemptOutcome::Failed);
  EXPECT_EQ(terminal.failure, JobAttemptFailure::WorkerHeartbeatTimeout)
      << terminal.message;
  EXPECT_NE(terminal.failure, JobAttemptFailure::WorkerRuntimeTimeout);
  EXPECT_FALSE(terminal.output_receipt.has_value());
  EXPECT_EQ(service->find_artifact(terminal.output_artifact_id), nullptr);
  EXPECT_TRUE(SingleTenantJobServiceTestAccess::
                  wait_for_owned_worker_thread_count_at_most(*service, 0U, 2s));
  EXPECT_EQ(
      SingleTenantJobServiceTestAccess::live_worker_process_count(*service),
      0U);
  const TenantQuotaSnapshot quota = service->quota_snapshot();
  EXPECT_EQ(quota.active_attempts, 0U);
  EXPECT_EQ(quota.cpu_slots, 0U);
  EXPECT_EQ(quota.host_memory_bytes, 0U);
  EXPECT_EQ(quota.output_bytes, 0U);
  EXPECT_EQ(quota.staging_bytes, 0U);
  EXPECT_EQ(quota.retention_bytes, 0U);
  EXPECT_EQ(quota.retained_artifacts, 0U);
  EXPECT_TRUE(quota.device_bytes.empty());
}

TEST(WorkerSupervisor, ReapDeadlineFailStopsWithoutBlockingFallback) {
  ScopedSupervisorRoot root;
  WorkerManagerOptions options = supervisor_options();
  options.cooperative_cancel_timeout = 50ms;
  options.terminate_timeout = 50ms;
  options.kill_reap_timeout = 50ms;
  options.io_timeout = 50ms;
  options.defer_reap_observation_for_test =
      std::make_shared<std::atomic<bool>>(true);

  EXPECT_DEATH(
      {
        auto service = make_service(root.path(), options);
        const JobSubmission submitted =
            service->submit(fixture_spec("fixture.ignore"));
        static_cast<void>(submitted);
        if (!wait_until(
                [&] {
                  return SingleTenantJobServiceTestAccess::
                             live_worker_process_count(*service) == 1U;
                },
                2s)) {
          throw std::runtime_error("death-test worker did not become live");
        }
        service.reset();
      },
      "exact worker was not reaped before the SIGKILL deadline");
}

TEST(WorkerSupervisor,
     CompletionReconstructionBadAllocFailStopsBeforeRecordRetirement) {
  WorkerManagerOptions options = supervisor_options();
  options.fail_completion_construction_for_test =
      std::make_shared<std::atomic<bool>>(true);

  EXPECT_EXIT(
      {
        try {
          WorkerManagerCallbacks callbacks;
          callbacks.begin_assignment = [](const AttemptIdentity&) -> bool {
            throw std::runtime_error(
                "injected primary supervision callback failure");
          };
          callbacks.cancellation_requested = [](const AttemptIdentity&) {
            return false;
          };
          callbacks.complete_assignment = [](WorkerManagerCompletion) {
            ::_exit(kCompletionCallbackInvoked);
          };
          WorkerManager manager(std::make_shared<FixtureWorkerFactory>(),
                                std::move(callbacks), options, false);
          manager.start(completion_failure_assignment());
          manager.shutdown();
          ::_exit(kCompletionRecordDeleted);
        } catch (...) {
          ::_exit(kCompletionProbeRaised);
        }
      },
      ::testing::KilledBySignal(SIGABRT),
      "completion fact could not be constructed or delivered");
}

TEST(WorkerSupervisor,
     FirstFailureCompletionBadAllocFailStopsBeforeCallbackOrRetirement) {
  EXPECT_EXIT(
      {
        provoke_initial_completion_construction_failure(
            WorkerManagerCompletionConstructionPointForTest::Failure, false,
            "fixture.nonzero");
      },
      ::testing::KilledBySignal(SIGABRT),
      "completion fact could not be constructed or delivered");
}

TEST(WorkerSupervisor,
     FirstForcedCancellationBadAllocFailStopsBeforeCallbackOrRetirement) {
  EXPECT_EXIT(
      {
        provoke_initial_completion_construction_failure(
            WorkerManagerCompletionConstructionPointForTest::ForcedCancellation,
            false, "fixture.ignore");
      },
      ::testing::KilledBySignal(SIGABRT),
      "completion fact could not be constructed or delivered");
}

TEST(WorkerSupervisor,
     FirstExternalReportBadAllocFailStopsBeforeCallbackOrRetirement) {
  EXPECT_EXIT(
      {
        provoke_initial_completion_construction_failure(
            WorkerManagerCompletionConstructionPointForTest::Report, false,
            "fixture.slow.success");
      },
      ::testing::KilledBySignal(SIGABRT),
      "completion fact could not be constructed or delivered");
}

TEST(WorkerSupervisor,
     FirstInProcessReportBadAllocFailStopsBeforeCallbackOrRetirement) {
  EXPECT_EXIT(
      {
        provoke_initial_completion_construction_failure(
            WorkerManagerCompletionConstructionPointForTest::Report, true,
            "fixture.in-process.report");
      },
      ::testing::KilledBySignal(SIGABRT),
      "completion fact could not be constructed or delivered");
}

TEST(WorkerSupervisor,
     CompletionCallbackExceptionFailStopsBeforeRecordRetirement) {
  WorkerManagerOptions options = supervisor_options();

  EXPECT_EXIT(
      {
        try {
          WorkerManagerCallbacks callbacks;
          callbacks.begin_assignment = [](const AttemptIdentity&) {
            return true;
          };
          callbacks.cancellation_requested = [](const AttemptIdentity&) {
            return false;
          };
          callbacks.complete_assignment = [](WorkerManagerCompletion) {
            throw std::runtime_error("injected completion callback fault");
          };
          WorkerManager manager(std::make_shared<FixtureWorkerFactory>(),
                                std::move(callbacks), options, false);
          manager.start(completion_failure_assignment("fixture.nonzero"));
          manager.shutdown();
          ::_exit(kCompletionRecordDeleted);
        } catch (...) {
          ::_exit(kCompletionProbeRaised);
        }
      },
      ::testing::KilledBySignal(SIGABRT),
      "completion fact could not be constructed or delivered");
}

TEST(WorkerSupervisor, CooperativeAndForcedCancellationRemainDistinct) {
  {
    ScopedSupervisorRoot root;
    auto heartbeat_observed = std::make_shared<std::atomic<bool>>(false);
    WorkerManagerOptions options = supervisor_options();
    options.heartbeat_timeout = 2s;
    options.cooperative_cancel_timeout = 2s;
    options.first_external_heartbeat_observed_for_test = heartbeat_observed;
    auto service = make_service(root.path(), std::move(options));
    const JobSubmission cooperative =
        service->submit(fixture_spec("fixture.cooperative"));

    ASSERT_TRUE(wait_until(
        [&] { return heartbeat_observed->load(std::memory_order_acquire); },
        2s));
    ASSERT_TRUE(service->cancel(cooperative.job_id));
    const JobSnapshot terminal = wait_terminal(*service, cooperative.job_id);

    EXPECT_EQ(terminal.state, JobState::Cancelled) << terminal.message;
    EXPECT_TRUE(terminal.attempt_settled);
    EXPECT_EQ(terminal.attempt_outcome, JobAttemptOutcome::Cancelled);
    EXPECT_EQ(terminal.failure, JobAttemptFailure::CancellationObserved);
    EXPECT_EQ(
        SingleTenantJobServiceTestAccess::live_worker_process_count(*service),
        0U);
  }

  {
    ScopedSupervisorRoot root;
    auto heartbeat_observed = std::make_shared<std::atomic<bool>>(false);
    WorkerManagerOptions options = supervisor_options();
    options.heartbeat_timeout = 2s;
    options.first_external_heartbeat_observed_for_test = heartbeat_observed;
    auto service = make_service(root.path(), std::move(options));
    const JobSubmission ignored =
        service->submit(fixture_spec("fixture.ignore"));

    ASSERT_TRUE(wait_until(
        [&] { return heartbeat_observed->load(std::memory_order_acquire); },
        2s));
    const auto started = std::chrono::steady_clock::now();
    ASSERT_TRUE(service->cancel(ignored.job_id));
    const JobSnapshot terminal = wait_terminal(*service, ignored.job_id);
    const auto elapsed = std::chrono::steady_clock::now() - started;

    EXPECT_EQ(terminal.state, JobState::Cancelled) << terminal.message;
    EXPECT_TRUE(terminal.attempt_settled);
    EXPECT_EQ(terminal.attempt_outcome, JobAttemptOutcome::Cancelled);
    EXPECT_EQ(terminal.failure, JobAttemptFailure::WorkerCancellationForced);
    EXPECT_LT(elapsed, 2s);
    EXPECT_EQ(
        SingleTenantJobServiceTestAccess::live_worker_process_count(*service),
        0U);
  }
}

TEST(WorkerSupervisor, ZeroExitZombieIsNotForcedCancellation) {
  ScopedSupervisorRoot root;
  WorkerManagerOptions options = supervisor_options();
  options.await_pre_signal_zero_exit_for_test = true;
  auto service = make_service(root.path(), std::move(options));
  const JobSubmission submitted =
      service->submit(fixture_spec("fixture.cancel-race.zero-exit-zombie"));
  ASSERT_TRUE(wait_until(
      [&] {
        const auto snapshot = service->query(submitted.job_id);
        return snapshot.has_value() && snapshot->state == JobState::Running;
      },
      2s));

  ASSERT_TRUE(service->cancel(submitted.job_id));
  const JobSnapshot terminal = wait_terminal(*service, submitted.job_id);

  EXPECT_EQ(terminal.state, JobState::Failed) << terminal.message;
  EXPECT_EQ(terminal.attempt_outcome, JobAttemptOutcome::Failed);
  EXPECT_EQ(terminal.failure, JobAttemptFailure::WorkerChannel)
      << terminal.message;
  EXPECT_NE(terminal.failure, JobAttemptFailure::WorkerCancellationForced);
}

TEST(WorkerSupervisor, CancelDeadlineReapMustDrainBufferedFailedReport) {
  ScopedSupervisorRoot root;
  WorkerManagerOptions options = supervisor_options();
  options.cooperative_cancel_timeout = 10ms;
  options.await_cancel_deadline_zero_exit_for_test = true;
  auto service = make_service(root.path(), std::move(options));
  const JobSubmission submitted =
      service->submit(fixture_spec("fixture.cancel-race.failed-report"));

  ASSERT_TRUE(service->cancel(submitted.job_id));
  const JobSnapshot terminal = wait_terminal(*service, submitted.job_id);

  EXPECT_EQ(terminal.state, JobState::Failed) << terminal.message;
  EXPECT_EQ(terminal.attempt_outcome, JobAttemptOutcome::Failed);
  EXPECT_EQ(terminal.failure, JobAttemptFailure::Compute) << terminal.message;
  EXPECT_EQ(terminal.message,
            "fixture preserved worker failure after cancel send closed");
  EXPECT_NE(terminal.failure, JobAttemptFailure::WorkerChannel);
  EXPECT_NE(terminal.failure, JobAttemptFailure::WorkerCancellationForced);
  EXPECT_TRUE(SingleTenantJobServiceTestAccess::
                  wait_for_owned_worker_thread_count_at_most(*service, 0U, 2s));
  EXPECT_EQ(
      SingleTenantJobServiceTestAccess::live_worker_process_count(*service),
      0U);
}

TEST(WorkerSupervisor, CancelSendFailurePreservesWorkerFailureAndExit) {
  /**
   * @brief Maps one fixture cancel-channel fault to its exact terminal truth.
   * @throws Nothing for aggregate initialization and value operations.
   */
  struct CancelRaceCase final {
    /** @brief Fixture behavior selected after closing its cancel read side. */
    const char* mode;
    /** @brief Exact expected worker-owned or wait-status failure. */
    JobAttemptFailure failure;
  };
  const std::array<CancelRaceCase, 4U> cases{{
      {"fixture.cancel-race.failed-report", JobAttemptFailure::Compute},
      {"fixture.cancel-race.nonzero", JobAttemptFailure::WorkerExit},
      {"fixture.cancel-race.signal", JobAttemptFailure::WorkerSignal},
      {"fixture.cancel-race.channel-close", JobAttemptFailure::WorkerChannel},
  }};

  ScopedSupervisorRoot root;
  auto service = make_service(root.path(), supervisor_options());
  for (std::size_t repetition = 0U; repetition < 3U; ++repetition) {
    for (const CancelRaceCase& test_case : cases) {
      const JobSubmission submitted =
          service->submit(fixture_spec(test_case.mode));
      ASSERT_TRUE(service->cancel(submitted.job_id))
          << test_case.mode << " repetition " << repetition;
      const JobSnapshot terminal = wait_terminal(*service, submitted.job_id);
      EXPECT_EQ(terminal.state, JobState::Failed)
          << test_case.mode << " repetition " << repetition << ": "
          << terminal.message;
      EXPECT_EQ(terminal.attempt_outcome, JobAttemptOutcome::Failed)
          << test_case.mode << " repetition " << repetition;
      EXPECT_EQ(terminal.failure, test_case.failure)
          << test_case.mode << " repetition " << repetition << ": "
          << terminal.message;
      EXPECT_NE(terminal.failure, JobAttemptFailure::WorkerCancellationForced);
    }
  }
}

TEST(WorkerSupervisor,
     CancelChannelFailureCannotEraseObservedSignalWaitStatus) {
  ScopedSupervisorRoot root;
  WorkerManagerOptions options = supervisor_options();
  options.await_cancel_channel_failure_exit_for_test = true;
  options.inject_cancel_channel_failure_for_test = true;
  auto service = make_service(root.path(), std::move(options));
  const JobSubmission submitted =
      service->submit(fixture_spec("fixture.cancel-race.signal"));

  ASSERT_TRUE(service->cancel(submitted.job_id));
  const JobSnapshot terminal = wait_terminal(*service, submitted.job_id);

  EXPECT_EQ(terminal.state, JobState::Failed) << terminal.message;
  EXPECT_EQ(terminal.attempt_outcome, JobAttemptOutcome::Failed);
  EXPECT_EQ(terminal.failure, JobAttemptFailure::WorkerSignal)
      << terminal.message;
  EXPECT_NE(terminal.failure, JobAttemptFailure::WorkerChannel);
  EXPECT_NE(terminal.failure, JobAttemptFailure::WorkerCancellationForced);
}

TEST(
    WorkerSupervisor,
    CancelChannelFailureSignalBeforeCooperativeDeadlineOutranksShortPostReportTimeout) {
  ScopedSupervisorRoot root;
  WorkerManagerOptions options = supervisor_options();
  options.post_report_timeout = 10ms;
  options.cooperative_cancel_timeout = 1s;
  options.inject_cancel_channel_failure_for_test = true;
  auto service = make_service(root.path(), std::move(options));
  const JobSubmission submitted =
      service->submit(fixture_spec("fixture.cancel-race.delayed-signal"));

  ASSERT_TRUE(service->cancel(submitted.job_id));
  const JobSnapshot terminal = wait_terminal(*service, submitted.job_id);

  EXPECT_EQ(terminal.state, JobState::Failed) << terminal.message;
  EXPECT_EQ(terminal.attempt_outcome, JobAttemptOutcome::Failed);
  EXPECT_EQ(terminal.failure, JobAttemptFailure::WorkerSignal)
      << terminal.message;
  EXPECT_NE(terminal.failure, JobAttemptFailure::WorkerChannel);
  EXPECT_NE(terminal.failure, JobAttemptFailure::WorkerCancellationForced);
}

TEST(WorkerSupervisor,
     CandidateReportDeadlineCannotPreemptActiveCancellationSignalTruth) {
  ScopedSupervisorRoot root;
  WorkerManagerOptions options = supervisor_options();
  options.post_report_timeout = 10ms;
  options.cooperative_cancel_timeout = 1s;
  auto service = make_service(root.path(), std::move(options));
  const JobSubmission submitted = service->submit(
      fixture_spec("fixture.cancel-race.report-delayed-signal"));
  ASSERT_TRUE(wait_until(
      [&] {
        const auto snapshot = service->query(submitted.job_id);
        return snapshot.has_value() && snapshot->state == JobState::Running;
      },
      2s));

  ASSERT_TRUE(service->cancel(submitted.job_id));
  const JobSnapshot terminal = wait_terminal(*service, submitted.job_id);

  EXPECT_EQ(terminal.state, JobState::Failed) << terminal.message;
  EXPECT_EQ(terminal.attempt_outcome, JobAttemptOutcome::Failed);
  EXPECT_EQ(terminal.failure, JobAttemptFailure::WorkerSignal)
      << terminal.message;
  EXPECT_EQ(terminal.message,
            "worker died by signal 9 (OOM-compatible SIGKILL)");
  EXPECT_NE(terminal.failure, JobAttemptFailure::WorkerProtocol);
  EXPECT_NE(terminal.failure, JobAttemptFailure::WorkerCancellationForced);
  EXPECT_TRUE(SingleTenantJobServiceTestAccess::
                  wait_for_owned_worker_thread_count_at_most(*service, 0U, 2s));
  EXPECT_EQ(
      SingleTenantJobServiceTestAccess::live_worker_process_count(*service),
      0U);
}

TEST(WorkerSupervisor, BulkOutputAndCheckpointUseDataPlaneAndCommit) {
  ScopedSupervisorRoot root;
  auto heartbeat_observed = std::make_shared<std::atomic<bool>>(false);
  auto pending_heartbeat_ordinal =
      std::make_shared<std::atomic<std::uint64_t>>(0U);
  auto defer_output = std::make_shared<std::atomic<bool>>(true);
  auto output_paused = std::make_shared<std::atomic<bool>>(false);
  WorkerManagerOptions options = supervisor_options();
  options.heartbeat_timeout = kBulkHeartbeatEvidenceTimeout;
  options.attempt_runtime_timeout = 5s;
  options.io_timeout = 2s;
  options.first_external_heartbeat_observed_for_test = heartbeat_observed;
  options.latest_output_pending_heartbeat_ordinal_for_test =
      pending_heartbeat_ordinal;
  options.defer_output_drain_for_test = defer_output;
  options.output_transfer_paused_for_test = output_paused;
  auto service = make_service(root.path(), std::move(options), nullptr,
                              bulk_data_plane_quota());
  const JobSpec spec(GraphArtifactId("fixture.former-control-bound-output"), 0,
                     OutputSlotId("image.final"), bulk_data_plane_resources());

  const JobSubmission submitted = service->submit(spec);
  ASSERT_TRUE(wait_until(
      [&] { return output_paused->load(std::memory_order_acquire); }, 3s));
  const std::uint64_t heartbeat_ordinal_before_crossing =
      pending_heartbeat_ordinal->load(std::memory_order_acquire);
  const auto pending_started = std::chrono::steady_clock::now();
  std::this_thread::sleep_until(pending_started +
                                kBulkHeartbeatEvidenceTimeout +
                                kBulkHeartbeatEvidenceMargin);
  const auto pending_elapsed =
      std::chrono::steady_clock::now() - pending_started;
  const std::optional<JobSnapshot> still_pending =
      service->query(submitted.job_id);
  const std::uint64_t heartbeat_ordinal_after_crossing =
      pending_heartbeat_ordinal->load(std::memory_order_acquire);
  defer_output->store(false, std::memory_order_release);
  const JobSnapshot terminal = wait_terminal(*service, submitted.job_id);

  ASSERT_TRUE(still_pending.has_value());
  EXPECT_FALSE(is_terminal_job_state(still_pending->state));
  EXPECT_GE(pending_elapsed, kBulkHeartbeatEvidenceTimeout);
  EXPECT_GE(heartbeat_ordinal_after_crossing, 2U);
  EXPECT_GT(heartbeat_ordinal_after_crossing,
            heartbeat_ordinal_before_crossing);
  EXPECT_EQ(terminal.assignment, submitted.assignment);
  EXPECT_EQ(terminal.state, JobState::Succeeded) << terminal.message;
  EXPECT_EQ(terminal.attempt_outcome, JobAttemptOutcome::Succeeded);
  EXPECT_TRUE(terminal.attempt_settled);
  EXPECT_EQ(terminal.failure, JobAttemptFailure::None);
  EXPECT_TRUE(heartbeat_observed->load(std::memory_order_acquire));
  ASSERT_TRUE(terminal.output_receipt.has_value());
  const std::shared_ptr<const ArtifactRecord> artifact =
      service->find_artifact(terminal.output_artifact_id);
  ASSERT_NE(artifact, nullptr);
  EXPECT_EQ(artifact->payload.size(), kBulkPayloadAboveFormerControlBytes);
  EXPECT_EQ(artifact->receipt.artifact_id,
            terminal.output_receipt->artifact_id);
  EXPECT_EQ(artifact->receipt.output_commit_id,
            terminal.output_receipt->output_commit_id);
  EXPECT_EQ(artifact->receipt.content_digest,
            terminal.output_receipt->content_digest);

  const JobSubmission consumer = service->submit(
      JobSpec(GraphArtifactId("fixture.checkpoint"), 0,
              OutputSlotId("image.checkpoint-consumer"),
              bulk_data_plane_resources(), terminal.output_artifact_id));
  const JobSnapshot consumed = wait_terminal(*service, consumer.job_id);
  EXPECT_EQ(consumed.state, JobState::Succeeded) << consumed.message;
  EXPECT_EQ(consumed.attempt_outcome, JobAttemptOutcome::Succeeded);
  EXPECT_EQ(consumed.failure, JobAttemptFailure::None);
  ASSERT_TRUE(consumed.output_receipt.has_value());
  const std::shared_ptr<const ArtifactRecord> consumer_artifact =
      service->find_artifact(consumed.output_artifact_id);
  ASSERT_NE(consumer_artifact, nullptr);
  EXPECT_EQ(consumer_artifact->payload.size(), sizeof(std::uint32_t));

  const TenantQuotaSnapshot quota = service->quota_snapshot();
  EXPECT_EQ(quota.active_attempts, 0U);
  EXPECT_EQ(quota.cpu_slots, 0U);
  EXPECT_EQ(quota.host_memory_bytes, 0U);
  EXPECT_EQ(quota.output_bytes, 0U);
  EXPECT_EQ(quota.staging_bytes, 0U);
  EXPECT_EQ(quota.retention_bytes,
            artifact->payload.size() + consumer_artifact->payload.size());
  EXPECT_EQ(quota.retained_artifacts, 2U);
  for (const auto& device : quota.device_bytes) {
    EXPECT_EQ(device.second, 0U);
  }
  EXPECT_TRUE(SingleTenantJobServiceTestAccess::
                  wait_for_owned_worker_thread_count_at_most(*service, 0U, 2s));
  EXPECT_EQ(
      SingleTenantJobServiceTestAccess::live_worker_process_count(*service),
      0U);
}

TEST(WorkerSupervisor,
     BulkOutputWithoutPostReportHeartbeatsTimesOutWithoutResidue) {
  ScopedSupervisorRoot root;
  auto heartbeat_observed = std::make_shared<std::atomic<bool>>(false);
  auto defer_output = std::make_shared<std::atomic<bool>>(true);
  auto output_paused = std::make_shared<std::atomic<bool>>(false);
  WorkerManagerOptions options = supervisor_options();
  options.heartbeat_timeout = kBulkHeartbeatEvidenceTimeout;
  options.attempt_runtime_timeout = 5s;
  options.io_timeout = 2s;
  options.first_external_heartbeat_observed_for_test = heartbeat_observed;
  options.defer_output_drain_for_test = defer_output;
  options.output_transfer_paused_for_test = output_paused;
  auto service = make_service(root.path(), std::move(options), nullptr,
                              bulk_data_plane_quota());
  const JobSpec spec(
      GraphArtifactId(
          "fixture.former-control-bound-output.first-heartbeat-only"),
      0, OutputSlotId("image.final"), bulk_data_plane_resources());

  const JobSubmission submitted = service->submit(spec);
  ASSERT_TRUE(wait_until(
      [&] { return output_paused->load(std::memory_order_acquire); }, 3s));
  const JobSnapshot terminal = wait_terminal(*service, submitted.job_id);
  defer_output->store(false, std::memory_order_release);

  EXPECT_EQ(terminal.state, JobState::Failed) << terminal.message;
  EXPECT_TRUE(terminal.attempt_settled);
  EXPECT_EQ(terminal.attempt_outcome, JobAttemptOutcome::Failed);
  EXPECT_EQ(terminal.failure, JobAttemptFailure::WorkerHeartbeatTimeout)
      << terminal.message;
  EXPECT_NE(terminal.failure, JobAttemptFailure::WorkerRuntimeTimeout);
  EXPECT_TRUE(heartbeat_observed->load(std::memory_order_acquire));
  EXPECT_FALSE(terminal.output_receipt.has_value());
  EXPECT_EQ(service->find_artifact(terminal.output_artifact_id), nullptr);
  EXPECT_TRUE(SingleTenantJobServiceTestAccess::
                  wait_for_owned_worker_thread_count_at_most(*service, 0U, 2s));
  EXPECT_EQ(
      SingleTenantJobServiceTestAccess::live_worker_process_count(*service),
      0U);
  const TenantQuotaSnapshot quota = service->quota_snapshot();
  EXPECT_EQ(quota.active_attempts, 0U);
  EXPECT_EQ(quota.cpu_slots, 0U);
  EXPECT_EQ(quota.host_memory_bytes, 0U);
  EXPECT_EQ(quota.output_bytes, 0U);
  EXPECT_EQ(quota.staging_bytes, 0U);
  EXPECT_EQ(quota.retention_bytes, 0U);
  EXPECT_EQ(quota.retained_artifacts, 0U);
  EXPECT_TRUE(quota.device_bytes.empty());
}

TEST(WorkerSupervisor, StaleLeaseCannotCancelFreshRetryProcess) {
  ScopedSupervisorRoot root;
  const std::filesystem::path fifo = root.path() / "worker-retry.fifo";
  create_worker_filesystem_fifo(fifo);
  const GraphArtifactId graph_id("fixture.retry.hold");
  auto factory = std::make_shared<FixtureWorkerFactory>(
      filesystem_fixture_catalog(graph_id, fifo));
  auto service =
      make_service(root.path(), supervisor_options(), std::move(factory));
  const JobSubmission first = service->submit(fixture_spec(graph_id.value()));
  const JobSnapshot failed = wait_terminal(*service, first.job_id);
  ASSERT_EQ(failed.failure, JobAttemptFailure::WorkerExit);
  ASSERT_TRUE(SingleTenantJobServiceTestAccess::
                  wait_for_owned_worker_thread_count_at_most(*service, 0U, 2s));
  const TenantQuotaSnapshot released = service->quota_snapshot();
  EXPECT_EQ(released.active_attempts, 0U);

  const std::optional<JobSubmission> retry = service->retry(first.job_id);
  ASSERT_TRUE(retry.has_value());
  ScopedProbeDescriptor writer(wait_for_worker_filesystem_reader(fifo, 2s));
  ASSERT_EQ(
      SingleTenantJobServiceTestAccess::live_worker_process_count(*service),
      1U);
  EXPECT_FALSE(SingleTenantJobServiceTestAccess::request_exact_worker_cancel(
      *service, first.assignment));
  release_worker_filesystem_reader(writer.get());
  writer.reset();
  const JobSnapshot succeeded = wait_terminal(*service, retry->job_id);
  EXPECT_EQ(succeeded.state, JobState::Succeeded);
  EXPECT_NE(retry->assignment.worker_instance_id,
            first.assignment.worker_instance_id);
  EXPECT_NE(retry->assignment.worker_lease_generation,
            first.assignment.worker_lease_generation);
  EXPECT_EQ(succeeded.output_artifact_id, failed.output_artifact_id);
  EXPECT_EQ(succeeded.output_commit_id, failed.output_commit_id);
}

TEST(WorkerSupervisor, RetryCheckpointAndRestartPreserveDurableAuthority) {
  ScopedSupervisorRoot root;
  JobId completed_job_id;
  ArtifactId checkpoint_id;
  OutputCommitReceipt checkpoint_receipt;
  std::vector<std::byte> checkpoint_payload;
  {
    auto service = make_service(root.path(), supervisor_options());
    const JobSubmission first = service->submit(fixture_spec("fixture.retry"));
    completed_job_id = first.job_id;
    ASSERT_EQ(wait_terminal(*service, first.job_id).state, JobState::Failed);
    ASSERT_TRUE(
        SingleTenantJobServiceTestAccess::
            wait_for_owned_worker_thread_count_at_most(*service, 0U, 2s));
    const std::optional<JobSubmission> retry = service->retry(first.job_id);
    ASSERT_TRUE(retry.has_value());
    const JobSnapshot succeeded = wait_terminal(*service, first.job_id);
    ASSERT_EQ(succeeded.state, JobState::Succeeded);
    ASSERT_TRUE(succeeded.output_receipt.has_value());
    ASSERT_NE(succeeded.spec, nullptr);
    EXPECT_EQ(succeeded.assignment, retry->assignment);
    EXPECT_EQ(succeeded.assignment.job_spec_digest, retry->job_spec_digest);
    EXPECT_EQ(succeeded.output_receipt->attempt, retry->assignment);
    EXPECT_EQ(succeeded.output_receipt->attempt.job_spec_digest,
              succeeded.spec->digest());
    EXPECT_EQ(succeeded.output_receipt->achieved_durability,
              ArtifactDurability::CrashDurable);
    checkpoint_id = succeeded.output_receipt->artifact_id;
    checkpoint_receipt = *succeeded.output_receipt;
    const std::shared_ptr<const ArtifactRecord> artifact =
        service->find_artifact(checkpoint_id);
    ASSERT_NE(artifact, nullptr);
    EXPECT_EQ(artifact->receipt.attempt, checkpoint_receipt.attempt);
    EXPECT_EQ(artifact->receipt.artifact_id, checkpoint_id);
    EXPECT_EQ(artifact->receipt.output_commit_id,
              checkpoint_receipt.output_commit_id);
    EXPECT_EQ(artifact->receipt.content_digest,
              checkpoint_receipt.content_digest);
    EXPECT_EQ(artifact->receipt.achieved_durability,
              ArtifactDurability::CrashDurable);
    EXPECT_EQ(artifact->receipt.content_digest,
              hash_artifact_content(artifact->payload.data(),
                                    artifact->payload.size()));
    checkpoint_payload = artifact->payload;
    const TenantQuotaSnapshot quota = service->quota_snapshot();
    EXPECT_EQ(quota.active_attempts, 0U);
    EXPECT_EQ(quota.retained_artifacts, 1U);
    EXPECT_EQ(quota.retention_bytes, checkpoint_payload.size());
    EXPECT_TRUE(
        SingleTenantJobServiceTestAccess::
            wait_for_owned_worker_thread_count_at_most(*service, 0U, 2s));
    EXPECT_EQ(
        SingleTenantJobServiceTestAccess::live_worker_process_count(*service),
        0U);
  }
  {
    auto recovered = make_service(root.path(), supervisor_options());
    const std::optional<JobSnapshot> prior = recovered->query(completed_job_id);
    ASSERT_TRUE(prior.has_value());
    ASSERT_EQ(prior->state, JobState::Succeeded);
    ASSERT_TRUE(prior->output_receipt.has_value());
    EXPECT_EQ(prior->output_receipt->artifact_id, checkpoint_id);
    EXPECT_EQ(prior->output_receipt->attempt, checkpoint_receipt.attempt);
    EXPECT_EQ(prior->output_receipt->content_digest,
              checkpoint_receipt.content_digest);
    EXPECT_EQ(prior->output_receipt->achieved_durability,
              ArtifactDurability::CrashDurable);
    const std::shared_ptr<const ArtifactRecord> recovered_checkpoint =
        recovered->find_artifact(checkpoint_id);
    ASSERT_NE(recovered_checkpoint, nullptr);
    EXPECT_EQ(recovered_checkpoint->payload, checkpoint_payload);
    EXPECT_EQ(recovered_checkpoint->receipt.content_digest,
              checkpoint_receipt.content_digest);
    EXPECT_EQ(recovered_checkpoint->receipt.achieved_durability,
              ArtifactDurability::CrashDurable);
    const TenantQuotaSnapshot recovered_quota = recovered->quota_snapshot();
    EXPECT_EQ(recovered_quota.active_attempts, 0U);
    EXPECT_EQ(recovered_quota.retained_artifacts, 1U);
    EXPECT_EQ(recovered_quota.retention_bytes, checkpoint_payload.size());
    const JobSubmission checkpoint =
        recovered->submit(fixture_spec("fixture.checkpoint", checkpoint_id));
    const JobSnapshot terminal = wait_terminal(*recovered, checkpoint.job_id);
    EXPECT_EQ(terminal.state, JobState::Succeeded);
    ASSERT_NE(terminal.spec, nullptr);
    ASSERT_TRUE(terminal.spec->checkpoint_artifact_id().has_value());
    EXPECT_EQ(*terminal.spec->checkpoint_artifact_id(), checkpoint_id);
    EXPECT_EQ(terminal.assignment, checkpoint.assignment);
    EXPECT_EQ(terminal.assignment.job_spec_digest, checkpoint.job_spec_digest);
    EXPECT_NE(terminal.assignment.job_id, checkpoint_receipt.attempt.job_id);
    EXPECT_NE(terminal.assignment.worker_instance_id,
              checkpoint_receipt.attempt.worker_instance_id);
    ASSERT_TRUE(terminal.output_receipt.has_value());
    EXPECT_EQ(terminal.output_receipt->attempt, checkpoint.assignment);
    EXPECT_EQ(terminal.output_receipt->achieved_durability,
              ArtifactDurability::CrashDurable);
    const std::shared_ptr<const ArtifactRecord> reused_checkpoint =
        recovered->find_artifact(checkpoint_id);
    ASSERT_NE(reused_checkpoint, nullptr);
    EXPECT_EQ(reused_checkpoint->payload, checkpoint_payload);
    EXPECT_EQ(reused_checkpoint->receipt.content_digest,
              checkpoint_receipt.content_digest);
    const TenantQuotaSnapshot final_quota = recovered->quota_snapshot();
    EXPECT_EQ(final_quota.active_attempts, 0U);
    EXPECT_EQ(final_quota.retained_artifacts, 2U);
    EXPECT_EQ(final_quota.retention_bytes,
              checkpoint_payload.size() +
                  terminal.output_receipt->descriptor.payload_bytes);
    EXPECT_TRUE(
        SingleTenantJobServiceTestAccess::
            wait_for_owned_worker_thread_count_at_most(*recovered, 0U, 2s));
    EXPECT_EQ(
        SingleTenantJobServiceTestAccess::live_worker_process_count(*recovered),
        0U);
  }
}

TEST(WorkerSupervisor, ShutdownDrainsIgnoringWorkersWithinConcurrentBound) {
  ScopedSupervisorRoot root;
  auto service = make_service(root.path(), supervisor_options());
  static_cast<void>(service->submit(fixture_spec("fixture.ignore")));
  static_cast<void>(service->submit(fixture_spec("fixture.ignore")));
  ASSERT_TRUE(wait_until(
      [&] {
        return SingleTenantJobServiceTestAccess::live_worker_process_count(
                   *service) == 2U;
      },
      2s));
  const auto started = std::chrono::steady_clock::now();
  service.reset();
  EXPECT_LT(std::chrono::steady_clock::now() - started, 2s);
}

TEST(WorkerSupervisor,
     ProductConstructionRejectsEveryOversizedDurationBeforeDurableOwnership) {
  ScopedSupervisorRoot root;
  for (std::size_t field_index = 0U;
       field_index < kWorkerDurationFieldCases.size(); ++field_index) {
    const WorkerDurationFieldCase& field =
        kWorkerDurationFieldCases[field_index];
    const std::array<std::chrono::milliseconds, 2U> rejected_values{
        field.maximum + 1ms, std::chrono::milliseconds::max()};
    for (std::size_t value_index = 0U; value_index < rejected_values.size();
         ++value_index) {
      SCOPED_TRACE(std::string(field.name) + " candidate " +
                   std::to_string(rejected_values[value_index].count()));
      const std::filesystem::path rejected_root =
          root.path() / ("duration-rejected-" + std::to_string(field_index) +
                         "-" + std::to_string(value_index));
      WorkerManagerOptions options = supervisor_options();
      options.*(field.member) = rejected_values[value_index];
      bool rejected = false;
      try {
        auto service = make_service(rejected_root, std::move(options));
        static_cast<void>(service);
      } catch (const std::invalid_argument& error) {
        rejected = true;
        EXPECT_NE(std::string(error.what()).find(field.name),
                  std::string::npos);
      } catch (...) {
        ADD_FAILURE() << "oversized duration raised the wrong exception";
      }
      EXPECT_TRUE(rejected);
      EXPECT_FALSE(std::filesystem::exists(rejected_root));
    }
  }
}

TEST(WorkerSupervisor, ExactDurationBoundsPreserveClosedLaunchPolicy) {
  ScopedSupervisorRoot root;
  WorkerManagerOptions boundary_options = supervisor_options();
  for (const WorkerDurationFieldCase& field : kWorkerDurationFieldCases) {
    boundary_options.*(field.member) = field.maximum;
  }
  auto service = make_service(root.path() / "duration-exact-bound",
                              std::move(boundary_options));
  ASSERT_NE(service, nullptr);
  service.reset();

  WorkerProcessLaunchOptions launch{3, 5, 6, kMaximumWorkerDuration,
                                    kMaximumWorkerDuration};
  WorkerProcessLaunchArguments arguments =
      make_worker_process_launch_arguments(launch);
  char executable[] = "photospider-worker";
  std::array<char*, 6U> argv{executable,
                             arguments.control_fd.data(),
                             arguments.checkpoint_data_fd.data(),
                             arguments.output_data_fd.data(),
                             arguments.startup_timeout.data(),
                             arguments.io_timeout.data()};
  const WorkerProcessLaunchOptions parsed = parse_worker_process_launch_options(
      static_cast<int>(argv.size()), argv.data());
  EXPECT_EQ(parsed.startup_timeout, kMaximumWorkerDuration);
  EXPECT_EQ(parsed.io_timeout, kMaximumWorkerDuration);

  launch.startup_timeout = kMaximumWorkerDuration + 1ms;
  EXPECT_THROW(make_worker_process_launch_arguments(launch),
               std::invalid_argument);

  WorkerManagerOptions tied = supervisor_options();
  tied.heartbeat_interval = 100ms;
  tied.heartbeat_timeout = 100ms;
  const std::filesystem::path tied_root =
      root.path() / "duration-heartbeat-tie";
  EXPECT_THROW(make_service(tied_root, std::move(tied)), std::invalid_argument);
  EXPECT_FALSE(std::filesystem::exists(tied_root));
}

TEST(WorkerSupervisor, CheckedDeadlineRejectsClockRangeOverflow) {
  const auto increment = validate_and_convert_worker_duration(
      1ms, kMaximumWorkerDuration, "synthetic_timeout");
  const auto latest_base =
      std::chrono::steady_clock::time_point::max() - increment;
  EXPECT_EQ(checked_worker_deadline(latest_base, 1ms),
            std::chrono::steady_clock::time_point::max());
  EXPECT_THROW(checked_worker_deadline(
                   latest_base + std::chrono::steady_clock::duration{1}, 1ms),
               std::overflow_error);
  EXPECT_NO_THROW(checked_worker_deadline(
      std::chrono::steady_clock::time_point{}, kMaximumWorkerDuration));
  EXPECT_THROW(checked_worker_deadline(std::chrono::steady_clock::time_point{},
                                       std::chrono::milliseconds::max()),
               std::invalid_argument);
}

TEST(WorkerSupervisor, ProductConstructionRejectsUnmarkedOrMissingExecutable) {
  ScopedSupervisorRoot root;
  WorkerManagerOptions valid = supervisor_options();
  const std::filesystem::path untouched_root =
      root.path() / "invalid-service-root";
  EXPECT_THROW(SingleTenantJobService(TenantId("tenant.supervisor"),
                                      supervisor_quota(), untouched_root,
                                      std::make_shared<UnmarkedWorkerFactory>(),
                                      DurableServerStateOptions{},
                                      TenantQuotaAuthorityOptions{}, valid),
               std::invalid_argument);
  EXPECT_FALSE(std::filesystem::exists(untouched_root));

  WorkerManagerOptions missing = supervisor_options();
  missing.worker_executable = root.path() / "missing-worker";
  EXPECT_THROW(SingleTenantJobService(TenantId("tenant.supervisor"),
                                      supervisor_quota(), root.path(),
                                      std::make_shared<FixtureWorkerFactory>(),
                                      DurableServerStateOptions{},
                                      TenantQuotaAuthorityOptions{}, missing),
               std::invalid_argument);
}

TEST(WorkerSupervisor, ProductConstructionRejectsAutoReapingSigchldPolicies) {
  ScopedSupervisorRoot root;
  EXPECT_EXIT(
      {
        ::_exit(probe_sigchld_construction_rejection(
            root.path() / "sigign-construction", true, 0));
      },
      ::testing::ExitedWithCode(0), "");
#ifdef SA_NOCLDWAIT
  EXPECT_EXIT(
      {
        ::_exit(probe_sigchld_construction_rejection(
            root.path() / "nocldwait-construction", false, SA_NOCLDWAIT));
      },
      ::testing::ExitedWithCode(0), "");
#endif
}

TEST(WorkerSupervisor, AutoReapAfterSpawnFailStopsReapingAuthority) {
  ScopedSupervisorRoot root;
  EXPECT_EXIT(
      { provoke_reap_authority_loss(root.path() / "sigign-runtime", true, 0); },
      ::testing::KilledBySignal(SIGABRT),
      "exact worker reaping authority was lost");
#ifdef SA_NOCLDWAIT
  EXPECT_EXIT(
      {
        provoke_reap_authority_loss(root.path() / "nocldwait-runtime", false,
                                    SA_NOCLDWAIT);
      },
      ::testing::KilledBySignal(SIGABRT),
      "exact worker reaping authority was lost");
#endif
}

}  // namespace
}  // namespace ps::server
