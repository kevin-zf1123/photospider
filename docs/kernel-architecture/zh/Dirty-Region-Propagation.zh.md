# 脏区传播与 Tile 映射规范

本文规范脏区（Dirty Region, ROI）在节点图中的传播规则、与 Tile 网格的相互映射，以及常见算子的传播策略。实现目标是：充分精确、不漏算、尽量少算，且与 Micro/Macro 粒度边界自然衔接。HP/RT 计算域与 Micro/Macro 粒度是两个独立维度：dirty propagation 可以跨图节点传播，但不能把 RT task 建模成直接连接到 HP task，反向也不成立。

本文中的 RT/HP 网格尺寸是当前实现参数，不是永久 ABI。调度器和算子可以依赖本文描述的数据流和安全性要求，但不应把具体 tile 尺寸写死为外部兼容承诺。

## 1. 基本原则

- Dirty region 来自 node-local change。发生变化的节点在自己的输出坐标空间中报告 local dirty region。
  Frontend interaction 可以更新某个 node，compute 也可以让某个 node 发现 dirty state，但发出的
  dirty region 永远由 node 产生。
- Propagation 语义由节点对应的算子提供，而不是由 `InteractionService` 或 scheduler 提供。
  算子应显式定义 dirty 和 forward propagation 行为；identity fallback 是 legacy migration
  support，不应被视为新算子的充分行为。
- 传播单位是几何区域 `cv::Rect`（或 ROI 集），而非离散 Tile 索引。
- Forward affected-region propagation 将 local dirty region 向下游映射到受影响节点和 tile。
  Backward compute-demand propagation 使用算子提供的 `propagate_dirty_roi(downstream_roi, node)`
  推导所需上游输入区域。
- 在粒度边界（Micro↔Macro）处，使用 `ReTileTask` 承接，在同一 compute domain 内完成 ROI 与 Tile 网格之间的映射与裁剪。HP/RT 同步是单独的 coordinator/cache update 职责，不是 RT 与 HP task 之间的 dirty-propagation 边。
- Dirty-region signal 只更新图级 dirty state；它们不是 compute-task trigger。`DirtyRegionNode`
  应明确自己的 lifecycle：开始创建 dirty region、用当前 ROI 更新 dirty region、结束创建 dirty
  region。之后由 compute policy 决定是在 node 关闭 region 后创建 HP 或 RT request，还是在 node
  仍在更新时把变化合并进已经活跃的 realtime request。
- `DirtyRegionPlanner` 应维护图级 dirty state，而不是强迫每个消费者独立重新计算 propagation。

## 2. 图级 Dirty Snapshot

目标 dirty-region state 是图级 `DirtyRegionSnapshot`。它应由当前 graph/runtime 的 dirty-state
层拥有，并由 `InteractionService`、dirty work-set materialization、测试和 debug tooling 消费。

Snapshot 应包含三类状态，但只有 dirty source membership 和 lifecycle 可以直接由 node lifecycle event 写入：

- `dirty_source_nodes`：当前 dirty generation 中发出过 dirty state 的 node 集合。即使触发某个
  dirty event 的局部事件已经被处理，该 node 仍保持 dirty 标记，因为下游 work 可能会一直被 abort
  或刷新，直到最后一次 dirty update 稳定下来。
- `dirty_updating_count`：从 dirty source node lifecycle 派生出的计数，表示当前处于
  begin/end dirty-region lifecycle 中的 dirty source node 数量。当它降到 0 时，executor
  可以在最后一批相关 work 结束后结束当前 compute request。它不是 compute-task reference count。
- `actual_dirty_region`：由 propagator 根据 dirty source set 产生的 dirty region、tile、
  monolithic escalation 和 edge mapping。每次 dirty source set 变化后，它会通过增量或全量
  propagation 刷新。

Snapshot 与 graph topology state 同级存在。它不是可执行的 `ComputeTaskGraph`，也不拥有运行期依赖计数器、引用计数、ready queue、task-priority queue 或 scheduler policy。这些执行期产物会在每次 update 中由 executor 和 scheduler 基于该请求的 compute plan 与当前 dirty snapshot 维护。

