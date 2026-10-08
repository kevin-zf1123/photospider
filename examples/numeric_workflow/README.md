# Numeric workflows

Most editable workflows use the installed C++ API, and most manual example
targets are excluded from the default build. The root registers behavior tests
for the prepared workflow and `photospider_numeric_math_batch`, plus
`test_numeric_unary_result`, `test_numeric_binary_result`,
`test_numeric_expression_result`, `test_numeric_sequences_result`,
`test_numeric_comparisons_result`, `test_numeric_ranges_result`,
`test_numeric_interpolation_result`,
`test_numeric_layouts_result`, `test_numeric_indexing_result`,
`test_numeric_reductions_result`, `test_numeric_ordering_result`,
`test_numeric_scans_result`, `test_numeric_calculus_result`,
`test_numeric_lut1d_result`, `test_numeric_lut3d_result`,
`test_numeric_baking3d_result`, `test_numeric_bezier_result`,
`test_numeric_parametric_result`, `test_numeric_inverse_result`,
`test_numeric_resampling_result`, `test_numeric_lowpass_result`,
`test_numeric_lowpass_nonuniform_result` and
`test_numeric_lowpass_execution_result`.
The installed consumer registers `installed_numeric_sequences_result`,
`installed_numeric_comparisons_result`,
`installed_numeric_ranges_result`, `installed_numeric_interpolation_result`,
`installed_numeric_layouts_result`, `installed_numeric_indexing_result`,
`installed_numeric_reductions_result`, `installed_numeric_ordering_result`,
`installed_numeric_scans_result`, `installed_numeric_calculus_result`,
`installed_numeric_lut1d_result`, `installed_numeric_lut3d_result`,
`installed_numeric_baking3d_result`, `installed_numeric_bezier_result`,
`installed_numeric_parametric_result`, `installed_numeric_inverse_result`,
`installed_numeric_resampling_result`, `installed_numeric_lowpass_result`,
`installed_numeric_lowpass_nonuniform_result` and
`installed_numeric_lowpass_execution_result`
for the corresponding example sources
linked to the SDK's `Photospider::kernel` target.
Building `test_numeric_result_math` also builds the unary, binary, expression,
sequence, range, interpolation, layout, indexing, reduction, ordering, scan,
calculus, LUT1D, LUT3D, measured LUT3D baking, Bezier, parametric Bezier,
inverse, resampling, uniform and nonuniform lowpass, lowpass execution and
math-batch executables.
This is not a complete list of the category's CTest targets.
The [implementation table](../../docs/built-in_ops/01-numeric/implementation.md)
records completed families and delivery validation.

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
python3 oracle/ops/numeric/expression_oracle.py build/numeric/examples/numeric_workflow/photospider_numeric_expression apple
build/numeric/examples/numeric_workflow/photospider_numeric_expression apple benchmark_quick
build/numeric/examples/numeric_workflow/photospider_numeric_category_benchmark apple
build/numeric/examples/numeric_workflow/photospider_numeric_category_benchmark apple extended
build/numeric/examples/numeric_workflow/photospider_numeric_category_benchmark apple legacy
build/numeric/examples/numeric_workflow/photospider_numeric_expression apple benchmark_wide
python3 examples/numeric_workflow/cost_inventory.py build/numeric/examples/numeric_workflow/photospider_numeric_inventory
```

`photospider_numeric_category_benchmark` builds Result-bound cases and reads
Result outputs, including current `reduce_sum` and `prefix_sum` diagnostics.
The historical Value-path CSV records below remain evidence for their recorded
Value runs only. To inspect current accumulator-attempt and fallback counters,
filter the Strict default cases:

```sh
build/kernel-dev/examples/numeric_workflow/photospider_numeric_category_benchmark strict default reduce_sum
build/kernel-dev/examples/numeric_workflow/photospider_numeric_category_benchmark strict default prefix_sum
```

Each command produced rows for N=1 and N=256. All four rows passed output
checks; `evaluated_values` was 1 and 256 respectively, with zero strict
fallbacks. This is a diagnostics smoke check, not a performance comparison.

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

## Finite elementwise Result operations

`finite.cpp` composes unsuffixed `numeric.abs`, `numeric.minimum`, and
`numeric.maximum` as Result nodes. It binds Float64 `[2,3]` inputs
`a=[[-0,-3,4],[5,-2,6]]` and `b=[[0,2,-4],[3,-7,9]]`. One
`execute_fragments` request selects coordinates `(0,1)` and `(1,2)` for all three
outputs. The Results contain `abs=[3,6]`, `minimum=[2,6]`, and `maximum=[3,6]` at
those coordinates; dependency source support for both `a` and `b` is exactly
the selected footprint.

Build and run the manual target explicitly. It is excluded from the default
build and has no CTest registration:

```sh
cmake --build build/kernel-dev --target photospider_numeric_finite -j8
build/kernel-dev/examples/numeric_workflow/photospider_numeric_finite
```

These unsuffixed finite keys validate typed tuples and atomic sample groups
before requesting Data for the selected footprint; matching validation and
Data support may share one Need. The suffixed `numeric.abs_strict`,
`numeric.minimum_strict`, and `numeric.maximum_strict` are separate Whole
operations with their own IEEE contracts.

## Basic curves, fields and analysis Results

`basic.cpp` composes `field.smoothstep` → `field.apply_lut_1d` → `grade.levels`
with `analysis.histogram` and `analysis.histogram_out_of_range`. It also samples
the same `[K,2]` controls with linear and monotone curves and connects the
smoothstep coverage Result to `image.mask`. Its inputs are Results: a Float32
`[2,2]` field `[-1,0,.5,2]`, a Float32 table `[0,2]`, Float64 controls
`[0,0,.5,.25,1,1]`, and a canonical RGBA Float32 image with cell shape `[2,2,4]`
and batch axes `[1,1]`.

The workflow checks coverage and levels `[0,0,.5,1]`, histogram bins
`[2,0,1,1]`, out-of-range counts `[0,0]`, linear samples
`[0,.125,.25,.625,1]`, monotone samples
`[0,.078125,.25,.546875,1]`, and masked alpha matching coverage. The output
schemas distinguish generic numeric tensors from canonical image coverage.
The `photospider_numeric_basic` target is an explicit manual executable, not a
CTest target.

```sh
cmake --build build/kernel-dev --target photospider_numeric_basic -j8
build/kernel-dev/examples/numeric_workflow/photospider_numeric_basic
```

The shared Result inputs and region rules are summarized in
[Basic Operations](../../docs/kernel-architecture/Basic-Operations.md).

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
description to a `[2,2,4]` Result input and uses the public `abs_node`, `Compiler`,
and `ExecutionContext` APIs to request the red component at `[1,0]`. The output
is `abs(-12)=12` with no color facet. The Data Need selects that component, while
the Whole operation validates the complete typed source, including alpha and
unrequested colors. Its relation records full source support; changing the
selected color's alpha is Validation-dirty for the requested output region.
Changing alpha in another color also fails Whole validation. The successful
request and its source-support mapping are checked with joint grouping both
enabled and disabled. A ResultRef window can read the authorized red component
alone; publishing that one channel as a ColorArray tensor is rejected because
the publication must contain a complete tuple. A full-tuple window remains
readable after releasing the source handle. The `tuple_output_view()` example
uses a ColorArray output with one explicit trailing axis; compiled publication
groups that axis with the typed channel tuple. It checks that the returned view
keeps the source data address and storage owner after context retirement and
that sample `[1,2]` still reads `6`.

```sh
cmake --build build/numeric --target photospider_numeric_color_array -j 8
build/numeric/examples/numeric_workflow/photospider_numeric_color_array
python3 oracle/ops/numeric/color_array_oracle.py build/numeric/examples/numeric_workflow/photospider_numeric_color_array
```

The standalone Fraction oracle checks 1,847 exact metadata decisions, including
extreme binary64 coordinates, zero normalization scales and near-singular bases.
These manual checks are excluded from CTest and integration registration.

`--joint-validation` runs a direct CPU Result contract-2 probe for tuple Needs.
It rejects a per-member Validation Need that omits channels and accepts complete
tuples, including tuples supplied by multiple Needs from the same member. A
second-stage history case checks that Validation from another observation cannot
close the current observation. `--dependency-proof` runs the compact mapping,
history, joint-transport, and schema-grouping checks. These probes exercise the
Result relation rules independently from the `public_workflow()` example.

```sh
cmake --build build/numeric --target photospider_numeric_color_array -j8
build/numeric/examples/numeric_workflow/photospider_numeric_color_array --joint-validation
build/numeric/examples/numeric_workflow/photospider_numeric_color_array --dependency-proof
```

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

The `ColorArrayDescriptor::profile` facet on the `samples` tensor names
`profile.value().identity()`. `ResourceBindings` supplies the accepted profile
to compilation and to the Result builder; a digest without a bound owner is
rejected. The compiled graph, runtime Result bindings and published Results
retain the selected immutable profile. Unused resources are pruned, and the
facet metadata remains separate from numeric samples.

The runnable graph declares `inks` as a Result input, binds a `ResultRef`, and
publishes a Result output. The compiler resolves declared and inferred profile
identities, including for Empty demand. The example's small Value fixtures only
check typed backing and facet behavior; they are not the workflow's public input
or output path. A generic numeric Result operation drops the ICC owner when its
output facet no longer references the profile.

The manual `manual.profile_output` operation exercises direct and structured
Result execution. It verifies that an Empty no-fields request retains the
profile without starting producer callbacks, that a late cancellation returns
`Cancelled` without publishing, and that insufficient referenced-resource
capacity rejects both Empty and nonempty requests before callback entry. A
partial CMYK request closes over the full color tuple in direct, ROI, atom and
`DemandHandle` paths. Replacing the source binding publishes a new Result with
the replacement sample and profile while a frozen Result continues to expose
the original sample and owner. Resource-bearing Results skip the optional
sample-only cache and remain valid through their owning Result references.

```sh
cmake --build build/numeric --target photospider_numeric_icc -j 8
build/numeric/examples/numeric_workflow/photospider_numeric_icc
python3 oracle/ops/numeric/icc_oracle.py \
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
partial-channel output closure in direct, ROI, atom and `DemandHandle` paths,
Empty Results, and an ICC reference limit that fails before callback entry.
Resource-bearing Results currently bypass the optional sample-only memory and
disk caches. Their owners remain available through Result references, read
windows and frozen snapshots.
The importer validates ICC v2/v4 CMYK output-device structure; optional and
private tag payloads are not a CMM transform certification. The example does
not perform color conversion or ICC LUT evaluation. Public C++ consumers must
rebuild against package 0.32.0. These manual targets have no integration/CTest
registration.

## Color ramps

The 45 CRV-06 profile keys expose 15 public helpers in
`photospider/numeric/color_ramps.hpp`: 42 keys cover eight non-RGB models and
three keys cover RGB/RGBA. The nine models are separate operations: RGB, CMYK,
XYZ, CIELAB, CIELCh(ab), OKLab, OKLCh, HSL and YCbCr. The required `colors_type`
argument is an authoring hint used to choose the default output dtype; an
explicit dtype option overrides it, and Compiler validates the connected Result
metadata. XYZ defaults to D65; CIELAB/CIELCh use D50. CMYK requires an imported
and bound ICC profile, and YCbCr requires an explicit NCL description. HSL
uses sRGB primaries, D65 and sRGB transfer coordinates by default.

Direct workflow nodes require static String parameters `color_description`,
`dtype` and `out_of_domain`; RGB also takes `output_association`, and hue
variants take `output_hue_unit`. Public constructors provide model-specific
description defaults and use `out_of_domain=clamp`; `reject` is the other
domain policy. RGB transfer can be Linear, Srgb or Gamma with an explicit
positive finite Float64 exponent. RGBA output association is independent of
input association and defaults to Premultiplied.

Every operation accepts ordered `input`, `stops` and `colors` Result inputs.
Each input has one tensor member under any schema id/version/key, and its full
`sample_shape()` includes batch axes. Input shape has rank 1..7 and a logical
product at most 2^40; stops have K=1..65536, and color rows have shape [K,C].
Rational-pi hue variants use split [K,2] colors and Int64 numerator and
denominator [K] inputs. A positive denominator q is required for a selected
mathematical row; an unselected generic q<=0 row has no added math-domain
check, while typed/upstream validation covers the complete inputs. Floating
ports independently accept Float32 or Float64. Output `values` is a Result
using `photospider.tensor` v1/member
`samples`, shape `sample_shape(input)+[C]`, with a ColorArray v1 facet and
`atomic_trailing_axes=1`. The output dtype defaults from the authoring hint.

`color_ramp_xyz_node` demonstrates the shared interface:

```cpp
auto node = ps::numeric::color_ramp_xyz_node(
    1, input, stops, colors, ps::ElementType::Float64,
    xyz_description);
```

For input=[.5], stops=[0,1] and colors=[[0,0,0],[.5,1,1.5]], the output is
[[.25,.5,.75]] with the declared XYZ description. Each input coordinate
selects one complete color. A fragment request for one output component still
returns the whole color tuple.

For RGB stops=[0,1] with black/white colors and linear transfer,
input=[0,.5,1] returns black, the exact encoded midpoint 0.5 in each channel,
and white. Under the default piecewise sRGB transfer, the midpoint bits are
Float32 `0x3f3c405b` and Float64 `0x3fe7880b5e230e4f`. For straight RGBA input,
transparent red to opaque blue at the midpoint returns `[0,0,.5,.5]` with the
default premultiplied output association; selecting straight output returns
`[0,0,1,.5]`.

The nine models interpolate supplied coordinates; these operators do not
perform model conversion, gamut clipping or hue wrapping. RGB decodes and
encodes according to its explicit transfer description, interpolating in linear
light. Three-channel RGB defaults to sRGB primaries/D65/sRGB transfer. RGBA
requires Straight or Premultiplied input association; output association is
independently selectable and defaults to Premultiplied. CMYK interpolation
retains and publishes the explicitly bound ICC profile, but does not perform
profile conversion. The shared target contract requires the native CIELAB/CIELCh
lightness coordinate l=L*/100. The current runtime still uses ColorArray v1's
older implicit L* scale; this Result migration does not change that scale, so
the difference remains open.

CIELCh, OKLCh and HSL each provide Radian, PiMultiple and RationalPi entrypoints.
Hue follows the original unwrapped coordinate, including at zero chroma or
saturation. RationalPi keeps the Int64 numerator and positive denominator exact
until final conversion. No shortest-path or normalization mode is applied.
YCbCr uses the caller's complete NCL matrix/range declaration; the operator
interpolates those encoded components without decoding or re-encoding them.

Each nonempty Whole Result execution requests Data, Validation and Descriptor
(role 13) for every input. Typed/upstream validation covers the complete tensor
members. Authorized windows feed the math directly without collecting or
copying full input arrays. The callback validates all stops, then all positions,
before color arithmetic. A query mathematically uses one exact hit, clamp or
singleton row, or two enclosing rows. Generic color rows outside evaluated
stencils remain numerically unused, while typed/upstream failures anywhere can
still fail the Run. Empty demand performs static preparation and reads no sample
payload.

The Result writer publishes a complete immutable `samples` tensor with full
certified coverage and global coordinates. The output retains the full-color
ColorArray facet and any ICC resource selected by that facet; the host records
source Result associations. Dirty mapping follows recorded output demand, while
an input edit invalidates the complete output. Source strides, offsets and
unaligned storage remain supported through authorized windows. The Root-owned
stop index uses 8*K bytes plus allocator/metadata overhead; arithmetic
workspaces, output storage and retained resources are charged to host budgets.
Cancellation or resource failure prevents partial publication. Public output
owners survive context teardown.

### Current validation

The focused `test_numeric_result_math` CTest passes 1/1 in 6.93 seconds. Its
`color_ramp_workflows`, `color_ramp_boundaries`, `color_ramp_icc` and
`color_ramp_active_cancel` cases cover Empty ICC execution, generic clamp
resource selection, ICC ownership, and cancellation during exact arithmetic in
both an RGB 640-limb slot and a coordinate 144-limb slot. The active-cancellation
cases return all Root resources to baseline after context teardown. ICC and
successful Result lifetime cases retain their valid owners after teardown.

Seven public manual groups pass under Strict and the locally available Apple
profile. The Result `--probe` matches 1,784 independent Fraction/Machin-pi
cases per profile and 352 RGB rational/root/Decimal cases per profile. Coverage
includes all nine models, hue entrypoints, batched and strided layouts, complete
typed/upstream validation, and Result output resource ownership. The installed
`installed_result_numeric` consumer passes 1/1 in 0.21 seconds; it calls public
XYZ and CMYK helpers and reads ColorArray output and its ICC owner after context
teardown. Repeated requests return consistent values and rebinding updates the
source association; these checks do not demonstrate a warm-cache hit.

Reproduce the focused suite, manual groups and independent oracles with:

```sh
cmake --build build/kernel-dev --target test_numeric_result_math photospider_numeric_color_ramps -j8
ctest --test-dir build/kernel-dev -R '^test_numeric_result_math$' --output-on-failure
build/kernel-dev/examples/numeric_workflow/photospider_numeric_color_ramps strict
python3 oracle/ops/numeric/color_ramp_oracle.py build/kernel-dev/examples/numeric_workflow/photospider_numeric_color_ramps strict
python3 oracle/ops/numeric/rgb_ramp_oracle.py build/kernel-dev/examples/numeric_workflow/photospider_numeric_color_ramps strict
build/kernel-dev/examples/numeric_workflow/photospider_numeric_color_ramps apple
python3 oracle/ops/numeric/color_ramp_oracle.py build/kernel-dev/examples/numeric_workflow/photospider_numeric_color_ramps apple
python3 oracle/ops/numeric/rgb_ramp_oracle.py build/kernel-dev/examples/numeric_workflow/photospider_numeric_color_ramps apple
```

Historical performance measurements were not rerun for this Result execution.
There is no x86 numerical or maximum-shape result, and CRV-06 has no GPU
variant. MPFR and the Fraction references are independent test oracles only.
The ColorArray v1 CIELAB/CIELCh lightness metadata difference from native
`l=L*/100` remains open; the Result migration does not convert units. See the
[CRV-06 family specification](../../docs/built-in_ops/01-numeric/op_specs/CRV-06_color_ramp.md)
for mathematical, resource and acceptance requirements.
## Joint three-axis color LUTs

`apply_lut3d_trilinear_node` and `apply_lut3d_tetrahedral_node` take Result inputs `input`, `table`, and `axis`. Each Result contains one tensor member under any schema id/version/key; shapes use the complete `sample_shape()`, including batch axes. Input shape is [...,3], rank 2..8, with product <=2^40. Table shape is [N0,N1,N2,3], with each Ni in 2..256. Axis is Float64[3,3], with one [start,end,step] row per input component. Input and table independently accept Float32/64. The `values` output is a packed immutable `photospider.tensor` v1 Result with member `samples`, the complete input shape, selected Float32/64 dtype, and ColorArray v1 facet with `atomic_trailing_axes=1`.

Pass input/output color descriptions explicitly. They must use the same supported three-component model; primaries, transfer, white and hue fields may differ where the model allows it. No alpha or CMYK input is supported, and no transfer conversion, model conversion or hue wrapping occurs. `Lut3dOptions` defaults output dtype from the authoring `input_type` hint, profile Strict and domain Reject; Clamp is the other policy. The hint is not inferred from the connected Result; the compiler validates the actual edge. ColorArray v1 still has its existing implicit CIELAB L* scale, while the revised shared coordinate contract requires l=L*/100. This migration does not convert units or close that compatibility gap.

```cpp
auto lut = ps::numeric::apply_lut3d_trilinear_node(
    10, input, table, axis, ps::ElementType::Float64,
    input_description, output_description);
```

For a 2x2x2 table storing (r*g,g*b,b*r) at binary vertices, axis rows [0,1,1], and input [.75,.25,.5], trilinear returns [.1875,.125,.375]; tetrahedral returns [.25,.25,.5]. Strict uses exact weights and one final rounding. Only positive-weight vertices enter arithmetic, at most eight for trilinear or four for tetrahedral. Trilinear uses tensor-product weights. Tetrahedral uses the main-diagonal six-tetrahedra split; exact local coordinates sort descending with input-axis order 0,1,2 breaking ties. Stored axis directions can change the tetrahedral interpolation result.

Each nonempty Whole Result invocation validates the dynamic axes, every original
input color and query before clamp, then evaluates the selected vertices. Typed
validation and upstream errors apply to the complete active inputs and may
precede callback numeric checks. The continuation records the current input,
table, and axis ObjectIds. A fresh content-equivalent source can reuse the output
cache while publishing associations to the current inputs. A repeated
identical demand returns the same output ObjectId. Input or table replacement
dirties the recorded query support; axis replacement updates axis-dependent
preparation and results. Static preparation survives query, table, and axis
rebinding.

The planner closes each requested sample to a complete three-component color.
The Result publishes full certified coverage in global sample coordinates,
including for sparse demand. Empty demand polls no numerical producer and reports
empty coverage. Each invocation publishes atomically. The Root continuation
retains source owners while authorized zero-copy read windows access input and
table storage; it does not make a dense copy of the whole table. `UniformAxis`
retains its Root-owned grids, costing 8*sum(Ni) bytes, while the packed output
and exact arithmetic workspace consume additional Root resources. Results and
authorized windows remain readable after source owners and the execution
context retire; releasing the final escaped window returns live Root resources
to zero.

The Result declaration requires a well-formed ColorArray facet whose atomic
channel axis is legal for its tensor layout. The fixture exercises reversed and
unaligned input/table layouts and verifies caller and worker floating
environments. Numeric failures have Run scope and output publication remains
all-or-nothing. Resource or work limits reject execution with a typed error and
release unpublished allocations; cancellation is checked during exact
arithmetic work.

The manual `lut3d.cpp` workflow runs six groups under Strict and Apple. It covers
both methods, Result bindings and reads, complete-color sparse closure, current
source associations, repeated ObjectId reuse, fresh-source cache hits,
preparation reuse and rebinding, Empty and failed table producers, typed
ColorArray validation, all input layouts, caller/worker floating environments,
and source-owner retirement. Trilinear and tetrahedral outputs retain 12 and
24 Payload bytes respectively after source owners retire. Escaped read windows
remain valid after context teardown, and releasing the final windows returns
all live Root resources to zero. A zero-stride Float64 table with maximum
256^3 grid shape uses an 8-byte backing window and succeeds under an 8 MiB Root
Payload cap, demonstrating that execution does not allocate a 384 MiB dense
table. A separate 2^38-position sparse query is rejected as `CapacityLimit` at
node 1 because the complete output exceeds capacity; it does not test
successful numerical execution at that output size.

The independent Fraction oracle passes 1,062 cases for each Strict and Apple
profile, with exact expected-string checks unchanged. The registered
`test_numeric_lut3d_result` runs the same manual fixture under Strict; the
shared `test_numeric_result_math` covers separate LUT workflow, boundary,
preparation, and resource integration fixtures and is not evidence for those
six manual groups.

