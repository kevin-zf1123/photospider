// Photospider SDK-only serial scheduler example plugin.

#include <cstdint>
#include <cstring>
#include <string>

#include "photospider/scheduler/scheduler_plugin_api.hpp"
#include "schedulers/sdk_example_scheduler.hpp"

namespace {

/** @brief Stable type name exported by this plugin. */
constexpr char kTypeName[] = "serial_debug_example";

/** @brief Human-readable scheduler description copied by the host. */
// NOLINTBEGIN(whitespace/indent_namespace)
constexpr char kDescription[] =
    "Example: single-threaded serial scheduler implemented only with the "
    "installed Photospider scheduler SDK.";
// NOLINTEND

}  // namespace

extern "C" {

/**
 * @brief Reports the exact scheduler SDK generation used by this DSO.
 * @return `PS_SCHEDULER_PLUGIN_ABI_VERSION`.
 * @throws Nothing.
 */
PHOTOSPIDER_SCHEDULER_PLUGIN_EXPORT std::uint32_t
ps_scheduler_plugin_get_abi_version() noexcept {
  return ps::PS_SCHEDULER_PLUGIN_ABI_VERSION;
}

/**
 * @brief Reports one exported scheduler type.
 * @return One.
 * @throws Nothing.
 */
PHOTOSPIDER_SCHEDULER_PLUGIN_EXPORT std::uint32_t
ps_scheduler_plugin_get_count() {
  return 1U;
}

/**
 * @brief Returns the scheduler type at an index.
 * @param index Zero-based type index.
 * @return `serial_debug_example` for index zero, otherwise null.
 * @throws Nothing.
 */
PHOTOSPIDER_SCHEDULER_PLUGIN_EXPORT const char* ps_scheduler_plugin_get_name(
    std::uint32_t index) {
  return index == 0U ? kTypeName : nullptr;
}

/**
 * @brief Returns the scheduler description at an index.
 * @param index Zero-based type index.
 * @return Stable description for index zero, otherwise null.
 * @throws Nothing.
 */
PHOTOSPIDER_SCHEDULER_PLUGIN_EXPORT const char*
ps_scheduler_plugin_get_description(std::uint32_t index) {
  return index == 0U ? kDescription : nullptr;
}

/**
 * @brief Creates one serial SDK example scheduler.
 * @param type_name Requested type name.
 * @param num_workers Resolved ABI v2 hard grant in `[1,8]` retained by the
 * instance.
 * @return Plugin-owned instance, or null for an unsupported type.
 * @throws std::invalid_argument if a supported type receives a grant outside
 * `[1,8]`.
 * @throws std::bad_alloc if instance or type-name storage cannot allocate.
 * @note Serial execution owns zero worker threads, which is permitted because
 *       a plugin may own fewer than its grant. The host must release the result
 *       through the matching destroy export.
 */
PHOTOSPIDER_SCHEDULER_PLUGIN_EXPORT ps::IScheduler* ps_scheduler_plugin_create(
    const char* type_name, std::uint32_t num_workers) {
  if (type_name == nullptr || std::strcmp(type_name, kTypeName) != 0) {
    return nullptr;
  }
  return new ps::scheduler_example::SdkExampleScheduler(
      kTypeName, ps::scheduler_example::ExamplePolicy::Serial, num_workers);
}

/**
 * @brief Destroys one instance created by this DSO.
 * @param scheduler Plugin-owned scheduler, or null.
 * @return Nothing.
 * @throws Nothing under the scheduler destructor contract.
 */
PHOTOSPIDER_SCHEDULER_PLUGIN_EXPORT void ps_scheduler_plugin_destroy(
    ps::IScheduler* scheduler) {
  delete scheduler;
}

/**
 * @brief Returns the human-readable implementation version.
 * @return Stable version string copied once by the loader.
 * @throws Nothing.
 */
PHOTOSPIDER_SCHEDULER_PLUGIN_EXPORT const char*
ps_scheduler_plugin_get_version() {
  return "2.0.0";
}

}  // extern "C"
