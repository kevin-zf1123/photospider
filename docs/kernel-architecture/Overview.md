# 实时异构内核 - 架构总览与行为预期

本文档定义本内核的总体架构目标、核心概念、关键数据结构、调度与规划策略、异构设备支持、收敛机制、以及系统级行为预期。该设计以“意图驱动、状态分离、分层调度”为核心，既保证实时交互的低延迟，又最大化后台吞吐。

## 1. 设计哲学（Intent-Driven · State-Separated · Layered Scheduling）

- 意图驱动：一切计算先归因于 ComputeIntent（全局高精度 vs 实时交互），由 Planner 选择路径与粒度。
- 状态分离：每个节点维护 RT 与 HP 双缓存、版本号与 ROI，互不污染但可收敛同步。
- 分层调度：静态图分析（加载时）+ 动态规划（每次 compute），并结合双优先级队列与任务组推进。

## 2. 术语与固定粒度

- HP Micro_64：64x64，后台高精度路径的基础微粒度（固定）。
- HP Macro_256：256x256，后台高精度路径的宏粒度（固定）。
- RT Proxy Micro_16：16x16（在一张 1/16 像素数量的实时代理图上，等价于原图 1/4 线性尺度）。实时路径强制使用该粒度。
- Monolithic：整图/整层优化实现，优先用于离线 HP。
- RT Cache/HP Cache：节点输出的双缓存与版本域，分别服务于实时预览与最终画质。
- ComputeIntent：`GlobalHighPrecision`（离线/全图）与 `RealTimeUpdate`（实时/增量）。
- TaskGroup：面向 Micro→Macro 场景的协同批处理单元，用于“波次调度”。

坐标/尺度约定：若 RT 代理图是原图在宽高各 1/4（像素数量约 1/16），则
- RT 16x16 ↔ HP 64x64（放大 4 倍映射）；
- HP 256x256 ↔ RT 64x64（缩小 4 倍映射）。

## 3. 数据与状态模型

每个 Node 维护如下核心状态：

- `cached_output_real_time`（RT）：用于实时预览；允许短期低精度与近似。
- `cached_output_high_precision`（HP）：用于导出/全局画质；最终一致性来源。
- 版本域：`rt_version`、`hp_version`，以及可选的 `rt_roi`、`hp_roi`（最近一次变更的影响范围）。
- 一致性规则：
  - RT 与 HP 可不一致；当 HP 完成对应区域计算时，通过 DownsampleTask 更新 RT 并同步版本。
  - 任意时刻，HP 始终代表“最终画质”的单一真相；RT 是收敛到 HP 的暂态近似。
  - 图像节点：默认 RT 缓存采用代理分辨率（宽高各 1/4）；数据/标量节点：RT/HP 一致。

## 4. Planner：静态 + 动态规划

### 4.1 静态图分析（加载时一次性）

- 依赖图构建：分别在 HP（以 Micro_64 为单位）与 RT（以 Proxy Micro_16 为单位）建立任务级依赖信息；Planner 维护两者之间的尺度映射关系。
- 异构分区：根据 `device_preference` 标注 CPU/GPU 计算域。
- 数据同步点：在跨域边界预插 `DataSyncTask`（`UploadTask`/`DownloadTask`）。
- 粒度边界：标注 `ReTileTask` 的位置（Micro→Macro、Macro→Micro）。

### 4.2 动态规划（每次 compute）

1) 意图解析：读取 `ComputeIntent` 与上下文（`dirty_roi`、优先级等）。

2) 路径选择：
- GlobalHighPrecision：激活全图路径；优先 Monolithic；否则 Macro Tile 化。
- RealTimeUpdate：双路径规划（RT 高优先级 + HP 后台）。

3) 任务激活与剪枝：
- 离线：激活所有任务，普通优先级，Monolithic 或 Macro Tile。
- 实时：
  - 脏区传播（正反向）：从 `dirty_roi` 推导受影响节点与 ROI。
  - 剪枝：仅激活与脏区相交的任务，其他休眠。

