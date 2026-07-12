#include "photospider/ipc/host.hpp"

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <functional>
#include <future>
#include <iterator>
#include <limits>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "ipc/client_interrupt_access.hpp"
#include "ipc/codec.hpp"
#include "ipc/ipc_host_runtime.hpp"
#include "ipc/unix_socket.hpp"
#include "photospider/ipc/client.hpp"

namespace ps::ipc {
namespace {

/**
 * @brief Builds the exact adapter-stop status required by protocol version 1.
 * @return Transport code 5/name `client_stopped`.
 * @throws std::bad_alloc if diagnostic storage cannot be allocated.
 * @note This is a local lifecycle classification and never appears on wire.
 */
OperationStatus client_stopped_status() {
  return internal::failure_status(OperationErrorDomain::Transport, 5,
                                  "client_stopped",
                                  "IPC Host adapter has stopped");
}

/**
 * @brief Builds a local malformed-peer status for adapter-level invariants.
 * @param message Exact diagnostic explaining the violated typed invariant.
 * @return Protocol `invalid_request` failure.
 * @throws std::bad_alloc if diagnostic storage cannot be allocated.
 * @note The helper is used only after a typed Client call returned success.
 */
OperationStatus invalid_adapter_response(std::string message) {
  return internal::failure_status(OperationErrorDomain::Protocol,
                                  internal::kInvalidRequestCode,
                                  "invalid_request", std::move(message));
}

/**
 * @brief Converts one Host graph-session label to its daemon opaque id.
 * @param session Host session value returned previously by this adapter.
 * @return Opaque IPC session value with identical owned text.
 * @throws std::bad_alloc if copied identifier storage cannot allocate.
 * @note No parsing, normalization, or backend lookup occurs locally.
 */
IpcSessionId ipc_session(const GraphSessionId& session) {
  return {session.value};
}

/**
 * @brief Executes one typed value call through a fresh Client connection.
 * @tparam Value Public value returned by the typed method.
 * @tparam Call Callable receiving the connected Client exactly once.
 * @param socket_path Immutable daemon socket path.
 * @param call Exact typed Client operation to invoke.
 * @return Typed call result or the one-shot connection failure.
 * @throws std::bad_alloc if Client, request, response, or value allocation
 *         fails.
 * @note The helper performs one connect and one callable invocation. It never
 *       reconnects or retries a mutation.
 */
template <typename Value, typename Call>
IpcResult<Value> short_value_call(const std::string& socket_path, Call call) {
  Client client;
  OperationStatus connected = client.connect(socket_path);
  if (!connected.ok) {
    return {std::move(connected), {}};
  }
  return call(client);
}

/**
 * @brief Executes one typed void call through a fresh Client connection.
 * @tparam Call Callable receiving the connected Client exactly once.
 * @param socket_path Immutable daemon socket path.
 * @param call Exact typed Client operation to invoke.
 * @return Void status from connect or the one typed operation.
 * @throws std::bad_alloc if Client, request, response, or status allocation
 *         fails.
 * @note No automatic reconnect or mutation retry is performed.
 */
template <typename Call>
VoidResult short_void_call(const std::string& socket_path, Call call) {
  Client client;
  OperationStatus connected = client.connect(socket_path);
  if (!connected.ok) {
    return {std::move(connected)};
  }
  return call(client);
}

/**
 * @brief Moves a typed IPC value result into the equivalent Host result.
 * @tparam Value Shared public payload type.
 * @param result IPC result to consume.
 * @return Host-shaped status/value result without status translation.
 * @throws Whatever moving the status or value throws.
 * @note Every error domain, numeric code, stable name, and message is retained.
 */
template <typename Value>
Result<Value> host_result(IpcResult<Value> result) {
  return {std::move(result.status), std::move(result.value)};
}

class AsyncCompletion;

/**
 * @brief Shared stop, active-descriptor, socket, and injected polling state.
 *
 * @throws std::bad_alloc when immutable state or active Client tracking grows.
 * @note Workers register stable shared Client ownership before connecting.
 *       Destruction publishes stop first, wakes waiters, and then calls
 *       shutdown while holding this registry mutex. A worker unregisters
 *       before its final Client reference can close the descriptor, so no raw
 *       pointer snapshot or fd-reuse window exists.
 */
class PollingRuntimeState {
 public:
  /**
   * @brief Stores immutable socket and complete runtime dependencies.
   * @param socket_path Absolute daemon socket path.
   * @param dependencies Validated clock/wait/wake/artifact callbacks.
   * @throws std::bad_alloc if path or callback transfer fails.
   */
  PollingRuntimeState(std::string socket_path,
                      internal::IpcHostRuntimeDependencies dependencies)
      : socket_path_(std::move(socket_path)),
        dependencies_(std::move(dependencies)) {}

  /**
   * @brief Returns the immutable daemon socket path.
   * @return Borrowed path valid for this shared state's lifetime.
   * @throws Nothing.
   */
  const std::string& socket_path() const noexcept { return socket_path_; }

  /**
   * @brief Returns the immutable callback bundle.
   * @return Borrowed callbacks valid through worker join.
   * @throws Nothing.
   */
  const internal::IpcHostRuntimeDependencies& dependencies() const noexcept {
    return dependencies_;
  }

  /**
   * @brief Registers one worker Client before it begins connecting.
   * @param client Stable shared Client owned through unregister.
   * @return False when adapter stop already forbids a new RPC.
   * @throws std::bad_alloc if active tracking growth fails.
   * @note Publishing before connect closes the stop/descriptor race. A stop
   *       during connect may observe no descriptor, so the caller must check
   *       `stopped()` again after connect and before writing its RPC.
   */
  bool register_client(const std::shared_ptr<Client>& client) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (stopped_) {
      return false;
    }
    active_clients_.push_back(client);
    return true;
  }

  /**
   * @brief Removes one active Client before worker-local destruction.
   * @param client Exact registered Client address.
   * @throws Nothing.
   * @note Shared ownership remains stable while the registry mutex is held.
   */
  void unregister_client(const Client* client) noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    active_clients_.erase(
        std::remove_if(active_clients_.begin(), active_clients_.end(),
                       [client](const std::shared_ptr<Client>& active) {
                         return active.get() == client;
                       }),
        active_clients_.end());
  }

  /**
   * @brief Atomically publishes adapter stop.
   * @throws Nothing.
   * @note This method does not wake or interrupt; the destructor preserves the
   *       specified stop, wake, shutdown, future-completion, join order.
   */
  void stop() noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    stopped_ = true;
  }

  /**
   * @brief Reports whether stop has been published.
   * @return True after adapter destruction begins.
   * @throws Nothing.
   */
  bool stopped() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return stopped_;
  }

  /**
   * @brief Shuts down every currently registered worker Client.
   * @throws Nothing.
   * @note The mutex remains held while stable shared Client ownership is used.
   *       `ClientInterruptAccess` takes only the Client descriptor mutex and no
   *       path calls back into this runtime, so lock ordering is acyclic.
   *       Shutdown wakes IO; workers retain sole close ownership.
   */
  void interrupt_active_clients() noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const std::shared_ptr<Client>& client : active_clients_) {
      internal::ClientInterruptAccess::interrupt(client.get());
    }
  }

  /**
   * @brief Publishes a worker status only if stop has not linearized first.
   * @param completion Exactly-once future completion.
   * @param status Terminal or RPC status produced by the worker.
   * @throws std::future_error only if promise invariants are violated.
   * @note This method holds the same mutex as `stop()`, making adapter stop and
   *       worker outcome publication one total order. A worker that loses to
   *       stop discards its outcome; only the destructor publishes preallocated
   *       client_stopped after wake and descriptor interruption.
   */
  void complete_worker_status(
      const std::shared_ptr<AsyncCompletion>& completion,
      OperationStatus status);

  /**
   * @brief Publishes a worker exception only if stop has not linearized first.
   * @param completion Exactly-once future completion.
   * @param exception Worker exception to retain.
   * @throws std::future_error only if promise invariants are violated.
   * @note Stop discards the exception through the same runtime mutex; only the
   *       destructor publishes client_stopped in the required lifecycle order.
   */
  void complete_worker_exception(
      const std::shared_ptr<AsyncCompletion>& completion,
      std::exception_ptr exception);

 private:
  /** @brief Immutable daemon Unix socket path. */
  std::string socket_path_;

  /** @brief Clock, waiter, wake, and artifact ownership policy. */
  internal::IpcHostRuntimeDependencies dependencies_;

  /** @brief Serializes stop and stable active Client ownership. */
  mutable std::mutex mutex_;

  /** @brief True after adapter destruction begins. */
  bool stopped_ = false;

  /** @brief Stable worker Clients whose descriptors may need shutdown. */
  std::vector<std::shared_ptr<Client>> active_clients_;
};

