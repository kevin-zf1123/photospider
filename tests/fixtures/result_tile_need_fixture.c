#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>

#include "photospider/plugin/result_tile_helpers.h"

static const ps_result_tensor_spec_v7 tensor = {
    .struct_size = sizeof(tensor),
    .key = "number",
    .key_size = 6,
    .element_type = PS_RESULT_ELEMENT_FLOAT64_V7,
    .rank = 1,
    .shape = {1},
    .channel_axis = PS_RESULT_NO_CHANNEL_V7};
static const ps_result_schema_v7 schema = {
    .struct_size = sizeof(schema),
    .id = "test.c_tile_need",
    .id_size = sizeof("test.c_tile_need") - 1,
    .version = 1,
    .tensors = &tensor,
    .tensor_count = 1};
static const ps_result_port_v7 input = {.struct_size = sizeof(input),
                                        .tensor_key = "number",
                                        .tensor_key_size = 6};
static const ps_result_output_v7 outputs[] = {
    {.struct_size = sizeof(ps_result_output_v7),
     .key = "value",
     .key_size = 5,
     .port = {.struct_size = sizeof(ps_result_port_v7), .schema = &schema},
     .input_count = UINT32_MAX},
    {.struct_size = sizeof(ps_result_output_v7),
     .key = "independent",
     .key_size = 11,
     .port = {.struct_size = sizeof(ps_result_port_v7), .schema = &schema},
     .input_count = 0}};
static const ps_result_parameter_descriptor_v7 mode = {
    sizeof(mode), "mode", 4, PS_RESULT_PARAMETER_INT64_V7, 1, 1, 0, 28};
struct State {
  uint32_t stage;
};
static void (*observer)(void*, int);
static void* observer_context;
PS_RESULT_EXPORT void ps_result_tile_need_observe(void (*callback)(void*, int),
                                                  void* context) {
  observer = callback;
  observer_context = context;
}

