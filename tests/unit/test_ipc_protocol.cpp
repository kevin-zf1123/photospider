#include <arpa/inet.h>
#include <gtest/gtest.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
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
#include <thread>
#include <utility>
#include <vector>

#include "ipc/codec.hpp"
#include "ipc/frame.hpp"
#include "ipc/request_router.hpp"
#include "ipc/session_registry.hpp"
#include "ipc/unix_socket.hpp"
#include "photospider/host/host.hpp"
#include "photospider/ipc/client.hpp"
#include "scheduler/scheduler_plugin_loader.hpp"  // NOLINT(build/include_subdir)

#ifndef PS_TEST_SCHEDULER_PLUGIN_PATH
#define PS_TEST_SCHEDULER_PLUGIN_PATH \
  "build/test_schedulers/libdestroy_count_scheduler_plugin.dylib"
#endif

namespace ps::ipc::internal {
namespace {

/** @brief Environment key selecting scheduler fixture lifecycle failures. */
constexpr const char* kSchedulerFailureEnvironment =  // NOLINT
    "PS_DESTROY_COUNT_SCHEDULER_FAILURE";             // NOLINT

/** @brief Scheduler type exported by the deterministic close-failure fixture.
 */
constexpr const char* kDestroyCountSchedulerType = "destroy_count_test";

/** @brief Maximum diagnostic label bytes retained in a socket test directory.
 */
constexpr std::size_t kTempDirectoryLabelBytes = 12;

/**
 * @brief Owns one unique protected temporary directory for IPC unit tests.
 *
 * @throws std::filesystem::filesystem_error if setup fails.
 * @note Destruction removes all contained sockets and files best-effort.
 */
class ScopedTempDirectory {
 public:
  /**
   * @brief Creates one empty mode-0700 directory.
   *
   * @param label Stable test-specific directory prefix; only its first 12 bytes
   *        are retained to preserve a conservative Unix socket path budget.
   * @throws std::bad_alloc if bounded path construction cannot allocate.
   * @throws std::filesystem::filesystem_error if setup fails.
   */
  explicit ScopedTempDirectory(const std::string& label)
      : path_(std::filesystem::temp_directory_path() /
              (label.substr(0, kTempDirectoryLabelBytes) + "-" +
               std::to_string(::getpid()) + "-" +
               std::to_string(sequence_++))) {
    std::filesystem::remove_all(path_);
    std::filesystem::create_directories(path_);
    if (::chmod(path_.c_str(), 0700) != 0) {
      throw std::filesystem::filesystem_error(
          "chmod", path_, std::error_code(errno, std::generic_category()));
    }
  }

  /**
   * @brief Prevents two test owners from deleting one temporary tree.
   * @param other Owner that retains cleanup responsibility.
   * @throws Nothing because this operation is unavailable.
   * @note Each test creates its own unique directory.
   */
  ScopedTempDirectory(const ScopedTempDirectory& other) = delete;

  /**
   * @brief Prevents replacing temporary-tree cleanup ownership by copy.
   * @param other Owner whose temporary path remains unchanged.
   * @return No value because this operation is unavailable.
   * @throws Nothing because this operation is unavailable.
   * @note The owned path remains fixed for the helper lifetime.
   */
  ScopedTempDirectory& operator=(const ScopedTempDirectory& other) = delete;

  /**
   * @brief Removes the temporary tree best-effort.
   *
   * @throws Nothing.
   */
  ~ScopedTempDirectory() {
    std::error_code ignored;
    std::filesystem::remove_all(path_, ignored);
  }

  /**
   * @brief Returns the owned directory path.
   *
   * @return Stable path reference valid until destruction.
   * @throws Nothing.
   */
  const std::filesystem::path& path() const noexcept { return path_; }

 private:
  /** @brief Owned temporary directory path. */
  std::filesystem::path path_;

  /** @brief Process-local uniqueness sequence. */
  static std::uint64_t sequence_;
};

std::uint64_t ScopedTempDirectory::sequence_ = 0;

/**
 * @brief Temporarily sets one scheduler-fixture environment value.
 *
 * @throws std::bad_alloc if the key or previous value cannot be copied.
 * @throws std::runtime_error if the platform environment update fails.
 * @note Tests using this helper are process-serial because environment values
 *       are global. Destruction restores the exact prior value best-effort.
 */
class ScopedEnvironmentValue final {
 public:
  /**
   * @brief Saves the current value and installs one fixture selection.
   * @param name Environment key copied for this guard's lifetime.
   * @param value New value visible to the scheduler plugin.
   * @throws std::bad_alloc if owned strings cannot be allocated.
   * @throws std::runtime_error if the environment cannot be updated.
   */
  ScopedEnvironmentValue(const char* name, const std::string& value)
      : name_(name) {
    if (const char* previous = std::getenv(name)) {
      previous_ = std::string(previous);
    }
    set(value);
  }

  /**
   * @brief Restores the saved environment state without hiding test failures.
   * @throws Nothing; platform restoration failures are suppressed.
   */
  ~ScopedEnvironmentValue() noexcept {
    try {
      if (previous_) {
        set(*previous_);
      } else {
        clear();
      }
    } catch (...) {
    }
  }

  /**
   * @brief Prevents duplicate restoration ownership.
   * @param other Guard that remains the sole restoration owner.
   * @throws Nothing because construction is unavailable.
   */
  ScopedEnvironmentValue(const ScopedEnvironmentValue& other) = delete;

  /**
   * @brief Prevents replacing one active environment guard.
   * @param other Guard whose environment key remains unchanged.
   * @return No value because assignment is unavailable.
   * @throws Nothing because assignment is unavailable.
   */
  ScopedEnvironmentValue& operator=(const ScopedEnvironmentValue& other) =
      delete;

 private:
  /**
   * @brief Installs a new value for the owned key.
   * @param value Value to publish process-wide.
   * @return Nothing.
   * @throws std::runtime_error if the platform call fails.
   */
  void set(const std::string& value) {
#if defined(_WIN32)
    if (_putenv_s(name_.c_str(), value.c_str()) != 0) {
      throw std::runtime_error("_putenv_s failed");
    }
#else
    if (setenv(name_.c_str(), value.c_str(), 1) != 0) {
      throw std::runtime_error("setenv failed");
    }
#endif
  }

  /**
   * @brief Removes the owned environment key.
   * @return Nothing.
   * @throws std::runtime_error if the platform call fails.
   */
  void clear() {
#if defined(_WIN32)
    if (_putenv_s(name_.c_str(), "") != 0) {
      throw std::runtime_error("_putenv_s clear failed");
    }
#else
    if (unsetenv(name_.c_str()) != 0) {
      throw std::runtime_error("unsetenv failed");
    }
#endif
  }

  /** @brief Environment key retained through restoration. */
  std::string name_;
  /** @brief Previous value, or nullopt when the key was absent. */
  std::optional<std::string> previous_;
};

/**
 * @brief Returns the deterministic scheduler lifecycle fixture library path.
 *
 * @return Platform-specific path below the CMake test scheduler directory.
 * @throws std::bad_alloc if path or filename construction cannot allocate.
 */
std::filesystem::path destroy_count_scheduler_plugin_path() {
  return std::filesystem::path(PS_TEST_SCHEDULER_PLUGIN_PATH);
}

/**
 * @brief Clears process-global scheduler plugins on every router-test exit.
 *
 * @throws Nothing.
 * @note Declare before the Host owner so reverse destruction destroys all graph
 *       runtimes before the loader releases its final plugin mapping.
 */
class ScopedSchedulerPluginCleanup final {
 public:
  /**
   * @brief Clears stale scheduler plugin state before a fixture test begins.
   * @throws Nothing; cleanup failures are suppressed for assertion safety.
   */
  ScopedSchedulerPluginCleanup() noexcept { clear(); }

  /** @brief Clears scheduler state after later-declared Host destruction. */
  ~ScopedSchedulerPluginCleanup() noexcept { clear(); }

 private:
  /** @brief Clears plugin mappings and diagnostics behind a no-throw fence. */
  static void clear() noexcept {
    try {
      SchedulerPluginLoader::instance().clear_plugins();
      SchedulerPluginLoader::instance().clear_errors();
    } catch (...) {
    }
  }

 public:
  /**
   * @brief Prevents duplicate process-global cleanup ownership.
   * @param other Guard that retains cleanup responsibility.
   * @throws Nothing because construction is unavailable.
   */
  ScopedSchedulerPluginCleanup(const ScopedSchedulerPluginCleanup& other) =
      delete;

  /**
   * @brief Prevents replacing process-global cleanup ownership.
   * @param other Guard whose cleanup responsibility remains unchanged.
   * @return No value because assignment is unavailable.
   * @throws Nothing because assignment is unavailable.
   */
  ScopedSchedulerPluginCleanup& operator=(
      const ScopedSchedulerPluginCleanup& other) = delete;
};

/**
 * @brief Sends every byte in one test buffer.
 *
 * @param fd Connected socket descriptor.
 * @param data Bytes to send.
 * @param size Byte count.
 * @return True when every byte was sent.
 * @throws Nothing.
 * @note The helper retries interrupted writes and is used only by test peers.
 */
bool send_all_for_test(int fd, const unsigned char* data,
                       std::size_t size) noexcept {
  std::size_t offset = 0;
  while (offset < size) {
    const ssize_t count = ::send(fd, data + offset, size - offset, 0);
    if (count > 0) {
      offset += static_cast<std::size_t>(count);
      continue;
    }
    if (count < 0 && errno == EINTR) {
      continue;
    }
    return false;
  }
  return true;
}

/**
 * @brief Creates one bound local listener used by malformed-response tests.
 *
 * @param path Socket path below a protected test directory.
 * @return Owned listening descriptor.
 * @throws std::runtime_error on socket, bind, or listen failure.
 * @note The caller removes the socket through its temporary-directory owner.
 */
UniqueFd create_test_listener(const std::string& path) {
  UniqueFd listener(::socket(AF_UNIX, SOCK_STREAM, 0));
  if (!listener) {
    throw std::runtime_error("test listener socket failed");
  }
  sockaddr_un address{};
  address.sun_family = AF_UNIX;
  std::memcpy(address.sun_path, path.c_str(), path.size() + 1);
  const socklen_t length =
      static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) + path.size() + 1);
  if (::bind(listener.get(), reinterpret_cast<sockaddr*>(&address), length) !=
          0 ||
      ::listen(listener.get(), 1) != 0) {
    throw std::runtime_error("test listener bind/listen failed");
  }
  return listener;
}

/**
 * @brief Serves one syntactically valid but intentionally uncorrelated reply.
 *
 * @param listener Bound listener descriptor.
 * @param response_id Id to return instead of the client-generated id.
 * @param response_version Protocol version to return.
 * @return True when request read and response write both complete.
 * @throws Nothing; connection failures become false.
 */
bool serve_uncorrelated_response(int listener, const std::string& response_id,
                                 std::int32_t response_version) noexcept {
  UniqueFd peer(::accept(listener, nullptr, nullptr));
  if (!peer) {
    return false;
  }
  try {
    const FrameReadResult request = read_frame(peer.get());
    if (request.state != FrameReadState::Complete) {
      return false;
    }
    const Json response{{"protocol_version", response_version},
                        {"id", response_id},
                        {"result", Json{{"pong", true},
                                        {"server_instance_id",
                                         "0123456789abcdef0123456789abcdef"}}}};
    return write_frame(peer.get(), response.dump()).ok;
  } catch (...) {
    return false;
  }
}

/**
 * @brief Serves one correlated success envelope with a caller-selected result.
 *
 * @param listener Bound local listener descriptor.
 * @param result Intentionally valid or malformed typed result object.
 * @return True when one request is read, correlated, and answered completely.
 * @throws Nothing; framing, parsing, and allocation failures become false.
 * @note This real local peer exercises public `Client` result validation
 *       without exposing a raw-response hook in production code.
 */
