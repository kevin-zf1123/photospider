# Oracle provenance

The mathematical helper source is the supplied `photospider-03-generation-drafts.zip`.
Executable behavior and supported subsets are described by this suite's README and
coverage table; generated logs are not reference fixtures.

Philox4x64-10 multipliers, Weyl increments and round ordering were checked against
[Random123 philox.h](https://github.com/DEShawResearch/random123/blob/main/include/Random123/philox.h).
The three ten-round known-answer cases in `philox4x64_kat.json` were transcribed
from [the authors' kat_vectors](https://github.com/DEShawResearch/random123/blob/main/tests/kat_vectors)
from the upstream known-answer inventory. Zero/all-one/pi-word cases test the core independently of our Python
implementation and experimental address layout. No upstream implementation is vendored.

Perlin's 256-entry numerical permutation follows
[the author's reference](https://mrl.cs.nyu.edu/~perlin/noise/).
The oracle evaluates the complete exact polynomial with Fraction arithmetic;
it does not promise Java intermediate-rounding identity. Compare the table with
the committed [specification data](../../docs/built-in_ops/03-generation/op_specs/NOI_perlin2002_permutation.md).

Remaining fixtures are hand analytic values, rational derivations or explicitly
labelled finite diagnostics. High-precision mpmath values, ordinary-math Bridson
candidates and synthetic rank spectra are not certificates. No third-party
production blue-noise asset, license approval or whole-domain admission is implied.
