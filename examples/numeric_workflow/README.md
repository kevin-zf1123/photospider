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
