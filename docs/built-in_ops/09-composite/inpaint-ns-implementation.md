# Local Navier-Stokes inpainting implementation

This document describes the built-in OpenCV and native Apple Silicon operations,
their resource boundaries, runnable public workflow and acceptance evidence.
The authoritative contract is [PNT-05A](op_specs/PNT-05A_local_inpaint_navier_stokes.md).

## Public behavior

Both operations take ordered Float32 Image `[H,W,4]` and canonical Float32
coverage `[H,W]`, require exact Int64 `radius` in `[1,32]`, and expose only named
output `image`:

- `image.local_inpaint_navier_stokes_native_apple_silicon`
- `image.local_inpaint_navier_stokes_openCV`

Image channels must be ordered linear-sRGB D65 premultiplied RGBA, alpha exactly
one. Scene/display reference and image facets are preserved. Masks must contain
numeric zero or one. The full domain is validated even for zero masks and ROI
requests. Hole RGB is zeroed before each sequential Float32 channel solve;
only hole RGB is copied back. Known samples and alpha keep their original bits.
Whole traits conservatively demand and invalidate both complete inputs.

The synchronous ABI's declarative inference checks typed dtype/rank and preserves
input image metadata. It has no custom synchronous metadata validator: the
additional exact color profile, equal spatial shape and `3..32768` limits are
checked at callback entry. **The specification's requirement to reject every
shape/profile violation during inference is not fully implemented.** Host port
validation can run before that callback check. No shared ABI was changed to add
custom inference. This also limits claims about rejecting enormous synthetic
computed views without a full host scan.

## Implementation and resource boundaries

`inpaint_ns_native.cpp` is a standalone C++ port of the single-channel Float32
OpenCV 4.12.0 NS path, source blob
`2f2f368fa13da0bc1426b71862205048c6ea0f94`. It retains the original Intel license
in source and in installed `share/licenses/Photospider/inpaint_ns_license.txt`.
It has no OpenCV headers, calls or symbols. It uses a fixed host-owned heap,
row-major initial insertion, stable insertion-order ties and up/left/down/right
frontier neighbors. The pinned source passes its mask state to NS, so initial
queued known neighbors deliberately retain KNOWN state. Gradients and mixed
Float32/Float64 expressions retain source order. No SIMD/Metal/FMA optimization
or private thread pool is used.

With `N=H*W`, `P=(H+2)*(W+2)`, native callback payload capacities are:

| Buffer | Bytes |
| --- | ---: |
| Output | `16N` |
| Binary mask | `N` |
| Reused Float32 work plane | `4N` |
| Padded state | `P` |
| Padded arrival times | `4P` |
| Fixed heap, 16-byte entry | `16N` |
| Total callback payload | `37N+5P` |

Initial band and hole pixels are disjoint. Each hole changes to BAND before
insertion, so each canvas pixel is inserted at most once; total insertion order
and live capacity are bounded by N. Dimensions bound every product and signed
index. All payload buffers use `call.allocator`. The conservative workspace
reservation is `2 * demanded_input_bytes + 65536` excluding output. At minimum
3x3 dimensions this bounds scratch `21N+5P < 35N`. Upstream materialization,
retained caller storage, metadata and output collectors have their existing
host accounting; these formulas are not RSS measurements.

Validation polls every 512 pixels (five samples each), output copying every
1024 pixels, packing/copy-back every 4096 samples, and native initialization
every 1024 grid positions. Frontier processing checks every 64 pops and 4096
candidate visits. Allocation and channel boundaries and prepublication also
check cancellation. Host allocation internals themselves remain the allocator's
responsibility. No callback publishes partially completed results.

The adapter calls the locally provisioned OpenCV 4.12.0 sequentially on three
Float32 planes. Host output/packing uses `25N` bytes. Its external OpenCV
matrices/queue are **not host-budgeted**. A separate conservative estimate for
this pinned source and libc++ vector growth is `7P+32N+64KiB`; this is not a hard
allocation guarantee or measured peak. The internal channel call cannot observe
the host cancellation token. Only before/after-channel cancellation is promised.
`cv::Error::StsNoMem` maps to ResourceExhausted. No process-global OpenCV allocator
or thread configuration is changed.

The adapter and results derived from it do not enter reusable memory or disk
result caches. Replacing a same-version OpenCV library can change numerical
behavior without changing the kernel source build identity. Native results keep
the standard cache policy; in-run Whole materialization still applies to both.

