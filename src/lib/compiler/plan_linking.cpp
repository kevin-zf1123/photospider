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
    const ValidatedIR& ir, const PlanningOptions& options,
    const std::vector<Backend>* placements) const {
  if (!ir.current() || !ir.revision() ||
      ir.operation_registry_.lock().get() != operations_.get())
    return Result<ExecutionPlan>({ErrorCode::Stale, "invalid validated IR"});
  if ((options.execution_mode != ExecutionMode::CpuExact &&
       options.execution_mode != ExecutionMode::NativeGpu) ||
      !options.tile_extent || (options.tile_extent & (options.tile_extent - 1)))
    return Result<ExecutionPlan>(
        invalid("invalid planning mode or tile extent"));
  auto lowered_blocks =
      compiler_internal::lower_dependency_paths(ir.blocks_, ir.form_);
  ExecutionPlan result;
  result.linked_program_ = ir.program();
  result.linked_program_.blocks = lowered_blocks;
  result.linked_program_.entry = ir.form_ == IRForm::DependencyPaths
                                     ? lowered_blocks.front().id
                                     : ir.entry_;
  result.linked_program_.form = IRForm::ControlFlow;
  result.revision_ = ir.revision_;
  result.input_declarations_ = ir.input_declarations_;
  result.resources_ = ir.resources_;
  result.input_ir_digest_ = ir.digest_;
  result.operation_registry_ = operations_;
  result.current_check_ = ir.current_check_;
  result.execution_mode_ = options.execution_mode;
  result.tile_extent_ = options.tile_extent;
  result.output_requests_ = options.output_requests;
  std::map<IRValueId, PlanInput> values;
  std::map<IRBlockId, std::size_t> blocks;
  std::map<IRCallId, IRBlockId> call_blocks;
  std::map<IRCallId, std::vector<std::size_t>> call_steps;
  for (std::size_t i = 0; i < ir.input_bindings_.size(); ++i)
    values.emplace(ir.input_bindings_[i].value, PlanWorkflowInput{i});
  for (const auto& block : lowered_blocks) {
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
  result.entry_ = blocks.at(result.linked_program_.entry);
  for (const auto& node : ir.nodes_) {
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
  for (const auto& node : ir.nodes_)
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
  for (std::size_t bi = 0; bi < lowered_blocks.size(); ++bi) {
    const auto& source = lowered_blocks[bi];
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
  for (const auto& output : ir.outputs_) {
    result.outputs_.emplace(output.name, result.return_signature_.size());
    result.return_signature_.push_back(
        {output.name, output.result_schema, {}, {}});
  }
  if (options.output_names) {
    std::map<std::string, std::size_t> selected;
    for (const auto& name : *options.output_names) {
      auto found = result.outputs_.find(name);
      if (found == result.outputs_.end() || !selected.emplace(*found).second)
        return Result<ExecutionPlan>(
            invalid("unknown or repeated selected output"));
    }
    result.outputs_ = std::move(selected);
  }
  auto status = finalize_plan(&result);
  if (!status.ok())
    return Result<ExecutionPlan>(status);
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

}  // namespace ps
