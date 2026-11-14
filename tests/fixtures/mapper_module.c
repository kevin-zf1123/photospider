#include "photospider/plugin/operation_module_api.h"
static ps_kernel_status_v8 local_map(void* context,
                                     const ps_mapping_query_v1* query,
                                     const ps_mapping_sink_v1* sink) {
  (void)context;
  if (!query->source_count)
    return (ps_kernel_status_v8){
        .code = PS_KERNEL_TYPE_MISMATCH_V8};  // NOLINT(readability/braces)
  (void)sink->interval(sink->context, query->source_count + 1, 0, 1);
  const ps_kernel_status_v8 success = {0};
  return success;
}
static const ps_mapper_v1 mapper = {
    sizeof(mapper), 1, {"bad-sink", 8}, {"fixture", 7}, 1, 1, 0, local_map};
static void destroy(void* p) {
  (void)p;
}
PS_OPERATION_MODULE_EXPORT const ps_operation_module_v10*
ps_operation_module_get_api_v10(void) {
  static const ps_operation_module_v10 table = {
      sizeof(table),
      PS_OPERATION_MODULE_ABI_VERSION_10,
      0,
      0,
      0,
      destroy,
      &mapper,
      1};
  return &table;
}