bool serve_correlated_result(int listener, const Json& result) noexcept {
  UniqueFd peer(::accept(listener, nullptr, nullptr));
  if (!peer) {
    return false;
  }
  try {
    const FrameReadResult request_frame = read_frame(peer.get());
    if (request_frame.state != FrameReadState::Complete) {
      return false;
    }
    const JsonParseResult request = parse_json(request_frame.payload);
    if (!request.ok || !request.value.is_object() ||
        !request.value.value("id", Json()).is_string()) {
      return false;
    }
    const Json response{{"protocol_version", kProtocolVersion},
                        {"id", request.value["id"]},
                        {"result", result}};
    return write_frame(peer.get(), response.dump()).ok;
  } catch (...) {
    return false;
  }
}

/**
 * @brief Serves one caller-provided raw response after reading a client frame.
 *
 * @param listener Bound local listener descriptor.
 * @param response_payload Complete intentionally malformed response JSON.
 * @return True when the request read and raw response write both complete.
 * @throws Nothing; framing failures become false.
 * @note The helper preserves duplicate object keys that constructing a `Json`
 *       value would otherwise collapse.
 */
bool serve_raw_response(int listener,
                        const std::string& response_payload) noexcept {
  UniqueFd peer(::accept(listener, nullptr, nullptr));
  if (!peer) {
    return false;
  }
  try {
    if (read_frame(peer.get()).state != FrameReadState::Complete) {
      return false;
    }
    return write_frame(peer.get(), response_payload).ok;
  } catch (...) {
    return false;
  }
}

/**
 * @brief Parses one router response for structural assertions.
 *
 * @param payload Complete response payload.
 * @return Parsed JSON object.
 * @throws std::runtime_error if parsing unexpectedly fails.
 */
Json parse_response(const std::string& payload) {
  JsonParseResult parsed = parse_json(payload);
  if (!parsed.ok) {
    throw std::runtime_error(parsed.message);
  }
  return std::move(parsed.value);
}

/**
 * @brief Verifies one complete internal enum label table transactionally.
 *
 * @tparam Enum Public enum type handled by the overloaded codec.
 * @tparam Count Number of stable version 1 labels.
 * @param mappings Expected enum/label pairs.
 * @throws std::bad_alloc if test JSON or diagnostics cannot allocate.
 * @note Invalid memory enum values and malformed JSON must leave outputs
 *       unchanged; this helper does not establish any future enum vocabulary.
 */
template <typename Enum, std::size_t Count>
void expect_enum_codec(
    const std::array<std::pair<Enum, const char*>, Count>& mappings) {
  for (const auto& mapping : mappings) {
    Json encoded = "sentinel";
    ASSERT_TRUE(encode_enum(mapping.first, &encoded));
    EXPECT_EQ(encoded, mapping.second);
    Enum decoded = mappings.front().first;
    ASSERT_TRUE(decode_enum(encoded, &decoded));
    EXPECT_EQ(decoded, mapping.first);
  }

  Json unchanged = "sentinel";
  EXPECT_FALSE(encode_enum(static_cast<Enum>(999), &unchanged));
  EXPECT_EQ(unchanged, "sentinel");
  const std::vector<Json> malformed = {Json("future_value"), Json(""),
                                       Json("UPPERCASE"),    Json(1),
                                       Json(true),           Json(nullptr)};
  for (const Json& value : malformed) {
    Enum output = mappings.front().first;
    EXPECT_FALSE(decode_enum(value, &output));
    EXPECT_EQ(output, mappings.front().first);
  }
}

TEST(FrameCodec, PrefixUsesBigEndian) {
  int descriptors[2] = {-1, -1};
  ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, descriptors), 0);
  UniqueFd writer(descriptors[0]);
  UniqueFd reader(descriptors[1]);
  ASSERT_TRUE(write_frame(writer.get(), "abc").ok);
  std::array<unsigned char, 4> header{};
  ASSERT_EQ(::recv(reader.get(), header.data(), header.size(), 0), 4);
  EXPECT_EQ(header, (std::array<unsigned char, 4>{0, 0, 0, 3}));
}

TEST(FrameCodec, ReadsFragmentedHeaderAndPayload) {
  int descriptors[2] = {-1, -1};
  ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, descriptors), 0);
  UniqueFd reader(descriptors[0]);
  UniqueFd writer(descriptors[1]);
  const std::string payload = "fragmented-payload";
  const std::uint32_t network_length =
      htonl(static_cast<std::uint32_t>(payload.size()));
  std::array<unsigned char, 4> header{};
  std::memcpy(header.data(), &network_length, header.size());
  bool write_ok = true;
  std::thread peer([&] {
    for (unsigned char byte : header) {
      write_ok = write_ok && send_all_for_test(writer.get(), &byte, 1);
    }
    for (unsigned char byte : payload) {
      write_ok = write_ok && send_all_for_test(writer.get(), &byte, 1);
    }
  });
  const FrameReadResult result = read_frame(reader.get());
  peer.join();
  EXPECT_TRUE(write_ok);
  EXPECT_EQ(result.state, FrameReadState::Complete);
  EXPECT_EQ(result.payload, payload);
}

TEST(FrameCodec, DistinguishesCleanAndTruncatedEof) {
  int clean_pair[2] = {-1, -1};
  ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, clean_pair), 0);
  UniqueFd clean_reader(clean_pair[0]);
  UniqueFd clean_writer(clean_pair[1]);
  clean_writer.reset();
  EXPECT_EQ(read_frame(clean_reader.get()).state, FrameReadState::CleanEof);

  int truncated_pair[2] = {-1, -1};
  ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, truncated_pair), 0);
  UniqueFd truncated_reader(truncated_pair[0]);
  UniqueFd truncated_writer(truncated_pair[1]);
  const unsigned char partial[] = {0, 0};
  ASSERT_TRUE(
      send_all_for_test(truncated_writer.get(), partial, sizeof(partial)));
  truncated_writer.reset();
  EXPECT_EQ(read_frame(truncated_reader.get()).state,
            FrameReadState::Truncated);
}

TEST(FrameCodec, RejectsZeroAndOversizedBeforeBody) {
  for (std::uint32_t length : {0U, 16U * 1024U * 1024U + 1U}) {
    int descriptors[2] = {-1, -1};
    ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, descriptors), 0);
    UniqueFd reader(descriptors[0]);
    UniqueFd writer(descriptors[1]);
    const std::uint32_t network_length = htonl(length);
    ASSERT_TRUE(send_all_for_test(
        writer.get(), reinterpret_cast<const unsigned char*>(&network_length),
        sizeof(network_length)));
    EXPECT_EQ(read_frame(reader.get()).state, FrameReadState::InvalidLength);
  }
}

TEST(FrameCodec, TransfersMaximumFrameWithoutDeadlock) {
  int descriptors[2] = {-1, -1};
  ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, descriptors), 0);
  UniqueFd reader(descriptors[0]);
  UniqueFd writer(descriptors[1]);
  const std::string payload(kMaximumFramePayloadBytes, 'x');
  FrameWriteResult written;
  std::thread peer([&] { written = write_frame(writer.get(), payload); });
  const FrameReadResult result = read_frame(reader.get());
  peer.join();
  ASSERT_TRUE(written.ok) << written.message;
  ASSERT_EQ(result.state, FrameReadState::Complete);
  EXPECT_EQ(result.payload.size(), payload.size());
  EXPECT_EQ(result.payload.front(), 'x');
  EXPECT_EQ(result.payload.back(), 'x');
}

TEST(ProtocolEnvelope, RejectsDuplicatesAndMalformedRequests) {
  const JsonParseResult duplicate = parse_json(R"({"id":"one","id":"two"})");
  EXPECT_FALSE(duplicate.ok);
  EXPECT_TRUE(duplicate.duplicate_key);
  EXPECT_TRUE(duplicate.ambiguous_top_level_id);

  std::unique_ptr<Host> host = create_embedded_host();
  ASSERT_NE(host, nullptr);
  RequestRouter router(*host, "0.1.0");
  Json malformed = parse_response(router.route("{"));
  EXPECT_TRUE(malformed["id"].is_null());
  EXPECT_EQ(malformed["error"]["name"], "parse_error");

  Json duplicated = parse_response(router.route(
      R"({"protocol_version":1,"id":"a","id":"b","method":"daemon.ping","params":{}})"));
  EXPECT_TRUE(duplicated["id"].is_null());
  EXPECT_EQ(duplicated["error"]["name"], "invalid_request");

  Json nested_duplicate = parse_response(router.route(
      R"({"protocol_version":1,"id":"nested","method":"daemon.ping","params":{"value":1,"value":2}})"));
  EXPECT_EQ(nested_duplicate["id"], "nested");
  EXPECT_EQ(nested_duplicate["error"]["name"], "invalid_request");
}

TEST(ProtocolEnvelope, NegotiatesVersionAndRejectsUnknownMethodAndSession) {
  std::unique_ptr<Host> host = create_embedded_host();
  ASSERT_NE(host, nullptr);
  RequestRouter router(*host, "0.1.0");
  Json unsupported = parse_response(router.route(
      R"({"protocol_version":2,"id":"v","method":"daemon.ping","params":{}})"));
  EXPECT_EQ(unsupported["id"], "v");
  EXPECT_EQ(unsupported["error"]["name"], "unsupported_protocol");
  EXPECT_EQ(unsupported["error"]["supported_versions"], Json::array({1}));

  Json unknown = parse_response(router.route(
      R"({"protocol_version":1,"id":"m","method":"compute.submit","params":{}})"));
  EXPECT_EQ(unknown["error"]["name"], "method_not_found");

  Json missing = parse_response(router.route(
      R"({"protocol_version":1,"id":"s","method":"inspect.graph","params":{"session_id":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"}})"));
  EXPECT_EQ(missing["error"]["domain"], "graph");
  EXPECT_EQ(missing["error"]["code"], 2);
  EXPECT_EQ(missing["error"]["name"], "not_found");
}

TEST(ProtocolEnvelope, TreatsEveryNonV1IntegerAsUnsupported) {
  std::unique_ptr<Host> host = create_embedded_host();
  ASSERT_NE(host, nullptr);
  RequestRouter router(*host, "0.1.0");
  const std::vector<Json> unsupported_versions = {
      Json(std::uint64_t{4294967297ULL}),
      Json(std::numeric_limits<std::uint64_t>::max()),
      Json(std::numeric_limits<std::int64_t>::min())};
  for (const Json& version : unsupported_versions) {
    const Json response = parse_response(router.route(Json{
        {"protocol_version", version},
        {"id", "unsupported"},
        {"method", "daemon.ping"},
        {"params", Json::object()}}.dump()));
    EXPECT_EQ(response["error"]["domain"], "protocol");
    EXPECT_EQ(response["error"]["name"], "unsupported_protocol");
    EXPECT_EQ(response["error"]["supported_versions"], Json::array({1}));
  }
}

TEST(IntegerCodec, PreservesExactSignedAndUnsignedBoundaries) {
  std::int64_t signed_value = 17;
  EXPECT_TRUE(decode_integer(Json(std::numeric_limits<std::int64_t>::min()),
                             &signed_value));
  EXPECT_EQ(signed_value, std::numeric_limits<std::int64_t>::min());
  EXPECT_TRUE(decode_integer(Json(std::numeric_limits<std::int64_t>::max()),
                             &signed_value));
  EXPECT_EQ(signed_value, std::numeric_limits<std::int64_t>::max());

  std::uint64_t unsigned_value = 19;
  EXPECT_TRUE(decode_integer(Json(std::numeric_limits<std::uint64_t>::max()),
                             &unsigned_value));
  EXPECT_EQ(unsigned_value, std::numeric_limits<std::uint64_t>::max());
  EXPECT_FALSE(decode_integer(Json(std::numeric_limits<std::int64_t>::min()),
                              &unsigned_value));
  EXPECT_EQ(unsigned_value, std::numeric_limits<std::uint64_t>::max());

  int narrow_value = 23;
  EXPECT_FALSE(
      decode_integer(Json(std::uint64_t{4294967296ULL}), &narrow_value));
  EXPECT_EQ(narrow_value, 23);
  EXPECT_FALSE(decode_integer(Json(std::numeric_limits<std::uint64_t>::max()),
                              &signed_value));
  EXPECT_EQ(signed_value, std::numeric_limits<std::int64_t>::max());
}

