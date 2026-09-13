# Local Navier-Stokes inpaint workflow (PNT-05A)

This example uses only installed public headers and `Photospider::kernel`. It
builds immutable opaque linear RGBA and binary coverage inputs, runs one
registered PNT-05A operation with the required Int64 `radius`, and inspects the
named `image` result. It never links OpenCV, so it also runs unchanged in a
native-only build.

| Operation key | Boundary |
| --- | --- |
| `image.local_inpaint_navier_stokes_openCV` | pinned OpenCV 4.12.0 `cv::inpaint(..., INPAINT_NS)` on three Float32 planes |
| `image.local_inpaint_navier_stokes_native_apple_silicon` | licensed standalone port, no OpenCV header, symbol or linkage |

Both publish only the named output `image` and require Int64 `radius` in
`[1,32]`. The unsuffixed `image.local_inpaint_navier_stokes` is the semantic
family name, not a third registration.

The operator contract, resource model, cancellation cadence and the corrected
variant boundaries are documented in
[`Image-Operations.md`](../../docs/kernel-architecture/Image-Operations.md).

## Build, run and check

```sh
cmake -S . -B build/inpaint-ns -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DBUILD_TESTING=ON -DPHOTOSPIDER_ENABLE_METAL=OFF -DBUILD_SHARED_LIBS=OFF \
  -DCMAKE_OSX_ARCHITECTURES=arm64
cmake --build build/inpaint-ns --target test_local_inpaint_navier_stokes \
  photospider_inpaint_ns_workflow -j 3
./build/inpaint-ns/examples/inpaint_ns_workflow/photospider_inpaint_ns_workflow
ctest --test-dir build/inpaint-ns \
  -R '^(test_local_inpaint_navier_stokes|example_inpaint_ns_workflow)$' \
  --output-on-failure
```

`-DCMAKE_OSX_ARCHITECTURES=arm64` is required: this shell runs translated
(`x86_64`), while the OpenCV 4.12.0 builds here are arm64-only, and
`Photospider::kernel` must be a genuine Apple Silicon build. The adapter and the
oracle link whatever `pkg-config opencv4` resolves; the measurements below use
the spec 0.3.1 bound `opencv-strict` reference via `PKG_CONFIG_PATH`. `--key
<operation key>` selects one variant and `--radius <1..32>` one radius.

Actual output of the run above:

```text
PNT-05A local_inpaint_navier_stokes radius=3 registry_keys=70
[image.local_inpaint_navier_stokes_native_apple_silicon] plan_digest=8c9fbffd91f38143 steps=1 peak_live_bytes=4484
[image.local_inpaint_navier_stokes_native_apple_silicon] descriptor=Float32 shape=9,9,4, region=0:9,0:9,0:4, byte_offset=0 strides=144,16,4, facets=1 photospider.image@2 payload_bytes=250
[image.local_inpaint_navier_stokes_native_apple_silicon] preserved_samples=315 hole_samples=9 hole_finite=yes hole_value=0.335325,0.204942,0.359172
[image.local_inpaint_navier_stokes_native_apple_silicon] host_scratch_model=3188 bytes
[image.local_inpaint_navier_stokes_openCV] plan_digest=0a2906c73e34c0de steps=1 peak_live_bytes=2025
[image.local_inpaint_navier_stokes_openCV] descriptor=Float32 shape=9,9,4, region=0:9,0:9,0:4, byte_offset=0 strides=144,16,4, facets=1 photospider.image@2 payload_bytes=250
[image.local_inpaint_navier_stokes_openCV] preserved_samples=315 hole_samples=9 hole_finite=yes hole_value=0.335325,0.204942,0.359172
[image.local_inpaint_navier_stokes_openCV] host_scratch_model=729 bytes external_estimate=4719 bytes
```

Both variants preserve all 315 unmasked and alpha samples bit-for-bit and agree
on every hole sample here. The `external_estimate` line declares the pinned
library's untracked footprint of one channel call (7 bytes per padded sample for
the guard grid plus up to 32 bytes per padded sample for the inserting-order
heap vector); those bytes are never charged to the host execution budget and the
observed maximum-RSS increase is printed next to it when the platform reports
`getrusage`.

