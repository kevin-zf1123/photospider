#include "core/host_output_authorization.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "core/value_validation.hpp"
#include "photospider/data/image_view.hpp"

namespace ps {
namespace {

/**
 * @brief Adds two byte-domain values without unsigned wraparound.
 * @param left First nonnegative value.
 * @param right Second nonnegative value.
 * @return Exact sum.
 * @throws std::overflow_error when the sum exceeds std::size_t.
 */
std::size_t checked_output_add(std::size_t left, std::size_t right) {
  if (left > std::numeric_limits<std::size_t>::max() - right) {
    throw std::overflow_error("Host output byte addition overflowed.");
  }
  return left + right;
}

/**
 * @brief Multiplies two byte-domain values without unsigned wraparound.
 * @param left First nonnegative value.
 * @param right Second nonnegative value.
 * @return Exact product.
 * @throws std::overflow_error when the product exceeds std::size_t.
 */
std::size_t checked_output_multiply(std::size_t left, std::size_t right) {
  if (right != 0U && left > std::numeric_limits<std::size_t>::max() / right) {
    throw std::overflow_error("Host output byte multiplication overflowed.");
  }
  return left * right;
}

/**
 * @brief Reports whether one integer is a positive power of two.
 * @param value Candidate alignment.
 * @return True exactly for positive powers of two.
 * @throws Nothing.
 */
bool is_power_of_two(std::size_t value) noexcept {
  return value != 0U && (value & (value - 1U)) == 0U;
}

/**
 * @brief Converts a checked signed coordinate distance to std::size_t.
 * @param coordinate Endpoint greater than or equal to origin.
 * @param origin Inclusive origin.
 * @return Exact nonnegative distance.
 * @throws std::invalid_argument when coordinate precedes origin.
 * @throws std::overflow_error when the distance exceeds std::size_t.
 * @note Unsigned subtraction after standard signed-to-unsigned conversion
 * produces the exact mathematical distance across the complete int64 domain.
 */
std::size_t checked_coordinate_distance(std::int64_t coordinate,
                                        std::int64_t origin) {
  if (coordinate < origin) {
    throw std::invalid_argument(
        "Host output coordinate precedes the planned origin.");
  }
  const std::uint64_t distance = static_cast<std::uint64_t>(coordinate) -
                                 static_cast<std::uint64_t>(origin);
  if constexpr (sizeof(std::size_t) < sizeof(std::uint64_t)) {
    if (distance > std::numeric_limits<std::size_t>::max()) {
      throw std::overflow_error(
          "Host output coordinate distance exceeds size_t.");
    }
  }
  return static_cast<std::size_t>(distance);
}

/**
 * @brief Tests strict intersection of two nonempty image rectangles.
 * @param left First validated rectangle.
 * @param right Second validated rectangle in the same domain.
 * @return True when their selected logical pixels intersect.
 * @throws Nothing.
 */
bool image_rectangles_overlap(const ImageRect& left,
                              const ImageRect& right) noexcept {
  return left.x_begin < right.x_end && right.x_begin < left.x_end &&
         left.y_begin < right.y_end && right.y_begin < left.y_end;
}

/**
 * @brief Tests byte intersection of two individually sorted span vectors.
 * @param left First checked non-overlapping span vector.
 * @param right Second checked non-overlapping span vector.
 * @return True when any byte belongs to both vectors.
 * @throws Nothing under checked span-end preconditions.
 */
bool output_spans_overlap(
    const std::vector<HostOutputWriteSpan>& left,
    const std::vector<HostOutputWriteSpan>& right) noexcept {
  std::size_t left_index = 0U;
  std::size_t right_index = 0U;
  while (left_index < left.size() && right_index < right.size()) {
    const HostOutputWriteSpan& left_span = left[left_index];
    const HostOutputWriteSpan& right_span = right[right_index];
    const std::size_t left_end =
        left_span.allocation_offset + left_span.byte_size;
    const std::size_t right_end =
        right_span.allocation_offset + right_span.byte_size;
    if (left_span.allocation_offset < right_end &&
        right_span.allocation_offset < left_end) {
      return true;
    }
    if (left_end <= right_span.allocation_offset) {
      ++left_index;
    } else {
      ++right_index;
    }
  }
  return false;
}

/**
 * @brief Converts the planned data window to its exact image Region atom.
 * @param facet Valid complete image metadata.
 * @return Nonempty built-in image-domain rectangle.
 * @throws Nothing under validated nonempty data-window preconditions.
 */
ImageRect planned_image_rect(const ImageFacet& facet) noexcept {
  return ImageRect{image_region_domain(), facet.data_window.x_begin,
                   facet.data_window.x_end, facet.data_window.y_begin,
                   facet.data_window.y_end};
}

}  // namespace

/** @copydoc DenseImageOutputPlan::DenseImageOutputPlan */
DenseImageOutputPlan::DenseImageOutputPlan(
    std::string output_name, DenseTensorDescriptor descriptor,
    ImageFacet image_facet, StridedLayout layout, std::size_t storage_size,
    std::size_t alignment, RegionSet region, std::size_t width,
    std::size_t height, std::size_t channels, std::size_t element_bytes,
    std::size_t pixel_bytes, std::size_t row_stride)
    : output_name_(std::move(output_name)),
      descriptor_(std::move(descriptor)),
      image_facet_(std::move(image_facet)),
      layout_(std::move(layout)),
      storage_size_(storage_size),
      alignment_(alignment),
      region_(std::move(region)),
      width_(width),
      height_(height),
      channels_(channels),
      element_bytes_(element_bytes),
      pixel_bytes_(pixel_bytes),
      row_stride_(row_stride) {}  // NOLINT(whitespace/indent_namespace)

/** @copydoc DenseImageOutputPlan::create */
DenseImageOutputPlan DenseImageOutputPlan::create(
    std::string output_name, DenseTensorDescriptor descriptor,
    ImageFacet image_facet, StridedLayout layout, std::size_t storage_size,
    std::size_t alignment) {
  if (output_name.empty() || output_name.size() > kMaximumHostOutputNameBytes ||
      output_name.find('\0') != std::string::npos) {
    throw std::invalid_argument(
        "Host output name must contain 1-128 non-NUL bytes.");
  }
  if (!is_power_of_two(alignment) || alignment > kMaximumHostOutputAlignment) {
    throw std::invalid_argument(
        "Host output alignment is outside the frozen power-of-two bound.");
  }

  validate_dense_tensor_image_metadata(descriptor, image_facet);
  validate_dense_tensor_producer_envelope(descriptor, layout, storage_size);
  const std::size_t element_bytes = dense_tensor_element_bytes(descriptor);
  if (alignment < element_bytes) {
    throw std::invalid_argument(
        "Host output alignment is smaller than its element width.");
  }

  const std::size_t rank = descriptor.shape.size();
  if (layout.byte_strides.size() != rank || image_facet.x_axis >= rank ||
      image_facet.y_axis >= rank ||
      (image_facet.channel_axis.has_value() &&
       *image_facet.channel_axis >= rank)) {
    throw std::invalid_argument(
        "Host output image axes and layout must match tensor rank.");
  }
  for (std::size_t axis = 0U; axis < rank; ++axis) {
    const bool assigned = axis == image_facet.x_axis ||
                          axis == image_facet.y_axis ||
                          (image_facet.channel_axis.has_value() &&
                           axis == *image_facet.channel_axis);
    if (!assigned && descriptor.shape[axis] != 1U) {
      throw std::invalid_argument(
          "Host output unassigned DenseImage axes must be singleton.");
    }
  }

  const std::size_t width = image_bounds_width(image_facet.data_window);
  const std::size_t height = image_bounds_height(image_facet.data_window);
  const std::size_t channels = image_facet.channel_axis.has_value()
                                   ? descriptor.shape[*image_facet.channel_axis]
                                   : 1U;
  const std::size_t pixel_bytes =
      checked_output_multiply(channels, element_bytes);
  if (pixel_bytes >
      static_cast<std::size_t>(std::numeric_limits<std::ptrdiff_t>::max())) {
    throw std::overflow_error("Host output pixel stride exceeds ptrdiff_t.");
  }
  if (layout.byte_strides[image_facet.x_axis] !=
      static_cast<std::ptrdiff_t>(pixel_bytes)) {
    throw std::invalid_argument(
        "Host output x stride must match interleaved pixel width.");
  }
  if (image_facet.channel_axis.has_value() &&
      layout.byte_strides[*image_facet.channel_axis] !=
          static_cast<std::ptrdiff_t>(element_bytes)) {
    throw std::invalid_argument(
        "Host output channel stride must match channel element width.");
  }
  const std::ptrdiff_t signed_row_stride =
      layout.byte_strides[image_facet.y_axis];
  if (signed_row_stride <= 0) {
    throw std::invalid_argument("Host output row stride must be positive.");
  }
  const std::size_t row_stride = static_cast<std::size_t>(signed_row_stride);
  const std::size_t row_bytes = checked_output_multiply(width, pixel_bytes);
  if (row_stride < row_bytes) {
    throw std::invalid_argument(
        "Host output row stride is smaller than active row bytes.");
  }
  const std::size_t expected_storage = checked_output_add(
      checked_output_multiply(height - 1U, row_stride), row_bytes);
  if (expected_storage != storage_size) {
    throw std::invalid_argument(
        "Host output storage size does not match its interleaved rows.");
  }

  const ImageRect full_rect = planned_image_rect(image_facet);
  RegionSet region = RegionSet::from_image_rect(full_rect);
  return DenseImageOutputPlan(
      std::move(output_name), std::move(descriptor), std::move(image_facet),
      std::move(layout), storage_size, alignment, std::move(region), width,
      height, channels, element_bytes, pixel_bytes, row_stride);
}

/**
 * @brief Shared synchronized state for one Host output binding and its grants.
 *
 * @throws std::bad_alloc when plan, reservation, failure, or publication
 * storage cannot allocate.
 * @note The state owns the one ValueBuilder and whole WriteLease. Grant
 * pointers borrow that allocation only after checking lifecycle and generation
 * under `mutex`; payload writes then proceed without holding the mutex.
 */
struct HostOutputWriteGrant::State final {
  /** @brief Closed lifecycle discriminator for the mutable binding. */
  enum class Lifecycle {
    /** @brief Grant issuance and successful retirement are permitted. */
    Open,
    /** @brief Seal has closed issuance and is publishing the Value. */
    Sealing,
    /** @brief One immutable Value has been published. */
    Sealed,
    /** @brief Sticky failure or cancellation prevents publication. */
    Failed,
  };

