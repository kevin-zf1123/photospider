#include <algorithm>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "compiler/identity.hpp"
#include "compiler/limits.hpp"
#include "photospider/compiler/compiler.hpp"
#include "plugin/dependency_identity.hpp"

namespace ps {
namespace {
Status invalid(const char* message) {
  return {ErrorCode::InvalidArgument, message};
}
using Unit = std::tuple<std::size_t, std::uint32_t, std::uint32_t, TileIndex>;

Result<Footprint> coverage(const TileGrid& grid, const TileSet& tiles) {
  return grid.coverage(tiles, compiler_internal::static_set_limits());
}

Result<std::vector<Footprint>> coverage(const std::vector<TileGrid>& grids,
                                        const TileRequest& request) {
  std::vector<Footprint> result;
  for (std::uint32_t i = 0; i < grids.size(); ++i) {
    auto found = request.members.find(i);
    auto samples = coverage(
        grids[i], found == request.members.end() ? TileSet{} : found->second);
    if (!samples.ok())
      return Result<std::vector<Footprint>>(samples.status());
    result.push_back(samples.take_value());
  }
  return Result<std::vector<Footprint>>(std::move(result));
}
Result<TileRequest> close_publication(const SchemaTemplate& schema,
                                      const std::vector<TileGrid>& grids,
                                      TileRequest request) {
  auto resolved = request.resolve(grids);
  if (!resolved.ok())
    return resolved;
  request = resolved.take_value();
  for (const auto& scope : schema.publication_scopes) {
    bool used = false;
    for (auto member : scope.members)
      used = used || request.members.count(member);
    if (!used)
      continue;
    if (scope.policy == ResultPublicationPolicy::CompleteBundle) {
      for (auto member : scope.members) {
        auto all =
            TileSet::range(ResourceBudget{}, 0, grids[member].tile_count());
        if (!all.ok())
          return Result<TileRequest>(all.status());
        request.members[member] = all.take_value();
      }
    } else if (scope.policy == ResultPublicationPolicy::StablePrefix) {
      auto member = scope.members[0];
      const auto& selected = request.members.at(member);
      if (!selected.empty()) {
        auto prefix = TileSet::range(ResourceBudget{}, 0,
                                     selected.intervals().back().end);
        if (!prefix.ok())
          return Result<TileRequest>(prefix.status());
        request.members[member] = prefix.take_value();
      }
    }
  }
  return Result<TileRequest>(std::move(request));
}
// Validators are compiler-visible dependencies: their full certification
// barrier is resolved before any consumer can observe the selected value.
Result<TileRequest> certification_closure(const PlanStep& step,
                                          std::uint32_t stage,
                                          TileRequest request) {
  const bool complete =
      stage == UINT32_MAX
          ? step.prepared->requires_complete_validation(true, step.output_index)
          : step.prepared->requires_complete_stage_validation(stage);
  if (complete && !request.empty())
    return TileRequest::all(step.tile_grids, ResourceBudget{});
  return close_publication(*step.output_result_schema, step.tile_grids,
                           std::move(request));
}
}  // namespace

const SchemaTemplate& ExecutionPlan::input_schema(
    const PlanInput& input) const {
  if (const auto* step = std::get_if<PlanStepInput>(&input))
    return *steps_.at(step->step_index).output_result_schema;
  if (const auto* declaration = std::get_if<PlanWorkflowInput>(&input))
    return *input_declarations_.at(declaration->declaration_index)
                .result_schema;
  return *parameters_.at(std::get<PlanArgumentInput>(input).parameter_index)
              .result_schema;
}
const std::vector<TileGrid>& ExecutionPlan::input_grids(
    const PlanInput& input) const {
  if (const auto* step = std::get_if<PlanStepInput>(&input))
    return steps_.at(step->step_index).tile_grids;
  if (const auto* declaration = std::get_if<PlanWorkflowInput>(&input))
    return input_tile_grids_.at(declaration->declaration_index);
  return parameters_.at(std::get<PlanArgumentInput>(input).parameter_index)
      .tile_grids;
}

Result<ExecutionPlan> Compiler::link_plan(
    const OptimizedGraphIR& optimized, const PlanningOptions& options,
    const std::vector<Backend>* placements) const {
  if (!optimized.current() || !optimized.revision() ||
      optimized.operation_registry_.lock().get() != operations_.get())
    return Result<ExecutionPlan>({ErrorCode::Stale, "invalid optimized IR"});
  if ((options.execution_mode != ExecutionMode::CpuExact &&
       options.execution_mode != ExecutionMode::NativeGpu) ||
      !options.tile_extent || (options.tile_extent & (options.tile_extent - 1)))
    return Result<ExecutionPlan>(
        invalid("invalid planning mode or tile extent"));
  ExecutionPlan result;
  result.linked_program_ = optimized.program();
  result.revision_ = optimized.revision_;
  result.input_declarations_ = optimized.input_declarations_;
  result.resources_ = optimized.resources_;
  result.optimized_digest_ = optimized.digest_;
  result.operation_registry_ = operations_;
  result.current_check_ = optimized.current_check_;
  result.execution_mode_ = options.execution_mode;
  result.tile_extent_ = options.tile_extent;
  result.output_requests_ = options.output_requests;
  std::map<IRValueId, PlanInput> values;
  std::map<IRBlockId, std::size_t> blocks;
  std::map<IRCallId, IRBlockId> call_blocks;
  std::map<IRCallId, std::vector<std::size_t>> call_steps;
  for (std::size_t i = 0; i < optimized.input_bindings_.size(); ++i)
    values.emplace(optimized.input_bindings_[i].value, PlanWorkflowInput{i});
  for (const auto& block : optimized.blocks_) {
    PlanBlock physical;
    physical.id = block.id;
    blocks.emplace(block.id, result.blocks_.size());
    for (const auto& parameter : block.parameters) {
      const auto index = result.parameters_.size();
      values.emplace(parameter.value, PlanArgumentInput{index});
      physical.parameters.push_back(index);
      result.parameters_.push_back(
          {parameter.value, block.id, parameter.result_schema, {}});
    }
    for (auto call : block.calls)
      call_blocks.emplace(call, block.id);
    result.blocks_.push_back(std::move(physical));
  }
  result.entry_ = blocks.at(optimized.entry_);
  for (const auto& node : optimized.nodes_) {
    for (std::uint32_t oi = 0; oi < node.outputs.size(); ++oi) {
      PlanStep step;
      step.node_id = node.id;
      step.block = call_blocks.at(node.id);
      step.value = node.outputs[oi].value;
      step.operation = node.operation;
      step.output_index = oi;
      step.parameters = node.parameters;
      step.prepared = node.prepared;
      auto selected = select_operation_output(node.traits, oi);
      if (!selected.ok())
        return Result<ExecutionPlan>(selected.status());
      step.traits = selected.take_value();
      step.output_result_schema = node.outputs[oi].result_schema;
      if (placements) {
        if (result.steps_.size() >= placements->size())
          return Result<ExecutionPlan>(
              invalid("incomplete physical placement table"));
        step.backend = (*placements)[result.steps_.size()];
      } else {
        step.backend = options.execution_mode == ExecutionMode::NativeGpu &&
                               node.traits.supports_gpu
                           ? Backend::Gpu
                           : Backend::Cpu;
      }
      values.emplace(step.value, PlanStepInput{result.steps_.size()});
      call_steps[node.id].push_back(result.steps_.size());
      result.steps_.push_back(std::move(step));
    }
  }
  for (const auto& node : optimized.nodes_)
    for (auto index : call_steps.at(node.id))
      for (auto input : node.inputs)
        result.steps_[index].inputs.push_back(values.at(input));
  auto lower_edge = [&](const IREdge& edge) {
    PlanEdge target;
    target.target = blocks.at(edge.target);
    for (auto value : edge.arguments)
      target.arguments.push_back(values.at(value));
    return target;
  };
  for (std::size_t bi = 0; bi < optimized.blocks_.size(); ++bi) {
    const auto& source = optimized.blocks_[bi];
    auto& block = result.blocks_[bi];
    for (auto call : source.calls)
      block.calls.insert(block.calls.end(), call_steps.at(call).begin(),
                         call_steps.at(call).end());
    if (const auto* jump = std::get_if<IRJump>(&source.terminator)) {
      block.terminator = PlanJump{lower_edge(jump->edge)};
    } else if (const auto* branch =
                   std::get_if<IRConditionalJump>(&source.terminator)) {
      block.terminator = PlanConditionalJump{values.at(branch->condition),
                                             lower_edge(branch->if_true),
                                             lower_edge(branch->if_false)};
    } else {
      PlanReturn returned;
      for (auto value : std::get<IRReturn>(source.terminator).values)
        returned.values.push_back(values.at(value));
      block.terminator = std::move(returned);
    }
  }
  for (const auto& output : optimized.outputs_) {
    result.outputs_.emplace(output.name, result.return_signature_.size());
    result.return_signature_.push_back(
        {output.name, output.result_schema, {}, {}});
  }
  auto status = finalize_plan(&result);
  if (!status.ok())
    return Result<ExecutionPlan>(status);
  return Result<ExecutionPlan>(std::move(result));
}

Result<ExecutionPlan> Compiler::plan(const OptimizedGraphIR& optimized,
                                     const PlanningOptions& options) const {
  auto linked = link_plan(optimized, options);
  if (!linked.ok())
    return linked;
  auto result = linked.take_value();
  auto status = expand_plan(&result, options);
  if (status.ok())
    status = validate_plan(result);
  if (!status.ok())
    return Result<ExecutionPlan>(status);
  result.digest_.value = compiler_internal::physical_digest(result);
  result.cache_key_.value =
      compiler_internal::plan_cache_key(result.digest_.value);
  auto work = UINT64_MAX;
  result.structured_templates_ =
      contract_internal::dependency_cache_templates(result, {}, &work, {});
  return Result<ExecutionPlan>(std::move(result));
}

Status Compiler::finalize_plan(ExecutionPlan* destination) const {
  auto& plan = *destination;
  plan.input_tile_grids_.clear();
  for (const auto& input : plan.input_declarations_) {
    auto grids = TileGrid::members(*input.result_schema, plan.tile_extent_);
    if (!grids.ok())
      return grids.status();
    plan.input_tile_grids_.push_back(grids.take_value());
  }
  for (auto& parameter : plan.parameters_) {
    auto grids = TileGrid::members(*parameter.result_schema, plan.tile_extent_);
    if (!grids.ok())
      return grids.status();
    parameter.tile_grids = grids.take_value();
  }
  for (auto& step : plan.steps_) {
    auto grids =
        TileGrid::members(*step.output_result_schema, plan.tile_extent_);
    if (!grids.ok())
      return grids.status();
    step.tile_grids = grids.take_value();
  }
  for (std::size_t i = 0; i < plan.return_signature_.size(); ++i) {
    auto& output = plan.return_signature_[i];
    output.record_index = plan.steps_.size() + i;
    auto grids = TileGrid::members(*output.result_schema, plan.tile_extent_);
    if (!grids.ok())
      return grids.status();
    output.tile_grids = grids.take_value();
  }
  for (const auto& requested : plan.output_requests_)
    if (!plan.outputs_.count(requested.first))
      return invalid("unknown requested output");
  for (const auto& named : plan.outputs_) {
    auto& output = plan.return_signature_[named.second];
    auto found = plan.output_requests_.find(named.first);
    auto resolved = found == plan.output_requests_.end()
                        ? TileRequest::all(output.tile_grids, ResourceBudget{})
                        : found->second.resolve(output.tile_grids);
    if (!resolved.ok())
      return resolved.status();
    plan.output_requests_[named.first] = resolved.take_value();
    auto samples =
        coverage(output.tile_grids, plan.output_requests_.at(named.first));
    if (!samples.ok())
      return samples.status();
    output.requested_samples = samples.take_value();
  }
  for (auto& step : plan.steps_) {
    auto metadata = std::make_shared<ResultProgramMetadata>();
    metadata->output = {step.output_result_schema};
    metadata->output_tile_grids = step.tile_grids;
    for (const auto& input : step.inputs) {
      metadata->inputs.push_back(
          {std::make_shared<const SchemaTemplate>(plan.input_schema(input))});
      metadata->input_tile_grids.push_back(plan.input_grids(input));
    }
    step.structured_metadata = std::move(metadata);
  }
  std::map<IRCallId, std::vector<std::shared_ptr<const PlanStep>>> stages;
  for (auto& step : plan.steps_) {
    auto found = stages.find(step.node_id);
    if (found == stages.end()) {
      std::vector<std::shared_ptr<const PlanStep>> prepared;
      for (const auto& declared : step.prepared->traits().shared_stages) {
        auto stage = std::make_shared<PlanStep>(step);
        stage->shared_stages.clear();
        stage->output_result_schema =
            std::make_shared<const SchemaTemplate>(declared.result_schema);
        auto grids =
            TileGrid::members(declared.result_schema, plan.tile_extent_);
        if (!grids.ok())
          return grids.status();
        stage->tile_grids = grids.take_value();
        stage->backend = declared.backend;
        stage->traits.cacheable = false;
        stage->traits.workspace_bytes = declared.workspace_bytes;
        auto& output = stage->traits.outputs[0];
        output.result_schema = declared.result_schema;
        output.output_schema = {};
        output.output_schema.result_schema_id = declared.result_schema.id;
        output.output_schema.result_schema_version =
            declared.result_schema.version;
        output.input_indices = declared.input_indices;
        output.continuation_bytes = declared.continuation_bytes;
        output.maximum_dependency_stages = declared.maximum_dependency_stages;
        output.maximum_output_payload_bytes =
            declared.maximum_output_payload_bytes;
        auto metadata =
            std::make_shared<ResultProgramMetadata>(*step.structured_metadata);
        metadata->output = {stage->output_result_schema};
        metadata->output_tile_grids = stage->tile_grids;
        stage->structured_metadata = std::move(metadata);
        prepared.push_back(std::move(stage));
      }
      found = stages.emplace(step.node_id, std::move(prepared)).first;
    }
    step.shared_stages = found->second;
  }
  return plan.current()
             ? Status::success()
             : Status{ErrorCode::Stale, "plan changed during linking"};
}

Status Compiler::expand_plan(ExecutionPlan* destination,
                             const PlanningOptions& options) const {
  auto& plan = *destination;
  if (!options.maximum_instructions || !options.maximum_dependency_edges)
    return invalid("static planning budgets must be positive");
  std::map<IRBlockId, std::size_t> block_by_id;
  std::map<IRCallId, std::size_t> primary;
  for (std::size_t bi = 0; bi < plan.blocks_.size(); ++bi) {
    block_by_id.emplace(plan.blocks_[bi].id, bi);
    plan.blocks_[bi].instructions.clear();
  }
  for (std::size_t i = 0; i < plan.steps_.size(); ++i)
    primary.emplace(plan.steps_[i].node_id, i);
  std::map<Unit, unsigned> color;
  std::map<Unit, std::pair<std::size_t, std::size_t>> locations;
  std::uint64_t units = 0, edges = 0;
  auto contract = [&](const Unit& unit) -> const PlanStep& {
    const auto& step = plan.steps_[std::get<0>(unit)];
    return std::get<1>(unit) == UINT32_MAX
               ? step
               : *step.shared_stages.at(std::get<1>(unit));
  };
  auto canonical = [&](Unit unit) {
    if (std::get<1>(unit) != UINT32_MAX) {
      std::get<0>(unit) = primary.at(plan.steps_[std::get<0>(unit)].node_id);
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
        plan.execution_mode_ == ExecutionMode::CpuExact)
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
                            plan.tile_extent_,
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
          closed = certification_closure(plan.steps_[producer->step_index],
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
      const auto& original = plan.steps_[instruction.step_index];
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
      const auto& original = plan.steps_[instruction.step_index];
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
      if (!instruction.shared_inputs.empty())
        return Result<Frame>({ErrorCode::BackendUnavailable,
                              "CPU fallback with shared reads requires "
                              "explicit IR control flow"});
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
          return Result<Frame>({ErrorCode::BackendUnavailable,
                                "CPU fallback with shared reads requires "
                                "explicit IR control flow"});
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
          return Result<Frame>(
              {ErrorCode::BackendUnavailable,
               "CPU fallback needs inputs outside the compiled GPU grants"});
        if (!read.descriptor_only && !read.tiles.empty() &&
            step.prepared->requires_complete_validation(false, read.input)) {
          for (std::uint32_t member = 0; member < grids.size(); ++member) {
            auto all =
                TileSet::range(ResourceBudget{}, 0, grids[member].tile_count());
            if (!all.ok())
              return Result<Frame>(all.status());
            if (!covered({read.input, member, false, all.take_value()}))
              return Result<Frame>(
                  {ErrorCode::BackendUnavailable,
                   "CPU fallback validator exceeds compiled GPU grants"});
          }
        }
      }
      instruction.cpu_fallback = true;
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
        auto bi = block_by_id.at(plan.steps_[std::get<0>(frame.unit)].block);
        auto& instructions = plan.blocks_[bi].instructions;
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
    const auto& step = plan.steps_[source->step_index];
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
  for (const auto& block : plan.blocks_) {
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
      for (const auto& named : plan.outputs_) {
        auto status = root(returned.values[named.second],
                           &plan.output_requests_.at(named.first));
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
    const auto& step = plan.steps_[pair.second];
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
  for (auto& block : plan.blocks_) {
    std::map<IRCallId, std::size_t> order;
    for (std::size_t i = 0; i < block.calls.size(); ++i)
      order.emplace(plan.steps_[block.calls[i]].node_id, i);
    std::vector<std::size_t> permutation(block.instructions.size());
    for (std::size_t i = 0; i < permutation.size(); ++i)
      permutation[i] = i;
    std::stable_sort(
        permutation.begin(), permutation.end(), [&](auto a, auto b) {
          return order.at(
                     plan.steps_[block.instructions[a].step_index].node_id) <
                 order.at(
                     plan.steps_[block.instructions[b].step_index].node_id);
        });
    std::vector<std::size_t> inverse(permutation.size());
    for (std::size_t i = 0; i < permutation.size(); ++i)
      inverse[permutation[i]] = i;
    std::vector<PlanTileInstruction> instructions;
    for (auto old : permutation) {
      auto instruction = std::move(block.instructions[old]);
      for (auto& dependency : instruction.dependencies)
        dependency = inverse[dependency];
      std::sort(instruction.dependencies.begin(),
                instruction.dependencies.end());
      instructions.push_back(std::move(instruction));
    }
    block.instructions = std::move(instructions);
  }
  return Status::success();
}

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
