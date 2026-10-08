# 曲线、采样与 LUT

已实现的基础子集、精确参数和 Region 见[基础算子实现](../../kernel-architecture/Basic-Operations.md)。规格表中的 Proposed 表示契约状态，不表示当前 runtime 未注册；NUM-01～15 与 CRV-01～11 合计 330 个 primitive keys 已进入当前 public registry，详细实现事实和验证边界见[实现进度](implementation.md)及对应 workflow README。分类表中的建议参数不覆盖现有接口。

状态 Proposed。NUM-01～15 与 CRV-01～11 合计 330 个 primitive keys 已进入当前 public registry，各簇实现事实、workflow 命令和验证边界以对应规格、README 与[实现进度](implementation.md)为准。输入使用Float32/64；控制点、表和采样位置都是显式数据，G3/G4/G5 已提供静态 shape、按端口辅助表需求和 computed scalar。

每个节点静态选择 degree，
anchors/handles/start/end 动态输入，输出 `values` 与 `axis`；输出默认 Float64。
strict 与 Apple Silicon CPU、x86-64 CPU accelerated 分别命名。以下其他族的建议不覆盖该具体规格。

各族 profile 路径的 Value/Result 类型与 Whole 规则以对应规格行为准，不能
据此推断 CRV-01～11 全部采用同一种 Result 路径。六个 LUT1D baking、两个
linear shaper、四个 resampling 和一个 LUT3D baking 仍是组合模板。
`curve.sample_linear` / `curve.sample_monotone` 是当前 controls-based Result
接口，接收 `[K,2]` 控制点并与 CRV-02 的 Bézier function sampler 分属不同算子。
输入 schema、输出和 Region 约定见[基础算子实现](../../kernel-architecture/Basic-Operations.md)。

非空 Whole 请求收集完整输入并计算完整输出，任何输入改动使完整输出失效；
远端 typed/upstream 错误和规定的数值错误可以导致 Run 失败。数学 stencil、
零权重选择、颜色身份和每族精度契约保持不变。稀疏请求也需要完整输入/输出
内存；Empty 不读取 payload。模板的独立输出投影、count=1 不读取 end，
以及 resampling positions 的独立转发仍按各自契约执行。CRV-09 旧 Value 人工流程、oracle、性能
测量、x86 与 GPU 尚未验证。逐簇公开延迟、数值核心、范围和 Instruments 证据见
[实现与性能说明](math-implementation.md)。

## 表示与目录

`[N,3]`可表示RGB三条独立函数，也可表示一个标量t到RGB的color ramp。相同shape不足以决定语义，必须写input arity、轴domain和输出通道角色。真正RGB三维LUT是`[Nr,Ng,Nb,3]`，三个颜色分量共同索引；CLF分别定义1D、3×1D与3D LUT，scalar→RGB color ramp是本规格另外定义的映射语义。[^clf]

