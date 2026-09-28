# PixelOE CPU, Metal and Vulkan Slang plugin

An independently built native operation module for Photospider 0.28. It ports PixelOE's pinned Slang pixelization pipeline, including all downscale modes, outline/statistics/weight options, color correction, sharpening, quantization and dithering. See [SPEC.md](SPEC.md) for the exact bounded parameter contract.

The host dynamically loads the shared module through `OperationRegistry::load_plugin`. The current package source uses base operation ABI v11 plus planar extension v3; the loader rejects ABI 10 and planar v1/v2 modules. Rebuild the plugin and C++ consumers against the matching SDK. Reference kernels are compiled from Slang ahead of time, with validated native SIMD specializations for selected CPU stages. PixelOE GPU entries include MSL for Metal and SPIR-V for Vulkan. The separate `backend=gpu` and `backend=vulkan` choices select distinct registered profiles; runtime validation requires the selected key to match the active native GPU backend. Python/Torch/Slang are build and validation dependencies only. Public kernel headers/libraries come exclusively from the installed package. No private Photospider headers and no built-in registration are used.

## Build

First build/install the modified kernel. Existing build options remain in its CMake cache. The example below uses an explicit prefix and build-local tools:

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

Validated with Slang 2026.18.2 on Apple Silicon. The generator validates the Slang CPU parameter struct and wraps each generated translation unit in its own namespace. Different compiler versions must pass the oracle again. Generated C++, frozen coefficient tables, objects and binaries stay in the build tree. The coefficient generator reproduces upstream FP16 Gaussian table construction and stores FP32 coefficients, including every supported low-rank SVD basis.

The package produces `libphotospider_pixeloe.so`, `pixeloe_workflow` and `pixeloe_benchmark` on the tested macOS configuration. CPU Whole computation uses synchronous ranges with caller participation. The `_cpu_tiled` operations use the CPU_STAGES coordinator and submit fixed-geometry single-thread tile callbacks to the shared host worker pool. Each stage joins before the next stage begins; the execution model is independent of the operation's Whole input/output dependency rule. The Metal and Vulkan native FP32 profiles use their matching native GPU services and retain intermediates across stages. ARM64 uses NEON; x86-64 admits AVX2 after CPU/OS feature detection, with ISA flags isolated to the specialization translation unit. Other targets use generated Slang code. Selected blur/morphology stages have native SIMD specializations that preserve the pinned Slang equations and FP operation order.

## Public workflow

```sh
build/pixeloe/pixeloe_workflow \
  build/pixeloe/libphotospider_pixeloe.so 1920 1080 \
  pixel_size=6 thickness=3 mode=contrast \
  workers=4 warmup=1 repeat=5 output=build/pixeloe-output.f32
```

The executable constructs a real WorkflowDocument, loads the external module, compiles through the public Compiler and executes with a planar input binding. Its source contains the complete typed parameter map; strings, Int64, Float64 and Bool values are kept distinct. Registry parameters are required; the example supplies upstream defaults. `input=/path/raw.f32` accepts packed HWC RGB binary32 bytes as an explicit fixture import boundary. Outputs are packed HWC FP32 for inspection. Neither fixture I/O nor planar input construction is timed.

The three CPU Whole keys are `pixeloe.pixelize`, `pixeloe.expanded` and `pixeloe.weight`. The three CPU staged keys append `_cpu_tiled`; Metal keys append `_metal_native_fp32`; Vulkan keys append `_vulkan_native_fp32`. All twelve publish `values`. `operation=pixeloe.expanded` or `pixeloe.weight` inspects the upstream intermediates. A workflow can request all three as separate nodes. Weight is explicitly computed even with thickness zero. These separate nodes currently repeat shared work.

`workers=1` selects the reference host quota; `workers=4` permits four host workers. `backend=cpu_tiled` selects the staged keys and routes all kernel calculations through host tile callbacks; it does not slice a completed Whole output. Every stage joins before the next stage starts. `maximum_parallelism` caps a stage grant while fixed work-item geometry and reduction order stay unchanged. Workgroup reductions and per-pixel SIMD arithmetic retain their fixed order.

The kernel exclusively owns thread scheduling. The plugin creates no threads and does not submit work to GCD or other pools. `PIXELOE_CPU_SIMD=0` selects the Slang reference path for diagnostics; default SIMD selection must preserve output bits. AOT compilation and coefficient initialization are outside measured hot runs; derived-result caching is disabled in the provided workload.

Input must be structural planar RGB FP32, finite [0,1], straight sRGB/D65. A missing color description explicitly means this operator's sRGB domain. Declared incompatible primaries, transfer, white, channel roles, units, reference, alpha or numeric encoding reject; perform the relevant conversion beforehand. Output RGB establishes canonical sRGB tensor metadata; the weight output is untagged scalar data. The operation never converts an ICC profile implicitly.

