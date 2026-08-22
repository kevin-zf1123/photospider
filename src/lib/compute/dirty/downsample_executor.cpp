#include "compute/dirty/downsample_executor.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <new>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "compute/compute_geometry.hpp"
#include "compute/compute_run.hpp"
#include "compute/dirty/node_executor.hpp"
#include "core/dense_image_processing.hpp"
#include "core/region_image_adapter.hpp"
#include "photospider/data/image_view.hpp"
#include "runtime/graph_event_service.hpp"
#include "runtime/graph_runtime.hpp"

namespace ps::compute {
namespace {

/**
 * @brief Copies committed proxy state for immutable downsample staging.
 *
 * @param state Committed proxy state to copy.
 * @return Independent metadata state retaining any immutable output Value.
 * @throws std::bad_alloc when output or Region metadata copying allocates.
 * @note ROI mutation never touches the retained Value. A fresh Host binding is
 * seeded from it later and replaces it only after successful seal.
 */
RealtimeProxyGraph::NodeState clone_proxy_state(
    const RealtimeProxyGraph::NodeState& state) {
  RealtimeProxyGraph::NodeState cloned;
  cloned.region_hp = state.region_hp;
  cloned.version = state.version;
  cloned.dirty_source_generation = state.dirty_source_generation;
  if (state.output) {
    cloned.output = *state.output;
  }
  return cloned;
}

/**
 * @brief Seeds one RT binding when prior proxy Value facts match exactly.
 * @param state Staged prior proxy state.
 * @param binding Fresh RT Host binding.
 * @return True after exact immutable bytes are copied; false when no compatible
 * prior image exists.
 * @throws Value-view or grant lifecycle exceptions from seed_from_value().
 * @note A mismatch is detected before mutable grant issuance.
 */
bool seed_downsample_binding(const RealtimeProxyGraph::NodeState& state,
                             HostOutputBinding& binding) {
  if (!state.output.has_value() || !state.output->has_image_value()) {
    return false;
  }
  const Value& value = state.output->image_value();
  if (!(value.dense_tensor_descriptor() == binding.plan().descriptor()) ||
      !value.image_facet().has_value() ||
      !(*value.image_facet() == binding.plan().image_facet())) {
    return false;
  }
  binding.seed_from_value(value);
  return true;
}

/**
 * @brief Copies one immutable Value ROI into an active RT binding.
 * @param source Full-extent Ready host-readable image Value.
 * @param roi Nonempty zero-origin RT selection.
 * @param binding Open RT output binding.
 * @return Nothing after exact row spans retire successfully.
 * @throws std::invalid_argument when Value facts or ROI disagree with plan.
 * @throws ReadyFenceAccessError or BufferAccessError for inaccessible bytes.
 * @throws std::overflow_error when row arithmetic is unrepresentable.
 * @throws std::logic_error or std::system_error from grant lifecycle failure.
 * @note Any exception after issuance retires the grant as failure, preventing
 * partial RT Value publication. Source row padding is never copied.
 */
void copy_downsample_value(const Value& source, const PixelRect& roi,
                           HostOutputBinding& binding) {
  const ImageView view(source);
  const DenseImageOutputPlan& plan = binding.plan();
  const PixelSize planned_size{static_cast<int>(plan.width()),
                               static_cast<int>(plan.height())};
  if (view.width() != plan.width() || view.height() != plan.height() ||
      view.channels() != plan.channels() ||
      !(view.descriptor() == plan.descriptor()) ||
      !(view.image_facet() == plan.image_facet()) || roi.width <= 0 ||
      roi.height <= 0 || !(clip_rect(roi, planned_size) == roi)) {
    throw std::invalid_argument(
        "Downsample Value disagrees with the RT output plan.");
  }
  const RegionSet logical_region =
      region_image_adapter::from_storage_pixel_rect(
          roi, plan.image_facet().data_window);
  const ImageRect& logical_roi =
      std::get<ImageRect>(logical_region.atoms().front());
  HostOutputWriteGrant grant = binding.grant_tile(logical_roi);
  try {
    const std::size_t row_bytes =
        static_cast<std::size_t>(roi.width) * plan.pixel_bytes();
    for (int row = 0; row < roi.height; ++row) {
      if (grant.span(static_cast<std::size_t>(row)).byte_size != row_bytes) {
        throw std::logic_error(
            "Downsample grant span disagrees with planned row width.");
      }
      std::byte* destination = grant.data(static_cast<std::size_t>(row));
      for (int column = 0; column < roi.width; ++column) {
        for (std::size_t channel = 0U; channel < plan.channels(); ++channel) {
          const std::size_t offset =
              static_cast<std::size_t>(column) * plan.pixel_bytes() +
              channel * plan.element_bytes();
          std::memcpy(
              destination + offset,
              view.channel_data(static_cast<std::size_t>(roi.x + column),
                                static_cast<std::size_t>(roi.y + row), channel),
              plan.element_bytes());
        }
      }
    }
    grant.retire_success();
  } catch (...) {
    try {
      if (grant.active()) {
        grant.retire_failure("Downsample Value copy failed.");
      }
    } catch (...) {
    }
    throw;
  }
}

/**
 * @brief Observes cancellation between downsample requests or before commit.
 *
 * @param run_lease Optional borrowed HP child lifecycle lease.
 * @return Nothing while cancellation has not claimed the matching Run.
 * @throws GraphError after accepted cancellation to suppress later proxy work.
 * @throws std::system_error when Run-state synchronization fails.
 * @note The Run retains the exact reason for outer service translation. One
 * One dense resize remains non-preemptible, but its result is checked before
 * proxy publication.
 */
void observe_downsample_cancellation(const ComputeRunLease* run_lease) {
  if (run_lease != nullptr && run_lease->observe_cancellation().has_value()) {
    throw GraphError(GraphErrc::ComputeError,
                     "ComputeRun cancelled during downsample execution.");
  }
}

}  // namespace

/** @copydoc DownsampleExecutor::DownsampleExecutor */
DownsampleExecutor::DownsampleExecutor(GraphModel& graph,
                                       RealtimeProxyGraph& proxy_graph,
                                       GraphRuntime* runtime,
                                       GraphEventService& events,
                                       const ComputeRunLease* run_lease)
    : graph_(graph),
      proxy_graph_(proxy_graph),
      runtime_(runtime),
      events_(events),
      run_lease_(run_lease) {}  // NOLINT

/** @copydoc DownsampleExecutor::execute */
void DownsampleExecutor::execute(const std::vector<Request>& requests) {
  for (const auto& request : requests) {
    observe_downsample_cancellation(run_lease_);
    execute_one(request);
    observe_downsample_cancellation(run_lease_);
  }
}

/** @copydoc DownsampleExecutor::execute_one */
void DownsampleExecutor::execute_one(const Request& request) {
  Node* node_ptr = find_current_node(request);
  if (!node_ptr) {
    return;
  }
  Node& node = *node_ptr;
  RealtimeProxyGraph::NodeState proxy_state;
  if (const RealtimeProxyGraph::NodeState* existing =
          proxy_graph_.find_state(node.id)) {
    proxy_state = clone_proxy_state(*existing);
  }
  if (proxy_state.version > request.hp_version) {
    log_stale_generation(node.id);
    return;
  }
  const NodeOutput& hp_output = *node.cached_output_high_precision;
  if (!hp_output.has_image_value()) {
    apply_passthrough(node, proxy_state, RegionSet::empty(),
                      request.hp_version);
    observe_downsample_cancellation(run_lease_);
    proxy_graph_.commit_node_state(node.id, std::move(proxy_state));
    return;
  }
  const Value& hp_value = hp_output.image_value();
  const ImageBounds& hp_bounds = hp_value.image_bounds();
  const std::size_t hp_width = image_bounds_width(hp_bounds);
  const std::size_t hp_height = image_bounds_height(hp_bounds);
  if (hp_width > static_cast<std::size_t>(std::numeric_limits<int>::max()) ||
      hp_height > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
    throw GraphError(GraphErrc::ComputeError,
                     "Downsample HP Value extent exceeds PixelSize.");
  }
  const PixelSize hp_size{static_cast<int>(hp_width),
                          static_cast<int>(hp_height)};
  const PixelRect roi_hp =
      normalize_hp_roi(request.region_hp, hp_bounds, hp_size);
  const RegionSet region_hp =
      region_image_adapter::from_storage_pixel_rect(roi_hp, hp_bounds);

  if (!proxy_state.output) {
    proxy_state.output = NodeOutput{};
  }
  proxy_state.output->data = hp_output.data;

  const StorageBinding hp_binding = hp_value.storage_binding();
  if (hp_binding.device.backend() != DeviceBackend::CPU ||
      !hp_binding.host_visible) {
    apply_passthrough(node, proxy_state, region_hp, request.hp_version);
    observe_downsample_cancellation(run_lease_);
    proxy_graph_.commit_node_state(node.id, std::move(proxy_state));
    return;
  }

  const PixelSize rt_size = scale_down_size(hp_size, kRtDownscaleFactor);
  if (rt_size.width <= 0 || rt_size.height <= 0) {
    apply_passthrough(node, proxy_state, region_hp, request.hp_version);
    observe_downsample_cancellation(run_lease_);
    proxy_graph_.commit_node_state(node.id, std::move(proxy_state));
    return;
  }

  const std::vector<const NodeOutput*> plan_inputs{&hp_output};
  HostOutputBinding output_binding =
      NodeExecutor::allocate_tiled_output_binding(
          node, plan_inputs, rt_size,
          TiledOutputInferenceFunc(
              NodeExecutor::infer_interpretation_preserving_output));
  const bool preserved_existing_bytes =
      seed_downsample_binding(proxy_state, output_binding);
  downsample_roi(hp_value, output_binding, roi_hp, rt_size);
  if (!proxy_state.output.has_value()) {
    proxy_state.output = NodeOutput{};
  }
  if (proxy_state.output->has_image_value()) {
    proxy_state.output->replace_image_value(output_binding.seal());
  } else {
    proxy_state.output->publish_image_value(output_binding.seal());
  }
  if (!preserved_existing_bytes) {
    proxy_state.region_hp.reset();
  }
  commit_rt_metadata(proxy_state, region_hp, request.hp_version);
  observe_downsample_cancellation(run_lease_);
  proxy_graph_.commit_node_state(node.id, std::move(proxy_state));
  events_.push(node.id, node.name, "downsample", 0.0);

  if (runtime_) {
    runtime_->log_event(GraphRuntime::ExecutionEvent::EXECUTE_TILE, node.id);
  }
}

/** @copydoc DownsampleExecutor::find_current_node */
Node* DownsampleExecutor::find_current_node(const Request& request) {
  Node* node_ptr = graph_.find_node_mutable(request.node_id);
  if (!node_ptr || !node_ptr->cached_output_high_precision) {
    return nullptr;
  }
  Node& node = *node_ptr;
  if (node.hp_version < request.hp_version) {
    log_stale_generation(node.id);
    return nullptr;
  }
  return node_ptr;
}

/** @copydoc DownsampleExecutor::normalize_hp_roi */
PixelRect DownsampleExecutor::normalize_hp_roi(const RegionSet& request_region,
                                               const ImageBounds& hp_bounds,
                                               const PixelSize& hp_size) const {
  PixelRect roi_hp = clip_rect(
      region_image_adapter::to_storage_pixel_rect(request_region, hp_bounds),
      hp_size);
  if (is_rect_empty(roi_hp) && hp_size.width > 0 && hp_size.height > 0) {
    roi_hp = PixelRect{0, 0, hp_size.width, hp_size.height};
  }
  return roi_hp;
}

/** @copydoc DownsampleExecutor::apply_passthrough */
void DownsampleExecutor::apply_passthrough(
    Node& node, RealtimeProxyGraph::NodeState& proxy_state,
    const RegionSet& region_hp, int hp_version) {
  proxy_state.output = node.cached_output_high_precision;
  commit_rt_metadata(proxy_state, region_hp, hp_version);
  events_.push(node.id, node.name, "downsample_passthrough", 0.0);
}

/** @copydoc DownsampleExecutor::downsample_roi */
PixelRect DownsampleExecutor::downsample_roi(const Value& hp_value,
                                             HostOutputBinding& output_binding,
                                             const PixelRect& roi_hp,
                                             const PixelSize& rt_size) const {
  PixelRect roi_rt =
      clip_rect(scale_down_rect(roi_hp, kRtDownscaleFactor), rt_size);
  if (is_rect_empty(roi_rt)) {
    roi_rt = PixelRect{0, 0, rt_size.width, rt_size.height};
  }

  const Value resized =
      dense_image_processing::resize_region(hp_value, roi_hp, rt_size, roi_rt);
  copy_downsample_value(resized, roi_rt, output_binding);
  return roi_rt;
}

/** @copydoc DownsampleExecutor::commit_rt_metadata */
void DownsampleExecutor::commit_rt_metadata(
    RealtimeProxyGraph::NodeState& proxy_state, const RegionSet& region_hp,
    int hp_version) {
  if (!region_hp.is_empty()) {
    if (proxy_state.region_hp.has_value()) {
      const RegionOperationResult merged =
          union_regions(*proxy_state.region_hp, region_hp);
      proxy_state.region_hp = merged.status() == RegionOperationStatus::Exact &&
                                      merged.region().has_value()
                                  ? *merged.region()
                                  : region_hp;
    } else {
      proxy_state.region_hp = region_hp;
    }
  }
  proxy_state.version = hp_version;
}

/** @copydoc DownsampleExecutor::log_stale_generation */
void DownsampleExecutor::log_stale_generation(int node_id) const {
  if (runtime_) {
    runtime_->log_event(GraphRuntime::ExecutionEvent::SKIP_STALE_GENERATION,
                        node_id);
  }
}

}  // namespace ps::compute
