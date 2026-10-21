# 通道、数值格式与颜色管理

当前 Result operation 支持状态以[Image Operations](../../kernel-architecture/Image-Operations.md)为准；此处各行分别标明当前 Result 注册状态和保留源码状态；“Proposed”只表示规格决策尚未接受，不表示实现未注册。各规格的目标行为和实现状态分别见对应规格页。

[FMT-09～18 范围审查](op_specs/FMT-09-18_scope_review.md)记录已完成的成员设计及实现前置条件。
当前列举的有效成员已完成实现，规格决策状态仍保持 Proposed。[FMT-14/15 实现与性能](FMT-14-15_implementation.md)记录六个原生算子和 native view；[FMT-12/13/18 实现与性能](FMT-12-13-18_implementation.md)记录十一个外部引擎算子、configured view、独立参考及资源/性能验证。

各规格的 Proposed 状态表示目标契约尚未接受，不等同于实现状态。当前内置 Result FMT families 包括 FMT-01 channel extraction、FMT-02 channel assembly、FMT-03 channel editing、FMT-04 alpha association、FMT-05A alpha setting、FMT-06 numeric conversion、FMT-08 SDK 描述编辑、FMT-09 transfer conversion、FMT-10 RGB basis conversion、FMT-11 model conversion、FMT-12 ICC、FMT-13 OCIO、FMT-14 gamut mapping、FMT-15A/B luminance mapping、native/configured FMT-15C helper 与 FMT-18 softproof/alarm，以及 `numeric.convert_format_strict`、`channel.literal_like_<profile>` 和 `channel.scalar_literal_<profile>` primitives。FMT-04A/B 的 alpha association operations 与 FMT-05A `alpha.set_<profile>` 使用固定 ABI 8 CPU keys；FMT-09 transfer、FMT-10 A/B/C native operations 和 FMT-11/MASK model operations 使用固定 ABI 8；FMT-05B/C helpers 编译为现有已注册 Result operations 的组合，FMT-10D 编译为 A/可选 C/B。`literal_like` 通过固定 ABI 8 CPU tile task 提供 FMT-05B opaque fallback，仅依赖输入 schema 并使用零 scratch；`scalar_literal` 是 FMT-03 无输入 shape-[1] provider。当前已有实现的 FMT source families 均提供 Result CPU operations，或提供可降低为已注册 operations 的公共 helper；Proposed 或尚无实现的规格成员不因此视为已实现。其他 FMT 成员的运行状态分别按对应规格记录。ICC/OCIO 转换使用独立外部引擎契约，Float64 端口不代表完整 Float64 内部精度。整数主要用于输入输出编码；跨通道 shape、资源输入与当前端口的限制见 G1..G5。

## 算子目录

目录已列出原 FMT-01～FMT-18 的目标功能及缺口规格；其中 FMT-07、FMT-16、FMT-17 后续已退休并分配替代归属，
其余编号不变。Result/Tensor 仅携带结构、数据和存储所有权；颜色、alpha、transfer、采样与 LUT 解释由每个消费算子的显式静态参数提供。通道操作不传播描述，内核不按领域标签匹配连线。没有描述时的 raw 数值路径仍遵循对应公式和结构约束。
完整图像采用 straight 解释，alpha 位于同一 tensor 内；外部 alpha 可作为显式输入，
不保留跨张量关联。空间 axes 表示逻辑拓扑。已接入固定 ABI 8 的算子按
[物理 Plan](../../kernel-architecture/Compiler-and-Execution.md) 声明独立 dense 输出 tile，
选中局部 tile 不要求整图连续虚拟地址。尚采用其他调用协议的族，其具体存储限制
以族规格与[内核存储规格](../../kernel-specs/Tensor-Storage-and-Region-Access.md)的适用范围为准。
该文件区分已确认结论、当前实现与待决问题；
已列成员的端口和算法见各自规格；未来扩展另列，尚未扩展当前运行时支持面。

NUM/CRV 与 FMT 统一使用 [TensorDescription 颜色描述](op_specs/FMT-COLOR_color_array_contract.md)，
旧 ColorArray 类型、编解码和兼容入口已移除。NUM 描述单个逻辑颜色 tuple，固定 `channel_axis=0`；
其物理端口仍为 Float32/Float64、rank 2～8、最后一轴含颜色分量。FMT 描述实际张量轴，
接收 CRV 输出时须显式按其 shape 设置通道轴。两者都以静态参数记录模型、白点、reference、transfer 等信息，不附着于结果。

