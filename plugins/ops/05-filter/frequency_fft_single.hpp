#pragma once

#include <algorithm>
#include <mutex>
#include <type_traits>
#include <utility>

#include "05-filter/frequency_fft_fixed.hpp"
#include "05-filter/frequency_fft_short_double.hpp"
#include "05-filter/frequency_parallel.hpp"

namespace ps::plugin_internal::frequency {
#if PS_FRQ_PACKETS
// Balanced factors expose SIMD lanes across native column transforms. A lone
// long axis also runs its independent bands in parallel; batched rows retain
// the caller's outer parallelism and use the serial entry point.
template <class Scalar>
class SingleTransform final {
 public:
  using Complex = TransformComplex<Scalar>;
  using Base = TransformEngine<Scalar>;
  using Roots = typename Base::Roots;
  using Packet = TransformPacket<Scalar>;
  static constexpr bool supports(std::uint64_t n) {
    return n >= 16384 && n <= 65536 && !(n & (n - 1));
  }
  static constexpr bool supports_row(std::uint64_t n) {
    return n >= 256 && n <= 65536 && !(n & (n - 1));
  }
  bool active() const { return twiddles_ != nullptr; }
  void prepare(std::uint64_t n, bool inverse, TransformCache<Scalar>& tables,
               FixedTransform::Cache& fixed, const Work& work) {
    n_ = n;
    a_ = FixedTransform::split(n);
    b_ = n / a_;
    inverse_ = inverse;
    twiddles_ = &twiddle_table(tables, n, a_, b_, inverse, work);
    if constexpr (std::is_same_v<Scalar, float>) {
      a_fixed_ = &FixedTransform::get(fixed, a_, inverse, work, true);
      b_fixed_ = &FixedTransform::get(fixed, b_, inverse, work, true);
    } else {
      a_roots_ = &Base::root_table(tables, a_, inverse, work);
      b_roots_ = &Base::root_table(tables, b_, inverse, work);
    }
  }
  static constexpr std::uint64_t native_cost(std::uint64_t n) {
    if constexpr (std::is_same_v<Scalar, float>)
      return FixedTransform::column_cost(n);
    else
      return ShortDoubleTransform::column_cost(n);
  }
  std::uint64_t cost() const {
    return a_ * native_cost(b_) + b_ * native_cost(a_) + 64 * n_;
  }
  PS_FRQ_VECTOR_TARGET void execute_serial(Complex* data, Complex* scratch,
                                           std::uint64_t capacity,
                                           const Work& work) const {
#if defined(__APPLE__) && defined(__aarch64__)
    constexpr std::uint64_t band = 16;
#else
    constexpr std::uint64_t band = 8;
#endif
    constexpr auto first_band = band / 2;
    require(capacity >= n_ + band * std::max(a_, b_),
            "serial FFT scratch capacity");
    auto* temporary = scratch + n_;
    const TransformBlock fuel{work, cost()};
    for (std::uint64_t begin = 0; begin < a_; begin += first_band) {
      columns<true>(data + begin, temporary + first_band * b_, a_, first_band,
                    temporary, first_band, b_, b_fixed_, fuel);
      transpose_band(temporary + first_band * b_, scratch, begin, first_band,
                     fuel);
    }
    for (std::uint64_t begin = 0; begin < b_; begin += band)
      columns(scratch + begin, data + begin, b_, b_, temporary, band, a_,
              a_fixed_, fuel);
    check(work(0));
  }
  PS_FRQ_VECTOR_TARGET void execute(Complex* data, Complex* scratch,
                                    std::uint64_t capacity,
                                    const Work& work) const {
#if defined(__APPLE__) && defined(__aarch64__)
    constexpr std::uint64_t band = 16;
#else
    constexpr std::uint64_t band = 8;
#endif
    constexpr auto first_band = band / 2;
    const auto worker_stride = band * std::max(a_, b_);
    require(capacity >= n_ + worker_stride, "single FFT scratch capacity");
    auto workers = static_cast<unsigned>(std::min<std::uint64_t>(
        {transform_pool().count(), cost() / (UINT64_C(1) << 19),
         (capacity - n_) / worker_stride, a_ / band, b_ / band}));
    workers = std::max(1U, workers);
#if defined(__APPLE__) && defined(__aarch64__)
    // There are only 8..16 second-stage bands on the supported long axes.
    workers = std::min(workers, 4U);
#endif
    if (workers == 1) {
      execute_serial(data, scratch, capacity, work);
      return;
    }
    // First-stage tasks own input columns and whole intermediate rows. The
    // second stage begins only after every first-stage write has joined.
    const auto first_cost = a_ * native_cost(b_) + 64 * n_;
    check(work(first_cost));
    const TransformAdmission first(work);
    transform_phase(work, [&] {
      transform_pool().run(
          a_ / first_band, workers,
          [&](std::uint64_t index, unsigned id) PS_FRQ_VECTOR_TARGET {
            const auto begin = index * first_band;
            auto local = first.local(first_band * (native_cost(b_) + 64 * b_));
            auto* temporary = scratch + n_ + id * worker_stride;
            {
              const TransformBlock arithmetic{local,
                                              first_band * native_cost(b_)};
              columns<true>(data + begin, temporary + first_band * b_, a_,
                            first_band, temporary, first_band, b_, b_fixed_,
                            arithmetic);
            }
            const TransformBlock transpose{local, 64 * first_band * b_};
            transpose_band(temporary + first_band * b_, scratch, begin,
                           first_band, transpose);
          });
    });
    const auto second_cost = b_ * native_cost(a_);
    check(work(second_cost));
    const TransformAdmission second(work);
    transform_phase(work, [&] {
      transform_pool().run(
          b_ / band, workers,
          [&](std::uint64_t index, unsigned id) PS_FRQ_VECTOR_TARGET {
            const auto begin = index * band;
            auto local = second.local(band * native_cost(a_));
            auto* temporary = scratch + n_ + id * worker_stride;
            const TransformBlock arithmetic{local, band * native_cost(a_)};
            columns(scratch + begin, data + begin, b_, b_, temporary, band, a_,
                    a_fixed_, arithmetic);
          });
    });
  }

