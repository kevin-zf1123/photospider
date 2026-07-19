#include "plugin/operation_host_adapter.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "graph/graph_model.hpp"
#include "graph/node.hpp"  // NOLINT(build/include_subdir)

namespace ps::plugin_host {
namespace {

/**
 * @brief Validates one plugin-produced ROI rectangle.
 * @param rect Public rectangle returned by an operation callback.
 * @return Validated kernel rectangle, including a permitted negative origin.
 * @throws std::invalid_argument when a dimension is negative or either
 * endpoint cannot be represented by the kernel signed-int geometry type.
 * @note Zero-area rectangles are valid and retain their supplied origin.
 */
PixelRect validated_kernel_rect(const PixelRect& rect) {
  const std::int64_t right = static_cast<std::int64_t>(rect.x) + rect.width;
  const std::int64_t bottom = static_cast<std::int64_t>(rect.y) + rect.height;
  if (rect.width < 0 || rect.height < 0 ||
      right < std::numeric_limits<int>::min() ||
      right > std::numeric_limits<int>::max() ||
      bottom < std::numeric_limits<int>::min() ||
      bottom > std::numeric_limits<int>::max()) {
    throw std::invalid_argument(
        "Operation ROI callback returned invalid geometry");
  }
  return rect;
}

/**
 * @brief Copies private spatial metadata into a public snapshot.
 * @param spatial Private metadata to copy.
 * @return Host-independent snapshot.
 * @throws Nothing.
 */
plugin::SpatialSnapshot to_public_spatial(
    const SpatialContext& spatial) noexcept {
  plugin::SpatialSnapshot result;
  result.transform_matrix = spatial.transform_matrix;
  result.inverse_matrix = spatial.inverse_matrix;
  result.local_inverse_matrix = spatial.local_inverse_matrix;
  result.absolute_roi = spatial.absolute_roi;
  result.global_scale_x = spatial.global_scale_x;
  result.global_scale_y = spatial.global_scale_y;
  return result;
}

/**
 * @brief Copies public spatial metadata into private storage.
 * @param spatial Public metadata to copy.
 * @return Private spatial value after complete numeric validation.
 * @throws std::invalid_argument when any matrix/scale value is non-finite, an
 * absolute-ROI dimension is negative, or an endpoint exceeds signed-int range.
 * @note Validation occurs before a converted output can receive a DSO lease or
 * enter any host cache.
 */
SpatialContext to_private_spatial(const plugin::SpatialSnapshot& spatial) {
  const auto all_finite = [](const std::array<double, 9>& matrix) {
    return std::all_of(matrix.begin(), matrix.end(),
                       [](double value) { return std::isfinite(value); });
  };
  const PixelRect& roi = spatial.absolute_roi;
  const std::int64_t right = static_cast<std::int64_t>(roi.x) + roi.width;
  const std::int64_t bottom = static_cast<std::int64_t>(roi.y) + roi.height;
  if (!all_finite(spatial.transform_matrix) ||
      !all_finite(spatial.inverse_matrix) ||
      !all_finite(spatial.local_inverse_matrix) ||
      !std::isfinite(spatial.global_scale_x) ||
      !std::isfinite(spatial.global_scale_y) || roi.width < 0 ||
      roi.height < 0 || right < std::numeric_limits<int>::min() ||
      right > std::numeric_limits<int>::max() ||
      bottom < std::numeric_limits<int>::min() ||
      bottom > std::numeric_limits<int>::max()) {
    throw std::invalid_argument(
        "Operation output has invalid spatial metadata");
  }
  SpatialContext result;
  result.transform_matrix = spatial.transform_matrix;
  result.inverse_matrix = spatial.inverse_matrix;
  result.local_inverse_matrix = spatial.local_inverse_matrix;
  result.absolute_roi = spatial.absolute_roi;
  result.global_scale_x = spatial.global_scale_x;
  result.global_scale_y = spatial.global_scale_y;
  return result;
}

/**
 * @brief Owns all storage behind one monolithic input view array.
 * @throws std::bad_alloc from recursive values or vector growth.
 * @note Vectors are reserved before pointers are installed, preventing pointer
 *       invalidation during construction.
 */
struct OperationInputStorage {
  /** @brief Owned public named-output snapshots. */
  std::vector<plugin::ParameterMap> data;
  /** @brief Owned public spatial snapshots. */
  std::vector<plugin::SpatialSnapshot> spatial;
  /** @brief Borrowed views pointing into data/spatial and host image values. */
  std::vector<plugin::OperationInputView> views;
};

/**
 * @brief Checks whether an image descriptor carries a usable pixel payload.
 * @param buffer Image descriptor to inspect.
 * @return True when dimensions are positive and CPU or backend storage exists.
 * @throws Nothing.
 * @note A false result lets named-data-only outputs expose a null public image
 * pointer instead of an apparently present empty descriptor.
 */
bool has_image_payload(const ImageBuffer& buffer) noexcept {
  return buffer.width > 0 && buffer.height > 0 && buffer.channels > 0 &&
         (buffer.data || buffer.context);
}

/**
 * @brief Keeps one plugin-origin shared payload and its DSO lease together.
 *
 * @throws Nothing during destruction; shared-owner construction may allocate
 * before an instance exists.
 * @note Member declaration order makes the plugin payload retire before the
 * final library lease.
 */
struct PluginPayloadLeaseState {
  /**
   * @brief Takes ownership of one plugin payload and matching library lease.
   * @param retained_library Library kept mapped through payload retirement.
   * @param retained_payload Plugin-origin shared payload to retire first.
   * @throws Nothing when shared-owner moves are non-throwing.
   */
  PluginPayloadLeaseState(std::shared_ptr<void> retained_library,
                          std::shared_ptr<void> retained_payload) noexcept
      : library_lifetime(std::move(retained_library)),
        payload(std::move(retained_payload)) {}

