#ifndef INCLUDE_PHOTOSPIDER_PLUGIN_RESULT_TILE_HELPERS_H_
#define INCLUDE_PHOTOSPIDER_PLUGIN_RESULT_TILE_HELPERS_H_

#include <stddef.h>
#include <stdint.h>

#include "photospider/plugin/result_operation_plugin_api.h"

/** @brief Validate a borrowed grid against its resolved Result geometry.
 * Checks the single-spatial-tensor/no-fields condition, exact axes, shape,
 * extent, clipped-grid counts and count overflow. Absent grids have zero
 * geometry. No allocation or pixel access occurs; work is bounded by rank 8.
 * Returns InvalidArgument for inconsistent metadata and otherwise success.
 */
static inline int ps_result_tile_grid_validate_v4(
    const ps_result_tile_grid_v4* grid, const ps_result_schema_v4* schema,
    uint64_t tile_extent) {
  if (!grid || grid->struct_size != sizeof(*grid) || !schema ||
      schema->struct_size != sizeof(*schema) || grid->present > 1 ||
      !tile_extent || (tile_extent & (tile_extent - 1)) ||
      (schema->tensor_count && !schema->tensors))
    return PS_RESULT_STATUS_INVALID_ARGUMENT_V4;
  const int spatial = schema->tensor_count == 1 && !schema->field_count &&
                      schema->tensors[0].spatial;
  if (!spatial) {
    if (grid->present || grid->rank || grid->batch_rank || grid->height_axis ||
        grid->width_axis || grid->tile_extent || grid->tile_rows ||
        grid->tile_columns || grid->tile_count)
      return PS_RESULT_STATUS_INVALID_ARGUMENT_V4;
    for (uint32_t axis = 0; axis < 8; ++axis)
      if (grid->shape[axis])
        return PS_RESULT_STATUS_INVALID_ARGUMENT_V4;
    return PS_RESULT_STATUS_OK_V4;
  }
  const ps_result_tensor_spec_v4* tensor = schema->tensors;
  if (tensor->struct_size != sizeof(*tensor) || tensor->spatial != 1 ||
      tensor->batch_rank > 8 || tensor->rank > 8 - tensor->batch_rank ||
      tensor->height_axis >= tensor->rank ||
      tensor->width_axis >= tensor->rank ||
      tensor->height_axis == tensor->width_axis || !grid->present ||
      grid->rank != tensor->batch_rank + tensor->rank ||
      grid->batch_rank != tensor->batch_rank ||
      grid->tile_extent != tile_extent ||
      grid->height_axis != tensor->batch_rank + tensor->height_axis ||
      grid->width_axis != tensor->batch_rank + tensor->width_axis)
    return PS_RESULT_STATUS_INVALID_ARGUMENT_V4;
  uint64_t count = 1;
  for (uint32_t axis = 0; axis < 8; ++axis) {
    const uint64_t bound = axis >= grid->rank ? 0
                           : axis < tensor->batch_rank
                               ? tensor->batch_shape[axis]
                               : tensor->shape[axis - tensor->batch_rank];
    if (grid->shape[axis] != bound)
      return PS_RESULT_STATUS_INVALID_ARGUMENT_V4;
    if (axis < tensor->batch_rank) {
      if (bound && count > UINT64_MAX / bound)
        return PS_RESULT_STATUS_INVALID_ARGUMENT_V4;
      count *= bound;
    }
  }
  const uint64_t height = grid->shape[grid->height_axis];
  const uint64_t width = grid->shape[grid->width_axis];
  const uint64_t rows = height / tile_extent + (height % tile_extent != 0);
  const uint64_t columns = width / tile_extent + (width % tile_extent != 0);
  if (grid->tile_rows != rows || grid->tile_columns != columns ||
      (rows && count > UINT64_MAX / rows))
    return PS_RESULT_STATUS_INVALID_ARGUMENT_V4;
  count *= rows;
  if (columns && count > UINT64_MAX / columns)
    return PS_RESULT_STATUS_INVALID_ARGUMENT_V4;
  count *= columns;
  return count == grid->tile_count ? PS_RESULT_STATUS_OK_V4
                                   : PS_RESULT_STATUS_INVALID_ARGUMENT_V4;
}

