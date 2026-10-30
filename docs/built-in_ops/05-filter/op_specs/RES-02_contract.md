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

Both members are registered CPU operations with native Float32/Float64 arithmetic. Specification status remains Proposed; implementation status and measured acceptance do not change that status. This family inherits the [FILTER common contract](FILTER_common_contract.md), [RES native floating-point contract](RES_native_contract.md), and [FMT](../../02-format-color/op_specs/FMT_common_contract.md).

## Members

| ID | Function Spec | Kind | Numeric Reference |
| --- | --- | --- | --- |
| RES-02A | [Plain NLM](RES-02A_nlm_plain.md) | primitive | F |
| RES-02B | [Noise-corrected NLM](RES-02B_nlm_noise_corrected.md) | primitive | F |

Suffix letters denote distinct mathematical/interface objects, not quality tiers that a backend may choose arbitrarily. Each primitive uses one native fp32/fp64 key; a helper orchestrates explicit algorithmic stages.

### Patch, Search, and Boundary
For each output q, candidate p ranges over the in-image square search window of radius s; out-of-image candidates are not reflected into extra candidates. Patch offsets d∈[-r,r]² access q+d and p+d using reflect_half. The guide and filtered input may differ. The G guide components and metric_scale are explicit, with G=1..4. Compute the native input-dtype distance D(q,p)=Σ_(d,c) ((g(q+d,c)-g(p+d,c))*scale_c)² / ((2r+1)²*G). The self-weight is 1; other weights use the member-defined native exponential. The output is RN_t(Σ_p w(q,p)*input[p]/Σ_p w). No maximum-neighbor rule replaces the self-weight, and there is no extra block-overlap aggregation. A zero patch radius remains nonlocal patch similarity, not identity; a zero search radius is explicitly identity.


## CPU Implementation and Public API

The registered keys are `restoration.nlm_plain` (ports `input, guide`) and
`restoration.nlm_noise_corrected` (ports `input, guide, noise_sigma`).
Each has one `output` Result containing one Tensor member `samples`, with
schema ID `photospider.tensor`, version 1.

| Parameter | Type | Required value / meaning |
| --- | --- | --- |
| `y_axis`, `x_axis` | Int64 | Distinct indices in the logical sample shape |
| `component_axis` | Int64 | `-1` for a scalar guide, otherwise a nonspatial logical axis |
| `guide_components` | Int64 | G, from 1 through 4 |
| `patch_radius` | Int64 | r ≥ 0 |
| `search_radius` | Int64 | s ≥ 0 |
| `h` | Float64 | Positive smoothing scale; converted once to the working dtype |
| `metric_scale` | String | Exactly G comma- or whitespace-separated real numbers; each is a positive finite scale representable in the working dtype |
| `max_work` | Optional Int64 | Existing per-invocation resource ceiling, default INT64_MAX |

All mathematical parameters are explicit. Positivity, finiteness and
representability are caller numerical premises; production performs no payload
domain scan. Parameter presence, type, metric-list syntax/length, axes,
nonnegative integral radii and schema compatibility are checked statically.

Input and guide are Float32 or Float64 with the same dtype and logical rank
(2 through the existing Tensor rank limit of 8). With `component_axis=-1`,
G must be 1 and all logical sample extents match. Otherwise the guide component
extent is G; the corresponding input extent is the number of independently
filtered input components and need not equal G. Every other sample extent
matches. Nonspatial coordinates select independent planes, with guide components
shared across input-component planes. Logical axes may be nonadjacent or
reversed; supported Tensor layouts are gathered through the existing typed view
interface. Output preserves the input Tensor descriptor, layout and batch
metadata. No RGB, alpha or color conversion is inferred.

Member B's sigma is a single Float64 Tensor with sample shape `{1}`, invariant
across all planes. Its value is converted once to the working dtype per
invocation. With s=0, both members perform a raw bit copy and declare no guide or
sigma payload reads; no patch workspace is allocated. Unused large patch radii
therefore do not cause patch-capacity rejection on this copy path.

## Resources and Runtime

Each output task uses full-image coordinates. Input support contains only
in-image candidate centers within s of the selected output rectangle; guide
support extends those centers by r with `reflect_half` against the full image.
The compiler closes the resulting finite support on input member tiles.
Output selection never creates an image boundary. An internal spatial-axis
exchange for a one-column image only changes traversal order.

