# 通道与颜色算子

包 0.20.0 移除旧通道、alpha、颜色算子，以及 `numeric.cast`、
`numeric.encode_range` 的源码、默认注册和专属旧测试。13 个完整 key 与保留范围见
[退休记录](../../built-in_ops/02-format-color/op_specs/FMT_legacy_retirement.md)。
查询、直接调用与编译这些 key 均返回 `NotFound`，无别名或兼容占位注册。

[目录](../../built-in_ops/02-format-color/representation.md)及
[FMT 公共规格](../../built-in_ops/02-format-color/op_specs/FMT_common_contract.md)
定义后续方向。FMT-01A/B 已有 CPU 实现，FMT-01C 提供公开 authoring helper；
规格决策状态仍为 Proposed。FMT-02A/B/C 及 FMT-03A/B 组合接口已实现，FMT-06、FMT-08 也已实现，详见下文；FMT-04/05 尚未实现，FMT-07 已退休。完整图像使用
planar 存储、straight 颜色和同张量内的 alpha。新算子须实现各自的精确请求、
metadata、数值和布局契约；旧 typed HWC 行为不构成新规格的子集实现。

NUM/CRV 使用的共享 ColorArray 描述、profile 所有权和数学基础设施保留。
这些能力不代表已注册格式转换，也不代表实现了新 FMT 算子族。

当前正确性测试覆盖提取、组装、编辑、元数据及严格数值转换。`test_compiler`
使用任意未知算子名验证查询、调用和编译均拒绝。历史删除名称清单不再作为
CTest 或安装消费门禁。

```sh
cmake --build build --target test_compiler test_channel_extraction test_channel_assembly test_channel_editing test_numeric_conversion test_metadata_assignment -j 8
ctest --test-dir build -R '^test_(compiler|channel_extraction|channel_assembly|channel_editing|numeric_conversion|metadata_assignment|installed_consumer)$' --output-on-failure
```

[英文说明](../Channel-and-Color-Operations.md)为权威来源。

## FMT-01 通道提取

`channel.extract_index_{strict,accelerated_apple_silicon,accelerated_x86_64}`
按静态 `index` 提取；`channel.extract_named_` 使用相同 profile 后缀，按
`match=name|role` 命名空间中的唯一精确 `selector` 提取。命名 CPU profile
必须符合宿主能力。所有 profile 对 UInt8、UInt16、Int8、Int16、Int64、Float32、
Float64 执行逐位复制，包括 NaN payload。

输入为 `input`，输出为 `values`。直接节点须提供 `metadata_mode=respect|raw|override`、
`keepdims` 和 `layout=auto|view|materialize`。`axis` 是小于 rank 的非负 Int64；
raw 或无描述输入必须显式提供。respect 检查显式轴与 metadata 一致；override
必须提供 `tensor_description_parameter` 编码的 `metadata_override`。rank 为 1..8，
移除轴要求 rank >= 2。提取不执行颜色转换、alpha 归一化或浮点运算。

公开 `TensorDescription` 编解码器使用 `photospider.tensor-description` v2，记录
轴、有序名称/角色/单位、分量解释与可选 ICC identity。投影保留适用解释和资源，
单个分量不获得完整颜色保证。override 仅影响本次调用。`format::split_channels`
展开 A 节点并返回 c0、c1 等句柄，编译时核验所提供的 producer metadata。

普通 tensor 通过 Dependency fragments 精确请求，支持负 stride 和零 stride。
planar 通道 view 保留源虚拟 backing，仅开放请求 ROI，最后一个 alias 释放后才
归还执行准入。物化只提供请求的输出页。raw 空间轴选择执行物化：keepdims 保留
planar 存储，删除空间轴后通过 `ExecutionResult::values` 返回普通 tensor。
不可用的强制 view 在执行时返回 `InvalidArgument/InvalidDomain` 和
`ViewUnavailable`。后续普通 tensor 节点通过 regional source 执行。
多个具名 planar 根分别执行各自请求的 Region。

