#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <utility>

#include "core/stored_failure.hpp"
#include "photospider/data/result.hpp"

namespace ps::data_internal {

// Owns admission, static schema/identity and synchronized publication state.
// The Result bundle destroys tensor/field backings before this base, keeping
// budget and resource capabilities alive until their physical storage retires.
// Capture and mutation borrow mutex; failure is sticky and revision advances
// only after backing admission and certification have completed.
struct ResultPublicationState {
  explicit ResultPublicationState(ResourceBudget value)
      : budget(std::move(value)) {}
  ResourceBudget budget;
  ResourceLease lease;
  mutable std::timed_mutex mutex;
  SchemaTemplate schema;
  ResourceString scope;
  std::shared_ptr<TensorBudget> image_budget;
  ResultGrowthLimits limits;
  std::uint64_t object = 0, revision = 1;
  core_internal::StoredFailure failure;
  std::atomic<bool> cancelled_publication{false};
  std::atomic<bool> kernel_active{false};
  Status reject_active_mutation() {
    if (!kernel_active.load())
      return Status::success();
    if (status().ok()) {
      auto rejected = Status{ErrorCode::InvalidArgument,
                             "producer mutation during active tensor write"};
      rejected.detail = {FailureOrigin::Protocol, FailureScope::Group};
      failure.record(rejected);
    }
    return status();
  }
  Status status() const {
    if (!failure.ok())
      return failure.status();
    return cancelled_publication.load() ? Status{ErrorCode::Cancelled, {}}
                                        : Status::success();
  }
  bool complete = false;
};
}  // namespace ps::data_internal
