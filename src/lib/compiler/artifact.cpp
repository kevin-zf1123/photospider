#include "photospider/compiler/artifact.hpp"

#include <map>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "compiler/artifact_text.hpp"
#include "compiler/identity.hpp"
#include "photospider/compiler/compiler.hpp"

namespace ps {
namespace {
using compiler_internal::ArtifactData;
Status invalid(const char* text) {
  return {ErrorCode::InvalidArgument, text};
}
Status exhausted() {
  return {ErrorCode::ResourceExhausted, "compiler artifact allocation failed"};
}

IRProgram program(const ArtifactData& data) {
  IRProgram result;
  result.input_declarations = data.declarations;
  result.input_bindings = data.input_bindings;
  result.blocks = data.blocks;
  result.entry = data.entry;
  result.form = data.form;
  result.returns = data.ir_outputs;
  for (const auto& record : data.nodes)
    result.calls.push_back(record.node);
  return result;
}
void store_program(ArtifactData* data, const IRProgram& source) {
  data->declarations = source.input_declarations;
  data->input_bindings = source.input_bindings;
  data->blocks = source.blocks;
  data->entry = source.entry;
  data->form = source.form;
  data->ir_outputs = source.returns;
  for (const auto& node : source.calls)
    data->nodes.push_back({node, node.prepared->resource_ids()});
}
Result<ArtifactManifest> manifest(ArtifactKind kind,
                                  const OperationRegistry& registry,
                                  const ArtifactData& data) {
  ArtifactManifest result;
  result.kind = kind;
  std::set<std::string> operations;
  std::set<ResourceIdentity> resources;
  for (const auto& record : data.nodes) {
    operations.insert(record.node.operation);
    resources.insert(record.resources.begin(), record.resources.end());
  }
  result.resources.assign(resources.begin(), resources.end());
  for (const auto& key : operations) {
    auto traits = registry.find_traits(key);
    if (!traits.ok())
      return Result<ArtifactManifest>(traits.status());
    result.operations.push_back(
        {key, compiler_internal::operation_digest(traits.value())});
  }
  return Result<ArtifactManifest>(std::move(result));
}
Result<ResourceSet> link(const OperationRegistry& operations,
                         const ArtifactData& data,
                         const ResourceSet& available) {
  std::set<std::string> declared, used;
  for (const auto& required : data.manifest.operations) {
    auto traits = operations.find_traits(required.key);
    if (!traits.ok())
      return Result<ResourceSet>(traits.status());
    if (compiler_internal::operation_digest(traits.value()) !=
        required.contract_digest)
      return Result<ResourceSet>(
          {ErrorCode::TypeMismatch,
           "operation contract changed: " + required.key});
    if (!declared.insert(required.key).second)
      return Result<ResourceSet>(invalid("duplicate operation dependency"));
  }
  for (const auto& record : data.nodes)
    used.insert(record.node.operation);
  if (used != declared)
    return Result<ResourceSet>(
        invalid("operation dependency table is not exact"));
  return available.select(data.manifest.resources);
}
Status validate_resources(const ValidatedIR& ir, const ArtifactData& data) {
  std::map<IRCallId, const compiler_internal::IRRecord*> records;
  std::set<ResourceIdentity> used;
  for (const auto& record : data.nodes)
    records.emplace(record.node.id, &record);
  for (const auto& call : ir.nodes()) {
    auto expected = records.at(call.id)->resources;
    auto actual = call.prepared->resource_ids();
    if (actual != expected)
      return {ErrorCode::TypeMismatch,
              "prepared resource dependencies changed"};
    used.insert(actual.begin(), actual.end());
  }
  if (used != std::set<ResourceIdentity>(data.manifest.resources.begin(),
                                         data.manifest.resources.end()))
    return invalid("resource dependency table is not exact");
  return Status::success();
}
template <class IR>
Result<std::string> export_graph(const IR& ir, ArtifactKind kind,
                                 const OperationRegistry& registry,
                                 const std::string& parent) {
  ArtifactData data;
  data.parent_digest = parent;
  data.provenance = ir.provenance();
  store_program(&data, ir.program());
  auto dependencies = manifest(kind, registry, data);
  if (!dependencies.ok())
    return Result<std::string>(dependencies.status());
  data.manifest = dependencies.take_value();
  return compiler_internal::encode_artifact(data);
}
}  // namespace

Result<std::string> CompilerArtifact::export_ir(const ValidatedIR& ir) {
  try {
    auto registry = ir.operation_registry_.lock();
    if (!registry || !ir.current() || !ir.revision())
      return Result<std::string>({ErrorCode::Stale, "IR is stale"});
    return export_graph(ir, ArtifactKind::IR, *registry,
                        ir.provenance().parent_digest);
  } catch (const std::bad_alloc&) {
    return Result<std::string>(exhausted());
  }
}
Result<std::string> CompilerArtifact::export_plan(const ExecutionPlan& plan) {
  try {
    auto registry = plan.data().operation_registry_.lock();
    if (!registry || !plan.current() || !plan.revision())
      return Result<std::string>({ErrorCode::Stale, "plan is stale"});
    // Artifact limits are fixed, even if a trusted planner was allowed a
    // larger in-memory program. Never emit tables our loader cannot accept.
    auto checked = physical::validate_program(plan.data().program_);
    if (!checked.ok())
      return Result<std::string>(checked);
    ArtifactData data;
    data.parent_digest = plan.data().input_ir_.value;
    store_program(&data, plan.data().source_program_);
    auto dependencies = manifest(ArtifactKind::ExecutionPlan, *registry, data);
    if (!dependencies.ok())
      return Result<std::string>(dependencies.status());
    data.manifest = dependencies.take_value();
    data.options.tile_extent = plan.data().program_.tile_extent;
    data.options.execution_mode = plan.data().mode_;
    data.options.output_requests = plan.data().requests_;
    data.plan_outputs = plan.data().outputs_;
    data.physical = plan.data().program_;
    data.bindings = plan.data().calls_;
    return compiler_internal::encode_artifact(data);
  } catch (const std::bad_alloc&) {
    return Result<std::string>(exhausted());
  }
}
Result<ArtifactManifest> CompilerArtifact::inspect(std::string_view text) {
  auto data = compiler_internal::decode_artifact(text);
  if (!data.ok())
    return Result<ArtifactManifest>(data.status());
  return Result<ArtifactManifest>(std::move(data.take_value().manifest));
}
Result<ValidatedIR> CompilerArtifact::import_ir(const Compiler& compiler,
                                                std::string_view text,
                                                ResourceSet resources) {
  try {
    auto decoded = compiler_internal::decode_artifact(text);
    if (!decoded.ok())
      return Result<ValidatedIR>(decoded.status());
    auto data = decoded.take_value();
    if (data.manifest.kind == ArtifactKind::ExecutionPlan)
      return Result<ValidatedIR>(invalid("expected psir"));
    auto bound_resources = link(*compiler.operations(), data, resources);
    if (!bound_resources.ok())
      return Result<ValidatedIR>(bound_resources.status());
    auto bound = compiler.bind_ir(program(data), bound_resources.take_value());
    if (!bound.ok())
      return Result<ValidatedIR>(bound.status());
    auto checked = validate_resources(bound.value(), data);
    if (!checked.ok())
      return Result<ValidatedIR>(checked);
    auto result = bound.take_value();
    result.provenance_ = data.provenance;
    result.provenance_.parent_digest = data.parent_digest;
    return Result<ValidatedIR>(std::move(result));
  } catch (const std::bad_alloc&) {
    return Result<ValidatedIR>(exhausted());
  }
}
Result<ExecutionPlan> CompilerArtifact::import_plan(
    std::shared_ptr<OperationRegistry> operations, std::string_view text,
    ResourceSet resources) {
  try {
    if (!operations || !operations->frozen())
      return Result<ExecutionPlan>(
          invalid("Plan loading requires a frozen operation registry"));
    // Binding/linking services use only the supplied contract environment.
    Compiler compiler(std::move(operations));
    auto decoded = compiler_internal::decode_artifact(text);
    if (!decoded.ok())
      return Result<ExecutionPlan>(decoded.status());
    auto data = decoded.take_value();
    if (data.manifest.kind != ArtifactKind::ExecutionPlan)
      return Result<ExecutionPlan>(invalid("expected psxp"));
    if (data.form != IRForm::ControlFlow)
      return Result<ExecutionPlan>(
          invalid("psxp requires lowered control flow"));
    auto bound_resources = link(*compiler.operations(), data, resources);
    if (!bound_resources.ok())
      return Result<ExecutionPlan>(bound_resources.status());
    auto bound = compiler.bind_ir(program(data), bound_resources.take_value());
    if (!bound.ok())
      return Result<ExecutionPlan>(bound.status());
    auto checked = validate_resources(bound.value(), data);
    if (!checked.ok())
      return Result<ExecutionPlan>(checked);
    auto validated = bound.take_value();
    // Validate serialized derived tables as supplied. Import never repairs
    // adjacency/reservations and never invokes an operation lowerer.
    checked = physical::validate_program(data.physical);
    if (!checked.ok())
      return Result<ExecutionPlan>(checked);
    data.options.output_names.emplace();
    for (const auto& output : data.plan_outputs)
      data.options.output_names->push_back(output.first);
    auto linked = compiler.link_plan(validated, data.options);
    if (!linked.ok())
      return linked;
    auto result = linked.take_value();
    if (data.plan_outputs != result.edit().outputs_ ||
        data.options.output_requests != result.edit().requests_ ||
        data.bindings.size() != data.physical.calls.size())
      return Result<ExecutionPlan>(invalid("invalid physical output mapping"));
    std::map<IRCallId, const SemanticNode*> nodes;
    for (const auto& node : result.edit().source_program_.calls)
      nodes.emplace(node.id, &node);
    for (std::size_t ci = 0; ci < data.bindings.size(); ++ci) {
      auto found = nodes.find(data.physical.calls[ci].source_call);
      if (found == nodes.end())
        return Result<ExecutionPlan>(invalid("unknown physical source call"));
      auto& binding = data.bindings[ci];
      binding.operation = found->second->operation;
      binding.parameters = found->second->parameters;
      binding.prepared = found->second->prepared;
    }
    result.edit().program_ = std::move(data.physical);
    result.edit().calls_ = std::move(data.bindings);
    result.edit().input_ir_.value = data.parent_digest;
    checked = compiler.validate_plan(result);
    if (!checked.ok())
      return Result<ExecutionPlan>(checked);
    result.edit().digest_.value = compiler_internal::physical_digest(result);
    return Result<ExecutionPlan>(std::move(result));
  } catch (const std::bad_alloc&) {
    return Result<ExecutionPlan>(exhausted());
  }
}

Status CompilerArtifact::save_ir(const ValidatedIR& ir,
                                 const std::string& path) {
  return compiler_internal::write_artifact_file(path, export_ir(ir));
}
Status CompilerArtifact::save_plan(const ExecutionPlan& plan,
                                   const std::string& path) {
  return compiler_internal::write_artifact_file(path, export_plan(plan));
}
Result<ValidatedIR> CompilerArtifact::load_ir(const Compiler& compiler,
                                              const std::string& path,
                                              ResourceSet resources) {
  auto text = compiler_internal::read_artifact_file(path);
  if (!text.ok())
    return Result<ValidatedIR>(text.status());
  return import_ir(compiler, text.value(), std::move(resources));
}
Result<ExecutionPlan> CompilerArtifact::load_plan(
    std::shared_ptr<OperationRegistry> operations, const std::string& path,
    ResourceSet resources) {
  auto text = compiler_internal::read_artifact_file(path);
  if (!text.ok())
    return Result<ExecutionPlan>(text.status());
  return import_plan(std::move(operations), text.value(), std::move(resources));
}
}  // namespace ps
