#pragma once

#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "00-foundation/image_program.hpp"
#include "01-numeric/numeric_tensor_program.hpp"
#include "plugin/port_validation.hpp"

namespace ps::plugin_internal::stmap_result {
using namespace numeric_ops;  // NOLINT(build/namespaces)
constexpr std::uint64_t coordinate_limit = UINT64_C(1) << 40;
enum class Boundary { Constant, Clamp, Wrap, Reflect, Mirror };
inline std::optional<std::uint64_t> boundary_index(std::int64_t index,
                                                   std::uint64_t extent,
                                                   Boundary mode) {
  const auto n = static_cast<std::int64_t>(extent);
  if (index >= 0 && index < n)
    return static_cast<std::uint64_t>(index);
  if (mode == Boundary::Constant)
    return {};
  if (mode == Boundary::Clamp)
    return index < 0 ? 0 : extent - 1;
  if (extent == 1)
    return 0;
  const auto period = mode == Boundary::Wrap      ? n
                      : mode == Boundary::Reflect ? 2 * n
                                                  : 2 * n - 2;
  auto position = index % period;
  if (position < 0)
    position += period;
  if (mode != Boundary::Wrap && position >= n)
    position =
        mode == Boundary::Reflect ? period - 1 - position : period - position;
  return static_cast<std::uint64_t>(position);
}
inline Boundary boundary(const std::string& mode) {
  if (mode == "constant")
    return Boundary::Constant;
  if (mode == "clamp")
    return Boundary::Clamp;
  if (mode == "wrap")
    return Boundary::Wrap;
  if (mode == "reflect")
    return Boundary::Reflect;
  if (mode == "mirror")
    return Boundary::Mirror;
  throw Status{ErrorCode::InvalidArgument, "unknown STMap boundary"};
}
struct Program final {
  struct Tap {
    std::uint64_t y = 0, x = 0;
    double weight = 0;
    bool present = false;
  };
  Boundary mode;
  unsigned stage = 0;
  std::size_t box = 0;
  Footprint outputs;
  std::array<std::uint64_t, 5> at{};
  std::array<Tap, 4> taps{};
  std::optional<ResultBuilder> builder;
  explicit Program(Boundary selected) : mode(selected) {}
  Region pixel() const {
    return Region({{at[0], 1}, {at[1], 1}, {at[2], 1}, {at[3], 1}, {0, 4}});
  }
  Region source_pixel(const Tap& tap) const {
    return Region({{at[0], 1}, {at[1], 1}, {tap.y, 1}, {tap.x, 1}, {0, 4}});
  }
  Region map_pixel(const ResultProgramQuery& query) const {
    const auto& map = query.inputs[1].result_schema->tensors[0];
    return map.batch_axes.empty()
               ? Region({{at[2], 1}, {at[3], 1}, {0, 2}})
               : Region(
                     {{at[0], 1}, {at[1], 1}, {at[2], 1}, {at[3], 1}, {0, 2}});
  }
  void next() {
    const auto& dimensions = outputs.boxes()[box].dimensions();
    for (std::size_t axis = 4; axis-- > 0;) {
      if (++at[axis] < dimensions[axis].offset + dimensions[axis].extent)
        return;
      at[axis] = dimensions[axis].offset;
    }
    if (++box < outputs.boxes().size()) {
      for (std::size_t axis = 0; axis < 5; ++axis) {
        at[axis] = outputs.boxes()[box].dimensions()[axis].offset;
      }
    }
  }
  ResultRelation support(const ResultProgramPhase& phase, std::uint32_t port,
                         std::uint32_t roles, const Region& region) const {
    const auto output_shape =
        phase.query.output.result_schema->tensors[0].sample_shape();
    const auto input_shape =
        phase.query.inputs[port].result_schema->tensors[0].sample_shape();
    std::vector<ResultMappedAxis> axes(input_shape.size());
    for (std::size_t axis = 0; axis < axes.size(); ++axis) {
      axes[axis].source_origin = region.dimensions()[axis].offset;
      axes[axis].extent = region.dimensions()[axis].extent;
    }
    return math_take(ResultRelation::mapped(
        phase.resources, output_shape, pixel(), input_shape, axes,
        {port, roles, 0, 0, ResultSupportTarget::Tensor, 0}));
  }
  ResultRelation relation(const ResultProgramPhase& phase) const {
    std::vector<ResultRelation> parts;
    auto add = [&](std::uint32_t port, std::uint32_t roles,
                   const Region& region) {
      const auto& spec = phase.query.inputs[port].result_schema->tensors[0];
      auto footprint =
          math_take(Footprint::from_regions(spec.sample_shape(), {region}));
      auto closed = math_take(spec.close_samples(footprint));
      if (closed == footprint) {
        parts.push_back(support(phase, port, roles | 4U, region));
        return;
      }
      parts.push_back(support(phase, port, roles, region));
      for (const auto& closure : closed.boxes())
        parts.push_back(support(phase, port, 4, closure));
    };
    add(1, 2, map_pixel(phase.query));
    for (const auto& tap : taps)
      if (tap.present)
        add(0, 1, source_pixel(tap));
    return math_take(ResultRelation::unite(phase.resources, parts));
  }
  Result<ResultProgramPoll> poll(const ResultProgramPhase& phase) try {
    auto scratch =
        math_take(phase.resources.reserve(ResourceCapacity::host(4096, 4096)));
    math_require(phase.consume_work(1));
    input_internal::Float32Environment environment;
    if (!environment.active())
      throw Status{ErrorCode::OperationFailed,
                   "STMap numeric environment unavailable"};
    const auto& output = *phase.query.output.result_schema;
    const auto shape = output.tensors[0].sample_shape();
    if (!outputs.valid()) {
      auto requested = phase.query.tensor_outputs
                           ? *phase.query.tensor_outputs
                           : math_take(Footprint::all(shape));
      outputs = math_take(output.tensors[0].close_samples(requested));
      if (outputs.empty()) {
        auto empty = math_take(ResultBuilder::start(phase.resources, output,
                                                    phase.query.semantic_key));
        math_require(empty.bind_descriptor_relation(
            math_take(ResultRelation::cartesian(phase.resources, 1, {}))));
        return Result<ResultProgramPoll>(
            ResultPublication{math_take(empty.seal()), true});
      }
      for (std::size_t axis = 0; axis < 5; ++axis)
        at[axis] = outputs.boxes()[0].dimensions()[axis].offset;
    }
    if (stage == 0) {
      ResultProgramNeed need;
      const auto& source = phase.query.inputs[0].result_schema->tensors[0];
      const auto& map = phase.query.inputs[1].result_schema->tensors[0];
      need.tensors.push_back(
          {0, 0, math_take(Footprint::none(source.sample_shape())), 8});
      need.tensors.push_back(
          {1, 0,
           math_take(Footprint::from_regions(map.sample_shape(),
                                             {map_pixel(phase.query)})),
           14});
      stage = 1;
      return Result<ResultProgramPoll>(std::move(need));
    }
    if (stage == 1) {
      if (!builder) {
        builder.emplace(math_take(ResultBuilder::start(
            phase.resources, output, phase.query.semantic_key, {},
            phase.association
                ? std::vector<std::uint64_t>(phase.association->begin(),
                                             phase.association->end())
                : std::vector<std::uint64_t>{},
            phase.query.tile_height, phase.query.tile_width,
            phase.query.resources)));
        std::vector<ResultRelation> descriptors;
        for (std::uint32_t port = 0; port < 2; ++port)
          descriptors.push_back(math_take(ResultRelation::cartesian(
              phase.resources, 1,
              {port, 8, 0, 1, ResultSupportTarget::Descriptor, 0})));
        math_require(builder->bind_descriptor_relation(
            math_take(ResultRelation::unite(phase.resources, descriptors))));
      }
      const auto region = map_pixel(phase.query);
      auto window = math_take(
          phase.tensors->at({1, 0}).acquire(region, phase.query.cancellation));
      std::vector<std::uint64_t> coordinate;
      for (const auto& d : region.dimensions())
        coordinate.push_back(d.offset);
      double u = 0, v = 0;
      auto run = math_take(window.row_run(coordinate));
      std::memcpy(&u, run.data, 8);
      coordinate.back() = 1;
      run = math_take(window.row_run(coordinate));
      std::memcpy(&v, run.data, 8);
      if (!std::isfinite(u) || !std::isfinite(v) ||
          std::abs(u) > coordinate_limit || std::abs(v) > coordinate_limit)
        throw Status{ErrorCode::OperationFailed,
                     "STMap coordinate must be finite and within +/-2^40"};
      const auto left = static_cast<std::int64_t>(std::floor(u - .5));
      const auto top = static_cast<std::int64_t>(std::floor(v - .5));
      const double fx = (u - .5) - static_cast<double>(left),
                   fy = (v - .5) - static_cast<double>(top);
      const auto& source = phase.query.inputs[0].result_schema->tensors[0];
      std::vector<Region> regions;
      for (unsigned dy = 0; dy < 2; ++dy)
        for (unsigned dx = 0; dx < 2; ++dx) {
          math_require(phase.consume_work(1));
          auto& tap = taps[dy * 2 + dx];
          const auto sy =
              boundary_index(top + dy, source.descriptor.shape[0], mode);
          const auto sx =
              boundary_index(left + dx, source.descriptor.shape[1], mode);
          tap.weight = (dy ? fy : 1 - fy) * (dx ? fx : 1 - fx);
          tap.present = sy.has_value() && sx.has_value();
          if (tap.present) {
            tap.y = *sy;
            tap.x = *sx;
            regions.push_back(source_pixel(tap));
          }
        }
      ResultProgramNeed need;
      need.tensors.push_back(
          {0, 0,
           math_take(Footprint::from_regions(source.sample_shape(), regions)),
           regions.empty() ? 8U : 13U});
      stage = 2;
      return Result<ResultProgramPoll>(std::move(need));
    }
    std::array<std::array<float, 4>, 4> values{};
    for (std::size_t i = 0; i < taps.size(); ++i)
      if (taps[i].present) {
        auto window = math_take(phase.tensors->at({0, 0}).acquire(
            source_pixel(taps[i]), phase.query.cancellation));
        for (std::uint64_t c = 0; c < 4; ++c) {
          math_require(phase.consume_work(1));
          auto run = math_take(
              window.row_run({at[0], at[1], taps[i].y, taps[i].x, c}));
          std::memcpy(&values[i][c], run.data, 4);
        }
      }
    std::array<float, 4> result{};
    for (std::size_t c = 0; c < 4; ++c) {
      double sum = 0;
      for (std::size_t i = 0; i < taps.size(); ++i)
        sum += static_cast<double>(values[i][c]) * taps[i].weight;
      result[c] = static_cast<float>(sum);
    }
    auto witness = relation(phase);
    math_require(builder->publish_tensor_kernel(
        0, pixel(),
        [&](const auto& writers) {
          return math_callback(phase, [&] {
            std::vector<std::uint64_t> coordinate(at.begin(), at.end());
            for (const auto& writer : writers) {
              MathTensorWriter target(writer);
              for (std::uint64_t c = 0; c < 4; ++c) {
                coordinate[4] = c;
                std::memcpy(target.address(coordinate), &result[c], 4);
              }
            }
            return phase.consume_work(0);
          });
        },
        std::move(witness), {true, true, true, true},
        phase.query.cancellation));
    at[4] = 0;
    next();
    if (box == outputs.boxes().size())
      return Result<ResultProgramPoll>(
          ResultPublication{math_take(builder->seal()), true});
    stage = 0;
    return poll(phase);
  } catch (const Status& status) {
    math_record_failure(phase, status);
    return Result<ResultProgramPoll>(status);
  }
};
inline OperationDefinition operation() {
  OperationDefinition definition;
  definition.key = "image.stmap";
  auto& traits = definition.traits;
  traits.input_count = 2;
  traits.input_schema.resize(2);
  for (auto& port : traits.input_schema)
    port.kind = OperationPortKind::Result;
  traits.input_schema[0].result_schema_id = "photospider.image";
  traits.input_schema[0].result_schema_version = 1;
  traits.input_schema[1].element_type =
      static_cast<std::uint32_t>(ElementType::Float64);
  set_whole_tensor_output(traits, ElementType::Float32, sizeof(Program));
  traits.outputs[0].key = "value";
  traits.outputs[0].region_rule = OperationRegionRule::Dependency;
  traits.outputs[0].maximum_dependency_stages = 1048576;
  traits.outputs[0].output_schema.result_schema_id = "photospider.image";
  traits.outputs[0].output_schema.tensor_key = "pixels";
  traits.outputs[0].result_schema = image_ops::image_schema();
  traits.requires_metadata_specialization = true;
  traits.parameter_schema = {
      {"boundary", OperationParameterType::String, true}};
  definition.specialize_metadata = [](const auto& inputs,
                                      const auto& parameters)
      -> Result<std::vector<OperationOutputSpecialization>> {
    try {
      math_require(
          image_ops::check_image(inputs[0], image_ops::ImageKind::Rgba));
      if (!inputs[1].result_schema ||
          !inputs[1].result_schema->fields.empty() ||
          inputs[1].result_schema->tensors.size() != 1)
        throw Status{ErrorCode::TypeMismatch,
                     "STMap requires one map tensor and no fields"};
      const auto& source = inputs[0].result_schema->tensors[0];
      const auto& map = inputs[1].result_schema->tensors[0];
      if (map.descriptor.shape.size() != 3 || map.descriptor.shape[2] != 2 ||
          !map.facets.empty() ||
          (!map.batch_axes.empty() && map.batch_axes != source.batch_axes) ||
          source.descriptor.shape[0] > coordinate_limit ||
          source.descriptor.shape[1] > coordinate_limit)
        throw Status{ErrorCode::TypeMismatch,
                     "STMap requires a generic two-component map and source "
                     "axes <=2^40"};
      boundary(std::get<std::string>(parameters.at("boundary")));
      auto schema = image_ops::image_schema();
      schema.tensors[0].batch_axes = source.batch_axes;
      schema.tensors[0].descriptor.shape = {map.descriptor.shape[0],
                                            map.descriptor.shape[1], 4};
      OperationOutputSpecialization output;
      output.metadata.result_schema =
          std::make_shared<const SchemaTemplate>(std::move(schema));
      return Result<std::vector<OperationOutputSpecialization>>(
          std::vector<OperationOutputSpecialization>{std::move(output)});
    } catch (const Status& status) {
      return Result<std::vector<OperationOutputSpecialization>>(status);
    }
  };
  definition.start_result = [](const ResultProgramQuery& query,
                               const BufferAllocator& allocator) {
    return ResultContinuation::make<Program>(
        allocator,
        boundary(std::get<std::string>(query.parameters.at("boundary"))));
  };
  return definition;
}
}  // namespace ps::plugin_internal::stmap_result