| ID / 提议操作 | 输入 → 输出 | 参数与方法 | 验收 |
| --- | --- | --- | --- |
| CRV-01 插值族 | Result 输入各含一个 tensor member；schema id 与 member key 可任意。由 `sample_shape()` 取得 x[K]、y[K] 或 y[K,C]、query[N]；输出 `values` 为 `photospider.tensor` schema 的 `samples` member，shape 为 [N] 或 [N,C]，facets 为空 | 单／多函数 linear 与 PCHIP 是四个独立算子。非空 Whole 请求对输入声明 role 13，计算并发布完整输出 coverage；坐标保持全局值，输出 association 记录实际来源 ObjectIds；空请求不读取 payload | [具体规格](op_specs/CRV-01_interpolate.md)，状态 Proposed；12 个 keys 已注册。当前 Result CTest 与安装消费检查通过；旧 Value 路径 oracle/performance 不作为 Result 证据。`curve.sample_linear` / `curve.sample_monotone` 是独立的 controls-based 接口 |
| CRV-02 Bezier 函数采样 | anchors、relative handles、start、end 为 Result tensor 输入；`sample_shape()` 分别为 [K,2]、[K-1,degree-1,2]、[1]、[1]。输出 `values` 是 `photospider.tensor` v1 / `samples`，[count]；`axis` 同 schema/member，Float64[3]、atomic trailing axis=1 | 二次或三次；Whole role 13 读取 active 输入、先验证全局 x 单调和全部 query，再求 y；count=1 排除 end，Empty 不读 payload；非空输出完整 coverage、global coordinates 和 source ObjectId association | [完整规格](op_specs/CRV-02_sample_bezier_function.md)，Proposed。保留 RN64 控制点重建、精确单调性验证和逆求解；旧 manual/oracle/performance 不代表当前 Result 验证 |
| CRV-03 参数 Bezier 求值 | anchors、relative handles、segment_indices、t 为 Result tensor 输入；`sample_shape()` 分别为 [K,D]、[K-1,degree-1,D]、[N]、[N]；输出 `values` 为 `photospider.tensor` v1 / `samples`，[N,D] | 二次或三次；Whole role 13 验证全部 segment/t 后逐行逐分量精确求值，保留 D=1 轴；端点只选择锚点，内部使用对应控制点；非空输出完整 coverage，Empty 不读 payload | [具体规格](op_specs/CRV-03_evaluate_bezier.md)，Proposed。保留 RN64 重建、一次最终舍入和 exact polynomial；旧 manual/oracle/performance 不代表当前 Result 验证 |
| CRV-04 bake_lut1d templates | 六种函数来源+start/end/count→values/axis | 六个独立命名组合模板；作者侧 profile 默认 strict；插值查询固定 Float64；输出按需请求 | [具体规格](op_specs/CRV-04_bake_lut1d.md)，Proposed；六个公开构造器已实现并通过展开图等价验证，不自动保存或冻结，离散误差单独验收 |
| CRV-05 LUT1D 应用 | `input`、`table`、`axis` 均为单 tensor member 的 Result，schema/key 可任意；形状使用完整 `sample_shape()`（含 batch axes）。Scalar table [L]，多通道 [L,C]，axis Float64[3]；输出 `values` 为 `photospider.tensor` v1 / `samples`，完整输入 shape，facets 为空 | 独立单表与逐通道表；Whole role 13 读取三输入、先校验完整 RN64 均匀轴和全部 query，再执行数学表项选择；完整输出 coverage/global coordinates，association 记录源 ObjectIds；axis grid 由 Root 计费，table 经授权零拷贝窗口读取 | [具体规格](op_specs/CRV-05_apply_lut1d.md)，Proposed；focused CTest 与安装消费测试通过。旧1416-case oracle/manual/benchmark、最大L/C及x86数值执行未重跑 |
| CRV-06 color_ramp family | `input`、`stops`、`colors`（RationalPi 另有 Int64 分子/分母）均为单 tensor Result，schema/key 可任意；形状来自完整 `sample_shape()`，输出 `values` 为 `photospider.tensor` v1 / `samples`，shape `sample_shape(input)+[C]` | 45 个 profile keys 均为 Whole Result，role 13 验证全部输入；授权窗口直接读取，输出携带 ColorArray v1 facet、atomic trailing axis=1 与完整 coverage；仅被数学选中的 RationalPi 分母要求为正，未选中的 q<=0 不构成数学域错误；typed/upstream 验证仍覆盖完整输入 | [具体规格](op_specs/CRV-06_color_ramp.md)，Proposed / implemented_subset；focused Result CTest、Strict/本机 Apple 数值组和安装消费通过；x86、最大规模与性能未复测。目标要求 Lab/CIELCh l=L*/100；当前 ColorArray v1 运行时仍保留旧隐式 L* 尺度 |
| CRV-07 三轴 LUT3D | `input`、`table`、`axis` 为单 tensor member 的 Result，可用任意 schema/key；完整 `sample_shape()` 为 input[...,3]、table[N0,N1,N2,3]、axis[3,3]。输出 `values` 为 `photospider.tensor` v1 / `samples`，保持 input shape，使用选定 dtype，携带 ColorArray v1 与 atomic trailing axis=1 | trilinear 与 tetrahedral 独立；Whole role 13 请求三输入，按轴→原始input/query→clamp→表算术顺序验证；正权顶点分别最多8/4；完整ColorArray coverage、global coords与源ObjectId association；Root拥有网格并经授权零拷贝窗口读表 | [具体规格](op_specs/CRV-07_apply_lut3d.md)，Proposed / implemented_subset。focused Result CTest 与安装消费测试通过；ColorArray v1 的 CIELAB L* 单位与新 l=L*/100 契约差距未在本迁移关闭；其他证据边界见 workflow README |
| CRV-08 标量坐标 Shaper | `input`、`lower`、`upper` 各为单 tensor member 的 Result，可用任意 schema/key；完整 `sample_shape()` 保留 batch axes，bounds 为同 dtype [1]。输出 `values` 为 `photospider.tensor` v1 / `samples`，保持 input shape 与 input dtype，facets 为空 | 线性正反是 Result-based remap/constant 组合模板；log2 正反是六个 Whole Result profile keys。全部校验有限有序 bounds，log2 还要求 lower>0；不夹紧，IEEE special-value、端点和 signed-zero 规则依各式定义 | [具体规格](op_specs/CRV-08_shaper.md)，Proposed；focused Result CTest 与安装消费检查通过。Strict 与本机 Apple profile 有数值证据；x86、GPU、最大规模和性能未验证 |
| CRV-09 三维 LUT 烘焙 | `bake_lut3d` 将点式同模型颜色变换展开为普通 workflow 节点。动态 axis 与可选 validation_points 是单 tensor Result 输入；采样网格与验证点使用完整 `sample_shape()` | 15 个几何 profile keys 为 Whole Result，role 13 校验完整输入；导出 table 是 `photospider.tensor` v1 / `samples[N0,N1,N2,3]`，带 ColorArray v1 facet 与 atomic trailing axis=1；内部 owned table 是 `curve.bake_lut3d.table` v2 / `colors`；report 是 `curve.bake_lut3d.report` v1 Measured 摘要；表输出受全局质量 gate 限制 | [具体规格](op_specs/CRV-09_bake_lut3d.md)，Proposed / implemented_subset。Result focused CTest 与安装消费测试通过（新 profile 测试使用本机 Strict）；误差只在网格 cell centers 与可选点测量，不是全域界；ColorArray v1 的 CIELAB L* 单位差距未关闭 |
| CRV-10 反函数 | 三个 Result 输入各含一个任意 schema/key 的 tensor member；`sample_shape()` 为 x[K]、y[K]、query[N]；输出 `values` 为 `photospider.tensor` / `samples`，shape [N]，facets 为空 | 六个 linear/PCHIP profile keys；Whole role 13 读取完整输入，先验证全局 x/y topology 和全部 query；非空请求发布完整 certified coverage，坐标保持全局，association 记录 source ObjectIds | [具体规格](op_specs/CRV-10_invert.md)，Proposed；保留原始数学曲线求逆和 exact PCHIP 格点判定。当前 focused Result 覆盖与旧 manual/oracle/performance 证据边界见规格和 workflow README |
| CRV-11 resample signal | 重采样模板沿用 CRV-01；uniform `input` 和 nonuniform `positions`/`values` 是单 tensor Result，可用任意有效 schema/key，shape 使用完整 `sample_shape()`；uniform `values` 与 nonuniform `samples` 保持各自输入 shape/dtype | 四个插值模板；15 个 uniform 与 15 个 nonuniform profile keys 均为 Whole Result，role 13 校验所有输入；授权窗口直接读取，完整事务输出并保留原数学与边界规则 | [具体规格](op_specs/CRV-11_resample_signal.md)，Proposed；Result CTest、Strict/本机 Apple 手动流程、独立 oracle 与安装消费通过；x86 数值和性能未复测，低通不保证零混叠 |

