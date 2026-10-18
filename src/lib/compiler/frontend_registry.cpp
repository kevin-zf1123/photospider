#include <memory>
#include <stdexcept>
#include <string>
#include <utility>

#include "compiler/frontend/DAG/dag.hpp"
#include "photospider/compiler/frontend.hpp"

namespace ps {
Status FrontendRegistry::add(FrontendDefinition definition) {
  if (frozen_ || definition.name.empty() || definition.name.size() > 1024 ||
      definition.version.empty() || definition.version.size() > 128 ||
      !definition.lower || find(definition.name))
    return {ErrorCode::InvalidArgument,
            "invalid/duplicate frontend or frozen registry"};
  definitions_.push_back(std::move(definition));
  return Status::success();
}
const FrontendDefinition* FrontendRegistry::find(
    const std::string& name) const noexcept {
  for (const auto& definition : definitions_)
    if (definition.name == name)
      return &definition;
  return nullptr;
}
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
}  // namespace ps
