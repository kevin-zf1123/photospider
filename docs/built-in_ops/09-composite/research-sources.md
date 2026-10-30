# Compositing and keying mathematics

The member equations and [COM shared contract](op_specs/COM_common_contract.md) define these operations. Public formulas are distinguished from native algorithm choices. Production computation uses native FP32/FP64, while independent oracles provide strictly rounded reference values. No commercial application's layer, group or UI behavior follows from a shared mode name.

## Coverage and color blending

- Porter and Duff, [Compositing Digital Images](https://keithp.com/~keithp/porterduff/p253-porter.pdf), SIGGRAPH 1984: coverage factors and associated-color algebra.
- W3C, [Compositing and Blending Level 1](https://www.w3.org/TR/2024/CRD-compositing-1-20240321/), 21 March 2024: Porter-Duff equations, separable modes and RGB nonseparable constructions. COM explicitly defines its linear and encoded working-domain members, straight output and zero-alpha behavior.

Native additions include raw color/weight mass, unbounded artistic formulas, the two Hard Mix definitions, selected-coordinate whole-color comparison, coverage dissolve, retained-alpha color addition and image interpolation. Their formulas are fully specified by the member documents.

## Color coordinates

[FMT model mathematics](../02-format-color/op_specs/FMT-11_model_math.md) and [FMT RGB basis mathematics](../02-format-color/op_specs/FMT-10_basis_math.md) define CIELAB normalization, OKLab coordinates, white points and RGB geometry. COM uses those coordinate meanings without inheriting FMT's strict production arithmetic profile. Native Lab/OKLab hue/chroma substitutions and the three neutral-background policies are project definitions.

Whole-color selection compares linear Y, native OKLab L or normalized CIELAB l, depending on the member. An explicit model conversion is composed around a selector when the surrounding workflow uses a different color model.

## Matting

Levin, Lischinski and Weiss, [A Closed-Form Solution to Natural Image Matting](https://people.csail.mit.edu/alevin/papers/Matting-Levin-Lischinski-Weiss-PAMI.pdf), IEEE TPAMI 30(2), 2008, DOI [10.1109/TPAMI.2007.1177](https://doi.org/10.1109/TPAMI.2007.1177). The paper supplies the local color-line matting Laplacian. KEY fixes its own clipped 3x3 windows, exact known-label constraints, native numerical solve criterion and final alpha clamp. Those choices are explicit in the [KEY matting contract](op_specs/KEY_matting_contract.md).

## Guided refinement

He, Sun and Tang, [Guided Image Filtering](https://kaiminghe.github.io/publications/eccv10guidedfilter.pdf), ECCV 2010: local linear filtering through window means, variance and covariance. KEY-07D uses a scalar guide, cropped finite windows, a positive regularizer and explicit final [0,1] clamp. The two window stages determine support H(2r).

## Evidence, distance and spill

Scalar range selection, original-donor nearest background, aligned per-channel temporal median, regularized foreground estimation, green-excess suppression, chromatic-direction suppression and original-donor edge extension are explicit native formulas. Their confidence, failure and copy policies are defined locally; no private keyer or editor algorithm is inferred.

The full CIELAB distance key uses conventional DeltaE76 units, restoring the factor 100 for normalized l. The chroma-only key excludes the lightness coordinate. RGB difference is an explicitly different metric.

## Oracle arithmetic

The independent oracle uses exact rational arithmetic for algebra, Gaussian elimination and equality/tie decisions. Expressions involving square roots use integer/rational enclosures that certify the final nearest/ties-to-even bit pattern. Reference-domain diagnostics and small-system limits belong to the oracle tool and do not prescribe production validation or resource behavior.
