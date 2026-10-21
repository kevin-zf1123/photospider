# FMT-14/15 implementation and integration boundaries

This page records the current implementation and measured scope. The member
specifications remain **Proposed**; that decision status is independent of
runtime registration. Mathematical definitions remain in the
[FMT-14](op_specs/FMT-14_gamut_mapping_contract.md) and
[FMT-15](op_specs/FMT-15_tone_view_contract.md) contracts.

## Implemented surface

All six native operations are registered in the default bundled registry and
use fixed invocation ABI 8 on CPU Host. They accept one single-tensor Result
with Float32/Float64 samples, full sample rank 1..8 and at most 2^40 samples.
Schema identity, tensor key and batch prefixes are preserved; no color
description is attached to an output.

| Member | Registered key / public helper | Output and behavior |
| --- | --- | --- |
| FMT-14A | `color.clip_rgb_gamut_strict` | `values`; componentwise clipping to [0,1], retaining in-range bits including negative zero. |
| FMT-14B | `color.reduce_oklab_chroma_v1_strict` | `values`; D65 OKLab chroma-ray search with 32 Float32 or 64 Float64 exact dyadic iterations and the specified endpoint/neutral guards. |
| FMT-14C | `color.compress_rgb_chroma_v1_strict` | `values`; exact-Y neutral-ray compression, knee default 0.8. |
| FMT-14D | `color.check_rgb_gamut_strict` | `mask`; same-dtype exact +0/1 out-of-box indicator, with optional `keepdims`. |
| FMT-15A | `color.map_luminance_v1_strict` | `values`; simple or white-extended Reinhard luminance mapping with explicit gain and negative-luminance policy. |
| FMT-15B | `color.convert_display_luminance_strict` | `values`; explicitly directed relative/nits affine conversion with one final rounding. |
| FMT-15C, native stages | `ps::format::compose_view` | Transactional authoring helper; retains the real constituent nodes and their rounding. |
| FMT-15C, configured stages | Resource-aware `ps::format::compose_view` overloads | Validates FMT-13 stages and native or configured display endpoints against explicit immutable resources before committing the graph. |

Implementation files are
[`gamut_tone.cpp`](../../../plugins/ops/02-format-color/gamut_tone.cpp),
[`color_point.hpp`](../../../plugins/ops/02-format-color/color_point.hpp) and
[`view_authoring.cpp`](../../../plugins/ops/02-format-color/view_authoring.cpp).
The helper is declared in
[`photospider/ops/format/view.hpp`](../../../plugins/ops/include/photospider/ops/format/view.hpp)
and included by `photospider/ops.hpp`. Consumers link `Photospider::ops`.
No accelerated key or GPU implementation is added by this work.

## Static configuration

The described path supplies an encoded `input_description` and selects a
complete `group`. A single axis-free Gray component can instead supply its
component description without a group; an explicit axis-free Gray group is
also supported. Global, group and selected-channel interpretations must agree.
RGB roles are resolved independently of physical component order. Spatial
descriptions/selectors must use the structural spatial channel axis.

Native coordinates require linear transfer, a straight-compatible association
and identity numeric encoding. A profile/configured source needs an explicit,
complete analytic binding with matching model, roles, order and units; no
external engine is run to discover such an equivalence. Overlapping selected
color groups are rejected. All geometry and description validation runs before
request lowering, including for Empty or bypass-only observations.

Relative coordinates accept units `relative` or `1`; Gray additionally accepts
`relative_luminance`. Absolute display coordinates accept `cd/m2` or
`cd/m^2`. Canonical references are `scene` and `display`, with
`coordinates.scale` carrying an explicit relative/absolute assertion when
present. Legacy `scene_relative`, `display_relative`,
`display_absolute` and `absolute_display_cd_m2` are normalized consistently;
contradictory scale/unit assertions fail. Semantic Gray must be `linear_y`.
A native view endpoint requires an explicit scale and transfer identity.

