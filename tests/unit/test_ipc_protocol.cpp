#include <arpa/inet.h>
#include <fcntl.h>
#include <gtest/gtest.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "ipc/client_collection_budget.hpp"
#include "ipc/client_interrupt_access.hpp"
#include "ipc/codec.hpp"
#include "ipc/frame.hpp"
#include "ipc/protocol_bounds.hpp"
#include "ipc/request_router.hpp"
#include "ipc/session_registry.hpp"
#include "ipc/unix_socket.hpp"
#include "photospider/host/host.hpp"
#include "photospider/ipc/client.hpp"
#include "support/ipc_host_spy.hpp"

#ifndef PS_TEST_OP_PLUGIN_DIR
#define PS_TEST_OP_PLUGIN_DIR "build/test_plugins/lifecycle"
#endif

namespace ps::ipc::internal {
namespace {

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
 * @brief Starts and stops one router runtime below a protected test directory.
 *
 * @throws std::runtime_error if the lifecycle file or runtime cannot start.
 * @note The helper owns only the lock descriptor. The borrowed router and
 *       directory must outlive it, and destruction drains all private runtime
 *       registries before their owners are destroyed.
 */
class ScopedRequestRouterRuntime final {
 public:
  /**
   * @brief Creates a private lifecycle file and starts router admission.
   * @param router Router whose output/snapshot/compute/session runtime starts.
   * @param directory Protected directory owning socket-related paths.
   * @throws std::runtime_error on descriptor, mode, or runtime failure.
   */
  ScopedRequestRouterRuntime(RequestRouter& router,
                             const ScopedTempDirectory& directory)
      : router_(router),
        lock_fd_(::open((directory.path() / "router.sock.lock").c_str(),
                        O_CREAT | O_RDWR | O_CLOEXEC | O_NOFOLLOW, 0600)) {
    if (!lock_fd_ || ::fchmod(lock_fd_.get(), 0600) != 0) {
      throw std::runtime_error("cannot create router lifecycle test lock");
    }
    const OperationStatus started = router_.start_runtime(
        (directory.path() / "router.sock").string(), lock_fd_.get());
    if (!started.ok) {
      throw std::runtime_error(started.message);
    }
  }

  /** @brief Drains and stops the borrowed router. @throws Nothing. */
  ~ScopedRequestRouterRuntime() noexcept {
    router_.begin_shutdown();
    router_.finish_shutdown();
  }

  /**
   * @brief Prevents duplicate shutdown ownership.
   * @param other Runtime guard that remains the owner.
   * @throws Nothing because construction is unavailable.
   */
  ScopedRequestRouterRuntime(const ScopedRequestRouterRuntime& other) = delete;

  /**
   * @brief Prevents replacement of active runtime ownership.
   * @param other Guard that remains unchanged.
   * @return No value because assignment is unavailable.
   * @throws Nothing because assignment is unavailable.
   */
  ScopedRequestRouterRuntime& operator=(
      const ScopedRequestRouterRuntime& other) = delete;

 private:
  /** @brief Borrowed router stopped during destruction. */
  RequestRouter& router_;
  /** @brief Owned exact-mode lifecycle descriptor. */
  UniqueFd lock_fd_;
};

/**
 * @brief Returns the active-build directory for the lifecycle operation DSO.
 * @return CMake-provided repository fixture output directory.
 * @throws std::bad_alloc if path storage cannot allocate.
 * @note The compile definition uses `TARGET_FILE_DIR`, so multi-config and
 *       non-default build trees never depend on a hard-coded repository path.
 */
std::filesystem::path lifecycle_operation_plugin_dir() {
  return std::filesystem::path(PS_TEST_OP_PLUGIN_DIR);
}

/**
 * @brief Clears process-global operation plugins around a lifecycle test.
 *
 * @throws std::runtime_error when initial cleanup cannot create a Host or
 *         report a successful unload.
 * @note Destruction creates a fresh Host and retries explicit global unload
 *       behind a no-throw fence. Host destruction itself is never treated as
 *       plugin cleanup, which is the ownership behavior under test.
 */
class ScopedOperationPluginCleanup final {
 public:
  /**
   * @brief Performs required initial explicit process-global cleanup.
   * @throws std::bad_alloc if Host or diagnostic ownership cannot allocate.
   * @throws std::runtime_error if Host creation or explicit unload fails.
   */
  ScopedOperationPluginCleanup() {
    std::unique_ptr<Host> host = create_embedded_host();
    if (!host) {
      throw std::runtime_error("cannot create operation plugin cleanup Host");
    }
    const Result<int> unloaded = host->plugins_unload_all();
    if (!unloaded.status.ok) {
      throw std::runtime_error(unloaded.status.message);
    }
  }

  /**
   * @brief Explicitly unloads any fixture left after an assertion.
   * @throws Nothing; Host creation and unload failures are contained.
   */
  ~ScopedOperationPluginCleanup() noexcept {
    try {
      std::unique_ptr<Host> host = create_embedded_host();
      if (host) {
        (void)host->plugins_unload_all();
      }
    } catch (...) {
    }
  }

  /**
   * @brief Prevents duplicate global cleanup ownership.
   * @throws Nothing because this operation is unavailable.
   */
  ScopedOperationPluginCleanup(const ScopedOperationPluginCleanup&) = delete;

  /**
   * @brief Prevents replacing global cleanup ownership.
   * @return No value because assignment is unavailable.
   * @throws Nothing because this operation is unavailable.
   */
  ScopedOperationPluginCleanup& operator=(const ScopedOperationPluginCleanup&) =
      delete;
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
 * @brief One expected Client request and correlated scripted reply.
 *
 * @throws std::bad_alloc when copied method or JSON storage cannot allocate.
 * @note `error=false` publishes `payload` under `result`; `error=true`
 *       publishes it under `error`. The helper owns no production Client hook.
 */
struct ScriptedClientReply {
  /** @brief Exact wire method expected from the next Client call. */
  std::string method;

  /** @brief Caller-selected typed result or structured error object. */
  Json payload;

  /** @brief Whether payload belongs to the error response branch. */
  bool error = false;
};

/**
 * @brief Builds one canonical traversal-order stable-page result.
 * @param session_id Exact opaque session echo.
 * @param first_key First globally sorted ending-node key on the page.
 * @param nested_counts Node-id count for each indivisible branch row.
 * @param offset Exact zero-based outer-row offset.
 * @param has_more Whether another scripted page follows.
 * @param cursor Stable continuation cursor used while `has_more` is true.
 * @return Complete known-field page result for the direct Client.
 * @throws std::bad_alloc if nested arrays or result storage cannot allocate.
 * @note Repeated zero node ids are intentional public values. Only branch keys
 *       require strict cross-page ordering in this test surface.
 */
Json traversal_order_page_result(const IpcSessionId& session_id, int first_key,
                                 const std::vector<std::size_t>& nested_counts,
                                 std::size_t offset, bool has_more,
                                 const std::string& cursor) {
  Json orders = Json::array();
  int key = first_key;
  for (const std::size_t nested_count : nested_counts) {
    orders.push_back(Json{{"ending_node_id", key++},
                          {"node_ids", std::vector<int>(nested_count, 0)}});
  }
  return Json{{"session_id", session_id.value},
              {"orders", std::move(orders)},
              {"offset", offset},
              {"has_more", has_more},
              {"cursor", has_more ? Json(cursor) : Json(nullptr)}};
}

/**
 * @brief Script and exact canonical/raw byte totals for source paging tests.
 *
 * @throws Nothing for default construction; vector mutation may allocate.
 * @note Canonical bytes count only decoded public key/source rows. Raw bytes
 *       additionally include caller-selected forward-compatible members.
 */
struct StableSourceReplyScript {
  /** @brief One correlated reply per indivisible source row. */
  std::vector<ScriptedClientReply> replies;

  /** @brief Complete canonical array bytes across every scripted page. */
  std::size_t canonical_bytes = 2;

