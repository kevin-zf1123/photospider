# FMT-12/13/18 and configured-view implementation

This record covers the implemented ICC, OCIO and softproof members and the
configured part of FMT-15C. Specification decision status remains **Proposed**;
the enumerated active members have executable implementations. Native gamut,
tone/display arithmetic and the other partial-member fixes are recorded in
[FMT-14/15 implementation](FMT-14-15_implementation.md).

## Implemented surface

| Member | Registered CPU operation key | Output |
| --- | --- | --- |
| FMT-12A | `color.icc_transform_lcms2_19_1_cpu` | `values` |
| FMT-12B | `color.icc_apply_devicelink_lcms2_19_1_cpu` | `values` |
| FMT-12C | `color.icc_transform_chain_lcms2_19_1_cpu` | `values` |
| FMT-13A | `color.ocio_convert_space_2_5_2_cpu` | `values` |
| FMT-13B | `color.ocio_display_view_2_5_2_cpu` | `values` |
| FMT-13C | `color.ocio_looks_2_5_2_cpu` | `values` |
| FMT-13D | `color.ocio_named_transform_2_5_2_cpu` | `values` |
| FMT-13E | `color.ocio_file_transform_2_5_2_cpu` | `values` |
| FMT-13F | `color.ocio_transform_tree_2_5_2_cpu` | `values` |
| FMT-18A | `color.softproof_lcms2_19_1_cpu` | `values` |
| FMT-18B | `color.gamut_alarm_lcms2_19_1_source_dims_v1_cpu` | `mask` |

All eleven use fixed invocation ABI 8, CPU Host placement, one input Tensor,
Float32/Float64, full sample rank 1..8 and at most 2^40 samples. Integers require
explicit prior conversion. `auto` and `materialize` produce independent dense
output tiles; forced `view` is rejected. No new GPU or native strict/accelerated
alias is introduced for external engines.

The implementation is in
[icc_operations.cpp](../../../plugins/ops/02-format-color/icc_operations.cpp),
[ocio_operations.cpp](../../../plugins/ops/02-format-color/ocio_operations.cpp),
[icc_engine.c](../../../plugins/ops/02-format-color/icc_engine.c) and
[ocio_engine.cpp](../../../plugins/ops/02-format-color/ocio_engine.cpp).
The public helpers are in
[external_color.hpp](../../../plugins/ops/include/photospider/ops/format/external_color.hpp)
and [view.hpp](../../../plugins/ops/include/photospider/ops/format/view.hpp),
also exported by `photospider/ops.hpp`.

### Static configuration and coordinates

A described invocation supplies `input_description` and a complete `group`;
a raw invocation supplies an explicit cell-relative `axis` and ordered
`components`. Axis-free Gray omits both. Raw coordinates still obey finite
engine-input and unit-conversion rules. No operation discovers color meaning
from the input Result or a filename.

ICC admits v2/v4 ordinary input/display/output/color-space profiles, DeviceLink
for B and forward internal abstract stages for C. Endpoints cover Gray, RGB,
CMYK, XYZ and CIELAB. `icc_chain` seals 2..255 ordered stages with bytes,
direction and applicable intent/BPC. Actual linking checks the declared
directions. B requires complete forward `entry_space`/`exit_space` assertions;
C requires them at DeviceLink endpoints. The profile importer performs
structural admission; every operation additionally builds the required CMM
transform, even if no pixel will be executed.

ICC public RGB/Gray/CMYK coordinates use the family nominal 0..1 scale; XYZ
uses relative white Y=1; CIELAB uses l=L*/100 and unchanged a*/b*. CMYK is
multiplied by 100 and Lab l by 100 at the double-formatter boundary, with the
inverse adaptation on output. Explicit percentage, L*=0..100, code-value or
absolute-nits unit declarations are rejected until the caller converts them.
Empty described component units inherit the native scale; `1`/`relative` and
the matching Lab `a*`/`b*` roles are accepted. Semantic CMYK checks [0,1];
finite raw CMYK permits extensions without an adapter clamp.

Both public dtypes use LCMS double formatters. This does not imply binary64
arithmetic throughout the LCMS pipeline: its actual Float32/table stages and
formatter narrowing remain observable. ICC `execution` is `optimized` by
default or `reference` where exposed. FMT-18B has a single fixed algorithm
and no execution/threshold/alarm-color parameter.

