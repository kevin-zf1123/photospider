# 通道与颜色算子

当前 default registry 提供 FMT-01 extraction、FMT-02 assembly、FMT-03 editing helpers、FMT-04 alpha association、FMT-05A alpha setting、FMT-06 numeric conversion、FMT-08 metadata assignment、FMT-09 transfer conversion、FMT-10 RGB basis conversion 以及 `channel.literal_like_<profile>`、`channel.scalar_literal_<profile>` Result primitives 和 FMT-11 model conversion，使用 Result operation ABI 2。FMT-05B/C 是基于已注册 operations 的编译期 helper 组合；FMT-10D 是对已注册 A/B/C operations 的公开 graph helper；FMT-11 的 native model conversion 已注册；S helper 降低为 MASK threshold operation。[英文镜像](../Channel-and-Color-Operations.md) 描述相同的当前状态。

下表区分当前 Result 注册项和仍保持 Proposed 的其他 FMT 契约。

## 当前 Result 算子族与保留的 FMT 契约

下文提到的 `photospider/ops/format/*.hpp` 编写辅助属于只含头文件的 `Photospider::ops_headers` target。它们的实现编译在内核库中，因此使用方需要同时链接 `Photospider::kernel` 和 `Photospider::ops_headers`。

| 算子族 | Keys 与公开 authoring API | 当前状态 |
| --- | --- | --- |
| 通道选择 | Result keys `channel.extract_index_<profile>`、`channel.extract_named_<profile>`；`format::split_channels` | 已注册 Result operation；一个 tensor member 且没有 fields；精确选中通道的 Data + Descriptor support。 |
| Literal-like fill | `channel.literal_like_<profile>` | 已注册单输入 Result operation；仅请求 Descriptor support，并按 output descriptor 对请求样本重复 prepared raw bits。 |
| Scalar literal | `channel.scalar_literal_<profile>` | 已注册的无输入 Whole Result primitive；以 prepared native bits 输出 shape `[1]`，供 FMT-03 literal source 使用。 |
| Metadata 编辑 | Result key `metadata.assign_<profile>`；`format::assign_metadata`、`format::remove_metadata` | 已注册 Result operation；一个 tensor member 且没有 fields；same-coordinate Data + Descriptor，无 Validation/Control 或 planar callback。 |
| 通道组装与编辑 | `channel.assemble_<profile>`、`channel.concatenate_<profile>`、`channel.assemble_mapped_<profile>`；已安装的 `channel_assembly.hpp`、`channel_editing.hpp` helpers | Result keys 已注册；FMT-03 helpers 展开为 mapped assembly，没有独立 swizzle/replace key。 |
| Alpha 关联与编辑 | `alpha.associate_<profile>`、`alpha.unassociate_<profile>`、`alpha.set_<profile>`；已安装 `photospider/ops/format/alpha.hpp` helpers | 三个成员各有 strict、Apple Silicon 和 x86-64 CPU profile，共九个 Result ABI 2 keys。FMT-05B/C helpers 编译期组合为已注册的 extraction、literal-like fill、mapped assembly 与 metadata assignment operations。 |
| 数值格式转换 | `numeric.convert_format_strict` | 单一已注册 strict Result ABI 2 key，覆盖七种 dtype 之间的 49 个转换对。 |
| Transfer 转换 | `color.transfer_encode_<profile>`、`color.transfer_decode_<profile>`；`photospider/ops/format/transfer.hpp` 提供 `TransferDefinition` codec | encode/decode 各有 strict、Apple Silicon 与 x86-64 CPU Result ABI 2 key，共六个已注册 keys。 |
| RGB basis 转换 | `color.rgb_to_xyz_<profile>`、`color.xyz_to_rgb_<profile>`、`color.adapt_xyz_white_<profile>`；已安装 `photospider/ops/format/rgb_basis.hpp` helpers | A/B/C 跨三个 profile 共注册九个 Result ABI 2 CPU keys。`format::convert_linear_rgb` 事务式组合已注册 stages，不增加 D key。 |
| Model 转换 | A-R、T 的 `color.*_<profile>`；FMT-11S helper 降低为 `mask.threshold_channel_<profile>` | 三个 CPU profiles 共 57 个 native model keys；S 不新增 color key。单 tensor Float32/Float64 Result，full sample rank 至多 8；sample count 受 Result schema 可表示范围和执行资源限制约束。 |

