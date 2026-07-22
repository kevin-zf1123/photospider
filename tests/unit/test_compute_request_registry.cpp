#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <future>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "ipc/codec.hpp"
#include "ipc/compute_request_registry.hpp"
#include "ipc/session_registry.hpp"

namespace ps::ipc::internal {
namespace {

using std::chrono_literals::operator""h;
using std::chrono_literals::operator""s;

/**
 * @brief Produces one deterministic valid 128-bit-shaped opaque value.
 *
 * @param value Low-order test sequence value.
 * @return Exactly 32 lowercase hexadecimal characters.
 * @throws std::bad_alloc if string storage cannot be allocated.
 */
std::string opaque_value(std::uint64_t value) {
  constexpr char kHex[] = "0123456789abcdef";
  std::string result(32, '0');
  std::size_t index = result.size();
  do {
    result[--index] = kHex[value & 0x0fU];
    value >>= 4U;
  } while (value != 0 && index != 0);
  return result;
}

/**
 * @brief Thread-safe deterministic opaque source with optional scripted values.
 *
 * @throws std::bad_alloc when returned string storage cannot be allocated.
 * @note Scripted values cover collisions and malformed generator output; after
 *       exhaustion, monotonically increasing valid values are returned.
 */
class OpaqueSequence {
 public:
  /**
   * @brief Creates an incrementing source.
   * @param first First fallback numeric value.
   * @throws Nothing.
   */
  explicit OpaqueSequence(std::uint64_t first = 1) : next_(first) {}

  /**
   * @brief Creates a scripted source followed by incrementing fallback values.
   * @param scripted Exact candidates returned first.
   * @param first First fallback numeric value after the script.
   * @throws std::bad_alloc if script ownership cannot be allocated.
   */
  OpaqueSequence(std::vector<std::string> scripted, std::uint64_t first)
      : scripted_(std::move(scripted)), next_(first) {}

  /**
   * @brief Returns the next candidate under one source mutex.
   * @return Scripted or generated candidate.
   * @throws std::bad_alloc if copied storage cannot be allocated.
   */
  std::string next() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (scripted_index_ < scripted_.size()) {
      return scripted_[scripted_index_++];
    }
    return opaque_value(next_++);
  }

 private:
  /** @brief Serializes scripted index and fallback sequence. */
  std::mutex mutex_;

  /** @brief Exact candidates returned before fallback generation. */
  std::vector<std::string> scripted_;

  /** @brief Next unread scripted candidate. */
  std::size_t scripted_index_ = 0;

  /** @brief Next fallback numeric value. */
  std::uint64_t next_ = 1;
};

/**
 * @brief Deterministic monotonic clock advanced only by the test thread.
 *
 * @throws Nothing.
 * @note The clock begins at the steady-clock epoch and never reads wall time.
 */
class ManualClock {
 public:
  /**
   * @brief Returns the current injected timestamp.
   * @return Timestamp protected by the clock mutex.
   * @throws Nothing.
   */
  ComputeRequestRegistry::TimePoint now() noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return now_;
  }

  /**
   * @brief Advances the injected timestamp by a nonnegative duration.
   * @param duration Exact monotonic increment.
   * @return Nothing.
   * @throws std::invalid_argument when asked to move backwards.
   */
  void advance(std::chrono::steady_clock::duration duration) {
    if (duration < std::chrono::steady_clock::duration::zero()) {
      throw std::invalid_argument("manual clock cannot move backwards");
    }
    std::lock_guard<std::mutex> lock(mutex_);
    now_ += duration;
  }

 private:
  /** @brief Serializes injected timestamp reads and advances. */
  std::mutex mutex_;

  /** @brief Current deterministic timestamp. */
  ComputeRequestRegistry::TimePoint now_{};
};

/**
 * @brief Deterministic executor gate with a bounded observation watchdog.
 *
 * @throws Nothing except platform mutex failures.
 * @note The executor calls `enter_and_wait`; the test observes `entered` and
 *       explicitly releases it. No timing sleep establishes ordering.
 */
class StepGate {
 public:
  /**
   * @brief Marks executor entry and blocks until the test releases it.
   * @return Nothing.
   * @throws std::system_error if platform synchronization fails.
   */
  void enter_and_wait() {
    std::unique_lock<std::mutex> lock(mutex_);
    entered_ = true;
    cv_.notify_all();
    cv_.wait(lock, [this] { return released_; });
  }

  /**
   * @brief Waits until the executor reports entry.
   * @return True before the two-second failure watchdog expires.
   * @throws Nothing except platform mutex failures.
   */
  bool wait_until_entered() {
    std::unique_lock<std::mutex> lock(mutex_);
    return cv_.wait_for(lock, 2s, [this] { return entered_; });
  }

  /**
   * @brief Releases every current or future executor wait.
   * @return Nothing.
   * @throws std::system_error if platform synchronization fails.
   */
  void release() {
    std::lock_guard<std::mutex> lock(mutex_);
    released_ = true;
    cv_.notify_all();
  }

 private:
  /** @brief Serializes entry/release state. */
  std::mutex mutex_;

  /** @brief Wakes test and executor waiters. */
  std::condition_variable cv_;

  /** @brief Whether the executor reached the gate. */
  bool entered_ = false;

  /** @brief Whether the executor may leave the gate. */
  bool released_ = false;
};

/**
 * @brief Reusable barrier that releases exactly the configured arrivals.
 *
 * @throws Nothing except platform synchronization failures.
 * @note Used only to align competing submit/release test threads.
 */
class StartBarrier {
 public:
  /**
   * @brief Creates one fixed-size barrier.
   * @param participants Positive number of arrivals required.
   * @throws std::invalid_argument for zero participants.
   */
  explicit StartBarrier(std::size_t participants)
      : participants_(participants) {
    if (participants_ == 0) {
      throw std::invalid_argument("barrier requires participants");
    }
  }

  /**
   * @brief Arrives and waits until every configured participant arrives.
   * @return Nothing.
   * @throws std::system_error if platform synchronization fails.
   */
  void arrive_and_wait() {
    std::unique_lock<std::mutex> lock(mutex_);
    ++arrived_;
    if (arrived_ == participants_) {
      cv_.notify_all();
      return;
    }
    cv_.wait(lock, [this] { return arrived_ == participants_; });
  }

 private:
  /** @brief Required total arrivals. */
  const std::size_t participants_;

  /** @brief Serializes arrival count. */
  std::mutex mutex_;

  /** @brief Wakes participants after the final arrival. */
  std::condition_variable cv_;

  /** @brief Current arrivals. */
  std::size_t arrived_ = 0;
};

/**
 * @brief Commits one deterministic active session for registry tests.
 *
 * @param sessions Session registry under test.
 * @param name Public display name.
 * @param host_value Exact private Host session value.
 * @return Committed opaque session id.
 * @throws std::runtime_error if setup violates a registry invariant.
 */
IpcSessionId activate_session(SessionRegistry* sessions,
                              const std::string& name,
                              const std::string& host_value) {
  IpcResult<IpcSessionId> reserved = sessions->reserve(name);
  if (!reserved.status.ok ||
      !sessions->commit(reserved.value, GraphSessionId{host_value}).ok) {
    throw std::runtime_error("cannot activate deterministic test session");
  }
  return reserved.value;
}

