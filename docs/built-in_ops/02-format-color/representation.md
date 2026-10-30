# 通道、数值格式与颜色管理

本页是 FMT-01～18 的入口目录。端口、参数、公式与数值类别以链接的英文族契约和成员规格为准。

## 基本术语

| 术语 | 含义 |
| --- | --- |
| 显式颜色描述 | 颜色模型、通道顺序、原色与白点、transfer、scene/display reference、亮度单位与 alpha 关联作为消费算子的静态参数传入；Result/Tensor 只携带结构与数据，不附着或传播描述。 |
| [TensorDescription](op_specs/FMT-COLOR_color_array_contract.md) | NUM/CRV 与 FMT 共用的颜色描述编码。NUM/CRV 描述单个逻辑颜色 tuple（`channel_axis=0`），FMT 描述实际张量轴；接收 CRV 输出时按其 shape 显式设置通道轴。 |
| straight 图像 | 完整图像采用 straight alpha，alpha 位于同一 tensor 内；预乘转换只出现在显式边界或算子内部。外部 alpha 可作为显式输入，不保留跨张量关联。 |
| 相对坐标尺度 | Lab/LCh 存储 `l=L*/100`，允许有限负值与 HDR，见[通用相对坐标尺度](op_specs/FMT_relative_coordinate_scale.md)。 |
| 同尺寸平面 | DAG 内 Y/Cb/Cr 等颜色平面与 alpha 始终同尺寸、同采样网格（4:4:4）；外部子采样与打包由 codec 处理，见[codec 边界](op_specs/FMT_codec_boundary.md)。 |
| raw 数值路径 | 未提供颜色描述时，算子按公式和结构约束处理数值，不推断颜色解释。 |

## 基础数值规定

FMT 继承 [FMT 共同契约](op_specs/FMT_common_contract.md)以及 [NUM 共同契约](../01-numeric/op_specs/NUM_common_contract.md)的舍入规定。语义样本有效性、有限性与整数目标的可表示性是 unchecked 调用方前提。整数 dtype 主要用于输入输出编码。ICC 与 OCIO 使用独立外部引擎契约（Little CMS 2.19.1、OpenColorIO 2.5.2），Float64 端口不代表完整 Float64 内部精度。模型间转换按“基础转换对 + 显式组合路径”定义，覆盖范围见[模型转换覆盖表](op_specs/FMT_model_conversion_coverage.md)；不提供统一的自动转换入口。

## 族目录

FMT-07、FMT-16、FMT-17 已退休，编号保留。

