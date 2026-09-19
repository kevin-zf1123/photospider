#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <string>

#include "01-numeric/exact_sampling.hpp"
#include "photospider/execution/resource_allocator.hpp"

namespace ps::plugin_internal::numeric_ops {
// Dynamic uniform-axis consumer. Knots are individually rounded weighted
// endpoints, not repeated step additions. All storage uses the active host
// resource allocator and belongs to the caller's continuation.
struct UniformAxis final {
  ExactSampling sampling;
  ResourceVector<std::uint64_t> knots;
  bool descending = false;
  explicit UniformAxis(SequenceProfile profile) : sampling(profile) {}
  std::uint64_t key(std::uint64_t bits) const {
    const auto ordered = BinaryParts::decode(bits, false).order_key();
    return descending ? UINT64_MAX - ordered : ordered;
  }
  Status validate(const std::array<std::uint64_t, 3>& axis, unsigned count,
                  const std::function<Status(std::uint64_t)>& consume) {
    const auto invalid = [](const std::string& message) {
      return Status{ErrorCode::OperationFailed,
                    message,
                    FailureReason::InvalidDomain,
                    {FailureOrigin::Domain, FailureScope::Atom}};
    };
    for (unsigned j = 0; j < 3; ++j) {
      const auto value = BinaryParts::decode(axis[j], false);
      if (value.nan || value.infinite)
        return invalid("nonfinite axis component=" + std::to_string(j));
    }
    if (count == 1) {
      if (axis[0] != axis[1] || axis[2] != 0)
        return invalid(
            "singleton axis requires bit-identical endpoints and +0 step");
      auto work = consume(1);
      if (!work.ok())
        return work;
      knots.assign(1, axis[0]);
      return Status::success();
    }
    const auto a = BinaryParts::decode(axis[0], false).order_key();
    const auto b = BinaryParts::decode(axis[1], false).order_key();
    if (a == b)
      return invalid("axis endpoints must differ");
    descending = b < a;
    auto step = sampling.weighted(axis[1], axis[0], 1, 1, count - 1, false,
                                  true, consume);
    if (!step.ok())
      return step.status();
    const auto parts = BinaryParts::decode(step.value(), false);
    if (!parts.magnitude || parts.infinite || parts.negative != descending ||
        step.value() != axis[2])
      return invalid("axis component=2 inconsistent or unrepresentable step");
    knots.resize(count);
    for (unsigned j = 0; j < count; ++j) {
      auto work = consume(1);
      if (!work.ok())
        return work;
      auto value = sampling.coordinate(j, count, {axis[0], axis[1]}, consume);
      if (!value.ok())
        return value.status();
      knots[j] = value.value();
      if (j && key(knots[j - 1]) >= key(knots[j]))
        return invalid("non-strict reconstructed axis knot=" +
                       std::to_string(j));
    }
    return Status::success();
  }
};
}  // namespace ps::plugin_internal::numeric_ops
