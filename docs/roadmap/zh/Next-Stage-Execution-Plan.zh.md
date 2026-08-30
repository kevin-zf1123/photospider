# 拆仓后路线图 v3

## 状态与权威

本文是截至 2026-08-31 的权威拆仓后执行路线图，取代 Execution Plan
v2，作为 K0 至 K5 的当前排序权威。除非某行明确标为“已完成”，本文都是
目标工作；它不声称 typed compiler 已经存在。

当前 runtime 事实以 `docs/kernel-architecture/` 为权威，已接受决策以
`docs/adr/` 和英文 OpenSpec 为权威，版本/构建/CI 基线以
[拆仓后开发契约](../../development/zh/Post-Split-Development-Contract.zh.md)
为权威，实时 readiness 以 GitHub Issues 和 Projects 为权威。每个实现切片都继承
[执行切片完成定义](../../development/zh/Execution-Slice-Definition-of-Done.zh.md)。

## 可核验的拆仓后基线

- [#242](https://github.com/kevin-zf1123/photospider/issues/242) 已完成。Kernel
  [PR #243](https://github.com/kevin-zf1123/photospider/pull/243)
  从本仓移除 daemon/IPC authority，daemon
  [PR #1](https://github.com/kevin-zf1123/photospider-daemon/pull/1)
  建立了保留历史的独立仓。
- `photospider` 是 embedded kernel、operation runtime、installed package 和未来 typed
  compiler 的主开发仓。
- `photospider-daemon` 处于 IPC v2 compatible-maintenance，拥有 client、protocol、
  transport、codec、router、registries、`photospiderd` lifecycle 及 daemon 自有 tests/docs。
  K0 不扩展 protocol v3。
- Kernel Host/runtime、Job/worker、policy、trust、isolation 和 evidence code 仍保留实现且归
  kernel 所有。K0 只默认关闭 single-tenant Job product，不删除 option、targets 或 tests。
- 当前 source 具有 `GraphDefinition`/YAML ingestion 和 `ComputePlan` diagnostics。
  `WorkflowDocument`、typed compiler IRs、semantic traits、plan digests、plan cache 和
  incremental compilation 仍是未来目标。

## 仓库与版本边界

| 版本轴 | K0 值 | Owner | 兼容规则 |
| --- | --- | --- | --- |
| Photospider package | 0.1.0 | kernel 仓 | generated package file 接受 same minor |
| PhotospiderDaemon package | 0.1.0 | daemon 仓 | generated package file 接受 same minor |
| Local IPC protocol | v2 | daemon 仓 | 精确冻结的 60-method wire surface |

虽然普通 package file 公告 same-minor compatibility，daemon producer 和 installed client
当前仍精确要求 Photospider 0.1.0。Package compatibility 与 wire compatibility 相互
独立。未来 WorkflowDocument、IR、planner、digest、plan-cache 和 trait 版本也是独立
kernel contract；它们都不是 IPC 版本。

## K0-K5 执行序列

### K0：稳定拆仓后开发基线

K0 建立 ownership、defaults、package ranges、presets、比例适当的 CI、Roadmap v3、
focused Issues、agent guidance 和 verification。它不实现 typed compiler，也不实现
protocol v3。

K0 必须交付：

- kernel `kernel-dev`、`op-dev`、`legacy-full` presets；
- single-tenant Job 默认 `OFF`，`legacy-full` 显式保留；
- same-minor package files 与 daemon exact Photospider 0.1.0 discovery；
- pinned daemon PR CI 与有界 scheduled current-main signal；
- focused Host、compiler-version、plugin-DX 和 daemon-maintenance Issues；
- 中英文契约、OpenSpec、tracking 和 clean verification。

### K1：冻结 compiler、document 与 plan 版本

[#245](https://github.com/kevin-zf1123/photospider/issues/245) 只决定
WorkflowDocument、semantic/optimized IR、planner、digest、plan-cache 和 operation-trait 版本及
breaking migration。它是 open parent #196 的 focused native child。#199/#200 等待 #245，
而不等待 #196 下全部 release、rollback、signing、persistence 与 artifact 工作关闭。

### K2：先冻结 traits，再冻结 source document

1. [#199](https://github.com/kevin-zf1123/photospider/issues/199) 冻结最小
   OperationSemanticTraits v1。
2. [#200](https://github.com/kevin-zf1123/photospider/issues/200) 冻结 versioned
   WorkflowDocument 及单向 GraphDefinition/YAML migration。

Trait v1 只覆盖 purity/side effects、determinism、cacheability、shape inference、Region/halo
behavior、static/dynamic inputs、supported candidates 和 fail-closed `Unknown`。Time/media、
numeric、fusion、in-place 与 materialization 只保留 extensible identity，不给出 v1 behavior promise。

Operation ABI v1 exact-size C contract 不变。Traits 可使用 engine-owned registry 或单独版本化
sidecar。无 traits 的 ABI-v1 plugin 为 `Unknown`，只允许保守 no-optimization lowering。
第一版 WorkflowDocument 版本形状可扩展，但只支持一个 function、一个 region、一个 block，且无环。

### K3：交付差分 Compiler MVP

[#201](https://github.com/kevin-zf1123/photospider/issues/201) 与
[#202](https://github.com/kevin-zf1123/photospider/issues/202) 在 K2 后并行。两者必须共同
证明 canonical no-optimization document-to-plan path，以及独立 semantic/optimized/plan identities。
Unknown semantics 必须 reject 或保守 lowering；stale plan identity 不得执行。

K3 必须使用 engine-owned APIs 与 installed package consumers。Internal IR 不序列化进 daemon，
[#194](https://github.com/kevin-zf1123/photospider/issues/194) 或无关外围 cleanup 不混入 #199。

### K4：删除 legacy planner，再做 incremental optimization

差分路径证明 semantic equivalence 后，以一次 authority cut 删除 legacy planner，不保留双
planner 或 compatibility alias。然后在新 authority 上实现
[#203](https://github.com/kevin-zf1123/photospider/issues/203) incremental recompilation 与
[#149](https://github.com/kevin-zf1123/photospider/issues/149) canonical dead/identity/constant/CSE passes。

### K5：先证明真实 operations，再进入 heterogeneous planning

在 cost、liveness、tiling、transfer、residency 与 heterogeneous placement 前，先用真实 operation
package 和独立 oracle。P0 starter/conformance/embedded CPU runner 为
[#246](https://github.com/kevin-zf1123/photospider/issues/246)。Data-provider tooling 为 P2 #247；
lockfile #248 被 #200 阻塞；policy tooling #249 以延期 Compiler MVP 之后而关闭。

只有真实 operation compiler evidence 完成后，HEX planning 和 flagship vertical 才可成为活跃执行工作。

## 关键路径

```text
post-split contract (K0)
  -> compiler/document/plan versions (#245)
  -> OperationSemanticTraits v1 (#199)
  -> WorkflowDocument (#200)
  -> no-opt lowering + identities (#201 + #202)
  -> delete the legacy planner
  -> incremental compile + canonical passes (#203 + #149)
  -> real operations and independent oracles
  -> heterogeneous planner
```

并行分支只能在各自 Start decision 已接受时推进。Project parent 与 Completion gate 不是 Start edge。

## 拆仓后 focused Issue map

| 原 aggregate/mixed scope | 当前处置 |
| --- | --- |
| #195 installed Host + IPC callers | 以 superseded 关闭；kernel facade #244，daemon adoption `photospider-daemon#2` |
| #196 广义 version/release policy | 保持 open；已完成 extraction #242，focused compiler-version child #245 |
| #143 plugin tooling mega-slice | aggregate：operation #246、data provider #247、lockfile #248、deferred policy #249 |
| daemon package/CI/protocol | daemon #3 package range、#4 pinned/scheduled CI、#5 IPC v2 maintenance、#6 compiler-blocked next protocol |

完整 OPEN inventory 未发现可在不切分的情况下 native transfer 的 pure daemon-owned Issue。
Mixed #195 在 kernel 仓保留历史和 native parent，以 reciprocal successors 保留 provenance。
Closed #242 保留为历史 evidence，不转移。

## Projects #7-#14 仍是长期域边界

Roadmap v3 只改变活跃排序，不改变
[下一阶段开发方案](Next-Stage-Development-Program.zh.md)中已接受的 Portfolio
Architecture v1 domains。

| Project | 保留的长期 authority |
| --- | --- |
| #7 FND | build/package/Host seam/plugin DX/release mechanisms |
| #8 IR | WorkflowDocument、compiler IR、identities、plans 与 legal optimization |
| #9 HEX | costs、memory/liveness、tile/halo、placement、transfer、residency、fallback |
| #10 MED | color、alpha、channels、time 与 independent semantic oracles |
| #11 INT | viewer/edit/history/cancellation/quality product semantics |
| #12 REN | progressive renderer/AOV/checkpoint product semantics |
| #13 AUT | Python/testbench/batch/provenance/resume product semantics |
| #14 SRV | future kernel service、durable Job、worker、security 与 operational domains |

Project #14 不是 standalone IPC v2 daemon。其 deferred Job、worker、policy、trust、isolation、evidence
和 service targets 仍归 kernel 所有。原 Execution Plan v2 Wave F 只在 Git history 中保留为
portfolio planning context，不是当前 Compiler 开发阶段。

## CI 与跨仓 gate

Kernel PR 验证 kernel、operation、compiler、package 与 installed consumer behavior。Kernel CI 不
checkout daemon 仓。只有 kernel change 修改 installed API/package boundary，或 release gate 显式要求时，
才请求 daemon downstream gate。

Daemon PR 在支持平台验证 pinned supported kernel revision 与 frozen old four-cell baseline。
Scheduled/manual Ubuntu job 把 current kernel `main` 作为 drift signal，不是每个 kernel PR 的 required check。

## 完成规则

一个 Issue 只在 focused evidence、英文 authority、中文 mirror、implementation、tests、Project fields
和 live dependencies 一致后关闭。Future compiler Issues 保持 open，直到它们真正实现各自 scope。
K0 完成不能作为 K1-K5 capabilities 已存在的证据。