/** @brief Resolve this computation's output member to its logical rectangle.
 * Whole returns the complete selected tensor, including all batch/cell axes.
 * Tiled accepts only slot zero and returns one clipped grid tile. Empty returns
 * the tensor rank with all extents zero. No pixel, scratch or heap allocation
 * occurs; at most eight axes are inspected. The query is a callback borrow and
 * the caller owns `region`. An inconsistent grid, task or member selection
 * returns InvalidArgument without modifying `region`.
 */
static inline int ps_result_output_region_v4(const ps_result_query_v4* query,
                                             uint32_t slot,
                                             ps_result_region_v4* region) {
  if (!query || query->struct_size != sizeof(*query) || !region ||
      !query->output || !query->output->port.schema ||
      slot >= query->output->port.schema->tensor_count ||
      !query->output->port.schema->tensors || query->tiled > 1 ||
      (!query->tiled && query->tile_unit != 0) ||
      query->requested.struct_size != sizeof(query->requested) ||
      query->requested.whole > 1 || query->requested.tile_count > 1 ||
      ((query->requested.tile_count == 0) !=
       (query->requested.tiles == NULL)) ||
      (query->requested.whole && (query->requested.tile_count || query->tiled)))
    return PS_RESULT_STATUS_INVALID_ARGUMENT_V4;
  if (ps_result_tile_grid_validate_v4(query->output_tile_grid,
                                      query->output->port.schema,
                                      query->tile_extent) ||
      (query->output->execution != PS_RESULT_WHOLE_V4 &&
       query->output->execution != PS_RESULT_REGIONAL_V4) ||
      query->tiled != (query->output->execution == PS_RESULT_REGIONAL_V4 &&
                       query->output_tile_grid->present))
    return PS_RESULT_STATUS_INVALID_ARGUMENT_V4;
  const ps_result_tensor_spec_v4* tensor =
      query->output->port.schema->tensors + slot;
  if (tensor->batch_rank > 8 || tensor->rank > 8 - tensor->batch_rank)
    return PS_RESULT_STATUS_INVALID_ARGUMENT_V4;
  ps_result_region_v4 resolved = {0, 0, {0}, {0}};
  resolved.struct_size = sizeof(resolved);
  resolved.rank = tensor->rank + tensor->batch_rank;
  if (!query->requested.whole && !query->requested.tile_count) {
    *region = resolved;
    return PS_RESULT_STATUS_OK_V4;
  }
  for (uint32_t axis = 0; axis < resolved.rank; ++axis)
    resolved.extent[axis] = axis < tensor->batch_rank
                                ? tensor->batch_shape[axis]
                                : tensor->shape[axis - tensor->batch_rank];
  if (query->requested.whole) {
    *region = resolved;
    return PS_RESULT_STATUS_OK_V4;
  }
  const ps_result_tile_grid_v4* grid = query->output_tile_grid;
  if (!query->tiled || slot || !grid || grid->struct_size != sizeof(*grid) ||
      grid->present != 1 || grid->rank != resolved.rank ||
      grid->batch_rank != tensor->batch_rank || !grid->tile_extent ||
      grid->tile_extent != query->tile_extent || !grid->tile_rows ||
      !grid->tile_columns || query->tile_unit >= grid->tile_count ||
      query->requested.tiles[0] != query->tile_unit ||
      grid->height_axis < grid->batch_rank || grid->height_axis >= grid->rank ||
      grid->width_axis < grid->batch_rank || grid->width_axis >= grid->rank ||
      grid->height_axis == grid->width_axis)
    return PS_RESULT_STATUS_INVALID_ARGUMENT_V4;
  const uint64_t column = query->tile_unit % grid->tile_columns;
  const uint64_t row = query->tile_unit / grid->tile_columns % grid->tile_rows;
  uint64_t batch = query->tile_unit / grid->tile_columns / grid->tile_rows;
  for (uint32_t axis = 0; axis < grid->rank; ++axis)
    if (grid->shape[axis] != resolved.extent[axis])
      return PS_RESULT_STATUS_INVALID_ARGUMENT_V4;
  for (uint32_t axis = grid->batch_rank; axis > 0;) {
    --axis;
    if (!grid->shape[axis])
      return PS_RESULT_STATUS_INVALID_ARGUMENT_V4;
    resolved.offset[axis] = batch % grid->shape[axis];
    resolved.extent[axis] = 1;
    batch /= grid->shape[axis];
  }
  if (batch || row > UINT64_MAX / grid->tile_extent ||
      column > UINT64_MAX / grid->tile_extent)
    return PS_RESULT_STATUS_INVALID_ARGUMENT_V4;
  resolved.offset[grid->height_axis] = row * grid->tile_extent;
  resolved.offset[grid->width_axis] = column * grid->tile_extent;
  for (uint32_t axis = 0; axis < grid->rank; ++axis) {
    if (axis != grid->height_axis && axis != grid->width_axis)
      continue;
    if (resolved.offset[axis] >= grid->shape[axis])
      return PS_RESULT_STATUS_INVALID_ARGUMENT_V4;
    const uint64_t remaining = grid->shape[axis] - resolved.offset[axis];
    resolved.extent[axis] =
        remaining < grid->tile_extent ? remaining : grid->tile_extent;
  }
  *region = resolved;
  return PS_RESULT_STATUS_OK_V4;
}

