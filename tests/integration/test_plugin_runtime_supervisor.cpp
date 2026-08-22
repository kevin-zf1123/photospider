/**
 * @file test_plugin_runtime_supervisor.cpp
 * @brief Verifies the product-linked isolated CPU runtime supervisor.
 */
#include <dirent.h>
#include <fcntl.h>
#include <gtest/gtest.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <array>
#include <atomic>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <filesystem>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "compute/compute_run.hpp"  // NOLINT(build/include_subdir)
#include "compute/execution/execution_service.hpp"  // NOLINT(build/include_subdir)
#include "execution/device/plugin_runtime_supervisor.hpp"  // NOLINT(build/include_subdir)
#include "execution/execution_task_runtime.hpp"  // NOLINT(build/include_subdir)
#include "execution/isolation/isolated_cpu_invocation_test_probe.hpp"  // NOLINT(build/include_subdir)
#include "plugin/operation_runtime_router.hpp"  // NOLINT(build/include_subdir)

#ifndef PS_TEST_ISOLATED_CPU_FIXTURE_PATH
#error "PS_TEST_ISOLATED_CPU_FIXTURE_PATH must name the runtime fixture"
#endif
#ifndef PS_TEST_PLUGIN_RUNTIME_SILENT_FIXTURE_PATH
#error "PS_TEST_PLUGIN_RUNTIME_SILENT_FIXTURE_PATH must name the silent fixture"
#endif
#ifndef PS_TEST_PLUGIN_RUNTIME_CORRUPT_FIXTURE_PATH
#error \
    "PS_TEST_PLUGIN_RUNTIME_CORRUPT_FIXTURE_PATH must name the corrupt fixture"
#endif

namespace ps::execution {
namespace {

/**
 * @brief Captured public facts from one expected supervisor failure.
 * @throws std::bad_alloc when copied diagnostic storage cannot allocate.
 */
struct ObservedRuntimeFault final {
  /** @brief Primary factual classification. */
  PluginRuntimeFaultKind kind = PluginRuntimeFaultKind::Channel;
  /** @brief Exact wait status when reconciled. */
  std::optional<int> wait_status;
  /** @brief Natural nonzero exit code when applicable. */
  std::optional<int> exit_code;
  /** @brief Terminating signal when applicable. */
  std::optional<int> signal_number;
  /** @brief Strongest supervisor termination action. */
  PluginRuntimeTerminationStage termination_stage =
      PluginRuntimeTerminationStage::None;
  /** @brief `SIGKILL` compatibility observation without OOM causation. */
  bool memory_pressure_compatible = false;
  /** @brief Host-owned diagnostic text. */
  std::string diagnostic;
};

#if defined(__linux__)
/**
 * @brief Unique owner for one supervisor-test POSIX descriptor.
 * @throws Nothing for construction and destruction.
 */
class ScopedSupervisorTestFd final {
 public:
  /**
   * @brief Takes ownership of one descriptor or invalid sentinel.
   * @param descriptor Descriptor to close at destruction.
   * @throws Nothing.
   */
  explicit ScopedSupervisorTestFd(int descriptor) noexcept
      : descriptor_(descriptor) {}

  /** @brief Closes the retained descriptor once without throwing. */
  ~ScopedSupervisorTestFd() noexcept {
    if (descriptor_ >= 0) {
      static_cast<void>(::close(descriptor_));
    }
  }

  /** @brief Prevents duplicate descriptor ownership. */
  ScopedSupervisorTestFd(const ScopedSupervisorTestFd&) = delete;
  /** @brief Prevents duplicate descriptor assignment. */
  ScopedSupervisorTestFd& operator=(const ScopedSupervisorTestFd&) = delete;

  /**
   * @brief Returns the retained descriptor.
   * @return Descriptor or invalid sentinel.
   * @throws Nothing.
   */
  int get() const noexcept { return descriptor_; }

