# FMT-10 实现说明

本页说明当前 SDK 与内置算子的实现边界。数学契约以对应 op_specs 为准；Proposed 决策状态与已注册实现状态分开记录。

## 1. 覆盖范围和接口

| 成员 | 实现 |
|---|---|
| A | `color.rgb_to_xyz_{strict,accelerated_apple_silicon,accelerated_x86_64}` |
| B | `color.xyz_to_rgb_{strict,accelerated_apple_silicon,accelerated_x86_64}` |
| C | `color.adapt_xyz_white_{strict,accelerated_apple_silicon,accelerated_x86_64}` |
| D | C++ `ps::format::convert_linear_rgb`，事务式展开 A → 可选 C → B；没有 native D 注册项 |

头文件为 `photospider/ops/format/rgb_basis.hpp`，也由 umbrella header 导出。四个同名 authoring helper 接受实际输入边、`RgbBasisOptions` 和可选注册表。它们沿真实输入边推导结构，颜色解释来自显式 input_description；新增节点先在工作副本中静态校验，失败时不修改原图。已有/前向引用的节点 ID、导出 ID 均参与占用检查。D 根据显式源配置计算中间 XYZ 配置，并写入各生成节点的 input_description 参数；没有 edge→description 隐藏表或 Result 描述传播。

七个 preset 名为 `srgb_rec709`、`display_p3`、`rec2020`、`adobe_rgb_1998`、`prophoto_rgb`、`aces_ap0`、`aces_ap1`。方法为 `xyz_scaling`、`bradford`、`cat02`、`cat16`。B 的 white policy 仅有 require_match/preserve_xyz；D 额外支持 adapt，必须显式选择方法。没有自动 transfer、曝光、色域裁剪或峰值亮度缩放。

`RgbBasis` 支持 preset 以及可选的一致性断言，或完整自定义 6 个 primary xy 和 2 个 white xy。公开 codec 将 preset 编码为名称，自定义编码为 `basis-v1:` 加 8 个以逗号分隔的 16 位小写 binary64 十六进制字；白点为 `xy-v1:` 加 2 个字。解析也接受白点 shorthand `d65`、`d50`、`aces`。作者 helper 会规范化几何零的符号；直接传入非规范字符串会失败，避免多种等价序列化污染 identity。

原生 ParameterValue 层面的 raw `components` 是例如 `"2,3,0"` 的规范三索引字符串，`axis` 是 Int64；推荐使用类型化 helper，避免手写 codec。几何/selector/description/layout/profile 都是编译期静态选择，非适用字段会被拒绝。

## 2. 元数据与身份变换

使用 SDK TensorDescription 的 v4/v5 codec 作为显式参数。有描述时必须选择三角色完整组；RGB/XYZ 角色顺序独立于物理索引。没有描述时显式提供 axis、components 和几何。输出只携带结构和样本；下游配置由调用方提供。

原生相对单位接受 `relative` / `1`，绝对显示单位接受 `cd/m2` / `cd/m^2`；reference 接受 `scene` / `display` 与兼容拼写 `scene_relative`、`display_relative`、`display_absolute`、`absolute_display_cd_m2`。绝对坐标必须是 display reference；显式 `coordinates.scale` 必须与单位一致。别名不产生任何数值缩放，XYZ 也不隐含 Y=100 归一化。FMT-15C 的 native view helper 复用相同解析，并保留 D 的 A→可选 C→B 舍入边界。

非空 global/group/selected-channel 描述必须一致。profile/configured 坐标只有在提供明确、模型/角色顺序/单位一致的 analytic binding 时才被解释为本族的原生解析坐标；不会推断 ICC/OCIO 的等价矩阵，更不会运行 profile engine。D 的私有中间配置删除失效的 profile/configured/binding 字段并保留适用单位，随后显式写入后续节点参数。单个算子的 Result 不携带这些配置；资源仅按 preparation 显式返回的身份选择。

其他组、alpha、AOV 的数据按位旁路。与选中组重叠的另一组会被拒绝，避免变换后留下互相矛盾的别名解释；这是明确保守的 admission 边界。

