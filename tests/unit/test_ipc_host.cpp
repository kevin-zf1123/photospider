#include <fcntl.h>
#include <gtest/gtest.h>
#include <poll.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <functional>
#include <future>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include "ipc/codec.hpp"
#include "ipc/frame.hpp"
#include "ipc/ipc_host_runtime.hpp"
#include "ipc/server.hpp"
#include "ipc/unix_socket.hpp"
#include "photospider/ipc/client.hpp"
#include "photospider/ipc/host.hpp"
#include "support/ipc_host_spy.hpp"

namespace ps::ipc::internal {
namespace {

/**
 * @brief Owns one protected temporary directory for IPC Host tests.
 * @throws std::filesystem::filesystem_error if setup or permissions fail.
 * @note Destruction removes sockets, lock files, and output directories
 *       best-effort after their server owner has stopped.
 */
class ScopedTempDirectory {
 public:
  /**
   * @brief Creates one unique mode-0700 temporary directory.
   * @param label Short test-specific path prefix.
   * @throws std::filesystem::filesystem_error if setup fails.
   * @throws std::system_error if chmod fails.
   */
  explicit ScopedTempDirectory(const std::string& label)
      : path_(std::filesystem::temp_directory_path() /
              (label + "-" + std::to_string(::getpid()) + "-" +
               std::to_string(sequence_++))) {
    std::filesystem::remove_all(path_);
    std::filesystem::create_directories(path_);
    if (::chmod(path_.c_str(), 0700) != 0) {
      throw std::system_error(errno, std::generic_category(), "chmod");
    }
  }

  /**
   * @brief Prevents duplicate temporary-tree cleanup ownership.
   * @throws Nothing because this operation is unavailable.
   */
  ScopedTempDirectory(const ScopedTempDirectory&) = delete;

  /**
   * @brief Prevents replacing temporary-tree ownership by copy.
   * @return No value because this operation is unavailable.
   * @throws Nothing because this operation is unavailable.
   */
  ScopedTempDirectory& operator=(const ScopedTempDirectory&) = delete;

  /** @brief Removes the temporary tree best-effort. @throws Nothing. */
  ~ScopedTempDirectory() {
    std::error_code ignored;
    std::filesystem::remove_all(path_, ignored);
  }

  /**
   * @brief Returns the owned directory path.
   * @return Borrowed path valid for this helper lifetime.
   * @throws Nothing.
   */
  const std::filesystem::path& path() const noexcept { return path_; }

 private:
  /** @brief Unique protected directory path. */
  std::filesystem::path path_;

