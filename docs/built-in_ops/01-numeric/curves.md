# 曲线、采样与 LUT

已实现的基础子集、精确参数和 Region 见[基础算子实现](../../kernel-architecture/Basic-Operations.md)。规格表中的 Proposed 表示契约状态，不表示当前 runtime 未注册；NUM-01～15 与 CRV-01～11 合计 330 个 primitive keys 已进入当前 public registry，详细实现事实和验证边界见[实现进度](implementation.md)及对应 workflow README。分类表中的建议参数不覆盖现有接口。

状态 Proposed。CRV-01～11 本轮范围的具体规格为 D1 草稿；规格状态与运行时状态独立。NUM-01～15 与 CRV-01～11 合计 330 个 primitive keys 已进入当前 public registry，各簇实现事实、workflow 命令和验证边界以对应规格、README 与[实现进度](implementation.md)为准。输入使用Float32/64；控制点、表和采样位置都是显式数据，G3/G4/G5 已提供静态 shape、按端口辅助表需求和 computed scalar。

本轮控制点 generator 选择二次/三次 Bézier 锚点与相对控制柄，见
[CRV-02 具体规格](op_specs/CRV-02_sample_bezier_function.md)。每个节点静态选择 degree，
anchors/handles/start/end 动态输入，输出 `values` 与 `axis`；输出默认 Float64。
strict 与 Apple Silicon CPU、x86-64 CPU accelerated 分别命名。以下其他族的建议不覆盖该具体规格。

## 表示与目录

`[N,3]`可表示RGB三条独立函数，也可表示一个标量t到RGB的color ramp。相同shape不足以决定语义，必须写input arity、轴domain和输出通道角色。真正RGB三维LUT是`[Nr,Ng,Nb,3]`，三个颜色分量共同索引；CLF分别定义1D、3×1D与3D LUT，scalar→RGB color ramp是本规格另外定义的映射语义。[^clf]

| ID / 提议操作 | 输入 → 输出 | 参数与方法 | 验收 |
| --- | --- | --- | --- |
| CRV-01 interpolate family | 单函数 x[K],y[K],query[N]→[N]；多函数 x[K],y[K,C],query[N]→[N,C] | 单／多函数与 linear/PCHIP 共四个独立算子；动态 query、Whole 全输入及完整输出、全局 x 校验、保留数学 y stencil；linear 三版本位一致，PCHIP 加速最终 4 ULP 并保持形状 | [具体规格](op_specs/CRV-01_interpolate.md)，规格 Proposed；十二个 key 已实现并完成公开 workflow 验证，当前三 profile 均精确舍入；旧 sample_linear/monotone 接口单独记录 |
| CRV-02 bezier_function | anchors/handle offsets+start/end/count → `values[N]`,`axis[3]` | 二次或三次；Whole 完整输入/输出；全局验证 x 单调，按命中段进行 y 数学计算；先解 Bx(t)=x，再取 By(t)；允许尖角和 y 过冲 | [完整规格](op_specs/CRV-02_sample_bezier_function.md)，Proposed；三 profile keys 已实现并通过 public workflow/oracle 验证；默认资源限制下 dense 大请求可能 ResourceExhausted |
| CRV-03 evaluate_bezier | anchors/handles+segment_indices[N]+t[N]→[N,D] | 同阶二次／三次参数 Bézier；控制点 RN64 重建，strict 整式正确舍入、加速最终 4 ULP；Whole 全输入/输出，保留按段／分量的数学选择 | [具体规格](op_specs/CRV-03_evaluate_bezier.md)，Proposed；三 profile keys 和公开构造器已实现并验证，数学端点选择锚点、完整输入校验、允许回折与退化 |
| CRV-04 bake_lut1d templates | 六种函数来源+start/end/count→values/axis | 六个独立命名组合模板；作者侧 profile 默认 strict；插值查询固定 Float64；输出按需请求 | [具体规格](op_specs/CRV-04_bake_lut1d.md)，Proposed；六个公开构造器已实现并通过展开图等价验证，不自动保存或冻结，离散误差单独验收 |
| CRV-05 apply_lut1d family | input+table[L] 或 table[L,C]+axis[3]→同形结果 | 单表与逐通道多表独立；共享动态轴、固定线性插值；三版本位一致；按请求表项／通道读取 | [具体规格](op_specs/CRV-05_apply_lut1d.md)，Proposed；六个 profile keys 已实现并验证，支持单点表、反向轴、三种域外策略及六种 baking 消费链 |
| CRV-06 color_ramp family | input+stops+颜色表（有理色相拆分整数分子/分母）→input.shape+[C] | RGB、CMYK、XYZ、CIELAB、CIELCh(ab)、OKLab、OKLCh、HSL、YCbCr 独立实现；携带通用颜色数组描述 | [具体规格](op_specs/CRV-06_color_ramp.md)，Proposed；九种模型已澄清，LCh/HSL 各三入口，原始 hue 保留圈数 |
| CRV-07 apply_lut3d | 三分量颜色+table[N0,N1,N2,3]+axis[3,3]→同形颜色 | trilinear/tetrahedral 独立；八种模型，同模型内可改变描述；三版本整式正确舍入 | [具体规格](op_specs/CRV-07_apply_lut3d.md)，Proposed；全局轴校验、非零权重顶点按需、整颜色观察 |
| CRV-08 shaper | 数值+共享 lower/upper→同形数值 | linear 正反为 remap 模板，log2 正反为 primitive；IEEE 值、无夹紧；log 加速 4 ULP 且单调 | [具体规格](op_specs/CRV-08_shaper.md)，Proposed；完整公式、端点精确、动态边界校验 |
| CRV-09 bake_lut3d | 逐颜色 workflow+axis→table/axis/report | 同模型 3D 组合模板；固定网格、全单元中心＋额外点 Measured 验收；报告独立，表通过后发布 | [具体规格](op_specs/CRV-09_bake_lut3d.md)，Proposed；调用者声明逐颜色独立性，非全域误差证明 |
| CRV-10 invert | x/y/query→反查 x | linear/PCHIP 独立；严格单调 y 升降序、reject/clamp；反解数学曲线，PCHIP 加速按 x 的 4 ULP 验收 | [具体规格](op_specs/CRV-10_invert.md)，Proposed；全局 x/y 校验，3D 逆暂不纳入 |
| CRV-11 resample signal | positions/values→samples/positions；独立低通保持采样轴 | 四个插值模板；等/不等间距各五种低通核，后者连续折线卷积 | [具体规格](op_specs/CRV-11_resample_signal.md)，Proposed；明确核/边界/精度，低通不保证零混叠 |

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
