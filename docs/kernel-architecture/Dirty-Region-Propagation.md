# 脏区传播与 Tile 映射规范

本文规范脏区（Dirty Region, ROI）在节点图中的传播规则、与 Tile 网格的相互映射，以及常见算子的传播策略。实现目标是：充分精确、不漏算、尽量少算，且与 Micro/Macro 粒度边界自然衔接。

本文中的 RT/HP 网格尺寸是当前实现参数，不是永久 ABI。调度器和算子可以依赖本文描述的数据流和安全性要求，但不应把具体 tile 尺寸写死为外部兼容承诺。

## 1. 基本原则

- 传播单位是几何区域 `cv::Rect`（或 ROI 集），而非离散 Tile 索引。
- 在跨节点传播时，使用算子提供的 `propagate_dirty_roi(downstream_roi, node)` 做“反向需求”推导。
- 在粒度边界（Micro↔Macro）处，使用 `ReTileTask` 承接，完成 ROI 与 Tile 网格之间的映射与裁剪。

## 2. ROI 运算、网格与尺度

- 并集/包围盒：多个 ROI 合并时，默认取并集的最小包围盒（可扩展为稀疏 ROI 表示以提升精度）。
- 网格对齐：按当前网格对齐（RT 代理：16；HP：64/256）以便切分成离散 Tile 集。
- 尺度映射：当前 RT 代理为原图宽高 1/4（像素约 1/16）。因此
  - RT 16x16 ↔ HP 64x64（×4 放大或 ÷4 缩小）；
  - HP 256x256 ↔ RT 64x64。

当前默认参数：

| 参数 | 当前值 | 说明 |
| --- | --- | --- |
| RT downscale factor | 4 | 当前代理尺度，可调优。 |
| RT Micro tile | 16x16 | 当前交互式更新粒度，可调优。 |
| HP Micro tile | 64x64 | 当前 HP 小块粒度，可调优。 |
| HP Macro tile | 256x256 | 当前 HP 吞吐优先粒度，可调优。 |

参考实现见《Overview.md》附录。

## 3. Micro→Macro：上采样边界

- HP 域：输入为多个 HP Micro_64；并集→对齐到 HP Macro_256；插入 `ReTileTask` 聚合。
- RT→HP 跨尺度：输入为 RT 16x16；先放大到 HP 64x64，再按上段规则映射到 HP Macro_256。
- 输出：Macro Tile 任务集合（每块 256x256）。

目的：保证 Macro 算子获取完整、连续的大块数据；避免“离散点”破坏算法特性（如 FFT、卷积域分块）。

## 4. Macro→Micro：下采样边界

- HP 域：输入为一个或多个 HP Macro_256；与 HP Micro_64 网格相交得到所有受影响的 Micro_64。
- HP→RT 跨尺度：对 HP Macro_256 先缩小至 RT 64x64，再与 RT 16x16 网格相交得到受影响 RT Tiles。
- 输出：Micro Tile 任务集合（HP: 64；RT: 16）。

目的：将 Macro 级变化“均匀扩散”到下游微块，确保不漏算。

注：在 HP 路径中，Planner 可在 Macro→Micro 映射后对下游 Micro 进行“动态粒度粗化”，将一组对齐同一 Macro_256 的 Micro_64 合并为单一 Macro_256 任务，减少调度碎片与切换；RT 路径默认不进行此粗化（强制 Proxy Micro_16 以满足帧预算）。

## 5. 算子传播策略（示例）

- 点算子（像素级无邻域）：`add_weighted`、`multiply`、`curve_transform`
  - 传播：`propagate_dirty_roi` 恒等返回。

- 邻域算子（卷积/模糊）：`gaussian_blur`、`convolve`
  - 传播：对 `downstream_roi` 向外扩展 kernel 半径（含 padding 策略）。

- 几何算子：`resize`、`warp`
  - 传播：对 `downstream_roi` 做逆变换；结果取包围盒；必要时添加安全边距。