  /** @brief Complete raw array bytes including unknown row members. */
  std::size_t raw_bytes = 2;
};

/**
 * @brief Builds eight source pages with one exact canonical aggregate size.
 * @param target_bytes Required complete canonical row-array byte total.
 * @param add_unknown_fields Whether each raw row carries an ignored member.
 * @return Script whose canonical total equals `target_bytes` exactly.
 * @throws std::invalid_argument if the target cannot be represented by eight
 *         individually bounded source rows.
 * @throws std::bad_alloc if strings, JSON, or reply storage cannot allocate.
 * @note Seven rows use the maximum 8 MiB source and the final row absorbs the
 *       exact remainder. Every individual response remains below 16 MiB.
 */
StableSourceReplyScript stable_source_reply_script(std::size_t target_bytes,
                                                   bool add_unknown_fields) {
  static constexpr std::size_t kPageCount = 8;
  const std::string cursor(32, 'e');
  if (target_bytes < 2U + (kPageCount - 1U)) {
    throw std::invalid_argument("stable source target is too small");
  }

  StableSourceReplyScript script;
  script.replies.reserve(kPageCount);
  std::size_t remaining_row_bytes = target_bytes - 2U - (kPageCount - 1U);
  for (std::size_t index = 0; index < kPageCount; ++index) {
    const std::string key = "source-" + std::to_string(1000U + index);
    const std::size_t empty_row_bytes =
        encode_plugin_source_row(key, "").dump().size();
    std::size_t source_bytes = kLargeTextMaxBytes;
    if (index + 1U == kPageCount) {
      if (remaining_row_bytes < empty_row_bytes) {
        throw std::invalid_argument("stable source target underflows last row");
      }
      source_bytes = remaining_row_bytes - empty_row_bytes;
    }
    if (source_bytes > kLargeTextMaxBytes) {
      throw std::invalid_argument("stable source target exceeds row bound");
    }

    Json row = encode_plugin_source_row(key, std::string(source_bytes, 's'));
    const std::size_t canonical_row_bytes = row.dump().size();
    if (canonical_row_bytes > remaining_row_bytes) {
      throw std::invalid_argument("stable source target arithmetic failed");
    }
    remaining_row_bytes -= canonical_row_bytes;
    script.canonical_bytes += canonical_row_bytes;
    if (index != 0) {
      ++script.canonical_bytes;
      ++script.raw_bytes;
    }
    if (add_unknown_fields) {
      row["future_member"] = "ignored by this protocol version";
    }
    script.raw_bytes += row.dump().size();

    Json rows = Json::array();
    rows.push_back(std::move(row));
    const bool has_more = index + 1U != kPageCount;
    Json payload{{"sources", std::move(rows)},
                 {"offset", index},
                 {"has_more", has_more},
                 {"cursor", has_more ? Json(cursor) : Json(nullptr)}};
    script.replies.push_back(
        ScriptedClientReply{"plugins.ops_sources", std::move(payload)});
  }
  if (remaining_row_bytes != 0 || script.canonical_bytes != target_bytes) {
    throw std::invalid_argument("stable source target was not exact");
  }
  return script;
}

/**
 * @brief Serves a sequence of correlated replies over one Client connection.
 * @param listener Bound local listener descriptor.
 * @param replies Exact expected method/reply script.
 * @param requests Receives every parsed request after its method validates.
 * @return True only after every scripted request and reply completes.
 * @throws Nothing; parsing, framing, method, or allocation failures become
 *         false and close the accepted peer descriptor.
 * @note The real local socket exercises request serialization, one-attempt
 *       behavior, correlation, and multi-page sequencing without exposing raw
 *       JSON through production Client APIs.
 */
bool serve_scripted_client_replies(
    int listener, const std::vector<ScriptedClientReply>& replies,
    std::vector<Json>* requests) noexcept {
  UniqueFd peer(::accept(listener, nullptr, nullptr));
  if (!peer || requests == nullptr) {
    return false;
  }
  try {
    for (const ScriptedClientReply& reply : replies) {
      const FrameReadResult frame = read_frame(peer.get());
      if (frame.state != FrameReadState::Complete) {
        return false;
      }
      JsonParseResult parsed = parse_json(frame.payload);
      if (!parsed.ok || !parsed.value.is_object() ||
          !parsed.value.value("id", Json()).is_string() ||
          !parsed.value.value("method", Json()).is_string() ||
          parsed.value["method"].get<std::string>() != reply.method) {
        return false;
      }
      requests->push_back(parsed.value);
      Json response{{"protocol_version", kProtocolVersion},
                    {"id", parsed.value["id"]}};
      response[reply.error ? "error" : "result"] = reply.payload;
      if (!write_frame(peer.get(), response.dump()).ok) {
        return false;
      }
    }
    return true;
  } catch (...) {
    return false;
  }
}

/**
 * @brief Reads exactly one complete Client request and closes without replying.
 * @param listener Bound local listener descriptor.
 * @param request Receives the parsed request object.
 * @return True when one complete valid object was received.
 * @throws Nothing; accept, frame, parse, or allocation failures become false.
 * @note Closing after the request models ambiguous transport loss after a
 *       mutation may already have reached the peer. A conforming Client must
 *       return that read failure without sending another request.
 */
bool receive_one_client_request_then_close(int listener,
                                           Json* request) noexcept {
  UniqueFd peer(::accept(listener, nullptr, nullptr));
  if (!peer || request == nullptr) {
    return false;
  }
  try {
    const FrameReadResult frame = read_frame(peer.get());
    if (frame.state != FrameReadState::Complete) {
      return false;
    }
    JsonParseResult parsed = parse_json(frame.payload);
    if (!parsed.ok || !parsed.value.is_object()) {
      return false;
    }
    *request = std::move(parsed.value);
    return true;
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
 * @brief Routes one complete typed request through a borrowed router.
 * @param router Active router whose runtime has already started.
 * @param method Exact version 1 method name.
 * @param params Complete method params object.
 * @param id Correlated request id.
 * @return Parsed correlated response.
 * @throws std::bad_alloc or std::runtime_error on construction/parse failure.
 * @note The helper is used by tests that intentionally replace the complete
 *       Host/router lifetime between calls and therefore cannot use one
 *       fixture-owned router member.
 */
Json route_test_request(RequestRouter& router, const std::string& method,
                        Json params, std::string id) {
  return parse_response(router.route(Json{
      {"protocol_version", kProtocolVersion},
      {"id", std::move(id)},
      {"method", method},
      {"params", std::move(params)}}.dump()));
}

/**
 * @brief Returns the exact JSON representation of one public pixel rectangle.
 *
 * @param rect Rectangle to copy.
 * @return Object with signed x, y, width, and height fields.
 * @throws std::bad_alloc if JSON storage cannot allocate.
 * @note This expectation helper is intentionally independent of the production
 *       `encode_pixel_rect()` implementation.
 */
Json expected_rect(const PixelRect& rect) {
  return Json{{"x", rect.x},
              {"y", rect.y},
              {"width", rect.width},
              {"height", rect.height}};
}

/**
 * @brief Builds one nontrivial dirty-region value for router schema tests.
 *
 * @return Snapshot containing every nested dirty collection and public enum.
 * @throws std::bad_alloc if copied strings or containers cannot allocate.
 */
DirtyRegionInspectionSnapshot make_protocol_dirty_snapshot() {
  DirtyRegionInspectionSnapshot snapshot;
  snapshot.graph_generation = 91;
  snapshot.sources.push_back(
      DirtySourceSnapshot{NodeId{7},
                          DirtyDomain::RealTime,
                          DirtySourceLifecycleState::Updating,
                          13,
                          {PixelRect{1, 2, 3, 4}}});
  snapshot.dirty_tiles.push_back(
      DirtyTileSnapshot{NodeId{8}, DirtyDomain::HighPrecision, -2, 5, 64,
                        PixelRect{-10, 20, 30, 40}});
  snapshot.dirty_monolithic_nodes.push_back(DirtyMonolithicRegionSnapshot{
      NodeId{9}, DirtyDomain::RealTime, PixelRect{6, 7, 8, 9}, false});
  snapshot.actual_dirty_rois.emplace(
      10, std::vector<PixelRect>{PixelRect{11, 12, 13, 14}});
  snapshot.edge_mappings.push_back(DirtyEdgeMappingSnapshot{
      NodeId{7}, NodeId{8}, DirtyDomain::RealTime, PixelRect{1, 1, 2, 2},
      PixelRect{3, 3, 4, 4}, DirtyEdgeDirection::ForwardAffected});
  return snapshot;
}

/**
 * @brief Builds one nontrivial compute-planning inspection value.
 * @return Snapshot containing public enums, counts, ROI, and nested samples.
 * @throws std::bad_alloc if copied strings or vectors cannot allocate.
 */
ComputePlanningInspectionSnapshot make_protocol_planning_snapshot() {
  ComputePlanningInspectionSnapshot snapshot;
  snapshot.intent = ComputeIntent::RealTimeUpdate;
  snapshot.target_node = NodeId{12};
  snapshot.parallel = true;
  snapshot.topology_generation = 44;
  snapshot.expansion_cache_key = "rt:12:44";
  snapshot.planned_node_count = 2;
  snapshot.task_count = 2;
  snapshot.tile_task_count = 1;
  snapshot.monolithic_task_count = 0;
  snapshot.node_task_count = 1;
  snapshot.dependency_count = 1;
  snapshot.initial_task_count = 1;
  snapshot.active_task_count = 2;
  snapshot.dirty_source_task_count = 1;
  snapshot.downstream_task_count = 1;
  snapshot.initial_downstream_task_count = 1;
  snapshot.planned_node_sample = {NodeId{11}, NodeId{12}};
  ComputePlanningTaskSnapshot task;
  task.task_id = 1;
  task.node = NodeId{12};
  task.kind = "tile";
  task.domain = DirtyDomain::RealTime;
  task.output_roi = PixelRect{1, 2, 3, 4};
  task.tile_x = 5;
  task.tile_y = 6;
  task.tile_size = 64;
  task.whole_output = false;
  task.dirty_selected = true;
  task.dirty_generation = 45;
  task.dependency_task_ids = {0};
  snapshot.task_sample.push_back(std::move(task));
  return snapshot;
}

/**
 * @brief Builds a dense but component-valid current-planning fixture.
 * @param task_count Number of sampled planning tasks.
 * @param dependencies_per_task Number of zero-valued dependency ids per task.
 * @param kind_bytes UTF-8 bytes in every task kind.
 * @param cache_key_bytes UTF-8 bytes in the expansion cache key.
 * @return Planning value with valid ids, enums, rectangles, and requested
 *         collection dimensions.
 * @throws std::length_error when `task_count` cannot be represented by test
 *         task ids.
 * @throws std::bad_alloc if fixture storage cannot be allocated.
 */
ComputePlanningInspectionSnapshot make_dense_protocol_planning_snapshot(
    std::size_t task_count, std::size_t dependencies_per_task,
    std::size_t kind_bytes = 4, std::size_t cache_key_bytes = 0) {
  if (task_count > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
    throw std::length_error("planning fixture task count exceeds int ids");
  }
  ComputePlanningInspectionSnapshot snapshot;
  snapshot.intent = ComputeIntent::RealTimeUpdate;
  snapshot.target_node = NodeId{1};
  snapshot.expansion_cache_key.assign(cache_key_bytes, 'k');
  snapshot.task_sample.reserve(task_count);
  for (std::size_t index = 0; index < task_count; ++index) {
    ComputePlanningTaskSnapshot task;
    task.task_id = static_cast<int>(index);
    task.node = NodeId{1};
    task.kind.assign(kind_bytes, 't');
    task.domain = DirtyDomain::RealTime;
    task.dependency_task_ids.assign(dependencies_per_task, 0);
    snapshot.task_sample.push_back(std::move(task));
  }
  return snapshot;
}

/**
 * @brief Builds deterministic traversal branches for recursive count bounds.
 * @param branch_count Number of ending-node map entries.
 * @param nodes_per_branch Nested node-id entries in every branch.
 * @param first_branch_extra Additional nested entries in the first branch.
 * @return Ordered traversal map with nonnegative keys and ids.
 * @throws std::bad_alloc if map or vector storage cannot allocate.
 * @throws std::length_error if requested test dimensions overflow `size_t`.
 */
std::map<int, std::vector<NodeId>> make_dense_traversal_orders(
    std::size_t branch_count, std::size_t nodes_per_branch,
    std::size_t first_branch_extra = 0) {
  if (nodes_per_branch >
      std::numeric_limits<std::size_t>::max() - first_branch_extra) {
    throw std::length_error("traversal fixture dimensions overflow");
  }
  std::map<int, std::vector<NodeId>> orders;
  for (std::size_t branch = 0; branch < branch_count; ++branch) {
    const std::size_t nodes =
        nodes_per_branch + (branch == 0 ? first_branch_extra : 0U);
    orders.emplace(static_cast<int>(branch),
                   std::vector<NodeId>(nodes, NodeId{1}));
  }
  return orders;
}

/**
 * @brief Builds the independent expected JSON for the dirty snapshot fixture.
 *
 * @param session_id Opaque daemon session returned by graph load.
 * @return Exact dirty-region result object.
 * @throws std::bad_alloc if JSON storage cannot allocate.
 */
Json expected_protocol_dirty_snapshot(const std::string& session_id) {
  return Json{
      {"session_id", session_id},
      {"graph_generation", 91},
      {"sources", Json::array({Json{
                      {"node_id", 7},
                      {"domain", "real_time"},
                      {"lifecycle", "updating"},
                      {"generation", 13},
                      {"source_rois",
                       Json::array({expected_rect(PixelRect{1, 2, 3, 4})})}}})},
      {"dirty_tiles",
       Json::array(
           {Json{{"node_id", 8},
                 {"domain", "high_precision"},
                 {"tile_x", -2},
                 {"tile_y", 5},
                 {"tile_size", 64},
                 {"pixel_roi", expected_rect(PixelRect{-10, 20, 30, 40})}}})},
      {"dirty_monolithic_nodes",
       Json::array({Json{{"node_id", 9},
                         {"domain", "real_time"},
                         {"pixel_roi", expected_rect(PixelRect{6, 7, 8, 9})},
                         {"whole_output", false}}})},
      {"actual_dirty_rois",
       Json::array({Json{{"node_id", 10},
                         {"rois", Json::array({expected_rect(
                                      PixelRect{11, 12, 13, 14})})}}})},
      {"edge_mappings",
       Json::array({Json{{"from_node_id", 7},
                         {"to_node_id", 8},
                         {"domain", "real_time"},
                         {"from_roi", expected_rect(PixelRect{1, 1, 2, 2})},
                         {"to_roi", expected_rect(PixelRect{3, 3, 4, 4})},
                         {"direction", "forward_affected"}}})}};
}

/**
 * @brief Returns valid params for one Host-routed session-control method.
 *
 * @param method Exact one of the 21 Host-routed session-control method names.
 * @param session_id Valid opaque daemon session id.
 * @return Complete params object with deterministic nontrivial values.
 * @throws std::bad_alloc if JSON or string construction cannot allocate.
 * @throws std::invalid_argument if `method` is outside the supported family.
 */
Json valid_host_routed_params(std::string_view method,
                              const std::string& session_id) {
  Json params{{"session_id", session_id}};
  if (method == "graph.reload" || method == "graph.save") {
    params["yaml_path"] = "/tmp/host-routed-graph.yaml";
  } else if (method == "graph.node_yaml.get") {
    params["node_id"] = 17;
  } else if (method == "graph.node_yaml.set") {
    params["node_id"] = 18;
    params["yaml_text"] = "id: 18\nname: host_routed\n";
  } else if (method == "cache.cache_all_nodes" ||
             method == "cache.synchronize_disk") {
    params["precision"] = "float32";
  } else if (method == "dirty.begin" || method == "dirty.update") {
    params["node_id"] = 19;
    params["domain"] = "real_time";
    params["source_roi"] = expected_rect(PixelRect{-1, 2, 3, 4});
  } else if (method == "dirty.end") {
    params["node_id"] = 20;
    params["domain"] = "high_precision";
  } else if (method == "inspect.roi_forward") {
    params["start_node_id"] = 21;
    params["target_node_id"] = 22;
    params["start_roi"] = expected_rect(PixelRect{5, 6, 7, 8});
  } else if (method == "inspect.roi_backward") {
    params["target_node_id"] = 23;
    params["source_node_id"] = 24;
    params["target_roi"] = expected_rect(PixelRect{9, 10, 11, 12});
  } else {
    static constexpr std::array<std::string_view, 9> kSessionOnlyMethods = {
        "cache.clear_all",      "cache.clear_drive",  "cache.clear_memory",
        "cache.free_transient", "compute.last_error", "compute.last_io_time",
        "compute.timing",       "graph.clear",        "inspect.ending_nodes"};
    const bool session_only =
        method == "inspect.node_ids" ||
        std::find(kSessionOnlyMethods.begin(), kSessionOnlyMethods.end(),
                  method) != kSessionOnlyMethods.end();
    if (!session_only) {
      throw std::invalid_argument("unknown Host-routed session-control method");
    }
  }
  return params;
}

/**
 * @brief Builds one complete nontrivial compute-submit params object.
 * @param session_id Valid opaque daemon session identity.
 * @param result_mode Exact `status` or `image` selection.
 * @return Nested public Host request values plus one ignored future field.
 * @throws std::bad_alloc if JSON or string storage cannot allocate.
 * @note Values exercise every task-3.3 known field while demonstrating that
 *       unknown members remain forward-compatible.
 */
Json valid_compute_submit_params(const std::string& session_id,
                                 std::string result_mode = "status") {
  return Json{{"session_id", session_id},
              {"node_id", 37},
              {"cache", Json{{"precision", "float16"},
                             {"force_recache", true},
                             {"disable_disk_cache", true},
                             {"nosave", true},
                             {"future_cache_field", "ignored"}}},
              {"execution", Json{{"parallel", true}, {"quiet", true}}},
              {"telemetry", Json{{"enable_timing", true}}},
              {"intent", "real_time_update"},
              {"dirty_roi", expected_rect(PixelRect{-4, 5, 6, 7})},
              {"result_mode", std::move(result_mode)},
              {"future_submit_field", true}};
}

/**
 * @brief Creates one exact Graph-domain failure used by no-retry tests.
 *
 * @return Graph IO status with deliberately noncanonical input name so router
 *         canonicalization is observable.
 * @throws std::bad_alloc if diagnostic storage cannot allocate.
 */
OperationStatus host_routed_graph_failure() {
  return OperationStatus{false, OperationErrorDomain::Graph,
                         static_cast<std::int32_t>(GraphErrc::Io), "ignored",
                         "host-routed Host failure"};
}

/**
 * @brief Shared heap-owned state for one blocking protocol Host hook.
 *
 * @throws Nothing for default construction.
 * @note The hook captures this state by value so an assertion return cannot
 *       leave it borrowing test-stack storage while the router worker joins.
 */
struct ProtocolGateState {
  /** @brief Mutex serializing entry observation and gate release. */
  std::mutex mutex;

  /** @brief Condition variable waking entry observers and the blocked hook. */
  std::condition_variable changed;

  /** @brief Whether the blocking Host hook has reached the gate. */
  bool entered = false;

  /** @brief Whether the blocking Host hook may leave the gate. */
  bool released = false;
};

/**
 * @brief Failure-safe owner that opens one condition-variable test gate.
 *
 * @throws Nothing.
 * @note Establish this guard before any assertion or call that can leave the
 *       Host hook blocked. When a local future owns the waiting operation,
 *       declare the guard after that future so reverse destruction opens the
 *       gate before the future joins.
 */
class ScopedProtocolGateRelease final {
 public:
  /**
   * @brief Borrows one mutex, condition variable, and release flag.
   * @param mutex Mutex protecting `released`.
   * @param changed Condition variable observed by the blocked Host hook.
   * @param released Flag that opens the test gate when true.
   * @throws Nothing.
   * @note The borrowed mutex, condition variable, and flag must outlive this
   *       guard.
   */
  ScopedProtocolGateRelease(std::mutex& mutex, std::condition_variable& changed,
                            bool& released) noexcept
      : mutex_(mutex), changed_(changed), released_(released) {}

  /**
   * @brief Opens the gate before dependent futures are destroyed.
   * @throws Nothing.
   * @note The guard must not be destroyed while its thread already owns the
   *       borrowed mutex, because release deliberately acquires that mutex.
   */
  ~ScopedProtocolGateRelease() noexcept { release(); }

  /**
   * @brief Prevents duplicate release ownership.
   * @param other Guard that retains release responsibility.
   * @throws Nothing because construction is unavailable.
   */
  ScopedProtocolGateRelease(const ScopedProtocolGateRelease& other) = delete;

  /**
   * @brief Prevents replacing one active release owner.
   * @param other Guard that retains release responsibility.
   * @return No value because assignment is unavailable.
   * @throws Nothing because assignment is unavailable.
   */
  ScopedProtocolGateRelease& operator=(const ScopedProtocolGateRelease& other) =
      delete;

  /**
   * @brief Idempotently opens the borrowed gate and wakes every waiter.
   * @return Nothing.
   * @throws Nothing.
   * @note The caller must not already own the borrowed mutex.
   */
  void release() noexcept {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      released_ = true;
    }
    changed_.notify_all();
  }

 private:
  /** @brief Mutex protecting the borrowed release flag. */
  std::mutex& mutex_;

  /** @brief Condition variable used to wake the blocked Host hook. */
  std::condition_variable& changed_;

  /** @brief Borrowed flag controlling the Host hook gate. */
  bool& released_;
};

/**
 * @brief Fixture owning one loaded spy-backed router session.
 *
 * @throws std::bad_alloc or std::runtime_error if router setup cannot allocate
 *         metadata or generate its opaque ids.
 * @note `SetUp()` routes the real `graph.load` protocol path, captures its
 *       daemon id, and then clears load bookkeeping from the spy.
 */
class HostRoutedGraphStateProtocolTest : public ::testing::Test {
 protected:
  /**
   * @brief Loads one deterministic private Host session through the router.
   * @return Nothing.
   * @throws std::bad_alloc if request, response, registry, or spy storage
   *         cannot allocate.
   * @throws std::runtime_error if response parsing unexpectedly fails.
   * @note The real `graph.load` path creates the opaque mapping. Load
   *       bookkeeping is cleared before each test body so call counts describe
   *       only the Host-routed graph-state method under test.
   */
  void SetUp() override {
    const Json loaded = route(
        "graph.load",
        Json{{"session_name", "host_routed_session"}, {"root_dir", "/tmp"}});
    ASSERT_TRUE(loaded.contains("result")) << loaded.dump();
    ASSERT_TRUE(loaded["result"].value("session_id", Json()).is_string());
    session_id_ = loaded["result"]["session_id"].get<std::string>();
    ASSERT_TRUE(valid_opaque_id(session_id_));
    host_.reset_invocations();
  }

  /**
   * @brief Routes one typed request through the real request envelope.
   * @param method Exact wire method.
   * @param params Complete params object.
   * @param id Optional correlated id used for frame-budget edge cases.
   * @return Parsed correlated response object.
   * @throws std::bad_alloc or std::runtime_error on unexpected construction or
   *         parse failure.
   */
  Json route(const std::string& method, Json params,
             std::string id = std::string()) {
    if (id.empty()) {
      id = method;
    }
    return parse_response(router_.route(Json{
        {"protocol_version", kProtocolVersion},
        {"id", std::move(id)},
        {"method", method},
        {"params", std::move(params)}}.dump()));
  }

  /**
   * @brief Polls one accepted job until a terminal snapshot is observed.
   * @param compute_id Valid opaque job identity returned by submit.
   * @return Last correlated status response, terminal on success.
   * @throws std::bad_alloc or std::runtime_error on request/response failure.
   * @note The two-second deadline is only a deadlock watchdog. Polling yields
   *       instead of sleeping for correctness or relying on fixed timing.
   */
  Json wait_for_compute_terminal(const std::string& compute_id) {
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(2);
    Json response;
    while (std::chrono::steady_clock::now() < deadline) {
      response = route("compute.status", Json{{"compute_id", compute_id}});
      if (!response.contains("result")) {
        return response;
      }
      const std::string state = response["result"]["state"].get<std::string>();
      if (state == "succeeded" || state == "failed") {
        return response;
      }
      std::this_thread::yield();
    }
    ADD_FAILURE() << "compute job did not become terminal before deadline";
    return response;
  }

  /** @brief Protected runtime path owner declared before router runtime. */
  ScopedTempDirectory runtime_directory_{"ps-ipc-route"};

  /** @brief Complete configurable Host test double. */
  ::ps::testing::IpcHostSpy host_;

  /** @brief Router under test, borrowing `host_`. */
  RequestRouter router_{host_, "host-routed-test"};

  /** @brief Active snapshot/output/compute runtime for routed calls. */
  ScopedRequestRouterRuntime runtime_{router_, runtime_directory_};

  /** @brief Opaque daemon id mapped to the spy's private Host session. */
  std::string session_id_;
};

TEST_F(HostRoutedGraphStateProtocolTest,
       PluginControlRoutesExactHostValuesWithoutSessionIdentity) {
  HostPluginLoadReport report;
  report.attempted = 2;
  report.loaded = 1;
  report.errors.push_back(
      HostPluginLoadError{"", GraphErrc::Io, "fixture load failure"});
  report.new_op_keys = {"zeta:op", "alpha:op"};
  host_.set_plugin_load_report(report);

  const Json loaded = route(
      "plugins.load_report",
      Json{{"directories", Json::array({"relative/plugins", "/tmp/ops/*"})},
           {"session_id", "ignored-forward-field"}});
  ASSERT_TRUE(loaded.contains("result")) << loaded.dump();
  const Json& value = loaded["result"];
  EXPECT_EQ(value["attempted"], 2);
  EXPECT_EQ(value["loaded"], 1);
  ASSERT_EQ(value["errors"].size(), 1U);
  EXPECT_EQ(value["errors"][0]["path"], "");
  EXPECT_EQ(value["errors"][0]["code"], static_cast<int>(GraphErrc::Io));
  EXPECT_EQ(value["errors"][0]["name"], "io");
  EXPECT_EQ(value["errors"][0]["message"], "fixture load failure");
  EXPECT_EQ(value["new_op_keys"], Json::array({"zeta:op", "alpha:op"}));
  ASSERT_EQ(host_.invocations().size(), 1U);
  EXPECT_EQ(host_.invocations()[0].texts,
            (std::vector<std::string>{"relative/plugins", "/tmp/ops/*"}));
  EXPECT_TRUE(host_.invocations()[0].session.value.empty());

  host_.reset_invocations();
  EXPECT_EQ(route("plugins.seed_builtins", Json{{"future", true}})["result"],
            Json::object());
  EXPECT_EQ(route("plugins.seed_builtins", Json::object())["result"],
            Json::object());
  EXPECT_EQ(host_.call_count("plugins.seed_builtins"), 2U);

  host_.reset_invocations();
  host_.set_plugins_unload_count(3);
  const Json unloaded = route("plugins.unload_all", Json{{"future", 1}});
  ASSERT_TRUE(unloaded.contains("result")) << unloaded.dump();
  EXPECT_EQ(unloaded["result"], (Json{{"unloaded", 3}}));
  EXPECT_EQ(host_.call_count("plugins.unload_all"), 1U);
}

TEST_F(HostRoutedGraphStateProtocolTest,
       PolicyAndExecutionGlobalRoutesPreserveExactHostValues) {
  host_.set_policy_description("deterministic policy");
  Json response = route("policy.description",
                        Json{{"type", "fixture_policy"}, {"future", true}});
  ASSERT_TRUE(response.contains("result")) << response.dump();
  EXPECT_EQ(response["result"],
            (Json{{"type", "fixture_policy"},
                  {"description", "deterministic policy"}}));
  ASSERT_EQ(host_.invocations().size(), 1U);
  EXPECT_EQ(host_.invocations()[0].method, "policy.description");
  EXPECT_EQ(host_.invocations()[0].text, "fixture_policy");
  EXPECT_TRUE(host_.invocations()[0].session.value.empty());

  host_.reset_invocations();
  host_.set_policy_scan_count(7);
  response =
      route("policy.scan",
            Json{{"directories", Json::array({"relative/policies", "/tmp/*"})},
                 {"future", "ignored"}});
  ASSERT_TRUE(response.contains("result")) << response.dump();
  EXPECT_EQ(response["result"], (Json{{"loaded", 7}}));
  ASSERT_EQ(host_.invocations().size(), 1U);
  EXPECT_EQ(host_.invocations()[0].texts,
            (std::vector<std::string>{"relative/policies", "/tmp/*"}));

  host_.reset_invocations();
  response = route("policy.load",
                   Json{{"path", "relative/plugin.dylib"}, {"future", 1}});
  ASSERT_TRUE(response.contains("result")) << response.dump();
  EXPECT_EQ(response["result"], Json::object());
  ASSERT_EQ(host_.invocations().size(), 1U);
  EXPECT_EQ(host_.invocations()[0].text, "relative/plugin.dylib");

  host_.reset_invocations();
  response = route("policy.configure_defaults",
                   Json{{"interactive_type", "interactive"},
                        {"throughput_type", "throughput"},
                        {"future", Json::object()}});
  ASSERT_TRUE(response.contains("result")) << response.dump();
  EXPECT_EQ(response["result"], Json::object());
  ASSERT_EQ(host_.invocations().size(), 1U);
  EXPECT_EQ(host_.invocations()[0].text, "interactive\nthroughput");

  host_.reset_invocations();
  host_.set_execution_description("private CPU route");
  response = route("execution.description", Json{{"type", "cpu"}});
  ASSERT_TRUE(response.contains("result")) << response.dump();
  EXPECT_EQ(response["result"],
            (Json{{"type", "cpu"}, {"description", "private CPU route"}}));
  EXPECT_EQ(host_.call_count("execution.description"), 1U);

  host_.reset_invocations();
  response =
      route("execution.configure_defaults", Json{{"hp_type", "cpu"},
                                                 {"rt_type", "serial_debug"},
                                                 {"worker_count", 3U}});
  ASSERT_TRUE(response.contains("result")) << response.dump();
  ASSERT_EQ(host_.invocations().size(), 1U);
  EXPECT_EQ(host_.invocations()[0].text, "cpu\nserial_debug");
  EXPECT_EQ(host_.invocations()[0].worker_count, 3U);
}

/**
 * @brief Proves execution worker limits are enforced before Host mutation.
 * @return Nothing.
 * @throws std::bad_alloc If request, response, or spy storage cannot allocate.
 * @throws GoogleTest assertion failures when exact routing or rejection
 * violates the protocol contract.
 * @note Zero and eight must each reach Host exactly once. The accepted eight
 * invocation remains the only recorded mutation after nine is rejected, so
 * the test observes both pre-Host validation and unchanged prior state.
 */
TEST_F(HostRoutedGraphStateProtocolTest,
       ExecutionDefaultsBoundWorkerCountBeforeHostAccess) {
  for (const unsigned int worker_count : {0U, kExecutionWorkerRequestMax}) {
    host_.reset_invocations();
    const Json accepted = route("execution.configure_defaults",
                                Json{{"hp_type", "cpu"},
                                     {"rt_type", "serial_debug"},
                                     {"worker_count", worker_count}});
    ASSERT_TRUE(accepted.contains("result")) << accepted.dump();
    EXPECT_EQ(accepted["result"], Json::object());
    ASSERT_EQ(host_.call_count("execution.configure_defaults"), 1U);
    ASSERT_EQ(host_.invocations().size(), 1U);
    EXPECT_EQ(host_.invocations().front().worker_count, worker_count);
  }

  const Json rejected =
      route("execution.configure_defaults",
            Json{{"hp_type", "cpu"},
                 {"rt_type", "serial_debug"},
                 {"worker_count", kExecutionWorkerRequestMax + 1U}});
  ASSERT_TRUE(rejected.contains("error")) << rejected.dump();
  EXPECT_EQ(rejected["error"]["domain"], "protocol");
  EXPECT_EQ(rejected["error"]["code"], kInvalidParamsCode);
  EXPECT_EQ(rejected["error"]["name"], "invalid_params");
  ASSERT_EQ(host_.call_count("execution.configure_defaults"), 1U);
  ASSERT_EQ(host_.invocations().size(), 1U);
  EXPECT_EQ(host_.invocations().front().worker_count,
            kExecutionWorkerRequestMax);
}

TEST_F(HostRoutedGraphStateProtocolTest,
       PolicyAndExecutionInfoRoutesPreserveScopeAndCopiedValues) {
  PolicyInfoSnapshot policy_info;
  policy_info.policy_class = PolicyClass::Throughput;
  policy_info.policy_type = "fixture_policy";
  policy_info.binding_generation = 9;
  policy_info.fault = PolicyFaultSnapshot{PolicyFaultReason::CallbackStatus,
                                          std::uint32_t{4}, "fixture fault"};
  host_.set_policy_info(policy_info);

  Json response =
      route("policy.info", Json{{"policy_class", "throughput"},
                                {"session_id", "ignored-global-field"},
                                {"future", true}});
  ASSERT_TRUE(response.contains("result")) << response.dump();
  EXPECT_EQ(response["result"],
            (Json{{"policy_class", "throughput"},
                  {"policy_type", "fixture_policy"},
                  {"binding_generation", 9},
                  {"fault", Json{{"reason", "callback_status"},
                                 {"callback_status", 4},
                                 {"message", "fixture fault"}}}}));
  ASSERT_EQ(host_.invocations().size(), 1U);
  EXPECT_EQ(host_.invocations()[0].method, "policy.info");
  EXPECT_EQ(host_.invocations()[0].policy_class, PolicyClass::Throughput);
  EXPECT_TRUE(host_.invocations()[0].session.value.empty());

  host_.reset_invocations();
  response = route("policy.replace", Json{{"policy_class", "interactive"},
                                          {"type", "fixture_policy"},
                                          {"future", Json::array()}});
  ASSERT_TRUE(response.contains("result")) << response.dump();
  EXPECT_EQ(response["result"], Json::object());
  ASSERT_EQ(host_.invocations().size(), 1U);
  EXPECT_EQ(host_.invocations()[0].method, "policy.replace");
  EXPECT_EQ(host_.invocations()[0].policy_class, PolicyClass::Interactive);
  EXPECT_EQ(host_.invocations()[0].text, "fixture_policy");

  host_.reset_invocations();
  ExecutionInfoSnapshot execution_info;
  execution_info.intent = ComputeIntent::RealTimeUpdate;
  execution_info.execution_type = "gpu_pipeline";
  execution_info.stats = "workers=3;running=true";
  host_.set_execution_info(execution_info);
  response = route("execution.info", Json{{"session_id", session_id_},
                                          {"intent", "real_time_update"},
                                          {"future", true}});
  ASSERT_TRUE(response.contains("result")) << response.dump();
  EXPECT_EQ(response["result"], (Json{{"session_id", session_id_},
                                      {"intent", "real_time_update"},
                                      {"execution_type", "gpu_pipeline"},
                                      {"stats", "workers=3;running=true"}}));
  ASSERT_EQ(host_.invocations().size(), 1U);
  EXPECT_EQ(host_.invocations()[0].method, "execution.info");
  EXPECT_EQ(host_.invocations()[0].session.value, "ipc-host-spy-session");
  EXPECT_EQ(host_.invocations()[0].intent, ComputeIntent::RealTimeUpdate);

  host_.reset_invocations();
  response =
      route("execution.replace", Json{{"session_id", session_id_},
                                      {"intent", "global_high_precision"},
                                      {"type", "serial_debug"}});
  ASSERT_TRUE(response.contains("result")) << response.dump();
  ASSERT_EQ(host_.invocations().size(), 1U);
  EXPECT_EQ(host_.invocations()[0].method, "execution.replace");
  EXPECT_EQ(host_.invocations()[0].session.value, "ipc-host-spy-session");
  EXPECT_EQ(host_.invocations()[0].intent, ComputeIntent::GlobalHighPrecision);
  EXPECT_EQ(host_.invocations()[0].text, "serial_debug");
}

TEST_F(HostRoutedGraphStateProtocolTest,
       PolicyAndExecutionRoutesRejectMalformedValuesBeforeHostAccess) {
  std::vector<std::pair<std::string, Json>> malformed = {
      {"policy.description", Json::object()},
      {"policy.description", Json{{"type", "Uppercase"}}},
      {"policy.description",
       Json{{"type", std::string(kPolicyTypeMaxBytes + 1, 't')}}},
      {"policy.scan", Json{{"directories", "not-an-array"}}},
      {"policy.scan", Json{{"directories", Json::array({""})}}},
      {"policy.load", Json{{"path", std::string("bad\0path", 8)}}},
      {"policy.configure_defaults",
       Json{{"interactive_type", "interactive"}, {"throughput_type", "Bad"}}},
      {"policy.info", Json{{"policy_class", "future_class"}}},
      {"policy.replace", Json{{"policy_class", "interactive"}, {"type", ""}}},
      {"execution.description", Json{{"type", "future_route"}}},
      {"execution.configure_defaults", Json{{"hp_type", "cpu"},
                                            {"rt_type", "serial_debug"},
                                            {"worker_count", -1}}},
      {"execution.configure_defaults", Json{{"hp_type", "future_route"},
                                            {"rt_type", "serial_debug"},
                                            {"worker_count", 1}}},
      {"execution.info",
       Json{{"session_id", session_id_}, {"intent", "future_intent"}}},
      {"execution.replace", Json{{"session_id", session_id_},
                                 {"intent", "real_time_update"},
                                 {"type", "future_route"}}},
  };
  Json too_many_directories = Json::array();
  for (std::size_t index = 0; index < kPathArrayMaxEntries + 1; ++index) {
    too_many_directories.push_back("policy-dir");
  }
  malformed.push_back(
      {"policy.scan", Json{{"directories", too_many_directories}}});

  for (std::size_t index = 0; index < malformed.size(); ++index) {
    const Json response = route(malformed[index].first, malformed[index].second,
                                "malformed-policy-" + std::to_string(index));
    ASSERT_TRUE(response.contains("error")) << response.dump();
    EXPECT_EQ(response["error"]["domain"], "protocol");
    EXPECT_EQ(response["error"]["name"], "invalid_params");
  }
  EXPECT_TRUE(host_.invocations().empty());

  std::string unknown_session(32, 'a');
  if (unknown_session == session_id_) {
    unknown_session.front() = 'b';
  }
  const Json invalid_before_session =
      route("execution.replace", Json{{"session_id", unknown_session},
                                      {"intent", "future_intent"},
                                      {"type", "serial_debug"}});
  EXPECT_EQ(invalid_before_session["error"]["domain"], "protocol");
  EXPECT_TRUE(host_.invocations().empty());

  const Json unknown = route(
      "execution.info",
      Json{{"session_id", unknown_session}, {"intent", "real_time_update"}});
  EXPECT_EQ(unknown["error"]["domain"], "graph");
  EXPECT_EQ(unknown["error"]["name"], "not_found");
  EXPECT_TRUE(host_.invocations().empty());
}

TEST_F(HostRoutedGraphStateProtocolTest,
       PolicyAndExecutionRoutesRejectMalformedCopiedHostValues) {
  host_.set_status("policy.load", host_routed_graph_failure());
  Json response = route("policy.load", Json{{"path", "failed-plugin.dylib"}});
  ASSERT_TRUE(response.contains("error")) << response.dump();
  EXPECT_EQ(response["error"]["domain"], "graph");
  EXPECT_EQ(response["error"]["name"], "io");
  EXPECT_EQ(host_.call_count("policy.load"), 1U);

  host_.reset_invocations();
  host_.set_status("policy.load", OperationStatus{});
  host_.set_policy_description(std::string(kPathTextMaxBytes + 1, 'd'));
  response = route("policy.description", Json{{"type", "fixture_policy"}});
  ASSERT_TRUE(response.contains("error")) << response.dump();
  EXPECT_EQ(response["error"]["domain"], "protocol");
  EXPECT_EQ(response["error"]["name"], "response_too_large");
  EXPECT_EQ(host_.call_count("policy.description"), 1U);

  host_.reset_invocations();
  host_.set_policy_description(std::string("invalid\xc3\x28", 9));
  response = route("policy.description", Json{{"type", "fixture_policy"}});
  ASSERT_TRUE(response.contains("error")) << response.dump();
  EXPECT_EQ(response["error"]["domain"], "daemon");
  EXPECT_EQ(response["error"]["name"], "internal_error");
  EXPECT_EQ(host_.call_count("policy.description"), 1U);

  host_.reset_invocations();
  PolicyInfoSnapshot invalid_policy;
  invalid_policy.policy_class = PolicyClass::Interactive;
  invalid_policy.policy_type = "Bad";
  invalid_policy.binding_generation = 1;
  host_.set_policy_info(invalid_policy);
  response = route("policy.info", Json{{"policy_class", "interactive"}});
  ASSERT_TRUE(response.contains("error")) << response.dump();
  EXPECT_EQ(response["error"]["domain"], "daemon");
  EXPECT_EQ(response["error"]["name"], "internal_error");
  EXPECT_EQ(host_.call_count("policy.info"), 1U);

  host_.reset_invocations();
  ExecutionInfoSnapshot mismatched;
  mismatched.intent = ComputeIntent::GlobalHighPrecision;
  mismatched.execution_type = "serial_debug";
  mismatched.stats = "idle";
  host_.set_execution_info(mismatched);
  response = route("execution.info", Json{{"session_id", session_id_},
                                          {"intent", "real_time_update"}});
  ASSERT_TRUE(response.contains("error")) << response.dump();
  EXPECT_EQ(response["error"]["domain"], "daemon");
  EXPECT_EQ(response["error"]["name"], "internal_error");
  EXPECT_EQ(host_.call_count("execution.info"), 1U);
}

TEST_F(HostRoutedGraphStateProtocolTest,
       PluginLoadValidatesEveryKnownDirectoryBeforeMutation) {
  std::vector<Json> malformed = {
      Json::object(),
      Json{{"directories", "not-an-array"}},
      Json{{"directories", Json::array({""})}},
      Json{{"directories", Json::array({std::string("bad\0path", 8)})}},
      Json{{"directories",
            Json::array({std::string(kPathTextMaxBytes + 1, 'x')})}},
      Json{{"directories", Json::array()}},
  };
  malformed.back()["directories"] = Json::array();
  for (std::size_t index = 0; index < kPathArrayMaxEntries + 1; ++index) {
    malformed.back()["directories"].push_back("path");
  }

  for (std::size_t index = 0; index < malformed.size(); ++index) {
    const Json response = route("plugins.load_report", malformed[index],
                                "malformed-plugin-" + std::to_string(index));
    ASSERT_TRUE(response.contains("error")) << response.dump();
    EXPECT_EQ(response["error"]["domain"], "protocol");
    EXPECT_EQ(response["error"]["name"], "invalid_params");
  }
  EXPECT_EQ(host_.call_count("plugins.load_report"), 0U);

  const Json accepted =
      route("plugins.load_report",
            Json{{"directories", Json::array()}, {"future_option", "ignored"}});
  ASSERT_TRUE(accepted.contains("result")) << accepted.dump();
  EXPECT_EQ(host_.call_count("plugins.load_report"), 1U);
}

TEST_F(HostRoutedGraphStateProtocolTest,
       PluginControlPreservesHostFailureAndRejectsMalformedReturnedValues) {
  host_.set_status(
      "plugins.load_report",
      OperationStatus{false, OperationErrorDomain::Graph,
                      static_cast<std::int32_t>(GraphErrc::NoOperation),
                      "wrong-name", "no registrar"});
  Json response =
      route("plugins.load_report", Json{{"directories", Json::array()}});
  ASSERT_TRUE(response.contains("error")) << response.dump();
  EXPECT_EQ(response["error"]["domain"], "graph");
  EXPECT_EQ(response["error"]["code"],
            static_cast<int>(GraphErrc::NoOperation));
  EXPECT_EQ(response["error"]["name"], "no_operation");
  EXPECT_EQ(host_.call_count("plugins.load_report"), 1U);

  host_.reset_invocations();
  host_.set_status("plugins.load_report", OperationStatus{});
  HostPluginLoadReport malformed;
  malformed.attempted = 1;
  malformed.loaded = 1;
  malformed.errors.push_back(
      HostPluginLoadError{"plugin", GraphErrc::Io, "inconsistent"});
  host_.set_plugin_load_report(malformed);
  response = route("plugins.load_report", Json{{"directories", Json::array()}});
  ASSERT_TRUE(response.contains("error")) << response.dump();
  EXPECT_EQ(response["error"]["domain"], "daemon");
  EXPECT_EQ(response["error"]["name"], "internal_error");
  EXPECT_EQ(host_.call_count("plugins.load_report"), 1U);

  host_.reset_invocations();
  malformed.loaded = 0;
  malformed.errors.front().code = static_cast<GraphErrc>(1000);
  host_.set_plugin_load_report(malformed);
  response = route("plugins.load_report", Json{{"directories", Json::array()}});
  ASSERT_TRUE(response.contains("error")) << response.dump();
  EXPECT_EQ(response["error"]["name"], "internal_error");
  EXPECT_EQ(host_.call_count("plugins.load_report"), 1U);

  host_.reset_invocations();
  host_.set_plugins_unload_count(-1);
  response = route("plugins.unload_all", Json::object());
  ASSERT_TRUE(response.contains("error")) << response.dump();
  EXPECT_EQ(response["error"]["name"], "internal_error");
  EXPECT_EQ(host_.call_count("plugins.unload_all"), 1U);
}

TEST_F(HostRoutedGraphStateProtocolTest,
       PluginReportValidatesAllGraphCodesTextBoundsAndAggregateFrame) {
  const std::array<GraphErrc, 9> codes = {
      GraphErrc::Unknown,     GraphErrc::NotFound,
      GraphErrc::Cycle,       GraphErrc::Io,
      GraphErrc::InvalidYaml, GraphErrc::MissingDependency,
      GraphErrc::NoOperation, GraphErrc::InvalidParameter,
      GraphErrc::ComputeError};
  HostPluginLoadReport report;
  report.attempted = static_cast<int>(codes.size());
  for (GraphErrc code : codes) {
    report.errors.push_back(
        HostPluginLoadError{"", code, std::string("diagnostic\xff", 11)});
  }
  host_.set_plugin_load_report(report);
  Json response =
      route("plugins.load_report", Json{{"directories", Json::array()}});
  ASSERT_TRUE(response.contains("result")) << response.dump();
  ASSERT_EQ(response["result"]["errors"].size(), codes.size());
  for (std::size_t index = 0; index < codes.size(); ++index) {
    EXPECT_EQ(response["result"]["errors"][index]["code"],
              static_cast<int>(codes[index]));
    EXPECT_EQ(response["result"]["errors"][index]["name"],
              graph_error_stable_name(codes[index]));
    EXPECT_TRUE(response["result"]["errors"][index]["message"].is_string());
  }
  EXPECT_EQ(host_.call_count("plugins.load_report"), 1U);

  host_.reset_invocations();
  report.attempted = 1;
  report.errors.resize(1);
  report.errors.front() = HostPluginLoadError{std::string("invalid\xc3\x28", 9),
                                              GraphErrc::Io, "diagnostic"};
  host_.set_plugin_load_report(report);
  response = route("plugins.load_report", Json{{"directories", Json::array()}});
  ASSERT_TRUE(response.contains("error")) << response.dump();
  EXPECT_EQ(response["error"]["domain"], "daemon");
  EXPECT_EQ(response["error"]["name"], "internal_error");
  EXPECT_EQ(host_.call_count("plugins.load_report"), 1U);

  host_.reset_invocations();
  report.errors.front().path.clear();
  report.new_op_keys = {std::string(kShortTextMaxBytes + 1, 'k')};
  host_.set_plugin_load_report(report);
  response = route("plugins.load_report", Json{{"directories", Json::array()}});
  ASSERT_TRUE(response.contains("error")) << response.dump();
  EXPECT_EQ(response["error"]["domain"], "protocol");
  EXPECT_EQ(response["error"]["name"], "response_too_large");
  EXPECT_EQ(host_.call_count("plugins.load_report"), 1U);

  host_.reset_invocations();
  report.new_op_keys.clear();
  report.attempted = 2048;
  report.errors.assign(
      2048,
      HostPluginLoadError{std::string(kPathTextMaxBytes, 'p'), GraphErrc::Io,
                          std::string(kDiagnosticTextMaxBytes, 'm')});
  host_.set_plugin_load_report(std::move(report));
  response = route("plugins.load_report", Json{{"directories", Json::array()}},
                   std::string(kRequestTextMaxBytes, 'r'));
  ASSERT_TRUE(response.contains("error")) << response.dump();
  EXPECT_EQ(response["error"]["domain"], "protocol");
  EXPECT_EQ(response["error"]["name"], "response_too_large");
  EXPECT_EQ(host_.call_count("plugins.load_report"), 1U);
}

TEST_F(HostRoutedGraphStateProtocolTest,
       ComputeLifecyclePreservesEveryTypedHostRequestFieldAndStableShapes) {
  const Json submitted =
      route("compute.submit", valid_compute_submit_params(session_id_));
  ASSERT_TRUE(submitted.contains("result")) << submitted.dump();
  const Json& queued = submitted["result"];
  ASSERT_EQ(queued.size(), 6U);
  ASSERT_TRUE(queued["compute_id"].is_string());
  const std::string compute_id = queued["compute_id"].get<std::string>();
  EXPECT_TRUE(valid_opaque_id(compute_id));
  EXPECT_EQ(queued["session_id"], session_id_);
  EXPECT_EQ(queued["state"], "queued");
  EXPECT_FALSE(queued["cancellable"].get<bool>());
  EXPECT_TRUE(queued["status"].is_null());
  EXPECT_TRUE(queued["output"].is_null());

  const Json terminal = wait_for_compute_terminal(compute_id);
  ASSERT_TRUE(terminal.contains("result")) << terminal.dump();
  EXPECT_EQ(terminal["result"]["state"], "succeeded");
  EXPECT_EQ(terminal["result"]["session_id"], session_id_);
  EXPECT_FALSE(terminal["result"]["cancellable"].get<bool>());
  EXPECT_EQ(terminal["result"]["status"], (Json{{"ok", true},
                                                {"domain", "none"},
                                                {"code", 0},
                                                {"name", ""},
                                                {"message", ""}}));
  EXPECT_TRUE(terminal["result"]["output"].is_null());

  const std::vector<::ps::testing::IpcHostInvocation> calls =
      host_.invocations();
  ASSERT_EQ(calls.size(), 1U);
  ASSERT_EQ(calls.front().method, "compute.submit");
  ASSERT_TRUE(calls.front().compute_request.has_value());
  EXPECT_FALSE(calls.front().image_compute);
  const HostComputeRequest& request = *calls.front().compute_request;
  EXPECT_EQ(request.session.value, "ipc-host-spy-session");
  EXPECT_EQ(request.node.value, 37);
  EXPECT_EQ(request.cache.precision, "float16");
  EXPECT_TRUE(request.cache.force_recache);
  EXPECT_TRUE(request.cache.disable_disk_cache);
  EXPECT_TRUE(request.cache.nosave);
  EXPECT_TRUE(request.execution.parallel);
  EXPECT_TRUE(request.execution.quiet);
  EXPECT_TRUE(request.telemetry.enable_timing);
  ASSERT_TRUE(request.intent.has_value());
  EXPECT_EQ(*request.intent, ComputeIntent::RealTimeUpdate);
  ASSERT_TRUE(request.dirty_roi.has_value());
  EXPECT_EQ(request.dirty_roi->x, -4);
  EXPECT_EQ(request.dirty_roi->y, 5);
  EXPECT_EQ(request.dirty_roi->width, 6);
  EXPECT_EQ(request.dirty_roi->height, 7);

  const Json result = route("compute.result", Json{{"compute_id", compute_id}});
  ASSERT_TRUE(result.contains("result")) << result.dump();
  EXPECT_EQ(result["result"], terminal["result"]);
  const Json repeated =
      route("compute.result",
            Json{{"compute_id", compute_id}, {"future_query_field", true}});
  ASSERT_TRUE(repeated.contains("result")) << repeated.dump();
  EXPECT_EQ(repeated["result"], result["result"]);
  EXPECT_EQ(host_.call_count("compute.submit"), 1U);

  const Json released =
      route("compute.release", Json{{"compute_id", compute_id}});
  ASSERT_TRUE(released.contains("result")) << released.dump();
  EXPECT_EQ(released["result"],
            (Json{{"compute_id", compute_id}, {"released", true}}));
  const Json absent = route("compute.status", Json{{"compute_id", compute_id}});
  ASSERT_TRUE(absent.contains("error")) << absent.dump();
  EXPECT_EQ(absent["error"]["domain"], "daemon");
  EXPECT_EQ(absent["error"]["name"], "job_not_found");
}

TEST_F(HostRoutedGraphStateProtocolTest,
       ComputeSubmitPreservesEveryDefaultForMinimalAndNullableRequests) {
  std::vector<Json> requests{Json{{"session_id", session_id_},
                                  {"node_id", 41},
                                  {"result_mode", "status"}},
                             Json{{"session_id", session_id_},
                                  {"node_id", 42},
                                  {"intent", nullptr},
                                  {"dirty_roi", nullptr},
                                  {"result_mode", "status"}}};

  for (std::size_t index = 0; index < requests.size(); ++index) {
    SCOPED_TRACE(index);
    host_.reset_invocations();
    const Json submitted = route("compute.submit", std::move(requests[index]),
                                 "minimal-compute-" + std::to_string(index));
    ASSERT_TRUE(submitted.contains("result")) << submitted.dump();
    const std::string compute_id =
        submitted["result"]["compute_id"].get<std::string>();
    const Json terminal = wait_for_compute_terminal(compute_id);
    ASSERT_TRUE(terminal.contains("result")) << terminal.dump();
    EXPECT_EQ(terminal["result"]["state"], "succeeded");

    const std::vector<::ps::testing::IpcHostInvocation> calls =
        host_.invocations();
    ASSERT_EQ(calls.size(), 1U);
    ASSERT_EQ(calls.front().method, "compute.submit");
    ASSERT_TRUE(calls.front().compute_request.has_value());
    EXPECT_FALSE(calls.front().image_compute);
    const HostComputeRequest& request = *calls.front().compute_request;
    EXPECT_EQ(request.session.value, "ipc-host-spy-session");
    EXPECT_EQ(request.node.value, 41 + static_cast<std::int64_t>(index));
    EXPECT_TRUE(request.cache.precision.empty());
    EXPECT_FALSE(request.cache.force_recache);
    EXPECT_FALSE(request.cache.disable_disk_cache);
    EXPECT_FALSE(request.cache.nosave);
    EXPECT_FALSE(request.execution.parallel);
    EXPECT_FALSE(request.execution.quiet);
    EXPECT_FALSE(request.telemetry.enable_timing);
    EXPECT_FALSE(request.intent.has_value());
    EXPECT_FALSE(request.dirty_roi.has_value());

    const Json released =
        route("compute.release", Json{{"compute_id", compute_id}},
              "minimal-release-" + std::to_string(index));
    ASSERT_TRUE(released.contains("result")) << released.dump();
  }
}

TEST_F(HostRoutedGraphStateProtocolTest,
       ComputeSubmitValidatesAllKnownFieldsBeforeSessionOrHostAccess) {
  const std::string unknown_session(32, 'a');
  const Json valid = valid_compute_submit_params(unknown_session);
  std::vector<std::pair<std::string, Json>> malformed;

  Json missing_session = valid;
  missing_session.erase("session_id");
  malformed.emplace_back("missing session", std::move(missing_session));
  Json malformed_session = valid;
  malformed_session["session_id"] = "not-an-opaque-id";
  malformed.emplace_back("malformed session", std::move(malformed_session));
  Json negative_node = valid;
  negative_node["node_id"] = -1;
  malformed.emplace_back("negative node", std::move(negative_node));
  Json wide_node = valid;
  wide_node["node_id"] = std::uint64_t{4294967296ULL};
  malformed.emplace_back("wide node", std::move(wide_node));
  Json cache_scalar = valid;
  cache_scalar["cache"] = true;
  malformed.emplace_back("cache scalar", std::move(cache_scalar));
  Json precision_type = valid;
  precision_type["cache"]["precision"] = 1;
  malformed.emplace_back("precision type", std::move(precision_type));
  Json precision_bound = valid;
  precision_bound["cache"]["precision"] =
      std::string(kShortTextMaxBytes + 1, 'p');
  malformed.emplace_back("precision bound", std::move(precision_bound));
  Json cache_flag = valid;
  cache_flag["cache"]["nosave"] = 1;
  malformed.emplace_back("cache flag", std::move(cache_flag));
  Json execution_object = valid;
  execution_object["execution"] = Json::array();
  malformed.emplace_back("execution object", std::move(execution_object));
  Json execution_flag = valid;
  execution_flag["execution"]["parallel"] = "true";
  malformed.emplace_back("execution flag", std::move(execution_flag));
  Json telemetry_flag = valid;
  telemetry_flag["telemetry"]["enable_timing"] = nullptr;
  malformed.emplace_back("telemetry flag", std::move(telemetry_flag));
  Json intent = valid;
  intent["intent"] = "future_intent";
  malformed.emplace_back("intent", std::move(intent));
  Json dirty_roi = valid;
  dirty_roi["dirty_roi"]["width"] = 1.5;
  malformed.emplace_back("dirty roi", std::move(dirty_roi));
  Json missing_mode = valid;
  missing_mode.erase("result_mode");
  malformed.emplace_back("missing result mode", std::move(missing_mode));
  Json unknown_mode = valid;
  unknown_mode["result_mode"] = "stream";
  malformed.emplace_back("unknown result mode", std::move(unknown_mode));

  for (auto& test_case : malformed) {
    const Json response =
        route("compute.submit", std::move(test_case.second), test_case.first);
    ASSERT_TRUE(response.contains("error"))
        << test_case.first << ": " << response.dump();
    EXPECT_EQ(response["error"]["domain"], "protocol") << test_case.first;
    EXPECT_EQ(response["error"]["name"], "invalid_params") << test_case.first;
  }
  EXPECT_EQ(host_.call_count("compute.submit"), 0U);

  Json nullable = valid_compute_submit_params(unknown_session);
  nullable["intent"] = nullptr;
  nullable["dirty_roi"] = nullptr;
  const Json admitted = route("compute.submit", std::move(nullable));
  ASSERT_TRUE(admitted.contains("error")) << admitted.dump();
  EXPECT_EQ(admitted["error"]["domain"], "graph");
  EXPECT_EQ(admitted["error"]["name"], "not_found");
  EXPECT_EQ(host_.call_count("compute.submit"), 0U);

  for (std::string_view method :
       {"compute.status", "compute.result", "compute.release"}) {
    for (const Json& params :
         {Json::object(), Json{{"compute_id", 1}}, Json{{"compute_id", "abc"}},
          Json{{"compute_id", std::string(32, 'A')}}}) {
      const Json response = route(std::string(method), params);
      ASSERT_TRUE(response.contains("error")) << response.dump();
      EXPECT_EQ(response["error"]["domain"], "protocol");
      EXPECT_EQ(response["error"]["name"], "invalid_params");
    }
  }
}

TEST_F(HostRoutedGraphStateProtocolTest,
       AcceptedHostAndImageFailuresRemainImmutableNestedResults) {
  host_.set_status(
      "compute.submit",
      failure_status(OperationErrorDomain::Graph,
                     static_cast<std::int32_t>(GraphErrc::ComputeError),
                     "ignored", "exact accepted Host failure"));
  Json submitted =
      route("compute.submit", valid_compute_submit_params(session_id_));
  ASSERT_TRUE(submitted.contains("result")) << submitted.dump();
  const std::string failed_id =
      submitted["result"]["compute_id"].get<std::string>();
  Json failed = wait_for_compute_terminal(failed_id);
  ASSERT_TRUE(failed.contains("result")) << failed.dump();
  EXPECT_EQ(failed["result"]["state"], "failed");
  EXPECT_EQ(failed["result"]["status"]["domain"], "graph");
  EXPECT_EQ(failed["result"]["status"]["code"],
            static_cast<std::int32_t>(GraphErrc::ComputeError));
  EXPECT_EQ(failed["result"]["status"]["name"], "compute_error");
  EXPECT_EQ(failed["result"]["status"]["message"],
            "exact accepted Host failure");
  EXPECT_TRUE(failed["result"]["output"].is_null());

  host_.set_status("compute.submit", ok_status());
  const Json immutable =
      route("compute.result", Json{{"compute_id", failed_id}});
  ASSERT_TRUE(immutable.contains("result")) << immutable.dump();
  EXPECT_EQ(immutable["result"], failed["result"]);
  EXPECT_EQ(host_.call_count("compute.submit"), 1U);
  ASSERT_TRUE(route("compute.release", Json{{"compute_id", failed_id}})
                  .contains("result"));

  ImageBuffer invalid_image;
  invalid_image.width = 1;
  host_.set_compute_image(std::move(invalid_image));
  host_.reset_invocations();
  submitted = route("compute.submit",
                    valid_compute_submit_params(session_id_, "image"));
  ASSERT_TRUE(submitted.contains("result")) << submitted.dump();
  const std::string image_id =
      submitted["result"]["compute_id"].get<std::string>();
  Json image_failed = wait_for_compute_terminal(image_id);
  ASSERT_TRUE(image_failed.contains("result")) << image_failed.dump();
  EXPECT_EQ(image_failed["result"]["state"], "failed");
  EXPECT_EQ(image_failed["result"]["status"]["domain"], "daemon");
  EXPECT_EQ(image_failed["result"]["status"]["name"], "internal_error");
  EXPECT_TRUE(image_failed["result"]["output"].is_null());
  const auto image_calls = host_.invocations();
  ASSERT_EQ(image_calls.size(), 1U);
  EXPECT_TRUE(image_calls.front().image_compute);
  EXPECT_EQ(host_.call_count("compute.submit"), 1U);
  const Json image_result =
      route("compute.result", Json{{"compute_id", image_id}});
  ASSERT_TRUE(image_result.contains("result")) << image_result.dump();
  EXPECT_EQ(image_result["result"], image_failed["result"]);
  ASSERT_TRUE(route("compute.release", Json{{"compute_id", image_id}})
                  .contains("result"));

  ImageBuffer valid_image =
      make_aligned_cpu_image_buffer(2, 2, 1, DataType::UINT8);
  ASSERT_NE(valid_image.data, nullptr);
  host_.set_compute_image(std::move(valid_image));
  host_.reset_invocations();
  submitted = route("compute.submit",
                    valid_compute_submit_params(session_id_, "image"));
  ASSERT_TRUE(submitted.contains("result")) << submitted.dump();
  const std::string successful_image_id =
      submitted["result"]["compute_id"].get<std::string>();
  const Json image_succeeded = wait_for_compute_terminal(successful_image_id);
  ASSERT_TRUE(image_succeeded.contains("result")) << image_succeeded.dump();
  EXPECT_EQ(image_succeeded["result"]["state"], "succeeded");
  EXPECT_TRUE(image_succeeded["result"]["status"]["ok"].get<bool>());
  EXPECT_TRUE(image_succeeded["result"]["output"].is_null());
  const auto successful_image_calls = host_.invocations();
  ASSERT_EQ(successful_image_calls.size(), 1U);
  EXPECT_TRUE(successful_image_calls.front().image_compute);

  const Json delivered =
      route("compute.result", Json{{"compute_id", successful_image_id}});
  ASSERT_TRUE(delivered.contains("result")) << delivered.dump();
  ASSERT_TRUE(delivered["result"]["output"].is_object());
  const Json& output = delivered["result"]["output"];
  ASSERT_EQ(output.size(), 12U);
  ASSERT_TRUE(output["output_id"].is_string());
  ASSERT_TRUE(output["delivery_id"].is_string());
  EXPECT_TRUE(valid_opaque_id(output["output_id"].get<std::string>()));
  EXPECT_TRUE(valid_opaque_id(output["delivery_id"].get<std::string>()));
  EXPECT_TRUE(
      std::filesystem::path(output["path"].get<std::string>()).is_absolute());
  EXPECT_EQ(output["width"], 2);
  EXPECT_EQ(output["height"], 2);
  EXPECT_EQ(output["channels"], 1);
  EXPECT_EQ(output["data_type"], "uint8");
  EXPECT_EQ(output["device"], "cpu");
  EXPECT_EQ(output["row_step"], 2);
  EXPECT_EQ(output["byte_size"], 4);
  EXPECT_TRUE(output["filesystem_device"].is_number_unsigned());
  EXPECT_TRUE(output["inode"].is_number_unsigned());
  EXPECT_FALSE(output.contains("output_reference"));
  EXPECT_TRUE(
      std::filesystem::is_regular_file(output["path"].get<std::string>()));

  const Json repeated =
      route("compute.result", Json{{"compute_id", successful_image_id}});
  ASSERT_TRUE(repeated.contains("result")) << repeated.dump();
  EXPECT_EQ(repeated["result"]["output"], output);
  EXPECT_EQ(host_.call_count("compute.submit"), 1U);

  const Json released =
      route("compute.release", Json{{"compute_id", successful_image_id},
                                    {"delivery_id", output["delivery_id"]}});
  ASSERT_TRUE(released.contains("result")) << released.dump();
  EXPECT_FALSE(std::filesystem::exists(output["path"].get<std::string>()));
}

TEST_F(HostRoutedGraphStateProtocolTest,
       EmptyImageAndArtifactQuotaRemainNormalTerminalOutcomes) {
  host_.set_compute_image(ImageBuffer{});
  Json submitted = route("compute.submit",
                         valid_compute_submit_params(session_id_, "image"));
  ASSERT_TRUE(submitted.contains("result")) << submitted.dump();
  const std::string empty_id =
      submitted["result"]["compute_id"].get<std::string>();
  const Json empty_terminal = wait_for_compute_terminal(empty_id);
  ASSERT_TRUE(empty_terminal.contains("result")) << empty_terminal.dump();
  EXPECT_EQ(empty_terminal["result"]["state"], "succeeded");
  EXPECT_TRUE(empty_terminal["result"]["output"].is_null());
  const Json empty_result =
      route("compute.result", Json{{"compute_id", empty_id}});
  ASSERT_TRUE(empty_result.contains("result")) << empty_result.dump();
  EXPECT_TRUE(empty_result["result"]["output"].is_null());
  ASSERT_TRUE(route("compute.release", Json{{"compute_id", empty_id}})
                  .contains("result"));

  auto byte = std::make_shared<std::uint8_t>(0x5a);
  ImageBuffer oversized;
  oversized.width = 512 * 1024 * 1024 + 1;
  oversized.height = 1;
  oversized.channels = 1;
  oversized.type = DataType::UINT8;
  oversized.device = Device::CPU;
  oversized.step = static_cast<std::size_t>(oversized.width);
  oversized.data = std::shared_ptr<void>(byte, byte.get());
  host_.set_compute_image(std::move(oversized));
  submitted = route("compute.submit",
                    valid_compute_submit_params(session_id_, "image"));
  ASSERT_TRUE(submitted.contains("result")) << submitted.dump();
  const std::string quota_id =
      submitted["result"]["compute_id"].get<std::string>();
  const Json quota_terminal = wait_for_compute_terminal(quota_id);
  ASSERT_TRUE(quota_terminal.contains("result")) << quota_terminal.dump();
  EXPECT_EQ(quota_terminal["result"]["state"], "failed");
  EXPECT_EQ(quota_terminal["result"]["status"]["domain"], "daemon");
  EXPECT_EQ(quota_terminal["result"]["status"]["name"],
            "artifact_limit_exceeded");
  EXPECT_TRUE(quota_terminal["result"]["output"].is_null());
  const Json quota_result =
      route("compute.result", Json{{"compute_id", quota_id}});
  ASSERT_TRUE(quota_result.contains("result")) << quota_result.dump();
  EXPECT_EQ(quota_result["result"], quota_terminal["result"]);
  ASSERT_TRUE(route("compute.release", Json{{"compute_id", quota_id}})
                  .contains("result"));
}

TEST_F(HostRoutedGraphStateProtocolTest,
       ResultTimeTamperIsTopLevelAndLeavesTerminalJobReleasable) {
  ImageBuffer image = make_aligned_cpu_image_buffer(2, 1, 1, DataType::UINT8);
  ASSERT_NE(image.data, nullptr);
  host_.set_compute_image(std::move(image));
  const Json submitted = route(
      "compute.submit", valid_compute_submit_params(session_id_, "image"));
  ASSERT_TRUE(submitted.contains("result")) << submitted.dump();
  const std::string compute_id =
      submitted["result"]["compute_id"].get<std::string>();
  ASSERT_TRUE(wait_for_compute_terminal(compute_id).contains("result"));

  const Json delivered =
      route("compute.result", Json{{"compute_id", compute_id}});
  ASSERT_TRUE(delivered.contains("result")) << delivered.dump();
  const Json metadata = delivered["result"]["output"];
  ASSERT_TRUE(metadata.is_object());
  const std::string artifact_path = metadata["path"].get<std::string>();
  const std::string delivery_id = metadata["delivery_id"].get<std::string>();
  const std::filesystem::path victim =
      runtime_directory_.path() / "output-tamper-victim.bin";
  {
    std::ofstream stream(victim, std::ios::binary);
    stream.exceptions(std::ios::badbit | std::ios::failbit);
    stream.put('v');
  }
  ASSERT_EQ(::chmod(victim.c_str(), 0600), 0);
  ASSERT_EQ(::unlink(artifact_path.c_str()), 0);
  ASSERT_EQ(::symlink(victim.c_str(), artifact_path.c_str()), 0);

  const Json missing =
      route("compute.result", Json{{"compute_id", compute_id}});
  ASSERT_TRUE(missing.contains("error")) << missing.dump();
  EXPECT_EQ(missing["error"]["domain"], "daemon");
  EXPECT_EQ(missing["error"]["name"], "artifact_not_found");
  const Json released =
      route("compute.release",
            Json{{"compute_id", compute_id}, {"delivery_id", delivery_id}});
  ASSERT_TRUE(released.contains("result")) << released.dump();
  EXPECT_TRUE(std::filesystem::exists(victim));
  struct stat replacement{};
  ASSERT_EQ(::lstat(artifact_path.c_str(), &replacement), 0);
  EXPECT_TRUE(S_ISLNK(replacement.st_mode));
}

TEST_F(HostRoutedGraphStateProtocolTest,
       MatchingDeliveryCanReleaseLeaseAfterNormalJobRemoval) {
  ImageBuffer image = make_aligned_cpu_image_buffer(1, 1, 1, DataType::UINT8);
  ASSERT_NE(image.data, nullptr);
  host_.set_compute_image(std::move(image));
  const Json submitted = route(
      "compute.submit", valid_compute_submit_params(session_id_, "image"));
  ASSERT_TRUE(submitted.contains("result")) << submitted.dump();
  const std::string compute_id =
      submitted["result"]["compute_id"].get<std::string>();
  ASSERT_TRUE(wait_for_compute_terminal(compute_id).contains("result"));
  const Json delivered =
      route("compute.result", Json{{"compute_id", compute_id}});
  ASSERT_TRUE(delivered.contains("result")) << delivered.dump();
  const Json metadata = delivered["result"]["output"];
  const std::string artifact_path = metadata["path"].get<std::string>();
  const std::string delivery_id = metadata["delivery_id"].get<std::string>();

  const Json removed =
      route("compute.release", Json{{"compute_id", compute_id}});
  ASSERT_TRUE(removed.contains("result")) << removed.dump();
  EXPECT_TRUE(std::filesystem::exists(artifact_path));
  const Json absent = route("compute.status", Json{{"compute_id", compute_id}});
  ASSERT_TRUE(absent.contains("error")) << absent.dump();
  EXPECT_EQ(absent["error"]["name"], "job_not_found");

  const Json orphan_released =
      route("compute.release", Json{{"compute_id", compute_id},
                                    {"delivery_id", delivery_id},
                                    {"future_release_field", true}});
  ASSERT_TRUE(orphan_released.contains("result")) << orphan_released.dump();
  EXPECT_EQ(orphan_released["result"],
            (Json{{"compute_id", compute_id}, {"released", true}}));
  EXPECT_FALSE(std::filesystem::exists(artifact_path));
  const Json repeated =
      route("compute.release",
            Json{{"compute_id", compute_id}, {"delivery_id", delivery_id}});
  ASSERT_TRUE(repeated.contains("error")) << repeated.dump();
  EXPECT_EQ(repeated["error"]["name"], "job_not_found");
}

TEST_F(HostRoutedGraphStateProtocolTest,
       MalformedDeliveryIdDoesNotRemoveTerminalJob) {
  const Json submitted =
      route("compute.submit", valid_compute_submit_params(session_id_));
  ASSERT_TRUE(submitted.contains("result")) << submitted.dump();
  const std::string compute_id =
      submitted["result"]["compute_id"].get<std::string>();
  ASSERT_TRUE(wait_for_compute_terminal(compute_id).contains("result"));

  for (const Json& delivery_id :
       {Json(nullptr), Json(1), Json("abc"), Json(std::string(32, 'A'))}) {
    const Json rejected =
        route("compute.release",
              Json{{"compute_id", compute_id}, {"delivery_id", delivery_id}});
    ASSERT_TRUE(rejected.contains("error")) << rejected.dump();
    EXPECT_EQ(rejected["error"]["domain"], "protocol");
    EXPECT_EQ(rejected["error"]["name"], "invalid_params");
    EXPECT_TRUE(route("compute.status", Json{{"compute_id", compute_id}})
                    .contains("result"));
  }
  EXPECT_TRUE(route("compute.release", Json{{"compute_id", compute_id}})
                  .contains("result"));
}

TEST(IpcOutputDeliveryValidation,
     BindsPrivateReferenceAndRequiresExactScalarLayout) {
  OutputArtifactDelivery delivery;
  delivery.metadata.output_id = "11111111111111111111111111111111";
  delivery.metadata.path = "/tmp/output-validation.bin";
  delivery.metadata.width = 2;
  delivery.metadata.height = 3;
  delivery.metadata.channels = 2;
  delivery.metadata.device = Device::CPU;
  delivery.delivery_id = "22222222222222222222222222222222";

  const std::vector<std::pair<DataType, std::size_t>> scalar_cases = {
      {DataType::UINT8, 1}, {DataType::INT8, 1},    {DataType::UINT16, 2},
      {DataType::INT16, 2}, {DataType::FLOAT32, 4}, {DataType::FLOAT64, 8}};
  for (const auto& scalar_case : scalar_cases) {
    delivery.metadata.data_type = scalar_case.first;
    delivery.metadata.row_step = 4 * scalar_case.second;
    delivery.metadata.byte_size = 12 * scalar_case.second;
    EXPECT_TRUE(
        valid_output_delivery_for_wire(delivery, delivery.metadata.output_id));
  }

  delivery.metadata.data_type = DataType::UINT16;
  delivery.metadata.row_step = 8;
  delivery.metadata.byte_size = 24;
  EXPECT_FALSE(valid_output_delivery_for_wire(
      delivery, "33333333333333333333333333333333"));
  ++delivery.metadata.row_step;
  EXPECT_FALSE(
      valid_output_delivery_for_wire(delivery, delivery.metadata.output_id));
  --delivery.metadata.row_step;
  ++delivery.metadata.byte_size;
  EXPECT_FALSE(
      valid_output_delivery_for_wire(delivery, delivery.metadata.output_id));
  --delivery.metadata.byte_size;
  delivery.metadata.data_type = static_cast<DataType>(999);
  EXPECT_FALSE(
      valid_output_delivery_for_wire(delivery, delivery.metadata.output_id));

  delivery.metadata.data_type = DataType::FLOAT64;
  delivery.metadata.width = std::numeric_limits<int>::max();
  delivery.metadata.channels = std::numeric_limits<int>::max();
  delivery.metadata.row_step = std::numeric_limits<std::size_t>::max();
  delivery.metadata.byte_size = std::numeric_limits<std::size_t>::max();
  EXPECT_FALSE(
      valid_output_delivery_for_wire(delivery, delivery.metadata.output_id));
}

TEST_F(HostRoutedGraphStateProtocolTest,
       ComputePollingReleaseAndCloseRemainAvailableWithoutHostLock) {
  const auto gate = std::make_shared<ProtocolGateState>();
  std::future<Json> closing;
  ScopedProtocolGateRelease release_guard(gate->mutex, gate->changed,
                                          gate->released);
  host_.set_call_hook([gate](std::string_view method) {
    if (method != "compute.submit") {
      return;
    }
    std::unique_lock<std::mutex> lock(gate->mutex);
    gate->entered = true;
    gate->changed.notify_all();
    gate->changed.wait(lock, [gate] { return gate->released; });
  });

  const Json submitted =
      route("compute.submit", valid_compute_submit_params(session_id_));
  ASSERT_TRUE(submitted.contains("result")) << submitted.dump();
  const std::string compute_id =
      submitted["result"]["compute_id"].get<std::string>();
  {
    std::unique_lock<std::mutex> lock(gate->mutex);
    ASSERT_TRUE(gate->changed.wait_for(lock, std::chrono::seconds(2),
                                       [gate] { return gate->entered; }));
  }

  const Json running =
      route("compute.status", Json{{"compute_id", compute_id}});
  ASSERT_TRUE(running.contains("result")) << running.dump();
  EXPECT_EQ(running["result"]["state"], "running");
  EXPECT_TRUE(running["result"]["status"].is_null());
  EXPECT_TRUE(route("daemon.ping", Json::object()).contains("result"));
  for (std::string_view method : {"compute.result", "compute.release"}) {
    const Json premature =
        route(std::string(method), Json{{"compute_id", compute_id}});
    ASSERT_TRUE(premature.contains("error")) << premature.dump();
    EXPECT_EQ(premature["error"]["domain"], "daemon");
    EXPECT_EQ(premature["error"]["name"], "job_not_ready");
  }

  closing = std::async(std::launch::async, [&] {
    return route("graph.close", Json{{"session_id", session_id_}});
  });
  const auto closing_deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(2);
  Json rejected;
  while (std::chrono::steady_clock::now() < closing_deadline) {
    rejected =
        route("compute.submit", valid_compute_submit_params(session_id_));
    if (rejected.contains("error") && rejected["error"]["domain"] == "graph") {
      break;
    }
    std::this_thread::yield();
  }
  ASSERT_TRUE(rejected.contains("error")) << rejected.dump();
  EXPECT_EQ(rejected["error"]["name"], "not_found");
  EXPECT_EQ(closing.wait_for(std::chrono::milliseconds(0)),
            std::future_status::timeout);

  release_guard.release();
  ASSERT_EQ(closing.wait_for(std::chrono::seconds(2)),
            std::future_status::ready);
  const Json closed = closing.get();
  ASSERT_TRUE(closed.contains("result")) << closed.dump();
  EXPECT_TRUE(closed["result"]["closed"].get<bool>());
  const Json terminal = wait_for_compute_terminal(compute_id);
  ASSERT_TRUE(terminal.contains("result")) << terminal.dump();
  EXPECT_EQ(terminal["result"]["state"], "succeeded");

  const Json released =
      route("compute.release", Json{{"compute_id", compute_id}});
  ASSERT_TRUE(released.contains("result")) << released.dump();
  EXPECT_EQ(released["result"],
            (Json{{"compute_id", compute_id}, {"released", true}}));
  host_.set_call_hook({});
}

TEST_F(HostRoutedGraphStateProtocolTest,
       ConcurrentTerminalReleaseHasOneAtomicStableAcknowledgement) {
  const Json submitted =
      route("compute.submit", valid_compute_submit_params(session_id_));
  ASSERT_TRUE(submitted.contains("result")) << submitted.dump();
  const std::string compute_id =
      submitted["result"]["compute_id"].get<std::string>();
  ASSERT_TRUE(wait_for_compute_terminal(compute_id).contains("result"));

  auto left = std::async(std::launch::async, [&] {
    return route("compute.release", Json{{"compute_id", compute_id}}, "left");
  });
  auto right = std::async(std::launch::async, [&] {
    return route("compute.release", Json{{"compute_id", compute_id}}, "right");
  });
  const Json left_result = left.get();
  const Json right_result = right.get();
  const int success_count = static_cast<int>(left_result.contains("result")) +
                            static_cast<int>(right_result.contains("result"));
  EXPECT_EQ(success_count, 1);
  const Json& success =
      left_result.contains("result") ? left_result : right_result;
  const Json& absent =
      left_result.contains("error") ? left_result : right_result;
  EXPECT_EQ(success["result"],
            (Json{{"compute_id", compute_id}, {"released", true}}));
  EXPECT_EQ(absent["error"]["domain"], "daemon");
  EXPECT_EQ(absent["error"]["name"], "job_not_found");
}

TEST_F(HostRoutedGraphStateProtocolTest,
       ComputeActiveCapacityRejectsBeforeAnyAdditionalHostAccess) {
  const auto gate = std::make_shared<ProtocolGateState>();
  ScopedProtocolGateRelease release_guard(gate->mutex, gate->changed,
                                          gate->released);
  host_.set_call_hook([gate](std::string_view method) {
    if (method != "compute.submit") {
      return;
    }
    std::unique_lock<std::mutex> lock(gate->mutex);
    gate->entered = true;
    gate->changed.notify_all();
    gate->changed.wait(lock, [gate] { return gate->released; });
  });

  std::vector<std::string> compute_ids;
  Json first =
      route("compute.submit", valid_compute_submit_params(session_id_));
  ASSERT_TRUE(first.contains("result")) << first.dump();
  compute_ids.push_back(first["result"]["compute_id"].get<std::string>());
  {
    std::unique_lock<std::mutex> lock(gate->mutex);
    ASSERT_TRUE(gate->changed.wait_for(lock, std::chrono::seconds(2),
                                       [gate] { return gate->entered; }));
  }

  for (int index = 1; index < 64; ++index) {
    Json params = valid_compute_submit_params(session_id_);
    params["node_id"] = index;
    Json queued = route("compute.submit", std::move(params),
                        "capacity-" + std::to_string(index));
    ASSERT_TRUE(queued.contains("result")) << queued.dump();
    EXPECT_EQ(queued["result"]["state"], "queued");
    compute_ids.push_back(queued["result"]["compute_id"].get<std::string>());
  }
  Json rejected =
      route("compute.submit", valid_compute_submit_params(session_id_),
            "capacity-rejected");
  ASSERT_TRUE(rejected.contains("error")) << rejected.dump();
  EXPECT_EQ(rejected["error"]["domain"], "daemon");
  EXPECT_EQ(rejected["error"]["name"], "capacity_exceeded");
  EXPECT_EQ(host_.call_count("compute.submit"), 1U);

  release_guard.release();
  for (const std::string& compute_id : compute_ids) {
    const Json terminal = wait_for_compute_terminal(compute_id);
    ASSERT_TRUE(terminal.contains("result")) << terminal.dump();
    ASSERT_TRUE(route("compute.release", Json{{"compute_id", compute_id}})
                    .contains("result"));
  }
  EXPECT_EQ(host_.call_count("compute.submit"), 64U);
  host_.set_call_hook({});
}

/**
 * @brief Monotonic clock controlled by stable paging protocol tests.
 *
 * @throws Nothing.
 * @note The router invokes `now()` synchronously; tests advance only between
 *       complete requests, so no additional synchronization is required.
 */
class ManualCollectionClock final {
 public:
  /**
   * @brief Returns the current injected time.
   * @return Exact deterministic monotonic sample.
   * @throws Nothing.
   */
  std::chrono::steady_clock::time_point now() const noexcept { return now_; }

  /**
   * @brief Advances injected monotonic time.
   * @param duration Positive duration to add.
   * @return Nothing.
   * @throws Nothing for the small deterministic test durations used here.
   */
  void advance(std::chrono::steady_clock::duration duration) noexcept {
    now_ += duration;
  }

 private:
  /** @brief Current deterministic monotonic sample. */
  std::chrono::steady_clock::time_point now_{};
};

/**
 * @brief Returns a small injectable paging policy for router behavior tests.
 * @return One cursor, 16 recursive entries, 8 KiB, and one-second TTL limits.
 * @throws Nothing.
 */
CollectionSnapshotLimits small_protocol_snapshot_limits() noexcept {
  CollectionSnapshotLimits limits;
  limits.records = 1;
  limits.total_bytes = 8192;
  limits.reservation_bytes = 8192;
  limits.snapshot_entries = 16;
  limits.snapshot_bytes = 8192;
  limits.page_entries = kGeneralPageMaxEntries;
  limits.ttl = std::chrono::seconds(1);
  return limits;
}

/**
 * @brief Fixture for stable cursor identity, lifetime, and quota behavior.
 *
 * @throws std::bad_alloc or std::runtime_error if deterministic runtime setup
 *         cannot allocate or start.
 * @note Every test owns a fresh router, Host spy, cursor source, and runtime;
 *       no cursor or quota state crosses test boundaries.
 */
class StableInspectionPagingProtocolTest : public ::testing::Test {
 protected:
  /**
   * @brief Loads one deterministic Host session and clears load bookkeeping.
   * @return Nothing.
   * @throws std::bad_alloc or std::runtime_error on request construction or
   *         response parsing failure.
   */
  void SetUp() override {
    const Json loaded =
        route("graph.load",
              Json{{"session_name", "stable_paging"}, {"root_dir", "/tmp"}});
    ASSERT_TRUE(loaded.contains("result")) << loaded.dump();
    session_id_ = loaded["result"]["session_id"].get<std::string>();
    ASSERT_TRUE(valid_opaque_id(session_id_));
    host_.reset_invocations();
  }

  /**
   * @brief Routes one typed request through a complete protocol envelope.
   * @param method Exact wire method.
   * @param params Complete typed params.
   * @param id Optional correlated id used for frame-budget edge cases.
   * @return Parsed response object.
   * @throws std::bad_alloc or std::runtime_error on construction/parse failure.
   */
  Json route(const std::string& method, Json params,
             std::string id = "stable-page") {
    return parse_response(router_.route(Json{
        {"protocol_version", kProtocolVersion},
        {"id", std::move(id)},
        {"method", method},
        {"params", std::move(params)}}.dump()));
  }

  /**
   * @brief Returns the next deterministic valid opaque cursor.
   * @return Unique 32-lowercase-hex token for this fixture.
   * @throws Nothing while the bounded test sequence remains below 256.
   */
  std::string next_cursor() noexcept {
    static constexpr char kHex[] = "0123456789abcdef";
    const std::size_t value = cursor_sequence_++;
    std::string cursor(32, '0');
    cursor[30] = kHex[(value >> 4U) & 0x0fU];
    cursor[31] = kHex[value & 0x0fU];
    return cursor;
  }

  /**
   * @brief Builds complete router dependencies with a focused snapshot policy.
   * @return Default compute/output policy plus deterministic snapshot inputs.
   * @throws std::bad_alloc if callback storage cannot allocate.
   * @note Only the snapshot component differs from production in this fixture;
   *       the generic private dependency value prevents a snapshot-only router
   *       constructor from becoming a parallel composition seam.
   */
  RequestRouterRuntimeDependencies runtime_dependencies() {
    RequestRouterRuntimeDependencies dependencies;
    dependencies.snapshot_limits = small_protocol_snapshot_limits();
    dependencies.snapshot_clock = [this] { return clock_.now(); };
    dependencies.snapshot_id_generator = [this] { return next_cursor(); };
    return dependencies;
  }

  /** @brief Deterministic cursor-expiry clock. */
  ManualCollectionClock clock_;
  /** @brief Cursor uniqueness source. */
  std::size_t cursor_sequence_ = 1;
  /** @brief Protected output/runtime path owner. */
  ScopedTempDirectory runtime_directory_{"ps-ipc-pages"};
  /** @brief Complete configurable Host test double. */
  ::ps::testing::IpcHostSpy host_;
  /** @brief Router with small deterministic snapshot limits and callbacks. */
  RequestRouter router_{host_, "stable-paging-test", runtime_dependencies()};
  /** @brief Active daemon-private runtime. */
  ScopedRequestRouterRuntime runtime_{router_, runtime_directory_};
  /** @brief Opaque daemon session mapped to the spy Host session. */
  std::string session_id_;
};

TEST_F(StableInspectionPagingProtocolTest,
       PluginKeyViewSortsBeforeFrozenPagingAndCallsHostOnce) {
  host_.set_ops_combined_keys({"zeta:op", "alpha:op", "middle:op"});
  const Json first = route("plugins.ops_combined_keys", Json{{"limit", 2}});
  ASSERT_TRUE(first.contains("result")) << first.dump();
  EXPECT_EQ(first["result"]["keys"], Json::array({"alpha:op", "middle:op"}));
  ASSERT_TRUE(first["result"]["cursor"].is_string());
  EXPECT_TRUE(first["result"]["has_more"].get<bool>());
  EXPECT_EQ(first["result"]["offset"], 0);

  const Json final = route("plugins.ops_combined_keys",
                           Json{{"cursor", first["result"]["cursor"]},
                                {"offset", 2},
                                {"limit", 2},
                                {"session_id", "ignored"}});
  ASSERT_TRUE(final.contains("result")) << final.dump();
  EXPECT_EQ(final["result"]["keys"], Json::array({"zeta:op"}));
  EXPECT_FALSE(final["result"]["has_more"].get<bool>());
  EXPECT_TRUE(final["result"]["cursor"].is_null());
  EXPECT_EQ(final["result"]["offset"], 2);
  EXPECT_EQ(host_.call_count("plugins.ops_combined_keys"), 1U);
}

TEST_F(StableInspectionPagingProtocolTest,
       PluginSourceViewsUseSortedRowsAndCursorBinding) {
  host_.set_ops_sources(
      {{"zeta:op", "/plugins/zeta"}, {"alpha:op", "built-in"}});
  host_.set_ops_combined_sources(
      {{"middle:op", "mixed"}, {"beta:op", "/plugins/beta"}});

  const Json first = route("plugins.ops_sources", Json{{"limit", 1}});
  ASSERT_TRUE(first.contains("result")) << first.dump();
  ASSERT_EQ(first["result"]["sources"].size(), 1U);
  EXPECT_EQ(first["result"]["sources"][0],
            Json({{"key", "alpha:op"}, {"source", "built-in"}}));
  ASSERT_TRUE(first["result"]["cursor"].is_string());

  const Json mismatched = route(
      "plugins.ops_combined_sources",
      Json{{"cursor", first["result"]["cursor"]}, {"offset", 1}, {"limit", 1}});
  ASSERT_TRUE(mismatched.contains("error")) << mismatched.dump();
  EXPECT_EQ(mismatched["error"]["domain"], "daemon");
  EXPECT_EQ(mismatched["error"]["name"], "cursor_not_found");
  EXPECT_EQ(host_.call_count("plugins.ops_combined_sources"), 0U);

  const Json final = route(
      "plugins.ops_sources",
      Json{{"cursor", first["result"]["cursor"]}, {"offset", 1}, {"limit", 1}});
  ASSERT_TRUE(final.contains("result")) << final.dump();
  EXPECT_EQ(
      final["result"]["sources"],
      Json::array({Json{{"key", "zeta:op"}, {"source", "/plugins/zeta"}}}));
  EXPECT_EQ(host_.call_count("plugins.ops_sources"), 1U);

  const Json combined =
      route("plugins.ops_combined_sources", Json{{"limit", 4}});
  ASSERT_TRUE(combined.contains("result")) << combined.dump();
  EXPECT_EQ(combined["result"]["sources"],
            Json::array({Json{{"key", "beta:op"}, {"source", "/plugins/beta"}},
                         Json{{"key", "middle:op"}, {"source", "mixed"}}}));
  EXPECT_EQ(host_.call_count("plugins.ops_combined_sources"), 1U);
}

TEST_F(StableInspectionPagingProtocolTest,
       PluginViewsReserveBeforeHostAndEnforceSnapshotBounds) {
  host_.set_ops_combined_keys({"a", "b"});
  const Json held = route("plugins.ops_combined_keys", Json{{"limit", 1}});
  ASSERT_TRUE(held.contains("result")) << held.dump();
  ASSERT_TRUE(held["result"]["cursor"].is_string());

  host_.set_ops_sources({{"source:key", "built-in"}});
  Json response = route("plugins.ops_sources", Json{{"limit", 1}});
  ASSERT_TRUE(response.contains("error")) << response.dump();
  EXPECT_EQ(response["error"]["name"], "capacity_exceeded");
  EXPECT_EQ(host_.call_count("plugins.ops_sources"), 0U);

  clock_.advance(std::chrono::seconds(2));
  response = route(
      "plugins.ops_combined_keys",
      Json{{"cursor", held["result"]["cursor"]}, {"offset", 1}, {"limit", 1}});
  ASSERT_TRUE(response.contains("error")) << response.dump();
  EXPECT_EQ(response["error"]["name"], "cursor_not_found");

  std::vector<std::string> oversized;
  for (std::size_t index = 0;
       index < small_protocol_snapshot_limits().snapshot_entries + 1; ++index) {
    oversized.push_back("key:" + std::to_string(index));
  }
  host_.set_ops_combined_keys(std::move(oversized));
  host_.reset_invocations();
  response = route("plugins.ops_combined_keys", Json{{"limit", 4}});
  ASSERT_TRUE(response.contains("error")) << response.dump();
  EXPECT_EQ(response["error"]["domain"], "protocol");
  EXPECT_EQ(response["error"]["name"], "response_too_large");
  EXPECT_EQ(host_.call_count("plugins.ops_combined_keys"), 1U);
}

TEST_F(StableInspectionPagingProtocolTest,
       PluginViewsRejectMalformedControlsAndReturnedTextWithoutMutation) {
  const std::vector<Json> malformed = {
      Json{{"limit", 0}},
      Json{{"limit", kGeneralPageMaxEntries + 1}},
      Json{{"offset", 0}, {"limit", 1}},
      Json{{"cursor", "malformed"}, {"offset", 0}, {"limit", 1}},
  };
  for (const Json& params : malformed) {
    const Json response = route("plugins.ops_sources", params);
    ASSERT_TRUE(response.contains("error")) << response.dump();
    EXPECT_EQ(response["error"]["name"], "invalid_params");
  }
  EXPECT_EQ(host_.call_count("plugins.ops_sources"), 0U);

  host_.set_ops_sources({{"", "built-in"}});
  Json response = route("plugins.ops_sources", Json{{"limit", 1}});
  ASSERT_TRUE(response.contains("error")) << response.dump();
  EXPECT_EQ(response["error"]["domain"], "daemon");
  EXPECT_EQ(response["error"]["name"], "internal_error");
  EXPECT_EQ(host_.call_count("plugins.ops_sources"), 1U);

  host_.reset_invocations();
  host_.set_ops_sources(
      {{"valid:key", std::string(kLargeTextMaxBytes + 1, 's')}});
  response = route("plugins.ops_sources", Json{{"limit", 1}});
  ASSERT_TRUE(response.contains("error")) << response.dump();
  EXPECT_EQ(response["error"]["domain"], "protocol");
  EXPECT_EQ(response["error"]["name"], "response_too_large");
  EXPECT_EQ(host_.call_count("plugins.ops_sources"), 1U);
}

TEST_F(StableInspectionPagingProtocolTest,
       PolicyListsSortFreezeAndBindPagesWithoutSecondHostCall) {
  host_.set_policy_types({"zeta", "alpha", "middle"});
  Json first =
      route("policy.types", Json{{"limit", 2}, {"session_id", "ignored"}});
  ASSERT_TRUE(first.contains("result")) << first.dump();
  EXPECT_EQ(first["result"]["types"], Json::array({"alpha", "middle"}));
  ASSERT_TRUE(first["result"]["cursor"].is_string());
  EXPECT_TRUE(first["result"]["has_more"].get<bool>());
  EXPECT_EQ(first["result"]["offset"], 0);
  EXPECT_EQ(host_.call_count("policy.types"), 1U);

  host_.set_policy_types({"changed-after-publication"});
  Json response = route(
      "policy.loaded_plugins",
      Json{{"cursor", first["result"]["cursor"]}, {"offset", 2}, {"limit", 1}});
  ASSERT_TRUE(response.contains("error")) << response.dump();
  EXPECT_EQ(response["error"]["name"], "cursor_not_found");
  EXPECT_EQ(host_.call_count("policy.loaded_plugins"), 0U);

  response = route(
      "policy.types",
      Json{{"cursor", first["result"]["cursor"]}, {"offset", 1}, {"limit", 1}});
  ASSERT_TRUE(response.contains("error")) << response.dump();
  EXPECT_EQ(response["error"]["name"], "cursor_not_found");
  EXPECT_EQ(host_.call_count("policy.types"), 1U);

  response = route("policy.types", Json{{"cursor", first["result"]["cursor"]},
                                        {"offset", 2},
                                        {"limit", 2},
                                        {"future", true}});
  ASSERT_TRUE(response.contains("result")) << response.dump();
  EXPECT_EQ(response["result"]["types"], Json::array({"zeta"}));
  EXPECT_FALSE(response["result"]["has_more"].get<bool>());
  EXPECT_TRUE(response["result"]["cursor"].is_null());
  EXPECT_EQ(response["result"]["offset"], 2);
  EXPECT_EQ(host_.call_count("policy.types"), 1U);

  host_.set_policy_loaded_plugins(
      {"/plugins/zeta.dylib", "/plugins/alpha.dylib"});
  response = route("policy.loaded_plugins", Json{{"limit", 8}});
  ASSERT_TRUE(response.contains("result")) << response.dump();
  EXPECT_EQ(response["result"]["plugins"],
            Json::array({"/plugins/alpha.dylib", "/plugins/zeta.dylib"}));
  EXPECT_TRUE(response["result"]["cursor"].is_null());
  EXPECT_FALSE(response["result"]["has_more"].get<bool>());
  EXPECT_EQ(host_.call_count("policy.loaded_plugins"), 1U);

  host_.reset_invocations();
  host_.set_execution_types({"serial_debug", "cpu", "gpu_pipeline"});
  response = route("execution.types", Json{{"limit", 3}});
  ASSERT_TRUE(response.contains("result")) << response.dump();
  EXPECT_EQ(response["result"]["types"],
            Json::array({"cpu", "gpu_pipeline", "serial_debug"}));
  EXPECT_FALSE(response["result"]["has_more"].get<bool>());
  EXPECT_EQ(host_.call_count("execution.types"), 1U);
}

TEST_F(StableInspectionPagingProtocolTest,
       PolicyListsReserveBeforeHostAndEnforceQuotaExpiryAndSize) {
  host_.set_policy_types({"alpha", "beta"});
  const Json held = route("policy.types", Json{{"limit", 1}});
  ASSERT_TRUE(held.contains("result")) << held.dump();
  ASSERT_TRUE(held["result"]["cursor"].is_string());

  host_.set_policy_loaded_plugins({"/plugins/one.dylib"});
  Json response = route("policy.loaded_plugins", Json{{"limit", 1}});
  ASSERT_TRUE(response.contains("error")) << response.dump();
  EXPECT_EQ(response["error"]["domain"], "daemon");
  EXPECT_EQ(response["error"]["name"], "capacity_exceeded");
  EXPECT_EQ(host_.call_count("policy.loaded_plugins"), 0U);

  clock_.advance(std::chrono::seconds(2));
  response = route(
      "policy.types",
      Json{{"cursor", held["result"]["cursor"]}, {"offset", 1}, {"limit", 1}});
  ASSERT_TRUE(response.contains("error")) << response.dump();
  EXPECT_EQ(response["error"]["name"], "cursor_not_found");
  EXPECT_EQ(host_.call_count("policy.types"), 1U);

  std::vector<std::string> oversized(
      small_protocol_snapshot_limits().snapshot_entries + 1,
      "/plugins/repeated.dylib");
  host_.set_policy_loaded_plugins(std::move(oversized));
  host_.reset_invocations();
  response = route("policy.loaded_plugins", Json{{"limit", 4}});
  ASSERT_TRUE(response.contains("error")) << response.dump();
  EXPECT_EQ(response["error"]["domain"], "protocol");
  EXPECT_EQ(response["error"]["name"], "response_too_large");
  EXPECT_EQ(host_.call_count("policy.loaded_plugins"), 1U);
}

TEST_F(StableInspectionPagingProtocolTest,
       PolicyListsRejectMalformedControlsAndReturnedLabelsWithoutRetry) {
  const std::vector<Json> malformed = {
      Json{{"limit", 0}},
      Json{{"limit", kGeneralPageMaxEntries + 1}},
      Json{{"offset", 0}, {"limit", 1}},
      Json{{"cursor", "malformed"}, {"offset", 0}, {"limit", 1}},
  };
  for (const Json& params : malformed) {
    const Json response = route("policy.types", params);
    ASSERT_TRUE(response.contains("error")) << response.dump();
    EXPECT_EQ(response["error"]["name"], "invalid_params");
  }
  EXPECT_EQ(host_.call_count("policy.types"), 0U);

  host_.set_policy_types({""});
  Json response = route("policy.types", Json{{"limit", 1}});
  ASSERT_TRUE(response.contains("error")) << response.dump();
  EXPECT_EQ(response["error"]["domain"], "daemon");
  EXPECT_EQ(response["error"]["name"], "internal_error");
  EXPECT_EQ(host_.call_count("policy.types"), 1U);

  host_.reset_invocations();
  host_.set_policy_types({std::string(kPolicyTypeMaxBytes + 1, 't')});
  response = route("policy.types", Json{{"limit", 1}});
  ASSERT_TRUE(response.contains("error")) << response.dump();
  EXPECT_EQ(response["error"]["domain"], "daemon");
  EXPECT_EQ(response["error"]["name"], "internal_error");
  EXPECT_EQ(host_.call_count("policy.types"), 1U);

  host_.reset_invocations();
  host_.set_policy_types({"duplicate", "duplicate"});
  response = route("policy.types", Json{{"limit", 2}});
  ASSERT_TRUE(response.contains("error")) << response.dump();
  EXPECT_EQ(response["error"]["domain"], "daemon");
  EXPECT_EQ(response["error"]["name"], "internal_error");
  EXPECT_EQ(host_.call_count("policy.types"), 1U);

  host_.reset_invocations();
  host_.set_execution_types({"cpu", "unknown"});
  response = route("execution.types", Json{{"limit", 2}});
  ASSERT_TRUE(response.contains("error")) << response.dump();
  EXPECT_EQ(response["error"]["domain"], "daemon");
  EXPECT_EQ(response["error"]["name"], "internal_error");
  EXPECT_EQ(host_.call_count("execution.types"), 1U);

  host_.reset_invocations();
  host_.set_policy_loaded_plugins({std::string("invalid\xc3\x28", 9)});
  response = route("policy.loaded_plugins", Json{{"limit", 1}});
  ASSERT_TRUE(response.contains("error")) << response.dump();
  EXPECT_EQ(response["error"]["domain"], "daemon");
  EXPECT_EQ(response["error"]["name"], "internal_error");
  EXPECT_EQ(host_.call_count("policy.loaded_plugins"), 1U);

  host_.reset_invocations();
  host_.set_policy_loaded_plugins({std::string(kPathTextMaxBytes + 1, 'p')});
  response = route("policy.loaded_plugins", Json{{"limit", 1}});
  ASSERT_TRUE(response.contains("error")) << response.dump();
  EXPECT_EQ(response["error"]["domain"], "protocol");
  EXPECT_EQ(response["error"]["name"], "response_too_large");
  EXPECT_EQ(host_.call_count("policy.loaded_plugins"), 1U);
}

TEST(ProtocolOperationPlugins,
     RealFixtureRemainsProcessOwnedAcrossHostAndRouterLifetimes) {
  ScopedOperationPluginCleanup cleanup;
  const std::filesystem::path plugin_dir = lifecycle_operation_plugin_dir();
  ASSERT_TRUE(std::filesystem::exists(plugin_dir))
      << "lifecycle operation plugin directory was not built: " << plugin_dir;

  {
    ScopedTempDirectory runtime_directory{"ps-plugin-one"};
    std::unique_ptr<Host> loader = create_embedded_host();
    ASSERT_NE(loader, nullptr);
    RequestRouter router(*loader, "plugin-lifecycle-loader");
    ScopedRequestRouterRuntime runtime(router, runtime_directory);
    const Json loaded = route_test_request(
        router, "plugins.load_report",
        Json{{"directories", Json::array({plugin_dir.string()})}},
        "plugin-load");
    ASSERT_TRUE(loaded.contains("result")) << loaded.dump();
    EXPECT_EQ(loaded["result"]["attempted"], 1);
    EXPECT_EQ(loaded["result"]["loaded"], 1);
    EXPECT_TRUE(loaded["result"]["errors"].empty());
    EXPECT_NE(std::find(loaded["result"]["new_op_keys"].begin(),
                        loaded["result"]["new_op_keys"].end(),
                        Json("plugin_lifecycle:op")),
              loaded["result"]["new_op_keys"].end());
  }

  {
    ScopedTempDirectory runtime_directory{"ps-plugin-two"};
    std::unique_ptr<Host> observer = create_embedded_host();
    ASSERT_NE(observer, nullptr);
    RequestRouter router(*observer, "plugin-lifecycle-observer");
    ScopedRequestRouterRuntime runtime(router, runtime_directory);
    const Json sources =
        route_test_request(router, "plugins.ops_sources", Json{{"limit", 4096}},
                           "plugin-sources-after-host-destruction");
    ASSERT_TRUE(sources.contains("result")) << sources.dump();
    const auto found =
        std::find_if(sources["result"]["sources"].begin(),
                     sources["result"]["sources"].end(), [](const Json& row) {
                       return row.value("key", "") == "plugin_lifecycle:op";
                     });
    ASSERT_NE(found, sources["result"]["sources"].end());
    EXPECT_EQ((*found)["source"],
              std::filesystem::absolute(plugin_dir).string() + "/" +
                  std::filesystem::path((*found)["source"].get<std::string>())
                      .filename()
                      .string());

    const Json seeded_once = route_test_request(router, "plugins.seed_builtins",
                                                Json::object(), "seed-once");
    const Json seeded_twice = route_test_request(
        router, "plugins.seed_builtins", Json::object(), "seed-twice");
    ASSERT_TRUE(seeded_once.contains("result")) << seeded_once.dump();
    ASSERT_TRUE(seeded_twice.contains("result")) << seeded_twice.dump();

    const Json unloaded = route_test_request(router, "plugins.unload_all",
                                             Json::object(), "plugin-unload");
    ASSERT_TRUE(unloaded.contains("result")) << unloaded.dump();
    EXPECT_GE(unloaded["result"]["unloaded"].get<int>(), 1);
    const Json after_unload =
        route_test_request(router, "plugins.ops_sources", Json{{"limit", 4096}},
                           "plugin-sources-after-unload");
    ASSERT_TRUE(after_unload.contains("result")) << after_unload.dump();
    EXPECT_EQ(std::count_if(after_unload["result"]["sources"].begin(),
                            after_unload["result"]["sources"].end(),
                            [](const Json& row) {
                              return row.value("key", "") ==
                                     "plugin_lifecycle:op";
                            }),
              0);
  }

  {
    ScopedTempDirectory runtime_directory{"ps-plugin-three"};
    std::unique_ptr<Host> fresh = create_embedded_host();
    ASSERT_NE(fresh, nullptr);
    RequestRouter router(*fresh, "plugin-lifecycle-fresh");
    ScopedRequestRouterRuntime runtime(router, runtime_directory);
    const Json sources =
        route_test_request(router, "plugins.ops_sources", Json{{"limit", 4096}},
                           "plugin-sources-fresh-host");
    ASSERT_TRUE(sources.contains("result")) << sources.dump();
    EXPECT_EQ(std::count_if(sources["result"]["sources"].begin(),
                            sources["result"]["sources"].end(),
                            [](const Json& row) {
                              return row.value("key", "") ==
                                     "plugin_lifecycle:op";
                            }),
              0);
  }
}

TEST_F(HostRoutedGraphStateProtocolTest,
       RoutesRemainingInspectionFamiliesWithCopiedPublicValues) {
  NodeInspectionView node;
  node.id = NodeId{7};
  node.name = "copied-node";
  node.type = "fixture";
  node.subtype = "source";
  node.parameters = {{"mode", "exact"}};
  node.source_label = std::nullopt;
  node.debug = DebugMetadataSnapshot{};
  node.debug->min_val = std::numeric_limits<double>::quiet_NaN();
  node.debug->max_val = std::numeric_limits<double>::infinity();

  GraphInspectionView graph;
  graph.nodes = {node, node};
  graph.nodes.back().id = NodeId{8};
  host_.set_inspected_graph(graph);
  host_.set_inspected_node(node);

  HostDependencyTreeSnapshot tree;
  tree.scope = HostDependencyTreeScope::StartNode;
  tree.start_node = NodeId{7};
  tree.root_nodes = {NodeId{7}};
  tree.entries.push_back(HostDependencyTreeEntry{0, std::nullopt, node, false});
  host_.set_dependency_tree(tree);
  host_.set_traversal_orders(
      {{7, {NodeId{1}, NodeId{7}}}, {8, {NodeId{2}, NodeId{8}}}});
  host_.set_traversal_details(
      {{7,
        {HostTraversalNodeSnapshot{NodeId{1}, "source", true, false},
         HostTraversalNodeSnapshot{NodeId{7}, "ending", false, true}}}});
  host_.set_trees_containing_node({NodeId{7}, NodeId{8}});
  host_.set_dirty_region(make_protocol_dirty_snapshot());
  const ComputePlanningInspectionSnapshot planning =
      make_protocol_planning_snapshot();
  host_.set_compute_planning(planning);
  ComputePlanningInspectionSnapshot scalar_only_planning = planning;
  scalar_only_planning.planned_node_sample.clear();
  scalar_only_planning.task_sample.clear();
  host_.set_recent_compute_planning({planning, scalar_only_planning});

  host_.reset_invocations();
  Json response = route("inspect.graph", Json{{"session_id", session_id_}});
  ASSERT_TRUE(response.contains("result")) << response.dump();
  ASSERT_EQ(response["result"]["nodes"].size(), 2u);
  EXPECT_TRUE(response["result"]["cursor"].is_null());
  EXPECT_FALSE(response["result"]["has_more"].get<bool>());
  EXPECT_TRUE(response["result"]["nodes"][0]["source_label"].is_null());
  EXPECT_TRUE(response["result"]["nodes"][0]["debug"]["min_val"].is_null());
  EXPECT_TRUE(response["result"]["nodes"][0]["debug"]["max_val"].is_null());
  EXPECT_EQ(host_.call_count("inspect.graph"), 1u);

  host_.reset_invocations();
  response = route("inspect.dependency_tree", Json{{"session_id", session_id_},
                                                   {"node_id", 7},
                                                   {"include_metadata", true}});
  ASSERT_TRUE(response.contains("result")) << response.dump();
  EXPECT_EQ(response["result"]["scope"], "start_node");
  EXPECT_EQ(response["result"]["start_node_id"], 7);
  ASSERT_EQ(response["result"]["entries"].size(), 1u);
  EXPECT_TRUE(response["result"]["entries"][0]["incoming_edge"].is_null());
  ASSERT_EQ(host_.call_count("inspect.dependency_tree"), 1u);
  ASSERT_EQ(host_.invocations().size(), 1u);
  EXPECT_EQ(host_.invocations().front().optional_node->value, 7);
  EXPECT_TRUE(host_.invocations().front().flag);

  host_.reset_invocations();
  response =
      route("inspect.traversal_orders", Json{{"session_id", session_id_}});
  ASSERT_TRUE(response.contains("result")) << response.dump();
  ASSERT_EQ(response["result"]["orders"].size(), 2u);
  EXPECT_EQ(response["result"]["orders"][0]["ending_node_id"], 7);
  EXPECT_EQ(response["result"]["orders"][1]["node_ids"], Json::array({2, 8}));
  EXPECT_EQ(host_.call_count("inspect.traversal_orders"), 1u);

  host_.reset_invocations();
  response =
      route("inspect.traversal_details", Json{{"session_id", session_id_}});
  ASSERT_TRUE(response.contains("result")) << response.dump();
  ASSERT_EQ(response["result"]["branches"].size(), 1u);
  EXPECT_TRUE(response["result"]["branches"][0]["nodes"][0]["has_memory_cache"]
                  .get<bool>());
  EXPECT_TRUE(response["result"]["branches"][0]["nodes"][1]["has_disk_cache"]
                  .get<bool>());
  EXPECT_EQ(host_.call_count("inspect.traversal_details"), 1u);

  host_.reset_invocations();
  response = route("inspect.trees_containing_node",
                   Json{{"session_id", session_id_}, {"node_id", 7}});
  ASSERT_TRUE(response.contains("result")) << response.dump();
  EXPECT_EQ(response["result"]["ending_node_ids"], Json::array({7, 8}));
  ASSERT_EQ(host_.call_count("inspect.trees_containing_node"), 1u);
  EXPECT_EQ(host_.invocations().front().first_node.value, 7);

  host_.reset_invocations();
  response = route("inspect.dirty_region", Json{{"session_id", session_id_}});
  ASSERT_TRUE(response.contains("result")) << response.dump();
  EXPECT_EQ(response["result"], expected_protocol_dirty_snapshot(session_id_));
  EXPECT_EQ(host_.call_count("inspect.dirty_region"), 1u);

  host_.reset_invocations();
  response =
      route("inspect.compute_planning", Json{{"session_id", session_id_}});
  ASSERT_TRUE(response.contains("result")) << response.dump();
  EXPECT_EQ(response["result"]["planning"]["intent"], "real_time_update");
  EXPECT_EQ(response["result"]["planning"]["task_sample"][0]["domain"],
            "real_time");
  EXPECT_EQ(response["result"]["planning"]["task_sample"][0]["output_roi"],
            expected_rect(PixelRect{1, 2, 3, 4}));
  EXPECT_EQ(host_.call_count("inspect.compute_planning"), 1u);

  host_.set_compute_planning(std::nullopt);
  host_.reset_invocations();
  response =
      route("inspect.compute_planning", Json{{"session_id", session_id_}});
  ASSERT_TRUE(response.contains("result")) << response.dump();
  EXPECT_TRUE(response["result"]["planning"].is_null());
  EXPECT_EQ(host_.call_count("inspect.compute_planning"), 1u);

  host_.reset_invocations();
  response = route("inspect.recent_compute_planning",
                   Json{{"session_id", session_id_}});
  ASSERT_TRUE(response.contains("result")) << response.dump();
  ASSERT_EQ(response["result"]["snapshots"].size(), 2u);
  EXPECT_TRUE(response["result"]["cursor"].is_null());
  EXPECT_EQ(host_.call_count("inspect.recent_compute_planning"), 1u);
}

TEST_F(StableInspectionPagingProtocolTest,
       AggregatesStablePagesWithoutLossDuplicationOrRepeatedHostCalls) {
  host_.set_node_ids({NodeId{3}, NodeId{1}, NodeId{3}, NodeId{5}, NodeId{8}});
  Json params{{"session_id", session_id_}, {"limit", 2}};
  Json response = route("inspect.node_ids", params);
  ASSERT_TRUE(response.contains("result")) << response.dump();
  std::vector<int> aggregate;
  std::string cursor;
  std::size_t offset = 0;
  while (true) {
    const Json& result = response["result"];
    EXPECT_EQ(result["offset"], offset);
    for (const Json& node : result["node_ids"]) {
      aggregate.push_back(node.get<int>());
    }
    offset += result["node_ids"].size();
    if (!result["has_more"].get<bool>()) {
      EXPECT_TRUE(result["cursor"].is_null());
      break;
    }
    ASSERT_TRUE(result["cursor"].is_string());
    if (cursor.empty()) {
      cursor = result["cursor"].get<std::string>();
    } else {
      EXPECT_EQ(result["cursor"], cursor);
    }
    response = route("inspect.node_ids",
                     Json{{"session_id", session_id_},
                          {"cursor", cursor},
                          {"offset", offset},
                          {"limit", 2}},
                     std::string(kRequestTextMaxBytes, '\x01'));
    ASSERT_TRUE(response.contains("result")) << response.dump();
  }
  EXPECT_EQ(aggregate, (std::vector<int>{3, 1, 3, 5, 8}));
  EXPECT_EQ(host_.call_count("inspect.node_ids"), 1u);

  const Json released =
      route("inspect.node_ids", Json{{"session_id", session_id_},
                                     {"cursor", cursor},
                                     {"offset", offset},
                                     {"limit", 1}});
  ASSERT_TRUE(released.contains("error")) << released.dump();
  EXPECT_EQ(released["error"]["name"], "cursor_not_found");

  host_.set_traversal_orders({{2, {NodeId{1}, NodeId{2}}},
                              {4, {NodeId{3}, NodeId{4}}},
                              {6, {NodeId{5}, NodeId{6}}}});
  host_.reset_invocations();
  const Json branches = route("inspect.traversal_orders",
                              Json{{"session_id", session_id_}, {"limit", 2}});
  ASSERT_TRUE(branches.contains("result")) << branches.dump();
  ASSERT_EQ(branches["result"]["orders"].size(), 2u);
  const std::string branch_cursor =
      branches["result"]["cursor"].get<std::string>();
  const Json branch_final =
      route("inspect.traversal_orders", Json{{"session_id", session_id_},
                                             {"cursor", branch_cursor},
                                             {"offset", 2},
                                             {"limit", 2}});
  ASSERT_TRUE(branch_final.contains("result")) << branch_final.dump();
  ASSERT_EQ(branch_final["result"]["orders"].size(), 1u);
  EXPECT_EQ(branch_final["result"]["orders"][0]["ending_node_id"], 6);
  EXPECT_EQ(branch_final["result"]["orders"][0]["node_ids"],
            Json::array({5, 6}));
  EXPECT_EQ(host_.call_count("inspect.traversal_orders"), 1u);
}

TEST_F(StableInspectionPagingProtocolTest,
       RejectsMalformedMismatchedAndExpiredCursorsWithoutHostAccess) {
  host_.set_trees_containing_node({NodeId{1}, NodeId{2}, NodeId{3}});
  const Json first =
      route("inspect.trees_containing_node",
            Json{{"session_id", session_id_}, {"node_id", 7}, {"limit", 1}});
  ASSERT_TRUE(first.contains("result")) << first.dump();
  const std::string cursor = first["result"]["cursor"].get<std::string>();

  Json response = route("inspect.trees_containing_node",
                        Json{{"session_id", session_id_},
                             {"node_id", 7},
                             {"cursor", std::string(32, 'A')},
                             {"offset", 1},
                             {"limit", 1}});
  ASSERT_TRUE(response.contains("error")) << response.dump();
  EXPECT_EQ(response["error"]["name"], "invalid_params");

  response =
      route("inspect.trees_containing_node", Json{{"session_id", session_id_},
                                                  {"node_id", 8},
                                                  {"cursor", cursor},
                                                  {"offset", 1},
                                                  {"limit", 1}});
  ASSERT_TRUE(response.contains("error")) << response.dump();
  EXPECT_EQ(response["error"]["name"], "cursor_not_found");

  response = route("inspect.ending_nodes", Json{{"session_id", session_id_},
                                                {"cursor", cursor},
                                                {"offset", 1},
                                                {"limit", 1}});
  ASSERT_TRUE(response.contains("error")) << response.dump();
  EXPECT_EQ(response["error"]["name"], "cursor_not_found");

  response = route("inspect.trees_containing_node",
                   Json{{"session_id", std::string(32, 'e')},
                        {"node_id", 7},
                        {"cursor", cursor},
                        {"offset", 1},
                        {"limit", 1}});
  ASSERT_TRUE(response.contains("error")) << response.dump();
  EXPECT_EQ(response["error"]["name"], "cursor_not_found");

  response =
      route("inspect.trees_containing_node", Json{{"session_id", session_id_},
                                                  {"node_id", 7},
                                                  {"cursor", cursor},
                                                  {"offset", 2},
                                                  {"limit", 1}});
  ASSERT_TRUE(response.contains("error")) << response.dump();
  EXPECT_EQ(response["error"]["name"], "cursor_not_found");

  clock_.advance(std::chrono::seconds(2));
  response =
      route("inspect.trees_containing_node", Json{{"session_id", session_id_},
                                                  {"node_id", 7},
                                                  {"cursor", cursor},
                                                  {"offset", 1},
                                                  {"limit", 1}});
  ASSERT_TRUE(response.contains("error")) << response.dump();
  EXPECT_EQ(response["error"]["name"], "cursor_not_found");
  EXPECT_EQ(host_.call_count("inspect.trees_containing_node"), 1u);
  EXPECT_EQ(host_.call_count("inspect.ending_nodes"), 0u);
}

TEST_F(StableInspectionPagingProtocolTest,
       RejectsMalformedKnownInspectionParamsBeforeAnyHostAccess) {
  const std::vector<std::pair<std::string, Json>> malformed = {
      {"inspect.traversal_orders", Json{{"session_id", std::string(31, 'a')}}},
      {"inspect.trees_containing_node", Json{{"session_id", session_id_}}},
      {"inspect.trees_containing_node",
       Json{{"session_id", session_id_}, {"node_id", 1.5}}},
      {"inspect.dependency_tree",
       Json{{"session_id", session_id_}, {"include_metadata", "yes"}}},
      {"inspect.node_ids", Json{{"session_id", session_id_}, {"limit", 0}}},
      {"inspect.node_ids", Json{{"session_id", session_id_}, {"limit", 1.5}}},
      {"inspect.node_ids",
       Json{{"session_id", session_id_}, {"offset", 0}, {"limit", 1}}},
      {"inspect.node_ids", Json{{"session_id", session_id_},
                                {"cursor", std::string(32, 'a')},
                                {"limit", 1}}},
      {"inspect.recent_compute_planning",
       Json{{"session_id", session_id_},
            {"cursor", std::string(32, 'a')},
            {"offset", std::numeric_limits<std::uint64_t>::max()},
            {"limit", 1}}},
  };
  for (const auto& test_case : malformed) {
    const Json response = route(test_case.first, test_case.second);
    ASSERT_TRUE(response.contains("error"))
        << test_case.first << response.dump();
    EXPECT_EQ(response["error"]["domain"], "protocol") << test_case.first;
    EXPECT_EQ(response["error"]["name"], "invalid_params") << test_case.first;
  }
  EXPECT_TRUE(host_.invocations().empty());
}

TEST_F(StableInspectionPagingProtocolTest,
       PublishedSnapshotSurvivesGraphCloseUntilItsFinalPage) {
  host_.set_node_ids({NodeId{4}, NodeId{5}, NodeId{6}});
  const Json first = route("inspect.node_ids",
                           Json{{"session_id", session_id_}, {"limit", 1}});
  ASSERT_TRUE(first.contains("result")) << first.dump();
  const std::string cursor = first["result"]["cursor"].get<std::string>();

  const Json closed = route("graph.close", Json{{"session_id", session_id_}});
  ASSERT_TRUE(closed.contains("result")) << closed.dump();

  const Json second =
      route("inspect.node_ids", Json{{"session_id", session_id_},
                                     {"cursor", cursor},
                                     {"offset", 1},
                                     {"limit", 1}});
  ASSERT_TRUE(second.contains("result")) << second.dump();
  EXPECT_EQ(second["result"]["node_ids"], Json::array({5}));
  ASSERT_TRUE(second["result"]["has_more"].get<bool>());

  const Json final = route("inspect.node_ids", Json{{"session_id", session_id_},
                                                    {"cursor", cursor},
                                                    {"offset", 2},
                                                    {"limit", 1}});
  ASSERT_TRUE(final.contains("result")) << final.dump();
  EXPECT_EQ(final["result"]["node_ids"], Json::array({6}));
  EXPECT_FALSE(final["result"]["has_more"].get<bool>());
  EXPECT_EQ(host_.call_count("inspect.node_ids"), 1u);
  EXPECT_EQ(host_.call_count("graph.close"), 1u);
}

TEST_F(StableInspectionPagingProtocolTest,
       RejectsCapacityExhaustionBeforeAnySecondHostCall) {
  host_.set_node_ids({NodeId{1}, NodeId{2}, NodeId{3}});
  const Json held = route("inspect.node_ids",
                          Json{{"session_id", session_id_}, {"limit", 1}});
  ASSERT_TRUE(held.contains("result")) << held.dump();
  ASSERT_TRUE(held["result"]["has_more"].get<bool>());

  host_.set_ending_nodes({NodeId{9}});
  Json response =
      route("inspect.ending_nodes", Json{{"session_id", session_id_}});
  ASSERT_TRUE(response.contains("error")) << response.dump();
  EXPECT_EQ(response["error"]["name"], "capacity_exceeded");
  EXPECT_EQ(host_.call_count("inspect.ending_nodes"), 0u);
}

TEST_F(StableInspectionPagingProtocolTest,
       RejectsEntryAndByteOversizeWithoutPublishingCursorOrLeakingQuota) {
  host_.set_node_ids(std::vector<NodeId>(17, NodeId{7}));
  Json response = route("inspect.node_ids", Json{{"session_id", session_id_}});
  ASSERT_TRUE(response.contains("error")) << response.dump();
  EXPECT_EQ(response["error"]["name"], "response_too_large");
  EXPECT_EQ(host_.call_count("inspect.node_ids"), 1u);

  GraphInspectionView graph;
  NodeInspectionView node;
  node.id = NodeId{1};
  node.name = "large-public-node";
  node.type = "fixture";
  node.subtype = "source";
  node.source_label = std::string(8100, 's');
  graph.nodes.push_back(std::move(node));
  host_.set_inspected_graph(std::move(graph));
  response = route("inspect.graph", Json{{"session_id", session_id_}});
  ASSERT_TRUE(response.contains("error")) << response.dump();
  EXPECT_EQ(response["error"]["name"], "response_too_large");
  EXPECT_EQ(host_.call_count("inspect.graph"), 1u);

  host_.set_ending_nodes({NodeId{9}});
  response = route("inspect.ending_nodes", Json{{"session_id", session_id_}});
  ASSERT_TRUE(response.contains("result")) << response.dump();
  EXPECT_EQ(response["result"]["ending_node_ids"], Json::array({9}));
  EXPECT_TRUE(response["result"]["cursor"].is_null());
  EXPECT_EQ(host_.call_count("inspect.ending_nodes"), 1u);
}

TEST_F(StableInspectionPagingProtocolTest,
       DependencyByteAccountingAcceptsExactLimitAndRejectsLimitPlusOne) {
  HostDependencyTreeSnapshot tree;
  tree.root_nodes = {NodeId{1}};
  HostDependencyTreeEntry entry;
  entry.node.id = NodeId{1};
  entry.node.name = "byte-boundary";
  entry.node.type = "fixture";
  entry.node.subtype = "source";
  entry.node.source_label = std::string();
  tree.entries.push_back(std::move(entry));

  const std::size_t base_bytes =
      encode_dependency_tree(IpcSessionId{session_id_}, tree).dump().size();
  ASSERT_LT(base_bytes, small_protocol_snapshot_limits().snapshot_bytes);
  tree.entries.front().node.source_label = std::string(
      small_protocol_snapshot_limits().snapshot_bytes - base_bytes, 'x');
  ASSERT_EQ(
      encode_dependency_tree(IpcSessionId{session_id_}, tree).dump().size(),
      small_protocol_snapshot_limits().snapshot_bytes);

  host_.set_dependency_tree(tree);
  Json response =
      route("inspect.dependency_tree", Json{{"session_id", session_id_}});
  ASSERT_TRUE(response.contains("result")) << response.dump();
  ASSERT_EQ(response["result"]["entries"].size(), 1u);
  EXPECT_EQ(host_.call_count("inspect.dependency_tree"), 1u);

  tree.entries.front().node.source_label->push_back('y');
  ASSERT_EQ(
      encode_dependency_tree(IpcSessionId{session_id_}, tree).dump().size(),
      small_protocol_snapshot_limits().snapshot_bytes + 1U);
  host_.set_dependency_tree(std::move(tree));
  host_.reset_invocations();
  response =
      route("inspect.dependency_tree", Json{{"session_id", session_id_}});
  ASSERT_TRUE(response.contains("error")) << response.dump();
  EXPECT_EQ(response["error"]["domain"], "protocol");
  EXPECT_EQ(response["error"]["name"], "response_too_large");
  EXPECT_EQ(host_.call_count("inspect.dependency_tree"), 1u);

  host_.set_ending_nodes({NodeId{9}});
  host_.reset_invocations();
  response = route("inspect.ending_nodes", Json{{"session_id", session_id_}});
  ASSERT_TRUE(response.contains("result")) << response.dump();
  EXPECT_EQ(response["result"]["ending_node_ids"], Json::array({9}));
  EXPECT_EQ(host_.call_count("inspect.ending_nodes"), 1u);
}

TEST_F(StableInspectionPagingProtocolTest,
       OversizedDependencyRootsRejectWithoutSnapshotQuotaLeak) {
  HostDependencyTreeSnapshot tree;
  tree.root_nodes.assign(kGeneralPageMaxEntries + 1U, NodeId{1});
  host_.set_dependency_tree(std::move(tree));

  Json response =
      route("inspect.dependency_tree", Json{{"session_id", session_id_}});
  ASSERT_TRUE(response.contains("error")) << response.dump();
  EXPECT_EQ(response["error"]["domain"], "protocol");
  EXPECT_EQ(response["error"]["name"], "response_too_large");
  EXPECT_EQ(host_.call_count("inspect.dependency_tree"), 1u);

  host_.set_ending_nodes({NodeId{11}});
  host_.reset_invocations();
  response = route("inspect.ending_nodes", Json{{"session_id", session_id_}});
  ASSERT_TRUE(response.contains("result")) << response.dump();
  EXPECT_EQ(response["result"]["ending_node_ids"], Json::array({11}));
  EXPECT_EQ(host_.call_count("inspect.ending_nodes"), 1u);
}

TEST_F(StableInspectionPagingProtocolTest,
       RecursiveEntryAccountingCoversEveryNestedInspectionFamily) {
  GraphInspectionView graph;
  NodeInspectionView node;
  node.id = NodeId{1};
  node.name = "parameter-count";
  node.type = "fixture";
  node.subtype = "source";
  for (int index = 0; index < 16; ++index) {
    node.parameters.emplace("key-" + std::to_string(index), "value");
  }
  graph.nodes = {node};
  host_.set_inspected_graph(std::move(graph));
  Json response = route("inspect.graph", Json{{"session_id", session_id_}});
  ASSERT_TRUE(response.contains("error")) << response.dump();
  EXPECT_EQ(response["error"]["name"], "response_too_large");
  EXPECT_EQ(host_.call_count("inspect.graph"), 1u);

  node.parameters.clear();
  node.space = SpatialSnapshot{};
  GraphInspectionView spatial_graph;
  spatial_graph.nodes = {node};
  host_.set_inspected_graph(std::move(spatial_graph));
  host_.reset_invocations();
  response = route("inspect.graph", Json{{"session_id", session_id_}});
  ASSERT_TRUE(response.contains("error")) << response.dump();
  EXPECT_EQ(response["error"]["name"], "response_too_large");
  EXPECT_EQ(host_.call_count("inspect.graph"), 1u);

  node.space.reset();
  for (int index = 0; index < 15; ++index) {
    node.parameters.emplace("dependency-" + std::to_string(index), "value");
  }
  HostDependencyTreeSnapshot tree;
  tree.root_nodes = {NodeId{1}};
  tree.entries.push_back(
      HostDependencyTreeEntry{0, std::nullopt, std::move(node), false});
  host_.set_dependency_tree(std::move(tree));
  host_.reset_invocations();
  response =
      route("inspect.dependency_tree", Json{{"session_id", session_id_}});
  ASSERT_TRUE(response.contains("error")) << response.dump();
  EXPECT_EQ(response["error"]["name"], "response_too_large");
  EXPECT_EQ(host_.call_count("inspect.dependency_tree"), 1u);

  host_.set_traversal_details(
      {{1,
        std::vector<HostTraversalNodeSnapshot>(
            16, HostTraversalNodeSnapshot{NodeId{1}, "node", false, false})}});
  host_.reset_invocations();
  response =
      route("inspect.traversal_details", Json{{"session_id", session_id_}});
  ASSERT_TRUE(response.contains("error")) << response.dump();
  EXPECT_EQ(response["error"]["name"], "response_too_large");
  EXPECT_EQ(host_.call_count("inspect.traversal_details"), 1u);

  ComputePlanningInspectionSnapshot planning;
  planning.target_node = NodeId{1};
  planning.planned_node_sample.assign(16, NodeId{1});
  host_.set_recent_compute_planning({planning});
  host_.reset_invocations();
  response = route("inspect.recent_compute_planning",
                   Json{{"session_id", session_id_}});
  ASSERT_TRUE(response.contains("error")) << response.dump();
  EXPECT_EQ(response["error"]["name"], "response_too_large");
  EXPECT_EQ(host_.call_count("inspect.recent_compute_planning"), 1u);

  host_.set_node_ids({NodeId{21}});
  host_.reset_invocations();
  response = route("inspect.node_ids", Json{{"session_id", session_id_}});
  ASSERT_TRUE(response.contains("result")) << response.dump();
  EXPECT_EQ(response["result"]["node_ids"], Json::array({21}));
  EXPECT_EQ(host_.call_count("inspect.node_ids"), 1u);
}

TEST_F(HostRoutedGraphStateProtocolTest,
       TraversalRecursiveEntryBoundaryIsInclusiveAndPreScanned) {
  static constexpr std::size_t kBranches = 64;
  static constexpr std::size_t kNodesAtExactLimit = 4095;
  static_assert(
      kBranches + kBranches * kNodesAtExactLimit == kSnapshotMaxEntries,
      "fixture must exercise the exact recursive entry limit");

  host_.set_traversal_orders(
      make_dense_traversal_orders(kBranches, kNodesAtExactLimit));
  Json response =
      route("inspect.traversal_orders", Json{{"session_id", session_id_}});
  ASSERT_TRUE(response.contains("result")) << response.dump();
  EXPECT_EQ(response["result"]["orders"].size(), kBranches);
  EXPECT_TRUE(response["result"]["cursor"].is_null());
  EXPECT_EQ(host_.call_count("inspect.traversal_orders"), 1u);

  host_.set_traversal_orders(
      make_dense_traversal_orders(kBranches, kNodesAtExactLimit, 1));
  host_.reset_invocations();
  response =
      route("inspect.traversal_orders", Json{{"session_id", session_id_}});
  ASSERT_TRUE(response.contains("error")) << response.dump();
  EXPECT_EQ(response["error"]["name"], "response_too_large");
  EXPECT_EQ(host_.call_count("inspect.traversal_orders"), 1u);

  host_.set_traversal_orders(make_dense_traversal_orders(kBranches, 4096));
  host_.reset_invocations();
  response =
      route("inspect.traversal_orders", Json{{"session_id", session_id_}});
  ASSERT_TRUE(response.contains("error")) << response.dump();
  EXPECT_EQ(response["error"]["name"], "response_too_large");
  EXPECT_EQ(host_.call_count("inspect.traversal_orders"), 1u);

  host_.set_node_ids({NodeId{17}});
  host_.reset_invocations();
  response = route("inspect.node_ids", Json{{"session_id", session_id_}});
  ASSERT_TRUE(response.contains("result")) << response.dump();
  EXPECT_EQ(response["result"]["node_ids"], Json::array({17}));
  EXPECT_EQ(host_.call_count("inspect.node_ids"), 1u);
}

TEST_F(HostRoutedGraphStateProtocolTest,
       DynamicGraphPagesKeepAggregateFramesBoundedAndStable) {
  static constexpr std::size_t kLargeLabelBytes = 6U * 1024U * 1024U;
  GraphInspectionView graph;
  for (int index = 0; index < 3; ++index) {
    NodeInspectionView node;
    node.id = NodeId{index + 1};
    node.name = "large-row-" + std::to_string(index);
    node.type = "fixture";
    node.subtype = "source";
    node.source_label =
        std::string(kLargeLabelBytes, static_cast<char>('a' + index));
    graph.nodes.push_back(std::move(node));
  }
  host_.set_inspected_graph(std::move(graph));
  host_.reset_invocations();

  const std::string worst_id(kRequestTextMaxBytes, '\x01');
  Json response = route(
      "inspect.graph",
      Json{{"session_id", session_id_}, {"limit", kGeneralPageMaxEntries}},
      worst_id);
  ASSERT_TRUE(response.contains("result")) << response.dump();
  ASSERT_TRUE(response["result"]["has_more"].get<bool>());
  ASSERT_EQ(response["result"]["nodes"].size(), 2u);

  std::vector<int> node_ids;
  std::vector<std::size_t> page_sizes;
  std::string cursor;
  std::size_t offset = 0;
  while (true) {
    ASSERT_TRUE(response.contains("result")) << response.dump();
    EXPECT_LE(response.dump().size(), kMaximumFramePayloadBytes);
    const Json& result = response["result"];
    EXPECT_EQ(result["offset"], offset);
    ASSERT_FALSE(result["nodes"].empty());
    page_sizes.push_back(result["nodes"].size());
    for (const Json& node : result["nodes"]) {
      node_ids.push_back(node["id"].get<int>());
      const std::string& label =
          node["source_label"].get_ref<const std::string&>();
      EXPECT_EQ(label.size(), kLargeLabelBytes);
      EXPECT_EQ(label.front(),
                static_cast<char>('a' + node["id"].get<int>() - 1));
    }
    offset += result["nodes"].size();
    if (!result["has_more"].get<bool>()) {
      EXPECT_TRUE(result["cursor"].is_null());
      break;
    }
    ASSERT_TRUE(result["cursor"].is_string());
    if (cursor.empty()) {
      cursor = result["cursor"].get<std::string>();
    } else {
      EXPECT_EQ(result["cursor"], cursor);
    }
    response = route("inspect.graph",
                     Json{{"session_id", session_id_},
                          {"cursor", cursor},
                          {"offset", offset},
                          {"limit", kGeneralPageMaxEntries}},
                     worst_id);
  }
  EXPECT_EQ(node_ids, (std::vector<int>{1, 2, 3}));
  EXPECT_EQ(page_sizes, (std::vector<std::size_t>{2, 1}));
  EXPECT_EQ(offset, 3U);
  EXPECT_EQ(host_.call_count("inspect.graph"), 1u);
}

TEST_F(HostRoutedGraphStateProtocolTest,
       DirectPlanningIgnoresStableSnapshotEntryQuotaWithinFrame) {
  static constexpr std::size_t kTaskCount = 65;
  static constexpr std::size_t kDependenciesPerTask = 4096;
  static_assert(
      kTaskCount + kTaskCount * kDependenciesPerTask > kSnapshotMaxEntries,
      "fixture must exceed the stable snapshot recursive quota");
  host_.set_compute_planning(
      make_dense_protocol_planning_snapshot(kTaskCount, kDependenciesPerTask));
  host_.reset_invocations();

  const Json response =
      route("inspect.compute_planning", Json{{"session_id", session_id_}});
  ASSERT_TRUE(response.contains("result")) << response.dump();
  ASSERT_TRUE(response["result"]["planning"].is_object());
  EXPECT_EQ(response["result"]["planning"]["task_sample"].size(), kTaskCount);
  EXPECT_LE(response.dump().size(), kMaximumFramePayloadBytes);
  EXPECT_EQ(host_.call_count("inspect.compute_planning"), 1u);
}

TEST_F(HostRoutedGraphStateProtocolTest,
       DirectNodePreflightAcceptsAnExactFrameBoundary) {
  static constexpr std::string_view kRequestId = "tight-node";
  NodeInspectionView node;
  node.id = NodeId{1};
  node.name = "boundary";
  node.type = "fixture";
  node.subtype = "source";
  node.parameters.emplace("payload", std::string(kLargeTextMaxBytes, 'p'));
  node.source_label = std::string();

  const auto payload_size = [&](const NodeInspectionView& value) {
    return Json{{"protocol_version", kProtocolVersion},
                {"id", kRequestId},
                {"result", Json{{"session_id", session_id_},
                                {"node", encode_node(value)}}}}
        .dump()
        .size();
  };
  const std::size_t base_size = payload_size(node);
  ASSERT_LT(base_size, kMaximumFramePayloadBytes);
  const std::size_t remaining = kMaximumFramePayloadBytes - base_size;
  ASSERT_LE(remaining, kLargeTextMaxBytes);
  node.source_label->assign(remaining, 's');
  ASSERT_EQ(payload_size(node), kMaximumFramePayloadBytes);
  host_.set_inspected_node(node);
  host_.reset_invocations();

  const Json response =
      route("inspect.node", Json{{"session_id", session_id_}, {"node_id", 1}},
            std::string(kRequestId));
  ASSERT_TRUE(response.contains("result")) << response.dump();
  EXPECT_EQ(response.dump().size(), kMaximumFramePayloadBytes);
  EXPECT_EQ(host_.call_count("inspect.node"), 1u);
}

TEST_F(HostRoutedGraphStateProtocolTest,
       DirectPreflightRejectsClearlyOversizedNodeAndPlanningValues) {
  NodeInspectionView node;
  node.id = NodeId{1};
  node.name = "oversized";
  node.type = "fixture";
  node.subtype = "source";
  node.parameters.emplace("payload", std::string(kLargeTextMaxBytes, 'p'));
  node.source_label = std::string(kLargeTextMaxBytes, 's');
  host_.set_inspected_node(std::move(node));
  host_.reset_invocations();

  Json response =
      route("inspect.node", Json{{"session_id", session_id_}, {"node_id", 1}});
  ASSERT_TRUE(response.contains("error")) << response.dump();
  EXPECT_EQ(response["error"]["domain"], "protocol");
  EXPECT_EQ(response["error"]["name"], "response_too_large");
  EXPECT_EQ(host_.call_count("inspect.node"), 1u);

  host_.set_compute_planning(make_dense_protocol_planning_snapshot(
      kGeneralPageMaxEntries, 512, kShortTextMaxBytes, kLargeTextMaxBytes));
  host_.reset_invocations();
  response =
      route("inspect.compute_planning", Json{{"session_id", session_id_}});
  ASSERT_TRUE(response.contains("error")) << response.dump();
  EXPECT_EQ(response["error"]["domain"], "protocol");
  EXPECT_EQ(response["error"]["name"], "response_too_large");
  EXPECT_EQ(host_.call_count("inspect.compute_planning"), 1u);
}

TEST_F(HostRoutedGraphStateProtocolTest,
       RejectsOneIndivisibleInspectionValueBeforeCursorPublication) {
  GraphInspectionView graph;
  NodeInspectionView node;
  node.id = NodeId{1};
  node.name = "indivisible";
  node.type = "fixture";
  node.subtype = "source";
  node.parameters.emplace("first", std::string(kLargeTextMaxBytes, 'a'));
  node.parameters.emplace("second", std::string(kLargeTextMaxBytes, 'b'));
  graph.nodes.push_back(std::move(node));
  host_.set_inspected_graph(std::move(graph));
  host_.reset_invocations();

  const Json response =
      route("inspect.graph", Json{{"session_id", session_id_}});
  ASSERT_TRUE(response.contains("error")) << response.dump();
  EXPECT_EQ(response["error"]["domain"], "protocol");
  EXPECT_EQ(response["error"]["name"], "response_too_large");
  EXPECT_EQ(host_.call_count("inspect.graph"), 1u);

  host_.set_node_ids({NodeId{2}});
  host_.reset_invocations();
  const Json retry =
      route("inspect.node_ids", Json{{"session_id", session_id_}});
  ASSERT_TRUE(retry.contains("result")) << retry.dump();
  EXPECT_TRUE(retry["result"]["cursor"].is_null());
  EXPECT_EQ(host_.call_count("inspect.node_ids"), 1u);
}

TEST_F(HostRoutedGraphStateProtocolTest,
       SeparatesReturnedInspectionBoundsFromMalformedPublicValues) {
  ComputePlanningInspectionSnapshot planning =
      make_protocol_planning_snapshot();
  planning.intent = static_cast<ComputeIntent>(99);
  host_.set_compute_planning(planning);
  host_.reset_invocations();
  Json response =
      route("inspect.compute_planning", Json{{"session_id", session_id_}});
  ASSERT_TRUE(response.contains("error")) << response.dump();
  EXPECT_EQ(response["error"]["domain"], "daemon");
  EXPECT_EQ(response["error"]["name"], "internal_error");
  EXPECT_EQ(host_.call_count("inspect.compute_planning"), 1u);

  planning = make_protocol_planning_snapshot();
  planning.expansion_cache_key = std::string(kLargeTextMaxBytes + 1, 'k');
  host_.set_compute_planning(std::move(planning));
  host_.reset_invocations();
  response =
      route("inspect.compute_planning", Json{{"session_id", session_id_}});
  ASSERT_TRUE(response.contains("error")) << response.dump();
  EXPECT_EQ(response["error"]["domain"], "protocol");
  EXPECT_EQ(response["error"]["name"], "response_too_large");
  EXPECT_EQ(host_.call_count("inspect.compute_planning"), 1u);

  host_.set_traversal_details(
      {{7,
        {HostTraversalNodeSnapshot{NodeId{7}, std::string("bad\xff", 4), false,
                                   false}}}});
  host_.reset_invocations();
  response =
      route("inspect.traversal_details", Json{{"session_id", session_id_}});
  ASSERT_TRUE(response.contains("error")) << response.dump();
  EXPECT_EQ(response["error"]["domain"], "daemon");
  EXPECT_EQ(response["error"]["name"], "internal_error");
  EXPECT_EQ(host_.call_count("inspect.traversal_details"), 1u);
}

TEST_F(HostRoutedGraphStateProtocolTest,
       RoutesTenVoidMutatorsOnceWithExactHostSessionAndArguments) {
  static constexpr std::array<std::string_view, 10> kMethods = {
      "graph.reload",         "graph.save",
      "graph.clear",          "graph.node_yaml.set",
      "cache.clear_all",      "cache.clear_drive",
      "cache.clear_memory",   "cache.cache_all_nodes",
      "cache.free_transient", "cache.synchronize_disk",
  };

  for (std::string_view method : kMethods) {
    host_.reset_invocations();
    const Json params = valid_host_routed_params(method, session_id_);
    const Json response = route(std::string(method), params);
    ASSERT_TRUE(response.contains("result")) << method << response.dump();
    EXPECT_EQ(response["result"], Json::object()) << method;

    const std::vector<::ps::testing::IpcHostInvocation> calls =
        host_.invocations();
    ASSERT_EQ(calls.size(), 1u) << method;
    EXPECT_EQ(calls.front().method, method);
    EXPECT_EQ(calls.front().session.value, "ipc-host-spy-session");
    if (method == "graph.reload" || method == "graph.save") {
      EXPECT_EQ(calls.front().text, "/tmp/host-routed-graph.yaml");
    } else if (method == "graph.node_yaml.set") {
      EXPECT_EQ(calls.front().first_node.value, 18);
      EXPECT_EQ(calls.front().text, "id: 18\nname: host_routed\n");
    } else if (method == "cache.cache_all_nodes" ||
               method == "cache.synchronize_disk") {
      EXPECT_EQ(calls.front().text, "float32");
    }
  }
}

TEST_F(HostRoutedGraphStateProtocolTest,
       RoutesThreeDirtyMutatorsOnceAndPreservesNestedResultAndArguments) {
  host_.set_dirty_region(make_protocol_dirty_snapshot());
  static constexpr std::array<std::string_view, 3> kMethods = {
      "dirty.begin", "dirty.update", "dirty.end"};

  for (std::string_view method : kMethods) {
    host_.reset_invocations();
    const Json response = route(std::string(method),
                                valid_host_routed_params(method, session_id_));
    ASSERT_TRUE(response.contains("result")) << method << response.dump();
    EXPECT_EQ(response["result"],
              expected_protocol_dirty_snapshot(session_id_));

    const std::vector<::ps::testing::IpcHostInvocation> calls =
        host_.invocations();
    ASSERT_EQ(calls.size(), 1u) << method;
    EXPECT_EQ(calls.front().method, method);
    EXPECT_EQ(calls.front().session.value, "ipc-host-spy-session");
    if (method == "dirty.end") {
      EXPECT_EQ(calls.front().first_node.value, 20);
      EXPECT_EQ(calls.front().dirty_domain, DirtyDomain::HighPrecision);
      EXPECT_EQ(calls.front().roi.x, 0);
      EXPECT_EQ(calls.front().roi.y, 0);
      EXPECT_EQ(calls.front().roi.width, 0);
      EXPECT_EQ(calls.front().roi.height, 0);
    } else {
      EXPECT_EQ(calls.front().first_node.value, 19);
      EXPECT_EQ(calls.front().dirty_domain, DirtyDomain::RealTime);
      EXPECT_EQ(calls.front().roi.x, -1);
      EXPECT_EQ(calls.front().roi.y, 2);
      EXPECT_EQ(calls.front().roi.width, 3);
      EXPECT_EQ(calls.front().roi.height, 4);
    }
  }
}

TEST_F(HostRoutedGraphStateProtocolTest,
       RoutesEightQueriesOnceWithExactArgumentsAndResultSchemas) {
  host_.set_node_yaml("id: 17\nname: copied\n");
  host_.set_node_ids({NodeId{4}, NodeId{2}, NodeId{4}});
  host_.set_ending_nodes({NodeId{9}});
  host_.set_forward_roi(PixelRect{-7, 8, 9, 10});
  host_.set_backward_roi(PixelRect{11, -12, 13, 14});
  TimingSnapshot timing;
  timing.node_timings = {NodeTimingSnapshot{NodeId{3}, "first", 1.25, "cpu"},
                         NodeTimingSnapshot{NodeId{5}, "second", 2.5, "cache"}};
  timing.total_ms = 3.75;
  host_.set_timing(timing);
  host_.set_last_io_time(17.5);
  host_.set_last_error(
      OperationStatus{false, OperationErrorDomain::Graph,
                      static_cast<std::int32_t>(GraphErrc::ComputeError),
                      "ignored", "copied diagnostic"});

  /**
   * @brief One exact query route and independently constructed result shape.
   * @throws std::bad_alloc when owned JSON construction cannot allocate.
   * @note Values are immutable after the test matrix is assembled.
   */
  struct QueryExpectation {
    /** @brief Exact wire method. */
    std::string_view method;
    /** @brief Exact result object expected from the router. */
    Json result;
  };
  const std::vector<QueryExpectation> cases = {
      {"graph.node_yaml.get", Json{{"session_id", session_id_},
                                   {"node_id", 17},
                                   {"yaml_text", "id: 17\nname: copied\n"}}},
      {"inspect.node_ids", Json{{"session_id", session_id_},
                                {"node_ids", Json::array({4, 2, 4})},
                                {"offset", 0},
                                {"has_more", false},
                                {"cursor", nullptr}}},
      {"inspect.ending_nodes", Json{{"session_id", session_id_},
                                    {"ending_node_ids", Json::array({9})},
                                    {"offset", 0},
                                    {"has_more", false},
                                    {"cursor", nullptr}}},
      {"inspect.roi_forward",
       Json{{"session_id", session_id_},
            {"roi", expected_rect(PixelRect{-7, 8, 9, 10})}}},
      {"inspect.roi_backward",
       Json{{"session_id", session_id_},
            {"roi", expected_rect(PixelRect{11, -12, 13, 14})}}},
      {"compute.timing",
       Json{{"session_id", session_id_},
            {"node_timings", Json::array({Json{{"node_id", 3},
                                               {"name", "first"},
                                               {"elapsed_ms", 1.25},
                                               {"source", "cpu"}},
                                          Json{{"node_id", 5},
                                               {"name", "second"},
                                               {"elapsed_ms", 2.5},
                                               {"source", "cache"}}})},
            {"total_ms", 3.75}}},
      {"compute.last_io_time",
       Json{{"session_id", session_id_}, {"last_io_time_ms", 17.5}}},
      {"compute.last_error",
       Json{{"session_id", session_id_},
            {"status",
             Json{{"ok", false},
                  {"domain", "graph"},
                  {"code", static_cast<std::int32_t>(GraphErrc::ComputeError)},
                  {"name", "compute_error"},
                  {"message", "copied diagnostic"}}}}},
  };

  for (const QueryExpectation& test_case : cases) {
    host_.reset_invocations();
    const Json response =
        route(std::string(test_case.method),
              valid_host_routed_params(test_case.method, session_id_));
    ASSERT_TRUE(response.contains("result"))
        << test_case.method << response.dump();
    EXPECT_EQ(response["result"], test_case.result) << test_case.method;

    const std::vector<::ps::testing::IpcHostInvocation> calls =
        host_.invocations();
    ASSERT_EQ(calls.size(), 1u) << test_case.method;
    EXPECT_EQ(calls.front().method, test_case.method);
    EXPECT_EQ(calls.front().session.value, "ipc-host-spy-session");
    if (test_case.method == "graph.node_yaml.get") {
      EXPECT_EQ(calls.front().first_node.value, 17);
    } else if (test_case.method == "inspect.roi_forward") {
      EXPECT_EQ(calls.front().first_node.value, 21);
      EXPECT_EQ(calls.front().second_node.value, 22);
      EXPECT_EQ(calls.front().roi.x, 5);
      EXPECT_EQ(calls.front().roi.y, 6);
      EXPECT_EQ(calls.front().roi.width, 7);
      EXPECT_EQ(calls.front().roi.height, 8);
    } else if (test_case.method == "inspect.roi_backward") {
      EXPECT_EQ(calls.front().first_node.value, 23);
      EXPECT_EQ(calls.front().second_node.value, 24);
      EXPECT_EQ(calls.front().roi.x, 9);
      EXPECT_EQ(calls.front().roi.y, 10);
      EXPECT_EQ(calls.front().roi.width, 11);
      EXPECT_EQ(calls.front().roi.height, 12);
    }
  }
}

TEST_F(HostRoutedGraphStateProtocolTest,
       EveryMutatorPropagatesOneGraphFailureWithoutRetry) {
  static constexpr std::array<std::string_view, 13> kMutators = {
      "graph.reload",
      "graph.save",
      "graph.clear",
      "graph.node_yaml.set",
      "cache.clear_all",
      "cache.clear_drive",
      "cache.clear_memory",
      "cache.cache_all_nodes",
      "cache.free_transient",
      "cache.synchronize_disk",
      "dirty.begin",
      "dirty.update",
      "dirty.end",
  };

  for (std::string_view method : kMutators) {
    host_.set_status(std::string(method), host_routed_graph_failure());
    host_.reset_invocations();
    const Json response = route(std::string(method),
                                valid_host_routed_params(method, session_id_));
    ASSERT_TRUE(response.contains("error")) << method << response.dump();
    EXPECT_EQ(response["error"]["domain"], "graph") << method;
    EXPECT_EQ(response["error"]["code"],
              static_cast<std::int32_t>(GraphErrc::Io))
        << method;
    EXPECT_EQ(response["error"]["name"], "io") << method;
    EXPECT_EQ(response["error"]["message"], "host-routed Host failure")
        << method;
    EXPECT_EQ(host_.call_count(method), 1u) << method;
  }
}

TEST_F(HostRoutedGraphStateProtocolTest,
       SevenValueRoutesPropagateOneGraphFailureWithoutRetry) {
  static constexpr std::array<std::string_view, 7> kMethods = {
      "graph.node_yaml.get", "inspect.node_ids",     "inspect.ending_nodes",
      "inspect.roi_forward", "inspect.roi_backward", "compute.timing",
      "compute.last_io_time"};

  for (std::string_view method : kMethods) {
    host_.set_status(std::string(method), host_routed_graph_failure());
    host_.reset_invocations();
    const Json response = route(std::string(method),
                                valid_host_routed_params(method, session_id_));
    ASSERT_TRUE(response.contains("error")) << method << response.dump();
    EXPECT_EQ(response["error"]["domain"], "graph") << method;
    EXPECT_EQ(response["error"]["code"],
              static_cast<std::int32_t>(GraphErrc::Io))
        << method;
    EXPECT_EQ(response["error"]["name"], "io") << method;
    EXPECT_EQ(response["error"]["message"], "host-routed Host failure")
        << method;
    EXPECT_EQ(host_.call_count(method), 1u) << method;
  }
}

TEST_F(HostRoutedGraphStateProtocolTest,
       LastErrorNestsFutureProtocolAndDaemonStatusesWithoutTopLevelFailure) {
  /**
   * @brief One future status vocabulary pair preserved as diagnostic data.
   * @throws std::bad_alloc when copied status strings cannot allocate.
   * @note The router must not reinterpret these unknown code/name pairs.
   */
  struct StatusCase {
    /** @brief Host diagnostic status to expose as data. */
    OperationStatus status;
    /** @brief Stable expected wire-domain label. */
    std::string domain;
  };
  const std::vector<StatusCase> cases = {
      {OperationStatus{false, OperationErrorDomain::Protocol, -32122,
                       "future_protocol_failure", "protocol diagnostic"},
       "protocol"},
      {OperationStatus{false, OperationErrorDomain::Daemon, -32123,
                       "future_daemon_failure", "daemon diagnostic"},
       "daemon"},
  };

  for (const StatusCase& test_case : cases) {
    host_.set_last_error(test_case.status);
    host_.reset_invocations();
    const Json response =
        route("compute.last_error",
              valid_host_routed_params("compute.last_error", session_id_));
    ASSERT_TRUE(response.contains("result")) << response.dump();
    EXPECT_FALSE(response.contains("error"));
    const Json& status = response["result"]["status"];
    EXPECT_FALSE(status["ok"].get<bool>());
    EXPECT_EQ(status["domain"], test_case.domain);
    EXPECT_EQ(status["code"], test_case.status.code);
    EXPECT_EQ(status["name"], test_case.status.name);
    EXPECT_EQ(status["message"], test_case.status.message);
    EXPECT_EQ(host_.call_count("compute.last_error"), 1u);
  }

  host_.set_last_error(OperationStatus{
      false, OperationErrorDomain::Daemon, -32124, "future_long_failure",
      std::string(kDiagnosticTextMaxBytes + 1, 'd')});
  host_.reset_invocations();
  const Json bounded =
      route("compute.last_error",
            valid_host_routed_params("compute.last_error", session_id_));
  ASSERT_TRUE(bounded.contains("result")) << bounded.dump();
  const std::string message =
      bounded["result"]["status"]["message"].get<std::string>();
  EXPECT_LE(message.size(), kDiagnosticTextMaxBytes);
  EXPECT_NE(message.find("[truncated]"), std::string::npos);
  EXPECT_EQ(host_.call_count("compute.last_error"), 1u);
}

TEST_F(HostRoutedGraphStateProtocolTest,
       MutatorEncodingFailureBecomesInternalErrorWithoutRetry) {
  host_.set_status("graph.clear",
                   OperationStatus{false, OperationErrorDomain::Graph, 999, "",
                                   "malformed Host status after mutation"});
  const Json response = route(
      "graph.clear", valid_host_routed_params("graph.clear", session_id_));
  ASSERT_TRUE(response.contains("error")) << response.dump();
  EXPECT_EQ(response["error"]["domain"], "daemon");
  EXPECT_EQ(response["error"]["name"], "internal_error");
  EXPECT_EQ(host_.call_count("graph.clear"), 1u);
}

TEST_F(HostRoutedGraphStateProtocolTest,
       EveryRouteRejectsMalformedSessionWithoutHostAccess) {
  static constexpr std::array<std::string_view, 21> kMethods = {
      "cache.cache_all_nodes",
      "cache.clear_all",
      "cache.clear_drive",
      "cache.clear_memory",
      "cache.free_transient",
      "cache.synchronize_disk",
      "compute.last_error",
      "compute.last_io_time",
      "compute.timing",
      "dirty.begin",
      "dirty.end",
      "dirty.update",
      "graph.clear",
      "graph.node_yaml.get",
      "graph.node_yaml.set",
      "graph.reload",
      "graph.save",
      "inspect.ending_nodes",
      "inspect.node_ids",
      "inspect.roi_backward",
      "inspect.roi_forward",
  };

  for (std::string_view method : kMethods) {
    Json params = valid_host_routed_params(method, session_id_);
    params["session_id"] = std::string(kOpaqueIdHexCharacters - 1, 'a');
    host_.reset_invocations();
    const Json response = route(std::string(method), std::move(params));
    ASSERT_TRUE(response.contains("error")) << method << response.dump();
    EXPECT_EQ(response["error"]["domain"], "protocol") << method;
    EXPECT_EQ(response["error"]["name"], "invalid_params") << method;
    EXPECT_TRUE(host_.invocations().empty()) << method;
  }
}

TEST_F(HostRoutedGraphStateProtocolTest,
       EveryRouteRejectsUnknownSessionWithoutHostAccess) {
  static constexpr std::array<std::string_view, 21> kMethods = {
      "cache.cache_all_nodes",
      "cache.clear_all",
      "cache.clear_drive",
      "cache.clear_memory",
      "cache.free_transient",
      "cache.synchronize_disk",
      "compute.last_error",
      "compute.last_io_time",
      "compute.timing",
      "dirty.begin",
      "dirty.end",
      "dirty.update",
      "graph.clear",
      "graph.node_yaml.get",
      "graph.node_yaml.set",
      "graph.reload",
      "graph.save",
      "inspect.ending_nodes",
      "inspect.node_ids",
      "inspect.roi_backward",
      "inspect.roi_forward",
  };
  std::string unknown_session(kOpaqueIdHexCharacters, 'a');
  if (unknown_session == session_id_) {
    unknown_session.front() = 'b';
  }

  for (std::string_view method : kMethods) {
    host_.reset_invocations();
    const Json response = route(
        std::string(method), valid_host_routed_params(method, unknown_session));
    ASSERT_TRUE(response.contains("error")) << method << response.dump();
    EXPECT_EQ(response["error"]["domain"], "graph") << method;
    EXPECT_EQ(response["error"]["name"], "not_found") << method;
    EXPECT_TRUE(host_.invocations().empty()) << method;
  }
}

TEST_F(HostRoutedGraphStateProtocolTest,
       ValidatesEveryAdditionalKnownParamBeforeUnknownSessionAdmission) {
  std::string unknown_session(kOpaqueIdHexCharacters, 'b');
  if (unknown_session == session_id_) {
    unknown_session.front() = 'c';
  }
  /**
   * @brief One request proving known-field validation precedes admission.
   * @throws std::bad_alloc when owned method or JSON storage cannot allocate.
   * @note Each case carries a well-formed but absent opaque session id.
   */
  struct MalformedCase {
    /** @brief Wire method under test. */
    std::string method;
    /** @brief Params with one malformed known field. */
    Json params;
  };
  std::vector<MalformedCase> cases;

  Json reload = valid_host_routed_params("graph.reload", unknown_session);
  reload["yaml_path"] = "relative.yaml";
  cases.push_back({"graph.reload", std::move(reload)});
  Json save = valid_host_routed_params("graph.save", unknown_session);
  save["yaml_path"] = Json::array();
  cases.push_back({"graph.save", std::move(save)});
  Json get_yaml =
      valid_host_routed_params("graph.node_yaml.get", unknown_session);
  get_yaml["node_id"] = -1;
  cases.push_back({"graph.node_yaml.get", std::move(get_yaml)});
  Json set_yaml =
      valid_host_routed_params("graph.node_yaml.set", unknown_session);
  set_yaml["yaml_text"] = Json::object();
  cases.push_back({"graph.node_yaml.set", std::move(set_yaml)});
  Json cache =
      valid_host_routed_params("cache.cache_all_nodes", unknown_session);
  cache["precision"] = std::string(kShortTextMaxBytes + 1, 'p');
  cases.push_back({"cache.cache_all_nodes", std::move(cache)});
  Json sync =
      valid_host_routed_params("cache.synchronize_disk", unknown_session);
  sync["precision"] = Json::array({"bad", "precision"});
  cases.push_back({"cache.synchronize_disk", std::move(sync)});
  Json dirty_begin = valid_host_routed_params("dirty.begin", unknown_session);
  dirty_begin["node_id"] = std::uint64_t{2147483648ULL};
  cases.push_back({"dirty.begin", std::move(dirty_begin)});
  Json dirty_update = valid_host_routed_params("dirty.update", unknown_session);
  dirty_update["domain"] = "future_domain";
  cases.push_back({"dirty.update", std::move(dirty_update)});
  Json dirty_end = valid_host_routed_params("dirty.end", unknown_session);
  dirty_end["domain"] = 1;
  cases.push_back({"dirty.end", std::move(dirty_end)});
  Json forward =
      valid_host_routed_params("inspect.roi_forward", unknown_session);
  forward["target_node_id"] = -1;
  cases.push_back({"inspect.roi_forward", std::move(forward)});
  Json backward =
      valid_host_routed_params("inspect.roi_backward", unknown_session);
  backward["target_roi"].erase("height");
  cases.push_back({"inspect.roi_backward", std::move(backward)});

  for (const MalformedCase& test_case : cases) {
    host_.reset_invocations();
    const Json response = route(test_case.method, test_case.params);
    ASSERT_TRUE(response.contains("error"))
        << test_case.method << response.dump();
    EXPECT_EQ(response["error"]["domain"], "protocol") << test_case.method;
    EXPECT_EQ(response["error"]["name"], "invalid_params") << test_case.method;
    EXPECT_TRUE(host_.invocations().empty()) << test_case.method;
  }
}

TEST_F(HostRoutedGraphStateProtocolTest,
       UnknownParamsFieldsAreForwardCompatibleForAllRoutes) {
  static constexpr std::array<std::string_view, 21> kMethods = {
      "cache.cache_all_nodes",
      "cache.clear_all",
      "cache.clear_drive",
      "cache.clear_memory",
      "cache.free_transient",
      "cache.synchronize_disk",
      "compute.last_error",
      "compute.last_io_time",
      "compute.timing",
      "dirty.begin",
      "dirty.end",
      "dirty.update",
      "graph.clear",
      "graph.node_yaml.get",
      "graph.node_yaml.set",
      "graph.reload",
      "graph.save",
      "inspect.ending_nodes",
      "inspect.node_ids",
      "inspect.roi_backward",
      "inspect.roi_forward",
  };

  for (std::string_view method : kMethods) {
    Json params = valid_host_routed_params(method, session_id_);
    params["future_field"] = Json{{"nested", Json::array({1, 2, 3})}};
    host_.reset_invocations();
    const Json response = route(std::string(method), std::move(params));
    ASSERT_TRUE(response.contains("result")) << method << response.dump();
    EXPECT_EQ(host_.call_count(method), 1u) << method;
  }
}

TEST_F(HostRoutedGraphStateProtocolTest,
       PreservesInclusivePathYamlPrecisionNodeDomainAndRoiBounds) {
  const std::string exact_path = "/" + std::string(kPathTextMaxBytes - 1, 'p');
  Json reload = valid_host_routed_params("graph.reload", session_id_);
  reload["yaml_path"] = exact_path;
  Json response = route("graph.reload", reload);
  ASSERT_TRUE(response.contains("result")) << response.dump();
  ASSERT_EQ(host_.call_count("graph.reload"), 1u);
  EXPECT_EQ(host_.invocations().front().text, exact_path);

  host_.reset_invocations();
  reload["yaml_path"] = exact_path + "p";
  response = route("graph.reload", reload);
  EXPECT_EQ(response["error"]["name"], "invalid_params");
  EXPECT_TRUE(host_.invocations().empty());

  const std::string exact_yaml(kLargeTextMaxBytes, 'y');
  Json set_yaml = valid_host_routed_params("graph.node_yaml.set", session_id_);
  set_yaml["yaml_text"] = exact_yaml;
  response = route("graph.node_yaml.set", set_yaml);
  ASSERT_TRUE(response.contains("result")) << response.dump();
  ASSERT_EQ(host_.call_count("graph.node_yaml.set"), 1u);
  EXPECT_EQ(host_.invocations().front().text.size(), kLargeTextMaxBytes);

  host_.reset_invocations();
  set_yaml["yaml_text"] = exact_yaml + "y";
  response = route("graph.node_yaml.set", set_yaml);
  EXPECT_EQ(response["error"]["name"], "invalid_params");
  EXPECT_TRUE(host_.invocations().empty());

  host_.reset_invocations();
  set_yaml["yaml_text"] = "";
  response = route("graph.node_yaml.set", set_yaml);
  ASSERT_TRUE(response.contains("result")) << response.dump();
  ASSERT_EQ(host_.call_count("graph.node_yaml.set"), 1u);
  EXPECT_TRUE(host_.invocations().front().text.empty());

  host_.reset_invocations();
  const std::string nul_yaml("id:\0exact", 9);
  set_yaml["yaml_text"] = nul_yaml;
  response = route("graph.node_yaml.set", set_yaml);
  ASSERT_TRUE(response.contains("result")) << response.dump();
  ASSERT_EQ(host_.call_count("graph.node_yaml.set"), 1u);
  EXPECT_EQ(host_.invocations().front().text, nul_yaml);

  host_.reset_invocations();
  const std::string exact_precision(kShortTextMaxBytes, 'f');
  Json cache = valid_host_routed_params("cache.cache_all_nodes", session_id_);
  cache["precision"] = exact_precision;
  response = route("cache.cache_all_nodes", cache);
  ASSERT_TRUE(response.contains("result")) << response.dump();
  ASSERT_EQ(host_.call_count("cache.cache_all_nodes"), 1u);
  EXPECT_EQ(host_.invocations().front().text, exact_precision);

  host_.reset_invocations();
  cache["precision"] = exact_precision + "f";
  response = route("cache.cache_all_nodes", cache);
  EXPECT_EQ(response["error"]["name"], "invalid_params");
  EXPECT_TRUE(host_.invocations().empty());

  host_.reset_invocations();
  Json sync = valid_host_routed_params("cache.synchronize_disk", session_id_);
  sync["precision"] = "";
  response = route("cache.synchronize_disk", sync);
  ASSERT_TRUE(response.contains("result")) << response.dump();
  ASSERT_EQ(host_.call_count("cache.synchronize_disk"), 1u);
  EXPECT_TRUE(host_.invocations().front().text.empty());

  host_.reset_invocations();
  const std::string nul_precision("fp\0exact", 8);
  sync["precision"] = nul_precision;
  response = route("cache.synchronize_disk", sync);
  ASSERT_TRUE(response.contains("result")) << response.dump();
  ASSERT_EQ(host_.call_count("cache.synchronize_disk"), 1u);
  EXPECT_EQ(host_.invocations().front().text, nul_precision);

  host_.reset_invocations();
  Json node = valid_host_routed_params("graph.node_yaml.get", session_id_);
  node["node_id"] = std::numeric_limits<int>::max();
  response = route("graph.node_yaml.get", node);
  ASSERT_TRUE(response.contains("result")) << response.dump();
  ASSERT_EQ(host_.call_count("graph.node_yaml.get"), 1u);
  EXPECT_EQ(host_.invocations().front().first_node.value,
            std::numeric_limits<int>::max());

  host_.reset_invocations();
  node["node_id"] = std::uint64_t{2147483648ULL};
  response = route("graph.node_yaml.get", node);
  EXPECT_EQ(response["error"]["name"], "invalid_params");
  EXPECT_TRUE(host_.invocations().empty());

  host_.reset_invocations();
  Json dirty = valid_host_routed_params("dirty.begin", session_id_);
  dirty["node_id"] = 0;
  dirty["domain"] = "high_precision";
  dirty["source_roi"] = expected_rect(PixelRect{
      std::numeric_limits<int>::min(), std::numeric_limits<int>::max(),
      std::numeric_limits<int>::min(), std::numeric_limits<int>::max()});
  response = route("dirty.begin", dirty);
  ASSERT_TRUE(response.contains("result")) << response.dump();
  ASSERT_EQ(host_.call_count("dirty.begin"), 1u);
  const auto dirty_call = host_.invocations().front();
  EXPECT_EQ(dirty_call.first_node.value, 0);
  EXPECT_EQ(dirty_call.dirty_domain, DirtyDomain::HighPrecision);
  EXPECT_EQ(dirty_call.roi.x, std::numeric_limits<int>::min());
  EXPECT_EQ(dirty_call.roi.y, std::numeric_limits<int>::max());
  EXPECT_EQ(dirty_call.roi.width, std::numeric_limits<int>::min());
  EXPECT_EQ(dirty_call.roi.height, std::numeric_limits<int>::max());

  host_.reset_invocations();
  Json backward = valid_host_routed_params("inspect.roi_backward", session_id_);
  backward["target_roi"]["x"] = std::uint64_t{2147483648ULL};
  response = route("inspect.roi_backward", backward);
  EXPECT_EQ(response["error"]["name"], "invalid_params");
  EXPECT_TRUE(host_.invocations().empty());
}

TEST_F(HostRoutedGraphStateProtocolTest,
       ObservationRoutesPreserveExactEventAndTraceWireSchemas) {
  ComputeEventBatch event_batch;
  event_batch.events = {
      ComputeEventSnapshot{2, NodeId{7}, "first", "cpu", 1.25},
      ComputeEventSnapshot{3, NodeId{8}, "second", "cache",
                           std::numeric_limits<double>::quiet_NaN()}};
  event_batch.next_sequence = 4;
  event_batch.has_more = false;
  event_batch.dropped_count = 5;
  host_.set_compute_event_batch(event_batch);

  Json response =
      route("events.drain", Json{{"session_id", session_id_},
                                 {"limit", 2},
                                 {"max_entries", 9999},
                                 {"future_observation_field", true}});
  ASSERT_TRUE(response.contains("result")) << response.dump();
  EXPECT_EQ(response["result"],
            (Json{{"session_id", session_id_},
                  {"events", Json::array({Json{{"sequence", 2},
                                               {"node_id", 7},
                                               {"name", "first"},
                                               {"source", "cpu"},
                                               {"elapsed_ms", 1.25}},
                                          Json{{"sequence", 3},
                                               {"node_id", 8},
                                               {"name", "second"},
                                               {"source", "cache"},
                                               {"elapsed_ms", nullptr}}})},
                  {"next_sequence", 4},
                  {"has_more", false},
                  {"dropped_count", 5}}));
  ASSERT_EQ(host_.call_count("events.drain"), 1u);
  auto invocation = host_.invocations().front();
  EXPECT_EQ(invocation.session.value, "ipc-host-spy-session");
  EXPECT_EQ(invocation.first_node.value, 2);

  ExecutionTracePage trace_page;
  trace_page.events = {
      ExecutionTraceEventSnapshot{
          kObservationSequenceExhausted - 2, 31, NodeId{-1}, -1,
          HostExecutionTraceAction::RethrowException, 9001},
      ExecutionTraceEventSnapshot{kObservationSequenceExhausted - 1, 32,
                                  NodeId{9}, 4,
                                  HostExecutionTraceAction::ExecuteTile, 9002}};
  trace_page.next_sequence = kObservationSequenceExhausted;
  trace_page.has_more = false;
  trace_page.dropped_count = kObservationSequenceExhausted;
  host_.set_execution_trace_page(trace_page);
  host_.reset_invocations();

  response = route("execution.trace",
                   Json{{"session_id", session_id_},
                        {"after_sequence", kObservationSequenceExhausted - 3},
                        {"limit", 2},
                        {"max_entries", 1}});
  ASSERT_TRUE(response.contains("result")) << response.dump();
  const Json& result = response["result"];
  EXPECT_EQ(result["session_id"], session_id_);
  ASSERT_EQ(result["events"].size(), 2u);
  EXPECT_EQ(result["events"][0],
            (Json{{"sequence", kObservationSequenceExhausted - 2},
                  {"epoch", 31},
                  {"node_id", -1},
                  {"worker_id", -1},
                  {"action", "rethrow_exception"},
                  {"timestamp_us", 9001}}));
  EXPECT_EQ(result["events"][1]["sequence"], kObservationSequenceExhausted - 1);
  EXPECT_EQ(result["events"][1]["action"], "execute_tile");
  EXPECT_EQ(result["next_sequence"], kObservationSequenceExhausted);
  EXPECT_FALSE(result["has_more"].get<bool>());
  EXPECT_EQ(result["dropped_count"], kObservationSequenceExhausted);
  ASSERT_EQ(host_.call_count("execution.trace"), 1u);
  invocation = host_.invocations().front();
  EXPECT_EQ(invocation.first_node.value, 2);
  EXPECT_EQ(invocation.text, std::to_string(kObservationSequenceExhausted - 3));
}

TEST_F(HostRoutedGraphStateProtocolTest,
       ObservationPagingPreservesDestructiveAndNonDestructiveBoundaries) {
  ComputeEventBatch first_events;
  first_events.events = {
      ComputeEventSnapshot{1, NodeId{1}, "one", "compute", 1.0}};
  first_events.next_sequence = 2;
  first_events.has_more = true;
  first_events.dropped_count = 1;
  host_.set_compute_event_batch(first_events);
  Json first =
      route("events.drain", Json{{"session_id", session_id_}, {"limit", 1}});
  ASSERT_TRUE(first.contains("result")) << first.dump();
  EXPECT_EQ(first["result"]["events"][0]["sequence"], 1);
  EXPECT_TRUE(first["result"]["has_more"].get<bool>());
  EXPECT_EQ(first["result"]["dropped_count"], 1);

  ComputeEventBatch second_events;
  second_events.events = {
      ComputeEventSnapshot{2, NodeId{2}, "two", "compute", 2.0}};
  second_events.next_sequence = 3;
  host_.set_compute_event_batch(second_events);
  Json second =
      route("events.drain", Json{{"session_id", session_id_}, {"limit", 1}});
  ASSERT_TRUE(second.contains("result")) << second.dump();
  EXPECT_EQ(second["result"]["events"][0]["sequence"], 2);
  EXPECT_FALSE(second["result"]["has_more"].get<bool>());
  EXPECT_EQ(second["result"]["dropped_count"], 0);

  ExecutionTracePage first_trace;
  first_trace.events = {ExecutionTraceEventSnapshot{
      10, 1, NodeId{3}, 0, HostExecutionTraceAction::Execute, 100}};
  first_trace.next_sequence = 10;
  first_trace.has_more = true;
  first_trace.dropped_count = 9;
  host_.set_execution_trace_page(first_trace);
  Json trace = route(
      "execution.trace",
      Json{{"session_id", session_id_}, {"after_sequence", 0}, {"limit", 1}});
  ASSERT_TRUE(trace.contains("result")) << trace.dump();
  const Json repeated = route(
      "execution.trace",
      Json{{"session_id", session_id_}, {"after_sequence", 0}, {"limit", 1}});
  ASSERT_TRUE(repeated.contains("result")) << repeated.dump();
  EXPECT_EQ(repeated["result"], trace["result"]);

  ExecutionTracePage second_trace;
  second_trace.events = {ExecutionTraceEventSnapshot{
      11, 2, NodeId{4}, 1, HostExecutionTraceAction::ExecuteTile, 101}};
  second_trace.next_sequence = 11;
  second_trace.dropped_count = 0;
  host_.set_execution_trace_page(second_trace);
  const Json final = route(
      "execution.trace",
      Json{{"session_id", session_id_}, {"after_sequence", 10}, {"limit", 1}});
  ASSERT_TRUE(final.contains("result")) << final.dump();
  EXPECT_EQ(final["result"]["events"][0]["sequence"], 11);
  EXPECT_FALSE(final["result"]["has_more"].get<bool>());

  ExecutionTracePage empty_trace;
  empty_trace.next_sequence = 11;
  host_.set_execution_trace_page(empty_trace);
  const Json empty = route(
      "execution.trace",
      Json{{"session_id", session_id_}, {"after_sequence", 11}, {"limit", 1}});
  ASSERT_TRUE(empty.contains("result")) << empty.dump();
  EXPECT_TRUE(empty["result"]["events"].empty());
  EXPECT_EQ(empty["result"]["next_sequence"], 11);
  EXPECT_FALSE(empty["result"]["has_more"].get<bool>());
  EXPECT_EQ(host_.call_count("events.drain"), 2u);
  EXPECT_EQ(host_.call_count("execution.trace"), 4u);
}

TEST_F(HostRoutedGraphStateProtocolTest,
       ObservationParamsRejectExactNumericBoundsBeforeHostAccess) {
  const std::vector<Json> invalid_event_limits = {
      Json(),   Json(0),   Json(kComputeEventDrainMaxLimit + 1),
      Json(-1), Json(1.5), Json(std::numeric_limits<uint64_t>::max())};
  for (const Json& limit : invalid_event_limits) {
    Json params{{"session_id", session_id_}};
    if (!limit.is_null()) {
      params["limit"] = limit;
    }
    const Json response = route("events.drain", std::move(params));
    ASSERT_TRUE(response.contains("error")) << response.dump();
    EXPECT_EQ(response["error"]["name"], "invalid_params");
  }
  EXPECT_EQ(host_.call_count("events.drain"), 0u);

  Json max_entries_only{{"session_id", session_id_}, {"max_entries", 1}};
  Json response = route("events.drain", std::move(max_entries_only));
  EXPECT_EQ(response["error"]["name"], "invalid_params");
  EXPECT_EQ(host_.call_count("events.drain"), 0u);

  ComputeEventBatch empty_events;
  empty_events.next_sequence = 1;
  host_.set_compute_event_batch(empty_events);
  for (std::size_t limit :
       {kComputeEventDrainMinLimit, kComputeEventDrainMaxLimit}) {
    response = route("events.drain",
                     Json{{"session_id", session_id_}, {"limit", limit}});
    ASSERT_TRUE(response.contains("result")) << response.dump();
    EXPECT_TRUE(response["result"]["events"].empty());
    EXPECT_EQ(response["result"]["next_sequence"], 1);
  }
  EXPECT_EQ(host_.call_count("events.drain"), 2u);

  const std::vector<Json> invalid_trace_cursors = {Json(), Json(-1), Json(1.5),
                                                   Json("1")};
  for (const Json& cursor : invalid_trace_cursors) {
    Json params{{"session_id", session_id_}, {"limit", 1}};
    if (!cursor.is_null()) {
      params["after_sequence"] = cursor;
    }
    response = route("execution.trace", std::move(params));
    ASSERT_TRUE(response.contains("error")) << response.dump();
    EXPECT_EQ(response["error"]["name"], "invalid_params");
  }
  for (const Json& limit : std::vector<Json>{
           Json(0), Json(kExecutionTraceMaxLimit + 1), Json(-1), Json(1.5)}) {
    response = route("execution.trace", Json{{"session_id", session_id_},
                                             {"after_sequence", 0},
                                             {"limit", limit}});
    ASSERT_TRUE(response.contains("error")) << response.dump();
    EXPECT_EQ(response["error"]["name"], "invalid_params");
  }
  EXPECT_EQ(host_.call_count("execution.trace"), 0u);

  ExecutionTracePage terminal;
  terminal.next_sequence = kObservationSequenceExhausted;
  host_.set_execution_trace_page(terminal);
  response = route("execution.trace",
                   Json{{"session_id", session_id_},
                        {"after_sequence", kObservationSequenceExhausted},
                        {"limit", kExecutionTraceMaxLimit}});
  ASSERT_TRUE(response.contains("result")) << response.dump();
  EXPECT_EQ(response["result"]["next_sequence"], kObservationSequenceExhausted);
  EXPECT_EQ(host_.call_count("execution.trace"), 1u);
}

TEST_F(HostRoutedGraphStateProtocolTest,
       ObservationRoutesPreserveHostFailuresAndSessionLookup) {
  const std::string unknown_session(32, 'a');
  Json response = route("events.drain",
                        Json{{"session_id", unknown_session}, {"limit", 1}});
  ASSERT_TRUE(response.contains("error")) << response.dump();
  EXPECT_EQ(response["error"]["domain"], "graph");
  EXPECT_EQ(response["error"]["name"], "not_found");
  EXPECT_EQ(host_.call_count("events.drain"), 0u);

  host_.set_status("events.drain", host_routed_graph_failure());
  response =
      route("events.drain", Json{{"session_id", session_id_}, {"limit", 1}});
  ASSERT_TRUE(response.contains("error")) << response.dump();
  EXPECT_EQ(response["error"]["domain"], "graph");
  EXPECT_EQ(response["error"]["name"], "io");
  EXPECT_EQ(host_.call_count("events.drain"), 1u);

  host_.set_status("execution.trace", host_routed_graph_failure());
  response = route(
      "execution.trace",
      Json{{"session_id", session_id_}, {"after_sequence", 0}, {"limit", 1}});
  ASSERT_TRUE(response.contains("error")) << response.dump();
  EXPECT_EQ(response["error"]["domain"], "graph");
  EXPECT_EQ(response["error"]["name"], "io");
  EXPECT_EQ(host_.call_count("execution.trace"), 1u);
}

TEST_F(HostRoutedGraphStateProtocolTest,
       ObservationRoutesRejectMalformedHostValuesWithoutRetry) {
  ComputeEventBatch invalid_utf8;
  invalid_utf8.events = {ComputeEventSnapshot{
      1, NodeId{1}, std::string("\xc3\x28", 2), "source", 1.0}};
  invalid_utf8.next_sequence = 2;
  host_.set_compute_event_batch(invalid_utf8);
  Json response =
      route("events.drain", Json{{"session_id", session_id_}, {"limit", 1}});
  EXPECT_EQ(response["error"]["domain"], "daemon");
  EXPECT_EQ(response["error"]["name"], "internal_error");
  EXPECT_EQ(host_.call_count("events.drain"), 1u);

  ComputeEventBatch oversized;
  oversized.events = {ComputeEventSnapshot{
      1, NodeId{1}, std::string(kComputeEventTextMaxBytes + 1, 'x'), "source",
      1.0}};
  oversized.next_sequence = 2;
  host_.set_compute_event_batch(oversized);
  host_.reset_invocations();
  response =
      route("events.drain", Json{{"session_id", session_id_}, {"limit", 1}});
  EXPECT_EQ(response["error"]["domain"], "protocol");
  EXPECT_EQ(response["error"]["name"], "response_too_large");
  EXPECT_EQ(host_.call_count("events.drain"), 1u);

  ExecutionTracePage malformed_trace;
  malformed_trace.events = {ExecutionTraceEventSnapshot{
      4, 1, NodeId{1}, 0, static_cast<HostExecutionTraceAction>(999), 1}};
  malformed_trace.next_sequence = 4;
  host_.set_execution_trace_page(malformed_trace);
  host_.reset_invocations();
  response = route(
      "execution.trace",
      Json{{"session_id", session_id_}, {"after_sequence", 3}, {"limit", 1}});
  EXPECT_EQ(response["error"]["domain"], "daemon");
  EXPECT_EQ(response["error"]["name"], "internal_error");
  EXPECT_EQ(host_.call_count("execution.trace"), 1u);

  malformed_trace.events.front().action = HostExecutionTraceAction::Execute;
  malformed_trace.events.front().node = NodeId{-2};
  host_.set_execution_trace_page(malformed_trace);
  host_.reset_invocations();
  response = route(
      "execution.trace",
      Json{{"session_id", session_id_}, {"after_sequence", 3}, {"limit", 1}});
  ASSERT_TRUE(response.contains("error")) << response.dump();
  EXPECT_EQ(response["error"]["domain"], "daemon");
  EXPECT_EQ(response["error"]["name"], "internal_error");
  EXPECT_EQ(host_.call_count("execution.trace"), 1u);

  malformed_trace.events.front().node = NodeId{1};
  malformed_trace.events.front().worker_id = -2;
  host_.set_execution_trace_page(malformed_trace);
  host_.reset_invocations();
  response = route(
      "execution.trace",
      Json{{"session_id", session_id_}, {"after_sequence", 3}, {"limit", 1}});
  ASSERT_TRUE(response.contains("error")) << response.dump();
  EXPECT_EQ(response["error"]["domain"], "daemon");
  EXPECT_EQ(response["error"]["name"], "internal_error");
  EXPECT_EQ(host_.call_count("execution.trace"), 1u);

  malformed_trace.events.front().worker_id = 0;
  malformed_trace.next_sequence = kObservationSequenceExhausted;
  host_.set_execution_trace_page(malformed_trace);
  host_.reset_invocations();
  response = route(
      "execution.trace",
      Json{{"session_id", session_id_}, {"after_sequence", 3}, {"limit", 1}});
  EXPECT_EQ(response["error"]["domain"], "daemon");
  EXPECT_EQ(response["error"]["name"], "internal_error");
  EXPECT_EQ(host_.call_count("execution.trace"), 1u);
}

TEST_F(HostRoutedGraphStateProtocolTest,
       NodeListRoutesPreserveSingleAndMultipleStablePages) {
  static constexpr std::array<std::string_view, 2> kMethods = {
      "inspect.node_ids", "inspect.ending_nodes"};
  for (std::string_view method : kMethods) {
    for (std::size_t count :
         {std::size_t{0}, std::size_t{1}, kGeneralPageMaxEntries}) {
      std::vector<NodeId> nodes;
      nodes.reserve(count);
      for (std::size_t index = 0; index < count; ++index) {
        nodes.push_back(NodeId{static_cast<int>((index * 7) % 19)});
      }
      if (method == "inspect.node_ids") {
        host_.set_node_ids(nodes);
      } else {
        host_.set_ending_nodes(nodes);
      }
      host_.reset_invocations();
      const Json response = route(
          std::string(method), valid_host_routed_params(method, session_id_));
      ASSERT_TRUE(response.contains("result")) << method << response.dump();
      const char* field =
          method == "inspect.node_ids" ? "node_ids" : "ending_node_ids";
      ASSERT_EQ(response["result"][field].size(), count) << method;
      for (std::size_t index = 0; index < count; ++index) {
        EXPECT_EQ(response["result"][field][index], nodes[index].value)
            << method << index;
      }
      EXPECT_EQ(host_.call_count(method), 1u) << method;
    }

    std::vector<NodeId> oversized(kGeneralPageMaxEntries + 1, NodeId{7});
    if (method == "inspect.node_ids") {
      host_.set_node_ids(std::move(oversized));
    } else {
      host_.set_ending_nodes(std::move(oversized));
    }
    host_.reset_invocations();
    const Json first = route(std::string(method),
                             valid_host_routed_params(method, session_id_));
    ASSERT_TRUE(first.contains("result")) << method << first.dump();
    const char* field =
        method == "inspect.node_ids" ? "node_ids" : "ending_node_ids";
    ASSERT_EQ(first["result"][field].size(), kGeneralPageMaxEntries);
    ASSERT_TRUE(first["result"]["has_more"].get<bool>());
    ASSERT_TRUE(first["result"]["cursor"].is_string());
    Json continuation = valid_host_routed_params(method, session_id_);
    continuation["cursor"] = first["result"]["cursor"];
    continuation["offset"] = kGeneralPageMaxEntries;
    continuation["limit"] = kGeneralPageMaxEntries;
    const Json final = route(std::string(method), std::move(continuation));
    ASSERT_TRUE(final.contains("result")) << method << final.dump();
    ASSERT_EQ(final["result"][field], Json::array({7}));
    EXPECT_FALSE(final["result"]["has_more"].get<bool>());
    EXPECT_TRUE(final["result"]["cursor"].is_null());
    EXPECT_EQ(host_.call_count(method), 1u) << method;
  }
}

TEST_F(HostRoutedGraphStateProtocolTest,
       RejectsMalformedHostNodeListsAfterOneHostCall) {
  host_.set_node_ids({NodeId{1}, NodeId{-1}, NodeId{2}});
  Json response =
      route("inspect.node_ids",
            valid_host_routed_params("inspect.node_ids", session_id_));
  ASSERT_TRUE(response.contains("error")) << response.dump();
  EXPECT_EQ(response["error"]["domain"], "daemon");
  EXPECT_EQ(response["error"]["name"], "internal_error");
  EXPECT_EQ(host_.call_count("inspect.node_ids"), 1u);

  host_.reset_invocations();
  host_.set_ending_nodes({NodeId{-2}});
  response =
      route("inspect.ending_nodes",
            valid_host_routed_params("inspect.ending_nodes", session_id_));
  ASSERT_TRUE(response.contains("error")) << response.dump();
  EXPECT_EQ(response["error"]["domain"], "daemon");
  EXPECT_EQ(response["error"]["name"], "internal_error");
  EXPECT_EQ(host_.call_count("inspect.ending_nodes"), 1u);
}

TEST_F(HostRoutedGraphStateProtocolTest,
       TimingRoutesBoundNestedRowsAndRejectMalformedHostValues) {
  TimingSnapshot exact;
  exact.node_timings.reserve(kGeneralPageMaxEntries);
  for (std::size_t index = 0; index < kGeneralPageMaxEntries; ++index) {
    exact.node_timings.push_back(NodeTimingSnapshot{
        NodeId{static_cast<int>(index)}, "node", static_cast<double>(index),
        index % 2 == 0 ? "cpu" : "cache"});
  }
  exact.total_ms = 4096.5;
  host_.set_timing(exact);
  Json response = route("compute.timing", valid_host_routed_params(
                                              "compute.timing", session_id_));
  ASSERT_TRUE(response.contains("result")) << response.dump();
  ASSERT_EQ(response["result"]["node_timings"].size(), kGeneralPageMaxEntries);
  EXPECT_EQ(response["result"]["node_timings"].front()["node_id"], 0);
  EXPECT_EQ(response["result"]["node_timings"].back()["node_id"], 4095);
  EXPECT_EQ(response["result"]["total_ms"], 4096.5);
  EXPECT_EQ(host_.call_count("compute.timing"), 1u);

  exact.node_timings.push_back(
      NodeTimingSnapshot{NodeId{4096}, "last", 1.0, "cpu"});
  host_.set_timing(exact);
  host_.reset_invocations();
  response = route("compute.timing",
                   valid_host_routed_params("compute.timing", session_id_));
  ASSERT_TRUE(response.contains("error")) << response.dump();
  EXPECT_EQ(response["error"]["domain"], "protocol");
  EXPECT_EQ(response["error"]["name"], "response_too_large");
  EXPECT_EQ(host_.call_count("compute.timing"), 1u);

  TimingSnapshot negative_node;
  negative_node.node_timings.push_back(
      NodeTimingSnapshot{NodeId{-1}, "bad", 1.0, "cpu"});
  host_.set_timing(negative_node);
  host_.reset_invocations();
  response = route("compute.timing",
                   valid_host_routed_params("compute.timing", session_id_));
  EXPECT_EQ(response["error"]["domain"], "daemon");
  EXPECT_EQ(response["error"]["name"], "internal_error");
  EXPECT_EQ(host_.call_count("compute.timing"), 1u);

  TimingSnapshot invalid_utf8;
  invalid_utf8.node_timings.push_back(
      NodeTimingSnapshot{NodeId{1}, std::string("\xc3\x28", 2), 1.0, "cpu"});
  host_.set_timing(invalid_utf8);
  host_.reset_invocations();
  response = route("compute.timing",
                   valid_host_routed_params("compute.timing", session_id_));
  EXPECT_EQ(response["error"]["domain"], "daemon");
  EXPECT_EQ(response["error"]["name"], "internal_error");
  EXPECT_EQ(host_.call_count("compute.timing"), 1u);

  TimingSnapshot oversized_text;
  oversized_text.node_timings.push_back(NodeTimingSnapshot{
      NodeId{1}, std::string(kShortTextMaxBytes + 1, 'n'), 1.0, "cpu"});
  host_.set_timing(oversized_text);
  host_.reset_invocations();
  response = route("compute.timing",
                   valid_host_routed_params("compute.timing", session_id_));
  EXPECT_EQ(response["error"]["domain"], "protocol");
  EXPECT_EQ(response["error"]["name"], "response_too_large");
  EXPECT_EQ(host_.call_count("compute.timing"), 1u);
}

TEST_F(HostRoutedGraphStateProtocolTest,
       NonfiniteTimingAndLastIoValuesEncodeAsNull) {
  TimingSnapshot timing;
  timing.node_timings.push_back(NodeTimingSnapshot{
      NodeId{1}, "nan", std::numeric_limits<double>::quiet_NaN(), "cpu"});
  timing.node_timings.push_back(NodeTimingSnapshot{
      NodeId{2}, "infinity", std::numeric_limits<double>::infinity(), "cpu"});
  timing.total_ms = -std::numeric_limits<double>::infinity();
  host_.set_timing(timing);
  Json response = route("compute.timing", valid_host_routed_params(
                                              "compute.timing", session_id_));
  ASSERT_TRUE(response.contains("result")) << response.dump();
  EXPECT_TRUE(response["result"]["node_timings"][0]["elapsed_ms"].is_null());
  EXPECT_TRUE(response["result"]["node_timings"][1]["elapsed_ms"].is_null());
  EXPECT_TRUE(response["result"]["total_ms"].is_null());

  host_.set_last_io_time(std::numeric_limits<double>::quiet_NaN());
  host_.reset_invocations();
  response =
      route("compute.last_io_time",
            valid_host_routed_params("compute.last_io_time", session_id_));
  ASSERT_TRUE(response.contains("result")) << response.dump();
  EXPECT_TRUE(response["result"]["last_io_time_ms"].is_null());
  EXPECT_EQ(host_.call_count("compute.last_io_time"), 1u);
}

TEST_F(HostRoutedGraphStateProtocolTest,
       TimingFramePreflightAcceptsFitAndRejectsEscapedAggregateOnce) {
  TimingSnapshot near_frame;
  const std::size_t source_bytes = kLargeTextMaxBytes - 8192U;
  near_frame.node_timings.push_back(NodeTimingSnapshot{
      NodeId{1}, "first", 1.0, std::string(source_bytes, 's')});
  near_frame.node_timings.push_back(NodeTimingSnapshot{
      NodeId{2}, "second", 2.0, std::string(source_bytes, 's')});
  host_.set_timing(std::move(near_frame));

  Json response = route("compute.timing", valid_host_routed_params(
                                              "compute.timing", session_id_));
  ASSERT_TRUE(response.contains("result")) << response.dump();
  EXPECT_FALSE(response.contains("error"));
  EXPECT_EQ(host_.call_count("compute.timing"), 1u);

  TimingSnapshot escaped_aggregate;
  constexpr std::size_t kEscapedSourceBytes = 3U * 1024U * 1024U;
  for (int node = 0; node < 3; ++node) {
    escaped_aggregate.node_timings.push_back(NodeTimingSnapshot{
        NodeId{node}, "escaped", 1.0, std::string(kEscapedSourceBytes, '\n')});
  }
  host_.set_timing(std::move(escaped_aggregate));
  host_.reset_invocations();

  response = route("compute.timing",
                   valid_host_routed_params("compute.timing", session_id_));
  ASSERT_TRUE(response.contains("error")) << response.dump();
  EXPECT_FALSE(response.contains("result"));
  EXPECT_EQ(response["error"]["domain"], "protocol");
  EXPECT_EQ(response["error"]["name"], "response_too_large");
  EXPECT_EQ(host_.call_count("compute.timing"), 1u);
}

TEST_F(HostRoutedGraphStateProtocolTest,
       DirtyRoutesBoundTopLevelAndNestedCollections) {
  DirtyRegionInspectionSnapshot exact;
  exact.sources.reserve(kGeneralPageMaxEntries);
  for (std::size_t index = 0; index < kGeneralPageMaxEntries; ++index) {
    exact.sources.push_back(DirtySourceSnapshot{NodeId{static_cast<int>(index)},
                                                DirtyDomain::HighPrecision,
                                                DirtySourceLifecycleState::Idle,
                                                index,
                                                {}});
  }
  host_.set_dirty_region(exact);
  Json response = route("dirty.begin",
                        valid_host_routed_params("dirty.begin", session_id_));
  ASSERT_TRUE(response.contains("result")) << response.dump();
  EXPECT_EQ(response["result"]["sources"].size(), kGeneralPageMaxEntries);
  EXPECT_EQ(host_.call_count("dirty.begin"), 1u);

  exact.sources.push_back(DirtySourceSnapshot{NodeId{4096},
                                              DirtyDomain::HighPrecision,
                                              DirtySourceLifecycleState::Idle,
                                              4096,
                                              {}});
  host_.set_dirty_region(exact);
  host_.reset_invocations();
  response = route("dirty.update",
                   valid_host_routed_params("dirty.update", session_id_));
  ASSERT_TRUE(response.contains("error")) << response.dump();
  EXPECT_EQ(response["error"]["domain"], "protocol");
  EXPECT_EQ(response["error"]["name"], "response_too_large");
  EXPECT_EQ(host_.call_count("dirty.update"), 1u);

  DirtyRegionInspectionSnapshot nested;
  nested.sources.push_back(DirtySourceSnapshot{
      NodeId{1}, DirtyDomain::RealTime, DirtySourceLifecycleState::Updating, 1,
      std::vector<PixelRect>(kGeneralPageMaxEntries, PixelRect{1, 2, 3, 4})});
  host_.set_dirty_region(nested);
  host_.reset_invocations();
  response =
      route("dirty.end", valid_host_routed_params("dirty.end", session_id_));
  ASSERT_TRUE(response.contains("result")) << response.dump();
  EXPECT_EQ(response["result"]["sources"][0]["source_rois"].size(),
            kGeneralPageMaxEntries);
  EXPECT_EQ(host_.call_count("dirty.end"), 1u);

  nested.sources.front().source_rois.push_back(PixelRect{5, 6, 7, 8});
  host_.set_dirty_region(nested);
  host_.reset_invocations();
  response = route("dirty.begin",
                   valid_host_routed_params("dirty.begin", session_id_));
  ASSERT_TRUE(response.contains("error")) << response.dump();
  EXPECT_EQ(response["error"]["domain"], "protocol");
  EXPECT_EQ(response["error"]["name"], "response_too_large");
  EXPECT_EQ(host_.call_count("dirty.begin"), 1u);
}

TEST_F(HostRoutedGraphStateProtocolTest,
       DirtyFramePreflightAggregatesNestedRoisAcrossCollectionsOnce) {
  constexpr std::size_t kRows = 2048;
  const std::vector<PixelRect> rois(128, PixelRect{1, 2, 3, 4});
  DirtyRegionInspectionSnapshot aggregate;
  aggregate.sources.reserve(kRows);
  for (std::size_t index = 0; index < kRows; ++index) {
    aggregate.sources.push_back(DirtySourceSnapshot{
        NodeId{static_cast<int>(index)}, DirtyDomain::HighPrecision,
        DirtySourceLifecycleState::Settled, index, rois});
    aggregate.actual_dirty_rois.emplace(static_cast<int>(kRows + index), rois);
  }
  aggregate.dirty_tiles.push_back(DirtyTileSnapshot{
      NodeId{5000}, DirtyDomain::RealTime, 1, 2, 64, PixelRect{1, 2, 3, 4}});
  aggregate.dirty_monolithic_nodes.push_back(DirtyMonolithicRegionSnapshot{
      NodeId{5001}, DirtyDomain::RealTime, PixelRect{1, 2, 3, 4}, false});
  aggregate.edge_mappings.push_back(DirtyEdgeMappingSnapshot{
      NodeId{5000}, NodeId{5001}, DirtyDomain::RealTime, PixelRect{1, 2, 3, 4},
      PixelRect{5, 6, 7, 8}, DirtyEdgeDirection::ForwardAffected});
  host_.set_dirty_region(std::move(aggregate));

  const Json response = route(
      "dirty.begin", valid_host_routed_params("dirty.begin", session_id_));
  ASSERT_TRUE(response.contains("error")) << response.dump();
  EXPECT_FALSE(response.contains("result"));
  EXPECT_EQ(response["error"]["domain"], "protocol");
  EXPECT_EQ(response["error"]["name"], "response_too_large");
  EXPECT_EQ(host_.call_count("dirty.begin"), 1u);
}

TEST_F(HostRoutedGraphStateProtocolTest,
       EveryDirtyCollectionRejects4097EntriesAfterOneHostCall) {
  /**
   * @brief Verifies one oversized Host snapshot is rejected without retry.
   * @param snapshot Complete configured Host value.
   * @param label Diagnostic collection label for assertion output.
   * @throws std::bad_alloc if snapshot or response handling cannot allocate.
   */
  const auto expect_response_too_large =
      [&](DirtyRegionInspectionSnapshot snapshot, std::string_view label) {
        host_.set_dirty_region(std::move(snapshot));
        host_.reset_invocations();
        const Json response =
            route("dirty.begin",
                  valid_host_routed_params("dirty.begin", session_id_));
        ASSERT_TRUE(response.contains("error")) << label << response.dump();
        EXPECT_EQ(response["error"]["domain"], "protocol") << label;
        EXPECT_EQ(response["error"]["name"], "response_too_large") << label;
        EXPECT_EQ(host_.call_count("dirty.begin"), 1u) << label;
      };

  DirtyRegionInspectionSnapshot tiles;
  tiles.dirty_tiles.resize(kGeneralPageMaxEntries + 1);
  for (std::size_t index = 0; index < tiles.dirty_tiles.size(); ++index) {
    tiles.dirty_tiles[index].node = NodeId{static_cast<int>(index)};
  }
  expect_response_too_large(std::move(tiles), "dirty_tiles");

  DirtyRegionInspectionSnapshot monolithic;
  monolithic.dirty_monolithic_nodes.resize(kGeneralPageMaxEntries + 1);
  for (std::size_t index = 0; index < monolithic.dirty_monolithic_nodes.size();
       ++index) {
    monolithic.dirty_monolithic_nodes[index].node =
        NodeId{static_cast<int>(index)};
  }
  expect_response_too_large(std::move(monolithic), "dirty_monolithic_nodes");

  DirtyRegionInspectionSnapshot actual_rows;
  for (std::size_t index = 0; index < kGeneralPageMaxEntries + 1; ++index) {
    actual_rows.actual_dirty_rois.emplace(static_cast<int>(index),
                                          std::vector<PixelRect>{});
  }
  expect_response_too_large(std::move(actual_rows), "actual_dirty_rois");

  DirtyRegionInspectionSnapshot edges;
  edges.edge_mappings.resize(kGeneralPageMaxEntries + 1);
  for (std::size_t index = 0; index < edges.edge_mappings.size(); ++index) {
    edges.edge_mappings[index].from_node = NodeId{static_cast<int>(index)};
    edges.edge_mappings[index].to_node = NodeId{static_cast<int>(index)};
  }
  expect_response_too_large(std::move(edges), "edge_mappings");

  DirtyRegionInspectionSnapshot nested_actual;
  nested_actual.actual_dirty_rois.emplace(
      1, std::vector<PixelRect>(kGeneralPageMaxEntries + 1,
                                PixelRect{1, 2, 3, 4}));
  expect_response_too_large(std::move(nested_actual), "actual_dirty_rois.rois");
}

TEST_F(HostRoutedGraphStateProtocolTest,
       DirtyMutatorSuccessEncodingFailuresDoNotRetryHost) {
  DirtyRegionInspectionSnapshot invalid_enum;
  invalid_enum.sources.push_back(
      DirtySourceSnapshot{NodeId{1},
                          static_cast<DirtyDomain>(999),
                          DirtySourceLifecycleState::Idle,
                          1,
                          {}});
  host_.set_dirty_region(invalid_enum);
  Json response = route("dirty.begin",
                        valid_host_routed_params("dirty.begin", session_id_));
  ASSERT_TRUE(response.contains("error")) << response.dump();
  EXPECT_EQ(response["error"]["domain"], "daemon");
  EXPECT_EQ(response["error"]["name"], "internal_error");
  EXPECT_EQ(host_.call_count("dirty.begin"), 1u);

  DirtyRegionInspectionSnapshot invalid_node;
  invalid_node.dirty_tiles.push_back(
      DirtyTileSnapshot{NodeId{-1}, DirtyDomain::HighPrecision, 0, 0, 64,
                        PixelRect{0, 0, 64, 64}});
  host_.set_dirty_region(invalid_node);
  host_.reset_invocations();
  response = route("dirty.update",
                   valid_host_routed_params("dirty.update", session_id_));
  ASSERT_TRUE(response.contains("error")) << response.dump();
  EXPECT_EQ(response["error"]["domain"], "daemon");
  EXPECT_EQ(response["error"]["name"], "internal_error");
  EXPECT_EQ(host_.call_count("dirty.update"), 1u);

  DirtyRegionInspectionSnapshot oversized;
  oversized.sources.resize(kGeneralPageMaxEntries + 1);
  for (std::size_t index = 0; index < oversized.sources.size(); ++index) {
    oversized.sources[index].node = NodeId{static_cast<int>(index)};
  }
  host_.set_dirty_region(std::move(oversized));
  host_.reset_invocations();
  response =
      route("dirty.end", valid_host_routed_params("dirty.end", session_id_));
  ASSERT_TRUE(response.contains("error")) << response.dump();
  EXPECT_EQ(response["error"]["domain"], "protocol");
  EXPECT_EQ(response["error"]["name"], "response_too_large");
  EXPECT_EQ(host_.call_count("dirty.end"), 1u);
}

TEST_F(HostRoutedGraphStateProtocolTest,
       ReturnedYamlHonorsInclusiveBoundAndRejectsOversizeAfterHost) {
  host_.set_node_yaml(std::string(kLargeTextMaxBytes, 'y'));
  Json response =
      route("graph.node_yaml.get",
            valid_host_routed_params("graph.node_yaml.get", session_id_));
  ASSERT_TRUE(response.contains("result")) << response.dump();
  EXPECT_EQ(
      response["result"]["yaml_text"].get_ref<const std::string&>().size(),
      kLargeTextMaxBytes);
  EXPECT_EQ(host_.call_count("graph.node_yaml.get"), 1u);

  host_.set_node_yaml(std::string(kLargeTextMaxBytes + 1, 'y'));
  host_.reset_invocations();
  response =
      route("graph.node_yaml.get",
            valid_host_routed_params("graph.node_yaml.get", session_id_));
  ASSERT_TRUE(response.contains("error")) << response.dump();
  EXPECT_EQ(response["error"]["domain"], "protocol");
  EXPECT_EQ(response["error"]["name"], "response_too_large");
  EXPECT_EQ(host_.call_count("graph.node_yaml.get"), 1u);
}

TEST_F(HostRoutedGraphStateProtocolTest,
       HostMutexSerializesCallsAndCloseWaitsForAdmittedCall) {
  std::mutex gate_mutex;
  std::condition_variable gate_changed;
  bool reload_entered = false;
  bool release_reload = false;
  host_.set_call_hook([&](std::string_view method) {
    if (method != "graph.reload") {
      return;
    }
    std::unique_lock<std::mutex> lock(gate_mutex);
    reload_entered = true;
    gate_changed.notify_all();
    gate_changed.wait(lock, [&] { return release_reload; });
  });

  {
    auto reload_future = std::async(std::launch::async, [&] {
      return route("graph.reload",
                   valid_host_routed_params("graph.reload", session_id_));
    });
    std::future<Json> timing_future;
    ScopedProtocolGateRelease release_guard(gate_mutex, gate_changed,
                                            release_reload);
    bool observed_reload = false;
    {
      std::unique_lock<std::mutex> lock(gate_mutex);
      observed_reload = gate_changed.wait_for(lock, std::chrono::seconds(2),
                                              [&] { return reload_entered; });
    }
    if (!observed_reload) {
      ADD_FAILURE() << "reload Host hook was not entered before deadline";
      return;
    }

    std::promise<void> timing_started;
    std::future<void> timing_entered = timing_started.get_future();
    timing_future = std::async(std::launch::async, [&] {
      timing_started.set_value();
      return route("compute.timing",
                   valid_host_routed_params("compute.timing", session_id_));
    });
    timing_entered.wait();
    EXPECT_EQ(timing_future.wait_for(std::chrono::milliseconds(25)),
              std::future_status::timeout);
    EXPECT_EQ(host_.call_count("compute.timing"), 0u);

    const Json ping_response = route("daemon.ping", Json::object());
    EXPECT_TRUE(ping_response.contains("result")) << ping_response.dump();
    if (ping_response.contains("result")) {
      EXPECT_TRUE(ping_response["result"]["pong"].get<bool>());
    }

    release_guard.release();
    const Json reload_response = reload_future.get();
    const Json timing_response = timing_future.get();
    ASSERT_TRUE(reload_response.contains("result")) << reload_response.dump();
    ASSERT_TRUE(timing_response.contains("result")) << timing_response.dump();

    const std::vector<::ps::testing::IpcHostInvocation> serialized_calls =
        host_.invocations();
    ASSERT_EQ(serialized_calls.size(), 2u);
    EXPECT_EQ(serialized_calls[0].method, "graph.reload");
    EXPECT_EQ(serialized_calls[1].method, "compute.timing");
  }

  {
    std::lock_guard<std::mutex> lock(gate_mutex);
    reload_entered = false;
    release_reload = false;
  }
  host_.reset_invocations();

  {
    auto reload_future = std::async(std::launch::async, [&] {
      return route("graph.reload",
                   valid_host_routed_params("graph.reload", session_id_));
    });
    std::future<Json> close_future;
    ScopedProtocolGateRelease release_guard(gate_mutex, gate_changed,
                                            release_reload);
    bool observed_reload = false;
    {
      std::unique_lock<std::mutex> lock(gate_mutex);
      observed_reload = gate_changed.wait_for(lock, std::chrono::seconds(2),
                                              [&] { return reload_entered; });
    }
    if (!observed_reload) {
      ADD_FAILURE()
          << "second reload Host hook was not entered before deadline";
      return;
    }

    close_future = std::async(std::launch::async, [&] {
      return route("graph.close", Json{{"session_id", session_id_}});
    });
    EXPECT_EQ(close_future.wait_for(std::chrono::milliseconds(25)),
              std::future_status::timeout);
    EXPECT_EQ(host_.call_count("graph.close"), 0u);

    release_guard.release();
    const Json reload_response = reload_future.get();
    const Json close_response = close_future.get();
    ASSERT_TRUE(reload_response.contains("result")) << reload_response.dump();
    ASSERT_TRUE(close_response.contains("result")) << close_response.dump();
    EXPECT_TRUE(close_response["result"]["closed"].get<bool>());

    const std::vector<::ps::testing::IpcHostInvocation> close_calls =
        host_.invocations();
    ASSERT_EQ(close_calls.size(), 2u);
    EXPECT_EQ(close_calls[0].method, "graph.reload");
    EXPECT_EQ(close_calls[1].method, "graph.close");
  }

  host_.set_call_hook({});
  host_.reset_invocations();
  const Json after_close =
      route("compute.timing",
            valid_host_routed_params("compute.timing", session_id_));
  ASSERT_TRUE(after_close.contains("error")) << after_close.dump();
  EXPECT_EQ(after_close["error"]["domain"], "graph");
  EXPECT_EQ(after_close["error"]["name"], "not_found");
  EXPECT_TRUE(host_.invocations().empty());
}

/**
 * @brief Verifies one complete internal enum label table transactionally.
 *
 * @tparam Enum Public enum type handled by the overloaded codec.
 * @tparam Count Number of stable version 1 labels.
 * @param mappings Expected enum/label pairs.
 * @return Nothing.
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

TEST(FrameCodec, StageDeadlinePathPreservesFragmentedFrame) {
  int descriptors[2] = {-1, -1};
  ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, descriptors), 0);
  UniqueFd reader(descriptors[0]);
  UniqueFd writer(descriptors[1]);
  const std::string payload = "deadline-fragmented-payload";
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
  const FrameReadResult result = read_frame_with_stage_deadlines(
      reader.get(), std::chrono::steady_clock::now() + std::chrono::seconds(1),
      std::chrono::seconds(1));
  peer.join();
  EXPECT_TRUE(write_ok);
  EXPECT_EQ(result.state, FrameReadState::Complete) << result.message;
  EXPECT_EQ(result.payload, payload);
}

TEST(FrameCodec, StageDeadlinePathPreservesEofAndLengthValidation) {
  int clean_pair[2] = {-1, -1};
  ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, clean_pair), 0);
  UniqueFd clean_reader(clean_pair[0]);
  UniqueFd clean_writer(clean_pair[1]);
  clean_writer.reset();
  EXPECT_EQ(read_frame_with_stage_deadlines(
                clean_reader.get(),
                std::chrono::steady_clock::now() + std::chrono::seconds(1),
                std::chrono::seconds(1))
                .state,
            FrameReadState::CleanEof);

  int truncated_pair[2] = {-1, -1};
  ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, truncated_pair), 0);
  UniqueFd truncated_reader(truncated_pair[0]);
  UniqueFd truncated_writer(truncated_pair[1]);
  const unsigned char partial[] = {0, 0};
  ASSERT_TRUE(
      send_all_for_test(truncated_writer.get(), partial, sizeof(partial)));
  truncated_writer.reset();
  EXPECT_EQ(read_frame_with_stage_deadlines(
                truncated_reader.get(),
                std::chrono::steady_clock::now() + std::chrono::seconds(1),
                std::chrono::seconds(1))
                .state,
            FrameReadState::Truncated);

  int invalid_pair[2] = {-1, -1};
  ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, invalid_pair), 0);
  UniqueFd invalid_reader(invalid_pair[0]);
  UniqueFd invalid_writer(invalid_pair[1]);
  const std::uint32_t invalid_network_length = htonl(0);
  ASSERT_TRUE(send_all_for_test(
      invalid_writer.get(),
      reinterpret_cast<const unsigned char*>(&invalid_network_length),
      sizeof(invalid_network_length)));
  EXPECT_EQ(read_frame_with_stage_deadlines(
                invalid_reader.get(),
                std::chrono::steady_clock::now() + std::chrono::seconds(1),
                std::chrono::seconds(1))
                .state,
            FrameReadState::InvalidLength);
}

TEST(FrameCodec, StageDeadlinePathReportsHeaderAndPayloadExpiry) {
  constexpr auto kExpiry = std::chrono::milliseconds(20);
  int header_pair[2] = {-1, -1};
  ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, header_pair), 0);
  UniqueFd header_reader(header_pair[0]);
  UniqueFd header_writer(header_pair[1]);
  const unsigned char partial_header = 0;
  ASSERT_TRUE(send_all_for_test(header_writer.get(), &partial_header,
                                sizeof(partial_header)));
  const FrameReadResult header_result = read_frame_with_stage_deadlines(
      header_reader.get(), std::chrono::steady_clock::now() + kExpiry,
      std::chrono::seconds(1));
  EXPECT_EQ(header_result.state, FrameReadState::IoError);
  EXPECT_EQ(header_result.message, "frame header deadline exceeded");

  int payload_pair[2] = {-1, -1};
  ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, payload_pair), 0);
  UniqueFd payload_reader(payload_pair[0]);
  UniqueFd payload_writer(payload_pair[1]);
  const std::uint32_t payload_network_length = htonl(8);
  ASSERT_TRUE(send_all_for_test(
      payload_writer.get(),
      reinterpret_cast<const unsigned char*>(&payload_network_length),
      sizeof(payload_network_length)));
  const unsigned char partial_payload = '{';
  ASSERT_TRUE(send_all_for_test(payload_writer.get(), &partial_payload,
                                sizeof(partial_payload)));
  const FrameReadResult payload_result = read_frame_with_stage_deadlines(
      payload_reader.get(),
      std::chrono::steady_clock::now() + std::chrono::seconds(1), kExpiry);
  EXPECT_EQ(payload_result.state, FrameReadState::IoError);
  EXPECT_EQ(payload_result.message, "frame payload deadline exceeded");
}

TEST(FrameCodec, DeadlineWritePreservesCompletePartialTransfer) {
  int descriptors[2] = {-1, -1};
  ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, descriptors), 0);
  UniqueFd writer(descriptors[0]);
  UniqueFd reader(descriptors[1]);
  const int send_buffer_bytes = 1024;
  ASSERT_EQ(::setsockopt(writer.get(), SOL_SOCKET, SO_SNDBUF,
                         &send_buffer_bytes, sizeof(send_buffer_bytes)),
            0);
  const std::string payload(1024U * 1024U, 'x');
  FrameReadResult read_result;
  std::thread peer([&] { read_result = read_frame(reader.get()); });
  const FrameWriteResult write_result = write_frame_before_deadline(
      writer.get(), payload,
      std::chrono::steady_clock::now() + std::chrono::seconds(2));
  peer.join();
  ASSERT_TRUE(write_result.ok) << write_result.message;
  ASSERT_EQ(read_result.state, FrameReadState::Complete) << read_result.message;
  EXPECT_EQ(read_result.payload, payload);
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
      R"({"protocol_version":2,"id":"a","id":"b","method":"daemon.ping","params":{}})"));
  EXPECT_TRUE(duplicated["id"].is_null());
  EXPECT_EQ(duplicated["error"]["name"], "invalid_request");

  Json nested_duplicate = parse_response(router.route(
      R"({"protocol_version":2,"id":"nested","method":"daemon.ping","params":{"value":1,"value":2}})"));
  EXPECT_EQ(nested_duplicate["id"], "nested");
  EXPECT_EQ(nested_duplicate["error"]["name"], "invalid_request");
}

TEST(ProtocolEnvelope, NegotiatesVersionAndRejectsUnknownMethodAndSession) {
  std::unique_ptr<Host> host = create_embedded_host();
  ASSERT_NE(host, nullptr);
  RequestRouter router(*host, "0.1.0");
  Json unsupported = parse_response(router.route(
      R"({"protocol_version":1,"id":"v","method":"daemon.ping","params":{}})"));
  EXPECT_EQ(unsupported["id"], "v");
  EXPECT_EQ(unsupported["error"]["name"], "unsupported_protocol");
  EXPECT_EQ(unsupported["error"]["supported_versions"], Json::array({2}));

  Json compute_without_params = parse_response(router.route(
      R"({"protocol_version":2,"id":"m","method":"compute.submit","params":{}})"));
  EXPECT_EQ(compute_without_params["error"]["name"], "invalid_params");
  Json unknown = parse_response(router.route(
      R"({"protocol_version":2,"id":"u","method":"compute.cancel","params":{}})"));
  EXPECT_EQ(unknown["error"]["name"], "method_not_found");

  Json missing = parse_response(router.route(
      R"({"protocol_version":2,"id":"s","method":"inspect.graph","params":{"session_id":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"}})"));
  EXPECT_EQ(missing["error"]["domain"], "graph");
  EXPECT_EQ(missing["error"]["code"], 2);
  EXPECT_EQ(missing["error"]["name"], "not_found");
}

/**
 * @brief Locks version metadata, dispatch coverage, and route admission.
 *
 * @throws std::bad_alloc if test or router value construction cannot allocate.
 * @note The independent normative array detects omissions, duplicates, and
 *       aliases. Routing every normative entry detects unsupported claims,
 *       while representative nonmembers exercise the generic allowlist that
 *       prevents any unadvertised route family from becoming reachable.
 */
TEST(ProtocolContract,
     AdvertisesAndRoutesExactlyTheNormativeVersionTwoMethods) {
  static constexpr std::array<std::string_view, 60> kExpectedMethods = {
      "cache.cache_all_nodes",
      "cache.clear_all",
      "cache.clear_drive",
      "cache.clear_memory",
      "cache.free_transient",
      "cache.synchronize_disk",
      "compute.last_error",
      "compute.last_io_time",
      "compute.release",
      "compute.result",
      "compute.status",
      "compute.submit",
      "compute.timing",
      "daemon.ping",
      "daemon.version",
      "dirty.begin",
      "dirty.end",
      "dirty.update",
      "events.drain",
      "execution.configure_defaults",
      "execution.description",
      "execution.info",
      "execution.replace",
      "execution.trace",
      "execution.types",
      "graph.clear",
      "graph.close",
      "graph.list",
      "graph.load",
      "graph.node_yaml.get",
      "graph.node_yaml.set",
      "graph.reload",
      "graph.save",
      "inspect.compute_planning",
      "inspect.dependency_tree",
      "inspect.dirty_region",
      "inspect.ending_nodes",
      "inspect.graph",
      "inspect.node",
      "inspect.node_ids",
      "inspect.recent_compute_planning",
      "inspect.roi_backward",
      "inspect.roi_forward",
      "inspect.traversal_details",
      "inspect.traversal_orders",
      "inspect.trees_containing_node",
      "plugins.load_report",
      "plugins.ops_combined_keys",
      "plugins.ops_combined_sources",
      "plugins.ops_sources",
      "plugins.seed_builtins",
      "plugins.unload_all",
      "policy.configure_defaults",
      "policy.description",
      "policy.info",
      "policy.load",
      "policy.loaded_plugins",
      "policy.replace",
      "policy.scan",
      "policy.types"};
  static constexpr std::array<std::string_view, 18> kUnadvertisedMethods = {
      "compute.async",
      "compute.cancel",
      "compute.image",
      "daemon.shutdown",
      "events.subscribe",
      "graph.open",
      "host.call",
      "inspect.nodes",
      "plugins.load",
      "scheduler.configure_defaults",
      "scheduler.description",
      "scheduler.info",
      "scheduler.load",
      "scheduler.loaded_plugins",
      "scheduler.replace",
      "scheduler.scan",
      "scheduler.trace",
      "scheduler.types"};

  ::ps::testing::IpcHostSpy host;
  RequestRouter router(host, "contract-test");
  /**
   * @brief Routes one empty-params method through a complete envelope.
   * @param method Candidate version 2 method or unadvertised control name.
   * @return Parsed correlated router response owned by the test.
   * @throws std::bad_alloc or std::runtime_error when construction or parsing
   *         fails.
   * @note The empty params object intentionally distinguishes family
   *       recognition from `method_not_found`; method-specific validation or
   *       default spy behavior may otherwise produce either response branch.
   */
  const auto call = [&router](std::string_view method) {
    return parse_response(router.route(Json{
        {"protocol_version", kProtocolVersion},
        {"id", std::string(method)},
        {"method", std::string(method)},
        {"params", Json::object()}}.dump()));
  };

  const Json version = call("daemon.version");
  ASSERT_TRUE(version.contains("result")) << version.dump();
  ASSERT_TRUE(version["result"].value("methods", Json()).is_array());
  const std::vector<std::string> advertised =
      version["result"]["methods"].get<std::vector<std::string>>();
  ASSERT_EQ(advertised.size(), kExpectedMethods.size());
  EXPECT_TRUE(std::is_sorted(advertised.begin(), advertised.end()));
  EXPECT_EQ(std::adjacent_find(advertised.begin(), advertised.end()),
            advertised.end());
  for (std::size_t index = 0; index < kExpectedMethods.size(); ++index) {
    EXPECT_EQ(advertised[index], kExpectedMethods[index]) << "index " << index;
  }

  for (std::string_view method : kExpectedMethods) {
    const Json response = call(method);
    ASSERT_TRUE(response.contains("result") || response.contains("error"))
        << method << ": " << response.dump();
    if (response.contains("error")) {
      EXPECT_NE(response["error"]["name"], "method_not_found")
          << method << ": " << response.dump();
    }
  }

  for (std::string_view method : kUnadvertisedMethods) {
    const Json response = call(method);
    ASSERT_TRUE(response.contains("error"))
        << method << ": " << response.dump();
    EXPECT_EQ(response["error"]["name"], "method_not_found")
        << method << ": " << response.dump();
  }
}

TEST(ProtocolEnvelope, TreatsEveryNonV2IntegerAsUnsupported) {
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
    EXPECT_EQ(response["error"]["supported_versions"], Json::array({2}));
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

TEST(OpaqueIdCodec, ValidatesOneExactSharedVersionTwoShape) {
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
      {kExecutionTraceMinLimit, kExecutionTraceMaxLimit},
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

TEST(EnumCodec, RoundTripsEveryDefinedVersionTwoLabel) {
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
  expect_enum_codec(std::array<std::pair<HostExecutionTraceAction, const char*>,
                               9>{{
      {HostExecutionTraceAction::AssignInitial, "assign_initial"},
      {HostExecutionTraceAction::Execute, "execute"},
      {HostExecutionTraceAction::ExecuteTile, "execute_tile"},
      {HostExecutionTraceAction::ExecuteDirtySource, "execute_dirty_source"},
      {HostExecutionTraceAction::ExecuteDirtyDownstreamNode,
       "execute_dirty_downstream_node"},
      {HostExecutionTraceAction::ExecuteDirtyDownstreamTile,
       "execute_dirty_downstream_tile"},
      {HostExecutionTraceAction::SkipStaleGeneration, "skip_stale_generation"},
      {HostExecutionTraceAction::RethrowException, "rethrow_exception"},
      {HostExecutionTraceAction::Unknown, "unknown"},
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
      {"protocol_version", kProtocolVersion},
      {"id", exact_id},
      {"method", "unknown"},
      {"params", Json::object()}}.dump()));
  EXPECT_EQ(exact_id_response["id"], exact_id);
  EXPECT_EQ(exact_id_response["error"]["name"], "method_not_found");

  const Json long_id_response = parse_response(router.route(Json{
      {"protocol_version", kProtocolVersion},
      {"id", std::string(kRequestTextMaxBytes + 1, 'i')},
      {"method", "unknown"},
      {"params", Json::object()}}.dump()));
  EXPECT_TRUE(long_id_response["id"].is_null());
  EXPECT_EQ(long_id_response["error"]["name"], "invalid_request");

  const Json exact_method_response = parse_response(router.route(Json{
      {"protocol_version", kProtocolVersion},
      {"id", "method-exact"},
      {"method", std::string(kRequestTextMaxBytes, 'm')},
      {"params", Json::object()}}.dump()));
  EXPECT_EQ(exact_method_response["error"]["name"], "method_not_found");
  const Json long_method_response = parse_response(router.route(Json{
      {"protocol_version", kProtocolVersion},
      {"id", "method-long"},
      {"method", std::string(kRequestTextMaxBytes + 1, 'm')},
      {"params", Json::object()}}.dump()));
  EXPECT_EQ(long_method_response["id"], "method-long");
  EXPECT_EQ(long_method_response["error"]["name"], "invalid_request");
}

TEST(ProtocolParams, IgnoresUnknownFieldsButStillValidatesKnownFields) {
  ScopedTempDirectory temp("ps-ipc-params");
  std::unique_ptr<Host> host = create_embedded_host();
  ASSERT_NE(host, nullptr);
  RequestRouter router(*host, "0.1.0");
  ScopedRequestRouterRuntime runtime(router, temp);
  for (const std::string& method :
       {std::string("daemon.ping"), std::string("daemon.version"),
        std::string("graph.list")}) {
    const Json response = parse_response(router.route(Json{
        {"protocol_version", kProtocolVersion},
        {"id", method},
        {"method", method},
        {"future_envelope", Json{{"nested", true}}},
        {"params",
         Json{{"future_field", Json{{"nested", true}}}}}}.dump()));
    EXPECT_TRUE(response.contains("result")) << response.dump();
  }
  const Json malformed_known = parse_response(router.route(Json{
      {"protocol_version", kProtocolVersion},
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
      {"protocol_version", kProtocolVersion},
      {"id", "session-exact"},
      {"method", "graph.load"},
      {"params", Json{{"session_name", std::string(kShortTextMaxBytes, 's')},
                      {"root_dir", temp.path().string()}}}}.dump()));
  EXPECT_TRUE(exact_session.contains("result") ||
              exact_session["error"]["name"] != "invalid_params")
      << exact_session.dump();

  const Json long_session = parse_response(router.route(Json{
      {"protocol_version", kProtocolVersion},
      {"id", "session-long"},
      {"method", "graph.load"},
      {"params",
       Json{{"session_name", std::string(kShortTextMaxBytes + 1, 's')},
            {"root_dir", temp.path().string()}}}}.dump()));
  EXPECT_EQ(long_session["error"]["name"], "invalid_params");

  const std::string exact_path = "/" + std::string(kPathTextMaxBytes - 1, 'p');
  const Json accepted_path = parse_response(router.route(Json{
      {"protocol_version", kProtocolVersion},
      {"id", "path-exact"},
      {"method", "graph.load"},
      {"params", Json{{"session_name", "path_exact"},
                      {"root_dir", exact_path}}}}.dump()));
  EXPECT_TRUE(accepted_path.contains("result") ||
              accepted_path["error"]["name"] != "invalid_params")
      << accepted_path.dump();

  const Json long_path = parse_response(router.route(Json{
      {"protocol_version", kProtocolVersion},
      {"id", "path-long"},
      {"method", "graph.load"},
      {"params", Json{{"session_name", "path_long"},
                      {"root_dir", exact_path + "p"}}}}.dump()));
  EXPECT_EQ(long_path["error"]["name"], "invalid_params");

  const Json nul_session = parse_response(router.route(Json{
      {"protocol_version", kProtocolVersion},
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
      Json{{"protocol_version", kProtocolVersion},
           {"id", "unsafe-name"},
           {"method", "graph.load"},
           {"params", Json{{"session_name", "../unsafe"},
                           {"root_dir", untouched_root.string()}}}},
      Json{{"protocol_version", kProtocolVersion},
           {"id", "relative-root"},
           {"method", "graph.load"},
           {"params",
            Json{{"session_name", "safe"}, {"root_dir", "relative/root"}}}},
      Json{{"protocol_version", kProtocolVersion},
           {"id", "negative-node"},
           {"method", "inspect.node"},
           {"params", Json{{"session_id", valid_session_id}, {"node_id", -1}}}},
      Json{{"protocol_version", kProtocolVersion},
           {"id", "wrong-node-type"},
           {"method", "inspect.node"},
           {"params",
            Json{{"session_id", valid_session_id}, {"node_id", "one"}}}},
      Json{{"protocol_version", kProtocolVersion},
           {"id", "overflow-node"},
           {"method", "inspect.node"},
           {"params", Json{{"session_id", valid_session_id},
                           {"node_id", std::uint64_t{4294967296ULL}}}}},
      Json{{"protocol_version", kProtocolVersion},
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

/**
 * @brief Proves an exact Graph parse failure releases the IPC name for retry.
 * @return Nothing.
 * @throws Nothing when the router preserves `invalid_yaml`, rolls back its
 * daemon-side name reservation, and accepts the corrected document; GoogleTest
 * records mismatches.
 * @note The first request reaches the real embedded Host rather than failing
 * protocol parameter validation.
 */
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
      {"protocol_version", kProtocolVersion},
      {"id", "first"},
      {"method", "graph.load"},
      {"params", params}}.dump()));
  EXPECT_EQ(first["error"]["domain"], "graph");
  EXPECT_EQ(first["error"]["name"], "invalid_yaml");
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
      {"protocol_version", kProtocolVersion},
      {"id", "second"},
      {"method", "graph.load"},
      {"params", params}}.dump()));
  ASSERT_TRUE(second.contains("result"));
  EXPECT_EQ(second["result"]["session_name"], "retry_session");
  router.begin_shutdown();
  router.finish_shutdown();
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
  /**
   * @brief One expected stable version 1 error identity.
   *
   * @throws Nothing for construction and scalar access.
   * @note The row is test-owned immutable table data used only to verify exact
   *       domain/code/name pairing; it carries no runtime status ownership.
   */
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
  ScopedRequestRouterRuntime runtime(router, temp);
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

/**
 * @brief Completes one injected pending connect without a second attempt.
 *
 * @return Nothing; GoogleTest assertions report attempt count, completion, or
 *         descriptor-flag mismatch.
 * @throws std::bad_alloc or std::system_error if callback, diagnostic, or
 *         socketpair setup cannot complete.
 * @note A connected socketpair makes writable/SO_ERROR completion
 *       deterministic while the injected attempt returns `EINPROGRESS` once.
 */
TEST(UnixSocketConnect, CompletesOnePendingAttemptAndRestoresBlockingFlags) {
  int descriptors[2] = {-1, -1};
  ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, descriptors), 0);
  UniqueFd connecting(descriptors[0]);
  UniqueFd peer(descriptors[1]);
  const int original_flags = ::fcntl(connecting.get(), F_GETFL, 0);
  ASSERT_GE(original_flags, 0);
  ASSERT_EQ(::fcntl(connecting.get(), F_SETFL, original_flags & ~O_NONBLOCK),
            0);
  int attempts = 0;
  std::string message;
  const bool connected = connect_prepared_unix_socket_with_attempt(
      connecting.get(), "/injected/pending.sock", [] { return false; },
      [&attempts](int, const void*, std::size_t) {
        ++attempts;
        errno = EINPROGRESS;
        return -1;
      },
      &message);

  EXPECT_TRUE(connected) << message;
  EXPECT_EQ(attempts, 1);
  const int restored_flags = ::fcntl(connecting.get(), F_GETFL, 0);
  ASSERT_GE(restored_flags, 0);
  EXPECT_EQ(restored_flags & O_NONBLOCK, 0);
}

/**
 * @brief Retries Linux backlog `EAGAIN` on the same unconnected descriptor.
 *
 * @return Nothing; GoogleTest assertions report logical completion, attempt
 *         count, descriptor identity, or restored-flag mismatch.
 * @throws std::bad_alloc or std::system_error if socket or callback storage
 *         cannot be created.
 * @note The injected sequence is `EAGAIN`, `EAGAIN`, success. No connection is
 *       marked pending and no frame/RPC exists during the two 10-ms slices.
 */
TEST(UnixSocketConnect, RetriesBacklogEagainOnSameFdUntilSuccess) {
  std::string creation_message;
  UniqueFd connecting = create_unix_stream_socket(&creation_message);
  ASSERT_TRUE(connecting) << creation_message;
  const int original_flags = ::fcntl(connecting.get(), F_GETFL, 0);
  ASSERT_GE(original_flags, 0);
  std::vector<int> attempted_fds;
  std::string message;
  const bool connected = connect_prepared_unix_socket_with_attempt(
      connecting.get(), "/injected/backlog.sock", [] { return false; },
      [&attempted_fds](int fd, const void*, std::size_t) {
        attempted_fds.push_back(fd);
        if (attempted_fds.size() < 3U) {
          errno = EAGAIN;
          return -1;
        }
        return 0;
      },
      &message);

  EXPECT_TRUE(connected) << message;
  ASSERT_EQ(attempted_fds.size(), 3U);
  EXPECT_TRUE(std::all_of(attempted_fds.begin(), attempted_fds.end(),
                          [&](int fd) { return fd == connecting.get(); }));
  const int restored_flags = ::fcntl(connecting.get(), F_GETFL, 0);
  ASSERT_GE(restored_flags, 0);
  EXPECT_EQ(restored_flags, original_flags);
}

/**
 * @brief Re-enters a real Linux AF_UNIX connect after backlog `EAGAIN`.
 *
 * A backlog-zero listener retains one filler connection. A raw nonblocking
 * connect on the target descriptor must then return Linux's real `EAGAIN`.
 * The production helper receives that same descriptor, enters its bounded
 * zero-descriptor poll slice, and retries only after the test accepts the
 * filler connection.
 *
 * @return Nothing; GoogleTest assertions report kernel errno, retry-wait
 *         entry, same-descriptor completion, peer state, or flag mismatch.
 * @throws std::bad_alloc, std::runtime_error, or std::system_error if socket,
 *         path, callback, or thread setup cannot complete.
 * @note Non-Linux platforms skip this kernel-specific regression because
 *       their AF_UNIX backlog-full classification is not `EAGAIN`.
 */
TEST(UnixSocketConnect, LinuxRealBacklogEagainReentersConnectOnSameDescriptor) {
#if !defined(__linux__)
  GTEST_SKIP() << "Linux AF_UNIX backlog EAGAIN semantics required";
#else
  ScopedTempDirectory temp("ps-ipc-backlog");
  const std::string socket_path = (temp.path() / "server.sock").string();
  UniqueFd listener = create_test_listener(socket_path);
  ASSERT_EQ(::listen(listener.get(), 0), 0);

  sockaddr_un address{};
  address.sun_family = AF_UNIX;
  std::memcpy(address.sun_path, socket_path.c_str(), socket_path.size() + 1);
  const socklen_t address_length = static_cast<socklen_t>(
      offsetof(sockaddr_un, sun_path) + socket_path.size() + 1);

  std::string creation_message;
  UniqueFd filler = create_unix_stream_socket(&creation_message);
  ASSERT_TRUE(filler) << creation_message;
  ASSERT_EQ(::connect(filler.get(), reinterpret_cast<sockaddr*>(&address),
                      address_length),
            0);

  UniqueFd target = create_unix_stream_socket(&creation_message);
  ASSERT_TRUE(target) << creation_message;
  const int target_fd = target.get();
  const int original_flags = ::fcntl(target_fd, F_GETFL, 0);
  ASSERT_GE(original_flags, 0);
  ASSERT_EQ(::fcntl(target_fd, F_SETFL, original_flags | O_NONBLOCK), 0);
  ASSERT_EQ(::connect(target_fd, reinterpret_cast<sockaddr*>(&address),
                      address_length),
            -1);
  ASSERT_EQ(errno, EAGAIN);
  ASSERT_EQ(::fcntl(target_fd, F_SETFL, original_flags), 0);

  std::mutex retry_mutex;
  std::condition_variable retry_changed;
  std::size_t predicate_calls = 0;
  bool connector_done = false;
  std::atomic<bool> stop{false};
  bool connected = false;
  std::string message;
  std::thread connector([&] {
    connected = connect_prepared_unix_socket(
        target_fd, socket_path,
        [&] {
          std::lock_guard<std::mutex> lock(retry_mutex);
          ++predicate_calls;
          retry_changed.notify_all();
          return stop.load();
        },
        &message);
    {
      std::lock_guard<std::mutex> lock(retry_mutex);
      connector_done = true;
    }
    retry_changed.notify_all();
  });

  bool retry_wait_entered = false;
  {
    std::unique_lock<std::mutex> lock(retry_mutex);
    retry_wait_entered = retry_changed.wait_for(
        lock, std::chrono::seconds(2), [&] { return predicate_calls >= 2U; });
  }
  UniqueFd accepted_filler(::accept(listener.get(), nullptr, nullptr));
  bool completed_in_time = false;
  {
    std::unique_lock<std::mutex> lock(retry_mutex);
    completed_in_time = retry_changed.wait_for(lock, std::chrono::seconds(2),
                                               [&] { return connector_done; });
  }
  if (!completed_in_time) {
    stop.store(true);
    (void)::shutdown(target_fd, SHUT_RDWR);
    (void)::shutdown(listener.get(), SHUT_RDWR);
  }
  connector.join();

  UniqueFd accepted_target;
  if (connected) {
    pollfd pending{listener.get(), POLLIN, 0};
    if (::poll(&pending, 1, 500) == 1 && (pending.revents & POLLIN) != 0) {
      accepted_target.reset(::accept(listener.get(), nullptr, nullptr));
    }
  }

  ASSERT_TRUE(retry_wait_entered);
  ASSERT_TRUE(accepted_filler);
  ASSERT_TRUE(completed_in_time);
  EXPECT_TRUE(connected) << message;
  ASSERT_TRUE(accepted_target);
  EXPECT_EQ(target.get(), target_fd);
  sockaddr_un peer{};
  socklen_t peer_size = sizeof(peer);
  EXPECT_EQ(
      ::getpeername(target_fd, reinterpret_cast<sockaddr*>(&peer), &peer_size),
      0);
  const int restored_flags = ::fcntl(target_fd, F_GETFL, 0);
  ASSERT_GE(restored_flags, 0);
  EXPECT_EQ(restored_flags, original_flags);
#endif
}

/**
 * @brief Accepts `EISCONN` after a same-fd backlog transient.
 *
 * @return Nothing; GoogleTest assertions report logical completion, attempt
 *         count, or restored-flag mismatch.
 * @throws std::bad_alloc or std::system_error if socket or callbacks cannot be
 *         created.
 * @note The injected `EAGAIN` then `EISCONN` sequence models connection state
 *       becoming visible while the same logical retry is scheduled.
 */
TEST(UnixSocketConnect, AcceptsEisconnAfterBacklogTransient) {
  std::string creation_message;
  UniqueFd connecting = create_unix_stream_socket(&creation_message);
  ASSERT_TRUE(connecting) << creation_message;
  const int original_flags = ::fcntl(connecting.get(), F_GETFL, 0);
  ASSERT_GE(original_flags, 0);
  int attempts = 0;
  std::string message;
  const bool connected = connect_prepared_unix_socket_with_attempt(
      connecting.get(), "/injected/backlog.sock", [] { return false; },
      [&attempts](int, const void*, std::size_t) {
        ++attempts;
        errno = attempts == 1 ? EAGAIN : EISCONN;
        return -1;
      },
      &message);

  EXPECT_TRUE(connected) << message;
  EXPECT_EQ(attempts, 2);
  const int restored_flags = ::fcntl(connecting.get(), F_GETFL, 0);
  ASSERT_GE(restored_flags, 0);
  EXPECT_EQ(restored_flags, original_flags);
}

/**
 * @brief Gives stop precedence over an immediate successful connect attempt.
 *
 * @return Nothing; GoogleTest assertions report stop classification, attempt
 *         count, or restored-flag mismatch.
 * @throws std::bad_alloc or std::system_error if socket or callbacks cannot be
 *         created.
 * @note The predicate becomes true only after the injected attempt returns
 *       zero, closing the helper's immediate-success stop race independently
 *       of the Client's outer latch check.
 */
TEST(UnixSocketConnect, StopWinsImmediateConnectCompletion) {
  std::string creation_message;
  UniqueFd connecting = create_unix_stream_socket(&creation_message);
  ASSERT_TRUE(connecting) << creation_message;
  const int original_flags = ::fcntl(connecting.get(), F_GETFL, 0);
  ASSERT_GE(original_flags, 0);
  std::size_t predicate_calls = 0;
  int attempts = 0;
  std::string message;
  const bool connected = connect_prepared_unix_socket_with_attempt(
      connecting.get(), "/injected/immediate.sock",
      [&predicate_calls] { return ++predicate_calls >= 2U; },
      [&attempts](int, const void*, std::size_t) {
        ++attempts;
        return 0;
      },
      &message);

  EXPECT_FALSE(connected);
  EXPECT_EQ(message, "connect interrupted");
  EXPECT_EQ(attempts, 1);
  EXPECT_GE(predicate_calls, 2U);
  const int restored_flags = ::fcntl(connecting.get(), F_GETFL, 0);
  ASSERT_GE(restored_flags, 0);
  EXPECT_EQ(restored_flags, original_flags);
}

/**
 * @brief Gives a latched stop precedence over simultaneous writability.
 *
 * @return Nothing; GoogleTest assertions report completion, attempt count, or
 *         restored-flag mismatch.
 * @throws std::bad_alloc or std::system_error if socketpair, callbacks, or
 *         diagnostics cannot be created.
 * @note The connected socketpair is immediately writable. The predicate turns
 *       true only after poll returns, proving the helper checks stop before
 *       accepting an otherwise successful `SO_ERROR` completion.
 */
TEST(UnixSocketConnect, StopWinsWritablePendingCompletion) {
  int descriptors[2] = {-1, -1};
  ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, descriptors), 0);
  UniqueFd connecting(descriptors[0]);
  UniqueFd peer(descriptors[1]);
  const int original_flags = ::fcntl(connecting.get(), F_GETFL, 0);
  ASSERT_GE(original_flags, 0);
  ASSERT_EQ(::fcntl(connecting.get(), F_SETFL, original_flags & ~O_NONBLOCK),
            0);
  std::size_t predicate_calls = 0;
  int attempts = 0;
  std::string message;
  const bool connected = connect_prepared_unix_socket_with_attempt(
      connecting.get(), "/injected/pending.sock",
      [&predicate_calls] { return ++predicate_calls >= 3U; },
      [&attempts](int, const void*, std::size_t) {
        ++attempts;
        errno = EINPROGRESS;
        return -1;
      },
      &message);

