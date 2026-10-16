# 数据模型

## 1. 范围与所有权

编译器拥有不可变 workflow 结构和已解析的静态 schema。算子通过 Result 端口交换 `ResultRef` owner。一个 Result 包含 1 到 16 个具名 Tensor 成员，其名称、dtype、rank、cell shape、batch 轴、atomic 末尾轴和逻辑网格在准备后的 plan 中固定。结构可以依赖输入 schema 和静态参数，不能依赖运行期样本值。输入结构改变后需要重新准备和编译。

`Tensor` 是唯一数值载体。`TensorSpan` 表示其中一个矩形 backing 映射，不构成第二种数值模型。`CpuStorage`、buffer、native allocation 和资源租约表示物理资源。`ValueRef` 仍是图引用，`Result<T>` 仍是错误返回模板。

Result 和 Tensor 不附着颜色、alpha、transfer 或 LUT 解释。算法需要的解释由显式参数和资源提供。普通图像算子默认 straight alpha；内核不认证此约定，也不根据四通道推断 RGBA。

## 2. 核心结构与内存布局

公共结构位于 [`result.hpp`](../../../include/photospider/data/result.hpp)、[`tensor.hpp`](../../../include/photospider/data/tensor.hpp) 和 [`tensor_span.hpp`](../../../include/photospider/data/tensor_span.hpp)。WorkflowDocument 使用 schema 7。`WorkflowInputDeclaration::result_schema` 必须提供，`ExecutionBinding` 按精确声明名称提供拥有所有权的 Result。

支持 UInt8、Int8、UInt16、Int16、Int64、Float32 和 Float64。完整样本 rank 仍为 1 到 8，extent 必须为正。Scalar 使用 `[1]`。不引入 rank-zero、零 extent、ragged 或运行期 rank。样本 shape 由显式 batch extent 和 cell shape 拼接。`InputAxis` 与 `InputElements` 尺寸表达式在执行前根据静态输入结构解析。

数据依赖的记录数量使用静态容量，例如 `records: Float32[N,R]` 和 `count: Int64[1]`，或同形状数据加 mask。这些名称和数值都是普通 payload。读取 count 或 mask 需要 payload Need 并记录依赖。每个发布 tile 都完整定义，包括按算子契约填充的未使用位置。缺失 tile 不缩短数组。容量不足时失败，除非算子明确承诺最多 K 个结果。

宿主新建的 Result backing 默认使用 tiled planar 存储。逻辑网格独立于物理分配。物理 tile 和目录随实际生产覆盖增长，不要求预留完整逻辑画布或建立稠密目录。小 Tensor 和边缘 tile 按实际覆盖分配。同一 backing 可以服务不同逻辑 tile extent 的 plan。

`Tensor::create` 校验 descriptor、精确授权和不可变片段。兼容重叠去重，覆盖缺失失败。负 stride、zero-stride broadcast、共享 owner 的多个片段和合法外部 backing 保留其映射。结构检查覆盖 dtype、rank、坐标、字节跨度、溢出、所有权和同步。

算子获取拥有型 `ResultTensorReadWindow`，通过受限 `ResultTensorWriteWindow` callback 写入。`row_run()` 与 `rectangle_run()` 返回真实地址和 stride，并在授权、物理 tile 和片段边界截断。保留窗口会保留 backing。借用的 callback 服务在返回时失效，包括 poll 暂停时。

稠密收集是显式操作。`Tensor::collect` 和 SDK collection 检查覆盖与容量、计费复制并观察取消。设备不支持的映射需要显式、计费的 packing 或 transfer。完整逻辑覆盖不意味着连续存储。

## 3. 调度与状态

一个 Run 固定 plan、binding snapshot 和 generation。每个成员在 plan-wide `tile_extent` 下拥有逻辑网格；该值是正二次幂，默认 128。空间成员切分 H/W，非空间成员切分最后两个非 atomic cell 轴。显式 batch 坐标保持独立，atomic 末尾轴保持完整。见 [ADR 0023](../../adr/zh/0023-track-dependencies-between-fixed-tiles.zh.md#31-成员-tile-网格)。

Payload Need 使用成员 slot 和紧凑 tile 区间寻址。Descriptor-only 请求必须显式提出，且不授予样本。空 payload 集合不执行工作。Continuation 可以跨多次 poll 请求输入。普通输出 actor 恰好发布一个完整成员 tile；Run 共享 stage 承担全局计算，可以生产多个 tile。见 [Global Results](Global-Results.zh.md)。

Metadata 闭包包括 schema 和 identity 所需的静态输入。可执行闭包遵循所选输出投影和声明的共享 stage 依赖。仅用于 metadata 的 producer 不执行 payload callback。Binding schema 在 callback 前检查，plan 不保留调用方样本地址。

Workflow 输入 id 和 node id 分属不同命名空间。最多 4096 个输入声明使用唯一非零 id 和唯一的 1 到 128 字节可打印 ASCII 名称。Context 退役后，Result owner 和读窗口仍保留 backing 与 Root 租约。执行保留的外部 backing 计入 Referenced 容量。

## 4. 算法与校验

逻辑 schema 不必能装入一个稠密 allocation。实际分配和寻址字节跨度必须满足表示范围与预算。Tile 数、区间端点和索引运算检查溢出。Tensor 存储保留整数和浮点位模式；数值域约束由算子和显式绑定的 validator 施加。

`SchemaTemplate::id` 与 `version` 标识结构契约并进入编译 identity，不在内核中选择领域语义。LUT bake 等领域报告 schema 属于 ops SDK。完整 validator 要求其范围内所有成员均已获得完整授权，不自动增加输入 Need。

## 5. 限制与非目标

Result 不具有持久序列化或恢复契约。静态 schema 和 tile 几何不意味着稠密驻留。Batch 轴不携带时间含义。内核不提供动态 field 行或独立的整体 Result 执行模式。[ADR 0022](../../adr/zh/0022-freeze-operation-c-abi-v3.zh.md) 仍为 Proposed，其目标 ABI 设计与已实现的 ABI 5 分开描述。
