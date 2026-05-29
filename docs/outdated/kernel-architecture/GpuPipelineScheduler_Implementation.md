# M3.5: GPU Pipeline 调度器实现文档

## 概述

本文档描述了 M3.5 里程碑中实现的 `GpuPipelineScheduler`（又名 HeterogeneousScheduler），这是一个支持 CPU/GPU 异构调度的调度器，能够同时运行 RT（实时）和 HP（高精度）管线。

## 架构设计

### 核心特性

1. **异构调度**：HP 任务优先使用 GPU（Metal），RT 任务强制使用 CPU 以保证低延迟
2. **并发执行**：RT 和 HP 可以同时运行在不同的工作线程上
3. **优先级抢占**：RT 任务优先级高于 HP 任务，CPU 工作线程会优先处理 RT 队列
4. **Epoch 管理**：支持任务取消机制，新 Epoch 可以取消过期任务

### 调度器结构

```
GpuPipelineScheduler
├── CPU Workers (处理 RT + HP CPU 任务)
│   ├── 优先处理 RT 队列（高优先级）
│   └── 然后处理 HP CPU 队列（普通优先级）
├── GPU Workers (处理 HP GPU 任务)
│   └── 专门处理 GPU 任务队列
└── 任务队列
    ├── rt_queue_ (RT 任务，高优先级)
    ├── hp_cpu_queue_ (HP CPU 任务)
    └── gpu_queue_ (GPU 任务)
```

### 设备路由逻辑

根据 `ComputeIntent` 和可用设备自动选择最优实现：

| Intent | 设备优先级 | 说明 |
|--------|-----------|------|
| HP (GlobalHighPrecision) | GPU > CPU | 吞吐量优先，使用 GPU 加速 |
| RT (RealTimeUpdate) | CPU Tiled > GPU | 延迟优先，使用 CPU 保证响应 |

## 文件结构

```
include/kernel/scheduler/
├── gpu_pipeline_scheduler.hpp   # 调度器头文件
├── i_scheduler.hpp              # IScheduler 接口
└── scheduler_factory.hpp        # 工厂类

src/kernel/scheduler/
├── gpu_pipeline_scheduler.cpp   # 调度器实现
└── scheduler_factory.cpp        # 工厂实现（已更新）

tests/
└── test_gpu_pipeline_scheduler.cpp  # 单元测试
```

## 使用方式

### 1. 通过工厂创建

```cpp
#include "kernel/scheduler/scheduler_factory.hpp"

// 使用 "gpu_pipeline" 或 "heterogeneous" 类型名
auto scheduler = SchedulerFactory::create("gpu_pipeline");
// 或
auto scheduler = SchedulerFactory::create("heterogeneous", /*num_workers=*/4);
```

### 2. 手动配置

```cpp
#include "kernel/scheduler/gpu_pipeline_scheduler.hpp"

GpuPipelineScheduler::Config config;
config.cpu_workers = 4;           // CPU 工作线程数
config.gpu_workers = 1;           // GPU 工作线程数
config.prefer_gpu_for_hp = true;  // HP 优先使用 GPU
config.force_cpu_for_rt = true;   // RT 强制使用 CPU

auto scheduler = std::make_unique<GpuPipelineScheduler>(config);
scheduler->start();
```

### 3. 与 GraphRuntime 集成

```cpp
auto& runtime = kernel.runtime(graph_name);

// 为 HP 设置 GPU Pipeline 调度器
auto hp_scheduler = std::make_unique<GpuPipelineScheduler>(config);
hp_scheduler->start();
runtime.set_scheduler(ComputeIntent::GlobalHighPrecision, std::move(hp_scheduler));

// 为 RT 设置独立的 CPU 调度器（可选）
auto rt_scheduler = std::make_unique<CpuWorkStealingScheduler>(4);
rt_scheduler->start();
runtime.set_scheduler(ComputeIntent::RealTimeUpdate, std::move(rt_scheduler));
```

### 4. 提交计算任务

```cpp
ComputeOptions hp_opts;
hp_opts.intent = ComputeIntent::GlobalHighPrecision;
hp_opts.node_id = node_id;

ComputeOptions rt_opts;
rt_opts.intent = ComputeIntent::RealTimeUpdate;
rt_opts.node_id = node_id;

// 并发提交 HP 和 RT 任务
auto hp_future = runtime.submit_compute(hp_opts);
auto rt_future = runtime.submit_compute(rt_opts);

// RT 任务会被优先处理
auto rt_result = rt_future.get();
auto hp_result = hp_future.get();
```

