#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>

#include "core/resource_observation.hpp"
#include "execution/dependency_records.hpp"
#include "execution/shared_results.hpp"
#include "photospider/plugin/result_program.hpp"
#include "plugin/failure_latch.hpp"

namespace ps::execution_internal {
// Type-erased participant marker; the Actor remains its owning control block.
class JointParticipant {
 protected:
  ~JointParticipant() = default;
};
struct JointMemberState final {
  bool complete;
  ErrorCode terminal;
};
// This interface is borrowed synchronously. It drives already-owned carrier
// retirement and Actor/Run mutations, but is never retained by the cohort.
class StructuredJointHost {
 public:
  virtual ~StructuredJointHost() = default;
  virtual JointMemberState state(const JointParticipant&) const = 0;
  virtual bool pending(const JointParticipant&) const = 0;
  virtual void retire_submission(const std::shared_ptr<JointParticipant>&) = 0;
  virtual void clear_submission(JointParticipant&) = 0;
  virtual void install_records(std::unique_ptr<DependencyRecords>) = 0;
  virtual Status import_completed(const JointParticipant&) = 0;
  virtual void detach(JointParticipant&) = 0;
  virtual Status discard_attempt(JointParticipant&) = 0;
  virtual void restart(JointParticipant&) = 0;
  virtual void retire(JointParticipant&, const Status&, bool record_poll) = 0;
  virtual void fallback_observed() = 0;
};
// Owns the cohort continuation, rollback record and shared cancellation gate.
// Members and pending carriers are weak: pending task owners retain callbacks,
// and their retirement must precede releasing this state. refresh() only reads
// member-owned cancellation/lease state; it never drives or drains a callback.
struct StructuredJointState {
  struct Member {
    std::size_t index = 0;
    std::weak_ptr<JointParticipant> actor;
    SharedResults::Lease shared;
    CancellationToken token;
    std::shared_ptr<plugin_internal::FailureLatch> failure;
    std::shared_ptr<std::atomic<bool>> done;
  };
  explicit StructuredJointState(const ResourceBudget& root);
  std::shared_ptr<core_internal::PayloadObservation> payload;
  ResourceVector<Member> members;
  ResourceVector<std::weak_ptr<JointParticipant>> pending_members;
  std::shared_ptr<plugin_internal::FailureLatch> failure;
  mutable CancellationSource cancellation;
  std::unique_ptr<DependencyRecords> saved_records;
  ResultJointContinuation continuation;
  Result<ResultJointContinuation> started{Status{ErrorCode::Internal, {}}};
  Result<ResourceVector<ResultJointOutcome>> polled{
      Status{ErrorCode::Internal, {}}};
  bool pending = false, starting = false, entered = false;
  std::uint64_t visit = 0;
  bool contract2 = false;
  std::atomic<bool> driving{false};
  void refresh() const;
  bool peers() const;
  bool complete() const;
  void retire_submission(StructuredJointHost&);
  void release(bool fallback, StructuredJointHost&);
  Status fail(const Status&, StructuredJointHost&);
};
}  // namespace ps::execution_internal
