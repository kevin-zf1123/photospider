/*
 * Licensed standalone port of the pinned reference algorithm.
 *
 * Pinned source: OpenCV tag 4.12.0, `modules/photo/src/inpaint.cpp`, blob
 * 2f2f368fa13da0bc1426b71862205048c6ea0f94. The functions `FastMarching_solve`,
 * the narrow-band construction and the single-channel `INPAINT_NS` frontier of
 * `icvNSInpaintFMM<float>` are ported here, including their exact Float32 /
 * Float64 operation order, guard grid, index branches and insertion order.
 * This translation unit includes no OpenCV header, symbol or linkage; it runs
 * on host-accounted scratch and observes cancellation inside the frontier.
 *
 * Floating-point flags: this file uses the shared operation profile
 * `-fno-fast-math -frounding-math -ffp-contract=off`, which spec 0.3.1 also
 * binds for the common OpenCV 4.12.0 reference. Compiled that way the port is
 * bit-exact to both the pinned source and that reference; contracting it would
 * instead reproduce the FMA-contracted package-manager distribution build and
 * drift from the bound reference on large sequential-hole fixtures.
 *
 * Retained notice of the ported work:
 *
 *                        Intel License Agreement
 *                For Open Source Computer Vision Library
 *
 * Copyright (C) 2000, Intel Corporation, all rights reserved.
 * Third party copyrights are property of their respective icvers.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *   * Redistribution's of source code must retain the above copyright notice,
 *     this list of conditions and the following disclaimer.
 *
 *   * Redistribution's in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *
 *   * The name of Intel Corporation may not be used to endorse or promote
 *     products derived from this software without specific prior written
 *     permission.
 *
 * This software is provided by the copyright holders and contributors "as is"
 * and any express or implied warranties, including, but not limited to, the
 * implied warranties of merchantability and fitness for a particular purpose
 * are disclaimed. In no event shall the Intel Corporation or contributors be
 * liable for any direct, indirect, incidental, special, exemplary, or
 * consequential damages (including, but not limited to, procurement of
 * substitute goods or services; loss of use, data, or profits; or business
 * interruption) however caused and on any theory of liability, whether in
 * contract, strict liability, or tort (including negligence or otherwise)
 * arising in any way out of the use of this software, even if advised of the
 * possibility of such damage.
 */

#include <cmath>
#include <cstdint>
#include <cstring>
#include <utility>

#include "00-foundation/basic_execution.hpp"
#include "09-composite/local_inpaint_navier_stokes_common.hpp"
#include "plugin/builtin_operations.hpp"

namespace ps::plugin_internal {
namespace {
using namespace local_inpaint_ns;  // NOLINT(build/namespaces)

/** @brief Guard-grid state codes of the pinned source. */
constexpr std::uint8_t state_known = 0;
constexpr std::uint8_t state_band = 1;
constexpr std::uint8_t state_inside = 2;
/** @brief Frontier cancellation cadence of the native contract. */
constexpr std::uint64_t frontier_poll_pops = 64;
constexpr std::uint64_t frontier_poll_visits = 4096;

/**
 * @brief One pinned heap entry: arrival time, padded row/column, order.
 * @note The exact 16-byte layout assumed by the profile's scratch model.
 */
struct HeapEntry final {
  float arrival = 0;
  std::int32_t row = 0;
  std::int32_t column = 0;
  std::int32_t order = 0;
};

/**
 * @brief Bounded inserting-order binary min-heap over host-owned entries.
 * @note `arrival` then insertion `order` is a strict total order, so the pop
 * sequence equals the pinned `std::priority_queue` sequence. Every padded
 * pixel is inserted at most once, so `capacity == padded` never overflows.
 */
class FrontierHeap final {
 public:
  FrontierHeap(HeapEntry* entries, std::uint64_t capacity) noexcept
      : entries_(entries), capacity_(capacity) {}

  /** @brief Inserts one entry for a staged padded coordinate. */
  bool push(float arrival, std::int32_t row, std::int32_t column) noexcept {
    if (size_ >= capacity_)
      return false;
    entries_[size_] = HeapEntry{arrival, row, column, next_order_};
    ++next_order_;
    std::uint64_t index = size_;
    ++size_;
    while (index > 0) {
      const std::uint64_t parent = (index - 1) / 2;
      if (!before(entries_[index], entries_[parent]))
        break;
      const HeapEntry temporary = entries_[index];
      entries_[index] = entries_[parent];
      entries_[parent] = temporary;
      index = parent;
    }
    return true;
  }

