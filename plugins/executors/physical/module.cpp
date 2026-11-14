#include "sdk/execution/provider.hpp"

extern "C" PS_OPERATION_MODULE_EXPORT const ps_executor_module_v2*
ps_executor_module_get_api_v2(void) {
  return &ps::executor_sdk::physical_api();
}
