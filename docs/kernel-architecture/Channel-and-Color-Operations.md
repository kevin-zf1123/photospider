# Channel, alpha and color operations

The default registry provides ten CPU Whole operations. They use the shared
ABI/Traits 8 descriptor inference in [ADR 0020](../adr/0020-composable-operation-foundations.md),
including public compile/execute, direct invocation and C contract loading.
The [Chinese mirror](zh/Channel-and-Color-Operations.zh.md) follows this document.
All outputs are packed, nonempty Values allocated through the host. Input reads
honor byte offset, storage origin and signed/zero strides without alignment
assumptions. There is no implicit resize, dtype conversion or GPU implementation.

| Key | Input → output | Required static parameters and meaning |
| --- | --- | --- |
| `channel.extract` | Typed Float32/64 Image/VectorField/ComplexField HWC → same-dtype HW | Int64 `index`, 0..63 and below input C. Output is a ScalarField with the selected role/unit. Image alpha produces canonical `coverage_semantics()` for existing mask ports. |
| `channel.merge` | 2..4 equal-dtype/equal-shape Float32/64 HW inputs → HWC | String `semantic`, produced by `semantic_parameter(target)`. Target is Image (Float32, C=3/4), VectorField (Float32/64, C=2/3) or ComplexField (Float32/64, C=2). Inputs are unfaceted generic arrays, ScalarFields or coverage masks; known role/unit must match each target channel. Names need not match. |
| `channel.swizzle` | Float32/64 HWC → HWC with selected C | String `indices`, produced by `channel_indices_parameter({2,1,0,3})`. Repetitions are allowed; no implicit constants. Output metadata follows the selection rules below. |
| `alpha.associate` | Straight RGB Float32 HWC → coverage-premultiplied RGB | None. Multiply RGB by alpha; zero alpha yields zero RGB. |
| `alpha.unassociate` | Coverage-premultiplied RGB Float32 HWC → straight RGB | None. Divide RGB by every positive alpha; zero alpha yields zero RGB. No epsilon. |
| `color.assign` | Float32 HWC → typed Image with unchanged sample bytes | String `semantic`, produced by `semantic_parameter(target)`. Explicitly changes interpretation and validates the target sample domain. |
| `color.rgb_to_xyz`, `color.xyz_to_rgb` | Linear sRGB ↔ XYZ Float32 HWC | None. Both use the exact canonical D65 white from `rgba_semantics().white`. Input association must be none or straight. |
| `color.xyz_to_lab`, `color.lab_to_xyz` | XYZ ↔ Lab Float32 HWC | None. The input's explicitly declared positive XYZ white, normalized to Y=1, is retained. D65 and explicitly supplied D50 are supported without adaptation. |

All parameters shown are required; constructors supply choices explicitly.
Built-in merge rejects input counts outside 2..4 before callback entry or IR
publication: direct invocation returns `InvalidArgument`, and compilation returns
`TypeMismatch` under the existing arity checks. This bound applies to the built-in operation;
custom repeated groups retain their separately declared bounds within 1024.
Extraction/merging/assignment establish only their inferred typed facet and remove
unrelated annotations. Generic merge inputs with opaque interpretation are
rejected. Generic numeric arithmetic can remove a field's guarantees before an
explicit merge establishes its target and validates samples. This supports
extract → multiply → merge without retaining an invalid mask/image facet.

Swizzle preserves transformed typed metadata only when the resulting roles still
form a valid descriptor. Complete RGB↔BGR permutations preserve the image profile.
Repeated/missing roles, misplaced alpha, reversed complex components and other
unrepresentable selections produce an unfaceted generic HWC Value. Removing alpha
from straight RGBA can produce a three-channel Image. Removing alpha from
coverage-premultiplied RGBA produces generic data because the retained RGB is
still multiplied by alpha. Swizzle never unassociates samples. Typed source
samples remain validated before use; generic permutations preserve sample bits.

The canonical index String is comma-separated decimal: 1..64 indices, each
0..63, at most 191 bytes, with no spaces, signs or leading zeros. Public helpers
produce/parse it; callers need not assemble strings. `IndexListCount` uses the
same parser to infer C at compile time. The closed semantic rules describe
extract, swizzle, merge, alpha and color transformations independently of operation
keys. Rules/parameters enter existing compiler and result-cache identities.
The C contract uses the same enum vocabulary and inference; no arbitrary metadata
callback or new WorkflowDocument parameter variant is introduced.

