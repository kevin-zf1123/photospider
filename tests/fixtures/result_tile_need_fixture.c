#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>

#include "photospider/plugin/result_tile_helpers.h"

static const ps_result_tensor_spec_v3 tensor = {
    .struct_size = sizeof(tensor),
    .key = "number",
    .key_size = 6,
    .element_type = PS_RESULT_ELEMENT_FLOAT64_V3,
    .rank = 1,
    .shape = {1},
    .channel_axis = PS_RESULT_NO_CHANNEL_V3};
static const ps_result_schema_v3 schema = {
    .struct_size = sizeof(schema),
    .id = "test.c_tile_need",
    .id_size = sizeof("test.c_tile_need") - 1,
    .version = 1,
    .publication = PS_RESULT_COMPLETE_BUNDLE_V3,
    .tensors = &tensor,
    .tensor_count = 1};
static const ps_result_port_v3 input = {.struct_size = sizeof(input),
                                        .kind = PS_RESULT_OBJECT_V3,
                                        .tensor_key = "number",
                                        .tensor_key_size = 6};
static const ps_result_output_v3 outputs[] = {
    {.struct_size = sizeof(ps_result_output_v3),
     .key = "value",
     .key_size = 5,
     .port = {.struct_size = sizeof(ps_result_port_v3),
              .kind = PS_RESULT_OBJECT_V3,
              .schema = &schema},
     .input_count = UINT32_MAX,
     .execution = PS_RESULT_WHOLE_V3},
    {.struct_size = sizeof(ps_result_output_v3),
     .key = "independent",
     .key_size = 11,
     .port = {.struct_size = sizeof(ps_result_port_v3),
              .kind = PS_RESULT_OBJECT_V3,
              .schema = &schema},
     .input_count = 0,
     .execution = PS_RESULT_WHOLE_V3}};
