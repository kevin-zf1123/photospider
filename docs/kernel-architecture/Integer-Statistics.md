# Integer histogram and global grade

[Chinese reader version](zh/Integer-Statistics.zh.md).

## 1. Scope and ownership

The statistics API describes integer-coded scalar rasters, sparse histograms, global parameters, and graded output. Histogram and Grade read tensor windows from Result inputs; Parameters reads the Histogram Result's fields. Each producer publishes a Result under the execution root's capacity, work, I/O, and stage budgets. A Result's association stores source ObjectIds as facts; it does not retain the referenced source payload.

## 2. Data layout and memory

```cpp
struct StatisticsSpec final {
  std::uint64_t height = 1, width = 1;
  std::uint32_t bins = 8;
};
enum class StatisticsRepresentation : std::uint32_t {
  Histogram = 1, Parameters, Graded
};
Result<SchemaTemplate> statistics_schema(
    StatisticsRepresentation representation, const StatisticsSpec& spec);
Result<double> statistics_mean(std::int64_t total, std::int64_t count);
```

`StatisticsSpec` requires positive H and W and 1 to 65536 bins. The raster sample product is bounded by `(INT64_MAX-4095)/8`. Histogram and Grade numeric inputs are single-tensor Results with no fields, batch axes, or tensor facets. Each tensor has the exact HW shape in `StatisticsSpec`; Histogram takes facet-free Int64 values and a facet-free UInt8 mask, while Grade takes facet-free Int64 values. Parameters consumes a `photospider.integer_histogram` Result, and Grade also consumes the matching `photospider.integer_statistics` Result. A nonzero mask byte selects a sample; selected values must lie in `[0,bins)`. Masked-out values do not participate. The operation does not infer color or apply a transfer function.

| Result schema | Fields and publication |
| --- | --- |
| `photospider.integer_histogram` | RuntimeCount Int64 `bin`; matching Int64 `count` rows; CompleteBundle |
| `photospider.integer_statistics` | Int64 `[count,total,valid]` and Float64 `mean`; CompleteBundle |
| `photospider.graded_scalar` | Fixed H*W Float64 `pixels`; HW domain; StablePrefix |

Histogram IDs are strictly increasing and only positive-count bins are stored. An absent bin means zero after the complete histogram seals. Empty selection yields no rows. Statistics for an empty selection have `[0,0,0]` and a nonsemantic zero mean; a nonempty selection of zeros is valid with mean zero. Physical Result window size is not part of schema identity.

Each statistics output owns its published field storage. A loaded field `CpuStorage` also retains its read plan and Result implementation, so it remains readable after the `ResultRef` and execution context are released. The loaded window keeps its backing live until that window is released; the ObjectId association alone does not keep an upstream Result's payload alive.

## 3. Execution and state machine

```text
variant Result binding
          |
          v
 example source operations --Need--> Int64 and UInt8 tensor Results
          |                                  |
          +------------------+---------------+
                             v
                  histogram producer
                             |
                    sparse Histogram Result
                             v
                   parameter producer
                             |
                     complete parameters
                             |
                    grade producer
                             |
                  Float64 Result / stable prefixes
                             |
                    streaming Result sink
```

The histogram factory checks a necessary request-stage lower bound before binding inputs. With `S=H*ceil(W/512)*ceil(bins/512)`, it must fit the source request polls plus a completion poll. If `S>=1,000,000`, the factory returns `ResourceExhausted`. The implementation uses division checks to avoid overflow. Passing admission is not a reservation: the effective producer limit is also bounded by the dependency option (default 4096 stages) and root stage budget. Later I/O, work, stage, or capacity exhaustion prevents a complete histogram publication.

Histogram and Parameters use CompleteBundle and Conservative support expressed through typed Tensor, Field, and Descriptor relations. Parameters waits for the complete histogram, then checks sparse IDs, positive counts, totals, integer overflow, and selected count against HW. Grade validates the complete global parameter Result before producing any pixel prefix. Each output pixel depends on its source sample plus shared parameter fields and descriptor support. Descriptor observations are separate from data rows, even for empty collections. If later pixel work fails, an already published stable prefix remains valid.

Histogram source operations receive tensor Needs in row-bounded strips of at most 4096 bytes per field, independently of the selected Result window size. Other source reads and Result field I/O use at most `min(user_page_bytes,4096)`. The 24-byte `count_total_valid` field record is indivisible; a smaller selected window fails with `ResourceExhausted`. Histogram counters and temporary state, source/result windows, mandatory backing, and every initialization/reset/scan consume root capacity or work as applicable.

## 4. Algorithms and math

The `bin_ranges_512` producer reserves `min(bins,512)` Int64 counters, rereads the immutable source for each bin range, and appends positive bins in increasing order. It uses at most 4096 bytes of counter storage. The work is `O(N*ceil(B/512)+B)` for N raster samples and B bins. This trades repeated source reads for bounded counter memory; it does not allocate a full B-counter array or an unaccounted input spool.

Parameters computes `D=sum(count)` and `T=sum(bin*count)` with checked nonnegative Int64 arithmetic. For `D>0`, the mean is the correctly rounded binary64 value of the exact ratio:

$$
\mu = \operatorname{round}_{64}(T/D), \qquad 0 \le T/D < 65536.
$$

Integer long division extracts 53 significant bits and compares the remainder for ties-to-even. Converting T and D separately to binary64 before division can produce a different result. The public `statistics_mean(total,count)` helper accepts nonnegative Int64 total and positive Int64 count in the same ratio domain. Empty input publishes `[0,0,0]` and a nonsemantic zero mean; nonempty zero-only input is valid with mean zero.

`statistics.grade` requires a finite nonnegative Float64 target, valid parameters, and positive mean. Before using the rounded mean it checks the exact integer ratio against `bins-1`. It computes

$$
\mathrm{gain}=\operatorname{round}_{64}(target/mean),\qquad
 y_i=\operatorname{round}_{64}(\mathrm{gain}\cdot\operatorname{round}_{64}(x_i)).
$$

The implementation uses nearest ties-to-even, gradual underflow, no contraction, and restores the caller's floating-point environment. Nonfinite gain or output is `ArithmeticOverflow`; empty, zero-mean, or inconsistent parameters are `InvalidDomain`. It does not clamp or substitute an approximate result.

## 5. Limitations and non-goals

- Input is an integer-coded scalar domain; color, luminance, and transfer conversion are outside this operation.
- Histogram's fixed stage precheck is recipe-specific; Parameters and Grade do not apply it.
- A measured Float64 grade has no certified numerical error bound. Managed capacity does not bound process RSS.
- Physical page geometry does not change schema identity or shared-result keys.

See [the statistics workflow](../../examples/statistics_workflow/README.md) for the public entry point and runnable usage.