/** @brief Request every member of one input Result. No input is prefetched.
 * Call during start/poll on its entry thread and propagate nonzero status.
 */
static inline int ps_result_need_whole_v4(const ps_result_services_v4* host,
                                          uint32_t input) {
  const ps_result_tile_need_v4 need = {sizeof(need), input, 1, 0, NULL, 0};
  return host->need_tiles(host->context, &need);
}
/** @brief Request global semantic descriptor facts, without payload access. */
static inline int ps_result_need_descriptor_v4(
    const ps_result_services_v4* host, uint32_t input) {
  const ps_result_tile_need_v4 need = {sizeof(need), input, 0, 1, NULL, 0};
  return host->need_tiles(host->context, &need);
}
/** @brief Convert operation-selected rectangles to explicit producer indices.
 * Compiled input grids are authoritative. All axes are bounds-checked; partial
 * cell axes select complete spatial tiles, while batch coordinates stay exact.
 * Inputs without a grid request Whole. Empty rectangles are ignored, but an
 * entirely empty payload selection is invalid. Duplicate tiles are normalized
 * by the host. Work and temporary indices use host services; scratch is
 * released on success and drained at callback retirement after a sticky
 * service failure. At most 65536 rectangles/expanded indices are accepted. This
 * helper must run on the callback entry thread. It retains no query, rectangle,
 * scratch, or service pointer. Propagate any nonzero return status.
 */
