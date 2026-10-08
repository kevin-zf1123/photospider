#include <cstring>
#include <iostream>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "../../examples/unified_result_workflow/minimal_ops.hpp"
#include "photospider/photospider.hpp"
#include "support/test_support.hpp"
namespace {
using namespace ps;  // NOLINT(build/namespaces)
void require(bool condition, std::string_view message) {
  if (!condition)
    throw std::runtime_error(std::string(message));
}
std::pair<ResultRef, ResultRef> run() {
  auto registry = std::make_shared<OperationRegistry>();
  require(unified_example::register_gather(registry.get()).ok(),
          "register Result outputs");
  require(registry->freeze().ok(), "freeze");
  WorkflowDocument doc;
  auto schema =
      std::make_shared<SchemaTemplate>(unified_example::image_schema());
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
  control.result_schema =
      std::make_shared<SchemaTemplate>(unified_example::numeric_schema());
  doc.inputs = {image_a, image_b, control};
  doc.nodes = {{1,
                "example.gather",
                {WorkflowInputReference{1}, WorkflowInputReference{2},
                 WorkflowInputReference{3}},
                {}}};
  doc.outputs = {{"out", 1, "image"}, {"count", 1, "count"}};
  GraphContext graph(doc);
  Compiler compiler(registry);
  auto planned = compiler.compile(graph);
  require(planned.ok(), planned.status().message);
  ExecutionContextConfig config;
  config.managed_resources = ResourceLimits{};
  ExecutionContext context(registry, config);
  auto budget = context.resource_budget().take_value();
  ExecutionBinding a;
  a.name = "a";
  a.result = unified_example::input_image(budget);
  ExecutionBinding b;
  b.name = "b";
  b.result = unified_example::input_image(budget, 5000);
  std::vector<std::int64_t> selectors(32, 0);
  ExecutionBinding c;
  c.name = "control";
  c.result = unified_example::input_control(budget, selectors);
  auto executed = context.execute(planned.value().plan, {{a, b, c}});
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
  auto frozen = context.freeze(planned.value().plan, {{a, b, c}});
  require(frozen.ok(), "freeze typed Result bindings");
  auto wanted = Footprint::from_regions(
                    {2, 2, 2, 4}, {Region({{1, 1}, {0, 1}, {1, 1}, {2, 1}})})
                    .take_value();
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
  auto dirty = sparse.value().dependencies.potential_dirty("control", wanted);
  require(dirty.ok() && dirty.value().at("out") == wanted, "control transpose");
  auto handle =
      context.open_demand(planned.value().plan, {{a, b, c}}).take_value();
  auto initial = handle.request({{"out", wanted}});
  require(initial.ok(), initial.status().message);
  selectors[22] = 3;
  c.result = unified_example::input_control(budget, selectors);
  auto changed = handle.replace_bindings({{a, b, c}});
  require(changed.ok(), changed.status().message);
  require(changed.value().potential_dirty.at("out") == wanted,
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
  const auto sources = updated_result.association();
  require(sources.size() == 2 && sources[0] == b.result.object_id() &&
              sources[1] == c.result.object_id(),
          "image retains ordered branch and control source identities");
  require(count_result.association().empty(),
          "constant count has no source association");
  auto new_data = Footprint::from_regions(
                      {2, 2, 2, 4}, {Region({{1, 1}, {0, 1}, {1, 1}, {3, 1}})})
                      .take_value();
  auto new_dirty = updated.value().dependencies.potential_dirty("b", new_data);
  require(new_dirty.ok() && new_dirty.value().at("out") == wanted,
          "new branch witness replaces old support");
  auto old_dirty = initial.value().dependencies.potential_dirty("b", new_data);
  require(old_dirty.ok() && old_dirty.value().at("out").empty(),
          "old evidence is immutable");
  selectors[0] = 7;
  c.result = unified_example::input_control(budget, selectors);
  auto unrelated =
      handle.replace_bindings({{a, b, c}}, SnapshotAccessOptions{2, {}});
  require(unrelated.ok(), unrelated.status().message);
  require(unrelated.value().potential_dirty.at("out").empty(),
          "unconsumed Control remains clean");
  const auto evaluate = [&](const DemandQuery& query) {
    auto current = context.freeze(planned.value().plan, {{a, b, c}});
    require(current.ok(), current.status().message);
    return context.execute_fragments(current.value(), query);
  };
  selectors[0] = 99;
  c.result = unified_example::input_control(budget, selectors);
  auto bounded = evaluate({{"out", wanted}});
  require(bounded.ok(), bounded.status().message);
  const auto scalar = Footprint::all({1}).take_value();
  auto count_only = evaluate({{"count", scalar}});
  require(count_only.ok(), count_only.status().message);
  const auto& independent = count_only.value().results.at("count");
  require(independent
                  .read_tensor(independent.descriptor().value(), 0, {0}, &count,
                               sizeof(count))
                  .ok() &&
              count == 32,
          "count does not read invalid unneeded selectors");
  auto empty = evaluate({{"out", Footprint::none({2, 2, 2, 4}).take_value()},
                         {"count", Footprint::none({1}).take_value()}});
  require(empty.ok(), empty.status().message);
  for (const auto& entry : empty.value().results)
    require(entry.second.descriptor().value().tensor_coverage(0).empty(),
            "empty Result demand publishes no samples");
  auto invalid_at =
      Footprint::from_regions({2, 2, 2, 4},
                              {Region({{0, 1}, {0, 1}, {0, 1}, {0, 1}})})
          .take_value();
  for (auto invalid : {-1, 8}) {
    selectors[0] = invalid;
    c.result = unified_example::input_control(budget, selectors);
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