The LUT3D-specific root CTest passed 1/1 in 1.48 s (1.49 s total). The shared
`test_numeric_result_math` passed in a separate CTest selection that also
included baking coverage (4.83 s); it was not paired with the LUT3D test. After
installing the current 0.32.0 package and rebuilding the consumer from the
current source, the three-test installed selection passed 3/3 in 5.71 s;
`installed_numeric_lut3d_result` took 2.36 s. The installed consumer's direct
Apple run also passed all six groups.

Reproduce the focused Result test with:

```sh
cmake --build build/kernel-dev --target test_numeric_lut3d_result test_numeric_result_math -j8
build/kernel-dev/examples/numeric_workflow/photospider_numeric_lut3d strict
build/kernel-dev/examples/numeric_workflow/photospider_numeric_lut3d apple
python3 oracle/ops/numeric/lut3d_oracle.py build/kernel-dev/examples/numeric_workflow/photospider_numeric_lut3d strict
python3 oracle/ops/numeric/lut3d_oracle.py build/kernel-dev/examples/numeric_workflow/photospider_numeric_lut3d apple
ctest --test-dir build/kernel-dev -R '^(test_numeric_lut3d_result|test_numeric_result_math)$' --output-on-failure
```

Reproduce the installed public-consumer check with:

```sh
cmake --install build/kernel-dev --prefix build/kernel-dev/result-only-install
cmake -S tests/consumer -B build/kernel-dev/repeated-result-consumer -DPhotospider_DIR="$PWD/build/kernel-dev/result-only-install/lib/cmake/Photospider" -DCMAKE_PREFIX_PATH="$PWD/build/kernel-dev/result-only-install"
cmake --build build/kernel-dev/repeated-result-consumer --target photospider_numeric_lut3d_consumer -j8
ctest --test-dir build/kernel-dev/repeated-result-consumer -R '^installed_numeric_lut3d_result$' --output-on-failure
build/kernel-dev/repeated-result-consumer/photospider_numeric_lut3d_consumer apple
```

The installed consumer compiles this same manual source against the installed
`Photospider::kernel` package (0.32.0); its CTest runs Strict, and the final
command runs Apple directly. The existing 0.18 Value-path benchmark is
historical and is not evidence of current Result performance. No x86 execution
or native GPU execution was run for this Result fixture. The
[CRV-07 contract](../../docs/built-in_ops/01-numeric/op_specs/CRV-07_apply_lut3d.md)
and [method specifications](../../docs/built-in_ops/01-numeric/op_specs/CRV-07A_apply_lut3d_trilinear.md)
retain the formulas, color support, limits, errors and acceptance requirements.

## Sequence generators

`sequences.cpp` declares dynamic scalar bindings, creates a `WorkflowDocument`,
compiles it with `Compiler`, and executes it with `ExecutionContext`. The public
`photospider/numeric/sequences.hpp` helpers `linspace_node` and `arange_node`
create nodes with explicit parameters. `SequenceInput` carries a workflow input
reference and an immutable, single-tensor Result schema hint; it does not retain
payload. The compiler validates the actual graph edge independently.

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

Named outputs are `values[count]` and `axis[3]`, each a separate Result identity.
Both use `photospider.tensor` / `samples`; the axis tensor has shape `[3]` and
`atomic_trailing_axes=1`, with Float64 components for floating sequences and
Int64 components for integer sequences. Whole execution reads authorized scalar
windows and computes the complete selected output before consumer projection.
The returned Result retains the complete published object and global sample
coordinates; a partial request is not a packed ROI Result. An axis-component
request closes over all three tuple components. For `count=1`, runtime reads
only the first input, while static specialization still checks the declared
second input. For larger counts both inputs are required, including for an
endpoint-only request. An overflow anywhere fails that selected values Result
with Run scope; axis failure leaves a separately successful values Result
intact. Active input edits invalidate the complete selected output.

The example's raw scalar `Value` objects are typed backing used to build source
Results. The caller-owned backing and generated payload are charged to the same
execution Root. Empty output demand produces empty coverage without sample
reads or arithmetic. The current Whole implementation does not report per-atom
numeric counters.
[Whole validation and timing](../../docs/built-in_ops/01-numeric/sequences-whole.md).

From the repository root, using Clang:

```sh
cmake -S . -B build/kernel-dev -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_BUILD_TYPE=RelWithDebInfo -DBUILD_TESTING=ON
cmake --build build/kernel-dev --target photospider_numeric_sequences -j8
build/kernel-dev/examples/numeric_workflow/photospider_numeric_sequences strict
ctest --test-dir build/kernel-dev -R '^test_numeric_sequences_result$' --output-on-failure
build/kernel-dev/examples/numeric_workflow/photospider_numeric_sequences apple_silicon
build/kernel-dev/examples/numeric_workflow/photospider_numeric_sequences x86_64
python3 oracle/ops/numeric/sequence_oracle.py build/kernel-dev/examples/numeric_workflow/photospider_numeric_sequences strict
```

The strict and available accelerated profiles run different registered keys;
an unavailable x86 profile returns `BackendUnavailable` rather than selecting a
different key. The oracle is independent of the sequence implementation, but
historical oracle counts and WSL/AVX2 runs below are not evidence for this
Result workflow or for native x86 execution.

Expected results include `linspace(0,1,5)` values `[0,.25,.5,.75,1]`, axis
`[0,1,.25]`, and Int64 `arange(3,-2,4)` values `[3,1,-1,-3]`, axis `[3,-3,-2]`.
The focused Result fixture checks exact values and tuple projection, extreme
cancellation, direct Float32 rounding, signed zero, unused failing producers,
independent axis overflow, static schema errors, cache dependencies, work and
capacity failures, strided input, cancellation, and Result readability after
context teardown. It also covers count-one exclusion of a failing second
producer while preserving static schema validation, count-two upstream failure,
and Empty requests without producer work or sample support. The latest completed
root and installed consumer CTests each passed 1/1; the complete `apple_silicon`
workflow also exited successfully. The independent Fraction/IEEE oracle passed
960 cases for each of `strict` and `apple_silicon`. Each oracle case checks a
selected global output index from Whole execution; it does not demonstrate
ROI-local sequence arithmetic. A step change from 0.5 to 1 recomputes the full
256-sample values Result: index 255 becomes 255 and `computed_elements` is 256.
After the execution context retires, the retained Result still accounts for at
least 2048 live Payload bytes; releasing its final owner returns all Root
resource dimensions to zero. No x86 execution or performance result is claimed.

`facilities.cpp` implements `manual.numeric_probe`, `manual.failed_source` and
`manual.structured_sum` as structured Result operations. Their workflow edges
and outputs are Result-backed; the structured example binds its source as a
Result tensor. The probe reports numeric work before returning either an atom
failure or a Need whose upstream source fails. The four baseline cases retain
`evaluated_values=2` and the original error provenance. A Host-pressure source
also reports one value, consumes its remaining Host headroom, and returns
`OperationFailed/ShortIo/Io/Group` at node 2. Its C2 caller has reported two
values, so the aggregate is three with either joint-grouping mode; the source
failure provenance remains intact. The Result timing entry is admitted from
Root before callback dispatch, so this path does not need to grow the timing
container after the callback consumes Host capacity. Disabling joint grouping
still drives the probe's required single-member C2 joint callback.

The structured consumer binds a five-sample input Result and reads it through a
Tensor Need over two polls per observation. It checks five observations, ten
polls and `sum=35`. Its producer's `atomic_trailing_axes` value is preserved in
the consumer's compiled input metadata; a direct joint start with mismatched
static grouping returns `Stale`. The example also reports both
`OperationTiming.numeric.evaluated_values` and `computed_elements` to show that
arithmetic work and published output samples are separate counts.

## Installed consumer

Build the kernel as above, then compile the example sources against its
installation:

```sh
cmake --install build/kernel-dev --prefix "$PWD/build/kernel-dev/sequence-install"
cmake -S tests/consumer -B build/kernel-dev/sequence-consumer -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_PREFIX_PATH="$PWD/build/kernel-dev/sequence-install" -DPhotospider_DIR="$PWD/build/kernel-dev/sequence-install/lib/cmake/Photospider" -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build/kernel-dev/sequence-consumer --target photospider_numeric_sequences_consumer -j8
ctest --test-dir build/kernel-dev/sequence-consumer -R '^installed_numeric_sequences_result$' --output-on-failure
cmake --build build/kernel-dev/sequence-consumer --target photospider_numeric_interpolation_consumer -j8
ctest --test-dir build/kernel-dev/sequence-consumer -R '^installed_numeric_interpolation_result$' --output-on-failure
```

The sequence and interpolation installed-consumer tests compile their example
sources against the installed `Photospider::kernel` package. Historical
Value-workflow and WSL/AVX2 results are not current evidence for the Result
source path or native x86 support. These examples make no performance claim.

The range consumer can be built against the same installed SDK:

```sh
cmake --install build/kernel-dev --prefix build/kernel-dev/result-repeat-install
cmake -S tests/consumer -B build/kernel-dev/repeated-result-consumer -DCMAKE_PREFIX_PATH="$PWD/build/kernel-dev/result-repeat-install" -DPhotospider_DIR="$PWD/build/kernel-dev/result-repeat-install/lib/cmake/Photospider" -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build/kernel-dev/repeated-result-consumer --target photospider_numeric_ranges_consumer -j8
ctest --test-dir build/kernel-dev/repeated-result-consumer -R '^installed_numeric_ranges_result$' --output-on-failure
```

The installed range consumer passed 1/1 and links the installed
`Photospider::kernel` package.

## Expression sampling and static preparation: NUM-01

`sample_expression_node` in `photospider/numeric/expression.hpp` authors the
three suffixed profile keys. Its Result inputs are one tensor member under any
key, each with `sample_shape()` [1] and Float32 or Float64 dtype; ports may mix
dtypes. The `values` output uses `photospider.tensor` / `samples`, selected dtype
and shape `[N]`. The `axis` output uses the same schema/member, Float64 shape
`[3]`, and an atomic trailing tuple `[start,end,step]`; for N=1 it is
`[start,start,+0]`. Each output has its own ObjectId and associates the actual
active input ObjectIds. The workflow connects the output ports explicitly; no
additional values/axis pairing identity is created.

The public helper takes each named coefficient as its own workflow edge:

```cpp
auto node = ps::numeric::sample_expression_node(
    1, "a*x+b", ps::WorkflowInputReference{1}, ps::WorkflowInputReference{2},
    5, {{"a", ps::WorkflowInputReference{3}},
        {"b", ps::WorkflowInputReference{4}}},
    ps::ElementType::Float64, ps::CpuNumericProfile::Strict);
```

With start=0, end=1, a=2 and b=1, `values` is `[1,1.5,2,2.5,3]` and
`axis` is `[0,1,0.25]`. Rebinding a=3 and b=-1 on the same compiled workflow
changes values to `[-1,-0.25,0.5,1.25,2]` and leaves axis unchanged.

Static preparation validates the grammar, coefficient names and all declared
input schemas, then shares one immutable AST between both outputs and future
runs with new dynamic bindings. Whole execution requests active inputs with
Data, Validation and Descriptor (role 13) in batches of at most 64 ports. With
128 coefficients, values has 130 declared ports; for count=1 it requests 129
active inputs in three Need stages before publication. Registration declares a
conservative 258 input slots and reserves five input Need stages plus
publication, but the 256-node AST limit constrains the number of realizable
distinct coefficients. Count=1 values uses start and coefficients but excludes
end; count=1 axis uses only start. For N>=2, values uses start, end and
coefficients, while axis uses only start and end. Empty does no payload work.

Strict evaluation follows the documented coordinate interpolation and RN64 step
rules, then evaluates each AST primitive in left-to-right postorder with its own
RN64 rounding. It does not round the complete expression only once. Accelerated
profiles propagate strict RN64 reference enclosures through four-sample batches
and replay uncertain samples strictly. Final output conversion occurs once.
See [NUM-01](../../docs/built-in_ops/01-numeric/op_specs/NUM-01_sample_expression.md)
for the grammar, coordinate and numeric contracts.

The expression workflow uses the configured `build/kernel-dev` tree:

```sh
cmake --build build/kernel-dev --target photospider_numeric_expression -j8
ctest --test-dir build/kernel-dev -R '^test_numeric_expression_result$' --output-on-failure
python3 oracle/ops/numeric/expression_oracle.py \
  build/kernel-dev/examples/numeric_workflow/photospider_numeric_expression strict
build/kernel-dev/examples/numeric_workflow/photospider_numeric_expression apple
python3 oracle/ops/numeric/expression_oracle.py \
  build/kernel-dev/examples/numeric_workflow/photospider_numeric_expression apple
cmake --build build/kernel-dev/repeated-result-consumer \
  --target photospider_numeric_expression_workflow_consumer -j8
ctest --test-dir build/kernel-dev/repeated-result-consumer \
  -R '^installed_numeric_expression_result$' --output-on-failure
```

`expression.cpp` builds a `WorkflowDocument`, binds source Results and invokes
the public Result execution API. Its `Value` objects are local typed backing;
the fixture publishes them under the execution Root and accounts their storage
as Referenced. It does not use the separate unsuffixed
`numeric.sample_expression` operation.

The root registers `test_numeric_expression_result` for the strict example and
`installed_numeric_expression_result` for the same source linked only to the
installed `Photospider::kernel` target. Both tests passed after the balanced
128-coefficient, association and Empty checks were added. The strict and Apple
default runs passed, and the independent Fraction/MPFR oracle passed 715 cases
per profile with MPFR 4.2.2. The focused expression test checks that both
output programs share the prepared AST, mixed Float32/Float64 scalar inputs,
and a balanced 128-coefficient expression (255 AST nodes, height 8) over 130
declared ports.
That case supplies 129 active inputs in three Need polls, then publishes the
outputs. Other cases check values and the atomic three-component axis, including
a selected-component request, N=1 omission of the failing end producer,
axis-only omission of coefficient inputs, full-domain failure outside a
requested ROI, duplicate adjacent coordinates, invalid input metadata,
Float32 overflow, and Result lifetime. A valid Empty values+axis
request leaves both coverages empty, computes zero elements, and does not start
the failing coefficient producer. The broader `test_numeric_result_math`
kernel suite also covers Empty expression queries.

Cache checks distinguish two paths: repeating a request on the same Frozen
binding reuses each output ObjectId; a fresh source Result and fresh Frozen
binding can hit completed values and axis entries; tests compare each output's
association with the active input ObjectIds. Rebinding coefficients changes
values while leaving axis clean. Tests also cover worker and caller
floating-environment restoration, 1024-unit work rejection, cancellation after
the callback has admitted at least 10000 units, and final Root resource release.
The cancellation check does not identify a particular expression node or
refinement step as the interruption point.

The expression executable keeps `benchmark`, `benchmark_quick`, and
`benchmark_wide`. The quick mode runs `2*x+1` and `exp(x)` at N=65536 for both
Whole and three-point ROI queries, producing four rows; the wide mode uses the
same count with a wider interval. Each row has one warmup and seven timed
executions. `invocations` is the operation poll count, while
`computed_elements` reports the full N because the Whole operation computes
before projection. The evaluated-value, strict-math-call and fallback columns
are `N/A` for this Whole Result path.

The strict quick run's four rows passed their output checks: each reported
poll_count 2 and computed_elements 65536 for both Whole and ROI. The shared
context's cumulative Root Payload peak was 765520 bytes, including warmup.
This records the run's managed Payload use; it is not a speed comparison.

Workflow timing covers `execute_fragments`; source creation, compilation,
freezing, output reads and bit checks are outside the timer. The reported
`peak_payload_bytes` is the Root's cumulative Payload peak across setup, warmup
and timed runs, excluding input storage charged as Referenced. It is not RSS.
These fields describe the benchmark boundary and do not provide a speed
comparison with the older Value/callback measurements.

### Whole static preparation in `prepared.cpp`

The `manual.whole_prepared` case accepts one Float64 scalar Result and publishes
two Results: `values` has shape `[5]`, and `axis` has shape `[3]` with one atomic
trailing axis. Static preparation creates one immutable program shared by both
outputs and by the semantic, optimized, and compiled representations. Repeated
execution reuses that prepared program. A request for one axis component closes
to the complete three-component tuple; direct and concurrent Result starts
check the exact Float64 bits for 7.0.

The `mask` parameter selects the static input projection. `mask=0` excludes the
input, so an upstream Result source is not started. `mask=1` selects the scalar
input and records its ObjectId in each output association. Duplicate and
out-of-range projections (`mask=2` and `mask=3`) are rejected during
specialization. When the input is selected, an upstream source failure reaches
the Whole operation. Direct `start_result` checks the prepared schema and exact
parameter seal before starting the callback. This paragraph describes the
Whole Result case; C2 Result behavior is described below.

An Empty tensor request also skips the selected source Need and all tensor
reads and writes. The callback seals a metadata-only Result with empty tensor
coverage; the source-start count remains at the single start from the earlier
nonempty failing-source check.

### C2 Result preparation and joint execution

The `manual.prepared` operation declares two Result outputs and a C2
`PerAtomOutcome` joint callback. One compile prepares an immutable `Program`
shared by both outputs and by the semantic, optimized, and compiled plans.
`execute_atoms` returns six independent Result observations: five `values`
samples and one `axis` tuple. The axis query closes to its complete three-value
atomic tuple. Every checked output sample has the exact Float64 bits for 7.0.

With cache capacity set to zero, disabling grouping produces six one-member
joint starts rather than switching to the singleton callback; enabling grouping
reduces the start count below six. Sparse value queries and tile plans reuse the
same preparation. Direct joint checks with two and five members prepare once,
start once, poll once, and publish the requested Results. Static seals reject a
foreign registry, changed schema, signed-zero parameter drift, and distinct
NaN payload bits before another preparation. A started continuation can outlive
the registry and external prepared handle, and its published Result remains
readable after the continuation is released. The test checks the retained
payload falls from eight bytes to zero when the last Result owner is released.
These are independent per-atom Result publications; this example does not
exercise aggregation of a wide Object or Field dependency into one Result.

### Whole Result view preparation

`manual.whole_view` requests its complete input tensor with Data, Validation,
and Descriptor roles. In compiled structured CPU Whole execution, the
coordinator validates and supplies that Need, then prepares each authorized box
before the next computation poll. It first proves an affine view over the
existing owner. If that mapping is unavailable, the strict view policy returns
`ViewUnavailable` before the computation callback; Auto may instead collect
the authorized samples into immutable Root-owned private backing retained by
the `ResultTensorInput`. The callback acquires that prepared window and
publishes a Result tensor view. Auto falls back only for an unavailable view. A
work, cancellation, or resource failure remains an error.

The fixture checks a zero-stride Result with an 8-byte backing and an 8-byte
output payload cap, multi-owner rejection, Auto collection with a zero-byte
output cap, and same-owner fragments that join into an affine view without
collection. Collection and its metadata still consume Root capacity even when
the output cap is zero. Under the fixture's 80-byte Root Payload cap, the source
completes and seals before Auto preparation returns `ResourceExhausted`; the
computation callback does not run, and live Payload returns to zero. A
pre-cancelled execution also avoids source payload and computation. An
out-of-domain tensor read returns `UnauthorizedRead`; copying the capability
shares the immutable prepared backing without widening its grant. The output
keeps its Result schema, association, and resource owners; its physical view
retains the source Result. An external input owner remains readable after the
execution context retires, and all Root resource dimensions reach zero after
the final owner is dropped. A deliberately ignored workspace-allocation error
remains sticky as `ResourceExhausted`; the output cap checks publication and
does not prevent earlier Root allocations. Direct `ResultProgramPhase` calls
do not automatically fulfill Needs or prepare these views. This path is limited
to compiled structured CPU Whole execution; it does not extend to joint, GPU,
or the C Result ABI.

The completed-result cache also exercises this path. A warm request replays the
original grant and dependency facts without running the computation callback;
setting `maximum_dependency_cache_work=0` instead recomputes normally and
reports zero cache work. Cache misses use the ordinary Need path and prepare
views before computation.

The focused local `test_numeric_prepared` CTest and the manual executable pass
with these Whole view checks. The installed-kernel consumer uses the same
`prepared.cpp` source. These checks do not imply support for wider Object/Field
aggregation or joint/GPU view preparation.

Rebuild and run the focused local target with:

```sh
cmake --build build/kernel-dev --target test_numeric_prepared photospider_numeric_prepared -j8
ctest --test-dir build/kernel-dev -R '^test_numeric_prepared$' --output-on-failure
build/kernel-dev/examples/numeric_workflow/photospider_numeric_prepared
```

The separate unsuffixed `numeric.sample_expression` and `lut.apply_1d` Result
workflow is demonstrated by the `expression-lut` scenario in the
[foundations workflow](../foundations_workflow/README.md). Those keys use a
coefficient-vector input and a SampledSignal query/table contract; they are not
the named-coefficient profile operations described in this section.

## Array construction: NUM-03

The NUM-03 examples use Result inputs and outputs throughout the public
workflow. `ArrayWorkflow::source` converts the example's generic numeric
backing into a typed Result binding; compilation, freezing, ordinary execution,
fragment execution, and `DemandHandle` requests operate on Results. The
`photospider/numeric/arrays.hpp` helpers expose `constant_node` and
`broadcast_node`, each with an explicit shape, `View` or `Dense` layout, and
numeric profile. The example's `Value` objects are local storage backings for
generic numeric data, not workflow ports.

```sh
cmake --build build/kernel-dev --target photospider_numeric_arrays test_numeric_result_arrays -j8
build/kernel-dev/examples/numeric_workflow/photospider_numeric_arrays _strict
ctest --test-dir build/kernel-dev -R '^test_numeric_result_arrays$' --output-on-failure
```

The manual executable covers ordinary compilation and execution as well as
frozen and fragmented requests. `test_numeric_result_arrays` runs the Result
array scenarios directly; strict and available accelerated CPU profiles are
checked, with an unavailable x86 profile reported as `BackendUnavailable` on
the tested Apple Silicon host.

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

`constant` and `broadcast_examples` exercise the public whole-output behavior.
Constant View owns an independent scalar-sized copy, while Broadcast View
retains the complete source Result owner and its strides. Dense can collect a
multi-owner source; View returns `ViewUnavailable`. Empty requests do not call
the source operation. The `manual.array_split` source publishes a generic
Int64 `{2}` Result whose two samples have independent physical owners. View
cannot represent that split backing, while Dense collects both samples. A
separate typed Float32 spatial image Result with invalid RGBA semantics is
rejected. This fixture uses a 1 MiB Root payload limit and a 64 KiB source page.

`array_boundaries` checks all 256 UInt8 source values and an unaligned,
negative-stride Int64 mapping. `structured_views` compiles a custom consumer
that requests the last sample of a giant broadcast through Tensor Need. A
separate input has one batch axis of extent 2 and a cell shape of `{3}`; the
consumer reads the last sample at batch coordinate 1 and cell coordinate 2.
The test also checks ordinary and frozen `[2^20,2^20]` constant Results and a
`[2^38,3]` broadcast, each backed by an 8-byte scalar where applicable.