/**
 * @brief Creates one minimally valid Host compute value for a node sequence.
 *
 * @param node Node value used by executor fixtures.
 * @return Request whose session is replaced by registry admission.
 * @throws Nothing.
 */
HostComputeRequest request_for(int node) {
  HostComputeRequest request;
  request.node = NodeId{node};
  return request;
}

/**
 * @brief Waits until one job reaches a requested state without sleeping.
 *
 * @param registry Compute registry under test.
 * @param id Existing compute identity.
 * @param state State whose complete snapshot is required.
 * @return Last successful snapshot, or a default snapshot on watchdog expiry.
 * @throws std::bad_alloc if status snapshot construction cannot allocate.
 * @note The two-second steady-clock deadline is a failure watchdog only.
 */
ComputeRequestSnapshot wait_for_state(ComputeRequestRegistry* registry,
                                      const ComputeRequestId& id,
                                      ComputeRequestState state) {
  const auto deadline = std::chrono::steady_clock::now() + 2s;
  while (std::chrono::steady_clock::now() < deadline) {
    IpcResult<ComputeRequestSnapshot> current = registry->status(id);
    if (current.status.ok && current.value.state == state) {
      return current.value;
    }
    std::this_thread::yield();
  }
  return {};
}

/**
 * @brief Waits until one job reaches either terminal state.
 *
 * @param registry Compute registry under test.
 * @param id Existing compute identity.
 * @return Last complete terminal snapshot, or a default on watchdog expiry.
 * @throws std::bad_alloc if status snapshot construction cannot allocate.
 */
ComputeRequestSnapshot wait_for_terminal(ComputeRequestRegistry* registry,
                                         const ComputeRequestId& id) {
  const auto deadline = std::chrono::steady_clock::now() + 2s;
  while (std::chrono::steady_clock::now() < deadline) {
    IpcResult<ComputeRequestSnapshot> current = registry->status(id);
    if (current.status.ok &&
        (current.value.state == ComputeRequestState::Succeeded ||
         current.value.state == ComputeRequestState::Failed)) {
      return current.value;
    }
    std::this_thread::yield();
  }
  return {};
}

/**
 * @brief Returns a successful empty-output publisher fixture.
 * @return Callable that accepts any image without retaining output ownership.
 * @throws std::bad_alloc if function storage cannot be allocated.
 */
ComputeRequestRegistry::OutputPublisher empty_output_publisher() {
  return [](const ComputeRequestId&, ImageBuffer) {
    return ComputeOutputPublication{ok_status(), {}};
  };
}

TEST(ComputeRequestRegistryConstruction, RejectsInvalidPolicyAndCallbacks) {
  OpaqueSequence sessions_ids(1);
  SessionRegistry sessions([&] { return sessions_ids.next(); });
  const auto status_executor = [](const HostComputeRequest&) {
    return ok_status();
  };
  const auto image_executor = [](const HostComputeRequest&) {
    return Result<ImageBuffer>{ok_status(), {}};
  };

  ComputeRequestRegistryLimits zero_active;
  zero_active.active = 0;
  EXPECT_THROW(ComputeRequestRegistry(sessions, status_executor, image_executor,
                                      empty_output_publisher(), zero_active),
               std::invalid_argument);

  ComputeRequestRegistryLimits zero_terminal;
  zero_terminal.terminal = 0;
  EXPECT_THROW(ComputeRequestRegistry(sessions, status_executor, image_executor,
                                      empty_output_publisher(), zero_terminal),
               std::invalid_argument);

  ComputeRequestRegistryLimits zero_ttl;
  zero_ttl.terminal_ttl = std::chrono::steady_clock::duration::zero();
  EXPECT_THROW(ComputeRequestRegistry(sessions, status_executor, image_executor,
                                      empty_output_publisher(), zero_ttl),
               std::invalid_argument);

  EXPECT_THROW(ComputeRequestRegistry(sessions, {}, image_executor,
                                      empty_output_publisher()),
               std::invalid_argument);
}

TEST(ComputeRequestRegistrySubmission, PublishesQueuedCommitSnapshot) {
  OpaqueSequence sessions_ids(1);
  OpaqueSequence compute_ids(100);
  SessionRegistry sessions([&] { return sessions_ids.next(); });
  const IpcSessionId session =
      activate_session(&sessions, "alpha", "private-alpha");
  StepGate gate;
  ComputeRequestRegistry registry(
      sessions,
      [&](const HostComputeRequest& request) {
        EXPECT_EQ(request.session.value, "private-alpha");
        gate.enter_and_wait();
        return ok_status();
      },
      [](const HostComputeRequest&) {
        return Result<ImageBuffer>{ok_status(), {}};
      },
      empty_output_publisher(), {}, {}, [&] { return compute_ids.next(); });

  EXPECT_FALSE(
      registry.submit(session, request_for(1), ComputeResultMode::Status)
          .status.ok);
  ASSERT_TRUE(registry.start().ok);
  const IpcResult<ComputeRequestSnapshot> submitted =
      registry.submit(session, request_for(1), ComputeResultMode::Status);
  ASSERT_TRUE(submitted.status.ok) << submitted.status.message;
  EXPECT_TRUE(valid_opaque_id(submitted.value.compute_id.value));
  EXPECT_EQ(submitted.value.session_id.value, session.value);
  EXPECT_EQ(submitted.value.state, ComputeRequestState::Queued);
  EXPECT_FALSE(submitted.value.cancellable);
  EXPECT_FALSE(submitted.value.terminal_status.has_value());
  ASSERT_TRUE(gate.wait_until_entered());
  EXPECT_EQ(wait_for_state(&registry, submitted.value.compute_id,
                           ComputeRequestState::Running)
                .state,
            ComputeRequestState::Running);
  gate.release();
  EXPECT_EQ(wait_for_terminal(&registry, submitted.value.compute_id).state,
            ComputeRequestState::Succeeded);
}

