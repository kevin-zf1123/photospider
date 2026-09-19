# Numeric workflows

These editable workflows use the installed C++ API. Their targets are excluded
from the default build and have no CTest registration. The category's
[implementation table](../../docs/built-in_ops/01-numeric/implementation.md)
records the remaining families.

## Sequence generators

`sequences.cpp` declares dynamic scalar bindings, creates a `WorkflowDocument`,
compiles it with `Compiler`, and executes it with `ExecutionContext`. The public
`photospider/numeric/sequences.hpp` helpers `linspace_node` and `arange_node`
create nodes with explicit parameters. `SequenceInput` carries an authoring
reference and descriptor; compiler inference checks the actual graph edge.

The six default registry keys are `numeric.linspace` and `numeric.arange`, each
with one required suffix: `_strict`, `_accelerated_apple_silicon`, or
`_accelerated_x86_64`. Choose the key explicitly. An incompatible accelerated
profile returns `BackendUnavailable`. All profiles use the same bounded exact
integer formulas and direct IEEE rounding; NEON and AVX2 perform widening limb
multiplication with ordered carry propagation.

Both require static `count: Int64` in `[1,1048576]` and `dtype: String`.
Linspace accepts Float32/Float64 scalar `start,end` and floating output.
Arange accepts floating `start,step` with floating output, or two Int64 scalars
with `dtype="int64"`. The authoring default is Float64, except two Int64 arange
inputs default to Int64. Integer/floating kinds cannot be mixed implicitly.

Named outputs are `values[count]` and `axis[3]`; axis uses Float64 for floating
sequences and Int64 for integer sequences. Outputs have empty facets. Values
have exact per-index dependencies. The complete axis tuple is one observation,
including when the caller selects one component. Unselected endpoints are not
read, and count=1 ignores the second input entirely. Arithmetic failures affect
only the requested observation. Diagnostic records expose actual profile,
implementation/compiler identity, evaluated values and fallback counters.

From the repository root, using Clang:

```sh
cmake -S . -B build/numeric -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_BUILD_TYPE=RelWithDebInfo -DBUILD_TESTING=OFF
cmake --build build/numeric --target photospider_numeric_sequences photospider_numeric_facilities -j 8
build/numeric/examples/numeric_workflow/photospider_numeric_sequences strict
build/numeric/examples/numeric_workflow/photospider_numeric_facilities
python3 examples/numeric_workflow/sequence_oracle.py build/numeric/examples/numeric_workflow/photospider_numeric_sequences strict
```

Use `apple_silicon` or `x86_64` instead of `strict` to exercise the corresponding
accelerated key. Pass the same argument to the Python oracle. WSL builds use
Clang and validate correctness only.

Expected results include `linspace(0,1,5)` values `[0,.25,.5,.75,1]`, axis
`[0,1,.25]`, and Int64 `arange(3,-2,4)` values `[3,1,-1,-3]`, axis `[3,-3,-2]`.
The executable checks exact bytes, extreme cancellation, direct Float32
rounding, signed zeros, unused failing producers, independent axis overflow,
static errors, tuple certificates, cancellation, cache dependencies, bounded
resource failures, strided input and result-owner lifetime. It also checks
restoration of the caller's floating environment. `sequence_oracle.py` computes
rational formulas and IEEE rounding independently and checks 960 workflow cases.

`facilities.cpp` demonstrates host-owned numeric reports, retaining arithmetic
counts for failed atoms with joint execution enabled or disabled. Its structured
consumer reads five observations over ten polls and checks `sum=35`, ten invocations and five
reported values. It also checks compiler propagation of tuple metadata.

## Installed consumer

Build the kernel as above, then compile the same sources against its installation:

```sh
cmake --install build/numeric --prefix "$PWD/build/numeric-prefix"
cmake -S examples/numeric_workflow -B build/numeric-consumer -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_PREFIX_PATH="$PWD/build/numeric-prefix"
cmake --build build/numeric-consumer -j 8
build/numeric-consumer/photospider_numeric_sequences
build/numeric-consumer/photospider_numeric_facilities
```

Actual validation on 2026-09-14 used Apple Clang 21 locally and Clang 18.1.3 in
Ubuntu WSL on x86-64. Strict/NEON locally and strict/AVX2 in WSL passed the public
workflows and 960-case oracle. Local installed-consumer compilation and execution
also passed. These are correctness checks; no performance result is claimed.

## Expression sampling and static preparation: NUM-01

`photospider/numeric/expression.hpp` provides `sample_expression_node`. The
editable `examples()` and `bindings_errors_and_cache()` functions in
`expression.cpp` supply the complete public WorkflowDocument, dynamic bindings,
Compiler and ExecutionContext path. For example, their node construction is:

```cpp
auto node = ps::numeric::sample_expression_node(
    1, "a*x+b", ps::WorkflowInputReference{1}, ps::WorkflowInputReference{2},
    5, {{"a", ps::WorkflowInputReference{3}},
        {"b", ps::WorkflowInputReference{4}}},
    ps::ElementType::Float64, ps::CpuNumericProfile::Strict);
```

Bind start=0, end=1, a=2, b=1 as Float32/64 `[1]` values. The named outputs
are `values=[1,1.5,2,2.5,3]` and Float64 `axis=[0,1,.25]`. Reusing the plan
with a=3, b=-1 returns `[-1,-.25,.5,1.25,2]` and the same axis. Coefficient
names are sorted bytewise and must exactly match the free names in the source.
Count is required in `[1,1048576]`; the default output dtype is Float64.
The three registered keys are `numeric.sample_expression` with suffix
`_strict`, `_accelerated_apple_silicon` or `_accelerated_x86_64`.

The language supports decimal constants, `x`, `pi`, `e`, named coefficients,
parentheses, unary signs, `+ - * / ^`, and `abs sqrt exp ln sin cos tan min max`.
Source length is at most 4096 bytes, with at most 256 AST nodes and height 32.
Exponentiation is right associative and binds above unary signs. Coordinates
use exact endpoint interpolation and one binary64 rounding; each expression
primitive then rounds to binary64 in left-to-right postorder. Final conversion
rounds once to the selected output dtype. Inputs and intermediate values must
be finite; failures identify the global sample and source span. Count=1 ignores
the end payload. Axis-only requests skip coefficients and expression evaluation.

