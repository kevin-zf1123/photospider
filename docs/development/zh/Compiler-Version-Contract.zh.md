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
receipt。Operation-v2 parameter schema 与已验证 value 影响 semantic identity。Float64
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

## 已接受的 S1 目标版本，尚未实现

[ADR 0016](../../adr/zh/0016-workflow-inputs-and-execution-bindings.zh.md) 按已接受
契约定义目标 package 0.3.0、WorkflowDocument schema 2、OperationTraits 3、v3 编译
身份域及 operation ABI 3。Provider ABI 保持1，新增 Float32 元素4。ABI 3 为
C/C++ 公布相同逐端口约束，明确拒绝旧 operation ABI 2，不保留兼容适配器。
该具体契约已接受；当前头文件、运行版本和构建要求没有改变。

C++20 工具链另行评估。#257 验证新静态/共享安装消费者和旧次版本拒绝。
Daemon 新功能按需排期，破坏性 package 迁移仍需协调维护；状态写入继续遵循
[任务协作](Task-Collaboration.zh.md) 中的授权。