TEST(ComputeRequestRegistryExecution, UsesOneWorkerFifoAndMatchingExecutors) {
  OpaqueSequence sessions_ids(1);
  OpaqueSequence compute_ids(100);
  SessionRegistry sessions([&] { return sessions_ids.next(); });
  const IpcSessionId session =
      activate_session(&sessions, "alpha", "private-alpha");
  StepGate first_gate;
  std::mutex events_mutex;
  std::vector<int> events;
  std::thread::id worker_id;
  std::atomic<int> in_flight{0};
  std::atomic<int> maximum_in_flight{0};
  std::atomic<int> status_calls{0};
  std::atomic<int> image_calls{0};

  const auto record_entry = [&](int node) {
    const int active = ++in_flight;
    int prior = maximum_in_flight.load();
    while (active > prior &&
           !maximum_in_flight.compare_exchange_weak(prior, active)) {
    }
    {
      std::lock_guard<std::mutex> lock(events_mutex);
      if (worker_id == std::thread::id{}) {
        worker_id = std::this_thread::get_id();
      } else {
        EXPECT_EQ(worker_id, std::this_thread::get_id());
      }
      events.push_back(node);
    }
  };

  ComputeRequestRegistry registry(
      sessions,
      [&](const HostComputeRequest& request) {
        ++status_calls;
        record_entry(request.node.value);
        if (request.node.value == 1) {
          first_gate.enter_and_wait();
        }
        --in_flight;
        return ok_status();
      },
      [&](const HostComputeRequest& request) {
        ++image_calls;
        record_entry(request.node.value);
        --in_flight;
        return Result<ImageBuffer>{ok_status(), {}};
      },
      empty_output_publisher(), {}, {}, [&] { return compute_ids.next(); });
  ASSERT_TRUE(registry.start().ok);

  const auto first =
      registry.submit(session, request_for(1), ComputeResultMode::Status);
  ASSERT_TRUE(first.status.ok);
  ASSERT_TRUE(first_gate.wait_until_entered());
  const auto second =
      registry.submit(session, request_for(2), ComputeResultMode::Status);
  const auto third =
      registry.submit(session, request_for(3), ComputeResultMode::Image);
  ASSERT_TRUE(second.status.ok);
  ASSERT_TRUE(third.status.ok);
  EXPECT_EQ(registry.status(second.value.compute_id).value.state,
            ComputeRequestState::Queued);
  first_gate.release();

  EXPECT_EQ(wait_for_terminal(&registry, first.value.compute_id).state,
            ComputeRequestState::Succeeded);
  EXPECT_EQ(wait_for_terminal(&registry, second.value.compute_id).state,
            ComputeRequestState::Succeeded);
  EXPECT_EQ(wait_for_terminal(&registry, third.value.compute_id).state,
            ComputeRequestState::Succeeded);
  EXPECT_EQ(status_calls.load(), 2);
  EXPECT_EQ(image_calls.load(), 1);
  EXPECT_EQ(maximum_in_flight.load(), 1);
  std::lock_guard<std::mutex> lock(events_mutex);
  EXPECT_EQ(events, (std::vector<int>{1, 2, 3}));
}

TEST(ComputeRequestRegistryExecution,
     RetainsExactFailureAndContainsWorkerExceptions) {
  OpaqueSequence sessions_ids(1);
  OpaqueSequence compute_ids(100);
  SessionRegistry sessions([&] { return sessions_ids.next(); });
  const IpcSessionId session =
      activate_session(&sessions, "alpha", "private-alpha");
  std::atomic<int> calls{0};
  ComputeRequestRegistry registry(
      sessions,
      [&](const HostComputeRequest& request) {
        ++calls;
        if (request.node.value == 1) {
          return failure_status(
              OperationErrorDomain::Graph,
              static_cast<std::int32_t>(GraphErrc::ComputeError),
              "compute_error", "exact graph diagnostic");
        }
        if (request.node.value == 2) {
          throw std::runtime_error("executor runtime failure");
        }
        if (request.node.value == 3) {
          throw std::bad_alloc();
        }
        return ok_status();
      },
      [](const HostComputeRequest&) {
        return Result<ImageBuffer>{ok_status(), {}};
      },
      empty_output_publisher(), {}, {}, [&] { return compute_ids.next(); });
  ASSERT_TRUE(registry.start().ok);

  std::vector<ComputeRequestId> ids;
  for (int node = 1; node <= 4; ++node) {
    const auto submitted =
        registry.submit(session, request_for(node), ComputeResultMode::Status);
    ASSERT_TRUE(submitted.status.ok);
    ids.push_back(submitted.value.compute_id);
  }
  const ComputeRequestSnapshot graph_failure =
      wait_for_terminal(&registry, ids[0]);
  ASSERT_EQ(graph_failure.state, ComputeRequestState::Failed);
  ASSERT_TRUE(graph_failure.terminal_status.has_value());
  EXPECT_EQ(graph_failure.terminal_status->domain, OperationErrorDomain::Graph);
  EXPECT_EQ(graph_failure.terminal_status->code,
            static_cast<std::int32_t>(GraphErrc::ComputeError));
  EXPECT_EQ(graph_failure.terminal_status->name, "compute_error");
  EXPECT_EQ(graph_failure.terminal_status->message, "exact graph diagnostic");

  for (std::size_t index : {1U, 2U}) {
    const ComputeRequestSnapshot failed =
        wait_for_terminal(&registry, ids[index]);
    ASSERT_EQ(failed.state, ComputeRequestState::Failed);
    ASSERT_TRUE(failed.terminal_status.has_value());
    EXPECT_EQ(failed.terminal_status->domain, OperationErrorDomain::Daemon);
    EXPECT_EQ(failed.terminal_status->code, kInternalErrorCode);
    EXPECT_EQ(failed.terminal_status->name, "internal_error");
  }
  EXPECT_EQ(wait_for_terminal(&registry, ids[3]).state,
            ComputeRequestState::Succeeded);
  EXPECT_EQ(calls.load(), 4);
}

TEST(ComputeRequestRegistryExecution,
     ContainsExactOutputFailureAndContinuesAfterPublisherException) {
  OpaqueSequence sessions_ids(1);
  OpaqueSequence compute_ids(100);
  SessionRegistry sessions([&] { return sessions_ids.next(); });
  const IpcSessionId session =
      activate_session(&sessions, "alpha", "private-alpha");
  std::atomic<int> publications{0};
  ComputeRequestRegistry registry(
      sessions, [](const HostComputeRequest&) { return ok_status(); },
      [](const HostComputeRequest&) {
        ImageBuffer image;
        image.width = 1;
        return Result<ImageBuffer>{ok_status(), std::move(image)};
      },
      [&](const ComputeRequestId&, ImageBuffer) {
        const int publication = ++publications;
        if (publication == 1) {
          return ComputeOutputPublication{
              failure_status(
                  OperationErrorDomain::Daemon, kArtifactLimitExceededCode,
                  "artifact_limit_exceeded", "exact output quota diagnostic"),
              {}};
        }
        if (publication == 2) {
          throw std::runtime_error("publisher failure");
        }
        return ComputeOutputPublication{ok_status(), {}};
      },
      {}, {}, [&] { return compute_ids.next(); });
  ASSERT_TRUE(registry.start().ok);

  std::vector<ComputeRequestId> ids;
  for (int node = 1; node <= 3; ++node) {
    const auto submitted =
        registry.submit(session, request_for(node), ComputeResultMode::Image);
    ASSERT_TRUE(submitted.status.ok);
    ids.push_back(submitted.value.compute_id);
  }
  const ComputeRequestSnapshot quota_failure =
      wait_for_terminal(&registry, ids[0]);
  ASSERT_EQ(quota_failure.state, ComputeRequestState::Failed);
  ASSERT_TRUE(quota_failure.terminal_status.has_value());
  EXPECT_EQ(quota_failure.terminal_status->domain,
            OperationErrorDomain::Daemon);
  EXPECT_EQ(quota_failure.terminal_status->code, kArtifactLimitExceededCode);
  EXPECT_EQ(quota_failure.terminal_status->name, "artifact_limit_exceeded");
  EXPECT_EQ(quota_failure.terminal_status->message,
            "exact output quota diagnostic");

  const ComputeRequestSnapshot publisher_exception =
      wait_for_terminal(&registry, ids[1]);
  ASSERT_EQ(publisher_exception.state, ComputeRequestState::Failed);
  ASSERT_TRUE(publisher_exception.terminal_status.has_value());
  EXPECT_EQ(publisher_exception.terminal_status->code, kInternalErrorCode);
  EXPECT_EQ(wait_for_terminal(&registry, ids[2]).state,
            ComputeRequestState::Succeeded);
  EXPECT_EQ(publications.load(), 3);
}

