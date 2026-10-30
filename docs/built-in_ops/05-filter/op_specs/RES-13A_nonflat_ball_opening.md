---
spec_schema_version: 1
id: RES-13A
kind: primitive
category: 05-filter
status: Proposed
document_maturity: D2_draft
implementation_status: not_implemented
parent_id: RES-13
function: nonflat_ball_opening
proposed_operation_keys:
- restoration.nonflat_ball_opening_strict
- restoration.nonflat_ball_opening_accelerated_apple_silicon
- restoration.nonflat_ball_opening_accelerated_x86_64
numeric_reference: B+S
oracle_scope: mathematical_reference
---

# RES-13A: nonflat_ball_opening

Background estimation with a non-flat spherical structuring element.

Inherits the [RES-13 family contract](RES-13_contract.md), [FILTER common contract](FILTER_common_contract.md), [NUM numerical/precision contract](../../01-numeric/op_specs/NUM_common_contract.md), and [FMT metadata/straight-color semantics](../../02-format-color/op_specs/FMT_common_contract.md). RN, underflow, and accelerated error baselines are inherited and may not be weakened.

## Interface

Operation keys: `restoration.nonflat_ball_opening_strict`, `restoration.nonflat_ball_opening_accelerated_apple_silicon`, `restoration.nonflat_ball_opening_accelerated_x86_64`.

### Ports

Single-plane `input` → `background`.

### Parameters

Integer radius r ≥ 1; height h > 0 in input-intensity units; axes; boundary=reflect_half; disk footprint satisfying dx²+dy² ≤ r².

Inherit NaN, Inf, overflow, signed-zero, payload, rounding, and precision semantics from the corresponding NUM operation. Callers guarantee the member's finite-parameter, control-model and numerical-domain premises; production execution does not validate those premises. Copy/bypass and unconsumed data follow their dependency contracts. Every parameter must be supplied explicitly; constructors provide no defaults, except constants fixed by a named mathematical profile. Real parameters denote their stored Float64 values; decimal text is not treated as an exact real.

## Mathematics

Numeric classification: **B+S**. See [FILTER_numeric_reference](FILTER_numeric_reference.md) for the meanings of E/B/S.

Define b(d)=RN64(h*(sqrt(1-(dx²+dy²)/r²)-1)) ≤ 0, with b(0)=0 and b=-h at the edge. First compute erosion e(q)=RN_t(min_d(E(I,q+d)-b(d))), then dilation B(q)=RN_t(max_d(E(e,q-d)+b(d))). This profile is named nonflat_ball_opening_v1 with explicit min/max and boundary semantics; it does not claim bitwise equivalence to every ImageJ or scikit-image rolling-ball implementation.

## Behavior

The two stages have a worst-case support radius of 2r. Extend the erosion over the full logical domain for the second stage; do not crop it to the requested ROI.

ROI and tile edges are not image boundaries; boundary coordinates use the complete logical image.

## Acceptance

Constants are preserved; a small bright spot may be removed. Spatial radius r is distinct from intensity height h. Verify single-pixel boundary behavior. Test anti-extensivity at general boundaries under this definition rather than assuming the infinite-domain result applies.

## References

[S24 · scikit-image rolling-ball example](../research-sources.md#s24)