## Acceptance evidence

`tests/integration/test_local_inpaint_navier_stokes.cpp` is the single source of
in-tree and installed-consumer evidence. It is rebuilt unchanged against an
isolated installation of the kernel.

| ID | In-tree coverage (all rows executed, exit 0) |
| --- | --- |
| T01 | `zero_mask_identity`: zero and signed-zero mask returns the complete input bits; invalid radius, `0.5` coverage, nonopaque alpha, infinite RGB and a non-premultiplied profile all fail before the noop shortcut |
| T02 | `analytic_constant`: 5x5 constant `(.25,.5,.75,1)`, center hole, r=1 stays the constant; alpha bit-exact; pinned oracle compared |
| T03 | `placeholder_independence`: two fixtures with different finite hole placeholders produce bit-identical outputs |
| T04 | `pinned_oracle` + `common_harness_fixtures`: gradients, lines, checkerboards, scratches, blocks, sparse and random holes at r=1/3/8/32 versus the independent pinned oracle; 5100 hole samples compared |
| T05 | `pinned_oracle`: four edges, corners, disjoint holes, single known sample, radius 32 and radius 8 scratch; oracle compared and unmasked bits checked |
| T06 | `full_mask`: `K=N` fails with `OperationFailed` |
| T07 | `invalid_inputs`: `0.5`, `0.25`, `-1`, `2`, NaN coverage; nonopaque alpha; NaN/Inf RGB; NaN hole placeholder |
| T08 | `registration_and_schema`: closed schema, `radius` 1 and 32 accepted, 0/-1/33/100/Float64/absent/unknown rejected with `InvalidArgument` |
| T09 | `extent_bounds` + `common_harness_fixtures`: 3x3 accepted, 2xN/Nx2/1x1/2x2 rejected with `TypeMismatch`, 32769x3 and 3x32769 rejected; mismatched mask grid rejected |
| T10 | `region_and_tiles`: nonzero ROI equals the same crop of the complete result, tiled planning, Whole descriptor/Region/facet inference, distant NaN fails global validation |
| T11 | `computed_views`: padded, offset, negative-stride and zero-stride inputs are packed-equivalent, inputs stay immutable |
| T12 | `determinism`: repeat invocations and a caller `FE_UPWARD` run are bit-identical; rounding mode restored |
| T13 | `extreme_values`: signed/HDR constants are unclamped and unmasked samples unchanged; overflowing arithmetic fails |
| T14 | `invalidation`: changed samples, mask, radius, image association and a distant NaN never reuse a stale success; radius changes the plan digest |
| T15 | `allocation_limits`: exact declared scratch succeeds, `needed-1`, `needed/2`, `needed/8` fail with `ResourceExhausted` and release every owner, then a later exact-budget run succeeds |
| T16 | `cancellation`: pre-entry and per-allocation-stage cancellation return `Cancelled` with all owners released; a bounded-time trigger requests a stop while the solver runs and asserts `Cancelled` with every owner released (a bounded trigger, not a deterministic single-sample seam); a default plan fails `Stale` before cancellation is read |
| T17 | `public_workflow` + this example + the installed consumer: registry -> compile -> bind -> execute -> named `image` with facets, shape, numbers and backend evidence |
| T18 | `concurrent_invocations`: four threads x six invocations per variant repeat the serial result exactly |

Measured with the pinned oracle linked (`PS_INPAINT_NAVIER_STOKES_ORACLE`):

```text
PNT-05A variants: image.local_inpaint_navier_stokes_native_apple_silicon image.local_inpaint_navier_stokes_openCV
image.local_inpaint_navier_stokes_native_apple_silicon vs pinned oracle 4.12.0: samples=5100 bit_differences=0 above_half_tolerance=0 worst_absolute=0 worst_tolerance_fraction=0
image.local_inpaint_navier_stokes_openCV vs pinned oracle 4.12.0: samples=5100 bit_differences=0 above_half_tolerance=0 worst_absolute=0 worst_tolerance_fraction=0
cross-variant bit differences: 0 (worst fraction of the profile tolerance 0)
```

