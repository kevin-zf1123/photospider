#include <utility>

#include "photospider/plugin/geometry_mappers.hpp"
#include "sdk/operation/operation_module.hpp"
const ps_mapper_v1* geometry_mapper_definitions();
namespace ps {
Status register_geometry_mappers(OperationRegistry& registry) {
  const ps_operation_module_v10 table{
      sizeof(table), PS_OPERATION_MODULE_ABI_VERSION_10, nullptr, 0, nullptr,
      [](void*) {},  geometry_mapper_definitions(),      2};
  auto definitions = plugin_internal::import_operation_module(&table, {});
  if (!definitions.ok())
    return definitions.status();
  for (auto& mapper : definitions.value().mappers) {
    if (registry.has_mapper(mapper.contract, mapper.provider))
      continue;
    auto status = registry.register_mapper(std::move(mapper));
    if (!status.ok())
      return status;
  }
  return Status::success();
}
}  // namespace ps
