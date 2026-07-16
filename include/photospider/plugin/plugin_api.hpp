#pragma once

#include <stdexcept>
#include <string>
#include <utility>

#include "photospider/plugin/op_contract.hpp"

/**
 * @file plugin_api.hpp
 * @brief Provisional version-two operation plugin C++ ABI.
 *
 * The exported C entry receives a borrowed host registrar, while callbacks and
 * value types remain a provisional C++17 ABI. C linkage protects only the
 * entrypoint name; the registrar table, `std::function` callbacks, containers,
 * shared ownership, and exceptions still cross the DSO boundary. Providers
 * therefore require the matching Photospider SDK and a compatible compiler,
 * standard library, C++ ABI, allocator/runtime, exception model, and RTTI
 * settings. This interface promises neither pure C consumption nor
 * cross-toolchain or long-term binary compatibility.
 */

#if defined(_WIN32)
#if defined(PHOTOSPIDER_PLUGIN_BUILD)
#define PHOTOSPIDER_OPERATION_PLUGIN_EXPORT __declspec(dllexport)
#else
#define PHOTOSPIDER_OPERATION_PLUGIN_EXPORT
#endif
#elif defined(PHOTOSPIDER_PLUGIN_BUILD)
#define PHOTOSPIDER_OPERATION_PLUGIN_EXPORT \
  __attribute__((visibility("default")))
#else
#define PHOTOSPIDER_OPERATION_PLUGIN_EXPORT
#endif

namespace ps::plugin {

/**
 * @brief Exact required version-two operation registration symbol.
 * @throws Nothing.
 * @note The loader resolves only this symbol; v1 and unversioned entries are
 * rejected without callback publication.
 */
inline constexpr const char* kOperationPluginRegisterSymbolV2 =
    "register_photospider_ops_v2";  // NOLINT(whitespace/indent_namespace)

/**
 * @brief Host-provided callback table for one plugin registration transaction.
 *
 * Plugins call the typed helpers; the raw function pointers are public only so
 * the host can construct the ABI table without exporting registry ownership.
 * The registrar and user_data are borrowed for the registration call and must
 * never be retained.
 *
 * @throws std::invalid_argument from helpers when type/subtype is empty,
 * contains ':', or contains an embedded NUL, or the operation callback is
 * empty.
 * @throws std::logic_error from helpers when the host table is incomplete.
 * @note Every successful helper call mutates only a host shadow transaction
 *       until the complete candidate is published.
 */
struct OperationPluginRegistrar {
  /**
   * @brief Host callback type for HP monolithic registration.
   * @param user_data Borrowed host transaction context.
   * @param type Borrowed null-terminated operation type.
   * @param subtype Borrowed null-terminated operation subtype.
   * @param callback Owned plugin callback moved into staged host state.
   * @param metadata Scheduling metadata copied into staged host state.
   * @return Nothing.
   * @throws Host validation, callback-copy, or allocation exceptions unchanged.
   * @note No registration becomes process-visible before candidate commit.
   */
  using RegisterHpMonolithic = void (*)(void*, const char*, const char*,
                                        MonolithicOperation, OperationMetadata);
  /**
   * @brief Host callback type for HP tiled registration.
   * @param user_data Borrowed host transaction context.
   * @param type Borrowed null-terminated operation type.
   * @param subtype Borrowed null-terminated operation subtype.
   * @param callback Owned tiled callback moved into staged host state.
   * @param metadata Scheduling metadata copied into staged host state.
   * @return Nothing.
   * @throws Host validation, callback-copy, or allocation exceptions unchanged.
   * @note Registration is transaction-local until candidate commit.
   */
  using RegisterHpTiled = void (*)(void*, const char*, const char*,
                                   TiledOperation, OperationMetadata);
  /**
   * @brief Host callback type for RT tiled registration.
   * @param user_data Borrowed host transaction context.
   * @param type Borrowed null-terminated operation type.
   * @param subtype Borrowed null-terminated operation subtype.
   * @param callback Owned tiled callback moved into staged host state.
   * @param metadata Scheduling metadata copied into staged host state.
   * @return Nothing.
   * @throws Host validation, callback-copy, or allocation exceptions unchanged.
   * @note Registration is transaction-local until candidate commit.
   */
  using RegisterRtTiled = void (*)(void*, const char*, const char*,
                                   TiledOperation, OperationMetadata);
  /**
   * @brief Host callback type for dirty ROI registration.
   * @param user_data Borrowed host transaction context.
   * @param type Borrowed null-terminated operation type.
   * @param subtype Borrowed null-terminated operation subtype.
   * @param callback Owned dirty propagator moved into staged host state.
   * @return Nothing.
   * @throws Host validation, callback-copy, or allocation exceptions unchanged.
   * @note Registration is transaction-local until candidate commit.
   */
  using RegisterDirty = void (*)(void*, const char*, const char*,
                                 DirtyRoiPropagator);
  /**
   * @brief Host callback type for forward ROI registration.
   * @param user_data Borrowed host transaction context.
   * @param type Borrowed null-terminated operation type.
   * @param subtype Borrowed null-terminated operation subtype.
   * @param callback Owned forward propagator moved into staged host state.
   * @return Nothing.
   * @throws Host validation, callback-copy, or allocation exceptions unchanged.
   * @note Registration is transaction-local until candidate commit.
   */
  using RegisterForward = void (*)(void*, const char*, const char*,
                                   ForwardRoiPropagator);
  /**
   * @brief Host callback type for dependency LUT registration.
   * @param user_data Borrowed host transaction context.
   * @param type Borrowed null-terminated operation type.
   * @param subtype Borrowed null-terminated operation subtype.
   * @param callback Owned dependency builder moved into staged host state.
   * @param data_dependent Whether image content revisions participate in the
   * dependency cache identity.
   * @return Nothing.
   * @throws Host validation, callback-copy, or allocation exceptions unchanged.
   * @note The flag is explicit at the same transaction boundary as the
   * callback, so registration order cannot mix a builder with stale operation
   * metadata. Static builders pass false and remain reusable across pixel-only
   * image revisions.
   */
  using RegisterDependency = void (*)(void*, const char*, const char*,
                                      DependencyLutBuilder, bool);
  /**
   * @brief Host callback type for device monolithic registration.
   * @param user_data Borrowed host transaction context.
   * @param type Borrowed null-terminated operation type.
   * @param subtype Borrowed null-terminated operation subtype.
   * @param device Device capability label for the implementation.
   * @param callback Owned callback moved into staged host state.
   * @param metadata Scheduling metadata copied into staged host state.
   * @return Nothing.
   * @throws Host validation, callback-copy, or allocation exceptions unchanged.
   * @note Registration is transaction-local until candidate commit.
   */
  using RegisterDeviceMonolithic = void (*)(void*, const char*, const char*,
                                            Device, MonolithicOperation,
                                            OperationMetadata);
  /**
   * @brief Host callback type for device tiled registration.
   * @param user_data Borrowed host transaction context.
   * @param type Borrowed null-terminated operation type.
   * @param subtype Borrowed null-terminated operation subtype.
   * @param device Device capability label for the implementation.
   * @param callback Owned callback moved into staged host state.
   * @param metadata Scheduling metadata copied into staged host state.
   * @return Nothing.
   * @throws Host validation, callback-copy, or allocation exceptions unchanged.
   * @note Registration is transaction-local until candidate commit.
   */
  using RegisterDeviceTiled = void (*)(void*, const char*, const char*, Device,
                                       TiledOperation, OperationMetadata);

