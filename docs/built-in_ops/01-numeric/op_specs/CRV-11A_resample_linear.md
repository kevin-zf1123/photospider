---
spec_schema_version: 1
id: CRV-11A
parent_id: CRV-11
function: resample_linear
proposed_template_names:
  - curve.resample_linear
category: 01-numeric
kind: composite_workflow
status: Proposed
document_maturity: D1_draft
implementation_status: implemented
clarification_status: complete
---

# CRV-11A: resample_linear

Numeric profile: strict retains the exact reference defined below. Floating
arithmetic in accelerated profiles follows the shared
[final FP32 four-ULP contract](NUM_accelerated_contract.md), including its
range/fallback rules. Discrete results, copies, selected endpoints and special
values remain exact.

## Interface and expansion

Ordered dynamic bindings are positions[K], values[K], new_positions[N].
Export samples[N] and positions[N]. This template binds these inputs to
[the corresponding interpolator](CRV-01A_interpolate_linear.md) as x,y,query. All float dtype mixing,
counts, model-free semantics, query order/repetition and finite samples rules
are inherited.

Static profile is strict/apple_silicon/x86_64, default strict. dtype and
out_of_domain are the corresponding interpolation parameters/defaults.
Strict samples are whole-formula correctly rounded; accelerated samples obey
the shared final FP32 bound, retaining exact selected samples and endpoints.
Domain extrapolation has precisely the selected source interpolator's meaning.
No filtering, implicit uniform sampling or periodic extension is performed.

## Output-specific execution

[CRV-11's template contract](CRV-11_resample_signal.md) is normative. Samples use fixed curve tasks, with complete x, selected query rows and complete y rows for the selected columns. Physical support is closed through each source grid. Finite consumed samples, ordered knots, reject-domain queries and representable outputs are caller preconditions. Only selected output tiles are computed and allocated.

Positions independently copies requested new_positions tiles through `core.identity`, preserving descriptor, dtype and raw special bits. Its separate allocations retain ownership beyond context lifetime. Position-only selection has no old positions/values dependency. Samples and positions belong to different IR calls; they retain each call's retirement outcome. Joint exports require a successful Run. Rebinding, source failures, resource admission and cancellation follow the expanded operations.

## Acceptance and status

Use the corresponding CRV-01 analytic fixtures for samples and exact byte
comparison against new_positions for positions. Test nonfinite positions-only
forwarding independently of valid samples demand, partial indices,
output-specific static dependencies, both outputs jointly/separately, profile/dtype mixing,
strides, context-lifetime ownership, low budgets and cancellation. Use a temporary public workflow that expands this template and requests
samples/positions through Compiler/ExecutionContext.

## Maintained implementation and validation

This template uses the public helper in `photospider/ops/numeric/resampling.hpp`
and ordinary workflow composition. Temporary acceptance inherits CRV-01 exact-copy
and numerical-accuracy checks and adds composition checks; it does not require
a separate resampling formula oracle. Per-value numerical/fallback counters are unavailable.
