The [trigonometric implementation](trig-performance.md) uses certified
polynomial kernels for eligible Float32 sin/cos and pi/sinc calculations.

Fixed NUM-04/05 point invocations gather up to 64 Float32/Float64 operands and
call the binary64 SLEEF adapter once per eligible block. The original bit-level
special cases, exact power/angle landmarks, per-result enclosure and strict
refinement remain in CertifiedMath. Unsupported lanes are sanitized before
SIMD and retain their original bits for fallback.

`PointMathScratch<Arithmetic>` contains the arithmetic state, general batch
workspace and Float32 SIMD workspace together. The Plan declares its actual
size, and admission reserves the required capacity per active CPU lane before
issue. Runtime callbacks query cancellation at each batch and within scalar
refinement; they do not charge per-value work or submit numeric counters through
ABI 8. A required failure prevents successful call publication.

Each NUM-04/05 invocation establishes one nearest/gradual-underflow environment
for its selected output tile, including reduced pi/rational-pi calculations.
CRV-01/10 curves and CRV-11 uniform lowpass similarly borrow one environment
within each fixed invocation. Independent scalar calls retain a local guard.
This amortizes setup without vectorizing inverse bisection or reordering lowpass
taps. NUM-01 batches AST evaluation within its selected values tile. Exact
integer, reduction, scan and nonuniform-integration formulas remain unchanged.

Temporary public checks should compare scalar and batched bits for both dtypes,
mixed special/ordinary inputs, batch boundaries and tails, signed/unaligned/zero
strides, all four host rounding modes and preexisting flags. Exercise complete
and sparse tile selections, exact source-tile closure, scratch/output admission,
cancellation, concurrent Runs and retained output lifetime. Separate scalar
helper work-budget checks from the fixed invocation protocol. Independent public
oracles establish numerical bounds; batching correctness does not by itself
establish performance on every profile or platform.
