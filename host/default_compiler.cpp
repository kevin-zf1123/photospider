#include <memory>
#include <stdexcept>
#include <utility>

#include "photospider/compiler/compiler.hpp"
#include "plugins/frontends/dag/dag.hpp"
#include "plugins/optimizers/deduplicate/optimizer_internal.hpp"
#include "plugins/planners/static_tiles/static_tiles.hpp"
namespace ps {
Compiler::Compiler(std::shared_ptr<OperationRegistry> ops,
                   std::shared_ptr<const OptimizerRegistry> optimizers,
                   std::shared_ptr<const PlannerRegistry> planners,
                   std::shared_ptr<const FrontendRegistry> frontends)
    : Compiler(
          std::move(ops),
          CompilerComponents{
              optimizers ? std::move(optimizers)
                         : make_default_optimizer_registry(),
              planners ? std::move(planners) : make_default_planner_registry(),
              frontends ? std::move(frontends)
                        : make_default_frontend_registry()}) {
}  // NOLINT(whitespace/indent_namespace)
std::shared_ptr<const FrontendRegistry> make_default_frontend_registry() {
  static const auto registry = [] {
    auto result = std::make_shared<FrontendRegistry>();
    auto status = result->add(compiler_internal::dag_frontend());
    if (!status.ok())
      throw std::logic_error(status.message);
    result->freeze();
    return std::shared_ptr<const FrontendRegistry>(std::move(result));
  }();
  return registry;
}
std::shared_ptr<const PlannerRegistry> make_default_planner_registry() {
  static const auto registry = [] {
    auto result = std::make_shared<PlannerRegistry>();
    auto status = result->add(compiler_internal::static_tile_planner());
    if (!status.ok())
      throw std::logic_error(status.message);
    result->freeze();
    return std::shared_ptr<const PlannerRegistry>(std::move(result));
  }();
  return registry;
}
std::shared_ptr<const OptimizerRegistry> make_default_optimizer_registry() {
  static const auto registry = [] {
    auto result = std::make_shared<OptimizerRegistry>();
    auto status = result->add(compiler_internal::dependency_path_optimizer());
    if (!status.ok())
      throw std::logic_error(status.message);
    result->freeze();
    return std::shared_ptr<const OptimizerRegistry>(std::move(result));
  }();
  return registry;
}
}  // namespace ps
