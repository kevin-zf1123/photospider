# PixelOE CPU Slang plugin

An independently built native operation module for Photospider 0.24. It ports
PixelOE's pinned Slang pixelization pipeline, including all downscale modes,
outline/statistics/weight options, color correction, sharpening, quantization
and dithering. See [SPEC.md](SPEC.md) for the exact bounded parameter contract.

The host dynamically loads the shared module through
`OperationRegistry::load_plugin`. It uses base operation ABI v9 plus the new
planar extension v1. Reference kernels are compiled from Slang ahead of time,
with validated native SIMD specializations for selected CPU stages.
Python/Torch/Slang are build and validation dependencies only. Public
kernel headers/libraries come exclusively from the installed package. No private
Photospider headers and no built-in registration are used.

## Build

First build/install the modified kernel. Existing build options remain in its
CMake cache. The example below uses an explicit prefix and build-local tools:

```sh
cmake --build build --target photospider -j 8
cmake --install build --prefix "$PWD/build/pixeloe-install"
python3 -m venv build/pixeloe-python
build/pixeloe-python/bin/pip install numpy torch pillow
cmake -S plugins/ops/PixelOE -B build/pixeloe \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_PREFIX_PATH="$PWD/build/pixeloe-install" \
  -DSLANGC=/absolute/path/to/slangc \
  -DPython3_EXECUTABLE="$PWD/build/pixeloe-python/bin/python"
cmake --build build/pixeloe -j 8
```

Validated with Slang 2026.18.2 on Apple Silicon. The generator validates the
Slang CPU parameter struct and wraps each generated translation unit in its own
namespace. Different compiler versions must pass the oracle again. Generated
C++, frozen coefficient tables, objects and binaries stay in the build tree.
The coefficient generator reproduces upstream FP16 Gaussian table construction
and stores FP32 coefficients, including every supported low-rank SVD basis.

The package produces `libphotospider_pixeloe.so`, `pixeloe_workflow` and
`pixeloe_benchmark` on the tested macOS configuration. The module is CPU only;
all computation stays on the host-assigned callback thread. ARM64 uses NEON;
x86-64 admits AVX2 after CPU/OS feature detection, with ISA flags isolated to
the specialization translation unit. Other targets use generated Slang code.
Selected blur/morphology stages have native SIMD specializations that preserve
the pinned Slang equations and FP operation order.

## Public workflow

```sh
build/pixeloe/pixeloe_workflow \
  build/pixeloe/libphotospider_pixeloe.so 1920 1080 \
  pixel_size=6 thickness=3 mode=contrast \
  warmup=1 repeat=5 output=build/pixeloe-output.f32
```

The executable constructs a real WorkflowDocument, loads the external module,
compiles through the public Compiler and executes with a planar input binding.
Its source contains the complete typed parameter map; strings, Int64, Float64
and Bool values are kept distinct. Registry parameters are required; the example
supplies upstream defaults. `input=/path/raw.f32` accepts packed HWC RGB binary32
bytes as an explicit fixture import boundary. Outputs are packed HWC FP32 for
inspection. Neither fixture I/O nor planar input construction is timed.

Operation names are `pixeloe.pixelize`, `pixeloe.expanded` and `pixeloe.weight`;
each publishes `values`. `operation=pixeloe.expanded` or `pixeloe.weight` inspects
the upstream intermediates. A workflow can request all three as separate nodes.
Weight is explicitly computed even with thickness zero. These separate nodes
currently repeat shared work.

The kernel exclusively owns thread scheduling. The plugin creates no threads
and does not submit work to GCD or other pools. `PIXELOE_CPU_SIMD=0` selects the
Slang reference path for diagnostics; default SIMD selection must preserve output
bits. AOT compilation and coefficient initialization are outside measured hot runs; derived-result caching is disabled
in the provided workload.

