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
Parameters domain() {
  return {{"domain_min", 0.},
          {"domain_max", 1.},
          {"out_of_domain", std::string("reject")}};
}
Parameters generated(double value) {
  return {{"height", std::int64_t{1}},
          {"width", std::int64_t{3}},
          {"dtype", std::string("float32")},
          {"value", value}};
}
}  // namespace
void basic_curves() {
  const auto source = array<float>({.125, .25, .375, .5, .25, .125, 0, .5},
                                   {1, 2, 4}, facets(ps::rgba_semantics()));
  const auto points = array<float>({0, 0, .5, .25, 1, 1}, {3, 2});
  const auto mask =
      array<float>({0, 1}, {1, 2}, facets(ps::coverage_semantics()));
  auto sampling = domain();
  sampling["count"] = std::int64_t{5};
  std::vector<ps::WorkflowNode> nodes = {
      {1, "alpha.unassociate", {ps::WorkflowInputReference{1}}, {}},
      {2, "curve.sample_monotone", {ps::WorkflowInputReference{2}}, sampling}};
  for (std::int64_t channel = 0; channel < 4; ++channel)
    nodes.push_back(
        next(3 + channel, "channel.extract", 1, {{"index", channel}}));
  nodes.push_back({7, "field.apply_lut_1d", {ref(3), ref(2)}, domain()});
  auto straight = ps::rgba_semantics();
  straight.association = "straight";
  nodes.push_back({8,
                   "channel.merge",
                   {ref(7), ref(4), ref(5), ref(6)},
                   interpretation(straight)});
  nodes.push_back(next(9, "alpha.associate", 8));
  nodes.push_back(
      {10,
       "image.mix",
       {ps::WorkflowInputReference{1}, ref(9), ps::WorkflowInputReference{3}},
       {}});
  exact<float>(output(evaluate({source, points, mask}, nodes)),
               {.125, .25, .375, .5, .125, .125, 0, .5});
  std::cout << "basic-curves: PCHIP -> channel LUT -> premul mix, alpha=.5\n";
}
void basic_masks() {
  const auto impulse = array<float>({0, 0, 0, 0, 1, 0, 0, 0, 0}, {3, 3},
                                    facets(ps::coverage_semantics()));
  Parameters footprint{{"radius", std::int64_t{1}},
                       {"footprint", std::string("square")}};
  const auto result = output(evaluate(
      {impulse},
      {{1, "mask.invert", {ps::WorkflowInputReference{1}}, {}},
       {2,
        "mask.combine",
        {ref(1), ps::WorkflowInputReference{1}},
        {{"operation", std::string("and")}, {"algebra", std::string("fuzzy")}}},
       {3,
        "mask.combine",
        {ref(2), ps::WorkflowInputReference{1}},
        {{"operation", std::string("or")}, {"algebra", std::string("fuzzy")}}},
       next(4, "mask.dilate", 3, footprint),
       next(5, "mask.erode", 4, footprint),
       next(6, "field.box_mean", 5, {{"radius", std::int64_t{1}}})}));
  close(result, std::vector<float>(9, 1.F / 9));
  close(output(operation("mask.invert", {result})),
        std::vector<float>(9, 8.F / 9));
  std::cout << "basic-masks: Boolean -> closing -> feather, coverage=1/9\n";
}
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
void basic_fields() {
  const Parameters coords{{"height", std::int64_t{1}},
                          {"width", std::int64_t{3}},
                          {"dtype", std::string("float32")},
                          {"axis", std::string("x")},
                          {"space", std::string("normalized")}};
  auto nodes = std::vector<ps::WorkflowNode>{
      {1, "field.coordinate", {}, coords},
      {2, "field.constant", {}, generated(.25)},
      {3, "field.constant", {}, generated(1)},
      next(4, "field.smoothstep", 1, {{"edge0", 0.}, {"edge1", 1.}}),
      next(5, "grade.levels", 2,
           {{"black", 0.},
            {"white", 1.},
            {"gamma", 2.},
            {"out_min", 0.},
            {"out_max", 1.}}),
      {6,
       "channel.merge",
       {ref(2), ref(2), ref(2), ref(3)},
       interpretation(ps::rgba_semantics())},
      {7,
       "channel.merge",
       {ref(5), ref(5), ref(5), ref(3)},
       interpretation(ps::rgba_semantics())},
      {8, "image.mix", {ref(6), ref(7), ref(4)}, {}}};
  std::vector<float> expected;
  for (double x : {1. / 6, .5, 5. / 6}) {
    const float v = static_cast<float>(.25 + .25 * x * x * (3 - 2 * x));
    expected.insert(expected.end(), {v, v, v, 1});
  }
  close(output(evaluate({}, nodes)), expected);
  std::cout << "basic-fields: normalized coordinates -> soft mask -> local "
               "levels, alpha=1\n";
}
}  // namespace foundations
