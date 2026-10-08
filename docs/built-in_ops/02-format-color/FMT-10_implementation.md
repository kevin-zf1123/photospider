# FMT-10 候选实现说明

本文件描述此次交接的实现，而非更改原始 op_specs 的数学契约或宣称上游验收完成。原始规格文件的 Proposed/not_implemented 前置字段保留，便于 Codex 按原始要求独立审查。

## 1. 覆盖范围和接口

| 成员 | 实现 |
|---|---|
| A | `color.rgb_to_xyz_{strict,accelerated_apple_silicon,accelerated_x86_64}` |
| B | `color.xyz_to_rgb_{strict,accelerated_apple_silicon,accelerated_x86_64}` |
| C | `color.adapt_xyz_white_{strict,accelerated_apple_silicon,accelerated_x86_64}` |
| D | C++ `ps::format::convert_linear_rgb`，事务式展开 A → 可选 C → B；没有 native D 注册项 |

头文件为 `photospider/format/rgb_basis.hpp`，也由 umbrella header 导出。四个同名 authoring helper 接受实际输入边、`RgbBasisOptions` 和可选注册表。它们沿真实输入边推导元数据，不相信另行传入的猜测；新增节点先在工作副本中静态校验，失败时不修改原图。已有/前向引用的节点 ID、导出 ID 均参与占用检查。D 的 override 仅作用于 A 的源解释，后续节点消费前一节点的实际输出描述。

七个 preset 名为 `srgb_rec709`、`display_p3`、`rec2020`、`adobe_rgb_1998`、`prophoto_rgb`、`aces_ap0`、`aces_ap1`。方法为 `xyz_scaling`、`bradford`、`cat02`、`cat16`。B 的 white policy 仅有 require_match/preserve_xyz；D 额外支持 adapt，必须显式选择方法。没有自动 transfer、曝光、色域裁剪或峰值亮度缩放。

`RgbBasis` 支持 preset 以及可选的一致性断言，或完整自定义 6 个 primary xy 和 2 个 white xy。公开 codec 将 preset 编码为名称，自定义编码为 `basis-v1:` 加 8 个以逗号分隔的 16 位小写 binary64 十六进制字；白点为 `xy-v1:` 加 2 个字。解析也接受白点 shorthand `d65`、`d50`、`aces`。作者 helper 会规范化几何零的符号；直接传入非规范字符串会失败，避免多种等价序列化污染 identity。

原生 ParameterValue 层面的 raw `components` 是例如 `"2,3,0"` 的规范三索引字符串，`axis` 是 Int64；推荐使用类型化 helper，避免手写 codec。几何/selector/mode/layout/profile 都是编译期静态选择，非适用字段会被拒绝。

## 2. 元数据与身份变换

使用已有 tensor-description v4，不新增其序列化版本。semantic 必须明确选择三角色完整组，RGB/XYZ 角色顺序独立于物理索引顺序；源与目标 dtype、形状、结构轴和通道数不变。raw 对存储浮点数运算，不伪造 RGB/XYZ 转换标签。legacy typed Image/ColorArray 必须先显式迁移。

本实现的原生单位字符串为 `1` / `cd/m2`；reference 为 `scene_relative`、`display_relative`、`absolute_display_cd_m2`，并接受已有生态中的 `scene` / `display`。绝对值要求 `absolute_display_cd_m2` 或 `display`，相对值不能标成绝对单位。XYZ 使用相同数值尺度，而非隐含 Y=100 归一化。单位字符串的上游统一约定是需要重点复核的接口决定。

非空 global/group/selected-channel 描述必须一致。profile/configured 坐标只有在提供明确、模型/角色顺序/单位一致的 analytic binding 时才被解释为本族的原生解析坐标；不会推断 ICC/OCIO 的等价矩阵，更不会运行 profile engine。变换后的选中组删除失效的 profile/configured/binding 标签，并将由 binding 单独提供的单位实体化。资源引用仍受宿主既有 owner 校验约束，静态描述不能制造资源 owner。

其他组、alpha、AOV 的数据按位旁路。与选中组重叠的另一组会被拒绝，避免变换后留下互相矛盾的别名解释；这是明确保守的 admission 边界。

