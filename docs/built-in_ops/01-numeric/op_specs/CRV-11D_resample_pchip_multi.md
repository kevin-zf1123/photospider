---
spec_schema_version: 1
id: CRV-11D
parent_id: CRV-11
function: resample_pchip_multi
proposed_template_names:
  - curve.resample_pchip_multi
category: 01-numeric
kind: composite_workflow
status: Proposed
document_maturity: D1_draft
implementation_status: implemented
clarification_status: complete
---

# CRV-11D: resample_pchip_multi

Numeric profile: strict retains the exact reference defined below. Floating
arithmetic in accelerated profiles follows the shared
[final FP32 four-ULP contract](NUM_accelerated_contract.md), including its
range/fallback rules. Discrete results, copies, selected endpoints and special
values remain exact.

## Interface and expansion

Ordered dynamic bindings are positions[K], values[K,C], new_positions[N].
Export samples[N,C] and positions[N]. This template binds these inputs to
[the corresponding interpolator](CRV-01D_interpolate_pchip_multi.md) as x,y,query. All float dtype mixing,
counts, model-free semantics, query order/repetition and finite samples rules
are inherited. C=1 retains its second dimension; channels have no implicit color meaning.

Static profile is strict/apple_silicon/x86_64, default strict. dtype and
out_of_domain are the corresponding interpolation parameters/defaults.
Strict samples correctly round the exact PCHIP formula; accelerated satisfies the final 4-ULP/shape/monotonicity contract and uses strict fallback where required.
Domain extrapolation has precisely the selected source interpolator's meaning.
No filtering, implicit uniform sampling or periodic extension is performed.

## Output-specific execution

[CRV-11's complete template contract](CRV-11_resample_signal.md) is normative.
Samples demand inherits the forward interpolator's Whole execution: typed and
upstream validation covers all active input Results, authorized Result windows
provide sample reads, full output allocation and input invalidation apply, and
numeric failures retain Run scope. Mathematical column independence remains;
invalid undelivered queries or columns can fail the run. Empty reads no payload.
Positions output independently forwards requested new_positions as a mapped
Result view, preserving dtype, descriptor, facets, owner and raw special bits.
It does not allocate a copied payload, invoke the interpolator or read old
positions/values. Normal typed/upstream validation still applies to the requested
query region. Joint requests cannot broaden semantic dependencies.

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

This template is maintained through the public helper in
`photospider/numeric/resampling.hpp` and ordinary workflow composition. The
resampling executable passes eight groups under each of Strict and Apple; it
retains CRV-01's exact-copy and numerical-accuracy checks and defines no separate
resampling oracle. The focused root `test_numeric_resampling_result` passes 1/1. Whole numerical/fallback counters are N/A.
