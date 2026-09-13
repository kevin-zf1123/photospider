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
