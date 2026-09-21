---
spec_schema_version: 1
id: CRV-11C
parent_id: CRV-11
function: resample_linear_multi
proposed_template_names:
  - curve.resample_linear_multi
category: 01-numeric
kind: composite_workflow
status: Proposed
document_maturity: D1_draft
implementation_status: implemented
implementation_branch: numeric-optimize
implementation_base_commit: eb0e90c8
implementation_updated: 2026-09-21
clarification_status: complete
repository_branch: ops-specs
repository_commit: 6617c78c
---

# CRV-11C: resample_linear_multi

Numeric profile: strict retains the exact reference defined below. Floating
arithmetic in accelerated profiles follows the shared
[final FP32 four-ULP contract](NUM_accelerated_contract.md), including its
range/fallback rules. Discrete results, copies, selected endpoints and special
values remain exact.

## Interface and expansion

Ordered dynamic bindings are positions[K], values[K,C], new_positions[N].
Export samples[N,C] and positions[N]. This template binds these inputs to
[the corresponding interpolator](CRV-01C_interpolate_linear_multi.md) as x,y,query. All float dtype mixing,
counts, model-free semantics, query order/repetition and finite samples rules
are inherited. C=1 retains its second dimension; channels have no implicit color meaning.

Static profile is strict/apple_silicon/x86_64, default strict. dtype and
out_of_domain are the corresponding interpolation parameters/defaults.
Strict samples are whole-formula correctly rounded; accelerated samples obey
the shared final FP32 bound, retaining exact selected samples and endpoints.
Domain extrapolation has precisely the selected source interpolator's meaning.
No filtering, implicit uniform sampling or periodic extension is performed.

## Output-specific execution

[CRV-11's complete template contract](CRV-11_resample_signal.md) is normative.
Samples demand inherits the forward interpolator's Whole execution: complete
positions/values/new_positions collection, full output allocation, full input
invalidation and Run numerical failures. Mathematical column independence remains;
invalid undelivered queries or columns can fail the run. Empty reads no payload.
Positions output independently forwards requested new_positions with bitwise
dtype/descriptor/owner preservation, including floating special values; it does
not invoke the interpolator or read old positions/values. Normal upstream
validation is still preserved. Joint requests cannot broaden semantic dependencies.

Inherit explicit dirty support per output, immutable mapped fragments or aliases,
arbitrary strides, owners beyond context lifetime, budget accounting for source
and template state, cache-off, cancellation and upstream failures. No failed sample
is hidden by a successful positions output, and no positions-only observation
inherits an unrelated interpolation failure.

## Acceptance and status

Use the corresponding CRV-01 analytic fixtures for samples and exact byte
comparison against new_positions for positions. Test nonfinite positions-only
forwarding followed by failed samples demand, partial indices/columns,
independent dirty effects, both outputs jointly/separately, profile/dtype mixing,
strides, context-lifetime ownership, low budgets and cancellation. The maintained public workflow expands this template and requests samples/positions
through Compiler/ExecutionContext; commands and current evidence are linked below.

## Maintained implementation and validation

This public resampling template is maintained through the corresponding helper
in `photospider/numeric/resampling.hpp` and ordinary workflow composition. See
[signal-resampling](../../../../examples/numeric_workflow/README.md#signal-resampling)
and the [CRV-11 umbrella](CRV-11_resample_signal.md) for commands and shared
validation evidence. Native Clang21 Strict/Apple validation for the Whole revision is recorded in
that workflow and the math implementation notes. WSL/AVX2 and installed-package
consumers have not been rerun. Whole numerical/fallback counters are N/A.
