#include <gtest/gtest.h>

#include <algorithm>
#include <mutex>
#include <set>
#include <stdexcept>
#include <vector>

#include "adapter/buffer_adapter_opencv.hpp"
#include "graph_model.hpp"
#include "kernel/graph_runtime.hpp"
#include "kernel/interaction.hpp"
#include "kernel/services/compute-service/compute_cache_policy.hpp"
#include "kernel/services/compute-service/compute_geometry.hpp"
#include "kernel/services/compute-service/compute_metrics_recorder.hpp"
#include "kernel/services/compute-service/dirty_region_planner.hpp"
#include "kernel/services/compute-service/intent_update_coordinator.hpp"
#include "kernel/services/compute-service/node_executor.hpp"
#include "kernel/services/compute-service/node_input_resolver.hpp"
#include "kernel/services/compute-service/task_graph_planning.hpp"
#include "kernel/services/compute_service.hpp"
#include "kernel/services/graph_cache_service.hpp"
#include "kernel/services/graph_event_service.hpp"
#include "kernel/services/graph_traversal_service.hpp"
#include "kernel/services/roi_propagation_service.hpp"

namespace ps {
namespace {

Node make_node(int id, std::string type, std::string subtype) {
  Node node;
  node.id = id;
  node.name = "split_node_" + std::to_string(id);
  node.type = std::move(type);
  node.subtype = std::move(subtype);
  node.parameters = YAML::Node(YAML::NodeType::Map);
  return node;
}

NodeOutput make_image_output(int width, int height, int channels = 1,
                             float value = 1.0f) {
  NodeOutput output;
  output.image_buffer =
      make_aligned_cpu_image_buffer(width, height, channels, DataType::FLOAT32);
  toCvMat(output.image_buffer).setTo(value);
  return output;
}

void register_split_ops() {
  static std::once_flag once;
  std::call_once(once, [] {
    auto& registry = OpRegistry::instance();
    registry.register_op_hp_monolithic(
        "split_plan", "source",
        MonolithicOpFunc(
            [](const Node& node, const std::vector<const NodeOutput*>&) {
              return make_image_output(node.parameters["width"].as<int>(64),
                                       node.parameters["height"].as<int>(64));
            }));
    registry.register_op_hp_monolithic(
        "split_plan", "monolithic",
        MonolithicOpFunc(
            [](const Node&, const std::vector<const NodeOutput*>& inputs) {
              if (inputs.empty())
                throw GraphError(GraphErrc::MissingDependency, "missing input");
              return make_image_output(inputs.front()->image_buffer.width,
                                       inputs.front()->image_buffer.height);
            }));
    ps::OpMetadata micro_meta;
    micro_meta.tile_preference = ps::TileSizePreference::MICRO;
    registry.register_op_hp_tiled(
        "split_plan", "tile",
        TileOpFunc([](const Node&, const OutputTile& output_tile,
                      const std::vector<InputTile>&) {
          toCvMat(output_tile).setTo(2.0f);
        }),
        micro_meta);
    registry.register_op_rt_tiled(
        "split_plan", "tile",
        TileOpFunc([](const Node&, const OutputTile& output_tile,
                      const std::vector<InputTile>&) {
          toCvMat(output_tile).setTo(2.0f);
        }),
        micro_meta);
  });
}

compute::FullTaskGraph expand_full_task_graph(GraphModel& graph,
                                              ComputeIntent intent) {
  compute::FullTaskGraphExpander expander;
  return expander.expand(graph, intent);
}

compute::ComputePlan node_cache_pruned_plan(
    GraphModel& graph, const compute::ComputeRequest& request,
    const std::vector<int>& execution_order) {
  compute::NodeCacheTaskGraphPruner pruner;
  return pruner.prune(expand_full_task_graph(graph, request.intent), request,
                      execution_order, graph);
}

compute::ComputePlan dirty_snapshot_pruned_plan(
    const compute::ComputePlan& node_cache_plan,
    const compute::DirtyRegionSnapshot& snapshot) {
  compute::DirtySnapshotTaskGraphPruner pruner;
  return pruner.prune(node_cache_plan, snapshot);
}

}  // namespace

TEST(ComputeGeometrySplit, CoversClippingAlignmentScalingMergingAndHalo) {
  using namespace compute;

  EXPECT_TRUE(is_rect_empty(cv::Rect(0, 0, 0, 5)));
  EXPECT_EQ(clip_rect(cv::Rect(-5, 2, 12, 10), cv::Size(10, 8)),
            cv::Rect(0, 2, 7, 6));
  EXPECT_EQ(align_rect(cv::Rect(5, 6, 10, 11), 8), cv::Rect(0, 0, 16, 24));
  EXPECT_EQ(merge_rect(cv::Rect(2, 3, 4, 5), cv::Rect(10, 1, 2, 4)),
            cv::Rect(2, 1, 10, 7));
  EXPECT_EQ(scale_down_size(cv::Size(65, 33), 4), cv::Size(17, 9));
  EXPECT_EQ(scale_down_rect(cv::Rect(3, 5, 10, 11), 4), cv::Rect(0, 1, 4, 3));
  EXPECT_EQ(scale_up_rect(cv::Rect(2, 3, 4, 5), 4), cv::Rect(8, 12, 16, 20));
  EXPECT_EQ(calculate_halo(cv::Rect(4, 4, 8, 8), 3, cv::Size(14, 20)),
            cv::Rect(1, 1, 13, 14));
}

TEST(ComputeCachePolicySplit, PreservesHpAuthorityAndRtNonAuthority) {
  Node node = make_node(1, "split", "cache");
  node.cached_output_real_time = make_image_output(4, 4);
  EXPECT_FALSE(compute::ComputeCachePolicy::has_reusable_output(node));
  EXPECT_EQ(compute::ComputeCachePolicy::reusable_output(node), nullptr);
  ASSERT_NE(compute::ComputeCachePolicy::interactive_output(node), nullptr);

  node.cached_output_high_precision = make_image_output(8, 8);
  EXPECT_EQ(
      compute::ComputeCachePolicy::reusable_output(node)->image_buffer.width,
      8);
  EXPECT_TRUE(compute::ComputeCachePolicy::can_read_disk_cache(false, false));
  EXPECT_FALSE(compute::ComputeCachePolicy::can_read_disk_cache(true, false));
  EXPECT_FALSE(compute::ComputeCachePolicy::can_read_disk_cache(false, true));

  NodeOutput& rt_target = compute::ComputeCachePolicy::ensure_target_output(
      node, ComputeIntent::RealTimeUpdate);
  EXPECT_EQ(&rt_target, &*node.cached_output_real_time);
  NodeOutput& hp_target = compute::ComputeCachePolicy::ensure_target_output(
      node, ComputeIntent::GlobalHighPrecision);
  EXPECT_EQ(&hp_target, &*node.cached_output_high_precision);
}

TEST(NodeInputResolverSplit,
     ClonesParametersTransfersInputsAndReportsMissingData) {
  GraphModel graph("cache/split-input-resolver");
  Node parent = make_node(10, "split", "parent");
  parent.cached_output_high_precision = make_image_output(12, 7);
  parent.cached_output_high_precision->data["threshold"] = YAML::Node(42);
  graph.add_node(parent);

  Node child = make_node(20, "split", "child");
  child.parameters["threshold"] = 1;
  child.parameter_inputs.push_back({10, "threshold", "threshold"});
  child.image_inputs.push_back({10, "image"});

  auto resolved = compute::NodeInputResolver::resolve(
      child,
      [&](int upstream_id) -> const NodeOutput* {
        return compute::ComputeCachePolicy::reusable_output(
            graph.node(upstream_id));
      },
      "resolver test");

  ASSERT_EQ(resolved.image_inputs.size(), 1u);
  EXPECT_EQ(child.runtime_parameters["threshold"].as<int>(), 42);
  EXPECT_EQ(child.parameters["threshold"].as<int>(), 1)
      << "runtime parameter cloning must not mutate static parameters";
  ASSERT_TRUE(child.last_input_size_hp.has_value());
  EXPECT_EQ(*child.last_input_size_hp, cv::Size(12, 7));

  Node missing_named_output = child;
  missing_named_output.parameter_inputs[0].from_output_name = "missing";
  EXPECT_THROW(compute::NodeInputResolver::resolve(
                   missing_named_output,
                   [&](int) -> const NodeOutput* {
                     return &*graph.node(10).cached_output_high_precision;
                   },
                   "resolver test"),
               GraphError);

  Node missing_image = child;
  EXPECT_THROW(
      compute::NodeInputResolver::resolve(
          missing_image, [&](int) -> const NodeOutput* { return nullptr; },
          "resolver test"),
      GraphError);
}

TEST(NodeExecutorSplit,
     SharesMonolithicTiledMixingRandomAccessAndExceptionWrapping) {
  GraphModel graph("cache/split-node-executor");

  Node mono = make_node(1, "split_exec", "mono");
  OpRegistry::OpVariant mono_op = MonolithicOpFunc(
      [](const Node&, const std::vector<const NodeOutput*>& inputs) {
        return make_image_output(inputs.front()->image_buffer.width,
                                 inputs.front()->image_buffer.height);
      });
  std::vector<const NodeOutput*> mono_inputs;
  NodeOutput mono_input = make_image_output(5, 3);
  mono_inputs.push_back(&mono_input);
  NodeOutput mono_output =
      compute::NodeExecutor::execute(graph, mono, mono_op, mono_inputs);
  EXPECT_EQ(mono_output.image_buffer.width, 5);
  EXPECT_EQ(mono_output.image_buffer.height, 3);

  Node tiled = make_node(2, "image_mixing", "tile");
  bool saw_normalized_second_input = false;
  int tiled_calls = 0;
  std::set<const ImageBuffer*> normalized_second_buffers;
  OpRegistry::OpVariant tile_op =
      TileOpFunc([&](const Node&, const OutputTile& output_tile,
                     const std::vector<InputTile>& input_tiles) {
        ASSERT_EQ(input_tiles.size(), 2u);
        ASSERT_NE(input_tiles[1].buffer, nullptr);
        ++tiled_calls;
        normalized_second_buffers.insert(input_tiles[1].buffer);
        saw_normalized_second_input = input_tiles[1].buffer->width == 8 &&
                                      input_tiles[1].buffer->height == 8 &&
                                      input_tiles[1].buffer->channels == 3;
        toCvMat(output_tile).setTo(3.0f);
      });
  NodeOutput base = make_image_output(8, 8, 3);
  NodeOutput secondary = make_image_output(4, 4, 1);
  std::vector<const NodeOutput*> tiled_inputs{&base, &secondary};
  compute::TiledExecutionConfig tiled_config;
  tiled_config.tile_size = 4;
  NodeOutput tiled_output = compute::NodeExecutor::execute(
      graph, tiled, tile_op, tiled_inputs, tiled_config);
  EXPECT_TRUE(saw_normalized_second_input);
  EXPECT_EQ(tiled_calls, 4);
  ASSERT_EQ(normalized_second_buffers.size(), 1u);
  EXPECT_NE(*normalized_second_buffers.begin(), &secondary.image_buffer);
  EXPECT_EQ(tiled_output.image_buffer.width, 8);
  EXPECT_EQ(tiled_output.image_buffer.channels, 3);

  auto& registry = OpRegistry::instance();
  registry.register_dirty_propagator(
      "split_exec", "random_tile",
      DirtyRoiPropFunc([](const Node&, const cv::Rect& roi, const GraphModel&) {
        return compute::expand_rect(roi, 2);
      }));
  Node random_node = make_node(3, "split_exec", "random_tile");
  compute::TiledExecutionConfig random_config;
  random_config.metadata = OpMetadata{};
  random_config.metadata->access_pattern =
      OpMetadata::InputAccessPattern::RandomAccess;
  EXPECT_EQ(compute::NodeExecutor::input_roi_for_tile(
                graph, random_node, cv::Rect(1, 1, 4, 4), base.image_buffer,
                random_config),
            cv::Rect(0, 0, 7, 7));

  OpRegistry::OpVariant failing_op =
      MonolithicOpFunc([](const Node&, const std::vector<const NodeOutput*>&) {
        throw std::runtime_error("boom");
        return NodeOutput{};
      });
  EXPECT_THROW(
      compute::NodeExecutor::execute(graph, mono, failing_op, mono_inputs),
      GraphError);
}

TEST(ComputeMetricsRecorderSplit, FinalizesMetadataAndDebugStatistics) {
  NodeOutput input = make_image_output(3, 3, 1, 2.0f);
  input.space.absolute_roi = cv::Rect(5, 6, 3, 3);
  NodeOutput output = make_image_output(3, 3, 1, 4.0f);

  compute::ComputeMetricsRecorder::finalize_output_metadata(output, {&input},
                                                            true, 12.7);
  EXPECT_EQ(output.space.absolute_roi, input.space.absolute_roi);
  EXPECT_GT(output.debug.timestamp_us, 0u);
  EXPECT_EQ(output.debug.execution_time_ms, 13u);
  EXPECT_EQ(output.debug.compute_device, "CPU");
  EXPECT_FLOAT_EQ(output.debug.min_val, 4.0f);
  EXPECT_FLOAT_EQ(output.debug.max_val, 4.0f);
  EXPECT_FALSE(output.debug.has_nan);
}

TEST(DirtyRegionPlannerSplit,
     ProducesGraphScopedSnapshotAndMonolithicEscalation) {
  register_split_ops();
  GraphModel graph("cache/split-dirty-planner");
  Node source = make_node(10, "split_plan", "tile");
  source.parameters["width"] = 128;
  source.parameters["height"] = 128;
  Node mono = make_node(42, "split_plan", "monolithic");
  mono.image_inputs.push_back({10, "image"});
  graph.add_node(source);
  graph.add_node(mono);
  graph.validate_topology();

  GraphTraversalService traversal;
  RoiPropagationService propagation;
  compute::DirtyRegionPlanner planner(traversal, propagation);
  auto plan = planner.plan_high_precision(graph, 42, cv::Rect(5, 5, 10, 10));

  ASSERT_TRUE(plan.entries.count(42));
  EXPECT_EQ(plan.entries.at(42).roi_hp, cv::Rect(0, 0, 128, 128))
      << "monolithic nodes must escalate local dirty work to the full output";
  EXPECT_FALSE(plan.snapshot.empty());
  EXPECT_FALSE(plan.snapshot.dirty_monolithic_nodes.empty());
  EXPECT_FALSE(plan.snapshot.dirty_source_nodes.empty());
  EXPECT_FALSE(plan.snapshot.actual_dirty_rois.empty());
  EXPECT_TRUE(plan.snapshot.per_node_dirty_rois.count(42));
  EXPECT_FALSE(plan.snapshot.edge_mappings.empty());
  EXPECT_NE(compute::DirtyRegionPlanner::describe_snapshot(plan.snapshot)
                .find("edges="),
            std::string::npos);

  EXPECT_THROW(planner.plan_real_time(graph, 42, cv::Rect()), GraphError);
}

TEST(DirtyRegionPlannerSplit,
     SourceLifecycleKeepsMembershipAndDerivesActualRegions) {
  register_split_ops();
  GraphModel graph("cache/split-dirty-source-lifecycle");
  Node source = make_node(10, "split_plan", "tile");
  source.parameters["width"] = 64;
  source.parameters["height"] = 64;
  graph.add_node(source);
  graph.validate_topology();

  GraphTraversalService traversal;
  RoiPropagationService propagation;
  compute::DirtyRegionPlanner planner(traversal, propagation);

  EXPECT_THROW(
      planner.begin_dirty_source(graph, 99, compute::DirtyDomain::HighPrecision,
                                 cv::Rect(0, 0, 8, 8)),
      GraphError);
  EXPECT_THROW(planner.begin_dirty_source(
                   graph, 10, compute::DirtyDomain::HighPrecision, cv::Rect()),
               GraphError);

  auto begin = planner.begin_dirty_source(
      graph, 10, compute::DirtyDomain::HighPrecision, cv::Rect(1, 2, 8, 8));
  EXPECT_EQ(begin.dirty_source_nodes, (std::vector<int>{10}));
  ASSERT_TRUE(begin.dirty_source_state.count(10));
  EXPECT_EQ(begin.dirty_source_state.at(10).lifecycle,
            compute::DirtySourceLifecycleState::Updating);
  EXPECT_EQ(begin.dirty_updating_count, 1u);
  EXPECT_TRUE(begin.actual_dirty_rois.count(10));
  EXPECT_FALSE(begin.dirty_tiles.empty());

  auto end =
      planner.end_dirty_source(graph, 10, compute::DirtyDomain::HighPrecision);
  EXPECT_EQ(end.dirty_source_nodes, (std::vector<int>{10}))
      << "source membership remains until the dirty generation settles";
  ASSERT_TRUE(end.dirty_source_state.count(10));
  EXPECT_EQ(end.dirty_source_state.at(10).lifecycle,
            compute::DirtySourceLifecycleState::Settled);
  EXPECT_EQ(end.dirty_updating_count, 0u);
  ASSERT_TRUE(graph.last_dirty_region_snapshot_debug.has_value());
  EXPECT_NE(graph.last_dirty_region_snapshot_debug->find("sources=1"),
            std::string::npos);
  EXPECT_NE(graph.last_dirty_region_snapshot_debug->find("actual=1"),
            std::string::npos);
}

TEST(TaskGraphPlanningSplit, PreservesSequentialParallelPlanParity) {
  register_split_ops();
  GraphModel graph("cache/split-plan-parity");
  Node independent = make_node(10, "split_plan", "source");
  independent.parameters["width"] = 16;
  independent.parameters["height"] = 16;
  Node dirty_source = make_node(42, "split_plan", "tile");
  dirty_source.parameters["width"] = 16;
  dirty_source.parameters["height"] = 16;
  Node monolithic = make_node(100, "split_plan", "monolithic");
  monolithic.image_inputs.push_back({42, "image"});
  graph.add_node(independent);
  graph.add_node(dirty_source);
  graph.add_node(monolithic);
  graph.validate_topology();

  compute::DirtyRegionSnapshot snapshot;
  snapshot.graph_generation = 7;
  snapshot.dirty_source_nodes.push_back(42);
  snapshot.per_node_dirty_rois[42].push_back(cv::Rect(0, 0, 16, 16));
  snapshot.per_node_dirty_rois[100].push_back(cv::Rect(0, 0, 8, 8));
  snapshot.actual_dirty_rois = snapshot.per_node_dirty_rois;
  snapshot.dirty_tiles.push_back({42, compute::DirtyDomain::HighPrecision,
                                  compute::DirtyTileLevel::Micro, 0, 0, 16,
                                  cv::Rect(0, 0, 16, 16)});
  snapshot.dirty_monolithic_nodes.push_back(
      {100, compute::DirtyDomain::HighPrecision, cv::Rect(0, 0, 8, 8), true});
  snapshot.edge_mappings.push_back(
      {42, 100, compute::DirtyDomain::HighPrecision, cv::Rect(0, 0, 16, 16),
       cv::Rect(0, 0, 8, 8), compute::DirtyEdgeDirection::BackwardDemand});
  std::vector<int> execution_order{10, 42, 100};

  compute::ComputeRequest sequential;
  sequential.intent = ComputeIntent::GlobalHighPrecision;
  sequential.target_node_id = 100;
  sequential.parallel = false;
  compute::ComputeRequest parallel = sequential;
  parallel.parallel = true;

  const auto sequential_base =
      node_cache_pruned_plan(graph, sequential, execution_order);
  const auto parallel_base =
      node_cache_pruned_plan(graph, parallel, execution_order);
  const auto sequential_plan =
      dirty_snapshot_pruned_plan(sequential_base, snapshot);
  const auto parallel_plan =
      dirty_snapshot_pruned_plan(parallel_base, snapshot);
  EXPECT_EQ(sequential_plan.planned_nodes, parallel_plan.planned_nodes);
  EXPECT_EQ(sequential_plan.planned_nodes, (std::vector<int>{10, 42, 100}));
  ASSERT_EQ(sequential_plan.planned_work.size(), 3u);
  EXPECT_EQ(sequential_plan.planned_work[1].node_id, 42);
  EXPECT_EQ(sequential_plan.planned_work[1].represented_hp_roi,
            cv::Rect(0, 0, 16, 16));
  EXPECT_EQ(sequential_plan.planned_work[1].execution_roi,
            cv::Rect(0, 0, 16, 16));
  EXPECT_EQ(sequential_plan.planned_work[2].node_id, 100);
  EXPECT_TRUE(sequential_plan.planned_work[2].whole_output);
  ASSERT_EQ(sequential_plan.task_graph.dependencies.size(), 1u);
  EXPECT_EQ(sequential_plan.task_graph.dependencies[0].from_node_id, 42);
  EXPECT_EQ(sequential_plan.task_graph.dependencies[0].to_node_id, 100);
  EXPECT_EQ(sequential_plan.task_graph.dependencies[0].domain,
            compute::DirtyDomain::HighPrecision);
  ASSERT_EQ(sequential_plan.task_graph.tasks.size(), 3u);
  auto task_for_node = [&](int node_id) -> const compute::PlannedTask& {
    auto it = std::find_if(sequential_plan.task_graph.tasks.begin(),
                           sequential_plan.task_graph.tasks.end(),
                           [&](const compute::PlannedTask& task) {
                             return task.node_id == node_id;
                           });
    EXPECT_NE(it, sequential_plan.task_graph.tasks.end());
    return *it;
  };
  const auto& tile_task = task_for_node(42);
  const auto& mono_task = task_for_node(100);
  EXPECT_EQ(tile_task.kind, compute::PlannedTaskKind::Tile);
  EXPECT_TRUE(tile_task.source_boundary_eligible);
  EXPECT_TRUE(tile_task.dirty_selected);
  EXPECT_EQ(tile_task.dirty_generation, 7u);
  EXPECT_EQ(mono_task.kind, compute::PlannedTaskKind::Monolithic);
  EXPECT_TRUE(mono_task.whole_output);
  EXPECT_NE(std::find(sequential_plan.task_graph.initial_task_ids.begin(),
                      sequential_plan.task_graph.initial_task_ids.end(),
                      tile_task.task_id),
            sequential_plan.task_graph.initial_task_ids.end());
  EXPECT_NE(std::find(mono_task.dependency_task_ids.begin(),
                      mono_task.dependency_task_ids.end(), tile_task.task_id),
            mono_task.dependency_task_ids.end());
  EXPECT_EQ(sequential_plan.task_graph.dependencies.size(),
            parallel_plan.task_graph.dependencies.size());
  EXPECT_EQ(sequential_plan.task_graph.tasks.size(),
            parallel_plan.task_graph.tasks.size());
}

TEST(TaskGraphPlanningSplit, ExpandsFullGraphBeforeNodeCachePruning) {
  register_split_ops();
  GraphModel graph("cache/split-full-tile-plan");
  Node source = make_node(1, "split_plan", "tile");
  source.parameters["width"] = 32;
  source.parameters["height"] = 16;
  source.cached_output_high_precision = make_image_output(32, 16);
  Node downstream = make_node(2, "split_plan", "tile");
  downstream.parameters["width"] = 32;
  downstream.parameters["height"] = 16;
  downstream.image_inputs.push_back({1, "image"});
  Node unrelated = make_node(99, "split_plan", "tile");
  unrelated.parameters["width"] = 32;
  unrelated.parameters["height"] = 16;
  graph.add_node(source);
  graph.add_node(downstream);
  graph.add_node(unrelated);
  graph.validate_topology();

  const auto full_graph =
      expand_full_task_graph(graph, ComputeIntent::GlobalHighPrecision);
  EXPECT_EQ(full_graph.expanded_node_ids, (std::vector<int>{1, 2, 99}));
  ASSERT_EQ(full_graph.task_graph.tasks.size(), 6u);
  EXPECT_NE(std::find_if(full_graph.task_graph.tasks.begin(),
                         full_graph.task_graph.tasks.end(),
                         [](const compute::PlannedTask& task) {
                           return task.node_id == 99;
                         }),
            full_graph.task_graph.tasks.end())
      << "full expansion must include unrelated nodes before request pruning";

  compute::ComputeRequest request;
  request.intent = ComputeIntent::GlobalHighPrecision;
  request.target_node_id = 2;
  const auto plan = node_cache_pruned_plan(graph, request, {1, 2});

  EXPECT_EQ(plan.planned_nodes, (std::vector<int>{1, 2}));
  ASSERT_EQ(plan.planned_work.size(), 2u);
  EXPECT_TRUE(plan.planned_work.front().reusable_cache_available)
      << "node/cache pruning records cache state without changing full graph";
  ASSERT_EQ(plan.task_graph.tasks.size(), 4u);
  for (const auto& task : plan.task_graph.tasks) {
    EXPECT_NE(task.node_id, 99);
    EXPECT_EQ(task.kind, compute::PlannedTaskKind::Tile);
    EXPECT_EQ(task.domain, compute::DirtyDomain::HighPrecision);
    EXPECT_EQ(task.tile_size, 16);
    EXPECT_TRUE(task.dirty_selected)
        << "without a dirty snapshot, all full-frame tasks are active";
  }
}

TEST(TaskGraphPlanningSplit,
     DirtySnapshotTaskGraphPrunerExcludesSourceBoundaryTasks) {
  register_split_ops();
  GraphModel graph("cache/split-dirty-pruner");
  Node source = make_node(1, "split_plan", "tile");
  source.parameters["width"] = 32;
  source.parameters["height"] = 16;
  Node downstream = make_node(2, "split_plan", "tile");
  downstream.image_inputs.push_back({1, "image"});
  graph.add_node(source);
  graph.add_node(downstream);
  graph.validate_topology();

  compute::DirtyRegionSnapshot snapshot;
  snapshot.graph_generation = 3;
  snapshot.dirty_source_nodes.push_back(1);
  snapshot.source_roi_records[1].push_back(
      {1, compute::DirtyDomain::HighPrecision, cv::Rect(0, 0, 16, 16), 3});
  snapshot.per_node_dirty_rois[2].push_back(cv::Rect(0, 0, 16, 16));
  snapshot.actual_dirty_rois = snapshot.per_node_dirty_rois;

  compute::ComputeRequest request;
  request.intent = ComputeIntent::GlobalHighPrecision;
  request.target_node_id = 2;
  request.dirty_roi = cv::Rect(0, 0, 16, 16);
  const auto base_plan = node_cache_pruned_plan(graph, request, {1, 2});
  const auto plan = dirty_snapshot_pruned_plan(base_plan, snapshot);

  compute::DirtySnapshotTaskGraphPruner pruner;
  const auto work_set = pruner.materialize(plan, snapshot);
  EXPECT_EQ(work_set.generation, 3u);
  EXPECT_EQ(work_set.dirty_source_task_ids.size(), 2u);
  ASSERT_EQ(work_set.downstream_task_ids.size(), 1u);
  const auto& downstream_task =
      plan.task_graph.tasks.at(work_set.downstream_task_ids.front());
  EXPECT_EQ(downstream_task.node_id, 2);
  EXPECT_FALSE(downstream_task.source_boundary_eligible);
  EXPECT_TRUE(downstream_task.dirty_selected);

  compute::TaskGraphReadyChecker ready_checker;
  const auto ready = ready_checker.initial_ready_task_ids(
      plan.task_graph, &work_set.downstream_task_ids);
  EXPECT_EQ(ready, work_set.downstream_task_ids)
      << "source-boundary dependencies are satisfied by the source lane";
}

TEST(TaskGraphPlanningSplit,
     DirtySnapshotTaskGraphPrunerFiltersCrossDomainEdges) {
  register_split_ops();
  GraphModel graph("cache/split-cross-domain-pruner");
  Node source = make_node(1, "split_plan", "tile");
  source.parameters["width"] = 64;
  source.parameters["height"] = 16;
  Node downstream = make_node(2, "split_plan", "tile");
  downstream.parameters["width"] = 64;
  downstream.parameters["height"] = 16;
  downstream.image_inputs.push_back({1, "image"});
  graph.add_node(source);
  graph.add_node(downstream);
  graph.validate_topology();

  compute::DirtyRegionSnapshot snapshot;
  snapshot.graph_generation = 11;
  snapshot.per_node_dirty_rois[1].push_back(cv::Rect(0, 0, 64, 64));
  snapshot.per_node_dirty_rois[2].push_back(cv::Rect(0, 0, 64, 64));
  snapshot.dirty_tiles.push_back({1, compute::DirtyDomain::RealTime,
                                  compute::DirtyTileLevel::Micro, 0, 0, 16,
                                  cv::Rect(0, 0, 16, 16)});
  snapshot.dirty_tiles.push_back({2, compute::DirtyDomain::RealTime,
                                  compute::DirtyTileLevel::Micro, 1, 0, 16,
                                  cv::Rect(16, 0, 16, 16)});
  snapshot.edge_mappings.push_back(
      {1, 2, compute::DirtyDomain::RealTime, cv::Rect(0, 0, 64, 64),
       cv::Rect(0, 0, 64, 64), compute::DirtyEdgeDirection::BackwardDemand});
  snapshot.edge_mappings.push_back(
      {1, 2, compute::DirtyDomain::HighPrecision, cv::Rect(0, 0, 64, 64),
       cv::Rect(0, 0, 64, 64), compute::DirtyEdgeDirection::BackwardDemand});

  compute::ComputeRequest request;
  request.intent = ComputeIntent::RealTimeUpdate;
  request.target_node_id = 2;
  request.dirty_roi = cv::Rect(0, 0, 64, 64);

  const auto base_plan = node_cache_pruned_plan(graph, request, {1, 2});
  const auto plan = dirty_snapshot_pruned_plan(base_plan, snapshot);

  ASSERT_EQ(plan.task_graph.dependencies.size(), 1u);
  EXPECT_EQ(plan.task_graph.dependencies[0].domain,
            compute::DirtyDomain::RealTime);
  EXPECT_EQ(plan.task_graph.dependencies[0].from_roi, cv::Rect(0, 0, 64, 64));
  ASSERT_EQ(plan.task_graph.tasks.size(), 8u);
  for (const auto& task : plan.task_graph.tasks) {
    EXPECT_EQ(task.domain, compute::DirtyDomain::RealTime);
  }
}

TEST(IntentUpdateCoordinatorSplit,
     ValidatesRtDirtyRoiAndCoordinatesDualPathWithoutParallel) {
  EXPECT_THROW(compute::IntentUpdateCoordinator::validate(
                   ComputeIntent::RealTimeUpdate, std::nullopt),
               GraphError);
  EXPECT_NO_THROW(compute::IntentUpdateCoordinator::validate(
      ComputeIntent::RealTimeUpdate, cv::Rect(0, 0, 4, 4)));

  auto decision = compute::IntentUpdateCoordinator::decide(
      ComputeIntent::RealTimeUpdate, true, true);
  EXPECT_TRUE(decision.requires_dirty_roi);
  EXPECT_TRUE(decision.run_high_precision_update);
  EXPECT_TRUE(decision.run_real_time_update);
  EXPECT_TRUE(decision.submit_updates_concurrently);

  auto inline_decision = compute::IntentUpdateCoordinator::decide(
      ComputeIntent::RealTimeUpdate, false, true);
  EXPECT_TRUE(inline_decision.requires_dirty_roi);
  EXPECT_TRUE(inline_decision.run_high_precision_update);
  EXPECT_TRUE(inline_decision.run_real_time_update);
  EXPECT_FALSE(inline_decision.submit_updates_concurrently);

  bool ran_hp = false;
  bool ran_rt = false;
  bool ran_global_dirty = false;
  std::vector<std::string> stages;
  NodeOutput rt_output = make_image_output(4, 4);
  compute::IntentUpdateCallbacks callbacks;
  callbacks.run_global_high_precision = [&]() -> NodeOutput& {
    return rt_output;
  };
  callbacks.run_global_high_precision_dirty_update = [&]() -> NodeOutput& {
    ran_global_dirty = true;
    return rt_output;
  };
  callbacks.run_high_precision_update = [&]() { ran_hp = true; };
  callbacks.run_real_time_update = [&]() -> NodeOutput& {
    ran_rt = true;
    return rt_output;
  };
  callbacks.real_time_output = [&]() -> NodeOutput& { return rt_output; };
  callbacks.record_stage = [&](const std::string& stage) {
    stages.push_back(stage);
  };

  NodeOutput& coordinated =
      compute::IntentUpdateCoordinator::coordinate_intent_update(
          ComputeIntent::RealTimeUpdate, nullptr, nullptr, cv::Rect(0, 0, 4, 4),
          callbacks);
  EXPECT_EQ(&coordinated, &rt_output);
  EXPECT_TRUE(ran_hp);
  EXPECT_TRUE(ran_rt);
  EXPECT_NE(std::find(stages.begin(), stages.end(),
                      "intent_coordinator_decision_inline"),
            stages.end());
  EXPECT_NE(
      std::find(stages.begin(), stages.end(), "intent_coordinator_inline_hp"),
      stages.end());
  EXPECT_NE(
      std::find(stages.begin(), stages.end(), "intent_coordinator_inline_rt"),
      stages.end());

  stages.clear();
  NodeOutput& coordinated_global_dirty =
      compute::IntentUpdateCoordinator::coordinate_intent_update(
          ComputeIntent::GlobalHighPrecision, nullptr, nullptr,
          cv::Rect(0, 0, 4, 4), callbacks);
  EXPECT_EQ(&coordinated_global_dirty, &rt_output);
  EXPECT_TRUE(ran_global_dirty);
  EXPECT_NE(std::find(stages.begin(), stages.end(),
                      "intent_coordinator_global_dirty_update"),
            stages.end());
  EXPECT_FALSE(
      std::any_of(stages.begin(), stages.end(), [](const std::string& stage) {
        return stage.find("full_recompute") != std::string::npos;
      }));

  ran_hp = false;
  ran_rt = false;
  stages.clear();
  NodeOutput& coordinated_without_runtime =
      compute::IntentUpdateCoordinator::coordinate_intent_update(
          ComputeIntent::RealTimeUpdate, nullptr, nullptr, cv::Rect(0, 0, 4, 4),
          callbacks);
  EXPECT_EQ(&coordinated_without_runtime, &rt_output);
  EXPECT_TRUE(ran_hp);
  EXPECT_TRUE(ran_rt);
  EXPECT_NE(std::find(stages.begin(), stages.end(),
                      "intent_coordinator_decision_inline"),
            stages.end());
}

TEST(GlobalHighPrecisionDirtyUpdate, UsesDirtyPlanningForGlobalHpDirtyRoi) {
  register_split_ops();
  GraphModel graph("cache/global-hp-dirty-update");
  Node source = make_node(1, "split_plan", "source");
  source.parameters["width"] = 64;
  source.parameters["height"] = 64;
  graph.add_node(source);
  Node downstream = make_node(2, "split_plan", "tile");
  downstream.image_inputs.push_back({1, "image"});
  graph.add_node(downstream);
  graph.rebuild_topology_index();

  GraphTraversalService traversal;
  GraphCacheService cache;
  GraphEventService events;
  ComputeService compute(traversal, cache, events);

  NodeOutput& output = compute.compute(
      graph, ComputeIntent::GlobalHighPrecision, 2, "float32",
      false /* force */, false /* timing */, true /* disable disk cache */,
      nullptr, cv::Rect(8, 8, 16, 16));

  EXPECT_EQ(output.image_buffer.width, 64);
  EXPECT_EQ(output.image_buffer.height, 64);
  ASSERT_TRUE(graph.last_dirty_region_snapshot.has_value());
  EXPECT_FALSE(graph.last_dirty_region_snapshot->actual_dirty_rois.empty());
  ASSERT_TRUE(graph.last_compute_plan.has_value());
  EXPECT_EQ(graph.last_compute_plan->intent,
            ComputeIntent::GlobalHighPrecision);
  EXPECT_EQ(graph.last_compute_plan->target_node_id, 2);
  EXPECT_FALSE(graph.last_compute_plan->task_graph.tasks.empty());
  EXPECT_TRUE(std::any_of(
      graph.last_compute_plan->task_graph.tasks.begin(),
      graph.last_compute_plan->task_graph.tasks.end(),
      [](const compute::PlannedTask& task) { return task.dirty_selected; }));

  auto recorded_events = events.drain();
  EXPECT_TRUE(std::any_of(recorded_events.begin(), recorded_events.end(),
                          [](const GraphEventService::ComputeEvent& event) {
                            return event.source ==
                                   "intent_coordinator_global_dirty_update";
                          }));
  EXPECT_TRUE(std::any_of(recorded_events.begin(), recorded_events.end(),
                          [](const GraphEventService::ComputeEvent& event) {
                            return event.source == "hp_update";
                          }));
}

TEST(DirtySourceLifecycleFacade, UsesInteractionServicePublicBoundary) {
  Kernel kernel;
  InteractionService svc(kernel);
  svc.cmd_seed_builtin_ops();

  auto loaded = svc.cmd_load_graph("dirty_facade", "sessions",
                                   "util/testcases/dirty_region_test.yaml");
  ASSERT_TRUE(loaded.has_value());

  auto begin = svc.cmd_begin_dirty_source(
      *loaded, 1, compute::DirtyDomain::HighPrecision, cv::Rect(0, 0, 32, 32));
  ASSERT_TRUE(begin.has_value());
  EXPECT_EQ(begin->graph_generation, 1u);
  EXPECT_EQ(begin->dirty_updating_count, 1u);
  ASSERT_TRUE(begin->dirty_source_state.count(1));
  EXPECT_EQ(begin->dirty_source_state.at(1).lifecycle,
            compute::DirtySourceLifecycleState::Updating);

  auto update = svc.cmd_update_dirty_source(*loaded, 1,
                                            compute::DirtyDomain::HighPrecision,
                                            cv::Rect(16, 16, 16, 16));
  ASSERT_TRUE(update.has_value());
  EXPECT_EQ(update->graph_generation, begin->graph_generation);
  ASSERT_TRUE(update->source_roi_records.count(1));
  EXPECT_EQ(update->source_roi_records.at(1).size(), 2u);

  auto snapshot = svc.cmd_dirty_region_snapshot(*loaded);
  ASSERT_TRUE(snapshot.has_value());
  EXPECT_EQ(snapshot->graph_generation, update->graph_generation);
  EXPECT_TRUE(snapshot->actual_dirty_rois.count(1));

  auto end =
      svc.cmd_end_dirty_source(*loaded, 1, compute::DirtyDomain::HighPrecision);
  ASSERT_TRUE(end.has_value());
  EXPECT_EQ(end->dirty_updating_count, 0u);
  ASSERT_TRUE(end->dirty_source_state.count(1));
  EXPECT_EQ(end->dirty_source_state.at(1).lifecycle,
            compute::DirtySourceLifecycleState::Settled);
}

TEST(DirtyControlLaneFacade, ExposesWakeupAndCutoffThroughInteractionService) {
  Kernel kernel;
  InteractionService svc(kernel);
  svc.cmd_seed_builtin_ops();

  auto loaded = svc.cmd_load_graph("dirty_control_lane", "sessions",
                                   "util/testcases/dirty_region_test.yaml");
  ASSERT_TRUE(loaded.has_value());

  auto begin = svc.cmd_begin_dirty_source_control(
      *loaded, 1, compute::DirtyDomain::HighPrecision, cv::Rect(0, 0, 32, 32));
  ASSERT_TRUE(begin.has_value());
  EXPECT_EQ(begin->event, compute::DirtyControlEvent::Begin);
  EXPECT_EQ(begin->generation, 1u);
  EXPECT_EQ(begin->dirty_updating_count, 1u);
  EXPECT_TRUE(begin->should_wake_dispatcher);
  EXPECT_FALSE(begin->cutoff_after_downstream);

  auto update = svc.cmd_update_dirty_source_control(
      *loaded, 1, compute::DirtyDomain::HighPrecision,
      cv::Rect(16, 16, 16, 16));
  ASSERT_TRUE(update.has_value());
  EXPECT_EQ(update->event, compute::DirtyControlEvent::Update);
  EXPECT_EQ(update->generation, begin->generation);
  EXPECT_EQ(update->dirty_updating_count, 1u);
  EXPECT_TRUE(update->should_wake_dispatcher);
  EXPECT_FALSE(update->cutoff_after_downstream);
  ASSERT_TRUE(update->snapshot.source_roi_records.count(1));
  EXPECT_EQ(update->snapshot.source_roi_records.at(1).size(), 2u);

  auto end = svc.cmd_end_dirty_source_control(
      *loaded, 1, compute::DirtyDomain::HighPrecision);
  ASSERT_TRUE(end.has_value());
  EXPECT_EQ(end->event, compute::DirtyControlEvent::End);
  EXPECT_EQ(end->generation, begin->generation);
  EXPECT_EQ(end->dirty_updating_count, 0u);
  EXPECT_TRUE(end->should_wake_dispatcher);
  EXPECT_TRUE(end->cutoff_after_downstream);
  ASSERT_TRUE(end->snapshot.dirty_source_state.count(1));
  EXPECT_EQ(end->snapshot.dirty_source_state.at(1).lifecycle,
            compute::DirtySourceLifecycleState::Settled);
}

}  // namespace ps