## 算子多设备实现

### 注册 GPU 算子

```cpp
auto& registry = OpRegistry::instance();

// 注册 GPU (Metal) 版本
OpMetadata gpu_meta;
gpu_meta.device_preference = Device::GPU_METAL;
gpu_meta.cost_score = 50;  // 成本越低优先级越高
registry.register_impl("image_process", "gaussian_blur", Device::GPU_METAL,
                       gpu_gaussian_blur_op, gpu_meta);

// 注册 CPU 版本
OpMetadata cpu_meta;
cpu_meta.device_preference = Device::CPU;
cpu_meta.cost_score = 100;
registry.register_impl("image_process", "gaussian_blur", Device::CPU,
                       cpu_gaussian_blur_op, cpu_meta);
```

### 查询最优实现

```cpp
std::vector<Device> available = {Device::CPU, Device::GPU_METAL};

// HP 模式会选择 GPU（cost_score 更低）
auto hp_best = registry.select_best_implementation(
    "image_process", "gaussian_blur", available, ComputeIntent::GlobalHighPrecision);

// RT 模式会选择 CPU Tiled（延迟优先）
auto rt_best = registry.select_best_implementation(
    "image_process", "gaussian_blur", available, ComputeIntent::RealTimeUpdate);
```

## 测试验证

运行测试：

```bash
cd build
./tests/test_gpu_pipeline_scheduler
```

测试覆盖：
- ✅ 调度器工厂创建
- ✅ 启动/关闭生命周期
- ✅ HP 模式 GPU 优先选择
- ✅ RT 模式 CPU 优先选择
- ✅ RT 和 HP 并发执行
- ✅ RT 优先级抢占
- ✅ Epoch 任务取消
- ✅ 异常处理和传播

## 配置选项

| 选项 | 默认值 | 说明 |
|------|--------|------|
| `cpu_workers` | `hardware_concurrency` | CPU 工作线程数 |
| `gpu_workers` | 1 | GPU 工作线程数 |
| `prefer_gpu_for_hp` | true | HP 模式是否优先使用 GPU |
| `force_cpu_for_rt` | true | RT 模式是否强制使用 CPU |
| `rt_preempt_threshold_ms` | 16 | RT 抢占阈值（毫秒） |

## 未来扩展

1. **动态负载均衡**：根据 GPU 利用率动态分配任务
2. **多 GPU 支持**：扩展到多 GPU 环境
3. **任务着色**：实现 TaskGroup 以优化缓存局部性
4. **跨设备数据同步**：自动插入 UploadTask/DownloadTask

## 调度器插件系统

### 概述

从 M3.5 开始，调度器支持作为独立动态库（dylib/dll/so）编译，通过插件系统动态加载。

### 插件目录结构

```
build/
├── schedulers/                         # 调度器插件目录
│   ├── libcpu_work_stealing_plugin.dylib
│   ├── libgpu_pipeline_plugin.dylib
│   └── libserial_debug_plugin.dylib
```

### 开发新调度器插件

1. **实现调度器类**（继承 `IScheduler`）
2. **创建插件文件**使用 `PS_IMPLEMENT_SINGLE_SCHEDULER_PLUGIN` 宏：

```cpp
#include "kernel/scheduler/scheduler_plugin_api.hpp"
#include "my_scheduler.hpp"

PS_IMPLEMENT_SINGLE_SCHEDULER_PLUGIN(
  "my_scheduler",
  "My custom scheduler description",
  ps::MyScheduler
)
```

3. **在 CMakeLists.txt 中添加编译规则**

### CLI 命令

```bash
# 扫描并加载调度器插件
scheduler scan

# 从指定目录加载
scheduler scan /path/to/schedulers

# 加载单个插件
scheduler load /path/to/my_scheduler.dylib

# 查看已加载的插件
scheduler plugins

# 列出所有可用调度器类型
scheduler list

# 使用插件提供的调度器
scheduler set hp my_scheduler
```

### 配置

在 `config.yaml` 中配置调度器插件目录：

```yaml
scheduler_dirs:
  - build/schedulers
  - /custom/schedulers
```

## 相关文档

- [Migration_Phase3_Scheduler.md](Migration_Phase3_Scheduler.md) - 迁移计划
- [Scheduler_Architecture_Spec.md](Scheduler_Architecture_Spec.md) - 架构规范
- [Roadmap.md](Roadmap.md) - 开发路线图
