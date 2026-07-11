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
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
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

namespace ps::ipc::internal {
namespace {

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
   * @param label Stable test-specific directory prefix.
   * @throws std::filesystem::filesystem_error if setup fails.
   */
  explicit ScopedTempDirectory(const std::string& label)
      : path_(std::filesystem::temp_directory_path() /
              (label + "-" + std::to_string(::getpid()) + "-" +
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
   *
   * @throws Nothing because this operation is unavailable.
   * @note Each test creates its own unique directory.
   */
  ScopedTempDirectory(const ScopedTempDirectory&) = delete;

  /**
   * @brief Prevents replacing temporary-tree cleanup ownership by copy.
   *
   * @return No value because this operation is unavailable.
   * @throws Nothing because this operation is unavailable.
   * @note The owned path remains fixed for the helper lifetime.
   */
  ScopedTempDirectory& operator=(const ScopedTempDirectory&) = delete;

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

TEST(ProtocolParams, RejectsInvalidValuesWithoutHostMutation) {
  ScopedTempDirectory temp("photospider-ipc-invalid-params");
  std::unique_ptr<Host> host = create_embedded_host();
  ASSERT_NE(host, nullptr);
  RequestRouter router(*host, "0.1.0");
  const std::string valid_session_id(32, 'a');
  const std::filesystem::path untouched_root = temp.path() / "untouched";
  const std::vector<Json> requests = {
      Json{{"protocol_version", 1},
           {"id", "ping-params"},
           {"method", "daemon.ping"},
           {"params", Json{{"unexpected", true}}}},
      Json{{"protocol_version", 1},
           {"id", "version-params"},
           {"method", "daemon.version"},
           {"params", Json{{"unexpected", true}}}},
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
  router.close_all_sessions();
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
  const std::optional<GraphSessionId> resolved_beta =
      registry.resolve(beta.value);
  ASSERT_TRUE(resolved_beta.has_value());
  EXPECT_EQ(resolved_beta->value, "private-beta");
  EXPECT_FALSE(registry.reserve("beta").status.ok);
  const auto alpha = registry.reserve("alpha");
  ASSERT_TRUE(alpha.status.ok);
  EXPECT_EQ(alpha.value.value, std::string(32, 'b'));
  ASSERT_TRUE(registry.commit(alpha.value, GraphSessionId{"private-alpha"}).ok);
  const std::optional<GraphSessionId> resolved_alpha =
      registry.resolve(alpha.value);
  ASSERT_TRUE(resolved_alpha.has_value());
  EXPECT_EQ(resolved_alpha->value, "private-alpha");

  const auto pending = registry.reserve("pending");
  ASSERT_TRUE(pending.status.ok);
  registry.rollback(pending.value);
  EXPECT_FALSE(registry.resolve(pending.value).has_value());

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