  /**
   * @brief Registry record for one active whole or tile reservation.
   * @throws std::bad_alloc when copied span storage cannot allocate.
   */
  struct Reservation final {
    /** @brief Unique nonzero active grant identifier. */
    std::uint64_t grant_id = 0U;
    /** @brief Exact logical image rectangle reserved by the grant. */
    ImageRect image_region;
    /** @brief True when the complete writable envelope is reserved. */
    bool whole = false;
    /** @brief Sorted checked byte spans retained for overlap proof. */
    std::vector<HostOutputWriteSpan> spans;
  };

  /** @brief Serializes lifecycle, grant registry, and sticky diagnostic. */
  mutable std::mutex mutex;
  /** @brief Immutable output plan copied before allocation. */
  DenseImageOutputPlan plan;
  /** @brief Sole allocation-to-Value publication authority. */
  ValueBuilder builder;
  /** @brief Sole whole-allocation mutable lease, reset before seal. */
  std::optional<WriteLease> lease;
  /** @brief Borrowed allocation base retained by builder and lease. */
  std::byte* allocation_base = nullptr;
  /** @brief Immutable physical allocation identity. */
  AllocationIdentity allocation_identity;
  /** @brief Current binding lifecycle. */
  Lifecycle lifecycle = Lifecycle::Open;
  /** @brief Current revocation generation; changes on terminal closure. */
  std::uint64_t generation = 1U;
  /** @brief Next nonzero grant ID, never reused. */
  std::uint64_t next_grant_id = 1U;
  /** @brief Active pairwise-disjoint whole/tile reservations. */
  std::vector<Reservation> reservations;
  /** @brief First sticky failure or cancellation diagnostic. */
  std::optional<std::string> failure;
  /** @brief Published Value retained solely to preserve publish-once state. */
  std::optional<Value> published_value;

