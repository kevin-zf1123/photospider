---
spec_schema_version: 1
id: CMP-common
kind: shared_operator_contract
category: 09-composite
status: Proposed
document_maturity: D1_draft
implementation_status: not_implemented
clarification_status: complete
---

# CMP: basic coverage composition

Inherit [COM common](COM_common_contract.md). This contract applies to all 27 CMP-01 through CMP-08 members. Each member's frontmatter specifies its `composite.*` operation key. Arithmetic members use their explicitly named linear or encoded working coordinates; domain conversion is an explicit upstream operation. Copy and clear primitives are domain-independent.

## Interface

All ports use the same native type `T`, Float32 or Float64. `S` denotes the non-component dimensions, including any preserved batch prefix. The component axis is explicit even for Gray. Each Result has one tensor, with the output schema and member key specified by COM.

| Members | Ordered inputs | Ordered outputs |
| --- | --- | --- |
| CMP-01A | `template: T[S,C]` | `image: T[S,C]` |
| CMP-02A | `source: T[S,C]` | `image: T[S,C]` |
| CMP-02B | `destination: T[S,C]` | `image: T[S,C]` |
| CMP-03C/F | `source: T[S,C]`, `background: T[S,C]`, `blended: T[S,K]` | `image: T[S,C]` |
| CMP-08A/C | `source: T[S,C]`, `background: T[S,C]` | `color_mass: T[S,K]`, `mass: T[S,1]` |
| Other arithmetic members | `source: T[S,C]`, `background: T[S,C]` | `image: T[S,C]` |

K is 1 for Gray or 3 for RGB. Blended and raw output components follow the ordered selected color roles, with their component axis in the same position as the image component axis. Images may contain additional unselected slots. Clear and the two selectors do not require color or alpha selectors.

All parameters below are static. Parameters without a default are required in their applicable selection mode.

| Parameter | Type | Meaning |
| --- | --- | --- |
| `axis` | Int64 | Arithmetic raw mode: required cell-local component axis, excluding the batch prefix, with `0 <= axis < cell_rank`. Spatial tensors use their declared channel axis. |
| `components` | String | Arithmetic raw mode: required canonical comma-separated distinct nonnegative color indices, such as `"0"` or `"2,1,0"`, in Gray or red/green/blue role order. Exactly 1 or 3 indices are selected. |
| `alpha_index` | Int64 | Arithmetic raw mode: required nonnegative alpha index within the component axis and distinct from selected colors. |
| `input_description` | String | Arithmetic semantic mode: canonical FMT tensor-description parameter encoding, applied to both images. Color and alpha encoding is native; the selected model is Gray or RGB with straight association. |
| `group` | String | Required with `input_description`: name of the complete selected color group. Alpha is resolved from that group; the optional per-channel table need not be present. |
| `max_work` | Int64 | Optional nonnegative native work bound, default `INT64_MAX`, for each invocation. It does not change the mathematical result. |

Raw `axis`/`components`/`alpha_index` and semantic `input_description`/`group` are mutually exclusive. The String component representation specializes COM's `Array[Int64]` notation. Raw selection asserts the member's working domain. A semantic description supplies a `linear` transfer for linear members and an explicit non-linear transfer for encoded members. Matching color basis, white, reference units and image interpretation remain caller premises. No broadcasting is inserted.

## Mathematics

`C_s` and `C_b` denote straight source and background color components; `a` and `b` denote their alpha values. `S` denotes the non-component shape in port signatures. Member mathematics written with source/background symbols `S,B` uses those symbols for `C_s,C_b`, respectively. All color equations apply componentwise in the member's declared working domain.

### Porter-Duff equations

For factors `F_s` and `F_b`, the output alpha `A` and color mass `P` are:

$$
A = F_s a + F_b b, \qquad P = F_s a C_s + F_b b C_b.
$$

The straight image output is `(P/A, A)` for positive `A` and `(+0, A)` for zero `A`. Atop copies the retained input alpha as specified below.

| Operation | `F_s` | `F_b` |
| --- | --- | --- |
| source over | 1 | 1-a |
| destination over | 1-b | 1 |
| source in | b | 0 |
| destination in | 0 | a |
| source out | 1-b | 0 |
| destination out | 0 | 1-a |
| source atop | b | 1-a |
| destination atop | 1-b | a |
| xor | 1-b | 1-a |

Source-over with supplied blended color `F` uses:

$$
P = (1-a)bC_b + (1-b)aC_s + abF, \qquad A = a+b-ab.
$$

`F` is a stored input in the compositing domain. Its producer supplies the blend-mode result explicitly.

### Plus equations

