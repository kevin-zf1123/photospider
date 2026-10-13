#include <cstdint>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <utility>
#include <vector>

#include "support/kernel_registry_fixture.hpp"
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
  auto schema = multi_result::schema(ElementType::Float64, {1, width});
  schema.tensors[0].layout.spatial = true;
  schema.tensors[0].layout.channel_axis = {};
  schema.tensors[0].layout.height_axis = 0;
  schema.tensors[0].layout.width_axis = 1;
  WorkflowDocument document;
  document.inputs = {multi_result::declaration(1, "source", schema)};
  document.nodes = {{1, "core.identity", {WorkflowInputReference{1}}, {}}};
  document.outputs = {{"result", 1, "value"}};
  auto operations = ps::test::kernel_registry();
  GraphContext graph(document);
  PlanningOptions options;
  options.tile_extent = 1;
  const auto compiled = checked(Compiler(operations).compile(graph, options));
  ExecutionContext execution(operations, {1, false, 8, 65536});
  const auto root = checked(execution.resource_budget());
  auto builder = checked(
      ResultBuilder::start(root, schema, "dependency.sparse", {}, {}, 1));
  for (const auto coordinate : {UINT64_C(1), width - 2}) {
    const double value = static_cast<double>(coordinate);
    auto bytes = checked(root.allocator().allocate(sizeof(value)));
    std::memcpy(bytes.data(), &value, sizeof(value));
    multi_result::check(builder.publish_tensor(
        0, Region({{0, 1}, {coordinate, 1}}), {0, {8, 8}, {0, coordinate}},
        std::move(bytes).freeze()));
  }
  ExecutionBinding source;
  source.name = "source";
  source.result = checked(builder.seal());
  const auto tiles = checked(TileSet::from_indices(root, {1, width - 2}));
  const TileRequest request{false, tiles};
  const auto coverage = checked(Footprint::from_regions(
      {1, width},
      {Region({{0, 1}, {1, 1}}), Region({{0, 1}, {width - 2, 1}})}));
  const auto frozen = checked(execution.freeze(compiled.plan, {{source}}));
  auto result =
      checked(execution.execute_fragments(frozen, {{"result", request}}));
  const auto& fragments = result.results.at("result");
  require(multi_result::number(fragments, {0, 1}) == 1 &&
              multi_result::number(fragments, {0, width - 2}) == 999999998,
          "independent identity oracle failed");
  const auto facts = checked(fragments.descriptor());
  require(facts.tensor_coverage(0) == coverage, "sparse coverage mismatch");
  double hole = 7;
  require(fragments.read_tensor(facts, 0, {0, width / 2}, &hole, sizeof(hole))
                      .code == ErrorCode::InvalidArgument &&
              hole == 7,
          "hole was materialized");
  ResourceVector<TileChange> changes{ResourceAllocator<TileChange>(root)};
  changes.push_back({PlanWorkflowInput{0}, 0,
                     checked(TileSet::from_indices(root, {1})), false});
  const auto dirty = checked(result.dependencies.dirty_units(changes));
  require(dirty.size() == 1 && dirty[0] == TileUnit{0, 0, 1},
          "identity tile invalidation");
  for (auto tile : {UINT64_C(1), width - 2}) {
    auto record = checked(result.dependencies.tile_record({0, 0, tile}));
    require(record && record->inputs.size() == 1 && !record->inputs[0].whole &&
                record->inputs[0].tiles ==
                    checked(TileSet::from_indices(root, {tile})),
            "identity records only its actual source tile");
  }
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
int main() {
  try {
    data_workflow();
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
