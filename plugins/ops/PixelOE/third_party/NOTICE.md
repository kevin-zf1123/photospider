PixelOE source snapshot: https://github.com/KohakuBlueleaf/PixelOE
Revision: 0239787b8bb3e0c0dac615a33c896311d40cb46e
Author: Shih-Ying Yeh (KohakuBlueLeaf) and contributors.
License: Apache-2.0, reproduced in LICENSE.PixelOE.

The shaders directory is the upstream Slang shader tree. The native pipeline,
coefficient generator and CPU dispatch generation outside this directory adapt
upstream Python orchestration and CPU ABI discovery. Photospider changes the
host allocation, planar I/O, metadata, cancellation, numerical build policy and
packaging. Any shader edits must be listed here.

The following additional GPU shaders were copied verbatim from the upstream
Slang tree at revision
[`940718df7b4e258a78947dc9186bcb4e039cf53a`](https://github.com/KohakuBlueleaf/PixelOE/tree/940718df7b4e258a78947dc9186bcb4e039cf53a/src/pixeloe/slang):

- `downscale/kcentroid_atomic.slang`
- `downscale/kc_common.slang`

These files are covered by the upstream Apache-2.0 license reproduced in
`LICENSE.PixelOE`.

Photospider also modifies the upstream shader `dither/palette.slang`:
`reflect_col` maps every index to zero when the image width is one. This keeps
single-column error-diffusion reads on the only available column.
