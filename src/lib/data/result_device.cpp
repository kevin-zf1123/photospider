#include <algorithm>
#include <chrono>
#include <functional>
#include <memory>
#include <new>
#include <utility>

#include "data/resource_storage.hpp"
#include "data/result_device_access.hpp"
#include "data/result_state.hpp"
#include "data/result_support.hpp"
#include "photospider/core/checked_math.hpp"

namespace ps::data_internal {
Result<std::shared_ptr<const gpu_internal::DeviceStorage>>
ResultDeviceAccess::reference(
    const ResourceBudget& budget,
    std::shared_ptr<const gpu_internal::DeviceStorage> storage) {
  using Answer = Result<std::shared_ptr<const gpu_internal::DeviceStorage>>;
  if (!storage)
    return Answer({ErrorCode::InvalidArgument, "missing Device storage"});
  const auto* identity = storage.get();
  auto retained = ResourceStorageAccess::reference(
      budget, storage, storage->capacity(), storage->owned_by(budget));
  if (!retained.ok())
    return Answer(retained.status());
  return Answer(std::shared_ptr<const gpu_internal::DeviceStorage>(
      retained.take_value(), identity));
}
Status ResultDeviceAccess::publish(
    ResultBuilder& builder, std::uint32_t slot, const Region& region,
    const StridedLayout& layout,
    std::shared_ptr<const gpu_internal::DeviceStorage> storage,
    const CancellationToken& cancellation) try {
  auto owner = builder.impl_;
  if (!owner)
    return {ErrorCode::Stale, {}};
  ResourceAllocationScope scope(owner->budget);
  std::unique_lock<std::timed_mutex> lock(owner->mutex, std::defer_lock);
  while (!lock.try_lock_for(std::chrono::milliseconds(2)))
    if (cancellation.cancelled()) {
      owner->cancelled_publication.store(true);
      return {ErrorCode::Cancelled, {}};
    }
  const auto reject = [&](Status status) {
    owner->failure.record(status);
    return status;
  };
  if (auto busy = owner->reject_active_mutation(); !busy.ok())
    return busy;
  if (owner->complete || !owner->status().ok())
    return owner->status().ok() ? Status{ErrorCode::Stale, {}}
                                : owner->status();
  if (cancellation.cancelled())
    return reject({ErrorCode::Cancelled, {}});
  if (slot >= owner->schema.tensors.size() || owner->revision == UINT64_MAX ||
      !storage)
    return reject(invalid_schema());
  const auto& spec = owner->schema.tensors[slot];
  const SampleDescriptor sample(spec);
  auto& target = owner->tensors[slot];
  FootprintLimits limits;
  limits.cancellation = cancellation;
  limits.consume_work = [root = owner->budget](auto n) {
    return root.consume({n});
  };
  auto samples = Footprint::from_region(sample.value.shape, region, limits);
  if (!samples.ok())
    return reject(samples.status());
  auto closed = spec.close_samples(samples.value(), limits);
  if (!closed.ok())
    return reject(closed.status());
  if (closed.value() != samples.value())
    return reject(invalid_schema());
  auto overlap = samples.value().intersect(target.coverage, limits);
  if (!overlap.ok())
    return reject(overlap.status());
  if (!overlap.value().empty())
    return reject(invalid_schema());
  auto next = target.coverage.unite(samples.value(), limits);
  if (!next.ok())
    return reject(next.status());
  auto retained = reference(owner->budget, std::move(storage));
  if (!retained.ok())
    return reject(retained.status());
  auto span = DeviceTensorSpan::from_storage(sample.value, region, layout,
                                             retained.take_value());
  if (!span.ok())
    return reject(span.status());
  std::uint64_t growth_bytes =
      sizeof(DeviceTensorSpan) +
      region.rank() * (sizeof(RegionDimension) + 3 * sizeof(std::uint64_t));
  ResultRef::Impl::StorageOwners new_owners(
      std::less<const void*>{}, ResourceAllocator<const void*>(owner->budget));
  if (!owner->storage_owners.count(span.value().storage().get())) {
    new_owners.insert(span.value().storage().get());
    if (!core_internal::checked_add(
            growth_bytes, span.value().storage()->capacity(), &growth_bytes))
      return reject({ErrorCode::ResourceExhausted, {}});
  }
  auto growth = owner->image_budget->charge(growth_bytes);
  if (!growth.ok())
    return reject(growth);
  std::shared_ptr<void> growth_owner;
  try {
    growth_owner = std::shared_ptr<void>(
        nullptr, [budget = owner->image_budget, growth_bytes](void*) {
          budget->release(growth_bytes);
        });
  } catch (...) {
    owner->image_budget->release(growth_bytes);
    throw;
  }
  const auto reserve_one = [](auto* values) {
    if (values->size() == values->max_size())
      throw std::bad_alloc();
    if (values->size() == values->capacity()) {
      const auto growth =
          std::min(values->max_size() - values->capacity(),
                   std::max<std::size_t>(values->capacity(), 4));
      values->reserve(values->capacity() + growth);
    }
  };
  reserve_one(&target.device);
  reserve_one(&target.affine_growth);
  if (cancellation.cancelled())
    return reject({ErrorCode::Cancelled, {}});
  owner->storage_owners.merge(new_owners);
  target.device.push_back(span.take_value());
  target.affine_growth.push_back(std::move(growth_owner));
  target.coverage = next.take_value();
  target.affine_canonical = false;
  ++owner->revision;
  return Status::success();
} catch (const std::bad_alloc&) {
  Status status{ErrorCode::ResourceExhausted, {}};
  builder.fail(status);
  return status;
} catch (...) {
  Status status{ErrorCode::OperationFailed, {}};
  builder.fail(status);
  return status;
}
Result<DeviceTensorSpan> ResultDeviceAccess::acquire(
    const ResultRef& result, const ResultDescriptor& facts, std::uint32_t slot,
    const Region& region, const CancellationToken& cancellation) try {
  using Answer = Result<DeviceTensorSpan>;
  const auto& owner = result.impl_;
  if (!owner || (result.captured_ &&
                 facts.revision() > result.captured_->descriptor.revision()))
    return Answer({ErrorCode::Stale, {}});
  ResourceAllocationScope scope(owner->budget);
  std::unique_lock<std::timed_mutex> lock(owner->mutex, std::defer_lock);
  while (!lock.try_lock_for(std::chrono::milliseconds(2)))
    if (cancellation.cancelled())
      return Answer({ErrorCode::Cancelled, {}});
  if (cancellation.cancelled())
    return Answer({ErrorCode::Cancelled, {}});
  if (facts.object_id() != owner->object || !facts.revision() ||
      facts.revision() > owner->revision || slot >= facts.tensor_count() ||
      slot >= owner->schema.tensors.size())
    return Answer({ErrorCode::Stale, "invalid Device descriptor"});
  const auto& spec = owner->schema.tensors[slot];
  const SampleDescriptor sample(spec);
  if (region.empty() || !region.validate(sample.value.shape).ok())
    return Answer({ErrorCode::InvalidArgument, "invalid Device rectangle"});
  FootprintLimits limits;
  limits.cancellation = cancellation;
  limits.consume_work = [root = owner->budget](auto n) {
    return root.consume({n});
  };
  auto requested = Footprint::from_region(sample.value.shape, region, limits);
  if (!requested.ok())
    return Answer(requested.status());
  auto missing =
      requested.value().subtract(facts.tensor_coverage(slot), limits);
  if (!missing.ok())
    return Answer(missing.status());
  if (!missing.value().empty())
    return Answer(unavailable());
  for (const auto& span : owner->tensors[slot].device) {
    auto charged = owner->budget.consume({region.rank() + 1});
    if (!charged.ok())
      return Answer(charged);
    if (cancellation.cancelled())
      return Answer({ErrorCode::Cancelled, {}});
    if (!regions_overlap(region, span.region()))
      continue;
    auto view = span.view(region);
    if (view.ok())
      return view;
    if (view.status().code != ErrorCode::TypeMismatch)
      return view;
  }
  return Answer({ErrorCode::TypeMismatch,
                 "Device input requires one retained allocation"});
} catch (const Status& status) {
  return Result<DeviceTensorSpan>(status);
} catch (const std::bad_alloc&) {
  return Result<DeviceTensorSpan>({ErrorCode::ResourceExhausted, {}});
}
}  // namespace ps::data_internal
