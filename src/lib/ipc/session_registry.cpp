#include "ipc/session_registry.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "ipc/codec.hpp"

namespace ps::ipc::internal {
namespace {

/**
 * @brief Builds the stable missing-session status used by lifecycle admission.
 *
 * @return Graph-domain NotFound without exposing a private Host identifier.
 * @throws std::bad_alloc if diagnostic storage cannot be allocated.
 * @note Closing rows and globally stopped admission intentionally look absent
 *       to new session-scoped callers.
 */
OperationStatus session_not_found_status() {
  return failure_status(OperationErrorDomain::Graph,
                        static_cast<std::int32_t>(GraphErrc::NotFound),
                        "not_found", "opaque graph session was not found");
}

/**
 * @brief Builds the fail-safe status before a Host close invocation starts.
 * @return Daemon-domain internal error naming the pre-invocation boundary.
 * @throws std::bad_alloc if diagnostic storage cannot be allocated.
 */
OperationStatus host_close_not_started_status() {
  return failure_status(
      OperationErrorDomain::Daemon, kInternalErrorCode, "internal_error",
      "HostCloseNotStarted: Host close invocation did not begin");
}

}  // namespace

/** @copydoc SessionRegistry::HostCallAdmission::HostCallAdmission */
SessionRegistry::HostCallAdmission::HostCallAdmission(
    SessionRegistry* owner, std::string token, GraphSessionId host_session)
    : owner_(owner),                             // NOLINT
      token_(std::move(token)),                  // NOLINT
      host_session_(std::move(host_session)) {}  // NOLINT

/** @copydoc SessionRegistry::HostCallAdmission::~HostCallAdmission */
SessionRegistry::HostCallAdmission::~HostCallAdmission() noexcept {
  reset();
}

/** @copydoc SessionRegistry::HostCallAdmission::HostCallAdmission */
SessionRegistry::HostCallAdmission::HostCallAdmission(
    HostCallAdmission&& other) noexcept
    : owner_(std::exchange(other.owner_, nullptr)),     // NOLINT
      token_(std::move(other.token_)),                  // NOLINT
      host_session_(std::move(other.host_session_)) {}  // NOLINT

/** @copydoc SessionRegistry::HostCallAdmission::operator= */
SessionRegistry::HostCallAdmission&
SessionRegistry::HostCallAdmission::operator=(
    HostCallAdmission&& other) noexcept {
  if (this != &other) {
    reset();
    owner_ = std::exchange(other.owner_, nullptr);
    token_ = std::move(other.token_);
    host_session_ = std::move(other.host_session_);
  }
  return *this;
}

/** @copydoc SessionRegistry::HostCallAdmission::host_session */
const GraphSessionId& SessionRegistry::HostCallAdmission::host_session()
    const noexcept {  // NOLINT
  return host_session_;
}

/** @copydoc SessionRegistry::HostCallAdmission::active */
bool SessionRegistry::HostCallAdmission::active() const noexcept {
  return owner_ != nullptr;
}

/** @copydoc SessionRegistry::HostCallAdmission::reset */
void SessionRegistry::HostCallAdmission::reset() noexcept {
  SessionRegistry* owner = std::exchange(owner_, nullptr);
  if (owner != nullptr) {
    owner->release_admission(token_, AdmissionKind::HostCall);
  }
  token_.clear();
  host_session_.value.clear();
}

/** @copydoc SessionRegistry::JobAdmission::JobAdmission */
SessionRegistry::JobAdmission::JobAdmission(SessionRegistry* owner,
                                            std::string token,
                                            GraphSessionId host_session)
    : owner_(owner),                             // NOLINT
      token_(std::move(token)),                  // NOLINT
      host_session_(std::move(host_session)) {}  // NOLINT

/** @copydoc SessionRegistry::JobAdmission::~JobAdmission */
SessionRegistry::JobAdmission::~JobAdmission() noexcept {
  reset();
}

/** @copydoc SessionRegistry::JobAdmission::JobAdmission */
SessionRegistry::JobAdmission::JobAdmission(JobAdmission&& other) noexcept
    : owner_(std::exchange(other.owner_, nullptr)),     // NOLINT
      token_(std::move(other.token_)),                  // NOLINT
      host_session_(std::move(other.host_session_)) {}  // NOLINT

/** @copydoc SessionRegistry::JobAdmission::operator= */
SessionRegistry::JobAdmission& SessionRegistry::JobAdmission::operator=(
    JobAdmission&& other) noexcept {
  if (this != &other) {
    reset();
    owner_ = std::exchange(other.owner_, nullptr);
    token_ = std::move(other.token_);
    host_session_ = std::move(other.host_session_);
  }
  return *this;
}

/** @copydoc SessionRegistry::JobAdmission::host_session */
const GraphSessionId& SessionRegistry::JobAdmission::host_session()
    const noexcept {  // NOLINT
  return host_session_;
}

/** @copydoc SessionRegistry::JobAdmission::active */
bool SessionRegistry::JobAdmission::active() const noexcept {
  return owner_ != nullptr;
}

/** @copydoc SessionRegistry::JobAdmission::reset */
void SessionRegistry::JobAdmission::reset() noexcept {
  SessionRegistry* owner = std::exchange(owner_, nullptr);
  if (owner != nullptr) {
    owner->release_admission(token_, AdmissionKind::Job);
  }
  token_.clear();
  host_session_.value.clear();
}

/** @copydoc SessionRegistry::CloseGeneration::CloseGeneration */
SessionRegistry::CloseGeneration::CloseGeneration()
    : terminal_status(host_close_not_started_status()) {}  // NOLINT

/** @copydoc SessionRegistry::CloseClaim::CloseClaim */
SessionRegistry::CloseClaim::CloseClaim(
    SessionRegistry* owner, std::string token, GraphSessionId host_session,
    std::shared_ptr<CloseGeneration> generation, Role role)
    : owner_(owner),                           // NOLINT
      token_(std::move(token)),                // NOLINT
      host_session_(std::move(host_session)),  // NOLINT
      generation_(std::move(generation)),      // NOLINT
      role_(role) {}                           // NOLINT

/** @copydoc SessionRegistry::CloseClaim::~CloseClaim */
SessionRegistry::CloseClaim::~CloseClaim() noexcept {
  reset();
}

/** @copydoc SessionRegistry::CloseClaim::CloseClaim */
SessionRegistry::CloseClaim::CloseClaim(CloseClaim&& other) noexcept
    : owner_(std::exchange(other.owner_, nullptr)),         // NOLINT
      token_(std::move(other.token_)),                      // NOLINT
      host_session_(std::move(other.host_session_)),        // NOLINT
      generation_(std::move(other.generation_)),            // NOLINT
      role_(other.role_),                                   // NOLINT
      host_close_started_(other.host_close_started_),       // NOLINT
      host_close_completed_(other.host_close_completed_) {  // NOLINT
  other.host_close_started_ = false;
  other.host_close_completed_ = false;
}

/** @copydoc SessionRegistry::CloseClaim::operator= */
SessionRegistry::CloseClaim& SessionRegistry::CloseClaim::operator=(
    CloseClaim&& other) noexcept {
  if (this != &other) {
    reset();
    owner_ = std::exchange(other.owner_, nullptr);
    token_ = std::move(other.token_);
    host_session_ = std::move(other.host_session_);
    generation_ = std::move(other.generation_);
    role_ = other.role_;
    host_close_started_ = other.host_close_started_;
    host_close_completed_ = other.host_close_completed_;
    other.host_close_started_ = false;
    other.host_close_completed_ = false;
  }
  return *this;
}

/** @copydoc SessionRegistry::CloseClaim::host_session */
const GraphSessionId& SessionRegistry::CloseClaim::host_session()
    const noexcept {  // NOLINT
  return host_session_;
}

/** @copydoc SessionRegistry::CloseClaim::role */
SessionRegistry::CloseClaim::Role SessionRegistry::CloseClaim::role() const {
  if (!active()) {
    throw std::logic_error("Inactive daemon close claim has no role.");
  }
  return role_;
}

/** @copydoc SessionRegistry::CloseClaim::generation */
std::uint64_t SessionRegistry::CloseClaim::generation() const {
  if (!active() || generation_->generation == 0U) {
    throw std::logic_error(
        "Inactive daemon close claim has no generation identity.");
  }
  return generation_->generation;
}

/** @copydoc SessionRegistry::CloseClaim::mark_host_close_started */
void SessionRegistry::CloseClaim::mark_host_close_started() noexcept {
  if (!active() || role_ != Role::Owner || host_close_started_) {
    std::terminate();
  }
  owner_->mark_host_close_started(token_, generation_);
  host_close_started_ = true;
}

/** @copydoc SessionRegistry::CloseClaim::complete_host_close */
void SessionRegistry::CloseClaim::complete_host_close(
    OperationStatus status) noexcept {
  if (!active() || role_ != Role::Owner || !host_close_started_ ||
      host_close_completed_) {
    std::terminate();
  }
  owner_->complete_host_close(token_, generation_, std::move(status));
  host_close_completed_ = true;
}

/** @copydoc SessionRegistry::CloseClaim::wait_result */
OperationStatus SessionRegistry::CloseClaim::wait_result() const {
  if (!active()) {
    throw std::logic_error(
        "Inactive daemon close claim has no generation result.");
  }
  return owner_->wait_close_result(generation_);
}

/** @copydoc SessionRegistry::CloseClaim::active */
bool SessionRegistry::CloseClaim::active() const noexcept {
  return owner_ != nullptr && generation_ != nullptr;
}

/** @copydoc SessionRegistry::CloseClaim::reset */
void SessionRegistry::CloseClaim::reset() noexcept {
  SessionRegistry* owner = std::exchange(owner_, nullptr);
  if (owner != nullptr) {
    owner->release_close_claim(token_, generation_, role_ == Role::Owner,
                               host_close_started_, host_close_completed_);
  }
  token_.clear();
  host_session_.value.clear();
  generation_.reset();
  role_ = Role::Joiner;
  host_close_started_ = false;
  host_close_completed_ = false;
}

/** @copydoc SessionRegistry::SessionRegistry() */
SessionRegistry::SessionRegistry() : SessionRegistry(generate_opaque_id) {}

/** @copydoc SessionRegistry::SessionRegistry(TokenGenerator) */
SessionRegistry::SessionRegistry(TokenGenerator generator)
    : token_generator_(std::move(generator)) {
  if (!token_generator_) {
    token_generator_ = generate_opaque_id;
  }
}

/** @copydoc SessionRegistry::reserve */
IpcResult<IpcSessionId> SessionRegistry::reserve(
    const std::string& session_name) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!accepting_) {
    return {session_not_found_status(), {}};
  }
  if (pending_by_name_.count(session_name) != 0 ||
      active_by_name_.count(session_name) != 0) {
    return {
        failure_status(OperationErrorDomain::Graph,
                       static_cast<std::int32_t>(GraphErrc::InvalidParameter),
                       "invalid_parameter",
                       "session name is already loading or active"),
        {}};
  }
  for (std::size_t attempt = 0; attempt < 128; ++attempt) {
    std::string token = token_generator_();
    if (!valid_opaque_id(token)) {
      return {failure_status(OperationErrorDomain::Daemon, kInternalErrorCode,
                             "internal_error",
                             "opaque token generator returned invalid data"),
              {}};
    }
    if (pending_by_token_.count(token) != 0 ||
        active_by_token_.count(token) != 0) {
      continue;
    }
    pending_by_token_.emplace(token, session_name);
    try {
      pending_by_name_.emplace(session_name, token);
    } catch (...) {
      pending_by_token_.erase(token);
      throw;
    }
    return {ok_status(), {std::move(token)}};
  }
  return {failure_status(OperationErrorDomain::Daemon, kInternalErrorCode,
                         "internal_error",
                         "opaque session id collision limit reached"),
          {}};
}

