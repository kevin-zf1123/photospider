#pragma once

#include <cassert>
#include <functional>
#include <mutex>
#include <set>
#include <utility>

#include "photospider/core/checked_math.hpp"
#include "photospider/plugin/result_program.hpp"
#include "plugin/failure_latch.hpp"

namespace ps::plugin_internal {
// Retain weak provenance across Need rounds. A legal view keeps its source
// alive; the bound itself must not prolong source payload ownership.
class ResultPayloadBound final {
 public:
  explicit ResultPayloadBound(std::uint64_t maximum, bool cumulative = false)
      : maximum_(maximum), cumulative_(cumulative) {}
  Status status() const { return failure_.snapshot(); }
  Status capture(const ResultProgramPhase& phase) try {
    std::lock_guard<std::mutex> lock(mutex_);
    ResourceAllocationScope scope(phase.resources);
    if (sources_.empty())
      sources_ =
          SourceSet(std::less<std::pair<std::uint64_t, std::uint64_t>>{},
                    ResourceAllocator<std::pair<std::uint64_t, std::uint64_t>>(
                        phase.resources));
    if (borrowed_.empty())
      borrowed_ = OwnerSet(
          std::owner_less<std::weak_ptr<const CpuStorage>>{},
          ResourceAllocator<std::weak_ptr<const CpuStorage>>(phase.resources));
    auto previous = failure_.snapshot();
    if (!previous.ok())
      return previous;
    auto add = [&](const ResultRef& result) -> Status {
      if (!result.valid())
        return Status::success();
      auto status = work(phase, 1);
      if (!status.ok())
        return status;
      auto facts = result.descriptor(false);
      if (!facts.ok())
        return facts.status();
      const auto key =
          std::make_pair(result.object_id(), facts.value().revision());
      if (sources_.count(key) || (base_ && base_->sources_.count(key)))
        return Status::success();
      auto storage =
          result.cache_storage([&](auto n) { return work(phase, n); }, false);
      if (!storage.ok())
        return storage.status();
      for (const auto& allocation : storage.value())
        if (!base_ || !base_->borrowed_.count(allocation.lifetime))
          borrowed_.insert(allocation.lifetime);
      sources_.insert(key);
      return Status::success();
    };
    for (const auto& input : phase.results) {
      auto status = add(input.second);
      if (!status.ok())
        return failure_.record(status);
    }
    if (phase.shared_stages) {
      for (const auto& input : *phase.shared_stages) {
        auto status = add(input.second);
        if (!status.ok())
          return failure_.record(status);
      }
    }
    if (phase.tensors) {
      for (const auto& entry : *phase.tensors) {
        const auto& input = entry.second;
        if (!input.payload_authorized_)
          continue;
        auto status = add(input.result_);
        if (!status.ok())
          return failure_.record(status);
      }
    }
    return Status::success();
  } catch (const std::bad_alloc&) {
    return failure_.record(Status{ErrorCode::ResourceExhausted, {}});
  }
  Status import_sources(const ResultPayloadBound& source,
                        const ResultProgramPhase& phase) try {
    if (this == &source)
      return Status::success();
    std::scoped_lock lock(mutex_, source.mutex_);
    ResourceAllocationScope scope(phase.resources);
    auto status = source.failure_.snapshot();
    if (!status.ok())
      return status;
    status = work(phase, source.borrowed_.size());
    if (!status.ok())
      return status;
    if (borrowed_.empty())
      borrowed_ = OwnerSet(
          std::owner_less<std::weak_ptr<const CpuStorage>>{},
          ResourceAllocator<std::weak_ptr<const CpuStorage>>(phase.resources));
    for (const auto& owner : source.borrowed_)
      if (!base_ || !base_->borrowed_.count(owner))
        borrowed_.insert(owner);
    return Status::success();
  } catch (const std::bad_alloc&) {
    return Status{ErrorCode::ResourceExhausted, {}};
  }
  // A cache candidate holds the live ledger lock and stages only new entries.
  // No prior owner directory is copied and no failure reaches the live latch.
  // The coordinator keeps the live ledger alive until commit or abandonment.
  Result<std::shared_ptr<ResultPayloadBound>> candidate(
      const ResultProgramPhase& phase) try {
    using Answer = Result<std::shared_ptr<ResultPayloadBound>>;
    std::unique_lock<std::mutex> lock(mutex_);
    auto status = failure_.snapshot();
    if (!status.ok())
      return Answer(status);
    auto delta = std::allocate_shared<ResultPayloadBound>(
        ResourceAllocator<ResultPayloadBound>(phase.resources), maximum_,
        cumulative_);
    delta->base_ = this;
    delta->base_lock_ = std::move(lock);
    delta->total_ = total_;
    return Answer(std::move(delta));
  } catch (const std::bad_alloc&) {
    return Result<std::shared_ptr<ResultPayloadBound>>(
        Status{ErrorCode::ResourceExhausted, {}});
  }
  // The validated candidate owns mutex_ until this no-allocation commit.
  // Keeping this ledger's address stable also permits concurrent poll capture.
  void commit_candidate(ResultPayloadBound& delta) {
    assert(delta.base_ == this && delta.base_lock_.owns_lock());
    if (!delta.sources_.empty()) {
      if (sources_.empty())
        sources_ = std::move(delta.sources_);
      else
        sources_.merge(delta.sources_);
    }
    if (!delta.borrowed_.empty()) {
      if (borrowed_.empty())
        borrowed_ = std::move(delta.borrowed_);
      else
        borrowed_.merge(delta.borrowed_);
    }
    if (!delta.owners_.empty()) {
      if (owners_.empty())
        owners_ = std::move(delta.owners_);
      else
        owners_.merge(delta.owners_);
    }
    total_ = delta.total_;
    delta.base_ = nullptr;
    delta.base_lock_.unlock();
  }
  Status check(const ResultRef& result, const ResultProgramPhase& phase) try {
    std::lock_guard<std::mutex> lock(mutex_);
    ResourceAllocationScope scope(phase.resources);
    auto previous = failure_.snapshot();
    if (!previous.ok())
      return previous;
    if (!result.valid() || !result.owned_by(phase.resources))
      return failure_.record(
          Status{ErrorCode::InvalidArgument,
                 "invalid Result output payload provenance",
                 FailureReason::None,
                 {FailureOrigin::Protocol, FailureScope::Group}});
    auto consume = [&](std::uint64_t amount) { return work(phase, amount); };
    auto output = result.cache_storage(consume, false);
    if (!output.ok())
      return failure_.record(output.status());
    if (owners_.empty())
      owners_ = OwnerSet(
          std::owner_less<std::weak_ptr<const CpuStorage>>{},
          ResourceAllocator<std::weak_ptr<const CpuStorage>>(phase.resources));
    std::uint64_t bytes = cumulative_ ? total_ : 0;
    for (const auto& allocation : output.value()) {
      auto charged = consume(1);
      if (!charged.ok())
        return failure_.record(charged);
      if (borrowed_.count(allocation.lifetime) ||
          (base_ && base_->borrowed_.count(allocation.lifetime)) ||
          (cumulative_ &&
           (owners_.count(allocation.lifetime) ||
            (base_ && base_->owners_.count(allocation.lifetime)))))
        continue;
      if (!core_internal::can_add(allocation.bytes, bytes, maximum_))
        return failure_.record(
            Status{ErrorCode::ResourceExhausted,
                   "new Result backing exceeds its declared payload bound",
                   FailureReason::CapacityLimit,
                   {FailureOrigin::Resource, FailureScope::Group}});
      bytes += allocation.bytes;
      if (cumulative_)
        owners_.insert(allocation.lifetime);
    }
    if (cumulative_)
      total_ = bytes;
    return Status::success();
  } catch (const std::bad_alloc&) {
    return failure_.record(Status{ErrorCode::ResourceExhausted, {}});
  }

 private:
  static Status work(const ResultProgramPhase& phase, std::uint64_t amount) {
    if (phase.query.cancellation.cancelled())
      return Status{ErrorCode::Cancelled, {}};
    return phase.consume_work ? phase.consume_work(amount)
                              : phase.resources.consume({amount});
  }
  mutable std::mutex mutex_;
  using OwnerSet = std::set<std::weak_ptr<const CpuStorage>,
                            std::owner_less<std::weak_ptr<const CpuStorage>>,
                            ResourceAllocator<std::weak_ptr<const CpuStorage>>>;
  OwnerSet owners_;
  std::uint64_t maximum_, total_ = 0;
  bool cumulative_;
  using SourceSet =
      std::set<std::pair<std::uint64_t, std::uint64_t>,
               std::less<std::pair<std::uint64_t, std::uint64_t>>,
               ResourceAllocator<std::pair<std::uint64_t, std::uint64_t>>>;
  SourceSet sources_;
  OwnerSet borrowed_;
  FailureLatch failure_;
  ResultPayloadBound* base_ = nullptr;
  std::unique_lock<std::mutex> base_lock_;
};
}  // namespace ps::plugin_internal
