# Repository-owned operations

Directories follow the responsibilities in `docs/built-in_ops`. Each registered
C++ operation owns one `.cpp`, named by replacing the key's dot with an underscore.
`src/lib/plugin/builtin_operations.cpp` only aggregates registration functions;
it contains no operation callbacks. Shared algorithms and addressing adapters
are private headers, never installed ABI. Registration schemas remain next to
individual operation callbacks/specializations.

| Directory | Responsibility |
| --- | --- |
| `00-foundation` | Core execution probes, generic host/addressing helpers |
| `01-numeric` | Array arithmetic, curves, expressions, LUTs and smoothstep |
| `02-format-color` | Channel routing, alpha association and color models |
| `03-generation` | Coordinate and constant fields |
| `04-mask-morphology` | Coverage Boolean, morphology, connected components and attributes |
| `05-filter` | Field/image spatial filters and fixed-kernel algorithms |
| `07-grade` | Exposure gain and levels |
| `08-transform` | Image/mask downsampling |
| `09-composite` | Opacity, masks, image mix, source-over and brush stamping |
| `10-analysis` | Histograms and out-of-range counts |

`rgba32f` is the existing independently buildable C operation-module/Metal
backend package. Its C ABI translation unit and shaders retain their separate
backend packaging; the eight corresponding C++ operations follow the categories
above. New first-version basic operations are CPU only.

Public behavior and examples are documented in `docs/kernel-architecture`,
including `Basic-Operations.md`. CMake's explicit PHOTOSPIDER_OPERATION_SOURCES
list is reused for the product and test kernel and for strict floating-point
compile options. Changes to private helper headers participate in cache build
identity through the existing recursive source inventory.
