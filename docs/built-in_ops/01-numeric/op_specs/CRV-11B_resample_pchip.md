---
spec_schema_version: 1
id: CRV-11B
parent_id: CRV-11
function: resample_pchip
proposed_template_names:
  - curve.resample_pchip
category: 01-numeric
kind: composite_workflow
status: Proposed
document_maturity: D1_draft
implementation_status: not_implemented
clarification_status: complete
repository_branch: ops-specs
repository_commit: 6617c78c
---

# CRV-11B: resample_pchip

## Interface and expansion

Ordered dynamic bindings are positions[K], values[K], new_positions[N].
Export samples[N] and positions[N]. This template binds these inputs to
[the corresponding interpolator](CRV-01B_interpolate_pchip.md) as x,y,query. All float dtype mixing,
counts, model-free semantics, query order/repetition and finite samples rules
are inherited.

Static profile is strict/apple_silicon/x86_64, default strict. dtype and
out_of_domain are the corresponding interpolation parameters/defaults.
Strict samples correctly round the exact PCHIP formula; accelerated satisfies the final 4-ULP/shape/monotonicity contract and reports strict fallback where required.
Domain extrapolation has precisely the selected source interpolator's meaning.
No filtering, implicit uniform sampling or periodic extension is performed.

## Output-specific execution

[CRV-11's complete template contract](CRV-11_resample_signal.md) is normative.
Samples demand inherits the forward interpolator's global source-position
validation and local query/value support.
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
forwarding followed by failed samples demand, partial indices,
independent dirty effects, both outputs jointly/separately, profile/dtype mixing,
strides, context-lifetime ownership, low budgets and cancellation. A conceptual
public workflow expands this template and requests samples/positions through
Compiler/ExecutionContext; actual run commands and verified output remain required
for delivery. This specification does not claim current template support.
