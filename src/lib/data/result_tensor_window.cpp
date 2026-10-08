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
  const auto shape = first.spec().sample_shape();
  FootprintLimits limits;
  limits.cancellation = cancellation;
  limits.consume_work = [budget](auto n) { return budget.consume({n}); };
  auto requested = Footprint::from_regions(shape, {region}, limits);
  auto covered = Footprint::none(shape, limits);
  if (!requested.ok() || !covered.ok())
    return Answer(requested.ok() ? covered.status() : requested.status());
  std::size_t affine = 0, planar = 0, owners = 0, leases = 0;
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
    auto part = Footprint::from_regions(shape, {window.region_}, limits);
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
    planar += window.pieces_.size();
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
  result.cancellation_ = cancellation;
  result.owners_ =
      ResourceVector<ResultRef>{ResourceAllocator<ResultRef>(budget)};
  result.source_leases_ =
      ResourceVector<ResourceLease>{ResourceAllocator<ResourceLease>(budget)};
  result.affine_ = ResourceVector<Value>{ResourceAllocator<Value>(budget)};
  result.pieces_ = ResourceVector<PlanarImageReadWindow>{
      ResourceAllocator<PlanarImageReadWindow>(budget)};
  result.piece_batches_ = ResourceVector<std::array<std::uint64_t, 8>>{
      ResourceAllocator<std::array<std::uint64_t, 8>>(budget)};
  result.piece_order_ =
      ResourceVector<std::size_t>{ResourceAllocator<std::size_t>(budget)};
  result.owners_.reserve(owners);
  result.source_leases_.reserve(leases);
  result.affine_.reserve(affine);
  result.pieces_.reserve(planar);
  result.piece_batches_.reserve(planar);
  result.piece_order_.reserve(planar);
  for (auto& window : windows) {
    if (window.owners_.empty())
      result.owners_.push_back(std::move(window.owner_));
    else
      for (auto& owner : window.owners_)
        result.owners_.push_back(std::move(owner));
    result.source_leases_.push_back(std::move(window.lease_));
    for (auto& prior : window.source_leases_)
      result.source_leases_.push_back(std::move(prior));
    for (auto& value : window.affine_)
      result.affine_.push_back(std::move(value));
    for (std::size_t i = 0; i < window.pieces_.size(); ++i) {
      result.pieces_.push_back(std::move(window.pieces_[i]));
      result.piece_batches_.push_back(window.piece_batches_[i]);
      result.piece_order_.push_back(result.piece_order_.size());
    }
  }
  std::uint64_t rounds = 1;
  for (auto n = planar; n > 1; n >>= 1)
    ++rounds;
  if (planar > UINT64_MAX / (rounds * (1 + result.spec().batch_axes.size())))
    return Answer(Status{ErrorCode::ResourceExhausted, {}});
  auto charged =
      budget.consume({planar * rounds * (1 + result.spec().batch_axes.size())});
  if (!charged.ok())
    return Answer(charged);
  std::sort(
      result.piece_order_.begin(), result.piece_order_.end(),
      [&](std::size_t a, std::size_t b) {
        for (std::size_t axis = 0; axis < result.spec().batch_axes.size();
             ++axis)
          if (result.piece_batches_[a][axis] != result.piece_batches_[b][axis])
            return result.piece_batches_[a][axis] <
                   result.piece_batches_[b][axis];
        return a < b;
      });
  if (cancellation.cancelled())
    return Answer(Status{ErrorCode::Cancelled, {}});
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
  const auto pieces =
      static_cast<std::uint64_t>(window.affine_.size()) + window.pieces_.size();
  const auto fixed = 4 * rank + 2;
  if (pieces == UINT64_MAX || pieces + 1 > (UINT64_MAX - fixed) / (rank + 1))
    return Result<std::uint64_t>(Status{ErrorCode::ResourceExhausted, {}});
  return Result<std::uint64_t>((pieces + 1) * (rank + 1) + fixed);
}
Result<bool> execution_internal::ResultWindowAccess::visit_backing_regions(
    const ResultTensorReadWindow& window, const ResourceBudget& budget,
    const std::function<Status(const Region&)>& visitor) {
  using Answer = Result<bool>;
  if (!window.valid())
    return Answer(Status{ErrorCode::Stale, {}});
  if (window.affine_.size() + window.pieces_.size() <= 1)
    return Answer(false);
  auto scratch = budget.reserve(ResourceCapacity::host(512, 512));
  if (!scratch.ok())
    return Answer(scratch.status());
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
  for (std::size_t i = 0; i < window.pieces_.size(); ++i) {
    std::vector<RegionDimension> dims;
    for (std::size_t axis = 0; axis < window.spec().batch_axes.size(); ++axis)
      dims.push_back({window.piece_batches_[i][axis], 1});
    const auto& cell = window.pieces_[i].region().dimensions();
    dims.insert(dims.end(), cell.begin(), cell.end());
    auto status = visit(Region(std::move(dims)));
    if (!status.ok())
      return Answer(status);
  }
  return Answer(true);
}
Result<ResultTensorReadWindow>
execution_internal::ResultWindowAccess::replace_backing(
    ResultTensorReadWindow window, Value backing, ResourceLease metadata) {
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
  window.pieces_.clear();
  window.piece_batches_.clear();
  window.piece_order_.clear();
  return Answer(std::move(window));
}
Result<Value> execution_internal::ResultWindowAccess::affine(
    const ResultTensorReadWindow& window) {
  if (!window.valid())
    return Result<Value>(Status{ErrorCode::Stale, {}});
  if (window.cancellation_.cancelled())
    return Result<Value>(Status{ErrorCode::Cancelled, {}});
  if (window.affine_.size() != 1 || !window.pieces_.empty())
    return Result<Value>(Status{ErrorCode::NotFound, {}});
  return window.affine_[0].view(window.region_);
}
Result<std::optional<Value>> execution_internal::ResultWindowAccess::input_view(
    const ResultTensorReadWindow& window, const FootprintLimits& limits) {
  using Answer = Result<std::optional<Value>>;
  if (!window.valid())
    return Answer(Status{ErrorCode::Stale, {}});
  if (window.cancellation_.cancelled() || limits.cancellation.cancelled())
    return Answer(Status{ErrorCode::Cancelled, {}});
  if (!window.pieces_.empty() || window.affine_.empty())
    return Answer(std::optional<Value>{});
  return input_internal::join_affine_view(
      {window.spec().descriptor.element_type, window.spec().sample_shape()},
      window.region_, window.affine_, limits);
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
Result<std::size_t> ResultTensorReadWindow::find_piece(
    const std::vector<std::uint64_t>& at) const {
  using Answer = Result<std::size_t>;
  const auto compare = [&](std::size_t index) -> Result<int> {
    if (cancellation_.cancelled())
      return Result<int>(Status{ErrorCode::Cancelled, {}});
    for (std::size_t axis = 0; axis < spec().batch_axes.size(); ++axis) {
      auto charged = owner_.impl_->budget.consume({1});
      if (!charged.ok())
        return Result<int>(charged);
      if (piece_batches_[index][axis] != at[axis])
        return Result<int>(piece_batches_[index][axis] < at[axis] ? -1 : 1);
    }
    return Result<int>(0);
  };
  std::size_t low = 0, high = piece_order_.size();
  while (low < high) {
    const auto middle = low + (high - low) / 2;
    auto order = compare(piece_order_[middle]);
    if (!order.ok())
      return Answer(order.status());
    if (order.value() < 0)
      low = middle + 1;
    else
      high = middle;
  }
  for (; low < piece_order_.size(); ++low) {
    const auto index = piece_order_[low];
    auto order = compare(index);
    if (!order.ok())
      return Answer(order.status());
    if (order.value() != 0)
      break;
    auto charged =
        owner_.impl_->budget.consume({pieces_[index].region().rank() + 1});
    if (!charged.ok())
      return Answer(charged);
    bool covered = true;
    for (std::size_t axis = 0; axis < pieces_[index].region().rank(); ++axis) {
      const auto d = pieces_[index].region().dimensions()[axis];
      const auto coordinate = at[spec().batch_axes.size() + axis];
      covered =
          covered && coordinate >= d.offset && coordinate - d.offset < d.extent;
    }
    if (covered)
      return Answer(index);
  }
  return Answer(data_internal::unavailable());
}
Result<ResultTensorRun> ResultTensorReadWindow::row_run(
    const std::vector<std::uint64_t>& at) const {
  if (cancellation_.cancelled())
    return Result<ResultTensorRun>(Status{ErrorCode::Cancelled, {}});
  if (!valid() || !inside(region_, at))
    return Result<ResultTensorRun>(Status{
        ErrorCode::InvalidArgument, "coordinate outside Result tensor window"});
  if (owners_.size() > 1) {
    auto charged = owner_.impl_->budget.consume(
        {(affine_.size() + 1) * (region_.rank() + 1)});
    if (!charged.ok())
      return Result<ResultTensorRun>(charged);
  }
  for (const auto& value : affine_) {
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
    const auto width = Value::element_size(spec().descriptor.element_type);
    // The validated Value span guarantees this product, including broadcasts.
    const auto bytes = (samples - 1) * magnitude + width;
    auto observed =
        data_internal::ResultHostAccessScope::observe(value.storage());
    if (!observed.ok())
      return Result<ResultTensorRun>(observed);
    return Result<ResultTensorRun>(ResultTensorRun{
        value.bytes().data() + address.value(), samples, bytes, stride});
  }
  std::vector<std::uint64_t> spatial(at.begin() + spec().batch_axes.size(),
                                     at.end());
  auto selected = find_piece(at);
  if (!selected.ok())
    return Result<ResultTensorRun>(selected.status());
  {
    const auto& piece = pieces_[selected.value()];
    auto run = piece.row_run(spatial);
    if (!run.ok())
      return Result<ResultTensorRun>(run.status());
    return Result<ResultTensorRun>(ResultTensorRun{
        run.value().data, run.value().samples, run.value().bytes,
        static_cast<std::int64_t>(
            Value::element_size(spec().descriptor.element_type))});
  }
  return Result<ResultTensorRun>(data_internal::unavailable());
}
Result<ResultTensorRectangle> ResultTensorReadWindow::rectangle_run(
    const std::vector<std::uint64_t>& at) const {
  auto row = row_run(at);
  if (!row.ok())
    return Result<ResultTensorRectangle>(row.status());
  if (owners_.size() > 1) {
    auto charged = owner_.impl_->budget.consume(
        {(affine_.size() + 1) * (region_.rank() + 1)});
    if (!charged.ok())
      return Result<ResultTensorRectangle>(charged);
  }
  for (const auto& value : affine_) {
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
  std::vector<std::uint64_t> spatial(at.begin() + spec().batch_axes.size(),
                                     at.end());
  auto selected = find_piece(at);
  if (!selected.ok())
    return Result<ResultTensorRectangle>(selected.status());
  {
    const auto& piece = pieces_[selected.value()];
    auto run = piece.rectangle_run(spatial);
    if (!run.ok())
      return Result<ResultTensorRectangle>(run.status());
    return Result<ResultTensorRectangle>(ResultTensorRectangle{
        row.take_value(), run.value().rows,
        static_cast<std::int64_t>(run.value().row_stride_bytes)});
  }
  return Result<ResultTensorRectangle>(data_internal::unavailable());
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
  const auto width = Value::element_size(spec().descriptor.element_type);
  if (planar_) {
    auto run = planar_->row_run(std::vector<std::uint64_t>(
        at.begin() + spec().batch_axes.size(), at.end()));
    return run.ok() ? Answer(ResultTensorMutableRun{
                          run.value().data, run.value().samples,
                          run.value().bytes, static_cast<std::int64_t>(width)})
                    : Answer(run.status());
  }
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
  if (planar_) {
    auto rectangle = planar_->rectangle_run(std::vector<std::uint64_t>(
        at.begin() + spec().batch_axes.size(), at.end()));
    return rectangle.ok() ? Answer(ResultTensorMutableRectangle{
                                row.take_value(), rectangle.value().rows,
                                static_cast<std::int64_t>(
                                    rectangle.value().row_stride_bytes)})
                          : Answer(rectangle.status());
  }
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
  for (const auto& piece : pieces_) {
    const auto token = piece.image_->owner_token();
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
  if (region.empty() || !region.validate(spec.sample_shape()).ok())
    return Answer(Status{ErrorCode::InvalidArgument,
                         "image window requires one frame/layer rectangle"});
  FootprintLimits limits;
  limits.cancellation = cancellation;
  limits.consume_work = [budget = impl_->budget](auto n) {
    return budget.consume({n});
  };
  auto requested =
      Footprint::from_regions(spec.sample_shape(), {region}, limits);
  if (!requested.ok())
    return Answer(requested.status());
  auto missing = requested.value().subtract(facts.tensors_[slot], limits);
  if (!missing.ok())
    return Answer(missing.status());
  if (!missing.value().empty())
    return Answer(data_internal::unavailable());
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
    auto capture = std::make_shared<Capture>();
    capture->lease = admitted.take_value();
    capture->descriptor = facts;
    for (std::size_t i = 0; i < facts.field_count(); ++i)
      capture->fields[i] = impl_->fields[i].relation;
    for (std::size_t i = 0; i < facts.tensor_count(); ++i)
      capture->tensors[i] = impl_->tensors[i].relation;
    capture->basis = impl_->descriptor_relation;
    capture->dependencies = impl_->dependencies;
    result.owner_.captured_ = std::move(capture);
  }
  result.slot_ = slot;
  result.region_ = region;
  result.pieces_ = ResourceVector<PlanarImageReadWindow>(
      ResourceAllocator<PlanarImageReadWindow>(impl_->budget));
  result.affine_ =
      ResourceVector<Value>(ResourceAllocator<Value>(impl_->budget));
  result.piece_batches_ = ResourceVector<std::array<std::uint64_t, 8>>(
      ResourceAllocator<std::array<std::uint64_t, 8>>(impl_->budget));
  result.piece_order_ = ResourceVector<std::size_t>(
      ResourceAllocator<std::size_t>(impl_->budget));
  result.cancellation_ = cancellation;
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

      auto available = Footprint::from_regions(spec.sample_shape(),
                                               {value.region()}, limits);
      if (!available.ok())
        return Answer(available.status());
      auto covered = remaining.intersect(available.value(), limits);
      if (!covered.ok())
        return Answer(covered.status());
      for (const auto& box : covered.value().boxes()) {
        auto part = value.view(box);
        if (!part.ok())
          return Answer(part.status());
        result.affine_.push_back(part.take_value());
      }
      auto next = remaining.subtract(covered.value(), limits);
      if (!next.ok())
        return Answer(next.status());
      remaining = next.take_value();
    }
    if (remaining.empty())
      return Answer(std::move(result));
  }
  if (!spec.layout.spatial)
    return Answer(data_internal::unavailable());
  ResourceVector<data_internal::ResultSpatialBacking> backings(
      ResourceAllocator<data_internal::ResultSpatialBacking>(impl_->budget));
  for (const auto& entry : impl_->tensors[slot].backing) {
    if (cancellation.cancelled())
      return Answer(Status{ErrorCode::Cancelled, {}});
    auto inspected = impl_->budget.consume({spec.batch_axes.size() + 1});
    if (!inspected.ok())
      return Answer(inspected);
    bool needed = true;
    for (std::size_t axis = 0; axis < spec.batch_axes.size(); ++axis) {
      const auto d = region.dimensions()[axis];
      needed = needed && entry.batch[axis] >= d.offset &&
               entry.batch[axis] - d.offset < d.extent;
    }
    if (needed)
      backings.push_back(entry);
  }
  ResourceVector<std::pair<Region, PlanarImage>> views(
      ResourceAllocator<std::pair<Region, PlanarImage>>(impl_->budget));
  auto region_lease = impl_->budget.reserve(ResourceCapacity::host(
      impl_->tensors[slot].views.size() * spec.sample_shape().size() *
          sizeof(RegionDimension),
      impl_->tensors[slot].views.size() * spec.sample_shape().size() *
          sizeof(RegionDimension)));
  if (!region_lease.ok())
    return Answer(region_lease.status());
  for (const auto& view : impl_->tensors[slot].views) {
    if (cancellation.cancelled())
      return Answer(Status{ErrorCode::Cancelled, {}});
    auto inspected = impl_->budget.consume({view.region.rank() + 1});
    if (!inspected.ok())
      return Answer(inspected);
    views.emplace_back(view.region, view.backing);
  }
  lock.unlock();
  auto scratch = impl_->budget.reserve(ResourceCapacity::host(512, 512));
  if (!scratch.ok())
    return Answer(scratch.status());
  const auto compare_batch = [&](const auto& left, const auto& right) {
    for (std::size_t axis = 0; axis < spec.batch_axes.size(); ++axis) {
      if (cancellation.cancelled())
        throw Status{ErrorCode::Cancelled, {}};
      auto charged = impl_->budget.consume({1});
      if (!charged.ok())
        throw charged;
      if (left[axis] != right[axis])
        return left[axis] < right[axis] ? -1 : 1;
    }
    return 0;
  };
  std::sort(backings.begin(), backings.end(),
            [&](const auto& a, const auto& b) {
              return compare_batch(a.batch, b.batch) < 0;
            });
  const auto append = [&](const Region& box,
                          const PlanarImage* fixed) -> Status {
    auto dimensions = box.dimensions();
    const auto batches = spec.batch_axes.size();
    for (std::size_t axis = 0; axis < batches; ++axis)
      dimensions[axis].extent = 1;
    for (;;) {
      auto charged = impl_->budget.consume({1});
      if (!charged.ok())
        return charged;
      std::array<std::uint64_t, 8> key{};
      for (std::size_t axis = 0; axis < batches; ++axis)
        key[axis] = dimensions[axis].offset;
      const PlanarImage* backing = fixed;
      if (!backing) {
        auto entry =
            std::lower_bound(backings.begin(), backings.end(), key,
                             [&](const auto& value, const auto& at) {
                               return compare_batch(value.batch, at) < 0;
                             });
        if (entry != backings.end() && compare_batch(entry->batch, key) == 0)
          backing = &entry->image;
      }
      if (!backing)
        return data_internal::unavailable();
      auto window = backing->certified_window(
          Region(std::vector<RegionDimension>(dimensions.begin() + batches,
                                              dimensions.end())),
          cancellation);
      if (!window.ok())
        return window.status();
      result.pieces_.push_back(window.take_value());
      result.piece_batches_.push_back(key);
      bool advanced = false;
      for (std::size_t axis = batches; axis-- > 0;) {
        if (++dimensions[axis].offset <
            box.dimensions()[axis].offset + box.dimensions()[axis].extent) {
          advanced = true;
          break;
        }
        dimensions[axis].offset = box.dimensions()[axis].offset;
      }
      if (!advanced)
        return Status::success();
    }
  };
  for (const auto& view : views) {
    auto mapped =
        Footprint::from_regions(spec.sample_shape(), {view.first}, limits);
    if (!mapped.ok())
      return Answer(mapped.status());
    auto covered = remaining.intersect(mapped.value(), limits);
    if (!covered.ok())
      return Answer(covered.status());
    for (const auto& box : covered.value().boxes()) {
      auto status = append(box, &view.second);
      if (!status.ok())
        return Answer(status);
    }
    auto next = remaining.subtract(mapped.value(), limits);
    if (!next.ok())
      return Answer(next.status());
    remaining = next.take_value();
  }
  for (const auto& box : remaining.boxes()) {
    auto status = append(box, nullptr);
    if (!status.ok())
      return Answer(status);
  }
  result.piece_order_.reserve(result.pieces_.size());
  for (std::size_t i = 0; i < result.pieces_.size(); ++i)
    result.piece_order_.push_back(i);
  std::sort(result.piece_order_.begin(), result.piece_order_.end(),
            [&](auto left, auto right) {
              return compare_batch(result.piece_batches_[left],
                                   result.piece_batches_[right]) < 0;
            });
  return Answer(std::move(result));
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
                              const CancellationToken& cancellation) const {
  if (!impl_)
    return Status{ErrorCode::Stale, {}};
  if (!destination || slot >= schema().tensors.size() ||
      bytes !=
          Value::element_size(schema().tensors[slot].descriptor.element_type))
    return Status{ErrorCode::InvalidArgument, "invalid image read"};
  if (slot >= facts.tensor_count() || !facts.tensor_coverage(slot).contains(at))
    return Status{ErrorCode::InvalidArgument, "unauthorized image sample"};
  std::vector<RegionDimension> dimensions;
  for (auto n : at)
    dimensions.push_back({n, 1});
  auto window =
      acquire_tensor(facts, slot, Region(std::move(dimensions)), cancellation);
  if (!window.ok())
    return window.status();
  auto work = impl_->budget.consume({1, bytes, 1});
  if (!work.ok())
    return work;
  auto run = window.value().row_run(at);
  if (!run.ok())
    return run.status();
  std::memcpy(destination, run.value().data, bytes);
  return Status::success();
}
Result<ResultRelation> ResultRef::tensor_relation(std::uint32_t slot) const {
  if (captured_)
    return slot < captured_->descriptor.tensor_count()
               ? Result<ResultRelation>(captured_->tensors[slot])
               : Result<ResultRelation>(data_internal::unavailable());
  if (!impl_)
    return Result<ResultRelation>(Status{ErrorCode::Stale, {}});
  std::lock_guard<std::timed_mutex> lock(impl_->mutex);
  if (slot >= impl_->schema.tensors.size() ||
      !impl_->tensors[slot].relation.valid() ||
      (!impl_->complete &&
       impl_->schema.publication == PublishPolicy::CompleteBundle))
    return Result<ResultRelation>(data_internal::unavailable());
  return Result<ResultRelation>(impl_->tensors[slot].relation);
}
}  // namespace ps