/** @copydoc SessionRegistry::commit */
OperationStatus SessionRegistry::commit(const IpcSessionId& session_id,
                                        const GraphSessionId& host_session) {
  std::lock_guard<std::mutex> lock(mutex_);
  const auto pending = pending_by_token_.find(session_id.value);
  if (pending == pending_by_token_.end() || host_session.value.empty() ||
      active_by_host_.count(host_session.value) != 0 ||
      active_by_name_.count(pending->second) != 0) {
    return failure_status(OperationErrorDomain::Daemon, kInternalErrorCode,
                          "internal_error",
                          "session registry commit invariant failed");
  }
  const std::string session_name = pending->second;
  const std::shared_ptr<CloseGeneration> close_generation =
      std::make_shared<CloseGeneration>();
  const auto token_inserted = active_by_token_.emplace(
      session_id.value,
      ActiveEntry{host_session, session_name, LifecycleState::Active, 0U, 0U,
                  close_generation});
  if (!token_inserted.second) {
    return failure_status(OperationErrorDomain::Daemon, kInternalErrorCode,
                          "internal_error",
                          "opaque session id was already active");
  }
  bool host_index_inserted = false;
  bool name_index_inserted = false;
  try {
    const auto host_inserted =
        active_by_host_.emplace(host_session.value, session_id.value);
    if (!host_inserted.second) {
      active_by_token_.erase(session_id.value);
      return failure_status(OperationErrorDomain::Daemon, kInternalErrorCode,
                            "internal_error",
                            "Host session was already mapped");
    }
    host_index_inserted = true;
    const auto name_inserted =
        active_by_name_.emplace(session_name, session_id.value);
    if (!name_inserted.second) {
      active_by_host_.erase(host_session.value);
      active_by_token_.erase(session_id.value);
      return failure_status(OperationErrorDomain::Daemon, kInternalErrorCode,
                            "internal_error",
                            "session display name was already active");
    }
    name_index_inserted = true;
  } catch (...) {
    if (name_index_inserted) {
      active_by_name_.erase(session_name);
    }
    if (host_index_inserted) {
      active_by_host_.erase(host_session.value);
    }
    active_by_token_.erase(session_id.value);
    throw;
  }
  pending_by_name_.erase(session_name);
  pending_by_token_.erase(pending);
  return ok_status();
}

