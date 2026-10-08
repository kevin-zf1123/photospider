---
spec_schema_version: 1
id: CRV-06D
parent_id: CRV-06
function: color_ramp_cielch
operation_family: curve.color_ramp_cielch
category: 01-numeric
kind: shared_operator_contract
status: Proposed
document_maturity: D1_draft
implementation_status: implemented_subset
clarification_status: complete
---

# CRV-06D: color_ramp_cielch

Dynamic inputs inherit the [family Result tensor-port contract](CRV-06_color_ramp.md#result-tensor-ports): each is a Result with exactly one tensor member and no fields, under any structurally valid schema id/version/member key. Shape checks use complete `sample_shape()` values, including batch axes.

## Revised lightness coordinate and implementation boundary

The [shared scale contract](../../02-format-color/op_specs/FMT_relative_coordinate_scale.md)
requires native CIELAB/CIELCh l=L*/100, including ramp stops' color values,
LUT input axes and output table coordinates. Finite values outside 0..1 remain
legal. Opponent/chroma scales and arithmetic formulas are unchanged. The current runtime ColorArray v1 encodes L* in its implicit 0..100 unit;
this target scale applies only after public metadata, fixtures and consumers
adopt it explicitly. No implicit unit alias or inference from sample magnitude
is permitted.

Numeric profile: strict retains the exact reference defined below. Floating
arithmetic in accelerated profiles follows the shared
[final FP32 four-ULP contract](NUM_accelerated_contract.md), including its
range/fallback rules. Discrete results, copies, selected endpoints and special
values remain exact.

This is the independently required CIELCh(ab) ramp, distinct from CIELAB,
OKLab and OKLCh. The initial variant has three color channels and no alpha.
Use explicit stops and a complete color-array description. This shared contract
defines three independent input entrypoints; it does not register a runtime
input-representation mode. The three primitives are implemented separately.

## Coordinates and unnormalized hue

Channels are l=L*/100, C*, h. l accepts finite extensions, C* is finite and nonnegative,
and white is explicit, default D50. No alpha is present. Hue uses floating radians,
floating pi multiples or exact Int64 p/q*pi. Selected rows require q>0, while an
unselected generic q<=0 value adds no mathematical-domain error; typed/upstream
validation still covers all inputs. Unreduced fractions are valid. Preserve the
original hue value, its sign and winding count;
there is no modulo reduction, one-turn output range or hue_path parameter.

Input hue remains active even when C*=0, including at direct hit/clamp and when
both selected chromas are zero. Never borrow another hue or force achromatic hue
to zero. The same rule holds when output chroma underflows to zero.

Three independent entrypoints use colors[K,3] for floating hues, or
lightness_chroma[K,2], hue_numerator[K], hue_denominator[K] for rational pi hues.
Output remains three floating color channels. Static output_hue_unit is radian
or pi_multiple, default radian for radian input and pi_multiple otherwise.
The output color description records that actual unit. Rational input is retained
exactly through interpolation and any unit conversion, then rounded only at output.

## Ports, parameters and descriptor

Inherit the [CIELAB ramp](CRV-06C_color_ramp_cielab.md) for dynamic input/stops,
finite strictly increasing stops, K=1..65536, clamp/reject default clamp,
singleton semantics, mixed Float32/Float64 ports, positive extents and logical
element limits of 2^40. Input rank is 1..7; values has shape sample_shape(input)+[3].
Floating entrypoints default output dtype to colors dtype; the rational entrypoint
defaults it to lightness_chroma dtype. Numerator/denominator are Int64[K], with
the same K as stops and lightness_chroma. No implicit broadcast is performed.

Direct nodes supply static color_description, dtype, out_of_domain and
output_hue_unit; constructors write the defaults specified here. The explicit
description identifies CIELCh(ab), reference-white xy (default D50), channel units,
no alpha, and the entrypoint's input hue representation. Attached descriptions
must match. For rational input, the description interprets the coordinated ports
as one color table; lightness_chroma[K,2] alone is not a complete three-channel
ColorArray. Output is a complete CIELCh(ab) ColorArray whose hue unit is the
selected floating output_hue_unit. No RGB transfer, gamut conversion, alpha or
white adaptation occurs. The public ColorArray codec supplies the description.

## Exact interpolation and unit conversion

For an interior query, w=(input-stops[j])/(stops[j+1]-stops[j]) exactly.
Compute L=(1-w)*L0+w*L1 and C=(1-w)*C0+w*C1. For floating radian input, the
exact source hue is h in radians; for floating pi input it is h*pi; for rational
pi input it is (p/q)*pi. Interpolate those original mathematical angles linearly:
H=(1-w)*H0+w*H1. Output H for radian, or H/pi for pi_multiple. Never replace
an endpoint by a coterminal angle. In particular 0 to 4*pi has midpoint 2*pi.

Strict correctly rounds the complete formula to output dtype, ties-to-even.
Accelerated uses the shared FP32-scaled final-result bound. When input and output hue units
match, cancel pi symbolically rather than performing an unnecessary rounded
multiply/divide. Exact rational source hues must not first become rounded
floating multipliers. Do not round weights, intermediate endpoint differences,
pi products or components before the final destination conversion.

Hit/clamp/K=1 selects one row and directly converts L/C and hue to the output
units/dtype. Identical selected rows give the same result; both remain dependencies.
All demanded floating source components and all returned components must be
finite. C>=0 is required; negative zero C is valid. No intermediate-overflow
failure is allowed when the exact final result is representable. Exact rational
handling covers INT64_MIN without signed abs or intermediate overflow.

Exact unit conversion may require certified precision for pi. ResourceExhausted
is returned if the host budget is exhausted before rounding is determined; it
never authorizes a 4-ULP approximation. Rational-to-same-unit interpolation can
use integer/rational arithmetic; mixed pi/radian output uses certified intervals
and exact cancellation as needed. Results are independent of query order and
request partition. Direct hit/clamp/identical-row conversions preserve source floating hue zero
signs, including across positive pi unit conversion. A rational zero numerator
represents +0. On genuine interpolation, exact zero hue is +0 and nonzero
underflow retains the mathematical sign. Apply the same direct/identical versus
mixed zero rules to L/C. All selected source rows are validated before shortcuts.

## Demand, invalidation, resources and failures

All formal strict and accelerated keys use Whole Result programs. A nonempty
Run declares Data, Validation and Descriptor needs (role 13) for every input, so
typed and upstream validation covers complete tensor members. The executor reads
authorized windows directly; the callback does not collect or copy complete input
arrays. It validates all stops, then every input position, before color arithmetic.
Each position uses one exact hit/clamp/singleton row or two enclosing rows for its
mathematics. Generic color rows outside all evaluated stencils remain numerically
unused, while their typed/upstream validation still applies. Empty reads no sample
payload after static preparation.

The `values` port publishes an immutable `photospider.tensor` v1 Result with
`samples` shape `sample_shape(input)+[C]`, the ColorArray v1 facet and
`atomic_trailing_axes=1`. Whole writes publish the full output transactionally;
the Result retains full certified coverage and global coordinates. Fragments expose
requested regions while retaining the full output owner. Arbitrary immutable source
strides, offsets and unaligned storage are supported. Result association records
source ObjectIds, and selected ColorArray resources remain owned by the output.
Input edits invalidate complete recorded output demand. Numeric errors have Run
scope; failed callbacks publish no color subset. Upstream, resource and cancellation
errors retain their categories.

Lookup work remains O(K+N log K), plus exact or certified arithmetic. Full output
payload is N*C*sizeof(dtype), even for a small request. Account source owners/windows,
fixed arithmetic workspace, the Root-owned ResourceVector stop index (8K element
bytes plus allocator/metadata overhead), output and retained descriptors/resources.
No per-output dependency records or point-state array is retained. Work/capacity and
cancellation checks apply during scans, lookup, arithmetic and before publication;
incomplete certification fails ResourceExhausted. No reduced-precision fallback is added.

C=0 never suppresses hue mathematics or positive-denominator validation for
selected rational rows. Certified pi conversion retains its fixed precision
ceiling; a finite large angle does not create an unbounded precision cache.

Malformed statics/descriptions fail preflight with InvalidArgument/InvalidDomain;
port shapes/dtypes and descriptor mismatch use TypeMismatch. Nonfinite demanded
floats, negative C, selected q<=0, invalid stops or rejected input positions use
OperationFailed/InvalidDomain at Run scope. Final narrowing
overflow uses OperationFailed/ArithmeticOverflow. Platform mismatch, resources,
cancellation, stale and upstream failures retain their established categories.
No partially valid L/C/h tuple is published. Implementation cannot substitute a
4-ULP path when exact rounding or unit conversion exceeds the budget.

## Acceptance and public workflow obligation

Use independent exact-rational interpolation for equal input/output units and
certified pi enclosures for unit conversion. Test every profile, destination dtype,
unit pair and source float dtype. Include negative and multi-turn hue, huge finite
values, cancellation, INT64_MIN, equivalent unreduced fractions and C=0. A rounded
floating pi input is interpreted as its actual binary value, not exact pi.

With stops=[0,1], pi-multiple colors=[[0.25,2,0],[0.75,4,4]], query=0.5 returns
[[0.5,3,2]]. For hues 1.75 and 0.25 the midpoint is 1; for hues -3 and 5 it is 1.
Rational hues 7/4 and 1/4 yield the same midpoint as those exact floating values.
Equal endpoints remain constant; neither same direction modulo a turn nor C=0
changes the formula. A hit at hue=4 retains 4 in pi units. No hue_path parameter
or normalization mode is accepted.

Test channel requests expanding to full colors, mathematically unused generic rows and complete typed validation,
selected q=0 failing even at C=0, global stop failures, whole-input dirty support,
strides, budgets, cancellation, cache-off and output lifetime. The public
Compiler/ExecutionContext workflow binds the entrypoint-specific ports and
checks named values and metadata, with commands and independent expected
results linked below.

- [Radian entrypoint](CRV-06D1_color_ramp_cielch.md).
- [Floating pi entrypoint](CRV-06D2_color_ramp_cielch_pi.md).
- [Rational pi entrypoint](CRV-06D3_color_ramp_cielch_rational_pi.md).

The maintainer also requested exact p/q*pi input counterparts in NUM-04. Their
specifications are recorded in [the rational-pi contract](NUM-04_rational_pi_contract.md)
and do not replace the existing floating pi-multiple operations.

- [Ramp family](CRV-06_color_ramp.md).
- [CIELAB counterpart](CRV-06C_color_ramp_cielab.md).
- [Color-array description](../../02-format-color/op_specs/FMT-COLOR_color_array_contract.md).
- [Operator template](../../00-foundation/spec-template.md).
