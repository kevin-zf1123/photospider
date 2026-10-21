# 曲线、采样与 LUT

已实现的基础子集、精确参数和 Region 见[基础算子实现](../../kernel-architecture/Basic-Operations.md)。规格表中的 Proposed 表示契约状态，不表示当前 runtime 未注册；NUM-01～15 与 CRV-01～11 合计 330 个 primitive keys 已进入当前 public registry，详细实现事实和验证边界见[实现进度](implementation.md)。分类表中的建议参数不覆盖现有接口。

状态 Proposed。NUM-01～15 与 CRV-01～11 合计 330 个 primitive keys 已进入当前 public registry，各簇实现事实、workflow 命令和验证边界以对应规格、README 与[实现进度](implementation.md)为准。输入使用Float32/64；控制点、表和采样位置都是显式数据，G3/G4/G5 已提供静态 shape、按端口辅助表需求和 computed scalar。

每个节点静态选择 degree，
anchors/handles/start/end 动态输入，输出 `values` 与 `axis`；输出默认 Float64。
strict 与 Apple Silicon CPU、x86-64 CPU accelerated 分别命名。以下其他族的建议不覆盖该具体规格。

各族 profile 路径的 Tensor/Result 类型与执行规则以对应规格行为准，不能
据此推断 CRV-01～11 全部采用同一种 Result 路径。六个 LUT1D baking、两个
linear shaper、四个 resampling 和一个 LUT3D baking 仍是组合模板。
`curve.sample_linear` / `curve.sample_monotone` 是当前 controls-based Result
接口，接收 `[K,2]` 控制点并与 CRV-02 的 Bézier function sampler 分属不同算子。
输入 schema、输出和 Region 约定见[基础算子实现](../../kernel-architecture/Basic-Operations.md)。

CRV-01 使用固定 CPU task：每个所选输出 tile 声明完整 x、对应 query 行，以及所选列范围内的完整 y 行，按各源 grid 闭包。它仅计算所选全局坐标，使用预分配 ExactCurve scratch。有限性、x 递增、reject 域与最终可表示性均为调用方前提。

CRV-02 使用固定 CPU task：values 按所选 tile 读取完整 controls 和活动 endpoints，直接搜索并重建所选段；axis 独立读取 scalar，count=1 排除 end 且 axis 零 scratch。拓扑与数值条件为调用方前提；联合选择的 outputs 共享 call 发布结果。

CRV-03 使用固定 CPU task：各所选输出 tile 读取对应 segment/t 行及所选分量的完整候选 anchor/handle 行；运行时段索引保留地址安全检查，t 与数值有限性、重建／输出可表示性由调用方保证。输出仅分配所选 tiles，各 invocation 独占固定精确算术 scratch。

CRV-10 同样使用固定任务，每所选输出 tile 读取完整 x/y 和对应 query 行；全局有限控制点与单调拓扑由调用方保证，求解原数学曲线的反函数。

CRV-05 使用固定 CPU task：L>1 按输出区域读取 query、所选通道的完整 L 行 table 和 axis 两端点，二分查找时按需重建 RN64 knot。完整轴有效性、step 一致性、有限域和可表示性为调用方前提。L=1 只读 table，input/axis 保留 schema 引用，零 scratch；reject 的 input 等于轴起点仍为调用方前提。

所有已注册曲线族均使用固定 lowering、kernel preparation 和 ABI 8 invocation。
完整输入支持由编译期 reads 表达；输出与 scratch 容量按各族物理闭包确定，不能由
完整输入要求推导出完整输出分配。共享计算使用显式 task 与私有 value。Empty
省略纯输出任务。模板的独立输出投影、count=1 排除 end，以及 resampling positions
的独立转发仍按各自契约执行。数学 stencil、零权重选择、颜色身份和精度契约见
各族规格；其数值验证与原生执行器的调度测量分开报告。已有逐簇延迟、数值核心、
范围和 Instruments 记录见[实现与性能说明](math-implementation.md)。

