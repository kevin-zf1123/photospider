#pragma once
#include "photospider/plugin/operation_registry.hpp"
namespace ps {
/** @brief Add missing bundled static geometry solvers to a mutable catalog.
 * Call after explicit module registration and before freeze. */
Status register_geometry_mappers(OperationRegistry& registry);
}  // namespace ps