OCIO always executes its pinned F32 CPU processor. Float64 input explicitly
rounds once to binary32 under the representability premise, then widens the F32 result
exactly. `reference` uses OPTIMIZATION_NONE and `optimized` uses the pinned
OPTIMIZATION_DEFAULT. Identity/equality/data routes still pass through this
numerical boundary.

OCIO A-D require a config snapshot; E/F may use an explicit empty resolver if
there are no symbolic references. D/E/F require complete forward entry/exit
assertions; inverse swaps their effective meanings. D/E/F labels are caller
provenance, not colorimetry inferred from a LUT or NamedTransform. A/B/C validate
canonical space/role/alias resolution against the actual engine.

Public FileTransform interpolation is validated against the selected reader.
The pinned nearest request is rejected because its CPU implementation uses
linear interpolation; a concrete tetrahedral request for a 1D LUT is likewise
rejected. Default/best select the pinned effective method, which inspection
records. Interpolation inside imported configs/files retains the pinned
reader's interpretation. No stricter public-authoring rule is retroactively
applied to those imported declarations.

### Alpha, demand and channel replacement

Any requested color consumes its complete source color group at that point.
All colors in retained output tiles are computed and written under caller
numerical premises, without a result-validation pass. Bypass-only alpha/AOV
requests neither read color samples nor call the pixel engine, but still require
complete static engine construction. Alpha and AOVs are copied as raw bits,
including NaN payloads and signed zero; hidden straight color is not suppressed.

Same-count conversion preserves the physical selected slots. Count-changing
ICC conversion replaces the selected group at its smallest source slot and
retains unselected channels in order. For RGB slots [5,1,3] in seven channels,
conversion to CMYK produces [old0,C,M,Y,K,old2,old4,old6]. Same-count XYZ instead
uses slots [5,1,3]. Axis-free Gray expansion requires explicit `output_axis`.

Lowering declares the exact source-tile union for each retained output tile.
Execution uses prepared fixed-size coordinate/scratch state and batches of at
most 64 color points. No complete-input image scan or hidden tile-size policy is
introduced. FMT-18B emits only a mask, with the family `keepdims` rules.

## Construction, budgets and lifetime

**Engine construction is synchronous and noncancellable.** The 2026-10-21
contract revision removes construction cancellation requirements and probes
from parsing, linking, BPC, LUT inversion and gamut-grid generation. It does not
remove allocation/work admission, full Empty/bypass validation or cleanup.

There are two deliberately distinct phases:

1. Resource validation, authoring inspection and configured-view validation
   construct and destroy a complete engine using independent positive Int64
   `construction_bytes` and `construction_work` limits. Defaults are 268435456
   bytes and 4294967296 work units. The byte limit applies to Host, Metadata
   and Payload resource capacity; a work unit is an instrumented construction
   admission, not a wall-clock duration.
2. Prepared-kernel construction uses the existing ResourceAllocationScope
   metadata-budget bridge and retains the resulting engine under the caller's
   execution budget. Direct preparation without that bridge uses the explicit
   construction limits. Validation is not misrepresented as a charge against
   an execution budget that is absent at its call site.

Each prepared kernel owns an engine and mutex; concurrent reuse serializes
calls to that instance, while separate kernels own independent contexts.
Immutable resource bytes may be shared normally. No mutable resource-dependent
engine cache is installed in shared static operation state. Multiple prepared
tiles may therefore construct separate engines. Reusing a prepared plan
amortizes construction over runs; there is no hidden global cache or automatic
eviction.

Private malloc/new hooks account payload, allocation headers and alignment
padding until destruction. C construction work refusal uses a C-only unwind
boundary; it never crosses C++ destructors. The tracker reclaims partial orphan
allocations after normal object cleanup. C++ construction uses ordinary
exception unwinding, and its work probes do not throw during cleanup. A
first-failure latch rejects an apparently successful engine returned after an
internal allocation/work fallback. Private contexts and closed memory streams
avoid abandoned file or mutex ownership in these failure paths.

Bounded process-lifetime OCIO registries, yaml-cpp lexer/emitter expressions and finite C++ exception-control bookkeeping are runtime bootstrap overhead, separate from per-build engine payload. A synchronized bootstrap initializes every pinned YAML expression before enabling an invocation allocator, so anchor/alias and other optional syntax cannot bind static storage to a temporary engine owner. The private runtime prevents engine std::vector/string/stream storage from escaping through a process-wide allocator replacement.

