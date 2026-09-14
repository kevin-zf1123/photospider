#include <algorithm>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "data/dependency_metadata.hpp"
#include "photospider/data/dependency.hpp"

namespace ps {
namespace {
struct MappingStop {
  Status status;
};
Status invalid(const char* message) {
  return Status{ErrorCode::InvalidArgument, message};
}
void check(Status status) {
  if (!status.ok())
    throw MappingStop{std::move(status)};
}
template <class T>
T take(Result<T> result) {
  check(result.status());
  return result.take_value();
}
template <class F>
auto guarded(F&& function) -> decltype(function()) {
  try {
    return function();
  } catch (const MappingStop& stop) {
    return decltype(function())(stop.status);
  }
}
struct MappingBudget {
  const FootprintLimits& limits;
  std::uint64_t remaining, entries = 0;
  explicit MappingBudget(const FootprintLimits& bounds)
      : limits(bounds), remaining(bounds.maximum_work) {}
  Status charge(std::uint64_t count) {
    if (limits.cancellation.cancelled())
      return Status{ErrorCode::Cancelled, {}};
    if (count > remaining)
      return Status{ErrorCode::ResourceExhausted, "mapping work limit",
                    FailureReason::WorkLimit};
    remaining -= count;
    return limits.consume_work ? limits.consume_work(count) : Status::success();
  }
  void work(std::uint64_t count = 1) { check(charge(count)); }
  void retain(std::uint64_t count) {
    if (count > limits.maximum_boxes - entries)
      throw MappingStop{Status{ErrorCode::ResourceExhausted,
                               "mapping metadata limit",
                               FailureReason::CapacityLimit}};
    entries += count;
    work(count);
  }
  FootprintLimits geometry() {
    auto result = limits;
    result.maximum_work = remaining;
    result.maximum_boxes = limits.maximum_boxes - entries;
    result.consume_work = [this](std::uint64_t n) { return charge(n); };
    return result;
  }
};
bool less_need(const DependencyMappedNeed& a, const DependencyMappedNeed& b) {
  if (a.port != b.port)
    return a.port < b.port;
  if (a.roles != b.roles)
    return a.roles < b.roles;
  if (a.axes != b.axes)
    return a.axes < b.axes;
  return a.tags < b.tags;
}
std::vector<DependencyMappedNeed> canonical_maps(
    const std::vector<DependencyMappedNeed>& inputs,
    const std::vector<std::uint64_t>& output,
    const std::vector<std::vector<std::uint64_t>>& shapes,
    MappingBudget* budget) {
  std::vector<DependencyMappedNeed> result;
  for (const auto& source : inputs) {
    budget->work();
    if (source.port >= shapes.size() || !source.roles ||
        (source.roles & ~15U) ||
        (!source.axes.empty() &&
         source.axes.size() != shapes[source.port].size()))
      throw MappingStop{invalid("invalid mapped dependency port/axes/roles")};
    budget->retain(1 + source.axes.size() + source.tags.size());
    auto need = source;
    std::uint32_t used = 0;
    for (std::size_t i = 0; i < need.axes.size(); ++i) {
      budget->work();
      auto& axis = need.axes[i];
      if (axis.observation_axis < -1 ||
          axis.observation_axis >= static_cast<std::int32_t>(output.size()))
        throw MappingStop{invalid("mapped observation axis outside rank")};
      if (axis.observation_axis >= 0) {
        const auto bit = 1U << axis.observation_axis;
        if ((used & bit) ||
            output[axis.observation_axis] > shapes[need.port][i])
          throw MappingStop{
              invalid("mapped axes must be injective and in bounds")};
        used |= bit;
        axis.fixed = {0, 0};
      } else if (!axis.fixed.extent ||
                 axis.fixed.offset > shapes[need.port][i] ||
                 axis.fixed.extent > shapes[need.port][i] - axis.fixed.offset) {
        throw MappingStop{invalid("mapped fixed interval outside input")};
      }
    }
    for (const auto& tag : need.tags) {
      budget->work();
      if (!tag.kind)
        throw MappingStop{invalid("zero mapped tag kind")};
    }
    std::sort(need.tags.begin(), need.tags.end());
    need.tags.erase(std::unique(need.tags.begin(), need.tags.end()),
                    need.tags.end());
    if (need.axes.empty() && need.tags.empty())
      continue;
    for (std::uint32_t role = 1; role <= 8; role <<= 1) {
      if (!(source.roles & role))
        continue;
      budget->retain(1 + need.axes.size() + need.tags.size());
      need.roles = role;
      result.push_back(need);
    }
  }
  std::sort(result.begin(), result.end(), less_need);
  result.erase(std::unique(result.begin(), result.end()), result.end());
  return result;
}
std::vector<DependencyNeed> project(
    const std::vector<DependencyMappedNeed>& maps, const Footprint& subset,
    const std::vector<std::vector<std::uint64_t>>& shapes,
    MappingBudget* budget) {
  std::map<std::pair<std::uint32_t, std::uint32_t>, DependencyNeed> grouped;
  if (subset.empty())
    return {};
  for (const auto& map : maps) {
    budget->retain(1 + map.tags.size());
    std::vector<Region> boxes;
    if (!map.axes.empty()) {
      for (const auto& box : subset.boxes()) {
        budget->retain(1 + map.axes.size());
        if (boxes.size() >= budget->limits.maximum_boxes)
          throw MappingStop{Status{ErrorCode::ResourceExhausted, {}}};
        std::vector<RegionDimension> dimensions;
        for (const auto& axis : map.axes)
          dimensions.push_back(axis.observation_axis < 0
                                   ? axis.fixed
                                   : box.dimensions()[axis.observation_axis]);
        boxes.emplace_back(std::move(dimensions));
      }
    }
    auto samples = take(
        Footprint::from_regions(shapes[map.port], boxes, budget->geometry()));
    // Normalization can split permuted boxes; charge the actual retained set.
    budget->retain(samples.boxes().size() * (1 + shapes[map.port].size()));
    const auto key = std::make_pair(map.port, map.roles);
    auto found = grouped.find(key);
    if (found == grouped.end()) {
      grouped.emplace(key, DependencyNeed{map.port, map.roles,
                                          std::move(samples), map.tags});
    } else {
      found->second.samples =
          take(found->second.samples.unite(samples, budget->geometry()));
      budget->retain(found->second.samples.boxes().size() *
                     (1 + shapes[map.port].size()));
      found->second.tags.insert(found->second.tags.end(), map.tags.begin(),
                                map.tags.end());
    }
  }
  std::vector<DependencyNeed> result;
  for (auto& entry : grouped) {
    auto& need = entry.second;
    std::sort(need.tags.begin(), need.tags.end());
    need.tags.erase(std::unique(need.tags.begin(), need.tags.end()),
                    need.tags.end());
    result.push_back(std::move(need));
  }
  return result;
}
bool same_needs(const std::vector<DependencyNeed>& a,
                const std::vector<DependencyNeed>& b) {
  if (a.size() != b.size())
    return false;
  for (std::size_t i = 0; i < a.size(); ++i)
    if (a[i].port != b[i].port || a[i].roles != b[i].roles ||
        a[i].samples != b[i].samples || a[i].tags != b[i].tags)
      return false;
  return true;
}
Footprint point(const std::vector<std::uint64_t>& shape,
                const std::vector<std::uint64_t>& coordinate,
                MappingBudget* budget) {
  std::vector<RegionDimension> dims;
  for (auto c : coordinate)
    dims.push_back({c, 1});
  return take(Footprint::from_regions(shape, {Region(std::move(dims))},
                                      budget->geometry()));
}
std::vector<DependencyMapPiece> as_pieces(
    const DependencyCertificate& certificate, MappingBudget* budget) {
  budget->retain(certificate.metadata_entries());
  if (certificate.mapped())
    return certificate.mapping_pieces();
  std::vector<DependencyMapPiece> result;
  for (const auto& row : certificate.rows()) {
    DependencyMapPiece piece{
        point(certificate.coverage().shape(), row.output, budget),
        {}};
    for (const auto& need : row.inputs) {
      if (need.samples.empty()) {
        piece.inputs.push_back({need.port, need.roles, {}, need.tags});
      } else {
        bool first = true;
        for (const auto& box : need.samples.boxes()) {
          budget->work();
          DependencyMappedNeed mapped{
              need.port,
              need.roles,
              {},
              first ? need.tags : std::vector<DependencyTag>{}};
          for (const auto& axis : box.dimensions())
            mapped.axes.push_back({-1, axis});
          piece.inputs.push_back(std::move(mapped));
          first = false;
        }
      }
    }
    result.push_back(std::move(piece));
  }
  return result;
}
}  // namespace
std::uint64_t DependencyCertificate::measure_storage() const noexcept {
  std::uint64_t total = 0;
  const auto add = [&](std::uint64_t count, std::uint64_t scale = 1) {
    if (count > (UINT64_MAX - total) / scale)
      total = UINT64_MAX;
    else
      total += count * scale;
  };
  const auto footprint = [&](const Footprint& samples) {
    add(samples.shape().size());
    add(samples.boxes().size(), 1 + 2 * samples.shape().size());
  };
  add(identity_.size());
  add(input_shapes_.size());
  for (const auto& shape : input_shapes_)
    add(shape.size());
  footprint(coverage_);
  if (mapped_) {
    add(pieces_.size());
    for (const auto& piece : pieces_) {
      footprint(piece.coverage);
      add(piece.inputs.size());
      for (const auto& need : piece.inputs) {
        add(need.axes.size(), 4);
        add(need.tags.size(), 3);
      }
    }
  } else {
    add(rows_.size());
    for (const auto& row : rows_) {
      add(row.output.size());
      add(row.inputs.size());
      for (const auto& need : row.inputs) {
        add(need.tags.size(), 3);
        footprint(need.samples);
      }
    }
  }
  return total;
}
const std::vector<AtomCertificate>& DependencyCertificate::rows() const {
  if (mapped_)
    throw std::logic_error("mapped certificate has no materialized rows");
  return rows_;
}
Result<DependencyCertificate> DependencyCertificate::create_mapped(
    std::string identity, Footprint coverage,
    std::vector<std::vector<std::uint64_t>> input_shapes,
    std::vector<DependencyMapPiece> pieces, const FootprintLimits& limits) {
  return create_mapped_owned(std::move(identity), std::move(coverage),
                             std::move(input_shapes), std::move(pieces), limits,
                             {});
}
Result<DependencyCertificate> DependencyCertificate::create_mapped_owned(
    std::string identity, Footprint coverage,
    std::vector<std::vector<std::uint64_t>> input_shapes,
    std::vector<DependencyMapPiece> pieces, const FootprintLimits& limits,
    const std::shared_ptr<const dependency_internal::MetadataOwner>& source) {
  return guarded([&]() -> Result<DependencyCertificate> {
    MappingBudget budget(limits);
    budget.work();
    if (identity.empty() || identity.size() > 4096 || !coverage.valid() ||
        input_shapes.size() > 1024)
      return Result<DependencyCertificate>(
          invalid("invalid mapped certificate identity/domain"));
    for (const auto& shape : input_shapes)
      take(Footprint::none(shape, budget.geometry()));
    auto visited = take(Footprint::none(coverage.shape(), budget.geometry()));
    std::vector<DependencyMapPiece> normalized;
    for (auto& piece : pieces) {
      budget.retain(1 + piece.coverage.boxes().size());
      if (!piece.coverage.valid() || piece.coverage.shape() != coverage.shape())
        return Result<DependencyCertificate>(
            invalid("mapped piece domain mismatch"));
      if (!take(piece.coverage.subtract(coverage, budget.geometry())).empty() ||
          !take(piece.coverage.intersect(visited, budget.geometry())).empty())
        return Result<DependencyCertificate>(
            invalid("mapped pieces overlap or exceed coverage"));
      auto maps =
          canonical_maps(piece.inputs, coverage.shape(), input_shapes, &budget);
      visited = take(visited.unite(piece.coverage, budget.geometry()));
      if (!piece.coverage.empty())
        normalized.push_back({std::move(piece.coverage), std::move(maps)});
    }
    if (visited != coverage)
      return Result<DependencyCertificate>(
          invalid("mapped certificate has unknown observations"));
    DependencyCertificate result;
    result.metadata_owner_ = dependency_internal::metadata_owner(
        dependency_internal::certificate_bytes(identity, coverage, input_shapes,
                                               {}, normalized, false),
        source);
    result.identity_ = std::move(identity);
    result.coverage_ = std::move(coverage);
    result.input_shapes_ = std::move(input_shapes);
    result.mapped_ = true;
    result.pieces_ = std::move(normalized);
    result.metadata_entries_ = budget.entries;
    result.storage_entries_ = result.measure_storage();
    return Result<DependencyCertificate>(std::move(result));
  });
}
Result<DependencyCertificate> DependencyCertificate::with_identity(
    std::string identity, const FootprintLimits& limits) const {
  return guarded([&]() -> Result<DependencyCertificate> {
    MappingBudget budget(limits);
    budget.retain(metadata_entries_);
    if (!valid() || identity.empty() || identity.size() > 4096)
      return Result<DependencyCertificate>(
          invalid("invalid rebound certificate identity"));
    DependencyCertificate result(*this, std::move(identity));
    result.storage_entries_ = result.measure_storage();
    return Result<DependencyCertificate>(std::move(result));
  });
}
Result<AtomCertificate> DependencyCertificate::row(
    const std::vector<std::uint64_t>& coordinate,
    const FootprintLimits& limits) const {
  return guarded([&]() -> Result<AtomCertificate> {
    MappingBudget budget(limits);
    if (!valid() || !coverage_.contains(coordinate))
      return Result<AtomCertificate>(
          invalid("unknown certificate observation"));
    auto subset = point(coverage_.shape(), coordinate, &budget);
    return Result<AtomCertificate>(
        AtomCertificate{coordinate, take(backward(subset, budget.geometry()))});
  });
}
Result<std::vector<AtomCertificate>> DependencyCertificate::materialize(
    const FootprintLimits& limits) const {
  return guarded([&]() -> Result<std::vector<AtomCertificate>> {
    MappingBudget budget(limits);
    auto count = take(coverage_.element_count());
    if (count > limits.maximum_boxes || count > budget.remaining)
      return Result<std::vector<AtomCertificate>>(
          Status{ErrorCode::ResourceExhausted, {}});
    std::vector<AtomCertificate> result;
    check(coverage_.visit(
        [&](const auto& coordinate) {
          auto materialized = take(row(coordinate, budget.geometry()));
          std::uint64_t weight = 1;
          for (const auto& need : materialized.inputs)
            weight += 1 + need.tags.size() + need.samples.boxes().size();
          budget.retain(weight);
          result.push_back(std::move(materialized));
          return Status::success();
        },
        limits.maximum_boxes, limits.cancellation));
    return Result<std::vector<AtomCertificate>>(std::move(result));
  });
}
Result<DependencyCertificate> DependencyCertificate::restrict_mapped(
    const Footprint& subset, const FootprintLimits& limits) const {
  return guarded([&]() -> Result<DependencyCertificate> {
    MappingBudget budget(limits);
    if (!take(subset.subtract(coverage_, budget.geometry())).empty())
      return Result<DependencyCertificate>(
          invalid("unknown mapped observation"));
    dependency_internal::MetadataBytes construction_bytes;
    construction_bytes.add(dependency_internal::certificate_bytes(
        identity_, subset, input_shapes_, {}, {}, true));
    std::size_t selected_count = 0;
    for (const auto& piece : pieces_) {
      budget.work();
      auto intersection =
          take(piece.coverage.intersect(subset, budget.geometry()));
      if (!intersection.empty()) {
        ++selected_count;
        construction_bytes.add(1, sizeof(DependencyMapPiece));
        construction_bytes.footprint(intersection, false);
        construction_bytes.block(piece.inputs, true);
        for (const auto& need : piece.inputs) {
          construction_bytes.block(need.axes, true);
          construction_bytes.block(need.tags, true);
        }
      }
    }
    if (selected_count > limits.maximum_boxes)
      return Result<DependencyCertificate>(
          Status{ErrorCode::ResourceExhausted,
                 {},
                 FailureReason::CapacityLimit});
    auto construction = dependency_internal::metadata_owner(
        construction_bytes.bytes, metadata_owner_);
    std::vector<DependencyMapPiece> selected;
    selected.reserve(selected_count);
    for (const auto& piece : pieces_) {
      budget.work();
      auto intersection =
          take(piece.coverage.intersect(subset, budget.geometry()));
      if (!intersection.empty()) {
        budget.retain(1 + intersection.boxes().size());
        for (const auto& need : piece.inputs)
          budget.retain(1 + need.axes.size() + need.tags.size());
        selected.push_back({std::move(intersection), piece.inputs});
      }
    }
    return create_mapped_owned(identity_, subset, input_shapes_,
                               std::move(selected), budget.geometry(),
                               metadata_owner_);
  });
}
Result<std::vector<DependencyNeed>> DependencyCertificate::backward_mapped(
    const Footprint& subset, const FootprintLimits& limits) const {
  return guarded([&]() -> Result<std::vector<DependencyNeed>> {
    MappingBudget budget(limits);
    if (!take(subset.subtract(coverage_, budget.geometry())).empty())
      return Result<std::vector<DependencyNeed>>(
          invalid("unknown mapped observation"));
    std::map<std::pair<std::uint32_t, std::uint32_t>, DependencyNeed> needs;
    for (const auto& piece : pieces_) {
      budget.work();
      auto intersection =
          take(piece.coverage.intersect(subset, budget.geometry()));
      auto projected =
          project(piece.inputs, intersection, input_shapes_, &budget);
      for (auto& need : projected) {
        const auto key = std::make_pair(need.port, need.roles);
        auto found = needs.find(key);
        if (found == needs.end()) {
          needs.emplace(key, std::move(need));
        } else {
          found->second.samples = take(
              found->second.samples.unite(need.samples, budget.geometry()));
          budget.retain(found->second.samples.boxes().size() *
                        (1 + input_shapes_[need.port].size()));
          found->second.tags.insert(found->second.tags.end(), need.tags.begin(),
                                    need.tags.end());
        }
      }
    }
    std::vector<DependencyNeed> result;
    for (auto& entry : needs) {
      auto& tags = entry.second.tags;
      std::sort(tags.begin(), tags.end());
      tags.erase(std::unique(tags.begin(), tags.end()), tags.end());
      result.push_back(std::move(entry.second));
    }
    return Result<std::vector<DependencyNeed>>(std::move(result));
  });
}
Result<Footprint> DependencyCertificate::transpose_mapped(
    const DependencyNeed& dirty, const FootprintLimits& limits) const {
  return guarded([&]() -> Result<Footprint> {
    MappingBudget budget(limits);
    if (!valid() || dirty.port >= input_shapes_.size() || !dirty.roles ||
        (dirty.roles & ~15U) || !dirty.samples.valid() ||
        dirty.samples.shape() != input_shapes_[dirty.port])
      return Result<Footprint>(invalid("invalid mapped transpose input"));
    for (const auto& tag : dirty.tags)
      if (!tag.kind)
        return Result<Footprint>(invalid("zero dirty tag kind"));
    auto result = take(Footprint::none(coverage_.shape(), budget.geometry()));
    for (const auto& piece : pieces_) {
      budget.work();
      for (const auto& map : piece.inputs) {
        budget.work(1 + dirty.tags.size());
        if (map.port != dirty.port || !(map.roles & dirty.roles))
          continue;
        bool tagged = false;
        for (const auto& tag : dirty.tags)
          tagged |= std::binary_search(map.tags.begin(), map.tags.end(), tag);
        if (tagged) {
          result = take(result.unite(piece.coverage, budget.geometry()));
          continue;
        }
        if (map.axes.empty())
          continue;
        std::vector<Region> inverse;
        for (const auto& box : dirty.samples.boxes()) {
          budget.work(1 + map.axes.size());
          auto dimensions = Region::whole(coverage_.shape()).dimensions();
          bool hit = true;
          for (std::size_t i = 0; i < map.axes.size(); ++i) {
            const auto& axis = map.axes[i];
            const auto& change = box.dimensions()[i];
            if (axis.observation_axis < 0) {
              hit &= axis.fixed.offset < change.offset + change.extent &&
                     change.offset < axis.fixed.offset + axis.fixed.extent;
            } else {
              const auto size = coverage_.shape()[axis.observation_axis];
              if (change.offset >= size)
                hit = false;
              else
                dimensions[axis.observation_axis] = {
                    change.offset,
                    std::min(change.extent, size - change.offset)};
            }
          }
          if (hit) {
            if (inverse.size() >= limits.maximum_boxes)
              return Result<Footprint>(
                  Status{ErrorCode::ResourceExhausted, {}});
            inverse.emplace_back(std::move(dimensions));
          }
        }
        auto affected = take(Footprint::from_regions(coverage_.shape(), inverse,
                                                     budget.geometry()));
        affected = take(affected.intersect(piece.coverage, budget.geometry()));
        result = take(result.unite(affected, budget.geometry()));
      }
    }
    return Result<Footprint>(std::move(result));
  });
}
Result<DependencyCertificate> DependencyCertificate::merge_mapped(
    const DependencyCertificate& other, const FootprintLimits& limits) const {
  return guarded([&]() -> Result<DependencyCertificate> {
    MappingBudget budget(limits);
    dependency_internal::MetadataBytes construction_bytes;
    construction_bytes.add(dependency_internal::certificate_bytes(
        identity_, coverage_, input_shapes_, rows_, pieces_, true));
    construction_bytes.add(dependency_internal::certificate_bytes(
        other.identity_, other.coverage_, other.input_shapes_, other.rows_,
        other.pieces_, true));
    // Fixed-axis conversion replaces 16-byte region dimensions with 24-byte
    // axes; this bounded workspace also covers vector growth and both copies.
    if (construction_bytes.bytes > UINT64_MAX / 4)
      return Result<DependencyCertificate>(
          Status{ErrorCode::ResourceExhausted,
                 {},
                 FailureReason::CapacityLimit});
    auto construction = dependency_internal::metadata_owner(
        construction_bytes.bytes * 4,
        metadata_owner_ ? metadata_owner_ : other.metadata_owner_);
    auto pieces = as_pieces(*this, &budget);
    auto incoming = as_pieces(other, &budget);
    for (auto& next : incoming) {
      auto remainder = next.coverage;
      for (const auto& prior : pieces) {
        budget.work();
        auto overlap =
            take(prior.coverage.intersect(next.coverage, budget.geometry()));
        if (!overlap.empty() && prior.inputs != next.inputs) {
          const auto count = take(overlap.element_count());
          if (count > budget.remaining)
            return Result<DependencyCertificate>(Status{
                ErrorCode::ResourceExhausted, "mapping equivalence work limit",
                FailureReason::WorkLimit});
          check(overlap.visit(
              [&](const auto& coordinate) {
                budget.work();
                auto one = point(coverage_.shape(), coordinate, &budget);
                if (!same_needs(
                        project(prior.inputs, one, input_shapes_, &budget),
                        project(next.inputs, one, input_shapes_, &budget)))
                  return invalid("inconsistent overlapping mapped certificate");
                return Status::success();
              },
              limits.maximum_work, limits.cancellation));
        }
        remainder = take(remainder.subtract(prior.coverage, budget.geometry()));
      }
      if (remainder.empty())
        continue;
      auto same = std::find_if(
          pieces.begin(), pieces.end(),
          [&](const auto& old) { return old.inputs == next.inputs; });
      if (same != pieces.end()) {
        same->coverage =
            take(same->coverage.unite(remainder, budget.geometry()));
      } else {
        budget.retain(1);
        pieces.push_back({std::move(remainder), std::move(next.inputs)});
      }
    }
    auto coverage = take(coverage_.unite(other.coverage_, budget.geometry()));
    return create_mapped_owned(
        identity_, std::move(coverage), input_shapes_, std::move(pieces),
        budget.geometry(),
        metadata_owner_ ? metadata_owner_ : other.metadata_owner_);
  });
}
}  // namespace ps
