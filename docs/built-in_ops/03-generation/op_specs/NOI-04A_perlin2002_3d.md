---
spec_schema_version: 1
id: NOI-04A
parent_id: NOI-04
function: perlin2002_3d
kind: primitive
category: 03-generation
status: Accepted
implementation_status: implemented_subset
clarification_status: gpu_implementation_validated_on_intel_vulkan
operation_keys:
  - noise.perlin2002_3d_v1_strict_cpu_whole
  - noise.perlin2002_3d_v1_strict_cpu_tiled
  - noise.perlin2002_3d_v1_strict_gpu
---

# NOI-04A: perlin2002 3d

Inherit [NOI-04](NOI-04_gradient_noise_contract.md), [GEN
common](GEN_common_contract.md), and the applicable [random](NOI_random_contract.md) and
[geometry](PTH_geometry_contract.md) contracts. The three operation keys listed above are registered. A key includes algorithm version and profile; within one version/profile, output bits and discrete decisions are fixed.

## Interface and representation

coordinates[S...,3] finite Float32/64; values[S...], rank>=1.

## Parameters and domain

dtype default Float64; no seed/frame/custom permutation; period 256 on each axis.

## Mathematical specialization

Exact floor and modulo256; fade(f)=6f^5-15f^4+10f^3. Use the fixed 256-entry permutation
and grad(hash&15): u=x when h<8 else y; v=y when h<4, x for h=12/14, otherwise z; bits
0/1 choose signs. Sum all eight weighted corner dots before RN. See
NOI_perlin2002_permutation.md.

## Registered CPU implementations

`noise.perlin2002_3d_v1_strict_cpu_whole` is registered. Its single positional
input is generic `coordinates[S...,3]`, Float32 or Float64, with S rank 1..7
and complete coordinate element count <=2^40. Output `values[S...]` is generic
numeric data. Optional static String `dtype` is `float32` or `float64`, default
`float64`. There are no seed, permutation, origin, frequency or hidden color
parameters. Upstream nodes define the coordinates and their units.

This explicit Whole form computes and validates all coordinates. A source edit
invalidates the complete output. It uses only CPU, with synchronous host ranges
and a 1..64 worker grant; changing the grant preserves output bits. Three
fixed-capacity integer tiers cover common fractional denominator q<=31, q<=63
and the entire binary64 domain q<=1074. All input coordinates are interpreted
exactly and only the final result is rounded. Nonfinite coordinates reject;
no result is published after any domain, work, allocation or cancellation error.
Scratch is allocated through the invocation allocator before workers execute and
retired only after the range barrier. The current declared workspace conservatively
reserves 64 complete slot workspaces, even for a smaller host quota.

`noise.perlin2002_3d_v1_strict_cpu_tiled` implements the Regional rule below,
with the same ports, parameter and exact arithmetic. A static Dependency-v1 map
requires all three coordinates only for each requested output observation.
Every tile callback executes on one host thread; the kernel controls tile
concurrency and ordered publication. Managed decoded scratch requires 72 bytes
per output sample. Empty requests perform no payload reads.

`noise.perlin2002_3d_v1_strict_gpu` uses Whole demand and invalidation, with the
same ports and final exact IEEE result. Metal executes its MSL integer shader;
Vulkan executes a SPIR-V 1.5 shader generated from `perlin.slang`. Both compute
the complete integer polynomial; host finite-input admission and work bounding do
not calculate output values. The GPU entry has no CPU fallback. Vulkan also
requires device 64-bit integer and 8-bit storage-buffer features. The FreeBSD
Intel UHD 770 implementation passes the independent 1,566-case Fraction/IEEE
comparison and focused GPU tests. NVIDIA for this Perlin port and Linux hardware
remain untested. The accepted CPU mathematical contract and CPU/SIMD behavior are
unchanged. See [Perlin implementation](../perlin-implementation.md) and the
[public workflow](../../../../examples/perlin_workflow/README.md) for registration,
native execution, independent bit comparisons and platform evidence.

## Dependencies, resources and failures

Execution rule: **Regional**. Compute requested output coordinates only. Read
corresponding query/sample inputs and necessary controls; path/mesh/resource validation
may require complete control data without computing the whole output canvas. Full
shared-control/topology/resource changes conservatively invalidate dependent output;
sample changes follow the actual dependency map. Empty requests perform static preflight
without payload reads or advancing a random sequence. Compute only requested output
mathematics, retaining required shared validation.

Regional coordinate samples, O(8*count(Q)); arbitrary finite coordinate reduction is
exact.

All output, scratch, indices, validation work and retained owners are accounted. Poll
cancellation in bounded loops. Preserve schema, domain, NoSolution, NotConverged,
InvalidQuality, ArithmeticOverflow and host resource failures as distinct outcomes.
Never replace budget/cancellation failure with a lower-quality successful prefix. Views,
lifetime and actual returned coverage inherit GEN-common.

## Independent fixtures and acceptance

Integer lattice +0, period256, negative and huge coordinates, independent Fraction
polynomial; conservative abs(noise)<=2, no strict [-1,1] claim or Java bit identity.

Check requested-region/full-output equivalence, actual read/work bounds, legal
strides/offsets, separate/joint outputs, cache identity, low budgets, cancellation,
associations and retained owners as applicable. The [oracle
coverage](../oracle-coverage.md) describes the available finite reference subset; it does
not exhaust the mathematical input domain or certify every GPU device. Vulkan Perlin
results on Intel UHD 770 match the independent oracle for the recorded finite fixture set;
this does not establish NVIDIA, Linux, or other Vulkan implementations.