/**
 * @brief RAII registration for one active polling/result/release Client.
 * @throws std::bad_alloc if Client or registry allocation fails.
 * @note Registration precedes connect; destruction unregisters before the
 *       shared Client can close its descriptor.
 */
class ActiveClientRegistration {
 public:
  /**
   * @brief Creates and conditionally registers one disconnected Client.
   * @param runtime Shared adapter runtime.
   * @throws std::bad_alloc if Client or registry allocation fails.
   */
  explicit ActiveClientRegistration(
      std::shared_ptr<PollingRuntimeState> runtime)
      : runtime_(std::move(runtime)), client_(std::make_shared<Client>()) {
    registered_ = runtime_->register_client(client_);
  }

  /**
   * @brief Unregisters before releasing worker-local shared ownership.
   * @throws Nothing.
   */
  ~ActiveClientRegistration() {
    if (registered_) {
      runtime_->unregister_client(client_.get());
    }
  }

  /**
   * @brief Prevents duplicating one active registry entry.
   * @throws Nothing because this operation is unavailable.
   */
  ActiveClientRegistration(const ActiveClientRegistration&) = delete;

  /**
   * @brief Prevents replacing active registry ownership by copy.
   * @return No value because this operation is unavailable.
   * @throws Nothing because this operation is unavailable.
   */
  ActiveClientRegistration& operator=(const ActiveClientRegistration&) = delete;

  /**
   * @brief Reports whether registration beat adapter stop.
   * @return True when the Client may attempt connect.
   * @throws Nothing.
   */
  bool registered() const noexcept { return registered_; }

  /**
   * @brief Returns the stable worker-owned Client.
   * @return Shared Client retained through unregister.
   * @throws Nothing.
   */
  const std::shared_ptr<Client>& client() const noexcept { return client_; }

 private:
  /** @brief Shared runtime receiving unregister. */
  std::shared_ptr<PollingRuntimeState> runtime_;

  /** @brief Stable Client ownership published in the active registry. */
  std::shared_ptr<Client> client_;

  /** @brief Whether active registration succeeded. */
  bool registered_ = false;
};

/**
 * @brief Performs one stop-aware worker value RPC through a fresh Client.
 * @tparam Value Typed result payload.
 * @tparam Call Callable invoking exactly one Client method.
 * @param runtime Shared adapter polling runtime.
 * @param call Exact typed operation.
 * @return Typed result, exact connection/RPC failure, or client_stopped.
 * @throws std::bad_alloc if Client, registry, request, or value allocation
 *         fails.
 * @note The Client is registered before connect and stop is rechecked after
 *       connect before any request write. Each RPC is attempted once.
 */
template <typename Value, typename Call>
IpcResult<Value> tracked_value_call(
    const std::shared_ptr<PollingRuntimeState>& runtime, Call call) {
  ActiveClientRegistration registration(runtime);
  if (!registration.registered()) {
    return {client_stopped_status(), {}};
  }
  OperationStatus connected =
      registration.client()->connect(runtime->socket_path());
  if (!connected.ok) {
    if (runtime->stopped()) {
      return {client_stopped_status(), {}};
    }
    return {std::move(connected), {}};
  }
  if (runtime->stopped()) {
    internal::ClientInterruptAccess::interrupt(registration.client().get());
    return {client_stopped_status(), {}};
  }
  IpcResult<Value> result = call(*registration.client());
  if (runtime->stopped()) {
    return {client_stopped_status(), {}};
  }
  return result;
}

/**
 * @brief Copies one Host compute request into an opaque-session submission.
 * @param request Existing public Host request.
 * @param mode Status-only or image result mode.
 * @return Complete typed submission preserving every compute option.
 * @throws std::bad_alloc if copied strings allocate.
 */
ComputeSubmitRequest compute_submission(const HostComputeRequest& request,
                                        ComputeResultMode mode) {
  ComputeSubmitRequest submit;
  submit.session_id = ipc_session(request.session);
  submit.node = request.node;
  submit.cache = request.cache;
  submit.execution = request.execution;
  submit.telemetry = request.telemetry;
  submit.intent = request.intent;
  submit.dirty_roi = request.dirty_roi;
  submit.result_mode = mode;
  return submit;
}

/**
 * @brief Reports whether one compute job state is terminal.
 * @param state Typed job lifecycle state.
 * @return True for Succeeded or Failed.
 * @throws Nothing.
 */
bool terminal_state(ComputeJobState state) noexcept {
  return state == ComputeJobState::Succeeded ||
         state == ComputeJobState::Failed;
}

/**
 * @brief Compares two complete immutable operation statuses.
 * @param left First status.
 * @param right Second status.
 * @return True when every canonical/nested field is equal.
 * @throws Nothing.
 */
bool same_status(const OperationStatus& left,
                 const OperationStatus& right) noexcept {
  return left.ok == right.ok && left.domain == right.domain &&
         left.code == right.code && left.name == right.name &&
         left.message == right.message;
}

/**
 * @brief Result of polling one accepted job to a terminal status snapshot.
 * @throws std::bad_alloc when copied status or snapshot storage allocates.
 * @note `snapshot` is authoritative only when `status.ok` is true.
 */
struct TerminalPollResult {
  /** @brief Poll orchestration status. */
  OperationStatus status;

  /** @brief Last validated terminal status snapshot. */
  ComputeJobSnapshot snapshot;
};

/**
 * @brief Polls one accepted job with the exact interruptible cadence.
 * @param runtime Shared adapter runtime.
 * @param submitted Accepted queued job returned by submit.
 * @return Terminal snapshot or first exact RPC/lifecycle/protocol failure.
 * @throws std::bad_alloc if Client, failure-status diagnostic, or callback
 *         allocation fails.
 * @throws std::system_error if the production waiter cannot lock or wait on
 *         its synchronization primitives.
 * @throws Whatever an injected clock, waiter, or stop predicate throws.
 * @note The first status RPC is immediate. Each successful nonterminal status
 *       waits 10 ms, then 20, 40, 80, 160, 320, and at most 500 ms thereafter.
 *       Every status uses one fresh Client and one attempt; no total timeout or
 *       resubmission exists.
 */
TerminalPollResult poll_to_terminal(
    const std::shared_ptr<PollingRuntimeState>& runtime,
    const ComputeJobSnapshot& submitted) {
  std::chrono::milliseconds delay(10);
  ComputeJobState previous = ComputeJobState::Queued;
  while (true) {
    IpcResult<ComputeJobSnapshot> polled =
        tracked_value_call<ComputeJobSnapshot>(
            runtime, [&submitted](Client& client) {
              return client.compute_status(submitted.compute_id);
            });
    if (!polled.status.ok) {
      return {std::move(polled.status), {}};
    }
    if (polled.value.session_id.value != submitted.session_id.value) {
      return {invalid_adapter_response(
                  "compute.status returned a mismatched submitted session"),
              {}};
    }
    if (previous == ComputeJobState::Running &&
        polled.value.state == ComputeJobState::Queued) {
      return {invalid_adapter_response(
                  "compute.status regressed from running to queued"),
              {}};
    }
    previous = polled.value.state;
    if (terminal_state(polled.value.state)) {
      return {internal::ok_status(), std::move(polled.value)};
    }

    const std::chrono::steady_clock::time_point deadline =
        runtime->dependencies().monotonic_now() + delay;
    const bool interrupted = runtime->dependencies().wait_until(
        deadline, [runtime] { return runtime->stopped(); });
    if (interrupted || runtime->stopped()) {
      return {client_stopped_status(), {}};
    }
    delay = std::min(delay * 2, std::chrono::milliseconds(500));
  }
}

/**
 * @brief Validates immutable result identity against terminal status polling.
 * @param submitted Original accepted queued snapshot.
 * @param terminal Last terminal status snapshot.
 * @param result Terminal `compute.result` snapshot.
 * @param allow_output Whether image output metadata is valid for this request.
 * @return Canonical success or local Protocol failure.
 * @throws std::bad_alloc if failure diagnostics allocate.
 * @note This binds the session omitted from direct Client status/result
 *       expectations and rejects any terminal state/status mutation.
 */
