#include "photospider/compiler/compiler.hpp"

#include <algorithm>
#include <chrono>
#include <functional>
#include <map>
#include <memory>
#include <queue>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "compiler/identity.hpp"
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
std::uint64_t elapsed(std::chrono::steady_clock::time_point start,
                      std::chrono::steady_clock::time_point end) {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::microseconds>(end - start)
          .count());
}
}  // namespace

Compiler::Compiler(std::shared_ptr<OperationRegistry> operations)
    : operations_(std::move(operations)) {
  if (!operations_ || !operations_->frozen())
    throw std::invalid_argument(
        "Compiler requires a frozen operation registry");
}

Result<SemanticGraphIR> Compiler::analyze(const GraphSnapshot& snapshot,
                                          ResourceSet resources) const {
  if (!snapshot.current())
    return Result<SemanticGraphIR>(
        {ErrorCode::Stale, "graph snapshot is stale"});
  const auto& document = snapshot.document();
  if (document.schema_version != 7 || document.nodes.size() > 65536 ||
      document.inputs.size() > 4096 || document.outputs.size() > 4096 ||
      document.outputs.empty())
    return Result<SemanticGraphIR>(invalid("unsupported DAG version/counts"));
  IRProgram program;
  program.input_declarations = document.inputs;
  program.entry = 1;
  IRBlock entry;
  entry.id = 1;
  IRValueId next_value = 1;
  std::map<std::uint64_t, IRValueId> external;
  auto declarations = document.inputs;
  std::sort(declarations.begin(), declarations.end(),
            [](const auto& a, const auto& b) { return a.id < b.id; });
  for (const auto& input : declarations) {
    if (!external.emplace(input.id, next_value).second)
      return Result<SemanticGraphIR>(invalid("duplicate DAG input"));
    program.input_bindings.push_back({input.id, next_value++});
  }
  std::map<IRCallId, const WorkflowNode*> sources;
  std::map<std::pair<IRCallId, std::string>, IRValueId> values;
  for (const auto& node : document.nodes)
    if (!node.id || !sources.emplace(node.id, &node).second)
      return Result<SemanticGraphIR>(invalid("duplicate or zero DAG node"));
  for (const auto& pair : sources) {
    const auto& node = *pair.second;
    auto traits = operations_->find_traits(node.operation);
    if (!traits.ok())
      return Result<SemanticGraphIR>(traits.status());
    SemanticNode call;
    call.id = node.id;
    call.operation = node.operation;
    call.parameters = node.parameters;
    for (const auto& output : traits.value().outputs) {
      values.emplace(std::make_pair(node.id, output.key), next_value);
      call.outputs.push_back({output.key, {}, next_value++});
    }
    program.calls.push_back(std::move(call));
  }
  for (auto& call : program.calls) {
    for (const auto& input : sources.at(call.id)->inputs) {
      if (const auto* source = std::get_if<WorkflowNodeOutput>(&input)) {
        auto found = values.find({source->source_node, source->source_port});
        if (found == values.end())
          return Result<SemanticGraphIR>(
              {ErrorCode::NotFound, "DAG references a missing output"});
        call.inputs.push_back(found->second);
      } else {
        auto found =
            external.find(std::get<WorkflowInputReference>(input).input_id);
        if (found == external.end())
          return Result<SemanticGraphIR>(
              {ErrorCode::NotFound, "DAG references a missing input"});
        call.inputs.push_back(found->second);
      }
    }
  }
  auto outputs = document.outputs;
  std::sort(outputs.begin(), outputs.end(),
            [](const auto& a, const auto& b) { return a.name < b.name; });
  IRReturn returned;
  std::vector<IRCallId> roots;
  for (const auto& output : outputs) {
    auto found = values.find({output.node_id, output.port});
    if (found == values.end())
      return Result<SemanticGraphIR>(
          {ErrorCode::NotFound, "DAG return references a missing output"});
    program.returns.push_back({output.name, {}});
    returned.values.push_back(found->second);
    roots.push_back(output.node_id);
  }
  // Preserve every frontend call in raw IR. Starting at named results gives
  // input-ordered depth-first lowering, while subsequent roots retain unused
  // calls and effects without cloning shared invocations.
  for (const auto& source : sources)
    roots.push_back(source.first);
  std::map<IRCallId, unsigned> color;
  for (auto root : roots) {
    if (color[root] == 2)
      continue;
    std::vector<std::pair<IRCallId, std::size_t>> stack{{root, 0}};
    color[root] = 1;
    while (!stack.empty()) {
      auto& frame = stack.back();
      const auto& inputs = sources.at(frame.first)->inputs;
      if (frame.second == inputs.size()) {
        color[frame.first] = 2;
        entry.calls.push_back(frame.first);
        stack.pop_back();
        continue;
      }
      const auto& input = inputs[frame.second++];
      const auto* upstream = std::get_if<WorkflowNodeOutput>(&input);
      if (!upstream || color[upstream->source_node] == 2)
        continue;
      if (color[upstream->source_node] == 1)
        return Result<SemanticGraphIR>(
            {ErrorCode::Cycle, "DAG contains a cycle"});
      color[upstream->source_node] = 1;
      stack.push_back({upstream->source_node, 0});
    }
  }
  if (entry.calls.empty()) {
    entry.terminator = std::move(returned);
    program.blocks.push_back(std::move(entry));
  } else {
    // The DFS continuation after each upstream call is an explicit jump.
    // A multi-input node is reached only after all of its input branches;
    // shared frontend invocations keep one definition and one call site.
    program.entry = entry.calls.front();
    for (std::size_t i = 0; i < entry.calls.size(); ++i) {
      IRBlock block;
      block.id = entry.calls[i];
      block.calls = {entry.calls[i]};
      if (i + 1 < entry.calls.size())
        block.terminator = IRJump{{entry.calls[i + 1], {}}};
      else
        block.terminator = std::move(returned);
      program.blocks.push_back(std::move(block));
    }
  }
  auto bound = bind_ir(std::move(program), std::move(resources));
  if (!bound.ok())
    return bound;
  auto semantic = bound.take_value();
  semantic.revision_ = snapshot.revision();
  semantic.current_check_ = [snapshot]() noexcept {
    return snapshot.current();
  };
  if (!semantic.current())
    return Result<SemanticGraphIR>(
        {ErrorCode::Stale, "DAG changed during lowering"});
  return Result<SemanticGraphIR>(std::move(semantic));
}