The current kernel's planar Whole execution requires a whole output request. Read a crop from the complete published image afterward. This is intentional because weights, color moments and palettes can depend on the full input. Nearest upscale retains upstream symmetric replicate padding: with pixel_size=6, 128x128 becomes 132x132 and 4096x4096 becomes 4098x4098.

A zero Lab source variance during color matching fails explicitly. In particular, a pure black input with default color matching is a numerical-domain failure; disable color matching to process it. There is no hidden NaN-to-color clamp. `blur_impl=tiled` uses the portable direct exact-table convolution on CPU; `sym` uses the upstream symmetry-folding kernel. Other algorithm options preserve their selected math. Low-rank rank 1 is the upstream approximation and is not advertised as a correctly rounded full-rank blur.

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
build/pixeloe-python/bin/python plugins/ops/PixelOE/tools/validate.py \
  --upstream cache/PixelOE-upstream --runner build/pixeloe/pixeloe_workflow \
  --plugin build/pixeloe/libphotospider_pixeloe.so --backend vulkan \
  --out build/pixeloe-vulkan-validation
build/pixeloe/pixeloe_check_simd
PIXELOE_CPU_SIMD=0 build/pixeloe/pixeloe_benchmark 1920 1080 5
PIXELOE_CPU_SIMD=1 build/pixeloe/pixeloe_benchmark 1920 1080 5
build/pixeloe-python/bin/python plugins/ops/PixelOE/tools/benchmark_assets.py \
  --assets assets/codec --build build/pixeloe --out build/pixeloe-assets
```

The oracle checkout must be revision `0239787b8bb3e0c0dac615a33c896311d40cb46e`. `validate.py` uses the upstream Torch implementation and independent Torch convolution/sliding-statistics oracles for Slang-only options. It does not invoke these compiled shaders as its oracle. `check_contract.py` verifies exact scalar/SIMD/rounding equality, signed-zero and subnormal copies, invalid-domain failures and resource limits.

`pixeloe_benchmark` measures the native pipeline and sum of synchronous CPU kernel dispatches (Slang reference or admitted SIMD), with warmup and the same synthetic input formula. It excludes public binding/planning/planar transfer and uses a local scratch ledger; the public workflow measures actual managed execution separately. Per-kernel values are mean dispatch totals; median pipeline/kernel values are reported separately. They must not be added to public latency.

Real-image tests decode once before timing, preserve uint8/uint16 precision on normalization, reorder BGR to RGB, and explicitly discard alpha. They do not claim ICC conversion. The measured calls read already prepared FP32 input; codec/file I/O and input planar construction are excluded. Timings, environment, validation coverage and remaining platform limits are recorded in [PERFORMANCE.md](PERFORMANCE.md).

Upstream authorship and Apache-2.0 license are in [third_party/NOTICE.md](third_party/NOTICE.md).

`pixeloe_check_simd` compares 3720 blur/morphology stage cases bitwise against AOT Slang, including short rows, padding, tails, all blur radii and modes, signed zero, subnormals, cancellation and callback-thread service ownership. Exit 77 means that the host lacks a supported SIMD ISA, so no SIMD claim is made.

## Portable CPU source bundle

A generated-source bundle permits native compilation on a host without a Slang executable or Torch. The source shaders, generator identities, tool versions, all artifact hashes and Slang licenses travel with the bundle. CMake verifies these identities before compiling it. The native compiler still builds every kernel for the destination CPU and ABI.

```sh
build/pixeloe-python/bin/python plugins/ops/PixelOE/tools/cpu_sources.py export \
  --root plugins/ops/PixelOE --generated build/pixeloe/generated \
  --slang-root cache/slang-2026.18.2 \
  --bundle out/gpu-whole-tiled/dependencies/pixeloe-cpu
cmake -S plugins/ops/PixelOE -B build/pixeloe-native \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=/path/to/installed/kernel \
  -DPIXELOE_PREGENERATED_DIR=/path/to/pixeloe-cpu
cmake --build build/pixeloe-native -j 8
```

FreeBSD uses the Slang prelude's documented platform override for its POSIX helpers. This does not load Linux binaries. The bundle only implements CPU kernels; it is not a GPU pipeline.

## CPU tiled workflow

```sh
build/pixeloe/pixeloe_workflow \
  build/pixeloe/libphotospider_pixeloe.so 1920 1080 \
  backend=cpu_tiled workers=4 warmup=1 repeat=5 \
  output=build/pixeloe-cpu-tiled-output.f32