OperationStatus validate_terminal_result(const ComputeJobSnapshot& submitted,
                                         const ComputeJobSnapshot& terminal,
                                         const ComputeJobSnapshot& result,
                                         bool allow_output) {
  if (result.session_id.value != submitted.session_id.value ||
      result.compute_id.value != submitted.compute_id.value) {
    return invalid_adapter_response(
        "compute.result returned a mismatched submitted identity");
  }
  if (result.state != terminal.state || !result.status || !terminal.status ||
      !same_status(*result.status, *terminal.status)) {
    return invalid_adapter_response(
        "compute.result changed the immutable terminal outcome");
  }
  if (!allow_output && result.output) {
    return invalid_adapter_response(
        "status compute.result unexpectedly returned image output");
  }
  return internal::ok_status();
}

/**
 * @brief Releases one terminal job best-effort through one fresh connection.
 * @param runtime Shared adapter runtime.
 * @param compute_id Terminal job identity.
 * @param delivery_id Optional matching artifact delivery lease.
 * @throws Nothing; allocation and transport failures are contained.
 * @note Stop suppresses cleanup so unfinished accepted jobs remain daemon-
 *       owned for explicit access, bounded eviction, or TTL. No retry occurs.
 */
void best_effort_release(
    const std::shared_ptr<PollingRuntimeState>& runtime,
    const ComputeRequestId& compute_id,
    const DeliveryLeaseId* delivery_id = nullptr) noexcept {
  if (runtime->stopped()) {
    return;
  }
  try {
    std::optional<DeliveryLeaseId> copied_delivery;
    if (delivery_id != nullptr) {
      copied_delivery = *delivery_id;
    }
    (void)tracked_value_call<ComputeReleaseResult>(
        runtime, [&compute_id, &copied_delivery](Client& client) {
          return client.release_compute(compute_id, copied_delivery);
        });
  } catch (...) {
  }
}

/**
 * @brief Scope guard for terminal-known best-effort compute cleanup.
 *
 * @throws Nothing for construction, lease attachment, and destruction.
 * @note The guard starts with only the trusted submitted compute id. Result
 *       outer failures and cross-RPC mismatches therefore release without an
 *       untrusted lease. After successful result validation, image paths may
 *       attach the matching delivery id. Adapter stop suppresses all cleanup.
 */
class TerminalCleanupGuard {
 public:
  /**
   * @brief Arms terminal cleanup for one accepted job.
   * @param runtime Shared adapter runtime that outlives this guard.
   * @param compute_id Trusted submitted job identity that outlives this guard.
   * @throws Nothing.
   */
  TerminalCleanupGuard(const std::shared_ptr<PollingRuntimeState>& runtime,
                       const ComputeRequestId& compute_id) noexcept
      : runtime_(runtime), compute_id_(&compute_id) {}

  /**
   * @brief Releases the terminal job/optional lease best-effort.
   * @throws Nothing.
   */
  ~TerminalCleanupGuard() {
    best_effort_release(runtime_, *compute_id_, delivery_id_);
  }

  /**
   * @brief Prevents duplicate cleanup attempts for one terminal job.
   * @throws Nothing because this operation is unavailable.
   */
  TerminalCleanupGuard(const TerminalCleanupGuard&) = delete;

  /**
   * @brief Prevents replacing one armed terminal cleanup by copy.
   * @return No value because this operation is unavailable.
   * @throws Nothing because this operation is unavailable.
   */
  TerminalCleanupGuard& operator=(const TerminalCleanupGuard&) = delete;

  /**
   * @brief Adds one successfully validated matching delivery lease.
   * @param delivery_id Typed result lease identity.
   * @throws Nothing.
   * @note The result object is declared before this guard, so the borrowed
   *       lease remains alive through guard destruction and no cleanup-path
   *       allocation can lose the matching lease.
   */
  void set_delivery(const DeliveryLeaseId& delivery_id) noexcept {
    delivery_id_ = &delivery_id;
  }

 private:
  /** @brief Shared runtime used for one cleanup attempt. */
  std::shared_ptr<PollingRuntimeState> runtime_;

  /** @brief Borrowed trusted job identity from successful submit. */
  const ComputeRequestId* compute_id_ = nullptr;

  /** @brief Optional validated lease borrowed from the live result object. */
  const DeliveryLeaseId* delivery_id_ = nullptr;
};

/**
 * @brief Preallocated exactly-once completion for one async Host future.
 *
 * @throws std::bad_alloc when promise shared state or preallocated stop status
 *         is constructed.
 * @note Preallocating `client_stopped` lets the noexcept adapter destructor
 * move that owned status into an unfinished promise without allocating.
 */
class AsyncCompletion {
 public:
  /**
   * @brief Creates an empty promise and preallocates its stop status.
   * @throws std::bad_alloc if promise or diagnostic storage cannot allocate.
   */
  AsyncCompletion() : stopped_status_(client_stopped_status()) {}

  /**
   * @brief Takes the caller-visible future exactly once.
   * @return Future bound to this completion.
   * @throws std::future_error if called more than once.
   */
  std::future<OperationStatus> take_future() { return promise_.get_future(); }

  /**
   * @brief Publishes one final status when still unfinished.
   * @param status Exact terminal/RPC status to move into the future.
   * @throws std::future_error only if promise invariants are violated.
   * @note Atomic arbitration prevents worker/destructor double completion.
   */
  void complete(OperationStatus status) {
    if (!completed_.exchange(true)) {
      promise_.set_value(std::move(status));
    }
  }

  /**
   * @brief Publishes one worker exception when still unfinished.
   * @param exception Owned exception captured by the worker boundary.
   * @throws std::future_error only if promise invariants are violated.
   * @note Resource exhaustion remains exceptional per Host contract.
   */
  void complete_exception(std::exception_ptr exception) {
    if (!completed_.exchange(true)) {
      promise_.set_exception(std::move(exception));
    }
  }

  /**
   * @brief Completes an unfinished future with preallocated client_stopped.
   * @throws Nothing; unexpected future errors are contained.
   * @note Called only by adapter destruction after stop/wake/IO interruption.
   */
  void complete_stopped() noexcept {
    if (!completed_.exchange(true)) {
      try {
        promise_.set_value(std::move(stopped_status_));
      } catch (...) {
      }
    }
  }

 private:
  /** @brief Promise shared state returned to the Host caller. */
  std::promise<OperationStatus> promise_;

  /** @brief Exactly-once worker/destructor arbitration flag. */
  std::atomic<bool> completed_{false};

  /** @brief Owned no-allocation destructor completion value. */
  OperationStatus stopped_status_;
};

/** @copydoc PollingRuntimeState::complete_worker_status */
void PollingRuntimeState::complete_worker_status(
    const std::shared_ptr<AsyncCompletion>& completion,
    OperationStatus status) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (stopped_) {
    return;
  }
  completion->complete(std::move(status));
}

/** @copydoc PollingRuntimeState::complete_worker_exception */
void PollingRuntimeState::complete_worker_exception(
    const std::shared_ptr<AsyncCompletion>& completion,
    std::exception_ptr exception) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (stopped_) {
    return;
  }
  completion->complete_exception(std::move(exception));
}

/**
 * @brief Start/cancel gate created before remote async submission.
 *
 * @throws std::bad_alloc when accepted job identities are copied into the gate.
 * @note Creating the waiting thread before `compute.submit` ensures a local
 *       thread-creation failure cannot strand an accepted remote job without a
 *       caller future. The gate contains no daemon cancellation behavior.
 */
class AsyncStartGate {
 public:
  /**
   * @brief Waits until submission succeeds or local startup is abandoned.
   * @param submitted Receives the accepted queued snapshot on success.
   * @return True only after `start()` publishes both identities.
   * @throws std::bad_alloc if identity copy to outputs fails.
   */
  bool wait(ComputeJobSnapshot* submitted) {
    std::unique_lock<std::mutex> lock(mutex_);
    changed_.wait(lock, [this] { return ready_ || cancelled_; });
    if (cancelled_) {
      return false;
    }
    *submitted = std::move(submitted_);
    return true;
  }