static atomic_uint_fast64_t input_starts;
PS_RESULT_EXPORT uint64_t ps_result_tile_need_starts(void) {
  return atomic_load(&input_starts);
}
static int start(void* user, void* raw, const ps_result_query_v7* query,
                 const ps_result_services_v7* host) {
  atomic_fetch_add(&input_starts, 1);
  (void)user;
  (void)host;
  ((struct State*)raw)->stage = 0;
  if (query->input_count != 1 || !query->input_tile_grids)
    return 6;
  const ps_result_tile_grids_v7* members = query->input_tile_grids;
  if (members->struct_size != sizeof(*members) || members->count != 1 ||
      !members->members)
    return 6;
  const ps_result_tile_grid_v7* grid = members->members;
  if (query->parameters[0].int64_value >= 25 &&
      query->parameters[0].int64_value <= 27)
    return ps_result_tile_grid_validate_v7(
        grid, query->inputs[0].schema->tensors, query->tile_extent);
  return grid->struct_size == sizeof(*grid) && grid->rank == 5 &&
                 grid->batch_rank == 2 && grid->height_axis == 2 &&
                 grid->width_axis == 3 && grid->tile_extent == 2 &&
                 grid->tile_rows == 1 && grid->tile_columns == 3 &&
                 grid->tile_count == 12 && grid->shape[0] == 2 &&
                 grid->shape[1] == 2 && grid->shape[2] == 1 &&
                 grid->shape[3] == 5 && grid->shape[4] == 2
             ? 0
             : 6;
}
static int poll(void* user, void* raw, const ps_result_query_v7* query,
                const ps_result_services_v7* host) {
  (void)user;
  const int64_t selected = query->parameters[0].int64_value;
  if (!((struct State*)raw)->stage++) {
    ps_result_tile_interval_v7 indices[8192] = {{7, 8}, {6, 7}, {6, 7}};
    ps_result_tile_need_v7 need = {sizeof(need), 0, 0, 0, indices, 3};
    int status = 0;
    if (selected == 1 || selected == 20 || selected == 21 || selected == 22) {
      ps_result_region_v7 region = {.struct_size = sizeof(region),
                                    .rank = 5,
                                    .offset = {1, 0, 0, 1, 1},
                                    .extent = {1, 1, 1, 1, 1}};
      if (selected == 20)
        region.offset[3] = 5;
      if (selected == 21)
        region.extent[3] = 0;
      if (selected == 22) {
        region.offset[0] = region.offset[1] = 0;
        region.extent[0] = region.extent[1] = 2;
        region.offset[3] = 3;
        region.extent[3] = 2;
      }
      status = ps_result_need_regions_v7(host, query, 0, 0, &region, 1);
    } else if (selected == 2 || selected == 14 || selected == 26) {
      status = ps_result_to_whole_v7(host, query, 0);
    } else if (selected == 3 || selected == 13 || selected == 27) {
      status = ps_result_need_descriptor_v7(host, 0);
    } else if (selected == 18) {
      status = host->need_tiles(host->context, NULL);
    } else if (selected == 19) {
      status = host->need_tiles(
          host->context,
          (const ps_result_tile_need_v7*)((const uint8_t*)&need + 1));
    } else {
      switch (selected) {
        case 4:
          need.tensor_slot = need.descriptor_only = 1;
          need.intervals = NULL;
          need.interval_count = 0;
          break;
        case 5:
          need.descriptor_only = 1;
          break;
        case 6:
          need.intervals = NULL;
          need.interval_count = 0;
          break;
        case 7:
          indices[0] = (ps_result_tile_interval_v7){12, 13};
          break;
        case 8:
          need.interval_count = 0;
          break;
        case 9:
          need.intervals = NULL;
          break;
        case 10:
          need.descriptor_only = 2;
          break;
        case 11:
          need.struct_size = 0;
          break;
        case 12:
          need.input = 1;
          break;
        case 15:
          need.interval_count = 65537;
          break;
        case 16:
          need.interval_count = 1;
          indices[0] = (ps_result_tile_interval_v7){6, 7};
          break;
        case 17:
          need.tensor_slot = 1;
          indices[0] = (ps_result_tile_interval_v7){12, 13};
          break;
        case 24:
          need.intervals =
              (const ps_result_tile_interval_v7*)((const uint8_t*)indices + 1);
          break;
        case 28:
          need.interval_count = 8192;
          break;
        default:
          break;
      }
      if (selected == 28 && observer)
        observer(observer_context, -1);
      status = host->need_tiles(host->context, &need);
      if (selected == 28 && observer)
        observer(observer_context, status);
      if (!status && (selected == 16 || selected == 23)) {
        // NOLINTBEGIN(readability/braces): C compound literal assignment.
        indices[0] = selected == 23 ? (ps_result_tile_interval_v7){12, 13}
                                    : (ps_result_tile_interval_v7){7, 8};
        // NOLINTEND
        status = host->need_tiles(host->context, &need);
        if (selected == 23 && observer)
          observer(observer_context, status);
      }
      // NOLINTBEGIN(readability/braces): C compound literal assignment.
      indices[0] = indices[1] = indices[2] =
          (ps_result_tile_interval_v7){UINT64_MAX, 0};
      // NOLINTEND
    }
    // Wire errors must remain sticky even if a callback ignores their status.
    return (selected == 20 || selected == 21) && status ? status
                                                        : PS_RESULT_NEED_V7;
  }
  double value = 0;
  if (selected == 6 || selected == 21) {
    value = 42;
  } else if (selected == 3 || selected == 27) {
    ps_result_descriptor_v7 descriptor = {.struct_size = sizeof(descriptor)};
    if (host->result_descriptor(host->context, 0, &descriptor))
      return 1;
    value = descriptor.tensor_count;
  } else {
    uint64_t at[] = {1, 0, 0, 0, 0};
    if (selected == 2 || selected == 26) {
      at[1] = 1;
      at[3] = 4;
      at[4] = 1;
    } else if (selected == 22) {
      at[3] = 3;
    }
    if (host->read_tensor(host->context, 0, 0, at, 5, &value, sizeof(value)))
      return 1;
  }
  const ps_result_region_v7 region = {.struct_size = sizeof(region),
                                      .rank = 1,
                                      .extent = {1}};
  if (host->begin_result(host->context) ||
      host->publish_tensor(host->context, 0, &region, (const uint8_t*)&value,
                           sizeof(value)) ||
      host->publish_result(host->context, 1))
    return 1;
  return PS_RESULT_PUBLISH_V7;
}
static void destroy(void* user, void* state) {
  (void)user;
  (void)state;
}
static void destroy_api(void* user) {
  (void)user;
}
static const ps_result_tensor_spec_v7 query_tensors[] = {
    {.struct_size = sizeof(ps_result_tensor_spec_v7),
     .key = "image",
     .key_size = 5,
     .element_type = PS_RESULT_ELEMENT_FLOAT64_V7,
     .rank = 3,
     .shape = {2, 3, 5},
     .batch_rank = 2,
     .batch_shape = {2, 2},
     .spatial = 1,
     .height_axis = 1,
     .width_axis = 2,
     .channel_axis = 0}};
static const ps_result_schema_v7 query_schemas[] = {
    {.struct_size = sizeof(ps_result_schema_v7),
     .id = "test.c_tile_query",
     .id_size = sizeof("test.c_tile_query") - 1,
     .version = 1,
     .tensors = query_tensors,
     .tensor_count = 1}};
static const ps_result_output_v7 query_outputs[] = {
    {.struct_size = sizeof(ps_result_output_v7),
     .key = "tiled",
     .key_size = 5,
     .port = {.struct_size = sizeof(ps_result_port_v7),
              .schema = query_schemas}}};