TEST(IntegerCodec, RejectsEveryNonIntegerCategoryTransactionally) {
  const std::vector<Json> malformed = {
      Json(1.0),
      Json(1.5),
      Json(true),
      Json("1"),
      Json(nullptr),
      Json(std::numeric_limits<double>::infinity()),
      Json(std::numeric_limits<double>::quiet_NaN())};
  for (const Json& value : malformed) {
    std::uint32_t output = 37;
    EXPECT_FALSE(decode_integer(value, &output));
    EXPECT_EQ(output, 37U);
  }

  std::int32_t signed_output = 41;
  EXPECT_TRUE(decode_integer(Json(std::numeric_limits<std::int32_t>::min()),
                             &signed_output));
  EXPECT_EQ(signed_output, std::numeric_limits<std::int32_t>::min());
  EXPECT_TRUE(decode_integer(Json(std::numeric_limits<std::int32_t>::max()),
                             &signed_output));
  EXPECT_EQ(signed_output, std::numeric_limits<std::int32_t>::max());
  EXPECT_FALSE(
      decode_integer(Json(std::uint64_t{2147483648ULL}), &signed_output));
  EXPECT_EQ(signed_output, std::numeric_limits<std::int32_t>::max());

  std::uint32_t unsigned_output = 43;
  EXPECT_TRUE(decode_integer(Json(std::numeric_limits<std::uint32_t>::max()),
                             &unsigned_output));
  EXPECT_EQ(unsigned_output, std::numeric_limits<std::uint32_t>::max());
  EXPECT_FALSE(decode_integer(Json(-1), &unsigned_output));
  EXPECT_EQ(unsigned_output, std::numeric_limits<std::uint32_t>::max());
  EXPECT_FALSE(
      decode_integer(Json(std::uint64_t{4294967296ULL}), &unsigned_output));
  EXPECT_EQ(unsigned_output, std::numeric_limits<std::uint32_t>::max());
}

TEST(OpaqueIdCodec, ValidatesOneExactSharedVersionOneShape) {
  const std::vector<std::pair<std::string, bool>> cases = {
      {"0123456789abcdef0123456789abcdef", true},
      {std::string(32, '0'), true},
      {std::string(32, 'f'), true},
      {std::string(31, 'a'), false},
      {std::string(33, 'a'), false},
      {std::string(32, 'A'), false},
      {std::string(31, 'a') + "g", false},
      {std::string(31, 'a') + "-", false},
      {std::string(15, 'a') + std::string(1, '\0') + std::string(16, 'a'),
       false}};
  for (const auto& test_case : cases) {
    EXPECT_EQ(valid_opaque_id(test_case.first), test_case.second)
        << test_case.first.size();
  }
  EXPECT_EQ(generate_opaque_id().size(), kOpaqueIdHexCharacters);
  EXPECT_TRUE(valid_opaque_id(generate_opaque_id()));

  std::string decoded = "sentinel";
  EXPECT_TRUE(
      decode_opaque_id(Json("0123456789abcdef0123456789abcdef"), &decoded));
  EXPECT_EQ(decoded, "0123456789abcdef0123456789abcdef");
  const std::vector<Json> malformed = {Json(std::string(31, 'a')),
                                       Json(std::string(33, 'a')),
                                       Json(std::string(32, 'A')),
                                       Json(std::string(31, 'a') + "g"),
                                       Json(7),
                                       Json(true),
                                       Json(nullptr),
                                       Json::array(),
                                       Json::object()};
  for (const Json& value : malformed) {
    decoded = "sentinel";
    EXPECT_FALSE(decode_opaque_id(value, &decoded));
    EXPECT_EQ(decoded, "sentinel");
  }
}

TEST(StringCodec, ValidatesTheSharedSessionDisplayNameShape) {
  const std::vector<std::pair<std::string, bool>> cases = {
      {"session", true},
      {std::string(kShortTextMaxBytes, 's'), true},
      {std::string(kShortTextMaxBytes + 1, 's'), false},
      {"", false},
      {".", false},
      {"..", false},
      {"parent/child", false},
      {"parent\\child", false},
      {std::string("safe\0tail", 9), false},
      {std::string("\xc0\xaf", 2), false}};
  for (const auto& test_case : cases) {
    EXPECT_EQ(valid_session_name(test_case.first), test_case.second);
  }
}

TEST(StringCodec, EnforcesUtf8ByteBoundsAndTransactionalArrays) {
  const std::array<std::size_t, 4> limits = {
      kRequestTextMaxBytes, kShortTextMaxBytes, kPathTextMaxBytes,
      kLargeTextMaxBytes};
  for (std::size_t limit : limits) {
    std::string output = "sentinel";
    EXPECT_TRUE(
        decode_bounded_string(Json(std::string(limit, 'a')), limit, &output));
    EXPECT_EQ(output.size(), limit);
    output = "sentinel";
    EXPECT_FALSE(decode_bounded_string(Json(std::string(limit + 1, 'a')), limit,
                                       &output));
    EXPECT_EQ(output, "sentinel");
  }

  const std::string exact_multibyte = std::string(126, 'a') + "\xc3\xa9";
  std::string decoded;
  EXPECT_TRUE(decode_bounded_string(Json(exact_multibyte), kRequestTextMaxBytes,
                                    &decoded));
  EXPECT_EQ(decoded, exact_multibyte);
  const JsonParseResult escaped = parse_json(R"("\u00e9")");
  ASSERT_TRUE(escaped.ok) << escaped.message;
  EXPECT_TRUE(decode_bounded_string(escaped.value, 2, &decoded));
  EXPECT_EQ(decoded, "\xc3\xa9");
  decoded = "unchanged";
  EXPECT_FALSE(decode_bounded_string(escaped.value, 1, &decoded));
  EXPECT_EQ(decoded, "unchanged");
  const std::vector<std::string> invalid_utf8 = {
      std::string("\xc0\xaf", 2), std::string("\xed\xa0\x80", 3),
      std::string("\xf4\x90\x80\x80", 4), std::string("\xe2\x82", 2)};
  for (const std::string& malformed : invalid_utf8) {
    decoded = "unchanged";
    EXPECT_FALSE(
        decode_bounded_string(Json(malformed), kShortTextMaxBytes, &decoded));
    EXPECT_EQ(decoded, "unchanged");
    EXPECT_FALSE(valid_utf8(malformed));
  }

  Json paths = Json::array();
  for (std::size_t index = 0; index < kPathArrayMaxEntries; ++index) {
    paths.push_back("/path");
  }
  std::vector<std::string> decoded_paths = {"sentinel"};
  EXPECT_TRUE(decode_bounded_string_array(paths, kPathArrayMaxEntries,
                                          kPathTextMaxBytes, &decoded_paths));
  EXPECT_EQ(decoded_paths.size(), kPathArrayMaxEntries);
  paths.push_back("/overflow");
  decoded_paths = {"sentinel"};
  EXPECT_FALSE(decode_bounded_string_array(paths, kPathArrayMaxEntries,
                                           kPathTextMaxBytes, &decoded_paths));
  EXPECT_EQ(decoded_paths, std::vector<std::string>({"sentinel"}));

  for (std::size_t malformed_index = 0; malformed_index < 3;
       ++malformed_index) {
    Json malformed_element = Json::array({"first", "middle", "last"});
    malformed_element[malformed_index] = 7;
    decoded_paths = {"sentinel"};
    EXPECT_FALSE(
        decode_bounded_string_array(malformed_element, kGeneralPageMaxEntries,
                                    kShortTextMaxBytes, &decoded_paths));
    EXPECT_EQ(decoded_paths, std::vector<std::string>({"sentinel"}));
  }

  Json page = Json::array();
  for (std::size_t index = 0; index < kGeneralPageMaxEntries; ++index) {
    page.push_back(nullptr);
  }
  EXPECT_TRUE(valid_bounded_array(page, kGeneralPageMaxEntries));
  page.push_back(nullptr);
  EXPECT_FALSE(valid_bounded_array(page, kGeneralPageMaxEntries));
  EXPECT_FALSE(valid_bounded_array(Json::object(), kGeneralPageMaxEntries));
}

TEST(PageCodec, ValidatesLimitsAndOverflowWithoutPublishingPartialValues) {
  const std::array<std::pair<std::size_t, std::size_t>, 2> ranges = {{
      {kComputeEventDrainMinLimit, kComputeEventDrainMaxLimit},
      {kSchedulerTraceMinLimit, kSchedulerTraceMaxLimit},
  }};
  for (const auto& range : ranges) {
    std::size_t output = 17;
    EXPECT_TRUE(decode_page_limit(Json(range.first), range.first, range.second,
                                  &output));
    EXPECT_EQ(output, range.first);
    EXPECT_TRUE(decode_page_limit(Json(range.second), range.first, range.second,
                                  &output));
    EXPECT_EQ(output, range.second);
    output = 17;
    EXPECT_FALSE(decode_page_limit(Json(range.first - 1), range.first,
                                   range.second, &output));
    EXPECT_EQ(output, 17U);
    EXPECT_FALSE(decode_page_limit(Json(range.second + 1), range.first,
                                   range.second, &output));
    EXPECT_EQ(output, 17U);
  }

  std::size_t offset = 23;
  std::size_t limit = 29;
  EXPECT_TRUE(
      decode_page_window(Json(std::numeric_limits<std::size_t>::max() - 1),
                         Json(1), kGeneralPageMaxEntries, &offset, &limit));
  EXPECT_EQ(offset, std::numeric_limits<std::size_t>::max() - 1);
  EXPECT_EQ(limit, 1U);
  offset = 23;
  limit = 29;
  EXPECT_FALSE(decode_page_window(Json(std::numeric_limits<std::size_t>::max()),
                                  Json(1), kGeneralPageMaxEntries, &offset,
                                  &limit));
  EXPECT_EQ(offset, 23U);
  EXPECT_EQ(limit, 29U);
  EXPECT_FALSE(decode_page_window(Json(-1), Json(1), kGeneralPageMaxEntries,
                                  &offset, &limit));
  EXPECT_FALSE(decode_page_window(Json(0), Json(0), kGeneralPageMaxEntries,
                                  &offset, &limit));
  EXPECT_FALSE(decode_page_window(Json(0), Json(1.5), kGeneralPageMaxEntries,
                                  &offset, &limit));
}

/**
 * @brief Proves the reusable graph-session page boundary and row schema.
 *
 * The exact 4,096-row page must round-trip while preserving order, and an
 * unknown row member must be ignored. Both outbound and inbound 4,097-row
 * pages are rejected, and malformed rows leave a previously published result
 * unchanged.
 */
TEST(PageCodec, BoundsGraphSessionSummariesAndDecodesTransactionally) {
  const GraphSessionSummary summary{
      IpcSessionId{"0123456789abcdef0123456789abcdef"}, "session"};
  const std::vector<GraphSessionSummary> exact_page(kGeneralPageMaxEntries,
                                                    summary);
  Json encoded = encode_session_summaries(exact_page);
  ASSERT_EQ(encoded.size(), kGeneralPageMaxEntries);
  encoded.front()["future_member"] = Json{{"nested", true}};

  std::vector<GraphSessionSummary> decoded;
  std::string message = "stale";
  ASSERT_TRUE(decode_session_summaries(encoded, &decoded, &message)) << message;
  ASSERT_EQ(decoded.size(), kGeneralPageMaxEntries);
  EXPECT_EQ(decoded.front().session_id.value, summary.session_id.value);
  EXPECT_EQ(decoded.front().session_name, summary.session_name);

  std::vector<GraphSessionSummary> oversized = exact_page;
  oversized.push_back(summary);
  EXPECT_THROW((void)encode_session_summaries(oversized), std::length_error);

  encoded.push_back(encoded.front());
  std::vector<GraphSessionSummary> unchanged = {
      {IpcSessionId{"ffffffffffffffffffffffffffffffff"}, "sentinel"}};
  message = "stale";
  EXPECT_FALSE(decode_session_summaries(encoded, &unchanged, &message));
  ASSERT_EQ(unchanged.size(), 1U);
  EXPECT_EQ(unchanged.front().session_name, "sentinel");

  const std::vector<Json> malformed_rows = {
      Json::object(), Json::array({Json{{"session_name", "missing-id"}}}),
      Json::array({Json{{"session_id", std::string(32, 'A')},
                        {"session_name", "uppercase-id"}}}),
      Json::array({Json{{"session_id", summary.session_id.value},
                        {"session_name", "unsafe/name"}}}),
      Json::array({Json{{"session_id", summary.session_id.value},
                        {"session_name", 7}}})};
  for (const Json& malformed : malformed_rows) {
    unchanged = {
        {IpcSessionId{"ffffffffffffffffffffffffffffffff"}, "sentinel"}};
    message = "stale";
    EXPECT_FALSE(decode_session_summaries(malformed, &unchanged, &message));
    ASSERT_EQ(unchanged.size(), 1U);
    EXPECT_EQ(unchanged.front().session_id.value,
              "ffffffffffffffffffffffffffffffff");
    EXPECT_EQ(unchanged.front().session_name, "sentinel");
    EXPECT_FALSE(message.empty());
  }

  const std::vector<GraphSessionSummary> invalid_rows = {
      {IpcSessionId{std::string(31, 'a')}, "session"},
      {IpcSessionId{summary.session_id.value}, "unsafe/name"}};
  for (const GraphSessionSummary& invalid : invalid_rows) {
    EXPECT_THROW((void)encode_session_summaries({invalid}),
                 std::invalid_argument);
  }
}

