#include "photospider/data/footprint.hpp"

#include <algorithm>
#include <queue>
#include <utility>
#include <vector>

namespace ps {
namespace {
using Box = std::vector<RegionDimension>;
using Boxes = std::vector<Box>;
enum class Combine { Union, Intersection, Difference };

bool equal_boxes(const Boxes& a, const Boxes& b) {
  if (a.size() != b.size())
    return false;
  for (std::size_t i = 0; i < a.size(); ++i) {
    if (a[i].size() != b[i].size())
      return false;
    for (std::size_t j = 0; j < a[i].size(); ++j)
      if (a[i][j].offset != b[i][j].offset || a[i][j].extent != b[i][j].extent)
        return false;
  }
  return true;
}

// Private typed unwinding keeps every recursive allocation behind its bound.
struct Stop final {
  Status status;
};
struct Work final {
  const FootprintLimits& limits;
  std::uint64_t remaining;
  explicit Work(const FootprintLimits& value)
      : limits(value), remaining(value.maximum_work) {
    check();
  }
  void check() const {
    if (limits.cancellation.cancelled())
      throw Stop{Status::failure(ErrorCode::Cancelled, "footprint cancelled")};
  }
  void tick() {
    check();
    if (!remaining)
      throw Stop{Status::failure(ErrorCode::ResourceExhausted,
                                 "footprint work limit")};
    --remaining;
  }
  void capacity(std::size_t count) const {
    check();
    if (count >= limits.maximum_boxes)
      throw Stop{
          Status::failure(ErrorCode::ResourceExhausted, "footprint box limit")};
  }
};

bool selected(bool left, bool right, Combine operation) {
  switch (operation) {
    case Combine::Union:
      return left || right;
    case Combine::Intersection:
      return left && right;
    case Combine::Difference:
      return left && !right;
  }
  return false;
}

// Sweep in descriptor axis order. Adjacent intervals merge exactly when their
// recursively canonical suffix sets agree. This removes input tiling identity.
Boxes sweep(const Boxes& a, const Boxes& b, std::size_t axis, std::size_t rank,
            Combine operation, Work* work) {
  work->tick();
  if (axis == rank)
    return selected(!a.empty(), !b.empty(), operation) ? Boxes{Box{}} : Boxes{};
  if (a.empty() && (b.empty() || operation != Combine::Union))
    return {};
  if (b.empty() && operation == Combine::Intersection)
    return {};
  std::vector<std::uint64_t> boundaries;
  for (const auto* boxes : {&a, &b})
    for (const auto& box : *boxes) {
      work->tick();
      boundaries.push_back(box[axis].offset);
      boundaries.push_back(box[axis].offset + box[axis].extent);
    }
  std::sort(boundaries.begin(), boundaries.end(), [&](auto x, auto y) {
    work->tick();
    return x < y;
  });
  boundaries.erase(std::unique(boundaries.begin(), boundaries.end()),
                   boundaries.end());
  Boxes result, previous;
  std::uint64_t start = 0, end = 0;
  auto flush = [&] {
    for (auto suffix : previous) {
      work->tick();
      work->capacity(result.size());
      suffix.insert(suffix.begin(), {start, end - start});
      result.push_back(std::move(suffix));
    }
  };
  for (std::size_t i = 1; i < boundaries.size(); ++i) {
    work->tick();
    const auto lo = boundaries[i - 1], hi = boundaries[i];
    Boxes left, right;
    for (const auto& box : a) {
      work->tick();
      if (box[axis].offset <= lo && hi <= box[axis].offset + box[axis].extent)
        left.push_back(box);
    }
    for (const auto& box : b) {
      work->tick();
      if (box[axis].offset <= lo && hi <= box[axis].offset + box[axis].extent)
        right.push_back(box);
    }
    auto suffix = sweep(left, right, axis + 1, rank, operation, work);
    if (i > 1 && equal_boxes(previous, suffix)) {
      end = hi;
    } else {
      flush();
      previous = std::move(suffix);
      start = lo;
      end = hi;
    }
  }
  flush();
  return result;
}

Status shape_status(const std::vector<std::uint64_t>& shape) {
  if (shape.empty() || shape.size() > 8 ||
      std::find(shape.begin(), shape.end(), 0) != shape.end())
    return Status::failure(ErrorCode::InvalidArgument,
                           "footprint requires nonzero rank-1..8 domain");
  return Status::success();
}

Result<std::vector<Region>> combine(const std::vector<std::uint64_t>& shape,
                                    const std::vector<Region>& a,
                                    const std::vector<Region>& b,
                                    Combine operation,
                                    const FootprintLimits& limits) {
  auto status = shape_status(shape);
  if (!status.ok())
    return Result<std::vector<Region>>(status);
  try {
    Work work(limits);
    Boxes left, right;
    for (int operand = 0; operand < 2; ++operand) {
      const auto* regions = operand == 0 ? &a : &b;
      auto& target = operand == 0 ? left : right;
      for (const auto& region : *regions) {
        work.tick();
        status = region.validate(shape);
        if (!status.ok())
          return Result<std::vector<Region>>(status);
        if (!region.empty()) {
          work.capacity(target.size());
          target.push_back(region.dimensions());
        }
      }
    }
    auto normalized = sweep(left, right, 0, shape.size(), operation, &work);
    std::vector<Region> result;
    for (auto& box : normalized)
      result.emplace_back(std::move(box));
    return Result<std::vector<Region>>(std::move(result));
  } catch (const Stop& stop) {
    return Result<std::vector<Region>>(stop.status);
  }
}
}  // namespace

Result<Footprint> Footprint::from_regions(std::vector<std::uint64_t> shape,
                                          const std::vector<Region>& boxes,
                                          const FootprintLimits& limits) {
  auto normalized = combine(shape, boxes, {}, Combine::Union, limits);
  if (!normalized.ok())
    return Result<Footprint>(normalized.status());
  Footprint result;
  result.shape_ = std::move(shape);
  result.boxes_ = normalized.take_value();
  return Result<Footprint>(std::move(result));
}
Result<Footprint> Footprint::all(std::vector<std::uint64_t> shape,
                                 const FootprintLimits& limits) {
  auto status = shape_status(shape);
  if (!status.ok())
    return Result<Footprint>(status);
  return from_regions(shape, {Region::whole(shape)}, limits);
}
Result<Footprint> Footprint::none(std::vector<std::uint64_t> shape,
                                  const FootprintLimits& limits) {
  return from_regions(std::move(shape), {}, limits);
}
bool Footprint::contains(
    const std::vector<std::uint64_t>& coordinate) const noexcept {
  if (!valid() || coordinate.size() != shape_.size())
    return false;
  for (const auto& box : boxes_) {
    bool inside = true;
    for (std::size_t axis = 0; axis < shape_.size(); ++axis) {
      const auto d = box.dimensions()[axis];
      if (coordinate[axis] < d.offset ||
          coordinate[axis] - d.offset >= d.extent) {
        inside = false;
        break;
      }
    }
    if (inside)
      return true;
  }
  return false;
}
Result<Footprint> Footprint::unite(const Footprint& other,
                                   const FootprintLimits& limits) const {
  if (shape_ != other.shape_)
    return Result<Footprint>(Status::failure(ErrorCode::InvalidArgument,
                                             "footprint domain mismatch"));
  auto boxes = combine(shape_, boxes_, other.boxes_, Combine::Union, limits);
  if (!boxes.ok())
    return Result<Footprint>(boxes.status());
  Footprint result;
  result.shape_ = shape_;
  result.boxes_ = boxes.take_value();
  return Result<Footprint>(std::move(result));
}
Result<Footprint> Footprint::intersect(const Footprint& other,
                                       const FootprintLimits& limits) const {
  if (shape_ != other.shape_)
    return Result<Footprint>(Status::failure(ErrorCode::InvalidArgument,
                                             "footprint domain mismatch"));
  auto boxes =
      combine(shape_, boxes_, other.boxes_, Combine::Intersection, limits);
  if (!boxes.ok())
    return Result<Footprint>(boxes.status());
  Footprint result;
  result.shape_ = shape_;
  result.boxes_ = boxes.take_value();
  return Result<Footprint>(std::move(result));
}
Result<Footprint> Footprint::subtract(const Footprint& other,
                                      const FootprintLimits& limits) const {
  if (shape_ != other.shape_)
    return Result<Footprint>(Status::failure(ErrorCode::InvalidArgument,
                                             "footprint domain mismatch"));
  auto boxes =
      combine(shape_, boxes_, other.boxes_, Combine::Difference, limits);
  if (!boxes.ok())
    return Result<Footprint>(boxes.status());
  Footprint result;
  result.shape_ = shape_;
  result.boxes_ = boxes.take_value();
  return Result<Footprint>(std::move(result));
}
bool Footprint::operator==(const Footprint& other) const noexcept {
  if (shape_ != other.shape_ || boxes_.size() != other.boxes_.size())
    return false;
  for (std::size_t i = 0; i < boxes_.size(); ++i)
    for (std::size_t axis = 0; axis < shape_.size(); ++axis) {
      const auto a = boxes_[i].dimensions()[axis];
      const auto b = other.boxes_[i].dimensions()[axis];
      if (a.offset != b.offset || a.extent != b.extent)
        return false;
    }
  return true;
}
Result<std::uint64_t> Footprint::element_count() const {
  if (!valid())
    return Result<std::uint64_t>(shape_status(shape_));
  std::uint64_t sum = 0;
  for (const auto& box : boxes_) {
    auto count = box.element_count();
    if (!count.ok())
      return count;
    if (count.value() > UINT64_MAX - sum)
      return Result<std::uint64_t>(Status::failure(ErrorCode::ResourceExhausted,
                                                   "footprint count overflow"));
    sum += count.value();
  }
  return Result<std::uint64_t>(sum);
}
Result<Footprint> Footprint::tile_cover(
    const std::vector<std::uint64_t>& geometry,
    const FootprintLimits& limits) const {
  if (!valid() || geometry.size() != shape_.size() ||
      !shape_status(geometry).ok())
    return Result<Footprint>(
        Status::failure(ErrorCode::InvalidArgument, "invalid tile geometry"));
  std::vector<std::uint64_t> shape;
  for (std::size_t axis = 0; axis < shape_.size(); ++axis)
    shape.push_back(shape_[axis] / geometry[axis] +
                    (shape_[axis] % geometry[axis] != 0));
  std::vector<Region> boxes;
  try {
    Work work(limits);
    for (const auto& box : boxes_) {
      work.tick();
      work.capacity(boxes.size());
      Box dimensions;
      for (std::size_t axis = 0; axis < shape_.size(); ++axis) {
        const auto d = box.dimensions()[axis];
        const auto first = d.offset / geometry[axis];
        const auto last = (d.offset + d.extent - 1) / geometry[axis];
        dimensions.push_back({first, last - first + 1});
      }
      boxes.emplace_back(std::move(dimensions));
    }
    auto remaining = limits;
    remaining.maximum_work = work.remaining;
    return from_regions(std::move(shape), boxes, remaining);
  } catch (const Stop& stop) {
    return Result<Footprint>(stop.status);
  }
}
Status Footprint::visit(
    const std::function<Status(const std::vector<std::uint64_t>&)>& visitor,
    std::uint64_t maximum_samples,
    const CancellationToken& cancellation) const {
  if (!valid() || !visitor)
    return Status::failure(ErrorCode::InvalidArgument,
                           "invalid footprint visit");
  if (cancellation.cancelled())
    return Status::failure(ErrorCode::Cancelled, "footprint visit cancelled");
  struct Cursor {
    std::size_t box;
    std::vector<std::uint64_t> coordinate;
  };
  auto greater = [](const Cursor& a, const Cursor& b) {
    return a.coordinate > b.coordinate;
  };
  std::priority_queue<Cursor, std::vector<Cursor>, decltype(greater)> ready(
      greater);
  for (std::size_t i = 0; i < boxes_.size(); ++i) {
    std::vector<std::uint64_t> coordinate;
    for (auto d : boxes_[i].dimensions())
      coordinate.push_back(d.offset);
    ready.push({i, std::move(coordinate)});
  }
  while (!ready.empty()) {
    if (cancellation.cancelled())
      return Status::failure(ErrorCode::Cancelled, "footprint visit cancelled");
    if (!maximum_samples)
      return Status::failure(ErrorCode::ResourceExhausted,
                             "footprint visit limit");
    --maximum_samples;
    auto cursor = ready.top();
    ready.pop();
    auto status = visitor(cursor.coordinate);
    if (!status.ok())
      return status;
    for (std::size_t axis = shape_.size(); axis-- > 0;) {
      const auto d = boxes_[cursor.box].dimensions()[axis];
      ++cursor.coordinate[axis];
      if (cursor.coordinate[axis] < d.offset + d.extent) {
        ready.push(std::move(cursor));
        break;
      }
      cursor.coordinate[axis] = d.offset;
    }
  }
  return Status::success();
}
}  // namespace ps
