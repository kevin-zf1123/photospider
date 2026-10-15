#pragma once
#include "photospider/ops/export.hpp"
#include "photospider/plugin/operation_registry.hpp"
namespace ps {
/** @brief Register reusable explicit operator contracts before registry freeze.
 * No schema identifiers or data labels select these validators. Operations
 * bind the returned stable identities through their declared port contracts.
 * Duplicate registration fails; callbacks retain no mutable global state.
 */
PHOTOSPIDER_OPS_SDK_API Status
register_ops_contracts(OperationRegistry& registry);
inline ExtensionIdentity rgba_contract() {
  return {"ops.rgba", 1, "finite-premultiplied-v1"};
}
inline ExtensionIdentity bake_report_contract() {
  return {"ops.bake-report", 1, "measured-fields-v1"};
}
inline ExtensionIdentity bake_table_contract() {
  return {"ops.bake-table", 1, "color-tuples-v1"};
}
}  // namespace ps
