#include "compute/dirty/dirty_node_executor.hpp"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <new>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

#include "compute/compute_geometry.hpp"
#include "compute/compute_run.hpp"
#include "compute/dirty/dirty_execution_common.hpp"
#include "compute/dirty/dirty_update_executor.hpp"
#include "compute/dirty/node_executor.hpp"
#include "compute/execution/execution_service.hpp"
#include "compute/request/compute_cache_policy.hpp"
#include "core/image_buffer_processing.hpp"
#include "core/ops.hpp"
#include "core/value_image_adapter.hpp"
#include "runtime/graph_event_service.hpp"
#include "runtime/graph_runtime.hpp"

namespace ps::compute {
namespace {

/**
 * @brief Replaces or first-publishes one canonical image Value in staging.
 * @param output Mutable request-local result with no compatibility staging.
 * @param value Valid sealed image Value to publish.
 * @return Nothing.
 * @throws std::invalid_argument for compatibility staging or an invalid Value.
 * @throws std::logic_error only from an inconsistent replacement state.
 * @throws std::bad_alloc when first publication allocates map/name storage.
 * @note The helper changes no Region, graph revision, HP/RT generation, or
 * formal cache state; those facts remain at the enclosing commit boundary.
 */
void publish_staged_image_value(NodeOutput* output, Value value) {
  if (output == nullptr || output->has_compatibility_image() ||
      !value.valid()) {
    throw std::invalid_argument(
        "Dirty image staging requires a destination and sealed Value.");
  }
  if (output->has_image_value()) {
    output->replace_image_value(std::move(value));
  } else {
    output->publish_image_value(std::move(value));
  }
}

/**
 * @brief Builds allocation-inference inputs with prior output precedence.
 * @param preferred Prior staged output whose format should be retained.
 * @param inputs Destination-indexed execution inputs.
 * @return Pointer vector used only before Host binding allocation.
 * @throws std::bad_alloc when vector storage cannot allocate.
 * @note Execution still receives the original input vector. The preferred
 * pointer is added only when it carries a canonical image Value.
 */
std::vector<const NodeOutput*> output_plan_inputs(
    const NodeOutput& preferred, const std::vector<const NodeOutput*>& inputs) {
  std::vector<const NodeOutput*> result;
  result.reserve(inputs.size() + (preferred.has_image_value() ? 1U : 0U));
  if (preferred.has_image_value()) {
    result.push_back(&preferred);
  }
  result.insert(result.end(), inputs.begin(), inputs.end());
  return result;
}

/**
 * @brief Seeds a fresh Host output binding from one immutable staged Value.
 *
 * @param source Prior request-local or committed output.
 * @param binding Open destination binding whose plan is already frozen.
 * @return True after every logical source element has been copied and the
 * whole grant retired; false when source lacks an exactly matching image.
 * @throws std::invalid_argument, std::out_of_range, std::overflow_error, or
 * std::bad_alloc when Value metadata/view access fails.
 * @throws std::logic_error or std::system_error from grant lifecycle failure.
 * @note A mismatch is detected before grant issuance. On any post-issuance
 * exception the grant retires as failure and makes the binding unpublishable.
 */
bool seed_output_binding(const NodeOutput& source, HostOutputBinding& binding) {
  if (!source.has_image_value()) {
    return false;
  }
  const Value& value = source.image_value();
  const DenseImageOutputPlan& plan = binding.plan();
  if (!(value.dense_tensor_descriptor() == plan.descriptor()) ||
      !value.image_facet().has_value() ||
      !(*value.image_facet() == plan.image_facet())) {
    return false;
  }

  binding.seed_from_value(value);
  return true;
}

/**
 * @brief Resolves the frozen executable tiled-task count for one node.
 * @param counts Optional product request map indexed by graph node id.
 * @param node_id Node whose shared binding will be joined and sealed.
 * @return Positive selected task count, or one for direct single-task callers.
 * @throws std::logic_error when a supplied product map omits the node or
 * records zero tasks.
 * @note The count is request metadata only. It grants no write authority and
 * is never inferred from concurrently starting callbacks.
 */
std::size_t frozen_tiled_task_count(
    const std::unordered_map<int, std::size_t>* counts, int node_id) {
  if (counts == nullptr) {
    return 1U;
  }
  const auto count_it = counts->find(node_id);
  if (count_it == counts->end() || count_it->second == 0U) {
    throw std::logic_error(
        "Dirty tiled node has no frozen selected-task count.");
  }
  return count_it->second;
}

/**
 * @brief Converts one validated Host output plan to current ImageBuffer type.
 * @param plan Immutable plan whose whole-byte element facts are inspected.
 * @return Equivalent current DataType.
 * @throws std::invalid_argument when the combination is unsupported.
 * @note This conversion is used only to validate callback-local RT scratch
 * projections and never becomes plan or Value identity authority.
 */
DataType output_plan_data_type(const DenseImageOutputPlan& plan) {
  const DenseTensorDescriptor& descriptor = plan.descriptor();
  const std::uint32_t bits = descriptor.storage_encoding.bit_width;
  switch (descriptor.element_semantics) {
    case ElementSemantics::UnsignedInteger:
      if (bits == 8U)
        return DataType::UINT8;
      if (bits == 16U)
        return DataType::UINT16;
      break;
    case ElementSemantics::SignedInteger:
      if (bits == 8U)
        return DataType::INT8;
      if (bits == 16U)
        return DataType::INT16;
      break;
    case ElementSemantics::FloatingPoint:
      if (bits == 32U)
        return DataType::FLOAT32;
      if (bits == 64U)
        return DataType::FLOAT64;
      break;
  }
  throw std::invalid_argument(
      "Dirty output plan element type has no ImageBuffer projection.");
}

/**
 * @brief Copies one compatibility scratch ROI into a checked Host tile grant.
 * @param source Read-only callback-local CPU image with full planned extent.
 * @param roi Nonempty zero-origin PixelRect selected for publication.
 * @param binding Open Host output binding.
 * @return Nothing after exact row spans retire successfully.
 * @throws std::invalid_argument when source facts or ROI disagree with plan.
 * @throws std::overflow_error when row arithmetic is unrepresentable.
 * @throws std::logic_error or std::system_error from grant lifecycle failure.
 * @note The source is algorithm scratch only. The binding remains the sole
 * mutable output allocation and any exception fails it closed.
 */
void copy_scratch_roi_to_binding(const ImageBuffer& source,
                                 const PixelRect& roi,
                                 HostOutputBinding& binding) {
  const DenseImageOutputPlan& plan = binding.plan();
  const PixelSize planned_size{static_cast<int>(plan.width()),
                               static_cast<int>(plan.height())};
  if (roi.width <= 0 || roi.height <= 0 ||
      !(clip_rect(roi, planned_size) == roi) ||
      source.width != planned_size.width ||
      source.height != planned_size.height ||
      source.channels != static_cast<int>(plan.channels()) ||
      source.type != output_plan_data_type(plan) ||
      source.device != Device::CPU || !source.data) {
    throw std::invalid_argument(
        "Dirty RT scratch image disagrees with the Host output plan.");
  }
  const ImageBounds& bounds = plan.image_facet().data_window;
  const std::int64_t x_begin =
      bounds.x_begin + static_cast<std::int64_t>(roi.x);
  const std::int64_t y_begin =
      bounds.y_begin + static_cast<std::int64_t>(roi.y);
  const ImageRect region{image_region_domain(), x_begin,
                         x_begin + static_cast<std::int64_t>(roi.width),
                         y_begin,
                         y_begin + static_cast<std::int64_t>(roi.height)};
  HostOutputWriteGrant grant = binding.grant_tile(region);
  try {
    const std::size_t row_bytes =
        static_cast<std::size_t>(roi.width) * plan.pixel_bytes();
    const std::size_t x_offset =
        static_cast<std::size_t>(roi.x) * plan.pixel_bytes();
    for (int row = 0; row < roi.height; ++row) {
      const HostOutputWriteSpan& span =
          grant.span(static_cast<std::size_t>(row));
      if (span.byte_size != row_bytes) {
        throw std::logic_error(
            "Dirty RT grant span disagrees with planned row width.");
      }
      std::memcpy(grant.data(static_cast<std::size_t>(row)),
                  image_buffer_row_data(source, roi.y + row) + x_offset,
                  row_bytes);
    }
    grant.retire_success();
  } catch (...) {
    try {
      if (grant.active()) {
        grant.retire_failure("Dirty RT scratch copy failed.");
      }
    } catch (...) {
    }
    throw;
  }
}

/**
 * @brief Observes cancellation at a dirty node or tile execution boundary.
 *
 * @param run_lease Optional borrowed lifecycle lease.
 * @return Nothing while cancellation has not claimed this child Run.
 * @throws GraphError after accepted cancellation to stop further staged work.
 * @throws std::system_error when Run-state synchronization fails.
 * @note ComputeRun retains the exact stable reason; the outer service wrapper
 * performs the public translation and this helper cannot override that outcome.
 */
void observe_dirty_node_cancellation(const ComputeRunLease* run_lease) {
  if (run_lease != nullptr && run_lease->observe_cancellation().has_value()) {
    throw GraphError(
        GraphErrc::ComputeError,
        "ComputeRun cancelled before dirty node or tile execution.");
  }
}

/**
 * @brief Acquires the exact process gate and resource vector for one direct
 * dirty provider callback.
 *
 * @param service Optional process execution authority. Null selects the
 * already-admitted physical-worker path and returns an inactive lease.
 * @param run_lease Borrowed installed Run lease required with service.
 * @param operation Frozen callback identity, metadata, and resource demand.
 * @return Move-only lease held by the caller only across provider entry.
 * @throws std::invalid_argument when service is supplied without a Run lease.
 * @throws GraphError or standard exceptions from operation gate waiting,
 * cancellation observation, and resource admission.
 * @note Input resolution and output-buffer allocation happen before callers
 * enter this helper. `ExecutionService` first copies the helper-local
 * constraints into the returned lease state, then waits for gate availability
 * without a resource reservation and admits the exact vector. The gate borrows
 * only that state-owned copy, which survives this helper's return. Destruction
 * releases the vector and gate during both ordinary return and exception
 * unwinding.
 */
OperationExecutionLease acquire_direct_dirty_operation(
    ExecutionService* service, const ComputeRunLease* run_lease,
    const DirtyResolvedOperation& operation) {
  if (service == nullptr) {
    return OperationExecutionLease{};
  }
  if (run_lease == nullptr) {
    throw std::invalid_argument(
        "Direct dirty operation execution requires a Run lease.");
  }
  const OperationExecutionConstraints constraints{
      operation.implementation_identity, operation.metadata.reentrant,
      operation.metadata.maximum_parallelism, operation.metadata.exclusive_key};
  const ReadyTaskResourceDemand demand{operation.metadata.retained_memory_bytes,
                                       operation.metadata.scratch_bytes, 0U,
                                       1U};
  return service->acquire_operation_execution(*run_lease, constraints, demand);
}

}  // namespace

/** @copydoc HighPrecisionDirtyNodeExecutor::HighPrecisionDirtyNodeExecutor */
HighPrecisionDirtyNodeExecutor::HighPrecisionDirtyNodeExecutor(
    DirtyNodeExecutionContext context,
    HighPrecisionDirtyWriteBuffer& hp_write_buffer)
    : graph_(context.graph),
      runtime_(context.runtime),
      events_(context.events),
      snapshot_(context.snapshot),
      resolved_operations_(context.resolved_operations),
      dirty_generation_(context.dirty_generation),
      hp_write_buffer_(hp_write_buffer),
      node_synchronization_(context.node_synchronization),
      run_lease_(context.run_lease),
      direct_execution_service_(context.direct_execution_service),
      tiled_task_counts_(context.tiled_task_counts) {}  // NOLINT

/** @copydoc HighPrecisionDirtyNodeExecutor::execute */
void HighPrecisionDirtyNodeExecutor::execute(Node& node,
                                             const HpPlanEntry& entry) {
  observe_dirty_node_cancellation(run_lease_);
  if (entry.region_hp.is_empty()) {
    return;
  }
  const bool dirty_source = is_dirty_source_node(snapshot_, node.id);
  if (should_skip_node(node, dirty_source)) {
    return;
  }

  log_dirty_node_execution(runtime_, node.id, dirty_source);
  Node node_for_exec;
  ResolvedNodeInputs resolved_inputs;
  {
    std::lock_guard<std::mutex> lock(node_mutex(node.id));
    node_for_exec = node;
    resolved_inputs = resolve_inputs(node_for_exec);
  }

  const auto operation_it = resolved_operations_.find(node.id);
  if (operation_it == resolved_operations_.end()) {
    throw GraphError(GraphErrc::NoOperation,
                     "No suitable HP operator (tiled or monolithic) for " +
                         node_for_exec.type + ":" + node_for_exec.subtype);
  }
  const bool tiled_operation =
      std::holds_alternative<TileOpFunc>(operation_it->second.operation);
  execute_operation(node_for_exec, entry, resolved_inputs.image_inputs,
                    operation_it->second);

  observe_dirty_node_cancellation(run_lease_);
  {
    std::lock_guard<std::mutex> lock(node_mutex(node.id));
    commit_node(node, entry, dirty_source, tiled_operation);
  }
}

ResolvedNodeInputs HighPrecisionDirtyNodeExecutor::resolve_inputs(
    Node& node) const {
  return NodeInputResolver::resolve(
      node,
      [&](int upstream_id) -> const NodeOutput* {
        if (const NodeOutput* staged =
                hp_write_buffer_.find_output(upstream_id)) {
          return staged;
        }
        const Node* upstream = graph_.find_node(upstream_id);
        if (!upstream) {
          return nullptr;
        }
        return ComputeCachePolicy::reusable_output(*upstream);
      },
      "HP update");
}

/**
 * @brief Executes the planning-time frozen HP implementation for one node.
 *
 * @param node Node being computed.
 * @param entry HP dirty ROI and extent metadata.
 * @param image_inputs_ready Resolved HP image inputs.
 * @param operation Planning-time selected monolithic or tiled operation.
 * @return Nothing.
 * @throws std::bad_alloc when staging or selected operation execution exhausts
 * memory.
 * @throws GraphError when the frozen operation fails or returns no output.
 * @note Device-aware selection, including tiled preference, completed before
 * Run admission. Output stays staged until the dirty write buffer commits.
 */
void HighPrecisionDirtyNodeExecutor::execute_operation(
    Node& node, const HpPlanEntry& entry,
    const std::vector<const NodeOutput*>& image_inputs_ready,
    const DirtyResolvedOperation& operation) const {
  if (std::holds_alternative<TileOpFunc>(operation.operation)) {
    NodeOutput staged_output;
    {
      std::lock_guard<std::mutex> lock(node_mutex(node.id));
      staged_output = hp_write_buffer_.ensure_output(node);
    }
    const std::vector<const NodeOutput*> plan_inputs =
        output_plan_inputs(staged_output, image_inputs_ready);
    HostOutputBinding& output_binding =
        hp_write_buffer_.ensure_tiled_output_binding(
            node,
            NodeExecutor::freeze_tiled_output_plan(plan_inputs, entry.hp_size),
            frozen_tiled_task_count(tiled_task_counts_, node.id));
    {
      OperationExecutionLease operation_lease = acquire_direct_dirty_operation(
          direct_execution_service_, run_lease_, operation);
      execute_tiled(node, std::get<TileOpFunc>(operation.operation), entry,
                    operation.metadata, image_inputs_ready, output_binding);
    }
    return;
  }
  execute_monolithic(node, entry,
                     std::get<MonolithicOpFunc>(operation.operation), operation,
                     image_inputs_ready);
}

/** @copydoc HighPrecisionDirtyNodeExecutor::execute_tiled */
void HighPrecisionDirtyNodeExecutor::execute_tiled(
    Node& node, const TileOpFunc& tile_fn, const HpPlanEntry& entry,
    const OpMetadata& metadata,
    const std::vector<const NodeOutput*>& image_inputs_ready,
    HostOutputBinding& output_binding) const {
  TiledExecutionConfig config;
  config.tile_size = kHpMicroTileSize;
  config.output_roi = entry.roi_hp;
  config.output_size = entry.hp_size;
  config.forced_halo = entry.halo_hp;
  config.metadata = metadata;
  config.on_tile = [this](const PixelRect&) {
    observe_dirty_node_cancellation(run_lease_);
  };
  if (runtime_) {
    runtime_->log_event(GraphRuntime::ExecutionEvent::EXECUTE_TILE, node.id);
    runtime_->log_event(
        GraphRuntime::ExecutionEvent::EXECUTE_DIRTY_DOWNSTREAM_TILE, node.id);
  }
  NodeExecutor::execute_tiled_into_binding(
      graph_, node, tile_fn, image_inputs_ready, output_binding, config);
}

void HighPrecisionDirtyNodeExecutor::execute_monolithic(
    Node& node, const HpPlanEntry& entry, const MonolithicOpFunc& mono_fn,
    const DirtyResolvedOperation& operation,
    const std::vector<const NodeOutput*>& image_inputs_ready) const {
  TiledExecutionConfig config;
  config.output_region = entry.region_hp;
  NodeOutput result;
  {
    OperationExecutionLease operation_lease = acquire_direct_dirty_operation(
        direct_execution_service_, run_lease_, operation);
    result = NodeExecutor::execute(graph_, node, OpRegistry::OpVariant{mono_fn},
                                   image_inputs_ready, config);
  }
  if (!result.has_image_value() && result.data.empty()) {
    throw GraphError(GraphErrc::ComputeError,
                     "Monolithic HP operator produced no output for " +
                         node.type + ":" + node.subtype);
  }
  std::lock_guard<std::mutex> lock(node_mutex(node.id));
  if (ops::find_core_region_monolithic_operation(node.type, node.subtype,
                                                 mono_fn)
          .has_value()) {
    hp_write_buffer_.stage_region_output(node, std::move(result),
                                         entry.region_hp);
  } else {
    hp_write_buffer_.ensure_output(node) = std::move(result);
  }
}

void HighPrecisionDirtyNodeExecutor::commit_node(Node& node,
                                                 const HpPlanEntry& entry,
                                                 bool dirty_source,
                                                 bool tiled_operation) {
  hp_write_buffer_.mark_updated(node, entry.region_hp, dirty_source,
                                dirty_generation_);
  if (tiled_operation) {
    hp_write_buffer_.complete_tiled_task(node);
  }
  events_.push(node.id, node.name, "hp_update", 0.0);
}

bool HighPrecisionDirtyNodeExecutor::should_skip_node(const Node& node,
                                                      bool dirty_source) const {
  uint64_t committed_generation = 0;
  auto generation_it = graph_.dirty_source_hp_commit_generation.find(node.id);
  if (generation_it != graph_.dirty_source_hp_commit_generation.end()) {
    committed_generation = generation_it->second;
  }
  return dirty_source &&
         should_skip_stale_dirty_source(runtime_, node.id, committed_generation,
                                        dirty_generation_);
}

std::mutex& HighPrecisionDirtyNodeExecutor::node_mutex(int node_id) const {
  return node_synchronization_.mutex_for(node_id);
}

/** @copydoc RealTimeDirtyNodeExecutor::RealTimeDirtyNodeExecutor */
RealTimeDirtyNodeExecutor::RealTimeDirtyNodeExecutor(
    DirtyNodeExecutionContext context, RealtimeProxyGraph& proxy_graph,
    RealtimeProxyWriteBuffer& rt_write_buffer)
    : graph_(context.graph),
      runtime_(context.runtime),
      events_(context.events),
      snapshot_(context.snapshot),
      resolved_operations_(context.resolved_operations),
      dirty_generation_(context.dirty_generation),
      stabilized_parameters_(context.stabilized_parameters),
      proxy_graph_(proxy_graph),
      rt_write_buffer_(rt_write_buffer),
      node_synchronization_(context.node_synchronization),
      run_lease_(context.run_lease),
      direct_execution_service_(context.direct_execution_service),
      exact_factor_four_preview_(context.exact_factor_four_preview),
      tiled_task_counts_(context.tiled_task_counts) {}  // NOLINT

/** @copydoc RealTimeDirtyNodeExecutor::execute */
void RealTimeDirtyNodeExecutor::execute(Node& node, const RtPlanEntry& entry) {
  observe_dirty_node_cancellation(run_lease_);
  if (is_rect_empty(entry.roi_rt)) {
    return;
  }
  const bool dirty_source = is_dirty_source_node(snapshot_, node.id);
  if (should_skip_node(node, dirty_source)) {
    return;
  }

  log_dirty_node_execution(runtime_, node.id, dirty_source);
  Node node_for_exec;
  ResolvedNodeInputs resolved_inputs;
  {
    std::lock_guard<std::mutex> lock(node_mutex(node.id));
    node_for_exec = node;
    resolved_inputs = resolve_inputs(node_for_exec);
  }
  const auto operation_it = resolved_operations_.find(node.id);
  if (operation_it == resolved_operations_.end()) {
    throw GraphError(GraphErrc::NoOperation,
                     "No operator registered for node " + node_for_exec.type +
                         ":" + node_for_exec.subtype);
  }
  const DirtyResolvedOperation& selected_operation = operation_it->second;
  const OpRegistry::OpVariant& operation = selected_operation.operation;
  const bool tiled_operation = std::holds_alternative<TileOpFunc>(operation);
  NodeOutput staged_output;
  {
    std::lock_guard<std::mutex> lock(node_mutex(node.id));
    staged_output = rt_write_buffer_.ensure_output(node.id);
  }
  if (std::holds_alternative<MonolithicOpFunc>(operation)) {
    NodeOutput result;
    {
      OperationExecutionLease operation_lease = acquire_direct_dirty_operation(
          direct_execution_service_, run_lease_, selected_operation);
      result = std::get<MonolithicOpFunc>(operation)(
          node_for_exec, resolved_inputs.image_inputs);
    }
    if (!result.has_image_value() && result.data.empty() &&
        result.named_values.empty()) {
      throw GraphError(GraphErrc::ComputeError,
                       "Monolithic RT operator produced no output for " +
                           node_for_exec.type + ":" + node_for_exec.subtype);
    }
    bool preserved_existing_bytes = staged_output.has_image_value();
    if (result.has_image_value()) {
      const std::vector<const NodeOutput*> plan_inputs =
          output_plan_inputs(result, {});
      HostOutputBinding output_binding =
          NodeExecutor::allocate_tiled_output_binding(plan_inputs,
                                                      entry.rt_size);
      preserved_existing_bytes =
          seed_output_binding(staged_output, output_binding);
      copy_monolithic_image_roi(result, entry, output_binding,
                                exact_factor_four_preview_ && dirty_source);
      publish_staged_image_value(&result, output_binding.seal());
      staged_output = std::move(result);
    } else {
      if (staged_output.has_image_value()) {
        result.publish_image_value(staged_output.image_value());
      }
      staged_output = std::move(result);
    }
    std::lock_guard<std::mutex> lock(node_mutex(node.id));
    rt_write_buffer_.stage_output(node.id, std::move(staged_output),
                                  preserved_existing_bytes);
  } else {
    const std::vector<const NodeOutput*> plan_inputs =
        output_plan_inputs(staged_output, resolved_inputs.image_inputs);
    HostOutputBinding& output_binding =
        rt_write_buffer_.ensure_tiled_output_binding(
            node.id,
            NodeExecutor::freeze_tiled_output_plan(plan_inputs, entry.rt_size),
            frozen_tiled_task_count(tiled_task_counts_, node.id));
    {
      OperationExecutionLease operation_lease = acquire_direct_dirty_operation(
          direct_execution_service_, run_lease_, selected_operation);
      execute_tiled(node_for_exec, std::get<TileOpFunc>(operation), entry,
                    selected_operation.metadata, resolved_inputs.image_inputs,
                    output_binding);
    }
  }

  observe_dirty_node_cancellation(run_lease_);
  {
    std::lock_guard<std::mutex> lock(node_mutex(node.id));
    commit_node(node, entry, dirty_source, tiled_operation);
  }
}

ResolvedNodeInputs RealTimeDirtyNodeExecutor::resolve_inputs(Node& node) const {
  const NodeInputResolver::OutputLookup image_lookup =
      [&](int upstream_id) -> const NodeOutput* {
    if (const NodeOutput* staged = rt_write_buffer_.find_output(upstream_id)) {
      return staged;
    }
    if (const NodeOutput* proxy_output =
            proxy_graph_.find_output(upstream_id)) {
      return proxy_output;
    }
    const Node* upstream = graph_.find_node(upstream_id);
    if (!upstream) {
      return nullptr;
    }
    return ComputeCachePolicy::reusable_output(*upstream);
  };
  const NodeInputResolver::OutputLookup parameter_lookup =
      [&](int upstream_id) -> const NodeOutput* {
    if (stabilized_parameters_) {
      if (const NodeOutput* stabilized =
              stabilized_parameters_->find_parameter_output(upstream_id)) {
        return stabilized;
      }
    }
    return image_lookup(upstream_id);
  };
  return NodeInputResolver::resolve(node, image_lookup, parameter_lookup,
                                    "RT update");
}

/** @copydoc RealTimeDirtyNodeExecutor::copy_monolithic_image_roi */
void RealTimeDirtyNodeExecutor::copy_monolithic_image_roi(
    const NodeOutput& result, const RtPlanEntry& entry,
    HostOutputBinding& output_binding, bool exact_factor_four_source) const {
  if (!result.has_image_value()) {
    throw GraphError(GraphErrc::ComputeError,
                     "RT image copy requires a canonical image Value.");
  }
  const ImageBuffer source =
      value_image_adapter::snapshot_cpu_image_buffer(result.image_value());
  std::optional<ImageBuffer> normalized_result;
  const ImageBuffer* selected = &source;
  if (exact_factor_four_source) {
    normalized_result =
        make_aligned_cpu_image_buffer(entry.rt_size.width, entry.rt_size.height,
                                      source.channels, source.type);
    image_processing::exact_box_average_factor_four_region(
        source, *normalized_result, entry.roi_rt);
    selected = &*normalized_result;
  } else if (source.width != entry.rt_size.width ||
             source.height != entry.rt_size.height) {
    normalized_result =
        image_processing::resize_cpu_image_buffer(source, entry.rt_size);
    selected = &*normalized_result;
  }
  copy_scratch_roi_to_binding(*selected, entry.roi_rt, output_binding);
}

/** @copydoc RealTimeDirtyNodeExecutor::execute_tiled */
void RealTimeDirtyNodeExecutor::execute_tiled(
    Node& node, const TileOpFunc& tile_fn, const RtPlanEntry& entry,
    const OpMetadata& metadata,
    const std::vector<const NodeOutput*>& image_inputs_ready,
    HostOutputBinding& output_binding) const {
  TiledExecutionConfig config;
  config.tile_size = kRtTileSize;
  config.output_roi = entry.roi_rt;
  config.output_size = entry.rt_size;
  config.forced_halo = entry.halo_rt;
  config.metadata = metadata;
  config.on_tile = [this](const PixelRect&) {
    observe_dirty_node_cancellation(run_lease_);
  };
  NodeExecutor::execute_tiled_into_binding(
      graph_, node, tile_fn, image_inputs_ready, output_binding, config);
  if (runtime_) {
    runtime_->log_event(GraphRuntime::ExecutionEvent::EXECUTE_TILE, node.id);
    runtime_->log_event(
        GraphRuntime::ExecutionEvent::EXECUTE_DIRTY_DOWNSTREAM_TILE, node.id);
  }
}

void RealTimeDirtyNodeExecutor::commit_node(Node& node,
                                            const RtPlanEntry& entry,
                                            bool dirty_source,
                                            bool tiled_operation) {
  rt_write_buffer_.mark_updated(node.id, entry.region_hp, dirty_source,
                                dirty_generation_);
  if (tiled_operation) {
    rt_write_buffer_.complete_tiled_task(node.id);
  }
  events_.push(node.id, node.name, "rt_update", 0.0);
}

bool RealTimeDirtyNodeExecutor::should_skip_node(const Node& node,
                                                 bool dirty_source) const {
  const uint64_t committed_generation =
      proxy_graph_.dirty_source_generation(node.id);
  return dirty_source &&
         should_skip_stale_dirty_source(runtime_, node.id, committed_generation,
                                        dirty_generation_);
}

std::mutex& RealTimeDirtyNodeExecutor::node_mutex(int node_id) const {
  return node_synchronization_.mutex_for(node_id);
}

}  // namespace ps::compute