TEST(ComputeRequestRegistryCapacity, EnforcesGlobalActiveLimitBeforeExecution) {
  OpaqueSequence sessions_ids(1);
  OpaqueSequence compute_ids(100);
  SessionRegistry sessions([&] { return sessions_ids.next(); });
  const IpcSessionId alpha =
      activate_session(&sessions, "alpha", "private-alpha");
  const IpcSessionId beta = activate_session(&sessions, "beta", "private-beta");
  StepGate first_gate;
  std::atomic<int> calls{0};
  ComputeRequestRegistryLimits limits;
  limits.active = 2;
  ComputeRequestRegistry registry(
      sessions,
      [&](const HostComputeRequest& request) {
        ++calls;
        if (request.node.value == 1) {
          first_gate.enter_and_wait();
        }
        return ok_status();
      },
      [](const HostComputeRequest&) {
        return Result<ImageBuffer>{ok_status(), {}};
      },
      empty_output_publisher(), limits, {}, [&] { return compute_ids.next(); });
  ASSERT_TRUE(registry.start().ok);

  const auto first =
      registry.submit(alpha, request_for(1), ComputeResultMode::Status);
  ASSERT_TRUE(first.status.ok);
  ASSERT_TRUE(first_gate.wait_until_entered());
  const auto second =
      registry.submit(beta, request_for(2), ComputeResultMode::Status);
  ASSERT_TRUE(second.status.ok);
  const auto rejected =
      registry.submit(alpha, request_for(3), ComputeResultMode::Status);
  EXPECT_FALSE(rejected.status.ok);
  EXPECT_EQ(rejected.status.domain, OperationErrorDomain::Daemon);
  EXPECT_EQ(rejected.status.code, kCapacityExceededCode);
  EXPECT_EQ(calls.load(), 1);
  first_gate.release();
  EXPECT_EQ(wait_for_terminal(&registry, second.value.compute_id).state,
            ComputeRequestState::Succeeded);
  EXPECT_EQ(calls.load(), 2);
}

TEST(ComputeRequestRegistryCapacity, ConcurrentSubmitSharesTheLastSlot) {
  OpaqueSequence sessions_ids(1);
  OpaqueSequence compute_ids(100);
  SessionRegistry sessions([&] { return sessions_ids.next(); });
  const IpcSessionId session =
      activate_session(&sessions, "alpha", "private-alpha");
  StepGate executor_gate;
  StartBarrier barrier(2);
  ComputeRequestRegistryLimits limits;
  limits.active = 1;
  ComputeRequestRegistry registry(
      sessions,
      [&](const HostComputeRequest&) {
        executor_gate.enter_and_wait();
        return ok_status();
      },
      [](const HostComputeRequest&) {
        return Result<ImageBuffer>{ok_status(), {}};
      },
      empty_output_publisher(), limits, {}, [&] { return compute_ids.next(); });
  ASSERT_TRUE(registry.start().ok);

  IpcResult<ComputeRequestSnapshot> left;
  IpcResult<ComputeRequestSnapshot> right;
  std::thread left_thread([&] {
    barrier.arrive_and_wait();
    left = registry.submit(session, request_for(1), ComputeResultMode::Status);
  });
  std::thread right_thread([&] {
    barrier.arrive_and_wait();
    right = registry.submit(session, request_for(2), ComputeResultMode::Status);
  });
  left_thread.join();
  right_thread.join();
  EXPECT_NE(left.status.ok, right.status.ok);
  EXPECT_EQ(left.status.ok ? right.status.code : left.status.code,
            kCapacityExceededCode);
  ASSERT_TRUE(executor_gate.wait_until_entered());
  executor_gate.release();
  const ComputeRequestId accepted =
      left.status.ok ? left.value.compute_id : right.value.compute_id;
  EXPECT_EQ(wait_for_terminal(&registry, accepted).state,
            ComputeRequestState::Succeeded);
}

TEST(ComputeRequestRegistryRetention, EvictsOnlyOldestTerminalRecord) {
  OpaqueSequence sessions_ids(1);
  OpaqueSequence compute_ids(100);
  SessionRegistry sessions([&] { return sessions_ids.next(); });
  const IpcSessionId session =
      activate_session(&sessions, "alpha", "private-alpha");
  ComputeRequestRegistryLimits limits;
  limits.terminal = 2;
  ComputeRequestRegistry registry(
      sessions, [](const HostComputeRequest&) { return ok_status(); },
      [](const HostComputeRequest&) {
        return Result<ImageBuffer>{ok_status(), {}};
      },
      empty_output_publisher(), limits, {}, [&] { return compute_ids.next(); });
  ASSERT_TRUE(registry.start().ok);

  std::vector<ComputeRequestId> ids;
  for (int node = 1; node <= 3; ++node) {
    const auto submitted =
        registry.submit(session, request_for(node), ComputeResultMode::Status);
    ASSERT_TRUE(submitted.status.ok);
    ids.push_back(submitted.value.compute_id);
    ASSERT_EQ(wait_for_terminal(&registry, ids.back()).state,
              ComputeRequestState::Succeeded);
  }
  const auto evicted = registry.status(ids[0]);
  EXPECT_FALSE(evicted.status.ok);
  EXPECT_EQ(evicted.status.code, kJobNotFoundCode);
  EXPECT_TRUE(registry.status(ids[1]).status.ok);
  EXPECT_TRUE(registry.status(ids[2]).status.ok);
}

TEST(ComputeRequestRegistryLookup, ResultAndReleasePreserveImmutableTerminal) {
  OpaqueSequence sessions_ids(1);
  OpaqueSequence compute_ids(100);
  SessionRegistry sessions([&] { return sessions_ids.next(); });
  const IpcSessionId session =
      activate_session(&sessions, "alpha", "private-alpha");
  StepGate gate;
  ComputeRequestRegistry registry(
      sessions,
      [&](const HostComputeRequest&) {
        gate.enter_and_wait();
        return failure_status(
            OperationErrorDomain::Graph,
            static_cast<std::int32_t>(GraphErrc::ComputeError), "compute_error",
            "immutable diagnostic");
      },
      [](const HostComputeRequest&) {
        return Result<ImageBuffer>{ok_status(), {}};
      },
      empty_output_publisher(), {}, {}, [&] { return compute_ids.next(); });
  ASSERT_TRUE(registry.start().ok);
  const auto submitted =
      registry.submit(session, request_for(1), ComputeResultMode::Status);
  ASSERT_TRUE(submitted.status.ok);
  ASSERT_TRUE(gate.wait_until_entered());

  EXPECT_EQ(registry.result(submitted.value.compute_id).status.code,
            kJobNotReadyCode);
  EXPECT_EQ(registry.release(submitted.value.compute_id).code,
            kJobNotReadyCode);
  gate.release();
  ASSERT_EQ(wait_for_terminal(&registry, submitted.value.compute_id).state,
            ComputeRequestState::Failed);

  auto first = registry.result(submitted.value.compute_id);
  ASSERT_TRUE(first.status.ok);
  ASSERT_TRUE(first.value.terminal_status.has_value());
  first.value.terminal_status->message = "caller mutation";
  const auto second = registry.result(submitted.value.compute_id);
  ASSERT_TRUE(second.status.ok);
  ASSERT_TRUE(second.value.terminal_status.has_value());
  EXPECT_EQ(second.value.terminal_status->message, "immutable diagnostic");
  EXPECT_TRUE(registry.release(submitted.value.compute_id).ok);
  EXPECT_EQ(registry.status(submitted.value.compute_id).status.code,
            kJobNotFoundCode);
  EXPECT_EQ(registry.release(submitted.value.compute_id).code,
            kJobNotFoundCode);
}