  /** @brief Per-process suffix source. */
  static inline std::size_t sequence_ = 0;
};

/**
 * @brief Runs the real internal Server around one test Host on a worker thread.
 * @throws std::runtime_error or std::system_error when pipe, server, thread, or
 *         bounded readiness setup fails.
 * @note Stop uses the production self-pipe path and joins the foreground run;
 *       no server worker is detached.
 */
class RunningServer {
 public:
  /**
   * @brief Starts one real Server and waits for typed ping readiness.
   * @param host Host borrowed until this helper is destroyed.
   * @param socket_path Absolute socket path below a protected directory.
   * @throws std::runtime_error if readiness is not reached within two seconds.
   * @throws std::system_error if pipe or thread creation fails.
   */
  RunningServer(Host& host, std::string socket_path)
      : socket_path_(std::move(socket_path)),
        server_(std::make_unique<Server>(host, "ipc-host-test")) {
    int pipe_fds[2] = {-1, -1};
    if (::pipe(pipe_fds) != 0) {
      throw std::system_error(errno, std::generic_category(), "pipe");
    }
    stop_read_.reset(pipe_fds[0]);
    stop_write_.reset(pipe_fds[1]);
    run_thread_ = std::thread([this] {
      status_ = server_->run(ServerOptions{socket_path_}, stop_read_.get());
    });

    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (std::chrono::steady_clock::now() < deadline) {
      Client client;
      if (client.connect(socket_path_).ok) {
        const IpcResult<DaemonPing> ping = client.ping();
        if (ping.status.ok) {
          return;
        }
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    (void)stop();
    throw std::runtime_error("IPC Host test server readiness timed out");
  }

  /**
   * @brief Prevents sharing one server thread and stop pipe.
   * @throws Nothing because this operation is unavailable.
   */
  RunningServer(const RunningServer&) = delete;

  /**
   * @brief Prevents replacing server lifecycle ownership by copy.
   * @return No value because this operation is unavailable.
   * @throws Nothing because this operation is unavailable.
   */
  RunningServer& operator=(const RunningServer&) = delete;

  /**
   * @brief Stops and joins the server best-effort.
   * @throws Nothing; stop-result copying is contained during destruction.
   */
  ~RunningServer() noexcept {
    try {
      (void)stop();
    } catch (...) {
    }
  }

  /**
   * @brief Signals the real stop pipe and joins the foreground server.
   * @return Final Server status, or the already recorded status on repetition.
   * @throws std::bad_alloc if copying the final diagnostic cannot allocate.
   * @throws std::system_error if the owned server thread cannot be joined;
   *         stop write failure remains only in the returned test status.
   */
  OperationStatus stop() {
    if (!stopped_) {
      stopped_ = true;
      const char marker = 'x';
      (void)::write(stop_write_.get(), &marker, 1);
      if (run_thread_.joinable()) {
        run_thread_.join();
      }
    }
    return status_;
  }

 private:
  /** @brief Absolute server socket path. */
  std::string socket_path_;

  /** @brief Real internal Server ownership. */
  std::unique_ptr<Server> server_;

  /** @brief Stop self-pipe read end borrowed by Server::run. */
  UniqueFd stop_read_;

  /** @brief Stop self-pipe write end owned by the test controller. */
  UniqueFd stop_write_;

  /** @brief Foreground Server::run thread. */
  std::thread run_thread_;

  /** @brief Final Server completion status. */
  OperationStatus status_;

  /** @brief Idempotent stop flag. */
  bool stopped_ = false;
};

/**
 * @brief One expected short-connection request and correlated scripted reply.
 * @throws std::bad_alloc when method or JSON payload storage allocates.
 * @note `error=true` publishes the payload as the structured error branch.
 *       An optional hook runs after request capture and before reply framing.
 */
struct ShortConnectionReply {
  /**
   * @brief Builds one expected request/reply step with an optional hook.
   * @param expected_method Exact method required from the accepted request.
   * @param response_payload Typed result or structured-error payload.
   * @param structured_error Whether to use the error response branch.
   * @param hook Optional action after request capture and before reply framing.
   * @throws std::bad_alloc if owned method, JSON, or hook storage allocates.
   */
  ShortConnectionReply(std::string expected_method, Json response_payload,
                       bool structured_error = false,
                       std::function<bool(const Json& request)> hook = {})
      : method(std::move(expected_method)),
        payload(std::move(response_payload)),
        error(structured_error),
        before_reply(std::move(hook)) {}

  /** @brief Exact method required from the next accepted connection. */
  std::string method;

  /** @brief Typed result object or structured remote error object. */
  Json payload;

  /** @brief Whether payload is returned under `error` instead of `result`. */
  bool error = false;

  /**
   * @brief Optional request-aware action immediately before reply framing.
   * @param request Parsed matching request retained in the request log.
   * @return True to continue; false to fail the scripted peer.
   * @throws Whatever an injected hook throws; the serving boundary converts
   *         it to a false result.
   */
  std::function<bool(const Json& request)> before_reply;
};

/**
 * @brief Reads and validates one framed request from a scripted peer socket.
 * @param peer Connected peer descriptor.
 * @param expected_method Exact method required in the request.
 * @param request Receives the parsed request object.
 * @return True only for one complete request of the expected method.
 * @throws Nothing; frame, JSON, validation, and allocation failures return
 *         false.
 */
bool read_expected_request(int peer, const std::string& expected_method,
                           Json* request) noexcept {
  if (request == nullptr) {
    return false;
  }
  try {
    const FrameReadResult frame = read_frame(peer);
    if (frame.state != FrameReadState::Complete) {
      return false;
    }
    JsonParseResult parsed = parse_json(frame.payload);
    if (!parsed.ok || !parsed.value.is_object() ||
        !parsed.value.value("id", Json()).is_string() ||
        parsed.value.value("method", Json()) != Json(expected_method)) {
      return false;
    }
    *request = std::move(parsed.value);
    return true;
  } catch (...) {
    return false;
  }
}

/**
 * @brief Writes one success or structured-error reply correlated to a request.
 * @param peer Connected peer descriptor.
 * @param request Previously validated request containing a string id.
 * @param payload Typed result object or structured remote error object.
 * @param error Whether to publish `payload` under the error branch.
 * @return True when the complete response frame is written.
 * @throws Nothing; JSON, framing, and allocation failures return false.
 */
bool write_correlated_reply(int peer, const Json& request, const Json& payload,
                            bool error = false) noexcept {
  try {
    Json response{{"protocol_version", kProtocolVersion},
                  {"id", request["id"]}};
    response[error ? "error" : "result"] = payload;
    return write_frame(peer, response.dump()).ok;
  } catch (...) {
    return false;
  }
}

/**
 * @brief Watches a live listener for any forbidden follow-up connection.
 * @param listener Bound listener kept open through the lifecycle under test.
 * @param stop Atomic controller flag ending the watch.
 * @param extra_connection Receives true if any connection is accepted.
 * @return True unless poll/accept fails before requested stop.
 * @throws Nothing.
 * @note Accepted unexpected peers are closed immediately so buggy adapter code
 *       cannot make test teardown block waiting for an unscripted response.
 */
bool watch_for_extra_connection(int listener, const std::atomic<bool>* stop,
                                bool* extra_connection) noexcept {
  if (stop == nullptr || extra_connection == nullptr) {
    return false;
  }
  while (!stop->load()) {
    pollfd descriptor{listener, POLLIN, 0};
    const int poll_result = ::poll(&descriptor, 1, 10);
    if (poll_result < 0 && errno == EINTR) {
      continue;
    }
    if (poll_result < 0) {
      return false;
    }
    if (poll_result == 0 || (descriptor.revents & POLLIN) == 0) {
      continue;
    }
    UniqueFd unexpected(::accept(listener, nullptr, nullptr));
    if (!unexpected) {
      return stop->load();
    }
    *extra_connection = true;
  }
  return true;
}

/**
 * @brief Creates one local listener for short-connection scripted peers.
 * @param socket_path Absolute socket path to bind.
 * @return Owned listening descriptor.
 * @throws std::runtime_error if socket configuration, bind, or listen fails.
 * @throws std::bad_alloc if diagnostics allocate.
 * @note The protected parent directory is owned by ScopedTempDirectory.
 */
UniqueFd create_test_listener(const std::string& socket_path) {
  UniqueFd listener(::socket(AF_UNIX, SOCK_STREAM, 0));
  if (!listener) {
    throw std::runtime_error("test listener socket failed");
  }
  std::string message;
  if (!configure_no_sigpipe(listener.get(), &message)) {
    throw std::runtime_error(message);
  }
  sockaddr_un address{};
  address.sun_family = AF_UNIX;
  if (socket_path.size() + 1U > sizeof(address.sun_path)) {
    throw std::runtime_error("test listener path exceeds sun_path");
  }
  std::memcpy(address.sun_path, socket_path.c_str(), socket_path.size() + 1U);
  const socklen_t length = static_cast<socklen_t>(
      offsetof(sockaddr_un, sun_path) + socket_path.size() + 1U);
  if (::bind(listener.get(), reinterpret_cast<sockaddr*>(&address), length) !=
          0 ||
      ::listen(listener.get(), 32) != 0) {
    throw std::runtime_error("test listener bind/listen failed");
  }
  return listener;
}

/**
 * @brief Serves one correlated reply per independently accepted Client.
 * @param listener Bound local listener descriptor.
 * @param replies Exact method/result script.
 * @param requests Receives every parsed request in acceptance order.
 * @return True only when the complete script is consumed successfully.
 * @throws Nothing; accept/frame/JSON/allocation failures return false.
 * @note This matches IPC Host short-lived connection ownership rather than the
 *       direct Client's multi-call connection tests.
 */
bool serve_short_connection_replies(
    int listener, const std::vector<ShortConnectionReply>& replies,
    std::vector<Json>* requests) noexcept {
  if (requests == nullptr) {
    return false;
  }
  try {
    for (const ShortConnectionReply& reply : replies) {
      UniqueFd peer(::accept(listener, nullptr, nullptr));
      if (!peer) {
        return false;
      }
      Json request;
      if (!read_expected_request(peer.get(), reply.method, &request)) {
        return false;
      }
      requests->push_back(request);
      if (reply.before_reply && !reply.before_reply(request)) {
        return false;
      }
      if (!write_correlated_reply(peer.get(), request, reply.payload,
                                  reply.error)) {
        return false;
      }
    }
    return true;
  } catch (...) {
    return false;
  }
}

/**
 * @brief Builds one complete typed compute-job response object.
 * @param compute_id Opaque accepted job id.
 * @param session_id Opaque submitted session id.
 * @param state Exact wire lifecycle state.
 * @param status Null for nonterminal or exact nested terminal status.
 * @param output Null or typed output delivery object.
 * @return Complete six-field job result.
 * @throws std::bad_alloc if JSON or string storage allocates.
 */
Json compute_job_result(const std::string& compute_id,
                        const std::string& session_id, const std::string& state,
                        const Json& status, const Json& output = nullptr) {
  return Json{{"compute_id", compute_id}, {"session_id", session_id},
              {"state", state},           {"cancellable", false},
              {"status", status},         {"output", output}};
}

/**
 * @brief Returns one successful empty image for status-only runtime tests.
 * @param metadata Unused artifact metadata.
 * @return Canonical empty ImageBuffer success.
 * @throws Nothing.
 * @note Status-only tests never invoke this callback; a concrete function keeps
 *       the injected callback type explicit for every supported compiler.
 */
::ps::Result<ImageBuffer> consume_empty_test_artifact(
    const ::ps::ipc::OutputArtifactMetadata& metadata) {
  (void)metadata;
  return {OperationStatus{}, ImageBuffer{}};
}

/**
 * @brief Injectable monotonic clock and zero-sleep deadline recorder.
 * @throws std::bad_alloc when callback or recorded-deadline storage grows.
 * @note The object must outlive every Host constructed from `dependencies()`;
 *       all mutable state is mutex-protected for concurrent async workers.
 */
class RecordingPollingRuntime {
 public:
  /** @brief Artifact callback type retained by the injected runtime. */
  using ArtifactConsumer = std::function<Result<ImageBuffer>(
      const ::ps::ipc::OutputArtifactMetadata&)>;

  /**
   * @brief Stores immutable completion and artifact callbacks.
   * @param before_async_completion Hook called before async outcome publish.
   * @param consume_artifact Artifact consumer used by image-mode requests.
   * @throws std::bad_alloc if callback transfer allocates.
   * @note Callback targets must synchronize their own shared mutable state.
   */
  explicit RecordingPollingRuntime(
      std::function<void()> before_async_completion = [] {},
      ArtifactConsumer consume_artifact = consume_empty_test_artifact)
      : before_async_completion_(std::move(before_async_completion)),
        consume_artifact_(std::move(consume_artifact)) {}

  /**
   * @brief Creates a complete runtime bundle using this synchronized state.
   * @return Clock/wait/wake and retained completion/artifact callbacks.
   * @throws std::bad_alloc if callback allocation fails.
   */
  IpcHostRuntimeDependencies dependencies() {
    IpcHostRuntimeDependencies dependencies;
    dependencies.monotonic_now = [this] {
      std::lock_guard<std::mutex> lock(mutex_);
      return now_;
    };
    dependencies.wait_until =
        [this](std::chrono::steady_clock::time_point deadline,
               const std::function<bool()>& should_stop) {
          {
            std::lock_guard<std::mutex> lock(mutex_);
            waits_.push_back(
                std::chrono::duration_cast<std::chrono::milliseconds>(deadline -
                                                                      now_));
            now_ = deadline;
          }
          return should_stop();
        };
    dependencies.wake_waiters = [this] {
      std::lock_guard<std::mutex> lock(mutex_);
      ++wake_count_;
    };
    dependencies.before_async_completion = [this] {
      before_async_completion_();
    };
    dependencies.consume_artifact =
        [this](const ::ps::ipc::OutputArtifactMetadata& value) {
          return consume_artifact_(value);
        };
    return dependencies;
  }

  /**
   * @brief Returns recorded wait durations.
   * @return Owned wait sequence in call order.
   * @throws std::bad_alloc if vector copying fails.
   */
  std::vector<std::chrono::milliseconds> waits() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return waits_;
  }

  /**
   * @brief Returns the number of destructor wake broadcasts.
   * @return Synchronized wake callback count.
   * @throws Nothing.
   */
  std::size_t wake_count() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return wake_count_;
  }

 private:
  /** @brief Serializes clock, waits, and wake count. */
  mutable std::mutex mutex_;

  /** @brief Injected monotonic timestamp advanced by each fake wait. */
  std::chrono::steady_clock::time_point now_{};

  /** @brief Exact requested delay sequence. */
  std::vector<std::chrono::milliseconds> waits_;

  /** @brief Number of adapter destruction wake calls. */
  std::size_t wake_count_ = 0;

  /** @brief Immutable hook invoked immediately before async publication. */
  std::function<void()> before_async_completion_;

  /** @brief Immutable image artifact consumer. */
  ArtifactConsumer consume_artifact_;
};

/**
 * @brief Injectable waiter that blocks until adapter destruction wakes it.
 * @throws Nothing for construction; callback allocation may throw when the
 *         dependency bundle is copied.
 * @note A two-second fallback keeps lifecycle regressions bounded while the
 *       normal path proves the wake callback releases a real blocked wait.
 */
class BlockingPollingRuntime {
 public:
  /**
   * @brief Creates the synchronized clock/wait/wake dependency bundle.
   * @return Complete dependencies with a blocking waiter and empty artifact
   *         consumer.
   * @throws std::bad_alloc if callback storage allocation fails.
   */
  IpcHostRuntimeDependencies dependencies() {
    IpcHostRuntimeDependencies dependencies;
    dependencies.monotonic_now = [] {
      return std::chrono::steady_clock::time_point{};
    };
    dependencies.wait_until =
        [this](std::chrono::steady_clock::time_point deadline,
               const std::function<bool()>& should_stop) {
          (void)deadline;
          std::unique_lock<std::mutex> lock(mutex_);
          waiting_ = true;
          changed_.notify_all();
          const bool stopped =
              changed_.wait_for(lock, std::chrono::seconds(2), should_stop);
          return stopped || should_stop();
        };
    dependencies.wake_waiters = [this] {
      {
        std::lock_guard<std::mutex> lock(mutex_);
        ++wake_count_;
      }
      changed_.notify_all();
    };
    dependencies.before_async_completion = [] {};
    dependencies.consume_artifact = consume_empty_test_artifact;
    return dependencies;
  }

