# Graph 生命周期

`GraphContext` 拥有 copied `WorkflowDocument`、nonzero monotonic revision 与 shared
currentness state。它被独立分配，没有 kernel name 或 global registry。

构造会在 revision one 发布 caller source。Semantic validation 之后才在
`Compiler::analyze` 中发生；构造不声明 document 可以成功编译。

`snapshot()` 复制 coherent document/revision pair。`replace()` 先分配新的 immutable
document，再在同一锁下推进 revision 并交换 source。Allocation/revision-overflow failure
保持 state 不变。成功 replacement 会让全部旧 snapshot、IR 与 plan 单调变成 stale。

Context 析构把 outstanding snapshot 标记为 non-current。Observing change 的 executing Run
返回 `Stale` 并拒绝 late output。析构不拥有、停止或 join shared `ExecutionContext`；
compile/execute 使用对象期间，caller 必须保持普通 C++ lifetime safety。

Kernel 没有 document filesystem adapter、implicit directory、durable graph identity、
Session lifetime 或 persistence service。
