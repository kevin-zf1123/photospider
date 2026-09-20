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
#include "plugin/builtin_operations.hpp"

namespace ps::plugin_internal {
namespace {
using namespace bake_ops;  // NOLINT(build/namespaces)
enum class Kind { Pack, Measure, Unpack, Gate };
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
    if (inputs[0].result_schema ||
        inputs[0].descriptor.element_type != table.element_type ||
        inputs[0].descriptor.shape != table.shape)
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
      if (inputs[port].result_schema ||
          inputs[port].descriptor.element_type != expected[port].element_type ||
          inputs[port].descriptor.shape != expected[port].shape)
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
    output.metadata.descriptor = table;
    output.metadata.facets = {facet.take_value()};
  }
  return Answer(std::vector<OperationOutputSpecialization>{std::move(output)});
}
struct PackState {
  unsigned stage = 0;
  std::shared_ptr<const dependency_internal::MetadataOwner> phase_metadata;
  std::uint64_t row = 0, batch = 0;
  ResultBuilder builder;
  Poll need(const ResultProgramPhase& phase) {
    const auto& descriptor = phase.query.inputs[0].descriptor;
    const auto row_width = 3 * Value::element_size(descriptor.element_type);
    batch = std::min(
        {elements(descriptor) / 3 - row,
         descriptor.shape[2] - row % descriptor.shape[2],
         std::min<std::uint64_t>(phase.query.page_bytes, 4096) / row_width});
    if (!batch)
      return Poll(Status{ErrorCode::ResourceExhausted,
                         "bake color exceeds Result window",
                         FailureReason::CapacityLimit});
    auto support = rows(descriptor, row, batch);
    if (!support.ok())
      return Poll(support.status());
    stage = 1;
    return Poll(ResultProgramNeed{{{0, support.take_value()}}, {}, {}});
  }
  Poll poll(const ResultProgramPhase& phase) {
    phase_metadata.reset();
    phase_metadata = dependency_internal::metadata_owner(65536);
    const auto& descriptor = phase.query.inputs[0].descriptor;
    const auto count = elements(descriptor) / 3;
    if (!stage) {
      auto made = ResultBuilder::start(
          phase.resources, *phase.query.output.result_schema,
          phase.query.semantic_key,
          {count, count * 3 * Value::element_size(descriptor.element_type)});
      if (!made.ok())
        return Poll(made.status());
      builder = made.take_value();
      auto relation =
          ResultRelation::cartesian(phase.resources, 1, {0, 5, 0, count * 3});
      if (!relation.ok())
        return Poll(relation.status());
      auto bound = builder.bind_descriptor_relation(relation.take_value());
      if (!bound.ok())
        return Poll(bound);
      return need(phase);
    }
    if (stage == 1) {
      const unsigned width = Value::element_size(descriptor.element_type);
      auto buffer = phase.allocator.allocate(batch * 3 * width);
      if (!buffer.ok())
        return Poll(buffer.status());
      auto bytes = buffer.take_value();
      for (std::uint64_t i = 0; i < batch * 3; ++i) {
        auto work =
            phase.consume_work(phase.values.at(0).fragments().size() + 8);
        if (!work.ok())
          return Poll(work);
        auto read = phase.read(0, coordinate(row * 3 + i, descriptor.shape),
                               bytes.data() + i * width, width);
        if (!read.ok())
          return Poll(read);
      }
      auto append = builder.prepare_append(0, batch, std::move(bytes).freeze());
      if (!append.ok())
        return Poll(append.status());
      stage = 2;
      return Poll(ResultProgramNeed{{}, {}, {append.take_value()}});
    }
    row += batch;
    if (row < count)
      return need(phase);
    auto relation =
        ResultRelation::cartesian(phase.resources, count, {0, 5, 0, count * 3});
    if (!relation.ok())
      return Poll(relation.status());
    auto published = builder.publish(0, count, relation.take_value(),
                                     {true, true, true, true});
    if (!published.ok())
      return Poll(published);
    auto complete = builder.seal();
    return complete.ok() ? Poll(ResultPublication{complete.take_value(), true})
                         : Poll(complete.status());
  }
};
struct MeasureState {
  unsigned stage = 0;
  std::shared_ptr<const dependency_internal::MetadataOwner> phase_metadata;
  std::uint64_t row = 0, batch = 0;
  Lut3dBakeReport report;
  numeric_ops::ExactBakeError arithmetic;
  ResultRef table;
  ResultBuilder builder;
  Result<ResultRelation> relation(const ResultProgramPhase& phase,
                                  std::uint64_t count) {
    std::vector<ResultSupport> supports;
    for (unsigned i = 0; i < 6; ++i) {
      if (i == 2) {
        auto facts = table.descriptor();
        if (!facts.ok())
          return Result<ResultRelation>(facts.status());
        supports.push_back({2, 5, 0, facts.value().rows(0) * 3});
        supports.push_back({2, 8, 0, 1});
      } else {
        supports.push_back(
            {i, 5, 0, elements(phase.query.inputs[i].descriptor)});
      }
    }
    return global_relation(phase, count, supports);
  }
  Poll need(const ResultProgramPhase& phase) {
    batch = std::min<std::uint64_t>(
        64, phase.query.inputs[3].descriptor.shape[0] - row);
    ResultProgramNeed need;
    for (unsigned port : {3U, 4U, 5U}) {
      auto region = rows(phase.query.inputs[port].descriptor, row, batch);
      if (!region.ok())
        return Poll(region.status());
      need.values.push_back({port, region.take_value()});
    }
    stage = 2;
    return Poll(std::move(need));
  }
  Poll poll(const ResultProgramPhase& phase) {
    phase_metadata.reset();
    phase_metadata = dependency_internal::metadata_owner(65536);
    auto decoded = lut3d_bake_description(*phase.query.output.result_schema);
    if (!decoded.ok())
      return Poll(decoded.status());
    const auto& spec = decoded.value();
    if (!stage) {
      ResultProgramNeed need;
      // A source may ignore generated colors entirely. The complete original
      // grid therefore remains an explicit global validation prerequisite.
      for (unsigned port : {0U, 1U}) {
        auto support = all(phase, port);
        if (!support.ok())
          return Poll(support.status());
        need.values.push_back({port, support.take_value()});
      }
      need.results.push_back({2, 0, true, 0});
      stage = 1;
      return Poll(std::move(need));
    }
    if (stage == 1) {
      table = phase.results.at(2);
      for (unsigned i = 0; i < 9; ++i) {
        auto value = read(phase, 0, {i / 3, i % 3});
        if (!value.ok())
          return Poll(value.status());
        const auto bits = value.value();
        std::memcpy(&report.axis[i], &bits, 8);
      }
      report.validation_count = phase.query.inputs[3].descriptor.shape[0];
      return need(phase);
    }
    if (stage == 2) {
      for (std::uint64_t i = 0; i < batch; ++i) {
        std::array<std::uint64_t, 3> point{}, reference{}, value{};
        for (unsigned c = 0; c < 3; ++c) {
          auto q = read(phase, 3, {row + i, c}),
               r = read(phase, 4, {row + i, c}),
               v = read(phase, 5, {row + i, c});
          if (!q.ok() || !r.ok() || !v.ok())
            return Poll(!q.ok()   ? q.status()
                        : !r.ok() ? r.status()
                                  : v.status());
          point[c] = q.value();
          reference[c] = r.value();
          value[c] = v.value();
        }
        if (!model_valid(spec.input_description.model, point) ||
            !model_valid(spec.output_description.model, reference) ||
            !model_valid(spec.output_description.model, value))
          return Poll(domain("measurement requires finite model-valid colors"));
        bool passed = true;
        for (unsigned c = 0; c < 3; ++c) {
          auto accepted =
              arithmetic.check(reference[c], value[c], raw(spec.atol),
                               raw(spec.rtol), phase.consume_work);
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
    for (unsigned field = 0; field < 11; ++field) {
      const auto count = (field == 1 || field == 5) ? 3 : 1;
      auto support = relation(phase, count);
      if (!support.ok())
        return Poll(support.status());
      auto published = builder.publish(field, count, support.take_value(),
                                       {true, true, true, true});
      if (!published.ok())
        return Poll(published);
    }
    auto sealed = builder.seal();
    return sealed.ok() ? Poll(ResultPublication{sealed.take_value(), true})
                       : Poll(sealed.status());
  }
};
struct UnpackState {
  bool gate;
  unsigned stage = 0;
  std::shared_ptr<const dependency_internal::MetadataOwner> phase_metadata;
  ResultRef table, report;
  ResultDescriptor descriptor;
  std::shared_ptr<const dependency_internal::MetadataOwner> construction;
  std::unique_ptr<numeric_ops::ArrayPublication> publication;
  ResourceVector<MutableValue> outputs;
  numeric_ops::ExactBakeError arithmetic;
  std::uint64_t box = 0, offset = 0, batch = 0;
  explicit UnpackState(bool gated) : gate(gated) {}
  Poll next(const ResultProgramPhase& phase) {
    const auto& region = phase.query.value_outputs->boxes()[box];
    const auto& logical = phase.query.output.descriptor.shape;
    auto rest = offset;
    std::array<std::uint64_t, 4> at{};
    for (unsigned i = 4; i; --i) {
      at[i - 1] = region.dimensions()[i - 1].offset +
                  rest % region.dimensions()[i - 1].extent;
      rest /= region.dimensions()[i - 1].extent;
    }
    const auto bytes_per_row =
        3 * Value::element_size(phase.query.output.descriptor.element_type);
    batch = std::min(
        region.dimensions()[2].offset + region.dimensions()[2].extent - at[2],
        phase.query.page_bytes / bytes_per_row);
    if (!batch)
      return Poll(Status{ErrorCode::ResourceExhausted,
                         "bake table row exceeds Result window",
                         FailureReason::CapacityLimit});
    const auto first = (at[0] * logical[1] + at[1]) * logical[2] + at[2];
    auto read = table.prepare_read(descriptor, 0, first, batch);
    if (!read.ok())
      return Poll(read.status());
    stage = 4;
    return Poll(ResultProgramNeed{{}, {}, {read.take_value()}});
  }
  Poll poll(const ResultProgramPhase& phase) {
    phase_metadata.reset();
    phase_metadata = dependency_internal::metadata_owner(65536);
    if (!stage) {
      stage = gate ? 1 : 3;
      return Poll(ResultProgramNeed{{}, {{gate ? 1U : 0U, 0, true, 0}}, {}});
    }
    if (stage == 1) {
      report = phase.results.at(1);
      auto facts = report.descriptor();
      if (!facts.ok())
        return Poll(facts.status());
      ResultProgramNeed need;
      for (unsigned field : {0U, 7U, 8U, 9U, 10U}) {
        auto read = report.prepare_read(facts.value(), field, 0, 1);
        if (!read.ok())
          return Poll(read.status());
        need.io.push_back(read.take_value());
      }
      stage = 2;
      return Poll(std::move(need));
    }
    if (stage == 2) {
      const auto passed =
          std::get<std::shared_ptr<const CpuStorage>>(phase.io[0])->bytes()[0];
      if (!passed) {
        std::int64_t index;
        std::array<std::uint64_t, 3> point{}, reference{}, lut{};
        std::memcpy(&index,
                    std::get<std::shared_ptr<const CpuStorage>>(phase.io[1])
                        ->bytes()
                        .data(),
                    8);
        std::memcpy(point.data(),
                    std::get<std::shared_ptr<const CpuStorage>>(phase.io[2])
                        ->bytes()
                        .data(),
                    24);
        std::memcpy(reference.data(),
                    std::get<std::shared_ptr<const CpuStorage>>(phase.io[3])
                        ->bytes()
                        .data(),
                    24);
        std::memcpy(lut.data(),
                    std::get<std::shared_ptr<const CpuStorage>>(phase.io[4])
                        ->bytes()
                        .data(),
                    24);
        auto spec = lut3d_bake_description(report.schema());
        if (!spec.ok())
          return Poll(spec.status());
        unsigned component = 0;
        for (; component < 3; ++component) {
          auto accepted = arithmetic.check(
              reference[component], lut[component], raw(spec.value().atol),
              raw(spec.value().rtol), phase.consume_work);
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
      stage = 3;
      return Poll(ResultProgramNeed{{}, {{0, 0, true, 0}}, {}});
    }
    if (stage == 3) {
      table = phase.results.at(0);
      if (gate && (report.association().size() != 1 ||
                   report.association()[0] != table.object_id()))
        return Poll(Status{ErrorCode::OperationFailed,
                           "bake report/table object mismatch",
                           FailureReason::InvalidAssociation});
      auto facts = table.descriptor();
      if (!facts.ok())
        return Poll(facts.status());
      descriptor = facts.take_value();
      construction = dependency_internal::metadata_owner(
          65536 + phase.query.value_outputs->boxes().size() * 8192);
      publication = std::make_unique<numeric_ops::ArrayPublication>(
          phase.query.value_outputs->boxes().size(), 4);
      for (const auto& region : phase.query.value_outputs->boxes()) {
        auto allocated = MutableValue::allocate(phase.query.output.descriptor,
                                                region, phase.allocator);
        if (!allocated.ok())
          return Poll(allocated.status());
        outputs.push_back(allocated.take_value());
      }
      return next(phase);
    }
    const auto width =
        Value::element_size(phase.query.output.descriptor.element_type);
    auto bytes =
        std::get<std::shared_ptr<const CpuStorage>>(phase.io[0])->bytes();
    if (bytes.size() != batch * 3 * width)
      return Poll(domain("bake table read window size"));
    auto work = phase.consume_work(batch * 3);
    if (!work.ok())
      return Poll(work);
    std::memcpy(
        static_cast<std::uint8_t*>(outputs[box].data()) + offset * width,
        bytes.data(), bytes.size());
    offset += batch * 3;
    std::uint64_t count = 1;
    for (const auto& d : phase.query.value_outputs->boxes()[box].dimensions())
      count *= d.extent;
    if (offset == count) {
      ++box;
      offset = 0;
    }
    if (box < outputs.size())
      return next(phase);
    ResourceVector<Value> published;
    for (auto& output : outputs) {
      auto value = std::move(output).publish(phase.query.output.facets,
                                             phase.query.resources);
      if (!value.ok())
        return Poll(value.status());
      auto retained = publication->retain(value.take_value());
      if (!retained.ok())
        return Poll(retained.status());
      published.push_back(retained.take_value());
    }
    auto value = publication->finish(
        phase.query.output.descriptor, *phase.query.value_outputs,
        published.data(), published.size(), {}, phase.query.output.facets,
        phase.query.resources);
    if (!value.ok())
      return Poll(value.status());
    const auto full = elements(phase.query.output.descriptor);
    auto identity = ResultRelation::identity(phase.resources, full, 0, 5);
    if (!identity.ok())
      return Poll(identity.status());
    auto descriptor_support =
        ResultRelation::cartesian(phase.resources, full, {0, 8, 0, 1});
    if (!descriptor_support.ok())
      return Poll(descriptor_support.status());
    std::vector<ResultRelation> parts{identity.take_value(),
                                      descriptor_support.take_value()};
    if (gate) {
      auto report_support =
          global_relation(phase, full, {{1, 7, 0, 37}, {1, 8, 0, 1}});
      if (!report_support.ok())
        return Poll(report_support.status());
      parts.push_back(report_support.take_value());
    }
    auto relation = ResultRelation::unite(phase.resources, parts);
    return relation.ok() ? Poll(ResultValuePublication{value.take_value(),
                                                       relation.take_value()})
                         : Poll(relation.status());
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
  const auto object = [&](unsigned port, const char* id) {
    traits.input_schema[port].kind = OperationPortKind::Result;
    traits.input_schema[port].result_schema_id = id;
    traits.input_schema[port].result_schema_version = 1;
  };
  if (kind == Kind::Measure)
    object(2, "curve.bake_lut3d.table");
  if (kind == Kind::Unpack || kind == Kind::Gate)
    object(0, "curve.bake_lut3d.table");
  if (kind == Kind::Gate)
    object(1, "curve.bake_lut3d.report");
  traits.requires_metadata_specialization = true;
  traits.workspace_bytes = kind == Kind::Pack      ? 4096
                           : kind == Kind::Measure ? 289
                                                   : 0;
  auto& out = traits.outputs[0];
  out.key = kind == Kind::Measure ? "report"
            : kind == Kind::Pack  ? "table"
                                  : "values";
  out.region_rule = OperationRegionRule::Dependency;
  out.dependency_version = 2;
  out.continuation_bytes = kind == Kind::Pack      ? sizeof(PackState)
                           : kind == Kind::Measure ? sizeof(MeasureState)
                                                   : sizeof(UnpackState);
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
    out.output_schema.result_schema_version = 1;
  }
  result.specialize_metadata = [kind](const auto& inputs, const auto& params) {
    return specialize(kind, inputs, params);
  };
  result.start_result =
      [kind](const auto&, const auto& allocator) -> Result<ResultContinuation> {
    if (kind == Kind::Pack)
      return ResultContinuation::make<PackState>(allocator);
    if (kind == Kind::Measure)
      return ResultContinuation::make<MeasureState>(allocator);
    return ResultContinuation::make<UnpackState>(allocator, kind == Kind::Gate);
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
