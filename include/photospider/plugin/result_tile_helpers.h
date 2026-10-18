#ifndef INCLUDE_PHOTOSPIDER_PLUGIN_RESULT_TILE_HELPERS_H_
#define INCLUDE_PHOTOSPIDER_PLUGIN_RESULT_TILE_HELPERS_H_
#include <stddef.h>
#include <stdint.h>

#include "photospider/plugin/result_operation_plugin_api.h"

/** @brief Validate one member grid against static tensor geometry.
 * No allocation or payload access. Missing free axes use the rank sentinel.
 */
static inline int ps_result_tile_grid_validate_v7(
    const ps_result_tile_grid_v7* grid, const ps_result_tensor_spec_v7* tensor,
    uint64_t tile_extent) {
  if (!grid || !tensor || grid->struct_size != sizeof(*grid) ||
      tensor->struct_size != sizeof(*tensor) || !tile_extent ||
      (tile_extent & (tile_extent - 1)) || tensor->batch_rank > 8 ||
      !tensor->rank || tensor->rank > 8 - tensor->batch_rank ||
      tensor->atomic_trailing_axes > tensor->rank)
    return PS_RESULT_STATUS_INVALID_ARGUMENT_V7;
  const uint32_t rank = tensor->rank + tensor->batch_rank;
  const uint32_t free_rank = rank - tensor->atomic_trailing_axes;
  uint32_t y = rank, x = rank, mask = 0;
  if (tensor->spatial) {
    y = tensor->batch_rank + tensor->height_axis;
    x = tensor->batch_rank + tensor->width_axis;
    if (x >= free_rank || y >= free_rank || x == y)
      return PS_RESULT_STATUS_INVALID_ARGUMENT_V7;
  } else {
    if (free_rank > tensor->batch_rank)
      x = free_rank - 1;
    if (free_rank > tensor->batch_rank + 1)
      y = free_rank - 2;
  }
  uint64_t count = 1, rows = 1, columns = 1;
  for (uint32_t axis = 0; axis < rank; ++axis) {
    const uint64_t extent = axis < tensor->batch_rank
                                ? tensor->batch_shape[axis]
                                : tensor->shape[axis - tensor->batch_rank];
    if (!extent || grid->shape[axis] != extent)
      return PS_RESULT_STATUS_INVALID_ARGUMENT_V7;
    uint64_t factor = 1;
    if (axis == x || axis == y) {
      factor = extent / tile_extent + (extent % tile_extent != 0);
      if (axis == x)
        columns = factor;
      if (axis == y)
        rows = factor;
    } else if (axis < (tensor->spatial ? tensor->batch_rank : free_rank)) {
      factor = extent;
      mask |= 1U << axis;
    }
    if (factor > UINT64_MAX / count)
      return PS_RESULT_STATUS_INVALID_ARGUMENT_V7;
    count *= factor;
  }
  for (uint32_t axis = rank; axis < 8; ++axis)
    if (grid->shape[axis])
      return PS_RESULT_STATUS_INVALID_ARGUMENT_V7;
  return grid->rank == rank && grid->batch_rank == tensor->batch_rank &&
                 grid->height_axis == y && grid->width_axis == x &&
                 grid->batch_axis_mask == mask &&
                 grid->tile_extent == tile_extent && grid->tile_rows == rows &&
                 grid->tile_columns == columns && grid->tile_count == count
             ? PS_RESULT_STATUS_OK_V7
             : PS_RESULT_STATUS_INVALID_ARGUMENT_V7;
}
/** @brief Resolve the selected output member's complete tile rectangle. */
static inline int ps_result_output_region_v7(const ps_result_query_v7* query,
                                             uint32_t slot,
                                             ps_result_region_v7* region) {
  if (!query || !region || query->struct_size != sizeof(*query) ||
      query->shared_stage != UINT32_MAX || !query->output ||
      !query->output->port.schema || !query->output_tile_grids ||
      query->output_tile_grids->struct_size !=
          sizeof(*query->output_tile_grids) ||
      query->output_tile_grids->count !=
          query->output->port.schema->tensor_count ||
      slot != query->tensor_slot || slot >= query->output_tile_grids->count ||
      slot >= query->output->port.schema->tensor_count ||
      !query->output_tile_grids->members)
    return PS_RESULT_STATUS_INVALID_ARGUMENT_V7;
  const ps_result_tile_grid_v7* grid = query->output_tile_grids->members + slot;
  if (ps_result_tile_grid_validate_v7(
          grid, query->output->port.schema->tensors + slot,
          query->tile_extent) ||
      query->tile_unit >= grid->tile_count)
    return PS_RESULT_STATUS_INVALID_ARGUMENT_V7;
  ps_result_region_v7 result = {0, 0, {0}, {0}};
  result.struct_size = sizeof(result);
  result.rank = grid->rank;
  for (uint32_t axis = 0; axis < grid->rank; ++axis)
    result.extent[axis] = grid->shape[axis];
  uint64_t batch = query->tile_unit / grid->tile_columns / grid->tile_rows;
  for (uint32_t axis = grid->rank; axis-- > 0;) {
    if (!(grid->batch_axis_mask & (1U << axis)))
      continue;
    result.offset[axis] = batch % grid->shape[axis];
    result.extent[axis] = 1;
    batch /= grid->shape[axis];
  }
  const uint32_t axes[2] = {grid->height_axis, grid->width_axis};
  const uint64_t indices[2] = {
      query->tile_unit / grid->tile_columns % grid->tile_rows,
      query->tile_unit % grid->tile_columns};
  for (uint32_t i = 0; i < 2; ++i)
    if (axes[i] < grid->rank) {
      const uint32_t axis = axes[i];
      result.offset[axis] = indices[i] * grid->tile_extent;
      const uint64_t remaining = grid->shape[axis] - result.offset[axis];
      result.extent[axis] =
          remaining < grid->tile_extent ? remaining : grid->tile_extent;
    }
  *region = result;
  return PS_RESULT_STATUS_OK_V7;
}
/** @brief Request static descriptor facts without authorizing payload access.
 */
