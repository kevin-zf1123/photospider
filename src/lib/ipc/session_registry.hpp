#pragma once

#include <condition_variable>
#include <cstddef>
#include <functional>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include "photospider/host/host.hpp"
#include "photospider/ipc/protocol.hpp"

namespace ps::ipc::internal {

/**
 * @brief Thread-safe loading/active mapping across opaque, Host, and display
 * identities.
 *
 * Reservations prevent two concurrent loads from publishing the same display
 * name. A successful Host load is committed with the exact Host-returned
 * session id. Failed Host loads or commit failures roll back the reservation.
 *
 * @throws std::bad_alloc when registry storage allocation fails.
 * @note The registry mutex is independent of the daemon Host mutex. Admission
 *       captures and counts a row without retaining this mutex across Host;
 *       close marks/waits admission before it acquires the Host mutex. Host
 *       transactions may update registry indexes while holding only Host plus
 *       a short registry critical section.
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
   * @brief Move-only admission for one ordinary session-scoped Host call.
   *
   * @throws std::bad_alloc when moved string ownership cannot be represented.
   * @note Destruction releases exactly one admitted-call count. The admission
   *       owns no Host object and must be destroyed before its registry.
   */
  class HostCallAdmission {
   public:
    /**
     * @brief Creates an inactive admission used by failed result values.
     * @throws Nothing.
     */
    HostCallAdmission() = default;

    /**
     * @brief Releases one active admitted-call count when present.
     * @throws Nothing.
     */
    ~HostCallAdmission() noexcept;

    /**
     * @brief Prevents duplicating one admitted-call count.
     * @throws Nothing because construction is unavailable.
     */
    HostCallAdmission(const HostCallAdmission&) = delete;

    /**
     * @brief Prevents duplicating one admitted-call count by assignment.
     * @return No value because copying is unavailable.
     * @throws Nothing because this operation is unavailable.
     */
    HostCallAdmission& operator=(const HostCallAdmission&) = delete;

    /**
     * @brief Transfers one admitted-call count from another owner.
     * @param other Admission that becomes inactive.
     * @throws Nothing.
     */
    HostCallAdmission(HostCallAdmission&& other) noexcept;

    /**
     * @brief Releases any current count and transfers another admission.
     * @param other Admission that becomes inactive.
     * @return This admission after transfer.
     * @throws Nothing.
     */
    HostCallAdmission& operator=(HostCallAdmission&& other) noexcept;

    /**
     * @brief Returns the private Host session captured at admission time.
     * @return Stable reference valid for this admission's active lifetime.
     * @throws Nothing.
     */
    const GraphSessionId& host_session() const noexcept;

    /**
     * @brief Reports whether this object owns an admitted-call count.
     * @return True only before release or move.
     * @throws Nothing.
     */
    bool active() const noexcept;

   private:
    friend class SessionRegistry;

    /**
     * @brief Creates one active admitted-call owner.
     * @param owner Registry that owns the counted row.
     * @param token Opaque session token used to release the count.
     * @param host_session Private Host session captured under the registry
     * lock.
     * @throws std::bad_alloc if owned strings cannot be moved or copied.
     */
    HostCallAdmission(SessionRegistry* owner, std::string token,
                      GraphSessionId host_session);

    /**
     * @brief Releases the owned count when active.
     * @return Nothing.
     * @throws Nothing.
     */
    void reset() noexcept;

    /** @brief Registry whose row owns the count, or null when inactive. */
    SessionRegistry* owner_ = nullptr;

    /** @brief Opaque row key retained independently of map node addresses. */
    std::string token_;

    /** @brief Private Host value captured before external serialization. */
    GraphSessionId host_session_;
  };

  /**
   * @brief Move-only admission retained by one queued or running compute job.
   *
   * @throws std::bad_alloc when moved string ownership cannot be represented.
   * @note The compute registry keeps this token through complete terminal
   *       publication, then destroys it outside the compute-registry lock.
   */
  class JobAdmission {
   public:
    /**
     * @brief Creates an inactive admission used by failed result values.
     * @throws Nothing.
     */
    JobAdmission() = default;

    /**
     * @brief Releases one queued/running-job count when present.
     * @throws Nothing.
     */
    ~JobAdmission() noexcept;

    /**
     * @brief Prevents duplicating one queued/running-job count.
     * @throws Nothing because construction is unavailable.
     */
    JobAdmission(const JobAdmission&) = delete;

