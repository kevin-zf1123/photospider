#pragma once

#include "photospider/data/statistics.hpp"
#include "photospider/plugin/operation_registry.hpp"

namespace ps {
enum class StatisticsOperation : std::uint32_t {
  Histogram = 1,
  Parameters,
  Grade
};
/** @brief Ready-to-register statistics.histogram/parameters/grade CPU stages.
 * Histogram reads immutable Int64 values and UInt8 mask and seals sparse
 * counters. The named bin-range recipe scans once per 512-bin range and admits
 * min(bins,512)*sizeof(Int64) Payload bytes for counters. Source/output windows
 * are bounded, at most 4096 bytes per field. Inputs must support snapshot
 * rereads. Parameters validates the sealed histogram and computes checked
 * integer count/total and a correctly rounded mean. Grade takes values and
 * complete parameters, requires finite nonnegative Float64 parameter "target",
 * valid statistics and mean>0, then publishes Float64 rows as stable prefixes.
 * Grading uses nearest binary64 target/mean and gain*converted_input. Overflow
 * fails explicitly; no CertifiedBound is claimed. Failed/empty statistics are
 * never hidden as an empty graded image. Every Result association is sealed;
 * dependency support is Conservative, with shared global support plus a compact
 * identity relation for each grade sample. Physical windows do not change keys.
 * @return A definition, or the schema validation error. Histogram additionally
 * returns ResourceExhausted before source binding when
 * H*ceil(W/512)*ceil(bins/512) >= 1000000: required source polls alone leave no
 * final poll within this recipe's fixed stage cap. This lower-bound check does
 * not admit output I/O or guarantee completion under the Run's resource limits.
 * Parameters, Grade and statistics_schema do not impose this scan restriction.
 */
PHOTOSPIDER_API Result<OperationDefinition> make_statistics_operation(
    StatisticsOperation operation, const StatisticsSpec& spec);
}  // namespace ps