可运行的公开 workflow 位于
[`test_channel_extraction.cpp`](../../../tests/integration/test_channel_extraction.cpp)。
首个 fixture 使用 [2,2,4] 的 B/A/R/G 数据并请求第二行，独立检查 alpha [75,99]、
named red [12,13] 和 split c2 [12,13]。其他 oracle 覆盖七种 dtype、rank eight、
CHW/HWC、跨 tile ROI、raw 空间轴结果 [105,106,109,110]、所有权、profile override、
取消和不连续根请求。

```sh
cmake --build build --target test_channel_extraction -j 8
ctest --test-dir build -R '^test_channel_extraction$' --output-on-failure
```

成功表示 exit code 0 且所有 oracle 断言通过。安装消费目标
`photospider_channel_extraction_consumer` 编译运行同一源码。原生宿主 CPU 验证
不代表其他 ISA 或 GPU 已验证。原生索引提取的实测结果与 Instruments 热点见
[性能 workflow](../../../examples/channel_extraction_performance/README.md)。混合执行目前逐个重新编译
普通 tensor 节点；诊断包含节点时间，但未统一汇总子执行的整次内存峰值，也不代表 RSS。


## FMT-02 通道组装

0.21.0 实现 `channel.assemble`、`channel.concatenate`、
`channel.assemble_mapped` 的 strict、accelerated_apple_silicon 和
accelerated_x86_64 入口；决策状态保持 Proposed。A 插入新通道轴，B 按输入顺序
拼接已有通道轴，C 按完整目标槽位映射复制来源分量。支持 1..1024 个重复输入，
UInt8/UInt16/Int8/Int16/Int64/Float32/Float64 同 dtype 逐位复制，无隐式广播或转换。

公开 `format::assemble_channels`、`concatenate_channels`、
`assemble_mapped_channels` 位于 `photospider/format/channel_assembly.hpp`，
返回可连接的 `WorkflowNodeOutput`。默认 respect/auto；直接节点显式填写模式、
layout 和输出轴。B 的 input_axes 使用 `v1;axis;_;axis`；C 的 input_structure
使用 `v1;c;h2;h_`；input_overrides 使用升序唯一序号的
`v1;ordinal:description_hex;...`；mapping 使用
`v1;input,match,selector_hex,destination,component_hex;...`。整数为无前导零的
非负十进制，selector 为严格 UTF-8 的小写十六进制，缺失 component 写 `_`。
output_description 与 component 描述由公开 codec 编码。所有 String 仍受 8192
字节参数上限约束。来源选择独立于目标语义赋值。

当前 0.23 元数据为 tensor-description v4/TDM4；v1-v3 明确拒绝，调用者重新编码后消费。
新增逐分量 TensorInterpretation 和显式 TensorColorGroup，包含通道索引、对应
分量、解释及内部 alpha 索引。完整颜色组校验模型分量与所需结构字段，组与通道
同字段冲突失败。输出目标字段允许局部重解释；未重定义的适用字段继续按 respect
校验。更换模型删除不再适用的旧模型字段，不进行像素转换或生成有效性证明。
ICC 引用必须有真实资源 owner，并由结果保留。raw 保留可投影分量说明，但不消费
其语义、不传播冲突的公共网格。完整字段词汇见英文权威文档。

静态 DependencyMapPiece 表达精确 Data 及反向 dirty fan-out。generic 路径支持
离散 Footprint 和正/负/零 stride；planar 路径按映射片段请求上游，未选 producer
不执行，重复来源选择去重。内部矩形规划包络只用于调度，payload 请求保持精确。
物化输出一次准备并原子发布请求窗口，页面只覆盖实际请求样本。generic/planar
来源可在非通道 shape 一致时组合。

通用张量 view 需要共同 owner 和单一仿射映射；planar view 需要共同根 owner 的
连续物理通道、相同空间映射及各来源自己的授权覆盖。auto 在不能证明时物化；
强制 view 返回 ViewUnavailable。视图保留 backing 和资源计费到最终 owner 退休。

