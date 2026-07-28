#include "photospider/data/region.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace ps {
namespace {

// NOLINTBEGIN(whitespace/indent_namespace)
/** @brief Fixed built-in image-domain identity. */
constexpr RegionDomainKey kImageDomain{0x50484f544f535049ULL,
                                       0x4445525f494d4731ULL};
/** @brief Fixed built-in dense-tensor-domain identity. */
constexpr RegionDomainKey kDenseTensorDomain{0x50484f544f535049ULL,
                                             0x4445525f54454e31ULL};
// NOLINTEND

/**
 * @brief Returns one atom's explicit domain key.
 * @param atom Atom to inspect.
 * @return Borrowed-by-value domain key.
 * @throws Nothing.
 */
RegionDomainKey atom_domain(const RegionAtom& atom) noexcept {
  return std::visit([](const auto& value) { return value.domain; }, atom);
}

/**
 * @brief Returns a stable atom-kind ordering value.
 * @param atom Atom to inspect.
 * @return Zero for ImageRect and one for TensorSlice.
 * @throws Nothing.
 */
std::size_t atom_kind(const RegionAtom& atom) noexcept {
  return atom.index();
}

/**
 * @brief Reports whether one ImageRect selects no coordinate.
 * @param rect Valid non-inverted rectangle.
 * @return True when either half-open interval is empty.
 * @throws Nothing.
 */
bool empty_image_rect(const ImageRect& rect) noexcept {
  return rect.x_begin == rect.x_end || rect.y_begin == rect.y_end;
}

/**
 * @brief Reports whether one TensorSlice selects no coordinate.
 * @param slice Valid non-inverted, positive-rank slice.
 * @return True when any half-open axis interval is empty.
 * @throws Nothing.
 */
bool empty_tensor_slice(const TensorSlice& slice) noexcept {
  return std::any_of(
      slice.axes.begin(), slice.axes.end(),
      [](const RegionInterval& axis) { return axis.begin == axis.end; });
}

/**
 * @brief Validates one atom before canonical normalization.
 * @param atom Candidate atom.
 * @return True when the atom is semantically empty.
 * @throws std::invalid_argument for invalid keys, rank, or intervals.
 */
bool validate_atom(const RegionAtom& atom) {
  if (!atom_domain(atom).valid()) {
    throw std::invalid_argument("Region atom requires a nonzero domain key.");
  }
  if (const auto* image = std::get_if<ImageRect>(&atom)) {
    if (image->x_end < image->x_begin || image->y_end < image->y_begin) {
      throw std::invalid_argument(
          "ImageRect half-open endpoints must not be inverted.");
    }
    return empty_image_rect(*image);
  }
  const TensorSlice& tensor = std::get<TensorSlice>(atom);
  if (tensor.axes.empty()) {
    throw std::invalid_argument("TensorSlice requires positive rank.");
  }
  for (const RegionInterval& axis : tensor.axes) {
    if (axis.end < axis.begin) {
      throw std::invalid_argument(
          "TensorSlice half-open endpoints must not be inverted.");
    }
  }
  return empty_tensor_slice(tensor);
}

/**
 * @brief Intersects two same-domain same-kind atoms exactly.
 * @param left First validated atom.
 * @param right Second validated compatible atom.
 * @return Exact atom; it may be semantically empty.
 * @throws std::invalid_argument when tensor ranks differ.
 * @throws std::bad_alloc when tensor-axis storage cannot allocate.
 */
RegionAtom intersect_compatible_atoms(const RegionAtom& left,
                                      const RegionAtom& right) {
  if (const auto* left_image = std::get_if<ImageRect>(&left)) {
    const ImageRect& right_image = std::get<ImageRect>(right);
    const std::int64_t x_begin =
        std::max(left_image->x_begin, right_image.x_begin);
    const std::int64_t y_begin =
        std::max(left_image->y_begin, right_image.y_begin);
    return ImageRect{
        left_image->domain, x_begin,
        std::max(x_begin, std::min(left_image->x_end, right_image.x_end)),
        y_begin,
        std::max(y_begin, std::min(left_image->y_end, right_image.y_end))};
  }
  const TensorSlice& left_tensor = std::get<TensorSlice>(left);
  const TensorSlice& right_tensor = std::get<TensorSlice>(right);
  if (left_tensor.axes.size() != right_tensor.axes.size()) {
    throw std::invalid_argument(
        "TensorSlice rank mismatch cannot be normalized.");
  }
  TensorSlice result;
  result.domain = left_tensor.domain;
  result.axes.reserve(left_tensor.axes.size());
  for (std::size_t axis = 0U; axis < left_tensor.axes.size(); ++axis) {
    const std::uint64_t begin =
        std::max(left_tensor.axes[axis].begin, right_tensor.axes[axis].begin);
    result.axes.push_back(
        {begin, std::max(begin, std::min(left_tensor.axes[axis].end,
                                         right_tensor.axes[axis].end))});
  }
  return result;
}

/**
 * @brief Reports exact containment for compatible atoms.
 * @param outer Candidate containing atom.
 * @param inner Candidate contained atom.
 * @return True when every coordinate selected by inner satisfies outer.
 * @throws Nothing under same-kind, same-rank preconditions.
 */
bool compatible_atom_contains(const RegionAtom& outer,
                              const RegionAtom& inner) noexcept {
  if (const auto* outer_image = std::get_if<ImageRect>(&outer)) {
    const ImageRect& inner_image = std::get<ImageRect>(inner);
    return outer_image->x_begin <= inner_image.x_begin &&
           outer_image->x_end >= inner_image.x_end &&
           outer_image->y_begin <= inner_image.y_begin &&
           outer_image->y_end >= inner_image.y_end;
  }
  const TensorSlice& outer_tensor = std::get<TensorSlice>(outer);
  const TensorSlice& inner_tensor = std::get<TensorSlice>(inner);
  if (outer_tensor.axes.size() != inner_tensor.axes.size()) {
    return false;
  }
  for (std::size_t axis = 0U; axis < outer_tensor.axes.size(); ++axis) {
    if (outer_tensor.axes[axis].begin > inner_tensor.axes[axis].begin ||
        outer_tensor.axes[axis].end < inner_tensor.axes[axis].end) {
      return false;
    }
  }
  return true;
}

/**
 * @brief Checks whether two closed endpoint spans overlap or touch.
 * @param left_begin Left inclusive beginning.
 * @param left_end Left exclusive ending.
 * @param right_begin Right inclusive beginning.
 * @param right_end Right exclusive ending.
 * @return True when the union has no gap.
 * @throws Nothing.
 */
template <typename Integer>
bool intervals_overlap_or_touch(Integer left_begin, Integer left_end,
                                Integer right_begin,
                                Integer right_end) noexcept {
  return left_begin <= right_end && right_begin <= left_end;
}

/**
 * @brief Computes a bounding atom for same-kind, same-domain operands.
 * @param left First compatible atom.
 * @param right Second compatible atom.
 * @return Bounding ImageRect or axis-wise TensorSlice hull.
 * @throws std::invalid_argument for tensor-rank mismatch.
 * @throws std::bad_alloc when tensor-axis storage cannot allocate.
 */
RegionAtom atom_hull(const RegionAtom& left, const RegionAtom& right) {
  if (const auto* left_image = std::get_if<ImageRect>(&left)) {
    const ImageRect& right_image = std::get<ImageRect>(right);
    return ImageRect{left_image->domain,
                     std::min(left_image->x_begin, right_image.x_begin),
                     std::max(left_image->x_end, right_image.x_end),
                     std::min(left_image->y_begin, right_image.y_begin),
                     std::max(left_image->y_end, right_image.y_end)};
  }
  const TensorSlice& left_tensor = std::get<TensorSlice>(left);
  const TensorSlice& right_tensor = std::get<TensorSlice>(right);
  if (left_tensor.axes.size() != right_tensor.axes.size()) {
    throw std::invalid_argument("TensorSlice rank mismatch has no hull.");
  }
  TensorSlice result;
  result.domain = left_tensor.domain;
  result.axes.reserve(left_tensor.axes.size());
  for (std::size_t axis = 0U; axis < left_tensor.axes.size(); ++axis) {
    result.axes.push_back(
        {std::min(left_tensor.axes[axis].begin, right_tensor.axes[axis].begin),
         std::max(left_tensor.axes[axis].end, right_tensor.axes[axis].end)});
  }
  return result;
}

/**
 * @brief Tests whether a two-atom union remains one atom exactly.
 * @param left First compatible atom.
 * @param right Second compatible atom.
 * @return True when the bounding atom introduces no missing coordinates.
 * @throws Nothing.
 */
bool atom_union_is_exact(const RegionAtom& left,
                         const RegionAtom& right) noexcept {
  if (compatible_atom_contains(left, right) ||
      compatible_atom_contains(right, left)) {
    return true;
  }
  if (const auto* left_image = std::get_if<ImageRect>(&left)) {
    const ImageRect& right_image = std::get<ImageRect>(right);
    const bool same_x = left_image->x_begin == right_image.x_begin &&
                        left_image->x_end == right_image.x_end;
    const bool same_y = left_image->y_begin == right_image.y_begin &&
                        left_image->y_end == right_image.y_end;
    return (same_x && intervals_overlap_or_touch(
                          left_image->y_begin, left_image->y_end,
                          right_image.y_begin, right_image.y_end)) ||
           (same_y &&
            intervals_overlap_or_touch(left_image->x_begin, left_image->x_end,
                                       right_image.x_begin, right_image.x_end));
  }
  const TensorSlice& left_tensor = std::get<TensorSlice>(left);
  const TensorSlice& right_tensor = std::get<TensorSlice>(right);
  if (left_tensor.axes.size() != right_tensor.axes.size()) {
    return false;
  }
  std::size_t varying_axes = 0U;
  for (std::size_t axis = 0U; axis < left_tensor.axes.size(); ++axis) {
    if (left_tensor.axes[axis] == right_tensor.axes[axis]) {
      continue;
    }
    ++varying_axes;
    if (varying_axes > 1U ||
        !intervals_overlap_or_touch(
            left_tensor.axes[axis].begin, left_tensor.axes[axis].end,
            right_tensor.axes[axis].begin, right_tensor.axes[axis].end)) {
      return false;
    }
  }
  return varying_axes <= 1U;
}

/**
 * @brief Validates the caller's explicit atom budget.
 * @param budget Candidate operation budget.
 * @return True when the budget is nonzero and within the hard V-4 maximum.
 * @throws Nothing.
 */
bool valid_budget(const RegionComplexityBudget& budget) noexcept {
  return budget.maximum_atoms > 0U &&
         budget.maximum_atoms <= RegionSet::kMaximumAtoms;
}

/**
 * @brief Returns a typed budget-exhaustion outcome.
 * @return TooComplex without a replacement Region.
 * @throws std::bad_alloc when diagnostic storage cannot allocate.
 */
RegionOperationResult invalid_budget_result() {
  return RegionOperationResult::failure(
      RegionOperationStatus::TooComplex,
      "Region operation atom budget is zero or exceeds the V-4 hard limit.");
}

/**
 * @brief Locates one exact domain in a canonical clause.
 * @param region Clause Region to inspect.
 * @param domain Exact key to find.
 * @return Borrowed atom pointer or nullptr.
 * @throws Nothing.
 */
const RegionAtom* find_domain_atom(const RegionSet& region,
                                   RegionDomainKey domain) noexcept {
  for (const RegionAtom& atom : region.atoms()) {
    if (atom_domain(atom) == domain) {
      return &atom;
    }
  }
  return nullptr;
}

}  // namespace

