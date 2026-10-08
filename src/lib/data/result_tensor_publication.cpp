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

#include "data/affine_view.hpp"
#include "data/result_host_access.hpp"
#include "data/result_state.hpp"
#include "data/result_support.hpp"
#include "data/result_window_access.hpp"
#include "data/value_validation.hpp"
#include "photospider/data/representation.hpp"
#include "photospider/data/result.hpp"
#include "photospider/data/tensor_description.hpp"

namespace ps {
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
                                     ResultRelation relation,
                                     ResultFinality finality,
                                     const CancellationToken& cancellation) {
  return publish_tensor_storage(slot, region, std::move(layout),
                                std::move(storage), std::move(relation),
                                finality, cancellation, {});
}
Status ResultBuilder::publish_tensor_storage(
    std::uint32_t slot, const Region& region, StridedLayout layout,
    std::shared_ptr<const CpuStorage> storage, ResultRelation relation,
    ResultFinality finality, const CancellationToken& cancellation,
    ResourceVector<ResultRef> sources) try {
  if (!impl_)
    return Status{ErrorCode::Stale, {}};
  ResourceAllocationScope scope(impl_->budget);
  std::unique_lock<std::timed_mutex> lock(impl_->mutex, std::defer_lock);
  while (!lock.try_lock_for(std::chrono::milliseconds(2))) {
    if (cancellation.cancelled()) {
      impl_->cancelled_publication.store(true);
      return Status{ErrorCode::Cancelled, {}};
    }
  }
  if (auto busy = impl_->reject_active_mutation(); !busy.ok())
    return busy;
  const auto reject = [&](Status status) {
    impl_->failure.record(status);
    return status;
  };
  if (impl_->complete || !impl_->status().ok())
    return impl_->status().ok() ? Status{ErrorCode::Stale, {}}
                                : impl_->status();
  if (cancellation.cancelled())
    return reject({ErrorCode::Cancelled, {}});
  if (slot >= impl_->schema.tensors.size() || !storage ||
      !finality.satisfied() || !impl_->descriptor_relation.valid() ||
      !relation.owned_by(impl_->budget))
    return reject(data_internal::invalid_schema());
  const auto& spec = impl_->schema.tensors[slot];
  auto& target = impl_->tensors[slot];
  // A source view retains its independent root without admitting its payload
  // again. Importing external storage instead references it in this root
  // ledger.
  auto retained = sources.empty()
                      ? impl_->budget.reference(std::move(storage))
                      : Result<std::shared_ptr<const CpuStorage>>(storage);
  if (!retained.ok())
    return reject(retained.status());
  if (sources.empty() &&
      retained.value()->capacity() > impl_->limits.maximum_bytes)
    return reject(
        {ErrorCode::ResourceExhausted, "tensor storage growth limit"});
  auto value =
      Value::from_storage({spec.descriptor.element_type, spec.sample_shape()},
                          region, std::move(layout), retained.take_value());
  if (!value.ok())
    return reject(value.status());
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
  auto count = spec.sample_count();
  if (closed.value() != samples.value() ||
      relation.coverage() != (count.ok() ? count.value() : UINT64_MAX))
    return reject(data_internal::invalid_schema());
  auto overlap = samples.value().intersect(target.coverage, limits);
  if (!overlap.ok())
    return reject(overlap.status());
  if (!overlap.value().empty())
    return reject(data_internal::invalid_schema());
  auto coverage = target.coverage.unite(samples.value(), limits);
  if (!coverage.ok())
    return reject(coverage.status());
  if (relation.guarantee() != DependencyGuarantee::Unknown) {
    auto certified = relation.certify(samples.value(), limits);
    if (!certified.ok())
      return reject(certified);
  }
  auto restricted = relation.restrict_to(spec.sample_shape(), region);
  if (!restricted.ok())
    return reject(restricted.status());
  relation = restricted.take_value();
  auto combined =
      !target.coverage.empty() && !target.relation.same_owner(relation)
          ? ResultRelation::unite(impl_->budget, {target.relation, relation})
          : Result<ResultRelation>(relation);
  if (!combined.ok())
    return reject(combined.status());
  auto admission = impl_->budget.reserve(ResourceCapacity::host(
      sizeof(Value) +
          region.rank() * (sizeof(RegionDimension) + 3 * sizeof(std::uint64_t)),
      sizeof(Value) + region.rank() * (sizeof(RegionDimension) +
                                       3 * sizeof(std::uint64_t))));
  if (!admission.ok())
    return reject(admission.status());
  const auto metadata_bytes =
      sizeof(Value) +
      region.rank() * (sizeof(RegionDimension) + 3 * sizeof(std::uint64_t));
  std::uint64_t growth_bytes =
      sources.empty() ? value.value().storage()->capacity() + metadata_bytes
                      : metadata_bytes;
  for (const auto& member : impl_->tensors)
    if (std::any_of(member.affine.begin(), member.affine.end(),
                    [&](const auto& prior) {
                      return prior.storage() == value.value().storage();
                    })) {
      growth_bytes = metadata_bytes;
      break;
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
  target.affine_owners.reserve(target.affine_owners.size() + sources.size());
  target.affine_growth.reserve(target.affine_growth.size() + 1);
  target.affine.reserve(target.affine.size() + 1);
  target.affine_metadata.reserve(target.affine_metadata.size() + 1);
  if (cancellation.cancelled())
    return reject({ErrorCode::Cancelled, {}});
  if (impl_->revision == UINT64_MAX)
    return reject(data_internal::invalid_schema());
  for (auto& source : sources)
    target.affine_owners.push_back(std::move(source));
  target.affine.push_back(value.take_value());
  target.affine_metadata.push_back(admission.take_value());
  target.affine_growth.push_back(std::move(growth_owner));
  target.coverage = coverage.take_value();
  target.relation = combined.take_value();
  ++impl_->revision;
  return Status::success();
} catch (const std::bad_alloc&) {
  auto status = Status{ErrorCode::ResourceExhausted, {}};
  fail(status);
  return status;
} catch (...) {
  auto status = Status{ErrorCode::OperationFailed, {}};
  fail(status);
  return status;
}
Status ResultBuilder::publish_tensor(
    std::uint32_t slot, const Region& region, ByteView packed,
    ResultRelation relation, ResultFinality finality,
    const CancellationToken& cancellation) try {
  if (!impl_)
    return Status{ErrorCode::Stale, {}};
  if (slot >= impl_->schema.tensors.size()) {
    auto status = data_internal::invalid_schema();
    fail(status);
    return status;
  }
  const auto width =
      Value::element_size(impl_->schema.tensors[slot].descriptor.element_type);
  auto count = region.element_count();
  if (!count.ok() || count.value() > UINT64_MAX / width ||
      count.value() * width != packed.size() ||
      (packed.size() && !packed.data())) {
    auto status = data_internal::invalid_schema();
    fail(status);
    return status;
  }
  auto copy =
      impl_->budget.consume({0, packed.size(), packed.size() ? 1U : 0U});
  if (!copy.ok()) {
    fail(copy);
    return copy;
  }
  if (!impl_->schema.tensors[slot].layout.spatial && count.value()) {
    auto allocated = impl_->budget.allocator().allocate(packed.size());
    if (!allocated.ok()) {
      fail(allocated.status());
      return allocated.status();
    }
    auto buffer = allocated.take_value();
    if (packed.size())
      std::memcpy(buffer.data(), packed.data(), packed.size());
    StridedLayout layout;
    layout.byte_strides.resize(region.rank());
    std::uint64_t stride = width;
    for (std::size_t axis = region.rank(); axis; --axis) {
      layout.origin.push_back(0);
      if (stride > INT64_MAX) {
        auto failure = data_internal::invalid_schema();
        fail(failure);
        return failure;
      }
      layout.byte_strides[axis - 1] = stride;
      stride *= region.dimensions()[axis - 1].extent;
    }
    for (std::size_t axis = 0; axis < region.rank(); ++axis)
      layout.origin[axis] = region.dimensions()[axis].offset;
    return publish_tensor(slot, region, std::move(layout),
                          std::move(buffer).freeze(), std::move(relation),
                          finality, cancellation);
  }
  return publish_tensor_kernel(
      slot, region,
      [&](const auto& writers) {
        std::uint64_t next = 0;
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
            const auto n =
                writer.sample_axis() == last ? run.value().samples : 1;
            std::memcpy(run.value().data, packed.data() + next, n * width);
            next += n * width;
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
      std::move(relation), finality, cancellation);
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
    ResultRelation relation, ResultFinality finality,
    const CancellationToken& cancellation) {
  const auto owner = impl_;
  if (!owner)
    return Status{ErrorCode::Stale, {}};
  try {
    std::optional<ResourceAllocationScope> metadata_scope;
    const auto* active_root = resource_internal::metadata_budget();
    if (!active_root || !active_root->same_owner(owner->budget))
      metadata_scope.emplace(owner->budget);
    std::unique_lock<std::timed_mutex> lock(owner->mutex, std::defer_lock);
    while (!lock.try_lock_for(std::chrono::milliseconds(2)))
      if (cancellation.cancelled()) {
        owner->cancelled_publication = true;
        return Status{ErrorCode::Cancelled, {}};
      }
    if (cancellation.cancelled()) {
      owner->cancelled_publication = true;
      return Status{ErrorCode::Cancelled, {}};
    }
    const auto reject = [&](Status status) {
      owner->failure.record(status);
      return status;
    };
    if (owner->complete || !owner->status().ok())
      return owner->status().ok() ? Status{ErrorCode::Stale, {}}
                                  : owner->status();
    if (auto busy = owner->reject_active_mutation(); !busy.ok())
      return busy;
    owner->kernel_active.store(true);
    ProducerWriteGuard producer_guard{&owner->kernel_active};
    const auto revision = owner->revision;
    if (slot >= owner->schema.tensors.size() || !finality.satisfied() ||
        !owner->descriptor_relation.valid() ||
        !relation.owned_by(owner->budget))
      return reject(data_internal::invalid_schema());
    const auto& spec = owner->schema.tensors[slot];
    auto& target = owner->tensors[slot];
    FootprintLimits limits;
    limits.cancellation = cancellation;
    limits.consume_work = [root = owner->budget](auto n) {
      return root.consume({n});
    };
    auto samples =
        Footprint::from_regions(spec.sample_shape(), {region}, limits);
    if (!samples.ok())
      return reject(samples.status());
    auto closed = spec.close_samples(samples.value(), limits);
    if (!closed.ok())
      return reject(closed.status());
    const auto domain_count = spec.sample_count();
    if (closed.value() != samples.value() ||
        relation.coverage() !=
            (domain_count.ok() ? domain_count.value() : UINT64_MAX))
      return reject(data_internal::invalid_schema());
    auto overlap = samples.value().intersect(target.coverage, limits);
    if (!overlap.ok())
      return reject(overlap.status());
    if (!overlap.value().empty())
      return reject(data_internal::invalid_schema());
    auto coverage = target.coverage.unite(samples.value(), limits);
    if (!coverage.ok())
      return reject(coverage.status());
    auto count = samples.value().element_count();
    const auto width = Value::element_size(spec.descriptor.element_type);
    if (!count.ok() || count.value() > UINT64_MAX / width || !write ||
        owner->revision == UINT64_MAX)
      return reject(data_internal::invalid_schema());
    auto copied_work = owner->budget.consume({count.value()});
    if (!copied_work.ok())
      return reject(copied_work);
    if (relation.guarantee() != DependencyGuarantee::Unknown) {
      auto certified = relation.certify(samples.value(), limits);
      if (!certified.ok())
        return reject(certified);
    }
    auto restricted = relation.restrict_to(spec.sample_shape(), region);
    if (!restricted.ok())
      return reject(restricted.status());
    relation = restricted.take_value();
    auto combined =
        !target.coverage.empty() && target.relation.valid() &&
                !target.relation.same_owner(relation)
            ? ResultRelation::unite(owner->budget, {target.relation, relation})
            : Result<ResultRelation>(relation);
    if (!combined.ok())
      return reject(combined.status());
    // A sparse logical domain need not have a representable full canvas.
    // Reuse the bounded affine transaction when planar byte geometry cannot
    // represent that domain; the typed schema and global coordinates persist.
    const bool affine = !spec.layout.spatial || !domain_count.ok() ||
                        domain_count.value() > UINT64_MAX / width;
    if (affine && !samples.value().empty()) {
      auto allocated =
          owner->budget.allocator().allocate(count.value() * width);
      if (!allocated.ok())
        return reject(allocated.status());
      auto buffer = allocated.take_value();
      ResourceVector<ResultTensorWriteWindow> windows{
          ResourceAllocator<ResultTensorWriteWindow>(owner->budget)};
      ResultTensorWriteWindow window;
      window.spec_ = &spec;
      window.region_ = region;
      window.affine_data_ = buffer.data();
      window.affine_strides_.resize(region.rank());
      std::uint64_t stride = width;
      StridedLayout layout;
      layout.byte_strides.resize(region.rank());
      layout.origin.resize(region.rank());
      for (std::size_t axis = region.rank(); axis; --axis) {
        if (stride > INT64_MAX)
          return reject(data_internal::invalid_schema());
        window.affine_strides_[axis - 1] = stride;
        layout.byte_strides[axis - 1] = stride;
        layout.origin[axis - 1] = region.dimensions()[axis - 1].offset;
        stride *= region.dimensions()[axis - 1].extent;
      }
      windows.push_back(std::move(window));
      lock.unlock();
      auto written = write(windows);
      lock.lock();
      if (!owner->status().ok())
        return owner->status();
      if (impl_ != owner || owner->complete || owner->revision != revision)
        return reject(
            {ErrorCode::Stale, "tensor producer changed during callback"});
      if (!written.ok())
        return reject(written);
      producer_guard.release();
      lock.unlock();
      return publish_tensor(slot, region, std::move(layout),
                            std::move(buffer).freeze(), std::move(relation),
                            finality, cancellation);
    }
    if (!samples.value().empty()) {
      const auto batches = spec.batch_axes.size();
      Region plane_region(std::vector<RegionDimension>(
          region.dimensions().begin() + batches, region.dimensions().end()));
      ResourceVector<PlanarImageWriteWindow> writers{
          ResourceAllocator<PlanarImageWriteWindow>(owner->budget)};
      ResourceVector<data_internal::ResultSpatialBacking> candidates{
          ResourceAllocator<data_internal::ResultSpatialBacking>(
              owner->budget)};
      std::vector<std::uint64_t> at;
      for (std::size_t axis = 0; axis < batches; ++axis)
        at.push_back(region.dimensions()[axis].offset);
      for (;;) {
        PlanarImage backing;
        for (const auto& entry : target.backing)
          if (std::equal(entry.batch.begin(), entry.batch.end(), at.begin())) {
            backing = entry.image;
            break;
          }
        if (!backing.valid()) {
          auto created = [&]() -> Result<PlanarImage> {
            std::uint64_t bytes =
                spec.layout.groups.size() * sizeof(ImageComponentGroup);
            for (const auto& group : spec.layout.groups)
              bytes += group.role.size();
            auto lease =
                owner->budget.reserve(ResourceCapacity::host(bytes, bytes));
            if (!lease.ok())
              return Result<PlanarImage>(lease.status());
            auto config = target.config;
            config.groups = spec.layout.groups;
            return PlanarImage::create(spec.descriptor, config, spec.facets,
                                       owner->resources);
          }();
          if (!created.ok())
            return reject(created.status());
          backing = created.take_value();
          candidates.push_back(
              {ResourceVector<std::uint64_t>(
                   at.begin(), at.end(),
                   ResourceAllocator<std::uint64_t>(owner->budget)),
               backing});
        }
        auto prepared = backing.begin_write(plane_region, cancellation);
        if (!prepared.ok())
          return reject(prepared.status());
        writers.push_back(prepared.take_value());
        std::size_t axis = batches;
        while (axis) {
          --axis;
          const auto d = region.dimensions()[axis];
          if (++at[axis] < d.offset + d.extent)
            break;
          at[axis] = d.offset;
        }
        if (!axis && (at.empty() || at[0] == region.dimensions()[0].offset))
          break;
      }
      // New roots stay private until the producer transaction succeeds. Reserve
      // installation metadata before invoking user work so commit cannot
      // allocate.
      target.backing.reserve(target.backing.size() + candidates.size());
      // User work must not hold the Result state lock. Descriptor readers and
      // ownership publication on independent builders remain reentrant. Planar
      // writer locks protect the unpublished bytes until commit or rollback.
      ResourceVector<ResultTensorWriteWindow> windows{
          ResourceAllocator<ResultTensorWriteWindow>(owner->budget)};
      windows.reserve(writers.size());
      at.clear();
      for (std::size_t axis = 0; axis < batches; ++axis)
        at.push_back(region.dimensions()[axis].offset);
      for (const auto& writer : writers) {
        ResultTensorWriteWindow window;
        window.spec_ = &spec;
        auto dimensions = region.dimensions();
        for (std::size_t axis = 0; axis < batches; ++axis)
          dimensions[axis] = {at[axis], 1};
        window.region_ = Region(std::move(dimensions));
        window.planar_ = &writer;
        windows.push_back(std::move(window));
        for (std::size_t axis = batches; axis;) {
          --axis;
          const auto d = region.dimensions()[axis];
          if (++at[axis] < d.offset + d.extent)
            break;
          at[axis] = d.offset;
        }
      }
      lock.unlock();
      auto written = write(windows);
      lock.lock();
      if (!owner->status().ok())
        return owner->status();
      if (impl_ != owner || owner->complete || owner->revision != revision)
        return reject(
            {ErrorCode::Stale, "tensor producer changed during callback"});
      if (!written.ok())
        return reject(written);
      if (cancellation.cancelled())
        return reject(Status{ErrorCode::Cancelled, {}});
      // All admission/copy/cancellation checks precede this allocation-free
      // commit barrier. A failed preparation retires every fresh page/window.
      for (auto& writer : writers) {
        auto status = writer.commit();
        if (!status.ok())
          return reject(status);
      }
      for (auto& candidate : candidates)
        target.backing.push_back(std::move(candidate));
    }
    target.coverage = coverage.take_value();
    target.relation = combined.take_value();
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
    const ResultTensorViewTransform& transform, ResultRelation relation,
    ResultFinality finality, const CancellationToken& cancellation) try {
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
    if (slot >= impl_->schema.tensors.size() || !finality.satisfied() ||
        !impl_->descriptor_relation.valid() ||
        !relation.owned_by(impl_->budget)) {
      impl_->failure.record(data_internal::invalid_schema());
      return impl_->status();
    }
    prior = impl_->tensors[slot].coverage;
  }
  const auto unavailable = [] {
    return Status{ErrorCode::InvalidArgument,
                  "ViewUnavailable: source has no affine physical mapping"};
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
  if (source.owner_.request_record_)
    return reject({ErrorCode::InvalidArgument,
                   "terminal Result cannot supply a tensor view",
                   FailureReason::None,
                   {FailureOrigin::Protocol, FailureScope::Group}});

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
  const auto count = spec.sample_count();
  if (closed.value() != samples.value() ||
      relation.coverage() != (count.ok() ? count.value() : UINT64_MAX) ||
      source.owner_.object_id() == impl_->object)
    return reject(data_internal::invalid_schema());
  auto overlap = samples.value().intersect(prior, limits);
  if (!overlap.ok())
    return reject(overlap.status());
  if (!overlap.value().empty())
    return reject(data_internal::invalid_schema());
  if (relation.guarantee() != DependencyGuarantee::Unknown) {
    auto certified = relation.certify(samples.value(), limits);
    if (!certified.ok())
      return reject(certified);
  }
  // Metadata for temporary source layouts, rank-bounded coordinates and chunk
  // factors is admitted before constructing standard-library scratch vectors.
  auto scratch = impl_->budget.reserve(ResourceCapacity::host(4096, 4096));
  if (!scratch.ok()) {
    fail(scratch.status());
    return scratch.status();
  }

  if (source.affine_.empty() || !source.pieces_.empty())
    return unavailable();
  auto joined =
      input_internal::join_affine_view(source.affine_.front().descriptor(),
                                       source.region_, source.affine_, limits);
  if (!joined.ok())
    return reject(joined.status());
  if (!joined.value())
    return unavailable();
  Result<Value> common(std::move(*joined.value()));
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
          if (factor >
              static_cast<unsigned __int128>(INT64_MAX) / magnitude / product) {
            merge = false;
            break;
          }
          product *= factor;
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
          if (extent > UINT64_MAX / assigned)
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
            static_cast<__int128>(common.value().layout().byte_strides[axis]) *
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
      for (const auto& view : tensor.views)
        for (const auto& owner : view.owners)
          pending.push_back(owner);
    }
  }
  ResourceVector<ResultRef> owners{ResourceAllocator<ResultRef>(impl_->budget)};
  if (source.owners_.empty())
    owners.push_back(source.owner_);
  else
    owners.insert(owners.end(), source.owners_.begin(), source.owners_.end());
  return publish_tensor_storage(slot, region, std::move(layout),
                                common.value().storage(), std::move(relation),
                                finality, cancellation, std::move(owners));
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
    ResultRelation relation, ResultFinality finality,
    const CancellationToken& cancellation) try {
  if (!impl_)
    return Status{ErrorCode::Stale, {}};
  std::optional<ResourceAllocationScope> scope;
  if (!resource_internal::metadata_budget() ||
      !resource_internal::metadata_budget()->same_owner(impl_->budget))
    scope.emplace(impl_->budget);
  // All graph ownership mutations share one gate before taking Result locks.
  // Kernel callbacks never hold those locks, so snapshot traversal cannot form
  // a callback/ownership lock cycle. The gate makes indirect cycle checks and
  // edge publication atomic with competing builders and host associations.
  std::unique_lock<std::timed_mutex> ownership(result_ownership_mutex,
                                               std::defer_lock);
  while (!ownership.try_lock_for(std::chrono::milliseconds(2)))
    if (cancellation.cancelled()) {
      impl_->cancelled_publication = true;
      return Status{ErrorCode::Cancelled, {}};
    }
  std::unique_lock<std::timed_mutex> lock(impl_->mutex, std::defer_lock);
  while (!lock.try_lock_for(std::chrono::milliseconds(2)))
    if (cancellation.cancelled()) {
      impl_->cancelled_publication = true;
      return Status{ErrorCode::Cancelled, {}};
    }
  if (cancellation.cancelled()) {
    impl_->cancelled_publication = true;
    return Status{ErrorCode::Cancelled, {}};
  }
  if (auto busy = impl_->reject_active_mutation(); !busy.ok())
    return busy;
  auto reject = [&](Status status) {
    impl_->failure.record(status);
    return status;
  };
  auto unavailable = [] {
    return Status{
        ErrorCode::InvalidArgument,
        "ViewUnavailable: sources have no canonical common-owner mapping"};
  };
  if (impl_->complete || !impl_->status().ok())
    return impl_->status().ok() ? Status{ErrorCode::Stale, {}}
                                : impl_->status();
  if (slot >= impl_->schema.tensors.size() || !finality.satisfied() ||
      !impl_->descriptor_relation.valid() || !relation.owned_by(impl_->budget))
    return reject(data_internal::invalid_schema());
  const auto& spec = impl_->schema.tensors[slot];
  auto& target = impl_->tensors[slot];
  if (!spec.layout.spatial || region.empty() ||
      !region.validate(spec.sample_shape()).ok() ||
      !singleton_batches(spec, region) || sources.empty())
    return reject(data_internal::invalid_schema());
  FootprintLimits limits;
  limits.cancellation = cancellation;
  limits.consume_work = [budget = impl_->budget](auto n) {
    return budget.consume({n});
  };
  auto samples = Footprint::from_regions(spec.sample_shape(), {region}, limits);
  if (!samples.ok())
    return reject(samples.status());
  auto closed = spec.close_samples(samples.value(), limits);
  if (!closed.ok())
    return reject(closed.status());
  const auto domain_count = spec.sample_count();
  if (closed.value() != samples.value() ||
      relation.coverage() !=
          (domain_count.ok() ? domain_count.value() : UINT64_MAX))
    return reject(data_internal::invalid_schema());
  auto overlap = samples.value().intersect(target.coverage, limits);
  if (!overlap.ok())
    return reject(overlap.status());
  if (!overlap.value().empty())
    return reject(data_internal::invalid_schema());
  auto next = target.coverage.unite(samples.value(), limits);
  if (!next.ok())
    return reject(next.status());
  if (relation.guarantee() != DependencyGuarantee::Unknown) {
    auto certified = relation.certify(samples.value(), limits);
    if (!certified.ok())
      return reject(certified);
  }
  std::vector<PlanarImage> tensors;
  std::vector<std::uint64_t> channels, counts;
  ResourceVector<ResultRef> owners(ResourceAllocator<ResultRef>(impl_->budget));
  std::vector<RegionDimension> spatial(
      region.dimensions().begin() + spec.batch_axes.size(),
      region.dimensions().end());
  for (const auto* source : sources) {
    if (source && source->valid() && source->owner_.request_record_)
      return reject({ErrorCode::InvalidArgument,
                     "terminal Result cannot supply a tensor view",
                     FailureReason::None,
                     {FailureOrigin::Protocol, FailureScope::Group}});
    if (!source || !source->valid() || source->pieces_.empty() ||
        !source->affine_.empty())
      return unavailable();
    // A view may cross root budgets; retaining its independently accounted
    // source is mandatory. Only new output/view metadata is charged here.
    ResourceVector<ResultRef> pending(
        ResourceAllocator<ResultRef>(impl_->budget));
    if (source->owners_.empty())
      pending.push_back(source->owner_);
    else
      pending.insert(pending.end(), source->owners_.begin(),
                     source->owners_.end());
    std::vector<std::uint64_t> visited;
    while (!pending.empty()) {
      auto current = pending.back();
      pending.pop_back();
      if (current.object_id() == impl_->object)
        return reject(data_internal::invalid_schema());
      if (std::find(visited.begin(), visited.end(), current.object_id()) !=
          visited.end())
        continue;
      if (visited.size() >= 4096)
        return reject(Status{ErrorCode::ResourceExhausted,
                             "view association work limit"});
      visited.push_back(current.object_id());
      auto work = impl_->budget.consume({1});
      if (!work.ok())
        return reject(work);
      std::unique_lock<std::timed_mutex> source_lock(current.impl_->mutex,
                                                     std::defer_lock);
      while (!source_lock.try_lock_for(std::chrono::milliseconds(2)))
        if (cancellation.cancelled())
          return reject(Status{ErrorCode::Cancelled, {}});
      for (const auto& image : current.impl_->tensors) {
        for (const auto& owner : image.affine_owners)
          pending.push_back(owner);
        for (const auto& view : image.views)
          for (const auto& owner : view.owners)
            pending.push_back(owner);
      }
    }
    const auto& source_spec = source->spec();
    const auto& input_region = source->region_.dimensions();
    if (input_region[source_spec.batch_axes.size() +
                     source_spec.layout.height_axis]
                .offset != spatial[spec.layout.height_axis].offset ||
        input_region[source_spec.batch_axes.size() +
                     source_spec.layout.height_axis]
                .extent != spatial[spec.layout.height_axis].extent ||
        input_region[source_spec.batch_axes.size() +
                     source_spec.layout.width_axis]
                .offset != spatial[spec.layout.width_axis].offset ||
        input_region[source_spec.batch_axes.size() +
                     source_spec.layout.width_axis]
                .extent != spatial[spec.layout.width_axis].extent)
      return unavailable();
    const auto& first = *source->pieces_[0].image_;
    const auto zero =
        std::vector<std::uint64_t>(source_spec.descriptor.shape.size(), 0);
    const auto origin = first.byte_offset(zero);
    if (!origin.ok())
      return reject(origin.status());
    for (const auto& piece : source->pieces_) {
      auto offset = piece.image_->byte_offset(zero);
      if (!offset.ok())
        return reject(offset.status());
      if (piece.image_->owner_token() != first.owner_token() ||
          offset.value() != origin.value())
        return unavailable();
    }
    const auto c = source_spec.layout.channel_axis
                       ? input_region[source_spec.batch_axes.size() +
                                      *source_spec.layout.channel_axis]
                       : RegionDimension{0, 1};
    tensors.push_back(first);
    channels.push_back(c.offset);
    counts.push_back(c.extent);
    if (source->owners_.empty())
      owners.push_back(source->owner_);
    else
      owners.insert(owners.end(), source->owners_.begin(),
                    source->owners_.end());
  }
  auto alias = PlanarImage::assemble_view(
      tensors, channels, counts, spec.descriptor,
      data_internal::planar_layout(spec.layout), Region(std::move(spatial)),
      spec.facets, impl_->image_budget, cancellation, impl_->resources);
  if (!alias.ok()) {
    if (alias.status().code == ErrorCode::InvalidArgument &&
        alias.status().message.find("ViewUnavailable") != std::string::npos)
      return alias.status();
    return reject(alias.status());
  }
  auto restricted = relation.restrict_to(spec.sample_shape(), region);
  if (!restricted.ok())
    return reject(restricted.status());
  relation = restricted.take_value();
  auto combined =
      !target.coverage.empty() && !target.relation.same_owner(relation)
          ? ResultRelation::unite(impl_->budget, {target.relation, relation})
          : Result<ResultRelation>(relation);
  if (!combined.ok())
    return reject(combined.status());
  if (impl_->revision == UINT64_MAX)
    return reject(data_internal::invalid_schema());
  target.views.push_back({region, alias.take_value(), std::move(owners)});
  target.coverage = next.take_value();
  target.relation = combined.take_value();
  ++impl_->revision;
  return Status::success();
} catch (const std::bad_alloc&) {
  auto status = Status{ErrorCode::ResourceExhausted, {}};
  fail(status);
  return status;
} catch (...) {
  auto status = Status{ErrorCode::OperationFailed, {}};
  fail(status);
  return status;
}
}  // namespace ps