  /**
   * @brief Owns one freshly allocated open builder and whole lease.
   * @param plan_in Complete immutable plan.
   * @param builder_in Fresh matching ValueBuilder.
   * @param lease_in Sole active whole-allocation WriteLease.
   * @throws std::logic_error if the supposedly active input lease violates its
   * constructor precondition.
   */
  State(DenseImageOutputPlan plan_in, ValueBuilder builder_in,
        WriteLease lease_in)
      : plan(std::move(plan_in)),
        builder(std::move(builder_in)),
        lease(std::move(lease_in)),
        allocation_base(lease->data()),
        allocation_identity(lease->allocation_identity()) {}

  /**
   * @brief Finds one active reservation by exact grant ID.
   * @param grant_id Nonzero issued identifier.
   * @return Iterator into reservations, or end when absent.
   * @throws Nothing.
   * @note Caller holds `mutex`.
   */
  auto find_reservation(std::uint64_t grant_id) noexcept {
    return std::find_if(reservations.begin(), reservations.end(),
                        [grant_id](const Reservation& reservation) {
                          return reservation.grant_id == grant_id;
                        });
  }

  /**
   * @brief Tests one grant identity against open lifecycle state.
   * @param grant_id Candidate issued grant ID.
   * @param expected_generation Generation captured by the grant.
   * @return True when lifecycle, generation, and reservation all match.
   * @throws Nothing.
   * @note Caller holds `mutex`.
   */
  bool grant_active(std::uint64_t grant_id,
                    std::uint64_t expected_generation) noexcept {
    return lifecycle == Lifecycle::Open && generation == expected_generation &&
           find_reservation(grant_id) != reservations.end();
  }

