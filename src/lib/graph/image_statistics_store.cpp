#include "graph/image_statistics_store.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <type_traits>
#include <utility>
#include <vector>

#include "photospider/data/image_view.hpp"

namespace ps {

/**
 * @brief Request-local cancellation/publication linearization state.
 * @throws Nothing for destruction after successful mutex construction.
 * @note The mutex is always acquired before the store mutex, so cancellation
 * can determine whether it happened before or after derived publication.
 */
struct ImageStatisticsCancellationState final {
  /** @brief Serializes cancellation with the publication critical section. */
  mutable std::mutex mutex;

  /** @brief True after cancellation linearizes under `mutex`. */
  bool cancelled = false;
};

/**
 * @brief Cache state shared with accepted tasks independently of facade life.
 * @throws std::bad_alloc when retained result storage grows.
 * @note Entries remain oldest-to-newest and are compared by complete key.
 */
struct ImageStatisticsStoreState final {
  /**
   * @brief Creates empty state with one immutable positive bound.
   * @param maximum Positive retained-entry limit.
   * @throws Nothing after caller validation.
   */
  explicit ImageStatisticsStoreState(std::size_t maximum) noexcept
      : maximum_entries(maximum) {}

  /** @brief Immutable retained-entry bound. */
  const std::size_t maximum_entries;

  /** @brief Protects every entry lookup and mutation. */
  mutable std::mutex mutex;

  /** @brief Oldest-to-newest complete validated derived results. */
  std::vector<ImageStatisticsResult> entries;
};

namespace {

/**
 * @brief Mutable observed accumulator for one selected stable channel.
 * @throws std::bad_alloc when histogram storage cannot allocate.
 * @note Exceptional floating values never participate in finite extrema or
 * histogram counts.
 */
struct ChannelAccumulator final {
  /** @brief Stable channel identity echoed in the result. */
  ChannelId id;

  /** @brief Physical ImageView channel coordinate. */
  std::size_t channel_index = 0U;

  /** @brief Number of finite observed samples. */
  std::uint64_t finite_count = 0U;

  /** @brief Number of observed NaN samples. */
  std::uint64_t nan_count = 0U;

  /** @brief Number of observed positive infinity samples. */
  std::uint64_t positive_infinity_count = 0U;

  /** @brief Number of observed negative infinity samples. */
  std::uint64_t negative_infinity_count = 0U;

  /** @brief Finite minimum when at least one finite sample exists. */
  std::optional<double> minimum;

  /** @brief Finite maximum when at least one finite sample exists. */
  std::optional<double> maximum;

  /** @brief Fixed histogram bins for Histogram queries. */
  std::optional<std::vector<std::uint64_t>> histogram_bins;

  /** @brief Finite samples below the requested histogram interval. */
  std::uint64_t below_histogram_count = 0U;