/** @copydoc SessionRegistry::rollback */
void SessionRegistry::rollback(const IpcSessionId& session_id) noexcept {
  std::lock_guard<std::mutex> lock(mutex_);
  const auto pending = pending_by_token_.find(session_id.value);
  if (pending == pending_by_token_.end()) {
    return;
  }
  pending_by_name_.erase(pending->second);
  pending_by_token_.erase(pending);
}

/** @copydoc SessionRegistry::admit_host_call */
IpcResult<SessionRegistry::HostCallAdmission> SessionRegistry::admit_host_call(
    const IpcSessionId& session_id) {
  std::lock_guard<std::mutex> lock(mutex_);
  const auto found = active_by_token_.find(session_id.value);
  if (!accepting_ || found == active_by_token_.end() ||
      found->second.lifecycle != LifecycleState::Active) {
    return {session_not_found_status(), {}};
  }
  if (found->second.admitted_host_calls ==
      std::numeric_limits<std::size_t>::max()) {
    return {failure_status(OperationErrorDomain::Daemon, kInternalErrorCode,
                           "internal_error",
                           "session Host-call admission count exhausted"),
            {}};
  }
  HostCallAdmission admission(this, found->first, found->second.host_session);
  ++found->second.admitted_host_calls;
  return {ok_status(), std::move(admission)};
}

