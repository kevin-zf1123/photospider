#include "runtime/graph_event_service.hpp"

#include <algorithm>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

namespace ps {
namespace {

/**
 * @brief Returns the byte width of one canonical UTF-8 scalar.
 *
 * @param value Complete candidate byte sequence.
 * @param offset Byte offset of the candidate leading byte.
 * @return Scalar width from one through four, or zero for invalid input.
 * @throws Nothing.
 * @note The check rejects stray continuations, truncation, overlong encodings,
 *       surrogate code points, and values above U+10FFFF before event text is
 *       copied into retained ring storage.
 */
std::size_t utf8_scalar_bytes(std::string_view value,
                              std::size_t offset) noexcept {
  if (offset >= value.size()) {
    return 0;
  }
  const auto byte = [&value](std::size_t index) {
    return static_cast<unsigned char>(value[index]);
  };
  const unsigned char first = byte(offset);
  if (first <= 0x7fU) {
    return 1;
  }
  if (first >= 0xc2U && first <= 0xdfU) {
    return offset + 1 < value.size() && byte(offset + 1) >= 0x80U &&
                   byte(offset + 1) <= 0xbfU
               ? 2U
               : 0U;
  }
  if (first >= 0xe0U && first <= 0xefU) {
    if (offset + 2 >= value.size()) {
      return 0;
    }
    const unsigned char second = byte(offset + 1);
    const unsigned char third = byte(offset + 2);
    const bool second_valid =
        first == 0xe0U   ? second >= 0xa0U && second <= 0xbfU
        : first == 0xedU ? second >= 0x80U && second <= 0x9fU
                         : second >= 0x80U && second <= 0xbfU;
    return second_valid && third >= 0x80U && third <= 0xbfU ? 3U : 0U;
  }
  if (first >= 0xf0U && first <= 0xf4U) {
    if (offset + 3 >= value.size()) {
      return 0;
    }
    const unsigned char second = byte(offset + 1);
    const unsigned char third = byte(offset + 2);
    const unsigned char fourth = byte(offset + 3);
    const bool second_valid =
        first == 0xf0U   ? second >= 0x90U && second <= 0xbfU
        : first == 0xf4U ? second >= 0x80U && second <= 0x8fU
                         : second >= 0x80U && second <= 0xbfU;
    return second_valid && third >= 0x80U && third <= 0xbfU &&
                   fourth >= 0x80U && fourth <= 0xbfU
               ? 4U
               : 0U;
  }
  return 0;
}

/**
 * @brief Validates one complete canonical UTF-8 byte sequence.
 *
 * @param value Candidate event name or source bytes.
 * @return True only when every byte belongs to a valid Unicode scalar.
 * @throws Nothing.
 * @note Embedded NUL is a valid scalar for observation text; this boundary
 *       enforces encoding and byte length, not path semantics.
 */
bool valid_utf8(std::string_view value) noexcept {
  std::size_t offset = 0;
  while (offset < value.size()) {
    const std::size_t scalar_bytes = utf8_scalar_bytes(value, offset);
    if (scalar_bytes == 0) {
      return false;
    }
    offset += scalar_bytes;
  }
  return true;
}

/**
 * @brief Increments an unsigned observation counter without wrapping.
 *
 * @param value Counter to update.
 * @return Nothing.
 * @throws Nothing.
 */
void saturating_increment(uint64_t& value) noexcept {
  if (value != kObservationSequenceExhausted) {
    ++value;
  }
}

/**
 * @brief Advances a valid publication sequence to its successor or sentinel.
 *
 * @param sequence Valid publication sequence.
 * @return The next sequence, using `kObservationSequenceExhausted` after the
 *         final valid value.
 * @throws Nothing.
 */
uint64_t sequence_successor(uint64_t sequence) noexcept {
  if (sequence >= kObservationSequenceExhausted - 1) {
    return kObservationSequenceExhausted;
  }
  return sequence + 1;
}

}  // namespace

/** @copydoc GraphEventService::GraphEventService */
GraphEventService::GraphEventService(std::size_t capacity,
                                     uint64_t initial_sequence,
                                     uint64_t initial_dropped_count)
    : slots_(capacity),
      next_sequence_(initial_sequence),
      dropped_count_(initial_dropped_count) {
  if (capacity == 0) {
    throw std::invalid_argument("compute-event ring capacity must be nonzero");
  }
  if (initial_sequence == 0) {
    throw std::invalid_argument(
        "compute-event initial sequence must be nonzero");
  }
  static_assert(std::is_nothrow_move_constructible_v<ComputeEventSnapshot>);
  static_assert(std::is_nothrow_move_assignable_v<ComputeEventSnapshot>);
}

/** @copydoc GraphEventService::push */
void GraphEventService::push(int id, const std::string& name,
                             const std::string& source, double ms) {
  const bool oversized = name.size() > kComputeEventTextMaxBytes ||
                         source.size() > kComputeEventTextMaxBytes;
  const bool invalid_text =
      !oversized && (!valid_utf8(name) || !valid_utf8(source));

  std::lock_guard<std::mutex> lock(mutex_);
  if (next_sequence_ == kObservationSequenceExhausted) {
    saturating_increment(dropped_count_);
    return;
  }

  const uint64_t sequence = next_sequence_;
  if (oversized || invalid_text) {
    next_sequence_ = sequence_successor(sequence);
    saturating_increment(dropped_count_);
    return;
  }

  ComputeEventSnapshot event;
  event.sequence = sequence;
  event.node = NodeId{id};
  event.name = name;
  event.source = source;
  event.elapsed_ms = ms;

  next_sequence_ = sequence_successor(sequence);
  if (size_ == slots_.size()) {
    slots_[head_] = std::move(event);
    head_ = (head_ + 1) % slots_.size();
    saturating_increment(dropped_count_);
    return;
  }

  const std::size_t insertion = (head_ + size_) % slots_.size();
  slots_[insertion].emplace(std::move(event));
  ++size_;
}

/** @copydoc GraphEventService::drain */
ComputeEventBatch GraphEventService::drain(std::size_t limit) {
  if (limit < kComputeEventDrainMinLimit ||
      limit > kComputeEventDrainMaxLimit) {
    throw std::invalid_argument("compute-event drain limit is out of range");
  }

  std::lock_guard<std::mutex> lock(mutex_);
  ComputeEventBatch batch;
  const std::size_t return_count = std::min(limit, size_);
  batch.events.reserve(return_count);

  for (std::size_t offset = 0; offset < return_count; ++offset) {
    const std::size_t index = (head_ + offset) % slots_.size();
    batch.events.push_back(std::move(*slots_[index]));
  }
  for (std::size_t offset = 0; offset < return_count; ++offset) {
    const std::size_t index = (head_ + offset) % slots_.size();
    slots_[index].reset();
  }

  head_ = (head_ + return_count) % slots_.size();
  size_ -= return_count;
  batch.has_more = size_ != 0;
  if (!batch.has_more && next_sequence_ == kObservationSequenceExhausted) {
    batch.next_sequence = kObservationSequenceExhausted;
  } else {
    batch.next_sequence =
        batch.events.empty() ? next_sequence_
                             : sequence_successor(batch.events.back().sequence);
  }
  batch.dropped_count = dropped_count_;
  dropped_count_ = 0;
  return batch;
}

}  // namespace ps