```sh
cmake --build build/numeric --target photospider_numeric_expression photospider_numeric_prepared -j 8
build/numeric/examples/numeric_workflow/photospider_numeric_expression strict
build/numeric/examples/numeric_workflow/photospider_numeric_prepared
python3 examples/numeric_workflow/expression_oracle.py \
  build/numeric/examples/numeric_workflow/photospider_numeric_expression strict
```

Use `apple` or `x86` only on that CPU target. The oracle requires MPFR 4.2+
through the library selection described under NUM-04. The example passes
explicit work budgets to `execute_fragments`; complex expressions and large
requests can exhaust a smaller budget. Ordinary accelerated transcendental
operations currently use the strict certified backend and report per-function
fallbacks. Unresolved bounded refinement returns ResourceExhausted.

The public `OperationDefinition::prepare_static` facility parses immutable
programs once per compiler node. Semantic nodes, optimized nodes and plan steps
share the sealed `PreparedOperation` owner across dynamic runs and outputs.
Direct callers can pass an explicit matching prepared handle; absent a handle,
preflight prepares once per call, including a compatible joint request. There
is no global preparation cache. Static source/program storage uses ordinary
host allocations outside runtime managed-scratch admission, with operation
size bounds and no separately enforced preparation budget. Runtime continuations
and mathematical scratch remain admitted and metered. Package 0.15 requires
C++ consumers to rebuild; C operation ABI 9 is unchanged.

`prepared.cpp` is the editable public registration/session example. It checks
preparation counts, metadata/parameter-bit identity, foreign-handle rejection,
program/definition lifetime and diagnostic merge behavior. `expression.cpp`
also checks unused failing producers, shared-scalar reads, sparse/dirty/cache
behavior, precise failure spans, strided inputs/fenv, isolated Atom failures,
multi-box cancellation and controlled metadata-exhaustion recovery. Up to 16
input ports use a two-poll regional path; larger coefficient sets use bounded
16-port staged reads and may need a larger certificate-box budget, as shown
by the 24-coefficient fixture. Only requested sample coordinates are evaluated.