Pixel cancellation remains checked around 64-point batches and bounded output
writes. Existing immutable-resource import APIs retain their independent
cancellation-token behavior. Native arithmetic refinement retains its own
cancellation contract. No frozen kernel/public kernel ABI changes were needed.

| Failure | Observable status |
| --- | --- |
| Unsupported dtype/group/layout structure | TypeMismatch, or InvalidArgument for a forbidden explicit option |
| Invalid profile, dependency, endpoint, inverse or alpha effect | InvalidArgument / InvalidDomain |
| Construction capacity/work refusal | ResourceExhausted with the original reason |
| Consumed values, semantic CMYK range, formatter/unit narrowing and requested results | Unchecked caller numerical premises; no host domain/overflow diagnostic |
| Actual private-engine execution failure | OperationFailed; original resource/cancellation failures retain their codes |
| Cancelled pixel execution | Cancelled |

Unpublished writes and partial engine state are discarded on failure.
Published results retain their data-storage owners independently of engine
lifetime. Engine resources are not attached to output values.

## Pinned engines and closed resources

[ColorEngines.cmake](../../../cmake/ColorEngines.cmake) and
[ColorOCIO.cmake](../../../cmake/ColorOCIO.cmake) pin source archives by SHA-256:
Little CMS 2.19.1, OpenColorIO 2.5.2, yaml-cpp 0.8.0, expat 2.7.2, pystring 1.1.4,
Imath 3.2.1, minizip-ng 4.0.10 and sse2neon revision
`227cc413fb2d50b2a10073087be96b59d5364aea`. The CMake files contain complete URLs
and hashes; upstream licenses are included in the installation rules.

The engine is a private shared library with a small C ABI. OCIO's C++ namespace,
static libc++ and static libc++abi remain private; no C++ object crosses its ABI.
Building requires a compatible Clang/LLVM toolchain, `llvm-config`,
`llvm-objcopy`, libc++ headers/static archive, libc++abi archive and runtime
license. The validated runtime was LLVM 23.1.2. The build has explicit cache
variables for these locations; it does not change the kernel's C++ runtime.

Color-engine configuration requires CMake 3.24 or newer and Python 3. FetchContent downloads the pinned archives by default. For offline configuration, extract the matching archives locally and supply `FETCHCONTENT_SOURCE_DIR_PHOTOSPIDER_LCMS_SOURCE` plus `FETCHCONTENT_SOURCE_DIR_PS_COLOR_<DEPENDENCY>` for `YAML`, `EXPAT`, `PYSTRING`, `IMATH`, `MINIZIP`, `SSE2NEON` and `OCIO`. Each value names the extracted source root. Runtime locations can be supplied through `PS_COLOR_LLVM_CONFIG`, `PS_COLOR_OBJCOPY`, `PS_COLOR_LIBCXX_ARCHIVE`, `PS_COLOR_LIBCXX_HEADERS`, `PS_COLOR_LIBCXXABI_ARCHIVE` and `PS_COLOR_RUNTIME_LICENSE`. The engine library remains a required dependency of bundled operations; disabling `PHOTOSPIDER_BUILD_OPS` leaves the kernel and optional operations SDK independent of these engines.

Target-local `-fno-fast-math -ffp-contract=off` apply to the bridge and private
dependencies. On the measured ARM64 build, OCIO's SSE-family implementations use
sse2neon; AVX, AVX2, AVX512 and F16C are disabled. No cross-platform bit-equality
or unmeasured GPU support is claimed.

`external_color_engine_identity()` returns a versioned identity containing both
engine releases and a digest of integration sources, CMake configuration,
private runtime archives, compiler/platform/math options and actual SIMD
settings. `ocio_config` incorporates that identity into the immutable manifest.
Changing an adapter/build requires creating a matching snapshot rather than
silently treating an older snapshot as the same execution identity.

OCIO snapshots own config bytes, normalized logical file names and bytes,
explicit frozen context, and declared canonical spaces/reference types. They
do not read the working directory, host files, process environment, current
OCIO config or ambient optimization flags. Global path/hash/file caches and
processor caches are disabled for this private path. A same-name file in
another snapshot does not become this invocation's resource.