## 表示与目录

`[N,3]`可表示RGB三条独立函数，也可表示一个标量t到RGB的color ramp。相同shape不足以决定语义，必须写input arity、轴domain和输出通道角色。真正RGB三维LUT是`[Nr,Ng,Nb,3]`，三个颜色分量共同索引；CLF分别定义1D、3×1D与3D LUT，scalar→RGB color ramp是本规格另外定义的映射语义。[^clf]

| ID / 提议操作 | 输入 → 输出 | 参数与方法 | 验收 |
| --- | --- | --- | --- |
| CRV-01 插值族 | Result 输入各含一个 tensor member；schema id 与 member key 可任意。由 `sample_shape()` 取得 x[K]、y[K] 或 y[K,C]、query[N]；输出 `values` 为 `photospider.tensor` schema 的 `samples` member，shape 为 [N] 或 [N,C]，不附着领域描述 | 单／多函数 linear 与 PCHIP 是四个独立算子。每所选输出 tile 一个固定任务，读取完整 x、对应 query 行和所选 y 列的全部行；仅分配、计算选中输出，坐标保持全局；空请求不读取 payload | [具体规格](op_specs/CRV-01_interpolate.md)，状态 Proposed；12 个 keys 已注册。数值与执行行为按本族规格现场验证。`curve.sample_linear` / `curve.sample_monotone` 是独立的 controls-based 接口 |
| CRV-02 Bezier 函数采样 | anchors [K,2]、relative handles [K-1,degree-1,2]、start/end [1]；输出 values [count] 与原子 Float64 axis [3] | 固定 values tile 直接搜索 anchor x、重建所选段并精确求逆；axis 仅依赖 scalar；count=1 排除 end，axis 零 scratch；全局 x 拓扑及所选数值条件由调用方保证 | [具体规格](op_specs/CRV-02_sample_bezier_function.md)，Proposed；Bezier baking 可直接展开执行 |
| CRV-03 参数曲线求值 | anchors [K,D]、relative handles [K-1,degree-1,D]、Int64 segment_indices [N]、t [N] 为单 tensor member Result；使用含 batch 的完整 shape；输出 generic values [N,D]，D=1 保留 | 固定 CPU task 按输出 tile 读取 query 行及所选列的完整候选 controls；段索引保留地址 guard，数值前提由调用方保证；RN64 重建后完整多项式一次舍入，只分配所选输出 | [具体规格](op_specs/CRV-03_evaluate_bezier.md)，Proposed；物理闭包与独立数值参考分别验收 |
| CRV-04 bake_lut1d templates | 六种函数来源+start/end/count→values/axis | 六个独立命名组合模板；作者侧 profile 默认 strict；插值查询固定 Float64；输出按需请求 | [具体规格](op_specs/CRV-04_bake_lut1d.md)，Proposed；六个公开构造器已实现，修改时须验证展开图等价，不自动保存或冻结，离散误差单独验收 |
| CRV-05 LUT1D 应用 | `input`、`table`、`axis` 均为单 tensor member 的 Result，形状使用含 batch 的完整 `sample_shape()`；table [L] 或 [L,C]，axis Float64[3]；输出 `values` 为 generic tensor，保持完整输入 shape | 固定 CPU task 只计算所选 tile；L>1 读取 query 区域、所选列的完整 table 行及轴端点，按需重建 RN64 knot；L=1 只读 table、零 scratch；数值轴和定义域为调用方前提 | [具体规格](op_specs/CRV-05_apply_lut1d.md)，Proposed；精确值、依赖闭包、稀疏资源边界和实际 CPU profile 按规格验收 |
| CRV-06 color_ramp family | `input`、`stops`、`colors`（RationalPi 另有 Int64 分子/分母）均为单 tensor Result，schema/key 可任意；形状来自完整 `sample_shape()`，输出 `values` 为 `photospider.tensor` v1 / `samples`，shape `sample_shape(input)+[C]` | 45 个 key 使用固定 CPU task，读取对应 query 区域及完整 stops/colors/p/q 闭包；数值合法性为调用方前提，保留完整 row 恒等、straight RGB alpha 加权与未归一化 hue；二分直接读取 stops，invocation 独占固定算术 scratch，仅分配所选输出 tiles；CMYK 每次 preparation 校验冻结 ICC | [具体规格](op_specs/CRV-06_color_ramp.md)，Proposed / implemented；使用 TensorDescription 逻辑 tuple，Lab/CIELCh 为 l=L*/100，旧描述和 RGB association 选项移除；[实际迁移验证](NUM-CRV_color_migration.md) |
| CRV-07 三轴 LUT3D | `input`、`table`、`axis` 为单 tensor member 的 Result，可用任意 schema/key；完整 `sample_shape()` 为 input[...,3]、table[N0,N1,N2,3]、axis[3,3]。输出 `values` 为 `photospider.tensor` v1 / `samples`，保持 input shape，使用选定 dtype，不带领域描述，atomic trailing axis=1 | trilinear 与 tetrahedral 独立；每个所选输出 tile 一个固定 CPU task，读取对应 query 区域、完整 table 和六个 axis 端点，各按源网格闭包；数值合法性为调用方前提；正权完整顶点最多 8/4；invocation 独占固定 knots/算术 scratch，仅分配所选输出 tiles | [具体规格](op_specs/CRV-07_apply_lut3d.md)，Proposed / implemented；输入／输出均为显式 TensorDescription 逻辑 tuple，Lab/CIELCh 轴和表统一 l=L*/100，旧载荷被拒绝 |
| CRV-08 Coordinate shaper | input 为单 tensor Result、完整 shape 含 batch；lower/upper 为同 dtype [1]；输出 generic values 保持 shape/dtype | 线性正反为固定 remap/constant 模板；log2 正反六个 key 每所选 tile 读取 query 区域与两 bounds，固定 scratch；有限有序 bounds（log 额外 lower>0）为调用方前提，IEEE 与完整公式语义保留 | [具体规格](op_specs/CRV-08_shaper.md)，Proposed；按实际 CPU profile 验证精确值、单调性与资源行为 |
| CRV-09 三维 LUT 烘焙 | `bake_lut3d` 将点式同模型颜色变换展开为普通 workflow 节点。动态 axis 与可选 validation_points 是单 tensor Result 输入；采样网格与验证点使用完整 `sample_shape()` | 15 个几何 profile key 使用固定 CPU task，仅计算选中 tiles；grid/center 读取六端点并使用固定 scratch，纯 extra 与 color 为零 scratch；数值合法性为调用方前提。四个 Result 算子使用固定 CPU task；Pack/Unpack/Gate 将所选样本复制到独立 dense tile；Measure 等待完整 owned table 并处理全部验证点，一次写出完整报告；Gate 依次检查配置、ObjectId 和容差；退休 observer 可保留随后 Gate 失败前的报告；导出 table 是 `photospider.tensor` v1 / `samples[N0,N1,N2,3]`，不带领域描述，atomic trailing axis=1；内部 owned table 是 `curve.bake_lut3d.table` v4 / `colors`；report 是 `curve.bake_lut3d.report` v4 的 13 个静态 Tensor 成员、361 字节 Measured 摘要；`configuration_id` 在 v2 摘要域绑定完整 TensorDescription 编码与显式烘焙配置；`measured_table_id` 是报告的 UInt8[1,8] 负载，按小端编码实际测量的 owned table ObjectId，gate 从报告读取该值并与当前 table 输入的 ObjectId 精确比较；reader 接收 `BakedLut3d.description`；表输出受全局质量 gate 限制 | [具体规格](op_specs/CRV-09_bake_lut3d.md)，Proposed / implemented；Lab/LCh 的轴、表与验证点统一 l=L*/100；旧 v3 表／报告被拒绝。误差只在网格 cell centers 与可选点测量，不是全域界；标量 atol 按新坐标单位重新选择 |
| CRV-10 反函数 | 三个 Result 输入各含一个任意 schema/key 的 tensor member；`sample_shape()` 为 x[K]、y[K]、query[N]；输出 `values` 为 `photospider.tensor` / `samples`，shape [N]，不附着领域描述 | 六个 linear/PCHIP profile keys；每所选输出 tile 一个固定任务，读取完整 x/y 和对应 query 行；全局控制点有限性、单调拓扑、reject 域与输出可表示性是调用方前提；只计算所选全局输出 | [具体规格](op_specs/CRV-10_invert.md)，Proposed；保留原始数学曲线求逆和 exact PCHIP 格点判定。数值与执行验收要求见规格 |
| CRV-11 resample signal | 重采样模板沿用 CRV-01；uniform `input` 和 nonuniform `positions`/`values` 是单 tensor Result，可用任意有效 schema/key，shape 使用完整 `sample_shape()`；uniform `values` 与 nonuniform `samples` 保持各自输入 shape/dtype | 四个插值模板；15 个 uniform key 每所选输出 tile 一个固定 CPU task，按非零 tap 与边界映射生成源 tile 闭包，独占固定 scratch；仅计算、分配选中输出并保留 IEEE 语义。15 个 nonuniform key 每所选 tile 声明完整 positions 与所选其他轴对应的完整过滤轴 values，流式分段和固定系数 scratch 保留连续积分；数值合法性为调用方前提 | [具体规格](op_specs/CRV-11_resample_signal.md)，Proposed；使用独立参考现场验证实际 CPU profile，低通不保证零混叠 |

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
具体坐标舍入及所选邻点分离前提见单算子规格。其他采样族的区间模式须另行澄清；
CRV-02 当前通过 `sample_bezier_function_node` 公开入口进入 registry；其余目标规格仍按各自状态维护。

