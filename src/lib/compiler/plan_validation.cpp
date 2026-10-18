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

Status Compiler::validate_plan(const ExecutionPlan& plan) const {
  if (plan.blocks_.empty() || plan.entry_ >= plan.blocks_.size())
    return invalid("invalid physical entry");
  using Location = std::pair<std::size_t, std::size_t>;
  std::map<Unit, Location> locations;
  std::map<IRCallId, std::size_t> primary;
  std::map<IRBlockId, std::size_t> block_indices;
  for (std::size_t i = 0; i < plan.steps_.size(); ++i)
    primary.emplace(plan.steps_[i].node_id, i);
  for (std::size_t bi = 0; bi < plan.blocks_.size(); ++bi)
    block_indices.emplace(plan.blocks_[bi].id, bi);
  auto canonical = [&](Unit unit) {
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
    return unit;
  };
  for (std::size_t bi = 0; bi < plan.blocks_.size(); ++bi) {
    const auto& block = plan.blocks_[bi];
    std::map<IRCallId, std::size_t> order;
    for (std::size_t i = 0; i < block.calls.size(); ++i)
      order.emplace(plan.steps_[block.calls[i]].node_id, i);
    std::size_t previous_call = 0;
    for (std::size_t i = 0; i < block.instructions.size(); ++i) {
      const auto& instruction = block.instructions[i];
      if (instruction.step_index >= plan.steps_.size())
        return invalid("static instruction references a missing call");
      const auto& original = plan.steps_[instruction.step_index];
      if (original.block != block.id || !order.count(original.node_id) ||
          (instruction.shared_stage != UINT32_MAX &&
           instruction.shared_stage >= original.shared_stages.size()))
        return invalid("static instruction belongs to another block");
      if (order.at(original.node_id) < previous_call)
        return invalid("static instruction order violates IR call order");
      previous_call = order.at(original.node_id);
      const auto& step =
          instruction.shared_stage == UINT32_MAX
              ? original
              : *original.shared_stages[instruction.shared_stage];
      if (instruction.cpu_fallback &&
          (instruction.shared_stage != UINT32_MAX ||
           step.backend != Backend::Gpu || !step.traits.supports_cpu ||
           !step.traits.supports_gpu || !step.traits.allows_cpu_fallback ||
           !step.traits.deterministic || !step.traits.side_effect_free ||
           !instruction.shared_inputs.empty()))
        return invalid("invalid compiled CPU fallback");
      if ((step.backend != Backend::Cpu && step.backend != Backend::Gpu) ||
          (step.backend == Backend::Cpu && !step.traits.supports_cpu) ||
          (step.backend == Backend::Gpu && !step.traits.supports_gpu) ||
          (plan.execution_mode_ == ExecutionMode::CpuExact &&
           step.backend != Backend::Cpu))
        return {ErrorCode::BackendUnavailable,
                "stored instruction backend is unsupported"};
      if (instruction.tensor_slot >= step.tile_grids.size() ||
          instruction.tile_unit >=
              step.tile_grids[instruction.tensor_slot].tile_count())
        return invalid("stored instruction tile is outside its grid");
      const Unit unit{instruction.step_index, instruction.shared_stage,
                      instruction.tensor_slot, instruction.tile_unit};
      if (canonical(unit) != unit ||
          !locations.emplace(unit, Location{bi, i}).second)
        return invalid("duplicate or noncanonical static tile instruction");
      if (!std::is_sorted(instruction.dependencies.begin(),
                          instruction.dependencies.end()) ||
          std::adjacent_find(instruction.dependencies.begin(),
                             instruction.dependencies.end()) !=
              instruction.dependencies.end())
        return invalid("noncanonical static dependency list");
      for (auto dependency : instruction.dependencies)
        if (dependency >= i)
          return invalid("static dependency is not ready");
      auto one = TileSet::range(ResourceBudget{}, instruction.tile_unit,
                                instruction.tile_unit + 1);
      if (!one.ok())
        return one.status();
      TileRequest publication;
      publication.members.emplace(instruction.tensor_slot, one.take_value());
      if (instruction.shared_stage != UINT32_MAX) {
        if (instruction.tensor_outputs)
          return invalid("shared instruction has a public tile output");
        auto closed =
            certification_closure(step, instruction.shared_stage, publication);
        if (!closed.ok())
          return closed.status();
        publication = closed.take_value();
      } else {
        auto expected =
            coverage(step.tile_grids[instruction.tensor_slot],
                     publication.members.at(instruction.tensor_slot));
        if (!expected.ok())
          return expected.status();
        if (!instruction.tensor_outputs ||
            *instruction.tensor_outputs != expected.value())
          return invalid("stored public tile output mismatch");
      }
      auto resolved = instruction.publication.resolve(step.tile_grids);
      if (!resolved.ok() || resolved.value() != publication ||
          instruction.publication != publication)
        return invalid("stored tile publication mismatch");
      auto expected = coverage(step.tile_grids, publication);
      if (!expected.ok() || expected.value() != instruction.publication_samples)
        return invalid("stored publication samples mismatch");
      const auto& projection = step.traits.outputs[0].input_indices;
      auto allowed = [&](std::uint32_t port) {
        return port < step.inputs.size() &&
               (!projection || std::find(projection->begin(), projection->end(),
                                         port) != projection->end());
      };
      std::set<std::pair<std::uint32_t, std::uint32_t>> reads;
      for (const auto& read : instruction.inputs) {
        if (!allowed(read.input) ||
            !reads
                 .emplace(read.input,
                          read.descriptor_only ? UINT32_MAX : read.tensor_slot)
                 .second)
          return invalid("duplicate or unauthorized static input");
        TileNeed need{read.input, read.tensor_slot, read.descriptor_only,
                      read.tiles};
        auto normalized = need.resolve(
            plan.input_grids(step.inputs[read.input]), ResourceBudget{});
        if (!normalized.ok())
          return normalized.status();
        if (read.descriptor_only) {
          if (!read.samples.empty())
            return invalid("descriptor-only input has stored samples");
          continue;
        }
        auto samples = coverage(
            plan.input_grids(step.inputs[read.input])[read.tensor_slot],
            read.tiles);
        if (!samples.ok() || samples.value() != read.samples)
          return invalid("stored input samples mismatch");
      }
      for (const auto& read : instruction.inputs) {
        if (read.descriptor_only ||
            !step.prepared->requires_complete_validation(false, read.input))
          continue;
        const auto& grids = plan.input_grids(step.inputs[read.input]);
        for (std::uint32_t member = 0; member < grids.size(); ++member) {
          const auto found = std::find_if(
              instruction.inputs.begin(), instruction.inputs.end(),
              [&](const auto& other) {
                return !other.descriptor_only && other.input == read.input &&
                       other.tensor_slot == member;
              });
          if (found == instruction.inputs.end() ||
              found->tiles.size() != grids[member].tile_count())
            return invalid("static input omits complete validation coverage");
        }
      }
      std::set<std::uint32_t> stages;
      for (const auto& read : instruction.shared_inputs) {
        if (read.stage >= original.shared_stages.size() ||
            !stages.insert(read.stage).second)
          return invalid("duplicate or missing static shared stage");
        for (auto port : original.prepared->traits()
                             .shared_stages[read.stage]
                             .input_indices)
          if (!allowed(port))
            return invalid("static shared read exceeds input projection");
        const auto& shared = *original.shared_stages[read.stage];
        auto normalized = read.tiles.resolve(shared.tile_grids);
        if (!normalized.ok() || normalized.value() != read.tiles)
          return invalid("stored shared tiles mismatch");
        auto closed = certification_closure(shared, read.stage, read.tiles);
        if (!closed.ok() || closed.value() != read.tiles)
          return invalid("stored shared read is not publication-closed");
        auto samples = coverage(shared.tile_grids, read.tiles);
        if (!samples.ok() || samples.value() != read.samples)
          return invalid("stored shared samples mismatch");
      }
    }
  }
  auto ready = [&](Unit unit, std::size_t block, std::size_t before,
                   const std::vector<std::size_t>* declared) -> Status {
    auto found = locations.find(canonical(unit));
    if (found == locations.end())
      return invalid("planned producer tile is missing");
    if (found->second.first == block) {
      if (found->second.second >= before)
        return invalid("planned producer tile is not ready");
      if (declared && !std::binary_search(declared->begin(), declared->end(),
                                          found->second.second))
        return invalid("static dependency list omits an input producer");
    }
    // The owning IR has already proved cross-block value dominance.
    return Status::success();
  };
  auto require = [&](const PlanInput& input, const TileRequest* request,
                     std::size_t block, std::size_t before,
                     const std::vector<std::size_t>* declared) -> Status {
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
            ready({source->step_index, UINT32_MAX, member.first, tile}, block,
                  before, declared);
        if (!status.ok())
          return status;
      }
    return Status::success();
  };
  for (std::size_t bi = 0; bi < plan.blocks_.size(); ++bi) {
    const auto& block = plan.blocks_[bi];
    for (std::size_t i = 0; i < block.instructions.size(); ++i) {
      const auto& instruction = block.instructions[i];
      const auto& original = plan.steps_[instruction.step_index];
      for (const auto& read : instruction.inputs) {
        if (read.descriptor_only)
          continue;
        TileRequest needed;
        if (!read.tiles.empty())
          needed.members.emplace(read.tensor_slot, read.tiles);
        auto status = require(original.inputs[read.input], &needed, bi, i,
                              &instruction.dependencies);
        if (!status.ok())
          return status;
      }
      for (const auto& read : instruction.shared_inputs)
        for (const auto& member : read.tiles.members)
          for (auto tile : member.second) {
            auto status =
                ready({instruction.step_index, read.stage, member.first, tile},
                      bi, i, &instruction.dependencies);
            if (!status.ok())
              return status;
          }
      if (instruction.shared_stage != UINT32_MAX && instruction.tile_unit) {
        const auto& stage = *original.shared_stages[instruction.shared_stage];
        const auto* scope = stage.output_result_schema->publication_scope(
            instruction.tensor_slot);
        if (scope && scope->policy == ResultPublicationPolicy::StablePrefix) {
          auto status =
              ready({instruction.step_index, instruction.shared_stage,
                     instruction.tensor_slot, instruction.tile_unit - 1},
                    bi, i, &instruction.dependencies);
          if (!status.ok())
            return status;
        }
      }
    }
    std::vector<const PlanEdge*> edges;
    if (const auto* jump = std::get_if<PlanJump>(&block.terminator)) {
      edges.push_back(&jump->edge);
    } else if (const auto* branch =
                   std::get_if<PlanConditionalJump>(&block.terminator)) {
      auto status = require(branch->condition, nullptr, bi,
                            block.instructions.size(), nullptr);
      if (!status.ok())
        return status;
      edges = {&branch->if_true, &branch->if_false};
    } else {
      const auto& returned = std::get<PlanReturn>(block.terminator);
      for (const auto& named : plan.outputs_) {
        auto status = require(returned.values[named.second],
                              &plan.output_requests_.at(named.first), bi,
                              block.instructions.size(), nullptr);
        if (!status.ok())
          return status;
      }
    }
    for (const auto* edge : edges)
      for (const auto& argument : edge->arguments) {
        auto status =
            require(argument, nullptr, bi, block.instructions.size(), nullptr);
        if (!status.ok())
          return status;
      }
  }
  for (const auto& pair : primary) {
    const auto& step = plan.steps_[pair.second];
    const auto bi = block_indices.at(step.block);
    if (!step.traits.side_effect_free) {
      auto status = require(PlanStepInput{pair.second}, nullptr, bi,
                            plan.blocks_[bi].instructions.size(), nullptr);
      if (!status.ok())
        return status;
    }
    for (std::uint32_t stage = 0; stage < step.shared_stages.size(); ++stage) {
      if (!step.prepared->traits().shared_stages[stage].mandatory)
        continue;
      const auto& grids = step.shared_stages[stage]->tile_grids;
      for (std::uint32_t slot = 0; slot < grids.size(); ++slot)
        for (TileIndex tile = 0; tile < grids[slot].tile_count(); ++tile) {
          auto status = ready({pair.second, stage, slot, tile}, bi,
                              plan.blocks_[bi].instructions.size(), nullptr);
          if (!status.ok())
            return status;
        }
    }
  }
  return Status::success();
}

}  // namespace ps