TEST(ComputeRequestRegistryRetention, UsesTerminalTimeAndDoesNotRefreshTtl) {
  OpaqueSequence sessions_ids(1);
  OpaqueSequence compute_ids(100);
  auto clock = std::make_shared<ManualClock>();
  SessionRegistry sessions([&] { return sessions_ids.next(); });
  const IpcSessionId session =
      activate_session(&sessions, "alpha", "private-alpha");
  ComputeRequestRegistryLimits limits;
  limits.terminal_ttl = 10s;
  ComputeRequestRegistry registry(
      sessions, [](const HostComputeRequest&) { return ok_status(); },
      [](const HostComputeRequest&) {
        return Result<ImageBuffer>{ok_status(), {}};
      },
      empty_output_publisher(), limits, [clock] { return clock->now(); },
      [&] { return compute_ids.next(); });
  ASSERT_TRUE(registry.start().ok);
  const auto submitted =
      registry.submit(session, request_for(1), ComputeResultMode::Status);
  ASSERT_TRUE(submitted.status.ok);
  ASSERT_EQ(wait_for_terminal(&registry, submitted.value.compute_id).state,
            ComputeRequestState::Succeeded);

  clock->advance(9s);
  EXPECT_TRUE(registry.status(submitted.value.compute_id).status.ok);
  EXPECT_TRUE(registry.result(submitted.value.compute_id).status.ok);
  clock->advance(1s);
  EXPECT_EQ(registry.status(submitted.value.compute_id).status.code,
            kJobNotFoundCode);
}

TEST(ComputeRequestRegistryRetention, NeverExpiresActiveWork) {
  OpaqueSequence sessions_ids(1);
  OpaqueSequence compute_ids(100);
  auto clock = std::make_shared<ManualClock>();
  SessionRegistry sessions([&] { return sessions_ids.next(); });
  const IpcSessionId session =
      activate_session(&sessions, "alpha", "private-alpha");
  StepGate gate;
  ComputeRequestRegistryLimits limits;
  limits.terminal_ttl = 1s;
  ComputeRequestRegistry registry(
      sessions,
      [&](const HostComputeRequest&) {
        gate.enter_and_wait();
        return ok_status();
      },
      [](const HostComputeRequest&) {
        return Result<ImageBuffer>{ok_status(), {}};
      },
      empty_output_publisher(), limits, [clock] { return clock->now(); },
      [&] { return compute_ids.next(); });
  ASSERT_TRUE(registry.start().ok);
  const auto submitted =
      registry.submit(session, request_for(1), ComputeResultMode::Status);
  ASSERT_TRUE(submitted.status.ok);
  ASSERT_TRUE(gate.wait_until_entered());
  clock->advance(1h);
  EXPECT_EQ(registry.cleanup_expired(), 0U);
  EXPECT_EQ(registry.status(submitted.value.compute_id).value.state,
            ComputeRequestState::Running);
  gate.release();
  ASSERT_EQ(wait_for_terminal(&registry, submitted.value.compute_id).state,
            ComputeRequestState::Succeeded);
  clock->advance(1s);
  EXPECT_EQ(registry.status(submitted.value.compute_id).status.code,
            kJobNotFoundCode);
}

TEST(ComputeRequestRegistryOutput,
     EvictionCleanupPrecedesReplacementTerminalPublication) {
  OpaqueSequence sessions_ids(1);
  OpaqueSequence compute_ids(100);
  SessionRegistry sessions([&] { return sessions_ids.next(); });
  const IpcSessionId session =
      activate_session(&sessions, "alpha", "private-alpha");
  StepGate replacement_executor_gate;
  StepGate cleanup_gate;
  std::promise<void> cleanup_finished;
  std::future<void> cleanup_finished_future = cleanup_finished.get_future();
  std::atomic<bool> inspect_eviction{false};
  std::atomic<bool> evicted_absent{false};
  std::atomic<bool> replacement_found{false};
  std::atomic<ComputeRequestState> replacement_state{
      ComputeRequestState::Queued};
  std::atomic<int> cleaned{0};
  int published = 0;
  ComputeRequestId evicted_id;
  ComputeRequestId replacement_id;
  ComputeRequestRegistry* registry_ptr = nullptr;
  ComputeRequestRegistryLimits limits;
  limits.terminal = 1;
  ComputeRequestRegistry registry(
      sessions, [](const HostComputeRequest&) { return ok_status(); },
      [&](const HostComputeRequest& request) {
        if (request.node.value == 2) {
          replacement_executor_gate.enter_and_wait();
        }
        ImageBuffer image;
        image.width = 1;
        return Result<ImageBuffer>{ok_status(), std::move(image)};
      },
      [&](const ComputeRequestId&, ImageBuffer) {
        const int sequence = ++published;
        return ComputeOutputPublication{
            ok_status(),
            ComputeOutputOwnership(
                "linearized-output-" + std::to_string(sequence),
                [&, sequence](const std::optional<std::string>&) {
                  const bool inspect =
                      sequence == 1 &&
                      inspect_eviction.load(std::memory_order_acquire);
                  if (inspect) {
                    const auto evicted = registry_ptr->status(evicted_id);
                    evicted_absent.store(
                        !evicted.status.ok &&
                            evicted.status.code == kJobNotFoundCode,
                        std::memory_order_release);
                    const auto replacement =
                        registry_ptr->status(replacement_id);
                    replacement_found.store(replacement.status.ok,
                                            std::memory_order_release);
                    if (replacement.status.ok) {
                      replacement_state.store(replacement.value.state,
                                              std::memory_order_release);
                    }
                    cleanup_gate.enter_and_wait();
                  }
                  ++cleaned;
                  if (inspect) {
                    cleanup_finished.set_value();
                  }
                })};
      },
      limits, {}, [&] { return compute_ids.next(); });
  registry_ptr = &registry;
  ASSERT_TRUE(registry.start().ok);

  const auto first =
      registry.submit(session, request_for(1), ComputeResultMode::Image);
  ASSERT_TRUE(first.status.ok);
  evicted_id = first.value.compute_id;
  ASSERT_EQ(wait_for_terminal(&registry, evicted_id).state,
            ComputeRequestState::Succeeded);

  const auto second =
      registry.submit(session, request_for(2), ComputeResultMode::Image);
  ASSERT_TRUE(second.status.ok);
  replacement_id = second.value.compute_id;
  const bool replacement_entered =
      replacement_executor_gate.wait_until_entered();
  EXPECT_TRUE(replacement_entered);
  if (!replacement_entered) {
    replacement_executor_gate.release();
    return;
  }
  inspect_eviction.store(true, std::memory_order_release);
  replacement_executor_gate.release();

  const bool cleanup_entered = cleanup_gate.wait_until_entered();
  EXPECT_TRUE(cleanup_entered);
  if (cleanup_entered) {
    const auto visible_replacement = registry.status(replacement_id);
    EXPECT_TRUE(visible_replacement.status.ok);
    if (visible_replacement.status.ok) {
      EXPECT_EQ(visible_replacement.value.state, ComputeRequestState::Running);
    }
    EXPECT_TRUE(evicted_absent.load(std::memory_order_acquire));
    EXPECT_TRUE(replacement_found.load(std::memory_order_acquire));
    EXPECT_EQ(replacement_state.load(std::memory_order_acquire),
              ComputeRequestState::Running);
    EXPECT_EQ(cleaned.load(), 0);
  }
  cleanup_gate.release();
  EXPECT_EQ(cleanup_finished_future.wait_for(2s), std::future_status::ready);
  EXPECT_EQ(wait_for_terminal(&registry, replacement_id).state,
            ComputeRequestState::Succeeded);
  EXPECT_EQ(cleaned.load(), 1);
}