static inline int ps_result_need_regions_v4(const ps_result_services_v4* host,
                                            const ps_result_query_v4* query,
                                            uint32_t input,
                                            const ps_result_region_v4* regions,
                                            uint32_t count) {
  if (!host || !query || input >= query->input_count || !regions || !count ||
      count > 65536 || !query->input_tile_grids || !query->inputs ||
      !query->inputs[input].schema ||
      query->inputs[input].schema->tensor_count != 1)
    return PS_RESULT_STATUS_INVALID_ARGUMENT_V4;
  const ps_result_tile_grid_v4* grid = query->input_tile_grids + input;
  if (ps_result_tile_grid_validate_v4(grid, query->inputs[input].schema,
                                      query->tile_extent))
    return PS_RESULT_STATUS_INVALID_ARGUMENT_V4;
  const ps_result_tensor_spec_v4* tensor = query->inputs[input].schema->tensors;
  if (grid->struct_size != sizeof(*grid) || grid->present > 1 || !tensor ||
      tensor->batch_rank > 8 || tensor->rank > 8 - tensor->batch_rank)
    return PS_RESULT_STATUS_INVALID_ARGUMENT_V4;
  const uint32_t rank = tensor->rank + tensor->batch_rank;
  if (grid->present &&
      (!grid->tile_extent || grid->rank != rank ||
       grid->batch_rank != tensor->batch_rank || grid->height_axis >= rank ||
       grid->width_axis >= rank || grid->height_axis == grid->width_axis))
    return PS_RESULT_STATUS_INVALID_ARGUMENT_V4;
  int status =
      host->consume_work(host->context, UINT64_C(1) * count * (rank + 1));
  if (status)
    return status;
  uint64_t total = 0;
  for (uint32_t r = 0; r < count; ++r) {
    const ps_result_region_v4* box = regions + r;
    if (box->struct_size != sizeof(*box) || box->rank != rank)
      return PS_RESULT_STATUS_INVALID_ARGUMENT_V4;
    int empty = 0;
    for (uint32_t axis = 0; axis < rank; ++axis) {
      const uint64_t bound = grid->present ? grid->shape[axis]
                             : axis < tensor->batch_rank
                                 ? tensor->batch_shape[axis]
                                 : tensor->shape[axis - tensor->batch_rank];
      if (box->offset[axis] > bound ||
          box->extent[axis] > bound - box->offset[axis])
        return PS_RESULT_STATUS_INVALID_ARGUMENT_V4;
      empty |= box->extent[axis] == 0;
    }
    if (empty)
      continue;
    uint64_t units = 1;
    if (grid->present) {
      for (uint32_t axis = 0; axis < rank; ++axis) {
        uint64_t factor = 1;
        if (axis < grid->batch_rank)
          factor = box->extent[axis];
        else if (axis == grid->height_axis || axis == grid->width_axis)
          factor =
              (box->offset[axis] + box->extent[axis] - 1) / grid->tile_extent -
              box->offset[axis] / grid->tile_extent + 1;
        if (factor > 65536 / units)
          return PS_RESULT_STATUS_RESOURCE_EXHAUSTED_V4;
        units *= factor;
      }
    }
    if (units > 65536 - total)
      return PS_RESULT_STATUS_RESOURCE_EXHAUSTED_V4;
    total += units;
  }
  if (!total)
    return PS_RESULT_STATUS_INVALID_ARGUMENT_V4;
  if (!grid->present)
    return ps_result_need_whole_v4(host, input);
  status = host->consume_work(host->context, total * (grid->batch_rank + 1));
  if (status)
    return status;
  uint8_t* scratch = NULL;
  status =
      host->allocate_scratch(host->context, total * sizeof(uint64_t), &scratch);
  if (status)
    return status;
  uint64_t* indices = (uint64_t*)scratch;  // NOLINT(readability/casting)
  uint64_t cursor = 0;
  for (uint32_t r = 0; r < count; ++r) {
    const ps_result_region_v4* box = regions + r;
    int empty = 0;
    for (uint32_t axis = 0; axis < rank; ++axis)
      empty |= box->extent[axis] == 0;
    if (empty)
      continue;
    uint64_t batches = 1;
    for (uint32_t axis = 0; axis < grid->batch_rank; ++axis)
      batches *= box->extent[axis];
    const uint64_t first_row =
        box->offset[grid->height_axis] / grid->tile_extent;
    const uint64_t last_row =
        (box->offset[grid->height_axis] + box->extent[grid->height_axis] - 1) /
        grid->tile_extent;
    const uint64_t first_column =
        box->offset[grid->width_axis] / grid->tile_extent;
    const uint64_t last_column =
        (box->offset[grid->width_axis] + box->extent[grid->width_axis] - 1) /
        grid->tile_extent;
    for (uint64_t batch = 0; batch < batches; ++batch) {
      uint64_t remaining = batch, coordinates[8] = {0}, index = 0;
      for (uint32_t axis = grid->batch_rank; axis > 0;) {
        --axis;
        coordinates[axis] = box->offset[axis] + remaining % box->extent[axis];
        remaining /= box->extent[axis];
      }
      for (uint32_t axis = 0; axis < grid->batch_rank; ++axis)
        index = index * grid->shape[axis] + coordinates[axis];
      for (uint64_t row = first_row; row <= last_row; ++row)
        for (uint64_t column = first_column; column <= last_column; ++column)
          indices[cursor++] =
              (index * grid->tile_rows + row) * grid->tile_columns + column;
    }
  }
  const ps_result_tile_need_v4 need = {sizeof(need), input, 0, 0,
                                       indices,      total};
  status = host->need_tiles(host->context, &need);
  const int released = host->release_scratch(host->context, scratch);
  return status ? status : released;
}
#endif  // INCLUDE_PHOTOSPIDER_PLUGIN_RESULT_TILE_HELPERS_H_
