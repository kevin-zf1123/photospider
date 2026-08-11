/**
 * @file test_isolated_cpu_invocation.cpp
 * @brief Verifies real fresh-process shared-memory CPU invocation behavior.
 */
#include <dirent.h>
#include <fcntl.h>
#include <gtest/gtest.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <future>
#include <limits>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include "execution/isolated_cpu_invocation.hpp"  // NOLINT(build/include_subdir)
#include "execution/isolated_cpu_invocation_test_probe.hpp"  // NOLINT(build/include_subdir)

#ifndef PS_TEST_ISOLATED_CPU_FIXTURE_PATH
#error "PS_TEST_ISOLATED_CPU_FIXTURE_PATH must name the process fixture"
#endif

namespace ps::execution {
namespace {

/**
 * @brief Unique owner for one test-only POSIX descriptor.
 * @throws Nothing for moves and destruction.
 */
class ScopedTestFd final {
 public:
  /**
   * @brief Takes ownership of one descriptor or invalid sentinel.
   * @param descriptor Descriptor to close once.
   * @throws Nothing.
   */
  explicit ScopedTestFd(int descriptor) noexcept : descriptor_(descriptor) {}

  /** @brief Closes the retained descriptor once. */
  ~ScopedTestFd() noexcept {
    if (descriptor_ >= 0) {
      static_cast<void>(::close(descriptor_));
    }
  }

  /** @brief Prevents duplicate ownership. */
  ScopedTestFd(const ScopedTestFd&) = delete;
  /** @brief Prevents duplicate assignment. */
  ScopedTestFd& operator=(const ScopedTestFd&) = delete;

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
 * @brief Ensures a borrowed test socket write half is shut before async join.
 * @throws Nothing for construction, shutdown, and destruction.
 * @note The socket descriptor remains owned by `ScopedTestFd`.
 */
class ScopedTestWriteHalf final {
 public:
  /**
   * @brief Borrows one connected socket whose write half remains open.
   * @param descriptor Valid socket descriptor.
   * @throws Nothing.
   */
  explicit ScopedTestWriteHalf(int descriptor) noexcept
      : descriptor_(descriptor) {}

  /**
   * @brief Best-effort shuts the still-borrowed write half.
   * @throws Nothing; channel errors cannot replace the active test result.
   */
  ~ScopedTestWriteHalf() noexcept { finish(); }

  /** @brief Prevents duplicate borrowed shutdown responsibility. */
  ScopedTestWriteHalf(const ScopedTestWriteHalf&) = delete;
  /** @brief Prevents duplicate borrowed shutdown assignment. */
  ScopedTestWriteHalf& operator=(const ScopedTestWriteHalf&) = delete;

  /**
   * @brief Shuts the borrowed write half at most once.
   * @return Nothing after clearing test shutdown responsibility.
   * @throws Nothing; framing assertions observe production behavior separately.
   */
  void finish() noexcept {
    const int descriptor = std::exchange(descriptor_, -1);
    if (descriptor < 0) {
      return;
    }
    int result = -1;
    do {
      result = ::shutdown(descriptor, SHUT_WR);
    } while (result < 0 && errno == EINTR);
  }

