---
spec_schema_version: 1
id: NOI-random
kind: shared_operator_contract
category: 03-generation
status: Accepted
implementation_status: partially_implemented
clarification_status: specified
---

# Random identity: Philox4x64-10

Inherit [GEN common](GEN_common_contract.md). The core algorithm is Philox4x64-10. Production v1 fixes the counter layout, seed-to-key mapping and Float32/Float64 half-open mappings and open52 mapping below. The member selections and general integer rejection rule below are fixed. Integer sampling requires explicit draw_base and attempt-budget configuration with the nonoverlap premises below; it has no default budget or implicit consumer. The [oracle](../../../../oracle/ops/generation/README.md) tests the core against independent upstream answers; its exact addressed references follow these mappings. Reference validation does not establish operator registration or backend execution.

## Current implementation scope

GEN-01..04, NOI-01..04, PTH-01..02 and PTH-09A are implemented.
Other schemas and members in this shared contract retain their own implementation
status.

## Address range and identity

Global integer x/y are signed 32. frame, draw and stream are each 0..2^32-1, carried by existing Int64 parameters. channel is 0..65535 and internal domain is 8-bit; callers cannot override a member's domain. These ranges are unchecked caller preconditions; neither the producer nor consumer validates them. Encode x/y as their 32-bit two's-complement bit patterns.

The production v1 counter is the ordered tuple `(c0,c1,c2,c3)` of unsigned 64-bit words. Bit 0 is the least significant bit of each word. The formulas specify integer word values independently of host byte order; byte serialization is outside this layout contract.

| Word | Production v1 value |
| --- | --- |
| `c0` | `uint32_bits(x) | (uint64(uint32_bits(y)) << 32)` |
| `c1` | `uint64(frame) | (uint64(draw) << 32)` |
| `c2` | `uint64(stream) | (uint64(channel) << 32) | (uint64(domain) << 48)` |
| `c3` | `0` |

This directly encodes all 184 address bits without hashing. Bits 56..63 of c2 and all 64 bits of c3 are reserved and must be zero in v1. Assigning meaning to any of these 72 bits requires a new layout version.

The production v1 Philox key is the ordered tuple `(seed_bits,0)`. Seed retains its complete 64-bit bit pattern; a signed Int64 seed uses its two's-complement representation. No hash or additional mixing is applied when constructing the key. The second key word is zero; Philox itself still performs its prescribed ten rounds and key schedule.

Use global pixel/cell and logical channel, not rounded coordinates, SIMD lanes, node
addresses, clocks, thread IDs, tile IDs or traversal order. Candidate ordinal, iteration
and attempt mappings require explicit bounds. Random frame is a discrete identifier;
continuous time and rank-volume lookup frame have separate semantics. CPU/GPU integer
core outputs agree; floating transforms follow their named profiles. Address injectivity
does not mean unique individual samples or statistical independence across keys.

## Domain assignments

| Tag | Purpose |
| --- | --- |
| 1 | Uniform white samples |
| 2 | Box-Muller samples |
| 3 | Jittered grid |
| 4 | Finite-candidate rejection |
| 5 | FIFO Bridson |
| 6 | Seeded2D gradient lattice |
| 7 | Cellular sites |
| 8 | Poisson quantile |
| 9 | Integer looks |
| 10 | Artistic grain white source |
| 11 | Reserved for explicit random resource displacement; current rank lookups take explicit offsets |

The purpose partition is fixed. The following member selections use the production v1 counter, key and floating mappings. Single-sample members fix draw to zero and do not expose a user-adjustable draw parameter. Seed and stream remain explicit caller inputs.