  EXPECT_FALSE(connected);
  EXPECT_EQ(message, "connect interrupted");
  EXPECT_EQ(attempts, 1);
  EXPECT_GE(predicate_calls, 3U);
  const int restored_flags = ::fcntl(connecting.get(), F_GETFL, 0);
  ASSERT_GE(restored_flags, 0);
  EXPECT_EQ(restored_flags & O_NONBLOCK, 0);
}

/**
 * @brief Interrupts a pending writable wait without retrying connect.
 *
 * A socketpair sender is filled to `EAGAIN`, then an injected connect attempt
 * reports `EINPROGRESS`. The stop predicate and descriptor shutdown race while
 * the state machine is in its bounded poll loop; interruption must win over a
 * wake with `SO_ERROR==0`, preserve one attempt, and restore original flags.
 *
 * @return Nothing; GoogleTest assertions report stop latency, attempt count,
 *         diagnostic, or descriptor-flag mismatch.
 * @throws std::bad_alloc, std::runtime_error, or std::system_error if socket,
 *         callback, diagnostic, or thread setup cannot complete.
 * @note The peer remains open and never drains bytes, making POLLOUT
 * unavailable until shutdown wakes the 10-ms interruption slice.
 */
TEST(UnixSocketConnect, InterruptsPendingAttemptAndRestoresBlockingFlags) {
  int descriptors[2] = {-1, -1};
  ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, descriptors), 0);
  UniqueFd connecting(descriptors[0]);
  UniqueFd peer(descriptors[1]);
  const int original_flags = ::fcntl(connecting.get(), F_GETFL, 0);
  ASSERT_GE(original_flags, 0);
  ASSERT_EQ(::fcntl(connecting.get(), F_SETFL, original_flags | O_NONBLOCK), 0);
  std::array<std::byte, 4096> bytes{};
  while (true) {
    const ssize_t sent =
        ::send(connecting.get(), bytes.data(), bytes.size(), 0);
    if (sent > 0) {
      continue;
    }
    ASSERT_EQ(sent, -1);
    ASSERT_TRUE(errno == EAGAIN || errno == EWOULDBLOCK);
    break;
  }
  ASSERT_EQ(::fcntl(connecting.get(), F_SETFL, original_flags & ~O_NONBLOCK),
            0);

  std::atomic<bool> stop{false};
  std::mutex state_mutex;
  std::condition_variable state_changed;
  std::size_t predicate_calls = 0;
  bool pending_wait_entered = false;
  int attempts = 0;
  bool connected = true;
  std::string message;
  std::thread connector([&] {
    connected = connect_prepared_unix_socket_with_attempt(
        connecting.get(), "/injected/pending.sock",
        [&] {
          std::lock_guard<std::mutex> lock(state_mutex);
          ++predicate_calls;
          if (predicate_calls >= 2U) {
            pending_wait_entered = true;
            state_changed.notify_all();
          }
          return stop.load();
        },
        [&attempts](int, const void*, std::size_t) {
          ++attempts;
          errno = EINPROGRESS;
          return -1;
        },
        &message);
  });
  bool entered = false;
  {
    std::unique_lock<std::mutex> lock(state_mutex);
    entered = state_changed.wait_for(lock, std::chrono::seconds(2),
                                     [&] { return pending_wait_entered; });
  }
  const auto stop_started = std::chrono::steady_clock::now();
  stop.store(true);
  (void)::shutdown(connecting.get(), SHUT_RDWR);
  connector.join();
  const auto stop_elapsed = std::chrono::steady_clock::now() - stop_started;

  ASSERT_TRUE(entered);
  EXPECT_FALSE(connected);
  EXPECT_EQ(message, "connect interrupted");
  EXPECT_EQ(attempts, 1);
  EXPECT_LT(stop_elapsed, std::chrono::milliseconds(500));
  const int restored_flags = ::fcntl(connecting.get(), F_GETFL, 0);
  ASSERT_GE(restored_flags, 0);
  EXPECT_EQ(restored_flags & O_NONBLOCK, 0);
}