  /** @brief Opaque host transaction context. */
  void* user_data = nullptr;
  /** @brief HP monolithic callback installed by the host. */
  RegisterHpMonolithic register_hp_monolithic = nullptr;
  /** @brief HP tiled callback installed by the host. */
  RegisterHpTiled register_hp_tiled = nullptr;
  /** @brief RT tiled callback installed by the host. */
  RegisterRtTiled register_rt_tiled = nullptr;
  /** @brief Dirty ROI callback installed by the host. */
  RegisterDirty register_dirty = nullptr;
  /** @brief Forward ROI callback installed by the host. */
  RegisterForward register_forward = nullptr;
  /** @brief Dependency LUT callback installed by the host. */
  RegisterDependency register_dependency = nullptr;
  /** @brief Device monolithic callback installed by the host. */
  RegisterDeviceMonolithic register_device_monolithic = nullptr;
  /** @brief Device tiled callback installed by the host. */
  RegisterDeviceTiled register_device_tiled = nullptr;

  /**
   * @brief Registers an HP monolithic implementation.
   * @param type Operation type.
   * @param subtype Operation subtype.
   * @param callback Plugin callback to move into the host transaction.
   * @param metadata Scheduling metadata.
   * @return Nothing.
   * @throws std::invalid_argument for an invalid type/subtype segment or empty
   * callback.
   * @throws std::logic_error if the host callback is absent.
   * @throws Any host registration/storage exception unchanged.
   */
  void register_op_hp_monolithic(const std::string& type,
                                 const std::string& subtype,
                                 MonolithicOperation callback,
                                 OperationMetadata metadata = {}) const {
    require_name_segment(type, "operation type");
    require_name_segment(subtype, "operation subtype");
    require_operation_callback(callback, "HP monolithic");
    require(register_hp_monolithic, "HP monolithic");
    register_hp_monolithic(user_data, type.c_str(), subtype.c_str(),
                           std::move(callback), metadata);
  }