TEST(PixelRectCodec, PreservesEveryIntBoundaryAndIgnoresUnknownFields) {
  PixelRect original{std::numeric_limits<int>::min(),
                     std::numeric_limits<int>::max(), -7, 0};
  Json encoded = encode_pixel_rect(original);
  encoded["future_field"] = true;
  PixelRect decoded{1, 2, 3, 4};
  ASSERT_TRUE(decode_pixel_rect(encoded, &decoded));
  EXPECT_EQ(decoded.x, original.x);
  EXPECT_EQ(decoded.y, original.y);
  EXPECT_EQ(decoded.width, original.width);
  EXPECT_EQ(decoded.height, original.height);

  const std::vector<Json> malformed = [&] {
    std::vector<Json> values;
    Json missing = encoded;
    missing.erase("width");
    values.push_back(std::move(missing));
    Json fractional = encoded;
    fractional["x"] = 1.5;
    values.push_back(std::move(fractional));
    Json overflow = encoded;
    overflow["height"] = std::uint64_t{4294967296ULL};
    values.push_back(std::move(overflow));
    Json wrong_type = encoded;
    wrong_type["y"] = "zero";
    values.push_back(std::move(wrong_type));
    return values;
  }();
  for (const Json& value : malformed) {
    PixelRect unchanged{11, 12, 13, 14};
    EXPECT_FALSE(decode_pixel_rect(value, &unchanged));
    EXPECT_EQ(unchanged.x, 11);
    EXPECT_EQ(unchanged.y, 12);
    EXPECT_EQ(unchanged.width, 13);
    EXPECT_EQ(unchanged.height, 14);
  }
}

TEST(EnumCodec, RoundTripsEveryDefinedVersionOneLabel) {
  expect_enum_codec(std::array<std::pair<ComputeIntent, const char*>, 2>{{
      {ComputeIntent::GlobalHighPrecision, "global_high_precision"},
      {ComputeIntent::RealTimeUpdate, "real_time_update"},
  }});
  expect_enum_codec(std::array<std::pair<DirtyDomain, const char*>, 2>{{
      {DirtyDomain::HighPrecision, "high_precision"},
      {DirtyDomain::RealTime, "real_time"},
  }});
  expect_enum_codec(
      std::array<std::pair<DirtySourceLifecycleState, const char*>, 3>{{
          {DirtySourceLifecycleState::Idle, "idle"},
          {DirtySourceLifecycleState::Updating, "updating"},
          {DirtySourceLifecycleState::Settled, "settled"},
      }});
  expect_enum_codec(std::array<std::pair<DirtyEdgeDirection, const char*>, 2>{{
      {DirtyEdgeDirection::ForwardAffected, "forward_affected"},
      {DirtyEdgeDirection::BackwardDemand, "backward_demand"},
  }});
  expect_enum_codec(std::array<std::pair<HostGraphEdgeKind, const char*>, 2>{{
      {HostGraphEdgeKind::ImageInput, "image_input"},
      {HostGraphEdgeKind::ParameterInput, "parameter_input"},
  }});
  expect_enum_codec(
      std::array<std::pair<HostDependencyTreeScope, const char*>, 2>{{
          {HostDependencyTreeScope::EndingNodes, "ending_nodes"},
          {HostDependencyTreeScope::StartNode, "start_node"},
      }});
  expect_enum_codec(std::array<std::pair<HostSchedulerTraceAction, const char*>,
                               9>{{
      {HostSchedulerTraceAction::AssignInitial, "assign_initial"},
      {HostSchedulerTraceAction::Execute, "execute"},
      {HostSchedulerTraceAction::ExecuteTile, "execute_tile"},
      {HostSchedulerTraceAction::ExecuteDirtySource, "execute_dirty_source"},
      {HostSchedulerTraceAction::ExecuteDirtyDownstreamNode,
       "execute_dirty_downstream_node"},
      {HostSchedulerTraceAction::ExecuteDirtyDownstreamTile,
       "execute_dirty_downstream_tile"},
      {HostSchedulerTraceAction::SkipStaleGeneration, "skip_stale_generation"},
      {HostSchedulerTraceAction::RethrowException, "rethrow_exception"},
      {HostSchedulerTraceAction::Unknown, "unknown"},
  }});
  expect_enum_codec(std::array<std::pair<DataType, const char*>, 6>{{
      {DataType::UINT8, "uint8"},
      {DataType::INT8, "int8"},
      {DataType::UINT16, "uint16"},
      {DataType::INT16, "int16"},
      {DataType::FLOAT32, "float32"},
      {DataType::FLOAT64, "float64"},
  }});
  expect_enum_codec(std::array<std::pair<Device, const char*>, 4>{{
      {Device::CPU, "cpu"},
      {Device::GPU_METAL, "gpu_metal"},
      {Device::GPU_CUDA, "gpu_cuda"},
      {Device::ASIC_NPU, "asic_npu"},
  }});
}

TEST(ProtocolEnvelope, EnforcesRequestAndMethodUtf8ByteBounds) {
  std::unique_ptr<Host> host = create_embedded_host();
  ASSERT_NE(host, nullptr);
  RequestRouter router(*host, "0.1.0");

  const std::string exact_id(kRequestTextMaxBytes, 'i');
  const Json exact_id_response = parse_response(router.route(Json{
      {"protocol_version", 1},
      {"id", exact_id},
      {"method", "unknown"},
      {"params", Json::object()}}.dump()));
  EXPECT_EQ(exact_id_response["id"], exact_id);
  EXPECT_EQ(exact_id_response["error"]["name"], "method_not_found");

  const Json long_id_response = parse_response(router.route(Json{
      {"protocol_version", 1},
      {"id", std::string(kRequestTextMaxBytes + 1, 'i')},
      {"method", "unknown"},
      {"params", Json::object()}}.dump()));
  EXPECT_TRUE(long_id_response["id"].is_null());
  EXPECT_EQ(long_id_response["error"]["name"], "invalid_request");

  const Json exact_method_response = parse_response(router.route(Json{
      {"protocol_version", 1},
      {"id", "method-exact"},
      {"method", std::string(kRequestTextMaxBytes, 'm')},
      {"params", Json::object()}}.dump()));
  EXPECT_EQ(exact_method_response["error"]["name"], "method_not_found");
  const Json long_method_response = parse_response(router.route(Json{
      {"protocol_version", 1},
      {"id", "method-long"},
      {"method", std::string(kRequestTextMaxBytes + 1, 'm')},
      {"params", Json::object()}}.dump()));
  EXPECT_EQ(long_method_response["id"], "method-long");
  EXPECT_EQ(long_method_response["error"]["name"], "invalid_request");
}

TEST(ProtocolParams, IgnoresUnknownFieldsButStillValidatesKnownFields) {
  std::unique_ptr<Host> host = create_embedded_host();
  ASSERT_NE(host, nullptr);
  RequestRouter router(*host, "0.1.0");
  for (const std::string& method :
       {std::string("daemon.ping"), std::string("daemon.version"),
        std::string("graph.list")}) {
    const Json response = parse_response(router.route(Json{
        {"protocol_version", 1},
        {"id", method},
        {"method", method},
        {"future_envelope", Json{{"nested", true}}},
        {"params",
         Json{{"future_field", Json{{"nested", true}}}}}}.dump()));
    EXPECT_TRUE(response.contains("result")) << response.dump();
  }
  const Json malformed_known = parse_response(router.route(Json{
      {"protocol_version", 1},
      {"id", "known"},
      {"method", "inspect.node"},
      {"params", Json{{"session_id", std::string(32, 'a')},
                      {"node_id", "wrong"},
                      {"future_field", true}}}}.dump()));
  EXPECT_EQ(malformed_known["error"]["name"], "invalid_params");
}

TEST(ProtocolParams, EnforcesSessionAndFilesystemPathByteBounds) {
  ScopedTempDirectory temp("ps-ipc-text-bounds");
  std::unique_ptr<Host> host = create_embedded_host();
  ASSERT_NE(host, nullptr);
  RequestRouter router(*host, "0.1.0");

  const Json exact_session = parse_response(router.route(Json{
      {"protocol_version", 1},
      {"id", "session-exact"},
      {"method", "graph.load"},
      {"params", Json{{"session_name", std::string(kShortTextMaxBytes, 's')},
                      {"root_dir", temp.path().string()}}}}.dump()));
  EXPECT_TRUE(exact_session.contains("result") ||
              exact_session["error"]["name"] != "invalid_params")
      << exact_session.dump();

  const Json long_session = parse_response(router.route(Json{
      {"protocol_version", 1},
      {"id", "session-long"},
      {"method", "graph.load"},
      {"params",
       Json{{"session_name", std::string(kShortTextMaxBytes + 1, 's')},
            {"root_dir", temp.path().string()}}}}.dump()));
  EXPECT_EQ(long_session["error"]["name"], "invalid_params");

  const std::string exact_path = "/" + std::string(kPathTextMaxBytes - 1, 'p');
  const Json accepted_path = parse_response(router.route(Json{
      {"protocol_version", 1},
      {"id", "path-exact"},
      {"method", "graph.load"},
      {"params", Json{{"session_name", "path_exact"},
                      {"root_dir", exact_path}}}}.dump()));
  EXPECT_TRUE(accepted_path.contains("result") ||
              accepted_path["error"]["name"] != "invalid_params")
      << accepted_path.dump();

  const Json long_path = parse_response(router.route(Json{
      {"protocol_version", 1},
      {"id", "path-long"},
      {"method", "graph.load"},
      {"params", Json{{"session_name", "path_long"},
                      {"root_dir", exact_path + "p"}}}}.dump()));
  EXPECT_EQ(long_path["error"]["name"], "invalid_params");

  const Json nul_session = parse_response(router.route(Json{
      {"protocol_version", 1},
      {"id", "session-nul"},
      {"method", "graph.load"},
      {"params", Json{{"session_name", std::string("safe\0tail", 9)},
                      {"root_dir", temp.path().string()}}}}.dump()));
  EXPECT_EQ(nul_session["error"]["name"], "invalid_params");
  router.begin_shutdown();
  router.finish_shutdown();
}