The selected graph is resolved before CPU optimization. Primitive transforms
are cloned, finite/static parameters audited and all admitted dynamic
properties frozen. Matrix alpha coupling/changes are rejected, even if another
matrix later cancels the change. Range and admitted fixed-function renderers
preserve alpha; exponent modes that modify alpha are rejected. Bounds include
depth 64, 4096 resolved stages, 1D LUT length 2^20 and 3D grid size 129. These are
checked admission limits, not promises that every such construction fits a
particular budget.

Reference-space policy is checked on logical scene/display crossings before
equality-group or data elision. Missing dependencies may trigger only the
pinned allowed Look-file alternative; malformed data and host resource refusal
do not become a successful fallback.

### Softproof and independent gamut alarm

FMT-18A constructs the four-profile route source → proof → proof → display.
Rendering intent/BPC apply to the specified first two stages; the terminal
proofing intent is explicitly relative or absolute. This preserves the family's
LCMS recipe rather than adding a second project tone map.

FMT-18B constructs its owning check transform with a Lab4 terminal and borrows
the independent source-domain gamut pipeline while that transform lives.
The integration corrects stock cross-channel-count dimensions to source inputs
and one output: Gray 49, RGB 49³, CMYK 23⁴ grid points. It retains actual sampler-completion checks and failure latching. Generated
Lab/ΔE/ratio numerical conditions are unchecked; no extra finite-value checks
are injected before integer conversion.
Input passes through the actual float formatter; finite score >0 becomes
exact 1, otherwise exact +0. There is no display alarm-color comparison.

## Public authoring and inspection

`ColorResourceBinding` contains the immutable owner to add to ResourceSet and
the encoded parameter value to put on a node. Neither field is a mutable path.

| Helper | Purpose |
| --- | --- |
| `icc_profile_parameter(profile)` | Profile identity plus verified endpoint/header assertions |
| `icc_chain(stages, budget)` | Owned ordered profile-chain resource |
| `ocio_config(snapshot, budget)` | Closed config with this build's engine/settings identity |
| `ocio_file(logical_name, immutable_bytes)` | Standalone frozen LUT/CLF/CTF binding |
| `ocio_transform_tree(yaml, budget)` | Frozen declarative transform/tree resource |
| `color_endpoint_parameter(interpretation, units)` | Complete ordered endpoint assertion |
| `inspect_external_color(key, input, parameters, resources)` | Actual synchronous engine construction and owned diagnostic text |

Inspection validates the same published parameter schema, then records actual
ICC stages/tags/routes and settings, or OCIO canonical endpoints, selected Look
candidate, logical resource/file reader, effective interpolation, frozen
primitive values and CPU cache identity. This human-readable text is not a
stable serialization protocol. Engines are destroyed before the owned string
is returned. Failed inspection does not modify input metadata or resources.

For example, given an actual source input declaration and explicitly imported
profiles, the following public graph fragment applies FMT-12A:

```cpp
auto resources = take(ps::ResourceSet::create(
    {source_profile.resource(), destination_profile.resource()}, budget));
std::map<std::string, ps::ParameterValue> parameters{
    {"axis", std::int64_t{1}},
    {"components", std::string("0,1,2")},
    {"source_profile", ps::format::icc_profile_parameter(source_profile)},
    {"destination_profile",
     ps::format::icc_profile_parameter(destination_profile)},
    {"intent", std::string("relative_colorimetric")},
    {"bpc", std::string("off")},
    {"execution", std::string("optimized")},
};
document.nodes.push_back(
    {1, "color.icc_transform_lcms2_19_1_cpu",
     {ps::WorkflowInputReference{1}}, parameters});
document.outputs.push_back({"result", 1, "values"});
ps::GraphContext graph(document); // Retain through plan execution.
auto compiled = take(ps::Compiler(registry).compile(graph, planning, resources));
auto prepared = take(execution.prepare(*compiled.plan));
auto run = take(execution.submit(prepared, {{source_binding}}));
auto result = take(run.join());
```

Here `take` unwraps a successful Result and reports its Status otherwise.
The fragment uses raw native RGB coordinates in a rank-two tensor with channel
axis 1; a described invocation replaces the raw selectors with its explicit
description/group. It does not assign a color description to the output Result.

For an OCIO A node, import a snapshot with actual bytes and use:

```cpp
ps::OcioConfigSnapshot snapshot;
snapshot.config = config_bytes;
snapshot.files["show/look.spi1d"] = lut_bytes;
snapshot.context["SHOT"] = "shot_010"; // Frozen caller value.
snapshot.spaces = {{"working", "scene"}, {"rendered", "display"}};
auto config = take(ps::format::ocio_config(snapshot, budget));
auto resources = take(ps::ResourceSet::create({config.resource()}, budget));
std::map<std::string, ps::ParameterValue> parameters{
    {"axis", std::int64_t{1}},
    {"components", std::string("0,1,2")},
    {"config", ps::format::color_resource_parameter(config.resource())},
    {"source_space", std::string("name:working")},
    {"destination_space", std::string("name:rendered")},
    {"reference_bridge", std::string("config_default")},
};
auto trace = take(ps::format::inspect_external_color(
    "color.ocio_convert_space_2_5_2_cpu",
    ps::OperationMetadata{input_schema}, parameters, resources));
```

This fragment requires those spaces and a usable explicit default bridge in the
supplied config. Names alone do not synthesize a bridge or a display.

### Configured FMT-15C

Resource-aware `compose_view` overloads take ResourceSet and either
NativeDisplayConditions or ConfiguredDisplayConditions. They validate actual
registry inference and actual pinned OCIO construction for every stage before
atomically replacing the document. Forward/inverse assertions are checked in
the effective direction; conflicting preexisting endpoint parameters fail.
Selected-channel overrides are rebuilt, while unrelated groups, shared alpha,
AOV interpretations and axes are preserved.

For a fully declared config RGB input/output group named `color`:

```cpp
ps::format::ViewStage stage{
    "color.ocio_display_view_2_5_2_cpu",
    {{"group", std::string("color")},
     {"config", ps::format::color_resource_parameter(config.resource())},
     {"source_space", std::string("role:scene_linear")},
     {"reference_bridge", std::string("config_default")},
     {"display", std::string("monitor")},
     {"view", std::string("film")}},
    entry, exit};
ps::format::ConfiguredDisplayConditions display{
    exit, "color", config.identity(), "monitor", "film", "config_default"};
auto output = ps::format::compose_view(
    document, input_edge, entry, {stage}, display, resources);
```

The descriptions must identify the actual config/canonical spaces and
reference types. Configured display conditions infer no nits, black level or
monitor calibration. A later native stage requires an explicit compatible
analytic binding; it does not infer sRGB/ACES meaning from a configured name.
No extra native tone/transfer stage is automatically appended.

## Independent verification

The retained [oracle sources and build recipe](../../../oracle/ops/format_color/README.md)
link unmodified numerical/parser/renderer sources from the pinned releases.
Reference OCIO edits only CMake packaging/dependency integration; preparation
checks C/C++ files remain byte-identical to the extracted source. It does not
reuse production allocation hooks, closed resolver patches or gamut correction.
The retained build recipe was configured and built independently; its 11 LCMS
and 4 OCIO fixture files reproduce the original reference files byte-for-byte.
The OCIO preparation check verified 707 unchanged C/C++ source/header files.

The final public-workflow comparison passed **204 workflows and 2232
bit-exact oracle rows**, covering all eleven operations, Float32/Float64,
reference/optimized where exposed, and both OCIO directions. Color bits,
bypass NaN payloads and negative-zero bits were checked separately. The ICC
corpus uses ten deterministic synthetic/reopened profiles, five endpoint
models, DeviceLink, internal Lab/XYZ abstract stages, RGB/CMYK proofing and every
allowed intent/BPC combination. Synthetic CMYK is an interface fixture, not a
calibrated printer characterization.

For RGB/RGB gamut, the oracle checks the stock owning pipeline and an
independently expressed sampler. Gray→RGB, CMYK→RGB and RGB→CMYK use a separate
source-domain hull builder with stock helper transforms and stock CLUT
interpolation. The sampler independently expresses the pinned ΔE/threshold
formula; it does not call the production correction.

The final 45 boundary checks and 4 integration checks additionally cover
Empty/bypass-only full construction,
nonzero-origin sparse input, legacy consumed-nonfinite/overflow failures (historical evidence under the former checked contract; no longer required or implemented),
construction memory/work limits, execution cancellation, unsupported
interpolation, selector case/namespaces, closed host-file lookup, dynamic
property freezing, alpha-coupling and compensating-alpha-matrix rejection.
Inspection checks actual routes/readers/Look choice and parameter schema
rejection. Topology checks cover noncanonical RGB slots, count-changing CMYK,
axis-free Gray and mixed OCIO→native view equivalence. Configured display-view
manual equivalence and missing-view/conflicting-endpoint rollback also passed.

