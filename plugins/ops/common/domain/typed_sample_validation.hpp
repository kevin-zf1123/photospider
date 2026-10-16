#include "photospider/ops/errors.hpp"
#pragma once

#include <array>
#include <cmath>
#include <functional>
#include <vector>

#include "common/floating_environment.hpp"
#include "photospider/ops/color_array.hpp"

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
Status validate_color_samples(const ColorArrayDescriptor& color,
                              const TensorDescriptor& descriptor,
                              const Region& region, Read read,
                              ErrorCode failure,
                              const std::function<ErrorCode()>& stop) {
  auto status = validate_color_array_descriptor(color, descriptor);
  if (!status.ok())
    return status;
  if (region.empty() || region.dimensions().back().offset != 0 ||
      region.dimensions().back().extent != descriptor.shape.back())
    return {ErrorCode::TypeMismatch, "color coverage omits complete tuple"};
  plugin_internal::FloatingEnvironment environment;
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
    std::uint64_t domain_code = 0;
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
        domain_code = ops_error::InvalidAssociation.value;
      }
    }
    return valid ? Status::success()
                 : Status{failure, "sample violates color-array domain",
                          DomainErrorCode{domain_code}};
  });
}
}  // namespace ps::input_internal