只有原生矩阵**精确等于 I**才能走身份路径；C 的等白点仍先验证所选方法的所有白响应。raw identity 在非有限分类之前复制，保留 sNaN payload、符号和 -0。semantic identity 只验证被请求的选中样本；不检查未请求的另外两个坐标或旁路 alpha。D 即使同 basis 也不替换成 I，因为 A/B 的逐节点舍入和中间失败必须保留。

## 3. 精确算法与有证书候选

### 精确矩阵

复用内部 `exact_numeric.hpp` 的有界 Natural/Rational、ResourceVector 和工作计费。坐标从 binary64 位模式构造精确有理数，H 方法常数按规范十进制整数/分母构造。矩阵逆使用精确伴随式；乘除做交叉约分，加法使用约简的分母合并，减少中间位宽。

准备阶段构造并冻结 A/B/C 的 3×3 精确矩阵；每行保留三个带符号整数分子和公共分母，避免逐像素重新推导或反复拼接分母。主基底/白归一化/锥响应有效性均为精确判断，无 epsilon 或 condition-number 截断。

每个 finite 行将三个输入解码为整数 significand 与二进制指数，统一指数后累加精确乘积，最后仅舍入一次到原 dtype。`round_row` 先去除公共 2 因子，用位长和比较确定二进制指数，再只求最终 24/53 位 significand 所需的商位（最多 54 次比较/减法），由整数余数实施 ties-to-even。避免既有通用 bit-by-bit divide 按整个大分子位宽循环。极小/极大值、舍入进位、带符号 underflow 和有限到无限的溢出均有独立 oracle 覆盖。

raw 特殊值先按输入顺序传播首个 NaN（quiet bit，保留 payload/sign），然后处理零系数 × infinity、相反符号 infinity 等规定行为。非 identity 行即使某系数为零也消费/分类全部三项。精确零带隐含 +0 bias，结果为 +0；非零负数舍入下溢可得到 -0。

### 候选与接受条件

构建开关：

- `PHOTOSPIDER_FMT10_ALGORITHM=CERTIFIED`（默认）或 `EXACT`。
- `PHOTOSPIDER_FMT10_SIMD=ON`（默认）或 `OFF`。
- `PHOTOSPIDER_FMT10_EXACT_KERNEL=LIMB`（默认）或 `REFERENCE`。

CERTIFIED 预先保存每个系数的 RN64 近似和保守绝对误差上界。候选 DAG 固定为 `(a*x+b*y)+c*z`，不允许 FMA 收缩/fast-math/重关联。以乘积绝对值和、系数误差项和 underflow 余量构造绝对误差 E，并按 IEEE 位模式取相邻浮点值、向外扩展获得 `[lo,hi]`（值语义等同相应 nextafter）。`2^-48` 系数比 binary64 unit roundoff 大 32 倍，系数误差项乘二，另加 `2^-1020` 覆盖本短 DAG 的下溢误差。数学证明的完整性，特别是误差界自身舍入、次正规数和极端抵消，应独立审查；测试通过不替代证明。

Float32 只在两个端点 RN32 得到相同有限位模式时接受，因此 strict 也可使用快速候选而不放宽正确舍入。strict Float64 一律精确。accelerated Float64 仅在区间严格落于正常 Float32 表示范围，且端点位于同一 RN32 bin 时接受 double 候选；其他情况回退精确，包括参考为零、次正规或超出 Float32 范围的情形。此条件比单节点 NUM 的四 Float32-step 允许误差更保守。

候选采用有界 64 lane scratch。x86 profile 使用 AVX2 四个 double，Apple Silicon profile 使用 NEON 两个 double，尾部标量；strict 使用标量候选。没有全局 `-mavx2`，不会令 baseline 因宿主不支持 ISA 而非法执行。非有限位模式不会作为 sNaN 送入 SIMD。后端可用性复用既有 profile 检测，unsupported profile 显式失败，不静默改选。

FMT-10 没有超越函数，未改 SLEEF，也不需要在本族调用 SLEEF；其仍为整个项目现有依赖。