| ID / 提议操作 | 输入 → 输出 | 关键参数和建议默认 | 实现/支持与验收 |
| --- | --- | --- | --- |
| [FMT-01 通道提取族](op_specs/FMT-01_channel_extraction_contract.md) | 单 tensor Result→选定 cell-axis 分量或独立 channel handles | A 静态索引；B 静态名称／角色；C 编译期展开 A 节点；batch axes 保留；rank-one 要求 `keepdims=true` | 固定调用 ABI 8；映射源 tile 闭包、零 scratch、独立输出；auto/materialize 逐位复制，非空 view 仅依赖 schema 并失败；输入可显式提供 TensorDescription v4/v5；输出无描述；A/B 为 CPU keys，C 为 helper。其他 FMT 家族状态以各自规格为准 |
| [FMT-02 通道组装与拼接族](op_specs/FMT-02_channel_assembly_contract.md) | A 单分量插轴组装；B 通道轴拼接；C 显式映射组装 | 非通道 shape 严格相同；A/B 顺序固定，C 源可复用；三者逐位复制；下游目标解释另行显式配置 | CPU A/B/C 使用固定 ABI 8，决策状态保持 Proposed；完整输出 tile 决定源 tile 并集；零 scratch；auto/materialize 独立复制，非空 view 仅依赖 schema 并失败 |
| [FMT-03 通道重排与替换族](op_specs/FMT-03_channel_editing_contract.md) | A 重排／子集／重复／常量槽位；B 定点替换，其他槽位直通 | A/B 都只改数据映射；不继承源或目标描述；同 dtype 标量及有类型字面量；原输入同时取值 | CPU helpers 编译期组合固定 FMT-02C，规格仍 Proposed；scalar literal 使用零输入、零 scratch 的固定调用；auto/materialize 独立复制，非空 view 失败 |
| [FMT-04 alpha 边界适配族](op_specs/FMT-04_alpha_association_contract.md) | A straight 图像→预乘数值；B 预乘数值→straight 图像 | 语义适配输入输出均含内置 alpha，shape／通道位置不变；预乘结果仅用于显式边界 | 三个 CPU profiles 使用固定 ABI 8；保持 Proposed；静态源 tile 闭包和独立输出；保留 NUM 乘除与零 alpha 数学规则，语义样本有效性为调用者前提 |
| [FMT-05 alpha 编辑族](op_specs/FMT-05_alpha_editing_contract.md) | A 设置／添加内置 alpha；B 提取；C 移除 | 颜色逐位保留，零 alpha 不清除隐藏 straight 颜色；共享通道局部编辑 | Proposed；A 的三个 CPU profiles 为固定 ABI 8 keys，零 scratch 逐位复制，非空 view 仅依赖 schema 并失败；B/C helpers 可执行地组合现有 Result operations，逐位处理七种 dtype，不做样本域验证。`channel.literal_like_<profile>` 是固定 ABI 8、仅依赖 schema 的 opaque fill primitive；`channel.scalar_literal_<profile>` 为 FMT-03 的无输入 shape-[1] provider |
| [FMT-06 数值类型与区间转换](op_specs/FMT-06_numeric_conversion_contract.md) | 单 tensor Result→目标 dtype 的同坐标 `values` | 默认按 dtype 区间缩放，可显式关闭；完整 channel table 用 cell-relative axis | 单一 strict 固定 ABI 8 key；49 个转换对；完整输出 tile 与源网格闭包；独立 dense 输出、固定精确 scratch；reject 可表示性及整数目标有限源属于调用者前提；非空 view 失败；Proposed |
| [FMT-07 已退休](op_specs/FMT-07_retired.md) | 不再定义本族成员 | 数值编码约束归 FMT-06 后续成员；抖动／半色调归 GRD-29/30；opaque 补值规则归 FMT-05B | 保留编号，不新增运行时接口 |
| [FMT-08 描述编辑](op_specs/FMT-08_metadata_assignment_contract.md) | 显式 TensorDescription→新配置对象 | patch/replace；dependencies=error/cascade；missing=error/ignore | SDK 纯函数 edit_description；无 workflow key、样本读取或图节点；下游显式接收返回配置 |
| [FMT-09 transfer 编码／解码](op_specs/FMT-09_transfer_contract.md) | A 解码、B 编码；Float32/64 单 tensor Result，单 RGB/Gray 组或显式 raw components | 十类静态曲线；PQ/1886 为绝对 nits；按曲线明确 reference／定义域；cell-relative axis | 六个固定 ABI 8 CPU keys；完整输出 tile 闭包、固定数值 scratch、独立 dense 输出；语义样本域属于调用者前提；非空 view 失败；Proposed；无 GPU |
| [FMT-10 RGB 基底／XYZ／白点适应](op_specs/FMT-10_rgb_basis_contract.md) | A RGB→XYZ、B XYZ→RGB、C 适应；D 编译期 A→可选 C→B | 七预设＋自定义 xy；四种完全适应；显式 preserve_xyz／adapt；相对／绝对尺度不隐式换算；Result 单 tensor inputs/outputs | 九个固定 ABI 8 CPU keys 注册 A/B/C；D 保留逐节点舍入；保留输出 tile 的非 identity 参与行映射全部三分量，完整 I 与旁路映射对应样本，再按源网格取闭包；固定 scratch、独立 dense 输出；语义有限性为调用者前提；非空 view 失败；Proposed；无 GPU |
| [FMT-11 颜色模型转换族](op_specs/FMT-11_model_conversion_contract.md) | A～P 八组方向对；Q/R Gray 提取／中性重建；S/T 二值化／两级展开 | 原生浮点、精确分量依赖；Lab/LCh 使用 l=L*/100；NCL YCbCr，显式路径 | 澄清完成，A-R/T 共 57 个 strict/Apple/x86 CPU keys，S helper 降低为 MASK threshold；固定 ABI 8；Float32/64；完整输出 tile 决定源网格闭包，auto/materialize 独立 dense 输出，纯复制／常量零 scratch；语义数值有效性为调用者前提；Q 非空 view 仅依赖 schema 并失败，其他成员静态拒绝 view；Proposed |
| [FMT-12 ICC 转换族](op_specs/FMT-12_icc_transform_contract.md) | A 双 profile；B DeviceLink；C 显式有序 profile 链 | Little CMS 2.19.1 CPU；ICC v2/v4；optimized 默认，reference 显式 | 三个固定 ABI 8 CPU keys 已实现；五种端点模型、普通/DeviceLink/有序链、显式单位与 intent/BPC；构建同步不可取消，保留内存/工作预算，像素执行可取消；Proposed 决策状态保留 |
| [FMT-13 OCIO 转换族](op_specs/FMT-13_ocio_transform_contract.md) | A 空间；B display/view；C Looks；D NamedTransform；E 文件；F 变换树 | OCIO 2.5.2 CPU；optimized 默认，reference 显式；资源／属性冻结 | 六个固定 ABI 8 CPU keys 已实现；闭合配置/上下文/文件、动态属性冻结、实际路径检查；Float64 显式 RN32 后精确扩宽，alpha/AOV 逐位直通；同步不可取消构建，保留预算与像素取消 |
| [FMT-14 色域映射](op_specs/FMT-14_gamut_mapping_contract.md) | A 分量裁剪；B OKLab 降色度；C 线性 RGB knee 压缩；D 越界 mask | 显式目标线性 RGB 0..1；固定算法与迭代次数，无隐式 tone/transfer | 四个 strict CPU keys 已实现，固定 ABI 8；Float32/64、精确组件映射与源 tile 闭包、有限域检查；Proposed 决策状态保留 |
| [FMT-15 tone/view](op_specs/FMT-15_tone_view_contract.md) | A Reinhard 亮度映射；B 相对显示值与 nits 转换；C 显式 view 组合 | 曝光、白锚点和黑白亮度明确；阶段顺序由调用方列出 | A/B strict CPU 与 native/configured C helper 已实现；资源感知重载验证真实 OCIO 阶段与最终端点；保留逐节点舍入、失败及事务式图修改 |
| [FMT-16 已退休](op_specs/FMT-16_retired.md) | 外部色度子采样／重建归 input/output codec | 内部 Y/Cb/Cr 始终同尺寸 1:1:1 | 不再提议内核 chroma_resample 算子；RGB↔YCbCr 数学保留 FMT-11 |
| [FMT-17 已退休](op_specs/FMT-17_retired.md) | 外部布局与打包归 input/output codec | 内核内全部图像遵守 planar | 不再提议独立 image.layout_convert 算子；分配／访问遵守 kernel，逻辑轴操作归 NUM／通道族 |
| [FMT-18 软打样与警告](op_specs/FMT-18_softproof_contract.md) | A proof 颜色；B 独立 ICC gamut alarm mask | 固定 LCMS、intent/BPC；mask 读取独立 pipeline，并要求源域维度修正 | 两个固定 ABI 8 CPU keys 已实现；Gray 49、RGB 49³、CMYK 23⁴ 源域建表；独立 pipeline 生成精确 0/1 mask；构建同步不可取消且受预算约束；设备告警不同于 FMT-14 几何边界 |

