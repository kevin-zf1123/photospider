---
spec_schema_version: 1
id: FMT-04
kind: shared_operator_contract
category: 02-format-color
status: Proposed
document_maturity: D1_draft
implementation_status: implemented_cpu_fixed_abi_8
clarification_status: complete
---

# FMT-04: explicit alpha representation boundary adapters

The default registry provides `alpha.associate_<profile>` and `alpha.unassociate_<profile>` for `strict`, `accelerated_apple_silicon`, and `accelerated_x86_64`. Semantic calls bind one Result; raw plane/scalar calls may bind a second Result.

Inherits [FMT-common](FMT_common_contract.md) and its NUM baseline. Members [A associate](FMT-04A_associate_alpha.md) and [B unassociate](FMT-04B_unassociate_alpha.md) are Result CPU operations with three profile keys each. Alpha is an explicit graph input or an internal channel; canonical images stay straight.

## Purpose

Canonical complete images use straight colors and internal alpha. Semantic A produces explicitly described premultiplied numeric payloads for export or an external interface; semantic B converts explicitly described premultiplied payloads back to straight. A boundary payload is a generic tensor with a source or target representation description, not a second canonical image type. The caller converts premultiplied boundary samples before supplying them to a consumer requiring straight colors, or explicitly reinterprets the numbers without claiming preservation of color. Results carry no attached representation label. A consumer may reject unsupported static interpretation choices; it does not infer or validate association from the payload.

Both semantic adapters require alpha in the same tensor and preserve shape, dtype, channel locations and alpha bits. Independent planes/scalars are first explicitly assembled, using FMT-02 or FMT-05 where applicable. FMT-04 does not insert/remove alpha slots or append alpha. Raw numerical modes may receive explicit external weights as ordinary input ports.

Each call converts one explicit RGB or Gray group. Unselected groups, depth, normals, emission and AOVs are copied unchanged. There is no premultiplied Lab/CMYK semantic conversion. Multiple groups are converted by explicit nodes; a partially converted multi-group tensor remains a boundary payload while any complete color group is premultiplied. A downstream consumer may use a complete straight-image interpretation only when all required groups have been converted.

A requires the selected group to be straight with an internal alpha reference; B requires it to be explicitly premultiplied with an internal alpha reference. A description already in the target state is rejected during preparation. A color group without alpha must first receive an internal alpha through an explicit assembly/edit. The original input and its other consumers remain unchanged.

The arithmetic acts on stored coordinates: P=alpha*C even when C has encoded transfer. No implicit transfer decode, linearization, gamut/range conversion or alpha encoding occurs. Callers explicitly choose color/transfer boundary order. RGB/Gray samples may be finite signed/HDR values; there is no RGB<=alpha bound. The caller supplies finite alpha in [0,1] when semantic color conversion consumes it.

## Interface

Input `input` and output `values` have the same shape and Float32/Float64 dtype. A call connects one source Result and optionally one explicit alpha Result for raw plane/scalar modes; each connected Result contains exactly one tensor. Nonscalar batch prefixes match exactly; scalar weights are unbatched shape `[1]`. The complete sample rank, including batch and cell axes, is at most 8 and each full sample count is at most 2^40 with positive extents. A rank-one channel vector is legal and retains its rank.

With `input_description`, the input has a declared channel axis, selected RGB indices or a Gray index, and a distinct internal alpha index outside those color indices. Index/name/role selectors follow the FMT-01 exact unique namespace rules. There is no HWC assumption. A channel axis contains at most 65,536 slots. Let S be the shape with the channel axis erased.

| Parameter | Definition / default |
| --- | --- |
| input_description | Optional explicit TensorDescription. Its presence selects described RGB/Gray association; its absence selects raw arithmetic with explicit structure and components. |
| group | Required explicit unique RGB/Gray group in semantic modes. |
| input_structure / axis | Structure/axis assertion consistent with the effective description; raw supplies structure explicitly. |
| components | Raw-only nonempty distinct selected indices, or component 0 for a tensor without a channel axis. |
| alpha_source | Semantic: selected group's internal alpha reference, optionally asserted by an explicit matching internal selector. Raw: required internal index, explicit plane input or explicit scalar input. |
| alpha | Raw-only optional explicit weight port, same dtype; plane has exactly S (full input shape for a component input), scalar has exactly [1]. |

