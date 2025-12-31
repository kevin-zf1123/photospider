// Photospider kernel: IScheduler interface for pluggable scheduling strategies
// M3.2: Interface Abstraction - Define the IScheduler interface
// M3.6: Node-Level Scheduling - Scheduler becomes decision maker
#pragma once

#include <future>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <opencv2/core.hpp>

#include "ps_types.hpp"  // For ComputeIntent, NodeOutput, Device

namespace ps {

class GraphRuntime;  // Forward declaration
class Node;          // Forward declaration
class GraphModel;    // Forward declaration

// =============================================================================
// TaskGroup: 支持 Micro Tile 聚合为 Macro Task
// 用于优化 GPU 批处理：将多个小 tile 聚合为一个大任务以减少 kernel launch 开销
// =============================================================================
struct TaskGroup {
  // 组 ID
  uint64_t group_id{0};
  
  // 原始的 Micro tile 列表（16x16）
  std::vector<cv::Rect> micro_tiles;
  
  // 聚合后的边界框（用于 Macro 处理）
  cv::Rect bounding_box;
  
  // 是否已聚合为 Macro task
  bool is_aggregated{false};
  
  // 所属节点 ID
  int node_id{-1};
  
  // 计算意图
  ComputeIntent intent{ComputeIntent::GlobalHighPrecision};
  
  // Epoch（用于取消）
  uint64_t epoch{0};
  
  // 辅助方法：计算 tile 总数
  size_t tile_count() const { return micro_tiles.size(); }
  
  // 辅助方法：检查是否为空
  bool empty() const { return micro_tiles.empty(); }
  
  // 辅助方法：添加 tile 并更新边界框
  void add_tile(const cv::Rect& tile) {
    if (micro_tiles.empty()) {
      bounding_box = tile;
    } else {
      // 合并边界框
      int x0 = std::min(bounding_box.x, tile.x);
      int y0 = std::min(bounding_box.y, tile.y);
      int x1 = std::max(bounding_box.x + bounding_box.width, 
                        tile.x + tile.width);
      int y1 = std::max(bounding_box.y + bounding_box.height,
                        tile.y + tile.height);
      bounding_box = cv::Rect(x0, y0, x1 - x0, y1 - y0);
    }
    micro_tiles.push_back(tile);
  }
};

// =============================================================================
// NodeScheduleRequest: Node-Level 调度请求
// 封装了对单个节点的调度请求，让 Scheduler 能够做出设备/粒度决策
// =============================================================================
struct NodeScheduleRequest {
  // 目标节点 ID
  int node_id{-1};
  
  // 节点类型（用于查找实现）
  std::string node_type;
  std::string node_subtype;
  
  // 脏区域
  cv::Rect dirty_roi;
  
  // 计算意图
  ComputeIntent intent{ComputeIntent::GlobalHighPrecision};
  
  // Epoch
  uint64_t epoch{0};
  
  // 缓存精度
  std::string cache_precision{"int8"};
  
  // 是否强制重计算
  bool force_recache{false};
  
  // 是否启用计时
  bool enable_timing{false};
  
  // 是否禁用磁盘缓存
  bool disable_disk_cache{false};
};

// -----------------------------------------------------------------------------
// ComputeOptions: 计算请求的选项
// 封装了调度器执行一次计算所需的全部上下文信息
// -----------------------------------------------------------------------------
struct ComputeOptions {
  // 计算意图：GlobalHighPrecision (HP) 或 RealTimeUpdate (RT)
  ComputeIntent intent = ComputeIntent::GlobalHighPrecision;
  
  // 目标节点 ID
  int node_id = -1;
  
  // 脏区域（可选）：仅计算该区域内的像素
  // 如果为空，则计算全图
  std::optional<cv::Rect> dirty_roi = std::nullopt;
  
  // 缓存精度标识（如 "int8", "float32"）
  std::string cache_precision = "int8";
  
  // 是否强制重新计算（忽略缓存）
  bool force_recache = false;
  
  // 是否启用计时
  bool enable_timing = false;
  
  // 是否禁用磁盘缓存
  bool disable_disk_cache = false;
  
  // 当前 Epoch（用于任务取消）
  uint64_t epoch = 0;
};

// -----------------------------------------------------------------------------
// IScheduler: 调度策略接口
// 定义了调度器必须具备的行为。调度器不拥有数据，只操作数据。
// M3.6 升级：从 Task-Level 变为 Node-Level，让 Scheduler 成为决策者
// -----------------------------------------------------------------------------
class IScheduler {
 public:
  virtual ~IScheduler() = default;

  // ---------------------------------------------------------------------------
  // 生命周期管理
  // ---------------------------------------------------------------------------
  
  /// @brief 初始化：绑定到特定的 GraphRuntime
  /// @param runtime GraphRuntime 实例，提供对 GraphModel、MetalContext 等资源的访问
  /// @note 调度器应保存此指针但不拥有它的所有权
  virtual void attach(GraphRuntime* runtime) = 0;
  
