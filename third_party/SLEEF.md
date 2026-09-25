# Private numeric math dependency

SLEEF 3.9.0, commit `906ca7512ee483296780a81a21b9ca715d40dfe1`:
[upstream source](https://github.com/shibatch/sleef/tree/906ca7512ee483296780a81a21b9ca715d40dfe1)

## Prepare the source before configuring

Builders must download the unmodified pinned upstream source into
`third_party/sleef/` from the Photospider repository root. This directory is
ignored by Git and is not included in Photospider checkouts or source archives.
SLEEF is a required source dependency; a system-installed SLEEF library does not
supply the internal source files used by the adapter. No separate SLEEF build or
installation is required.

Run these commands from the Photospider repository root with an absent
`third_party/sleef/` directory:

```sh
curl --fail --location \
  https://github.com/shibatch/sleef/archive/906ca7512ee483296780a81a21b9ca715d40dfe1.tar.gz \
  --output /tmp/photospider-sleef-3.9.0.tar.gz
mkdir -p third_party/sleef
tar -xzf /tmp/photospider-sleef-3.9.0.tar.gz \
  --strip-components=1 -C third_party/sleef
```

Alternatively, obtain the same pinned source on a connected machine and copy it
to this directory before an offline build. Keep the complete source tree,
including `LICENSE.txt`. Configure rejects a missing/incomplete source directory
or a version other than 3.9.0. Builders must use the pinned, unmodified commit;
the version check alone does not authenticate source contents. Source contents
remain part of the kernel cache build identity.

Photospider provides download commands only in documentation, with no download
script, CMake download, or submodule. CI prepares this prerequisite with a
separate `actions/checkout` step pinned to the same upstream commit.

## Private integration

License: `sleef/LICENSE.txt`. Installation copies this license to
`share/licenses/Photospider/SLEEF.txt` under the default install data directory.
Installed static/shared Photospider consumers need no SLEEF checkout or target.
`photospider_sleef.c` compiles the binary64 AdvSIMD or AVX2/FMA implementation
into a private object with translation-unit-local mathematical symbols.
The project checks ISA availability before dispatch. No upstream dispatcher,
thread pool, allocator hook, generated installed header or configure-time
network dependency is used. The kernel archive contains the adapter object.

Only exp, ln, sin, cos, tan, pow and atan2 u10 entry points are selected in the
adapter. The exp entry is used by binary64 NUM-04 and NUM-01 AST enclosures.
Trigonometric fast
paths restrict arguments to the common small-argument reduction so lane grouping
cannot select a different reduction. Unverified domains use strict evaluation.

NUM-04 Float32 exp additionally uses a normal-range SIMD polynomial adapted from
ik_llama.cpp `dad2cb3e55138cbdd7df988c66c0c3c54f6d34ad`, implemented in
`plugins/ops/01-numeric/exp_simd.cpp`. Its MIT notice is retained in
`IK_LLAMA_LICENSE.txt` and installed alongside SLEEF's license. It has no new
source-download prerequisite. The comparison backends remain removed. Float64
exp and NUM-01 expression exp use SLEEF binary64 enclosures with final-result
certification; the Float32 IQK certificate does not justify narrowing their
binary64 inputs. The [exp implementation and measured scope](../docs/built-in_ops/01-numeric/exp-performance.md)
include the rational error certificate, fallback domain and reproduction.
