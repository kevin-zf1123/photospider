#include <cmath>
#include <cstring>
#include <iostream>
#include <limits>
#include <memory>
#include <utility>
#include <vector>

#include "support/result_image_execution_fixture.hpp"

namespace {
using namespace ps::test_image;  // NOLINT(build/namespaces)
void repeated_tensor_shapes() {
  SchemaTemplate schema;
  schema.id = "test.repeated";
  ResultTensorSpec member;
  member.key = "samples";
  member.descriptor = {ElementType::UInt8, {3}};
  member.batch_axes = {2};
  schema.tensors.push_back(member);
  OperationPortConstraint constraint;

  constraint.tensor_key = "samples";
  OperationTraits traits;
  traits.input_count = 0;
  traits.input_schema = {constraint};
  traits.repeated_minimum = 1;
  traits.repeated_maximum = 4;
  traits.outputs[0].output_schema = constraint;
  traits.outputs[0].output_schema.result_schema_id = "test.repeated";
  traits.outputs[0].output_schema.result_schema_version = 1;
  traits.outputs[0].result_schema = schema;
  auto resolved = take(resolve_operation_traits(traits, 2, {}));
  OperationMetadata first, second;
  first.result_schema = std::make_shared<SchemaTemplate>(schema);
  second.result_schema = first.result_schema;
  require(infer_operation_outputs(resolved, {first, second}, {}).ok(),
          "repeated matching Result sample domains infer successfully");
  schema.tensors[0].batch_axes = {4};
  second.result_schema = std::make_shared<SchemaTemplate>(schema);
  auto different = infer_operation_outputs(resolved, {first, second}, {});
  require(
      !different.ok() && different.status().code == ErrorCode::TypeMismatch,
      "repeated Result inputs include batch axes in homogeneous logical shape");
}
void named_scalar_workflows() {
  SchemaTemplate source_schema;
  source_schema.id = "test.scalar.members";
  for (const char* key : {"unused", "other", "gain"}) {
    ResultTensorSpec tensor;
    tensor.key = key;
    tensor.descriptor = {ElementType::Float32, {1}};
    source_schema.tensors.push_back(tensor);
  }
  auto output_schema = source_schema;
  output_schema.tensors.resize(1);
  auto registry = std::make_shared<OperationRegistry>();
  auto consumers = std::make_shared<std::atomic<unsigned>>(0);
  auto producers = std::make_shared<std::atomic<unsigned>>(0);
  for (unsigned profile = 0; profile < 4; ++profile) {
    const bool consumer = profile == 0;
    OperationDefinition operation;
    operation.key = consumer       ? "test.scalar.consumer"
                    : profile == 1 ? "test.scalar.nan"
                    : profile == 2 ? "test.scalar.range"
                                   : "test.scalar.valid";
    operation.traits.input_count = consumer ? 2 : 0;
    operation.traits.requires_metadata_specialization = consumer;
    if (consumer) {
      OperationPortConstraint port;
      port.tensor_key = "gain";
      port.element_type = static_cast<std::uint32_t>(ElementType::Float32);
      port.rank = 1;
      operation.traits.input_schema = {port, port};
      operation.specialize_metadata = [output_schema](const auto& inputs,
                                                      const auto&)
          -> Result<std::vector<OperationOutputSpecialization>> {
        for (const auto& input : inputs) {
          const auto& tensors = input.result_schema->tensors;
          if (tensors.size() != 3 || tensors[2].key != "gain" ||
              tensors[2].sample_shape() != std::vector<std::uint64_t>{1})
            return Result<std::vector<OperationOutputSpecialization>>(
                {ErrorCode::TypeMismatch, "named scalar shape"});
        }
        OperationOutputSpecialization output;
        output.metadata.result_schema =
            std::make_shared<const SchemaTemplate>(output_schema);
        return Result<std::vector<OperationOutputSpecialization>>(
            std::vector<OperationOutputSpecialization>{std::move(output)});
      };
    }
    auto& output = operation.traits.outputs[0];
    output.output_schema.result_schema_id = source_schema.id;
    output.output_schema.result_schema_version = 1;
    output.result_schema = consumer ? output_schema : source_schema;
    output.input_indices =
        consumer ? std::vector<std::uint32_t>{0} : std::vector<std::uint32_t>{};
    install_scalar_member(operation, consumer,
                          profile == 1 ? std::numeric_limits<float>::quiet_NaN()
                          : profile == 2 ? 1.5F
                                         : .5F,
                          consumer ? consumers : producers);
    auto registered = registry->register_operation(std::move(operation));
    require(registered.ok(), registered.message.c_str());
  }
  require(registry->freeze().ok(), "scalar registry freeze");
  ExecutionContext context(registry);
  auto root = take(context.resource_budget());
  const auto binding = [&](float value) {
    auto builder =
        take(ResultBuilder::start(root, source_schema, "scalar.source"));
    auto buffer = take(root.allocator().allocate(sizeof(value)));
    std::memcpy(buffer.data(), &value, sizeof(value));
    auto storage = std::move(buffer).freeze();
    for (std::uint32_t member = 0; member < 3; ++member)
      require(builder
                  .publish_tensor(member, Region::whole({1}), {0, {INT64_MIN}},
                                  storage)
                  .ok(),
              "signed singleton source");
    return ExecutionBinding{"control", take(builder.seal())};
  };
  auto ignored = binding(std::numeric_limits<float>::quiet_NaN());
  ignored.name = "ignored";
  WorkflowDocument direct;
  direct.inputs = {
      {1, "control", std::make_shared<SchemaTemplate>(source_schema)},
      {2, "ignored", std::make_shared<SchemaTemplate>(source_schema)}};
  direct.nodes = {{91,
                   "test.scalar.consumer",
                   {WorkflowInputReference{1}, WorkflowInputReference{2}},
                   {}}};
  direct.outputs = {{"out", 91, "value"}};
  GraphContext graph(direct);
  auto plan = take(Compiler(registry).compile(graph)).plan.value();
  const auto& physical = plan.program();
  require(physical.tasks.size() == 1 && physical.tasks[0].reads.size() == 1 &&
              physical.tasks[0].reads[0].port == 0 &&
              physical.tiles[physical.tasks[0].reads[0].tile].member == 2 &&
              physical.values[physical.inputs[1]].tiles.empty(),
          "named scalar fixes one member read and excludes the other port");
  const auto check_bits = [](const ExecutionResult& result, float expected) {
    const auto& output = result.results.at("out");
    float actual = 0;
    require(
        output.read_tensor(take(output.descriptor()), 0, {0}, &actual, 4).ok(),
        "scalar result read");
    require(std::memcmp(&actual, &expected, 4) == 0,
            "named scalar preserves sample bits");
  };
  for (float value : {.5F, 1.5F, std::numeric_limits<float>::quiet_NaN()}) {
    const auto before = consumers->load();
    auto result =
        take(multi_result::run(context, plan, {{binding(value), ignored}}));
    check_bits(result, value);
    require(consumers->load() == before + 1,
            "each binding executes its consumer");
  }
  const char* names[] = {"test.scalar.nan", "test.scalar.range",
                         "test.scalar.valid"};
  const float values[] = {std::numeric_limits<float>::quiet_NaN(), 1.5F, .5F};
  for (unsigned profile = 0; profile < 3; ++profile) {
    auto computed = direct;
    computed.inputs.erase(computed.inputs.begin());
    computed.nodes = {
        {70, names[profile], {}, {}},
        {91,
         "test.scalar.consumer",
         {WorkflowNodeOutput{70, "value"}, WorkflowInputReference{2}},
         {}}};
    GraphContext computed_graph(computed);
    auto compiled = take(Compiler(registry).compile(computed_graph));
    const auto before = producers->load();
    auto result = take(multi_result::run(context, *compiled.plan, {{ignored}}));
    check_bits(result, values[profile]);
    require(producers->load() == before + 1,
            "only the selected producer member executes");
  }
  auto wrong_metadata = direct;
  auto wrong_schema = std::make_shared<SchemaTemplate>(source_schema);
  wrong_schema->tensors[2].descriptor.shape = {2};
  wrong_metadata.inputs[1].result_schema = wrong_schema;
  GraphContext wrong_graph(wrong_metadata);
  auto refused = Compiler(registry).compile(wrong_graph);
  require(!refused.ok() && refused.status().code == ErrorCode::TypeMismatch,
          "excluded payload still has its static scalar metadata checked");
}
void foundation_tensor_workflows() {
  Driver driver;
  WorkflowDocument document;
  document.nodes = {{1, "core.constant", {}, {{"value", 2.5}}},
                    {2, "core.identity", {WorkflowNodeOutput{1, "value"}}, {}}};
  document.outputs = {{"number", 1, "value"}, {"alias", 2, "value"}};
  auto graph = std::make_shared<GraphContext>(document);
  auto compiled = take(Compiler(driver.registry).compile(*graph));
  auto output = take(multi_result::run(*driver.context, compiled.plan.value()));
  auto number = output.results.at("number");
  auto alias = output.results.at("alias");
  auto source_window = take(
      number.acquire_tensor(take(number.descriptor()), 0, Region::whole({1})));
  auto view_window = take(
      alias.acquire_tensor(take(alias.descriptor()), 0, Region::whole({1})));
  const auto first = take(source_window.row_run({0}));
  const auto last = take(view_window.row_run({0}));
  double value = 0;
  std::memcpy(&value, last.data, sizeof(value));
  require(value == 2.5 && first.data != last.data &&
              source_window.storage_owner_token() !=
                  view_window.storage_owner_token(),
          "public identity COPY owns independent output storage");
  {
    SchemaTemplate schema;
    schema.id = "photospider.tensor";
    ResultTensorSpec spec;
    spec.key = "samples";
    spec.descriptor = {ElementType::UInt8, {UINT64_MAX}};
    schema.tensors.push_back(spec);
    auto buffer = take(driver.root.allocator().allocate(1));
    buffer.data()[0] = 0x72;
    auto storage = std::move(buffer).freeze();
    const Region region = Region::whole({UINT64_MAX});
    auto builder =
        take(ResultBuilder::start(driver.root, schema, "identity.source"));
    require(builder.publish_tensor(0, region, {0, {0}}, storage).ok(),
            "huge identity affine input");
    ExecutionBinding binding;
    binding.name = "samples";
    binding.result = take(builder.seal());
    PlanningOptions planning;
    planning.planner = "broadcast-inputs";
    const auto planned_grid =
        take(TileGrid::from_tensor(spec, planning.tile_extent));
    const auto last_tile = planned_grid.tile_count() - 1;
    planning.output_requests["out"] = {
        {{0, take(TileSet::range(driver.root, last_tile, last_tile + 1))}}};
    auto run =
        driver.prepare("core.identity", {binding}, {}, "value", planning);
    ResultRef identity;
    const auto& grid = run.plan.output("out").tile_grids[0];
    const auto tile = grid.tile_count() - 1;
    const auto selected_region = take(grid.region(tile));
    TileRequest selected_request;
    selected_request.members.emplace(
        0, take(TileSet::range(driver.root, tile, tile + 1)));
    auto selected_plan =
        take(run.plan.select_outputs({{"out", selected_request}}));
    auto result =
        multi_result::run(*driver.context, selected_plan, run.bindings);
    require(result.ok(), result.status().message.c_str());
    identity = result.value().results.at("out");
    auto window = take(identity.acquire_tensor(take(identity.descriptor()), 0,
                                               selected_region));
    require(
        *take(window.row_run({UINT64_MAX - 2})).data == 0x72 &&
            window.storage_owner_token() != storage.get() &&
            take(identity.descriptor()).tensor_coverage(0) ==
                take(Footprint::from_regions({UINT64_MAX}, {selected_region})),
        "COPY materializes the exact sparse ROI from a huge broadcast "
        "source");
  }
  SchemaTemplate huge;
  huge.id = "photospider.tensor";
  ResultTensorSpec member;
  member.key = "samples";
  member.descriptor = {ElementType::UInt8, {UINT64_MAX, 2}};
  huge.tensors.push_back(member);
  const auto shape = member.sample_shape();
  auto builder =
      take(ResultBuilder::start(driver.root, huge, "identity.huge.rank2"));
  auto storage_buffer = take(driver.root.allocator().allocate(1));
  storage_buffer.data()[0] = 0x5a;
  auto storage = std::move(storage_buffer).freeze();
  require(builder.publish_tensor(0, Region::whole(shape), {0, {0, 0}}, storage)
              .ok(),
          "huge rank2 sparse affine input");
  ExecutionBinding binding;
  binding.name = "samples";
  binding.result = take(builder.seal());
  PlanningOptions planning;
  planning.planner = "broadcast-inputs";
  const auto planned_grid =
      take(TileGrid::from_tensor(member, planning.tile_extent));
  const auto last_tile = planned_grid.tile_count() - 1;
  planning.output_requests["out"] = {
      {{0, take(TileSet::range(driver.root, last_tile, last_tile + 1))}}};
  auto prepared =
      driver.prepare("core.identity", {binding}, {}, "value", planning);
  require(prepared.plan.output("out").tile_grids[0].tile_count() ==
              planned_grid.tile_count(),
          "planning a sparse request preserves the complete logical grid");
  TileRequest selected_request;
  selected_request.members.emplace(
      0, take(TileSet::range(driver.root, last_tile, last_tile + 1)));
  auto selected =
      take(prepared.plan.select_outputs({{"out", selected_request}}));
  auto loaded = take(CompilerArtifact::import_plan(
      driver.registry, take(CompilerArtifact::export_plan(selected))));
  auto huge_output =
      multi_result::run(*driver.context, loaded, prepared.bindings);
  require(huge_output.ok(), huge_output.status().message.c_str());
  auto result = huge_output.value().results.at("out");
  uint8_t byte = 0;
  require(result.read_tensor(take(result.descriptor()), 0, {UINT64_MAX - 1, 1},
                             &byte, 1)
                  .ok() &&
              byte == 0x5a,
          "huge rank2 Result identity reads a representable source span");
  const auto& program = loaded.program();
  require(
      program.tasks.size() == 1 && program.tasks[0].reads.size() == 1 &&
          program.tiles[program.tasks[0].reads[0].tile].tile_index == last_tile,
      "huge tensor has one statically declared source tile");
}
}  // namespace
int main() {
  try {
    repeated_tensor_shapes();
    named_scalar_workflows();
    foundation_tensor_workflows();
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
