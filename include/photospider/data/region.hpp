#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

/**
 * @file region.hpp
 * @brief Canonical dependency-neutral logical Region value contracts.
 */

namespace ps {

/**
 * @brief Stable explicit identity of one logical Region coordinate domain.
 *
 * @throws Nothing for ordinary value operations.
 * @note The all-zero value is invalid. Keys identify logical interpretation;
 *       they are not allocation, Value revision, graph, cache, or persistence
 *       identities.
 */
struct RegionDomainKey {
  /** @brief Most-significant fixed identity word. */
  std::uint64_t high = 0U;
  /** @brief Least-significant fixed identity word. */
  std::uint64_t low = 0U;

  /**
   * @brief Reports whether this key names a usable logical domain.
   * @return True unless both identity words are zero.
   * @throws Nothing.
   */
  bool valid() const noexcept { return high != 0U || low != 0U; }

  /**
   * @brief Compares complete logical-domain identity.
   * @param other Key to compare.
   * @return True when both fixed words match.
   * @throws Nothing.
   */
  bool operator==(const RegionDomainKey& other) const noexcept {
    return high == other.high && low == other.low;
  }

  /**
   * @brief Provides deterministic canonical ordering.
   * @param other Key to compare.
   * @return True when this key sorts before `other`.
   * @throws Nothing.
   */
  bool operator<(const RegionDomainKey& other) const noexcept {
    return high < other.high || (high == other.high && low < other.low);
  }
};

/**
 * @brief Returns the reserved built-in image-coordinate domain.
 * @return Nonzero process-independent key for x/y image coordinates.
 * @throws Nothing.
 * @note The key is a logical discriminator and never enters cache identity.
 */
RegionDomainKey image_region_domain() noexcept;

/**
 * @brief Returns the reserved built-in dense-tensor coordinate domain.
 * @return Nonzero process-independent key for rank-general tensor axes.
 * @throws Nothing.
 * @note The key does not imply an ImageFacet or any x/y axis positions.
 */
RegionDomainKey dense_tensor_region_domain() noexcept;

/**
 * @brief One unsigned half-open interval used by TensorSlice.
 *
 * @throws Nothing for ordinary value operations.
 * @note Construction through RegionSet rejects inverted intervals and
 *       canonicalizes an empty interval to the complete Empty Region.
 */
struct RegionInterval {
  /** @brief Inclusive logical-axis beginning. */
  std::uint64_t begin = 0U;
  /** @brief Exclusive logical-axis ending. */
  std::uint64_t end = 0U;

  /**
   * @brief Compares both half-open endpoints.
   * @param other Interval to compare.
   * @return True when begin and end both match.
   * @throws Nothing.
   */
  bool operator==(const RegionInterval& other) const noexcept {
    return begin == other.begin && end == other.end;
  }
};

/**
 * @brief Signed half-open two-dimensional logical image rectangle.
 *
 * @throws Nothing for ordinary value operations.
 * @note Endpoints are stored directly so Region algebra never needs unchecked
 *       origin-plus-extent arithmetic.
 */
struct ImageRect {
  /** @brief Explicit logical image domain. */
  RegionDomainKey domain = image_region_domain();
  /** @brief Inclusive x endpoint. */
  std::int64_t x_begin = 0;
  /** @brief Exclusive x endpoint. */
  std::int64_t x_end = 0;
  /** @brief Inclusive y endpoint. */
  std::int64_t y_begin = 0;
  /** @brief Exclusive y endpoint. */
  std::int64_t y_end = 0;

  /**
   * @brief Compares domain and all endpoints.
   * @param other Rectangle to compare.
   * @return True when every logical fact matches.
   * @throws Nothing.
   */
  bool operator==(const ImageRect& other) const noexcept {
    return domain == other.domain && x_begin == other.x_begin &&
           x_end == other.x_end && y_begin == other.y_begin &&
           y_end == other.y_end;
  }
};

/**
 * @brief Rank-general half-open logical DenseTensor slice.
 *
 * @throws std::bad_alloc when copied axis storage cannot allocate.
 * @note Axis order is authoritative. No axis is guessed from an ImageFacet.
 */
struct TensorSlice {
  /** @brief Explicit logical tensor domain. */
  RegionDomainKey domain = dense_tensor_region_domain();
  /** @brief One ordered half-open interval per logical tensor axis. */
  std::vector<RegionInterval> axes;

