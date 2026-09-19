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