只有原生矩阵**精确等于 I**才能走身份路径；C 的等白点仍先验证所选方法的所有白响应。raw identity 在非有限分类之前复制，保留 sNaN payload、符号和 -0。semantic identity 要求调用者保证完整保留输出 tile 中实际复制的选中样本有限；旁路 alpha 没有额外有限性要求。D 即使同 basis 也不替换成 I，因为 A/B 的逐节点舍入和中间失败必须保留。

### 精确矩阵

复用内部 `exact_numeric.hpp` 的有界 Natural/Rational、ResourceVector 和工作计费。坐标从 binary64 位模式构造精确有理数，H 方法常数按规范十进制整数/分母构造。矩阵逆使用精确伴随式；乘除做交叉约分，加法使用约简的分母合并，减少中间位宽。

准备阶段构造并冻结 A/B/C 的 3×3 精确矩阵；每行保留三个带符号整数分子和公共分母，避免逐像素重新推导或反复拼接分母。主基底/白归一化/锥响应有效性均为精确判断，无 epsilon 或 condition-number 截断。

每行静态保存三个带符号分子和一个正公共分母，分别受 8192-bit Natural 上限约束，随后导入只读 `FixedInteger<164>`。运行时将 finite binary64 输入表示为以 `2^-1074` 为单位的整数。单项乘积小于 `2^10290`，三项绝对值之和小于 `2^10292`；商位对齐及舍入所用双倍余数均容纳于 10496 bit。`ExactRatioWorkspace<164>` 通过整数商和余数完成一次 ties-to-even 舍入，包含符号下溢零、舍入进位和 raw 溢出到无穷。

全部可变整数属于调用的固定 scratch：ratio 的 numerator、denominator、term、shifted、candidate 以及单独的 input integer，共六个固定整数。静态 rows 由共享只读 owner 持有；运行调用不会构造 Natural/Rational、分配 ResourceVector 或调用 throwing TLS work。

raw 特殊值先按输入顺序传播首个 NaN（quiet bit，保留 payload/sign），然后处理零系数 × infinity、相反符号 infinity 等规定行为。非 identity 行即使某系数为零也消费/分类全部三项。精确零带隐含 +0 bias，结果为 +0；非零负数舍入下溢可得到 -0。

### 候选与接受条件

构建开关：

- `PHOTOSPIDER_FMT10_ALGORITHM=CERTIFIED`（默认）或 `EXACT`。
- `PHOTOSPIDER_FMT10_SIMD=ON`（默认）或 `OFF`。

CERTIFIED 预先保存每个系数的 RN64 近似和保守绝对误差上界。候选 DAG 固定为 `(a*x+b*y)+c*z`，不允许 FMA 收缩/fast-math/重关联。以乘积绝对值和、系数误差项和 underflow 余量构造绝对误差 E，并按 IEEE 位模式取相邻浮点值、向外扩展获得 `[lo,hi]`（值语义等同相应 nextafter）。`2^-48` 系数比 binary64 unit roundoff 大 32 倍，系数误差项乘二，另加 `2^-1020` 覆盖本短 DAG 的下溢误差。数学证明的完整性，特别是误差界自身舍入、次正规数和极端抵消，应独立审查；测试通过不替代证明。

Float32 只在两个端点 RN32 得到相同有限位模式时接受，因此 strict 也可使用快速候选而不放宽正确舍入。strict Float64 一律精确。accelerated Float64 仅在区间严格落于正常 Float32 表示范围，且端点位于同一 RN32 bin 时接受 double 候选；其他情况回退精确，包括参考为零、次正规或超出 Float32 范围的情形。此条件比单节点 NUM 的四 Float32-step 允许误差更保守。

候选采用有界 64 lane scratch。x86 profile 使用 AVX2 四个 double，Apple Silicon profile 使用 NEON 两个 double，尾部标量；strict 使用标量候选。没有全局 `-mavx2`，不会令 baseline 因宿主不支持 ISA 而非法执行。非有限位模式不会作为 sNaN 送入 SIMD。后端可用性复用既有 profile 检测，unsupported profile 显式失败，不静默改选。

FMT-10 没有超越函数，未改 SLEEF，也不需要在本族调用 SLEEF；其仍为整个项目现有依赖。

## 4. 调度、存储与资源

