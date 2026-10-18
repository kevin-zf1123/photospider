#include "photospider/compiler/artifact.hpp"

#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "compiler/artifact_text.hpp"
#include "compiler/identity.hpp"
#include "plugin/dependency_identity.hpp"

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
Result<ResourceSet> link(const Compiler& compiler, const ArtifactData& data,
                         const ResourceSet& available) {
  std::set<std::string> declared, used;
  for (const auto& required : data.manifest.operations) {
    auto traits = compiler.operations()->find_traits(required.key);
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
Status validate_resources(const SemanticGraphIR& ir, const ArtifactData& data) {
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
  store_program(&data, ir.program());
  auto dependencies = manifest(kind, registry, data);
  if (!dependencies.ok())
    return Result<std::string>(dependencies.status());
  data.manifest = dependencies.take_value();
  return compiler_internal::encode_artifact(data);
}
}  // namespace

Result<std::string> CompilerArtifact::export_ir(const SemanticGraphIR& ir) {
  try {
    auto registry = ir.operation_registry_.lock();
    if (!registry || !ir.current() || !ir.revision())
      return Result<std::string>({ErrorCode::Stale, "IR is stale"});
    return export_graph(ir, ArtifactKind::SemanticIR, *registry, {});
  } catch (const std::bad_alloc&) {
    return Result<std::string>(exhausted());
  }
}
Result<std::string> CompilerArtifact::export_ir(const OptimizedGraphIR& ir) {
  try {
    auto registry = ir.operation_registry_.lock();
    if (!registry || !ir.current() || !ir.revision())
      return Result<std::string>({ErrorCode::Stale, "optimized IR is stale"});
    return export_graph(ir, ArtifactKind::OptimizedIR, *registry,
                        ir.semantic_digest().value);
  } catch (const std::bad_alloc&) {
    return Result<std::string>(exhausted());
  }
}
Result<std::string> CompilerArtifact::export_plan(const ExecutionPlan& plan) {
  try {
    auto registry = plan.operation_registry_.lock();
    if (!registry || !plan.current() || !plan.revision())
      return Result<std::string>({ErrorCode::Stale, "plan is stale"});
    ArtifactData data;
    data.parent_digest = plan.optimized_digest_.value;
    store_program(&data, plan.linked_program_);
    auto dependencies = manifest(ArtifactKind::ExecutionPlan, *registry, data);
    if (!dependencies.ok())
      return Result<std::string>(dependencies.status());
    data.manifest = dependencies.take_value();
    data.options.tile_extent = plan.tile_extent_;
    data.options.execution_mode = plan.execution_mode_;
    data.options.output_requests = plan.output_requests_;
    data.plan_outputs = plan.outputs_;
    for (const auto& step : plan.steps_)
      data.steps.push_back({step, {}});
    for (const auto& block : plan.blocks_)
      data.code.emplace(block.id, block.instructions);
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
Result<ImportedIR> CompilerArtifact::import_ir(const Compiler& compiler,
                                               std::string_view text,
                                               ResourceSet resources) {
  try {
    auto decoded = compiler_internal::decode_artifact(text);
    if (!decoded.ok())
      return Result<ImportedIR>(decoded.status());
    auto data = decoded.take_value();
    if (data.manifest.kind == ArtifactKind::ExecutionPlan)
      return Result<ImportedIR>(invalid("expected psir"));
    auto bound_resources = link(compiler, data, resources);
    if (!bound_resources.ok())
      return Result<ImportedIR>(bound_resources.status());
    auto bound = compiler.bind_ir(program(data), bound_resources.take_value());
    if (!bound.ok())
      return Result<ImportedIR>(bound.status());
    auto checked = validate_resources(bound.value(), data);
    if (!checked.ok())
      return Result<ImportedIR>(checked);
    if (data.manifest.kind == ArtifactKind::SemanticIR)
      return Result<ImportedIR>(ImportedIR{bound.take_value()});
    const auto& linked = bound.value();
    OptimizedGraphIR result;
    result.revision_ = linked.revision_;
    result.nodes_ = linked.nodes_;
    result.outputs_ = linked.outputs_;
    result.blocks_ = linked.blocks_;
    result.entry_ = linked.entry_;
    result.input_bindings_ = linked.input_bindings_;
    result.input_declarations_ = linked.input_declarations_;
    result.resources_ = linked.resources_;
    result.current_check_ = linked.current_check_;
    result.operation_registry_ = linked.operation_registry_;
    result.semantic_digest_.value = data.parent_digest;
    result.digest_.value = compiler_internal::optimized_digest(
        result.semantic_digest_.value, result.program());
    return Result<ImportedIR>(ImportedIR{std::move(result)});
  } catch (const std::bad_alloc&) {
    return Result<ImportedIR>(exhausted());
  }
}
Result<ExecutionPlan> CompilerArtifact::import_plan(const Compiler& compiler,
                                                    std::string_view text,
                                                    ResourceSet resources) {
  try {
    auto decoded = compiler_internal::decode_artifact(text);
    if (!decoded.ok())
      return Result<ExecutionPlan>(decoded.status());
    auto data = decoded.take_value();
    if (data.manifest.kind != ArtifactKind::ExecutionPlan)
      return Result<ExecutionPlan>(invalid("expected psxp"));
    auto bound_resources = link(compiler, data, resources);
    if (!bound_resources.ok())
      return Result<ExecutionPlan>(bound_resources.status());
    auto bound = compiler.bind_ir(program(data), bound_resources.take_value());
    if (!bound.ok())
      return Result<ExecutionPlan>(bound.status());
    auto checked = validate_resources(bound.value(), data);
    if (!checked.ok())
      return Result<ExecutionPlan>(checked);
    const auto& declarations = bound.value();
    OptimizedGraphIR optimized;
    optimized.revision_ = declarations.revision_;
    optimized.nodes_ = declarations.nodes_;
    optimized.outputs_ = declarations.outputs_;
    optimized.blocks_ = declarations.blocks_;
    optimized.entry_ = declarations.entry_;
    optimized.input_bindings_ = declarations.input_bindings_;
    optimized.input_declarations_ = declarations.input_declarations_;
    optimized.resources_ = declarations.resources_;
    optimized.current_check_ = declarations.current_check_;
    optimized.operation_registry_ = declarations.operation_registry_;
    optimized.digest_.value = data.parent_digest;
    std::vector<Backend> placements;
    for (const auto& record : data.steps)
      placements.push_back(record.step.backend);
    // Bind declarations and stored placements without optimization, backend
    // selection, tile expansion or an operation dependency-mapping callback.
    auto linked = compiler.link_plan(optimized, data.options, &placements);
    if (!linked.ok())
      return linked;
    auto result = linked.take_value();
    if (data.steps.size() != result.steps_.size() ||
        data.code.size() != result.blocks_.size())
      return Result<ExecutionPlan>(
          invalid("incomplete physical call/code table"));
    for (std::size_t i = 0; i < data.steps.size(); ++i) {
      const auto& record = data.steps[i].step;
      auto& step = result.steps_[i];
      if (record.node_id != step.node_id ||
          record.output_index != step.output_index)
        return Result<ExecutionPlan>(
            invalid("physical call table order mismatch"));
      if (result.execution_mode_ == ExecutionMode::CpuExact &&
          record.backend != Backend::Cpu)
        return Result<ExecutionPlan>(
            invalid("CPU plan contains GPU placement"));
      step.backend = record.backend;
    }
    for (auto& block : result.blocks_) {
      auto found = data.code.find(block.id);
      if (found == data.code.end())
        return Result<ExecutionPlan>(invalid("missing physical code block"));
      block.instructions = std::move(found->second);
    }
    for (const auto& named : data.plan_outputs)
      if (named.second >= result.return_signature_.size() ||
          result.return_signature_[named.second].name != named.first)
        return Result<ExecutionPlan>(
            invalid("invalid physical return mapping"));
    result.outputs_ = data.plan_outputs;
    result.output_requests_ = data.options.output_requests;
    result.optimized_digest_.value = data.parent_digest;
    checked = compiler.finalize_plan(&result);
    if (checked.ok())
      checked = compiler.validate_plan(result);
    if (!checked.ok())
      return Result<ExecutionPlan>(checked);
    result.digest_.value = compiler_internal::physical_digest(result);
    result.cache_key_.value =
        compiler_internal::plan_cache_key(result.digest_.value);
    auto work = UINT64_MAX;
    result.structured_templates_ =
        contract_internal::dependency_cache_templates(result, {}, &work, {});
    return Result<ExecutionPlan>(std::move(result));
  } catch (const std::bad_alloc&) {
    return Result<ExecutionPlan>(exhausted());
  }
}

Status CompilerArtifact::save_ir(const SemanticGraphIR& ir,
                                 const std::string& path) {
  return compiler_internal::write_artifact_file(path, export_ir(ir));
}
Status CompilerArtifact::save_ir(const OptimizedGraphIR& ir,
                                 const std::string& path) {
  return compiler_internal::write_artifact_file(path, export_ir(ir));
}
Status CompilerArtifact::save_plan(const ExecutionPlan& plan,
                                   const std::string& path) {
  return compiler_internal::write_artifact_file(path, export_plan(plan));
}
Result<ImportedIR> CompilerArtifact::load_ir(const Compiler& compiler,
                                             const std::string& path,
                                             ResourceSet resources) {
  auto text = compiler_internal::read_artifact_file(path);
  if (!text.ok())
    return Result<ImportedIR>(text.status());
  return import_ir(compiler, text.value(), std::move(resources));
}
Result<ExecutionPlan> CompilerArtifact::load_plan(const Compiler& compiler,
                                                  const std::string& path,
                                                  ResourceSet resources) {
  auto text = compiler_internal::read_artifact_file(path);
  if (!text.ok())
    return Result<ExecutionPlan>(text.status());
  return import_plan(compiler, text.value(), std::move(resources));
}
}  // namespace ps
