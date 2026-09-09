# 表达式与一维 LUT 算子

默认 registry 通过公开 WorkflowDocument、Compiler、ExecutionContext 提供
`numeric.sample_expression` 与 `lut.apply_1d`。两者均为 CPU Whole，输出 packed Float32。
已接受范围见 [ADR 0020](../../adr/0020-composable-operation-foundations.md)，
[英文实现文档](../Expression-and-LUT-Operations.md)为权威说明。

## 表达式采样

输入是无 facet 的 generic Float64 `[K]` 动态系数，`1<=K<=256`；全部系数必须有限，
包含未使用项。必填静态参数是 String `expression`、Int64 `count` `[1,1048576]`、
有限 Float64 `start`、有限正 Float64 `step`。构造端显式写默认值，例如 count=3、
start=0、step=.5，registry 不补参数。端点 `fma(count-1,step,start)` 必须有限，count>1
时还必须大于 start。输出 Float32 `[count]` 带 SampledSignal：通道 name/role 为
`value`，样本单位与采样轴单位均为 `dimensionless`，origin/step 来自参数。每个 plan
的 shape 固定。

语法允许十进制/科学计数数字、`x`、`c[index]`、括号、一元 `+ -`、二元 `+ - * / ^`，
一元 `abs sqrt exp log sin cos`，二元 `min max`。系数索引为非负十进制整数并检查 K。
拒绝十六进制数字、NaN/infinity 名称及任意标识符。幂右结合且优先于一元负号：
`-2^2=-4`、`2^-2=.25`、`2^3^2=512`，`0^0=1`。源码最多 4096 bytes，真实 AST 最多
256 节点、height 32，括号不增加节点。编译与执行共享迭代 parser；无脚本、循环或
文件 I/O，registry/plan 不保存 Run 绑定。

采样坐标为 `fma(i,step,start)`，受控 nearest/gradual-underflow 环境中以 Float64
求值。每个子表达式必须有限，`min(1e300*1e300,0)` 也失败。除零、函数定义域错误
失败；转 Float32 前检查范围。调用方 allocator 计量输出与 4096 bytes 系数/求值
scratch。AST 内及样本间检查取消；失败释放未发布分配并恢复调用方浮点环境。

## 线性 LUT

输入为 Float32 SampledSignal query，以及单通道 Float32 `[N]` SampledSignal 或
Lut 表，N>=2。Query 的**样本值单位**必须等于表的**采样轴单位**；query 轴单位与表
样本单位可独立。表 origin 有限、step 有限正，`fma(N-1,step,origin)` 端点有限且递增。
必填 String `out_of_domain` 为构造端默认 `reject` 或显式 `clip`；clip 越域返回最近
端点。输出保持 query shape、facets 为空，不自动建立表样本单位的输出语义。

端点精确返回表样本。内部插值对局部距离和加权乘积做 Float64 补偿，以 step 的二进制
指数归一化；先组合分子再除法以保留消减，同时避免巨大 step 的中间溢出。支持 signed、
HDR 与正负最大 Float32，中间无 gamut clamp。内部索引>=`2^53`、不可表示算术或
丢弃补偿量超过 Float32 ULP 的八分之一时以 `OperationFailed` 拒绝。首版为线性一维
插值，不包含颜色 3D LUT。

共享闭集元数据规则 `SampleExpression` (13)、`ApplyLut1d` (14) 复用
`output_semantic_input`/`output_semantic_parameter`，在 IR/直接 callback 前验证
元数据、参数及输出 shape/dtype，C 插件声明使用同一路径。前者的 String 参数是
表达式，另必填 Float64 start/step，count 来自已解析输出；后者为两个有序输入，
String 参数指定越域策略。系数索引复用同一 parser，不按 key 分派推断。ABI/Traits
保持 7。错误表达式/参数为 InvalidArgument，元数据不匹配为 TypeMismatch，计算
数值错误为附样本索引的 OperationFailed；直接非法 typed 绑定保持 InvalidArgument。
取消、stale 与资源耗尽保持既有错误码。

## 公开 workflow 与可检查结果

[test_expression_operations.cpp](../../../tests/integration/test_expression_operations.cpp)
的 sample()/luts()/gain_scene() 提供公开 workflow；document() 声明 Value，run()
编译 GraphContext 后使用每次绑定 execute。Generator 的公开节点写法如下：

```cpp
ps::WorkflowNode generator{
    1, "numeric.sample_expression", {ps::WorkflowInputReference{1}},
    {{"expression", std::string("c[0]*x^2")}, {"count", std::int64_t{3}},
     {"start", 0.0}, {"step", 0.5}}};
```

输入 1 绑定 Float64 `[1]` 系数 1，得到 `[0,.25,1]`。将其连到 lut.apply_1d 的第 2
输入，第 1 输入是 dimensionless SampledSignal query `.25`，显式 out_of_domain=reject，
得到 `.125`。修改 expression、系数、start/step 或静态 count 可组合新流程；静态
shape 改变须重编译。count=1、表达式 c[0]*2 可连接 image.exposure_gain 标量端口；
同一 plan 修改动态系数，每次消费及 cache hit 均在进入 gain 前检查 `[0,16]`。

```sh
cmake --build build/issue257-static --target test_expression_operations -j 8
ctest --test-dir build/issue257-static -R '^test_expression_operations$' --output-on-failure
```

退出零检查独立表达式/LUT oracle、多 count、语法/数值边界、顺序/并发绑定、缓存非法
gain 拒绝、共享取消和分配释放。近端点 oracle 为 `222044608266240F`，另检查精确
加权消减零、正负最大 Float32 中点零与 DBL_MAX step。安装 consumer 通过已安装公共
target 构建相同源码及纯 C 契约 fixture。

区域/stream 结果缓存允许已通过 preflight、完整 Whole dense 的直接 Value，最多
2048 bytes，覆盖 256 个 Float64 系数。类别、dtype、rank/shape、精确 facet、byte
长度及全部位进入 key；更大或局部输入无资格。纯 generic/scalar 普通 execute 保持
既有快速路径，该资格用于区域执行及 execute_stream。Generator→image 的普通 execute
实际检查 cache hit。未扩展 snapshot/disk 值种类，参见[缓存模型](Cache-Model.zh.md)。
