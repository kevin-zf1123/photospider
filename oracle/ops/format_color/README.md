# Independent format/color oracles

These sources compute numerical expectations independently of the bundled
operations. They do not establish workflow execution, resource ownership,
cancellation, backend dispatch or arbitrary-input error bounds. Temporary
operation/runtime validators and generated fixture data are removed after
validation; the mathematical oracle sources remain.

Existing subdirectories cover FMT-04 alpha arithmetic, FMT-06 numeric conversion,
FMT-09 transfer functions, FMT-10 exact geometry, FMT-11 model mathematics and
shared channel/tile mappings. Their per-source entry points and family
specifications describe the respective scope.

## FMT-14 / FMT-15

[`fmt14_15/generate.py`](fmt14_15/generate.py) generates 670 deterministic cases
for all six native gamut/tone/display members, in Float32 and Float64. The
generator uses Python 3.9 or newer and mpmath 1.3.0. It imports only the retained
independent FMT-10/FMT-11 Python oracles, never production C++ or a system color
engine.

- Geometry uses Python `Fraction` and independent Gaussian elimination.
- IEEE rounding uses integer quotients/remainders and ties-to-even.
- Compression, tone and display formulas use exact rational arithmetic,
  including extreme finite parameters and exact cancellation.
- OKLab reduction rounds every specified intermediate stage. Its forward
  transcendental calculation is repeated at 180 and 360 decimal digits and
  must produce identical stored bits. The inverse stages and all 32/64
  dyadic bisection steps are exact. The returned acceptance-decision list can
  support a separate branch-trace validator.
- Fixtures include negative zero, minimum subnormals, maximum finite values,
  neutral/end-point cases, signed/HDR values, a dyadic-weight basis, negative-Y
  rejection, finite-input intermediate overflow and final output overflow.
  The two-precision stability test is not a universal proof of correctly
  rounded transcendental functions.

Create a scratch virtual environment and generate/check temporary fixtures:

```sh
python3 -m venv build/ops-dev/oracle-python
build/ops-dev/oracle-python/bin/python -m pip install mpmath==1.3.0
build/ops-dev/oracle-python/bin/python oracle/ops/format_color/fmt14_15/generate.py --output build/ops-dev/format-oracles.txt
build/ops-dev/oracle-python/bin/python oracle/ops/format_color/fmt14_15/generate.py --output build/ops-dev/format-oracles.txt --check
```

Each row contains `kind narrow basis gain white knee black peak extended signed
to_nits input[3] expected[3] error`. Kinds 0..5 are clip, OKLab reduction,
compression, gamut mask, tone, display conversion. `narrow=1` selects Float32.
Input/expected samples are hexadecimal native IEEE words; parameters are
round-trippable decimal representations of binary64 values. Error 0 means
success, 1 means InvalidDomain, and 2 means ArithmeticOverflow. For a mask only
the first expected word is an output component.

Both optimized and exact-reference arithmetic matched all 670 rows in the
focused implementation validation. Public workflow and performance evidence is
recorded separately in
[FMT-14/15 implementation](../../../docs/built-in_ops/02-format-color/FMT-14-15_implementation.md).
Regenerate expectations for future implementation work, write a temporary
public workflow checker, then remove generated data and temporary runtime
programs after the checks complete.


## FMT-12 / FMT-13 / FMT-18

The retained reference programs are
[external/lcms_reference.c](external/lcms_reference.c) and
[external/ocio_reference.cpp](external/ocio_reference.cpp).
They link the pinned upstream engines independently of Photospider. Neither
program links production operation adapters, private allocation/work hooks,
the closed OCIO resolver or the production gamut correction.

### Reproducible stock-engine build

[external/CMakeLists.txt](external/CMakeLists.txt) builds both reference
programs. It requires CMake 3.24 or newer, Python 3, a compatible C11/C++17
Clang toolchain, and the extracted pinned sources. The validated build uses
AppleClang 21 on ARM64. Run these commands from the repository root after the
normal color-engine configuration has populated `build/format-deps`:

```sh
cmake -S oracle/ops/format_color/external \
  -B build/ops-dev/reference-recipe-build \
  -DPS_COLOR_REFERENCE_DEPS="$PWD/build/format-deps" \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DCMAKE_PROJECT_INCLUDE=
cmake --build build/ops-dev/reference-recipe-build \
  --target lcms_reference ocio_reference -j8
mkdir -p build/ops-dev/reference-recipe-fixtures/lcms
mkdir -p build/ops-dev/reference-recipe-fixtures/ocio
build/ops-dev/reference-recipe-build/lcms_reference \
  build/ops-dev/reference-recipe-fixtures/lcms
build/ops-dev/reference-recipe-build/ocio_reference \
  build/ops-dev/reference-recipe-fixtures/ocio
```