`broadcast_cache` distinguishes repeated queries against the same frozen
Result object from reuse with a fresh, equivalent Result source. Rebinding both
an unobserved and an observed source invalidates the Whole output; the new
demand sees the replacement while the old frozen binding retains its value.
`array_owner_and_payload_cache` checks NaN sign and payload bits in cache keys,
an oversized 4096-byte scalar backing that retires after an 8-byte constant
Result is published, and a broadcast Result that retains its 24-byte source
backing. An acquired read window keeps that storage alive after the Result is
released; releasing the window retires the final payload owner.

`whole_array_budgets` and `array_schema_and_capacity` check malformed shapes,
map constraints, output and work limits, and cancellation after output
allocation or during copying. Work is precharged before the dense output is
allocated, so the low-work case fails before the dense output is admitted.
Small execution state may already have been allocated. The cancellation checks
separately trigger after output allocation and after
additional issued work confirms that copying is underway; failed runs release
unpublished output storage. The bit-pattern cases set each of the four rounding
modes on the calling thread and verify that its mode and preexisting
`FE_DIVBYZERO` flag are unchanged. They do not assert floating-point state on
worker threads. No current Result performance measurements are claimed by these
correctness cases.

The six `numeric.constant` and `numeric.broadcast` keys execute Whole Result
operations. A constant View owns one scalar-sized copy; a broadcast View retains
its complete source owner and rejects a source that needs multiple owners.
Dense requests materialize the complete target before consumer projection.
Source edits invalidate the complete output. Constant Dense fills packed output
by growing a repeated prefix and copying blocks of at most 64 KiB between
cancellation polls. Broadcast uses Scalar/NEON/AVX2 32-byte gather-copy blocks
and exact tails. These are current implementation behaviors, not claims about
the historical timing rows. The [NUM-03 Whole execution page](../../docs/built-in_ops/01-numeric/arrays-whole.md)
documents the current Result behavior and keeps earlier timing records
separate. `array_owner_and_payload_cache()` exercises Result cache identity and
the lifetime of scalar-copy and borrowed-window owners.

`test_numeric_result_arrays` compiles `arrays.cpp` with `PS_RESULT_ARRAY_TEST`
and runs the Result array cases, including `staged_array_support`, cache and
owner tests, profile-specific boundaries, `structured_views`, and dense/schema
checks. It verifies a dense `[2,3]` Result filled with six Int64 sevens,
malformed shapes rejected during schema validation, and Int64/Float32/Float64
bit preservation. Ordinary and frozen constant Results cover shape
`[2^20,2^20]`; the structured consumer reads a `[2^38,3]` broadcast through
Tensor Need and returns a one-sample Result. The batch-axis case has one batch
axis of extent 2 and cell shape `[3]`; the last coordinate is `[1,2]`.
Published Results and acquired windows remain readable after context teardown
until their final owners are released.

Final validation on this host passed the local Result array and output-payload
tests (2/2), with the array behavior test also passing five repeat-until-fail
runs. The fresh installed consumer passed `installed_result_arrays` and
`installed_result_output_payload` (2/2). Standalone imported-package runs of
`photospider_numeric_arrays` with `_strict` and
`_accelerated_apple_silicon` each exited successfully.

```sh
cmake --build build/kernel-dev --target test_numeric_result_arrays -j8
ctest --test-dir build/kernel-dev -R '^test_numeric_result_arrays$' --output-on-failure
```

The installed consumer is registered as `installed_result_arrays`, and that
consumer has passed. Configure a fresh imported-package build and run it with:

```sh
cmake --install build/kernel-dev --prefix build/kernel-dev/arrays-result-install
cmake -S tests/consumer -B build/kernel-dev/arrays-result-consumer \
  -DCMAKE_PREFIX_PATH="$PWD/build/kernel-dev/arrays-result-install" \
  -DPhotospider_DIR="$PWD/build/kernel-dev/arrays-result-install/lib/cmake/Photospider" \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build/kernel-dev/arrays-result-consumer --target photospider_result_arrays_consumer -j8
ctest --test-dir build/kernel-dev/arrays-result-consumer -R '^installed_result_arrays$' --output-on-failure
```

The focused Result cap probe is separate from those array workflows. CMake
compiles `arrays.cpp` with `PS_RESULT_PAYLOAD_TEST` as
`test_result_output_payload`. Its `manual.output_payload` operation declares an
8-byte output cap and a separate 8-byte workspace, then attempts to publish a
16-byte Float64 tensor. Direct polling rejects that publication with sticky
`ResourceExhausted/CapacityLimit`; compiled execution also rejects it, and the
joint path reports the cap failure per member. A second case sets the cap to
zero and publishes an authorized view of an input Result, confirming that
shared physical backing is not charged as new output payload. This is a
publication-time bound: the callback's attempted allocation is still charged
to the Root before the host rejects the Result. The probe also checks
cumulative prefix limits with and without backing reuse, metadata admission
before callback entry, foreign-Root rejection, and preservation of a prior
callback failure. Cancellation during one joint member's cap scan leaves a
healthy peer able to succeed. The same probe is built as an installed consumer
against the imported kernel and retains the existing Metadata/Entries
bookkeeping checks.

```sh
cmake --build build/kernel-dev --target test_result_output_payload -j8
ctest --test-dir build/kernel-dev -R '^test_result_output_payload$' --output-on-failure
```

Build the installed consumer from the imported kernel package with:

```sh
cmake --install build/kernel-dev --prefix build/kernel-dev/consumer-install
cmake -S tests/consumer -B build/kernel-dev/consumer-build -DCMAKE_PREFIX_PATH="$PWD/build/kernel-dev/consumer-install"
cmake --build build/kernel-dev/consumer-build --target photospider_result_output_payload_consumer -j8
ctest --test-dir build/kernel-dev/consumer-build -R '^installed_result_output_payload$' --output-on-failure
```

## Range operations: NUM-06

`photospider_numeric_ranges` publishes its input data as tensor Results, builds
a `WorkflowDocument`, and composes public `broadcast_node` helpers with the
versioned `numeric.remap_range` and `numeric.clamp` keys. The Result operations
require matching complete `sample_shape()` and dtype across every input; they
do not broadcast or cast implicitly. The example uses four broadcast bounds to
map `[0,.5,1,2]` to `[0,127.5,255,510]`, then clamps the result to
`[0,127.5,255,255]`.

Each operation runs Whole. A nonempty request reads authorized windows and
validates all active inputs, including bounds outside the requested output
projection, before publishing the complete output Result. A sparse query still
returns the full Result with global sample coordinates; it does not create a
packed ROI Result. The output drops input facets and batch-axis topology. Its
dependency association names the actual current source ObjectIds. For a sparse
query, edits to every input dirty the observed output footprint without
changing the full-Result publication contract. Empty demand publishes empty
coverage without payload reads or sample arithmetic.

The fixture's raw `Value` objects are typed backing only: it creates source
Results under the execution Root using the original storage rather than
collecting them into a second input buffer. Its typed clamp case attaches an
RGB Straight-alpha facet to the upper-bound Result. A sparse green-channel query
still rejects an invalid unselected alpha at input 3; a valid alpha permits the
full generic output, whose facets are empty. Cache checks distinguish same-
Frozen identity sharing from a completed-result hit for fresh source Results,
verify that replay associates the current input ObjectIds, and show that
changing a bound invalidates the cached output.

Build the focused executable and run its registered Result test with Clang:

```sh
cmake --build build/kernel-dev --target photospider_numeric_ranges -j8
build/kernel-dev/examples/numeric_workflow/photospider_numeric_ranges _strict
ctest --test-dir build/kernel-dev -R '^test_numeric_ranges_result$' --output-on-failure
python3 oracle/ops/numeric/range_oracle.py \
  build/kernel-dev/examples/numeric_workflow/photospider_numeric_ranges _strict
```

The focused Result test checks Whole errors and support/dirty mapping, required
five-port reads even at endpoints, typed validation, warm-cache bound changes,
caller and worker floating-environment restoration, negative/zero/unaligned
strides, shifted origins, singleton axes, 65-element Float32 tails, and global
coordinate projection. Every input's dirty mapping for sparse demand returns
the observed output footprint, while publication remains complete. Work,
Payload, scratch and cancellation failures release unpublished output. The
separate `test_numeric_result_math.cpp` integration fixture retains broader
clamp/remap checks but was not rerun for this migration. Profile names are `_strict`,
`_accelerated_apple_silicon` and `_accelerated_x86_64`; an unavailable profile
returns `BackendUnavailable` rather than falling back to another key. The
current root Result test passed 1/1 and the Apple Silicon default workflow
exited successfully. The independent exact Fraction oracle passed 2,826 cases
for each of the strict and Apple Silicon profiles. The installed range consumer
passed 1/1. No native GPU, x86 execution or performance result is claimed. See
[NUM-06 Whole behavior and checks](../../docs/built-in_ops/01-numeric/range-whole.md).

## Interpolation: NUM-08

`photospider_numeric_interpolation` creates source tensor Results and composes
the six versioned `numeric.mix_*` and `numeric.smoothstep_*` keys through a
public `WorkflowDocument`, `Compiler`, `ExecutionContext` and explicit
`broadcast_node` path. The fixture's `Value` instances are typed backing only;
the workflow publishes source Results under the execution Root using their
original storage.

```sh
cmake --build build/kernel-dev --target photospider_numeric_interpolation -j8
build/kernel-dev/examples/numeric_workflow/photospider_numeric_interpolation _strict
ctest --test-dir build/kernel-dev -R '^test_numeric_interpolation_result$' --output-on-failure
python3 oracle/ops/numeric/interpolation_oracle.py \
  build/kernel-dev/examples/numeric_workflow/photospider_numeric_interpolation _strict
```

Use `_accelerated_apple_silicon` on Apple Silicon or `_accelerated_x86_64` on
x86-64. The workflow expects
`smoothstep=[0,0,.15625,.5,.84375,1,1]` and
`mix=[10,10,11.5625,15,18.4375,20,20]`. Each operation uses Whole execution and
requests Data, Validation and Descriptor support (role 13) for all three inputs.
The callback validates all factors or edges, reads the authorized windows, and
publishes the complete output Result. A sparse query keeps global coordinates
and a sparse dependency footprint; it does not produce a packed ROI Result.
Invalid factors or edges anywhere fail with Run scope and no Atom key. Mix
requires and validates both endpoints even at `t=0` or `t=1`; upstream and typed
failures can precede factor validation. Smoothstep validates edges before
processing a sample, so invalid edges take precedence over input NaN. Empty
demand returns empty coverage without payload reads, arithmetic or input support.

The current workflow checks the smoothstep-to-mix composition, complete support
and per-port dirty mapping, sparse global-coordinate queries, and upstream
failures for both mix endpoints even when one endpoint is selected. Its typed
RGB Straight-alpha cases reject an invalid unselected alpha and accept the
same-schema valid control; outputs drop input facets. Cache checks distinguish
same-Frozen identity reuse from completed-result reuse with fresh source
Results, verify association to current source IDs, and confirm that editing an
unselected endpoint invalidates and recomputes the Whole output. Changing the
factor updates the value. Negative/unaligned layouts, shifted origins,
65-sample Float32 tails, sNaN endpoint bit copies, and caller/worker floating-
environment restoration are also checked. Work, payload, scratch and
cancellation failures release unpublished results; a retained Result remains
readable after context teardown, and its final release returns all Root
resources to zero.

The current Result test passed 1/1, the Apple Silicon default workflow exited
successfully, and the installed consumer passed 1/1. The independent IEEE/Fraction
oracle passed 5,244 cases for each of the strict and Apple Silicon profiles. No
x86 execution, native GPU support or performance result is claimed. The separate
`test_numeric_result_math.cpp` integration fixture retains additional batch,
special-value and pre-cancellation coverage; that broader fixture was not
rerun for this Result update. See [NUM-08 Whole behavior and checks](../../docs/built-in_ops/01-numeric/interpolation-whole.md).

## Layout transforms: NUM-09

`photospider_numeric_layouts` exercises the nine `array.*` profile keys through
the public `reshape_node`, `transpose_node` and `slice_node` helpers in
`photospider/numeric/layouts.hpp`. Each source is a single-tensor Result under
any schema/member key; raw `Value` objects in the fixture provide backing only.
Results are published under the same execution Root. The helpers default to
`TransformLayout::Auto`; `View` and `Dense` can be selected explicitly.

```sh
cmake --build build/kernel-dev --target photospider_numeric_layouts -j8
build/kernel-dev/examples/numeric_workflow/photospider_numeric_layouts strict
ctest --test-dir build/kernel-dev -R '^test_numeric_layouts_result$' --output-on-failure
build/kernel-dev/examples/numeric_workflow/photospider_numeric_layouts apple
python3 oracle/ops/numeric/layout_oracle.py \
  build/kernel-dev/examples/numeric_workflow/photospider_numeric_layouts strict
python3 oracle/ops/numeric/layout_oracle.py \
  build/kernel-dev/examples/numeric_workflow/photospider_numeric_layouts apple
```

The installed consumer compiles the same example source against the installed
SDK package:

```sh
cmake --install build/kernel-dev --prefix "$PWD/build/kernel-dev/layouts-result-install"
cmake -S tests/consumer -B build/kernel-dev/layouts-result-consumer -DCMAKE_PREFIX_PATH="$PWD/build/kernel-dev/layouts-result-install" -DPhotospider_DIR="$PWD/build/kernel-dev/layouts-result-install/lib/cmake/Photospider" -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build/kernel-dev/layouts-result-consumer --target photospider_numeric_layouts_consumer -j8
ctest --test-dir build/kernel-dev/layouts-result-consumer -R '^installed_numeric_layouts_result$' --output-on-failure
```

The CLI profile arguments are `strict`, `apple` and `x86`; each selects its
registered CPU profile. Unsupported profiles return `BackendUnavailable`.
Reshape preserves row-major logical order, transpose applies a static axis
permutation, and slice uses static counts with dynamic Int64 starts and steps.
The complete input `sample_shape()` includes batch axes, has rank 1..8 and no
more than 2^40 logical samples. Supported types are UInt8, Int8, UInt16, Int16,
Int64, Float32 and Float64. Outputs use
`photospider.tensor` / `samples`, the complete transformed shape as ordinary
axes, and empty facets/batch-axis metadata.

All three operations use CPU Whole execution. A nonempty query requests complete
active input support with Data, Validation and Descriptor roles (role 13),
which triggers typed-payload validation, then publishes a complete Result and
preserves global output coordinates. `Q`
restricts observed dependencies and consumer reads, not output
publication. A View requires one affine source/output owner; compatible fragments
may join only when they share that owner. The workflow compares owner tokens and
first addresses for legal views. Multiple owners fail `View`; Auto falls back
to publishing a complete Root-accounted packed output only for
`ViewUnavailable`, while Dense always publishes a complete packed output.
Other failures do not trigger fallback. Empty queries have empty coverage and
support. Cross-run content caching is disabled because content
does not prove physical layout; same-Run sharing remains available.

Coverage includes invalid shape/type/step metadata, the all-singleton step-port
exclusion rule, exact full-domain slice checks, typed RGB alpha validation and
valid typed controls, zero-stride views and dense-budget rejection, same-owner
fragment joining, independent-owner rejection, escaped Result/window lifetime,
and final Root resource release. The operation reports the selected
implementation and `view_elements` or `copied_elements`. The current Root
focused test and strict/Apple seven-dtype oracle results are recorded in
[NUM-09 Whole execution](../../docs/built-in_ops/01-numeric/layouts-whole.md).
The installed CTest `installed_numeric_layouts_result` passes 1/1 against
`Photospider::kernel`. There is no current x86 execution, native GPU execution
or performance claim.

## Indexing and scatter: NUM-10

`photospider_numeric_indexing` exercises the eighteen concatenate, gather and
scatter profile keys through the public helpers in
`photospider/numeric/indexing.hpp`. The fixture builds its `WorkflowDocument`
and Result bindings through the public C++ API. Its local `Value` arrays supply
typed backing only; source tensors are published as Results under the execution
Root. Each input Result contributes one tensor member under any schema/member
key. The complete `sample_shape()` includes batch axes, has rank 1..8 and at
most 2^40 elements. Inputs use UInt8, Int64, Float32 or Float64; gather/scatter
indices are Int64.

```sh
cmake --build build/kernel-dev --target photospider_numeric_indexing -j8
ctest --test-dir build/kernel-dev -R '^test_numeric_indexing_result$' --output-on-failure
build/kernel-dev/examples/numeric_workflow/photospider_numeric_indexing strict
python3 oracle/ops/numeric/index_oracle.py build/kernel-dev/examples/numeric_workflow/photospider_numeric_indexing strict
build/kernel-dev/examples/numeric_workflow/photospider_numeric_indexing apple
python3 oracle/ops/numeric/index_oracle.py build/kernel-dev/examples/numeric_workflow/photospider_numeric_indexing apple
```

All operations use CPU Whole execution with Data, Validation and Descriptor
support (role 13) for active inputs. Static metadata is checked during
specialization; a nonempty Tensor Need triggers typed-payload validation. The
program reads through authorized windows and publishes a complete
`photospider.tensor`/`samples` Result whose full output shape uses ordinary axes;
facets and batch metadata are dropped. A downstream query `Q` limits observed
dependencies and consumer reads, not preparation or publication. Empty output
has empty coverage and support and performs no sample arithmetic. Upstream,
typed, resource and cancellation failures remain visible even when they concern
input or output coordinates outside `Q`.

Concatenate supports 2..256 ordered inputs with matching dtype, rank and
non-axis extents. `layout="view"` proves one affine mapping across complete
inputs and can join compatible fragments sharing a physical owner; an
incompatible or independent owner returns `ViewUnavailable`. `layout="dense"`
materializes the complete packed output. Concatenate has no Auto mode and is not
content-cacheable because content does not identify physical viewability.
Gather preserves the order of repeated Int64 indices. Scatter replacement
selects the last matching update; sum, minimum and maximum combine the base with
matching updates in input order. Sum uses exact accumulation and a single final
conversion/range check. Copies preserve raw bits, including signaling NaNs.

The public workflow checks concatenate `[[1,2,5],[3,4,6]]`, gather
`[[12,10,12],[22,20,22]]`, and scatter replace/sum/min/max
`[10,3,4]` / `[10,25,34]` / `[10,2,4]` / `[10,23,34]`. It also covers
same-owner View addresses and associations after context teardown, independent
owners, unselected input failures, repeated and invalid indices, exact integer
cancellation and overflow, typed RGB validation, Empty requests, caller
floating-environment preservation, strided inputs, raw signaling-NaN copy and
quieting behavior, completed-result cache association with current sources,
full output coverage versus Q-scoped dependency support/dirty mapping,
work/capacity failure and resource release.
Repeated requests on one Frozen workflow reuse the same Result identity; a fresh
binding and Frozen workflow with equal input content checks completed-result
reuse and current-source association separately.

The installed consumer builds the same source against the installed SDK:

```sh
cmake --install build/kernel-dev --prefix "$PWD/build/kernel-dev/indexing-result-install"
cmake -S tests/consumer -B build/kernel-dev/indexing-result-consumer -DCMAKE_PREFIX_PATH="$PWD/build/kernel-dev/indexing-result-install" -DPhotospider_DIR="$PWD/build/kernel-dev/indexing-result-install/lib/cmake/Photospider" -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build/kernel-dev/indexing-result-consumer --target photospider_numeric_indexing_consumer -j8
ctest --test-dir build/kernel-dev/indexing-result-consumer -R '^installed_numeric_indexing_result$' --output-on-failure
```

The `strict`, `apple` and `x86` arguments select registered CPU profiles;
accelerated profiles require a compatible host. The independent coordinate,
contributor and Fraction oracle is in
[`index_oracle.py`](../../oracle/ops/numeric/index_oracle.py). It independently
checks output bits across generated cases, including exceptional-value and
signed-zero ordering. The focused
`test_numeric_indexing_result` CTest passes 1/1, the strict and Apple public
workflows exit successfully, and the oracle passes 3,858 cases per profile. The
installed consumer `installed_numeric_indexing_result` passes 1/1 against the
installed `Photospider::kernel` package. The existing `test_numeric_result_math`
integration fixture contains additional indexing coverage; it was not rerun for
this Result migration. No current x86, native GPU or performance result is
claimed. See [NUM-10 Whole execution](../../docs/built-in_ops/01-numeric/indexing-whole.md)
for resource and evidence boundaries.

## Reductions: NUM-11

The 21 formal sum/minimum/maximum/mean/count/variance/std keys use Whole. Each
input Result has one tensor member under any key; its complete `sample_shape()`
includes batch axes, with rank 1..8 and at most 2^40 elements. Outputs use
`photospider.tensor` / `samples`, preserve rank, set reduced axes to one and
drop facets and batch topology. The six numerical reducers request role 13 for
nonempty input, validate the typed payload, and read authorized Result windows
directly. They compute every group and publish the complete dense output before
projection, so an unrequested overflow or typed error fails the Run. They do
not first pack the full input. Empty demand performs no sample arithmetic.

Count uses the statically validated schema and axes only. Its empty runtime
input projection creates no source observation or association and does not
start an upstream sample producer. It computes reduced extents in O(rank) and
publishes the complete keepdims output over one 8-byte Int64 zero-stride owner.
For example, reducing axis 1 of a 2^40-element source yields shape
`[2^20,1]`, with `2^20` in both ends of the output.

```sh
cmake --build build/kernel-dev --target photospider_numeric_reductions -j8
build/kernel-dev/examples/numeric_workflow/photospider_numeric_reductions strict
python3 oracle/ops/numeric/reduction_oracle.py build/kernel-dev/examples/numeric_workflow/photospider_numeric_reductions strict
python3 oracle/ops/numeric/reduction_oracle.py build/kernel-dev/examples/numeric_workflow/photospider_numeric_reductions apple
ctest --test-dir build/kernel-dev -R '^test_numeric_reductions_result$' --output-on-failure
```

The `[1,2,3],[4,5,6]` example reduces axis 1 to sums `[6,15]`, minima `[1,4]`,
maxima `[3,6]`, means `[2,5]`, counts `[3,3]`, variances `[2/3,2/3]` and
standard deviations `[sqrt(2/3),sqrt(2/3)]`, with destination rounding.
`reduction_oracle.py` checks 4,740 independent cases per strict and available
Apple Silicon profile. The current root CTest passes 1/1, and both available
default workflow profiles exit successfully. The workflow checks all seven
reducers under four rounding modes on the actual worker and caller, plus
unaligned, negative- and zero-stride inputs, typed validation, invalid `ddof`,
Empty, unrequested-group overflow, work limits, cancellation, and a generated
4,096-sample Result source. The Root Payload peak includes that source and is
asserted between 32 KiB and the configured 128 KiB cap; after context retirement,
the retained dense reduction output accounts for 8 bytes, and the last release
returns all Root resources to zero. A two-poll source failure after publishing
a certified 128-sample prefix preserves the upstream error and performs no
numeric computation. The `2^40` count case leaves its failing source factory
unstarted, records no source observations or association, and uses an 8-byte
zero-stride output whose escaped read window survives context retirement.