C ABI fault injection passed **3388 allocation/work trials and 200 concurrent
constructions**. Cases include ordinary ICC, Gray/CMYK source-domain gamut,
OCIO trees and inverse files. Every failure was followed by a valid normal
construction/application; tracked live allocation count and bytes returned to
zero. Nth allocations were exhaustive for the small ordinary/OCIO cases and
sampled for expensive gamut construction, with additional work-prefix refusal.
This is observed cleanup evidence, not a general sanitizer or whole-engine
formal proof.

Temporary runtime validators, benchmark executables and generated fixtures are
removed after validation under the repository policy. Independent oracle
sources/build recipes and these measured records remain. No installed-package
consumption test or kernel modification is part of this work.
The final photospider_ops build succeeded. All 26 changed/new C/C++ files
passed ClangFormat 21.1.3 dry-run and cpplint 2.0.2; diff whitespace and local
Markdown targets were checked, with no frozen kernel/public kernel file changes.

## Measured optimization

Measurement platform: Apple M5 ARM64, macOS 27.2 beta (26B5101f),
AppleClang 21, RelWithDebInfo `-O2`, private LLVM 23.1.2 C++ runtime.
Measurements use the same immutable fixture/settings/dtype on each comparison.
Profiling and uninstrumented timing are separate.

The public workload is **16384 points, one output tile, one CPU worker,
two warm-ups and nine timed runs**; the table reports median submit-through-join
milliseconds. Compilation includes resource validation; preparation builds
retained engines and is measured separately. These are warm repeated-run
measurements, not cold end-to-end image import timings.

Optimized execution (FMT-18B has one fixed mode):

| Operation | F32 before → after (ms) | F32 reduction | F64 before → after (ms) | F64 reduction |
| --- | ---: | ---: | ---: | ---: |
| FMT-12A profiles | 2.421 → 1.885 | 22.1% | 2.331 → 1.812 | 22.2% |
| FMT-12B DeviceLink | 2.668 → 1.308 | 51.0% | 1.793 → 1.273 | 29.0% |
| FMT-12C chain (3) | 4.453 → 3.514 | 21.1% | 4.285 → 3.429 | 20.0% |
| FMT-18A RGB proof | 4.663 → 3.213 | 31.1% | 4.599 → 3.200 | 30.4% |
| FMT-18B RGB gamut | 1.071 → 0.806 | 24.7% | 0.999 → 0.837 | 16.3% |
| FMT-13A space | 0.945 → 0.753 | 20.4% | 0.866 → 0.721 | 16.8% |
| FMT-13B display/view | 0.904 → 0.755 | 16.5% | 0.853 → 0.731 | 14.4% |
| FMT-13C Looks | 0.970 → 0.757 | 22.0% | 0.908 → 0.710 | 21.8% |
| FMT-13D NamedTransform | 0.938 → 0.751 | 19.9% | 0.897 → 0.735 | 18.1% |
| FMT-13E file | 0.881 → 0.759 | 13.8% | 0.862 → 0.740 | 14.2% |
| FMT-13F tree | 1.274 → 1.045 | 17.9% | 1.170 → 1.060 | 9.4% |

Reference execution, measured with the same corpus and runtime boundary:

| Operation | F32 before → after (ms) | F32 reduction | F64 before → after (ms) | F64 reduction |
| --- | ---: | ---: | ---: | ---: |
| FMT-12A profiles | 2.355 → 1.804 | 23.4% | 2.353 → 1.793 | 23.8% |
| FMT-12B DeviceLink | 1.840 → 1.339 | 27.2% | 1.785 → 1.293 | 27.6% |
| FMT-12C chain (3) | 4.535 → 3.477 | 23.3% | 4.894 → 3.400 | 30.5% |
| FMT-18A RGB proof | 4.628 → 3.177 | 31.4% | 4.585 → 3.210 | 30.0% |
| FMT-13A space | 0.901 → 0.748 | 17.1% | 0.880 → 0.735 | 16.5% |
| FMT-13B display/view | 0.894 → 0.748 | 16.4% | 0.858 → 0.727 | 15.2% |
| FMT-13C Looks | 1.042 → 0.836 | 19.9% | 0.991 → 0.808 | 18.5% |
| FMT-13D NamedTransform | 0.916 → 0.758 | 17.3% | 0.903 → 0.737 | 18.4% |
| FMT-13E file | 0.919 → 0.738 | 19.7% | 0.886 → 0.718 | 19.0% |
| FMT-13F tree | 1.233 → 1.078 | 12.6% | 1.184 → 1.058 | 10.7% |

