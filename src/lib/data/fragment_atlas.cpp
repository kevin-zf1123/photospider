#include "photospider/data/fragment_atlas.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <stdexcept>
#include <utility>
#include <vector>

#include "data/result_window_access.hpp"
#include "photospider/plugin/result_program.hpp"

namespace ps {
namespace {
using Key = std::array<std::uint64_t, 8>;
struct Tile {
  Key key{};
  std::uint64_t mask = 0, offset = 0, slot = 0;
};
std::uint64_t hash_key(const Key& key, std::size_t rank) {
  std::uint64_t hash = UINT64_C(14695981039346656037);
  for (std::size_t axis = 0; axis < rank; ++axis)
    for (unsigned byte = 0; byte < 8; ++byte) {
      hash ^= (key[axis] >> (8 * byte)) & 255;
      hash *= UINT64_C(1099511628211);
    }
  return hash;
}
std::uint64_t load(const std::uint8_t* bytes) {
  std::uint64_t value = 0;
  for (unsigned i = 0; i < 8; ++i)
    value |= static_cast<std::uint64_t>(bytes[i]) << (8 * i);
  return value;
}
void store(std::uint8_t* bytes, std::uint64_t value) {
  for (unsigned i = 0; i < 8; ++i)
    bytes[i] = static_cast<std::uint8_t>(value >> (8 * i));
}
unsigned population(std::uint64_t mask) {
  unsigned count = 0;
  for (; mask; mask &= mask - 1)
    ++count;
  return count;
}
Status exhausted() {
  return Status{ErrorCode::ResourceExhausted, {}};
}
Status invalid() {
  return Status{ErrorCode::InvalidArgument, "invalid fragment atlas"};
}
struct Work {
  std::uint64_t left;
  CancellationToken cancel;
  std::function<Status(std::uint64_t)> charge = {};
  Status consume(std::uint64_t cost) {
    if (cancel.cancelled())
      return Status{ErrorCode::Cancelled, {}};
    if (cost > left)
      return exhausted();
    auto status = charge ? charge(cost) : Status::success();
    if (status.ok())
      left -= cost;
    return status;
  }
};
Result<MutableValue> allocate(std::uint64_t bytes,
                              const BufferAllocator& allocator) {
  return MutableValue::allocate({ElementType::UInt8, {bytes}},
                                Region::whole({bytes}), allocator);
}
}  // namespace
struct FragmentAtlasPlan::Impl {
  ResourceLease metadata;
  ValueDescriptor descriptor;
  Footprint coverage;
  std::vector<std::uint64_t> tile_shape;
  ResourceVector<Tile> tiles;
  std::uint64_t bytes = 0, slots = 2, prepared_work = 0, packing_work = 0;
};
Result<FragmentAtlasPlan> FragmentAtlasPlan::prepare(
    const ValueFragments& input, std::vector<std::uint64_t> geometry,
    const FootprintLimits& limits) {
  if (!input.valid())
    return Result<FragmentAtlasPlan>(invalid());
  auto local = limits;
  local.consume_work = {};
  return prepare_regions(input.descriptor(), input.coverage(),
                         std::move(geometry), local);
}
Result<FragmentAtlasPlan> FragmentAtlasPlan::prepare(
    const ResultTensorInput& input, std::vector<std::uint64_t> geometry,
    const FootprintLimits& limits) {
  if (!input.result_.valid())
    return Result<FragmentAtlasPlan>(invalid());
  return prepare_regions(
      {input.spec().descriptor.element_type, input.spec().sample_shape()},
      input.coverage(), std::move(geometry), limits);
}
Result<FragmentAtlasPlan> FragmentAtlasPlan::prepare_regions(
    const ValueDescriptor& descriptor, const Footprint& coverage,
    std::vector<std::uint64_t> geometry, const FootprintLimits& limits) {
  Work work{limits.maximum_work, limits.cancellation, limits.consume_work};
  auto status = work.consume(0);
  if (!status.ok())
    return Result<FragmentAtlasPlan>(status);
  if (!coverage.valid() || descriptor.shape != coverage.shape())
    return Result<FragmentAtlasPlan>(invalid());
  const auto rank = descriptor.shape.size();
  const auto raw = coverage.boxes().size();
  const auto scale = 1 + 2 * rank;
  if (raw > (UINT64_MAX - rank - 1) / scale)
    return Result<FragmentAtlasPlan>(exhausted());
  status = work.consume(1 + rank + raw * scale);
  if (!status.ok())
    return Result<FragmentAtlasPlan>(status);
  ResourceLease metadata;
  if (auto* root = resource_internal::metadata_budget()) {
    const auto bytes =
        (descriptor.shape.size() + std::max(geometry.capacity(), rank)) *
        sizeof(std::uint64_t);
    auto admitted = root->reserve(ResourceCapacity::host(bytes, bytes));
    if (!admitted.ok())
      return Result<FragmentAtlasPlan>(admitted.status());
    metadata = admitted.take_value();
  }
  const auto& shape = descriptor.shape;
  if (geometry.empty()) {
    geometry.resize(shape.size(), 1);
    std::uint64_t remaining = 64;
    for (std::size_t axis = shape.size(); axis; --axis) {
      geometry[axis - 1] = std::min(shape[axis - 1], remaining);
      remaining /= geometry[axis - 1];
    }
  }
  if (geometry.size() != shape.size())
    return Result<FragmentAtlasPlan>(invalid());
  std::uint64_t volume = 1;
  for (auto size : geometry) {
    if (!size || size > 64 / volume)
      return Result<FragmentAtlasPlan>(invalid());
    volume *= size;
  }
  auto count = coverage.element_count();
  const auto width = Value::element_size(descriptor.element_type);
  if (!count.ok() || count.value() > UINT64_MAX / width)
    return Result<FragmentAtlasPlan>(exhausted());
  status = work.consume(count.value());
  if (!status.ok())
    return Result<FragmentAtlasPlan>(status);
  std::map<Key, std::uint64_t, std::less<Key>,
           ResourceAllocator<std::pair<const Key, std::uint64_t>>>
      masks;
  status = coverage.visit(
      [&](const auto& at) {
        auto charged = work.consume(shape.size() + 1);
        if (!charged.ok())
          return charged;
        Key key{};
        std::uint64_t bit = 0;
        for (std::size_t axis = 0; axis < shape.size(); ++axis) {
          key[axis] = at[axis] / geometry[axis];
          bit = bit * geometry[axis] + at[axis] % geometry[axis];
        }
        auto found = masks.find(key);
        if (found == masks.end()) {
          if (masks.size() >= limits.maximum_boxes)
            return exhausted();
          found = masks.emplace(key, 0).first;
        }
        found->second |= UINT64_C(1) << bit;
        return Status::success();
      },
      count.value(), limits.cancellation);
  if (!status.ok())
    return Result<FragmentAtlasPlan>(status);
  auto plan = std::allocate_shared<Impl>(ResourceAllocator<Impl>{});
  plan->metadata = std::move(metadata);
  plan->descriptor = descriptor;
  plan->coverage = coverage;
  plan->tile_shape = std::move(geometry);
  plan->bytes = count.value() * width;
  if (masks.size() > UINT64_MAX / 2)
    return Result<FragmentAtlasPlan>(exhausted());
  while (plan->slots < masks.size() * 2) {
    if (plan->slots > UINT64_MAX / 2)
      return Result<FragmentAtlasPlan>(exhausted());
    plan->slots *= 2;
  }
  if (plan->slots > SIZE_MAX ||
      plan->slots > UINT64_MAX / kFragmentAtlasSlotBytes)
    return Result<FragmentAtlasPlan>(exhausted());
  const auto tile_work = 1 + shape.size() * 8;
  if (masks.size() > (UINT64_MAX - plan->slots) / tile_work)
    return Result<FragmentAtlasPlan>(exhausted());
  status = work.consume(plan->slots + masks.size() * tile_work);
  if (!status.ok())
    return Result<FragmentAtlasPlan>(status);
  ResourceVector<std::uint8_t> occupied(static_cast<std::size_t>(plan->slots),
                                        false);
  std::uint64_t offset = 0;
  for (const auto& entry : masks) {
    status = work.consume(0);
    if (!status.ok())
      return Result<FragmentAtlasPlan>(status);
    auto slot = hash_key(entry.first, shape.size()) & (plan->slots - 1);
    while (occupied[slot]) {
      status = work.consume(1);
      if (!status.ok())
        return Result<FragmentAtlasPlan>(status);
      slot = (slot + 1) & (plan->slots - 1);
    }
    occupied[slot] = true;
    plan->tiles.push_back({entry.first, entry.second, offset, slot});
    offset += population(entry.second) * width;
  }
  status = work.consume(0);
  if (!status.ok())
    return Result<FragmentAtlasPlan>(status);
  plan->prepared_work = limits.maximum_work - work.left;
  const auto add_cost = [&](std::uint64_t n, std::uint64_t factor) {
    if (n > (UINT64_MAX - plan->packing_work) / factor)
      return false;
    plan->packing_work += n * factor;
    return true;
  };
  if (!add_cost(1, 1 + rank) || !add_cost(raw, 2 * scale) ||
      !add_cost(plan->slots, 10) || !add_cost(plan->tiles.size(), 74) ||
      !add_cost(count.value(), shape.size() + width))
    return Result<FragmentAtlasPlan>(exhausted());
  FragmentAtlasPlan result;
  result.impl_ = std::move(plan);
  return Result<FragmentAtlasPlan>(std::move(result));
}
std::uint64_t FragmentAtlasPlan::payload_allocation_bytes() const {
  if (!impl_)
    throw std::logic_error("invalid fragment atlas plan");
  return std::max<std::uint64_t>(1, impl_->bytes);
}
std::uint64_t FragmentAtlasPlan::directory_allocation_bytes() const {
  if (!impl_)
    throw std::logic_error("invalid fragment atlas plan");
  return impl_->slots * kFragmentAtlasSlotBytes;
}
std::uint64_t FragmentAtlasPlan::occupied_tiles() const {
  if (!impl_)
    throw std::logic_error("invalid fragment atlas plan");
  return impl_->tiles.size();
}
std::uint64_t FragmentAtlasPlan::preparation_work() const {
  if (!impl_)
    throw std::logic_error("invalid fragment atlas plan");
  return impl_->prepared_work;
}
std::uint64_t FragmentAtlasPlan::materialization_work() const {
  if (!impl_)
    throw std::logic_error("invalid fragment atlas plan");
  return impl_->packing_work;
}
Result<FragmentAtlas> FragmentAtlasPlan::materialize(
    const ValueFragments& input, const BufferAllocator& allocator,
    const FootprintLimits& limits) const {
  if (!input.valid())
    return Result<FragmentAtlas>(invalid());
  auto local = limits;
  local.consume_work = {};
  return materialize_regions(
      input.descriptor(), input.coverage(),
      [&](const auto& at, void* destination, std::size_t bytes) {
        return input.read(at, destination, bytes);
      },
      allocator, local);
}
Result<FragmentAtlas> FragmentAtlasPlan::materialize(
    const ResultTensorInput& input, const BufferAllocator& allocator,
    const FootprintLimits& limits) const {
  using Answer = Result<FragmentAtlas>;
  if (!impl_ || !input.result_.valid())
    return Answer(invalid());
  if (!input.payload_authorized_)
    return Answer(input.read({}, nullptr, 0, limits.cancellation));
  Work work{limits.maximum_work, limits.cancellation, limits.consume_work};
  const auto rank = input.coverage().shape().size();
  const auto boxes = input.coverage().boxes().size();
  const auto per_box = 1 + 2 * rank;
  if (boxes > limits.maximum_boxes || boxes == UINT64_MAX ||
      boxes + 1 > UINT64_MAX / per_box ||
      boxes > (UINT64_MAX - 1) / ((boxes + 1) * per_box))
    return Answer(exhausted());
  auto status = work.consume(1 + boxes * (boxes + 1) * per_box);
  if (!status.ok())
    return Answer(status);
  ResourceVector<ResultTensorReadWindow> windows;
  std::uint64_t max_read_work = 0;
  for (const auto& box : input.coverage().boxes()) {
    auto window = input.acquire(box, limits.cancellation);
    if (!window.ok())
      return Answer(window.status());
    auto read_work =
        execution_internal::ResultWindowAccess::read_work(window.value());
    if (!read_work.ok())
      return Answer(read_work.status());
    max_read_work = std::max(max_read_work, read_work.value());
    windows.push_back(window.take_value());
  }
  const auto count = input.coverage().element_count();
  if (!count.ok() || boxes > (UINT64_MAX - max_read_work) / (rank + 1))
    return Answer(exhausted());
  const auto cost = max_read_work + boxes * (rank + 1);
  if (cost && count.value() > UINT64_MAX / cost)
    return Answer(exhausted());
  status = work.consume(count.value() * cost);
  if (!status.ok())
    return Answer(status);
  auto remaining = limits;
  remaining.maximum_work = work.left;
  return materialize_regions(
      {input.spec().descriptor.element_type, input.spec().sample_shape()},
      input.coverage(),
      [&](const auto& at, void* destination, std::size_t bytes) {
        for (const auto& window : windows) {
          bool contains = true;
          for (std::size_t axis = 0; axis < at.size(); ++axis) {
            const auto& span = window.region().dimensions()[axis];
            contains = contains && at[axis] >= span.offset &&
                       at[axis] - span.offset < span.extent;
          }
          if (!contains)
            continue;
          auto row = window.row_run(at);
          if (!row.ok())
            return row.status();
          std::memcpy(destination, row.value().data, bytes);
          return Status::success();
        }
        return invalid();
      },
      allocator, remaining);
}
Result<FragmentAtlas> FragmentAtlasPlan::materialize_regions(
    const ValueDescriptor& descriptor, const Footprint& coverage,
    const std::function<Status(const std::vector<std::uint64_t>&, void*,
                               std::size_t)>& read,
    const BufferAllocator& allocator, const FootprintLimits& limits) const {
  Work work{limits.maximum_work, limits.cancellation, limits.consume_work};
  auto status = work.consume(0);
  if (!status.ok())
    return Result<FragmentAtlas>(status);
  if (!impl_ || !coverage.valid())
    return Result<FragmentAtlas>(invalid());
  const auto rank = impl_->descriptor.shape.size();
  const auto left = impl_->coverage.boxes().size();
  const auto right = coverage.boxes().size();
  if (left > UINT64_MAX - right ||
      left + right > (UINT64_MAX - rank - 1) / (1 + 2 * rank))
    return Result<FragmentAtlas>(exhausted());
  status = work.consume(1 + rank + (left + right) * (1 + 2 * rank));
  if (!status.ok())
    return Result<FragmentAtlas>(status);
  if (descriptor.shape != impl_->descriptor.shape ||
      descriptor.element_type != impl_->descriptor.element_type ||
      coverage != impl_->coverage)
    return Result<FragmentAtlas>(invalid());
  const auto width = Value::element_size(impl_->descriptor.element_type);
  const auto metadata_work = 1 + rank + (left + right) * (1 + 2 * rank);
  status = work.consume(impl_->packing_work - metadata_work);
  if (!status.ok() || impl_->tiles.size() > limits.maximum_boxes)
    return Result<FragmentAtlas>(status.ok() ? exhausted() : status);
  auto payload = allocate(payload_allocation_bytes(), allocator);
  if (!payload.ok())
    return Result<FragmentAtlas>(payload.status());
  status = work.consume(0);
  if (!status.ok())
    return Result<FragmentAtlas>(status);
  auto directory = allocate(directory_allocation_bytes(), allocator);
  if (!directory.ok())
    return Result<FragmentAtlas>(directory.status());
  status = work.consume(0);
  if (!status.ok())
    return Result<FragmentAtlas>(status);
  auto data = payload.take_value(), index = directory.take_value();
  std::memset(index.data(), 0, index.size());
  data.data()[0] = 0;
  std::vector<std::uint64_t> at(impl_->descriptor.shape.size());
  for (const auto& tile : impl_->tiles) {
    auto* slot = index.data() + tile.slot * kFragmentAtlasSlotBytes;
    for (std::size_t axis = 0; axis < 8; ++axis)
      store(slot + 8 * axis, tile.key[axis]);
    store(slot + 64, tile.mask);
    store(slot + 72, tile.offset);
    auto offset = tile.offset;
    for (unsigned bit = 0; bit < 64; ++bit) {
      if (!(tile.mask & (UINT64_C(1) << bit)))
        continue;
      status = work.consume(0);
      if (!status.ok())
        return Result<FragmentAtlas>(status);
      auto local = static_cast<std::uint64_t>(bit);
      for (std::size_t axis = at.size(); axis; --axis) {
        const auto size = impl_->tile_shape[axis - 1];
        at[axis - 1] = tile.key[axis - 1] * size + local % size;
        local /= size;
      }
      status = read(at, data.data() + offset, width);
      if (!status.ok())
        return Result<FragmentAtlas>(status);
      offset += width;
    }
  }
  status = work.consume(0);
  if (!status.ok())
    return Result<FragmentAtlas>(status);
  auto packed = std::move(data).publish(), lookup = std::move(index).publish();
  if (!packed.ok() || !lookup.ok())
    return Result<FragmentAtlas>(packed.ok() ? lookup.status()
                                             : packed.status());
  FragmentAtlas result{impl_->descriptor,   impl_->tile_shape,
                       impl_->slots,        impl_->bytes,
                       packed.take_value(), lookup.take_value()};
  status = work.consume(0);
  if (!status.ok())
    return Result<FragmentAtlas>(status);
  return Result<FragmentAtlas>(std::move(result));
}
Result<std::uint64_t> FragmentAtlas::address(
    const std::vector<std::uint64_t>& at) const {
  if (at.empty() || at.size() > 8 ||
      static_cast<std::uint32_t>(descriptor.element_type) < 1 ||
      static_cast<std::uint32_t>(descriptor.element_type) > 7 ||
      at.size() != descriptor.shape.size() || tile_shape.size() != at.size() ||
      !payload.valid() || !directory.valid() || !slot_count ||
      (slot_count & (slot_count - 1)) ||
      slot_count > directory.bytes().size() / kFragmentAtlasSlotBytes ||
      payload_bytes > payload.bytes().size())
    return Result<std::uint64_t>(invalid());
  Key key{};
  std::uint64_t bit = 0, volume = 1;
  for (std::size_t axis = 0; axis < at.size(); ++axis) {
    if (!tile_shape[axis] || tile_shape[axis] > 64 / volume ||
        at[axis] >= descriptor.shape[axis])
      return Result<std::uint64_t>(invalid());
    volume *= tile_shape[axis];
    key[axis] = at[axis] / tile_shape[axis];
    bit = bit * tile_shape[axis] + at[axis] % tile_shape[axis];
  }
  auto slot = hash_key(key, at.size()) & (slot_count - 1);
  for (std::uint64_t probe = 0; probe < slot_count; ++probe) {
    const auto* entry =
        directory.bytes().data() + slot * kFragmentAtlasSlotBytes;
    const auto mask = load(entry + 64);
    if (!mask)
      break;
    bool same = true;
    for (std::size_t axis = 0; axis < 8; ++axis)
      same = same && load(entry + axis * 8) == key[axis];
    if (same) {
      if (!(mask & (UINT64_C(1) << bit)))
        break;
      const auto offset = load(entry + 72);
      const auto width = Value::element_size(descriptor.element_type);
      const auto prior = population(mask & ((UINT64_C(1) << bit) - 1));
      if (offset > payload_bytes || prior * width > payload_bytes - offset ||
          width > payload_bytes - offset - prior * width)
        return Result<std::uint64_t>(invalid());
      return Result<std::uint64_t>(offset + prior * width);
    }
    slot = (slot + 1) & (slot_count - 1);
  }
  return Result<std::uint64_t>(
      Status{ErrorCode::NotFound, "fragment atlas hole"});
}
}  // namespace ps