Result<SemanticGraphIR> Compiler::bind_ir(IRProgram program,
                                          ResourceSet resources) const {
  if (program.calls.size() > 65536 || program.blocks.empty() ||
      program.blocks.size() > 65536 ||
      program.input_declarations.size() > 4096 || program.returns.size() > 4096)
    return Result<SemanticGraphIR>(invalid("IR count bounds are invalid"));
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
      return Result<SemanticGraphIR>(invalid("invalid or duplicate IR input"));
    auto status = input_internal::validate_declaration(&input);
    if (!status.ok())
      return Result<SemanticGraphIR>(status);
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
    return Result<SemanticGraphIR>(
        invalid("IR inputs must bind every declaration"));
  for (std::size_t i = 0; i < declarations.size(); ++i) {
    const auto& binding = program.input_bindings[i];
    if (binding.declaration_id != declarations[i].id ||
        !define(binding.value, {SIZE_MAX, 0, 0, declarations[i].result_schema}))
      return Result<SemanticGraphIR>(
          invalid("duplicate or invalid IR input value"));
  }
  std::map<IRBlockId, std::size_t> block_by_id;
  std::map<IRCallId, std::pair<std::size_t, std::size_t>> location;
  for (std::size_t bi = 0; bi < program.blocks.size(); ++bi) {
    const auto& block = program.blocks[bi];
    if (!block.id || !block_by_id.emplace(block.id, bi).second ||
        block.parameters.size() > 4096 || block.calls.size() > 65536)
      return Result<SemanticGraphIR>(invalid("invalid IR block"));
    for (const auto& parameter : block.parameters) {
      if (!parameter.result_schema)
        return Result<SemanticGraphIR>(
            invalid("block parameter requires schema"));
      auto status = parameter.result_schema->validate();
      if (!status.ok())
        return Result<SemanticGraphIR>(status);
      if (!define(parameter.value, {bi, 0, 0, parameter.result_schema}))
        return Result<SemanticGraphIR>(invalid("duplicate block parameter"));
    }
    for (std::size_t ci = 0; ci < block.calls.size(); ++ci)
      if (!location.emplace(block.calls[ci], std::make_pair(bi, ci + 1)).second)
        return Result<SemanticGraphIR>(
            invalid("call belongs to multiple blocks"));
  }
  const auto start = block_by_id.find(program.entry);
  if (start == block_by_id.end() ||
      !program.blocks[start->second].parameters.empty())
    return Result<SemanticGraphIR>(invalid("invalid IR entry block"));
  const auto entry = start->second;
  std::map<IRCallId, const SemanticNode*> calls;
  for (const auto& call : program.calls) {
    if (!call.id || !valid_text(call.operation, 1024) ||
        !calls.emplace(call.id, &call).second || !location.count(call.id) ||
        call.inputs.size() > 1024 || call.parameters.size() > 1024)
      return Result<SemanticGraphIR>(invalid("invalid or unplaced IR call"));
    for (const auto& parameter : call.parameters)
      if (!valid_text(parameter.first, 1024) ||
          (std::holds_alternative<std::string>(parameter.second) &&
           std::get<std::string>(parameter.second).size() > 8192))
        return Result<SemanticGraphIR>(invalid("malformed IR parameter"));
    auto traits = operations_->find_traits(call.operation);
    if (!traits.ok())
      return Result<SemanticGraphIR>(traits.status());
    const auto& contract = traits.value();
    const auto count = call.inputs.size();
    const bool repeated =
        contract.repeated_maximum && !contract.repeated_resolved;
    if (count < contract.input_count ||
        (repeated ? count - contract.input_count < contract.repeated_minimum ||
                        count - contract.input_count > contract.repeated_maximum
                  : count != contract.input_count))
      return Result<SemanticGraphIR>(
          {ErrorCode::TypeMismatch,
           "IR call input count mismatch",
           FailureReason::None,
           {FailureOrigin::Schema, FailureScope::Group}});
    if (call.outputs.size() != traits.value().outputs.size())
      return Result<SemanticGraphIR>(invalid("IR call result count mismatch"));
    auto at = location.at(call.id);
    for (std::size_t oi = 0; oi < call.outputs.size(); ++oi)
      if (call.outputs[oi].key != traits.value().outputs[oi].key ||
          !define(call.outputs[oi].value, {at.first, at.second, call.id, {}}))
        return Result<SemanticGraphIR>(
            invalid("invalid or duplicate call result"));
  }
  if (location.size() != calls.size())
    return Result<SemanticGraphIR>(invalid("block references a missing call"));
  std::vector<std::vector<std::size_t>> successors(program.blocks.size());
  std::vector<std::vector<std::size_t>> predecessors(program.blocks.size());
  for (std::size_t bi = 0; bi < program.blocks.size(); ++bi) {
    for (const auto* edge : edges(program.blocks[bi].terminator)) {
      auto target = block_by_id.find(edge->target);
      if (target == block_by_id.end() ||
          edge->arguments.size() !=
              program.blocks[target->second].parameters.size())
        return Result<SemanticGraphIR>(invalid("invalid IR edge target/arity"));
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
    return Result<SemanticGraphIR>(invalid("IR contains unreachable blocks"));
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
          return Result<SemanticGraphIR>(
              invalid("SSA definition does not dominate call"));
    for (const auto* edge : edges(block.terminator))
      for (auto value : edge->arguments)
        if (!check_use(value, bi, block.calls.size() + 1))
          return Result<SemanticGraphIR>(
              invalid("SSA definition does not dominate edge"));
    if (const auto* branch =
            std::get_if<IRConditionalJump>(&block.terminator)) {
      if (!check_use(branch->condition, bi, block.calls.size() + 1))
        return Result<SemanticGraphIR>(invalid("undefined branch condition"));
    }
    if (const auto* returned = std::get_if<IRReturn>(&block.terminator)) {
      if (returned->values.size() != program.returns.size())
        return Result<SemanticGraphIR>(invalid("IR return arity mismatch"));
      for (auto value : returned->values)
        if (!check_use(value, bi, block.calls.size() + 1))
          return Result<SemanticGraphIR>(
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
  SemanticGraphIR semantic;
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
      return Result<SemanticGraphIR>(prepared.status());
    node.prepared = prepared.take_value();
    node.traits = node.prepared->traits();
    auto inferred =
        infer_operation_outputs(node.traits, input_metadata, node.parameters);
    if (!inferred.ok())
      return Result<SemanticGraphIR>(inferred.status());
    for (std::size_t oi = 0; oi < node.outputs.size(); ++oi) {
      auto schema = inferred.value()[oi].result_schema;
      if (node.outputs[oi].result_schema &&
          !node.outputs[oi].result_schema->same_schema(*schema))
        return Result<SemanticGraphIR>(
            {ErrorCode::TypeMismatch, "IR result schema mismatch"});
      node.outputs[oi].result_schema = schema;
      definitions.at(node.outputs[oi].value).schema = std::move(schema);
    }
    auto selected = resources.select(node.prepared->resource_ids());
    if (!selected.ok())
      return Result<SemanticGraphIR>(selected.status());
    auto joined = semantic.resources_.unite(selected.value());
    if (!joined.ok())
      return Result<SemanticGraphIR>(joined.status());
    semantic.resources_ = joined.take_value();
    semantic.nodes_.push_back(std::move(node));
    for (auto dependent : dependents[id])
      if (--indegree[dependent] == 0)
        ready.push(dependent);
  }
  if (semantic.nodes_.size() != calls.size())
    return Result<SemanticGraphIR>(
        {ErrorCode::Cycle,
         "recursive SSA definition requires block parameters"});
  names.clear();
  for (auto& output : program.returns) {
    if (!valid_text(output.name, 1024) || !names.insert(output.name).second)
      return Result<SemanticGraphIR>(invalid("invalid IR return name"));
    if (output.result_schema) {
      auto status = output.result_schema->validate();
      if (!status.ok())
        return Result<SemanticGraphIR>(status);
    }
  }
  for (const auto& block : program.blocks) {
    for (const auto* edge : edges(block.terminator)) {
      const auto& parameters =
          program.blocks[block_by_id.at(edge->target)].parameters;
      for (std::size_t i = 0; i < parameters.size(); ++i)
        if (!definitions.at(edge->arguments[i])
                 .schema->same_schema(*parameters[i].result_schema))
          return Result<SemanticGraphIR>(
              {ErrorCode::TypeMismatch, "IR edge schema mismatch"});
    }
    if (const auto* branch = std::get_if<IRConditionalJump>(&block.terminator))
      if (!condition_schema(*definitions.at(branch->condition).schema))
        return Result<SemanticGraphIR>(
            {ErrorCode::TypeMismatch, "branch requires one UInt8 sample"});
    if (const auto* returned = std::get_if<IRReturn>(&block.terminator)) {
      for (std::size_t i = 0; i < returned->values.size(); ++i) {
        auto schema = definitions.at(returned->values[i]).schema;
        auto& expected = program.returns[i].result_schema;
        if (!expected)
          expected = schema;
        if (!expected->same_schema(*schema))
          return Result<SemanticGraphIR>(
              {ErrorCode::TypeMismatch, "IR return schema mismatch"});
      }
    }
  }
  for (const auto& output : program.returns)
    if (!output.result_schema)
      return Result<SemanticGraphIR>(
          invalid("return signature requires schema"));
  semantic.input_declarations_ = std::move(program.input_declarations);
  semantic.input_bindings_ = std::move(program.input_bindings);
  semantic.blocks_ = std::move(program.blocks);
  semantic.entry_ = program.entry;
  semantic.outputs_ = std::move(program.returns);
  semantic.operation_registry_ = operations_;
  semantic.current_check_ =
      [registry = std::weak_ptr<OperationRegistry>(operations_)]() noexcept {
        return !registry.expired();
      };
  semantic.digest_.value =
      compiler_internal::semantic_digest(semantic.program());
  return Result<SemanticGraphIR>(std::move(semantic));
}

Result<OptimizedGraphIR> Compiler::optimize(
    const SemanticGraphIR& semantic) const {
  if (!semantic.current() || !semantic.revision() ||
      semantic.operation_registry_.lock().get() != operations_.get())
    return Result<OptimizedGraphIR>({ErrorCode::Stale, "invalid semantic IR"});
  OptimizedGraphIR optimized;
  optimized.revision_ = semantic.revision_;
  optimized.nodes_ = semantic.nodes_;
  optimized.outputs_ = semantic.outputs_;
  optimized.blocks_ = semantic.blocks_;
  optimized.entry_ = semantic.entry_;
  optimized.input_bindings_ = semantic.input_bindings_;
  optimized.input_declarations_ = semantic.input_declarations_;
  optimized.resources_ = semantic.resources_;
  optimized.semantic_digest_ = semantic.digest_;
  optimized.current_check_ = semantic.current_check_;
  optimized.operation_registry_ = operations_;
  optimized.digest_.value = compiler_internal::optimized_digest(
      optimized.semantic_digest_.value, optimized.program());
  return Result<OptimizedGraphIR>(std::move(optimized));
}

Result<CompiledWorkflow> Compiler::compile(const GraphContext& context,
                                           const PlanningOptions& options,
                                           ResourceSet resources) const {
  auto snapshot = context.snapshot();
  auto start = std::chrono::steady_clock::now();
  auto semantic = analyze(snapshot, std::move(resources));
  auto analyzed = std::chrono::steady_clock::now();
  if (!semantic.ok())
    return Result<CompiledWorkflow>(semantic.status());
  auto optimized = optimize(semantic.value());
  auto optimized_at = std::chrono::steady_clock::now();
  if (!optimized.ok())
    return Result<CompiledWorkflow>(optimized.status());
  auto physical = plan(optimized.value(), options);
  auto planned = std::chrono::steady_clock::now();
  if (!physical.ok())
    return Result<CompiledWorkflow>(physical.status());
  if (!snapshot.current())
    return Result<CompiledWorkflow>(
        {ErrorCode::Stale, "DAG changed during compilation"});
  CompiledWorkflow compiled;
  compiled.semantic = semantic.take_value();
  compiled.optimized = optimized.take_value();
  compiled.plan = physical.take_value();
  compiled.diagnostics = {elapsed(start, analyzed),
                          elapsed(analyzed, optimized_at),
                          elapsed(optimized_at, planned)};
  return Result<CompiledWorkflow>(std::move(compiled));
}
}  // namespace ps
