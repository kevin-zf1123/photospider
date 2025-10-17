# 内核开发计划与里程碑（详细版）

本计划在 6–8 周内分阶段推进，从基础骨架到实时交互、再到异构优化，包含任务拆解、文件定位、验收标准、测试建议与风险提示。所有任务默认以最小变更、渐进集成、可回退为原则推进。

## 概览

- 阶段一（W1–W3）：内核基础迁移与 Planner/调度骨架
- 阶段二（W4–W5）：实时双管线（RT/HP）与收敛逻辑
- 阶段三（W6–W8）：异构计算与高级调度优化

建议使用 Feature Flag（编译开关或运行时配置）：`PS2_RT_KERNEL`, `PS2_HETERO`, `PS2_TASKGROUP`，逐步放量。

---

## 阶段一：内核基础架构迁移（2–3 周）

目标：完成“骨架”搭建，保持现有全局计算行为不变，API 与数据结构为后续演进打底。

### W1：数据结构与 API 改造

- Task 1.1：引入 ComputeIntent
  - 修改位置：`include/`（公共头）、`src/kernel/`（实现）。
  - 内容：`enum class ComputeIntent { GlobalHighPrecision, RealTimeUpdate };`
  - 验收：编译通过；`NodeGraph::compute` 新增参数但对旧路径保持默认。

- Task 1.2：Node 双缓存与版本
  - 修改位置：`src/kernel/node.*`（或节点状态定义处）。
  - 内容：为每个节点新增 `cached_output_real_time`、`cached_output_high_precision`、`rt_version`、`hp_version`、可选 `rt_roi`/`hp_roi`。
  - 验收：旧计算路径写入 HP；RT 字段空置不影响当前功能。

- Task 1.3：OpRegistry 支持多实现（OpImplementations）
  - 修改位置：`src/ops.cpp` 与相关注册/调用点。
  - 内容：每个 Op 允许注册 `monolithic_op_hp`（可选）、`tiled_op_hp`（必须）、`tiled_op_rt`（可选）。
  - 验收：现有算子至少以 `tiled_op_hp` 注册；原有调用路径不崩溃。

- Task 1.4：Kernel/NodeGraph compute API 调整
  - 修改位置：`src/kernel/node_graph_parallel.cpp`、`src/kernel/graph_runtime.mm` 相关接口。
  - 内容：`compute(intent, dirty_roi_opt)`；intent 影响规划选择，`dirty_roi` 默认空。
  - 验收：`GlobalHighPrecision` 计算路径可用；REPL 不需要改动即可使用。

### W2–W3：调度器基础改造

- Task 2.1：GraphRuntime 双优先级队列
  - 修改位置：`src/kernel/graph_runtime.mm`（或 Runtime 实现）。
  - 内容：`high_priority_q`、`normal_q`，最小可用调度结构。
  - 验收：具备基本入队/出队、统计计数。

- Task 2.2：工作线程 run_loop 抢占
  - 内容：先取高优先级、再取普通队列、最后窃取；空闲休眠。
  - 验收：有可观测指标（日志/计数器）证明优先级生效。

- Task 2.3：`compute_parallel` 建立 switch(intent) 框架
  - 修改位置：`src/kernel/node_module/node_graph_parallel.cpp`。
  - 验收：不同 intent 能走不同分支（占位实现即可）。

- Task 2.4：实现 `GlobalHighPrecision` 分支
  - 内容：优先调用 `monolithic_op_hp`；否则以 Macro Tile（256x256）走 `tiled_op_hp`。
  - 验收：结果与当前版本一致或等价；大图内存超限时能平稳回退分块。

里程碑交付：REPL 仍可全局计算；底层已具备新结构（intent + 双优先级 + 多实现）。

---

## 阶段二：实时更新管线（2 周）

目标：具备核心实时交互能力（RT），并在后台持续推进 HP 与收敛。

### W4：实时（RT）路径规划

- Task 3.1：`RealTimeUpdate` 的 RT 规划
  - 内容：从 `dirty_roi` 出发，正反向传播，激活相交任务；强制 RT Proxy Micro_16（在 1/4 线性尺度的代理图上）。
  - 验收：触发局部更新时，仅 RT Cache 在脏区被更新；无全图扫描。

- Task 3.2：为 1–2 个核心算子提供 `tiled_op_rt`
  - 建议：`add_weighted`, `gaussian_blur`（初期可直接调用 HP 实现）。
  - 验收：算子路径可被识别为 RT 实现或回退 HP 实现。

- Task 3.3：脏区传播（初版）
  - 内容：为所有算子临时提供“恒等传播”；针对模糊/卷积扩展 ROI 半径。
  - 验收：传播函数可被调用，ROI 能正确到达下游。

里程碑交付：可通过命令或 API 发起局部更新；仅脏区的 RT Cache 被修改。

### W5：后台 HP 与收敛

- Task 4.1：HP 管线规划（并行于 RT）
  - 内容：与 RT 同源脏区，后台优先以 Macro_256 推进；必要时使用 Micro_64（micro-macro 混合以降低调度开销）。
  - 验收：后台任务可被正常调度执行，互不阻塞 RT。