Both callbacks save/restore floating environment and use nearest rounding and
gradual underflow. Source flags disable fast math and FP contraction. Finite
checks plus overflow/invalid/divide-by-zero exception checks reject nonfinite
arithmetic; native also checks weight/accumulation intermediates explicitly.
Unmasked signed zero is copied through `memcpy`.

## Actual focused evidence

Host: Apple M5, macOS 27.0, AppleClang 21.0.0.21000101, RelWithDebInfo,
static arm64 kernel, Metal OFF. Adapter-enabled validation uses OpenCV 4.12.0
core/imgproc/photo built with `-fno-fast-math -frounding-math -ffp-contract=off`.
A binary with the same OpenCV version but different contraction settings is
outside this numerical profile. CMake checks the version; the dependency
provider must supply the specified floating-point configuration.
`test_inpaint_ns` passes in both adapter-enabled and native-only builds.
The adapter-enabled test independently calls OpenCV and checks 288
backend/shape/pattern/radius combinations: 144 per backend, shapes 3x5, 5x7,
11x13, 17x19; radii 1/3/8/32; constant, asymmetric pattern, scratches, edge/corner,
block, scattered, checkerboard and one-known-pixel masks. It also reruns every
case with changed finite hole placeholders. Tolerance is exactly
`1e-6+1e-5*abs(reference)`. Native-only builds run 16 matrix constant cases plus the exact T02 fixture and
the remaining OpenCV-independent checks.

| ID | Actual evidence and remaining scope |
| --- | --- |
| T01 | PASS: zero-mask exact bytes including image/mask signed zero; NaN/Inf still rejected. |
| T02 | PASS: exact 5x5 `(0.25,0.5,0.75,1)` center-hole r=1 analytic fixture, plus matrix and public example. |
| T03 | PASS: placeholder change preserves complete output bytes for all 288 matrix cells. |
| T04 | PASS for the listed small synthetic patterns and all four radii; no real-image corpus. |
| T05 | PASS: combined top/left and bottom/right edges/corners, scattered holes, one known corner/center. No sanitizer run. |
| T06 | PASS: full mask fails OperationFailed. |
| T07 | PASS: fractional/out-of-range/NaN masks, alpha .5, NaN/Inf RGB. Direct host InvalidArgument is preserved where detected first. |
| T08 | PASS: 1/32 accepted; 0/33, absence, Float64 and unknown key rejected. Negative radius follows the same schema, not separately tested. |
| T09 | PARTIAL: minimum dimension 3 works, dimensions 1/2 reject; no huge/overflow descriptor test and no complete inference-stage rejection, as explained above. |
| T10 | PASS: nonzero 3x2 ROI at (1,1), 1x1 tiles, Whole crop equality and distant NaN rejection. |
| T11 | PARTIAL: direct public registry invocation of valid padded, offset, nonzero-origin, negative/zero-stride views matches packed input; inputs remain immutable. No computed fixture-operation workflow was added. |
| T12 | PASS: repeated outputs, FE_UPWARD inputs, caller rounding restored. No cross-compiler bit claim. |
| T13 | PASS: signed/HDR synthetic samples, large alternating 1e30 gradients rejected as OperationFailed. No exhaustive Float32 extreme sweep. |
| T14 | PARTIAL: independent changed input/mask/radius/metadata executions and distant-NaN failures covered; same-context native cache reuse and adapter recomputation are checked, but no complete changed-input cache-invalidation regression. |
| T15 | PASS native: each of six allocations fails independently, owners release, exact 1170-byte 5x5 capacity succeeds and 1169 fails. Adapter host buffers accounted, external allocation failure injection absent. |
| T16 | PARTIAL native: pre-entry and six allocation-boundary cancellation points fail Cancelled. Frontier/init/prepublication polling is source-reviewed, not deterministically interrupted in tests; Stale and latency tests absent. Adapter internal interruption unsupported by adapter contract. |
| T17 | PASS: registry, compile, bind, execute and named image in public workflow; adapter-enabled and native-only installed consumers run; exact facets and numbers checked. |
| T18 | PASS: two independent asynchronous invocations per backend match baseline; no concurrency stress/sanitizer matrix. |

## Build and run

The native implementation is built by default, with OpenCV discovery disabled. Enabling
`PHOTOSPIDER_ENABLE_INPAINT_OPENCV` additionally registers the adapter and makes
OpenCV 4.12.0 a dependency of the installed kernel package.

For adapter-enabled builds, first provide the pinned reference library. A local
build can be prepared as follows (Git, CMake and a C++ compiler are required):

