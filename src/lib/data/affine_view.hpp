#pragma once

#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

#include "core/resource_observation.hpp"
#include "photospider/core/checked_math.hpp"
#include "photospider/data/tensor.hpp"

namespace ps::input_internal {
// Prove one affine address map over an already certified rectangle. Singleton
// pieces do not prescribe a stride; neighboring logical samples infer it.
// Caller admits rank-bounded standard-container scratch before entry; this
// helper counts its arrays until the returned span owns independent metadata.
template <class Parts>
Result<std::optional<TensorSpan>> join_affine_view(
    const TensorDescriptor& descriptor, const Region& domain,
    const Parts& parts, const FootprintLimits& limits,
    const ResourceLease& scratch) {
  using Answer = Result<std::optional<TensorSpan>>;
  if (parts.empty())
    return Answer(std::optional<TensorSpan>{});
  uint64_t spent = 0;
  const auto work = [&](uint64_t amount) {
    if (limits.cancellation.cancelled())
      return Status{ErrorCode::Cancelled, {}};
    if (!core_internal::can_add(amount, spent, limits.maximum_work))
      return Status{ErrorCode::ResourceExhausted, {}, FailureReason::WorkLimit};
    spent += amount;
    return limits.consume_work ? limits.consume_work(amount)
                               : Status::success();
  };
  const auto owner = parts.front().storage();
  for (const auto& part : parts) {
    auto charged = work(1);
    if (!charged.ok())
      return Answer(charged);
    if (part.storage() != owner)
      return Answer(std::optional<TensorSpan>{});
  }
  if (parts.size() == 1) {
    auto view = parts.front().view(domain);
    return view.ok() ? Answer(std::optional<TensorSpan>{view.take_value()})
                     : Answer(view.status());
  }
  const auto address = [&](const std::vector<uint64_t>& at) -> Result<size_t> {
    for (const auto& part : parts) {
      auto charged = work(at.size());
      if (!charged.ok())
        return Result<size_t>(charged);
      bool contains = true;
      for (size_t axis = 0; axis < at.size(); ++axis) {
        const auto d = part.region().dimensions()[axis];
        contains =
            contains && at[axis] >= d.offset && at[axis] - d.offset < d.extent;
      }
      if (contains)
        return part.byte_address(at);
    }
    return Result<size_t>(
        Status{ErrorCode::Internal, "certified affine window has a hole"});
  };
  core_internal::MetadataAllocation allocation(scratch);
  std::vector<uint64_t> point;
  point.reserve(domain.rank());
  allocation.commit(point.capacity() * sizeof(uint64_t));
  for (auto d : domain.dimensions())
    point.push_back(d.offset);
  StridedLayout layout;
  layout.origin = point;
  allocation.commit(layout.origin.capacity() * sizeof(uint64_t));
  auto base = address(point);
  if (!base.ok())
    return Answer(base.status());
  layout.byte_offset = base.value();
  layout.byte_strides.resize(domain.rank(), 0);
  allocation.commit(layout.byte_strides.capacity() * sizeof(int64_t));
  for (size_t axis = 0; axis < domain.rank(); ++axis) {
    if (domain.dimensions()[axis].extent == 1)
      continue;
    ++point[axis];
    auto next = address(point);
    --point[axis];
    if (!next.ok())
      return Answer(next.status());
    const auto stride = static_cast<__int128>(next.value()) - base.value();
    if (stride < INT64_MIN || stride > INT64_MAX)
      return Answer(std::optional<TensorSpan>{});
    layout.byte_strides[axis] = static_cast<int64_t>(stride);
  }
  for (const auto& part : parts) {
    auto charged = work(domain.rank());
    if (!charged.ok())
      return Answer(charged);
    __int128 expected = base.value();
    for (size_t axis = 0; axis < domain.rank(); ++axis) {
      const auto d = part.region().dimensions()[axis];
      point[axis] = d.offset;
      const auto term = static_cast<__int128>(d.offset - layout.origin[axis]) *
                        layout.byte_strides[axis];
      if (__builtin_add_overflow(expected, term, &expected))
        return Answer(std::optional<TensorSpan>{});
      if (d.extent > 1 &&
          part.layout().byte_strides[axis] != layout.byte_strides[axis])
        return Answer(std::optional<TensorSpan>{});
    }
    auto actual = part.byte_address(point);
    if (!actual.ok())
      return Answer(actual.status());
    if (expected != actual.value())
      return Answer(std::optional<TensorSpan>{});
  }
  auto joined = TensorSpan::from_storage(descriptor, domain, layout, owner);
  return joined.ok() ? Answer(std::optional<TensorSpan>{joined.take_value()})
                     : Answer(joined.status());
}
}  // namespace ps::input_internal