OpenImageIO官方提供通道重排、premult/unpremult、色彩变换等独立操作，支持这种职责拆分；Photospider的端口与默认值是本规格建议。[^oiio] ICC与OCIO分别采用其公开语义和资源配置，不能只以“色域名字”替代完整输入。[^icc][^ocio]

## 模型与支持面的精确定义

原目录仅列模型名称，转换覆盖不完整。现已确认按“基础转换对＋显式组合路径”
补齐全部所列模型，见[模型转换覆盖表](op_specs/FMT_model_conversion_coverage.md)。
ICC／OCIO 分别由 FMT-12／13 定义独立引擎契约，FMT-14/15/18 已分别规定映射、渲染与软打样，不增加统一自动转换入口。
[通用相对坐标尺度](op_specs/FMT_relative_coordinate_scale.md)规定 Lab/LCh 存储 l=L*/100，
允许有限负值／HDR；NUM/CRV 的描述、LUT 轴／表、参考数据与消费者已完成该尺度迁移。
RGB ramp 输入输出为 straight；烘焙内部表／报告为 v4，配置摘要域为 v2，旧格式明确拒绝。

| 名称 | 语义和必要信息 | 首期建议 |
| --- | --- | --- |
| RGB/RGBA | RGB原色、白点、transfer及alpha关联；RGBA的A不是颜色维度 | 依模型／空间描述明确支持范围；按各成员当前实现范围使用 |
| Gray | 线性Y、编码luma Y′、Lab l=L*/100、OKLab L 与任意标量分量须区分 | FMT-11 显式灰度化与中性重建；通道提取不等同于灰度转换 |
| Black/White | 二值颜色与 coverage mask 语义分开；阈值／抖动／半色调需显式选择 | 明确二值描述和黑白展开；FMT-06 数值编码，codec 负责 1-bit 打包 |
| XYZ / xyY | XYZ白点/归一化；xyY在XYZ总和或y=0时的策略 | signed Float32/64，黑点显式处理 |
| CIELAB / LCh(ab) | 参考白点；存储 l=L*/100，名义0..1并允许有限扩展；a*/b*、C*尺度不变 | D50建议用于ICC连接，其他白点必须声明 |
| OKLab / OKLCh | 使用其规定的转换；OKLab L通常0..1尺度 | 按明确版本矩阵，signed cbrt，不能用Lab尺度套用 |
| HSL / HSV(HSB) | 对哪一种RGB数值计算；hue无色时无定义，L不是物理亮度 | 明确线性／编码 RGB 描述；灰色 H/S=+0；HSB 为 HSV 编写别名 |
| YCbCr | RGB′的系数矩阵、量化range、chroma siting/sampling | BT.601/709/2020等各用命名模式；YCbCr与YUV不任意互称 |
| CMYK | 印刷条件、纸白、黑生成与总墨量通过profile决定 | ICC-only首版；没有profile就拒绝“真实颜色转换” |
| ACES | ACES2065-1/ACEScg 原色空间、ACEScc/ACEScct 编码和完整渲染流程分开 | FMT-10 原色，FMT-09 编码；FMT-13 使用明确版本的 config／builtin，FMT-15 提供显式原生阶段组合，不冒用 ACES 渲染名称 |

