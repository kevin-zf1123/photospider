# ADR 0005：Workflow Source Publication 与 Compilation 是分离的原子步骤

- 状态：已接受，由 ADR 0015 修订
- 日期：2026-09-01 边界修订

## 背景

Kernel 接收 caller-owned、format-neutral 的 `WorkflowDocument`。替换 source state 与
验证 compiler semantics 具有不同 failure boundary：source replacement 必须原子完成，
invalid graph 必须失败且不能发布 partial IR 或 plan。

## 决策

`GraphContext` 复制完整 source document，并拥有 nonzero monotonic revision。
`snapshot()` 捕获一对 coherent document/revision。`replace()` 在 publication 前准备新
副本，再在同一锁下推进 revision。Allocation failure 保持 source/revision 不变；revision
overflow 在 publication 前失败。成功 replacement 会立即使旧 snapshot、IR 与 plan stale。

Source publication 不声明 semantic validity。`Compiler::analyze` 把 captured snapshot 作为
一个 fail-before-publication transaction 验证：

- document version、nonzero unique node id、unique nonempty output name；
- source reference、port、operation availability 与 parameter vocabulary；
- cycle-free deterministic topology；
- operation input count 与静态推导的 type/shape/Region rule；
- 返回完整 `SemanticGraphIR` 前的 graph currentness。

Recoverable failure 使用 public `ErrorCode` 类别，例如 `InvalidArgument`、`NotFound`、
`Cycle`、`TypeMismatch` 与 `Stale`。`std::bad_alloc` 仍表示 process resource exhaustion，
不能被转换为成功的 empty graph。

后续 optimizer/planner stage 都会先构造完整 immutable value 再返回，并重复 currentness
检查。Failure 不会泄漏 partial IR/plan。

## 边界

`WorkflowDocument` 是 in-memory compiler input。Kernel 不拥有 file discovery、parser、
YAML adapter、document persistence、storage service 或 daemon wire error mapping。
Consumer 可以先把文件或 local IPC payload 转换为 document，再调用 kernel。

## 结果

- 即使 replacement 之后被判定语义非法，source replacement 仍保持原子性。
- Compiler validation 不会部分修改 graph context。
- Revision check 拒绝由已替换 source 编译出的 plan。
- File-format policy 位于 kernel package 之外。