TEST(ProtocolParams, RejectsInvalidValuesWithoutHostMutation) {
  ScopedTempDirectory temp("photospider-ipc-invalid-params");
  std::unique_ptr<Host> host = create_embedded_host();
  ASSERT_NE(host, nullptr);
  RequestRouter router(*host, "0.1.0");
  const std::string valid_session_id(32, 'a');
  const std::filesystem::path untouched_root = temp.path() / "untouched";
  const std::vector<Json> requests = {
      Json{{"protocol_version", 1},
           {"id", "unsafe-name"},
           {"method", "graph.load"},
           {"params", Json{{"session_name", "../unsafe"},
                           {"root_dir", untouched_root.string()}}}},
      Json{{"protocol_version", 1},
           {"id", "relative-root"},
           {"method", "graph.load"},
           {"params",
            Json{{"session_name", "safe"}, {"root_dir", "relative/root"}}}},
      Json{{"protocol_version", 1},
           {"id", "negative-node"},
           {"method", "inspect.node"},
           {"params", Json{{"session_id", valid_session_id}, {"node_id", -1}}}},
      Json{{"protocol_version", 1},
           {"id", "wrong-node-type"},
           {"method", "inspect.node"},
           {"params",
            Json{{"session_id", valid_session_id}, {"node_id", "one"}}}},
      Json{{"protocol_version", 1},
           {"id", "overflow-node"},
           {"method", "inspect.node"},
           {"params", Json{{"session_id", valid_session_id},
                           {"node_id", std::uint64_t{4294967296ULL}}}}},
      Json{{"protocol_version", 1},
           {"id", "overflow-tree-node"},
           {"method", "inspect.dependency_tree"},
           {"params", Json{{"session_id", valid_session_id},
                           {"node_id", std::uint64_t{4294967296ULL}}}}}};
  for (const Json& request : requests) {
    const Json response = parse_response(router.route(request.dump()));
    EXPECT_EQ(response["error"]["domain"], "protocol");
    EXPECT_EQ(response["error"]["name"], "invalid_params");
  }
  const Result<std::vector<GraphSessionId>> sessions = host->list_graphs();
  ASSERT_TRUE(sessions.status.ok);
  EXPECT_TRUE(sessions.value.empty());
  EXPECT_FALSE(std::filesystem::exists(untouched_root));
}

TEST(ProtocolGraphLoad, FailedHostLoadReleasesNameForRetry) {
  ScopedTempDirectory temp("photospider-ipc-load-rollback");
  std::unique_ptr<Host> host = create_embedded_host();
  ASSERT_NE(host, nullptr);
  RequestRouter router(*host, "0.1.0");
  const std::filesystem::path yaml_path = temp.path() / "graph.yaml";
  {
    std::ofstream invalid(yaml_path);
    invalid.exceptions(std::ios::badbit | std::ios::failbit);
    invalid << "- id: [unterminated\n";
  }
  const Json params{{"session_name", "retry_session"},
                    {"root_dir", (temp.path() / "sessions").string()},
                    {"yaml_path", yaml_path.string()}};
  const Json first = parse_response(router.route(Json{
      {"protocol_version", 1},
      {"id", "first"},
      {"method", "graph.load"},
      {"params", params}}.dump()));
  EXPECT_EQ(first["error"]["domain"], "graph");
  const Result<std::vector<GraphSessionId>> after_failure = host->list_graphs();
  ASSERT_TRUE(after_failure.status.ok);
  EXPECT_TRUE(after_failure.value.empty());

  {
    std::ofstream output(yaml_path);
    output.exceptions(std::ios::badbit | std::ios::failbit);
    output << "- id: 1\n"
              "  name: retry_source\n"
              "  type: ipc_fixture\n"
              "  subtype: source\n"
              "  parameters:\n"
              "    width: 6\n"
              "    height: 4\n";
  }
  const Json second = parse_response(router.route(Json{
      {"protocol_version", 1},
      {"id", "second"},
      {"method", "graph.load"},
      {"params", params}}.dump()));
  ASSERT_TRUE(second.contains("result"));
  EXPECT_EQ(second["result"]["session_name"], "retry_session");
  router.begin_shutdown();
  router.finish_shutdown();
}

/**
 * @brief Verifies non-NotFound close failure retains graph and opaque mapping.
 *
 * @throws Nothing when real Host, router, and scheduler fixture behavior holds;
 *         GoogleTest records any mismatch.
 * @note The fixture throws GraphError::Io from scheduler shutdown. The close
 *       boundary must preserve that exact category while the same opaque id is
 *       listed and remains usable until a later successful close removes the
 *       mapping.
 */
TEST(ProtocolGraphClose, ShutdownFailureRetainsMappingAndAllowsRetry) {
  ScopedTempDirectory temp("photospider-ipc-close-failure");
  ScopedSchedulerPluginCleanup scheduler_cleanup;
  std::unique_ptr<Host> host = create_embedded_host();
  ASSERT_NE(host, nullptr);

  const std::filesystem::path plugin_path =
      destroy_count_scheduler_plugin_path();
  ASSERT_TRUE(std::filesystem::exists(plugin_path))
      << "scheduler close-failure fixture was not built: " << plugin_path;
  const VoidResult plugin_load = host->scheduler_load(plugin_path.string());
  ASSERT_TRUE(plugin_load.status.ok) << plugin_load.status.message;

  HostSchedulerConfig scheduler_config;
  scheduler_config.hp_type = kDestroyCountSchedulerType;
  scheduler_config.rt_type = "serial_debug";
  const VoidResult configured =
      host->configure_scheduler_defaults(scheduler_config);
  ASSERT_TRUE(configured.status.ok) << configured.status.message;

  RequestRouter router(*host, "0.1.0");
  const Json load_response = parse_response(router.route(
      Json{{"protocol_version", 1},
           {"id", "load"},
           {"method", "graph.load"},
           {"params", Json{{"session_name", "close_failure_retry"},
                           {"root_dir", (temp.path() / "sessions").string()}}}}
          .dump()));
  ASSERT_TRUE(load_response.contains("result")) << load_response.dump();
  const std::string session_id =
      load_response["result"]["session_id"].get<std::string>();

  {
    ScopedEnvironmentValue failure(kSchedulerFailureEnvironment,
                                   "shutdown_graph_io");
    const Json failed_close = parse_response(router.route(Json{
        {"protocol_version", 1},
        {"id", "close-failed"},
        {"method", "graph.close"},
        {"params", Json{{"session_id", session_id}}}}.dump()));
    ASSERT_TRUE(failed_close.contains("error")) << failed_close.dump();
    EXPECT_EQ(failed_close["error"]["domain"], "graph");
    EXPECT_EQ(failed_close["error"]["code"],
              static_cast<std::int32_t>(GraphErrc::Io));
    EXPECT_EQ(failed_close["error"]["name"], "io");
    EXPECT_NE(failed_close["error"]["message"].get<std::string>().find(
                  "fixture shutdown graph-io failure"),
              std::string::npos);
  }

  const Result<GraphInspectionView> inspected_after_failure =
      host->inspect_graph(GraphSessionId{"close_failure_retry"});
  ASSERT_TRUE(inspected_after_failure.status.ok)
      << inspected_after_failure.status.message;

  const Json listed = parse_response(router.route(Json{
      {"protocol_version", 1},
      {"id", "list-after-failure"},
      {"method", "graph.list"},
      {"params", Json::object()}}.dump()));
  ASSERT_TRUE(listed.contains("result")) << listed.dump();
  ASSERT_EQ(listed["result"]["sessions"].size(), 1u);
  EXPECT_EQ(listed["result"]["sessions"][0]["session_id"], session_id);
  EXPECT_EQ(listed["result"]["sessions"][0]["session_name"],
            "close_failure_retry");

  const Json retry_close = parse_response(router.route(Json{
      {"protocol_version", 1},
      {"id", "close-retry"},
      {"method", "graph.close"},
      {"params", Json{{"session_id", session_id}}}}.dump()));
  ASSERT_TRUE(retry_close.contains("result")) << retry_close.dump();
  EXPECT_TRUE(retry_close["result"]["closed"].get<bool>());

  const Json missing_retry = parse_response(router.route(Json{
      {"protocol_version", 1},
      {"id", "close-missing"},
      {"method", "graph.close"},
      {"params", Json{{"session_id", session_id}}}}.dump()));
  ASSERT_TRUE(missing_retry.contains("error")) << missing_retry.dump();
  EXPECT_EQ(missing_retry["error"]["domain"], "graph");
  EXPECT_EQ(missing_retry["error"]["name"], "not_found");
}

TEST(ProtocolErrors, PreservesEveryGraphErrcCodeAndName) {
  const std::vector<std::pair<GraphErrc, std::string>> expected = {
      {GraphErrc::Unknown, "unknown"},
      {GraphErrc::NotFound, "not_found"},
      {GraphErrc::Cycle, "cycle"},
      {GraphErrc::Io, "io"},
      {GraphErrc::InvalidYaml, "invalid_yaml"},
      {GraphErrc::MissingDependency, "missing_dependency"},
      {GraphErrc::NoOperation, "no_operation"},
      {GraphErrc::InvalidParameter, "invalid_parameter"},
      {GraphErrc::ComputeError, "compute_error"},
  };
  for (std::size_t index = 0; index < expected.size(); ++index) {
    const OperationStatus status = graph_status(
        OperationStatus{false, OperationErrorDomain::Graph,
                        static_cast<std::int32_t>(expected[index].first),
                        "ignored", "diagnostic"});
    EXPECT_EQ(status.domain, OperationErrorDomain::Graph);
    EXPECT_EQ(status.code, static_cast<std::int32_t>(index + 1));
    EXPECT_EQ(status.name, expected[index].second);
    EXPECT_EQ(status.message, "diagnostic");
  }
}

TEST(OperationStatusModel, CanonicalizesSuccessAndPreservesOtherDomains) {
  const OperationStatus success = ok_status();
  EXPECT_TRUE(success.ok);
  EXPECT_EQ(success.domain, OperationErrorDomain::None);
  EXPECT_EQ(success.code, 0);
  EXPECT_TRUE(success.name.empty());
  EXPECT_TRUE(success.message.empty());
  EXPECT_FALSE(checked_graph_error_code(success).has_value());

  const std::array<OperationErrorDomain, 3> domains = {
      OperationErrorDomain::Transport, OperationErrorDomain::Protocol,
      OperationErrorDomain::Daemon};
  for (const OperationErrorDomain domain : domains) {
    const OperationStatus original =
        failure_status(domain, -17, "future_name", "diagnostic");
    const OperationStatus preserved = graph_status(original);
    EXPECT_FALSE(preserved.ok);
    EXPECT_EQ(preserved.domain, domain);
    EXPECT_EQ(preserved.code, -17);
    EXPECT_EQ(preserved.name, "future_name");
    EXPECT_EQ(preserved.message, "diagnostic");
    EXPECT_FALSE(checked_graph_error_code(preserved).has_value());
  }
}

TEST(OperationStatusModel, ErrorCodecPreservesProtocolGraphAndDaemonValues) {
  const std::array<OperationStatus, 3> statuses = {
      failure_status(OperationErrorDomain::Protocol, -32099, "future_protocol",
                     "protocol diagnostic"),
      failure_status(OperationErrorDomain::Graph, 9, "compute_error",
                     "graph diagnostic"),
      failure_status(OperationErrorDomain::Daemon, -32100, "future_daemon",
                     "daemon diagnostic")};
  for (const OperationStatus& original : statuses) {
    OperationStatus decoded;
    std::string message;
    ASSERT_TRUE(decode_error(encode_error(original), &decoded, &message))
        << message;
    EXPECT_EQ(decoded.ok, original.ok);
    EXPECT_EQ(decoded.domain, original.domain);
    EXPECT_EQ(decoded.code, original.code);
    EXPECT_EQ(decoded.name, original.name);
    EXPECT_EQ(decoded.message, original.message);
  }
}