  /** @brief Finite samples at or above the requested histogram interval. */
  std::uint64_t above_histogram_count = 0U;
};

/**
 * @brief Exact logical image rectangle selected for one scan.
 * @throws Nothing for aggregate construction and destruction.
 */
struct StatisticsScanRect final {
  /** @brief Inclusive signed x endpoint. */
  std::int64_t x_begin = 0;
  /** @brief Exclusive signed x endpoint. */
  std::int64_t x_end = 0;
  /** @brief Inclusive signed y endpoint. */
  std::int64_t y_begin = 0;
  /** @brief Exclusive signed y endpoint. */
  std::int64_t y_end = 0;
};

/**
 * @brief Throws the typed cancellation terminal after synchronized inspection.
 * @param cancellation Matching request-local state.
 * @return Nothing when cancellation has not linearized.
 * @throws ImageStatisticsCancelled when cancellation already won.
 * @throws std::system_error when request-state synchronization fails.
 */
void throw_if_statistics_cancelled(
    const std::shared_ptr<ImageStatisticsCancellationState>& cancellation) {
  std::lock_guard<std::mutex> lock(cancellation->mutex);
  if (cancellation->cancelled) {
    throw ImageStatisticsCancelled();
  }
}

/**
 * @brief Adds one observed count without unsigned wraparound.
 * @param value Mutable accumulated count.
 * @param increment Positive count to add.
 * @return Nothing.
 * @throws std::overflow_error when the exact count is unrepresentable.
 */
void increment_count(std::uint64_t* value, std::uint64_t increment = 1U) {
  if (*value > std::numeric_limits<std::uint64_t>::max() - increment) {
    throw std::overflow_error("Image statistics sample count overflowed.");
  }
  *value += increment;
}

/**
 * @brief Reads one potentially unaligned whole-byte scalar.
 * @tparam Scalar C++ scalar matching immutable Value element facts.
 * @param bytes Address of at least sizeof(Scalar) readable bytes.
 * @return Bit-preserving scalar copy.
 * @throws Nothing.
 */
template <typename Scalar>
Scalar read_statistics_scalar(const std::byte* bytes) noexcept {
  Scalar value{};
  std::memcpy(&value, bytes, sizeof(value));
  return value;
}

/**
 * @brief Maps one contained finite sample to a stable unit-interval position.
 * @param value Finite sample in `[minimum, maximum)`.
 * @param minimum Finite inclusive histogram lower bound.
 * @param maximum Finite exclusive histogram upper bound greater than minimum.
 * @return Finite position clamped to `[0,1]` for defensive rounding edges.
 * @throws std::overflow_error when validated finite inputs cannot produce a
 * finite positive normalization span or result.
 * @note Same-sign bounds use their exact finite difference so adjacent large
 * doubles remain distinguishable. Bounds that cross zero are first scaled by
 * their largest magnitude; subtraction then occurs inside `[-1,1]` and cannot
 * overflow even for `[-DBL_MAX, DBL_MAX]`.
 */
double normalized_histogram_position(double value, double minimum,
                                     double maximum) {
  double normalized = 0.0;
  if (minimum < 0.0 && maximum > 0.0) {
    const double scale = std::max(-minimum, maximum);
    const double scaled_minimum = minimum / scale;
    const double scaled_maximum = maximum / scale;
    const double scaled_value = value / scale;
    const double scaled_range = scaled_maximum - scaled_minimum;
    if (!(scaled_range > 0.0) || !std::isfinite(scaled_range)) {
      throw std::overflow_error(
          "Image statistics histogram scaling lost its finite range.");
    }
    normalized = (scaled_value - scaled_minimum) / scaled_range;
  } else {
    const double range = maximum - minimum;
    if (!(range > 0.0) || !std::isfinite(range)) {
      throw std::overflow_error(
          "Image statistics histogram bounds lost their finite range.");
    }
    normalized = (value - minimum) / range;
  }
  if (!std::isfinite(normalized)) {
    throw std::overflow_error(
        "Image statistics histogram position is not finite.");
  }
  return std::clamp(normalized, 0.0, 1.0);
}

/**
 * @brief Adds one converted scalar to a selected-channel accumulator.
 * @param value Scalar converted to binary64 for bounded observation.
 * @param floating_point Whether NaN/infinity classification applies.
 * @param histogram Optional validated Histogram parameters.
 * @param accumulator Mutable selected-channel result state.
 * @return Nothing.
 * @throws std::overflow_error when any observed count is unrepresentable.
 * @note Finite integer inputs in the implemented 8/16-bit domain convert
 * exactly. Histogram upper bounds are exclusive. Every contained finite value
 * is normalized without unbounded subtraction and checked before conversion
 * to a bin index; a rounding-edge index is then clamped to the final bin.
 */
void observe_statistics_scalar(
    double value, bool floating_point,
    const std::optional<ImageHistogramParameters>& histogram,
    ChannelAccumulator* accumulator) {
  if (floating_point && std::isnan(value)) {
    increment_count(&accumulator->nan_count);
    return;
  }
  if (floating_point && std::isinf(value)) {
    if (value > 0.0) {
      increment_count(&accumulator->positive_infinity_count);
    } else {
      increment_count(&accumulator->negative_infinity_count);
    }
    return;
  }

  increment_count(&accumulator->finite_count);
  accumulator->minimum = accumulator->minimum.has_value()
                             ? std::min(*accumulator->minimum, value)
                             : value;
  accumulator->maximum = accumulator->maximum.has_value()
                             ? std::max(*accumulator->maximum, value)
                             : value;
  if (!histogram.has_value()) {
    return;
  }
  if (value < histogram->minimum) {
    increment_count(&accumulator->below_histogram_count);
    return;
  }
  if (value >= histogram->maximum) {
    increment_count(&accumulator->above_histogram_count);
    return;
  }
  const double normalized = normalized_histogram_position(
      value, histogram->minimum, histogram->maximum);
  std::size_t bin = static_cast<std::size_t>(
      normalized * static_cast<double>(histogram->bin_count));
  if (bin >= histogram->bin_count) {
    bin = histogram->bin_count - 1U;
  }
  increment_count(&(*accumulator->histogram_bins)[bin]);
}

/**
 * @brief Resolves the exact scan rectangle from a canonical query Region.
 * @param query Validated statistics query.
 * @param image Complete source ImageFacet.
 * @return Whole data window or the one exact bounded image-domain rectangle.
 * @throws std::invalid_argument when the Region requires an unsupported domain,
 * atom kind, mapping, or exceeds the immutable data window.
 * @note Statistics scheduling never clips or widens requested work because the
 * complete normalized Region participates in cache identity.
 */
StatisticsScanRect resolve_statistics_scan_rect(
    const ImageStatisticsQuery& query, const ImageFacet& image) {
  const ImageBounds& bounds = image.data_window;
  if (query.region.is_whole()) {
    return {bounds.x_begin, bounds.x_end, bounds.y_begin, bounds.y_end};
  }
  if (query.region.atoms().size() != 1U) {
    throw std::invalid_argument(
        "Image statistics scan requires Whole or one image-domain Region.");
  }
  const auto* rect = std::get_if<ImageRect>(&query.region.atoms().front());
  if (rect == nullptr || !(rect->domain == image_region_domain())) {
    throw std::invalid_argument(
        "Image statistics scan cannot map the requested Region domain.");
  }
  if (rect->x_begin < bounds.x_begin || rect->x_end > bounds.x_end ||
      rect->y_begin < bounds.y_begin || rect->y_end > bounds.y_end) {
    throw std::invalid_argument(
        "Image statistics Region exceeds the immutable data window.");
  }
  return {rect->x_begin, rect->x_end, rect->y_begin, rect->y_end};
}

/**
 * @brief Finds one stable channel's physical image-axis coordinate.
 * @param schema Valid source channel schema.
 * @param channel Valid stable channel identity.
 * @return Exact zero-based channel-axis coordinate.
 * @throws std::invalid_argument when the channel is absent.
 */
std::size_t find_channel_index(const ChannelSchema& schema, ChannelId channel) {
  const auto found =
      std::find_if(schema.channels.begin(), schema.channels.end(),
                   [channel](const ChannelDescription& candidate) {
                     return candidate.id == channel;
                   });
  if (found == schema.channels.end()) {
    throw std::invalid_argument(
        "Image statistics selector is absent from the channel schema.");
  }
  return static_cast<std::size_t>(found - schema.channels.begin());
}

/**
 * @brief Resolves the query selector into stable-ID-ordered scan accumulators.
 * @param query Validated channel or group selection.
 * @param view Source image retaining complete metadata and payload.
 * @return One accumulator per selected stable channel.
 * @throws std::invalid_argument when schema, channel, group, or membership is
 * absent or inconsistent.
 * @throws std::bad_alloc when accumulator or histogram storage cannot allocate.
 * @note Physical schema order selects addresses; result order follows stable
 * ChannelId as required by the bounded result contract.
 */
std::vector<ChannelAccumulator> make_channel_accumulators(
    const ImageStatisticsQuery& query, const ImageView& view) {
  const std::optional<ChannelSchema>& schema =
      view.image_facet().channel_schema;
  if (!schema.has_value()) {
    throw std::invalid_argument(
        "Image statistics requires an explicit stable channel schema.");
  }

  std::vector<ChannelId> selected;
  if (query.selection.channel.has_value()) {
    selected.push_back(*query.selection.channel);
  } else {
    const ChannelGroupId group_id = *query.selection.group;
    const auto group =
        std::find_if(schema->groups.begin(), schema->groups.end(),
                     [group_id](const ChannelGroupDescription& candidate) {
                       return candidate.id == group_id;
                     });
    if (group == schema->groups.end()) {
      throw std::invalid_argument(
          "Image statistics group is absent from the channel schema.");
    }
    selected = group->members;
  }
  std::sort(selected.begin(), selected.end());

  std::vector<ChannelAccumulator> accumulators;
  accumulators.reserve(selected.size());
  for (const ChannelId channel : selected) {
    ChannelAccumulator accumulator;
    accumulator.id = channel;
    accumulator.channel_index = find_channel_index(*schema, channel);
    if (query.histogram.has_value()) {
      accumulator.histogram_bins =
          std::vector<std::uint64_t>(query.histogram->bin_count, 0U);
    }
    accumulators.push_back(std::move(accumulator));
  }
  return accumulators;
}

/**
 * @brief Scans one typed selected-channel set through checked logical
 * addresses.
 * @tparam Scalar C++ scalar matching source element facts.
 * @param view Valid retaining ImageView.
 * @param rect Exact contained logical image rectangle.
 * @param query Complete validated query.
 * @param cancellation Request-local cancellation state polled before each row.
 * @param accumulators Mutable stable-ID-ordered result accumulators.
 * @return Nothing.
 * @throws ImageStatisticsCancelled, overflow, bounds, allocation, or mutex
 * exceptions from cancellation and checked ImageView access.
 * @note No padding, compatibility projection, or unselected channel is read.
 */
template <typename Scalar>
void scan_typed_statistics(
    const ImageView& view, const StatisticsScanRect& rect,
    const ImageStatisticsQuery& query,
    const std::shared_ptr<ImageStatisticsCancellationState>& cancellation,
    std::vector<ChannelAccumulator>* accumulators) {
  for (std::int64_t y = rect.y_begin; y < rect.y_end; ++y) {
    throw_if_statistics_cancelled(cancellation);
    for (std::int64_t x = rect.x_begin; x < rect.x_end; ++x) {
      for (ChannelAccumulator& accumulator : *accumulators) {
        const Scalar scalar = read_statistics_scalar<Scalar>(
            view.channel_data_at(x, y, accumulator.channel_index));
        observe_statistics_scalar(static_cast<double>(scalar),
                                  std::is_floating_point_v<Scalar>,
                                  query.histogram, &accumulator);
      }
    }
  }
}

/**
 * @brief Dispatches a validated source scan by immutable element facts.
 * @param view Valid retaining ImageView.
 * @param rect Exact contained logical image rectangle.
 * @param query Complete validated version-1 request.
 * @param cancellation Request-local cancellation state.
 * @param accumulators Mutable result accumulators.
 * @return Nothing.
 * @throws std::invalid_argument for quantized or unsupported element facts.
 * @throws Scan, cancellation, overflow, allocation, or synchronization
 * exceptions unchanged.
 */
void scan_statistics_pixels(
    const ImageView& view, const StatisticsScanRect& rect,
    const ImageStatisticsQuery& query,
    const std::shared_ptr<ImageStatisticsCancellationState>& cancellation,
    std::vector<ChannelAccumulator>* accumulators) {
  const DenseTensorDescriptor& descriptor = view.descriptor();
  if (descriptor.quantization.has_value()) {
    throw std::invalid_argument(
        "Image statistics does not infer dequantization semantics.");
  }
  const std::uint32_t bits = descriptor.storage_encoding.bit_width;
  if (descriptor.element_semantics == ElementSemantics::UnsignedInteger &&
      bits == 8U) {
    scan_typed_statistics<std::uint8_t>(view, rect, query, cancellation,
                                        accumulators);
  } else if (descriptor.element_semantics == ElementSemantics::SignedInteger &&
             bits == 8U) {
    scan_typed_statistics<std::int8_t>(view, rect, query, cancellation,
                                       accumulators);
  } else if (descriptor.element_semantics ==
                 ElementSemantics::UnsignedInteger &&
             bits == 16U) {
    scan_typed_statistics<std::uint16_t>(view, rect, query, cancellation,
                                         accumulators);
  } else if (descriptor.element_semantics == ElementSemantics::SignedInteger &&
             bits == 16U) {
    scan_typed_statistics<std::int16_t>(view, rect, query, cancellation,
                                        accumulators);
  } else if (descriptor.element_semantics == ElementSemantics::FloatingPoint &&
             bits == 32U) {
    scan_typed_statistics<float>(view, rect, query, cancellation, accumulators);
  } else if (descriptor.element_semantics == ElementSemantics::FloatingPoint &&
             bits == 64U) {
    scan_typed_statistics<double>(view, rect, query, cancellation,
                                  accumulators);
  } else {
    throw std::invalid_argument(
        "Image statistics does not support this element encoding.");
  }
}

/**
 * @brief Converts completed accumulators into one validated bounded result.
 * @param key Complete Value-backed statistics cache key.
 * @param accumulators Completed stable-ID-ordered scan state.
 * @return Complete result accepted by validate_image_statistics_result().
 * @throws std::bad_alloc when result ownership cannot allocate.
 * @throws Validation exceptions when internal scan state is inconsistent.
 */
ImageStatisticsResult finish_statistics_result(
    ImageStatisticsCacheKey key, std::vector<ChannelAccumulator> accumulators) {
  ImageStatisticsResult result;
  result.key = std::move(key);
  result.channels.reserve(accumulators.size());
  for (ChannelAccumulator& accumulator : accumulators) {
    ImageChannelStatistics channel;
    channel.channel = accumulator.id;
    channel.finite_sample_count = accumulator.finite_count;
    channel.nan_count = accumulator.nan_count;
    channel.positive_infinity_count = accumulator.positive_infinity_count;
    channel.negative_infinity_count = accumulator.negative_infinity_count;
    channel.minimum = accumulator.minimum;
    channel.maximum = accumulator.maximum;
    channel.histogram_bins = std::move(accumulator.histogram_bins);
    channel.below_histogram_count = accumulator.below_histogram_count;
    channel.above_histogram_count = accumulator.above_histogram_count;
    result.channels.push_back(std::move(channel));
  }
  validate_image_statistics_result(result);
  return result;
}

/**
 * @brief Performs one complete Value-backed statistics scan.
 * @param value Exact Ready immutable source retained by scheduled work.
 * @param key Complete validated identity derived from `value` and the query.
 * @param cancellation Matching request-local cancellation state.
 * @return Complete validated result carrying the exact key.
 * @throws ImageView, query support, cancellation, overflow, validation, or
 * allocation exceptions unchanged.
 * @note The function reads no alternate image representation, graph
 * generation, HP/RT version,
 * allocation identity, or descriptor digest.
 */
ImageStatisticsResult scan_image_statistics(
    Value value, ImageStatisticsCacheKey key,
    const std::shared_ptr<ImageStatisticsCancellationState>& cancellation) {
  throw_if_statistics_cancelled(cancellation);
  if (key.query.algorithm_version != 1U) {
    throw std::invalid_argument(
        "Image statistics scan supports algorithm version one only.");
  }
  ImageView view(std::move(value));
  const StatisticsScanRect rect =
      resolve_statistics_scan_rect(key.query, view.image_facet());
  std::vector<ChannelAccumulator> accumulators =
      make_channel_accumulators(key.query, view);
  scan_statistics_pixels(view, rect, key.query, cancellation, &accumulators);
  throw_if_statistics_cancelled(cancellation);
  return finish_statistics_result(std::move(key), std::move(accumulators));
}

/**
 * @brief Finds one complete key in oldest-to-newest cache order.
 * @param entries Locked store entries.
 * @param key Complete validated key.
 * @return Iterator to the exact key or `entries.end()`.
 * @throws Nothing under complete key equality.
 */
auto find_statistics_entry(std::vector<ImageStatisticsResult>& entries,
                           const ImageStatisticsCacheKey& key) noexcept {
  return std::find_if(entries.begin(), entries.end(),
                      [&key](const ImageStatisticsResult& result) {
                        return result.key == key;
                      });
}

}  // namespace

/** @copydoc ImageStatisticsCancelled::ImageStatisticsCancelled */
ImageStatisticsCancelled::ImageStatisticsCancelled()
    : std::runtime_error("Image statistics request was cancelled.") {}

/** @copydoc ScheduledImageStatistics::ScheduledImageStatistics */
ScheduledImageStatistics::ScheduledImageStatistics(
    std::future<ImageStatisticsResult> completion,
    std::shared_ptr<ImageStatisticsCancellationState> cancellation) noexcept
    : completion_(std::move(completion)),
      cancellation_(
          std::move(cancellation)) {  // NOLINT(whitespace/indent_namespace)
}

/** @copydoc ScheduledImageStatistics::cancel */
void ScheduledImageStatistics::cancel() {
  std::lock_guard<std::mutex> lock(cancellation_->mutex);
  cancellation_->cancelled = true;
}

/** @copydoc ScheduledImageStatistics::cancelled */
bool ScheduledImageStatistics::cancelled() const {
  std::lock_guard<std::mutex> lock(cancellation_->mutex);
  return cancellation_->cancelled;
}

/** @copydoc ScheduledImageStatistics::take_completion */
std::future<ImageStatisticsResult> ScheduledImageStatistics::take_completion() {
  if (!completion_.valid()) {
    throw std::future_error(std::future_errc::no_state);
  }
  return std::move(completion_);
}

/** @copydoc ImageStatisticsStore::ImageStatisticsStore */
ImageStatisticsStore::ImageStatisticsStore(std::size_t maximum_entries) {
  if (maximum_entries == 0U ||
      maximum_entries > kMaximumImageStatisticsCacheEntries) {
    throw std::invalid_argument(
        "Image statistics cache entry bound is invalid.");
  }
  state_ = std::make_shared<ImageStatisticsStoreState>(maximum_entries);
}

/** @copydoc ImageStatisticsStore::schedule */
ScheduledImageStatistics ImageStatisticsStore::schedule(
    Value value, std::optional<ContentDigest> content_digest,
    ImageStatisticsQuery query, const Scheduler& scheduler) const {
  if (!value.valid() ||
      value.representation_kind() != ValueRepresentationKind::DenseTensor ||
      !value.image_facet().has_value()) {
    throw std::invalid_argument(
        "Image statistics scheduling requires a valid image Value.");
  }
  ReadyFenceSnapshot readiness = value.ready_fence().poll();
  if (!readiness.ready()) {
    throw ReadyFenceAccessError(std::move(readiness));
  }
  if (!scheduler) {
    throw std::invalid_argument(
        "Image statistics scheduling requires a task receiver.");
  }
  ImageStatisticsCacheKey key{value.revision_id(), std::move(content_digest),
                              std::move(query)};
  validate_image_statistics_cache_key(key);

  auto cancellation = std::make_shared<ImageStatisticsCancellationState>();
  auto promise = std::make_shared<std::promise<ImageStatisticsResult>>();
  std::future<ImageStatisticsResult> completion = promise->get_future();
  if (std::optional<ImageStatisticsResult> cached = lookup(key)) {
    promise->set_value(std::move(*cached));
    return ScheduledImageStatistics(std::move(completion),
                                    std::move(cancellation));
  }

  const std::shared_ptr<ImageStatisticsStoreState> state = state_;
  Task task = [value = std::move(value), key = std::move(key), cancellation,
               promise, state]() mutable {
    try {
      ImageStatisticsResult scanned =
          scan_image_statistics(std::move(value), std::move(key), cancellation);
      ImageStatisticsResult published = ImageStatisticsStore::publish(
          std::move(scanned), state, cancellation);
      promise->set_value(std::move(published));
    } catch (...) {
      try {
        promise->set_exception(std::current_exception());
      } catch (...) {
        std::terminate();
      }
    }
  };
  scheduler(std::move(task));
  return ScheduledImageStatistics(std::move(completion),
                                  std::move(cancellation));
}

/** @copydoc ImageStatisticsStore::lookup */
std::optional<ImageStatisticsResult> ImageStatisticsStore::lookup(
    const ImageStatisticsCacheKey& key) const {
  validate_image_statistics_cache_key(key);
  std::lock_guard<std::mutex> lock(state_->mutex);
  auto found = find_statistics_entry(state_->entries, key);
  if (found == state_->entries.end()) {
    return std::nullopt;
  }
  return *found;
}

/** @copydoc ImageStatisticsStore::invalidate_revision */
std::size_t ImageStatisticsStore::invalidate_revision(
    ValueRevisionId revision) const {
  if (!revision.valid()) {
    throw std::invalid_argument(
        "Image statistics invalidation requires a valid revision.");
  }
  std::lock_guard<std::mutex> lock(state_->mutex);
  const std::size_t previous = state_->entries.size();
  state_->entries.erase(
      std::remove_if(state_->entries.begin(), state_->entries.end(),
                     [revision](const ImageStatisticsResult& result) {
                       return result.key.revision == revision;
                     }),
      state_->entries.end());
  return previous - state_->entries.size();
}

/** @copydoc ImageStatisticsStore::clear */
std::size_t ImageStatisticsStore::clear() const {
  std::lock_guard<std::mutex> lock(state_->mutex);
  const std::size_t removed = state_->entries.size();
  state_->entries.clear();
  return removed;
}

/** @copydoc ImageStatisticsStore::size */
std::size_t ImageStatisticsStore::size() const {
  std::lock_guard<std::mutex> lock(state_->mutex);
  return state_->entries.size();
}

/** @copydoc ImageStatisticsStore::maximum_entries */
std::size_t ImageStatisticsStore::maximum_entries() const noexcept {
  return state_->maximum_entries;
}

/** @copydoc ImageStatisticsStore::publish */
ImageStatisticsResult ImageStatisticsStore::publish(
    ImageStatisticsResult result,
    const std::shared_ptr<ImageStatisticsStoreState>& state,
    const std::shared_ptr<ImageStatisticsCancellationState>&
        cancellation) {  // NOLINT(whitespace/indent_namespace)
  validate_image_statistics_result(result);
  std::lock_guard<std::mutex> cancellation_lock(cancellation->mutex);
  if (cancellation->cancelled) {
    throw ImageStatisticsCancelled();
  }
  std::lock_guard<std::mutex> cache_lock(state->mutex);
  auto existing = find_statistics_entry(state->entries, result.key);
  if (existing != state->entries.end()) {
    return *existing;
  }

  ImageStatisticsResult completion_result = result;
  state->entries.push_back(std::move(result));
  if (state->entries.size() > state->maximum_entries) {
    state->entries.erase(state->entries.begin());
  }
  return completion_result;
}

}  // namespace ps
