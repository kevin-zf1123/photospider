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