 private:
  /** @brief Sole owned descriptor. */
  int descriptor_ = -1;
};

/**
 * @brief Replaces one preopened supervised runtime source in place.
 * @param descriptor Writable descriptor opened before supervisor construction.
 * @return Nothing after truncation, replacement, and durable flush.
 * @throws std::system_error when any exact mutation operation fails.
 */
void overwrite_supervised_runtime_source(int descriptor) {
  if (::ftruncate(descriptor, 0) != 0) {
    throw std::system_error(errno, std::generic_category(),
                            "truncate supervised runtime source");
  }
  constexpr std::string_view kReplacement = "not an executable runtime\n";
  std::size_t offset = 0U;
  while (offset < kReplacement.size()) {
    const ssize_t written =
        ::pwrite(descriptor, kReplacement.data() + offset,
                 kReplacement.size() - offset, static_cast<off_t>(offset));
    if (written < 0 && errno == EINTR) {
      continue;
    }
    if (written <= 0) {
      throw std::system_error(errno == 0 ? EIO : errno, std::generic_category(),
                              "replace supervised runtime source");
    }
    offset += static_cast<std::size_t>(written);
  }
  if (::fsync(descriptor) != 0) {
    throw std::system_error(errno, std::generic_category(),
                            "flush supervised runtime source replacement");
  }
}
#endif

/**
 * @brief Creates one deterministic nonzero opaque id.
 * @param seed First sequence byte.
 * @return Complete comparison-only id.
 * @throws Nothing.
 */
IsolatedCpuOpaqueId supervisor_test_id(std::uint8_t seed) noexcept {
  IsolatedCpuOpaqueId id;
  for (std::size_t index = 0U; index < id.bytes.size(); ++index) {
    id.bytes[index] = static_cast<std::byte>(seed + index);
  }
  return id;
}

/**
 * @brief Creates one complete deterministic invocation identity.
 * @param domain Unique per-call invocation-id domain.
 * @return Valid identity tuple with nonzero generations.
 * @throws Nothing.
 */
IsolatedCpuInvocationIdentity supervisor_test_identity(
    std::uint8_t domain) noexcept {
  IsolatedCpuInvocationIdentity identity;
  identity.tenant_id = supervisor_test_id(1U);
  identity.job_id = supervisor_test_id(17U);
  identity.attempt_id = supervisor_test_id(33U);
  identity.worker_id = supervisor_test_id(49U);
  identity.worker_lease_generation = 3U;
  identity.plugin_package_id = supervisor_test_id(65U);
  identity.plugin_generation = 5U;
  identity.invocation_id = supervisor_test_id(domain);
  return identity;
}

#if defined(__linux__)
/**
 * @brief Converts one signed package-id byte sequence to its operation ABI key.
 * @param package Exact sixteen manifest bytes.
 * @return Two big-endian opaque words used only for router comparison.
 * @throws Nothing.
 * @note The conversion assigns no package semantics and transfers no trust,
 * executable, process, or resource authority.
 */
ps_operation_identity_v1 operation_runtime_key(
    const PluginPackageId& package) noexcept {
  ps_operation_identity_v1 result{};
  std::uint64_t* const words[]{&result.word0, &result.word1};
  for (std::size_t word = 0U; word < 2U; ++word) {
    for (std::size_t byte = 0U; byte < 8U; ++byte) {
      *words[word] = (*words[word] << 8U) |
                     static_cast<std::uint64_t>(package[word * 8U + byte]);
    }
  }
  return result;
}
#endif

/**
 * @brief Creates the maintained two-by-three u8 tensor descriptor.
 * @return Unquantized six-element descriptor.
 * @throws std::bad_alloc when shape storage cannot allocate.
 */
DenseTensorDescriptor supervisor_test_descriptor() {
  DenseTensorDescriptor descriptor;
  descriptor.shape = {2U, 3U};
  descriptor.element_semantics = ElementSemantics::UnsignedInteger;
  descriptor.storage_encoding = StorageEncoding{8U};
  return descriptor;
}

/**
 * @brief Creates the exact contiguous layout for the maintained descriptor.
 * @return Positive non-overlapping six-byte layout.
 * @throws std::bad_alloc when stride storage cannot allocate.
 */
StridedLayout supervisor_test_layout() {
  return StridedLayout{{3, 1}, 0U};
}

/**
 * @brief Creates one exact six-byte output plan.
 * @return Positive-stride CPU DenseTensor plan.
 * @throws std::bad_alloc when descriptor/layout storage cannot allocate.
 */
IsolatedCpuDenseTensorOutputPlan supervisor_test_output_plan() {
  IsolatedCpuDenseTensorOutputPlan plan;
  plan.port_identity = supervisor_test_id(97U);
  plan.plan_identity = supervisor_test_id(113U);
  plan.schema_identity = supervisor_test_id(129U);
  plan.layout_identity = supervisor_test_id(145U);
  plan.descriptor = supervisor_test_descriptor();
  plan.layout = supervisor_test_layout();
  plan.storage_size = 6U;
  plan.alignment = 1U;
  return plan;
}

/**
 * @brief Creates one zero-input invocation with one standard output.
 * @param operation Runtime fixture operation.
 * @param domain Unique invocation id domain.
 * @return Complete Host invocation plan.
 * @throws std::bad_alloc when copied state cannot allocate.
 */
IsolatedCpuHostInvocation supervisor_test_invocation(std::string operation,
                                                     std::uint8_t domain) {
  IsolatedCpuHostInvocation invocation;
  invocation.identity = supervisor_test_identity(domain);
  invocation.operation = std::move(operation);
  invocation.operation_identity = supervisor_test_id(161U);
  invocation.implementation_identity = supervisor_test_id(177U);
  invocation.configuration_schema_identity = supervisor_test_id(193U);
  invocation.outputs.push_back(supervisor_test_output_plan());
  return invocation;
}

/**
 * @brief Creates conservative fast bounds for real-process integration tests.
 * @return Positive ordered supervisor timing policy.
 * @throws Nothing.
 * @note Product deadlines remain distinct from the outer CTest timeout.
 */
PluginRuntimeSupervisorOptions supervisor_test_options() noexcept {
  PluginRuntimeSupervisorOptions options;
  options.startup_timeout = std::chrono::milliseconds{1000};
  options.heartbeat_interval = std::chrono::milliseconds{25};
  options.heartbeat_timeout = std::chrono::milliseconds{300};
  options.invocation_timeout = std::chrono::milliseconds{1200};
  options.response_timeout = std::chrono::milliseconds{800};
  options.termination_grace = std::chrono::milliseconds{120};
  options.kill_reap_timeout = std::chrono::milliseconds{800};
  options.restart_backoff = std::chrono::milliseconds{5};
  return options;
}

/**
 * @brief Creates one attempt-local resource authority for supervisor tests.
 * @return Fresh ledger sized for one sequential 1-TiB-ceiling runtime.
 * @throws std::bad_alloc when ledger state cannot allocate.
 * @note Each executor owns a distinct ledger and every invocation identity
 * within that executor is unique, including recovery calls after failures.
 */
std::shared_ptr<ResourceLedger> supervisor_resource_ledger() {
  return std::make_shared<ResourceLedger>(
      ResourceVector{}, std::vector<DeviceResourceLimit>{},
      PluginResourceVector{1U, 1U, 1ULL << 40U, 64ULL * 1024ULL * 1024ULL,
                           4096U});
}

/**
 * @brief Copies one six-byte Ready output into test-owned bytes.
 * @param value Valid contiguous output Value.
 * @return Exact physical bytes.
 * @throws DenseTensorView and validation failures unchanged.
 */
std::array<std::byte, 6U> supervisor_test_bytes(const Value& value) {
  DenseTensorView view(value);
  if (view.storage_size() != 6U) {
    throw std::runtime_error("supervised output size changed");
  }
  std::array<std::byte, 6U> bytes{};
  std::memcpy(bytes.data(), view.data(), bytes.size());
  return bytes;
}

/**
 * @brief Counts currently open parent descriptors without retaining one.
 * @return Open descriptor count excluding the enumeration descriptor.
 * @throws std::system_error when `/dev/fd` cannot be enumerated exactly.
 */
std::size_t supervisor_open_descriptor_count() {
  DIR* directory = ::opendir("/dev/fd");
  if (directory == nullptr) {
    throw std::system_error(errno, std::generic_category(),
                            "open /dev/fd for supervisor leak test");
  }
  const int enumeration_fd = ::dirfd(directory);
  std::size_t count = 0U;
  errno = 0;
  while (const struct dirent* entry = ::readdir(directory)) {
    const char* const begin = entry->d_name;
    const char* const end = begin + std::strlen(begin);
    std::int64_t descriptor = -1;
    const auto parsed = std::from_chars(begin, end, descriptor, 10);
    if (begin != end && parsed.ec == std::errc() && parsed.ptr == end &&
        descriptor >= 0 && descriptor != enumeration_fd) {
      ++count;
    }
  }
  const int read_error = errno;
  if (::closedir(directory) != 0 && read_error == 0) {
    throw std::system_error(errno, std::generic_category(),
                            "close /dev/fd for supervisor leak test");
  }
  if (read_error != 0) {
    throw std::system_error(read_error, std::generic_category(),
                            "read /dev/fd for supervisor leak test");
  }
  return count;
}

/**
 * @brief Invokes one expected-failure call and captures typed public facts.
 * @param executor Supervised-only product executor.
 * @param invocation Complete Host invocation plan.
 * @return Captured `PluginRuntimeFault` facts.
 * @throws std::runtime_error when the invocation unexpectedly succeeds.
 * @throws Non-supervisor failures unchanged.
 */
ObservedRuntimeFault observe_runtime_fault(
    PluginInvocationExecutor* executor,
    const IsolatedCpuHostInvocation& invocation) {
  if (executor == nullptr) {
    throw std::invalid_argument("supervisor test executor is null");
  }
  try {
    static_cast<void>(executor->invoke(invocation));
  } catch (const PluginRuntimeFault& fault) {
    return ObservedRuntimeFault{fault.kind(),
                                fault.wait_status(),
                                fault.exit_code(),
                                fault.signal_number(),
                                fault.termination_stage(),
                                fault.memory_pressure_compatible(),
                                fault.what()};
  }
  throw std::runtime_error("supervised fixture unexpectedly succeeded");
}

/**
 * @brief Captures one failure after deterministically delaying event
 * acceptance.
 * @param executor Supervised-only product executor.
 * @param invocation Complete Host invocation plan.
 * @param event Lifecycle event whose next acceptance is delayed.
 * @param delay Nonnegative one-shot post-receive delay.
 * @return Captured `PluginRuntimeFault` facts after resetting the test seam.
 * @throws Failure-capture, invocation, or probe errors unchanged.
 * @note The reset also runs when an earlier phase fails before consuming the
 * armed event, so one test cannot perturb a later invocation.
 */
ObservedRuntimeFault observe_runtime_fault_after_lifecycle_acceptance_delay(
    PluginInvocationExecutor* executor,
    const IsolatedCpuHostInvocation& invocation,
    SupervisedLifecycleTestEvent event, std::chrono::milliseconds delay) {
  IsolatedCpuInvocationTestProbe::delay_next_lifecycle_event_acceptance(event,
                                                                        delay);
  try {
    ObservedRuntimeFault fault = observe_runtime_fault(executor, invocation);
    IsolatedCpuInvocationTestProbe::delay_next_lifecycle_event_acceptance(
        event, std::chrono::milliseconds{0});
    return fault;
  } catch (...) {
    IsolatedCpuInvocationTestProbe::delay_next_lifecycle_event_acceptance(
        event, std::chrono::milliseconds{0});
    throw;
  }
}

/**
 * @brief Captures one failure after delaying successful request shutdown.
 * @param executor Supervised-only product executor.
 * @param invocation Complete Host invocation plan.
 * @param delay Nonnegative one-shot post-shutdown acceptance delay.
 * @return Captured `PluginRuntimeFault` facts after resetting the test seam.
 * @throws Failure-capture, invocation, or probe errors unchanged.
 * @note The reset also runs when request validation, startup, or send fails
 * before successful `SHUT_WR`, so no unconsumed delay reaches another call.
 */
ObservedRuntimeFault
observe_runtime_fault_after_request_shutdown_acceptance_delay(
    PluginInvocationExecutor* executor,
    const IsolatedCpuHostInvocation& invocation,
    std::chrono::milliseconds delay) {
  IsolatedCpuInvocationTestProbe::
      delay_next_supervised_request_shutdown_acceptance(delay);
  try {
    ObservedRuntimeFault fault = observe_runtime_fault(executor, invocation);
    IsolatedCpuInvocationTestProbe::
        delay_next_supervised_request_shutdown_acceptance(
            std::chrono::milliseconds{0});
    return fault;
  } catch (...) {
    IsolatedCpuInvocationTestProbe::
        delay_next_supervised_request_shutdown_acceptance(
            std::chrono::milliseconds{0});
    throw;
  }
}

/**
 * @brief Captures one failure after pausing Host continuation immediately
 * after complete request-transfer acceptance.
 * @param executor Supervised-only product executor.
 * @param invocation Complete Host invocation plan.
 * @param delay Nonnegative one-shot post-acceptance delay.
 * @return Captured `PluginRuntimeFault` facts after resetting the test seam.
 * @throws Failure-capture, invocation, or probe errors unchanged.
 * @note The reset also runs when request validation, startup, transfer, or
 * acceptance fails before consuming the armed delay, so no perturbation can
 * reach another invocation.
 */
ObservedRuntimeFault
observe_runtime_fault_after_request_transfer_post_acceptance_delay(
    PluginInvocationExecutor* executor,
    const IsolatedCpuHostInvocation& invocation,
    std::chrono::milliseconds delay) {
  IsolatedCpuInvocationTestProbe::
      delay_next_supervised_request_transfer_post_acceptance(delay);
  try {
    ObservedRuntimeFault fault = observe_runtime_fault(executor, invocation);
    IsolatedCpuInvocationTestProbe::
        delay_next_supervised_request_transfer_post_acceptance(
            std::chrono::milliseconds{0});
    return fault;
  } catch (...) {
    IsolatedCpuInvocationTestProbe::
        delay_next_supervised_request_transfer_post_acceptance(
            std::chrono::milliseconds{0});
    throw;
  }
}

/**
 * @brief Captures one post-ownership response-channel observation overflow.
 * @param executor Supervised-only product executor.
 * @param invocation Complete Host invocation plan that reaches response wait.
 * @return Captured phase-typed `PluginRuntimeFault` facts after seam reset.
 * @throws Failure-capture, invocation, or probe errors unchanged.
 * @note Reset runs on every exit. The production seam is also consumed at the
 * invocation entry, before an earlier exceptional path can leave it armed.
 */
ObservedRuntimeFault
observe_runtime_fault_after_response_channel_observation_overflow(
    PluginInvocationExecutor* executor,
    const IsolatedCpuHostInvocation& invocation) {
  IsolatedCpuInvocationTestProbe::
      force_next_response_channel_observation_overflow(true);
  try {
    ObservedRuntimeFault fault = observe_runtime_fault(executor, invocation);
    IsolatedCpuInvocationTestProbe::
        force_next_response_channel_observation_overflow(false);
    return fault;
  } catch (...) {
    IsolatedCpuInvocationTestProbe::
        force_next_response_channel_observation_overflow(false);
    throw;
  }
}

/**
 * @brief Checks exactly one fresh child was spawned and reaped.
 * @param before Observation immediately before a call.
 * @param after Observation immediately after a call.
 * @return Nothing after GoogleTest assertions.
 * @throws Nothing.
 */
void expect_supervised_child_reaped(
    const IsolatedCpuInvocationTestSnapshot& before,
    const IsolatedCpuInvocationTestSnapshot& after) noexcept {
  EXPECT_EQ(after.spawned_children, before.spawned_children + 1U);
  EXPECT_EQ(after.reaped_children, before.reaped_children + 1U);
  EXPECT_GT(after.last_reaped_child, 0);
  if (after.last_reaped_child <= 0 ||
      after.last_reaped_child > std::numeric_limits<pid_t>::max()) {
    return;
  }
  int status = 0;
  errno = 0;
  EXPECT_EQ(
      ::waitpid(static_cast<pid_t>(after.last_reaped_child), &status, WNOHANG),
      -1);
  EXPECT_EQ(errno, ECHILD);
}

/**
 * @brief Minimal allocation-free Host observation target for service tests.
 * @throws Nothing from construction or callbacks.
 */
class SupervisorExecutionHost final : public ExecutionHostContext {
 public:
  /** @copydoc ExecutionHostContext::set_task_context */
  void set_task_context(int worker_id, std::uint64_t epoch,
                        std::optional<ExecutionTaskAuditIdentity>
                            task_identity) noexcept override {
    (void)worker_id;
    (void)epoch;
    (void)task_identity;
  }

