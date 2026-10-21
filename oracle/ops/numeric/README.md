# Numeric and curve oracles

This directory contains independent mathematical references, deterministic input
samples and numerical acceptance predicates for numeric and curve operations.
`reference_cases()` yields structured input/expected pairs; modules with separate
`cases()` and `reference()` functions also expose those calculations directly.
IEEE words, structured values and symbolic errors retain each reference's
existing representation. No module launches a native runner or reports that an
operation implementation passed. Write implementation adapters temporarily.

`common/mpfr.py` supplies the shared MPFR binding. `num04/exp_bound.py` and
`num04/trig_bound.py` retain their analytic certificate entry points.
`common/accuracy.py` retains strict and accelerated error bounds;
`num04/exp64.py` retains exact fallback and input-distinction predicates, and
`crv08/reference.py` retains its monotonicity predicate. `num12/reference.py`
retains exact sorting and profile-dependent quantile acceptance.
`num01/reference.py` requires exact axis/error values and applies the selected
profile only to expression results.

Run from the repository root and import `oracle.ops.numeric.<group>.<module>`. Mathematical
references may require the documented MPFR library. Generator and certificate
commands remain independent of compiled operator executables.

## Native color migration

`common/color_geometry.py` independently classifies white/primary geometry with
Fraction arithmetic; it does not import the production TensorDescription resolver.
The former color_array.py filename is retired along with the old public codec.

`crv06/reference.py` keeps exact coordinate/rational-hue interpolation and IEEE
rounding; its CIELAB/CIELCh cases store `l=L*/100`, including negative/HDR l and
unchanged large a*/b*/C*. `crv06/rgb.py` accepts `with_alpha` and computes straight
RGB/RGBA input and output, retaining internal linear-light alpha weighting,
direct endpoint rules, hidden transparent colors and tiny-alpha cases. No
premultiplied storage or output-association oracle option remains.
`crv06/icc.py` provides independent printing-profile fixtures.

`crv07/reference.py` includes normalized Lab/LCh identity tables and axes.
`crv09/reference.py` uses the same native units for grid/extra points, table
values and error coordinates, with explicit source/table dtype rounding.
The bake tolerance remains one scalar atol plus the existing relative term;
only lightness changes scale, so callers must reauthor mixed-coordinate
tolerances instead of automatically dividing every threshold by 100.

To enumerate independent references without a native adapter:

```python
from oracle.ops.numeric.crv06 import reference as coordinates, rgb
from oracle.ops.numeric.crv07 import reference as lut
from oracle.ops.numeric.crv09 import reference as bake

for module in (coordinates, rgb, lut, bake):
    pairs = list(module.reference_cases())
    print(module.__name__, len(pairs))
```

Some reference pairs describe mathematical domain/overflow boundaries. Fixed NUM
invocations treat those invalid numerical samples as outside caller premises;
a temporary implementation adapter must select valid cases explicitly. Static
description, old-codec, old report-version and resource admission are separate
rejection checks. No oracle alone establishes runtime acceptance. Executed
migration evidence is in
[NUM-CRV_color_migration.md](../../../docs/built-in_ops/01-numeric/NUM-CRV_color_migration.md).
