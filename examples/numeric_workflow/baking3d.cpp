#include <algorithm>
#include <cfenv>  // NOLINT(build/c++11)
#include <cstring>
#include <iostream>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "icc_fixture.hpp"  // NOLINT(build/include_subdir)
#include "photospider/data/representation.hpp"
#include "photospider/numeric/arrays.hpp"
#include "photospider/numeric/binary.hpp"
#include "photospider/numeric/color_ramps.hpp"
#include "photospider/numeric/lut3d_baking.hpp"
#include "photospider/numeric/matrix.hpp"
#include "photospider/photospider.hpp"
#include "point_math_checks.hpp"  // NOLINT(build/include_subdir)
#include "result_fixture.hpp"     // NOLINT(build/include_subdir)

namespace {
namespace rf = numeric_result_fixture;
void require(bool value, const char* message) {
  if (!value)
    throw std::runtime_error(message);
}
template <class T>
T take(ps::Result<T> value) {
  if (!value.ok())
    throw std::runtime_error(
        value.status().message +
        " code=" + std::to_string(static_cast<unsigned>(value.status().code)) +
        " reason=" +
        std::to_string(static_cast<unsigned>(value.status().reason)) +
        " node=" + std::to_string(value.status().detail.node_id));
  return value.take_value();
}
// Source admission preserves the actual resource failure returned by Root.
ps::Result<ps::ResultRef> source(
    const ps::ResourceBudget& root, const ps::Value& value,
    const ps::SchemaTemplate& schema,
    std::string_view scope = "manual.bake3d.source") {
  using Answer = ps::Result<ps::ResultRef>;
  auto made = ps::ResultBuilder::start(root, schema, scope, {}, {}, 128, 128,
                                       value.resources());
  if (!made.ok())
    return Answer(made.status());
  auto builder = made.take_value();
  auto descriptor = ps::ResultRelation::cartesian(root, 1, {});
  if (!descriptor.ok())
    return Answer(descriptor.status());
  auto status = builder.bind_descriptor_relation(descriptor.take_value());
  if (!status.ok())
    return Answer(status);
  auto count = schema.tensors[0].sample_count();
  if (!count.ok())
    return Answer(count.status());
  auto relation = ps::ResultRelation::cartesian(root, count.value(), {});
  if (!relation.ok())
    return Answer(relation.status());
  status =
      builder.publish_tensor(0, value.region(), value.layout(), value.storage(),
                             relation.take_value(), {true, true, true, true});
  return status.ok() ? builder.seal() : Answer(status);
}
void declare(ps::WorkflowDocument* document, std::uint64_t id,
             const std::string& name, const ps::Value& value) {
  document->inputs.push_back(
      {id, name,
       std::make_shared<const ps::SchemaTemplate>(rf::source_schema(value))});
}
struct StaticSource {
  ps::Value backing;
  explicit StaticSource(ps::Value value) : backing(std::move(value)) {}
  ps::Result<ps::ResultProgramPoll> poll(const ps::ResultProgramPhase& phase) {
    auto result =
        source(phase.resources, backing, *phase.query.output.result_schema,
               phase.query.semantic_key);
    return result.ok() ? ps::Result<ps::ResultProgramPoll>(
                             ps::ResultPublication{result.take_value(), true})
                       : ps::Result<ps::ResultProgramPoll>(result.status());
  }
};
void register_source(const std::shared_ptr<ps::OperationRegistry>& registry,
                     const std::string& key, const ps::Value& backing,
                     const ps::SchemaTemplate* schema = nullptr) {
  ps::OperationDefinition producer;
  producer.key = key;
  producer.traits.input_count = 0;
  producer.traits.input_schema.clear();
  producer.traits.cacheable = false;
  auto& output = producer.traits.outputs[0];
  output.key = "values";
  output.region_rule = ps::OperationRegionRule::Whole;
  output.continuation_bytes = sizeof(StaticSource);
  output.maximum_dependency_stages = 1;
  output.result_schema = schema ? *schema : rf::source_schema(backing);
  output.output_schema.kind = ps::OperationPortKind::Result;
  output.output_schema.result_schema_id = std::string(output.result_schema->id);
  output.output_schema.result_schema_version = output.result_schema->version;
  producer.start_result = [backing](const auto&, const auto& allocator) {
    return ps::ResultContinuation::make<StaticSource>(allocator, backing);
  };
  auto status = registry->register_operation(std::move(producer));
  if (!status.ok())
    throw std::runtime_error("register Result source: " + status.message);
}
ps::Value axis() {
  const double values[9]{0, 1, 1, 0, 1, 1, 0, 1, 1};
  std::vector<std::uint8_t> bytes(sizeof(values));
  std::memcpy(bytes.data(), values, sizeof(values));
  return take(ps::Value::create({ps::ElementType::Float64, {3, 3}},
                                ps::Region::whole({3, 3}), {0, {24, 8}},
                                std::move(bytes)));
}
ps::Value doubles(std::vector<std::uint64_t> shape,
                  const std::vector<double>& values) {
  std::vector<std::uint8_t> bytes(values.size() * 8);
  std::memcpy(bytes.data(), values.data(), bytes.size());
  std::vector<std::int64_t> strides(shape.size());
  std::uint64_t stride = 8;
  for (unsigned i = shape.size(); i; --i) {
    strides[i - 1] = stride;
    stride *= shape[i - 1];
  }
  return take(ps::Value::create({ps::ElementType::Float64, shape},
                                ps::Region::whole(shape), {0, strides},
                                std::move(bytes)));
}
ps::numeric::Lut3dSourceBuilder source_builder(unsigned mode,
                                               ps::CpuNumericProfile profile) {
  return [mode, profile](ps::WorkflowDocument& graph,
                         const ps::numeric::Lut3dSourceInput& source)
             -> ps::Result<ps::WorkflowNodeOutput> {
    if (!mode)
      return ps::Result<ps::WorkflowNodeOutput>(source.colors);
    auto ids = take(
        ps::numeric::available_workflow_node_ids(graph, 5, {source.colors}));
    if (mode == 1) {
      graph.nodes.push_back(take(ps::numeric::multiply_node(
          ids[0], source.colors, source.colors, profile)));
      return ps::Result<ps::WorkflowNodeOutput>({ids[0], "values"});
    }
    if (mode == 6) {
      graph.nodes.push_back(take(ps::numeric::matrix_transform_node(
          ids[0], source.colors, ps::WorkflowInputReference{4},
          ps::WorkflowInputReference{5}, profile)));
      graph.nodes.push_back(take(ps::numeric::multiply_node(
          ids[1], ps::WorkflowNodeOutput{ids[0], "values"},
          ps::WorkflowNodeOutput{ids[0], "values"}, profile)));
      graph.nodes.push_back(take(ps::numeric::matrix_transform_node(
          ids[2], source.colors, ps::WorkflowInputReference{6},
          ps::WorkflowInputReference{5}, profile)));
      graph.nodes.push_back(take(ps::numeric::add_node(
          ids[3], ps::WorkflowNodeOutput{ids[1], "values"},
          ps::WorkflowNodeOutput{ids[2], "values"}, profile)));
      return ps::Result<ps::WorkflowNodeOutput>({ids[3], "values"});
    }
    if (mode == 5) {
      graph.nodes.push_back(take(ps::numeric::matrix_transform_node(
          ids[0], source.colors, ps::WorkflowInputReference{4},
          ps::WorkflowInputReference{5}, profile)));
      graph.nodes.push_back(take(ps::numeric::multiply_node(
          ids[1], source.colors, ps::WorkflowNodeOutput{ids[0], "values"},
          profile)));
      return ps::Result<ps::WorkflowNodeOutput>({ids[1], "values"});
    }
    // Explicit shared parameter source; mode 4 deliberately ignores colors.
    graph.nodes.push_back(take(ps::numeric::constant_node(
        ids[0], ps::WorkflowInputReference{2}, source.descriptor.shape,
        ps::numeric::ArrayLayout::View, profile)));
    if (mode == 4)
      return ps::Result<ps::WorkflowNodeOutput>({ids[0], "values"});
    graph.nodes.push_back(take(ps::numeric::multiply_node(
        ids[1], source.colors, ps::WorkflowNodeOutput{ids[0], "values"},
        profile)));
    return ps::Result<ps::WorkflowNodeOutput>({ids[1], "values"});
  };
}
struct Fixture {
  std::shared_ptr<ps::OperationRegistry> registry =
      ps::make_default_operation_registry();
  ps::WorkflowDocument document;
  std::vector<ps::Value> backing;
  ps::numeric::BakedLut3d baked;
  ps::ExecutionOptions execution;
  ps::ResourceBindings resources;
  Fixture(unsigned mode, ps::numeric::Lut3dBakeOptions options, ps::Value axes,
          const std::vector<double>& extras = {},
          ps::ResourceBindings profiles = {})
      : resources(std::move(profiles)) {
    const auto add = [&](std::uint64_t id, const std::string& name,
                         const ps::Value& value) {
      declare(&document, id, name, value);
      backing.push_back(value);
    };
    add(1, "axis", axes);
    if (mode == 3 || mode == 4)
      add(2, "gain", doubles({1}, {1}));
    if (mode == 5) {
      add(4, "matrix", doubles({3, 3}, {0, 1, 0, 0, 0, 1, 1, 0, 0}));
      add(5, "bias", doubles({3}, {0, 0, 0}));
    }
    if (mode == 6) {
      add(4, "red", doubles({3, 3}, {1, 0, 0, 0, 0, 0, 0, 0, 0}));
      add(5, "bias", doubles({3}, {0, 0, 0}));
      add(6, "green_blue", doubles({3, 3}, {0, 0, 0, 0, 1, 0, 0, 0, 1}));
    }
    std::optional<ps::numeric::Lut3dValidationPoints> points;
    if (!extras.empty()) {
      add(3, "extras", doubles({extras.size() / 3, 3}, extras));
      points = ps::numeric::Lut3dValidationPoints{ps::WorkflowInputReference{3},
                                                  extras.size() / 3};
    }
    baked = take(ps::numeric::bake_lut3d(
        document, registry, ps::WorkflowInputReference{1},
        source_builder(mode, options.profile), options, points, resources));
    execution.maximum_dependency_work = UINT64_C(1) << 30;
    execution.dependencies.maximum_work = UINT64_C(1) << 30;
  }
  ps::Result<ps::ExecutionBindings> bindings(const ps::ResourceBudget& root) {
    ps::ExecutionBindings result;
    for (std::size_t i = 0; i < backing.size(); ++i) {
      auto value = source(root, backing[i], *document.inputs[i].result_schema);
      if (!value.ok())
        return ps::Result<ps::ExecutionBindings>(value.status());
      result.inputs.push_back({document.inputs[i].name, value.take_value()});
    }
    return ps::Result<ps::ExecutionBindings>(std::move(result));
  }
  ps::Result<ps::ExecutionResult> run(
      const std::string& selected,
      const ps::CancellationToken& cancellation = {}) {
    auto ref = selected == "table"  ? baked.table
               : selected == "axis" ? baked.axis
                                    : baked.report;
    document.outputs = {{selected, ref.source_node, ref.source_port}};
    ps::GraphContext graph(document);
    auto compiled = ps::Compiler(registry).compile(graph, {}, resources);
    if (!compiled.ok())
      return ps::Result<ps::ExecutionResult>(compiled.status());
    ps::ExecutionContextConfig config;
    config.cpu_workers = 1;
    config.managed_resources = ps::ResourceLimits{};
    ps::ExecutionContext context(registry, config);
    auto bound = bindings(take(context.resource_budget()));
    if (!bound.ok())
      return ps::Result<ps::ExecutionResult>(bound.status());
    return context.execute(compiled.value().plan, bound.take_value(),
                           cancellation, execution);
  }
};
void facilities(ps::CpuNumericProfile profile) {
  auto color = ps::numeric::color_ramp_rgb_description();
  ps::numeric::Lut3dBakeOptions options{
      {2, 2, 2}, ps::Lut3dInterpolation::Trilinear,
      .1,        0,
      color,     color,
      true,      {},
      profile};
  Fixture red_square(6, options, axis());
  auto red_report = take(ps::read_lut3d_bake_report(
      take(red_square.run("report")).results.at("report")));
  require(!red_report.passed &&
              red_report.max_abs_error == std::array<double, 3>{.25, 0, 0} &&
              red_report.first_failure_reference ==
                  std::array<double, 3>{.25, .5, .5} &&
              red_report.first_failure_lut == std::array<double, 3>{.5, .5, .5},
          "normative red-square source fixture");
  Fixture repeated(1, options, axis(), {.5, .5, .5, .5, .5, .5});
  auto report = take(ps::read_lut3d_bake_report(
      take(repeated.run("report")).results.at("report")));
  require(!report.passed && report.validation_count == 3 &&
              report.failed_count == 3 && report.first_failure_index == 0 &&
              report.max_error_index[0] == 0,
          "duplicate extras count independently with earliest tie");
  ps::ResultRef retained;
  repeated.execution.result_publication = [&](ps::ValueRef output,
                                              const ps::ResultRef& value) {
    if (output.node_id == repeated.baked.report.source_node)
      retained = value;
    return ps::Status::success();
  };
  require(!repeated.run("table").ok() && retained.valid() &&
              !take(ps::read_lut3d_bake_report(retained)).passed,
          "false report remains owned after dependent table failure");
  Fixture outside(0, options, axis(), {1.25, .5, .5});
  require(outside.run("axis").ok() && !outside.run("report").ok(),
          "extra range errors do not affect axis");
  auto hue = color;
  hue.model = ps::ColorModel::Cielch;
  hue.primaries.reset();
  hue.transfer.reset();
  hue.white = ps::color_white_d50();
  hue.hue = ps::ColorHueUnit::Radian;
  auto invalid_grid_options = options;
  invalid_grid_options.input_description = hue;
  invalid_grid_options.output_description = hue;
  Fixture constant(4, invalid_grid_options,
                   doubles({3, 3}, {0, 1, 1, -1, 1, 2, 0, 1, 1}));
  require(constant.run("axis").ok(), "axis-only does not evaluate color grid");
  auto bad_grid = constant.run("report");
  require(!bad_grid.ok() &&
              bad_grid.status().reason == ps::FailureReason::InvalidDomain,
          "constant source cannot bypass full-grid model validation");
  auto shared_options = options;
  shared_options.atol = 0;
  Fixture shared(3, shared_options, axis());
  auto first = take(shared.run("report"));
  shared.backing[1] = doubles({1}, {2});
  auto changed = take(shared.run("report"));
  require(
      take(ps::read_lut3d_bake_report(changed.results.at("report"))).passed &&
          changed.results.at("report").object_id() !=
              first.results.at("report").object_id(),
      "new shared-parameter snapshot obtains new report");
  std::cout << "LUT3D repeated extras, retained failed report, ignored-input "
               "grid validation and shared snapshots PASS\n";
}
void authoring(ps::CpuNumericProfile profile) {
  auto registry = ps::make_default_operation_registry();
  auto input = axis();
  ps::WorkflowDocument document;
  declare(&document, 1, "axis", input);
  auto color = ps::numeric::color_ramp_rgb_description();
  ps::numeric::Lut3dBakeOptions options{
      {2, 2, 2}, ps::Lut3dInterpolation::Trilinear,
      0,         0,
      color,     color,
      true,      {},
      profile};
  auto bad = ps::numeric::bake_lut3d(
      document, registry, ps::WorkflowInputReference{1},
      [](ps::WorkflowDocument& staged,
         const ps::numeric::Lut3dSourceInput& source) {
        auto changed = *staged.inputs[0].result_schema;
        changed.tensors[0].key = "mutated";
        staged.inputs[0].result_schema =
            std::make_shared<const ps::SchemaTemplate>(std::move(changed));
        return ps::Result<ps::WorkflowNodeOutput>(source.colors);
      },
      options);
  require(!bad.ok() && document.nodes.empty() &&
              document.inputs[0].result_schema->tensors[0].key == "data",
          "builder cannot mutate existing Result schemas");
  ps::ResourceBudget root;
  auto bytes = numeric_fixture::fixture();
  auto icc = take(ps::IccProfile::import({bytes.data(), bytes.size()}, root));
  auto resources = take(ps::ResourceBindings::create({icc}, root));
  ps::ColorArrayDescriptor cmyk;
  cmyk.model = ps::ColorModel::Cmyk;
  cmyk.reference = ps::ColorReference::ProfileRelative;
  cmyk.white.reset();
  cmyk.profile = icc.identity();
  auto cmyk_schema = rf::source_schema(doubles({1, 4}, {0, 0, 0, 0}));
  cmyk_schema.tensors[0].facets = {take(ps::encode_color_array(cmyk))};
  cmyk_schema.tensors[0].atomic_trailing_axes = 1;
  document.inputs.push_back(
      {2, "cmyk", std::make_shared<const ps::SchemaTemplate>(cmyk_schema)});
  auto baked = take(ps::numeric::bake_lut3d(
      document, registry, ps::WorkflowInputReference{1},
      source_builder(0, profile), options, {}, resources));
  document.outputs = {
      {"report", baked.report.source_node, baked.report.source_port}};
  ps::GraphContext graph(document);
  require(ps::Compiler(registry).compile(graph, {}, resources).ok(),
          "ICC resource bindings reach authoring and compile");
  std::cout << "LUT3D transactional source-builder schema guard and "
               "independent ICC composition PASS\n";
}
ps::ResultRef sealed(ps::ResourceBudget root, const ps::SchemaTemplate& schema,
                     const std::vector<std::vector<std::uint8_t>>& fields) {
  if (!schema.tensors.empty()) {
    require(schema.tensors.size() == 1 && fields.size() == 1,
            "malformed table has one tensor backing");
    const auto& tensor = schema.tensors[0];
    const auto shape = tensor.sample_shape();
    std::vector<std::int64_t> strides(shape.size());
    std::uint64_t stride =
        ps::Value::element_size(tensor.descriptor.element_type);
    for (unsigned axis = shape.size(); axis; --axis) {
      strides[axis - 1] = stride;
      stride *= shape[axis - 1];
    }
    auto backing = take(
        ps::Value::create({tensor.descriptor.element_type, shape},
                          ps::Region::whole(shape), {0, strides}, fields[0]));
    return take(source(root, backing, schema));
  }
  auto builder = take(ps::ResultBuilder::start(
      root, schema, "manual-malformed-bake", {72, 4096}));
  require(builder
              .bind_descriptor_relation(
                  take(ps::ResultRelation::cartesian(root, 1, {0, 5, 0, 1})))
              .ok(),
          "bind malformed fixture witness");
  for (unsigned i = 0; i < fields.size(); ++i) {
    const auto count = schema.fields[i].rows.value;
    require(builder.append(i, count, {fields[i].data(), fields[i].size()}).ok(),
            "append malformed fixture");
    require(builder
                .publish(i, count,
                         take(ps::ResultRelation::cartesian(root, count,
                                                            {0, 5, 0, 1})),
                         {true, true, true, true})
                .ok(),
            "publish malformed fixture");
  }
  return take(builder.seal());
}
void representation_and_limits(ps::CpuNumericProfile profile) {
  auto color = ps::numeric::color_ramp_rgb_description();
  ps::numeric::Lut3dBakeOptions options{
      {2, 2, 2}, ps::Lut3dInterpolation::Trilinear,
      0,         0,
      color,     color,
      true,      {},
      profile};
  Fixture normal(0, options, axis());
  auto output = take(normal.run("report"));
  auto schema = output.results.at("report").schema();
  auto description = take(ps::lut3d_bake_description(schema));
  ps::ResourceBudget root;
  std::vector<std::vector<std::uint8_t>> fields;
  for (unsigned field = 0; field < schema.fields.size(); ++field)
    fields.push_back(std::vector<std::uint8_t>(
        take(schema.row_bytes(field)) * schema.fields[field].rows.value));
  fields[0][0] = 1;
  const std::int64_t count = 1, none = -1;
  std::memcpy(fields[2].data(), &count, 8);
  std::memcpy(fields[7].data(), &none, 8);
  auto bad_report = sealed(root, schema, fields);
  require(!ps::read_lut3d_bake_report(bad_report).ok() &&
              !ps::validate_representation(bad_report, root, 4096).ok(),
          "registered report rejects finite degenerate axes");
  auto table_schema = take(ps::lut3d_bake_table_schema(description));
  require(
      ps::validate_representation(
          sealed(root, table_schema, {std::vector<std::uint8_t>(8 * 3 * 8)}),
          root, 72)
          .ok(),
      "valid table across three bounded read windows");
  for (auto bad_bits :
       {UINT64_C(0x7ff0000000000000), UINT64_C(0x7ff8000000000042)}) {
    std::vector<std::uint8_t> bytes(8 * 3 * 8);
    std::memcpy(bytes.data() + 22 * 8, &bad_bits, 8);
    auto malformed = sealed(root, table_schema, {bytes});
    auto rejected = ps::validate_representation(malformed, root, 72);
    require(
        !rejected.ok() && rejected.reason == ps::FailureReason::InvalidDomain,
        "registered owned table rejects nonfinite colors");
  }
  auto lch = color;
  lch.model = ps::ColorModel::Cielch;
  lch.primaries.reset();
  lch.transfer.reset();
  lch.white = ps::color_white_d50();
  lch.hue = ps::ColorHueUnit::Radian;
  description.input_description = lch;
  description.output_description = lch;
  auto lch_schema = take(ps::lut3d_bake_table_schema(description));
  std::vector<std::uint8_t> bytes(8 * 3 * 8);
  const double negative = -1;
  std::memcpy(bytes.data() + 8, &negative, 8);
  require(
      !ps::validate_representation(sealed(root, lch_schema, {bytes}), root, 72)
           .ok(),
      "registered owned table rejects negative chroma");
  for (unsigned ceiling = 0; ceiling < 4; ++ceiling) {
    Fixture limited(0, options, axis());
    if (ceiling == 0)
      limited.execution.maximum_dependency_work = 1;
    if (ceiling == 1)
      limited.execution.dependencies.maximum_state_bytes = 1;
    if (ceiling == 2)
      limited.execution.dependencies.maximum_stages = 1;
    if (ceiling == 3)
      limited.execution.maximum_result_window_bytes = 8;
    auto failed = limited.run("report");
    require(!failed.ok() &&
                failed.status().code == ps::ErrorCode::ResourceExhausted,
            "bake work/state/stage/window ceilings");
    require(limited.run("axis").ok() || ceiling < 3,
            "small Result I/O window does not constrain independent axis");
  }
  ps::ResourceLimits source_limits;
  source_limits.capacity[ps::ResourceKind::Referenced] = 1;
  ps::ResourceBudget source_root(source_limits);
  auto admission = source(source_root, normal.backing[0],
                          *normal.document.inputs[0].result_schema);
  require(!admission.ok() &&
              admission.status().code == ps::ErrorCode::ResourceExhausted &&
              admission.status().reason == ps::FailureReason::CapacityLimit,
          "low source admission budget preserves its original resource status");
  point_math_checks::released(source_root);
  auto mutated = schema;
  mutated.fields[0].element_type = ps::ElementType::Float64;
  require(!ps::lut3d_bake_description(mutated).ok(),
          "report complete schema identity");
  std::cout << "LUT3D registered report/table data validation and "
               "work/state/stage/window limits PASS\n";
}
void associations_and_streaming(ps::CpuNumericProfile profile) {
  auto color = ps::numeric::color_ramp_rgb_description();
  ps::numeric::Lut3dBakeOptions options{
      {2, 2, 2}, ps::Lut3dInterpolation::Trilinear,
      0,         0,
      color,     color,
      true,      {},
      profile};
  Fixture changed(0, options, axis());
  ps::WorkflowNode alternate;
  ps::WorkflowNodeOutput grid;
  for (const auto& node : changed.document.nodes) {
    if (node.operation == "curve.pack_lut3d")
      alternate = node;
    if (node.operation.find("curve.bake_lut3d_grid_") == 0)
      grid = {node.id, "values"};
  }
  alternate.id =
      take(ps::numeric::available_workflow_node_ids(changed.document, 1))[0];
  alternate.inputs[0] = grid;
  for (auto& node : changed.document.nodes)
    if (node.id == changed.baked.table.source_node)
      node.inputs[0] = ps::WorkflowNodeOutput{alternate.id, "table"};
  changed.document.nodes.push_back(alternate);
  auto mismatch = changed.run("table");
  require(!mismatch.ok() &&
              mismatch.status().reason == ps::FailureReason::InvalidAssociation,
          "same-schema table cannot borrow another owned table's report");
  Fixture interrupted(0, options, axis());
  ps::CancellationSource cancellation;
  bool table_sealed = false, report_sealed = false;
  interrupted.execution.result_publication = [&](ps::ValueRef,
                                                 const ps::ResultRef& result) {
    if (result.schema().id == "curve.bake_lut3d.table") {
      table_sealed = true;
      cancellation.cancel();
    }
    if (result.schema().id == "curve.bake_lut3d.report")
      report_sealed = true;
    return ps::Status::success();
  };
  auto stopped = interrupted.run("report", cancellation.token());
  require(table_sealed && !report_sealed && !stopped.ok() &&
              stopped.status().code == ps::ErrorCode::Cancelled,
          "cancelled measurement cannot publish a completed report");
  auto medium_options = options;
  medium_options.shape = {8, 8, 8};
  Fixture medium(0, medium_options,
                 doubles({3, 3}, {0, 1, 1. / 7, 0, 1, 1. / 7, 0, 1, 1. / 7}));
  medium.execution.maximum_result_window_bytes = 72;
  auto result = take(medium.run("report"));
  auto measured =
      take(ps::read_lut3d_bake_report(result.results.at("report"), 72));
  require(measured.passed && measured.validation_count == 343,
          "512-color table and 343 references use bounded multiwindow backing");
  std::cout << "LUT3D owned-object gate association, cancellation before "
               "report and bounded multiwindow baking PASS\n";
}
void regional_layouts(ps::CpuNumericProfile profile) {
  auto color = ps::numeric::color_ramp_rgb_description();
  ps::numeric::Lut3dBakeOptions options{
      {2, 2, 2}, ps::Lut3dInterpolation::Trilinear,
      0,         0,
      color,     color,
      true,      {},
      profile};
  Fixture fixture(0, options, axis());
  auto registry = ps::make_default_operation_registry(false);
  auto reversed_axis = take(ps::BufferAllocator{}.allocate(73));
  const auto original_axis = axis();
  for (unsigned i = 0; i < 9; ++i)
    std::memcpy(reversed_axis.data() + 1 + (8 - i) * 8,
                original_axis.bytes().data() + i * 8, 8);
  auto strided = take(ps::Value::from_storage(
      {ps::ElementType::Float64, {3, 3}}, ps::Region::whole({3, 3}),
      {65, {-24, -8}}, std::move(reversed_axis).freeze()));
  register_source(registry, "example.strided_axis", strided);
  auto control = std::make_shared<point_math_checks::Control>();
  std::vector<std::string> checked_keys;
  for (auto& node : fixture.document.nodes) {
    if (node.operation.find("curve.bake_lut3d_") != 0)
      continue;
    const auto original = node.operation;
    if (std::find(checked_keys.begin(), checked_keys.end(), original) ==
        checked_keys.end()) {
      node = point_math_checks::checked_node(registry, node, control);
      checked_keys.push_back(original);
    } else {
      node.operation = "manual.checked." + original;
    }
  }
  require(registry->freeze().ok(), "freeze strided geometry registry");
  fixture.registry = registry;
  const auto id =
      take(ps::numeric::available_workflow_node_ids(fixture.document, 1))[0];
  for (auto& node : fixture.document.nodes)
    if (node.id == fixture.baked.axis.source_node)
      node.inputs[0] = ps::WorkflowNodeOutput{id, "values"};
  fixture.document.nodes.push_back({id, "example.strided_axis", {}, {}});
  for (int mode : {FE_TONEAREST, FE_UPWARD, FE_DOWNWARD, FE_TOWARDZERO}) {
    control->rounding = mode;
    fenv_t saved;
    require(fegetenv(&saved) == 0 && fesetround(mode) == 0 &&
                feclearexcept(FE_ALL_EXCEPT) == 0 &&
                feraiseexcept(FE_DIVBYZERO) == 0,
            "set bake floating environment");
    auto result = take(fixture.run("report"));
    require(
        take(ps::read_lut3d_bake_report(result.results.at("report"))).passed,
        "negative unaligned axis view");
    require(fegetround() == mode && fetestexcept(FE_ALL_EXCEPT) == FE_DIVBYZERO,
            "bake preserves floating environment");
    require(fesetenv(&saved) == 0, "restore bake environment");
  }
  ps::ResultRef retained;
  ps::ResourceBudget budget;
  {
    fixture.document.outputs = {{"table", fixture.baked.table.source_node,
                                 fixture.baked.table.source_port}};
    ps::GraphContext graph(fixture.document);
    auto plan = take(ps::Compiler(fixture.registry).compile(graph));
    ps::ExecutionContextConfig config;
    config.cpu_workers = 1;
    config.managed_resources = ps::ResourceLimits{};
    ps::ExecutionContext context(fixture.registry, config);
    budget = take(context.resource_budget());
    auto frozen =
        take(context.freeze(plan.plan, take(fixture.bindings(budget))));
    const auto wanted = take(ps::Footprint::from_regions(
        {2, 2, 2, 3}, {ps::Region({{1, 1}, {0, 1}, {1, 1}, {1, 1}})}));
    auto partial = take(context.execute_fragments(frozen, {{"table", wanted}},
                                                  {}, fixture.execution));
    retained = partial.results.at("table");
    const auto closed = take(ps::Footprint::from_regions(
        {2, 2, 2, 3}, {ps::Region({{1, 1}, {0, 1}, {1, 1}, {0, 3}})}));
    require(take(retained.descriptor()).tensor_coverage(0) == closed,
            "gated partial table closes complete color");
  }
  const double expected[3]{1, 0, 1};
  for (unsigned c = 0; c < 3; ++c) {
    double value;
    require(rf::read(retained, {1, 0, 1, c}, &value, 8).ok() &&
                value == expected[c],
            "partial table global origin survives context");
  }
  require(
      control->computation_polls > 0,
      "worker fenv checks observe actual geometry continuation computation");
  require(budget.statistics().live[ps::ResourceKind::Metadata] > 0,
          "published table retains metadata admission");
  retained = {};
  require(budget.statistics().live[ps::ResourceKind::Payload] == 0 &&
              budget.statistics().live[ps::ResourceKind::Metadata] == 0,
          "final table owner releases payload and metadata");
  options.atol = .1;
  Fixture failed(1, options, axis());
  failed.document.outputs = {{"table", failed.baked.table.source_node,
                              failed.baked.table.source_port}};
  ps::GraphContext graph(failed.document);
  auto plan = take(ps::Compiler(failed.registry).compile(graph));
  ps::ExecutionContextConfig config;
  config.cpu_workers = 1;
  config.managed_resources = ps::ResourceLimits{};
  ps::ExecutionContext context(failed.registry, config);
  auto frozen = take(context.freeze(
      plan.plan, take(failed.bindings(take(context.resource_budget())))));
  auto vertex = take(ps::Footprint::from_regions(
      {2, 2, 2, 3}, {ps::Region({{0, 1}, {0, 1}, {0, 1}, {0, 3}})}));
  auto gated = context.execute_fragments(frozen, {{"table", vertex}}, {},
                                         failed.execution);
  require(!gated.ok() &&
              gated.status().message.find(
                  "LutApproximationToleranceExceeded") != std::string::npos,
          "exact requested grid vertex still requires global center quality");
  std::cout << "LUT3D negative unaligned source, floating environment, partial "
               "global gate and escaped metadata ownership PASS\n";
}
void specialization_contract(ps::CpuNumericProfile profile) {
  auto color = ps::numeric::color_ramp_rgb_description();
  ps::numeric::Lut3dBakeOptions options{
      {2, 2, 2}, ps::Lut3dInterpolation::Trilinear,
      0,         0,
      color,     color,
      true,      {},
      profile};
  Fixture fixture(0, options, axis());
  auto schema = take(fixture.run("report")).results.at("report").schema();
  for (unsigned kind = 0; kind < 5; ++kind) {
    auto registry = std::make_shared<ps::OperationRegistry>();
    ps::OperationDefinition definition;
    definition.key = "example.specialized_report";
    definition.traits.requires_metadata_specialization = true;
    auto& out = definition.traits.outputs[0];
    out.key = "report";
    out.region_rule = ps::OperationRegionRule::Dependency;
    out.continuation_bytes = 1;
    out.maximum_dependency_stages = 1;
    out.result_schema = schema;
    out.output_schema.kind = ps::OperationPortKind::Result;
    out.output_schema.result_schema_id = std::string(schema.id);
    out.output_schema.result_schema_version = 1;
    definition.specialize_metadata = [schema, kind](const auto&, const auto&) {
      ps::OperationOutputSpecialization output;
      auto specialized = schema;
      if (kind == 1)
        specialized.id = "example.other_schema";
      if (kind == 2)
        specialized.version = 2;
      if (kind == 3)
        output.metadata.descriptor.shape = {1};
      if (kind == 4)
        output.regional_atomic = true;
      output.metadata.result_schema =
          std::make_shared<const ps::SchemaTemplate>(specialized);
      return ps::Result<std::vector<ps::OperationOutputSpecialization>>(
          {output});
    };
    definition.start_result = [](const auto&, const auto&) {
      return ps::Result<ps::ResultContinuation>(
          ps::Status{ps::ErrorCode::Internal, "compile-only fixture"});
    };
    require(registry->register_operation(std::move(definition)).ok(),
            "register result specialization template");
    registry->freeze();
    ps::WorkflowDocument document;
    document.nodes = {{1, "example.specialized_report", {}, {}}};
    document.outputs = {{"report", 1, "report"}};
    ps::GraphContext graph(document);
    auto compiled = ps::Compiler(registry).compile(graph);
    require(compiled.ok() == (kind == 0),
            "Result specialization preserves kind/id/version and excludes "
            "Value flags");
  }
  std::cout << "Result schema specialization compiler admission and "
               "kind/id/version guards PASS\n";
}
ps::ColorArrayDescriptor model_description(unsigned model) {
  auto description = ps::numeric::color_ramp_rgb_description();
  const ps::ColorModel models[]{ps::ColorModel::Rgb,    ps::ColorModel::Xyz,
                                ps::ColorModel::Cielab, ps::ColorModel::Oklab,
                                ps::ColorModel::Cielch, ps::ColorModel::Oklch,
                                ps::ColorModel::Hsl,    ps::ColorModel::Ycbcr};
  require(model < 8, "color model");
  description.model = models[model];
  if (model != 0 && model != 6 && model != 7) {
    description.primaries.reset();
    description.transfer.reset();
  }
  if (model == 2 || model == 4)
    description.white = ps::color_white_d50();
  if (model == 4 || model == 5 || model == 6)
    description.hue = ps::ColorHueUnit::Radian;
  if (model == 7)
    description.ncl_coefficients =
        take(ps::color_ncl_coefficients(ps::ColorNclPreset::Bt709));
  return description;
}
ps::Value reversed_value(const ps::Value& value) {
  const auto width = ps::Value::element_size(value.descriptor().element_type);
  const auto count = value.bytes().size() / width;
  auto buffer = take(ps::BufferAllocator{}.allocate(value.bytes().size() + 1));
  for (std::size_t i = 0; i < count; ++i)
    std::memcpy(buffer.data() + 1 + (count - 1 - i) * width,
                value.bytes().data() + i * width, width);
  auto strides = value.layout().byte_strides;
  for (auto& stride : strides)
    stride = -stride;
  return take(ps::Value::from_storage(
      value.descriptor(), value.region(), {1 + (count - 1) * width, strides},
      std::move(buffer).freeze(), value.facets()));
}
void whole_geometry(ps::CpuNumericProfile profile) {
  const auto color = ps::numeric::color_ramp_rgb_description();
  ps::numeric::Lut3dBakeOptions options{
      {2, 2, 256}, ps::Lut3dInterpolation::Trilinear,
      0,           0,
      color,       color,
      true,        {},
      profile};
  const auto axes = doubles({3, 3}, {0, 1, 1, 0, 1, 1, 0, 255, 1});
  Fixture fixture(0, options, axes, std::vector<double>(128 * 3, .5));
  std::vector<ps::WorkflowNode> nodes;
  for (const auto& node : fixture.document.nodes)
    if (node.operation.find("curve.bake_lut3d_") == 0)
      nodes.push_back(node);
  auto extra = *std::find_if(nodes.begin(), nodes.end(), [](const auto& node) {
    return node.operation.find("points_extra") != std::string::npos;
  });
  extra.operation.erase(extra.operation.find("_extra"), 6);
  extra.parameters["extra_count"] = std::int64_t{0};
  nodes.push_back(extra);
  unsigned tested = 0;
  for (const auto& node : nodes) {
    const bool is_axis = node.operation.find("_axis_") != std::string::npos;
    const bool is_grid = node.operation.find("_grid_") != std::string::npos;
    const bool is_color = node.operation.find("_color_") != std::string::npos;
    const bool extras = node.operation.find("_extra_") != std::string::npos;
    std::vector<ps::Value> inputs{axes};
    if (is_color)
      inputs[0] = doubles({257, 3}, std::vector<double>(257 * 3, .5));
    if (extras)
      inputs.push_back(doubles({128, 3}, std::vector<double>(128 * 3, .5)));
    std::vector<ps::OperationMetadata> metadata;
    std::vector<ps::Region> demands;
    for (const auto& value : inputs) {
      ps::OperationMetadata item;
      item.result_schema =
          std::make_shared<const ps::SchemaTemplate>(rf::source_schema(value));
      metadata.push_back(std::move(item));
      demands.push_back(value.region());
    }
    auto traits = take(fixture.registry->resolve_traits(
        node.operation, metadata, node.parameters));
    const auto shape =
        traits.outputs[0].result_schema->tensors[0].sample_shape();
    const auto region = ps::Region::whole(shape);
    const auto bytes = take(region.element_count()) * 8;
    point_math_checks::resources(node, inputs, bytes);
    for (unsigned layout = 0; layout < 2; ++layout) {
      auto values = inputs;
      if (layout)
        for (auto& value : values)
          value = reversed_value(value);
      point_math_checks::Workflow workflow(node, values);
      const auto output = take(workflow.run()).results.at("values");
      const auto& tensor = output.schema().tensors[0];
      const auto output_bytes = rf::bytes(output);
      require(tensor.sample_shape() == shape &&
                  (is_axis ? tensor.facets.empty() : !tensor.facets.empty()) &&
                  take(output.descriptor()).tensor_coverage(0) ==
                      take(ps::Footprint::all(shape)),
              "Whole Result geometry shape, coverage and ColorArray identity");
      for (std::uint64_t i = 0; i < bytes / 8; ++i) {
        double actual;
        std::memcpy(&actual, output_bytes.data() + i * 8, 8);
        const auto row = i / 3, c = i % 3;
        double expected = .5;
        if (is_axis)
          std::memcpy(&expected, axes.bytes().data() + i * 8, 8);
        else if (is_grid)
          expected = c == 0 ? row / 512 : c == 1 ? (row / 256) % 2 : row % 256;
        else if (!is_color && row < 255)
          expected = c == 2 ? row + .5 : .5;
        require(actual == expected,
                "independent geometry axis/grid/center/extra/color values");
      }
    }
    if (is_grid) {
      auto huge = node;
      for (const auto* parameter : {"n0", "n1", "n2"})
        huge.parameters[parameter] = std::int64_t{256};
      auto full_axes = doubles({3, 3}, {0, 255, 1, 0, 255, 1, 0, 255, 1});
      ps::ResourceLimits limits;
      limits.capacity[ps::ResourceKind::Payload] = 8 * 1024 * 1024;
      point_math_checks::Workflow workflow(huge, {full_axes}, limits);
      auto failed = workflow.run();
      require(!failed.ok() &&
                  failed.status().code == ps::ErrorCode::ResourceExhausted &&
                  failed.status().reason == ps::FailureReason::CapacityLimit &&
                  failed.status().detail.node_id == huge.id,
              "maximal Whole grid output rejects bounded payload at grid node");
    }
    ++tested;
  }
  require(tested >= 5, "all five geometry registrations covered");
  auto report = take(ps::read_lut3d_bake_report(
      take(fixture.run("report")).results.at("report")));
  require(report.passed && report.validation_count == 383,
          "full 6144-byte table rows and 64-row measure windows retain report "
          "semantics");
  std::cout << "five Whole geometries: direct layout/oracle/workspace/cancel, "
               "maximal output budget and full-row Result packing PASS\n";
}
void packed_result_layouts(ps::CpuNumericProfile profile) {
  const auto color = ps::numeric::color_ramp_rgb_description();
  for (bool narrow : {false, true}) {
    const auto dtype =
        narrow ? ps::ElementType::Float32 : ps::ElementType::Float64;
    ps::numeric::Lut3dBakeOptions options{
        {2, 2, 256}, ps::Lut3dInterpolation::Trilinear,
        0,           0,
        color,       color,
        true,        dtype,
        profile};
    Fixture fixture(0, options, doubles({3, 3}, {0, 1, 1, 0, 1, 1, 0, 255, 1}));
    const std::vector<std::uint64_t> shape{2, 2, 256, 3};
    auto made = take(ps::MutableValue::allocate(
        {dtype, shape}, ps::Region::whole(shape), ps::BufferAllocator{}));
    for (unsigned i = 0; i < 3072; ++i) {
      if (narrow) {
        const float value = (i % 17) * .25f;
        std::memcpy(made.data() + i * 4, &value, 4);
      } else {
        const double value = (i % 17) * .25;
        std::memcpy(made.data() + i * 8, &value, 8);
      }
    }
    auto original = take(std::move(made).publish());
    auto declared = rf::source_schema(original);
    declared.tensors[0].facets = {take(ps::encode_color_array(color))};
    declared.tensors[0].atomic_trailing_axes = 1;
    auto reversed = reversed_value(original);
    auto registry = ps::make_default_operation_registry(false);
    register_source(registry, "example.strided_bake_table", reversed,
                    &declared);
    require(registry->freeze().ok(),
            "strided Result transport registry freeze");
    const auto id =
        take(ps::numeric::available_workflow_node_ids(fixture.document, 1))[0];
    std::uint64_t pack_id = 0;
    for (auto& node : fixture.document.nodes) {
      if (node.operation == "curve.pack_lut3d") {
        node.inputs[0] = ps::WorkflowNodeOutput{id, "values"};
        pack_id = node.id;
      }
      if (node.operation == "curve.unpack_lut3d")
        fixture.document.outputs = {{"table", node.id, "values"}};
    }
    fixture.document.nodes.push_back(
        {id, "example.strided_bake_table", {}, {}});
    ps::GraphContext graph(fixture.document);
    auto plan = take(ps::Compiler(registry).compile(graph));
    for (auto window : {UINT64_C(72), UINT64_C(4096), UINT64_C(6144),
                        UINT64_C(16384), UINT64_C(65536)}) {
      ps::ExecutionContextConfig config;
      config.cpu_workers = 1;
      config.managed_resources = ps::ResourceLimits{};
      ps::ExecutionContext context(registry, config);
      auto execution = fixture.execution;
      execution.maximum_result_window_bytes = window;
      auto output = take(context.execute(
          plan.plan, take(fixture.bindings(take(context.resource_budget()))),
          {}, execution));
      if (window == 65536) {
        const auto timing = std::find_if(
            output.diagnostics.operation_timings.begin(),
            output.diagnostics.operation_timings.end(),
            [&](const auto& item) { return item.output.node_id == pack_id; });
        require(timing != output.diagnostics.operation_timings.end() &&
                    timing->invocation_count == 2,
                "complete 2x2x256 table packs through a tensor Need and view "
                "publication");
      }
      const auto& value = output.results.at("table");
      const auto bytes = rf::bytes(value);
      const auto authorized = take(value.acquire_tensor(
          take(value.descriptor()), 0, ps::Region::whole(shape)));
      const auto row = take(authorized.row_run({0, 0, 0, 0}));
      require(
          row.data == reversed.storage()->bytes().data() +
                          reversed.layout().byte_offset &&
              take(context.resource_budget())
                      .statistics()
                      .live[ps::ResourceKind::Payload] == 0,
          "pack/unpack retain legal reversed owner as zero-copy Result views");
      require(bytes.size() == original.bytes().size() &&
                  std::memcmp(bytes.data(), original.bytes().data(),
                              bytes.size()) == 0 &&
                  value.schema().tensors[0].facets.front().payload ==
                      declared.tensors[0].facets.front().payload,
              "packed Result collect preserves negative/unaligned Float32/64 "
              "rows and color metadata");
    }
  }
  std::cout << "Result pack/unpack negative-unaligned Float32/64 full rows and "
               "72-byte windows PASS\n";
}
struct FailedSource {
  std::shared_ptr<unsigned> calls;
  explicit FailedSource(std::shared_ptr<unsigned> count)
      : calls(std::move(count)) {}
  ps::Result<ps::ResultProgramPoll> poll(const ps::ResultProgramPhase&) {
    ++*calls;
    return ps::Result<ps::ResultProgramPoll>(ps::Status{
        ps::ErrorCode::OperationFailed, "bake source failed in Result poll"});
  }
};
void upstream_failures(ps::CpuNumericProfile profile) {
  const auto color = ps::numeric::color_ramp_rgb_description();
  ps::numeric::Lut3dBakeOptions options{
      {2, 2, 2}, ps::Lut3dInterpolation::Trilinear,
      0,         0,
      color,     color,
      true,      {},
      profile};
  Fixture fixture(0, options, axis());
  auto registry = ps::make_default_operation_registry(false);
  auto calls = std::make_shared<unsigned>(0);
  ps::OperationDefinition definition;
  definition.key = "example.failed_bake_source";
  definition.traits.input_count = 0;
  definition.traits.input_schema.clear();
  definition.traits.cacheable = false;
  auto& output = definition.traits.outputs[0];
  output.key = "values";
  output.region_rule = ps::OperationRegionRule::Whole;
  output.continuation_bytes = sizeof(FailedSource);
  output.maximum_dependency_stages = 1;
  output.result_schema =
      rf::source_schema(doubles({2, 2, 2, 3}, std::vector<double>(24)));
  output.output_schema.kind = ps::OperationPortKind::Result;
  output.output_schema.result_schema_id = std::string(output.result_schema->id);
  output.output_schema.result_schema_version = output.result_schema->version;
  definition.start_result = [calls](const auto&, const auto& allocator) {
    return ps::ResultContinuation::make<FailedSource>(allocator, calls);
  };
  require(registry->register_operation(std::move(definition)).ok(),
          "register failing Result bake source");
  require(registry->freeze().ok(), "freeze failing source registry");
  fixture.registry = registry;
  const auto id =
      take(ps::numeric::available_workflow_node_ids(fixture.document, 1))[0];
  for (auto& node : fixture.document.nodes)
    if (node.operation.find("curve.bake_lut3d_color") == 0 &&
        std::get<std::string>(node.parameters.at("dtype")) == "float64") {
      const auto input = std::get<ps::WorkflowNodeOutput>(node.inputs[0]);
      const auto source_node = std::find_if(
          fixture.document.nodes.begin(), fixture.document.nodes.end(),
          [&](const auto& source) { return source.id == input.source_node; });
      if (source_node->operation.find("curve.bake_lut3d_grid") == 0)
        node.inputs[0] = ps::WorkflowNodeOutput{id, "values"};
    }
  fixture.document.nodes.push_back({id, "example.failed_bake_source", {}, {}});
  require(fixture.run("axis").ok() && *calls == 0,
          "axis-only skips failed source computation");
  fixture.document.outputs = {{"table", fixture.baked.table.source_node,
                               fixture.baked.table.source_port}};
  {
    ps::GraphContext graph(fixture.document);
    auto plan = take(ps::Compiler(registry).compile(graph));
    ps::ExecutionContextConfig config;
    config.cpu_workers = 1;
    config.managed_resources = ps::ResourceLimits{};
    ps::ExecutionContext context(registry, config);
    auto frozen = take(context.freeze(
        plan.plan, take(fixture.bindings(take(context.resource_budget())))));
    auto empty = take(context.execute_fragments(
        frozen,
        {{"table", take(ps::Footprint::from_regions({2, 2, 2, 3}, {}))}}, {},
        fixture.execution));
    require(*calls == 0 && take(empty.results.at("table").descriptor())
                               .tensor_coverage(0)
                               .empty(),
            "Empty table skips failed source and global report computation");
  }
  for (const auto* selected : {"report", "table"}) {
    auto failed = fixture.run(selected);
    require(!failed.ok() &&
                failed.status().code == ps::ErrorCode::OperationFailed &&
                failed.status().detail.node_id == id &&
                failed.status().message == "bake source failed in Result poll",
            "report/table preserve upstream Result source failure provenance");
  }
  require(*calls == 2,
          "each dependent bake run observes one failed source poll");
  std::cout
      << "LUT3D real upstream Result failures, axis independence and Empty "
         "table with no source/report work PASS\n";
}
void cache_and_preparation(ps::CpuNumericProfile profile) {
  const auto color = ps::numeric::color_ramp_rgb_description();
  ps::numeric::Lut3dBakeOptions options{
      {2, 2, 2}, ps::Lut3dInterpolation::Trilinear,
      0,         0,
      color,     color,
      true,      {},
      profile};
  Fixture fixture(3, options, axis());
  fixture.document.outputs = {
      {"table", fixture.baked.table.source_node,
       fixture.baked.table.source_port},
      {"axis", fixture.baked.axis.source_node, fixture.baked.axis.source_port},
      {"report", fixture.baked.report.source_node,
       fixture.baked.report.source_port}};
  ps::GraphContext graph(fixture.document);
  auto compiled = take(ps::Compiler(fixture.registry).compile(graph));
  std::vector<std::shared_ptr<const ps::PreparedOperation>> preparation;
  for (const auto& step : compiled.plan.steps()) {
    require(step.prepared != nullptr, "bake static preparation exists");
    preparation.push_back(step.prepared);
  }
  ps::ExecutionContextConfig config;
  config.cpu_workers = 1;
  config.result_cache_bytes = 64 * 1024 * 1024;
  config.maximum_dependency_cache_metadata = 1048576;
  config.managed_resources = ps::ResourceLimits{};
  ps::ExecutionContext context(fixture.registry, config);
  const auto root = take(context.resource_budget());
  auto bound = take(fixture.bindings(root));
  const auto frozen = take(context.freeze(compiled.plan, bound));
  auto execution = fixture.execution;
  execution.maximum_dependency_cache_work = 128 * 1024 * 1024;
  std::map<std::uint64_t, ps::ResultRef> publications;
  execution.result_publication = [&](ps::ValueRef output,
                                     const ps::ResultRef& result) {
    publications[output.node_id] = result;
    return ps::Status::success();
  };
  const ps::DemandQuery query{{"table", take(ps::Footprint::all({2, 2, 2, 3}))},
                              {"axis", take(ps::Footprint::all({3, 3}))}};
  auto demand = take(context.open_demand(compiled.plan, bound));
  auto cold = take(demand.request(query, {}, execution));
  auto repeated = take(demand.request(query, {}, execution));
  require(cold.results.at("table").object_id() ==
              repeated.results.at("table").object_id(),
          "same bake demand retains completed table Result");
  auto fresh = take(fixture.bindings(root));
  publications.clear();
  auto warm = take(context.execute_fragments(
      take(context.freeze(compiled.plan, fresh)), query, {}, execution));
  require(
      warm.diagnostics.cache_hits > 0 &&
          rf::bytes(cold.results.at("table")) ==
              rf::bytes(warm.results.at("table")) &&
          take(ps::read_lut3d_bake_report(
                   publications.at(fixture.baked.report.source_node)))
              .passed,
      "fresh same-content Result inputs reuse bake cache with identical table");
  const auto axis_association = warm.results.at("axis").association();
  require(
      std::find(axis_association.begin(), axis_association.end(),
                fresh.inputs[0].result.object_id()) != axis_association.end() &&
          std::find(axis_association.begin(), axis_association.end(),
                    bound.inputs[0].result.object_id()) ==
              axis_association.end(),
      "cached axis records current direct source association");
  std::uint64_t owned_node = 0;
  for (const auto& node : fixture.document.nodes)
    if (node.operation == "curve.pack_lut3d")
      owned_node = node.id;
  const auto report_association =
      publications.at(fixture.baked.report.source_node).association();
  require(report_association.size() >= 3 &&
              report_association[2] == publications.at(owned_node).object_id(),
          "warm report associates its current owned table");
  const auto table_association = warm.results.at("table").association();
  for (const auto node : {owned_node, fixture.baked.report.source_node})
    require(
        std::find(table_association.begin(), table_association.end(),
                  publications.at(node).object_id()) != table_association.end(),
        "gate associates its current direct table/report endpoints");
  fresh.inputs[1].result = take(source(
      root, doubles({1}, {2}), *fixture.document.inputs[1].result_schema));
  auto changed = take(context.execute(
      take(context.freeze(compiled.plan, fresh)), {}, execution));
  double sample = 0;
  require(
      rf::read(changed.results.at("table"), {1, 1, 1, 0}, &sample, 8).ok() &&
          sample == 2 &&
          take(ps::read_lut3d_bake_report(changed.results.at("report"))).passed,
      "shared parameter rebind produces newly measured table");
  fresh.inputs[0].result =
      take(source(root, doubles({3, 3}, {0, 2, 2, 0, 2, 2, 0, 2, 2}),
                  *fixture.document.inputs[0].result_schema));
  auto axis_changed = take(context.execute(
      take(context.freeze(compiled.plan, fresh)), {}, execution));
  require(
      rf::read(axis_changed.results.at("table"), {1, 1, 1, 0}, &sample, 8)
              .ok() &&
          sample == 4 &&
          take(ps::read_lut3d_bake_report(axis_changed.results.at("report")))
              .passed,
      "axis rebind changes grid and quality measurement");
  require(
      rf::read(take(context.execute(frozen, {}, execution)).results.at("table"),
               {1, 1, 1, 0}, &sample, 8)
              .ok() &&
          sample == 1,
      "earlier frozen Result bindings retain original bake");
  for (std::size_t i = 0; i < preparation.size(); ++i)
    require(compiled.plan.steps()[i].prepared == preparation[i],
            "dynamic axis/shared parameter rebinding reuses every preparation");
  auto support = take(changed.dependencies.source_support());
  require(support.at("axis") == take(ps::Footprint::all({3, 3})) &&
              support.at("gain") == take(ps::Footprint::all({1})),
          "global bake retains complete axis and shared parameter support");
  auto dirty =
      take(changed.dependencies.potential_dirty("gain", support.at("gain")));
  require(dirty.at("table") == take(ps::Footprint::all({2, 2, 2, 3})) &&
              dirty.at("axis").empty(),
          "shared parameter edit invalidates table while independent axis "
          "stays clean");
  std::cout
      << "LUT3D fresh-content cache/current associations, frozen bindings, "
         "preparation reuse and global source/dirty support PASS\n";
}
void independent_owners(ps::CpuNumericProfile profile) {
  const auto color = ps::numeric::color_ramp_rgb_description();
  for (const auto dtype :
       {ps::ElementType::Float32, ps::ElementType::Float64}) {
    ps::ResultRef table, axis_result, report;
    ps::ResultTensorReadWindow window;
    ps::ResourceBudget root;
    std::weak_ptr<const ps::CpuStorage> source_owner;
    {
      ps::numeric::Lut3dBakeOptions options{
          {2, 2, 2}, ps::Lut3dInterpolation::Trilinear,
          0,         0,
          color,     color,
          true,      dtype,
          profile};
      Fixture fixture(0, options, axis());
      source_owner = fixture.backing[0].storage();
      fixture.document.outputs = {{"table", fixture.baked.table.source_node,
                                   fixture.baked.table.source_port},
                                  {"axis", fixture.baked.axis.source_node,
                                   fixture.baked.axis.source_port},
                                  {"report", fixture.baked.report.source_node,
                                   fixture.baked.report.source_port}};
      ps::GraphContext graph(fixture.document);
      auto plan = take(ps::Compiler(fixture.registry).compile(graph));
      ps::ExecutionContextConfig config;
      config.cpu_workers = 1;
      config.managed_resources = ps::ResourceLimits{};
      ps::ExecutionContext context(fixture.registry, config);
      root = take(context.resource_budget());
      auto output = take(context.execute(
          plan.plan, take(fixture.bindings(root)), {}, fixture.execution));
      table = output.results.at("table");
      axis_result = output.results.at("axis");
      report = output.results.at("report");
      window = take(table.acquire_tensor(take(table.descriptor()), 0,
                                         ps::Region::whole({2, 2, 2, 3})));
    }
    require(source_owner.expired(),
            "bake outputs retire external axis source owner");
    require(take(ps::read_lut3d_bake_report(report)).passed,
            "independent report survives source and context retirement");
    const auto before = root.statistics().live[ps::ResourceKind::Payload];
    report = {};
    require(!report.valid() &&
                root.statistics().live[ps::ResourceKind::Payload] <= before,
            "report handle releases while dependent view ancestry may retain "
            "backing");
    double endpoint = 0;
    require(rf::read(axis_result, {0, 1}, &endpoint, 8).ok() && endpoint == 1,
            "independent axis remains readable after report release");
    axis_result = {};
    table = {};
    const auto row = take(window.row_run({1, 0, 1, 0}));
    const auto width = ps::Value::element_size(dtype);
    std::uint64_t bits = 0;
    std::memcpy(&bits, row.data, width);
    require(bits == (dtype == ps::ElementType::Float32
                         ? UINT64_C(0x3f800000)
                         : UINT64_C(0x3ff0000000000000)) &&
                root.statistics().live[ps::ResourceKind::Payload] >= 24 * width,
            "authorized table window survives independent output release");
    window = {};
    point_math_checks::released(root);
  }
  std::cout
      << "LUT3D source retirement, independent Float32/64 table/axis/report "
         "owners, escaped window and all-Root release PASS\n";
}
void probe(ps::CpuNumericProfile profile) {
  unsigned mode, method, dtype, model;
  std::array<std::uint64_t, 3> shape;
  std::size_t extra_count;
  while (std::cin >> mode >> method >> dtype >> model >> shape[0] >> shape[1] >>
         shape[2] >> extra_count) {
    const auto read_double = [] {
      std::string text;
      require(static_cast<bool>(std::cin >> text), "bake probe truncated");
      const auto bits = std::stoull(text, nullptr, 16);
      double value;
      std::memcpy(&value, &bits, 8);
      return value;
    };
    require((mode == 0 || mode == 1 || (mode == 5 || mode == 6)) &&
                method < 2 && (dtype == 3 || dtype == 4) && extra_count < 64,
            "bake probe framing");
    const auto absolute = read_double(), relative = read_double();
    std::vector<double> axes(9), extra(extra_count * 3);
    for (auto& v : axes)
      v = read_double();
    for (auto& v : extra)
      v = read_double();
    auto color = model_description(model);
    ps::numeric::Lut3dBakeOptions options{
        shape,
        method ? ps::Lut3dInterpolation::Tetrahedral
               : ps::Lut3dInterpolation::Trilinear,
        absolute,
        relative,
        color,
        color,
        true,
        static_cast<ps::ElementType>(dtype),
        profile};
    Fixture fixture(mode, options, doubles({3, 3}, axes), extra);
    auto output = fixture.run("report");
    if (!output.ok()) {
      if (output.status().reason != ps::FailureReason::InvalidDomain &&
          output.status().reason != ps::FailureReason::ArithmeticOverflow)
        throw std::runtime_error(output.status().message);
      std::cout << (output.status().reason ==
                            ps::FailureReason::ArithmeticOverflow
                        ? "overflow"
                        : "domain")
                << '\n';
      continue;
    }
    auto report =
        take(ps::read_lut3d_bake_report(output.value().results.at("report")));
    std::cout << report.passed << ' ' << report.validation_count << ' '
              << report.failed_count;
    const auto emit = [](double value) {
      std::uint64_t word;
      std::memcpy(&word, &value, 8);
      std::cout << ' ' << std::hex << word << std::dec;
    };
    for (auto v : report.max_abs_error)
      emit(v);
    for (auto index : report.max_error_index)
      std::cout << ' ' << index;
    for (auto v : report.max_error_point)
      emit(v);
    std::cout << ' ' << report.first_failure_index;
    for (auto v : report.first_failure_input)
      emit(v);
    for (auto v : report.first_failure_reference)
      emit(v);
    for (auto v : report.first_failure_lut)
      emit(v);
    std::cout << '\n';
  }
}
void examples(ps::CpuNumericProfile profile) {
  auto registry = ps::make_default_operation_registry();
  for (auto method : {ps::Lut3dInterpolation::Trilinear,
                      ps::Lut3dInterpolation::Tetrahedral}) {
    for (bool square : {false, true}) {
      auto input = axis();
      ps::WorkflowDocument document;
      declare(&document, 1, "axis", input);
      ps::numeric::Lut3dBakeOptions options{
          {2, 2, 2},
          method,
          square ? .1 : 0.,
          0.,
          ps::numeric::color_ramp_rgb_description(),
          ps::numeric::color_ramp_rgb_description(),
          true,
          {},
          profile};
      auto baked = take(ps::numeric::bake_lut3d(
          document, registry, ps::WorkflowInputReference{1},
          [square, profile](ps::WorkflowDocument& graph,
                            const ps::numeric::Lut3dSourceInput& source)
              -> ps::Result<ps::WorkflowNodeOutput> {
            if (!square)
              return ps::Result<ps::WorkflowNodeOutput>(source.colors);
            auto ids = ps::numeric::available_workflow_node_ids(
                graph, 1, {source.colors});
            if (!ids.ok())
              return ps::Result<ps::WorkflowNodeOutput>(ids.status());
            auto node = ps::numeric::multiply_node(
                ids.value()[0], source.colors, source.colors, profile);
            if (!node.ok())
              return ps::Result<ps::WorkflowNodeOutput>(node.status());
            graph.nodes.push_back(node.take_value());
            return ps::Result<ps::WorkflowNodeOutput>(
                {ids.value()[0], "values"});
          },
          options));
      document.outputs = {
          {"report", baked.report.source_node, baked.report.source_port}};
      ps::GraphContext graph(document);
      auto compiled = take(ps::Compiler(registry).compile(graph));
      ps::ExecutionContextConfig config;
      config.cpu_workers = 1;
      config.managed_resources = ps::ResourceLimits{};
      ps::ExecutionContext context(registry, config);
      ps::ExecutionOptions execution;
      execution.maximum_dependency_work = UINT64_C(1) << 30;
      execution.dependencies.maximum_work = UINT64_C(1) << 30;
      const ps::ExecutionBindings bindings{
          {{"axis", take(source(take(context.resource_budget()), input,
                                *document.inputs[0].result_schema))}}};
      auto output =
          take(context.execute(compiled.plan, bindings, {}, execution));
      auto report =
          take(ps::read_lut3d_bake_report(output.results.at("report")));
      require(report.passed == !square && report.validation_count == 1 &&
                  report.failed_count == (square ? 1 : 0),
              "measured report verdict/counts");
      require(report.first_failure_index == (square ? 0 : -1), "first failure");
      for (unsigned c = 0; c < 3; ++c)
        require(report.max_abs_error[c] == (square ? .25 : 0.) &&
                    report.max_error_index[c] == 0,
                "exact report maxima");
      document.outputs = {
          {"table", baked.table.source_node, baked.table.source_port}};
      ps::GraphContext table_graph(document);
      auto table_plan = take(ps::Compiler(registry).compile(table_graph));
      auto table = context.execute(table_plan.plan, bindings, {}, execution);
      if (square)
        require(
            !table.ok() &&
                table.status().reason == ps::FailureReason::InvalidDomain &&
                table.status().message.find(
                    "LutApproximationToleranceExceeded") != std::string::npos,
            "failed measured table is gated");
      else
        require(table.ok() && table.value()
                                      .results.at("table")
                                      .schema()
                                      .tensors[0]
                                      .sample_shape() ==
                                  std::vector<std::uint64_t>{2, 2, 2, 3},
                "passed table shape");
      document.outputs = {
          {"axis", baked.axis.source_node, baked.axis.source_port}};
      ps::GraphContext axis_graph(document);
      auto axis_plan = take(ps::Compiler(registry).compile(axis_graph));
      auto independent =
          take(context.execute(axis_plan.plan, bindings, {}, execution));
      const auto axis_bytes = rf::bytes(independent.results.at("axis"));
      require(axis_bytes.size() == 72 &&
                  !std::memcmp(axis_bytes.data(), input.bytes().data(), 72),
              "axis is independent of quality failure");
    }
  }
  std::cout << "measured LUT3D identity pass, square error=.25, "
               "report/table/axis independence, both methods PASS\n";
}
}  // namespace
int main(int argc, char** argv) {
  try {
    const std::string selected = argc > 1 ? argv[1] : "strict";
    require(selected == "strict" || selected == "apple" || selected == "x86",
            "profile");
    const auto profile = selected == "strict" ? ps::CpuNumericProfile::Strict
                         : selected == "apple"
                             ? ps::CpuNumericProfile::AppleSiliconNeon
                             : ps::CpuNumericProfile::X86Avx2;
    if (argc > 2 && std::string(argv[2]) == "--probe") {
      probe(profile);
    } else {
      examples(profile);
      facilities(profile);
      authoring(profile);
      representation_and_limits(profile);
      associations_and_streaming(profile);
      specialization_contract(profile);
      regional_layouts(profile);
      whole_geometry(profile);
      packed_result_layouts(profile);
      upstream_failures(profile);
      cache_and_preparation(profile);
      independent_owners(profile);
    }
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
