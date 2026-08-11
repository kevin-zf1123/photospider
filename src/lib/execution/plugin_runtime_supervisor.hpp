/**
 * @file plugin_runtime_supervisor.hpp
 * @brief Declares bounded source-private isolated CPU runtime supervision.
 */
#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>

#include "execution/isolated_cpu_invocation.hpp"  // NOLINT(build/include_subdir)

namespace ps::execution {

/** @brief Fixed supervision descriptor installed in a supervised runtime. */
inline constexpr int kPluginRuntimeSupervisionDescriptor = 5;

/**
 * @brief Closed primary facts reported by one supervised runtime failure.
 * @note Values describe observable lifecycle facts, not #104 trust policy.
 */
enum class PluginRuntimeFaultKind : std::uint8_t {
  /** @brief Exec/session startup exceeded its absolute deadline. */
  StartupDeadline = 0,
  /** @brief Request transfer or callback exceeded its own absolute deadline. */
  InvocationDeadline = 1,
  /** @brief Response, exit, validation, or publication exceeded its deadline.
   */
  ResponseDeadline = 2,
  /** @brief Authenticated lifecycle traffic stopped within the gap bound. */
  HeartbeatTimeout = 3,
  /** @brief A fixed lifecycle frame violated version/session/sequence rules. */
  LifecycleProtocol = 4,
  /** @brief A supervision or invocation channel failed without stronger fact.
   */
  Channel = 5,
  /** @brief A normal runtime supplied absent, malformed, or inconsistent
     output. */
  BadOutput = 6,
  /** @brief The runtime naturally exited with a nonzero status. */
  ProcessExit = 7,
  /** @brief The runtime naturally terminated through a signal. */
  ProcessSignal = 8,
  /** @brief Exact PID ownership moved to the quarantining deferred reaper. */
  ReapPending = 9,
};

/**
 * @brief Strongest supervisor termination action sent to an exact child PID.
 */
enum class PluginRuntimeTerminationStage : std::uint8_t {
  /** @brief No supervisor termination signal was sent. */
  None = 0,
  /** @brief `SIGTERM` was the strongest signal sent. */
  Sigterm = 1,
  /** @brief `SIGKILL` was the strongest signal sent. */
  Sigkill = 2,
};

/**
 * @brief Positive bounded timing policy for one fresh runtime invocation.
 * @note These bounds provide lifecycle supervision only. They are not CPU,
 * memory, syscall, network, or package-trust policy.
 */
struct PluginRuntimeSupervisorOptions final {
  /** @brief Fork-through-authenticated-startup absolute bound. */
  std::chrono::milliseconds startup_timeout{2000};
  /** @brief Runtime heartbeat emission interval. */
  std::chrono::milliseconds heartbeat_interval{100};
  /** @brief Maximum gap between authenticated lifecycle events. */
  std::chrono::milliseconds heartbeat_timeout{500};
  /**
   * @brief Independent request-transfer and callback-completion bounds.
   * @note The complete request send receives this full duration first. A new
   * full-duration callback window starts only after that transfer completes.
   */
  std::chrono::milliseconds invocation_timeout{5000};
  /** @brief Completion-through-validated-publication absolute bound. */
  std::chrono::milliseconds response_timeout{2000};
  /** @brief Wait after `SIGTERM` before `SIGKILL`. */
  std::chrono::milliseconds termination_grace{250};
  /** @brief Final synchronous exact-PID wait after `SIGKILL`. */
  std::chrono::milliseconds kill_reap_timeout{1000};
  /** @brief Delay before launching after a prior classified fault. */
  std::chrono::milliseconds restart_backoff{10};
};

/**
 * @brief Host-owned typed failure from one supervised runtime invocation.
 * @throws std::bad_alloc when retaining bounded diagnostic storage fails.
 */
class PluginRuntimeFault : public std::runtime_error {
 public:
  /**
   * @brief Creates one complete observable runtime-fault record.
   * @param kind Primary failure fact preserved across cleanup escalation.
   * @param message Bounded-intended Host diagnostic.
   * @param wait_status Exact POSIX wait status when observed.
   * @param exit_code Exact natural exit code when applicable.
   * @param signal_number Exact terminating signal when applicable.
   * @param termination_stage Strongest supervisor signal sent.
   * @param memory_pressure_compatible True only for observed `SIGKILL`.
   * @throws std::bad_alloc when runtime-error storage cannot allocate.
   * @note The compatibility bit never proves OOM causation.
   */
  PluginRuntimeFault(PluginRuntimeFaultKind kind, const std::string& message,
                     std::optional<int> wait_status = std::nullopt,
                     std::optional<int> exit_code = std::nullopt,
                     std::optional<int> signal_number = std::nullopt,
                     PluginRuntimeTerminationStage termination_stage =
                         PluginRuntimeTerminationStage::None,
                     bool memory_pressure_compatible = false);