Both variants reproduce the pinned oracle bit-for-bit on the acceptance set; the
frozen tolerance is not relaxed and no margin is extrapolated beyond the
measured fixtures.

Spec revision 0.3.1 binds one common arm64 OpenCV 4.12.0 reference built with
`-fno-fast-math -frounding-math -ffp-contract=off` (the coordinator's
`opencv-strict`), and both variants use the same profile and are bit-exact to
that reference. A diagnostic that compiles the pinned `inpaint.cpp` verbatim
into one translation unit with the kernel's flags confirms the port's fidelity
over a 512x512 random-hole fixture (65262 hole samples per channel):

```text
registered native port vs verbatim pinned source (same flags):  bit_diff=0 (all channels)
verbatim pinned source (same flags) vs opencv-strict reference:  bit_diff=0 (all channels)
verbatim pinned source built -ffp-contract=fast vs distribution: bit_diff=0 (all channels)
```

Historical measurement kept for the record: the package-manager distribution of
`libopencv_photo` contracts multiply-adds (48 `fmadd` and 18 `fmsub` inside
`icvInpaint`), and a non-contracted build differs from it by up to 15 times the
frozen tolerance on that fixture because sequential hole dependencies amplify
last-place differences. That is why the profile, the adapter linkage and the
native port all bind the non-contracted reference; the native translation unit
has no per-file floating-point override.

## Independent comparison harness

The coordinator's common harness was rebuilt privately against this library
(`common.cpp`, unmodified) and run for both keys:

```text
{"summary":true,"key":"image.local_inpaint_navier_stokes_native_apple_silicon","passed":64,"failed":0}
{"summary":true,"key":"image.local_inpaint_navier_stokes_openCV","passed":64,"failed":0}
```

This run links the coordinator's bound `opencv-strict` reference as the oracle
and the adapter, matching the controlled configuration.

## Reviewer findings and their fixes

An independent review of the first submission reported six defects; all six are
fixed and verified by the rows above and by the harness:

1. The native `FastMarching_solve` used Float32 for the pinned `double a11, a22,
   m12` locals and their expressions, which changed arrival times and made the
   17x23 block fixture exceed the tolerance at r=3/8/32. It now keeps the pinned
   binary64 comparisons, mixed term and `1+m12` sums, and the block fixture
   passes at every radius.
2. Extents were only bounded by the pixel-count limit, so 32769-wide or -tall
   inputs were accepted. Each axis is now bounded by 32768 and rejected with
   `ResourceExhausted`.
3. A fixed `RgbaFloat32` input port rejected the legal display reference. The
   image port is now a typed `SemanticKind::Image` constraint, the callback
   validates the exact remaining profile, and a display-referenced input keeps
   its facet byte-for-byte.
4. Cancellation counters counted rows, not logical samples. Validation, copying,
   packing, hole checking and native initialization now count every RGBA and
   mask read against a 4096-sample budget; the frontier keeps 64 pops / 4096
   candidate visits.
5. A refused pinned allocation (`cv::Error::StsNoMem`) was reported as
   `OperationFailed`; it now maps to `ResourceExhausted`.
6. Only final hole samples were checked for nonfinite values. Both variants now
   clear and test `FE_INVALID | FE_OVERFLOW | FE_DIVBYZERO` around each channel
   solve, so a finite published value can no longer hide an overflowing
   intermediate, and the caller's exception flags are restored unchanged.

A later harness addition, `large-frontier` (512x512, 25% random holes, r=1),
then exposed amplified drift against the FMA-contracted distribution build. It
was resolved by spec 0.3.1 binding the common non-contracted reference: both
variants are bit-exact to that reference, and the harness passes 64/64 for each
variant, `large-frontier` included.

