# Numeric and curve oracles

This directory contains the Python references and acceptance drivers for the
built-in numeric and curve operations. Run commands from the repository root;
each oracle's executable arguments and expected result are documented beside
the corresponding C++ workflow in the [numeric workflow README](../../../examples/numeric_workflow/README.md).

`math_oracle_support.py` supplies the shared MPFR binding. `exp_bound.py` and
`trig_bound.py` compute analytic bounds for the respective native kernels.
The `*_oracle.py` programs hold the independent mathematical references and
candidate comparison drivers. The C++ workflows, benchmarks, and measurement
scripts remain in `examples/numeric_workflow/`.