Described association with an external alpha port is rejected. A standalone Gray plane must be assembled with alpha first. In raw mode, plane versus scalar is explicit; no squeeze, broadcast or resizing occurs. Empty S uses internal/scalar raw weights rather than rank-zero external planes. Structure, selectors, group state and source kinds are static; input samples may change between executions. Association and unassociation expose no layout parameter. There is no integer normalization or mixed-dtype promotion.

## Mathematics

Let x be one requested participating input component, a its selected alpha and RN_t the NUM strict rounding to input/output dtype t, with ties-to-even and gradual underflow. Semantic modes read both operands. The caller supplies finite x and finite a in [0,1] for every evaluated participating coordinate.

```
associate:
    a == +/-0: y = +0
    a > 0:     y = RN_t(x * a)

unassociate:
    a == +/-0 and x == +/-0: y = +0
    a == +/-0 and x != 0:   outside the semantic caller preconditions
    a > 0:                 y = RN_t(x / a)
```

All positive alpha values, including subnormals, follow the arithmetic branch. A small positive value is never replaced by zero and no epsilon denominator is used. The caller guarantees that the correctly rounded semantic result is finite, including unassociation by a small positive alpha. The operation applies no clipping. Gradual underflow to a subnormal or signed zero is allowed. The a=0 branch is canonical positive zero; for a>0, zero signs follow NUM arithmetic. Internal alpha and other pass-through components are copied bit-for-bit, preserving even the sign of zero.

Strict multiplication/division rounds the exact result once to t, as in NUM-05C/D. Named accelerated arithmetic inherits those NUM profile guarantees, including exact classification, predicates and copied values, with the same caller preconditions and no FMT-specific tolerance. Opacity a=1 is numerically identity. Round-trip association is not generally lossless because rounding/underflow and zero-alpha hidden-color removal can lose information.

In raw mode the arithmetic is exactly NUM multiply(x,a) or divide(x,a), with x as the first operand and a as the second. Both operands are read even when an identity/special value determines the result. No coverage range, finite-color or source association check applies. NUM's NaN priority/payload, signed zero, division-by-zero and floating-overflow outputs apply; 0/0 is NaN, not the semantic +0 branch. This is deliberately a different formula from semantic association at zero. Raw output contains only structure and samples; callers provide any association and alpha interpretation to downstream operations. Original input samples supply every operand, including when the selected component is also the raw weight.

## Behavior

A computes premultiplied samples and B computes straight samples for the selected group. Neither writes a description into the result. A caller connecting A to B must supply B with an explicit premultiplied input description. No full-color validity is certified by producing only a subset.

The output alpha index refers to this output tensor. Alpha coverage is determined by the produced output tiles: a request for R can produce alpha as well when they share a spatial tile. Missing coverage is an ordinary access failure, with no hidden snapshot or manufactured zero/one value.

Each converted component depends on its input sample and its mapped alpha sample. Pass-through channels depend only on their own source. Scalar alpha contributes its sole sample only when a participating component is evaluated. Semantic preconditions cover the participating coordinates actually evaluated, including alpha zero or one. Violating a semantic premise promises no numerical result or diagnostic. Declared reads are not suppressed at alpha zero or one.

## Errors

Malformed parameters, descriptions, selectors, channel relationships and shapes fail static preparation. Semantic domain and finite-result premises are not runtime error checks.

## Acceptance

Independent rational/NUM bit references for both dtypes, semantic zero and subnormal alpha, raw NaN/Inf/zero behavior, all weight source kinds and exact pass-through samples; complete and selected output tiles; noncanonical axes; batch/spatial metadata; signed/zero/unaligned inputs.