  /**
   * @brief Waits until the worker is actually blocked in `wait_until`.
   * @param timeout Maximum controller wait.
   * @return True when blocking began before the deadline.
   * @throws Nothing.
   */
  bool wait_until_blocked(std::chrono::milliseconds timeout) noexcept {
    std::unique_lock<std::mutex> lock(mutex_);
    return changed_.wait_for(lock, timeout, [this] { return waiting_; });
  }

  /**
   * @brief Returns the synchronized wake callback count.
   * @return Number of destruction wake broadcasts.
   * @throws Nothing.
   */
  std::size_t wake_count() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return wake_count_;
  }

 private:
  /** @brief Serializes wait entry and wake counting. */
  mutable std::mutex mutex_;

  /** @brief Coordinates worker wait and controller observation. */
  std::condition_variable changed_;

  /** @brief True after the worker entered the injected wait. */
  bool waiting_ = false;

  /** @brief Number of adapter wake broadcasts. */
  std::size_t wake_count_ = 0;
};

/**
 * @brief Deterministically blocks one async worker at its completion hook.
 * @throws Nothing for construction; waiting uses only synchronized state.
 * @note The test controller must call `release()` before destruction.
 */
class AsyncCompletionGate {
 public:
  /**
   * @brief Marks the hook entered and blocks until released.
   * @return Nothing.
   * @throws Nothing.
   */
  void block() noexcept {
    std::unique_lock<std::mutex> lock(mutex_);
    entered_ = true;
    changed_.notify_all();
    changed_.wait(lock, [this] { return released_; });
  }

  /**
   * @brief Waits a bounded interval for the worker to enter the hook.
   * @param timeout Maximum wall-clock wait used only by the test controller.
   * @return True when the worker reached `block()` before the deadline.
   * @throws Nothing.
   */
  bool wait_until_entered(std::chrono::milliseconds timeout) noexcept {
    std::unique_lock<std::mutex> lock(mutex_);
    return changed_.wait_for(lock, timeout, [this] { return entered_; });
  }

  /**
   * @brief Releases the blocked worker idempotently.
   * @return Nothing.
   * @throws Nothing.
   */
  void release() noexcept {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      released_ = true;
    }
    changed_.notify_all();
  }

 private:
  /** @brief Serializes hook entry and release. */
  std::mutex mutex_;

  /** @brief Wakes controller and worker state transitions. */
  std::condition_variable changed_;

  /** @brief True after the worker reached the hook. */
  bool entered_ = false;

  /** @brief True after the controller permits worker completion. */
  bool released_ = false;
};

/**
 * @brief Releases an async completion gate before a later-declared Host dies.
 * @throws Nothing.
 * @note Declare this guard immediately after the Host owner so assertion or
 *       exception unwinding cannot make Host destruction join a gated worker.
 */
class AsyncCompletionGateReleaseGuard {
 public:
  /**
   * @brief Borrows the gate that must outlive this guard.
   * @param gate Gate released during destruction.
   * @throws Nothing.
   */
  explicit AsyncCompletionGateReleaseGuard(AsyncCompletionGate& gate) noexcept
      : gate_(&gate) {}

  /** @brief Releases the borrowed gate idempotently. @throws Nothing. */
  ~AsyncCompletionGateReleaseGuard() { gate_->release(); }

  /**
   * @brief Prevents duplicate release-guard ownership.
   * @throws Nothing because this operation is unavailable.
   */
  AsyncCompletionGateReleaseGuard(const AsyncCompletionGateReleaseGuard&) =
      delete;

  /**
   * @brief Prevents replacing release-guard ownership by copy.
   * @return No value because this operation is unavailable.
   * @throws Nothing because this operation is unavailable.
   */
  AsyncCompletionGateReleaseGuard& operator=(
      const AsyncCompletionGateReleaseGuard&) = delete;

 private:
  /** @brief Borrowed gate released once on scope exit. */
  AsyncCompletionGate* gate_;
};

/**
 * @brief Exercises all 53 Host virtuals through the real typed IPC stack.
 *
 * The test loads one opaque session, invokes every graph, compute, diagnostic,
 * inspection, dirty, observation, cache, plugin, and scheduler operation, and
 * compares the backend spy's exact Host-entry multiset. Compute and plugin
 * conveniences intentionally share their documented wire/Host primitive.
 *
 * @return Nothing; GoogleTest assertions report dispatch or status mismatch.
 * @throws std::bad_alloc, std::runtime_error, std::system_error, or
 *         std::future_error if deterministic test setup cannot complete.
 * @note The public factory performs no construction-time connection. Every
 *       operation uses the production short-lived Client and real Server.
 */