For a non-copy task let Ai be the candidate-center rectangle area, Ag the
unclipped reflected-guide workspace area, and Ao the output rectangle area.
Scratch is exactly `sizeof(T) * (Ai + (G+2)*Ag + 2*Ao)` bytes, reused for each
nonspatial plane in that task. Published Result pages and input owners are
accounted separately by the existing runtime. Capacity products use checked
wide integers; unrepresentable coordinates, workspace or work produce
ResourceExhausted with ArithmeticOverflow, before unsafe address arithmetic.

A nonempty invocation admits one algorithm stage and precharges a conservative
finite work bound before gathering payloads. Write K=2r+1,
sy=min(s,H-1), sx=min(s,W-1), L=sy(2sx+1)+sx, and let Ho×Wo be the selected
output rectangle. The declared per-plane bound is:

```text
Ai + G*Ag + 2*Ao + 9*L*(Ho+2r)*(Wo+2r)*(3G+2K+40)
```

The invocation multiplies this by its independent-plane count and adds the
stage cost through the existing work service. For s=0 the charge is one stage
plus the number of copied samples. These are resource units, not a prediction
of hardware instructions. Callers must provision both Run and node budgets
for the requested shape; large NLM windows can exceed a default Run work limit.

Cancellation follows the [RES algorithmic-block contract](FILTER_common_contract.md#res-work-admission-and-cancellation), independently of the prepaid work bound. Gather, initialization and output loops use chunks of at most 256 samples; the s=0 copy checks every 128 samples. SIMD SSD, patch and weight/application loops check at 256-position boundaries, with bounded packet and scalar tails. Fixed patch codelets have 1, 3, 5, 7 or 11 taps; generic patch codelets additionally check at 256-tap boundaries inside each output packet or scalar sample. Scalar fallback loops retain their own bounded checks, and displacement and row traversal provide outer checkpoints.

The work between checks depends on tap count, guide components and SIMD width. For example, a 256-output, 11-tap SIMD patch block performs 2560 scalar additions between its entry check and the next block check. Generic tap loops retain their internal checks as the patch radius grows. Acceptance checks these block bounds and cancellation/drain behavior, and reports latency for the tested backend and workload.

Empty selections create no payload tasks and consume no stage. Each task owns its scratch and output; no Run data or mutable scratch survives in a process cache. The implementation uses the existing generation operation and v9 work service, with the existing v8 invocation path; it changes no kernel ABI, allocator, ownership rule or thread policy. CPU tasks use their assigned executor lanes and create no private worker pool.

## Shared Execution and Numeric Obligations

All members use numeric class F under [RES native floating-point contract](RES_native_contract.md). The input dtype selects native fp32 or fp64 arithmetic and stored state. Caller domain premises remain unchecked; shape, support, copy branches and algorithmic stages remain normative. There is no strict/accelerated quality split or NUM four-ULP fallback.

Positive-weight normalization, signed kernels, local regression, and global inverse problems have different semantics and must not be treated as one alpha-blurring operation. See [color composition](FILTER_color_composition.md). Spatial extension, anchors, and origins follow the [boundary contract](FILTER_boundary_contract.md); structured outputs follow the [collection contract](FILTER_collections_contract.md). Shape depends only on descriptors; each member defines mathematical support and fixed compiled reads.

## Dependencies and Acceptance

Each member’s support set is normative. Having a halo does not imply independent execution on an arbitrary cropped ROI; paging does not eliminate a whole-image dependency. Staged rounding, global connectivity, transforms, and iterative state must follow each member’s definition. Outputs are independently requestable. Only selected computation and required algorithmic control contribute payload reads; no payload-validation phase is introduced.

All members must pass the [common acceptance protocol](FILTER_oracle_protocol.md). Oracle reference scope is documented in the [oracle guide](../../../../oracle/ops/filter/README.md); it does not replace runtime acceptance. D1/D2 indicate draft maturity, not acceptance status.

## References and Conformance Notes

[S18 · Buades, Coll, Morel: Non-Local Means Denoising](../research-sources.md#s18)

Boundary rules, tie-breaking, numeric domains, public schemas, and proposed keys follow the applicable contracts. Algorithm sources do not establish parameter defaults or grant licenses to third-party software.