All 42 mode/dtype benchmark checksums match before/after. The independent
2232-row bit comparison separately checks numerical correctness; a checksum
alone is not treated as a correctness oracle. The baseline already uses the
allocation-free OCIO applyRGB path, so this workflow table measures the later
work-hook and dtype-specialization changes.

Final RGB-gamut compilation measured 87.44–95.48 ms and preparation
88.12–92.57 ms; the warm mask runs above exclude those construction costs.
These one-shot construction measurements are descriptive, not stable
construction-speedup claims.

The measured optimizations are:

- OCIO uses `CPUProcessor::applyRGB` at the normative one-point boundary, avoiding
  the four image-packing allocations of repeated one-pixel PlanarImageDesc
  application. With the same stock processor and 262144 points, two warm-ups and
  nine trials, Planar application measured 26.320584 ms versus 1.225083 ms for
  applyRGB (21.48×); checksums and compared point bits match. This is an
  **engine-API microbenchmark**, not a claimed workflow speedup.
- LCMS construction work checks are inlined, allowing the compiler to hoist
  thread-local lookup and cheaply bypass construction-only work in pixel loops.
  The number of construction admissions and first-failure semantics are
  unchanged. The original ICC trace attributed 20.29% of leaf sample weight to
  the out-of-line work function and 8.13% to TLS address lookup.
- Dtype-specialized external kernels make 4/8-byte reads/writes compile-time
  constants, reducing variable-size copy overhead in gather/scatter while
  preserving arbitrary valid source strides and exact bypass bits.
- Normal successful engine teardown avoids scanning the orphan tracker when no
  live host allocations remain. Failure cleanup still drains the tracker.
- OCIO's pinned ARM SIMD implementation remains enabled; rendering loops do not
  acquire construction work probes. Numeric acceptance remains tied to the
  specified processor/rounding boundaries.

Before the last two hot-path changes, 15-second xctrace Time Profiler recordings
contained 14804 ICC and 12026 OCIO weighted samples. ICC application was 67.68%
inclusive, with ordinary curve/pow work and the avoidable work-check overhead.
OCIO's PointReader was 29.73% inclusive and memmove 17.10% leaf; engine application
was 14.68% inclusive, motivating constant-width gather/scatter. ICC had 29
unknown leaf samples and 14 empty stacks; OCIO had 19 and 41 respectively.
The export does not report a dropped-sample count, so zero loss is not assumed.
Inclusive percentages overlap and are not additive.

Gamut table construction remains materially more expensive than warm mask
evaluation. This cost is part of the specified 49³/23⁴ algorithm; it is fully
admitted even for one requested pixel. Callers should retain prepared plans
when reusing the same resources, and budget separate per-tile engines.
The timing table describes this machine/workload only. Other OS/architectures,
arbitrary real profiles and physical gamut/color quality have not been
established by these measurements.

## Unchecked revision and verification (2026-10-21)

Host input, formatter/narrowing and requested-output numerical checks have been
removed. Static schema/resource and transform-node structure is still prepared;
finite values, coefficient domains and representable arithmetic are caller
premises. ICC ordinary evaluation uses its direct formatter path. Gamut sampling
retains actual completion/error latching and allocation/work accounting without
extra finite Lab/DeltaE/ratio instrumentation. OCIO retains node/alpha structure,
processor construction and actual engine failures without coefficient/LUT scans
or an extra host transform `validate()` call. Private host identities are
`lcms2.19.1.host2`, `ocio2.5.2.host2` and `ps-color-2`; the C function ABI is unchanged.

The existing ops/SDK/private-engine build and C/C++ formatting/lint passed. A
public workflow checked a finite OCIO matrix fixture `(R,G,B)->(2R,G,B/2)` with
exact binary results. Three additional ICC checks passed: ordinary sRGB-to-sRGB
reference evaluation within 1e-4, same-profile gamut classification of legal
in-gamut samples, and propagation of actual construction-work exhaustion. The
588-byte synthetic sRGB fixture was generated for this run; temporary sources,
binaries and profile bytes were removed afterwards. These checks do not extend
the historical performance results below or establish arbitrary-profile color
accuracy. Earlier nonfinite/domain rejection observations describe the former
checked contract and are superseded by the current unchecked premises.