Input must be structural planar RGB FP32, finite [0,1], straight sRGB/D65. A
missing color description explicitly means this operator's sRGB domain. Declared
incompatible primaries, transfer, white, channel roles, units, reference, alpha
or numeric encoding reject; perform the relevant conversion beforehand. Output
RGB establishes canonical sRGB tensor metadata; the weight output is untagged
scalar data. The operation never converts an ICC profile implicitly.

The current kernel's planar Whole execution requires a whole output request.
Read a crop from the complete published image afterward. This is intentional
because weights, color moments and palettes can depend on the full input.
Nearest upscale retains upstream symmetric replicate padding: with pixel_size=6,
128x128 becomes 132x132 and 4096x4096 becomes 4098x4098.

A zero Lab source variance during color matching fails explicitly. In particular,
a pure black input with default color matching is a numerical-domain failure;
disable color matching to process it. There is no hidden NaN-to-color clamp.
`blur_impl=tiled` uses the portable direct exact-table convolution on CPU;
`sym` uses the upstream symmetry-folding kernel. Other algorithm options preserve
their selected math. Low-rank rank 1 is the upstream approximation and is not
advertised as a correctly rounded full-rank blur.

## Validation and timing

```sh
ctest --test-dir build \
  -R '^(test_planar_plugin|test_planar_image_workflow|test_plugin_registry)$' \
  --output-on-failure
build/pixeloe-python/bin/pip install kornia torchvision opencv-python-headless
build/pixeloe-python/bin/python plugins/ops/PixelOE/tools/validate.py \
  --upstream cache/PixelOE-upstream --runner build/pixeloe/pixeloe_workflow \
  --plugin build/pixeloe/libphotospider_pixeloe.so --out build/pixeloe-validation
build/pixeloe-python/bin/python plugins/ops/PixelOE/tools/check_contract.py \
  build/pixeloe build/pixeloe-contract
build/pixeloe/pixeloe_check_simd
PIXELOE_CPU_SIMD=0 build/pixeloe/pixeloe_benchmark 1920 1080 5
PIXELOE_CPU_SIMD=1 build/pixeloe/pixeloe_benchmark 1920 1080 5
build/pixeloe-python/bin/python plugins/ops/PixelOE/tools/benchmark_assets.py \
  --assets assets/codec --build build/pixeloe --out build/pixeloe-assets
```

The oracle checkout must be revision
`0239787b8bb3e0c0dac615a33c896311d40cb46e`. `validate.py` uses the upstream Torch
implementation and independent Torch convolution/sliding-statistics oracles for
Slang-only options. It does not invoke these compiled shaders as its oracle.
`check_contract.py` verifies exact scalar/SIMD/rounding equality, signed-zero
and subnormal copies, invalid-domain failures and resource limits.

`pixeloe_benchmark` measures the native pipeline and sum of synchronous CPU
kernel dispatches (Slang reference or admitted SIMD), with warmup and the same synthetic input formula. It excludes
public binding/planning/planar transfer and uses a local scratch ledger; the
public workflow measures actual managed execution separately. Per-kernel values
are mean dispatch totals; median pipeline/kernel values are reported separately.
They must not be added to public latency.

Real-image tests decode once before timing, preserve uint8/uint16 precision on
normalization, reorder BGR to RGB, and explicitly discard alpha. They do not
claim ICC conversion. The measured calls read already prepared FP32 input;
codec/file I/O and input planar construction are excluded. Timings, environment,
validation coverage and remaining platform limits are recorded in
[PERFORMANCE.md](PERFORMANCE.md).

Upstream authorship and Apache-2.0 license are in [third_party/NOTICE.md](third_party/NOTICE.md).

`pixeloe_check_simd` compares 3720 blur/morphology stage cases bitwise against
AOT Slang, including short rows, padding, tails, all blur radii and modes,
signed zero, subnormals, cancellation and callback-thread service ownership.
Exit 77 means that the host lacks a supported SIMD ISA, so no SIMD claim is made.