static atomic_uint_fast64_t query_starts;
PS_RESULT_EXPORT uint64_t ps_result_tile_query_starts(uint32_t output) {
  return output == 0 ? atomic_load(&query_starts) : UINT64_MAX;
}
static int query_start(void* user, void* raw, const ps_result_query_v7* query,
                       const ps_result_services_v7* host) {
  (void)user;
  (void)raw;
  (void)host;
  if (query->output_index == 0)
    atomic_fetch_add(&query_starts, 1);
  if (query->output_index != 0 || query->input_count ||
      query->input_tile_grids || !query->output_tile_grids ||
      query->tile_extent != 2 || query->output_tile_grids->count != 1)
    return 6;
  const ps_result_tile_grid_v7* grid = query->output_tile_grids->members;
  if (!grid || grid->height_axis != 3 || grid->width_axis != 4 ||
      grid->tile_count != 24)
    return 6;
  return 0;
}
static int query_poll(void* user, void* raw, const ps_result_query_v7* query,
                      const ps_result_services_v7* host) {
  (void)user;
  (void)raw;
  if (host->begin_result(host->context))
    return 1;
  {
    const uint32_t slot = query->tensor_slot;
    ps_result_region_v7 region;
    if (ps_result_output_region_v7(query, slot, &region))
      return 6;
    const ps_result_tensor_spec_v7* spec =
        query->output->port.schema->tensors + slot;
    uint64_t count = 1;
    for (uint32_t axis = 0; axis < region.rank; ++axis)
      count *= region.extent[axis];
    if (count > 120)
      return 6;
    double values[120];
    for (uint64_t i = 0; i < count; ++i) {
      uint64_t remaining = i, at[8] = {0}, flat = 0;
      for (uint32_t axis = region.rank; axis > 0;) {
        --axis;
        at[axis] = region.offset[axis] + remaining % region.extent[axis];
        remaining /= region.extent[axis];
      }
      for (uint32_t axis = 0; axis < region.rank; ++axis) {
        const uint64_t bound = axis < spec->batch_rank
                                   ? spec->batch_shape[axis]
                                   : spec->shape[axis - spec->batch_rank];
        flat = flat * bound + at[axis];
      }
      values[i] = (double)(flat + 1);
    }
    if (host->publish_tensor(host->context, slot, &region,
                             (const uint8_t*)values, count * sizeof(double)))
      return 1;
  }
  return host->publish_result(host->context, 1) ? 1 : PS_RESULT_PUBLISH_V7;
}
static int static_mapping(void* user, const ps_result_static_query_v7* query,
                          const ps_result_static_mapping_sink_v7* sink) {
  (void)user;
  if (query->output_index != 0)
    return 0;
  const int64_t selected = query->parameters[0].int64_value;
  ps_result_tile_interval_v7 intervals[4] = {{6, 8}};
  ps_result_tile_need_v7 need = {sizeof(need), 0, 0, 0, intervals, 1};
  if (selected == 1) {
    intervals[0].end = 7;
  } else if (selected == 2 || selected == 26) {
    intervals[0].begin = 0;
    intervals[0].end = query->input_tile_grids[0].members[0].tile_count;
  } else if (selected == 3 || selected == 13 || selected == 27) {
    need.descriptor_only = 1;
    need.intervals = NULL;
    need.interval_count = 0;
  } else if (selected == 22) {
    for (uint32_t i = 0; i < 4; ++i) {
      intervals[i].begin = 3 * i + 1;
      intervals[i].end = 3 * i + 3;
    }
    need.interval_count = 4;
  } else if (selected != 0 && selected != 16 && selected != 25) {
    return 0;
  }
  return sink->add_input(sink->context, &need);
}
static const ps_result_operation_v7 operations[] = {
    {.struct_size = sizeof(ps_result_operation_v7),
     .key = "test.c_tile_need",
     .key_size = sizeof("test.c_tile_need") - 1,
     .flags = PS_RESULT_FLAG_CPU_V7 | PS_RESULT_FLAG_DETERMINISTIC_V7 |
              PS_RESULT_FLAG_SIDE_EFFECT_FREE_V7,
     .inputs = &input,
     .input_count = 1,
     .outputs = outputs,
     .output_count = 2,
     .parameters = &mode,
     .parameter_count = 1,
     .state_bytes = sizeof(struct State),
     .workspace_bytes = 65536,
     .maximum_stages = 2,
     .start = start,
     .poll = poll,
     .destroy = destroy,
     .map_dependencies = static_mapping},
    {.struct_size = sizeof(ps_result_operation_v7),
     .key = "test.c_tile_query",
     .key_size = sizeof("test.c_tile_query") - 1,
     .flags = PS_RESULT_FLAG_CPU_V7 | PS_RESULT_FLAG_DETERMINISTIC_V7 |
              PS_RESULT_FLAG_SIDE_EFFECT_FREE_V7,
     .outputs = query_outputs,
     .output_count = 1,
     .workspace_bytes = 65536,
     .maximum_stages = 1,
     .start = query_start,
     .poll = query_poll,
     .destroy = destroy}};
static const ps_result_operation_plugin_api_v7 api = {
    .struct_size = sizeof(api),
    .abi_version = PS_RESULT_OPERATION_ABI_VERSION_7,
    .operations = operations,
    .operation_count = 2,
    .destroy = destroy_api};
PS_RESULT_EXPORT const ps_result_operation_plugin_api_v7*
ps_result_operation_plugin_get_api_v7(void) {
  return &api;
}
