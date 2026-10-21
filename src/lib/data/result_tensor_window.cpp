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

#include "core/resource_observation.hpp"
#include "data/affine_view.hpp"
#include "data/footprint_index.hpp"
#include "data/result_device_access.hpp"
#include "data/result_state.hpp"
#include "data/result_support.hpp"
#include "data/result_window_access.hpp"
#include "data/tensor_validation.hpp"
#include "photospider/core/checked_math.hpp"
#include "photospider/data/result.hpp"

namespace ps {
ResultTensorReadWindow& ResultTensorReadWindow::operator=(
    ResultTensorReadWindow&& other) noexcept {
  if (this != &other) {
    ResultTensorReadWindow replaced(std::move(other));
    using std::swap;
    swap(lease_, replaced.lease_);
    swap(owner_, replaced.owner_);
    owners_.swap(replaced.owners_);
    source_leases_.swap(replaced.source_leases_);
    swap(slot_, replaced.slot_);
    swap(region_, replaced.region_);
    swap(cancellation_, replaced.cancellation_);
    affine_.swap(replaced.affine_);
    swap(affine_canonical_, replaced.affine_canonical_);
  }
  return *this;
}
namespace {
struct WindowWorkFailure {
  Status status;
};
void window_tick(const ResourceBudget& root,
                 const CancellationToken& cancellation,
                 std::uint64_t count = 1) {
  if (cancellation.cancelled())
    throw WindowWorkFailure{Status{ErrorCode::Cancelled, {}}};
  auto status = root.consume({count});
  if (!status.ok())
    throw WindowWorkFailure{std::move(status)};
}
}  // namespace
Status execution_internal::ResultWindowAccess::index(
    ResultTensorReadWindow& window) try {
  const auto& root = window.owner_.impl_->budget;
  const auto tick = [&] { window_tick(root, window.cancellation_); };
  window.affine_canonical_ = false;
  std::sort(window.affine_.begin(), window.affine_.end(),
            [&](const auto& a, const auto& b) {
              for (std::size_t axis = 0; axis < window.region_.rank(); ++axis) {
                tick();
                const auto x = a.region().dimensions()[axis],
                           y = b.region().dimensions()[axis];
                if (x.offset != y.offset)
                  return x.offset < y.offset;
                if (x.extent != y.extent)
                  return x.extent < y.extent;
              }
              return false;
            });
  // Equal prefix intervals must form one group. The first different interval
  // of adjacent rectangles must be disjoint, so per-axis binary lookup is safe.
  for (std::size_t i = 1; i < window.affine_.size(); ++i) {
    bool disjoint = false;
    for (std::size_t axis = 0; axis < window.region_.rank(); ++axis) {
      tick();
      const auto a = window.affine_[i - 1].region().dimensions()[axis];
      const auto b = window.affine_[i].region().dimensions()[axis];
      if (a.offset == b.offset && a.extent == b.extent)
        continue;
      disjoint = a.offset + a.extent <= b.offset;
      break;
    }
    if (!disjoint)
      return Status::success();
  }
  window.affine_canonical_ = true;
  return Status::success();
} catch (const WindowWorkFailure& failure) {
  return failure.status;
}

Result<std::size_t> execution_internal::ResultWindowAccess::find(
    const ResultTensorReadWindow& window,
    const std::vector<std::uint64_t>& at) try {
  const auto tick = [&] {
    window_tick(window.owner_.impl_->budget, window.cancellation_);
  };
  if (window.affine_canonical_)
    return Result<std::size_t>(footprint_internal::containing(
        window.affine_.size(), at.size(), at.data(),
        [&](auto i) -> const Region& { return window.affine_[i].region(); },
        tick));
  for (std::size_t i = 0; i < window.affine_.size(); ++i) {
    bool contains = true;
    for (std::size_t axis = 0; axis < at.size(); ++axis) {
      tick();
      const auto d = window.affine_[i].region().dimensions()[axis];
      if (at[axis] < d.offset || at[axis] - d.offset >= d.extent) {
        contains = false;
        break;
      }
    }
    if (contains)
      return Result<std::size_t>(i);
  }
  return Result<std::size_t>(window.affine_.size());
} catch (const WindowWorkFailure& failure) {
  return Result<std::size_t>(failure.status);
}

Result<TensorSpan> ResultTensorReadWindow::affine_view() const {
  return execution_internal::ResultWindowAccess::affine(*this);
}
Result<std::uint64_t> ResultTensorReadWindow::read_work() const {
  return execution_internal::ResultWindowAccess::read_work(*this);
}
Result<bool> ResultTensorReadWindow::visit_backing_regions(
    const ResourceBudget& budget,
    const std::function<Status(const Region&)>& visitor) const {
  return execution_internal::ResultWindowAccess::visit_backing_regions(
      *this, budget, visitor);
}

Result<ResultTensorReadWindow> execution_internal::ResultWindowAccess::compose(
    const ResourceBudget& budget, const Region& region,
    ResourceVector<ResultTensorReadWindow> windows,
    const CancellationToken& cancellation) try {
  using Answer = Result<ResultTensorReadWindow>;
  ResourceAllocationScope scope(budget);
  if (windows.empty() || windows.size() > 65536 || region.empty())
    return Answer(data_internal::invalid_schema());
  auto& first = windows.front();
  if (!first.valid() || !first.owner_.owned_by(budget))
    return Answer(data_internal::invalid_schema());
  const data_internal::SampleDescriptor sample(first.spec());
  const auto& shape = sample.value.shape;
  FootprintLimits limits;
  limits.cancellation = cancellation;
  limits.consume_work = [budget](auto n) { return budget.consume({n}); };
  auto requested = Footprint::from_region(shape, region, limits);
  auto covered = Footprint::none(shape, limits);
  if (!requested.ok() || !covered.ok())
    return Answer(requested.ok() ? covered.status() : requested.status());
  std::size_t affine = 0, owners = 0, leases = 0;
  for (const auto& window : windows) {
    if (!window.valid() || !window.owner_.owned_by(budget) ||
        window.slot_ != first.slot_)
      return Answer(data_internal::invalid_schema());
    auto charged = budget.consume({1 + window.owner_.schema().canonical_size() +
                                   first.owner_.schema().canonical_size() +
                                   region.rank()});
    if (!charged.ok())
      return Answer(charged);
    if (!window.owner_.schema().same_schema(first.owner_.schema()))
      return Answer(data_internal::invalid_schema());
    auto part = Footprint::from_region(shape, window.region_, limits);
    if (!part.ok())
      return Answer(part.status());
    auto outside = part.value().subtract(requested.value(), limits);
    auto overlap = part.value().intersect(covered.value(), limits);
    if (!outside.ok() || !overlap.ok())
      return Answer(outside.ok() ? overlap.status() : outside.status());
    if (!outside.value().empty() || !overlap.value().empty())
      return Answer(data_internal::invalid_schema());
    auto joined = covered.value().unite(part.value(), limits);
    if (!joined.ok())
      return Answer(joined.status());
    covered = std::move(joined);
    affine += window.affine_.size();
    owners += window.owners_.empty() ? 1 : window.owners_.size();
    leases += 1 + window.source_leases_.size();
  }
  if (covered.value() != requested.value())
    return Answer(data_internal::unavailable());
  auto lease = budget.reserve(ResourceCapacity::host(
      sizeof(ResultTensorReadWindow) + region.rank() * sizeof(RegionDimension),
      sizeof(ResultTensorReadWindow) +
          region.rank() * sizeof(RegionDimension)));
  if (!lease.ok())
    return Answer(lease.status());
  ResultTensorReadWindow result;
  result.lease_ = lease.take_value();
  result.owner_ = first.owner_;
  result.slot_ = first.slot_;
  result.region_ = region;
  core_internal::ResourceMetadataAccess::commit(
      result.lease_,
      result.region_.dimensions().capacity() * sizeof(RegionDimension));
  result.cancellation_ = cancellation;
  result.owners_ =
      ResourceVector<ResultRef>{ResourceAllocator<ResultRef>(budget)};
  result.source_leases_ =
      ResourceVector<ResourceLease>{ResourceAllocator<ResourceLease>(budget)};
  result.affine_ =
      ResourceVector<TensorSpan>{ResourceAllocator<TensorSpan>(budget)};
  result.owners_.reserve(owners);
  result.source_leases_.reserve(leases);
  result.affine_.reserve(affine);
  for (auto& window : windows) {
    if (window.owners_.empty())
      result.owners_.push_back(std::move(window.owner_));
    else
      for (auto& owner : window.owners_)
        result.owners_.push_back(std::move(owner));
    const auto region_bytes =
        window.region_.dimensions().capacity() * sizeof(RegionDimension);
    window.region_ = {};
    core_internal::ResourceMetadataAccess::withdraw(window.lease_,
                                                    region_bytes);
    result.source_leases_.push_back(std::move(window.lease_));
    for (auto& prior : window.source_leases_)
      result.source_leases_.push_back(std::move(prior));
    for (auto& value : window.affine_)
      result.affine_.push_back(std::move(value));
  }
  if (cancellation.cancelled())
    return Answer(Status{ErrorCode::Cancelled, {}});
  auto indexed = index(result);
  if (!indexed.ok())
    return Answer(indexed);
  return Answer(std::move(result));
} catch (const std::bad_alloc&) {
  return Result<ResultTensorReadWindow>(
      Status{ErrorCode::ResourceExhausted, {}});
} catch (...) {
  return Result<ResultTensorReadWindow>(Status{ErrorCode::OperationFailed, {}});
}
Result<std::uint64_t> execution_internal::ResultWindowAccess::read_work(
    const ResultTensorReadWindow& window) {
  if (!window.valid())
    return Result<std::uint64_t>(Status{ErrorCode::Stale, {}});
  const auto rank = window.region_.rank();
  auto affine = static_cast<std::uint64_t>(window.affine_.size());
  if (window.affine_canonical_ && affine > 1) {
    std::uint64_t levels = 1;
    for (auto n = affine; n; n >>= 1)
      levels += 2;
    affine = levels * rank;
  }
  const auto pieces = affine;
  const auto fixed = 4 * rank + 2;
  if (pieces == UINT64_MAX ||
      !core_internal::can_multiply_add(pieces + 1, rank + 1, fixed))
    return Result<std::uint64_t>(Status{ErrorCode::ResourceExhausted, {}});
  return Result<std::uint64_t>((pieces + 1) * (rank + 1) + fixed);
}
Result<bool> execution_internal::ResultWindowAccess::visit_backing_regions(
    const ResultTensorReadWindow& window, const ResourceBudget& budget,
    const std::function<Status(const Region&)>& visitor) {
  using Answer = Result<bool>;
  if (!window.valid())
    return Answer(Status{ErrorCode::Stale, {}});
  if (window.affine_.size() <= 1)
    return Answer(false);
  const auto visit = [&](const Region& region) {
    if (window.cancellation_.cancelled())
      return Status{ErrorCode::Cancelled, {}};
    auto charged = budget.consume({region.rank() + 1});
    return charged.ok() ? visitor(region) : charged;
  };
  for (const auto& part : window.affine_) {
    auto status = visit(part.region());
    if (!status.ok())
      return Answer(status);
  }
  return Answer(true);
}
Result<ResultTensorReadWindow>
execution_internal::ResultWindowAccess::replace_backing(
    ResultTensorReadWindow window, TensorSpan backing, ResourceLease metadata) {
  using Answer = Result<ResultTensorReadWindow>;
  if (!window.valid() || !backing.valid() ||
      backing.descriptor().element_type !=
          window.spec().descriptor.element_type ||
      backing.descriptor().shape.size() !=
          window.spec().batch_axes.size() +
              window.spec().descriptor.shape.size() ||
      backing.region().rank() != window.region().rank())
    return Answer(
        Status{ErrorCode::InvalidArgument, "native tensor backing mismatch"});
  for (std::size_t i = 0; i < backing.descriptor().shape.size(); ++i) {
    const auto expected =
        i < window.spec().batch_axes.size()
            ? window.spec().batch_axes[i]
            : window.spec()
                  .descriptor.shape[i - window.spec().batch_axes.size()];
    if (backing.descriptor().shape[i] != expected)
      return Answer(
          Status{ErrorCode::InvalidArgument, "native tensor shape mismatch"});
  }
  for (std::size_t i = 0; i < window.region().rank(); ++i)
    if (backing.region().dimensions()[i].offset !=
            window.region().dimensions()[i].offset ||
        backing.region().dimensions()[i].extent !=
            window.region().dimensions()[i].extent)
      return Answer(
          Status{ErrorCode::InvalidArgument, "native tensor region mismatch"});
  window.affine_.clear();
  window.affine_.push_back(std::move(backing));
  if (metadata.valid())
    window.source_leases_.push_back(std::move(metadata));
  return Answer(std::move(window));
}
Result<TensorSpan> execution_internal::ResultWindowAccess::affine(
    const ResultTensorReadWindow& window) {
  if (!window.valid())
    return Result<TensorSpan>(Status{ErrorCode::Stale, {}});
  if (window.cancellation_.cancelled())
    return Result<TensorSpan>(Status{ErrorCode::Cancelled, {}});
  if (window.affine_.size() != 1)
    return Result<TensorSpan>(Status{ErrorCode::NotFound, {}});
  ResourceAllocationScope scope(window.owner_.impl_->budget);
  return window.affine_[0].view(window.region_);
}
Result<std::optional<TensorSpan>>
execution_internal::ResultWindowAccess::input_view(
    const ResultTensorReadWindow& window, const FootprintLimits& limits) {
  using Answer = Result<std::optional<TensorSpan>>;
  if (!window.valid())
    return Answer(Status{ErrorCode::Stale, {}});
  if (window.cancellation_.cancelled() || limits.cancellation.cancelled())
    return Answer(Status{ErrorCode::Cancelled, {}});
  if (window.affine_.empty())
    return Answer(std::optional<TensorSpan>{});
  ResourceAllocationScope scope(window.owner_.impl_->budget);
  auto scratch =
      window.owner_.impl_->budget.reserve(ResourceCapacity::host(4096, 4096));
  if (!scratch.ok())
    return Answer(scratch.status());
  core_internal::MetadataAllocation allocation(scratch.value());
  const auto& spec = window.spec();
  TensorDescriptor descriptor;
  descriptor.element_type = spec.descriptor.element_type;
  descriptor.shape.reserve(spec.batch_axes.size() +
                           spec.descriptor.shape.size());
  allocation.commit(descriptor.shape.capacity() * sizeof(std::uint64_t));
  descriptor.shape.insert(descriptor.shape.end(), spec.batch_axes.begin(),
                          spec.batch_axes.end());
  descriptor.shape.insert(descriptor.shape.end(), spec.descriptor.shape.begin(),
                          spec.descriptor.shape.end());
  return input_internal::join_affine_view(
      descriptor, window.region_, window.affine_, limits, scratch.value());
}
namespace {
bool inside(const Region& region, const std::vector<std::uint64_t>& at) {
  if (region.rank() != at.size())
    return false;
  for (std::size_t i = 0; i < at.size(); ++i) {
    const auto d = region.dimensions()[i];
    if (at[i] < d.offset || at[i] - d.offset >= d.extent)
      return false;
  }
  return true;
}
}  // namespace
Status ResultTensorReadWindow::copy_to(std::uint8_t* destination,
                                       std::uint64_t bytes) const try {
  if (!valid() || !destination)
    return {ErrorCode::InvalidArgument, {}};
  auto count = region_.element_count();
  const auto width = TensorSpan::element_size(spec().descriptor.element_type);
  if (!count.ok() || !core_internal::can_multiply(count.value(), width) ||
      bytes != count.value() * width)
    return {ErrorCode::InvalidArgument, "tensor copy byte count mismatch"};
  const auto metadata_bytes = region_.rank() * sizeof(std::uint64_t);
  auto scratch = owner_.impl_->budget.reserve(
      ResourceCapacity::host(metadata_bytes, metadata_bytes));
  if (!scratch.ok())
    return scratch.status();
  core_internal::MetadataAllocation allocation(scratch.value());
  std::vector<std::uint64_t> at;
  at.reserve(region_.rank());
  allocation.commit(at.capacity() * sizeof(std::uint64_t));
  for (const auto& axis : region_.dimensions())
    at.push_back(axis.offset);
  for (std::uint64_t sample = 0; sample < count.value(); ++sample) {
    if (cancellation_.cancelled())
      return {ErrorCode::Cancelled, {}};
    auto charged = owner_.impl_->budget.consume({1});
    if (!charged.ok())
      return charged;
    auto row = row_run(at);
    if (!row.ok())
      return row.status();
    std::memcpy(destination + sample * width, row.value().data, width);
    for (std::size_t axis = at.size(); axis-- > 0;) {
      const auto d = region_.dimensions()[axis];
      if (++at[axis] < d.offset + d.extent)
        break;
      at[axis] = d.offset;
    }
  }
  return Status::success();
} catch (const std::bad_alloc&) {
  return {ErrorCode::ResourceExhausted, {}};
}
Result<ResultTensorRun> ResultTensorReadWindow::row_run(
    const std::vector<std::uint64_t>& at) const {
  if (cancellation_.cancelled())
    return Result<ResultTensorRun>(Status{ErrorCode::Cancelled, {}});
  if (!valid() || !inside(region_, at))
    return Result<ResultTensorRun>(Status{
        ErrorCode::InvalidArgument, "coordinate outside Result tensor window"});
  auto found = execution_internal::ResultWindowAccess::find(*this, at);
  if (!found.ok())
    return Result<ResultTensorRun>(found.status());
  const auto first = found.value();
  const auto last = first == affine_.size() ? first : first + 1;
  for (auto i = first; i < last; ++i) {
    const auto& value = affine_[i];
    if (!inside(value.region(), at))
      continue;
    auto address = value.byte_address(at);
    if (!address.ok())
      return Result<ResultTensorRun>(address.status());
    const auto axis = sample_axis();
    const auto d = value.region().dimensions()[axis];
    const auto samples = d.offset + d.extent - at[axis];
    const auto stride = value.layout().byte_strides[axis];
    const auto magnitude = stride < 0
                               ? static_cast<std::uint64_t>(-(stride + 1)) + 1
                               : static_cast<std::uint64_t>(stride);
    const auto width = TensorSpan::element_size(spec().descriptor.element_type);
    // The validated TensorSpan span guarantees this product, including
    // broadcasts.
    const auto bytes = (samples - 1) * magnitude + width;
    return Result<ResultTensorRun>(ResultTensorRun{
        value.bytes().data() + address.value(), samples, bytes, stride});
  }
  return Result<ResultTensorRun>(data_internal::unavailable());
}
Result<ResultTensorRectangle> ResultTensorReadWindow::rectangle_run(
    const std::vector<std::uint64_t>& at) const {
  auto row = row_run(at);
  if (!row.ok())
    return Result<ResultTensorRectangle>(row.status());
  auto found = execution_internal::ResultWindowAccess::find(*this, at);
  if (!found.ok())
    return Result<ResultTensorRectangle>(found.status());
  const auto first = found.value();
  const auto last = first == affine_.size() ? first : first + 1;
  for (auto i = first; i < last; ++i) {
    const auto& value = affine_[i];
    if (!inside(value.region(), at))
      continue;
    if (!row_axis())
      return Result<ResultTensorRectangle>(
          ResultTensorRectangle{row.take_value(), 1, 0});
    const auto axis = *row_axis();
    const auto d = value.region().dimensions()[axis];
    return Result<ResultTensorRectangle>(
        ResultTensorRectangle{row.take_value(), d.offset + d.extent - at[axis],
                              value.layout().byte_strides[axis]});
  }
  return Result<ResultTensorRectangle>(data_internal::unavailable());
}
Result<ResultTensorWriteWindow> ResultTensorWriteWindow::borrow_dense(
    const ResultTensorSpec& spec, TensorWriter& writer) {
  auto topology = data_internal::tensor_topology(spec);
  if (!topology.ok())
    return Result<ResultTensorWriteWindow>(topology);
  if (!writer.data() ||
      writer.descriptor().element_type != spec.descriptor.element_type ||
      writer.descriptor().shape != spec.sample_shape())
    return Result<ResultTensorWriteWindow>(
        Status{ErrorCode::TypeMismatch, "dense writer schema mismatch"});
  ResultTensorWriteWindow window;
  window.spec_ = &spec;
  window.region_ = writer.region();
  window.affine_data_ = writer.data();
  for (auto stride : writer.layout().byte_strides) {
    if (stride < 0)
      return Result<ResultTensorWriteWindow>(
          Status{ErrorCode::InvalidArgument, "invalid dense writer stride"});
    window.affine_strides_.push_back(static_cast<std::uint64_t>(stride));
  }
  return Result<ResultTensorWriteWindow>(std::move(window));
}
std::uint32_t ResultTensorWriteWindow::sample_axis() const {
  return spec().layout.spatial
             ? spec().batch_axes.size() + spec().layout.width_axis
             : region_.rank() - 1;
}
Result<ResultTensorMutableRun> ResultTensorWriteWindow::row_run(
    const std::vector<std::uint64_t>& at) const {
  using Answer = Result<ResultTensorMutableRun>;
  if (!valid() || !inside(region_, at))
    return Answer({ErrorCode::InvalidArgument,
                   "coordinate outside Result tensor writer"});
  const auto width = TensorSpan::element_size(spec().descriptor.element_type);

  std::uint64_t offset = 0;
  for (std::size_t axis = 0; axis < at.size(); ++axis)
    offset +=
        (at[axis] - region_.dimensions()[axis].offset) * affine_strides_[axis];
  const auto axis = sample_axis();
  const auto d = region_.dimensions()[axis];
  const auto count = d.offset + d.extent - at[axis];
  return Answer(ResultTensorMutableRun{
      affine_data_ + offset, count, (count - 1) * affine_strides_[axis] + width,
      static_cast<std::int64_t>(affine_strides_[axis])});
}
Result<ResultTensorMutableRectangle> ResultTensorWriteWindow::rectangle_run(
    const std::vector<std::uint64_t>& at) const {
  using Answer = Result<ResultTensorMutableRectangle>;
  auto row = row_run(at);
  if (!row.ok())
    return Answer(row.status());

  if (at.size() == 1)
    return Answer(ResultTensorMutableRectangle{row.take_value(), 1, 0});
  const auto axis = spec().layout.spatial
                        ? spec().batch_axes.size() + spec().layout.height_axis
                        : at.size() - 2;
  const auto d = region_.dimensions()[axis];
  return Answer(ResultTensorMutableRectangle{
      row.take_value(), d.offset + d.extent - at[axis],
      static_cast<std::int64_t>(affine_strides_[axis])});
}
const void* ResultTensorReadWindow::storage_owner_token() const noexcept {
  const void* root = nullptr;
  for (const auto& value : affine_) {
    const auto token = value.storage().get();
    if (root && root != token)
      return nullptr;
    root = token;
  }

  return root;
}
Result<ResultTensorReadWindow> ResultRef::acquire_tensor(
    const ResultDescriptor& facts, std::uint32_t slot, const Region& region,
    const CancellationToken& cancellation) const try {
  using Answer = Result<ResultTensorReadWindow>;
  if (!impl_ ||
      (captured_ && facts.revision() > captured_->descriptor.revision()))
    return Answer(Status{ErrorCode::Stale, {}});
  std::optional<ResourceAllocationScope> scope;
  if (!resource_internal::metadata_budget() ||
      !resource_internal::metadata_budget()->same_owner(impl_->budget))
    scope.emplace(impl_->budget);
  std::unique_lock<std::timed_mutex> lock(impl_->mutex, std::defer_lock);
  while (!lock.try_lock_for(std::chrono::milliseconds(2)))
    if (cancellation.cancelled())
      return Answer(Status{ErrorCode::Cancelled, {}});
  if (cancellation.cancelled())
    return Answer(Status{ErrorCode::Cancelled, {}});
  if (facts.object_ != impl_->object || !facts.revision_ ||
      facts.revision_ > impl_->revision || slot >= facts.tensor_count_ ||
      slot >= impl_->schema.tensors.size())
    return Answer(Status{ErrorCode::Stale, "invalid image descriptor"});
  const auto& spec = impl_->schema.tensors[slot];
  const data_internal::SampleDescriptor sample(spec);
  if (region.empty() || !region.validate(sample.value.shape).ok())
    return Answer(Status{ErrorCode::InvalidArgument,
                         "image window requires one frame/layer rectangle"});
  FootprintLimits limits;
  limits.cancellation = cancellation;
  limits.consume_work = [budget = impl_->budget](auto n) {
    return budget.consume({n});
  };
  auto requested = Footprint::from_region(sample.value.shape, region, limits);
  if (!requested.ok())
    return Answer(requested.status());
  auto missing = requested.value().subtract(facts.tensors_[slot], limits);
  if (!missing.ok())
    return Answer(missing.status());
  if (!missing.value().empty())
    return Answer(data_internal::unavailable());
  for (const auto& span : impl_->tensors[slot].device) {
    auto charged = impl_->budget.consume({region.rank() + 1});
    if (!charged.ok())
      return Answer(charged);
    if (cancellation.cancelled())
      return Answer(Status{ErrorCode::Cancelled, {}});
    if (data_internal::regions_overlap(region, span.region()))
      return Answer(
          Status{ErrorCode::BackendUnavailable,
                 "Device Result requires an explicit COPY to CPU storage"});
  }
  auto lease = impl_->budget.reserve(ResourceCapacity::host(
      sizeof(ResultTensorReadWindow) + region.rank() * sizeof(RegionDimension),
      sizeof(ResultTensorReadWindow) +
          region.rank() * sizeof(RegionDimension)));
  if (!lease.ok())
    return Answer(lease.status());
  ResultTensorReadWindow result;
  result.lease_ = lease.take_value();
  result.owner_ = *this;
  if (!captured_) {
    auto admitted = impl_->budget.reserve(
        ResourceCapacity::host(sizeof(Capture), sizeof(Capture)));
    if (!admitted.ok())
      return Answer(admitted.status());
    auto owned = std::make_unique<Capture>();
    owned->lease = admitted.take_value();
    core_internal::ResourceMetadataAccess::commit(owned->lease,
                                                  sizeof(Capture));
    auto capture = std::shared_ptr<Capture>(std::move(owned));
    capture->descriptor = facts;
    result.owner_.captured_ = std::move(capture);
  }
  result.slot_ = slot;
  result.region_ = region;
  core_internal::ResourceMetadataAccess::commit(
      result.lease_,
      result.region_.dimensions().capacity() * sizeof(RegionDimension));
  result.affine_ =
      ResourceVector<TensorSpan>(ResourceAllocator<TensorSpan>(impl_->budget));
  result.cancellation_ = cancellation;
  const auto append_view = [&](const TensorSpan& value,
                               const Region& part_region) {
    auto part = value.view(part_region);
    if (!part.ok())
      return part.status();
    result.affine_.push_back(part.take_value());
    return Status::success();
  };
  auto scratch = impl_->budget.reserve(
      ResourceCapacity::host(region.rank() * sizeof(RegionDimension),
                             region.rank() * sizeof(RegionDimension)));
  if (!scratch.ok())
    return Answer(scratch.status());
  const auto& backing = impl_->tensors[slot];
  if (backing.affine_canonical) {
    std::array<std::uint64_t, 8> at{};
    for (std::size_t axis = 0; axis < region.rank(); ++axis)
      at[axis] = region.dimensions()[axis].offset;
    std::uint64_t levels = 1;
    for (auto n = backing.affine.size(); n; n >>= 1)
      levels += 2;
    auto charged = impl_->budget.consume({levels * region.rank()});
    if (!charged.ok())
      return Answer(charged);
    const auto index = footprint_internal::containing(
        backing.affine.size(), region.rank(), at.data(),
        [&](auto i) -> const Region& { return backing.affine[i].region(); },
        [] {});
    if (index != backing.affine.size()) {
      const auto& value = backing.affine[index];
      bool contained = true;
      for (std::size_t axis = 0; axis < region.rank(); ++axis) {
        const auto a = region.dimensions()[axis],
                   b = value.region().dimensions()[axis];
        contained &= a.extent <= b.extent - (a.offset - b.offset);
      }
      if (contained) {
        auto appended = append_view(value, region);
        if (!appended.ok())
          return Answer(appended);
        result.affine_canonical_ = true;
        return Answer(std::move(result));
      }
    }
    // Distinct canonical values are disjoint. The authorization check already
    // proved this entire request covered, so clip each backing once instead of
    // repeatedly subtracting its prefix from a growing set of remaining boxes.
    charged =
        impl_->budget.consume({backing.affine.size() * (region.rank() + 1)});
    if (!charged.ok())
      return Answer(charged);
    for (const auto& value : backing.affine) {
      if (cancellation.cancelled())
        return Answer(Status{ErrorCode::Cancelled, {}});
      core_internal::MetadataAllocation allocation(scratch.value());
      auto dims = region.dimensions();
      allocation.commit(dims.capacity() * sizeof(RegionDimension));
      bool overlaps = true;
      for (std::size_t axis = 0; axis < dims.size(); ++axis) {
        const auto other = value.region().dimensions()[axis];
        const auto first = std::max(dims[axis].offset, other.offset);
        const auto last = std::min(dims[axis].offset + dims[axis].extent,
                                   other.offset + other.extent);
        if (first >= last) {
          overlaps = false;
          break;
        }
        dims[axis] = {first, last - first};
      }
      if (!overlaps)
        continue;
      auto appended = append_view(value, Region(std::move(dims)));
      if (!appended.ok())
        return Answer(appended);
    }
    result.affine_canonical_ = true;
    return Answer(std::move(result));
  }
  auto remaining = requested.take_value();
  if (!impl_->tensors[slot].affine.empty()) {
    for (const auto& value : impl_->tensors[slot].affine) {
      if (remaining.empty())
        break;
      if (cancellation.cancelled())
        return Answer(Status{ErrorCode::Cancelled, {}});
      auto inspected = impl_->budget.consume({region.rank() + 1});
      if (!inspected.ok())
        return Answer(inspected);
      bool overlaps = true;
      for (std::size_t axis = 0; axis < region.rank(); ++axis) {
        const auto a = region.dimensions()[axis];
        const auto b = value.region().dimensions()[axis];
        if (a.offset >= b.offset + b.extent ||
            b.offset >= a.offset + a.extent) {
          overlaps = false;
          break;
        }
      }
      if (!overlaps)
        continue;

      auto available =
          Footprint::from_region(sample.value.shape, value.region(), limits);
      if (!available.ok())
        return Answer(available.status());
      auto covered = remaining.intersect(available.value(), limits);
      if (!covered.ok())
        return Answer(covered.status());
      for (const auto& box : covered.value().boxes()) {
        auto appended = append_view(value, box);
        if (!appended.ok())
          return Answer(appended);
      }
      auto next = remaining.subtract(covered.value(), limits);
      if (!next.ok())
        return Answer(next.status());
      remaining = next.take_value();
    }
    if (remaining.empty()) {
      auto indexed = execution_internal::ResultWindowAccess::index(result);
      if (!indexed.ok())
        return Answer(indexed);
      return Answer(std::move(result));
    }
  }
  return Answer(data_internal::unavailable());
} catch (const Status& status) {
  return Result<ResultTensorReadWindow>(status);
} catch (const std::bad_alloc&) {
  return Result<ResultTensorReadWindow>(
      Status{ErrorCode::ResourceExhausted,
             {},
             FailureReason::CapacityLimit,
             {FailureOrigin::Resource, FailureScope::Group}});
}
Status ResultRef::read_tensor(const ResultDescriptor& facts, std::uint32_t slot,
                              const std::vector<std::uint64_t>& at,
                              void* destination, std::size_t bytes,
                              const CancellationToken& cancellation) const try {
  if (!impl_)
    return Status{ErrorCode::Stale, {}};
  if (!destination || slot >= schema().tensors.size() ||
      bytes != TensorSpan::element_size(
                   schema().tensors[slot].descriptor.element_type))
    return Status{ErrorCode::InvalidArgument, "invalid image read"};
  if (slot >= facts.tensor_count() || !facts.tensor_coverage(slot).contains(at))
    return Status{ErrorCode::InvalidArgument, "unauthorized image sample"};
  const auto metadata_bytes = at.size() * sizeof(RegionDimension);
  auto scratch = impl_->budget.reserve(
      ResourceCapacity::host(metadata_bytes, metadata_bytes));
  if (!scratch.ok())
    return scratch.status();
  core_internal::MetadataAllocation allocation(scratch.value());
  std::vector<RegionDimension> dimensions;
  dimensions.reserve(at.size());
  allocation.commit(dimensions.capacity() * sizeof(RegionDimension));
  for (auto n : at)
    dimensions.push_back({n, 1});
  const Region region(std::move(dimensions));
  auto window = acquire_tensor(facts, slot, region, cancellation);
  if (!window.ok())
    return window.status();
  auto work = impl_->budget.consume({1});
  if (!work.ok())
    return work;
  auto run = window.value().row_run(at);
  if (!run.ok())
    return run.status();
  std::memcpy(destination, run.value().data, bytes);
  return Status::success();
} catch (const std::bad_alloc&) {
  return {ErrorCode::ResourceExhausted, {}};
}
}  // namespace ps
