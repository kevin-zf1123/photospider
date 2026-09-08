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
receipt。Operation-v3 parameter schema 与已验证 value 影响 semantic identity。Float64
parameter 会按 fixed little-endian order 编码 copied IEEE-754 binary64 的精确 bit，因此
`+0.0` 与 `-0.0` 具有不同的 semantic、optimized、plan 与 cache-key identity。不会执行
NaN-payload、infinity 或 signed-zero normalization，这份 digest contract 也不增加
finite-only validation。plan-derived output/input Region 影响 physical plan identity。

## Cache 兼容性

`PlanCacheKey` 覆盖 domain-separated plan identity。如果 embedding 创建 derived
compiler cache，它必须在复用前验证 schema、stage identity、operation trait 与
target capability。任何 mismatch 都变成 cache miss 并重建；删除 cache 始终有效。

## Change checklist

- 只更新受影响的 public 或 internal version。
- Canonical byte 有意变化时更新 canonical digest vector。
- 更新受影响的英文公开文档与中文镜像。
- 更新 live GitHub Issue/Project state 与 checked-in delivery snapshot。
- 运行 focused stage validation 与隔离 installed consumer。
- 除非独立显式产品决策要求，否则不增加 compatibility shim 或第二 reader。

## 已实现的 S1 版本

#257 实现 [ADR 0016](../../adr/0016-workflow-inputs-and-execution-bindings.md)：
package 0.3.0、WorkflowDocument schema 2、OperationTraits 3 和 operation ABI 3。
Provider ABI 保持 1，增加 Float32 element code 4。C++ consumer 必须重新构建；
schema 1 和 operation ABI 2 被拒绝，无兼容适配器。
`execute(plan, token, options)` 迁移为 `execute(plan, {}, token, options)`。
SameMinorVersion 拒绝 0.2 package consumer 消费 0.3。

Identity domain 为 `semantic-graph-ir-v3`、`optimizer-v3-canonical-noop`、
`physical-plan-v3` 和 `plan-cache-key-v3`。Canonical declaration 编码 id、name、
element、shape、Region、layout 和按 key 排序的 facet key/version/payload。
Ordered source 使用 tag 1 表示 node/step、2 表示 declaration。Port kind 与 binary32
interval endpoint bit 进入 traits；整数采用 uint64 little-endian 编码。
Payload byte 和 binding order 不进入 stage identity，也未引入 runtime result cache。

仍要求 C++17。静态/共享 installed consumer 通过 C++ 和 C plugin 路径执行具名图像
oracle，并检查旧 minor version 拒绝。Daemon feature/wire 工作独立，其 0.2 package
consumer 需要协调迁移后才能消费 0.3。状态写入遵循[任务协作](Task-Collaboration.zh.md)。
