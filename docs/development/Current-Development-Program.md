# Current Development Program

- Snapshot date: 2026-09-09
- Audited foundation: `main@54d57f3` (settled S2); S3 delivery tracked by #268
- Current milestone: S3 cache, frozen execution and interactive image workflows

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

S2 is settled by [kernel PR #267](https://github.com/kevin-zf1123/photospider/pull/267)
at `main@54d57f3`, with companion daemon PR #16. Its regional storage, bounded
allocation and Gaussian/mask/composition fixture remain the S3 foundation.

[S3 #268](https://github.com/kevin-zf1123/photospider/issues/268) implements the
accepted [ADR 0018](../adr/0018-local-result-caches-and-frozen-execution.md):
package 0.5 / operation ABI and traits 5, immutable tiled inputs, frozen exports,
regional result sharing, box/brush operations, application-owned preview policy
and disposable cross-restart disk regions. C++17, schema 2 and provider ABI 1
remain. Ordered leaf Issues are #269 through #277; daemon #17 consumes the
installed package and preserves its IPC subset.

The runnable [S3 workflow guide](../kernel-architecture/S3-Workflow.md) records
public entrypoints and independent oracles. GitHub Issues own current local
validation, independent review, required CI/bot fixes and protected merge status.
This snapshot does not turn accepted contracts or local commits into a claim
of remote settlement. Native GPU and incremental compiler #203 remain outside
this milestone; broader HEX/MED parents are not closed by S3.

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
