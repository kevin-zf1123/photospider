// FILE: include/plugin_api.hpp
#pragma once

#include <stdexcept>
#include <string>
#include <utility>

#include "node.hpp"
#include "ps_types.hpp"

/**
 * @file plugin_api.hpp
 * @brief Operation plugin registration ABI used by dynamic plugins.
 *
 * Operation plugins export one C symbol,
 * `register_photospider_ops_v1(ps::OperationPluginRegistrar*)`. The host
 * creates the registrar, owns the real `OpRegistry`, and forwards each
 * registration call into the host process. Plugins therefore do not need to
 * locate or share the registry singleton across static-library and
 * dynamic-library boundaries.
 *
 * @note The callable signatures still use Photospider C++ value contracts such
 * as `NodeOutput`, `OpMetadata`, and `std::function`. This is a narrow
 * host-provided registration ABI, not a long-term pure C plugin ABI.
 */
/**
 * @brief Marks the operation plugin registration entry for export.
 *
 * Plugin targets define `PHOTOSPIDER_PLUGIN_BUILD`, causing the versioned
 * registration entry to be exported from the plugin dynamic library. Host
 * targets include this header only to learn the function pointer type and
 * symbol name, so they leave the declaration unannotated.
 *
 * @note Non-Windows builds add default visibility when the plugin build macro
 * is present. Existing builds without hidden visibility would export the symbol
 * anyway, but the explicit annotation documents ownership.
 */
#if defined(_WIN32) && defined(PHOTOSPIDER_PLUGIN_BUILD)
#define PLUGIN_API __declspec(dllexport)
#elif defined(_WIN32)
#define PLUGIN_API
#elif defined(PHOTOSPIDER_PLUGIN_BUILD)
#define PLUGIN_API __attribute__((visibility("default")))
#else
#define PLUGIN_API
#endif

namespace ps {

/**
 * @brief Host-provided operation registration callback table.
 *
 * The loader fills this table with callbacks that mutate the host-owned
 * `OpRegistry`. Plugin code receives a pointer to the registrar and calls its
 * member helpers instead of calling `OpRegistry::instance()` directly. The
 * helper methods validate that the expected host callback is present, forward
 * operation names as borrowed C strings, and move callback objects into the
 * host registry.
 *
 * @throws std::logic_error from helper methods if a plugin receives an
 * incomplete registrar table.
 * @note The registrar is borrowed and valid only during the
 * `register_photospider_ops_v1` call. Plugins must not store the registrar or
 * its `user_data` pointer for later use.
 */
struct OperationPluginRegistrar {
  /**
   * @brief Host callback for HP monolithic operation registration.
   *
   * @param user_data Opaque host context passed back unchanged.
   * @param type Operation type string, borrowed for the duration of the call.
   * @param subtype Operation subtype string, borrowed for the duration of the
   * call.
   * @param fn Monolithic callback to move into the host registry.
   * @param meta Metadata associated with the HP implementation.
   * @throws Exceptions from the host registry or callback storage may
   * propagate through the plugin registration entry point.
   * @note Plugin authors should call `register_op_hp_monolithic` instead of
   * invoking this callback directly.
   */
  using RegisterHpMonolithicFn = void (*)(void* user_data, const char* type,
                                          const char* subtype,
                                          MonolithicOpFunc fn, OpMetadata meta);

  /**
   * @brief Host callback for HP tiled operation registration.
   *
   * @param user_data Opaque host context passed back unchanged.
   * @param type Operation type string, borrowed for the duration of the call.
   * @param subtype Operation subtype string, borrowed for the duration of the
   * call.
   * @param fn Tiled callback to move into the host registry.
   * @param meta Metadata associated with the HP tiled implementation.
   * @throws Exceptions from the host registry or callback storage may
   * propagate through the plugin registration entry point.
   * @note Plugin authors should call `register_op_hp_tiled` instead of
   * invoking this callback directly.
   */
  using RegisterHpTiledFn = void (*)(void* user_data, const char* type,
                                     const char* subtype, TileOpFunc fn,
                                     OpMetadata meta);

  /**
   * @brief Host callback for RT tiled operation registration.
   *
   * @param user_data Opaque host context passed back unchanged.
   * @param type Operation type string, borrowed for the duration of the call.
   * @param subtype Operation subtype string, borrowed for the duration of the
   * call.
   * @param fn Tiled callback to move into the host registry.
   * @param meta Metadata associated with the RT tiled implementation.
   * @throws Exceptions from the host registry or callback storage may
   * propagate through the plugin registration entry point.
   * @note Plugin authors should call `register_op_rt_tiled` instead of
   * invoking this callback directly.
   */
  using RegisterRtTiledFn = void (*)(void* user_data, const char* type,
                                     const char* subtype, TileOpFunc fn,
                                     OpMetadata meta);

