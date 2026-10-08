#include <algorithm>
#include <iostream>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "support/result_numeric_observation_fixture.hpp"

namespace {
using namespace ps::test_numeric;  // NOLINT(build/namespaces)
struct BakePlan {
  std::shared_ptr<GraphContext> graph;
  ExecutionPlan plan;
  ExecutionBindings bindings;
  numeric::BakedLut3d exports;
};
BakePlan bake_plan(Driver& d, ResultRef axis, ResultRef extras,
                   Lut3dInterpolation method, ElementType dtype,
                   bool square = false, double tolerance = 0,
                   CpuNumericProfile profile = CpuNumericProfile::Strict) {
  WorkflowDocument document;
  ExecutionBindings bindings;
  const auto bind = [&](uint64_t id, const char* name,
                        const ResultRef& result) {
    WorkflowInputDeclaration input;
    input.id = id;
    input.name = name;
    input.result_schema =
        std::make_shared<const SchemaTemplate>(result.schema());
    document.inputs.push_back(std::move(input));
    bindings.inputs.push_back({name, result});
  };
  bind(11, "axis", axis);
  auto zero = d.source({ElementType::Float64, {1}}, {0}, {1, {8}});
  bind(12, "shared", zero);
  if (extras.valid())
    bind(13, "extras", extras);
  numeric::Lut3dBakeOptions options;
  options.shape = {2, 2, 2};
  options.interpolation = method;
  options.atol = tolerance;
  options.rtol = 0;
  options.input_description = lut3d_description();
  options.output_description = options.input_description;
  options.source_pointwise = true;
  options.table_dtype = dtype;
  options.profile = profile;
  unsigned expansions = 0;
  const auto source = [&](WorkflowDocument& staged,
                          const numeric::Lut3dSourceInput& input)
      -> Result<WorkflowNodeOutput> {
    ++expansions;
    if (!square)
      return Result<WorkflowNodeOutput>(input.colors);
    auto ids = take(numeric::available_workflow_node_ids(
        staged, 2, {input.colors, WorkflowInputReference{12}}));
    staged.nodes.push_back(take(numeric::constant_node(
        ids[0], WorkflowInputReference{12}, input.descriptor.shape)));
    staged.nodes.push_back(
        {ids[1],
         "numeric.mix_strict",
         {WorkflowNodeOutput{ids[0], "values"}, input.colors, input.colors},
         {}});
    return Result<WorkflowNodeOutput>(WorkflowNodeOutput{ids[1], "values"});
  };
  auto exported = take(numeric::bake_lut3d(
      document, d.registry, WorkflowInputReference{11}, source, options,
      extras.valid()
          ? std::optional<
                numeric::Lut3dValidationPoints>{{WorkflowInputReference{13},
                                                 extras.schema()
                                                     .tensors[0]
                                                     .sample_shape()[0]}}
          : std::nullopt));
  require(expansions == 2, "bake source expands twice during authoring only");
  document.outputs = {
      {"table", exported.table.source_node, exported.table.source_port},
      {"axis", exported.axis.source_node, exported.axis.source_port},
      {"report", exported.report.source_node, exported.report.source_port}};
  for (const auto& node : document.nodes)
    if (node.operation == "curve.pack_lut3d" ||
        node.operation == "curve.unpack_lut3d")
      document.outputs.push_back(
          {node.operation == "curve.pack_lut3d" ? "owned" : "unpacked", node.id,
           node.operation == "curve.pack_lut3d" ? "table" : "values"});
  auto graph = std::make_shared<GraphContext>(std::move(document));
  auto compiled = take(Compiler(d.registry).compile(*graph));
  return {graph, std::move(compiled.plan), std::move(bindings), exported};
}
ExecutionOptions bake_execution() {
  ExecutionOptions options;
  options.maximum_dependency_work = UINT64_C(1) << 30;
  options.dependencies.maximum_work = UINT64_C(1) << 30;
  options.maximum_result_window_bytes = 72;
  return options;
}
void lut3d_baking_workflows() {
  for (auto method :
       {Lut3dInterpolation::Trilinear, Lut3dInterpolation::Tetrahedral})
    for (auto dtype : {ElementType::Float32, ElementType::Float64}) {
      Driver d(UINT64_MAX, 64 * 1048576);
      auto axes =
          d.source({ElementType::Float64, {3, 3}},
                   {double_bits(1), 0, double_bits(-1), 0, double_bits(1),
                    double_bits(1), 0, double_bits(1), double_bits(1)},
                   {1, {24, 8}});
      auto extras = d.source({ElementType::Float64, {2, 3}}, {double_bits(.5)},
                             {1, {0, 0}});
      auto prepared = bake_plan(d, axes, extras, method, dtype);
      auto frozen = take(d.context->freeze(prepared.plan, prepared.bindings));
      const auto q = take(Footprint::from_regions(
          {2, 2, 2, 3}, {Region({{1, 1}, {0, 1}, {1, 1}, {1, 1}})}));
      auto full_run = take(d.context->execute(prepared.plan, prepared.bindings,
                                              {}, bake_execution()));
      ResultRef linked_report;
      auto options = bake_execution();
      options.result_publication = [&](ValueRef source,
                                       const ResultRef& published) {
        if (source.node_id == prepared.exports.report.source_node)
          linked_report = published;
        return Status::success();
      };
      auto execution = take(d.context->execute_fragments(
          frozen,
          {{"table", q},
           {"axis", take(Footprint::all({3, 3}))},
           {"owned", take(Footprint::all({2, 2, 2, 3}))},
           {"unpacked", q}},
          {}, options));
      auto table = execution.results.at("table");
      auto owned = execution.results.at("owned");
      auto unpacked = execution.results.at("unpacked");
      auto report = linked_report;
      require(report.valid(),
              "gated report is observed in matching frozen execution");
      auto measured = take(read_lut3d_bake_report(report, 72));
      require(measured.passed && measured.validation_count == 3 &&
                  measured.failed_count == 0 &&
                  measured.first_failure_index == -1 &&
                  report.association().size() >= 6 &&
                  report.association()[2] == owned.object_id(),
              "identity bake passes centers and duplicate extras with owned "
              "table provenance");
      require(owned.schema().version == 2 && owned.schema().fields.empty() &&
                  owned.schema().tensors[0].key == "colors",
              "owned bake is canonical typed Result tensor");
      auto full = take(owned.acquire_tensor(take(owned.descriptor()), 0,
                                            Region::whole({2, 2, 2, 3})));
      const Region color({{1, 1}, {0, 1}, {1, 1}, {0, 3}});
      auto gated =
          take(table.acquire_tensor(take(table.descriptor()), 0, color));
      auto extracted =
          take(unpacked.acquire_tensor(take(unpacked.descriptor()), 0, color));
      require(
          full.storage_owner_token() == gated.storage_owner_token() &&
              full.storage_owner_token() == extracted.storage_owner_token(),
          "pack/unpack/quality gate preserve actual authorized table backing");
      require(read_bits(table, {1, 0, 1, 0}) == 0 &&
                  read_bits(table, {1, 0, 1, 2}) ==
                      (dtype == ElementType::Float32 ? float_bits(1)
                                                     : double_bits(1)),
              "gated Result coordinates preserve descending axes and local "
              "color closure");
      d.context.reset();
      require(read_bits(table, {1, 0, 1, 2}) == (dtype == ElementType::Float32
                                                     ? float_bits(1)
                                                     : double_bits(1)) &&
                  take(read_lut3d_bake_report(report, 72)).passed,
              "baked table view/report owners survive context retirement");
    }
}
BakePlan select_bake(Driver& d, const BakePlan& source,
                     const std::string& name) {
  auto document = source.graph->snapshot().document();
  const auto selected =
      std::find_if(document.outputs.begin(), document.outputs.end(),
                   [&](const auto& output) { return output.name == name; });
  require(selected != document.outputs.end(), "selected bake export exists");
  document.outputs = {*selected};
  auto graph = std::make_shared<GraphContext>(std::move(document));
  auto compiled = take(Compiler(d.registry).compile(*graph));
  return {graph, std::move(compiled.plan), source.bindings, source.exports};
}
ResultRef bake_report_copy(
    Driver& d, const ResultRef& source,
    std::optional<std::pair<unsigned, uint64_t>> corruption = {}) {
  auto associations = source.association();
  auto builder = take(ResultBuilder::start(
      d.root, source.schema(), "test.bake.report", {},
      std::vector<uint64_t>(associations.begin(), associations.end())));
  require(builder
              .bind_descriptor_relation(take(ResultRelation::cartesian(
                  d.root, 1, {0, 8, 0, 0, ResultSupportTarget::Descriptor, 0})))
              .ok(),
          "bound report descriptor");
  auto descriptor = take(source.descriptor());
  for (unsigned field = 0; field < source.schema().fields.size(); ++field) {
    const auto count = descriptor.rows(field);
    auto bytes =
        take(take(source.prepare_read(descriptor, field, 0, count)).load(72));
    std::vector<uint8_t> changed(bytes->bytes().data(),
                                 bytes->bytes().data() + bytes->bytes().size());
    if (corruption && corruption->first == field)
      std::memcpy(changed.data(), &corruption->second,
                  std::min<size_t>(8, changed.size()));
    require(builder.append(field, count, {changed.data(), changed.size()}).ok(),
            "bound report field");
    require(builder
                .publish(field, count,
                         take(ResultRelation::cartesian(
                             d.root, count,
                             {0, 1, 0, 0, ResultSupportTarget::Field, field})),
                         {true, true, true, true})
                .ok(),
            "bound report field coverage");
  }
  return take(builder.seal());
}
void lut3d_baking_boundaries() {
  for (auto method :
       {Lut3dInterpolation::Trilinear, Lut3dInterpolation::Tetrahedral}) {
    Driver d(UINT64_MAX, 64 * 1048576);
    auto axes = d.source({ElementType::Float64, {3, 3}},
                         {0, double_bits(1), double_bits(1)}, {1, {0, 8}});
    auto extras = d.source({ElementType::Float64, {2, 3}}, {double_bits(.5)},
                           {1, {0, 0}});
    auto square =
        bake_plan(d, axes, extras, method, ElementType::Float64, true, .1);
    auto report_only = select_bake(d, square, "report");
    auto report =
        take(d.context->execute(report_only.plan, report_only.bindings, {},
                                bake_execution()))
            .results.at("report");
    auto measured = take(read_lut3d_bake_report(report, 72));
    require(
        !measured.passed && measured.validation_count == 3 &&
            measured.failed_count == 3 && measured.first_failure_index == 0 &&
            measured.max_abs_error == std::array<double, 3>{.25, .25, .25} &&
            measured.max_error_index == std::array<int64_t, 3>{0, 0, 0} &&
            measured.first_failure_reference ==
                std::array<double, 3>{.25, .25, .25} &&
            measured.first_failure_lut == std::array<double, 3>{.5, .5, .5},
        "exact square bake errors retain earliest duplicate-point maxima and "
        "first failure");
    auto table_only = select_bake(d, square, "table");
    auto failed = d.context->execute(table_only.plan, table_only.bindings, {},
                                     bake_execution());
    require(!failed.ok() &&
                failed.status().reason == FailureReason::InvalidDomain &&
                failed.status().message.find(
                    "LutApproximationToleranceExceeded") != std::string::npos,
            "failed measured report gates table with original diagnostic");
    auto outside =
        d.source({ElementType::Float64, {1, 3}}, {double_bits(2)}, {1, {0, 0}});
    auto bad = bake_plan(d, axes, outside, method, ElementType::Float64);
    auto axis_only = select_bake(d, bad, "axis");
    require(
        d.context
            ->execute(axis_only.plan, axis_only.bindings, {}, bake_execution())
            .ok(),
        "axis-only skips invalid extra point and source");
    auto bad_report = select_bake(d, bad, "report");
    require(!d.context
                 ->execute(bad_report.plan, bad_report.bindings, {},
                           bake_execution())
                 .ok(),
            "report validates every extra point domain");
    auto identity = bake_plan(d, axes, {}, method, ElementType::Float64);
    std::vector<ResultRef> sampled_tables;
    auto options = bake_execution();
    const auto document = identity.graph->snapshot().document();
    uint64_t pack_node = 0;
    for (const auto& node : document.nodes)
      if (node.operation == "curve.pack_lut3d")
        pack_node = node.id;
    options.result_publication = [&](ValueRef source,
                                     const ResultRef& published) {
      if (source.node_id == pack_node)
        sampled_tables.push_back(published);
      return Status::success();
    };
    auto all =
        take(d.context->execute(identity.plan, identity.bindings, {}, options));
    auto valid = all.results.at("report");
    const auto associated = valid.association()[2];
    auto sampled = std::find_if(
        sampled_tables.begin(), sampled_tables.end(),
        [&](const auto& table) { return table.object_id() == associated; });
    require(sampled != sampled_tables.end(),
            "report retains its actual sampled table instance");
    auto table = *sampled;
    for (auto corruption :
         {std::make_pair(0U, uint64_t{2}), std::make_pair(3U, uint64_t{1}),
          std::make_pair(7U, uint64_t{0})}) {
      auto malformed = bake_report_copy(d, valid, corruption);
      auto gate = d.prepare("curve.gate_lut3d", {table, malformed});
      auto rejected = d.context->execute_fragments(
          take(d.context->freeze(gate.plan, gate.bindings)),
          {{"out", take(Footprint::all({2, 2, 2, 3}))}}, {}, bake_execution());
      require(!rejected.ok() &&
                  rejected.status().reason == FailureReason::InvalidDomain,
              "external report fields cannot bypass quality validation");
    }
    auto gate = d.prepare("curve.gate_lut3d", {table, valid});
    auto tiny = bake_execution();
    tiny.maximum_result_window_bytes = 24;
    auto rejected = d.context->execute_fragments(
        take(d.context->freeze(gate.plan, gate.bindings)),
        {{"out", take(Footprint::all({2, 2, 2, 3}))}}, {}, tiny);
    require(
        !rejected.ok() &&
            rejected.status().code == ErrorCode::ResourceExhausted &&
            rejected.status().reason == FailureReason::CapacityLimit,
        "bound report gate preserves capacity category for a 72-byte field");
    const Status sentinel{ErrorCode::ResourceExhausted,
                          "table validation work sentinel",
                          FailureReason::WorkLimit,
                          {FailureOrigin::Resource, FailureScope::Run}};
    auto status = validate_representation(table, d.root, 72, {},
                                          [&](uint64_t) { return sentinel; });
    require(status.code == sentinel.code && status.reason == sentinel.reason &&
                status.message == sentinel.message &&
                status.detail.origin == sentinel.detail.origin &&
                status.detail.scope == sentinel.detail.scope,
            "typed owned table validation preserves complete budget failure "
            "status");
  }
}
void lut3d_baking_authoring_and_resources() {
  Driver d(UINT64_MAX, 64 * 1048576);
  auto axis = d.source({ElementType::Float64, {3, 3}},
                       {0, double_bits(1), double_bits(1)}, {1, {0, 8}});
  auto extras =
      d.source({ElementType::Float64, {70, 3}}, {double_bits(.5)}, {1, {0, 0}});
  auto prepared = bake_plan(d, axis, extras, Lut3dInterpolation::Tetrahedral,
                            ElementType::Float64);
  auto only = select_bake(d, prepared, "report");
  auto report =
      take(d.context->execute(only.plan, only.bindings, {}, bake_execution()))
          .results.at("report");
  require(take(read_lut3d_bake_report(report, 72)).validation_count == 71 &&
              take(read_lut3d_bake_report(report, 72)).passed,
          "bake measurements cross the 64-row Need boundary without losing "
          "duplicate points");
  WorkflowDocument document;
  WorkflowInputDeclaration declaration;
  declaration.id = 11;
  declaration.name = "axis";
  declaration.result_schema =
      std::make_shared<const SchemaTemplate>(axis.schema());
  document.inputs.push_back(declaration);
  numeric::Lut3dBakeOptions options;
  options.shape = {2, 2, 2};
  options.interpolation = Lut3dInterpolation::Trilinear;
  options.atol = options.rtol = 0;
  options.input_description = options.output_description = lut3d_description();
  options.source_pointwise = true;
  auto rejected = numeric::bake_lut3d(
      document, d.registry, WorkflowInputReference{11},
      [](WorkflowDocument& staged, const numeric::Lut3dSourceInput& input) {
        auto altered =
            std::make_shared<SchemaTemplate>(*staged.inputs[0].result_schema);
        altered->id = "altered.input";
        staged.inputs[0].result_schema = std::move(altered);
        return Result<WorkflowNodeOutput>(input.colors);
      },
      options);
  require(!rejected.ok() &&
              rejected.status().code == ErrorCode::InvalidArgument &&
              document.nodes.empty() &&
              document.inputs[0].result_schema->same_schema(axis.schema()),
          "source authoring cannot replace existing Result declarations and "
          "failure leaves document unchanged");
  auto identity = bake_plan(d, axis, {}, Lut3dInterpolation::Trilinear,
                            ElementType::Float64);
  SchemaTemplate split_schema;
  split_schema.id = "test.fragmented.colors";
  ResultTensorSpec split_tensor;
  split_tensor.key = "colors";
  split_tensor.descriptor = {ElementType::Float64, {2, 2, 2, 3}};
  split_tensor.facets = {take(encode_color_array(lut3d_description()))};
  split_tensor.atomic_trailing_axes = 1;
  split_schema.tensors.push_back(split_tensor);
  auto split_builder =
      take(ResultBuilder::start(d.root, split_schema, "test.fragmented"));
  require(split_builder
              .bind_descriptor_relation(take(ResultRelation::cartesian(
                  d.root, 1, {0, 8, 0, 0, ResultSupportTarget::Descriptor, 0})))
              .ok(),
          "fragmented color descriptor");
  for (unsigned r = 0; r < 2; ++r) {
    auto bytes = take(d.root.allocator().allocate(96));
    const auto bits = double_bits(r + .25);
    for (unsigned j = 0; j < 12; ++j)
      std::memcpy(bytes.data() + 8 * j, &bits, 8);
    require(
        split_builder
            .publish_tensor(
                0, Region({{r, 1}, {0, 2}, {0, 2}, {0, 3}}),
                {0, {96, 48, 24, 8}, {r, 0, 0, 0}}, std::move(bytes).freeze(),
                take(ResultRelation::cartesian(
                    d.root, 24, {0, 1, 0, 0, ResultSupportTarget::Tensor, 0})),
                {true, true, true, true})
            .ok(),
        "fragmented color half");
  }
  auto split = take(split_builder.seal());
  std::map<std::string, ParameterValue> pack_parameters;
  for (const auto& node : identity.graph->snapshot().document().nodes)
    if (node.operation == "curve.pack_lut3d")
      pack_parameters = node.parameters;
  auto packed =
      take(d.run("curve.pack_lut3d", {split}, {}, pack_parameters, {}, "table"))
          .results.at("out");
  auto unpacked = take(d.run("curve.unpack_lut3d", {packed})).results.at("out");
  auto packed_window = take(packed.acquire_tensor(take(packed.descriptor()), 0,
                                                  Region::whole({2, 2, 2, 3})));
  auto unpacked_window = take(unpacked.acquire_tensor(
      take(unpacked.descriptor()), 0, Region::whole({2, 2, 2, 3})));
  auto source_half = take(split.acquire_tensor(
      take(split.descriptor()), 0, Region({{0, 1}, {0, 2}, {0, 2}, {0, 3}})));
  require(read_bits(packed, {1, 1, 1, 2}) == double_bits(1.25) &&
              packed_window.storage_owner_token() !=
                  source_half.storage_owner_token() &&
              packed_window.storage_owner_token() ==
                  unpacked_window.storage_owner_token(),
          "unavailable multi-owner packing materializes once and legal "
          "unpacking retains its actual view");
  auto table_only = select_bake(d, identity, "table");
  CancellationSource stop;
  bool triggered = false;
  auto execution = bake_execution();
  uint64_t pack_id = 0;
  for (const auto& node : identity.graph->snapshot().document().nodes)
    if (node.operation == "curve.pack_lut3d")
      pack_id = node.id;
  execution.result_publication = [&](ValueRef source, const ResultRef&) {
    if (source.node_id == pack_id) {
      triggered = true;
      stop.cancel();
    }
    return Status::success();
  };
  auto failed = d.context->execute(table_only.plan, table_only.bindings,
                                   stop.token(), execution);
  require(
      triggered && !failed.ok() && failed.status().code == ErrorCode::Cancelled,
      "active bake cancellation after sampled table publication rejects final "
      "gated output");
}
}  // namespace
int main() {
  try {
    lut3d_baking_workflows();
    lut3d_baking_boundaries();
    lut3d_baking_authoring_and_resources();
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
