# 通道、数值格式与颜色管理

2026-09-23：package 0.20.0 移除旧 02-format-color 全部实现，以及历史放在
01-numeric 的 `numeric.cast`／`numeric.encode_range`。完整清单、保留的共享设施
和公开回归见[退休记录](op_specs/FMT_legacy_retirement.md)。FMT-01A/B 的 CPU 注册与 FMT-01C helper 已实现，运行方式见
[公开 workflow](../../kernel-architecture/Channel-and-Color-Operations.md#fmt-01-channel-extraction)；FMT-02 A/B/C 也已实现；其余新规格仍待实现。
历史 ADR 中的 typed Image/Layer 行为不再作为新 FMT 的实现契约。

[FMT-09～18 范围审查](op_specs/FMT-09-18_scope_review.md)记录已完成的成员设计及实现前置条件。
FMT-14/15/18 按维护者 2026-09-24 的委托完成设计；规格仍保持 Proposed。

状态 Proposed。已完成设计的 FMT-01～18 活跃成员以各自 D1 spec 为准，均不据此声称实现；FMT-07 已退休。ICC/OCIO 使用独立外部引擎契约，Float64 端口不代表完整 Float64 内部精度。FMT-01/FMT-02 使用新的 tensor-description v2；其余各族运行时实现、完整 metadata 迁移与外部引擎集成仍待完成。整数主要用于输入输出编码；跨通道 shape、资源输入与当前端口的限制见 G1..G5。

## 算子目录

2026-09-22 的分类级澄清见 [FMT 公共规格](op_specs/FMT_common_contract.md)。
已确认覆盖原 FMT-01～FMT-18 的功能并补齐缺口；其中 FMT-07、FMT-16、FMT-17 后续已退休并分配替代归属，
其余编号不变。目标采用通用张量／张量集合与可组合
metadata，移除 Image/Layer 特殊语义类型。算子按实际消费的语义校验，支持显式
raw／调用级 override；raw 保留适用描述但不继承样本有效性保证。
后续[内核存储规格](../../kernel-specs/Tensor-Storage-and-Region-Access.md)进一步要求
图像全部 planar、DAG 统一 tile 尺寸、边缘保留有效行并将行宽补齐到 tile 宽度、
下一 tile 起点强制页对齐，以及整图连续虚拟地址和
显式按页提供 backing。2026-09-23 进一步确认完整图像统一 straight，
alpha 为同张量内的独立平面；外部 alpha 可作为显式输入，但不保留跨张量关联。
该文件区分已确认结论、当前实现与待决问题；
已列成员的端口和算法见各自规格；未来扩展另列，尚未扩展当前运行时支持面。

CRV-06 已触发[通用颜色数组描述](op_specs/FMT-COLOR_color_array_contract.md)的配套澄清：
支持 Float32/Float64、rank 2～8、最后一轴为颜色通道，携带模型及适用的色域、白点、reference 和 transfer。
该历史描述被 NUM/CRV 使用；FMT 的任意轴、Gray/HSV/xyY 和通用 metadata 扩展仍待实现。

| ID / 提议操作 | 输入 → 输出 | 关键参数和建议默认 | 实现/支持与验收 |
| --- | --- | --- | --- |
| [FMT-01 通道提取族](op_specs/FMT-01_channel_extraction_contract.md) | 任意显式通道轴张量→单分量张量／独立输出引用 | A 静态索引；B 静态名称／角色；C 编译期拆分；keepdims=false，rank-1 要求 true | 精确区域请求；auto/view/materialize；保留分量解释；CPU A/B/C 已实现，使用 tensor-description v2；旧 `channel.extract` 已移除 |
| [FMT-02 通道组装与拼接族](op_specs/FMT-02_channel_assembly_contract.md) | A 单分量插轴组装；B 通道轴拼接；C 显式映射组装 | 非通道 shape 严格相同；A/B 顺序固定，C 源可复用；三者均支持目标通道／颜色组语义重解释且逐位复制 | CPU A/B/C 已实现，决策状态保持 Proposed；精确请求与失效映射；auto/view/materialize；参见公开 workflow 与性能结果 |
| [FMT-03 通道重排与替换族](op_specs/FMT-03_channel_editing_contract.md) | A 重排／子集／重复／常量槽位；B 定点替换，其他槽位直通 | A 语义随来源，B 保留目标语义；同 dtype 标量及有类型字面量；原输入同时取值 | CPU 已实现，规格仍 Proposed；公开编译期组合复用 FMT-02C，融合标量填充；精确请求与失效映射，auto/view/materialize |
| [FMT-04 alpha 边界适配族](op_specs/FMT-04_alpha_association_contract.md) | A straight 图像→预乘数值；B 预乘数值→straight 图像 | 语义适配输入输出均含内置 alpha，shape／通道位置不变；预乘结果仅用于显式边界 | 澄清完成，Proposed/未实现；保留 NUM 乘除与零 alpha 数学规则，不保留外部持久关联 |
| [FMT-05 alpha 编辑族](op_specs/FMT-05_alpha_editing_contract.md) | A 设置／添加内置 alpha；B 提取；C 移除 | 颜色逐位保留，零 alpha 不清除隐藏 straight 颜色；共享通道局部编辑 | 澄清完成，Proposed/未实现；A 浮点原生、B/C 保留 dtype 的编译期组合；auto/view/materialize，无外部持久关联 |
| [FMT-06 数值类型与区间转换](op_specs/FMT-06_numeric_conversion_contract.md) | A 张量→统一目标 dtype，shape／通道顺序不变 | 默认按 dtype 区间缩放，可显式关闭；支持逐通道区间；舍入后 reject/clip | A 澄清完成，Proposed/未实现；合法码值／有效位数约束分配给后续成员待澄清；静态恒等才可 view |
| [FMT-07 已退休](op_specs/FMT-07_retired.md) | 不再定义本族成员 | 数值编码约束归 FMT-06 后续成员；抖动／半色调归 GRD-29/30；opaque 补值规则归 FMT-05B | 保留编号，不新增运行时接口；旧 `numeric.encode_range` 已移除 |
| [FMT-08 语义描述赋予／移除](op_specs/FMT-08_metadata_assignment_contract.md) | A 原子赋予／重解释；B 删除，样本与逻辑结构不变 | patch 默认、replace 显式；依赖默认报错，可 cascade；missing 默认报错，可 ignore；auto/view/materialize | CPU 已实现；A 原生、B 编译期组合；v3 schema 与资源快照，不扫描样本，不替代数值／存储转换 |
| [FMT-09 transfer 编码／解码](op_specs/FMT-09_transfer_contract.md) | A 解码、B 编码；同 dtype／shape 的单 RGB/Gray 组 | 十类静态曲线；PQ/1886 为绝对 nits；其他 reference／定义域依曲线明确 | 澄清完成，Proposed/未实现；精确逐分量；alpha 直通；静态恒等才可 view；HLG 公布端点行为保留 |
| [FMT-10 RGB 基底／XYZ／白点适应](op_specs/FMT-10_rgb_basis_contract.md) | A RGB→XYZ、B XYZ→RGB、C 适应；D 编译期组合 | 七预设＋自定义 xy；四种完全适应；显式 preserve_xyz／adapt；相对／绝对尺度不隐式换算 | 澄清完成，Proposed/未实现；非恒等行读取三分量；原生精确 I 可复制；D 保留逐节点舍入与失败 |
| [FMT-11 颜色模型转换族](op_specs/FMT-11_model_conversion_contract.md) | A～P 八组方向对；Q/R Gray 提取／中性重建；S/T 二值化／两级展开 | 原生浮点、精确分量依赖；Lab/LCh 使用 l=L*/100；NCL YCbCr，显式路径 | 澄清完成，Proposed/未实现；19 个原生成员＋S 组合；CMYK 归 FMT-12，CL 留后续独立成员 |
| [FMT-12 ICC 转换族](op_specs/FMT-12_icc_transform_contract.md) | A 双 profile；B DeviceLink；C 显式有序 profile 链 | Little CMS 2.19.1 CPU；ICC v2/v4；optimized 默认，reference 显式 | 澄清完成，Proposed/未实现；浮点原生单位适配、profile 定义空间、实际 intent/BPC 路径明确 |
| [FMT-13 OCIO 转换族](op_specs/FMT-13_ocio_transform_contract.md) | A 空间；B display/view；C Looks；D NamedTransform；E 文件；F 变换树 | OCIO 2.5.2 CPU；optimized 默认，reference 显式；资源／属性冻结 | 澄清完成，Proposed/未实现；三分量、F32 引擎边界、精确区域；alpha 独立直通 |
| [FMT-14 色域映射](op_specs/FMT-14_gamut_mapping_contract.md) | A 分量裁剪；B OKLab 降色度；C 线性 RGB knee 压缩；D 越界 mask | 显式目标线性 RGB 0..1；固定算法与迭代次数，无隐式 tone/transfer | 设计完成，Proposed/未实现；原生 strict CPU，精确区域与分量依赖 |
| [FMT-15 tone/view](op_specs/FMT-15_tone_view_contract.md) | A Reinhard 亮度映射；B 相对显示值与 nits 转换；C 显式 view 组合 | 曝光、白锚点和黑白亮度明确；阶段顺序由调用方列出 | 设计完成，Proposed/未实现；无自动曝光统计，保留组合节点的舍入与失败 |
| [FMT-16 已退休](op_specs/FMT-16_retired.md) | 外部色度子采样／重建归 input/output codec | 内部 Y/Cb/Cr 始终同尺寸 1:1:1 | 不再提议内核 chroma_resample 算子；RGB↔YCbCr 数学保留 FMT-11 |
| [FMT-17 已退休](op_specs/FMT-17_retired.md) | 外部布局与打包归 input/output codec | 内核内全部图像遵守 planar | 不再提议独立 image.layout_convert 算子；分配／访问遵守 kernel，逻辑轴操作归 NUM／通道族 |
| [FMT-18 软打样与警告](op_specs/FMT-18_softproof_contract.md) | A proof 颜色；B 独立 ICC gamut alarm mask | 固定 LCMS、intent/BPC；mask 读取独立 pipeline，并要求源域维度修正 | 设计完成，Proposed/未实现；不通过报警色反推 mask；设备告警不同于 FMT-14 几何边界 |

OpenImageIO官方提供通道重排、premult/unpremult、色彩变换等独立操作，支持这种职责拆分；Photospider的端口与默认值是本规格建议。[^oiio] ICC与OCIO分别采用其公开语义和资源配置，不能只以“色域名字”替代完整输入。[^icc][^ocio]

## 模型与支持面的精确定义

原目录仅列模型名称，转换覆盖不完整。现已确认按“基础转换对＋显式组合路径”
补齐全部所列模型，见[模型转换覆盖表](op_specs/FMT_model_conversion_coverage.md)。
FMT-09/10/11 已完成本轮澄清；Gray 四种含义、HSB→HSV 编写别名和本轮原生模型路径已确定。
ICC／OCIO 分别由 FMT-12／13 定义独立引擎契约，FMT-14/15/18 已分别规定映射、渲染与软打样，不增加统一自动转换入口。
[通用相对坐标尺度](op_specs/FMT_relative_coordinate_scale.md)规定 Lab/LCh 存储 l=L*/100，
允许有限负值／HDR；既有 ColorArray v1 与 NUM/CRV 的语义迁移仍待实现。


| 名称 | 语义和必要信息 | 首期建议 |
| --- | --- | --- |
| RGB/RGBA | RGB原色、白点、transfer及alpha关联；RGBA的A不是颜色维度 | 依模型／空间描述明确支持范围；新 FMT 待实现 |
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

`a≤0.04045` 时sRGB解码为 `a/12.92`，其他非负值为 `((a+0.055)/1.055)^2.4`。新增 ColorArray 的 RGB ramp 采用对绝对值运算后恢复符号的扩展，见其具体规格；旧 typed 格式转换已移除。RGB原色转换使用线性值，经XYZ及所选白点适应，再到目标线性RGB。对编码RGB直接乘原色矩阵会得到不同结果。

Lab转换中 `f(t)=cbrt(t)` 当 `t>(6/29)^3`，否则 `t/(3(6/29)^2)+4/29`；标准量 `L*=116f(Y/Yn)-16`，原生存储 `l=L*/100`、`a*=500(f(X/Xn)-f(Y/Yn))`、`b*=200(f(Y/Yn)-f(Z/Zn))`。LCh 的坐标转换使用 `C=hypot(a,b)`、弧度 `h=atan2(b,a)`，或显式输出 `atan2(b,a)/pi` 的 π 倍数；原点策略由对应转换规格定义。已有 LCh/HSL 输入的 hue 在 ramp/LUT 中保留原值与圈数，不做归一化或角度制转换。采样、白点与超范围策略影响结果，不能仅测试常见RGB颜色块。

## alpha 与 VFX 数据的边界

历史 CoverageRGBA/Layer 的预乘约束不再决定新 FMT 的完整图像语义。导入预乘数据时，零 alpha 非零颜色如何保留为独立 emission 必须由显式边界契约确定。[^exr]

新规格中完整图像统一 straight，颜色转换处理颜色组并保留内置 alpha。
预乘转换仅位于显式边界或算子内部；空间重采样、混合与合成分别规定内部
alpha 运算、零 alpha 输出及工作色彩空间。独立 emission 不自动参与 alpha 运算。

## 使用与验收

概念链路：`decode→explicit source metadata→straight boundary adaptation if needed→transfer decode→primaries/profile transform→grade→explicit gamut/tone map→transfer encode→numeric format encode→host export`。若decoder已经输出线性camera RGB或已应用WB，必须从相应阶段进入，避免重复处理。

解析检查：identity matrix/通道重排往返；黑白与参考白；直通alpha位模式；0,.04045,1等分段点；灰轴中性；合法signed/HDR；整数上下限和halfway rounding；codec 侧另验收 4:2:0 奇数尺寸的 chroma 位置。profile/OCIO以固定资源和已知色块检查数值或ΔE，精度目标在该转换的实现规格中定义。CPU/GPU对比不能掩盖CMM算法差别。

## 来源

[^oiio]: OpenImageIO，[*ImageBufAlgo: Image Processing*](https://openimageio.readthedocs.io/en/latest/imagebufalgo.html)，滚动官方文档，访问2026-09-09。仅用于API职责示例，不据此宣称稳定版号。
[^icc]: International Color Consortium，[*ICC.1:2022 Profile Specification*](https://www.color.org/specification/ICC.1-2022-05.pdf)，2022-05；profile、PCS与rendering intent。
[^ocio]: Academy Software Foundation，[*OpenColorIO Documentation*](https://opencolorio.readthedocs.io/en/latest/)，滚动文档；配置、processor与display/view体系。
[^css]: W3C，[*CSS Color Module Level 4*](https://www.w3.org/TR/css-color-4/)，滚动规范草案；Lab、OKLab、色彩转换与色域概念。
[^itu]: ITU-R，[*BT.2100-3: Image parameter values for high dynamic range television*](https://www.itu.int/rec/R-REC-BT.2100)，2025；指定版本的PQ/HLG与量化要求。
[^exr]: OpenEXR，[*Technical Introduction: Premultiplied vs. Un-Premultiplied Color Channels*](https://openexr.com/en/latest/TechnicalIntroduction.html#premultiplied-vs-un-premultiplied-color-channels)，滚动官方文档；零alpha发光颜色。

Softproof 与 gamut alarm 以 [FMT-18 固定源码及修正规则](op_specs/FMT-18_softproof_contract.md)为准，继承 FMT-12 的 LCMS 2.19.1 引擎边界。

## 同尺寸平面与 codec 边界

已确认 DAG 内 Y/Cb/Cr 等颜色平面始终同尺寸、同采样网格，保持 1:1:1 的平面尺寸关系，
即 full-resolution 4:4:4。alpha 仍为同张量内的同尺寸平面。所有内核图像算子严格遵守
planar，不能通过 raw/override 变更存储约束。

输入 codec 负责外部子采样重建与布局解包，输出 codec 负责显式选定的子采样、打包和压缩。
RGB↔YCbCr 数学转换仍归 FMT-11；不再需要在 DAG 内引入异尺寸颜色平面 carrier。
codec API、滤波、siting 和边缘规则另行定义，见[边界契约](op_specs/FMT_codec_boundary.md)。