```sh
git clone --depth 1 --branch 4.12.0 https://github.com/opencv/opencv.git build/opencv-source
cmake -S build/opencv-source -B build/opencv-build \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_OSX_ARCHITECTURES=arm64 \
  -DCMAKE_INSTALL_PREFIX="$PWD/build/opencv-strict" \
  -DCMAKE_CXX_FLAGS="-fno-fast-math -frounding-math -ffp-contract=off" \
  -DCMAKE_C_FLAGS="-fno-fast-math -frounding-math -ffp-contract=off" \
  -DBUILD_LIST=core,imgproc,photo -DBUILD_SHARED_LIBS=ON \
  -DBUILD_TESTS=OFF -DBUILD_PERF_TESTS=OFF -DBUILD_EXAMPLES=OFF \
  -DBUILD_opencv_apps=OFF -DBUILD_JAVA=OFF -DBUILD_opencv_python3=OFF \
  -DWITH_IPP=OFF -DWITH_OPENCL=OFF -DWITH_ITT=OFF -DWITH_PROTOBUF=OFF \
  -DWITH_LAPACK=OFF -DCPU_BASELINE=NEON -DCPU_DISPATCH=
cmake --build build/opencv-build -j 3
cmake --install build/opencv-build
```

Point `OpenCV_DIR` at that installation or an equivalently configured dependency:

```sh
cmake -S . -B build/inpaint-ns -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DBUILD_TESTING=ON -DPHOTOSPIDER_ENABLE_METAL=OFF -DBUILD_SHARED_LIBS=OFF \
  -DPHOTOSPIDER_ENABLE_INPAINT_OPENCV=ON \
  -DOpenCV_DIR="$PWD/build/opencv-strict/lib/cmake/opencv4"
cmake --build build/inpaint-ns --target test_inpaint_ns photospider_inpaint_ns_workflow -j 3
ctest --test-dir build/inpaint-ns -R '^test_inpaint_ns$' --output-on-failure
build/inpaint-ns/examples/inpaint_ns_workflow/photospider_inpaint_ns_workflow
build/inpaint-ns/examples/inpaint_ns_workflow/photospider_inpaint_ns_workflow image.local_inpaint_navier_stokes_openCV
cmake --install build/inpaint-ns --prefix "$PWD/build/inpaint-ns/install"
cmake -S examples/inpaint_ns_workflow -B build/inpaint-ns/consumer \
  -DCMAKE_PREFIX_PATH="$PWD/build/inpaint-ns/install;$PWD/build/opencv-strict"
cmake --build build/inpaint-ns/consumer -j 3
build/inpaint-ns/consumer/photospider_inpaint_ns_workflow
build/inpaint-ns/consumer/photospider_inpaint_ns_workflow image.local_inpaint_navier_stokes_openCV
```

Both operations print center `0.25,0.5,0.75,1 PASS`. The example compiles a
workflow through the public registry, binds opaque RGBA and binary coverage,
executes radius 3 and checks the named `image` output and preserved facets.
The standalone consumer uses only installed headers and `Photospider::kernel`.

Native-only build and consumer:

```sh
cmake -S . -B build/inpaint-native -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DBUILD_TESTING=ON -DPHOTOSPIDER_ENABLE_METAL=OFF -DBUILD_SHARED_LIBS=OFF \
  -DPHOTOSPIDER_ENABLE_INPAINT_OPENCV=OFF
cmake --build build/inpaint-native --target test_inpaint_ns photospider_inpaint_ns_workflow -j 3
ctest --test-dir build/inpaint-native -R '^test_inpaint_ns$' --output-on-failure
cmake --install build/inpaint-native --prefix "$PWD/build/inpaint-native/install"
cmake -S examples/inpaint_ns_workflow -B build/inpaint-native/consumer \
  -DCMAKE_PREFIX_PATH="$PWD/build/inpaint-native/install" \
  -DCMAKE_DISABLE_FIND_PACKAGE_OpenCV=TRUE
cmake --build build/inpaint-native/consumer -j 3
build/inpaint-native/consumer/photospider_inpaint_ns_workflow
otool -L build/inpaint-native/consumer/photospider_inpaint_ns_workflow
nm -u build/inpaint-native/libphotospider.a | rg -i 'opencv|__ZN2cv'
```

Native-only validation prints PASS. The checked consumer links libc++ and
libSystem, with no OpenCV dependency; the symbol filter returns no matches
(`rg` exit 1). The OpenCV operation is absent when its adapter is disabled.

Focused tests and public examples were run in adapter-enabled and native-only
configurations. Installed consumers were also checked. ClangFormat 21, cpplint
and `git diff --check` passed for the implementation. Full CTest, sanitizer,
Metal and cross-platform validation are outside the recorded evidence.