The cache checks separate same-Frozen object identity sharing from completed
reuse with a fresh equal-content Result and Frozen workflow. Reuse carries the
current source ObjectId. Replacing an unrequested group invalidates the complete
output; the first group remains 3 while the changed second group becomes 11.
The original full result remains readable after context retirement, and its last
owner releases the Root payload.

The reducer reports one local `NumericDiagnostics` record at callback completion.
`evaluated_values` counts authorized source words supplied to the exact
accumulator, not output groups; `reduce_count` has a metadata-only identity and
zero evaluated values. The implementation identity includes the selected
profile and host/build identity. Its ISA suffix identifies the four-word output
selection helper, not SIMD arithmetic across the reduction algorithm. Strict
and Apple checks report zero strict-math calls, strict fallbacks and fallback
reasons. WorkLimit or cancellation may prevent diagnostics from merging, and
window acquisition/read `Status` or outer `std::bad_alloc` handling can bypass
the local final-report attempt.

The focused CTest is `test_numeric_reductions_result`. The installed consumer
compiles the same example against `Photospider::kernel`; configure and run it
with:

```sh
cmake --install build/kernel-dev --prefix build/kernel-dev/result-repeat-install
cmake -S tests/consumer -B build/kernel-dev/repeated-result-consumer \
  -DCMAKE_PREFIX_PATH="$PWD/build/kernel-dev/result-repeat-install" \
  -DPhotospider_DIR="$PWD/build/kernel-dev/result-repeat-install/lib/cmake/Photospider" \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build/kernel-dev/repeated-result-consumer --target photospider_numeric_reductions_consumer -j8
ctest --test-dir build/kernel-dev/repeated-result-consumer -R '^installed_numeric_reductions_result$' --output-on-failure
```

The final focused root selection completed 40 tests in 19.79 seconds: 39
passed and one skipped, with zero failures. It included
`test_numeric_reductions_result` and the shared `test_numeric_result_math`
fixture. Root resource tests passed 3/3, and the installed numeric selection
passed 10/10. The independent reduction oracle passes 4,740 cases per Strict
and Apple profile, and both default manual profiles exit successfully.
Sibling-source 4 MiB and 5 MiB Root Payload peak assertions passed in the
complete `test_dependency_program` run. The skipped test was
`test_vulkan_gpu`, with return code 77. x86 execution, Vulkan execution and new
performance measurements were not run. The installed consumer compile
uses only the installed prefix's `include` directory plus
`-fno-fast-math -frounding-math -ffp-contract=off`, and links the prefix's
`lib/libphotospider.a`.

See [NUM-11 Whole execution](../../docs/built-in_ops/01-numeric/reductions-whole.md)
for resource accounting and evidence limits. No x86 execution, GPU support,
maximum-shape throughput or RSS bound is claimed here.

## Ordered Result reductions and scan

`ordered.cpp` constructs one Float64 tensor Result containing `[1,2,3,4,5,6]`
and runs `numeric.mean`, `numeric.variance`, and `numeric.ordered_scan` in the
same `ExecutionContext` execution. Each node's original output port is named
`value`; all three outputs are Results with the `photospider.tensor` v1 schema
and `samples` member. The example checks mean `3.5`, population variance `35/12`,
and inclusive prefixes `[1,3,6,10,15,21]`.

The target is excluded from the default build and is a manual executable with no
CTest registration:

```sh
cmake --build build/kernel-dev --target photospider_numeric_ordered -j8
build/kernel-dev/examples/numeric_workflow/photospider_numeric_ordered
```

## Ordering and quantile: NUM-12

The six formal sort and quantile keys use Whole. Each source input is a Result
with one tensor member under any key, using UInt8, Int64, Float32 or Float64 and
a `sample_shape()` of rank 1..8 with at most 2^40 elements. A selected sort
output owns a complete `values` or `indices` Result; the other output is not
allocated. Both outputs retain the complete source shape, convert batch axes
to ordinary axes, and have no facets. Sort requests full source support with
Data, Validation and Descriptor roles and reads through authorized Result
windows. Quantile uses a separate Float32/64 `[1]` Result for `q`; for axis
length at least two it requests full source and q support, while axis length
one statically validates q's schema but has no runtime q Need or observation.
For longer axes, source failures can precede q validation.

```sh
cmake --build build/kernel-dev --target photospider_numeric_ordering -j8
build/kernel-dev/examples/numeric_workflow/photospider_numeric_ordering strict
python3 oracle/ops/numeric/ordering_oracle.py build/kernel-dev/examples/numeric_workflow/photospider_numeric_ordering strict
python3 oracle/ops/numeric/ordering_oracle.py build/kernel-dev/examples/numeric_workflow/photospider_numeric_ordering apple
ctest --test-dir build/kernel-dev -R '^test_numeric_ordering_result$' --output-on-failure
```

The executable accepts `strict`, `apple` and `x86`; the accelerated profiles
require a compatible host and otherwise return `BackendUnavailable`.

For `[3,1,1,2]`, stable sorting yields values `[1,1,2,3]` and original indices
`[1,2,3,0]`; quantile of `[0,10,20,30]` at `q=.25` is `7.5`. The current
workflow also checks sparse requests against complete Whole publication,
stable ordering across non-last-axis and disjoint lines, q projection and
failure precedence, typed input validation, source/q replacement and Result
lifetime. It runs a separate public dependency-v2 block probe: a pure staged
two-output operation enables `share_blocks_across_outputs`, and its block state
is reusable across outputs while each output retains its own Need roles,
association and dependency evidence. The formal sort and quantile operators are
Whole and do not use staged block sharing. The installed consumer uses this
same source with the installed `Photospider::kernel` package:

```sh
cmake --install build/kernel-dev --prefix build/kernel-dev/result-repeat-install
cmake -S tests/consumer -B build/kernel-dev/repeated-result-consumer -DCMAKE_PREFIX_PATH="$PWD/build/kernel-dev/result-repeat-install" -DPhotospider_DIR="$PWD/build/kernel-dev/result-repeat-install/lib/cmake/Photospider" -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build/kernel-dev/repeated-result-consumer --target photospider_numeric_ordering_consumer -j8
ctest --test-dir build/kernel-dev/repeated-result-consumer -R '^installed_numeric_ordering_result$' --output-on-failure
```

The current Root workflow test and installed consumer each pass 1/1. The
ordering oracle passes 2,072 cases in both Strict and Apple profiles. The
separate C block fixture also passes in the Root and installed consumer tests;
its exact coverage is described in [Global Results](../../docs/kernel-architecture/Global-Results.md).

See [NUM-12 Whole execution](../../docs/built-in_ops/01-numeric/ordering-whole.md)
for current behavior boundaries and validation limits.

## Exact comparisons and select: NUM-07

`photospider_numeric_comparisons` exercises the public `WorkflowDocument`,
`Compiler`, `ExecutionContext`, Result bindings and `execute_fragments` path for
the six predicates, `is_close` and `select`. The fixture's `Value` objects are
typed backing used to create source Results; the workflow binds and reads only
Results. Build and run the strict profile with:

```sh
cmake --build build/kernel-dev --target photospider_numeric_comparisons -j 8
build/kernel-dev/examples/numeric_workflow/photospider_numeric_comparisons _strict
ctest --test-dir build/kernel-dev -R '^test_numeric_comparisons_result$' --output-on-failure
python3 oracle/ops/numeric/comparison_oracle.py \
  build/kernel-dev/examples/numeric_workflow/photospider_numeric_comparisons _strict
```

Use `_accelerated_apple_silicon` on Apple Silicon or
`_accelerated_x86_64` on x86-64. An unsupported host reports
`BackendUnavailable`. The example expects
`NUM-07: six predicates; select=[10,2,30] ... passed`. All 24 formal keys
use Whole Result execution. Each input is a Result with one tensor in slot 0;
the full `sample_shape()` must match across inputs, including batch axes. A
nonempty Whole poll requests each active input with Data, Validation and
Descriptor roles (13), then reads through its authorized Result window. It
publishes a complete `photospider.tensor` v1 / `samples` Result with ordinary
axes and no input facets or batch topology. A sparse consumer request still
receives the complete Whole Result; its footprint scopes observed dependency
roots, and an edit to either comparison input can dirty every observed output.

The six predicates accept UInt8, Int64, Float32 and Float64; `is_close` accepts
Float32 and Float64 and uses exact binary-rational threshold arithmetic.
Predicates return UInt8 0/1. `select` accepts a UInt8 condition plus two
same-shape, same-dtype branches, eagerly validates both branches, and copies
selected bits without quieting signaling NaNs or changing signed zero. An
invalid condition byte fails the Whole invocation with Run scope after required
source and typed validation. Empty requests still seal a zero-coverage Result
and descriptor witness, but issue no tensor Need, start no computed source, and
perform no sample arithmetic. Static schema and tolerance checks still apply.

The example checks `select=[10,2,30]`, the `MAX/-MAX` `is_close` result of 0
without floating overflow, full source support and dirty mapping, and binding
replacement. A fresh Result binding with a fresh freeze exercises the completed
content cache; repeating the same frozen demand reuses the same Result
observation. Editing an unselected branch invalidates the Whole cache, and
changing the condition selects the replacement value `99`. Other checks cover
cancellation/resource cleanup, worker and caller floating-environment
restoration, 65-lane tails and unaligned, shifted, singleton, reversed and
zero-stride layouts. A typed RGBA Result image input with alpha `1` succeeds;
alpha `2` is rejected on either input port.
`comparison_oracle.py` compares raw IEEE relations and tolerance arithmetic
against an independent Fraction oracle. The root CTest entry is
`test_numeric_comparisons_result`; the installed consumer registers
`installed_numeric_comparisons_result` from the same example source against
`Photospider::kernel`. Current Result evidence: strict and Apple manual runs
exit 0, root and installed CTests each pass 1/1, and the oracle passes 3,760
cases per profile.
See [NUM-07 implementation and historical measurements](../../docs/built-in_ops/01-numeric/comparison-whole.md).

## Prefix sums and integral image: NUM-13

The six formal scan profiles use Whole Result execution. Each input is a Result
with one tensor member under any key; `sample_shape()` includes batch axes. The
`values` output is a `photospider.tensor` v1 Result with member `samples`, full
global shape, ordinary output axes and no facets. `prefix_sum([1,2,3])` produces
`[0,1,3,6]`; `integral_image([[1,2],[3,4]])` produces
`[[0,0,0],[0,1,3],[0,4,10]]`. Exact carry survives floating output
overflow/cancellation, and rounded outputs never become arithmetic state.

For each nonempty request, the Whole program requests complete source support
with Data, Validation and Descriptor roles (13), reads through authorized
Result windows, computes the complete output and publishes full coverage in
global coordinates. The requested footprint scopes observed dependency roots;
it does not limit reads from the returned Result's published coverage or turn
that Result into a packed ROI. The kernel does not pack the complete input into
a second payload. Empty output has zero coverage, performs no sample arithmetic
and does not start a computed input producer. Integral image treats each
unselected axis as an independent plane; the two selected axes may be
nonadjacent.

```sh
cmake --build build/kernel-dev --target photospider_numeric_scans -j8
build/kernel-dev/examples/numeric_workflow/photospider_numeric_scans strict
python3 oracle/ops/numeric/scan_oracle.py build/kernel-dev/examples/numeric_workflow/photospider_numeric_scans strict
python3 oracle/ops/numeric/scan_oracle.py build/kernel-dev/examples/numeric_workflow/photospider_numeric_scans apple
ctest --test-dir build/kernel-dev -R '^test_numeric_scans_result$' --output-on-failure
```

The executable accepts `strict`, `apple` and `x86`; accelerated profiles require
a compatible host. Integer overflow at any complete output coordinate fails the
Run, even outside the requested projection. The current behavior test checks
exact carries, NaN priority, zero-boundary validation, dirty support, Result
lifetime, resource admission and cancellation. A generated source publishes
4,096 Int64 samples; the scan adds them once, checks selected outputs and
certifies all 4,097 boundary values. Its measured Root Payload peak must stay
between 65,544 and 131,072 bytes. A retained output and then its read window
remain usable after context teardown, and releasing the final owner returns Root
resources to zero. Cache checks distinguish same-frozen identity sharing from
completed-result reuse on fresh bindings; source edits invalidate the observed
Whole output. A stable-prefix source confirms that a zero-boundary result still
requires its producer's tail: it publishes a NaN-containing prefix, then reports
the original tail error before scan arithmetic begins. See [NUM-13 Whole
execution](../../docs/built-in_ops/01-numeric/scans-whole.md) for implementation
and validation boundaries. The overflow case requests the representable
integral-image value at `[2,2]`, but fails `Domain/Run` because the complete
output overflows at the unrequested coordinate `[1,2]`; the failure has no Atom
key, while its diagnostic identifies global output coordinate `[1,2]`; Root
resources return to zero.

The installed consumer compiles the same source against the installed
`Photospider::kernel` package:

```sh
cmake --install build/kernel-dev --prefix build/kernel-dev/result-repeat-install
cmake -S tests/consumer -B build/kernel-dev/repeated-result-consumer -DCMAKE_PREFIX_PATH="$PWD/build/kernel-dev/result-repeat-install" -DPhotospider_DIR="$PWD/build/kernel-dev/result-repeat-install/lib/cmake/Photospider" -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build/kernel-dev/repeated-result-consumer --target photospider_numeric_scans_consumer -j8
ctest --test-dir build/kernel-dev/repeated-result-consumer -R '^installed_numeric_scans_result$' --output-on-failure
```

The scan reports one local `NumericDiagnostics` record at callback completion.
`evaluated_values` counts authorized source words supplied to the exact
accumulator, not output positions. Prefix zero boundaries, carry snapshots and
final output conversions do not increment it. The implementation identity
includes the selected profile and host/build identity; its ISA suffix describes
the four-word output-selection helper, not SIMD execution of the scan
algorithm. Strict and Apple checks report zero strict-math calls, strict
fallbacks and fallback reasons. WorkLimit or cancellation may prevent the
report from merging, while window acquisition/read `Status` and outer
`std::bad_alloc` handling can bypass the local report attempt. Cache hits add no
new attempts; Empty demand performs no sample arithmetic.

The final focused root selection completed 40 tests in 19.79 seconds: 39
passed and one skipped, with zero failures. It included
`test_numeric_scans_result` and `test_numeric_result_math`. Both Strict and
Apple default workflows exit successfully, and the independent scan oracle
passes 2,544 cases per profile. The installed numeric selection passed 10/10.
Root resource tests passed 3/3. Sibling-source 4 MiB and 5 MiB Root Payload
peak assertions passed in the complete `test_dependency_program` run. The
skipped test was `test_vulkan_gpu`, with return code 77. x86 execution, Vulkan
execution and new performance measurements were not run.
This is a focused consumer selection, not a full package matrix.

## Exact affine matrix transforms: NUM-14

The `matrix_transform_node` helper in `photospider/numeric/matrix.hpp` authors
three Result inputs in port order: `vectors`, `matrix`, `bias`. Each input Result
has one tensor member under any member key. Their `sample_shape()` values are
`[...,Cin]`, `[Cout,Cin]`, and `[Cout]`; dtypes match and are Float32 or Float64.
The output port key is `values`, with schema `photospider.tensor` and tensor
member `samples`. Its shape is `[...,Cout]` with no facets or batch topology.
Example `Value` objects are private typed backing used to construct source
Results; workflow bindings, execution, and output reads use Result. The
operation computes the complete exact dot product plus bias and rounds once.
Singular matrices are valid.

Every nonempty Whole request requires Data, Validation, and Descriptor (role 13)
for all three inputs, processes vectors in blocks of at most 64, and publishes
the complete packed output before projection. Empty demand reads no payload and
performs no matrix arithmetic. Any active input change invalidates the observed
output. The exact accumulator and Float32 candidate certificate, including the
SCALAR, Accelerate, and SME candidate algorithms, are specified in
[NUM-14](../../docs/built-in_ops/01-numeric/op_specs/NUM-14_matrix_transform.md).

The current focused Result math coverage is run from the existing macOS
`build/kernel-dev` tree, configured with Accelerate enabled and SME disabled:

```sh
cmake --build build/kernel-dev --target test_numeric_result_math -j8
ctest --test-dir build/kernel-dev -R '^test_numeric_result_math$' --output-on-failure
```

The `matrix_*` integration cases in `test_numeric_result_math` cover all
Cin/Cout combinations from 2 through 4 over 130 vectors, negative strides,
batch axes, block tails, Float64 rank-one vectors, special values, complete
typed input validation, Empty demand, work/resource limits, and pre-cancellation.
The recorded focused CTest run passed 1/1. It does not run the manual executable's
additional preparation, association, owner-lifetime, or checked-worker fixtures.
This focused test does not establish that Accelerate or SME hardware instructions
ran individually.

The separate `photospider_numeric_matrix` manual executable is also registered
as `test_numeric_matrix_result`. Its default path checks exact callback work
thresholds, caller and worker floating environments, required source failure
despite a vector NaN, compiled preparation reuse after a dynamic matrix edit,
warm content-cache reuse with refreshed associations for all three fresh source
Results, and independent source/output/window ownership through final Root
release. Replacing the matrix in an open demand produces `[4,9]`; an escaped
Result and read window keep one 16-byte Float64 output payload alive after the
source and context retire, then release it when the final window is released.

The executable and Fraction oracle use the same Result workflow:

```sh
cmake --build build/kernel-dev --target photospider_numeric_matrix -j8
build/kernel-dev/examples/numeric_workflow/photospider_numeric_matrix strict
python3 oracle/ops/numeric/matrix_oracle.py \
  build/kernel-dev/examples/numeric_workflow/photospider_numeric_matrix strict
python3 oracle/ops/numeric/matrix_oracle.py \
  build/kernel-dev/examples/numeric_workflow/photospider_numeric_matrix apple
```

The independent oracle passed 1,598 exact Fraction cases in Strict and 1,598 in
Apple. The installed-package consumer passed `installed_numeric_matrix_result`
1/1 under Strict, and its Apple profile completed the full manual checks
successfully:

```sh
ctest --test-dir build/kernel-dev/repeated-result-consumer \
  -R '^installed_numeric_matrix_result$' --output-on-failure
build/kernel-dev/repeated-result-consumer/photospider_numeric_matrix_consumer apple
```

The bounded CLI checks also passed bitwise comparison and reported the complete
output count with two polls for each workload:

```sh
build/kernel-dev/examples/numeric_workflow/photospider_numeric_matrix apple benchmark 4096 4 4 float32
build/kernel-dev/examples/numeric_workflow/photospider_numeric_matrix strict benchmark 65 4 3 float64 cancellation
build/kernel-dev/examples/numeric_workflow/photospider_numeric_matrix apple grid 8
```

These bounded checks validate the named workloads; they are not a performance
campaign. Benchmark elapsed time includes Result coordination, digest, and
polling work. Historical Value-path performance measurements are not current
Result performance data. The CMake example and installed consumer use
`-fno-fast-math -frounding-math -ffp-contract=off`. Neither these checks nor the
focused CTest establish that Accelerate or SME instructions executed on hardware.

## Discrete derivatives and cumulative integration: NUM-15

The six registered calculus profile keys run Whole Result programs through the
public helpers in `photospider/numeric/calculus.hpp`. The manual fixture keeps
`Value` only as source backing and binds each declared input as a Result whose
tensor publication references that storage. A sparse query still computes
and publishes the complete output with full tensor coverage. The query limits the
recorded dependency observation and dirty mapping; it does not trim Result
coverage. For `integrate_1d` with N=1, static specialization selects only input
2 (initial), so failed samples and step producers remain unstarted. Empty demand
starts no producer and publishes empty tensor coverage.

The fixture checks representative discrete results:
`derivative([0,1,4], step=1)=[1,2,3]` and
`integrate([0,1,2], step=1, initial=0)=[0,0.5,2]`. It also checks negative and
zero strides, unaligned storage, caller and worker floating environments,
rejection of incompatible rank-one RGBA schema facets, actual continuation work
limits and cancellation, and complete release of Root resources. After the
execution context and source backing are destroyed, an escaped Result and its
authorized read window share 24 Payload bytes; releasing both returns live Root
resources to zero.

Build and run the manual behavior test, then compare the independent 1,810-case
Fraction corpus. Strict is checked bit-for-bit; Apple uses the shared accelerated
FP32-scaled acceptance bound, so it is not a bit-exact claim.

```sh
cmake --build build/kernel-dev --target test_numeric_result_math -j8
ctest --test-dir build/kernel-dev \
  -R '^(test_numeric_calculus_result|test_numeric_result_math)$' --output-on-failure
build/kernel-dev/examples/numeric_workflow/photospider_numeric_calculus strict
python3 oracle/ops/numeric/calculus_oracle.py build/kernel-dev/examples/numeric_workflow/photospider_numeric_calculus strict
build/kernel-dev/examples/numeric_workflow/photospider_numeric_calculus apple
python3 oracle/ops/numeric/calculus_oracle.py build/kernel-dev/examples/numeric_workflow/photospider_numeric_calculus apple
```

The root focused CTest passed 2/2 (`test_numeric_calculus_result` and
`test_numeric_result_math`, 4.67 s). The manual executable completed all six
check groups under Strict and Apple. Each oracle profile passed all 1,810 cases;
Strict was bit-exact and Apple used the shared FP32-scaled bound. The installed
consumer built from package 0.32.0 and passed its Strict CTest 1/1 (0.07 s); its
Apple profile completed all six check groups. The installed consumer compiles
the same source against the imported `Photospider::kernel` package. See
[NUM-15 Whole execution](../../docs/built-in_ops/01-numeric/calculus-whole.md)
for the focused coverage and package-consumer commands.

## Unary mathematics and exact rational pi: NUM-04

`photospider/numeric/unary.hpp` provides independently named constructors for
`abs`, `neg`, `sqrt`, `exp`, `ln`, `sin`, `cos`, `tan`, `floor`, `ceil`, `round`,
`sign`, `reciprocal`, `sinpi`, `cospi`, `tanpi`, `sinc`, and `sincpi`. Append
`_node(id, input, profile)` to these names. Four additional
`sinpi_rational_node`, `cospi_rational_node`, `tanpi_rational_node` and
`sincpi_rational_node` helpers take two same-shape Int64 input references,
followed by output dtype (default Float64) and profile.

All 66 keys use Result input and output ports. Each input Result must contain
exactly one tensor member in slot 0, and may also carry fields. Its schema ID
and facets may vary, while its tensor dtype and sample shape must match the
other inputs. Recognized tensor facets and spatial metadata still undergo
full-input validation; allowing a numeric NaN does not bypass an invalid typed
sample such as an alpha value. The output port is named `values` and
publishes one `photospider.tensor` Result with tensor key `samples`, the same
sample shape, and empty facets. Ordinary unary operations preserve dtype;
rational helpers select Float32 or Float64 explicitly. Most basic transforms
support all four dtypes; neg excludes UInt8, while roots, reciprocals and
transcendentals require Float32/64. Integer range failures affect the complete
Whole invocation.

