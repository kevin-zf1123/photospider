#pragma once

#include <array>
#include <cmath>
#include <functional>
#include <vector>

#include "data/value_validation.hpp"
#include "photospider/data/color_array.hpp"
#include "photospider/data/semantic.hpp"

namespace ps::input_internal {
template <class Function>
Status visit_typed_samples(const Region& region,
                           const std::function<ErrorCode()>& stop,
                           Function function) {
  const auto& dims = region.dimensions();
  std::vector<uint64_t> at;
  at.reserve(dims.size());
  for (auto d : dims)
    at.push_back(d.offset);
  uint64_t visited = 0;
  for (;;) {
    if ((visited++ & 1023U) == 0 && stop) {
      auto code = stop();
      if (code != ErrorCode::Ok)
        return {code, {}};
    }
    auto status = function(at);
    if (!status.ok())
      return status;
    size_t axis = at.size();
    while (axis) {
      --axis;
      if (++at[axis] < dims[axis].offset + dims[axis].extent)
        break;
      at[axis] = dims[axis].offset;
    }
    if (!axis && at[0] == dims[0].offset)
      return Status::success();
  }
}
template <class Read>
Status validate_semantic_samples(const SemanticDescriptor& semantic,
                                 const ValueDescriptor& descriptor,
                                 const Region& region, size_t batch_rank,
                                 Read read, ErrorCode failure,
                                 const std::function<ErrorCode()>& stop) {
  Float32Environment environment;
  if (!environment.active())
    return {ErrorCode::OperationFailed,
            "cannot set typed sample numeric environment"};
  auto status = validate_semantic_descriptor(semantic, descriptor);
  if (!status.ok())
    return status;
  if (region.empty())
    return {ErrorCode::TypeMismatch, "empty typed coverage"};
  if (semantic.kind == SemanticKind::ByteResource)
    return Status::success();
  const bool image = semantic.kind == SemanticKind::Image;
  const auto channel_axis = batch_rank + 2;
  if (image &&
      (region.dimensions()[channel_axis].offset != 0 ||
       region.dimensions()[channel_axis].extent != semantic.channels.size()))
    return {ErrorCode::TypeMismatch, "image coverage omits channels"};
  std::array<double, 3> color{};
  return visit_typed_samples(region, stop, [&](const auto& at) {
    auto sample = read(at);
    if (!sample.ok())
      return sample.status();
    const double value = sample.value();
    bool valid = std::isfinite(value);
    if (semantic.kind == SemanticKind::Mask)
      valid &= value >= 0 && value <= 1;
    if (image) {
      const auto channel = at[channel_axis];
      if (channel < 3)
        color[channel] = value;
      else
        valid &=
            value >= 0 && value <= 1 &&
            (semantic.association != "coverage_premultiplied" || value != 0 ||
             (color[0] == 0 && color[1] == 0 && color[2] == 0));
    }
    return valid ? Status::success()
                 : Status{failure, "sample violates typed semantic domain"};
  });
}
template <class Read>
Status validate_color_samples(const ColorArrayDescriptor& color,
                              const ValueDescriptor& descriptor,
                              const Region& region, Read read,
                              ErrorCode failure,
                              const std::function<ErrorCode()>& stop) {
  auto status = validate_color_array_descriptor(color, descriptor);
  if (!status.ok())
    return status;
  if (region.empty() || region.dimensions().back().offset != 0 ||
      region.dimensions().back().extent != descriptor.shape.back())
    return {ErrorCode::TypeMismatch, "color coverage omits complete tuple"};
  Float32Environment environment;
  if (!environment.active())
    return {ErrorCode::OperationFailed, "cannot set color numeric environment"};
  std::array<double, 4> tuple{};
  return visit_typed_samples(region, stop, [&](const auto& at) {
    auto sample = read(at);
    if (!sample.ok())
      return sample.status();
    const double value = sample.value();
    const auto channel = at.back();
    tuple[channel] = value;
    bool valid = std::isfinite(value);
    auto reason = FailureReason::InvalidDomain;
    if (color.model == ColorModel::Cmyk)
      valid &= value >= 0 && value <= 1;
    if ((color.model == ColorModel::Cielch ||
         color.model == ColorModel::Oklch) &&
        channel == 1)
      valid &= value >= 0;
    if (color.model == ColorModel::Rgb && channel == 3) {
      valid &= value >= 0 && value <= 1;
      if (valid && color.association == ColorAssociation::Premultiplied &&
          value == 0 && (tuple[0] != 0 || tuple[1] != 0 || tuple[2] != 0)) {
        valid = false;
        reason = FailureReason::InvalidAssociation;
      }
    }
    return valid
               ? Status::success()
               : Status{failure, "sample violates color-array domain", reason};
  });
}
}  // namespace ps::input_internal
