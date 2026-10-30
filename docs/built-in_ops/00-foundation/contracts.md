# 公共数据与执行约定

本页定义各类别规格共同继承的数据、颜色、坐标、依赖、错误与验收约定。内核数据模型、执行、资源与生命周期以公开头文件和 [kernel 架构指南](../../kernel-architecture/README.md)为准，尤其是[数据模型](../../kernel-architecture/Data-Model.md)、[依赖采样](../../kernel-architecture/Dependency-Sampling.md)、[资源](../../kernel-architecture/Managed-Resources.md)和[全局结果](../../kernel-architecture/Global-Results.md)；成员规格只引用这些约定，不重复描述。

## Unchecked 执行边界

成员规格、族契约及 SDK helper 统一遵循 [Compiler and Execution](../../kernel-architecture/Compiler-and-Execution.md) 的 unchecked 契约。成员定义各自的数学输入域、输出构造和算法分支，并继承以下责任划分。

| 阶段 | 责任 |
| --- | --- |
| 编译、导入及静态 preparation | 校验 schema/version、成员目录、dtype、rank、固定 extent、轴与 shape 关系、参数存在性、类型与语法。 |
| 算子计算 | 调用方保证运行时 count、offset、index、ID、关联、padding、编码规范与数值/几何前提；生产者按规格构造完整输出，消费者直接使用这些前提。 |
| 独立 oracle 与验收程序 | 在参考工具中诊断非法请求，并检查合法输入的结果、容差和存储规则。此类诊断不构成生产执行的失败承诺。 |

静态参数的有限性、正值、顺序及几何可表示性也属于 unchecked 前提；决定静态结构的约束仍在准备阶段检查。违反 payload 或数值前提时，结果、诊断与恢复行为均无保证。生产者不在发布前扫描输出，消费者不重复验证 payload；将这些检查命名为质量认证不会改变其性质。

保留规格明确要求的数学分支、候选筛选、正确舍入细化、显式 clip/NaN 策略、质量测量与显式质量门控。资源耗尽、取消、设备失败及合法输入下定义的 NoSolution/NotConverged 等运行结果仍按各自契约处理。数据有效性、物理访问权限和算法精度是分别定义的责任。

## 数据对象与输出

| 概念对象 | 形状 | 规格需要说明的语义 |
| --- | --- | --- |
| scalar / sampled signal | Tensor `[1]` / `[N,C]`，单通道可约定 `[N]` | 自变量、间距/positions、端点、单位 |
| image / image plane | typed Float32 HWC / HW ImagePlane | 颜色模型、通道角色、transfer、primaries/white、reference、alpha；plane origin/step 与实际 source support 分开 |
| mask / scalar / vector field | HW coverage Float32 [0,1]；signed field 与 vector 按各端口契约 | coverage/probability/fuzzy 区别、单位、分量坐标；不将 depth/导数冒充 mask |
| ComplexField | typed Float32/Float64 Tensor `[H,W,2]`，Real/Imag 角色 | 这类 dense field 不自动具有 FFT 的原尺寸、packing 和变换 basis |
| Spectrum | Result 的 Float64 complex-pair records | 原 shape、axes/order、packing、sign/norm、shift、sampling、real policy；没有新增 complex dtype |
| Bands / PathSet / DynamicPoints / Components | 有版本 schema 的静态多 Tensor Result | 成员身份、拓扑、有效 count、ID basis、关联与 descriptor；静态容量与有效 count，空集合用 count=0 表达 |
| Layer / Response / WeightedSum | composite SDK 的值类型 | CoverageRGBA 与 emission、Q/T response、mass 与 weight 分开；不能只凭 shape 互换 |
| independent 1D LUT / coupled RGB 3D LUT | `[N,C]` / `[Nr,Ng,Nb,3]` 的逻辑设计 | 同轴逐通道与三轴耦合不同 |
| sequence / deep samples | 帧索引集合 / offsets+samples 的后续设计 | timestamp/timebase、缺帧、sample depth/opacity；专用 schema 尚未定义 |

静态命名多输出使用 `WorkflowNodeOutput{node, port}` 和`WorkflowOutput{name, node, port}`，各输出可有不同 shape、Region 和依赖。pure 算子的未请求端口不执行。所有端口都传递 Result，每个 Result 的 Tensor 成员名称、数量、dtype、rank 和正 extent 在 preparation 后固定。数据相关的数量使用静态容量与普通 payload 表达，例如 `records[N,R]` 和 `count:Int64[1]`，或同形状数据加 mask；有效数量可以为零，shape 仍保持正 extent。未使用位置按算子规则填充；要求完整结果的算子在容量不足时失败。


## 颜色、alpha 与 emission

普通图像算子默认采用 straight alpha，宿主 backing 默认 tiled planar。调用方不需要声明已遵循这些默认约定，内核不扫描认证，也不自动 associate/unassociate。以下 CoverageRGBA、Layer 等显式领域类型仍遵循自身 premultiplied 数学。

