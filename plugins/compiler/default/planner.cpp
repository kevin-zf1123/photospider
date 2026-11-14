#include <algorithm>
#include <exception>
#include <string>
#include <utility>

#include "photospider/compiler/compiler.hpp"
#include "photospider/compiler/program_services.hpp"
#include "sdk/program/identity.hpp"

namespace ps {
Result<ExecutionPlan> Compiler::plan(const ValidatedIR& ir,
                                     const PlanningOptions& options) const {
  using Answer = Result<ExecutionPlan>;
  if (!ir.current() || !ir.revision() ||
      ir.operation_registry_.lock().get() != operations_.get())
    return Answer({ErrorCode::Stale, "invalid planner input IR"});
  const auto* definition = planners().find(options.planner);
  if (!definition)
    return Answer({ErrorCode::NotFound, "unknown planner: " + options.planner});
  if (std::find(definition->supported_ir_forms.begin(),
                definition->supported_ir_forms.end(),
                ir.form()) == definition->supported_ir_forms.end())
    return Answer(
        {ErrorCode::InvalidArgument, "planner does not support IR form"});
  if (!options.maximum_tasks || !options.maximum_records ||
      !options.maximum_dependency_edges || !options.maximum_work)
    return Answer({ErrorCode::InvalidArgument,
                   "physical planning limits must be positive"});
  const auto producer =
      "planner " + definition->name + "@" + definition->version;
  try {
    ProgramServices services(operations_);
    auto linked = services.link_plan(ir, options);
    if (!linked.ok())
      return linked;
    PlanBuilder builder(linked.take_value());
    auto status = definition->plan(ir, builder, options);
    if (!status.ok()) {
      status.message = producer + ": " + status.message;
      return Answer(std::move(status));
    }
    auto result = std::move(builder.plan_);
    if (result.edit().program_.tasks.size() > options.maximum_tasks)
      return Answer(
          {ErrorCode::ResourceExhausted, producer + ": physical task limit"});
    status = services.validate_plan(result);
    if (!status.ok()) {
      if (status.code == ErrorCode::ResourceExhausted) {
        status.message = producer + ": " + status.message;
        return Answer(std::move(status));
      }
      return Answer(
          {ErrorCode::Internal,
           producer + " produced invalid physical program: " + status.message});
    }
    const auto& program = result.program();
    for (unsigned i = 0; i < result.input_declarations().size(); ++i) {
      const auto& name = result.input_declarations()[i].name;
      const auto& input = program.values[program.inputs[i]];
      auto layout = options.input_layouts.find(name);
      auto domain = options.input_domains.find(name);
      if (layout != options.input_layouts.end() &&
          input.input_layout != layout->second)
        return Answer({ErrorCode::Internal,
                       producer + " changed an explicit input layout"});
      if (domain != options.input_domains.end())
        for (auto id : input.tiles)
          if (program.tiles[id].domain != domain->second)
            return Answer({ErrorCode::Internal,
                           producer + " changed an explicit input domain"});
    }
    result.edit().digest_.value = compiler_internal::physical_digest(result);
    if (!result.current())
      return Answer({ErrorCode::Stale, "IR changed during planning"});
    return Answer(std::move(result));
  } catch (const std::bad_alloc&) {
    return Answer(
        {ErrorCode::ResourceExhausted, producer + " allocation failed"});
  } catch (const std::exception& error) {
    return Answer({ErrorCode::Internal, producer + ": " + error.what()});
  } catch (...) {
    return Answer({ErrorCode::Internal, producer + " callback failed"});
  }
}
const PlannerRegistry& Compiler::planners() const {
  return *planners_;
}
}  // namespace ps
