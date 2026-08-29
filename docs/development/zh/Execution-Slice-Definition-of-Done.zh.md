# 执行切片完成定义

## 状态与权威

本文是 Projects #7 至 #14 未来实现切片共用的完成与 readiness 契约。它是 target-planning
contract，不是 capability 已存在的证据。当前行为仍由 `docs/kernel-architecture/` 记录；已接受
决策由 `docs/adr/` 和英文 OpenSpec 记录；实时状态由 GitHub Issues/Projects 记录。

Issue 只保留自身切片特有事实，并链接本文获得公共治理。Program/aggregate Issue 可以使用等价
program-level 形式，但必须维护真实 descendant checklist 与 completion closure。

## 必需 Issue 结构

```markdown
## Current baseline
当前权威对象、可观察行为、限制、source entry points 与长期 tests。未来 target 不得写成 current。

## Required human decisions
AFK agent 不得自行决定的 architecture authority、ABI/schema、migration、semantic legality、
security boundary、numeric target 或 product scope。获接受后链接 governing ADR/OpenSpec。

## Authority map
Owner:
Non-owners:
Input authority:
Output/commit authority:
Persistence authority:

## Public and schema impact
Public C++ API:
Plugin ABI:
IPC/API:
Document schema:
Artifact schema:
Migration policy:

## Minimal vertical
指定一个 fixture/corpus、一条真实 input-to-authoritative-execution path、一个 observable result，
以及刻意排除的精确边界。

## Failure and fallback
Unknown semantic:
Unsupported backend:
Resource exhaustion:
Cancellation:
Stale result:
Persistence failure:
Rollback:

## Exact dependencies
### Start
开始 design/implementation 前必须具备的 contracts。

### Integration
只包含其输出是本切片接入真实 product path 所必需的 upstream prerequisite vertical。不得在此
列出 downstream adopters、demonstrations 或 cross-slice validation。

### Consumers / integration validation
下游 adopters、联合演示、跨切片验证、package consumers 与证明本切片可被消费的 evidence。
本章节不创建依赖边。

### Completion
关闭前所需 evidence/conformance。Completion 是关闭门禁，不是执行边。只有确实需要完整
Project closure 时才列 Project parent。

## Verification
Unit:
Integration:
Package consumer:
Differential oracle:
Benchmark:
Fault injection:
Platform matrix:

## Completion evidence
Exact commands:
Expected artifacts:
Review gates:
Documentation updates:
Known limitations:

## Project traceability
Native parent:
Native descendants:
Projects:
Phase / Target / Risk / Work Type / Verification:
```

## Readiness 提升检查清单

`Work Type=AFK|HITL` 描述预期执行模式，不是 readiness state。只有以下全部满足时才能使用
`ready-for-agent`：

- [ ] Governing English ADR/OpenSpec 已接受，或 Issue 已证明无需新增 authority、public/schema、
      semantic、security 或 migration decision。
- [ ] 已链接精确 upstream Issue/document sections，并分别列出 Start、Integration、Completion。
- [ ] Owner、non-owner、input、output/commit、persistence authority 已冻结；被替换 authority 会完整
      删除且不留下永久 wrapper。
- [ ] Public API、plugin ABI、IPC/API、document/artifact schema、versioning、migration impact 已裁定。
- [ ] 已冻结命名 vertical fixture/corpus 与 independent oracle/invariant。
- [ ] Unknown、unsupported、exhaustion、cancellation、stale、persistence、rollback 为 fail-closed，
      或有显式有界 fallback。
- [ ] 已列出精确长期 tests、commands、expected artifacts、platform matrix、docs 与 review evidence。
- [ ] Project fields、native hierarchy 与 Issue body 一致。

Portfolio dependency validator 只使用 Start 与 Integration 构建执行图，并要求强连通分量为零、
互反依赖对为零。Completion 与 `Consumers / integration validation` 作为关闭条件和 consumer
evidence 单独验证，绝不作为执行边。

缺少任一项时，使用 canonical roles `needs-triage`、`needs-info` 或 `ready-for-human`，不得创建
替代 role label。缺 required decision/fixture 时把 Project `Verification` 设为 `Blocked`；spec
完整但 evidence 未运行时设为 `Planned`。

## 完成门禁

实现切片只有在以下全部满足时才算完成：

1. 每项 acceptance statement 都有长期 product behavior test 支撑；
2. 按需覆盖 success、failure、concurrency、cancellation、resource settlement、persistence 与
   package/platform boundaries；
3. implementation 匹配已接受 authority/migration decision，不存在重复 legacy/new API；
4. 英文权威文档与忠实中文镜像连贯；
5. 记录 exact commands、environment、outputs、limitations、review threads 与 remote integration；
6. Issue、required descendants、dependency links、Project fields、completion state 全部一致。

Consumers 与跨切片验证可以提供 completion evidence，但不能成为 producer 的反向依赖。

Numeric performance target 必须先经代表性 baseline review。关闭 parent、Project、prototype 或
planning task 本身绝不会把 target 提升为 current behavior。

## 仅规划变更

Planning-only change 验证 OpenSpec、Markdown/link/parity、GitHub Issue/Project graph 与两个 Git
repositories，并如实记录：`Not run: clean configure/full build/CTest; no runtime or source behavior
changed`，不得虚构 runtime test evidence。