/** @copydoc image_region_domain */
RegionDomainKey image_region_domain() noexcept {
  return kImageDomain;
}

/** @copydoc dense_tensor_region_domain */
RegionDomainKey dense_tensor_region_domain() noexcept {
  return kDenseTensorDomain;
}

/** @copydoc RegionSet::empty */
RegionSet RegionSet::empty() noexcept {
  return RegionSet{};
}

/** @copydoc RegionSet::whole */
RegionSet RegionSet::whole() noexcept {
  return RegionSet(Kind::Whole, {});
}

/** @copydoc RegionSet::from_atoms */
RegionSet RegionSet::from_atoms(std::vector<RegionAtom> atoms) {
  if (atoms.empty()) {
    return whole();
  }
  for (const RegionAtom& atom : atoms) {
    if (validate_atom(atom)) {
      return empty();
    }
  }
  std::sort(atoms.begin(), atoms.end(),
            [](const RegionAtom& left, const RegionAtom& right) {
              const RegionDomainKey left_domain = atom_domain(left);
              const RegionDomainKey right_domain = atom_domain(right);
              return left_domain < right_domain ||
                     (!(right_domain < left_domain) &&
                      atom_kind(left) < atom_kind(right));
            });

  std::vector<RegionAtom> normalized;
  normalized.reserve(std::min(atoms.size(), RegionSet::kMaximumAtoms + 1U));
  for (RegionAtom& atom : atoms) {
    if (normalized.empty() ||
        !(atom_domain(normalized.back()) == atom_domain(atom))) {
      normalized.push_back(std::move(atom));
    } else {
      if (atom_kind(normalized.back()) != atom_kind(atom)) {
        throw std::invalid_argument(
            "One Region domain cannot mix ImageRect and TensorSlice atoms.");
      }
      normalized.back() = intersect_compatible_atoms(normalized.back(), atom);
      if (validate_atom(normalized.back())) {
        return empty();
      }
    }
    if (normalized.size() > kMaximumAtoms) {
      throw std::length_error(
          "Region clause exceeds the V-4 normalized atom limit.");
    }
  }
  return RegionSet(Kind::Clause, std::move(normalized));
}