Snapshot 不创建 compute task。`ComputeTaskGraph` 会枚举 compute request 可用的 node task 和 tile task，包括没有 dirty ROI 时的 full-frame tiled parallelism。Dirty work-set materialization 只从该 graph 中选择或裁剪 task。

Snapshot 应避免保存原始 node 或 tile 指针。它应使用稳定 id 和坐标数据，使其在 undo/redo、
reload 和 node replacement 工作流中仍可检查。

Dirty-node lifecycle event 会通过 `Kernel` / `InteractionService` facade 进入串行的图级
`DirtyControlLane`。Control lane 更新 `DirtyRegionSnapshot` 中的 dirty source membership
和 lifecycle state；随后 propagator 从这些 source 派生 `actual_dirty_region`，并返回用于
work-set materialization 的 wakeup/cutoff 决策。它不是 scheduler 拥有的普通 compute task
queue，也不应下放为 node-local compute ownership。

TODO：定义面向未来 GUI 的 node 到 `InteractionService` subscription/event transport。Node
仍作为 realtime update event 和 dirty-region record 的来源，而 `InteractionService` 只暴露面向前端的
inspection 和 lifecycle/update-request hook，不成为 dirty-region generator。该设计必须说明事件来源、
dirty-region 生成责任、node/facade 边界以及 GUI 消费方式。

建议的内部 key：

```text
DirtyTileKey {
  node_id
  domain: HP | RT
  level: Micro | Macro
  tile_x
  tile_y
  tile_size
  pixel_roi
}

DirtyMonolithicRegion {
  node_id
  domain: HP | RT
  pixel_roi
  whole_output: true
}

DirtyEdgeMapping {
  from_node_id
  to_node_id
  from_roi
  to_roi
  direction: ForwardAffected | BackwardDemand
}
```

仅用于显示的字符串可以格式化为 `node:12/hp.micro(3,1)` 或 `edge:12->15`，
但字符串路径不应成为主要存储格式。

为了未来 undo/redo 或 replay，snapshot 可能需要 generation metadata 和 origin events，
例如参数变更、用户操作、缓存失效或版本递增。

## 3. ROI 运算、网格与尺度

- 并集/包围盒：多个 ROI 合并时，默认取并集的最小包围盒（可扩展为稀疏 ROI 表示以提升精度）。
- 网格对齐：按当前 domain 的网格对齐，以便切分成离散 Tile 集。
- 尺度映射：当前 RT 代理为原图宽高 1/4（像素约 1/16）。因此
  - RT Micro_16 ↔ HP Micro_64（×4 放大或 ÷4 缩小）；
  - RT Macro_64 ↔ HP Macro_256。

当前默认参数：

| Domain 粒度 | 当前值 | 坐标空间 | 说明 |
| --- | --- | --- | --- |
| RT downscale factor | 4 | N/A | 当前代理尺度，可调优。 |
| RT Micro tile | 16x16 | RT proxy space | 当前交互式更新粒度，可调优。 |
| RT Macro tile | 64x64 | RT proxy space | 当前 RT 吞吐/粗化粒度，可调优。 |
| HP Micro tile | 64x64 | HP full-resolution space | 当前 HP 小块粒度，可调优。 |
| HP Macro tile | 256x256 | HP full-resolution space | 当前 HP 吞吐优先粒度，可调优。 |

RT Macro_64 与 HP Micro_64 当前拥有相同的数值 tile 大小，但它们属于不同的 domain/granularity 组合。数值相等并不表示 RT macro tile 可以与 HP micro tile 互换。

参考实现见《Overview.md》附录。

## 4. Micro→Macro：上采样边界

- HP 域：输入为多个 HP Micro_64；并集→对齐到 HP Macro_256；插入 `ReTileTask` 聚合。
- RT 域：输入为多个 RT Micro_16；并集→对齐到 RT Macro_64；插入 `ReTileTask` 聚合。
- 输出：与输入属于同一 domain 的 Macro Tile 任务集合（RT Macro_64 或 HP Macro_256）。