Alpha operations preserve channel order, white, interpretation and alpha bits
(including negative zero). Straight zero-alpha hidden colors are deliberately
lost by association; this is not a lossless round-trip. Positive subnormal alpha
is divided directly under nearest-even/gradual-underflow mode. Overflow after
unassociation fails. The caller's floating environment is restored.

Color conversion reads the first three channels by role, so BGR and reordered
XYZ/Lab are supported. Outputs have canonical color names/roles (`R,G,B`, `X,Y,Z`,
`L,a,b`) and optional alpha named `A`; alpha bytes are copied unchanged. RGB↔XYZ
uses binary64 rational sRGB/D65 matrices. XYZ↔Lab uses the CIELAB piecewise function
with thresholds `216/24389` and `24389/27`, relative to the supplied white. These
formulae follow the [W3C color conversion reference](https://www.w3.org/TR/css-color-4/#color-conversion-code);
Photospider's explicit-white contract does not perform CSS's D65↔D50 adaptation.
Finite signed/HDR/out-of-gamut results are retained; there is no gamut clamp,
transfer encoding, ICC or OCIO processing. Every result is checked before
Float32 narrowing. Non-finite or unrepresentable results fail with a pixel index.

Malformed parameters/semantic payloads return `InvalidArgument`; valid but
incompatible model/association/white, channel bounds relationships or role/unit
contracts fail before callbacks (`TypeMismatch`, or `InvalidArgument` for an
out-of-range selection). Malformed units/transfer combinations are rejected by
the canonical semantic encoder itself. Invalid assigned/merged sample values and
numeric overflow return `OperationFailed`. Cancellation/resource errors retain
their codes, and unpublished output allocations are released.

## Executable public combinations

[test_color_operations.cpp](../../tests/integration/test_color_operations.cpp)
uses public Values, WorkflowDocument, Compiler and ExecutionContext. `graph()`
declares and binds inputs; `composition()` is the minimal extract/process/merge
example: four extraction nodes, a generic numeric multiply on red, then a merge
whose target is `semantic_parameter(rgba_semantics())`. Edit the multiplier or
connect another numeric node before merge; green, blue and alpha remain unchanged.
The same source is compiled against isolated installed static/shared packages.

```sh
cmake --build build/issue257-static --target test_color_operations -j 8
ctest --test-dir build/issue257-static -R '^test_color_operations$' --output-on-failure
ctest --test-dir build/issue257-static -R '^test_installed_consumer$' --output-on-failure
```

Replace `issue257-static` with an existing shared build to run the same public
consumer. Exit zero checks these independent expectations:

- `[-2,.5,4,.5]` with red multiplied by two becomes `[-4,.5,4,.5]` with the exact
  target RGBA facet; factor one round-trips exactly.
- Extracted alpha `.5,1` feeds the existing `mask.downsample_box` and produces `.75`.
- RGB↔BGR permutation round-trips bytes; repeated indices run and produce generic
  values; premul/straight alpha removal differ as documented.
- Straight `[-2,.5,4,.5]` associates to `[-1,.25,2,.5]`; tiny positive alpha can
  round-trip `1,-1,0`, while hidden colors at zero alpha are lost and alpha bits
  remain unchanged.
- Linear red converts to XYZ approximately `[.4123908,.2126390,.01933082]`;
  signed/HDR colors round-trip. Each declared D65/D50 white maps to Lab `[100,0,0]`,
  black maps to zero, and signed XYZ returns within the stated test tolerance.
- Merge metadata reports 2..4 inputs; valid 2/3/4-channel targets execute through
  direct and compiled calls, while counts 1/5 enter no callback.
- Float64 vector and Float32 complex fields use the same merge/extract contracts;
  padded, unaligned reversed-channel producers are read correctly.
- Invalid semantics, associations, parameters, role/unit assignments, dense
  allocation limits and cancellation fail without a partial successful result.

To convert premul RGB through Lab, compose `alpha.unassociate → color.rgb_to_xyz
→ color.xyz_to_lab → color.lab_to_xyz → color.xyz_to_rgb → alpha.associate`.
The white remains D65 throughout. For D50 Lab, begin with explicitly declared
D50 XYZ; RGB conversion requires a separately explicit adaptation, which is
outside this delivery.
