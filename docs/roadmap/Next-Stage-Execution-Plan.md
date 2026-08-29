# Next-Stage Execution Plan

## Status and authority

This document is the authoritative issue-level Execution Plan v2 for GitHub
Projects #7 through #14. It records future target work, exact dependency types,
parallel branches, native descendant closure, readiness, and the cross-project
flagship. It does not describe current runtime behavior.

[Next-Stage Development Program](Next-Stage-Development-Program.md) remains the
accepted Portfolio Architecture v1: eight domains, goals, non-goals, and parent
Issues. Current behavior remains authoritative in `docs/kernel-architecture/`,
accepted decisions in `docs/adr/` and English OpenSpec, and live state in GitHub
Issues and Projects.

Every implementation Issue inherits the
[Execution Slice Definition of Done](../development/Execution-Slice-Definition-of-Done.md).

## Live baseline on 2026-08-29

- Projects #7 through #14 remain open, private, and unchanged in domain scope.
- [#140](https://github.com/kevin-zf1123/photospider/issues/140) is
  `CLOSED/COMPLETED`. [PR #191](https://github.com/kevin-zf1123/photospider/pull/191)
  delivered one producer build, ccache reuse, eight build-smoke shards, and
  three labelled CTest consumers. PR #188 was not merged and is not main
  evidence.
- #139 and #141 through #240 remain open future/planning targets. No future
  capability becomes current by appearing in this graph.
- Current source has `GraphDefinition`/YAML ingestion, `ComputePlan`, private Run
  cancellation/event paging, explicit Region/tile/halo mechanisms, device
  transfer/residency, and immutable `JobSpec -> Attempt -> Artifact/OutputCommit`
  authority. `WorkflowDocument`, `OperationSemanticTraits`, plan digests,
  incremental compile, plan liveness/materialization, retile, public request
  handles, and the complete flagship remain targets.

## Dependency semantics

Every implementation Issue records three independent edge classes:

| Edge | Meaning |
| --- | --- |
| `Start` | Decisions and contracts required before design or implementation. |
| `Integration` | Only real upstream prerequisite verticals whose output is required to join this slice to a real product path. |
| `Completion` | Closure evidence and conformance required before the Issue may close; never an execution edge. |

Downstream adopters, joint demonstrations, cross-slice checks, package
consumers, and evidence that a slice is consumable are recorded under
`Consumers / integration validation`. That section is not a dependency class
and contributes no execution edge.

A Project parent may be a Completion gate. It is never an undifferentiated
Start or Integration gate. Completion is checked separately as a closure
condition and is never parsed as an execution or scheduling edge. Issue-number
order is not a serial calendar; independent contract, fixture, package, and
internal-refactor branches may proceed in parallel.

## Readiness and fields

`Work Type=AFK|HITL` describes execution mode and is not readiness. The only
triage roles are `needs-triage`, `needs-info`, `ready-for-agent`,
`ready-for-human`, and `wontfix`. No open portfolio Issue is
`ready-for-agent` at this planning boundary: contract decisions are
`ready-for-human`; incomplete implementation/aggregate slices are
`needs-triage`. Project `Verification` is `Planned` for reviewable human
contracts and `Blocked` for work missing upstream decisions/fixtures. #140 is
the only `Done / Verified` item.

Promotion to `ready-for-agent` requires the complete checklist in the shared
Definition of Done, including accepted authority/schema decisions, a named
fixture, fail-closed/fallback behavior, exact dependency edges, and executable
evidence.

## Delivery waves

Waves express the shortest product path; branches inside a wave can run in
parallel when their own Start dependencies are met.

### Wave A: governance, mechanisms, and early evidence

- preserve completed [#140](https://github.com/kevin-zf1123/photospider/issues/140);
- freeze version/schema/release/rollback policy in
  [#196](https://github.com/kevin-zf1123/photospider/issues/196);
- define internal Host seams in
  [#194](https://github.com/kevin-zf1123/photospider/issues/194), while
  [#142](https://github.com/kevin-zf1123/photospider/issues/142) and plugin DX
  [#143](https://github.com/kevin-zf1123/photospider/issues/143) proceed on
  independent branches;
- define OperationSemanticTraits/facet mechanisms in
  [#199](https://github.com/kevin-zf1123/photospider/issues/199) and the one-way
  WorkflowDocument migration in
  [#200](https://github.com/kevin-zf1123/photospider/issues/200);
- start the heterogeneous baseline harness
  [#212](https://github.com/kevin-zf1123/photospider/issues/212) before cost
  implementation; and
- define MED base identity/unknown rules in
  [#214](https://github.com/kevin-zf1123/photospider/issues/214).

### Wave B: minimal compiler path and early validation entrances

- deliver the no-optimization differential path
  [#201](https://github.com/kevin-zf1123/photospider/issues/201) and identities
  [#202](https://github.com/kevin-zf1123/photospider/issues/202);
- build validation/explain incrementally in
  [#148](https://github.com/kevin-zf1123/photospider/issues/148);
- develop color [#158](https://github.com/kevin-zf1123/photospider/issues/158),
  alpha [#159](https://github.com/kevin-zf1123/photospider/issues/159), channel
  [#160](https://github.com/kevin-zf1123/photospider/issues/160), and rational
  time [#215](https://github.com/kevin-zf1123/photospider/issues/215) in
  parallel; and
- expose early Python/testbench entrances
  [#176](https://github.com/kevin-zf1123/photospider/issues/176) /
  [#177](https://github.com/kevin-zf1123/photospider/issues/177) and public
  request/cancellation/update handles
  [#224](https://github.com/kevin-zf1123/photospider/issues/224).

### Wave C: optimization and physical planning

- implement canonical passes in
  [#149](https://github.com/kevin-zf1123/photospider/issues/149);
- implement affine, pointwise, channel-demand, temporal-hoisting, and
  materialization/fallback descendants
  [#204](https://github.com/kevin-zf1123/photospider/issues/204) through
  [#208](https://github.com/kevin-zf1123/photospider/issues/208), each gated by
  only the traits/MED contract it consumes;
- define cost [#209](https://github.com/kevin-zf1123/photospider/issues/209),
  liveness/alias/peak memory
  [#210](https://github.com/kevin-zf1123/photospider/issues/210), and tile/halo/
  retile/global fallback
  [#211](https://github.com/kevin-zf1123/photospider/issues/211); and
- integrate explicit transfer/residency planning in
  [#153](https://github.com/kevin-zf1123/photospider/issues/153).

### Wave D: heterogeneous and interactive verticals

- execute the named resident chain
  [#154](https://github.com/kevin-zf1123/photospider/issues/154), then calibrate
  with unchanged workloads in
  [#213](https://github.com/kevin-zf1123/photospider/issues/213) and stabilize
  explain evidence in [#156](https://github.com/kevin-zf1123/photospider/issues/156);
- freeze the viewer/session contract
  [#164](https://github.com/kevin-zf1123/photospider/issues/164);
- keep graph history [#220](https://github.com/kevin-zf1123/photospider/issues/220)
  separate from pixel history
  [#221](https://github.com/kevin-zf1123/photospider/issues/221);
- freeze the external brush-engine boundary
  [#222](https://github.com/kevin-zf1123/photospider/issues/222), integrate tile
  edits [#223](https://github.com/kevin-zf1123/photospider/issues/223), then
  add quality arbitration
  [#225](https://github.com/kevin-zf1123/photospider/issues/225); and
- close interaction evidence with
  [#168](https://github.com/kevin-zf1123/photospider/issues/168).

### Wave E: renderer and batch productization

- freeze external renderer producer/resource ownership
  [#170](https://github.com/kevin-zf1123/photospider/issues/170) and AOV/output
  schema [#172](https://github.com/kevin-zf1123/photospider/issues/172) before
  progressive scheduling
  [#171](https://github.com/kevin-zf1123/photospider/issues/171);
- deliver Deep [#226](https://github.com/kevin-zf1123/photospider/issues/226)
  and multiview [#227](https://github.com/kevin-zf1123/photospider/issues/227)
  independently;
- build renderer corpus/goldens
  [#228](https://github.com/kevin-zf1123/photospider/issues/228) before
  checkpoint/resume [#229](https://github.com/kevin-zf1123/photospider/issues/229);
- implement exact sequence work
  [#178](https://github.com/kevin-zf1123/photospider/issues/178), canonical
  manifest/outcomes [#230](https://github.com/kevin-zf1123/photospider/issues/230),
  and durable provenance/resume
  [#231](https://github.com/kevin-zf1123/photospider/issues/231); and
- complete package/release consumers through
  [#180](https://github.com/kevin-zf1123/photospider/issues/180) and
  [#198](https://github.com/kevin-zf1123/photospider/issues/198).

### Wave F: bounded production service

- freeze identity/auth/quota
  [#182](https://github.com/kevin-zf1123/photospider/issues/182) and tenant
  durable migration [#183](https://github.com/kevin-zf1123/photospider/issues/183);
- prove isolated worker/plugin execution
  [#234](https://github.com/kevin-zf1123/photospider/issues/234) and artifact/
  filesystem authorization
  [#235](https://github.com/kevin-zf1123/photospider/issues/235);
- build the private API
  [#232](https://github.com/kevin-zf1123/photospider/issues/232) before the
  authenticated public API
  [#233](https://github.com/kevin-zf1123/photospider/issues/233);
- harden egress/secrets/recovery
  [#236](https://github.com/kevin-zf1123/photospider/issues/236); and
- bound operations as observability/load baseline
  [#237](https://github.com/kevin-zf1123/photospider/issues/237), chaos/rollback
  [#238](https://github.com/kevin-zf1123/photospider/issues/238), and backup/
  restore/runbooks [#239](https://github.com/kevin-zf1123/photospider/issues/239).

First-production scope is single-node, single-region, CPU-first, and
non-active-active.

## Native descendant map

The original direct children remain attached to their Project parent. Broad
children are aggregate Issues with the following required descendants:

| Aggregate | Required descendants |
| --- | --- |
| #141 | [#194](https://github.com/kevin-zf1123/photospider/issues/194), [#195](https://github.com/kevin-zf1123/photospider/issues/195) |
| #144 | [#196](https://github.com/kevin-zf1123/photospider/issues/196), [#197](https://github.com/kevin-zf1123/photospider/issues/197), [#198](https://github.com/kevin-zf1123/photospider/issues/198) |
| #146 | [#199](https://github.com/kevin-zf1123/photospider/issues/199), [#200](https://github.com/kevin-zf1123/photospider/issues/200) |
| #147 | [#201](https://github.com/kevin-zf1123/photospider/issues/201), [#202](https://github.com/kevin-zf1123/photospider/issues/202), [#203](https://github.com/kevin-zf1123/photospider/issues/203) |
| #150 | [#204](https://github.com/kevin-zf1123/photospider/issues/204)–[#208](https://github.com/kevin-zf1123/photospider/issues/208) |
| #152 | [#209](https://github.com/kevin-zf1123/photospider/issues/209)–[#211](https://github.com/kevin-zf1123/photospider/issues/211) |
| #155 | [#212](https://github.com/kevin-zf1123/photospider/issues/212), [#213](https://github.com/kevin-zf1123/photospider/issues/213) |
| #161 | [#215](https://github.com/kevin-zf1123/photospider/issues/215)–[#217](https://github.com/kevin-zf1123/photospider/issues/217) |
| #162 | [#218](https://github.com/kevin-zf1123/photospider/issues/218), [#219](https://github.com/kevin-zf1123/photospider/issues/219) |
| #165 | [#220](https://github.com/kevin-zf1123/photospider/issues/220), [#221](https://github.com/kevin-zf1123/photospider/issues/221) |
| #166 | [#222](https://github.com/kevin-zf1123/photospider/issues/222), [#223](https://github.com/kevin-zf1123/photospider/issues/223) |
| #167 | [#224](https://github.com/kevin-zf1123/photospider/issues/224), [#225](https://github.com/kevin-zf1123/photospider/issues/225) |
| #173 | [#226](https://github.com/kevin-zf1123/photospider/issues/226), [#227](https://github.com/kevin-zf1123/photospider/issues/227) |
| #174 | [#228](https://github.com/kevin-zf1123/photospider/issues/228), [#229](https://github.com/kevin-zf1123/photospider/issues/229) |
| #179 | [#230](https://github.com/kevin-zf1123/photospider/issues/230), [#231](https://github.com/kevin-zf1123/photospider/issues/231) |
| #184 | [#232](https://github.com/kevin-zf1123/photospider/issues/232), [#233](https://github.com/kevin-zf1123/photospider/issues/233) |
| #185 | [#234](https://github.com/kevin-zf1123/photospider/issues/234)–[#236](https://github.com/kevin-zf1123/photospider/issues/236) |
| #186 | [#237](https://github.com/kevin-zf1123/photospider/issues/237)–[#239](https://github.com/kevin-zf1123/photospider/issues/239) |

Direct additions are #192/#193 under FND parent #139, MED base #214 under
#157, and the flagship #240 under #145. Project completion uses verified
descendant closure rather than a fixed five-child or six-item count.

## Flagship compositing vertical

[#240](https://github.com/kevin-zf1123/photospider/issues/240) is the single
cross-project closure Issue and has one native parent, #145. It belongs to
Projects #7 through #14 and never becomes a second execution authority.

```text
Read / Input Value
  -> Affine Transform
  -> Curve / Grade
  -> Gaussian Blur
  -> Mask + Over / Merge
  -> OCIO Display
  -> Viewer / File / Python result
```

| Project | Flagship responsibility |
| --- | --- |
| FND #7 | Contract, fixture, oracle, golden, package, and release-test governance. |
| IR #8 | WorkflowDocument -> IR -> plan, traits, identities, explain, and legal optimizations. |
| HEX #9 | Cost, liveness, tile/halo, CPU/Metal placement, transfer, counters, and fallback. |
| MED #10 | Fixed color/alpha/channel metadata, OCIO, Over legality, and independent semantic oracle. |
| INT #11 | Small ROI, edit storm, public cancellation/update, preview/final, and stale suppression. |
| REN #12 | External progressive AOV input and plan/trace/terminal evidence. |
| AUT #13 | Python construction/consumption/oracle comparison and durable final batch. |
| SRV #14 | Isolated immutable job submission and authorized artifact/result replay. |

Required evidence includes an independent CPU oracle, fixed metadata, multiple
resolutions, small ROI/halo behavior, deterministic edit storm, final batch,
plan/trace goldens, cache/transfer/reuse counters, and explicit CPU/Metal
selection/fallback. Affine, true mask/Over, OCIO display, and the dedicated
end-to-end oracle are current gaps; `add_weighted` or `curve_transform` cannot
stand in for those contracts.

## Completion rule

All future implementation Issues remain open until their own evidence is
complete. An aggregate closes only after its required descendants. A Project
closes only after its verified descendant closure and parent evidence. This
active planning change remains unarchived until human acceptance.

The dependency validator constructs the execution graph from Start and
Integration edges only. That graph must contain zero strongly connected
components and zero reciprocal dependency pairs. Completion gates and
`Consumers / integration validation` references are audited separately and
never contribute execution edges.
