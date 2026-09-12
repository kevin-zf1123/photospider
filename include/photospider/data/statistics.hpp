#pragma once

#include "photospider/data/result.hpp"

namespace ps {
/** @brief Integer-coded scalar raster and fixed half-open bins [j,j+1).
 * Values are Int64 HW and mask is UInt8 HW; nonzero mask selects a sample.
 * Selected values must be in [0,bins), with bins in 1..65536. Masked-out values
 * do not participate in statistics. No transfer-function/color conversion is
 * implicit. Dimensions are positive and checked before state allocation/I/O.
 */
struct StatisticsSpec final {
  std::uint64_t height = 1, width = 1;
  std::uint32_t bins = 8;
};
enum class StatisticsRepresentation : std::uint32_t {
  Histogram = 1,
  Parameters,
  Graded
};
/** @brief Sparse ordered histogram, complete mean, or monotone graded pixels.
 * Histogram fields are Int64 bin IDs and matching positive counts, with
 * RuntimeCount/FieldRows. Absent bins mean zero; an empty histogram has no
 * rows. Parameters fields are Int64[count,total,valid] and a Float64 mean.
 * Empty statistics have valid=0 and a nonsemantic zero mean placeholder.
 * Nonempty zero-mean statistics remain valid. Graded Float64 rows use HW raster
 * order. Physical window size is excluded from schema identity.
 */
PHOTOSPIDER_API Result<SchemaTemplate> statistics_schema(
    StatisticsRepresentation representation, const StatisticsSpec& spec);
PHOTOSPIDER_API Result<StatisticsSpec> statistics_spec(
    const SchemaTemplate& schema);
/** @brief Correctly rounds nonnegative integer total/count to binary64.
 * count>0 and total/count<65536; inputs are at most INT64_MAX. Integer long
 * division determines 53 significant bits and ties-to-even before exact ldexp.
 * It does not divide separately rounded large numerator/denominator doubles.
 * No numerical error bound for subsequent floating-point grade is implied.
 */
PHOTOSPIDER_API Result<double> statistics_mean(std::int64_t total,
                                               std::int64_t count);
}  // namespace ps
