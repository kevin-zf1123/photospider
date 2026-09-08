# Current Development Program

- Snapshot date: 2026-09-09
- Audited implementation baseline: `d85e7b8` (S2), following `main@70b760f`
- Current milestone: S2 CPU regional execution and installed daemon consumption

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

S1 is settled at `main@70b760f`. S2 implements the accepted
[ADR 0017](../adr/0017-cpu-regional-execution-and-storage.md) in kernel 0.4.0,
OperationTraits 4 and operation ABI 4. C++17, source schema 2 and provider ABI 1
remain. The ordered local commits are:

| Issue | Implementation | Commit |
| --- | --- | --- |
| [#263](https://github.com/kevin-zf1123/photospider/issues/263) | Accepted research and storage/execution contract | `f0c1ae0` |
| [#264](https://github.com/kevin-zf1123/photospider/issues/264) | Regional Value storage, host buffers, ABI4 and installed consumers | `cd9bdcc` |
| [#210](https://github.com/kevin-zf1123/photospider/issues/210) | Completion-owned allocation and bounded workspace | `b3cbc52` |
| [#211](https://github.com/kevin-zf1123/photospider/issues/211) | Lazy tile plans and static halo specialization | `40dfd69` |
| [#265](https://github.com/kevin-zf1123/photospider/issues/265) | Regional sources, collection and ordered streaming | `d9f4032` |
| [#266](https://github.com/kevin-zf1123/photospider/issues/266) | Gaussian/exposure/mask/source-over public vertical | `3b08b41` |
| [daemon #15](https://github.com/kevin-zf1123/photospider-daemon/issues/15) | Installed kernel 0.4 consumption with existing IPC subset | `53ec2ca` (daemon) |

Independent comprehensive review corrections are in `d85e7b8`; fixed fuzz-seed
migration is in `921ad5c`. Direct invocation shares checked demand derivation,
Whole chains release completed ancestors, and C image address/clamp arithmetic
has dedicated regression coverage. Computed-mask errors retain operation-failure
classification. No outstanding blocker/required was found in the local recheck.

`S2Image.RegionAndTiles` runs through public APIs in C++ and the maintained C
module. Whole/tiled outputs are bitwise equal and match an independent 2D oracle.
The 65536² source example processes a 5x7 ROI in nine tiles with 9900 source bytes,
1808 actual peak bytes and a 3840-byte conservative reservation. Exact/one-byte-
short budgets, malformed views, Whole chains, fan-out, concurrent snapshots,
source/sink failures and cancellation/currentness have focused coverage.

Local static/shared kernel validation covers all 15 registered tests across the
integration pass and focused correction reruns, including isolated installed
consumers. Daemon static/shared validation covers 15 tests each. The C module's
focused UBSAN no-recover example also passes. Protected Linux/macOS static/shared,
ASAN/TSAN, Codex bot review, merge and final settlement are recorded in the linked
Issues and PRs; local validation alone is not the delivery gate.

Project #9 mirrors these CPU leaves; daemon #15 mirrors Project #15. #152 stays
open for native-device scope, with #209 machine-cost calibration and #153/#154
outside this CPU milestone. No HEX or MED parent is closed by the S2 CPU subset.
S3 cache/interaction and native GPU work remain separately scoped future work.

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
- [#151](https://github.com/kevin-zf1123/photospider/issues/151) remains gated
  on explicit device storage/access, cost, liveness, transfer, residency,
  fallback, and real operation-vertical decisions.
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
Daemon compatibility maintenance consumes installed 0.4; new bindings and bulk
transport remain demand-driven work in the daemon repository.