  /** @copydoc ExecutionHostContext::clear_task_context */
  void clear_task_context() noexcept override {}

  /** @copydoc ExecutionHostContext::log_event */
  void log_event(ExecutionTraceAction action, int node_id, int worker_id,
                 std::uint64_t epoch,
                 std::optional<ExecutionTaskAuditIdentity>
                     task_identity) noexcept override {
    (void)action;
    (void)node_id;
    (void)worker_id;
    (void)epoch;
    (void)task_identity;
  }
};

/**
 * @brief Creates one deterministic service Run submission.
 * @param graph_identity Stable test label.
 * @param revision Nonzero graph/revision identity.
 * @param target_node_id Diagnostic target node.
 * @return Valid throughput HP submission without a wall-clock deadline.
 * @throws std::bad_alloc when copied identity storage cannot allocate.
 */
compute::ComputeRunSubmission supervisor_run_submission(
    std::string graph_identity, std::uint64_t revision, int target_node_id) {
  return compute::ComputeRunSubmission{
      std::move(graph_identity),
      GraphInstanceId{revision},
      GraphRevision{revision},
      target_node_id,
      ComputeIntent::GlobalHighPrecision,
      compute::ComputeRunQuality::Full,
      compute::ComputeRunQos{compute::ComputeRunQosClass::Throughput,
                             std::nullopt, 3, 2},
      compute::SupersessionIdentity{
          compute::SupersessionKey(target_node_id,
                                   ComputeIntent::GlobalHighPrecision),
          compute::SupersessionGeneration(1)},
      nullptr};
}

/**
 * @brief Builds one ready callback that invokes the supervised executor.
 * @param lease Strong matching Run lease moved into the task.
 * @param local_task_id Run-local task identity.
 * @param executor Borrowed executor that outlives synchronous Run drainage.
 * @param invocation Complete isolated invocation moved into callback storage.
 * @param completed Counter advanced only after a healthy plugin result.
 * @return Move-owned dependency-ready service submission.
 * @throws Construction/allocation failures unchanged.
 */
compute::ReadyTaskSubmission supervised_ready_submission(
    compute::ComputeRunLease lease, std::uint64_t local_task_id,
    PluginInvocationExecutor& executor, IsolatedCpuHostInvocation invocation,
    std::atomic_int* completed) {
  const compute::ComputeRunTaskIdentity identity =
      lease.task_identity(local_task_id);
  return compute::ReadyTaskSubmission(
      std::move(lease), identity, static_cast<int>(local_task_id), true,
      [&executor, invocation = std::move(invocation), completed](
          compute::ComputeRunLease& retained_lease,
          const compute::ComputeRunTaskIdentity& retained_identity,
          ExecutionTaskRuntime& runtime) mutable {
        if (retained_lease.descriptor().id() != retained_identity.run_id()) {
          throw std::logic_error(
              "supervised callback observed mismatched Run identity");
        }
        const IsolatedCpuHostInvocationResult result =
            executor.invoke(invocation);
        if (result.outcome != IsolatedCpuInvocationOutcome::Succeeded) {
          throw std::runtime_error(
              "supervised callback did not produce successful output");
        }
        if (completed != nullptr) {
          completed->fetch_add(1, std::memory_order_relaxed);
        }
        runtime.dec_tasks_to_complete();
      });
}

/**
 * @brief Establishes the contract for checked supervisor defaults.
 * @throws Nothing; GoogleTest records assertion failures.
 */
TEST(PluginRuntimeSupervisor, DefaultDeadlinesArePositiveAndOrdered) {
  const PluginRuntimeSupervisorOptions options;

  EXPECT_GT(options.startup_timeout.count(), 0);
  EXPECT_GT(options.heartbeat_interval.count(), 0);
  EXPECT_GT(options.heartbeat_timeout, options.heartbeat_interval);
  EXPECT_GT(options.invocation_timeout.count(), 0);
  EXPECT_GT(options.response_timeout.count(), 0);
  EXPECT_GT(options.termination_grace.count(), 0);
  EXPECT_GT(options.kill_reap_timeout.count(), 0);
  EXPECT_GT(options.restart_backoff.count(), 0);
}

/**
 * @brief Proves ordinary, exact-fit, and one-tick-overflow supervisor deadline
 * arithmetic without depending on a real clock near its maximum.
 * @throws Nothing; GoogleTest records assertion failures.
 * @note The source-private probe delegates to the production helper used by
 * every supervisor deadline derivation.
 */
TEST(PluginRuntimeSupervisor, CheckedDeadlineRejectsClockRangeOverflow) {
  using Clock = std::chrono::steady_clock;
  const auto one_millisecond =
      std::chrono::duration_cast<Clock::duration>(std::chrono::milliseconds{1});
  const Clock::time_point ordinary_base{Clock::duration{41}};
  EXPECT_EQ(
      IsolatedCpuInvocationTestProbe::checked_supervisor_deadline_for_test(
          ordinary_base, std::chrono::milliseconds{1}),
      ordinary_base + one_millisecond);

  const Clock::time_point latest_base =
      Clock::time_point::max() - one_millisecond;
  EXPECT_EQ(
      IsolatedCpuInvocationTestProbe::checked_supervisor_deadline_for_test(
          latest_base, std::chrono::milliseconds{1}),
      Clock::time_point::max());
  EXPECT_THROW(
      IsolatedCpuInvocationTestProbe::checked_supervisor_deadline_for_test(
          latest_base + Clock::duration{1}, std::chrono::milliseconds{1}),
      std::overflow_error);
}

/**
 * @brief Rejects zero restart backoff before any child ownership can begin.
 * @throws Nothing; GoogleTest records assertion failures.
 */
TEST(PluginRuntimeSupervisor, RejectsZeroRestartBackoffAtConstruction) {
  PluginRuntimeSupervisorOptions options = supervisor_test_options();
  options.restart_backoff = std::chrono::milliseconds{0};

  EXPECT_THROW(PluginRuntimeSupervisor(
                   std::filesystem::path(PS_TEST_ISOLATED_CPU_FIXTURE_PATH),
                   supervisor_resource_ledger(), {}, options),
               std::invalid_argument);
}

/**
 * @brief Verifies a supervised child executes through the linked production
 * archive, publishes validated bytes, and is synchronously reaped.
 * @throws Standard fixture, transport, and assertion failures observed by
 * GoogleTest.
 */
TEST(PluginRuntimeSupervisor, ExecutesThroughProductArchiveAndPublishesOutput) {
  PluginInvocationExecutor executor(
      std::filesystem::path(PS_TEST_ISOLATED_CPU_FIXTURE_PATH),
      supervisor_resource_ledger(), {}, supervisor_test_options());
  const IsolatedCpuInvocationTestSnapshot before =
      IsolatedCpuInvocationTestProbe::snapshot();

  const IsolatedCpuHostInvocationResult result =
      executor.invoke(supervisor_test_invocation("fixture.fill_sequence", 97U));

  ASSERT_EQ(result.outcome, IsolatedCpuInvocationOutcome::Succeeded);
  ASSERT_EQ(result.outputs.size(), 1U);
  const std::array<std::byte, 6U> expected{std::byte{0}, std::byte{1},
                                           std::byte{2}, std::byte{3},
                                           std::byte{4}, std::byte{5}};
  EXPECT_EQ(supervisor_test_bytes(result.outputs[0]), expected);
  expect_supervised_child_reaped(before,
                                 IsolatedCpuInvocationTestProbe::snapshot());
}

/**
 * @brief Verifies the operation runtime router selects the exact signed
 * supervised executor and publishes validated bytes without direct fallback.
 * @throws Standard trust, route, transport, publication, and assertion failures
 * observed by GoogleTest.
 * @note Positive exact-object execution is Linux-only. Darwin's focused ABI
 * test covers route-before-process failure, while supervisor construction
 * remains deliberately unsupported there.
 */
TEST(PluginRuntimeSupervisor, OperationRuntimeRouterInvokesExactSignedPackage) {
#if defined(__linux__)
  auto executor = std::make_shared<PluginInvocationExecutor>(
      std::filesystem::path(PS_TEST_ISOLATED_CPU_FIXTURE_PATH),
      supervisor_resource_ledger(), PluginInvocationResourcePolicy{},
      supervisor_test_options());
  const ps_operation_identity_v1 runtime_key =
      operation_runtime_key(executor->package_identity().package_id);
  const IsolatedCpuInvocationTestSnapshot before =
      IsolatedCpuInvocationTestProbe::snapshot();
  plugin_host::install_supervised_operation_runtime_route(
      runtime_key, executor, []() { return supervisor_test_identity(98U); });
  IsolatedCpuHostInvocationResult result;
  try {
    result = plugin_host::invoke_supervised_operation_runtime(
        runtime_key, supervisor_test_invocation("fixture.fill_sequence", 199U));
  } catch (...) {
    static_cast<void>(
        plugin_host::remove_supervised_operation_runtime_route(runtime_key));
    throw;
  }
  EXPECT_TRUE(
      plugin_host::remove_supervised_operation_runtime_route(runtime_key));

  ASSERT_EQ(result.outcome, IsolatedCpuInvocationOutcome::Succeeded);
  ASSERT_EQ(result.outputs.size(), 1U);
  const std::array<std::byte, 6U> expected{std::byte{0}, std::byte{1},
                                           std::byte{2}, std::byte{3},
                                           std::byte{4}, std::byte{5}};
  EXPECT_EQ(supervisor_test_bytes(result.outputs[0]), expected);
  expect_supervised_child_reaped(before,
                                 IsolatedCpuInvocationTestProbe::snapshot());
#else
  GTEST_SKIP() << "exact signed runtime routing is Linux-only";
#endif
}

/**
 * @brief Proves a Linux supervisor runs its sealed private runtime after an
 * already-open writer destroys the original source bytes.
 * @throws Filesystem, trust, process, and assertion failures unchanged.
 */
TEST(PluginRuntimeSupervisor, LinuxRuntimeSnapshotSurvivesSourceMutation) {
#if defined(__linux__)
  const std::filesystem::path root =
      std::filesystem::temp_directory_path() /
      "photospider-supervised-runtime-source-mutation-test";
  std::error_code ignored;
  std::filesystem::remove_all(root, ignored);
  std::filesystem::create_directories(root);
  const std::filesystem::path candidate = root / "runtime";
  std::filesystem::copy_file(PS_TEST_ISOLATED_CPU_FIXTURE_PATH, candidate);
  ScopedSupervisorTestFd writer(::open(candidate.c_str(), O_RDWR));
  ASSERT_GE(writer.get(), 0);
  auto ledger = supervisor_resource_ledger();
  PluginInvocationExecutor executor(candidate, ledger, {},
                                    supervisor_test_options());

  overwrite_supervised_runtime_source(writer.get());
  const IsolatedCpuHostInvocationResult result =
      executor.invoke(supervisor_test_invocation("fixture.fill_sequence", 98U));

  ASSERT_EQ(result.outcome, IsolatedCpuInvocationOutcome::Succeeded);
  ASSERT_EQ(result.outputs.size(), 1U);
  const std::array<std::byte, 6U> expected{std::byte{0}, std::byte{1},
                                           std::byte{2}, std::byte{3},
                                           std::byte{4}, std::byte{5}};
  EXPECT_EQ(supervisor_test_bytes(result.outputs[0]), expected);
  EXPECT_EQ(ledger->plugin_snapshot().reserved, PluginResourceVector{});
  std::filesystem::remove_all(root);
#else
  GTEST_SKIP() << "sealed runtime descriptor execution is Linux-only";
#endif
}

/**
 * @brief Proves Darwin rejects supervisor construction before any token,
 * capability-materialization, or child-process side effect.
 * @throws Standard construction and assertion failures unchanged.
 */
TEST(PluginRuntimeSupervisor, DarwinRuntimeConstructionFailsBeforeSideEffects) {
#if defined(__APPLE__)
  auto ledger = supervisor_resource_ledger();
  const ResourceLedger::PluginSnapshot ledger_before =
      ledger->plugin_snapshot();
  const IsolatedCpuInvocationTestSnapshot probe_before =
      IsolatedCpuInvocationTestProbe::snapshot();
  try {
    PluginInvocationExecutor executor(
        std::filesystem::path(PS_TEST_ISOLATED_CPU_FIXTURE_PATH), ledger, {},
        supervisor_test_options());
    static_cast<void>(executor);
    FAIL() << "Darwin supervised runtime construction must fail closed";
  } catch (const PluginTrustError& error) {
    EXPECT_EQ(error.code(), PluginTrustErrorCode::ExactObjectUnsupported);
  }
  const ResourceLedger::PluginSnapshot ledger_after = ledger->plugin_snapshot();
  const IsolatedCpuInvocationTestSnapshot probe_after =
      IsolatedCpuInvocationTestProbe::snapshot();
  EXPECT_EQ(ledger_after.reserved, ledger_before.reserved);
  EXPECT_EQ(ledger_after.high_water, ledger_before.high_water);
  EXPECT_EQ(probe_after.host_capability_materialization_attempts,
            probe_before.host_capability_materialization_attempts);
  EXPECT_EQ(probe_after.spawned_children, probe_before.spawned_children);
  EXPECT_EQ(probe_after.reaped_children, probe_before.reaped_children);
#else
  GTEST_SKIP() << "Darwin fail-closed construction is platform-specific";
#endif
}

/**
 * @brief Drains a queued authenticated completion before classifying the
 * already exited child, then proves exact cleanup and fresh recovery.
 * @throws Standard fixture, transport, and assertion failures observed by
 * GoogleTest.
 * @note The source-private hold waits for exact child exit without reaping it.
 * Because the endpoint joins its heartbeat thread before sending completion
 * and exits only after the complete response, release proves all child work is
 * finished while both real socket payloads remain available to the Host.
 */
TEST(PluginRuntimeSupervisor,
     DrainsQueuedCompletionBeforeClassifyingNormalExitAndRecovers) {
  PluginRuntimeSupervisorOptions options = supervisor_test_options();
  options.restart_backoff = std::chrono::milliseconds{1};
  PluginInvocationExecutor executor(
      std::filesystem::path(PS_TEST_ISOLATED_CPU_FIXTURE_PATH),
      supervisor_resource_ledger(), {}, options);
  const std::size_t descriptor_baseline = supervisor_open_descriptor_count();
  const IsolatedCpuInvocationTestSnapshot before =
      IsolatedCpuInvocationTestProbe::snapshot();
  ASSERT_FALSE(before.invocation_monitor_exit_hold_armed);

  IsolatedCpuInvocationTestProbe::hold_next_invocation_monitor_until_child_exit(
      true);
  IsolatedCpuHostInvocationResult first;
  try {
    first = executor.invoke(
        supervisor_test_invocation("fixture.fill_sequence", 101U));
  } catch (...) {
    IsolatedCpuInvocationTestProbe::
        hold_next_invocation_monitor_until_child_exit(false);
    throw;
  }
  IsolatedCpuInvocationTestProbe::hold_next_invocation_monitor_until_child_exit(
      false);

  ASSERT_EQ(first.outcome, IsolatedCpuInvocationOutcome::Succeeded);
  ASSERT_EQ(first.outputs.size(), 1U);
  const std::array<std::byte, 6U> expected{std::byte{0}, std::byte{1},
                                           std::byte{2}, std::byte{3},
                                           std::byte{4}, std::byte{5}};
  EXPECT_EQ(supervisor_test_bytes(first.outputs[0]), expected);
  const IsolatedCpuInvocationTestSnapshot after_first =
      IsolatedCpuInvocationTestProbe::snapshot();
  expect_supervised_child_reaped(before, after_first);
  EXPECT_EQ(after_first.exact_frames_received,
            before.exact_frames_received + 1U);
  EXPECT_FALSE(after_first.invocation_monitor_exit_hold_armed);
  const std::int64_t first_reaped_pid = after_first.last_reaped_child;

  const IsolatedCpuHostInvocationResult recovered = executor.invoke(
      supervisor_test_invocation("fixture.fill_sequence", 102U));
  ASSERT_EQ(recovered.outcome, IsolatedCpuInvocationOutcome::Succeeded);
  ASSERT_EQ(recovered.outputs.size(), 1U);
  EXPECT_EQ(supervisor_test_bytes(recovered.outputs[0]), expected);
  const IsolatedCpuInvocationTestSnapshot after_recovery =
      IsolatedCpuInvocationTestProbe::snapshot();
  expect_supervised_child_reaped(after_first, after_recovery);
  EXPECT_EQ(after_recovery.exact_frames_received,
            after_first.exact_frames_received + 1U);
  EXPECT_NE(after_recovery.last_reaped_child, first_reaped_pid);
  EXPECT_FALSE(after_recovery.invocation_monitor_exit_hold_armed);
  EXPECT_EQ(supervisor_open_descriptor_count(), descriptor_baseline);
}

/**
 * @brief Proves bounded request transfer does not consume the callback's full
 * invocation-completion window.
 * @throws Standard fixture, transport, and assertion failures observed by
 * GoogleTest.
 */
TEST(PluginRuntimeSupervisor, RequestTransferDoesNotConsumeCallbackBudget) {
  PluginRuntimeSupervisorOptions options = supervisor_test_options();
  options.invocation_timeout = std::chrono::milliseconds{1000};
  PluginInvocationExecutor executor(
      std::filesystem::path(PS_TEST_ISOLATED_CPU_FIXTURE_PATH),
      supervisor_resource_ledger(), {}, options);
  const IsolatedCpuInvocationTestSnapshot before =
      IsolatedCpuInvocationTestProbe::snapshot();

  IsolatedCpuInvocationTestProbe::delay_next_supervised_request_send(
      std::chrono::milliseconds{600});
  IsolatedCpuHostInvocationResult result;
  try {
    result = executor.invoke(
        supervisor_test_invocation("fixture.delayed_fill_sequence", 105U));
  } catch (...) {
    IsolatedCpuInvocationTestProbe::delay_next_supervised_request_send(
        std::chrono::milliseconds{0});
    throw;
  }

  ASSERT_EQ(result.outcome, IsolatedCpuInvocationOutcome::Succeeded);
  ASSERT_EQ(result.outputs.size(), 1U);
  const std::array<std::byte, 6U> expected{std::byte{0}, std::byte{1},
                                           std::byte{2}, std::byte{3},
                                           std::byte{4}, std::byte{5}};
  EXPECT_EQ(supervisor_test_bytes(result.outputs[0]), expected);
  expect_supervised_child_reaped(before,
                                 IsolatedCpuInvocationTestProbe::snapshot());
}

/**
 * @brief Rejects a complete request whose successful write-half shutdown is
 * accepted only after the absolute request-transfer deadline, then proves
 * exact retirement and fresh-process recovery without output publication.
 * @throws Standard fixture, transport, and assertion failures observed by
 * GoogleTest.
 */
TEST(PluginRuntimeSupervisor,
     RejectsRequestShutdownAcceptedAfterTransferDeadlineAndRecovers) {
  PluginRuntimeSupervisorOptions options = supervisor_test_options();
  options.heartbeat_interval = std::chrono::milliseconds{20};
  options.heartbeat_timeout = std::chrono::milliseconds{600};
  options.invocation_timeout = std::chrono::milliseconds{120};
  options.restart_backoff = std::chrono::milliseconds{1};
  PluginInvocationExecutor executor(
      std::filesystem::path(PS_TEST_ISOLATED_CPU_FIXTURE_PATH),
      supervisor_resource_ledger(), {}, options);
  const std::size_t descriptor_baseline = supervisor_open_descriptor_count();
  const IsolatedCpuInvocationTestSnapshot before =
      IsolatedCpuInvocationTestProbe::snapshot();
  ASSERT_FALSE(before.request_shutdown_acceptance_delay_armed);

  const ObservedRuntimeFault fault =
      observe_runtime_fault_after_request_shutdown_acceptance_delay(
          &executor, supervisor_test_invocation("fixture.fill_sequence", 109U),
          std::chrono::milliseconds{240});
  const IsolatedCpuInvocationTestSnapshot after_fault =
      IsolatedCpuInvocationTestProbe::snapshot();

  EXPECT_EQ(fault.kind, PluginRuntimeFaultKind::InvocationDeadline);
  EXPECT_NE(fault.diagnostic.find("request transfer"), std::string::npos);
  expect_supervised_child_reaped(before, after_fault);
  EXPECT_EQ(after_fault.exact_frames_received, before.exact_frames_received);
  EXPECT_FALSE(after_fault.request_shutdown_acceptance_delay_armed);
  const std::int64_t failed_reaped_pid = after_fault.last_reaped_child;

  const IsolatedCpuHostInvocationResult recovered = executor.invoke(
      supervisor_test_invocation("fixture.fill_sequence", 110U));
  ASSERT_EQ(recovered.outcome, IsolatedCpuInvocationOutcome::Succeeded);
  ASSERT_EQ(recovered.outputs.size(), 1U);
  const std::array<std::byte, 6U> expected{std::byte{0}, std::byte{1},
                                           std::byte{2}, std::byte{3},
                                           std::byte{4}, std::byte{5}};
  EXPECT_EQ(supervisor_test_bytes(recovered.outputs[0]), expected);
  const IsolatedCpuInvocationTestSnapshot after_recovery =
      IsolatedCpuInvocationTestProbe::snapshot();
  expect_supervised_child_reaped(after_fault, after_recovery);
  EXPECT_EQ(after_recovery.exact_frames_received,
            after_fault.exact_frames_received + 1U);
  EXPECT_NE(after_recovery.last_reaped_child, failed_reaped_pid);
  EXPECT_FALSE(after_recovery.request_shutdown_acceptance_delay_armed);
  EXPECT_EQ(supervisor_open_descriptor_count(), descriptor_baseline);
}

/**
 * @brief Proves a scheduler pause after exact request-transfer acceptance
 * cannot grant fresh callback or heartbeat budgets, then verifies exact
 * retirement and fresh-process recovery without output publication.
 * @throws Standard fixture, transport, and assertion failures observed by
 * GoogleTest.
 * @note The deterministic pause exceeds both accepted-at-derived deadlines
 * while the real child remains in callback work. Invocation deadline must
 * retain priority over heartbeat timeout when the Host resumes.
 */
TEST(PluginRuntimeSupervisor,
     PostAcceptancePauseCannotGrantFreshCallbackBudgetsAndRecovers) {
  PluginRuntimeSupervisorOptions options = supervisor_test_options();
  options.heartbeat_interval = std::chrono::milliseconds{20};
  options.heartbeat_timeout = std::chrono::milliseconds{250};
  options.invocation_timeout = std::chrono::milliseconds{350};
  options.restart_backoff = std::chrono::milliseconds{1};
  PluginInvocationExecutor executor(
      std::filesystem::path(PS_TEST_ISOLATED_CPU_FIXTURE_PATH),
      supervisor_resource_ledger(), {}, options);
  const std::size_t descriptor_baseline = supervisor_open_descriptor_count();
  const IsolatedCpuInvocationTestSnapshot before =
      IsolatedCpuInvocationTestProbe::snapshot();
  ASSERT_FALSE(before.request_transfer_post_acceptance_delay_armed);

  const ObservedRuntimeFault fault =
      observe_runtime_fault_after_request_transfer_post_acceptance_delay(
          &executor,
          supervisor_test_invocation("fixture.delayed_fill_sequence", 111U),
          std::chrono::milliseconds{400});
  const IsolatedCpuInvocationTestSnapshot after_fault =
      IsolatedCpuInvocationTestProbe::snapshot();

  EXPECT_EQ(fault.kind, PluginRuntimeFaultKind::InvocationDeadline);
  EXPECT_NE(fault.diagnostic.find("invocation timed out"), std::string::npos);
  expect_supervised_child_reaped(before, after_fault);
  EXPECT_EQ(after_fault.exact_frames_received, before.exact_frames_received);
  EXPECT_FALSE(after_fault.request_transfer_post_acceptance_delay_armed);
  const std::int64_t failed_reaped_pid = after_fault.last_reaped_child;

  const IsolatedCpuHostInvocationResult recovered = executor.invoke(
      supervisor_test_invocation("fixture.fill_sequence", 112U));
  ASSERT_EQ(recovered.outcome, IsolatedCpuInvocationOutcome::Succeeded);
  ASSERT_EQ(recovered.outputs.size(), 1U);
  const std::array<std::byte, 6U> expected{std::byte{0}, std::byte{1},
                                           std::byte{2}, std::byte{3},
                                           std::byte{4}, std::byte{5}};
  EXPECT_EQ(supervisor_test_bytes(recovered.outputs[0]), expected);
  const IsolatedCpuInvocationTestSnapshot after_recovery =
      IsolatedCpuInvocationTestProbe::snapshot();
  expect_supervised_child_reaped(after_fault, after_recovery);
  EXPECT_EQ(after_recovery.exact_frames_received,
            after_fault.exact_frames_received + 1U);
  EXPECT_NE(after_recovery.last_reaped_child, failed_reaped_pid);
  EXPECT_FALSE(after_recovery.request_transfer_post_acceptance_delay_armed);
  EXPECT_EQ(supervisor_open_descriptor_count(), descriptor_baseline);
}

/**
 * @brief Distinguishes a missing startup event from an authenticated-session
 * mismatch during the startup deadline.
 * @throws Standard fixture, transport, and assertion failures observed by
 * GoogleTest.
 */
TEST(PluginRuntimeSupervisor, RejectsSilentAndWrongNonceStartup) {
  PluginRuntimeSupervisorOptions options = supervisor_test_options();
  options.startup_timeout = std::chrono::milliseconds{250};
  PluginInvocationExecutor silent(
      std::filesystem::path(PS_TEST_PLUGIN_RUNTIME_SILENT_FIXTURE_PATH),
      supervisor_resource_ledger(), {}, options);
  IsolatedCpuHostInvocation silent_invocation =
      supervisor_test_invocation("fixture.fill_sequence", 113U);
  silent_invocation.identity.plugin_generation = 6U;
  const ObservedRuntimeFault silent_fault =
      observe_runtime_fault(&silent, silent_invocation);
  EXPECT_EQ(silent_fault.kind, PluginRuntimeFaultKind::StartupDeadline);
  EXPECT_NE(silent_fault.termination_stage,
            PluginRuntimeTerminationStage::None);

  PluginInvocationExecutor corrupt(
      std::filesystem::path(PS_TEST_PLUGIN_RUNTIME_CORRUPT_FIXTURE_PATH),
      supervisor_resource_ledger(), {}, options);
  IsolatedCpuHostInvocation corrupt_invocation =
      supervisor_test_invocation("fixture.fill_sequence", 129U);
  corrupt_invocation.identity.plugin_generation = 7U;
  const ObservedRuntimeFault corrupt_fault =
      observe_runtime_fault(&corrupt, corrupt_invocation);
  EXPECT_EQ(corrupt_fault.kind, PluginRuntimeFaultKind::LifecycleProtocol);
  EXPECT_FALSE(corrupt_fault.diagnostic.empty());
}

/**
 * @brief Rejects a valid queued startup event when acceptance occurs after the
 * absolute startup deadline.
 * @throws Standard fixture, transport, and assertion failures observed by
 * GoogleTest.
 */
TEST(PluginRuntimeSupervisor, RejectsQueuedStartedAfterStartupDeadline) {
  PluginRuntimeSupervisorOptions options = supervisor_test_options();
  options.startup_timeout = std::chrono::milliseconds{200};
  PluginInvocationExecutor executor(
      std::filesystem::path(PS_TEST_ISOLATED_CPU_FIXTURE_PATH),
      supervisor_resource_ledger(), {}, options);
  const IsolatedCpuInvocationTestSnapshot before =
      IsolatedCpuInvocationTestProbe::snapshot();

  const ObservedRuntimeFault fault =
      observe_runtime_fault_after_lifecycle_acceptance_delay(
          &executor, supervisor_test_invocation("fixture.fill_sequence", 137U),
          SupervisedLifecycleTestEvent::RuntimeStarted,
          std::chrono::milliseconds{400});

  EXPECT_EQ(fault.kind, PluginRuntimeFaultKind::StartupDeadline);
  EXPECT_NE(fault.termination_stage, PluginRuntimeTerminationStage::None);
  expect_supervised_child_reaped(before,
                                 IsolatedCpuInvocationTestProbe::snapshot());
}

/**
 * @brief Rejects a queued heartbeat whose acceptance crosses the active gap
 * deadline without waiting for the longer invocation bound.
 * @throws Standard fixture, transport, and assertion failures observed by
 * GoogleTest.
 */
TEST(PluginRuntimeSupervisor, RejectsQueuedHeartbeatAfterGapDeadline) {
  PluginRuntimeSupervisorOptions options = supervisor_test_options();
  options.heartbeat_interval = std::chrono::milliseconds{20};
  options.heartbeat_timeout = std::chrono::milliseconds{120};
  options.invocation_timeout = std::chrono::milliseconds{800};
  PluginInvocationExecutor executor(
      std::filesystem::path(PS_TEST_ISOLATED_CPU_FIXTURE_PATH),
      supervisor_resource_ledger(), {}, options);
  const IsolatedCpuInvocationTestSnapshot before =
      IsolatedCpuInvocationTestProbe::snapshot();

  const ObservedRuntimeFault fault =
      observe_runtime_fault_after_lifecycle_acceptance_delay(
          &executor, supervisor_test_invocation("fixture.hang", 138U),
          SupervisedLifecycleTestEvent::Heartbeat,
          std::chrono::milliseconds{240});

  EXPECT_EQ(fault.kind, PluginRuntimeFaultKind::HeartbeatTimeout);
  EXPECT_NE(fault.termination_stage, PluginRuntimeTerminationStage::None);
  expect_supervised_child_reaped(before,
                                 IsolatedCpuInvocationTestProbe::snapshot());
}

/**
 * @brief Rejects a queued completion after the absolute invocation deadline
 * even though the heartbeat-gap deadline remains in the future.
 * @throws Standard fixture, transport, and assertion failures observed by
 * GoogleTest.
 */
TEST(PluginRuntimeSupervisor, RejectsQueuedCompletionAfterInvocationDeadline) {
  PluginRuntimeSupervisorOptions options = supervisor_test_options();
  options.heartbeat_interval = std::chrono::milliseconds{20};
  options.heartbeat_timeout = std::chrono::milliseconds{600};
  options.invocation_timeout = std::chrono::milliseconds{120};
  PluginInvocationExecutor executor(
      std::filesystem::path(PS_TEST_ISOLATED_CPU_FIXTURE_PATH),
      supervisor_resource_ledger(), {}, options);
  const IsolatedCpuInvocationTestSnapshot before =
      IsolatedCpuInvocationTestProbe::snapshot();

  const ObservedRuntimeFault fault =
      observe_runtime_fault_after_lifecycle_acceptance_delay(
          &executor, supervisor_test_invocation("fixture.fill_sequence", 139U),
          SupervisedLifecycleTestEvent::InvocationCompleted,
          std::chrono::milliseconds{240});

  EXPECT_EQ(fault.kind, PluginRuntimeFaultKind::InvocationDeadline);
  expect_supervised_child_reaped(before,
                                 IsolatedCpuInvocationTestProbe::snapshot());
}

/**
 * @brief Preserves the absolute invocation deadline as the primary cause when
 * both invocation and heartbeat bounds expire before queued completion is
 * accepted.
 * @throws Standard fixture, transport, and assertion failures observed by
 * GoogleTest.
 */
TEST(PluginRuntimeSupervisor,
     InvocationDeadlineOutranksHeartbeatAtQueuedCompletion) {
  PluginRuntimeSupervisorOptions options = supervisor_test_options();
  options.heartbeat_interval = std::chrono::milliseconds{20};
  options.heartbeat_timeout = std::chrono::milliseconds{100};
  options.invocation_timeout = std::chrono::milliseconds{140};
  PluginInvocationExecutor executor(
      std::filesystem::path(PS_TEST_ISOLATED_CPU_FIXTURE_PATH),
      supervisor_resource_ledger(), {}, options);
  const IsolatedCpuInvocationTestSnapshot before =
      IsolatedCpuInvocationTestProbe::snapshot();

  const ObservedRuntimeFault fault =
      observe_runtime_fault_after_lifecycle_acceptance_delay(
          &executor, supervisor_test_invocation("fixture.fill_sequence", 140U),
          SupervisedLifecycleTestEvent::InvocationCompleted,
          std::chrono::milliseconds{240});

  EXPECT_EQ(fault.kind, PluginRuntimeFaultKind::InvocationDeadline);
  expect_supervised_child_reaped(before,
                                 IsolatedCpuInvocationTestProbe::snapshot());
}

/**
 * @brief Confirms natural exit and signal termination remain separately
 * observable without inventing an out-of-memory cause.
 * @throws Standard fixture, transport, and assertion failures observed by
 * GoogleTest.
 */
TEST(PluginRuntimeSupervisor, ReportsNaturalExitAndSignalFacts) {
  PluginInvocationExecutor executor(
      std::filesystem::path(PS_TEST_ISOLATED_CPU_FIXTURE_PATH),
      supervisor_resource_ledger(), {}, supervisor_test_options());

  const ObservedRuntimeFault exit_fault = observe_runtime_fault(
      &executor, supervisor_test_invocation("fixture.crash", 145U));
  EXPECT_EQ(exit_fault.kind, PluginRuntimeFaultKind::ProcessExit);
  EXPECT_EQ(exit_fault.exit_code, 73);
  EXPECT_EQ(exit_fault.termination_stage, PluginRuntimeTerminationStage::None);

  const ObservedRuntimeFault signal_fault = observe_runtime_fault(
      &executor, supervisor_test_invocation("fixture.sigkill", 161U));
  EXPECT_EQ(signal_fault.kind, PluginRuntimeFaultKind::ProcessSignal);
  EXPECT_EQ(signal_fault.signal_number, SIGKILL);
  EXPECT_TRUE(signal_fault.memory_pressure_compatible);
  EXPECT_EQ(signal_fault.termination_stage,
            PluginRuntimeTerminationStage::None);
}

/**
 * @brief Confirms a live callback heartbeat cannot mask the invocation
 * deadline while a stopped child is classified by heartbeat loss.
 * @throws Standard fixture, transport, and assertion failures observed by
 * GoogleTest.
 */
TEST(PluginRuntimeSupervisor, SeparatesInvocationAndHeartbeatTimeouts) {
  PluginRuntimeSupervisorOptions options = supervisor_test_options();
  options.invocation_timeout = std::chrono::milliseconds{450};
  options.heartbeat_timeout = std::chrono::milliseconds{180};
  PluginInvocationExecutor executor(
      std::filesystem::path(PS_TEST_ISOLATED_CPU_FIXTURE_PATH),
      supervisor_resource_ledger(), {}, options);

  const ObservedRuntimeFault callback_hang = observe_runtime_fault(
      &executor, supervisor_test_invocation("fixture.hang", 177U));
  EXPECT_EQ(callback_hang.kind, PluginRuntimeFaultKind::InvocationDeadline);
  EXPECT_EQ(callback_hang.termination_stage,
            PluginRuntimeTerminationStage::Sigterm);

  const ObservedRuntimeFault stopped = observe_runtime_fault(
      &executor, supervisor_test_invocation("fixture.stop", 193U));
  EXPECT_EQ(stopped.kind, PluginRuntimeFaultKind::HeartbeatTimeout);
  EXPECT_NE(stopped.termination_stage, PluginRuntimeTerminationStage::None);
}

/**
 * @brief Proves an ignored graceful termination escalates to SIGKILL without
 * replacing the primary invocation-deadline diagnosis.
 * @throws Standard fixture, transport, and assertion failures observed by
 * GoogleTest.
 */
TEST(PluginRuntimeSupervisor, EscalatesIgnoredTerminationWithoutChangingCause) {
  PluginRuntimeSupervisorOptions options = supervisor_test_options();
  options.invocation_timeout = std::chrono::milliseconds{350};
  PluginInvocationExecutor executor(
      std::filesystem::path(PS_TEST_ISOLATED_CPU_FIXTURE_PATH),
      supervisor_resource_ledger(), {}, options);
  const IsolatedCpuInvocationTestSnapshot before =
      IsolatedCpuInvocationTestProbe::snapshot();

  const ObservedRuntimeFault fault = observe_runtime_fault(
      &executor,
      supervisor_test_invocation("fixture.ignore_termination_hang", 209U));

  EXPECT_EQ(fault.kind, PluginRuntimeFaultKind::InvocationDeadline);
  EXPECT_EQ(fault.termination_stage, PluginRuntimeTerminationStage::Sigkill);
  EXPECT_EQ(fault.signal_number, SIGKILL);
  EXPECT_TRUE(fault.memory_pressure_compatible);
  expect_supervised_child_reaped(before,
                                 IsolatedCpuInvocationTestProbe::snapshot());
}

/**
 * @brief Proves authenticated lifecycle completion cannot leave response
 * transfer or child teardown unbounded.
 * @throws Standard fixture, transport, and assertion failures observed by
 * GoogleTest.
 */
TEST(PluginRuntimeSupervisor, BoundsResponseAfterAuthenticatedCompletion) {
  PluginRuntimeSupervisorOptions options = supervisor_test_options();
  options.response_timeout = std::chrono::milliseconds{250};
  PluginInvocationExecutor executor(
      std::filesystem::path(PS_TEST_ISOLATED_CPU_FIXTURE_PATH),
      supervisor_resource_ledger(), {}, options);

  const ObservedRuntimeFault fault = observe_runtime_fault(
      &executor, supervisor_test_invocation("fixture.response_hang", 225U));

  EXPECT_EQ(fault.kind, PluginRuntimeFaultKind::ResponseDeadline);
  EXPECT_NE(fault.termination_stage, PluginRuntimeTerminationStage::None);
}

/**
 * @brief Classifies normal-exit truncated framing and late descriptor rights
 * as bad output instead of a successful result.
 * @throws Standard fixture, transport, and assertion failures observed by
 * GoogleTest.
 */
TEST(PluginRuntimeSupervisor, ClassifiesNormalMalformedOutputAsBadOutput) {
  PluginInvocationExecutor executor(
      std::filesystem::path(PS_TEST_ISOLATED_CPU_FIXTURE_PATH),
      supervisor_resource_ledger(), {}, supervisor_test_options());
  const std::array<const char*, 2U> operations{"fixture.truncated",
                                               "fixture.late_rights"};
  for (std::size_t index = 0U; index < operations.size(); ++index) {
    const ObservedRuntimeFault fault = observe_runtime_fault(
        &executor,
        supervisor_test_invocation(operations[index],
                                   static_cast<std::uint8_t>(233U + index)));
    EXPECT_EQ(fault.kind, PluginRuntimeFaultKind::BadOutput);
    EXPECT_EQ(fault.exit_code, 0);
  }
}

/**
 * @brief Classifies definitive premature response EOF as bad output even while
 * the child remains alive, then proves exact retirement and fresh recovery.
 * @throws Standard fixture, transport, and assertion failures observed by
 * GoogleTest.
 */
TEST(PluginRuntimeSupervisor,
     ClassifiesLiveChildPrematureResponseEofAsBadOutputAndRecovers) {
  PluginRuntimeSupervisorOptions options = supervisor_test_options();
  options.restart_backoff = std::chrono::milliseconds{1};
  PluginInvocationExecutor executor(
      std::filesystem::path(PS_TEST_ISOLATED_CPU_FIXTURE_PATH),
      supervisor_resource_ledger(), {}, options);
  const std::size_t descriptor_baseline = supervisor_open_descriptor_count();
  const IsolatedCpuInvocationTestSnapshot before =
      IsolatedCpuInvocationTestProbe::snapshot();

  const ObservedRuntimeFault fault = observe_runtime_fault(
      &executor, supervisor_test_invocation("fixture.truncated_hang", 237U));
  const IsolatedCpuInvocationTestSnapshot after_fault =
      IsolatedCpuInvocationTestProbe::snapshot();

  EXPECT_EQ(fault.kind, PluginRuntimeFaultKind::BadOutput);
  EXPECT_EQ(fault.termination_stage, PluginRuntimeTerminationStage::Sigterm);
  EXPECT_EQ(fault.signal_number, SIGTERM);
  expect_supervised_child_reaped(before, after_fault);

  const IsolatedCpuHostInvocationResult recovered = executor.invoke(
      supervisor_test_invocation("fixture.fill_sequence", 238U));
  ASSERT_EQ(recovered.outcome, IsolatedCpuInvocationOutcome::Succeeded);
  ASSERT_EQ(recovered.outputs.size(), 1U);
  const IsolatedCpuInvocationTestSnapshot after_recovery =
      IsolatedCpuInvocationTestProbe::snapshot();
  expect_supervised_child_reaped(after_fault, after_recovery);
  EXPECT_EQ(supervisor_open_descriptor_count(), descriptor_baseline);
}

/**
 * @brief Maps an owned response-channel observation overflow by current phase,
 * then proves exact retirement, cleared probe state, and fresh recovery.
 * @throws Standard fixture, transport, and assertion failures observed by
 * GoogleTest.
 * @note The response-hang fixture authenticates completion and remains alive.
 * The source-private seam then replaces the Host control socket with an owned
 * regular descriptor so the real receiver reports `ENOTSOCK` before its short
 * status-observation derivation is forced past the monotonic clock range. No
 * response frame can be published. An initial invalid Host plan also proves
 * invocation-entry consumption clears the seam before preflight can fail.
 */
TEST(PluginRuntimeSupervisor,
     MapsOwnedResponseChannelObservationOverflowAndRecovers) {
  PluginRuntimeSupervisorOptions options = supervisor_test_options();
  options.restart_backoff = std::chrono::milliseconds{1};
  PluginInvocationExecutor executor(
      std::filesystem::path(PS_TEST_ISOLATED_CPU_FIXTURE_PATH),
      supervisor_resource_ledger(), {}, options);
  const std::size_t descriptor_baseline = supervisor_open_descriptor_count();
  const IsolatedCpuInvocationTestSnapshot before =
      IsolatedCpuInvocationTestProbe::snapshot();
  ASSERT_FALSE(before.response_channel_observation_overflow_armed);

  IsolatedCpuHostInvocation invalid =
      supervisor_test_invocation("fixture.fill_sequence", 239U);
  invalid.outputs.clear();
  IsolatedCpuInvocationTestProbe::
      force_next_response_channel_observation_overflow(true);
  ASSERT_TRUE(IsolatedCpuInvocationTestProbe::snapshot()
                  .response_channel_observation_overflow_armed);
  EXPECT_THROW(executor.invoke(invalid), IsolatedCpuProtocolError);
  const IsolatedCpuInvocationTestSnapshot after_rejected_preflight =
      IsolatedCpuInvocationTestProbe::snapshot();
  EXPECT_EQ(after_rejected_preflight.spawned_children, before.spawned_children);
  EXPECT_EQ(after_rejected_preflight.reaped_children, before.reaped_children);
  EXPECT_EQ(after_rejected_preflight.exact_frames_received,
            before.exact_frames_received);
  EXPECT_FALSE(
      after_rejected_preflight.response_channel_observation_overflow_armed);
  EXPECT_EQ(supervisor_open_descriptor_count(), descriptor_baseline);

  const ObservedRuntimeFault fault =
      observe_runtime_fault_after_response_channel_observation_overflow(
          &executor, supervisor_test_invocation("fixture.response_hang", 240U));
  const IsolatedCpuInvocationTestSnapshot after_fault =
      IsolatedCpuInvocationTestProbe::snapshot();

  EXPECT_EQ(fault.kind, PluginRuntimeFaultKind::ResponseDeadline);
  EXPECT_NE(fault.kind, PluginRuntimeFaultKind::Channel);
  EXPECT_NE(fault.diagnostic.find("deadline arithmetic failed"),
            std::string::npos);
  expect_supervised_child_reaped(after_rejected_preflight, after_fault);
  EXPECT_EQ(after_fault.exact_frames_received,
            after_rejected_preflight.exact_frames_received);
  EXPECT_FALSE(after_fault.response_channel_observation_overflow_armed);
  const std::int64_t failed_reaped_pid = after_fault.last_reaped_child;

  const IsolatedCpuHostInvocationResult recovered = executor.invoke(
      supervisor_test_invocation("fixture.fill_sequence", 245U));
  ASSERT_EQ(recovered.outcome, IsolatedCpuInvocationOutcome::Succeeded);
  ASSERT_EQ(recovered.outputs.size(), 1U);
  const std::array<std::byte, 6U> expected{std::byte{0}, std::byte{1},
                                           std::byte{2}, std::byte{3},
                                           std::byte{4}, std::byte{5}};
  EXPECT_EQ(supervisor_test_bytes(recovered.outputs[0]), expected);
  const IsolatedCpuInvocationTestSnapshot after_recovery =
      IsolatedCpuInvocationTestProbe::snapshot();
  expect_supervised_child_reaped(after_fault, after_recovery);
  EXPECT_EQ(after_recovery.exact_frames_received,
            after_fault.exact_frames_received + 1U);
  EXPECT_NE(after_recovery.last_reaped_child, failed_reaped_pid);
  EXPECT_FALSE(after_recovery.response_channel_observation_overflow_armed);
  EXPECT_EQ(supervisor_open_descriptor_count(), descriptor_baseline);
}

/**
 * @brief Verifies a failed invocation never falls back in-process and a fresh
 * child serves the next request without PID or descriptor leakage.
 * @throws Standard fixture, transport, and assertion failures observed by
 * GoogleTest.
 */
TEST(PluginRuntimeSupervisor, FailedInvocationDoesNotFallbackAndLaterRecovers) {
  PluginRuntimeSupervisorOptions options = supervisor_test_options();
  options.restart_backoff = std::chrono::milliseconds{1};
  PluginInvocationExecutor executor(
      std::filesystem::path(PS_TEST_ISOLATED_CPU_FIXTURE_PATH),
      supervisor_resource_ledger(), {}, options);
  const std::size_t descriptor_baseline = supervisor_open_descriptor_count();
  const IsolatedCpuInvocationTestSnapshot before =
      IsolatedCpuInvocationTestProbe::snapshot();

  const ObservedRuntimeFault fault = observe_runtime_fault(
      &executor, supervisor_test_invocation("fixture.crash", 241U));
  EXPECT_EQ(fault.kind, PluginRuntimeFaultKind::ProcessExit);
  const IsolatedCpuHostInvocationResult recovered = executor.invoke(
      supervisor_test_invocation("fixture.fill_sequence", 242U));

  ASSERT_EQ(recovered.outcome, IsolatedCpuInvocationOutcome::Succeeded);
  ASSERT_EQ(recovered.outputs.size(), 1U);
  const IsolatedCpuInvocationTestSnapshot after =
      IsolatedCpuInvocationTestProbe::snapshot();
  EXPECT_EQ(after.spawned_children, before.spawned_children + 2U);
  EXPECT_EQ(after.reaped_children, before.reaped_children + 2U);
  EXPECT_EQ(supervisor_open_descriptor_count(), descriptor_baseline);
}

/**
 * @brief Proves a runtime crash fails only its owning Run callback and the
 * fixed service worker executes a later unrelated Run.
 * @throws Standard service, Run, and supervised invocation failures observed
 * by GoogleTest.
 */
TEST(PluginRuntimeSupervisor, RuntimeFaultRemainsRunLocalAndWorkerRecovers) {
  PluginInvocationExecutor executor(
      std::filesystem::path(PS_TEST_ISOLATED_CPU_FIXTURE_PATH),
      supervisor_resource_ledger(), {}, supervisor_test_options());
  compute::ExecutionService service(1U);
  SupervisorExecutionHost host;
  std::atomic_int completed{0};

  compute::ComputeRun failed_run(
      supervisor_run_submission("supervisor-failed-run", 301U, 41));
  std::vector<compute::ReadyTaskSubmission> failed_ready;
  failed_ready.push_back(supervised_ready_submission(
      failed_run.acquire_lease(), 0U, executor,
      supervisor_test_invocation("fixture.crash", 243U), &completed));
  try {
    service.execute_run(host, "cpu", std::move(failed_ready), 1);
    FAIL() << "runtime crash did not escape the owning Run boundary";
  } catch (const PluginRuntimeFault&) {
    EXPECT_TRUE(failed_run.publish_failed(std::current_exception()));
  }
  const auto failed_outcome = failed_run.terminal_outcome();
  ASSERT_TRUE(failed_outcome.has_value());
  EXPECT_EQ(failed_outcome->kind, compute::ComputeRunTerminalKind::Failed);
  EXPECT_EQ(completed.load(std::memory_order_relaxed), 0);

  compute::ComputeRun healthy_run(
      supervisor_run_submission("supervisor-healthy-run", 302U, 42));
  std::vector<compute::ReadyTaskSubmission> healthy_ready;
  healthy_ready.push_back(supervised_ready_submission(
      healthy_run.acquire_lease(), 0U, executor,
      supervisor_test_invocation("fixture.fill_sequence", 244U), &completed));
  EXPECT_NO_THROW(
      service.execute_run(host, "cpu", std::move(healthy_ready), 1));
  EXPECT_EQ(completed.load(std::memory_order_relaxed), 1);
  EXPECT_EQ(service.resource_snapshot().reserved, ResourceVector{});
}

}  // namespace
}  // namespace ps::execution