4) 粒度与优先级：
- RT 管线：高优先级；强制使用 Proxy Micro_16；可用 `tiled_op_rt`，无则回退 `tiled_op_hp`（在 RT 代理尺度上）。
- HP 管线：普通优先级；优先 Macro_256，必要时 Micro_64（micro-macro 混合以降低调度开销）。

## 5. 调度与执行模型

### 5.1 队列与抢占

- 双优先级队列：`high_priority_q`（RT）优先消费；`normal_q`（HP）被抢占。
- Worker `run_loop`：始终先尝试从高优先级队列取任务；空则取普通队列；支持任务取消。

伪代码：

```
while (running) {
  if (task = high_q.try_pop()) { execute(task); continue; }
  if (task = normal_q.try_pop()) { execute(task); continue; }
  task = steal(); if (task) { execute(task); continue; }
  wait();
}
```

### 5.2 波次调度（Micro→Macro）

- 对同一 Macro Tile 的大量上游 Micro 任务，Planner 组装为 TaskGroup，暗示“同色”处理。
- 分配少量线程“专注”该组，减少缓存抖动、提升共享率；其余线程继续背景工作或拉取其他任务。

### 5.3 工作窃取与局部性

- 每线程本地队列 + 全局队列混合；优先从本地拉取相邻“同色”任务。
- 允许从相邻线程窃取普通优先级任务；高优先级任务避免被四处分裂，优先保持组内局部性。

### 5.4 动态粒度粗化（Dynamic Granularity Coarsening）

- 目标：检测 `Micro → Macro → Micro` 等模式时，避免在 Macro 之后对下游 Micro 进行“粉碎式”调度，改为在本次计算中将该段下游 Micro 重写为 Macro 任务，提高吞吐与局部性。
- 场景：B 为 HP Macro_256，C 默认为 HP Micro_64；若 B 的输出覆盖某个 Macro_256，Planner 可将 C 的该区域改写为单一 Macro_256 任务（或少量较大的块）。
- 约束：
  - HP 路径默认启用；RT 路径默认禁用（因 RT 强制 Proxy Micro_16 帧预算优先）。可通过策略在 RT 中有限启用（当脏区恰好对齐 Macro 且预算允许）。
  - 仅当 C 的 `tiled_op_hp` 对 Macro_256 支持良好时启用；否则退化为 Micro_64。
- 依赖重写：C 的 Macro 任务直接依赖 B 的对应 Macro 输出；后续下游对该区域的依赖从原 16×Micro 变为 1×Macro。
- 代价模型（启发式）：
  - 若“预计 Micro 任务数 × 平均调度成本” > “1×Macro 计算开销 × 阈值”，则执行粗化；阈值建议 0.5–0.8 以偏向粗化。
  - 对齐与合并：优先对齐 Macro_256；不足整块的边缘区域可按 2×Micro_64 合并处理。
- 可配置：`PS2_COARSENING=on|off|rt_safe`；指标：粗化触发次数、节省的任务数、端到端耗时对比。

### 5.5 并行模式（混合自适应）

- 空间并行（Spatial）：同一节点的多 Tile 并行；适合宽而浅的图，优先用于入口阶段。
- 任务并行（Task）：多节点并行（分支并发）；适合含分支的图，开销低、调度简单。
- 流水线并行（Pipeline）：同一数据流沿路径逐层推进（A_tile0→B_tile0→C_tile0…）；适合深而窄、低延迟场景。
- 策略：
  - 入口大量可并行 Tile → 空间并行；
  - 出现分支 → 任务并行；
  - 深链路局部 → 倾向流水线（结合任务亲和与本地队列）；
  - Micro→Macro 的集中区域 → 结合 TaskGroup 与粗化，减少碎片化与切换。

## 6. 收敛与一致性（Convergence Scheduling）

