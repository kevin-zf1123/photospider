# Compiler 与本地 Execution

## Compile stage

`Compiler::analyze` 检查 current `GraphSnapshot`、bounded document count/text、unique
node/output id、reference、port、operation availability、input count、每个 operation 的
closed required typed parameter schema、deterministic acyclic topology 与 static
scalar/preserve/match/fixed output descriptor inference。unknown、missing 或 wrong-type
parameter 会在 IR publication 前失败；builtin 不合成 default。它按 node-id tie-break 的 topological order
发布 immutable `SemanticGraphIR` 与 `SemanticGraphDigest`。

`Compiler::optimize` 在当前 baseline 中是显式 conservative no-op。它把 semantic node
复制进独立 `OptimizedGraphIR`，并产生 domain-separated `OptimizedGraphDigest`。

`Compiler::plan` 复制 dependency-ordered step，选择 CPU 或声明支持的 optional local GPU
backend，记录 estimated bytes，并使用 Whole、elementwise-exact 或 clipped Halo rule
把 optional named output Region 反向传播为每个 step 的 output/input demand，然后产生
`ExecutionPlan`、`ExecutionPlanDigest` 与
`PlanCacheKey`。任何 stage 都不包含 callback pointer、DSO handle、allocation、native
device 或 daemon object。每个 stage 还携带 exact frozen operation registry 的 private
runtime-only weak identity；它不进入 digest/serialization。

## Execution

`ExecutionContext` 拥有固定 CPU pool、optional single-worker GPU callback lane、bounded
queue、frozen operation registry 与 modeled-byte ledger。`execute` 创建一个 private
`ExecutionRun`，采用 deterministic ready-step ordering 与 caller-selected maximum
parallelism。

当 dependency 与 consumer 的 backend label 不同时，Run 复制 immutable bytes，创建一个
不同的 validated Value。该 copy 显式计入 transfer count/bytes。Backend label 是 Run-local
derived state；kernel 不暴露 native GPU handle 或 persistent residency registry。

每个 operation result 都会按 planned element type/shape 检查。每个 producer Value 在
transfer/callback entry 前必须覆盖 consumer planned input demand；callback 与 ABI v2 input
view 会接收该精确 demand。Executor 仍 materialize complete Value。Execution context 必须使用
产生 plan 的同一 frozen registry。Work 前、completion 期间和 result assembly 前都
会检查 cancellation 与 plan currentness。Late cancelled/stale result 会释放资源，不能
进入 caller-visible `ExecutionResult`。

Operation ABI v2 callback 无需改变 C signature 或 descriptor layout，就能区分 ordinary
failure 与 backend unavailable。只有 optional GPU attempt 返回显式 backend-unavailable
result 且 copied trait 允许 fallback 时，executor 才会在 CPU 上重试。ordinary failure 与
unknown nonzero callback result 会让 Run 失败，不产生 CPU attempt。

## Diagnostic

Raw diagnostic 包含 compile-stage duration、execute duration、operation attempt
timing/outcome、selected backend、transfer count/bytes、peak modeled bytes、fallback reason、
plan digest 与 result digest。它们是 observation，不是 verdict 或 release evidence。
