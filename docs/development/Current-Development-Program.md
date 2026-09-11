# Current Development Program

- Snapshot date: 2026-09-12
- Delivery baseline: `ops@ffc5d0e297b9d0ea136975413d3443e23e6fa458`, package 0.8.0 / ABI 8
- Current milestone: [independent results and Atomic joint execution #302](https://github.com/kevin-zf1123/photospider/issues/302)
- Branch: `codex/multi-output-semantics` into `ops`; no main or daemon delivery

## Authority and current state

GitHub Issues own live delivery status. [ADR 0021](../adr/0021-independent-node-results.md)
records the maintainer-approved target; acceptance does not establish implementation.
The baseline contains the operation foundations and G4 execution capabilities.
Earlier main@fba06270 / ops-foundations snapshots describe historical milestones.
This snapshot does not audit or change main. ADR 0015 retains product authority.

## Ordered implementation leaves

| Order | Issue | Acceptance |
| --- | --- | --- |
| M0 | [#303](https://github.com/kevin-zf1123/photospider/issues/303) | Research/contracts |
| M1 | [#304](https://github.com/kevin-zf1123/photospider/issues/304) | Output traits and ABI 9 |
| M2 | [#305](https://github.com/kevin-zf1123/photospider/issues/305) | Multi-output compilation and planning |
| M3 | [#306](https://github.com/kevin-zf1123/photospider/issues/306) | Per-output execution/dependencies/caches |
| M4 | [#307](https://github.com/kevin-zf1123/photospider/issues/307) | Per-observation C/C++ outcomes |
| M5 | [#308](https://github.com/kevin-zf1123/photospider/issues/308) | Atomic joint scheduling |
| M6 | [#309](https://github.com/kevin-zf1123/photospider/issues/309) | Plane semantics and 420 |
| M7 | [#310](https://github.com/kevin-zf1123/photospider/issues/310) | Horizontal crop outputs |
| M8 | [#311](https://github.com/kevin-zf1123/photospider/issues/311) | Independent channel convolutions |
| M9 | [#312](https://github.com/kevin-zf1123/photospider/issues/312) | Fractional-radius Gaussian image/kernel |
| M10 | [#313](https://github.com/kevin-zf1123/photospider/issues/313) | Installed workflows and combined acceptance |

Each leaf depends on the preceding slice and has a separate validated commit.
Implementation commits M0–M9 are `fd796975` through `9193d9ac`. M10 adds the
[installed public workflow](../../examples/multi_output_workflow/README.md),
its joint on/off consumer gate, and shared-view compaction for parameter-sized
kernels. The local static/shared builds, thirteen focused tests per linkage,
and installed consumers passed on 2026-09-12; M10's final thirteen-check runs per linkage also passed after
the compaction fix. The installed static maximum-radius workflow produced the
complete 129×129 matrix, matched the independent kernel/convolution oracles,
and recorded zero image reads for its kernel-only run. Public delivery is pending the following gates. Final delivery requires a
fresh independent comprehensive review, required fixes, one PR to ops, all six
existing CI jobs and Codex bot review on the final HEAD, a merge commit, explicit
Issue settlement, local ops synchronization and task-branch cleanup. Only the
kernel repository is in scope. Dynamic outputs, RequestRecord joint execution,
new Metal algorithms, daemon migration and #206 channel pruning remain separate.

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