带 profile 后缀的 channel extraction、assembly、alpha、metadata、transfer、RGB basis、literal-like fill 和 scalar-literal operations 使用 `strict`、`accelerated_apple_silicon` 或 `accelerated_x86_64`；加速变体要求宿主支持对应能力。Numeric conversion 使用唯一的不带后缀 strict key。Model conversion keys 使用 strict、accelerated_apple_silicon 和 accelerated_x86_64 profiles。当前 Result operations 使用 `start_result` 与 Result tensor protocol。

公开 tensor-description encoder 在不存在 `coordinates` 记录时生成 v4；tensor、component、channel 或 group 含有 `coordinates` 时生成 v5。Decoder 接受版本与 discriminator 一致的 v4/TDM4 和 v5/TDM5；旧版本和 discriminator 失配均拒绝。兼容的 coordinate assertion 按字段合并；同一字段的非空断言不相等时失败。完整 schema 见[张量语义 metadata](Tensor-Semantic-Metadata.zh.md)，model 专用要求见 [FMT-11 model conversion contract](../../../docs/built-in_ops/02-format-color/op_specs/FMT-11_model_conversion_contract.md)。

## FMT-01 通道提取

`channel.extract_index_{strict,accelerated_apple_silicon,accelerated_x86_64}` 按静态 `index` 提取；`channel.extract_named_` 使用相同 profile 后缀，在独立的 `match=name|role` 命名空间精确匹配 `selector`。Named CPU profile 必须符合宿主能力。六个 Result key 对 UInt8、UInt16、Int8、Int16、Int64、Float32 和 Float64 保留所有样本位，包括 NaN payload。

每个 node 接收只有一个 tensor member 且没有 fields 的 Result，发布 `values` Result。静态参数 `metadata_mode=respect|raw|override`、`axis`、`keepdims` 和 `layout=auto|view|materialize`。`axis` 索引 tensor descriptor，不包括 Result batch prefix；执行时将它平移到完整 sample shape。Respect 将显式 axis 与 TensorDescription 比较。Override 使用 `tensor_description_parameter` 编码的本地描述。Named lookup 要求完整 channel table，不能使用 raw。输入输出保留 batch axes；`keepdims=false` 删除选中的 cell axis，要求 descriptor rank 至少为 2。操作不做颜色转换、alpha 归一化、样本验证或浮点运算。

Public authoring interface 从 `photospider/ops/format/channel.hpp` 安装。Runtime 投影选中 component 的 TensorDescription，并按 shape 变化重映射适用 cell-axis metadata。普通提取输出只保留投影后的 TensorDescription facet，不保证保留所有 opaque annotations。单通道不会因此获得 complete-color 或样本有效性保证。`format::split_channels` 为每个通道展开一个 index node，返回 `c0`、`c1` 等句柄；helper 校验声明的完整 schema 和 physical layout。每个生成 node 使用完整 canonical schema 的 domain-separated SHA-256 摘要（64 个小写十六进制字符）和独立的 physical-layout assertion，以便编译时检查 producer。摘要包含 opaque metadata 与 batch axes，不读取样本，也不是样本哈希或有效性证明。

Dependency-v2 以 Data (1) 和 Descriptor (8) 请求精确的选中通道 support，不请求 Validation 或 Control。Dirty mapping 将选中通道变化映射到对应输出坐标；未选通道不使输出变脏。Generic tensor view 支持有效的正、负和零 stride，Result mapper 可以分区并保留多个 backing owner。选取 spatial tensor 的 channel axis 可以保留源 physical owner；切取 spatial height/width 时，auto 会按投影后的 layout materialize，强制 view 返回 `InvalidArgument/InvalidDomain` 和 `ViewUnavailable`。Squeeze 删除 spatial axis 时输出成为 generic layout；keepdims 保留 extent-one spatial layout。物化只复制请求的输出区域，每 256 样本以内检查取消。Empty demand 发布空 Result，不读取 source payload。Result cache 被禁用。

[公开 Result workflow](../../../examples/channel_extraction_workflow/README.md) 使用 B/A/R/G UInt8 `[2,2,4]` tensor，只请求 `c2` 第二行 ROI，检查结果 `[12,13]`。`test_channel_extraction` 覆盖 direct index/named、任意轴、七种 dtype、batch/spatial layouts、owner partitions、metadata、resources、取消和限制。

```sh
cmake --build build --target photospider_channel_extraction_workflow test_channel_extraction -j 8
./build/examples/channel_extraction_workflow/photospider_channel_extraction_workflow
ctest --test-dir build -R '^test_channel_extraction$' --output-on-failure
```

