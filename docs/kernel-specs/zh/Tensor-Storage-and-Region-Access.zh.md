---
spec_schema_version: 1
id: KERNEL-tensor-storage-zh
kind: shared_kernel_contract
status: Accepted
implementation_status: implemented_cpu
---

# Tensor 存储与区域访问

本文定义静态 Result Tensor 成员已实现的存储契约。见[英文权威文档](../Tensor-Storage-and-Region-Access.md)、[数据模型](../../kernel-architecture/zh/Data-Model.zh.md)和 [Global Results](../../kernel-architecture/zh/Global-Results.zh.md)。

## 范围与所有权

`SchemaTemplate::tensors` 包含 1 到 16 个固定具名成员。`TensorDescriptor` 提供 dtype 和正 cell extent；`batch_axes` 在其前面添加显式 batch extent。完整样本 rank 为 1 到 8。`atomic_trailing_axes` 把末尾 cell 轴组合成 observation。可选空间拓扑使用相对于 cell 的 H/W 和 channel 轴，H/W 不得与 atomic 轴重叠。

唯一数值表示是由拥有型矩形 `TensorSpan` 映射组成的 `Tensor`。Buffer、`CpuStorage`、native storage 和资源租约提供物理 backing。Result 和拥有型窗口保留所需 backing。Shape 和 schema id 都不认证颜色或 alpha 语义。

### C++ 注册中的元数据推导输出

Preparation 根据静态输入 schema 和参数解析输出 schema。Extent 可以固定，或由输入轴、元素数量推导。运行期 payload 不改变结构。C ABI 5 metadata resolution 可以提供输出 schema 和静态共享 stage 声明，但不保留由算子持有的任意 prepared-state 指针。见 [Plugin ABI](../../kernel-architecture/zh/Plugin-ABI.zh.md)。

## 核心数据模型与内存

逻辑 shape、逻辑 tile 网格和物理分配相互独立。Schema 不必具有能装入一次 allocation 的稠密元素乘积。实际寻址跨度、tile 数、区间端点和分配大小使用受检运算。不提供 field 存储或运行期 extent 协议。有效数量可变时使用固定容量 Tensor 和普通 payload count/mask 成员。

宿主新建 backing 默认 tiled planar。Backing 目录只保存已生产片段，不为完整逻辑域建立条目。不要求预留完整画布虚拟地址。`ResultBuilder::start()` 不分配全部逻辑样本。

### 仿射 backing

`StridedLayout` 包含有符号字节 stride 和 origin。只要每个元素都位于保留存储内，正、负、零 stride 均合法。Singleton 轴不增加跨度。Zero stride 可以让很大的逻辑 broadcast 域共享很小的 backing。Padding 字节不属于逻辑样本。

Row run 的 `data` 指向第一个逻辑样本。`sample_stride_bytes` 可为负或零；`bytes` 描述受检的寻址跨度，不表示可向前连续复制的范围。Rectangle run 额外提供有符号 row stride。访问必须使用实际 run 元数据。

### 空间 backing

空间拓扑独立于字节顺序控制逻辑 H/W run 轴。默认写入在 batch 和物理 tile 边界切分请求 Footprint，提供受限的 planar 写窗口。非空间向量、矩阵和高维 Tensor 使用同一 backing 与窗口机制。显式 batch 坐标和 atomic 末尾轴保持结构含义。

一次发布可以分配由多个 chunk span 共享的 buffer，tile 不必独占 allocation。兼容的外部仿射或分片 backing 仍合法。Plan 的逻辑 tile extent 可以与输入 backing 不同。

## Tile 布局与受检运算

Plan-wide `tile_extent` 是正二次幂，默认 128。空间网格切分 H/W；非空间网格切分最后两个非 atomic cell 轴，其余非 atomic cell 轴和显式 batch 轴形成 batch 坐标。只有一个可分割轴时，每块最多 T 个 observation；没有可分割轴时，每个 batch 坐标形成一个 tile。

