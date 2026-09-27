---
spec_schema_version: 1
id: RES-02
kind: family_contract
category: 05-filter
status: Proposed
document_maturity: D1_draft
implementation_status: not_implemented
parent_id: 05-filter
members:
- RES-02A
- RES-02B
---

# RES-02: Non-Local Means

A per-pixel estimate over a finite search window. No additional block-estimate aggregation is included, so no unsupported halo locality is implied.

This specification defines the intended mathematics and interface contract only; it makes no claim about current implementation or registration. It inherits the [FILTER common contract](FILTER_common_contract.md), [NUM](../../01-numeric/op_specs/NUM_common_contract.md), and [FMT](../../02-format-color/op_specs/FMT_common_contract.md). No member has been marked Accepted or as a production implementation.

## Members

| ID | Function Spec | Kind | Numeric Reference |
| --- | --- | --- | --- |
| RES-02A | [Plain NLM](RES-02A_nlm_plain.md) | primitive | B+E |
| RES-02B | [Noise-corrected NLM](RES-02B_nlm_noise_corrected.md) | primitive | B+E |

Suffix letters denote distinct mathematical/interface objects, not quality tiers that a backend may choose arbitrarily. A primitive may propose strict and accelerated profiles; a helper only orchestrates explicit stages.

## Shared Mathematics, Units, and Profiles


### Patch, Search, and Boundary
For each output q, candidate p ranges over the in-image square search window of radius s; out-of-image candidates are not reflected into extra candidates. Patch offsets d∈[-r,r]² access q+d and p+d using reflect_half. The guide and filtered input may differ. The G guide components and metric_scale are explicit, with G=1..4. The exact distance is D(q,p)=Σ_(d,c) ((g(q+d,c)-g(p+d,c))*scale_c)² / ((2r+1)²*G). The self-weight is 1; other weights are member-defined baked64 values. The output is RN_t(Σ_p w(q,p)*input[p]/Σ_p w). No maximum-neighbor rule replaces the self-weight, and there is no extra block-overlap aggregation. A zero patch radius remains nonlocal patch similarity, not identity; a zero search radius is explicitly identity.


## Shared Execution and Numeric Obligations

E denotes evaluation of the exact expression followed by one final RN_t; B denotes explicitly generated, fixed coefficients; S denotes explicitly staged rounding. See the [numeric reference](FILTER_numeric_reference.md). Ordinary arithmetic propagates NaN, Inf, floating-point overflow, signed zero, payload, rounding, and precision according to the corresponding NUM operation. Members must still validate their actual parameter, control-model, and solution-domain constraints. Strict bits, final FP32-scaled four-ULP bounds, gradual underflow, copy behavior, and exact discrete branches inherit NUM.

Positive-weight normalization, signed kernels, local regression, and global inverse problems have different semantics and must not be treated as one alpha-blurring operation. See [color composition](FILTER_color_composition.md). Spatial extension, anchors, and origins follow the [boundary contract](FILTER_boundary_contract.md); structured outputs follow the [collection contract](FILTER_collections_contract.md). Shape depends only on descriptors; actual dependencies and dirty propagation are declared by each member.

## Dependencies and Acceptance

Each member’s support set is normative. Having a halo does not imply independent execution on an arbitrary cropped ROI; paging does not eliminate a whole-image dependency. Staged rounding, global connectivity, transforms, and iterative state must follow each member’s definition. Outputs are independently requestable; validating payloads of an unrequested component must not introduce a hidden whole-image dependency.

All members must pass the [common acceptance protocol](FILTER_oracle_protocol.md). Oracle reference scope is documented in the [oracle guide](../../../../oracle/ops/filter/README.md); it does not replace runtime acceptance. D1/D2 indicate draft maturity, not acceptance status.

## References and Conformance Notes

[S18 · Buades, Coll, Morel: Non-Local Means Denoising](../research-sources.md#s18)

Boundary rules, tie-breaking, numeric domains, public schemas, and proposed keys follow the applicable contracts. Algorithm sources do not establish parameter defaults or grant licenses to third-party software.
