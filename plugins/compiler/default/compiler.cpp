#include "photospider/compiler/compiler.hpp"

#include <chrono>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "photospider/compiler/program_services.hpp"

namespace ps {
namespace {
std::uint64_t elapsed(std::chrono::steady_clock::time_point start,
                      std::chrono::steady_clock::time_point end) {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::microseconds>(end - start)
          .count());
}
}  // namespace

Compiler::Compiler(std::shared_ptr<OperationRegistry> operations,
                   CompilerComponents components)
    : operations_(std::move(operations)),
      optimizers_(std::move(components.optimizers)),
      planners_(std::move(components.planners)),
      frontends_(std::move(components.frontends)) {
  if (!operations_ || !operations_->frozen() || !optimizers_ ||
      !optimizers_->frozen() || !planners_ || !planners_->frozen() ||
      !frontends_ || !frontends_->frozen())
    throw std::invalid_argument("compiler requires nonnull frozen catalogs");
}

Result<ValidatedIR> Compiler::bind_ir(IRProgram program,
                                      ResourceSet resources) const {
  return ProgramServices(operations_)
      .bind_ir(std::move(program), std::move(resources));
}

Result<CompiledWorkflow> Compiler::compile(
    const GraphContext& context, const PlanningOptions& options,
    ResourceSet resources, const std::vector<OptimizerInvocation>& optimizers,
    const std::string& frontend) const {
  auto snapshot = context.snapshot();
  auto start = std::chrono::steady_clock::now();
  auto semantic = analyze(snapshot, std::move(resources), frontend);
  auto analyzed = std::chrono::steady_clock::now();
  if (!semantic.ok())
    return Result<CompiledWorkflow>(semantic.status());
  CompiledWorkflow compiled;
  compiled.ir_versions.push_back({0, semantic.take_value()});
  for (const auto& invocation : optimizers) {
    auto next = optimize(*compiled.ir_versions.back().ir, invocation);
    if (!next.ok())
      return Result<CompiledWorkflow>(next.status());
    compiled.ir_versions.push_back(
        {compiled.ir_versions.size(), next.take_value()});
  }
  auto optimized_at = std::chrono::steady_clock::now();
  auto physical = plan(*compiled.ir_versions.back().ir, options);
  auto planned = std::chrono::steady_clock::now();
  if (!physical.ok())
    return Result<CompiledWorkflow>(physical.status());
  if (!snapshot.current())
    return Result<CompiledWorkflow>(
        {ErrorCode::Stale, "DAG changed during compilation"});
  compiled.plan = physical.take_value();
  compiled.diagnostics = {
      elapsed(start, analyzed),
      optimizers.empty() ? 0 : elapsed(analyzed, optimized_at),
      elapsed(optimized_at, planned)};
  return Result<CompiledWorkflow>(std::move(compiled));
}
}  // namespace ps