/** @copydoc SessionRegistry::admit_job */
IpcResult<SessionRegistry::JobAdmission> SessionRegistry::admit_job(
    const IpcSessionId& session_id) {
  std::lock_guard<std::mutex> lock(mutex_);
  const auto found = active_by_token_.find(session_id.value);
  if (!accepting_ || found == active_by_token_.end() ||
      found->second.lifecycle != LifecycleState::Active) {
    return {session_not_found_status(), {}};
  }
  if (found->second.admitted_jobs == std::numeric_limits<std::size_t>::max()) {
    return {failure_status(OperationErrorDomain::Daemon, kInternalErrorCode,
                           "internal_error",
                           "session job admission count exhausted"),
            {}};
  }
  JobAdmission admission(this, found->first, found->second.host_session);
  ++found->second.admitted_jobs;
  return {ok_status(), std::move(admission)};
}

/** @copydoc SessionRegistry::begin_close */
IpcResult<SessionRegistry::CloseClaim> SessionRegistry::begin_close(
    const IpcSessionId& session_id) {
  return begin_close_impl(session_id, false);
}

/** @copydoc SessionRegistry::begin_shutdown_close */
IpcResult<SessionRegistry::CloseClaim> SessionRegistry::begin_shutdown_close(
    const IpcSessionId& session_id) {
  return begin_close_impl(session_id, true);
}

