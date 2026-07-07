// Photospider kernel: Scheduler Plugin API
// 定义调度器插件必须实现的导出接口
#pragma once

#include "kernel/scheduler/i_scheduler.hpp"

// =============================================================================
// 调度器插件导出接口
// 每个调度器 dylib 必须实现以下 C 函数
// =============================================================================

#ifdef __cplusplus
extern "C" {
#endif

/// @brief 获取插件中注册的调度器类型数量
/// @return 调度器类型的数量
typedef int (*SchedulerPluginGetCountFunc)();

/// @brief 获取指定索引的调度器类型名称
/// @param index 调度器索引（0 到 count-1）
/// @return 调度器类型名称（如 "cpu_work_stealing"）
typedef const char* (*SchedulerPluginGetNameFunc)(int index);

/// @brief 获取指定索引的调度器描述
/// @param index 调度器索引
/// @return 调度器描述字符串
typedef const char* (*SchedulerPluginGetDescriptionFunc)(int index);

/// @brief 创建指定类型的调度器实例
/// @param type_name 调度器类型名称
/// @param num_workers 工作线程数（0 表示自动）
/// @return 调度器实例指针，调用者负责释放
typedef ps::IScheduler* (*SchedulerPluginCreateFunc)(const char* type_name,
                                                      unsigned int num_workers);

/// @brief 销毁调度器实例
/// @param scheduler 要销毁的调度器指针
typedef void (*SchedulerPluginDestroyFunc)(ps::IScheduler* scheduler);

/// @brief 获取插件版本
/// @return 版本字符串
typedef const char* (*SchedulerPluginGetVersionFunc)();

// =============================================================================
// 导出函数名称常量
// =============================================================================

#define PS_SCHEDULER_PLUGIN_GET_COUNT "ps_scheduler_plugin_get_count"
#define PS_SCHEDULER_PLUGIN_GET_NAME "ps_scheduler_plugin_get_name"
#define PS_SCHEDULER_PLUGIN_GET_DESCRIPTION "ps_scheduler_plugin_get_description"
#define PS_SCHEDULER_PLUGIN_CREATE "ps_scheduler_plugin_create"
#define PS_SCHEDULER_PLUGIN_DESTROY "ps_scheduler_plugin_destroy"
#define PS_SCHEDULER_PLUGIN_GET_VERSION "ps_scheduler_plugin_get_version"

#ifdef __cplusplus
}
#endif

// =============================================================================
// 插件实现辅助宏
// =============================================================================

/// @brief 声明一个调度器插件的标准导出函数
/// @param ... 调度器类型名称列表
#define PS_DECLARE_SCHEDULER_PLUGIN(...)                                       \
  extern "C" {                                                                 \
  int ps_scheduler_plugin_get_count();                                         \
  const char* ps_scheduler_plugin_get_name(int index);                         \
  const char* ps_scheduler_plugin_get_description(int index);                  \
  ps::IScheduler* ps_scheduler_plugin_create(const char* type_name,            \
                                              unsigned int num_workers);       \
  void ps_scheduler_plugin_destroy(ps::IScheduler* scheduler);                 \
  const char* ps_scheduler_plugin_get_version();                               \
  }

/// @brief 定义单一调度器类型的插件实现
/// @param TYPE_NAME 调度器类型名称字符串
/// @param DESCRIPTION 调度器描述字符串
/// @param CREATE_EXPR 创建调度器的表达式
#define PS_IMPLEMENT_SINGLE_SCHEDULER_PLUGIN(TYPE_NAME, DESCRIPTION, CREATE_EXPR) \
  extern "C" {                                                                    \
  int ps_scheduler_plugin_get_count() { return 1; }                               \
  const char* ps_scheduler_plugin_get_name(int index) {                           \
    return (index == 0) ? TYPE_NAME : nullptr;                                    \
  }                                                                               \
  const char* ps_scheduler_plugin_get_description(int index) {                    \
    return (index == 0) ? DESCRIPTION : nullptr;                                  \
  }                                                                               \
  ps::IScheduler* ps_scheduler_plugin_create(const char* type_name,               \
                                              unsigned int num_workers) {         \
    if (std::string(type_name) == TYPE_NAME) {                                    \
      return CREATE_EXPR;                                                         \
    }                                                                             \
    return nullptr;                                                               \
  }                                                                               \
  void ps_scheduler_plugin_destroy(ps::IScheduler* scheduler) {                   \
    delete scheduler;                                                             \
  }                                                                               \
  const char* ps_scheduler_plugin_get_version() { return "1.0.0"; }               \
  }