The raw path uses a cell-relative Int64 `axis` and a canonical String index
list such as `"0,1,2"`. Axis-free raw Gray supplies neither axis nor components.
FMT-14A/D need no raw geometry. FMT-14B/C require `target_basis`; FMT-15A RGB
also requires it. The basis uses the existing FMT-10 preset or `basis-v1:`
codec. FMT-14B requires exact stored D65 xy; FMT-14C and FMT-15A RGB require
strictly positive exact Y weights. Raw FMT-15A/B additionally require
`model=rgb|gray`, `reference` and `units`. Those raw assertions are not
accepted alongside `input_description`.

| Member | Additional parameters |
| --- | --- |
| FMT-14C | Float64 `knee` in [0,1), default 0.8. |
| FMT-14D | Bool `keepdims`, default false; rank-one input requires true. |
| FMT-15A | `curve=simple|white_extended`, default simple; positive finite Float64 `gain`, default 1; positive finite `white` required only for white_extended; `negative_luminance=reject|signed_extension`, default reject. |
| FMT-15B | Required `direction=relative_to_nits|nits_to_relative`, `black_nits` and `white_nits`, with finite 0 <= black < white. |

Every member accepts `layout=auto|materialize`; both create independent dense
output tiles. Forced view rejects statically. FMT-14D drops the selected channel
axis or retains it with extent one, preserving batch and spatial topology.
All other members preserve shape and bypass alpha/AOV sample bits.

## Strict mathematics and optimization

Finite consumed values are checked even in raw mode. FMT-14A and FMT-15B read
only the requested selected components. FMT-14B/C, FMT-14D and FMT-15A RGB read
the full selected RGB triple for each requested color point; bypass-only work
reads only its own samples. Zero alpha does not remove hidden-color work.

FMT-14B stores the strict RGB-to-XYZ result and strict XYZ-to-OKLab result at
the input dtype before searching. Every candidate rounds scaled a/b, then
OKLab-to-XYZ, then XYZ-to-RGB at the same specified boundaries. The search
retains the accepted candidate itself and uses exact dyadic bounds. It makes
no assumption that the ray is monotone or that the result is a globally maximal
chroma. Required intermediate/result finiteness and representability are
unchecked caller premises. Gamut classification remains part of the mapping
formula; it does not serve as a numerical-domain validator.

FMT-14C and FMT-15A use fixed-capacity rational arithmetic. Static parameters
and geometry are exact stored binary64 rationals. Exact cancellation at Y=0
is retained; intermediate machine overflow does not substitute for the
specified exact expression. FMT-15B compiles a static exact affine row so its
fallback avoids general rational GCD work for every pixel. All non-copy
expressions round once at the prescribed boundary; exact arithmetic zero is
canonical +0.

The measured fast paths preserve strict results:

- Binary64 directed enclosures use exact point parameters and outward bounds
  for exact RGB weights. A Float32 candidate is accepted only when both finite,
  nonzero enclosure endpoints round to the same Float32 bits; ambiguous values
  fall back to exact arithmetic.
- Compression also certifies Y endpoint and below-knee copy branches. These
  branches benefit Float64 as well.
- FMT-14B reuses FMT-10's certified Float32 matrix-row candidate, with exact
  fallback. It does not collapse any RGB/XYZ/OKLab rounding boundary.
- Static white-square, display range and knee complement are prepared once.
  Float64 tone/display arithmetic skips an enclosure that cannot certify a
  unique Float64 value. Float64 tone mapping and OKLab search remain primarily
  exact arithmetic.
- Floating environment guards restore caller state. The target retains
  `-fno-fast-math -frounding-math -ffp-contract=off`. No approximate SIMD
  replacement or new ISA requirement is introduced.

The private compile-time `PHOTOSPIDER_FMT_MAPPING_REFERENCE` switch disables
these mapping shortcuts for the temporary comparison build. It is not a
registered operation option or a public numerical profile. The reference is
the exact implementation developed for these new operations, not an earlier
released implementation.