- HP 任务完成后，对应 ROI 与 RT 版本对比：若 RT 过时，立即生成高优先级 `DownsampleTask`。
- `DownsampleTask` 更新 RT Cache 并同步版本号，确保视觉从 RT 平滑收敛到 HP。
- 行为预期：
  - 交互结束后，若有后台资源，画质在数百毫秒–数秒内逐步“变清晰”。
  - 如被新交互打断，未完成的 HP 工作可取消或继续小步推进（实现可配）。

## 7. 异构设备支持（CPU/GPU）

- `device_preference`：按节点/算子声明优先计算域。
- `UploadTask`/`DownloadTask`：在依赖边界自动插入；ROI 有界同步，避免全量传输。
- 跨域 ROI：Planner 在图上“携带”空间坐标与尺度信息，确保下游设备获取精确输入范围。

## 8. 脏区传播（Dirty Region Propagation）

### 8.1 传播单位与基本规则

- 传播单位为几何区域（`cv::Rect`），而非离散 Tile 索引列表。
- ROI 运算：合并用并集+包围盒（按需求可细化为稀疏 ROI 集），跨节点用算子自定义的 `propagate_dirty_roi` 转换。

### 8.2 Micro↔Macro 边界

- HP Micro_64 → HP Macro_256：将上游多个 64x64 ROI 的并集映射到最小覆盖的 256x256 Tile 集（插 `ReTileTask`）。
- HP Macro_256 → HP Micro_64：对脏 Macro 矩形与 64x64 网格相交，得到所有受影响的 Micro_64。
- RT/HP 跨尺度：通过尺度变换在 RT 16 与 HP 64/256 间进行 ROI 映射（上采样/下采样 + 对齐裁剪）。

### 8.3 算子特性

- `propagate_dirty_roi(downstream_roi, node)`：返回上游所需的输入 ROI。
  - 点算子（如 add_weighted）：返回原 ROI。
  - 卷积/模糊：返回 ROI 外扩 kernel 半径。
  - 几何变换（resize/warp）：对 ROI 做逆变换并取包围盒。

### 8.4 例：Micro–Macro–Micro（三节点链，含尺度映射）

设定：RT 代理尺度为原图宽高 1/4（像素 1/16）。

- A：RT Proxy Micro_16（16x16，RT 代理网格）。
- B：HP Macro_256（256x256，原图网格）。
- C：RT Proxy Micro_16（16x16，RT 代理网格）。

过程：
- 初始脏区（A）：`A:(0,0) (1,1) (3,1)` 三个 16x16 RT Tile。
- A→B（跨尺度 + Micro→Macro）：将三块 RT 16x16 放大 4× 映射到 HP 64x64，再求并集并上取整到 Macro_256，落在同一 `B_macro(0,0)`（256x256），需重算整块。（通过 ReTile 聚合）
- B→C（Macro→Micro + 跨尺度）：将 `B_macro(0,0)`（256x256）缩小 4× 到 RT 尺度，对齐到 16x16 网格，相交得到 `C:(0,0)–(3,3)` 共 16 个 RT 16x16 Tile。

结论：C 需要更新 16 个 RT 16x16 Tile，而不是那 3 个离散点。Macro 节点在 HP 域内形成“信息扩散块”，经尺度映射后覆盖到 RT 代理网格。

## 9. 算子接口与注册（OpImplementations）

- `monolithic_op_hp`（可选）：整图高效实现；用于离线 HP 优先。
- `tiled_op_hp`（必须）：高精度分块版本；供实时回退与后台计算。
- `tiled_op_rt`（可选）：低精度快速版本；未提供时 RT 路径回退 `tiled_op_hp`（在 RT 代理尺度上，且强制 Proxy Micro_16）。
- `propagate_dirty_roi(const cv::Rect&, const Node&)`：脏区反向映射函数。

注册建议：默认为所有现有算子注册 `tiled_op_hp`，逐步补充 `tiled_op_rt` 与 `monolithic_op_hp`。

## 10. API 外形与约定

