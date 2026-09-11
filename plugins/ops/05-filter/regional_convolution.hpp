#pragma once

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "00-foundation/basic_common.hpp"
#include "00-foundation/multi_output.hpp"

namespace ps::plugin_internal::convolution {
inline Status invalid(const char* message) {
  return Status{ErrorCode::InvalidArgument, message};
}
inline std::string prefix(std::uint32_t output, bool image) {
  constexpr const char* names[] = {"r_", "g_", "b_"};
  return image ? names[output] : "";
}
inline Status validate(const std::vector<OperationMetadata>& inputs,
                       const std::map<std::string, ParameterValue>& parameters,
                       bool image) {
  if (image) {
    bool found = false;
    for (const auto& facet : inputs[0].facets) {
      if (facet.key != "photospider.image")
        continue;
      auto semantic = decode_semantic(facet);
      if (!semantic.ok() || semantic.value().model != "rgb")
        return invalid("channel convolution requires RGB image semantics");
      found = true;
    }
    if (!found)
      return invalid("channel convolution requires image semantics");
  } else if (!inputs[0].facets.empty()) {
    if (inputs[0].facets.size() != 1)
      return invalid("field convolution requires one scalar interpretation");
    auto semantic = decode_semantic(inputs[0].facets[0]);
    if (!semantic.ok())
      return semantic.status();
    const auto kind = semantic.value().kind;
    if (kind != SemanticKind::ScalarField && kind != SemanticKind::ImagePlane &&
        inputs[0].facets[0].payload !=
            encode_semantic(coverage_semantics()).value().payload)
      return invalid(
          "field convolution requires scalar, coverage or plane semantics");
  }
  for (std::uint32_t output = 0; output < (image ? 3U : 1U); ++output) {
    const auto& kernel = inputs[output + 1];
    if (!kernel.facets.empty() ||
        kernel.descriptor.element_type != inputs[0].descriptor.element_type)
      return invalid("kernel must be generic and share input dtype");
    const auto name = prefix(output, image);
    const auto ay = std::get<std::int64_t>(parameters.at(name + "anchor_y"));
    const auto ax = std::get<std::int64_t>(parameters.at(name + "anchor_x"));
    const auto& boundary =
        std::get<std::string>(parameters.at(name + "boundary"));
    if (ay < 0 || ax < 0 || kernel.descriptor.shape[0] > INT64_MAX ||
        kernel.descriptor.shape[1] > INT64_MAX ||
        static_cast<std::uint64_t>(ay) >= kernel.descriptor.shape[0] ||
        static_cast<std::uint64_t>(ax) >= kernel.descriptor.shape[1] ||
        (boundary != "zero" && boundary != "clamp"))
      return invalid("invalid convolution anchor or boundary");
  }
  return Status::success();
}
inline std::vector<OperationParameterSpec> parameters(bool image) {
  std::vector<OperationParameterSpec> result;
  for (std::uint32_t output = 0; output < (image ? 3U : 1U); ++output) {
    const auto name = prefix(output, image);
    result.push_back({name + "anchor_y", OperationParameterType::Int64, true});
    result.push_back({name + "anchor_x", OperationParameterType::Int64, true});
    result.push_back({name + "boundary", OperationParameterType::String, true});
  }
  std::sort(result.begin(), result.end(),
            [](const auto& a, const auto& b) { return a.key < b.key; });
  return result;
}
inline Result<double> read(const DependencyPhase& phase, std::uint32_t port,
                           const std::vector<std::uint64_t>& coordinate) {
  double sample = 0;
  Status status;
  if (phase.query.inputs[port].descriptor.element_type ==
      ElementType::Float32) {
    float value = 0;
    status = phase.read(port, coordinate, &value, sizeof(value));
    sample = value;
  } else {
    status = phase.read(port, coordinate, &sample, sizeof(sample));
  }
  if (!status.ok())
    return Result<double>(status);
  if (!std::isfinite(sample))
    return Result<double>(Status{ErrorCode::OperationFailed,
                                 "convolution sample must be finite"});
  return Result<double>(sample);
}
struct State {
  bool image = false, requested = false;
  std::uint32_t channel = 0;
  State(bool image, std::uint32_t channel) : image(image), channel(channel) {}
  Result<DependencyPoll> poll(const DependencyPhase& phase) {
    const auto output = phase.query.output_index;
    const auto kernel_port = image ? output + 1 : 1;
    const auto name = prefix(output, image);
    const auto ay =
        std::get<std::int64_t>(phase.query.parameters.at(name + "anchor_y"));
    const auto ax =
        std::get<std::int64_t>(phase.query.parameters.at(name + "anchor_x"));
    const bool clamp =
        std::get<std::string>(phase.query.parameters.at(name + "boundary")) ==
        "clamp";
    const auto& shape = phase.query.inputs[0].descriptor.shape;
    const auto& kernel_shape = phase.query.inputs[kernel_port].descriptor.shape;
    const auto kh = kernel_shape[0], kw = kernel_shape[1];
    const auto atom = multi_output::coordinate(phase);
    if (!requested) {
      requested = true;
      std::uint64_t top, bottom, left, right;
      basic_internal::shifted(atom[0], ay - static_cast<std::int64_t>(kh - 1),
                              shape[0], true, &top);
      basic_internal::shifted(atom[0], ay, shape[0], true, &bottom);
      basic_internal::shifted(atom[1], ax - static_cast<std::int64_t>(kw - 1),
                              shape[1], true, &left);
      basic_internal::shifted(atom[1], ax, shape[1], true, &right);
      std::vector<RegionDimension> region{{top, bottom - top + 1},
                                          {left, right - left + 1}};
      if (image)
        region.push_back({0, shape[2]});
      auto input = Footprint::from_regions(shape, {Region(region)}, phase.sets);
      auto kernel = Footprint::all(kernel_shape, phase.sets);
      if (!input.ok())
        return Result<DependencyPoll>(input.status());
      if (!kernel.ok())
        return Result<DependencyPoll>(kernel.status());
      return multi_output::need(phase,
                                {{0, 5, input.take_value(), {}},
                                 {kernel_port, 5, kernel.take_value(), {}}});
    }
    if (kw && kh > UINT64_MAX / kw)
      return Result<DependencyPoll>(
          Status{ErrorCode::ResourceExhausted, "kernel size overflow"});
    auto charged = phase.consume_work(kh * kw);
    if (!charged.ok())
      return Result<DependencyPoll>(charged);
    double sum = 0;
    for (std::uint64_t y = 0; y < kh; ++y)
      for (std::uint64_t x = 0; x < kw; ++x) {
        auto coefficient = read(phase, kernel_port, {y, x});
        if (!coefficient.ok())
          return Result<DependencyPoll>(coefficient.status());
        std::uint64_t sy, sx;
        const bool inside =
            basic_internal::shifted(atom[0], ay - static_cast<std::int64_t>(y),
                                    shape[0], clamp, &sy) &&
            basic_internal::shifted(atom[1], ax - static_cast<std::int64_t>(x),
                                    shape[1], clamp, &sx);
        double sample = 0;
        if (inside) {
          std::vector<std::uint64_t> address{sy, sx};
          if (image)
            address.push_back(channel);
          auto value = read(phase, 0, address);
          if (!value.ok())
            return Result<DependencyPoll>(value.status());
          sample = value.value();
        }
        const double product = sample * coefficient.value();
        sum = sum + product;
        if (!std::isfinite(product) || !std::isfinite(sum))
          return Result<DependencyPoll>(Status{
              ErrorCode::OperationFailed, "convolution intermediate overflow"});
      }
    if (phase.query.output.descriptor.element_type == ElementType::Float32) {
      if (std::abs(sum) > std::numeric_limits<float>::max())
        return Result<DependencyPoll>(
            Status{ErrorCode::OperationFailed, "convolution Float32 overflow"});
      const float value = static_cast<float>(sum);
      return multi_output::finish(phase, &value, sizeof(value));
    }
    return multi_output::finish(phase, &sum, sizeof(sum));
  }
};
inline std::uint32_t channel(const DependencyQuery& query) {
  const char* roles[] = {"red", "green", "blue"};
  for (const auto& facet : query.inputs[0].facets)
    if (facet.key == "photospider.image") {
      auto semantic = decode_semantic(facet).take_value();
      for (std::uint32_t i = 0; i < 3; ++i)
        if (semantic.channels[i].role == roles[query.output_index])
          return i;
    }
  return 0;
}
}  // namespace ps::plugin_internal::convolution