目的：保证 Macro 算子获取完整、连续的大块数据；避免“离散点”破坏算法特性（如 FFT、卷积域分块）。

## 5. Macro→Micro：下采样边界

- HP 域：输入为一个或多个 HP Macro_256；与 HP Micro_64 网格相交得到所有受影响的 Micro_64。
- RT 域：输入为一个或多个 RT Macro_64；与 RT Micro_16 网格相交得到所有受影响的 Micro_16。
- 输出：与输入属于同一 domain 的 Micro Tile 任务集合（RT Micro_16 或 HP Micro_64）。

目的：将 Macro 级变化“均匀扩散”到下游微块，确保不漏算。

注：在 HP 路径中，Planner 可在 Macro→Micro 映射后对下游 Micro 进行“动态粒度粗化”，将一组对齐同一 Macro_256 的 Micro_64 合并为单一 Macro_256 任务，减少调度碎片与切换；RT 路径默认不进行此粗化（强制 Proxy Micro_16 以满足帧预算）。

## 6. 算子传播策略（示例）

- 点算子（像素级无邻域）：`add_weighted`、`multiply`、`curve_transform`
  - 传播：`propagate_dirty_roi` 恒等返回。

- 邻域算子（卷积/模糊）：`gaussian_blur`、`convolve`
  - 传播：对 `downstream_roi` 向外扩展 kernel 半径（含 padding 策略）。

- 几何算子：`resize`、`warp`
  - 传播：对 `downstream_roi` 做逆变换；结果取包围盒；必要时添加安全边距。

- 通道/拼接：`extract_channel`、`merge`
  - 传播：恒等或按通道/拼接规则映射 ROI 到对应输入。

- 可加载 operation plugin：
  - 传播：每个当前插件都应同时注册 dirty 和 forward ROI propagator。Legacy identity fallback
    只保留为迁移支持，不应作为插件契约完整的证据。
  - 标准插件示例：`image_process:invert` 和 `image_process:threshold` 是逐像素 HP monolithic
    变换，使用显式 pass-through ROI；`io:save` 是带副作用的 HP monolithic sink，它的显式
    pass-through ROI 描述 planning metadata，而执行阶段会重写完整文件；
    `image_generator:perlin_noise_metal` 是 monolithic Metal generator，拥有显式
    generator-local pass-through ROI metadata，不是 tiled Metal 执行路径。

### 6.1. 静态公式 VS 数据依赖 LUT

- **静态公式** 仍然是主战场（`resize`、`crop`、`blur` 等）；这一类算子仅需要依赖参数或缓存后的 `SpatialContext` 信息即可通过 `RoiPropagationService::compute_upstream_roi` 完成逆映射。
- **数据依赖算子**（如 liquid/warp/displacement）无法通过参数静态推导，必须在 `OpRegistry` 中同时注册：
  - 一个 `DependencyLutBuilder`，它负责生成 `SpatialDependencyMap`（网格化的 tile-to-upstream ROI 表）。
  - （可选）`OpMetadata::data_dependent` 标记与调度器能够识别它必须访问 LUT。
- `RoiPropagationService::compute_upstream_roi` 会在静态公式之后尝试使用 LUT：当前 ROI 覆盖的网格 Cell 会被查表，返回的上游 ROI 自动与静态结果合并，供 `ComputeService`/Planner 使用。这样可以在不牺牲性能的前提下，把只涂抹一小块的液化操作定量约束在其真正受影响的输入区域。

LUT 的生命周期附着在 `Node::dependency_lut`，每当参数集 `parameters_version` 发生变化时，builder 会在第一次传播时重新生成。构建过程只遍历有限数量网格点，通常在毫秒级别即可完成，因此可以同步懒加载（ComputeService 或调试命令在传播路径上直接触发），不会引发长时间卡顿。

