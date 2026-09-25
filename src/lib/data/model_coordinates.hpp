#pragma once

#include <optional>
#include <string>

#include "photospider/data/tensor_description.hpp"

namespace ps::data_internal {
// Empty fields do not assert absence. Validate all overlaps before modifying
// the destination, so a rejected assertion never leaves a partial merge.
inline bool overlay_model_coordinates(
    std::optional<TensorModelCoordinates>* destination,
    const std::optional<TensorModelCoordinates>& source, bool assertion) {
  if (!source)
    return true;
  if (!*destination) {
    *destination = source;
    return true;
  }
  auto& a = **destination;
  const auto& b = *source;
  const auto compatible = [](const std::string& x, const std::string& y) {
    return x.empty() || y.empty() || x == y;
  };
  if (assertion &&
      (!compatible(a.scale, b.scale) || !compatible(a.observer, b.observer) ||
       !compatible(a.gray_kind, b.gray_kind) ||
       (a.ncl_coefficients && b.ncl_coefficients &&
        *a.ncl_coefficients != *b.ncl_coefficients)))
    return false;
  if (!b.scale.empty())
    a.scale = b.scale;
  if (!b.observer.empty())
    a.observer = b.observer;
  if (!b.gray_kind.empty())
    a.gray_kind = b.gray_kind;
  if (b.ncl_coefficients)
    a.ncl_coefficients = b.ncl_coefficients;
  return true;
}
}  // namespace ps::data_internal