## Scalar literal primitive

Default registry 注册三个 `channel.scalar_literal_<profile>` CPU keys。这个无输入
Whole Result operation 输出 generic shape `[1]` tensor，dtype 由调用方指定，payload 为已准备好的小写 hex native bytes。支持 UInt8、UInt16、Int8、Int16、Int64、Float32 和 Float64。它服务 FMT-03 literal sources，不实现 alpha helper。每次运行使用独立准备状态；Empty observation 不保留 payload state。

## Literal-like fill primitive

Default registry 注册三个 `channel.literal_like_<profile>` CPU keys。每个 operation 接收一个含单 tensor member 的 Result，并发布一个含单 tensor member 的 Result。Execution 只请求 Descriptor support（role 8），不读取来源 sample payload。Operation 根据 `bits` 重复同 dtype raw bits；`output_description`、可选 cell-axis `axis` 和 `keepdims` 定义输出 tensor。它保留 batch axes、schema id、tensor key、opaque facets 和 owned resources。完整 sample count 为 `[1, 2^40]`。其可选 `axis` 索引 cell axes，不包含 Result batch prefix。

静态必需参数为 `bits`、`expected_inputs`、`output_description`、`layout`、`authoring_member` 和 `keepdims`；`axis` 可选。`expected_inputs` 使用 `result-v1`、64 字符 schema digest 和单独的 physical-layout assertion。`auto` 与 `materialize` 生成请求字节；非空 forced `view` 返回 `ViewUnavailable`。Empty observation 无状态。此 primitive 可用于 FMT-05B opaque 路径的 lowering，但不是公开 `extract_alpha` helper。

## FMT-02 通道组装

`channel.assemble_<profile>`、`channel.concatenate_<profile>` 和
`channel.assemble_mapped_<profile>` 各提供 strict、Apple Silicon 与 x86 CPU
profile。每个 operation 接收 1 到 1024 个 input Results，每个输入含一个 tensor member，
没有 fields，并发布一个 `values` Result。七种 dtype 按位复制。所有输入的 batch prefix 必须一致；
非 scalar 来源在通道轴以外的 sample shape 必须相同。完整输入与输出 sample count 不超过 2^40；
batch axes 与 cell axes 合计的 full sample rank 不超过 8。各成员的 `axis`、`input_axes` 和
`output_axis` 均索引 cell axes，不包含 batch prefix。输出保留首来源的 schema id、tensor key、
global metadata 和 publication policy；tensor facets 根据推导出的 TDM 重建，不复制未知 source
tensor facets。

A 在相同 component shape 中插入通道轴。B 按输入顺序拼接通道块，每个输入可使用不同通道轴。C 把显式 component/channel selector 映射到完整输出槽位，每个槽位恰有一个来源。所有已连接输入，即使未被映射使用，也执行 Descriptor 检查；Data 仅覆盖请求输出区域内实际映射的来源。操作不广播、不重采样、不转换数值或执行样本域验证。

已安装的 `photospider/ops/format/channel_assembly.hpp` helpers 追加静态 graph node，不读取样本。generic retained view 要求整个请求区域由同一 `CpuStorage` owner 和单一 affine 地址式表示。planar view 还须由 ResultBuilder 证明同一根、按输出顺序连续的 planes 和匹配 row pitch。强制 view 缺少证明时返回 `ViewUnavailable`；`auto` 仅在该错误下回退复制。物化只读取请求区域。每次运行拥有独立的 prepared state，Empty observation 不保留 payload state。Need 按每组 64 个处理，最多执行 16 组并进行 publication。1024 输入用例需要显式 64 MiB metadata capacity；默认 16 MiB 会受控地返回资源错误。[性能指南](../../../examples/channel_assembly_performance/README.md) 将历史 Value/planar 表格与当前 Result smoke 区分；未运行完整 Result 矩阵或 profiling。

## FMT-03 通道编辑

已安装的 `format::swizzle_channels` 与 `format::replace_channels` 是 graph authoring helpers，展开为 `channel.assemble_mapped_<profile>`；使用 literal 时还会用 `channel.scalar_literal_<profile>`。没有独立的 native swizzle/replace key。Swizzle 映射非空有序槽位；replace 对原始输入的不同目标同时赋值，空替换列表表示 identity。Literal 使用预先准备的 native bits。所有连接输入，包括未使用输入，都参与 Descriptor 检查；Data 需求遵循有效来源映射。

