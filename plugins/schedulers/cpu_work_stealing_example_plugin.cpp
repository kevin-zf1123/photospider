// Photospider SDK-only CPU worker scheduler example plugin.

#include <cstdint>
#include <cstring>
#include <string>

#include "photospider/scheduler/scheduler_plugin_api.hpp"
#include "schedulers/sdk_example_scheduler.hpp"

namespace {

/** @brief Stable type name exported by this plugin. */
constexpr char kTypeName[] = "cpu_work_stealing_example";

/** @brief Human-readable scheduler description copied by the host. */
// NOLINTBEGIN(whitespace/indent_namespace)
constexpr char kDescription[] =
    "Example: multi-threaded CPU scheduler implemented only with the installed "
    "Photospider scheduler SDK.";
// NOLINTEND

}  // namespace

extern "C" {

/**
 * @brief Reports the exact scheduler SDK generation used by this DSO.
 * @return `PS_SCHEDULER_PLUGIN_ABI_VERSION`.
 * @throws Nothing.
 * @note The loader resolves and validates this symbol before every other
 * export.
 */
PHOTOSPIDER_SCHEDULER_PLUGIN_EXPORT std::uint32_t
ps_scheduler_plugin_get_abi_version() noexcept {
  return ps::PS_SCHEDULER_PLUGIN_ABI_VERSION;
}

/**
 * @brief Reports the single CPU scheduler type exported by this DSO.
 * @return One.
 * @throws Nothing.
 * @note The fixed-width result is consumed only after ABI validation.
 */
PHOTOSPIDER_SCHEDULER_PLUGIN_EXPORT std::uint32_t
ps_scheduler_plugin_get_count() {
  return 1U;
}

/**
 * @brief Returns the scheduler type at an index.
 * @param index Zero-based type index.
 * @return Stable type name for index zero, otherwise null.
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
 * @brief Creates one CPU-worker SDK example scheduler.
 * @param type_name Requested type name.
 * @param num_workers Worker count, or zero for hardware concurrency.
 * @return Plugin-owned instance, or null for an unsupported type.
 * @throws std::bad_alloc if instance or worker configuration storage fails.
 */
PHOTOSPIDER_SCHEDULER_PLUGIN_EXPORT ps::IScheduler* ps_scheduler_plugin_create(
    const char* type_name, std::uint32_t num_workers) {
  if (type_name == nullptr || std::strcmp(type_name, kTypeName) != 0) {
    return nullptr;
  }
  return new ps::scheduler_example::SdkExampleScheduler(
      kTypeName, ps::scheduler_example::ExamplePolicy::CpuWorkers, num_workers);
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
 * @return Process-lifetime version literal copied once by the loader.
 * @throws Nothing.
 * @note This string is descriptive and does not replace numeric ABI gating.
 */
PHOTOSPIDER_SCHEDULER_PLUGIN_EXPORT const char*
ps_scheduler_plugin_get_version() {
  return "2.0.0";
}

}  // extern "C"
