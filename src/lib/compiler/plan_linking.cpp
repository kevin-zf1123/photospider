#include <algorithm>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <utility>

#include "compiler/dependency_paths.hpp"
#include "photospider/compiler/compiler.hpp"

namespace ps {
Result<ExecutionPlan> Compiler::link_plan(
    const ValidatedIR& ir, const PlanningOptions& options) const {
  using Answer = Result<ExecutionPlan>;
  if (!ir.current() || !ir.revision() ||
      ir.operation_registry_.lock().get() != operations_.get())
    return Answer({ErrorCode::Stale, "invalid planner input IR"});
  if ((options.execution_mode != ExecutionMode::CpuExact &&
       options.execution_mode != ExecutionMode::NativeGpu) ||
      !options.tile_extent || (options.tile_extent & (options.tile_extent - 1)))
    return Answer(
        {ErrorCode::InvalidArgument, "invalid planning mode or tile extent"});
  ExecutionPlan result;
  result.edit().revision_ = ir.revision_;
  result.edit().current_check_ = ir.current_check_;
  result.edit().operation_registry_ = operations_;
  result.edit().input_ir_ = ir.digest_;
  result.edit().resources_ = ir.resources_;
  result.edit().inputs_ = ir.input_declarations_;
  result.edit().mode_ = options.execution_mode;
  result.edit().limits_ = {options.maximum_records,
                           options.maximum_dependency_edges,
                           options.maximum_work};
  auto blocks = compiler_internal::lower_dependency_paths(ir.blocks_, ir.form_);
  result.edit().source_program_ = ir.program();
  result.edit().source_program_.blocks = blocks;
  result.edit().source_program_.form = IRForm::ControlFlow;
  result.edit().source_program_.entry =
      ir.form_ == IRForm::DependencyPaths ? blocks.front().id : ir.entry_;
  auto& p = result.edit().program_;
  p.tile_extent = options.tile_extent;
  std::map<IRValueId, physical::Id> values;
  std::map<IRBlockId, physical::Id> block_ids;
  std::map<IRCallId, const SemanticNode*> nodes;
  for (const auto& node : ir.nodes_)
    nodes.emplace(node.id, &node);
  auto schema = [&](std::shared_ptr<const SchemaTemplate> type) {
    auto index = static_cast<physical::Id>(p.schemas.size());
    p.schemas.push_back(std::move(type));
    return index;
  };
  auto value = [&](IRValueId source, std::shared_ptr<const SchemaTemplate> type,
                   physical::ValueKind kind, physical::Id owner) {
    physical::ValueDesc v;
    v.source_value = source;
    v.schema = schema(std::move(type));
    v.kind = kind;
    v.owner = owner;
    auto index = static_cast<physical::Id>(p.values.size());
    values.emplace(source, index);
    p.values.push_back(std::move(v));
    return index;
  };
  for (const auto& contract : options.input_layouts) {
    if (static_cast<unsigned>(contract.second) > 1 ||
        std::none_of(
            result.edit().inputs_.begin(), result.edit().inputs_.end(),
            [&](const auto& input) { return input.name == contract.first; }))
      return Answer(
          {ErrorCode::InvalidArgument, "invalid input layout contract"});
  }
  for (const auto& contract : options.input_domains) {
    const auto layout = options.input_layouts.find(contract.first);
    if (static_cast<unsigned>(contract.second) > 2 ||
        (contract.second != physical::Domain::Host &&
         layout != options.input_layouts.end() &&
         layout->second == physical::InputLayout::DenseHost) ||
        std::none_of(
            result.edit().inputs_.begin(), result.edit().inputs_.end(),
            [&](const auto& input) { return input.name == contract.first; }))
      return Answer(
          {ErrorCode::InvalidArgument, "invalid input memory domain contract"});
  }
  for (physical::Id i = 0; i < ir.input_bindings_.size(); ++i) {
    p.inputs.push_back(value(ir.input_bindings_[i].value,
                             ir.input_declarations_[i].result_schema,
                             physical::ValueKind::Input, i));
    auto layout = options.input_layouts.find(result.edit().inputs_[i].name);
    if (layout != options.input_layouts.end())
      p.values[p.inputs.back()].input_layout = layout->second;
    auto grids = TileGrid::members(*ir.input_declarations_[i].result_schema,
                                   options.tile_extent);
    if (!grids.ok())
      return Answer(grids.status());
    result.edit().input_grids_.push_back(grids.take_value());
  }
  for (physical::Id bi = 0; bi < blocks.size(); ++bi) {
    block_ids.emplace(blocks[bi].id, bi);
    physical::BlockDesc block;
    block.source_block = blocks[bi].id;
    for (const auto& parameter : blocks[bi].parameters)
      block.arguments.push_back(value(parameter.value, parameter.result_schema,
                                      physical::ValueKind::Argument, bi));
    for (auto call_id : blocks[bi].calls) {
      const auto& node = *nodes.at(call_id);
      auto ci = static_cast<physical::Id>(p.calls.size());
      physical::CallDesc call;
      call.source_call = node.id;
      call.block = bi;
      call.ordinal = static_cast<physical::Id>(block.calls.size());
      block.calls.push_back(ci);
      p.calls.push_back(std::move(call));
      PlanCallBinding binding;
      binding.operation = node.operation;
      binding.prepared = node.prepared;
      binding.parameters = node.parameters;
      for (const auto& output : node.outputs)
        binding.outputs.push_back(value(output.value, output.result_schema,
                                        physical::ValueKind::Produced, ci));
      result.edit().calls_.push_back(std::move(binding));
    }
    p.blocks.push_back(std::move(block));
  }
  p.entry = block_ids.at(ir.form_ == IRForm::DependencyPaths ? blocks.front().id
                                                             : ir.entry_);
  for (physical::Id ci = 0; ci < p.calls.size(); ++ci)
    for (auto input : nodes.at(p.calls[ci].source_call)->inputs)
      result.edit().calls_[ci].inputs.push_back(values.at(input));
  auto edge = [&](const IREdge& source) {
    physical::EdgeDesc e;
    e.target = block_ids.at(source.target);
    for (auto argument : source.arguments)
      e.arguments.push_back(values.at(argument));
    return e;
  };
  for (physical::Id bi = 0; bi < blocks.size(); ++bi) {
    auto& block = p.blocks[bi];
    if (const auto* jump = std::get_if<IRJump>(&blocks[bi].terminator)) {
      block.end = physical::EndKind::Jump;
      block.edges = {edge(jump->edge)};
    } else if (const auto* branch =
                   std::get_if<IRConditionalJump>(&blocks[bi].terminator)) {
      block.end = physical::EndKind::Branch;
      block.condition = values.at(branch->condition);
      block.edges = {edge(branch->if_true), edge(branch->if_false)};
    } else {
      block.end = physical::EndKind::Return;
      for (auto vi : std::get<IRReturn>(blocks[bi].terminator).values)
        block.return_values.push_back(values.at(vi));
    }
  }
  for (const auto& declaration : ir.outputs_) {
    result.edit().outputs_.emplace(declaration.name,
                                   result.edit().returns_.size());
    auto grids =
        TileGrid::members(*declaration.result_schema, options.tile_extent);
    if (!grids.ok())
      return Answer(grids.status());
    result.edit().returns_.push_back(
        {declaration.name, declaration.result_schema, grids.take_value()});
    p.return_schemas.push_back(schema(declaration.result_schema));
  }
  if (options.output_names) {
    std::map<std::string, std::size_t> selected;
    for (const auto& name : *options.output_names) {
      auto found = result.edit().outputs_.find(name);
      if (found == result.edit().outputs_.end() ||
          !selected.emplace(*found).second)
        return Answer(
            {ErrorCode::InvalidArgument, "unknown or repeated output name"});
    }
    result.edit().outputs_ = std::move(selected);
  }
  for (const auto& requested : options.output_requests)
    if (!result.edit().outputs_.count(requested.first))
      return Answer({ErrorCode::InvalidArgument, "unknown requested output"});
  for (const auto& named : result.edit().outputs_) {
    auto found = options.output_requests.find(named.first);
    const auto& grids = result.edit().returns_[named.second].tile_grids;
    auto request = found == options.output_requests.end()
                       ? TileRequest::all(grids, ResourceBudget{})
                       : found->second.resolve(grids);
    if (!request.ok())
      return Answer(request.status());
    result.edit().requests_.emplace(named.first, request.take_value());
  }
  return Answer(std::move(result));
}
}  // namespace ps