TEST(ComputeRequestRegistryOutput,
     CleansExactlyOnceOnEvictionReleaseAndExpiry) {
  OpaqueSequence sessions_ids(1);
  OpaqueSequence compute_ids(100);
  auto clock = std::make_shared<ManualClock>();
  SessionRegistry sessions([&] { return sessions_ids.next(); });
  const IpcSessionId session =
      activate_session(&sessions, "alpha", "private-alpha");
  std::atomic<int> published{0};
  std::atomic<int> cleaned{0};
  ComputeRequestRegistryLimits limits;
  limits.terminal = 1;
  limits.terminal_ttl = 1s;
  ComputeRequestRegistry registry(
      sessions, [](const HostComputeRequest&) { return ok_status(); },
      [](const HostComputeRequest&) {
        ImageBuffer image;
        image.width = 1;
        return Result<ImageBuffer>{ok_status(), std::move(image)};
      },
      [&](const ComputeRequestId&, ImageBuffer) {
        const int sequence = ++published;
        return ComputeOutputPublication{
            ok_status(),
            ComputeOutputOwnership(
                "output-" + std::to_string(sequence),
                [&](const std::optional<std::string>&) { ++cleaned; })};
      },
      limits, [clock] { return clock->now(); },
      [&] { return compute_ids.next(); });
  ASSERT_TRUE(registry.start().ok);

  const auto first =
      registry.submit(session, request_for(1), ComputeResultMode::Image);
  ASSERT_TRUE(first.status.ok);
  ASSERT_EQ(wait_for_terminal(&registry, first.value.compute_id).state,
            ComputeRequestState::Succeeded);
  const auto second =
      registry.submit(session, request_for(2), ComputeResultMode::Image);
  ASSERT_TRUE(second.status.ok);
  ASSERT_EQ(wait_for_terminal(&registry, second.value.compute_id).state,
            ComputeRequestState::Succeeded);
  EXPECT_EQ(cleaned.load(), 1);
  EXPECT_TRUE(registry.release(second.value.compute_id).ok);
  EXPECT_EQ(cleaned.load(), 2);

  const auto third =
      registry.submit(session, request_for(3), ComputeResultMode::Image);
  ASSERT_TRUE(third.status.ok);
  ASSERT_EQ(wait_for_terminal(&registry, third.value.compute_id).state,
            ComputeRequestState::Succeeded);
  clock->advance(1s);
  EXPECT_EQ(registry.cleanup_expired(), 1U);
  EXPECT_EQ(cleaned.load(), 3);

  const auto fourth =
      registry.submit(session, request_for(4), ComputeResultMode::Image);
  ASSERT_TRUE(fourth.status.ok);
  ASSERT_EQ(wait_for_terminal(&registry, fourth.value.compute_id).state,
            ComputeRequestState::Succeeded);
  registry.shutdown();
  EXPECT_EQ(cleaned.load(), 4);
  registry.shutdown();
  EXPECT_EQ(cleaned.load(), 4);
}

TEST(SessionRegistryLifecycle, CloseWaitsOrdinaryAdmissionAndRejectsNewWork) {
  OpaqueSequence sessions_ids(1);
  SessionRegistry sessions([&] { return sessions_ids.next(); });
  const IpcSessionId session =
      activate_session(&sessions, "alpha", "private-alpha");
  auto admitted = sessions.admit_host_call(session);
  ASSERT_TRUE(admitted.status.ok);

  auto closing = std::async(std::launch::async,
                            [&] { return sessions.begin_close(session); });
  const auto deadline = std::chrono::steady_clock::now() + 2s;
  while (std::chrono::steady_clock::now() < deadline &&
         sessions.admit_host_call(session).status.ok) {
    std::this_thread::yield();
  }
  EXPECT_FALSE(sessions.admit_host_call(session).status.ok);
  EXPECT_EQ(closing.wait_for(0s), std::future_status::timeout);
  admitted.value = {};
  ASSERT_EQ(closing.wait_for(2s), std::future_status::ready);
  auto claim = closing.get();
  ASSERT_TRUE(claim.status.ok);
  EXPECT_EQ(claim.value.host_session().value, "private-alpha");
  claim.value.erase();
  EXPECT_FALSE(sessions.admit_host_call(session).status.ok);
}

TEST(SessionRegistryLifecycle, RetryableCloseReopensAndSuccessfulCloseErases) {
  OpaqueSequence sessions_ids(1);
  SessionRegistry sessions([&] { return sessions_ids.next(); });
  const IpcSessionId session =
      activate_session(&sessions, "alpha", "private-alpha");

  {
    auto claim = sessions.begin_close(session);
    ASSERT_TRUE(claim.status.ok);
    EXPECT_FALSE(sessions.admit_host_call(session).status.ok);
  }
  EXPECT_TRUE(sessions.admit_host_call(session).status.ok);
  {
    auto claim = sessions.begin_close(session);
    ASSERT_TRUE(claim.status.ok);
    claim.value.reopen();
  }
  EXPECT_TRUE(sessions.admit_host_call(session).status.ok);
  {
    auto claim = sessions.begin_close(session);
    ASSERT_TRUE(claim.status.ok);
    claim.value.erase();
  }
  EXPECT_FALSE(sessions.admit_host_call(session).status.ok);
}