For nonempty Whole requests, the operation needs complete input support with
Data, Validation and Descriptor roles. The coordinator supplies authorized
Result tensor windows, so negative and zero strides do not require a packed
input copy. The operation computes a complete packed output before the executor
projects the requested coordinates. Empty Result requests retain static
validation and resource admission but skip input reads and computation, leaving
empty output coverage. Any changed input coordinate invalidates all observed
output coordinates.
Rational denominators must be positive at every logical coordinate,
including zero numerators. Both rational sources remain dependencies.

```sh
cmake --build build/kernel-dev --target photospider_numeric_unary -j8
build/kernel-dev/examples/numeric_workflow/photospider_numeric_unary strict
ctest --test-dir build/kernel-dev -R '^test_numeric_unary_result$' --output-on-failure
python3 oracle/ops/numeric/unary_oracle.py \
  build/kernel-dev/examples/numeric_workflow/photospider_numeric_unary strict
```

The independent oracle requires an MPFR 4.2+ shared library matching Python's
architecture, only for this manual check. Set `PHOTOSPIDER_ORACLE_MPFR` to select
it explicitly. On Apple Silicon, `/opt/homebrew/bin/python3.11` can be used with
the native Homebrew library. The kernel does not depend on MPFR; it uses bundled
SLEEF for accelerated arithmetic. The oracle prints the actual version it
loaded. The constant-generation check uses only Python's standard library:

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
512 Mi dependency work / 1024 Mi total execution work units. `CheckedProgram`
wraps the actual Result continuation and instruments `ResultProgramPhase::consume_work`
inside its CPU worker poll. A 1,024-unit callback-work limit produces WorkLimit
after computation begins; another case cancels when cumulative callback work
reaches 10,000 units. The wrapper excludes source creation, freeze and
coordinator work. It checks that all Root resources retire, but does not locate
cancellation within a particular transcendental refinement. The worker-poll
wrapper separately sets four rounding modes with `FE_DIVBYZERO` present and
checks that the original continuation restores that state. An outer loop also
checks the caller thread's rounding mode and exception flags across execution.
These are finite example budgets, not default or universal success guarantees.
Exact elementary state is small; transcendental state includes a fixed
12288-bit limb arena and uses directed precision from 128 through 4096
fractional bits. Unresolved rounding returns ResourceExhausted. Ordinary
accelerated transcendental values use SLEEF binary64 kernels and conservative
final-error checks in the documented ranges. SLEEF is bundled for kernel
acceleration; MPFR is used only by the independent oracle. Rejected candidates
use strict fallback; special/algebraic paths retain their exact rules.
See [the mathematical implementation notes](../../docs/built-in_ops/01-numeric/math-implementation.md).

The registered `test_numeric_unary_result` CTest runs the strict Result workflow.
The manual checks also cover every function's negative strides and four
caller-thread fenv modes/flags, full-input support and dirty projection,
typed-schema validation, Whole integer/denominator errors, upstream failure,
work/cancellation/capacity cleanup, retained Result lifetime, and warm-cache
changes to NaN sign/payload. Use `apple` or `x86` only on a host with that CPU
profile. Current strict and Apple C++ checks and the independent 7,524-case
integer/Fraction/MPFR oracle for each profile passed with MPFR 4.2.2. The
installed `Photospider::kernel` consumer passed `installed_numeric_unary_result`
(1/1) in `build/kernel-dev/repeated-result-consumer`.

```sh
ctest --test-dir build/kernel-dev/repeated-result-consumer \
  -R '^installed_numeric_unary_result$' --output-on-failure
```

Native local timing is available separately:

```sh
build/kernel-dev/examples/numeric_workflow/photospider_numeric_unary strict benchmark
```

The CSV reports all 22 functions at N=1 and N=256, Float64, Whole demand, one
worker, cache off, seven measured repetitions, median/max microseconds,
`peak_payload_bytes`, and an `N/A` fallbacks field. The timer covers each synchronous
`execute_fragments` call, including workflow execution and result assembly;
source construction, compilation, freezing, readback and verification are
outside it. The reported peak is the execution Root's cumulative Payload peak
observed across source setup, warmup and measured runs. It excludes source
backing accounted as Referenced and is not an RSS measurement. Every run checks
computed elements and output bits. Ordinary rational timing uses p/q=1/7. These
are public Result workflow timings, not callback medians or a comparison with
historical Value/core measurements.

## Binary mathematics: NUM-05

`photospider/numeric/binary.hpp` provides `add_node`, `subtract_node`,
`multiply_node`, `divide_node`, `minimum_node`, `maximum_node`, `pow_node`,
`atan2_node` and `atan2pi_node`. Each takes `(id, first, second, profile)`;
profile defaults to Strict. Angle arguments are ordered `(y,x)`, other arguments
`(a,b)`. Each Result input has exactly one tensor member in slot 0 and may also
carry fields; input schema IDs may differ. The input dtype and full sample shape
must match, with rank 1..8, positive extents and at most 2^40 samples. Recognized
tensor facets and spatial metadata still receive full-input typed validation;
numeric NaN handling does not override typed alpha validity. The `values`
output is one `photospider.tensor` Result with tensor key `samples`, the same
full sample shape as ordinary axes, and empty facets; input batch topology is
dropped. Divide, power and angles require Float32/64;
the other five accept UInt8/Int64/Float32/Float64. Use explicit broadcast and
cast operations for shape and dtype adaptation.

Nonempty Whole executions issue Data, Validation and Descriptor Needs for both
complete inputs. The coordinator supplies authorized tensor windows, so legal
negative and zero strides do not require a packed input copy. The operation
computes the complete packed Result before the executor projects the requested
coordinates. Empty queries may run a metadata-only poll but do not read samples
or perform arithmetic, and return empty tensor coverage. A changed coordinate
in either input invalidates all observed output coordinates; special identities
do not suppress the other input's obligation. Integer overflow anywhere fails
the Whole invocation with Run scope and no Atom key. IEEE nonfinite results
follow the operation-specific rules.

```sh
cmake --build build/kernel-dev --target photospider_numeric_binary -j8
build/kernel-dev/examples/numeric_workflow/photospider_numeric_binary strict
ctest --test-dir build/kernel-dev -R '^test_numeric_binary_result$' --output-on-failure
python3 oracle/ops/numeric/binary_oracle.py \
  build/kernel-dev/examples/numeric_workflow/photospider_numeric_binary strict
build/kernel-dev/examples/numeric_workflow/photospider_numeric_binary apple
python3 oracle/ops/numeric/binary_oracle.py \
  build/kernel-dev/examples/numeric_workflow/photospider_numeric_binary apple
ctest --test-dir build/kernel-dev/repeated-result-consumer \
  -R '^installed_numeric_binary_result$' --output-on-failure
```

The executable constructs its `WorkflowDocument` and Result bindings through the
public C++ API. Its `Value` arrays are local immutable typed backing; the fixture
publishes them as source Results under the execution Root, with the source
allocation charged as Referenced. The `cache_and_composition` fixture computes
`(a+b)*b` for `a=[1,2,3]`, `b=[2,2,2]`, yielding `[6,8,10]`.
`pow([2,-2,-2],[3,3,.5])` yields `[8,-8,canonical_NaN]`.
`atan2pi([+0,-0,1],[-0,-0,+0])` yields `[1,-1,.5]`; the radian-angle fixture
checks independently rounded pi bits. The MPFR 4.2+ oracle supports strict and
Apple runs; use `apple` or `x86` only on a host with that CPU profile.

The focused `test_numeric_binary_result` CTest runs the strict profile, and the
installed `Photospider::kernel` consumer runs the same example source as
`installed_numeric_binary_result`; both passed. Strict and Apple example runs
also passed, with 14,174 independent integer/Fraction/MPFR oracle cases for
each profile. Manual checks cover nine operations, exact fixtures, full-input
support and dirty mapping, Whole UInt8/Int64 overflow, typed validation on both
ports, required upstream producers, negative and zero-stride inputs, unaligned
storage, worker and caller floating environments, work/cancellation/capacity
cleanup, escaped Result lifetime, and NaN cache invalidation. The cache checks
distinguish same-Frozen ObjectId reuse from completed-result reuse with equal
content in a fresh Result and Frozen workflow. These paths provide CPU evidence
only.