FMT-03 的所有 nonscalar inputs 必须与 base 具有完全相同的 batch prefix；scalar inputs 是无 batch 的 shape `[1]`。Axis 参数索引 cell axes，不包含 batch prefix。包含 batch 与 cell axes 的 full sample rank 不超过 8。FMT-03 输入断言以 `result-inputs-v1:` 开头，后接 64 个小写 SHA-256 十六进制字符，固定 81 字符。带 domain separation 的摘要覆盖所有连接输入的完整有序 canonical schema 和 physical layout，包括重复及未使用输入。不包含 samples，也不是 samples 有效性的证明。Literal operation 使用 shape `[1]` 和 prepared native bits，并采用 Whole 语义。Scalar row repetition 每次最多复制 256 个 samples，同时计费 work 并检查 cancellation。[公开 workflow](../../../examples/channel_editing/README.md) 展示可运行示例。

## FMT-04 alpha 关联

`format::associate_alpha` 与 `format::unassociate_alpha` 会追加已注册的
`alpha.associate_<profile>` 和 `alpha.unassociate_<profile>` Result operations。
每个成员均有 strict、Apple Silicon 和 x86-64 CPU profile。输入为无 fields 的
single-tensor Result；第二个可选 Result 仅用于 raw 模式显式提供 alpha plane
或 scalar。Semantic color 与被消费的 alpha 具有 Data 和 Validation support；
仅 passthrough 的值不会增加额外验证；所有连接输入均请求 Descriptor，且不使用
Control。公式、raw 模式、layout 与错误规则见 [FMT-04 contract](../../built-in_ops/02-format-color/op_specs/FMT-04_alpha_association_contract.md)。

## FMT-05 alpha 提取与移除

已安装的 `photospider/ops/format/alpha.hpp` 提供 `format::extract_alpha` 和
`format::remove_alpha` 编译期图组合。它们接收无 fields 的 single-tensor
Result，逐位保留七种支持的 dtype，保留 batch axes，并将 axis 解释为相对
cell axes 的位置。包含 batch 与 cell axes 的完整 sample rank 不超过 8，
完整 sample count 不超过 2^40。它们不扫描样本来验证 alpha domain。

存在 alpha 时，提取会降低为 `channel.extract_index`；显式 opaque fallback
降低为只请求 Descriptor support 的 `channel.literal_like`。移除含 channel
的输入会降低为 mapped assembly；component Gray identity 使用 `metadata.assign`，
仍请求 Data。helpers 在 compile time 检查来源 schema 与 physical layout。
FMT-05A `set_alpha` 和 FMT-04 association helpers 使用已注册的
`alpha.set_<profile>`、`alpha.associate_<profile>` 与
`alpha.unassociate_<profile>` native keys。

## FMT-06 数值格式转换

`numeric.convert_format_strict` 是唯一已注册的 strict Result ABI 2 key。它接收
一个只含单 tensor member、没有 fields 的 Result，并发布一个名为 `values`、只含单
tensor 的 output Result。七种 source/target dtype 共支持 49 个转换对。包含 batch
与 cell axes 的完整 sample rank 不超过 8；sample count 不超过 2^40。输出保留 Result
schema identity、tensor key、logical shape 和 batch axes，并更新 tensor dtype 与
encoding facets；`atomic_trailing_axes` 设为零。

Operation 以 Data (role 1) 和 Descriptor (role 8) 请求 source support，不请求
Validation 或 Control，不扫描未请求 samples，错误保持在请求的 observation scope。
Axis 参数索引 cell axes，不包含 batch axes。数值规则、typed endpoint 格式与 metadata
传播见 [FMT-06 contract](../../built-in_ops/02-format-color/op_specs/FMT-06_numeric_conversion_contract.md)。对完整 sample coordinate `q`，channel-table 项由
`q[batch_prefix_length + axis]` 定位；`axis` 本身是 cell-axis index，不含 Result
batch prefix。额外的 batch/view/Empty/bit-stride、ICC resource、并发 plan 复用和最终 payload 释放检查均通过；SME 测试也通过。`test_alpha_numeric_interop` 的四种 inherited/moved 组合通过；这不代表 model-conversion interop 已验证。六项 numeric/alpha/resource focused CTests 均通过。installed numeric-conversion 和 alpha-numeric-interoperability consumers 均通过；standalone performance consumer 已完成 configure/build。13 个串行 Result smoke case 均通过逐位 oracle，但不据此得出 Result 性能结论。

