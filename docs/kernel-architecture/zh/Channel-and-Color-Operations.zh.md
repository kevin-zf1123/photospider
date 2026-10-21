# 通道与颜色算子

[英文权威页面](../Channel-and-Color-Operations.md)。

内置格式算子使用 Result 端口；各族的调用协议分别在下文说明。Result 只携带结构与样本，不附着颜色、alpha、transfer、采样或 encoding 描述。每个算子通过显式静态参数接收所需解释。描述是配置，不是样本有效性的证明；内核不会仅因调用者对结构相容的数据赋予不同含义而拒绝连线。

公开 workflow helper 与运行算子属于 `Photospider::ops`。消费者链接该目标，它依赖 `Photospider::ops_sdk`、`Photospider::ops_headers` 与 kernel。共享描述、codec、资源封装、精确领域数学和 `format::edit_description` 属于 `Photospider::ops_sdk`；SDK 只使用 kernel 公共接口。`TensorDescription` codec 按坐标记录选择规范 TDM4/TDM5，见[张量描述](Tensor-Semantic-Metadata.zh.md)。

## 注册算子族与配置

| 算子族 | 公开 key 或 helper | 显式配置与输出 |
| --- | --- | --- |
| [FMT-01](../../built-in_ops/02-format-color/op_specs/FMT-01_channel_extraction_contract.md) | `channel.extract_index_<profile>`、`channel.extract_named_<profile>`；`format::split_channels` | axis、index 或 name/role selector、keepdims、layout；可选 input_description 提供轴与名称选择。values 按位复制；split 返回独立 handles。 |
| [FMT-02](../../built-in_ops/02-format-color/op_specs/FMT-02_channel_assembly_contract.md) | `channel.assemble_<profile>`、`channel.concatenate_<profile>`、`channel.assemble_mapped_<profile>` | 结构轴、映射和可选逐输入 input_descriptions；values 无附着描述。 |
| [FMT-03](../../built-in_ops/02-format-color/op_specs/FMT-03_channel_editing_contract.md) | `format::swizzle_channels`、`format::replace_channels` | mapped assembly 上的事务式组合；typed literal 添加 scalar_literal 节点；均返回 values。 |
| [FMT-04](../../built-in_ops/02-format-color/op_specs/FMT-04_alpha_association_contract.md) | `alpha.associate_<profile>`、`alpha.unassociate_<profile>` | 可选 input_description+group，否则显式 raw structure/components/alpha source。values 总是物化，无 layout 参数。 |
| [FMT-05](../../built-in_ops/02-format-color/op_specs/FMT-05_alpha_editing_contract.md) | `alpha.set_<profile>`；`format::extract_alpha`、`format::remove_alpha` | 必需 input_description 与 group。Set/extract 返回 values；remove 返回 mapped values 或无轴 core.identity.value。 |
| [FMT-06](../../built-in_ops/02-format-color/op_specs/FMT-06_numeric_conversion_contract.md) | `numeric.convert_format_strict` | dtype、rescale、显式/默认 range、条件性 axis、rounding、overflow、layout；没有描述参数。 |
| [FMT-08](../../built-in_ops/02-format-color/op_specs/FMT-08_metadata_assignment_contract.md) | SDK `format::edit_description` | 纯配置编辑，无 workflow key、图修改、payload 读取或资源查找。 |
| [FMT-09](../../built-in_ops/02-format-color/op_specs/FMT-09_transfer_contract.md) | `color.transfer_decode_<profile>`、`color.transfer_encode_<profile>` | 显式曲线参数与可选输入描述；输出 values。 |
| [FMT-10](../../built-in_ops/02-format-color/op_specs/FMT-10_rgb_basis_contract.md) | RGB/XYZ、XYZ/RGB、白点适应 key；`format::convert_linear_rgb` | 显式源解释与几何；D 生成 A→可选 C→B，并写入显式中间节点参数。 |
| [FMT-11](../../built-in_ops/02-format-color/op_specs/FMT-11_model_conversion_contract.md) | 19 个原生成员各有三种 profile；S helper 使用 MASK threshold key | 显式描述或有序 raw selector 与模型参数；输出 values，Q/R 改变通道数。 |
| [FMT-14](../../built-in_ops/02-format-color/op_specs/FMT-14_gamut_mapping_contract.md) | `color.clip_rgb_gamut_strict`、`color.reduce_oklab_chroma_v1_strict`、`color.compress_rgb_chroma_v1_strict`、`color.check_rgb_gamut_strict` | 显式原生相对线性 RGB；A/B/C 输出 values，D 输出同 dtype 二值 mask。 |
| [FMT-15](../../built-in_ops/02-format-color/op_specs/FMT-15_tone_view_contract.md) | `color.map_luminance_v1_strict`、`color.convert_display_luminance_strict`；`format::compose_view` | 显式 RGB/linear-Y Gray、gain/curve 或显示范围；native C 保留逐阶段舍入和显式配置。 |