颜色空间参考采用CSS Color 4对Lab/OKLab/极坐标和颜色转换的公开描述；CSS使用语境不自动规定Photospider的HDR工作空间。[^css] BT.2100的PQ/HLG、量化与显示参数必须使用指定版本的约定，其标准量化rounding不能被通用cast的ties-even默认覆盖。[^itu]

## 数学核心

`a≤0.04045` 时sRGB解码为 `a/12.92`，其他非负值为 `((a+0.055)/1.055)^2.4`。RGB ramp 采用对绝对值运算后恢复符号的扩展，见其具体规格。RGB原色转换使用线性值，经XYZ及所选白点适应，再到目标线性RGB。对编码RGB直接乘原色矩阵会得到不同结果。

Lab转换中 `f(t)=cbrt(t)` 当 `t>(6/29)^3`，否则 `t/(3(6/29)^2)+4/29`；标准量 `L*=116f(Y/Yn)-16`，原生存储 `l=L*/100`、`a*=500(f(X/Xn)-f(Y/Yn))`、`b*=200(f(Y/Yn)-f(Z/Zn))`。LCh 的坐标转换使用 `C=hypot(a,b)`、弧度 `h=atan2(b,a)`，或显式输出 `atan2(b,a)/pi` 的 π 倍数；原点策略由对应转换规格定义。已有 LCh/HSL 输入的 hue 在 ramp/LUT 中保留原值与圈数，不做归一化或角度制转换。采样、白点与超范围策略影响结果，不能仅测试常见RGB颜色块。