  /**
   * @brief Host callback for dirty ROI propagator registration.
   *
   * @param user_data Opaque host context passed back unchanged.
   * @param type Operation type string, borrowed for the duration of the call.
   * @param subtype Operation subtype string, borrowed for the duration of the
   * call.
   * @param fn Dirty ROI propagator to move into the host registry.
   * @throws Exceptions from the host registry or callback storage may
   * propagate through the plugin registration entry point.
   * @note Plugins should register explicit ROI contracts for loadable
   * operations instead of relying on legacy identity fallback.
   */
  using RegisterDirtyPropagatorFn = void (*)(void* user_data, const char* type,
                                             const char* subtype,
                                             DirtyRoiPropFunc fn);

  /**
   * @brief Host callback for forward ROI propagator registration.
   *
   * @param user_data Opaque host context passed back unchanged.
   * @param type Operation type string, borrowed for the duration of the call.
   * @param subtype Operation subtype string, borrowed for the duration of the
   * call.
   * @param fn Forward ROI propagator to move into the host registry.
   * @throws Exceptions from the host registry or callback storage may
   * propagate through the plugin registration entry point.
   * @note Plugins should register explicit ROI contracts for loadable
   * operations instead of relying on legacy identity fallback.
   */
  using RegisterForwardPropagatorFn = void (*)(void* user_data,
                                               const char* type,
                                               const char* subtype,
                                               ForwardRoiPropFunc fn);

  /**
   * @brief Host callback for dependency LUT builder registration.
   *
   * @param user_data Opaque host context passed back unchanged.
   * @param type Operation type string, borrowed for the duration of the call.
   * @param subtype Operation subtype string, borrowed for the duration of the
   * call.
   * @param fn Dependency LUT builder to move into the host registry.
   * @param mark_data_dependent Whether the operation should be marked as
   * data-dependent for planning.
   * @throws Exceptions from the host registry or callback storage may
   * propagate through the plugin registration entry point.
   * @note Dependency builders describe spatially data-dependent operations and
   * are optional for pointwise plugins.
   */
  using RegisterDependencyBuilderFn = void (*)(void* user_data,
                                               const char* type,
                                               const char* subtype,
                                               DependencyLutBuilder fn,
                                               bool mark_data_dependent);

  /**
   * @brief Host callback for device-specific monolithic registration.
   *
   * @param user_data Opaque host context passed back unchanged.
   * @param type Operation type string, borrowed for the duration of the call.
   * @param subtype Operation subtype string, borrowed for the duration of the
   * call.
   * @param device Device capability label for the implementation.
   * @param fn Monolithic implementation to move into the host registry.
   * @param meta Metadata used by registry selection policy.
   * @throws Exceptions from the host registry or callback storage may
   * propagate through the plugin registration entry point.
   * @note This extends the same operation key with a device-specific
   * implementation rather than creating a separate registry singleton.
   */
  using RegisterDeviceMonolithicFn = void (*)(void* user_data, const char* type,
                                              const char* subtype,
                                              Device device,
                                              MonolithicOpFunc fn,
                                              OpMetadata meta);

  /**
   * @brief Host callback for device-specific tiled registration.
   *
   * @param user_data Opaque host context passed back unchanged.
   * @param type Operation type string, borrowed for the duration of the call.
   * @param subtype Operation subtype string, borrowed for the duration of the
   * call.
   * @param device Device capability label for the implementation.
   * @param fn Tiled implementation to move into the host registry.
   * @param meta Metadata used by registry selection policy.
   * @throws Exceptions from the host registry or callback storage may
   * propagate through the plugin registration entry point.
   * @note This extends the same operation key with a device-specific
   * implementation rather than creating a separate registry singleton.
   */
  using RegisterDeviceTiledFn = void (*)(void* user_data, const char* type,
                                         const char* subtype, Device device,
                                         TileOpFunc fn, OpMetadata meta);

  /** @brief Opaque host context supplied to every callback. */
  void* user_data = nullptr;
  /** @brief Host HP monolithic registration callback. */
  RegisterHpMonolithicFn register_hp_monolithic = nullptr;
  /** @brief Host HP tiled registration callback. */
  RegisterHpTiledFn register_hp_tiled = nullptr;
  /** @brief Host RT tiled registration callback. */
  RegisterRtTiledFn register_rt_tiled = nullptr;
  /** @brief Host dirty ROI propagator registration callback. */
  RegisterDirtyPropagatorFn register_dirty_propagator_fn = nullptr;
  /** @brief Host forward ROI propagator registration callback. */
  RegisterForwardPropagatorFn register_forward_propagator_fn = nullptr;
  /** @brief Host dependency LUT builder registration callback. */
  RegisterDependencyBuilderFn register_dependency_builder_fn = nullptr;
  /** @brief Host device-specific monolithic registration callback. */
  RegisterDeviceMonolithicFn register_device_monolithic = nullptr;
  /** @brief Host device-specific tiled registration callback. */
  RegisterDeviceTiledFn register_device_tiled = nullptr;