  /**
   * @brief Registers an HP tiled implementation.
   * @param type Operation type.
   * @param subtype Operation subtype.
   * @param callback Plugin callback to move into the host transaction.
   * @param metadata Scheduling metadata.
   * @return Nothing.
   * @throws std::invalid_argument for an invalid type/subtype segment or empty
   * callback.
   * @throws std::logic_error if the host callback is absent.
   * @throws Any host registration/storage exception unchanged.
   */
  void register_op_hp_tiled(const std::string& type, const std::string& subtype,
                            TiledOperation callback,
                            OperationMetadata metadata = {}) const {
    require_name_segment(type, "operation type");
    require_name_segment(subtype, "operation subtype");
    require_operation_callback(callback, "HP tiled");
    require(register_hp_tiled, "HP tiled");
    register_hp_tiled(user_data, type.c_str(), subtype.c_str(),
                      std::move(callback), metadata);
  }

  /**
   * @brief Registers an RT tiled implementation.
   * @param type Operation type.
   * @param subtype Operation subtype.
   * @param callback Plugin callback to move into the host transaction.
   * @param metadata Scheduling metadata.
   * @return Nothing.
   * @throws std::invalid_argument for an invalid type/subtype segment or empty
   * callback.
   * @throws std::logic_error if the host callback is absent.
   * @throws Any host registration/storage exception unchanged.
   */
  void register_op_rt_tiled(const std::string& type, const std::string& subtype,
                            TiledOperation callback,
                            OperationMetadata metadata = {}) const {
    require_name_segment(type, "operation type");
    require_name_segment(subtype, "operation subtype");
    require_operation_callback(callback, "RT tiled");
    require(register_rt_tiled, "RT tiled");
    register_rt_tiled(user_data, type.c_str(), subtype.c_str(),
                      std::move(callback), metadata);
  }

  /**
   * @brief Registers an explicit dirty ROI propagator.
   * @param type Operation type.
   * @param subtype Operation subtype.
   * @param callback Propagator to move into the host transaction.
   * @return Nothing.
   * @throws std::invalid_argument for an invalid type/subtype segment or empty
   * callback.
   * @throws std::logic_error if the host callback is absent.
   * @throws Any host registration/storage exception unchanged.
   */
  void register_dirty_propagator(const std::string& type,
                                 const std::string& subtype,
                                 DirtyRoiPropagator callback) const {
    require_name_segment(type, "operation type");
    require_name_segment(subtype, "operation subtype");
    require_operation_callback(callback, "dirty ROI");
    require(register_dirty, "dirty ROI");
    register_dirty(user_data, type.c_str(), subtype.c_str(),
                   std::move(callback));
  }

  /**
   * @brief Registers an explicit forward ROI propagator.
   * @param type Operation type.
   * @param subtype Operation subtype.
   * @param callback Propagator to move into the host transaction.
   * @return Nothing.
   * @throws std::invalid_argument for an invalid type/subtype segment or empty
   * callback.
   * @throws std::logic_error if the host callback is absent.
   * @throws Any host registration/storage exception unchanged.
   */
  void register_forward_propagator(const std::string& type,
                                   const std::string& subtype,
                                   ForwardRoiPropagator callback) const {
    require_name_segment(type, "operation type");
    require_name_segment(subtype, "operation subtype");
    require_operation_callback(callback, "forward ROI");
    require(register_forward, "forward ROI");
    register_forward(user_data, type.c_str(), subtype.c_str(),
                     std::move(callback));
  }

  /**
   * @brief Registers a dependency LUT builder.
   * @param type Operation type.
   * @param subtype Operation subtype.
   * @param callback Builder to move into the host transaction.
   * @param data_dependent Whether builder output depends on image pixels in
   * addition to topology, extents, and effective parameters.
   * @return Nothing.
   * @throws std::invalid_argument for an invalid type/subtype segment or empty
   * callback.
   * @throws std::logic_error if the host callback is absent.
   * @throws Any host registration/storage exception unchanged.
   */
  void register_dependency_builder(const std::string& type,
                                   const std::string& subtype,
                                   DependencyLutBuilder callback,
                                   bool data_dependent = false) const {
    require_name_segment(type, "operation type");
    require_name_segment(subtype, "operation subtype");
    require_operation_callback(callback, "dependency LUT");
    require(register_dependency, "dependency LUT");
    register_dependency(user_data, type.c_str(), subtype.c_str(),
                        std::move(callback), data_dependent);
  }