  /**
   * @brief Compares domain, rank, and every ordered interval.
   * @param other Slice to compare.
   * @return True when all logical facts match.
   * @throws Nothing under vector equality.
   */
  bool operator==(const TensorSlice& other) const noexcept {
    return domain == other.domain && axes == other.axes;
  }
};

/**
 * @brief One supported V-4 logical Region atom.
 *
 * @note Provider-defined atoms and persistence remain outside this bounded
 *       implementation slice.
 */
using RegionAtom = std::variant<ImageRect, TensorSlice>;

/**
 * @brief Canonical bounded Region represented as Empty, Whole, or one clause.
 *
 * The one nonempty clause is a conjunction of at most eight normalized atoms.
 * Different domains constrain the same selected work simultaneously. Repeated
 * same-domain atoms normalize by exact intersection.
 *
 * @throws std::bad_alloc when owned atom storage cannot allocate.
 * @note Whole is the identity of one empty clause; Empty has zero clauses.
 *       Neither is encoded as a zero-sized atom.
 */
class RegionSet final {
 public:
  /** @brief Maximum number of normalized atoms in the V-4 clause. */
  static constexpr std::size_t kMaximumAtoms = 8U;

  /** @brief Canonical top-level representation discriminator. */
  enum class Kind {
    /** @brief Zero clauses and no selected coordinates. */
    Empty,
    /** @brief One empty clause selecting every coordinate. */
    Whole,
    /** @brief One nonempty normalized conjunction. */
    Clause,
  };

  /**
   * @brief Constructs canonical Empty.
   * @throws Nothing.
   * @note Default construction is deliberately the safe no-work state.
   */
  RegionSet() noexcept = default;

  /**
   * @brief Creates canonical Empty.
   * @return Empty Region value.
   * @throws Nothing.
   */
  static RegionSet empty() noexcept;

  /**
   * @brief Creates canonical Whole.
   * @return Whole Region value.
   * @throws Nothing.
   */
  static RegionSet whole() noexcept;

  /**
   * @brief Normalizes one bounded conjunction of logical atoms.
   *
   * @param atoms Owned candidate atoms in any order.
   * @return Canonical Empty or one sorted nonempty clause.
   * @throws std::invalid_argument for invalid keys, rank zero, inverted
   *         intervals, or conflicting atom kinds sharing one domain.
   * @throws std::length_error when more than kMaximumAtoms remain normalized.
   * @throws std::bad_alloc when normalization storage cannot allocate.
   * @note Any empty atom makes the complete conjunction canonical Empty.
   */
  static RegionSet from_atoms(std::vector<RegionAtom> atoms);

  /**
   * @brief Creates one validated ImageRect Region.
   * @param rect Candidate logical rectangle.
   * @return Canonical Empty for an empty interval, otherwise one-atom clause.
   * @throws The same exceptions as from_atoms().
   */
  static RegionSet from_image_rect(ImageRect rect);

  /**
   * @brief Creates one validated TensorSlice Region.
   * @param slice Candidate rank-general slice.
   * @return Canonical Empty for any empty axis, otherwise one-atom clause.
   * @throws The same exceptions as from_atoms().
   */
  static RegionSet from_tensor_slice(TensorSlice slice);

  /**
   * @brief Returns the canonical representation kind.
   * @return Empty, Whole, or Clause.
   * @throws Nothing.
   */
  Kind kind() const noexcept { return kind_; }

  /**
   * @brief Reports canonical Empty.
   * @return True only for the zero-clause state.
   * @throws Nothing.
   */
  bool is_empty() const noexcept { return kind_ == Kind::Empty; }

  /**
   * @brief Reports canonical Whole.
   * @return True only for the one-empty-clause state.
   * @throws Nothing.
   */
  bool is_whole() const noexcept { return kind_ == Kind::Whole; }

  /**
   * @brief Returns the normalized conjunction atoms.
   * @return Borrowed empty vector for Empty/Whole or sorted clause atoms.
   * @throws Nothing.
   * @note The borrowed vector remains valid for this RegionSet lifetime.
   */
  const std::vector<RegionAtom>& atoms() const noexcept { return atoms_; }

  /**
   * @brief Compares canonical Region meaning.
   * @param other Region to compare.
   * @return True when kind and all normalized atoms match.
   * @throws Nothing under supported atom equality.
   */
  bool operator==(const RegionSet& other) const noexcept {
    return kind_ == other.kind_ && atoms_ == other.atoms_;
  }