TEST(IpcHostDispatch, MapsEveryCurrentHostVirtualWithoutFallback) {
  ScopedTempDirectory temp("ps-ipc-host");
  const std::string socket_path = (temp.path() / "server.sock").string();
  testing::IpcHostSpy backend(GraphSessionId{"private-backend-session"});

  NodeInspectionView node;
  node.id = NodeId{7};
  node.name = "node-seven";
  node.type = "fixture";
  node.subtype = "source";
  backend.set_inspected_node(node);
  SchedulerInfoSnapshot scheduler_info;
  scheduler_info.intent = ComputeIntent::GlobalHighPrecision;
  scheduler_info.scheduler_name = "serial";
  scheduler_info.stats = "idle";
  backend.set_scheduler_info(scheduler_info);

  RunningServer server(backend, socket_path);
  std::unique_ptr<Host> host = create_ipc_host(socket_path);
  ASSERT_NE(host, nullptr);

  GraphLoadRequest load;
  load.session = GraphSessionId{"display-session"};
  load.root_dir = temp.path().string();
  const Result<GraphSessionId> loaded = host->load_graph(load);
  ASSERT_TRUE(loaded.status.ok) << loaded.status.message;
  ASSERT_EQ(loaded.value.value.size(), 32U);
  const GraphSessionId session = loaded.value;

  EXPECT_TRUE(host->list_graphs().status.ok);
  EXPECT_TRUE(host->reload_graph(session, (temp.path() / "in.yaml").string())
                  .status.ok);
  EXPECT_TRUE(
      host->save_graph(session, (temp.path() / "out.yaml").string()).status.ok);
  EXPECT_TRUE(host->clear_graph(session).status.ok);

  HostComputeRequest compute;
  compute.session = session;
  compute.node = NodeId{7};
  compute.cache.precision = "fp32";
  EXPECT_TRUE(host->compute(compute).status.ok);
  Result<std::future<OperationStatus>> async = host->compute_async(compute);
  ASSERT_TRUE(async.status.ok) << async.status.message;
  EXPECT_TRUE(async.value.get().ok);
  const Result<ImageBuffer> image = host->compute_and_get_image(compute);
  EXPECT_TRUE(image.status.ok) << image.status.message;
  EXPECT_EQ(image.value.data, nullptr);

  EXPECT_TRUE(host->timing(session).status.ok);
  EXPECT_TRUE(host->last_io_time(session).status.ok);
  EXPECT_TRUE(host->last_error(session).ok);
  EXPECT_TRUE(host->list_node_ids(session).status.ok);
  EXPECT_TRUE(host->ending_nodes(session).status.ok);
  EXPECT_TRUE(host->get_node_yaml(session, NodeId{7}).status.ok);
  EXPECT_TRUE(host->set_node_yaml(session, NodeId{7}, "id: 7").status.ok);
  EXPECT_TRUE(host->inspect_node(session, NodeId{7}).status.ok);
  EXPECT_TRUE(host->inspect_graph(session).status.ok);
  EXPECT_TRUE(host->dependency_tree(session, std::nullopt, false).status.ok);
  EXPECT_TRUE(host->traversal_orders(session).status.ok);
  EXPECT_TRUE(host->traversal_details(session).status.ok);
  EXPECT_TRUE(host->trees_containing_node(session, NodeId{7}).status.ok);
  EXPECT_TRUE(
      host->project_roi(session, NodeId{1}, PixelRect{1, 2, 3, 4}, NodeId{7})
          .status.ok);
  EXPECT_TRUE(host->project_roi_backward(session, NodeId{7},
                                         PixelRect{4, 3, 2, 1}, NodeId{1})
                  .status.ok);
  EXPECT_TRUE(host->dirty_region_snapshot(session).status.ok);
  EXPECT_TRUE(host->compute_planning_snapshot(session).status.ok);
  EXPECT_TRUE(host->recent_compute_planning_snapshots(session).status.ok);
  EXPECT_TRUE(host->begin_dirty_source(session, NodeId{7},
                                       DirtyDomain::HighPrecision,
                                       PixelRect{0, 0, 2, 2})
                  .status.ok);
  EXPECT_TRUE(host->update_dirty_source(session, NodeId{7},
                                        DirtyDomain::HighPrecision,
                                        PixelRect{1, 1, 2, 2})
                  .status.ok);
  EXPECT_TRUE(
      host->end_dirty_source(session, NodeId{7}, DirtyDomain::HighPrecision)
          .status.ok);
  EXPECT_TRUE(host->drain_compute_events(session, 1).status.ok);
  EXPECT_TRUE(host->scheduler_trace(session, 0, 1).status.ok);

  EXPECT_TRUE(host->clear_cache(session).status.ok);
  EXPECT_TRUE(host->clear_drive_cache(session).status.ok);
  EXPECT_TRUE(host->clear_memory_cache(session).status.ok);
  EXPECT_TRUE(host->cache_all_nodes(session, "fp32").status.ok);
  EXPECT_TRUE(host->free_transient_memory(session).status.ok);
  EXPECT_TRUE(host->synchronize_disk_cache(session, "fp32").status.ok);

  const std::vector<std::string> dirs{temp.path().string()};
  EXPECT_TRUE(host->plugins_load_report(dirs).status.ok);
  EXPECT_TRUE(host->plugins_load(dirs).status.ok);
  EXPECT_TRUE(host->plugins_unload_all().status.ok);
  EXPECT_TRUE(host->seed_builtin_ops().status.ok);
  EXPECT_TRUE(host->ops_sources().status.ok);
  EXPECT_TRUE(host->ops_combined_keys().status.ok);
  EXPECT_TRUE(host->ops_combined_sources().status.ok);

  EXPECT_TRUE(host->scheduler_available_types().status.ok);
  EXPECT_TRUE(host->scheduler_description("serial").status.ok);
  EXPECT_TRUE(host->scheduler_scan(dirs).status.ok);
  EXPECT_TRUE(
      host->scheduler_load((temp.path() / "scheduler.so").string()).status.ok);
  EXPECT_TRUE(host->scheduler_loaded_plugins().status.ok);
  HostSchedulerConfig config;
  config.hp_type = "serial";
  config.rt_type = "serial";
  config.worker_count = 2;
  EXPECT_TRUE(host->configure_scheduler_defaults(config).status.ok);
  EXPECT_TRUE(host->scheduler_info(session, ComputeIntent::GlobalHighPrecision)
                  .status.ok);
  EXPECT_TRUE(host->replace_scheduler(
                      session, ComputeIntent::GlobalHighPrecision, "serial")
                  .status.ok);
  EXPECT_TRUE(host->close_graph(session).status.ok);

  const std::vector<testing::IpcHostInvocation> invocations =
      backend.invocations();
  std::map<std::string, std::size_t> actual_counts;
  std::vector<std::string> actual_methods;
  actual_methods.reserve(invocations.size());
  for (const testing::IpcHostInvocation& invocation : invocations) {
    ++actual_counts[invocation.method];
    actual_methods.push_back(invocation.method);
  }
  const std::vector<std::string> expected_methods = {
      "graph.load",
      "graph.list",
      "graph.reload",
      "graph.save",
      "graph.clear",
      "compute.submit",
      "compute.submit",
      "compute.submit",
      "compute.timing",
      "compute.last_io_time",
      "compute.last_error",
      "inspect.node_ids",
      "inspect.ending_nodes",
      "graph.node_yaml.get",
      "graph.node_yaml.set",
      "inspect.node",
      "inspect.graph",
      "inspect.dependency_tree",
      "inspect.traversal_orders",
      "inspect.traversal_details",
      "inspect.trees_containing_node",
      "inspect.roi_forward",
      "inspect.roi_backward",
      "inspect.dirty_region",
      "inspect.compute_planning",
      "inspect.recent_compute_planning",
      "dirty.begin",
      "dirty.update",
      "dirty.end",
      "events.drain",
      "scheduler.trace",
      "cache.clear_all",
      "cache.clear_drive",
      "cache.clear_memory",
      "cache.cache_all_nodes",
      "cache.free_transient",
      "cache.synchronize_disk",
      "plugins.load_report",
      "plugins.load_report",
      "plugins.unload_all",
      "plugins.seed_builtins",
      "plugins.ops_sources",
      "plugins.ops_combined_keys",
      "plugins.ops_combined_sources",
      "scheduler.types",
      "scheduler.description",
      "scheduler.scan",
      "scheduler.load",
      "scheduler.loaded_plugins",
      "scheduler.configure_defaults",
      "scheduler.info",
      "scheduler.replace",
      "graph.close"};
  const std::map<std::string, std::size_t> expected_counts = {
      {"cache.cache_all_nodes", 1},
      {"cache.clear_all", 1},
      {"cache.clear_drive", 1},
      {"cache.clear_memory", 1},
      {"cache.free_transient", 1},
      {"cache.synchronize_disk", 1},
      {"compute.last_error", 1},
      {"compute.last_io_time", 1},
      {"compute.submit", 3},
      {"compute.timing", 1},
      {"dirty.begin", 1},
      {"dirty.end", 1},
      {"dirty.update", 1},
      {"events.drain", 1},
      {"graph.clear", 1},
      {"graph.close", 1},
      {"graph.list", 1},
      {"graph.load", 1},
      {"graph.node_yaml.get", 1},
      {"graph.node_yaml.set", 1},
      {"graph.reload", 1},
      {"graph.save", 1},
      {"inspect.compute_planning", 1},
      {"inspect.dependency_tree", 1},
      {"inspect.dirty_region", 1},
      {"inspect.ending_nodes", 1},
      {"inspect.graph", 1},
      {"inspect.node", 1},
      {"inspect.node_ids", 1},
      {"inspect.recent_compute_planning", 1},
      {"inspect.roi_backward", 1},
      {"inspect.roi_forward", 1},
      {"inspect.traversal_details", 1},
      {"inspect.traversal_orders", 1},
      {"inspect.trees_containing_node", 1},
      {"plugins.load_report", 2},
      {"plugins.ops_combined_keys", 1},
      {"plugins.ops_combined_sources", 1},
      {"plugins.ops_sources", 1},
      {"plugins.seed_builtins", 1},
      {"plugins.unload_all", 1},
      {"scheduler.configure_defaults", 1},
      {"scheduler.description", 1},
      {"scheduler.info", 1},
      {"scheduler.load", 1},
      {"scheduler.loaded_plugins", 1},
      {"scheduler.replace", 1},
      {"scheduler.scan", 1},
      {"scheduler.trace", 1},
      {"scheduler.types", 1}};
  EXPECT_EQ(invocations.size(), 53U);
  EXPECT_EQ(actual_methods, expected_methods);
  EXPECT_EQ(actual_counts, expected_counts);

  host.reset();
  const OperationStatus stopped = server.stop();
  EXPECT_TRUE(stopped.ok) << stopped.message;
}

/**
 * @brief Verifies nested Graph failures and plugin convenience translation.
 *
 * A real Server accepts one compute whose backend Host returns a Graph failure;
 * the IPC Host must return that immutable terminal status after the normal
 * submit/status/result/release lifecycle. The same adapter must distinguish an
 * outer-success `last_error` RPC from its nested diagnostic failure and must
 * implement `plugins_load` from exactly one load-report mutation, selecting the
 * report's first Graph error without rewriting code, stable name, or message.
 *
 * @return Nothing; GoogleTest assertions report status or mutation-count
 *         mismatch.
 * @throws std::bad_alloc, std::runtime_error, or std::system_error if server,
 *         graph, callback, or adapter setup cannot complete.
 * @note This exercises the production router/registry rather than manufacturing
 *       a terminal error directly at the adapter boundary.
 */
TEST(IpcHostStatus,
     PreservesTerminalAndLastErrorNestedFailuresAndFirstPluginError) {
  ScopedTempDirectory temp("ps-ipc-status");
  const std::string socket_path = (temp.path() / "server.sock").string();
  testing::IpcHostSpy backend(GraphSessionId{"private-backend-session"});
  const OperationStatus compute_failure = failure_status(
      OperationErrorDomain::Graph, static_cast<std::int32_t>(GraphErrc::Io),
      "io", "exact terminal compute failure");
  const OperationStatus last_error_failure =
      failure_status(OperationErrorDomain::Graph,
                     static_cast<std::int32_t>(GraphErrc::ComputeError),
                     "compute_error", "exact nested last-error diagnostic");
  backend.set_status("compute.submit", compute_failure);
  backend.set_last_error(last_error_failure);
  HostPluginLoadReport report;
  report.attempted = 3;
  report.loaded = 1;
  report.errors.push_back(HostPluginLoadError{"/plugins/first.so",
                                              GraphErrc::InvalidYaml,
                                              "exact first plugin failure"});
  report.errors.push_back(
      HostPluginLoadError{"/plugins/second.so", GraphErrc::Io,
                          "later plugin failure must not replace first"});
  backend.set_plugin_load_report(std::move(report));

  RunningServer server(backend, socket_path);
  std::unique_ptr<Host> host = create_ipc_host(socket_path);
  GraphLoadRequest load;
  load.session = GraphSessionId{"display-session"};
  load.root_dir = temp.path().string();
  const Result<GraphSessionId> loaded = host->load_graph(load);
  ASSERT_TRUE(loaded.status.ok) << loaded.status.message;
  backend.reset_invocations();

  HostComputeRequest compute;
  compute.session = loaded.value;
  compute.node = NodeId{7};
  const VoidResult computed = host->compute(compute);
  const OperationStatus last_error = host->last_error(loaded.value);
  const VoidResult plugins =
      host->plugins_load({"/plugins/first", "/plugins/second"});

  EXPECT_EQ(computed.status.ok, compute_failure.ok);
  EXPECT_EQ(computed.status.domain, compute_failure.domain);
  EXPECT_EQ(computed.status.code, compute_failure.code);
  EXPECT_EQ(computed.status.name, compute_failure.name);
  EXPECT_EQ(computed.status.message, compute_failure.message);
  EXPECT_EQ(last_error.ok, last_error_failure.ok);
  EXPECT_EQ(last_error.domain, last_error_failure.domain);
  EXPECT_EQ(last_error.code, last_error_failure.code);
  EXPECT_EQ(last_error.name, last_error_failure.name);
  EXPECT_EQ(last_error.message, last_error_failure.message);
  EXPECT_FALSE(plugins.status.ok);
  EXPECT_EQ(plugins.status.domain, OperationErrorDomain::Graph);
  EXPECT_EQ(plugins.status.code,
            static_cast<std::int32_t>(GraphErrc::InvalidYaml));
  EXPECT_EQ(plugins.status.name, "invalid_yaml");
  EXPECT_EQ(plugins.status.message, "exact first plugin failure");
  EXPECT_EQ(backend.call_count("compute.submit"), 1U);
  EXPECT_EQ(backend.call_count("compute.last_error"), 1U);
  EXPECT_EQ(backend.call_count("plugins.load_report"), 1U);

  host.reset();
  const OperationStatus stopped = server.stop();
  EXPECT_TRUE(stopped.ok) << stopped.message;
}