三种 CPU profile 是 strict、accelerated_apple_silicon、accelerated_x86_64；加速 profile 需要匹配的主机能力。数值格式转换与原生 FMT-14/15 只有 strict key。Proposed 规格状态与注册状态分开记录；这些族没有 GPU 实现。

## 结构性通道操作

提取、组合与编辑保留 UInt8、Int8、UInt16、Int16、Int64、Float32、Float64 的样本位模式，包括 NaN payload 和 signed zero。它们不执行颜色转换、alpha 归一化或样本领域扫描。轴索引相对于 cell axes，不含 Result batch prefix；完整 sample rank 至多为 8。Assembly 接受 1..1024 个 single-Tensor Results，要求非 scalar 输入的 batch prefix 与非通道 shape 匹配，输入/输出样本上限为 2^40。Extraction 服从 Result backing 和请求区域的可表示性，没有族级 dense product 上限。

Extraction 使用固定调用 ABI 8，每个选中输出 tile 生成一个 CPU Host task。Lowering 将输出坐标映射到选中源通道，再求源网格的 tile 闭包；实际计算只复制选中坐标，物理输入 tile 可能包含其他通道。keepdims=false 删除 cell axis，并在必要时减少 atomic trailing count；batch prefix 保持不变。准备阶段验证精确 read slots、输出几何和零 scratch；输入可使用合法 signed/zero stride 或未对齐偏移，DenseHost 通过显式 COPY 导入。Auto/materialize 将位模式复制到独立 dense 输出；非空 view task 只依赖源 schema 并返回 InvalidArgument/InvalidDomain，Empty 保留静态检查且无 payload task。每至多 256 个样本检查取消。Split helper 在编译时核对完整 schema 和 spatial topology，包含 forward producer references；输出不附着描述。

Assembly 插入、拼接或映射通道索引。所有连接输入都参与静态 Descriptor 检查，payload 只读取选中坐标。Mapped record 只有 input、match、selector、destination，每个目标恰好填一次；没有 output_description 或 destination_component 参数。Replacement 同时读取原始不可变输入；被覆盖的 base 样本只有被另一个请求目标选中时才读取。Scalar record 显式重复无 batch 的 `[1]` 值，普通 plane 不隐式广播。

Assembly 使用固定 ABI 8，每个选中输出 tile 对应一个零 scratch CPU task。Lowering 按目标 span 映射源区域，逐端口求源网格 tile 并集；不跨过未使用的源区间填补空隙。完整输出 tile 可能包含多个通道，输入 tile 也可能覆盖额外样本。未参与映射的端口仅保留 schema 依赖。Auto/materialize 逐位复制到独立 dense 输出；非空 view 仅依赖 schema 并返回 InvalidArgument/ViewUnavailable，Empty 无 payload task。准备阶段验证精确 slots，且在展开预期源集合前以实际 slots 数限制闭包。运行时使用不可变 span/slot 表、固定大小坐标和一个缓存 view，每至多 256 个样本检查取消。失败调用不发布部分结果；输出 owner 可独立于源和上下文存活。