顺序query可线性扫描区间，预处理O(K)，求值O(K+N)；无序query可二分O(NlogK)。GPU可并行query，但完整表及shape需求要明确。动态表值由每次 Run 的绑定快照提供，并遵守已编译读取闭包，不隐式从可变文件读取。

默认不将曲线输出clip到[0,1]。对width要求非负时在该语义层校验或显式clip；对颜色 HDR 的范围按模型规定；LCh/HSL 保留原始 hue 与圈数，直接插值，不归一化。LUT inverse不能通过倒置数组次序实现一般反函数。

验收：x²在0,.5,1得0,.25,1；烘焙这三个点后线性查表.25得.125，准确表达离散表近似。控制点小扰动、PCHIP单调性、同shape不同arity、hue接缝与非单调inverse均需覆盖。概念链路为`expression/control points→curve→bake→apply→scope`。

## 来源

[^clf]: Academy，[*CLF Specification*](https://docs.acescentral.com/clf/specification/)，CLF v3；[Implementation Guide](https://docs.acescentral.com/clf/guides/)，LUT形状/顺序和独立测试资料。
[^pchip]: SciPy，[*PchipInterpolator*](https://docs.scipy.org/doc/scipy/reference/generated/scipy.interpolate.PchipInterpolator.html)，访问页面v1.18.0；单调插值与节点约束。
[^bspline]: SciPy，[*BSpline*](https://docs.scipy.org/doc/scipy/reference/generated/scipy.interpolate.BSpline.html)，访问页面v1.18.0；degree/knots。
[^resample]: SciPy，[*resample*](https://docs.scipy.org/doc/scipy/reference/generated/scipy.signal.resample.html)，访问页面v1.18.0；Fourier周期假设。

Measure 和 Gate 使用 Run 相关身份，每次新产生的 table 取得新的 ObjectId。固定执行器不保留跨 Run 的 result/checkpoint/block cache。报告 reader 需先获得全部 13 个成员覆盖并显式提供匹配配置，最大成员窗口为 72 字节。
