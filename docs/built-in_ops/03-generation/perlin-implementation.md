# Perlin 2002 strict Result execution

The registry exposes three Result operations: noise.perlin2002_3d_v1_strict_cpu_whole, noise.perlin2002_3d_v1_strict_cpu_tiled, and noise.perlin2002_3d_v1_strict_gpu. Each accepts one Result containing one numeric tensor coordinates[S...,3] and publishes the generic numeric tensor samples[S...] on the values output port. The implementation preserves the fixed Perlin 2002 permutation and exact integer polynomial, with one final IEEE-754 rounding.

## Result interface and demand

The input Result has one tensor member with any valid member key; coordinates names its semantic role in this operation. The tensor has Float32 or Float64 elements, rank 2..8, positive extents, and a final extent of three. The complete coordinate tensor contains at most 2^40 elements. Valid numeric facets are accepted on input; they do not change how coordinate bits are interpreted. The output uses schema photospider.tensor version 1, whose tensor key is samples and whose sample shape is S...; the operation output port is values. It carries no inferred color semantics. The optional static String parameter dtype selects float32 or float64; its default is float64. Upstream operations define coordinate construction, frequency, and units.

Whole and GPU declare Whole demand and dirty behavior: execution reads and validates all coordinates, and a change to any input sample dirties the full output. CPU tiled declares a Result Dependency-v2 map. For each requested output sample it requests the corresponding three coordinate components as Data and Validation support. Its output descriptor is declared separately. Empty output requests perform no input payload read and no Perlin arithmetic.

Tiled execution partitions each requested box into bounded output tiles. Leading sample axes advance one sample at a time; the last two sample axes use the requested tile height and width. The continuation publishes each completed tile in traversal order, retaining a valid Result prefix while more tiles remain. CPU tile callbacks run through the host cpu_tiles service for the current granted stage. This execution does not schedule a concurrent window of output tiles. The independent Whole path uses the host range-parallel service when available.

The tiled kernel first decodes every coordinate in the current tile, then evaluates those decoded values. Its decoded record occupies 72 bytes per output sample. The current declared input workspace allowance is six times the coordinate payload bytes for that tile; output and continuation storage are admitted separately. Work, scratch, output, continuation, and Result publication metadata remain subject to the execution root limits. Empty requests skip these allocations.

## Exact numerical contract

For each coordinate, binary input decoding obtains exact floor, modulo 256, and dyadic fractional values. The fade polynomial is

$$
f(t)=6t^5-15t^4+10t^3.
$$

The fixed 256-entry permutation and grad(hash & 15) rule select the eight corner gradients. The CPU computes the exact weighted sum with seven nested lerps; the native shader uses the expanded eight-corner polynomial. The permutation and gradient definition are specified in [NOI_perlin2002_permutation](op_specs/NOI_perlin2002_permutation.md). Both forms preserve the exact result and round only once to the requested output format, using ties-to-even. Integer lattice points return positive zero. A negative nonzero result that underflows to zero retains negative zero. Floating-point intermediate arithmetic is not used. The periodicity is 256 on each axis, the conservative magnitude bound is 2, and the contract does not claim strict [-1,1] range or Java bit identity.

With common denominator D=2^q, the conservative numerator bound needs at most 16q+5 bits. CPU arithmetic uses 8, 16, or 272 64-bit limbs for q<=31, q<=63, or q<=1074. GPU scratch uses 17 little-endian base-2^32 integers per active lane and selects 16, 32, or 544 words at the same tier boundaries. Float64 values travel as bits, so device Float64 arithmetic support is not required.

Each CPU sample reserves a conservative arithmetic work bound before its multiplications and checks the local credit during evaluation. At q=1074, PerlinExact::work_bound is 763,702 units per sample. Coordinate decode and addressing are charged before the reads. GPU admission scans input bits to select the limb tier and bound device work; shaders compute all output values. Work is a modeled operation count, not a device instruction count.

## Native GPU memory and execution

The GPU registration requires the native GPU service and has no CPU fallback. It submits the MSL/Metal or SPIR-V/Vulkan shader selected by that service. Each dispatch writes a disjoint output range and reuses scratch only after synchronous completion. Fast tiers use batches of at most 256 samples and group up to eight dispatches per synchronous submission. The full precision tier uses at most four samples per dispatch and one dispatch per submission. The largest scratch allocation is 557,056 bytes in the q<=63 tier; the q>63 full precision tier uses at most 147,968 bytes.

An authorized affine Result read window keeps its backing owner, byte offset, signed strides, and logical origin alive through shader execution. A same-device native owner can be bound directly without a transfer. When the authorized window is not one physical affine view, the host materializes the requested window into a dense native input buffer and accounts for that transfer. The shader address calculation supports negative and broadcast strides, padding, nonzero origins, and unaligned coordinates. Result owners retain published output after the execution context retires.

Metal has a 232-byte argument block. Vulkan packs a 432-byte std140 argument block and requires 64-bit integer arithmetic and 8-bit storage-buffer support. Installed-consumer checks pass for the test and example targets. ## Failures and limits

Nonfinite coordinates fail validation. Invalid shape or dtype metadata fails specialization. Arithmetic work, allocation, stage, or cancellation failures do not publish a successful final result; tiled execution may already have published earlier tiles as a Result prefix. Cancellation stops later GPU submissions, while submitted synchronous work retires before its owners are released. A missing GPU service or unsupported backend returns a backend error without CPU fallback.

## Reproduction

The public [workflow example](../../../examples/perlin_workflow/README.md) uses Result bindings and the values output port. The GPU checks include same-device affine input with zero transfer, two-chunk input with one transfer of 48 bytes, retained output after context retirement, cancellation release, and CPU bitwise comparison. The example sets the dependency stage limit to 3*N+1 for its requested sample count N.

The tiled fixture requests y=[3,14), x=[5,32) from a 17-by-35 coordinate field with tile height 4 and width 8. It checks 12 bounded source reads totaling 7,128 bytes, 12 ordered tile deliveries covering 297 samples, and ignores a NaN outside the requested region. A separate Float32 multi-box request covers 15 samples and checks finite-work, scratch-capacity, and in-region NaN errors. The independent Fraction oracle checks a finite fixture set, not the full mathematical domain or all GPU devices.
