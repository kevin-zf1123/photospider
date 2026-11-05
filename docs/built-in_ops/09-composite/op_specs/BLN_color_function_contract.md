---
spec_schema_version: 1
id: BLN-common
kind: shared_operator_contract
category: 09-composite
status: Proposed
document_maturity: D1_draft
implementation_status: partially_implemented
clarification_status: complete
---

# BLN: color functions and explicit image interpolation

Inherit [COM common](COM_common_contract.md). Most members are color-only functions with B=background and S=source. Alpha integration is separately expressed by CMP. The bounded/HDR domains and endpoint priority are specified by each member. `normal`, per-channel extrema and whole-color selection are bit-copy choices; ordinary arithmetic is native floating computation.

## Whole-color selection

BLN-17A/B receive linear RGB and compute its Float64 working-basis Y measurement. Resolve the basis Y row to Float64 coefficients r, then use w=r/sum(r), with positive sum(r). Coefficients may be signed. Publish each dot(w,RGB) measure to Float64 before comparison; the oracle normalizes the stored coefficients exactly and strictly rounds this measure. BLN-17C/D receive native OKLab and compare its stored L. BLN-17E/F receive native CIELAB and compare its stored l=L*/100. Each returns the selected complete tuple in the same input model. Explicit model-conversion nodes compose these choices into an RGB workflow; the selection node does not insert a conversion.

## Dissolve integer definition

Use unsigned 64-bit modular arithmetic. Define mix(v): z=v+0x9e3779b97f4a7c15; z=(z xor (z>>30))*0xbf58476d1ce4e5b9; z=(z xor (z>>27))*0x94d049bb133111eb; return z xor (z>>31). Truncate each arithmetic result to 64 bits. Start z=mix(seed interpreted modulo 2^64); for c in [global_x,global_y,frame], set z=mix(z xor (c modulo 2^64)). The result is U. Inputs seed/frame/coordinates are signed Int64 values. A fixed frame gives a stable spatial pattern; changing frame explicitly changes the pattern.

U/2^64 < source alpha chooses opaque source color; otherwise retain background color/alpha. This is a finite discrete random threshold, with no claim of an exactly proportional count in every tile. Seed and coordinate behavior are part of the algorithm; a random library choice cannot change them.

## Color addition and coverage interpolation

BLN-19 adds a linear RGB signal while retaining background alpha. It is distinct from the existing independently visible Layer emission representation. BLN-20 interpolates associated color and alpha, then returns straight coordinates. Its linear and encoded variants use separate identities. A same-grid t field lets an explicitly constructed soft mask control the interpolation. Other layer/mask policies remain compositions.

## Formula sources

The [research sources](../research-sources.md) distinguish W3C formula definitions from native additions. Hard Mix through vivid-light threshold and through B+S>=1 are separate definitions; at B=0,S=1 they return 0 and 1 respectively. No UI name selects an undocumented alternative formula.

## Current BLN-01 through BLN-10 profile

This profile applies to BLN-01A, BLN-02A/B/C, BLN-03A/B,
BLN-04A/B, BLN-05A/B, BLN-06A, BLN-07A, BLN-08A, BLN-09A
and BLN-10A. Each member uses the operation key declared in its
frontmatter and retains its stated input domain and endpoint rules.

### Parameters

The current parameter representation specializes the COM draft as follows:

| Parameter | Type | Meaning |
| --- | --- | --- |
| `axis` | Int64 | Raw mode: required component axis in the tensor cell descriptor, excluding the `batch_axes` prefix; `0 <= axis < cell_rank`. For a spatial tensor it is the declared channel axis. |
| `components` | String | Raw mode: required canonical comma-separated distinct indices, for example `"0"` or `"2,1,0"`. The selected indices cover the complete component axis, whose extent is exactly one for Gray or three for RGB. This String representation replaces the COM draft's `Array[Int64]` notation for these members. |
| `input_description` | String | Semantic mode: canonical FMT tensor-description parameter encoding with an explicit component axis and native Gray or RGB coordinates. The same description applies to both inputs. |
| `group` | String | Semantic mode: required name of the selected complete color group. Its component axis has exactly one Gray coordinate or three RGB coordinates. |
| `max_work` | Int64 | Optional nonnegative work bound, default `INT64_MAX`; it does not alter the mathematical result. |

Use either raw `axis`/`components` or semantic
`input_description`/`group`, never both. Both modes require an
explicit component axis, including for Gray. Batch axes are
preserved, and the resolved component axis follows their prefix.
Raw selection asserts the member's color domain without inferring
a transfer or color basis from the tensor shape. These color-only
ports contain no additional alpha or AOV slots. Shapes, dtypes and
ordered selections match across the two inputs.

### Native arithmetic

Arithmetic uses the input element type T, with nearest/ties-to-even
rounding and gradual underflow at native arithmetic stages.
Multiplication and addition or subtraction remain separate stages.
The same arithmetic graph applies at each global coordinate,
independently of the request's tile partition.

Normal copies the source element bits without background payload
access. Darken and Lighten copy the selected stored element and
retain background bits on equality. Member thresholds and endpoint
priority are evaluated from the stored background and source values.

Bounded Screen evaluates `B + ((1 - B) * S)`. Unbounded Screen
evaluates `(B + S) - (B * S)`. Other members use their declared
piecewise equations with native arithmetic at each parenthesized
stage. Soft Light uses the declared polynomial for `B <= 0.25`
and native square root otherwise. Dodge and Burn retain their
declared first-priority endpoints before the bounded quotient
result is selected.

The continuous comparison rules remain those of COM. Intermediate
rounding, overflow, underflow and invalid arithmetic follow the
native profile; this profile does not claim correctly rounding the
complete real expression once. No clamp is added beyond a member's
explicit bounded formula.
