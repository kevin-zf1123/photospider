#pragma once

#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "photospider/host/host.hpp"
#include "photospider/ipc/protocol.hpp"

namespace ps::ipc::internal {

/**
 * @brief Generates a 128-bit operating-system-entropy opaque identifier.
 *
 * @return Exactly 32 lowercase hexadecimal characters.
 * @throws std::runtime_error if the operating-system entropy source fails.
 * @throws std::bad_alloc if result storage cannot be allocated.
 * @note Values contain no pointer, path, session name, or process-address data.
 */
std::string generate_opaque_id();

/**
 * @brief Thread-safe loading/active mapping across opaque, Host, and display
 * identities.
 *
 * Reservations prevent two concurrent loads from publishing the same display
 * name. A successful Host load is committed with the exact Host-returned
 * session id. Failed Host loads or commit failures roll back the reservation.
 *
 * @throws std::bad_alloc when registry storage allocation fails.
 * @note The registry mutex is independent of the daemon Host mutex. Callers
 *       serialize multi-step Host/registry transactions with the Host mutex.
 */
class SessionRegistry {
 public:
  /**
   * @brief Function that returns one opaque token candidate.
   *
   * @throws std::runtime_error when a production entropy source fails.
   * @throws std::bad_alloc when candidate storage cannot be allocated.
   * @note Production uses operating-system entropy; deterministic tests may
   *       inject a generator without changing the public IPC boundary.
   */
  using TokenGenerator = std::function<std::string()>;

  /**
   * @brief Creates a registry using operating-system entropy.
   *
   * @throws std::bad_alloc if function storage allocation fails.
   */
  SessionRegistry();

  /**
   * @brief Creates a registry with a deterministic/injected token source.
   *
   * @param generator Candidate generator used until a collision-free token is
   *        reserved.
   * @throws std::bad_alloc if function storage allocation fails.
   * @note This constructor supports deterministic durable tests; production
   *       passes no generator and uses the operating-system source.
   */
  explicit SessionRegistry(TokenGenerator generator);

  /**
   * @brief Prevents copying mutex-protected registry ownership.
   *
   * @throws Nothing because this operation is unavailable.
   */
  SessionRegistry(const SessionRegistry&) = delete;

  /**
   * @brief Prevents duplicating registry indexes by assignment.
   *
   * @return No value because this operation is unavailable.
   * @throws Nothing because this operation is unavailable.
   */
  SessionRegistry& operator=(const SessionRegistry&) = delete;

  /**
   * @brief Reserves one loading token and session display name.
   *
   * @param session_name Validated caller-provided Host session name.
   * @return Reserved opaque id or a graph/daemon failure.
   * @throws std::bad_alloc if token or registry storage allocation fails.
   * @throws std::runtime_error if the entropy source fails.
   * @note At most 128 colliding candidates are attempted; no active mapping is
   *       published by this operation.
   */
  IpcResult<IpcSessionId> reserve(const std::string& session_name);

  /**
   * @brief Publishes one successful Host load into all three active indexes.
   *
   * @param session_id Previously reserved opaque id.
   * @param host_session Exact session id returned by `Host::load_graph`.
   * @return Success or daemon invariant failure.
   * @throws std::bad_alloc if active index allocation fails.
   * @note On allocation failure, partial opaque, Host, and display-name index
   *       insertions are removed before the exception propagates so the caller
   *       can close Host and rollback.
   */
  OperationStatus commit(const IpcSessionId& session_id,
                         const GraphSessionId& host_session);

  /**
   * @brief Removes one uncommitted loading reservation.
   *
   * @param session_id Reserved opaque id.
   * @throws Nothing.
   * @note Active mappings are not removed by this operation.
   */
  void rollback(const IpcSessionId& session_id) noexcept;

  /**
   * @brief Resolves one active opaque id to its private Host session.
   *
   * @param session_id Opaque daemon session identifier.
   * @return Copied Host session id, or nullopt when unknown/loading.
   * @throws std::bad_alloc if the returned string copy cannot be allocated.
   */
  std::optional<GraphSessionId> resolve(const IpcSessionId& session_id) const;

  /**
   * @brief Removes one active mapping from all three indexes.
   *
   * @param session_id Opaque daemon session identifier.
   * @throws Nothing.
   * @note Call only after successful Host close or Host `NotFound`.
   */
  void erase(const IpcSessionId& session_id) noexcept;

  /**
   * @brief Reconciles active mappings with one Host list snapshot.
   *
   * @param host_sessions Exact copied result of `Host::list_graphs()` obtained
   *        while the caller holds the Host mutex.
   * @return Sorted public summaries, or daemon invariant failure without
   *         exposing untracked Host names.
   * @throws std::bad_alloc if sets, strings, or summaries cannot be allocated.
   */
  IpcResult<std::vector<GraphSessionSummary>> reconcile(
      const std::vector<GraphSessionId>& host_sessions) const;

  /**
   * @brief Copies active opaque/Host mappings for daemon shutdown.
   *
   * @return Rows sorted by opaque id.
   * @throws std::bad_alloc if copied storage cannot be allocated.
   * @note Loading reservations are excluded because they have no committed
   *       Host session.
   */
  std::vector<std::pair<IpcSessionId, GraphSessionId>> active_sessions() const;

  /**
   * @brief Removes every loading and active mapping.
   *
   * @throws Nothing.
   * @note Call after shutdown attempted to close all copied Host sessions.
   */
  void clear() noexcept;

 private:
  /**
   * @brief Committed registry row containing private and display identities.
   *
   * @throws std::bad_alloc when copied strings cannot be allocated.
   */
  struct ActiveEntry {
    /** @brief Exact private session id returned by Host. */
    GraphSessionId host_session;

    /** @brief Original caller-provided display/session name. */
    std::string session_name;
  };

  /** @brief Serializes all registry indexes and reservations. */
  mutable std::mutex mutex_;

  /** @brief Candidate generator, using OS entropy in production. */
  TokenGenerator token_generator_;

  /** @brief Loading reservation indexed by opaque token. */
  std::map<std::string, std::string> pending_by_token_;

  /** @brief Loading reservation indexed by caller session name. */
  std::map<std::string, std::string> pending_by_name_;

  /** @brief Active entry indexed by opaque token. */
  std::map<std::string, ActiveEntry> active_by_token_;

  /** @brief Opaque token indexed by exact private Host session value. */
  std::map<std::string, std::string> active_by_host_;

  /** @brief Opaque token indexed by caller-provided display/session name. */
  std::map<std::string, std::string> active_by_name_;
};

}  // namespace ps::ipc::internal
