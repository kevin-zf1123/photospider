#pragma once

#include <functional>
#include <set>
#include <utility>

#include "photospider/plugin/result_program.hpp"
#include "plugin/failure_latch.hpp"

namespace ps::plugin_internal {
// Retain weak provenance across Need rounds. A legal view keeps its source
// alive; the bound itself must not prolong source payload ownership.
class ResultPayloadBound final {
 public:
  explicit ResultPayloadBound(std::uint64_t maximum) : maximum_(maximum) {}
  Status status() const { return failure_.snapshot(); }
  Status capture(const ResultProgramPhase& phase) try {
    ResourceAllocationScope scope(phase.resources);
    if (sources_.empty())
      sources_ =
          ResourceVector<Source>{ResourceAllocator<Source>(phase.resources)};
    if (input_storage_.empty())
      input_storage_ = ResourceVector<std::weak_ptr<const CpuStorage>>{
          ResourceAllocator<std::weak_ptr<const CpuStorage>>(phase.resources)};
    auto previous = failure_.snapshot();
    if (!previous.ok())
      return previous;
    auto add = [&](const ResultRef& result) -> Status {
      if (!result.valid())
        return Status::success();
      auto status = work(phase, 1 + sources_.size());
      if (!status.ok())
        return status;
      for (const auto& source : sources_)
        if (source.id == result.object_id())
          return Status::success();
      sources_.push_back({result.object_id(), result.weak()});
      return Status::success();
    };
    for (const auto& input : phase.results) {
      auto status = add(input.second);
      if (!status.ok())
        return failure_.record(status);
    }
    if (phase.tensors) {
      for (const auto& entry : *phase.tensors) {
        const auto& input = entry.second;
        if (!input.payload_authorized_)
          continue;
        auto status = add(input.result_);
        if (!status.ok())
          return failure_.record(status);
        for (const auto& piece : input.pieces_) {
          status = add(piece.result);
          if (!status.ok())
            return failure_.record(status);
        }
        for (const auto& backing : input.input_backing_) {
          status = work(phase, 1 + input_storage_.size());
          if (!status.ok())
            return failure_.record(status);
          bool found = false;
          for (const auto& storage : input_storage_)
            if (storage.lock() == backing->value.storage()) {
              found = true;
              break;
            }
          if (!found)
            input_storage_.push_back(backing->value.storage());
        }
      }
    }
    return Status::success();
  } catch (const std::bad_alloc&) {
    return failure_.record(Status{ErrorCode::ResourceExhausted, {}});
  }
  Status check(const ResultRef& result, const ResultProgramPhase& phase) try {
    ResourceAllocationScope scope(phase.resources);
    if (!result.valid() || !result.owned_by(phase.resources))
      return failure_.record(
          Status{ErrorCode::InvalidArgument,
                 "invalid Result output payload provenance",
                 FailureReason::None,
                 {FailureOrigin::Protocol, FailureScope::Group}});
    auto consume = [&](std::uint64_t amount) { return work(phase, amount); };
    auto output = result.cache_storage(consume, false, false);
    if (!output.ok())
      return failure_.record(output.status());
    std::set<const void*, std::less<const void*>,
             ResourceAllocator<const void*>>
        borrowed;
    for (const auto& source : sources_) {
      auto charged = consume(1);
      if (!charged.ok())
        return failure_.record(charged);
      auto owner = source.owner.lock();
      if (!owner.valid())
        continue;
      auto storage = owner.cache_storage(consume, false, false);
      if (!storage.ok())
        return failure_.record(storage.status());
      for (const auto& allocation : storage.value())
        borrowed.insert(allocation.owner);
    }
    for (const auto& weak : input_storage_) {
      auto charged = consume(1);
      if (!charged.ok())
        return failure_.record(charged);
      if (auto storage = weak.lock())
        borrowed.insert(storage.get());
    }
    std::uint64_t bytes = 0;
    for (const auto& allocation : output.value()) {
      auto charged = consume(1);
      if (!charged.ok())
        return failure_.record(charged);
      if (borrowed.count(allocation.owner))
        continue;
      if (allocation.bytes > maximum_ - bytes)
        return failure_.record(
            Status{ErrorCode::ResourceExhausted,
                   "new Result backing exceeds its declared payload bound",
                   FailureReason::CapacityLimit,
                   {FailureOrigin::Resource, FailureScope::Group}});
      bytes += allocation.bytes;
    }
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
  struct Source {
    std::uint64_t id;
    WeakResultRef owner;
  };
  std::uint64_t maximum_;
  ResourceVector<Source> sources_;
  ResourceVector<std::weak_ptr<const CpuStorage>> input_storage_;
  FailureLatch failure_;
};
}  // namespace ps::plugin_internal
