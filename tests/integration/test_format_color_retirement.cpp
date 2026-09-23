#include <array>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "photospider/photospider.hpp"
#include "support/test_support.hpp"

int main() {
  using namespace ps;  // NOLINT(build/namespaces)
  const auto registry = make_default_operation_registry();
  constexpr std::array<const char*, 13> retired{
      "channel.extract",     "channel.merge",         "channel.swizzle",
      "alpha.associate",     "alpha.unassociate",     "color.assign",
      "color.rgb_to_xyz",    "color.xyz_to_rgb",      "color.xyz_to_lab",
      "color.lab_to_xyz",    "color.rgb_to_ycbcr420", "numeric.cast",
      "numeric.encode_range"};
  const auto input = Value::from_float64(0.25);
  const std::vector<Value> inputs{input};
  const std::vector<Region> demands{input.region()};
  const std::map<std::string, ParameterValue> parameters;
  for (const auto* key : retired) {
    PS_CHECK(registry->find_traits(key).status().code == ErrorCode::NotFound);
    PS_CHECK(
        registry->invoke(key, {inputs, demands, parameters}).status().code ==
        ErrorCode::NotFound);
    WorkflowDocument document;
    document.inputs = {{1, "input", input.descriptor(), input.region(),
                        input.layout(), input.facets()}};
    document.nodes = {{1, key, {WorkflowInputReference{1}}, {}}};
    document.outputs = {{"result", 1, "value"}};
    GraphContext graph(document);
    PS_CHECK(Compiler(registry).compile(graph).status().code ==
             ErrorCode::NotFound);
  }
  // Unrelated numeric operations remain usable through the public graph API.
  WorkflowDocument document;
  document.inputs = {{1, "input", input.descriptor(), input.region(),
                      input.layout(), input.facets()}};
  document.nodes = {{1,
                     "numeric.add_strict",
                     {WorkflowInputReference{1}, WorkflowInputReference{1}},
                     {}}};
  document.outputs = {{"result", 1, "values"}};
  GraphContext graph(document);
  auto compiled = Compiler(registry).compile(graph);
  PS_CHECK(compiled.ok());
  ExecutionContext execution(registry);
  auto result = execution.execute(compiled.value().plan, {{{"input", input}}});
  PS_CHECK(result.ok());
  PS_CHECK(result.value().values.at("result").as_float64().value() == 0.5);
  return 0;
}
