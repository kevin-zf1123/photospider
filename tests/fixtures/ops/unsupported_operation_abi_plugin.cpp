#include <cstdint>
#include <cstdio>
#include <cstdlib>

#include "photospider/plugin/operation_plugin_api.h"

namespace {

/** @brief Environment variable naming the forbidden API-call marker file. */
constexpr const char* kInvocationMarkerEnvironment{
    "PS_UNSUPPORTED_OPERATION_ABI_MARKER",
};

/**
 * @brief Records an erroneous attempt to negotiate the unsupported ABI root.
 * @return Nothing.
 * @throws Nothing; marker I/O is deliberately best effort.
 * @note The loader must reject the numeric ABI version before resolving or
 * calling this exported root entry.
 */
void mark_api_invocation() noexcept {
  const char* path = std::getenv(kInvocationMarkerEnvironment);
  if (path == nullptr || path[0] == '\0') {
    return;
  }
  if (std::FILE* marker = std::fopen(path, "ab")) {
    (void)std::fputs("api_called\n", marker);
    (void)std::fclose(marker);
  }
}

}  // namespace

/**
 * @brief Advertises a future unsupported operation ABI generation.
 * @return Numeric version two, which the ABI-v1-only loader must reject.
 * @throws Nothing.
 */
extern "C" PS_OPERATION_PLUGIN_EXPORT std::uint32_t PS_OPERATION_CALL
ps_operation_plugin_get_abi_version(void) noexcept {
  return 2U;
}

/**
 * @brief Marks any loader violation that calls the root after version reject.
 * @param api_out Ignored Host destination.
 * @return Unsupported status.
 * @throws Nothing.
 * @note This fixture intentionally advertises version two and must never reach
 * this function in an ABI-v1-only Host.
 */
extern "C" PS_OPERATION_PLUGIN_EXPORT ps_operation_status_v1 PS_OPERATION_CALL
ps_operation_plugin_get_api_v1(ps_operation_plugin_api_v1* api_out) noexcept {
  (void)api_out;
  mark_api_invocation();
  return PS_OPERATION_STATUS_UNSUPPORTED_V1;
}