  /**
   * @brief Registers a device-specific monolithic implementation.
   * @param type Operation type.
   * @param subtype Operation subtype.
   * @param device Device capability label.
   * @param callback Plugin callback to move into the host transaction.
   * @param metadata Scheduling metadata.
   * @return Nothing.
   * @throws std::invalid_argument for an invalid type/subtype segment or empty
   * callback.
   * @throws std::logic_error if the host callback is absent.
   * @throws Any host registration/storage exception unchanged.
   */
  void register_impl(const std::string& type, const std::string& subtype,
                     Device device, MonolithicOperation callback,
                     OperationMetadata metadata = {}) const {
    require_name_segment(type, "operation type");
    require_name_segment(subtype, "operation subtype");
    require_operation_callback(callback, "device monolithic");
    require(register_device_monolithic, "device monolithic");
    register_device_monolithic(user_data, type.c_str(), subtype.c_str(), device,
                               std::move(callback), metadata);
  }

  /**
   * @brief Registers a device-specific tiled implementation.
   * @param type Operation type.
   * @param subtype Operation subtype.
   * @param device Device capability label.
   * @param callback Plugin callback to move into the host transaction.
   * @param metadata Scheduling metadata.
   * @return Nothing.
   * @throws std::invalid_argument for an invalid type/subtype segment or empty
   * callback.
   * @throws std::logic_error if the host callback is absent.
   * @throws Any host registration/storage exception unchanged.
   */
  void register_impl(const std::string& type, const std::string& subtype,
                     Device device, TiledOperation callback,
                     OperationMetadata metadata = {}) const {
    require_name_segment(type, "operation type");
    require_name_segment(subtype, "operation subtype");
    require_operation_callback(callback, "device tiled");
    require(register_device_tiled, "device tiled");
    register_device_tiled(user_data, type.c_str(), subtype.c_str(), device,
                          std::move(callback), metadata);
  }

 private:
  /**
   * @brief Validates one operation-key segment before crossing the raw ABI.
   * @param value Type or subtype supplied through the C++ helper.
   * @param label Stable diagnostic label.
   * @return Nothing.
   * @throws std::invalid_argument when value is empty, contains the canonical
   * key separator ':', or contains an embedded NUL that c_str() would truncate.
   * @note Validation before raw callback invocation prevents key collisions and
   * guarantees that rejected names cannot mutate the host transaction.
   */
  static void require_name_segment(const std::string& value,
                                   const char* label) {
    if (value.empty() || value.find(':') != std::string::npos ||
        value.find('\0') != std::string::npos) {
      throw std::invalid_argument(std::string("Invalid ") + label +
                                  " for operation registration");
    }
  }

  /**
   * @brief Validates one operation callback before crossing the raw ABI.
   * @tparam Callback Public std::function callback type.
   * @param callback Callback object to validate without moving it.
   * @param label Stable diagnostic label.
   * @return Nothing.
   * @throws std::invalid_argument when callback has no callable target.
   * @note Validation occurs before the host slot is inspected or invoked, so
   * an empty callback cannot mutate the host transaction even when a plugin
   * constructs the public registrar directly in a unit test.
   */
  template <typename Callback>
  static void require_operation_callback(const Callback& callback,
                                         const char* label) {
    if (!callback) {
      throw std::invalid_argument(std::string("Empty ") + label +
                                  " operation callback");
    }
  }

  /**
   * @brief Validates one host callback slot.
   * @tparam Callback Raw registrar function-pointer type.
   * @param callback Function pointer to validate.
   * @param label Stable diagnostic label.
   * @return Nothing.
   * @throws std::logic_error when callback is null.
   */
  template <typename Callback>
  static void require(Callback callback, const char* label) {
    if (!callback) {
      throw std::logic_error(std::string("OperationPluginRegistrar missing ") +
                             label + " callback");
    }
  }
};

/**
 * @brief Required version-two registration entry function-pointer type.
 * @param registrar Borrowed host transaction API valid only for the call.
 * @return Nothing.
 * @throws Plugin registration and host callback exceptions cross this
 * transitional C++ ABI unchanged.
 * @note The export has C symbol linkage, but its pointer/table/callback values
 * remain a provisional C++17 ABI under the matching-SDK/toolchain assumption.
 * The symbol version does not make the registrar a stable pure C data ABI.
 */
using RegisterPhotospiderOpsV2 = void (*)(OperationPluginRegistrar* registrar);

}  // namespace ps::plugin
