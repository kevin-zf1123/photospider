#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "data/input_validation.hpp"
#include "plugin/builtin_operations.hpp"

namespace ps::plugin_internal {
namespace {
constexpr std::uint64_t coordinate_limit = UINT64_C(1) << 40;
Status failed(const char* message) {
  return Status::failure(ErrorCode::OperationFailed, message);
}
Result<DependencyPoll> need(const DependencyPhase& phase, std::uint32_t port,
                            DependencyRole role,
                            const std::vector<Region>& regions) {
  auto samples = Footprint::from_regions(
      phase.query.inputs[port].descriptor.shape, regions);
  if (!samples.ok())
    return Result<DependencyPoll>(samples.status());
  std::vector<std::uint64_t> coordinate;
  for (const auto& d : phase.query.observations.boxes()[0].dimensions())
    coordinate.push_back(d.offset);
  return Result<DependencyPoll>(DependencyNeedBatch{
      {{coordinate,
        {{port, static_cast<std::uint32_t>(role), samples.take_value(), {}}}}},
      {}});
}
Result<DependencyPoll> finish(const DependencyPhase& phase, const void* bytes,
                              std::size_t size) {
  auto allocated =
      MutableValue::allocate(phase.query.output.descriptor,
                             phase.query.outputs.boxes()[0], phase.allocator);
  if (!allocated.ok())
    return Result<DependencyPoll>(allocated.status());
  auto output = allocated.take_value();
  if (output.size() != size)
    return Result<DependencyPoll>(failed("sampling output size mismatch"));
  std::memcpy(output.data(), bytes, size);
  auto value = std::move(output).publish(phase.query.output.facets);
  if (!value.ok())
    return Result<DependencyPoll>(value.status());
  auto result = ValueFragments::create(
      phase.query.output.descriptor, phase.query.output.facets,
      phase.query.outputs, {value.take_value()});
  if (!result.ok())
    return Result<DependencyPoll>(result.status());
  return Result<DependencyPoll>(result.take_value());
}
enum class Boundary { Constant, Clamp, Wrap, Reflect, Mirror };
std::optional<std::uint64_t> boundary_index(std::int64_t index,
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
struct StmapState {
  struct Tap {
    std::uint64_t y = 0, x = 0;
    double weight = 0;
    bool present = false;
  };
  explicit StmapState(Boundary mode) : boundary(mode) {}
  Boundary boundary;
  unsigned stage = 0;
  std::array<Tap, 4> taps;
  Result<DependencyPoll> poll(const DependencyPhase& phase) {
    const auto y = phase.query.outputs.boxes()[0].dimensions()[0].offset;
    const auto x = phase.query.outputs.boxes()[0].dimensions()[1].offset;
    if (stage == 0) {
      stage = 1;
      return need(phase, 1, DependencyRole::Control,
                  {Region({{y, 1}, {x, 1}, {0, 2}})});
    }
    if (stage == 1) {
      double u = 0, v = 0;
      auto status = phase.read(1, {y, x, 0}, &u, sizeof(u));
      if (!status.ok())
        return Result<DependencyPoll>(status);
      status = phase.read(1, {y, x, 1}, &v, sizeof(v));
      if (!status.ok())
        return Result<DependencyPoll>(status);
      if (!std::isfinite(u) || !std::isfinite(v) ||
          std::abs(u) > coordinate_limit || std::abs(v) > coordinate_limit)
        return Result<DependencyPoll>(
            failed("STMap coordinate must be finite and within +/-2^40"));
      const auto left = static_cast<std::int64_t>(std::floor(u - .5));
      const auto top = static_cast<std::int64_t>(std::floor(v - .5));
      const double fx = (u - .5) - static_cast<double>(left);
      const double fy = (v - .5) - static_cast<double>(top);
      std::vector<Region> regions;
      for (unsigned dy = 0; dy < 2; ++dy)
        for (unsigned dx = 0; dx < 2; ++dx) {
          status = phase.consume_work(1);
          if (!status.ok())
            return Result<DependencyPoll>(status);
          auto& tap = taps[dy * 2 + dx];
          const auto sy = boundary_index(
              top + dy, phase.query.inputs[0].descriptor.shape[0], boundary);
          const auto sx = boundary_index(
              left + dx, phase.query.inputs[0].descriptor.shape[1], boundary);
          tap.weight = (dy ? fy : 1 - fy) * (dx ? fx : 1 - fx);
          tap.present = sy.has_value() && sx.has_value();
          if (tap.present) {
            tap.y = *sy;
            tap.x = *sx;
            regions.emplace_back(Region({{tap.y, 1}, {tap.x, 1}, {0, 4}}));
          }
        }
      stage = 2;
      // Zero-weight taps are still declared and validated, in tap order.
      return need(phase, 0, DependencyRole::Data, regions);
    }
    std::array<float, 4> output{};
    std::array<std::array<float, 4>, 4> source{};
    for (std::size_t i = 0; i < taps.size(); ++i)
      if (taps[i].present) {
        for (std::uint64_t c = 0; c < 4; ++c) {
          auto status = phase.read(0, {taps[i].y, taps[i].x, c}, &source[i][c],
                                   sizeof(float));
          if (!status.ok())
            return Result<DependencyPoll>(status);
        }
      }
    for (std::size_t c = 0; c < 4; ++c) {
      double sum = 0;
      for (std::size_t i = 0; i < taps.size(); ++i)
        sum += static_cast<double>(source[i][c]) * taps[i].weight;
      output[c] = static_cast<float>(sum);
    }
    return finish(phase, output.data(), sizeof(output));
  }
};
struct RadiusState {
  static constexpr std::uint64_t chunk = 64;
  explicit RadiusState(bool scatter) : scatter(scatter) {}
  bool scatter;
  unsigned stage = 0;
  std::uint64_t cursor = 0, end = 0, chunk_end = 0, count = 0;
  std::array<std::uint64_t, chunk> hits{};
  double sum = 0;
  Result<DependencyPoll> controls(const DependencyPhase& phase,
                                  std::uint64_t size) {
    chunk_end = cursor + std::min(chunk, size - cursor);
    stage = 1;
    return need(phase, 1, DependencyRole::Control,
                {Region({{cursor, chunk_end - cursor}})});
  }
  Result<DependencyPoll> data(const DependencyPhase& phase) {
    std::vector<Region> regions;
    for (std::uint64_t i = 0; i < count; ++i)
      regions.emplace_back(Region({{hits[i], 1}}));
    stage = 2;
    return need(phase, 0, DependencyRole::Data, regions);
  }
  Result<DependencyPoll> gather_data(const DependencyPhase& phase) {
    count = std::min(chunk, end - cursor);
    const auto charged = phase.consume_work(count);
    if (!charged.ok())
      return Result<DependencyPoll>(charged);
    for (std::uint64_t i = 0; i < count; ++i)
      hits[i] = cursor + i;
    cursor += count;
    return data(phase);
  }
  Result<DependencyPoll> poll(const DependencyPhase& phase) {
    input_internal::Float32Environment environment;
    if (!environment.active())
      return Result<DependencyPoll>(
          failed("cannot establish radius floating environment"));

    const auto output = phase.query.outputs.boxes()[0].dimensions()[0].offset;
    const auto size = phase.query.inputs[0].descriptor.shape[0];
    if (stage == 0) {
      if (scatter)
        return controls(phase, size);
      stage = 1;
      return need(phase, 1, DependencyRole::Control, {Region({{output, 1}})});
    }
    if (stage == 1) {
      if (scatter) {
        count = 0;
        for (auto index = cursor; index < chunk_end; ++index) {
          std::int64_t radius = 0;
          auto status = phase.read(1, {index}, &radius, sizeof(radius));
          if (!status.ok())
            return Result<DependencyPoll>(status);
          if (radius < 0 ||
              static_cast<std::uint64_t>(radius) > coordinate_limit)
            return Result<DependencyPoll>(failed("radius must be in [0,2^40]"));
          const auto distance =
              index > output ? index - output : output - index;
          if (distance <= static_cast<std::uint64_t>(radius))
            hits[count++] = index;
        }
        cursor = chunk_end;
        return data(phase);
      }
      std::int64_t radius = 0;
      auto status = phase.read(1, {output}, &radius, sizeof(radius));
      if (!status.ok())
        return Result<DependencyPoll>(status);
      if (radius < 0 || static_cast<std::uint64_t>(radius) > coordinate_limit)
        return Result<DependencyPoll>(failed("radius must be in [0,2^40]"));
      const auto r = static_cast<std::uint64_t>(radius);
      cursor = output > r ? output - r : 0;
      end = output + 1 + std::min(r, size - output - 1);
      return gather_data(phase);
    }
    for (std::uint64_t i = 0; i < count; ++i) {
      double sample = 0;
      auto status = phase.read(0, {hits[i]}, &sample, sizeof(sample));
      if (!status.ok())
        return Result<DependencyPoll>(status);
      if (!std::isfinite(sample))
        return Result<DependencyPoll>(failed("nonfinite radius sample"));
      sum += sample;
      if (!std::isfinite(sum))
        return Result<DependencyPoll>(failed("radius sum overflow"));
    }
    if (scatter && cursor < size)
      return controls(phase, size);
    if (!scatter && cursor < end)
      return gather_data(phase);
    return finish(phase, &sum, sizeof(sum));
  }
};
OperationTraits staged(std::uint64_t state_bytes) {
  OperationTraits traits;
  traits.input_count = 2;
  traits.input_schema.resize(2);
  traits.region_rule = OperationRegionRule::Dependency;
  traits.dependency_version = 1;
  traits.continuation_bytes = state_bytes;
  traits.maximum_dependency_stages = 1048576;
  return traits;
}
}  // namespace
Status register_dependency_sampling(OperationRegistry* registry) {
  OperationDefinition stmap;
  stmap.key = "image.stmap";
  stmap.traits = staged(sizeof(StmapState));
  auto& t = stmap.traits;
  t.output_element_type = ElementType::Float32;
  t.output_schema.kind = OperationPortKind::RgbaFloat32;
  t.input_schema[0] = t.output_schema;
  t.input_schema[1].element_type =
      static_cast<std::uint32_t>(ElementType::Float64);
  t.input_schema[1].rank = 3;
  t.shape_rule = OperationShapeRule::Axes;
  t.output_axes = {{OperationExtentSource::InputAxis, 1, {}, 1, 0, 0},
                   {OperationExtentSource::InputAxis, 1, {}, 1, 1, 0},
                   {OperationExtentSource::Constant, 4, {}, 0, 0, 0}};
  t.output_semantic_rule = OperationSemanticRule::PreserveInput;
  t.parameter_schema = {{"boundary", OperationParameterType::String, true}};
  stmap.validate_dependency =
      [](const std::vector<OperationMetadata>& inputs,
         const std::map<std::string, ParameterValue>& parameters) {
        if (inputs[1].descriptor.shape[2] != 2 ||
            inputs[0].descriptor.shape[0] > coordinate_limit ||
            inputs[0].descriptor.shape[1] > coordinate_limit)
          return Status::failure(
              ErrorCode::TypeMismatch,
              "STMap requires a two-component map and source axes <=2^40");
        const auto& mode = std::get<std::string>(parameters.at("boundary"));
        if (mode != "constant" && mode != "clamp" && mode != "wrap" &&
            mode != "reflect" && mode != "mirror")
          return Status::failure(ErrorCode::InvalidArgument,
                                 "unknown STMap boundary");
        return Status::success();
      };
  stmap.start_dependency =
      [](const DependencyQuery& q,
         const BufferAllocator& allocator) -> Result<DependencyContinuation> {
    const auto& mode = std::get<std::string>(q.parameters.at("boundary"));
    const auto boundary = mode == "constant"  ? Boundary::Constant
                          : mode == "clamp"   ? Boundary::Clamp
                          : mode == "wrap"    ? Boundary::Wrap
                          : mode == "reflect" ? Boundary::Reflect
                                              : Boundary::Mirror;
    return DependencyContinuation::make<StmapState>(allocator, boundary);
  };
  auto status = registry->register_operation(std::move(stmap));
  if (!status.ok())
    return status;
  for (bool scatter : {false, true}) {
    OperationDefinition radius;
    radius.key = scatter ? "numeric.radius_scatter" : "numeric.radius_gather";
    radius.traits = staged(sizeof(RadiusState));
    radius.traits.shape_rule = OperationShapeRule::PreserveFirstInput;
    radius.traits.input_schema[0].element_type =
        static_cast<std::uint32_t>(ElementType::Float64);
    radius.traits.input_schema[1].element_type =
        static_cast<std::uint32_t>(ElementType::Int64);
    radius.traits.input_schema[0].rank = radius.traits.input_schema[1].rank = 1;
    radius.validate_dependency =
        [](const std::vector<OperationMetadata>& inputs,
           const std::map<std::string, ParameterValue>&) {
          if (inputs[0].descriptor.shape != inputs[1].descriptor.shape ||
              inputs[0].descriptor.shape[0] > coordinate_limit)
            return Status::failure(ErrorCode::TypeMismatch,
                                   "radius inputs must share a rank-one domain "
                                   "of at most 2^40 samples");
          return Status::success();
        };
    radius.start_dependency = [scatter](const DependencyQuery&,
                                        const BufferAllocator& allocator)
        -> Result<DependencyContinuation> {
      return DependencyContinuation::make<RadiusState>(allocator, scatter);
    };
    status = registry->register_operation(std::move(radius));
    if (!status.ok())
      return status;
  }
  return Status::success();
}
}  // namespace ps::plugin_internal
