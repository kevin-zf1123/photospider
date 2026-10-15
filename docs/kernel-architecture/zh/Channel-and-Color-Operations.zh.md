# 通道与颜色算子

[英文权威页面](../Channel-and-Color-Operations.md)。

内置格式算子使用 Result 端口和当前 Result C ABI 4。Result 只携带结构与样本，不附着颜色、alpha、transfer、采样或 encoding 描述。每个算子通过显式静态参数接收所需解释。描述是配置，不是样本有效性的证明；内核不会仅因调用者对结构相容的数据赋予不同含义而拒绝连线。

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

三种 CPU profile 是 strict、accelerated_apple_silicon、accelerated_x86_64；加速 profile 需要匹配的主机能力。数值格式转换只有 strict key。Proposed 规格状态与注册状态分开记录；这些族没有 GPU 实现。

## 结构性通道操作

提取、组合与编辑保留 UInt8、Int8、UInt16、Int16、Int64、Float32、Float64 的样本位模式，包括 NaN payload 和 signed zero。它们不执行颜色转换、alpha 归一化或样本领域扫描。轴索引相对于 cell axes，不含 Result batch prefix；完整 sample rank 至多为 8。Assembly 接受 1..1024 个 single-tensor/no-fields Results，要求非 scalar 输入的 batch prefix 与非通道 shape 匹配，输入/输出样本上限为 2^40。Extraction 服从 Result backing 和请求区域的可表示性，没有族级 dense product 上限。

Extraction 精确请求选中来源的 Data 与 Descriptor。keepdims=false 删除选定 cell axis，不能产生 rank zero；若删除 atomic trailing axis，结构字段同步调整。Split helper 校验完整规范结构与物理布局，包括 forward producer reference；输出不投影描述。

Assembly 插入、拼接或映射通道索引。所有连接输入都参与静态 Descriptor 检查，payload 只读取选中坐标。Mapped record 只有 input、match、selector、destination，每个目标恰好填一次；没有 output_description 或 destination_component 参数。Replacement 同时读取原始不可变输入；被覆盖的 base 样本只有被另一个请求目标选中时才读取。Scalar record 显式重复无 batch 的 `[1]` 值，普通 plane 不隐式广播。

通用 view 要求在保留 backing 上证明可表示的仿射映射；空间 view 还需满足 Result 存储证明。相同字节或共享 ancestry 本身不足。Auto 仅因物理 view 不可用而物化，保留其他错误。物化在 Root 预算和取消约束下事务式发布请求覆盖，不读取缺失样本、padding 或未请求通道。Empty observation 做静态检查但不处理样本。

`channel.literal_like_<profile>` 接收一个 tensor Result，仅请求 Descriptor。准备好的 bits、shape/axis/keepdims、source assertion 与 layout 定义生成输出；不接受输出描述。`channel.scalar_literal_<profile>` 是无输入 Whole primitive，从精确 native bits 产生同 dtype 的 `[1]` tensor。

## Alpha

FMT-04 的描述路径要求显式 RGB/Gray group、internal alpha 和对应源 association。即使 alpha 为零，也校验请求颜色的有限性和 alpha 定义域。公式、舍入、下溢及 raw IEEE 行为见族规格。无描述时，raw 调用使用显式 components 和 internal/plane/scalar 权重。FMT-04 不隐式插入 alpha，也不导出 metadata alpha 关系。

FMT-05 要求调用方提供 straight 解释。Set 使用 Float32/64，校验请求的新 alpha∈[0,1] 与选定颜色；Extract/remove 按位处理全部七种 dtype，不新增样本领域扫描。已有 alpha 提取复制精确位模式；alpha_encoding 只配置缺失 alpha 的 opaque 回退，其 coverage-one code 必须精确可表示。

带通道的 remove 使用 mapped assembly、结构 assertion 和对应 layout 策略。无轴且缺失 alpha 的 identity 使用 core.identity，返回 value，不增加 source assertion，不转换 layout。后续消费者接收自己的描述。读取 alpha 不代表已发布 alpha coverage；后续消费者按普通路径请求必要样本。

## 数值格式与 transfer

FMT-06 实现全部 49 对 dtype 转换。Rescale 默认为 true，源与目标区间分别来自显式值或 dtype 默认值；pure cast 拒绝 ranges 和 axis。逐通道 range table 使用显式 cell axis。精确仿射结果只向目标舍入一次，再执行 reject/clip。Result 保留结构 schema identity、tensor key、shape、batch，改变 dtype 并将 atomic_trailing_axes 设为零；不传播 decoder。仅需 Data 与 Descriptor；同 dtype 的静态 identity 可保留合法 view。SIMD/SME 路径共享 strict 数值契约，测量时仍需独立的 native dispatch 证据。

FMT-09 有 input_description 时要求唯一 group，并禁止 raw axis/components。Decode 可从该配置解析完整曲线；encode 要求目标 curve 和 linear 源解释。无描述时必需 components 与 curve，index list 还要求 axis。各曲线的单位/reference 校验保留。输出保留 shape/dtype、atomic_trailing_axes=0，不带 transfer 标签。

Linear 与 gamma=1 是 bit-copy identity。合法完整 identity 可使用 view，并执行所需领域校验。其他映射物化；forced view 拒绝静态非恒等变换。Simple 路径每 run 最多处理 1024 个样本，普通曲线路径每 batch 最多 64 个，使用按需准入的 workspace 并恢复浮点环境。

## 基底、白点与模型转换

FMT-10 应用精确 RGB/XYZ 矩阵，以及显式选择的 XYZ Scaling、Bradford、CAT02、CAT16 适应。描述路径要求选定 RGB/XYZ group；raw 调用要求 axis、有序 components 与几何。非 identity 行消费全部三个来源项，包括零系数；精确 identity 只读对应样本，并在配置要求时校验。旁路 alpha/AOV 精确复制。Transfer、exposure、gamut 和 rendering 是独立操作。

D 在修改原图前暂存完整展开。它根据显式源描述推导中间 XYZ 配置并写入生成节点参数，不从边恢复描述。A→B 和 A→C→B 保留各节点的舍入与失败；相同源/目标 basis 不允许将组合替换成 identity。

FMT-11 A–R/T 提供 57 个按 profile 区分的原生 model key，S 使用三个 MASK threshold key。显式描述可解析唯一 group，并允许相符的 axis/components assertion。Raw 调用提供成员要求的 selectors、white、hue unit、NCL 系数、Gray kind 或常量。Raw XYZ→xyY 需要实现声明的 white_x/white_y。Q 删除两个选定槽；R 展开 Gray 槽或插入显式 output_axis。输出不附着模型描述。

仅 Q 支持 forced view。选定描述样本使用 Data 与 Validation；raw T 始终校验精确 0/1 selector；raw S 与旁路使用 Data；常量 R 输出只需 Descriptor。发布、错误、owner、预算和取消服从 Result 契约。数值报告通过 SDK report_numeric/reader 使用内核有界通用诊断通道；cache hit 不报告未执行的算术。