## 插值选择

| 方法 | 连续性和控制 | 适用 / 取舍 |
| --- | --- | --- |
| linear | C0，局部、无区间过冲 | 首版最容易预测；宽度与折线调色 |
| PCHIP | C1、保持单调形状，二阶导数可跳 | 单调tone、非负width；不替代几何B-spline |
| cubic spline | 常见C2；边界natural/clamped/not-a-knot/periodic显式 | 更平滑，可能overshoot |
| cubic Hermite | 端点值+切线 | 艺术控制、动画缓动；切线可导致越界 |
| B-spline | degree、knots决定局部支撑与连续性 | 几何和拟合；控制点通常不在曲线上 |
| Fourier | 全局周期基、均匀采样假设 | 明确周期的轮廓/宽度调制；非周期端点可振铃 |

PCHIP与B-spline性质可对照SciPy官方实现定义；Fourier resample的周期假设明确公开。[^pchip][^bspline][^resample] 建议不要将全局高阶多项式拟合作为普通tone或width默认，局部编辑会影响全域且可能产生较大振荡。

## 采样和执行

NUM-01 与 CRV-02 的采样形式为 `start,end,count`，含端点并支持反向；
count=1 只在 start 求值且不读取 end。返回的 axis 保留起点、终点和推导步长，
具体坐标舍入及相邻坐标重复检查见单算子规格。其他采样族的区间模式须另行澄清；
CRV-02 当前通过 `sample_bezier_function_node` 公开入口进入 registry；其余目标规格仍按各自状态维护。

顺序query可线性扫描区间，预处理O(K)，求值O(K+N)；无序query可二分O(NlogK)。GPU可并行query，但完整表及shape需求要明确。动态表值变化应纳入绑定快照和缓存依赖，不隐式从可变文件读取。

默认不将曲线输出clip到[0,1]。对width要求非负时在该语义层校验或显式clip；对颜色 HDR 的范围按模型规定；LCh/HSL 保留原始 hue 与圈数，直接插值，不归一化。LUT inverse不能通过倒置数组次序实现一般反函数。

验收：x²在0,.5,1得0,.25,1；烘焙这三个点后线性查表.25得.125，准确表达离散表近似。控制点小扰动、PCHIP单调性、同shape不同arity、hue接缝与非单调inverse均需覆盖。概念链路为`expression/control points→curve→bake→apply→scope`。

## 来源

[^clf]: Academy，[*CLF Specification*](https://docs.acescentral.com/clf/specification/)，CLF v3；[Implementation Guide](https://docs.acescentral.com/clf/guides/)，LUT形状/顺序和独立测试资料。
[^pchip]: SciPy，[*PchipInterpolator*](https://docs.scipy.org/doc/scipy/reference/generated/scipy.interpolate.PchipInterpolator.html)，访问页面v1.18.0；单调插值与节点约束。
[^bspline]: SciPy，[*BSpline*](https://docs.scipy.org/doc/scipy/reference/generated/scipy.interpolate.BSpline.html)，访问页面v1.18.0；degree/knots。
[^resample]: SciPy，[*resample*](https://docs.scipy.org/doc/scipy/reference/generated/scipy.signal.resample.html)，访问页面v1.18.0；Fourier周期假设。