- `enum class ComputeIntent { GlobalHighPrecision, RealTimeUpdate };`
- `NodeGraph::compute(intent, dirty_roi_opt)`：根据意图规划并调度。
- `GraphRuntime`：
  - `enqueue(Task, priority)`；
  - `run_loop()`：抢占式消费；
  - 取消/窃取/任务组支持。

## 11. 系统级行为预期与不变量

- 正确性：任何被 ROI 影响到的像素在最终 HP 结果中必被重算；RT 结果随 Downsample 收敛。
- 延迟目标：
  - RT 帧内响应：< 16–33ms（高优先级、RT Proxy Micro_16）。
  - 背景收敛：数百毫秒–数秒（图像大小与算子相关，HP Micro_64/Macro_256 混合推进）。
- 内存：
  - 双缓存导致显存/内存加倍开销，优先 RT 限制在 Micro ROI；HP 可分块迭代。
- 回退策略：
  - 如果 `tiled_op_rt` 缺失，RT 回退 `tiled_op_hp` + Micro 粒度。
  - 如果 Monolithic 内存超限，回退 Macro Tile。

## 12. 度量与可观测性

- 关键指标：
  - RT/HP 队列深度、等待/执行时长、抢占次数；
  - ROI 面积与 Tile 数；
  - Downsample 触发/合并数、收敛时间；
  - DataSync 传输字节与耗时。
- 工具：
  - REPL 指令扩展：`metrics`, `trace on/off`, `debug roi`；
  - 事件日志：任务生命周期、取消、窃取与组调度。

## 13. 风险与边界情形

- 大核半径卷积的 ROI 外扩可能引发“远程影响”，Planner 需上限裁剪并以渐进收敛替代一次性覆盖。
- 复杂几何变换下的 ROI 逆映射不规则，可退化为包围盒 + 额外 padding。
- 高频交互导致后台任务频繁取消：采用“软取消”与“短阶段提交”策略维持吞吐。
- 异构设备带宽成为瓶颈：强制 ROI 限制 + 异步批传输（包内合并）。

## 14. 附：ROI→Tile 映射参考实现（伪代码）

```
Rect align_to_grid(Rect r, int tile) {
  int x0 = floor(r.x / tile) * tile;
  int y0 = floor(r.y / tile) * tile;
  int x1 = ceil((r.x + r.w) / tile) * tile;
  int y1 = ceil((r.y + r.h) / tile) * tile;
  return Rect{x0, y0, x1 - x0, y1 - y0};
}

vector<Tile> rect_to_tiles(Rect r, int tile) {
  Rect a = align_to_grid(r, tile);
  vector<Tile> out;
  for (int y = a.y; y < a.y + a.h; y += tile)
    for (int x = a.x; x < a.x + a.w; x += tile)
      out.push_back({x / tile, y / tile});
  return out;
}
```

## 15. 基准观察与策略指导（基于 4K 高斯模糊）

- 观测摘要：
  - Monolithic 明显快于 Macro_256；在 4096×4096、短链/长链下可达 3–5× 优势；Micro_16 极慢（调度开销为主）。
  - 长链下 Monolithic 仍线性扩展，未被缓存拖累；Tiled 的“缓存本地性”优势不足以弥补调度开销。
- 原因分析：
  - 现代 CPU L3 缓存与内存带宽足以高效处理 64MB 量级整块数据；OpenCV 内部实现已良好利用向量化与缓存层次，整块处理代价小。
  - Tiled 方案在任务数暴增时调度/同步/函数调用开销成为主因，尤其在 Micro_16 级别。
- 策略落地：
  - Global/离线：优先 Monolithic；内存/实现受限时退化 Macro_256；尽量避免 Micro。
  - RT：仅在代理图上使用 Proxy Micro_16；严控 ROI 与帧预算；必要时回退 hp 实现但仍保持 16 粒度。
  - HP 后台：采用 Micro_64/Macro_256 混合，并启用“动态粒度粗化”减少 Macro 后的 Micro 碎片化。
  - 调参建议：在 4K 及以上分辨率优先打开 `PS2_COARSENING`；对短小 ROI 或非对齐区域维持默认策略。
