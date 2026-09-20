# Numeric workflows

These editable workflows use the installed C++ API. Their targets are excluded
from the default build and have no CTest registration. The category's
[implementation table](../../docs/built-in_ops/01-numeric/implementation.md)
records the completed families and delivery validation.

## Accelerated FP32 quality and performance

The accelerated floating contract is a final-output bound of 4 FP32 ULP.
Float64 outputs retain Float64 storage and are checked directly against the
FP32-scaled absolute bound. Strict results, special values and discrete results
remain exact; zero, FP32 subnormal range and wider references use strict.
`accuracy_oracle.py` implements this independent acceptance rule without narrowing
Float64 errors. Curve monotonicity additionally requires unique final rounding.
See [the contract](../../docs/built-in_ops/01-numeric/op_specs/NUM_accelerated_contract.md)
and [current implementation and measurements](../../docs/built-in_ops/01-numeric/op_specs/NUM_accelerated_contract.md#current-implementation).

```sh
cmake --build build/numeric --target photospider_numeric_expression photospider_numeric_unary photospider_numeric_binary photospider_numeric_category_benchmark photospider_numeric_inventory -j 6
build/numeric/examples/numeric_workflow/photospider_numeric_expression apple
python3 examples/numeric_workflow/expression_oracle.py build/numeric/examples/numeric_workflow/photospider_numeric_expression apple
build/numeric/examples/numeric_workflow/photospider_numeric_expression apple benchmark_quick
build/numeric/examples/numeric_workflow/photospider_numeric_category_benchmark apple
build/numeric/examples/numeric_workflow/photospider_numeric_category_benchmark apple extended
build/numeric/examples/numeric_workflow/photospider_numeric_category_benchmark apple legacy
build/numeric/examples/numeric_workflow/photospider_numeric_expression apple benchmark_wide
python3 examples/numeric_workflow/cost_inventory.py build/numeric/examples/numeric_workflow/photospider_numeric_inventory
```

Use `x86` on AVX2/FMA hosts and `strict` for exact references. Benchmark drivers
perform one warmup and seven measured runs, reporting median and maximum; plan
compilation and input freeze are outside timing. Cache is disabled and one host
worker is used. The expression quick benchmark retains N=65536 Whole and sparse
queries. Its public checks include nonlinear Whole/ROI/tail equality and resource
failure cleanup. The ordering workflow includes non-last-axis and disjoint-box
line reuse under a fixed work budget. `extended` covers the remaining 49 modern
basenames, and `legacy` covers 20 legacy value keys plus four individually timed
Result callbacks within the bake workflow. Source-support counts are unique
certified elements, not internal read-call counts. Callback times and whole
execution times remain separately labeled.

For the internal math layer only, build/run `photospider_numeric_math_benchmark
apple`; this in-tree developer target uses private headers. The other benchmark
and correctness workflows use the public API. `signal_benchmark apple invert`
or `signal_benchmark apple lowpass_uniform_` filters a hotspot without changing
its workload or timing protocol. Optional category argument 3 filters operation
names within `basic`/`extended` modes.

## ColorArray facilities

`photospider/data/color_array.hpp` supplies a separate `ColorArrayDescriptor`,
canonical facet/static-parameter codecs, numeric primary/white/NCL presets and
regional sample validation. It supports Float32/64 rank 2..8, channels last,
with logical element count at most 2^40. Static rational hue descriptions are
accepted by the parameter helpers and rejected as runtime Value facets.
White and primary normalization use exact integer determinant checks, including
virtual primaries and near-singular invertible matrices. ICC identity metadata
does not by itself own profile bytes. The explicit ICC resource path below
provides that owner; the color-ramp operators below consume it.

The editable `public_workflow()` in `color_array.cpp` attaches an RGB straight
description to `[2,2,4]` input and uses the existing public `abs_node`, `Compiler`
and `ExecutionContext` APIs to request one component. Its expected result is
`abs(-12)=12`, with an empty output facet set. The Data need stays component-local;
Validation, source support and dirty mapping include the same color's four
channels. An invalid alpha in that color fails; an unrelated invalid color is
not read. The example also checks typed snapshots/fragments, exact static map
proofs, cross-atom isolation and floating environment restoration.

```sh
cmake --build build/numeric --target photospider_numeric_color_array -j 8
build/numeric/examples/numeric_workflow/photospider_numeric_color_array
python3 examples/numeric_workflow/color_array_oracle.py build/numeric/examples/numeric_workflow/photospider_numeric_color_array
```

The standalone Fraction oracle checks 1,847 exact metadata decisions, including
extreme binary64 coordinates, zero normalization scales and near-singular bases.
These manual checks are excluded from CTest and integration registration.

## Immutable ICC resources

`icc.cpp` uses the installed public API to import caller-provided bytes, bind
accepted profiles by SHA-256 plus byte length, and retain the selected owner:

```cpp
auto profile = ps::IccProfile::import(bytes, root);  // bytes is ps::ByteView
if (!profile.ok()) return profile.status();
auto bindings = ps::ResourceBindings::create({profile.value()}, root);
if (!bindings.ok()) return bindings.status();
auto compiled = ps::Compiler(registry).compile(graph, {}, bindings.value());
```

CMYK `ColorArrayDescriptor::profile` names `profile.value().identity()`.
Pass the bindings as the last argument of `Value::create/from_storage` or
`MutableValue::publish`. Publication resolves the identity and retains the
accepted immutable profile independently of the importing caller. Unused
resources are pruned; a bare CMYK digest cannot create a valid Value.
`Compiler::analyze/compile` resolves all declared/inferred identities, including
Empty demand. Dependency callbacks publish with `phase.query.resources`;
ordinary callbacks receive `invocation.resources`. Structured v2 continuations
also receive and retain the accepted query resource set. Metadata and dynamic
numeric samples remain separate.

```sh
cmake --build build/numeric --target photospider_numeric_icc -j 8
build/numeric/examples/numeric_workflow/photospider_numeric_icc
python3 examples/numeric_workflow/icc_oracle.py \
  build/numeric/examples/numeric_workflow/photospider_numeric_icc
# Explicit local file loader, with a 64 MiB manual loader limit:
build/numeric/examples/numeric_workflow/photospider_numeric_icc --inspect profile.icc
```

Expected manual output ends in `PASS`; the independent Python oracle checks
75 valid/malformed ICC structures and identities using `hashlib`. `--inspect`
prints `OK <length> <sha256>` or `ERR <code> <diagnostic>`. The synthetic profile
models file structure, not a printing condition. The example checks immutable
input copying, byte-identity deduplication, cancellation/work/capacity cleanup,
canonical fragment ancestry, snapshot patching, compiler lifetime, callbacks,
partial-channel output closure in direct/ROI/Atom/DemandHandle/v2 paths, Empty results and an ICC reference limit that fails before callback entry.
Resource-bearing results currently bypass the optional sample-only memory and
disk caches. They remain reusable through owning Values, fragments and snapshots.
The importer validates ICC v2/v4 CMYK output-device structural contracts;
optional/private tag payloads are not a CMM transform certification. No color
conversion or ICC LUT evaluation occurs. Public C++ consumers must rebuild for
package 0.16.0. These targets have no integration/CTest registration.

## Color ramps

The independent XYZ, CMYK, CIELAB, OKLab, YCbCr and three hue forms of
CIELCh/OKLCh/HSL have 42 registered CPU-profile keys; RGB/RGBA adds three keys.
Their constructors are in
`photospider/numeric/color_ramps.hpp`. The required colors dtype hint selects the
default output dtype; Compiler checks the actual connected metadata. XYZ defaults
to D65 and CIELAB/CIELCh to D50. CMYK requires an explicit imported/bound profile;
YCbCr requires an explicit full NCL description. HSL defaults to sRGB/D65/sRGB
transfer coordinates. These primitives interpolate the supplied model components.

`color_ramps.cpp::examples()` demonstrates public construction and execution:

```cpp
auto node = ps::numeric::color_ramp_xyz_node(
    1, ps::WorkflowInputReference{1}, ps::WorkflowInputReference{2},
    ps::WorkflowInputReference{3}, ps::ElementType::Float64);
// input=[.5], stops=[0,1], colors=[[0,0,0],[.5,1,1.5]]
// values=[[.25,.5,.75]], with an XYZ ColorArray descriptor.
```

A component request returns the complete color. `color_ramps.cpp` also checks
Lab `[50,0,10]`, whole-row zero signs, unwrapped pi hue, achromatic hue retention
and exact Int64 rational cancellation to `-.5`. The raw-node `--probe` path
supports the independent `color_ramp_oracle.py` (exact Fraction formulas and
Machin alternating-series bounds for pi); it does not use production arithmetic.

`rgb_examples()` uses `color_ramp_rgb_node` through the same public execution
path. Its default three-channel description is sRGB primaries/D65/sRGB transfer.
With `input=[0,.5,1]`, `stops=[0,1]` and black/white colors, the middle Float64
component is `0x3fe7880b5e230e4f` (Float32 `0x3f3c405b`). Four-channel input
explicitly selects `ColorAssociation::Straight` or `Premultiplied`; output
defaults to Premultiplied and can select Straight independently. Transparent
red to opaque blue yields `[0,0,.5,.5]` at the midpoint with the default output.
The output facet records that association. `RgbRampOptions::dtype` selects the
destination type, and `ColorArrayDescriptor::transfer` selects Linear, Srgb or
Gamma with an explicit positive finite Float64 exponent.

The independent `rgb_ramp_oracle.py` uses rational integer-root comparisons for
sRGB and Decimal log/exp enclosures for general gamma. It checks whole-expression
rounding, association conversions, thresholds, HDR/negative components,
subnormals, exact integer-gamma cancellation and invalid complete colors. Its
huge-gamma fixture uses an analytical bound. Manual groups also cover sparse
selected rows, a `2^38`-position constant view, exact dirty/cache replacement,
negative/unaligned strides, descriptor mismatch, floating-environment restoration,
Empty requests, inner RGB/pi work/cancellation, diagnostics and escaped owners.

```sh
cmake --build build/numeric --target photospider_numeric_color_ramps -j 8
build/numeric/examples/numeric_workflow/photospider_numeric_color_ramps strict
python3 examples/numeric_workflow/color_ramp_oracle.py \
  build/numeric/examples/numeric_workflow/photospider_numeric_color_ramps strict
python3 examples/numeric_workflow/rgb_ramp_oracle.py \
  build/numeric/examples/numeric_workflow/photospider_numeric_color_ramps strict
```

Use `apple` or `x86` only on the matching processor. Global stops are validated
before queries; only selected complete color rows are read. Rational hue inputs
keep original Int64 numerator/positive denominator until final rounding, with no
angle wrapping. Current non-RGB implementations return strict bits; accelerated arithmetic
permits the shared final FP32-scaled bound.
RGB profiles currently use exact algebraic/direct paths and certified whole
transfer enclosures with profile-specific integer comparisons. They produce the
strict bits, within the accelerated four-ULP contract; alpha and failure
classification are exact. Gamma normalization and delayed exponent scaling keep
finite final answers possible when straight RGB or a transfer intermediate
exceeds Float64 range. No floating approximation is published before both final
enclosing endpoints round identically.

Certified unit/transfer refinement has a 4096-bit ceiling and fixed admitted
integer capacity. Other unresolved exact cancellations or rounding boundaries
can return ResourceExhausted/CapacityLimit; work and cancellation may stop earlier.
These are explicit execution limits. Native Clang 21 Strict/Apple and Ubuntu WSL
Clang 18 Strict/AVX2 passed seven manual groups and 352 RGB oracle cases per
profile; non-RGB oracle coverage is 1784 cases per profile. The installed 0.16
ColorArray/ICC/ramp consumers passed, as did 19 affected existing NUM/CRV manual
consumers under Strict. The focused compiler unit, old-minor rejection,
ClangFormat 21/cpplint and independent math, entry, ownership and cache reviews
passed. WSL results establish numerical correctness only.

## Joint three-axis color LUTs

`photospider/numeric/lut3d.hpp` exposes `apply_lut3d_trilinear_node` and
`apply_lut3d_tetrahedral_node`. They consume `input[...,3]`,
`table[N0,N1,N2,3]` and Float64 `axis[3,3]`, whose rows are `[start,end,step]`.
Input/table Float32 or Float64 types may differ; output defaults to the input
dtype hint. Each axis has 2..256 entries and independently ascends or descends
under the same rounded-grid checks as LUT1D. `Lut3dOptions` defaults to Reject;
Clamp validates original source colors before clamping coordinates.

Both color descriptions are explicit and must use the same supported
three-component model. Their primaries/transfer/white/hue fields may differ:
the table itself expresses that transformation. There is no implicit transfer,
adaptation or hue wrapping. Existing attached ColorArray descriptions must
match. Output carries the declared target description and returns complete
colors when any component is requested.

The editable `lut3d.cpp::examples()` binds all three inputs, constructs each
method through its public helper, compiles and runs it, and inspects values,
facets and exact source support. For a 2x2x2 table storing `(r*g,g*b,b*r)` at
binary vertices and input `[.75,.25,.5]`, trilinear gives
`[.1875,.125,.375]`; tetrahedral gives `[.25,.25,.5]`. Both correctly round the
whole exact formula once. Zero-weight vertices are excluded from reads and
validation; the tetrahedral diagonal midpoint therefore depends on just two
vertices. Direct/all-negative-zero mixtures preserve the specified zero signs.

```sh
cmake --build build/numeric --target photospider_numeric_lut3d -j 8
build/numeric/examples/numeric_workflow/photospider_numeric_lut3d strict
python3 examples/numeric_workflow/lut3d_oracle.py \
  build/numeric/examples/numeric_workflow/photospider_numeric_lut3d strict
```

Use `apple` or `x86` on matching processors. The five manual groups check the
cross-component fixture; exact sparse/dirty/cache behavior; maximum 256^3 table
and `2^38`-position public constant-view composition; all-port unaligned/negative
strides, typed metadata, floating environment and resource interruption; and
whole-color Atom failures with axis/query/table producer ordering. The independent
Fraction oracle checks 1062 cases per profile, including eight axis directions,
unequal extents, every cube/split boundary, all eight models, mixed dtypes,
extreme cancellation/subnormals and demanded versus zero-weight invalid colors.
The target is excluded from the default build and CTest/integration registration.

Native Clang 21 Strict/Apple and Ubuntu WSL Clang 18 Strict/AVX2 passed all five
manual groups and 1062 oracle cases per profile. Installed 0.16 consumers,
the focused compiler unit, ClangFormat 21/cpplint and independent math/entry
reviews passed. WSL measurements are used only for correctness.

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
requests can exhaust a smaller budget. Accelerated expressions propagate strict RN64 reference enclosures across
four-sample batches using admitted SLEEF binary64 kernels. Only rejected samples
replay strict evaluation and report per-function fallbacks. Unresolved bounded refinement returns ResourceExhausted.

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

Local strict and Apple profile runs passed 5244 oracle cases per profile and
the complete manual workflow. Ubuntu WSL Clang strict/x86 passed the same
5244 cases per profile and manual checks; the installed consumer passed locally. The smoothstep implementation uses a bounded 104-limb
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
Indexing diagnostics use their own family/algorithm identity plus the selected
copy path, host, floating-point build flags and complete Clang version string.
The complete longest report, including vendor version metadata and its trailing
NUL, is checked against the 256-byte field at compilation.
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
and release. A 4096-value constant signal is read once through 64 windows for
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
fractional bits. Unresolved rounding returns ResourceExhausted. Ordinary accelerated transcendental values use SLEEF binary64 kernels and
conservative final-error checks in the documented ranges. Rejected candidates
use reported strict fallback; special/algebraic paths retain their exact rules.
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
work budgets shown in `Fixture::run`; ordinary accelerated power/angle results use bounded SLEEF candidates,
with reported strict fallback only when their final enclosure is rejected. No universal refinement-success
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

Use `apple` or `x86` only on the corresponding CPU. Strict and Float64 outputs use exact rational whole-formula evaluation,
including unrounded PCHIP slopes, and one final RN-even conversion. Accelerated
Float32 candidates require uniquely rounded enclosures to preserve monotonicity;
unresolved cases use exact fallback. Collinear PCHIP stencils use the equivalent
linear formula after exact cross-product checks. NEON/AVX2 also supply integer
comparison/publication helpers. Exact knot and clamp paths read one y
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


## LUT1D baking templates: CRV-04

`photospider/numeric/lut1d.hpp` provides six authoring functions:
`bake_lut1d_expression`, `bake_lut1d_bezier`, `bake_lut1d_linear`,
`bake_lut1d_pchip`, `bake_lut1d_linear_multi`, `bake_lut1d_pchip_multi`.
They append ordinary source nodes to a caller-owned WorkflowDocument and return
`BakedLut1d{values,axis}` references. Export those references explicitly or
connect them to later nodes. No payload execution, file or frozen result is
created during construction.

For a document declaring/binding input 1=start:[0] and input 2=end:[1], the
minimal expression template is:

```cpp
auto baked = ps::numeric::bake_lut1d_expression(
    document, "x^2", ps::numeric::sequence_input(document.inputs[0]),
    ps::numeric::sequence_input(document.inputs[1]), 3);
if (!baked.ok()) throw std::runtime_error(baked.status().message);
auto exports = baked.value().outputs();
document.outputs.assign(exports.begin(), exports.end());
```

Request values and axis through Compiler/ExecutionContext to obtain
values=[0,.25,1], axis=[0,1,.5]. `Fixture::build` in `baking.cpp` contains the
complete editable path for all six sources and its hand-authored counterpart.
Use `outputs("table2","grid2")` to export another bake without workflow-name
collisions, or pass `.values`/`.axis` as WorkflowNodeOutput inputs. Node IDs
are allocated around existing and referenced producer IDs, including supplied
edges; declaration IDs use their separate namespace. Construction checks the
65536-node limit and leaves graph contents unchanged on failure. Concurrent
mutation of the same document requires caller synchronization.

All templates require count=1..1048576; dtype defaults to Float64 and profile
to Strict. `SequenceInput` carries a static Float32/64[1] hint for each dynamic
endpoint; Compiler validates the actual connections. Expression coefficients
are supplied as a named map, and canonical names are derived from the source.
Bezier additionally requires degree=2/3 and anchors/relative handles; its domain
defaults to Reject. Four interpolation templates take x/y and CurveDomain
(default Reject). They always generate Float64 linspace queries, even for a
Float32 table. Single templates return [count], multi return [count,C],
including C=1. Both export an independent Float64 axis[3].

| Template | Inputs in addition to endpoints | Endpoint/count fixture | Expected values |
| --- | --- | --- | --- |
| expression | `x^2` | 0 to 1, 3 | [0,.25,1] |
| Bezier | anchors=[[0,0],[1,1]], quadratic offsets=[[[.5,0]]] | 0 to 1, 3 | [0,.25,1] |
| linear | x=[0,1,2], y=[0,2,4] | 0 to 2, 3 | [0,2,4] |
| PCHIP | x=[0,1,2], y=[0,1,4] | 0 to 2, 5 | [0,.3125,1,2.1875,4] |
| linear multi | x=[0,1], y=[[0,10],[2,8]] | 0 to 1, 3 | [[0,10],[1,9],[2,8]] |
| PCHIP multi | x=[0,1,2], y=[[0,4],[1,3],[4,0]] | .5 to 1.5, 2 | [[.3125,3.6875],[2.1875,1.8125]] |

```sh
cmake --build build/numeric --target photospider_numeric_baking -j 8
build/numeric/examples/numeric_workflow/photospider_numeric_baking strict
```

Use `apple`/`x86` only on the corresponding CPU. Five manual groups cover
48 generated-versus-explicit graph pairs per profile, analytic values, mixed
endpoints and both output dtypes, values-only/axis-only/combined/ROI demand,
read/dirty equivalence, cache binding changes, owner lifetime, source errors,
pre-cancelled execution, work/payload limits, IDs and exports. A sparse multi
PCHIP fixture requests two values from a million-row logical table under a
1 MiB controlled-payload limit. The target remains outside CTest/integration.

Source semantics remain visible: N=1 ignores end; axis skips function controls;
expression/Bezier N>1 reject equal endpoints, while interpolation keeps
linspace's repeated coordinates. Interpolation values at index zero may ignore
end, even when the axis would fail; values requests do not force axis execution.
Dynamic bindings and ordinary cache witnesses control reevaluation. The later
consumer applies its own axis validity rules and approximation: a linear LUT
through [0,.25,1] returns .125 at x=.25, whereas continuous x^2 is .0625.
The CRV-05 example below executes all six consumer chains and checks that
discretization separately.


## LUT1D application: CRV-05

`apply_lut1d_node` and `apply_lut1d_channels_node` in
`photospider/numeric/lut1d.hpp` preserve input shape and use dynamic table/axis
inputs. Input and table independently accept Float32/64; axis is Float64[3].
The scalar table is [L]; the channels table is [L,C] for input[...,C], with
rank-1 [C] and C=1 retained. L is 1..1048576; input rank is 1..8 with positive
logical products <=2^40. Output `values` has empty facets and one scalar Atom
per input coordinate. Three explicit CPU profile keys exist for each operation.

```cpp
auto apply = ps::numeric::apply_lut1d_node(
    1, ps::WorkflowInputReference{1}, ps::WorkflowInputReference{2},
    ps::WorkflowInputReference{3}, ps::ElementType::Float64);
```

The final required argument is the input dtype hint used for default output
dtype. An explicit optional output dtype overrides it; Compiler independently
validates the actual graph edges. Domain defaults to `CurveDomain::Reject`;
Clamp and LinearExtrapolate are explicit options. Bind input=[0,.25,.5,1],
table=[0,.25,1], axis=[0,1,.5] to obtain [0,.125,.25,1]. Reversing table and
axis to [1,0,-.5] gives the same function. The channels fixture uses
input=[[0,1],[.25,.5]], table=[[0,10],[2,8]], axis=[0,1,1] and returns
[[0,8],[.5,9]], so each channel has its own query/selected pair.

The complete editable workflow is `examples()` in `lut1d.cpp`.
`baking_chains()` connects each of the six public baking templates directly:

```cpp
// baked is returned by any scalar CRV-04 template; query is a WorkflowInput.
auto consumer = ps::numeric::apply_lut1d_node(
    100, query, baked.values, baked.axis, ps::ElementType::Float64);
```

Use `apply_lut1d_channels_node` for multi-function bakes. The example records
x^2 baked at [0,.5,1] then queried at .25 as .125, while continuous x^2 is
.0625. PCHIP is also sampled before discrete linear application; the consumer
does not retain the source interpolation method.

```sh
cmake --build build/numeric --target photospider_numeric_lut1d -j 8
build/numeric/examples/numeric_workflow/photospider_numeric_lut1d strict
python3 examples/numeric_workflow/lut1d_oracle.py \
  build/numeric/examples/numeric_workflow/photospider_numeric_lut1d strict
```

Use `apple` or `x86` only on the corresponding CPU. Every nonempty request first
validates the entire endpoint-weighted RN64 grid and the exact derived step,
then requested queries, then only selected table singleton/pairs. The supplied
step is never repeatedly accumulated. L=1 requires bit-identical endpoints and
+0 step; every query remains required. Equal/collapsed grids can be produced by
an interpolation bake but are rejected by this consumer. Invalid axis/query
requests read no table values. Axis validation work remains global even for an
endpoint request. Current profiles round the complete line once and agree
bitwise, including descending pairs and finite results across huge cancellation.

Six public manual groups cover analytic/domain cases, exact read/dirty support,
invalid axes/zero signs, all-port strides and fenv, typed Image query closure,
cache/Atom/upstream ordering, axis/arithmetic cancellation and resource/owner
release, all six baking chains, a full 1048576-point grid and sparse 2^39
channels. `Fixture::run` supplies explicit work budgets. Grid storage is 8L bytes;
exact scratch and per-cell certificates are separately admitted. Large dense
requests may exhaust metadata/association budgets. Results remain readable after
the helper's context is destroyed. The executable stays outside default builds,
CTest and integration tests; WSL Clang verifies correctness only. See the
[implementation notes](../../docs/built-in_ops/01-numeric/math-implementation.md#crv-05-dynamic-axis-lut1d).

## Scalar coordinate shapers

`photospider/numeric/shapers.hpp` exposes four composable helpers. All take
Float32 or Float64 input of rank 1..8 with positive extents, at most `2^40`
values, and dynamic same-dtype `lower[1]`/`upper[1]` scalars. Output `values`
retains shape/dtype and has empty facets; a shaper does not change a color
transfer description. Bounds must be finite and strictly ordered, with
`0<lower` additionally required for the logarithmic pair.

```cpp
#include <photospider/numeric/shapers.hpp>
// graph is a WorkflowDocument; x/lower/upper are ordinary WorkflowInput edges.
auto linear = ps::numeric::linear_shaper(graph, x, lower, upper, input_descriptor);
auto inverse = ps::numeric::linear_shaper_inverse(
    graph, linear.value(), lower, upper, input_descriptor);
auto logarithmic = ps::numeric::log2_shaper_node(100, x, lower, upper);
auto log_inverse = ps::numeric::log2_shaper_inverse_node(
    101, ps::WorkflowNodeOutput{100, "values"}, lower, upper);
```

Check each `Result` before accessing its value, as the complete editable
`Fixture` and `examples()` in [shapers.cpp](shapers.cpp) do. Linear helpers
append ordinary remap, scalar constant, cast and constant-view nodes, return a
connectable output reference and preserve existing exports. They reserve IDs
against existing declarations and references. They do not register additional
primitive keys. The inverse includes the mandatory scalar bound-order guard.
Log helpers return nodes for the six `curve.log2_shaper{,_inverse}` CPU keys;
add them and desired exports to the graph normally. All helpers default to
Strict; pass `CpuNumericProfile` explicitly to select another profile.

For linear bounds `[-2,2]`, input `[-2,0,2,4]` returns `[0,.5,1,1.5]`; inverse
recovers those inputs exactly. For log bounds `[1,16]`, input `[1,2,4,16,.5,32]`
returns `[0,.25,.5,1,-.25,1.25]`, and inverse recovers those powers. General
rounded forward/inverse composition need not recover the original bits. No
implicit clipping occurs; an explicit numeric clamp can be connected separately.
Log forward rounds the complete logarithm ratio once; inverse rounds
`lower*(upper/lower)^t` once without a rounded intermediate ratio. Input NaNs
retain payload/sign and are quieted. Log forward maps either zero to `-Inf`,
negative values to canonical NaN, and `+Inf` to `+Inf`; inverse maps `-Inf` to
`+0` and `+Inf` to `+Inf`. Valid bounds are required before these numeric paths.

```sh
cmake --build build/numeric --target photospider_numeric_shapers -j 8
build/numeric/examples/numeric_workflow/photospider_numeric_shapers strict
python3 examples/numeric_workflow/shaper_oracle.py \
  build/numeric/examples/numeric_workflow/photospider_numeric_shapers strict
```

Use `apple` or `x86` on its matching architecture. Five manual groups print
`PASS`; the independent Fraction/directed MPFR oracle prints `4196` cases and
checks exact bits and monotonic groups. Manuals inspect public execution,
reverse singleton partitioning, shared-bound cache replacement, pointwise and
shared dirty support, ColorArray validation closure, signed-zero inverse guard,
unaligned/negative strides, floating environment and work/state/stage/cancellation
failure cleanup. The oracle needs MPFR 4.2+ as described in the existing math
oracle setup. It is not linked into the product.

Current log profiles agree bitwise. General certified evaluation in accelerated
profiles records a strict scalar fallback; exact/special branches do not. This
combined mapping is monotone independently of request partition. Refinement is
bounded at 4096 fraction bits and can fail `ResourceExhausted/CapacityLimit`;
host work limits and cancellation can stop it earlier. Every nonempty output
requires both shared scalars plus local input and its typed validation closure.
Empty output reads no payload. These manual targets have no integration-test or
CTest registration. WSL Clang supplies numerical correctness evidence only.

## Measured three-dimensional LUT baking

`photospider/numeric/lut3d_baking.hpp` expands a pointwise source transform into
ordinary workflow nodes. A source builder receives generated Float64 colors and
their descriptor, appends its source nodes and returns an output reference. It is
called twice during authoring: once for `[N0,N1,N2,3]` grid colors, once for
`[P,3]` validation colors. Existing shared inputs remain normal graph edges.
The helper retains no builder/capture for execution. The caller explicitly
asserts independence from position, batch shape and other sampled colors.

```cpp
#include <photospider/numeric/lut3d_baking.hpp>
#include <photospider/numeric/color_ramps.hpp>

auto color = ps::numeric::color_ramp_rgb_description();
ps::numeric::Lut3dBakeOptions options{
    {2, 2, 2}, ps::Lut3dInterpolation::Trilinear, 0, 0,
    color, color, true, {}, ps::CpuNumericProfile::Strict};
auto baked = ps::numeric::bake_lut3d(
    document, registry, axis_edge,
    [](ps::WorkflowDocument&, const ps::numeric::Lut3dSourceInput& input) {
      return ps::Result<ps::WorkflowNodeOutput>(input.colors); // identity
    }, options);
// Check baked.ok(); these references do not automatically alter graph exports.
document.outputs = {{"report", baked.value().report.source_node, "report"}};
```

The complete binding, compilation, execution and inspection code is in
[examples() and Fixture](baking3d.cpp). `source_builder()` adds editable square,
explicit Float32 cast, shared scalar gain, constant and cross-component sources
using current public helpers. `available_workflow_node_ids` finds collision-free
IDs before appending source nodes. A builder may append nodes only; an error or
exception leaves the caller's document unchanged. Pass optional `ResourceBindings`
when an existing document names ICC resources, and pass them to final compilation
as usual. Source keys/profiles are preserved; choosing the bake profile only
selects generated sampling and LUT application facilities.

Bind axis Float64 `[3,3]` to `[[0,1,1],[0,1,1],[0,1,1]]` for the identity example.
Shape extents are 2..256 independently. Each axis may ascend or descend under
CRV-07's exact reconstructed-grid rules. Optional validation points are dynamic
Float64 `[M,3]`, M=1..1048576; omit the argument for none. Centers of every grid
cell are always included, with each coordinate rounded once from its two actual
Float64 neighbors. Extras follow centers in array order and must lie in-domain;
repeated points count separately. Source and converted colors must remain finite
and legal in the explicitly supplied same-model descriptions. Table dtype defaults
to the inferred source dtype; a source Float32 cast is part of the reference.

`read_lut3d_bake_report(result.results.at("report"))` reads a sealed
`curve.bake_lut3d.report` v1 Result. The fixed 289-byte payload contains pass/counts,
validated axis, exact-error maxima rounded upward with their points/earliest
indices, and the first failure's input/reference/LUT values. Schema metadata says
Measured and records shape, method, tolerances, dtypes, color descriptions and
source recipe identity. It is not a bound over the continuous domain.

Identity passes zero tolerance. `facilities()` also runs the specified `(r*r,g,b)` source: the center reference
is `[.25,.5,.5]`, applied LUT is `[.5,.5,.5]` and maximum errors are `[.25,0,0]`.
The introductory example squares each component on a 2×2×2
grid: its center reference is `[.25,.25,.25]`, while either applied LUT gives
`[.5,.5,.5]`. With atol=.1/rtol=0, the report has passed=false, failed_count=1,
max_abs_error=`[.25,.25,.25]`, first_failure_index=0. Table requests fail with
`LutApproximationToleranceExceeded`, including a request for one exact grid
vertex; axis-only remains independent. The relative test uses
`abs(lut-reference)<=atol+rtol*abs(reference)` with exact arithmetic.

For separate report-only inspection, export report only. Ordinary multi-output
`execute` remains fail-fast. To retain a completed failed report from a table
execution, use `ExecutionOptions::result_publication`, as `facilities()` does;
the callback receives an owning ResultRef. No incomplete/erroring measurement is
published as a successful failed report. The owned sampled-table Result is an
intermediate with no quality guarantee. Gate checks its object association with
the passed report before reading requested table fragments. Different shared
parameter snapshots cause a new measurement; matching shapes alone are insufficient.

```sh
cmake --build build/numeric --target photospider_numeric_baking3d -j 8
build/numeric/examples/numeric_workflow/photospider_numeric_baking3d strict
python3 examples/numeric_workflow/baking3d_oracle.py \
  build/numeric/examples/numeric_workflow/photospider_numeric_baking3d strict
```

Use `apple`/`x86` on the corresponding CPU. Seven manual groups check independent
outputs, duplicate extras, Float32 scope, all-grid validation even for a constant
source, ICC authoring, malformed registered data, object association, cancellation,
resource limits, 512-color multiwindow baking, partial output origins, negative
unaligned source strides, caller floating environment and escaped metadata owners.
The independent Fraction oracle checks all report fields for 480 cases, including
eight models, both methods, both dtypes, all axis directions and cross-component
sources. No integration-test or CTest registration is added.

All work uses the final graph's single immutable binding snapshot. Geometry,
source requests, table backing, conversion and report validation are admitted
against host limits. Table backing is stored through bounded Result I/O windows;
measurement keeps a fixed-size summary and requests at most 64 validation colors
per batch. Source operators still determine their own physical work and storage,
and full generated grid validation currently requests the complete grid. A small
output ROI does not reduce this global work. Examples provide explicit work
budgets; large bakes may require larger work/stage/capacity limits or fail cleanly.
The current report reader needs a window of at least 72 bytes. WSL Clang is used
for correctness, with no performance inference.

## Inverse curves

`photospider/numeric/inverse_curves.hpp` provides `invert_linear_node` and
`invert_pchip_node`. Each takes dynamic `x[K]`, `y[K]`, `query[N]`, independently
Float32/64, and returns generic `values[N]`. `x` is finite and strictly increasing;
`y` is finite and strictly increasing or decreasing. The static output dtype
is Float64 by default; domain policy is Reject by default or explicit Clamp.
K is 2..65536 and N is 1..2^40. There is no inverse extrapolation mode.

This ordinary node composes with the forward helpers, NUM operations and exports:

```cpp
#include <photospider/numeric/inverse_curves.hpp>

// document.inputs binds ids 1, 2, 3 to x, y, query respectively.
auto inverse = ps::numeric::invert_pchip_node(
    10, ps::WorkflowInputReference{1}, ps::WorkflowInputReference{2},
    ps::WorkflowInputReference{3});
if (!inverse.ok()) return inverse.status();
document.nodes.push_back(inverse.take_value());
document.outputs.push_back({"x_values", 10, "values"});
// Compile with Compiler(registry), freeze the caller's bindings, and use
// ExecutionContext::execute_fragments for all or selected x_values indices.
```

The complete editable `inverse.cpp` supplies the bindings and execution. Its
linear fixture `x=[0,1,3], y=[0,2,4], query=[3,1,3]` returns `[2,0.5,2]`.
PCHIP `x=[0,1,2], y=[0,1,4], query=[0.3125,2.1875,0.3125]` returns
`[0.5,1.5,0.5]`. Negating y and query preserves these results. The composition
example connects a forward interpolator's `values` directly to inverse `query`
and recovers `[0.5,1,1.5]` for its exact dyadic samples. In general two rounded
forward/inverse outputs need not round-trip arbitrary query bits.

```sh
cmake --build build/clang21-numeric --target photospider_numeric_inverse -j 6
build/clang21-numeric/examples/numeric_workflow/photospider_numeric_inverse strict
build/clang21-numeric/examples/numeric_workflow/photospider_numeric_inverse apple
python3 examples/numeric_workflow/inverse_oracle.py \
  build/clang21-numeric/examples/numeric_workflow/photospider_numeric_inverse strict
python3 examples/numeric_workflow/inverse_oracle.py \
  build/clang21-numeric/examples/numeric_workflow/photospider_numeric_inverse apple
# A supported Clang x86-64/AVX2 build uses x86 instead of apple.
```

Linear evaluates the exact rational inverse with one final conversion. PCHIP
inverts the original exact forward Hermite polynomial and derivatives. It compares
that polynomial at destination IEEE lattice points and their exact midpoint,
including subnormals and overflow boundaries. All current profiles correctly
round the same result. Accelerated PCHIP reports a strict scalar fallback for
non-knot queries when K>2; knot/clamp and K=2 paths need no fallback. Exact
knot/clamp conversion preserves x's signed zero, other exact zeros are +0, and
nonzero underflow keeps its sign. Only the returned root can fail narrowing
overflow; an unreturned endpoint cannot reject a finite root.

Each nonempty demand validates all x/y, then the requested query positions.
Both global arrays invalidate every dependent output; query support stays local.
Typed/upstream validation and failures remain observable, including on exact
knot/clamp paths. Outputs own packed fragments at their requested global origins.
The fixed integer arena and promoted 16K-byte x/y storage are host-accounted;
root comparisons and limb operations consume work and poll cancellation. Large
requests or extreme scales can require explicitly larger host budgets, and an
exhausted solver returns ResourceExhausted without an approximate substitute.

Four manual groups cover fixtures, global/local failures and dirty support,
strides/floating environment, schema/Empty, cancellation/resource release,
cache replacement, public composition, partition equivalence, typed/upstream
failures and fallback diagnostics. `inverse_oracle.py` checks 407 independent
Fraction cases using normalized Hermite formulas and rational root bisection,
including both directions/dtypes, mixed input precision, zero endpoint slopes,
normal/subnormal ties, narrow intervals, large scales and output overflow.
These executables have no CTest or integration registration.

Validated with native Clang21 Strict/Apple and Ubuntu WSL Clang18 Strict/AVX2:
all four groups and all 407 oracle cases passed per profile. Installed package
0.16 consumers passed both native profiles. WSL results establish numerical
correctness, with no performance claim. The shared forward and LUT1D arithmetic
regressions passed 2487 and 1416 cases per native profile.

## Signal resampling

`photospider/numeric/resampling.hpp` exposes four ordinary workflow templates:
`resample_linear`, `resample_pchip`, `resample_linear_multi`, and
`resample_pchip_multi`. They append the corresponding existing CRV-01 node and
an independent `core.identity` forwarding `new_positions`. Inputs are
`positions[K]`, `values[K]` or `[K,C]`, and `new_positions[N]`, independently
Float32/64. Returned `ResampledSignal` contains connectable `samples` and
`positions` references, plus `outputs()` for explicit caller-named exports.
Sample dtype defaults to Float64 and domain policy to Reject, with the forward
interpolator's Clamp and LinearExtrapolate policies also available.

Only sample demand validates/interpolates the old signal. Position-only demand
preserves new-position dtype, descriptor, facets, special-value bits and owners,
including generic sNaN/Inf/-0, without touching the old positions or values.
Normal typed/upstream source validation still applies. IDs avoid declarations,
existing exports and forward references; failure leaves the document unchanged.

The editable `resampling.cpp::filtered_workflow` connects an explicit Hann
low-pass to linear resampling through the installed API:

```cpp
#include <photospider/numeric/lowpass.hpp>
#include <photospider/numeric/resampling.hpp>

// Inputs 1/2/3 bind old positions, signal values and requested new positions.
auto filter = ps::numeric::lowpass_uniform_hann_sinc_node(
    10, ps::WorkflowInputReference{2}, 0, 2, .25,
    ps::numeric::LowpassBoundary::Wrap);
if (!filter.ok()) return filter.status();
document.nodes.push_back(filter.take_value());
auto sampled = ps::numeric::resample_linear(
    document, ps::WorkflowInputReference{1},
    ps::WorkflowNodeOutput{10, "values"}, ps::WorkflowInputReference{3});
if (!sampled.ok()) return sampled.status();
auto exports = sampled.value().outputs();
document.outputs.insert(document.outputs.end(), exports.begin(), exports.end());
// Compile, freeze the bindings, then request samples and/or positions.
```

With old positions `[0,1,2,3,4,5,6,7]`, input `[1,-1,1,-1,1,-1,1,-1]`
and new positions `[0,2,4,6]`, the four output samples all have Float64 bits
`0x3fcc6b828682ab42`: `(pi-2)/(pi+2)` rounded once. This is the measured residual
of this finite kernel at the original Nyquist frequency. It is positive and
nonzero; this particular short filter does not remove all aliasing. The exported
positions are exactly `[0,2,4,6]`. Choose a larger radius or different parameters
and rerun the independent response checks for a different quality requirement.

```sh
cmake --build build/clang21-numeric --target photospider_numeric_resampling -j 6
build/clang21-numeric/examples/numeric_workflow/photospider_numeric_resampling strict
```

The four groups also check single/multi fixtures, independent special positions,
transactional IDs and typed SampledSignal position metadata. Use `apple` on an
Apple Silicon build or `x86` on a supported Clang AVX2 build.

## Uniform lowpass

`photospider/numeric/lowpass.hpp` provides five independent
`lowpass_uniform_{hann_sinc,hamming_sinc,blackman_sinc,kaiser_sinc,gaussian}_node`
helpers. Input Float32/64 arrays have rank 1..8 and at most 2^40 elements;
static `axis` selects independent signals, output `values` keeps shape/dtype and
has generic facets. Radius 1..4096 is required. Sinc cutoff is in `(0,.5)`
cycles/sample; Kaiser additionally takes finite `beta>=0`. Gaussian takes
finite `sigma>0` in samples instead of cutoff. No unused parameter is accepted.
Boundary defaults to Reflect without endpoint repetition; Replicate, Zero and
Wrap are explicit alternatives. Output positions remain aligned to the input.

Every logical nonzero coefficient is included, even when its numerical enclosure
is too small to affect a finite result. Exact sinc integer zeros and Hann/Blackman
endpoints are omitted from Data support. Logical order `-R..R` controls first-NaN
payload/sign and infinite contribution aggregation, including repeated reflected
indices. These are successful IEEE results. Finite constant extended samples
preserve their identical bits; other exact zeros are +0. Caller floating state
is preserved. Inputs with attached typed semantics retain their additional
validation requirements.

The whole mathematical sum and full normalizer are enclosed before one final
rounding. Current accelerated keys report strict scalar fallback. This is not
an implementation with pre-rounded Float64 weights. Work/precision/capacity
exhaustion returns an explicit failure; source support is still determined by
mathematical nonzero taps. A partial output allocates only its requested payload.

For `[0,0,1,0,0]`, radius 2 and center index 2, the exact Float64 fixtures are:

| Kernel | Parameters | Center bits |
| --- | --- | --- |
| Hann sinc | cutoff=.25 | `3fe38d7050d05568` |
| Hamming sinc | cutoff=.25 | `3fe2f660651f7f7c` |
| Blackman sinc | cutoff=.25 | `3fe655124d269c1c` |
| Kaiser sinc | cutoff=.25, beta=0 | `3fdc2755e149a310` |
| Gaussian | sigma=1 | `3fd9c486742831f7` |

```sh
cmake --build build/clang21-numeric --target \
  photospider_numeric_lowpass photospider_numeric_lowpass_execution -j 6
build/clang21-numeric/examples/numeric_workflow/photospider_numeric_lowpass strict
build/clang21-numeric/examples/numeric_workflow/photospider_numeric_lowpass_execution strict
python3 examples/numeric_workflow/lowpass_oracle.py \
  build/clang21-numeric/examples/numeric_workflow/photospider_numeric_lowpass strict
```

The 474-case independent directed MPFR oracle checks complete sums for all five
kernels/dtypes/boundaries, impulses, exact quarter-wave and Nyquist periodic
sinusoids, repeated logical taps, source specials and extreme Gaussian scales.
This verifies the defined discrete response, not a universal attenuation target.
`lowpass_execution.cpp` exercises every family under all-port negative/unaligned
layouts and four floating modes, plus sparse 2^40-element composition, cache
replacement, actual typed payload/upstream errors, inner cancellation, resource
limits and data/metadata ownership after context destruction.

## Nonuniform lowpass

Five `lowpass_nonuniform_{hann_sinc,hamming_sinc,blackman_sinc,kaiser_sinc,gaussian}_node`
helpers take `positions[K]` and `values`, independently Float32/64. Positions
must be finite and strictly increasing, K=2..1048576; `axis` selects the values
dimension of length K. Other dimensions are independent. Output `samples` has
the original values shape/dtype and generic facets. `support_radius>0` and
Gaussian `sigma>0` use coordinate units; sinc `cutoff>0` uses cycles per coordinate
unit, with no .5 limit. Kaiser additionally takes `beta>=0`.

The operation convolves the exact piecewise-linear reconstruction against the
continuous kernel using coordinate-length measure. Reflect folds at the domain
endpoints, Replicate extends endpoint values, Zero extends +0, and Wrap repeats
the domain with a possible seam jump and no extra connecting segment. Full-kernel
normalization remains in force at boundaries. Global positions are validated for
each nonempty request. Values are read only at endpoints of reconstructed
segments with positive integration length, for requested other-axis coordinates.
An isolated contact does not add a read; algebraic cancellation cannot remove a
required endpoint's validation. All demanded values and final results must be
finite; source nonfinite data and final overflow fail the dependent sample.

`lowpass_nonuniform.cpp` contains the full public binding/execution example. For
`positions=[0,.75,2]`, `values=[1,2.5,5]`, `support_radius=.5`, every kernel
returns exactly `2.5` at the middle position; inserting collinear knots preserves
it. For `positions=[0,1]`, `values=[2,2]`, radius `.5`, Zero returns `[1,1]`
and the other boundaries return `[2,2]`. With minimum subnormal values under Zero,
the exact half-minimum result rounds to +0; with three minima it rounds to two
minima, in both Float32 and Float64.

```sh
cmake --build build/clang21-numeric --target photospider_numeric_lowpass_nonuniform -j 6
build/clang21-numeric/examples/numeric_workflow/photospider_numeric_lowpass_nonuniform strict
python3 examples/numeric_workflow/nonuniform_lowpass_oracle.py \
  build/clang21-numeric/examples/numeric_workflow/photospider_numeric_lowpass_nonuniform strict
```

The 245-case independent Fraction/directed MPFR oracle enumerates exact boundary
copies, uses product-to-sum identities for cosine windows and exact rational
Gaussian/Kaiser polynomial coefficients, and integrates the reconstructed affine
pieces with explicit remainder bounds. It covers irregular gaps and narrow hats,
collinear insertion, periodic reconstructed sinusoids, seam jumps and repeated
periods, mixed dtypes and subnormal ties. Uniform and nonuniform outputs need not
agree on an equally spaced input: their mathematical reconstruction/measure differ.

The implementation uses exact coordinate partitions, exact paired-affine identities
for constant/half/zero landmarks, and certified global Taylor moments for general
signals. Precision refines from 128 to 4096 bits and Taylor order up to 512.
Large support-to-sigma ratios, high cutoff-radius products, large beta, near
midpoint cancellation or many repeated periods can exhaust those limits or host
work/capacity. No unconverged approximation is published. Each stored piece owns
four 12288-bit coordinate records plus indices; exact source copies, polynomial
vectors and growth overlap are admitted explicitly. This first implementation
prioritizes certified results and composability; it makes no throughput claim.
All lowpass/resampling executables are manual and excluded from CTest/integration.

All twelve CRV-11 manual groups, 474 uniform cases and 245 continuous cases
passed on native Clang21 Strict/Apple and Ubuntu WSL Clang18 Strict/AVX2.
Installed package0.16 consumers passed both native profiles. The focused compiler
unit, ClangFormat21/cpplint and independent math/runtime reviews passed; required
review fixes cover bounded scale-scan cancellation and pre-allocation output
metadata admission. WSL results are numerical correctness evidence only.

## Native signal timing and accounting

`signal_benchmark.cpp` runs inverse-linear/PCHIP and all ten lowpass kernels
through public Compiler/ExecutionContext. It verifies analytic raw bits before
accepting every timed result and prints source-support counts, fallback counts,
output bytes, managed payload/metadata peaks and owners retained after context
destruction. Bindings, compilation and freeze precede each timed execution.

```sh
cmake --build build/clang21-numeric --target photospider_numeric_signal_benchmark -j 6
build/clang21-numeric/examples/numeric_workflow/photospider_numeric_signal_benchmark strict
build/clang21-numeric/examples/numeric_workflow/photospider_numeric_signal_benchmark apple
```

Measured on Apple M5, native Clang21 RelWithDebInfo, one CPU worker, cache off,
three repetitions per case, Float64. The small fixture requests one output; the
representative larger fixture requests 256. Inverse uses 33 identity knots and
Whole query demand. Lowpass uses 1025 regularly spaced values containing isolated
unit impulses, requesting 256 disjoint impulse centers. Uniform radius is 2,
continuous radius is .5, sinc cutoff is .25, Kaiser beta=0, Gaussian sigma=1.
The independent continuous impulse bits come from the Fraction/MPFR oracle.
This declares a bounded sparse workload, not a full-limit throughput benchmark.

| Operation, 256 requested | Strict median / max (ms) | Apple median / max (ms) |
| --- | --- | --- |
| `invert_linear` | 11.019 / 11.697 | 10.179 / 10.223 |
| `invert_pchip` | 669.863 / 674.808 | 672.903 / 673.073 |
| `lowpass_uniform_hann_sinc` | 444.593 / 451.396 | 446.980 / 449.168 |
| `lowpass_uniform_hamming_sinc` | 452.191 / 452.363 | 447.984 / 448.620 |
| `lowpass_uniform_blackman_sinc` | 454.561 / 455.322 | 449.111 / 452.224 |
| `lowpass_uniform_kaiser_sinc` | 441.216 / 446.360 | 437.927 / 439.528 |
| `lowpass_uniform_gaussian` | 445.108 / 448.195 | 437.444 / 439.143 |
| `lowpass_nonuniform_hann_sinc` | 1681.032 / 1721.745 | 1664.975 / 1677.099 |
| `lowpass_nonuniform_hamming_sinc` | 1668.530 / 1672.319 | 1683.545 / 1693.331 |
| `lowpass_nonuniform_blackman_sinc` | 4892.051 / 4941.196 | 4850.130 / 4866.580 |
| `lowpass_nonuniform_kaiser_sinc` | 1836.944 / 1837.451 | 1816.246 / 1819.265 |
| `lowpass_nonuniform_gaussian` | 889.141 / 897.047 | 902.272 / 909.420 |

All 48 cases passed all three repetitions, exact output checks and final-owner
release. Every 256-output case retains 2048 payload bytes after context teardown.
Inverse peaks were 289928 payload / 11481032 metadata bytes; its retained metadata
was 5792 bytes. Uniform peaks were 207616 / 3878176 bytes; continuous peaks were
207720 / 8706912 bytes. Their 256 separate fragments retained 336272 metadata
bytes. The peak payload column combines output and continuation scratch; the
current public ledger does not separately attribute scratch. Metadata includes
limbs in ResourceVectors, interval maps, coefficients and dependency structures.
Caller-preallocated source backing and legacy STL/shared-owner bookkeeping
exclusions are not standalone scratch measurements. The executable prints the
small-fixture rows and exact unique source support counts as well.

All accelerated lowpass and non-knot PCHIP inverse evaluations reported strict
fallback; inverse-linear did not. These timings include dependency planning,
validation and publication, and establish no speedup. The present continuous
Blackman global polynomial path and disjoint certificate construction are costly;
callers should use explicit budgets and small requests while composing workflows.
WSL timing is intentionally not used as a performance reference.

## Native category timing and accounting

`category_benchmark.cpp` supplies a public-API, manual measurement for the 18
remaining NUM/CRV functional clusters. It checks every returned element against
an independent analytic bit pattern on every repetition, reads the first value
again after the context has been destroyed, and requires all managed payload and
metadata charges to reach zero after the retained output is released.

```sh
cmake --build build/numeric --target photospider_numeric_category_benchmark -j 4
build/numeric/examples/numeric_workflow/photospider_numeric_category_benchmark strict > build/category-strict.csv
build/numeric/examples/numeric_workflow/photospider_numeric_category_benchmark apple > build/category-apple.csv
```

Expected: 36 CSV rows per profile, exit 0, with the fixture checks below passing.
The actual run on 2026-09-20 used Apple M5, Clang 21, RelWithDebInfo, one CPU
worker, CPU-only execution, and both result/dependency caches disabled. There
are three repetitions per row. These are workflow execution times, including
planning of runtime dependencies, validation and output publication. Graph
construction, compilation, freeze, result checking and result destruction are
outside the timer. The context is reused for three executions; there is no
separate discarded warm-up. Median and maximum are reported, with no statistical
percentile or throughput claim from three samples. No build or other acceptance
process ran concurrently with these measurements. WSL is used only for numerical
correctness and supplies no timing reference.

Each cluster has a small analytic fixture and a declared larger exploratory
shape: N=1 and N=256, except integration uses N+1 samples and baking uses cube
side 2 and 5. These modest shapes exercise the current dependency machinery;
they do not establish maximum-shape throughput or cover every primitive,
parameter, model and dtype in a cluster. Values use Float64, with Int64 gather
and Bézier indices and a UInt8 comparison output. All requests are Whole.
CRV-04 has no separate arithmetic primitive; its public baking composition is
covered by the LUT and expression workflows above.

| Cluster / measured operation | Inputs and exact expected result |
| --- | --- |
| NUM-02 linspace | Scalars 0 and N-1; N outputs equal 0..N-1 |
| NUM-03 constant (dense) | Scalar 1.5; N copies of the same Float64 bits |
| NUM-06 remap_range | N copies of x=.25, input bounds [0,1], output bounds [-2,2]; every result -1 |
| NUM-07 is_close | N copies of 1 and 1.125, atol=.125, rtol=0; every UInt8 result 1 |
| NUM-08 smoothstep | N copies of x=.5 with bounds [0,1]; every result .5 |
| NUM-09 transpose (dense) | [N,2] consecutive integers; output [2,N] at [j,i] equals 2*i+j |
| NUM-10 gather | 0..N-1 with reversed Int64 indices; output N-1..0 |
| NUM-11 reduce_sum | N unit samples; one output N, exactly N accumulator input attempts |
| NUM-12 sort (values export) | Reversed 0..N-1; sorted values 0..N-1 |
| NUM-13 prefix_sum | N unit samples; N+1 outputs 0..N, exactly N accumulator input attempts |
| NUM-14 matrix_transform | N vectors [2,2], identity 2x2 matrix, bias [1,1]; every vector [3,3] |
| NUM-15 integrate_1d | N+1 unit samples, step 1, initial 0; outputs 0..N |
| CRV-03 evaluate_bezier | Quadratic anchors [[0,0],[2,0]], relative handle [1,2], N queries t=.5 on segment 0; [1,1] each |
| CRV-05 apply_lut1d | Table [0,2], axis [0,1,1], N queries .25; .5 each |
| CRV-06 color_ramp_rgb | Linear-transfer RGB black/white stops 0/1; N queries .5; [.5,.5,.5] each |
| CRV-07 apply_lut3d_trilinear | Identity 2x2x2 XYZ table; N colors [.25,.5,.75] returned unchanged |
| CRV-08 log2_shaper | N copies of 4, scalar bounds [1,16]; .5 each |
| CRV-09 bake_lut3d template | Identity XYZ source, side 2/5, axis 0..1, trilinear, atol=rtol=0; globally gated table equals exact grid coordinates |

Times below are **median / maximum in milliseconds**. Larger shape refers to
the declared N=256 or side-5 fixture, not an implementation limit.

| Cluster | Small Strict | Small Apple | Larger Strict | Larger Apple |
| --- | ---: | ---: | ---: | ---: |
| NUM-02 | 0.112 / 0.536 | 0.101 / 0.176 | 62.565 / 63.224 | 67.225 / 67.619 |
| NUM-03 | 0.078 / 0.116 | 0.081 / 0.099 | 0.080 / 0.094 | 0.082 / 0.084 |
| NUM-06 | 0.122 / 0.137 | 0.119 / 0.122 | 109.375 / 110.556 | 114.585 / 114.942 |
| NUM-07 | 0.099 / 0.130 | 0.093 / 0.099 | 0.184 / 0.184 | 0.167 / 0.169 |
| NUM-08 | 0.106 / 0.116 | 0.100 / 0.105 | 82.447 / 85.433 | 82.208 / 82.611 |
| NUM-09 | 0.101 / 0.146 | 0.098 / 0.112 | 0.137 / 0.139 | 0.135 / 0.136 |
| NUM-10 | 0.100 / 0.112 | 0.100 / 0.109 | 2.182 / 2.263 | 2.229 / 2.350 |
| NUM-11 | 0.103 / 0.121 | 0.081 / 0.088 | 0.210 / 0.219 | 0.198 / 0.213 |
| NUM-12 | 0.084 / 0.108 | 0.074 / 0.087 | 163.261 / 185.410 | 157.608 / 157.656 |
| NUM-13 | 0.093 / 0.137 | 0.067 / 0.100 | 126.692 / 127.284 | 125.140 / 127.949 |
| NUM-14 | 0.146 / 0.165 | 0.142 / 0.160 | 8.836 / 9.116 | 8.484 / 8.819 |
| NUM-15 | 0.126 / 0.160 | 0.128 / 0.137 | 194.473 / 195.772 | 195.033 / 202.138 |
| CRV-03 | 0.270 / 0.301 | 0.253 / 0.287 | 30.289 / 30.487 | 26.653 / 26.772 |
| CRV-05 | 0.143 / 0.177 | 0.156 / 0.168 | 9.060 / 9.152 | 8.292 / 8.358 |
| CRV-06 | 2.085 / 2.119 | 2.076 / 2.116 | 43.732 / 43.760 | 36.195 / 36.374 |
| CRV-07 | 0.391 / 0.408 | 0.367 / 0.401 | 64.018 / 65.629 | 59.307 / 59.877 |
| CRV-08 | 0.135 / 0.165 | 0.119 / 0.156 | 4.111 / 4.250 | 4.100 / 4.120 |
| CRV-09 | 19.823 / 19.893 | 19.309 / 19.476 | 70.785 / 71.746 | 68.568 / 69.089 |

Managed measurements below are bytes, written **small → larger**. Payload peak
includes admitted arithmetic/continuation scratch and published storage; the
current public counter does not separate those two contributions. Metadata peak
covers the context's freeze and all three executions. Retained payload/metadata
are read after context/frozen-plan destruction with only the output retained.
These exact resource counts were equal between the two profiles. Source payloads
are allocated before the context and excluded; legacy STL bookkeeping/control
blocks and process RSS are outside this admission model. Output payload bytes
equal retained payload bytes in these fixtures.

| Cluster | Peak payload | Peak metadata | Retained payload | Retained metadata | Source support elements | Numeric evaluations |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| NUM-02 | 1120 → 3160 | 4960 → 1141504 | 8 → 2048 | 544 → 139264 | 1 → 2 | 1 → 256 |
| NUM-03 | 48 → 2088 | 4272 → 4272 | 8 → 2048 | 544 → 544 | 1 → 1 | 1 → 256 |
| NUM-06 | 3144 → 5184 | 11080 → 2214520 | 8 → 2048 | 544 → 139264 | 5 → 1280 | 1 → 256 |
| NUM-07 | 2329 → 2584 | 5528 → 5528 | 1 → 256 | 544 → 544 | 2 → 512 | 1 → 256 |
| NUM-08 | 7864 → 9904 | 7768 → 1828328 | 8 → 2048 | 544 → 139264 | 3 → 768 | 1 → 256 |
| NUM-09 | 408 → 4488 | 10168 → 10168 | 16 → 4096 | 5968 → 5968 | 2 → 512 | 2 → 512 |
| NUM-10 | 5216 → 7256 | 21832 → 3261352 | 8 → 2048 | 5792 → 5792 | 2 → 512 | 1 → 256 |
| NUM-11 | 4176 → 4176 | 40952 → 40952 | 8 → 8 | 5792 → 5792 | 1 → 256 | 1 → 256 |
| NUM-12 | 4224 → 38904 | 27464 → 2049328 | 8 → 2048 | 5792 → 1482752 | 1 → 256 | 1 → 256 |
| NUM-13 | 6176 → 10256 | 96704 → 6367808 | 16 → 2056 | 5792 → 5792 | 1 → 256 | 1 → 256 |
| NUM-14 | 2936 → 7016 | 63080 → 13586248 | 16 → 4096 | 5968 → 5968 | 8 → 518 | 2 → 512 |
| NUM-15 | 6024 → 10104 | 81296 → 10274824 | 16 → 2056 | 5792 → 5792 | 4 → 259 | 2 → 257 |
| CRV-03 | 519680 → 527840 | 143752 → 20597608 | 16 → 4096 | 5968 → 5968 | 8 → 518 | 2 → 512 |
| CRV-05 | 287000 → 291080 | 110736 → 9984072 | 8 → 2048 | 5792 → 5792 | 6 → 261 | 1 → 256 |
| CRV-06 | 723936 → 736176 | 70648 → 8192248 | 24 → 6144 | 5968 → 5968 | 9 → 264 | 3 → 768 |
| CRV-07 | 522024 → 534264 | 129976 → 15114776 | 24 → 6144 | 5968 → 5968 | 36 → 801 | 3 → 768 |
| CRV-08 | 208384 → 212464 | 75568 → 8832952 | 8 → 2048 | 5792 → 5792 | 3 → 258 | 1 → 256 |
| CRV-09 | 543248 → 550808 | 424380 → 4001956 | 192 → 3000 | 6032 → 6032 | unavailable → unavailable | 3 → 192 |

`source_elements` is the unique named source support union, not physical read
calls. NUM-11/13 additionally assert actual accumulator attempts. NUM-15 reports
2/257 sample accumulator attempts and includes two scalar controls in source
support. CRV-09's Result path currently returns no sample dependency-evidence
object; its source support is explicitly `unavailable`, not zero. Its only
external source is the nine-value axis. Its 3/192 numeric evaluations are the
interpolated validation-center components, not a claim that all generated table
components were included in that counter. The table request still executes the
global zero-tolerance quality gate; all expected table values are checked.

Every case passed exact bits and reported zero strict fallbacks in this fixture.
That describes these chosen exact/dyadic cases only; general logarithms, transfer
functions and other accelerated operations can take the documented Strict
fallback paths. Both profiles' CSVs include the same raw fields for every row.

The driver explicitly admits 256 MiB live payload, 128 MiB managed metadata and
64 MiB per-session state, with work limits of 64*2^30 execution and 32*2^30
per-dependency units. The first attempted 256-query Bézier run reached the
16 MiB default metadata ceiling; the final measured peak is shown above. This is
an example of explicit caller admission, not a change to library defaults.
Large dense association sets remain expensive, particularly sequence/remap,
sort and prefix/integration requests. These measurements complement the sparse,
streamed, resource-limit and independent numerical acceptance cases; they do not
justify a production throughput promise.
