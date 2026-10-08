#include "execution/dependency_record.hpp"

#include "execution/dependency_evidence_state.hpp"

namespace ps::execution_internal {
Status dependency_failure(const char* message) {
  return Status::failure(ErrorCode::InvalidArgument, message);
}

void DependencyRecord::retire(DependencyRecord* record) noexcept {
  thread_local DependencyRecord* pending = nullptr;
  thread_local bool draining = false;
  record->retired_next = pending;
  pending = record;
  if (draining)
    return;
  draining = true;
  while (pending) {
    auto* next = pending;
    pending = next->retired_next;
    delete next;
  }
  draining = false;
}
DependencyTarget dependency_target(const ExecutionPlan& plan,
                                   const PlanInput& input) {
  if (const auto* step = std::get_if<PlanStepInput>(&input))
    return {false, plan.steps().at(step->step_index).node_id,
            plan.steps().at(step->step_index).output_index};
  return {true, plan.input_declarations()
                    .at(std::get<PlanWorkflowInput>(input).declaration_index)
                    .id};
}
}  // namespace ps::execution_internal