/** @copydoc SessionRegistry::begin_close_impl */
IpcResult<SessionRegistry::CloseClaim> SessionRegistry::begin_close_impl(
    const IpcSessionId& session_id, bool allow_stopped_admission) {
  std::unique_lock<std::mutex> lock(mutex_);
  auto found = active_by_token_.find(session_id.value);
  if ((!accepting_ && !allow_stopped_admission) ||
      found == active_by_token_.end()) {
    return {session_not_found_status(), {}};
  }
  const std::shared_ptr<CloseGeneration> generation =
      found->second.close_generation;
  if (generation == nullptr) {
    return {
        failure_status(OperationErrorDomain::Daemon, kInternalErrorCode,
                       "internal_error", "session close generation is missing"),
        {}};
  }

  std::string token = found->first;
  GraphSessionId host_session = found->second.host_session;
  bool owns_backend_progression = false;
  if (found->second.lifecycle == LifecycleState::Active) {
    const bool joins_completed_preinvocation_failure =
        generation->state == CloseGeneration::State::HostCloseNotStarted &&
        generation->participants != 0U;
    if (!joins_completed_preinvocation_failure) {
      if ((generation->state != CloseGeneration::State::Idle &&
           generation->state != CloseGeneration::State::HostCloseNotStarted) ||
          generation->participants != 0U) {
        return {failure_status(OperationErrorDomain::Daemon, kInternalErrorCode,
                               "internal_error",
                               "session close generation cannot be selected"),
                {}};
      }
      if (generation->generation == std::numeric_limits<std::uint64_t>::max()) {
        return {failure_status(OperationErrorDomain::Daemon, kInternalErrorCode,
                               "internal_error",
                               "session close generation is exhausted"),
                {}};
      }
      ++generation->generation;
      generation->state = CloseGeneration::State::Preparing;
      found->second.lifecycle = LifecycleState::Closing;
      owns_backend_progression = true;
    }
  } else if (generation->state != CloseGeneration::State::Preparing &&
             generation->state != CloseGeneration::State::HostInvoked) {
    return {failure_status(OperationErrorDomain::Daemon, kInternalErrorCode,
                           "internal_error",
                           "closing session has no live generation"),
            {}};
  }

  if (!owns_backend_progression) {
    if (generation->participants == std::numeric_limits<std::size_t>::max()) {
      return {failure_status(OperationErrorDomain::Daemon, kInternalErrorCode,
                             "internal_error",
                             "session close participant count exhausted"),
              {}};
    }
    ++generation->participants;
    CloseClaim claim(this, std::move(token), std::move(host_session),
                     generation, CloseClaim::Role::Joiner);
    return {ok_status(), std::move(claim)};
  }

  try {
    lifecycle_cv_.wait(lock, [this, &session_id] {
      const auto current = active_by_token_.find(session_id.value);
      return current == active_by_token_.end() ||
             (current->second.admitted_host_calls == 0 &&
              current->second.admitted_jobs == 0);
    });
  } catch (...) {
    found = active_by_token_.find(session_id.value);
    if (found != active_by_token_.end() &&
        found->second.lifecycle == LifecycleState::Closing &&
        found->second.close_generation == generation &&
        generation->state == CloseGeneration::State::Preparing) {
      found->second.lifecycle = LifecycleState::Active;
      generation->state = CloseGeneration::State::HostCloseNotStarted;
    }
    lock.unlock();
    lifecycle_cv_.notify_all();
    return {generation->terminal_status, {}};
  }
  found = active_by_token_.find(session_id.value);
  if (found == active_by_token_.end() ||
      found->second.lifecycle != LifecycleState::Closing ||
      found->second.close_generation != generation ||
      generation->state != CloseGeneration::State::Preparing) {
    std::terminate();
  }
  if (generation->participants == std::numeric_limits<std::size_t>::max()) {
    found->second.lifecycle = LifecycleState::Active;
    generation->state = CloseGeneration::State::HostCloseNotStarted;
    lock.unlock();
    lifecycle_cv_.notify_all();
    return {generation->terminal_status, {}};
  }
  ++generation->participants;
  CloseClaim claim(this, std::move(token), std::move(host_session), generation,
                   CloseClaim::Role::Owner);
  return {ok_status(), std::move(claim)};
}

