---
spec_schema_version: 1
id: NOI-04A
parent_id: NOI-04
function: perlin2002_3d
kind: primitive
category: 03-generation
status: Accepted
implementation_status: not_implemented
clarification_status: member_technical_details_pending
proposed_operation_keys:
  - noise.perlin2002_3d_v1_strict_cpu_whole
  - noise.perlin2002_3d_v1_strict_cpu_tiled
  - noise.perlin2002_3d_v1_strict_gpu
---

# NOI-04A: perlin2002 3d

Inherit [NOI-04](NOI-04_gradient_noise_contract.md), [GEN common](GEN_common_contract.md) and the applicable [geometry](PTH_geometry_contract.md) rules. The fixed permutation and gradient selection are specified in [NOI_perlin2002_permutation](NOI_perlin2002_permutation.md). This member uses its fixed permutation independently of the specified Philox v1 addressed-sequence protocol. Upstream operations construct coordinates and define their units.

The listed keys are proposed implementations. Current production sources and the bundled registry provide no NOI-04A operation; the retained Python oracle supplies mathematical references only.

## Result interface

Each proposed operation accepts one Result input containing one numeric tensor `coordinates[S...,3]`. The input schema selects its sole tensor member, so callers may choose any valid member key; `coordinates` is the semantic role. Elements are Float32 or Float64, rank is 2..8, every extent is positive, and the complete tensor contains at most 2^40 scalar elements. The `values` output is a generic `photospider.tensor` version 1 Result with tensor key `samples` and shape `S...`. It carries structure and numerical samples; coordinate units and any downstream color interpretation remain explicit operation configuration. Optional static String `dtype` is `float32` or `float64`, default `float64`.

## Shared mathematical behavior

Use the fixed 256-entry permutation, exact floor and modulo-256 reduction, quintic fade `6t^5-15t^4+10t^3`, and the specified `grad(hash & 15)` corner rule. Evaluate the eight exact corner contributions and round the complete polynomial once to the requested IEEE output format, ties-to-even. Integer lattice inputs produce positive zero; a negative nonzero value that underflows retains negative zero. The period is 256 on each axis and the conservative magnitude bound is 2. Strict output follows this mathematical expression rather than Java's intermediate double rounding. Coordinates are the only numerical input; seed, custom permutation, origin and frequency are not implicit parameters.

## Fixed execution contract

Implement these members through fixed invocation ABI 8 using immutable preparation, statically declared tasks, explicit input-member tile reads, output tile writes and admitted scratch. Worker assignment does not change output bits. Each source read closes on its own tile grid; an affine view's physical tile closure may include samples outside the mathematical coordinate support.

The CPU complete-input and GPU variants read the complete coordinate tensor for a nonempty request. Their declared dependencies cover that full input. CPU parallel work uses separate compiled tasks and their dependencies. The CPU tiled variant computes each retained output tile from the corresponding complete xyz triples and assumes those consumed triples are finite. Descriptor-only facts use schema reads. Empty output requests retain static preflight and emit no payload task.

The GPU variant requires an implemented and validated native backend with explicit transfers, device program identity, scratch and completion ownership. Its strict polynomial result must match the mathematical contract. GPU unavailability remains an explicit failure. Metal and Vulkan implementation and runtime validation are outstanding.

The executor publishes successful call Results in retirement order after all required tasks succeed. A failed required task prevents successful publication of that call's output; this member does not promise a usable partial output from a failed call. Published storage owners remain valid after context teardown.

## Resources and failures

Preparation checks dtypes, shapes, layouts, task signatures, exact read/write slots and scratch capacity. Charge source tile records, transfers, exact-integer workspace and output backing through existing kernel facilities. Arithmetic workspace bounds, refinement tiers and native dispatch sizes must be established for the actual implementation; the oracle does not supply production allocation or throughput guarantees. Poll cancellation during bounded arithmetic and iteration work.

Shape/type mismatches fail static preparation. Consumed coordinates are finite as an unchecked caller premise. Arithmetic capacity/work refusal, cancellation and unavailable backends preserve their operational status; incomplete certification cannot produce an approximate substitute. No publication or consumption stage validates coordinate payloads.

## Independent reference and acceptance

The retained [Fraction oracle](../../../../oracle/ops/generation/noi04/rng.py) evaluates the fixed polynomial; [reference samples](../../../../oracle/ops/generation/noi04/samples.py) enumerate Float32/64 inputs and outputs, negative coordinates, subnormals and large finite coordinates. These sources establish finite reference cases, not current C++ or GPU execution.

Temporary public-workflow acceptance must cover the declared schema and dtypes, whole/tiled equivalence, nonzero and disjoint requests, finite consumed coordinates and exact declared read support, exact source tile closure, capacity/work refusal, cancellation and retained output lifetime. Each GPU backend additionally needs actual native dispatch and transfer/completion checks against the independent reference. Preserve the oracle and remove temporary operation validators after implementation validation, following the repository policy. These acceptance cases remain unexecuted for a current NOI-04A implementation.
