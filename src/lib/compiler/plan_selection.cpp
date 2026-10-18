#include <algorithm>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "compiler/dependency_paths.hpp"
#include "compiler/identity.hpp"
#include "compiler/plan_support.hpp"
#include "photospider/compiler/compiler.hpp"
#include "plugin/dependency_identity.hpp"

namespace ps {
using compiler_internal::plan_support::certification_closure;
using compiler_internal::plan_support::close_publication;
using compiler_internal::plan_support::coverage;
using compiler_internal::plan_support::invalid;
using compiler_internal::plan_support::Unit;

Status Compiler::select_plan(ExecutionPlan* destination) const {
  auto& plan = *destination;
  using Location = std::pair<std::size_t, std::size_t>;
  std::map<Unit, Location> locations;
  std::map<IRCallId, std::size_t> primary;
  std::vector<std::vector<bool>> retained;
  for (std::size_t si = 0; si < plan.steps_.size(); ++si)
    primary.emplace(plan.steps_[si].node_id, si);
  for (std::size_t bi = 0; bi < plan.blocks_.size(); ++bi) {
    const auto& code = plan.blocks_[bi].instructions;
    retained.emplace_back(code.size(), false);
    for (std::size_t i = 0; i < code.size(); ++i) {
      const auto& instruction = code[i];
      locations.emplace(Unit{instruction.step_index, instruction.shared_stage,
                             instruction.tensor_slot, instruction.tile_unit},
                        Location{bi, i});
    }
  }
  std::vector<Location> pending;
  auto keep = [&](Unit unit) -> Status {
    if (std::get<1>(unit) != UINT32_MAX) {
      std::get<0>(unit) = primary.at(plan.steps_[std::get<0>(unit)].node_id);
      const auto& stage =
          *plan.steps_[std::get<0>(unit)].shared_stages[std::get<1>(unit)];
      const auto* scope =
          stage.output_result_schema->publication_scope(std::get<2>(unit));
      if (stage.prepared->requires_complete_stage_validation(
              std::get<1>(unit))) {
        std::get<2>(unit) = 0;
        std::get<3>(unit) = 0;
      } else if (scope &&
                 scope->policy == ResultPublicationPolicy::CompleteBundle) {
        std::get<2>(unit) = scope->members[0];
        std::get<3>(unit) = 0;
      }
    }
    auto found = locations.find(unit);
    if (found == locations.end())
      return invalid("selected tile is absent from the compiled plan");
    const auto [block, instruction] = found->second;
    if (!retained[block][instruction]) {
      retained[block][instruction] = true;
      pending.push_back(found->second);
    }
    return Status::success();
  };
  auto root = [&](const PlanInput& input,
                  const TileRequest* request) -> Status {
    const auto* source = std::get_if<PlanStepInput>(&input);
    if (!source)
      return Status::success();
    const auto& step = plan.steps_[source->step_index];
    auto selected = request
                        ? request->resolve(step.tile_grids)
                        : TileRequest::all(step.tile_grids, ResourceBudget{});
    if (!selected.ok())
      return selected.status();
    auto closed =
        certification_closure(step, UINT32_MAX, selected.take_value());
    if (!closed.ok())
      return closed.status();
    for (const auto& member : closed.value().members)
      for (auto tile : member.second) {
        auto status =
            keep({source->step_index, UINT32_MAX, member.first, tile});
        if (!status.ok())
          return status;
      }
    return Status::success();
  };
  for (const auto& block : plan.blocks_) {
    std::vector<const PlanEdge*> edges;
    if (const auto* jump = std::get_if<PlanJump>(&block.terminator)) {
      edges.push_back(&jump->edge);
    } else if (const auto* branch =
                   std::get_if<PlanConditionalJump>(&block.terminator)) {
      auto status = root(branch->condition, nullptr);
      if (!status.ok())
        return status;
      edges = {&branch->if_true, &branch->if_false};
    } else {
      const auto& returned = std::get<PlanReturn>(block.terminator);
      for (const auto& named : plan.outputs_) {
        auto status = root(returned.values[named.second],
                           &plan.output_requests_.at(named.first));
        if (!status.ok())
          return status;
      }
    }
    for (const auto* edge : edges)
      for (const auto& argument : edge->arguments) {
        auto status = root(argument, nullptr);
        if (!status.ok())
          return status;
      }
  }
  for (const auto& pair : primary) {
    const auto& step = plan.steps_[pair.second];
    if (!step.traits.side_effect_free) {
      auto status = root(PlanStepInput{pair.second}, nullptr);
      if (!status.ok())
        return status;
    }
    for (std::uint32_t stage = 0; stage < step.shared_stages.size(); ++stage) {
      if (!step.prepared->traits().shared_stages[stage].mandatory)
        continue;
      const auto& grids = step.shared_stages[stage]->tile_grids;
      for (std::uint32_t slot = 0; slot < grids.size(); ++slot)
        for (TileIndex tile = 0; tile < grids[slot].tile_count(); ++tile) {
          auto status = keep({pair.second, stage, slot, tile});
          if (!status.ok())
            return status;
        }
    }
  }
  while (!pending.empty()) {
    const auto [bi, at] = pending.back();
    pending.pop_back();
    const auto& instruction = plan.blocks_[bi].instructions[at];
    for (auto dependency : instruction.dependencies) {
      if (dependency >= at)
        return invalid("selected dependency is not ready");
      if (!retained[bi][dependency]) {
        retained[bi][dependency] = true;
        pending.emplace_back(bi, dependency);
      }
    }
    const auto& step = plan.steps_[instruction.step_index];
    for (const auto& read : instruction.inputs) {
      if (read.descriptor_only)
        continue;
      TileRequest selected;
      if (!read.tiles.empty())
        selected.members.emplace(read.tensor_slot, read.tiles);
      auto status = root(step.inputs[read.input], &selected);
      if (!status.ok())
        return status;
    }
    for (const auto& read : instruction.shared_inputs)
      for (const auto& member : read.tiles.members)
        for (auto tile : member.second) {
          auto status =
              keep({instruction.step_index, read.stage, member.first, tile});
          if (!status.ok())
            return status;
        }
  }
  for (std::size_t bi = 0; bi < plan.blocks_.size(); ++bi) {
    auto& previous = plan.blocks_[bi].instructions;
    std::vector<std::size_t> remap(previous.size());
    std::vector<PlanTileInstruction> code;
    for (std::size_t i = 0; i < previous.size(); ++i) {
      if (!retained[bi][i])
        continue;
      remap[i] = code.size();
      for (auto& dependency : previous[i].dependencies)
        dependency = remap[dependency];
      code.push_back(std::move(previous[i]));
    }
    previous = std::move(code);
  }
  return validate_plan(plan);
}

Result<ExecutionPlan> ExecutionPlan::select_outputs(
    const std::map<std::string, TileRequest>& requests) const {
  auto registry = operation_registry_.lock();
  if (!current() || !registry)
    return Result<ExecutionPlan>({ErrorCode::Stale, "tile parent is stale"});
  ExecutionPlan derived = *this;
  derived.outputs_.clear();
  derived.output_requests_.clear();
  for (const auto& requested : requests) {
    auto found = outputs_.find(requested.first);
    if (found == outputs_.end())
      return Result<ExecutionPlan>(invalid("unknown tile output"));
    auto resolved =
        requested.second.resolve(output(requested.first).tile_grids);
    if (!resolved.ok())
      return Result<ExecutionPlan>(resolved.status());
    if (!output_requests_.at(requested.first).contains(resolved.value()))
      return Result<ExecutionPlan>(invalid("tile exceeds requested output"));
    derived.outputs_.emplace(requested.first, found->second);
    derived.output_requests_.emplace(requested.first, resolved.take_value());
  }
  Compiler compiler(registry);
  auto status = compiler.finalize_plan(&derived);
  if (status.ok())
    status = compiler.select_plan(&derived);
  if (!status.ok())
    return Result<ExecutionPlan>(status);
  derived.digest_.value = compiler_internal::physical_digest(derived);
  derived.cache_key_.value =
      compiler_internal::plan_cache_key(derived.digest_.value);
  auto work = UINT64_MAX;
  derived.structured_templates_ =
      contract_internal::dependency_cache_templates(derived, {}, &work, {});
  return Result<ExecutionPlan>(std::move(derived));
}

Result<ExecutionPlan> ExecutionPlan::tile_plan(
    const std::string& name, const TileRequest& request) const {
  return select_outputs({{name, request}});
}
}  // namespace ps