The optional `benchmark` mode reports nine operations at N=1 and N=256 for
Float64 Whole execution with one worker, cache off, a warmup and seven measured
repetitions. It times `execute_fragments`; source construction, compile, freeze,
readback and output checks are outside the timer. `peak_payload_bytes` is the
execution Root's cumulative Payload peak, including source setup and warmup but
excluding input storage accounted as Referenced. It is not an RSS measure. Each
run checks the computed-element count and every output bit. This is Result
workflow timing, not a callback median or a comparison with historical
Value/core measurements. The earlier NUM-05 measurements in [math implementation](../../docs/built-in_ops/01-numeric/math-implementation.md#num-05-validation-and-native-timing)
are historical Value/callback evidence, not current Result performance results.

## Explicit-query curves: CRV-01

The public helpers `interpolate_linear_node`, `interpolate_pchip_node`, `interpolate_linear_multi_node` and `interpolate_pchip_multi_node` construct four independent explicit-query operations. Each input is a Result with one tensor member under an arbitrary schema id and member key. `sample_shape()` supplies x[K], y[K] or y[K,C], and query[N]; the three inputs independently accept Float32/64. Output port `values` is a Result with schema `photospider.tensor`, member `samples`, shape [N] or [N,C], and empty facets. Batch dimensions remain ordinary sample axes. A nonempty Whole request reads all three inputs with Data, Validation and Descriptor (role 13), computes the complete output and records the actual source ObjectIds in its association. Empty reads no payload.

The helper owns node metadata, while the compiler checks actual binding metadata; helpers may be used concurrently. Require K=2..65536 and positive logical products no greater than 2^40. Constructors default to Float64 and `reject`; direct WorkflowDocument nodes must set String parameters `dtype` and `out_of_domain` explicitly (`float32`/`float64`, and `reject`/`clamp`/`linear_extrapolate`). See the [CRV-01 family specification](../../docs/built-in_ops/01-numeric/op_specs/CRV-01_interpolate.md) for complete numerical and resource contracts. For example, connect bindings for x, y and query in this helper order:

```cpp
auto node = ps::numeric::interpolate_pchip_node(
    1, ps::WorkflowInputReference{1}, ps::WorkflowInputReference{2},
    ps::WorkflowInputReference{3}, ps::ElementType::Float64,
    ps::numeric::CurveDomain::Reject, ps::CpuNumericProfile::Strict);
```

For x=[0,1,2], y=[0,1,4], query=[.5,1.5,.5], PCHIP returns [.3125,2.1875,.3125]. Linear interpolation of x=[0,1,3], y=[0,2,4], query=[2,.5,2] returns [3,1,3]. `LinearExtrapolate` uses the linear endpoint secant or PCHIP endpoint tangent; `Clamp` returns the selected endpoint y. Strict and Float64 results use exact rational formulas with one final RN-even conversion. Accelerated Float32 uses conservative enclosures and exact fallback when the enclosure does not determine one rounded result.

Build and run the Result manual fixture under Strict and Apple, then run the
registered focused tests and independent exact Fraction oracles with:

```sh
cmake --build build/kernel-dev --target test_numeric_result_math test_result_image_contracts -j8
build/kernel-dev/examples/numeric_workflow/photospider_numeric_curves strict
build/kernel-dev/examples/numeric_workflow/photospider_numeric_curves apple
ctest --test-dir build/kernel-dev -R '^(test_numeric_curves_result|test_numeric_result_math|test_result_image_contracts)$' --output-on-failure
python3 oracle/ops/numeric/curve_oracle.py ./build/kernel-dev/examples/numeric_workflow/photospider_numeric_curves strict
python3 oracle/ops/numeric/curve_oracle.py ./build/kernel-dev/examples/numeric_workflow/photospider_numeric_curves apple
build/kernel-dev/examples/numeric_workflow/photospider_numeric_curves strict benchmark
build/kernel-dev/examples/numeric_workflow/photospider_numeric_curves apple benchmark
```

`test_numeric_curves_result` runs the manual Result fixtures in `curves.cpp` under Strict; the direct invocations above run the same six fixture groups under Strict and Apple. Both manual runs passed. Coverage includes Result declaration, binding and reads; sparse Whole full-input support, dirty mapping and errors; negative-stride/unaligned and zero-stride layouts across all ports with worker and caller floating-environment checks; Empty and static metadata; computation work, cancellation, output and scratch limits; fresh-source content-cache reuse with refreshed associations; same-demand retention; query/topology rebinding with static preparation reuse; upstream errors; giant sparse-demand rejection as `CapacityLimit` at node 1; typed Mask validation (`input_id=2`); and source-owner retirement with retained Result/read-window access, 16 live Payload bytes and final Root release. The focused CTest run passed 3/3 in 5.09 seconds. The independent exact Fraction oracle passed 2,487 bit-exact cases under each profile; its corpus covers all four operation forms, mixed input and destination dtypes, all three domain policies, extreme values and the monotonicity regression. The installed package 0.32 consumer was recompiled and linked against `build/kernel-dev/result-only-install`; `installed_numeric_curves_result` passed 1/1 under Strict in 0.36 seconds, and the consumer's direct Apple run passed all six fixture groups.

Build and run the installed consumer against an installed package with:

```sh
cmake --install build/kernel-dev --prefix build/kernel-dev/result-only-install
cmake -S tests/consumer -B build/kernel-dev/repeated-result-consumer -DCMAKE_PREFIX_PATH="$PWD/build/kernel-dev/result-only-install" -DPhotospider_DIR="$PWD/build/kernel-dev/result-only-install/lib/cmake/Photospider"
cmake --build build/kernel-dev/repeated-result-consumer --target photospider_numeric_curves_consumer -j8
ctest --test-dir build/kernel-dev/repeated-result-consumer -R '^installed_numeric_curves_result$' --output-on-failure
build/kernel-dev/repeated-result-consumer/photospider_numeric_curves_consumer apple
```

The optional `benchmark` mode reports eight rows per profile: N=1 and N=64 for four operations with K=17, C=1/2, Float64, one CPU worker, cache disabled, dependency-cache proof disabled, and three repetitions. Its timer surrounds `execute_fragments`, identified by `timing_scope=result_execute_fragments`: it includes coordinator discovery, continuation polls, computation, publication and digest work, while excluding source construction, compile, freeze, readback and output checks. Each row reported two continuation polls and N*C computed elements. The CSV `peak_payload_bytes` field records the managed Root Payload peak from `diagnostics.managed_resources`; the fixture checks that the peak covers at least the complete output bytes (8*N*C). The Strict and Apple bounded smoke runs both passed all eight rows. Their Root Payload peaks, equal across profiles, were:

| N | C | Root Payload peak (bytes) |
| ---: | ---: | ---: |
| 1 | 1 | 288536 |
| 64 | 1 | 289040 |
| 1 | 2 | 288544 |
| 64 | 2 | 289552 |

These are correctness and timing-scope smoke results; they do not establish a performance improvement or a platform matrix. They are not comparable to historical Value/callback measurements described in [math implementation](../../docs/built-in_ops/01-numeric/math-implementation.md#crv-01-exact-interpolation).

## Bezier function sampling: CRV-02

`sample_bezier_function_node` authors a quadratic or cubic single-valued Bezier function. Each input is a Result with one tensor member under any schema id/version/key; its complete `sample_shape()` is anchors[K,2], handles[K-1,degree-1,2], start[1], or end[1]. Input dtypes independently accept Float32/64. Require K=2..65536, degree 2 or 3, and count 1..1048576.

The `values` output is a Result using schema `photospider.tensor` v1/member `samples`, shape [count], selected Float32/64 dtype (default Float64), empty facets. The `axis` output uses the same schema/member, Float64 shape [3], `atomic_trailing_axes=1`, and empty facets. Both outputs record their actual active source ObjectIds. Constructor defaults are Float64 and Reject; direct WorkflowDocument nodes must provide `degree`, `count`, `dtype` and `out_of_domain` explicitly. For count=1, specialization excludes `end`: values reads anchors, handles and start; axis reads only start.

```cpp
auto node = ps::numeric::sample_bezier_function_node(
    1, ps::WorkflowInputReference{1}, ps::WorkflowInputReference{2},
    ps::WorkflowInputReference{3}, ps::WorkflowInputReference{4}, 3, 9);
```

For anchors=[[0,0],[1,1]], handles=[[[0,.25],[-1,-.25]]], start=[0], end=[1], the requested values at indices {0,1,8} are {0:0,1:.5,8:1}; axis is [0,1,.125]. At x=.125 the curve parameter is t=.5: the implementation solves Bx(t)=x, then evaluates By(t).

The Whole Result continuation requests active inputs with Data, Validation and Descriptor roles (role 13). For `values` it validates the complete x topology and generated coordinates before y arithmetic, computes every sample, and publishes one dense Result with full certified coverage in global sample coordinates, even for a sparse request. Empty demand reads no input payload and executes no numeric kernel; the runtime can still poll metadata and seal the Result. The `axis` output reads only its active endpoint inputs and does not read controls. Each output uses its own all-or-nothing Result transaction. Query and domain errors precede y arithmetic; generic y data outside the selected mathematical stencil is not scanned, while recognized typed validation still covers each active input. Published output owners and authorized read windows keep the output payload alive past context teardown; source-owner weak-reference checks confirm all source owners can retire while retained outputs remain readable.

The manual fixture in `bezier.cpp` uses Results for declarations, bindings, outputs and authorized reads. Value objects provide only immutable source backing. Its six groups exercise Result reads, errors, layouts, resource limits, cancellation, cache association and owner lifetimes. Both Strict and Apple runs passed all six groups. The independent Fraction oracle passed 382 numerical/error cases and four full-output capacity-rejection cases under each profile. Build and run the focused root test and oracle with:

```sh
cmake --build build/kernel-dev --target photospider_numeric_bezier -j8
ctest --test-dir build/kernel-dev -R '^test_numeric_bezier_result$' --output-on-failure
build/kernel-dev/examples/numeric_workflow/photospider_numeric_bezier strict
build/kernel-dev/examples/numeric_workflow/photospider_numeric_bezier apple
python3 oracle/ops/numeric/bezier_oracle.py ./build/kernel-dev/examples/numeric_workflow/photospider_numeric_bezier strict
python3 oracle/ops/numeric/bezier_oracle.py ./build/kernel-dev/examples/numeric_workflow/photospider_numeric_bezier apple
```

The fixture checks same-demand Result identity, two current cached outputs for fresh-source content-cache hits with current source associations, static `PreparedOperation` reuse after binding changes, full Whole output coverage, request-recorded dependency mapping, and separate `values` and `axis` Results. Count-one specialization excludes `end`; `axis` never reads controls. It checks that all source owners can retire while retained value and axis read windows remain readable, then releases the final owners and verifies all Root usage returns to zero. Negative, zero and unaligned input strides are exercised across all ports with worker and caller floating-environment checks. Work limits, cancellation, output and scratch capacity failures use the real continuation path. Typed validation rejects the invalid unrequested sample 1.5 in an otherwise valid Mask input before numeric arithmetic; the facet itself is valid. Unused generic y samples remain unscanned. The maximum count case verifies rejection of the complete dense output under a 1 MiB Payload budget; it is a capacity test, not a successful million-sample run. Full numerical formulas, topology checks and resource bounds are in the [CRV-02 specification](../../docs/built-in_ops/01-numeric/op_specs/CRV-02_sample_bezier_function.md).

The focused root tests `test_numeric_result_math` and `test_numeric_bezier_result` passed 2/2 in 4.99 seconds (4.39 seconds for the shared integration executable and 0.60 seconds for the manual fixture). The independent code and contract review found no unresolved blocker or required change.

The installed consumer compiles the same `bezier.cpp` fixture against the installed package. Reproduce it with:

```sh
cmake --install build/kernel-dev --prefix build/kernel-dev/result-only-install
cmake -S tests/consumer -B build/kernel-dev/repeated-result-consumer -DCMAKE_PREFIX_PATH="$PWD/build/kernel-dev/result-only-install" -DPhotospider_DIR="$PWD/build/kernel-dev/result-only-install/lib/cmake/Photospider"
cmake --build build/kernel-dev/repeated-result-consumer --target photospider_numeric_bezier_consumer -j8
ctest --test-dir build/kernel-dev/repeated-result-consumer -R '^installed_numeric_bezier_result$' --output-on-failure
build/kernel-dev/repeated-result-consumer/photospider_numeric_bezier_consumer apple
```

The package 0.32.0 consumer compiled and linked against `result-only-install`; `installed_numeric_bezier_result` passed 1/1 under Strict in 0.71 seconds, and the direct Apple invocation passed all six groups.

The separate `test_numeric_result_math` integration target continues to cover `bezier_workflows`, `bezier_boundaries`, `bezier_projection` and `bezier_resources`; its 22 golden words include six function outputs and sixteen CRV-03 parametric outputs. Those shared integration checks do not replace the manual `bezier.cpp` fixture or its installed consumer. The bounded benchmark times `execute_fragments`, with cache and dependency-cache proof disabled. Strict and Apple each passed 16 regular rows across degree 2/3, K=2/64, N=17/129 and Whole/ROI demand; all used Float64 and three repetitions. Each row reported two polls and N computed elements. Root Payload peaks were 523,368 bytes for N=17 and 524,264 bytes for N=129. Each profile also passed two stress rows, with two polls, one computed element and a 523,240-byte Root Payload peak. Timing scope is `result_execute_fragments`; it includes coordinator discovery, continuation polling, computation, publication and digest, and excludes compilation, source creation, freeze, readback and output checks. Whole execution does not expose per-value root-solver or fallback counters; those fields remain N/A. These are bounded behavior and timing-scope smoke checks and do not establish performance improvement. x86 and maximum physical K/N execution were not tested.

## Parametric Bezier evaluation: CRV-03

`parametric.cpp` exercises `evaluate_bezier_node` through the public Result
workflow API. The four declarations name schema-backed inputs; bindings publish
Results and the execution output is a Result read through its tensor read API.
Its local `Value` objects provide immutable source storage only; the source
Results refer to that storage and account it as Referenced. The output is
`photospider.tensor` v1 / member `samples`, shape [N,D], selected Float32/64
dtype and empty facets. Inputs are one-member Result tensors under any schema
id/version/key with complete `sample_shape()` values: anchors[K,D],
handles[K-1,degree-1,D], segment_indices[N] and t[N]. Anchors, handles and t
independently accept Float32/64; segment indices are Int64. Require K=2..65536,
D>=1, N>=1 and each input/output logical product <=2^40. D=1 retains its
component axis.

The output port `values` is a Result using schema `photospider.tensor` v1/member `samples`, shape [N,D], selected Float32/64 dtype (default Float64), and empty facets.

```cpp
auto node = ps::numeric::evaluate_bezier_node(
    1, ps::WorkflowInputReference{1}, ps::WorkflowInputReference{2},
    ps::WorkflowInputReference{3}, ps::WorkflowInputReference{4}, 2);
```

With anchors=[[0,0],[2,0]], handles=[[[1,2]]], segment_indices=[0,0,0], t=[0,.5,1], the result is [[0,0],[1,1],[2,0]]. A cubic example with anchors=[[0,0],[3,0]], handles=[[[1,3],[-1,3]]] at segment=0,t=.5 returns [1.5,2.25]. Handles are reconstructed relative to the start anchor for quadratic/outgoing controls and the end anchor for cubic incoming controls; each sum rounds to Float64 before polynomial evaluation.

The Whole Result program requests all four active inputs with Data, Validation
and Descriptor (role 13), validates every segment/t row before component
arithmetic, evaluates all N*D cells and publishes one dense Result with full
certified coverage in global coordinates. A sparse query changes dependency
and dirty mappings but does not reduce computation or output allocation. Empty
has empty output coverage and does not poll a failing handle producer; static
metadata and the other execution stages still apply. For nonempty work, a
failing handle producer propagates before endpoint shortcuts or invalid-query
arithmetic. Mathematical endpoints read only the selected anchor; interiors
read their segment's anchors and handles. Generic values outside evaluated
stencils are not numerically checked, while typed validation and upstream
failures cover the complete active Results. The fixture's RGBA handle Result
uses spatial layout and channel axis 2. Although its t=.5 query requests only
column 0, Whole execution computes all four columns and reads the alpha handle.
A separate t=0 case shows that endpoint mathematics skips handles while full
typed validation still rejects an invalid alpha value.
Output association records current source ObjectIds. Output Results own their
published payload after source storage and execution context retire; an
authorized read window shares the output owner and can outlive the Result.
See the [CRV-03 specification](../../docs/built-in_ops/01-numeric/op_specs/CRV-03_evaluate_bezier.md)
for polynomial, rounding, error and resource contracts.

The manual checks cover quadratic/cubic examples, the exact Bernstein result,
RN64 control reconstruction, signed zero and extreme cancellation; all 16
combinations of reversed, unaligned input layouts plus zero-stride sources;
caller and actual worker floating-environment preservation; nine malformed static Result schemas;
resource limits, cancellation, and release of all Root allocations. Cache
checks cover repeated demand identity, one content-cache hit with four current
source associations, and reuse of the prepared operation after segment/t
replacement. A sparse query returns full Whole coverage. Public constant-node
composition with 2^39 columns and with 2^40 rows confirms that full output
capacity is required and rejects at the parametric node with
ResourceExhausted/CapacityLimit; these tests verify rejection and do not run
successful physical maximum-shape numerical workloads. Typed RGBA handles use
the Result schema's spatial/channel metadata; invalid alpha is rejected during
Whole validation even for t=0.

Build and run the focused root behavior test and independent oracle with:

```sh
cmake --build build/kernel-dev --target test_numeric_result_math photospider_numeric_parametric -j8
ctest --test-dir build/kernel-dev \
  -R '^(test_numeric_result_math|test_numeric_parametric_result)$' --output-on-failure
build/kernel-dev/examples/numeric_workflow/photospider_numeric_parametric apple
PYTHONPYCACHEPREFIX=build/kernel-dev/python-pycache python3 oracle/ops/numeric/parametric_oracle.py build/kernel-dev/examples/numeric_workflow/photospider_numeric_parametric strict
PYTHONPYCACHEPREFIX=build/kernel-dev/python-pycache python3 oracle/ops/numeric/parametric_oracle.py build/kernel-dev/examples/numeric_workflow/photospider_numeric_parametric apple
```

Strict and Apple each passed all six manual groups and all 1,428 independent
Fraction Bernstein/RN64 oracle cases bit-for-bit. The focused root CTest selection passed
`test_numeric_result_math` and `test_numeric_parametric_result` 2/2 in 4.67
seconds (4.50 seconds and 0.17 seconds respectively). The root target also
builds the manual executable. The separate
`test_numeric_result_math` integration fixture continues to cover
`bezier_workflows`, `bezier_boundaries`, `bezier_projection` and
`bezier_resources`; its 22 Fraction golden words include six function and 16
parametric results. Those integration cases complement the new manual fixture
and do not replace it. The installed consumer compiles this same
`parametric.cpp` against the installed 0.32.0 package:

```sh
cmake --install build/kernel-dev \
  --prefix "$PWD/build/kernel-dev/result-only-install"
cmake -S tests/consumer -B build/kernel-dev/repeated-result-consumer \
  -DPhotospider_DIR="$PWD/build/kernel-dev/result-only-install/lib/cmake/Photospider"
cmake --build build/kernel-dev/repeated-result-consumer \
  --target photospider_numeric_parametric_consumer -j8
ctest --test-dir build/kernel-dev/repeated-result-consumer \
  -R '^installed_numeric_parametric_result$' --output-on-failure
build/kernel-dev/repeated-result-consumer/photospider_numeric_parametric_consumer apple
```

The installed 0.32.0 consumer compiled and linked against
`result-only-install`; `installed_numeric_parametric_result` passed 1/1 under
Strict in 0.17 seconds, and its direct Apple invocation passed all six groups.
The independent 1428-case Bernstein/RN64 oracle remains unchanged and passes
against the Result-backed manual executable under Strict and Apple. The earlier
package 0.18 Value-path performance measurements remain historical evidence
only; they do not measure the current Result implementation. x86 and successful
maximum physical K/N execution were not tested.

## LUT1D baking templates: CRV-04

`photospider/numeric/lut1d.hpp` provides six authoring functions: `bake_lut1d_expression`, `bake_lut1d_bezier`, `bake_lut1d_linear`, `bake_lut1d_pchip`, `bake_lut1d_linear_multi` and `bake_lut1d_pchip_multi`. Each appends one or two ordinary operation nodes to the caller-owned `WorkflowDocument` and returns `BakedLut1d` references to `values` and `axis`. The constructor authors graph metadata; the caller explicitly exports the references or connects them to later nodes, then compiles and executes the workflow through the public Result API.

For a document declaring Result inputs `start:[1]` and `end:[1]`, the minimal expression template is:

```cpp
auto baked = ps::numeric::bake_lut1d_expression(
    document, "x^2", ps::numeric::sequence_input(document.inputs[0]),
    ps::numeric::sequence_input(document.inputs[1]), 3);
if (!baked.ok()) throw std::runtime_error(baked.status().message);
auto exports = baked.value().outputs();
document.outputs.assign(exports.begin(), exports.end());
```

The endpoint declarations carry Result schemas; each `SequenceInput` copies its declaration's schema hint, and the compiler validates the actual bound Result. Requesting both outputs in this example yields `values=[0,.25,1]` and `axis=[0,1,.5]`. `Fixture::build` and `Fixture::run` in `baking.cpp` show the generated graph, equivalent hand-authored graph, Result bindings and execution for all six source templates.

The constructors reserve IDs already present in the document and IDs referenced by existing and supplied edges, then allocate the lowest available positive IDs. They enforce the 65,536-node limit and construct all nodes before mutating the document. A failed constructor leaves the graph unchanged. The caller chooses unique output labels with `outputs("table2", "grid2")`; declaration IDs occupy a separate namespace. Concurrent edits to one document require caller synchronization.

All templates require `count` in `[1,1048576]`; output dtype defaults to Float64 and profile defaults to Strict. Expression coefficients use a named input map. Bezier requires degree 2 or 3, anchor and relative-handle Results, and defaults to `BezierDomain::Reject`. The four interpolation forms take x/y Results and default to `CurveDomain::Reject`. They generate Float64 linspace query coordinates even when the table dtype is Float32. Scalar templates return `values[count]`; multi-function templates return `values[count,C]`, including `C=1`. Each exports an independent Float64 `axis[3]` Result containing start, end and step. For `count=1`, axis is `[start,start,0]` and the expanded source omits the end payload; larger counts include both endpoint samples.

| Template | Inputs in addition to endpoints | Endpoint/count fixture | Expected values |
| --- | --- | --- | --- |
| expression | `x^2` | 0 to 1, 3 | [0,.25,1] |
| Bezier | anchors=[[0,0],[1,1]], quadratic offsets=[[[.5,0]]] | 0 to 1, 3 | [0,.25,1] |
| linear | x=[0,1,2], y=[0,2,4] | 0 to 2, 3 | [0,2,4] |
| PCHIP | x=[0,1,2], y=[0,1,4] | 0 to 2, 5 | [0,.3125,1,2.1875,4] |
| linear multi | x=[0,1], y=[[0,10],[2,8]] | 0 to 1, 3 | [[0,10],[1,9],[2,8]] |
| PCHIP multi | x=[0,1,2], y=[[0,4],[1,3],[4,0]] | .5 to 1.5, 2 | [[.3125,3.6875],[2.1875,1.8125]] |

```sh
cmake --build build/kernel-dev --target photospider_numeric_baking -j 8
build/kernel-dev/examples/numeric_workflow/photospider_numeric_baking strict
build/kernel-dev/examples/numeric_workflow/photospider_numeric_baking apple
ctest --test-dir build/kernel-dev -R '^(test_numeric_baking_result|test_numeric_result_math)$' --output-on-failure
```

To compile and run the same example against the installed kernel package, configure the existing Unix Makefiles consumer tree with an explicit package location:

```sh
cmake -S tests/consumer -B build/kernel-dev/repeated-result-consumer \
  -DPhotospider_DIR="$PWD/build/kernel-dev/result-only-install/lib/cmake/Photospider" \
  -DCMAKE_PREFIX_PATH="$PWD/build/kernel-dev/result-only-install"
cmake --build build/kernel-dev/repeated-result-consumer --target photospider_numeric_baking_consumer -j 8
ctest --test-dir build/kernel-dev/repeated-result-consumer -R '^installed_numeric_baking_result$' --output-on-failure
```

The manual executable accepts `strict`, `apple` or `x86`; run accelerated profiles on a matching CPU. Its graph comparison covers six templates, two table dtypes and four demand modes (full values, axis-only, joint values and axis, and sparse values), or 48 generated/hand-authored graph pairs per selected profile. The fixture checks each emitted tensor against a separate analytic value set and compares graph output descriptors, Result bits, source dependencies and dirty mapping. Those graph fixtures establish template equivalence; independent numeric oracle cases for the underlying operations remain separate evidence.

`Fixture` stores source arrays as immutable `Value` backing and declares each workflow input with its Result schema. It publishes each backing storage as an immutable source Result and binds those Results to the compiled plan. `Fixture::run` returns `DemandResult.results`, and the fixture reads tensor samples through authorized Result reads.

All formal expanded outputs use CPU Whole continuations. A nonempty `values` request computes and publishes the complete table with full coverage, even when the query requests only a few cells. Interpolation templates also materialize the full Float64 query array (`8*count` bytes), followed by the complete table (`count*C*dtype_size` bytes) and source workspace. These allocations share the execution's Root resource budget. A sparse one-million-row multi-PCHIP request returns `ResourceExhausted / CapacityLimit` at node 1 before the full linspace query Result can be admitted under a 1 MiB Payload cap; this is capacity-rejection evidence, not a successful maximum-size run.

The six-template fixture checks source dependency and dirty-region correspondence between the generated and hand-authored graphs. It also checks empty `values` demand with a real failing source continuation: the producer is not polled and the result has empty coverage. Axis-only demand skips function controls and interpolation x/y. For `count=1`, the source omits the end payload; for `count>1`, even a first-value request requires end and computes the full grid. Expression and Bezier reject equal endpoints, while interpolation inherits linspace's repeated-coordinate behavior. Typed validation and upstream errors still apply to the complete active Result inputs, and arithmetic errors keep the source operation's Run scope.

The fixture exercises cache reuse for repeated demand and fresh same-content source Results. A one-node sampler graph asserts exactly two cache hits for its `values` and `axis` Results. Interpolation graphs also export the linspace `query_values` Result and assert exactly three hits. Each returned association uses the active source ObjectIds: an interpolation table associates directly with x, y and the current query Result, while that query and the independent axis associate with the current endpoint Results. Replacing endpoint bindings preserves static `PreparedOperation` objects while recomputing values and axis. All input ports use reversed, unaligned, or scalar-zero backing layouts; controlled executions check caller and actual worker floating-environment restoration, WorkLimit and cancellation after computation begins. Both injected computation failures release live Payload to zero.

The lifecycle group covers all six templates and both output dtypes. It retires every input backing while retaining the independently published values and axis Results and their authorized read windows. The values and axis buffers have separate Root Payload owners; each remains readable after its Result handle is released, and releasing the final windows returns live Root resource counts to zero.

A linear LUT over samples `[0,.25,1]` returns `.125` at query `.25`, while continuous `x^2` is `.0625`. Baking stores discrete samples; CRV-05 interpolates that table and does not retain the source interpolation method. The root focused selection passed `test_numeric_baking_result` and `test_numeric_result_math` 2/2 (0.38 and 4.83 seconds; 5.22 seconds total). Strict and Apple direct runs each passed all seven manual groups. The freshly compiled installed 0.32.0 consumer passed the broader baking/inverse/LUT3D selection 3/3 in 5.71 seconds; the baking case took 2.39 seconds, and the installed Apple invocation passed all seven groups. The CTest durations include work from concurrent CPU tests and are not performance measurements. The manual example target is excluded from the default build; `test_numeric_baking_result` registers its direct behavior entry.


## LUT1D application: CRV-05

`apply_lut1d_node` and `apply_lut1d_channels_node` accept three Result inputs
in order: `input`, `table` and `axis`. Each contains one tensor member under any
schema id/version/key; use the complete `sample_shape()`, including batch axes.
Input/table dtypes independently accept Float32/64; axis is Float64[3]. Scalar
table shape is [L]; channel table shape is [L,C] and maps the final input axis,
retaining rank-1 [C] and C=1. Require 1<=L<=1048576, input rank 1..8 and
logical products <=2^40.

The output port `values` is a Result using `photospider.tensor` v1/member
`samples`, preserving the complete input shape in immutable packed samples with
selected Float32/64 dtype and empty facets. The constructor's `input_type`
argument is only a dtype hint for the default output dtype; optional output dtype
overrides it, and the compiler validates bound edges. Constructors default to
the input dtype hint and `CurveDomain::Reject`. Direct WorkflowDocument nodes
must provide static String `dtype` and `out_of_domain` explicitly. Clamp and
LinearExtrapolate are explicit policies.

```cpp
auto apply = ps::numeric::apply_lut1d_node(
    1, ps::WorkflowInputReference{1}, ps::WorkflowInputReference{2},
    ps::WorkflowInputReference{3}, ps::ElementType::Float64);
```

Bind input=[0,.25,.5,1], table=[0,.25,1], axis=[0,1,.5] to obtain
[0,.125,.25,1]. Reversing table and axis to [1,0,-.5] gives the same function.
The channels example uses input=[[0,1],[.25,.5]], table=[[0,10],[2,8]],
axis=[0,1,1] and returns [[0,8],[.5,9]], because each channel has its own
query and selected table pair.

CRV-04 baking templates return ordinary `values` and `axis` node references that
can connect directly to these Result inputs. Baking samples a function before
application; the consumer interpolates the resulting discrete table and does not
retain the source interpolation method. The six baking chains and their
discretization behavior are documented in the [CRV-04 section](#lut1d-baking-templates-crv-04).

Each nonempty Whole request uses Data, Validation and Descriptor (role 13) for
input, table and axis. It reconstructs and validates the complete endpoint-
weighted RN64 grid, validates all input queries before table arithmetic, computes
every output element/channel, then publishes one immutable dense Result with
full certified coverage and global coordinates. The output association records
actual source ObjectIds, and publication is all-or-nothing. Empty reads no
payload. Dirty mapping follows recorded output demand. Math knot/clamp/singleton
selects one table entry; other queries use the adjacent pair. Generic table
entries outside every evaluated stencil are numerically unused, while typed and
upstream validation still covers the complete Result input. A mathematical
error in an unrequested query can fail the Run.

`UniformAxis` stores the reconstructed Float64 grid in a Root-owned allocation.
The default `ResourceAllocator` charges this axis grid to Metadata; input and
table reads use authorized zero-copy Root windows over their source storage. The
operator does not allocate a dense copy of the complete table. Root accounts
retained owners and read windows, while the output and exact workspace are
separately budgeted. See the [CRV-05 family specification](../../docs/built-in_ops/01-numeric/op_specs/CRV-05_apply_lut1d.md),
[scalar specification](../../docs/built-in_ops/01-numeric/op_specs/CRV-05A_apply_lut1d.md) and [per-channel specification](../../docs/built-in_ops/01-numeric/op_specs/CRV-05B_apply_lut1d_channels.md)
for exact formulas, axis validation, errors, resource bounds and acceptance
requirements.

Run the current focused Result test with:

```sh
cmake --build build/kernel-dev --target test_numeric_result_math -j8
build/kernel-dev/examples/numeric_workflow/photospider_numeric_lut1d strict
build/kernel-dev/examples/numeric_workflow/photospider_numeric_lut1d apple
python3 oracle/ops/numeric/lut1d_oracle.py build/kernel-dev/examples/numeric_workflow/photospider_numeric_lut1d strict
python3 oracle/ops/numeric/lut1d_oracle.py build/kernel-dev/examples/numeric_workflow/photospider_numeric_lut1d apple
ctest --test-dir build/kernel-dev -R '^(test_numeric_lut1d_result|test_numeric_result_math)$' --output-on-failure
```

The `test_numeric_lut1d_result` manual workflow exercises seven groups under
Strict and the locally available Apple profile. Its independent Fraction driver
passes 1,416 bit-equal cases per profile; those cases do not claim to cover every
table size, channel count or input domain. The adapted fixture also checks the
six CRV-04 baking-to-LUT chains, static preparation reuse across input/table/axis
replacement, cache associations refreshed to the current source Results, and
full Whole output coverage after a sparse query. It covers all-port reversed
unaligned layouts, zero-stride aliases, caller and worker floating-point
environments, Empty, static schema bounds, recognized typed RGBA validation of
an unrequested alpha component, and upstream Result failure propagation. Work,
cancellation, output/scratch capacity rejection and context teardown restore all
Root resources. Retained scalar and channel outputs each account 16 live Payload
bytes while a read window remains; releasing the Result and window returns all
Root resources to zero. A [2^39]-channel sparse output request fails at the LUT
node with `ResourceExhausted/CapacityLimit`; a separate L=1,048,576 case succeeds
with the axis grid charged to Metadata and without a dense table copy.

The latest focused root CTest selection passed `test_numeric_lut1d_result` and
`test_numeric_result_math` 2/2 in 4.74 seconds; the existing math integration
test took 4.42 seconds and the LUT1D manual test took 0.31 seconds. The installed
consumer compiled and linked this same source against package 0.32.0, then passed
`installed_numeric_lut1d_result` 1/1 under Strict in 0.38 seconds (0.39 seconds
total). Its direct Apple run also passed all seven groups. These tests are
separate: `test_numeric_result_math` is the existing integration fixture, while
the new manual and installed tests execute `lut1d.cpp`.

The existing integration fixture retains its LUT1D boundary and resource
coverage: custom Result schemas, channel batch shape [2,2,2], eight scalar
Fraction golden bits, and axis/query/table failure precedence. It also cancels
inside a 352-limb `ExactCurve` slot, rejects a sparse request from input[262144]
when the full Float64 output needs 2 MiB under a 1 MiB Payload cap, and accepts a
zero-stride Float32 table [262145] under that cap with only the 8-byte output
added to live Payload.

The installed `installed_numeric_lut1d_result` CTest test runs the
`photospider_numeric_lut1d_consumer` executable, which compiles the same
`lut1d.cpp` against the installed `Photospider::kernel` package. Use the
installed prefix and explicit `Photospider_DIR` when configuring the consumer
tree. The tested commands and result are:

```sh
cmake --install build/kernel-dev --prefix build/kernel-dev/result-only-install
cmake -S tests/consumer -B build/kernel-dev/repeated-result-consumer -DPhotospider_DIR="$PWD/build/kernel-dev/result-only-install/lib/cmake/Photospider"
cmake --build build/kernel-dev/repeated-result-consumer --target photospider_numeric_lut1d_consumer -j8
ctest --test-dir build/kernel-dev/repeated-result-consumer -R '^installed_numeric_lut1d_result$' --output-on-failure
build/kernel-dev/repeated-result-consumer/photospider_numeric_lut1d_consumer apple
```

The Result oracle is an independent exact Fraction comparison for the enumerated
cases, not a claim that all Float32 inputs meet a stronger bound than the
published accelerated contract. The full dynamic axis grid at maximum L was
exercised; the complete maximum physical channel-table allocation and x86 numeric
execution were not run. The old package 0.18 Value-adapter performance
measurements remain historical and do not measure this Result workflow.

## Scalar coordinate shapers

`photospider/numeric/shapers.hpp` exposes two linear authoring helpers and two
log2 node helpers. Each input edge is a Result with one tensor member under any
schema id/version/key; use its full `sample_shape()`, including batch axes.
`input` is Float32/Float64 of rank 1..8, positive extents and at most `2^40`
elements. `lower` and `upper` are same-dtype scalar tensors of shape [1], finite
and ordered `lower<upper`; log2 also requires `0<lower`. Output port `values`
is an immutable Result using `photospider.tensor` v1/member `samples`, preserving
input shape and input dtype, with empty facets. A shaper does not change a color
transfer description.

```cpp
#include <photospider/numeric/shapers.hpp>
// graph is a WorkflowDocument; these edges bind Result tensors.
auto linear = ps::numeric::linear_shaper(graph, x, lower, upper, input_descriptor);
auto inverse = ps::numeric::linear_shaper_inverse(
    graph, linear.value(), lower, upper, input_descriptor);
auto logarithmic = ps::numeric::log2_shaper_node(100, x, lower, upper);
auto log_inverse = ps::numeric::log2_shaper_inverse_node(
    101, ps::WorkflowNodeOutput{100, "values"}, lower, upper);
```

The linear helpers' `input_descriptor` is an authoring shape/dtype hint. The
compiler validates the actual connected Result schemas and bound tensors.
Check each authoring `Result` before accessing its value. The linear helpers
append ordinary Result remap/constant nodes and, for Float32 constants, a
one-element sequence node. They return a connectable `values` output reference,
preserve existing exports and allocate IDs that avoid declared/referenced
producers. They do not add linear primitive keys. The inverse includes the
scalar `lower<upper` guard. Log helpers create nodes for the six
`curve.log2_shaper{,_inverse}` CPU keys. All helpers default to Strict; pass
`CpuNumericProfile` explicitly to select another profile.

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

All six log keys use Whole Result programs with Data, Validation and Descriptor
(role 13) for all three inputs. Linear templates use the same Whole Result
remap/constant nodes and guard. Typed/upstream validation covers the complete
Result inputs; authorized windows feed numeric kernels directly without a full
Value collection or input copy. Bounds are checked before input IEEE handling in
the callback, although typed/upstream failures can occur first. Empty reads no
payload. Any input or bound edit invalidates the complete recorded output demand.
Each nonempty execution publishes the full immutable output, even when a caller
requests a sparse region. Output ownership survives context teardown. Numeric
bound failures use Run scope; existing source failures, resource exhaustion,
backend unavailability, cancellation and stale bindings retain their categories.
Log refinement remains bounded at 4096 fraction bits; unresolved proofs fail
ResourceExhausted. Whole fallback/evaluation counters are unavailable.

Run the focused Result CTest and the Strict and Apple manual/oracle probes with:

```sh
cmake --build build/kernel-dev --target test_numeric_result_math photospider_numeric_shapers -j8
ctest --test-dir build/kernel-dev -R '^test_numeric_result_math$' --output-on-failure
build/kernel-dev/examples/numeric_workflow/photospider_numeric_shapers strict
python3 oracle/ops/numeric/shaper_oracle.py build/kernel-dev/examples/numeric_workflow/photospider_numeric_shapers strict
build/kernel-dev/examples/numeric_workflow/photospider_numeric_shapers apple
python3 oracle/ops/numeric/shaper_oracle.py build/kernel-dev/examples/numeric_workflow/photospider_numeric_shapers apple
```

The Result CTest passes 1/1 in 7.22 seconds. It covers `shaper_workflows`,
`shaper_boundaries`, `shaper_preparation_and_cancel` and the existing
`result_shaper_authoring` checks. All six manual fixture groups pass under Strict
and the local Apple profile. The independent Fraction/directed-MPFR 4.2.2 oracle
passes 4,196 cases under each profile against the Result probe. The fixtures
cover all four forms and both dtypes, joint and reversed partitions, typed
inputs, negative and unaligned strides, floating-environment preservation,
zero and large budgets, cancellation and upstream failures.

The installed consumer passes 1/1 in 0.33 seconds. It exercises public log
forward/inverse composition, linear inverse wiring and reads outputs after
context teardown. Reproduce the installed check with:

```sh
cmake --install build/kernel-dev --prefix build/kernel-dev/consumer-install
cmake -S tests/consumer -B build/kernel-dev/consumer-build -DCMAKE_PREFIX_PATH="$PWD/build/kernel-dev/consumer-install"
cmake --build build/kernel-dev/consumer-build --target photospider_result_numeric_consumer -j8
ctest --test-dir build/kernel-dev/consumer-build -R '^installed_result_numeric$' --output-on-failure
```

Whole execution certifies the complete output and retains global coordinates;
a sparse request does not reduce the Result descriptor's coverage. Repeated
requests return consistent values, but these checks do not establish a warm
cache hit. x86 numerical execution, GPU execution, maximum-size shapes and
performance were not tested. MPFR 4.2.2 is used only by the independent oracle.

## Measured three-dimensional LUT baking

`photospider/numeric/lut3d_baking.hpp` expands a pointwise source transform into
ordinary workflow nodes. A source builder receives a generated Float64 Result
tensor and its descriptor, appends source nodes and returns an output reference.
It is called twice during authoring: for `[N0,N1,N2,3]` grid colors and `[P,3]`
validation colors. Each source output must resolve to one Result tensor with the
matching complete sample shape and Float32/64 dtype. An attached ColorArray
facet must match the required description; a generic tensor may omit that
facet. Shared inputs remain ordinary graph edges. The helper retains no
builder or capture for execution. The caller asserts that source output depends
on each sampled color only, not on position, batch shape or other samples.

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

The public `baking3d.cpp` harness runs the Result workflow. Its input fixtures
keep `Value` as backing storage only; bindings publish that storage through
immutable Root-owned Referenced Results. Geometry, table, axis and report
outputs are Results.

The helper stages authoring changes and leaves existing declarations, Result
schemas, nodes and exports intact. A source builder appends nodes only; an error
or exception leaves the caller's document unchanged. Both expansions are
analyzed for matching tensor shape, dtype and color metadata. Static preparation
stores immutable geometry and report POD state for reuse during execution.
Grid and validation sources use the same immutable binding snapshot, including
shared inputs and optional `ResourceBindings`. Source keys and profiles are
preserved; the bake profile selects only generated sampling and LUT application
facilities.

Bind axis as a Result containing one Float64 tensor of `sample_shape()` [3,3],
for example `[[0,1,1],[0,1,1],[0,1,1]]` for the identity example.
Shape extents are 2..256 independently. Each axis may ascend or descend under
CRV-07's exact reconstructed-grid rules. Optional validation points are dynamic
Result tensor Float64 [M,3], M=1..1048576; omit the argument for none. Centers of every grid
cell are always included, with each coordinate rounded once from its two actual
Float64 neighbors. Extras follow centers in array order and must lie in-domain;
repeated points count separately. Source and converted colors must remain finite
and legal in the explicitly supplied same-model descriptions. Table dtype defaults
to the inferred source output dtype; an explicit Float32/64 conversion rounds
each component once. Source graph operation keys/profiles and explicit casts
define the baked reference.

`read_lut3d_bake_report(result.results.at("report"))` reads a sealed
`curve.bake_lut3d.report` v1 Result. The fixed 289-byte payload contains pass/counts,
validated axis, exact-error maxima rounded upward with their points/earliest
indices, and the first failure's input/reference/LUT values. Schema metadata says
Measured and records shape, method, tolerances, dtypes, color descriptions and
source recipe identity. It is not a bound over the continuous domain.

The exported table is an immutable tensor `photospider.tensor` Result with
member `samples`, shape [N0,N1,N2,3], selected table dtype, ColorArray v1 facet
and `atomic_trailing_axes=1`. Pack's internal owned table uses
`curve.bake_lut3d.table` v2, with one `colors` tensor and the same shape, dtype,
facet and measured metadata. Geometry outputs use Result tensor schemas;
color-valued outputs carry the relevant ColorArray facet, while axis output is
plain Float64 [3,3]. All 15 geometry profile keys use Whole Result programs
with role-13 input validation.
`execute()` can return the report field Result. `execute_fragments()` supports
tensor footprints for table and axis; report fields are fixed Result fields, not
a generic tensor footprint. Requesting report evaluates all source rows and the
global measurement.

Measure observes axis, grid, owned table, validation points, source reference and
applied LUT as Result inputs. The host records observed ObjectIds in port order
in the report association: axis, grid and owned table occupy its first three
entries, while source batches may contribute multiple later owners. Entry 2
identifies the owned-table Result. The gate validates all 11 report fields before
requesting the table. After it requests the full table with role 13 and typed
validation, it checks entry 2 against the actual table input before publishing
the requested color ROI; it preserves that ROI's global coordinates. This order
does not promise that an association mismatch precedes table-pixel validation.

Pack, unpack and gate request authorized tensor views and retain legal
common-owner mappings. The harness covers Float32/Float64 across all five
geometries, reversed and unaligned layouts, and verifies that a legal packed
window points into its original backing storage. Pack takes two Result polls in
this path; the former Value-copy poll is absent. A physical mapping fallback
uses transactional Result materialization charged to Root payload. The legal
view path reports zero Root Payload. Empty tensor coverage skips payload
validation, and the owned-table validator preserves callback status category
and scope, including WorkLimit.

Identity passes zero tolerance. For the source `(r*r,g,b)`, a 2×2×2 grid and
center `[.5,.5,.5]` give reference `[.25,.5,.5]`, applied LUT `[.5,.5,.5]` and
maximum errors `[.25,0,0]`. For a source that squares each component on a
2×2×2 grid, the center reference is `[.25,.25,.25]`, while either applied LUT gives
`[.5,.5,.5]`. With atol=.1/rtol=0, the report has passed=false, failed_count=1,
max_abs_error=`[.25,.25,.25]`, first_failure_index=0. Table requests fail with
`LutApproximationToleranceExceeded`, including a request for one exact grid
vertex; axis-only remains independent. The relative test uses
`abs(lut-reference)<=atol+rtol*abs(reference)` with exact arithmetic.

For separate report-only inspection, export report only. `execute()` returns the
report fields; `execute_fragments()` takes tensor footprints for table/axis and
does not select fixed report fields with a generic [1] tensor footprint.
Ordinary multi-output `execute` remains fail-fast. To retain a completed failed report from a table
execution, use `ExecutionOptions::result_publication`, as `facilities()` does;
the callback receives an owning ResultRef. No incomplete/erroring measurement is
published as a successful failed report. A report read window must admit its
largest 72-byte field; otherwise gate/report validation fails with
ResourceExhausted/CapacityLimit. The report uses the same pure POD validation
rules as `read_lut3d_bake_report`. The owned sampled-table Result is an
intermediate with no quality guarantee. Gate checks the report's owned-table
association against the supplied table ObjectId before reading requested table
fragments. Different shared parameter snapshots cause a new measurement;
matching shapes alone are insufficient.

The `baking3d.cpp` manual executable accepts `strict` and `apple`; each runs 12
behavior groups. The Fraction oracle passes 384 cases per profile. The cases
cover all five geometries, both table dtypes, reversed and unaligned layouts,
table/axis/report demand, Empty and source-error ordering, cache hits and current
ObjectId association, preparation reuse and rebinding, report quality gating,
source owner retirement, and read-window lifetime. Its cache fixture observes
14 hits in the measured run but asserts only that at least one hit occurs. It
sets a 64 MiB Result cache, `maximum_dependency_cache_metadata` to 1,048,576
proof units, and `maximum_dependency_cache_work` to 128 Mi work units; the
metadata limit is not a byte count. A huge grid case verifies
CapacityLimit at its geometry node; it does not compute a maximum-shape table.

Build and run the focused behavior test and oracle with:

```sh
cmake --build build/kernel-dev --target test_numeric_baking3d_result -j8
ctest --test-dir build/kernel-dev -R '^test_numeric_baking3d_result$' --output-on-failure
build/kernel-dev/examples/numeric_workflow/photospider_numeric_baking3d strict
PYTHONPYCACHEPREFIX=build/kernel-dev/python-pycache python3 oracle/ops/numeric/baking3d_oracle.py build/kernel-dev/examples/numeric_workflow/photospider_numeric_baking3d strict
build/kernel-dev/examples/numeric_workflow/photospider_numeric_baking3d apple
PYTHONPYCACHEPREFIX=build/kernel-dev/python-pycache python3 oracle/ops/numeric/baking3d_oracle.py build/kernel-dev/examples/numeric_workflow/photospider_numeric_baking3d apple
```

The final root focused selection passed 9/9 tests in 15.30 seconds: eight
manual workflow tests, including `test_numeric_baking3d_result` (5.12 seconds),
and the shared `test_numeric_result_math` integration test (4.58 seconds). The
shared test's bake assertions provide independent fixture coverage, not all
`baking3d.cpp` cases.

The installed SDK consumer builds the same example against
`Photospider::kernel` and registers `installed_numeric_baking3d_result`.
Reproduce it with the configured 0.32.0 install prefix:

```sh
cmake --install build/kernel-dev --prefix build/kernel-dev/result-only-install
cmake -S tests/consumer -B build/kernel-dev/repeated-result-consumer -DPhotospider_DIR="$PWD/build/kernel-dev/result-only-install/lib/cmake/Photospider" -DCMAKE_PREFIX_PATH="$PWD/build/kernel-dev/result-only-install"
cmake --build build/kernel-dev/repeated-result-consumer --target photospider_numeric_baking3d_consumer -j8
ctest --test-dir build/kernel-dev/repeated-result-consumer -R '^installed_numeric_baking3d_result$' --output-on-failure
```

The fresh 0.32.0 installed-consumer selection passed 8/8 tests in 14.63 seconds;
`installed_numeric_baking3d_result` passed in 5.91 seconds. Running the installed
consumer directly with `apple` passed all 12 behavior groups.

Report reading requires a Result window large enough for its largest fixed-size
field (72 bytes). The measured quality guarantee applies at the listed centers
and extra points using the chosen interpolation and table dtype. It is not a
continuous-domain bound. This validation does not establish x86, maximum-shape
success or native-GPU execution. Historical 0.18 Value-path timings and raw
traces do not measure the current Result implementation.

Table, axis and report can be requested, retained and released independently.
Releasing the report handle does not promise immediate release of all report
backing while a table view or read window retains dependency ancestry. The
authorized table window remains readable after all three output handles and
fixture/context owners are released; releasing its final window returns Root
live capacity to zero. External Float32/Float64 axis backing expires after
fixture and context teardown.

## Inverse curves

`photospider/numeric/inverse_curves.hpp` provides `invert_linear_node` and `invert_pchip_node`. Each input is a Result containing one tensor member under any schema id and member key; `sample_shape()` supplies x[K], y[K] and query[N]. The three inputs independently accept Float32/64. x must be finite and strictly increasing; y must be finite and strictly increasing or decreasing. The `values` output is a Result with schema `photospider.tensor`, member `samples`, shape [N] and empty facets. K is 2..65536 and N is 1..2^40. Constructors default to Float64 and `reject`; direct workflow nodes must provide String parameters `dtype` and `out_of_domain` explicitly (`float32`/`float64`, `reject`/`clamp`).

The inverse solves the original forward function, without swapping x/y and fitting a new interpolator. Linear computes the selected segment as one exact rational expression. PCHIP inverts the original exact Hermite polynomial and compares it at destination lattice values and their exact midpoint, including subnormal and overflow boundaries. Strict rounds final x once. Accelerated profiles follow the shared final FP32 four-ULP contract; PCHIP uses a certified bracket and the strict solver when needed. Knot/clamp and signed-zero rules remain exact.

```cpp
#include <photospider/numeric/inverse_curves.hpp>

// document.inputs binds ids 1, 2, 3 to x, y, query respectively.
auto inverse = ps::numeric::invert_pchip_node(
    10, ps::WorkflowInputReference{1}, ps::WorkflowInputReference{2},
    ps::WorkflowInputReference{3});
if (!inverse.ok()) return inverse.status();
document.nodes.push_back(inverse.take_value());
document.outputs.push_back({"x_values", 10, "values"});
```

For x=[0,1,3], y=[0,2,4], query=[3,1,3], linear returns [2,0.5,2]. PCHIP for x=[0,1,2], y=[0,1,4], query=[0.3125,2.1875,0.3125] returns [0.5,1.5,0.5]. A public composition connects a forward interpolator's `values` output directly to inverse `query`; the exact dyadic sample returns [0.5,1,1.5]. Arbitrary rounded forward/inverse outputs need not recover original query bits.

All six keys use Whole execution. A nonempty request collects all inputs with Data, Validation and Descriptor (role 13), validates global x/y topology and every query before inverse arithmetic, then computes every query and publishes a complete dense Result with full coverage and global sample coordinates. The Result association records the current source ObjectIds. Empty demand reads no payload; an Empty request propagates an upstream failure without polling the failed producer. Input edits invalidate the complete output, and invalid data outside the delivered footprint can fail the run. The 16*K promoted x/y bytes, fixed exact workspace and dense output are resource-accounted. Work or cancellation failures release temporary storage, while output ownership survives context teardown.

The manual fixture covers K=65536 successfully. A public `numeric.constant` View with N=2^40 is rejected as `ResourceExhausted/CapacityLimit` at the inverse node because Whole execution requires complete output storage; this is a capacity check, not a numerical run at that physical output size. The fixture also checks a true fresh-source cache hit with current direct source associations, repeated-demand ObjectId retention, static preparation reuse, typed `SampledSignal` rejection attributed to input 2, both floating-environment preservation at the caller and actual computation worker, and Float32/Float64 source-owner retirement. An escaped output and read window retain 12/24 Payload bytes; releasing the final window returns Root live capacity to zero. See the [CRV-10 family specification](../../docs/built-in_ops/01-numeric/op_specs/CRV-10_invert.md) for algorithm and error details.

Build and run the manual Result fixture and independent Fraction oracle with:

```sh
cmake --build build/kernel-dev --target photospider_numeric_inverse test_numeric_inverse_result -j8
build/kernel-dev/examples/numeric_workflow/photospider_numeric_inverse strict
build/kernel-dev/examples/numeric_workflow/photospider_numeric_inverse apple
python3 oracle/ops/numeric/inverse_oracle.py build/kernel-dev/examples/numeric_workflow/photospider_numeric_inverse strict
python3 oracle/ops/numeric/inverse_oracle.py build/kernel-dev/examples/numeric_workflow/photospider_numeric_inverse apple
ctest --test-dir build/kernel-dev -R '^test_numeric_inverse_result$' --output-on-failure
```

The manual fixture, shared integration cases and independent oracle provide separate evidence: the manual source owns its Result bindings and owner-lifetime checks; `test_numeric_result_math` covers `inverse_workflows` and `inverse_boundaries`; the 407-case Fraction oracle checks numerical outputs. Strict and Apple manual runs each pass five groups, and each oracle run passes all 407 cases. Strict matches the exact reference; Apple is checked against the shared FP32-scaled bound, not bitwise equality. The dedicated `test_numeric_inverse_result` CTest passes 1/1 in 0.89 seconds (0.94 seconds total for its focused selection). The separate `test_numeric_result_math` integration test passed in a distinct selection alongside `test_numeric_baking_result`; the selection passed 2/2 in 5.22 seconds, with 4.83 seconds in the shared math executable. An independent source/contract review and worker verification found no blocker or required changes.

The installed consumer target `photospider_numeric_inverse_consumer` uses the 0.32.0 SDK package, and CTest names it `installed_numeric_inverse_result`. The installed consumer selection for baking, inverse and LUT3D passed 3/3 in 5.71 seconds; the inverse case took 0.94 seconds. Its direct Apple run passed all five groups. Reproduce that consumer check with:

```sh
cmake --install build/kernel-dev --prefix build/kernel-dev/result-only-install
cmake -S tests/consumer -B build/kernel-dev/repeated-result-consumer -DPhotospider_DIR="$PWD/build/kernel-dev/result-only-install/lib/cmake/Photospider" -DCMAKE_PREFIX_PATH="$PWD/build/kernel-dev/result-only-install"
cmake --build build/kernel-dev/repeated-result-consumer --target photospider_numeric_inverse_consumer -j8
ctest --test-dir build/kernel-dev/repeated-result-consumer -R '^installed_numeric_inverse_result$' --output-on-failure
```

## Signal resampling

`photospider/numeric/resampling.hpp` exposes four ordinary workflow templates: `resample_linear`, `resample_pchip`, `resample_linear_multi`, and `resample_pchip_multi`. Each appends the corresponding CRV-01 interpolator node and an independent `core.identity` node for `new_positions`. The inputs are `positions[K]`, `values[K]` or `[K,C]`, and `new_positions[N]`, independently Float32/64. The returned `ResampledSignal` holds connectable `samples` and `positions` node references; `outputs()` creates explicit caller-named exports. Sample dtype defaults to Float64 and domain policy to Reject, with the selected interpolator's Clamp and LinearExtrapolate policies also available.

Bindings and execution outputs use Results throughout; the `Value` objects in the manual fixture provide immutable source backing. `positions` maps the bound query Result's descriptor, facets and raw bits, including sNaN/Inf/-0, without copying its payload. Its requested source support is exactly the requested query region and it has no curve-source dependency. Normal typed and upstream validation still applies to requested positions. A `samples` request inherits CRV-01 Whole behavior: complete positions, values and query validation, full output coverage, complete dirty support, and Run failures for invalid undelivered queries or columns. The four helpers preserve existing exports, reserve collision-free IDs for declarations, references and outputs, and leave the document unchanged on authoring failure.

The editable `resampling.cpp::filtered_workflow` connects an explicit Hann low-pass to linear resampling through the installed API:

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

With old positions `[0,1,2,3,4,5,6,7]`, signal `[1,-1,1,-1,1,-1,1,-1]` and new positions `[0,2,4,6]`, Strict samples have Float64 bits `0x3fcc6b828682ab42`, the once-rounded value of `(pi-2)/(pi+2)`. Accelerated profiles use the shared FP32 four-ULP bound. This finite filter leaves a positive Nyquist residual, so this example does not remove all aliasing. The exported positions are exactly `[0,2,4,6]`; select and measure other parameters for a different quality requirement.

The manual executable passes eight groups under each of Strict and Apple. It retains CRV-01's exact-copy and numeric-accuracy checks and has no independent resampling oracle. Float64 layouts cover all four templates, eight source-layout combinations, and all four caller/actual-worker rounding modes. Float32 coverage separately checks special-bit forwarding, typed validation and owner lifetime; it does not run the full fenv matrix. Typed `SampledSignal` validation is output-specific: a valid requested positions element succeeds, a requested typed NaN fails with `InvalidArgument` at input 3, and a samples request validates the complete query and rejects a remote NaN at input 3. The manual fixture binds source backing as Referenced without creating Root Payload copies. A mapped positions Result and its authorized read window keep the query owner alive after the Result handle is released; releasing the last window retires that owner. K=65536 performs a real endpoint interpolation. At N=2^40 the positions view succeeds without materialization, while the complete Whole samples output fails with `ResourceExhausted/CapacityLimit` at node 1. This checks rejection of the full output allocation, not numerical execution at that size.

Build the manual example and its focused behavior test, then run the shared integration fixture separately:

```sh
cmake --build build/kernel-dev --target test_numeric_result_math photospider_numeric_resampling -j8
ctest --test-dir build/kernel-dev -R '^(test_numeric_resampling_result|test_numeric_result_math)$' --output-on-failure
build/kernel-dev/examples/numeric_workflow/photospider_numeric_resampling strict
build/kernel-dev/examples/numeric_workflow/photospider_numeric_resampling apple
```

The dedicated `test_numeric_resampling_result` checks this manual behavior; `test_numeric_result_math` is a separate shared integration fixture. The root focused test passed, and the two fixture scopes must not be treated as an independent resampling oracle.

The installed 0.32.0 consumer compiles the same public example through `Photospider::kernel`. Its Strict CTest passed 1/1, and the direct Apple run passed all eight groups:

```sh
cmake --install build/kernel-dev --prefix build/kernel-dev/result-only-install
cmake -S tests/consumer -B build/kernel-dev/repeated-result-consumer \
  -DPhotospider_DIR="$PWD/build/kernel-dev/result-only-install/lib/cmake/Photospider"
cmake --build build/kernel-dev/repeated-result-consumer --target photospider_numeric_resampling_consumer -j8
ctest --test-dir build/kernel-dev/repeated-result-consumer -R '^installed_numeric_resampling_result$' --output-on-failure
build/kernel-dev/repeated-result-consumer/photospider_numeric_resampling_consumer apple
```

Use `x86` only on a supported AVX2 build; no x86 run or Result performance run is recorded for this revision.

## Uniform lowpass

`photospider/numeric/lowpass.hpp` provides five independent
`lowpass_uniform_{hann_sinc,hamming_sinc,blackman_sinc,kaiser_sinc,gaussian}_node`
helpers. The `input` port is a Result with one tensor member and no fields under any
structurally valid schema id/version/member key. Use complete `sample_shape()`
including batch axes. The tensor is Float32/64, rank 1..8, positive extents and
at most 2^40 elements. Static `axis` selects independent signals. Output port
`values` is a `photospider.tensor` v1/member `samples` Result that retains the
full shape/dtype and has generic facets. The output schema selects its own
resources rather than copying source-only facets/resources. Sinc cutoff is in `(0,.5)`
cycles/sample; Kaiser additionally takes finite `beta>=0`. Gaussian takes
finite `sigma>0` in samples instead of cutoff. No unused parameter is accepted.
Boundary defaults to Reflect without endpoint repetition; Replicate, Zero and
Wrap are explicit alternatives. Output positions remain aligned to the input.

Every logical nonzero coefficient is included, even when its numerical enclosure
is too small to affect a finite result. Exact sinc integer zeros and Hann/Blackman
endpoints are omitted from numerical operands; Whole role-13 typed/upstream validation still covers the full input member. Logical order `-R..R` controls first-NaN
payload/sign and infinite contribution aggregation, including repeated reflected
indices. These are successful IEEE results. Finite constant extended samples
preserve their identical bits; other exact zeros are +0. Caller floating state
is preserved. Whole role-13 validation retains attached typed semantics.

The whole mathematical sum and full normalizer are enclosed before one final
rounding in strict. Accelerated prepares certified coefficient enclosures once
per Whole invocation and applies the final FP32 bound, with strict fallback if
unresolved. Whole counters are N/A. All 15 keys use Whole Result programs. Each nonempty Run declares Data,
Validation and Descriptor (role 13) for the complete input tensor. The callback
reads authorized windows directly without collecting or copying the full input;
typed/upstream validation still covers all elements, including outside delivery.
Empty reads no sample payload after static preflight. Whole writes the complete
same-shape output transactionally with full coverage and global coordinates; an
input edit invalidates complete recorded output demand. Legal strides and the
logical zero-tap/IEEE rules remain unchanged. The Root accounts source windows,
the full dense output, coefficient/tap workspaces and math state, so sparse
requests can exhaust capacity. Cancellation or failure publishes no partial
output, and the owning Result remains live after context teardown.

For `[0,0,1,0,0]`, radius 2 and center index 2, the exact Float64 fixtures are:

| Kernel | Parameters | Center bits |
| --- | --- | --- |
| Hann sinc | cutoff=.25 | `3fe38d7050d05568` |
| Hamming sinc | cutoff=.25 | `3fe2f660651f7f7c` |
| Blackman sinc | cutoff=.25 | `3fe655124d269c1c` |
| Kaiser sinc | cutoff=.25, beta=0 | `3fdc2755e149a310` |
| Gaussian | sigma=1 | `3fd9c486742831f7` |

```sh
cmake --build build/kernel-dev --target test_numeric_result_math photospider_numeric_lowpass photospider_numeric_lowpass_execution -j8
ctest --test-dir build/kernel-dev -R '^(test_numeric_lowpass_result|test_numeric_lowpass_execution_result|test_numeric_result_math)$' --output-on-failure
build/kernel-dev/examples/numeric_workflow/photospider_numeric_lowpass strict
build/kernel-dev/examples/numeric_workflow/photospider_numeric_lowpass apple
build/kernel-dev/examples/numeric_workflow/photospider_numeric_lowpass_execution strict
build/kernel-dev/examples/numeric_workflow/photospider_numeric_lowpass_execution apple
PYTHONPYCACHEPREFIX=build/kernel-dev/python-pycache python3 oracle/ops/numeric/lowpass_oracle.py build/kernel-dev/examples/numeric_workflow/photospider_numeric_lowpass strict
PYTHONPYCACHEPREFIX=build/kernel-dev/python-pycache python3 oracle/ops/numeric/lowpass_oracle.py build/kernel-dev/examples/numeric_workflow/photospider_numeric_lowpass apple
```

The Result-bound 474-case independent directed MPFR oracle checks complete sums
for all five kernels/dtypes/boundaries, impulses, exact quarter-wave and Nyquist
periodic sinusoids, repeated logical taps, source specials and extreme Gaussian
scales. This verifies the defined discrete response, not a universal attenuation
target. Uniform nonempty Runs declare complete Data, Validation and Descriptor
needs, read authorized Result windows without collecting/copying source tensors,
and publish full dense coverage transactionally. Empty demand reads no sample
payload after static preflight. Typed/upstream failures can arise outside the
requested region; failures publish no partial Result. Static preparation is
reused after rebinding, and fresh-content execution verifies a cache hit with the
current direct source association. Source storage is charged as Referenced with
zero Root Payload copy; Float32/64 lowpass outputs retain 20/40 Payload bytes,
and an authorized window remains readable after its Result handle is released.

`lowpass_execution.cpp` checks all ten kernels, all-port negative/unaligned
layouts, four rounding modes with actual worker and caller environment checks,
non-last-axis constants, cache replacement, current associations, static
preparation reuse, typed/upstream failures, Empty demand, active cancellation,
ownership and output capacity. Its `mode 2` case rejects the combined Payload
requirement for output, workspace and continuation; it does not isolate a scratch
allocation failure. The 2^40-element sparse-demand case rejects the complete
Whole output at node 1 with `CapacityLimit`, rather than attempting a numerical
run at that size.

Under both Strict and Apple, the uniform and nonuniform executables pass two groups each, and the execution executable passes four groups. `lowpass_period_cancel` enters `geometry.partition`, cancels on its 8192-unit work charge and checks payload rollback and Root retirement. No x86 numerical execution, native GPU run or Result performance run was done. The benchmark tables below are historical Value-path measurements.

Reproduce the focused Result run with:

```sh
cmake --build build/kernel-dev --target test_numeric_result_math photospider_numeric_lowpass photospider_numeric_lowpass_nonuniform photospider_numeric_lowpass_execution -j8
ctest --test-dir build/kernel-dev -R '^(test_numeric_lowpass_result|test_numeric_lowpass_nonuniform_result|test_numeric_lowpass_execution_result|test_numeric_result_math)$' --output-on-failure
```

The dedicated `test_numeric_lowpass_result` and `test_numeric_lowpass_execution_result` CTests pass. The shared `test_numeric_result_math` fixture also passes, and the final focused cross-family root selection passed 9/9 in 15.30 seconds. The installed 0.32.0 `Photospider::kernel` consumers below use separate test registrations from those root tests.

Reproduce the installed public consumer check with:

```sh
cmake --install build/kernel-dev --prefix build/kernel-dev/result-only-install
cmake -S tests/consumer -B build/kernel-dev/repeated-result-consumer \
  -DPhotospider_DIR="$PWD/build/kernel-dev/result-only-install/lib/cmake/Photospider"
cmake --build build/kernel-dev/repeated-result-consumer --target photospider_numeric_lowpass_consumer photospider_numeric_lowpass_execution_consumer -j8
ctest --test-dir build/kernel-dev/repeated-result-consumer -R '^(installed_numeric_lowpass_result|installed_numeric_lowpass_execution_result)$' --output-on-failure
build/kernel-dev/repeated-result-consumer/photospider_numeric_lowpass_consumer apple
build/kernel-dev/repeated-result-consumer/photospider_numeric_lowpass_execution_consumer apple
```

The installed uniform consumer passes its Strict CTest and two direct Apple groups; the execution consumer passes its Strict CTest and four direct Apple groups. These tests compile the public example sources against the installed 0.32.0 package without private headers. The performance tables below describe the earlier Value path and do not measure Result performance.

## Nonuniform lowpass

Five `lowpass_nonuniform_{hann_sinc,hamming_sinc,blackman_sinc,kaiser_sinc,gaussian}_node`
helpers take `positions` and `values` as Results, each with exactly one tensor
member and no fields under any structurally valid schema id/version/member key.
Use complete `sample_shape()` values including batch axes. Positions has shape
[K], is Float32/64, finite and strictly increasing, K=2..1048576. Values
independently uses Float32/64 with rank 1..8 and product <=2^40; static `axis`
selects the extent K, and other dimensions identify independent signals. Output
`samples` is a `photospider.tensor` v1/member `samples` Result with the original
full values shape/dtype and generic facets. The output schema selects its own
resources rather than copying source-only facets/resources. `support_radius>0` and Gaussian
`sigma>0` use coordinate units; sinc `cutoff>0` uses cycles per coordinate unit,
with no .5 limit. Kaiser additionally takes `beta>=0`.

The operation convolves the exact piecewise-linear reconstruction against the
continuous kernel using coordinate-length measure. Reflect folds at the domain
endpoints, Replicate extends endpoint values, Zero extends +0, and Wrap repeats
the domain with a possible seam jump and no extra connecting segment. Full-kernel
normalization remains in force at boundaries. Global positions are validated for
each nonempty request. All 15 keys use Whole Result programs. Nonempty Runs declare Data, Validation
and Descriptor (role 13) for both complete input members. Typed/upstream checks
cover all elements. The callback reads positions through authorized windows into Root-owned
promoted coordinate storage, then reads values through authorized windows
without collecting or copying the complete values tensor. It computes all
centers and columns while retaining the original positive-length segments and
exact endpoint rules. Nonfinite values or output overflow anywhere, including
outside delivery, fail Domain/Run. Empty reads no sample payload after static
preflight. The Result writer publishes the complete tensor in one transaction
with full coverage and global coordinates; input edits invalidate complete
recorded output demand. Root budgets promoted positions, a reused piece vector,
full output and math workspaces. Failures/cancellation release temporary state
and publish no partial output. Whole numerical/fallback counters are unavailable;
nonuniform profiles still use strict math.

`lowpass_nonuniform.cpp` contains the full public binding/execution example. For
`positions=[0,.75,2]`, `values=[1,2.5,5]`, `support_radius=.5`, every kernel
returns exactly `2.5` at the middle position; inserting collinear knots preserves
it. For `positions=[0,1]`, `values=[2,2]`, radius `.5`, Zero returns `[1,1]`
and the other boundaries return `[2,2]`. With minimum subnormal values under Zero,
the exact half-minimum result rounds to +0; with three minima it rounds to two
minima, in both Float32 and Float64.

```sh
cmake --build build/kernel-dev --target test_numeric_result_math photospider_numeric_lowpass_nonuniform -j8
ctest --test-dir build/kernel-dev -R '^(test_numeric_lowpass_nonuniform_result|test_numeric_result_math)$' --output-on-failure
build/kernel-dev/examples/numeric_workflow/photospider_numeric_lowpass_nonuniform strict
build/kernel-dev/examples/numeric_workflow/photospider_numeric_lowpass_nonuniform apple
PYTHONPYCACHEPREFIX=build/kernel-dev/python-pycache python3 oracle/ops/numeric/nonuniform_lowpass_oracle.py build/kernel-dev/examples/numeric_workflow/photospider_numeric_lowpass_nonuniform strict
PYTHONPYCACHEPREFIX=build/kernel-dev/python-pycache python3 oracle/ops/numeric/nonuniform_lowpass_oracle.py build/kernel-dev/examples/numeric_workflow/photospider_numeric_lowpass_nonuniform apple
```

The Result-bound 245-case independent Fraction/directed MPFR oracle enumerates
exact boundary copies, uses product-to-sum identities for cosine windows and
exact rational Gaussian/Kaiser polynomial coefficients, and integrates the
affine pieces with explicit remainder bounds. It covers irregular gaps and narrow
hats, collinear insertion, periodic reconstructed sinusoids, seam jumps and
repeated periods, mixed dtypes and subnormal ties. Uniform and nonuniform outputs need not
agree on an equally spaced input: their mathematical reconstruction/measure differ.

The implementation uses exact coordinate partitions, exact paired-affine identities
for constant/half/zero landmarks, and certified global Taylor moments for general
signals. Precision refines from 128 to 4096 bits and Taylor order up to 512.
Large support-to-sigma ratios, high cutoff-radius products, large beta, near
midpoint cancellation or many repeated periods can exhaust those limits or host
work/capacity. No unconverged approximation is published. Each stored piece owns
four 12288-bit coordinate records plus indices; exact source copies, polynomial
vectors and growth overlap are admitted explicitly. This implementation
prioritizes certified results and composability; it makes no throughput claim.
The manual example target is excluded from the default build, and its direct
behavior is registered as `test_numeric_lowpass_nonuniform_result`. The shared
`test_numeric_result_math` integration fixture is separate from that manual CTest.

The nonuniform manual executable passes two groups under each of Strict and
Apple. Its independent Fraction/directed-MPFR oracle passes 245 cases per profile
with exact `got == want` bits. The dedicated root CTest and shared math fixture
pass; both are included in the final focused cross-family selection, which passed
9/9 in 15.30 seconds. The selection also includes adjacent numeric families. No
x86 numerical execution, native GPU run, full CTest or Result performance run was
done. The timing tables below are historical measurements from the earlier Value
implementation.

The installed 0.32.0 consumer compiles this source against the public
`Photospider::kernel` package. Its Strict CTest passed 1/1, and its direct Apple
run passed two groups:

```sh
cmake --install build/kernel-dev --prefix build/kernel-dev/result-only-install
cmake -S tests/consumer -B build/kernel-dev/repeated-result-consumer \
  -DPhotospider_DIR="$PWD/build/kernel-dev/result-only-install/lib/cmake/Photospider"
cmake --build build/kernel-dev/repeated-result-consumer --target photospider_numeric_lowpass_nonuniform_consumer -j8
ctest --test-dir build/kernel-dev/repeated-result-consumer -R '^installed_numeric_lowpass_nonuniform_result$' --output-on-failure
build/kernel-dev/repeated-result-consumer/photospider_numeric_lowpass_nonuniform_consumer apple
```
## Historical regional signal timing and accounting

The following table predates Whole and is retained as historical measurement.
Current counters and storage differ; use the CRV-10/11 math notes for this revision.

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

## Category benchmark diagnostics and historical Value measurements

The current category benchmark binds Result sources and checks Result outputs
against its fixtures. The filtered Strict commands above verify the NUM-11 and
NUM-13 diagnostic counters. The CSV and timing records in this section are
historical Value-path measurements and do not describe current Result
performance.

```sh
cmake --build build/numeric --target photospider_numeric_category_benchmark -j 4
build/numeric/examples/numeric_workflow/photospider_numeric_category_benchmark strict > build/category-strict.csv
build/numeric/examples/numeric_workflow/photospider_numeric_category_benchmark apple > build/category-apple.csv
```

The historical Value-path run produced 36 CSV rows per profile. The recorded run
used Apple M5, Clang 21, RelWithDebInfo, one CPU
worker, CPU-only execution, and both result/dependency caches disabled. There
are three repetitions per row. These are workflow execution times, including
planning of runtime dependencies, validation and output publication. Graph
construction, compilation, freeze, result checking and result destruction are
outside the timer. The context is reused for three executions; there is no
separate discarded warm-up. Median and maximum are reported, with no statistical
percentile or throughput claim from three samples. No build or other acceptance
process ran concurrently with these measurements. WSL is used only for numerical
correctness and supplies no timing reference.

The earlier benchmark fixtures used a small analytic case and a larger
exploratory shape: N=1 and N=256, except integration uses N+1 samples and baking
uses cube side 2 and 5. These shapes describe that Value-path benchmark, not the
current Result implementation; they do not establish maximum-shape throughput
or cover every primitive, parameter, model and dtype in a cluster. Values use Float64, with Int64 gather
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

Every case in the historical run passed exact bits and reported zero strict
fallbacks in that fixture. That describes those chosen exact/dyadic cases only; general logarithms, transfer
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

### NUM-04 exp SIMD implementation

The formal Float32 accelerated exp helpers use a certified normal-range NEON or
AVX2/FMA batch kernel. Float64 exp and NUM-01 expression exp use certified
SLEEF binary64 enclosures in [-80,80], without narrowing their inputs. The
[implementation and performance report](../../docs/built-in_ops/01-numeric/exp-performance.md)
provides scope, accuracy, platform results and profiler limitations.

```sh
mkdir -p build/num04-exp
cmake --build build/kernel-dev --target photospider_numeric_exp_benchmark -j8
python3 oracle/ops/numeric/exp_bound.py
python3 oracle/ops/numeric/exp_oracle.py build/num04-exp/oracle.bin
build/kernel-dev/examples/numeric_workflow/photospider_numeric_exp_benchmark check build/num04-exp/oracle.bin
build/kernel-dev/examples/numeric_workflow/photospider_numeric_exp_benchmark --timing-scopes
python3 examples/numeric_workflow/exp_measure.py build/kernel-dev/examples/numeric_workflow/photospider_numeric_exp_benchmark build/num04-exp/timings.csv
```

The corpus generator requires independent MPFR 4.2+. Its acceptance executable
checks each input in six partitions, enforces the four-step bound, and checks
layout, floating-environment, resource, and cancellation behavior. A recent
Result-path run passed all 20,503 cases across six partitions with a maximum
distance of two steps. The benchmark is a manual target available with
`BUILD_TESTING=ON`; its `public` and `core` layers execute a Result workflow.
Append a Linux CPU number to `exp_measure.py` for `taskset` affinity. The
maintained driver measures the production IQK implementation only. The example
`photospider_numeric_exp_benchmark public 262144 10 7` measures seven public
runs after one warmup.

The timing fixture uses a private `Value` only to create immutable numeric
backing. It publishes that backing as a source tensor Result under the execution
Root, compiles the benchmark workflow once per input schema, and freezes a fresh
binding for each new source Result. The Result cache is disabled and the
workflow has one CPU worker. `public` times `Workflow::run`, including its
`ExecutionContext::execute` call, coordinator work, continuation factory and
initial Need, digest calculation, host publication, and the small wrapper that
reads Root statistics and extracts the `ResultRef`. Benchmark operation
definition creation and registration, source construction/publication, compile
and static preparation, freeze, and output readback are outside that timer.
`core` measures only `ResultContinuation::poll` on a CPU worker phase that has
supplied tensors. It includes the checked `consume_work` observer and Result
publication, and excludes continuation factory setup, initial Need, coordinator
work, source admission, compile/freeze, and readback. `raw` retains the
preallocated SIMD kernel timer. The public layer reports peak Root Payload,
including Root-owned state, scratch, and output; immutable caller input storage
is accounted as Referenced. This is managed Root accounting, not process RSS. Core and raw do
not report a Payload peak.

The exp A/B tables in the linked report retain their original Value/callback
timing boundaries; they are historical and are not same-scope comparisons with
the current Result workflow and computation-poll layers. A small smoke set of
21 public/core/raw runs across exp and the six trig functions completed, using
unaligned public input, reverse core input, and a 65-sample tail. It checks that
the current paths run and report valid timings; it is not a performance campaign
or a platform matrix. `--timing-scopes` reports each executable's timer
boundary, and the CSV drivers append it to every timing row.
The SLEEF comparison backends were removed after the recorded A/B evaluation.
`exp64_oracle.py UNARY_EXECUTABLE [strict|apple|x86]` separately checks Float64
exp against independent MPFR, including admission boundaries, strict special /
range handling and inputs that cannot be represented in Float32.

`photospider_numeric_math_batch` runs seven certified math operations (`exp`,
`ln`, `sin`, `cos`, `tan`, `pow`, and `atan2`) with Float32 and Float64 Result
inputs. The fixture creates raw descriptor/layout byte buffers, publishes them
as tensor Results under the execution Root, then compiles and freezes a workflow
with Result bindings. A private `CertifiedMath` instance supplies the scalar
oracle. The checks compare exact output bits across scalar and Result execution,
unaligned, reverse, zero-stride, and transposed layouts, and partitions of 1,
3, 7, 64, and 65 samples. They also cover special values, caller floating-point
environment restoration, resource thresholds, pre-cancellation, and reading a
retained output after the execution context retires.

The floating-point environment checks set and inspect the calling thread's
state around `execute`; the math operation runs on the CPU worker. The resource
checks first measure one complete cold execution, then repeat with the observed
work and Payload limits and with each limit reduced by one. The exact observed
limits are fixture-specific.

The scalar oracle uses private arithmetic headers and `photospider_test_kernel`,
so this executable is not a standalone installed-package consumer. Its workflow
still uses the public Result execution API. The executable checks whether its
architecture-selected math profile is available before running; if unavailable,
it returns 77, which CTest treats as a skipped test.

Build and run the focused executable with:

```sh
cmake --build build/kernel-dev --target photospider_numeric_math_batch -j8
build/kernel-dev/examples/numeric_workflow/photospider_numeric_math_batch
build/kernel-dev/examples/numeric_workflow/photospider_numeric_math_batch time 262144
ctest --test-dir build/kernel-dev -R '^test_numeric_math_batch$' --output-on-failure
```

The `time` mode writes `scalar_math_us` and `result_workflow_us`. It times the
scalar `CertifiedMath::evaluate` loop and, separately, `ExecutionContext::execute`
on a frozen Result workflow. Source construction, compilation, and freezing are
outside the workflow timer; output readback and bit comparison are outside both
timers. The workflow uses one CPU worker and disables the Result cache. These
measurements are not callback medians and are not directly comparable with the
historical core/callback measurements in [the NUM/CRV batch report](../../docs/built-in_ops/01-numeric/batch-performance.md).
The [packed adapter and FP64 exp update](../../docs/built-in_ops/01-numeric/adapter-performance.md)
records the newer CPU-instruction reductions, Apple/FreeBSD paired measurements,
and added exp/binary64/multidimensional-layout checks.

`trig_bound.py` analytically certifies the actual SIMD coefficients and rounding
graph. `trig_oracle.py OUTPUT_DIRECTORY` generates independent MPFR corpora;
`photospider_numeric_trig_benchmark check FUNCTION CORPUS` checks each corpus
through six batch partitions, layouts, floating modes and resource/cancellation
fixtures. `trig_measure.py CURRENT [BASELINE]` records the timer boundary
reported by each executable in its `timing_scope` CSV column. An older
executable that does not support `--timing-scopes` is marked `unknown`; rows
with unknown or different scopes are not directly comparable. The current
`core` scope differs from the historical callback timer, so do not use an old
baseline's core row as a paired comparison. See [the trig report](../../docs/built-in_ops/01-numeric/trig-performance.md).

`freebsd_analysis.py CURRENT_TRIG BASELINE_TRIG EXP [OUTPUT]` also writes a
`timing_scope` field for every row. Historical baseline rows whose executable
does not advertise a scope are marked `unknown`; use only matching known scopes
for comparisons. To reproduce the historical FreeBSD campaign, build both
executables with Clang 22 and the same C++ runtime; this does not make different
timer scopes comparable. The command is:

```sh
cpuset -l 2 python3.12 examples/numeric_workflow/freebsd_analysis.py \
  build/examples/numeric_workflow/photospider_numeric_trig_benchmark \
  baseline-build/examples/numeric_workflow/photospider_numeric_trig_benchmark \
  build/examples/numeric_workflow/photospider_numeric_exp_benchmark timing-final.csv
```

The trig timing CLI accepts `FUNCTION LAYER N SPAN REPETITIONS
[profile|measure] [normal|mixed|outside|landmark|tiny] [LAYOUT]`. Layout 0/1/2/3
selects unaligned/reverse/broadcast/dense. Both current `public` and `core`
layers support these input layouts; `raw` requires dense normal-domain input.
`profile` retains a warmup output check and skips repeated checks for sampling;
use `measure` for latency campaigns. `photospider_numeric_math_batch time [N]`
selects the Result workflow array size.
The signal benchmark accepts `PROFILE FILTER [float32|float64] [REPETITIONS]`;
the dtype option applies only to inverse output, while lowpass remains Float64.

See [the full scoped FreeBSD analysis](../../docs/built-in_ops/01-numeric/freebsd-full-analysis.md)
for the 721 timing rows, additional CRV commands/artifacts, profiling boundaries,
correctness checks and observed regressions. `freebsd_plot.py DATA_DIR OUTPUT.png`
generates a PNG and SVG overview from these CSVs and requires optional matplotlib.
