#include "photospider/plugin/operation_module_api.h"
/* A real module with only the former query symbol. The current loader must
 * reject it before calling any entry. */
PS_OPERATION_MODULE_EXPORT const void* ps_operation_module_get_api_v8(void) {
  return 0;
}
PS_OPERATION_MODULE_EXPORT const void* ps_operation_module_get_api_v9(void) {
  return 0;
}
PS_OPERATION_MODULE_EXPORT const void* ps_executor_module_get_api_v1(void) {
  return 0;
}