TEST(SessionRegistryLifecycle, GlobalStopRejectsAndRestartRestoresAdmission) {
  OpaqueSequence sessions_ids(1);
  SessionRegistry sessions([&] { return sessions_ids.next(); });
  const IpcSessionId session =
      activate_session(&sessions, "alpha", "private-alpha");

  sessions.stop_admission();
  EXPECT_FALSE(sessions.admit_host_call(session).status.ok);
  EXPECT_FALSE(sessions.admit_job(session).status.ok);
  EXPECT_FALSE(sessions.begin_close(session).status.ok);
  EXPECT_FALSE(sessions.reserve("beta").status.ok);

  sessions.start_admission();
  EXPECT_TRUE(sessions.admit_host_call(session).status.ok);
  const IpcSessionId beta = activate_session(&sessions, "beta", "private-beta");
  EXPECT_TRUE(sessions.admit_host_call(beta).status.ok);
}

TEST(ComputeRequestRegistryLifecycle, CloseWaitsQueuedJobWithoutHostDeadlock) {
  OpaqueSequence sessions_ids(1);
  OpaqueSequence compute_ids(100);
  SessionRegistry sessions([&] { return sessions_ids.next(); });
  const IpcSessionId blocker =
      activate_session(&sessions, "blocker", "private-blocker");
  const IpcSessionId target =
      activate_session(&sessions, "target", "private-target");
  StepGate blocker_gate;
  ComputeRequestRegistry registry(
      sessions,
      [&](const HostComputeRequest& request) {
        if (request.node.value == 1) {
          blocker_gate.enter_and_wait();
        }
        return ok_status();
      },
      [](const HostComputeRequest&) {
        return Result<ImageBuffer>{ok_status(), {}};
      },
      empty_output_publisher(), {}, {}, [&] { return compute_ids.next(); });
  ASSERT_TRUE(registry.start().ok);
  const auto first =
      registry.submit(blocker, request_for(1), ComputeResultMode::Status);
  ASSERT_TRUE(first.status.ok);
  ASSERT_TRUE(blocker_gate.wait_until_entered());
  const auto queued =
      registry.submit(target, request_for(2), ComputeResultMode::Status);
  ASSERT_TRUE(queued.status.ok);

  auto closing = std::async(std::launch::async,
                            [&] { return sessions.begin_close(target); });
  const auto deadline = std::chrono::steady_clock::now() + 2s;
  while (std::chrono::steady_clock::now() < deadline &&
         sessions.admit_job(target).status.ok) {
    std::this_thread::yield();
  }
  EXPECT_FALSE(sessions.admit_job(target).status.ok);
  const auto rejected =
      registry.submit(target, request_for(3), ComputeResultMode::Status);
  EXPECT_FALSE(rejected.status.ok);
  EXPECT_EQ(rejected.status.domain, OperationErrorDomain::Graph);
  EXPECT_EQ(rejected.status.code,
            static_cast<std::int32_t>(GraphErrc::NotFound));
  EXPECT_EQ(closing.wait_for(0s), std::future_status::timeout);
  blocker_gate.release();
  EXPECT_EQ(wait_for_terminal(&registry, queued.value.compute_id).state,
            ComputeRequestState::Succeeded);
  ASSERT_EQ(closing.wait_for(2s), std::future_status::ready);
  auto claim = closing.get();
  ASSERT_TRUE(claim.status.ok);
  claim.value.erase();
  EXPECT_TRUE(registry.result(queued.value.compute_id).status.ok);
}

TEST(ComputeRequestRegistryLifecycle,
     SimultaneousSubmitAndCloseHaveOneAtomicSessionWinner) {
  OpaqueSequence sessions_ids(1);
  OpaqueSequence compute_ids(100);
  SessionRegistry sessions([&] { return sessions_ids.next(); });
  const IpcSessionId session =
      activate_session(&sessions, "alpha", "private-alpha");
  StepGate executor_gate;
  std::atomic<int> executor_calls{0};
  ComputeRequestRegistry registry(
      sessions,
      [&](const HostComputeRequest&) {
        ++executor_calls;
        executor_gate.enter_and_wait();
        return ok_status();
      },
      [](const HostComputeRequest&) {
        return Result<ImageBuffer>{ok_status(), {}};
      },
      empty_output_publisher(), {}, {}, [&] { return compute_ids.next(); });
  ASSERT_TRUE(registry.start().ok);
  StartBarrier barrier(2);

  auto submitting = std::async(std::launch::async, [&] {
    barrier.arrive_and_wait();
    return registry.submit(session, request_for(1), ComputeResultMode::Status);
  });
  auto closing = std::async(std::launch::async, [&] {
    barrier.arrive_and_wait();
    return sessions.begin_close(session);
  });
  ASSERT_EQ(submitting.wait_for(2s), std::future_status::ready);
  IpcResult<ComputeRequestSnapshot> submitted = submitting.get();
  if (submitted.status.ok) {
    ASSERT_TRUE(executor_gate.wait_until_entered());
    EXPECT_EQ(closing.wait_for(0s), std::future_status::timeout);
    executor_gate.release();
    EXPECT_EQ(wait_for_terminal(&registry, submitted.value.compute_id).state,
              ComputeRequestState::Succeeded);
    EXPECT_EQ(executor_calls.load(), 1);
  } else {
    EXPECT_EQ(submitted.status.domain, OperationErrorDomain::Graph);
    EXPECT_EQ(submitted.status.code,
              static_cast<std::int32_t>(GraphErrc::NotFound));
    EXPECT_EQ(executor_calls.load(), 0);
  }

  ASSERT_EQ(closing.wait_for(2s), std::future_status::ready);
  auto first_close = closing.get();
  ASSERT_TRUE(first_close.status.ok);
  first_close.value.reopen();
  auto count_probe = sessions.begin_close(session);
  ASSERT_TRUE(count_probe.status.ok);
  count_probe.value.erase();
}

TEST(ComputeRequestRegistryLifecycle,
     ShutdownRejectsNewWorkDrainsAndSupportsRestart) {
  OpaqueSequence sessions_ids(1);
  OpaqueSequence compute_ids(100);
  SessionRegistry sessions([&] { return sessions_ids.next(); });
  const IpcSessionId session =
      activate_session(&sessions, "alpha", "private-alpha");
  StepGate first_gate;
  std::atomic<int> calls{0};
  ComputeRequestRegistry registry(
      sessions,
      [&](const HostComputeRequest& request) {
        ++calls;
        if (request.node.value == 1) {
          first_gate.enter_and_wait();
        }
        return ok_status();
      },
      [](const HostComputeRequest&) {
        return Result<ImageBuffer>{ok_status(), {}};
      },
      empty_output_publisher(), {}, {}, [&] { return compute_ids.next(); });
  ASSERT_TRUE(registry.start().ok);
  const auto first =
      registry.submit(session, request_for(1), ComputeResultMode::Status);
  ASSERT_TRUE(first.status.ok);
  ASSERT_TRUE(first_gate.wait_until_entered());
  const auto second =
      registry.submit(session, request_for(2), ComputeResultMode::Status);
  ASSERT_TRUE(second.status.ok);

  registry.stop_admission();
  auto draining =
      std::async(std::launch::async, [&] { registry.drain_and_join(); });
  const IpcResult<ComputeRequestSnapshot> rejected =
      registry.submit(session, request_for(3), ComputeResultMode::Status);
  EXPECT_FALSE(rejected.status.ok);
  EXPECT_EQ(draining.wait_for(0s), std::future_status::timeout);
  first_gate.release();
  ASSERT_EQ(draining.wait_for(2s), std::future_status::ready);
  draining.get();
  EXPECT_EQ(wait_for_terminal(&registry, first.value.compute_id).state,
            ComputeRequestState::Succeeded);
  EXPECT_EQ(wait_for_terminal(&registry, second.value.compute_id).state,
            ComputeRequestState::Succeeded);
  EXPECT_EQ(calls.load(), 2);

  ASSERT_TRUE(registry.start().ok);
  const auto restarted =
      registry.submit(session, request_for(4), ComputeResultMode::Status);
  ASSERT_TRUE(restarted.status.ok);
  EXPECT_EQ(wait_for_terminal(&registry, restarted.value.compute_id).state,
            ComputeRequestState::Succeeded);
  registry.shutdown();
  registry.shutdown();
}