- Task 4.2：任务取消机制（最小可行）
  - 内容：基于版本戳的“软取消”，过期任务执行前自检退出；必要时中断排队。
  - 验收：高频交互下，后台浪费计算显著减少。

- Task 4.3：收敛逻辑（DownsampleTask + 版本同步）
  - 内容：HP 完成触发高优先级 Downsample；RT 缓存与版本更新。
  - 验收：可观测到预览从 RT 平滑收敛到 HP。

里程碑交付：实时交互完整；停止操作后可见画质逐步提升直至最终画质。

---

## 阶段三：异构与高级优化（2–3 周）

目标：打通 CPU/GPU 混合执行通路，并优化 Micro→Macro 的协同效率。

### W6：GPU 集成

- Task 5.1：Planner 检查 `device_preference`
  - 验收：规划阶段能区分计算域，并输出跨域边界。

- Task 5.2：`UploadTask`/`DownloadTask`
  - 内容：仅同步 ROI 区域；支持异步批量；错误回退。
  - 验收：跨域时自动插入同步任务，结果正确。

- Task 5.3：双路径中的跨域管理
  - 验收：Global 与 RealTime 两条路径均可自动插入同步任务。

里程碑交付：GPU 与 CPU 可混合，数据传输由 Planner 自动管理。

### W7–W8：高级调度优化

- Task 6.1：TaskGroup 与“任务着色”（Micro→Macro 核心）
  - 内容：将同一 Macro 目标的 Micro 划为一组，指派少量线程集中推进。
  - 验收：缓存命中率上升、切换减少、端到端延迟改善。

- Task 6.2：工作窃取与本地队列增强
  - 内容：支持从邻域窃取普通任务；高优先级尽量保持组内局部性。
  - 验收：压力场景下吞吐与尾延迟更稳健。

- Task 6.3（可选）：完善 `propagate_dirty_roi`
  - 内容：为常见算子提供精确传播（逆变换、半径外扩等）。
  - 验收：局部更新范围更紧凑，计算量进一步降低。

- Task 6.4：动态粒度粗化（Macro 后的 Micro 改写为 Macro）
  - 内容：在 HP 路径启用 `PS2_COARSENING`，检测 `Micro→Macro→Micro` 模式，将下游对齐 Macro_256 的 Micro_64 合并为单一 Macro_256 任务；提供启发式与指标。
  - 验收：在 4K 图上端到端耗时降低，Micro 任务数显著减少；正确性与 ROI 覆盖不变。

- Task 6.5：流水线并行（Pipeline）增强
  - 内容：引入数据流亲和与按流推进策略（A_tile→B_tile→C_tile），在深链场景降低首帧延迟。
  - 验收：深链 benchmark 下首个终端 Tile 的出帧时间缩短，尾延迟稳定。

里程碑交付：Micro→Macro 转换时性能显著提升，交互更顺滑。

---

## 脑图：文件与模块映射（参考）

- 规划与调度
  - `src/kernel/node_module/node_graph_parallel.cpp`：`compute_parallel` 的 intent 分支与任务激活。
  - `src/kernel/graph_runtime.mm`：队列、run_loop、任务取消/窃取；统计指标。

- 算子与注册
  - `src/ops.cpp`：`OpImplementations` 注册表；`monolithic_op_hp` / `tiled_op_hp` / `tiled_op_rt`。

- 状态与缓存
  - `src/kernel/node.*`：节点的 RT/HP 缓存、版本与 ROI 字段。

---

## 测试计划与验收要点

- 单元测试：
  - ROI 映射与 Tile 切分（对齐、相交、并集）；
  - `propagate_dirty_roi` 对常见算子的正确性；
  - 任务取消的版本戳行为（过期即弃）。

- 集成测试：
  - REPL 发起 `GlobalHighPrecision` 与 `RealTimeUpdate`；
  - 交互模拟：连续小步 ROI 改动，观察 RT 延迟与 HP 收敛。

- 基准与指标：
  - RT 帧内延迟 P50/P95；
  - HP 吞吐（tiles/sec）；
  - Downsample 触发与收敛时间；
  - DataSync 传输字节与耗时占比。
  - 粒度粗化收益：粗化触发率、任务数变化、端到端耗时变化（含 4K 长链/短链对比）。

---

## 风险与回退策略

- 实时路径功能不完整：保留旧的全局路径作为硬回退；RT 失败自动切至 HP Micro。
- 大图/大半径导致 ROI 膨胀：限制单帧可处理 ROI 面积，分批推进并逐步收敛。
- GPU 不可用或驱动异常：自动回退 CPU，保留同步 API 兼容层。
- 任务风暴：引入合并/去重（按节点+ROI 合并），并按帧预算限流。

---

## 里程碑交付检查清单（摘要）

- M1（阶段一末）：
  - Intent + 双缓存 + 多实现 + 双队列可用；
  - Global 路径结果与旧版等价；
  - 基本指标可观测。

- M2（阶段二末）：
  - RT 局部更新可见；
  - HP 后台推进 + Downsample 收敛；
  - 高频交互下系统稳定、取消有效。

- M3（阶段三末）：
  - CPU/GPU 混合与 ROI 级同步；
  - TaskGroup 提升 Micro→Macro 性能；
  - 常见算子 ROI 传播完善。
