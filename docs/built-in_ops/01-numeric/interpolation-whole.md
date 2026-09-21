# NUM-08 Whole execution

The six formal `numeric.mix_*` and `numeric.smoothstep_*` keys use synchronous
Whole callbacks, metadata-only shape/dtype/profile checks, one fixed arithmetic
workspace and complete packed output. There are no dependency maps or retained
continuations. Output remains `values`, with the input Float32/Float64 dtype,
shape and empty facets. The public workflow directly authors formal keys and
uses existing `broadcast_node` helpers. Legacy `field.smoothstep` is separate.

Nonempty requests collect and typed-validate all three complete inputs; Empty
reads no payload and invokes no callback. The executor projects the complete
result to consumer coordinates. Any source edit invalidates all observed
outputs. Invalid factors/edges anywhere, including outside a projection, fail
with InvalidArgument/InvalidDomain/Run and no Atom key; diagnostics preserve
port, raw bits and global coordinate. No partial output is published.

Eager mix input behavior was explicitly selected for this migration: unselected
endpoint source/typed failures are visible and may precede callback factor
checking. Mathematical selection stays unchanged. t=-0/0 copies a, t=1 copies b,
including sNaN and signed zero without quieting. Interior NaN/Inf precedence,
one-round exact `(1-t)*a+t*b`, and smoothstep's edge validation before input NaN
remain unchanged. The original exact linear/cubic workspace and accelerated
final-error gate are reused. No NUM-14 certificate or Float64 narrowing is added.
Scalar/NEON/AVX2 facilities and matrix Scalar/Accelerate/SME targets remain.

Capacity includes three full input collections, full N × dtype-width output and
one fixed InterpolationMath workspace. This can increase sparse-request memory
and work. Allocator-owned scratch/output release on failure; owned results may
outlive context. Work/cancellation checks occur per element, in exact arithmetic
and before publication. Per-value numeric counters are N/A for these callbacks.

## Executed validation

2026-09-21, Apple M5 arm64, macOS 27.0 (26A5425a), Clang 21.1.3,
RelWithDebInfo, no fast math and disabled FP contraction for numerical code:

- Strict and Apple each passed 5,244 independent IEEE/Fraction oracle cases,
  using their specified exact/final-FP32-scaled acceptance rules.
- Both public manual suites passed composition, eager source failure and
  source-before-factor priority, Q-outside invalid factor/edge, edge-before-NaN,
  full source support/dirty, Empty, typed-invalid unselected endpoints,
  unselected-cache invalidation and factor mutation.
- Direct tests passed endpoint sNaN copying/fenv, exact cubic fenv, negative/
  zero/unaligned strides, shifted origins, arbitrary singleton strides and
  65-element Float32 all-port tails. Direct partial output rejects; public
  sparse projection retains global coordinates.
- Complete output/scratch/work budget rejection and cancellation after arithmetic
  began released unpublished Payload. The cancellation watcher does not identify
  an exact instruction inside a single refinement.
- Five focused CTests passed: numeric operations, dependency sampling, execution
  demand, resources, compiler. ClangFormat 21/cpplint and independent code/spec
  review passed. No current x86, installed-consumer or full release matrix ran.

```sh
DEVELOPER_DIR=/Library/Developer/CommandLineTools cmake --build build/clang21-numeric \
  --target photospider_numeric_interpolation -j8
build/clang21-numeric/examples/numeric_workflow/photospider_numeric_interpolation _strict
python3 examples/numeric_workflow/interpolation_oracle.py \
  build/clang21-numeric/examples/numeric_workflow/photospider_numeric_interpolation _strict
```

Repeat with `_accelerated_apple_silicon`. The editable public workflow gives
`smoothstep=[0,0,.15625,.5,.84375,1,1]`, then
`mix=[10,10,11.5625,15,18.4375,20,20]`. Historical WSL/installed-consumer passes
were for the pre-Whole staged implementation.

## Public/core timings and profiler

Float64 `[N]` with dyadic `t[i]=(i%17)/16`. Smoothstep takes x=t and edges 0/1;
mix takes endpoints 0/1 and factor t. Independent expected bits come from the
exactly representable dyadic polynomial `t*t*(3-2*t)` or t. Every output was
checked after every completed timed execution. Before registration objects are
from `5681916f`, linked against the same kernel. One worker, cache off, 1 GiB
public Payload limit, 2^40 dependency/run/Footprint work limits, 512 MiB dependency
state and default managed ResourceLimits. One warm-up plus seven timed samples.

Public timing excludes generation, compile, freeze and verification. Core timing
uses the actual reusable InterpolationMath engine with prebuilt raw inputs,
output stores and a work counter; public allocation, collect and managed work
admission are excluded. N=1 is an endpoint and reaches clock resolution. Public
and core are separate measured layers, not a subtraction-derived overhead.
Milliseconds, median of seven samples:

| Operation | N | Dependency public | Whole public | Core | Whole public min–max |
|---|---:|---:|---:|---:|---:|
| smoothstep | 1 | .103292 | .045500 | resolution limited | .040459–.059708 |
| smoothstep | 16 | .176208 | .085167 | .032041 | .083041–.100833 |
| smoothstep | 64 | .338500 | .224500 | .131083 | .206792–.240125 |
| smoothstep | 256 | 1.092710 | .683125 | .516708 | .658666–.688625 |
| smoothstep | 16384 | 61.040800 | 40.959200 | 33.066700 | 40.704700–41.626700 |
| mix | 1 | .106084 | .042167 | resolution limited | .038500–.054834 |
| mix | 16 | 1.718670 | .053875 | .005666 | .042125–.058958 |
| mix | 64 | 9.478370 | .078250 | .022500 | .067834–.097459 |
| mix | 256 | 83.201900 | .151208 | .084083 | .130959–.156167 |
| mix | 16384 | not sampled | 6.325790 | 5.714420 | 6.230500–6.344750 |

Smoothstep N=16,384 controlled Payload grew from 138,928 to 531,976 bytes. Mix
N=256 grew from 9,904 to 15,880; Whole N=16,384 is 531,976. These are not RSS.
Large legacy mix, 4096², strict and cross-platform performance were not sampled.

Two 12-second Instruments runs at N=16,384 used continuous cache-off Whole calls
and checked the first output; timings above used separate fully checked runs.
Inclusive ratios overlap and cannot be summed:

- Smoothstep: 11,983 execution-chain samples; callback 99.82%, numerical
  smoothstep 98.45%, collect .03%, ResourceBudget::consume 13.16%. Leading leaves
  are multiply_fixed<104> (3,724) and fixed-integer subtract (1,433). Memmove
  appeared 1,084 times; its individual call-site attribution was not established.
- Mix: 11,989 execution-chain samples; callback 98.65%, numerical mix 89.59%,
  collect .06%, work admission 3.41%. Leading leaves are set_product/subtract/add
  (2,431 / 2,419 / 1,763). Remaining cost is directly observed fixed-integer
  arithmetic; no alternative floating formula is inferred to satisfy the contract.

Ignored local artifacts: `build/interpolation-whole/{scale.cpp,core.cpp,
build_scale.py,timings.csv,smoothstep.trace,mix.trace}`, exported XML/sample JSON/
summary files, oracle/manual logs and `ctest.log`. Public smaller fixtures are
also available through the category benchmark with `apple selected smoothstep`
and `apple extended mix`, at N=1/256 and its separately documented budgets.