`channel.literal_like_<profile>` 使用固定调用 ABI 8。每个选中输出 tile 对应一个 CPU Host task；它通过 `schema_reads` 引用输入结构，不读取输入 payload。准备阶段核对源 schema assertion、bits、axis/keepdims、layout、输出 tile 几何和零 scratch。auto/materialize 将同 dtype 原始位模式写入预分配输出，保留 signaling NaN 位模式；view 在非空执行时返回 InvalidArgument/InvalidDomain，Empty 不执行填充。填充每至多 256 次写入检查取消。输出及逐调用退休结果遵循固定执行器的所有权协议。`channel.scalar_literal_<profile>` 使用零输入、零 scratch 的固定 task，从精确 native bits 生成一个同 dtype `[1]` tile。

## Alpha

FMT-04 的描述路径要求显式 RGB/Gray group、internal alpha 和对应源 association。固定 ABI 8 task 要求调用者保证参与颜色、alpha 和舍入结果的有效性，包括零 alpha 分支的语义前提。公式、舍入、下溢及 raw IEEE 行为见族规格。无描述时，raw 调用使用显式 components 和 internal/plane/scalar 权重。FMT-04 不隐式插入 alpha，也不导出 metadata alpha 关系。

FMT-05 要求调用方提供 straight 解释。Set 使用固定 ABI 8 和 Float32/64；颜色与 alpha 的语义有效性由调用者保证。每个输出槽只读其映射源，复制颜色不增加 alpha 验证读；完整输出 tile 决定源读取闭包。Set auto/materialize 使用独立 dense 输出与零 scratch，非空 view 仅依赖 schema 并返回 ViewUnavailable。Extract/remove 按位处理全部七种 dtype，不新增样本领域扫描。已有 alpha 提取复制精确位模式；alpha_encoding 只配置缺失 alpha 的 opaque 回退，其 coverage-one code 必须精确可表示。

带通道的 remove 使用 mapped assembly、结构 assertion 和对应 layout 策略。无轴且缺失 alpha 的 identity 使用 core.identity，返回 value，不增加 source assertion，不应用 helper 的 layout 策略；core.identity 生成显式 COPY task。后续消费者接收自己的描述。读取 alpha 不代表已发布 alpha coverage；后续消费者按普通路径请求必要样本。

## 数值格式与 transfer

FMT-06 通过固定调用 ABI 8 实现全部 49 对 dtype 转换。Rescale 默认为 true，源与目标区间分别来自显式值或 dtype 默认值；pure cast 拒绝 ranges 和 axis。逐通道 range table 使用显式 cell axis。精确仿射结果只向目标舍入一次。reject 要求舍入结果可表示，整数目标要求原始源样本有限；clip 和浮点 NaN/Inf 映射仍是结果语义。Result 保留结构 schema identity、tensor key、shape、batch，改变 dtype 并将 atomic_trailing_axes 设为零；不传播 decoder。每个完整输出 tile 对应一个 task，读取自身在源网格上的闭包并写入独立 dense 分配。identity 用零 scratch 复制原始位，非 identity 使用固定精确算术 scratch。非空 identity view 仅依赖 schema，并返回 ViewUnavailable。SIMD/SME 路径共享 strict 数值契约，测量时仍需独立的 native dispatch 证据。

FMT-09 有 input_description 时选择唯一 group（含无轴 Gray group）或显式无轴 Gray component，并禁止 raw axis/components。Decode 可从该配置解析完整曲线；encode 要求目标 curve 和 linear 源解释。无描述时必需 components 与 curve，index list 还要求 axis。显式合并相容的 global/group/channel 断言与 analytic binding。规范 scene/display reference 要求显式 coordinate scale；相对单位接受 relative/1（Gray 也接受 relative_luminance），绝对显示单位接受 cd/m2/cd/m^2，兼容 reference 拼写须与显式 scale 一致。Gray 在 encode/linear decode 时必须为 linear_y，非线性 decode 时为 encoded_luma；拒绝 Lab/OKLab lightness。保留各曲线的 reference 校验。输出保留 shape/dtype、atomic_trailing_axes=0，不带 transfer 标签。

FMT-09 的六个 CPU keys 使用固定调用 ABI 8。语义输入有限性、曲线定义域和输出有限性属于完整保留输出 tile 上的调用者前提。Linear 与 gamma=1 按位复制，旁路 tile 使用零 scratch。Auto/materialize 写独立 dense tile；非 identity forced view 静态拒绝，非空 identity view 仅依赖 schema 并返回 ViewUnavailable。

