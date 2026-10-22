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

The default registry provides `alpha.associate_<profile>` for the three CPU
profiles. The operation uses fixed invocation ABI 8 and preserves the declared tensor
shape, dtype and channel positions. The specification remains Proposed. Validate the public operation with temporary independent numerical and ownership checks when changing it.

Inherit the complete [FMT-04 boundary contract](FMT-04_alpha_association_contract.md).
The operation accepts one single-tensor Result, with optional second input only for raw explicit plane/scalar weights.

## Interface and representation

Semantic `input` contains one selected straight RGB/Gray group and its distinct
internal alpha plane. Output `values` has identical shape, dtype and channel
positions, with premultiplied samples for the selected group. No description is attached.
It is not a canonical complete image. Use FMT-02/05 first when alpha starts as a
separate plane/scalar or the original image has no alpha. No semantic external
alpha port or persistent alpha relation is accepted. Unselected groups/AOVs pass
through as exact sample copies.

Float32/Float64 and strict/named CPU profiles inherit the family. `input_description` is an explicit
call-local source interpretation; it does not turn the adapter output into
straight or bypass the internal-alpha requirement. Repeated association of an
already premultiplied selected group fails preflight.

## Mathematical specialization

For each evaluated color coordinate, read x and internal a. The caller supplies
finite x and finite a in [0,1]. At either signed zero alpha, output color +0; otherwise strict output is
RN_t(x*a). Copy alpha and nonparticipating samples bit-for-bit. The finite-color premise also applies at alpha=0. Signed/HDR colors are legal and no [0,a] clamp applies.
Encoded C=0.5 with a=0.5 produces P=0.25 without transfer decoding. Hidden color
at zero alpha is lost in this boundary conversion; no auxiliary capture is made.
A binary Gray stored-value guarantee cannot generally survive multiplication.

Raw instead uses NUM-05C multiply(x,a), permits explicit internal/plane/scalar
weights and follows NUM NaN/Inf/signed-zero/overflow behavior. It does not apply
semantic zero clearing or declare successful premultiplied boundary conversion.
Raw output carries no description or sample-validity guarantee.

## Dependencies, resources and failures

Each complete retained output tile declares its participating source components
and mapped alpha in their own source grids. Pass-through components declare only
their original samples. Both operands remain required at alpha=0/1. Static group,
state and shape checks apply to every call, including Empty. Output allocations
are independent and become public at successful ordered call retirement.

The output retains its alpha slot structurally. A downstream call explicitly
requests the result's alpha when needed. A selected output tile can include more
channels than an original sample request; missing detached coverage remains an
ordinary missing-coverage error. No hidden input-alpha binding is retained.

## Independent fixtures and acceptance

Straight Float32 RGBA [-2,0.5,4,0.5] yields boundary samples [-1,0.25,2,0.5].
For the same finite colors and alpha=-0, color outputs are +0 and alpha remains
-0. A participating NaN color at alpha=0 violates the caller preconditions.
Additional source-tile samples outside evaluated components add no premises.

For Float32 x=1 and a=2^-149, output is 2^-149. For x=0.5 with that alpha,
ties-to-even yields +0; x=-0.5 yields -0. Repeat Float64 at 2^-1074. At a=1,
finite samples including -0 preserve bits. These products can
underflow and later unassociation need not recover their original colors.

For a Gray+alpha tensor with samples [8,0.25] and [2,1], output samples are
[2,0.25] and [2,1]. Keep the internal alpha channel in the shape;
external alpha must first be explicitly assembled. Check a deliberately
noncanonical axis/channel map and R-only, alpha-only and AOV-only requests.
A copy-only output tile may preserve a NaN bit pattern without color-domain validation.

Raw fixtures include Inf*0 -> NUM NaN, -0*1 -> -0, operand-order NaN priority
and weights outside [0,1]. Semantic and raw results cannot share an incorrect
compiled invocation identity. A normal straight-image consumer requires the
caller to pair this boundary payload with an appropriate conversion or explicit
interpretation. B explicitly restores straight samples, subject to its required coverage.

Conceptual DAG: straight image with internal alpha -> A -> export adapter.
The native Result implementation uses the public helper and registry contract described above. Runtime and performance claims are limited to checks actually run on the target host.
