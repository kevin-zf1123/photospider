#include "photospider/data/quality.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <memory>
#include <utility>

#include "data/input_validation.hpp"

namespace ps {
namespace {
struct Header {
  QualityEvidence evidence = QualityEvidence::None;
  std::uint32_t snapshot_size = 0;
  std::uint64_t dimension = 0;
  double residual = 0, bound = 0;
  std::array<char, 256> snapshot{};
};
Header header(const std::shared_ptr<const CpuStorage>& storage) noexcept {
  Header result;
  if (storage)
    std::memcpy(&result, storage->bytes().data(), sizeof(result));
  return result;
}
Status invalid() {
  return {ErrorCode::InvalidArgument, "invalid numerical quality report",
          FailureReason::InvalidQuality};
}
bool snapshot_valid(std::string_view snapshot) {
  return !snapshot.empty() && snapshot.size() <= 256 &&
         std::all_of(snapshot.begin(), snapshot.end(),
                     [](unsigned char c) { return c >= 0x21 && c <= 0x7e; });
}
void set_snapshot(Header* destination, std::string_view snapshot) {
  destination->snapshot_size = static_cast<std::uint32_t>(snapshot.size());
  std::memcpy(destination->snapshot.data(), snapshot.data(), snapshot.size());
}
std::int64_t magnitude(std::int64_t value) {
  return value < 0 ? -value : value;
}
}  // namespace
bool QualityReport::owned_by(const BufferAllocator& allocator) const noexcept {
  return valid() &&
         (allocator.owns(*storage_) || allocator.owns_allocation(*storage_));
}
QualityEvidence QualityReport::evidence() const noexcept {
  return header(storage_).evidence;
}
std::string_view QualityReport::snapshot() const noexcept {
  if (!storage_)
    return {};
  const auto* data = reinterpret_cast<const char*>(storage_->bytes().data());
  return {data + offsetof(Header, snapshot), header(storage_).snapshot_size};
}
std::uint64_t QualityReport::dimension() const noexcept {
  return header(storage_).dimension;
}
double QualityReport::residual() const noexcept {
  return header(storage_).residual;
}
std::optional<double> QualityReport::error_bound() const noexcept {
  const auto facts = header(storage_);
  return facts.evidence == QualityEvidence::CertifiedBound
             ? std::optional<double>(facts.bound)
             : std::nullopt;
}
std::uint64_t QualityReport::proof_count() const noexcept {
  return evidence() == QualityEvidence::CertifiedBound ? dimension() : 0;
}
Result<std::array<std::int64_t, 3>> QualityReport::proof_row(
    std::uint64_t row) const {
  if (row >= proof_count())
    return Result<std::array<std::int64_t, 3>>(invalid());
  std::array<std::int64_t, 3> result{};
  std::memcpy(result.data(),
              storage_->bytes().data() + sizeof(Header) + row * 24, 24);
  return Result<std::array<std::int64_t, 3>>(result);
}
Result<QualityReport> QualityReport::measured_residual(
    std::string_view snapshot, std::uint64_t dimension, double residual,
    const BufferAllocator& allocator) {
  input_internal::Float32Environment environment;
  if (!environment.active())
    return Result<QualityReport>(Status{ErrorCode::OperationFailed, {}});
  if (!snapshot_valid(snapshot) || !dimension || !std::isfinite(residual) ||
      residual < 0)
    return Result<QualityReport>(invalid());
  auto allocated = allocator.allocate(sizeof(Header));
  if (!allocated.ok())
    return Result<QualityReport>(allocated.status());
  auto buffer = allocated.take_value();
  Header facts;
  facts.evidence = QualityEvidence::Measured;
  facts.dimension = dimension;
  facts.residual = residual;
  set_snapshot(&facts, snapshot);
  std::memcpy(buffer.data(), &facts, sizeof(facts));
  QualityReport report;
  report.storage_ = std::move(buffer).freeze();
  return Result<QualityReport>(report);
}
Result<QualityReport> QualityReport::certify_integer_diagonal(
    std::string_view snapshot, const std::int64_t* diagonal,
    const std::int64_t* estimate, const std::int64_t* rhs, std::uint64_t count,
    const BufferAllocator& allocator,
    const std::function<Status(std::uint64_t)>& consume_work,
    const CancellationToken& cancellation) {
  input_internal::Float32Environment environment;
  if (!environment.active())
    return Result<QualityReport>(Status{ErrorCode::OperationFailed, {}});
  if (!snapshot_valid(snapshot) || !count || count > 4096 || !diagonal ||
      !estimate || !rhs || !consume_work)
    return Result<QualityReport>(invalid());
  if (cancellation.cancelled())
    return Result<QualityReport>(Status{ErrorCode::Cancelled, {}});
  auto allocated = allocator.allocate(sizeof(Header) + count * 24);
  if (!allocated.ok())
    return Result<QualityReport>(allocated.status());
  auto buffer = allocated.take_value();
  std::int64_t residual = 0, minimum = 1LL << 26;
  for (std::uint64_t i = 0; i < count; ++i) {
    if (cancellation.cancelled())
      return Result<QualityReport>(Status{ErrorCode::Cancelled, {}});
    auto charged = consume_work(1);
    if (!charged.ok())
      return Result<QualityReport>(charged);
    const std::array<std::int64_t, 3> row{diagonal[i], estimate[i], rhs[i]};
    for (auto value : row)
      if (value < -(1LL << 26) || value > (1LL << 26))
        return Result<QualityReport>(invalid());
    if (!row[0])
      return Result<QualityReport>(invalid());
    residual = std::max(residual, magnitude(row[0] * row[1] - row[2]));
    minimum = std::min(minimum, magnitude(row[0]));
    std::memcpy(buffer.data() + sizeof(Header) + i * 24, row.data(), 24);
  }
  Header facts;
  facts.evidence = QualityEvidence::CertifiedBound;
  facts.dimension = count;
  facts.residual = static_cast<double>(residual);
  volatile double quotient = facts.residual / static_cast<double>(minimum);
  facts.bound =
      residual
          ? std::nextafter(quotient, std::numeric_limits<double>::infinity())
          : 0;
  set_snapshot(&facts, snapshot);
  std::memcpy(buffer.data(), &facts, sizeof(facts));
  QualityReport report;
  report.storage_ = std::move(buffer).freeze();
  return Result<QualityReport>(report);
}
}  // namespace ps