  /// @brief 分离：从 GraphRuntime 断开连接，清理内部状态
  /// @note 在替换调度器或关闭 Runtime 时调用
  virtual void detach() = 0;
  
  /// @brief 启动调度器（启动工作线程等）
  virtual void start() = 0;
  
  /// @brief 停止调度器（等待所有任务完成并关闭工作线程）
  virtual void shutdown() = 0;

  // ---------------------------------------------------------------------------
  // 核心调度 API (Task-Level - Legacy)
  // ---------------------------------------------------------------------------
  
  /// @brief 核心调度入口：提交一个计算任务
  /// @param opts 计算选项，包含 intent、node_id、dirty_roi 等
  /// @return 一个 future，在计算完成后返回 NodeOutput
  /// @note 此方法是线程安全的，可以从任意线程调用
  /// @deprecated 推荐使用 schedule_node() 替代
  virtual std::future<NodeOutput> schedule(const ComputeOptions& opts) = 0;

  // ---------------------------------------------------------------------------
  // 核心调度 API (Node-Level - M3.6 新增)
  // 让 Scheduler 成为主动决策者：选择设备、选择粒度、编排流水线
  // ---------------------------------------------------------------------------
  
  /// @brief Node-Level 调度入口：让 Scheduler 决定如何计算一个节点
  /// @param request 节点调度请求，包含节点信息和 ROI
  /// @param graph GraphModel 引用，用于访问节点数据
  /// @return 一个 future，在计算完成后返回 NodeOutput
  /// @note Scheduler 将根据内部优先级表选择最优实现
  /// @note 默认实现会回退到 schedule()
  virtual std::future<NodeOutput> schedule_node(
      const NodeScheduleRequest& request, GraphModel& graph) {
    // 默认实现：转换为 ComputeOptions 调用 legacy API
    ComputeOptions opts;
    opts.intent = request.intent;
    opts.node_id = request.node_id;
    opts.dirty_roi = request.dirty_roi;
    opts.cache_precision = request.cache_precision;
    opts.force_recache = request.force_recache;
    opts.enable_timing = request.enable_timing;
    opts.disable_disk_cache = request.disable_disk_cache;
    opts.epoch = request.epoch;
    return schedule(opts);
  }
  
  /// @brief 批量 Node-Level 调度：同时提交多个节点请求
  /// @param requests 节点调度请求列表
  /// @param graph GraphModel 引用
  /// @return future 列表，与 requests 一一对应
  virtual std::vector<std::future<NodeOutput>> schedule_nodes(
      const std::vector<NodeScheduleRequest>& requests, GraphModel& graph) {
    std::vector<std::future<NodeOutput>> futures;
    futures.reserve(requests.size());
    for (const auto& req : requests) {
      futures.push_back(schedule_node(req, graph));
    }
    return futures;
  }
  
  /// @brief 创建 TaskGroup 用于 Micro→Macro 聚合优化
  /// @param node_id 节点 ID
  /// @param dirty_tiles 脏 tile 列表
  /// @param intent 计算意图
  /// @param epoch 当前 epoch
  /// @return TaskGroup 实例
  virtual TaskGroup create_task_group(
      int node_id,
      const std::vector<cv::Rect>& dirty_tiles,
      ComputeIntent intent,
      uint64_t epoch) {
    TaskGroup group;
    group.node_id = node_id;
    group.intent = intent;
    group.epoch = epoch;
    for (const auto& tile : dirty_tiles) {
      group.add_tile(tile);
    }
    return group;
  }
  
  /// @brief 检查是否应该将 TaskGroup 聚合为 Macro task
  /// @param group TaskGroup 实例
  /// @return true 如果应该聚合（例如 HP 模式 + GPU 可用）
  virtual bool should_aggregate_to_macro(const TaskGroup& group) const {
    // 默认策略：HP 模式且 tile 数量足够多时聚合
    return group.intent == ComputeIntent::GlobalHighPrecision &&
           group.tile_count() >= 4;
  }

  // ---------------------------------------------------------------------------
  // 状态查询
  // ---------------------------------------------------------------------------
  
  /// @brief 获取调度器名称（用于日志和 CLI 显示）
  /// @return 调度器的可读名称
  virtual std::string name() const = 0;
  
  /// @brief 获取调度器的运行时统计信息（用于 CLI 监控）
  /// @return 包含统计信息的字符串，如队列长度、执行任务数等
  virtual std::string get_stats() const = 0;
  
  /// @brief 检查调度器是否正在运行
  /// @return true 如果调度器已启动且未关闭
  virtual bool is_running() const = 0;
  
  // ---------------------------------------------------------------------------
  // 能力查询 (M3.6 新增)
  // ---------------------------------------------------------------------------
  
  /// @brief 检查调度器是否支持 Node-Level 调度
  /// @return true 如果支持 schedule_node()
  virtual bool supports_node_level_scheduling() const { return false; }
  
  /// @brief 检查调度器是否支持 TaskGroup 聚合
  /// @return true 如果支持 Micro→Macro 聚合优化
  virtual bool supports_task_group_aggregation() const { return false; }
};

}  // namespace ps
