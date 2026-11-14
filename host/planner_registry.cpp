#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>

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
}  // namespace ps
