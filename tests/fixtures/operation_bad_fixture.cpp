#include <atomic>
#include <cstdint>

#include "photospider/plugin/operation_module_api.h"

namespace {
std::atomic<std::uint32_t> calls{0};
// NOLINTBEGIN(whitespace/indent_namespace)
const ps_operation_module_v8 api{sizeof(api), 1, nullptr, 0, nullptr, nullptr};
// NOLINTEND
}  // namespace

extern "C" PS_OPERATION_MODULE_EXPORT const ps_operation_module_v8*
ps_operation_module_get_api_v8(void) {
  ++calls;
  return &api;
}

extern "C" PS_OPERATION_MODULE_EXPORT std::uint32_t ps_bad_operation_api_calls(
    void) {
  return calls.load();
}