    /**
     * @brief Prevents duplicating one job count by assignment.
     * @return No value because copying is unavailable.
     * @throws Nothing because this operation is unavailable.
     */
    JobAdmission& operator=(const JobAdmission&) = delete;

    /**
     * @brief Transfers one queued/running-job count from another owner.
     * @param other Admission that becomes inactive.
     * @throws Nothing.
     */
    JobAdmission(JobAdmission&& other) noexcept;

    /**
     * @brief Releases any current count and transfers another admission.
     * @param other Admission that becomes inactive.
     * @return This admission after transfer.
     * @throws Nothing.
     */
    JobAdmission& operator=(JobAdmission&& other) noexcept;

    /**
     * @brief Returns the private Host session captured at admission time.
     * @return Stable reference valid for this admission's active lifetime.
     * @throws Nothing.
     */
    const GraphSessionId& host_session() const noexcept;

    /**
     * @brief Reports whether this object owns a queued/running-job count.
     * @return True only before release or move.
     * @throws Nothing.
     */
    bool active() const noexcept;

   private:
    friend class SessionRegistry;

    /**
     * @brief Creates one active queued/running-job owner.
     * @param owner Registry that owns the counted row.
     * @param token Opaque session token used to release the count.
     * @param host_session Private Host session captured under the registry
     * lock.
     * @throws std::bad_alloc if owned strings cannot be moved or copied.
     */
    JobAdmission(SessionRegistry* owner, std::string token,
                 GraphSessionId host_session);

    /**
     * @brief Releases the owned count when active.
     * @return Nothing.
     * @throws Nothing.
     */
    void reset() noexcept;

    /** @brief Registry whose row owns the count, or null when inactive. */
    SessionRegistry* owner_ = nullptr;

    /** @brief Opaque row key retained independently of map node addresses. */
    std::string token_;

    /** @brief Private Host value captured before external execution. */
    GraphSessionId host_session_;
  };

  /**
   * @brief Exclusive move-only claim for one graph-close lifecycle.
   *
   * @throws std::bad_alloc when moved string ownership cannot be represented.
   * @note `begin_close()` marks the row Closing and waits admitted work before
   *       publishing this claim. Destruction without `erase()` reopens the row.
   */
  class CloseClaim {
   public:
    /**
     * @brief Creates an inactive claim used by failed result values.
     * @throws Nothing.
     */
    CloseClaim() = default;

    /**
     * @brief Reopens an unresolved close claim best-effort.
     * @throws Nothing.
     */
    ~CloseClaim() noexcept;

    /**
     * @brief Prevents duplicating exclusive close ownership.
     * @throws Nothing because construction is unavailable.
     */
    CloseClaim(const CloseClaim&) = delete;

    /**
     * @brief Prevents duplicating exclusive close ownership by assignment.
     * @return No value because copying is unavailable.
     * @throws Nothing because this operation is unavailable.
     */
    CloseClaim& operator=(const CloseClaim&) = delete;

    /**
     * @brief Transfers exclusive close ownership from another claim.
     * @param other Claim that becomes inactive.
     * @throws Nothing.
     */
    CloseClaim(CloseClaim&& other) noexcept;

    /**
     * @brief Reopens any current claim and transfers another claim.
     * @param other Claim that becomes inactive.
     * @return This claim after transfer.
     * @throws Nothing.
     */
    CloseClaim& operator=(CloseClaim&& other) noexcept;

    /**
     * @brief Returns the private Host session safe to close.
     * @return Stable reference valid until this claim is completed or moved.
     * @throws Nothing.
     */
    const GraphSessionId& host_session() const noexcept;

    /**
     * @brief Removes the closed or Host-missing session mapping.
     * @return Nothing.
     * @throws Nothing.
     * @note Call only after Host close success or Graph NotFound.
     */
    void erase() noexcept;

    /**
     * @brief Reopens session admission after a retryable Host close failure.
     * @return Nothing.
     * @throws Nothing.
     */
    void reopen() noexcept;

    /**
     * @brief Reports whether this object owns an unresolved close claim.
     * @return True only before erase, reopen, or move.
     * @throws Nothing.
     */
    bool active() const noexcept;

   private:
    friend class SessionRegistry;