  /** @brief Library lease declared first so it is destroyed last. */
  std::shared_ptr<void> library_lifetime;
  /** @brief Plugin-origin payload destroyed while the DSO remains mapped. */
  std::shared_ptr<void> payload;
};

/**
 * @brief Wraps one plugin-origin shared payload in an aliasing DSO lease.
 * @param payload Original shared pixel or backend-context owner.
 * @param library_lifetime Matching operation-plugin library lease.
 * @return Empty for a shared pointer without a control block, otherwise an
 * alias preserving the original payload address and retaining both owners.
 * @throws std::bad_alloc if host lease-state allocation fails.
 * @note The host-instantiated control block releases the original payload
 * before releasing the DSO lease.
 */
std::shared_ptr<void> retain_plugin_payload(
    const std::shared_ptr<void>& payload,
    const std::shared_ptr<void>& library_lifetime) {
  if (payload.use_count() == 0) {
    return {};
  }
  void* payload_address = payload.get();
  auto state =
      std::make_shared<PluginPayloadLeaseState>(library_lifetime, payload);
  return std::shared_ptr<void>(state, payload_address);
}

/**
 * @brief Attaches copy-safe plugin leases to one converted operation result.
 * @param result Converted host output whose image handles may originate in the
 * plugin DSO.
 * @param library_lifetime Matching operation-plugin library lease.
 * @return Nothing.
 * @throws std::bad_alloc if either payload wrapper allocation fails.
 * @note Both wrappers are staged before publication. This host post-processing
 * runs after the plugin exception fence, so allocation failure remains an
 * ordinary host `std::bad_alloc`.
 */
void attach_result_library_lifetime(
    NodeOutput& result, const std::shared_ptr<void>& library_lifetime) {
  std::shared_ptr<void> retained_data =
      retain_plugin_payload(result.image_buffer.data, library_lifetime);
  std::shared_ptr<void> retained_context =
      retain_plugin_payload(result.image_buffer.context, library_lifetime);
  result.image_buffer.data = std::move(retained_data);
  result.image_buffer.context = std::move(retained_context);
  result.plugin_library_lifetime = library_lifetime;
}

/**
 * @brief Builds callback-scoped public views for private upstream outputs.
 * @param inputs Borrowed private output pointers in image-input order.
 * @return Storage owning every copied non-image value and view.
 * @throws std::bad_alloc unchanged from recursive copying or vector allocation.
 * @note Null inputs become empty views without dereferencing host storage.
 *       Every non-null NodeOutput exposes its spatial snapshot even when it is
 *       data-only and therefore has no image payload.
 */
OperationInputStorage make_operation_inputs(
    const std::vector<const NodeOutput*>& inputs) {
  OperationInputStorage storage;
  storage.data.reserve(inputs.size());
  storage.spatial.reserve(inputs.size());
  for (const NodeOutput* input : inputs) {
    if (!input) {
      storage.data.emplace_back();
      storage.spatial.emplace_back();
      continue;
    }
    storage.data.push_back(input->data);
    storage.spatial.push_back(to_public_spatial(input->space));
  }
  storage.views.reserve(inputs.size());
  for (std::size_t index = 0; index < inputs.size(); ++index) {
    const NodeOutput* input = inputs[index];
    if (!input) {
      storage.views.emplace_back();
      continue;
    }
    const bool has_image = has_image_payload(input->image_buffer);
    storage.views.push_back(plugin::OperationInputView{
        has_image ? &input->image_buffer : nullptr,
        storage.data[index].empty() ? nullptr : &storage.data[index],
        &storage.spatial[index]});
  }
  return storage;
}

/**
 * @brief Converts a complete public operation output to private storage.
 * @param output Public value moved out of plugin code.
 * @return Complete private output without a library lease.
 * @throws std::invalid_argument for an invalid image descriptor or spatial
 * snapshot.
 * @throws std::bad_alloc unchanged from recursive map moves/copies.
 * @note The caller attaches its private DSO lease only after this conversion
 *       succeeds; during conversion the callback wrapper still retains it.
 *       Named values remain ParameterValue objects without format conversion.
 */
NodeOutput operation_output_to_private(plugin::OperationOutput output) {
  validate_image_buffer(output.image_buffer);
  NodeOutput result;
  result.image_buffer = std::move(output.image_buffer);
  result.data = std::move(output.data);
  result.space = to_private_spatial(output.spatial);
  result.debug.computed_by_worker_id = output.debug.computed_by_worker_id;
  result.debug.timestamp_us = output.debug.timestamp_us;
  result.debug.execution_time_ms = output.debug.execution_time_ms;
  result.debug.min_val = output.debug.min_value;
  result.debug.max_val = output.debug.max_value;
  result.debug.has_nan = output.debug.has_nan;
  result.debug.compute_device = std::move(output.debug.compute_device);
  return result;
}

/**
 * @brief Returns the preferred cached private output for one node.
 * @param node Node whose HP cache is inspected.
 * @return Borrowed cached output or nullptr when absent.
 * @throws Nothing.
 */
const NodeOutput* cached_output(const Node& node) noexcept {
  return node.cached_output_high_precision
             ? &node.cached_output_high_precision.value()
             : nullptr;
}

/**
 * @brief Owns one public ROI invocation and every pointer target it exposes.
 * @throws std::bad_alloc from parameter, edge, data, or spatial conversion.
 * @note Member order and pre-reservation keep edge payload pointers stable.
 */
struct RoiInvocationStorage {
  /** @brief Current node identity and owned effective parameters. */
  plugin::NodeView node;
  /** @brief Available input named-output snapshots. */
  std::vector<plugin::ParameterMap> available_data;
  /** @brief Available input spatial snapshots. */
  std::vector<plugin::SpatialSnapshot> available_spatial;
  /** @brief Ordered image-input edge snapshots. */
  std::vector<plugin::InputEdgeView> edges;
  /** @brief Requested ROI copied for later context construction. */
  PixelRect requested_roi;
  /** @brief Output extent copied for later context construction. */
  PixelSize output_extent;
  /** @brief Active input index for forward propagation, otherwise null. */
  std::optional<std::size_t> active_input_index;

