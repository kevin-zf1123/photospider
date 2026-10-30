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
implementation_status: implemented
clarification_status: complete
---

# CRV-11B: resample_pchip

Numeric profile: strict retains the exact reference defined below. Floating arithmetic in accelerated profiles follows the shared [final FP32 four-ULP contract](NUM_accelerated_contract.md). Discrete results, copies, selected endpoints and special values remain exact.

## Interface

Ordered dynamic bindings are positions[K], values[K], new_positions[N]. Exports are samples[N] and positions[N]. This template binds these inputs to [the corresponding interpolator](CRV-01B_interpolate_pchip.md) as x,y,query. All float dtype mixing, counts, model-free semantics, query order/repetition and finite samples rules are inherited. The authoring helper is in `resampling.hpp`.

Static profile is strict/apple_silicon/x86_64, default strict. dtype and out_of_domain are the corresponding interpolation parameters/defaults. Strict samples correctly round the exact PCHIP formula; accelerated satisfies the final 4-ULP/shape/monotonicity contract. Domain extrapolation has precisely the selected source interpolator's meaning. No filtering, implicit uniform sampling or periodic extension is performed.

## Behavior

[CRV-11's template contract](CRV-11_resample_signal.md) is normative. Each samples element depends on all positions, its new_positions row and the values rows (columns for the selected function) per the interpolator. Finite consumed samples, ordered knots, reject-domain queries and representable outputs are caller preconditions.

Positions copies the requested new_positions elements through `core.identity`, preserving descriptor, dtype and raw special bits. Position-only requests have no old positions/values dependency. Samples and positions are separate exports.

## Acceptance

The corresponding CRV-01 analytic fixtures apply for samples, and positions compare byte for byte with new_positions. Nonfinite positions-only forwarding is tested independently of valid samples demand, including partial indices, both outputs jointly and separately, and profile/dtype mixing.
