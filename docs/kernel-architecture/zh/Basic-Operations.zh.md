# 基础算子

默认注册表通过公开 WorkflowDocument、Compiler、ExecutionContext 提供十二组、
共 21 个新增 CPU 算子。ABI/Traits 保持 7。[英文版本](../Basic-Operations.md)
是实现契约；[research](../../built-in_ops/00-foundation/basic-operations-research.md)
记录算法来源与已批准的首版边界。

## 输入与参数

场为 Float32/Float64 `[H,W]`，允许普通数组、ScalarField 或 canonical coverage
解释。除另有说明外，输出 dtype 跟随第一个输入。数值二元输入及场与表/核要求相同
dtype；二元数值、mask、image 还要求形状匹配。没有隐式广播、转换或 GPU 执行。
以下静态参数均必填，默认值是 workflow 构造时显式填写的选择。

| 算子 | 输入输出与必填参数 |
| --- | --- |
| `curve.sample_linear/monotone` | 普通 `[K,2]`，K>=2，输出普通 `[count]`；Int64 count 2..1048576，有限 Float64 domain_min<domain_max，String out_of_domain=reject/clip。示例默认 256、0、1、reject。控制点全部有限，x 严格递增，y 可转向、有符号或 HDR。 |
| `field.apply_lut_1d` | 场及同 dtype 普通 `[N]` 表，N>=2，输出普通场；Float64 domain_min/max、String out_of_domain=reject/clip，默认 0、1、reject。 |
| `mask.invert` | canonical Float32 coverage 输入输出，无参数。 |
| `mask.combine` | 同 HW coverage；String operation=and/or/xor、algebra=fuzzy/independent_coverage；示例默认 fuzzy。 |
| `image.mix` | 同规格 canonical premul RGBA A/B 与同 HW coverage M，保留图像解释，无参数。 |
| `field.box_mean` | 场输出同 dtype/解释；Int64 radius 1..64，示例 1。 |
| `field.gaussian_blur` | 同 box，增加有限 Float64 sigma 0..64，示例 1；零表示 identity。 |
| `mask.dilate/erode` | coverage 输入输出；Int64 radius 0..64、String footprint=square/disk，示例 1、square。 |
| `field.convolve/correlate` | 场及同 dtype 普通 `[Kh,Kw]` 核，输出普通场；非负 Int64 anchor_y/x，位于核内（相关另限制 <=2^53-1）；String boundary=clamp/zero。奇数核同样显式给 anchor。 |
| `analysis.histogram` | 场到 Int64 `[bins]`；Int64 bins 1..1048576、有限 Float64 range_min<range_max，默认 256、0、1。 |
| `analysis.histogram_out_of_range` | 场到 Int64 `[2]`，顺序 underflow、overflow；有限 Float64 range_min<range_max。 |
| `grade.levels` | 场到同 dtype 普通场；有限 Float64 black<white、gamma>0、out_min<=out_max，默认 0、1、1、0、1。 |
| `numeric.minimum/maximum/abs` | 有限 Float32/64 rank 1..8 数组，输出同 dtype 普通数组，无参数。 |
| `field.smoothstep` | 场到 Float32 canonical coverage；有限 Float64 edge0<edge1，默认 0、1。 |
| `field.coordinate` | 无输入，普通 HW 输出；正 Int64 height/width <=2^53-1；String dtype=float32/float64、axis=x/y、space=pixel/normalized，示例 float32、x、pixel。 |
| `field.constant` | 同 shape/dtype 参数，增加有限 Float64 value，示例零。 |

## 数值与图像语义

曲线使用端点加权的均匀坐标，包含定义域两端；采样坐标重合时报错。线性插值命中控制点。
PCHIP 使用加权调和内部斜率及受限单侧端点斜率，两点退化为线性；浮点结果限制在所在段
控制值的范围内。线性曲线不裁剪 y。查询超出控制域时 reject 或取最近端点。
LUT 使用包含两端的表坐标、线性插值及相同 reject/clip 策略。普通表不自动建立
既有 `lut.apply_1d` 使用的 SampledSignal 元数据。

NOT=`1-A`。fuzzy AND/OR/XOR 分别为 min、max、abs(A-B)；independent coverage
分别为 AB、A+B-AB、A+B-2AB。image.mix 对 RGBA 全部执行 `(1-M)A+MB`，M=0/1
精确返回端点；相同 alpha 保持不变。RGB 调色在提取前 unassociate，合并后 associate，
见 basic-curves 示例。