TEST(OperationStatusModel, EnforcesEveryKnownCodeNamePairTransactionally) {
  /** @brief One expected stable version 1 error identity. */
  struct Mapping {
    /** @brief Public failure domain represented by the mapping. */
    OperationErrorDomain domain;
    /** @brief Lowercase wire spelling of `domain`. */
    const char* domain_name;
    /** @brief Stable signed numeric code. */
    std::int32_t code;
    /** @brief Stable lowercase name paired with `code`. */
    const char* name;
  };
  const std::array<Mapping, 22> mappings{{
      {OperationErrorDomain::Protocol, "protocol", kParseErrorCode,
       "parse_error"},
      {OperationErrorDomain::Protocol, "protocol", kInvalidRequestCode,
       "invalid_request"},
      {OperationErrorDomain::Protocol, "protocol", kMethodNotFoundCode,
       "method_not_found"},
      {OperationErrorDomain::Protocol, "protocol", kInvalidParamsCode,
       "invalid_params"},
      {OperationErrorDomain::Protocol, "protocol", kUnsupportedProtocolCode,
       "unsupported_protocol"},
      {OperationErrorDomain::Protocol, "protocol", kResponseTooLargeCode,
       "response_too_large"},
      {OperationErrorDomain::Graph, "graph", 1, "unknown"},
      {OperationErrorDomain::Graph, "graph", 2, "not_found"},
      {OperationErrorDomain::Graph, "graph", 3, "cycle"},
      {OperationErrorDomain::Graph, "graph", 4, "io"},
      {OperationErrorDomain::Graph, "graph", 5, "invalid_yaml"},
      {OperationErrorDomain::Graph, "graph", 6, "missing_dependency"},
      {OperationErrorDomain::Graph, "graph", 7, "no_operation"},
      {OperationErrorDomain::Graph, "graph", 8, "invalid_parameter"},
      {OperationErrorDomain::Graph, "graph", 9, "compute_error"},
      {OperationErrorDomain::Daemon, "daemon", kInternalErrorCode,
       "internal_error"},
      {OperationErrorDomain::Daemon, "daemon", kJobNotFoundCode,
       "job_not_found"},
      {OperationErrorDomain::Daemon, "daemon", kJobNotReadyCode,
       "job_not_ready"},
      {OperationErrorDomain::Daemon, "daemon", kCapacityExceededCode,
       "capacity_exceeded"},
      {OperationErrorDomain::Daemon, "daemon", kArtifactNotFoundCode,
       "artifact_not_found"},
      {OperationErrorDomain::Daemon, "daemon", kArtifactLimitExceededCode,
       "artifact_limit_exceeded"},
      {OperationErrorDomain::Daemon, "daemon", kCursorNotFoundCode,
       "cursor_not_found"},
  }};
  for (const Mapping& mapping : mappings) {
    const Json encoded = encode_error(failure_status(
        mapping.domain, mapping.code, "wrong_input_name", "diagnostic"));
    EXPECT_EQ(encoded["name"], mapping.name);
    OperationStatus decoded;
    std::string message;
    ASSERT_TRUE(decode_error(Json{{"domain", mapping.domain_name},
                                  {"code", mapping.code},
                                  {"name", mapping.name},
                                  {"message", "diagnostic"},
                                  {"future_field", true}},
                             &decoded, &message))
        << message;
    EXPECT_EQ(decoded.domain, mapping.domain);
    EXPECT_EQ(decoded.code, mapping.code);
    EXPECT_EQ(decoded.name, mapping.name);

    OperationStatus unchanged = failure_status(OperationErrorDomain::Daemon, 77,
                                               "sentinel", "unchanged");
    EXPECT_FALSE(decode_error(Json{{"domain", mapping.domain_name},
                                   {"code", mapping.code},
                                   {"name", "wrong_name"},
                                   {"message", "diagnostic"}},
                              &unchanged, &message));
    EXPECT_EQ(unchanged.code, 77);
    EXPECT_EQ(unchanged.name, "sentinel");

    unchanged = failure_status(OperationErrorDomain::Daemon, 79, "sentinel",
                               "unchanged");
    EXPECT_FALSE(decode_error(Json{{"domain", mapping.domain_name},
                                   {"code", mapping.code + 1000},
                                   {"name", mapping.name},
                                   {"message", "diagnostic"}},
                              &unchanged, &message));
    EXPECT_EQ(unchanged.code, 79);
    EXPECT_EQ(unchanged.name, "sentinel");
  }

  OperationStatus future;
  std::string message;
  ASSERT_TRUE(decode_error(Json{{"domain", "daemon"},
                                {"code", -32199},
                                {"name", "future_daemon"},
                                {"message", "future diagnostic"}},
                           &future, &message));
  EXPECT_EQ(future.code, -32199);
  EXPECT_EQ(future.name, "future_daemon");
}

TEST(OperationStatusModel, NestedCodecRequiresCanonicalTransactionalStatus) {
  const std::array<OperationStatus, 4> statuses = {
      ok_status(),
      failure_status(OperationErrorDomain::Protocol, kInvalidParamsCode,
                     "invalid_params", "protocol diagnostic"),
      failure_status(OperationErrorDomain::Graph, 4, "io", "graph diagnostic"),
      failure_status(OperationErrorDomain::Daemon, kCapacityExceededCode,
                     "capacity_exceeded", "daemon diagnostic")};
  for (const OperationStatus& status : statuses) {
    Json encoded = encode_operation_status(status);
    encoded["future_field"] = Json{{"nested", true}};
    OperationStatus decoded = failure_status(OperationErrorDomain::Daemon, 91,
                                             "sentinel", "unchanged");
    std::string message;
    ASSERT_TRUE(decode_operation_status(encoded, &decoded, &message))
        << message;
    EXPECT_EQ(decoded.ok, status.ok);
    EXPECT_EQ(decoded.domain, status.domain);
    EXPECT_EQ(decoded.code, status.code);
    EXPECT_EQ(decoded.name, status.name);
    EXPECT_EQ(decoded.message, status.message);
  }

  const std::vector<Json> malformed = {Json{{"ok", true},
                                            {"domain", "graph"},
                                            {"code", 0},
                                            {"name", ""},
                                            {"message", ""}},
                                       Json{{"ok", true},
                                            {"domain", "none"},
                                            {"code", 0},
                                            {"name", "stale"},
                                            {"message", ""}},
                                       Json{{"ok", false},
                                            {"domain", "transport"},
                                            {"code", 1},
                                            {"name", "connect_failed"},
                                            {"message", "diagnostic"}},
                                       Json{{"ok", false},
                                            {"domain", "daemon"},
                                            {"code", kJobNotFoundCode},
                                            {"name", "job_not_ready"},
                                            {"message", "diagnostic"}},
                                       Json{{"ok", "false"},
                                            {"domain", "daemon"},
                                            {"code", kJobNotFoundCode},
                                            {"name", "job_not_found"},
                                            {"message", "diagnostic"}}};
  for (const Json& value : malformed) {
    OperationStatus unchanged = failure_status(OperationErrorDomain::Daemon, 93,
                                               "sentinel", "unchanged");
    std::string message;
    EXPECT_FALSE(decode_operation_status(value, &unchanged, &message));
    EXPECT_EQ(unchanged.code, 93);
    EXPECT_EQ(unchanged.name, "sentinel");
  }
}

TEST(OperationStatusModel, BoundsDiagnosticsAtCompleteUtf8Scalars) {
  const std::string long_multibyte =
      std::string(kDiagnosticTextMaxBytes - 1, 'a') + "\xc3\xa9";
  const std::string bounded = bounded_diagnostic(long_multibyte);
  EXPECT_LE(bounded.size(), kDiagnosticTextMaxBytes);
  EXPECT_TRUE(valid_utf8(bounded));
  EXPECT_NE(bounded.find("[truncated]"), std::string::npos);

  const std::string invalid =
      std::string("prefix-") + std::string("\xf0\x28\x8c\x28", 4) + "-suffix";
  const std::string repaired = bounded_diagnostic(invalid);
  EXPECT_LE(repaired.size(), kDiagnosticTextMaxBytes);
  EXPECT_TRUE(valid_utf8(repaired));

  const Json encoded = encode_error(failure_status(
      OperationErrorDomain::Daemon, kInternalErrorCode, "internal_error",
      std::string(kDiagnosticTextMaxBytes + 1, 'd')));
  EXPECT_LE(encoded["message"].get_ref<const std::string&>().size(),
            kDiagnosticTextMaxBytes);
  OperationStatus unchanged =
      failure_status(OperationErrorDomain::Daemon, 95, "sentinel", "unchanged");
  std::string message;
  EXPECT_FALSE(decode_error(
      Json{{"domain", "daemon"},
           {"code", kInternalErrorCode},
           {"name", "internal_error"},
           {"message", std::string(kDiagnosticTextMaxBytes + 1, 'd')}},
      &unchanged, &message));
  EXPECT_EQ(unchanged.code, 95);
  EXPECT_EQ(unchanged.name, "sentinel");
}

TEST(OperationStatusModel, RejectsInvalidEncodedFailureStatuses) {
  EXPECT_THROW((void)encode_error(ok_status()), std::invalid_argument);
  EXPECT_NO_THROW((void)encode_operation_status(ok_status()));
  const std::vector<OperationStatus> invalid_argument_statuses = {
      failure_status(OperationErrorDomain::None, 1, "failure", "diagnostic"),
      failure_status(OperationErrorDomain::Transport, 1, "connect_failed",
                     "diagnostic"),
      failure_status(static_cast<OperationErrorDomain>(999), 1, "failure",
                     "diagnostic"),
      failure_status(OperationErrorDomain::Daemon, -32199, "", "diagnostic"),
      failure_status(OperationErrorDomain::Daemon, -32199,
                     std::string("\xc0\xaf", 2), "diagnostic"),
      failure_status(OperationErrorDomain::Daemon, -32199, "job_not_found",
                     "diagnostic")};
  for (const OperationStatus& status : invalid_argument_statuses) {
    EXPECT_THROW((void)encode_error(status), std::invalid_argument);
    EXPECT_THROW((void)encode_operation_status(status), std::invalid_argument);
  }
  const OperationStatus oversized =
      failure_status(OperationErrorDomain::Daemon, -32199,
                     std::string(kShortTextMaxBytes + 1, 'n'), "diagnostic");
  EXPECT_THROW((void)encode_error(oversized), std::length_error);
  EXPECT_THROW((void)encode_operation_status(oversized), std::length_error);
}

/**
 * @brief Verifies real router mapping for rejected Host inspection values.
 *
 * @throws Nothing when graph loading and both routed inspections retain their
 *         version 1 error contracts; GoogleTest records any mismatch.
 * @note The loaded YAML deliberately preserves one 1,025-byte node name and
 *       one negative node id. `inspect.node` therefore reaches the outbound
 *       text bound, while `inspect.graph` reaches malformed-value validation,
 *       without exposing a production test hook or substituting a fake Host.
 */
TEST(ProtocolErrors, MapsRealInspectionCodecFailuresToStableErrors) {
  ScopedTempDirectory temp("ps-ipc-codec-errors");
  const std::filesystem::path yaml_path = temp.path() / "graph.yaml";
  {
    std::ofstream output(yaml_path);
    output.exceptions(std::ios::badbit | std::ios::failbit);
    output << "- id: 1\n"
              "  name: "
           << std::string(kShortTextMaxBytes + 1, 'n')
           << "\n"
              "  type: ipc_fixture\n"
              "  subtype: source\n"
              "- id: -1\n"
              "  name: invalid_id\n"
              "  type: ipc_fixture\n"
              "  subtype: source\n";
  }

  std::unique_ptr<Host> host = create_embedded_host();
  ASSERT_NE(host, nullptr);
  RequestRouter router(*host, "0.1.0");
  const Json loaded = parse_response(router.route(Json{
      {"protocol_version", kProtocolVersion},
      {"id", "load-codec-errors"},
      {"method", "graph.load"},
      {"params",
       Json{{"session_name", "codec_error_routes"},
            {"root_dir", (temp.path() / "sessions").string()},
            {"yaml_path", yaml_path.string()}}}}.dump()));
  ASSERT_TRUE(loaded.contains("result")) << loaded.dump();
  ASSERT_TRUE(loaded["result"].value("session_id", Json()).is_string());
  const std::string session_id =
      loaded["result"]["session_id"].get<std::string>();
  ASSERT_TRUE(valid_opaque_id(session_id));

  const Json oversized_node = parse_response(router.route(Json{
      {"protocol_version", kProtocolVersion},
      {"id", "inspect-oversized-node"},
      {"method", "inspect.node"},
      {"params",
       Json{{"session_id", session_id}, {"node_id", 1}}}}.dump()));
  ASSERT_TRUE(oversized_node.contains("error")) << oversized_node.dump();
  EXPECT_EQ(oversized_node["error"]["domain"], "protocol");
  EXPECT_EQ(oversized_node["error"]["code"], kResponseTooLargeCode);
  EXPECT_EQ(oversized_node["error"]["name"], "response_too_large");

  const Json invalid_graph = parse_response(router.route(Json{
      {"protocol_version", kProtocolVersion},
      {"id", "inspect-invalid-graph"},
      {"method", "inspect.graph"},
      {"params", Json{{"session_id", session_id}}}}.dump()));
  ASSERT_TRUE(invalid_graph.contains("error")) << invalid_graph.dump();
  EXPECT_EQ(invalid_graph["error"]["domain"], "daemon");
  EXPECT_EQ(invalid_graph["error"]["code"], kInternalErrorCode);
  EXPECT_EQ(invalid_graph["error"]["name"], "internal_error");

  router.begin_shutdown();
  router.finish_shutdown();
}

