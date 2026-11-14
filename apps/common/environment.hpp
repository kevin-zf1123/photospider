#pragma once
#include <memory>
#include <string>
#include <vector>

#include "common/file_io.hpp"
#include "photospider/plugin/operation_registry.hpp"
namespace ps::tools {
std::shared_ptr<OperationRegistry> load_operations(
    const std::vector<std::string>& libraries, Files* files,
    const std::function<Status(OperationRegistry&)>& configure = {});
void list_operations(const OperationRegistry& registry);
}  // namespace ps::tools