/** @copydoc SessionRegistry::start_admission */
void SessionRegistry::start_admission() noexcept {
  std::lock_guard<std::mutex> lock(mutex_);
  accepting_ = true;
}

/** @copydoc SessionRegistry::stop_admission */
void SessionRegistry::stop_admission() noexcept {
  std::lock_guard<std::mutex> lock(mutex_);
  accepting_ = false;
}

/** @copydoc SessionRegistry::reconcile */
IpcResult<std::vector<GraphSessionSummary>> SessionRegistry::reconcile(
    const std::vector<GraphSessionId>& host_sessions) const {
  std::set<std::string> host_values;
  for (const GraphSessionId& session : host_sessions) {
    if (!host_values.insert(session.value).second) {
      return {failure_status(OperationErrorDomain::Daemon, kInternalErrorCode,
                             "internal_error",
                             "Host list contains duplicate session ids"),
              {}};
    }
  }

  std::lock_guard<std::mutex> lock(mutex_);
  if (active_by_token_.size() != active_by_host_.size() ||
      active_by_token_.size() != active_by_name_.size() ||
      host_values.size() != active_by_host_.size()) {
    return {failure_status(OperationErrorDomain::Daemon, kInternalErrorCode,
                           "internal_error",
                           "Host and daemon session registry disagree"),
            {}};
  }
  for (const std::string& host_value : host_values) {
    if (active_by_host_.count(host_value) == 0) {
      return {failure_status(OperationErrorDomain::Daemon, kInternalErrorCode,
                             "internal_error",
                             "Host and daemon session registry disagree"),
              {}};
    }
  }

  std::vector<GraphSessionSummary> summaries;
  summaries.reserve(active_by_token_.size());
  for (const auto& row : active_by_token_) {
    if (row.second.lifecycle != LifecycleState::Active) {
      continue;
    }
    summaries.push_back({{row.first}, row.second.session_name});
  }
  std::sort(
      summaries.begin(), summaries.end(),
      [](const GraphSessionSummary& left, const GraphSessionSummary& right) {
        return std::tie(left.session_name, left.session_id.value) <
               std::tie(right.session_name, right.session_id.value);
      });
  return {ok_status(), std::move(summaries)};
}

/** @copydoc SessionRegistry::active_sessions */
std::vector<std::pair<IpcSessionId, GraphSessionId>>
SessionRegistry::active_sessions() const {
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<std::pair<IpcSessionId, GraphSessionId>> result;
  result.reserve(active_by_token_.size());
  for (const auto& row : active_by_token_) {
    result.push_back({IpcSessionId{row.first}, row.second.host_session});
  }
  return result;
}

/** @copydoc SessionRegistry::clear */
void SessionRegistry::clear() noexcept {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    pending_by_token_.clear();
    pending_by_name_.clear();
    active_by_token_.clear();
    active_by_host_.clear();
    active_by_name_.clear();
  }
  lifecycle_cv_.notify_all();
}