仅当完整 declared mapping 是静态 identity，且源 storage 可表示该映射时，same-dtype
转换才可发布 generic 或 spatial view。只请求一个恒等通道不能使整个变换结果可 view。
Forced view 无法证明时返回 `ViewUnavailable`；`auto` 执行 materialize。物化在 Root
budgets 与 cancellation 约束下事务发布请求 target-width samples。Empty demand 无状态。

单一 strict key 使用精确 scalar 转换，并在内部选择 AArch64 NEON、runtime-checked
amd64 AVX2 和符合条件的 Apple SME Float32→UInt8 路径，均遵守相同数值契约。没有 GPU
实现。

## FMT-09 Transfer 编码与解码

Registry 提供 `color.transfer_decode_<profile>` 与
`color.transfer_encode_<profile>`，分别覆盖 strict、accelerated Apple Silicon
和 accelerated x86-64 CPU profile。每个 operation 接收一个含单个 Float32 或
Float64 tensor、无 fields 的 Result，并发布一个名为 `values`、含单个 tensor 的
output Result。它保留 Result schema identity、tensor key、logical shape 和 batch
axes，更新 semantic transfer facets，并将 `atomic_trailing_axes` 设为零。包含
batch 与 cell axes 的完整 sample rank 不超过 8；sample count 不超过 2^40。

Semantic mode 选择明确的 RGB 或 Gray group。参与的颜色 samples 请求 Data 与
Validation；透传 samples 仅请求 Data。Raw mode 选择显式 components 或所有
components，仅请求 Data，不做 semantic domain validation。每个连接的输入都请求
Descriptor support；这些 operations 不请求 Control。`axis` 是 cell-axis index，
不包含 Result batch prefix。Empty output demand 不保留本次执行状态。

Linear 与 gamma 为 1 的 `power_gamma` 是逐位恒等映射。完整 identity map 可对合法
generic 或 spatial Result storage 建立 view；semantic 请求仍会验证选中的 samples。
Forced view 遇到静态非恒等映射时，在 compile/direct preflight 拒绝；identity 的
physical mapping 无法证明时，在 observation evaluation 返回失败。Auto 仅在非恒等
变换或物理 view 无法表示时 materialize。物化在 Root budgets 和 cancellation 限制下
事务发布请求区域，并遵守 Result owner 与 resource 生命周期。数值 workspace 延迟分配；
简单路径每批最多 1024 samples，通用曲线路径每批最多 64。Strict fallback 与
floating-environment 行为遵循 NUM。Transfer operations 没有 GPU backend。集成测试在 strict
和 Apple Silicon profiles 上完成 16,760 次 golden 尝试；本机不具备 x86-64
backend，因此跳过该 profile。25 个指定的串行 Result performance smoke case 均通过
golden gate，每项包含两次 warmup 和一次测量执行。这些结果不代表完整矩阵，也不足以
得出性能结论。

## FMT-10 RGB 基底与 XYZ

Registry 为 `color.rgb_to_xyz_<profile>`、`color.xyz_to_rgb_<profile>` 和
`color.adapt_xyz_white_<profile>` 在 strict、accelerated Apple Silicon 与
accelerated x86-64 profiles 下各注册一个 key，共九个 Result ABI 2 CPU keys。
每个 operation 接收一个含单个 Float32 或 Float64 tensor、无 fields 的 Result，
并发布一个名为 `values`、含单个 tensor 的 Result。输出保留 Result schema
identity、tensor key、logical shape 和 batch axes，更新适用的 semantic facets，
并将 `atomic_trailing_axes` 设为零。包含 batch 和 cell axes 的完整 sample rank
最多为 8；sample count 最多为 2^40。已安装的
`photospider/ops/format/rgb_basis.hpp` 提供静态 codec，以及
`format::rgb_to_xyz`、`format::xyz_to_rgb`、`format::adapt_xyz_white` 和
`format::convert_linear_rgb` authoring helpers。

A/B 使用 FMT-10 中的精确矩阵在 RGB 与 XYZ 坐标间转换。C 使用显式的 full
chromatic adaptation，可选 XYZ Scaling、Bradford、CAT02 或 CAT16。D 不是
native operation；它的 helper 按 A、可选 C、B 顺序追加 graph stages，保留每个
stage 的舍入和错误。Transfer、exposure、gamut mapping、alpha association 和
scene/display conversion 仍由独立 operation 处理。