一般 transfer task 每批收集 64 个点，使用预先声明的 fast/compact/wide scratch。有理分支静态编译为有界二次多项式，超越函数分支通过显式状态执行定向区间精度细化。内部精度／容量重试与取消、调用者资源失败分开处理。调用内数值状态不分配堆存储，在多个 Run 间独立，并恢复浮点环境。Gamma=2 保留带符号 square/sqrt SIMD 路径，使用较小的固定收集缓冲区。

## 基底、白点与模型转换

FMT-10 应用精确 RGB/XYZ 矩阵，以及显式选择的 XYZ Scaling、Bradford、CAT02、CAT16 适应。描述路径要求选定 RGB/XYZ group；raw 调用要求 axis、有序 components 与几何。九个 CPU keys 使用固定调用 ABI 8。非 identity 行消费全部三个来源项，包括零系数；完整矩阵 identity 只读对应样本。语义输入与输出有限性是保留输出 tile 上的调用者前提。源闭包按输入网格展开，auto/materialize 写独立 dense tile。非 identity forced view 静态拒绝；精确 I 的非空 view 只读 schema 并返回 ViewUnavailable。旁路 alpha/AOV 精确复制。Transfer、exposure、gamut 和 rendering 是独立操作。

FMT-10 接受 relative/1 与 cd/m2/cd/m^2 单位别名，并校验显式 coordinate scale 一致；别名不产生数值转换。

D 在修改原图前暂存完整展开。它根据显式源描述推导中间 XYZ 配置并写入生成节点参数，不从边恢复描述。A→B 和 A→C→B 保留各节点的舍入与失败；相同源/目标 basis 不允许将组合替换成 identity。

FMT-11 A–R/T 提供 57 个按 profile 区分的原生 model key，S 使用三个 MASK threshold key。显式描述可解析唯一 group，并允许相符的 axis/components assertion。Raw 调用提供成员要求的 selectors、white、hue unit、NCL 系数、Gray kind 或常量。Raw XYZ→xyY 需要实现声明的 white_x/white_y。Q 删除两个选定槽；R 展开 Gray 槽或插入显式 output_axis。输出不附着模型描述。

全部 60 个 keys 使用固定调用 ABI 8。每个完整选中输出 tile 通过不可变通道 span 映射源区域，再取源网格 tile 并集。纯复制和常量使用零 scratch；无源样本依赖的 task 保留 schema 依赖。算术使用固定有理数／区间 scratch 和 64 点收集缓冲区。Auto/materialize 产生独立 dense 输出。Q 静态接受 forced view，但非空 view task 仅依赖 schema，并返回 InvalidArgument/ViewUnavailable；其他成员静态拒绝 forced view。Empty 无 payload task。语义数值有效性与 T 的精确 0/1 selector 属于调用者前提。黑 XYZ、零 chroma 的数学替代规则与 raw IEEE 行为保留。静态描述检查拒绝有效 association 非 straight 的输入。发布遵循成功调用的顺序退休；此路径不发送逐算子数值报告。

FMT-11 算术拥有固定容量的有理数工作区，以及供根、幂、三角函数和 atan2 共用的 checked interval pool。取消与算术容量失败保留首次错误的 code/reason，并停止后续工作回调。精度细化只清除尚未收敛的区间状态，不重试已取消或容量耗尽的算术。每个分量求值前重置数值状态。这些算术状态转换使用显式状态，不使用异常存储。工作回调在求值期间借用；区间回调引用工作区内的有理数状态，因此工作区禁止复制和移动。Kernel preparation 核对精确 read slots、输出几何与 scratch 后，通过固定 CPU 入口调用该工作区。

## 原生色域与亮度映射

FMT-14 A-D 与 FMT-15 A/B 提供六个固定 ABI 8 strict CPU key，接收一个 Float32/Float64 tensor，完整 rank 1..8、sample count 至多 2^40。描述选择完整 RGB group，FMT-15 也接受 linear-Y Gray；raw 调用提供所需 selector、几何、model/reference 与单位。静态检查 native encoding、straight-compatible association、组重叠、空间 channel axis 与几何字段结构。各模式的数值／几何参数域、样本有限性及中间量／结果可表示性均为 unchecked 前提，不扫描输入或发布结果。其他组、alpha、AOV 逐位复制。