/**
 * @brief Returns the first status-RPC outer failure without terminal cleanup.
 *
 * Submission succeeds, but the first immediate `compute.status` reply is an
 * exact daemon error. The adapter must preserve that outer status and stop the
 * lifecycle without result, release, resubmission, or retry.
 *
 * @return Nothing; GoogleTest assertions report status or request mismatch.
 * @throws std::bad_alloc, std::runtime_error, or std::system_error if socket,
 *         callback, JSON, or peer setup cannot complete.
 * @note A live-listener watcher captures any forbidden follow-up connection.
 */
TEST(IpcHostStatus, ReturnsFirstStatusOuterFailureWithoutTerminalCleanup) {
  ScopedTempDirectory temp("ps-ipc-status-fail");
  const std::string socket_path = (temp.path() / "peer.sock").string();
  UniqueFd listener = create_test_listener(socket_path);
  const std::string compute_id(32, 'b');
  const std::string session_id(32, 'a');
  const std::vector<ShortConnectionReply> replies = {
      {"compute.submit",
       compute_job_result(compute_id, session_id, "queued", nullptr)},
      {"compute.status",
       Json{{"domain", "daemon"},
            {"code", -32010},
            {"name", "job_not_found"},
            {"message", "exact outer status failure"}},
       true}};
  std::vector<Json> requests;
  bool served = false;
  bool extra_connection_seen = false;
  std::atomic<bool> stop_extra_watch{false};
  std::thread peer([&] {
    const bool script_served =
        serve_short_connection_replies(listener.get(), replies, &requests);
    served = script_served &&
             watch_for_extra_connection(listener.get(), &stop_extra_watch,
                                        &extra_connection_seen);
  });
  RecordingPollingRuntime runtime;
  std::unique_ptr<Host> host =
      create_ipc_host_with_dependencies(socket_path, runtime.dependencies());
  HostComputeRequest request;
  request.session = GraphSessionId{session_id};
  request.node = NodeId{7};
  const VoidResult result = host->compute(request);
  stop_extra_watch.store(true);
  (void)::shutdown(listener.get(), SHUT_RDWR);
  listener.reset();
  peer.join();

  ASSERT_TRUE(served);
  EXPECT_FALSE(result.status.ok);
  EXPECT_EQ(result.status.domain, OperationErrorDomain::Daemon);
  EXPECT_EQ(result.status.code, -32010);
  EXPECT_EQ(result.status.name, "job_not_found");
  EXPECT_EQ(result.status.message, "exact outer status failure");
  EXPECT_TRUE(runtime.waits().empty());
  EXPECT_FALSE(extra_connection_seen);
  ASSERT_EQ(requests.size(), 2U);
  EXPECT_EQ(requests[0]["method"], "compute.submit");
  EXPECT_EQ(requests[1]["method"], "compute.status");
}

/**
 * @brief Verifies factory construction performs no daemon connection.
 * @return Nothing; GoogleTest assertions report construction or status errors.
 * @throws std::bad_alloc if adapter allocation fails.
 * @note The first ordinary call returns the exact one-attempt connect failure;
 *       no embedded Host is created as fallback.
 */
TEST(IpcHostFactory, DefersConnectionAndReturnsExactTransportFailure) {
  std::unique_ptr<Host> host =
      create_ipc_host("/path/that/does/not/exist.sock");
  ASSERT_NE(host, nullptr);
  const Result<std::vector<GraphSessionId>> result = host->list_graphs();
  EXPECT_FALSE(result.status.ok);
  EXPECT_EQ(result.status.domain, OperationErrorDomain::Transport);
  EXPECT_EQ(result.status.code, 1);
  EXPECT_EQ(result.status.name, "connect_failed");
}

/**
 * @brief Verifies exact polling cadence, one-attempt RPCs, and cleanup parity.
 *
 * A short-connection peer returns nine nonterminal status snapshots before one
 * terminal success, then a matching result and a failed best-effort release.
 * The synchronous Host call must wait 10/20/40/80/160/320/500/500/500 ms on
 * the injected monotonic timeline, never resubmit, and retain terminal success
 * despite cleanup failure.
 *
 * @return Nothing; GoogleTest assertions report cadence, method, or status
 *         mismatch.
 * @throws std::bad_alloc, std::runtime_error, or std::system_error if socket,
 *         JSON, callback, or peer setup cannot complete.
 * @note Fake waits advance across 2.13 seconds of cumulative virtual delay
 *       without wall-clock sleep. Together with the adapter's absence of a
 *       total-timeout branch, this covers the required unbounded sync policy.
 */
TEST(IpcHostPolling,
     UsesExactExponentialCadenceAndIgnoresReleaseFailureWithoutRetry) {
  ScopedTempDirectory temp("ps-ipc-poll");
  const std::string socket_path = (temp.path() / "peer.sock").string();
  UniqueFd listener = create_test_listener(socket_path);
  const std::string compute_id(32, 'b');
  const std::string session_id(32, 'a');
  const Json success = encode_operation_status(ok_status());

  std::vector<ShortConnectionReply> replies;
  replies.push_back(
      {"compute.submit",
       compute_job_result(compute_id, session_id, "queued", nullptr)});
  for (int index = 0; index < 9; ++index) {
    replies.push_back(
        {"compute.status",
         compute_job_result(compute_id, session_id,
                            index == 0 ? "queued" : "running", nullptr)});
  }
  replies.push_back(
      {"compute.status",
       compute_job_result(compute_id, session_id, "succeeded", success)});
  replies.push_back(
      {"compute.result",
       compute_job_result(compute_id, session_id, "succeeded", success)});
  replies.push_back({"compute.release",
                     Json{{"domain", "daemon"},
                          {"code", -32010},
                          {"name", "job_not_found"},
                          {"message", "scripted cleanup failure"}},
                     true});

  std::vector<Json> requests;
  bool served = false;
  std::thread peer([&] {
    served = serve_short_connection_replies(listener.get(), replies, &requests);
  });
  RecordingPollingRuntime runtime;
  std::unique_ptr<Host> host =
      create_ipc_host_with_dependencies(socket_path, runtime.dependencies());
  HostComputeRequest request;
  request.session = GraphSessionId{session_id};
  request.node = NodeId{7};
  request.cache.precision = "fp32";
  const VoidResult result = host->compute(request);
  (void)::shutdown(listener.get(), SHUT_RDWR);
  listener.reset();
  peer.join();

  ASSERT_TRUE(served);
  EXPECT_TRUE(result.status.ok) << result.status.message;
  EXPECT_EQ(runtime.waits(),
            (std::vector<std::chrono::milliseconds>{
                std::chrono::milliseconds(10), std::chrono::milliseconds(20),
                std::chrono::milliseconds(40), std::chrono::milliseconds(80),
                std::chrono::milliseconds(160), std::chrono::milliseconds(320),
                std::chrono::milliseconds(500), std::chrono::milliseconds(500),
                std::chrono::milliseconds(500)}));
  ASSERT_EQ(requests.size(), replies.size());
  EXPECT_EQ(requests.front()["method"], "compute.submit");
  EXPECT_EQ(requests.back()["method"], "compute.release");
  EXPECT_EQ(std::count_if(requests.begin(), requests.end(),
                          [](const Json& row) {
                            return row["method"] == "compute.submit";
                          }),
            1);
  EXPECT_EQ(std::count_if(requests.begin(), requests.end(),
                          [](const Json& row) {
                            return row["method"] == "compute.status";
                          }),
            10);
}

/**
 * @brief Verifies destruction wakes a worker blocked between status polls.
 *
 * The peer returns one queued status so the async worker enters a real injected
 * condition-variable wait. Adapter destruction must publish stop, call the
 * wake callback, complete the future with exact `client_stopped`, and join
 * without a second status RPC, result, release, or resubmission.
 *
 * @return Nothing; GoogleTest assertions report wake, timing, status, or
 *         request failures.
 * @throws std::bad_alloc, std::runtime_error, or std::system_error if socket,
 *         callback, future, JSON, or peer setup cannot complete.
 * @note The injected wait has a two-second fallback; the passing path must
 *       finish well before it, proving notification rather than timeout.
 */
