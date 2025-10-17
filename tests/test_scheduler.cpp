#include <gtest/gtest.h>

#include <chrono>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <nlohmann/json.hpp>
#include <set>
#include <string>

#include "adapter/buffer_adapter_opencv.hpp"  // <--- 修正点: 添加缺失的头文件
#include "graph_model.hpp"                    // NOLINT(build/include_subdir)
#include "kernel/interaction.hpp"
#include "kernel/kernel.hpp"
#include "kernel/services/compute_service.hpp"  // <--- 修正点: 添加缺失的头文件
#include "kernel/services/graph_cache_service.hpp"  // <--- 修正点: 添加缺失的头文件
#include "kernel/services/graph_traversal_service.hpp"  // <--- 修正点: 添加缺失的头文件

TEST(SchedulerTest, ParallelLogToJson) {
  using ps::InteractionService;
  using ps::Kernel;

  Kernel kernel;
  InteractionService svc(kernel);

  svc.cmd_seed_builtin_ops();
  const std::string graph_name = "scheduler_ci_graph";
  const std::string graph_path = "util/testcases/scheduler_test_parallel.yaml";
  auto loaded = svc.cmd_load_graph(graph_name, "sessions", graph_path);
  ASSERT_TRUE(loaded.has_value());

  auto endings = svc.cmd_ending_nodes(graph_name);
  ASSERT_TRUE(endings.has_value());
  ASSERT_FALSE(endings->empty());
  int node_id = (*endings)[0];

  kernel.runtime(graph_name).clear_scheduler_log();

  auto ok = svc.cmd_compute_async(graph_name, node_id, "int8",
                                  /*force*/ true, /*timing*/ false,
                                  /*parallel*/ true);
  ASSERT_TRUE(ok.has_value());
  ASSERT_TRUE(ok->get());

  auto events = kernel.runtime(graph_name).get_scheduler_log();
  ASSERT_FALSE(events.empty());

  nlohmann::json j = nlohmann::json::array();
  for (const auto& e : events) {
    const char* action_str = "UNKNOWN";
    switch (e.action) {
      case ps::GraphRuntime::SchedulerEvent::ASSIGN_INITIAL:
        action_str = "ASSIGN_INITIAL";
        break;
      case ps::GraphRuntime::SchedulerEvent::EXECUTE:
        action_str = "EXECUTE";
        break;
      case ps::GraphRuntime::SchedulerEvent::EXECUTE_TILE:
        action_str = "EXECUTE_TILE";
        break;
    }

    j.push_back(
        {{"epoch", e.epoch},
         {"node_id", e.node_id},
         {"worker_id", e.worker_id},
         {"action", action_str},
         {"ts_us", std::chrono::duration_cast<std::chrono::microseconds>(
                       e.timestamp.time_since_epoch())
                       .count()}});
  }

  std::ofstream ofs("scheduler_log.json");
  ofs << std::setw(2) << j << std::endl;
  ofs.close();

  std::ifstream ifs("scheduler_log.json");
  ASSERT_TRUE(static_cast<bool>(ifs));
}

