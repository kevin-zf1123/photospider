# Region 语义

当前 package 没有 dirty-update API、ROI execution mode、dirty-source lifecycle 或
incremental propagation engine。

每个 published `Value` 带一个 bounds-checked rank-general `Region`。
Compiler-visible `OperationTraits` 带一个 closed rule：

- `Whole`：完整 logical coverage；
- `Elementwise`：input/output coordinate 直接对应；
- `Halo`：elementwise input demand 加 nonzero symmetric radius。

Planning 接受 named workflow output 的 optional bounded demand，并反向遍历 plan。
`Whole` 要求每个完整 input；`Elementwise` 把 exact output interval 映射到每个 shape-
compatible input；`Halo` 对 exact demand 做对称扩张，并以不会溢出
`offset + extent + radius` 的方式 clip 到 input shape。多个 downstream demand 保守合并为
bounding Region。每个 output/input demand 都进入 physical plan/cache identity。

当前 executor 仍计算完整 Value，不 crop/materialize partial Value。Transfer/callback
entry 前，它验证 available Value Region 覆盖 plan-derived input demand，并把 demand 传给
C++ callback/operation ABI v4 view。Operation callback 必须返回 descriptor 与 plan 匹配、
Region 覆盖完整 descriptor，且 layout 已通过普通 Value validation 的 Value。

Incremental dirty propagation 位于 active package 边界之外。Demand legality 不能创建
worker、storage、daemon state，也不能宣称 partial execution 已存在。

## S1 逐端口 demand

OperationTraits 4 提供 ordered input schema。Value port 保留上述规则。
Float32Scalar port 始终请求 whole {1}。Image port 要求 Float32 {H,W,4} 和精确 linear
premultiplied profile；Elementwise 映射 H/W 空间 demand，Halo 只扩展并裁剪 H/W。
所有图像 demand 必须完整包含 channel {offset=0, extent=4}。Partial-channel、empty、
out-of-bounds 和 unknown-name demand 在 planning 失败，也检查从通用下游 consumer
传播的 partial-channel demand。Step input 为 tagged `PlanStepInput` 或
`PlanWorkflowInput`，declaration reference 不形成 scheduling task。

修改 PlanningOptions.output_regions、tile_height 或 tile_width 会重新规划 optimized IR。
tile 高/宽必须为正，默认 128x128。每个输出名称的精确 Region 进入物理 identity，包括
指向同一节点但请求不同区域的别名。运行期 payload 不进入 identity。

## 惰性 tile 规划

ExecutionPlan::tile_plan(name, region) 为该名称请求中的非空子区域派生去掉无关依赖的
计划，无需分析源文档或构造完整 tile 网格。逐 tile 反推需求，只在本 tile 合并 fan-out，
重叠 halo 可以重算。whole_boundary（Whole 规则、非确定或有副作用）步骤要求完整
coverage，并须每 Run 只执行一次；#265 负责运行期保留和流式执行。在 #265 接入前，
普通整图 executor 仍是当前集成路径。

静态参数可声明有限闭区间。Int64 边界必须是 +/- (2^53-1) 内的精确整数；Float64 边界
必须有限。halo_radius_parameter 必须引用 required bounded Int64，最小值至少 1，
最大值不超过 UINT32_MAX，所属算子为 Halo 且固定 radius 为零。analyze 校验参数后
将解析半径写入 node traits，再推导 shape/demand。无边界 Float64 保留原先精确 bit 行为。

Float32Mask 端口为二维 {H,W}、无 facet、有限 [0,1]，空间尺寸匹配图像。需求映射图像
前两轴，scalar 仍为完整 {1}。halo 对逻辑图像边界裁剪，不对 tile 边界裁剪；即使通过
通用下游传播，图像部分通道需求也会被拒绝。tile 工作集按区域输出加声明 scratch
计算。test_tile_plan 覆盖参数范围、边界/裁剪、小 tile、fan-out、mask、Whole/effect 和 identity。
