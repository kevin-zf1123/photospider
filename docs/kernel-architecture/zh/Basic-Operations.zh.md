# 基础算子

默认注册表通过公开 WorkflowDocument、Compiler、ExecutionContext 提供其余基础 CPU 算子。ABI/Traits 保持 7。[英文版本](../Basic-Operations.md)
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
| `image.mix` | 同规格 canonical premul RGBA A/B 与同 HW coverage M，保留图像解释，无参数。 |
| `analysis.histogram` | 场到 Int64 `[bins]`；Int64 bins 1..1048576、有限 Float64 range_min<range_max，默认 256、0、1。 |
| `analysis.histogram_out_of_range` | 场到 Int64 `[2]`，顺序 underflow、overflow；有限 Float64 range_min<range_max。 |
| `grade.levels` | 场到同 dtype 普通场；有限 Float64 black<white、gamma>0、out_min<=out_max，默认 0、1、1、0、1。 |
| `numeric.minimum/maximum/abs` | 有限 Float32/64 rank 1..8 数组，输出同 dtype 普通数组，无参数。 |
| `field.smoothstep` | 场到 Float32 canonical coverage；有限 Float64 edge0<edge1，默认 0、1。 |

## 数值与图像语义

曲线使用端点加权的均匀坐标，包含定义域两端；采样坐标重合时报错。线性插值命中控制点。
PCHIP 使用加权调和内部斜率及受限单侧端点斜率，两点退化为线性；浮点结果限制在所在段
控制值的范围内。线性曲线不裁剪 y。查询超出控制域时 reject 或取最近端点。
LUT 使用包含两端的表坐标、线性插值及相同 reject/clip 策略。普通表不自动建立
既有 `lut.apply_1d` 使用的 SampledSignal 元数据。

image.mix 对 RGBA 全部执行 `(1-M)A+MB`，M=0/1
精确返回端点；相同 alpha 保持不变。RGB 调色在提取前 unassociate，合并后 associate，
见 basic-curves 示例。

直方图边界使用保留端点的补偿 Float64 均匀插值，边界重合时报错；二分比较边界，
复杂度 O(HW*log(bins)+bins)。区间左闭右开，最后一格包含上界；范围外样本不进入 bins，由独立节点计数。
使用 checked Int64。levels 先计算 `t=clamp((x-black)/(white-black),0,1)`，再计算
`out_min+(out_max-out_min)*pow(t,1/gamma)`；gamma>1 提亮中间调；gamma=1 直接在原输入区间做补偿插值以保持抵消精度。
smoothstep 对 edge 区间得到相同形式的 t，再计算 `t*t*(3-2*t)`。
min/max 的零 tie 选负零/正零，abs 把负零变正零。旧测试算子
`field.coordinate` 和 `field.constant` 的实现与注册已删除，不保留别名。
新生成规格不表示替代算子已经可用。

## 执行、错误与资源

Elementwise：numeric min/max/abs、levels、smoothstep、image mix。
Whole：曲线、field LUT、直方图。Whole 完整物化必须满足预算。静态 shape 改变需要重新编译，
控制点、表和核样本是每次执行绑定。

输入支持 byte offset、负/零 stride 和非零 storage origin。输出与 scratch 使用宿主
allocator。PCHIP 每个控制点预留三个 Float64 数组元素；遍历轮询取消，错误释放
未发布分配，并恢复调用者浮点环境。

非有限样本及不可表示的算术/输出返回 OperationFailed；元数据或 shape 不兼容返回
TypeMismatch；非法静态参数返回 InvalidArgument。现有 traits 无法表达的跨参数关系
与普通场/表 shape 限制在 callback 检查，不增加按 operation key 的共享推导。
直接 typed 绑定错误保留宿主 InvalidArgument 约定；取消及 ResourceExhausted 保留
各自错误码。失败不返回部分成功 Value。

## 公开 workflow 与验证

[独立示例](../../../examples/foundations_workflow) 提供当前 numeric 与 expression-lut 流程；
`tests/integration/test_basic_operations.cpp` 检查数值样本、ROI、特殊视图、执行绑定、
浮点环境、取消及资源预算。

```sh
cmake --build build --target test_basic_operations photospider_foundations_workflow -j 8
ctest --test-dir build -R '^(test_basic_operations|test_workflow_numeric_reductions|test_workflow_expression_lut)$' --output-on-failure
```
