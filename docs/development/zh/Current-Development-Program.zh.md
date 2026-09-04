# 当前开发计划

- 快照日期：2026-09-04
- 已审计实现 baseline：`main@703569bb74164f061b233f9edc2c0b964bc868fb`
- 当前 milestone：S1 可复用 input-to-result vertical

## 角色与权威

本文件记录公开 delivery baseline、当前 milestone、active leaf Issue、dependency 与
执行顺序。它不能修改 ADR 0015 的产品边界，也不能修改
`docs/kernel-architecture/` 记录的行为。

公开 GitHub Issue 是 live delivery-status authority。本快照与 Issue 不一致时，以 Issue
为准，并同步修订本文件。GitHub Project 是 maintainer operational view，只同步 Issue
状态，不能覆盖 Issue。私有 personal-overlay OpenSpec 文件属于 maintainer working
note，没有公开 authority，也不构成交付 gate。

Active Project container 是
[#7 FND](https://github.com/users/kevin-zf1123/projects/7)、
[#8 IR](https://github.com/users/kevin-zf1123/projects/8)、
[#9 HEX](https://github.com/users/kevin-zf1123/projects/9) 与
[#10 MED](https://github.com/users/kevin-zf1123/projects/10)。S1 kernel Issue 改变
compiler 与 execution contract，因此进入 Project #8。

## 已结算实现 baseline

以下能力在已审计 baseline 已经完整交付，并已与对应 GitHub Issue 对齐：

| 领域 | 已交付 Issue | 当前证据 |
| --- | --- | --- |
| 紧凑 build 与 validation profile | [#192](https://github.com/kevin-zf1123/photospider/issues/192)、[#193](https://github.com/kevin-zf1123/photospider/issues/193) | `CMakeLists.txt`、`CMakePresets.json`、`.github/workflows/ci.yml` 与测试文档 |
| Package 与 public-version 边界 | [#196](https://github.com/kevin-zf1123/photospider/issues/196)、[#198](https://github.com/kevin-zf1123/photospider/issues/198) | `docs/development/Compiler-Version-Contract.md`、package export 与隔离 installed consumer |
| Typed source 与 compiler stage | [#199](https://github.com/kevin-zf1123/photospider/issues/199)、[#200](https://github.com/kevin-zf1123/photospider/issues/200)、[#201](https://github.com/kevin-zf1123/photospider/issues/201)、[#202](https://github.com/kevin-zf1123/photospider/issues/202) | 公开 WorkflowDocument、operation trait、semantic/optimized IR、physical plan、typed digest 与 focused test |
| Raw benchmark vertical | [#240](https://github.com/kevin-zf1123/photospider/issues/240) | `RawBenchmarkRunner`、named oracle 或显式 unchecked 状态、raw diagnostic 与 execution regression |

最新 baseline CI 是
[`kernel-ci` run 68](https://github.com/kevin-zf1123/photospider/actions/runs/33738054894)。
它在 Linux 与 macOS 上通过 static/shared kernel、ASAN 与 TSAN。

## 当前 milestone

S1 使 compiled graph 可以重复处理 caller-owned runtime Value。该 milestone 分离 graph
input declaration、compile-time fact、per-run binding、output demand 与 result
transport。

### Critical path

1. [#256](https://github.com/kevin-zf1123/photospider/issues/256)
   冻结 `WorkflowInputDeclaration`、`ExecutionBindings`、validation、identity、
   output-demand、Value lifetime 与最小 element vocabulary。
2. [#257](https://github.com/kevin-zf1123/photospider/issues/257)
   按已接受的公开 contract 实现，并增加 focused negative 与 installed consumer 覆盖。
3. [#258](https://github.com/kevin-zf1123/photospider/issues/258)
   增加一个真实 input Value、operation chain、named output 与 independent correctness
   oracle，并使用一个 compiled plan 重复执行。
4. Daemon 把已接受 kernel contract 投影为 per-Job binding 与 ephemeral bulk-result
   path。Daemon program 记录其自身 Issue 与 lifecycle constraint。

Kernel contract 与 daemon bulk-transport decision 可以并行。Kernel implementation 在
contract decision 完成后开始。最终 daemon vertical 在 kernel execution 与 daemon
transport contract 均完成后开始。

## 当前 milestone 以外的 active backlog

- [#246](https://github.com/kevin-zf1123/photospider/issues/246) 只保留可复用 operation
  starter、external consumer example 与精简 usage guide；现有正负 DSO fixture 是
  baseline。
- [#247](https://github.com/kevin-zf1123/photospider/issues/247) 只保留可复用
  data-provider starter、external consumer example 与精简 usage guide；现有 provider
  ABI fixture 是 baseline。
- [#248](https://github.com/kevin-zf1123/photospider/issues/248) 保留可选、由 embedding
  拥有的 operation-set manifest。它对 WorkflowDocument 定义的既有 dependency 已经
  完成，因此 Issue 解除 blocked 状态，等待优先级判断。
- [#148](https://github.com/kevin-zf1123/photospider/issues/148) 只保留 structured
  explain 与剩余显式 IR/plan validator delta。
- [#149](https://github.com/kevin-zf1123/photospider/issues/149) 与
  [#203](https://github.com/kevin-zf1123/photospider/issues/203) 在 S1 后依次处理
  trait-proven optimization 与 disposable incremental recompilation。
- [#151](https://github.com/kevin-zf1123/photospider/issues/151) 继续等待明确的 device
  storage/access、cost、liveness、transfer、residency、fallback 与真实 operation
  vertical 决策。
- MED work 只由选定 operation vertical 的 semantic 需求启用。

## Issue 执行契约

可执行 leaf Issue 记录 audited baseline commit、remaining delta、governing public
document、public/API/schema impact、start dependency、integration dependency、
completion gate、named fixture 或 vertical、精确 test 与 oracle、non-goal，以及预期
completion evidence。Parent Issue 只作为 index 与 closure aggregator，不携带
`ready-for-agent`。

## 更新规则

Audited baseline、当前 milestone、critical path 或 blocked reason 变化时更新本快照。
普通 implementation detail 保留在所属 Issue 与 test 中。每项 status claim 必须引用已
完成 code 与 test；unchecked item 不定义当前行为。