| 族 | 输入 → 输出 | 关键参数和语义 |
| --- | --- | --- |
| [FMT-01 通道提取族](op_specs/FMT-01_channel_extraction_contract.md) | 单 tensor Result→选定 cell-axis 分量或独立 channel handles | A 静态索引；B 静态名称／角色；C 编译期展开 A 节点；batch axes 保留；rank-one 要求 `keepdims=true` |
| [FMT-02 通道组装与拼接族](op_specs/FMT-02_channel_assembly_contract.md) | A 单分量插轴组装；B 通道轴拼接；C 显式映射组装 | 非通道 shape 严格相同；A/B 顺序固定，C 源可复用；三者逐位复制；下游目标解释另行显式配置 |
| [FMT-03 通道重排与替换族](op_specs/FMT-03_channel_editing_contract.md) | A 重排／子集／重复／常量槽位；B 定点替换，其他槽位直通 | A/B 都只改数据映射；不继承源或目标描述；同 dtype 标量及有类型字面量；原输入同时取值 |
| [FMT-04 alpha 边界适配族](op_specs/FMT-04_alpha_association_contract.md) | A straight 图像→预乘数值；B 预乘数值→straight 图像 | 语义适配输入输出均含内置 alpha，shape／通道位置不变；预乘结果仅用于显式边界 |
| [FMT-05 alpha 编辑族](op_specs/FMT-05_alpha_editing_contract.md) | A 设置／添加内置 alpha；B 提取；C 移除 | 颜色逐位保留，零 alpha 不清除隐藏 straight 颜色；共享通道局部编辑 |
| [FMT-06 数值类型与区间转换](op_specs/FMT-06_numeric_conversion_contract.md) | 单 tensor Result→目标 dtype 的同坐标 `values` | 默认按 dtype 区间缩放，可显式关闭；完整 channel table 用 cell-relative axis |
| [FMT-07 已退休](op_specs/FMT-07_retired.md) | 不再定义本族成员 | 数值编码约束归 FMT-06 后续成员；抖动／半色调归 GRD-29/30；opaque 补值规则归 FMT-05B |
| [FMT-08 描述编辑](op_specs/FMT-08_metadata_assignment_contract.md) | 显式 TensorDescription→新配置对象 | patch/replace；dependencies=error/cascade；missing=error/ignore |
| [FMT-09 transfer 编码／解码](op_specs/FMT-09_transfer_contract.md) | A 解码、B 编码；Float32/64 单 tensor Result，单 RGB/Gray 组或显式 raw components | 十类静态曲线；PQ/1886 为绝对 nits；按曲线明确 reference／定义域；cell-relative axis |
| [FMT-10 RGB 基底／XYZ／白点适应](op_specs/FMT-10_rgb_basis_contract.md) | A RGB→XYZ、B XYZ→RGB、C 适应；D 编译期 A→可选 C→B | 七预设＋自定义 xy；四种完全适应；显式 preserve_xyz／adapt；相对／绝对尺度不隐式换算；Result 单 tensor inputs/outputs |
| [FMT-11 颜色模型转换族](op_specs/FMT-11_model_conversion_contract.md) | A～P 八组方向对；Q/R Gray 提取／中性重建；S/T 二值化／两级展开 | 原生浮点、精确分量依赖；Lab/LCh 使用 l=L*/100；NCL YCbCr，显式路径 |
| [FMT-12 ICC 转换族](op_specs/FMT-12_icc_transform_contract.md) | A 双 profile；B DeviceLink；C 显式有序 profile 链 | Little CMS 2.19.1 CPU；ICC v2/v4；optimized 默认，reference 显式 |
| [FMT-13 OCIO 转换族](op_specs/FMT-13_ocio_transform_contract.md) | A 空间；B display/view；C Looks；D NamedTransform；E 文件；F 变换树 | OCIO 2.5.2 CPU；optimized 默认，reference 显式；资源／属性冻结 |
| [FMT-14 色域映射](op_specs/FMT-14_gamut_mapping_contract.md) | A 分量裁剪；B OKLab 降色度；C 线性 RGB knee 压缩；D 越界 mask | 显式目标线性 RGB 0..1；固定算法与迭代次数，无隐式 tone/transfer |
| [FMT-15 tone/view](op_specs/FMT-15_tone_view_contract.md) | A Reinhard 亮度映射；B 相对显示值与 nits 转换；C 显式 view 组合 | 曝光、白锚点和黑白亮度明确；阶段顺序由调用方列出 |
| [FMT-16 已退休](op_specs/FMT-16_retired.md) | 外部色度子采样／重建归 input/output codec | 内部 Y/Cb/Cr 始终同尺寸 1:1:1 |
| [FMT-17 已退休](op_specs/FMT-17_retired.md) | 外部布局与打包归 input/output codec | 内核内全部图像遵守 planar |
| [FMT-18 软打样与警告](op_specs/FMT-18_softproof_contract.md) | A proof 颜色；B 独立 ICC gamut alarm mask | 固定 LCMS、intent/BPC；mask 读取独立 pipeline，并要求源域维度修正 |

OpenImageIO官方提供通道重排、premult/unpremult、色彩变换等独立操作，支持这种职责拆分；Photospider的端口与默认值是本规格的选择。ICC与OCIO分别采用其公开语义和资源配置，不能只以“色域名字”替代完整输入。[^icc][^ocio]

## 颜色模型