 private:
  /**
   * @brief Constructs an already-normalized canonical state.
   * @param kind Canonical representation discriminator.
   * @param atoms Sorted normalized atoms for Clause, empty otherwise.
   * @throws std::bad_alloc when atom ownership transfer allocates.
   * @note Callers are implementation helpers that have validated invariants.
   */
  RegionSet(Kind kind, std::vector<RegionAtom> atoms)
      : kind_(kind), atoms_(std::move(atoms)) {}

  /** @brief Canonical top-level state. */
  Kind kind_ = Kind::Empty;
  /** @brief Owned normalized clause atoms, empty for Empty and Whole. */
  std::vector<RegionAtom> atoms_;
};

/**
 * @brief Explicit bounded algebra policy for one Region operation.
 *
 * @throws Nothing for ordinary value operations.
 * @note A zero atom budget is invalid and produces TooComplex. Conservative
 *       widening is opt-in and always labelled in the returned outcome.
 */
struct RegionComplexityBudget {
  /** @brief Maximum normalized atoms accepted by this operation. */
  std::size_t maximum_atoms = RegionSet::kMaximumAtoms;
  /** @brief Whether an explicitly labelled bounding superset may be returned.
   */
  bool allow_conservative_superset = false;
};

/**
 * @brief Typed status of Region algebra or clipping.
 *
 * @note Only Exact and ConservativeSuperset carry a Region value.
 */
enum class RegionOperationStatus {
  /** @brief Exact normalized result. */
  Exact,
  /** @brief Explicitly requested labelled widening. */
  ConservativeSuperset,
  /** @brief Required domain mapping is unavailable. */
  Unknown,
  /** @brief Operand kinds or ranks cannot be interpreted by this operation. */
  Unsupported,
  /** @brief Exact result exceeds the bounded representation or caller budget.
   */
  TooComplex,
};

/**
 * @brief Owned typed outcome of one Region algebra operation.
 *
 * @throws std::bad_alloc when copied Region or diagnostic storage allocates.
 * @note Failure states never smuggle Empty or Whole as a replacement result.
 */
class RegionOperationResult final {
 public:
  /**
   * @brief Creates an Exact outcome.
   * @param region Exact normalized result.
   * @return Owned Exact result.
   * @throws std::bad_alloc when Region storage cannot be copied.
   */
  static RegionOperationResult exact(RegionSet region);

  /**
   * @brief Creates a labelled conservative superset.
   * @param region Explicit widened result.
   * @param reason Owned reader-facing widening reason.
   * @return Owned ConservativeSuperset result.
   * @throws std::bad_alloc when storage cannot allocate.
   */
  static RegionOperationResult conservative_superset(RegionSet region,
                                                     std::string reason);

  /**
   * @brief Creates a result without a Region value.
   * @param status Unknown, Unsupported, or TooComplex.
   * @param reason Owned diagnostic.
   * @return Owned typed failure.
   * @throws std::invalid_argument when status could carry a Region.
   * @throws std::bad_alloc when diagnostic storage cannot allocate.
   */
  static RegionOperationResult failure(RegionOperationStatus status,
                                       std::string reason);

  /**
   * @brief Returns the typed outcome discriminator.
   * @return Exact, ConservativeSuperset, Unknown, Unsupported, or TooComplex.
   * @throws Nothing.
   */
  RegionOperationStatus status() const noexcept { return status_; }

  /**
   * @brief Returns the carried Region when the status permits one.
   * @return Borrowed optional Region.
   * @throws Nothing.
   */
  const std::optional<RegionSet>& region() const noexcept { return region_; }

  /**
   * @brief Returns the owned diagnostic or conservative label.
   * @return Borrowed diagnostic string.
   * @throws Nothing.
   */
  const std::string& reason() const noexcept { return reason_; }

 private:
  /**
   * @brief Constructs one internally validated outcome.
   * @param status Typed outcome.
   * @param region Optional carried Region.
   * @param reason Owned diagnostic.
   * @throws std::bad_alloc when ownership transfer allocates.
   */
  RegionOperationResult(RegionOperationStatus status,
                        std::optional<RegionSet> region, std::string reason)
      : status_(status),
        region_(std::move(region)),
        reason_(std::move(reason)) {}

