#include "execution/structured_joint_state.hpp"

#include <algorithm>
#include <memory>
#include <utility>

namespace ps::execution_internal {
StructuredJointState::StructuredJointState(const ResourceBudget& root)
    : payload(std::make_shared<core_internal::PayloadObservation>()),
      members(ResourceAllocator<Member>(root)),
      pending_members(ResourceAllocator<std::weak_ptr<JointParticipant>>(root)),
      failure(std::allocate_shared<plugin_internal::FailureLatch>(
          ResourceAllocator<plugin_internal::FailureLatch>(root))) {}
void StructuredJointState::refresh() const {
  bool active = false;
  for (const auto& member : members) {
    if (member.done->load())
      continue;
    member.shared.refresh();
    active |= !member.token.cancelled();
  }
  if (!active)
    cancellation.cancel();
}
bool StructuredJointState::peers() const {
  for (const auto& member : members)
    if (!member.done->load() && !member.token.cancelled() &&
        member.shared.continue_for_peers())
      return true;
  return false;
}
bool StructuredJointState::complete() const {
  return std::all_of(members.begin(), members.end(),
                     [](const auto& member) { return member.done->load(); });
}
void StructuredJointState::retire_submission(StructuredJointHost& host) {
  if (auto carrier = pending_members.front().lock();
      carrier && host.pending(*carrier)) {
    refresh();
    host.retire_submission(carrier);
  }
  for (const auto& weak : pending_members)
    if (auto member = weak.lock())
      host.clear_submission(*member);
  pending_members.clear();
  pending = false;
}
void StructuredJointState::release(bool fallback, StructuredJointHost& host) {
  if (fallback)
    host.fallback_observed();
  Status restored = Status::success();
  if (fallback && saved_records) {
    host.install_records(std::move(saved_records));
    for (const auto& member : members)
      if (auto actor = member.actor.lock();
          actor && host.state(*actor).complete && restored.ok())
        restored = host.import_completed(*actor);
  }
  continuation = {};
  started = Result<ResultJointContinuation>(Status{ErrorCode::Stale, {}});
  for (const auto& member : members)
    if (auto actor = member.actor.lock()) {
      host.detach(*actor);
      const auto active = [&] {
        auto state = host.state(*actor);
        return !state.complete && state.terminal == ErrorCode::Ok;
      };
      if (!restored.ok() && active())
        host.retire(*actor, restored, false);
      if (fallback && active()) {
        auto status = host.discard_attempt(*actor);
        if (!status.ok()) {
          host.retire(*actor, status, false);
          continue;
        }
      }
      if (fallback && active())
        host.restart(*actor);
    }
}
Status StructuredJointState::fail(const Status& failure,
                                  StructuredJointHost& host) {
  for (const auto& member : members)
    if (auto actor = member.actor.lock()) {
      const auto state = host.state(*actor);
      if (!state.complete && state.terminal == ErrorCode::Ok)
        host.retire(*actor, failure, true);
    }
  release(false, host);
  return Status::success();
}
}  // namespace ps::execution_internal
