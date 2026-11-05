---
spec_schema_version: 1
id: BLN-common
kind: shared_operator_contract
category: 09-composite
status: Proposed
document_maturity: D1_draft
implementation_status: implemented
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
## Current BLN-11 through BLN-20 profile

This profile applies to BLN-11A/B, BLN-12A, BLN-13A, BLN-14A,
BLN-15A/B, BLN-16A/B, BLN-17A/B/C/D/E/F, BLN-18A, BLN-19A
and BLN-20A/B. Each uses the operation key declared in its
frontmatter and retains its stated input domain and endpoint rules.

### Current prototypes

All ports use the same native element type T, Float32 or Float64.
S denotes the matching non-component dimensions, including preserved
batch axes. A component axis is explicit even for Gray.

| Members | Ordered inputs | Output |
| --- | --- | --- |
| BLN-11 through BLN-16 | `background: T[S,K]`, `source: T[S,K]`; K=1 or 3, with no alpha or AOV slots | `color: T[S,K]` |
| BLN-17A through BLN-17F | `background: T[S,3]`, `source: T[S,3]`; exactly three color coordinates | `color: T[S,3]` |
| BLN-18A | `source: T[S,C]`, `background: T[S,C]`; selected Gray/RGB and separate alpha | `image: T[S,C]` |
| BLN-19A | `background: T[S,C]`, `emission: T[S,3]`, `gain: T[1]`; selected RGB and separate alpha | `image: T[S,C]` |
| BLN-20A/B | `first: T[S,C]`, `second: T[S,C]`, `mix: T[S,1]`; selected Gray/RGB and separate alpha | `image: T[S,C]` |

For BLN-19A, gain is a required unbatched tensor; emission coordinates
follow the selected RGB role order. For BLN-20A/B, mix is a required
same-grid field with the component axis in the same position as the
image inputs. A uniform mix value uses an explicit constant field.
BLN-18 through BLN-20 permit additional unselected slots.

### Parameters and coordinate interpretation

| Parameter | Type | Current meaning |
| --- | --- | --- |
| `axis` | Int64 | Required in raw mode: component axis in the cell descriptor, excluding the `batch_axes` prefix; `0 <= axis < cell_rank`. Spatial tensors use their declared channel axis. |
| `components` | String | Required in raw mode: canonical comma-separated distinct nonnegative indices, such as `"0"` or `"2,1,0"`. These are ordered Gray/RGB roles, or `l,a,b` roles for native OKLab/CIELAB. BLN-11 through BLN-17 cover the complete component axis. |
| `input_description` | String | Semantic mode: canonical FMT tensor-description parameter encoding. The same description applies to both image inputs. For BLN-19A it describes background; emission is the corresponding three-coordinate field. |
| `group` | String | Required in semantic mode: name of the complete selected color group. |
| `alpha_index` | Int64 | BLN-18 through BLN-20 raw mode: required nonnegative alpha index, within the component axis and disjoint from selected color. Semantic mode takes alpha from the selected group and forbids this raw parameter. |
| `source_basis` | String | BLN-17A/B: required in raw mode; a FMT RGB-basis preset such as `srgb_rec709`, or canonical `basis-v1:` encoding. Semantic mode may resolve it from explicit primaries and white. An explicit parameter must agree with description assertions. |
| `source_white` | String | BLN-17E/F: required in raw mode; a white preset (`d65`, `d50`, `aces`) or canonical `xy-v1:` encoding. Semantic mode may resolve an explicit description white. An explicit parameter must agree with it. |
| `working_domain` | String | BLN-18A: `linear` or `encoded`; required in raw mode. Semantic mode resolves it from the explicit transfer, and an explicit value must agree. |
| `x_axis`, `y_axis` | Int64 | BLN-18A: distinct cell axes, different from the component axis. Required without spatial layout; otherwise resolved from the declared width/height axes, with explicit values checked for agreement. |
| `origin_x`, `origin_y` | Int64 | BLN-18A: default 0; added to full-image x/y coordinates. The generated coordinates must remain representable as Int64. |
| `seed`, `frame` | Int64 | BLN-18A: default 0, interpreted modulo 2^64 by the counter definition. |
| `max_work` | Int64 | Optional nonnegative work bound, default `INT64_MAX`; it does not alter the mathematical result. |

