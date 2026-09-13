#pragma once

#include "photospider/data/representation.hpp"
#include "photospider/plugin/operation_registry.hpp"

namespace ps {
enum class ComponentOperation : std::uint32_t { Labels = 1, Area, Filter };
/** @brief Sorted paged Int64[id,area] index on a fixed min-pixel label basis.
 * Runtime row count may be zero. The producer binds exactly one complete
 * Components ObjectId; filter checks that association and all index rows
 * against the associated Components table before using the index. Metadata
 * retains HW, maximum_count and MinPixel scheme. No missing property is
 * interpreted as zero.
 */
PHOTOSPIDER_API Result<SchemaTemplate> component_area_schema(
    const ComponentsSpec& spec);
/** @brief Complete HW UInt8 binary filter result with flattened mask rows.
 * Output is one iff label!=0 and its associated area>=minimum_area. The
 * required Int64 threshold is positive. An empty component set produces
 * all-zero pixels.
 */
PHOTOSPIDER_API Result<SchemaTemplate> component_filter_schema(
    const ComponentsSpec& spec);
/** @brief Ready-to-register components4.labels/area/filter CPU operations.
 * Labels takes facet-free UInt8 HW, where nonzero is foreground. It evaluates
 * four-connectivity over the entire original raster using left/top edges and
 * rank union on a mandatory 32-byte {parent,rank,minimum,area} record per
 * pixel. Resident source, union and output windows are bounded; provisional
 * capacity is charged independently of final component count. IDs are 1+minimum
 * raster position (components_min_pixel_v1), with background zero. This does
 * not alter the existing mask.components compact-label operation. Area consumes
 * complete Components and builds a paged sorted index; Filter takes Components
 * plus that index and required Int64 minimum_area. Filter verifies the
 * association and uses bounded binary-search windows. Missing or inconsistent
 * properties fail; dynamic zero count is valid. All outputs use CompleteBundle
 * and Conservative support and own their backing/associations. Only
 * ComponentIdScheme::MinPixel is supported here. Work, stages, windows,
 * temporary backing and maximum_count can fail explicitly within root budgets;
 * managed capacity is not a process RSS bound. The representation's count/basis
 * validator alone does not establish connectivity of arbitrary imported labels.
 */
PHOTOSPIDER_API Result<OperationDefinition> make_component_operation(
    ComponentOperation operation, const ComponentsSpec& spec);
}  // namespace ps
