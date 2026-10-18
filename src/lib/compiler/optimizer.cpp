#include <algorithm>
#include <exception>
#include <map>
#include <string>
#include <utility>

#include "photospider/compiler/compiler.hpp"

namespace ps {
namespace {
bool same_interface(const ValidatedIR& before, const ValidatedIR& after) {
  if (before.input_declarations().size() != after.input_declarations().size() ||
      before.outputs().size() != after.outputs().size())
    return false;
  std::map<std::string, const SchemaTemplate*> inputs;
  for (const auto& input : before.input_declarations())
    inputs.emplace(input.name, input.result_schema.get());
  for (const auto& input : after.input_declarations()) {
    auto found = inputs.find(input.name);
    if (found == inputs.end() ||
        !found->second->same_schema(*input.result_schema))
      return false;
  }
  for (std::size_t i = 0; i < before.outputs().size(); ++i)
    if (before.outputs()[i].name != after.outputs()[i].name ||
        !before.outputs()[i].result_schema->same_schema(
            *after.outputs()[i].result_schema))
      return false;
  return true;
}
}  // namespace
Result<ValidatedIR> Compiler::optimize(
    const ValidatedIR& input, const OptimizerInvocation& invocation) const {
  if (!input.current() || !input.revision() ||
      input.operation_registry_.lock().get() != operations_.get())
    return Result<ValidatedIR>(
        {ErrorCode::Stale, "invalid optimizer input IR"});
  const auto* definition = optimizers().find(invocation.name);
  if (!definition)
    return Result<ValidatedIR>(
        {ErrorCode::NotFound, "unknown optimizer: " + invocation.name});
  const auto producer =
      "optimizer " + definition->name + "@" + definition->version;
  try {
    if (std::find(definition->supported_ir_forms.begin(),
                  definition->supported_ir_forms.end(),
                  input.form()) == definition->supported_ir_forms.end())
      return Result<ValidatedIR>({ErrorCode::InvalidArgument,
                                  producer + " does not support this IR form"});
    auto resolved = definition->resolve_options(invocation.options);
    if (!resolved.ok())
      return Result<ValidatedIR>(resolved.status());
    auto options = resolved.take_value();
    auto transformed = definition->transform(input, options);
    if (!transformed.ok()) {
      auto status = transformed.status();
      status.message = producer + ": " + status.message;
      return Result<ValidatedIR>(std::move(status));
    }
    auto validated = bind_ir(transformed.take_value(), input.resources());
    if (!validated.ok())
      return Result<ValidatedIR>(
          {ErrorCode::Internal,
           producer + " produced invalid IR: " + validated.status().message});
    auto result = validated.take_value();
    if (!same_interface(input, result))
      return Result<ValidatedIR>(
          {ErrorCode::Internal,
           producer + " changed the public input/output contract"});
    result.revision_ = input.revision_;
    result.current_check_ = input.current_check_;
    result.provenance_ = {input.digest().value, definition->name,
                          definition->version, options};
    if (!result.current())
      return Result<ValidatedIR>(
          {ErrorCode::Stale, "IR changed during optimization"});
    return Result<ValidatedIR>(std::move(result));
  } catch (const std::bad_alloc&) {
    return Result<ValidatedIR>(
        {ErrorCode::ResourceExhausted, producer + " allocation failed"});
  } catch (const std::exception& e) {
    return Result<ValidatedIR>(
        {ErrorCode::Internal, producer + ": " + e.what()});
  } catch (...) {
    return Result<ValidatedIR>(
        {ErrorCode::Internal, producer + " callback failed"});
  }
}
const OptimizerRegistry& Compiler::optimizers() const {
  return optimizers_ ? *optimizers_ : *make_default_optimizer_registry();
}
}  // namespace ps
