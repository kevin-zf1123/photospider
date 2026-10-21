#include <algorithm>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "photospider/compiler/compiler.hpp"

namespace ps {
namespace {
Status invalid(const char* text) {
  return {ErrorCode::InvalidArgument, text};
}
bool same_parameters(const std::map<std::string, ParameterValue>& a,
                     const std::map<std::string, ParameterValue>& b) {
  if (a.size() != b.size())
    return false;
  auto right = b.begin();
  for (const auto& left : a) {
    if (left.first != right->first ||
        left.second.index() != right->second.index())
      return false;
    if (const auto* number = std::get_if<double>(&left.second)) {
      const double other = std::get<double>(right->second);
      if (std::memcmp(number, &other, sizeof(other)))
        return false;
    } else if (left.second != right->second) {
      return false;
    }
    ++right;
  }
  return true;
}
}  // namespace
Status Compiler::finalize_plan(ExecutionPlan* plan) const {
  auto status =
      physical::finalize_program(&plan->edit().program_, plan->edit().limits_);
  return status.ok() ? validate_plan(*plan) : status;
}
Status Compiler::validate_plan(const ExecutionPlan& plan) const {
  const auto& p = plan.data().program_;
  auto status = physical::validate_program(p, plan.data().limits_);
  if (!status.ok())
    return status;
  const auto& ir = plan.data().source_program_;
  if (p.calls.size() != plan.data().calls_.size() ||
      p.calls.size() != ir.calls.size() ||
      p.blocks.size() != ir.blocks.size() ||
      p.inputs.size() != plan.data().inputs_.size() ||
      p.return_schemas.size() != plan.data().returns_.size())
    return invalid("physical and semantic signature mismatch");
  std::map<IRCallId, const SemanticNode*> nodes;
  for (const auto& node : ir.calls)
    nodes.emplace(node.id, &node);
  std::map<IRBlockId, const IRBlock*> blocks;
  for (const auto& block : ir.blocks)
    blocks.emplace(block.id, &block);
  if (p.blocks[p.entry].source_block != ir.entry)
    return invalid("physical entry changed");
  std::map<IRValueId, physical::Id> canonical_values;
  auto bind_value = [&](IRValueId source, physical::Id vi) {
    return vi < p.values.size() && p.values[vi].source_value == source &&
           canonical_values.emplace(source, vi).second;
  };
  if (ir.input_bindings.size() != p.inputs.size() ||
      ir.returns.size() != plan.data().returns_.size())
    return invalid("physical input or return signature changed");
  for (std::size_t i = 0; i < p.inputs.size(); ++i)
    if (!bind_value(ir.input_bindings[i].value, p.inputs[i]))
      return invalid("duplicate physical source value");
  for (const auto& block : p.blocks) {
    auto found = blocks.find(block.source_block);
    if (found == blocks.end() ||
        block.arguments.size() != found->second->parameters.size())
      return invalid("physical block parameter signature changed");
    for (std::size_t i = 0; i < block.arguments.size(); ++i)
      if (!bind_value(found->second->parameters[i].value, block.arguments[i]))
        return invalid("duplicate physical source parameter");
  }
  for (std::size_t ci = 0; ci < p.calls.size(); ++ci) {
    auto found = nodes.find(p.calls[ci].source_call);
    if (found == nodes.end() ||
        plan.data().calls_[ci].outputs.size() != found->second->outputs.size())
      return invalid("physical output signature changed");
    for (std::size_t i = 0; i < plan.data().calls_[ci].outputs.size(); ++i)
      if (!bind_value(found->second->outputs[i].value,
                      plan.data().calls_[ci].outputs[i]))
        return invalid("duplicate physical source output");
  }
  for (physical::Id vi = 0; vi < p.values.size(); ++vi) {
    auto source = p.values[vi].source_value;
    if (source &&
        (!canonical_values.count(source) || canonical_values.at(source) != vi))
      return invalid("private value impersonates an IR definition");
  }
  for (std::size_t i = 0; i < plan.data().returns_.size(); ++i)
    if (plan.data().returns_[i].name != ir.returns[i].name ||
        !plan.data().returns_[i].result_schema->same_schema(
            *ir.returns[i].result_schema) ||
        !p.schemas[p.return_schemas[i]]->same_schema(
            *ir.returns[i].result_schema))
      return invalid("physical return schema changed");
  auto canonical = [&](physical::Id vi) {
    return p.values[vi].kind == physical::ValueKind::Imported
               ? p.values[vi].import_source
               : vi;
  };
  auto source = [&](physical::Id vi) {
    return p.values[canonical(vi)].source_value;
  };
  for (std::size_t i = 0; i < plan.data().inputs_.size(); ++i)
    if (source(p.inputs[i]) != ir.input_bindings[i].value ||
        !p.schemas[p.values[p.inputs[i]].schema]->same_schema(
            *plan.data().inputs_[i].result_schema))
      return invalid("physical input binding changed");
  for (const auto& task : p.tasks)
    if (plan.data().mode_ == ExecutionMode::CpuExact &&
        p.placements[task.placement].backend != physical::Backend::Cpu)
      return invalid("physical task has unsupported execution mode");
  for (physical::Id ci = 0; ci < p.calls.size(); ++ci) {
    auto found = nodes.find(p.calls[ci].source_call);
    if (found == nodes.end())
      return invalid("physical call is absent from source IR");
    const auto& node = *found->second;
    const auto& binding = plan.data().calls_[ci];
    if (!binding.prepared || binding.prepared != node.prepared ||
        !same_parameters(binding.parameters, node.parameters) ||
        binding.operation != node.operation ||
        binding.inputs.size() != node.inputs.size() ||
        binding.outputs.size() != node.outputs.size())
      return invalid("physical call binding mismatch");
    for (std::size_t i = 0; i < node.inputs.size(); ++i)
      if (binding.inputs[i] >= p.values.size() ||
          binding.inputs[i] != canonical_values.at(node.inputs[i]))
        return invalid("physical call input changed");
    for (std::size_t i = 0; i < node.outputs.size(); ++i) {
      auto vi = binding.outputs[i];
      if (vi >= p.values.size() || source(vi) != node.outputs[i].value ||
          p.values[vi].kind != physical::ValueKind::Produced ||
          p.values[vi].owner != ci ||
          !p.schemas[p.values[vi].schema]->same_schema(
              *node.outputs[i].result_schema))
        return invalid("physical call output changed");
    }
    if ((p.calls[ci].effect_task != physical::none) ==
        binding.prepared->traits().side_effect_free)
      return invalid("physical call effect contract mismatch");
    for (auto ti : p.calls[ci].tasks) {
      const auto& task = p.tasks[ti];
      if (task.kind != physical::TaskKind::Copy &&
          p.kernels[task.kernel].operation != binding.operation)
        return invalid("physical task uses a foreign operation");
      auto backend = p.placements[task.placement].backend;
      if (task.kind != physical::TaskKind::Copy &&
          ((backend == physical::Backend::Cpu &&
            !binding.prepared->traits().supports_cpu) ||
           (backend == physical::Backend::Gpu &&
            !binding.prepared->traits().supports_gpu)))
        return invalid("physical task has unsupported placement");
      for (const auto& read : task.reads) {
        const auto vi = p.tiles[read.tile].value;
        const auto& value = p.values[vi];
        if (value.kind == physical::ValueKind::Produced && value.owner == ci)
          continue;
        if (read.port >= binding.inputs.size() ||
            binding.inputs[read.port] != canonical(vi))
          return invalid(
              "physical task read does not bind its declared input port");
      }
    }
  }
  for (const auto& block : p.blocks) {
    auto found = blocks.find(block.source_block);
    if (found == blocks.end())
      return invalid("physical block absent from IR");
    const auto& original = *found->second;
    if (block.calls.size() != original.calls.size() ||
        block.arguments.size() != original.parameters.size())
      return invalid("physical block signature changed");
    for (std::size_t i = 0; i < block.calls.size(); ++i)
      if (p.calls[block.calls[i]].source_call != original.calls[i])
        return invalid("physical call retirement order changed");
    for (std::size_t i = 0; i < block.arguments.size(); ++i)
      if (source(block.arguments[i]) != original.parameters[i].value ||
          !p.schemas[p.values[block.arguments[i]].schema]->same_schema(
              *original.parameters[i].result_schema))
        return invalid("physical block parameter changed");
    std::vector<const IREdge*> edges;
    if (const auto* jump = std::get_if<IRJump>(&original.terminator)) {
      if (block.end != physical::EndKind::Jump)
        return invalid("physical terminator changed");
      edges = {&jump->edge};
    } else if (const auto* branch =
                   std::get_if<IRConditionalJump>(&original.terminator)) {
      if (block.end != physical::EndKind::Branch ||
          canonical(block.condition) != canonical_values.at(branch->condition))
        return invalid("physical branch changed");
      edges = {&branch->if_true, &branch->if_false};
    } else {
      const auto& returned = std::get<IRReturn>(original.terminator);
      if (block.end != physical::EndKind::Return ||
          block.return_values.size() != returned.values.size())
        return invalid("physical return changed");
      for (std::size_t i = 0; i < returned.values.size(); ++i)
        if (canonical(block.return_values[i]) !=
            canonical_values.at(returned.values[i]))
          return invalid("physical return value changed");
    }
    if (edges.size() != block.edges.size())
      return invalid("physical edge count changed");
    for (std::size_t i = 0; i < edges.size(); ++i) {
      const auto& edge = block.edges[i];
      if (p.blocks[edge.target].source_block != edges[i]->target ||
          edge.arguments.size() != edges[i]->arguments.size())
        return invalid("physical edge changed");
      for (std::size_t j = 0; j < edge.arguments.size(); ++j)
        if (canonical(edge.arguments[j]) !=
            canonical_values.at(edges[i]->arguments[j]))
          return invalid("physical edge argument changed");
    }
    if (block.end == physical::EndKind::Return) {
      for (const auto& output : plan.data().outputs_) {
        if (output.second >= block.return_values.size() ||
            !plan.data().requests_.count(output.first))
          return invalid("invalid physical output selection");
        const auto vi = block.return_values[output.second];
        std::map<std::uint32_t, std::vector<TileIndex>> members;
        for (auto ti : p.values[vi].tiles)
          members[p.tiles[ti].member].push_back(p.tiles[ti].tile_index);
        for (const auto& request :
             plan.data().requests_.at(output.first).members) {
          auto present =
              TileSet::from_indices(ResourceBudget{}, members[request.first]);
          if (!present.ok())
            return present.status();
          if (!present.value().contains(request.second))
            return invalid("selected return tile is absent");
        }
      }
    }
  }
  return Status::success();
}
}  // namespace ps
