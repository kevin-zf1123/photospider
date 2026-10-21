# NUM-06 fixed range execution

The six `numeric.clamp_*` and `numeric.remap_range_*` profile definitions use fixed CPU tile invocations through ABI 8. Each selected output tile becomes one task. The strict, Apple Silicon and x86-64 keys fix the numerical profile; preparation rejects unavailable host profiles.

## Tensor and Plan contract

Each input Result contains one tensor member with any valid key. Complete `sample_shape()` includes batch axes and must match across ports, with rank 1..8, positive extents and at most 2^40 samples. Clamp accepts UInt8, Int64, Float32 or Float64 across three ports: input, lower, upper. Remap accepts Float32 or Float64 across five ports: input, source lower/upper, target lower/upper. All ports share dtype.

Output `values` uses schema `photospider.tensor` v1 and member `samples`. It preserves dtype and the complete shape as ordinary axes. Lowering declares every input tile intersecting the requested output region, using each input's own spatial, batch and atomic grid. Prepare checks complete fixed slot coverage, dtype, region, Host placement and scratch. The kernel borrows admitted affine input views and writes one separate preallocated dense output tile. Empty selection emits no task; sparse selection computes only selected complete tiles.

## Numerical preconditions and results

Clamp requires non-NaN bounds with lower <= upper; equal and infinite bounds are valid. Remap requires finite bounds with strictly increasing source endpoints; target endpoints may increase, decrease or be equal. These are unchecked caller preconditions. The kernel performs no bounds-validation scan or promised InvalidBounds reporting.

Clamp quiets input NaNs while preserving sign/payload. For other inputs it selects lower below the interval, upper above it, or copies input bits inside it. Signed zeros compare equal, so `clamp(-0,+0,+0)` returns `-0`. Integer ordering uses exact unsigned or sign-adjusted keys without float conversion.

Remap quiets input NaNs first. Equal target endpoints return target-lower bits; exact source endpoints copy their corresponding target bits. An infinite input produces the infinity sign determined by the slope. Finite inputs compute `t0 + (x-s0)*(t1-t0)/(s1-s0)` as one exact rational before final rounding. Strict uses exact destination rounding; accelerated profiles may accept a quotient enclosure within the shared final FP32-scaled bound. Overflow produces signed infinity, gradual underflow preserves the exact sign, and exact non-special zero is positive zero. Endpoint copies retain supplied signed zeros.

## Scratch, cancellation and ownership

Each task declares `sizeof(RangeMath)` Host scratch. Block admission reserves one distinct workspace per active worker lane before issue. Prepared grids, slot maps and profile remain immutable across Runs; mutable comparison and rational state live only in invocation scratch. Runtime uses bounded stack coordinates and cached input views, with no payload allocation.

The invocation establishes nearest rounding with gradual underflow and restores the worker environment. Exact-arithmetic checkpoints query cancellation per sample and during refinement without dynamic sample-work charges. Accepted callbacks retain input/output/scratch until return. Failure discards unpublished output, while successful Results retain output owners after context destruction.

## Validation scope

Use temporary public execution checks with exact integer/bit references for clamp and independent rational references for remap. Include extreme source widths, reversed and constant target intervals, signed-zero endpoints, NaN payloads, sparse/empty selections, distinct input grids, signed strides, imported Plans, concurrent Runs and rejected malformed task signatures. Report actual supported hardware separately from source registration and do not infer GPU execution or performance from CPU correctness checks.