TEST(InspectionJson, RoundTripsNullAndNonFiniteSnapshots) {
  NodeInspectionView node;
  node.id = NodeId{7};
  node.name = "node";
  node.type = "fixture";
  node.subtype = "source";
  node.parameters = {{"answer", "42"}};
  node.debug = DebugMetadataSnapshot{};
  node.debug->min_val = std::numeric_limits<double>::quiet_NaN();
  node.debug->max_val = std::numeric_limits<double>::infinity();
  node.space = SpatialSnapshot{};
  node.space->global_scale_x = std::numeric_limits<double>::quiet_NaN();
  const Json encoded = encode_node(node);
  EXPECT_TRUE(encoded["source_label"].is_null());
  EXPECT_TRUE(encoded["debug"]["min_val"].is_null());
  EXPECT_TRUE(encoded["debug"]["max_val"].is_null());
  EXPECT_TRUE(encoded["space"]["global_scale_x"].is_null());

  NodeInspectionView decoded;
  std::string message;
  ASSERT_TRUE(decode_node(encoded, &decoded, &message)) << message;
  EXPECT_TRUE(std::isnan(decoded.debug->min_val));
  EXPECT_TRUE(std::isnan(decoded.debug->max_val));
  EXPECT_TRUE(std::isnan(decoded.space->global_scale_x));

  HostDependencyTreeSnapshot tree;
  tree.scope = HostDependencyTreeScope::StartNode;
  tree.start_node = NodeId{7};
  tree.root_nodes = {NodeId{7}};
  tree.entries.push_back(HostDependencyTreeEntry{0, std::nullopt, node, false});
  const Json tree_json = encode_dependency_tree(
      IpcSessionId{"0123456789abcdef0123456789abcdef"}, tree);
  HostDependencyTreeSnapshot decoded_tree;
  ASSERT_TRUE(decode_dependency_tree(tree_json, &decoded_tree, &message))
      << message;
  ASSERT_EQ(decoded_tree.entries.size(), 1U);
  EXPECT_EQ(decoded_tree.scope, HostDependencyTreeScope::StartNode);
  EXPECT_EQ(decoded_tree.start_node->value, 7);
}

TEST(InspectionJson, EnforcesValueBoundsAndTransactionalNestedShapes) {
  NodeInspectionView node;
  node.id = NodeId{7};
  node.name = std::string(kShortTextMaxBytes, 'n');
  node.type = "fixture";
  node.subtype = "source";
  node.parameters = {{"answer", "42"}};
  node.space = SpatialSnapshot{};
  Json encoded = encode_node(node);
  encoded["future_field"] = true;
  NodeInspectionView decoded;
  std::string message;
  ASSERT_TRUE(decode_node(encoded, &decoded, &message)) << message;
  EXPECT_EQ(decoded.name.size(), kShortTextMaxBytes);

  NodeInspectionView oversized = node;
  oversized.name.push_back('n');
  EXPECT_THROW((void)encode_node(oversized), std::length_error);
  Json oversized_json = encoded;
  oversized_json["name"] = oversized.name;
  decoded.id = NodeId{31};
  EXPECT_FALSE(decode_node(oversized_json, &decoded, &message));
  EXPECT_EQ(decoded.id.value, 31);

  for (std::size_t matrix_size : {std::size_t{8}, std::size_t{10}}) {
    Json malformed_matrix = encoded;
    malformed_matrix["space"]["transform_matrix"] = Json::array();
    for (std::size_t index = 0; index < matrix_size; ++index) {
      malformed_matrix["space"]["transform_matrix"].push_back(0.0);
    }
    decoded.id = NodeId{33};
    EXPECT_FALSE(decode_node(malformed_matrix, &decoded, &message));
    EXPECT_EQ(decoded.id.value, 33);
  }

  Json too_many_parameters = encoded;
  too_many_parameters["parameters"] = Json::object();
  for (std::size_t index = 0; index <= kGeneralPageMaxEntries; ++index) {
    too_many_parameters["parameters"][std::to_string(index)] = "value";
  }
  decoded.id = NodeId{35};
  EXPECT_FALSE(decode_node(too_many_parameters, &decoded, &message));
  EXPECT_EQ(decoded.id.value, 35);

  const IpcSessionId session_id{"0123456789abcdef0123456789abcdef"};
  Json oversized_graph{{"session_id", session_id.value},
                       {"nodes", Json::array()}};
  for (std::size_t index = 0; index <= kGeneralPageMaxEntries; ++index) {
    oversized_graph["nodes"].push_back(nullptr);
  }
  GraphInspectionView graph;
  graph.session.value = "sentinel";
  EXPECT_FALSE(decode_graph(oversized_graph, &graph, &message));
  EXPECT_EQ(graph.session.value, "sentinel");
  GraphInspectionView too_many_nodes;
  too_many_nodes.nodes.resize(kGeneralPageMaxEntries + 1, node);
  EXPECT_THROW((void)encode_graph(session_id, too_many_nodes),
               std::length_error);
  EXPECT_THROW((void)encode_graph(IpcSessionId{"malformed"}, {}),
               std::invalid_argument);

  HostDependencyTreeSnapshot tree;
  tree.root_nodes.resize(kGeneralPageMaxEntries + 1, NodeId{7});
  EXPECT_THROW((void)encode_dependency_tree(session_id, tree),
               std::length_error);
  Json oversized_tree{
      {"session_id", session_id.value}, {"scope", "ending_nodes"},
      {"start_node_id", nullptr},       {"graph_empty", false},
      {"start_node_found", true},       {"no_ending_nodes", false},
      {"root_node_ids", Json::array()}, {"entries", Json::array()}};
  for (std::size_t index = 0; index <= kGeneralPageMaxEntries; ++index) {
    oversized_tree["root_node_ids"].push_back(7);
  }
  HostDependencyTreeSnapshot unchanged_tree;
  unchanged_tree.start_node = NodeId{37};
  EXPECT_FALSE(
      decode_dependency_tree(oversized_tree, &unchanged_tree, &message));
  ASSERT_TRUE(unchanged_tree.start_node.has_value());
  EXPECT_EQ(unchanged_tree.start_node->value, 37);
}

TEST(InspectionJson, RejectsEveryOutOfRangeTypedInteger) {
  NodeInspectionView node;
  node.id = NodeId{7};
  node.name = "node";
  node.type = "fixture";
  node.subtype = "source";
  node.parameters = {{"answer", "42"}};
  node.debug = DebugMetadataSnapshot{};
  node.space = SpatialSnapshot{};
  const Json valid_node = encode_node(node);
  std::string message;

  const std::vector<Json> malformed_nodes = [&] {
    std::vector<Json> values;
    Json id_overflow = valid_node;
    id_overflow["id"] = std::uint64_t{4294967296ULL};
    values.push_back(std::move(id_overflow));
    Json worker_overflow = valid_node;
    worker_overflow["debug"]["computed_by_worker_id"] =
        std::numeric_limits<std::uint64_t>::max();
    values.push_back(std::move(worker_overflow));
    Json timestamp_underflow = valid_node;
    timestamp_underflow["debug"]["timestamp_us"] =
        std::numeric_limits<std::int64_t>::min();
    values.push_back(std::move(timestamp_underflow));
    Json extent_overflow = valid_node;
    extent_overflow["space"]["extent"]["width"] = std::uint64_t{4294967296ULL};
    values.push_back(std::move(extent_overflow));
    Json rectangle_underflow = valid_node;
    rectangle_underflow["space"]["absolute_roi"]["x"] =
        std::numeric_limits<std::int64_t>::min();
    values.push_back(std::move(rectangle_underflow));
    return values;
  }();
  for (const Json& malformed : malformed_nodes) {
    NodeInspectionView decoded;
    decoded.id = NodeId{31};
    EXPECT_FALSE(decode_node(malformed, &decoded, &message));
    EXPECT_EQ(decoded.id.value, 31);
  }

  HostDependencyTreeSnapshot tree;
  tree.scope = HostDependencyTreeScope::StartNode;
  tree.start_node = NodeId{7};
  tree.root_nodes = {NodeId{7}};
  HostGraphEdgeSnapshot edge;
  edge.from_node = NodeId{3};
  edge.to_node = NodeId{7};
  edge.input_index = 0;
  tree.entries.push_back(HostDependencyTreeEntry{0, edge, node, false});
  const Json valid_tree = encode_dependency_tree(
      IpcSessionId{"0123456789abcdef0123456789abcdef"}, tree);
  const std::vector<Json> malformed_trees = [&] {
    std::vector<Json> values;
    Json start_overflow = valid_tree;
    start_overflow["start_node_id"] = std::uint64_t{4294967296ULL};
    values.push_back(std::move(start_overflow));
    Json root_underflow = valid_tree;
    root_underflow["root_node_ids"][0] =
        std::numeric_limits<std::int64_t>::min();
    values.push_back(std::move(root_underflow));
    Json depth_overflow = valid_tree;
    depth_overflow["entries"][0]["depth"] =
        std::numeric_limits<std::uint64_t>::max();
    values.push_back(std::move(depth_overflow));
    Json from_node_overflow = valid_tree;
    from_node_overflow["entries"][0]["incoming_edge"]["from_node_id"] =
        std::numeric_limits<std::uint64_t>::max();
    values.push_back(std::move(from_node_overflow));
    Json input_index_underflow = valid_tree;
    input_index_underflow["entries"][0]["incoming_edge"]["input_index"] = -1;
    values.push_back(std::move(input_index_underflow));
    return values;
  }();
  for (const Json& malformed : malformed_trees) {
    HostDependencyTreeSnapshot decoded;
    decoded.start_node = NodeId{31};
    EXPECT_FALSE(decode_dependency_tree(malformed, &decoded, &message));
    ASSERT_TRUE(decoded.start_node.has_value());
    EXPECT_EQ(decoded.start_node->value, 31);
  }
}

TEST(SessionRegistry, HandlesCollisionsRollbackReconciliationAndSorting) {
  std::vector<std::string> candidates = {
      std::string(32, 'a'), std::string(32, 'a'), std::string(32, 'b'),
      std::string(32, 'c')};
  std::size_t next = 0;
  SessionRegistry registry([&] { return candidates.at(next++); });
  const auto beta = registry.reserve("beta");
  ASSERT_TRUE(beta.status.ok);
  ASSERT_TRUE(registry.commit(beta.value, GraphSessionId{"private-beta"}).ok);
  const auto resolved_beta = registry.admit_host_call(beta.value);
  ASSERT_TRUE(resolved_beta.status.ok);
  EXPECT_EQ(resolved_beta.value.host_session().value, "private-beta");
  EXPECT_FALSE(registry.reserve("beta").status.ok);
  const auto alpha = registry.reserve("alpha");
  ASSERT_TRUE(alpha.status.ok);
  EXPECT_EQ(alpha.value.value, std::string(32, 'b'));
  ASSERT_TRUE(registry.commit(alpha.value, GraphSessionId{"private-alpha"}).ok);
  const auto resolved_alpha = registry.admit_host_call(alpha.value);
  ASSERT_TRUE(resolved_alpha.status.ok);
  EXPECT_EQ(resolved_alpha.value.host_session().value, "private-alpha");

  const auto pending = registry.reserve("pending");
  ASSERT_TRUE(pending.status.ok);
  registry.rollback(pending.value);
  EXPECT_FALSE(registry.admit_host_call(pending.value).status.ok);

  const auto listed = registry.reconcile(
      {GraphSessionId{"private-beta"}, GraphSessionId{"private-alpha"}});
  ASSERT_TRUE(listed.status.ok);
  ASSERT_EQ(listed.value.size(), 2U);
  EXPECT_EQ(listed.value[0].session_name, "alpha");
  EXPECT_EQ(listed.value[1].session_name, "beta");

  const auto disagreement =
      registry.reconcile({GraphSessionId{"private-alpha"}});
  EXPECT_FALSE(disagreement.status.ok);
  EXPECT_EQ(disagreement.status.domain, OperationErrorDomain::Daemon);
}