box/Gaussian 为可分离方形支撑滤波，画布外 clamp，按完整窗口归一化，使用 Float64
中间值。Gaussian 样本为 `exp(-.5*(distance/sigma)^2)` 并对有限支撑归一化；极小 sigma
使非中心权重为零。形态学为灰度 max/min，画布外零，尺寸不变；disk 精确包含
`dx²+dy²<=radius²` 的离散偏移。r=0 identity；opening/closing 组合两节点。
直接形态学复杂度 O(HW*r²)，可分离模糊 O(HW*r)。

correlate=`sum(K[j]*I[p+j-anchor])`；convolve=`sum(K[j]*I[p+anchor-j])`。
同尺寸 signed 输出，核按行优先顺序 Float64 累加，复杂度 O(HW*Kh*Kw)。没有自动
核归一化、bias、alpha 或传递函数处理。

直方图边界使用保留端点的补偿 Float64 均匀插值，边界重合时报错；二分比较边界，
复杂度 O(HW*log(bins)+bins)。区间左闭右开，最后一格包含上界；范围外样本不进入 bins，由独立节点计数。
使用 checked Int64。levels 先计算 `t=clamp((x-black)/(white-black),0,1)`，再计算
`out_min+(out_max-out_min)*pow(t,1/gamma)`；gamma>1 提亮中间调；gamma=1 直接在原输入区间做补偿插值以保持抵消精度。
smoothstep 对 edge 区间得到相同形式的 t，再计算 `t*t*(3-2*t)`。
min/max 的零 tie 选负零/正零，abs 把负零变正零。坐标为像素中心 `i+.5` 或
`(i+.5)/轴长度`，x 向右、y 向下；单像素归一化坐标为 .5。

## 执行、错误与资源

Elementwise：numeric min/max/abs、levels、smoothstep、mask Boolean、image mix。
Halo：声明正半径的 box/Gaussian。Whole：曲线、field LUT、相关、直方图、
形态学和无输入生成器。卷积使用精确分阶段 kernel/邻域读取，详见
[多输出算子](Multi-Output-Operations.zh.md)。Whole 完整物化必须满足预算。静态 shape 改变需要重新编译，
控制点、表和核样本是每次执行绑定。

输入支持 byte offset、负/零 stride 和非零 storage origin。输出与 scratch 使用宿主
allocator。PCHIP 每个控制点预留三个 Float64 数组元素；模糊预留 129 个 Float64 权重
及限制在声明输入 demand 内的横向 Float64 中间场。其余新增算子不申请样本 scratch。遍历轮询取消，错误释放
未发布分配，并恢复调用者浮点环境。

非有限样本及不可表示的算术/输出返回 OperationFailed；元数据或 shape 不兼容返回
TypeMismatch；非法静态参数返回 InvalidArgument。现有 traits 无法表达的跨参数关系
与普通场/表 shape 限制在 callback 检查，不增加按 operation key 的共享推导。
直接 typed 绑定错误保留宿主 InvalidArgument 约定；取消及 ResourceExhausted 保留
各自错误码。失败不返回部分成功 Value。

## 公开 workflow 与验证

[独立示例](../../../examples/foundations_workflow) 仅通过
`find_package(Photospider CONFIG REQUIRED COMPONENTS kernel)` 构建，basic.cpp 提供：

| 场景 | 可检查结果 |
| --- | --- |
| basic-curves | unassociate → PCHIP → 通道 LUT → merge → associate → mix；alpha=.5。 |
| basic-masks | Boolean → 膨胀/腐蚀 → box 羽化；九个 coverage 样本均为 1/9。 |
| basic-filters | 非对称相关/卷积 → 绝对差 `[4,4,4]`；直方图 `[0,3]`、范围外 `[0,0]`。 |
| basic-fields | 归一坐标/常量 → smoothstep → levels → 局部 image mix；alpha=1。 |

`tests/integration/test_basic_operations.cpp` 检查独立数值样本、参数与定义域错误、
ROI/Whole、特殊视图、执行绑定变化、浮点环境恢复、取消、scratch 拒绝及预算。
同一测试源与四个示例同时进入隔离 installed-consumer 验证。

```sh
cmake --build build/issue257-static --target test_basic_operations photospider_foundations_workflow -j 8
ctest --test-dir build/issue257-static -R '^(test_basic_operations|test_foundations_basic.*)$' --output-on-failure
```

[源码目录](../../../plugins/ops/README.md) 使用与文档相同的分类。每个已注册 C++
算子拥有一个实现文件；共用算法和宿主适配器为内部辅助。迁移保留已有 key 与 ABI。
