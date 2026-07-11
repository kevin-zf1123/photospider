#include "ipc/session_registry.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <set>
#include <stdexcept>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "ipc/codec.hpp"

namespace ps::ipc::internal {

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
  const auto token_inserted = active_by_token_.emplace(
      session_id.value, ActiveEntry{host_session, session_name});
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

/** @copydoc SessionRegistry::resolve */
std::optional<GraphSessionId> SessionRegistry::resolve(
    const IpcSessionId& session_id) const {
  std::lock_guard<std::mutex> lock(mutex_);
  const auto found = active_by_token_.find(session_id.value);
  if (found == active_by_token_.end()) {
    return std::nullopt;
  }
  return found->second.host_session;
}

/** @copydoc SessionRegistry::erase */
void SessionRegistry::erase(const IpcSessionId& session_id) noexcept {
  std::lock_guard<std::mutex> lock(mutex_);
  const auto found = active_by_token_.find(session_id.value);
  if (found == active_by_token_.end()) {
    return;
  }
  active_by_name_.erase(found->second.session_name);
  active_by_host_.erase(found->second.host_session.value);
  active_by_token_.erase(found);
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
  std::lock_guard<std::mutex> lock(mutex_);
  pending_by_token_.clear();
  pending_by_name_.clear();
  active_by_token_.clear();
  active_by_host_.clear();
  active_by_name_.clear();
}

}  // namespace ps::ipc::internal