完整公开测试为 `tests/integration/test_channel_assembly.cpp`。运行
`test_channel_assembly`、`test_channel_extraction`、`test_planar_image_workflow`、
`test_compiler`，预期四项通过。C 的跨 tile 用例只发布 [127,130)×[127,130) 的
来源通道 0 和 2，目标映射为 [2,0,2]：27 个 UInt8 输出样本仅请求 18 个来源字节，
未发布的通道 1 不参与读取。测试还覆盖 dtype/axis、特殊浮点位、stride、元数据
赋值与冲突、共享 view、混合图、资源上限、override 和 ICC 生命周期。

安装消费目标 `photospider_channel_assembly_consumer` 使用相同公开测试源码。
性能复现、Xcode Instruments 和 WSL Linux/x64 结果见
[性能工作流](../../../examples/channel_assembly_performance/README.md)。


## FMT-03 通道编辑

`photospider/format/channel_editing.hpp` 导出 `format::swizzle_channels` 和
`format::replace_channels`。两者事务式展开为 `channel.assemble_mapped_<profile>`，
不注册独立 swizzle/replace key，也不恢复旧别名。
[最小公开 workflow](../../../examples/channel_editing/README.md)运行偏移 ROI，
逐字节检查结果 `[8,21,31,0.5]`。

每个输入包含实际推导 metadata 和显式 channel/component/scalar 结构。输入 0
是 base，确定输出通道轴及非通道网格。A 支持选择、重复、遗漏和填充；B 同时读取
原始输入，替换唯一目标，保留未列出的样本和语义。name/role 精确区分大小写且
唯一；raw 要求显式轴和 index。七种 dtype、rank 1..8 均逐位保留，包括 NaN
payload 和负零。literal 显式携带 dtype 与字节，不经过 Float64，不执行 alpha 算术。

A 投影来源组件含义，仅保留唯一完整映射的组及 companion。B 计算完整目标描述，
避免 replacement 的额外源字段泄漏。显式组替换重叠或同名组，其余适用组保留。
raw 丢弃与新通道轴不相容的描述，override 仅影响本次调用。全部连接的空间输入
均检查坐标兼容性，包括未读取样本的输入；不隐式重采样。

FMT-03 lowerer 将 generic scalar fill 融合到 C 的内部 `s` 来源记录，每个被选择
输出坐标映射至 `V[0]`。普通 FMT-02C 的公开 `ChannelSourceStructure` 仍只接受
component/channels。生成节点携带 `authoring_member`、有界无损
`expected_inputs` 断言及 `output_description_complete`，参加正常编译身份计算。
helper 验证输入声明，编译时重新核验 producer metadata。全常量输出仍保留 base
Descriptor 检查，但不读取 base Data。

相同 typed literal 共用一个 `channel.scalar_literal_<profile>` provider。
其 Int64 `dtype` 必须是七种有效 ElementType，String `bits` 是精确本机字节序的
小写 hex。provider 无输入，输出 generic `[1]`、端口 `values`、空 facets，
使用单样本 Whole 语义和标量大小的独立 storage。非法 dtype/宽度/hex 在预检拒绝。
扩图按实际唯一 provider 加一个 C 节点计数，使用防冲突 ID，节点上限 65536；
标量填充不分配空间中间张量。

generic 的单 owner affine 结果可保留零步幅 scalar view；独立 owner 需物化，
canonical image 不能别名 scalar storage。planar 映射路径预留完整输出虚拟跨度，
只提供请求页，逻辑 Data 和 valid coverage 不随页或 tile 取整扩大。优化后的标量
复制在最多 1024 样本的块内倍增已初始化字节前缀，保留取消/currentness 检查和
原子发布，无浮点算术、未请求邻接读取、私有线程池或完成结果缓存。

`test_channel_editing` 为集成回归；`photospider_channel_editing_consumer` 针对
安装公共包运行同一 fixture。[性能结果](../../../examples/channel_editing_performance/README.md)
记录 FP32 128x128、4096x4096 continuous/tiled、稀疏请求、布局、复制与 backing
统计及优化效果。实现事实与规格 Proposed 决策状态分别记录。


## FMT-06 数值格式转换