## Fixed execution, capacity and failures

One CPU task is emitted per complete requested output tile. Immutable channel
spans distinguish transformed components from contiguous bypass ranges. Each
span maps to exact source coordinates, then takes the source grid's tile
closure; the union is deduplicated and bounded by preparation work/record
limits. Extra samples present in retained physical source tiles are not
numerically inspected.

Preparation checks the complete input/output schemas, output region, dtype,
Host domain, exact read slots and declared scratch. Arithmetic state is
placement-constructed in admitted scratch; point evaluation does not allocate
a private per-pixel heap workspace. Clip, mask and bypass-only tasks need no
arithmetic scratch. Prepared coefficients are shared immutably; mutable
arithmetic and outputs belong to the invocation. Fixed-capacity coefficient
imports check bounds before copying, and arithmetic capacity failures stop
before consuming placeholder results.

Cancellation is checked between at most 64 points, at most 256 output writes,
and inside exact arithmetic/refinement/search work. Status and first-failure
reasons are retained. Static configuration failures use InvalidArgument /
InvalidDomain or TypeMismatch for structural errors. Numerical parameter
domains, consumed finite samples, Y>=0 under reject, and finite representable
results are unchecked caller premises. Capacity failures and cancellation retain
their native codes. Successful results remain readable after source/context
teardown; failed calls expose no partial result.

## Native view authoring

`ViewStage` contains the operation identity, applicable parameter map and
complete input/output `TensorDescription` assertions. The ordered list admits
FMT-09 encode/decode, FMT-10 A/B/C and D, FMT-14A/B/C and FMT-15A/B.
The resource-aware overload also admits FMT-13A..F; see the external-engine
implementation linked below for configured endpoint validation. FMT-10D is
spelled `color.convert_linear_rgb` in the authoring list and requires an
explicit `profile`. It expands to A, optional C, then B; it is not a native key.

The helper resolves the real input edge and validates every intermediate
configuration with the registry. Input descriptions are validated before
normalization or indexed access. Equivalence resolves group inheritance and
native unit/reference aliases while retaining unrelated group and explicit
AOV assertions. Wrong parameter variants return a failed Result. Final
`NativeDisplayConditions` require explicit finite black/white luminance and a
complete display interpretation; any display-range or BT.1886 stage must agree,
and PQ display white must be <=10000 nits.

Only after all stages and the final endpoint succeed is the staged document
assigned back. Existing nodes/exports survive all failures. No stages are
reordered, fused or elided. The tested native recipe was tone -> clip -> sRGB
encode, compared bit-for-bit with manually authored nodes. A separate FMT-10D
recipe retained two RGB/XYZ conversion nodes.

For a validated RGBA entry whose complete group is named `image`, this is an
authoring fragment; the caller supplies the actual document, input edge and
entry configuration:

```cpp
#include <photospider/ops.hpp>

auto display_linear = entry;
display_linear.groups[0].interpretation.reference = "display";
auto display_encoded = display_linear;
display_encoded.groups[0].interpretation.transfer = "srgb";
std::vector<ps::format::ViewStage> stages{
    {"color.map_luminance_v1_strict", {{"group", std::string("image")}},
     entry, display_linear},
    {"color.clip_rgb_gamut_strict", {{"group", std::string("image")}},
     display_linear, display_linear},
    {"color.transfer_encode_strict",
     {{"group", std::string("image")}, {"curve", std::string("srgb")}},
     display_linear, display_encoded},
};
ps::format::NativeDisplayConditions display{
    display_encoded, "image", 0.1, 203.0};
auto output = ps::format::compose_view(document, input, entry, stages, display);
```

Here entry uses group-local scene-relative linear sRGB/Rec.709 assertions,
straight association and explicit `coordinates.scale=relative`; the display
numbers are caller choices for this example. This helper performs no monitor
lookup or automatic exposure.

## Verification and measured performance

