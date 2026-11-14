#include <memory>
#include <stdexcept>

#include "photospider/ops/registry.hpp"
#include "photospider/plugin/geometry_mappers.hpp"
namespace ps {
std::shared_ptr<OperationRegistry> make_default_operation_registry(
    bool freeze) {
  auto registry = make_runtime_operation_registry(false);
  auto status = register_geometry_mappers(*registry);
  if (status.ok() && freeze)
    status = registry->freeze();
  if (!status.ok())
    throw std::logic_error(status.message);
  return registry;
}
}  // namespace ps
