#include <algorithm>
#include <exception>
#include <string>
#include <utility>

#include "compiler/identity.hpp"
#include "photospider/compiler/compiler.hpp"
#include "plugin/dependency_identity.hpp"

namespace ps {
Result<ExecutionPlan> Compiler::plan(const ValidatedIR& ir,
                                     const PlanningOptions& options) const {
  if (!ir.current() || !ir.revision() ||
      ir.operation_registry_.lock().get() != operations_.get())
    return Result<ExecutionPlan>(
        {ErrorCode::Stale, "invalid planner input IR"});
  const auto* definition = planners().find(options.planner);
  if (!definition)
    return Result<ExecutionPlan>(
        {ErrorCode::NotFound, "unknown planner: " + options.planner});
  const auto producer =
      "planner " + definition->name + "@" + definition->version;
  if (std::find(definition->supported_ir_forms.begin(),
                definition->supported_ir_forms.end(),
                ir.form()) == definition->supported_ir_forms.end())
    return Result<ExecutionPlan>({ErrorCode::InvalidArgument,
                                  producer + " does not support this IR form"});
  if (!options.maximum_instructions || !options.maximum_dependency_edges)
    return Result<ExecutionPlan>({ErrorCode::InvalidArgument,
                                  "static planning budgets must be positive"});
  auto linked = link_plan(ir, options);
  if (!linked.ok())
    return linked;
  PlanBuilder builder(linked.take_value());
  try {
    auto status = definition->plan(ir, builder, options);
    if (!status.ok()) {
      status.message = producer + ": " + status.message;
      return Result<ExecutionPlan>(std::move(status));
    }
    auto result = std::move(builder.plan_);
    if (!result.current())
      return Result<ExecutionPlan>(
          {ErrorCode::Stale, "IR changed during planning"});
    std::uint64_t instructions = 0, edges = 0;
    for (const auto& block : result.blocks_) {
      if (block.instructions.size() >
          options.maximum_instructions - instructions)
        return Result<ExecutionPlan>(
            {ErrorCode::ResourceExhausted,
             producer + " exceeded instruction limit"});
      instructions += block.instructions.size();
      for (const auto& instruction : block.instructions) {
        if (instruction.dependencies.size() >
            options.maximum_dependency_edges - edges)
          return Result<ExecutionPlan>(
              {ErrorCode::ResourceExhausted,
               producer + " exceeded dependency edge limit"});
        edges += instruction.dependencies.size();
      }
    }
    status = validate_plan(result);
    if (!status.ok())
      return Result<ExecutionPlan>(
          {ErrorCode::Internal,
           producer + " produced invalid Plan: " + status.message});
    result.digest_.value = compiler_internal::physical_digest(result);
    result.cache_key_.value =
        compiler_internal::plan_cache_key(result.digest_.value);
    auto work = UINT64_MAX;
    result.structured_templates_ =
        contract_internal::dependency_cache_templates(result, {}, &work, {});
    if (!result.current())
      return Result<ExecutionPlan>(
          {ErrorCode::Stale, "IR changed during planning"});
    return Result<ExecutionPlan>(std::move(result));
  } catch (const std::bad_alloc&) {
    return Result<ExecutionPlan>(
        {ErrorCode::ResourceExhausted, producer + " allocation failed"});
  } catch (const std::exception& e) {
    return Result<ExecutionPlan>(
        {ErrorCode::Internal, producer + ": " + e.what()});
  } catch (...) {
    return Result<ExecutionPlan>(
        {ErrorCode::Internal, producer + " callback failed"});
  }
}
const PlannerRegistry& Compiler::planners() const {
  return planners_ ? *planners_ : *make_default_planner_registry();
}
}  // namespace ps
