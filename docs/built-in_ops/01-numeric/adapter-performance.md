## Implementation

Each fixed point-math invocation gathers at most 64 operands through its
prepared source views and records each dense destination address. Float32
candidate lanes are classified into a 64-bit rejection mask. Rejected input bits
remain available for exact fallback and are replaced with safe values before
SIMD arithmetic. Each result is stored at its recorded destination; a required
failure prevents successful publication of the call. The mathematical SIMD
coefficients and FMA graphs retain their certified numerical contract.

Signed, zero and unaligned input strides use the prepared coordinate reader.
Batch arithmetic and exact fallback share the invocation's normalized floating
environment. Cancellation is queried between batches and within refinement.
ABI 8 does not expose per-value work charging or numeric counters; output and
scratch capacity are admitted from the fixed Plan before issue. Tail handling
reads and writes only the retained output tile's valid samples.

FP64 NUM-04 exp restores SLEEF 3.9.0 `xexp` in [-80,80]. Both scalar and batch
paths retain binary64 inputs/output; the candidate is enclosed by four adjacent
binary64 steps and admitted only by the existing final FP32-scaled accuracy
guard. This is the existing accelerated Float64 contract, not a new global
four-ULP64 guarantee. Float32 NUM-04 exp still uses IQK. Exact special cases and
out-of-domain/range uncertainty retain strict evaluation.

NUM-01 also restores the binary64 SLEEF exp endpoint enclosures for its RN64
AST. This does not substitute the Float32 IQK certificate into expressions.
Final-result enclosure and strict replay preserve expression arithmetic for
valid inputs and retain cancellation. Nonzero denominators and valid numerical
domains are caller premises; replay does not add runtime input validation.

The current `PointMathScratch<Arithmetic>` contains the arithmetic object,
`MathBatchWorkspace` and `FloatMathWorkspace` together, including the per-lane
destination array. Preparation declares its actual size and block admission
reserves the required scratch per active CPU lane. The exact workspace is still
initialized; batching does not remove that fixed small-array cost. See
[fixed mathematical execution](point-math-execution.md) for the public boundary.