  /**
   * @brief Publishes one accepted job to the pre-created worker.
   * @param submitted Accepted queued snapshot transferred without copying.
   * @throws Nothing.
   * @note Publication wakes exactly the existing local worker and does not
   *       submit, resubmit, or mutate daemon state.
   */
  void start(ComputeJobSnapshot submitted) noexcept {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      submitted_ = std::move(submitted);
      ready_ = true;
    }
    changed_.notify_one();
  }

  /**
   * @brief Abandons a not-yet-started local worker and wakes it.
   * @throws Nothing.
   * @note This does not cancel or release any daemon job.
   */
  void cancel() noexcept {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      cancelled_ = true;
    }
    changed_.notify_one();
  }

 private:
  /** @brief Serializes gate publication and cancellation. */
  std::mutex mutex_;

  /** @brief Wakes the one pre-created worker. */
  std::condition_variable changed_;

  /** @brief True after accepted identities are published. */
  bool ready_ = false;

  /** @brief True when local worker startup must end without polling. */
  bool cancelled_ = false;

  /** @brief Accepted queued snapshot moved from the submitting thread. */
  ComputeJobSnapshot submitted_;
};

/**
 * @brief One joined async polling worker and its lifecycle controls.
 * @throws Nothing for default construction; thread creation may throw.
 * @note Records remain adapter-owned until reaped or destructor join.
 */
struct AsyncWorkerRecord {
  /** @brief Joinable worker thread. */
  std::thread thread;

  /** @brief Caller future completion shared with the worker. */
  std::shared_ptr<AsyncCompletion> completion;

  /** @brief Pre-submit worker start/cancel gate. */
  std::shared_ptr<AsyncStartGate> start_gate;

  /** @brief True only after worker cleanup is complete. */
  std::shared_ptr<std::atomic<bool>> finished;
};

/**
 * @brief Marks one worker finished on every normal or exceptional exit.
 * @throws Nothing.
 * @note The marker is declared first inside the thread entry so later catches
 *       and early returns cannot leave a completed thread unreapable.
 */
class WorkerFinishedMarker {
 public:
  /**
   * @brief Borrows one stable shared completion flag.
   * @param finished Worker record flag.
   * @throws Nothing.
   */
  explicit WorkerFinishedMarker(
      std::shared_ptr<std::atomic<bool>> finished) noexcept
      : finished_(std::move(finished)) {}

  /** @brief Publishes worker completion. @throws Nothing. */
  ~WorkerFinishedMarker() { finished_->store(true); }

 private:
  /** @brief Shared record flag outliving thread exit. */
  std::shared_ptr<std::atomic<bool>> finished_;
};

/**
 * @brief Runs one status-mode async job after pre-submit thread creation.
 * @param runtime Shared polling runtime.
 * @param completion Exactly-once caller future completion.
 * @param start_gate Accepted-job publication gate.
 * @param finished Reaping flag set on every exit.
 * @throws Nothing; all worker exceptions are published to the future.
 * @note Terminal status is published before best-effort release, so cleanup
 *       failure or adapter stop cannot replace an already obtained result.
 */
void run_async_status_worker(
    const std::shared_ptr<PollingRuntimeState>& runtime,
    const std::shared_ptr<AsyncCompletion>& completion,
    const std::shared_ptr<AsyncStartGate>& start_gate,
    const std::shared_ptr<std::atomic<bool>>& finished) noexcept {
  WorkerFinishedMarker finished_marker(finished);
  ComputeJobSnapshot submitted;
  std::optional<TerminalCleanupGuard> cleanup;
  try {
    if (!start_gate->wait(&submitted)) {
      return;
    }
    if (runtime->stopped()) {
      return;
    }
    TerminalPollResult terminal = poll_to_terminal(runtime, submitted);
    if (!terminal.status.ok) {
      runtime->complete_worker_status(completion, std::move(terminal.status));
      return;
    }
    IpcResult<ComputeJobSnapshot> result;
    cleanup.emplace(runtime, submitted.compute_id);
    result = tracked_value_call<ComputeJobSnapshot>(
        runtime, [&submitted](Client& client) {
          return client.compute_result(submitted.compute_id);
        });
    if (!result.status.ok) {
      runtime->complete_worker_status(completion, std::move(result.status));
      return;
    }
    OperationStatus validated = validate_terminal_result(
        submitted, terminal.snapshot, result.value, false);
    if (!validated.ok) {
      runtime->complete_worker_status(completion, std::move(validated));
      return;
    }
    runtime->dependencies().before_async_completion();
    runtime->complete_worker_status(completion,
                                    std::move(*result.value.status));
  } catch (...) {
    try {
      runtime->complete_worker_exception(completion, std::current_exception());
    } catch (...) {
    }
  }
}

/**
 * @brief Complete IPC Host ordinary-method adapter.
 *
 * The class stores only an immutable socket path and private polling runtime.
 * Every ordinary override creates one typed short-lived Client. Compute
 * overrides are implemented below through one joined polling lifecycle.
 *
 * @throws std::bad_alloc when immutable or runtime state allocation fails.
 * @note The adapter owns no daemon graph, plugin, scheduler, or job state.
 */
class IpcHost final : public Host {
 public:
  /**
   * @brief Creates one adapter with complete validated dependencies.
   * @param socket_path Absolute daemon socket path retained by value.
   * @param dependencies Clock, waiter, wake, and artifact callbacks.
   * @throws std::invalid_argument if any callback is absent.
   * @throws std::bad_alloc if retained state allocation fails.
   */
  IpcHost(std::string socket_path,
          internal::IpcHostRuntimeDependencies dependencies)
      : socket_path_(socket_path),
        runtime_(std::make_shared<PollingRuntimeState>(
            std::move(socket_path), std::move(dependencies))) {
    const internal::IpcHostRuntimeDependencies& runtime_dependencies =
        runtime_->dependencies();
    if (!runtime_dependencies.monotonic_now ||
        !runtime_dependencies.wait_until ||
        !runtime_dependencies.wake_waiters ||
        !runtime_dependencies.before_async_completion ||
        !runtime_dependencies.consume_artifact) {
      throw std::invalid_argument(
          "IPC Host runtime dependencies must be complete");
    }
  }

  /**
   * @brief Stops and joins all adapter-owned polling workers.
   * @throws Nothing.
   * @note Destruction publishes stop, wakes injected waiters, shuts down active
   *       worker descriptors, completes unfinished futures with preallocated
   *       client_stopped, and joins every worker in that order. It never closes
   *       daemon sessions, releases unfinished jobs, unloads plugins, or falls
   *       back to embedded execution.
   */
  ~IpcHost() override {
    runtime_->stop();
    try {
      runtime_->dependencies().wake_waiters();
    } catch (...) {
    }
    runtime_->interrupt_active_clients();
    for (AsyncWorkerRecord& worker : workers_) {
      worker.start_gate->cancel();
      worker.completion->complete_stopped();
    }
    for (AsyncWorkerRecord& worker : workers_) {
      if (worker.thread.joinable()) {
        worker.thread.join();
      }
    }
  }

  /** @copydoc Host::load_graph */
  Result<GraphSessionId> load_graph(const GraphLoadRequest& request) override {
    IpcResult<GraphSessionSummary> result = call_value<GraphSessionSummary>(
        [&request](Client& client) { return client.load_graph(request); });
    if (!result.status.ok) {
      return {std::move(result.status), {}};
    }
    return {std::move(result.status),
            GraphSessionId{std::move(result.value.session_id.value)}};
  }

  /** @copydoc Host::close_graph */
  VoidResult close_graph(const GraphSessionId& session) override {
    return call_void([&session](Client& client) {
      return client.close_graph(ipc_session(session));
    });
  }

  /** @copydoc Host::list_graphs */
  Result<std::vector<GraphSessionId>> list_graphs() const override {
    IpcResult<std::vector<GraphSessionSummary>> result =
        call_value<std::vector<GraphSessionSummary>>(
            [](Client& client) { return client.list_graphs(); });
    if (!result.status.ok) {
      return {std::move(result.status), {}};
    }
    std::vector<GraphSessionId> sessions;
    sessions.reserve(result.value.size());
    for (GraphSessionSummary& row : result.value) {
      sessions.push_back(GraphSessionId{std::move(row.session_id.value)});
    }
    return {std::move(result.status), std::move(sessions)};
  }

