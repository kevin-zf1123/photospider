#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "image_vertical/image_fixture.hpp"
#include "photospider/photospider.hpp"
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
      need.tensors.push_back({0, 0, samples.take_value(), 13});
      return Poll(std::move(need));
    }
    float value = 0;
    auto status = phase.read_tensor(0, 0, {0}, &value, 4);
    if (!status.ok())
      return Poll(status);
    auto builder = s1_fixture::take(
        ResultBuilder::start(phase.resources, *phase.query.output.result_schema,
                             phase.query.semantic_key));
    s1_fixture::check(builder.bind_descriptor_relation(
        s1_fixture::take(ResultRelation::cartesian(
            phase.resources, 1,
            {0, 8, 0, 1, ResultSupportTarget::Descriptor}))));
    s1_fixture::check(builder.publish_tensor(
        0, Region::whole({1}),
        ByteView(reinterpret_cast<const uint8_t*>(&value), 4),
        s1_fixture::take(ResultRelation::cartesian(
            phase.resources, 1, {0, 5, 0, 1, ResultSupportTarget::Tensor, 0})),
        {true, true, true, true}));
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
    output.dependency_version = 2;
    output.region_rule = OperationRegionRule::Dependency;
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
      unused->tensors[0].facets = {{"semantic", 1, {1}}};
    if (mode == 2)
      unused->tensors[0].facets = {{"semantic", 1, {2}}};
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

}  // namespace
int main() {
  return static_constraint_identity();
}
