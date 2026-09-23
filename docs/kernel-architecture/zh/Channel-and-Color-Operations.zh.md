# 通道与颜色算子

包 0.20.0 移除旧通道、alpha、颜色算子，以及 `numeric.cast`、
`numeric.encode_range` 的源码、默认注册和专属旧测试。13 个完整 key 与保留范围见
[退休记录](../../built-in_ops/02-format-color/op_specs/FMT_legacy_retirement.md)。
查询、直接调用与编译这些 key 均返回 `NotFound`，无别名或兼容占位注册。

[目录](../../built-in_ops/02-format-color/representation.md)及
[FMT 公共规格](../../built-in_ops/02-format-color/op_specs/FMT_common_contract.md)
定义后续方向。FMT-01A/B 已有 CPU 实现，FMT-01C 提供公开 authoring helper；
规格决策状态仍为 Proposed。FMT-03..08 尚未实现，FMT-07 已退休。完整图像使用
planar 存储、straight 颜色和同张量内的 alpha。新算子须实现各自的精确请求、
metadata、数值和布局契约；旧 typed HWC 行为不构成新规格的子集实现。

NUM/CRV 使用的共享 ColorArray 描述、profile 所有权和数学基础设施保留。
这些能力不代表已注册格式转换，也不代表实现了新 FMT 算子族。

[公开退休回归](../../../tests/integration/test_format_color_retirement.cpp)覆盖
全部旧 key 的查询、直接调用和编译，并运行独立核验结果为 0.5 的
`numeric.add_strict` workflow。安装消费测试编译运行同一源码：

```sh
cmake --build build --target test_format_color_retirement -j 8
ctest --test-dir build -R '^(test_format_color_retirement|test_installed_consumer)$' --output-on-failure
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

元数据迁移为 tensor-description v2/TDM2；v1 明确拒绝，调用者重新编码后消费。
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