  /** @brief Removes the minimum entry and reports its padded coordinate. */
  bool pop(std::int32_t* row, std::int32_t* column) noexcept {
    if (size_ == 0)
      return false;
    *row = entries_[0].row;
    *column = entries_[0].column;
    --size_;
    entries_[0] = entries_[size_];
    std::uint64_t index = 0;
    for (;;) {
      const std::uint64_t left = 2 * index + 1;
      const std::uint64_t right = left + 1;
      std::uint64_t smallest = index;
      if (left < size_ && before(entries_[left], entries_[smallest]))
        smallest = left;
      if (right < size_ && before(entries_[right], entries_[smallest]))
        smallest = right;
      if (smallest == index)
        break;
      const HeapEntry temporary = entries_[index];
      entries_[index] = entries_[smallest];
      entries_[smallest] = temporary;
      index = smallest;
    }
    return true;
  }

 private:
  static bool before(const HeapEntry& left, const HeapEntry& right) noexcept {
    if (left.arrival < right.arrival)
      return true;
    if (left.arrival > right.arrival)
      return false;
    return left.order < right.order;
  }

  HeapEntry* entries_;
  std::uint64_t capacity_;
  std::uint64_t size_ = 0;
  std::int32_t next_order_ = 0;
};

/** @brief Pinned four-way minimum with the pinned `MIN` comparison order. */
inline float min4(float a, float b, float c, float d) noexcept {
  a = a > b ? b : a;
  c = c > d ? d : c;
  return a > c ? c : a;
}

/**
 * @brief Pinned `FastMarching_solve` for the guard grid.
 * @note The pinned source declares `double sol, a11, a22, m12;` and loads the
 * Float32 arrival grid into binary64, so every comparison and intermediate
 * below stays binary64 and only the returned value narrows to Float32.
 */
float fast_marching_solve(std::int32_t i1, std::int32_t j1, std::int32_t i2,
                          std::int32_t j2, const std::uint8_t* flags,
                          const float* arrival, const Planar& planar) {
  const double a11 = arrival[planar.padded_index(i1, j1)];
  const double a22 = arrival[planar.padded_index(i2, j2)];
  const double m12 = a11 > a22 ? a22 : a11;
  double solution = 0;
  if (flags[planar.padded_index(i1, j1)] != state_inside) {
    if (flags[planar.padded_index(i2, j2)] != state_inside) {
      if (std::fabs(a11 - a22) >= 1.0) {
        solution = 1 + m12;
      } else {
        solution = (a11 + a22 + std::sqrt(2 - (a11 - a22) * (a11 - a22))) * 0.5;
      }
    } else {
      solution = 1 + a11;
    }
  } else if (flags[planar.padded_index(i2, j2)] != state_inside) {
    solution = 1 + a22;
  } else {
    solution = 1 + m12;
  }
  return static_cast<float>(solution);
}

/**
 * @brief Builds the pinned narrow band: cross-dilated holes minus holes.
 * @note The guard ring stays known, matching the pinned border assignment.
 */
void build_band(const OperationInvocation& call, const Planar& planar,
                const std::uint8_t* holes, std::uint8_t* band) {
  const std::int32_t rows = static_cast<std::int32_t>(planar.height) + 2;
  const std::int32_t columns = static_cast<std::int32_t>(planar.width) + 2;
  SampleWatch watch(call);
  for (std::int32_t row = 1; row < rows - 1; ++row) {
    for (std::int32_t column = 1; column < columns - 1; ++column) {
      std::uint8_t maximum = 0;
      const std::int32_t offsets[5][2] = {{0, 0},
                                          {-1, 0},
                                          {0, -1},
                                          {1, 0},
                                          {0, 1}};
      for (const auto& offset : offsets) {
        // Every guard-grid access is counted, so the cancellation gap stays
        // bounded in samples across the complete cross-dilation loop.
        watch.note();
        const std::int32_t r = row + offset[0];
        const std::int32_t c = column + offset[1];
        if (r < 0 || c < 0 || r >= rows || c >= columns)
          continue;
        const std::uint8_t value = holes[planar.padded_index(r, c)];
        if (value > maximum)
          maximum = value;
      }
      watch.note();
      const std::uint8_t centre = holes[planar.padded_index(row, column)];
      band[planar.padded_index(row, column)] =
          maximum > centre ? static_cast<std::uint8_t>(maximum - centre) : 0;
    }
  }
}

/**
 * @brief Runs the ported single-channel Navier-Stokes frontier in place.
 * @param flags Padded state grid, mutated exactly like the pinned `f`.
 * @param arrival Padded arrival times, mutated like the pinned `t`.
 * @param plane Packed HxW work plane, mutated only at hole samples.
 * @param heap Initialized with the narrow band in row-major insertion order.
 * @note Cancellation is observed at most every 64 pops or 4096 candidate
 * visits, whichever comes first, and around both frontier transitions.
 */
void solve_frontier(const OperationInvocation& call, const Planar& planar,
                    std::int32_t range, std::uint8_t* flags, float* arrival,
                    float* plane, FrontierHeap* heap) {
  const std::int32_t rows = static_cast<std::int32_t>(planar.height) + 2;
  const std::int32_t columns = static_cast<std::int32_t>(planar.width) + 2;
  auto plane_at = [plane, &planar](std::int32_t row,
                                   std::int32_t column) -> float& {
    return plane[planar.work_index(static_cast<std::uint64_t>(row),
                                   static_cast<std::uint64_t>(column))];
  };
  std::uint64_t pops = 0;
  std::uint64_t visits = 0;
  std::int32_t row = 0;
  std::int32_t column = 0;
  while (heap->pop(&row, &column)) {
    ++pops;
    if (pops >= frontier_poll_pops || visits >= frontier_poll_visits) {
      poll(call);
      pops = 0;
      visits = 0;
    }
    flags[planar.padded_index(row, column)] = state_known;
    for (std::int32_t neighbour = 0; neighbour < 4; ++neighbour) {
      std::int32_t i = row;
      std::int32_t j = column;
      if (neighbour == 0) {
        i = row - 1;
      } else if (neighbour == 1) {
        j = column - 1;
      } else if (neighbour == 2) {
        i = row + 1;
      } else {
        j = column + 1;
      }
      // Pinned single-channel branch compares against rows-1 / columns-1.
      if (i <= 0 || j <= 0 || i > rows - 1 || j > columns - 1)
        continue;
      if (flags[planar.padded_index(i, j)] != state_inside)
        continue;
      const float distance =
          min4(fast_marching_solve(i - 1, j, i, j - 1, flags, arrival, planar),
               fast_marching_solve(i + 1, j, i, j - 1, flags, arrival, planar),
               fast_marching_solve(i - 1, j, i, j + 1, flags, arrival, planar),
               fast_marching_solve(i + 1, j, i, j + 1, flags, arrival, planar));
      arrival[planar.padded_index(i, j)] = distance;
      float grad_i_x = 0;
      float grad_i_y = 0;
      float r_x = 0;
      float r_y = 0;
      float sum = 0;
      float weight = 1.0e-20F;
      for (std::int32_t k = i - range; k <= i + range; ++k) {
        const std::int32_t km = k - 1 + (k == 1 ? 1 : 0);
        const std::int32_t kp = k - 1 - (k == rows - 2 ? 1 : 0);
        for (std::int32_t l = j - range; l <= j + range; ++l) {
          ++visits;
          if (visits >= frontier_poll_visits) {
            poll(call);
            visits = 0;
            pops = 0;
          }
          const std::int32_t lm = l - 1 + (l == 1 ? 1 : 0);
          const std::int32_t lp = l - 1 - (l == columns - 2 ? 1 : 0);
          if (k <= 0 || l <= 0 || k >= rows - 1 || l >= columns - 1)
            continue;
          if (flags[planar.padded_index(k, l)] == state_inside)
            continue;
          const std::int32_t dy = i - k;
          const std::int32_t dx = j - l;
          if (dx * dx + dy * dy > range * range)
            continue;
          // Pinned single-channel orientation uses (i-k, j-l).
          r_y = static_cast<float>(i - k);
          r_x = static_cast<float>(j - l);
          const float length_r = r_x * r_x + r_y * r_y;
          const float dst = 1.0F / (length_r * length_r + 1.0F);
          if (flags[planar.padded_index(k + 1, l)] != state_inside) {
            if (flags[planar.padded_index(k - 1, l)] != state_inside) {
              grad_i_x = static_cast<float>(
                  std::fabs(plane_at(kp + 1, lm) - plane_at(kp, lm)) +
                  std::fabs(plane_at(kp, lm) - plane_at(km - 1, lm)));
            } else {
              grad_i_x = static_cast<float>(std::fabs(plane_at(kp + 1, lm) -
                                                      plane_at(kp, lm))) *
                         2.0F;
            }
          } else {
            if (flags[planar.padded_index(k - 1, l)] != state_inside) {
              grad_i_x = static_cast<float>(std::fabs(plane_at(kp, lm) -
                                                      plane_at(km - 1, lm))) *
                         2.0F;
            } else {
              grad_i_x = 0;
            }
          }
          if (flags[planar.padded_index(k, l + 1)] != state_inside) {
            if (flags[planar.padded_index(k, l - 1)] != state_inside) {
              grad_i_y = static_cast<float>(
                  std::fabs(plane_at(km, lp + 1) - plane_at(km, lm)) +
                  std::fabs(plane_at(km, lm) - plane_at(km, lm - 1)));
            } else {
              grad_i_y = static_cast<float>(std::fabs(plane_at(km, lp + 1) -
                                                      plane_at(km, lm))) *
                         2.0F;
            }
          } else {
            if (flags[planar.padded_index(k, l - 1)] != state_inside) {
              grad_i_y = static_cast<float>(std::fabs(plane_at(km, lm) -
                                                      plane_at(km, lm - 1))) *
                         2.0F;
            } else {
              grad_i_y = 0;
            }
          }
          grad_i_x = -grad_i_x;
          float direction = r_x * grad_i_x + r_y * grad_i_y;
          if (std::fabs(direction) <= 0.01F) {
            direction = 0.000001F;
          } else {
            // Pinned `VectorScalMult(r, gradI) / sqrt(VectorLength(r) *
            // VectorLength(gradI))` stays binary32: the target C++ math
            // overloads bind `sqrt`/`fabs` on float arguments.
            const float dot = r_x * grad_i_x + r_y * grad_i_y;
            const float length_gradient =
                grad_i_x * grad_i_x + grad_i_y * grad_i_y;
            direction = std::fabs(dot / std::sqrt(length_r * length_gradient));
          }
          const float w = dst * direction;
          sum += w * plane_at(k - 1, l - 1);
          weight += w;
        }
      }
      // Pinned `cv::saturate_cast<float>((double)Ia/s)` is an exact cast.
      plane_at(i - 1, j - 1) = static_cast<float>(static_cast<double>(sum) /
                                                  static_cast<double>(weight));
      flags[planar.padded_index(i, j)] = state_band;
      require(heap->push(distance, i, j), ErrorCode::OperationFailed,
              "inpaint frontier exceeded its bounded insertion capacity");
    }
  }
}

/** @brief Native profile solver with fully host-accounted scratch. */
void solve(const OperationInvocation& call, const Planar& planar,
           std::int64_t range, MutableValue* output) {
  const std::uint64_t padded = planar.padded;
  poll(call);
  auto work_buffer =
      take(call.allocator.allocate(planar.pixels * sizeof(float)));
  poll(call);
  auto mask_buffer = take(call.allocator.allocate(planar.pixels));
  poll(call);
  auto state_buffer = take(call.allocator.allocate(padded * 3));
  poll(call);
  auto arrival_buffer = take(call.allocator.allocate(padded * sizeof(float)));
  poll(call);
  auto heap_buffer = take(call.allocator.allocate(padded * sizeof(HeapEntry)));
  poll(call);
  auto* work = reinterpret_cast<float*>(work_buffer.data());
  auto* binary = mask_buffer.data();
  auto* state = state_buffer.data();
  auto* holes = state;
  auto* band = state + padded;
  auto* flags = state + 2 * padded;
  auto* arrival = reinterpret_cast<float*>(arrival_buffer.data());
  auto* entries = reinterpret_cast<HeapEntry*>(heap_buffer.data());
  build_binary_mask(call, planar, binary);
  SampleWatch guard_watch(call);
  for (std::uint64_t y = 0; y < planar.height; ++y) {
    for (std::uint64_t x = 0; x < planar.width; ++x) {
      guard_watch.note();
      if (binary[planar.work_index(y, x)] == 0)
        continue;
      holes[planar.padded_index(y + 1, x + 1)] = state_inside;
    }
  }
  build_band(call, planar, holes, band);
  const std::int32_t radius_value = static_cast<std::int32_t>(range);
  for (std::uint64_t channel = 0; channel < 3; ++channel) {
    poll(call);
    pack_plane(call, planar, channel, binary, work);
    // `pack_plane` observes cancellation at its entry only, and the guard
    // counter below resumes with its own remainder, so the packing tail needs
    // its own observation before initialization starts.
    poll(call);
    // Pinned per-channel re-initialization: f, t and the initial band heap.
    // The pinned `f.setTo(KNOWN)` runs as a cancellation-checked loop instead
    // of an unchecked bulk store.
    for (std::uint64_t index = 0; index < padded; ++index) {
      guard_watch.note();
      flags[index] = state_known;
    }
    for (std::uint64_t index = 0; index < padded; ++index) {
      guard_watch.note();
      if (band[index] != 0) {
        flags[index] = state_band;
        arrival[index] = 0.0F;
      } else {
        arrival[index] = initial_arrival_time;
      }
    }
    for (std::uint64_t index = 0; index < padded; ++index) {
      guard_watch.note();
      if (holes[index] != 0)
        flags[index] = state_inside;
    }
    FrontierHeap heap(entries, padded);
    for (std::uint64_t index = 0; index < padded; ++index) {
      guard_watch.note();
      if (band[index] == 0)
        continue;
      require(heap.push(0.0F,
                        static_cast<std::int32_t>(index / planar.padded_row()),
                        static_cast<std::int32_t>(index % planar.padded_row())),
              ErrorCode::OperationFailed,
              "inpaint band exceeded its bounded insertion capacity");
    }
    // Separate the shared guard counter's phase from the frontier's own pop
    // and visit counters: the frontier observes at 64 pops or 4096 visits, so
    // its first observation can be far from the last guard-phase one.
    poll(call);
    clear_arithmetic_exceptions();
    solve_frontier(call, planar, radius_value, flags, arrival, work, &heap);
    // Observe cancellation before reporting a dirty arithmetic channel, so a
    // stop requested during the frontier keeps the host Cancelled priority.
    poll(call);
    require_clean_arithmetic();
    require_finite_holes(call, planar, binary, work);
    write_holes(call, planar, call.output_region, work, binary, channel,
                output);
  }
}
}  // namespace
Status register_image_local_inpaint_navier_stokes_native(
    OperationRegistry* registry) {
  OperationDefinition operation;
  operation.key = "image.local_inpaint_navier_stokes_native_apple_silicon";
  auto& traits = operation.traits;
  traits.input_count = 2;
  traits.input_schema = {image_port(), mask_port()};
  traits.parameter_schema = {radius_parameter()};
  // Host-owned packing: one work plane (4N), one binary mask (N), the padded
  // state/band/mask triple (3P), padded arrival times (4P) and the bounded
  // heap (16P). Fixed floor covers the padded overhead of very small images.
  traits.workspace_bytes = 8192;
  traits.workspace_input_multiplier = 3;
  traits.outputs[0].key = "image";
  traits.outputs[0].output_element_type = ElementType::Float32;
  traits.outputs[0].shape_rule = OperationShapeRule::PreserveFirstInput;
  traits.outputs[0].region_rule = OperationRegionRule::Whole;
  traits.outputs[0].output_semantic_rule = OperationSemanticRule::PreserveInput;
  traits.outputs[0].output_schema = traits.input_schema.front();
  operation.callback = [](const OperationInvocation& call) {
    if (call.backend == Backend::Gpu)
      return Result<Value>(Status::failure(ErrorCode::BackendUnavailable,
                                           "image.local_inpaint_navier_stokes_"
                                           "native_apple_silicon is CPU only"));
    return run_profile(call, solve);
  };
  return registry->register_operation(std::move(operation));
}
}  // namespace ps::plugin_internal
