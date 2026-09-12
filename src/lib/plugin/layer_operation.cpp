#include "photospider/plugin/layer_operation.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <string>
#include <utility>
#include <vector>

#include "data/input_validation.hpp"

namespace ps {
namespace {
using Poll = Result<ResultProgramPoll>;
using Op = LayerOperation;
Status invalid(const char* message) {
  return {ErrorCode::TypeMismatch, message};
}
// NOLINTBEGIN(whitespace/indent_namespace)
constexpr const char* kNames[] = {"",
                                  "assemble",
                                  "over",
                                  "opacity",
                                  "emit_front",
                                  "emit_behind",
                                  "flatten",
                                  "response",
                                  "response_over",
                                  "coverage_raw_plus",
                                  "raw_checked",
                                  "raw_capped",
                                  "weight",
                                  "weighted_reduce",
                                  "weighted_finalize",
                                  "require_valid"};
// NOLINTEND
bool binary(Op op) {
  return op == Op::Assemble || op == Op::Over || op == Op::EmitFront ||
         op == Op::EmitBehind || op == Op::Flatten || op == Op::ResponseOver ||
         op == Op::CoverageRawPlus;
}
LayerRepresentation input_kind(Op op) {
  if (op == Op::ResponseOver)
    return LayerRepresentation::Response;
  if (op == Op::RawChecked || op == Op::RawCapped)
    return LayerRepresentation::RawSum;
  if (op == Op::Reduce)
    return LayerRepresentation::Contributions;
  if (op == Op::Finalize)
    return LayerRepresentation::WeightedSum;
  if (op == Op::RequireValid)
    return LayerRepresentation::OptionalLayer;
  return LayerRepresentation::Layer;
}
LayerRepresentation output_kind(Op op) {
  if (op == Op::Response || op == Op::ResponseOver)
    return LayerRepresentation::Response;
  if (op == Op::CoverageRawPlus)
    return LayerRepresentation::RawSum;
  if (op == Op::Weight)
    return LayerRepresentation::Contributions;
  if (op == Op::Reduce)
    return LayerRepresentation::WeightedSum;
  if (op == Op::Finalize)
    return LayerRepresentation::OptionalLayer;
  return LayerRepresentation::Layer;
}
Result<ResultGrowthLimits> growth_limits(Op op, const LayerSpec& spec) {
  std::uint64_t bytes_per_row = 28, rows = 1;
  switch (output_kind(op)) {
    case LayerRepresentation::Response:
    case LayerRepresentation::RawSum:
      bytes_per_row = 16;
      break;
    case LayerRepresentation::Contributions:
    case LayerRepresentation::WeightedSum:
      bytes_per_row = 64;
      break;
    case LayerRepresentation::OptionalLayer:
      bytes_per_row = 29;
      break;
    case LayerRepresentation::Layer:
      break;
  }
  if (op != Op::Reduce && op != Op::Finalize) {
    if (!spec.width || spec.height > UINT64_MAX / spec.width)
      return Result<ResultGrowthLimits>(invalid("layer row count overflow"));
    rows = spec.height * spec.width;
  }
  if (rows > UINT64_MAX / bytes_per_row)
    return Result<ResultGrowthLimits>(invalid("layer result byte overflow"));
  return Result<ResultGrowthLimits>(
      ResultGrowthLimits{rows, rows * bytes_per_row});
}
struct State {
  Op op;
  LayerSpec spec;
  ResultBuilder builder;
  std::array<ResultRef, 2> inputs;
  std::array<ResultDescriptor, 2> descriptors;
  std::array<std::uint64_t, 3> output_rows{};
  MutableValue image;
  unsigned stage = 0, depth = 0;
  std::uint64_t row = 0, count = 0, batch = 0;
  struct Frame {
    std::uint64_t count = 0;
    bool left_done = false;
    WeightedLayerSum left;
  };
  std::array<Frame, 64> stack{};
  WeightedLayerSum sum;
  State(Op operation, LayerSpec specification)
      : op(operation), spec(specification) {}
  Result<ResultRelation> relation(const ResultProgramPhase& phase,
                                  std::uint64_t outputs) {
    std::array<ResultRelation, 4> spans;
    unsigned span_count = 0;
    for (unsigned i = 0; i < phase.query.inputs.size(); ++i) {
      std::uint64_t support = 0;
      if (inputs[i].valid()) {
        auto descriptor =
            ResultRelation::cartesian(phase.resources, outputs, {i, 8, 0, 1},
                                      DependencyGuarantee::Conservative);
        if (!descriptor.ok())
          return descriptor;
        spans[span_count++] = descriptor.take_value();
        for (unsigned f = 0; f < descriptors[i].field_count(); ++f) {
          auto width =
              inputs[i].schema().row_bytes(f).value() /
              Value::element_size(inputs[i].schema().fields[f].element_type);
          if (descriptors[i].rows(f) > (UINT64_MAX - support) / width)
            return Result<ResultRelation>(invalid("layer support overflow"));
          support += descriptors[i].rows(f) * width;
        }
      } else {
        support = 1;
        for (auto n : phase.query.inputs[i].descriptor.shape)
          support *= n;
      }
      auto made = ResultRelation::cartesian(
          phase.resources, outputs,
          {i, inputs[i].valid() ? 7U : 15U, 0, support},
          DependencyGuarantee::Conservative);
      if (!made.ok())
        return made;
      spans[span_count++] = made.take_value();
    }
    if (span_count == 1)
      return Result<ResultRelation>(spans[0]);
    auto bridge = phase.resources.reserve(
        ResourceCapacity::host(span_count * sizeof(ResultRelation),
                               span_count * sizeof(ResultRelation)));
    if (!bridge.ok())
      return Result<ResultRelation>(bridge.status());
    return ResultRelation::unite(
        phase.resources,
        std::vector<ResultRelation>(spans.begin(), spans.begin() + span_count));
  }

