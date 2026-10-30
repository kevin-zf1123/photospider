---
spec_schema_version: 1
id: RES-02
kind: family_contract
category: 05-filter
status: Proposed
document_maturity: D1_draft
implementation_status: implemented
parent_id: 05-filter
members:
- RES-02A
- RES-02B
---

# RES-02: Non-Local Means

A per-pixel estimate over a finite search window. No additional block-estimate aggregation is included, so no unsupported halo locality is implied.

This family inherits the [FILTER common contract](FILTER_common_contract.md), [RES native floating-point contract](RES_native_contract.md), and [FMT](../../02-format-color/op_specs/FMT_common_contract.md).

## Members

| ID | Function Spec | Kind | Numeric Reference |
| --- | --- | --- | --- |
| RES-02A | [Plain NLM](RES-02A_nlm_plain.md) | primitive | F |
| RES-02B | [Noise-corrected NLM](RES-02B_nlm_noise_corrected.md) | primitive | F |

Suffix letters denote distinct mathematical/interface objects, not quality tiers. Each primitive uses one native fp32/fp64 key.

## Interface

The keys are `restoration.nlm_plain` (ports `input, guide`) and `restoration.nlm_noise_corrected` (ports `input, guide, noise_sigma`). Each has one `output` Result containing one Tensor member `samples`, with schema ID `photospider.tensor`, version 1.

| Parameter | Type | Required value / meaning |
| --- | --- | --- |
| `y_axis`, `x_axis` | Int64 | Distinct indices in the logical sample shape |
| `component_axis` | Int64 | `-1` for a scalar guide, otherwise a nonspatial logical axis |
| `guide_components` | Int64 | G, from 1 through 4 |
| `patch_radius` | Int64 | r >= 0 |
| `search_radius` | Int64 | s >= 0 |
| `h` | Float64 | Positive smoothing scale; converted once to the working dtype |
| `metric_scale` | String | Exactly G comma- or whitespace-separated real numbers; each is a positive finite scale representable in the working dtype |
| `max_work` | Optional Int64 | Per-invocation resource ceiling, default INT64_MAX |

All mathematical parameters are explicit. Positivity, finiteness and representability are caller numerical premises. Parameter presence, type, metric-list syntax and length, axes, nonnegative integral radii and schema compatibility are checked statically.

Input and guide are Float32 or Float64 with the same dtype and logical rank (2 through the Tensor rank limit of 8). With `component_axis=-1`, G must be 1 and all logical sample extents match. Otherwise the guide component extent is G; the corresponding input extent is the number of independently filtered input components and need not equal G. Every other sample extent matches. Nonspatial coordinates select independent planes, with guide components shared across input-component planes. Logical axes may be nonadjacent or reversed. The output preserves the input Tensor descriptor, layout and batch metadata. No RGB, alpha or color conversion is inferred.

Member B's sigma is a single Float64 Tensor with sample shape `{1}`, invariant across all planes, converted once to the working dtype per invocation.

## Mathematics

### Patch, Search, and Boundary

For each output q, candidate p ranges over the in-image square search window of radius s; out-of-image candidates are not reflected into extra candidates. Patch offsets d in [-r,r]^2 access q+d and p+d using reflect_half. The guide and filtered input may differ. The G guide components and metric_scale are explicit, with G=1..4. The native input-dtype distance is D(q,p)=sum_(d,c) ((g(q+d,c)-g(p+d,c))*scale_c)^2 / ((2r+1)^2*G). The self-weight is 1; other weights use the member-defined native exponential. The output is RN_t(sum_p w(q,p)*input[p]/sum_p w). No maximum-neighbor rule replaces the self-weight, and there is no extra block-overlap aggregation. A zero patch radius remains nonlocal patch similarity, not identity; a zero search radius is explicitly identity.

## Behavior

With s=0, both members perform a bit copy of the input and read no guide or sigma payload.

Each output task uses full-image coordinates. Input support contains only in-image candidate centers within s of the selected output rectangle; guide support extends those centers by r with `reflect_half` against the full image. Output selection never creates an image boundary.

## Numeric Requirements

All members use numeric class F under [RES native floating-point contract](RES_native_contract.md). The input dtype selects native fp32 or fp64 arithmetic and stored state. Caller domain premises remain unchecked; shape, support, copy branches and algorithmic stages remain normative. There is no strict/accelerated quality split or NUM four-ULP fallback.

Alpha semantics, spatial extension and structured outputs follow [color composition](FILTER_color_composition.md), the [boundary contract](FILTER_boundary_contract.md) and the [collection contract](FILTER_collections_contract.md).

## Behavior and Acceptance

Each member's support set is normative. Having a halo does not imply independent execution on an arbitrary cropped ROI. Staged rounding, global connectivity, transforms, and iterative state follow each member's definition. Outputs are independently requestable. Members are accepted against the [acceptance protocol](FILTER_oracle_protocol.md).

## References

[S18 · Buades, Coll, Morel: Non-Local Means Denoising](../research-sources.md#s18)
