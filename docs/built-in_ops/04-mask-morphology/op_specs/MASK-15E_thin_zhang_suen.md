---
spec_schema_version: 1
id: MASK-15E
kind: primitive
category: 04-mask-morphology
status: Proposed
document_maturity: D1_draft
implementation_status: not_implemented
clarification_status: draft_for_review
parent_id: MASK-15
function: thin_zhang_suen
oracle_entry: thin_zhang_suen
proposed_operation_keys:
  - mask.thin_zhang_suen_strict
---

# MASK-15E: Zhang-Suen binary thinning

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

Let B be the sum of b0..b7. Let A count cyclic 0-to-1 transitions,
A=sum_i [not b_i and b_(i+1)]. Require 2<=B<=6 and A=1 in both phases.
Phase 0 additionally requires not(b0 and b2 and b4) and not(b2 and b4 and b6).
Phase 1 additionally requires not(b0 and b2 and b6) and not(b0 and b4 and b6).
Delete exactly when all these conditions hold for that phase.

These are the specified Zhang-Suen predicates, without added endpoint or
component-count guards. In particular an isolated 2x2 foreground block is
entirely deleted in phase 0. The topology guarantee of MASK-15A must not be
transferred to this member. Algorithm fidelity and geometric/topological
properties are separate assertions; no universal one-pixel-centerline promise
is made.

## Independent acceptance

Single pixels and a one-pixel horizontal chain remain unchanged; empty input
foreground remains empty. Test diagonal pairs, 2x2 blocks, thick rectangles,
T/X junctions, rings and edge-touching shapes. Check exact output, subset of
input and fixed-point idempotence. Exercise both floating dtypes and reject
nonbinary inputs. Compare any library only with aligned padding and phase order.

The oracle entry is `thin_zhang_suen` in
[reference.py](../../../../oracle/ops/mask_morphology/reference.py).
A real public Compiler/ExecutionContext workflow is required for native acceptance.
[S24](../research-sources.md#s24) provides algorithm and implementation references;
borrowing predicates does not import UInt8/255 storage or protected canvas edges.