The retained [independent oracle](../../../oracle/ops/format_color/README.md)
generated **670 cases** covering all six native members and both dtypes.
Integer rounding/Fraction geometry is independent of production code; OKLab's
forward transcendental stage must agree at 180 and 360 decimal digits. This
stability check is evidence for these fixtures, not a universal transcendental
rounding proof. Both exact-reference and optimized builds matched expected
bits/errors.

Temporary public compile/prepare/submit/join checks covered:

- All six native members in Float32/Float64; analytic clip, compression, tone,
  display and mask fixtures; retained outputs after execution-context teardown.
- Sparse R-only clipping with unrequested NaN peers, alpha-only OKLab bypass,
  and, under the former checked contract, a selected OKLab output rejecting a
  nonfinite required peer. That historical rejection is superseded by the
  unchecked numerical premise and is not a current acceptance requirement.
- Axis-free raw and described Gray, including an explicit Gray group;
  rank-one mask `keepdims`; native view/manual equivalence and FMT-10D expansion.
- Atomic rollback for inconsistent transitions, malformed descriptions, wrong
  parameter variants and configured stages supplied without required resources.
  A multi-group view retained unselected RGB groups, shared alpha and AOV bits;
  claimed changes to
  an unselected group or an AOV interpretation were rejected transactionally.
- FMT-05B/C public compositions across all seven copy dtypes: 21 extraction,
  removal and default-opaque workflows, including NaN payloads, negative zero
  and exact Int64 maximum; explicit UInt16 10-bit and reversed UInt8 opaque
  encodings, rejected nonintegral opaque codes, and shared-alpha preservation.
- Low-budget rejection, latched cancellation within exact OKLab arithmetic,
  and cancelled public submissions.

The focused build used an Apple M5, macOS 27.2 beta (26B5101f), AppleClang
21.0.0 (clang-2100.3.34.2) from Xcode-beta, and RelWithDebInfo / O2. Modified
C++ files were formatted with ClangFormat 21.1.3 and checked with cpplint 2.0.2.
No GPU or x86 runtime result is claimed.

### Public workflow: submit through join

| Operation | Dtype | Pixels | Exact reference, ms | Optimized, ms | Reference / optimized |
| --- | --- | ---: | ---: | ---: | ---: |
| FMT-14C compression | f32 | 4096 | 351.447000 | 0.653167 | 538.07x |
| FMT-15A tone | f32 | 4096 | 346.671000 | 0.456250 | 759.83x |
| FMT-15B display | f32 | 4096 | 53.642100 | 0.345791 | 155.13x |
| FMT-14C compression | f64 | 4096 | 414.306000 | 309.597000 | 1.34x |
| FMT-15A tone | f64 | 4096 | 482.205000 | 479.557000 | 1.01x |
| FMT-15B display | f64 | 4096 | 59.846200 | 8.224420 | 7.28x |

### Arithmetic core only

| Operation | Dtype | Pixels | Exact reference, ms | Optimized, ms | Reference / optimized |
| --- | --- | ---: | ---: | ---: | ---: |
| FMT-14C compression | f32 | 1024 | 44.749900 | 0.174666 | 256.20x |
| FMT-15A tone | f32 | 1024 | 55.468200 | 0.048500 | 1143.67x |
| FMT-15B display | f32 | 1024 | 9.973250 | 0.024125 | 413.40x |
| FMT-14B OKLab reduction | f32 | 64 | 7.688500 | 1.674630 | 4.59x |
| FMT-14C compression | f64 | 1024 | 54.105700 | 39.905900 | 1.36x |
| FMT-15A tone | f64 | 1024 | 84.632500 | 84.476600 | 1.00x |
| FMT-15B display | f64 | 1024 | 10.629600 | 1.807750 | 5.88x |
| FMT-14B OKLab reduction | f64 | 64 | 398.193000 | 399.331000 | 1.00x |

