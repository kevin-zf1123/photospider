PixelOE source snapshot: https://github.com/KohakuBlueleaf/PixelOE
Revision: 0239787b8bb3e0c0dac615a33c896311d40cb46e
Author: Shih-Ying Yeh (KohakuBlueLeaf) and contributors.
License: Apache-2.0, reproduced in LICENSE.PixelOE.

The shaders directory is the upstream Slang shader tree. The native pipeline,
coefficient generator and CPU dispatch generation outside this directory adapt
upstream Python orchestration and CPU ABI discovery. Photospider changes the
host allocation, planar I/O, metadata, cancellation, numerical build policy and
packaging. Any shader edits must be listed here.