  /**
   * @brief Returns the primary observable fault kind.
   * @return Closed lifecycle/process/output classification.
   * @throws Nothing.
   */
  PluginRuntimeFaultKind kind() const noexcept { return kind_; }
  /**
   * @brief Returns the exact wait status when one was observed.
   * @return Exact POSIX status or no value before reconciliation.
   * @throws Nothing.
   */
  std::optional<int> wait_status() const noexcept { return wait_status_; }
  /**
   * @brief Returns the natural nonzero exit code when applicable.
   * @return Exit code or no value for non-exit facts.
   * @throws Nothing.
   */
  std::optional<int> exit_code() const noexcept { return exit_code_; }
  /**
   * @brief Returns the terminating signal when applicable.
   * @return Signal number or no value when none was observed.
   * @throws Nothing.
   */
  std::optional<int> signal_number() const noexcept { return signal_number_; }
  /**
   * @brief Returns the strongest supervisor signal sent.
   * @return None, SIGTERM, or SIGKILL escalation stage.
   * @throws Nothing.
   */
  PluginRuntimeTerminationStage termination_stage() const noexcept {
    return termination_stage_;
  }
  /**
   * @brief Reports whether an observed signal is memory-pressure compatible.
   * @return True only for `SIGKILL`; never an OOM-causation assertion.
   * @throws Nothing.
   */
  bool memory_pressure_compatible() const noexcept {
    return memory_pressure_compatible_;
  }

 private:
  /** @brief Closed primary failure fact. */
  PluginRuntimeFaultKind kind_;
  /** @brief Exact wait status, when reconciled. */
  std::optional<int> wait_status_;
  /** @brief Natural nonzero exit code, when applicable. */
  std::optional<int> exit_code_;
  /** @brief Natural or escalation terminating signal, when applicable. */
  std::optional<int> signal_number_;
  /** @brief Strongest owned escalation action. */
  PluginRuntimeTerminationStage termination_stage_;
  /** @brief `SIGKILL` observation without a causal OOM assertion. */
  bool memory_pressure_compatible_ = false;
};

/**
 * @brief Owns bounded lifecycle supervision for fresh isolated CPU runtimes.
 *
 * Each call preflights the #102 wire, launches one fresh execed process,
 * authenticates a private nonce-bound lifecycle stream, monitors heartbeat and
 * absolute deadlines, validates one response, and exactly retires the process
 * and capabilities. The object serializes calls so one instance never owns
 * overlapping child PIDs.
 *
 * @throws std::invalid_argument for invalid construction options.
 * @throws std::system_error when POSIX process-state inspection fails.
 */
class PluginRuntimeSupervisor final {
 public:
  /**
   * @brief Validates and retains the runtime path, protocol limits, and bounds.
   * @param runtime_executable Existing executable regular file.
   * @param options Positive monotonic lifecycle bounds.
   * @param limits Protocol-v1 request/response validation bounds.
   * @throws std::invalid_argument for an invalid path, limit, or duration.
   * @throws std::system_error when `SIGCHLD` state cannot be queried.
   * @throws std::bad_alloc when retained private state cannot allocate.
   * @note Path validation is operability only, not plugin trust admission.
   */
  explicit PluginRuntimeSupervisor(std::filesystem::path runtime_executable,
                                   PluginRuntimeSupervisorOptions options = {},
                                   IsolatedCpuInvocationLimits limits = {});

  /**
   * @brief Retires private state without an intentional unbounded caller wait.
   * @throws Nothing.
   * @note Any still-owned child enters the emergency exact-reap path.
   */
  ~PluginRuntimeSupervisor() noexcept;

  /**
   * @brief Prevents duplicate process-lifecycle authority.
   * @param other Source supervisor, never consumed because copying is deleted.
   * @throws Nothing because the operation is deleted.
   */
  PluginRuntimeSupervisor(const PluginRuntimeSupervisor&) = delete;
  /**
   * @brief Prevents duplicate process-lifecycle assignment.
   * @param other Source supervisor, never consumed because copying is deleted.
   * @return No value because assignment is deleted.
   * @throws Nothing because the operation is deleted.
   */
  PluginRuntimeSupervisor& operator=(const PluginRuntimeSupervisor&) = delete;
  /**
   * @brief Prevents moving an address-stable serialized supervisor.
   * @param other Source supervisor, never consumed because moving is deleted.
   * @throws Nothing because the operation is deleted.
   */
  PluginRuntimeSupervisor(PluginRuntimeSupervisor&&) = delete;
  /**
   * @brief Prevents moving an address-stable serialized supervisor.
   * @param other Source supervisor, never consumed because moving is deleted.
   * @return No value because assignment is deleted.
   * @throws Nothing because the operation is deleted.
   */
  PluginRuntimeSupervisor& operator=(PluginRuntimeSupervisor&&) = delete;

  /**
   * @brief Executes one authenticated, heartbeat-monitored invocation.
   * @param invocation Host-owned request, inputs, and exact output plans.
   * @return Typed callback outcome with fresh Values only after full success.
   * @throws PluginRuntimeFault for supervised lifecycle/process/output faults.
   * @throws IsolatedCpuProtocolError for invalid Host preflight state before a
   * child is created.
   * @throws Value/readiness/access/allocation exceptions from Host preparation
   * or fresh output publication.
   * @note A failure never falls back to direct non-supervised invocation.
   * Complete request transfer and callback completion each receive a fresh
   * full `invocation_timeout` window.
   */
  IsolatedCpuHostInvocationResult invoke(
      const IsolatedCpuHostInvocation& invocation);