TEST(IpcHostLifecycle,
     DestructionWakesBlockedPollingDeadlineAndCompletesClientStopped) {
  ScopedTempDirectory temp("ps-ipc-blocked-wait");
  const std::string socket_path = (temp.path() / "peer.sock").string();
  UniqueFd listener = create_test_listener(socket_path);
  const std::string compute_id(32, 'b');
  const std::string session_id(32, 'a');
  const std::vector<ShortConnectionReply> replies = {
      {"compute.submit",
       compute_job_result(compute_id, session_id, "queued", nullptr)},
      {"compute.status",
       compute_job_result(compute_id, session_id, "queued", nullptr)}};
  std::vector<Json> requests;
  bool served = false;
  bool extra_connection_seen = false;
  std::atomic<bool> stop_extra_watch{false};
  std::thread peer([&] {
    const bool script_served =
        serve_short_connection_replies(listener.get(), replies, &requests);
    served = script_served &&
             watch_for_extra_connection(listener.get(), &stop_extra_watch,
                                        &extra_connection_seen);
  });
  BlockingPollingRuntime runtime;
  std::unique_ptr<Host> host =
      create_ipc_host_with_dependencies(socket_path, runtime.dependencies());
  HostComputeRequest request;
  request.session = GraphSessionId{session_id};
  request.node = NodeId{7};
  Result<std::future<OperationStatus>> started = host->compute_async(request);
  const bool blocked =
      started.status.ok &&
      runtime.wait_until_blocked(std::chrono::milliseconds(2000));

  const auto destruction_started = std::chrono::steady_clock::now();
  host.reset();
  const auto destruction_elapsed =
      std::chrono::steady_clock::now() - destruction_started;
  stop_extra_watch.store(true);
  (void)::shutdown(listener.get(), SHUT_RDWR);
  listener.reset();
  peer.join();

  ASSERT_TRUE(started.status.ok) << started.status.message;
  ASSERT_TRUE(blocked);
  ASSERT_TRUE(started.value.valid());
  ASSERT_EQ(started.value.wait_for(std::chrono::seconds(0)),
            std::future_status::ready);
  const OperationStatus stopped = started.value.get();
  EXPECT_FALSE(stopped.ok);
  EXPECT_EQ(stopped.domain, OperationErrorDomain::Transport);
  EXPECT_EQ(stopped.code, 5);
  EXPECT_EQ(stopped.name, "client_stopped");
  EXPECT_LT(destruction_elapsed, std::chrono::milliseconds(1500));
  EXPECT_EQ(runtime.wake_count(), 1U);
  EXPECT_TRUE(served);
  EXPECT_FALSE(extra_connection_seen);
  ASSERT_EQ(requests.size(), 2U);
  EXPECT_EQ(requests[0]["method"], "compute.submit");
  EXPECT_EQ(requests[1]["method"], "compute.status");
}

/**
 * @brief Verifies destruction interrupts a status RPC blocked on peer I/O.
 *
 * The peer accepts one submission, then reads a status request without
 * replying. Adapter destruction must publish stop, wake the injected waiter,
 * shut down the registered Client descriptor, complete the future with exact
 * Transport code 5 `client_stopped`, and join without resubmitting or releasing
 * the unfinished daemon job.
 *
 * @return Nothing; GoogleTest assertions report lifecycle, status, or request
 *         ordering failures.
 * @throws std::bad_alloc, std::runtime_error, or std::system_error if test
 *         socket, JSON, callback, future, or thread setup cannot complete.
 * @note The peer uses a two-second poll fallback, so a regression fails a
 *       bounded timing assertion instead of hanging until the CTest timeout.
 */
TEST(IpcHostLifecycle,
     DestructionInterruptsBlockedStatusIoAndCompletesClientStopped) {
  ScopedTempDirectory temp("ps-ipc-blocked-status");
  const std::string socket_path = (temp.path() / "peer.sock").string();
  UniqueFd listener = create_test_listener(socket_path);
  const std::string compute_id(32, 'b');
  const std::string session_id(32, 'a');

  std::mutex peer_mutex;
  std::condition_variable peer_changed;
  bool status_request_seen = false;
  bool peer_saw_shutdown = false;
  bool extra_connection_seen = false;
  bool served = false;
  std::atomic<bool> stop_extra_watch{false};
  std::vector<Json> requests;
  std::thread peer([&] {
    bool local_served = false;
    UniqueFd submit_peer(::accept(listener.get(), nullptr, nullptr));
    Json submit_request;
    if (submit_peer &&
        read_expected_request(submit_peer.get(), "compute.submit",
                              &submit_request) &&
        write_correlated_reply(
            submit_peer.get(), submit_request,
            compute_job_result(compute_id, session_id, "queued", nullptr))) {
      requests.push_back(std::move(submit_request));
      UniqueFd status_peer(::accept(listener.get(), nullptr, nullptr));
      Json status_request;
      if (status_peer &&
          read_expected_request(status_peer.get(), "compute.status",
                                &status_request)) {
        requests.push_back(std::move(status_request));
        {
          std::lock_guard<std::mutex> lock(peer_mutex);
          status_request_seen = true;
        }
        peer_changed.notify_all();

        pollfd descriptor{status_peer.get(), POLLIN | POLLHUP, 0};
        const int poll_result = ::poll(&descriptor, 1, 2000);
        char byte = 0;
        const ssize_t read_result =
            poll_result > 0 ? ::read(status_peer.get(), &byte, 1) : -1;
        {
          std::lock_guard<std::mutex> lock(peer_mutex);
          peer_saw_shutdown = poll_result > 0 && read_result <= 0;
        }
        local_served = watch_for_extra_connection(
            listener.get(), &stop_extra_watch, &extra_connection_seen);
      }
    }
    {
      std::lock_guard<std::mutex> lock(peer_mutex);
      served = local_served;
    }
    peer_changed.notify_all();
  });

  RecordingPollingRuntime runtime;
  std::unique_ptr<Host> host =
      create_ipc_host_with_dependencies(socket_path, runtime.dependencies());
  HostComputeRequest request;
  request.session = GraphSessionId{session_id};
  request.node = NodeId{7};
  Result<std::future<OperationStatus>> started = host->compute_async(request);

  bool saw_status = false;
  if (started.status.ok) {
    std::unique_lock<std::mutex> lock(peer_mutex);
    saw_status = peer_changed.wait_for(lock, std::chrono::seconds(2),
                                       [&] { return status_request_seen; });
  }
  const auto destruction_started = std::chrono::steady_clock::now();
  host.reset();
  const auto destruction_elapsed =
      std::chrono::steady_clock::now() - destruction_started;
  stop_extra_watch.store(true);
  (void)::shutdown(listener.get(), SHUT_RDWR);
  listener.reset();
  peer.join();

  ASSERT_TRUE(started.status.ok) << started.status.message;
  ASSERT_TRUE(saw_status);
  ASSERT_TRUE(started.value.valid());
  ASSERT_EQ(started.value.wait_for(std::chrono::seconds(0)),
            std::future_status::ready);
  const OperationStatus stopped = started.value.get();
  EXPECT_FALSE(stopped.ok);
  EXPECT_EQ(stopped.domain, OperationErrorDomain::Transport);
  EXPECT_EQ(stopped.code, 5);
  EXPECT_EQ(stopped.name, "client_stopped");
  EXPECT_LT(destruction_elapsed, std::chrono::milliseconds(1500));
  EXPECT_EQ(runtime.wake_count(), 1U);
  EXPECT_TRUE(served);
  EXPECT_TRUE(peer_saw_shutdown);
  EXPECT_FALSE(extra_connection_seen);
  ASSERT_EQ(requests.size(), 2U);
  EXPECT_EQ(requests[0]["method"], "compute.submit");
  EXPECT_EQ(requests[1]["method"], "compute.status");
}

/**
 * @brief Proves adapter stop wins a race with validated async publication.
 *
 * The scripted job reaches a fully validated successful result, then the
 * injected completion hook blocks its worker. Destruction must linearize stop,
 * wake and interrupt first, publish preallocated `client_stopped` before its
 * join can finish, and discard the worker's later success. The stopped cleanup
 * path must not release the daemon job.
 *
 * @return Nothing; GoogleTest assertions report ordering, future, or request
 *         failures.
 * @throws std::bad_alloc, std::runtime_error, or std::system_error if socket,
 *         callback, JSON, future, or thread setup cannot complete.
 * @note The release guard is declared after Host ownership, so every exception
 *       or fatal assertion path opens the gate before implicit Host teardown.
 */
TEST(IpcHostLifecycle,
     StopBeforeAsyncPublicationCompletesFutureBeforeWorkerJoin) {
  ScopedTempDirectory temp("ps-ipc-completion-race");
  const std::string socket_path = (temp.path() / "peer.sock").string();
  UniqueFd listener = create_test_listener(socket_path);
  const std::string compute_id(32, 'b');
  const std::string session_id(32, 'a');
  const Json success = encode_operation_status(ok_status());
  const std::vector<ShortConnectionReply> replies = {
      {"compute.submit",
       compute_job_result(compute_id, session_id, "queued", nullptr)},
      {"compute.status",
       compute_job_result(compute_id, session_id, "succeeded", success)},
      {"compute.result",
       compute_job_result(compute_id, session_id, "succeeded", success)}};

  std::vector<Json> requests;
  bool served = false;
  bool extra_connection_seen = false;
  std::atomic<bool> stop_extra_watch{false};
  std::thread peer([&] {
    const bool script_served =
        serve_short_connection_replies(listener.get(), replies, &requests);
    served = script_served &&
             watch_for_extra_connection(listener.get(), &stop_extra_watch,
                                        &extra_connection_seen);
  });
  AsyncCompletionGate gate;
  RecordingPollingRuntime runtime([&gate] { gate.block(); });
  std::unique_ptr<Host> host =
      create_ipc_host_with_dependencies(socket_path, runtime.dependencies());
  AsyncCompletionGateReleaseGuard release_guard(gate);
  HostComputeRequest request;
  request.session = GraphSessionId{session_id};
  request.node = NodeId{7};
  Result<std::future<OperationStatus>> started = host->compute_async(request);

  const bool entered = started.status.ok &&
                       gate.wait_until_entered(std::chrono::milliseconds(2000));

  bool ready_before_join = false;
  bool wake_observed_when_future_ready = false;
  if (entered) {
    std::thread destroyer([&host] { host.reset(); });
    ready_before_join =
        started.value.wait_for(std::chrono::milliseconds(1000)) ==
        std::future_status::ready;
    wake_observed_when_future_ready =
        ready_before_join && runtime.wake_count() == 1U;
    gate.release();
    destroyer.join();
  } else {
    gate.release();
    host.reset();
  }
  stop_extra_watch.store(true);
  (void)::shutdown(listener.get(), SHUT_RDWR);
  listener.reset();
  peer.join();

  ASSERT_TRUE(started.status.ok) << started.status.message;
  ASSERT_TRUE(entered);
  ASSERT_TRUE(served);
  ASSERT_TRUE(started.value.valid());
  ASSERT_TRUE(ready_before_join);
  ASSERT_TRUE(wake_observed_when_future_ready);
  const OperationStatus stopped = started.value.get();
  EXPECT_FALSE(stopped.ok);
  EXPECT_EQ(stopped.domain, OperationErrorDomain::Transport);
  EXPECT_EQ(stopped.code, 5);
  EXPECT_EQ(stopped.name, "client_stopped");
  EXPECT_EQ(runtime.wake_count(), 1U);
  EXPECT_FALSE(extra_connection_seen);
  ASSERT_EQ(requests.size(), 3U);
  EXPECT_EQ(requests[0]["method"], "compute.submit");
  EXPECT_EQ(requests[1]["method"], "compute.status");
  EXPECT_EQ(requests[2]["method"], "compute.result");
}

