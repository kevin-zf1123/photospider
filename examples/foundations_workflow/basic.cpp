#include <cmath>
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

#include "workflow.hpp"  // NOLINT(build/include_subdir)

namespace foundations {
namespace {
ps::WorkflowInput ref(std::uint64_t id) {
  return ps::WorkflowNodeOutput{id, "value"};
}
}  // namespace
void basic_filters() {
  const auto source = array<float>({1, 2, 3}, {1, 3});
  const auto kernel = array<float>({1, 2}, {1, 2});
  const Parameters filter{{"anchor_y", std::int64_t{0}},
                          {"anchor_x", std::int64_t{0}},
                          {"boundary", std::string("zero")}};
  auto nodes = std::vector<ps::WorkflowNode>{
      {1,
       "field.correlate",
       {ps::WorkflowInputReference{1}, ps::WorkflowInputReference{2}},
       filter},
      {2,
       "field.convolve",
       {ps::WorkflowInputReference{1}, ps::WorkflowInputReference{2}},
       filter},
      {3, "numeric.subtract", {ref(1), ref(2)}, {}},
      next(4, "numeric.abs", 3),
      next(5, "analysis.histogram", 4,
           {{"bins", std::int64_t{2}}, {"range_min", 0.}, {"range_max", 8.}}),
      next(6, "analysis.histogram_out_of_range", 4,
           {{"range_min", 0.}, {"range_max", 8.}})};
  auto doc = document({source, kernel}, nodes);
  doc.outputs = {{"counts", 5, "value"},
                 {"outside", 6, "value"},
                 {"error", 4, "value"}};
  const auto registry = ps::make_default_operation_registry();
  ps::GraphContext graph(doc);
  auto compiled = take(ps::Compiler(registry).compile(graph));
  ps::ExecutionContext execution(registry);
  const auto result =
      take(execution.execute(compiled.plan, bindings({source, kernel})));
  exact<float>(result.values.at("error"), {4, 4, 4});
  exact<std::int64_t>(result.values.at("counts"), {0, 3});
  exact<std::int64_t>(result.values.at("outside"), {0, 0});
  std::cout << "basic-filters: asymmetric kernels -> abs error -> histogram "
               "[0,3], total=3\n";
}
}  // namespace foundations
