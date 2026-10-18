#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>

#include "compiler/planners/static_tiles.hpp"
#include "photospider/compiler/planner.hpp"

namespace ps {
Status PlannerRegistry::add(PlannerDefinition definition) {
  if (frozen_ || definition.name.empty() || definition.name.size() > 1024 ||
      definition.version.empty() || definition.version.size() > 128 ||
      !definition.plan || find(definition.name) ||
      definition.supported_ir_forms.empty())
    return {ErrorCode::InvalidArgument,
            "invalid/duplicate planner or frozen registry"};
  std::set<IRForm> forms;
  for (auto form : definition.supported_ir_forms)
    if ((form != IRForm::DependencyPaths && form != IRForm::ControlFlow) ||
        !forms.insert(form).second)
      return {ErrorCode::InvalidArgument, "invalid/duplicate planner IR form"};
  definitions_.push_back(std::move(definition));
  return Status::success();
}
const PlannerDefinition* PlannerRegistry::find(
    const std::string& name) const noexcept {
  for (const auto& definition : definitions_)
    if (definition.name == name)
      return &definition;
  return nullptr;
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
}  // namespace ps
