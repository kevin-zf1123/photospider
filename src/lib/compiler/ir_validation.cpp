#include <algorithm>
#include <functional>
#include <map>
#include <memory>
#include <queue>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "compiler/identity.hpp"
#include "photospider/compiler/compiler.hpp"
#include "plugin/port_validation.hpp"

namespace ps {
namespace {
Status invalid(const char* message) {
  return {ErrorCode::InvalidArgument, message};
}
bool valid_text(const std::string& value, std::size_t maximum) {
  return !value.empty() && value.size() <= maximum &&
         std::none_of(value.begin(), value.end(),
                      [](unsigned char c) { return c < 32 || c == 127; });
}
struct Definition {
  std::size_t block = SIZE_MAX;
  std::size_t position = 0;
  IRCallId call = 0;
  std::shared_ptr<const SchemaTemplate> schema;
};
std::vector<const IREdge*> edges(const IRTerminator& terminator) {
  if (const auto* jump = std::get_if<IRJump>(&terminator))
    return {&jump->edge};
  if (const auto* branch = std::get_if<IRConditionalJump>(&terminator))
    return {&branch->if_true, &branch->if_false};
  return {};
}
bool condition_schema(const SchemaTemplate& schema) {
  if (schema.tensors.size() != 1 ||
      schema.tensors[0].descriptor.element_type != ElementType::UInt8)
    return false;
  auto count = schema.tensors[0].sample_count();
  return count.ok() && count.value() == 1;
}
}  // namespace

Result<ValidatedIR> Compiler::bind_ir(IRProgram program,
                                      ResourceSet resources) const {
  if (program.calls.size() > 65536 || program.blocks.empty() ||
      program.blocks.size() > 65536 ||
      program.input_declarations.size() > 4096 || program.returns.size() > 4096)
    return Result<ValidatedIR>(invalid("IR count bounds are invalid"));
  if (program.form != IRForm::ControlFlow &&
      program.form != IRForm::DependencyPaths)
    return Result<ValidatedIR>(invalid("unknown IR form"));
  const bool paths = program.form == IRForm::DependencyPaths;
  std::size_t occurrences = 0;
  for (std::size_t bi = 0; bi < program.blocks.size(); ++bi) {
    const auto& block = program.blocks[bi];
    if (block.calls.size() > 1048576 - occurrences)
      return Result<ValidatedIR>(
          {ErrorCode::ResourceExhausted, "IR call occurrence limit"});
    occurrences += block.calls.size();
    if (!paths)
      continue;
    if (!block.parameters.empty() || block.calls.empty() ||
        program.entry != program.blocks.front().id)
      return Result<ValidatedIR>(invalid("invalid dependency path block"));
    if (bi + 1 < program.blocks.size()) {
      const auto* jump = std::get_if<IRJump>(&block.terminator);
      if (!jump || !jump->edge.arguments.empty() ||
          jump->edge.target != program.blocks[bi + 1].id)
        return Result<ValidatedIR>(
            invalid("dependency paths require a linear continuation"));
    } else if (!std::holds_alternative<IRReturn>(block.terminator)) {
      return Result<ValidatedIR>(invalid("dependency paths require return"));
    }
  }
  // Freeze mutable frontend owners before validating or preparing any call.
  std::map<const SchemaTemplate*, std::shared_ptr<const SchemaTemplate>> frozen;
  auto freeze = [&](std::shared_ptr<const SchemaTemplate>& schema) {
    if (!schema)
      return;
    auto found = frozen.find(schema.get());
    if (found == frozen.end())
      found = frozen
                  .emplace(schema.get(),
                           std::make_shared<const SchemaTemplate>(*schema))
                  .first;
    schema = found->second;
  };
  for (auto& declaration : program.input_declarations)
    freeze(declaration.result_schema);
  for (auto& block : program.blocks)
    for (auto& parameter : block.parameters)
      freeze(parameter.result_schema);
  for (auto& output : program.returns)
    freeze(output.result_schema);
  auto& declarations = program.input_declarations;
  std::sort(declarations.begin(), declarations.end(),
            [](const auto& a, const auto& b) { return a.id < b.id; });
  std::map<std::uint64_t, std::size_t> declaration_by_id;
  std::set<std::string> names;
  for (std::size_t i = 0; i < declarations.size(); ++i) {
    auto& input = declarations[i];
    if (!input.id || !input_internal::valid_input_name(input.name) ||
        !declaration_by_id.emplace(input.id, i).second ||
        !names.insert(input.name).second)
      return Result<ValidatedIR>(invalid("invalid or duplicate IR input"));
    auto status = input_internal::validate_declaration(&input);
    if (!status.ok())
      return Result<ValidatedIR>(status);
  }
  std::map<IRValueId, Definition> definitions;
  auto define = [&](IRValueId value, Definition definition) {
    return value && definitions.emplace(value, std::move(definition)).second;
  };
  std::sort(program.input_bindings.begin(), program.input_bindings.end(),
            [](const auto& a, const auto& b) {
              return a.declaration_id < b.declaration_id;
            });
  if (program.input_bindings.size() != declarations.size())
    return Result<ValidatedIR>(
        invalid("IR inputs must bind every declaration"));
  for (std::size_t i = 0; i < declarations.size(); ++i) {
    const auto& binding = program.input_bindings[i];
    if (binding.declaration_id != declarations[i].id ||
        !define(binding.value, {SIZE_MAX, 0, 0, declarations[i].result_schema}))
      return Result<ValidatedIR>(
          invalid("duplicate or invalid IR input value"));
  }
  std::map<IRBlockId, std::size_t> block_by_id;
  std::map<IRCallId, std::pair<std::size_t, std::size_t>> location;
  for (std::size_t bi = 0; bi < program.blocks.size(); ++bi) {
    const auto& block = program.blocks[bi];
    if (!block.id || !block_by_id.emplace(block.id, bi).second ||
        block.parameters.size() > 4096 || block.calls.size() > 65536)
      return Result<ValidatedIR>(invalid("invalid IR block"));
    for (const auto& parameter : block.parameters) {
      if (!parameter.result_schema)
        return Result<ValidatedIR>(invalid("block parameter requires schema"));
      auto status = parameter.result_schema->validate();
      if (!status.ok())
        return Result<ValidatedIR>(status);
      if (!define(parameter.value, {bi, 0, 0, parameter.result_schema}))
        return Result<ValidatedIR>(invalid("duplicate block parameter"));
    }
    for (std::size_t ci = 0; ci < block.calls.size(); ++ci)
      if (!location.emplace(block.calls[ci], std::make_pair(bi, ci + 1))
               .second &&
          !paths)
        return Result<ValidatedIR>(invalid("call belongs to multiple blocks"));
  }
  const auto start = block_by_id.find(program.entry);
  if (start == block_by_id.end() ||
      !program.blocks[start->second].parameters.empty())
    return Result<ValidatedIR>(invalid("invalid IR entry block"));
  const auto entry = start->second;
  std::map<IRCallId, const SemanticNode*> calls;
  for (const auto& call : program.calls) {
    if (!call.id || !valid_text(call.operation, 1024) ||
        !calls.emplace(call.id, &call).second || !location.count(call.id) ||
        call.inputs.size() > 1024 || call.parameters.size() > 1024)
      return Result<ValidatedIR>(invalid("invalid or unplaced IR call"));
    for (const auto& parameter : call.parameters)
      if (!valid_text(parameter.first, 1024) ||
          (std::holds_alternative<std::string>(parameter.second) &&
           std::get<std::string>(parameter.second).size() > 8192))
        return Result<ValidatedIR>(invalid("malformed IR parameter"));
    auto traits = operations_->find_traits(call.operation);
    if (!traits.ok())
      return Result<ValidatedIR>(traits.status());
    const auto& contract = traits.value();
    const auto count = call.inputs.size();
    const bool repeated =
        contract.repeated_maximum && !contract.repeated_resolved;
    if (count < contract.input_count ||
        (repeated ? count - contract.input_count < contract.repeated_minimum ||
                        count - contract.input_count > contract.repeated_maximum
                  : count != contract.input_count))
      return Result<ValidatedIR>(
          {ErrorCode::TypeMismatch,
           "IR call input count mismatch",
           FailureReason::None,
           {FailureOrigin::Schema, FailureScope::Group}});
    if (call.outputs.size() != traits.value().outputs.size())
      return Result<ValidatedIR>(invalid("IR call result count mismatch"));
    auto at = location.at(call.id);
    for (std::size_t oi = 0; oi < call.outputs.size(); ++oi)
      if (call.outputs[oi].key != traits.value().outputs[oi].key ||
          !define(call.outputs[oi].value, {at.first, at.second, call.id, {}}))
        return Result<ValidatedIR>(invalid("invalid or duplicate call result"));
  }
  if (location.size() != calls.size())
    return Result<ValidatedIR>(invalid("block references a missing call"));
  std::vector<std::vector<std::size_t>> successors(program.blocks.size());
  std::vector<std::vector<std::size_t>> predecessors(program.blocks.size());
  for (std::size_t bi = 0; bi < program.blocks.size(); ++bi) {
    for (const auto* edge : edges(program.blocks[bi].terminator)) {
      auto target = block_by_id.find(edge->target);
      if (target == block_by_id.end() ||
          edge->arguments.size() !=
              program.blocks[target->second].parameters.size())
        return Result<ValidatedIR>(invalid("invalid IR edge target/arity"));
      successors[bi].push_back(target->second);
      predecessors[target->second].push_back(bi);
    }
  }
  // Iterative DFS avoids using the C++ stack for long source graphs.
  std::vector<bool> seen(program.blocks.size());
  std::vector<std::size_t> postorder;
  std::vector<std::pair<std::size_t, std::size_t>> stack{{entry, 0}};
  seen[entry] = true;
  while (!stack.empty()) {
    auto& frame = stack.back();
    if (frame.second == successors[frame.first].size()) {
      postorder.push_back(frame.first);
      stack.pop_back();
    } else {
      auto next = successors[frame.first][frame.second++];
      if (!seen[next]) {
        seen[next] = true;
        stack.push_back({next, 0});
      }
    }
  }
  if (postorder.size() != program.blocks.size())
    return Result<ValidatedIR>(invalid("IR contains unreachable blocks"));
  std::reverse(postorder.begin(), postorder.end());
  std::vector<std::size_t> rank(program.blocks.size()),
      idom(program.blocks.size(), SIZE_MAX);
  for (std::size_t i = 0; i < postorder.size(); ++i)
    rank[postorder[i]] = i;
  idom[entry] = entry;
  auto intersect = [&](std::size_t a, std::size_t b) {
    while (a != b) {
      while (rank[a] > rank[b])
        a = idom[a];
      while (rank[b] > rank[a])
        b = idom[b];
    }
    return a;
  };
  bool changed = true;
  while (changed) {
    changed = false;
    for (auto block : postorder) {
      if (block == entry)
        continue;
      std::size_t parent = SIZE_MAX;
      for (auto predecessor : predecessors[block])
        if (idom[predecessor] != SIZE_MAX)
          parent =
              parent == SIZE_MAX ? predecessor : intersect(parent, predecessor);
      if (idom[block] != parent) {
        idom[block] = parent;
        changed = true;
      }
    }
  }
  auto dominates = [&](std::size_t a, std::size_t b) {
    if (paths)
      return a <= b;  // Raw paths have a validated linear block order.
    while (b != a && b != entry)
      b = idom[b];
    return b == a;
  };
  auto check_use = [&](IRValueId value, std::size_t block,
                       std::size_t position) {
    auto found = definitions.find(value);
    if (found == definitions.end())
      return false;
    const auto& def = found->second;
    if (def.block == SIZE_MAX)
      return true;
    if (def.block == block)
      return def.position < position;
    return dominates(def.block, block);
  };
  for (std::size_t bi = 0; bi < program.blocks.size(); ++bi) {
    const auto& block = program.blocks[bi];
    for (std::size_t ci = 0; ci < block.calls.size(); ++ci)
      for (auto value : calls.at(block.calls[ci])->inputs)
        if (!check_use(value, bi, ci + 1))
          return Result<ValidatedIR>(
              invalid("SSA definition does not dominate call"));
    for (const auto* edge : edges(block.terminator))
      for (auto value : edge->arguments)
        if (!check_use(value, bi, block.calls.size() + 1))
          return Result<ValidatedIR>(
              invalid("SSA definition does not dominate edge"));
    if (const auto* branch =
            std::get_if<IRConditionalJump>(&block.terminator)) {
      if (!check_use(branch->condition, bi, block.calls.size() + 1))
        return Result<ValidatedIR>(invalid("undefined branch condition"));
    }
    if (const auto* returned = std::get_if<IRReturn>(&block.terminator)) {
      if (returned->values.size() != program.returns.size())
        return Result<ValidatedIR>(invalid("IR return arity mismatch"));
      for (auto value : returned->values)
        if (!check_use(value, bi, block.calls.size() + 1))
          return Result<ValidatedIR>(
              invalid("SSA definition does not dominate return"));
    }
  }
  std::map<IRCallId, std::size_t> indegree;
  std::map<IRCallId, std::vector<IRCallId>> dependents;
  for (const auto& call : program.calls) {
    indegree.emplace(call.id, 0);
    for (auto value : call.inputs) {
      const auto producer = definitions.at(value).call;
      if (producer) {
        ++indegree[call.id];
        dependents[producer].push_back(call.id);
      }
    }
  }
  std::priority_queue<IRCallId, std::vector<IRCallId>, std::greater<IRCallId>>
      ready;
  for (const auto& pair : indegree)
    if (!pair.second)
      ready.push(pair.first);
  ValidatedIR semantic;
  semantic.revision_ = 1;
  while (!ready.empty()) {
    auto id = ready.top();
    ready.pop();
    auto node = *calls.at(id);
    std::vector<OperationMetadata> input_metadata;
    for (auto value : node.inputs)
      input_metadata.push_back({definitions.at(value).schema});
    auto prepared = operations_->prepare_operation(
        node.operation, input_metadata, node.parameters, resources);
    if (!prepared.ok())
      return Result<ValidatedIR>(prepared.status());
    node.prepared = prepared.take_value();
    node.traits = node.prepared->traits();
    auto inferred =
        infer_operation_outputs(node.traits, input_metadata, node.parameters);
    if (!inferred.ok())
      return Result<ValidatedIR>(inferred.status());
    for (std::size_t oi = 0; oi < node.outputs.size(); ++oi) {
      auto schema = inferred.value()[oi].result_schema;
      if (node.outputs[oi].result_schema &&
          !node.outputs[oi].result_schema->same_schema(*schema))
        return Result<ValidatedIR>(
            {ErrorCode::TypeMismatch, "IR result schema mismatch"});
      node.outputs[oi].result_schema = schema;
      definitions.at(node.outputs[oi].value).schema = std::move(schema);
    }
    auto selected = resources.select(node.prepared->resource_ids());
    if (!selected.ok())
      return Result<ValidatedIR>(selected.status());
    auto joined = semantic.resources_.unite(selected.value());
    if (!joined.ok())
      return Result<ValidatedIR>(joined.status());
    semantic.resources_ = joined.take_value();
    semantic.nodes_.push_back(std::move(node));
    for (auto dependent : dependents[id])
      if (--indegree[dependent] == 0)
        ready.push(dependent);
  }
  if (semantic.nodes_.size() != calls.size())
    return Result<ValidatedIR>(
        {ErrorCode::Cycle,
         "recursive SSA definition requires block parameters"});
  names.clear();
  for (auto& output : program.returns) {
    if (!valid_text(output.name, 1024) || !names.insert(output.name).second)
      return Result<ValidatedIR>(invalid("invalid IR return name"));
    if (output.result_schema) {
      auto status = output.result_schema->validate();
      if (!status.ok())
        return Result<ValidatedIR>(status);
    }
  }
  for (const auto& block : program.blocks) {
    for (const auto* edge : edges(block.terminator)) {
      const auto& parameters =
          program.blocks[block_by_id.at(edge->target)].parameters;
      for (std::size_t i = 0; i < parameters.size(); ++i)
        if (!definitions.at(edge->arguments[i])
                 .schema->same_schema(*parameters[i].result_schema))
          return Result<ValidatedIR>(
              {ErrorCode::TypeMismatch, "IR edge schema mismatch"});
    }
    if (const auto* branch = std::get_if<IRConditionalJump>(&block.terminator))
      if (!condition_schema(*definitions.at(branch->condition).schema))
        return Result<ValidatedIR>(
            {ErrorCode::TypeMismatch, "branch requires one UInt8 sample"});
    if (const auto* returned = std::get_if<IRReturn>(&block.terminator)) {
      for (std::size_t i = 0; i < returned->values.size(); ++i) {
        auto schema = definitions.at(returned->values[i]).schema;
        auto& expected = program.returns[i].result_schema;
        if (!expected)
          expected = schema;
        if (!expected->same_schema(*schema))
          return Result<ValidatedIR>(
              {ErrorCode::TypeMismatch, "IR return schema mismatch"});
      }
    }
  }
  for (const auto& output : program.returns)
    if (!output.result_schema)
      return Result<ValidatedIR>(invalid("return signature requires schema"));
  semantic.input_declarations_ = std::move(program.input_declarations);
  semantic.input_bindings_ = std::move(program.input_bindings);
  semantic.blocks_ = std::move(program.blocks);
  semantic.form_ = program.form;
  semantic.entry_ = program.entry;
  semantic.outputs_ = std::move(program.returns);
  semantic.operation_registry_ = operations_;
  semantic.current_check_ =
      [registry = std::weak_ptr<OperationRegistry>(operations_)]() noexcept {
        return !registry.expired();
      };
  semantic.provenance_ = {{}, "ir-validator", "1", {}};
  semantic.digest_.value = compiler_internal::ir_digest(semantic.program());
  return Result<ValidatedIR>(std::move(semantic));
}

}  // namespace ps