Clip 与显示范围转换逐分量计算；OKLab 降色度、RGB 压缩、gamut mask 与 RGB 亮度映射读取该点的完整三颜色分量。源区域按输入网格取闭包，每个完整输出 tile 对应一个固定 task。auto/materialize 写入独立预分配 dense 输出，atomic_trailing_axes=0；forced view 静态拒绝。Mask 投影保留 batch/空间拓扑，rank-one 使用 keepdims。准备阶段检查完整输入/输出 schema、精确 read slots 与 scratch。算术状态位于已准入的固定 scratch；旁路、clip、mask 不需要算术 workspace。取消检查间隔至多 64 点、256 次输出写入，并进入昂贵的精确运算内部。Empty 保留静态校验且没有 payload task。

Strict 实现使用精确有理公式及指定的阶段舍入。Float32 定向区间与矩阵候选只有在证明相同舍入位模式后才接受，否则回退精确运算。显示转换预编译精确仿射行。FMT-14B 保留固定 32/64 步 dyadic 搜索和已接受候选，不合并 RGB/XYZ/OKLab 阶段。不新增 accelerated key、近似 SIMD 契约或 GPU backend。[实现说明](../../built-in_ops/02-format-color/FMT-14-15_implementation.md)分别记录独立 oracle、公开工作流、算术耗时与 submit-through-join 耗时。

## 显式原生与配置化 view

`format::compose_view` 由 `photospider/ops/format/view.hpp` 和 ops umbrella header 导出，验证非空有序 native FMT-09/10/14/15 与 configured FMT-13A..F 阶段及最终显示条件。各阶段提供完整输入/输出描述与适用参数；FMT-10D 额外显式指定 profile。helper 解析真实图边、规范相容的原生断言、保留无关配置并校验黑白亮度/transfer 一致。描述结构在任何索引访问前验证，错误参数类型返回失败；显示亮度有限性与顺序、正权重及白点／基色几何合法性均为 unchecked 数值前提。只有完整推导成功才提交工作副本。不重排、融合、消除 identity，也不添加隐藏 transfer 或 native C key。

资源感知重载接收显式 ResourceSet，同时支持 NativeDisplayConditions 和 ConfiguredDisplayConditions。它在提交前构建真实 OCIO processor、解析规范空间与方向，并验证最终 configured display/view 端点。D/E/F 的端点含义仍是调用方断言。Configured 端点之后的原生阶段要求相容的 analytic binding；无关组、alpha/AOV 断言和 axes 保持完整。

## 固定外部颜色引擎

FMT-12 A/B/C、FMT-13 A..F 和 FMT-18 A/B 提供十一个固定调用 ABI 8 CPU keys。ICC 使用私有 Little CMS 2.19.1 context、double formatter、五种准入端点模型与显式 profile/chain 设置。OCIO 使用私有 2.5.2 processor，冻结属性并闭合 config/context/file 解析。OCIO Float64 输入显式舍入至 Float32，结果精确扩宽；alpha/AOV 逐位旁路。

每个 prepared kernel 拥有独立引擎、内存准入和 mutex；static operation state 中没有资源相关的隐式可变缓存。Empty/bypass-only 请求也执行完整资源构建验证。构建同步且不可取消，保留分配/工作量上限及首次失败锁存。像素执行每批至多 64 点，继续支持取消。现有准备阶段 ResourceAllocationScope 桥接将保留引擎计入执行预算；独立资源验证使用显式构建上限。内核及 ABI 冻结边界未改变。

FMT-18B 构建独立源域 alarm pipeline，Gray 为 49、RGB 为 49³、CMYK 为 23⁴ 网格点，并在 score 有限的 unchecked 前提下按 score>0 输出精确 1，否则输出 +0；不通过显示报警色反推 mask。构建检查返回实际标签、路径、文件、插值与选中的 Look。操作键、构建身份、独立 oracle 和实测平台边界见[外部引擎实现与性能](../../built-in_ops/02-format-color/FMT-12-13-18_implementation.md)。