| 名称 | 语义和必要信息 | 规格选择 |
| --- | --- | --- |
| RGB/RGBA | RGB原色、白点、transfer及alpha关联；RGBA的A不是颜色维度 | 依模型与空间描述明确支持范围 |
| Gray | 线性Y、编码luma Y′、Lab l=L*/100、OKLab L 与任意标量分量须区分 | FMT-11 显式灰度化与中性重建；通道提取不等同于灰度转换 |
| Black/White | 二值颜色与 coverage mask 语义分开；阈值／抖动／半色调需显式选择 | 明确二值描述和黑白展开；FMT-06 数值编码，codec 负责 1-bit 打包 |
| XYZ / xyY | XYZ白点/归一化；xyY在XYZ总和或y=0时的策略 | signed Float32/64，黑点显式处理 |
| CIELAB / LCh(ab) | 参考白点；存储 l=L*/100，名义0..1并允许有限扩展；a*/b*、C*尺度不变 | D50建议用于ICC连接，其他白点必须声明 |
| OKLab / OKLCh | 使用其规定的转换；OKLab L通常0..1尺度 | 按明确版本矩阵，signed cbrt，不能用Lab尺度套用 |
| HSL / HSV(HSB) | 对哪一种RGB数值计算；hue无色时无定义，L不是物理亮度 | 明确线性／编码 RGB 描述；灰色 H/S=+0；HSB 为 HSV 编写别名 |
| YCbCr | RGB′的系数矩阵、量化range、chroma siting/sampling | BT.601/709/2020等各用命名模式；YCbCr与YUV不任意互称 |
| CMYK | 印刷条件、纸白、黑生成与总墨量通过profile决定 | 仅经 ICC profile 转换；没有 profile 时不提供颜色转换 |
| ACES | ACES2065-1/ACEScg 原色空间、ACEScc/ACEScct 编码和完整渲染流程分开 | FMT-10 原色，FMT-09 编码；FMT-13 使用明确版本的 config／builtin，FMT-15 提供显式原生阶段组合，不冒用 ACES 渲染名称 |

颜色空间参考采用CSS Color 4对Lab/OKLab/极坐标和颜色转换的公开描述；CSS使用语境不自动规定Photospider的HDR工作空间。[^css] BT.2100的PQ/HLG、量化与显示参数必须使用指定版本的约定，其标准量化rounding不能被通用cast的ties-even默认覆盖。[^itu]

## 数学核心

`a≤0.04045` 时sRGB解码为 `a/12.92`，其他非负值为 `((a+0.055)/1.055)^2.4`。RGB ramp 采用对绝对值运算后恢复符号的扩展，见其具体规格。RGB原色转换使用线性值，经XYZ及所选白点适应，再到目标线性RGB。对编码RGB直接乘原色矩阵会得到不同结果。

Lab转换中 `f(t)=cbrt(t)` 当 `t>(6/29)^3`，否则 `t/(3(6/29)^2)+4/29`；标准量 `L*=116f(Y/Yn)-16`，原生存储 `l=L*/100`、`a*=500(f(X/Xn)-f(Y/Yn))`、`b*=200(f(Y/Yn)-f(Z/Zn))`。LCh 的坐标转换使用 `C=hypot(a,b)`、弧度 `h=atan2(b,a)`，或显式输出 `atan2(b,a)/pi` 的 π 倍数；原点策略由对应转换规格定义。已有 LCh/HSL 输入的 hue 在 ramp/LUT 中保留原值与圈数，不做归一化或角度制转换。采样、白点与超范围策略影响结果，不能仅测试常见RGB颜色块。

## alpha 与 emission 边界

导入预乘数据时，零 alpha 非零颜色是否保留为独立 emission 由显式边界契约确定。[^exr] 颜色算子以显式 straight 描述作为输入约定，处理指定颜色组并按各自规则复制内置 alpha。空间重采样、混合与合成分别规定内部 alpha 运算、零 alpha 输出及工作色彩空间；独立 emission 不参与 alpha 运算。

## 使用与验收

概念链路：`decode→explicit input_description/parameters→straight boundary adaptation if needed→transfer decode→primaries/profile transform→grade→explicit gamut/tone map→transfer encode→numeric format encode→host export`。decoder 已输出线性 camera RGB 或已应用白平衡时，从相应阶段进入，避免重复处理。

解析检查：identity 矩阵与通道重排往返；黑白与参考白；直通 alpha 位模式；0、0.04045、1 等分段点；灰轴中性；合法 signed/HDR；整数上下限与 halfway 舍入。ICC/OCIO 以固定资源和已知色块检查数值或 ΔE，精度目标由对应成员规格定义。

## 逐成员入口

