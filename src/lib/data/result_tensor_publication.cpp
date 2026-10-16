#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <functional>
#include <memory>
#include <mutex>
#include <new>
#include <numeric>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#include "core/callback_environment.hpp"
#include "data/affine_view.hpp"
#include "data/result_host_access.hpp"
#include "data/result_state.hpp"
#include "data/result_support.hpp"
#include "data/result_window_access.hpp"
#include "data/tensor_access.hpp"
#include "data/tensor_validation.hpp"
#include "photospider/core/checked_math.hpp"
#include "photospider/data/result.hpp"

namespace ps {
Result<std::uint64_t> ResultViewAxis::source_coordinate(
    std::uint64_t output) const {
  if (output_axis < 0 || !step)
    return Result<std::uint64_t>(source_origin);
  const auto distance =
      output >= output_origin ? output - output_origin : output_origin - output;
  const auto magnitude = step < 0 ? static_cast<std::uint64_t>(-(step + 1)) + 1
                                  : static_cast<std::uint64_t>(step);
  const bool subtract = (output < output_origin) != (step < 0);
  const auto available = subtract ? source_origin : UINT64_MAX - source_origin;
  if (!core_internal::can_multiply(distance, magnitude, available))
    return Result<std::uint64_t>(
        Status{ErrorCode::InvalidArgument, "tensor view coordinate overflow"});
  const auto delta = distance * magnitude;
  return Result<std::uint64_t>(subtract ? source_origin - delta
                                        : source_origin + delta);
}
namespace {
std::timed_mutex result_ownership_mutex;
struct ProducerWriteGuard final {
  std::atomic<bool>* active = nullptr;
  void release() noexcept {
    if (active) {
      active->store(false);
      active = nullptr;
    }
  }
  ~ProducerWriteGuard() { release(); }
};

}  // namespace
namespace {

template <class T>
void reserve_append(ResourceVector<T>* values, std::size_t count) {
  if (count > values->max_size() - values->size())
    throw std::bad_alloc();
  const auto required = values->size() + count;
  if (required <= values->capacity())
    return;
  const auto growth = std::min(values->max_size() - values->capacity(),
                               std::max<std::size_t>(values->capacity(), 4));
  values->reserve(std::max(required, values->capacity() + growth));
}

bool singleton_batches(const ResultTensorSpec& spec, const Region& region) {
  for (std::size_t axis = 0; axis < spec.batch_axes.size(); ++axis)
    if (region.dimensions()[axis].extent != 1)
      return false;
  return true;
}

}  // namespace
Status ResultBuilder::publish_tensor(std::uint32_t slot, const Region& region,
                                     StridedLayout layout,
                                     std::shared_ptr<const CpuStorage> storage,
                                     const CancellationToken& cancellation) {
  return publish_tensor_storage(slot, region, std::move(layout),
                                std::move(storage), cancellation, {});
}
Status ResultBuilder::publish_tensor_storage(
    std::uint32_t slot, const Region& region, StridedLayout layout,
    std::shared_ptr<const CpuStorage> storage,
    const CancellationToken& cancellation,
    ResourceVector<ResultRef> sources) try {
  if (!impl_)
    return {ErrorCode::Stale, {}};
  if (slot >= impl_->schema.tensors.size()) {
    auto status = data_internal::invalid_schema();
    fail(status);
    return status;
  }
  ResourceAllocationScope scope(impl_->budget);
  const auto& spec = impl_->schema.tensors[slot];
  auto span = TensorSpan::from_storage(
      {spec.descriptor.element_type, spec.sample_shape()}, region,
      std::move(layout), std::move(storage));
  if (!span.ok()) {
    fail(span.status());
    return span.status();
  }
  ResourceVector<TensorSpan> spans{
      ResourceAllocator<TensorSpan>(impl_->budget)};
  spans.push_back(span.take_value());
  return publish_tensor_spans(slot, region, spans, cancellation,
                              std::move(sources));
} catch (const std::bad_alloc&) {
  Status status{ErrorCode::ResourceExhausted, {}};
  fail(status);
  return status;
} catch (...) {
  Status status{ErrorCode::OperationFailed, {}};
  fail(status);
  return status;
}
Status ResultBuilder::publish_tensor_spans(
    std::uint32_t slot, const Region& region,
    const ResourceVector<TensorSpan>& spans,
    const CancellationToken& cancellation,
    ResourceVector<ResultRef> sources) try {
  if (!impl_)
    return {ErrorCode::Stale, {}};
  ResourceAllocationScope scope(impl_->budget);
  std::unique_lock<std::timed_mutex> lock(impl_->mutex, std::defer_lock);
  while (!lock.try_lock_for(std::chrono::milliseconds(2)))
    if (cancellation.cancelled()) {
      impl_->cancelled_publication.store(true);
      return {ErrorCode::Cancelled, {}};
    }
  const auto reject = [&](Status status) {
    impl_->failure.record(status);
    return status;
  };
  if (auto busy = impl_->reject_active_mutation(); !busy.ok())
    return busy;
  if (impl_->complete || !impl_->status().ok())
    return impl_->status().ok() ? Status{ErrorCode::Stale, {}}
                                : impl_->status();
  if (cancellation.cancelled())
    return reject({ErrorCode::Cancelled, {}});
  if (slot >= impl_->schema.tensors.size() || impl_->revision == UINT64_MAX)
    return reject(data_internal::invalid_schema());
  const auto& spec = impl_->schema.tensors[slot];
  auto& target = impl_->tensors[slot];
  FootprintLimits limits;
  limits.cancellation = cancellation;
  limits.consume_work = [root = impl_->budget](auto n) {
    return root.consume({n});
  };
  auto samples = Footprint::from_regions(spec.sample_shape(), {region}, limits);
  if (!samples.ok())
    return reject(samples.status());
  auto closed = spec.close_samples(samples.value(), limits);
  if (!closed.ok())
    return reject(closed.status());
  if (closed.value() != samples.value())
    return reject(data_internal::invalid_schema());
  auto overlap = samples.value().intersect(target.tensor.coverage(), limits);
  if (!overlap.ok())
    return reject(overlap.status());
  if (!overlap.value().empty())
    return reject(data_internal::invalid_schema());
  auto next = target.tensor.coverage().unite(samples.value(), limits);
  if (!next.ok())
    return reject(next.status());
  auto supplied =
      Tensor::create_view({spec.descriptor.element_type, spec.sample_shape()},
                          samples.value(), spans.data(), spans.size(), limits);
  if (!supplied.ok())
    return reject(supplied.status());
  const auto metadata_unit =
      sizeof(TensorSpan) +
      region.rank() * (sizeof(RegionDimension) + 3 * sizeof(std::uint64_t));
  std::uint64_t metadata_bytes = 0;
  if (!core_internal::checked_multiply(supplied.value().fragments().size(),
                                       metadata_unit, &metadata_bytes))
    return reject({ErrorCode::ResourceExhausted, {}});
  ResourceVector<TensorSpan> adopted{
      ResourceAllocator<TensorSpan>(impl_->budget)};
  std::uint64_t growth_bytes = metadata_bytes;
  ResultRef::Impl::StorageOwners new_owners(
      std::less<const CpuStorage*>{},
      ResourceAllocator<const CpuStorage*>(impl_->budget));
  adopted.reserve(supplied.value().fragments().size());
  for (const auto& span : supplied.value().fragments()) {
    auto storage =
        sources.empty()
            ? impl_->budget.reference(span.storage())
            : Result<std::shared_ptr<const CpuStorage>>(span.storage());
    if (!storage.ok())
      return reject(storage.status());
    auto work = impl_->budget.consume({1});
    if (!work.ok())
      return reject(work);
    const bool added = !impl_->storage_owners.count(storage.value().get()) &&
                       new_owners.insert(storage.value().get()).second;
    if (sources.empty() && added) {
      if (!core_internal::can_add(growth_bytes, storage.value()->capacity()))
        return reject({ErrorCode::ResourceExhausted, {}});
      growth_bytes += storage.value()->capacity();
    }
    auto value = TensorSpan::from_storage(span.descriptor(), span.region(),
                                          span.layout(), storage.take_value());
    if (!value.ok())
      return reject(value.status());
    adopted.push_back(value.take_value());
  }
  auto growth = impl_->image_budget->charge(growth_bytes, false, true);
  if (!growth.ok())
    return reject(growth.status());
  std::shared_ptr<void> growth_owner;
  try {
    growth_owner = std::shared_ptr<void>(
        nullptr, [budget = impl_->image_budget, growth_bytes](void*) {
          budget->release(growth_bytes);
        });
  } catch (...) {
    impl_->image_budget->release(growth_bytes);
    throw;
  }
  auto& destination = data_internal::TensorAccess::edit(target.tensor);
  reserve_append(&destination, adopted.size());
  reserve_append(&target.affine_owners, sources.size());
  reserve_append(&target.affine_growth, 1);
  if (cancellation.cancelled())
    return reject({ErrorCode::Cancelled, {}});
  impl_->storage_owners.merge(new_owners);
  for (auto& value : adopted)
    destination.push_back(std::move(value));
  for (auto& owner : sources)
    target.affine_owners.push_back(std::move(owner));
  target.affine_growth.push_back(std::move(growth_owner));
  target.affine_canonical = false;
  data_internal::TensorAccess::commit_coverage(target.tensor,
                                               next.take_value());
  ++impl_->revision;
  return Status::success();
} catch (const std::bad_alloc&) {
  Status status{ErrorCode::ResourceExhausted, {}};
  fail(status);
  return status;
} catch (...) {
  Status status{ErrorCode::OperationFailed, {}};
  fail(status);
  return status;
}
Status ResultBuilder::publish_tensor(
    std::uint32_t slot, const Region& region, ByteView packed,
    const CancellationToken& cancellation) try {
  if (!impl_)
    return Status{ErrorCode::Stale, {}};
  if (slot >= impl_->schema.tensors.size()) {
    auto status = data_internal::invalid_schema();
    fail(status);
    return status;
  }
  const auto width = TensorSpan::element_size(
      impl_->schema.tensors[slot].descriptor.element_type);
  auto count = region.element_count();
  if (!count.ok() || !core_internal::can_multiply(count.value(), width) ||
      count.value() * width != packed.size() ||
      (packed.size() && !packed.data())) {
    auto status = data_internal::invalid_schema();
    fail(status);
    return status;
  }
  return publish_tensor_kernel(
      slot, region,
      [&](const auto& writers) {
        for (const auto& writer : writers) {
          const auto& dimensions = writer.region().dimensions();
          std::vector<std::uint64_t> at;
          for (const auto d : dimensions)
            at.push_back(d.offset);
          for (;;) {
            if (cancellation.cancelled())
              return Status{ErrorCode::Cancelled, {}};
            auto run = writer.row_run(at);
            if (!run.ok())
              return run.status();
            // Packed publication is descriptor ordered. A last-axis physical
            // width run can be copied in one operation; channel-last input is
            // scattered.
            const auto last = at.size() - 1;
            const auto n = writer.sample_axis() == last &&
                                   run.value().sample_stride_bytes ==
                                       static_cast<std::int64_t>(width)
                               ? run.value().samples
                               : 1;
            std::uint64_t index = 0;
            for (std::size_t axis = 0; axis < at.size(); ++axis)
              index = index * region.dimensions()[axis].extent + at[axis] -
                      region.dimensions()[axis].offset;
            std::memcpy(run.value().data, packed.data() + index * width,
                        n * width);
            at[last] += n - 1;
            std::size_t axis = at.size();
            while (axis) {
              --axis;
              if (++at[axis] <
                  dimensions[axis].offset + dimensions[axis].extent)
                break;
              at[axis] = dimensions[axis].offset;
            }
            if (!axis && at[0] == dimensions[0].offset)
              break;
          }
        }
        return Status::success();
      },
      cancellation);
} catch (const std::bad_alloc&) {
  auto status = Status{ErrorCode::ResourceExhausted, {}};
  fail(status);
  return status;
} catch (...) {
  auto status = Status{ErrorCode::OperationFailed, {}};
  fail(status);
  return status;
}
Status ResultBuilder::publish_tensor_kernel(
    std::uint32_t slot, const Region& region,
    const std::function<Status(const ResourceVector<ResultTensorWriteWindow>&)>&
        write,
    const CancellationToken& cancellation) try {
  if (!impl_)
    return {ErrorCode::Stale, {}};
  if (slot >= impl_->schema.tensors.size()) {
    auto status = data_internal::invalid_schema();
    fail(status);
    return status;
  }
  FootprintLimits limits;
  limits.cancellation = cancellation;
  limits.consume_work = [root = impl_->budget](auto n) {
    return root.consume({n});
  };
  auto samples = Footprint::from_regions(
      impl_->schema.tensors[slot].sample_shape(), {region}, limits);
  if (!samples.ok()) {
    fail(samples.status());
    return samples.status();
  }
  return publish_tensor_kernel(slot, samples.value(), write, cancellation);
} catch (const std::bad_alloc&) {
  Status status{ErrorCode::ResourceExhausted, {}};
  fail(status);
  return status;
} catch (...) {
  Status status{ErrorCode::OperationFailed, {}};
  fail(status);
  return status;
}
Status ResultBuilder::publish_tensor_kernel(
    std::uint32_t slot, const Footprint& requested,
    const std::function<Status(const ResourceVector<ResultTensorWriteWindow>&)>&
        write,
    const CancellationToken& cancellation) {
  const Footprint samples = requested;
  const auto owner = impl_;
  if (!owner)
    return {ErrorCode::Stale, {}};
  try {
    ResourceAllocationScope scope(owner->budget);
    std::unique_lock<std::timed_mutex> lock(owner->mutex, std::defer_lock);
    while (!lock.try_lock_for(std::chrono::milliseconds(2)))
      if (cancellation.cancelled()) {
        owner->cancelled_publication = true;
        return {ErrorCode::Cancelled, {}};
      }
    const auto reject = [&](Status status) {
      owner->failure.record(status);
      return status;
    };
    if (cancellation.cancelled())
      return reject({ErrorCode::Cancelled, {}});
    if (owner->complete || !owner->status().ok())
      return owner->status().ok() ? Status{ErrorCode::Stale, {}}
                                  : owner->status();
    if (auto busy = owner->reject_active_mutation(); !busy.ok())
      return busy;
    const auto revision = owner->revision;
    if (!samples.valid() || slot >= owner->schema.tensors.size() || !write ||
        revision == UINT64_MAX)
      return reject(data_internal::invalid_schema());
    const auto& spec = owner->schema.tensors[slot];
    auto& target = owner->tensors[slot];
    if (samples.shape() != spec.sample_shape())
      return reject(data_internal::invalid_schema());
    owner->kernel_active.store(true);
    ProducerWriteGuard producer_guard{&owner->kernel_active};
    FootprintLimits limits;
    limits.cancellation = cancellation;
    limits.consume_work = [root = owner->budget](auto n) {
      return root.consume({n});
    };
    auto closed = spec.close_samples(samples, limits);
    if (!closed.ok())
      return reject(closed.status());
    if (closed.value() != samples)
      return reject(data_internal::invalid_schema());
    auto overlap = samples.intersect(target.tensor.coverage(), limits);
    if (!overlap.ok())
      return reject(overlap.status());
    if (!overlap.value().empty())
      return reject(data_internal::invalid_schema());
    auto coverage = target.tensor.coverage().unite(samples, limits);
    if (!coverage.ok())
      return reject(coverage.status());
    if (samples.empty()) {
      ++owner->revision;
      return Status::success();
    }
    auto count = samples.element_count();
    const auto width = TensorSpan::element_size(spec.descriptor.element_type);
    if (!count.ok() || !width ||
        !core_internal::can_multiply(count.value(), width, INT64_MAX))
      return reject({ErrorCode::ResourceExhausted, "batch tensor byte count"});
    const auto bytes = count.value() * width;
    if (bytes > owner->limits.maximum_bytes)
      return reject(
          {ErrorCode::ResourceExhausted, "tensor storage growth limit"});
    auto copied = owner->budget.consume({count.value()});
    if (!copied.ok())
      return reject(copied);
    const auto rank = samples.shape().size();
    ResourceVector<Region> chunks{ResourceAllocator<Region>(owner->budget)};
    const auto batch_rank = spec.batch_axes.size();
    const auto free_rank =
        spec.descriptor.shape.size() - spec.atomic_trailing_axes;
    const auto x = spec.layout.spatial ? batch_rank + spec.layout.width_axis
                   : free_rank         ? batch_rank + free_rank - 1
                                       : rank;
    const auto y = spec.layout.spatial ? batch_rank + spec.layout.height_axis
                   : free_rank > 1     ? batch_rank + free_rank - 2
                                       : rank;
    std::uint64_t boxes = 0;
    for (const auto& box : samples.boxes()) {
      auto charged = owner->budget.consume({rank + 1});
      if (!charged.ok())
        return reject(charged);
      std::uint64_t parts = 1;
      for (std::size_t axis = 0; axis < rank; ++axis) {
        const auto d = box.dimensions()[axis];
        const bool batch =
            axis <
                (spec.layout.spatial ? batch_rank : batch_rank + free_rank) &&
            axis != x && axis != y;
        const auto n = batch ? d.extent
                       : (axis == x || axis == y)
                           ? (d.offset + d.extent - 1) / target.tile_extent -
                                 d.offset / target.tile_extent + 1
                           : 1;
        if (!core_internal::checked_multiply(parts, n, &parts))
          return reject({ErrorCode::ResourceExhausted, "tensor tile count"});
      }
      if (!core_internal::checked_add(boxes, parts, &boxes))
        return reject({ErrorCode::ResourceExhausted, "tensor tile count"});
    }
    const auto metadata_unit =
        sizeof(TensorSpan) +
        rank * (sizeof(RegionDimension) + 3 * sizeof(std::uint64_t));
    if (!core_internal::can_multiply(boxes, metadata_unit) ||
        !core_internal::can_add(
            boxes, data_internal::TensorAccess::edit(target.tensor).size(),
            data_internal::TensorAccess::edit(target.tensor).max_size()))
      return reject(
          {ErrorCode::ResourceExhausted, "batch tensor metadata count"});
    const auto metadata_bytes = boxes * metadata_unit;
    if (!core_internal::can_add(metadata_bytes, bytes))
      return reject(
          {ErrorCode::ResourceExhausted, "batch tensor growth count"});
    const auto scratch_unit =
        rank * (2 * sizeof(RegionDimension) + 4 * sizeof(std::uint64_t));
    std::uint64_t scratch_bytes = 0;
    if (!core_internal::checked_multiply(boxes + 1, scratch_unit,
                                         &scratch_bytes))
      return reject({ErrorCode::ResourceExhausted, "tensor scratch metadata"});
    auto scratch = owner->budget.reserve(
        ResourceCapacity::host(scratch_bytes, scratch_bytes));
    if (!scratch.ok())
      return reject(scratch.status());
    chunks.reserve(boxes);
    for (const auto& box : samples.boxes()) {
      auto dims = box.dimensions();
      for (;;) {
        auto charged = owner->budget.consume({rank + 1});
        if (!charged.ok())
          return reject(charged);
        if (cancellation.cancelled())
          return reject({ErrorCode::Cancelled, {}});
        for (std::size_t axis = 0; axis < rank; ++axis) {
          const auto end =
              box.dimensions()[axis].offset + box.dimensions()[axis].extent;
          const bool batch =
              axis <
                  (spec.layout.spatial ? batch_rank : batch_rank + free_rank) &&
              axis != x && axis != y;
          dims[axis].extent =
              batch ? 1
              : (axis == x || axis == y)
                  ? std::min(end - dims[axis].offset,
                             target.tile_extent -
                                 dims[axis].offset % target.tile_extent)
                  : end - dims[axis].offset;
        }
        chunks.emplace_back(dims);
        bool advanced = false;
        for (std::size_t axis = rank; axis-- > 0;) {
          dims[axis].offset += dims[axis].extent;
          if (dims[axis].offset <
              box.dimensions()[axis].offset + box.dimensions()[axis].extent) {
            advanced = true;
            break;
          }
          dims[axis].offset = box.dimensions()[axis].offset;
        }
        if (!advanced)
          break;
      }
    }
    auto allocated = owner->budget.allocator().allocate(bytes);
    if (!allocated.ok())
      return reject(allocated.status());
    auto buffer = allocated.take_value();
    std::uint64_t growth_bytes = 0;
    if (!core_internal::checked_add(buffer.size(), metadata_bytes,
                                    &growth_bytes))
      return reject(
          {ErrorCode::ResourceExhausted, "batch tensor growth count"});
    auto growth = owner->image_budget->charge(growth_bytes, false, true);
    if (!growth.ok())
      return reject(growth.status());
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
    ResourceVector<StridedLayout> layouts{
        ResourceAllocator<StridedLayout>(owner->budget)};
    ResourceVector<ResultTensorWriteWindow> windows{
        ResourceAllocator<ResultTensorWriteWindow>(owner->budget)};
    ResourceVector<TensorSpan> values{
        ResourceAllocator<TensorSpan>(owner->budget)};
    layouts.reserve(boxes);
    windows.reserve(boxes);
    values.reserve(boxes);
    auto& destination = data_internal::TensorAccess::edit(target.tensor);
    reserve_append(&destination, boxes);
    reserve_append(&target.affine_growth, 1);
    std::uint64_t offset = 0;
    for (const auto& region : chunks) {
      if (cancellation.cancelled())
        return reject({ErrorCode::Cancelled, {}});
      ResultTensorWriteWindow window;
      window.spec_ = &spec;
      window.region_ = region;
      window.affine_data_ = buffer.data() + offset;
      window.affine_strides_.resize(rank);
      StridedLayout layout;
      layout.byte_offset = offset;
      layout.byte_strides.resize(rank);
      layout.origin.resize(rank);
      std::uint64_t stride = width;
      std::vector<std::size_t> order;
      if (x < rank)
        order.push_back(x);
      if (y < rank)
        order.push_back(y);
      for (std::size_t axis = rank; axis-- > 0;)
        if (axis != x && axis != y)
          order.push_back(axis);
      for (auto axis : order) {
        window.affine_strides_[axis] = stride;
        layout.byte_strides[axis] = stride;
        layout.origin[axis] = region.dimensions()[axis].offset;
        stride *= region.dimensions()[axis].extent;
      }
      offset += stride;
      layouts.push_back(std::move(layout));
      windows.push_back(std::move(window));
    }
    lock.unlock();
    Status written;
    try {
      written = core_internal::invoke_callback([&] { return write(windows); });
    } catch (const std::bad_alloc&) {
      written = {ErrorCode::ResourceExhausted, {}};
    } catch (...) {
      written = {ErrorCode::OperationFailed, {}};
    }
    lock.lock();
    if (!owner->status().ok())
      return owner->status();
    if (impl_ != owner || owner->complete || owner->revision != revision)
      return reject(
          {ErrorCode::Stale, "tensor producer changed during callback"});
    if (!written.ok())
      return reject(written);
    if (cancellation.cancelled())
      return reject({ErrorCode::Cancelled, {}});
    windows.clear();
    auto storage = std::move(buffer).freeze();
    for (std::size_t i = 0; i < boxes; ++i) {
      auto value = TensorSpan::from_storage(
          {spec.descriptor.element_type, samples.shape()}, chunks[i],
          std::move(layouts[i]), storage);
      if (!value.ok())
        return reject(value.status());
      values.push_back(value.take_value());
    }
    ResultRef::Impl::StorageOwners new_owners(
        std::less<const CpuStorage*>{},
        ResourceAllocator<const CpuStorage*>(owner->budget));
    new_owners.insert(storage.get());
    if (cancellation.cancelled())
      return reject({ErrorCode::Cancelled, {}});
    const bool canonical = false;
    // All allocations and fallible checks precede this single commit barrier.
    owner->storage_owners.merge(new_owners);
    for (auto& value : values)
      data_internal::TensorAccess::edit(target.tensor)
          .push_back(std::move(value));
    target.affine_growth.push_back(std::move(growth_owner));
    target.affine_canonical = canonical;
    data_internal::TensorAccess::commit_coverage(target.tensor,
                                                 coverage.take_value());
    ++owner->revision;
    return Status::success();
  } catch (const std::bad_alloc&) {
    auto status = Status{ErrorCode::ResourceExhausted, {}};
    std::lock_guard<std::timed_mutex> lock(owner->mutex);
    if (!owner->complete && owner->status().ok())
      owner->failure.record(status);
    return owner->status().ok() ? status : owner->status();
  } catch (...) {
    auto status = Status{ErrorCode::OperationFailed, {}};
    std::lock_guard<std::timed_mutex> lock(owner->mutex);
    if (!owner->complete && owner->status().ok())
      owner->failure.record(status);
    return owner->status().ok() ? status : owner->status();
  }
}
Status ResultBuilder::publish_tensor_view(
    std::uint32_t slot, const Region& region,
    const ResultTensorReadWindow& source,
    const ResultTensorViewTransform& transform,
    const CancellationToken& cancellation) try {
  if (!impl_)
    return {ErrorCode::Stale, {}};
  ResourceAllocationScope scope(impl_->budget);
  Footprint prior;
  {
    std::unique_lock<std::timed_mutex> lock(impl_->mutex, std::defer_lock);
    while (!lock.try_lock_for(std::chrono::milliseconds(2)))
      if (cancellation.cancelled()) {
        impl_->cancelled_publication = true;
        return {ErrorCode::Cancelled, {}};
      }
    if (!impl_->status().ok())
      return impl_->status();
    if (impl_->complete)
      return {ErrorCode::Stale, {}};
    if (auto busy = impl_->reject_active_mutation(); !busy.ok())
      return busy;
    if (slot >= impl_->schema.tensors.size()) {
      impl_->failure.record(data_internal::invalid_schema());
      return impl_->status();
    }
    prior = impl_->tensors[slot].tensor.coverage();
  }
  const auto unavailable = [] {
    return Status{ErrorCode::InvalidArgument,
                  "ViewUnavailable: source has no affine physical mapping",
                  FailureReason::ViewUnavailable};
  };
  const auto reject = [&](Status status) {
    fail(status);
    return status;
  };
  if (!source.valid() || slot >= impl_->schema.tensors.size() ||
      region.empty() ||
      !region.validate(impl_->schema.tensors[slot].sample_shape()).ok())
    return reject(data_internal::invalid_schema());
  if (cancellation.cancelled())
    return reject({ErrorCode::Cancelled, {}});

  const auto& spec = impl_->schema.tensors[slot];
  if (spec.descriptor.element_type != source.spec().descriptor.element_type)
    return reject(data_internal::invalid_schema());
  // Logical validity and exact source authorization precede physical strategy
  // selection. Invalid transforms cannot be laundered as Auto copy retries.
  if (transform.reshape) {
    if (!transform.source_axes.empty())
      return reject({ErrorCode::InvalidArgument,
                     "invalid tensor view transform",
                     FailureReason::None,
                     {FailureOrigin::Protocol, FailureScope::Group}});
    std::array<std::uint64_t, 8> remaining{};
    for (std::size_t axis = 0; axis < source.region_.rank(); ++axis)
      remaining[axis] = source.region_.dimensions()[axis].extent;
    const auto cancel_factor = [&](std::uint64_t extent) {
      for (std::size_t axis = 0; axis < source.region_.rank(); ++axis) {
        const auto divisor = std::gcd(remaining[axis], extent);
        remaining[axis] /= divisor;
        extent /= divisor;
      }
      return extent == 1;
    };
    bool equal = true;
    for (auto extent : spec.batch_axes)
      equal = cancel_factor(extent) && equal;
    for (auto extent : spec.descriptor.shape)
      equal = cancel_factor(extent) && equal;
    for (std::size_t axis = 0; axis < source.region_.rank(); ++axis)
      equal = remaining[axis] == 1 && equal;
    if (!equal)
      return reject({ErrorCode::InvalidArgument,
                     "reshape tensor view changes logical sample count",
                     FailureReason::None,
                     {FailureOrigin::Protocol, FailureScope::Group}});
  } else {
    if (transform.source_axes.size() != source.region_.rank())
      return reject({ErrorCode::InvalidArgument,
                     "invalid tensor view transform",
                     FailureReason::None,
                     {FailureOrigin::Protocol, FailureScope::Group}});
    for (std::size_t axis = 0; axis < source.region_.rank(); ++axis) {
      const auto& map = transform.source_axes[axis];
      if (map.extent != 1 || map.output_axis < -1 ||
          map.output_axis >= static_cast<std::int32_t>(region.rank()))
        return reject({ErrorCode::InvalidArgument,
                       "invalid tensor view transform",
                       FailureReason::None,
                       {FailureOrigin::Protocol, FailureScope::Group}});
      const auto output = map.output_axis < 0
                              ? RegionDimension{0, 1}
                              : region.dimensions()[map.output_axis];
      auto start = map.source_coordinate(output.offset);
      auto end = map.source_coordinate(output.offset + output.extent - 1);
      const auto allowed = source.region_.dimensions()[axis];
      if (!start.ok() || !end.ok() ||
          std::min(start.value(), end.value()) < allowed.offset ||
          std::max(start.value(), end.value()) - allowed.offset >=
              allowed.extent)
        return reject({ErrorCode::InvalidArgument,
                       "tensor view exceeds source authorization",
                       FailureReason::UnauthorizedRead,
                       {FailureOrigin::Protocol, FailureScope::Group}});
    }
  }
  FootprintLimits limits;
  limits.cancellation = cancellation;
  limits.consume_work = [root = impl_->budget](auto n) {
    return root.consume({n});
  };
  auto samples = Footprint::from_regions(spec.sample_shape(), {region}, limits);
  if (!samples.ok())
    return reject(samples.status());
  auto closed = spec.close_samples(samples.value(), limits);
  if (!closed.ok())
    return reject(closed.status());
  if (closed.value() != samples.value() ||
      source.owner_.object_id() == impl_->object)
    return reject(data_internal::invalid_schema());
  auto overlap = samples.value().intersect(prior, limits);
  if (!overlap.ok())
    return reject(overlap.status());
  if (!overlap.value().empty())
    return reject(data_internal::invalid_schema());
  // Metadata for temporary source layouts, rank-bounded coordinates and chunk
  // factors is admitted before constructing standard-library scratch vectors.
  auto scratch = impl_->budget.reserve(ResourceCapacity::host(4096, 4096));
  if (!scratch.ok()) {
    fail(scratch.status());
    return scratch.status();
  }

  if (source.affine_.empty())
    return unavailable();
  ResourceVector<TensorSpan> mapped{
      ResourceAllocator<TensorSpan>(impl_->budget)};
  if (!transform.reshape) {
    for (const auto& piece : source.affine_) {
      auto dims = region.dimensions();
      bool present = true;
      const auto floor_div = [](__int128 a, __int128 b) {
        auto q = a / b;
        if (a % b < 0)
          --q;
        return q;
      };
      for (std::size_t axis = 0; axis < source.region_.rank(); ++axis) {
        const auto& map = transform.source_axes[axis];
        const auto bound = piece.region().dimensions()[axis];
        if (map.output_axis < 0 || !map.step) {
          if (map.source_origin < bound.offset ||
              map.source_origin - bound.offset >= bound.extent)
            present = false;
          continue;
        }
        const __int128 lo =
            static_cast<__int128>(bound.offset) - map.source_origin;
        const __int128 hi = lo + bound.extent - 1;
        const __int128 step = map.step;
        const auto begin =
            step > 0 ? -floor_div(-lo, step) : -floor_div(hi, -step);
        const auto end = step > 0 ? floor_div(hi, step) : floor_div(-lo, -step);
        auto& d = dims[map.output_axis];
        const auto first = std::max(static_cast<__int128>(d.offset),
                                    begin + map.output_origin);
        const auto last =
            std::min(static_cast<__int128>(d.offset) + d.extent - 1,
                     end + map.output_origin);
        if (first > last) {
          present = false;
          break;
        }
        d = {static_cast<std::uint64_t>(first),
             static_cast<std::uint64_t>(last - first + 1)};
      }
      if (!present)
        continue;
      StridedLayout layout;
      layout.byte_strides.resize(region.rank());
      layout.origin.resize(region.rank());
      std::vector<std::uint64_t> at(source.region_.rank());
      for (std::size_t axis = 0; axis < region.rank(); ++axis)
        layout.origin[axis] = dims[axis].offset;
      for (std::size_t axis = 0; axis < at.size(); ++axis) {
        const auto& map = transform.source_axes[axis];
        auto position = map.source_coordinate(
            map.output_axis < 0 ? 0 : dims[map.output_axis].offset);
        if (!position.ok())
          return reject(position.status());
        at[axis] = position.value();
        if (map.output_axis >= 0 && dims[map.output_axis].extent > 1) {
          const __int128 stride =
              static_cast<__int128>(piece.layout().byte_strides[axis]) *
                  map.step +
              layout.byte_strides[map.output_axis];
          if (stride < INT64_MIN || stride > INT64_MAX)
            return unavailable();
          layout.byte_strides[map.output_axis] =
              static_cast<std::int64_t>(stride);
        }
      }
      auto address = piece.byte_address(at);
      if (!address.ok())
        return reject(address.status());
      layout.byte_offset = address.value();
      auto value = TensorSpan::from_storage(
          {spec.descriptor.element_type, spec.sample_shape()},
          Region(std::move(dims)), std::move(layout), piece.storage());
      if (!value.ok())
        return reject(value.status());
      mapped.push_back(value.take_value());
    }
  } else {
    auto joined = input_internal::join_affine_view(
        source.affine_.front().descriptor(), source.region_, source.affine_,
        limits);
    if (!joined.ok())
      return reject(joined.status());
    if (!joined.value())
      return unavailable();
    Result<TensorSpan> common(std::move(*joined.value()));
    auto work = impl_->budget.consume(
        {source.affine_.size() * source.region_.rank() + region.rank()});
    if (!work.ok())
      return reject(work);
    StridedLayout layout;
    layout.byte_strides.resize(region.rank(), 0);
    layout.origin.resize(region.rank());
    std::vector<std::uint64_t> source_at;
    for (auto d : source.region_.dimensions())
      source_at.push_back(d.offset);
    if (transform.reshape) {
      if (!transform.source_axes.empty())
        return reject(data_internal::invalid_schema());
      struct Chunk {
        std::vector<std::uint64_t> factors;
        std::int64_t stride = 0;
      };
      std::vector<Chunk> chunks;
      for (std::size_t axis = source.region_.rank(); axis;) {
        --axis;
        const auto extent = source.region_.dimensions()[axis].extent;
        if (extent == 1)
          continue;
        const auto stride = common.value().layout().byte_strides[axis];
        bool merge = !chunks.empty();
        if (merge && chunks.back().stride == 0) {
          merge = stride == 0;
        } else if (merge) {
          const auto inner = chunks.back().stride;
          const auto magnitude =
              inner < 0 ? -static_cast<__int128>(inner) : inner;
          std::uint64_t product = 1;
          for (auto factor : chunks.back().factors) {
            std::uint64_t next = 0;
            if (!core_internal::checked_multiply(product, factor, &next) ||
                !core_internal::can_multiply(
                    next, static_cast<std::uint64_t>(magnitude), INT64_MAX)) {
              merge = false;
              break;
            }
            product = next;
          }
          merge = merge && static_cast<__int128>(product) * inner == stride;
        }
        if (merge)
          chunks.back().factors.push_back(extent);
        else
          chunks.push_back({{extent}, stride});
      }
      std::size_t axis = region.rank();
      const auto shape = spec.sample_shape();
      for (auto& chunk : chunks) {
        std::uint64_t assigned = 1;
        auto remaining = [&] {
          return std::any_of(chunk.factors.begin(), chunk.factors.end(),
                             [](auto n) { return n != 1; });
        };
        while (axis && (remaining() || shape[axis - 1] == 1)) {
          --axis;
          const auto extent = shape[axis];
          const __int128 stride =
              extent == 1 ? 0 : static_cast<__int128>(assigned) * chunk.stride;
          if (stride < INT64_MIN || stride > INT64_MAX)
            return unavailable();
          layout.byte_strides[axis] = static_cast<std::int64_t>(stride);
          auto needed = extent;
          for (auto& factor : chunk.factors) {
            const auto divisor = std::gcd(factor, needed);
            factor /= divisor;
            needed /= divisor;
          }
          if (needed != 1)
            return unavailable();
          if (chunk.stride) {
            if (!core_internal::can_multiply(extent, assigned))
              return unavailable();
            assigned *= extent;
          }
        }
        if (remaining())
          return unavailable();
      }
      while (axis)
        if (shape[--axis] != 1)
          return unavailable();
      auto offset = common.value().byte_address(source_at);
      if (!offset.ok())
        return reject(offset.status());
      layout.byte_offset = offset.value();
      // Chunk factor cancellation proves equal logical sizes without requiring
      // their full-domain product to fit uint64. Zero strides remain zero.
    } else {
      for (std::size_t axis = 0; axis < source.region_.rank(); ++axis) {
        const auto& map = transform.source_axes[axis];
        const auto output = map.output_axis < 0
                                ? RegionDimension{0, 1}
                                : region.dimensions()[map.output_axis];
        auto start = map.source_coordinate(output.offset);
        source_at[axis] = start.value();
        if (map.output_axis >= 0 &&
            region.dimensions()[map.output_axis].extent > 1) {
          const __int128 stride =
              static_cast<__int128>(
                  common.value().layout().byte_strides[axis]) *
                  map.step +
              layout.byte_strides[map.output_axis];
          if (stride < INT64_MIN || stride > INT64_MAX)
            return unavailable();
          layout.byte_strides[map.output_axis] =
              static_cast<std::int64_t>(stride);
        }
      }
      auto offset = common.value().byte_address(source_at);
      if (!offset.ok())
        return reject(offset.status());
      layout.byte_offset = offset.value();
      for (std::size_t axis = 0; axis < region.rank(); ++axis)
        layout.origin[axis] = region.dimensions()[axis].offset;
    }
    auto reshaped = TensorSpan::from_storage(
        {spec.descriptor.element_type, spec.sample_shape()}, region,
        std::move(layout), common.value().storage());
    if (!reshaped.ok())
      return reject(reshaped.status());
    mapped.push_back(reshaped.take_value());
  }
  std::unique_lock<std::timed_mutex> ownership(result_ownership_mutex,
                                               std::defer_lock);
  while (!ownership.try_lock_for(std::chrono::milliseconds(2)))
    if (cancellation.cancelled())
      return reject({ErrorCode::Cancelled, {}});
  ResourceVector<ResultRef> pending{
      ResourceAllocator<ResultRef>(impl_->budget)};
  ResourceVector<std::uint64_t> visited{
      ResourceAllocator<std::uint64_t>(impl_->budget)};
  if (source.owners_.empty())
    pending.push_back(source.owner_);
  else
    pending.insert(pending.end(), source.owners_.begin(), source.owners_.end());
  while (!pending.empty()) {
    auto current = pending.back();
    pending.pop_back();
    if (current.object_id() == impl_->object)
      return reject(data_internal::invalid_schema());
    if (std::find(visited.begin(), visited.end(), current.object_id()) !=
        visited.end())
      continue;
    visited.push_back(current.object_id());
    auto consumed = impl_->budget.consume({1});
    if (!consumed.ok())
      return reject(consumed);
    std::unique_lock<std::timed_mutex> source_lock(current.impl_->mutex,
                                                   std::defer_lock);
    while (!source_lock.try_lock_for(std::chrono::milliseconds(2)))
      if (cancellation.cancelled())
        return reject({ErrorCode::Cancelled, {}});
    for (const auto& tensor : current.impl_->tensors) {
      for (const auto& owner : tensor.affine_owners)
        pending.push_back(owner);
    }
  }
  ResourceVector<ResultRef> owners{ResourceAllocator<ResultRef>(impl_->budget)};
  if (source.owners_.empty())
    owners.push_back(source.owner_);
  else
    owners.insert(owners.end(), source.owners_.begin(), source.owners_.end());
  return publish_tensor_spans(slot, region, mapped, cancellation,
                              std::move(owners));
} catch (const std::bad_alloc&) {
  auto status = Status{ErrorCode::ResourceExhausted, {}};
  fail(status);
  return status;
} catch (...) {
  auto status = Status{ErrorCode::OperationFailed, {}};
  fail(status);
  return status;
}
Status ResultBuilder::publish_tensor_view(
    std::uint32_t slot, const Region& region,
    const std::vector<const ResultTensorReadWindow*>& sources,
    const CancellationToken& cancellation) try {
  if (!impl_)
    return {ErrorCode::Stale, {}};
  const auto reject = [&](Status status) {
    fail(status);
    return status;
  };
  if (slot >= impl_->schema.tensors.size() || sources.empty())
    return reject(data_internal::invalid_schema());
  const auto& spec = impl_->schema.tensors[slot];
  if (!spec.layout.spatial || region.empty() ||
      !region.validate(spec.sample_shape()).ok() ||
      !singleton_batches(spec, region))
    return reject(data_internal::invalid_schema());
  const auto channel = spec.layout.channel_axis
                           ? spec.batch_axes.size() + *spec.layout.channel_axis
                           : region.rank();
  auto staging_schema = impl_->schema;
  staging_schema.tensors[slot].atomic_trailing_axes = 0;
  auto made =
      ResultBuilder::start(impl_->budget, staging_schema, "tensor.assembly",
                           impl_->limits, {}, impl_->tensors[slot].tile_extent);
  if (!made.ok())
    return reject(made.status());
  auto staging = made.take_value();
  std::uint64_t next =
      channel < region.rank() ? region.dimensions()[channel].offset : 0;
  for (const auto* source : sources) {
    if (!source || !source->valid() || source->region().rank() != region.rank())
      return reject(data_internal::invalid_schema());
    const auto& input = source->spec();
    if (!input.layout.spatial ||
        input.layout.channel_axis != spec.layout.channel_axis ||
        input.layout.height_axis != spec.layout.height_axis ||
        input.layout.width_axis != spec.layout.width_axis ||
        input.batch_axes.size() != spec.batch_axes.size())
      return reject({ErrorCode::InvalidArgument, "incompatible tensor axes"});
    auto dims = region.dimensions();
    if (channel < region.rank()) {
      const auto count = source->region().dimensions()[channel].extent;
      const auto bound = region.dimensions()[channel];
      if (next < bound.offset || count > bound.extent - (next - bound.offset))
        return reject(data_internal::invalid_schema());
      dims[channel] = {next, count};
      next += count;
    } else if (sources.size() != 1) {
      return reject(data_internal::invalid_schema());
    }
    ResultTensorViewTransform transform;
    for (std::size_t axis = 0; axis < region.rank(); ++axis)
      transform.source_axes.push_back(
          {static_cast<std::int32_t>(axis),
           source->region().dimensions()[axis].offset, 1, 1,
           dims[axis].offset});
    auto status = staging.publish_tensor_view(slot, Region(std::move(dims)),
                                              *source, transform, cancellation);
    if (!status.ok())
      return reject(status);
  }
  if (channel < region.rank() &&
      next != region.dimensions()[channel].offset +
                  region.dimensions()[channel].extent)
    return reject(data_internal::invalid_schema());
  auto sealed = staging.seal();
  if (!sealed.ok())
    return reject(sealed.status());
  auto facts = sealed.value().descriptor();
  if (!facts.ok())
    return reject(facts.status());
  auto window =
      sealed.value().acquire_tensor(facts.value(), slot, region, cancellation);
  if (!window.ok())
    return reject(window.status());
  ResultTensorViewTransform identity;
  for (std::size_t axis = 0; axis < region.rank(); ++axis)
    identity.source_axes.push_back({static_cast<std::int32_t>(axis), 0, 1, 1});
  return publish_tensor_view(slot, region, window.value(), identity,
                             cancellation);
} catch (const std::bad_alloc&) {
  Status status{ErrorCode::ResourceExhausted, {}};
  fail(status);
  return status;
} catch (...) {
  Status status{ErrorCode::OperationFailed, {}};
  fail(status);
  return status;
}
}  // namespace ps