默认物化按实际 chunk 宽高生成 stride 和 payload 大小，不把所有边缘行补到 T，也不让每个小 Tensor 对齐 OS 页。实际分配容量与逻辑覆盖分别准入。所有大小乘法和寻址运算都检查溢出。

### 边缘行分配示例

Float32 空间边缘 chunk 的高为 2、宽为 72、planar 通道数为 4 时，样本 payload 为 `2 * 72 * 4 * 4 = 2304` 字节。紧凑 planar row stride 为 `72 * 4 = 288` 字节。不要求 128 宽的补齐行或完整 128×128 tile。外部 backing 可以带 padding；读窗口暴露真实 stride，不把 padding 授权为样本。

## 读窗口与事务写入

`ResultRef::acquire_tensor` 返回拥有型、仅可移动的读窗口，受捕获 descriptor 和请求矩形限制。后续生产不扩大该授权。`row_run()` 与 `rectangle_run()` 在授权、物理 tile 和片段边界截断。空间 run 遵循 width/height，非空间 run 使用最后一个/倒数第二个轴。默认物化 chunk 在 batch 边界截断。普通外部 span 可以覆盖多个 batch；调用方遵循 sample_axis()、row_axis() 和实际返回范围，非空间 rectangle 可以沿 batch 轴跨越坐标。

窗口保留来源 owner 和取消状态。Token 取消后 run 查找可返回 Cancelled；此前返回的指针在窗口销毁前仍有 owner。获取窗口不执行上游 producer，也不填充空洞。获取和访问工作按相应 Root 与限制计费。

`publish_tensor_kernel()` 在 callback 前准入容量并准备私有写入。Region 和 Footprint 重载使用同一 Tensor 机制。Callback 只获得请求且尚未发布的覆盖；并行写入必须不相交并在返回前结束。借用写窗口在 callback 返回时失效。成功校验和取消检查后提交 backing，失败不暴露新覆盖。

Writer 活动期间拒绝同 producer 的修改或 seal；移动或替换其 builder 会使事务失败。此前已提交样本保持不可变。空覆盖不调用 writer、不分配样本 payload。普通执行器额外要求每次发布恰好覆盖一个完整所选成员 tile；共享 stage 提交可以包含多个完整 tile。

## 映射 view

映射可表示时，`publish_tensor_view()` 保留已授权来源窗口及其 owner，无需复制样本。轴变换和 reshape 使用显式 `ResultTensorViewTransform` 规则。非 reshape 变换独立映射每个来源片段，允许不同 owner 和 stride。Reshape 要求片段能合并成一个合法仿射映射。Singleton、负和 broadcast stride 仍接受校验。非法映射、空洞、溢出和来源到目标的所有权环均失败。

View 不创建输入授权。协调器没有替算子自动获取完整输入或静默复制的策略。SDK `to_whole` 发出普通 tile Need 并返回拥有型 capability；`to_tile` 发布兼容的保留 view。映射需要复制时，算子显式选择 collection 或 packing 并提供预算。

## 限制与错误处理

稠密 `Tensor::collect` 在分配前检查完整授权覆盖。Packed copy 和普通 stride copy 都遵循 work 限制与取消。Native packing/upload 和 download 有显式容量、work、owner 与同步检查。零 dispatch 的 native alias 仍需来源写入完成且数据可见。

Kernel callback guard 保存和恢复浮点状态，不强制数值模式。Ops SDK guard 在实际执行线程上建立算法所需模式，覆盖 worker、preparation、validator 和 poll，并在 callback 退出或暂停时恢复。Guard 不跨 poll 保留。

## 非目标

本契约不增加 rank-zero、零 extent、ragged 成员、动态 rank、隐藏的全量复制、语义布局认证、自动 alpha association、逐出后的 producer 重放或持久 backing。GPU 执行证据仍限定到实际后端与设备。
