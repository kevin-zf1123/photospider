#include <memory>
#include <stdexcept>
#include <string>
#include <utility>

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
}  // namespace ps