/** @copydoc SessionRegistry::release_admission */
void SessionRegistry::release_admission(const std::string& token,
                                        AdmissionKind kind) noexcept {
  bool wake_close = false;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = active_by_token_.find(token);
    if (found == active_by_token_.end()) {
      return;
    }
    std::size_t* count = kind == AdmissionKind::HostCall
                             ? &found->second.admitted_host_calls
                             : &found->second.admitted_jobs;
    if (*count == 0) {
      return;
    }
    --(*count);
    wake_close = found->second.lifecycle == LifecycleState::Closing &&
                 found->second.admitted_host_calls == 0 &&
                 found->second.admitted_jobs == 0;
  }
  if (wake_close) {
    lifecycle_cv_.notify_all();
  }
}

/** @copydoc SessionRegistry::mark_host_close_started */
void SessionRegistry::mark_host_close_started(
    const std::string& token,
    const std::shared_ptr<CloseGeneration>& generation) noexcept {
  try {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = active_by_token_.find(token);
    if (found == active_by_token_.end() ||
        found->second.lifecycle != LifecycleState::Closing ||
        found->second.close_generation != generation || generation == nullptr ||
        generation->state != CloseGeneration::State::Preparing) {
      std::terminate();
    }
    generation->state = CloseGeneration::State::HostInvoked;
  } catch (...) {
    std::terminate();
  }
}

/** @copydoc SessionRegistry::complete_host_close */
void SessionRegistry::complete_host_close(
    const std::string& token,
    const std::shared_ptr<CloseGeneration>& generation,
    OperationStatus status) noexcept {
  const bool terminal_status =
      status.ok || checked_graph_error_code(status) == GraphErrc::NotFound;
  if (!terminal_status) {
    std::terminate();
  }
  try {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      const auto found = active_by_token_.find(token);
      if (found == active_by_token_.end() ||
          found->second.lifecycle != LifecycleState::Closing ||
          found->second.close_generation != generation ||
          generation == nullptr ||
          generation->state != CloseGeneration::State::HostInvoked) {
        std::terminate();
      }
      generation->terminal_status = std::move(status);
      generation->state = CloseGeneration::State::Completed;
      active_by_name_.erase(found->second.session_name);
      active_by_host_.erase(found->second.host_session.value);
      active_by_token_.erase(found);
    }
    lifecycle_cv_.notify_all();
  } catch (...) {
    std::terminate();
  }
}

/** @copydoc SessionRegistry::wait_close_result */
OperationStatus SessionRegistry::wait_close_result(
    const std::shared_ptr<CloseGeneration>& generation) {
  if (generation == nullptr) {
    throw std::invalid_argument(
        "Daemon close result requires a generation record.");
  }
  std::unique_lock<std::mutex> lock(mutex_);
  lifecycle_cv_.wait(lock, [&generation] {
    return generation->state == CloseGeneration::State::Completed ||
           generation->state == CloseGeneration::State::HostCloseNotStarted;
  });
  return generation->terminal_status;
}

/** @copydoc SessionRegistry::release_close_claim */
void SessionRegistry::release_close_claim(
    const std::string& token,
    const std::shared_ptr<CloseGeneration>& generation, bool owner,
    bool host_close_started, bool host_close_completed) noexcept {
  bool notify = false;
  try {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (generation == nullptr || generation->participants == 0U) {
        std::terminate();
      }
      if (owner && host_close_started && !host_close_completed) {
        std::terminate();
      }
      if (owner && !host_close_started && !host_close_completed) {
        const auto found = active_by_token_.find(token);
        if (found == active_by_token_.end() ||
            found->second.lifecycle != LifecycleState::Closing ||
            found->second.close_generation != generation ||
            generation->state != CloseGeneration::State::Preparing) {
          std::terminate();
        }
        found->second.lifecycle = LifecycleState::Active;
        generation->state = CloseGeneration::State::HostCloseNotStarted;
        notify = true;
      }
      --generation->participants;
    }
    if (notify) {
      lifecycle_cv_.notify_all();
    }
  } catch (...) {
    std::terminate();
  }
}

}  // namespace ps::ipc::internal
