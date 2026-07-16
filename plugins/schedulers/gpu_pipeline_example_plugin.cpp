// Photospider SDK-only heterogeneous scheduler example plugin.

#include <cstdint>
#include <cstring>
#include <string>

#include "photospider/scheduler/scheduler_plugin_api.hpp"
#include "schedulers/sdk_example_scheduler.hpp"

namespace {

/** @brief First stable type alias exported by this plugin. */
constexpr char kGpuTypeName[] = "gpu_pipeline_example";

/** @brief Second stable type alias exported by this plugin. */
constexpr char kHeterogeneousTypeName[] = "heterogeneous_example";

/** @brief Human-readable description copied by the host. */
// NOLINTBEGIN(whitespace/indent_namespace)
constexpr char kDescription[] =
    "Example: heterogeneous scheduler implemented only with the installed "
    "Photospider scheduler SDK; advertises Metal when the host reports it.";
// NOLINTEND

/**
 * @brief Tests whether a requested type belongs to this plugin.
 * @param type_name Borrowed requested type, or null.
 * @return True for either stable alias.
 * @throws Nothing.
 */
bool supports_type(const char* type_name) noexcept {
  return type_name != nullptr &&
         (std::strcmp(type_name, kGpuTypeName) == 0 ||
          std::strcmp(type_name, kHeterogeneousTypeName) == 0);
}

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
 * @brief Reports the two pipeline aliases exported by this DSO.
 * @return Two.
 * @throws Nothing.
 * @note The fixed-width result is consumed only after ABI validation.
 */
PHOTOSPIDER_SCHEDULER_PLUGIN_EXPORT std::uint32_t
ps_scheduler_plugin_get_count() {
  return 2U;
}

/**
 * @brief Returns one stable scheduler alias.
 * @param index Zero-based type index.
 * @return Alias for indices zero and one, otherwise null.
 * @throws Nothing.
 */
PHOTOSPIDER_SCHEDULER_PLUGIN_EXPORT const char* ps_scheduler_plugin_get_name(
    std::uint32_t index) {
  if (index == 0U) {
    return kGpuTypeName;
  }
  if (index == 1U) {
    return kHeterogeneousTypeName;
  }
  return nullptr;
}

/**
 * @brief Returns the scheduler description at an index.
 * @param index Zero-based type index.
 * @return Stable description for either alias, otherwise null.
 * @throws Nothing.
 */
PHOTOSPIDER_SCHEDULER_PLUGIN_EXPORT const char*
ps_scheduler_plugin_get_description(std::uint32_t index) {
  return index < 2U ? kDescription : nullptr;
}

/**
 * @brief Creates one heterogeneous SDK example scheduler.
 * @param type_name Requested stable alias.
 * @param num_workers Resolved ABI v2 hard grant in `[1,8]`.
 * @return Plugin-owned instance, or null for an unsupported type.
 * @throws std::invalid_argument if a supported type receives a grant outside
 * `[1,8]`.
 * @throws std::bad_alloc if instance or worker configuration storage fails.
 * @note A successful instance owns exactly `num_workers` standard workers and
 *       no dedicated device worker. The built-in GPU scheduler's conservative
 *       grant-plus-one accounting does not alter this plugin ABI argument.
 */
PHOTOSPIDER_SCHEDULER_PLUGIN_EXPORT ps::IScheduler* ps_scheduler_plugin_create(
    const char* type_name, std::uint32_t num_workers) {
  if (!supports_type(type_name)) {
    return nullptr;
  }
  return new ps::scheduler_example::SdkExampleScheduler(
      type_name, ps::scheduler_example::ExamplePolicy::Heterogeneous,
      num_workers);
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