包 0.23.0 注册 `numeric.convert_format_strict`，接收一个 `input` 张量，
发布同形状、同坐标需求的 `values` 张量。必需的 `dtype` String 可选 `uint8`、
`uint16`、`int8`、`int16`、`int64`、`float32`、`float64`；七种类型的两个方向
均可组合。

默认 `rescale=true` 按 dtype 选择区间：无符号为 `[0,max]`，有符号为
`[min,max]`，浮点为 `[0,1]`。`rescale=false` 执行数值 cast，且拒绝区间与轴参数。
可选的 `source_range`、`target_range` 使用有类型端点 String：`i:<有符号十进制>`
或 `f64:<16位小写十六进制位>`；两个端点用逗号组成一对，完整通道表用分号连接。
出现通道表时必须给出非负 Int64 `axis`。来源下界必须小于上界；目标端点可以
倒序但不能相等。未请求通道的表项也在静态阶段检查。来源区间外按公式外推；
算子不改变 transfer、颜色模型或 alpha 关联。

可选 `rounding` 只接受 `ties_even`；`overflow` 为 `reject`（默认）或 `clip`。
`metadata_mode` 为 `respect`（默认）、附带 `metadata_override` 的 `override`、
或 `raw`。`layout` 为 `auto`（默认）、`view` 或 `materialize`；仅能对静态证明
的同 dtype 恒等映射及合法通用 Value owner 使用 view。平面图像物化请求页。
编码与 decoder 元数据随映射更新；不能用二进制浮点精确表示的 decoder 端点
使用 TDM4 有理数。只有被请求的样本会触发样本域错误。恒等复制保留 NaN 与
有符号零位；非恒等 NaN 遵循 FMT-06 payload 规则。
strict 实现在符合条件的连续平面区段内使用 AArch64 NEON 或运行时检查的
amd64 AVX2，覆盖 UInt8→Float32、
Float32→UInt8 和 Float64→Float32。异常样本及缩窄至次正规数的样本使用精确
标量路径。Int64→UInt8 使用精确整数区段内核。ISA 选择保持 strict 数值结果，
不单独注册 accelerated 入口。不支持 AVX2 的 amd64 CPU 使用可移植标量实现。
在编译器支持 ACLE、运行时具备 SME/SME_F64F64 且流式向量为 64 字节的 Apple
arm64 上，Float32→UInt8 还会将完全位于请求区域内的连续矩形（4,096～65,536
样本）合并为一次 streaming 调用。整数位检查确认整个矩形合法后，再用
binary64 精确乘法与 ties-even 舍入生成输出。检查拒绝时使用既有精确路径；
非整行或带行间 padding 的矩形保留逐行路径。工作预算在检查前计入，失败尝试
及其回退均计账。内部借用宿主的单调取消标志，至多每 64 样本用原子读取
检查一次，期间保持 streaming mode。宿主成功并完成取消/currentness 检查后
才发布输出。`PHOTOSPIDER_ENABLE_NUMERIC_CONVERSION_SME` 可关闭此候选；
Apple arm64 且编译支持时默认启用。其他转换对继续使用 NEON/AVX2。

公开 workflow 与字节/元数据检查见
[`test_numeric_conversion.cpp`](../../../tests/integration/test_numeric_conversion.cpp)；
全图、单通道与跨 tile 性能复现见
[`numeric conversion benchmark`](../../../examples/numeric_conversion_performance/README.md)。

## FMT-08 元数据赋值与删除

包 0.22.0 实现原生 `metadata.assign_<profile>` 和事务式
`format::remove_metadata`。样本与逻辑坐标保持不变，patch/replace/cascade
发布独立不可变描述。图像输入输出保持 planar；generic 路径用于非图像数值张量。
[v3 schema 与运行时约定](Tensor-Semantic-Metadata.zh.md) 说明类型化数值编码、
采样、profile/配置资源、路径、所有权和错误。
[公开示例](../../../examples/metadata_workflow/README.md) 检查特殊浮点位模式和源不可变性；
[性能说明](../../../examples/metadata_performance/README.md) 报告 planar 全图、单通道、
跨 tile 区域测量及有界复制优化。
