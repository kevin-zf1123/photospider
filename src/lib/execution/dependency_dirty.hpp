#pragma once

#include <cstdint>
#include <deque>
#include <map>
#include <mutex>
#include <optional>
#include <utility>

#include "photospider/data/footprint.hpp"

namespace ps::execution_internal {
/** @brief One incremental direct-edge propagation item in a fixed generation.
 * @note The owning demand coordinator retains certificates independently of
 * cached pixel Values, routes this delta with certificate.transpose(), and
 * aborts the generation if any downstream enqueue fails.
 */
struct DirtyDelta final {
  std::uint64_t record = 0;
  Footprint changed;
};
/** @brief Linearizes growing dirty sets; queued is not a processed flag.
 * @note A queue belongs to one immutable certificate generation. Replacing
 * certificates creates another queue. This object owns no pixel/worker state.
 */
class DirtyDeltaQueue final {
 public:
  explicit DirtyDeltaQueue(FootprintLimits limits = {})
      : limits_(std::move(limits)) {}
  Status receive(std::uint64_t record, const Footprint& dirty) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (failure_)
      return *failure_;
    if (!dirty.valid())
      return fail_locked(
          Status::failure(ErrorCode::InvalidArgument, "invalid dirty domain"));
    auto found = entries_.find(record);
    if (found == entries_.end()) {
      if (entries_.size() >= limits_.maximum_boxes)
        return fail_locked(Status::failure(ErrorCode::ResourceExhausted,
                                           "dirty record limit"));
      auto empty = Footprint::none(dirty.shape(), limits_);
      if (!empty.ok())
        return fail_locked(empty.status());
      found =
          entries_.emplace(record, Entry{empty.value(), empty.value(), false})
              .first;
    }
    auto accumulated = found->second.accumulated.unite(dirty, limits_);
    if (!accumulated.ok())
      return fail_locked(accumulated.status());
    if (accumulated.value() == found->second.accumulated)
      return Status::success();
    // Allocate queue storage before committing the changed set/queued flag.
    if (!found->second.queued)
      ready_.push_back(record);
    found->second.accumulated = accumulated.take_value();
    found->second.queued = true;
    return Status::success();
  }
  Result<std::optional<DirtyDelta>> take() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (failure_)
      return Result<std::optional<DirtyDelta>>(*failure_);
    if (ready_.empty())
      return Result<std::optional<DirtyDelta>>(std::nullopt);
    const auto record = ready_.front();
    auto& entry = entries_.at(record);
    auto delta = entry.accumulated.subtract(entry.propagated, limits_);
    if (!delta.ok())
      return Result<std::optional<DirtyDelta>>(fail_locked(delta.status()));
    auto propagated = entry.accumulated;
    DirtyDelta item{record, delta.take_value()};
    entry.propagated = std::move(propagated);
    entry.queued = false;
    ready_.pop_front();
    return Result<std::optional<DirtyDelta>>(std::move(item));
  }
  Result<Footprint> accumulated(std::uint64_t record) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (failure_)
      return Result<Footprint>(*failure_);
    auto found = entries_.find(record);
    if (found == entries_.end())
      return Result<Footprint>(
          Status::failure(ErrorCode::NotFound, "unknown dirty record"));
    return Result<Footprint>(found->second.accumulated);
  }
  Status fail(Status status) {
    std::lock_guard<std::mutex> lock(mutex_);
    return fail_locked(std::move(status));
  }

 private:
  Status fail_locked(Status status) {
    if (status.ok())
      status = Status::failure(ErrorCode::Internal, "dirty generation failed");
    if (!failure_)
      failure_ = std::move(status);
    return *failure_;
  }
  struct Entry {
    Footprint accumulated, propagated;
    bool queued = false;
  };
  FootprintLimits limits_;
  mutable std::mutex mutex_;
  std::map<std::uint64_t, Entry> entries_;
  std::deque<std::uint64_t> ready_;
  std::optional<Status> failure_;
};
}  // namespace ps::execution_internal