TEST(SessionRegistry, ProductionOpaqueIdHasStableWireShape) {
  const std::string token = generate_opaque_id();
  ASSERT_EQ(token.size(), 32U);
  EXPECT_TRUE(std::all_of(token.begin(), token.end(), [](char character) {
    return (character >= '0' && character <= '9') ||
           (character >= 'a' && character <= 'f');
  }));
}

TEST(ProtocolEnvelope, OversizedSuccessBecomesBoundedStructuredError) {
  Json result{{"large", std::string(kMaximumFramePayloadBytes, 'z')}};
  const std::string payload =
      encode_success_response("large", std::move(result));
  ASSERT_LE(payload.size(), kMaximumFramePayloadBytes);
  const Json response = parse_response(payload);
  EXPECT_EQ(response["id"], "large");
  EXPECT_EQ(response["error"]["domain"], "protocol");
  EXPECT_EQ(response["error"]["code"], kResponseTooLargeCode);
  EXPECT_EQ(response["error"]["name"], "response_too_large");
}

TEST(ClientLifecycle, RejectsUncorrelatedResponseAndClosesIdempotently) {
  for (const auto& response :
       std::vector<std::pair<std::string, std::int32_t>>{{"wrong-id", 1},
                                                         {"client-1", 2}}) {
    ScopedTempDirectory temp("photospider-ipc-client-unit");
    const std::string socket_path = (temp.path() / "server.sock").string();
    UniqueFd listener = create_test_listener(socket_path);
    bool served = false;
    std::thread peer([&] {
      served = serve_uncorrelated_response(listener.get(), response.first,
                                           response.second);
    });
    Client client;
    const OperationStatus connected = client.connect(socket_path);
    const IpcResult<DaemonPing> ping = client.ping();
    peer.join();
    ASSERT_TRUE(connected.ok);
    EXPECT_TRUE(served);
    EXPECT_FALSE(ping.status.ok);
    EXPECT_EQ(ping.status.domain, OperationErrorDomain::Protocol);
    EXPECT_FALSE(client.connected());
    client.disconnect();
    client.disconnect();
    EXPECT_FALSE(client.connected());
  }
}

TEST(ClientLifecycle, RejectsOverflowedEnvelopeVersionAndErrorCode) {
  const std::vector<std::string> responses = {
      R"({"protocol_version":4294967297,"id":"client-1","result":{"pong":true,"server_instance_id":"0123456789abcdef0123456789abcdef"}})",
      R"({"protocol_version":1,"id":"client-1","error":{"domain":"protocol","code":18446744073709551615,"name":"future","message":"diagnostic"}})"};
  for (const std::string& response : responses) {
    ScopedTempDirectory temp("ps-ipc-overflow-envelope");
    const std::string socket_path = (temp.path() / "server.sock").string();
    UniqueFd listener = create_test_listener(socket_path);
    Client client;
    ASSERT_TRUE(client.connect(socket_path).ok);
    bool served = false;
    std::thread peer(
        [&] { served = serve_raw_response(listener.get(), response); });
    const IpcResult<DaemonPing> ping = client.ping();
    peer.join();
    EXPECT_TRUE(served);
    EXPECT_FALSE(ping.status.ok);
    EXPECT_EQ(ping.status.domain, OperationErrorDomain::Protocol);
    EXPECT_EQ(ping.status.code, kInvalidRequestCode);
    EXPECT_FALSE(client.connected());
  }
}

TEST(ClientResultValidation, RejectsOverflowedVersionWithoutDesynchronizing) {
  ScopedTempDirectory temp("ps-ipc-overflow-version-result");
  const std::string socket_path = (temp.path() / "server.sock").string();
  UniqueFd listener = create_test_listener(socket_path);
  Client client;
  ASSERT_TRUE(client.connect(socket_path).ok);
  const Json malformed_result{
      {"protocol_version", std::uint64_t{4294967297ULL}},
      {"service_name", "photospiderd"},
      {"service_version", "0.1.0"},
      {"server_instance_id", "0123456789abcdef0123456789abcdef"},
      {"transport", "unix"},
      {"methods", Json::array({"daemon.ping"})}};
  bool served = false;
  std::thread peer([&] {
    served = serve_correlated_result(listener.get(), malformed_result);
  });
  const IpcResult<DaemonVersion> version = client.version();
  peer.join();
  EXPECT_TRUE(served);
  EXPECT_FALSE(version.status.ok);
  EXPECT_EQ(version.status.domain, OperationErrorDomain::Protocol);
  EXPECT_EQ(version.status.code, kInvalidRequestCode);
  EXPECT_TRUE(client.connected());
}

TEST(ClientResultValidation, RejectsEveryInexactMethodInventory) {
  Json exact_methods = Json::array();
  for (std::string_view method : kVersionOneMethodNames) {
    exact_methods.push_back(std::string(method));
  }
  std::vector<Json> malformed_methods;
  Json missing = exact_methods;
  missing.erase(missing.size() - 1);
  malformed_methods.push_back(std::move(missing));
  Json replaced = exact_methods;
  replaced[replaced.size() - 1] = "inspect.replacement";
  malformed_methods.push_back(std::move(replaced));
  Json duplicated = exact_methods;
  duplicated[duplicated.size() - 1] = duplicated[duplicated.size() - 2];
  malformed_methods.push_back(std::move(duplicated));

  for (const Json& methods : malformed_methods) {
    ScopedTempDirectory temp("ps-ipc-method-inventory");
    const std::string socket_path = (temp.path() / "server.sock").string();
    UniqueFd listener = create_test_listener(socket_path);
    Client client;
    ASSERT_TRUE(client.connect(socket_path).ok);
    const Json result{
        {"protocol_version", kProtocolVersion},
        {"service_name", "photospiderd"},
        {"service_version", "0.1.0"},
        {"server_instance_id", "0123456789abcdef0123456789abcdef"},
        {"transport", "unix"},
        {"methods", methods}};
    bool served = false;
    std::thread peer(
        [&] { served = serve_correlated_result(listener.get(), result); });
    const IpcResult<DaemonVersion> version = client.version();
    peer.join();
    EXPECT_TRUE(served);
    EXPECT_FALSE(version.status.ok);
    EXPECT_EQ(version.status.domain, OperationErrorDomain::Protocol);
    EXPECT_EQ(version.status.code, kInvalidRequestCode);
    EXPECT_TRUE(client.connected());
  }
}

TEST(ClientResultValidation, RejectsInspectionOverflowWithoutDesynchronizing) {
  ScopedTempDirectory temp("ps-ipc-overflow-inspection-result");
  const std::string socket_path = (temp.path() / "server.sock").string();
  UniqueFd listener = create_test_listener(socket_path);
  Client client;
  ASSERT_TRUE(client.connect(socket_path).ok);
  const IpcSessionId session_id{"0123456789abcdef0123456789abcdef"};
  NodeInspectionView node;
  node.id = NodeId{7};
  node.name = "node";
  node.type = "fixture";
  node.subtype = "source";
  Json malformed_node = encode_node(node);
  malformed_node["id"] = std::uint64_t{4294967296ULL};
  const Json malformed_result{
      {"session_id", session_id.value},
      {"nodes", Json::array({std::move(malformed_node)})}};
  bool served = false;
  std::thread peer([&] {
    served = serve_correlated_result(listener.get(), malformed_result);
  });
  const IpcResult<GraphInspectionView> graph = client.inspect_graph(session_id);
  peer.join();
  EXPECT_TRUE(served);
  EXPECT_FALSE(graph.status.ok);
  EXPECT_EQ(graph.status.domain, OperationErrorDomain::Protocol);
  EXPECT_EQ(graph.status.code, kInvalidRequestCode);
  EXPECT_TRUE(graph.value.nodes.empty());
  EXPECT_TRUE(client.connected());
}

TEST(ClientResultValidation, RejectsMalformedLoadIdentityFromLocalPeer) {
  const std::vector<Json> malformed_results = {
      Json{{"session_id", "ABCDEF0123456789ABCDEF0123456789"},
           {"session_name", "expected"}},
      Json{{"session_id", "0123456789abcdef0123456789abcdef"},
           {"session_name", "different"}}};
  for (const Json& malformed_result : malformed_results) {
    ScopedTempDirectory temp("photospider-ipc-client-load-result");
    const std::string socket_path = (temp.path() / "server.sock").string();
    UniqueFd listener = create_test_listener(socket_path);
    Client client;
    ASSERT_TRUE(client.connect(socket_path).ok);
    bool served = false;
    std::thread peer([&] {
      served = serve_correlated_result(listener.get(), malformed_result);
    });
    GraphLoadRequest request;
    request.session = GraphSessionId{"expected"};
    request.root_dir = temp.path().string();
    const IpcResult<GraphSessionSummary> loaded = client.load_graph(request);
    peer.join();
    EXPECT_TRUE(served);
    EXPECT_FALSE(loaded.status.ok);
    EXPECT_EQ(loaded.status.domain, OperationErrorDomain::Protocol);
    EXPECT_EQ(loaded.status.code, kInvalidRequestCode);
    EXPECT_TRUE(client.connected());
  }
}

TEST(ClientResultValidation, RejectsMalformedListIdentityFromLocalPeer) {
  ScopedTempDirectory temp("ps-ipc-list-result");
  const std::string socket_path = (temp.path() / "server.sock").string();
  UniqueFd listener = create_test_listener(socket_path);
  Client client;
  ASSERT_TRUE(client.connect(socket_path).ok);
  const Json malformed_result{
      {"sessions", Json::array({Json{{"session_id", std::string(31, 'a')},
                                     {"session_name", "listed"}}})}};
  bool served = false;
  std::thread peer([&] {
    served = serve_correlated_result(listener.get(), malformed_result);
  });
  const IpcResult<std::vector<GraphSessionSummary>> listed =
      client.list_graphs();
  peer.join();
  EXPECT_TRUE(served);
  EXPECT_FALSE(listed.status.ok);
  EXPECT_EQ(listed.status.domain, OperationErrorDomain::Protocol);
  EXPECT_EQ(listed.status.code, kInvalidRequestCode);
  EXPECT_TRUE(client.connected());
}

TEST(ClientResultValidation, RejectsMalformedDaemonIdentityFromLocalPeer) {
  ScopedTempDirectory temp("ps-ipc-ping-result");
  const std::string socket_path = (temp.path() / "server.sock").string();
  UniqueFd listener = create_test_listener(socket_path);
  Client client;
  ASSERT_TRUE(client.connect(socket_path).ok);
  const Json malformed_result{{"pong", true},
                              {"server_instance_id", "not-an-opaque-id"}};
  bool served = false;
  std::thread peer([&] {
    served = serve_correlated_result(listener.get(), malformed_result);
  });
  const IpcResult<DaemonPing> ping = client.ping();
  peer.join();
  EXPECT_TRUE(served);
  EXPECT_FALSE(ping.status.ok);
  EXPECT_EQ(ping.status.domain, OperationErrorDomain::Protocol);
  EXPECT_EQ(ping.status.code, kInvalidRequestCode);
  EXPECT_TRUE(client.connected());
}

TEST(ClientResultValidation, ClassifiesDuplicateResponseAsInvalidRequest) {
  ScopedTempDirectory temp("ps-ipc-duplicate-response");
  const std::string socket_path = (temp.path() / "server.sock").string();
  UniqueFd listener = create_test_listener(socket_path);
  Client client;
  ASSERT_TRUE(client.connect(socket_path).ok);
  const std::string response =
      R"({"protocol_version":1,"id":"client-1","result":{"pong":true,"pong":true,"server_instance_id":"0123456789abcdef0123456789abcdef"}})";
  bool served = false;
  std::thread peer(
      [&] { served = serve_raw_response(listener.get(), response); });
  const IpcResult<DaemonPing> ping = client.ping();
  peer.join();
  EXPECT_TRUE(served);
  EXPECT_FALSE(ping.status.ok);
  EXPECT_EQ(ping.status.domain, OperationErrorDomain::Protocol);
  EXPECT_EQ(ping.status.code, kInvalidRequestCode);
  EXPECT_EQ(ping.status.name, "invalid_request");
}

}  // namespace
}  // namespace ps::ipc::internal
