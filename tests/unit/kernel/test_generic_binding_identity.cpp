#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "photospider/photospider.hpp"
#include "support/image_input_fixture.hpp"
#include "support/multi_output_result_fixture.hpp"
#include "support/test_support.hpp"

namespace {
using namespace ps;  // NOLINT(build/namespaces)
struct Identity {
  bool requested = false;
  Result<ResultProgramPoll> poll(const ResultProgramPhase& phase) {
    using Poll = Result<ResultProgramPoll>;
    if (!requested) {
      requested = true;
      ResultProgramNeed need;
      auto samples = Footprint::all({1});
      if (!samples.ok())
        return Poll(samples.status());
      need.tiles.push_back({0, true, false, {}});
      return Poll(std::move(need));
    }
    float value = 0;
    auto status = phase.read_tensor(0, 0, {0}, &value, 4);
    if (!status.ok())
      return Poll(status);
    auto builder = s1_fixture::take(
        ResultBuilder::start(phase.resources, *phase.query.output.result_schema,
                             phase.query.semantic_key));

    s1_fixture::check(builder.publish_tensor(
        0, Region::whole({1}),
        ByteView(reinterpret_cast<const uint8_t*>(&value), 4)));
    return Poll(ResultPublication{s1_fixture::take(builder.seal()), true});
  }
};
int static_constraint_identity() {
  std::vector<std::string> semantic, physical;
  for (int mode = 0; mode < 6; ++mode) {
    auto registry = std::make_shared<OperationRegistry>();
    const auto schema = s1_fixture::schema({1}, false);
    OperationDefinition operation;
    operation.key = "source";
    auto& traits = operation.traits;
    traits.input_count = 1;
    OperationPortConstraint port;
    port.kind = OperationPortKind::Result;
    port.tensor_key = "value";
    if (mode >= 4) {
      port.scalar_bounds = true;
      port.maximum = mode == 4 ? 1 : 2;
    }
    traits.input_schema = {port};
    auto& output = traits.outputs[0];
    output.output_schema.kind = OperationPortKind::Result;
    output.output_schema.result_schema_id = schema.id;
    output.output_schema.result_schema_version = 1;
    output.result_schema = schema;
    output.region_rule = OperationRegionRule::Tiled;
    output.continuation_bytes = sizeof(Identity);
    output.maximum_dependency_stages = 2;
    operation.start_result = [](const ResultProgramQuery&,
                                const BufferAllocator& allocator) {
      return ResultContinuation::make<Identity>(allocator);
    };
    PS_CHECK(registry->register_operation(std::move(operation)).ok());
    PS_CHECK(registry->freeze().ok());
    Compiler compiler(registry);
    WorkflowDocument doc;
    doc.inputs = {s1_fixture::declaration(1, "input", schema),
                  s1_fixture::declaration(2, "unused", schema)};
    auto unused = std::make_shared<SchemaTemplate>(schema);
    doc.inputs[1].result_schema = unused;
    if (mode == 1)
      unused->tensors[0].atomic_trailing_axes = 1;
    if (mode == 2)
      unused->tensors[0].key = "different";
    doc.nodes = {
        {1, "source", {WorkflowInputReference{mode == 3 ? 2U : 1U}}, {}}};
    doc.outputs = {{"result", 1, "value"}};
    GraphContext graph(doc);
    auto compiled = compiler.compile(graph);
    PS_CHECK(compiled.ok());
    semantic.push_back(compiled.value().semantic.digest().value);
    physical.push_back(compiled.value().plan.digest().value);
  }
  for (std::size_t i = 0; i < semantic.size(); ++i)
    for (std::size_t j = i + 1; j < semantic.size(); ++j) {
      PS_CHECK(semantic[i] != semantic[j]);
      PS_CHECK(physical[i] != physical[j]);
    }
  return 0;
}

int extension_resource_identity() {
  std::vector<std::string> semantic, physical, cache;
  ResourceBudget root;
  for (unsigned mode = 0; mode < 4; ++mode) {
    auto registry = std::make_shared<OperationRegistry>();
    std::uint8_t byte = mode == 1 ? 2 : 1;
    auto resource =
        s1_fixture::take(ImmutableResource::import({&byte, 1}, root));
    auto resources = s1_fixture::take(ResourceSet::create({resource}, root));
    ValidationDefinition validator;
    validator.identity = {"fixture.identity", mode == 2 ? 2U : 1U,
                          mode == 3 ? "impl/2" : "impl/1"};
    validator.prepare = [](const auto&) {
      return Result<std::shared_ptr<const void>>(std::shared_ptr<const void>{});
    };
    PS_REQUIRE_OK(registry->register_validator(validator));
    OperationDefinition op;
    op.key = "fixture.identity";
    auto schema = s1_fixture::schema({1}, false);
    op.traits.outputs = {multi_result::output("value", schema)};
    op.traits.requires_metadata_specialization = true;
    op.traits.outputs[0].output_schema.result_schema_id = schema.id;
    op.traits.outputs[0].output_schema.result_schema_version = schema.version;
    op.traits.outputs[0].output_schema.validators = {validator.identity};
    op.prepare_static = [id = resource.identity(), schema](const auto&,
                                                           const auto&) {
      OperationPreparation p;
      p.resources = {id};
      p.outputs.resize(1);
      p.outputs[0].metadata.result_schema =
          std::make_shared<const SchemaTemplate>(schema);
      return Result<OperationPreparation>(std::move(p));
    };
    op.start_result = [](const auto&, const auto& allocator) {
      return ResultContinuation::make<Identity>(allocator);
    };
    PS_REQUIRE_OK(registry->register_operation(op));
    PS_REQUIRE_OK(registry->freeze());
    WorkflowDocument document;
    document.nodes = {{1, op.key, {}, {}}};
    document.outputs = {{"value", 1, "value"}};
    GraphContext graph(document);
    auto plan = Compiler(registry).compile(graph, {}, resources);
    PS_REQUIRE_OK(plan);
    semantic.push_back(plan.value().semantic.digest().value);
    physical.push_back(plan.value().plan.digest().value);
    cache.push_back(plan.value().plan.cache_key().value);
  }
  for (unsigned i = 0; i < 4; ++i)
    for (unsigned j = i + 1; j < 4; ++j) {
      PS_CHECK(semantic[i] != semantic[j]);
      PS_CHECK(physical[i] != physical[j]);
      PS_CHECK(cache[i] != cache[j]);
    }
  return 0;
}

}  // namespace
int main() {
  return static_constraint_identity() || extension_resource_identity();
}
