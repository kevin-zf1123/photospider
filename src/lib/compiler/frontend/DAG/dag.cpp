#include "compiler/frontend/DAG/dag.hpp"

#include <algorithm>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace ps::compiler_internal {
namespace {
Status invalid(const char* message) {
  return {ErrorCode::InvalidArgument, message};
}

Result<IRProgram> lower_dag(const WorkflowDocument& document,
                            const OperationRegistry& operations) {
  if (document.schema_version != 7 || document.nodes.size() > 65536 ||
      document.inputs.size() > 4096 || document.outputs.size() > 4096 ||
      document.outputs.empty())
    return Result<IRProgram>(invalid("unsupported DAG version/counts"));
  IRProgram program;
  program.form = IRForm::DependencyPaths;
  program.input_declarations = document.inputs;
  IRBlock chain;
  auto finish_chain = [&]() {
    if (chain.calls.empty())
      return;
    chain.id = program.blocks.size() + 1;
    if (program.blocks.empty())
      program.entry = chain.id;
    else
      program.blocks.back().terminator = IRJump{{chain.id, {}}};
    program.blocks.push_back(std::move(chain));
    chain = IRBlock{};
  };
  IRValueId next_value = 1;
  std::map<std::uint64_t, IRValueId> external;
  auto declarations = document.inputs;
  std::sort(declarations.begin(), declarations.end(),
            [](const auto& a, const auto& b) { return a.id < b.id; });
  for (const auto& input : declarations) {
    if (!external.emplace(input.id, next_value).second)
      return Result<IRProgram>(invalid("duplicate DAG input"));
    program.input_bindings.push_back({input.id, next_value++});
  }
  std::map<IRCallId, const WorkflowNode*> sources;
  std::map<std::pair<IRCallId, std::string>, IRValueId> values;
  for (const auto& node : document.nodes)
    if (!node.id || !sources.emplace(node.id, &node).second)
      return Result<IRProgram>(invalid("duplicate or zero DAG node"));
  for (const auto& pair : sources) {
    const auto& node = *pair.second;
    auto traits = operations.find_traits(node.operation);
    if (!traits.ok())
      return Result<IRProgram>(traits.status());
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
          return Result<IRProgram>(
              {ErrorCode::NotFound, "DAG references a missing output"});
        call.inputs.push_back(found->second);
      } else {
        auto found =
            external.find(std::get<WorkflowInputReference>(input).input_id);
        if (found == external.end())
          return Result<IRProgram>(
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
      return Result<IRProgram>(
          {ErrorCode::NotFound, "DAG return references a missing output"});
    program.returns.push_back({output.name, {}});
    returned.values.push_back(found->second);
    roots.push_back(output.node_id);
  }
  // Raw paths retain repeated requests for shared DAG invocations. Remaining
  // roots also preserve calls and effects unreachable from named outputs.
  const auto named_roots = roots.size();
  for (const auto& source : sources)
    roots.push_back(source.first);
  std::map<IRCallId, unsigned> color;
  std::size_t occurrences = 0;
  for (std::size_t ri = 0; ri < roots.size(); ++ri) {
    const auto root = roots[ri];
    if (ri >= named_roots && color[root] == 2)
      continue;
    finish_chain();
    std::vector<std::pair<IRCallId, std::size_t>> stack{{root, 0}};
    color[root] = 1;
    while (!stack.empty()) {
      auto& frame = stack.back();
      const auto& inputs = sources.at(frame.first)->inputs;
      if (frame.second == inputs.size()) {
        // A join starts its continuation only after every input branch has
        // been emitted. Unary successors can stay in that continuation.
        if (inputs.size() > 1)
          finish_chain();
        color[frame.first] = 2;
        if (++occurrences > 1048576 || program.blocks.size() >= 65536)
          return Result<IRProgram>(
              {ErrorCode::ResourceExhausted, "DAG dependency path limit"});
        chain.calls.push_back(frame.first);
        stack.pop_back();
        continue;
      }
      const auto& input = inputs[frame.second++];
      const auto* upstream = std::get_if<WorkflowNodeOutput>(&input);
      if (!upstream)
        continue;
      if (color[upstream->source_node] == 1)
        return Result<IRProgram>({ErrorCode::Cycle, "DAG contains a cycle"});
      // Descending after emitting calls switches to another dependency path.
      // Shared ancestors are expanded again without cloning their call IDs.
      finish_chain();
      color[upstream->source_node] = 1;
      stack.push_back({upstream->source_node, 0});
    }
  }
  finish_chain();
  // A validated named output always supplies at least one reachable call.
  program.blocks.back().terminator = std::move(returned);
  return Result<IRProgram>(std::move(program));
}
}  // namespace

FrontendDefinition dag_frontend() {
  return {"dag", "1", "WorkflowDocument DAG to dependency-path IR", lower_dag};
}
}  // namespace ps::compiler_internal
