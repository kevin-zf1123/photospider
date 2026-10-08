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
in `include/photospider/numeric/calculus.hpp`. This snippet assumes the caller
has already created the input references and workflow builder:

```cpp
auto derivative = ps::numeric::derivative_1d_node(
    1, samples, step);
auto integral = ps::numeric::integrate_1d_node(
    2, samples, step, initial);
```

The manual Result workflow in `examples/numeric_workflow/calculus.cpp` checks
the public Whole execution path. Its source fixture retains `Value` as backing
storage and publishes declared sources as Results with referenced tensor
storage. A nonempty sparse query still computes and publishes the complete
output with full tensor coverage. Whole input preparation reads the complete
active sources. The query scopes the recorded output dependency observation and
dirty mapping; it does not trim Result coverage. An interior derivative center
may be absent from that output's stencil while an edit to it still invalidates
the requested observation. For N=1 integration,
metadata remains statically checked for all three ports, while runtime support
contains only input 2 and failed sample/step producers remain unstarted. Empty
demand starts no failure producer and publishes empty tensor coverage.

The fixture also exercises negative strides and step direction, unaligned input,
zero-stride broadcast input, caller and worker floating-environment preservation
on real continuation polls, and schema rejection for incompatible RGBA facets on
rank-one numeric tensors. Checked continuation adapters impose actual work limits;
resource and cancellation paths are exercised with payload and scratch limits.
After context and source teardown, an escaped Result and an authorized read
window share one 24-byte Payload allocation. Releasing the Result leaves the
window readable; releasing the window returns all live Root counters to zero.
The source and output owners are checked separately.

The root manual test and installed consumer build and run
`examples/numeric_workflow/calculus.cpp`. The current family integration source is
[`test_numeric_result_math_calculus.cpp`](../../../tests/integration/numeric/test_numeric_result_math_calculus.cpp).
The independent oracle retains its 1,810 exact Fraction reference
cases. Strict must match every bit. Apple is judged by the shared accelerated
FP32-scaled bound, and the corpus includes a verified one-ULP difference, so
Apple is not described as bit-exact. The CPU example and installed consumer are
compiled with `-fno-fast-math -frounding-math -ffp-contract=off`.

Run the focused root coverage and the independent oracle from the configured
build directory:

```sh
cmake --build <build-dir> --target test_numeric_result_math_calculus photospider_numeric_calculus -j8
ctest --test-dir <build-dir> \
  -R '^(test_numeric_calculus_result|test_numeric_result_math_calculus)$' --output-on-failure
<build-dir>/examples/numeric_workflow/photospider_numeric_calculus strict
python3 oracle/ops/numeric/calculus_oracle.py <build-dir>/examples/numeric_workflow/photospider_numeric_calculus strict
<build-dir>/examples/numeric_workflow/photospider_numeric_calculus apple
python3 oracle/ops/numeric/calculus_oracle.py <build-dir>/examples/numeric_workflow/photospider_numeric_calculus apple
```

For installed-package validation, install the configured build, configure the
consumer with the explicit package directory, then build and run its focused
CTest:

```sh
cmake --install <build-dir> --prefix <build-dir>/result-only-install
```

Its current family source is
[`test_numeric_result_math_calculus.cpp`](../../../tests/integration/numeric/test_numeric_result_math_calculus.cpp). The manual executable completed six check groups in
both Strict and Apple. These checks validate the named local and installed
paths; they do not establish that Apple or x86 accelerated instructions executed
on hardware.
