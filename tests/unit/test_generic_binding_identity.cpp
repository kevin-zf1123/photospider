#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "image_vertical/image_fixture.hpp"
#include "photospider/photospider.hpp"
#include "support/test_support.hpp"

namespace {
using namespace ps;  // NOLINT(build/namespaces)
int static_constraint_identity() {
  std::vector<std::string> semantic, physical;
  for (int mode = 0; mode < 6; ++mode) {
    auto registry = std::make_shared<OperationRegistry>();
    OperationTraits traits;
    traits.input_count = 1;
    traits.input_schema.resize(1);
    traits.outputs[0].shape_rule = OperationShapeRule::PreserveFirstInput;
    traits.outputs[0].output_element_type = ElementType::Float32;
    if (mode == 4)
      traits.input_schema[0] = {OperationPortKind::Float32Scalar, 0, 1};
    if (mode == 5)
      traits.input_schema[0] = {OperationPortKind::Float32Scalar, 0, 2};
    if (mode >= 4)
      traits.outputs[0].shape_rule = OperationShapeRule::Scalar;
    PS_CHECK(registry
                 ->register_operation({"source", traits,
                                       [](const OperationInvocation& call) {
                                         return Result<Value>(call.inputs[0]);
                                       }})
                 .ok());
    registry->freeze();
    Compiler compiler(registry);
    WorkflowDocument doc;
    doc.inputs = {s1_fixture::declaration(1, "input", s1_fixture::scalar(1)),
                  s1_fixture::declaration(2, "unused", s1_fixture::scalar(1))};
    if (mode == 1)
      doc.inputs[1].facets = {{"semantic", 1, {1}}};
    if (mode == 2)
      doc.inputs[1].facets = {{"semantic", 1, {2}}};
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