/** @copydoc RegionSet::from_image_rect */
RegionSet RegionSet::from_image_rect(ImageRect rect) {
  return from_atoms({RegionAtom(std::move(rect))});
}

/** @copydoc RegionSet::from_tensor_slice */
RegionSet RegionSet::from_tensor_slice(TensorSlice slice) {
  return from_atoms({RegionAtom(std::move(slice))});
}

/** @copydoc RegionOperationResult::exact */
RegionOperationResult RegionOperationResult::exact(RegionSet region) {
  return RegionOperationResult(RegionOperationStatus::Exact, std::move(region),
                               {});
}

/** @copydoc RegionOperationResult::conservative_superset */
RegionOperationResult RegionOperationResult::conservative_superset(
    RegionSet region, std::string reason) {
  return RegionOperationResult(RegionOperationStatus::ConservativeSuperset,
                               std::move(region), std::move(reason));
}

/** @copydoc RegionOperationResult::failure */
RegionOperationResult RegionOperationResult::failure(
    RegionOperationStatus status, std::string reason) {
  if (status == RegionOperationStatus::Exact ||
      status == RegionOperationStatus::ConservativeSuperset) {
    throw std::invalid_argument(
        "A Region-carrying outcome requires an explicit Region value.");
  }
  return RegionOperationResult(status, std::nullopt, std::move(reason));
}