```

`backend=cpu_tiled` selects the three CPU_STAGES registrations. Each stage computes its fixed work-item grid, sends disjoint boxes through the shared job queue and waits for all callbacks before advancing. Callbacks run as single-threaded tasks on pool workers. The coordinator allocates and clears buffers, and obtains stable row spans; tile callbacks validate and copy input samples, then validate and copy output samples. The operation retains Whole dependencies and returns the complete output. CPU Whole and CPU tiled variants use the same CPU arithmetic profile and existing validated SIMD kernels. Their output comparison is bitwise for matching parameters and build. CPU and GPU numerical profiles remain distinct. Vulkan correctness currently covers FreeBSD Intel UHD Graphics 770; NVIDIA and Linux Vulkan validation and broader performance optimization remain open.

## Metal native FP32 workflow

```sh
build/pixeloe/pixeloe_workflow \
  build/pixeloe/libphotospider_pixeloe.so 1920 1080 \
  backend=gpu warmup=1 repeat=5 budget=8589934592
build/pixeloe/pixeloe_check_stages cpu
build/pixeloe/pixeloe_check_stages gpu
```

`backend=gpu` selects the `_metal_native_fp32` operation suffix and requires native Metal dispatches. The three public GPU keys are `pixeloe.pixelize_metal_native_fp32`, `pixeloe.expanded_metal_native_fp32` and `pixeloe.weight_metal_native_fp32`. Read the profile comparison in [SPEC.md](SPEC.md) before comparing CPU/GPU results. Subnormal arithmetic may flush and native math functions can differ from CPU libm; copy paths retain source bits. GPU profile identity includes the GPU, OS, Metal compiler, Slang version and generated source.

The CPU generator builds 77 AOT entries. Each GPU target builds those entries plus GPU-only `kc_iter_max` and `copy_words`, for 79 target-reflected entries per backend and 158 Metal/Vulkan records total. The Vulkan generator emits SPIR-V 1.5 with std140 uniform blocks; Slang 2026.18.2 and `spirv-val --target-env vulkan1.2` validate the generated modules. Of the 79 Vulkan entries, 74 use Shader capability only and five require ShaderInt64. The package set is the selected 79-entry table, not every upstream shader variant. In k-centroid mode, both GPU profiles submit four `kc_iter_max` iteration dispatches; each atomically reduces the maximum centroid change. The CPU still uses `kc_iter` followed by `diff_partial` and `diff_final` on each of its four iterations. The CPU entry table and its selected SIMD implementations remain unchanged.

`check_stages` runs independent scalar witnesses through real kernels; `tools/validate.py --backend gpu` runs the public workflow against the independent Torch reference. `tools/check_contract.py BUILD OUTPUT gpu` covers repeatability, input/metadata errors, work/capacity rejection, pre-cancellation and exact copy bits. A host without a native GPU returns skip code 77 from the stage checker. The macOS Metal stage result is recorded separately from Vulkan. It does not establish Vulkan behavior.

## Vulkan native FP32 workflow

```sh
build/pixeloe/pixeloe_workflow \
  build/pixeloe/libphotospider_pixeloe.so 31 25 \
  backend=vulkan workers=1 warmup=1 repeat=1 \
  input=build/pixeloe-vulkan-input.f32 \
  output=build/pixeloe-vulkan-output.f32
build/pixeloe/pixeloe_check_stages vulkan
```

The installed Photospider SDK must be built with `-DPHOTOSPIDER_ENABLE_VULKAN=ON` and Vulkan development headers/loader available at configuration time. `backend=vulkan` selects `pixeloe.pixelize_vulkan_native_fp32`, `pixeloe.expanded_vulkan_native_fp32` or `pixeloe.weight_vulkan_native_fp32` according to `operation=`. It requires a native Vulkan service and does not fall back to CPU or Metal. PixelOE's FreeBSD Intel UHD Graphics 770 validation passed 612 native stage dispatches; the 40-fixture Torch comparison passed with maximum absolute error 3.4570693969726562e-6 against `1e-5`. Separate public `expanded` and `weight` checks also passed. A paired three-round Vulkan k-centroid comparison on Intel recorded median-of-round-medians of 15.6455 to 9.73507 ms at 128², 21.3699 to 15.7411 ms at 256², and 36.6713 to 35.5624 ms at 512² when changing from the three-stage GPU reduction to the atomic iteration. The dispatch/submission count fell from 27/28 to 19/20, and output hashes matched in every pair. P95 varied substantially and the 512² result is small; these measurements do not establish a stable or general gain. NVIDIA and Linux are not validated.

The Vulkan profile allows native underflow and device math-function error, while exact copies preserve signed-zero and subnormal input bits. Its arithmetic and decision contract is separate from CPU and Metal; see [SPEC.md](SPEC.md) before comparing outputs. The latest Vulkan result records and validation-layer log are linked from [VULKAN_MODEL.md](../../../out/gpu-whole-tiled/VULKAN_MODEL.md).

The workflow reports native dispatch, submission and device-time counters separately from public workflow latency. `budget` sets the managed Host/Shared and payload bounds; `work_budget` sets cumulative work admission. `cancel_after_us=0` pre-cancels; a positive delay requests cancellation during execution after any warmup. The plugin itself creates no timer or worker thread. Failed calls report the typed error code and remaining live payload.
