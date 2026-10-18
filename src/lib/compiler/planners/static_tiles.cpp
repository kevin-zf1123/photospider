#include "compiler/planners/static_tiles.hpp"

#include <algorithm>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "compiler/plan_support.hpp"

namespace ps::compiler_internal {
using compiler_internal::plan_support::certification_closure;
using compiler_internal::plan_support::close_publication;
using compiler_internal::plan_support::coverage;
using compiler_internal::plan_support::invalid;
using compiler_internal::plan_support::Unit;

namespace {
Status expand(PlanBuilder& plan, const PlanningOptions& options) {
  if (!options.maximum_instructions || !options.maximum_dependency_edges)
    return invalid("static planning budgets must be positive");
  std::map<IRBlockId, std::size_t> block_by_id;
  std::map<IRCallId, std::size_t> primary;
  for (std::size_t bi = 0; bi < plan.blocks().size(); ++bi) {
    block_by_id.emplace(plan.blocks()[bi].id, bi);
    plan.instructions(bi).clear();
  }
  for (std::size_t i = 0; i < plan.steps().size(); ++i)
    primary.emplace(plan.steps()[i].node_id, i);
  std::map<Unit, unsigned> color;
  std::map<Unit, std::pair<std::size_t, std::size_t>> locations;
  std::uint64_t units = 0, edges = 0;
  auto contract = [&](const Unit& unit) -> const PlanStep& {
    const auto& step = plan.steps()[std::get<0>(unit)];
    return std::get<1>(unit) == UINT32_MAX
               ? step
               : *step.shared_stages.at(std::get<1>(unit));
  };
  auto canonical = [&](Unit unit) {
    if (std::get<1>(unit) != UINT32_MAX) {
      std::get<0>(unit) = primary.at(plan.steps()[std::get<0>(unit)].node_id);
      const auto& step = contract(unit);
      const auto* scope =
          step.output_result_schema->publication_scope(std::get<2>(unit));
      if (step.prepared->requires_complete_stage_validation(
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
  struct Frame {
    Unit unit;
    PlanTileInstruction instruction;
    std::vector<Unit> dependencies;
    std::size_t next = 0;
  };
  auto build = [&](Unit unit) -> Result<Frame> {
    if (++units > options.maximum_instructions)
      return Result<Frame>(
          {ErrorCode::ResourceExhausted, "static instruction limit"});
    Frame frame;
    frame.unit = unit;
    auto& instruction = frame.instruction;
    instruction.step_index = std::get<0>(unit);
    instruction.shared_stage = std::get<1>(unit);
    instruction.tensor_slot = std::get<2>(unit);
    instruction.tile_unit = std::get<3>(unit);
    const auto& step = contract(unit);
    if (instruction.shared_stage != UINT32_MAX &&
        step.backend == Backend::Gpu &&
        plan.execution_mode() == ExecutionMode::CpuExact)
      return Result<Frame>(
          {ErrorCode::BackendUnavailable,
           "GPU shared stage is unavailable in CPU execution mode"});
    if ((step.backend == Backend::Cpu && !step.traits.supports_cpu) ||
        (step.backend == Backend::Gpu && !step.traits.supports_gpu))
      return Result<Frame>(
          {ErrorCode::BackendUnavailable, "compiled backend unavailable"});
    if (instruction.tensor_slot >= step.tile_grids.size() ||
        instruction.tile_unit >=
            step.tile_grids[instruction.tensor_slot].tile_count())
      return Result<Frame>(invalid("invalid static tile address"));
    auto tile = TileSet::range(ResourceBudget{}, instruction.tile_unit,
                               instruction.tile_unit + 1);
    if (!tile.ok())
      return Result<Frame>(tile.status());
    instruction.publication.members.emplace(instruction.tensor_slot,
                                            tile.take_value());
    if (instruction.shared_stage != UINT32_MAX) {
      auto closed = certification_closure(step, instruction.shared_stage,
                                          instruction.publication);
      if (!closed.ok())
        return Result<Frame>(closed.status());
      instruction.publication = closed.take_value();
      const auto* scope =
          step.output_result_schema->publication_scope(instruction.tensor_slot);
      if (scope && scope->policy == ResultPublicationPolicy::StablePrefix &&
          instruction.tile_unit)
        frame.dependencies.push_back(
            canonical({instruction.step_index, instruction.shared_stage,
                       instruction.tensor_slot, instruction.tile_unit - 1}));
    } else {
      auto samples =
          coverage(step.tile_grids[instruction.tensor_slot],
                   instruction.publication.members.at(instruction.tensor_slot));
      if (!samples.ok())
        return Result<Frame>(samples.status());
      instruction.tensor_outputs = samples.take_value();
    }
    auto output_samples = coverage(step.tile_grids, instruction.publication);
    if (!output_samples.ok())
      return Result<Frame>(output_samples.status());
    instruction.publication_samples = output_samples.take_value();
    ResultStaticQuery query{*step.structured_metadata,
                            step.parameters,
                            *step.prepared,
                            step.backend,
                            plan.tile_extent(),
                            step.output_index,
                            instruction.shared_stage,
                            instruction.tensor_slot,
                            instruction.tile_unit,
                            instruction.publication,
                            instruction.tensor_outputs};
    auto mapped = step.prepared->map_dependencies(query);
    if (!mapped.ok())
      return Result<Frame>(mapped.status());
    const auto& projection = step.traits.outputs[0].input_indices;
    std::map<std::pair<std::uint32_t, std::uint32_t>, PlanTensorRead> reads;
    for (const auto& read : mapped.value().inputs) {
      if (read.input >= step.inputs.size() ||
          (projection && std::find(projection->begin(), projection->end(),
                                   read.input) == projection->end()))
        return Result<Frame>(invalid("static read violates input projection"));
      auto resolved = read.resolve(plan.input_grids(step.inputs[read.input]),
                                   ResourceBudget{});
      if (!resolved.ok())
        return Result<Frame>(resolved.status());
      const auto& item = resolved.value();
      if (!item.descriptor_only && item.tiles.empty())
        continue;
      auto key = std::make_pair(
          item.input, item.descriptor_only ? UINT32_MAX : item.tensor_slot);
      auto found = reads.find(key);
      if (found == reads.end()) {
        reads.emplace(key, PlanTensorRead{item.input,
                                          item.tensor_slot,
                                          item.descriptor_only,
                                          item.tiles,
                                          {}});
      } else {
        auto joined = found->second.tiles.unite(item.tiles, ResourceBudget{});
        if (!joined.ok())
          return Result<Frame>(joined.status());
        found->second.tiles = joined.take_value();
      }
    }
    std::set<std::uint32_t> full_inputs;
    for (const auto& item : reads)
      if (!item.second.descriptor_only &&
          step.prepared->requires_complete_validation(false, item.second.input))
        full_inputs.insert(item.second.input);
    for (auto port : full_inputs) {
      const auto& grids = plan.input_grids(step.inputs[port]);
      for (std::uint32_t member = 0; member < grids.size(); ++member) {
        auto all =
            TileSet::range(ResourceBudget{}, 0, grids[member].tile_count());
        if (!all.ok())
          return Result<Frame>(all.status());
        reads[{port, member}] =
            PlanTensorRead{port, member, false, all.take_value(), {}};
      }
    }
    for (auto& pair : reads) {
      auto& read = pair.second;
      const auto& input = step.inputs[read.input];
      auto samples =
          coverage(plan.input_grids(input)[read.tensor_slot], read.tiles);
      if (!samples.ok())
        return Result<Frame>(samples.status());
      read.samples = samples.take_value();
      if (!read.descriptor_only) {
        TileRequest needed;
        needed.members.emplace(read.tensor_slot, read.tiles);
        auto closed =
            close_publication(plan.input_schema(input), plan.input_grids(input),
                              std::move(needed));
        if (!closed.ok())
          return Result<Frame>(closed.status());
        if (const auto* producer = std::get_if<PlanStepInput>(&input)) {
          closed = certification_closure(plan.steps()[producer->step_index],
                                         UINT32_MAX, closed.take_value());
          if (!closed.ok())
            return Result<Frame>(closed.status());
          if (closed.value().size() > options.maximum_dependency_edges - edges)
            return Result<Frame>(
                {ErrorCode::ResourceExhausted, "static dependency edge limit"});
          for (const auto& member : closed.value().members)
            for (auto tile_index : member.second)
              frame.dependencies.push_back(
                  {producer->step_index, UINT32_MAX, member.first, tile_index});
        }
      }
      instruction.inputs.push_back(std::move(read));
    }
    std::map<std::uint32_t, TileRequest> shared_reads;
    for (const auto& read : mapped.value().shared_stages) {
      const auto& original = plan.steps()[instruction.step_index];
      if (read.stage >= original.shared_stages.size())
        return Result<Frame>(invalid("missing static shared stage"));
      const auto& stage = *original.shared_stages[read.stage];
      auto selected =
          read.tiles ? read.tiles->resolve(stage.tile_grids)
                     : TileRequest::all(stage.tile_grids, ResourceBudget{});
      if (!selected.ok())
        return Result<Frame>(selected.status());
      for (const auto& member : selected.value().members) {
        auto& target = shared_reads[read.stage].members[member.first];
        auto joined = target.unite(member.second, ResourceBudget{});
        if (!joined.ok())
          return Result<Frame>(joined.status());
        target = joined.take_value();
      }
    }
    for (const auto& merged : shared_reads) {
      const ResultSharedNeed read{merged.first, merged.second};
      const auto& original = plan.steps()[instruction.step_index];
      if (read.stage >= original.shared_stages.size())
        return Result<Frame>(
            invalid("static read references a missing shared stage"));
      const auto& stage = *original.shared_stages[read.stage];
      auto selected =
          read.tiles ? read.tiles->resolve(stage.tile_grids)
                     : TileRequest::all(stage.tile_grids, ResourceBudget{});
      if (!selected.ok())
        return Result<Frame>(selected.status());
      auto closed =
          certification_closure(stage, read.stage, selected.take_value());
      if (!closed.ok())
        return Result<Frame>(closed.status());
      PlanSharedRead compiled;
      compiled.stage = read.stage;
      compiled.tiles = closed.take_value();
      auto samples = coverage(stage.tile_grids, compiled.tiles);
      if (!samples.ok())
        return Result<Frame>(samples.status());
      compiled.samples = samples.take_value();
      if (compiled.tiles.size() > options.maximum_dependency_edges - edges)
        return Result<Frame>(
            {ErrorCode::ResourceExhausted, "static shared dependency limit"});
      for (const auto& member : compiled.tiles.members)
        for (auto tile_index : member.second)
          frame.dependencies.push_back(canonical(
              {instruction.step_index, read.stage, member.first, tile_index}));
      instruction.shared_inputs.push_back(std::move(compiled));
    }
    if (instruction.shared_stage == UINT32_MAX &&
        step.backend == Backend::Gpu && step.traits.allows_cpu_fallback &&
        step.traits.supports_cpu && step.traits.deterministic &&
        step.traits.side_effect_free) {
      auto cpu_query = query;
      cpu_query.backend = Backend::Cpu;
      auto cpu = step.prepared->map_dependencies(cpu_query);
      if (!cpu.ok())
        return Result<Frame>(cpu.status());
      // A permitted retry is optional. Keep the GPU instruction when its
      // fixed grants cannot prove a compatible CPU retry.
      bool compatible = instruction.shared_inputs.empty();
      for (const auto& read : cpu.value().shared_stages) {
        if (read.stage >= step.shared_stages.size())
          return Result<Frame>(
              invalid("CPU fallback references an unknown stage"));
        auto resolved =
            read.tiles
                ? read.tiles->resolve(
                      step.shared_stages[read.stage]->tile_grids)
                : TileRequest::all(step.shared_stages[read.stage]->tile_grids,
                                   ResourceBudget{});
        if (!resolved.ok())
          return Result<Frame>(resolved.status());
        if (!resolved.value().empty())
          compatible = false;
      }
      auto covered = [&](const TileNeed& read) {
        for (const auto& grant : instruction.inputs)
          if (grant.input == read.input &&
              (read.descriptor_only || (!grant.descriptor_only &&
                                        grant.tensor_slot == read.tensor_slot &&
                                        grant.tiles.contains(read.tiles))))
            return true;
        return !read.descriptor_only && read.tiles.empty();
      };
      for (const auto& read : cpu.value().inputs) {
        if (read.input >= step.inputs.size() ||
            (projection && std::find(projection->begin(), projection->end(),
                                     read.input) == projection->end()))
          return Result<Frame>(
              invalid("CPU fallback read violates input projection"));
        const auto& grids = plan.input_grids(step.inputs[read.input]);
        auto resolved = read.resolve(grids, ResourceBudget{});
        if (!resolved.ok())
          return Result<Frame>(resolved.status());
        if (!covered(resolved.value()))
          compatible = false;
        if (!read.descriptor_only && !read.tiles.empty() &&
            step.prepared->requires_complete_validation(false, read.input)) {
          for (std::uint32_t member = 0; member < grids.size(); ++member) {
            auto all =
                TileSet::range(ResourceBudget{}, 0, grids[member].tile_count());
            if (!all.ok())
              return Result<Frame>(all.status());
            if (!covered({read.input, member, false, all.take_value()}))
              compatible = false;
          }
        }
      }
      instruction.cpu_fallback = compatible;
    }
    std::sort(frame.dependencies.begin(), frame.dependencies.end());
    frame.dependencies.erase(
        std::unique(frame.dependencies.begin(), frame.dependencies.end()),
        frame.dependencies.end());
    if (frame.dependencies.size() > options.maximum_dependency_edges - edges)
      return Result<Frame>(
          {ErrorCode::ResourceExhausted, "static dependency edge limit"});
    edges += frame.dependencies.size();
    return Result<Frame>(std::move(frame));
  };
  auto emit = [&](Unit root) -> Status {
    root = canonical(root);
    if (color[root] == 2)
      return Status::success();
    std::vector<Frame> stack;
    auto initial = build(root);
    if (!initial.ok())
      return initial.status();
    stack.push_back(initial.take_value());
    color[root] = 1;
    while (!stack.empty()) {
      auto& frame = stack.back();
      if (frame.next < frame.dependencies.size()) {
        auto next = canonical(frame.dependencies[frame.next++]);
        if (color[next] == 1)
          return {ErrorCode::Cycle, "static tile dependency cycle"};
        if (color[next] == 2)
          continue;
        auto child = build(next);
        if (!child.ok())
          return child.status();
        color[next] = 1;
        stack.push_back(child.take_value());
      } else {
        auto bi = block_by_id.at(plan.steps()[std::get<0>(frame.unit)].block);
        auto& instructions = plan.instructions(bi);
        for (auto dependency : frame.dependencies) {
          auto where = locations.at(canonical(dependency));
          if (where.first == bi)
            frame.instruction.dependencies.push_back(where.second);
        }
        std::sort(frame.instruction.dependencies.begin(),
                  frame.instruction.dependencies.end());
        frame.instruction.dependencies.erase(
            std::unique(frame.instruction.dependencies.begin(),
                        frame.instruction.dependencies.end()),
            frame.instruction.dependencies.end());
        locations.emplace(frame.unit, std::make_pair(bi, instructions.size()));
        instructions.push_back(std::move(frame.instruction));
        color[frame.unit] = 2;
        stack.pop_back();
      }
    }
    return Status::success();
  };
  auto root = [&](const PlanInput& input,
                  const TileRequest* request) -> Status {
    const auto* source = std::get_if<PlanStepInput>(&input);
    if (!source)
      return Status::success();
    const auto& step = plan.steps()[source->step_index];
    auto all = request ? request->resolve(step.tile_grids)
                       : TileRequest::all(step.tile_grids, ResourceBudget{});
    if (!all.ok())
      return all.status();
    auto closed = certification_closure(step, UINT32_MAX, all.take_value());
    if (!closed.ok())
      return closed.status();
    if (closed.value().size() > options.maximum_instructions)
      return {ErrorCode::ResourceExhausted, "static root instruction limit"};
    for (const auto& member : closed.value().members)
      for (auto tile : member.second) {
        auto status =
            emit({source->step_index, UINT32_MAX, member.first, tile});
        if (!status.ok())
          return status;
      }
    return Status::success();
  };
  for (const auto& block : plan.blocks()) {
    std::vector<const PlanEdge*> edges_to_visit;
    if (const auto* jump = std::get_if<PlanJump>(&block.terminator)) {
      edges_to_visit.push_back(&jump->edge);
    } else if (const auto* branch =
                   std::get_if<PlanConditionalJump>(&block.terminator)) {
      auto status = root(branch->condition, nullptr);
      if (!status.ok())
        return status;
      edges_to_visit = {&branch->if_true, &branch->if_false};
    } else {
      const auto& returned = std::get<PlanReturn>(block.terminator);
      for (const auto& named : plan.outputs()) {
        auto status = root(returned.values[named.second],
                           &plan.output_requests().at(named.first));
        if (!status.ok())
          return status;
      }
    }
    for (const auto* edge : edges_to_visit)
      for (const auto& argument : edge->arguments) {
        auto status = root(argument, nullptr);
        if (!status.ok())
          return status;
      }
  }
  for (const auto& pair : primary) {
    const auto& step = plan.steps()[pair.second];
    if (!step.traits.side_effect_free) {
      auto status = root(PlanStepInput{pair.second}, nullptr);
      if (!status.ok())
        return status;
    }
    for (std::uint32_t si = 0; si < step.shared_stages.size(); ++si) {
      if (!step.prepared->traits().shared_stages[si].mandatory)
        continue;
      const auto& stage = *step.shared_stages[si];
      for (std::uint32_t member = 0; member < stage.tile_grids.size();
           ++member) {
        if (stage.tile_grids[member].tile_count() >
            options.maximum_instructions)
          return {ErrorCode::ResourceExhausted,
                  "mandatory stage instruction limit"};
        for (TileIndex tile = 0; tile < stage.tile_grids[member].tile_count();
             ++tile) {
          auto status = emit({pair.second, si, member, tile});
          if (!status.ok())
            return status;
        }
      }
    }
  }
  // Preserve IR call/effect order while retaining dependency order within each
  // call's expanded tile and shared-stage program.
  for (std::size_t bi = 0; bi < plan.blocks().size(); ++bi) {
    const auto& block = plan.blocks()[bi];
    auto& code = plan.instructions(bi);
    std::map<IRCallId, std::size_t> order;
    for (std::size_t i = 0; i < block.calls.size(); ++i)
      order.emplace(plan.steps()[block.calls[i]].node_id, i);
    std::vector<std::size_t> permutation(code.size());
    for (std::size_t i = 0; i < permutation.size(); ++i)
      permutation[i] = i;
    std::stable_sort(
        permutation.begin(), permutation.end(), [&](auto a, auto b) {
          return order.at(plan.steps()[code[a].step_index].node_id) <
                 order.at(plan.steps()[code[b].step_index].node_id);
        });
    std::vector<std::size_t> inverse(permutation.size());
    for (std::size_t i = 0; i < permutation.size(); ++i)
      inverse[permutation[i]] = i;
    std::vector<PlanTileInstruction> instructions;
    for (auto old : permutation) {
      auto instruction = std::move(code[old]);
      for (auto& dependency : instruction.dependencies)
        dependency = inverse[dependency];
      std::sort(instruction.dependencies.begin(),
                instruction.dependencies.end());
      instructions.push_back(std::move(instruction));
    }
    code = std::move(instructions);
  }
  return Status::success();
}

}  // namespace

PlannerDefinition static_tile_planner() {
  return {
      "static-tiles", "1", "Demand-driven static tile expansion",
      [](const ValidatedIR&, PlanBuilder& builder,
         const PlanningOptions& options) { return expand(builder, options); }};
}
}  // namespace ps::compiler_internal
