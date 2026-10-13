#pragma once

#include <cstdint>
#include <functional>
#include <string_view>

#include "photospider/data/result.hpp"

namespace ps {
/** @brief Whether a schema uses the Layer or LUT3D publication contract. */
PHOTOSPIDER_API bool has_representation_schema(std::string_view id) noexcept;
/** @brief Layer and LUT3D schema validation, called by
 * SchemaTemplate::validate. Unrelated user schemas return success. Does not
 * recursively call validate.
 */
PHOTOSPIDER_API Status
validate_representation_schema(const SchemaTemplate& schema);
/** @brief Bounded validation performed by the coordinator before a
 * recognized complete publication becomes observable. Uses the supplied root
 * for windows and work and requires matching root provenance. Unknown schemas
 * are left to their registered operation. Recognized families require complete
 * bundles; no future validation may revoke an observed prefix.
 * consume_work is an optional additional admission hook; root work is always
 * charged independently, including when that hook fails.
 */
PHOTOSPIDER_API Status validate_representation(
    const ResultRef& result, const ResourceBudget& resources,
    std::uint64_t maximum_window, const CancellationToken& cancellation = {},
    const std::function<Status(std::uint64_t)>& consume_work = {});
}  // namespace ps
