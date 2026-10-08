#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <utility>
#include <vector>

#include "data/affine_view.hpp"

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
  return join_affine_view(values.descriptor(), domain, parts, limits,
                          values.facets(), values.resources());
}
}  // namespace ps::input_internal