TEST(ComputeRequestRegistryLifecycle, ConcurrentShutdownSharesOneWorkerJoin) {
  OpaqueSequence sessions_ids(1);
  OpaqueSequence compute_ids(100);
  SessionRegistry sessions([&] { return sessions_ids.next(); });
  const IpcSessionId session =
      activate_session(&sessions, "alpha", "private-alpha");
  StepGate executor_gate;
  std::atomic<int> calls{0};
  ComputeRequestRegistry registry(
      sessions,
      [&](const HostComputeRequest&) {
        ++calls;
        executor_gate.enter_and_wait();
        return ok_status();
      },
      [](const HostComputeRequest&) {
        return Result<ImageBuffer>{ok_status(), {}};
      },
      empty_output_publisher(), {}, {}, [&] { return compute_ids.next(); });
  ASSERT_TRUE(registry.start().ok);
  const auto submitted =
      registry.submit(session, request_for(1), ComputeResultMode::Status);
  ASSERT_TRUE(submitted.status.ok);
  ASSERT_TRUE(executor_gate.wait_until_entered());
  StartBarrier barrier(2);

  auto left = std::async(std::launch::async, [&] {
    barrier.arrive_and_wait();
    registry.shutdown();
  });
  auto right = std::async(std::launch::async, [&] {
    barrier.arrive_and_wait();
    registry.shutdown();
  });
  EXPECT_EQ(left.wait_for(0s), std::future_status::timeout);
  EXPECT_EQ(right.wait_for(0s), std::future_status::timeout);
  executor_gate.release();
  EXPECT_EQ(left.wait_for(2s), std::future_status::ready);
  EXPECT_EQ(right.wait_for(2s), std::future_status::ready);
  left.get();
  right.get();
  EXPECT_EQ(calls.load(), 1);
  EXPECT_EQ(registry.status(submitted.value.compute_id).status.code,
            kJobNotFoundCode);
  ASSERT_TRUE(registry.start().ok);
  registry.shutdown();
}

TEST(ComputeRequestRegistryIdentity,
     RetriesCollisionAndRollsBackMalformedAdmission) {
  OpaqueSequence sessions_ids(1);
  const std::string first_id = opaque_value(100);
  const std::string second_id = opaque_value(101);
  OpaqueSequence compute_ids({first_id, first_id, second_id, "malformed"}, 200);
  SessionRegistry sessions([&] { return sessions_ids.next(); });
  const IpcSessionId session =
      activate_session(&sessions, "alpha", "private-alpha");
  ComputeRequestRegistry registry(
      sessions, [](const HostComputeRequest&) { return ok_status(); },
      [](const HostComputeRequest&) {
        return Result<ImageBuffer>{ok_status(), {}};
      },
      empty_output_publisher(), {}, {}, [&] { return compute_ids.next(); });
  ASSERT_TRUE(registry.start().ok);

  const auto first =
      registry.submit(session, request_for(1), ComputeResultMode::Status);
  ASSERT_TRUE(first.status.ok);
  EXPECT_EQ(first.value.compute_id.value, first_id);
  const auto second =
      registry.submit(session, request_for(2), ComputeResultMode::Status);
  ASSERT_TRUE(second.status.ok);
  EXPECT_EQ(second.value.compute_id.value, second_id);
  const auto malformed =
      registry.submit(session, request_for(3), ComputeResultMode::Status);
  EXPECT_FALSE(malformed.status.ok);
  EXPECT_EQ(malformed.status.code, kInternalErrorCode);

  ASSERT_EQ(wait_for_terminal(&registry, first.value.compute_id).state,
            ComputeRequestState::Succeeded);
  ASSERT_EQ(wait_for_terminal(&registry, second.value.compute_id).state,
            ComputeRequestState::Succeeded);
  auto claim = sessions.begin_close(session);
  ASSERT_TRUE(claim.status.ok);
  claim.value.erase();
}

TEST(ComputeRequestRegistryRace, ReleaseAndExpiryCleanOutputOnce) {
  OpaqueSequence sessions_ids(1);
  OpaqueSequence compute_ids(100);
  auto clock = std::make_shared<ManualClock>();
  SessionRegistry sessions([&] { return sessions_ids.next(); });
  const IpcSessionId session =
      activate_session(&sessions, "alpha", "private-alpha");
  std::atomic<int> cleaned{0};
  ComputeRequestRegistryLimits limits;
  limits.terminal_ttl = 1s;
  ComputeRequestRegistry registry(
      sessions, [](const HostComputeRequest&) { return ok_status(); },
      [](const HostComputeRequest&) {
        return Result<ImageBuffer>{ok_status(), {}};
      },
      [&](const ComputeRequestId&, ImageBuffer) {
        return ComputeOutputPublication{
            ok_status(),
            ComputeOutputOwnership(
                "race-output",
                [&](const std::optional<std::string>&) { ++cleaned; })};
      },
      limits, [clock] { return clock->now(); },
      [&] { return compute_ids.next(); });
  ASSERT_TRUE(registry.start().ok);
  const auto submitted =
      registry.submit(session, request_for(1), ComputeResultMode::Image);
  ASSERT_TRUE(submitted.status.ok);
  ASSERT_EQ(wait_for_terminal(&registry, submitted.value.compute_id).state,
            ComputeRequestState::Succeeded);
  clock->advance(1s);

  StartBarrier barrier(2);
  OperationStatus released;
  std::size_t expired = 0;
  std::thread release_thread([&] {
    barrier.arrive_and_wait();
    released = registry.release(submitted.value.compute_id);
  });
  std::thread expiry_thread([&] {
    barrier.arrive_and_wait();
    expired = registry.cleanup_expired();
  });
  release_thread.join();
  expiry_thread.join();
  EXPECT_EQ(cleaned.load(), 1);
  EXPECT_TRUE(released.ok || released.code == kJobNotFoundCode);
  EXPECT_LE(expired, 1U);
  EXPECT_FALSE(released.ok && expired == 1U);
  EXPECT_EQ(registry.status(submitted.value.compute_id).status.code,
            kJobNotFoundCode);
}

}  // namespace
}  // namespace ps::ipc::internal