## 4. 调度、存储与资源

静态 support 只依赖三个选中位置：用至多七个通道区间建 dependency pieces，而非按张量元素数生成表。非 identity 颜色行依赖三个源坐标；semantic 额外带 Validation，旁路只依赖自身，所有请求保留 Descriptor tag。dirty mapping 是该映射的转置。

generic materialize 按输出通道组织至多 64 个空间点，保留 packed output offsets，支持非末轴和负步长源读取；三槽 fragment cursor 缓存最近的各输入片段。高碎片化时 miss 仍会线性搜索 fragments，未引入全局 atlas 或隐藏缓存，这是实机 profiling 要量化的成本。

planar 使用宿主 row-run 的真实连续长度，取输入和输出可用长度的最小值后批处理，不跨瓦片/行尾/ROI 边界越读。只写真实请求区域，不把未生产的 padding 当零。矩阵、scratch、依赖/输出 owner 沿用宿主资源与生命周期机制；调用中的工作、取消和当前性检查没有被 raw/identity 绕过。

为了使 semantic identity 在 planar 下也能合法保留 view，宿主新增一个可选、只读的 `PlanarMappedValidationCallback`。BitwiseMapped 仍要求单一数据源，允许其 Data|Validation，并允许附带 tag-only Descriptor 源；没有 validator 的带 Validation 映射在准备阶段拒绝。执行器在发布 alias **或 materialized copy 前**只对相应 exact input window 调用验证，且检查 prepared identity、取消和 currentness。此改动跨越 registry、contract 和 execution，需要比局部数学代码更优先审查。C++ OperationDefinition 布局/API 有扩展，必须整树重编译；没有更改 C operation ABI。

8192-bit Natural 上限是真正资源上限，而不是所有有限几何都必然成功的承诺。近奇异/极端指数可能耗尽工作或容量，应返回资源错误，不能静默截断。基准和压力测试显式提高自己的预算，**没有放宽项目默认预算**。

## 5. 测试与边界

Python stdlib Fraction + Gaussian elimination 独立生成 28 个矩阵（七 preset、两个可精确对角化 basis、近奇异自定义 basis，以及两对白点的四种方法），两种 dtype、每种 24 组三元组，合计 1,344 case / 4,032 个期望结果。最终 IEEE 舍入用整数商/余数，不经 Python float 近似。生成器支持 `--check`。

数学单测覆盖上述 4,032 个精确结果和固定 raw 特殊值 oracle；另外比较随机 Float32 候选与精确结果以及 SIMD/标量 DAG。集成测试通过真实 registry/compiler/executor 再核对 4,032 个结果，并检查 permuted roles、旁路、元数据、显式 binding、D 事务/逐阶段舍入/中间溢出、tile-cross ROI、身份 view、负步长、rank-one、Empty、错误字段、静态 dependency/dirty、fuel/cancellation、fenv 恢复。ASAN/已有算子回归/变体通过情况以实际日志为准。

未声称穷尽全部 binary32/binary64 输入、任意自定义几何或全部资源竞争。实际 ICC/OCIO engine、GPU、新 observer、非线性 transfer 不在本族范围。binding 的新增测试是静态元数据测试，不是用伪造 identity 绕过 owner 的执行测试。

当前 planar callback 的公开接口不提供 NumericDiagnostics reporter，所以其 numeric 统计的 0 代表未暴露；generic 的 strict_fallbacks 沿用既有含义，只对 accelerated → exact 计数。要分析 strict 内部 certificate miss 比例，需在 `evaluate`/`certify` 加临时非计时 instrumentation 或 profiler，而不是误读这些列。

D helper 不再把调用者 registry 的 opaque state 当作内部 Preparation。源白点由
内建私有准备结果获取（仍执行真实 metadata/override/analytic binding 规则），实际
A/C/B 节点继续由调用者 registry 准备。空 state、合法外来 state、失败时事务回滚有回归。
此选择增加一次 A 的静态几何推导；准备阶段成本与 execute 成本应分开报告。

