# NUM-15 Whole execution

The six `numeric.derivative_1d_*` and `numeric.integrate_1d_*` operations execute
as Whole Result programs. Each input is one tensor member under any key. Samples
use Float32 or Float64 `sample_shape()` [N]; step and initial use the same dtype
and shape [1]. Each output uses port key `values` and has a `photospider.tensor`
Result schema with tensor member `samples`, shape [N], input dtype, and no
facets. Whole computes and publishes the
complete output before applying the consumer projection.

`numeric_tensor_program.hpp` supplies the Whole protocol. The writer borrows its window during publication; published Result
storage remains owned after context retirement. Logical row reads support
strided and negative-stride input storage.

`derivative_1d` requires N>=2. The kernel reads the two endpoint samples for
one-sided differences and the two neighboring samples for each interior central
difference. The center sample does not enter its own interior result, though
Whole input preparation still validates all samples. `ExactCalculus` evaluates
the exact quotient for the discrete formula and converts once to the output
dtype. The endpoint, NaN, infinity, zero, and rounding rules are specified in
[NUM-15A](op_specs/NUM-15A_derivative_1d.md).

`integrate_1d` uses cumulative trapezoids plus the initial value. For N=1, the
specialized output selects only input port 2 (initial) at runtime. The operation
copies its raw bits to output[0], including signaling NaN and signed zero; it
does not start the samples or step producers. Metadata for every input remains
statically checked, including matching shape and dtype. For N>1, Whole requests
all three inputs even when the consumer requests only output[0], and validates
step as finite and nonzero. Negative finite step is valid. `ExactCalculus` keeps
the exact weighted sample prefix across output conversions and rounds the full
initial-plus-area formula once per positive output index. See
[NUM-15B](op_specs/NUM-15B_integrate_1d.md) for the complete numerical rules.

Empty demand schedules no payload work or producer execution. A nonempty
failure publishes no partial output. Work and cancellation checks apply during
input reads, exact arithmetic, stores, and publication; resource or cancellation
errors retain their runtime categories. Nonempty Whole execution requires a
complete dense output and fixed exact arithmetic state, except that N=1
integration copies initial directly without constructing `ExactCalculus` state.

## Public use and focused validation

The public C++ helpers `derivative_1d_node` and `integrate_1d_node` are declared
in `plugins/ops/include/photospider/ops/numeric/calculus.hpp`. This snippet assumes the caller
has already created the input references and workflow builder:

```cpp
auto derivative = ps::numeric::derivative_1d_node(
    1, samples, step);
auto integral = ps::numeric::integrate_1d_node(
    2, samples, step, initial);
```

For temporary public-path validation, declare source Results backed by immutable
storage, bind them to a WorkflowDocument, and execute through the public kernel API. A nonempty sparse query still computes and publishes the complete
output with full tensor coverage. Whole input preparation reads the complete
active sources. The query scopes the recorded output dependency observation and
dirty mapping; it does not trim Result coverage. An interior derivative center
may be absent from that output's stencil while an edit to it still invalidates
the requested observation. For N=1 integration,
metadata remains statically checked for all three ports, while runtime support
contains only input 2 and failed sample/step producers remain unstarted. Empty
demand starts no failure producer and publishes empty tensor coverage.

Also exercise negative strides and step direction, unaligned input,
zero-stride broadcast input, caller and worker floating-environment preservation
on real continuation polls, and schema rejection for incompatible RGBA facets on
rank-one numeric tensors. Checked continuation adapters impose actual work limits;
resource and cancellation paths are exercised with payload and scratch limits.
For an N=3 Float64 case in an isolated Root, after context and source teardown,
an escaped Result and an authorized read window share one 24-byte Payload allocation. Releasing the Result leaves the
window readable; releasing the window returns all live Root counters to zero.
Check source and output owners separately.

Use an independent exact Fraction reference for the discrete formulas. Strict
results must match the final rounded reference bits. Accelerated results use the
shared final FP32-scaled bound and need not be bit-exact. Compile the temporary
checker with `-fno-fast-math -frounding-math -ffp-contract=off`.

Create these checks for the operation change and remove their source and build
entry points after validation. Report only the profiles actually executed.
