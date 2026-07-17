#pragma once

#include <memory>

#include "core/ps_types.hpp"  // NOLINT(build/include_subdir)
#include "photospider/plugin/plugin_api.hpp"

namespace ps {
class Node;
}

namespace ps::plugin_host {

/**
 * @brief Creates one callback-scoped public identity and parameter snapshot.
 *
 * @param node Host-private operation node.
 * @return Public view borrowing identity strings and owning effective params.
 * @throws std::bad_alloc unchanged from recursive parameter copying.
 * @note The returned identity views must not outlive node; its parameter map
 * may be copied or moved independently.
 */
plugin::NodeView make_node_view(const Node& node);

/**
 * @brief Validates one public device enumerator at the plugin boundary.
 * @param device Device value supplied by metadata or an explicit registrar.
 * @return The same validated device value.
 * @throws std::invalid_argument for an unknown enumerator.
 */
Device operation_device_to_private(Device device);

/**
 * @brief Converts public scheduling metadata to the private registry contract.
 *
 * @param metadata Public plugin metadata.
 * @return Equivalent private registry metadata.
 * @throws std::invalid_argument for an unknown enum value or negative cost.
 * @note The conversion performs no allocation or policy selection.
 */
OpMetadata operation_metadata_to_private(
    const plugin::OperationMetadata& metadata);

/**
 * @brief Adapts one public full-output callback to the private executor shape.
 *
 * @param callback Plugin callback to retain inside the returned adapter.
 * @param library_lifetime Optional DSO lease attached by a loaded-plugin
 * registrar after host-side output conversion succeeds.
 * @return Private callback that constructs complete public input snapshots and
 *         converts complete output values before returning.
 * @throws std::bad_alloc if adapter callable storage cannot be allocated.
 * @note For loaded plugins, the caller must fence the public callback itself
 * with the DSO exception wrapper before passing it here. Host pre-entry
 * conversion and post-return validation then remain outside that fence and
 * preserve their host-owned exception types. Effective parameters are copied
 * directly from Graph-owned format-neutral storage.
 */
MonolithicOpFunc adapt_monolithic_operation(
    plugin::MonolithicOperation callback,
    std::shared_ptr<void> library_lifetime = nullptr);

/**
 * @brief Adapts one public tiled callback to the private executor shape.
 *
 * @param callback Plugin callback to retain inside the returned adapter.
 * @return Private tiled callback over converted geometry and spatial views.
 * @throws std::bad_alloc if adapter callable storage cannot be allocated.
 * @note For loaded plugins, the supplied public callback must already carry
 * the DSO-only exception fence. Every public pointer remains valid only through
 * one callback.
 */
TileOpFunc adapt_tiled_operation(plugin::TiledOperation callback);

/**
 * @brief Adapts one public dirty ROI callback to private graph propagation.
 *
 * @param callback Plugin callback to retain inside the returned adapter.
 * @return Private propagator that builds an immutable topology snapshot.
 * @throws std::bad_alloc if adapter callable storage cannot be allocated.
 * @throws std::invalid_argument after callback return for a negative dimension
 * or signed-int endpoint overflow in the returned ROI.
 * @note For loaded plugins, the supplied public callback must already carry
 * the DSO-only exception fence. Effective parameters are copied before plugin
 * entry.
 */
DirtyRoiPropFunc adapt_dirty_propagator(plugin::DirtyRoiPropagator callback);

/**
 * @brief Adapts one public forward ROI callback to private graph propagation.
 *
 * @param callback Plugin callback to retain inside the returned adapter.
 * @return Private propagator that identifies the exact active input edge.
 * @throws std::bad_alloc if adapter callable storage cannot be allocated.
 * @throws std::invalid_argument after callback return for a negative dimension
 * or signed-int endpoint overflow in the returned ROI.
 * @note For loaded plugins, the supplied public callback must already carry
 * the DSO-only exception fence. Effective parameters are copied before plugin
 * entry.
 */
ForwardRoiPropFunc adapt_forward_propagator(
    plugin::ForwardRoiPropagator callback);

/**
 * @brief Adapts one public dependency builder to private LUT storage.
 *
 * @param callback Plugin callback to retain inside the returned adapter.
 * @return Private builder that validates the complete public LUT before
 *         returning a host value.
 * @throws std::bad_alloc if adapter callable storage cannot be allocated.
 * @note For loaded plugins, the supplied public callback must already carry
 * the DSO-only exception fence. Invalid, mismatched, or overflowing LUTs throw
 * host-owned `std::invalid_argument` after plugin return and before cache
 * replacement.
 */
DependencyLutBuilder adapt_dependency_builder(
    plugin::DependencyLutBuilder callback);

}  // namespace ps::plugin_host