 private:
  /** @brief Borrowed socket awaiting write-half shutdown, or -1. */
  int descriptor_ = -1;
};

/**
 * @brief Creates one deterministic nonzero invocation identity component.
 * @param seed First sequence byte.
 * @return Complete comparison-only opaque value.
 * @throws Nothing.
 */
IsolatedCpuOpaqueId integration_id(std::uint8_t seed) noexcept {
  IsolatedCpuOpaqueId id;
  for (std::size_t index = 0U; index < id.bytes.size(); ++index) {
    id.bytes[index] = static_cast<std::byte>(seed + index);
  }
  return id;
}

/**
 * @brief Creates one complete deterministic invocation identity.
 * @param domain Per-call identity domain below wraparound.
 * @return Valid tuple with current worker/plugin generations.
 * @throws Nothing.
 */
IsolatedCpuInvocationIdentity integration_identity(
    std::uint8_t domain) noexcept {
  IsolatedCpuInvocationIdentity identity;
  identity.tenant_id = integration_id(1U);
  identity.job_id = integration_id(17U);
  identity.attempt_id = integration_id(33U);
  identity.worker_id = integration_id(49U);
  identity.worker_lease_generation = 3U;
  identity.plugin_package_id = integration_id(65U);
  identity.plugin_generation = 5U;
  identity.invocation_id = integration_id(domain);
  return identity;
}

/**
 * @brief Creates the shared two-by-three u8 descriptor used by integration
 * tests.
 * @return Unquantized NativeScalar descriptor.
 * @throws std::bad_alloc when shape storage cannot allocate.
 */
DenseTensorDescriptor integration_descriptor() {
  DenseTensorDescriptor descriptor;
  descriptor.shape = {2U, 3U};
  descriptor.element_semantics = ElementSemantics::UnsignedInteger;
  descriptor.storage_encoding = StorageEncoding{8U};
  return descriptor;
}

/**
 * @brief Creates the exact contiguous layout for the integration descriptor.
 * @return Positive non-overlapping six-byte layout.
 * @throws std::bad_alloc when stride storage cannot allocate.
 */
StridedLayout integration_layout() {
  return StridedLayout{{3, 1}, 0U};
}

/**
 * @brief Publishes one six-byte immutable input Value.
 * @param bytes Exact payload bytes.
 * @return Fresh Ready CPU DenseTensor Value.
 * @throws ValueBuilder validation/allocation/publication errors unchanged.
 */
Value integration_input(const std::array<std::byte, 6U>& bytes) {
  ValueBuilder builder = ValueBuilder::allocate_cpu_dense_tensor(
      integration_descriptor(), std::nullopt, integration_layout(),
      bytes.size());
  {
    WriteLease lease = builder.acquire_write();
    std::memcpy(lease.data(), bytes.data(), bytes.size());
  }
  return builder.seal();
}

/**
 * @brief Creates one exact six-byte Host output plan.
 * @return Positive-stride unquantized CPU plan.
 * @throws std::bad_alloc when descriptor/layout vectors allocate.
 */
IsolatedCpuDenseTensorOutputPlan integration_output_plan() {
  return IsolatedCpuDenseTensorOutputPlan{
      integration_descriptor(), std::nullopt, integration_layout(), 6U};
}

/**
 * @brief Creates one exact one-byte Host output plan.
 * @return Rank-one u8 output plan.
 * @throws std::bad_alloc when descriptor/layout vectors allocate.
 */
IsolatedCpuDenseTensorOutputPlan one_byte_output_plan() {
  DenseTensorDescriptor descriptor;
  descriptor.shape = {1U};
  descriptor.element_semantics = ElementSemantics::UnsignedInteger;
  descriptor.storage_encoding = StorageEncoding{8U};
  return IsolatedCpuDenseTensorOutputPlan{std::move(descriptor), std::nullopt,
                                          StridedLayout{{1}, 0U}, 1U};
}

/**
 * @brief Creates one page-sized rank-one output for aggregate-bound tests.
 * @return Positive-stride output whose payload is one local VM page.
 * @throws std::system_error when the platform reports an invalid page size.
 * @throws std::bad_alloc when descriptor/layout vectors cannot allocate.
 */
IsolatedCpuDenseTensorOutputPlan page_sized_output_plan() {
  const int page_size = ::getpagesize();
  if (page_size <= 0) {
    throw std::system_error(EIO, std::generic_category(),
                            "query invocation test page size");
  }
  DenseTensorDescriptor descriptor;
  descriptor.shape = {static_cast<std::size_t>(page_size)};
  descriptor.element_semantics = ElementSemantics::UnsignedInteger;
  descriptor.storage_encoding = StorageEncoding{8U};
  return IsolatedCpuDenseTensorOutputPlan{std::move(descriptor), std::nullopt,
                                          StridedLayout{{1}, 0U},
                                          static_cast<std::size_t>(page_size)};
}

/**
 * @brief Creates a Host invocation with one standard output plan.
 * @param operation Child fixture operation key.
 * @param domain Unique deterministic invocation-id domain.
 * @param inputs Ready input Values.
 * @return Complete high-level request.
 * @throws std::bad_alloc when copied state cannot allocate.
 */
IsolatedCpuHostInvocation integration_invocation(
    std::string operation, std::uint8_t domain,
    std::vector<Value> inputs = {}) {
  IsolatedCpuHostInvocation invocation;
  invocation.identity = integration_identity(domain);
  invocation.operation = std::move(operation);
  invocation.inputs = std::move(inputs);
  invocation.outputs.push_back(integration_output_plan());
  return invocation;
}

/**
 * @brief Copies one Ready contiguous Value into test-owned bytes.
 * @param value Valid six-byte output.
 * @return Exact physical storage bytes.
 * @throws DenseTensorView validation/readiness/access errors unchanged.
 */
std::array<std::byte, 6U> integration_bytes(const Value& value) {
  DenseTensorView view(value);
  if (view.storage_size() != 6U) {
    throw std::runtime_error("isolated CPU integration output size changed");
  }
  std::array<std::byte, 6U> bytes{};
  std::memcpy(bytes.data(), view.data(), bytes.size());
  return bytes;
}

/**
 * @brief Counts this process's currently open descriptors without retaining
 * one.
 * @return Open descriptor count excluding the transient directory descriptor.
 * @throws std::system_error when `/dev/fd` cannot be enumerated exactly.
 * @note The helper observes leak behavior only; descriptor identities remain
 * process-local and never enter the invocation protocol.
 */
std::size_t count_open_descriptors() {
  DIR* directory = ::opendir("/dev/fd");
  if (directory == nullptr) {
    throw std::system_error(errno, std::generic_category(),
                            "open /dev/fd for invocation leak test");
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
                            "close /dev/fd for invocation leak test");
  }
  if (read_error != 0) {
    throw std::system_error(read_error, std::generic_category(),
                            "read /dev/fd for invocation leak test");
  }
  return count;
}

/**
 * @brief Creates an executor targeting the built test runtime fixture.
 * @param limits Retained endpoint limits for the Host adapter.
 * @return Operability-validated non-supervised executor.
 * @throws Construction validation errors unchanged.
 */
NonSupervisedIsolatedCpuInvocationExecutor integration_executor(
    IsolatedCpuInvocationLimits limits = {}) {
  return NonSupervisedIsolatedCpuInvocationExecutor(
      std::filesystem::path(PS_TEST_ISOLATED_CPU_FIXTURE_PATH), limits);
}

/**
 * @brief Enables no-signal test sends on one private stream endpoint.
 * @param socket Valid test socket.
 * @return Nothing after platform configuration.
 * @throws std::system_error when the platform option fails.
 */
void configure_test_sender(int socket) {
#ifdef SO_NOSIGPIPE
  int enabled = 1;
  if (::setsockopt(socket, SOL_SOCKET, SO_NOSIGPIPE, &enabled,
                   sizeof(enabled)) != 0) {
    throw std::system_error(errno, std::generic_category(),
                            "configure invocation test socket");
  }
#else
  static_cast<void>(socket);
#endif
}

/**
 * @brief Sends one complete test frame without ancillary descriptors.
 * @param socket Connected blocking stream endpoint.
 * @param bytes First byte of a nonempty frame.
 * @param size Positive frame size.
 * @return Nothing after all bytes are sent.
 * @throws std::invalid_argument for null or empty input.
 * @throws std::system_error for a channel failure or zero progress.
 */
void send_test_frame(int socket, const std::byte* bytes, std::size_t size) {
  if (bytes == nullptr || size == 0U) {
    throw std::invalid_argument("invocation test frame is empty");
  }
  int flags = 0;
#ifdef MSG_NOSIGNAL
  flags |= MSG_NOSIGNAL;
#endif
  std::size_t offset = 0U;
  while (offset != size) {
    ssize_t sent = -1;
    do {
      sent = ::send(socket, bytes + offset, size - offset, flags);
    } while (sent < 0 && errno == EINTR);
    if (sent <= 0) {
      throw std::system_error(sent < 0 ? errno : EIO, std::generic_category(),
                              "send invocation test frame");
    }
    offset += static_cast<std::size_t>(sent);
  }
}

/**
 * @brief Waits until the production receiver observes one exact frame length.
 * @param prior_count Snapshot count before the receiver starts.
 * @return True after the count advances, or false at the observation deadline.
 * @throws Nothing.
 * @note The bound controls only the test harness, not production invocation
 * supervision or callback behavior.
 */
bool wait_for_exact_frame_observation(std::uint64_t prior_count) noexcept {
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (IsolatedCpuInvocationTestProbe::snapshot().exact_frames_received ==
         prior_count) {
    if (std::chrono::steady_clock::now() >= deadline) {
      return false;
    }
    std::this_thread::yield();
  }
  return true;
}

/**
 * @brief Sends one delayed trailing byte, optionally with one delayed FD.
 * @param socket Connected test sender after the exact frame was consumed.
 * @param with_descriptor Whether to attach one `/dev/null` descriptor.
 * @return Nothing; peer-close errors are retained as the pre-fix observation.
 * @throws std::system_error when opening the optional descriptor fails.
 * @note A successful receiver must reject either form before returning.
 */
void send_delayed_tail(int socket, bool with_descriptor) {
  const std::byte tail{0x7f};
  int flags = 0;
#ifdef MSG_NOSIGNAL
  flags |= MSG_NOSIGNAL;
#endif
  if (!with_descriptor) {
    ssize_t sent = -1;
    do {
      sent = ::send(socket, &tail, 1U, flags);
    } while (sent < 0 && errno == EINTR);
    return;
  }

  ScopedTestFd descriptor(::open("/dev/null", O_RDONLY));
  if (descriptor.get() < 0) {
    throw std::system_error(errno, std::generic_category(),
                            "open delayed invocation test descriptor");
  }
  union AncillaryBuffer {
    struct cmsghdr alignment;
    std::array<unsigned char, CMSG_SPACE(sizeof(int))> bytes;
  } control{};
  struct iovec data{const_cast<std::byte*>(&tail), 1U};
  struct msghdr message{};
  message.msg_iov = &data;
  message.msg_iovlen = 1U;
  message.msg_control = control.bytes.data();
  message.msg_controllen = control.bytes.size();
  struct cmsghdr* header = CMSG_FIRSTHDR(&message);
  if (header == nullptr) {
    throw std::runtime_error("construct delayed invocation ancillary header");
  }
  header->cmsg_level = SOL_SOCKET;
  header->cmsg_type = SCM_RIGHTS;
  header->cmsg_len = CMSG_LEN(sizeof(int));
  const int raw_descriptor = descriptor.get();
  std::memcpy(CMSG_DATA(header), &raw_descriptor, sizeof(raw_descriptor));
  ssize_t sent = -1;
  do {
    sent = ::sendmsg(socket, &message, flags);
  } while (sent < 0 && errno == EINTR);
}

/**
 * @brief Sends one real descriptor with no stream payload on Darwin.
 * @param socket Connected test sender after the exact frame was consumed.
 * @return Nothing after `sendmsg` reports zero payload bytes.
 * @throws std::system_error when the descriptor open or send fails.
 * @throws std::runtime_error when the platform reports nonzero payload
 * progress.
 * @note Darwin installs the descriptor at the receiver even though both
 * `sendmsg` and `recvmsg` return zero. The caller deliberately retains the
 * socket write half so that this zero result cannot denote peer EOF.
 */
void send_zero_payload_right(int socket) {
  ScopedTestFd descriptor(::open("/dev/null", O_RDONLY));
  if (descriptor.get() < 0) {
    throw std::system_error(errno, std::generic_category(),
                            "open zero-payload invocation test descriptor");
  }
  union AncillaryBuffer {
    struct cmsghdr alignment;
    std::array<unsigned char, CMSG_SPACE(sizeof(int))> bytes;
  } control{};
  std::byte empty{};
  struct iovec data{&empty, 0U};
  struct msghdr message{};
  message.msg_iov = &data;
  message.msg_iovlen = 1U;
  message.msg_control = control.bytes.data();
  message.msg_controllen = control.bytes.size();
  struct cmsghdr* header = CMSG_FIRSTHDR(&message);
  if (header == nullptr) {
    throw std::runtime_error(
        "construct zero-payload invocation ancillary header");
  }
  header->cmsg_level = SOL_SOCKET;
  header->cmsg_type = SCM_RIGHTS;
  header->cmsg_len = CMSG_LEN(sizeof(int));
  const int raw_descriptor = descriptor.get();
  std::memcpy(CMSG_DATA(header), &raw_descriptor, sizeof(raw_descriptor));
  int flags = 0;
#ifdef MSG_NOSIGNAL
  flags |= MSG_NOSIGNAL;
#endif
  ssize_t sent = -1;
  do {
    sent = ::sendmsg(socket, &message, flags);
  } while (sent < 0 && errno == EINTR);
  if (sent < 0) {
    throw std::system_error(errno, std::generic_category(),
                            "send zero-payload invocation ancillary right");
  }
  if (sent != 0) {
    throw std::runtime_error(
        "zero-payload invocation ancillary send made byte progress");
  }
}

/**
 * @brief Exercises delayed trailing data against the production receiver.
 * @param with_descriptor Whether the delayed byte carries `SCM_RIGHTS`.
 * @return Nothing only when the receiver incorrectly accepts the exact frame.
 * @throws IsolatedCpuProtocolError when the receiver rejects the delayed tail.
 * @throws Invocation test setup/channel errors unchanged.
 */
void receive_frame_with_delayed_tail(bool with_descriptor) {
  int sockets[2] = {-1, -1};
  if (::socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) != 0) {
    throw std::system_error(errno, std::generic_category(),
                            "create invocation framing test socketpair");
  }
  ScopedTestFd sender(sockets[0]);
  ScopedTestFd receiver(sockets[1]);
  configure_test_sender(sender.get());
  const std::uint64_t prior_frames =
      IsolatedCpuInvocationTestProbe::snapshot().exact_frames_received;
  std::future<void> receive = std::async(std::launch::async, [&receiver]() {
    IsolatedCpuInvocationTestProbe::receive_one_packet(receiver.get());
  });
  ScopedTestWriteHalf sender_write(sender.get());
  std::array<std::byte, kIsolatedCpuPacketHeaderBytes> exact_frame{};
  send_test_frame(sender.get(), exact_frame.data(), exact_frame.size());
  if (!wait_for_exact_frame_observation(prior_frames)) {
    sender_write.finish();
    receive.get();
    throw std::runtime_error(
        "invocation test receiver did not observe the exact frame");
  }
  send_delayed_tail(sender.get(), with_descriptor);
  sender_write.finish();
  receive.get();
}

/**
 * @brief Exercises Darwin zero-payload rights against the production receiver.
 * @return Nothing only when the receiver incorrectly accepts the exact frame.
 * @throws IsolatedCpuProtocolError when the receiver rejects the ancillary
 * right.
 * @throws std::runtime_error when a test-harness observation deadline expires
 * after the asynchronous receiver is cleaned up.
 * @throws Invocation test setup/channel errors unchanged.
 * @note On a timely path the sender write half remains open until the
 * asynchronous production receiver returns or throws, proving that its
 * zero-byte `recvmsg` is not EOF. Only a test-harness timeout closes that write
 * half to unblock and consume the future; cleanup exceptions are discarded so
 * they cannot replace the deterministic timeout failure.
 */
void receive_frame_with_zero_payload_right() {
  int sockets[2] = {-1, -1};
  if (::socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) != 0) {
    throw std::system_error(errno, std::generic_category(),
                            "create invocation framing test socketpair");
  }
  ScopedTestFd sender(sockets[0]);
  ScopedTestFd receiver(sockets[1]);
  configure_test_sender(sender.get());
  const std::uint64_t prior_frames =
      IsolatedCpuInvocationTestProbe::snapshot().exact_frames_received;
  std::future<void> receive = std::async(std::launch::async, [&receiver]() {
    IsolatedCpuInvocationTestProbe::receive_one_packet(receiver.get());
  });
  ScopedTestWriteHalf sender_write(sender.get());
  std::array<std::byte, kIsolatedCpuPacketHeaderBytes> exact_frame{};
  send_test_frame(sender.get(), exact_frame.data(), exact_frame.size());
  const bool exact_frame_timed_out =
      !wait_for_exact_frame_observation(prior_frames);
  if (exact_frame_timed_out) {
    sender_write.finish();
    try {
      receive.get();
    } catch (...) {
      // The harness timeout remains authoritative over cleanup failures.
    }
    throw std::runtime_error(
        "invocation test receiver did not observe the exact frame");
  }
  send_zero_payload_right(sender.get());
  const bool receive_timed_out =
      receive.wait_for(std::chrono::seconds(2)) != std::future_status::ready;
  if (receive_timed_out) {
    sender_write.finish();
    try {
      receive.get();
    } catch (...) {
      // The harness timeout remains authoritative over cleanup failures.
    }
    throw std::runtime_error(
        "invocation receiver blocked on a delivered zero-payload right");
  }
  receive.get();
}