static const ps_result_parameter_descriptor_v3 mode = {
    sizeof(mode), "mode", 4, PS_RESULT_PARAMETER_INT64_V3, 1, 1, 0, 28};
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
static int start(void* user, void* raw, const ps_result_query_v3* query,
                 const ps_result_services_v3* host) {
  atomic_fetch_add(&input_starts, 1);
  (void)user;
  (void)host;
  ((struct State*)raw)->stage = 0;
  if (query->input_count != 1 || !query->input_tile_grids)
    return 6;
  const ps_result_tile_grid_v3* grid = query->input_tile_grids;
  if (query->parameters[0].int64_value >= 25 &&
      query->parameters[0].int64_value <= 27) {
    if (grid->struct_size != sizeof(*grid) || grid->present || grid->rank ||
        grid->batch_rank || grid->height_axis || grid->width_axis ||
        grid->tile_extent || grid->tile_rows || grid->tile_columns ||
        grid->tile_count)
      return 6;
    for (uint32_t i = 0; i < 8; ++i)
      if (grid->shape[i])
        return 6;
    return 0;
  }
  return grid->struct_size == sizeof(*grid) && grid->present == 1 &&
                 grid->rank == 5 && grid->batch_rank == 2 &&
                 grid->height_axis == 2 && grid->width_axis == 3 &&
                 grid->tile_extent == 2 && grid->tile_rows == 1 &&
                 grid->tile_columns == 3 && grid->tile_count == 12 &&
                 grid->shape[0] == 2 && grid->shape[1] == 2 &&
                 grid->shape[2] == 1 && grid->shape[3] == 5 &&
                 grid->shape[4] == 2
             ? 0
             : 6;
}
static int poll(void* user, void* raw, const ps_result_query_v3* query,
                const ps_result_services_v3* host) {
  (void)user;
  const int64_t selected = query->parameters[0].int64_value;
  if (!((struct State*)raw)->stage++) {
    uint64_t indices[8192] = {7, 6, 6};
    ps_result_tile_need_v3 need = {sizeof(need), 0, 0, 0, indices, 3};
    int status = 0;
    if (selected == 1 || selected == 20 || selected == 21 || selected == 22) {
      ps_result_region_v3 region = {.struct_size = sizeof(region),
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
      status = ps_result_need_regions_v3(host, query, 0, &region, 1);
    } else if (selected == 2 || selected == 14 || selected == 26) {
      status = ps_result_need_whole_v3(host, 0);
    } else if (selected == 3 || selected == 13 || selected == 27) {
      status = ps_result_need_descriptor_v3(host, 0);
    } else if (selected == 18) {
      status = host->need_tiles(host->context, NULL);
    } else if (selected == 19) {
      status = host->need_tiles(
          host->context,
          (const ps_result_tile_need_v3*)((const uint8_t*)&need + 1));
    } else {
      switch (selected) {
        case 4:
          need.whole = need.descriptor_only = 1;
          need.tiles = NULL;
          need.tile_count = 0;
          break;
        case 5:
          need.whole = 1;
          break;
        case 6:
          need.tiles = NULL;
          need.tile_count = 0;
          break;
        case 7:
          indices[0] = 12;
          break;
        case 8:
          need.tile_count = 0;
          break;
        case 9:
          need.tiles = NULL;
          break;
        case 10:
          need.whole = 2;
          break;
        case 11:
          need.struct_size = 0;
          break;
        case 12:
          need.input = 1;
          break;
        case 15:
          need.tile_count = 65537;
          break;
        case 16:
          need.tile_count = 1;
          indices[0] = 6;
          break;
        case 17:
          need.whole = 1;
          indices[0] = 12;
          break;
        case 24:
          need.tiles = (const uint64_t*)((const uint8_t*)indices + 1);
          break;
        case 28:
          need.tile_count = 8192;
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
        indices[0] = selected == 23 ? 12 : 7;
        status = host->need_tiles(host->context, &need);
        if (selected == 23 && observer)
          observer(observer_context, status);
      }
      indices[0] = indices[1] = indices[2] = UINT64_MAX;
    }
    // Wire errors must remain sticky even if a callback ignores their status.
    return (selected == 20 || selected == 21) && status ? status
                                                        : PS_RESULT_NEED_V3;
  }
  double value = 0;
  if (selected == 3 || selected == 27) {
    ps_result_descriptor_v3 descriptor = {.struct_size = sizeof(descriptor)};
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
  const ps_result_region_v3 region = {.struct_size = sizeof(region),
                                      .rank = 1,
                                      .extent = {1}};
  if (host->begin_result(host->context) ||
      host->publish_tensor(host->context, 0, &region, (const uint8_t*)&value,
                           sizeof(value)) ||
      host->publish_result(host->context, 1))
    return 1;
  return PS_RESULT_PUBLISH_V3;
}
static void destroy(void* user, void* state) {
  (void)user;
  (void)state;
}
static void destroy_api(void* user) {
  (void)user;
}
static const ps_result_tensor_spec_v3 query_tensors[] = {
    {.struct_size = sizeof(ps_result_tensor_spec_v3),
     .key = "image",
     .key_size = 5,
     .element_type = PS_RESULT_ELEMENT_FLOAT64_V3,
     .rank = 3,
     .shape = {2, 3, 5},
     .batch_rank = 2,
     .batch_shape = {2, 2},
     .spatial = 1,
     .height_axis = 1,
     .width_axis = 2,
     .channel_axis = 0},
    {.struct_size = sizeof(ps_result_tensor_spec_v3),
     .key = "number",
     .key_size = 6,
     .element_type = PS_RESULT_ELEMENT_FLOAT64_V3,
     .rank = 1,
     .shape = {1},
     .channel_axis = PS_RESULT_NO_CHANNEL_V3}};
static const ps_result_schema_v3 query_schemas[] = {
    {.struct_size = sizeof(ps_result_schema_v3),
     .id = "test.c_tile_query",
     .id_size = sizeof("test.c_tile_query") - 1,
     .version = 1,
     .publication = PS_RESULT_COMPLETE_BUNDLE_V3,
     .tensors = query_tensors,
     .tensor_count = 1},
    {.struct_size = sizeof(ps_result_schema_v3),
     .id = "test.c_tile_bundle",
     .id_size = sizeof("test.c_tile_bundle") - 1,
     .version = 1,
     .publication = PS_RESULT_COMPLETE_BUNDLE_V3,
     .tensors = query_tensors,
     .tensor_count = 2}};
static const ps_result_output_v3 query_outputs[] = {
    {.struct_size = sizeof(ps_result_output_v3),
     .key = "tiled",
     .key_size = 5,
     .port = {.struct_size = sizeof(ps_result_port_v3),
              .kind = PS_RESULT_OBJECT_V3,
              .schema = query_schemas},
     .execution = PS_RESULT_REGIONAL_V3},
    {.struct_size = sizeof(ps_result_output_v3),
     .key = "whole",
     .key_size = 5,
     .port = {.struct_size = sizeof(ps_result_port_v3),
              .kind = PS_RESULT_OBJECT_V3,
              .schema = query_schemas},
     .execution = PS_RESULT_WHOLE_V3},
    {.struct_size = sizeof(ps_result_output_v3),
     .key = "bundle",
     .key_size = 6,
     .port = {.struct_size = sizeof(ps_result_port_v3),
              .kind = PS_RESULT_OBJECT_V3,
              .schema = query_schemas + 1},
     .execution = PS_RESULT_REGIONAL_V3}};
static atomic_uint_fast64_t query_starts[3];
PS_RESULT_EXPORT uint64_t ps_result_tile_query_starts(uint32_t output) {
  return output < 3 ? atomic_load(query_starts + output) : UINT64_MAX;
}
static int query_start(void* user, void* raw, const ps_result_query_v3* query,
                       const ps_result_services_v3* host) {
  (void)user;
  (void)raw;
  (void)host;
  if (query->output_index < 3)
    atomic_fetch_add(query_starts + query->output_index, 1);
  if (query->output_index >= 3 || query->input_count ||
      query->input_tile_grids || !query->output_tile_grid ||
      query->tile_extent != 2 || query->tiled != (query->output_index == 0) ||
      query->output_tile_grid->present != (query->output_index != 2))
    return 6;
  if (query->output_index == 0) {
    if (query->requested.whole || query->requested.tile_count != 1 ||
        query->requested.tiles[0] != query->tile_unit ||
        query->output_tile_grid->height_axis != 3 ||
        query->output_tile_grid->width_axis != 4 ||
        query->output_tile_grid->tile_count != 24)
      return 6;
  } else if (!query->requested.whole || query->requested.tile_count ||
             query->tile_unit) {
    return 6;
  }
  return 0;
}
static int query_poll(void* user, void* raw, const ps_result_query_v3* query,
                      const ps_result_services_v3* host) {
  (void)user;
  (void)raw;
  if (host->begin_result(host->context))
    return 1;
  for (uint32_t slot = 0; slot < query->output->port.schema->tensor_count;
       ++slot) {
    ps_result_region_v3 region;
    if (ps_result_output_region_v3(query, slot, &region))
      return 6;
    const ps_result_tensor_spec_v3* spec =
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
      values[i] = slot ? 42 : (double)(flat + 1);
    }
    if (host->publish_tensor(host->context, slot, &region,
                             (const uint8_t*)values, count * sizeof(double)))
      return 1;
  }
  return host->publish_result(host->context, 1) ? 1 : PS_RESULT_PUBLISH_V3;
}
static const ps_result_operation_v3 operations[] = {
    {.struct_size = sizeof(ps_result_operation_v3),
     .key = "test.c_tile_need",
     .key_size = sizeof("test.c_tile_need") - 1,
     .flags = PS_RESULT_FLAG_CPU_V3 | PS_RESULT_FLAG_DETERMINISTIC_V3 |
              PS_RESULT_FLAG_SIDE_EFFECT_FREE_V3,
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
     .destroy = destroy},
    {.struct_size = sizeof(ps_result_operation_v3),
     .key = "test.c_tile_query",
     .key_size = sizeof("test.c_tile_query") - 1,
     .flags = PS_RESULT_FLAG_CPU_V3 | PS_RESULT_FLAG_DETERMINISTIC_V3 |
              PS_RESULT_FLAG_SIDE_EFFECT_FREE_V3,
     .outputs = query_outputs,
     .output_count = 3,
     .workspace_bytes = 65536,
     .maximum_stages = 1,
     .start = query_start,
     .poll = query_poll,
     .destroy = destroy}};
static const ps_result_operation_plugin_api_v3 api = {
    .struct_size = sizeof(api),
    .abi_version = PS_RESULT_OPERATION_ABI_VERSION_3,
    .operations = operations,
    .operation_count = 2,
    .destroy = destroy_api};
PS_RESULT_EXPORT const ps_result_operation_plugin_api_v3*
ps_result_operation_plugin_get_api_v3(void) {
  return &api;
}