/** @copydoc intersect_regions */
RegionOperationResult intersect_regions(const RegionSet& left,
                                        const RegionSet& right,
                                        RegionComplexityBudget budget) {
  if (!valid_budget(budget)) {
    return invalid_budget_result();
  }
  if (left.is_empty() || right.is_empty()) {
    return RegionOperationResult::exact(RegionSet::empty());
  }
  if (left.is_whole()) {
    if (right.atoms().size() > budget.maximum_atoms) {
      return invalid_budget_result();
    }
    return RegionOperationResult::exact(right);
  }
  if (right.is_whole()) {
    if (left.atoms().size() > budget.maximum_atoms) {
      return invalid_budget_result();
    }
    return RegionOperationResult::exact(left);
  }

  for (const RegionAtom& left_atom : left.atoms()) {
    const RegionAtom* right_atom =
        find_domain_atom(right, atom_domain(left_atom));
    if (!right_atom) {
      continue;
    }
    if (atom_kind(left_atom) != atom_kind(*right_atom)) {
      return RegionOperationResult::failure(
          RegionOperationStatus::Unsupported,
          "Region intersection found incompatible atom kinds for one domain.");
    }
    if (const auto* left_tensor = std::get_if<TensorSlice>(&left_atom)) {
      if (left_tensor->axes.size() !=
          std::get<TensorSlice>(*right_atom).axes.size()) {
        return RegionOperationResult::failure(
            RegionOperationStatus::Unsupported,
            "Region intersection found a TensorSlice rank mismatch.");
      }
    }
  }

  std::vector<RegionAtom> atoms = left.atoms();
  atoms.insert(atoms.end(), right.atoms().begin(), right.atoms().end());
  RegionSet result = RegionSet::from_atoms(std::move(atoms));
  if (!result.is_empty() && result.atoms().size() > budget.maximum_atoms) {
    return invalid_budget_result();
  }
  return RegionOperationResult::exact(std::move(result));
}