    /**
     * @brief Creates one exclusive claim after admitted work reaches zero.
     * @param owner Registry that owns the Closing row.
     * @param token Opaque row key.
     * @param host_session Private Host session safe for the caller to close.
     * @throws std::bad_alloc if owned strings cannot be moved or copied.
     */
    CloseClaim(SessionRegistry* owner, std::string token,
               GraphSessionId host_session);

    /** @brief Registry whose row is Closing, or null when resolved. */
    SessionRegistry* owner_ = nullptr;

    /** @brief Opaque row key retained independently of map node addresses. */
    std::string token_;

    /** @brief Private Host session captured before the wait completed. */
    GraphSessionId host_session_;
  };

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
   * @brief Atomically admits one ordinary session-scoped Host call.
   *
   * @param session_id Opaque active session identifier.
   * @return Move-only admission with the private Host session, or Graph
   *         NotFound when absent, Closing, or global admission is stopped.
   * @throws std::bad_alloc if result ownership cannot be allocated.
   * @note No registry mutex remains held when the caller later enters Host.
   */
  IpcResult<HostCallAdmission> admit_host_call(const IpcSessionId& session_id);

  /**
   * @brief Atomically admits one queued/running compute job for a session.
   *
   * @param session_id Opaque active session identifier.
   * @return Move-only admission with the private Host session, or Graph
   *         NotFound when absent, Closing, or global admission is stopped.
   * @throws std::bad_alloc if result ownership cannot be allocated.
   * @note The caller must retain the token until terminal state is completely
   *       published; lookup/status operations need no live-session admission.
   */
  IpcResult<JobAdmission> admit_job(const IpcSessionId& session_id);

  /**
   * @brief Claims one session close and waits all admitted work to finish.
   *
   * @param session_id Opaque active session identifier.
   * @return Exclusive close claim, or Graph NotFound when absent, already
   *         Closing, or global admission is stopped.
   * @throws std::bad_alloc if claim ownership cannot be allocated.
   * @note The row becomes Closing before waiting. The condition-variable wait
   *       releases the registry mutex, and the caller receives no held lock.
   */
  IpcResult<CloseClaim> begin_close(const IpcSessionId& session_id);

  /**
   * @brief Enables new load, Host-call, job, and close admission.
   * @return Nothing.
   * @throws Nothing.
   * @note Intended for a new daemon run after prior runtime cleanup. Existing
   *       Closing rows are not implicitly reopened.
   */
  void start_admission() noexcept;

  /**
   * @brief Rejects every new load, Host-call, job, and close admission.
   * @return Nothing.
   * @throws Nothing.
   * @note Already admitted work and existing mappings remain owned until their
   *       normal completion and shutdown cleanup.
   */
  void stop_admission() noexcept;

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
   * @brief Lifecycle state of one committed session row.
   * @throws Nothing.
   */
  enum class LifecycleState {
    /** @brief New session-scoped work may be admitted. */
    Active,
    /** @brief Close owns the row and new admission is rejected. */
    Closing,
  };

  /**
   * @brief Counter category released by one move-only admission.
   * @throws Nothing.
   */
  enum class AdmissionKind {
    /** @brief Ordinary session-scoped Host call. */
    HostCall,
    /** @brief Queued or running compute request. */
    Job,
  };

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

    /** @brief Whether this row accepts work or is exclusively Closing. */
    LifecycleState lifecycle = LifecycleState::Active;

    /** @brief Ordinary Host calls admitted but not yet released. */
    std::size_t admitted_host_calls = 0;

    /** @brief Compute jobs queued/running before terminal publication. */
    std::size_t admitted_jobs = 0;
  };

  /**
   * @brief Releases one counted admission and wakes a waiting close.
   * @param token Opaque active-row key.
   * @param kind Counter category to decrement.
   * @return Nothing.
   * @throws Nothing; missing/cleared rows are tolerated during shutdown.
   */
  void release_admission(const std::string& token, AdmissionKind kind) noexcept;

  /**
   * @brief Resolves one close claim by erasing or reopening its row.
   * @param token Opaque Closing-row key.
   * @param erase_mapping True to remove all indexes; false to reopen.
   * @return Nothing.
   * @throws Nothing.
   */
  void complete_close(const std::string& token, bool erase_mapping) noexcept;

  /** @brief Serializes all registry indexes and reservations. */
  mutable std::mutex mutex_;

  /** @brief Wakes close waiters after admitted call/job release. */
  std::condition_variable lifecycle_cv_;

  /** @brief Whether new load/session/job/close admission is enabled. */
  bool accepting_ = true;

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
