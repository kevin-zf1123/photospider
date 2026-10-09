#pragma once

#include <cstdint>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <utility>

#include "core/checked_math.hpp"
#include "data/result_host_access.hpp"
#include "data/result_window_access.hpp"
#include "photospider/plugin/result_program.hpp"
#include "plugin/dependency_discovery.hpp"

namespace ps::execution_internal {
// Only the synchronous host invocation installs this capability. It cannot
// escape into an operator continuation or change the public Result identity.
class ResultNativeScope final {
 public:
  using CacheWork = std::function<Status(std::uint64_t)>;
  using Upload = std::function<Result<std::pair<Value, std::uint64_t>>(
      const Value&, const CacheWork&)>;
  ResultNativeScope(const BufferAllocator& allocator, const Upload& upload,
                    const ResourceBudget& resources, FootprintLimits limits,
                    std::uint32_t maximum_requests,
                    std::function<bool(const CpuStorage&)> native_storage = {},
                    bool host_access = false, CacheWork cache_work = {})
      : allocator_(allocator),
        upload_(upload),
        resources_(resources),
        limits_(std::move(limits)),
        maximum_requests_(maximum_requests),
        native_storage_(std::move(native_storage)),
        host_access_(host_access),
        cache_work_(std::move(cache_work)),
        host_owners_(
            ResourceAllocator<std::weak_ptr<const CpuStorage>>(resources)),
        host_scope_([this](const auto& storage) {
          return consume_host_storage(storage);
        }),
        atlases_(std::less<AtlasKey>{},
                 ResourceAllocator<AtlasEntry>(resources)),
        previous_(active_) {
    active_ = this;
  }
  ~ResultNativeScope() { active_ = previous_; }
  ResultNativeScope(const ResultNativeScope&) = delete;
  ResultNativeScope& operator=(const ResultNativeScope&) = delete;
  // Called only when a CPU callback actually obtains a readable backing view.
  // GPU address acquisition and upload hashing do not install host access.
  Status consume_host_storage(
      const std::shared_ptr<const CpuStorage>& storage) {
    if (!host_access_ || !native_storage_ || !storage ||
        !native_storage_(*storage))
      return Status::success();
    // CPU range/tile helpers share this collector and drain before its caller
    // reads diagnostics or retires the scope. Every live backing counts once.
    std::lock_guard<std::mutex> lock(host_mutex_);
    for (const auto& owner : host_owners_) {
      const auto proof = owner.lock();
      if (proof && proof.get() == storage.get())
        return Status::success();
    }
    host_owners_.push_back(storage);
    ++host_accesses;
    return Status::success();
  }
  static Result<Value> input(const ResultTensorReadWindow& window,
                             const std::function<Status(std::uint64_t)>& work,
                             const CancellationToken& cancellation) {
    if (!active_ || !active_->upload_)
      return Result<Value>(Status{ErrorCode::BackendUnavailable, {}});
    auto charged = work(1 + window.region().rank());
    if (!charged.ok())
      return Result<Value>(charged);
    auto source = ResultWindowAccess::affine(window);
    if (!source.ok() && source.status().code != ErrorCode::NotFound)
      return source;
    if (!source.ok()) {
      const ValueDescriptor descriptor{window.spec().descriptor.element_type,
                                       window.spec().sample_shape()};
      const auto count = window.region().element_count();
      const auto width = Value::element_size(descriptor.element_type);
      if (!count.ok() || !core_internal::can_multiply(count.value(), width))
        return Result<Value>(Status{ErrorCode::ResourceExhausted, {}});
      auto lookup = ResultWindowAccess::read_work(window);
      if (!lookup.ok())
        return Result<Value>(lookup.status());
      std::uint64_t per_sample = 0;
      if (!core_internal::checked_add(
              lookup.value(), width + window.region().rank(), &per_sample) ||
          !core_internal::can_multiply(count.value(), per_sample))
        return Result<Value>(Status{ErrorCode::ResourceExhausted, {}});
      charged = work(count.value() * per_sample);
      if (!charged.ok())
        return Result<Value>(charged);
      auto allocated = MutableValue::allocate(descriptor, window.region(),
                                              active_->allocator_);
      if (!allocated.ok())
        return Result<Value>(allocated.status());
      auto writer = allocated.take_value();
      auto samples =
          Footprint::from_regions(descriptor.shape, {window.region()});
      if (!samples.ok())
        return Result<Value>(samples.status());
      std::uint64_t offset = 0;
      auto status = samples.value().visit(
          [&](const auto& at) {
            auto active = work(0);
            if (!active.ok())
              return active;
            auto row = window.row_run(at);
            if (!row.ok())
              return row.status();
            std::memcpy(writer.data() + offset, row.value().data, width);
            offset += width;
            return Status::success();
          },
          count.value(), cancellation);
      if (!status.ok())
        return Result<Value>(status);
      source = std::move(writer).publish();
      ++active_->transfers;
      active_->transferred_bytes += count.value() * width;
    }
    if (!source.ok())
      return source;
    auto uploaded = active_->upload_(source.value(), active_->cache_work_);
    if (!uploaded.ok())
      return Result<Value>(uploaded.status());
    if (uploaded.value().second) {
      ++active_->transfers;
      active_->transferred_bytes += uploaded.value().second;
    }
    return Result<Value>(std::move(uploaded.value().first));
  }
  static Result<MutableBuffer> output(std::uint64_t bytes) {
    if (!active_ || !active_->upload_)
      return Result<MutableBuffer>(Status{ErrorCode::BackendUnavailable, {}});
    return active_->allocator_.allocate(bytes);
  }
  static Result<ResourceVector<ResultTensorNeed>> discover(
      const ResultProgramQuery& query, std::uint32_t capacity,
      std::uint32_t candidates,
      const std::function<Status(const ResultGpuRequestTable&)>& compute,
      const std::function<Status(std::uint64_t)>& work) {
    using Answer = Result<ResourceVector<ResultTensorNeed>>;
    if (!active_ || !active_->upload_ || query.backend != Backend::Gpu ||
        !compute || !candidates || active_->discovering_)
      return Answer(Status{ErrorCode::InvalidArgument,
                           "invalid Result GPU discovery callback"});
    if (!capacity || capacity > 65536 ||
        capacity > active_->maximum_requests_ ||
        capacity > active_->limits_.maximum_boxes)
      return Answer(
          Status{ErrorCode::ResourceExhausted, "GPU discovery table limit"});
    const auto& charge = work;
    const auto bytes = 16 + std::uint64_t{capacity} * 144;
    auto status = charge(candidates);
    if (status.ok())
      status = charge(2 * bytes);
    if (!status.ok())
      return Answer(status);
    auto allocated = active_->allocator_.allocate(bytes);
    if (!allocated.ok())
      return Answer(allocated.status());
    auto buffer = allocated.take_value();
    if (buffer.size() != bytes)
      return Answer(Status{ErrorCode::InvalidArgument,
                           "GPU discovery allocator span mismatch"});
    status = charge(0);
    if (!status.ok())
      return Answer(status);
    std::memset(buffer.data(), 0, bytes);
    struct Scope {
      bool& active;
      ~Scope() { active = false; }
    } scope{active_->discovering_};
    active_->discovering_ = true;
    try {
      status = compute({buffer.data(), bytes, capacity});
    } catch (...) {
      static_cast<void>(std::move(buffer).freeze());
      throw;
    }
    auto frozen = std::move(buffer).freeze();
    if (status.ok())
      status = charge(0);
    if (!status.ok())
      return Answer(status);
    auto limits = active_->limits_;
    limits.cancellation = query.cancellation;
    limits.consume_work = charge;
    return plugin_internal::decode_result_discovery(
        *frozen, capacity, candidates, query, active_->resources_,
        std::move(limits), &active_->discovery_metadata_);
  }
  static Result<std::shared_ptr<const FragmentAtlas>> atlas(
      std::uint32_t port, std::uint32_t slot, const ResultTensorInput& input,
      const std::function<Status(std::uint64_t)>& work,
      const CancellationToken& cancellation) {
    using Answer = Result<std::shared_ptr<const FragmentAtlas>>;
    if (!active_ || !active_->upload_)
      return Answer(Status{ErrorCode::BackendUnavailable, {}});
    auto charged = work(1);
    if (!charged.ok())
      return Answer(charged);
    const AtlasKey key{port, slot};
    auto found = active_->atlases_.find(key);
    if (found != active_->atlases_.end())
      return Answer(found->second);
    auto limits = active_->limits_;
    limits.cancellation = cancellation;
    limits.consume_work = work;
    auto plan = FragmentAtlasPlan::prepare(input, {}, limits);
    if (!plan.ok())
      return Answer(plan.status());
    struct Owner {
      ResourceLease metadata;
      FragmentAtlas value;
    };
    const auto bytes =
        input.coverage().shape().size() * 2 * sizeof(std::uint64_t);
    auto metadata =
        active_->resources_.reserve(ResourceCapacity::host(bytes, bytes));
    if (!metadata.ok())
      return Answer(metadata.status());
    auto packed = plan.value().materialize(input, active_->allocator_, limits);
    if (!packed.ok())
      return Answer(packed.status());
    auto owner = std::allocate_shared<Owner>(
        ResourceAllocator<Owner>(active_->resources_));
    owner->metadata = metadata.take_value();
    owner->value = packed.take_value();
    std::shared_ptr<const FragmentAtlas> result(owner, &owner->value);
    active_->atlases_.emplace(key, result);
    active_->transfers += 2;
    active_->transferred_bytes += plan.value().payload_allocation_bytes() +
                                  plan.value().directory_allocation_bytes();
    return Answer(std::move(result));
  }
  std::uint64_t transfers = 0, transferred_bytes = 0, host_accesses = 0;

 private:
  const BufferAllocator& allocator_;
  const Upload& upload_;
  const ResourceBudget& resources_;
  FootprintLimits limits_;
  std::uint32_t maximum_requests_;
  std::function<bool(const CpuStorage&)> native_storage_;
  bool host_access_ = false;
  CacheWork cache_work_;
  ResourceVector<std::weak_ptr<const CpuStorage>> host_owners_;
  std::mutex host_mutex_;
  data_internal::ResultHostAccessScope host_scope_;
  std::uint64_t discovery_metadata_ = 1;
  bool discovering_ = false;
  using AtlasKey = std::pair<std::uint32_t, std::uint32_t>;
  using AtlasEntry =
      std::pair<const AtlasKey, std::shared_ptr<const FragmentAtlas>>;
  std::map<AtlasKey, std::shared_ptr<const FragmentAtlas>, std::less<AtlasKey>,
           ResourceAllocator<AtlasEntry>>
      atlases_;
  ResultNativeScope* previous_;
  inline static thread_local ResultNativeScope* active_ = nullptr;
};
}  // namespace ps::execution_internal