| Members | Color result | Alpha or mass result |
| --- | --- | --- |
| Raw plus | `color_mass = a*C_s + b*C_b` | `mass = a+b`, in [0,2] |
| Bounded plus | `P/A` for positive `A`, with `P = min(1, a*C_s + b*C_b)` | `A = min(1, a+b)` |

Raw plus returns separate weighted color coordinates and weight mass. Raw encoded mass is a weighted sum of encoded coordinates; physical light quantities require a declared linear domain. Coverage alpha and independent emission have their separate COM meanings. Bounded plus requires bounded source colors and produces `+0` color at zero `A`.

### Native arithmetic and exact behavior

CMP inherits [COM's numerical target and strict-reference policy](COM_common_contract.md#numerical-target-and-strict-reference). Fused arithmetic, including FMA, and equivalent reassociation are permitted within COM's continuous comparison tolerances, subject to the exact copy, zero, retained-alpha and dependency requirements below. The equations specify mathematical results; individual native rounding stages and a particular evaluation graph are implementation choices. For a given implementation, identical coordinates must produce the same result across scalar, contiguous and strided layouts and across ROI or tile partitions.

Native intermediate overflow, invalid arithmetic and underflow follow COM. Tiny alpha, cancellation and overflow can produce differences from the strict reference; COM requires tiny-alpha comparisons and disclosure of those differences. Its analytic-fixture tolerances apply to finite comparison cases and do not establish a universal bound for extreme inputs.

| Condition or member | Required numerical behavior |
| --- | --- |
| Zero native color coefficient | The corresponding term is `+0`, with the branch taken before reading that color sample. |
| Generated straight image with computed `A == 0` | Every selected color is `+0` before any color sample is read, including when native alpha computation underflows to zero. |
| Source-in / source-out with positive computed `A` | Return the stored source color, preserving a selected `-0`. |
| Destination-in / destination-out with positive computed `A` | Return the stored background color, preserving a selected `-0`. |
| Source-atop | Copy `b` as alpha, preserving its bits, including `-0`. At positive `b`, color is `a*C_s + (1-a)*C_b`, with the zero-coefficient rule applied to each term. |
| Destination-atop | Copy `a` as alpha, preserving its bits, including `-0`. At positive `a`, color is `(1-b)*C_s + b*C_b`, with the zero-coefficient rule applied to each term. |
| Explicit source/destination selection | Copy every requested input bit, including hidden colors, signed zero and NaN payloads. |
| Clear | Generate `+0` in every requested slot, including alpha and auxiliary channels. |

## Behavior

Every CMP member has matching-coordinate support. Empty requests read no payload. Shape, dtype, selectors and output schemas are static dependencies; numeric domains and matching color interpretations follow COM's unchecked caller premises.

| Requested output | Payload dependencies |
| --- | --- |
| Clear | Template descriptor only; no template sample access. |
| Explicit source/destination selection | Corresponding samples of the single selected input. |
| Ordinary image color | Colors and alpha samples used by the member's equation, subject to the zero-coefficient and zero-alpha branches. In/out read only the selected side's color. |
| Ordinary image alpha | Alpha samples used by the member's equation; source-atop reads only background alpha and destination-atop only source alpha. |
| Unselected image slots | Corresponding background slots, copied bitwise and independently of selected colors or alpha. |
| Raw `color_mass` | Corresponding selected colors and both alpha samples, subject to the zero-coefficient branches. Components follow the selected Gray or red/green/blue order. |
| Raw `mass` | Both alpha samples only. |

Raw outputs contain their declared color or mass components without AOVs. Requesting either raw output independently produces the same values as requesting both together.

## Acceptance

For source `(C_s,a) = ((1,0,0),0.5)` and background `(C_b,b) = ((0,0,1),0.5)`:

| Operation | Result in the declared coordinates |
| --- | --- |
| source over | (2/3,0,1/3,0.75) |
| destination over | (1/3,0,2/3,0.75) |
| source in / source out | (1,0,0,0.25) |
| source atop / xor | (0.5,0,0.5,0.5) |
| raw plus | color_mass=(0.5,0,0.5), mass=1 |

Use COM's finite comparison tolerances for continuous results and exact checks for the copy, zero and retained-alpha requirements. Compare identical coordinates across layouts, whole-image, nonzero ROI and tile requests, including independent alpha, AOV and raw-output requests.

The same numeric tuple in different color interpretations denotes different colors. Comparing a physical linear route with an encoded-artistic route requires explicit conversion of the same input colors into their respective domains. These mathematical choices follow the [Porter-Duff/W3C references](../research-sources.md#coverage-and-color-blending); they do not prescribe browser or editor layer semantics.