`ComputeService` 和 `DirtyRegionPlanner` 通过 `RoiPropagationService` 统一调用上述逻辑，因此执行代码不需要关心是静态还是数据依赖算子。该服务消费 `GraphModel` 拓扑邻接，但不拥有拓扑。正式传播边界来自 `GraphExtentResolver`：HP 缓存输出、显式 width/height 参数，或上游 HP 推导 fallback 可以提供范围；RT-only 临时状态不是正式 HP 传播范围。新算子必须显式定义 dirty/forward propagation 语义；当依赖关系是数据依赖时，应在注册阶段提供 `DependencyLutBuilder`，以获得精确的 ROI 传播。

## 7. Monolithic Dirty Escalation

当 tiled dirty region 传播到 monolithic node 时，planner 必须将该 node 的整个输出在该节点上标记为 dirty。
这是 monolithic 边界上的局部升级：如果该实现以单元方式产生输出，就不能安全地只重算输入 tile。

下游传播仍可再次缩小受影响区域。例如后续 crop、resize 或 transform 可能将 monolithic node 的 dirty output
投影到下游节点的更小区域。

## 8. 典型场景：同一 Domain 内的 Micro–Macro–Micro 链

假设一个 RT domain 链：

- A、B、C 都在 RT task pool 中执行。
- 触发：A 的 `(0,0) (1,1) (3,1)` 三个 RT Micro_16 Tile 更新；
- 推导：
  1) A→B：三块 RT Micro_16 聚合到所在的 RT Macro_64，即 `B_rt_macro(0,0)`，需重算整个 64x64 代理块。
  2) B→C：`B_rt_macro(0,0)` 与 RT Micro_16 网格相交，得到 `C_rt_micro:(0,0)–(3,3)` 16 个 Tile。

同样形态也适用于 HP task pool：三块 HP Micro_64 可以聚合到 `B_hp_macro(0,0)`，再扩散到该 HP Macro_256 覆盖的 16 个 HP Micro_64。

结论：C 需要更新 16 个同 domain 的 micro tile，而不是那 3 个离散点。Macro 节点是“信息扩散点”。这个场景不会创建 RT→HP 或 HP→RT 的图边；HP 与 RT 工作是由 compute intent 协调的分离 task-pool sibling。

## 9. RT/HP 双路径中的 ROI 使用

- RT：
  - 当前粒度为 RT Proxy Micro_16，尽量只在 `dirty_roi` 及其传播影响范围内更新；
  - 如缺 `tiled_op_rt`，回退 `tiled_op_hp`（在 RT 代理尺度）。

- HP：
  - 当前采用 Micro_64/Macro_256 混合（优先 Macro_256）推进吞吐；
  - 完成后触发 Downsample 更新 RT，并同步版本。

Planner 可以为了同步或 inspection 使用尺度转换表达相互对应的 HP/RT ROI，但 task dependency 仍留在各自 domain 内：RT Micro_16 <-> RT Macro_64，HP Micro_64 <-> HP Macro_256。

## 10. ROI 边界与裁剪

- 对图像边界处 ROI 进行裁剪，避免越界读写。
- 对大核半径或复杂逆变换导致的“远程影响”，引入面积上限与分批推进（逐步收敛）。

## 11. 取消、合并与去重

- 相同节点上的 ROI 任务可按时间窗合并（包围盒或稀疏集合），防止任务风暴。
- 以版本戳做“软取消”：执行前校验是否过期，过期即弃。

## 12. 校验与可视化

- `InteractionService` 是面向前端的 kernel interaction facade。在 dirty-region 语境中，
  它应暴露图级 snapshot 查询和可视化 hook；它不应被视为 dirty-region generation 或
  propagation 的权威来源。
- CLI/REPL 命令不是 realtime dirty-update 控制面。它不得暴露 RT intent 命令、dirty ROI
  创建命令，也不得暴露 `compute rt`、`--dirty-roi`、`dirty begin`、`dirty update`
  或 `dirty end` 这类 dirty source lifecycle 命令。
- 在构建、测试或前端可视化模式下，提供非 CLI 的 ROI/Tile 覆盖掩码 artifact，便于验证传播正确性。
- 指标：记录 ROI 面积、Tile 数、合并次数、取消次数，辅助调参。