mapped validator 和普通 planar callback 共用 worker、waiting admission、资源 Queue/Entries
租约、异常围栏及退休等待。一个 bounded mapped piece 集合只占一次回调队列项。
调用后包括普通异常和 bad_alloc 都按 Cancelled > Stale > 普通失败判定；回调引用在退休前
保持有效。图像操作通过 Result tensor publication、Need-authorized tensor windows，以及 affine view 或显式 materialization 路径访问和发布数据；相关行为入口见 [Tensor storage and region access](../../kernel-specs/Tensor-Storage-and-Region-Access.md)。View 与显式 materialize 路径分别有测试覆盖。

`RowUnsigned` 只初始化和复制实际有效的 32-bit limb 前缀，最多 256 limb（8192 bit）。
每行三个乘积、带符号累加和最终至多 54-bit 商除法使用固定栈暂存，不再为每次减法、
移位和临时 Rational 执行 ResourceVector 分配/resize。最终 RNE ties-to-even、特殊值
优先顺序、负下溢零和 +0 bias 不变。容量溢出显式抛 bad_alloc；近奇异几何仍可能耗尽
准备阶段 Natural 上限。乘/加/减和除法循环保留 Natural::work 与取消检查。
声明的 workspace 仍为 sizeof(Workspace)+64KiB；这不是 OS thread stack/RSS 的计量保证。
REFERENCE 可独立选择旧精确核；单测还在同一进程逐行对比新旧核。

`outward` 仅用于内部证书端点和系数误差界，不是通用 nextafter API：其 IEEE 结果值
一致，但不复制 libm 的 errno/浮点异常副作用。NaN 端点不被接受，零和无穷显式处理。
候选三乘二加 DAG、误差公式和 Float32 bin 接受条件保持原样；FP 环境仍由已有 guard
控制并恢复。测试用 100,000 个边界/伪随机 binary64 位模式对比标准 nextafter 的两方向
结果，并保留独立 Fraction oracle。有限测试不构成对完整证书数学完备性的形式证明。

planar 与 mapped validation 按 Float32/Float64 模板分开，使拷贝宽度编译期确定；
在一个真实 row-run 内消费多个 64-lane chunk，而不在每个 chunk 重查 row-run。
旁路通道直接复制连续 span，保留 alpha/sNaN/负零全部位模式。有限性分类每 lane 一次，
strict Float64/EXACT 不填充无用的浮点候选。没有跨 ROI/瓦片/通道越界读取，也没有融合 D。
SIMD 仍为原有 AVX2/Apple NEON 候选，运行时 ISA admission 未改变。

生成的 oracle fixture 仅排版变化，生成器同步生成规范排版；其全部 UINT64_C 输入与
预期字不变。仓库默认忽略规则明确放行 benchmark、说明、对照脚本及 oracle 生成器。
目标机仍须执行仓库指定 ClangFormat/cpplint 版本以及 ARM64 NEON 原生编译运行。

修复版已在 `ops-impl-FMT10` 工作区继续审核。共享 planar 调度入口补充每次
提交前的 root stage 扣账，及 admission/提交拒绝/提交异常后的统一
`Cancelled > Stale > ordinary failure` 选择。新增 stage 上限 0/1、跨执行累计
限制和 12 个提交失败/停止事件组合回归；独立复审关闭这两项 required。

本机只消费安装包的独立 executable 也通过同样门禁。
ClangFormat 21.1.3、cpplint 2.0.2 和 Fraction fixture 再生成检查通过。

同机补丁前后、相同 profile 的 1024² A/B/C/D accelerated 对照，本机为
1.98–2.30 倍，FreeBSD 为 1.97–2.04 倍；128² strict Float64 为
1.97–2.22 倍与 1.67–1.96 倍。4096² D Float32 为 2.23 倍与 2.01 倍，
Float64 为 2.04 倍与 1.96 倍。均为单 worker、cache off、输入预发布，取
多次执行去掉首样本后的中位数；不是跨机器比较或全帧独立 oracle 证明。
小 ROI 未证明稳定提升，generic 仅提升约 5–6%，四 worker 未改善单图内部吞吐。

这些为忽略的本次运行产物。