  /** @copydoc Host::reload_graph */
  VoidResult reload_graph(const GraphSessionId& session,
                          const std::string& path) override {
    return call_void([&](Client& client) {
      return client.reload_graph(ipc_session(session), path);
    });
  }

  /** @copydoc Host::save_graph */
  VoidResult save_graph(const GraphSessionId& session,
                        const std::string& path) override {
    return call_void([&](Client& client) {
      return client.save_graph(ipc_session(session), path);
    });
  }

  /** @copydoc Host::clear_graph */
  VoidResult clear_graph(const GraphSessionId& session) override {
    return call_void([&](Client& client) {
      return client.clear_graph(ipc_session(session));
    });
  }

  /** @copydoc Host::compute */
  VoidResult compute(const HostComputeRequest& request) override;

  /** @copydoc Host::compute_async */
  Result<std::future<OperationStatus>> compute_async(
      HostComputeRequest request) override;

  /** @copydoc Host::compute_and_get_image */
  Result<ImageBuffer> compute_and_get_image(
      const HostComputeRequest& request) override;

  /** @copydoc Host::timing */
  Result<TimingSnapshot> timing(const GraphSessionId& session) override {
    return mapped<TimingSnapshot>(
        [&](Client& client) { return client.timing(ipc_session(session)); });
  }

  /** @copydoc Host::last_io_time */
  Result<double> last_io_time(const GraphSessionId& session) const override {
    return mapped<double>([&](Client& client) {
      return client.last_io_time(ipc_session(session));
    });
  }

  /** @copydoc Host::last_error */
  OperationStatus last_error(const GraphSessionId& session) const override {
    IpcResult<OperationStatus> result =
        call_value<OperationStatus>([&](Client& client) {
          return client.last_error(ipc_session(session));
        });
    return result.status.ok ? std::move(result.value)
                            : std::move(result.status);
  }

  /** @copydoc Host::list_node_ids */
  Result<std::vector<NodeId>> list_node_ids(
      const GraphSessionId& session) override {
    return mapped<std::vector<NodeId>>([&](Client& client) {
      return client.list_node_ids(ipc_session(session));
    });
  }

  /** @copydoc Host::ending_nodes */
  Result<std::vector<NodeId>> ending_nodes(
      const GraphSessionId& session) override {
    return mapped<std::vector<NodeId>>([&](Client& client) {
      return client.ending_nodes(ipc_session(session));
    });
  }

  /** @copydoc Host::get_node_yaml */
  Result<std::string> get_node_yaml(const GraphSessionId& session,
                                    NodeId node) override {
    return mapped<std::string>([&](Client& client) {
      return client.get_node_yaml(ipc_session(session), node);
    });
  }

  /** @copydoc Host::set_node_yaml */
  VoidResult set_node_yaml(const GraphSessionId& session, NodeId node,
                           const std::string& yaml) override {
    return call_void([&](Client& client) {
      return client.set_node_yaml(ipc_session(session), node, yaml);
    });
  }

  /** @copydoc Host::inspect_node */
  Result<NodeInspectionView> inspect_node(const GraphSessionId& session,
                                          NodeId node) override {
    return mapped<NodeInspectionView>([&](Client& client) {
      return client.inspect_node(ipc_session(session), node);
    });
  }

  /** @copydoc Host::inspect_graph */
  Result<GraphInspectionView> inspect_graph(
      const GraphSessionId& session) override {
    return mapped<GraphInspectionView>([&](Client& client) {
      return client.inspect_graph(ipc_session(session));
    });
  }

  /** @copydoc Host::dependency_tree */
  Result<HostDependencyTreeSnapshot> dependency_tree(
      const GraphSessionId& session, std::optional<NodeId> node,
      bool include_metadata) override {
    return mapped<HostDependencyTreeSnapshot>([&](Client& client) {
      return client.inspect_dependency_tree(ipc_session(session), node,
                                            include_metadata);
    });
  }

  /** @copydoc Host::traversal_orders */
  Result<std::map<int, std::vector<NodeId>>> traversal_orders(
      const GraphSessionId& session) override {
    return mapped<std::map<int, std::vector<NodeId>>>([&](Client& client) {
      return client.traversal_orders(ipc_session(session));
    });
  }

  /** @copydoc Host::traversal_details */
  Result<std::map<int, std::vector<HostTraversalNodeSnapshot>>>
  traversal_details(const GraphSessionId& session) override {
    using Value = std::map<int, std::vector<HostTraversalNodeSnapshot>>;
    return mapped<Value>([&](Client& client) {
      return client.traversal_details(ipc_session(session));
    });
  }

  /** @copydoc Host::trees_containing_node */
  Result<std::vector<NodeId>> trees_containing_node(
      const GraphSessionId& session, NodeId node) override {
    return mapped<std::vector<NodeId>>([&](Client& client) {
      return client.trees_containing_node(ipc_session(session), node);
    });
  }

  /** @copydoc Host::project_roi */
  Result<PixelRect> project_roi(const GraphSessionId& session,
                                NodeId start_node, const PixelRect& start_roi,
                                NodeId target_node) override {
    return mapped<PixelRect>([&](Client& client) {
      return client.project_roi(ipc_session(session), start_node, start_roi,
                                target_node);
    });
  }

  /** @copydoc Host::project_roi_backward */
  Result<PixelRect> project_roi_backward(const GraphSessionId& session,
                                         NodeId target_node,
                                         const PixelRect& target_roi,
                                         NodeId source_node) override {
    return mapped<PixelRect>([&](Client& client) {
      return client.project_roi_backward(ipc_session(session), target_node,
                                         target_roi, source_node);
    });
  }

  /** @copydoc Host::dirty_region_snapshot */
  Result<DirtyRegionInspectionSnapshot> dirty_region_snapshot(
      const GraphSessionId& session) override {
    return mapped<DirtyRegionInspectionSnapshot>([&](Client& client) {
      return client.dirty_region_snapshot(ipc_session(session));
    });
  }

  /** @copydoc Host::compute_planning_snapshot */
  Result<std::optional<ComputePlanningInspectionSnapshot>>
  compute_planning_snapshot(const GraphSessionId& session) override {
    return mapped<std::optional<ComputePlanningInspectionSnapshot>>(
        [&](Client& client) {
          return client.compute_planning_snapshot(ipc_session(session));
        });
  }

  /** @copydoc Host::recent_compute_planning_snapshots */
  Result<std::vector<ComputePlanningInspectionSnapshot>>
  recent_compute_planning_snapshots(const GraphSessionId& session) override {
    return mapped<std::vector<ComputePlanningInspectionSnapshot>>(
        [&](Client& client) {
          return client.recent_compute_planning_snapshots(ipc_session(session));
        });
  }

  /** @copydoc Host::begin_dirty_source */
  Result<DirtyRegionInspectionSnapshot> begin_dirty_source(
      const GraphSessionId& session, NodeId node, DirtyDomain domain,
      const PixelRect& roi) override {
    return mapped<DirtyRegionInspectionSnapshot>([&](Client& client) {
      return client.begin_dirty_source(ipc_session(session), node, domain, roi);
    });
  }

  /** @copydoc Host::update_dirty_source */
  Result<DirtyRegionInspectionSnapshot> update_dirty_source(
      const GraphSessionId& session, NodeId node, DirtyDomain domain,
      const PixelRect& roi) override {
    return mapped<DirtyRegionInspectionSnapshot>([&](Client& client) {
      return client.update_dirty_source(ipc_session(session), node, domain,
                                        roi);
    });
  }

  /** @copydoc Host::end_dirty_source */
  Result<DirtyRegionInspectionSnapshot> end_dirty_source(
      const GraphSessionId& session, NodeId node, DirtyDomain domain) override {
    return mapped<DirtyRegionInspectionSnapshot>([&](Client& client) {
      return client.end_dirty_source(ipc_session(session), node, domain);
    });
  }

  /** @copydoc Host::drain_compute_events */
  Result<ComputeEventBatch> drain_compute_events(const GraphSessionId& session,
                                                 std::size_t limit) override {
    return mapped<ComputeEventBatch>([&](Client& client) {
      return client.drain_compute_events(ipc_session(session), limit);
    });
  }

