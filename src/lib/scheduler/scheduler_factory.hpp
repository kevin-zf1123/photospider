// Photospider kernel: Scheduler Factory
// M3.4: 根据配置字符串创建对应的调度器实例
#pragma once

#include <memory>
#include <string>
#include <vector>

#include "photospider/scheduler/scheduler.hpp"

namespace ps {

// =============================================================================
// SchedulerFactory: 调度器工厂
// 根据类型名称字符串创建对应的 IScheduler 实例
// =============================================================================
class SchedulerFactory {
 public:
  /// @brief 创建指定类型的调度器
  /// @param type_name 调度器类型名称（如 "cpu_work_stealing", "serial_debug"）
  /// @param num_workers 工作线程数（0 表示自动检测，仅对支持的调度器有效）
  /// @return 调度器实例的唯一指针，如果类型不支持则返回 nullptr
  static std::unique_ptr<IScheduler> create(const std::string& type_name,
                                            unsigned int num_workers = 0);

  /// @brief 获取所有支持的调度器类型名称
  /// @return 调度器类型名称列表
  static std::vector<std::string> supported_types();

  /// @brief 检查指定类型是否受支持
  /// @param type_name 调度器类型名称
  /// @return true 如果类型受支持
  static bool is_supported(const std::string& type_name);

  /// @brief 获取调度器类型的描述信息
  /// @param type_name 调度器类型名称
  /// @return 调度器的描述字符串
  static std::string description(const std::string& type_name);
};

}  // namespace ps
