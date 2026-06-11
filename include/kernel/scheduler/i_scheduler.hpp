// Photospider kernel: IScheduler interface for pluggable scheduling strategies
// M3.2: Interface Abstraction - Define the IScheduler interface
// Scheduler owns resource dispatch for already-planned tasks.
#pragma once

#include <string>

#include "ps_types.hpp"  // For ComputeIntent

namespace ps {

class GraphRuntime;  // Forward declaration

// -----------------------------------------------------------------------------
// IScheduler: 调度策略接口
// 定义调度器生命周期与状态查询。具体 planned-task 派发能力由
// SchedulerTaskRuntime 提供，graph-level planning 不属于 scheduler。
// -----------------------------------------------------------------------------
class IScheduler {
 public:
  virtual ~IScheduler() = default;

  // ---------------------------------------------------------------------------
  // 生命周期管理
  // ---------------------------------------------------------------------------

  /// @brief 初始化：绑定到特定的 GraphRuntime
  /// @param runtime GraphRuntime 实例，提供对 GraphModel、MetalContext
  /// 等资源的访问
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
