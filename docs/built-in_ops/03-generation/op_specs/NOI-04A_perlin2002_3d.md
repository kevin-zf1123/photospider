---
spec_schema_version: 1
id: NOI-04A
parent_id: NOI-04
function: perlin2002_3d
kind: primitive
category: 03-generation
status: Accepted
implementation_status: implemented_subset
clarification_status: result_gpu_validated_on_metal
operation_keys:
  - noise.perlin2002_3d_v1_strict_cpu_whole
  - noise.perlin2002_3d_v1_strict_cpu_tiled
  - noise.perlin2002_3d_v1_strict_gpu
---

# NOI-04A: perlin2002 3d

Inherit [NOI-04](NOI-04_gradient_noise_contract.md), [GEN common](GEN_common_contract.md), [random](NOI_random_contract.md), and [geometry](PTH_geometry_contract.md). The fixed permutation and gradient selection are specified in [NOI_perlin2002_permutation](NOI_perlin2002_permutation.md). Upstream operations construct coordinates and define their units.

## Result interface

Each operation accepts one Result input containing one numeric tensor coordinates[S...,3]. The input schema selects its sole tensor member, so callers may choose any valid member key; coordinates is the operation's semantic role. Elements are Float32 or Float64, rank is 2..8, every extent is positive, and the complete tensor contains at most 2^40 scalar elements. Valid numeric facets may accompany the input. The output port is values and publishes a generic photospider.tensor version 1 Result whose tensor key is samples and whose shape is S...; it has no inferred image or color semantics. Optional static String dtype is float32 or float64, default float64.

## Shared mathematical behavior

The implementation uses the fixed 256-entry permutation, exact floor and modulo-256 reduction, the quintic fade 6t^5-15t^4+10t^3, and the specified grad(hash & 15) corner rule in [NOI_perlin2002_permutation](NOI_perlin2002_permutation.md). It evaluates the eight exact corner contributions and rounds once to the requested IEEE output format, ties-to-even. Integer lattice inputs produce positive zero; a negative nonzero value that underflows to zero retains negative zero. The period is 256 on each axis and the conservative magnitude bound is 2; the contract does not claim strict [-1,1] range or Java bit identity. No seed, custom permutation, coordinate origin, frequency, or hidden color parameter is provided.

## Execution contracts

The CPU Whole and GPU operations request and validate the complete coordinate tensor. A source change dirties the complete output. CPU Whole uses host range parallelism when available; worker count does not change result bits.

The CPU tiled operation uses a Result Dependency-v2 map. Each requested output sample reads the corresponding three coordinate components as Data and Validation support. Descriptor support is declared independently. It partitions requested boxes into bounded tiles and publishes completed output in traversal order, so a Result prefix can exist while later tiles remain. The host cpu_tiles service schedules the granted stage; tile callbacks do not create their own threads. An empty request reads no coordinate payload and performs no Perlin arithmetic.

All three operations reject nonfinite coordinates. The native GPU operation requires a supported native backend and has no CPU fallback. Metal and Vulkan use integer shaders. The independent Fraction oracle covers CPU Whole, CPU tiled and Metal GPU outputs.

## Resources and failures

CPU arithmetic reserves a conservative per-sample bound before evaluation and checks local credit during multiplication. The maximum current bound is 763,702 units per sample at q=1074. CPU tiers use 8, 16, and 272 64-bit limbs for q<=31, q<=63, and q<=1074. GPU tiers use 16, 32, and 544 32-bit words per integer and 17 integers per active sample. The full GPU tier processes at most four samples per dispatch.

Shape/type errors fail specialization. Nonfinite data, work exhaustion, allocation failure, cancellation, or an unavailable native backend fails execution with its corresponding status. A tiled Result may retain successfully published prefix tiles if a later tile fails; it does not mark that prefix as a complete output.

## Acceptance and implementation evidence

The CPU Whole and tiled integration tests exercise Result bindings, output schema, exact reads, nonzero and disjoint regions, outside-region NaN, finite work and scratch failures, and cancellation/error paths. For a 17-by-35 Float64 field, the tiled ROI y=[3,14), x=[5,32) yields 12 source reads totaling 7,128 bytes and 12 ordered publications covering 297 samples. The native Metal Result test covers dispatch, affine native input, cancellation, bitwise CPU comparison, and result lifetime. The oracle covers a finite fixture set rather than the complete input domain or every GPU device.

See [Perlin implementation](../perlin-implementation.md) for current execution details and the [public workflow](../../../../examples/perlin_workflow/README.md) for build and run commands.
