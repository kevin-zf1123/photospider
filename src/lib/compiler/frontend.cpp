#include <exception>
#include <string>
#include <utility>

#include "photospider/compiler/compiler.hpp"

namespace ps {
Result<ValidatedIR> Compiler::analyze(const GraphSnapshot& snapshot,
                                      ResourceSet resources,
                                      const std::string& frontend) const {
  if (!snapshot.current())
    return Result<ValidatedIR>({ErrorCode::Stale, "graph snapshot is stale"});
  const auto* definition = frontends().find(frontend);
  if (!definition)
    return Result<ValidatedIR>(
        {ErrorCode::NotFound, "unknown frontend: " + frontend});
  const auto producer =
      "frontend " + definition->name + "@" + definition->version;
  try {
    auto lowered = definition->lower(snapshot.document(), *operations_);
    if (!lowered.ok()) {
      auto status = lowered.status();
      status.message = producer + ": " + status.message;
      return Result<ValidatedIR>(std::move(status));
    }
    if (!snapshot.current())
      return Result<ValidatedIR>(
          {ErrorCode::Stale, "source changed during lowering"});
    auto bound = bind_ir(lowered.take_value(), std::move(resources));
    if (!bound.ok()) {
      auto status = bound.status();
      status.message = producer + ": " + status.message;
      return Result<ValidatedIR>(std::move(status));
    }
    auto result = bound.take_value();
    result.provenance_ = {{}, definition->name, definition->version, {}};
    result.revision_ = snapshot.revision();
    result.current_check_ = [snapshot]() noexcept {
      return snapshot.current();
    };
    if (!result.current())
      return Result<ValidatedIR>(
          {ErrorCode::Stale, "source changed during analysis"});
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
const FrontendRegistry& Compiler::frontends() const {
  return frontends_ ? *frontends_ : *make_default_frontend_registry();
}
}  // namespace ps
