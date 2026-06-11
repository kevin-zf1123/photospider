#include <gtest/gtest.h>

#include <algorithm>
#include <mutex>
#include <stdexcept>
#include <vector>

#include "adapter/buffer_adapter_opencv.hpp"
#include "graph_model.hpp"
#include "kernel/graph_runtime.hpp"
#include "kernel/services/compute-service/compute_cache_policy.hpp"
#include "kernel/services/compute-service/compute_geometry.hpp"
#include "kernel/services/compute-service/compute_metrics_recorder.hpp"
#include "kernel/services/compute-service/compute_task_planner.hpp"
#include "kernel/services/compute-service/dirty_region_planner.hpp"
#include "kernel/services/compute-service/intent_update_coordinator.hpp"
#include "kernel/services/compute-service/node_executor.hpp"
#include "kernel/services/compute-service/node_input_resolver.hpp"
#include "kernel/services/graph_traversal_service.hpp"

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
  });
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
            graph.nodes.at(upstream_id));
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
                     return &*graph.nodes.at(10).cached_output_high_precision;
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
  OpRegistry::OpVariant tile_op =
      TileOpFunc([&](const Node&, const Tile& output_tile,
                     const std::vector<Tile>& input_tiles) {
        ASSERT_EQ(input_tiles.size(), 2u);
        saw_normalized_second_input = input_tiles[1].buffer->width == 8 &&
                                      input_tiles[1].buffer->height == 8 &&
                                      input_tiles[1].buffer->channels == 3;
        toCvMat(output_tile).setTo(3.0f);
      });
  NodeOutput base = make_image_output(8, 8, 3);
  NodeOutput secondary = make_image_output(4, 4, 1);
  std::vector<const NodeOutput*> tiled_inputs{&base, &secondary};
  NodeOutput tiled_output =
      compute::NodeExecutor::execute(graph, tiled, tile_op, tiled_inputs);
  EXPECT_TRUE(saw_normalized_second_input);
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
  Node source = make_node(10, "split_plan", "source");
  source.parameters["width"] = 128;
  source.parameters["height"] = 128;
  Node mono = make_node(42, "split_plan", "monolithic");
  mono.image_inputs.push_back({10, "image"});
  graph.add_node(source);
  graph.add_node(mono);
  graph.validate_topology();

  GraphTraversalService traversal;
  compute::DirtyRegionPlanner planner(traversal);
  auto plan = planner.plan_high_precision(graph, 42, cv::Rect(5, 5, 10, 10));

  ASSERT_TRUE(plan.entries.count(42));
  EXPECT_EQ(plan.entries.at(42).roi_hp, cv::Rect(0, 0, 128, 128))
      << "monolithic nodes must escalate local dirty work to the full output";
  EXPECT_FALSE(plan.snapshot.empty());
  EXPECT_FALSE(plan.snapshot.dirty_monolithic_nodes.empty());
  EXPECT_TRUE(plan.snapshot.per_node_dirty_rois.count(42));
  EXPECT_FALSE(plan.snapshot.edge_mappings.empty());
  EXPECT_NE(compute::DirtyRegionPlanner::describe_snapshot(plan.snapshot)
                .find("edges="),
            std::string::npos);

  EXPECT_THROW(planner.plan_real_time(graph, 42, cv::Rect()), GraphError);
}

TEST(ComputeTaskPlannerSplit, PreservesSequentialParallelPlanParity) {
  compute::DirtyRegionSnapshot snapshot;
  snapshot.graph_generation = 7;
  snapshot.per_node_dirty_rois[42].push_back(cv::Rect(0, 0, 16, 16));
  snapshot.per_node_dirty_rois[100].push_back(cv::Rect(0, 0, 8, 8));
  snapshot.dirty_tiles.push_back({42, compute::DirtyDomain::HighPrecision,
                                  compute::DirtyTileLevel::Micro, 0, 0, 16,
                                  cv::Rect(0, 0, 16, 16)});
  snapshot.dirty_monolithic_nodes.push_back(
      {100, compute::DirtyDomain::HighPrecision, cv::Rect(0, 0, 8, 8), true});
  snapshot.edge_mappings.push_back(
      {42, 100, compute::DirtyDomain::HighPrecision, cv::Rect(0, 0, 16, 16),
       cv::Rect(0, 0, 8, 8), compute::DirtyEdgeDirection::BackwardDemand});
  std::vector<int> execution_order{10, 42, 100};

  compute::ComputeTaskPlanner planner;
  compute::ComputeRequest sequential;
  sequential.intent = ComputeIntent::GlobalHighPrecision;
  sequential.target_node_id = 100;
  sequential.parallel = false;
  compute::ComputeRequest parallel = sequential;
  parallel.parallel = true;

  auto sequential_plan = planner.plan(sequential, execution_order, &snapshot);
  auto parallel_plan = planner.plan(parallel, execution_order, &snapshot);
  EXPECT_EQ(sequential_plan.planned_nodes, parallel_plan.planned_nodes);
  EXPECT_EQ(sequential_plan.planned_nodes, (std::vector<int>{42, 100}));
  ASSERT_EQ(sequential_plan.planned_work.size(), 2u);
  EXPECT_EQ(sequential_plan.planned_work[0].node_id, 42);
  EXPECT_EQ(sequential_plan.planned_work[0].represented_hp_roi,
            cv::Rect(0, 0, 16, 16));
  EXPECT_EQ(sequential_plan.planned_work[0].execution_roi,
            cv::Rect(0, 0, 16, 16));
  EXPECT_EQ(sequential_plan.planned_work[1].node_id, 100);
  EXPECT_TRUE(sequential_plan.planned_work[1].whole_output);
  ASSERT_EQ(sequential_plan.task_graph.dependencies.size(), 1u);
  EXPECT_EQ(sequential_plan.task_graph.dependencies[0].from_node_id, 42);
  EXPECT_EQ(sequential_plan.task_graph.dependencies[0].to_node_id, 100);
  EXPECT_EQ(sequential_plan.task_graph.dependencies[0].domain,
            compute::DirtyDomain::HighPrecision);
  ASSERT_EQ(sequential_plan.task_graph.tasks.size(), 2u);
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

TEST(ComputeTaskPlannerSplit, FiltersCrossDomainSnapshotEdges) {
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

  compute::ComputeTaskPlanner planner;
  compute::ComputeRequest request;
  request.intent = ComputeIntent::RealTimeUpdate;
  request.target_node_id = 2;
  request.dirty_roi = cv::Rect(0, 0, 64, 64);

  const auto plan = planner.plan(request, {1, 2}, &snapshot);

  ASSERT_EQ(plan.task_graph.dependencies.size(), 1u);
  EXPECT_EQ(plan.task_graph.dependencies[0].domain,
            compute::DirtyDomain::RealTime);
  ASSERT_EQ(plan.task_graph.tasks.size(), 2u);
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
  std::vector<std::string> stages;
  NodeOutput rt_output = make_image_output(4, 4);
  compute::IntentUpdateCallbacks callbacks;
  callbacks.run_global_high_precision = [&]() -> NodeOutput& {
    return rt_output;
  };
  callbacks.run_global_high_precision_dirty_recompute = [&]() -> NodeOutput& {
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

}  // namespace ps
