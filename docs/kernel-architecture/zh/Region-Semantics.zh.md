# Region 语义

当前 package 没有 dirty-update API、ROI execution mode、dirty-source lifecycle 或
incremental propagation engine。

每个 published `Value` 带一个 bounds-checked rank-general `Region`。
Compiler-visible `OperationTraits` 带一个 closed rule：

- `Whole`：完整 logical coverage；
- `Elementwise`：input/output coordinate 直接对应；
- `Halo`：elementwise input demand 加 nonzero symmetric radius。

Compiler 验证 rule combination，并将其复制到 IR/plan identity。当前 executor 仍计算
完整 Value，不 materialize partial Region plan。Operation callback 必须返回 descriptor
与 plan 匹配、Region 覆盖完整 descriptor，且 layout 已通过普通 Value validation 的
Value。

Incremental dirty propagation 位于 active package 边界之外。Region trait 不能创建
worker、storage、daemon state，也不能宣称 partial execution 已存在。
