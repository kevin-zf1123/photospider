# 下一阶段执行计划

## 状态与权威

本文是 GitHub Projects #7 至 #14 的权威 Issue 级 Execution Plan v2，记录未来 target work、
精确 dependency types、parallel branches、native descendant closure、readiness 与跨项目 flagship。
它不描述当前 runtime behavior。

[下一阶段开发计划](Next-Stage-Development-Program.zh.md) 继续作为已接受的 Portfolio Architecture
v1，负责八个领域、goals/non-goals 与 parent Issues。当前行为由 `docs/kernel-architecture/` 掌握，
已接受决策由 `docs/adr/` 和英文 OpenSpec 掌握，live state 由 GitHub Issues/Projects 掌握。

每个 implementation Issue 继承
[执行切片完成定义](../../development/zh/Execution-Slice-Definition-of-Done.zh.md)。

## 2026-08-29 实时基线

- Projects #7 至 #14 保持 open/private，领域 scope 不变。
- [#140](https://github.com/kevin-zf1123/photospider/issues/140) 为
  `CLOSED/COMPLETED`；[PR #191](https://github.com/kevin-zf1123/photospider/pull/191)
  交付一个 producer build、ccache、八个 build-smoke shards 与三个 labelled CTest consumers。
  PR #188 未合并，不是 main evidence。
- #139 与 #141 至 #240 保持 open future/planning targets；进入本图不会使 capability 成为 current。
- 当前已有 `GraphDefinition`/YAML、`ComputePlan`、private Run cancellation/event paging、Region/
  tile/halo、device transfer/residency 与 immutable `JobSpec -> Attempt -> Artifact/OutputCommit`。
  `WorkflowDocument`、`OperationSemanticTraits`、plan digests、incremental compile、plan liveness/
  materialization、retile、public request handle 与 complete flagship 仍是 target。

## Dependency 语义

| Edge | 含义 |
| --- | --- |
| `Start` | 开始 design/implementation 前所需 decisions/contracts。 |
| `Integration` | 只包含其输出是本切片接入真实 product path 所必需的 upstream prerequisite vertical。 |
| `Completion` | Issue 关闭前所需 closure evidence/conformance；绝不是执行边。 |

Downstream adopters、联合演示、跨切片检查、package consumers 与证明切片可被消费的 evidence
记录在 `Consumers / integration validation` 下。该章节不是 dependency class，不贡献执行边。

Project parent 可以作为 Completion gate，但不能成为无差别 Start 或 Integration gate。
Completion 作为关闭条件单独检查，绝不解析为 execution/scheduling edge。Issue-number order 不是
serial calendar；满足自身 Start gate 的 contract、fixture、package 与 internal-refactor branches
可以并行。

## Readiness 与字段

`Work Type=AFK|HITL` 只描述 execution mode，不是 readiness。唯一 triage roles 为
`needs-triage`、`needs-info`、`ready-for-agent`、`ready-for-human`、`wontfix`。本 planning
boundary 没有 open portfolio Issue 为 `ready-for-agent`：contract decisions 使用
`ready-for-human`，未完整 implementation/aggregate slices 使用 `needs-triage`。Human contract 的
Project `Verification=Planned`，缺 upstream decision/fixture 的 work 为 `Blocked`；#140 是唯一
`Done / Verified` item。

只有完整满足公共 Definition of Done 的 accepted authority/schema decision、named fixture、
fail-closed/fallback、exact dependency edges 与 executable evidence，才能提升为 `ready-for-agent`。

## 交付 waves

Waves 表示最短 product path；同一 wave 内满足自身 Start dependencies 的 branches 可以并行。

### Wave A：治理、机制与早期证据

- 保留已完成 [#140](https://github.com/kevin-zf1123/photospider/issues/140)；
- 在 [#196](https://github.com/kevin-zf1123/photospider/issues/196) 冻结 version/schema/release/rollback；
- 由 [#194](https://github.com/kevin-zf1123/photospider/issues/194) 定义 internal Host seams，
  [#142](https://github.com/kevin-zf1123/photospider/issues/142) 与 plugin DX
  [#143](https://github.com/kevin-zf1123/photospider/issues/143) 走独立支线；
- 由 [#199](https://github.com/kevin-zf1123/photospider/issues/199) 定义 traits/facets，
  [#200](https://github.com/kevin-zf1123/photospider/issues/200) 定义单向 WorkflowDocument migration；
- 在成本实现前启动 [#212](https://github.com/kevin-zf1123/photospider/issues/212) baseline；
- 由 [#214](https://github.com/kevin-zf1123/photospider/issues/214) 定义 MED base。

### Wave B：最小 compiler path 与早期验证入口

- 交付 no-optimization path [#201](https://github.com/kevin-zf1123/photospider/issues/201) 与
  identities [#202](https://github.com/kevin-zf1123/photospider/issues/202)；
- 增量建立 [#148](https://github.com/kevin-zf1123/photospider/issues/148) validation/explain；
- 并行推进 color [#158](https://github.com/kevin-zf1123/photospider/issues/158)、alpha
  [#159](https://github.com/kevin-zf1123/photospider/issues/159)、channel
  [#160](https://github.com/kevin-zf1123/photospider/issues/160)、rational time
  [#215](https://github.com/kevin-zf1123/photospider/issues/215)；
- 前移 Python/testbench [#176](https://github.com/kevin-zf1123/photospider/issues/176) /
  [#177](https://github.com/kevin-zf1123/photospider/issues/177) 与 public request/update
  [#224](https://github.com/kevin-zf1123/photospider/issues/224)。

### Wave C：优化与物理 planning

- [#149](https://github.com/kevin-zf1123/photospider/issues/149) canonical passes；
- [#204](https://github.com/kevin-zf1123/photospider/issues/204) 至
  [#208](https://github.com/kevin-zf1123/photospider/issues/208) 分别负责 affine、pointwise、channel、
  temporal hoisting、materialization/fallback，并只依赖自身消费的 MED contract；
- [#209](https://github.com/kevin-zf1123/photospider/issues/209) cost、
  [#210](https://github.com/kevin-zf1123/photospider/issues/210) liveness/alias/peak memory、
  [#211](https://github.com/kevin-zf1123/photospider/issues/211) tile/halo/retile/global fallback；
- [#153](https://github.com/kevin-zf1123/photospider/issues/153) explicit transfer/residency planning。

### Wave D：异构与交互 verticals

- [#154](https://github.com/kevin-zf1123/photospider/issues/154) named resident chain，随后由
  [#213](https://github.com/kevin-zf1123/photospider/issues/213) calibration，
  [#156](https://github.com/kevin-zf1123/photospider/issues/156) 稳定 explain evidence；
- [#164](https://github.com/kevin-zf1123/photospider/issues/164) viewer/session；
- graph history [#220](https://github.com/kevin-zf1123/photospider/issues/220) 与 pixel history
  [#221](https://github.com/kevin-zf1123/photospider/issues/221) 分离；
- [#222](https://github.com/kevin-zf1123/photospider/issues/222) brush boundary、
  [#223](https://github.com/kevin-zf1123/photospider/issues/223) tile edits、
  [#225](https://github.com/kevin-zf1123/photospider/issues/225) quality arbitration；
- [#168](https://github.com/kevin-zf1123/photospider/issues/168) interaction evidence。

### Wave E：Renderer 与 batch 产品化

- [#170](https://github.com/kevin-zf1123/photospider/issues/170) external renderer ownership 与
  [#172](https://github.com/kevin-zf1123/photospider/issues/172) AOV/output schema 先于
  [#171](https://github.com/kevin-zf1123/photospider/issues/171) scheduling；
- Deep [#226](https://github.com/kevin-zf1123/photospider/issues/226) 与 multiview
  [#227](https://github.com/kevin-zf1123/photospider/issues/227) 独立；
- [#228](https://github.com/kevin-zf1123/photospider/issues/228) corpus/golden 先于
  [#229](https://github.com/kevin-zf1123/photospider/issues/229) checkpoint/resume；
- [#178](https://github.com/kevin-zf1123/photospider/issues/178) sequence、
  [#230](https://github.com/kevin-zf1123/photospider/issues/230) manifest/outcomes、
  [#231](https://github.com/kevin-zf1123/photospider/issues/231) durable resume；
- [#180](https://github.com/kevin-zf1123/photospider/issues/180) 与
  [#198](https://github.com/kevin-zf1123/photospider/issues/198) 完成 package/release consumers。

### Wave F：有界 production service

- [#182](https://github.com/kevin-zf1123/photospider/issues/182) identity/auth/quota 与
  [#183](https://github.com/kevin-zf1123/photospider/issues/183) tenant state；
- [#234](https://github.com/kevin-zf1123/photospider/issues/234) isolated worker/plugin 与
  [#235](https://github.com/kevin-zf1123/photospider/issues/235) artifact/filesystem authorization；
- private API [#232](https://github.com/kevin-zf1123/photospider/issues/232) 先于 public API
  [#233](https://github.com/kevin-zf1123/photospider/issues/233)；
- [#236](https://github.com/kevin-zf1123/photospider/issues/236) egress/secrets/recovery；
- [#237](https://github.com/kevin-zf1123/photospider/issues/237) baseline、
  [#238](https://github.com/kevin-zf1123/photospider/issues/238) chaos/rollback、
  [#239](https://github.com/kevin-zf1123/photospider/issues/239) backup/restore/runbooks。

首个 production scope 为 single-node、single-region、CPU-first、non-active-active。

## Native descendant map

| Aggregate | 必需 descendants |
| --- | --- |
| #141 | [#194](https://github.com/kevin-zf1123/photospider/issues/194)、[#195](https://github.com/kevin-zf1123/photospider/issues/195) |
| #144 | [#196](https://github.com/kevin-zf1123/photospider/issues/196)、[#197](https://github.com/kevin-zf1123/photospider/issues/197)、[#198](https://github.com/kevin-zf1123/photospider/issues/198) |
| #146 | [#199](https://github.com/kevin-zf1123/photospider/issues/199)、[#200](https://github.com/kevin-zf1123/photospider/issues/200) |
| #147 | [#201](https://github.com/kevin-zf1123/photospider/issues/201)、[#202](https://github.com/kevin-zf1123/photospider/issues/202)、[#203](https://github.com/kevin-zf1123/photospider/issues/203) |
| #150 | [#204](https://github.com/kevin-zf1123/photospider/issues/204)–[#208](https://github.com/kevin-zf1123/photospider/issues/208) |
| #152 | [#209](https://github.com/kevin-zf1123/photospider/issues/209)–[#211](https://github.com/kevin-zf1123/photospider/issues/211) |
| #155 | [#212](https://github.com/kevin-zf1123/photospider/issues/212)、[#213](https://github.com/kevin-zf1123/photospider/issues/213) |
| #161 | [#215](https://github.com/kevin-zf1123/photospider/issues/215)–[#217](https://github.com/kevin-zf1123/photospider/issues/217) |
| #162 | [#218](https://github.com/kevin-zf1123/photospider/issues/218)、[#219](https://github.com/kevin-zf1123/photospider/issues/219) |
| #165 | [#220](https://github.com/kevin-zf1123/photospider/issues/220)、[#221](https://github.com/kevin-zf1123/photospider/issues/221) |
| #166 | [#222](https://github.com/kevin-zf1123/photospider/issues/222)、[#223](https://github.com/kevin-zf1123/photospider/issues/223) |
| #167 | [#224](https://github.com/kevin-zf1123/photospider/issues/224)、[#225](https://github.com/kevin-zf1123/photospider/issues/225) |
| #173 | [#226](https://github.com/kevin-zf1123/photospider/issues/226)、[#227](https://github.com/kevin-zf1123/photospider/issues/227) |
| #174 | [#228](https://github.com/kevin-zf1123/photospider/issues/228)、[#229](https://github.com/kevin-zf1123/photospider/issues/229) |
| #179 | [#230](https://github.com/kevin-zf1123/photospider/issues/230)、[#231](https://github.com/kevin-zf1123/photospider/issues/231) |
| #184 | [#232](https://github.com/kevin-zf1123/photospider/issues/232)、[#233](https://github.com/kevin-zf1123/photospider/issues/233) |
| #185 | [#234](https://github.com/kevin-zf1123/photospider/issues/234)–[#236](https://github.com/kevin-zf1123/photospider/issues/236) |
| #186 | [#237](https://github.com/kevin-zf1123/photospider/issues/237)–[#239](https://github.com/kevin-zf1123/photospider/issues/239) |

FND parent #139 直接新增 #192/#193；MED parent #157 直接新增 #214；#145 直接新增 flagship #240。
Project completion 使用 verified descendant closure，不使用固定 five-child/six-item count。

## 旗舰合成纵向路径

[#240](https://github.com/kevin-zf1123/photospider/issues/240) 是唯一 cross-project closure Issue，
只有一个 native parent #145，并属于 Projects #7 至 #14。它绝不成为第二 execution authority。

```text
Read / Input Value
  -> Affine Transform
  -> Curve / Grade
  -> Gaussian Blur
  -> Mask + Over / Merge
  -> OCIO Display
  -> Viewer / File / Python result
```

| Project | Flagship 职责 |
| --- | --- |
| FND #7 | Contract、fixture、oracle、golden、package 与 release-test governance。 |
| IR #8 | WorkflowDocument -> IR -> plan、traits、identities、explain 与合法 optimizations。 |
| HEX #9 | Cost、liveness、tile/halo、CPU/Metal placement、transfer、counters 与 fallback。 |
| MED #10 | Fixed color/alpha/channel metadata、OCIO、Over legality 与 independent semantic oracle。 |
| INT #11 | Small ROI、edit storm、public cancellation/update、preview/final、stale suppression。 |
| REN #12 | External progressive AOV input 与 plan/trace/terminal evidence。 |
| AUT #13 | Python 构图/消费/oracle compare 与 durable final batch。 |
| SRV #14 | Isolated immutable job submit 与 authorized artifact/result replay。 |

证据必须包含 independent CPU oracle、fixed metadata、multiple resolutions、small ROI/halo、
deterministic edit storm、final batch、plan/trace goldens、cache/transfer/reuse counters 与显式 CPU/
Metal selection/fallback。Affine、true mask/Over、OCIO display 与 dedicated oracle 当前缺失；
`add_weighted`/`curve_transform` 不能冒充这些 contracts。

## 完成规则

所有 future implementation Issues 保持 open，直到自身 evidence 完整。Aggregate 只有在 required
descendants 完成后关闭；Project 只有在 verified descendant closure 与 parent evidence 完整后关闭。
本 active planning change 在 human acceptance 前保持不归档。

Dependency validator 只使用 Start 与 Integration edges 构建执行图；该图必须有零个强连通分量、
零个互反依赖对。Completion gates 与 `Consumers / integration validation` references 单独审核，
绝不贡献执行边。
