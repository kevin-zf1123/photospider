#include <string>
#include <utility>
#include <vector>

#include "04-mask-morphology/common.hpp"
#include "photospider/ops/mask/gap_fill.hpp"
#include "photospider/ops/numeric/workflow_authoring.hpp"

namespace ps::mask {
namespace {
using plugin_internal::mask::parameter;
using plugin_internal::mask::Parameters;
Result<GapFillOutputs> append_gap_fill(
    WorkflowDocument& document, WorkflowInput barrier, WorkflowInput seeds,
    const std::string& closure_key, const char* closure_port,
    Parameters parameters, std::int64_t connectivity, std::int64_t max_work) {
  parameter(connectivity == 4 || connectivity == 8, "gap-fill connectivity");
  parameter(max_work >= 0, "negative gap-fill max_work");
  auto ids =
      numeric::available_workflow_node_ids(document, 3, {barrier, seeds});
  if (!ids.ok())
    return Result<GapFillOutputs>(ids.status());
  const auto closure = ids.value()[0], prepare = ids.value()[1],
             flood = ids.value()[2];
  parameters["max_work"] = max_work;
  WorkflowDocument staged = document;
  staged.nodes.push_back(
      {closure, closure_key, {barrier}, std::move(parameters)});
  staged.nodes.push_back(
      {prepare,
       "mask.prepare_gap_fill_strict",
       {barrier, WorkflowNodeOutput{closure, closure_port}, seeds},
       {{"max_work", max_work}}});
  staged.nodes.push_back(
      {flood,
       "mask.flood_barrier_strict",
       {WorkflowNodeOutput{prepare, "seeds"},
        WorkflowNodeOutput{prepare, "barrier"}},
       {{"connectivity", connectivity}, {"max_work", max_work}}});
  GapFillOutputs outputs{{flood, "values"},
                         {prepare, "barrier"},
                         {prepare, "ignored_seeds"},
                         {prepare, "reopened_barrier"}};
  document = std::move(staged);
  return Result<GapFillOutputs>(std::move(outputs));
}
}  // namespace

Result<GapFillOutputs> fill_axis_gaps(WorkflowDocument& document,
                                      WorkflowInput barrier,
                                      WorkflowInput seeds,
                                      std::int64_t maximum_gap,
                                      std::int64_t fill_connectivity,
                                      std::int64_t max_work) {
  try {
    parameter(maximum_gap >= 0, "negative axis-gap support");
    return append_gap_fill(document, std::move(barrier), std::move(seeds),
                           "mask.bridge_axis_gaps_strict", "temporary",
                           {{"maximum_gap", maximum_gap}}, fill_connectivity,
                           max_work);
  } catch (const Status& failure) {
    return Result<GapFillOutputs>(failure);
  }
}
Result<GapFillOutputs> fill_morphological_gaps(
    WorkflowDocument& document, WorkflowInput barrier, WorkflowInput seeds,
    const GapFillFootprint& footprint, std::int64_t fill_connectivity,
    std::int64_t max_work) {
  try {
    Parameters parameters{{"footprint", footprint.kind}};
    if (footprint.radius)
      parameters["radius"] = *footprint.radius;
    if (footprint.offsets)
      parameters["offsets"] = *footprint.offsets;
    const plugin_internal::mask::Footprint checked(parameters);
    return append_gap_fill(document, std::move(barrier), std::move(seeds),
                           "mask.closing_strict", "values",
                           std::move(parameters), fill_connectivity, max_work);
  } catch (const Status& failure) {
    return Result<GapFillOutputs>(failure);
  }
}
}  // namespace ps::mask