static inline int ps_result_need_descriptor_v7(
    const ps_result_services_v7* host, uint32_t input) {
  if (!host || !host->need_tiles)
    return PS_RESULT_STATUS_INVALID_ARGUMENT_V7;
  const ps_result_tile_need_v7 need = {sizeof(need), input, 0, 1, NULL, 0};
  return host->need_tiles(host->context, &need);
}
/** @brief Request all members using explicit compact tile intervals.
 * Nonblocking: return Need from the enclosing poll before reading payload.
 * The helper retains no query or service pointers and never collects bytes.
 */
static inline int ps_result_to_whole_v7(const ps_result_services_v7* host,
                                        const ps_result_query_v7* query,
                                        uint32_t input) {
  if (!host || !host->need_tiles || !query || input >= query->input_count ||
      !query->input_tile_grids || !query->inputs)
    return PS_RESULT_STATUS_INVALID_ARGUMENT_V7;
  const ps_result_tile_grids_v7* grids = query->input_tile_grids + input;
  const ps_result_schema_v7* schema = query->inputs[input].schema;
  if (!schema || grids->struct_size != sizeof(*grids) || !grids->members ||
      grids->count != schema->tensor_count || !grids->count ||
      grids->count > 16)
    return PS_RESULT_STATUS_INVALID_ARGUMENT_V7;
  for (uint32_t slot = 0; slot < grids->count; ++slot)
    if (ps_result_tile_grid_validate_v7(
            grids->members + slot, schema->tensors + slot, query->tile_extent))
      return PS_RESULT_STATUS_INVALID_ARGUMENT_V7;
  for (uint32_t slot = 0; slot < grids->count; ++slot) {
    const ps_result_tile_interval_v7 range = {0,
                                              grids->members[slot].tile_count};
    const ps_result_tile_need_v7 need = {sizeof(need), input, slot, 0,
                                         &range,       1};
    const int status = host->need_tiles(host->context, &need);
    if (status)
      return status;
  }
  return PS_RESULT_STATUS_OK_V7;
}
/** @brief Request tiles intersecting explicit rectangles of one member.
 * Atomic axes select the complete tuple. Contiguous rows and batches become
 * intervals. At most 65536 intervals are admitted before scratch allocation;
 * failure is returned before any Need is submitted. Caller propagates status.
 */