颜色模型、通道顺序、primaries/white、transfer、scene/display reference、亮度单位、alpha association、dtype 与量化范围分别定义。YCbCr 还需 matrix/range/subsampling/siting。颜色描述作为算子的显式静态参数传入；Result/Tensor 不附着或传播描述。描述编辑由 SDK 纯函数完成。RGB/XYZ/Lab、transfer 与 alpha 转换各自限定输入域；ICC/OCIO 资源封装不代表已实现完整 CMM、tone/gamut mapping 或任意外部 profile 转换。

CoverageRGBA 保存 premultiplied P 和 A，finite signed/HDR P 合法，A∈[0,1]，A=0⇒P=0。Layer 另外保存 finite signed emission E，允许 A=0、E≠0。这些类型和纯函数由 `Photospider::ops_sdk` 提供；调用者选择一致的线性颜色空间，SDK 不定义固定 space id，也不隐式转换颜色空间。颜色非线性处理一般使用 unassociate→transform→associate；emission 有独立含义，不能除以 coverage alpha。任意旧 image 算子不自动成为 Layer 算子。

已实现 Layer over 分别计算 `(Ps+Ts*Pb, As+Ts*Ab, Es+Ts*Eb)`，Ts=1−As，各指定 primitive 按 binary32 nearest ties-to-even、gradual underflow 和无 FMA contraction 舍入。Coverage opacity 缩放 P/A 并保留 E；front/behind emission 使用各自明确遮挡公式。Flatten 在显式 opaque 背景 B 上计算 `(P+E)+(1-A)*B`，返回 A=1 的 `CoveragePixel`。Response 只保存 Q=P+E、T=1−A，不能恢复原 P/A/E；两种代数的数值成功域不同，编译器不隐式互换。RawSum 的 alpha mass 与加权平均的 W 分开；weighted leaf/add 不选择归约树，调用者决定合并顺序；W=0 finalization返回 `OptionalLayerPixel` 的 valid=false。关联下溢不通过清零 P、改写 emission 或 epsilon 修补。见[Layer runtime](../../kernel-architecture/Layer-Runtime.md)。

## 坐标与依赖映射

数组按 y,x,c，几何向量按 x,y。默认设计采用像素中心 `(x+.5,y+.5)`、左上原点、y 向下；ImagePlane 的 nominal origin 是源像素索引坐标，必须按该 plane 契约解释。角度单位、方向、pixel aspect 和 window 逐算子定义。边界扩展作用于完整逻辑图像；constant/clamp/reflect/mirror/wrap 的支持以各节点为准。

| 简记 | 数学依赖 | 规格须说明的内容 |
| --- | --- | --- |
| E | 同像素/样本 | 输出样本与输入样本的 shape 对应；多输出分别定义 |
| H(r) | 有限邻域 | 有限邻域半径；参数 radius 不自动等于整个算法的支持半径 |
| W | 全局计算 | 输出依赖完整输入；ROI 结果与整图结果对应位置相同 |
| R | 任意源采样 | 合法地址的有限静态范围 |
| S | scan/reduce/递推 | 遍历顺序、初值与累加精度 |
| T | 多时间点 | 序列、时间窗与边界帧规则 |

数学支持集说明每个输出样本依赖哪些输入样本；数据相关地址由有限静态 superset 覆盖。ROI、tile 与整图计算在相同位置给出相同结果，ROI 与 tile 边缘不构成新的图像边界。

## 资源与生命周期

输出、scratch、资源准入、取消、Result 寿命与 Context 关闭由内核契约统一规定，见 [Managed resources](../../kernel-architecture/Managed-Resources.md)、[Global Results](../../kernel-architecture/Global-Results.md)和 [Compiler and Execution](../../kernel-architecture/Compiler-and-Execution.md)。成员规格不重复这些规则。外部 LUT、profile 与 model 由宿主载入不可变快照，路径字符串不代表内容身份。

## 错误与数值质量

成员规格定义数学前提、算法精度细化以及合法输入下的实际结果状态（例如 NoSolution、NotConverged、质量门失败）；unchecked 前提不增加数值域或 payload 违约诊断。诊断字符串不作为分支条件。失败传播、取消与已退休 Result 的有效性见 [Compiler and Execution](../../kernel-architecture/Compiler-and-Execution.md)。

ops SDK 的 `QualityReport` 区分 Measured 与 CertifiedBound。有限 residual 包括零 residual 都不自动成为误差证明。依赖 Exact、数值误差界、受管理容量是不同保证。见[Atom errors and quality](../../kernel-architecture/Atom-Errors-and-Quality.md)。

随机算子的 key/counter 布局、seed、全局坐标、channel/frame/stream 及浮点变换误差以 [NOI 随机契约](../03-generation/op_specs/NOI_random_contract.md)和各成员规格为准。[^random]

## 公共验收

成员规格的验收给出解析样本、期望值、不变量与容差。有限浮点的 `atol=1e-6, rtol=1e-5` 仅作没有专门规定时的候选；成员规定的精确舍入、逐位一致或特定容差优先。局部算法检查 whole、ROI 与跨 tile 位置的结果一致；集合输出检查静态容量下的零与非零 count 及关联。

[^random]: John K. Salmon 等，*Parallel Random Numbers: As Easy as 1, 2, 3*，SC11，2011；[作者与项目页](https://random123.com/)。Counter-based RNG 的研究参考；Photospider 的具体键布局由 NOI 规格定义。