/** @brief Number of observed test mapping creations. */
std::atomic<std::size_t> observed_map_calls{0};

/** @brief Number of observed final-owner unmap calls. */
std::atomic<std::size_t> observed_unmap_calls{0};

/** @brief Number of observed final-owner descriptor close calls. */
std::atomic<std::size_t> observed_close_calls{0};

/** @brief Global sequence used to verify unmap-before-close cleanup order. */
std::atomic<std::size_t> observed_cleanup_sequence{0};

/** @brief Cleanup sequence assigned to the observed unmap call. */
std::atomic<std::size_t> observed_unmap_order{0};

/** @brief Cleanup sequence assigned to the observed close call. */
std::atomic<std::size_t> observed_close_order{0};

/** @brief POSIX result returned by the observed unmap call. */
std::atomic<int> observed_unmap_result{-1};

/** @brief POSIX result returned by the observed close call. */
std::atomic<int> observed_close_result{-1};

/**
 * @brief Maps one complete test artifact through the real read-only syscall.
 * @param fd Open validated artifact descriptor.
 * @param length Exact nonzero artifact byte count.
 * @return Mapping address, or `MAP_FAILED` with `errno` set.
 * @throws Nothing.
 */
void* observed_map_read_only(int fd, std::size_t length) noexcept {
  ++observed_map_calls;
  return ::mmap(nullptr, length, PROT_READ, MAP_PRIVATE, fd, 0);
}

/**
 * @brief Maps one artifact and changes its mode before post-map validation.
 * @param fd Open prevalidated artifact descriptor.
 * @param length Exact nonzero artifact byte count.
 * @return Real mapping address, or `MAP_FAILED` with `errno` set.
 * @throws Nothing.
 * @note `fchmod` failure is intentionally left for the production revalidation
 *       path to observe as either unchanged metadata or a system failure.
 */
void* observed_map_then_change_mode(int fd, std::size_t length) noexcept {
  void* mapping = observed_map_read_only(fd, length);
  if (mapping != MAP_FAILED) {
    (void)::fchmod(fd, 0640);
  }
  return mapping;
}

/**
 * @brief Counts and executes one real artifact unmap.
 * @param address Exact mapping base address.
 * @param length Exact mapped byte count.
 * @return POSIX `munmap` result.
 * @throws Nothing.
 */
int observed_unmap(void* address, std::size_t length) noexcept {
  ++observed_unmap_calls;
  observed_unmap_order = ++observed_cleanup_sequence;
  const int result = ::munmap(address, length);
  observed_unmap_result = result;
  return result;
}

/**
 * @brief Counts and executes one real mapping-owned descriptor close.
 * @param fd Exact descriptor retained for the mapping lifetime.
 * @return POSIX `close` result.
 * @throws Nothing.
 */
int observed_close(int fd) noexcept {
  ++observed_close_calls;
  observed_close_order = ++observed_cleanup_sequence;
  const int result = ::close(fd);
  observed_close_result = result;
  return result;
}

/**
 * @brief Resets every observable mapping lifetime counter.
 * @return Nothing.
 * @throws Nothing.
 */
void reset_observed_mapping_lifetime() noexcept {
  observed_map_calls = 0;
  observed_unmap_calls = 0;
  observed_close_calls = 0;
  observed_cleanup_sequence = 0;
  observed_unmap_order = 0;
  observed_close_order = 0;
  observed_unmap_result = -1;
  observed_close_result = -1;
}

/**
 * @brief Returns observable wrappers around the real mapping syscalls.
 * @return Complete source-private mapping operation table.
 * @throws Nothing.
 */
ArtifactMappingOperations observed_mapping_operations() noexcept {
  return {observed_map_read_only, observed_unmap, observed_close};
}

/**
 * @brief Returns operations that tamper mode immediately after a real mmap.
 * @return Observable post-map tamper operation table.
 * @throws Nothing.
 */
ArtifactMappingOperations post_map_tamper_operations() noexcept {
  return {observed_map_then_change_mode, observed_unmap, observed_close};
}

/**
 * @brief Creates one protected tight-row UINT8 artifact and exact metadata.
 * @tparam Size Compile-time nonzero payload byte count.
 * @param path Absolute path below a mode-0700 temporary directory.
 * @param bytes Exact artifact payload.
 * @return Metadata matching the new same-user mode-0600 regular file.
 * @throws std::bad_alloc or std::system_error if path storage or file IO fails.
 */
template <std::size_t Size>
::ps::ipc::OutputArtifactMetadata write_test_artifact(
    const std::filesystem::path& path,
    const std::array<std::uint8_t, Size>& bytes) {
  static_assert(Size > 0, "artifact test payload must not be empty");
  static_assert(
      Size <= static_cast<std::size_t>(std::numeric_limits<int>::max()),
      "artifact test width must fit int");
  UniqueFd artifact(
      ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600));
  if (!artifact) {
    throw std::system_error(errno, std::generic_category(), "artifact open");
  }
  std::size_t written = 0;
  while (written < bytes.size()) {
    const ssize_t count =
        ::write(artifact.get(), bytes.data() + written, bytes.size() - written);
    if (count < 0 && errno == EINTR) {
      continue;
    }
    if (count <= 0) {
      throw std::system_error(count < 0 ? errno : EIO, std::generic_category(),
                              "artifact write");
    }
    written += static_cast<std::size_t>(count);
  }
  if (::fchmod(artifact.get(), 0600) != 0) {
    throw std::system_error(errno, std::generic_category(), "artifact chmod");
  }
  struct stat artifact_stat{};
  if (::fstat(artifact.get(), &artifact_stat) != 0) {
    throw std::system_error(errno, std::generic_category(), "artifact fstat");
  }

  ::ps::ipc::OutputArtifactMetadata metadata;
  metadata.output_id = OutputArtifactId{std::string(32, 'c')};
  metadata.path = path.string();
  metadata.width = static_cast<int>(Size);
  metadata.height = 1;
  metadata.channels = 1;
  metadata.data_type = DataType::UINT8;
  metadata.device = Device::CPU;
  metadata.row_step = Size;
  metadata.byte_size = Size;
  metadata.filesystem_device = static_cast<std::uint64_t>(artifact_stat.st_dev);
  metadata.inode = static_cast<std::uint64_t>(artifact_stat.st_ino);
  return metadata;
}

/**
 * @brief Verifies one mismatched artifact is rejected before mapping.
 * @param metadata Tampered metadata or metadata for a tampered file.
 * @return Nothing; GoogleTest assertions report status or byte exposure.
 * @throws std::bad_alloc if validation diagnostics cannot allocate.
 */
void expect_artifact_mapping_rejected(
    const ::ps::ipc::OutputArtifactMetadata& metadata) {
  reset_observed_mapping_lifetime();
  const Result<ImageBuffer> mapped = consume_artifact_readonly_mapping(
      metadata, observed_mapping_operations());
  EXPECT_FALSE(mapped.status.ok);
  EXPECT_EQ(mapped.status.domain, OperationErrorDomain::Protocol);
  EXPECT_EQ(mapped.status.code, kInvalidRequestCode);
  EXPECT_EQ(mapped.status.name, "invalid_request");
  EXPECT_TRUE(mapped.value.data == nullptr);
  EXPECT_EQ(observed_map_calls.load(), 0U);
  EXPECT_EQ(observed_unmap_calls.load(), 0U);
  EXPECT_EQ(observed_close_calls.load(), 0U);
}

/**
 * @brief Rejects device, inode, mode, link, and size mismatches before mmap.
 * @return Nothing; GoogleTest assertions report setup, status, or exposure.
 * @throws std::bad_alloc, std::filesystem::filesystem_error, or
 *         std::system_error if test artifact setup fails.
 */
TEST(IpcHostArtifact, RejectsEveryProtectedArtifactIdentityTamperBeforeMmap) {
  ScopedTempDirectory temp("ps-ipc-tamper");
  const std::array<std::uint8_t, 6> bytes = {1, 2, 3, 4, 5, 6};

  ::ps::ipc::OutputArtifactMetadata wrong_device =
      write_test_artifact(temp.path() / "device.bin", bytes);
  wrong_device.filesystem_device ^= 1U;
  expect_artifact_mapping_rejected(wrong_device);

  ::ps::ipc::OutputArtifactMetadata wrong_inode =
      write_test_artifact(temp.path() / "inode.bin", bytes);
  wrong_inode.inode ^= 1U;
  expect_artifact_mapping_rejected(wrong_inode);

  ::ps::ipc::OutputArtifactMetadata wrong_mode =
      write_test_artifact(temp.path() / "mode.bin", bytes);
  ASSERT_EQ(::chmod(wrong_mode.path.c_str(), 0640), 0);
  expect_artifact_mapping_rejected(wrong_mode);

  ::ps::ipc::OutputArtifactMetadata extra_link =
      write_test_artifact(temp.path() / "link.bin", bytes);
  const std::filesystem::path second_link = temp.path() / "second-link.bin";
  ASSERT_EQ(::link(extra_link.path.c_str(), second_link.c_str()), 0);
  expect_artifact_mapping_rejected(extra_link);

  ::ps::ipc::OutputArtifactMetadata wrong_size =
      write_test_artifact(temp.path() / "size.bin", bytes);
  ASSERT_EQ(::truncate(wrong_size.path.c_str(), 5), 0);
  expect_artifact_mapping_rejected(wrong_size);
}

