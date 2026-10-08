# NUM-04/05 Whole execution

## Current NUM-04 unary Result path

The current unary family registers 66 Result operations in
`plugins/ops/01-numeric/numeric_unary.cpp` and exposes node constructors through
`photospider/numeric/unary.hpp`. Each input Result contains exactly one numeric
tensor member in slot 0 and may also carry fields. Metadata specialization
requires matching dtype and complete sample shape, rank 1..8, positive extents,
and at most 2^40 samples; input schema IDs and facet identities are not fixed.
Recognized tensor facets and spatial metadata still receive full-input typed
validation.

The Whole continuation requests complete input support with Data, Validation
and Descriptor roles. It reads authorized Result windows and preserves compatible
signed or zero strides without requiring packed input collection. Every
nonempty execution validates all input samples and writes one complete packed
Result with schema `photospider.tensor`, tensor key `samples`, and empty facets;
the executor then projects requested coordinates. Ordinary unary output keeps
the input dtype. Rational pi constructors accept matching Int64 tensors and
select a Float32 or Float64 output explicitly. Empty requests pass static
validation and resource admission, then produce empty tensor coverage without
input reads or arithmetic callbacks. Whole arithmetic errors retain Run scope,
and returned Results keep immutable backing after the execution context retires.

## Current NUM-05 binary Result path

`numeric_binary.cpp` registers 27 CPU Whole Result keys through the shared
`point_math_operation` implementation. The public helpers in
`photospider/numeric/binary.hpp` build editable workflows, and
`examples/numeric_workflow/binary.cpp` binds source Results through the public
C++ API. Its `Value` objects are local typed backing used to create those
Results.

Each input Result has one tensor member in slot 0 and may also contain fields.
Input schema IDs may differ, but dtype and the complete `sample_shape()` must
match. Rank is 1..8, extents are positive, and the sample count is at most
2^40. Recognized tensor facets and spatial metadata still receive full-input
typed validation. Nonempty Whole execution requests Data, Validation and
Descriptor support for both inputs, reads authorized windows, and publishes a
complete packed Result. The output uses `photospider.tensor` with tensor key
`samples`, retains the full logical sample shape as ordinary axes, and drops
input facets and batch topology. The executor projects requested coordinates
after publication. Empty requests can take a metadata-only poll and produce
empty coverage without sample reads or arithmetic. Integer overflow anywhere
in the full operation returns OperationFailed/ArithmeticOverflow/Run and no
Atom key.

The focused strict workflow is registered as `test_numeric_binary_result`.
Strict and Apple runs have independent integer/Fraction/MPFR oracle coverage;
the installed consumer runs the same example source through
`Photospider::kernel`. These are CPU checks, with no native GPU claim.

Accelerated ln, sin, cos, tan, pow and atan2 use 64-lane gather blocks for
Float32 and Float64. Their 2,624-byte fixed scratch is included in the declared
workspace. Fixed indexing and arithmetic admission precede SIMD; the scalar
semantic layer still resolves exact landmarks, special values and uncertified
lanes. All certified point callbacks lend one nearest/gradual environment to
nested helpers. See [batch validation and timings](batch-performance.md).

The subsequent [trigonometric kernels](trig-performance.md) reuse the Float32
batch adapter. Sin/cos retain the 2,624-byte maximum workspace for the Float64
branch; sinpi/cospi/sinc/sincpi add a 768-byte fixed Float32 block. Fixed-domain
proofs replace runtime enclosures only for admitted primitive outputs.
