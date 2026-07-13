#pragma once

#include <cstdint>

#include "photospider/scheduler/scheduler.hpp"

/**
 * @file scheduler_plugin_api.hpp
 * @brief Version-gated exports for the transitional scheduler plugin ABI.
 *
 * Each plugin explicitly defines every required `extern "C"` entry. This SDK
 * intentionally provides no declaration or implementation macro because those
 * macros hid lifecycle and export requirements.
 */

#if defined(_WIN32) && defined(PHOTOSPIDER_SCHEDULER_PLUGIN_BUILD)
#define PHOTOSPIDER_SCHEDULER_PLUGIN_EXPORT __declspec(dllexport)
#elif defined(_WIN32)
#define PHOTOSPIDER_SCHEDULER_PLUGIN_EXPORT
#elif defined(PHOTOSPIDER_SCHEDULER_PLUGIN_BUILD)
#define PHOTOSPIDER_SCHEDULER_PLUGIN_EXPORT \
  __attribute__((visibility("default")))
#else
#define PHOTOSPIDER_SCHEDULER_PLUGIN_EXPORT
#endif

namespace ps {

/**
 * @brief First explicit numeric scheduler SDK generation.
 *
 * @throws Nothing.
 * @note Exact equality gates known Photospider interface generations. It does
 *       not remove the requirement for compatible compiler, standard library,
 *       RTTI, exception runtime, and C++ ABI.
 */
inline constexpr std::uint32_t PS_SCHEDULER_PLUGIN_ABI_VERSION = 1U;

/** @brief Exact ABI handshake symbol resolved before every other export. */
inline constexpr char kSchedulerPluginGetAbiVersionSymbol[] =
    "ps_scheduler_plugin_get_abi_version";  // NOLINT(whitespace/indent_namespace)

/** @brief Scheduler type-count export symbol. */
inline constexpr char kSchedulerPluginGetCountSymbol[] =
    "ps_scheduler_plugin_get_count";  // NOLINT(whitespace/indent_namespace)

/** @brief Scheduler indexed-name export symbol. */
inline constexpr char kSchedulerPluginGetNameSymbol[] =
    "ps_scheduler_plugin_get_name";  // NOLINT(whitespace/indent_namespace)

/** @brief Scheduler indexed-description export symbol. */
inline constexpr char kSchedulerPluginGetDescriptionSymbol[] =
    "ps_scheduler_plugin_get_description";  // NOLINT(whitespace/indent_namespace)

/** @brief Scheduler instance-creation export symbol. */
inline constexpr char kSchedulerPluginCreateSymbol[] =
    "ps_scheduler_plugin_create";  // NOLINT(whitespace/indent_namespace)

/** @brief Scheduler instance-destruction export symbol. */
inline constexpr char kSchedulerPluginDestroySymbol[] =
    "ps_scheduler_plugin_destroy";  // NOLINT(whitespace/indent_namespace)

/** @brief Human-readable implementation-version export symbol. */
inline constexpr char kSchedulerPluginGetVersionSymbol[] =
    "ps_scheduler_plugin_get_version";  // NOLINT(whitespace/indent_namespace)

/**
 * @brief Non-throwing numeric ABI handshake function type.
 * @throws Nothing by ABI contract.
 * @note The loader invokes this export before resolving or calling any other
 *       plugin entry.
 */
using SchedulerPluginGetAbiVersionFunc = std::uint32_t (*)() noexcept;

/**
 * @brief Scheduler type-count function type.
 * @throws Plugin implementation exceptions locally; the host loader converts
 * them to host-owned exceptions while the candidate DSO remains mapped.
 * @note The fixed-width result bounds subsequent indexed metadata queries and
 *       must be at least one for a loadable scheduler plugin.
 */
using SchedulerPluginGetCountFunc = std::uint32_t (*)();

/**
 * @brief Indexed scheduler name function type.
 * @throws Plugin implementation exceptions locally; the host loader converts
 * them to host-owned exceptions while the candidate DSO remains mapped.
 * @note Every index below the reported count must return a stable non-null,
 *       non-empty type name. Text is plugin-owned and copied before DSO
 *       release.
 */
using SchedulerPluginGetNameFunc = const char* (*)(std::uint32_t index);

/**
 * @brief Indexed scheduler description function type.
 * @throws Plugin implementation exceptions locally; the host loader converts
 * them to host-owned exceptions while the candidate DSO remains mapped.
 * @note Returned text is plugin-owned and must be copied before DSO release.
 */
using SchedulerPluginGetDescriptionFunc = const char* (*)(std::uint32_t index);

/**
 * @brief Scheduler instance creation function type.
 * @throws Plugin construction exceptions locally; the host owner converts them
 * to host-owned exceptions while the DSO remains mapped.
 * @note A non-null result transfers ownership only to the matching destroy
 *       export; the host retains the DSO through destruction.
 */
using SchedulerPluginCreateFunc = IScheduler* (*)(const char* type_name,
                                                  std::uint32_t num_workers);

/**
 * @brief Scheduler instance destruction function type.
 * @throws Nothing under the plugin contract; host cleanup fences hostile
 *         implementations that throw.
 * @note The export receives only instances created by the same DSO.
 */
using SchedulerPluginDestroyFunc = void (*)(IScheduler* scheduler);

/**
 * @brief Human-readable implementation-version function type.
 * @throws Plugin implementation exceptions locally; the host loader converts
 * them to host-owned exceptions during compatible candidate staging.
 * @note The loader calls this once, copies the result, and never treats it as
 *       an ABI compatibility gate.
 */
using SchedulerPluginGetVersionFunc = const char* (*)();

}  // namespace ps
