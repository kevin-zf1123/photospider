## Implementation

The point-math callback now caches its output base pointer once. A packed input
block is copied into its accounted workspace in one operation; Float32 lanes are
classified once into a 64-bit rejection mask. Rejected input bits are retained
for exact fallback and replaced with zero before SIMD arithmetic. The block
result is copied out once and only rejected lanes are repaired. No output is
published if admission, cancellation or fallback fails. The mathematical SIMD
kernels and their coefficients/FMA graphs are unchanged.

Coordinate increments are skipped only when every input port is packed. Mixed
packed/transposed binary ports and arbitrary strides keep coordinate addressing.
The generic SLEEF batch and scalar paths also use the cached output pointer and
conditional coordinate advancement. Per-block indexing and fixed arithmetic
charges, per-fallback work, cancellation boundaries and fenv restoration remain
in place. Bit copies support unaligned storage and only cover valid tail lanes.

FP64 NUM-04 exp restores SLEEF 3.9.0 `xexp` in [-80,80]. Both scalar and batch
paths retain binary64 inputs/output; the candidate is enclosed by four adjacent
binary64 steps and admitted only by the existing final FP32-scaled accuracy
guard. This is the existing accelerated Float64 contract, not a new global
four-ULP64 guarantee. Float32 NUM-04 exp still uses IQK. Exact special cases and
out-of-domain/range uncertainty retain strict evaluation.

NUM-01 also restores the binary64 SLEEF exp endpoint enclosures for its RN64
AST. This does not substitute the Float32 IQK certificate into expressions.
Final-result enclosure and strict replay preserve the original expression
success/failure semantics, including cancellation and zero-denominator cases.

The exp operation's common workspace/trait now takes the maximum of the two
batch workspaces: 2,624 bytes instead of 768 bytes in the previous Float32-only
adapter. This adds **1,856 accounted payload bytes** for either exp dtype.
The large exact workspace is still initialized, so this update does not claim
to remove the fixed small-array cost. No public ABI or profile names changed.
