/**
 * @file test_plugin_runtime_supervisor.cpp
 * @brief Verifies the product-linked isolated CPU runtime supervisor.
 */
#include <dirent.h>
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
#include <optional>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "compute/compute_run.hpp"               // NOLINT(build/include_subdir)
#include "compute/execution_service.hpp"         // NOLINT(build/include_subdir)
#include "execution/execution_task_runtime.hpp"  // NOLINT(build/include_subdir)
#include "execution/isolated_cpu_invocation_test_probe.hpp"  // NOLINT(build/include_subdir)
#include "execution/plugin_runtime_supervisor.hpp"  // NOLINT(build/include_subdir)

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
  return IsolatedCpuDenseTensorOutputPlan{
      supervisor_test_descriptor(), std::nullopt, supervisor_test_layout(), 6U};
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
  void set_task_context(int worker_id, std::uint64_t epoch) noexcept override {
    (void)worker_id;
    (void)epoch;
  }

  /** @copydoc ExecutionHostContext::clear_task_context */
  void clear_task_context() noexcept override {}

  /** @copydoc ExecutionHostContext::log_event */
  void log_event(ExecutionTraceAction action, int node_id, int worker_id,
                 std::uint64_t epoch) noexcept override {
    (void)action;
    (void)node_id;
    (void)worker_id;
    (void)epoch;
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
 * @brief Rejects zero restart backoff before any child ownership can begin.
 * @throws Nothing; GoogleTest records assertion failures.
 */
TEST(PluginRuntimeSupervisor, RejectsZeroRestartBackoffAtConstruction) {
  PluginRuntimeSupervisorOptions options = supervisor_test_options();
  options.restart_backoff = std::chrono::milliseconds{0};

  EXPECT_THROW(
      PluginRuntimeSupervisor(
          std::filesystem::path(PS_TEST_ISOLATED_CPU_FIXTURE_PATH), options),
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
      supervisor_test_options());
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
 * @brief Proves bounded request transfer does not consume the callback's full
 * invocation-completion window.
 * @throws Standard fixture, transport, and assertion failures observed by
 * GoogleTest.
 */
TEST(PluginRuntimeSupervisor, RequestTransferDoesNotConsumeCallbackBudget) {
  PluginRuntimeSupervisorOptions options = supervisor_test_options();
  options.invocation_timeout = std::chrono::milliseconds{1000};
  PluginInvocationExecutor executor(
      std::filesystem::path(PS_TEST_ISOLATED_CPU_FIXTURE_PATH), options);
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
      options);
  const ObservedRuntimeFault silent_fault = observe_runtime_fault(
      &silent, supervisor_test_invocation("fixture.fill_sequence", 113U));
  EXPECT_EQ(silent_fault.kind, PluginRuntimeFaultKind::StartupDeadline);
  EXPECT_NE(silent_fault.termination_stage,
            PluginRuntimeTerminationStage::None);

  PluginInvocationExecutor corrupt(
      std::filesystem::path(PS_TEST_PLUGIN_RUNTIME_CORRUPT_FIXTURE_PATH),
      options);
  const ObservedRuntimeFault corrupt_fault = observe_runtime_fault(
      &corrupt, supervisor_test_invocation("fixture.fill_sequence", 129U));
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
      std::filesystem::path(PS_TEST_ISOLATED_CPU_FIXTURE_PATH), options);
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
      std::filesystem::path(PS_TEST_ISOLATED_CPU_FIXTURE_PATH), options);
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
      std::filesystem::path(PS_TEST_ISOLATED_CPU_FIXTURE_PATH), options);
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
      std::filesystem::path(PS_TEST_ISOLATED_CPU_FIXTURE_PATH), options);
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
      supervisor_test_options());

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
      std::filesystem::path(PS_TEST_ISOLATED_CPU_FIXTURE_PATH), options);

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
      std::filesystem::path(PS_TEST_ISOLATED_CPU_FIXTURE_PATH), options);
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
      std::filesystem::path(PS_TEST_ISOLATED_CPU_FIXTURE_PATH), options);

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
      supervisor_test_options());
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
      std::filesystem::path(PS_TEST_ISOLATED_CPU_FIXTURE_PATH), options);
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
 * @brief Verifies a failed invocation never falls back in-process and a fresh
 * child serves the next request without PID or descriptor leakage.
 * @throws Standard fixture, transport, and assertion failures observed by
 * GoogleTest.
 */
TEST(PluginRuntimeSupervisor, FailedInvocationDoesNotFallbackAndLaterRecovers) {
  PluginRuntimeSupervisorOptions options = supervisor_test_options();
  options.restart_backoff = std::chrono::milliseconds{1};
  PluginInvocationExecutor executor(
      std::filesystem::path(PS_TEST_ISOLATED_CPU_FIXTURE_PATH), options);
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
      supervisor_test_options());
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
