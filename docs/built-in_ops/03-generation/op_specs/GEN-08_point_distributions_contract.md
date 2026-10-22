---
spec_schema_version: 1
id: GEN-08
kind: shared_operator_contract
category: 03-generation
status: Accepted
implementation_status: not_implemented
---

# GEN-08: Point distributions

Publish complete DynamicPoints results with Float64 positions, ordinal IDs and valid
ID-to-row associations. Minimum-distance predicates operate on published points.
Grid/jitter have fixed nx*ny counts and hard capacity; rejection/FIFO have normal
max_count stop targets plus separate hard budgets. No maximal packing/density promise.

## DynamicPoints v1 schema

The operations-domain schema identifier is `photospider.ops.dynamic_points`, version 1. It has three Tensor members with a positive compile-time capacity P; the runtime point count is a payload value, never a Tensor extent.

| Member | Type and shape | Meaning |
| --- | --- | --- |
| `positions` | Float64 `[P,2]` | Finite x/y positions in rows 0..count-1. |
| `ids` | Int64 `[P]` | Nonnegative unique IDs in the valid prefix. |
| `count` | Int64 `[1]` | Number of valid rows, 0<=count<=P. |

Generators assign IDs 0..count-1 in output order. A later filtering operation may preserve source IDs; consumers must not assume every input ID equals its row index. Rows count..P-1 are canonical padding: positions=(+0,+0), ids=0. Padding does not participate in geometry; the ID-uniqueness premise applies
only to the valid prefix.

For rejection and Bridson, max_count may be zero and P=max(1,max_count). A zero target succeeds with count=0 and count_reached without taking any random sample. Bounds, distance and numerical parameter requirements remain unchecked premises; only static structure is checked. Grid and jitter have P=nx*ny and count=P after checked capacity computation.

All three members are non-spatial, with no batch axes. positions has component axis 1 and atomic_trailing_axes=1, so an observation contains both coordinates. ids and count have no component axis and use scalar observations. Physical strides and offsets follow the existing Tensor interface; the schema does not impose contiguous interleaved point storage.

One CompleteBundle scope contains all three member slots. The operator must deliver them together after complete coverage, including canonical padding. In fixed invocation ABI 8 the scope is schema metadata, not an automatic scheduling or coverage closure: lowering must explicitly arrange all required member tile writes and successful call retirement. Fixed tile tasks may still compute rows in parallel. Counts, ID uniqueness and padding are unchecked premises, with no producer or consumer validation pass. The members share row identity within this Result; no internal ObjectId foreign key is needed. This target schema does not establish an SDK implementation.

## Point generation report v1

GEN-08C and GEN-08D return a separate fixed report output with schema `photospider.ops.point_generation_report`, version 1. Its members are `stop_reason`, Int64 `[1]`, and `points_object_id`, UInt8 `[8]`. Stop reasons are 0=count_reached, 1=candidates_exhausted, 2=active_exhausted. GEN-08C uses reasons 0/1 and GEN-08D uses 0/2. Read the actual point count from DynamicPoints.count. A zero normal target returns reason 0. Budget exhaustion and cancellation remain failed executions rather than successful report values. The points_object_id member encodes the nonzero 64-bit ObjectId of the complete points Result in eight little-endian bytes. The caller supplies the associated complete points object. A reader decodes the identity without comparing it to the points ObjectId or scanning either payload. Correct pairing and complete semantic contents are unchecked caller premises; equal content, equal schema or equal Run alone does not define the association.

These members are delivered through a two-stage workflow. First produce the final ordinary Produced points value and the internal raw stop reason. A downstream report binder reads all three points members and the raw reason, obtains the points input_object_ids identity, and writes the bound report. Return the same complete points value directly alongside the report. A copy or partial projection creates a different Result identity; perform any required copy before binding and bind the copied value instead. CompleteBundle metadata does not make a partial export complete.

The report binder has deterministic=false because its ObjectId bytes can differ across Runs. Point coordinates, ordinal IDs, counts and stop_reason retain the specified deterministic behavior. ObjectId association is valid only for the corresponding process object, not serialized/recreated or cross-Run copies. This uses existing ABI 8 input identities and complete-value export; no binder or public DynamicPoints workflow is implemented yet.

## Members

| ID | Specialization |
| --- | --- |
| [GEN-08A](GEN-08A_grid_points.md) | grid points |
| [GEN-08B](GEN-08B_jittered_grid_points.md) | jittered grid points |
| [GEN-08C](GEN-08C_poisson_rejection_points.md) | poisson rejection points |
| [GEN-08D](GEN-08D_bridson_points.md) | bridson points |

## Shared execution and numerical rules

Inherit [GEN common](GEN_common_contract.md), and applicable
[random](NOI_random_contract.md) and [path geometry](PTH_geometry_contract.md) rules.
Members declare Regional, Halo or complete-input from actual mathematical dependencies,
independently of static structure checks. GPU capability is member/backend-specific.
Same version/profile fixes numerical output and stop reasons; report ObjectId bytes have the explicit Run-local exception above. Numerical error, geometry error and
topology guarantees are separate. Source associations are unchecked caller premises; producers preserve declared ownership and lifetime by construction.

## Acceptance and implementation boundary

Use member-specific independent fixtures plus actual public workflow execution. The
[oracle coverage](../oracle-coverage.md) identifies finite exact helpers, Measured
diagnostics and remaining unsupported cases. It cannot establish public schema support,
GPU admission or complete domain certification. The specified schemas and RNG mappings require implementation and validation before registration. Unresolved numerical solver or backend choices remain explicit member limitations.