For native timing, run `photospider_numeric_expression strict benchmark` or
`apple benchmark`. This takes several minutes: `exp(x)` over 1048576 points
is evaluated three times. CSV records `2*x+1` and `exp(x)` at N=256, 65536,
1048576 with Whole and three-point ROI, Float64, one worker, cache off, median
and maximum elapsed microseconds, peak controlled payload, poll/evaluation/math
counts and fallbacks. Compile/freeze precede timing; synchronous execution and
result assembly are timed. Seven independent checkpoints and diagnostics are
checked. `scalar_support` counts unique input coordinates, not actual producer
invocations. WSL supplies correctness checks only. Actual results and limits
are in [the implementation notes](../../docs/built-in_ops/01-numeric/math-implementation.md#num-01-expression-and-preparation).
These manual executables have no CTest or integration registration.

## Array construction: NUM-03

`photospider_numeric_arrays` exercises `numeric.constant` and
`numeric.broadcast` with `Compiler`, `freeze`, `execute_fragments`, ordinary
execution and a structured public consumer. The public
`photospider/numeric/arrays.hpp` helpers are `constant_node` and
`broadcast_node`; both accept an explicit shape, layout (`View` or `Dense`),
and numeric profile. Build the explicit targets and run:

```sh
cmake --build build/numeric --target photospider_numeric_arrays -j 8
build/numeric/examples/numeric_workflow/photospider_numeric_arrays _strict
cmake --build build/numeric --target photospider_numeric_mappings -j 8
build/numeric/examples/numeric_workflow/photospider_numeric_mappings
```

The executable checks a `[1048576,1048576]` constant view backed by one 8-byte
Int64 scalar, a `[3]` to `[2,3,4]` broadcast backed by three Int64 samples, a
`[274877906944,3]` broadcast view with exact sparse support and dirty
replication, dense packing, resource bounds, structured consumption and owner
lifetime. Expected output includes `stored_bytes=8`, `last=7`, and
`3 source samples, exact view/dirty/support passed`. The mappings executable
compares compact mapped certificates to explicit rows and reports
`64 Q subsets x 8 dirty subsets x 3 roles match explicit rows`.

To compose a structured consumer, request a mutable built-in registry, register
the consumer with `register_operation`, then call `freeze` before compiling:

```cpp
auto registry = ps::make_default_operation_registry(false);
registry->register_operation(structured_last()); // Defined in arrays.cpp.
registry->freeze();
auto node = ps::numeric::broadcast_node(
    2, ps::WorkflowInputReference{1}, {UINT64_C(274877906944), 3}, {1});
// structured_views() below supplies document, bindings and ExecutionContext.
```

The `arrays.cpp` `structured_views()` function is the runnable example: it
registers `manual.structured_last`, composes it after the giant broadcast, and
checks the named result is an 8-byte view with `last=7`. These executables are
manual targets only; they have no CTest or integration-test registration. WSL
runs use Clang for numerical correctness only. On 2026-09-14, local AppleClang 21
strict/Apple and Ubuntu WSL Clang 18 strict/x86 runs passed, as did the local
installed consumer. Additional checked outputs cover all UInt8 values, IEEE bit
patterns, negative unaligned permutation, typed validation, separate owners,
Empty, cancellation, StageLimit and exact cache updates. No performance claim is
inferred from these runs.

Dense array implementations use 32-byte memcpy, NEON or AVX2 blocks and exact
byte tails. Diagnostics identify `memcpy32`, `NEON-copy32` or `AVX2-copy32` plus
build and host identity. View diagnostics identify scalar copy or owner retention.
`array_owner_and_payload_cache()` checks oversized source release, changed NaN
payloads in a warm constant cache and final release of borrowed broadcast storage.

## Range operations: NUM-06

`photospider_numeric_ranges` composes public `broadcast_node` helpers with the
versioned `numeric.remap_range` and `numeric.clamp` keys through one
`WorkflowDocument`. Build and run it with Clang:

```sh
cmake --build build/numeric --target photospider_numeric_ranges -j 8
build/numeric/examples/numeric_workflow/photospider_numeric_ranges _strict
python3 examples/numeric_workflow/range_oracle.py \
  build/numeric/examples/numeric_workflow/photospider_numeric_ranges _strict
```

Use `_accelerated_apple_silicon` on Apple Silicon or `_accelerated_x86_64` on
x86-64. The composition expects
`remap=[0,127.5,255,510]` followed by
`clamp=[0,127.5,255,255]`. The workflow also checks per-atom `InvalidBounds`,
exact sparse support and dirty mapping, required reads of all five remap
operands, upstream endpoint failure, WorkLimit and mid-refinement cancellation
cleanup. `range_oracle.py` uses raw IEEE decoding and exact rational arithmetic;
local AppleClang 21 strict/Apple and Ubuntu WSL Clang 18 strict/x86 each passed
2826 cases on 2026-09-14. The installed consumer also passed. Other checks cover
typed validation, dynamic-bound cache changes, caller floating flags,
negative/zero/unaligned strides and packed global ROI origins.
These are manual targets without CTest or
integration-test registration, and no performance result is claimed.

## Interpolation: NUM-08

`photospider_numeric_interpolation` exercises the six versioned keys
`numeric.mix_*` and `numeric.smoothstep_*` through the public
`WorkflowDocument`, `Compiler`, `ExecutionContext` and explicit
`broadcast_node` path. Build and run with Clang:

```sh
cmake --build build/numeric --target photospider_numeric_interpolation -j 8
build/numeric/examples/numeric_workflow/photospider_numeric_interpolation _strict
python3 examples/numeric_workflow/interpolation_oracle.py \
  build/numeric/examples/numeric_workflow/photospider_numeric_interpolation _strict
```

Use `_accelerated_apple_silicon` on Apple Silicon or `_accelerated_x86_64` on
x86-64. The workflow expects
`smoothstep=[0,0,.15625,.5,.84375,1,1]` and
`mix=[10,10,11.5625,15,18.4375,20,20]`, and checks staged factor control,
selected branch Data and typed-validation closure. It also checks exact sparse
support, factor-cache branch replacement, invalid-factor/edge atom isolation,
layout and ROI behavior, empty demand, sNaN caller fenv preservation, WorkLimit
and cancellation cleanup. `interpolation_oracle.py` uses raw IEEE decoding,
`Fraction` and direct destination rounding.

Local strict and Apple profile runs passed 5242 oracle cases per profile and
the complete manual workflow. Ubuntu WSL Clang strict/x86 passed the same
5242 cases per profile and manual checks; the installed consumer passed locally. The smoothstep implementation uses a bounded 104-limb
(6656-bit) exact cubic workspace with scalar `u128` multiplication and
NEON/AVX2 comparison helpers. Diagnostics describe the selected profile and
implementation; no performance result is claimed. This executable is a manual
target without CTest or integration-test registration.

## Layout transforms: NUM-09

`photospider_numeric_layouts` exercises the nine `array.*` profile keys through
the public `reshape_node`, `transpose_node` and `slice_node` helpers in
`photospider/numeric/layouts.hpp`. The helpers default to
`TransformLayout::Auto`; `View` and `Dense` can be selected explicitly. Build
and run the manual target with Clang:

```sh
cmake --build build/numeric --target photospider_numeric_layouts -j 8
build/numeric/examples/numeric_workflow/photospider_numeric_layouts strict
python3 examples/numeric_workflow/layout_oracle.py \
  build/numeric/examples/numeric_workflow/photospider_numeric_layouts strict
```

The CLI profile arguments are `strict`, `apple` and `x86`; the executable
translates them to the three operation keys, so they are not operation-key
suffixes. On an unsupported host the selected accelerated profile returns
`BackendUnavailable`. Expected output includes
`reshape: [2,3]->[3,2] ... passed`,
`transpose: permutation [2,0,1], values[k,i,j]=100*i+10*j+k passed`, and
`slice: [4,2,0], exact support/dirty, full-domain validation and ignored
singleton step passed`.

The example demonstrates that direct bindings are whole dense values, while a
public transpose node can create the physically strided intermediate used to
test per-request view proof, `auto` fallback and explicit `ViewUnavailable`.
Slice uses dynamic Int64[rank] `starts` and `steps`; a singleton `counts` axis
does not read its step. `layout_oracle.py` checks integer flatten/unflatten and
raw bit preservation across reshape, transpose and slice. On 2026-09-14, local AppleClang 21 strict/Apple and Ubuntu WSL Clang 18
strict/AVX2 passed 636 oracle cases per profile and the public examples. The
installed consumer passed. Additional checks cover unaligned/negative/zero
strides, shared and independent owners, typed Validation, schema/Empty,
WorkLimit/cancellation, dense capacity admission and final publication-owner
release. A constant view composed with transpose returns a 64x64 array of 7
while retaining an 8-byte payload under a 4096-byte execution budget. The
three layout operations are `cacheable=false` because the current content cache
does not witness physical owner/stride partitions; pure and active-run sharing
remain independent. These are manual targets without CTest or
integration-test registration, and no performance result is claimed.

## Indexing and scatter: NUM-10

`photospider_numeric_indexing` exercises the eighteen `array.*` keys through
the public helpers in `photospider/numeric/indexing.hpp`: `concatenate_node`,
`gather_node`, `scatter_replace_node`, `scatter_sum_node`,
`scatter_minimum_node` and `scatter_maximum_node`. Build and run with Clang:

```sh
cmake --build build/numeric --target photospider_numeric_indexing -j 8
build/numeric/examples/numeric_workflow/photospider_numeric_indexing strict
python3 examples/numeric_workflow/index_oracle.py \
  build/numeric/examples/numeric_workflow/photospider_numeric_indexing strict
```

The executable accepts `strict`, `apple` and `x86`; these select the profile
and are not operation-key suffixes. The basic fixtures include concatenate
`[[1,2],[3,4]] + [[5],[6]] -> [[1,2,5],[3,4,6]]`, gather
`[[10,11,12],[20,21,22]]` with indices `[2,0,2]` ->
`[[12,10,12],[22,20,22]]`, scatter replace `[10,3,4]`, scatter sum
`[10,25,34]`, scatter minimum `[10,2,4]`, and scatter maximum
`[10,23,34]` for the documented duplicate-target fixtures.

The manual workflow also checks exact disjoint support, static dependency piece
translation, duplicate contributor grouping, global index validation, raw and
quiet NaN behavior, signed zeros, negative/zero/unaligned strides, changed
index cache witnesses, typed validation, diagnostics, fenv, WorkLimit, state
limits and cancellation cleanup. `index_oracle.py` independently checks
integer coordinate mapping, contributor selection and Fraction aggregate
results. On 2026-09-14, local AppleClang 21 strict/Apple and Ubuntu WSL Clang 18
strict/AVX2 passed all manual checks and 3858 oracle cases per profile. The
installed public consumer and focused compiler/dependency/fragments/resources
units passed. Diagnostics retain `evaluated=5, copied=4` after the fifth value
fails; a copy-report WorkLimit stops before the next block is copied. The shared
static-piece mapping and aggregate math received independent scoped reviews.
These targets have no CTest or integration-test registration; no performance
result is claimed.

## Reductions: NUM-11

`photospider_numeric_reductions` exercises the 21 versioned keys through the
public constructors in `photospider/numeric/reductions.hpp`: sum, minimum,
maximum, mean, count, variance and standard deviation, each with strict,
Apple and x86 profile selection. Build and run with Clang:

```sh
cmake --build build/numeric --target photospider_numeric_reductions -j 8
build/numeric/examples/numeric_workflow/photospider_numeric_reductions strict
python3 examples/numeric_workflow/reduction_oracle.py \
  build/numeric/examples/numeric_workflow/photospider_numeric_reductions strict
```

The CLI profile arguments are `strict`, `apple` and `x86`; they select the
profile and are not operation-key suffixes. The basic fixture reduces
`[[1,2,3],[4,5,6]]` over axis `1` and expects sum `[[6],[15]]`, minimum
`[[1],[4]]`, maximum `[[3],[6]]`, mean `[[2],[5]]`, count `[[3],[3]]`,
variance `2/3` and standard deviation `sqrt(2/3)` in the selected output
dtype. Axes remain as extent-one keepdims dimensions.

The implementation streams value-reading groups through at most 64-value
windows and uses fixed exact accumulator state. `evaluated_values` counts
admitted accumulator input attempts; output observation counts are reported as
`computed_elements`. `reduce_count` reads no numeric samples and can return a
single 8-byte zero-stride owner for repeated counts. `reduction_oracle.py`
checks exact Fraction moments, NaN payload conversion and midpoint-square root
rounding. Local Clang 21 strict and Apple full manual workflows passed,
including streamed 4096-element groups under a 16 KiB live-payload limit,
giant 2^40 count with an 8-byte owner and zero producer calls, atom
support/dirty/overflow isolation, typed validation, Empty, cancellation, ddof,
strided-NaN and required third-window source-failure-after-NaN checks. Strict
and Apple installed consumers passed. Ubuntu WSL Clang 18.1.3 strict/x86 full
manual workflows and the updated 4740-case oracle per profile passed. Scoped
implementation and arithmetic reviews closed all required findings. These are
manual targets without CTest or integration-test registration, and no performance
result is claimed.

## Ordering and quantile: NUM-12

`photospider_numeric_ordering` exercises the six `array.sort_*` and
`numeric.quantile_*` keys through `photospider/numeric/ordering.hpp` and the
public `WorkflowDocument` workflow. Build and run the manual target with
Clang:

```sh
cmake --build build/numeric --target photospider_numeric_ordering -j 8
build/numeric/examples/numeric_workflow/photospider_numeric_ordering strict
python3 examples/numeric_workflow/ordering_oracle.py \
  build/numeric/examples/numeric_workflow/photospider_numeric_ordering strict
```

The executable accepts `strict`, `apple` and `x86` profile arguments. The basic
fixture expects sort `[3,1,1,2]` to produce values `[1,1,2,3]` and stable indices
`[1,2,3,0]`; quantile `[0,10,20,30]` at `q=.25` produces `7.5`. Sorting uses
iterative heapsort on `(numeric key, original index)`, preserving stable ties
with an 8N-byte permutation and 8-byte zero-stride incoming state. Quantile uses exact UInt128
rank selection and 4352-bit arithmetic for one final destination rounding.
Partial output still requires the selected source line.

`sort_node` exposes `values` and Int64 `indices`; `quantile_node` keeps the
reduced axis at extent one and defaults to Float64. Both take an explicit axis
and profile. The complete dtype, NaN, dependency and error contracts are beside
the public helpers and in the category's two operator specifications.

The example enables `ExecutionContextConfig::result_cache_bytes=65536` and
sets an explicit `ExecutionOptions::maximum_dependency_cache_work` budget.
These settings permit the host to reuse the immutable permutation between
values and indices while each keeps its own current source evidence. The
`share_blocks_across_outputs` trait is an opt-in for operation authors; ordinary
workflow authors use the registered sort helper. Cache-off, exhausted proof work
and insufficient retention capacity recompute without changing output bits.
The cache saves sorting work; a hit still checks current source bytes and copies
an 8N-byte state. There is no once-per-Run or performance guarantee.
`block_contracts()` demonstrates the new contract entirely through public
registry/session services, including differing Data/Control certificates.

Local Clang 21 strict/Apple and Ubuntu WSL Clang 18.1.3 strict/AVX2 passed
2072 independent stable-order/Fraction cases per profile, including 4097-element
lines exercising the high remainder word. The public manual checks independent
and combined outputs, a 128-element line with differing output dtypes, sparse
support/dirty mapping, cache-off/proof exhaustion, q/source replacement, skipped
and required failures, typed closure, Empty, negative strides, fenv flags,
work/cancellation cleanup and failed-attempt counters. The shared-block probe
checks scope opt-in, independent Data/Control certificates and changed input
coordinates/bits. Installed 0.14 consumers passed and an old 0.13 request was
rejected. Focused compiler/dependency/resources units and scoped implementation,
arithmetic and block-identity reviews passed. This manual target is excluded
from the default build and has no CTest/integration-test registration. WSL runs
establish numerical correctness only; no performance result is claimed.

## Exact comparisons and select: NUM-07

`photospider_numeric_comparisons` exercises the public `WorkflowDocument`,
`Compiler`, `ExecutionContext` and `execute_fragments` path for the six
predicates, `is_close` and `select`. Build and run a selected profile with
Clang:

```sh
cmake --build build/numeric --target photospider_numeric_comparisons -j 8
build/numeric/examples/numeric_workflow/photospider_numeric_comparisons _strict
python3 examples/numeric_workflow/comparison_oracle.py \
  build/numeric/examples/numeric_workflow/photospider_numeric_comparisons _strict
```

Use `_accelerated_apple_silicon` on Apple Silicon or
`_accelerated_x86_64` on x86-64. An unsupported host reports
`BackendUnavailable`. The example expects
`NUM-07: six predicates; select=[10,2,30] ... passed`, followed by selected
branch support `{0,2}` and `{1}`, and confirms the `MAX/-MAX` `is_close` result
is 0 without floating overflow. It separately checks that an invalid
condition byte 2 fails only atom coordinate 1, while errors from unselected
branches are not read.

`comparison_oracle.py` decodes raw IEEE values and uses `Fraction` for the
independent relation and tolerance oracle. The 3760-case set passed on 2026-09-14 under local AppleClang 21 strict/Apple
and Ubuntu WSL Clang 18 strict/x86, and the installed public consumer passed.
The composed `less -> select` workflow returns `[1,2,2]`; changing its condition
and branch bindings updates the exact support and selected values. Other checks
cover sNaN floating-environment preservation, typed validation, negative/zero
strides, work/state limits and cancellation cleanup. Select diagnostics
describe `scalar-condition`, `bit-choice` and an ISA `scratch-store`; they do
not claim four independent samples per SIMD operation or a performance gain.
These are manual targets with no CTest or integration-test registration.

## Prefix scans and integral images: NUM-13

`photospider_numeric_scans` exercises the six `numeric.prefix_sum_*` and
`numeric.integral_image_*` keys through the public
`photospider/numeric/scans.hpp` constructors `prefix_sum_node` and
`integral_image_node`. Build and run a selected profile with Clang:

```sh
cmake --build build/numeric --target photospider_numeric_scans -j 8
build/numeric/examples/numeric_workflow/photospider_numeric_scans strict
python3 examples/numeric_workflow/scan_oracle.py \
  build/numeric/examples/numeric_workflow/photospider_numeric_scans strict
```

The executable accepts `strict`, `apple` and `x86` profile arguments. The
basic output checks the prefix fixture `[1,2,3] -> [0,1,3,6]` and the integral
fixture `[[1,2],[3,4]] -> [[0,0,0],[0,1,3],[0,4,10]]`. The public constructors
can be placed in one `WorkflowDocument` with downstream numeric consumers;
their inputs preserve the existing array dtype, shape and axis contracts.

Each exact source sum is snapshotted before final conversion. Regional prefix
work is grouped by line and increasing boundary, with at most 64 source
elements per window; integral requests charge repeated rectangle work. A regional
invocation scans a source line once, while separate observations submitted via
`execute_atoms` may recompute it. There are no persistent scan checkpoints.
The output plan and per-observation association rows are accounted. Every
Need stage enumerates `Q`; dense same-line boundaries can require quadratic
association work, although numeric source values are scanned once. Resource
and stage limits may reject large requests. These details are implementation facts and
do not add a once-per-Run or performance guarantee.

Local Clang 21 strict/Apple and Ubuntu WSL Clang 18 strict/AVX2 passed the
manual fixtures and 2,280 independent exact oracle cases per profile. The
manual target also checks sparse/L-shaped support, integer Atom isolation,
typed/Empty/zero reads, output cap, negative strides/fenv flags, and sorting
work/cancellation cleanup. A 4,096-value source is scanned once through 65
windows for four sparse results within 16 KiB payload, with exact suffix dirty
support. Installed strict/Apple consumers and the focused compiler unit passed. This target is excluded from the default build and has no
CTest or integration-test registration; the current NUM-13 specifications
remain Proposed.

## Exact affine matrix transforms: NUM-14

`photospider/numeric/matrix.hpp` provides `matrix_transform_node(id, vectors,
matrix, bias, profile)`. The three dynamic ports share Float32 or Float64 dtype:
`vectors[...,Cin]`, `matrix[Cout,Cin]`, and `bias[Cout]`, with Cin/Cout in 2..4.
Output `values[...,Cout]` has empty facets. The operation computes each complete
dot product plus bias exactly and rounds once. Singular matrices are valid;
there is no inverse, cast, implicit broadcasting or homogeneous division.

```sh
cmake --build build/numeric --target photospider_numeric_matrix -j 8
build/numeric/examples/numeric_workflow/photospider_numeric_matrix strict
python3 examples/numeric_workflow/matrix_oracle.py \
  build/numeric/examples/numeric_workflow/photospider_numeric_matrix strict
```

The editable workflow in `matrix.cpp` checks
`[[1,2],[-1,0]] * [2,3] + [4,5] -> [12,3]`. A sparse output-component request
reads each selected complete vector, the selected matrix row and its bias;
shared row/bias transport is deduplicated. Typed Validation remains separate.
The manual checks exercise result lifetime, cache on/off, exact support/dirty,
negative strides on all three ports, fenv modes/flags, invalid metadata,
cancellation and WorkLimit cleanup, and required upstream failure after NaN.
The independent Fraction/raw-bit oracle includes 2..4 rectangular transforms,
batches, product overflow/underflow cancellation, rounding midpoints, infinity,
signed zero and vector-before-matrix-before-bias NaN priority.

Use `apple` or `x86` on the corresponding named CPU profile. This manual target
is excluded from the default build and has no CTest/integration registration.

Local Clang 21 strict/Apple and Ubuntu WSL Clang 18 strict/AVX2 passed 1,110
independent oracle cases per profile and the manual matrix checks. Installed
strict/Apple consumers, the focused compiler unit and scoped reviews passed.
WSL measurements support numerical correctness only.

## Discrete derivatives and cumulative integration: NUM-15

`photospider/numeric/calculus.hpp` provides `derivative_1d_node(id, samples,
step, profile)` and `integrate_1d_node(id, samples, step, initial, profile)`.
All ports share Float32 or Float64 dtype. Samples are `[N]`, controls `[1]`, and
output `values[N]` has empty facets. Derivatives require N>=2; integration N>=1.
Both cap N at 2^40. Step must be finite/nonzero when it is needed; negative step
is valid. Use explicit graph inputs with these constructors in a WorkflowDocument.

```sh
cmake --build build/numeric --target photospider_numeric_calculus -j 8
build/numeric/examples/numeric_workflow/photospider_numeric_calculus strict
python3 examples/numeric_workflow/calculus_oracle.py \
  build/numeric/examples/numeric_workflow/photospider_numeric_calculus strict
```

The executable checks derivative `[0,1,4]`, step `1` -> `[1,2,3]`, and cumulative
integration `[0,1,2]`, step `1`, initial `0` -> `[0,0.5,2]`. Derivative endpoints
are one-sided and interiors use a two-point central stencil; the center sample
is not read for its own interior output. Integration output zero copies initial
bits including sNaN/-0 without calling step or sample producers. Positive
outputs validate step first and use an exact weighted prefix plus initial, with
one final rounding. These formulas are discrete approximations to an underlying
continuous function, not exact continuous differentiation/integration.

Editable manual checks cover exact support/dirty, invalid-step Atom isolation,
failing-producer order, all-port strides/fenv, Empty/schema, work/cancellation
and release. A 4096-value constant signal is read once through 66 windows for
four sparse integral outputs. Dense requested boundaries can incur quadratic
association work; resource/stage limits are explicit. Separate calls and
execute_atoms may repeat scans. There are no persistent checkpoints.

The manual target is excluded from the default build and has no CTest or
integration registration. Use `apple` or `x86` for the corresponding CPU profile.

Local Clang 21 strict/Apple and Ubuntu WSL Clang 18 strict/AVX2 passed 1,810
independent calculus cases per profile and the manual checks. Installed
strict/Apple consumers, focused compiler unit and scoped reviews passed.
WSL is used for numerical correctness, with no performance claim.

## Unary mathematics and exact rational pi: NUM-04

`photospider/numeric/unary.hpp` provides independently named constructors for
`abs`, `neg`, `sqrt`, `exp`, `ln`, `sin`, `cos`, `tan`, `floor`, `ceil`, `round`,
`sign`, `reciprocal`, `sinpi`, `cospi`, `tanpi`, `sinc`, and `sincpi`. Append
`_node(id, input, profile)` to these names. Four additional
`sinpi_rational_node`, `cospi_rational_node`, `tanpi_rational_node` and
`sincpi_rational_node` helpers take two same-shape Int64 input references,
followed by output dtype (default Float64) and profile.

All 66 keys have `values` output with empty facets. Generic arrays retain shape;
use explicit broadcast/cast operators for adaptation. Most basic transforms
support all four dtypes; neg excludes UInt8, while roots, reciprocals and
transcendentals require Float32/64. Integer range failures affect the requested
Atom. Rational denominators must be positive at every requested coordinate,
including zero numerators. Both rational sources remain dependencies.

```sh
cmake --build build/numeric --target photospider_numeric_unary -j 8
build/numeric/examples/numeric_workflow/photospider_numeric_unary strict
python3 examples/numeric_workflow/unary_oracle.py \
  build/numeric/examples/numeric_workflow/photospider_numeric_unary strict
```

The independent oracle requires an MPFR 4.2+ shared library matching Python's
architecture, only for this manual check. Set `PHOTOSPIDER_ORACLE_MPFR` to select
it explicitly. On Apple Silicon, `/opt/homebrew/bin/python3.11` can be used with
the native Homebrew library. The kernel itself has no external math dependency.
The oracle prints the actual version it loaded. The constant-generation check
uses only Python's standard library:

```sh
python3 examples/numeric_workflow/generate_math_constants.py --check
```

`unary.cpp` is an editable WorkflowDocument -> Compiler -> ExecutionContext
example. Its fixtures include `sin(1)` -> Float64 bits `0x3feaed548f090cee`,
`cos(1)` -> `0x3fe14a280fb5068c`, and `tan(1)` -> `0x3ff8eb245cbee3a6`.
Exact rational `p=[0,1,1]`, `q=[1,6,2]` gives sinpi `[0,0.5,1]` and tanpi
`[0,RN(sqrt(1/3)),canonical_NaN]`. Floating pi-multiple functions interpret the
exact supplied float; they never multiply it by a rounded pi first.

The example sets one CPU worker, a 1 MiB controlled-payload limit, and explicit
512 Mi work units per dependency session / 1024 Mi per Run for mathematical
refinement. These are finite example budgets, not default or universal success
guarantees. Exact elementary state is small; transcendental state includes a
fixed 12288-bit limb arena and uses directed precision from 128 through 4096
fractional bits. Unresolved rounding returns ResourceExhausted. Ordinary
accelerated transcendental values currently use a reported strict fallback;
exact special/algebraic paths remain bitwise identical without that fallback.
See [the mathematical implementation notes](../../docs/built-in_ops/01-numeric/math-implementation.md).

Manual checks cover every function's negative strides and fenv modes/flags,
precise sparse Data/dirty and typed validation, integer/denominator Atom errors,
upstream failure, fallback counters, work/cancel/capacity cleanup, lifetime,
and warm-cache changes to NaN sign/payload. Use `apple` or `x86` only on that
CPU target. No CTest or integration-test registration is added.

Native local timing is available separately:

```sh
build/numeric/examples/numeric_workflow/photospider_numeric_unary strict benchmark
```

The CSV reports all functions at N=1 and N=256, Float64, Whole demand, one worker,
cache off, three repetitions, median/max microseconds and peak controlled payload.
Compilation and freezing occur before timing; synchronous execution and result
assembly are timed, and result bits/evaluation counts are checked. Ordinary
rational timing uses p/q=1/7. Timing is not an accelerated speedup claim; WSL
runs remain correctness-only.

## Binary mathematics: NUM-05

`photospider/numeric/binary.hpp` provides `add_node`, `subtract_node`,
`multiply_node`, `divide_node`, `minimum_node`, `maximum_node`, `pow_node`,
`atan2_node` and `atan2pi_node`. Each takes `(id, first, second, profile)`;
profile defaults to Strict. Angle arguments are ordered `(y,x)`, other arguments
`(a,b)`. Both inputs must match positive rank-1..8 shape and dtype, with at most
2^40 elements. `values` preserves shape/dtype with empty facets. Divide, power
and angles require Float32/64; the other five accept UInt8/Int64/Float32/64.
Use explicit broadcast/cast operators for adaptation.

```sh
cmake --build build/numeric --target photospider_numeric_binary -j 8
build/numeric/examples/numeric_workflow/photospider_numeric_binary strict
python3 examples/numeric_workflow/binary_oracle.py \
  build/numeric/examples/numeric_workflow/photospider_numeric_binary strict
```

The independent oracle uses the same MPFR 4.2+ library selection as NUM-04.
Use `apple` or `x86` only on that CPU. The executable is a public
WorkflowDocument -> Compiler -> ExecutionContext example. Its editable
`cache_and_composition` fixture computes `(a+b)*b` for a=[1,2,3], b=[2,2,2],
with expected output `[6,8,10]`. `pow([2,-2,-2],[3,3,.5])` gives
`[8,-8,canonical_NaN]`. `atan2pi([+0,-0,1],[-0,-0,+0])` gives `[1,-1,.5]`.
The corresponding radian angle fixture compares independently rounded pi bits.

All sources are read and validated even for `NaN^0`, `1^NaN`, or NaN-selected
minimum/maximum. Integer overflow fails only its requested Atom; floating domain
errors/overflow produce the specified IEEE numeric result. Cache witnesses
retain both operands even when a changed NaN leaves the result equal to one.
Pow and angle functions use the NUM-04 bounded interval state and explicit
work budgets shown in `Fixture::run`; ordinary accelerated transcendental
results currently report a strict fallback. No universal refinement-success
or performance improvement is promised.

The manual executable checks nine public fixtures and precise sparse support,
UInt8/Int64 overflow isolation, both-port typed validation and failing producers,
independent/all-port negative strides, unaligned/zero-stride storage, caller
floating environment, fallback/work/cancellation/capacity cleanup, escaped
lifetime and cache invalidation through a suppressed NaN. It has no new CTest
or integration-test registration.

Run `photospider_numeric_binary strict benchmark` (or `apple` locally) for the
nine-function N=1/256 CSV timing workload. It uses a=2, b=.3, Float64, Whole,
one worker, cache off and three checked repetitions. Compile/freeze precede the
timed synchronous execution and result assembly. [Recorded measurements](../../docs/built-in_ops/01-numeric/math-implementation.md#num-05-validation-and-native-timing)
include the actual validation platforms and resource boundaries.

## Explicit-query curves: CRV-01

`photospider/numeric/curves.hpp` provides four independent helpers:
`interpolate_linear_node`, `interpolate_pchip_node`,
`interpolate_linear_multi_node` and `interpolate_pchip_multi_node`.
Single-function inputs are x[K], y[K], query[N]; multi-function y is [K,C]
and output is [N,C], preserving C=1. Each port independently accepts
Float32/64. K is 2..65536 and positive logical products are at most 2^40.
The sole output is named `values`, with empty facets and Float64 by default.
For example, the editable `examples()` in `curves.cpp` constructs:

```cpp
auto node = ps::numeric::interpolate_pchip_node(
    1, ps::WorkflowInputReference{1}, ps::WorkflowInputReference{2},
    ps::WorkflowInputReference{3}, ps::ElementType::Float64,
    ps::numeric::CurveDomain::Reject, ps::CpuNumericProfile::Strict);
```

Bind x=[0,1,2], y=[0,1,4], query=[.5,1.5,.5]. Expected values are
[.3125,2.1875,.3125]. `LinearExtrapolate` at query=[-1,3] returns [0,8]
using the PCHIP endpoint tangents. Linear interpolation of x=[0,1,3],
y=[0,2,4], query=[2,.5,2] returns [3,1,3]. `Clamp` selects an endpoint y.
Direct nodes provide String `dtype` and `out_of_domain` explicitly. Helpers
write the defaults `float64` and `reject`; all keys begin `curve.interpolate_`
and end with the explicit `_strict`, `_accelerated_apple_silicon` or
`_accelerated_x86_64` profile suffix.

```sh
cmake --build build/numeric --target photospider_numeric_curves -j 8
build/numeric/examples/numeric_workflow/photospider_numeric_curves strict
python3 examples/numeric_workflow/curve_oracle.py \
  build/numeric/examples/numeric_workflow/photospider_numeric_curves strict
```

Use `apple` or `x86` only on the corresponding CPU. All profiles currently use
exact rational whole-formula evaluation, including unrounded PCHIP slopes, and
one final RN-even conversion. Their results agree bitwise; NEON/AVX2 supply
integer comparison/publication helpers. Exact knot and clamp paths read one y
and preserve its signed zero. Other exact zero results are -0 only when both
selected segment endpoints are -0. Numeric input or actual output must be finite;
there is no intermediate slope overflow rejection or output clipping.

A nonempty request validates all x knots before reading selected query rows,
then requests only selected y endpoints/stencils and columns. Empty reads no
payload. The regional path uses four polls and one lookup per distinct query
row, preserving per-cell error and dependency certificates. The manual checks
sparse support/dirty/column isolation, query/topology cache replacement, all-port
negative and unaligned strides, zero strides, caller fenv, typed Mask validation,
unused/required upstream failures, work/cancel/state/stage limits and escaped
owner lifetime. `cache_composition_and_upstream()` connects a public
`constant_node` with logical shape [2,2^39] to PCHIP and reads the last output
column as 7 under a 4 MiB controlled-payload limit.

`Fixture::run` and `direct()` show explicit work budgets. Exact arithmetic can
exhaust the default direct-invocation discovery budget even for a small batch;
use ExecutionOptions or DependencyRequest limits appropriate to the workload.
The shared fixed exact workspace is about 280 KiB per admitted continuation.
The optional x index uses 8K bytes; requested output and association metadata
are also admitted. Work/capacity exhaustion fails explicitly. See
[implementation notes](../../docs/built-in_ops/01-numeric/math-implementation.md#crv-01-exact-interpolation)
for arithmetic bounds, measured resources and validation.

Run `photospider_numeric_curves strict benchmark` or `apple benchmark` locally
for the four operations at K=17, N=1/64, C=1/2, Float64, Whole, one worker,
cache off, three repetitions. Compile/freeze precede timing; execution and result
assembly are timed and all identity-curve outputs/counters are checked. The
manual target remains excluded from default builds and CTest/integration tests.
WSL Clang runs are correctness-only.


## Bezier function sampling: CRV-02

`photospider/numeric/bezier.hpp` provides `sample_bezier_function_node`.
Bind anchors:Float32/64[K,2], relative handles:Float32/64[K-1,degree-1,2],
start:Float32/64[1] and end:Float32/64[1]. Each port chooses its dtype
independently. Static degree is 2 or 3, count is 1..1048576, K is 2..65536,
output dtype defaults to Float64 and `BezierDomain` defaults to Reject.
The outputs are generic `values[count]` and one Float64 `axis[3]` tuple.

```cpp
auto node = ps::numeric::sample_bezier_function_node(
    1, ps::WorkflowInputReference{1}, ps::WorkflowInputReference{2},
    ps::WorkflowInputReference{3}, ps::WorkflowInputReference{4}, 3, 9);
```

The editable `examples()` in `bezier.cpp` binds anchors=[[0,0],[1,1]],
handles=[[[0,.25],[-1,-.25]]], start=[0], end=[1]. It executes through
WorkflowDocument, Compiler and ExecutionContext. Requesting values indices
{0,1,8} returns {0:0,1:.5,8:1}; axis is [0,1,.125]. At x=.125 the curve
parameter is t=.5. Function sampling solves Bx(t)=x before evaluating By(t).
Changing only count, bindings or requested Regions reuses the public API.

```sh
cmake --build build/numeric --target photospider_numeric_bezier -j 8
build/numeric/examples/numeric_workflow/photospider_numeric_bezier strict
python3 examples/numeric_workflow/bezier_oracle.py \
  build/numeric/examples/numeric_workflow/photospider_numeric_bezier strict
```

Use `apple` or `x86` only on that CPU. The three explicit profile keys use
exact RN64 reconstruction and correctly rounded inverse/evaluation; integer
comparison/publication helpers select scalar, NEON or AVX2. All currently
agree bitwise, with zero numerical fallbacks. Interior exact zero is +0;
nonzero underflow keeps its sign. Knot/clamp copies preserve the anchor zero.
All demanded inputs, reconstructed controls and actual outputs must be finite.

Nonempty values first read the dynamic sampling scalars, then validate every
anchor/handle x and every segment's monotonicity, then read only selected y.
C0 corners, crossed cubic handles without x folding, and y overshoot are valid.
Knot/clamp reads only one anchor y. Axis-only never reads controls. For count=1,
end is unread and axis is [start,start,0]. Empty requests read no payload.
The five manual groups check these observations, sparse dirty mapping and
cache reselection, all-port negative/unaligned/zero strides and caller fenv,
independent Atom failures, typed Mask validation, source failure ordering,
work/cancel/stage/capacity failures and owner release.

`Fixture::run` sets explicit work budgets. Each continuation owns about
508 KiB of exact scratch; topology uses 8*(K+(degree-1)*(K-1)) bytes and every
requested sample also retains explicit dependency records. Per-Need metadata
reservation is 4096+16384*M bytes for M requested values. Large dense requests
can exhaust metadata/association limits despite a small numeric output.
Request small Regions, as the public fixture does, to inspect large logical
arrays under bounded resources. Work/capacity failure never reduces precision.
See [implementation notes](../../docs/built-in_ops/01-numeric/math-implementation.md#crv-02-exact-bezier-function-sampling).

`photospider_numeric_bezier strict benchmark` (or `apple benchmark`) prints
48 CSV rows for both degrees, K=2/64/4096/65536, N=256/65536/1048576,
Whole/three-point ROI, one worker, cache off and three repetitions. Each
successful y=x output is checked; failed dense rows retain their actual status.
`benchmark_stress` checks a nearly stationary x fixture and signed subnormal
y cancellation. Compile/freeze precede timing. Root calls count actual Bx sign
attempts, while issued work also includes topology and host bookkeeping;
source_coordinates is unique dependency support, not physical read-call count.
This target is excluded from default builds, CTest and integration registration.
WSL Clang validation is correctness-only.


## Parametric Bezier evaluation: CRV-03

`evaluate_bezier_node` in `photospider/numeric/bezier.hpp` evaluates the selected
quadratic/cubic segment at an explicit parameter. Dynamic inputs in order are
anchors:Float32/64[K,D], relative handles:Float32/64[K-1,degree-1,D],
segment_indices:Int64[N], t:Float32/64[N]. Each floating port is independent.
K is 2..65536; D and N are positive, with each input/output product <=2^40.
D=1 remains an axis. Output `values[N,D]` has empty facets and per-cell Atoms.
No geometry or color meaning follows from D; loops and degenerate curves work.

```cpp
auto node = ps::numeric::evaluate_bezier_node(
    1, ps::WorkflowInputReference{1}, ps::WorkflowInputReference{2},
    ps::WorkflowInputReference{3}, ps::WorkflowInputReference{4}, 2);
```

Bind anchors=[[0,0],[2,0]], handles=[[[1,2]]], segment_indices=[0,0,0],
t=[0,.5,1]. Expected values are [[0,0],[1,1],[2,0]]. The editable `examples()`
in `parametric.cpp` runs this graph through WorkflowDocument, Compiler and
ExecutionContext. A cubic example anchors=[[0,0],[3,0]], offsets=[[[1,3],[-1,3]]]
at segment=0,t=.5 yields [1.5,2.25]. Degree is explicit; dtype defaults to
Float64 and profile to Strict. `apple` and `x86` require their corresponding CPU.

```sh
cmake --build build/numeric --target photospider_numeric_parametric -j 8
build/numeric/examples/numeric_workflow/photospider_numeric_parametric strict
python3 examples/numeric_workflow/parametric_oracle.py \
  build/numeric/examples/numeric_workflow/photospider_numeric_parametric strict
```

Handles reconstruct relative to the start anchor (quadratic/outgoing) or end
anchor (cubic incoming), rounding each sum to Float64. The whole polynomial
then rounds once directly to the output dtype. Current profiles agree bitwise.
Endpoint t=0/1 reads only the corresponding anchor and preserves zero sign;
interior exact zero is -0 only when all reconstructed controls are -0.
Nonzero underflow keeps its sign. No control-conversion overflow is inferred
from unused Float32 bounds when the actual result is finite.

A nonempty request first reads only selected query rows, then the needed local
control components. Unrequested bad segments/t/components do not fail it.
Recognized Image handles retain full-channel Validation independently of the
requested Data component. Invalid segment/t reports InvalidArgument/InvalidDomain;
nonfinite demanded controls report OperationFailed/InvalidDomain; RN64 control
or actual output overflow reports OperationFailed/ArithmeticOverflow, naming
the affected output Atom. Upstream/resource/cancellation categories survive.

`cache_composition_and_typed()` composes existing public `constant_node` views
with this operator and checks two components across a 2^39-column shape and
the last row of a 2^40-row shape. Returned owners remain readable after the
execution context is destroyed. `Fixture::run` supplies explicit work budgets.
Exact scratch and per-request row/output/certificate storage are accounted;
dense requests can exhaust metadata/association limits. Five manual groups
cover these paths plus sparse dirty support, joint Atom isolation, all-port
negative/unaligned/zero strides and fenv, cache reselection, typed Image and
failing producer order, arithmetic cancellation and second-box owner release.
The manual executable stays outside CTest/integration registration. Validation
platforms and bounds are in the [implementation notes](../../docs/built-in_ops/01-numeric/math-implementation.md#crv-03-parametric-bezier-evaluation).