  /**
   * @brief Constructs a callback context after this storage reaches its final
   *        address.
   * @return Value context borrowing this object's node and edge storage.
   * @throws std::invalid_argument when active_input_index names no edge.
   * @note The returned context must not outlive this storage instance.
   */
  plugin::RoiContext view() const {
    const plugin::InputEdgeView* active_edge = nullptr;
    if (active_input_index) {
      const auto found = std::find_if(
          edges.begin(), edges.end(), [&](const plugin::InputEdgeView& edge) {
            return edge.input_index == *active_input_index;
          });
      if (found == edges.end()) {
        throw std::invalid_argument("Forward ROI active input edge is missing");
      }
      active_edge = &*found;
    }
    return plugin::RoiContext{&node, requested_roi, output_extent,
                              plugin::ArrayView<plugin::InputEdgeView>(edges),
                              active_edge};
  }
};

/**
 * @brief Builds a complete public ROI context from private topology state.
 *
 * @param node Current operation node.
 * @param graph Private graph used only while constructing the snapshot.
 * @param requested_roi ROI passed to the public callback.
 * @param output_extent Known current output extent.
 * @param input_extents Known image-input extents by destination index.
 * @param effective_parameters Exact parameter snapshot resolved by the caller.
 * @param active_input_index Active image-input index for forward propagation.
 * @param available_inputs Optional destination-indexed execution inputs. Null
 * selects immutable snapshots from the supplied planning graph.
 * @return Owned invocation storage with stable internal pointers.
 * @throws std::bad_alloc unchanged from snapshot construction.
 * @note The returned context contains no graph owner or mutable cache handle.
 *       Active-edge validation belongs to `RoiInvocationStorage::view()` after
 *       this storage has reached its final address.
 */
RoiInvocationStorage make_roi_invocation(
    const Node& node, const GraphModel& graph, const PixelRect& requested_roi,
    const PixelSize& output_extent, const std::vector<PixelSize>& input_extents,
    plugin::ParameterMap effective_parameters,
    std::optional<std::size_t> active_input_index,
    const std::vector<const NodeOutput*>* available_inputs = nullptr) {
  RoiInvocationStorage storage;
  storage.node = plugin::NodeView(node.id, node.name, node.type, node.subtype,
                                  std::move(effective_parameters));
  std::vector<GraphTopologyEdge> image_edges;
  for (const auto& edge : graph.upstream_edges(node.id)) {
    if (edge.kind == GraphTopologyEdgeKind::ImageInput) {
      image_edges.push_back(edge);
    }
  }
  std::sort(image_edges.begin(), image_edges.end(),
            [](const GraphTopologyEdge& left, const GraphTopologyEdge& right) {
              return left.input_index < right.input_index;
            });
  storage.available_data.reserve(image_edges.size());
  storage.available_spatial.reserve(image_edges.size());
  storage.edges.reserve(image_edges.size());
  for (const auto& edge : image_edges) {
    storage.available_data.emplace_back();
    storage.available_spatial.emplace_back();
    plugin::InputEdgeView public_edge;
    public_edge.source_node_id = edge.from_node_id;
    public_edge.source_output = edge.from_output_name;
    public_edge.input_index = edge.input_index;
    if (edge.input_index < input_extents.size()) {
      public_edge.extent = input_extents[edge.input_index];
    }
    const NodeOutput* available = nullptr;
    if (available_inputs) {
      if (edge.input_index < available_inputs->size()) {
        available = (*available_inputs)[edge.input_index];
      }
    } else if (edge.from_node_id >= 0 && graph.has_node(edge.from_node_id)) {
      available = cached_output(graph.node(edge.from_node_id));
    }
    if (available) {
      storage.available_data.back() = available->data;
      storage.available_spatial.back() = to_public_spatial(available->space);
      public_edge.has_available_input = true;
      const bool has_image = has_image_payload(available->image_buffer);
      public_edge.available_input = plugin::OperationInputView{
          has_image ? &available->image_buffer : nullptr,
          storage.available_data.back().empty()
              ? nullptr
              : &storage.available_data.back(),
          &storage.available_spatial.back()};
      if (public_edge.extent.width <= 0 || public_edge.extent.height <= 0) {
        public_edge.extent = PixelSize{available->image_buffer.width,
                                       available->image_buffer.height};
      }
    }
    storage.edges.push_back(std::move(public_edge));
  }
  storage.requested_roi = requested_roi;
  storage.output_extent = output_extent;
  storage.active_input_index = active_input_index;
  return storage;
}

/**
 * @brief Strictly converts one validated public LUT to private storage.
 * @param snapshot Public dependency table.
 * @param context Context used to validate input index and output extent.
 * @return Complete private table.
 * @throws std::invalid_argument for malformed dimensions, count, input index,
 * output mismatch, unavailable upstream extent, negative cell size, or
 * unrepresentable derived dimensions.
 * @throws std::bad_alloc unchanged from ROI vector allocation.
 * @note Validation completes before the returned value can replace a cache.
 */
SpatialDependencyMap dependency_lut_to_private(
    const plugin::DependencyLutSnapshot& snapshot,
    const plugin::RoiContext& context) {
  if (!snapshot.is_valid()) {
    throw std::invalid_argument(
        "Operation dependency LUT is structurally invalid");
  }
  const auto input_edge =
      std::find_if(context.input_edges.begin(), context.input_edges.end(),
                   [&](const plugin::InputEdgeView& edge) {
                     return edge.input_index == snapshot.upstream_input_index;
                   });
  if (input_edge == context.input_edges.end()) {
    throw std::invalid_argument(
        "Operation dependency LUT input index is invalid");
  }
  if (snapshot.output_extent.width != context.output_extent.width ||
      snapshot.output_extent.height != context.output_extent.height) {
    throw std::invalid_argument(
        "Operation dependency LUT output extent mismatch");
  }
  const std::size_t width =
      static_cast<std::size_t>(snapshot.output_extent.width);
  const std::size_t height =
      static_cast<std::size_t>(snapshot.output_extent.height);
  const std::size_t cell_width =
      static_cast<std::size_t>(snapshot.cell_size.width);
  const std::size_t cell_height =
      static_cast<std::size_t>(snapshot.cell_size.height);
  const std::size_t columns = 1 + (width - 1) / cell_width;
  const std::size_t rows = 1 + (height - 1) / cell_height;
  if (columns > static_cast<std::size_t>(std::numeric_limits<int>::max()) ||
      rows > static_cast<std::size_t>(std::numeric_limits<int>::max()) ||
      (rows != 0 &&
       columns >
           static_cast<std::size_t>(std::numeric_limits<int>::max()) / rows)) {
    throw std::invalid_argument("Operation dependency LUT dimensions overflow");
  }
  if (input_edge->extent.width <= 0 || input_edge->extent.height <= 0) {
    throw std::invalid_argument(
        "Operation dependency LUT input extent is unavailable");
  }

  const auto normalize_upstream_roi = [&](const PixelRect& roi) -> PixelRect {
    if (roi.width < 0 || roi.height < 0) {
      throw std::invalid_argument(
          "Operation dependency LUT cell has a negative size");
    }
    if (roi.width == 0 || roi.height == 0) {
      return PixelRect{};
    }
    const std::int64_t left = std::max<std::int64_t>(roi.x, 0);
    const std::int64_t top = std::max<std::int64_t>(roi.y, 0);
    const std::int64_t right = std::min<std::int64_t>(
        static_cast<std::int64_t>(roi.x) + roi.width, input_edge->extent.width);
    const std::int64_t bottom =
        std::min<std::int64_t>(static_cast<std::int64_t>(roi.y) + roi.height,
                               input_edge->extent.height);
    if (right <= left || bottom <= top) {
      return PixelRect{};
    }
    return PixelRect{static_cast<int>(left), static_cast<int>(top),
                     static_cast<int>(right - left),
                     static_cast<int>(bottom - top)};
  };
  SpatialDependencyMap result;
  result.grid_size_x = snapshot.cell_size.width;
  result.grid_size_y = snapshot.cell_size.height;
  result.cols = static_cast<int>(columns);
  result.rows = static_cast<int>(rows);
  result.output_extent = snapshot.output_extent;
  result.upstream_input_index = snapshot.upstream_input_index;
  result.cell_to_upstream_roi.reserve(snapshot.cell_to_upstream_roi.size());
  for (const PixelRect& roi : snapshot.cell_to_upstream_roi) {
    result.cell_to_upstream_roi.push_back(normalize_upstream_roi(roi));
  }
  return result;
}

}  // namespace

/** @copydoc ps::plugin_host::make_node_view */
plugin::NodeView make_node_view(const Node& node) {
  const plugin::ParameterMap& effective = node.runtime_parameters.empty()
                                              ? node.parameters
                                              : node.runtime_parameters;
  return plugin::NodeView(node.id, node.name, node.type, node.subtype,
                          effective);
}

/** @copydoc ps::plugin_host::operation_device_to_private */
Device operation_device_to_private(Device device) {
  switch (device) {
    case Device::CPU:
    case Device::GPU_METAL:
    case Device::GPU_CUDA:
    case Device::ASIC_NPU:
      return device;
    default:
      throw std::invalid_argument("Operation metadata has invalid device");
  }
}

/** @copydoc ps::plugin_host::operation_metadata_to_private */
OpMetadata operation_metadata_to_private(
    const plugin::OperationMetadata& metadata) {
  OpMetadata result;
  switch (metadata.tile_preference) {
    case plugin::TileSizePreference::Undefined:
      result.tile_preference = TileSizePreference::UNDEFINED;
      break;
    case plugin::TileSizePreference::Micro:
      result.tile_preference = TileSizePreference::MICRO;
      break;
    case plugin::TileSizePreference::Macro:
      result.tile_preference = TileSizePreference::MACRO;
      break;
    default:
      throw std::invalid_argument(
          "Operation metadata has invalid tile preference");
  }
  result.device_preference =
      operation_device_to_private(metadata.device_preference);
  if (metadata.cost_score < 0) {
    throw std::invalid_argument("Operation metadata cost score is negative");
  }
  result.cost_score = metadata.cost_score;
  switch (metadata.access_pattern) {
    case plugin::InputAccessPattern::SpatialAligned:
      result.access_pattern = OpMetadata::InputAccessPattern::SpatialAligned;
      break;
    case plugin::InputAccessPattern::RandomAccess:
      result.access_pattern = OpMetadata::InputAccessPattern::RandomAccess;
      break;
    default:
      throw std::invalid_argument(
          "Operation metadata has invalid input access pattern");
  }
  result.data_dependent = metadata.data_dependent;
  return result;
}

/** @copydoc ps::plugin_host::adapt_monolithic_operation */
MonolithicOpFunc adapt_monolithic_operation(
    plugin::MonolithicOperation callback,
    std::shared_ptr<void> library_lifetime) {
  return [callback = std::move(callback),
          library_lifetime = std::move(library_lifetime)](
             const Node& node,
             const std::vector<const NodeOutput*>& inputs) mutable {
    plugin::NodeView node_view = make_node_view(node);
    OperationInputStorage input_storage = make_operation_inputs(inputs);
    plugin::OperationOutput output = callback(
        node_view,
        plugin::ArrayView<plugin::OperationInputView>(input_storage.views));
    NodeOutput result = operation_output_to_private(std::move(output));
    if (library_lifetime) {
      attach_result_library_lifetime(result, library_lifetime);
    }
    return result;
  };
}

/** @copydoc ps::plugin_host::adapt_tiled_operation */
TileOpFunc adapt_tiled_operation(plugin::TiledOperation callback) {
  return [callback = std::move(callback)](
             const Node& node, const OutputTile& output,
             const std::vector<InputTile>& inputs) mutable {
    plugin::NodeView node_view = make_node_view(node);
    OutputTileView public_output{output.buffer, output.roi};
    std::vector<plugin::SpatialSnapshot> spatial;
    std::vector<plugin::OperationTileInputView> public_inputs;
    spatial.reserve(inputs.size());
    public_inputs.reserve(inputs.size());
    for (const InputTile& input : inputs) {
      const plugin::SpatialSnapshot* public_spatial = nullptr;
      if (input.spatial) {
        spatial.push_back(to_public_spatial(*input.spatial));
        public_spatial = &spatial.back();
      }
      public_inputs.push_back(plugin::OperationTileInputView{
          InputTileView{input.buffer, input.roi}, public_spatial});
    }
    callback(node_view, public_output,
             plugin::ArrayView<plugin::OperationTileInputView>(public_inputs));
  };
}

/** @copydoc ps::plugin_host::adapt_dirty_propagator */
DirtyRoiPropFunc adapt_dirty_propagator(plugin::DirtyRoiPropagator callback) {
  return [callback = std::move(callback)](
             const Node& node, const PixelRect& requested,
             const GraphModel& graph, const PixelSize& output_extent,
             const std::vector<PixelSize>& input_extents,
             const plugin::ParameterMap& effective_parameters,
             const std::vector<const NodeOutput*>* available_inputs) mutable {
    RoiInvocationStorage invocation = make_roi_invocation(
        node, graph, requested, output_extent, input_extents,
        effective_parameters, std::nullopt, available_inputs);
    const plugin::RoiContext context = invocation.view();
    return validated_kernel_rect(callback(context));
  };
}

/** @copydoc ps::plugin_host::adapt_forward_propagator */
ForwardRoiPropFunc adapt_forward_propagator(
    plugin::ForwardRoiPropagator callback) {
  return [callback = std::move(callback)](
             const Node& node, const PixelRect& requested,
             const GraphModel& graph, const PixelSize& parent_extent,
             const PixelSize& child_extent, std::size_t active_input_index,
             const std::vector<PixelSize>& input_extents,
             const plugin::ParameterMap& effective_parameters) mutable {
    std::vector<PixelSize> resolved_extents = input_extents;
    if (resolved_extents.size() <= active_input_index) {
      resolved_extents.resize(active_input_index + 1);
    }
    resolved_extents[active_input_index] = parent_extent;
    RoiInvocationStorage invocation = make_roi_invocation(
        node, graph, requested, child_extent, resolved_extents,
        effective_parameters, active_input_index);
    const plugin::RoiContext context = invocation.view();
    return validated_kernel_rect(callback(context));
  };
}

/** @copydoc ps::plugin_host::adapt_dependency_builder */
DependencyLutBuilder adapt_dependency_builder(
    plugin::DependencyLutBuilder callback) {
  return [callback = std::move(callback)](
             const Node& node, const GraphModel& graph,
             const std::vector<PixelSize>& upstream_extents,
             const PixelSize& downstream_extent,
             const plugin::ParameterMap& effective_parameters) mutable {
    RoiInvocationStorage invocation = make_roi_invocation(
        node, graph,
        PixelRect{0, 0, downstream_extent.width, downstream_extent.height},
        downstream_extent, upstream_extents, effective_parameters,
        std::nullopt);
    const plugin::RoiContext context = invocation.view();
    plugin::DependencyLutSnapshot public_lut = callback(context);
    return dependency_lut_to_private(public_lut, context);
  };
}

}  // namespace ps::plugin_host