  /**
   * @brief Registers an HP monolithic implementation with the host registry.
   *
   * @param type Operation type, such as `"image_process"`.
   * @param subtype Operation subtype, such as `"invert"`.
   * @param fn Monolithic operation callback.
   * @param meta Metadata used for scheduling and intent selection.
   * @throws std::logic_error when the registrar lacks the required host
   * callback.
   * @throws Exceptions from the host registry or callback storage may
   * propagate.
   * @note The callback is moved into the host registry and may point to plugin
   * code, so the host retains the plugin library while the key is active.
   */
  void register_op_hp_monolithic(const std::string& type,
                                 const std::string& subtype,
                                 MonolithicOpFunc fn,
                                 OpMetadata meta = {}) const {
    if (!register_hp_monolithic) {
      throw std::logic_error(
          "OperationPluginRegistrar missing HP monolithic callback.");
    }
    register_hp_monolithic(user_data, type.c_str(), subtype.c_str(),
                           std::move(fn), meta);
  }

  /**
   * @brief Registers an HP tiled implementation with the host registry.
   *
   * @param type Operation type, such as `"image_process"`.
   * @param subtype Operation subtype, such as `"gaussian_blur"`.
   * @param fn Tiled operation callback.
   * @param meta Metadata used for scheduling and intent selection.
   * @throws std::logic_error when the registrar lacks the required host
   * callback.
   * @throws Exceptions from the host registry or callback storage may
   * propagate.
   * @note The callback is moved into the host registry and may point to plugin
   * code, so the host retains the plugin library while the key is active.
   */
  void register_op_hp_tiled(const std::string& type, const std::string& subtype,
                            TileOpFunc fn, OpMetadata meta) const {
    if (!register_hp_tiled) {
      throw std::logic_error(
          "OperationPluginRegistrar missing HP tiled callback.");
    }
    register_hp_tiled(user_data, type.c_str(), subtype.c_str(), std::move(fn),
                      meta);
  }

  /**
   * @brief Registers an RT tiled implementation with the host registry.
   *
   * @param type Operation type, such as `"image_process"`.
   * @param subtype Operation subtype, such as `"gaussian_blur"`.
   * @param fn Tiled operation callback.
   * @param meta Metadata used for scheduling and intent selection.
   * @throws std::logic_error when the registrar lacks the required host
   * callback.
   * @throws Exceptions from the host registry or callback storage may
   * propagate.
   * @note Real-time registration is optional; HP-only plugins can omit this
   * call.
   */
  void register_op_rt_tiled(const std::string& type, const std::string& subtype,
                            TileOpFunc fn, OpMetadata meta) const {
    if (!register_rt_tiled) {
      throw std::logic_error(
          "OperationPluginRegistrar missing RT tiled callback.");
    }
    register_rt_tiled(user_data, type.c_str(), subtype.c_str(), std::move(fn),
                      meta);
  }

  /**
   * @brief Registers a dirty ROI propagator with the host registry.
   *
   * @param type Operation type associated with the propagator.
   * @param subtype Operation subtype associated with the propagator.
   * @param fn Dirty ROI propagator callback.
   * @throws std::logic_error when the registrar lacks the required host
   * callback.
   * @throws Exceptions from the host registry or callback storage may
   * propagate.
   * @note Loadable operation plugins should provide explicit dirty contracts.
   */
  void register_dirty_propagator(const std::string& type,
                                 const std::string& subtype,
                                 DirtyRoiPropFunc fn) const {
    if (!register_dirty_propagator_fn) {
      throw std::logic_error(
          "OperationPluginRegistrar missing dirty propagator callback.");
    }
    register_dirty_propagator_fn(user_data, type.c_str(), subtype.c_str(),
                                 std::move(fn));
  }

  /**
   * @brief Registers a forward ROI propagator with the host registry.
   *
   * @param type Operation type associated with the propagator.
   * @param subtype Operation subtype associated with the propagator.
   * @param fn Forward ROI propagator callback.
   * @throws std::logic_error when the registrar lacks the required host
   * callback.
   * @throws Exceptions from the host registry or callback storage may
   * propagate.
   * @note Loadable operation plugins should provide explicit forward
   * contracts.
   */
  void register_forward_propagator(const std::string& type,
                                   const std::string& subtype,
                                   ForwardRoiPropFunc fn) const {
    if (!register_forward_propagator_fn) {
      throw std::logic_error(
          "OperationPluginRegistrar missing forward propagator callback.");
    }
    register_forward_propagator_fn(user_data, type.c_str(), subtype.c_str(),
                                   std::move(fn));
  }