  /** @brief Typed outcome discriminator. */
  RegionOperationStatus status_ = RegionOperationStatus::Unknown;
  /** @brief Exact or conservative Region, absent for failures. */
  std::optional<RegionSet> region_;
  /** @brief Owned failure diagnostic or widening label. */
  std::string reason_;
};

/**
 * @brief Typed containment relation between two logical Regions.
 */
enum class RegionContainmentStatus {
  /** @brief The left Region contains the complete right Region. */
  Contains,
  /** @brief The left Region definitely does not contain the right Region. */
  DoesNotContain,
  /** @brief Required domain knowledge is unavailable. */
  Unknown,
  /** @brief Atom kinds or tensor ranks cannot be compared. */
  Unsupported,
  /** @brief Comparison exceeds the explicit complexity budget. */
  TooComplex,
};

/**
 * @brief Computes exact bounded Region intersection.
 * @param left First normalized Region.
 * @param right Second normalized Region.
 * @param budget Explicit nonzero complexity limit.
 * @return Typed normalized outcome.
 * @throws std::bad_alloc when result storage cannot allocate.
 * @note Unsupported rank/kind combinations remain typed and never become
 *       Empty.
 */
RegionOperationResult intersect_regions(const RegionSet& left,
                                        const RegionSet& right,
                                        RegionComplexityBudget budget = {});

/**
 * @brief Computes representable union or an explicitly labelled hull.
 * @param left First normalized Region.
 * @param right Second normalized Region.
 * @param budget Complexity limit and conservative-widening opt-in.
 * @return Exact, ConservativeSuperset, Unsupported, or TooComplex.
 * @throws std::bad_alloc when result or diagnostic storage cannot allocate.
 * @note A nonrectangular union never silently becomes a hull.
 */
RegionOperationResult union_regions(const RegionSet& left,
                                    const RegionSet& right,
                                    RegionComplexityBudget budget = {});

/**
 * @brief Computes a difference representable by the bounded V-4 subset.
 * @param left Region being subtracted from.
 * @param right Region to remove.
 * @param budget Explicit nonzero complexity limit.
 * @return Exact, Unsupported, or TooComplex.
 * @throws std::bad_alloc when result storage cannot allocate.
 * @note Sparse differences return TooComplex rather than widening.
 */
RegionOperationResult difference_regions(const RegionSet& left,
                                         const RegionSet& right,
                                         RegionComplexityBudget budget = {});

/**
 * @brief Tests whether `outer` contains all coordinates selected by `inner`.
 * @param outer Candidate containing Region.
 * @param inner Candidate contained Region.
 * @param budget Explicit nonzero complexity limit.
 * @return Typed containment relation.
 * @throws Nothing.
 * @note Missing or incompatible domains remain typed and are not interpreted
 *       as false geometry.
 */
RegionContainmentStatus region_contains(
    const RegionSet& outer, const RegionSet& inner,
    RegionComplexityBudget budget = {}) noexcept;

/**
 * @brief Clips a Region by one signed image-domain bound.
 * @param region Region to constrain.
 * @param bounds Valid or empty image bound naming the target domain.
 * @param budget Explicit nonzero complexity limit.
 * @return Typed normalized intersection outcome.
 * @throws std::invalid_argument when bounds are malformed.
 * @throws std::bad_alloc when result storage cannot allocate.
 * @note Whole clips to the exact bound; unrelated domains remain conjunctive.
 */
RegionOperationResult clip_region_to_image_bounds(
    const RegionSet& region, const ImageRect& bounds,
    RegionComplexityBudget budget = {});

/**
 * @brief Clips a Region by one concrete tensor shape.
 * @param region Region to constrain.
 * @param domain Explicit tensor domain to constrain.
 * @param shape Positive concrete extent per logical axis.
 * @param budget Explicit nonzero complexity limit.
 * @return Typed normalized intersection outcome.
 * @throws std::invalid_argument for invalid key, rank zero, or zero extent.
 * @throws std::overflow_error when an extent cannot fit uint64_t.
 * @throws std::bad_alloc when result storage cannot allocate.
 * @note Rank is preserved exactly; no image-axis interpretation occurs.
 */
RegionOperationResult clip_region_to_tensor_shape(
    const RegionSet& region, RegionDomainKey domain,
    const std::vector<std::size_t>& shape, RegionComplexityBudget budget = {});

}  // namespace ps
