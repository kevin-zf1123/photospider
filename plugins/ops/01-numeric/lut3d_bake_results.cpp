#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "01-numeric/exact_bake_error.hpp"
#include "01-numeric/lut3d_bake_common.hpp"
#include "data/dependency_metadata.hpp"
#include "data/lut3d_bake_validation.hpp"
#include "plugin/builtin_operations.hpp"

namespace ps::plugin_internal {
namespace {
using namespace bake_ops;  // NOLINT(build/namespaces)
enum class Kind { Pack, Measure, Unpack, Gate };
constexpr std::uint64_t kMeasureRows = 64;
struct BakeProgram {
  ColorModel input_model, output_model;
  std::uint64_t atol, rtol;
};
Lut3dBakeDescription placeholder() {
  Lut3dBakeDescription result;
  result.recipe_identity.assign(64, '0');
  return result;
}
Result<Footprint> rows(const ValueDescriptor& descriptor, std::uint64_t first,
                       std::uint64_t count) {
  auto at = coordinate(first * 3, descriptor.shape);
  std::vector<RegionDimension> dimensions;
  for (auto value : at)
    dimensions.push_back({value, 1});
  dimensions[dimensions.size() - 2].extent = count;
  dimensions.back() = {0, 3};
  return Footprint::from_regions(descriptor.shape, {Region(dimensions)});
}
Result<std::vector<OperationOutputSpecialization>> specialize(
    Kind kind, const std::vector<OperationMetadata>& inputs,
    const Parameters& parameters) {
  using Answer = Result<std::vector<OperationOutputSpecialization>>;
  const unsigned count = kind == Kind::Measure ? 6 : kind == Kind::Gate ? 2 : 1;
  if (inputs.size() != count)
    return Answer(mismatch("bake Result input count"));
  auto decoded = (kind == Kind::Pack || kind == Kind::Measure)
                     ? description(parameters)
                     : (inputs[0].result_schema
                            ? lut3d_bake_description(*inputs[0].result_schema)
                            : Result<Lut3dBakeDescription>(
                                  mismatch("owned bake table required")));
  if (!decoded.ok())
    return Answer(decoded.status());
  const auto& spec = decoded.value();
  auto table_schema = lut3d_bake_table_schema(spec),
       report_schema = lut3d_bake_schema(spec);
  if (!table_schema.ok() || !report_schema.ok())
    return Answer(!table_schema.ok() ? table_schema.status()
                                     : report_schema.status());
  const ValueDescriptor table{spec.table_dtype,
                              {spec.shape[0], spec.shape[1], spec.shape[2], 3}};
  auto facet = encode_color_array(spec.output_description);
  if (!facet.ok())
    return Answer(facet.status());
  OperationOutputSpecialization output;
  if (kind == Kind::Pack) {
    if (!sole_tensor(inputs[0]) ||
        tensor_descriptor(inputs[0]).element_type != table.element_type ||
        tensor_descriptor(inputs[0]).shape != table.shape)
      return Answer(mismatch("owned bake table shape/dtype"));
    auto valid = color_metadata(inputs[0], spec.output_description);
    if (!valid.ok())
      return Answer(valid);
    output.metadata.result_schema =
        std::make_shared<const SchemaTemplate>(table_schema.take_value());
  } else if (kind == Kind::Measure) {
    const auto points = product(spec.shape, 1) + spec.extra_points;
    const std::array<ValueDescriptor, 6> expected{
        ValueDescriptor{ElementType::Float64, {3, 3}},
        ValueDescriptor{ElementType::Float64, table.shape},
        table,
        ValueDescriptor{ElementType::Float64, {points, 3}},
        ValueDescriptor{spec.source_dtype, {points, 3}},
        ValueDescriptor{spec.table_dtype, {points, 3}}};
    for (unsigned port = 0; port < 6; ++port) {
      if (port == 2) {
        if (!inputs[port].result_schema ||
            !inputs[port].result_schema->same_schema(table_schema.value()))
          return Answer(mismatch("measurement owned table schema"));
        continue;
      }
      if (!sole_tensor(inputs[port]) ||
          tensor_descriptor(inputs[port]).element_type !=
              expected[port].element_type ||
          tensor_descriptor(inputs[port]).shape != expected[port].shape)
        return Answer(mismatch("measurement port shape/dtype"));
      if (port) {
        auto checked =
            color_metadata(inputs[port], port < 4 ? spec.input_description
                                                  : spec.output_description);
        if (!checked.ok())
          return Answer(checked);
      }
    }
    output.metadata.result_schema =
        std::make_shared<const SchemaTemplate>(report_schema.take_value());
  } else {
    if (!inputs[0].result_schema ||
        !inputs[0].result_schema->same_schema(table_schema.value()))
      return Answer(mismatch("bake table schema mismatch"));
    if (kind == Kind::Gate &&
        (!inputs[1].result_schema ||
         !inputs[1].result_schema->same_schema(report_schema.value())))
      return Answer(mismatch(
          "gated report must describe this exact bake recipe/dtype/method"));
    auto schema =
        numeric_ops::numeric_tensor_schema(table.element_type, table.shape);
    schema.tensors[0].facets = {facet.take_value()};
    schema.tensors[0].atomic_trailing_axes = 1;
    output.metadata.result_schema =
        std::make_shared<const SchemaTemplate>(std::move(schema));
  }
  return Answer(std::vector<OperationOutputSpecialization>{std::move(output)});
}
struct MeasureState {
  unsigned stage = 0;
  std::shared_ptr<const dependency_internal::MetadataOwner> phase_metadata;
  std::uint64_t row = 0, batch = 0;
  Lut3dBakeReport report;
  numeric_ops::ExactBakeError arithmetic;
  ResultTensorInput table;
  ColorModel input_model = ColorModel::Rgb, output_model = ColorModel::Rgb;
  std::uint64_t atol = 0, rtol = 0;
  ResultBuilder builder;
  Result<ResultRelation> relation(const ResultProgramPhase& phase,
                                  std::uint64_t count) {
    std::vector<ResultSupport> supports;
    for (unsigned i = 0; i < 6; ++i) {
      supports.push_back({i, 5, 0,
                          elements(tensor_descriptor(phase.query.inputs[i])),
                          ResultSupportTarget::Tensor, 0});
      supports.push_back({i, 8, 0, 1, ResultSupportTarget::Descriptor, 0});
    }
    return global_relation(phase, count, supports);
  }
  Poll need(const ResultProgramPhase& phase) {
    batch = std::min<std::uint64_t>(
        kMeasureRows, tensor_descriptor(phase.query.inputs[3]).shape[0] - row);
    ResultProgramNeed need;
    for (unsigned port : {3U, 4U, 5U}) {
      auto region =
          rows(tensor_descriptor(phase.query.inputs[port]), row, batch);
      if (!region.ok())
        return Poll(region.status());
      need.tensors.push_back({port, 0, region.take_value(), 13});
    }
    stage = 2;
    return Poll(std::move(need));
  }
  Poll poll(const ResultProgramPhase& phase) {
    phase_metadata.reset();
    phase_metadata = dependency_internal::metadata_owner(65536);
    if (!stage) {
      const auto& program =
          *static_cast<const BakeProgram*>(phase.query.prepared->state());
      input_model = program.input_model;
      output_model = program.output_model;
      atol = program.atol;
      rtol = program.rtol;
      ResultProgramNeed need;
      // A source may ignore generated colors entirely. The complete original
      // grid therefore remains an explicit global validation prerequisite.
      for (unsigned port : {0U, 1U, 2U}) {
        auto support = all(phase, port);
        if (!support.ok())
          return Poll(support.status());
        need.tensors.push_back({port, 0, support.take_value(), 13});
      }
      stage = 1;
      return Poll(std::move(need));
    }
    if (stage == 1) {
      table = phase.tensors->at({2, 0});
      for (unsigned i = 0; i < 9; ++i) {
        auto value = read(phase, 0, {i / 3, i % 3});
        if (!value.ok())
          return Poll(value.status());
        const auto bits = value.value();
        std::memcpy(&report.axis[i], &bits, 8);
      }
      report.validation_count =
          tensor_descriptor(phase.query.inputs[3]).shape[0];
      return need(phase);
    }
    if (stage == 2) {
      for (std::uint64_t i = 0; i < batch; ++i) {
        auto work = phase.consume_work(9);
        if (!work.ok())
          return Poll(work);
        if (phase.query.cancellation.cancelled())
          return Poll(Status{ErrorCode::Cancelled, {}});
        std::array<std::uint64_t, 3> point{}, reference{}, value{};
        const std::array<std::array<std::uint64_t, 3>*, 3> targets{
            &point, &reference, &value};
        for (unsigned p = 0; p < 3; ++p)
          for (unsigned c = 0; c < 3; ++c) {
            auto bits = read(phase, p + 3, {row + i, c});
            if (!bits.ok())
              return Poll(bits.status());
            (*targets[p])[c] = bits.value();
          }
        if (!model_valid(input_model, point) ||
            !model_valid(output_model, reference) ||
            !model_valid(output_model, value))
          return Poll(domain("measurement requires finite model-valid colors"));
        bool passed = true;
        for (unsigned c = 0; c < 3; ++c) {
          auto accepted = arithmetic.check(reference[c], value[c], atol, rtol,
                                           phase.consume_work);
          if (!accepted.ok())
            return Poll(accepted.status());
          passed &= accepted.value();
          if (row + i == 0 || arithmetic.compare(arithmetic.difference,
                                                 arithmetic.maxima[c]) > 0) {
            arithmetic.maxima[c] = arithmetic.difference;
            report.max_error_index[c] = row + i;
            std::memcpy(report.max_error_point.data() + 3 * c, point.data(),
                        24);
          }
        }
        if (!passed) {
          ++report.failed_count;
          if (report.first_failure_index < 0) {
            report.first_failure_index = row + i;
            std::memcpy(report.first_failure_input.data(), point.data(), 24);
            std::memcpy(report.first_failure_reference.data(), reference.data(),
                        24);
            std::memcpy(report.first_failure_lut.data(), value.data(), 24);
          }
        }
      }
      row += batch;
      if (row < static_cast<std::uint64_t>(report.validation_count))
        return need(phase);
      report.passed = report.failed_count == 0;
      for (unsigned c = 0; c < 3; ++c) {
        auto value = arithmetic.upward(c, phase.consume_work);
        if (!value.ok())
          return Poll(value.status());
        const auto bits = value.value();
        std::memcpy(&report.max_abs_error[c], &bits, 8);
      }
      auto made = ResultBuilder::start(
          phase.resources, *phase.query.output.result_schema,
          phase.query.semantic_key, {3, 289}, {table.object_id()});
      if (!made.ok())
        return Poll(made.status());
      builder = made.take_value();
      auto support = relation(phase, 1);
      if (!support.ok())
        return Poll(support.status());
      auto bound = builder.bind_descriptor_relation(support.take_value());
      if (!bound.ok())
        return Poll(bound);
      const std::uint8_t passed = report.passed;
      const std::array<const void*, 11> values{
          &passed,
          report.axis.data(),
          &report.validation_count,
          &report.failed_count,
          report.max_abs_error.data(),
          report.max_error_point.data(),
          report.max_error_index.data(),
          &report.first_failure_index,
          report.first_failure_input.data(),
          report.first_failure_reference.data(),
          report.first_failure_lut.data()};
      const std::array<std::uint64_t, 11> sizes{1,  72, 8,  8,  24, 72,
                                                24, 8,  24, 24, 24};
      ResultProgramNeed need;
      for (unsigned f = 0; f < 11; ++f) {
        if (sizes[f] > phase.query.page_bytes)
          return Poll(Status{ErrorCode::ResourceExhausted,
                             "bake report field exceeds Result window",
                             FailureReason::CapacityLimit});
        auto made = phase.allocator.allocate(sizes[f]);
        if (!made.ok())
          return Poll(made.status());
        auto bytes = made.take_value();
        std::memcpy(bytes.data(), values[f], sizes[f]);
        auto write = builder.prepare_append(f, (f == 1 || f == 5) ? 3 : 1,
                                            std::move(bytes).freeze());
        if (!write.ok())
          return Poll(write.status());
        need.io.push_back(write.take_value());
      }
      stage = 3;
      return Poll(std::move(need));
    }
    auto single_relation = relation(phase, 1),
         triple_relation = relation(phase, 3);
    if (!single_relation.ok() || !triple_relation.ok())
      return Poll(!single_relation.ok() ? single_relation.status()
                                        : triple_relation.status());
    for (unsigned field = 0; field < 11; ++field) {
      const auto count = (field == 1 || field == 5) ? 3 : 1;
      auto published = builder.publish(
          field, count,
          count == 1 ? single_relation.value() : triple_relation.value(),
          {true, true, true, true});
      if (!published.ok())
        return Poll(published);
    }
    auto sealed = builder.seal();
    return sealed.ok() ? Poll(ResultPublication{sealed.take_value(), true})
                       : Poll(sealed.status());
  }
};
struct TensorViewState {
  bool gate, pack;
  unsigned stage = 0;
  ResultRef report;
  numeric_ops::ExactBakeError arithmetic;
  Footprint output;
  Lut3dBakeReport fields;
  explicit TensorViewState(bool gated, bool packing)
      : gate(gated), pack(packing) {}
  Poll need_table(const ResultProgramPhase& phase) {
    stage = 4;
    ResultProgramNeed need;
    auto samples = all(phase, 0);
    if (!samples.ok())
      return Poll(samples.status());
    need.tensors.push_back({0, 0, samples.take_value(), 13});
    return Poll(std::move(need));
  }
  Poll publish(const ResultProgramPhase& phase) {
    using namespace numeric_ops;  // NOLINT(build/namespaces)
    const auto& schema = *phase.query.output.result_schema;
    const auto& shape = schema.tensors[0].sample_shape();
    auto builder = math_take(ResultBuilder::start(
        phase.resources, schema, phase.query.semantic_key, {},
        phase.association
            ? std::vector<std::uint64_t>(phase.association->begin(),
                                         phase.association->end())
            : std::vector<std::uint64_t>{}));
    auto descriptor = math_take(
        ResultRelation::cartesian(phase.resources, 1,
                                  {0, 8, 0, output.empty() ? 0U : 1U,
                                   ResultSupportTarget::Descriptor, 0}));
    if (gate) {
      auto support = math_take(
          ResultRelation::cartesian(phase.resources, 1,
                                    {1, 8, 0, output.empty() ? 0U : 1U,
                                     ResultSupportTarget::Descriptor, 0}));
      descriptor = math_take(
          ResultRelation::unite(phase.resources, {descriptor, support}));
    }
    math_require(builder.bind_descriptor_relation(std::move(descriptor)));
    if (!output.empty()) {
      const auto& input = phase.tensors->at({0, 0});
      if (gate && (report.association().size() < 3 ||
                   report.association()[2] != input.object_id()))
        return Poll(Status{ErrorCode::OperationFailed,
                           "bake report/table object mismatch",
                           FailureReason::InvalidAssociation});
      auto window =
          math_take(input.acquire(Region::whole(input.spec().sample_shape()),
                                  phase.query.cancellation));
      const auto count = math_take(schema.tensors[0].sample_count());
      std::vector<ResultMappedAxis> axes(shape.size());
      for (unsigned axis = 0; axis < shape.size(); ++axis)
        axes[axis].output_axis = axis;
      auto identity = math_take(
          ResultRelation::mapped(phase.resources, shape, Region::whole(shape),
                                 input.spec().sample_shape(), axes,
                                 {0, 1, 0, 0, ResultSupportTarget::Tensor, 0}));
      auto validation = math_take(ResultRelation::cartesian(
          phase.resources, count,
          {0, 4, 0, math_take(input.spec().sample_count()),
           ResultSupportTarget::Tensor, 0}));
      auto relation = math_take(
          ResultRelation::unite(phase.resources, {identity, validation}));
      if (gate) {
        std::vector<ResultSupport> supports;
        for (unsigned f = 0; f < report.schema().fields.size(); ++f)
          supports.push_back({1, 7, 0, (f == 1 || f == 5) ? 3U : 1U,
                              ResultSupportTarget::Field, f});
        supports.push_back({1, 8, 0, 1, ResultSupportTarget::Descriptor, 0});
        auto accepted = math_take(global_relation(phase, count, supports));
        relation = math_take(
            ResultRelation::unite(phase.resources, {relation, accepted}));
      }
      ResultTensorViewTransform transform;
      transform.source_axes.resize(shape.size());
      for (unsigned axis = 0; axis < shape.size(); ++axis)
        transform.source_axes[axis].output_axis = axis;
      for (const auto& region : output.boxes()) {
        auto status = builder.publish_tensor_view(
            0, region, window, transform, relation, {true, true, true, true},
            phase.query.cancellation);
        if (!status.ok() && status.code == ErrorCode::InvalidArgument &&
            status.message.find("ViewUnavailable") != std::string::npos) {
          status = builder.publish_tensor_kernel(
              0, region,
              [&](const auto& writers) {
                return math_callback(phase, [&] {
                  MathTensorReader reader(input, phase.query.cancellation);
                  MathTensorWriter writer(writers[0]);
                  const auto width =
                      Value::element_size(input.spec().descriptor.element_type);
                  std::vector<std::uint64_t> at;
                  for (auto axis : region.dimensions())
                    at.push_back(axis.offset);
                  const auto elements = region.element_count();
                  if (!elements.ok())
                    return elements.status();
                  for (std::uint64_t i = 0; i < elements.value(); ++i) {
                    math_require(phase.consume_work(1));
                    const auto bits = reader.bits(at);
                    std::memcpy(writer.address(at), &bits, width);
                    for (unsigned axis = at.size(); axis-- > 0;) {
                      if (++at[axis] < region.dimensions()[axis].offset +
                                           region.dimensions()[axis].extent)
                        break;
                      at[axis] = region.dimensions()[axis].offset;
                    }
                  }
                  return Status::success();
                });
              },
              relation, {true, true, true, true}, phase.query.cancellation);
        }
        math_require(status);
      }
    }
    return Poll(ResultPublication{math_take(builder.seal()), true});
  }
  Poll poll(const ResultProgramPhase& phase) try {
    using namespace numeric_ops;  // NOLINT(build/namespaces)
    auto scratch = math_take(
        phase.resources.reserve(ResourceCapacity::host(65536, 65536)));
    if (!stage) {
      const auto shape =
          phase.query.output.result_schema->tensors[0].sample_shape();
      output = phase.query.tensor_outputs ? *phase.query.tensor_outputs
                                          : math_take(Footprint::all(shape));
      if (output.empty())
        return publish(phase);
      if (pack)
        output = math_take(Footprint::all(shape));
      if (!gate)
        return need_table(phase);
      stage = 1;
      return Poll(ResultProgramNeed{{{1, 0, true, 0}}, {}});
    }
    if (stage == 1) {
      report = phase.results.at(1);
      auto facts = report.descriptor();
      if (!facts.ok())
        return Poll(facts.status());
      ResultProgramNeed need;
      for (unsigned field = 0; field < report.schema().fields.size(); ++field) {
        auto read = report.prepare_read(facts.value(), field, 0,
                                        (field == 1 || field == 5) ? 3 : 1);
        if (!read.ok())
          return Poll(read.status());
        if (read.value().byte_size() > phase.query.page_bytes)
          return Poll(Status{ErrorCode::ResourceExhausted,
                             "bake report field exceeds Result window",
                             FailureReason::CapacityLimit});
        need.io.push_back(read.take_value());
      }
      stage = 2;
      return Poll(std::move(need));
    }
    if (stage == 2) {
      std::uint8_t passed = 0;
      const std::array<void*, 11> targets{&passed,
                                          fields.axis.data(),
                                          &fields.validation_count,
                                          &fields.failed_count,
                                          fields.max_abs_error.data(),
                                          fields.max_error_point.data(),
                                          fields.max_error_index.data(),
                                          &fields.first_failure_index,
                                          fields.first_failure_input.data(),
                                          fields.first_failure_reference.data(),
                                          fields.first_failure_lut.data()};
      const std::array<std::uint64_t, 11> sizes{1,  72, 8,  8,  24, 72,
                                                24, 8,  24, 24, 24};
      for (unsigned f = 0; f < targets.size(); ++f) {
        const auto bytes =
            std::get<std::shared_ptr<const CpuStorage>>(phase.io[f])->bytes();
        if (bytes.size() != sizes[f])
          return Poll(domain("invalid bake report read window"));
        std::memcpy(targets[f], bytes.data(), sizes[f]);
      }
      auto spec = math_take(lut3d_bake_description(report.schema()));
      auto checked = input_internal::validate_lut3d_bake_report_values(
          spec, &fields, passed, phase.consume_work);
      if (!checked.ok())
        return Poll(checked);
      if (!fields.passed) {
        const auto index = fields.first_failure_index;
        std::array<std::uint64_t, 3> point{}, reference{}, lut{};
        std::memcpy(point.data(), fields.first_failure_input.data(), 24);
        std::memcpy(reference.data(), fields.first_failure_reference.data(),
                    24);
        std::memcpy(lut.data(), fields.first_failure_lut.data(), 24);
        const auto& program =
            *static_cast<const BakeProgram*>(phase.query.prepared->state());
        unsigned component = 0;
        for (; component < 3; ++component) {
          auto accepted =
              arithmetic.check(reference[component], lut[component],
                               program.atol, program.rtol, phase.consume_work);
          if (!accepted.ok())
            return Poll(accepted.status());
          if (!accepted.value())
            break;
        }
        if (component == 3)
          return Poll(domain("inconsistent failed bake report"));
        arithmetic.maxima[0] = arithmetic.difference;
        auto error = arithmetic.upward(0, phase.consume_work);
        if (!error.ok())
          return Poll(error.status());
        return Poll(Status{
            ErrorCode::OperationFailed,
            "LutApproximationToleranceExceeded index=" + std::to_string(index) +
                " component=" + std::to_string(component) +
                " point_bits=" + std::to_string(point[0]) + "," +
                std::to_string(point[1]) + "," + std::to_string(point[2]) +
                " reference_bits=" + std::to_string(reference[component]) +
                " lut_bits=" + std::to_string(lut[component]) +
                " error_upper_bits=" + std::to_string(error.value()),
            FailureReason::InvalidDomain,
            {FailureOrigin::Domain, FailureScope::Group}});
      }
      return need_table(phase);
    }
    return publish(phase);
  } catch (const Status& status) {
    numeric_ops::math_record_failure(phase, status);
    return Poll(status);
  } catch (const std::bad_alloc&) {
    auto failure = Status{ErrorCode::ResourceExhausted,
                          {},
                          FailureReason::CapacityLimit,
                          {FailureOrigin::Resource, FailureScope::Group}};
    numeric_ops::math_record_failure(phase, failure);
    return Poll(failure);
  }
};
OperationDefinition operation(Kind kind) {
  OperationDefinition result;
  result.key = kind == Kind::Pack      ? "curve.pack_lut3d"
               : kind == Kind::Measure ? "curve.measure_lut3d"
               : kind == Kind::Gate    ? "curve.gate_lut3d"
                                       : "curve.unpack_lut3d";
  auto& traits = result.traits;
  traits.input_count = kind == Kind::Measure ? 6 : kind == Kind::Gate ? 2 : 1;
  traits.input_schema.resize(traits.input_count);
  for (auto& input : traits.input_schema) {
    input.kind = OperationPortKind::Result;
    input.element_type_mask = 12;
  }
  if (kind == Kind::Measure)
    for (unsigned port : {0U, 1U, 3U})
      traits.input_schema[port].element_type_mask = 4;
  const auto object = [&](unsigned port, const char* id) {
    traits.input_schema[port].kind = OperationPortKind::Result;
    traits.input_schema[port].element_type_mask = 0;
    traits.input_schema[port].result_schema_id = id;
    traits.input_schema[port].result_schema_version =
        std::string(id) == "curve.bake_lut3d.table" ? 2 : 1;
  };
  if (kind == Kind::Measure)
    object(2, "curve.bake_lut3d.table");
  if (kind == Kind::Unpack || kind == Kind::Gate)
    object(0, "curve.bake_lut3d.table");
  if (kind == Kind::Gate)
    object(1, "curve.bake_lut3d.report");
  traits.requires_metadata_specialization = true;
  traits.workspace_bytes = kind == Kind::Measure ? 289 : 0;
  auto& out = traits.outputs[0];
  out.key = kind == Kind::Measure ? "report"
            : kind == Kind::Pack  ? "table"
                                  : "values";
  out.region_rule = OperationRegionRule::Dependency;
  out.dependency_version = 2;
  out.continuation_bytes =
      kind == Kind::Measure ? sizeof(MeasureState) : sizeof(TensorViewState);
  out.maximum_dependency_stages = 1048576;
  if (kind == Kind::Pack || kind == Kind::Measure) {
    traits.parameter_schema = report_parameters();
    auto schema = kind == Kind::Pack ? lut3d_bake_table_schema(placeholder())
                                     : lut3d_bake_schema(placeholder());
    if (!schema.ok())
      throw std::logic_error(schema.status().message);
    out.result_schema = schema.take_value();
    out.output_schema.kind = OperationPortKind::Result;
    out.output_schema.result_schema_id = std::string(out.result_schema->id);
    out.output_schema.result_schema_version = out.result_schema->version;
  }
  if (kind == Kind::Unpack || kind == Kind::Gate) {
    numeric_ops::set_whole_tensor_output(traits, ElementType::Float64,
                                         sizeof(TensorViewState));
    out.region_rule = OperationRegionRule::Dependency;
    out.maximum_dependency_stages = kind == Kind::Gate ? 4 : 2;
  }
  result.prepare_static =
      [kind](const auto& inputs,
             const auto& params) -> Result<OperationPreparation> {
    auto output = specialize(kind, inputs, params);
    if (!output.ok())
      return Result<OperationPreparation>(output.status());
    auto decoded = (kind == Kind::Pack || kind == Kind::Measure)
                       ? description(params)
                       : lut3d_bake_description(*inputs[0].result_schema);
    if (!decoded.ok())
      return Result<OperationPreparation>(decoded.status());
    const auto& spec = decoded.value();
    OperationPreparation prepared;
    prepared.outputs = output.take_value();
    prepared.state = std::make_shared<const BakeProgram>(
        BakeProgram{spec.input_description.model, spec.output_description.model,
                    raw(spec.atol), raw(spec.rtol)});
    return Result<OperationPreparation>(std::move(prepared));
  };
  result.start_result =
      [kind](const auto&, const auto& allocator) -> Result<ResultContinuation> {
    if (kind == Kind::Pack)
      return ResultContinuation::make<TensorViewState>(allocator, false, true);
    if (kind == Kind::Measure)
      return ResultContinuation::make<MeasureState>(allocator);
    return ResultContinuation::make<TensorViewState>(allocator,
                                                     kind == Kind::Gate, false);
  };
  return result;
}
}  // namespace
Status register_lut3d_bake_results(OperationRegistry* registry) {
  for (auto kind : {Kind::Pack, Kind::Measure, Kind::Unpack, Kind::Gate}) {
    auto status = registry->register_operation(operation(kind));
    if (!status.ok())
      return status;
  }
  return Status::success();
}
}  // namespace ps::plugin_internal
