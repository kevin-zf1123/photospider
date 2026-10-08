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
 * All operation inputs and outputs use Result. Histogram takes two bindings:
 * one Result with one unbatched, facet-free Int64 HW tensor and one with one
 * unbatched, facet-free UInt8 HW tensor; neither Result has fields. A nonzero
 * mask byte selects a value, which must be in [0,bins). Histogram seals sparse
 * Int64 bin/count fields. Its bin-range recipe scans once per 512-bin range
 * and admits min(bins,512)*sizeof(Int64) Payload bytes for counters. Source
 * strips are row-bounded to at most 4096 bytes per field; field I/O respects
 * the selected Result window.
 * Parameters consumes the Histogram Result and computes checked integer
 * count/total plus a correctly rounded mean. Grade takes one unbatched,
 * facet-free Int64 HW tensor Result with no fields and a complete Parameters
 * Result, requires a finite nonnegative Float64 parameter "target", valid
 * statistics, and mean>0, then
 * publishes Float64 rows as stable prefixes. Grading uses nearest-binary64
 * target/mean and gain*converted_input. Overflow fails explicitly; no
 * CertifiedBound is claimed. Failed or empty statistics are not published as
 * an empty graded raster. Typed Tensor, Field, and Descriptor relations provide
 * Conservative dependency support, including global histogram/parameter
 * support and per-sample grade support. Physical windows do not change keys.
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