 private:
  static const Roots& twiddle_table(TransformCache<Scalar>& tables,
                                    std::uint64_t n, std::uint64_t a,
                                    std::uint64_t b, bool inverse,
                                    const Work& work) {
    check(work(1));
    const auto key = std::make_pair(n, inverse);
    {
      std::lock_guard<std::mutex> lock(tables.mutex);
      const auto found = tables.single_twiddles.find(key);
      if (found != tables.single_twiddles.end())
        return found->second;
    }
    Roots values(tables.allocator);
    const TransformBlock build{work, 128 * n};
    values.reserve(n);
    for (std::uint64_t x = 0; x < a; ++x)
      for (std::uint64_t y = 0; y < b; ++y) {
        build.poll(128);
        values.push_back(Base::root(x * y, n, inverse));
      }
    check(work(0));
    std::lock_guard<std::mutex> lock(tables.mutex);
    return tables.single_twiddles.emplace(key, std::move(values)).first->second;
  }
  template <unsigned N, bool Inverse, bool ConstructDestination>
  PS_FRQ_VECTOR_TARGET void native_columns(
      const Complex* data, Complex* destination, std::uint64_t pitch,
      std::uint64_t destination_pitch, Complex* scratch, std::uint64_t count,
      const FixedTransform::Plan* plan, const TransformBlock& fuel) const {
    if constexpr (std::is_same_v<Scalar, float>) {
      FixedTransform::column<N, Inverse, true, ConstructDestination>(
          data, pitch, destination, destination_pitch, scratch, count, *plan,
          fuel);
    } else {
      ShortDoubleTransform::column<N, Inverse, true, ConstructDestination>(
          data, pitch, destination, destination_pitch, scratch, count,
          N == a_ ? *a_roots_ : *b_roots_, fuel);
    }
  }
  template <bool ConstructDestination = false>
  PS_FRQ_VECTOR_TARGET void columns(const Complex* data, Complex* destination,
                                    std::uint64_t pitch,
                                    std::uint64_t destination_pitch,
                                    Complex* scratch, std::uint64_t count,
                                    std::uint64_t n,
                                    const FixedTransform::Plan* plan,
                                    const TransformBlock& fuel) const {
    if (n == 16) {
      if (inverse_)
        native_columns<16, true, ConstructDestination>(
            data, destination, pitch, destination_pitch, scratch, count, plan,
            fuel);
      else
        native_columns<16, false, ConstructDestination>(
            data, destination, pitch, destination_pitch, scratch, count, plan,
            fuel);
    } else if (n == 32) {
      if (inverse_)
        native_columns<32, true, ConstructDestination>(
            data, destination, pitch, destination_pitch, scratch, count, plan,
            fuel);
      else
        native_columns<32, false, ConstructDestination>(
            data, destination, pitch, destination_pitch, scratch, count, plan,
            fuel);
    } else if (n == 64) {
      if (inverse_)
        native_columns<64, true, ConstructDestination>(
            data, destination, pitch, destination_pitch, scratch, count, plan,
            fuel);
      else
        native_columns<64, false, ConstructDestination>(
            data, destination, pitch, destination_pitch, scratch, count, plan,
            fuel);
    } else if (n == 128) {
      if (inverse_)
        native_columns<128, true, ConstructDestination>(
            data, destination, pitch, destination_pitch, scratch, count, plan,
            fuel);
      else
        native_columns<128, false, ConstructDestination>(
            data, destination, pitch, destination_pitch, scratch, count, plan,
            fuel);
    } else {
      if (inverse_)
        native_columns<256, true, ConstructDestination>(
            data, destination, pitch, destination_pitch, scratch, count, plan,
            fuel);
      else
        native_columns<256, false, ConstructDestination>(
            data, destination, pitch, destination_pitch, scratch, count, plan,
            fuel);
    }
  }
  PS_FRQ_VECTOR_TARGET void transpose_band(const Complex* data,
                                           Complex* destination,
                                           std::uint64_t begin,
                                           std::uint64_t count,
                                           const TransformBlock& fuel) const {
    constexpr auto kWidth = Packet::width;
    for (std::uint64_t y = 0; y < b_; y += kWidth) {
      for (std::uint64_t x = begin; x < begin + count; x += kWidth) {
        fuel.poll(64 * kWidth * kWidth);
        typename Packet::Register v[kWidth];
#if defined(__clang__)
#pragma clang loop unroll(full)
#endif
        for (unsigned row = 0; row < kWidth; ++row)
          v[row] = Packet::load(data + (y + row) * count + x - begin);
        if constexpr (std::is_same_v<Scalar, float>)
          Packet::transpose(v[0], v[1], v[2], v[3]);
        else
          ShortDoubleTransform::transpose(v[0], v[1]);
#if defined(__clang__)
#pragma clang loop unroll(full)
#endif
        for (unsigned row = 0; row < kWidth; ++row) {
          const auto offset = (x + row) * b_ + y;
          TransformIO<Scalar>::construct(
              destination + offset,
              Packet::product(v[row],
                              Packet::load(twiddles_->data() + offset)));
        }
      }
    }
  }
  const Roots *twiddles_ = nullptr, *a_roots_ = nullptr, *b_roots_ = nullptr;
  const FixedTransform::Plan *a_fixed_ = nullptr, *b_fixed_ = nullptr;
  std::uint64_t n_ = 0, a_ = 0, b_ = 0;
  bool inverse_ = false;
};
#endif
}  // namespace ps::plugin_internal::frequency
