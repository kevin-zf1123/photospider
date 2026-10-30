---
spec_schema_version: 1
id: FMT-04A
parent_id: FMT-04
function: associate_alpha
kind: primitive
category: 02-format-color
status: Proposed
document_maturity: D1_draft
implementation_status: implemented_cpu_fixed_abi_8
clarification_status: complete
registered_operation_keys:
  - alpha.associate_strict
  - alpha.associate_accelerated_apple_silicon
  - alpha.associate_accelerated_x86_64
---

# FMT-04A: export one group as premultiplied numeric samples

The registry provides `alpha.associate_<profile>` for the three CPU profiles. The operation preserves the declared tensor shape, dtype and channel positions.

Inherits the [FMT-04 boundary contract](FMT-04_alpha_association_contract.md). The operation accepts one single-tensor Result, with an optional second input only for raw explicit plane/scalar weights.

## Interface

Semantic `input` contains one selected straight RGB/Gray group and its distinct internal alpha plane. Output `values` has identical shape, dtype and channel positions, with premultiplied samples for the selected group. No description is attached; it is not a canonical complete image. Use FMT-02/05 first when alpha starts as a separate plane/scalar or the original image has no alpha. No semantic external alpha port is accepted. Unselected groups/AOVs pass through as exact sample copies.

Float32/Float64 and strict/named CPU profiles inherit the family. `input_description` is an explicit call-local source interpretation. Association of an already premultiplied selected group is rejected during preparation.

## Mathematics

For each evaluated color coordinate, read x and internal a. The caller supplies finite x and finite a in [0,1], also at alpha=0. At either signed zero alpha, output color +0; otherwise strict output is RN_t(x*a). Alpha and nonparticipating samples are copied bit-for-bit. Signed/HDR colors are legal and no [0,a] clamp applies. Encoded C=0.5 with a=0.5 produces P=0.25 without transfer decoding. Hidden color at zero alpha is lost in this boundary conversion. A binary Gray stored-value guarantee cannot generally survive multiplication.

Raw mode uses NUM-05C multiply(x,a), permits explicit internal/plane/scalar weights and follows NUM NaN/Inf/signed-zero/overflow behavior. It does not apply semantic zero clearing. Raw output carries no description or sample-validity guarantee.

## Behavior

Each output sample depends on the participating source component and its mapped alpha; pass-through components depend only on their own sample. Both operands are required at alpha=0/1. Static group, state and shape checks apply to every call, including Empty. The output retains its alpha slot structurally; missing detached coverage is an ordinary missing-coverage error.

## Acceptance

Straight Float32 RGBA [-2,0.5,4,0.5] yields boundary samples [-1,0.25,2,0.5]. For the same finite colors and alpha=-0, color outputs are +0 and alpha remains -0. A participating NaN color at alpha=0 violates the caller preconditions.

For Float32 x=1 and a=2^-149, output is 2^-149. For x=0.5 with that alpha, ties-to-even yields +0; x=-0.5 yields -0. Repeat Float64 at 2^-1074. At a=1, finite samples including -0 preserve bits. These products can underflow and later unassociation need not recover their original colors.

Gray+alpha samples [8,0.25] and [2,1] yield [2,0.25] and [2,1]. Check a noncanonical axis/channel map and R-only, alpha-only and AOV-only requests. A copy-only output may preserve a NaN bit pattern.

Raw fixtures: Inf*0 -> NUM NaN, -0*1 -> -0, operand-order NaN priority and weights outside [0,1].

## Conceptual DAG

straight image with internal alpha -> A -> export adapter
