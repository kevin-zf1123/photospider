#include <atomic>
#include <cstdint>

#include "photospider/plugin/result_operation_plugin_api.h"

namespace {
std::atomic<std::uint32_t> calls{0};
// NOLINTBEGIN(whitespace/indent_namespace)
const ps_result_operation_plugin_api_v2 api{sizeof(api), 1,       nullptr,
                                            0,           nullptr, nullptr};
// NOLINTEND
}  // namespace

extern "C" PS_RESULT_EXPORT const ps_result_operation_plugin_api_v2*
ps_result_operation_plugin_get_api_v2(void) {
  ++calls;
  return &api;
}

extern "C" PS_RESULT_EXPORT std::uint32_t ps_bad_operation_api_calls(void) {
  return calls.load();
}
