#### 1. 概述
本计划旨在完成内核从“单一硬编码调度”向“多策略可插拔调度”的转型。
目标：
1.  **解耦**: `GraphRuntime` 变为纯粹的资源持有者（Context）。
2.  **策略**: 引入 `IScheduler` 接口，实现 CPU Work-Stealing 和 GPU Pipeline 等不同策略。
3.  **异构**: 升级 `OpRegistry` 以支持同一算子的多设备实现，并支持基于优先级的路由。

#### 2. 里程碑分解

| 里程碑 | 周期 | 核心任务 | 验收标准 |
| :--- | :--- | :--- | :--- |
| **M3.1: 注册表升级** | Week 1 | 改造 `OpRegistry`，引入 `Device` 枚举与 `OpImplementation` 结构。 | 单元测试能注册同一算子的 CPU 和 Metal 版本，并能按 Metadata 检索。 |
| **M3.2: 接口抽象** | Week 1 | 定义 `IScheduler` 接口；改造 `GraphRuntime` 使其持有 `IScheduler` 指针。 | 代码编译通过，无逻辑断裂。 |
| **M3.3: 默认调度器迁移** | Week 2 | 将现有的 `run_loop` 和队列逻辑搬迁至 `CpuWorkStealingScheduler`。 | 现有的 `test_scheduler` 在新架构下全部通过。 |
| **M3.4: 内核集成** | Week 2 | 修改 `Kernel::load_graph` 以支持从 Config 注入不同调度器；CLI 新增调度器指令。 | 可通过 CLI `scheduler set` 切换调度器并运行计算。 |
| **M3.5: 异构调度器** | Week 3 | 实现 `HeterogeneousScheduler` (RT/HP 分离)，集成 Metal 算子。 | 在 Mac 上能跑通 CPU/GPU 混合 Pipeline，RT 响应无阻塞。 |