// C-compatible casts are required by this C11/C++17 public header.
// NOLINTBEGIN(readability/casting)
static inline int ps_result_need_regions_v7(const ps_result_services_v7* host,
                                            const ps_result_query_v7* query,
                                            uint32_t input, uint32_t slot,
                                            const ps_result_region_v7* regions,
                                            uint32_t count) {
  if (!host || !query || input >= query->input_count || !regions || !count ||
      count > 65536 || !query->input_tile_grids || !query->inputs ||
      !query->inputs[input].schema)
    return PS_RESULT_STATUS_INVALID_ARGUMENT_V7;
  const ps_result_tile_grids_v7* group = query->input_tile_grids + input;
  const ps_result_schema_v7* schema = query->inputs[input].schema;
  if (group->struct_size != sizeof(*group) || !group->members ||
      slot >= group->count || slot >= schema->tensor_count)
    return PS_RESULT_STATUS_INVALID_ARGUMENT_V7;
  const ps_result_tile_grid_v7* grid = group->members + slot;
  if (ps_result_tile_grid_validate_v7(grid, schema->tensors + slot,
                                      query->tile_extent))
    return PS_RESULT_STATUS_INVALID_ARGUMENT_V7;
  uint32_t axes[8], n = 0;
  uint64_t sizes[8], strides[8];
  for (uint32_t axis = 0; axis < grid->rank; ++axis)
    if (grid->batch_axis_mask & (1U << axis)) {
      axes[n] = axis;
      sizes[n++] = grid->shape[axis];
    }
  if (grid->height_axis < grid->rank) {
    axes[n] = grid->height_axis;
    sizes[n++] = grid->tile_rows;
  }
  if (grid->width_axis < grid->rank) {
    axes[n] = grid->width_axis;
    sizes[n++] = grid->tile_columns;
  }
  uint64_t stride = 1;
  for (uint32_t j = n; j-- > 0;) {
    strides[j] = stride;
    stride *= sizes[j];
  }
  uint64_t total = 0, written = 0;
  uint8_t* scratch = NULL;
  int status = host->consume_work(host->context,
                                  (uint64_t)count * (grid->rank + n + 1) *
                                      2);  // NOLINT(readability/casting)
  if (status)
    return status;
  for (uint32_t pass = 0; pass < 2; ++pass) {
    for (uint32_t r = 0; r < count; ++r) {
      const ps_result_region_v7* box = regions + r;
      if (box->struct_size != sizeof(*box) || box->rank != grid->rank)
        return PS_RESULT_STATUS_INVALID_ARGUMENT_V7;
      int empty = 0;
      for (uint32_t axis = 0; axis < grid->rank; ++axis) {
        if (box->offset[axis] > grid->shape[axis] ||
            box->extent[axis] > grid->shape[axis] - box->offset[axis])
          return PS_RESULT_STATUS_INVALID_ARGUMENT_V7;
        empty |= !box->extent[axis];
      }
      if (empty)
        continue;
      uint64_t begins[8], ends[8], at[8];
      for (uint32_t j = 0; j < n; ++j) {
        const uint32_t axis = axes[j];
        const uint64_t scale =
            grid->batch_axis_mask & (1U << axis) ? 1 : grid->tile_extent;
        begins[j] = box->offset[axis] / scale;
        ends[j] = (box->offset[axis] + box->extent[axis] - 1) / scale + 1;
        at[j] = begins[j];
      }
      uint32_t inner = n ? n - 1 : 0;
      while (inner && begins[inner] == 0 && ends[inner] == sizes[inner])
        --inner;
      uint64_t intervals = 1;
      for (uint32_t j = 0; j < inner; ++j) {
        if (ends[j] - begins[j] > 65536 / intervals)
          return PS_RESULT_STATUS_RESOURCE_EXHAUSTED_V7;
        intervals *= ends[j] - begins[j];
      }
      if (!pass) {
        if (intervals > 65536 - total)
          return PS_RESULT_STATUS_RESOURCE_EXHAUSTED_V7;
        total += intervals;
      } else {
        for (uint64_t i = 0; i < intervals; ++i) {
          uint64_t begin = 0;
          for (uint32_t j = 0; j < n; ++j)
            begin += at[j] * strides[j];
          ps_result_tile_interval_v7* out = (ps_result_tile_interval_v7*)
              scratch;  // NOLINT(readability/casting)
          out[written].begin = begin;
          out[written++].end =
              begin + (n ? (ends[inner] - begins[inner]) * strides[inner] : 1);
          for (uint32_t j = inner; j-- > 0;) {
            if (++at[j] < ends[j])
              break;
            at[j] = begins[j];
          }
        }
      }
    }
    if (!pass) {
      if (!total) {
        const ps_result_tile_need_v7 empty = {sizeof(empty), input, slot, 0,
                                              NULL,          0};
        return host->need_tiles(host->context, &empty);
      }
      status = host->consume_work(host->context, total * (n + 1));
      if (!status)
        status = host->allocate_scratch(
            host->context, total * sizeof(ps_result_tile_interval_v7),
            &scratch);
      if (status)
        return status;
    }
  }
  const ps_result_tile_need_v7 need = {
      sizeof(need), input, slot, 0, (const ps_result_tile_interval_v7*)scratch,
      written};
  status = host->need_tiles(host->context, &need);
  const int released = host->release_scratch(host->context, scratch);
  return status ? status : released;
}
// NOLINTEND
#endif  // INCLUDE_PHOTOSPIDER_PLUGIN_RESULT_TILE_HELPERS_H_
