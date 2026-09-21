---
spec_schema_version: 1
id: CRV-06H
parent_id: CRV-06
function: color_ramp_ycbcr
operation_family: curve.color_ramp_ycbcr
proposed_operation_keys:
  - curve.color_ramp_ycbcr_strict
  - curve.color_ramp_ycbcr_accelerated_apple_silicon
  - curve.color_ramp_ycbcr_accelerated_x86_64
category: 01-numeric
kind: primitive
status: Proposed
document_maturity: D1_draft
implementation_status: implemented
implementation_branch: numeric-optimize
implementation_base_commit: eb0e90c8
implementation_updated: 2026-09-21
clarification_status: complete
repository_branch: ops-specs
repository_commit: 6617c78c
---

# CRV-06H: color_ramp_ycbcr

Numeric profile: strict retains the exact reference defined below. Floating
arithmetic in accelerated profiles follows the shared
[final FP32 four-ULP contract](NUM_accelerated_contract.md), including its
range/fallback rules. Discrete results, copies, selected endpoints and special
values remain exact.

Directly interpolate the three Y',Cb,Cr components, retaining the same complete
color description. The description specifies underlying RGB primaries, white,
transfer, YCbCr matrix and numeric range/encoding. The initial variant has no
alpha and no chroma subsampling. It does not convert to RGB to perform blending.

The matrix uses the non-constant-luminance (NCL) form parameterized by static
Kr,Kb, requiring finite Kr>0, Kb>0 and exact Kr+Kb<1. Provide BT.601, BT.709 and
BT.2020 NCL matrix presets plus custom coefficients. A preset only fills matrix
coefficients; primaries/white and transfer remain explicit. BT.2020 constant
luminance uses a different definition and is not supported by this NCL variant.

## Numeric encoding and mathematical identity

Use unbiased floating coordinates: nominal Y' range [0,1], nominal Cb/Cr range
[-0.5,0.5], with neutral chroma at zero. All finite extensions are allowed;
nominal ranges are not clipping or validity limits. Integer full/limited code
values require a separate conversion before this ramp. No code-depth parameter,
chroma offset or full/limited code-range interpretation is inferred.

The descriptor identifies the coordinates through
Y'=Kr*R'+(1-Kr-Kb)*G'+Kb*B', Cb=(B'-Y')/(2*(1-Kb)),
Cr=(R'-Y')/(2*(1-Kr)). These formulas describe the model; the ramp consumes
Y'/Cb/Cr directly and executes no RGB conversion. Named matrix coefficients are
BT.601 Kr=0.299,Kb=0.114; BT.709 Kr=0.2126,Kb=0.0722; BT.2020 NCL
Kr=0.2627,Kb=0.0593. The decimal values identify the standard matrices;
preset helpers correctly round these decimals to Float64; stored values define
the descriptor, and identical custom coefficients match the same preset values. A matrix preset alone does not imply a complete broadcast
standard profile, its RGB primaries or its transfer.

## Interface, arithmetic and execution

Inherit [CIELAB CRV-06C](CRV-06C_color_ramp_cielab.md) for ordered dynamic
input, stops, colors ports and named values output. colors is [K,3] in Y',Cb,Cr
order; values is input.shape+[3]. Float32/Float64 ports may mix independently;
output dtype defaults to colors dtype. K is 1..65536, input rank 1..7, all
extents positive and logical element counts <=2^40. Stops are globally finite
and strictly increasing. out_of_domain is clamp/reject, default clamp, including
the established K=1 behavior. Input positions remain finite and are read even
when the selected table is constant.

Required static String parameters are color_description, dtype, out_of_domain.
The description explicitly declares YCbCr NCL, underlying RGB primaries/white/
transfer, matrix coefficients/preset, unbiased floating coordinate units and no
alpha. Existing attached colors metadata must match. No matrix default silently
fills a missing required description. The output retains that complete description.

For each component c, interpolate RN_dtype((1-w)*c0+w*c1), using the exact
stop-derived w and only one final rounding for strict. Accelerated results obey
the shared final FP32 bound. Hit/clamp/K=1 directly converts the selected row. Direct/identical-row
paths preserve signed zero; genuine interpolation exact zero is +0; nonzero
underflow preserves sign. Demanded components and final results must be finite.

