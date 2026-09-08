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
C++ callback/operation ABI v3 view。Operation callback 必须返回 descriptor 与 plan 匹配、
Region 覆盖完整 descriptor，且 layout 已通过普通 Value validation 的 Value。

Incremental dirty propagation 位于 active package 边界之外。Demand legality 不能创建
worker、storage、daemon state，也不能宣称 partial execution 已存在。

## S1 逐端口 demand

OperationTraits 3 提供 ordered input schema。Value port 保留上述规则。
Float32Scalar port 始终请求 whole {1}。Image port 要求 Float32 {H,W,4} 和精确 linear
premultiplied profile；Elementwise 映射 H/W 空间 demand，Halo 只扩展并裁剪 H/W。
所有图像 demand 必须完整包含 channel {offset=0, extent=4}。Partial-channel、empty、
out-of-bounds 和 unknown-name demand 在 planning 失败，也检查从通用下游 consumer
传播的 partial-channel demand。Step input 为 tagged `PlanStepInput` 或
`PlanWorkflowInput`，declaration reference 不形成 scheduling task。

修改 `PlanningOptions.output_regions` 会重新规划 optimized IR，named output 仍属于
文档事实。合并后的 normalized per-step demand 决定 physical identity；被另一个 whole
output name 覆盖的小范围请求不强制改变 identity。所有 output 仍为完整 whole Value。
