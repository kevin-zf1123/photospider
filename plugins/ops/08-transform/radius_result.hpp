#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

#include "01-numeric/numeric_tensor_program.hpp"
#include "data/input_validation.hpp"

namespace ps::plugin_internal::radius_result {
using namespace numeric_ops;  // NOLINT(build/namespaces)
constexpr std::uint64_t coordinate_limit = UINT64_C(1) << 40;
struct Program final {
  static constexpr std::uint64_t chunk = 64;
  bool scatter;
  unsigned stage = 0;
  std::size_t box = 0;
  std::uint64_t output = 0, cursor = 0, end = 0, chunk_end = 0, count = 0;
  std::array<std::uint64_t, chunk> hits{};
  double sum = 0;
  Footprint outputs;
  ResultRelation witness;
  std::optional<ResultBuilder> builder;
  explicit Program(bool selected) : scatter(selected) {}
  Region point() const { return Region({{output, 1}}); }
  void record(const ResultProgramPhase& phase, std::uint32_t port,
              std::uint32_t role, const Region& region) {
    const auto& spec = phase.query.inputs[port].result_schema->tensors[0];
    auto add = [&](std::uint32_t roles, const Region& source) {
      ResultMappedAxis axis;
      axis.source_origin = source.dimensions()[0].offset;
      axis.extent = source.dimensions()[0].extent;
      auto part = math_take(ResultRelation::mapped(
          phase.resources, outputs.shape(), point(), spec.sample_shape(),
          {axis}, {port, roles, 0, 0, ResultSupportTarget::Tensor, 0}));
      witness = witness.valid() ? math_take(ResultRelation::unite(
                                      phase.resources, {witness, part}))
                                : std::move(part);
    };
    add(role, region);
    auto closed = math_take(spec.close_samples(
        math_take(Footprint::from_regions(spec.sample_shape(), {region}))));
    for (const auto& closure : closed.boxes())
      add(4, closure);
  }
  Result<ResultProgramPoll> request(const ResultProgramPhase& phase,
                                    std::uint32_t port, std::uint32_t roles,
                                    const std::vector<Region>& regions) {
    ResultProgramNeed need;
    const auto other = 1U - port;
    need.tensors.push_back(
        {other, 0,
         math_take(Footprint::none(phase.query.inputs[other]
                                       .result_schema->tensors[0]
                                       .sample_shape())),
         8});
    auto samples = math_take(Footprint::from_regions(
        phase.query.inputs[port].result_schema->tensors[0].sample_shape(),
        regions));
    for (const auto& region : samples.boxes())
      record(phase, port, roles, region);
    need.tensors.push_back({port, 0, std::move(samples), roles | 12U});
    return Result<ResultProgramPoll>(std::move(need));
  }
  Result<ResultProgramPoll> controls(const ResultProgramPhase& phase,
                                     std::uint64_t size) {
    chunk_end = cursor + std::min(chunk, size - cursor);
    stage = 1;
    return request(phase, 1, 2, {Region({{cursor, chunk_end - cursor}})});
  }
  Result<ResultProgramPoll> data(const ResultProgramPhase& phase) {
    std::vector<Region> regions;
    for (std::uint64_t i = 0; i < count; ++i)
      regions.emplace_back(Region({{hits[i], 1}}));
    stage = 2;
    return request(phase, 0, 1, regions);
  }
  Result<ResultProgramPoll> gather_data(const ResultProgramPhase& phase) {
    count = std::min(chunk, end - cursor);
    for (std::uint64_t i = 0; i < count; ++i)
      hits[i] = cursor + i;
    cursor += count;
    return data(phase);
  }
  template <class T>
  T read(const ResultProgramPhase& phase, std::uint32_t port,
         std::uint64_t index) {
    math_require(phase.consume_work(1));
    auto window = math_take(phase.tensors->at({port, 0}).acquire(
        Region({{index, 1}}), phase.query.cancellation));
    auto run = math_take(window.row_run({index}));
    T sample{};
    std::memcpy(&sample, run.data, sizeof(sample));
    return sample;
  }
  Result<ResultProgramPoll> poll(const ResultProgramPhase& phase) try {
    auto scratch = math_take(
        phase.resources.reserve(ResourceCapacity::host(32768, 32768)));
    math_require(phase.consume_work(1));
    input_internal::Float32Environment environment;
    if (!environment.active())
      throw Status{ErrorCode::OperationFailed,
                   "cannot establish radius floating environment"};
    const auto& schema = *phase.query.output.result_schema;
    const auto size = schema.tensors[0].descriptor.shape[0];
    if (!outputs.valid()) {
      outputs =
          phase.query.tensor_outputs
              ? *phase.query.tensor_outputs
              : math_take(Footprint::all(schema.tensors[0].sample_shape()));
      if (outputs.empty()) {
        auto empty = math_take(ResultBuilder::start(phase.resources, schema,
                                                    phase.query.semantic_key));
        math_require(empty.bind_descriptor_relation(
            math_take(ResultRelation::cartesian(phase.resources, 1, {}))));
        return Result<ResultProgramPoll>(
            ResultPublication{math_take(empty.seal()), true});
      }
      output = outputs.boxes()[0].dimensions()[0].offset;
    }
    if (stage == 0) {
      cursor = 0;
      sum = 0;
      if (scatter)
        return controls(phase, size);
      stage = 1;
      return request(phase, 1, 2, {point()});
    }
    if (!builder) {
      builder.emplace(math_take(ResultBuilder::start(
          phase.resources, schema, phase.query.semantic_key, {},
          phase.association
              ? std::vector<std::uint64_t>(phase.association->begin(),
                                           phase.association->end())
              : std::vector<std::uint64_t>{})));
      std::vector<ResultRelation> descriptors;
      for (std::uint32_t port = 0; port < 2; ++port)
        descriptors.push_back(math_take(ResultRelation::cartesian(
            phase.resources, 1,
            {port, 8, 0, 1, ResultSupportTarget::Descriptor, 0})));
      math_require(builder->bind_descriptor_relation(
          math_take(ResultRelation::unite(phase.resources, descriptors))));
    }
    if (stage == 1) {
      auto radius = [&](std::uint64_t index) {
        const auto r = read<std::int64_t>(phase, 1, index);
        if (r < 0 || static_cast<std::uint64_t>(r) > coordinate_limit)
          throw Status{ErrorCode::OperationFailed,
                       "radius must be in [0,2^40]"};
        return static_cast<std::uint64_t>(r);
      };
      if (scatter) {
        count = 0;
        for (auto index = cursor; index < chunk_end; ++index) {
          const auto r = radius(index);
          const auto distance =
              index > output ? index - output : output - index;
          if (distance <= r)
            hits[count++] = index;
        }
        cursor = chunk_end;
        return data(phase);
      }
      const auto r = radius(output);
      cursor = output > r ? output - r : 0;
      end = output + 1 + std::min(r, size - output - 1);
      return gather_data(phase);
    }
    for (std::uint64_t i = 0; i < count; ++i) {
      const auto sample = read<double>(phase, 0, hits[i]);
      if (!std::isfinite(sample))
        throw Status{ErrorCode::OperationFailed, "nonfinite radius sample"};
      sum += sample;
      if (!std::isfinite(sum))
        throw Status{ErrorCode::OperationFailed, "radius sum overflow"};
    }
    if (scatter && cursor < size)
      return controls(phase, size);
    if (!scatter && cursor < end)
      return gather_data(phase);
    math_require(builder->publish_tensor_kernel(
        0, point(),
        [&](const auto& writers) {
          return math_callback(phase, [&] {
            for (const auto& writer : writers) {
              auto run = math_take(writer.row_run({output}));
              std::memcpy(run.data, &sum, sizeof(sum));
            }
            return phase.consume_work(0);
          });
        },
        std::move(witness), {true, true, true, true},
        phase.query.cancellation));
    const auto& d = outputs.boxes()[box].dimensions()[0];
    if (++output == d.offset + d.extent) {
      if (++box == outputs.boxes().size())
        return Result<ResultProgramPoll>(
            ResultPublication{math_take(builder->seal()), true});
      output = outputs.boxes()[box].dimensions()[0].offset;
    }
    witness = {};
    stage = 0;
    return poll(phase);
  } catch (const Status& status) {
    math_record_failure(phase, status);
    return Result<ResultProgramPoll>(status);
  }
};
inline OperationDefinition operation(bool scatter) {
  OperationDefinition definition;
  definition.key = scatter ? "numeric.radius_scatter" : "numeric.radius_gather";
  auto& traits = definition.traits;
  traits.input_count = 2;
  traits.input_schema.resize(2);
  for (std::uint32_t port = 0; port < 2; ++port) {
    traits.input_schema[port].kind = OperationPortKind::Result;
    traits.input_schema[port].element_type = static_cast<std::uint32_t>(
        port == 0 ? ElementType::Float64 : ElementType::Int64);
    traits.input_schema[port].rank = 1;
  }
  set_whole_tensor_output(traits, ElementType::Float64, sizeof(Program));
  traits.outputs[0].key = "value";
  traits.outputs[0].region_rule = OperationRegionRule::Dependency;
  traits.outputs[0].maximum_dependency_stages = 1048576;
  traits.requires_metadata_specialization = true;
  definition.specialize_metadata =
      [](const auto& inputs,
         const auto&) -> Result<std::vector<OperationOutputSpecialization>> {
    for (const auto& input : inputs)
      if (!input.result_schema || !input.result_schema->fields.empty() ||
          input.result_schema->tensors.size() != 1 ||
          !input.result_schema->tensors[0].batch_axes.empty())
        return Result<std::vector<OperationOutputSpecialization>>(
            Status{ErrorCode::TypeMismatch,
                   "radius requires one unbatched tensor per input"});
    const auto& source = inputs[0].result_schema->tensors[0];
    const auto& radius = inputs[1].result_schema->tensors[0];
    if (source.descriptor.shape != radius.descriptor.shape ||
        source.descriptor.shape[0] > coordinate_limit)
      return Result<std::vector<OperationOutputSpecialization>>(
          Status{ErrorCode::TypeMismatch,
                 "radius inputs must share a rank-one domain of at most 2^40 "
                 "samples"});
    OperationOutputSpecialization output;
    output.metadata.result_schema = std::make_shared<const SchemaTemplate>(
        numeric_tensor_schema(ElementType::Float64, source.descriptor.shape));
    return Result<std::vector<OperationOutputSpecialization>>(
        std::vector<OperationOutputSpecialization>{std::move(output)});
  };
  definition.start_result = [scatter](const ResultProgramQuery&,
                                      const BufferAllocator& allocator) {
    return ResultContinuation::make<Program>(allocator, scatter);
  };
  return definition;
}
}  // namespace ps::plugin_internal::radius_result
