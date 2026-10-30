---
spec_schema_version: 1
id: GEN-08
kind: shared_operator_contract
category: 03-generation
status: Accepted
implementation_status: implemented
---

# GEN-08: Point distributions

Publish complete DynamicPoints results with Float64 positions, ordinal IDs and valid ID-to-row associations. Minimum-distance predicates operate on published points. Grid and jitter have fixed nx*ny counts and a hard capacity; rejection and FIFO have normal max_count stop targets plus separate hard limits. No maximal packing or density promise is made.

## DynamicPoints v1 schema

The operations-domain schema identifier is `photospider.ops.dynamic_points`, version 1. It has three Tensor members with a positive compile-time capacity P; the runtime point count is a payload value, never a Tensor extent. The SDK factory is `ps::generation::dynamic_points_schema(std::uint64_t capacity)` in `photospider/ops/generation.hpp`.

| Member | Type and shape | Meaning |
| --- | --- | --- |
| `positions` | Float64 `[P,2]` | Finite x/y positions in rows 0..count-1. |
| `ids` | Int64 `[P]` | Nonnegative unique IDs in the valid prefix. |
| `count` | Int64 `[1]` | Number of valid rows, 0<=count<=P. |

Generators assign IDs 0..count-1 in output order. A later filtering operation may preserve source IDs; consumers must not assume every input ID equals its row index. Rows count..P-1 are canonical padding: positions=(+0,+0), ids=0. Padding does not participate in geometry; the ID-uniqueness premise applies only to the valid prefix.

For rejection and Bridson, max_count may be zero and P=max(1,max_count). A zero target succeeds with count=0 and count_reached without taking any random sample. Bounds, distance and numerical parameter requirements are unchecked premises; only static structure is checked. Grid and jitter have P=nx*ny and count=P after checked capacity computation.

All three members are non-spatial, with no batch axes. `positions` has component axis 1 and atomic_trailing_axes=1, so an observation contains both coordinates. `ids` and `count` use scalar observations. The schema does not impose contiguous interleaved point storage. One CompleteBundle scope contains all three member slots; the operator delivers them together after complete coverage, including canonical padding. Counts, ID uniqueness and padding are unchecked premises. The members share row identity within this Result; no internal ObjectId foreign key is needed.

## Point generation report v1

GEN-08C and GEN-08D return a separate fixed report output with schema `photospider.ops.point_generation_report`, version 1, constructed by `ps::generation::point_generation_report_schema()`. Its members are `stop_reason`, Int64 `[1]`, and `points_object_id`, UInt8 `[8]`. Stop reasons are 0=count_reached, 1=candidates_exhausted, 2=active_exhausted. GEN-08C uses reasons 0/1 and GEN-08D uses 0/2. The actual point count is DynamicPoints.count. A zero normal target returns reason 0. Budget exhaustion and cancellation are failed executions, not report values. `points_object_id` encodes the nonzero 64-bit ObjectId of the complete points Result in eight little-endian bytes. The caller supplies the associated complete points object; a reader decodes the identity without comparing it to the points ObjectId or scanning either payload. Correct pairing and complete semantic contents are unchecked caller premises; equal content, equal schema or equal Run alone does not define the association.

The members are delivered through a two-stage workflow. First the producer publishes the final ordinary points value and the raw stop reason. The binder `generation.bind_point_generation_report_v1` reads all three points members and the raw reason, obtains the points input identity, and writes the bound report. The same complete points value is returned directly alongside the report. A copy or partial projection creates a different Result identity; any required copy happens before binding and the copy is bound instead. CompleteBundle metadata does not make a partial export complete. The public workflow helpers `poisson_rejection_points` and `bridson_points` implement this sequence.

The binder has deterministic=false because its ObjectId bytes can differ across Runs. Point coordinates, ordinal IDs, counts and stop_reason keep their deterministic behavior. The ObjectId association is valid only for the corresponding process object, not for serialized, recreated or cross-Run copies.

## Members

| ID | Specialization |
| --- | --- |
| [GEN-08A](GEN-08A_grid_points.md) | grid points |
| [GEN-08B](GEN-08B_jittered_grid_points.md) | jittered grid points |
| [GEN-08C](GEN-08C_poisson_rejection_points.md) | poisson rejection points |
| [GEN-08D](GEN-08D_bridson_points.md) | bridson points |

## Shared rules

Members inherit [GEN common](GEN_common_contract.md) and the [random contract](NOI_random_contract.md). The same version and profile fix numerical output and stop reasons; the report ObjectId bytes have the Run-local exception above.