| Member | Address and output selection |
| --- | --- |
| NOI-01A uniform | Global pixel x/y, logical channel, caller frame, draw=0; word 0 maps to halfopen24 or halfopen53. |
| NOI-02A Gaussian | Global pixel x/y, logical channel, caller frame, draw=0; word 0 maps to open52 u and word 1 to halfopen53 v. |
| GEN-08B jitter | Cell (i,j), channel=frame=draw=0; words 0/1 map to halfopen53 x/y offsets. |
| GEN-08C rejection | Candidate i uses x=i, y=channel=frame=draw=0, domain=4; words 0/1 map to halfopen53 x/y coordinates in bounds. With K<=2^31-1 and 0<=i<K, x is representable. Changing max_count preserves the candidate prefix. |
| GEN-08D Bridson | Initial point uses x=y=0. Attempt a of iteration i uses x=i+1, y=a. channel=frame=draw=0, domain=5. Words 0/1 map to halfopen53 initial x/y or candidate radial/angular variables. |
| NOI-04B gradient | Valid corner x/y after any declared period modulo, channel=draw=0, caller frame, domain=6; word0 & 7 indexes the eight gradients in the member's fixed order. |
| NOI-05A cellular | Integer cell x/y, channel=0, caller frame, draw=0; words 0/1 map to halfopen53 x/y offsets. |
| NOI-08A Poisson | Global pixel x/y, logical channel, caller frame, draw=0; word 0 maps to open52. |
| NOI-09A speckle | Global pixel x/y, logical channel, caller frame, draw=j for look j=0..L-1; word 0 maps to open52. |
| NOI-09B grain source | Global pixel x/y, source logical channel, caller frame, draw=0, domain=10; use the Gaussian word 0/1 selection. |

Bridson iterations start at zero. Each iteration processes the earliest active point; increment the iteration after accepting one candidate or exhausting that point's attempts. With max_iterations in 0..2^31-1 and attempt in 0..k-1, k<=256, every encoded coordinate is representable. If another iteration is needed after its budget is exhausted, fail without publishing a successful prefix.

The two-sample selections leave words 2/3 unused. The one-sample selections leave words 1/2/3 unused. Never borrow adjacent domains after exhausting a draw range.

## Finite floating grids

The mathematical grid targets are halfopen24 j/2^24 for Float32 and halfopen53 j/2^53
for Float64, with the corresponding integer j range. open52 is (2j+1)/2^53 for
j=0..2^52-1, strictly in(0,1). These values are exactly representable.

Production v1 halfopen24 takes `j=word0>>40` from the first unsigned 64-bit Philox output word and returns exactly `j/2^24` as Float32. Production v1 halfopen53 takes `j=word0>>11` and returns exactly `j/2^53` as Float64. A halfopen53 helper applied to a member-selected output word uses the same high-53-bit extraction; each member must still specify its word selection. Production v1 open52 takes `j=w>>12` from a member-selected unsigned 64-bit output word w and returns exactly `(2*j+1)/2^53` as Float64. Native Float32 need not equal a cast of Float64. Box-Muller uses one block per logical channel: `u=open52(word0)` and `v=halfopen53(word1)`, with the cos branch and no neighbor-pair caching. Words 2 and 3 are unused. This selection applies to domain 2 and to the domain 10 artistic-grain Gaussian source; draw is fixed to zero. Finite-tail and statistical-grid differences from ideal continuous distributions must be reported rather than hidden.

Uniform integer selection targets [0,n) for positive Int64 n (1..2^63-1). Define the exact integer threshold t=2^64 mod n. At attempt a starting from zero, use draw=draw_base+a and take unsigned 64-bit word 0. Reject w<t; otherwise return w mod n. Words 1/2/3 are unused. Keep all other address fields and the key fixed across attempts. Compute the threshold and draw addition without wrapping. Each consumer must define draw_base, disjoint address allocations for distinct selections, and a finite attempt budget. The caller must choose draw_base and an attempt budget whose addressed attempts fit uint32. Exhausting a valid attempt budget fails; this is algorithm control, not address validation. Out-of-range configuration has no prescribed failure or wrapping behavior. Draw/algorithm limit failure remains distinct from host ResourceExhausted.

GEN-08/NOI-05 point placement explicitly rounds to Float64 then corrects a rounded upper
cell endpoint with nextDown. A representable interior is an unchecked input-domain premise. This
lattice rule does not alter the normal final rounding of periodic gradient coordinates.
Rank midpoint mapping has its own specified open-range rule.

## Core and sequence acceptance

Validate the official ten-round algorithm/constants using independent 4x64 known
answers, then separately validate packing injectivity, seed/key mapping and float grids.
Test signed coordinate limits, all 32-bit frame/draw/stream bits, channel/domain
separation within the supported domain, nonzero origin, ROI and order invariance. Reference-tool rejection of out-of-domain probes is not a runtime requirement. Cache
identity includes core/layout/grid/member/resource versions and all relevant parameters;
seed alone is insufficient. The old4x32 sequence is not a compatibility reference.
Passing core KATs and finite mapping tests does not prove all-domain numerical admission or runtime delivery.

Sources: [Random123](https://github.com/DEShawResearch/random123),
[Philox4x64](https://thesalmons.org/john/random123/releases/latest/docs/structr123_1_1Philox4x64__R.html).