Raw `axis`/`components` and semantic `input_description`/`group`
are mutually exclusive. This String component representation specializes
the COM draft's `Array[Int64]` notation. Raw mode explicitly asserts
the member's domain. Semantic BLN-17A/B, BLN-19A and BLN-20A require
linear RGB/Gray coordinates as appropriate; BLN-20B requires an explicit
nonlinear transfer. BLN-17C/D use native OKLab and BLN-17E/F native
CIELAB, with roles `l,a,b`, native L or l=L*/100, and no implicit
model conversion. Selected color and alpha encodings are native.

### Native arithmetic and exact decisions

Let RN_T denote nearest/ties-to-even rounding to T with gradual
underflow. Multiplication, addition, subtraction and division remain
separate native stages; no contraction is implied. The same stages
apply at each global coordinate independently of tile partition.

Linear Dodge evaluates `min(1, RN_T(B+S))`; Linear Burn evaluates
`max(+0, RN_T(RN_T(B+S)-1))`. Linear Light evaluates
`clamp(RN_T(B + RN_T(RN_T(2*S)-1)), +0, 1)`.
Difference evaluates `abs(RN_T(B-S))`. Exclusion evaluates
`RN_T(RN_T(B+S) - RN_T(RN_T(2*B)*S))`.

Vivid Light chooses its branch from stored `S<=0.5`. Its low branch
uses numerator `RN_T(1-B)` and denominator `RN_T(2*S)`; its high
branch uses B and `RN_T(2*RN_T(1-S))`. It divides once, bounds the
quotient by 1, and subtracts it from 1 in the low branch. The Burn
endpoint B=1 and Dodge endpoint B=0 retain first priority; a zero
denominator selects the declared branch endpoint. Pin Light uses
`RN_T(2*S)` and `RN_T(RN_T(2*S)-1)`, then the declared min/max
branch, retaining B bits on equality.

Hard Mix makes an exact discrete decision from the stored inputs.
With `n=min(B,S)` and `m=max(B,S)`, Hard Mix Sum returns 1
exactly when `n >= 1-m`. Hard Mix Vivid additionally requires
`B!=0`. The subtraction is exact in the threshold-relevant domain
m>=0.5; below that domain the inequality is false. This decides the
declared real threshold without first rounding B+S or the Vivid Light
result. At B=0,S=1 the two members return 1 and 0 respectively.

For BLN-17A/B, the RGB-basis Y row is first published as three
binary64 coefficients r. Normalize these stored values exactly to
w=r/sum(r), requiring a positive sum and permitting signed coefficients.
Each stored input tuple then produces its own RN64 publication of
the exact dot product with w; Float32 inputs promote without loss.
Compare the published measures, keep background on equality, and copy
the selected original tuple bits. Interval bounds may decide a pair
only when they certify this same published-measure comparison;
otherwise the exact measurement is evaluated. Candidate bounds may use
fused multiply-add for a nonnegative normalized row; certification
includes candidate rounding and the two separate RN64 measurement
publications. Native OKLab/CIELAB members compare the stored selected
L/l coordinate directly and retain background on ties.

For BLN-18A, use the counter definition above in x, y, frame order,
with global_x=origin_x+x and global_y=origin_y+y. Batch indices do
not implicitly modify seed or frame. The exact threshold is
`U < ceil(alpha*2^64)`; alpha=0 always selects background and
alpha=1 always selects source. U is never converted to a floating
random value. A source selection copies source color bits and
produces alpha=1; a background selection copies background color
and alpha bits. Unselected slots independently copy background.

BLN-19A computes `RN_T(background_color + RN_T(gain*emission))`.
Gain=0 copies the original image bits without an emission dependency.
Background alpha and unselected slots are copied unchanged.

For BLN-20A/B, stored t=0 selects first color/alpha bits and t=1
selects second color/alpha bits. Unselected slots always copy first,
including at t=1. At an interior t, evaluate in this order:

```text
a = RN_T(RN_T(1-t) * alpha_first)
b = RN_T(t * alpha_second)
A = RN_T(a+b)
P = RN_T((a==0 ? +0 : RN_T(a*C_first))
       + (b==0 ? +0 : RN_T(b*C_second)))
color = A>0 ? RN_T(P/A) : +0
alpha = A
```

A zero native weight, including one produced by multiplication
underflow, has no corresponding color dependency. Alpha-only
evaluation uses the alphas and mix field. Continuous arithmetic
uses the COM native comparison rules; the staged weights can
underflow to zero even when the complete strict expression has a
nonzero result. Explicit copies and discrete decisions retain their
exact requirements.
