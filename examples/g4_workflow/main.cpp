#include <cstring>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "result_support.hpp"  // NOLINT(build/include_subdir)

void progressive_workflow();
void dynamic_workflow();
void demand_workflow();
void sharing_workflow();
void radius_retention_workflow();
void reductions_workflow();
void scan_workflow();
void block_cache_workflow();

namespace {
void require(bool condition, const char* message) {
  if (!condition)
    throw std::runtime_error(message);
}
template <class T>
T checked(ps::Result<T> result) {
  if (!result.ok())
    throw std::runtime_error(result.status().message);
  return result.take_value();
}
void data_workflow() {
  using namespace ps;  // NOLINT(build/namespaces)
  constexpr std::uint64_t width = 1000000000;
  const auto schema = g4_result::schema(ElementType::Float64, width);
  WorkflowDocument document;
  document.inputs = {g4_result::declaration(1, "source", schema)};
  document.nodes = {{1, "core.identity", {WorkflowInputReference{1}}, {}}};
  document.outputs = {{"result", 1, "value"}};
  auto operations = make_default_operation_registry();
  GraphContext graph(document);
  const auto compiled = checked(Compiler(operations).compile(graph));
  ExecutionContext execution(operations, {1, false, 8, 4096});
  const auto root = checked(execution.resource_budget());
  auto builder = checked(ResultBuilder::start(root, schema, "g4.sparse"));
  g4_result::check(builder.bind_descriptor_relation(
      checked(ResultRelation::cartesian(root, 1, {}))));
  for (const auto coordinate : {UINT64_C(1), width - 2}) {
    const double value = static_cast<double>(coordinate);
    g4_result::check(builder.publish_tensor(
        0, Region({{coordinate, 1}}),
        {reinterpret_cast<const std::uint8_t*>(&value), sizeof(value)},
        checked(ResultRelation::cartesian(root, width, {})),
        {true, true, true, true}));
  }
  ExecutionBinding source;
  source.name = "source";
  source.result = checked(builder.seal());
  const auto request = checked(Footprint::from_regions(
      {width}, {Region({{1, 1}}), Region({{width - 2, 1}})}));
  const auto frozen = checked(execution.freeze(compiled.plan, {{source}}));
  auto result =
      checked(execution.execute_fragments(frozen, {{"result", request}}));
  const auto& fragments = result.results.at("result");
  require(g4_result::number(fragments, 1) == 1 &&
              g4_result::number(fragments, width - 2) == 999999998,
          "independent identity oracle failed");
  const auto facts = checked(fragments.descriptor());
  require(facts.tensor_coverage(0) == request, "sparse coverage mismatch");
  double hole = 7;
  require(
      fragments.read_tensor(facts, 0, {width / 2}, &hole, sizeof(hole)).code ==
              ErrorCode::InvalidArgument &&
          hole == 7,
      "hole was materialized");
  auto dirty = checked(Footprint::from_regions({width}, {Region({{1, 1}})}));
  require(checked(result.dependencies.potential_dirty(
                      "source", dirty, 1, {}, ResultSupportTarget::Tensor, 0))
                  .at("result") == dirty,
          "identity transpose oracle failed");
  require(checked(result.dependencies.source_support()).at("source") == request,
          "identity source support mismatch");
  require(root.statistics().peak[ResourceKind::Payload] <= 4096,
          "sparse source exceeded the payload budget");
  auto original = Value::create({ElementType::Int64, {2}}, Region::whole({2}),
                                {0, {8}}, std::vector<std::uint8_t>(16))
                      .take_value();
  InputSnapshotStore store({32, 1});
  auto snapshot = checked(store.import_value(original));
  require(checked(snapshot.content_identity(original.region())).size() == 64,
          "generic snapshot identity missing");
  std::cout << "data: values=[1,999999998], source_samples=2, hole=rejected, "
               "transpose={1}, generic_snapshot=ok\n";
}
}  // namespace
int main(int argc, char** argv) {
  try {
    if (argc == 2 && std::string(argv[1]) == "--radius-only") {
      dynamic_workflow();
      demand_workflow();
      radius_retention_workflow();
      return 0;
    }
    if (argc == 3 && std::string(argv[1]) == "--scenario") {
      const std::pair<const char*, void (*)()> scenarios[] = {
          {"data", data_workflow},      {"progressive", progressive_workflow},
          {"shared", sharing_workflow}, {"reductions", reductions_workflow},
          {"scan", scan_workflow},      {"blocks", block_cache_workflow}};
      for (const auto& scenario : scenarios)
        if (std::string(argv[2]) == scenario.first) {
          scenario.second();
          return 0;
        }
      throw std::runtime_error("unknown G4 scenario");
    }
    if (argc != 1)
      throw std::runtime_error(
          "usage: photospider_dependency_workflow [--radius-only | --scenario "
          "data|progressive|shared|reductions|scan|blocks]");
    data_workflow();
    progressive_workflow();
    dynamic_workflow();
    demand_workflow();
    sharing_workflow();
    radius_retention_workflow();
    reductions_workflow();
    scan_workflow();
    block_cache_workflow();
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
