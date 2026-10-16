# 逻辑区域与 Result 图像样本

## 范围与所有权

`Region` 描述逻辑 descriptor 域内的 offset/extent 区间。`Footprint` 表示精确样本覆盖，包括不连续 box 和空洞。二者都不描述物理字节位置。`Tensor` 拥有不可变 backing 片段；Result 和捕获的 descriptor 限定读 capability 可用的覆盖。

## 数据布局与内存

`ResultTensorSpec` 包含成员 key、dtype 与 cell shape、显式 `batch_axes`、`atomic_trailing_axes` 和可选空间布局。完整样本坐标由 batch 与 cell 轴拼接。空间 H/W 和 channel 索引相对于 cell，轴顺序显式指定。通道数量不隐含颜色或 alpha 解释。

`close_samples` 把样本区域闭包到末尾 atomic cell 轴。Tile 授权是另一项几何操作：空间 tile 完整覆盖所有非 H/W cell 轴，包括 channel，即使没有 atomic 轴也如此。非空间 tile 切分最后两个非 atomic cell 轴；更前面的此类轴和显式 batch 轴形成独立坐标。Atomic 轴保持完整。

所有逻辑 extent 都为正。空 Footprint 不选择样本，也不改变 schema。数据依赖的有效数量是静态容量 schema 内的普通 payload 成员。Padding、空洞和未提交样本不可读。拥有型读窗口保留实际的负、零或正 stride，并在物理和授权边界截断 run。

## 请求、执行与状态

Payload 请求使用成员 slot 和紧凑 tile 区间。算法可以先通过普通 Need 读取控制值，再选择后续 tile Need。内核记录这些 payload 依赖，不使用 data/control 角色标签。Descriptor-only 请求必须显式提出，不授予样本，也不启动 payload producer。空 payload 集合不建立依赖或授权。

要求完整覆盖的输入 validator 仅在算子请求完整授权范围后运行，不扩大 Need。完整输入算法使用 `sdk::to_whole`；全局工作可以在 Run 共享 stage 内执行一次。完整覆盖不构成另一种执行模式。

Dirty 表示可能受到影响，不表示数值必然不同。成员 tile 记录和冻结的共享证据决定失效传播。增加已提交覆盖属于生产进度，不属于内容编辑。精确样本 Footprint 仍用于窗口和 view，依赖与失效粒度为成员 tile。

## Tile 投影运算

正 tile extent `T` 和逻辑 extent `E` 对应 tile 数 `E / T + (E % T != 0)`，避免 `E + T - 1` 溢出。Plan 要求 T 为二次幂，默认 128。乘积、tile 索引和区间端点都检查溢出。完整覆盖保持为紧凑区间，实际任务按有界批次展开。

逻辑网格独立于 backing 几何。同一 backing 可用于不同 T 的 plan，逻辑授权随之改变，不必仅为匹配 tile 边界而重新打包。

## 限制与错误处理

越界索引、未授权读取、不兼容的重叠写入和不完整的普通 tile 发布均失败。普通发布必须完整，恰好覆盖所选成员 tile，其他成员为空。共享 stage 可以逐步发布多个完整 tile。固定 bundle 可见性可延迟公开并主动调度兄弟 tile；见 [Global Results](Global-Results.zh.md)。

取消、资源耗尽和过期 generation 保留各自错误状态。失败事务不安装部分 payload 或不完整依赖证据。此前已提交 tile 保持不可变。显式空输出 demand 不执行 payload 工作；mandatory 副作用遵循 [Global Results](Global-Results.zh.md) 中普通执行与显式 DemandQuery 的区别。