/**
 * @brief Latches adapter stop before a Client publishes its descriptor.
 *
 * @return Nothing; GoogleTest assertions report status, ownership, or an
 *         unexpected listener connection.
 * @throws std::bad_alloc, std::runtime_error, or std::system_error if temporary
 *         listener, Client, or diagnostics cannot be created.
 * @note This closes the register/descriptor-publication race: the later
 *       logical connection observes the private latch before any connect
 *       syscall.
 */
TEST(ClientLifecycle, InterruptBeforeConnectPreventsDescriptorPublication) {
  ScopedTempDirectory temp("ps-ipc-stop-connect");
  const std::string socket_path = (temp.path() / "server.sock").string();
  UniqueFd listener = create_test_listener(socket_path);
  Client client;
  ClientInterruptAccess::interrupt(&client);
  const OperationStatus status = client.connect(socket_path);

  EXPECT_FALSE(status.ok);
  EXPECT_EQ(status.domain, OperationErrorDomain::Transport);
  EXPECT_EQ(status.code, 1);
  EXPECT_EQ(status.name, "connect_failed");
  EXPECT_EQ(status.message, "connect interrupted");
  EXPECT_FALSE(client.connected());
  pollfd descriptor{listener.get(), POLLIN, 0};
  EXPECT_EQ(::poll(&descriptor, 1, 100), 0);
}