  /**
   * @brief Registers a dependency LUT builder with the host registry.
   *
   * @param type Operation type associated with the builder.
   * @param subtype Operation subtype associated with the builder.
   * @param fn Spatial dependency map builder callback.
   * @param mark_data_dependent Whether to mark the operation as
   * data-dependent.
   * @throws std::logic_error when the registrar lacks the required host
   * callback.
   * @throws Exceptions from the host registry or callback storage may
   * propagate.
   * @note Pointwise plugins usually do not need dependency builders.
   */
  void register_dependency_builder(const std::string& type,
                                   const std::string& subtype,
                                   DependencyLutBuilder fn,
                                   bool mark_data_dependent = true) const {
    if (!register_dependency_builder_fn) {
      throw std::logic_error(
          "OperationPluginRegistrar missing dependency builder callback.");
    }
    register_dependency_builder_fn(user_data, type.c_str(), subtype.c_str(),
                                   std::move(fn), mark_data_dependent);
  }

  /**
   * @brief Registers a device-specific monolithic implementation.
   *
   * @param type Operation type associated with the implementation.
   * @param subtype Operation subtype associated with the implementation.
   * @param device Device capability label used by selection policy.
   * @param fn Monolithic implementation callback.
   * @param meta Metadata used by scheduler and registry selection policy.
   * @throws std::logic_error when the registrar lacks the required host
   * callback.
   * @throws Exceptions from the host registry or callback storage may
   * propagate.
   * @note The host registry owns implementation ordering and unload
   * restoration.
   */
  void register_impl(const std::string& type, const std::string& subtype,
                     Device device, MonolithicOpFunc fn,
                     OpMetadata meta = {}) const {
    if (!register_device_monolithic) {
      throw std::logic_error(
          "OperationPluginRegistrar missing device monolithic callback.");
    }
    register_device_monolithic(user_data, type.c_str(), subtype.c_str(), device,
                               std::move(fn), meta);
  }

  /**
   * @brief Registers a device-specific tiled implementation.
   *
   * @param type Operation type associated with the implementation.
   * @param subtype Operation subtype associated with the implementation.
   * @param device Device capability label used by selection policy.
   * @param fn Tiled implementation callback.
   * @param meta Metadata used by scheduler and registry selection policy.
   * @throws std::logic_error when the registrar lacks the required host
   * callback.
   * @throws Exceptions from the host registry or callback storage may
   * propagate.
   * @note The host registry owns implementation ordering and unload
   * restoration.
   */
  void register_impl(const std::string& type, const std::string& subtype,
                     Device device, TileOpFunc fn, OpMetadata meta) const {
    if (!register_device_tiled) {
      throw std::logic_error(
          "OperationPluginRegistrar missing device tiled callback.");
    }
    register_device_tiled(user_data, type.c_str(), subtype.c_str(), device,
                          std::move(fn), meta);
  }
};

/**
 * @brief Exported operation plugin registration entry signature.
 *
 * @param registrar Borrowed host-provided registrar table. It is valid only
 * for the duration of the call and must not be stored by the plugin.
 * @return Nothing.
 * @throws Exceptions from plugin initialization or host registry registration
 * may propagate to the loader, which rolls back captured registry mutations.
 * @note The symbol name is versioned so the host can reject pre-registrar
 * no-argument plugins instead of accidentally calling an incompatible entry
 * point.
 */
using RegisterPhotospiderOpsV1 = void (*)(OperationPluginRegistrar* registrar);

/**
 * @brief Versioned C symbol name resolved by the operation plugin loader.
 *
 * @throws Nothing.
 * @note Keep this string synchronized with the exported declaration below.
 */
inline constexpr const char* kOperationPluginRegisterSymbolV1 =
    "register_photospider_ops_v1";

}  // namespace ps

/**
 * @brief Versioned operation plugin entry point implemented by each plugin.
 *
 * @param registrar Borrowed host-provided registrar. The plugin must call it
 * during this function and must not retain the pointer after returning.
 * @return Nothing.
 * @throws Exceptions from plugin initialization, registrar validation, or host
 * registry storage may propagate to the loader.
 * @note `extern "C"` prevents name mangling so the loader can resolve the
 * exact `register_photospider_ops_v1` symbol. The `_v1` suffix intentionally
 * rejects the old no-argument ABI instead of guessing at an incompatible
 * function signature.
 */
extern "C" PLUGIN_API void register_photospider_ops_v1(
    ps::OperationPluginRegistrar* registrar);