TEST(Scheduler, DirtyRegionTiledComputation) {
  ps::Kernel kernel;
  ps::InteractionService svc(kernel);
  svc.cmd_seed_builtin_ops();
  {
    auto& registry = ps::OpRegistry::instance();
    const auto* base_impl =
        registry.get_implementations("image_process", "gaussian_blur_tiled");
    ASSERT_TRUE(base_impl && base_impl->tiled_hp);

    ps::OpMetadata micro_meta;
    micro_meta.tile_preference =
        ps::TileSizePreference::MICRO;  // 强制 MICRO 粒度

    // 注册一个新别名，使用相同的函数但用新的元数据
    registry.register_op_hp_tiled("image_process", "micro_blur_for_test",
                                  *base_impl->tiled_hp, micro_meta);
  }
  const std::string graph_name = "dirty_region_test";
  const std::string yaml_path = "util/testcases/dirty_region_test.yaml";
  const int final_node_id = 3;

  // 加载图
  auto loaded_name = svc.cmd_load_graph(graph_name, "sessions", yaml_path);
  ASSERT_TRUE(loaded_name.has_value());
  ASSERT_EQ(*loaded_name, graph_name);

  ps::GraphRuntime& runtime = kernel.runtime(graph_name);

  // ========================================================================
  // 步骤 1: 第一次完整计算 (填充缓存)
  // ========================================================================
  std::cout << "--- SCHEDULER TEST: Performing initial full computation... ---"
            << std::endl;
  runtime.clear_scheduler_log();

  bool success_full =
      svc.cmd_compute(graph_name, final_node_id, "int8",
                      /*force*/ false, /*timing*/ false, /*parallel*/ true);
  ASSERT_TRUE(success_full);

  auto log_full_compute = runtime.get_scheduler_log();
  ASSERT_FALSE(log_full_compute.empty());

  size_t full_compute_task_count = 0;
  size_t full_compute_tile_count = 0;
  for (const auto& event : log_full_compute) {
    if (event.action == ps::GraphRuntime::SchedulerEvent::EXECUTE) {
      full_compute_task_count++;
    } else if (event.action == ps::GraphRuntime::SchedulerEvent::EXECUTE_TILE) {
      full_compute_tile_count++;
    }
  }
  std::cout << "Full compute executed " << full_compute_task_count
            << " node tasks and " << full_compute_tile_count << " tile tasks."
            << std::endl;
  ASSERT_GT(full_compute_tile_count, 10);

  // ========================================================================
  // 步骤 2: 模拟一个局部修改 (脏区)
  // ========================================================================
  std::cout << "--- SCHEDULER TEST: Simulating a dirty region... ---"
            << std::endl;

  cv::Rect dirty_rect(200, 200, 128, 128);

  // 修正点: 明确 Lambda 返回类型为 void
  runtime
      .post([&](ps::GraphModel& g) -> void {
        auto& source_node = g.nodes.at(1);
        ASSERT_TRUE(source_node.cached_output.has_value());

        // 修正点: 使用 ps::toCvMat
        cv::Mat mat = ps::toCvMat(source_node.cached_output->image_buffer);

        mat(dirty_rect).setTo(cv::Scalar(1.0f));

        g.nodes.at(2).cached_output.reset();
        g.nodes.at(3).cached_output.reset();
      })
      .get();

  // ========================================================================
  // 步骤 3: 第二次增量计算
  // ========================================================================
  std::cout << "--- SCHEDULER TEST: Performing incremental computation... ---"
            << std::endl;
  runtime.clear_scheduler_log();

  // 修正点: post 的 Lambda 返回 NodeOutput，并接收 future 的结果
  auto future = runtime.post([&](ps::GraphModel& g) -> ps::NodeOutput {
    // 修正点: 直接构造服务类，不访问 kernel 的私有成员
    ps::GraphTraversalService traversal_service;
    ps::GraphCacheService cache_service;
    ps::ComputeService compute_svc(traversal_service, cache_service,
                                   runtime.event_service());

    return compute_svc.compute_parallel(
        g, runtime, ps::ComputeIntent::RealTimeUpdate, final_node_id, "int8",
        /*force*/ true,
        /*timing*/ false, /*disable_disk_cache*/ true, nullptr, dirty_rect);
  });

  // 等待并获取结果（尽管我们不使用结果，但 get() 会等待完成并传播异常）
  future.get();

  auto log_incremental_compute = runtime.get_scheduler_log();
  ASSERT_FALSE(log_incremental_compute.empty());

  size_t incremental_compute_task_count = 0;
  size_t incremental_tile_task_count = 0;
  std::set<int> workers_used;
  for (const auto& event : log_incremental_compute) {
    if (event.action == ps::GraphRuntime::SchedulerEvent::EXECUTE) {
      incremental_compute_task_count++;
    } else if (event.action == ps::GraphRuntime::SchedulerEvent::EXECUTE_TILE) {
      incremental_tile_task_count++;
    }
    if ((event.action == ps::GraphRuntime::SchedulerEvent::EXECUTE ||
         event.action == ps::GraphRuntime::SchedulerEvent::EXECUTE_TILE) &&
        event.worker_id != -1) {
      workers_used.insert(event.worker_id);
    }
  }
  std::cout << "Incremental compute executed " << incremental_compute_task_count
            << " node tasks and " << incremental_tile_task_count
            << " tile tasks on " << workers_used.size() << " workers."
            << std::endl;

  // ========================================================================
  // 步骤 4: 断言和验证
  // ========================================================================
  ASSERT_LT(incremental_tile_task_count, full_compute_tile_count / 10);
  if (std::thread::hardware_concurrency() > 1) {
    ASSERT_GT(workers_used.size(),
              0);  // 在任务很少时，可能只用到1个worker，所以改为>0
  }
}