/** @copydoc region_contains */
RegionContainmentStatus region_contains(
    const RegionSet& outer, const RegionSet& inner,
    RegionComplexityBudget budget) noexcept {
  if (!valid_budget(budget) ||
      (!outer.is_empty() && !outer.is_whole() &&
       outer.atoms().size() > budget.maximum_atoms) ||
      (!inner.is_empty() && !inner.is_whole() &&
       inner.atoms().size() > budget.maximum_atoms)) {
    return RegionContainmentStatus::TooComplex;
  }
  if (inner.is_empty() || outer.is_whole()) {
    return RegionContainmentStatus::Contains;
  }
  if (outer.is_empty() || inner.is_whole()) {
    return RegionContainmentStatus::DoesNotContain;
  }
  for (const RegionAtom& outer_atom : outer.atoms()) {
    const RegionAtom* inner_atom =
        find_domain_atom(inner, atom_domain(outer_atom));
    if (!inner_atom) {
      return RegionContainmentStatus::DoesNotContain;
    }
    if (atom_kind(outer_atom) != atom_kind(*inner_atom)) {
      return RegionContainmentStatus::Unsupported;
    }
    if (const auto* outer_tensor = std::get_if<TensorSlice>(&outer_atom)) {
      if (outer_tensor->axes.size() !=
          std::get<TensorSlice>(*inner_atom).axes.size()) {
        return RegionContainmentStatus::Unsupported;
      }
    }
    if (!compatible_atom_contains(outer_atom, *inner_atom)) {
      return RegionContainmentStatus::DoesNotContain;
    }
  }
  return RegionContainmentStatus::Contains;
}

