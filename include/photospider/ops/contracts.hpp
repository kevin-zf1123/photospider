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
/** @brief Finite Float32 scalar selected by scalar_range.tensor_slot (Int64,
 * default 0), with inclusive scalar_range.minimum/maximum (Float64).
 * The three parameters are part of the operation identity. Preparation rejects
 * nonfinite/reversed bounds and non-scalar members; runtime checks only the
 * explicitly authorized [1] sample. Register via register_ops_contracts().
 */
inline ExtensionIdentity scalar_range_contract() {
  return {"ops.scalar-range", 1, "finite-f32-inclusive-v1"};
}
inline ExtensionIdentity bake_report_contract() {
  return {"ops.bake-report", 2, "measured-fields-v2"};
}
inline ExtensionIdentity bake_table_contract() {
  return {"ops.bake-table", 1, "color-tuples-v1"};
}
}  // namespace ps