  /** @copydoc Host::scheduler_trace */
  Result<SchedulerTracePage> scheduler_trace(const GraphSessionId& session,
                                             std::uint64_t after_sequence,
                                             std::size_t limit) override {
    return mapped<SchedulerTracePage>([&](Client& client) {
      return client.scheduler_trace(ipc_session(session), after_sequence,
                                    limit);
    });
  }

  /** @copydoc Host::clear_cache */
  VoidResult clear_cache(const GraphSessionId& session) override {
    return call_void([&](Client& client) {
      return client.clear_cache(ipc_session(session));
    });
  }

  /** @copydoc Host::clear_drive_cache */
  VoidResult clear_drive_cache(const GraphSessionId& session) override {
    return call_void([&](Client& client) {
      return client.clear_drive_cache(ipc_session(session));
    });
  }

  /** @copydoc Host::clear_memory_cache */
  VoidResult clear_memory_cache(const GraphSessionId& session) override {
    return call_void([&](Client& client) {
      return client.clear_memory_cache(ipc_session(session));
    });
  }

  /** @copydoc Host::cache_all_nodes */
  VoidResult cache_all_nodes(const GraphSessionId& session,
                             const std::string& precision) override {
    return call_void([&](Client& client) {
      return client.cache_all_nodes(ipc_session(session), precision);
    });
  }

  /** @copydoc Host::free_transient_memory */
  VoidResult free_transient_memory(const GraphSessionId& session) override {
    return call_void([&](Client& client) {
      return client.free_transient_memory(ipc_session(session));
    });
  }

  /** @copydoc Host::synchronize_disk_cache */
  VoidResult synchronize_disk_cache(const GraphSessionId& session,
                                    const std::string& precision) override {
    return call_void([&](Client& client) {
      return client.synchronize_disk_cache(ipc_session(session), precision);
    });
  }

  /** @copydoc Host::plugins_load_report */
  Result<HostPluginLoadReport> plugins_load_report(
      const std::vector<std::string>& dirs) override {
    return mapped<HostPluginLoadReport>(
        [&](Client& client) { return client.plugins_load_report(dirs); });
  }

  /** @copydoc Host::plugins_load */
  VoidResult plugins_load(const std::vector<std::string>& dirs) override {
    Result<HostPluginLoadReport> report = plugins_load_report(dirs);
    if (!report.status.ok || report.value.errors.empty()) {
      return {std::move(report.status)};
    }
    HostPluginLoadError& error = report.value.errors.front();
    return {internal::failure_status(
        OperationErrorDomain::Graph, static_cast<std::int32_t>(error.code),
        graph_error_stable_name(error.code), std::move(error.message))};
  }

  /** @copydoc Host::plugins_unload_all */
  Result<int> plugins_unload_all() override {
    return mapped<int>(
        [](Client& client) { return client.plugins_unload_all(); });
  }

  /** @copydoc Host::seed_builtin_ops */
  VoidResult seed_builtin_ops() override {
    return call_void([](Client& client) { return client.seed_builtin_ops(); });
  }

  /** @copydoc Host::ops_sources */
  Result<std::map<std::string, std::string>> ops_sources() const override {
    return mapped<std::map<std::string, std::string>>(
        [](Client& client) { return client.ops_sources(); });
  }

  /** @copydoc Host::ops_combined_keys */
  Result<std::vector<std::string>> ops_combined_keys() const override {
    return mapped<std::vector<std::string>>(
        [](Client& client) { return client.ops_combined_keys(); });
  }

  /** @copydoc Host::ops_combined_sources */
  Result<std::map<std::string, std::string>> ops_combined_sources()
      const override {
    return mapped<std::map<std::string, std::string>>(
        [](Client& client) { return client.ops_combined_sources(); });
  }

  /** @copydoc Host::scheduler_available_types */
  Result<std::vector<std::string>> scheduler_available_types() const override {
    return mapped<std::vector<std::string>>(
        [](Client& client) { return client.scheduler_available_types(); });
  }

  /** @copydoc Host::scheduler_description */
  Result<std::string> scheduler_description(
      const std::string& type) const override {
    return mapped<std::string>(
        [&](Client& client) { return client.scheduler_description(type); });
  }

  /** @copydoc Host::scheduler_scan */
  Result<std::size_t> scheduler_scan(
      const std::vector<std::string>& dirs) override {
    return mapped<std::size_t>(
        [&](Client& client) { return client.scheduler_scan(dirs); });
  }

  /** @copydoc Host::scheduler_load */
  VoidResult scheduler_load(const std::string& path) override {
    return call_void(
        [&](Client& client) { return client.scheduler_load(path); });
  }

  /** @copydoc Host::scheduler_loaded_plugins */
  Result<std::vector<std::string>> scheduler_loaded_plugins() const override {
    return mapped<std::vector<std::string>>(
        [](Client& client) { return client.scheduler_loaded_plugins(); });
  }

  /** @copydoc Host::configure_scheduler_defaults */
  VoidResult configure_scheduler_defaults(
      const HostSchedulerConfig& config) override {
    return call_void([&](Client& client) {
      return client.configure_scheduler_defaults(config);
    });
  }

  /** @copydoc Host::scheduler_info */
  Result<SchedulerInfoSnapshot> scheduler_info(
      const GraphSessionId& session, ComputeIntent intent) const override {
    return mapped<SchedulerInfoSnapshot>([&](Client& client) {
      return client.scheduler_info(ipc_session(session), intent);
    });
  }

  /** @copydoc Host::replace_scheduler */
  VoidResult replace_scheduler(const GraphSessionId& session,
                               ComputeIntent intent,
                               const std::string& type) override {
    return call_void([&](Client& client) {
      return client.replace_scheduler(ipc_session(session), intent, type);
    });
  }

 private:
  /**
   * @brief Invokes one value method through this adapter's socket path.
   * @tparam Value Public payload type.
   * @tparam Call Exact typed Client callable.
   * @param call Callable invoked once after one successful connect.
   * @return Typed IPC result with exact status.
   * @throws std::bad_alloc if transport or value allocation fails.
   */
  template <typename Value, typename Call>
  IpcResult<Value> call_value(Call call) const {
    return short_value_call<Value>(socket_path_, std::move(call));
  }

  /**
   * @brief Invokes one void method through this adapter's socket path.
   * @tparam Call Exact typed Client callable.
   * @param call Callable invoked once after one successful connect.
   * @return Exact void status.
   * @throws std::bad_alloc if transport or status allocation fails.
   */
  template <typename Call>
  VoidResult call_void(Call call) const {
    return short_void_call(socket_path_, std::move(call));
  }

  /**
   * @brief Invokes and converts one same-payload typed Client method.
   * @tparam Value Shared Host/Client public payload type.
   * @tparam Call Exact typed Client callable.
   * @param call Callable invoked once after one successful connect.
   * @return Host-shaped result preserving the exact status.
   * @throws std::bad_alloc if transport or value allocation fails.
   */
  template <typename Value, typename Call>
  Result<Value> mapped(Call call) const {
    return host_result(call_value<Value>(std::move(call)));
  }

  /**
   * @brief Joins and erases workers whose complete cleanup has finished.
   * @throws std::system_error only if an internal join invariant is violated.
   * @note Called before accepting another async request so completed joinable
   *       threads do not accumulate for the entire adapter lifetime.
   */
  void reap_finished_workers() {
    for (auto worker = workers_.begin(); worker != workers_.end();) {
      if (!worker->finished->load()) {
        ++worker;
        continue;
      }
      if (worker->thread.joinable()) {
        worker->thread.join();
      }
      worker = workers_.erase(worker);
    }
  }

  /** @brief Immutable daemon Unix socket path. */
  std::string socket_path_;

  /** @brief Shared stop, descriptor, polling, and artifact runtime. */
  std::shared_ptr<PollingRuntimeState> runtime_;

  /** @brief Adapter-owned joinable async workers awaiting reap/destruction. */
  std::list<AsyncWorkerRecord> workers_;
};

