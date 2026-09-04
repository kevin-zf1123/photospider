# Current Development Program

- Snapshot date: 2026-09-04
- Audited implementation baseline: `main@703569bb74164f061b233f9edc2c0b964bc868fb`
- Current milestone: S1 reusable input-to-result vertical

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

The latest baseline CI was
[`kernel-ci` run 68](https://github.com/kevin-zf1123/photospider/actions/runs/33738054894).
It passed on Linux and macOS for static and shared kernels, plus ASAN and TSAN.

## Current milestone

S1 makes a compiled graph reusable with caller-owned runtime Values. The
milestone separates graph input declarations, compile-time facts, per-run
bindings, output demand, and result transport.

### Critical path

1. [#256](https://github.com/kevin-zf1123/photospider/issues/256)
   freezes `WorkflowInputDeclaration`, `ExecutionBindings`, validation,
   identity, output-demand, Value lifetime, and the minimum element vocabulary.
2. [#257](https://github.com/kevin-zf1123/photospider/issues/257)
   implements the accepted public contract with focused negative and installed
   consumer coverage.
3. [#258](https://github.com/kevin-zf1123/photospider/issues/258)
   adds one real input Value, operation chain, named output, and independent
   correctness oracle across repeated executions of one compiled plan.
4. The daemon projects the accepted kernel contract into per-Job bindings and
   an ephemeral bulk-result path. The daemon program records its own Issues and
   lifecycle constraints.

The kernel contract and daemon bulk-transport decision may proceed in
parallel. Kernel implementation begins after the contract decision. The final
daemon vertical begins after both kernel execution and daemon transport
contracts are complete.

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

## Update rule

Update this snapshot when the audited baseline, current milestone, critical
path, or blocked reason changes. Ordinary implementation details remain in the
owning Issue and tests. Every status claim must cite completed code and tests;
an unchecked item does not define current behavior.