/**
 * @brief Rejects and unmaps a descriptor changed immediately after mmap.
 * @return Nothing; GoogleTest assertions report classification, byte exposure,
 *         or pre-ownership cleanup count/result mismatch.
 * @throws std::bad_alloc, std::filesystem::filesystem_error, or
 *         std::system_error if test artifact setup fails.
 */
TEST(IpcHostArtifact, PostMapIdentityMismatchUnmapsBeforeExposingBytes) {
  ScopedTempDirectory temp("ps-ipc-postmap");
  const std::array<std::uint8_t, 6> bytes = {1, 2, 3, 4, 5, 6};
  const ::ps::ipc::OutputArtifactMetadata metadata =
      write_test_artifact(temp.path() / "image.bin", bytes);
  reset_observed_mapping_lifetime();
  const Result<ImageBuffer> mapped =
      consume_artifact_readonly_mapping(metadata, post_map_tamper_operations());

  EXPECT_FALSE(mapped.status.ok);
  EXPECT_EQ(mapped.status.domain, OperationErrorDomain::Protocol);
  EXPECT_EQ(mapped.status.code, kInvalidRequestCode);
  EXPECT_EQ(mapped.status.name, "invalid_request");
  EXPECT_TRUE(mapped.value.data == nullptr);
  EXPECT_EQ(observed_map_calls.load(), 1U);
  EXPECT_EQ(observed_unmap_calls.load(), 1U);
  EXPECT_EQ(observed_unmap_result.load(), 0);
  EXPECT_EQ(observed_close_calls.load(), 0U);
}

/**
 * @brief Verifies shared mappings unmap and close once at final release.
 * @return Nothing; GoogleTest assertions report bytes, reference lifetime,
 *         syscall count/result/order, or source-unlink survival.
 * @throws std::bad_alloc, std::filesystem::filesystem_error, or
 *         std::system_error if artifact or shared-owner setup fails.
 */
TEST(IpcHostArtifact, SharedMappingUnmapsAndClosesExactlyOnceAtLastReference) {
  ScopedTempDirectory temp("ps-ipc-map-life");
  const std::filesystem::path artifact_path = temp.path() / "image.bin";
  const std::array<std::uint8_t, 6> expected = {0x10, 0x20, 0x30,
                                                0x40, 0x50, 0x60};
  const ::ps::ipc::OutputArtifactMetadata metadata =
      write_test_artifact(artifact_path, expected);
  reset_observed_mapping_lifetime();
  Result<ImageBuffer> mapped = consume_artifact_readonly_mapping(
      metadata, observed_mapping_operations());
  ASSERT_TRUE(mapped.status.ok) << mapped.status.message;
  ASSERT_NE(mapped.value.data, nullptr);
  const auto page_size = ::sysconf(_SC_PAGESIZE);
  ASSERT_GT(page_size, 0);
  EXPECT_EQ(reinterpret_cast<std::uintptr_t>(mapped.value.data.get()) %
                static_cast<std::uintptr_t>(page_size),
            0U);
  EXPECT_EQ(observed_map_calls.load(), 1U);
  EXPECT_EQ(observed_unmap_calls.load(), 0U);
  EXPECT_EQ(observed_close_calls.load(), 0U);

  ImageBuffer first = std::move(mapped.value);
  ImageBuffer second = first;
  std::shared_ptr<void> last = second.data;
  ASSERT_EQ(::unlink(artifact_path.c_str()), 0);
  first.data.reset();
  EXPECT_EQ(observed_unmap_calls.load(), 0U);
  EXPECT_EQ(observed_close_calls.load(), 0U);
  second.data.reset();
  EXPECT_EQ(observed_unmap_calls.load(), 0U);
  EXPECT_EQ(observed_close_calls.load(), 0U);
  EXPECT_EQ(std::memcmp(last.get(), expected.data(), expected.size()), 0);

  last.reset();
  EXPECT_EQ(observed_unmap_calls.load(), 1U);
  EXPECT_EQ(observed_close_calls.load(), 1U);
  EXPECT_EQ(observed_unmap_result.load(), 0);
  EXPECT_EQ(observed_close_result.load(), 0);
  EXPECT_EQ(observed_unmap_order.load(), 1U);
  EXPECT_EQ(observed_close_order.load(), 2U);
}

/**
 * @brief Verifies the public IPC Host maps image bytes before lease release.
 *
 * A production factory consumes one exact mode-0600 same-user regular artifact
 * advertised by a scripted terminal result. It must validate identity/layout,
 * map the tight CPU payload, release with the matching delivery lease, and
 * retain readable bytes after the source path is unlinked.
 *
 * @return Nothing; GoogleTest assertions report metadata, ownership, request,
 *         or image mismatch.
 * @throws std::bad_alloc, std::runtime_error, or std::system_error if file,
 *         socket, JSON, Host, or peer setup cannot complete.
 * @note Request order proves mapping finishes before the cleanup guard sends
 *       the lease-aware release; source unlink then models store cleanup.
 */
TEST(IpcHostArtifact,
     ProductionFactoryMapsProtectedImageBeforeLeaseAwareRelease) {
  ScopedTempDirectory temp("ps-ipc-artifact");
  const std::filesystem::path artifact_path = temp.path() / "image.bin";
  const std::array<std::uint8_t, 6> expected = {0x10, 0x20, 0x30,
                                                0x40, 0x50, 0x60};
  UniqueFd artifact(::open(artifact_path.c_str(),
                           O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600));
  ASSERT_TRUE(artifact);
  std::size_t written = 0;
  while (written < expected.size()) {
    const ssize_t count = ::write(artifact.get(), expected.data() + written,
                                  expected.size() - written);
    ASSERT_GT(count, 0);
    written += static_cast<std::size_t>(count);
  }
  ASSERT_EQ(::fchmod(artifact.get(), 0600), 0);
  struct stat artifact_stat{};
  ASSERT_EQ(::fstat(artifact.get(), &artifact_stat), 0);
  artifact.reset();

  const std::string socket_path = (temp.path() / "peer.sock").string();
  UniqueFd listener = create_test_listener(socket_path);
  const std::string session_id(32, 'a');
  const std::string compute_id(32, 'b');
  const std::string output_id(32, 'c');
  const std::string delivery_id(32, 'd');
  const Json success = encode_operation_status(ok_status());
  const Json output{
      {"output_id", output_id},
      {"delivery_id", delivery_id},
      {"path", artifact_path.string()},
      {"width", 2},
      {"height", 1},
      {"channels", 3},
      {"data_type", "uint8"},
      {"device", "cpu"},
      {"row_step", 6},
      {"byte_size", 6},
      {"filesystem_device", static_cast<std::uint64_t>(artifact_stat.st_dev)},
      {"inode", static_cast<std::uint64_t>(artifact_stat.st_ino)}};
  const std::vector<ShortConnectionReply> replies = {
      {"compute.submit",
       compute_job_result(compute_id, session_id, "queued", nullptr)},
      {"compute.status",
       compute_job_result(compute_id, session_id, "succeeded", success)},
      {"compute.result", compute_job_result(compute_id, session_id, "succeeded",
                                            success, output)},
      {"compute.release", Json{{"compute_id", compute_id}, {"released", true}},
       false, [&artifact_path](const Json&) {
         return ::unlink(artifact_path.c_str()) == 0;
       }}};

  std::vector<Json> requests;
  bool served = false;
  std::thread peer([&] {
    served = serve_short_connection_replies(listener.get(), replies, &requests);
  });
  std::unique_ptr<Host> host = create_ipc_host(socket_path);
  HostComputeRequest request;
  request.session = GraphSessionId{session_id};
  request.node = NodeId{7};
  const Result<ImageBuffer> image = host->compute_and_get_image(request);
  (void)::shutdown(listener.get(), SHUT_RDWR);
  listener.reset();
  peer.join();

  ASSERT_TRUE(served);
  EXPECT_FALSE(std::filesystem::exists(artifact_path));
  ASSERT_TRUE(image.status.ok) << image.status.message;
  ASSERT_NE(image.value.data, nullptr);
  EXPECT_EQ(image.value.width, 2);
  EXPECT_EQ(image.value.height, 1);
  EXPECT_EQ(image.value.channels, 3);
  EXPECT_EQ(image.value.type, DataType::UINT8);
  EXPECT_EQ(image.value.device, Device::CPU);
  EXPECT_EQ(image.value.step, 6U);
  EXPECT_EQ(image.value.context, nullptr);
  EXPECT_EQ(
      std::memcmp(image.value.data.get(), expected.data(), expected.size()), 0);
  ASSERT_EQ(requests.size(), 4U);
  EXPECT_EQ(requests[0]["params"]["result_mode"], "image");
  EXPECT_EQ(requests[3]["method"], "compute.release");
  EXPECT_EQ(requests[3]["params"]["compute_id"], compute_id);
  EXPECT_EQ(requests[3]["params"]["delivery_id"], delivery_id);
}

}  // namespace
}  // namespace ps::ipc::internal
