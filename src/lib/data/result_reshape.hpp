#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <utility>
#include <vector>

#include "photospider/core/resource_allocator.hpp"
#include "photospider/data/footprint.hpp"

namespace ps::reshape_internal {
// Eight uint64 axes fit in 512 bits. Ordinals never depend on dense byte size.
struct Ordinal {
  std::array<std::uint64_t, 8> limb{};
  explicit Ordinal(std::uint64_t n = 0) { limb[0] = n; }
  int compare(const Ordinal& other) const noexcept {
    for (size_t i = limb.size(); i-- > 0;)
      if (limb[i] != other.limb[i])
        return limb[i] < other.limb[i] ? -1 : 1;
    return 0;
  }
  void multiply(std::uint64_t n) noexcept {
    unsigned __int128 carry = 0;
    for (auto& v : limb) {
      const auto product = static_cast<unsigned __int128>(v) * n + carry;
      v = static_cast<std::uint64_t>(product);
      carry = product >> 64;
    }
  }
  void add(const Ordinal& other) noexcept {
    unsigned __int128 carry = 0;
    for (size_t i = 0; i < limb.size(); ++i) {
      const auto sum =
          static_cast<unsigned __int128>(limb[i]) + other.limb[i] + carry;
      limb[i] = static_cast<std::uint64_t>(sum);
      carry = sum >> 64;
    }
  }
  void decrement() noexcept {
    for (auto& v : limb) {
      if (v--)
        break;
    }
  }
  std::uint64_t divide(std::uint64_t divisor) noexcept {
    unsigned __int128 remainder = 0;
    for (size_t i = limb.size(); i-- > 0;) {
      const auto value = (remainder << 64) | limb[i];
      limb[i] = static_cast<std::uint64_t>(value / divisor);
      remainder = value % divisor;
    }
    return static_cast<std::uint64_t>(remainder);
  }
};
using Box = std::array<RegionDimension, 8>;
struct Axis {
  size_t index = 0;
  std::uint64_t extent = 1;
};
struct Projection {
  const ResourceBudget& budget;
  const FootprintLimits& limits;
  std::uint64_t remaining;
  ResourceVector<Box> boxes;
  Projection(const ResourceBudget& root, const FootprintLimits& bound)
      : budget(root),
        limits(bound),
        remaining(bound.maximum_work),
        boxes(ResourceAllocator<Box>(root)) {}
  Status tick(size_t n = 1) {
    if (limits.cancellation.cancelled())
      return {ErrorCode::Cancelled, {}};
    if (remaining < n)
      return {ErrorCode::ResourceExhausted, "reshape projection work limit"};
    remaining -= n;
    return limits.consume_work ? limits.consume_work(n) : budget.consume({n});
  }
  Status append(const Box& box) {
    auto status = tick();
    if (!status.ok())
      return status;
    if (boxes.size() >= limits.maximum_boxes)
      return {ErrorCode::ResourceExhausted,
              "reshape projection rectangle limit"};
    boxes.push_back(box);
    return Status::success();
  }
};
inline Status decode_interval(Projection& result,
                              const std::array<Axis, 8>& target, size_t begin,
                              size_t end, const std::array<uint64_t, 8>& first,
                              const std::array<uint64_t, 8>& last, Box prefix) {
  auto status = result.tick();
  if (!status.ok())
    return status;
  if (begin == end)
    return result.append(prefix);
  bool whole = true;
  for (size_t i = begin; i < end; ++i)
    whole &= first[i] == 0 && last[i] == target[i].extent - 1;
  if (whole) {
    for (size_t i = begin; i < end; ++i)
      prefix[target[i].index] = {0, target[i].extent};
    return result.append(prefix);
  }
  const auto axis = target[begin].index;
  if (first[begin] == last[begin]) {
    prefix[axis] = {first[begin], 1};
    return decode_interval(result, target, begin + 1, end, first, last, prefix);
  }
  bool leading = false, trailing = false;
  for (size_t i = begin + 1; i < end; ++i) {
    leading |= first[i] != 0;
    trailing |= last[i] != target[i].extent - 1;
  }
  if (leading) {
    auto upper = last;
    for (size_t i = begin + 1; i < end; ++i)
      upper[i] = target[i].extent - 1;
    prefix[axis] = {first[begin], 1};
    status =
        decode_interval(result, target, begin + 1, end, first, upper, prefix);
    if (!status.ok())
      return status;
  }
  const auto middle_first = first[begin] + static_cast<uint64_t>(leading);
  const auto middle_end = last[begin] + static_cast<uint64_t>(!trailing);
  if (middle_end > middle_first) {
    prefix[axis] = {middle_first, middle_end - middle_first};
    for (size_t i = begin + 1; i < end; ++i)
      prefix[target[i].index] = {0, target[i].extent};
    status = result.append(prefix);
    if (!status.ok())
      return status;
  }
  if (trailing) {
    auto lower = first;
    for (size_t i = begin + 1; i < end; ++i)
      lower[i] = 0;
    prefix[axis] = {last[begin], 1};
    return decode_interval(result, target, begin + 1, end, lower, last, prefix);
  }
  return Status::success();
}
inline Status project_block(Projection& result, const Region& box,
                            const std::array<Axis, 8>& from, size_t fb,
                            size_t fe, const std::array<Axis, 8>& to, size_t tb,
                            size_t te) {
  size_t prefix_end = fe;
  while (prefix_end > fb) {
    const auto d = box.dimensions()[from[prefix_end - 1].index];
    if (d.offset || d.extent != from[prefix_end - 1].extent)
      break;
    --prefix_end;
  }
  // The last restricted axis is one contiguous span; earlier axes enumerate
  // only within this independent reassociation block, never shared outer axes.
  const size_t span_axis = prefix_end == fb ? fb : prefix_end - 1;
  std::array<uint64_t, 8> coordinate{};
  for (size_t i = fb; i < fe; ++i)
    coordinate[i] = box.dimensions()[from[i].index].offset;
  Ordinal length(box.dimensions()[from[span_axis].index].extent);
  for (size_t i = span_axis + 1; i < fe; ++i)
    length.multiply(from[i].extent);
  for (;;) {
    auto status = result.tick(fe - fb + te - tb);
    if (!status.ok())
      return status;
    Ordinal lower;
    for (size_t i = fb; i < fe; ++i) {
      lower.multiply(from[i].extent);
      lower.add(Ordinal(coordinate[i]));
    }
    auto upper = lower;
    upper.add(length);
    upper.decrement();
    std::array<uint64_t, 8> first{}, last{};
    for (size_t i = te; i-- > tb;) {
      first[i] = lower.divide(to[i].extent);
      last[i] = upper.divide(to[i].extent);
    }
    Box base{};
    for (auto& d : base)
      d = {0, 1};
    status = decode_interval(result, to, tb, te, first, last, base);
    if (!status.ok())
      return status;
    bool advanced = false;
    for (size_t i = span_axis; i-- > fb;) {
      const auto d = box.dimensions()[from[i].index];
      if (++coordinate[i] < d.offset + d.extent) {
        advanced = true;
        break;
      }
      coordinate[i] = d.offset;
    }
    if (!advanced)
      return Status::success();
  }
}
inline bool equal_products(const std::vector<uint64_t>& a,
                           const std::vector<uint64_t>& b) {
  Ordinal left(1), right(1);
  for (auto n : a) {
    if (!n)
      return false;
    left.multiply(n);
  }
  for (auto n : b) {
    if (!n)
      return false;
    right.multiply(n);
  }
  return left.compare(right) == 0;
}
// Saturated rectangle bound from the same independent blocks and full suffix
// decomposition as project(). Logical sample count cannot estimate this cost.
inline uint64_t projection_cost(const std::vector<uint64_t>& from_shape,
                                const Footprint& requested,
                                const std::vector<uint64_t>& to_shape,
                                uint64_t cap) {
  const auto multiply = [cap](uint64_t a, uint64_t b) {
    return !b || a <= cap / b ? a * b : cap;
  };
  std::array<Axis, 8> from{}, to{};
  size_t nf = 0, nt = 0;
  for (size_t i = 0; i < from_shape.size(); ++i)
    if (from_shape[i] != 1)
      from[nf++] = {i, from_shape[i]};
  for (size_t i = 0; i < to_shape.size(); ++i)
    if (to_shape[i] != 1)
      to[nt++] = {i, to_shape[i]};
  uint64_t total = 0;
  for (const auto& query : requested.boxes()) {
    uint64_t pieces = 1;
    size_t f = 0, t = 0;
    while (f < nf && t < nt) {
      const auto fb = f, tb = t;
      Ordinal fp(from[f++].extent), tp(to[t++].extent);
      while (fp.compare(tp)) {
        if (fp.compare(tp) < 0)
          fp.multiply(from[f++].extent);
        else
          tp.multiply(to[t++].extent);
      }
      auto prefix_end = f;
      while (prefix_end > fb) {
        const auto d = query.dimensions()[from[prefix_end - 1].index];
        if (d.offset || d.extent != from[prefix_end - 1].extent)
          break;
        --prefix_end;
      }
      const auto span_axis = prefix_end == fb ? fb : prefix_end - 1;
      uint64_t spans = 1;
      for (size_t i = fb; i < span_axis; ++i)
        spans = multiply(spans, query.dimensions()[from[i].index].extent);
      const auto rectangles = prefix_end == fb ? 1 : 2 * (t - tb) - 1;
      pieces = multiply(pieces, multiply(spans, rectangles));
    }
    if (pieces >= cap - total)
      return cap;
    total += pieces;
  }
  return total;
}
inline Result<Footprint> project(const ResourceBudget& budget,
                                 const std::vector<uint64_t>& from_shape,
                                 const Footprint& requested,
                                 const std::vector<uint64_t>& to_shape,
                                 const FootprintLimits& limits) {
  using Answer = Result<Footprint>;
  if (from_shape.empty() || from_shape.size() > 8 || to_shape.empty() ||
      to_shape.size() > 8 || requested.shape() != from_shape ||
      !equal_products(from_shape, to_shape))
    return Answer(Status{ErrorCode::InvalidArgument,
                         "invalid reshape coordinate domains"});
  std::array<Axis, 8> from{}, to{};
  size_t nf = 0, nt = 0;
  for (size_t i = 0; i < from_shape.size(); ++i)
    if (from_shape[i] != 1)
      from[nf++] = {i, from_shape[i]};
  for (size_t i = 0; i < to_shape.size(); ++i)
    if (to_shape[i] != 1)
      to[nt++] = {i, to_shape[i]};
  Projection all(budget, limits);
  for (const auto& query : requested.boxes()) {
    ResourceVector<Box> combined{ResourceAllocator<Box>(budget)};
    Box base{};
    for (auto& d : base)
      d = {0, 1};
    combined.push_back(base);
    size_t f = 0, t = 0;
    while (f < nf && t < nt) {
      const auto fb = f, tb = t;
      Ordinal fp(from[f++].extent), tp(to[t++].extent);
      while (fp.compare(tp)) {
        if (fp.compare(tp) < 0) {
          if (f == nf)
            return Answer(
                Status{ErrorCode::InvalidArgument, "invalid reshape block"});
          fp.multiply(from[f++].extent);
        } else {
          if (t == nt)
            return Answer(
                Status{ErrorCode::InvalidArgument, "invalid reshape block"});
          tp.multiply(to[t++].extent);
        }
      }
      Projection block(budget, limits);
      block.remaining = all.remaining;
      auto status = project_block(block, query, from, fb, f, to, tb, t);
      all.remaining = block.remaining;
      if (!status.ok())
        return Answer(status);
      if (!block.boxes.empty() &&
          combined.size() > limits.maximum_boxes / block.boxes.size())
        return Answer(Status{ErrorCode::ResourceExhausted,
                             "reshape projection rectangle limit"});
      ResourceVector<Box> next{ResourceAllocator<Box>(budget)};
      next.reserve(combined.size() * block.boxes.size());
      for (const auto& prefix : combined)
        for (const auto& suffix : block.boxes) {
          status = all.tick();
          if (!status.ok())
            return Answer(status);
          auto mapped = prefix;
          for (size_t i = tb; i < t; ++i)
            mapped[to[i].index] = suffix[to[i].index];
          next.push_back(mapped);
        }
      combined = std::move(next);
    }
    for (const auto& box : combined) {
      auto status = all.append(box);
      if (!status.ok())
        return Answer(status);
    }
  }
  const auto bytes =
      all.boxes.size() *
      (sizeof(Region) + to_shape.size() * sizeof(RegionDimension));
  auto admitted = budget.reserve(ResourceCapacity::host(bytes, bytes));
  if (!admitted.ok())
    return Answer(admitted.status());
  std::vector<Region> regions;
  regions.reserve(all.boxes.size());
  for (const auto& box : all.boxes)
    regions.emplace_back(std::vector<RegionDimension>(
        box.begin(), box.begin() + to_shape.size()));
  return Footprint::from_regions(to_shape, regions, limits);
}
}  // namespace ps::reshape_internal
