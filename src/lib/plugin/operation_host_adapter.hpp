#pragma once

#include <memory>

#include "core/ps_types.hpp"  // NOLINT(build/include_subdir)
#include "photospider/plugin/operation_plugin_api.h"

namespace ps::plugin_host {

/**
 * @brief Owns one validated pure-C operation-plugin generation.
 *
 * A generation deep-copies every definition record and retains the native DSO
 * lease. Its final destructor invokes the plugin root destroy callback once,
 * while the library is still mapped. Executable registry callbacks capture a
 * shared generation owner, so replacement and explicit unload remove
 * publication immediately without unmapping in-flight code.
 *
 * @throws std::bad_alloc when Host-owned validation or copied metadata cannot
 * allocate.
 * @note The type is source-private. Plugins observe only the pure-C records
 * declared by `operation_plugin_api.h`.
 */
class OperationPluginGeneration final
    : public std::enable_shared_from_this<OperationPluginGeneration> {
 public:
  /** @brief Opaque Host-owned validated generation implementation. */
  struct Impl;

  /**
   * @brief Completes numeric discovery, root/suite negotiation, and definition
   * validation for one already-authorized native library.
   *
   * @param native_library_lifetime Shared native handle and exact-object trust
   * capability; it remains live through root/context destruction.
   * @param get_abi_version Resolved numeric discovery entry.
   * @param get_api Resolved exact ABI-v1 root entry.
   * @return Valid unpublished generation ready for registry staging.
   * @throws std::invalid_argument for any malformed status, size, version,
   * stride, count, identity, name, callback, reserved field, or relationship.
   * @throws std::bad_alloc when Host staging cannot allocate.
   * @note Only the numeric function is called before the ABI value equals one.
   * A successfully returned root earns exactly one destroy attempt even when a
   * later suite or definition record is rejected.
   */
  static std::shared_ptr<OperationPluginGeneration> create(
      std::shared_ptr<void> native_library_lifetime,
      ps_operation_plugin_get_abi_version_fn_v1 get_abi_version,
      ps_operation_plugin_get_api_fn_v1 get_api);

  /**
   * @brief Destroys plugin-owned generation state before releasing the DSO.
   * @throws Nothing; destroy status and diagnostic failures are observed but
   * cannot escape final retirement.
   * @note The plugin destroy callback is attempted exactly once for every root
   * that completed `get_api_v1` with `OK`.
   */
  ~OperationPluginGeneration() noexcept;

  OperationPluginGeneration(const OperationPluginGeneration&) = delete;
  OperationPluginGeneration& operator=(const OperationPluginGeneration&) =
      delete;

  /**
   * @brief Registers every copied implementation into a shadow registry.
   * @param registry Transaction-local registry receiving complete callbacks,
   * metadata, Region/dependency hooks, and generation leases.
   * @return Nothing.
   * @throws std::invalid_argument if a copied definition cannot map to the
   * current CPU execution model.
   * @throws std::bad_alloc or registry storage exceptions unchanged.
   * @note The caller wraps this method in `OpRegistry::capture_registration`;
   * this function never publishes the process singleton directly.
   */
  void register_into(OpRegistry& registry);

 private:
  /**
   * @brief Takes ownership of one completely initialized implementation.
   * @param impl Valid implementation whose native lease is declared first.
   * @throws Nothing.
   */
  explicit OperationPluginGeneration(std::unique_ptr<Impl> impl) noexcept;

  /** @brief Validated generation state, absent only during destruction. */
  std::unique_ptr<Impl> impl_;
};

}  // namespace ps::plugin_host
