/**
 * @file isolated_cpu_plugin_fixture.cpp
 * @brief Provides deterministic fresh-exec callbacks for CPU invocation tests.
 */
#include <fcntl.h>
#include <signal.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string>
#include <thread>

#include "execution/isolated_cpu_invocation.hpp"  // NOLINT(build/include_subdir)
#include "execution/plugin_runtime_supervisor.hpp"  // NOLINT(build/include_subdir)

namespace ps::execution {
namespace {

/**
 * @brief Sends a malformed response whose rights arrive after prior bytes.
 * @return Never returns; the fixture exits after transferring the test frame.
 * @throws Nothing; syscall failures terminate the fixture with status 74.
 * @note The first byte is sent without ancillary data. The remaining bytes are
 * sent with one `SCM_RIGHTS` record, deliberately violating protocol v1.
 * `SOCK_STREAM` may coalesce both sends into the first `recvmsg`; the Host must
 * fail closed either at the observed-segment gate or at its response FD
 * inventory gate.
 */
[[noreturn]] void send_late_rights_response_and_exit() noexcept {
  std::array<std::byte, kIsolatedCpuPacketHeaderBytes> frame{};
  const int descriptor = ::open("/dev/null", O_RDONLY);
  if (descriptor < 0) {
    _exit(74);
  }
  int flags = 0;
#ifdef MSG_NOSIGNAL
  flags |= MSG_NOSIGNAL;
#endif
  ssize_t first = -1;
  do {
    first =
        ::send(kIsolatedCpuRuntimeControlDescriptor, frame.data(), 1U, flags);
  } while (first < 0 && errno == EINTR);
  if (first != 1) {
    static_cast<void>(::close(descriptor));
    _exit(74);
  }

  union AncillaryBuffer {
    struct cmsghdr alignment;
    std::array<unsigned char, CMSG_SPACE(sizeof(int))> bytes;
  } control{};
  struct iovec data{frame.data() + 1U, frame.size() - 1U};
  struct msghdr message{};
  message.msg_iov = &data;
  message.msg_iovlen = 1U;
  message.msg_control = control.bytes.data();
  message.msg_controllen = control.bytes.size();
  struct cmsghdr* header = CMSG_FIRSTHDR(&message);
  if (header == nullptr) {
    static_cast<void>(::close(descriptor));
    _exit(74);
  }
  header->cmsg_level = SOL_SOCKET;
  header->cmsg_type = SCM_RIGHTS;
  header->cmsg_len = CMSG_LEN(sizeof(descriptor));
  std::memcpy(CMSG_DATA(header), &descriptor, sizeof(descriptor));
  ssize_t sent = -1;
  do {
    sent = ::sendmsg(kIsolatedCpuRuntimeControlDescriptor, &message, flags);
  } while (sent < 0 && errno == EINTR);
  if (sent <= 0) {
    static_cast<void>(::close(descriptor));
    _exit(74);
  }
  std::size_t offset = 1U + static_cast<std::size_t>(sent);
  while (offset < frame.size()) {
    ssize_t remainder = -1;
    do {
      remainder = ::send(kIsolatedCpuRuntimeControlDescriptor,
                         frame.data() + offset, frame.size() - offset, flags);
    } while (remainder < 0 && errno == EINTR);
    if (remainder <= 0) {
      static_cast<void>(::close(descriptor));
      _exit(74);
    }
    offset += static_cast<std::size_t>(remainder);
  }
  static_cast<void>(::close(descriptor));
  _exit(0);
}

/**
 * @brief Sends the fixed deliberately truncated response prefix.
 * @return Nothing after all four prefix bytes are transferred.
 * @throws Nothing; syscall failure exits the fixture with status 74.
 * @note The prefix is shorter than the protocol-v1 frame header and therefore
 * can never become a complete response.
 */
void send_truncated_response_prefix() noexcept {
  const std::array<std::byte, 4U> prefix{std::byte{'B'}, std::byte{'A'},
                                         std::byte{'D'}, std::byte{'!'}};
  int flags = 0;
#ifdef MSG_NOSIGNAL
  flags |= MSG_NOSIGNAL;
#endif
  std::size_t offset = 0U;
  while (offset != prefix.size()) {
    const ssize_t count =
        ::send(kIsolatedCpuRuntimeControlDescriptor, prefix.data() + offset,
               prefix.size() - offset, flags);
    if (count > 0) {
      offset += static_cast<std::size_t>(count);
      continue;
    }
    if (count < 0 && errno == EINTR) {
      continue;
    }
    _exit(74);
  }
}

/**
 * @brief Sends a deliberately truncated response prefix and exits normally.
 * @return Never returns; the process exits with status zero after the prefix.
 * @throws Nothing; syscall failure exits with status 74.
 * @note The Host must classify this normal-exit output as bad output rather
 * than process crash or a successful plugin result.
 */
[[noreturn]] void send_truncated_response_and_exit() noexcept {
  send_truncated_response_prefix();
  _exit(0);
}

/**
 * @brief Sends a truncated response, closes only the write half, and stays
 * alive.
 * @return Never returns; the supervisor must terminate the paused fixture.
 * @throws Nothing; send or shutdown failure exits with status 74.
 * @note This deterministic mode supplies definitive premature framing EOF while
 * remaining unconditionally alive beyond any fixed child-status observation
 * window. It uses no timing sleep or unowned process synchronization.
 */
[[noreturn]] void send_truncated_response_and_hang() noexcept {
  send_truncated_response_prefix();
  int shutdown_result = -1;
  do {
    shutdown_result = ::shutdown(kIsolatedCpuRuntimeControlDescriptor, SHUT_WR);
  } while (shutdown_result < 0 && errno == EINTR);
  if (shutdown_result != 0) {
    _exit(74);
  }
  for (;;) {
    static_cast<void>(::pause());
  }
}

/**
 * @brief Executes one deterministic fixture operation over mapped ranges.
 * @param invocation Fully validated callback-local invocation.
 * @return Typed success, plugin failure, or cancellation result.
 * @throws std::runtime_error for the explicit exception fixture operation.
 * @note No pointer, mapping, FD, or invocation state is retained after return.
 * The delayed fill operation simulates callback work while the endpoint's
 * independent heartbeat thread remains live.
 */
IsolatedCpuRuntimeCallbackResult run_fixture_operation(
    const IsolatedCpuRuntimeInvocation& invocation) {
  if (invocation.operation == "fixture.late_rights") {
    send_late_rights_response_and_exit();
  }
  if (invocation.operation == "fixture.crash") {
    _exit(73);
  }
  if (invocation.operation == "fixture.sigkill") {
    static_cast<void>(::kill(::getpid(), SIGKILL));
    _exit(74);
  }
  if (invocation.operation == "fixture.truncated") {
    send_truncated_response_and_exit();
  }
  if (invocation.operation == "fixture.hang") {
    for (;;) {
      static_cast<void>(::pause());
    }
  }
  if (invocation.operation == "fixture.ignore_termination_hang") {
    static_cast<void>(::signal(SIGTERM, SIG_IGN));
    for (;;) {
      static_cast<void>(::pause());
    }
  }
  if (invocation.operation == "fixture.stop") {
    static_cast<void>(::raise(SIGSTOP));
    for (;;) {
      static_cast<void>(::pause());
    }
  }
  if (invocation.operation == "fixture.throw") {
    throw std::runtime_error("fixture callback exception");
  }
  if (invocation.operation == "fixture.fail") {
    return IsolatedCpuRuntimeCallbackResult{
        IsolatedCpuInvocationOutcome::PluginFailed,
        "fixture callback reported deterministic failure"};
  }
  if (invocation.operation == "fixture.cancel") {
    return IsolatedCpuRuntimeCallbackResult{
        IsolatedCpuInvocationOutcome::Cancelled,
        "fixture callback observed cooperative cancellation"};
  }
  if (invocation.operation == "fixture.fill_sequence" ||
      invocation.operation == "fixture.delayed_fill_sequence" ||
      invocation.operation == "fixture.response_hang") {
    if (!invocation.inputs.empty() || invocation.outputs.size() != 1U) {
      return IsolatedCpuRuntimeCallbackResult{
          IsolatedCpuInvocationOutcome::PluginFailed,
          "fixture fill_sequence shape is invalid"};
    }
    if (invocation.operation == "fixture.delayed_fill_sequence") {
      std::this_thread::sleep_for(std::chrono::milliseconds{600});
    }
    const IsolatedCpuRuntimeTensor& output = invocation.outputs[0];
    for (std::size_t index = 0U; index < output.size; ++index) {
      output.output_data[index] = static_cast<std::byte>(index & 0xffU);
    }
    return IsolatedCpuRuntimeCallbackResult{
        IsolatedCpuInvocationOutcome::Succeeded,
        {}};
  }
  if (invocation.operation == "fixture.increment_u8") {
    if (invocation.inputs.size() != 1U || invocation.outputs.size() != 1U ||
        invocation.inputs[0].size != invocation.outputs[0].size) {
      return IsolatedCpuRuntimeCallbackResult{
          IsolatedCpuInvocationOutcome::PluginFailed,
          "fixture increment_u8 ranges are invalid"};
    }
    const IsolatedCpuRuntimeTensor& input = invocation.inputs[0];
    const IsolatedCpuRuntimeTensor& output = invocation.outputs[0];
    for (std::size_t index = 0U; index < output.size; ++index) {
      const std::uint8_t value =
          std::to_integer<std::uint8_t>(input.input_data[index]);
      output.output_data[index] =
          static_cast<std::byte>(static_cast<std::uint8_t>(value + 1U));
    }
    return IsolatedCpuRuntimeCallbackResult{
        IsolatedCpuInvocationOutcome::Succeeded,
        {}};
  }
  if (invocation.operation == "fixture.verify_isolation") {
    if (!invocation.inputs.empty() || invocation.outputs.size() != 1U ||
        invocation.outputs[0].size != 1U ||
        invocation.parameters.size() != 1U ||
        invocation.parameters[0].name != "inherited_fd" ||
        invocation.parameters[0].kind !=
            IsolatedCpuScalarKind::UnsignedInteger ||
        invocation.parameters[0].unsigned_value >
            static_cast<std::uint64_t>(std::numeric_limits<int>::max())) {
      return IsolatedCpuRuntimeCallbackResult{
          IsolatedCpuInvocationOutcome::PluginFailed,
          "fixture verify_isolation request is invalid"};
    }
    const int inherited_fd =
        static_cast<int>(invocation.parameters[0].unsigned_value);
    errno = 0;
    const bool descriptor_closed =
        ::fcntl(inherited_fd, F_GETFD) < 0 && errno == EBADF;
    const bool environment_empty = std::getenv("PATH") == nullptr;
    if (!descriptor_closed || !environment_empty) {
      return IsolatedCpuRuntimeCallbackResult{
          IsolatedCpuInvocationOutcome::PluginFailed,
          "fixture observed inherited authority across exec"};
    }
    invocation.outputs[0].output_data[0] = std::byte{1};
    return IsolatedCpuRuntimeCallbackResult{
        IsolatedCpuInvocationOutcome::Succeeded,
        {}};
  }
  if (invocation.operation == "fixture.verify_resource_limits") {
    if (!invocation.inputs.empty() || invocation.outputs.size() != 1U ||
        invocation.outputs[0].size != 1U ||
        invocation.parameters.size() != 3U ||
        invocation.parameters[0].name != "address_space_bytes" ||
        invocation.parameters[1].name != "cpu_time_seconds" ||
        invocation.parameters[2].name != "descriptor_count" ||
        invocation.parameters[0].kind !=
            IsolatedCpuScalarKind::UnsignedInteger ||
        invocation.parameters[1].kind !=
            IsolatedCpuScalarKind::UnsignedInteger ||
        invocation.parameters[2].kind !=
            IsolatedCpuScalarKind::UnsignedInteger) {
      return IsolatedCpuRuntimeCallbackResult{
          IsolatedCpuInvocationOutcome::PluginFailed,
          "fixture resource-limit request is invalid"};
    }
    struct rlimit address_space{};
    struct rlimit cpu_time{};
    struct rlimit descriptors{};
    struct rlimit core_dump{};
    const bool queried = ::getrlimit(RLIMIT_AS, &address_space) == 0 &&
                         ::getrlimit(RLIMIT_CPU, &cpu_time) == 0 &&
                         ::getrlimit(RLIMIT_NOFILE, &descriptors) == 0 &&
                         ::getrlimit(RLIMIT_CORE, &core_dump) == 0;
    const auto exact_limit = [](const struct rlimit& limit,
                                std::uint64_t expected) noexcept {
      return limit.rlim_cur == static_cast<rlim_t>(expected) &&
             limit.rlim_max == static_cast<rlim_t>(expected);
    };
    if (!queried ||
        !exact_limit(address_space, invocation.parameters[0].unsigned_value) ||
        !exact_limit(cpu_time, invocation.parameters[1].unsigned_value) ||
        !exact_limit(descriptors, invocation.parameters[2].unsigned_value) ||
        !exact_limit(core_dump, 0U)) {
      return IsolatedCpuRuntimeCallbackResult{
          IsolatedCpuInvocationOutcome::PluginFailed,
          "fixture observed missing or changed child resource limit"};
    }
    invocation.outputs[0].output_data[0] = std::byte{1};
    return IsolatedCpuRuntimeCallbackResult{
        IsolatedCpuInvocationOutcome::Succeeded,
        {}};
  }
  return IsolatedCpuRuntimeCallbackResult{
      IsolatedCpuInvocationOutcome::PluginFailed,
      "fixture operation is unknown"};
}

/**
 * @brief Applies deterministic callback-adjacent endpoint behavior.
 * @param point Exact supervised endpoint milestone.
 * @param invocation Fully validated callback-local invocation.
 * @return Nothing for ordinary operations; response-hang modes wait for
 * supervisor termination after authenticating callback completion.
 * @throws Nothing.
 */
void run_fixture_lifecycle_hook(
    PluginRuntimeLifecyclePoint point,
    const IsolatedCpuRuntimeInvocation& invocation) noexcept {
  if (point == PluginRuntimeLifecyclePoint::BeforeResponse &&
      invocation.operation == "fixture.response_hang") {
    for (;;) {
      static_cast<void>(::pause());
    }
  }
  if (point == PluginRuntimeLifecyclePoint::BeforeResponse &&
      invocation.operation == "fixture.truncated_hang") {
    send_truncated_response_and_hang();
  }
}

/**
 * @brief Selects one compile-time startup behavior for a fixture executable.
 * @return Normal, silent, or corrupted-startup behavior.
 * @throws Nothing.
 */
constexpr PluginRuntimeEndpointStartupBehavior
fixture_startup_behavior() noexcept {
#if defined(PS_TEST_PLUGIN_RUNTIME_SUPPRESS_STARTED)
  return PluginRuntimeEndpointStartupBehavior::SuppressStarted;
#elif defined(PS_TEST_PLUGIN_RUNTIME_CORRUPT_STARTED_NONCE)
  return PluginRuntimeEndpointStartupBehavior::CorruptStartedNonce;
#else
  return PluginRuntimeEndpointStartupBehavior::Normal;
#endif
}

}  // namespace
}  // namespace ps::execution

/**
 * @brief Serves exactly one invocation on the fixed fresh-exec descriptor.
 * @return Endpoint status suitable for Host child-exit validation.
 * @throws Nothing; the endpoint contains callback and protocol exceptions.
 */
int main() {
  errno = 0;
  if (::fcntl(ps::execution::kPluginRuntimeSupervisionDescriptor, F_GETFD) >=
      0) {
    return ps::execution::serve_supervised_isolated_cpu_invocation_once(
        ps::execution::kIsolatedCpuRuntimeControlDescriptor,
        ps::execution::kPluginRuntimeSupervisionDescriptor, {},
        ps::execution::run_fixture_operation,
        ps::execution::fixture_startup_behavior(),
        ps::execution::run_fixture_lifecycle_hook);
  }
  return ps::execution::serve_non_supervised_isolated_cpu_invocation_once(
      ps::execution::kIsolatedCpuRuntimeControlDescriptor, {},
      ps::execution::run_fixture_operation);
}
