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
#include "01-numeric/lut3d_bake_validation.hpp"
#include "common/registry.hpp"
#include "photospider/ops/contracts.hpp"
#include "photospider/ops/errors.hpp"
#include "photospider/plugin/tensor_helpers.hpp"
#include "photospider/plugin/tile_helpers.hpp"
#include "photospider/plugin/whole_stage.hpp"

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
Result<Footprint> rows(const TensorDescriptor& descriptor, std::uint64_t first,
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
  auto decoded = description(parameters);
  if (!decoded.ok())
    return Answer(decoded.status());
  const auto& spec = decoded.value();
  auto table_schema = lut3d_bake_table_schema(spec),
       report_schema = lut3d_bake_schema(spec);
  if (!table_schema.ok() || !report_schema.ok())
    return Answer(!table_schema.ok() ? table_schema.status()
                                     : report_schema.status());
  const TensorDescriptor table{
      spec.table_dtype,
      {spec.shape[0], spec.shape[1], spec.shape[2], 3}};
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
    const std::array<TensorDescriptor, 6> expected{
        TensorDescriptor{ElementType::Float64, {3, 3}},
        TensorDescriptor{ElementType::Float64, table.shape},
        table,
        TensorDescriptor{ElementType::Float64, {points, 3}},
        TensorDescriptor{spec.source_dtype, {points, 3}},
        TensorDescriptor{spec.table_dtype, {points, 3}}};
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
    schema.tensors[0].atomic_trailing_axes = 1;
    output.metadata.result_schema =
        std::make_shared<const SchemaTemplate>(std::move(schema));
  }
  return Answer(std::vector<OperationOutputSpecialization>{std::move(output)});
}
struct MeasureState {
  unsigned stage = 0;
  std::uint64_t row = 0, batch = 0;
  Lut3dBakeReport report;
  numeric_ops::ExactBakeError arithmetic;
  ResultTensorInput table;
  ColorModel input_model = ColorModel::Rgb, output_model = ColorModel::Rgb;
  std::uint64_t atol = 0, rtol = 0;
  ResultBuilder builder;
  Poll need(const ResultProgramPhase& phase) {
    batch = std::min<std::uint64_t>(
        kMeasureRows, tensor_descriptor(phase.query.inputs[3]).shape[0] - row);
    ResultProgramNeed need;
    for (unsigned port : {3U, 4U, 5U}) {
      auto region =
          rows(tensor_descriptor(phase.query.inputs[port]), row, batch);
      if (!region.ok())
        return Poll(region.status());
      {
        auto tile_need_0 = sdk::tile_need_for_samples(
            phase.resources, phase.query, port, 0, region.take_value());
        if (!tile_need_0.ok())
          return Result<ResultProgramPoll>(tile_need_0.status());
        need.tiles.push_back(tile_need_0.take_value());
      }
    }
    stage = 2;
    return Poll(std::move(need));
  }
  Poll poll(const ResultProgramPhase& phase) {
    auto scratch =
        phase.resources.reserve(ResourceCapacity::host(65536, 65536));
    if (!scratch.ok())
      return Poll(scratch.status());
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
        {
          auto tile_need_1 = sdk::tile_need_for_samples(
              phase.resources, phase.query, port, 0, support.take_value());
          if (!tile_need_1.ok())
            return Result<ResultProgramPoll>(tile_need_1.status());
          need.tiles.push_back(tile_need_1.take_value());
        }
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
          phase.query.semantic_key, {}, {table.object_id()});
      if (!made.ok())
        return Poll(made.status());
      builder = made.take_value();
      auto description_value = description(phase.query.parameters);
      if (!description_value.ok())
        return Poll(description_value.status());
      auto identity = lut3d_bake_configuration_id(description_value.value());
      if (!identity.ok())
        return Poll(identity.status());
      auto identity_written = builder.publish_tensor(
          11,
          Region::whole(
              phase.query.output.result_schema->tensors[11].sample_shape()),
          {reinterpret_cast<const std::uint8_t*>(identity.value().data()), 64},
          phase.query.cancellation);
      if (!identity_written.ok())
        return Poll(identity_written);
      // Report data survives shared grants and member projections unchanged.
      // The identity is Run-local and must not be inferred from association.
      std::array<std::uint8_t, 8> measured_table_id{};
      for (unsigned i = 0; i < measured_table_id.size(); ++i)
        measured_table_id[i] = (table.object_id() >> (8 * i)) & 255;
      auto measured_written = builder.publish_tensor(
          12, Region::whole({1, 8}),
          {measured_table_id.data(), measured_table_id.size()},
          phase.query.cancellation);
      if (!measured_written.ok())
        return Poll(measured_written);
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
      for (unsigned f = 0; f < 11; ++f) {
        if (sizes[f] > phase.query.page_bytes)
          return Poll(Status{ErrorCode::ResourceExhausted,
                             "bake report field exceeds Result window",
                             FailureReason::CapacityLimit});
        auto written = builder.publish_tensor(
            f,
            Region::whole(
                phase.query.output.result_schema->tensors[f].sample_shape()),
            {static_cast<const std::uint8_t*>(values[f]), sizes[f]},
            phase.query.cancellation);
        if (!written.ok())
          return Poll(written);
      }
    }
    auto sealed = builder.seal();
    return sealed.ok() ? Poll(ResultPublication{sealed.take_value(), true})
                       : Poll(sealed.status());
  }
};
struct TensorViewState {
  bool gate, pack;
  unsigned stage = 0;
  std::array<std::uint8_t, 8> measured_table_id{};
  numeric_ops::ExactBakeError arithmetic;
  Footprint output;
  Lut3dBakeReport fields;
  explicit TensorViewState(bool gated, bool packing)
      : gate(gated), pack(packing) {}
  Poll need_table(const ResultProgramPhase& phase) {
    stage = 2;
    ResultProgramNeed need;
    auto samples = all(phase, 0);
    if (!samples.ok())
      return Poll(samples.status());
    {
      auto tile_need_2 = sdk::tile_need_for_samples(
          phase.resources, phase.query, 0, 0, samples.take_value());
      if (!tile_need_2.ok())
        return Result<ResultProgramPoll>(tile_need_2.status());
      need.tiles.push_back(tile_need_2.take_value());
    }
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
    if (!output.empty()) {
      const auto& input = phase.tensors->at({0, 0});
      std::uint64_t measured = 0;
      for (unsigned i = 0; i < measured_table_id.size(); ++i)
        measured |= std::uint64_t{measured_table_id[i]} << (8 * i);
      if (gate && measured != input.object_id())
        return Poll(Status{ErrorCode::OperationFailed,
                           "bake report/table object mismatch",
                           ops_error::InvalidAssociation});
      auto window =
          math_take(input.acquire(Region::whole(input.spec().sample_shape()),
                                  phase.query.cancellation));
      ResultTensorViewTransform transform;
      transform.source_axes.resize(shape.size());
      for (unsigned axis = 0; axis < shape.size(); ++axis)
        transform.source_axes[axis].output_axis = axis;
      for (const auto& region : output.boxes()) {
        auto status = builder.publish_tensor_view(0, region, window, transform,
                                                  phase.query.cancellation);
        if (!status.ok() && status.reason == FailureReason::ViewUnavailable) {
          status = builder.publish_tensor_kernel(
              0, region,
              [&](const auto& writers) {
                return math_callback(phase, [&] {
                  MathTensorReader reader(input, phase.query.cancellation);
                  for (const auto& destination : writers) {
                    const auto& region = destination.region();
                    MathTensorWriter writer(destination);
                    const auto width = TensorSpan::element_size(
                        input.spec().descriptor.element_type);
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
                  }
                  return Status::success();
                });
              },
              phase.query.cancellation);
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
      auto step = sdk::to_whole(phase, 1);
      if (!step.ok())
        return Poll(step.status());
      if (const auto* need = std::get_if<ResultProgramNeed>(&step.value()))
        return Poll(*need);
    }
    if (stage == 1) {
      auto step = sdk::to_whole(phase, 1);
      if (!step.ok())
        return Poll(step.status());
      if (const auto* need = std::get_if<ResultProgramNeed>(&step.value()))
        return Poll(*need);
      const auto& inputs = std::get<sdk::WholeTensorInputs>(step.value());
      auto identity_window =
          inputs[12].acquire(Region::whole(inputs[12].spec().sample_shape()),
                             phase.query.cancellation);
      if (!identity_window.ok())
        return Poll(identity_window.status());
      auto identity_copied = identity_window.value().copy_to(
          measured_table_id.data(), measured_table_id.size());
      if (!identity_copied.ok())
        return Poll(identity_copied);
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
        if (sizes[f] > phase.query.page_bytes)
          return Poll(Status{ErrorCode::ResourceExhausted, {}});
        auto window =
            inputs[f].acquire(Region::whole(inputs[f].spec().sample_shape()),
                              phase.query.cancellation);
        if (!window.ok())
          return Poll(window.status());
        auto copied = window.value().copy_to(
            static_cast<std::uint8_t*>(targets[f]), sizes[f]);
        if (!copied.ok())
          return Poll(copied);
      }
      auto spec = math_take(description(phase.query.parameters));
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
    input.element_type_mask = 12;
  }
  if (kind == Kind::Measure)
    for (unsigned port : {0U, 1U, 3U})
      traits.input_schema[port].element_type_mask = 4;
  const auto object = [&](unsigned port, const char* id) {
    traits.input_schema[port].element_type_mask = 0;
    traits.input_schema[port].result_schema_id = id;
    traits.input_schema[port].result_schema_version = 2;
  };
  if (kind == Kind::Measure)
    object(2, "curve.bake_lut3d.table");
  if (kind == Kind::Unpack || kind == Kind::Gate)
    object(0, "curve.bake_lut3d.table");
  if (kind == Kind::Gate)
    object(1, "curve.bake_lut3d.report");
  traits.requires_metadata_specialization = true;
  traits.parameter_schema = report_parameters();
  traits.workspace_bytes = kind == Kind::Measure ? 361 : 0;
  // Object identity is Run-local, even with identical frozen inputs. Mark the
  // identity-producing/checking operations nondeterministic so their downstream
  // closure cannot share checkpoint or block state between Runs either.
  if (kind == Kind::Measure || kind == Kind::Gate) {
    traits.deterministic = false;
    traits.cacheable = false;
  }
  auto& out = traits.outputs[0];
  out.key = kind == Kind::Measure ? "report"
            : kind == Kind::Pack  ? "table"
                                  : "values";
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

    out.output_schema.result_schema_id = std::string(out.result_schema->id);
    out.output_schema.result_schema_version = out.result_schema->version;
  }
  if (kind == Kind::Unpack || kind == Kind::Gate) {
    numeric_ops::set_whole_tensor_output(traits, ElementType::Float64,
                                         sizeof(TensorViewState));
    out.maximum_dependency_stages = kind == Kind::Gate ? 3 : 2;
  }
  if (kind == Kind::Pack)
    out.output_schema.validators = {bake_table_contract()};
  if (kind == Kind::Measure) {
    out.output_schema.validators = {bake_report_contract()};
    traits.input_schema[2].validators = {bake_table_contract()};
  }
  if (kind == Kind::Gate)
    traits.input_schema[1].validators = {bake_report_contract()};
  if (kind == Kind::Gate || kind == Kind::Unpack)
    traits.input_schema[0].validators = {bake_table_contract()};
  result.prepare_static =
      [kind](const auto& inputs,
             const auto& params) -> Result<OperationPreparation> {
    auto output = specialize(kind, inputs, params);
    if (!output.ok())
      return Result<OperationPreparation>(output.status());
    auto decoded = description(params);
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
  if (kind == Kind::Pack || kind == Kind::Measure)
    return sdk::with_whole_stage(std::move(result), UINT64_C(1) << 48);
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
