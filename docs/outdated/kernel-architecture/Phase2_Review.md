# Phase 2 架构审查与验收报告

**日期**: 2025-12-31
**状态**: 完成 (Ready for Phase 3)

本文档记录了 Photospider 内核在 Phase 2 (Real-Time Pipeline) 结束时的架构状态、验证结果及遗留项。

## 1. 核心功能验收

### 1.1 实时路径规划 (RT Planning)
- **机制**: 基于 `ComputeIntent::RealTimeUpdate`。
- **实现**: `ComputeService::compute_real_time_update`。
- **行为**:
  - 强制使用 `kRtDownscaleFactor = 4` (即 1/16 像素量) 进行规划。
  - 使用 `kRtTileSize = 16` 进行分块。
  - 仅重算与 `dirty_roi` 及其传播区域相交的 Tile。
- **验证**: 通过 `test_propagation` 工具验证了脏区反向传播路径的正确性。

### 1.2 脏区传播 (Dirty Region Propagation)
- **机制**: `OpRegistry` 中的 `DirtyRoiPropFunc`。
- **覆盖率**:
  - `resize`: 实现了逆向坐标变换。
  - `crop`: 实现了区域交集与偏移。
  - `blur/convolve`: 实现了半径外扩。
  - 点操作: 恒等传播。
- **精度**: 目前采用包围盒（Bounding Box）策略，对于旋转等非轴对齐操作可能存在保守估计（Over-computation），但在 Phase 2 可接受。

### 1.3 双管线与收敛 (Pipeline Separation & Convergence)
- **分离**: Node 结构体中 `cached_output_real_time` 与 `cached_output_high_precision` 完全物理隔离。
- **并发**: RT 路径在主线程（或高优先级）立即执行；HP 路径在后台（普通优先级）异步推进。
- **收敛**: HP 计算完成后，通过 `DownsampleRequest` 生成高优先级任务，将高精结果下采样并覆盖 RT 缓存，同步 `rt_version`。
- **Fix**: 在 Phase 2 收尾阶段，修复了 `compute_parallel` 中 HP 更新阻塞 RT 返回的问题，实现了真正的异步后台渲染。

## 2. 关键数据流图 (Data Flow)

```mermaid
graph TD
    UserInput[User Interaction] -->|Dirty ROI| RT_Planner
    RT_Planner -->|High Priority| RT_Exec[RT Execution (Proxy 1/4)]
    RT_Exec -->|Update| RT_Cache[RT Cache]
    RT_Cache --> Display

    UserInput -.->|Async Trigger| HP_Planner
    HP_Planner -->|Normal Priority| HP_Exec[HP Execution (Full Res)]
    HP_Exec -->|Update| HP_Cache[HP Cache]
    
    HP_Exec -->|Complete| Downsample[Downsample Task]
    HP_Cache --> Downsample
    Downsample -->|Sync Version| RT_Cache
```

## 3. 遗留问题与 Phase 3 计划

1.  **HP 调度粒度**: 目前 HP 更新虽然是异步的，但在其内部仍然是串行执行 Tile 的（`compute_node_hp` 内部循环）。
    *   *Phase 3 目标*: 将 HP 的 Tile 循环拆解为独立的 `TileTask` 提交给 `GraphRuntime`，利用多核优势。
2.  **异构计算**: 目前所有计算均在 CPU。
    *   *Phase 3 目标*: 在 `GraphRuntime` 中引入 GPU Context，并利用 `OpMetadata::device_preference` 调度任务。
3.  **Task Groups**: 尚未实现 Micro->Macro 的任务组着色。
    *   *Phase 3 目标*: 引入 `TaskGroup` 概念，优化缓存局部性。

## 4. 结论

内核基础架构稳固，实时交互的延迟已降至预期范围（得益于 1/16 代理分辨率 + 异步 HP）。可以进入 Phase 3 进行异构加速与细粒度调度优化。