TEST(ClientLifecycle, RejectsUncorrelatedResponseAndClosesIdempotently) {
  for (const auto& response :
       std::vector<std::pair<std::string, std::int32_t>>{{"wrong-id", 2},
                                                         {"client-1", 1}}) {
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
      R"({"protocol_version":2,"id":"client-1","error":{"domain":"protocol","code":18446744073709551615,"name":"future","message":"diagnostic"}})"};
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

/**
 * @brief Rejects an excessive typed execution worker request before wire IO.
 *
 * The same Client is first queried while disconnected, then connected to a
 * scripted peer that expects only one exact-limit request. The excessive
 * request must preserve the historical disconnected-status precedence before
 * connect, become local protocol `invalid_params` after connect, and leave the
 * connection available for the accepted request.
 *
 * @return Nothing; GoogleTest records status, request-count, or value errors.
 * @throws std::bad_alloc, std::runtime_error, or std::system_error if socket,
 *         script, request, or peer-thread setup cannot complete.
 * @note The peer has no reply slot for the rejected call. Observing one request
 *       carrying eight therefore proves the connected rejection sent no frame
 *       without relying on timing or diagnostic message text.
 */
TEST(ClientExecutionDefaults,
     RejectsConnectedAboveLimitBeforeWireAndRetainsConnection) {
  ScopedTempDirectory temp("ps-ipc-exec-limit");
  const std::string socket_path = (temp.path() / "server.sock").string();
  UniqueFd listener = create_test_listener(socket_path);
  const std::vector<ScriptedClientReply> replies = {
      {"execution.configure_defaults", Json::object()}};
  std::vector<Json> requests;
  bool served = false;
  std::thread peer([&] {
    served = serve_scripted_client_replies(listener.get(), replies, &requests);
  });

  Client client;
  HostExecutionConfig excessive;
  excessive.hp_type = "cpu";
  excessive.rt_type = "serial_debug";
  excessive.worker_count = kExecutionWorkerRequestMax + 1U;
  const VoidResult disconnected =
      client.configure_execution_defaults(excessive);
  ASSERT_TRUE(client.connect(socket_path).ok);
  const VoidResult rejected = client.configure_execution_defaults(excessive);

  HostExecutionConfig accepted = excessive;
  accepted.worker_count = kExecutionWorkerRequestMax;
  const VoidResult accepted_result =
      client.configure_execution_defaults(accepted);
  const bool connected_after_accepted = client.connected();
  client.disconnect();
  const VoidResult disconnected_again =
      client.configure_execution_defaults(excessive);
  peer.join();

  ASSERT_FALSE(disconnected.status.ok);
  EXPECT_EQ(disconnected.status.domain, OperationErrorDomain::Transport);
  EXPECT_EQ(disconnected.status.code, 2);
  EXPECT_EQ(disconnected.status.name, "not_connected");
  ASSERT_FALSE(rejected.status.ok);
  EXPECT_EQ(rejected.status.domain, OperationErrorDomain::Protocol);
  EXPECT_EQ(rejected.status.code, kInvalidParamsCode);
  EXPECT_EQ(rejected.status.name, "invalid_params");
  EXPECT_TRUE(accepted_result.status.ok) << accepted_result.status.message;
  EXPECT_TRUE(served);
  ASSERT_EQ(requests.size(), 1U);
  EXPECT_EQ(requests.front()["params"]["worker_count"],
            kExecutionWorkerRequestMax);
  EXPECT_TRUE(connected_after_accepted);
  ASSERT_FALSE(disconnected_again.status.ok);
  EXPECT_EQ(disconnected_again.status.domain, OperationErrorDomain::Transport);
  EXPECT_EQ(disconnected_again.status.code, 2);
  EXPECT_EQ(disconnected_again.status.name, "not_connected");
  EXPECT_FALSE(client.connected());
}

/**
 * @brief Dispatches the exact typed version 2 Client surface once per method.
 *
 * A scripted local peer expects all 60 normative methods in canonical order
 * and returns one recoverable daemon error for each call. The test invokes
 * every public typed Client operation with representative values, then checks
 * request envelopes and selected nested parameters at the wire boundary.
 *
 * @return Nothing; GoogleTest assertions report dispatch or schema mismatch.
 * @throws std::bad_alloc, std::runtime_error, or std::system_error if socket,
 *         script, request capture, or peer-thread setup cannot complete.
 * @note Scripted errors intentionally bypass success-result decoding so this
 *       test isolates symbol coverage, exact method mapping, and no-retry
 *       dispatch from the method-specific codec tests.
 */
TEST(ClientSurface, ExposesAndDispatchesExactTypedVersionTwoMethodsOnce) {
  ScopedTempDirectory temp("ps-ipc-surface");
  const std::string socket_path = (temp.path() / "server.sock").string();
  UniqueFd listener = create_test_listener(socket_path);
  const Json scripted_error{{"domain", "daemon"},
                            {"code", kJobNotFoundCode},
                            {"name", "job_not_found"},
                            {"message", "scripted typed-call failure"}};
  std::vector<ScriptedClientReply> replies;
  replies.reserve(kVersionTwoMethodNames.size());
  for (std::string_view method : kVersionTwoMethodNames) {
    replies.push_back(
        ScriptedClientReply{std::string(method), scripted_error, true});
  }
  std::vector<Json> requests;
  bool served = false;
  std::thread peer([&] {
    served = serve_scripted_client_replies(listener.get(), replies, &requests);
  });

  Client client;
  ASSERT_TRUE(client.connect(socket_path).ok);
  const IpcSessionId session_id{std::string(32, 'a')};
  const ps::ipc::ComputeRequestId compute_id{std::string(32, 'b')};
  const DeliveryLeaseId delivery_id{std::string(32, 'c')};
  const PixelRect roi{-1, 2, 3, 4};
  const std::vector<std::string> directories{"/tmp/plugins"};
  HostPolicyConfig policy_config;
  policy_config.interactive_type = "interactive";
  policy_config.throughput_type = "throughput";
  HostExecutionConfig execution_config;
  execution_config.hp_type = "cpu";
  execution_config.rt_type = "serial_debug";
  execution_config.worker_count = 3;
  ComputeSubmitRequest compute_request;
  compute_request.session_id = session_id;
  compute_request.node = NodeId{7};
  compute_request.cache.precision = "float32";
  compute_request.intent = ComputeIntent::GlobalHighPrecision;
  compute_request.dirty_roi = roi;
  GraphLoadRequest graph_request;
  graph_request.session = GraphSessionId{"surface"};
  graph_request.root_dir = "/tmp";

  (void)client.cache_all_nodes(session_id, "float32");
  (void)client.clear_cache(session_id);
  (void)client.clear_drive_cache(session_id);
  (void)client.clear_memory_cache(session_id);
  (void)client.free_transient_memory(session_id);
  (void)client.synchronize_disk_cache(session_id, "float32");
  (void)client.last_error(session_id);
  (void)client.last_io_time(session_id);
  (void)client.release_compute(compute_id, delivery_id);
  (void)client.compute_result(compute_id);
  (void)client.compute_status(compute_id);
  (void)client.submit_compute(compute_request);
  (void)client.timing(session_id);
  (void)client.ping();
  (void)client.version();
  (void)client.begin_dirty_source(session_id, NodeId{7},
                                  DirtyDomain::HighPrecision, roi);
  (void)client.end_dirty_source(session_id, NodeId{7},
                                DirtyDomain::HighPrecision);
  (void)client.update_dirty_source(session_id, NodeId{7},
                                   DirtyDomain::HighPrecision, roi);
  (void)client.drain_compute_events(session_id, 1);
  (void)client.configure_execution_defaults(execution_config);
  (void)client.execution_description("cpu");
  (void)client.execution_info(session_id, ComputeIntent::GlobalHighPrecision);
  (void)client.replace_execution(session_id, ComputeIntent::GlobalHighPrecision,
                                 "serial_debug");
  (void)client.execution_trace(session_id, 0, 1);
  (void)client.execution_available_types();
  (void)client.clear_graph(session_id);
  (void)client.close_graph(session_id);
  (void)client.list_graphs();
  (void)client.load_graph(graph_request);
  (void)client.get_node_yaml(session_id, NodeId{7});
  (void)client.set_node_yaml(session_id, NodeId{7}, "id: 7");
  (void)client.reload_graph(session_id, "/tmp/reload.yaml");
  (void)client.save_graph(session_id, "/tmp/save.yaml");
  (void)client.compute_planning_snapshot(session_id);
  (void)client.inspect_dependency_tree(session_id, NodeId{7}, true);
  (void)client.dirty_region_snapshot(session_id);
  (void)client.ending_nodes(session_id);
  (void)client.inspect_graph(session_id);
  (void)client.inspect_node(session_id, NodeId{7});
  (void)client.list_node_ids(session_id);
  (void)client.recent_compute_planning_snapshots(session_id);
  (void)client.project_roi_backward(session_id, NodeId{7}, roi, NodeId{3});
  (void)client.project_roi(session_id, NodeId{3}, roi, NodeId{7});
  (void)client.traversal_details(session_id);
  (void)client.traversal_orders(session_id);
  (void)client.trees_containing_node(session_id, NodeId{7});
  (void)client.plugins_load_report(directories);
  (void)client.ops_combined_keys();
  (void)client.ops_combined_sources();
  (void)client.ops_sources();
  (void)client.seed_builtin_ops();
  (void)client.plugins_unload_all();
  (void)client.configure_policy_defaults(policy_config);
  (void)client.policy_description("fixture_policy");
  (void)client.policy_info(PolicyClass::Interactive);
  (void)client.policy_load("/tmp/policy.so");
  (void)client.policy_loaded_plugins();
  (void)client.replace_policy(PolicyClass::Interactive, "fixture_policy");
  (void)client.policy_scan(directories);
  (void)client.policy_available_types();
  peer.join();

  ASSERT_TRUE(served);
  ASSERT_EQ(requests.size(), kVersionTwoMethodNames.size());
  for (std::size_t index = 0; index < requests.size(); ++index) {
    EXPECT_EQ(requests[index]["method"], kVersionTwoMethodNames[index]);
    EXPECT_EQ(requests[index]["protocol_version"], kProtocolVersion);
    EXPECT_TRUE(requests[index]["params"].is_object());
  }
  const Json& release_params = requests[8]["params"];
  EXPECT_EQ(release_params["compute_id"], compute_id.value);
  EXPECT_EQ(release_params["delivery_id"], delivery_id.value);
  const Json& submit_params = requests[11]["params"];
  EXPECT_EQ(submit_params["session_id"], session_id.value);
  EXPECT_EQ(submit_params["node_id"], 7);
  EXPECT_EQ(submit_params["result_mode"], "status");
  EXPECT_TRUE(submit_params["cache"].is_object());
  EXPECT_TRUE(submit_params["execution"].is_object());
  EXPECT_TRUE(submit_params["telemetry"].is_object());
  EXPECT_EQ(requests[30]["params"]["yaml_text"], "id: 7");
  EXPECT_TRUE(client.connected());
}

/**
 * @brief Verifies aggregate-budget arithmetic at tiny and `size_t` ceilings.
 * @return Nothing; GoogleTest assertions report every failed invariant.
 * @throws std::bad_alloc if a rejection diagnostic cannot allocate.
 * @note The test allocates no page-sized buffer. It proves initial array
 *       brackets, entry overflow, byte overflow, and failed-admission
 *       transactionality directly through the private production budget.
 */
TEST(CollectionAggregateBudget,
     RejectsInitialAndSizeMaxOverflowWithoutMutatingCounters) {
  std::string message;
  CollectionAggregateBudget too_small(1, 1);
  EXPECT_FALSE(too_small.admit(CollectionPageMeasurement{}, &message));
  EXPECT_EQ(too_small.entries(), 0U);
  EXPECT_EQ(too_small.rows(), 0U);
  EXPECT_EQ(too_small.encoded_bytes(), 2U);

  const std::size_t maximum = std::numeric_limits<std::size_t>::max();
  CollectionAggregateBudget entry_budget(maximum, maximum);
  EXPECT_TRUE(entry_budget.admit(CollectionPageMeasurement{maximum, 0, 0, 0},
                                 &message));
  EXPECT_FALSE(
      entry_budget.admit(CollectionPageMeasurement{1, 0, 0, 0}, &message));
  EXPECT_EQ(entry_budget.entries(), maximum);
  EXPECT_EQ(entry_budget.rows(), 0U);
  EXPECT_EQ(entry_budget.encoded_bytes(), 2U);

  CollectionAggregateBudget byte_budget(maximum, maximum);
  EXPECT_TRUE(byte_budget.admit(
      CollectionPageMeasurement{1, 1, maximum - 2U, 0}, &message));
  EXPECT_FALSE(
      byte_budget.admit(CollectionPageMeasurement{1, 1, 1, 0}, &message));
  EXPECT_EQ(byte_budget.entries(), 1U);
  EXPECT_EQ(byte_budget.rows(), 1U);
  EXPECT_EQ(byte_budget.encoded_bytes(), maximum);
}

/**
 * @brief Advances stable graph-list paging by the actual decoded row count.
 *
 * The first scripted page contains one row despite the Client requesting the
 * general 4,096-row limit. The second request must carry the frozen cursor,
 * offset one, and limit one before both pages are published as one owned list.
 *
 * @return Nothing; GoogleTest assertions report paging or value mismatch.
 * @throws std::bad_alloc, std::runtime_error, or std::system_error if socket,
 *         script, aggregate, or peer-thread setup cannot complete.
 * @note The first request must contain no continuation fields. This directly
 *       guards frame-safe short first pages without exposing a public cursor.
 */
TEST(ClientCollectionAggregation, UsesStableCursorAndActualPageLength) {
  ScopedTempDirectory temp("ps-ipc-pages");
  const std::string socket_path = (temp.path() / "server.sock").string();
  UniqueFd listener = create_test_listener(socket_path);
  const std::string cursor(32, 'd');
  const std::vector<ScriptedClientReply> replies = {
      {"graph.list",
       Json{{"sessions", Json::array({Json{{"session_id", std::string(32, 'a')},
                                           {"session_name", "alpha"}}})},
            {"offset", 0},
            {"has_more", true},
            {"cursor", cursor}}},
      {"graph.list",
       Json{{"sessions", Json::array({Json{{"session_id", std::string(32, 'b')},
                                           {"session_name", "beta"}}})},
            {"offset", 1},
            {"has_more", false},
            {"cursor", nullptr}}}};
  std::vector<Json> requests;
  bool served = false;
  std::thread peer([&] {
    served = serve_scripted_client_replies(listener.get(), replies, &requests);
  });
  Client client;
  ASSERT_TRUE(client.connect(socket_path).ok);
  const IpcResult<std::vector<GraphSessionSummary>> result =
      client.list_graphs();
  peer.join();

  ASSERT_TRUE(served);
  ASSERT_TRUE(result.status.ok) << result.status.message;
  ASSERT_EQ(result.value.size(), 2U);
  EXPECT_EQ(result.value[0].session_name, "alpha");
  EXPECT_EQ(result.value[1].session_name, "beta");
  ASSERT_EQ(requests.size(), 2U);
  EXPECT_EQ(requests[0]["params"]["limit"], kGeneralPageMaxEntries);
  EXPECT_FALSE(requests[0]["params"].contains("cursor"));
  EXPECT_FALSE(requests[0]["params"].contains("offset"));
  EXPECT_EQ(requests[1]["params"]["cursor"], cursor);
  EXPECT_EQ(requests[1]["params"]["offset"], 1);
  EXPECT_EQ(requests[1]["params"]["limit"], 1);
}

/**
 * @brief Rejects a continuation page larger than its exact requested limit.
 *
 * The first graph-list page contains one row and therefore narrows the stable
 * continuation request to one row. An abnormal peer then returns two rows for
 * that `limit: 1` request. The Client must reject the oversized page before it
 * appends either row or publishes the earlier partial aggregate.
 *
 * @return Nothing; GoogleTest assertions report status or request mismatch.
 * @throws std::bad_alloc, std::runtime_error, or std::system_error if socket,
 *         script, aggregate, or peer-thread setup cannot complete.
 * @note Each scripted RPC is attempted exactly once. The validation is bound
 *       to the current request limit rather than only the protocol-wide 4,096
 *       row ceiling.
 */
TEST(ClientCollectionAggregation,
     RejectsPageLargerThanCurrentRequestLimitWithoutPartialValue) {
  ScopedTempDirectory temp("ps-ipc-overlim");
  const std::string socket_path = (temp.path() / "server.sock").string();
  UniqueFd listener = create_test_listener(socket_path);
  const std::string cursor(32, 'c');
  const std::vector<ScriptedClientReply> replies = {
      {"graph.list",
       Json{{"sessions", Json::array({Json{{"session_id", std::string(32, 'a')},
                                           {"session_name", "alpha"}}})},
            {"offset", 0},
            {"has_more", true},
            {"cursor", cursor}}},
      {"graph.list",
       Json{{"sessions", Json::array({Json{{"session_id", std::string(32, 'b')},
                                           {"session_name", "beta"}},
                                      Json{{"session_id", std::string(32, 'd')},
                                           {"session_name", "delta"}}})},
            {"offset", 1},
            {"has_more", false},
            {"cursor", nullptr}}}};
  std::vector<Json> requests;
  bool served = false;
  std::thread peer([&] {
    served = serve_scripted_client_replies(listener.get(), replies, &requests);
  });
  Client client;
  ASSERT_TRUE(client.connect(socket_path).ok);
  const IpcResult<std::vector<GraphSessionSummary>> result =
      client.list_graphs();
  peer.join();

  ASSERT_TRUE(served);
  EXPECT_FALSE(result.status.ok);
  EXPECT_EQ(result.status.domain, OperationErrorDomain::Protocol);
  EXPECT_EQ(result.status.code, kInvalidRequestCode);
  EXPECT_TRUE(result.value.empty());
  ASSERT_EQ(requests.size(), 2U);
  EXPECT_EQ(requests[1]["params"]["cursor"], cursor);
  EXPECT_EQ(requests[1]["params"]["offset"], 1);
  EXPECT_EQ(requests[1]["params"]["limit"], 1);
  EXPECT_TRUE(client.connected());
}

/**
 * @brief Rejects a changed continuation cursor transactionally.
 *
 * The scripted peer returns one accepted graph-list page and then changes the
 * frozen cursor while claiming another page remains. The Client must classify
 * the second response as a Protocol failure and discard the local aggregate.
 *
 * @return Nothing; GoogleTest assertions report status or request mismatch.
 * @throws std::bad_alloc, std::runtime_error, or std::system_error if socket,
 *         script, aggregate, or peer-thread setup cannot complete.
 * @note Both page attempts occur exactly once, no partial session list escapes,
 *       and the response-validation failure does not close the connection.
 */
TEST(ClientCollectionAggregation, RejectsUnstableCursorWithoutPartialValue) {
  ScopedTempDirectory temp("ps-ipc-badpage");
  const std::string socket_path = (temp.path() / "server.sock").string();
  UniqueFd listener = create_test_listener(socket_path);
  const std::vector<ScriptedClientReply> replies = {
      {"graph.list",
       Json{{"sessions", Json::array({Json{{"session_id", std::string(32, 'a')},
                                           {"session_name", "alpha"}}})},
            {"offset", 0},
            {"has_more", true},
            {"cursor", std::string(32, 'c')}}},
      {"graph.list",
       Json{{"sessions", Json::array({Json{{"session_id", std::string(32, 'b')},
                                           {"session_name", "beta"}}})},
            {"offset", 1},
            {"has_more", true},
            {"cursor", std::string(32, 'd')}}}};
  std::vector<Json> requests;
  bool served = false;
  std::thread peer([&] {
    served = serve_scripted_client_replies(listener.get(), replies, &requests);
  });
  Client client;
  ASSERT_TRUE(client.connect(socket_path).ok);
  const IpcResult<std::vector<GraphSessionSummary>> result =
      client.list_graphs();
  peer.join();

  ASSERT_TRUE(served);
  EXPECT_FALSE(result.status.ok);
  EXPECT_EQ(result.status.domain, OperationErrorDomain::Protocol);
  EXPECT_EQ(result.status.code, kInvalidRequestCode);
  EXPECT_TRUE(result.value.empty());
  EXPECT_EQ(requests.size(), 2U);
  EXPECT_TRUE(client.connected());
}

/**
 * @brief Preserves duplicate sorted strings within one stable result page.
 *
 * The scripted peer returns one non-decreasing policy-plugin label page
 * containing two equal adjacent values. The direct Client must decode and
 * aggregate that page once, preserve every original row, and stop at its
 * final-page marker.
 *
 * @return Nothing; GoogleTest assertions report value or request mismatch.
 * @throws std::bad_alloc, std::runtime_error, or std::system_error if
 *         deterministic socket, scripted response, or peer-thread setup fails.
 * @note Duplicate public string values are legal collection elements; this
 *       does not relax unique-key validation for map-backed collections.
 */
TEST(ClientCollectionAggregation,
     PreservesSinglePageDuplicateSortedStringsWithoutExtraRequest) {
  ScopedTempDirectory temp("ps-ipc-dup-page");
  const std::string socket_path = (temp.path() / "server.sock").string();
  UniqueFd listener = create_test_listener(socket_path);
  const std::vector<ScriptedClientReply> replies = {
      {"policy.loaded_plugins",
       Json{{"plugins", Json::array({"alpha", "alpha", "beta"})},
            {"offset", 0},
            {"has_more", false},
            {"cursor", nullptr}}}};
  std::vector<Json> requests;
  bool served = false;
  std::thread peer([&] {
    served = serve_scripted_client_replies(listener.get(), replies, &requests);
  });
  Client client;
  ASSERT_TRUE(client.connect(socket_path).ok);
  const IpcResult<std::vector<std::string>> result =
      client.policy_loaded_plugins();
  peer.join();

  ASSERT_TRUE(served);
  ASSERT_TRUE(result.status.ok) << result.status.message;
  EXPECT_EQ(result.value, (std::vector<std::string>{"alpha", "alpha", "beta"}));
  ASSERT_EQ(requests.size(), 1U);
  EXPECT_EQ(requests[0]["params"]["limit"], kGeneralPageMaxEntries);
  EXPECT_FALSE(requests[0]["params"].contains("cursor"));
  EXPECT_FALSE(requests[0]["params"].contains("offset"));
  EXPECT_TRUE(client.connected());
}

/**
 * @brief Preserves one duplicate spanning adjacent stable string pages.
 *
 * The scripted peer freezes a two-page policy-plugin label collection whose
 * final first-page value equals the next page's first value. The Client must
 * retain both rows in order, advance by the actual first-page length, and
 * perform exactly the two scripted RPC attempts.
 *
 * @return Nothing; GoogleTest assertions report value or request mismatch.
 * @throws std::bad_alloc, std::runtime_error, or std::system_error if
 *         deterministic socket, scripted response, or peer-thread setup fails.
 * @note Equality is legal at a page boundary because global sortedness is
 *       non-decreasing; a true ordering regression remains invalid.
 */
TEST(ClientCollectionAggregation,
     PreservesCrossPageDuplicateSortedStringsWithoutLossOrExtraRequest) {
  ScopedTempDirectory temp("ps-ipc-dup-edge");
  const std::string socket_path = (temp.path() / "server.sock").string();
  UniqueFd listener = create_test_listener(socket_path);
  const std::string cursor(32, 'e');
  const std::vector<ScriptedClientReply> replies = {
      {"policy.loaded_plugins",
       Json{{"plugins", Json::array({"alpha", "beta"})},
            {"offset", 0},
            {"has_more", true},
            {"cursor", cursor}}},
      {"policy.loaded_plugins",
       Json{{"plugins", Json::array({"beta", "gamma"})},
            {"offset", 2},
            {"has_more", false},
            {"cursor", nullptr}}}};
  std::vector<Json> requests;
  bool served = false;
  std::thread peer([&] {
    served = serve_scripted_client_replies(listener.get(), replies, &requests);
  });
  Client client;
  ASSERT_TRUE(client.connect(socket_path).ok);
  const IpcResult<std::vector<std::string>> result =
      client.policy_loaded_plugins();
  peer.join();

  ASSERT_TRUE(served);
  ASSERT_TRUE(result.status.ok) << result.status.message;
  EXPECT_EQ(result.value,
            (std::vector<std::string>{"alpha", "beta", "beta", "gamma"}));
  ASSERT_EQ(requests.size(), 2U);
  EXPECT_EQ(requests[1]["params"]["cursor"], cursor);
  EXPECT_EQ(requests[1]["params"]["offset"], 2);
  EXPECT_EQ(requests[1]["params"]["limit"], 2);
  EXPECT_TRUE(client.connected());
}

/**
 * @brief Rejects a genuine ordering regression in one sorted string page.
 *
 * The scripted peer returns a final page whose last value compares below the
 * preceding value. The Client must reject the response transactionally,
 * publish no partial vector, and make no request beyond that malformed page.
 *
 * @return Nothing; GoogleTest assertions report status or request mismatch.
 * @throws std::bad_alloc, std::runtime_error, or std::system_error if
 *         deterministic socket, scripted response, or peer-thread setup fails.
 * @note This guards the strict distinction between legal equality and illegal
 *       descending order without changing map-key uniqueness rules.
 */
TEST(ClientCollectionAggregation,
     RejectsDescendingSortedStringsWithoutPartialValueOrExtraRequest) {
  ScopedTempDirectory temp("ps-ipc-descend");
  const std::string socket_path = (temp.path() / "server.sock").string();
  UniqueFd listener = create_test_listener(socket_path);
  const std::vector<ScriptedClientReply> replies = {
      {"policy.loaded_plugins",
       Json{{"plugins", Json::array({"alpha", "gamma", "beta"})},
            {"offset", 0},
            {"has_more", false},
            {"cursor", nullptr}}}};
  std::vector<Json> requests;
  bool served = false;
  std::thread peer([&] {
    served = serve_scripted_client_replies(listener.get(), replies, &requests);
  });
  Client client;
  ASSERT_TRUE(client.connect(socket_path).ok);
  const IpcResult<std::vector<std::string>> result =
      client.policy_loaded_plugins();
  peer.join();

  ASSERT_TRUE(served);
  EXPECT_FALSE(result.status.ok);
  EXPECT_EQ(result.status.domain, OperationErrorDomain::Protocol);
  EXPECT_EQ(result.status.code, kInvalidRequestCode);
  EXPECT_TRUE(result.value.empty());
  EXPECT_EQ(requests.size(), 1U);
  EXPECT_TRUE(client.connected());
}

/**
 * @brief Accepts exactly 262,144 recursive traversal entries across two pages.
 * @return Nothing; GoogleTest assertions report protocol or value mismatch.
 * @throws std::bad_alloc or std::runtime_error if socket/test data setup fails.
 * @note Only 64 outer branches are returned. Nested node-id vectors supply the
 *       remaining entries, proving outer row count is not the quota proxy.
 */
TEST(ClientCollectionAggregation,
     AcceptsExactRecursiveEntryBoundaryAcrossPages) {
  ScopedTempDirectory temp("ps-ipc-entry");
  const std::string socket_path = (temp.path() / "server.sock").string();
  UniqueFd listener = create_test_listener(socket_path);
  const IpcSessionId session_id{std::string(32, 'a')};
  const std::string cursor(32, 'f');
  std::vector<std::size_t> first_counts(32, kGeneralPageMaxEntries);
  std::vector<std::size_t> final_counts(32, kGeneralPageMaxEntries);
  final_counts.back() = 4032;
  const std::vector<ScriptedClientReply> replies = {
      {"inspect.traversal_orders",
       traversal_order_page_result(session_id, 0, first_counts, 0, true,
                                   cursor)},
      {"inspect.traversal_orders",
       traversal_order_page_result(session_id, 32, final_counts, 32, false,
                                   cursor)}};
  std::vector<Json> requests;
  bool served = false;
  std::thread peer([&] {
    served = serve_scripted_client_replies(listener.get(), replies, &requests);
  });
  Client client;
  ASSERT_TRUE(client.connect(socket_path).ok);
  const IpcResult<std::map<int, std::vector<NodeId>>> result =
      client.traversal_orders(session_id);
  peer.join();

  ASSERT_TRUE(served);
  ASSERT_TRUE(result.status.ok) << result.status.message;
  ASSERT_EQ(result.value.size(), 64U);
  std::size_t recursive_entries = result.value.size();
  for (const auto& branch : result.value) {
    recursive_entries += branch.second.size();
  }
  EXPECT_EQ(recursive_entries, kSnapshotMaxEntries);
  ASSERT_EQ(requests.size(), 2U);
  EXPECT_EQ(requests[0]["params"]["limit"], kGeneralPageMaxEntries);
  EXPECT_EQ(requests[1]["params"]["limit"], 32);
  EXPECT_TRUE(client.connected());
}

/**
 * @brief Rejects one recursive entry after 64 sustained one-row pages.
 * @return Nothing; GoogleTest assertions report protocol or call mismatch.
 * @throws std::bad_alloc or std::runtime_error if socket/test data setup fails.
 * @note The first 64 pages total exactly 262,144 recursive entries. The final
 *       outer row exceeds by one, is not appended, returns no partial map, and
 *       is attempted exactly once without retry.
 */
TEST(ClientCollectionAggregation,
     RejectsOneRecursiveEntryAfterSustainedPagesWithoutPartialValue) {
  ScopedTempDirectory temp("ps-ipc-small");
  const std::string socket_path = (temp.path() / "server.sock").string();
  UniqueFd listener = create_test_listener(socket_path);
  const IpcSessionId session_id{std::string(32, 'a')};
  const std::string cursor(32, 'b');
  std::vector<ScriptedClientReply> replies;
  replies.reserve(65);
  for (std::size_t index = 0; index < 65; ++index) {
    const bool has_more = index + 1U != 65U;
    const std::size_t nested_count = index < 64U ? 4095U : 0U;
    replies.push_back(ScriptedClientReply{
        "inspect.traversal_orders",
        traversal_order_page_result(session_id, static_cast<int>(index),
                                    {nested_count}, index, has_more, cursor)});
  }
  std::vector<Json> requests;
  bool served = false;
  std::thread peer([&] {
    served = serve_scripted_client_replies(listener.get(), replies, &requests);
  });
  Client client;
  ASSERT_TRUE(client.connect(socket_path).ok);
  const IpcResult<std::map<int, std::vector<NodeId>>> result =
      client.traversal_orders(session_id);
  peer.join();

  ASSERT_TRUE(served);
  EXPECT_FALSE(result.status.ok);
  EXPECT_EQ(result.status.domain, OperationErrorDomain::Protocol);
  EXPECT_EQ(result.status.code, kInvalidRequestCode);
  EXPECT_TRUE(result.value.empty());
  EXPECT_EQ(requests.size(), 65U);
  EXPECT_TRUE(client.connected());
}

/**
 * @brief Accepts exactly 64 MiB of canonical rows despite unknown row fields.
 * @return Nothing; GoogleTest assertions report protocol or value mismatch.
 * @throws std::bad_alloc, std::invalid_argument, or std::runtime_error when
 *         deterministic socket or boundary-data setup fails.
 * @note Raw scripted rows exceed 64 MiB because every row has a future member.
 *       Decoding drops those members and canonical public measurement remains
 *       exactly at the inclusive version 1 byte boundary.
 */
TEST(ClientCollectionAggregation,
     AcceptsExactCanonicalByteBoundaryAndIgnoresUnknownMembers) {
  ScopedTempDirectory temp("ps-ipc-bytes");
  const std::string socket_path = (temp.path() / "server.sock").string();
  UniqueFd listener = create_test_listener(socket_path);
  const StableSourceReplyScript script =
      stable_source_reply_script(kSnapshotMaxBytes, true);
  ASSERT_EQ(script.canonical_bytes, kSnapshotMaxBytes);
  ASSERT_GT(script.raw_bytes, kSnapshotMaxBytes);
  std::vector<Json> requests;
  bool served = false;
  std::thread peer([&] {
    served = serve_scripted_client_replies(listener.get(), script.replies,
                                           &requests);
  });
  Client client;
  ASSERT_TRUE(client.connect(socket_path).ok);
  const IpcResult<std::map<std::string, std::string>> result =
      client.ops_sources();
  peer.join();

  ASSERT_TRUE(served);
  ASSERT_TRUE(result.status.ok) << result.status.message;
  EXPECT_EQ(result.value.size(), 8U);
  EXPECT_EQ(requests.size(), 8U);
  EXPECT_TRUE(client.connected());
}

/**
 * @brief Rejects canonical stable rows that exceed 64 MiB by one byte.
 * @return Nothing; GoogleTest assertions report protocol or call mismatch.
 * @throws std::bad_alloc, std::invalid_argument, or std::runtime_error when
 *         deterministic socket or boundary-data setup fails.
 * @note The final page is decoded and measured once, rejected before append,
 *       and the failed result publishes no sources from prior accepted pages.
 */
TEST(ClientCollectionAggregation,
     RejectsOneCanonicalByteBeyondBoundaryWithoutPartialValueOrRetry) {
  ScopedTempDirectory temp("ps-ipc-over");
  const std::string socket_path = (temp.path() / "server.sock").string();
  UniqueFd listener = create_test_listener(socket_path);
  const StableSourceReplyScript script =
      stable_source_reply_script(kSnapshotMaxBytes + 1U, false);
  ASSERT_EQ(script.canonical_bytes, kSnapshotMaxBytes + 1U);
  std::vector<Json> requests;
  bool served = false;
  std::thread peer([&] {
    served = serve_scripted_client_replies(listener.get(), script.replies,
                                           &requests);
  });
  Client client;
  ASSERT_TRUE(client.connect(socket_path).ok);
  const IpcResult<std::map<std::string, std::string>> result =
      client.ops_sources();
  peer.join();

  ASSERT_TRUE(served);
  EXPECT_FALSE(result.status.ok);
  EXPECT_EQ(result.status.domain, OperationErrorDomain::Protocol);
  EXPECT_EQ(result.status.code, kInvalidRequestCode);
  EXPECT_TRUE(result.value.empty());
  EXPECT_EQ(requests.size(), 8U);
  EXPECT_TRUE(client.connected());
}

/**
 * @brief Rejects every malformed compute-state/status/output coupling.
 *
 * Independent scripted peers return a nonterminal job with terminal success,
 * a succeeded job with failed status, and a succeeded status-only poll with
 * unexpected output metadata. Each response must fail typed validation before
 * any malformed snapshot is published.
 *
 * @return Nothing; GoogleTest assertions report status or attempt mismatch.
 * @throws std::bad_alloc, std::runtime_error, or std::system_error if scripted
 *         job values, sockets, or peer threads cannot be created.
 * @note Each case performs exactly one `compute.status` RPC. A malformed typed
 *       result is recoverable Protocol input and leaves the Client connected.
 */
TEST(ClientJobValidation, RejectsMalformedStateCouplingWithOneStatusAttempt) {
  const ps::ipc::ComputeRequestId compute_id{std::string(32, 'b')};
  const IpcSessionId session_id{std::string(32, 'a')};
  const Json success_status = encode_operation_status(ok_status());
  const Json failed_status = encode_operation_status(failure_status(
      OperationErrorDomain::Graph, static_cast<std::int32_t>(GraphErrc::Io),
      "io", "scripted failure"));
  std::vector<Json> malformed;
  malformed.push_back(Json{{"compute_id", compute_id.value},
                           {"session_id", session_id.value},
                           {"state", "running"},
                           {"cancellable", false},
                           {"status", success_status},
                           {"output", nullptr}});
  malformed.push_back(Json{{"compute_id", compute_id.value},
                           {"session_id", session_id.value},
                           {"state", "succeeded"},
                           {"cancellable", false},
                           {"status", failed_status},
                           {"output", nullptr}});
  malformed.push_back(Json{{"compute_id", compute_id.value},
                           {"session_id", session_id.value},
                           {"state", "succeeded"},
                           {"cancellable", false},
                           {"status", success_status},
                           {"output", Json::object()}});

  for (const Json& result_value : malformed) {
    ScopedTempDirectory temp("ps-ipc-badjob");
    const std::string socket_path = (temp.path() / "server.sock").string();
    UniqueFd listener = create_test_listener(socket_path);
    std::vector<Json> requests;
    bool served = false;
    const std::vector<ScriptedClientReply> replies = {
        {"compute.status", result_value}};
    std::thread peer([&] {
      served =
          serve_scripted_client_replies(listener.get(), replies, &requests);
    });
    Client client;
    ASSERT_TRUE(client.connect(socket_path).ok);
    const IpcResult<ComputeJobSnapshot> observed =
        client.compute_status(compute_id);
    peer.join();
    ASSERT_TRUE(served);
    EXPECT_FALSE(observed.status.ok);
    EXPECT_EQ(observed.status.domain, OperationErrorDomain::Protocol);
    EXPECT_EQ(observed.status.code, kInvalidRequestCode);
    EXPECT_EQ(requests.size(), 1U);
    EXPECT_TRUE(client.connected());
  }
}

/**
 * @brief Decodes image-output metadata and releases its matching lease once.
 *
 * The peer returns one terminal image job with opaque output and delivery ids,
 * tight CPU layout metadata, and filesystem identity. The Client publishes the
 * owned typed values, then sends exactly one lease-aware release containing the
 * same compute and delivery ids.
 *
 * @return Nothing; GoogleTest assertions report decode or request mismatch.
 * @throws std::bad_alloc, std::runtime_error, or std::system_error if socket,
 *         JSON, result storage, or peer-thread setup cannot complete.
 * @note This direct-Client test validates typed wire ownership only. Artifact
 *       opening, mmap lifetime, and identity revalidation belong to the
 *       installed IPC Host adapter boundary.
 */
TEST(ClientJobValidation, DecodesOutputAndReleasesMatchingLeaseOnce) {
  ScopedTempDirectory temp("ps-ipc-lease");
  const std::string socket_path = (temp.path() / "server.sock").string();
  UniqueFd listener = create_test_listener(socket_path);
  const ps::ipc::ComputeRequestId compute_id{std::string(32, 'b')};
  const IpcSessionId session_id{std::string(32, 'a')};
  const std::string output_id(32, 'c');
  const std::string delivery_id(32, 'd');
  const Json output{{"output_id", output_id},
                    {"delivery_id", delivery_id},
                    {"path", "/tmp/protected-output.bin"},
                    {"width", 2},
                    {"height", 2},
                    {"channels", 1},
                    {"data_type", "uint8"},
                    {"device", "cpu"},
                    {"row_step", 2},
                    {"byte_size", 4},
                    {"filesystem_device", 17},
                    {"inode", 23}};
  const Json job{{"compute_id", compute_id.value},
                 {"session_id", session_id.value},
                 {"state", "succeeded"},
                 {"cancellable", false},
                 {"status", encode_operation_status(ok_status())},
                 {"output", output}};
  const std::vector<ScriptedClientReply> replies = {
      {"compute.result", job},
      {"compute.release",
       Json{{"compute_id", compute_id.value}, {"released", true}}}};
  std::vector<Json> requests;
  bool served = false;
  std::thread peer([&] {
    served = serve_scripted_client_replies(listener.get(), replies, &requests);
  });
  Client client;
  ASSERT_TRUE(client.connect(socket_path).ok);
  const IpcResult<ComputeJobSnapshot> result =
      client.compute_result(compute_id);
  ASSERT_TRUE(result.status.ok) << result.status.message;
  ASSERT_TRUE(result.value.output.has_value());
  EXPECT_EQ(result.value.output->metadata.output_id.value, output_id);
  EXPECT_EQ(result.value.output->delivery_id.value, delivery_id);
  EXPECT_EQ(result.value.output->metadata.row_step, 2U);
  EXPECT_EQ(result.value.output->metadata.byte_size, 4U);
  const IpcResult<ComputeReleaseResult> released =
      client.release_compute(compute_id, result.value.output->delivery_id);
  peer.join();

  ASSERT_TRUE(served);
  ASSERT_TRUE(released.status.ok) << released.status.message;
  EXPECT_TRUE(released.value.released);
  ASSERT_EQ(requests.size(), 2U);
  EXPECT_EQ(requests[1]["params"]["compute_id"], compute_id.value);
  EXPECT_EQ(requests[1]["params"]["delivery_id"], delivery_id);
}

/**
 * @brief Rejects terminal image output with a non-tight row layout.
 *
 * The scripted result advertises a two-byte logical row but a three-byte row
 * step and corresponding padded total size. The direct Client must reject the
 * entire terminal snapshot as malformed rather than publishing output metadata
 * that violates the version 1 tight-row artifact contract.
 *
 * @return Nothing; GoogleTest assertions report status or attempt mismatch.
 * @throws std::bad_alloc, std::runtime_error, or std::system_error if socket,
 *         malformed JSON, or peer-thread setup cannot complete.
 * @note Validation performs one `compute.result` attempt, publishes no output,
 *       and keeps the connection available for later independent calls.
 */
TEST(ClientJobValidation, RejectsMalformedOutputByteLayout) {
  ScopedTempDirectory temp("ps-ipc-badout");
  const std::string socket_path = (temp.path() / "server.sock").string();
  UniqueFd listener = create_test_listener(socket_path);
  const ps::ipc::ComputeRequestId compute_id{std::string(32, 'b')};
  const Json malformed_job{
      {"compute_id", compute_id.value},
      {"session_id", std::string(32, 'a')},
      {"state", "succeeded"},
      {"cancellable", false},
      {"status", encode_operation_status(ok_status())},
      {"output", Json{{"output_id", std::string(32, 'c')},
                      {"delivery_id", std::string(32, 'd')},
                      {"path", "/tmp/protected-output.bin"},
                      {"width", 2},
                      {"height", 2},
                      {"channels", 1},
                      {"data_type", "uint8"},
                      {"device", "cpu"},
                      {"row_step", 3},
                      {"byte_size", 6},
                      {"filesystem_device", 17},
                      {"inode", 23}}}};
  std::vector<Json> requests;
  bool served = false;
  const std::vector<ScriptedClientReply> replies = {
      {"compute.result", malformed_job}};
  std::thread peer([&] {
    served = serve_scripted_client_replies(listener.get(), replies, &requests);
  });
  Client client;
  ASSERT_TRUE(client.connect(socket_path).ok);
  const IpcResult<ComputeJobSnapshot> result =
      client.compute_result(compute_id);
  peer.join();

  ASSERT_TRUE(served);
  EXPECT_FALSE(result.status.ok);
  EXPECT_EQ(result.status.domain, OperationErrorDomain::Protocol);
  EXPECT_EQ(result.status.code, kInvalidRequestCode);
  EXPECT_FALSE(result.value.output.has_value());
  EXPECT_EQ(requests.size(), 1U);
  EXPECT_TRUE(client.connected());
}

/**
 * @brief Rejects malicious output metadata above the artifact byte ceiling.
 *
 * The peer advertises a mathematically tight one-row UINT8 artifact whose
 * width, row step, and byte size are exactly 512 MiB plus one. The direct
 * Client must reject the typed metadata before any payload allocation or path
 * consumption and must not retry the result RPC.
 *
 * @return Nothing; GoogleTest assertions report status, output, or attempt
 *         mismatch.
 * @throws std::bad_alloc, std::runtime_error, or std::system_error if socket,
 *         bounded JSON, Client, or peer-thread setup cannot complete.
 * @note The nonexistent path is intentionally never opened by direct Client;
 *       artifact ownership belongs to the IPC Host after successful decoding.
 */
TEST(ClientJobValidation,
     RejectsOutputMetadataAboveArtifactLimitWithoutAllocation) {
  ScopedTempDirectory temp("ps-ipc-bigout");
  const std::string socket_path = (temp.path() / "server.sock").string();
  UniqueFd listener = create_test_listener(socket_path);
  const ps::ipc::ComputeRequestId compute_id{std::string(32, 'b')};
  constexpr std::size_t oversized = kOutputArtifactMaxBytes + 1U;
  const Json malicious_job{
      {"compute_id", compute_id.value},
      {"session_id", std::string(32, 'a')},
      {"state", "succeeded"},
      {"cancellable", false},
      {"status", encode_operation_status(ok_status())},
      {"output", Json{{"output_id", std::string(32, 'c')},
                      {"delivery_id", std::string(32, 'd')},
                      {"path", "/path/must/not/be-opened.bin"},
                      {"width", oversized},
                      {"height", 1},
                      {"channels", 1},
                      {"data_type", "uint8"},
                      {"device", "cpu"},
                      {"row_step", oversized},
                      {"byte_size", oversized},
                      {"filesystem_device", 17},
                      {"inode", 23}}}};
  std::vector<Json> requests;
  bool served = false;
  const std::vector<ScriptedClientReply> replies = {
      {"compute.result", malicious_job}};
  std::thread peer([&] {
    served = serve_scripted_client_replies(listener.get(), replies, &requests);
  });
  Client client;
  ASSERT_TRUE(client.connect(socket_path).ok);
  const IpcResult<ComputeJobSnapshot> result =
      client.compute_result(compute_id);
  peer.join();

  ASSERT_TRUE(served);
  EXPECT_FALSE(result.status.ok);
  EXPECT_EQ(result.status.domain, OperationErrorDomain::Protocol);
  EXPECT_EQ(result.status.code, kInvalidRequestCode);
  EXPECT_EQ(result.status.name, "invalid_request");
  EXPECT_FALSE(result.value.output.has_value());
  ASSERT_EQ(requests.size(), 1U);
  EXPECT_EQ(requests.front()["method"], "compute.result");
  EXPECT_TRUE(client.connected());
}

/**
 * @brief Returns ambiguous mutation transport loss without retrying the call.
 *
 * The peer records one complete `graph.node_yaml.set` request and closes the
 * socket without a response, modelling a mutation that may already have taken
 * effect remotely. The Client must return the first read failure and preserve
 * the exact one-attempt request arguments.
 *
 * @return Nothing; GoogleTest assertions report status or request mismatch.
 * @throws std::bad_alloc, std::runtime_error, or std::system_error if socket,
 *         request capture, or peer-thread setup cannot complete.
 * @note The expected Transport code is the established read/truncated-frame
 *       classification. The Client disconnects and never resends the mutation.
 */
TEST(ClientRetryPolicy, DoesNotRetryMutationAfterAmbiguousTransportLoss) {
  ScopedTempDirectory temp("ps-ipc-noretry");
  const std::string socket_path = (temp.path() / "server.sock").string();
  UniqueFd listener = create_test_listener(socket_path);
  Json request;
  bool received = false;
  std::thread peer([&] {
    received = receive_one_client_request_then_close(listener.get(), &request);
  });
  Client client;
  ASSERT_TRUE(client.connect(socket_path).ok);
  const IpcSessionId session_id{std::string(32, 'a')};
  const VoidResult result =
      client.set_node_yaml(session_id, NodeId{7}, "id: 7\nname: changed");
  peer.join();

  ASSERT_TRUE(received);
  EXPECT_FALSE(result.status.ok);
  EXPECT_EQ(result.status.domain, OperationErrorDomain::Transport);
  EXPECT_EQ(result.status.code, 4);
  EXPECT_EQ(request["method"], "graph.node_yaml.set");
  EXPECT_EQ(request["params"]["session_id"], session_id.value);
  EXPECT_EQ(request["params"]["node_id"], 7);
  EXPECT_FALSE(client.connected());
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
  for (std::string_view method : kVersionTwoMethodNames) {
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

/**
 * @brief Rejects node and dependency results that do not match the request.
 *
 * A scripted abnormal peer returns, in order, the wrong inspected node id, a
 * wrong start node for a node-scoped tree, an ending-node tree for a
 * node-scoped request, and a start-node tree for a graph-scoped request. The
 * Client must classify every response as malformed and publish no copied
 * snapshot from any call.
 *
 * @return Nothing; GoogleTest assertions report identity, status, or request
 *         mismatches.
 * @throws std::bad_alloc, std::runtime_error, or std::system_error if socket,
 *         scripted values, or peer-thread setup cannot complete.
 * @note The single connected Client remains synchronized after each local
 *       response-validation failure, and every operation is attempted once.
 */
TEST(ClientResultValidation,
     RejectsMismatchedNodeAndDependencyRequestIdentity) {
  ScopedTempDirectory temp("ps-ipc-bad-id");
  const std::string socket_path = (temp.path() / "server.sock").string();
  UniqueFd listener = create_test_listener(socket_path);
  const IpcSessionId session_id{std::string(32, 'a')};

  NodeInspectionView wrong_node;
  wrong_node.id = NodeId{8};
  wrong_node.name = "wrong-node";
  wrong_node.type = "fixture";
  wrong_node.subtype = "source";

  HostDependencyTreeSnapshot wrong_start_tree;
  wrong_start_tree.scope = HostDependencyTreeScope::StartNode;
  wrong_start_tree.start_node = NodeId{8};
  Json wrong_start = encode_dependency_tree(session_id, wrong_start_tree);
  wrong_start["offset"] = 0;
  wrong_start["has_more"] = false;
  wrong_start["cursor"] = nullptr;

  HostDependencyTreeSnapshot wrong_node_scope_tree;
  wrong_node_scope_tree.scope = HostDependencyTreeScope::EndingNodes;
  Json wrong_node_scope =
      encode_dependency_tree(session_id, wrong_node_scope_tree);
  wrong_node_scope["offset"] = 0;
  wrong_node_scope["has_more"] = false;
  wrong_node_scope["cursor"] = nullptr;

  HostDependencyTreeSnapshot wrong_graph_scope_tree;
  wrong_graph_scope_tree.scope = HostDependencyTreeScope::StartNode;
  wrong_graph_scope_tree.start_node = NodeId{7};
  Json wrong_graph_scope =
      encode_dependency_tree(session_id, wrong_graph_scope_tree);
  wrong_graph_scope["offset"] = 0;
  wrong_graph_scope["has_more"] = false;
  wrong_graph_scope["cursor"] = nullptr;

  const std::vector<ScriptedClientReply> replies = {
      {"inspect.node", Json{{"session_id", session_id.value},
                            {"node", encode_node(wrong_node)}}},
      {"inspect.dependency_tree", std::move(wrong_start)},
      {"inspect.dependency_tree", std::move(wrong_node_scope)},
      {"inspect.dependency_tree", std::move(wrong_graph_scope)}};
  std::vector<Json> requests;
  bool served = false;
  std::thread peer([&] {
    served = serve_scripted_client_replies(listener.get(), replies, &requests);
  });
  Client client;
  ASSERT_TRUE(client.connect(socket_path).ok);
  const IpcResult<NodeInspectionView> inspected =
      client.inspect_node(session_id, NodeId{7});
  const IpcResult<HostDependencyTreeSnapshot> wrong_start_result =
      client.inspect_dependency_tree(session_id, NodeId{7}, false);
  const IpcResult<HostDependencyTreeSnapshot> wrong_node_scope_result =
      client.inspect_dependency_tree(session_id, NodeId{7}, false);
  const IpcResult<HostDependencyTreeSnapshot> wrong_graph_scope_result =
      client.inspect_dependency_tree(session_id, std::nullopt, false);
  peer.join();

  ASSERT_TRUE(served);
  for (const OperationStatus* status : std::array<const OperationStatus*, 4>{
           &inspected.status, &wrong_start_result.status,
           &wrong_node_scope_result.status, &wrong_graph_scope_result.status}) {
    ASSERT_NE(status, nullptr);
    EXPECT_FALSE(status->ok);
    EXPECT_EQ(status->domain, OperationErrorDomain::Protocol);
    EXPECT_EQ(status->code, kInvalidRequestCode);
  }
  EXPECT_EQ(inspected.value.id.value, -1);
  EXPECT_TRUE(wrong_start_result.value.entries.empty());
  EXPECT_TRUE(wrong_node_scope_result.value.entries.empty());
  EXPECT_TRUE(wrong_graph_scope_result.value.entries.empty());
  ASSERT_EQ(requests.size(), 4U);
  EXPECT_EQ(requests[0]["params"]["node_id"], 7);
  EXPECT_EQ(requests[1]["params"]["node_id"], 7);
  EXPECT_EQ(requests[2]["params"]["node_id"], 7);
  EXPECT_FALSE(requests[3]["params"].contains("node_id"));
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
      R"({"protocol_version":2,"id":"client-1","result":{"pong":true,"pong":true,"server_instance_id":"0123456789abcdef0123456789abcdef"}})";
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
