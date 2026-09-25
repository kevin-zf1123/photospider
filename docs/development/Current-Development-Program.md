# Current Development Program

- Snapshot date: 2026-09-25.
- Audited remote baseline: `ops@61517151c0d0d2a2c0a70f52965350130eeda3a0`,
  package 0.24.0. This snapshot does not audit or change `main`.
- Local reviewed implementation: `99cc11126759a59deeb91f55c56c3443ce1bd28a`,
  not pushed at this audit. The CTest maintenance described below is an
  additional local working-tree change, not a remote delivery claim.
- GitHub Issues own delivery status; Projects mirror their remaining scope.
  [Task Collaboration](Task-Collaboration.md) defines the authorization boundary.
  [ADR 0015](../adr/0015-breaking-product-boundary-scope-reset.md) retains product
  authority. Accepted specifications are not implementation evidence.

## Implemented baseline and local delta

The remote baseline contains the typed public compile/plan/execute pipeline,
independent outputs and Atomic joint execution, CPU regional and staged
execution, local derived caches, native Metal storage/dispatch and planar image
storage. Maintained format operations include channel extraction/assembly,
channel-editing composition, strict numeric conversion and metadata assignment.

The independent-results milestone [#302](https://github.com/kevin-zf1123/photospider/issues/302)
and leaves #303–#313 are closed. [PR #314](https://github.com/kevin-zf1123/photospider/pull/314)
merged into `ops` at `575a424d`; that milestone's review/CI/merge procedure is
historical completion evidence, not a new per-task workflow requirement.

Local `99cc1112` adds sealed planar preparation reuse, explicit BitwiseMapped
value relations, public run/copy helpers and existing-SIMD generic conversion.
Its version axes are independent: OperationTraits 18, semantic/physical v16,
C operation ABI 9, provider ABI 1, WorkflowDocument 3 and TDM4. Package remains
0.24.0 and C++ consumers must rebuild together. The optimizer is still
`optimizer-v5-canonical-noop`; these runtime optimizations do not implement
cross-node fusion or incremental compilation. See
[Compiler Version Contract](Compiler-Version-Contract.md).

## Correctness inventory audit

All 79 previously registered default CTests and the conditional SME test were
reviewed by behavior. The default inventory now has 78 entries:

- Retire the historical 13-key format-deletion checklist, preserving synthetic
  unknown-operation lookup/invoke/compile rejection in `test_compiler`.
- Rename five G4-labelled and three foundations-labelled registrations by their
  actual dependency, GPU, numeric, expression and filter behavior.
- Retire the invalid generic-image STMap fixture and its timing mode; retain the
  other sparse/dependency/cache workflows. Planar STMap remains unimplemented.
- Correct the GPU fixture to test ordinary cancellation and prior Protocol
  failure separately. Keep zero-publication assertions in both cases.
- Build every registered executable by default; record unavailable SME as a
  skip. Installed runtime coverage is limited to the nested run target's actual
  commands, not all optional consumer targets.

After these local changes, the complete static CTest suite records macOS
77 passed / 1 failed and FreeBSD 70 passed / 1 failed / 7 Metal skips, each out
of 78. The conditional SME test actually executes and passes on the local
Apple M5. These are local results, not remote CI status. The remaining
`test_execution` failure is dynamic opaque-facet propagation: generic Drop and
its PreserveInput chain are allowed by the cache contract, while invocation
validation compares runtime facets to static metadata. The test remains active;
its assertion was not weakened or removed. See
[Testing and Validation](Testing-and-Validation.md) for current entry points.

The earlier `99cc1112` native static/shared and sanitizer audit also recorded
baseline failures and a FreeBSD `__thr_calloc` TSan report reproducible without
Photospider. A narrow suppression is not an unfiltered TSan pass. LeakSanitizer
was unsupported. This CTest maintenance task does not reclassify those platform
limitations or claim a new sanitizer matrix.

## Remaining kernel programs

- Foundations [#139](https://github.com/kevin-zf1123/photospider/issues/139):
  operation/provider starter tooling and optional manifests under #143,
  #246/#247/#248 remain. Current correctness failures still require repair.
- Compiler [#145](https://github.com/kevin-zf1123/photospider/issues/145),
  [#147](https://github.com/kevin-zf1123/photospider/issues/147): #148 structured
  explain/validator delta, then #149 conservative passes, then #203 disposable
  incremental recompilation. S1 input-contract acceptance is already complete;
  #148 requires triage of its remaining implementation, not renewed S1 approval.
  #150 remains a later explicitly scoped transform program.
- Heterogeneous execution [#151](https://github.com/kevin-zf1123/photospider/issues/151),
  [#152](https://github.com/kevin-zf1123/photospider/issues/152): CPU regional
  leaves and #153/#154/#156 native residency, execution and diagnostics are
  delivered. #209 cost units and planner-consumed machine profiles remain.
- Calibration [#155](https://github.com/kevin-zf1123/photospider/issues/155):
  #212/#213 remain. Native raw measurements are available, but do not establish
  calibrated profiles or automatic placement. No optimization/calibration
  parent is closed because of generic SIMD throughput results.

Daemon Session/Job, IPC and WebUI delivery remain in their own repositories and
were not re-audited by this kernel test-inventory task. Historical G/S milestone
labels do not define current CTest names or restore retired image contracts.

## Update rule

Refresh this snapshot when the audited baseline, current milestone, critical
path or blocker changes. Cite actual code, tests and delivery location. Issues
retain their scope and history; Projects reflect their status. Do not infer
implementation from an accepted proposal or mark local-only work remotely
complete. Normal implementation details belong in the owning Issue and tests.