/** @copydoc union_regions */
RegionOperationResult union_regions(const RegionSet& left,
                                    const RegionSet& right,
                                    RegionComplexityBudget budget) {
  if (!valid_budget(budget)) {
    return invalid_budget_result();
  }
  if (left.is_whole() || right.is_whole()) {
    return RegionOperationResult::exact(RegionSet::whole());
  }
  if (left.is_empty()) {
    if (right.atoms().size() > budget.maximum_atoms) {
      return invalid_budget_result();
    }
    return RegionOperationResult::exact(right);
  }
  if (right.is_empty()) {
    if (left.atoms().size() > budget.maximum_atoms) {
      return invalid_budget_result();
    }
    return RegionOperationResult::exact(left);
  }
  const RegionContainmentStatus left_contains =
      region_contains(left, right, budget);
  if (left_contains == RegionContainmentStatus::Contains) {
    return RegionOperationResult::exact(left);
  }
  const RegionContainmentStatus right_contains =
      region_contains(right, left, budget);
  if (right_contains == RegionContainmentStatus::Contains) {
    return RegionOperationResult::exact(right);
  }
  if (left_contains == RegionContainmentStatus::Unsupported ||
      right_contains == RegionContainmentStatus::Unsupported) {
    return RegionOperationResult::failure(
        RegionOperationStatus::Unsupported,
        "Region union cannot compare incompatible atom kinds or ranks.");
  }
  if (left.atoms().size() != 1U || right.atoms().size() != 1U ||
      !(atom_domain(left.atoms().front()) ==
        atom_domain(right.atoms().front())) ||
      atom_kind(left.atoms().front()) != atom_kind(right.atoms().front())) {
    return RegionOperationResult::failure(
        RegionOperationStatus::TooComplex,
        "Region union is not representable by one bounded clause.");
  }
  const RegionAtom& left_atom = left.atoms().front();
  const RegionAtom& right_atom = right.atoms().front();
  if (const auto* left_tensor = std::get_if<TensorSlice>(&left_atom)) {
    if (left_tensor->axes.size() !=
        std::get<TensorSlice>(right_atom).axes.size()) {
      return RegionOperationResult::failure(
          RegionOperationStatus::Unsupported,
          "Region union found a TensorSlice rank mismatch.");
    }
  }
  RegionSet hull = RegionSet::from_atoms({atom_hull(left_atom, right_atom)});
  if (atom_union_is_exact(left_atom, right_atom)) {
    return RegionOperationResult::exact(std::move(hull));
  }
  if (budget.allow_conservative_superset) {
    return RegionOperationResult::conservative_superset(
        std::move(hull),
        "Nonrectangular union widened to an explicit bounding Region.");
  }
  return RegionOperationResult::failure(
      RegionOperationStatus::TooComplex,
      "Nonrectangular union exceeds the one-clause exact Region subset.");
}