/**
 * @brief Checks one synchronous call produced and exactly reaped one child.
 * @param before Observation immediately before `invoke`.
 * @param after Observation immediately after `invoke` returned.
 * @return Nothing after GoogleTest assertions.
 * @throws Nothing.
 * @note The final `waitpid(WNOHANG)` runs only after production reported its
 * exact blocking reap, so it observes `ECHILD` without competing for a child.
 */
void expect_one_child_reaped(
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

TEST(IsolatedCpuInvocation, CopiesThroughFreshProcessAndPublishesFreshValue) {
  const std::array<std::byte, 6U> source{std::byte{0},   std::byte{1},
                                         std::byte{2},   std::byte{253},
                                         std::byte{254}, std::byte{255}};
  const Value input = integration_input(source);
  IsolatedCpuHostInvocation invocation =
      integration_invocation("fixture.increment_u8", 97U, {input});

  const IsolatedCpuHostInvocationResult result =
      integration_executor().invoke(invocation);

  ASSERT_EQ(result.outcome, IsolatedCpuInvocationOutcome::Succeeded);
  ASSERT_EQ(result.outputs.size(), 1U);
  EXPECT_TRUE(result.diagnostic.empty());
  EXPECT_NE(result.outputs[0].allocation_identity(),
            input.allocation_identity());
  const std::array<std::byte, 6U> expected{std::byte{1},   std::byte{2},
                                           std::byte{3},   std::byte{254},
                                           std::byte{255}, std::byte{0}};
  EXPECT_EQ(integration_bytes(result.outputs[0]), expected);
}

TEST(IsolatedCpuInvocation, SupportsZeroInputAndExactOutputPlan) {
  IsolatedCpuHostInvocation invocation =
      integration_invocation("fixture.fill_sequence", 113U);
  const IsolatedCpuHostInvocationResult result =
      integration_executor().invoke(invocation);

  ASSERT_EQ(result.outcome, IsolatedCpuInvocationOutcome::Succeeded);
  ASSERT_EQ(result.outputs.size(), 1U);
  const std::array<std::byte, 6U> expected{std::byte{0}, std::byte{1},
                                           std::byte{2}, std::byte{3},
                                           std::byte{4}, std::byte{5}};
  EXPECT_EQ(integration_bytes(result.outputs[0]), expected);
}

TEST(IsolatedCpuInvocation, PreservesTypedFailureCancellationAndException) {
  NonSupervisedIsolatedCpuInvocationExecutor executor = integration_executor();
  IsolatedCpuHostInvocationResult failed =
      executor.invoke(integration_invocation("fixture.fail", 129U));
  EXPECT_EQ(failed.outcome, IsolatedCpuInvocationOutcome::PluginFailed);
  EXPECT_TRUE(failed.outputs.empty());
  EXPECT_FALSE(failed.diagnostic.empty());

  IsolatedCpuHostInvocationResult cancelled =
      executor.invoke(integration_invocation("fixture.cancel", 145U));
  EXPECT_EQ(cancelled.outcome, IsolatedCpuInvocationOutcome::Cancelled);
  EXPECT_TRUE(cancelled.outputs.empty());
  EXPECT_FALSE(cancelled.diagnostic.empty());

  IsolatedCpuHostInvocationResult raised =
      executor.invoke(integration_invocation("fixture.throw", 161U));
  EXPECT_EQ(raised.outcome, IsolatedCpuInvocationOutcome::PluginFailed);
  EXPECT_TRUE(raised.outputs.empty());
  EXPECT_NE(raised.diagnostic.find("fixture callback exception"),
            std::string::npos);
}

TEST(IsolatedCpuInvocation,
     RejectsOversizedMetadataBeforeAnyHostMaterialization) {
  IsolatedCpuHostInvocation invocation =
      integration_invocation("fixture.fill_sequence", 169U);
  invocation.operation.assign(kMaximumIsolatedCpuOperationBytes + 1U, 'x');
  const std::size_t descriptors_before = count_open_descriptors();
  const IsolatedCpuInvocationTestSnapshot before =
      IsolatedCpuInvocationTestProbe::snapshot();

  EXPECT_THROW(integration_executor().invoke(invocation),
               IsolatedCpuProtocolError);

  const IsolatedCpuInvocationTestSnapshot after =
      IsolatedCpuInvocationTestProbe::snapshot();
  EXPECT_EQ(after.host_capability_materialization_attempts,
            before.host_capability_materialization_attempts);
  EXPECT_EQ(after.spawned_children, before.spawned_children);
  EXPECT_EQ(count_open_descriptors(), descriptors_before);
}

TEST(IsolatedCpuInvocation,
     RejectsAggregateMetadataPacketBeforeAnyHostMaterialization) {
  IsolatedCpuHostInvocation invocation =
      integration_invocation("fixture.fill_sequence", 169U);
  for (char suffix = 'a'; suffix <= 'q'; ++suffix) {
    IsolatedCpuScalarParameter parameter;
    parameter.name = "parameter_";
    parameter.name.push_back(suffix);
    parameter.kind = IsolatedCpuScalarKind::String;
    parameter.string_value.assign(kMaximumIsolatedCpuParameterStringBytes, 'x');
    invocation.parameters.push_back(std::move(parameter));
  }
  const std::size_t descriptors_before = count_open_descriptors();
  const IsolatedCpuInvocationTestSnapshot before =
      IsolatedCpuInvocationTestProbe::snapshot();

  EXPECT_THROW(integration_executor().invoke(invocation),
               IsolatedCpuProtocolError);

  const IsolatedCpuInvocationTestSnapshot after =
      IsolatedCpuInvocationTestProbe::snapshot();
  EXPECT_EQ(after.host_capability_materialization_attempts,
            before.host_capability_materialization_attempts);
  EXPECT_EQ(after.spawned_children, before.spawned_children);
  EXPECT_EQ(count_open_descriptors(), descriptors_before);
}

TEST(IsolatedCpuInvocation,
     RejectsInvalidOutputLayoutBeforeAnyHostMaterialization) {
  IsolatedCpuHostInvocation invocation =
      integration_invocation("fixture.fill_sequence", 170U);
  invocation.outputs[0].layout.byte_strides = {1, 1};
  const std::size_t descriptors_before = count_open_descriptors();
  const IsolatedCpuInvocationTestSnapshot before =
      IsolatedCpuInvocationTestProbe::snapshot();

  EXPECT_THROW(integration_executor().invoke(invocation),
               IsolatedCpuProtocolError);

  const IsolatedCpuInvocationTestSnapshot after =
      IsolatedCpuInvocationTestProbe::snapshot();
  EXPECT_EQ(after.host_capability_materialization_attempts,
            before.host_capability_materialization_attempts);
  EXPECT_EQ(after.spawned_children, before.spawned_children);
  EXPECT_EQ(count_open_descriptors(), descriptors_before);
}

TEST(IsolatedCpuInvocation, RejectsAggregatePlanBeforeAnyHostMaterialization) {
  IsolatedCpuHostInvocation invocation =
      integration_invocation("fixture.fill_sequence", 171U);
  invocation.outputs.push_back(page_sized_output_plan());
  IsolatedCpuInvocationLimits limits;
  limits.maximum_shared_memory_bytes =
      static_cast<std::uint64_t>(::getpagesize());
  const std::size_t descriptors_before = count_open_descriptors();
  const IsolatedCpuInvocationTestSnapshot before =
      IsolatedCpuInvocationTestProbe::snapshot();

  EXPECT_THROW(integration_executor(limits).invoke(invocation),
               IsolatedCpuProtocolError);

  const IsolatedCpuInvocationTestSnapshot after =
      IsolatedCpuInvocationTestProbe::snapshot();
  EXPECT_EQ(after.host_capability_materialization_attempts,
            before.host_capability_materialization_attempts);
  EXPECT_EQ(after.spawned_children, before.spawned_children);
  EXPECT_EQ(count_open_descriptors(), descriptors_before);
}

TEST(IsolatedCpuInvocation, RejectsAbnormalChildExitWithoutPublishingOutput) {
  EXPECT_THROW(integration_executor().invoke(
                   integration_invocation("fixture.crash", 177U)),
               IsolatedCpuInvocationError);
}

TEST(IsolatedCpuInvocation,
     RejectsRightsAfterEarlierResponseBytesWithoutResourceLeak) {
  const std::size_t descriptors_before = count_open_descriptors();
  const IsolatedCpuInvocationTestSnapshot state_before =
      IsolatedCpuInvocationTestProbe::snapshot();
  try {
    static_cast<void>(integration_executor().invoke(
        integration_invocation("fixture.late_rights", 185U)));
    FAIL() << "late ancillary rights were accepted";
  } catch (const IsolatedCpuProtocolError& error) {
    const std::string diagnostic = error.what();
    const bool rejected_after_observed_segment =
        diagnostic.find("after the first stream segment") != std::string::npos;
    const bool rejected_by_response_inventory =
        diagnostic.find("response unexpectedly carried descriptors") !=
        std::string::npos;
    EXPECT_TRUE(rejected_after_observed_segment ||
                rejected_by_response_inventory)
        << "unexpected response-rights classification: " << diagnostic;
  }
  const IsolatedCpuInvocationTestSnapshot state_after =
      IsolatedCpuInvocationTestProbe::snapshot();
  expect_one_child_reaped(state_before, state_after);
  EXPECT_EQ(count_open_descriptors(), descriptors_before);
}

TEST(IsolatedCpuInvocation, RejectsDelayedBytesAfterExactFrameWithoutFdLeak) {
  const std::size_t before = count_open_descriptors();
  EXPECT_THROW(receive_frame_with_delayed_tail(false),
               IsolatedCpuProtocolError);
  EXPECT_EQ(count_open_descriptors(), before);
}

TEST(IsolatedCpuInvocation, RejectsDelayedRightsAfterExactFrameWithoutFdLeak) {
  const std::size_t before = count_open_descriptors();
  EXPECT_THROW(receive_frame_with_delayed_tail(true), IsolatedCpuProtocolError);
  EXPECT_EQ(count_open_descriptors(), before);
}

TEST(IsolatedCpuInvocation,
     RejectsZeroPayloadRightsAfterExactFrameWithoutFdLeak) {
#if defined(__APPLE__)
  const std::size_t before = count_open_descriptors();
  EXPECT_THROW(receive_frame_with_zero_payload_right(),
               IsolatedCpuProtocolError);
  EXPECT_EQ(count_open_descriptors(), before);
#else
  GTEST_SKIP() << "Linux stream SCM_RIGHTS requires a nonempty payload";
#endif
}

TEST(IsolatedCpuInvocation, FreshExecClearsEnvironmentAndUnrelatedDescriptors) {
  ScopedTestFd lower_descriptor_one(::open("/dev/null", O_RDONLY));
  ScopedTestFd lower_descriptor_two(::open("/dev/null", O_RDONLY));
  ScopedTestFd inherited(::open("/dev/null", O_RDONLY));
  ASSERT_GE(lower_descriptor_one.get(), 0);
  ASSERT_GE(lower_descriptor_two.get(), 0);
  ASSERT_GE(inherited.get(), 5);
  IsolatedCpuHostInvocation invocation;
  invocation.identity = integration_identity(193U);
  invocation.operation = "fixture.verify_isolation";
  IsolatedCpuScalarParameter descriptor;
  descriptor.name = "inherited_fd";
  descriptor.kind = IsolatedCpuScalarKind::UnsignedInteger;
  descriptor.unsigned_value = static_cast<std::uint64_t>(inherited.get());
  invocation.parameters.push_back(std::move(descriptor));
  invocation.outputs.push_back(one_byte_output_plan());

  const IsolatedCpuHostInvocationResult result =
      integration_executor().invoke(invocation);
  ASSERT_EQ(result.outcome, IsolatedCpuInvocationOutcome::Succeeded);
  ASSERT_EQ(result.outputs.size(), 1U);
  DenseTensorView output(result.outputs[0]);
  ASSERT_EQ(output.storage_size(), 1U);
  EXPECT_EQ(output.data()[0], std::byte{1});
}

TEST(IsolatedCpuInvocation,
     RepeatedOutcomesRetireEveryFdMapAndExactlyReapEachChild) {
  NonSupervisedIsolatedCpuInvocationExecutor executor = integration_executor();
  const std::size_t descriptor_baseline = count_open_descriptors();
  const std::array<const char*, 4U> operations{"fixture.fill_sequence",
                                               "fixture.fail", "fixture.cancel",
                                               "fixture.throw"};
  const std::array<IsolatedCpuInvocationOutcome, 4U> expected_outcomes{
      IsolatedCpuInvocationOutcome::Succeeded,
      IsolatedCpuInvocationOutcome::PluginFailed,
      IsolatedCpuInvocationOutcome::Cancelled,
      IsolatedCpuInvocationOutcome::PluginFailed};
  for (std::uint8_t iteration = 0U; iteration < 20U; ++iteration) {
    const std::size_t scenario = iteration % operations.size();
    IsolatedCpuHostInvocation invocation = integration_invocation(
        operations[scenario], static_cast<std::uint8_t>(201U + iteration));
    const IsolatedCpuInvocationTestSnapshot before =
        IsolatedCpuInvocationTestProbe::snapshot();
    const IsolatedCpuHostInvocationResult result = executor.invoke(invocation);
    const IsolatedCpuInvocationTestSnapshot after =
        IsolatedCpuInvocationTestProbe::snapshot();
    ASSERT_EQ(result.outcome, expected_outcomes[scenario]);
    if (result.outcome == IsolatedCpuInvocationOutcome::Succeeded) {
      ASSERT_EQ(result.outputs.size(), 1U);
    } else {
      EXPECT_TRUE(result.outputs.empty());
      EXPECT_FALSE(result.diagnostic.empty());
    }
    expect_one_child_reaped(before, after);
    EXPECT_EQ(count_open_descriptors(), descriptor_baseline);
  }
  EXPECT_EQ(count_open_descriptors(), descriptor_baseline);
}

}  // namespace
}  // namespace ps::execution
