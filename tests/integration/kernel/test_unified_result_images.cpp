#include <cstring>
#include <iostream>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "photospider/photospider.hpp"
#include "support/test_support.hpp"
#include "support/unified_result_fixture.hpp"
namespace {
using namespace ps;  // NOLINT(build/namespaces)
void require(bool condition, std::string_view message) {
  if (!condition)
    throw std::runtime_error(std::string(message));
}
std::pair<ResultRef, ResultRef> run() {
  auto registry = std::make_shared<OperationRegistry>();
  require(unified_result_fixture::register_gather(registry.get()).ok(),
          "register Result outputs");
  require(registry->freeze().ok(), "freeze");
  WorkflowDocument doc;
  auto schema =
      std::make_shared<SchemaTemplate>(unified_result_fixture::image_schema());
  WorkflowInputDeclaration image_a;
  image_a.id = 1;
  image_a.name = "a";
  image_a.result_schema = schema;
  auto image_b = image_a;
  image_b.id = 2;
  image_b.name = "b";
  WorkflowInputDeclaration control;
  control.id = 3;
  control.name = "control";
  control.result_schema = std::make_shared<SchemaTemplate>(
      unified_result_fixture::numeric_schema());
  doc.inputs = {image_a, image_b, control};
  doc.nodes = {{1,
                "example.gather",
                {WorkflowInputReference{1}, WorkflowInputReference{2},
                 WorkflowInputReference{3}},
                {}}};
  doc.outputs = {{"out", 1, "image"}, {"count", 1, "count"}};
  GraphContext graph(doc);
  Compiler compiler(registry);
  PlanningOptions options;
  options.tile_extent = 1;
  auto planned = compiler.compile(graph, options);
  require(planned.ok(), planned.status().message);
  ExecutionContextConfig config;
  config.managed_resources = ResourceLimits{};
  ExecutionContext context(registry, config);
  auto budget = context.resource_budget().take_value();
  ExecutionBinding a;
  a.name = "a";
  a.result = unified_result_fixture::input_image(budget);
  ExecutionBinding b;
  b.name = "b";
  b.result = unified_result_fixture::input_image(budget, 5000);
  std::vector<std::int64_t> selectors(32, 0);
  ExecutionBinding c;
  c.name = "control";
  c.result = unified_result_fixture::input_control(budget, selectors);
  auto executed = context.execute(planned.value().plan.value(), {{a, b, c}});
  require(executed.ok(), executed.status().message);
  auto& result = executed.value().results.at("out");
  auto facts = result.descriptor().take_value();
  float pixel = 0;
  require(result.read_tensor(facts, 0, {1, 1, 1, 3}, &pixel, 4).ok() &&
              pixel == 1113,
          "frame/layer sample");
  std::int64_t count = 0;
  const auto& count_result = executed.value().results.at("count");
  require(count_result
                  .read_tensor(count_result.descriptor().value(), 0, {0},
                               &count, sizeof(count))
                  .ok() &&
              count == 32,
          "numeric Result sibling");
  auto frozen = context.freeze(planned.value().plan.value(), {{a, b, c}});
  require(frozen.ok(), "freeze typed Result bindings");
  const auto wanted_tiles = TileSet::from_indices(budget, {22}).take_value();
  const TileRequest wanted{{{0, wanted_tiles}}};
  auto sparse = context.execute_fragments(frozen.value(), {{"out", wanted}});
  require(sparse.ok(), sparse.status().message);
  auto sparse_result = sparse.value().results.at("out");
  auto captured = sparse_result.descriptor().take_value();
  require(
      sparse_result.read_tensor(captured, 0, {1, 0, 1, 2}, &pixel, 4).ok() &&
          pixel == 1012,
      "sparse sample");
  require(!sparse_result.read_tensor(captured, 0, {1, 0, 1, 1}, &pixel, 4).ok(),
          "hole is unauthorized");
  ResourceVector<TileChange> control_change{
      ResourceAllocator<TileChange>(budget)};
  control_change.push_back({PlanWorkflowInput{2}, 0, 0, {}, true});
  auto dirty = sparse.value().dependencies.dirty_units(control_change);
  const auto return_record = planned.value().plan->output("out").record_index;
  const auto expected_dirty = [&](const auto& units) {
    return units.size() == 2 && units[0] == TileUnit{0, 0, 0, 22} &&
           units[1] == TileUnit{return_record, 0, 0, 22};
  };
  require(dirty.ok() && expected_dirty(dirty.value()),
          "control descriptor change reaches the call tile and return root");
  const auto control_grid =
      TileGrid::from_schema(unified_result_fixture::numeric_schema(), 1, 0)
          .take_value();
  control_change[0].descriptor_changed = false;
  control_change[0].tiles =
      TileSet::range(budget, 0, control_grid.tile_count()).take_value();
  auto payload_dirty = sparse.value().dependencies.dirty_units(control_change);
  require(payload_dirty.ok() && expected_dirty(payload_dirty.value()),
          "full control payload change reaches the call tile and return root");
  auto handle = context.open_demand(planned.value().plan.value(), {{a, b, c}})
                    .take_value();
  auto initial = handle.request({{"out", wanted}});
  require(initial.ok(), initial.status().message);
  selectors[22] = 3;
  c.result = unified_result_fixture::input_control(budget, selectors);
  auto changed = handle.replace_bindings({{a, b, c}});
  require(changed.ok(), changed.status().message);
  require(
      changed.value().potential_dirty.at("out").members.at(0) == wanted_tiles,
      "consumed Control changes are dirty");
  auto updated = handle.request({{"out", wanted}});
  require(updated.ok(), updated.status().message);
  auto updated_result = updated.value().results.at("out");
  require(updated_result
                  .read_tensor(updated_result.descriptor().value(), 0,
                               {1, 0, 1, 2}, &pixel, 4)
                  .ok() &&
              pixel == 6013,
          "new branch/support executes");
  ResourceVector<TileChange> source_change{
      ResourceAllocator<TileChange>(budget)};
  source_change.push_back({PlanWorkflowInput{1}, 0, 0,
                           TileSet::from_indices(budget, {23}).take_value(),
                           false});
  auto new_dirty = updated.value().dependencies.dirty_units(source_change);
  require(new_dirty.ok() && expected_dirty(new_dirty.value()),
          "selected branch lies in the fixed static support");
  auto old_dirty = initial.value().dependencies.dirty_units(source_change);
  require(old_dirty.ok() && expected_dirty(old_dirty.value()),
          "prior snapshot retains the same immutable static support");
  const auto& original = initial.value().results.at("out");
  require(original.read_tensor(original.descriptor().value(), 0, {1, 0, 1, 2},
                               &pixel, 4)
                  .ok() &&
              pixel == 1012,
          "later execution changed the earlier Result");
  source_change[0].tiles = TileSet::from_indices(budget, {19}).take_value();
  auto other_row = updated.value().dependencies.dirty_units(source_change);
  require(other_row.ok() && other_row.value().empty(),
          "source changes in another row do not dirty this output");
  selectors[0] = 7;
  c.result = unified_result_fixture::input_control(budget, selectors);
  auto unrelated = handle.replace_bindings(
      {{a, b, c}}, SnapshotAccessOptions{32 + 2 * 4, {}});
  require(unrelated.ok(), unrelated.status().message);
  require(
      unrelated.value().potential_dirty.at("out").members.at(0) == wanted_tiles,
      "Full control payload edits conservatively invalidate subscribed tiles");
  const auto evaluate = [&](const DemandQuery& query) {
    auto current = context.freeze(planned.value().plan.value(), {{a, b, c}});
    require(current.ok(), current.status().message);
    return context.execute_fragments(current.value(), query);
  };
  selectors[0] = 99;
  c.result = unified_result_fixture::input_control(budget, selectors);
  auto bounded = evaluate({{"out", wanted}});
  require(bounded.ok(), bounded.status().message);
  const TileRequest scalar{{{0, TileSet::range(budget, 0, 1).take_value()}}};
  auto count_only = evaluate({{"count", scalar}});
  require(count_only.ok(), count_only.status().message);
  const auto& independent = count_only.value().results.at("count");
  require(independent
                  .read_tensor(independent.descriptor().value(), 0, {0}, &count,
                               sizeof(count))
                  .ok() &&
              count == 32,
          "count does not read invalid unneeded selectors");
  auto empty = evaluate({{"out", TileRequest{}}, {"count", TileRequest{}}});
  require(empty.ok(), empty.status().message);
  for (const auto& entry : empty.value().results)
    require(entry.second.descriptor().value().tensor_coverage(0).empty(),
            "empty Result demand publishes no samples");
  const TileRequest invalid_at{
      {{0, TileSet::from_indices(budget, {0}).take_value()}}};
  for (auto invalid : {-1, 8}) {
    selectors[0] = invalid;
    c.result = unified_result_fixture::input_control(budget, selectors);
    auto rejected = evaluate({{"out", invalid_at}});
    require(
        !rejected.ok() && rejected.status().code == ErrorCode::InvalidArgument,
        "consumed selector outside 0..7 is rejected");
  }
  for (std::size_t port : {0U, 2U}) {
    auto malformed = doc;
    auto wrong =
        std::make_shared<SchemaTemplate>(*malformed.inputs[port].result_schema);
    wrong->tensors[0].descriptor.shape.back() = 3;
    malformed.inputs[port].result_schema = std::move(wrong);
    auto rejected = compiler.compile(GraphContext(std::move(malformed)));
    require(!rejected.ok() && rejected.status().code == ErrorCode::TypeMismatch,
            "gather validates fixed Result dimensions before execution");
  }
  return {independent, updated_result};
}
}  // namespace
int main() {
  try {
    auto retained = run();
    std::int64_t count = 0;
    float sample = 0;
    require(retained.first
                    .read_tensor(retained.first.descriptor().value(), 0, {0},
                                 &count, sizeof(count))
                    .ok() &&
                count == 32 &&
                retained.second
                    .read_tensor(retained.second.descriptor().value(), 0,
                                 {1, 0, 1, 2}, &sample, sizeof(sample))
                    .ok() &&
                sample == 6013,
            "both Result outputs survive context retirement");
    std::cout << "unified Result tensors passed\n";
    return 0;
  } catch (const std::exception& e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