/** @copydoc difference_regions */
RegionOperationResult difference_regions(const RegionSet& left,
                                         const RegionSet& right,
                                         RegionComplexityBudget budget) {
  if (!valid_budget(budget)) {
    return invalid_budget_result();
  }
  if (left.is_empty() || right.is_whole()) {
    return RegionOperationResult::exact(RegionSet::empty());
  }
  if (right.is_empty()) {
    if (left.atoms().size() > budget.maximum_atoms) {
      return invalid_budget_result();
    }
    return RegionOperationResult::exact(left);
  }
  if (left.is_whole()) {
    return RegionOperationResult::failure(
        RegionOperationStatus::TooComplex,
        "Whole minus a bounded Region is not representable by one clause.");
  }
  const RegionContainmentStatus removal_contains =
      region_contains(right, left, budget);
  if (removal_contains == RegionContainmentStatus::Contains) {
    return RegionOperationResult::exact(RegionSet::empty());
  }
  RegionOperationResult overlap = intersect_regions(left, right, budget);
  if (overlap.status() != RegionOperationStatus::Exact) {
    return overlap;
  }
  if (overlap.region()->is_empty()) {
    return RegionOperationResult::exact(left);
  }
  if (left.atoms().size() != 1U || right.atoms().size() != 1U ||
      !(atom_domain(left.atoms().front()) ==
        atom_domain(right.atoms().front())) ||
      atom_kind(left.atoms().front()) != atom_kind(right.atoms().front())) {
    return RegionOperationResult::failure(
        RegionOperationStatus::TooComplex,
        "Region difference would require multiple clauses or atoms.");
  }

  const RegionAtom& source = left.atoms().front();
  const RegionAtom& cut = overlap.region()->atoms().front();
  if (const auto* source_image = std::get_if<ImageRect>(&source)) {
    const ImageRect& cut_image = std::get<ImageRect>(cut);
    ImageRect remainder = *source_image;
    const bool full_x = cut_image.x_begin == source_image->x_begin &&
                        cut_image.x_end == source_image->x_end;
    const bool full_y = cut_image.y_begin == source_image->y_begin &&
                        cut_image.y_end == source_image->y_end;
    if (full_x && cut_image.y_begin == source_image->y_begin) {
      remainder.y_begin = cut_image.y_end;
    } else if (full_x && cut_image.y_end == source_image->y_end) {
      remainder.y_end = cut_image.y_begin;
    } else if (full_y && cut_image.x_begin == source_image->x_begin) {
      remainder.x_begin = cut_image.x_end;
    } else if (full_y && cut_image.x_end == source_image->x_end) {
      remainder.x_end = cut_image.x_begin;
    } else {
      return RegionOperationResult::failure(
          RegionOperationStatus::TooComplex,
          "ImageRect difference would create a sparse Region.");
    }
    return RegionOperationResult::exact(
        RegionSet::from_image_rect(std::move(remainder)));
  }

  const TensorSlice& source_tensor = std::get<TensorSlice>(source);
  const TensorSlice& cut_tensor = std::get<TensorSlice>(cut);
  std::optional<std::size_t> differing_axis;
  bool remove_low = false;
  for (std::size_t axis = 0U; axis < source_tensor.axes.size(); ++axis) {
    if (source_tensor.axes[axis] == cut_tensor.axes[axis]) {
      continue;
    }
    if (differing_axis.has_value()) {
      return RegionOperationResult::failure(
          RegionOperationStatus::TooComplex,
          "TensorSlice difference would create a sparse Region.");
    }
    const RegionInterval& source_axis = source_tensor.axes[axis];
    const RegionInterval& cut_axis = cut_tensor.axes[axis];
    if (cut_axis.begin == source_axis.begin) {
      remove_low = true;
    } else if (cut_axis.end == source_axis.end) {
      remove_low = false;
    } else {
      return RegionOperationResult::failure(
          RegionOperationStatus::TooComplex,
          "TensorSlice difference splits one logical axis.");
    }
    differing_axis = axis;
  }
  if (!differing_axis.has_value()) {
    return RegionOperationResult::exact(RegionSet::empty());
  }
  TensorSlice remainder = source_tensor;
  if (remove_low) {
    remainder.axes[*differing_axis].begin =
        cut_tensor.axes[*differing_axis].end;
  } else {
    remainder.axes[*differing_axis].end =
        cut_tensor.axes[*differing_axis].begin;
  }
  return RegionOperationResult::exact(
      RegionSet::from_tensor_slice(std::move(remainder)));
}

/** @copydoc clip_region_to_image_bounds */
RegionOperationResult clip_region_to_image_bounds(
    const RegionSet& region, const ImageRect& bounds,
    RegionComplexityBudget budget) {
  RegionSet bound_region = RegionSet::from_image_rect(bounds);
  return intersect_regions(region, bound_region, budget);
}

/** @copydoc clip_region_to_tensor_shape */
RegionOperationResult clip_region_to_tensor_shape(
    const RegionSet& region, RegionDomainKey domain,
    const std::vector<std::size_t>& shape, RegionComplexityBudget budget) {
  if (!domain.valid()) {
    throw std::invalid_argument(
        "Tensor bounds require a nonzero Region domain key.");
  }
  if (shape.empty()) {
    throw std::invalid_argument("Tensor bounds require positive rank.");
  }
  TensorSlice bounds;
  bounds.domain = domain;
  bounds.axes.reserve(shape.size());
  for (std::size_t extent : shape) {
    if (extent == 0U) {
      throw std::invalid_argument(
          "Tensor bounds require positive concrete extents.");
    }
    if (extent > std::numeric_limits<std::uint64_t>::max()) {
      throw std::overflow_error(
          "Tensor bound extent exceeds uint64_t representation.");
    }
    bounds.axes.push_back({0U, static_cast<std::uint64_t>(extent)});
  }
  return intersect_regions(
      region, RegionSet::from_tensor_slice(std::move(bounds)), budget);
}

}  // namespace ps