| ID / spec | 类别 | 注册 key | 实现 |
| --- | --- | --- | --- |
| [FMT-01A — extract a cell axis by index](op_specs/FMT-01A_extract_channel_index.md) | primitive / D1_draft | `channel.extract_index_strict`<br>`channel.extract_index_accelerated_apple_silicon`<br>`channel.extract_index_accelerated_x86_64` | 已实现 |
| [FMT-01B — extract a cell axis by name or role](op_specs/FMT-01B_extract_channel_named.md) | primitive / D1_draft | `channel.extract_named_strict`<br>`channel.extract_named_accelerated_apple_silicon`<br>`channel.extract_named_accelerated_x86_64` | 已实现 |
| [FMT-01C — author one extraction node per channel](op_specs/FMT-01C_split_channels.md) | composite_workflow / D1_draft | — | 已实现 |
| [FMT-02A — assemble single components along a new channel axis](op_specs/FMT-02A_assemble_channels.md) | primitive / D1_draft | `channel.assemble_strict`<br>`channel.assemble_accelerated_apple_silicon`<br>`channel.assemble_accelerated_x86_64` | 已实现 |
| [FMT-02B — concatenate existing channel axes](op_specs/FMT-02B_concatenate_channels.md) | primitive / D1_draft | `channel.concatenate_strict`<br>`channel.concatenate_accelerated_apple_silicon`<br>`channel.concatenate_accelerated_x86_64` | 已实现 |
| [FMT-02C — assemble explicitly mapped components](op_specs/FMT-02C_assemble_mapped_channels.md) | primitive / D1_draft | `channel.assemble_mapped_strict`<br>`channel.assemble_mapped_accelerated_apple_silicon`<br>`channel.assemble_mapped_accelerated_x86_64` | 已实现 |
| [FMT-03A — reorder, select and fill channel slots](op_specs/FMT-03A_swizzle_channels.md) | composite_workflow / D1_draft | — | 已实现 |
| [FMT-03B — replace selected channel slots](op_specs/FMT-03B_replace_channels.md) | composite_workflow / D1_draft | — | 已实现 |
| [FMT-04A — export one group as premultiplied numeric samples](op_specs/FMT-04A_associate_alpha.md) | primitive / D1_draft | `alpha.associate_strict`<br>`alpha.associate_accelerated_apple_silicon`<br>`alpha.associate_accelerated_x86_64` | 已实现 |
| [FMT-04B — import premultiplied numeric samples as straight](op_specs/FMT-04B_unassociate_alpha.md) | primitive / D1_draft | `alpha.unassociate_strict`<br>`alpha.unassociate_accelerated_apple_silicon`<br>`alpha.unassociate_accelerated_x86_64` | 已实现 |
| [FMT-05A — set or add an internal alpha plane](op_specs/FMT-05A_set_alpha.md) | primitive / D1_draft | `alpha.set_strict`<br>`alpha.set_accelerated_apple_silicon`<br>`alpha.set_accelerated_x86_64` | 已实现 |
| [FMT-05B — extract internal alpha or explicitly generate opaque alpha](op_specs/FMT-05B_extract_alpha.md) | composite_workflow / D1_draft | — | 已实现 |
| [FMT-05C — remove one group's alpha relationship](op_specs/FMT-05C_remove_alpha.md) | composite_workflow / D1_draft | — | 已实现 |
| [FMT-06A — convert tensor dtype and numeric interval](op_specs/FMT-06A_convert_numeric_format.md) | primitive / D1_draft | `numeric.convert_format_strict` | 已实现 |
| [FMT-08A — patch or replace an explicit description](op_specs/FMT-08A_assign_metadata.md) | sdk_function / D1_draft | — | 已实现 |
| [FMT-08B — delete selected descriptions](op_specs/FMT-08B_remove_metadata.md) | sdk_function / D1_draft | — | 已实现 |
| [FMT-09A — decode transfer](op_specs/FMT-09A_decode_transfer.md) | primitive / D1_draft | `color.transfer_decode_strict`<br>`color.transfer_decode_accelerated_apple_silicon`<br>`color.transfer_decode_accelerated_x86_64` | 已实现 |
| [FMT-09B — encode transfer](op_specs/FMT-09B_encode_transfer.md) | primitive / D1_draft | `color.transfer_encode_strict`<br>`color.transfer_encode_accelerated_apple_silicon`<br>`color.transfer_encode_accelerated_x86_64` | 已实现 |
| [FMT-10A — linear RGB to XYZ](op_specs/FMT-10A_rgb_to_xyz.md) | primitive / D1_draft | `color.rgb_to_xyz_strict`<br>`color.rgb_to_xyz_accelerated_apple_silicon`<br>`color.rgb_to_xyz_accelerated_x86_64` | 已实现 |
| [FMT-10B — XYZ to linear RGB](op_specs/FMT-10B_xyz_to_rgb.md) | primitive / D1_draft | `color.xyz_to_rgb_strict`<br>`color.xyz_to_rgb_accelerated_apple_silicon`<br>`color.xyz_to_rgb_accelerated_x86_64` | 已实现 |
| [FMT-10C — adapt an XYZ reference white](op_specs/FMT-10C_adapt_xyz_white.md) | primitive / D1_draft | `color.adapt_xyz_white_strict`<br>`color.adapt_xyz_white_accelerated_apple_silicon`<br>`color.adapt_xyz_white_accelerated_x86_64` | 已实现 |
| [FMT-10D — compose linear RGB basis conversion](op_specs/FMT-10D_convert_linear_rgb.md) | composite_workflow / D1_draft | — | 已实现 |
| [FMT-11A — XYZ to normalized CIELAB](op_specs/FMT-11A_xyz_to_cielab.md) | primitive / D1_draft | `color.xyz_to_cielab_strict`<br>`color.xyz_to_cielab_accelerated_apple_silicon`<br>`color.xyz_to_cielab_accelerated_x86_64` | 已实现 |
| [FMT-11B — Normalized CIELAB to XYZ](op_specs/FMT-11B_cielab_to_xyz.md) | primitive / D1_draft | `color.cielab_to_xyz_strict`<br>`color.cielab_to_xyz_accelerated_apple_silicon`<br>`color.cielab_to_xyz_accelerated_x86_64` | 已实现 |
| [FMT-11C — Normalized CIELAB to CIELCh(ab)](op_specs/FMT-11C_cielab_to_cielch.md) | primitive / D1_draft | `color.cielab_to_cielch_strict`<br>`color.cielab_to_cielch_accelerated_apple_silicon`<br>`color.cielab_to_cielch_accelerated_x86_64` | 已实现 |
| [FMT-11D — CIELCh(ab) to normalized CIELAB](op_specs/FMT-11D_cielch_to_cielab.md) | primitive / D1_draft | `color.cielch_to_cielab_strict`<br>`color.cielch_to_cielab_accelerated_apple_silicon`<br>`color.cielch_to_cielab_accelerated_x86_64` | 已实现 |
| [FMT-11E — Relative D65 XYZ to OKLab](op_specs/FMT-11E_xyz_to_oklab.md) | primitive / D1_draft | `color.xyz_to_oklab_strict`<br>`color.xyz_to_oklab_accelerated_apple_silicon`<br>`color.xyz_to_oklab_accelerated_x86_64` | 已实现 |
| [FMT-11F — OKLab to relative D65 XYZ](op_specs/FMT-11F_oklab_to_xyz.md) | primitive / D1_draft | `color.oklab_to_xyz_strict`<br>`color.oklab_to_xyz_accelerated_apple_silicon`<br>`color.oklab_to_xyz_accelerated_x86_64` | 已实现 |
| [FMT-11G — OKLab to OKLCh](op_specs/FMT-11G_oklab_to_oklch.md) | primitive / D1_draft | `color.oklab_to_oklch_strict`<br>`color.oklab_to_oklch_accelerated_apple_silicon`<br>`color.oklab_to_oklch_accelerated_x86_64` | 已实现 |
| [FMT-11H — OKLCh to OKLab](op_specs/FMT-11H_oklch_to_oklab.md) | primitive / D1_draft | `color.oklch_to_oklab_strict`<br>`color.oklch_to_oklab_accelerated_apple_silicon`<br>`color.oklch_to_oklab_accelerated_x86_64` | 已实现 |
| [FMT-11I — Described RGB to HSL](op_specs/FMT-11I_rgb_to_hsl.md) | primitive / D1_draft | `color.rgb_to_hsl_strict`<br>`color.rgb_to_hsl_accelerated_apple_silicon`<br>`color.rgb_to_hsl_accelerated_x86_64` | 已实现 |
| [FMT-11J — HSL to its described RGB](op_specs/FMT-11J_hsl_to_rgb.md) | primitive / D1_draft | `color.hsl_to_rgb_strict`<br>`color.hsl_to_rgb_accelerated_apple_silicon`<br>`color.hsl_to_rgb_accelerated_x86_64` | 已实现 |
| [FMT-11K — Described RGB to HSV](op_specs/FMT-11K_rgb_to_hsv.md) | primitive / D1_draft | `color.rgb_to_hsv_strict`<br>`color.rgb_to_hsv_accelerated_apple_silicon`<br>`color.rgb_to_hsv_accelerated_x86_64` | 已实现 |
| [FMT-11L — HSV to its described RGB](op_specs/FMT-11L_hsv_to_rgb.md) | primitive / D1_draft | `color.hsv_to_rgb_strict`<br>`color.hsv_to_rgb_accelerated_apple_silicon`<br>`color.hsv_to_rgb_accelerated_x86_64` | 已实现 |
| [FMT-11M — Encoded RGB to NCL YCbCr](op_specs/FMT-11M_rgb_to_ycbcr_ncl.md) | primitive / D1_draft | `color.rgb_to_ycbcr_ncl_strict`<br>`color.rgb_to_ycbcr_ncl_accelerated_apple_silicon`<br>`color.rgb_to_ycbcr_ncl_accelerated_x86_64` | 已实现 |
| [FMT-11N — NCL YCbCr to encoded RGB](op_specs/FMT-11N_ycbcr_ncl_to_rgb.md) | primitive / D1_draft | `color.ycbcr_ncl_to_rgb_strict`<br>`color.ycbcr_ncl_to_rgb_accelerated_apple_silicon`<br>`color.ycbcr_ncl_to_rgb_accelerated_x86_64` | 已实现 |
| [FMT-11O — XYZ to xyY](op_specs/FMT-11O_xyz_to_xyy.md) | primitive / D1_draft | `color.xyz_to_xyy_strict`<br>`color.xyz_to_xyy_accelerated_apple_silicon`<br>`color.xyz_to_xyy_accelerated_x86_64` | 已实现 |
| [FMT-11P — xyY to XYZ](op_specs/FMT-11P_xyy_to_xyz.md) | primitive / D1_draft | `color.xyy_to_xyz_strict`<br>`color.xyy_to_xyz_accelerated_apple_silicon`<br>`color.xyy_to_xyz_accelerated_x86_64` | 已实现 |
| [FMT-11Q — Described color to corresponding Gray](op_specs/FMT-11Q_color_to_gray.md) | primitive / D1_draft | `color.color_to_gray_strict`<br>`color.color_to_gray_accelerated_apple_silicon`<br>`color.color_to_gray_accelerated_x86_64` | 已实现 |
| [FMT-11R — Gray to corresponding neutral color](op_specs/FMT-11R_gray_to_color.md) | primitive / D1_draft | `color.gray_to_color_strict`<br>`color.gray_to_color_accelerated_apple_silicon`<br>`color.gray_to_color_accelerated_x86_64` | 已实现 |
| [FMT-11S — Gray to binary Black/White](op_specs/FMT-11S_gray_to_black_white.md) | composite_workflow / D1_draft | — | 已实现 |
| [FMT-11T — Binary Black/White to explicit Gray levels](op_specs/FMT-11T_black_white_to_gray.md) | primitive / D1_draft | `color.black_white_to_gray_strict`<br>`color.black_white_to_gray_accelerated_apple_silicon`<br>`color.black_white_to_gray_accelerated_x86_64` | 已实现 |
| [FMT-12A — ordinary ICC profile conversion](op_specs/FMT-12A_transform_icc_profiles.md) | external_engine_adapter / D1_draft | `color.icc_transform_lcms2_19_1_cpu` | 已实现 |
| [FMT-12B — apply an ICC DeviceLink](op_specs/FMT-12B_apply_icc_devicelink.md) | external_engine_adapter / D1_draft | `color.icc_apply_devicelink_lcms2_19_1_cpu` | 已实现 |
| [FMT-12C — execute an ordered ICC profile chain](op_specs/FMT-12C_transform_icc_chain.md) | external_engine_adapter / D1_draft | `color.icc_transform_chain_lcms2_19_1_cpu` | 已实现 |
| [FMT-13A — configured color-space conversion](op_specs/FMT-13A_convert_ocio_space.md) | external_engine_adapter / D1_draft | `color.ocio_convert_space_2_5_2_cpu` | 已实现 |
| [FMT-13B — configured display and view](op_specs/FMT-13B_apply_ocio_display_view.md) | external_engine_adapter / D1_draft | `color.ocio_display_view_2_5_2_cpu` | 已实现 |
| [FMT-13C — explicit Look sequences](op_specs/FMT-13C_apply_ocio_looks.md) | external_engine_adapter / D1_draft | `color.ocio_looks_2_5_2_cpu` | 已实现 |
| [FMT-13D — configured NamedTransform](op_specs/FMT-13D_apply_ocio_named_transform.md) | external_engine_adapter / D1_draft | `color.ocio_named_transform_2_5_2_cpu` | 已实现 |
| [FMT-13E — frozen file transform](op_specs/FMT-13E_apply_ocio_file.md) | external_engine_adapter / D1_draft | `color.ocio_file_transform_2_5_2_cpu` | 已实现 |
| [FMT-13F — explicit transform tree](op_specs/FMT-13F_apply_ocio_transform_tree.md) | external_engine_adapter / D1_draft | `color.ocio_transform_tree_2_5_2_cpu` | 已实现 |
| [FMT-14A — RGB component gamut clipping](op_specs/FMT-14A_clip_rgb_gamut.md) | primitive / D1_draft | `color.clip_rgb_gamut_strict` | 已实现 |
| [FMT-14B — OKLab chroma reduction](op_specs/FMT-14B_reduce_oklab_chroma.md) | primitive / D1_draft | `color.reduce_oklab_chroma_v1_strict` | 已实现 |
| [FMT-14C — Linear RGB neutral-ray compression](op_specs/FMT-14C_compress_rgb_chroma.md) | primitive / D1_draft | `color.compress_rgb_chroma_v1_strict` | 已实现 |
| [FMT-14D — RGB gamut diagnostic mask](op_specs/FMT-14D_check_rgb_gamut.md) | primitive / D1_draft | `color.check_rgb_gamut_strict` | 已实现 |
| [FMT-15A — Explicit-exposure luminance mapping](op_specs/FMT-15A_map_luminance.md) | primitive / D1_draft | `color.map_luminance_v1_strict` | 已实现 |
| [FMT-15B — Display relative and nits conversion](op_specs/FMT-15B_convert_display_luminance.md) | primitive / D1_draft | `color.convert_display_luminance_strict` | 已实现 |
| [FMT-15C — Explicit view composition](op_specs/FMT-15C_compose_view.md) | composite_workflow / D1_draft | — | 已实现 |
| [FMT-18A — ICC proof-rendered display colors](op_specs/FMT-18A_softproof_icc.md) | external_engine_adapter / D1_draft | `color.softproof_lcms2_19_1_cpu` | 已实现 |
| [FMT-18B — Independent ICC gamut alarm](op_specs/FMT-18B_check_icc_gamut.md) | external_engine_adapter / D1_draft | `color.gamut_alarm_lcms2_19_1_source_dims_v1_cpu` | 已实现 |

## 相关入口

- [数学 oracle](../../../oracle/ops/format_color/README.md)

## 来源

[^icc]: International Color Consortium，[*ICC.1:2022 Profile Specification*](https://www.color.org/specification/ICC.1-2022-05.pdf)，2022-05；profile、PCS与rendering intent。
[^ocio]: Academy Software Foundation，[*OpenColorIO Documentation*](https://opencolorio.readthedocs.io/en/latest/)，滚动文档；配置、processor与display/view体系。
[^css]: W3C，[*CSS Color Module Level 4*](https://www.w3.org/TR/css-color-4/)，滚动规范草案；Lab、OKLab、色彩转换与色域概念。
[^itu]: ITU-R，[*BT.2100-3: Image parameter values for high dynamic range television*](https://www.itu.int/rec/R-REC-BT.2100)，2025；指定版本的PQ/HLG与量化要求。
[^exr]: OpenEXR，[*Technical Introduction: Premultiplied vs. Un-Premultiplied Color Channels*](https://openexr.com/en/latest/TechnicalIntroduction.html#premultiplied-vs-un-premultiplied-color-channels)，滚动官方文档；零alpha发光颜色。
