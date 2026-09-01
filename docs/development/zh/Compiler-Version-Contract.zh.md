# Compiler 版本契约

公开 package、`WorkflowDocument`、operation-trait schema、semantic IR、optimizer
rule set、physical planner 与 daemon IPC 是独立 version axis。ADR 0014 定义其
identity 分离；ADR 0015 定义产品边界。

## 公开兼容性

Photospider 处于 0.x 开发。Minor release 可以做明确 breaking public API/package
change。每个 installed-boundary change 必须说明影响，并通过隔离
`find_package(Photospider)` consumer。

内部 semantic/optimized/plan representation 不是公开 serialization format。
Package 不承诺解码或执行其他 build 的内部 IR。Daemon 绝不把内部 IR 放上 local
IPC。

## Digest

`SemanticGraphDigest`、`OptimizedGraphDigest`、`ExecutionPlanDigest` 与
`PlanCacheKey` 使用 canonical domain-separated input。它们排除 runtime allocation、
timing、cancellation、ready-queue state 与 daemon identity；是非安全
reproducibility/cache identity，不是 signature、attestation、durable object id 或
receipt。Operation-v2 parameter schema 与已验证 value 影响 semantic identity；
plan-derived output/input Region 影响 physical plan identity。

## Cache 兼容性

`PlanCacheKey` 覆盖 domain-separated plan identity。如果 embedding 创建 derived
compiler cache，它必须在复用前验证 schema、stage identity、operation trait 与
target capability。任何 mismatch 都变成 cache miss 并重建；删除 cache 始终有效。

## Change checklist

- 只更新受影响的 public 或 internal version。
- Canonical byte 有意变化时更新 canonical digest vector。
- 更新英文 architecture/OpenSpec 与中文镜像。
- 运行 focused stage validation 与隔离 installed consumer。
- 除非独立显式产品决策要求，否则不增加 compatibility shim 或第二 reader。