  Status descend(std::uint64_t n) {
    while (true) {
      if (depth == stack.size())
        return {ErrorCode::ResourceExhausted,
                "canonical weighted tree depth exhausted"};
      stack[depth++] = {n, false, {}};
      if (n <= 1)
        return Status::success();
      n /= 2;
    }
  }
  Status accept(WeightedLayerSum value, const ResultProgramPhase& phase) {
    if (!depth || stack[depth - 1].count != 1)
      return {ErrorCode::Internal, "invalid weighted tree cursor"};
    --depth;
    while (depth) {
      auto& parent = stack[depth - 1];
      if (!parent.left_done) {
        parent.left = value;
        parent.left_done = true;
        return descend(parent.count - parent.count / 2);
      }
      auto charged = phase.consume_work(8);
      if (!charged.ok())
        return charged;
      for (unsigned i = 0; i < 8; ++i) {
        volatile double combined =
            parent.left.components[i] + value.components[i];
        if (!std::isfinite(combined))
          return {ErrorCode::OperationFailed, "weighted tree overflow",
                  FailureReason::ArithmeticOverflow};
        value.components[i] = combined;
      }
      --depth;
    }
    sum = value;
    return Status::success();
  }
  Result<ResultWritePlan> write(const ResultProgramPhase& phase, unsigned field,
                                const void* data, std::size_t bytes) {
    auto memory = phase.allocator.allocate(bytes);
    if (!memory.ok())
      return Result<ResultWritePlan>(memory.status());
    auto buffer = memory.take_value();
    std::memcpy(buffer.data(), data, bytes);
    auto plan = builder.prepare_append(field, 1, std::move(buffer).freeze());
    if (plan.ok())
      ++output_rows[field];
    return plan;
  }
  Poll write_layer(const ResultProgramPhase& phase, const LayerPixel& pixel,
                   unsigned first = 0) {
    const std::array<float, 4> coverage{pixel.coverage.p[0],
                                        pixel.coverage.p[1],
                                        pixel.coverage.p[2], pixel.coverage.a};
    auto a = write(phase, first, coverage.data(), 16),
         b = write(phase, first + 1, pixel.emission.data(), 12);
    if (!a.ok() || !b.ok())
      return Poll(!a.ok() ? a.status() : b.status());
    return Poll(ResultProgramNeed{{}, {}, {a.take_value(), b.take_value()}});
  }
  Poll finish(const ResultProgramPhase& phase) {
    if (op == Op::Flatten) {
      auto published = std::move(image).publish(phase.query.output.facets);
      if (!published.ok())
        return Poll(published.status());
      const auto value = published.take_value();
      auto fragments = ValueFragments::create_view(
          phase.query.output.descriptor, phase.query.output.facets,
          *phase.query.value_outputs, &value, 1);
      if (!fragments.ok())
        return Poll(fragments.status());
      auto support = relation(phase, spec.height * spec.width * 4);
      return support.ok() ? Poll(ResultValuePublication{fragments.take_value(),
                                                        support.take_value()})
                          : Poll(support.status());
    }
    for (unsigned field = 0; field < builder.reference().schema().fields.size();
         ++field) {
      auto support = relation(phase, output_rows[field]);
      if (!support.ok())
        return Poll(support.status());
      auto status =
          builder.publish(field, output_rows[field], support.take_value(),
                          {true, true, true, true});
      if (!status.ok())
        return Poll(status);
    }
    auto result = builder.seal();
    return result.ok() ? Poll(ResultPublication{result.take_value(), true})
                       : Poll(result.status());
  }
  Poll poll(const ResultProgramPhase& phase) {
    input_internal::Float32Environment environment;
    if (!environment.active())
      return Poll(Status{ErrorCode::OperationFailed,
                         "strict layer environment unavailable"});
    if (stage == 0) {
      count = spec.height * spec.width;
      ResultProgramNeed need;
      for (unsigned i = 0; i < phase.query.inputs.size(); ++i)
        if (phase.query.inputs[i].result_schema)
          need.results.push_back({i, 0, true, 0});
      stage = 1;
      if (!need.results.empty())
        return Poll(std::move(need));
    }
    if (stage == 1) {
      for (const auto& item : phase.results) {
        inputs[item.first] = item.second;
        auto facts = item.second.descriptor();
        if (!facts.ok())
          return Poll(facts.status());
        descriptors[item.first] = facts.value();
      }
      if (op == Op::Reduce)
        count = descriptors[0].rows(0);
      if (op == Op::Flatten) {
        const auto whole = Footprint::all(phase.query.output.descriptor.shape);
        if (!whole.ok())
          return Poll(whole.status());
        if (*phase.query.value_outputs != whole.value())
          return Poll(invalid("layer.flatten requires Whole output request"));
        auto allocated = MutableValue::allocate(
            phase.query.output.descriptor,
            Region::whole(phase.query.output.descriptor.shape),
            phase.allocator);
        if (!allocated.ok())
          return Poll(allocated.status());
        image = allocated.take_value();
      } else {
        auto admission = phase.resources.reserve(ResourceCapacity::host(
            2 * sizeof(std::uint64_t), 2 * sizeof(std::uint64_t)));
        if (!admission.ok())
          return Poll(admission.status());
        std::vector<std::uint64_t> association;
        association.reserve(2);
        for (const auto& input : inputs)
          if (input.valid())
            association.push_back(input.object_id());
        auto limits = growth_limits(op, spec);
        if (!limits.ok())
          return Poll(limits.status());
        auto made = ResultBuilder::start(
            phase.resources, *phase.query.output.result_schema,
            phase.query.semantic_key, limits.take_value(),
            std::move(association));
        if (!made.ok())
          return Poll(made.status());
        builder = made.take_value();
        auto support = relation(phase, 1);
        if (!support.ok())
          return Poll(support.status());
        auto bound = builder.bind_descriptor_relation(support.take_value());
        if (!bound.ok())
          return Poll(bound);
      }
      stage = 2;
      if (op == Op::Reduce && count) {
        auto s = descend(count);
        if (!s.ok())
          return Poll(s);
      }
      if (op == Op::RequireValid) {
        auto valid = inputs[0].prepare_read(descriptors[0], 0, 0, 1);
        if (!valid.ok())
          return Poll(valid.status());
        stage = 5;
        return Poll(ResultProgramNeed{{}, {}, {valid.take_value()}});
      }
    }
    if (stage == 5) {
      const auto& page =
          std::get<std::shared_ptr<const CpuStorage>>(phase.io.at(0));
      if (page->bytes().data()[0] != 1)
        return Poll(Status{ErrorCode::OperationFailed,
                           "empty weighted result cannot become an image",
                           FailureReason::EmptyWeightedResult});
      stage = 2;
    }
    if (stage == 4)
      return finish(phase);
    if (op == Op::Reduce) {
      if (stage == 3) {
        const auto& page =
            std::get<std::shared_ptr<const CpuStorage>>(phase.io.at(0));
        for (std::uint64_t i = 0; i < batch; ++i) {
          auto charged = phase.consume_work(8);
          if (!charged.ok())
            return Poll(charged);
          WeightedLayerSum leaf;
          std::memcpy(leaf.components.data(), page->bytes().data() + i * 64,
                      64);
          auto checked = validate_layer_contribution({leaf.components});
          if (!checked.ok())
            return Poll(checked);
          for (unsigned c = 0; c < 7; ++c) {
            volatile double product = leaf.components[c] * leaf.components[7];
            if (!std::isfinite(product))
              return Poll(Status{ErrorCode::OperationFailed,
                                 "weighted leaf overflow",
                                 FailureReason::ArithmeticOverflow});
            leaf.components[c] = product;
          }
          checked = accept(leaf, phase);
          if (!checked.ok())
            return Poll(checked);
        }
        row += batch;
        stage = 2;
      }
      if (row == count) {
        auto checked = validate_weighted_layer_sum(sum);
        if (!checked.ok()) {
          checked.code = ErrorCode::OperationFailed;
          if (checked.reason == FailureReason::InvalidAssociation)
            checked.reason = FailureReason::AssociationUnderflow;
          return Poll(checked);
        }
        if (depth)
          return Poll(Status{ErrorCode::Internal, "incomplete weighted tree"});
        auto plan = write(phase, 0, sum.components.data(), 64);
        if (!plan.ok())
          return Poll(plan.status());
        stage = 4;
        return Poll(ResultProgramNeed{{}, {}, {plan.take_value()}});
      }
      batch = std::min(count - row, phase.query.page_bytes / 64);
      if (!batch)
        return Poll(Status{ErrorCode::ResourceExhausted,
                           "weighted record exceeds page window"});
      auto plan = inputs[0].prepare_read(descriptors[0], 0, row, batch);
      if (!plan.ok())
        return Poll(plan.status());
      stage = 3;
      return Poll(ResultProgramNeed{{}, {}, {plan.take_value()}});
    }
    if (stage == 3) {
      auto charged = phase.consume_work(32);
      if (!charged.ok())
        return Poll(charged);
      std::array<LayerPixel, 2> pixels;
      std::array<float, 3> rgb{};
      std::array<float, 4> record{};
      WeightedLayerSum weighted;
      std::size_t io = 0;
      for (unsigned i = 0; i < phase.query.inputs.size(); ++i) {
        if (!inputs[i].valid()) {
          const auto width = phase.query.inputs[i].descriptor.shape[2];
          for (unsigned c = 0; c < width; ++c) {
            float value = 0;
            auto read = phase.read(i, {row / spec.width, row % spec.width, c},
                                   &value, 4);
            if (!read.ok())
              return Poll(read);
            if (width == 4) {
              if (c == 3)
                pixels[i].coverage.a = value;
              else
                pixels[i].coverage.p[c] = value;
            } else {
              rgb[c] = value;
            }
          }
          if (op == Op::Assemble && i == 1)
            pixels[0].emission = rgb;
        } else {
          const auto family = input_kind(op);
          const auto& page =
              std::get<std::shared_ptr<const CpuStorage>>(phase.io.at(io++));
          if (family == LayerRepresentation::WeightedSum) {
            std::memcpy(weighted.components.data(), page->bytes().data(), 64);
          } else {
            std::memcpy(record.data(), page->bytes().data(), 16);
            pixels[i].coverage = {{record[0], record[1], record[2]}, record[3]};
            if (family == LayerRepresentation::Layer ||
                family == LayerRepresentation::OptionalLayer) {
              const auto& emission =
                  std::get<std::shared_ptr<const CpuStorage>>(
                      phase.io.at(io++));
              std::memcpy(pixels[i].emission.data(), emission->bytes().data(),
                          12);
            }
          }
        }
      }
      ++row;
      stage = 2;
      if (op == Op::Flatten) {
        auto flattened = layer_flatten(pixels[0], rgb);
        if (!flattened.ok())
          return Poll(flattened.status());
        const auto& p = flattened.value();
        const std::array<float, 4> output{p.p[0], p.p[1], p.p[2], p.a};
        std::memcpy(image.data() + (row - 1) * 16, output.data(), 16);
      } else if (op == Op::Weight) {
        const auto& p = pixels[0];
        auto checked = validate_layer(p);
        if (!checked.ok())
          return Poll(checked);
        LayerContribution contribution{
            {p.coverage.p[0], p.coverage.p[1], p.coverage.p[2], p.coverage.a,
             p.emission[0], p.emission[1], p.emission[2],
             std::get<double>(phase.query.parameters.at("weight"))}};
        auto plan = write(phase, 0, contribution.components.data(), 64);
        if (!plan.ok())
          return Poll(plan.status());
        return Poll(ResultProgramNeed{{}, {}, {plan.take_value()}});
      } else if (op == Op::Finalize) {
        auto result = weighted_layer_finalize(weighted);
        if (!result.ok())
          return Poll(result.status());
        const std::uint8_t valid = result.value().valid ? 1 : 0;
        auto bit = write(phase, 0, &valid, 1);
        if (!bit.ok())
          return Poll(bit.status());
        ResultProgramNeed need{{}, {}, {bit.take_value()}};
        if (valid) {
          auto values = write_layer(phase, result.value().value, 1);
          if (!values.ok())
            return values;
          auto extra = std::get<ResultProgramNeed>(values.take_value());
          for (auto& action : extra.io)
            need.io.push_back(std::move(action));
        }
        return Poll(std::move(need));
      } else if (op == Op::Response || op == Op::ResponseOver) {
        auto response =
            op == Op::Response
                ? layer_response(pixels[0])
                : response_over({pixels[0].coverage.p, pixels[0].coverage.a},
                                {pixels[1].coverage.p, pixels[1].coverage.a});
        if (!response.ok())
          return Poll(response.status());
        const auto& v = response.value();
        const std::array<float, 4> data{v.q[0], v.q[1], v.q[2], v.t};
        auto plan = write(phase, 0, data.data(), 16);
        if (!plan.ok())
          return Poll(plan.status());
        return Poll(ResultProgramNeed{{}, {}, {plan.take_value()}});
      } else if (op == Op::CoverageRawPlus) {
        auto raw = raw_rgba_plus(pixels[0].coverage, pixels[1].coverage);
        if (!raw.ok())
          return Poll(raw.status());
        const auto& v = raw.value();
        const std::array<float, 4> data{v.p[0], v.p[1], v.p[2], v.mass};
        auto plan = write(phase, 0, data.data(), 16);
        if (!plan.ok())
          return Poll(plan.status());
        return Poll(ResultProgramNeed{{}, {}, {plan.take_value()}});
      } else {
        Result<LayerPixel> result(pixels[0]);
        if (op == Op::Over)
          result = layer_over(pixels[0], pixels[1]);
        if (op == Op::Opacity)
          result = layer_opacity(pixels[0],
                                 static_cast<float>(std::get<double>(
                                     phase.query.parameters.at("factor"))));
        if (op == Op::EmitFront || op == Op::EmitBehind)
          result = layer_emit(pixels[0], rgb,
                              static_cast<float>(std::get<double>(
                                  phase.query.parameters.at("factor"))),
                              op == Op::EmitBehind);
        if (op == Op::RawChecked || op == Op::RawCapped) {
          auto coverage =
              raw_rgba_coverage({pixels[0].coverage.p, pixels[0].coverage.a},
                                op == Op::RawCapped);
          if (!coverage.ok())
            return Poll(coverage.status());
          result = Result<LayerPixel>(LayerPixel{coverage.take_value(), {}});
        }
        if (!result.ok())
          return Poll(result.status());
        auto checked = validate_layer(result.value());
        if (!checked.ok())
          return Poll(checked);
        return write_layer(phase, result.value());
      }
    }
    if (row == count)
      return finish(phase);
    ResultProgramNeed need;
    for (unsigned i = 0; i < phase.query.inputs.size(); ++i) {
      if (!inputs[i].valid()) {
        const auto& shape = phase.query.inputs[i].descriptor.shape;
        auto footprint =
            Footprint::from_regions(shape, {Region({{row / spec.width, 1},
                                                    {row % spec.width, 1},
                                                    {0, shape[2]}})});
        if (!footprint.ok())
          return Poll(footprint.status());
        need.values.push_back({i, footprint.take_value()});
      } else {
        const auto family = input_kind(op);
        const unsigned first =
            family == LayerRepresentation::OptionalLayer ? 1 : 0;
        const unsigned fields =
            family == LayerRepresentation::Layer ||
                    family == LayerRepresentation::OptionalLayer
                ? 2
                : 1;
        for (unsigned f = 0; f < fields; ++f) {
          auto read = inputs[i].prepare_read(descriptors[i], first + f, row, 1);
          if (!read.ok())
            return Poll(read.status());
          if (read.value().byte_size() > phase.query.page_bytes)
            return Poll(Status{ErrorCode::ResourceExhausted,
                               "layer record exceeds page window"});
          need.io.push_back(read.take_value());
        }
      }
    }
    stage = 3;
    return Poll(std::move(need));
  }
};
}  // namespace
Result<OperationDefinition> make_layer_operation(LayerOperation operation,
                                                 const LayerSpec& spec) {
  const auto number = static_cast<unsigned>(operation);
  if (!number || number > 15)
    return Result<OperationDefinition>(invalid("unknown layer operation"));
  auto raster = layer_schema(LayerRepresentation::Layer, spec);
  if (!raster.ok())
    return Result<OperationDefinition>(raster.status());
  if (number >= static_cast<unsigned>(Op::Reduce) &&
      (spec.height != 1 || spec.width != 1))
    return Result<OperationDefinition>(invalid(
        "weighted reduction/finalization output must have one location"));
  OperationDefinition definition;
  definition.key = std::string("layer.") + kNames[number];
  auto& traits = definition.traits;
  traits.input_count = binary(operation) ? 2 : 1;
  traits.input_schema.resize(traits.input_count);
  auto& out = traits.outputs[0];
  out.region_rule = OperationRegionRule::Dependency;
  out.dependency_version = 2;
  out.continuation_bytes = sizeof(State);
  out.maximum_dependency_stages = 1000000;
  traits.workspace_bytes = 4096;
  for (unsigned i = 0; i < traits.input_count; ++i) {
    auto& port = traits.input_schema[i];
    if (operation == Op::Assemble ||
        (i == 1 && (operation == Op::EmitFront || operation == Op::EmitBehind ||
                    operation == Op::Flatten))) {
      if (operation == Op::Assemble && i == 0)
        port.kind = OperationPortKind::RgbaFloat32;
    } else {
      auto schema =
          layer_schema(input_kind(operation),
                       operation == Op::Reduce || operation == Op::Finalize ||
                               operation == Op::RequireValid
                           ? LayerSpec{}
                           : spec);
      if (!schema.ok())
        return Result<OperationDefinition>(schema.status());
      port.kind = OperationPortKind::Result;
      port.result_schema_id = std::string(schema.value().id);
      port.result_schema_version = 1;
    }
  }
  if (operation == Op::Opacity || operation == Op::EmitFront ||
      operation == Op::EmitBehind) {
    const double bound = std::numeric_limits<float>::max();
    traits.parameter_schema.push_back(
        {"factor", OperationParameterType::Float64, true, true,
         operation == Op::Opacity ? 0 : -bound,
         operation == Op::Opacity ? 1 : bound});
  }
  if (operation == Op::Weight)
    traits.parameter_schema.push_back(
        {"weight", OperationParameterType::Float64, true, true, 0,
         std::numeric_limits<double>::max()});
  if (operation == Op::Flatten) {
    out.output_element_type = ElementType::Float32;
    out.shape_rule = OperationShapeRule::Fixed;
    out.fixed_output_shape = {spec.height, spec.width, 4};
    out.output_schema.kind = OperationPortKind::Typed;
    out.output_schema.semantic_kind =
        static_cast<std::uint32_t>(SemanticKind::Image);
    out.output_schema.element_type =
        static_cast<std::uint32_t>(ElementType::Float32);
    out.output_schema.rank = 3;
    out.output_semantic_rule = OperationSemanticRule::Establish;
    auto facet = encode_semantic(rgba_semantics());
    if (!facet.ok())
      return Result<OperationDefinition>(facet.status());
    out.output_facets = {facet.take_value()};
    out.output_schema.facets = out.output_facets;
  } else {
    auto schema =
        layer_schema(output_kind(operation), operation == Op::Weight ||
                                                     operation == Op::Reduce ||
                                                     operation == Op::Finalize
                                                 ? LayerSpec{}
                                                 : spec);
    if (!schema.ok())
      return Result<OperationDefinition>(schema.status());
    out.result_schema = schema.take_value();
    out.output_schema.kind = OperationPortKind::Result;
    out.output_schema.result_schema_id = std::string(out.result_schema->id);
    out.output_schema.result_schema_version = 1;
  }
  definition.start_result = [operation, spec](
                                const ResultProgramQuery& query,
                                const BufferAllocator& allocator) {
    for (unsigned i = 0; i < query.inputs.size(); ++i) {
      const auto& input = query.inputs[i];
      if (input.result_schema) {
        auto decoded = layer_spec(*input.result_schema);
        if (!decoded.ok())
          return Result<ResultContinuation>(decoded.status());
        const auto expected = operation == Op::Reduce ||
                                      operation == Op::Finalize ||
                                      operation == Op::RequireValid
                                  ? LayerSpec{}
                                  : spec;
        if (decoded.value().height != expected.height ||
            decoded.value().width != expected.width ||
            decoded.value().working_space != expected.working_space)
          return Result<ResultContinuation>(
              invalid("Layer raster or working-space mismatch"));
      } else {
        const auto width = operation == Op::Assemble && i == 0 ? 4U : 3U;
        if (input.descriptor.element_type != ElementType::Float32 ||
            (input.descriptor.shape.size() != 3 ||
             input.descriptor.shape[0] != spec.height ||
             input.descriptor.shape[1] != spec.width ||
             input.descriptor.shape[2] != width) ||
            (width == 3 && !input.facets.empty()))
          return Result<ResultContinuation>(
              invalid("explicit layer Value input contract mismatch"));
      }
    }
    return ResultContinuation::make<State>(allocator, operation, spec);
  };
  return Result<OperationDefinition>(std::move(definition));
}
}  // namespace ps
