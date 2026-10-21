# 逻辑区域与 Result 图像样本

## 范围与所有权

`Region` 描述逻辑 descriptor 域内的 offset/extent 区间。`Footprint` 表示精确样本覆盖，包括不连续 box 和空洞。二者都不描述物理字节位置。`Tensor` 拥有不可变 backing 片段；Result 和捕获的 descriptor 限定读 capability 可用的覆盖。

## 数据布局与内存

`ResultTensorSpec` 包含成员 key、dtype 与 cell shape、显式 `batch_axes`、`atomic_trailing_axes` 和可选空间布局。完整样本坐标由 batch 与 cell 轴拼接。空间 H/W 和 channel 索引相对于 cell，轴顺序显式指定。通道数量不隐含颜色或 alpha 解释。

`close_samples` 把样本区域闭包到末尾 atomic cell 轴。Tile 授权是另一项几何操作：空间 tile 完整覆盖所有非 H/W cell 轴，包括 channel，即使没有 atomic 轴也如此。非空间 tile 切分最后两个非 atomic cell 轴；更前面的此类轴和显式 batch 轴形成独立坐标。Atomic 轴保持完整。

所有逻辑 extent 都为正。空 Footprint 不选择样本，也不改变 schema。数据依赖的有效数量是静态容量 schema 内的普通 payload 成员。Padding、空洞和未提交样本不可读。拥有型读窗口保留实际的负、零或正 stride，并在物理和授权边界截断 run。

## 选择与执行

输出选择指定成员 slot 和紧凑 tile 区间。编译时，算子把选中的输出 tile 降低为声明了读取、写入与 scratch 的固定 task 图。完整输入算法声明全部所需输入 tile。只读取元数据时需要输入 schema，无须执行 payload producer。空 payload 选择省略纯输出 task；控制流条件和 mandatory 副作用仍作为根保留。

Plan 在执行前记录物理依赖。运行时控制流选择已编译分支或重复已编译 block。数值回调接收固定 view，不能请求额外的 producer 工作。精确样本 Footprint 描述外部 Result 与读窗口的覆盖；task 调度使用 Plan 的 tile 和依赖表。

## Tile 投影运算

正 tile extent `T` 和逻辑 extent `E` 对应 tile 数 `E / T + (E % T != 0)`，避免 `E + T - 1` 溢出。Plan 要求 T 为二次幂，默认 128。乘积、tile 索引和区间端点都检查溢出。输出选择紧凑保存完整 tile 区间；lowering 在显式编译上限内展开物理 task。

逻辑网格独立于 backing 几何。执行 preflight 要求每个选中的物理 tile 满足其声明布局与分配范围。因此不同 tile 网格可能需要显式 COPY task 来适配已有 backing。

## 限制与错误处理

数据层检查发布区域、重叠写入、仿射地址范围和读窗口授权。编译与 Run preflight 检查声明的 schema、layout 与存储范围。可信数值 kernel 必须遵守全部声明读取，并在成功前初始化全部声明输出样本；执行器不检查其样本访问或数值。Result 所有权与有序 call 导出见 [Global Results](Global-Results.zh.md#执行发布与生命周期)。

失败的 ResultBuilder 事务不增加覆盖，先前发布的区域保持不可变。取消在释放借用存储前排空已接受 task。空输出选择保留实际执行路径中的已编译控制流和 mandatory 副作用，具体契约见[编译器与执行](Compiler-and-Execution.zh.md)。