/** @copydoc IpcHost::compute */
VoidResult IpcHost::compute(const HostComputeRequest& request) {
  const ComputeSubmitRequest submit_request =
      compute_submission(request, ComputeResultMode::Status);
  IpcResult<ComputeJobSnapshot> submitted = call_value<ComputeJobSnapshot>(
      [&](Client& client) { return client.submit_compute(submit_request); });
  if (!submitted.status.ok) {
    return {std::move(submitted.status)};
  }

  TerminalPollResult terminal = poll_to_terminal(runtime_, submitted.value);
  if (!terminal.status.ok) {
    return {std::move(terminal.status)};
  }
  IpcResult<ComputeJobSnapshot> result;
  TerminalCleanupGuard cleanup(runtime_, submitted.value.compute_id);
  result = tracked_value_call<ComputeJobSnapshot>(
      runtime_, [&submitted](Client& client) {
        return client.compute_result(submitted.value.compute_id);
      });
  if (!result.status.ok) {
    OperationStatus failure = std::move(result.status);
    return {std::move(failure)};
  }
  OperationStatus validated = validate_terminal_result(
      submitted.value, terminal.snapshot, result.value, false);
  if (!validated.ok) {
    return {std::move(validated)};
  }
  OperationStatus outcome = std::move(*result.value.status);
  return {std::move(outcome)};
}

/** @copydoc IpcHost::compute_async */
Result<std::future<OperationStatus>> IpcHost::compute_async(
    HostComputeRequest request) {
  reap_finished_workers();
  auto completion = std::make_shared<AsyncCompletion>();
  std::future<OperationStatus> future = completion->take_future();
  auto start_gate = std::make_shared<AsyncStartGate>();
  auto finished = std::make_shared<std::atomic<bool>>(false);

  workers_.emplace_back();
  auto worker = std::prev(workers_.end());
  worker->completion = completion;
  worker->start_gate = start_gate;
  worker->finished = finished;
  try {
    worker->thread = std::thread(run_async_status_worker, runtime_, completion,
                                 start_gate, finished);
  } catch (...) {
    workers_.erase(worker);
    throw;
  }

  IpcResult<ComputeJobSnapshot> submitted;
  try {
    const ComputeSubmitRequest submit_request =
        compute_submission(request, ComputeResultMode::Status);
    submitted = call_value<ComputeJobSnapshot>(
        [&](Client& client) { return client.submit_compute(submit_request); });
  } catch (...) {
    start_gate->cancel();
    worker->thread.join();
    workers_.erase(worker);
    throw;
  }
  if (!submitted.status.ok) {
    OperationStatus failure = std::move(submitted.status);
    start_gate->cancel();
    worker->thread.join();
    workers_.erase(worker);
    return {std::move(failure), {}};
  }

  start_gate->start(std::move(submitted.value));
  return {internal::ok_status(), std::move(future)};
}

/** @copydoc IpcHost::compute_and_get_image */
Result<ImageBuffer> IpcHost::compute_and_get_image(
    const HostComputeRequest& request) {
  const ComputeSubmitRequest submit_request =
      compute_submission(request, ComputeResultMode::Image);
  IpcResult<ComputeJobSnapshot> submitted = call_value<ComputeJobSnapshot>(
      [&](Client& client) { return client.submit_compute(submit_request); });
  if (!submitted.status.ok) {
    return {std::move(submitted.status), {}};
  }

  TerminalPollResult terminal = poll_to_terminal(runtime_, submitted.value);
  if (!terminal.status.ok) {
    return {std::move(terminal.status), {}};
  }
  IpcResult<ComputeJobSnapshot> result;
  TerminalCleanupGuard cleanup(runtime_, submitted.value.compute_id);
  result = tracked_value_call<ComputeJobSnapshot>(
      runtime_, [&submitted](Client& client) {
        return client.compute_result(submitted.value.compute_id);
      });
  if (!result.status.ok) {
    OperationStatus failure = std::move(result.status);
    return {std::move(failure), {}};
  }
  OperationStatus validated = validate_terminal_result(
      submitted.value, terminal.snapshot, result.value, true);
  if (!validated.ok) {
    return {std::move(validated), {}};
  }
  if (!result.value.status->ok) {
    OperationStatus outcome = std::move(*result.value.status);
    return {std::move(outcome), {}};
  }
  if (!result.value.output) {
    return {std::move(*result.value.status), {}};
  }

  const DeliveryLeaseId& delivery_id = result.value.output->delivery_id;
  cleanup.set_delivery(delivery_id);
  return runtime_->dependencies().consume_artifact(
      result.value.output->metadata);
}

/**
 * @brief Production condition-variable waiter for steady-clock deadlines.
 * @throws std::system_error if synchronization primitive initialization fails.
 * @note One shared instance wakes every adapter polling worker on destruction.
 */
class DefaultPollingWaiter {
 public:
  /**
   * @brief Waits until one deadline or adapter-stop predicate.
   * @param deadline Absolute steady-clock deadline.
   * @param should_stop Predicate reading shared adapter stop.
   * @return True when stop became observable; false on normal deadline.
   * @throws std::system_error if mutex locking or condition-variable waiting
   *         fails.
   * @throws Whatever the supplied predicate throws.
   * @note Spurious wakes recheck both stop and the same absolute deadline.
   */
  bool wait_until(std::chrono::steady_clock::time_point deadline,
                  const std::function<bool()>& should_stop) {
    std::unique_lock<std::mutex> lock(mutex_);
    while (!should_stop()) {
      if (changed_.wait_until(lock, deadline) == std::cv_status::timeout) {
        return should_stop();
      }
    }
    return true;
  }

  /**
   * @brief Wakes all current deadline waits after stop publication.
   * @throws std::system_error if mutex locking fails.
   */
  void wake_all() {
    std::lock_guard<std::mutex> lock(mutex_);
    changed_.notify_all();
  }

 private:
  /** @brief Serializes condition-variable waiting only. */
  std::mutex mutex_;

  /** @brief Shared wake channel for every polling worker. */
  std::condition_variable changed_;
};

/**
 * @brief Returns scalar channel bytes without linking the embedded product.
 * @param type Public artifact data type.
 * @return Exact bytes for each current DataType value, or zero if unknown.
 * @throws Nothing.
 * @note This private switch keeps the installed IPC static archive independent
 *       from `image_buffer.cpp` and all backend libraries.
 */
std::size_t artifact_channel_bytes(DataType type) noexcept {
  switch (type) {
    case DataType::UINT8:
    case DataType::INT8:
      return 1;
    case DataType::UINT16:
    case DataType::INT16:
      return 2;
    case DataType::FLOAT32:
      return 4;
    case DataType::FLOAT64:
      return 8;
  }
  return 0;
}

/**
 * @brief Validates one opened artifact descriptor against daemon metadata.
 * @param metadata Strictly decoded output metadata.
 * @param file Current `fstat` snapshot of the opened descriptor.
 * @return True only for same-user regular exact-0600 one-link identity/size.
 * @throws Nothing.
 * @note Both pre-map and post-map validation use this same predicate.
 */
bool artifact_file_matches(const OutputArtifactMetadata& metadata,
                           const struct stat& file) noexcept {
  return S_ISREG(file.st_mode) && file.st_uid == ::geteuid() &&
         (file.st_mode & 07777) == 0600 && file.st_nlink == 1 &&
         static_cast<std::uint64_t>(file.st_dev) ==
             metadata.filesystem_device &&
         static_cast<std::uint64_t>(file.st_ino) == metadata.inode &&
         file.st_size >= 0 &&
         static_cast<std::uint64_t>(file.st_size) == metadata.byte_size;
}

/**
 * @brief Builds a local artifact IO failure with bounded system diagnostics.
 * @param operation Stable operation label.
 * @param error_number Captured errno value.
 * @return Transport read_failed status.
 * @throws std::bad_alloc if diagnostic storage cannot allocate.
 */
OperationStatus artifact_io_failure(const char* operation, int error_number) {
  return internal::failure_status(
      OperationErrorDomain::Transport, 4, "read_failed",
      std::string(operation) + ": " + std::strerror(error_number));
}

/**
 * @brief Maps one descriptor read-only from offset zero.
 * @param fd Open validated artifact descriptor.
 * @param length Exact nonzero artifact byte count.
 * @return Mapping address, or `MAP_FAILED` with `errno` set.
 * @throws Nothing.
 */
void* map_artifact_read_only(int fd, std::size_t length) noexcept {
  return ::mmap(nullptr, length, PROT_READ, MAP_PRIVATE, fd, 0);
}

/**
 * @brief Unmaps one complete artifact mapping.
 * @param address Exact mapping base address.
 * @param length Exact mapped byte count.
 * @return POSIX `munmap` result.
 * @throws Nothing.
 */
int unmap_artifact(void* address, std::size_t length) noexcept {
  return ::munmap(address, length);
}

