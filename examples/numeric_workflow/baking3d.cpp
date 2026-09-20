#include <cfenv>  // NOLINT(build/c++11)
#include <cstring>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
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

namespace {
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
    if (mode == 2) {
      graph.nodes.push_back({ids[0],
                             "numeric.cast",
                             {source.colors},
                             {{"dtype", std::string("float32")},
                              {"rounding", std::string("ties_even")},
                              {"overflow", std::string("reject")}}});
      return ps::Result<ps::WorkflowNodeOutput>({ids[0], "value"});
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
  ps::ExecutionBindings bindings;
  ps::numeric::BakedLut3d baked;
  ps::ExecutionOptions execution;
  ps::ResourceBindings resources;
  Fixture(unsigned mode, ps::numeric::Lut3dBakeOptions options, ps::Value axes,
          const std::vector<double>& extras = {},
          ps::ResourceBindings profiles = {})
      : resources(std::move(profiles)) {
    const auto add = [&](std::uint64_t id, const std::string& name,
                         const ps::Value& value) {
      document.inputs.push_back({id, name, value.descriptor(), value.region(),
                                 value.layout(), value.facets()});
      bindings.inputs.push_back({name, value});
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
    return context.execute(compiled.value().plan, bindings, cancellation,
                           execution);
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
  auto narrow_options = options;
  narrow_options.atol = 0;
  const auto close_axis =
      doubles({3, 3}, {1, 1 + 0x1p-23, 0x1p-23, 0, 1, 1, 0, 1, 1});
  Fixture narrow(2, narrow_options, close_axis);
  auto rounded = take(narrow.run("report"));
  auto narrow_report =
      take(ps::read_lut3d_bake_report(rounded.results.at("report")));
  require(narrow_report.passed && narrow_report.max_abs_error[0] == 0,
          "measurement applies LUT at explicit table dtype");
  auto desc =
      take(ps::lut3d_bake_description(rounded.results.at("report").schema()));
  require(desc.source_dtype == ps::ElementType::Float32 &&
              desc.table_dtype == ps::ElementType::Float32,
          "source cast and default table dtype are preserved");
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
  shared.bindings.inputs[1].value = doubles({1}, {2});
  auto changed = take(shared.run("report"));
  require(
      take(ps::read_lut3d_bake_report(changed.results.at("report"))).passed &&
          changed.results.at("report").object_id() !=
              first.results.at("report").object_id(),
      "new shared-parameter snapshot obtains new report");
  std::cout << "LUT3D repeated extras, retained failed report, explicit "
               "Float32 measurement, "
               "ignored-input grid validation and shared snapshots PASS\n";
}
void authoring(ps::CpuNumericProfile profile) {
  auto registry = ps::make_default_operation_registry();
  auto input = axis();
  ps::WorkflowDocument document;
  document.inputs.push_back(
      {1, "axis", input.descriptor(), input.region(), input.layout(), {}});
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
        staged.inputs[0].layout.origin = {1, 1};
        return ps::Result<ps::WorkflowNodeOutput>(source.colors);
      },
      options);
  require(!bad.ok() && document.nodes.empty() &&
              document.inputs[0].layout.origin.empty(),
          "builder cannot mutate existing layout origins");
  ps::ResourceBudget root;
  auto bytes = numeric_fixture::fixture();
  auto icc = take(ps::IccProfile::import({bytes.data(), bytes.size()}, root));
  auto resources = take(ps::ResourceBindings::create({icc}, root));
  ps::ColorArrayDescriptor cmyk;
  cmyk.model = ps::ColorModel::Cmyk;
  cmyk.reference = ps::ColorReference::ProfileRelative;
  cmyk.white.reset();
  cmyk.profile = icc.identity();
  document.inputs.push_back({2,
                             "cmyk",
                             {ps::ElementType::Float64, {1, 4}},
                             ps::Region::whole({1, 4}),
                             {0, {32, 8}},
                             {take(ps::encode_color_array(cmyk))}});
  auto baked = take(ps::numeric::bake_lut3d(
      document, registry, ps::WorkflowInputReference{1},
      source_builder(0, profile), options, {}, resources));
  document.outputs = {
      {"report", baked.report.source_node, baked.report.source_port}};
  ps::GraphContext graph(document);
  require(ps::Compiler(registry).compile(graph, {}, resources).ok(),
          "ICC resource bindings reach authoring and compile");
  std::cout << "LUT3D transactional source-builder origin guard and "
               "independent ICC composition PASS\n";
}
ps::ResultRef sealed(ps::ResourceBudget root, const ps::SchemaTemplate& schema,
                     const std::vector<std::vector<std::uint8_t>>& fields) {
  auto builder = take(ps::ResultBuilder::start(
      root, schema, "manual-malformed-bake", {64, 4096}));
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
  ps::OperationDefinition strided;
  strided.key = "example.strided_axis";
  strided.traits.input_count = 1;
  strided.traits.input_schema.resize(1);
  strided.traits.estimated_bytes = 73;
  strided.traits.outputs[0].shape_rule =
      ps::OperationShapeRule::PreserveFirstInput;
  strided.callback =
      [](const ps::OperationInvocation& call) -> ps::Result<ps::Value> {
    const auto bytes = call.inputs[0].bytes();
    auto allocated = call.allocator.allocate(73);
    if (!allocated.ok())
      return ps::Result<ps::Value>(allocated.status());
    auto reversed = allocated.take_value();
    for (unsigned i = 0; i < 9; ++i)
      std::memcpy(reversed.data() + 1 + (8 - i) * 8, bytes.data() + i * 8, 8);
    return ps::Value::from_storage({ps::ElementType::Float64, {3, 3}},
                                   ps::Region::whole({3, 3}), {65, {-24, -8}},
                                   std::move(reversed).freeze());
  };
  require(registry->register_operation(std::move(strided)).ok(),
          "register explicit strided axis producer");
  registry->freeze();
  fixture.registry = registry;
  const auto id =
      take(ps::numeric::available_workflow_node_ids(fixture.document, 1))[0];
  for (auto& node : fixture.document.nodes)
    if (node.id == fixture.baked.axis.source_node)
      node.inputs[0] = ps::WorkflowNodeOutput{id, "value"};
  fixture.document.nodes.push_back(
      {id, "example.strided_axis", {ps::WorkflowInputReference{1}}, {}});
  for (int mode : {FE_TONEAREST, FE_UPWARD, FE_DOWNWARD, FE_TOWARDZERO}) {
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
  ps::ValueFragments retained;
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
    auto frozen = take(context.freeze(plan.plan, fixture.bindings));
    const auto wanted = take(ps::Footprint::from_regions(
        {2, 2, 2, 3}, {ps::Region({{1, 1}, {0, 1}, {1, 1}, {1, 1}})}));
    auto partial = take(context.execute_fragments(frozen, {{"table", wanted}},
                                                  {}, fixture.execution));
    retained = partial.values.at("table");
    const auto closed = take(ps::Footprint::from_regions(
        {2, 2, 2, 3}, {ps::Region({{1, 1}, {0, 1}, {1, 1}, {0, 3}})}));
    require(retained.coverage() == closed,
            "gated partial table closes complete color");
  }
  const double expected[3]{1, 0, 1};
  for (unsigned c = 0; c < 3; ++c) {
    double value;
    require(retained.read({1, 0, 1, c}, &value, 8).ok() && value == expected[c],
            "partial table global origin survives context");
  }
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
  auto frozen = take(context.freeze(plan.plan, failed.bindings));
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
    out.dependency_version = 2;
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
    require((mode == 0 || mode == 1 || mode == 2 || (mode == 5 || mode == 6)) &&
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
      document.inputs.push_back(
          {1, "axis", input.descriptor(), input.region(), input.layout(), {}});
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
      const ps::ExecutionBindings bindings{{{"axis", input}}};
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
        require(
            table.ok() && table.value().values.at("table").descriptor().shape ==
                              std::vector<std::uint64_t>{2, 2, 2, 3},
            "passed table shape");
      document.outputs = {
          {"axis", baked.axis.source_node, baked.axis.source_port}};
      ps::GraphContext axis_graph(document);
      auto axis_plan = take(ps::Compiler(registry).compile(axis_graph));
      auto independent =
          take(context.execute(axis_plan.plan, bindings, {}, execution));
      require(independent.values.at("axis").bytes().size() == 72 &&
                  !std::memcmp(independent.values.at("axis").bytes().data(),
                               input.bytes().data(), 72),
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
    }
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
