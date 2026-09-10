#include <cstdint>
#include <iostream>
#include <vector>

#include "workflow.hpp"  // NOLINT(build/include_subdir)

namespace foundations {
namespace {
ps::Value mask(const std::vector<float>& data,
               const std::vector<std::uint64_t>& shape) {
  return array(data, shape, facets(ps::coverage_semantics()));
}
ps::Result<ps::ExecutionResult> label(const ps::Value& input,
                                      std::int64_t capacity) {
  ps::PlanningOptions tiled;
  tiled.tile_height = 1;
  tiled.tile_width = 1;
  return evaluate({input},
                  {{1,
                    "mask.components",
                    {ps::WorkflowInputReference{1}},
                    {{"capacity", capacity}}}},
                  tiled);
}
ps::Value attribute(const char* key, const ps::Value& labels,
                    std::int64_t capacity) {
  return output(operation(key, {labels}, {{"capacity", capacity}}));
}
}  // namespace
void components() {
  const auto empty = output(label(mask(std::vector<float>(6), {2, 3}), 1));
  exact<std::int64_t>(empty, std::vector<std::int64_t>(6));
  exact<std::int64_t>(attribute("component.count", empty, 2), {0});
  exact<std::int64_t>(attribute("component.area", empty, 2), {0, 0, 0});
  exact<std::int64_t>(attribute("component.bbox", empty, 2),
                      std::vector<std::int64_t>(12));
  const auto bridge = output(label(mask({1, 0, 1, 1, 1, 1}, {2, 3}), 1));
  exact<std::int64_t>(bridge, {1, 0, 1, 1, 1, 1});
  exact<std::int64_t>(attribute("component.area", bridge, 2), {0, 5, 0});
  exact<std::int64_t>(attribute("component.bbox", bridge, 2),
                      {0, 0, 0, 0, 0, 0, 3, 2, 0, 0, 0, 0});
  const auto input = mask({1, 0, 1, 0, 1, 0, 1, 0, 1}, {3, 3});
  const auto diagonal = output(label(input, 5));
  exact<std::int64_t>(diagonal, {1, 0, 2, 0, 3, 0, 4, 0, 5});
  exact<std::int64_t>(attribute("component.count", diagonal, 5), {5});
  rejected(label(input, 4), ps::ErrorCode::OperationFailed);
  ps::SemanticDescriptor field;
  field.kind = ps::SemanticKind::ScalarField;
  field.channels = {{"value", "value", "dimensionless"}};
  const auto values = array<float>({-.5F, 1, .25F, 1}, {2, 2}, facets(field));
  auto chain = output(evaluate(
      {values},
      {{1,
        "mask.threshold",
        {ps::WorkflowInputReference{1}},
        {{"threshold", .5}}},
       next(2, "mask.components", 1, {{"capacity", std::int64_t{1}}}),
       next(3, "component.count", 2, {{"capacity", std::int64_t{1}}})}));
  exact<std::int64_t>(chain, {1});
  std::cout << "components empty_labels_count=0 "
               "empty_tables=nonzero_capacity_zeroed bridge_area=5 "
               "bridge_bbox=[0,0,3,2] diagonal_labels=[1,0,2,0,3,0,4,0,5] "
               "cross_tile=whole capacity_overflow=rejected oracle=passed\n";
}
}  // namespace foundations
