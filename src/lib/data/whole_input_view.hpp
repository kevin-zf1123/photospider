#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <utility>
#include <vector>

#include "photospider/data/value_fragments.hpp"

namespace ps::input_internal {
// A Whole view may join compatible fragments of the same owner. Prove the
// affine address map at each rectangular piece, not by enumerating its pixels.
inline Result<std::optional<Value>> whole_input_view(
    const ValueFragments& values, const FootprintLimits& limits) {
  using Answer = Result<std::optional<Value>>;
  const auto& shape = values.descriptor().shape;
  const auto& parts = values.fragments();
  if (parts.empty() || values.coverage().boxes().size() != 1)
    return Answer(std::optional<Value>{});
  const auto& domain = values.coverage().boxes()[0];
  for (std::size_t j = 0; j < shape.size(); ++j)
    if (domain.dimensions()[j].offset ||
        domain.dimensions()[j].extent != shape[j])
      return Answer(std::optional<Value>{});
  std::uint64_t spent = 0;
  const auto work = [&](std::uint64_t amount) {
    if (limits.cancellation.cancelled())
      return Status{ErrorCode::Cancelled, {}};
    if (amount > limits.maximum_work - spent)
      return Status{ErrorCode::ResourceExhausted, {}, FailureReason::WorkLimit};
    spent += amount;
    return limits.consume_work ? limits.consume_work(amount)
                               : Status::success();
  };
  const auto owner = parts[0].storage();
  for (const auto& part : parts) {
    auto status = work(1);
    if (!status.ok())
      return Answer(status);
    if (part.storage() != owner)
      return Answer(std::optional<Value>{});
  }
  if (parts.size() == 1) {
    auto view = parts[0].view(domain);
    return view.ok() ? Answer(std::optional<Value>{view.take_value()})
                     : Answer(view.status());
  }
  const auto address =
      [&](const std::vector<std::uint64_t>& at) -> Result<std::size_t> {
    for (const auto& part : parts) {
      auto status = work(shape.size());
      if (!status.ok())
        return Result<std::size_t>(status);
      bool contains = true;
      for (std::size_t j = 0; j < shape.size(); ++j) {
        const auto dim = part.region().dimensions()[j];
        contains &= at[j] >= dim.offset && at[j] - dim.offset < dim.extent;
      }
      if (contains)
        return part.byte_address(at);
    }
    return Result<std::size_t>(
        Status{ErrorCode::Internal, "Whole view coverage hole"});
  };
  std::vector<std::uint64_t> point(shape.size(), 0);
  auto base = address(point);
  if (!base.ok())
    return Answer(base.status());
  std::vector<std::int64_t> strides(shape.size(), 0);
  for (std::size_t j = 0; j < shape.size(); ++j) {
    if (shape[j] == 1)
      continue;
    point[j] = 1;
    auto next = address(point);
    point[j] = 0;
    if (!next.ok())
      return Answer(next.status());
    const __int128 stride = static_cast<__int128>(next.value()) - base.value();
    if (stride < INT64_MIN || stride > INT64_MAX)
      return Answer(std::optional<Value>{});
    strides[j] = static_cast<std::int64_t>(stride);
  }
  for (const auto& part : parts) {
    auto status = work(shape.size());
    if (!status.ok())
      return Answer(status);
    __int128 expected = base.value();
    for (std::size_t j = 0; j < shape.size(); ++j) {
      const auto dim = part.region().dimensions()[j];
      point[j] = dim.offset;
      expected += static_cast<__int128>(dim.offset) * strides[j];
      if (dim.extent > 1 && part.layout().byte_strides[j] != strides[j])
        return Answer(std::optional<Value>{});
    }
    auto actual = part.byte_address(point);
    if (!actual.ok())
      return Answer(actual.status());
    if (expected != actual.value())
      return Answer(std::optional<Value>{});
  }
  auto view = Value::from_storage(values.descriptor(), domain,
                                  {base.value(), std::move(strides)}, owner,
                                  values.facets(), values.resources());
  return view.ok() ? Answer(std::optional<Value>{view.take_value()})
                   : Answer(view.status());
}
}  // namespace ps::input_internal
