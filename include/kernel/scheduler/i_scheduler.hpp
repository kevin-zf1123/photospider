// Photospider kernel: IScheduler interface for pluggable scheduling strategies
// M3.2: Interface Abstraction - Define the IScheduler interface
#pragma once

#include <future>
#include <memory>
#include <optional>
#include <string>

#include <opencv2/core.hpp>

#include "ps_types.hpp"  // For ComputeIntent, NodeOutput

namespace ps {

class GraphRuntime;  // Forward declaration

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
  // 核心调度 API
  // ---------------------------------------------------------------------------
  
  /// @brief 核心调度入口：提交一个计算任务
  /// @param opts 计算选项，包含 intent、node_id、dirty_roi 等
  /// @return 一个 future，在计算完成后返回 NodeOutput
  /// @note 此方法是线程安全的，可以从任意线程调用
  virtual std::future<NodeOutput> schedule(const ComputeOptions& opts) = 0;

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
};

}  // namespace ps
