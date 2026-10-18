#include <algorithm>
#include <cmath>
#include <map>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>

#include "compiler/optimizer_internal.hpp"
#include "photospider/compiler/optimizer.hpp"

namespace ps {
namespace {
bool bounded(const ParameterValue& value) {
  if (const auto* text = std::get_if<std::string>(&value))
    return text->size() <= 8192;
  if (const auto* number = std::get_if<double>(&value))
    return std::isfinite(*number);
  return true;
}
bool numeric(const ParameterValue& value) {
  return std::holds_alternative<std::int64_t>(value) ||
         std::holds_alternative<double>(value);
}
bool accepts(const OptimizerOption& schema, const ParameterValue& value) {
  if (value.index() != schema.default_value.index() || !bounded(value))
    return false;
  if (schema.minimum && value < *schema.minimum)
    return false;
  if (schema.maximum && value > *schema.maximum)
    return false;
  return schema.choices.empty() ||
         std::find(schema.choices.begin(), schema.choices.end(), value) !=
             schema.choices.end();
}
bool valid_schema(const OptimizerOption& schema) {
  for (const auto* bound : {&schema.minimum, &schema.maximum})
    if (*bound && (!numeric(**bound) ||
                   (**bound).index() != schema.default_value.index() ||
                   !bounded(**bound)))
      return false;
  if (schema.minimum && schema.maximum && *schema.maximum < *schema.minimum)
    return false;
  if (schema.choices.size() > 1024)
    return false;
  for (const auto& choice : schema.choices)
    if (choice.index() != schema.default_value.index() || !bounded(choice) ||
        (schema.minimum && choice < *schema.minimum) ||
        (schema.maximum && choice > *schema.maximum))
      return false;
  return accepts(schema, schema.default_value);
}
}  // namespace
Result<std::map<std::string, ParameterValue>>
OptimizerDefinition::resolve_options(
    const std::map<std::string, ParameterValue>& overrides) const {
  using Options = std::map<std::string, ParameterValue>;
  Options resolved;
  for (const auto& item : option_schema)
    resolved.emplace(item.first, item.second.default_value);
  for (const auto& item : overrides) {
    auto found = option_schema.find(item.first);
    if (found == option_schema.end() || !accepts(found->second, item.second))
      return Result<Options>({ErrorCode::InvalidArgument,
                              "unknown optimizer option, incorrect type, or "
                              "value outside schema: " +
                                  item.first});
    resolved[item.first] = item.second;
  }
  return Result<Options>(std::move(resolved));
}
Status OptimizerRegistry::add(OptimizerDefinition definition) {
  if (frozen_ || definition.name.empty() || definition.version.empty() ||
      !definition.transform || find(definition.name) ||
      definition.name.size() > 1024 || definition.version.size() > 128 ||
      definition.option_schema.size() > 1024 ||
      definition.supported_ir_forms.empty())
    return {ErrorCode::InvalidArgument,
            "invalid/duplicate optimizer or frozen registry"};
  std::set<IRForm> forms;
  for (auto form : definition.supported_ir_forms)
    if ((form != IRForm::DependencyPaths && form != IRForm::ControlFlow) ||
        !forms.insert(form).second)
      return {ErrorCode::InvalidArgument,
              "invalid/duplicate optimizer IR form"};
  for (const auto& item : definition.option_schema)
    if (item.first.empty() || item.first.size() > 1024 ||
        !valid_schema(item.second))
      return {ErrorCode::InvalidArgument,
              "invalid optimizer option schema: " + item.first};
  definitions_.push_back(std::move(definition));
  return Status::success();
}
const OptimizerDefinition* OptimizerRegistry::find(
    const std::string& name) const noexcept {
  for (const auto& definition : definitions_)
    if (definition.name == name)
      return &definition;
  return nullptr;
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
