#include "photospider/plugin/operation_module_api.h"
const ps_mapper_v1* geometry_mapper_definitions();
extern "C" PS_OPERATION_MODULE_EXPORT const ps_operation_module_v10*
ps_operation_module_get_api_v10(void) {
  static const ps_operation_module_v10 api{
      sizeof(api),  PS_OPERATION_MODULE_ABI_VERSION_10, nullptr, 0, nullptr,
      [](void*) {}, geometry_mapper_definitions(),      2};
  return &api;
}