/**
 * @brief Closes one mapping-owned artifact descriptor.
 * @param fd Exact descriptor transferred from `UniqueFd`.
 * @return POSIX `close` result.
 * @throws Nothing.
 */
int close_artifact_descriptor(int fd) noexcept {
  return ::close(fd);
}

/**
 * @brief Returns the production mapping operation table.
 * @return Direct read-only mmap, munmap, and close wrappers.
 * @throws Nothing.
 */
internal::ArtifactMappingOperations
default_artifact_mapping_operations() noexcept {
  return {map_artifact_read_only, unmap_artifact, close_artifact_descriptor};
}

/**
 * @brief Owns the final unmap and descriptor close for one shared mapping.
 * @throws Nothing for construction, copy, move, and invocation.
 * @note `std::shared_ptr` invokes one retained copy exactly once after the last
 *       image reference disappears, including control-block allocation
 *       failure during initial ownership construction.
 */
class ArtifactMappingDeleter {
 public:
  /**
   * @brief Captures one complete mapping lifetime.
   * @param length Exact mapped byte count.
   * @param fd Exact descriptor transferred into shared ownership.
   * @param operations Nonthrowing unmap/close operations.
   * @throws Nothing.
   */
  ArtifactMappingDeleter(
      std::size_t length, int fd,
      internal::ArtifactMappingOperations operations) noexcept
      : length_(length), fd_(fd), operations_(operations) {}

  /**
   * @brief Releases the mapping and then closes its retained descriptor.
   * @param address Exact mapping base address.
   * @return Nothing.
   * @throws Nothing; cleanup errors are intentionally ignored at final-owner
   *         destruction where no error channel exists.
   */
  void operator()(void* address) const noexcept {
    (void)operations_.unmap(address, length_);
    (void)operations_.close_fd(fd_);
  }

 private:
  /** @brief Exact mapped byte count. */
  std::size_t length_ = 0;

  /** @brief Descriptor retained until final mapping release. */
  int fd_ = -1;

  /** @brief Nonthrowing final-owner cleanup operations. */
  internal::ArtifactMappingOperations operations_;
};

/**
 * @brief Strictly validates and maps one lease-protected artifact.
 * @param metadata Daemon output metadata already validated by typed Client.
 * @param operations Complete nonthrowing mapping operation table.
 * @return Shared read-only mapped CPU ImageBuffer or exact local failure.
 * @throws std::bad_alloc if failure-status diagnostics or shared mapping
 *         ownership cannot allocate.
 * @note `O_NONBLOCK` prevents a same-uid path replacement with a FIFO from
 *       blocking before `fstat`; non-regular descriptors are then rejected.
 *       The delivery lease remains active throughout open, validation, mmap,
 *       and final descriptor revalidation. The returned shared owner unmaps
 *       and closes exactly once after its last copy is destroyed.
 */
Result<ImageBuffer> consume_artifact_readonly_mapping_impl(
    const OutputArtifactMetadata& metadata,
    internal::ArtifactMappingOperations operations) {
  if (operations.map_read_only == nullptr || operations.unmap == nullptr ||
      operations.close_fd == nullptr) {
    return {
        invalid_adapter_response("artifact mapping operations are incomplete"),
        {}};
  }
  if (metadata.path.empty() || metadata.path.front() != '/' ||
      metadata.path.find('\0') != std::string::npos || metadata.width <= 0 ||
      metadata.height <= 0 || metadata.channels <= 0 ||
      metadata.device != Device::CPU || metadata.byte_size == 0 ||
      metadata.byte_size > internal::kOutputArtifactMaxBytes) {
    return {invalid_adapter_response(
                "artifact metadata cannot describe a protected CPU file"),
            {}};
  }
  const std::size_t scalar_bytes = artifact_channel_bytes(metadata.data_type);
  const std::size_t width = static_cast<std::size_t>(metadata.width);
  const std::size_t height = static_cast<std::size_t>(metadata.height);
  const std::size_t channels = static_cast<std::size_t>(metadata.channels);
  const std::size_t maximum = std::numeric_limits<std::size_t>::max();
  if (scalar_bytes == 0 || width > maximum / channels ||
      width * channels > maximum / scalar_bytes) {
    return {invalid_adapter_response("artifact row layout overflows"), {}};
  }
  const std::size_t expected_step = width * channels * scalar_bytes;
  if (height > maximum / expected_step || metadata.row_step != expected_step ||
      metadata.byte_size != expected_step * height) {
    return {invalid_adapter_response("artifact byte layout is inconsistent"),
            {}};
  }
  internal::UniqueFd file(::open(
      metadata.path.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK));
  if (!file) {
    const int error_number = errno;
    return {artifact_io_failure("artifact open failed", error_number), {}};
  }
  struct stat before{};
  if (::fstat(file.get(), &before) != 0) {
    const int error_number = errno;
    return {artifact_io_failure("artifact fstat failed", error_number), {}};
  }
  if (!artifact_file_matches(metadata, before)) {
    return {invalid_adapter_response(
                "artifact identity, owner, mode, link, or size mismatched"),
            {}};
  }

  const std::size_t mapped_size = static_cast<std::size_t>(metadata.byte_size);
  void* mapping = operations.map_read_only(file.get(), mapped_size);
  if (mapping == MAP_FAILED) {
    const int error_number = errno;
    return {artifact_io_failure("artifact mmap failed", error_number), {}};
  }

  struct stat after{};
  if (::fstat(file.get(), &after) != 0) {
    const int error_number = errno;
    (void)operations.unmap(mapping, mapped_size);
    return {artifact_io_failure("artifact post-map fstat failed", error_number),
            {}};
  }
  if (!artifact_file_matches(metadata, after)) {
    (void)operations.unmap(mapping, mapped_size);
    return {invalid_adapter_response(
                "artifact changed during protected read-only mapping"),
            {}};
  }

  const int mapped_fd = file.release();
  std::shared_ptr<void> storage(
      mapping, ArtifactMappingDeleter(mapped_size, mapped_fd, operations));

  ImageBuffer image;
  image.width = metadata.width;
  image.height = metadata.height;
  image.channels = metadata.channels;
  image.type = metadata.data_type;
  image.device = Device::CPU;
  image.step = metadata.row_step;
  image.data = std::move(storage);
  image.context.reset();
  return {internal::ok_status(), std::move(image)};
}

/**
 * @brief Creates the production steady-clock/waiter/artifact callback bundle.
 * @return Complete private runtime dependencies.
 * @throws std::bad_alloc if shared waiter or callback allocation fails.
 * @throws std::system_error if production waiter synchronization primitives
 *         cannot be initialized.
 */
internal::IpcHostRuntimeDependencies default_runtime_dependencies() {
  auto waiter = std::make_shared<DefaultPollingWaiter>();
  internal::IpcHostRuntimeDependencies dependencies;
  dependencies.monotonic_now = [] { return std::chrono::steady_clock::now(); };
  dependencies.wait_until = [waiter](
                                std::chrono::steady_clock::time_point deadline,
                                const std::function<bool()>& should_stop) {
    return waiter->wait_until(deadline, should_stop);
  };
  dependencies.wake_waiters = [waiter] { waiter->wake_all(); };
  dependencies.before_async_completion = [] {};
  dependencies.consume_artifact = [](const OutputArtifactMetadata& metadata) {
    return internal::consume_artifact_readonly_mapping(
        metadata, default_artifact_mapping_operations());
  };
  return dependencies;
}

}  // namespace

namespace internal {

/** @copydoc consume_artifact_readonly_mapping */
Result<ImageBuffer> consume_artifact_readonly_mapping(
    const OutputArtifactMetadata& metadata,
    ArtifactMappingOperations operations) {
  return consume_artifact_readonly_mapping_impl(metadata, operations);
}

/** @copydoc create_ipc_host_with_dependencies */
std::unique_ptr<Host> create_ipc_host_with_dependencies(
    const std::string& socket_path, IpcHostRuntimeDependencies dependencies) {
  return std::make_unique<IpcHost>(socket_path, std::move(dependencies));
}

}  // namespace internal

/** @copydoc create_ipc_host */
std::unique_ptr<Host> create_ipc_host(const std::string& socket_path) {
  return internal::create_ipc_host_with_dependencies(
      socket_path, default_runtime_dependencies());
}

}  // namespace ps::ipc