  /**
   * @brief Returns the retained operability-validated executable path.
   * @return Borrowed path valid for this supervisor's lifetime.
   * @throws Nothing.
   */
  const std::filesystem::path& runtime_executable() const noexcept;
  /**
   * @brief Returns the retained lifecycle timing policy by value.
   * @return Complete validated duration policy.
   * @throws Nothing.
   */
  PluginRuntimeSupervisorOptions options() const noexcept;
  /**
   * @brief Returns the retained protocol-v1 limits by value.
   * @return Complete validated protocol-v1 bounds.
   * @throws Nothing.
   */
  IsolatedCpuInvocationLimits limits() const noexcept;

 private:
  /** @brief Address-stable private synchronization and recovery state. */
  class Impl;
  /** @brief Sole private implementation owner. */
  std::unique_ptr<Impl> impl_;
};

/**
 * @brief Source-private product executor selecting only supervised invocation.
 * @throws Construction and invocation failures from its owned supervisor.
 * @note This is not an installed API or current end-user plugin route.
 */
class PluginInvocationExecutor final {
 public:
  /**
   * @brief Constructs the one owned supervised execution route.
   * @param runtime_executable Existing executable regular file.
   * @param options Positive lifecycle bounds.
   * @param limits Protocol-v1 endpoint bounds.
   * @throws Construction failures from `PluginRuntimeSupervisor` unchanged.
   */
  explicit PluginInvocationExecutor(std::filesystem::path runtime_executable,
                                    PluginRuntimeSupervisorOptions options = {},
                                    IsolatedCpuInvocationLimits limits = {});

  /**
   * @brief Invokes only the owned supervised route.
   * @param invocation Host-owned invocation plan.
   * @return Complete Host result from the supervisor.
   * @throws All supervisor/preflight/publication failures unchanged.
   * @note There is no direct-adapter fallback or automatic downgrade.
   */
  IsolatedCpuHostInvocationResult invoke(
      const IsolatedCpuHostInvocation& invocation);

 private:
  /** @brief Sole supervised process-lifecycle route. */
  PluginRuntimeSupervisor supervisor_;
};

/**
 * @brief Deterministic source-private endpoint startup behavior.
 * @note Non-normal values exist to exercise long-lived fail-closed behavior;
 * they create no wire authority or production fallback.
 */
enum class PluginRuntimeEndpointStartupBehavior : std::uint8_t {
  /** @brief Echo the exact nonce and enter the normal endpoint. */
  Normal = 0,
  /** @brief Remain silent so the Host startup deadline must retire the child.
   */
  SuppressStarted = 1,
  /** @brief Corrupt the nonce in the first event so authentication must fail.
   */
  CorruptStartedNonce = 2,
};

/**
 * @brief Callback-adjacent endpoint milestones exposed to private fixtures.
 */
enum class PluginRuntimeLifecyclePoint : std::uint8_t {
  /** @brief Callback result and output bindings exist before completion event.
   */
  BeforeInvocationCompleted = 0,
  /** @brief Completion is authenticated but response bytes are not yet sent. */
  BeforeResponse = 1,
};

/**
 * @brief Process-local lifecycle instrumentation invoked by the endpoint.
 * @param point Exact endpoint milestone.
 * @param invocation Fully validated callback-local invocation.
 * @return Nothing after optional process-local instrumentation.
 * @throws Any exception; the endpoint contains it as infrastructure failure.
 * @note The hook is never serialized and must not retain invocation pointers.
 */
using PluginRuntimeLifecycleHook = std::function<void(
    PluginRuntimeLifecyclePoint, const IsolatedCpuRuntimeInvocation&)>;

/**
 * @brief Serves one authenticated, heartbeat-emitting runtime invocation.
 * @param control_fd Connected #102 framed data socket, normally fd 3.
 * @param supervision_fd Connected fixed lifecycle socket, normally fd 5.
 * @param limits Runtime-local protocol-v1 hard bounds.
 * @param callback Nonempty process-local CPU callback.
 * @param startup_behavior Deterministic startup behavior.
 * @param lifecycle_hook Optional process-local milestone instrumentation.
 * @return Zero after complete response send; nonzero for contained failure.
 * @throws Nothing; protocol, callback-adjacent, and channel failures are
 * contained as process status.
 * @note Session authentication binds this private launch only. The endpoint
 * supplies no package trust, sandbox, quota, or hostile-code attestation.
 */
int serve_supervised_isolated_cpu_invocation_once(
    int control_fd, int supervision_fd,
    const IsolatedCpuInvocationLimits& limits,
    const IsolatedCpuRuntimeCallback& callback,
    PluginRuntimeEndpointStartupBehavior startup_behavior =
        PluginRuntimeEndpointStartupBehavior::Normal,
    const PluginRuntimeLifecycleHook& lifecycle_hook = {}) noexcept;

}  // namespace ps::execution
