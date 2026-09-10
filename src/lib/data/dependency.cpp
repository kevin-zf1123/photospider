#include "photospider/data/dependency.hpp"

#include <algorithm>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace ps {
namespace {
Status invalid(const char* text) {
  return Status::failure(ErrorCode::InvalidArgument, text);
}
Status stopped(const FootprintLimits& limits) {
  return limits.cancellation.cancelled()
             ? Status::failure(ErrorCode::Cancelled,
                               "dependency operation cancelled")
             : Status::success();
}
Status bounded(std::uint64_t count, const FootprintLimits& limits) {
  auto status = stopped(limits);
  if (!status.ok())
    return status;
  if (count > limits.maximum_boxes || count > limits.maximum_work)
    return Status::failure(ErrorCode::ResourceExhausted,
                           "dependency metadata limit");
  return Status::success();
}
bool same_need(const DependencyNeed& a, const DependencyNeed& b) {
  return a.port == b.port && a.roles == b.roles && a.samples == b.samples &&
         a.tags == b.tags;
}
bool same_row(const AtomCertificate& a, const AtomCertificate& b) {
  if (a.output != b.output || a.inputs.size() != b.inputs.size())
    return false;
  for (std::size_t i = 0; i < a.inputs.size(); ++i)
    if (!same_need(a.inputs[i], b.inputs[i]))
      return false;
  return true;
}
Result<std::vector<DependencyNeed>> normalize(
    const std::vector<DependencyNeed>& inputs,
    const std::vector<std::vector<std::uint64_t>>& shapes,
    const FootprintLimits& limits) {
  std::map<std::pair<std::uint32_t, std::uint32_t>, DependencyNeed> by_role;
  for (const auto& need : inputs) {
    auto status = stopped(limits);
    if (!status.ok())
      return Result<std::vector<DependencyNeed>>(status);
    if (need.port >= shapes.size() || !need.roles || (need.roles & ~15U) ||
        !need.samples.valid() || need.samples.shape() != shapes[need.port])
      return Result<std::vector<DependencyNeed>>(
          invalid("invalid dependency need"));
    for (const auto& tag : need.tags)
      if (!tag.kind)
        return Result<std::vector<DependencyNeed>>(
            invalid("zero dependency tag kind"));
    for (std::uint32_t role = 1; role <= 8; role <<= 1) {
      if (!(need.roles & role))
        continue;
      const auto key = std::make_pair(need.port, role);
      auto found = by_role.find(key);
      if (found == by_role.end()) {
        auto copy = need;
        copy.roles = role;
        by_role.emplace(key, std::move(copy));
      } else {
        auto joined = found->second.samples.unite(need.samples, limits);
        if (!joined.ok())
          return Result<std::vector<DependencyNeed>>(joined.status());
        found->second.samples = joined.take_value();
        found->second.tags.insert(found->second.tags.end(), need.tags.begin(),
                                  need.tags.end());
      }
    }
  }
  std::vector<DependencyNeed> result;
  for (auto& entry : by_role) {
    auto& need = entry.second;
    std::sort(need.tags.begin(), need.tags.end());
    need.tags.erase(std::unique(need.tags.begin(), need.tags.end()),
                    need.tags.end());
    if (!need.samples.empty() || !need.tags.empty())
      result.push_back(std::move(need));
  }
  return Result<std::vector<DependencyNeed>>(std::move(result));
}
}  // namespace
Result<DependencyCertificate> DependencyCertificate::create(
    std::string identity, Footprint coverage,
    std::vector<std::vector<std::uint64_t>> input_shapes,
    std::vector<AtomCertificate> rows, const FootprintLimits& limits) {
  auto status = bounded(rows.size(), limits);
  if (!status.ok())
    return Result<DependencyCertificate>(status);
  if (identity.empty() || identity.size() > 4096 || !coverage.valid() ||
      input_shapes.size() > 1024)
    return Result<DependencyCertificate>(
        invalid("invalid dependency certificate identity/domain"));
  for (const auto& shape : input_shapes) {
    auto empty = Footprint::none(shape, limits);
    if (!empty.ok())
      return Result<DependencyCertificate>(empty.status());
  }
  const auto count = coverage.element_count();
  if (!count.ok())
    return Result<DependencyCertificate>(count.status());
  if (count.value() != rows.size())
    return Result<DependencyCertificate>(
        invalid("certificate has unknown or extra rows"));
  std::uint64_t entries = rows.size();
  for (const auto& row : rows) {
    if (!coverage.contains(row.output))
      return Result<DependencyCertificate>(
          invalid("certificate row outside coverage"));
    for (const auto& need : row.inputs) {
      const auto extra = need.tags.size() + need.samples.boxes().size() + 1;
      for (std::uint32_t role = 1; role <= 8; role <<= 1) {
        if (!(need.roles & role))
          continue;
        if (extra > limits.maximum_boxes ||
            entries > limits.maximum_boxes - extra)
          return Result<DependencyCertificate>(Status::failure(
              ErrorCode::ResourceExhausted, "certificate edge limit"));
        entries += extra;
        status = bounded(entries, limits);
        if (!status.ok())
          return Result<DependencyCertificate>(status);
      }
    }
  }
  std::sort(rows.begin(), rows.end(),
            [](const auto& a, const auto& b) { return a.output < b.output; });
  std::uint64_t published_entries = rows.size();
  for (std::size_t i = 0; i < rows.size(); ++i) {
    if (i && rows[i - 1].output == rows[i].output)
      return Result<DependencyCertificate>(
          invalid("duplicate certificate row"));
    auto canonical = normalize(rows[i].inputs, input_shapes, limits);
    if (!canonical.ok())
      return Result<DependencyCertificate>(canonical.status());
    rows[i].inputs = canonical.take_value();
    for (const auto& need : rows[i].inputs) {
      const auto extra = need.tags.size() + need.samples.boxes().size() + 1;
      if (extra > limits.maximum_boxes ||
          published_entries > limits.maximum_boxes - extra)
        return Result<DependencyCertificate>(Status::failure(
            ErrorCode::ResourceExhausted, "canonical certificate edge limit"));
      published_entries += extra;
      status = bounded(published_entries, limits);
      if (!status.ok())
        return Result<DependencyCertificate>(status);
    }
  }
  status = stopped(limits);
  if (!status.ok())
    return Result<DependencyCertificate>(status);
  DependencyCertificate result;
  result.identity_ = std::move(identity);
  result.coverage_ = std::move(coverage);
  result.input_shapes_ = std::move(input_shapes);
  result.rows_ = std::move(rows);
  return Result<DependencyCertificate>(std::move(result));
}
Result<DependencyCertificate> DependencyCertificate::restrict(
    const Footprint& subset, const FootprintLimits& limits) const {
  auto outside = subset.subtract(coverage_, limits);
  if (!outside.ok())
    return Result<DependencyCertificate>(outside.status());
  if (!outside.value().empty())
    return Result<DependencyCertificate>(invalid("unknown certificate row"));
  std::vector<AtomCertificate> selected;
  for (const auto& row : rows_)
    if (subset.contains(row.output))
      selected.push_back(row);
  return create(identity_, subset, input_shapes_, std::move(selected), limits);
}
Result<std::vector<DependencyNeed>> DependencyCertificate::backward(
    const Footprint& subset, const FootprintLimits& limits) const {
  auto restricted = restrict(subset, limits);
  if (!restricted.ok())
    return Result<std::vector<DependencyNeed>>(restricted.status());
  std::vector<DependencyNeed> needs;
  for (const auto& row : restricted.value().rows())
    needs.insert(needs.end(), row.inputs.begin(), row.inputs.end());
  return normalize(needs, input_shapes_, limits);
}
Result<Footprint> DependencyCertificate::transpose(
    const DependencyNeed& dirty, const FootprintLimits& limits) const {
  auto status = stopped(limits);
  if (!status.ok())
    return Result<Footprint>(status);
  if (!valid() || dirty.port >= input_shapes_.size() || !dirty.roles ||
      (dirty.roles & ~15U) || !dirty.samples.valid() ||
      dirty.samples.shape() != input_shapes_[dirty.port])
    return Result<Footprint>(invalid("invalid certificate transpose input"));
  for (const auto& tag : dirty.tags)
    if (!tag.kind)
      return Result<Footprint>(invalid("zero dirty tag kind"));
  std::vector<Region> affected;
  for (const auto& row : rows_) {
    bool hit = false;
    for (const auto& need : row.inputs) {
      if (need.port != dirty.port || !(need.roles & dirty.roles))
        continue;
      auto overlap = need.samples.intersect(dirty.samples, limits);
      if (!overlap.ok())
        return Result<Footprint>(overlap.status());
      hit |= !overlap.value().empty();
      for (const auto& tag : dirty.tags)
        hit |= std::binary_search(need.tags.begin(), need.tags.end(), tag);
    }
    if (hit) {
      std::vector<RegionDimension> dims;
      for (const auto coordinate : row.output)
        dims.push_back({coordinate, 1});
      affected.emplace_back(std::move(dims));
    }
  }
  return Footprint::from_regions(coverage_.shape(), affected, limits);
}
Result<DependencyCertificate> DependencyCertificate::merge(
    const DependencyCertificate& other, const FootprintLimits& limits) const {
  if (!valid() || identity_ != other.identity_ ||
      input_shapes_ != other.input_shapes_)
    return Result<DependencyCertificate>(
        invalid("incompatible certificate identities"));
  auto coverage = coverage_.unite(other.coverage_, limits);
  if (!coverage.ok())
    return Result<DependencyCertificate>(coverage.status());
  auto rows = rows_;
  for (const auto& row : other.rows_) {
    auto found = std::lower_bound(rows_.begin(), rows_.end(), row.output,
                                  [](const auto& entry, const auto& output) {
                                    return entry.output < output;
                                  });
    if (found != rows_.end() && found->output == row.output) {
      if (!same_row(*found, row))
        return Result<DependencyCertificate>(
            invalid("inconsistent overlapping certificate rows"));
    } else {
      rows.push_back(row);
    }
  }
  return create(identity_, coverage.take_value(), input_shapes_,
                std::move(rows), limits);
}
}  // namespace ps
