# Current Development Program

- Snapshot date: 2026-09-10
- Audited foundation: `main@fba06270` (delivered S4, package 0.6.0)
- Current milestone: G1/G2/G3/G5 operation foundations, #287; local implementation verified
- Delivery branch: `ops-foundations` into `ops`, with no merge into main

## Role and authority

This file records the public delivery baseline, current milestone, active leaf
Issues, dependencies, and execution order. It cannot change the product
boundary in ADR 0015 or behavior documented under `docs/kernel-architecture/`.

Public GitHub Issues are the live delivery-status authority. If this snapshot
differs from an Issue, the Issue prevails and this file must be reconciled.
GitHub Projects are maintainer operational views that mirror Issues and cannot
override them. Private personal-overlay OpenSpec files are maintainer working
notes. They have no public authority and do not gate delivery.

The active Project containers are
[#7 FND](https://github.com/users/kevin-zf1123/projects/7),
[#8 IR](https://github.com/users/kevin-zf1123/projects/8),
[#9 HEX](https://github.com/users/kevin-zf1123/projects/9), and
[#10 MED](https://github.com/users/kevin-zf1123/projects/10). The S1 kernel
Issues are tracked in Project #8 because they change compiler and execution
contracts.

## Settled implementation baseline

The following capabilities were already complete at the audited baseline
and were reconciled with their GitHub Issues:

| Area | Delivered Issues | Current evidence |
| --- | --- | --- |
| Compact build and validation profiles | [#192](https://github.com/kevin-zf1123/photospider/issues/192), [#193](https://github.com/kevin-zf1123/photospider/issues/193) | `CMakeLists.txt`, `CMakePresets.json`, `.github/workflows/ci.yml`, and testing documentation |
| Package and public-version boundary | [#196](https://github.com/kevin-zf1123/photospider/issues/196), [#198](https://github.com/kevin-zf1123/photospider/issues/198) | `docs/development/Compiler-Version-Contract.md`, package exports, and isolated installed consumers |
| Typed source and compiler stages | [#199](https://github.com/kevin-zf1123/photospider/issues/199), [#200](https://github.com/kevin-zf1123/photospider/issues/200), [#201](https://github.com/kevin-zf1123/photospider/issues/201), [#202](https://github.com/kevin-zf1123/photospider/issues/202) | Public WorkflowDocument, operation traits, semantic/optimized IR, physical plan, typed digests, and focused tests |
| Raw benchmark vertical | [#240](https://github.com/kevin-zf1123/photospider/issues/240) | `RawBenchmarkRunner`, named oracle or explicit unchecked status, raw diagnostics, and execution regressions |

The historical S0 baseline CI was
[`kernel-ci` run 68](https://github.com/kevin-zf1123/photospider/actions/runs/33738054894).
It passed on Linux and macOS for static and shared kernels, plus ASAN and TSAN.

## Current milestone

The synchronized baseline `main@fba06270` contains S4 package 0.6.0 and
operation ABI/traits 6: explicit CpuExact/MetalFp32 planning, native shared
storage, all eight image operations, regional uploads, bounded residency and
fallback. The [S4 guide](../kernel-architecture/S4-Workflow.md) and its executable
examples/tests document that implemented baseline. Foundation migration preserves
its CPU/C/Metal behavior; focused native validation has also exercised signed/HDR
image-v2 inputs with actual dispatches and zero fallback.

[Foundations #287](https://github.com/kevin-zf1123/photospider/issues/287) implements
[ADR 0020](../adr/0020-composable-operation-foundations.md). Its accepted target
is package 0.7.0/ABI 7, structured semantics and image v2, static output inference,
computed scalars, complete eight-op migration, cache/snapshot integration and
reusable numeric/channel/color/expression/LUT/component workflows. The local
implementation now includes all ten slices, with focused tests and isolated
static/shared consumers. The [standalone example](../../examples/foundations_workflow)
runs six public composition scenarios; owning operator guides link the focused
regressions. Live comprehensive-review, PR CI, Codex bot review and merge status
are recorded in #287 and its implementation PR. Local validation does not establish
those delivery gates. Schema 2, provider ABI 1 and C++17 remain.

| Order | Active leaf | Completion boundary |
| --- | --- | --- |
| 1 | #288 | Accepted contracts and target/fact separation |
| 2 | #289 | Shared semantics, output inference and C/C++ ABI |
| 3 | #290 | Eight-op C++/C/Metal migration |
| 4 | #291 | Snapshots, freeze and complete cache semantics |
| 5 | #292 | Computed scalar validation and composition |
| 6 | #293 | Numeric cast/range/arithmetic/reduction |
| 7 | #294 | Channel, alpha and reference-white color combinations |
| 8 | #295 | Bounded expression, dynamic coefficients and linear LUT |
| 9 | #296 | Threshold, labels and fixed-capacity attributes |
| 10 | #297 | Installed public workflow examples and combined acceptance |

Each leaf depends on the preceding delivery slice. ABI migration may update
existing callers mechanically to keep each commit buildable. The eight-op and
snapshot/cache slices have implemented their supported image-v2 representations. One implementation writer owns
project changes; the coordinator owns Issue/commit/PR administration. Separate
Issue commits precede a fresh independent comprehensive review, six required
CI jobs and Codex review-bot fixes. The sole implementation PR merges
`ops-foundations` into `ops` with a merge commit. Settle Issues after verified
ops delivery, retain local/remote ops, then remove only ops-foundations.

G4 spatial dependency expansion, G6 host assets, daemon 0.6 migration, full paths,
FFT and ICC/OCIO are outside this milestone. Main keeps its baseline; no 0.7
main delivery is implied. S5 calibration/automatic placement #209 and incremental
compiler #203 remain separate work. The broader operation catalogue is Proposed.

## Active backlog outside the milestone

- [#246](https://github.com/kevin-zf1123/photospider/issues/246) retains only a
  reusable operation starter, external consumer example, and concise usage
  guide; existing positive and negative DSO fixtures are the baseline.
- [#247](https://github.com/kevin-zf1123/photospider/issues/247) retains only a
  reusable data-provider starter, external consumer example, and concise usage
  guide; existing provider ABI fixtures are the baseline.
- [#248](https://github.com/kevin-zf1123/photospider/issues/248) remains an
  optional embedding-owned operation-set manifest. Its former dependency on
  WorkflowDocument definition is complete, so the Issue is unblocked and
  awaits prioritization.
- [#148](https://github.com/kevin-zf1123/photospider/issues/148) retains the
  structured explain and remaining explicit IR/plan-validator delta.
- [#149](https://github.com/kevin-zf1123/photospider/issues/149) and
  [#203](https://github.com/kevin-zf1123/photospider/issues/203) follow S1 with
  trait-proven optimization and disposable incremental recompilation.
- [#151](https://github.com/kevin-zf1123/photospider/issues/151) and #152
  retain cost/calibration work for S5; S4's explicit placement does not depend
  on #209 and does not close these broader parents.
- MED work is activated only by the semantic needs of a selected operation
  vertical.

## Issue execution contract

An executable leaf Issue records its audited baseline commit, remaining delta,
governing public document, public/API/schema impact, start dependency,
integration dependency, completion gate, named fixture or vertical, exact
tests and oracle, non-goals, and expected completion evidence. Parent Issues
are indexes and closure aggregators and do not carry `ready-for-agent`.

For task status, authorization endpoints and decision/implementation completion,
see [Task Collaboration](Task-Collaboration.md).

## Update rule

Update this snapshot when the audited baseline, current milestone, critical
path, or blocked reason changes. Ordinary implementation details remain in the
owning Issue and tests. Every status claim must cite completed code and tests;
an unchecked item does not define current behavior.

## Accepted development direction, 2026-09-05

The maintainer explicitly accepted the adjusted direction in this task:
[S1 images/ordinary parameters, S2 CPU regions, S3 cache/interaction, S4 native
GPU and S5 measured optimization](Refactor-Development-Plan.md). Float32 is an
accepted S1 goal. Daemon features are demand-driven; compatibility maintenance
continues. Decision delivery is tracked by
[#256](https://github.com/kevin-zf1123/photospider/issues/256).

ADR 0016 remains the S1 source/binding/profile contract. ADR 0017 replaces its
whole-storage/output, whole-image scan and modeled-budget clauses for S2.
Daemon compatibility maintenance consumes installed 0.6; new bindings and bulk
transport remain demand-driven work in the daemon repository.