Two further review findings were fixed in the same round:

7. The OpenCV adapter polled only before packing, so a stop requested during the
   last packing chunk could still enter the non-interruptible channel call. It
   now polls before packing, after the last packing chunk and after each channel
   call.
8. The native band construction and the per-channel flag reset ran without
   cancellation observations. `build_band` now takes the invocation and counts
   every guard-grid access with the shared sample watch, and the pinned
   `f.setTo(KNOWN)` reset is a checked loop instead of an unchecked bulk store,
   so native initialization stays inside the 4096-sample observation budget. The
   band values, arrival times and frontier order are unchanged.

## Native-only consumer

```sh
cmake -S . -B build/inpaint-ns-nativeonly -G Ninja \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo -DBUILD_TESTING=ON \
  -DPHOTOSPIDER_ENABLE_METAL=OFF -DBUILD_SHARED_LIBS=OFF \
  -DCMAKE_OSX_ARCHITECTURES=arm64 -DPHOTOSPIDER_ENABLE_OPENCV_INPAINT=OFF
cmake --build build/inpaint-ns-nativeonly --target \
  test_local_inpaint_navier_stokes photospider_inpaint_ns_workflow -j 3
./build/inpaint-ns-nativeonly/test_local_inpaint_navier_stokes
cmake --install build/inpaint-ns-nativeonly --prefix "$PWD/build/inpaint-ns-nativeonly/consumer/install"
cmake -S tests/consumer -B build/inpaint-ns-nativeonly/consumer/build -G Ninja \
  -DCMAKE_PREFIX_PATH="$PWD/build/inpaint-ns-nativeonly/consumer/install" \
  -DCMAKE_OSX_ARCHITECTURES=arm64 -DPHOTOSPIDER_CONSUMER_INPAINT_ADAPTER=OFF
cmake --build build/inpaint-ns-nativeonly/consumer/build --target \
  photospider_inpaint_ns_consumer photospider_inpaint_ns_workflow -j 3
```

Measured linkage of the native-only artifacts:

```text
kernel opencv undefined symbols: 0
test binary opencv dylibs: 0
example opencv dylibs: 0
in-tree test exit: 0     (native key only; the oracle rows are compiled out)
consumer exit: 0         (installed-package consumer, oracle compiled out)
consumer opencv dylibs: 0    consumer opencv symbols: 0
workflow opencv dylibs: 0    workflow opencv symbols: 0
```

The native-only consumer keeps the independent oracle disabled on purpose, so a
native-only consumer stays genuinely OpenCV-free; the consumer's own
`PHOTOSPIDER_CONSUMER_INPAINT_ORACLE` option can force it on for a diagnostic
build.

## Known gaps

- Only Apple Silicon (arm64) was built and measured; no cross-platform bit
  claim is made. On this toolchain the pinned float square root and absolute
  value bind binary32, which the port mirrors.
- Bit-exactness is stated against the bound non-contracted `opencv-strict`
  reference only. Against the FMA-contracted package-manager distribution the
  native variant drifts past the frozen tolerance on long sequential-hole
  fixtures; that difference is measured above and is why 0.3.1 binds one
  reference build.
- The pixel-count limit (`H*W <= 2^31-1`) is unreachable once both axes are
  bounded by 32768; it stays as a defensive check and is not exercised.
- Extents at the 32768 bound are only exercised through the rejection rows; a
  full 32768x32768 accept case would need a multi-gigabyte input and is left to
  the coordinator's harness.
- The OpenCV adapter's library-internal allocations and channel-granular
  cancellation are declared limitations, not measured against a host budget.
- Frontier-internal cancellation is covered with a bounded-time trigger rather
  than a deterministic single-sample seam, so the row proves that a requested
  stop is observed with all owners released inside the solver's runtime, not
  which individual poll observed it. Stage boundaries (helper entry, allocation
  edges, channel transitions, publication) are deterministic and are observed
  explicitly.
- No performance number is reported here; the coordinator runs one common
  public-API benchmark serially after all builds stop.
