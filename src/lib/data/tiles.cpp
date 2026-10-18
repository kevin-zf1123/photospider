#include "photospider/data/tiles.hpp"

#include <algorithm>
#include <iterator>
#include <memory>
#include <new>
#include <utility>
#include <vector>

#include "photospider/core/checked_math.hpp"
#include "photospider/data/result.hpp"

namespace ps {
struct TileSet::Storage {
  explicit Storage(const ResourceBudget& root)
      : intervals(ResourceAllocator<TileInterval>(root)),
        offsets(ResourceAllocator<std::uint64_t>(root)) {}
  ResourceVector<TileInterval> intervals;
  ResourceVector<std::uint64_t> offsets;
  std::uint64_t count = 0;
};
bool TileSet::owned_by(const ResourceBudget& root) const noexcept {
  return !storage_ || storage_->intervals.get_allocator().owned_by(root);
}
const ResourceVector<TileInterval>& TileSet::intervals() const noexcept {
  static const ResourceVector<TileInterval> empty;
  return storage_ ? storage_->intervals : empty;
}
Result<TileSet> TileSet::range(const ResourceBudget& root, TileIndex begin,
                               TileIndex end) {
  const TileInterval interval{begin, end};
  return from_intervals(root, &interval, 1);
}
Result<TileSet> TileSet::from_intervals(const ResourceBudget& root,
                                        const TileInterval* intervals,
                                        std::size_t count) try {
  using Answer = Result<TileSet>;
  if (!intervals && count)
    return Answer(Status{ErrorCode::InvalidArgument, "null tile intervals"});
  std::uint64_t passes = 3;
  for (auto n = count; n > 1; n /= 2)
    ++passes;
  if (!core_internal::can_multiply(count, passes))
    return Answer(Status{ErrorCode::ResourceExhausted, {}});
  auto charged = root.consume({count * passes});
  if (!charged.ok())
    return Answer(charged);
  if (!count)
    return Answer(TileSet{});
  auto storage =
      std::allocate_shared<Storage>(ResourceAllocator<Storage>(root), root);
  storage->intervals.assign(intervals, intervals + count);
  for (const auto range : storage->intervals)
    if (range.end < range.begin)
      return Answer(
          Status{ErrorCode::InvalidArgument, "reversed tile interval"});
  std::sort(storage->intervals.begin(), storage->intervals.end(),
            [](auto a, auto b) {
              return a.begin < b.begin || (a.begin == b.begin && a.end < b.end);
            });
  std::size_t written = 0;
  for (const auto range : storage->intervals) {
    if (range.begin == range.end)
      continue;
    if (written && storage->intervals[written - 1].end >= range.begin)
      storage->intervals[written - 1].end =
          std::max(storage->intervals[written - 1].end, range.end);
    else
      storage->intervals[written++] = range;
  }
  storage->intervals.resize(written);
  if (!written)
    return Answer(TileSet{});
  storage->offsets.reserve(written);
  for (const auto range : storage->intervals) {
    storage->offsets.push_back(storage->count);
    storage->count += range.end - range.begin;
  }
  TileSet result;
  result.storage_ = std::move(storage);
  return Answer(std::move(result));
} catch (const std::bad_alloc&) {
  return Result<TileSet>(Status{ErrorCode::ResourceExhausted, {}});
}
Result<TileSet> TileSet::from_indices(const ResourceBudget& root,
                                      const std::vector<TileIndex>& indices) {
  return from_indices(root, indices.data(), indices.size());
}
Result<TileSet> TileSet::from_indices(const ResourceBudget& root,
                                      const TileIndex* indices,
                                      std::size_t count) try {
  if (!indices && count)
    return Result<TileSet>(
        Status{ErrorCode::InvalidArgument, "null tile indices"});
  auto charged = root.consume({count});
  if (!charged.ok())
    return Result<TileSet>(charged);
  ResourceVector<TileInterval> ranges{ResourceAllocator<TileInterval>(root)};
  ranges.reserve(count);
  for (std::size_t i = 0; i < count; ++i) {
    if (indices[i] == UINT64_MAX)
      return Result<TileSet>(
          Status{ErrorCode::InvalidArgument, "tile index overflow"});
    ranges.push_back({indices[i], indices[i] + 1});
  }
  return from_intervals(root, ranges.data(), ranges.size());
} catch (const std::bad_alloc&) {
  return Result<TileSet>(Status{ErrorCode::ResourceExhausted, {}});
}
Result<TileSet> TileSet::unite(const TileSet& other,
                               const ResourceBudget& root) const try {
  const auto& a = intervals();
  const auto& b = other.intervals();
  if (!core_internal::can_add(a.size(), b.size()))
    return Result<TileSet>(Status{ErrorCode::ResourceExhausted, {}});
  auto charged = root.consume({a.size() + b.size()});
  if (!charged.ok())
    return Result<TileSet>(charged);
  ResourceVector<TileInterval> ranges{ResourceAllocator<TileInterval>(root)};
  ranges.reserve(a.size() + b.size());
  ranges.insert(ranges.end(), a.begin(), a.end());
  ranges.insert(ranges.end(), b.begin(), b.end());
  return from_intervals(root, ranges.data(), ranges.size());
} catch (const std::bad_alloc&) {
  return Result<TileSet>(Status{ErrorCode::ResourceExhausted, {}});
}
Result<TileSet> TileSet::intersect(const TileSet& other,
                                   const ResourceBudget& root) const try {
  auto work = root.consume({intervals().size() + other.intervals().size() + 1});
  if (!work.ok())
    return Result<TileSet>(work);
  ResourceVector<TileInterval> result{ResourceAllocator<TileInterval>(root)};
  std::size_t i = 0, j = 0;
  while (i < intervals().size() && j < other.intervals().size()) {
    const auto a = intervals()[i], b = other.intervals()[j];
    const auto begin = std::max(a.begin, b.begin), end = std::min(a.end, b.end);
    if (begin < end)
      result.push_back({begin, end});
    if (a.end < b.end)
      ++i;
    else
      ++j;
  }
  return from_intervals(root, result.data(), result.size());
} catch (const std::bad_alloc&) {
  return Result<TileSet>(Status{ErrorCode::ResourceExhausted, {}});
}
Result<TileSet> TileSet::subtract(const TileSet& other,
                                  const ResourceBudget& root) const try {
  auto work = root.consume({intervals().size() + other.intervals().size() + 1});
  if (!work.ok())
    return Result<TileSet>(work);
  ResourceVector<TileInterval> result{ResourceAllocator<TileInterval>(root)};
  std::size_t j = 0;
  for (auto range : intervals()) {
    while (j < other.intervals().size() &&
           other.intervals()[j].end <= range.begin)
      ++j;
    auto k = j;
    while (k < other.intervals().size() &&
           other.intervals()[k].begin < range.end) {
      const auto removed = other.intervals()[k++];
      if (removed.begin > range.begin)
        result.push_back({range.begin, removed.begin});
      range.begin = std::max(range.begin, removed.end);
      if (range.begin >= range.end)
        break;
    }
    if (range.begin < range.end)
      result.push_back(range);
  }
  return from_intervals(root, result.data(), result.size());
} catch (const std::bad_alloc&) {
  return Result<TileSet>(Status{ErrorCode::ResourceExhausted, {}});
}
Result<TileSet> TileSet::copy_to(const ResourceBudget& root) const {
  if (owned_by(root))
    return Result<TileSet>(*this);
  return from_intervals(root, intervals().data(), intervals().size());
}
std::size_t TileSet::size() const noexcept {
  return storage_ ? storage_->count : 0;
}
TileIndex TileSet::at(std::uint64_t position) const noexcept {
  const auto found = std::upper_bound(storage_->offsets.begin(),
                                      storage_->offsets.end(), position);
  const auto slot =
      static_cast<std::size_t>(found - storage_->offsets.begin() - 1);
  return storage_->intervals[slot].begin + position - storage_->offsets[slot];
}
bool TileSet::contains(TileIndex index) const noexcept {
  const auto& ranges = intervals();
  const auto found = std::upper_bound(
      ranges.begin(), ranges.end(), index,
      [](auto value, auto interval) { return value < interval.begin; });
  return found != ranges.begin() && index < std::prev(found)->end;
}
bool TileSet::contains(const TileSet& other) const noexcept {
  const auto& a = intervals();
  std::size_t i = 0;
  for (const auto b : other.intervals()) {
    while (i < a.size() && a[i].end <= b.begin)
      ++i;
    if (i == a.size() || a[i].begin > b.begin || a[i].end < b.end)
      return false;
  }
  return true;
}
bool TileSet::intersects(const TileSet& other) const noexcept {
  const auto& a = intervals();
  const auto& b = other.intervals();
  std::size_t i = 0, j = 0;
  while (i < a.size() && j < b.size()) {
    if (a[i].begin < b[j].end && b[j].begin < a[i].end)
      return true;
    if (a[i].end <= b[j].end)
      ++i;
    else
      ++j;
  }
  return false;
}
bool TileSet::operator==(const TileSet& other) const noexcept {
  return storage_ == other.storage_ || intervals() == other.intervals();
}
Result<TileGrid> TileGrid::from_schema(const SchemaTemplate& schema,
                                       std::uint64_t tile_extent,
                                       std::uint32_t tensor_slot) {
  using Answer = Result<TileGrid>;
  auto valid = schema.validate();
  if (!valid.ok())
    return Answer(valid);
  if (tensor_slot >= schema.tensors.size())
    return Answer(
        Status{ErrorCode::InvalidArgument, "missing tensor grid member"});
  return from_tensor(schema.tensors[tensor_slot], tile_extent);
}
Result<std::vector<TileGrid>> TileGrid::members(const SchemaTemplate& schema,
                                                std::uint64_t tile_extent) {
  using Answer = Result<std::vector<TileGrid>>;
  auto valid = schema.validate();
  if (!valid.ok())
    return Answer(valid);
  std::vector<TileGrid> result;
  result.reserve(schema.tensors.size());
  for (const auto& tensor : schema.tensors) {
    auto grid = from_tensor(tensor, tile_extent);
    if (!grid.ok())
      return Answer(grid.status());
    result.push_back(grid.take_value());
  }
  return Answer(std::move(result));
}
Result<TileGrid> TileGrid::from_tensor(const ResultTensorSpec& tensor,
                                       std::uint64_t tile_extent) {
  using Answer = Result<TileGrid>;
  if (!tile_extent || (tile_extent & (tile_extent - 1)))
    return Answer(Status{ErrorCode::InvalidArgument, "invalid tile extent"});
  const auto shape = tensor.sample_shape();
  if (shape.empty() || shape.size() > 8 ||
      tensor.atomic_trailing_axes > tensor.descriptor.shape.size() ||
      std::any_of(shape.begin(), shape.end(), [](auto n) { return n == 0; }))
    return Answer(
        Status{ErrorCode::InvalidArgument, "invalid tensor grid shape"});
  TileGrid grid;
  grid.extent_ = tile_extent;
  grid.rank_ = shape.size();
  grid.batch_rank_ = tensor.batch_axes.size();
  const auto free_rank = grid.rank_ - tensor.atomic_trailing_axes;
  grid.height_axis_ = grid.width_axis_ = grid.rank_;
  if (tensor.layout.spatial) {
    grid.height_axis_ = grid.batch_rank_ + tensor.layout.height_axis;
    grid.width_axis_ = grid.batch_rank_ + tensor.layout.width_axis;
    if (grid.height_axis_ >= free_rank || grid.width_axis_ >= free_rank ||
        grid.height_axis_ == grid.width_axis_)
      return Answer(
          Status{ErrorCode::InvalidArgument, "invalid spatial grid axes"});
  } else {
    if (free_rank > grid.batch_rank_)
      grid.width_axis_ = free_rank - 1;
    if (free_rank > grid.batch_rank_ + 1)
      grid.height_axis_ = free_rank - 2;
  }
  std::copy(shape.begin(), shape.end(), grid.shape_.begin());
  grid.batches_ = 1;
  for (std::uint32_t axis = 0;
       axis < (tensor.layout.spatial ? grid.batch_rank_ : free_rank); ++axis) {
    if (axis == grid.height_axis_ || axis == grid.width_axis_)
      continue;
    grid.batch_axis_mask_ |= 1U << axis;
    if (!core_internal::can_multiply(grid.batches_, shape[axis]))
      return Answer(
          Status{ErrorCode::ResourceExhausted, "tile batch count overflow"});
    grid.batches_ *= shape[axis];
  }
  const auto count = [tile_extent](std::uint64_t n) {
    return n / tile_extent + (n % tile_extent != 0);
  };
  grid.rows_ =
      grid.height_axis_ == grid.rank_ ? 1 : count(shape[grid.height_axis_]);
  grid.columns_ =
      grid.width_axis_ == grid.rank_ ? 1 : count(shape[grid.width_axis_]);
  if (!core_internal::can_multiply(grid.batches_, grid.rows_) ||
      !core_internal::can_multiply(grid.batches_ * grid.rows_, grid.columns_))
    return Answer(
        Status{ErrorCode::ResourceExhausted, "tile index space overflow"});
  grid.count_ = grid.batches_ * grid.rows_ * grid.columns_;
  return Answer(grid);
}
Result<Region> TileGrid::region(TileIndex tile) const {
  if (tile >= count_)
    return Result<Region>(
        Status{ErrorCode::InvalidArgument, "tile index outside grid"});
  const auto column = tile % columns_;
  const auto row = (tile / columns_) % rows_;
  auto batch = tile / columns_ / rows_;
  std::vector<RegionDimension> dimensions(rank_);
  for (std::uint32_t axis = 0; axis < rank_; ++axis)
    dimensions[axis] = {0, shape_[axis]};
  for (std::uint32_t axis = rank_; axis-- > 0;) {
    if (!(batch_axis_mask_ & (1U << axis)))
      continue;
    dimensions[axis] = {batch % shape_[axis], 1};
    batch /= shape_[axis];
  }
  const auto height = row * extent_, width = column * extent_;
  if (height_axis_ < rank_)
    dimensions[height_axis_] = {
        height, std::min(extent_, shape_[height_axis_] - height)};
  if (width_axis_ < rank_)
    dimensions[width_axis_] = {width,
                               std::min(extent_, shape_[width_axis_] - width)};
  return Result<Region>(Region(std::move(dimensions)));
}
Result<Footprint> TileGrid::coverage(const TileSet& tiles,
                                     const FootprintLimits& limits) const {
  using Answer = Result<Footprint>;
  if (!tiles.empty() && tiles.intervals().back().end > count_)
    return Answer(Status{ErrorCode::InvalidArgument, "tiles outside grid"});
  const std::vector<std::uint64_t> shape(shape_.begin(),
                                         shape_.begin() + rank_);
  auto result = Footprint::none(shape, limits);
  if (!result.ok())
    return result;
  std::array<std::uint32_t, 8> axes{};
  std::array<std::uint64_t, 8> counts{}, strides{}, digits{};
  std::size_t n = 0;
  for (std::uint32_t axis = 0; axis < rank_; ++axis)
    if (batch_axis_mask_ & (1U << axis)) {
      axes[n] = axis;
      counts[n++] = shape_[axis];
    }
  if (height_axis_ < rank_) {
    axes[n] = height_axis_;
    counts[n++] = rows_;
  }
  if (width_axis_ < rank_) {
    axes[n] = width_axis_;
    counts[n++] = columns_;
  }
  if (!n)
    return tiles.empty() ? result : Footprint::all(shape, limits);
  // Canonicalize the interval decomposition once. Repeated unions make a
  // sparse selection quadratic in the number of disjoint intervals.
  std::vector<Region> regions;
  std::uint64_t stride = 1;
  for (auto j = n; j-- > 0;) {
    strides[j] = stride;
    stride *= counts[j];
  }
  for (const auto interval : tiles.intervals()) {
    auto next = interval.begin;
    while (next < interval.end) {
      if (limits.cancellation.cancelled())
        return Answer(Status{ErrorCode::Cancelled, {}});
      if (limits.consume_work) {
        auto work = limits.consume_work(rank_ + n + 1);
        if (!work.ok())
          return Answer(work);
      }
      for (std::size_t j = 0; j < n; ++j)
        digits[j] = next / strides[j] % counts[j];
      auto axis = n - 1;
      const auto remaining = interval.end - next;
      while (axis && next % strides[axis - 1] == 0 &&
             remaining >= strides[axis - 1])
        --axis;
      const auto run =
          std::min(counts[axis] - digits[axis], remaining / strides[axis]);
      std::vector<RegionDimension> dimensions(rank_);
      for (std::uint32_t j = 0; j < rank_; ++j)
        dimensions[j] = {0, shape_[j]};
      for (std::size_t j = 0; j <= axis; ++j) {
        const auto selected = axes[j];
        const auto scale = batch_axis_mask_ & (1U << selected) ? 1 : extent_;
        const auto begin = digits[j] * scale;
        const auto length = j == axis ? run : 1;
        // Subtraction before multiplication avoids overflow in the clipped
        // tail.
        const auto end_tile = digits[j] + length;
        const auto end =
            end_tile == counts[j] ? shape_[selected] : end_tile * scale;
        dimensions[selected] = {begin, end - begin};
      }
      if (regions.size() >= limits.maximum_boxes) {
        // A stripe may normalize to fewer boxes than its interval pieces.
        // Keep the previous bounded-union acceptance at the box limit.
        auto batch = Footprint::from_regions(shape, regions, limits);
        if (!batch.ok())
          return batch;
        auto part = Footprint::from_regions(
            shape, {Region(std::move(dimensions))}, limits);
        if (!part.ok())
          return part;
        auto combined = batch.value().unite(part.value(), limits);
        if (!combined.ok())
          return combined;
        regions = combined.value().boxes();
      } else {
        regions.emplace_back(std::move(dimensions));
      }
      next += run * strides[axis];
    }
  }
  return Footprint::from_regions(shape, regions, limits);
}
Result<TileRequest> TileRequest::all(const std::vector<TileGrid>& grids,
                                     const ResourceBudget& root) {
  TileRequest request;
  for (std::uint32_t slot = 0; slot < grids.size(); ++slot) {
    auto tiles = TileSet::range(root, 0, grids[slot].tile_count());
    if (!tiles.ok())
      return Result<TileRequest>(tiles.status());
    request.members.emplace(slot, tiles.take_value());
  }
  return request.resolve(grids);
}
Result<TileRequest> TileRequest::resolve(
    const std::vector<TileGrid>& grids) const {
  using Answer = Result<TileRequest>;
  TileRequest result;
  std::uint64_t count = 0;
  for (const auto& member : members) {
    if (member.first >= grids.size() ||
        (!member.second.empty() && member.second.intervals().back().end >
                                       grids[member.first].tile_count()))
      return Answer(Status{ErrorCode::InvalidArgument,
                           "tile request outside member grid"});
    if (!core_internal::checked_add(count, member.second.size(), &count))
      return Answer(Status{ErrorCode::ResourceExhausted,
                           "tile request cardinality overflow"});
    if (!member.second.empty())
      result.members.insert(member);
  }
  return Answer(std::move(result));
}
Result<TileRequest> TileRequest::copy_to(const ResourceBudget& root) const {
  return unite({}, root);
}
Result<TileRequest> TileRequest::unite(const TileRequest& other,
                                       const ResourceBudget& root) const {
  TileRequest result;
  for (const auto* request : {this, &other})
    for (const auto& member : request->members) {
      auto found = result.members.find(member.first);
      auto merged = found == result.members.end()
                        ? member.second.copy_to(root)
                        : found->second.unite(member.second, root);
      if (!merged.ok())
        return Result<TileRequest>(merged.status());
      if (!merged.value().empty())
        result.members[member.first] = merged.take_value();
    }
  return Result<TileRequest>(std::move(result));
}
std::uint64_t TileRequest::size() const noexcept {
  std::uint64_t count = 0;
  for (const auto& member : members)
    count += member.second.size();
  return count;
}
std::size_t TileRequest::interval_count() const noexcept {
  std::size_t count = 0;
  for (const auto& member : members)
    count += member.second.intervals().size();
  return count;
}
TileAddress TileRequest::at(std::uint64_t position) const noexcept {
  for (const auto& member : members) {
    if (position < member.second.size())
      return {member.first, member.second.at(position)};
    position -= member.second.size();
  }
  return {UINT32_MAX, UINT64_MAX};
}
bool TileRequest::contains(const TileRequest& other) const noexcept {
  for (const auto& member : other.members) {
    const auto found = members.find(member.first);
    if (found == members.end() || !found->second.contains(member.second))
      return false;
  }
  return true;
}
Status TileNeed::validate() const {
  if (descriptor_only && (!tiles.empty() || tensor_slot))
    return Status{ErrorCode::InvalidArgument, "invalid tile Need kind"};
  return Status::success();
}
Result<TileNeed> TileNeed::resolve(const std::vector<TileGrid>& grids,
                                   const ResourceBudget& root) const {
  using Answer = Result<TileNeed>;
  auto valid = validate();
  if (!valid.ok())
    return Answer(valid);
  auto charged = root.consume({1 + tiles.intervals().size()});
  if (!charged.ok())
    return Answer(charged);
  if (!descriptor_only &&
      (tensor_slot >= grids.size() ||
       (!tiles.empty() &&
        tiles.intervals().back().end > grids[tensor_slot].tile_count())))
    return Answer(
        Status{ErrorCode::InvalidArgument, "tile Need outside member grid"});
  return Answer(*this);
}
}  // namespace ps