All formal strict and accelerated keys use Whole execution. Any nonempty
request collects complete input, stops and color arrays (and both rational-hue
integer arrays when present), with complete upstream and typed validation.
The callback validates every stop, then every position, before color arithmetic.
Each position still uses exactly one hit/clamp/singleton row or two enclosing
rows mathematically. Unused generic color rows are not subjected to new numeric
domain checks; invalid typed data or upstream failures anywhere still fail.
Empty requests perform static preflight but read no sample payload.

The output is one immutable dense Value of shape input.shape+[C]. The final
channel axis retains complete-color closure, ColorArray identity and owned ICC
resources where applicable. Public fragments expose the requested complete
colors while retaining the full output owner. Arbitrary immutable input strides,
offsets and unaligned storage are supported. Owners survive context teardown.
Any input edit invalidates the complete recorded output demand. Cache identity
retains descriptors, parameters, typed validation and resource identities.
Numeric errors have Run scope; no successful color subset survives a failed
callback. Upstream, resource and cancellation errors retain their categories.

For N=product(input.shape), lookup work is O(K+N log K), plus actual exact or
certified arithmetic. Full output payload is N*C*sizeof(dtype), even for a small
requested region. Account complete collected inputs, fixed admitted arithmetic
workspace, a ResourceVector stop index with 8K element bytes plus allocator and
metadata overhead, and retained descriptors/resources. No per-output dependency
records or point-state array is retained. Work/capacity limits and cancellation
apply during scans, lookup, arithmetic and before publication; incomplete
certification fails ResourceExhausted. No reduced-precision fallback is added.

## Failure and acceptance

Malformed description, matrix/domain parameters or unsupported representation
fail preflight with InvalidArgument/InvalidDomain; shape/dtype or attached
description mismatch uses TypeMismatch. Nonfinite demanded floats, invalid stops
or rejected positions use OperationFailed/InvalidDomain. Final overflow uses
OperationFailed/ArithmeticOverflow. Preserve resource/cancellation/upstream error
categories and fail the Whole run; no partly valid tuple is published.

Fixture: stops=[0,1], colors=[[0,0,0],[1,0.5,-0.5]], input=[0.5]
returns [[0.5,0.25,-0.25]] with unchanged declared YCbCr description. Include
finite Y' outside [0,1], large signed chroma, descriptor mismatches, custom matrix
validation, all float dtype combinations and signed zeros. Use independent exact
rational interpolation/rounding as the numeric oracle. Apply CRV-06C's full
partial-request, dirty, stride, budget, cancellation and owner-lifetime acceptance.
The maintained public Compiler/ExecutionContext workflow and current runtime evidence
are linked below; the document remains Proposed.

- [Ramp family](CRV-06_color_ramp.md).
- [Color-array description](../../02-format-color/op_specs/FMT-COLOR_color_array_contract.md).
- [BT.709-6](https://www.itu.int/rec/R-REC-BT.709-6-201506-I).
- [BT.601-7](https://www.itu.int/dms_pubrec/itu-r/rec/bt/R-REC-BT.601-7-201103-I%21%21PDF-E.pdf).
- [BT.2020-2](https://www.itu.int/rec/R-REC-BT.2020-2-201510-I/en).

## Maintained implementation and validation

Public [`color_ramp_ycbcr_node`](../../../../include/photospider/numeric/color_ramps.hpp)
constructs this primitive; [`color_ramps.cpp`](../../../../plugins/ops/01-numeric/color_ramps.cpp)
implements its Whole complete-output execution.

All profiles use exact rational component interpolation and return strict
bits, with one destination rounding. Complete-color metadata and complete-input typed
validation remain attached to the result.

See the [family implementation](CRV-06_color_ramp.md#maintained-implementation-and-validation),
[mathematical machinery](../math-implementation.md#crv-06-colorarray-and-color-ramps)
and [public workflow commands](../../../../examples/numeric_workflow/README.md#color-ramps)
for resource limits and actual validation.
