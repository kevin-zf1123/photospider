#include "photospider/data/tiles.hpp"

#include <algorithm>
#include <iterator>
#include <memory>
#include <new>
#include <utility>
#include <vector>

#include "core/checked_math.hpp"
#include "photospider/data/result.hpp"

namespace ps {
struct TileSet::Storage {
  explicit Storage(const ResourceBudget& root)
      : indices(ResourceAllocator<TileIndex>(root)) {}
  ResourceVector<TileIndex> indices;
};
bool TileSet::owned_by(const ResourceBudget& root) const noexcept {
  return !storage_ || storage_->indices.get_allocator().owned_by(root);
}
Result<TileSet> TileSet::from_indices(const ResourceBudget& root,
                                      const std::vector<TileIndex>& indices) {
  return from_indices(root, indices.data(), indices.size());
}
Result<TileSet> TileSet::from_indices(const ResourceBudget& root,
                                      const TileIndex* indices,
                                      std::size_t count) try {
  using Answer = Result<TileSet>;
  if (!indices && count)
    return Answer(Status{ErrorCode::InvalidArgument, "null tile indices"});
  std::uint64_t passes = 2;
  for (auto remaining = count; remaining > 1; remaining /= 2)
    ++passes;
  if (!core_internal::can_multiply(count, passes))
    return Answer(
        Status{ErrorCode::ResourceExhausted, "tile-set work overflow"});
  auto charged = root.consume({count * passes});
  if (!charged.ok())
    return Answer(charged);
  if (!count)
    return Answer(TileSet{});
  auto storage =
      std::allocate_shared<Storage>(ResourceAllocator<Storage>(root), root);
  storage->indices.assign(indices, indices + count);
  std::sort(storage->indices.begin(), storage->indices.end());
  storage->indices.erase(
      std::unique(storage->indices.begin(), storage->indices.end()),
      storage->indices.end());
  TileSet result;
  result.storage_ = std::move(storage);
  return Answer(std::move(result));
} catch (const std::bad_alloc&) {
  return Result<TileSet>(Status{ErrorCode::ResourceExhausted, {}});
}
Result<TileSet> TileSet::unite(const TileSet& other,
                               const ResourceBudget& root) const try {
  using Answer = Result<TileSet>;
  if (!core_internal::can_add(size(), other.size()))
    return Answer(
        Status{ErrorCode::ResourceExhausted, "tile-set size overflow"});
  auto charged = root.consume({size() + other.size()});
  if (!charged.ok())
    return Answer(charged);
  if (empty() && other.empty())
    return Answer(TileSet{});
  auto storage =
      std::allocate_shared<Storage>(ResourceAllocator<Storage>(root), root);
  storage->indices.reserve(size() + other.size());
  std::set_union(begin(), end(), other.begin(), other.end(),
                 std::back_inserter(storage->indices));
  TileSet result;
  result.storage_ = std::move(storage);
  return Answer(std::move(result));
} catch (const std::bad_alloc&) {
  return Result<TileSet>(Status{ErrorCode::ResourceExhausted, {}});
}
Result<TileSet> TileSet::copy_to(const ResourceBudget& root) const {
  return unite({}, root);
}
std::size_t TileSet::size() const noexcept {
  return storage_ ? storage_->indices.size() : 0;
}
const TileIndex* TileSet::begin() const noexcept {
  static const TileIndex empty = 0;
  return storage_ ? storage_->indices.data() : &empty;
}
const TileIndex* TileSet::end() const noexcept {
  return begin() + size();
}
bool TileSet::contains(TileIndex index) const noexcept {
  return std::binary_search(begin(), end(), index);
}
bool TileSet::operator==(const TileSet& other) const noexcept {
  return storage_ == other.storage_ ||
         (size() == other.size() && std::equal(begin(), end(), other.begin()));
}
Result<std::optional<TileGrid>> TileGrid::from_schema(
    const SchemaTemplate& schema, std::uint64_t tile_extent) {
  using Answer = Result<std::optional<TileGrid>>;
  if (!tile_extent || (tile_extent & (tile_extent - 1)))
    return Answer(
        Status{ErrorCode::InvalidArgument, "invalid square tile extent"});
  auto valid = schema.validate(true);
  if (!valid.ok())
    return Answer(valid);
  if (!schema.fields.empty() || schema.tensors.size() != 1 ||
      !schema.tensors[0].layout.spatial)
    return Answer(std::optional<TileGrid>{});
  const auto& tensor = schema.tensors[0];
  TileGrid grid;
  grid.extent_ = tile_extent;
  grid.batch_rank_ = static_cast<std::uint32_t>(tensor.batch_axes.size());
  grid.rank_ = grid.batch_rank_ +
               static_cast<std::uint32_t>(tensor.descriptor.shape.size());
  grid.height_axis_ = grid.batch_rank_ + tensor.layout.height_axis;
  grid.width_axis_ = grid.batch_rank_ + tensor.layout.width_axis;
  std::copy(tensor.batch_axes.begin(), tensor.batch_axes.end(),
            grid.shape_.begin());
  std::copy(tensor.descriptor.shape.begin(), tensor.descriptor.shape.end(),
            grid.shape_.begin() + grid.batch_rank_);
  grid.batches_ = 1;
  for (auto extent : tensor.batch_axes) {
    if (!core_internal::can_multiply(grid.batches_, extent))
      return Answer(
          Status{ErrorCode::ResourceExhausted, "tile batch count overflow"});
    grid.batches_ *= extent;
  }
  const auto count = [tile_extent](std::uint64_t extent) {
    return extent / tile_extent + (extent % tile_extent != 0);
  };
  grid.rows_ = count(grid.shape_[grid.height_axis_]);
  grid.columns_ = count(grid.shape_[grid.width_axis_]);
  if (!core_internal::can_multiply(grid.batches_, grid.rows_) ||
      !core_internal::can_multiply(grid.batches_ * grid.rows_, grid.columns_))
    return Answer(
        Status{ErrorCode::ResourceExhausted, "tile index space overflow"});
  grid.count_ = grid.batches_ * grid.rows_ * grid.columns_;
  return Answer(std::optional<TileGrid>{grid});
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
  for (std::uint32_t axis = batch_rank_; axis-- > 0;) {
    dimensions[axis] = {batch % shape_[axis], 1};
    batch /= shape_[axis];
  }
  const auto height = row * extent_, width = column * extent_;
  dimensions[height_axis_] = {height,
                              std::min(extent_, shape_[height_axis_] - height)};
  dimensions[width_axis_] = {width,
                             std::min(extent_, shape_[width_axis_] - width)};
  return Result<Region>(Region(std::move(dimensions)));
}
Result<TileRequest> TileRequest::resolve(const std::optional<TileGrid>& grid,
                                         bool tiled) const {
  using Answer = Result<TileRequest>;
  if ((whole && !tiles.empty()) || (tiled && !grid))
    return Answer(
        Status{ErrorCode::InvalidArgument, "invalid tile request combination"});
  if (tiles.empty())
    return Answer(*this);
  if (!grid || *(tiles.end() - 1) >= grid->tile_count())
    return Answer(Status{ErrorCode::InvalidArgument,
                         "tile request outside producer grid"});
  return Answer(tiled ? *this : TileRequest{true, {}});
}
bool TileRequest::contains(const TileRequest& other) const noexcept {
  return whole || (!other.whole &&
                   std::includes(tiles.begin(), tiles.end(),
                                 other.tiles.begin(), other.tiles.end()));
}
Status TileNeed::validate() const {
  const unsigned kinds = static_cast<unsigned>(whole) +
                         static_cast<unsigned>(descriptor_only) +
                         static_cast<unsigned>(!tiles.empty());
  return kinds == 1 ? Status::success()
                    : Status{ErrorCode::InvalidArgument,
                             "invalid tile Need combination"};
}
Result<TileNeed> TileNeed::resolve(const std::optional<TileGrid>& grid,
                                   bool tiled,
                                   const ResourceBudget& root) const {
  using Answer = Result<TileNeed>;
  auto valid = validate();
  if (!valid.ok())
    return Answer(valid);
  if (tiled && !grid)
    return Answer(
        Status{ErrorCode::InvalidArgument, "Tiled producer has no grid"});
  if (tiles.empty())
    return Answer(*this);
  if (!grid)
    return Answer(
        Status{ErrorCode::InvalidArgument, "producer has no tile grid"});
  auto charged = root.consume({tiles.size()});
  if (!charged.ok())
    return Answer(charged);
  for (auto tile : tiles)
    if (tile >= grid->tile_count())
      return Answer(
          Status{ErrorCode::InvalidArgument, "tile Need outside grid"});
  return Answer(tiled ? *this : TileNeed{input, true, false, {}});
}
}  // namespace ps