九个 A/B/C CPU keys 使用 fixed invocation ABI 8。lowering 为每个完整保留输出 tile 声明一个 task。至多七个通道区间区分颜色与旁路；非 identity 颜色行映射到三个源坐标，旁路及完整 identity 映射到自身。各映射 region 按输入自己的 TileGrid 扩张，生成去重 tile 并集。输入与输出的 atomic trailing axes 可以不同；输出总是设置为零。枚举前按矩形 tile 数检查上限，随后检查并集大小；准备 imported task 时上限同时受真实 read-slot 数约束。

`auto` 与 `materialize` 均写独立 dense row-major 输出 tile。非空 `view` 在完整矩阵为 I 时只依赖输入 schema，运行返回 `InvalidArgument/ViewUnavailable`；非 identity forced view 在静态准备时拒绝。Empty 请求仍执行静态校验，随后没有像素 task。输入负步长、零步长与未对齐地址由固定布局描述，宿主在 issue 前检查地址范围。

kernel preparation 检查完整输出 schema、tile region、dtype、Host domain、精确 read slots 与 scratch。运行按通道处理至多 64 个点，使用原输入坐标读取全部三项，保存固定输出 offsets 后散写到独立 tile。`PointReader<1>` 缓存最近 read view；跨 view 时二分查找已准备的 tile slots。复制专用 tile 使用零 scratch；算术 tile 的 `Workspace` 包含 candidate、raw/result/offset 数组、finite mask 和 `RowWorkspace`。scratch 在调用前由执行器准入，运行期只做 placement construction。

语义样本有效性属于调用者前提：非 identity 实际消费的三元组及完整保留输出 tile 中的舍入结果必须有限；完整 identity 实际复制的选中值必须有限。额外 source-grid closure 授予但未使用的样本不扩大该前提。raw IEEE 分类和候选数值质量判断仍属于计算公式。运行不提供样本域失败诊断或 `NumericDiagnostics` 发布。结构、配置、缺失 coverage、资源、后端、取消和当前性错误沿对应准备及执行阶段返回。

取消至多每 64 个点检查一次，精确乘法和除法另有有界检查点。运行的所有返回均恢复浮点环境。静态系数可被多个 Run 共享，可写 scratch 与输出按调用独立持有，宿主在完成后按 Call 顺序退休并发布 Result。D 的真实中间分配与阶段舍入被保留。

8192-bit Natural 上限约束静态精确几何构造；近奇异几何可能在准备时耗尽容量或工作预算。扩大运行整数到 10496 bit 用于容纳任意 finite binary64 样本与已接受系数的乘积，不改变几何 admission 上限。

## 5. 验证入口与边界

[独立 Fraction oracle](../../../oracle/ops/format_color/fmt10/generate.py) 使用 Python stdlib Fraction 与 Gaussian elimination 生成 28 个矩阵：七种 preset、两个对角 basis、一个近奇异自定义 basis，以及两对白点的四种方法。两种 dtype、每种 24 组三元组构成 1344 case / 4032 个期望值。最终 IEEE 舍入使用整数商和余数，运行 `python3 oracle/ops/format_color/fmt10/generate.py --check` 核验 fixture。

修改本族时现场检查公开 Plan 执行、artifact 往返、并发 Run、精确结果及 accelerated 误差界、raw 特殊值、permuted roles、旁路、显式配置、D 事务与逐阶段舍入、完整和部分输出 tiles、强制 view、负/零/未对齐源布局、Empty、错误参数、scratch 边界、运行期零分配、取消及 fenv 恢复。独立 oracle 保留；临时内置算子检查程序验证后删除。

D 使用私有准备结果推导中间配置，实际 A/C/B 节点仍由调用者 registry 准备。私有代码只读取自己创建的 Preparation；外部 registry 的 opaque state 保持不透明。准备和执行的成本分别衡量。

`outward` 用于内部误差区间端点；其 IEEE 值语义对应相邻浮点数，不承诺 libm errno 或异常标志副作用。候选误差界和舍入 bin 条件是数值正确性判断，不能因执行器取消样本验证而删除。

本族实现不包含 GPU、ICC/OCIO engine、新 observer 或非线性 transfer。有限测试不证明穷尽所有 binary32/binary64、自定义几何或资源竞争，也不替代候选误差界的数学论证。
