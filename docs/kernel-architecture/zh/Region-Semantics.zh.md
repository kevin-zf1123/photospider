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
C++ callback/operation ABI v2 view。Operation callback 必须返回 descriptor 与 plan 匹配、
Region 覆盖完整 descriptor，且 layout 已通过普通 Value validation 的 Value。

Incremental dirty propagation 位于 active package 边界之外。Demand legality 不能创建
worker、storage、daemon state，也不能宣称 partial execution 已存在。

## 已接受的 S1 目标，尚未实现

开发方向与 Float32 目标已经接受。[ADR 0016](../../adr/zh/0016-workflow-inputs-and-execution-bindings.zh.md)
已修订图像、普通标量、逐端口需求和 operation ABI v3 的具体方案；契约已经
Accepted。上述当前实现事实不变，未实现新元素或绑定；#256 跟踪决策交付，#257 负责实现。
