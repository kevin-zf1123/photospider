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
  constraint.kind = OperationPortKind::Result;
  constraint.tensor_key = "samples";
  OperationTraits traits;
  traits.input_count = 0;
  traits.input_schema = {constraint};
  traits.repeated_minimum = 1;
  traits.repeated_maximum = 4;
  traits.outputs[0].output_schema = constraint;
  traits.outputs[0].output_schema.result_schema_id = "test.repeated";
  traits.outputs[0].output_schema.result_schema_version = 1;
  traits.outputs[0].continuation_bytes = 1;
  traits.outputs[0].maximum_dependency_stages = 1;
  traits.outputs[0].result_schema = schema;
  traits.outputs[0].region_rule = OperationRegionRule::Dependency;
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
void scalar_prelude_workflows() {
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
  auto starts = std::make_shared<int>(0);
  auto producer_starts = std::make_shared<int>(0);
  for (int profile = 0; profile < 5; ++profile) {
    OperationDefinition operation;
    operation.key = profile == 0   ? "test.scalar.consumer"
                    : profile == 1 ? "test.scalar.nan"
                    : profile == 2 ? "test.scalar.range"
                    : profile == 3 ? "test.scalar.valid"
                                   : "test.scalar.loose";
    auto& traits = operation.traits;
    const bool consumer = profile == 0 || profile == 4;
    traits.input_count = consumer ? 2 : 0;
    traits.input_schema.clear();
    if (consumer) {
      OperationPortConstraint constraint;
      constraint.kind = OperationPortKind::Result;
      constraint.tensor_key = "gain";
      constraint.element_type = static_cast<uint32_t>(ElementType::Float32);
      constraint.scalar_bounds = true;
      constraint.maximum = profile == 4 ? 2 : 1;
      traits.input_schema = {constraint, constraint};
    }
    auto& output = traits.outputs[0];
    output.output_schema.kind = OperationPortKind::Result;
    output.output_schema.result_schema_id = "test.scalar.members";
    output.output_schema.result_schema_version = 1;
    output.result_schema = consumer ? output_schema : source_schema;
    output.region_rule = OperationRegionRule::Dependency;
    output.input_indices =
        consumer ? std::vector<uint32_t>{0} : std::vector<uint32_t>{};
    output.continuation_bytes = sizeof(ScalarPreludeState);
    output.maximum_dependency_stages = 1;
    operation.start_result = [consumer, starts, producer_starts, profile](
                                 const ResultProgramQuery&,
                                 const BufferAllocator& allocator) {
      if (consumer)
        ++*starts;
      else
        ++*producer_starts;
      return ResultContinuation::make<ScalarPreludeState>(
          allocator, consumer,
          profile == 1   ? std::numeric_limits<float>::quiet_NaN()
          : profile == 2 ? 1.5F
                         : .5F);
    };
    require(registry->register_operation(std::move(operation)).ok(),
            "scalar fixture registration");
  }
  require(registry->freeze().ok(), "scalar fixture freeze");
  ExecutionContextConfig config;
  config.managed_resources = ResourceLimits{};
  ExecutionContext context(registry, config);
  auto root = take(context.resource_budget());
  const auto binding = [&](float value) {
    auto builder =
        take(ResultBuilder::start(root, source_schema, "scalar.source"));
    require(builder
                .bind_descriptor_relation(
                    take(ResultRelation::cartesian(root, 1, {0, 8, 0, 0})))
                .ok(),
            "scalar source descriptor");
    auto buffer = take(root.allocator().allocate(sizeof(value)));
    std::memcpy(buffer.data(), &value, sizeof(value));
    auto storage = std::move(buffer).freeze();
    for (uint32_t slot = 0; slot < 3; ++slot)
      require(builder
                  .publish_tensor(
                      slot, Region::whole({1}), {0, {INT64_MIN}}, storage,
                      take(ResultRelation::cartesian(root, 1, {0, 1, 0, 0})),
                      {true, true, true, true})
                  .ok(),
              "scalar signed singleton source");
    ExecutionBinding input;
    input.name = "control";
    input.result = take(builder.seal());
    return input;
  };
  auto external = binding(.5F);
  auto ignored = binding(std::numeric_limits<float>::quiet_NaN());
  ignored.name = "ignored";
  WorkflowDocument direct;
  for (uint64_t id : {1, 2}) {
    WorkflowInputDeclaration declaration;
    declaration.id = id;
    declaration.name = id == 1 ? "control" : "ignored";
    declaration.result_schema = std::make_shared<SchemaTemplate>(source_schema);
    direct.inputs.push_back(declaration);
  }
  direct.nodes = {{91,
                   "test.scalar.consumer",
                   {WorkflowInputReference{1}, WorkflowInputReference{2}},
                   {}}};
  direct.outputs = {{"out", 91, "value"}};
  GraphContext graph(direct);
  auto plan = take(Compiler(registry).compile(graph)).plan;
  auto success = context.execute(plan, {{external, ignored}});
  require(success.ok() && *starts == 1,
          "selected named scalar is ready before consumer start and excluded "
          "NaN is not read");
  auto observations = take(success.value().dependencies.source_observations());
  bool validation = false, descriptor = false;
  for (const auto& observation : observations) {
    if (observation.input == "control") {
      validation |= observation.target == ResultSupportTarget::Tensor &&
                    observation.slot == 2 && (observation.roles & 4);
      descriptor |= observation.target == ResultSupportTarget::Descriptor &&
                    (observation.roles & 8);
    }
    require(observation.input != "ignored",
            "excluded input does not acquire implicit scalar witness");
  }
  require(
      validation && descriptor,
      "implicit scalar validation and descriptor retain full Result ancestry");
  {
    auto cached_config = config;
    cached_config.result_cache_bytes = 4096;
    ExecutionContext cached(registry, cached_config);
    auto cached_root = take(cached.resource_budget());
    ExecutionBindings inputs;
    for (const char* name : {"control", "ignored"}) {
      auto builder = take(ResultBuilder::start(cached_root, source_schema,
                                               "scalar.cache.source"));
      require(builder
                  .bind_descriptor_relation(
                      take(ResultRelation::cartesian(cached_root, 1, {})))
                  .ok(),
              "cached scalar source descriptor");
      const float number = .5F;
      for (uint32_t slot = 0; slot < 3; ++slot)
        require(builder
                    .publish_tensor(
                        slot, Region::whole({1}),
                        ByteView(reinterpret_cast<const uint8_t*>(&number), 4),
                        take(ResultRelation::cartesian(cached_root, 1, {})),
                        {true, true, true, true})
                    .ok(),
                "cached scalar source");
      inputs.inputs.push_back({name, take(builder.seal())});
    }
    {
      auto seed = cached.execute(plan, inputs);
      require(seed.ok() && cached.cache_statistics().entries == 1,
              "scalar completed Result retention");
    }
    ExecutionOptions limited;
    limited.maximum_dependency_cache_work = 0;
    uint64_t lower = 0, upper = 4096;
    while (lower + 1 < upper) {
      const auto middle = lower + (upper - lower) / 2;
      limited.maximum_dependency_work = middle;
      auto attempted = cached.execute(plan, inputs, {}, limited);
      if (attempted.ok()) {
        upper = middle;
      } else {
        require(attempted.status().code == ErrorCode::ResourceExhausted,
                "scalar Run admission boundary");
        lower = middle;
      }
    }
    // Ordinary Run identities have variable-length metadata. Keep bounded
    // headroom instead of assuming every later Run has the same exact cost.
    limited.maximum_dependency_work = upper + 128;
    for (uint64_t fuel : {8U, 64U, 128U, 256U, 512U, 1024U, 2048U}) {
      limited.maximum_dependency_cache_work = fuel;
      auto completed = cached.execute(plan, inputs, {}, limited);
      if (!completed.ok())
        std::cerr << "cache fuel=" << fuel
                  << " Run quota=" << limited.maximum_dependency_work
                  << " status="
                  << static_cast<unsigned>(completed.status().code)
                  << " reason="
                  << static_cast<unsigned>(completed.status().reason)
                  << " message=" << completed.status().message << '\n';
      require(completed.ok(),
              "optional cache miss restores scalar prelude with bounded Run "
              "quota");
    }
  }
  auto warm = context.execute(plan, {{external, ignored}});
  require(warm.ok(), "cached Result scalar consumer remains valid");
  const auto before = *starts;
  for (float number : {std::numeric_limits<float>::quiet_NaN(), 1.5F}) {
    auto invalid = context.execute(plan, {{binding(number), ignored}});
    require(!invalid.ok() &&
                invalid.status().code == ErrorCode::InvalidArgument &&
                invalid.status().detail.input_id == 1 &&
                invalid.status().detail.origin == FailureOrigin::Domain &&
                *starts == before,
            "bad direct named scalar reports input origin and never starts "
            "consumer");
  }
  for (const char* key : {"test.scalar.nan", "test.scalar.range"}) {
    auto computed = direct;
    computed.inputs.erase(computed.inputs.begin());
    computed.nodes = {
        {70, key, {}, {}},
        {91,
         "test.scalar.consumer",
         {WorkflowNodeOutput{70, "value"}, WorkflowInputReference{2}},
         {}}};
    GraphContext computed_graph(computed);
    auto computed_plan = take(Compiler(registry).compile(computed_graph)).plan;
    auto invalid = context.execute(computed_plan, {{ignored}});
    require(!invalid.ok() &&
                invalid.status().code == ErrorCode::OperationFailed &&
                invalid.status().detail.node_id == 70 &&
                invalid.status().detail.origin == FailureOrigin::Domain &&
                *starts == before,
            "bad computed named scalar reports upstream origin and never "
            "starts consumer");
  }
  auto shared = direct;
  shared.inputs.erase(shared.inputs.begin());
  shared.nodes = {{70, "test.scalar.range", {}, {}},
                  {80,
                   "test.scalar.loose",
                   {WorkflowNodeOutput{70, "value"}, WorkflowInputReference{2}},
                   {}},
                  {91,
                   "test.scalar.consumer",
                   {WorkflowNodeOutput{70, "value"}, WorkflowInputReference{2}},
                   {}}};
  shared.outputs.push_back({"loose", 80, "value"});
  auto shared_graph = std::make_shared<GraphContext>(shared);
  auto shared_plan = take(Compiler(registry).compile(*shared_graph)).plan;
  auto frozen = take(context.freeze(shared_plan, {{ignored}}));
  auto warm_source =
      context.execute_fragments(frozen, {{"loose", take(Footprint::all({1}))}});
  require(warm_source.ok(),
          "unbounded producer can publish a value valid for a looser consumer");
  const auto produced = *producer_starts, consumed = *starts;
  auto restricted =
      context.execute_fragments(frozen, {{"out", take(Footprint::all({1}))}});
  require(!restricted.ok() &&
              restricted.status().code == ErrorCode::OperationFailed &&
              restricted.status().detail.node_id == 70 &&
              *producer_starts == produced && *starts == consumed,
          "cached completed producer is revalidated per consumer before start");
  auto wrong_metadata = direct;
  auto wrong_schema = std::make_shared<SchemaTemplate>(source_schema);
  wrong_schema->tensors[2].descriptor.shape = {2};
  wrong_metadata.inputs[1].result_schema = wrong_schema;
  GraphContext wrong_graph(wrong_metadata);
  auto refused = Compiler(registry).compile(wrong_graph);
  require(!refused.ok() && refused.status().code == ErrorCode::TypeMismatch,
          "excluded runtime input still receives full static scalar metadata "
          "validation");
}
void foundation_tensor_workflows() {
  Driver driver;
  WorkflowDocument document;
  document.nodes = {{1, "core.constant", {}, {{"value", 2.5}}},
                    {2, "core.identity", {WorkflowNodeOutput{1, "value"}}, {}}};
  document.outputs = {{"number", 1, "value"}, {"alias", 2, "value"}};
  auto graph = std::make_shared<GraphContext>(document);
  auto compiled = take(Compiler(driver.registry).compile(*graph));
  auto output = take(driver.context->execute(compiled.plan));
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
  require(value == 2.5 && first.data == last.data &&
              source_window.storage_owner_token() ==
                  view_window.storage_owner_token(),
          "public constant to identity workflow publishes Result tensor views");
  for (bool sparse : {false, true}) {
    SchemaTemplate schema;
    schema.id = "photospider.tensor";
    ResultTensorSpec spec;
    spec.key = "samples";
    spec.descriptor = {ElementType::UInt8, {UINT64_MAX}};
    schema.tensors.push_back(spec);
    auto buffer = take(driver.root.allocator().allocate(1));
    buffer.data()[0] = 0x72;
    auto storage = std::move(buffer).freeze();
    const Region region =
        sparse ? Region({{UINT64_MAX - 2, 2}}) : Region::whole({UINT64_MAX});
    auto builder =
        take(ResultBuilder::start(driver.root, schema, "identity.source"));
    require(builder
                .bind_descriptor_relation(take(
                    ResultRelation::cartesian(driver.root, 1, {0, 8, 0, 0})))
                .ok(),
            "huge identity basis");
    require(builder
                .publish_tensor(0, region, {0, {0}}, storage,
                                take(ResultRelation::cartesian(
                                    driver.root, UINT64_MAX, {0, 1, 0, 0})),
                                {true, true, true, true})
                .ok(),
            "huge identity affine input");
    ExecutionBinding binding;
    binding.name = "samples";
    binding.result = take(builder.seal());
    auto run = driver.prepare("core.identity", {binding});
    ResultRef identity;
    if (sparse) {
      auto result = driver.context->execute_fragments(
          take(driver.context->freeze(run.plan, run.bindings)),
          {{"out", take(Footprint::from_regions({UINT64_MAX}, {region}))}});
      require(result.ok(), result.status().message.c_str());
      identity = result.value().results.at("out");
    } else {
      auto result = driver.context->execute(run.plan, run.bindings);
      require(result.ok(), result.status().message.c_str());
      identity = result.value().results.at("out");
    }
    auto window =
        take(identity.acquire_tensor(take(identity.descriptor()), 0, region));
    require(
        *take(window.row_run({UINT64_MAX - 2})).data == 0x72 &&
            window.storage_owner_token() == storage.get() &&
            take(identity.descriptor()).tensor_coverage(0) ==
                take(Footprint::from_regions({UINT64_MAX}, {region})),
        "Result identity preserves huge broadcast owner and exact sparse ROI");
  }
  SchemaTemplate huge;
  huge.id = "photospider.tensor";
  ResultTensorSpec member;
  member.key = "samples";
  member.descriptor = {ElementType::UInt8, {UINT64_MAX, 2}};
  huge.tensors.push_back(member);
  const auto shape = member.sample_shape();
  const Region last_point({{UINT64_MAX - 1, 1}, {1, 1}});
  auto builder =
      take(ResultBuilder::start(driver.root, huge, "identity.huge.rank2"));
  require(builder
              .bind_descriptor_relation(
                  take(ResultRelation::cartesian(driver.root, 1, {0, 8, 0, 0})))
              .ok(),
          "huge rank2 basis");
  auto storage_buffer = take(driver.root.allocator().allocate(1));
  storage_buffer.data()[0] = 0x5a;
  auto storage = std::move(storage_buffer).freeze();
  auto no_input = take(ResultRelation::mapped(
      driver.root, shape, last_point, {1}, {{-1, 0, 0, 1}}, {0, 1, 0, 0}));
  require(builder
              .publish_tensor(0, last_point, {0, {0, 0}}, storage, no_input,
                              {true, true, true, true})
              .ok(),
          "huge rank2 sparse affine input");
  ExecutionBinding binding;
  binding.name = "samples";
  binding.result = take(builder.seal());
  auto prepared = driver.prepare("core.identity", {binding});
  auto requested = take(Footprint::from_regions(shape, {last_point}));
  auto huge_output = driver.context->execute_fragments(
      take(driver.context->freeze(prepared.plan, prepared.bindings)),
      {{"out", requested}});
  require(huge_output.ok(), huge_output.status().message.c_str());
  auto result = huge_output.value().results.at("out");
  uint8_t byte = 0;
  require(result.read_tensor(take(result.descriptor()), 0, {UINT64_MAX - 1, 1},
                             &byte, 1)
                  .ok() &&
              byte == 0x5a,
          "huge rank2 Result identity reads a representable source span");
  auto support = take(huge_output.value().dependencies.source_observations());
  bool exact = false;
  for (const auto& observation : support)
    exact |= observation.input == "samples" &&
             observation.target == ResultSupportTarget::Tensor &&
             observation.samples == requested && (observation.roles & 1U);
  require(exact,
          "huge rank2 compact backward support retains exact coordinates");
  auto dirty = take(
      huge_output.value().dependencies.potential_dirty("samples", requested));
  require(dirty.at("out") == requested,
          "huge rank2 compact dirty preimage remains exact");
}
}  // namespace
int main() {
  try {
    repeated_tensor_shapes();
    scalar_prelude_workflows();
    foundation_tensor_workflows();
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