Semantic operation 选择完整且有序的 RGB 或 XYZ group。非恒等映射按每个请求的
输出 row 读取三个来源 samples，包括矩阵系数为零的项；这些 samples 请求 Data 与
Validation。精确 identity matrix 只读取对应的已选 sample，但 semantic mode 仍会
验证该值。Alpha、AOV 等 bypass components 仅请求对应的 Data。Raw mode 读取显式
有序 triple，不请求 semantic Validation。所有连接输入也请求 Descriptor support
(role 8)，不请求 Control。cell axes 不含 Result batch prefix。Empty output demand
无状态，也不请求 sample Need。

精确 identity matrix 可在合法的 generic 或 spatial Result mapping 上建立 view；
`materialize` 强制复制。Forced view 对静态非恒等 mapping 在 compile/direct preflight
拒绝；identity 的 physical representation 无法证明时，在 observation evaluation
失败。输出在 Root budgets 和 cancellation 约束下事务发布。Result cache 被禁用，
保留的 owner 遵守 Result 生命周期。没有 GPU 实现。

`format::convert_linear_rgb` 先完整构建 expansion，并在静态校验成功后才修改
WorkflowDocument。调用方必须串行化同一个 document 的写入；编译完成的 plan 及其不可变
prepared state 可以并发执行。D policy 选择 A→B、使用 `preserve_xyz` 的 A→B，或带
显式 method 的 A→C→B。helper 返回 B 的 `values` edge，不注册 D key。
`test_rgb_basis_math` 与 `test_rgb_basis` 已通过；integration suite 保留 4,032 项
independent oracle 检查，并覆盖 batched inputs、三种 Result storage layouts、identity
views、并发 plan 复用和 owner 释放。65,536-channel identity view 在 512 KiB Metadata
预算下也通过。25 个性能 smoke case 通过的是同一实现 strict reference gate，不是 independent
oracle；完整矩阵和性能结论尚不可得。

## FMT-08 元数据赋值与删除

注册表提供三个 Result CPU key：`metadata.assign_strict`、`metadata.assign_accelerated_apple_silicon` 和 `metadata.assign_accelerated_x86_64`。`format::assign_metadata` 构造 A；`format::remove_metadata` 将 B 事务式降低为 A。输入和输出均是无 fields 的 single-tensor Result；保留 schema id、tensor key、batch axes、descriptor、layout、source publication policy 和样本位。执行以 role 1 和 8 请求同坐标 Data 与 Descriptor support，不请求 Validation 或 Control。操作不扫描样本，并禁用 Result cache。Public header `photospider/ops/format/metadata.hpp` 单独安装。[公开示例](../../../examples/metadata_workflow/README.md) 检查特殊值位模式与源不可变性。metadata 到 extraction 的 configuration/resource chain 已覆盖；`channel.assemble` 组合尚未覆盖。

## FMT-11 颜色模型转换

FMT-11 的 19 个 native members 在三个 CPU profiles 下注册，共 57 个 model-conversion keys。已安装的 `photospider/ops/format/model_conversion.hpp` helpers 追加一个 key 并返回 `values` port；FMT-11S 追加已注册的 `mask.threshold_channel_<profile>` operation，没有 native color key。所有输入和输出都是无 fields 的单 tensor Result，dtype 为 Float32 或 Float64。完整 sample rank（batch 与 cell axes 合计）最多为 8；sample count 受 Result schema 可表示范围和执行资源限制约束。

`axis` 和 `output_axis` 索引 cell axes，不包含 batch prefix。Q 将选中的三分量缩为一个分量；R 将一个 Gray 分量扩展为三个分量，有现有轴时将该轴的 extent 增加 2；axis-free 输入则插入一个长度为 3 的 cell axis。输出保留 schema id、tensor key 和 batches，更新模型 metadata，并将 `atomic_trailing_axes` 设为零。语义模式下被选中的 samples 请求 Data 与 Validation；raw T 验证 binary selector，raw S 和 bypass samples 只请求 Data，R 的常量输出只需 Descriptor。Empty demand 不保留运行状态。Q 只有在完整映射通过 generic affine 或 canonical spatial Result view 证明时才返回 view；证明失败时 forced view 返回 `ViewUnavailable`，`auto` 则物化。其他转换物化输出，即使请求只包含 bypass 也拒绝 forced view。发布采用事务方式，且未注册 GPU profile。sparse Q view 的测试使用符合 canonical spatial view proof 的预期。