Each table entry is the median of three consecutive comparison campaigns,
each reporting the median of five runs. Checksums agree across both variants
and all three campaigns. Ratios near 1 are not evidence of an improvement;
Float64 tone mapping and OKLab reduction remain essentially unchanged.
These measurements describe this machine and workload, not an application-wide
or cross-platform speed guarantee.


Instruments CPU Profiler was recorded separately from elapsed-time benchmarks.
The initial exact path spent 98.49% of sampled stacks in RationalMath; GCD,
integer division and copying dominated leaves. After enclosure optimization,
the sampled workload shifted toward enclosure evaluation (FastInterval 44.28%
inclusive) and retained RationalMath 26.13% inclusive. The two profile recordings
used different repetition counts to collect enough samples; their wall times
are not compared. Exported stacks had no unknown leaf symbols; no explicit
lost-sample counter was available, so zero sampling loss is not asserted.
Subsequent changes cached static intervals, compiled the display affine row
and reused certified Float32 matrix rows.

The public workflow benchmark prepares the graph and input once, then times
submit-through-join with one CPU worker, one warm-up and five measured runs per campaign. Three
consecutive reference/optimized campaigns were recorded.
It uses a 64x64 RGBA image, tile extent 64, deterministic seed 14015 and the same
signed/HDR RGB samples in both builds. Hashing every output sample occurs
outside the timer. The arithmetic benchmark uses 1024 RGB triples, or 64 for
OKLab search, with the same seed and five repetitions. Corresponding complete
checksums agree. Neither benchmark measures compilation, input ingestion,
disk I/O or a full application.

The temporary validation source, binaries and generated fixture data are
removed after validation, as required by the repository's operator-test policy.
The oracle generator and these workload definitions are retained. The actual
verification commands used the reused build directory:

```sh
cmake --build build/ops-dev --target photospider_ops -j6
build/ops-dev/format_validation integration
build/ops-dev/format_validation boundary
build/ops-dev/format_validation oracle build/ops-dev/format-oracles.txt
build/ops-dev/format_validation_reference oracle build/ops-dev/format-oracles.txt
build/ops-dev/format_validation_reference bench 1024 5
build/ops-dev/format_validation bench 1024 5
build/ops-dev/format_validation_reference workflow_bench 5
build/ops-dev/format_validation workflow_bench 5
```

## External engine and configured-view integration

FMT-12A/B/C, FMT-13A..F and FMT-18A/B are implemented as eleven fixed ABI 8 CPU
operations. FMT-15C has resource-aware overloads for mixed native/OCIO recipes
and native or configured final display conditions. Its actual constituent
operations are constructed and validated before the staged document commits.

External-engine construction is synchronous and noncancellable, as requested
in the 2026-10-21 contract revision. Memory/work admission, first-failure
latching and complete Empty/bypass resource validation remain mandatory.
Pixel execution remains cancellable. This uses the existing preparation
budget bridge without changing the frozen kernel or invocation ABI.

See [FMT-12/13/18 implementation](FMT-12-13-18_implementation.md) for pinned
engines and private runtime integration, closed snapshots, independent numerical
oracles, failure injection, construction inspection, performance and configured
view examples. The native measurements above remain separate evidence.

## Unchecked revision verification (2026-10-21)

Finite input/intermediate/output conditions, positive weights, gain/white/knee
ranges and display-luminance geometry are unchecked caller premises, including
when parameters are available at preparation. The retained `reject` spelling for
negative luminance selects the nonnegative domain; it does not add a validator.
Explicit clipping, signed extension, gamut tests used by the mapping/search,
exact rounding and numerical refinement remain the algorithms themselves.

A temporary public workflow verified RGB clip and in-range signed-zero copying,
axis-free Gray simple tone mapping, its negative signed extension, and the
relative-to-nits mapping with black=1 and white=101. The same 33-check program
covered integer/float format clipping, ties-even and NaN payload handling. The
ops/SDK/private-engine build and modified-source formatting/lint passed. The
program was removed after execution; no new performance or other-platform claim
is made by these focused checks. Historical sample-rejection checks above are
superseded by the unchecked contract.
