/**
 * @file resource_demand_estimator.cpp
 * @brief Implements checked structural retained-memory estimators.
 */
#include "compute/resource_demand_estimator.hpp"

#include <cstddef>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

#include "compute/dirty_region_planner.hpp"
#include "photospider/plugin/node_view.hpp"

namespace ps::compute {
namespace {

/**
 * @brief Adds the owned capacity of one ordinary contiguous vector.
 * @tparam Value Vector element type.
 * @param values Vector whose capacity is charged.
 * @param estimate Checked destination estimator.
 * @return Nothing.
 * @throws GraphError when capacity conversion or multiplication overflows.
 */
template <typename Value>
void add_vector_capacity(const std::vector<Value>& values,
                         RetainedMemoryEstimator* estimate) {
  estimate->add_objects<Value>(static_cast<std::uint64_t>(values.capacity()));
}

/**
 * @brief Adds the packed allocation backing one `std::vector<bool>`.
 * @param values Packed Boolean vector whose bit capacity is charged.
 * @param estimate Checked destination estimator.
 * @return Nothing.
 * @throws GraphError when capacity rounding or accumulation overflows.
 */
void add_bool_vector_capacity(const std::vector<bool>& values,
                              RetainedMemoryEstimator* estimate) {
  const std::uint64_t bit_capacity =
      static_cast<std::uint64_t>(values.capacity());
  estimate->add_bytes(bit_capacity / 8U + (bit_capacity % 8U == 0U ? 0U : 1U));
}

/**
 * @brief Adds an owned string payload conservatively.
 * @param value String whose reported capacity is charged with its terminator.
 * @param estimate Checked destination estimator.
 * @return Nothing.
 * @throws GraphError when capacity plus terminator overflows.
 * @note Small-string payloads may be inline; charging them again is a
 * deliberate conservative bound, not a claim about library SSO layout.
 */
void add_string_payload(const std::string& value,
                        RetainedMemoryEstimator* estimate) {
  estimate->add_bytes(static_cast<std::uint64_t>(value.capacity()));
  estimate->add_bytes(1U);
}

/**
 * @brief Adds node and linkage storage for one ordered map.
 * @tparam Key Map key type.
 * @tparam Value Map mapped type.
 * @tparam Compare Comparator type.
 * @param values Map whose live nodes are charged.
 * @param estimate Checked destination estimator.
 * @return Nothing.
 * @throws GraphError when node storage arithmetic overflows.
 * @note Three pointers represent parent/left/right tree linkage. Allocator
 * metadata and implementation-specific color packing remain outside scope.
 */
template <typename Key, typename Value, typename Compare>
void add_map_nodes(const std::map<Key, Value, Compare>& values,
                   RetainedMemoryEstimator* estimate) {
  estimate->add_objects<typename std::map<Key, Value, Compare>::value_type>(
      static_cast<std::uint64_t>(values.size()));
  const std::uint64_t node_count = static_cast<std::uint64_t>(values.size());
  estimate->add_objects<void*>(node_count);
  estimate->add_objects<void*>(node_count);
  estimate->add_objects<void*>(node_count);
}

/**
 * @brief Adds bucket, node, linkage, and nested vector capacity for a map.
 * @tparam Value Inner vector element type.
 * @param values Unordered map with vector mapped values.
 * @param estimate Checked destination estimator.
 * @return Nothing.
 * @throws GraphError when checked structural arithmetic overflows.
 */
template <typename Value>
void add_unordered_vector_map(
    const std::unordered_map<int, std::vector<Value>>& values,
    RetainedMemoryEstimator* estimate) {
  estimate->add_objects<void*>(
      static_cast<std::uint64_t>(values.bucket_count()));
  estimate->add_objects<
      typename std::unordered_map<int, std::vector<Value>>::value_type>(
      static_cast<std::uint64_t>(values.size()));
  estimate->add_objects<void*>(static_cast<std::uint64_t>(values.size()));
  estimate->add_objects<void*>(static_cast<std::uint64_t>(values.size()));
  for (const auto& [node_id, items] : values) {
    (void)node_id;
    add_vector_capacity(items, estimate);
  }
}

/**
 * @brief Recursively adds dynamic storage for one parameter value.
 * @param value Recursive public parameter value.
 * @param estimate Checked destination estimator.
 * @return Nothing.
 * @throws GraphError when recursive structural arithmetic overflows.
 */
void add_parameter_value_dynamic(const plugin::ParameterValue& value,
                                 RetainedMemoryEstimator* estimate);

/**
 * @brief Adds node, key, and value storage for one parameter object.
 * @param object Ordered string-keyed parameter object.
 * @param estimate Checked destination estimator.
 * @return Nothing.
 * @throws GraphError when recursive structural arithmetic overflows.
 */
void add_parameter_object_dynamic(const plugin::ParameterValue::Object& object,
                                  RetainedMemoryEstimator* estimate) {
  add_map_nodes(object, estimate);
  for (const auto& [key, value] : object) {
    add_string_payload(key, estimate);
    add_parameter_value_dynamic(value, estimate);
  }
}

/** @copydoc add_parameter_value_dynamic */
void add_parameter_value_dynamic(const plugin::ParameterValue& value,
                                 RetainedMemoryEstimator* estimate) {
  if (value.is_string()) {
    add_string_payload(value.as_string(), estimate);
    return;
  }
  if (value.is_array()) {
    const plugin::ParameterValue::Array& array = value.as_array();
    add_vector_capacity(array, estimate);
    for (const plugin::ParameterValue& child : array) {
      add_parameter_value_dynamic(child, estimate);
    }
    return;
  }
  if (value.is_object()) {
    add_parameter_object_dynamic(value.as_object(), estimate);
  }
}

/**
 * @brief Adds dynamic vectors nested in one planned node summary.
 * @param work Planned node work whose vector capacities are charged.
 * @param estimate Checked destination estimator.
 * @return Nothing.
 * @throws GraphError when checked arithmetic overflows.
 */
void add_planned_work_dynamic(const PlannedNodeWork& work,
                              RetainedMemoryEstimator* estimate) {
  add_vector_capacity(work.dirty_rois, estimate);
  add_vector_capacity(work.dependency_node_ids, estimate);
  add_vector_capacity(work.dependent_node_ids, estimate);
  add_vector_capacity(work.task_ids, estimate);
}

/**
 * @brief Adds dynamic dependency ids nested in one planned task.
 * @param task Planned task whose dependency vector capacity is charged.
 * @param estimate Checked destination estimator.
 * @return Nothing.
 * @throws GraphError when checked arithmetic overflows.
 */
void add_planned_task_dynamic(const PlannedTask& task,
                              RetainedMemoryEstimator* estimate) {
  add_vector_capacity(task.dependency_task_ids, estimate);
}

/**
 * @brief Adds dynamic storage owned by one dirty-region snapshot.
 * @param snapshot Snapshot whose nested vectors/maps are inspected.
 * @param estimate Checked destination estimator.
 * @return Nothing.
 * @throws GraphError when checked structural arithmetic overflows.
 */
void add_dirty_snapshot_dynamic(const DirtyRegionSnapshot& snapshot,
                                RetainedMemoryEstimator* estimate) {
  add_vector_capacity(snapshot.dirty_source_nodes, estimate);

  estimate->add_objects<void*>(
      static_cast<std::uint64_t>(snapshot.dirty_source_state.bucket_count()));
  estimate->add_objects<decltype(snapshot.dirty_source_state)::value_type>(
      static_cast<std::uint64_t>(snapshot.dirty_source_state.size()));
  estimate->add_objects<void*>(
      static_cast<std::uint64_t>(snapshot.dirty_source_state.size()));
  estimate->add_objects<void*>(
      static_cast<std::uint64_t>(snapshot.dirty_source_state.size()));
  for (const auto& [node_id, state] : snapshot.dirty_source_state) {
    (void)node_id;
    add_vector_capacity(state.source_rois, estimate);
  }

  add_unordered_vector_map(snapshot.source_roi_records, estimate);
  add_vector_capacity(snapshot.dirty_tiles, estimate);
  add_vector_capacity(snapshot.dirty_monolithic_nodes, estimate);
  add_unordered_vector_map(snapshot.per_node_dirty_rois, estimate);
  add_unordered_vector_map(snapshot.actual_dirty_rois, estimate);
  add_vector_capacity(snapshot.edge_mappings, estimate);
}

}  // namespace

/** @copydoc compute_plan_dynamic_retained_memory_bytes */
std::uint64_t compute_plan_dynamic_retained_memory_bytes(
    const ComputePlan& plan) {
  RetainedMemoryEstimator estimate("ComputePlan");
  add_vector_capacity(plan.execution_order, &estimate);
  add_vector_capacity(plan.planned_nodes, &estimate);
  add_vector_capacity(plan.planned_work, &estimate);
  for (const PlannedNodeWork& work : plan.planned_work) {
    add_planned_work_dynamic(work, &estimate);
  }
  add_vector_capacity(plan.task_graph.tasks, &estimate);
  for (const PlannedTask& task : plan.task_graph.tasks) {
    add_planned_task_dynamic(task, &estimate);
  }
  add_vector_capacity(plan.task_graph.dependencies, &estimate);
  for (const PlannedDependency& dependency : plan.task_graph.dependencies) {
    add_string_payload(dependency.input_kind, &estimate);
  }
  add_vector_capacity(plan.task_graph.initial_task_ids, &estimate);
  return estimate.bytes();
}

/** @copydoc dirty_selection_dynamic_retained_memory_bytes */
std::uint64_t dirty_selection_dynamic_retained_memory_bytes(
    const DirtyTaskSelectionOverlay& selection) {
  RetainedMemoryEstimator estimate("DirtyTaskSelectionOverlay");
  add_vector_capacity(selection.active_task_ids, &estimate);
  add_vector_capacity(selection.dirty_source_task_ids, &estimate);
  add_vector_capacity(selection.downstream_task_ids, &estimate);
  add_vector_capacity(selection.initial_downstream_task_ids, &estimate);
  add_bool_vector_capacity(selection.active_task_flags, &estimate);
  add_bool_vector_capacity(selection.source_boundary_task_flags, &estimate);
  add_vector_capacity(selection.dependency_task_ids, &estimate);
  for (const std::vector<int>& dependencies : selection.dependency_task_ids) {
    add_vector_capacity(dependencies, &estimate);
  }

  estimate.add_objects<void*>(
      static_cast<std::uint64_t>(selection.node_selections.bucket_count()));
  estimate
      .add_objects<typename decltype(selection.node_selections)::value_type>(
          static_cast<std::uint64_t>(selection.node_selections.size()));
  const std::uint64_t node_selection_count =
      static_cast<std::uint64_t>(selection.node_selections.size());
  estimate.add_objects<void*>(node_selection_count);
  estimate.add_objects<void*>(node_selection_count);
  for (const auto& [node_id, node_selection] : selection.node_selections) {
    (void)node_id;
    add_vector_capacity(node_selection.dirty_rois, &estimate);
  }

  add_vector_capacity(selection.dependencies, &estimate);
  for (const PlannedDependency& dependency : selection.dependencies) {
    add_string_payload(dependency.input_kind, &estimate);
  }
  return estimate.bytes();
}

/** @copydoc high_precision_dirty_plan_retained_memory_bytes */
std::uint64_t high_precision_dirty_plan_retained_memory_bytes(
    const HighPrecisionDirtyPlan& plan) {
  RetainedMemoryEstimator estimate("HighPrecisionDirtyPlan");
  estimate.add_objects<HighPrecisionDirtyPlan>();
  add_vector_capacity(plan.execution_order, &estimate);
  estimate.add_objects<void*>(
      static_cast<std::uint64_t>(plan.entries.bucket_count()));
  estimate.add_objects<decltype(plan.entries)::value_type>(
      static_cast<std::uint64_t>(plan.entries.size()));
  estimate.add_objects<void*>(static_cast<std::uint64_t>(plan.entries.size()));
  estimate.add_objects<void*>(static_cast<std::uint64_t>(plan.entries.size()));
  add_dirty_snapshot_dynamic(plan.snapshot, &estimate);
  return estimate.bytes();
}

/** @copydoc real_time_dirty_plan_retained_memory_bytes */
std::uint64_t real_time_dirty_plan_retained_memory_bytes(
    const RealTimeDirtyPlan& plan) {
  RetainedMemoryEstimator estimate("RealTimeDirtyPlan");
  estimate.add_objects<RealTimeDirtyPlan>();
  add_vector_capacity(plan.execution_order, &estimate);
  estimate.add_objects<void*>(
      static_cast<std::uint64_t>(plan.entries.bucket_count()));
  estimate.add_objects<decltype(plan.entries)::value_type>(
      static_cast<std::uint64_t>(plan.entries.size()));
  estimate.add_objects<void*>(static_cast<std::uint64_t>(plan.entries.size()));
  estimate.add_objects<void*>(static_cast<std::uint64_t>(plan.entries.size()));
  add_dirty_snapshot_dynamic(plan.snapshot, &estimate);
  return estimate.bytes();
}

/** @copydoc node_output_dynamic_retained_memory_bytes */
std::uint64_t node_output_dynamic_retained_memory_bytes(
    const NodeOutput& output) {
  RetainedMemoryEstimator estimate("NodeOutput");
  add_parameter_object_dynamic(output.data, &estimate);
  add_string_payload(output.debug.compute_device, &estimate);
  return estimate.bytes();
}

/** @copydoc owned_callable_retained_memory_bytes */
std::uint64_t owned_callable_retained_memory_bytes(
    std::uint64_t capture_bytes) {
  RetainedMemoryEstimator estimate("owned callback");
  estimate.add_bytes(capture_bytes);
  estimate.add_shared_control_block();
  return estimate.bytes();
}

}  // namespace ps::compute
