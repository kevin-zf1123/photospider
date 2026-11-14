#include "photospider/plugin/planner_module_api.h"

static const ps_kernel_status_v8 ok_status = {0};

static ps_kernel_status_v8 scalar(void* unused, const ps_planner_host_v1* h) {
  (void)unused;
  for (uint32_t i = 0; i < h->call_count; i++) {
    ps_plan_call_v1 c;
    ps_kernel_status_v8 s = h->call(h->context, i, &c);
    if (s.code)
      return s;
    ps_plan_access_v1 reads[2], writes[1];
    if (c.input_count > 2 || c.output_count != 1) {
      s.code = PS_KERNEL_TYPE_MISMATCH_V8;
      return s;
    }
    for (uint32_t j = 0; j < c.input_count; j++)
      reads[j] = (ps_plan_access_v1){j, c.inputs[j], 0, 0, 0};
    writes[0] = (ps_plan_access_v1){0, c.outputs[0], 0, 0, 0};
    ps_plan_task_v1 t = {0};
    t.struct_size = sizeof(t);
    t.call = i;
    t.invocation_abi = 8;
    const ps_string_v8 sum = {"sum", 3}, constant = {"constant", 8};
    t.symbol = c.input_count ? sum : constant;
    t.contract = (ps_string_v8){"scalar-v1", 9};
    t.placement.profile = (ps_string_v8){"cpu", 3};
    t.reads = reads;
    t.read_count = c.input_count;
    t.writes = writes;
    t.write_count = 1;
    uint32_t id = 0;
    s = h->append_task(h->context, &t, &id);
    if (s.code)
      return s;
    s = h->export_value(h->context, i, c.outputs[0]);
    if (s.code)
      return s;
  }
  return ok_status;
}
static void destroy(void* unused) {
  (void)unused;
}
PS_OPERATION_MODULE_EXPORT const ps_planner_module_v1*
ps_planner_module_get_api_v1(void) {
  static const ps_planner_module_v1 api = {sizeof(api), 1,
                                           3,           {"scalar", 6},
                                           {"1", 1},    {"scalar fixture", 14},
                                           0,           scalar,
                                           destroy};
  return &api;
}