## alpha 与 VFX 数据的边界

导入预乘数据时，零 alpha 非零颜色如何保留为独立 emission 必须由显式边界契约确定。[^exr]

相关颜色算子以显式 straight 描述作为输入约定，处理指定颜色组并按各自规则复制内置 alpha。内核本身不推断或建立颜色解释。
预乘转换仅位于显式边界或算子内部；空间重采样、混合与合成分别规定内部
alpha 运算、零 alpha 输出及工作色彩空间。独立 emission 不自动参与 alpha 运算。

## 使用与验收

概念链路：`decode→explicit input_description/parameters→straight boundary adaptation if needed→transfer decode→primaries/profile transform→grade→explicit gamut/tone map→transfer encode→numeric format encode→host export`。若decoder已经输出线性camera RGB或已应用WB，必须从相应阶段进入，避免重复处理。

解析检查：identity matrix/通道重排往返；黑白与参考白；直通alpha位模式；0,.04045,1等分段点；灰轴中性；合法signed/HDR；整数上下限和halfway rounding；codec 侧另验收 4:2:0 奇数尺寸的 chroma 位置。profile/OCIO以固定资源和已知色块检查数值或ΔE，精度目标在该转换的实现规格中定义。CPU/GPU对比不能掩盖CMM算法差别。

## 来源

仅用于API职责示例，不据此宣称稳定版号。
[^icc]: International Color Consortium，[*ICC.1:2022 Profile Specification*](https://www.color.org/specification/ICC.1-2022-05.pdf)，2022-05；profile、PCS与rendering intent。
[^ocio]: Academy Software Foundation，[*OpenColorIO Documentation*](https://opencolorio.readthedocs.io/en/latest/)，滚动文档；配置、processor与display/view体系。
[^css]: W3C，[*CSS Color Module Level 4*](https://www.w3.org/TR/css-color-4/)，滚动规范草案；Lab、OKLab、色彩转换与色域概念。
[^itu]: ITU-R，[*BT.2100-3: Image parameter values for high dynamic range television*](https://www.itu.int/rec/R-REC-BT.2100)，2025；指定版本的PQ/HLG与量化要求。
[^exr]: OpenEXR，[*Technical Introduction: Premultiplied vs. Un-Premultiplied Color Channels*](https://openexr.com/en/latest/TechnicalIntroduction.html#premultiplied-vs-un-premultiplied-color-channels)，滚动官方文档；零alpha发光颜色。

Softproof 与 gamut alarm 以 [FMT-18 固定源码及修正规则](op_specs/FMT-18_softproof_contract.md)为准，继承 FMT-12 的 LCMS 2.19.1 引擎边界。

## FMT-02 固定组装与输出所有权

FMT-02A/B/C 使用固定 ABI 8。每个完整选中输出 tile 生成一个 CPU Host task，按照静态映射逐端口计算源 tile 并集，不用一个包围矩形跨越未使用的源区间。未参与当前输出 tile 的端口仅保留 schema 依赖。空间或 atomic 网格可能使输出 tile 包含多个通道，源网格也可能在同一输入 tile 中覆盖额外样本。

Auto/materialize 将原始位模式复制到独立 dense 输出，零 scratch；非空 view task 只依赖 schema 并返回 InvalidArgument/ViewUnavailable。Empty 保留静态校验但无 payload task。准备阶段以实际 slots 数量和有限 work 预算限制预期闭包；运行时不生成依赖或发布源别名。FMT-03 swizzle/replace 与 FMT-05C 的带通道分支继承这些规则，scalar literal 使用独立的零输入固定 task。详见 [FMT-02 族契约](op_specs/FMT-02_channel_assembly_contract.md)。

## 同尺寸平面与 codec 边界

已确认 DAG 内 Y/Cb/Cr 等颜色平面始终同尺寸、同采样网格，保持 1:1:1 的平面尺寸关系，
即 full-resolution 4:4:4。alpha 仍为同张量内的同尺寸平面。由宿主分配的图像 backing 默认采用 tile 内 planar 布局；符合授权及地址约束的外部 affine/signed-stride 视图仍可接入。显式颜色描述和 raw 参数不负责改变图像的采样拓扑。

输入 codec 负责外部子采样重建与布局解包，输出 codec 负责显式选定的子采样、打包和压缩。
RGB↔YCbCr 数学转换仍归 FMT-11；不再需要在 DAG 内引入异尺寸颜色平面 carrier。
codec API、滤波、siting 和边缘规则另行定义，见[边界契约](op_specs/FMT_codec_boundary.md)。
