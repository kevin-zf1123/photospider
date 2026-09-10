# ADR 0020：可组合数值、语义与输出基础

- 状态：Accepted；实现由 #289–#297 待交付
- 日期：2026-09-10
- 接受记录：维护者批准 G1/G2/G3/G5 实现计划，包括替换 image v1 和迁移全部八个已有算子。
- 基线：`main@fba06270a44b24de7dcf12ac82624c4a56e0fa67`，package 0.6.0、operation ABI/OperationTraits 6
- 追踪：[#287](https://github.com/kevin-zf1123/photospider/issues/287)
- 权威来源：[英文 ADR](../0020-composable-operation-foundations.md)

## 决策与边界

保持现有 Value/storage 模型、UInt8/Int64/Float64/Float32、C++17、每节点单个 `value`
输出、非零 rank 1..8 shape 和不可变的每 Run binding。通过同一共享实现，为编译器、
直接 registry 调用及可信纯 C operation ABI 增加可复用语义描述、静态输出推断和计算标量组合。

本决策替代 ADR 0016 的 image v1、RGB 非负和 bounded scalar 仅可直接绑定条款，
以及 ADR 0019 的 package/operation/trait 目标版本。ADR 0015 的 ownership、
ADR 0017 的 storage/Region/budget、ADR 0018 的 snapshot/cache/frozen ownership
继续适用。已有区域图像算子保留 Region 规则；新增 shape 变化、LUT 和全局算子首版
使用 Whole，不引入 G4 逐端口空间依赖协议。

G6 宿主资产、daemon 迁移、完整路径、FFT、ICC/OCIO 集成及其余算子目录不属于本次交付。
`built-in_ops` 研究目录中只有本文固定的子集被接受，其余保持 Proposed。ADR 接受目标，
不表示基线 registry 已提供新 API。

## G1：数值规则

主要计算与存储使用 Float32；系数、表达式求值、参考运算和确定性累加使用 Float64。
Generic Value 保留既有合法浮点位模式；finite/range 由语义描述和算子约束，不加入 Value 本身。

`numeric.cast` 只改变 dtype。`numeric.encode_range` 将 `src_min < src_max` 映射到
`dst_min < dst_max` 后转换 dtype。两者支持现有四种 dtype，整数舍入默认 ties-even，
溢出默认拒绝。`overflow="clip"` 显式夹紧有限值；整数输出或 range encoding 拒绝
NaN/infinity。浮点收窄拒绝有限值溢出，浮点 cast 保留所支持的非有限值。
Float→Int64 在转换前检查舍入后的值，不能把 INT64_MAX 转成已向上舍入的 binary64
作为上界；Int64 恒等/转换不进行不必要的 double 中转。
Range 采用 Float64 仿射运算（就近端点锚定、无误差高低位差及补偿乘加）、显式 fused multiply-add 和 nearest-even 舍入。
无法表示的仿射系数或超过输出 Float64 一个 ULP 的未解决商/累加误差显式失败；有限源值的求值结果超出目标 dtype 时遵循 reject/clip，
包含最终浮点溢出。Clip 不接受原本非有限的源值。本次 dither 固定关闭。

不增加隐式 clamp、cast、broadcast、gamma 操作或 epsilon。基础算术输入为同 dtype、
同 shape 的 finite Float32/Float64；除零和非有限计算结果失败。归约按逻辑 row-major
固定顺序用 Float64 累加；population variance 使用固定两遍 mean/deviation 算法，
`ddof=0`。CPU 容差逐算子规定；未指定 exact oracle 时初始有限值参考门槛为
`atol=1e-6, rtol=1e-5`。

## G2：typed semantics 与 image v2

新增公开 owned `SemanticDescriptor` 及编解码、对 ValueDescriptor/区域样本验证的 helper。
描述封闭 kind（scalar、image、mask、scalar field、vector field、complex field、
sampled signal、LUT、byte resource）、有序通道名/角色/单位、颜色模型、原色、参考白、
transfer、scene/display reference、总体单位、alpha association、坐标空间/方向及
均匀采样起点/步长及轴单位。缺少语义的 generic Value 仍合法，不能只靠 shape 恰好匹配满足 typed port。
Mask 区分 coverage/probability/membership；signed field 不使用 bounded mask kind。
Complex field 使用显式 real/imaginary 分量轴、完整未 shift 频谱/DC0、负号无归一化
forward 和 inverse /N；coordinate_space/direction 固定为 `frequency_unshifted` /
`forward_negative_inverse_1n`。Vector 坐标为 `pixel_displacement`、
`normalized_displacement`、`pixel_position` 或 `normalized_position`，方向为
forward/inverse。不新增 complex dtype 或 FFT runtime。
Byte resource 是带有界 media-type 标签的 UInt8 `[N]`，不包含装载 API。

Image 继续使用 `photospider.image` key，版本升为 **2**；其他 typed kind 使用
`photospider.semantic` **1**。一个 typed Value 至多拥有其中一个 semantic facet。
从活动接口移除 image-v1 载荷、旧 profile 常量、port kind 和 reader，不保留适配器或别名。
无关 opaque generic facet 保留现有 Value 契约。

Canonical payload 为与 native struct layout 无关的有序二进制记录：kind；通道数；
各通道的 name、role、unit；model；primaries；white X/Y/Z；transfer；reference；
unit；association；coordinate space；direction；sample origin/step；sample axis unit；media type。
字符串用 uint32 little-endian 字节数加无 NUL 的 strict UTF-8，数值用 IEEE-754
binary64 little-endian bits，通道数用 uint32 little-endian。Kind 和封闭词汇编码为
所记录的小写字符串。不适用字符串为空，数值为正零。元数据数值必须 finite；适用的白点
分量为正且 Y=1，采样步长为正。语义元数据零规范化为正零，样本保留 signed zero。
Typed payload 上限 4096 bytes，有通道时数量 1..64，各文本字段上限 128 字节。拒绝未知封闭词汇、
不适用的非空字段、重复、非法组合和尾部字节。这些上限不放宽已有 Value facet 限制。
Canonical payload 由公开 helper 统一生成；C table 使用相同数据的 copied pointer/count
record，并接受精确 ABI 校验。
公开 typed helper 将 descriptor 转为/解析静态 `semantic` String 参数，内容为 canonical
小写十六进制 payload bytes，最多 8192 字符。`color.assign` 和 `channel.merge` 使用
该参数；workflow 作者构造 typed descriptor 并调用 helper，不需手写十六进制。
保持 WorkflowDocument schema 2 和现有 String 上限。格式错误/非 canonical hex、
非法语义组合或超限记录在 callback/IR 发布前以 `InvalidArgument` 拒绝。
Generic opaque Value facet 上限保持。

Image 是 Float32 `[H,W,C]`，有序通道数与 C 一致。首工作空间为 linear sRGB/Rec.709
RGB、D65、scene-referred relative units；finite signed/HDR RGB 合法。RGB、XYZ、Lab
分别描述 model、通道角色和白点；Lab L* 与 a*/b* 使用各自单位，不能靠通道名猜测颜色。
参考白是 Y=1 的 XYZ；具名 D65 便捷构造在全部调用方固定为同一白点。

Alpha association 为 none、straight 或 coverage-premultiplied。存在时 alpha 为最后
一个 coverage 通道，范围 `[0,1]`。Coverage-premultiplied 在 alpha=0 时要求 RGB=0；
straight 允许隐藏颜色。Unassociate 对任何正 alpha 直接相除，不加 epsilon；alpha=0
时返回 RGB=0，溢出则拒绝。Associate 在 alpha=0 时按定义丢失 straight 隐藏颜色。
非线性颜色转换接受 unassociated color，调用者显式组合 unassociate/convert/associate。
XYZ/Lab 使用同一声明白点，linear-sRGB↔XYZ 使用 D65；D50 XYZ↔Lab 需显式白点。
不隐式执行 chromatic adaptation 或 gamut clamp。

端口约束描述接受的 dtype/shape、可选 typed kind/descriptor 和标量 bounds。
输出语义显式 preserve、establish、transform 或 drop。Preserve 复制真实输入 facets，
establish/transform 验证结果描述，drop 删除无法保证含义的语义。Typed output 进入
IR/plan，计算后验证，经过缓存保留原值。Generic 数值算术移除语义保证；通道提取输出
带原角色/单位的 field 或 coverage mask。Merge 显式建立声明的目标 image 描述，输入允许 generic HW、scalar field 或
coverage mask；已知角色/单位必须匹配对应目标通道。Generic 输入仅通过显式目标和样本
验证取得含义，因此支持 extract、generic arithmetic、merge 组合而不保留失效保证。Assign 只改变解释，发布前验证目标全部约束及样本。

以下八算子完整迁移 C++、C 模块、Metal、fixture 和示例：`image.exposure_gain`、
`image.opacity`、`image.gaussian_blur`、`image.mask`、`image.source_over`、
`image.downsample_box`、`mask.downsample_box`、`image.brush_circle`。其图像端口
仍专用于 linear D65 coverage-premultiplied RGBA，mask 为 typed coverage mask。
既有边界和公式保持，brush RGB 改为允许 finite signed。新 RGB/Lab 表示由对应通道/颜色
算子消费。Metal eligibility 必须接受代表性 signed 输入和输出，禁止通过统一
`channel < 0` 回退。Subnormal/overflow 保留 ADR 0019 的显式数值回退和真实 dispatch 验收。

## G3：静态描述推断

端口可声明精确 `element_type` 或 `element_type_mask`。Mask 低四位按 element code-1
对应 UInt8、Int64、Float64、Float32，零表示无额外限制。非零精确类型与 mask 互斥，
未知位在注册时拒绝。相同的 sized C constraint 字段经过复制、验证并进入 compiler/result
identity。尚未发布的 ABI/Traits 7 目标包含该字段；浮点数值端口使用 mask 12，二元算子
使用固定两个成员的同构组强制 dtype/shape 相等。

Output dtype 与 shape 独立。新增 dtype rule，选择声明 dtype、输入 dtype 或已验证的
静态 dtype 参数。Preserve/match shape rule 独立于 dtype 比较 shape。显式输出各轴
选择正整数常量、正静态 Int64 参数、输入轴或实际输入数量。四种来源均有非负 constant
偏移（默认零），使用 checked addition；组件 capacity+1 使用该字段。
按用途保留 Scalar/Fixed/Shrink；
共同 helper 在 callback 前一次性求解 descriptor/facets，结果再验证。运行期样本不决定 shape。

Registry 算子可有固定前缀和一个尾部同构重复输入组，显式声明最少/最多数量（启用时 minimum>=1），总输入不超过
1024；`channel.merge` 使用 1..64 个同 H/W、同 dtype 的 HW 输入；同构组约束 shape/dtype，
组员允许 generic value、scalar field 或 coverage mask。
Semantic lowering 将模板展开为精确有序输入表；直接调用执行相同 count/schema 展开。
Descriptor 与 C ABI 区分模板和已解析表。Shape 引用须指向实际存在的输入轴；
重复数量、静态轴参数及 checked allocation product 不合法时在 callback/IR 发布前失败。

闭集 `OperationSemanticRule` 也包含通道提取、选择、合并、alpha 关联变化和 RGB/XYZ/Lab
变换，均为与 operation key/callback 无关的共享描述规则。`MergeChannelsParameter`
接受 dtype/shape 合法的 Image/VectorField/ComplexField HWC 目标；已知 HW 源的
role/unit 逐通道匹配，name 不必相同。图像 alpha 提取使用 canonical coverage，
可直接组合既有 exact mask 端口。

`channel_indices_parameter` / `channel_indices_from_parameter` 编解码 canonical
逗号分隔十进制 String：1..64 项，各项 0..63，无空格/符号/前导零，最多 191 字节。
`IndexListCount` 通过同一 parser 扩展 extent 来源，该 String 同时驱动 swizzle 元数据。
完整合法角色的选择保留变换后的 typed 语义；重复/缺失角色、alpha 不在末位或其他
无法表示的选择输出 generic。去除 straight alpha 可保留 unassociated Image；
去除 premul alpha 必须移除 image facet，因为 RGB 仍被缩放。颜色转换按 role 读取
任意合法顺序，规范化全部输出通道名（含 alpha `A`），保持 alpha 样本 bits。
这些规则属于尚未发布的 ABI/Traits 7 词汇及既有 identity；WorkflowDocument schema 2、
provider ABI 1 保持。

闭集 `SampleExpression`/`ApplyLut1d` 在编译器、直接调用及 C 声明中共享有界 parser
与均匀域校验。前者输入 generic Float64 `[K]` (1..256)，要求有限 start、有限正 step，
验证已解析 Float32 `[count]` (1..1048576)，输出值/轴单位 dimensionless；多样本端点
有限且大于 start。后者接受 SampledSignal query 及 N>=2 的 SampledSignal/Lut 表，
query 值单位匹配 table 轴单位，输出 Drop。两者 Whole，见
[表达式与 LUT](../../kernel-architecture/zh/Expression-and-LUT-Operations.zh.md)。

## G5：计算标量与可复用算子子集

Generic Float32 `{1}` 上游可以连接 bounded scalar。编译检查 dtype/shape 和已知语义。
Bounded scalar 接受无 facet generic、dimensionless scalar 或完整 `{1}` coverage 的
dimensionless 单样本 signal。Dimensionless 指样本值单位，采样轴 domain/单位独立保留。
拒绝 image、mask 和带单位 field 语义。Declaration preflight 仍在任何 callback 前检查全部直接消费端区间。每次消费前验证
计算值 finite/range，包括 cache hit。直接 binding 数值错误为 `InvalidArgument`，
非法计算值为 `OperationFailed`，元数据不匹配为 `TypeMismatch`。Cancellation/currentness、
callback 前拒绝和不发布部分结果保持现有优先级。Gain `[0,16]`、opacity `[0,1]` 保持。
每 Run binding 与可变结果不进入共享 plan/registry。

以下为首版最小命名接口。默认值由 workflow 构造端显式提交，registry 不填默认。
所有新增算子首版使用 Whole。

| 算子族 | 首次交付固定行为 |
| --- | --- |
| `numeric.cast`、`numeric.encode_range` | 必填目标 dtype；rounding 为 `ties_even`，overflow 为 `reject` 或显式 `clip`；range 两端区间必填，dither off；同 shape、generic 输出。 |
| `numeric.add/subtract/multiply/divide`、`numeric.clamp` | 同 dtype/shape Float32/64；clamp finite inclusive min/max，min<=max；generic 输出，无 broadcast。 |
| `numeric.mean`、`numeric.variance` | 全样本归约为 generic Float64 `{1}`；population variance、固定 row-major 两遍累加。 |
| `channel.extract/merge/swizzle` | Extract 必填从零开始的 index；merge 从 generic HW/field/coverage 输入显式建立目标 descriptor，验证已知角色/单位和目标样本；swizzle 显式有界 index list，可重复，推断 C 并转换角色，无隐式常量/resize。 |
| `alpha.associate/unassociate`、`color.assign` | Association 仅改变颜色通道及描述，保持 alpha bits；assign 显式目标 descriptor，保持样本 bytes 并验证目标域。 |
| `color.rgb_to_xyz/xyz_to_rgb/xyz_to_lab/lab_to_xyz` | Float32 HWC RGB/XYZ/Lab，可带 straight alpha；binary64 系数/中间值、Float32 输出、alpha 不变；同声明白点，保留 signed/超色域值。 |
| `numeric.sample_expression` | 静态有界 expression、Float64 start/step、Int64 count>=1，step>0；Float32 `[count]`、sampled-signal 语义。动态 coefficients 为 Float64 `[K]`，K>=1，不改变每 Run shape。 |
| `lut.apply_1d` | Float32 sampled signal 加有均匀采样轴语义的 Float32 `[N]` SampledSignal/Lut 表，N>=2；线性插值，越域默认 `reject` 或显式 `clip`；保持 query shape，移除输入语义；不含 3D LUT 或跨通道耦合插值。 |
| `mask.threshold` | Finite Float32 HW scalar field 输出 typed coverage mask；显式 threshold .5，比较 `>=`。 |
| `mask.components`、`component.count/area/bbox` | Binary typed mask 输出 Int64 HW labels，之后用独立 count/area/bbox 节点；四连通，按 row-major 首像素编号 1..Kcap，背景 0；静态容量 Kcap>=1，超容量失败。Count Int64 `{1}` 只计前景；area Int64 `[Kcap+1]`；bbox Int64 `[Kcap+1,4]`，顺序 x_min,y_min,x_max_exclusive,y_max_exclusive；背景与未用记录为零。 |

各组件节点独立声明 capacity，使用既有 exact bounded Int64 `[1,2^53-1]`。
Labels 为 canonical ScalarField：Int64 HW、name/role `component_label`、整体/通道
单位 dimensionless，无 capacity facet。属性端口要求精确 facets 并逐值检查
`[0,capacity]`；允许编号空洞，count 是 distinct 非零 ID 数。较大声明容量的 producer
可连接较小 consumer，只要实际 ID 符合范围。容量约束包含桥接后的最终组件数。
Count 的受控去重 workspace 按输入样本数有界，不按 capacity 分配。
参见[组件算子](../../kernel-architecture/zh/Component-Operations.zh.md)。

表达式限十进制/科学计数数字（无 hex、NaN/infinity 名称）、x、`c[index]`、括号、一元 +/-、二元 +,-,*,/,^ 及纯函数
`abs`、`sqrt`、`exp`、`log`、`sin`、`cos`、`min`、`max`。幂右结合且优先于一元负号；`0^0=1`，每个子表达式必须有限。
UTF-8 源长度最多 4096 bytes、AST 最多 256 节点/深度 32、coefficients 最多 256、
count 最多 1,048,576；按声明表检查系数下标。Domain error/非有限结果带样本下标失败；
不支持循环、脚本、文件系统或隐式可变状态。验证与求值复用有界 parser，AST/参数进入
编译身份，系数 bytes 仅进入内容身份。所有循环/组件队列遵守 cooperative cancellation
和输出/scratch 资源计量。

## Snapshot、缓存与版本集成

InputSnapshot import/read/patch 接受所支持的 typed v2 image 描述，HWC C 取自
descriptor，不固定为四通道。Patch 保持完整 descriptor/facets、验证样本，C 比例 byte
offset 受溢出检查。Frozen execution 继续是内存 owned plan，保持 registry/snapshot
规则，不增加序列化 plan 格式。

Compiler identity 包含所有语义描述、输入输出约束、dtype/axis/semantic inference
rule、重复组上下界/解析数量和静态参数。Result-region key 还包含完整 resolved traits、
descriptor/facets、算子实现、精确输入内容/依赖、请求 coverage、numeric mode，以及适用的
backend/device identity。Bounds 或输出含义改变必须改变可复用 key；binding 顺序、
native pointer、time、allocation id 仍排除。

区域结果 key 也接受经过 preflight 的 dense、offset-zero 直接 Value：最多 2048 bytes，
派生 demand 必须覆盖完整 Whole 值。Snapshot、bounded-scalar、compact Whole Value 有
独立类别 tag；compact key 包含 dtype、rank/shape、精确 facet、byte 长度与原始 bytes，
包括负零和未使用系数。更大或局部直接输入无资格。该规则用于区域执行与 execute_stream；
纯 generic/scalar 的普通 execute 保持既有快速路径，未扩展 snapshot/disk 类型。公开
expression workflow 检查 2048/2049+ 边界、dtype/shape/facet 分离、并发系数及缓存非法
bounded 消费者拒绝。

Memory/native/disk entry 保留真实输出 facets；用预期输出语义验证存储元数据，在 hit
消费前验证数值约束，不重新构造旧固定 image facet。错误 disk format/checksum/metadata
作为 miss，不伪造 typed Value。仅发布已验证的 successful/current 结果，保持取消、
in-flight ownership、cache budget 和 GPU 不落磁盘规则。

目标：package **0.7.0**、operation ABI/OperationTraits **7**、semantic/physical-plan/
plan-cache domain v7、result-region-key v3。优化规则保持
`optimizer-v5-canonical-noop`，新 semantic input 改变其 digest。Result digest framing
保持 v2，因为已编码完整 facets/samples。磁盘派生数据升至 v2，旧 entry 作为 miss，
无旧 reader。Schema **2**、provider ABI **1**、C++17 保持。C++ consumer 重建；
拒绝 ABI 6、package 0.6 请求。Daemon 的 0.6 consumer 需要独立迁移。

## 交付与验收

从同步基线创建与 main 同起点的 `ops`、`ops-foundations`，在后者开发。唯一实现 PR
为 `ops-foundations` → `ops`，使用 merge commit、保留每开发 Issue 单独提交。
保留本地/远端 `ops`，验证交付后只删除 `ops-foundations`，不合入 main。顺序叶项：

| Issue | 完成边界 |
| --- | --- |
| #288 | 英文接受契约、中文镜像及目标/事实区分。 |
| #289 | Shared typed semantics、静态输出/重复输入推断、C/C++ ABI 及 identity。 |
| #290 | 全部八算子、C 模块、Metal、已有示例和回归迁移。 |
| #291 | Input snapshot、freeze、memory/native/disk cache 全部语义迁移。 |
| #292 | Computed scalar 正常/错误/cache/并发/cancelled 路径。 |
| #293 | 数值 cast/range/clamp/arithmetic/reduction 与 focused oracle。 |
| #294 | 通道、alpha 及显式参考白颜色组合。 |
| #295 | 有界 expression、动态 coefficients、linear 1D LUT。 |
| #296 | Threshold、稳定 labels、固定容量独立属性。 |
| #297 | 安装包公开 workflow 示例与组合验收。 |

`examples/foundations_workflow` 使用安装后的公开 WorkflowDocument/compile/execute，
给出命令和独立可检查输出。包括全部 256 个 UInt8 经 range encoding 往返、
halfway/overflow/non-finite 转换、负值下降 ramp、通道恒等/仅红通道缩放、合法 alpha
往返/零 alpha 隐藏颜色丢失/极小 alpha、signed/HDR 和 D65/D50 Lab 往返；
错误 unit/facet/association 必须失败。表达式 x^2 在 0,.5,1 得 [0,.25,1]，
linear LUT 在 .25 得 .125；不同静态 count 使用相同 operation key。动态系数按顺序/
并发复用同一计划并连接 gain，无 binding 混用；非法 fresh/cached scalar 不进入 callback。
无前景得到零 labels/count 与非空零属性表；覆盖对角连通、tile 边界组件、稳定编号和容量错误。

各实现切片执行 focused validation、修改 C/C++ 的 ClangFormat 21/cpplint，以及相关
隔离 static/shared consumer。八算子 CPU/C 模块/Metal 比较保留 ROI/tile/resource/
cancellation 回归；代表性 signed Metal 必须观察 native dispatch。全部叶项之后新建
独立全面审查，再推送、完成 PR 的 Linux/macOS static/shared/ASAN/TSAN 六项 CI、
Codex review bot 修复和复核、合入 ops、Issue 结算、清理分支。外部审查不可用时明确阻塞，
不报告验收完成。