  /**
   * @brief Revokes every grant and records the first sticky failure.
   * @param diagnostic Nonempty owned failure reason.
   * @return Nothing after lifecycle closure.
   * @throws std::bad_alloc when first-diagnostic storage cannot allocate.
   * @note Revocation and lease release happen before diagnostic assignment, so
   * allocation failure cannot leave mutable authority open. Caller holds
   * `mutex`; a sealed publication is never altered.
   */
  void fail(std::string diagnostic) {
    if (lifecycle == Lifecycle::Sealed) {
      return;
    }
    lifecycle = Lifecycle::Failed;
    if (generation != std::numeric_limits<std::uint64_t>::max()) {
      ++generation;
    }
    reservations.clear();
    lease.reset();
    if (!failure.has_value()) {
      failure = std::move(diagnostic);
    }
  }

  /**
   * @brief Best-effort no-throw revocation for destructors and move cleanup.
   * @param diagnostic Stable nonempty failure reason.
   * @throws Nothing.
   * @note Caller holds `mutex`. Diagnostic allocation failure is swallowed
   * only after lifecycle, reservations, generation, and lease are closed.
   */
  void fail_noexcept(const char* diagnostic) noexcept {
    if (lifecycle == Lifecycle::Sealed) {
      return;
    }
    lifecycle = Lifecycle::Failed;
    if (generation != std::numeric_limits<std::uint64_t>::max()) {
      ++generation;
    }
    reservations.clear();
    lease.reset();
    if (!failure.has_value()) {
      try {
        failure.emplace(diagnostic);
      } catch (...) {
      }
    }
  }
};

/** @copydoc HostOutputWriteGrant::HostOutputWriteGrant */
HostOutputWriteGrant::HostOutputWriteGrant(
    std::shared_ptr<State> state, std::uint64_t grant_id,
    std::uint64_t generation, ImageRect image_region,
    std::vector<HostOutputWriteSpan> spans) noexcept
    : state_(std::move(state)),
      grant_id_(grant_id),
      generation_(generation),
      image_region_(image_region),
      spans_(std::move(spans)) {}  // NOLINT(whitespace/indent_namespace)

/** @copydoc HostOutputWriteGrant::HostOutputWriteGrant */
HostOutputWriteGrant::HostOutputWriteGrant(
    HostOutputWriteGrant&& other) noexcept
    : state_(std::move(other.state_)),
      grant_id_(other.grant_id_),
      generation_(other.generation_),
      image_region_(other.image_region_),
      spans_(std::move(other.spans_)),
      retired_(other.retired_) {
  other.grant_id_ = 0U;
  other.generation_ = 0U;
  other.retired_ = true;
}

/** @copydoc HostOutputWriteGrant::operator= */
HostOutputWriteGrant& HostOutputWriteGrant::operator=(
    HostOutputWriteGrant&& other) noexcept {
  if (this != &other) {
    abandon_noexcept();
    state_ = std::move(other.state_);
    grant_id_ = other.grant_id_;
    generation_ = other.generation_;
    image_region_ = other.image_region_;
    spans_ = std::move(other.spans_);
    retired_ = other.retired_;
    other.grant_id_ = 0U;
    other.generation_ = 0U;
    other.retired_ = true;
  }
  return *this;
}

/** @copydoc HostOutputWriteGrant::~HostOutputWriteGrant */
HostOutputWriteGrant::~HostOutputWriteGrant() noexcept {
  abandon_noexcept();
}

/** @copydoc HostOutputWriteGrant::active */
bool HostOutputWriteGrant::active() const {
  if (!state_ || retired_ || grant_id_ == 0U) {
    return false;
  }
  std::lock_guard<std::mutex> lock(state_->mutex);
  return state_->grant_active(grant_id_, generation_);
}

/** @copydoc HostOutputWriteGrant::span */
const HostOutputWriteSpan& HostOutputWriteGrant::span(std::size_t index) const {
  return spans_.at(index);
}

/** @copydoc HostOutputWriteGrant::data */
std::byte* HostOutputWriteGrant::data(std::size_t index) const {
  const HostOutputWriteSpan& selected = spans_.at(index);
  if (!state_ || retired_ || grant_id_ == 0U) {
    throw std::logic_error("Host output grant is not active.");
  }
  std::lock_guard<std::mutex> lock(state_->mutex);
  if (!state_->grant_active(grant_id_, generation_)) {
    throw std::logic_error("Host output grant was revoked.");
  }
  return state_->allocation_base + selected.allocation_offset;
}

/** @copydoc HostOutputWriteGrant::bind_value_descriptor_metadata */
void HostOutputWriteGrant::bind_value_descriptor_metadata(
    DenseImageValueDescriptorMetadata metadata) {
  if (!state_ || retired_ || grant_id_ == 0U) {
    throw std::logic_error(
        "Inactive Host output grant cannot bind descriptor metadata.");
  }
  std::lock_guard<std::mutex> lock(state_->mutex);
  if (!state_->grant_active(grant_id_, generation_)) {
    throw std::logic_error(
        "Revoked Host output grant cannot bind descriptor metadata.");
  }
  try {
    DenseImageValueDescriptorMetadataAccess::attach(&state_->builder,
                                                    std::move(metadata));
  } catch (...) {
    state_->fail("Host output descriptor metadata attachment failed.");
    throw;
  }
}

/** @copydoc HostOutputWriteGrant::retire_success */
void HostOutputWriteGrant::retire_success() {
  if (!state_ || grant_id_ == 0U) {
    throw std::logic_error("Moved-from Host output grant cannot retire.");
  }
  std::lock_guard<std::mutex> lock(state_->mutex);
  if (retired_) {
    state_->fail("Host output grant retirement was attempted twice.");
    throw std::logic_error("Host output grant retired more than once.");
  }
  const auto reservation = state_->find_reservation(grant_id_);
  if (!state_->grant_active(grant_id_, generation_) ||
      reservation == state_->reservations.end()) {
    retired_ = true;
    throw std::logic_error("Revoked Host output grant cannot retire.");
  }
  state_->reservations.erase(reservation);
  retired_ = true;
}

/** @copydoc HostOutputWriteGrant::retire_failure */
void HostOutputWriteGrant::retire_failure(std::string diagnostic) {
  if (!state_ || grant_id_ == 0U) {
    throw std::logic_error("Moved-from Host output grant cannot retire.");
  }
  std::lock_guard<std::mutex> lock(state_->mutex);
  if (retired_) {
    state_->fail("Host output grant retirement was attempted twice.");
    throw std::logic_error("Host output grant retired more than once.");
  }
  if (diagnostic.empty()) {
    state_->fail("Host output grant failure diagnostic was empty.");
    retired_ = true;
    throw std::invalid_argument(
        "Host output grant failure diagnostic must be nonempty.");
  }
  retired_ = true;
  state_->fail(std::move(diagnostic));
}

/** @copydoc HostOutputWriteGrant::abandon_noexcept */
void HostOutputWriteGrant::abandon_noexcept() noexcept {
  if (!state_ || retired_ || grant_id_ == 0U) {
    state_.reset();
    return;
  }
  try {
    std::lock_guard<std::mutex> lock(state_->mutex);
    if (state_->find_reservation(grant_id_) != state_->reservations.end()) {
      state_->fail_noexcept(
          "Host output grant was destroyed without retirement.");
    }
    retired_ = true;
  } catch (...) {
  }
  state_.reset();
  grant_id_ = 0U;
  generation_ = 0U;
}

/** @copydoc HostOutputBinding::HostOutputBinding */
HostOutputBinding::HostOutputBinding(
    std::shared_ptr<HostOutputWriteGrant::State> state) noexcept
    : state_(std::move(state)) {}

/** @copydoc HostOutputBinding::HostOutputBinding */
HostOutputBinding::HostOutputBinding(HostOutputBinding&& other) noexcept =
    default;  // NOLINT(whitespace/indent_namespace)

/** @copydoc HostOutputBinding::operator= */
HostOutputBinding& HostOutputBinding::operator=(
    HostOutputBinding&& other) noexcept {
  if (this != &other) {
    cancel_noexcept();
    state_ = std::move(other.state_);
  }
  return *this;
}

/** @copydoc HostOutputBinding::~HostOutputBinding */
HostOutputBinding::~HostOutputBinding() noexcept {
  cancel_noexcept();
}

/** @copydoc HostOutputBinding::allocate */
HostOutputBinding HostOutputBinding::allocate(DenseImageOutputPlan plan) {
  ValueBuilder builder = ValueBuilder::allocate_cpu_dense_tensor(
      plan.descriptor(), plan.image_facet(), plan.layout(), plan.storage_size(),
      plan.alignment());
  WriteLease lease = builder.acquire_write();
  const std::uintptr_t address = reinterpret_cast<std::uintptr_t>(lease.data());
  if (address % plan.alignment() != 0U) {
    throw std::logic_error(
        "Host allocator did not satisfy the frozen output alignment.");
  }
  return HostOutputBinding(std::make_shared<HostOutputWriteGrant::State>(
      std::move(plan), std::move(builder), std::move(lease)));
}

/** @copydoc HostOutputBinding::plan */
const DenseImageOutputPlan& HostOutputBinding::plan() const {
  if (!state_) {
    throw std::logic_error("Moved-from Host output binding has no plan.");
  }
  return state_->plan;
}

/** @copydoc HostOutputBinding::allocation_identity */
AllocationIdentity HostOutputBinding::allocation_identity() const {
  if (!state_) {
    throw std::logic_error(
        "Moved-from Host output binding has no allocation identity.");
  }
  return state_->allocation_identity;
}

/** @copydoc HostOutputBinding::seed_from_value */
void HostOutputBinding::seed_from_value(const Value& source) {
  const DenseImageOutputPlan& destination_plan = plan();
  if (!source.valid() || !source.image_facet().has_value() ||
      !(source.dense_tensor_descriptor() == destination_plan.descriptor()) ||
      !(*source.image_facet() == destination_plan.image_facet())) {
    throw std::invalid_argument(
        "Host output seed Value must exactly match the frozen image plan.");
  }
  const ImageView view(source);
  HostOutputWriteGrant grant = grant_whole();
  try {
    std::byte* const destination = grant.data(0U);
    for (std::size_t y = 0U; y < destination_plan.height(); ++y) {
      for (std::size_t x = 0U; x < destination_plan.width(); ++x) {
        for (std::size_t channel = 0U; channel < destination_plan.channels();
             ++channel) {
          const std::size_t destination_offset =
              y * destination_plan.row_stride() +
              x * destination_plan.pixel_bytes() +
              channel * destination_plan.element_bytes();
          std::memcpy(destination + destination_offset,
                      view.channel_data(x, y, channel),
                      destination_plan.element_bytes());
        }
      }
    }
    grant.retire_success();
  } catch (...) {
    try {
      if (grant.active()) {
        grant.retire_failure("Host output Value seed failed.");
      }
    } catch (...) {
    }
    throw;
  }
}

/** @copydoc HostOutputBinding::grant_whole */
HostOutputWriteGrant HostOutputBinding::grant_whole() {
  if (!state_) {
    throw std::logic_error("Moved-from Host output binding cannot grant.");
  }
  std::lock_guard<std::mutex> lock(state_->mutex);
  if (state_->lifecycle != HostOutputWriteGrant::State::Lifecycle::Open) {
    throw std::logic_error("Closed Host output binding cannot grant.");
  }
  try {
    if (!state_->reservations.empty()) {
      state_->fail("Whole Host output grant overlaps a live grant.");
      throw std::logic_error("Whole Host output grant overlaps a live grant.");
    }
    if (state_->next_grant_id == std::numeric_limits<std::uint64_t>::max()) {
      state_->fail("Host output grant identity space is exhausted.");
      throw std::overflow_error(
          "Host output grant identity space is exhausted.");
    }
    const std::uint64_t grant_id = state_->next_grant_id++;
    std::vector<HostOutputWriteSpan> spans{
        HostOutputWriteSpan{0U, state_->plan.storage_size()}};
    const ImageRect region = planned_image_rect(state_->plan.image_facet());
    state_->reservations.push_back({grant_id, region, true, spans});
    return HostOutputWriteGrant(state_, grant_id, state_->generation, region,
                                std::move(spans));
  } catch (...) {
    if (state_->lifecycle == HostOutputWriteGrant::State::Lifecycle::Open) {
      state_->fail("Whole Host output grant issuance failed.");
    }
    throw;
  }
}

/** @copydoc HostOutputBinding::grant_tile */
HostOutputWriteGrant HostOutputBinding::grant_tile(
    ImageRect region, std::size_t required_alignment) {
  if (!state_) {
    throw std::logic_error("Moved-from Host output binding cannot grant.");
  }
  std::lock_guard<std::mutex> lock(state_->mutex);
  if (state_->lifecycle != HostOutputWriteGrant::State::Lifecycle::Open) {
    throw std::logic_error("Closed Host output binding cannot grant.");
  }
  try {
    const ImageBounds& bounds = state_->plan.image_facet().data_window;
    if (!(region.domain == image_region_domain()) ||
        region.x_begin >= region.x_end || region.y_begin >= region.y_end ||
        region.x_begin < bounds.x_begin || region.x_end > bounds.x_end ||
        region.y_begin < bounds.y_begin || region.y_end > bounds.y_end) {
      state_->fail("Host output tile lies outside the planned image Region.");
      throw std::invalid_argument(
          "Host output tile must be nonempty and inside the planned Region.");
    }
    if (!is_power_of_two(required_alignment) ||
        required_alignment > state_->plan.alignment()) {
      state_->fail("Host output tile alignment is invalid for the plan.");
      throw std::invalid_argument(
          "Host output tile alignment must be a supported power of two.");
    }

    const std::size_t x_offset =
        checked_coordinate_distance(region.x_begin, bounds.x_begin);
    const std::size_t y_offset =
        checked_coordinate_distance(region.y_begin, bounds.y_begin);
    const std::size_t tile_width =
        checked_coordinate_distance(region.x_end, region.x_begin);
    const std::size_t tile_height =
        checked_coordinate_distance(region.y_end, region.y_begin);
    if (tile_height > kMaximumHostOutputGrantSpans) {
      state_->fail("Host output tile exceeds the frozen row-span bound.");
      throw std::length_error(
          "Host output tile exceeds the frozen row-span bound.");
    }
    const std::size_t span_size =
        checked_output_multiply(tile_width, state_->plan.pixel_bytes());
    const std::size_t x_byte_offset =
        checked_output_multiply(x_offset, state_->plan.pixel_bytes());
    const std::size_t first_row_offset = checked_output_add(
        checked_output_multiply(y_offset, state_->plan.row_stride()),
        x_byte_offset);

    std::vector<HostOutputWriteSpan> spans;
    spans.reserve(tile_height);
    const std::uintptr_t base_address =
        reinterpret_cast<std::uintptr_t>(state_->allocation_base);
    for (std::size_t row = 0U; row < tile_height; ++row) {
      const std::size_t row_offset = checked_output_add(
          first_row_offset,
          checked_output_multiply(row, state_->plan.row_stride()));
      const std::size_t row_end = checked_output_add(row_offset, span_size);
      if (row_end > state_->plan.storage_size()) {
        state_->fail("Host output tile byte span exceeds its allocation.");
        throw std::out_of_range(
            "Host output tile byte span exceeds its allocation.");
      }
      if (row_offset >
              std::numeric_limits<std::uintptr_t>::max() - base_address ||
          (base_address + row_offset) % required_alignment != 0U) {
        state_->fail("Host output tile row address violates alignment.");
        throw std::invalid_argument(
            "Host output tile row address violates alignment.");
      }
      spans.push_back({row_offset, span_size});
    }

    for (const auto& live : state_->reservations) {
      if (live.whole || image_rectangles_overlap(region, live.image_region) ||
          output_spans_overlap(spans, live.spans)) {
        state_->fail("Host output tile overlaps a live grant.");
        throw std::logic_error("Host output tile overlaps a live grant.");
      }
    }
    if (state_->next_grant_id == std::numeric_limits<std::uint64_t>::max()) {
      state_->fail("Host output grant identity space is exhausted.");
      throw std::overflow_error(
          "Host output grant identity space is exhausted.");
    }
    const std::uint64_t grant_id = state_->next_grant_id++;
    state_->reservations.push_back({grant_id, region, false, spans});
    return HostOutputWriteGrant(state_, grant_id, state_->generation, region,
                                std::move(spans));
  } catch (...) {
    if (state_->lifecycle == HostOutputWriteGrant::State::Lifecycle::Open) {
      state_->fail("Host output tile grant issuance failed.");
    }
    throw;
  }
}

/** @copydoc HostOutputBinding::cancel */
void HostOutputBinding::cancel(std::string diagnostic) {
  if (!state_) {
    throw std::logic_error("Moved-from Host output binding cannot cancel.");
  }
  if (diagnostic.empty()) {
    throw std::invalid_argument(
        "Host output cancellation diagnostic must be nonempty.");
  }
  std::lock_guard<std::mutex> lock(state_->mutex);
  if (state_->lifecycle == HostOutputWriteGrant::State::Lifecycle::Sealed) {
    throw std::logic_error("Sealed Host output binding cannot cancel.");
  }
  state_->fail(std::move(diagnostic));
}

/** @copydoc HostOutputBinding::seal */
Value HostOutputBinding::seal() {
  if (!state_) {
    throw std::logic_error("Moved-from Host output binding cannot seal.");
  }
  std::lock_guard<std::mutex> lock(state_->mutex);
  if (state_->lifecycle == HostOutputWriteGrant::State::Lifecycle::Sealed) {
    throw std::logic_error("Host output binding has already published.");
  }
  if (state_->lifecycle != HostOutputWriteGrant::State::Lifecycle::Open) {
    throw std::logic_error("Failed Host output binding cannot publish.");
  }
  if (!state_->reservations.empty()) {
    state_->fail("Host output seal observed active grants.");
    throw std::logic_error(
        "Host output binding cannot seal with active grants.");
  }

  state_->lifecycle = HostOutputWriteGrant::State::Lifecycle::Sealing;
  if (state_->generation != std::numeric_limits<std::uint64_t>::max()) {
    ++state_->generation;
  }
  state_->lease.reset();
  try {
    Value published = state_->builder.seal();
    state_->published_value = published;
    state_->lifecycle = HostOutputWriteGrant::State::Lifecycle::Sealed;
    return published;
  } catch (...) {
    state_->fail("Host output Value publication failed.");
    throw;
  }
}

/** @copydoc HostOutputBinding::failure */
std::optional<std::string> HostOutputBinding::failure() const {
  if (!state_) {
    throw std::logic_error(
        "Moved-from Host output binding has no failure state.");
  }
  std::lock_guard<std::mutex> lock(state_->mutex);
  return state_->failure;
}

/** @copydoc HostOutputBinding::cancel_noexcept */
void HostOutputBinding::cancel_noexcept() noexcept {
  if (!state_) {
    return;
  }
  try {
    std::lock_guard<std::mutex> lock(state_->mutex);
    if (state_->lifecycle == HostOutputWriteGrant::State::Lifecycle::Open ||
        state_->lifecycle == HostOutputWriteGrant::State::Lifecycle::Sealing) {
      state_->fail_noexcept(
          "Host output binding was destroyed before publication.");
    }
  } catch (...) {
  }
  state_.reset();
}

}  // namespace ps