- 通道/拼接：`extract_channel`、`merge`
  - 传播：恒等或按通道/拼接规则映射 ROI 到对应输入。

### 5.1. 静态公式 VS 数据依赖 LUT

- **静态公式** 仍然是主战场（`resize`、`crop`、`blur` 等）；这一类算子仅需要依赖参数或缓存后的 `SpatialContext` 信息即可完成 `compute_upstream_roi` 的逆映射。
- **数据依赖算子**（如 liquid/warp/displacement）无法通过参数静态推导，必须在 `OpRegistry` 中同时注册：
  - 一个 `DependencyLutBuilder`，它负责生成 `SpatialDependencyMap`（网格化的 tile-to-upstream ROI 表）。
  - （可选）`OpMetadata::data_dependent` 标记与调度器能够识别它必须访问 LUT。
- `GraphTraversalService::compute_upstream_roi` 会在静态公式之后尝试使用 LUT：当前 ROI 覆盖的网格 Cell 会被查表，返回的上游 ROI 自动与静态结果合并，供 `ComputeService`/Planner 使用。这样可以在不牺牲性能的前提下，把只涂抹一小块的液化操作定量约束在其真正受影响的输入区域。

LUT 的生命周期附着在 `Node::dependency_lut`，每当参数集 `parameters_version` 发生变化时，builder 会在第一次传播时重新生成。构建过程只遍历有限数量网格点，通常在毫秒级别即可完成，因此可以同步懒加载（ComputeService 或调试命令在传播路径上直接触发），不会引发长时间卡顿。

调度器（ComputeService）已经通过 `GraphTraversalService::compute_upstream_roi` 统一调用上述逻辑，因此 Planner/执行中不再需要关心是静态还是数据依赖算子。只需确保新算子在注册阶段提供 `DependencyLutBuilder`，即可享受精确的 ROI 传播。

## 6. 典型场景：Micro–Macro–Micro 链（含尺度）

假设：

- A：RT Proxy Micro_16（16x16），B：HP Macro_256（256x256），C：RT Proxy Micro_16（16x16）。
- 触发：A 的 `(0,0) (1,1) (3,1)` 三个 16x16 Tile 更新；
- 推导：
  1) A→B：将三块 RT 16x16 放大 4× 到 HP 64x64，三者并集落入同一 `B_macro(0,0)`（256x256），需重算整块。
  2) B→C：将 `B_macro(0,0)`（256x256）缩小至 RT 64x64，与 16x16 网格相交，得到 `C:(0,0)–(3,3)` 16 个 Tile。

结论：C 需要更新 16 个 Tile，而不是那 3 个离散点。Macro 节点是“信息扩散点”。

## 7. RT/HP 双路径中的 ROI 使用

- RT：
  - 当前粒度为 RT Proxy Micro_16，尽量只在 `dirty_roi` 及其传播影响范围内更新；
  - 如缺 `tiled_op_rt`，回退 `tiled_op_hp`（在 RT 代理尺度）。

- HP：
  - 当前采用 Micro_64/Macro_256 混合（优先 Macro_256）推进吞吐；
  - 完成后触发 Downsample 更新 RT，并同步版本。

## 8. ROI 边界与裁剪

- 对图像边界处 ROI 进行裁剪，避免越界读写。
- 对大核半径或复杂逆变换导致的“远程影响”，引入面积上限与分批推进（逐步收敛）。

## 9. 取消、合并与去重

- 相同节点上的 ROI 任务可按时间窗合并（包围盒或稀疏集合），防止任务风暴。
- 以版本戳做“软取消”：执行前校验是否过期，过期即弃。

## 10. 校验与可视化

- 在构建或测试模式下，提供 `debug roi`：将 ROI/Tile 覆盖绘制为掩码输出，便于验证传播正确性。
- 指标：记录 ROI 面积、Tile 数、合并次数、取消次数，辅助调参。
