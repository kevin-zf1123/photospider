---
spec_schema_version: 1
id: MASK-15F
kind: primitive
category: 04-mask-morphology
status: Proposed
document_maturity: D1_draft
implementation_status: not_implemented
clarification_status: draft_for_review
parent_id: MASK-15
function: thin_guo_hall
oracle_entry: thin_guo_hall
proposed_operation_keys:
  - mask.thin_guo_hall_strict
---

# MASK-15F: Guo-Hall binary thinning

Inherit [MASK-common](MASK_common_contract.md) and the
[family contract](MASK-15_topology_reconstruction_contract.md), including the
01-numeric rules. This member defines its own synchronous deletion predicates;
MASK-15A/B's immediate-update and endpoint rules do not apply.

## Ports, parameters and output inference

input: Binary[H,W] -> skeleton: Binary[H,W], same Float32/Float64 dtype and shape.
No static parameters. Consume exact 0/1; reject soft values and NaN/Inf.
Canonicalize output zero to +0. The output is an index-grid thinning, with no
physical-spacing interpretation, radius output or implicit pruning.

## Schedule, boundary and demand

Use zero background outside the canvas. Every in-canvas foreground pixel,
including canvas-edge pixels, is eligible under the predicate below. A cycle
runs phase 0 then phase 1. Each phase evaluates all candidates from its immutable
phase-start image, then deletes all selected pixels simultaneously. Phase 1
uses phase 0's completed output. Stop only after a complete cycle deletes no
pixels. Thread order and tile decomposition cannot affect this schedule.
There is no successful early iteration cutoff. At most H*W successful cycles
are possible, followed by the terminating cycle. A work/stage budget failure
returns ResourceExhausted, and cancellation publishes no partial skeleton.

Any nonempty output request requires Whole input validation and iterative
computation; an empty Region requires descriptors only. Input changes may dirty
all output pixels. Charge the input retention, output/state and deletion mask;
reference work is O(iterations*H*W), scratch O(H*W). Poll at the common cancellation
bound, including predicate scans. The local 3x3 predicate does not imply a fixed
halo for the converged output. Mathematical references do not certify native
publication, resource accounting or exact Region reads.

## Neighborhood

Let b0..b7 be Boolean neighbor membership clockwise from north:
N, NE, E, SE, S, SW, W, NW. Indices below are modulo 8. The center must be
foreground to be deleted. Boolean AND/OR/NOT are exact; sums count true terms.

## Deletion predicate

C=sum_{i in {0,2,4,6}} [not b_i and (b_(i+1) or b_(i+2))].
N1=sum_{i in {0,2,4,6}} [b_(i-1) or b_i].
N2=sum_{i in {0,2,4,6}} [b_i or b_(i+1)]. Let N=min(N1,N2).
Phase 0 gate M=b6 and (b4 or b5 or not b7).
Phase 1 gate M=b2 and (b0 or b1 or not b3).
Delete exactly when C=1, 2<=N<=3 and M is false.

The fixed phase order is part of the member. No extra row-major deletion,
pruning, distance ordering or tie randomization is applied. The result need
not match Zhang-Suen or the project's distance-ordered thinning.

## Independent acceptance

Single pixels and a one-pixel horizontal chain remain unchanged; empty input
foreground remains empty. Test diagonal pairs, 2x2 blocks, thick rectangles,
T/X junctions, rings and edge-touching shapes. Check exact output, subset of
input and fixed-point idempotence. Exercise both floating dtypes and reject
nonbinary inputs. Compare any library only with aligned padding and phase order.

The oracle entry is `thin_guo_hall` in
[reference.py](../../../../oracle/ops/mask_morphology/reference.py).
A real public Compiler/ExecutionContext workflow is required for native acceptance.
[S24](../research-sources.md#s24) provides algorithm and implementation references;
borrowing predicates does not import UInt8/255 storage or protected canvas edges.
