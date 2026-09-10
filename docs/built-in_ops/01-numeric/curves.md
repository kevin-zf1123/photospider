# 曲线、采样与 LUT

2026-09-11：本轮基础子集的接口、Region 与可运行示例见[基础算子实现](../../kernel-architecture/zh/Basic-Operations.zh.md)。其他目录项继续保持原研究状态。

状态Proposed。标量插值/查表为D1，通用Path与LUT烘焙/求逆为D2。输入使用Float32/64；建议Float64构造系数、Float32表值。控制点、表和采样位置都是显式数据，shape/辅助表/有界scalar组合需G3/G4/G5。

## 表示与目录

`[N,3]`可表示RGB三条独立函数，也可表示一个标量t到RGB的color ramp。相同shape不足以决定语义，必须写input arity、轴domain和输出通道角色。真正RGB三维LUT是`[Nr,Ng,Nb,3]`，三个颜色分量共同索引；CLF分别定义1D、3×1D与3D LUT，scalar→RGB color ramp是本规格另外定义的映射语义。[^clf]

| ID / 提议操作 | 输入 → 输出 | 参数与方法 | 验收 |
| --- | --- | --- | --- |
| CRV-01 interpolate | x[K],y[K,C],query[N]→[N,C] | x严格递增；默认linear，tone profile可选PCHIP；domain外默认error，可选clamp/linear extension | 控制点命中、重复x报错、端点与外推 |
| CRV-02 bezier_function | 二维控制点+query x→y | 必须验证x(t)单调；先求Bx(t)=x，再取By(t) | 参数t不等于横轴x；多值曲线拒绝作为函数 |
| CRV-03 parametric evaluate | curve+参数t→[N,D] | quadratic/cubic Bézier、Hermite/B-spline；允许x回转 | 端点、切线、退化段；路径语义另见paths |
| CRV-04 bake_lut1d | expression/curve+domain→[N,C] | N=256建议、包含端点；输出不默认clip | 与连续函数的误差和顶点一致分别测 |
| CRV-05 apply_lut1d | scalar/RGB+表→结果 | linear默认；domain/对应通道/越界必填 | RGB独立应用、灰阶和alpha策略 |
| CRV-06 apply_color_ramp | scalar t+表→RGB/RGBA | 同一t取全部列；颜色插值空间与alpha规则 | 与三条独立LUT同shape不同输出 |
| CRV-07 apply_lut3d | RGB+cube→RGB | tetrahedral建议，trilinear可选，axis order显式 | identity cube、网格顶点、六面、非可分离映射 |
| CRV-08 shaper | HDR值→LUT坐标 | log/分段具名，domain、负值策略；不是tone map | HDR上界、暗部精度与inverse（若有） |
| CRV-09 compose/bake | 纯颜色变换链→表/链 | domain、网格分辨率、误差预算必填 | 采样外独立测试；空间滤镜不可烘成固定颜色LUT |
| CRV-10 invert | 单调曲线/可逆映射→逆或failure | 单调1D二分；3D数值逆为D2，多解/clip拒绝 | 双向误差、plateau与不可逆区明确 |
| CRV-11 resample signal | positions/values→新positions/values | 插值与缩采样低通分开；Fourier仅显式periodic | 正弦混叠与非周期端点 |

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

规范形式建议`x0,step,count`，见NUM-01。另提供UI半开区间step模式，例如[0,1)、step=.3得到0,.3,.6,.9；由整数索引计算，不能靠反复浮点加法决定长度。采样数上界、N*C溢出、端点不足和重复节点必须在分配前验证。

顺序query可线性扫描区间，预处理O(K)，求值O(K+N)；无序query可二分O(NlogK)。GPU可并行query，但完整表及shape需求要明确。动态表值变化应纳入绑定快照和缓存依赖，不隐式从可变文件读取。

默认不将曲线输出clip到[0,1]。对width要求非负时在该语义层校验或显式clip；对颜色HDR允许超1，对hue用周期角度。LUT inverse不能通过倒置数组次序实现一般反函数。

验收：x²在0,.5,1得0,.25,1；烘焙这三个点后线性查表.25得.125，准确表达离散表近似。控制点小扰动、PCHIP单调性、同shape不同arity、hue接缝与非单调inverse均需覆盖。概念链路为`expression/control points→curve→bake→apply→scope`。

## 来源

[^clf]: Academy，[*CLF Specification*](https://docs.acescentral.com/clf/specification/)，CLF v3；[Implementation Guide](https://docs.acescentral.com/clf/guides/)，LUT形状/顺序和独立测试资料。
[^pchip]: SciPy，[*PchipInterpolator*](https://docs.scipy.org/doc/scipy/reference/generated/scipy.interpolate.PchipInterpolator.html)，访问页面v1.18.0；单调插值与节点约束。
[^bspline]: SciPy，[*BSpline*](https://docs.scipy.org/doc/scipy/reference/generated/scipy.interpolate.BSpline.html)，访问页面v1.18.0；degree/knots。
[^resample]: SciPy，[*resample*](https://docs.scipy.org/doc/scipy/reference/generated/scipy.signal.resample.html)，访问页面v1.18.0；Fourier周期假设。
