---
spec_schema_version: 1
id: GRD-19A
parent_id: GRD-19
function: bilateral_log_base
kind: primitive
category: 07-grade
status: Proposed
document_maturity: D1_draft
implementation_status: implemented_cpu_fixed_abi_9
clarification_status: complete
execution: tile
proposed_operation_keys:
  - grade.bilateral_log_base
---

# GRD-19A: Bilateral base of log luminance

Inherits the [family contract](GRD-19_shadow_highlight_local_contract.md) and [GRD common contract](GRD_common_contract.md). The proposed tile operation `grade.bilateral_log_base` performs the transformation below.

## Interface

`luminance: T[H,W]` -> `base: Float64[H,W]`.

Y>=0. Static radius:Int64 0..4096 default 3, sigma_space:Float64>0 default 2 pixels, sigma_range:Float64>0 default 1 stop, epsilon:Float64>0 default 2^-16.

Port ordering, tensor members, calculation dtype, channel selection, static parameter types and unchecked premises follow the common contract. Output shape and dtype are those in the signature.

### Static parameters

| Parameter | Type | Meaning |
| --- | --- | --- |
| `radius` | Int64 | Cropped square radius in pixels, 0..4096; default 3. |
| `sigma_space` | Float64 | Positive finite spatial standard deviation in pixels; default 2. |
| `sigma_range` | Float64 | Positive finite range standard deviation in stops; default 1. |
| `epsilon` | Float64 | Positive finite luminance floor; default 2^-16. |
| `max_work` | Int64 | Optional per-invocation work limit, minimum zero; default 9223372036854775807. |

## Mathematics

z_p=log2(max(Y_p,epsilon)). For q in the cropped square radius neighborhood, w_pq=exp(-distance(p,q)^2/(2*sigma_space^2)-(z_p-z_q)^2/(2*sigma_range^2)). base_p=sum(w_pq*z_q)/sum(w_pq). Use Float64 sums. Equal neighborhood z values select that z.

### Current native algorithm/profile

The positive native vector log2 normalizes the mantissa to [3/4,3/2], chooses the nearest binary center `c=3/4+j/128`, and evaluates `log2(Y)=e+log2(c)+log1p(r)/ln(2)` with `r=(m-c)/c`. Rounded Float64 center/reciprocal tables and a degree-seven alternating log1p polynomial retain the common native accuracy criteria. Subnormal inputs are scaled by an exact power of two before normalization. A center at one avoids cancellation near log2(1). The same formula is used for full and tail packets.

Publish `z=log2(max(Float64(Y),epsilon))` in Float64. For each output coordinate, enumerate the cropped square in increasing global y, then x. Let `d=z_q-z_p`, `sx=(x_q-x_p)/sigma_space`, `sy=(y_q-y_p)/sigma_space`, and `q=d/sigma_range`. Equal published log coordinates define `d=0`, including equal infinities produced inside C/D. Form `w=exp(-0.5*sy*sy-0.5*sx*sx-0.5*q*q)` and accumulate Float64 `num += w*d`, `den += w`; return `z_p+num/den`. This centered weighted mean is algebraically the stated bilateral base and returns the published center exactly for a constant neighborhood. The center contributes `den += 1` at its original position in the accumulation order. Radius zero copies the published log value. Normal finite reciprocal scaling may implement division; extreme scales retain division so zero differences remain zero. A spatial exponent of negative infinity skips the neighbor before forming its log difference or weighted product, including when this algorithm is used inside C/D and luminance extraction has overflowed.

The native vector exponential reduces a regular argument to `k*ln(2)/64+r`, using rounded Float64 `2^(j/64)` and a degree-five polynomial for `exp(r)`, with `|r|<=ln(2)/128`. Bilateral exponents in [-700,0] use this reduction; other arguments use the full-domain native exponential, including subnormal results. This evaluates continuous weights without quantizing luminance, range differences or the neighborhood. The common native floating comparison criteria remain unchanged. The logarithm and weight evaluation for a sample is independent of packet length and of exceptional values in other samples.

Symmetric weights may be retained in a bounded ring of forward-neighbor rows: `w(p,q)=w(q,p)`. Reverse neighbors reuse the corresponding stored weight. Each output still accumulates in its original global y/x order; cache admission and fallback do not alter the cropped support or constant-neighborhood rule.

## Behavior

H(radius) on Y. Hidden straight colors participate when luminance is derived from an image. Tile boundaries retain full-image neighborhoods.

## Acceptance

Constant Y=0.25 gives base=-2. Radius zero gives log2(max(Y,epsilon)). Compare cached and direct observations with a spatial scale small enough to make non-center spatial exponents negative infinity.

Apply the common native-versus-strict comparison rules and compare ROI/tile observations at identical global coordinates. Reference equations and source attribution are in the family contract and [research sources](../research-sources.md).
