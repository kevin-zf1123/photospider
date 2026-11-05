---
spec_schema_version: 1
id: GRD-30A
parent_id: GRD-30
function: halftone_dot
kind: primitive
category: 07-grade
status: Proposed
document_maturity: D1_draft
implementation_status: implemented
clarification_status: complete
execution: tile
proposed_operation_keys:
  - grade.halftone_dot
---

# GRD-30A: Circular-dot halftone

Inherits the [family contract](GRD-30_halftone_contract.md) and [GRD common contract](GRD_common_contract.md). The proposed tile operation `grade.halftone_dot` performs the transformation below.

## Interface

`coverage: T[H,W]` -> `ink: T[H,W]`.

Coverage in [0,1]; edge=reject|clamp defaults reject. Required static basis:Float64[2,2] with columns bx,by in pixels, nonsingular, and origin:Float64[2]. samples_per_axis:Int64 1..16 default 4.

Port ordering, tensor members, calculation dtype, channel selection, static parameter types and unchecked premises follow the common contract. Output shape and dtype are those in the signature.

## Mathematics

At each SxS subpixel center (x+(i+0.5)/S,y+(j+0.5)/S), transform position-origin by inverse(basis), then take fractional u,v. d2=(u-0.5)^2+(v-0.5)^2. Define A=pi*d2 for d2<=1/4; otherwise A=pi*d2-4*d2*acos(1/(2*sqrt(d2)))+2*sqrt(d2-1/4). A(0)=0,A(1/2)=1. Count a subpixel as ink iff A<coverage. Output the count/S^2. Coverage 0/1 selects exact 0/1.

## Behavior

E on coverage plus absolute geometry. Coverage is constant within its output pixel; SxS averaging defines the sampled area approximation. An ink value of 1 means fully covered.

## Acceptance

Zero/full coverage returns 0/1. Translate both origin and image coordinates equally to preserve the pattern. Check a rotated nonsingular basis.

Apply the common native-versus-strict comparison rules and compare ROI/tile observations at identical global coordinates. Reference equations and source attribution are in the family contract and [research sources](../research-sources.md).