`PS_COLOR_REFERENCE_DEPS` may instead point to another directory containing
these exact extracted source folders:

| Source folder | Pinned version |
| --- | --- |
| `Little-CMS-lcms2.19.1` | Little CMS 2.19.1 |
| `OpenColorIO-2.5.2` | OpenColorIO 2.5.2 |
| `yaml-cpp-0.8.0` | yaml-cpp 0.8.0 |
| `libexpat-R_2_7_2` | expat 2.7.2 |
| `pystring-1.1.4` | pystring 1.1.4 |
| `Imath-3.2.1` | Imath 3.2.1 |
| `minizip-ng-4.0.10` | minizip-ng 4.0.10 |
| `sse2neon-227cc413fb2d50b2a10073087be96b59d5364aea` | sse2neon pinned revision |

The complete archive URLs and SHA-256 values are in
[ColorEngines.cmake](../../../cmake/ColorEngines.cmake) and
[ColorOCIO.cmake](../../../cmake/ColorOCIO.cmake). Use the original extracted
archives, not the production patched engine source directories.

[prepare_reference.py](external/prepare_reference.py) copies stock OCIO into
the temporary build directory and changes CMake packaging/dependency
integration only. It verifies that all C/C++ source/header files remain
byte-identical to the extracted input. LCMS is compiled directly from the
stock source directory. Target-local `-fno-fast-math -ffp-contract=off` also
apply to the OCIO target: inherited directory flags alone are insufficient
because upstream configuration resets its C++ flags. AVX, AVX2, AVX512 and
F16C are disabled to match the admitted build settings; the ARM64 reference
uses upstream SSE implementations through the pinned sse2neon headers.
The reference build uses its ordinary toolchain runtime and does not import
Photospider's privately rewritten libc++.

### Corpus and numerical boundary

The LCMS program writes ten deterministic serialized ICC profiles and
`reference.tsv` with **1752 rows**. Profiles are reopened before evaluation,
so the reference consumes the same bytes as the workflow. Cases cover:

- Gray, RGB, CMYK, XYZ and Lab endpoints, RGB DeviceLink, and three/four-profile
  chains with forward internal Lab/XYZ abstract stages.
- Ordinary conversion and RGB/CMYK softproofing with all admitted intent/BPC
  settings, in both optimized and reference modes.
- Both public dtypes, signed/extended values, zero/subnormal cases and finite
  values that overflow a required engine boundary.
- The stock owning RGB/RGB gamut pipeline and an independent source-domain
  sampler for Gray→RGB, CMYK→RGB, RGB→CMYK and RGB→RGB. The latter expresses
  the pinned hull formula separately using stock helper transforms and CLUT
  interpolation, without calling the production patch.

The OCIO program writes a config, a LUT, a transform-tree fragment and
`reference.tsv` with **480 rows**. Six operation families are evaluated in
both directions, OPTIMIZATION_NONE/OPTIMIZATION_DEFAULT, and Float32/Float64.
Each point is applied through stock one-pixel `PlanarImageDesc` with absent
alpha. Binary64-to-binary32 input rounding is implemented independently with
integer ties-to-even arithmetic; binary32 output is widened exactly.
Cases include role/alias resolution, display/view, Look, NamedTransform,
standalone file, Matrix/Exposure group, signed/HDR inputs and rounding ties.

Headers identify the engine/boundary; each data row includes the operation
case/settings, dtype, input IEEE words and expected output IEEE words. LCMS
rows also record expected failure status. The synthetic CMYK profile checks
interfaces and math; it is not a calibrated printer profile.

The final independent comparison matched **2232 rows across 204 public
workflows**, with color and bypass bits checked separately. Workflow
cancellation, sparse tiles, resource-budget failures, cleanup/concurrency,
channel topology, inspection and configured-view validation are separate
temporary runtime checks. Their measured results and performance scope are
recorded in
[FMT-12/13/18 implementation](../../../docs/built-in_ops/02-format-color/FMT-12-13-18_implementation.md).

After validation, remove the temporary reference build, generated fixtures
and any runtime checking/benchmark programs. Retain only these independent
oracle sources, their build recipe and the documented evidence.
