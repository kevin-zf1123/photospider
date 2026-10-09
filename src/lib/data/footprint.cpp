#include "photospider/data/footprint.hpp"

#include <algorithm>
#include <array>
#include <memory>
#include <optional>
#include <queue>
#include <utility>
#include <vector>

#include "core/radix_sort.hpp"
#include "data/footprint_index.hpp"
#include "photospider/core/resource_allocator.hpp"

namespace ps {
namespace {
using Box = ResourceVector<RegionDimension>;
using Boxes = ResourceVector<Box>;
enum class Combine { Union, Intersection, Difference };
struct Normalized {
  ResourceLease lease;
  std::vector<Region> boxes;
};

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
  std::uint64_t* consumed;
  explicit Work(const FootprintLimits& value, std::uint64_t* measured = nullptr)
      : limits(value), remaining(value.maximum_work), consumed(measured) {
    check();
  }
  ~Work() {
    if (consumed)
      *consumed = limits.maximum_work - remaining;
  }
  void check() const {
    if (limits.cancellation.cancelled())
      throw Stop{Status::failure(ErrorCode::Cancelled, "footprint cancelled")};
  }
  void tick(std::uint64_t count = 1) {
    check();
    if (count > remaining)
      throw Stop{Status::failure(ErrorCode::ResourceExhausted,
                                 "footprint work limit")};
    if (!count)
      return;
    if (limits.consume_work) {
      auto charged = limits.consume_work(count);
      if (!charged.ok())
        throw Stop{std::move(charged)};
    } else if (auto* root = resource_internal::metadata_budget()) {
      auto charged = root->consume({count});
      if (!charged.ok())
        throw Stop{std::move(charged)};
    }
    remaining -= count;
    check();
  }
  void capacity(std::size_t count) const {
    check();
    if (count >= limits.maximum_boxes)
      throw Stop{
          Status::failure(ErrorCode::ResourceExhausted, "footprint box limit")};
  }
};

// Canonical suffixes for one axis interval occupy a contiguous range. A point
// lookup narrows that range on each axis instead of scanning every rectangle.
const Region* containing_box(const std::vector<Region>& boxes,
                             const std::uint64_t* at, std::size_t rank,
                             Work* work = nullptr) {
  const auto index = footprint_internal::containing(
      boxes.size(), rank, at, [&](auto i) -> const Region& { return boxes[i]; },
      [&] {
        if (work)
          work->tick();
      });
  return index == boxes.size() ? nullptr : &boxes[index];
}

Result<bool> contains_rectangle(const Footprint& set, const Region& region,
                                FootprintLimits* remaining) {
  std::uint64_t consumed = 0;
  auto check = [&]() -> Result<bool> {
    try {
      Work work(*remaining, &consumed);
      work.tick();
      std::array<std::uint64_t, 8> at{};
      for (std::size_t axis = 0; axis < region.rank(); ++axis)
        at[axis] = region.dimensions()[axis].offset;
      const auto* box =
          containing_box(set.boxes(), at.data(), region.rank(), &work);
      if (!box)
        return Result<bool>(false);
      for (std::size_t axis = 0; axis < region.rank(); ++axis) {
        work.tick();
        const auto a = region.dimensions()[axis], b = box->dimensions()[axis];
        if (a.extent > b.extent - (a.offset - b.offset))
          return Result<bool>(false);
      }
      return Result<bool>(true);
    } catch (const Stop& stop) {
      return Result<bool>(stop.status);
    } catch (const std::bad_alloc&) {
      return Result<bool>(Status{ErrorCode::ResourceExhausted, {}});
    }
  };
  auto result = check();
  remaining->maximum_work -= consumed;
  return result;
}

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
  struct Event {
    std::uint64_t coordinate;
    std::size_t index;
    unsigned side;
    bool begin;
  };
  ResourceVector<Event> events;
  const Boxes* operands[] = {&a, &b};
  ResourceVector<std::size_t> active[2], positions[2];
  for (unsigned side = 0; side < 2; ++side) {
    positions[side].resize(operands[side]->size());
    for (std::size_t i = 0; i < operands[side]->size(); ++i) {
      work->tick();
      const auto d = (*operands[side])[i][axis];
      events.push_back({d.offset, i, side, true});
      events.push_back({d.offset + d.extent, i, side, false});
    }
  }
  // Small suffixes avoid histogram setup. Larger event sets use fixed-width
  // radix passes, charging each pass once instead of refreshing execution
  // state from every comparison in an O(n log n) sort.
  if (events.size() <= 32) {
    std::sort(events.begin(), events.end(), [&](const auto& x, const auto& y) {
      work->tick();
      return x.coordinate < y.coordinate;
    });
  } else {
    auto sorted = radix_internal::sort(
        &events, 1, [](const Event& event, auto) { return event.coordinate; },
        [&](auto count) {
          work->tick(count);
          return Status::success();
        },
        work->limits.cancellation);
    if (!sorted.ok())
      throw Stop{std::move(sorted)};
  }
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
  bool first = true;
  for (std::size_t i = 0; i < events.size();) {
    const auto lo = events[i].coordinate;
    do {
      work->tick();
      const auto event = events[i++];
      auto& set = active[event.side];
      auto& position = positions[event.side];
      if (event.begin) {
        position[event.index] = set.size();
        set.push_back(event.index);
      } else {
        const auto at = position[event.index];
        set[at] = set.back();
        position[set[at]] = at;
        set.pop_back();
      }
    } while (i < events.size() && events[i].coordinate == lo);
    if (i == events.size())
      break;
    const auto hi = events[i].coordinate;
    Boxes suffix;
    if (axis + 1 == rank) {
      if (selected(!active[0].empty(), !active[1].empty(), operation))
        suffix.push_back(Box{});
    } else {
      Boxes left, right;
      for (const auto index : active[0]) {
        work->tick();
        left.push_back(a[index]);
      }
      for (const auto index : active[1]) {
        work->tick();
        right.push_back(b[index]);
      }
      suffix = sweep(left, right, axis + 1, rank, operation, work);
    }
    if (!first && equal_boxes(previous, suffix)) {
      end = hi;
    } else {
      flush();
      previous = std::move(suffix);
      start = lo;
      end = hi;
    }
    first = false;
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

Result<Normalized> combine(const std::vector<std::uint64_t>& shape,
                           const std::vector<Region>& a,
                           const std::vector<Region>& b, Combine operation,
                           const FootprintLimits& limits,
                           std::uint64_t* consumed = nullptr,
                           std::uint64_t retained_bytes = 0) {
  if (consumed)
    *consumed = 0;
  auto status = shape_status(shape);
  if (!status.ok())
    return Result<Normalized>(status);
  try {
    Work work(limits, consumed);
    Boxes left, right;
    for (int operand = 0; operand < 2; ++operand) {
      const auto* regions = operand == 0 ? &a : &b;
      auto& target = operand == 0 ? left : right;
      for (const auto& region : *regions) {
        work.tick();
        status = region.validate(shape);
        if (!status.ok())
          return Result<Normalized>(status);
        if (!region.empty()) {
          work.capacity(target.size());
          target.emplace_back(region.dimensions().begin(),
                              region.dimensions().end());
        }
      }
    }
    work.check();
    // A validated single rectangle is already canonical. Projection creates
    // these frequently; a multidimensional sweep adds no geometry information.
    auto normalized =
        operation == Combine::Union && right.empty() && left.size() == 1
            ? std::move(left)
            : sweep(left, right, 0, shape.size(), operation, &work);
    Normalized result;
    if (auto* root = resource_internal::metadata_budget()) {
      auto bytes = retained_bytes + normalized.size() * sizeof(Region);
      for (const auto& box : normalized)
        bytes += box.size() * sizeof(RegionDimension);
      auto admitted = root->reserve(ResourceCapacity::host(bytes, bytes));
      if (!admitted.ok())
        return Result<Normalized>(admitted.status());
      result.lease = admitted.take_value();
    }
    result.boxes.reserve(normalized.size());
    for (auto& box : normalized)
      result.boxes.emplace_back(
          std::vector<RegionDimension>(box.begin(), box.end()));
    return Result<Normalized>(std::move(result));
  } catch (const Stop& stop) {
    return Result<Normalized>(stop.status);
  } catch (const std::bad_alloc&) {
    return Result<Normalized>(Status{ErrorCode::ResourceExhausted, {}});
  }
}
}  // namespace

struct Footprint::Impl {
  ResourceLease lease;
  std::optional<ResourceBudget> budget;
  std::vector<std::uint64_t> shape;
  std::vector<Region> boxes;
};
const std::vector<std::uint64_t>& Footprint::shape() const noexcept {
  static const std::vector<std::uint64_t> empty;
  return impl_ ? impl_->shape : empty;
}
const std::vector<Region>& Footprint::boxes() const noexcept {
  static const std::vector<Region> empty;
  return impl_ ? impl_->boxes : empty;
}
Result<Footprint> Footprint::make(std::vector<std::uint64_t> shape,
                                  std::vector<Region> boxes,
                                  ResourceLease lease) {
  try {
    if (!lease.valid()) {
      if (auto* root = resource_internal::metadata_budget()) {
        std::uint64_t bytes = sizeof(Impl) +
                              shape.capacity() * sizeof(std::uint64_t) +
                              boxes.capacity() * sizeof(Region);
        for (const auto& box : boxes)
          bytes += box.rank() * sizeof(RegionDimension);
        auto admitted = root->reserve(ResourceCapacity::host(bytes, bytes));
        if (!admitted.ok())
          return Result<Footprint>(admitted.status());
        lease = admitted.take_value();
      }
    }
    auto storage = std::make_shared<Impl>();
    if (auto* root = resource_internal::metadata_budget())
      storage->budget = *root;
    storage->lease = std::move(lease);
    storage->shape = std::move(shape);
    storage->boxes = std::move(boxes);
    Footprint result;
    result.impl_ = std::move(storage);
    return Result<Footprint>(std::move(result));
  } catch (const std::bad_alloc&) {
    return Result<Footprint>(Status{ErrorCode::ResourceExhausted, {}});
  }
}
Result<Footprint> Footprint::from_regions(std::vector<std::uint64_t> shape,
                                          const std::vector<Region>& boxes,
                                          const FootprintLimits& limits,
                                          std::uint64_t* consumed_work) {
  auto normalized = combine(shape, boxes, {}, Combine::Union, limits,
                            consumed_work, sizeof(Impl) + shape.capacity() * 8);
  if (!normalized.ok())
    return Result<Footprint>(normalized.status());
  auto result = normalized.take_value();
  return make(std::move(shape), std::move(result.boxes),
              std::move(result.lease));
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
  if (!valid() || coordinate.size() != this->shape().size())
    return false;
  return containing_box(boxes(), coordinate.data(), coordinate.size()) !=
         nullptr;
}
Result<Footprint> Footprint::unite(const Footprint& other,
                                   const FootprintLimits& limits) const {
  auto remaining = limits;
  std::optional<ResourceAllocationScope> scope;
  if (impl_ && impl_->budget && !resource_internal::metadata_budget())
    scope.emplace(*impl_->budget);
  if (this->shape() != other.shape())
    return Result<Footprint>(Status::failure(ErrorCode::InvalidArgument,
                                             "footprint domain mismatch"));
  if (valid() && other.valid()) {
    for (const auto& pair :
         {std::make_pair(this, &other), std::make_pair(&other, this)}) {
      if (pair.first->boxes().size() != 1)
        continue;
      auto contained =
          contains_rectangle(*pair.second, pair.first->boxes()[0], &remaining);
      if (!contained.ok())
        return Result<Footprint>(contained.status());
      if (contained.value() &&
          pair.second->boxes().size() <= limits.maximum_boxes)
        return Result<Footprint>(*pair.second);
    }
  }
  auto boxes =
      combine(this->shape(), this->boxes(), other.boxes(), Combine::Union,
              remaining, nullptr, sizeof(Impl) + shape().size() * 8);
  if (!boxes.ok())
    return Result<Footprint>(boxes.status());
  auto normalized = boxes.take_value();
  return make(shape(), std::move(normalized.boxes),
              std::move(normalized.lease));
}
Result<Footprint> Footprint::intersect(const Footprint& other,
                                       const FootprintLimits& limits) const {
  auto remaining = limits;
  std::optional<ResourceAllocationScope> scope;
  if (impl_ && impl_->budget && !resource_internal::metadata_budget())
    scope.emplace(*impl_->budget);
  if (this->shape() != other.shape())
    return Result<Footprint>(Status::failure(ErrorCode::InvalidArgument,
                                             "footprint domain mismatch"));
  if (valid() && other.valid()) {
    for (const auto& pair :
         {std::make_pair(this, &other), std::make_pair(&other, this)}) {
      if (pair.first->boxes().size() != 1)
        continue;
      auto contained =
          contains_rectangle(*pair.second, pair.first->boxes()[0], &remaining);
      if (!contained.ok())
        return Result<Footprint>(contained.status());
      if (contained.value() &&
          pair.first->boxes().size() <= limits.maximum_boxes)
        return Result<Footprint>(*pair.first);
    }
  }
  auto boxes = combine(this->shape(), this->boxes(), other.boxes(),
                       Combine::Intersection, remaining, nullptr,
                       sizeof(Impl) + shape().size() * 8);
  if (!boxes.ok())
    return Result<Footprint>(boxes.status());
  auto normalized = boxes.take_value();
  return make(shape(), std::move(normalized.boxes),
              std::move(normalized.lease));
}
Result<Footprint> Footprint::subtract(const Footprint& other,
                                      const FootprintLimits& limits) const {
  auto remaining = limits;
  std::optional<ResourceAllocationScope> scope;
  if (impl_ && impl_->budget && !resource_internal::metadata_budget())
    scope.emplace(*impl_->budget);
  if (this->shape() != other.shape())
    return Result<Footprint>(Status::failure(ErrorCode::InvalidArgument,
                                             "footprint domain mismatch"));
  if (valid() && other.valid() && boxes().size() == 1) {
    auto contained = contains_rectangle(other, boxes()[0], &remaining);
    if (!contained.ok())
      return Result<Footprint>(contained.status());
    if (contained.value())
      return Footprint::none(shape(), remaining);
  }
  auto boxes =
      combine(this->shape(), this->boxes(), other.boxes(), Combine::Difference,
              remaining, nullptr, sizeof(Impl) + shape().size() * 8);
  if (!boxes.ok())
    return Result<Footprint>(boxes.status());
  auto normalized = boxes.take_value();
  return make(shape(), std::move(normalized.boxes),
              std::move(normalized.lease));
}
bool Footprint::operator==(const Footprint& other) const noexcept {
  if (this->shape() != other.shape() ||
      this->boxes().size() != other.boxes().size())
    return false;
  for (std::size_t i = 0; i < this->boxes().size(); ++i)
    for (std::size_t axis = 0; axis < this->shape().size(); ++axis) {
      const auto a = this->boxes()[i].dimensions()[axis];
      const auto b = other.boxes()[i].dimensions()[axis];
      if (a.offset != b.offset || a.extent != b.extent)
        return false;
    }
  return true;
}
Result<std::uint64_t> Footprint::element_count() const {
  if (!valid())
    return Result<std::uint64_t>(shape_status(this->shape()));
  std::uint64_t sum = 0;
  for (const auto& box : this->boxes()) {
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
  std::optional<ResourceAllocationScope> scope;
  if (impl_ && impl_->budget && !resource_internal::metadata_budget())
    scope.emplace(*impl_->budget);
  if (!valid() || geometry.size() != this->shape().size() ||
      !shape_status(geometry).ok())
    return Result<Footprint>(
        Status::failure(ErrorCode::InvalidArgument, "invalid tile geometry"));
  ResourceLease bridge;
  if (const auto* root = resource_internal::metadata_budget()) {
    const auto bytes =
        this->boxes().size() *
            (sizeof(Region) + this->shape().size() * sizeof(RegionDimension)) +
        this->shape().size() * 8;
    auto admitted = root->reserve(ResourceCapacity::host(bytes, bytes));
    if (!admitted.ok())
      return Result<Footprint>(admitted.status());
    bridge = admitted.take_value();
  }
  std::vector<std::uint64_t> shape;
  shape.reserve(this->shape().size());
  for (std::size_t axis = 0; axis < this->shape().size(); ++axis)
    shape.push_back(this->shape()[axis] / geometry[axis] +
                    (this->shape()[axis] % geometry[axis] != 0));
  std::vector<Region> boxes;
  boxes.reserve(this->boxes().size());
  try {
    Work work(limits);
    for (const auto& box : this->boxes()) {
      work.tick();
      work.capacity(boxes.size());
      Box dimensions;
      for (std::size_t axis = 0; axis < this->shape().size(); ++axis) {
        const auto d = box.dimensions()[axis];
        const auto first = d.offset / geometry[axis];
        const auto last = (d.offset + d.extent - 1) / geometry[axis];
        dimensions.push_back({first, last - first + 1});
      }
      boxes.emplace_back(
          std::vector<RegionDimension>(dimensions.begin(), dimensions.end()));
    }
    auto remaining = limits;
    remaining.maximum_work = work.remaining;
    return from_regions(std::move(shape), boxes, remaining);
  } catch (const Stop& stop) {
    return Result<Footprint>(stop.status);
  } catch (const std::bad_alloc&) {
    return Result<Footprint>(Status{ErrorCode::ResourceExhausted, {}});
  }
}
Status Footprint::visit(
    const std::function<Status(const std::vector<std::uint64_t>&)>& visitor,
    std::uint64_t maximum_samples,
    const CancellationToken& cancellation) const {
  std::optional<ResourceAllocationScope> scope;
  if (impl_ && impl_->budget && !resource_internal::metadata_budget())
    scope.emplace(*impl_->budget);
  if (!valid() || !visitor)
    return Status::failure(ErrorCode::InvalidArgument,
                           "invalid footprint visit");
  if (cancellation.cancelled())
    return Status::failure(ErrorCode::Cancelled, "footprint visit cancelled");
  try {
    struct Cursor {
      std::size_t box;
      ResourceVector<std::uint64_t> coordinate;
    };
    auto greater = [](const Cursor& a, const Cursor& b) {
      return a.coordinate > b.coordinate;
    };
    std::priority_queue<Cursor, ResourceVector<Cursor>, decltype(greater)>
        ready(greater);
    for (std::size_t i = 0; i < this->boxes().size(); ++i) {
      ResourceVector<std::uint64_t> coordinate;
      for (auto d : this->boxes()[i].dimensions())
        coordinate.push_back(d.offset);
      ready.push({i, std::move(coordinate)});
    }
    while (!ready.empty()) {
      if (cancellation.cancelled())
        return Status::failure(ErrorCode::Cancelled,
                               "footprint visit cancelled");
      if (!maximum_samples)
        return Status::failure(ErrorCode::ResourceExhausted,
                               "footprint visit limit");
      --maximum_samples;
      if (auto* root = resource_internal::metadata_budget()) {
        auto work = root->consume({1});
        if (!work.ok())
          return work;
      }
      auto cursor = ready.top();
      ready.pop();
      ResourceLease coordinate_lease;
      if (auto* root = resource_internal::metadata_budget()) {
        const auto bytes = cursor.coordinate.size() * sizeof(std::uint64_t);
        auto admission = root->reserve(ResourceCapacity::host(bytes, bytes));
        if (!admission.ok())
          return admission.status();
        coordinate_lease = admission.take_value();
      }
      auto status = visitor(std::vector<std::uint64_t>(
          cursor.coordinate.begin(), cursor.coordinate.end()));
      if (!status.ok())
        return status;
      for (std::size_t axis = this->shape().size(); axis-- > 0;) {
        const auto d = this->boxes()[cursor.box].dimensions()[axis];
        ++cursor.coordinate[axis];
        if (cursor.coordinate[axis] < d.offset + d.extent) {
          ready.push(std::move(cursor));
          break;
        }
        cursor.coordinate[axis] = d.offset;
      }
    }
    return Status::success();
  } catch (const std::bad_alloc&) {
    return Status{ErrorCode::ResourceExhausted, {}};
  }
}
}  // namespace ps
