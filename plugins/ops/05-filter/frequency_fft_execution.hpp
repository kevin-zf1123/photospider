#pragma once

#include <algorithm>

#include "05-filter/frequency_fft_fixed.hpp"
#include "05-filter/frequency_fft_planned.hpp"
#include "05-filter/frequency_fft_short_double.hpp"
#include "05-filter/frequency_fft_single.hpp"
#include "05-filter/frequency_parallel.hpp"

namespace ps::plugin_internal::frequency {
#if PS_FRQ_PACKETS
template <class Scalar>
class TransformExecution final {
 public:
  using Engine = PlannedTransform<Scalar>;
  using Base = TransformEngine<Scalar>;
  using Complex = TransformComplex<Scalar>;
  static constexpr std::uint64_t native_column_limit = 65536;
  struct Axis {
    SingleTransform<Scalar> single;
    const typename Engine::Plan* mixed = nullptr;
    const typename Base::PowerPlan* power = nullptr;
    const FixedTransform::Plan* fixed = nullptr;
    const typename Base::Roots* short_roots = nullptr;
    const FixedTransform::Plan* crt_fixed16 = nullptr;
    const typename Base::Roots* crt_roots16 = nullptr;
    bool coprime_columns = false;
    std::uint64_t n = 0, cost = 0;
    bool inverse = false;
  };
  TransformExecution(std::uint64_t h, std::uint64_t pitch, std::uint64_t row,
                     bool inverse, typename Engine::Cache& plans,
                     TransformCache<Scalar>& tables,
                     FixedTransform::Cache& fixed, const Work& work,
                     bool packet_column_tail = false,
                     bool convolution_mixed = false)
      : height_(h),
        pitch_(pitch),
        row_(row),
        stride_(transform_worker_stride(h, pitch, row,
                                        std::is_same_v<Scalar, double>)),
        capacity_(transform_worker_capacity(h, pitch, row)),
        column_tail_(packet_column_tail && pitch >= 4 &&
                     std::is_same_v<Scalar, float>) {
    rows_ = prepare(row, inverse, plans, tables, fixed, work, true, false,
                    false, h == 1);
    columns_ =
        prepare(h, inverse, plans, tables, fixed, work,
                pitch % TransformPacket<Scalar>::width == 0 || column_tail_,
                true, pitch == 1, pitch == 1);
    if constexpr (std::is_same_v<Scalar, float>) {
      if (column_tail_ && columns_.fixed && pitch % 4) {
        columns_.mixed = &Engine::get(plans, h, inverse, work);
      }
    }
    // These strategies are private opt-ins for convolution. The original
    // mixed plans and their full-axis admission costs remain available.
    if (convolution_mixed) {
      columns_.coprime_columns = true;
      if (row == 48 && rows_.mixed) {
        if constexpr (std::is_same_v<Scalar, float>) {
          rows_.crt_fixed16 = &FixedTransform::get(fixed, 16, inverse, work);
        } else {
          rows_.crt_roots16 = &Base::root_table(tables, 16, inverse, work);
        }
      }
    }
  }
  void rows(Complex* data, Complex* scratch, const Work& work) const {
    execute(data, 1, height_, pitch_, scratch, rows_, work);
  }
  void real(Complex* data, std::uint64_t width, TransformCache<Scalar>& tables,
            const Work& work) const {
    const auto& roots = Base::root_table(tables, width, rows_.inverse, work);
    real(data, width, roots, work);
  }
  void real(Complex* data, std::uint64_t width,
            const typename Base::Roots& roots, const Work& work) const {
    transform_rows(
        height_, 32 + 64 * (width / 4), work,
        [&](std::uint64_t begin, std::uint64_t count, const Work& local) {
          Base::real_rows(data + begin * pitch_, count, width, rows_.inverse,
                          roots, local);
        });
  }
  // Opt-in for convolution buffers whose untouched rows are known zeros.
  // Preserve their physical row indices; the column phase still uses the
  // complete plane. Existing full-axis entries retain their behavior.
  void forward_rows(Complex* data, Complex* scratch, std::uint64_t first,
                    std::uint64_t count, const Work& work) const {
    require(!rows_.inverse && first <= height_ && count <= height_ - first,
            "forward FFT row range");
    if (count) {
      execute(data + first * pitch_, 1, count, pitch_, scratch, rows_, work);
    }
  }
  void forward_real_rows(Complex* data, std::uint64_t width,
                         const typename Base::Roots& roots, std::uint64_t first,
                         std::uint64_t count, const Work& work) const {
    require(!rows_.inverse && width % 2 == 0 && width / 2 == row_ &&
                pitch_ == row_ + 1 && first <= height_ &&
                count <= height_ - first,
            "forward real FFT row range");
    if (!count) {
      return;
    }
    transform_rows(
        count, 32 + 64 * (width / 4), work,
        [&](std::uint64_t begin, std::uint64_t lines, const Work& local) {
          Base::real_rows(data + (first + begin) * pitch_, lines, width, false,
                          roots, local);
        });
  }
  // After inverse columns, y is spatial and the remaining stages are
  // independent per row. Omitted rows remain in x-frequency form; callers
  // must never consume them as completed spatial output.
  void inverse_rows(Complex* data, Complex* scratch, std::uint64_t first,
                    std::uint64_t count, const Work& work) const {
    require(rows_.inverse && first <= height_ && count <= height_ - first,
            "inverse FFT output row range");
    if (count) {
      execute(data + first * pitch_, 1, count, pitch_, scratch, rows_, work);
    }
  }
  void inverse_real_rows(Complex* data, std::uint64_t width,
                         const typename Base::Roots& roots, std::uint64_t first,
                         std::uint64_t count, const Work& work) const {
    require(rows_.inverse && width % 2 == 0 && width / 2 == row_ &&
                pitch_ == row_ + 1 && first <= height_ &&
                count <= height_ - first,
            "inverse real FFT output row range");
    if (!count) {
      return;
    }
    transform_rows(
        count, 32 + 64 * (width / 4), work,
        [&](std::uint64_t begin, std::uint64_t lines, const Work& local) {
          Base::real_rows(data + (first + begin) * pitch_, lines, width, true,
                          roots, local);
        });
  }
  void columns(Complex* data, Complex* scratch, const Work& work) const {
    if constexpr (std::is_same_v<Scalar, double>) {
      if (columns_.short_roots) {
        if (pitch_ == 1) {
          execute(data, 1, 1, 1, scratch, columns_, work);
        } else {
          short_columns(data, scratch, work);
        }
        return;
      }
    }
    if constexpr (std::is_same_v<Scalar, float>) {
      if (columns_.fixed && height_ <= native_column_limit &&
          (pitch_ % 4 == 0 || column_tail_)) {
        fixed_columns(data, scratch, work);
        return;
      }
    }
    if ((height_ < 256 && !columns_.power) || pitch_ == 1 ||
        (height_ < 8192 && columns_.mixed)) {
      execute(data, pitch_, pitch_, 1, scratch, columns_, work);
      return;
    }
#if defined(__x86_64__)
    constexpr std::uint64_t power_column_limit = 4096;
#else
    constexpr std::uint64_t power_column_limit = 8192;
#endif
    if (height_ < power_column_limit && columns_.power &&
        pitch_ % TransformPacket<Scalar>::width == 0) {
      power_columns(data, scratch, work);
      return;
    }
    constexpr std::uint64_t block = 8;
    const auto cost = columns_.cost + 16 * height_;
    const auto workers = column_participants(cost * pitch_);
    // Thin tall planes still expose enough independent bands to participants.
#if defined(__x86_64__)
    // Keep a large-axis transpose strip in the private/shared CPU caches.
    const std::uint64_t maximum_band = height_ >= 4096 ? 8 : 64;
#else
    constexpr std::uint64_t maximum_band = 64;
#endif
    const auto band = std::min<std::uint64_t>(
        maximum_band, std::max<std::uint64_t>(
                          8, ((pitch_ + workers - 1) / workers + 7) / 8 * 8));
    check(work(cost * pitch_));
    const TransformAdmission admission(work);
    const auto bands = (pitch_ + band - 1) / band;
    const auto run_band = [&](std::uint64_t index, unsigned id) {
      const auto x0 = index * band, count = std::min(band, pitch_ - x0);
      auto local = admission.local(cost * count);
      auto* workspace = scratch + id * stride_;
      auto* transposed = workspace + transform_line_workspace(height_, row_);
      const auto target_pitch = height_ + 1;
      {
        const TransformBlock gather{local, 8 * height_ * count};
        if constexpr (std::is_same_v<Scalar, float>) {
          TransformIO<Scalar>::template transpose_four<true>(
              transposed, target_pitch, data + x0, pitch_, height_, count,
              gather);
        } else {
          for (std::uint64_t y0 = 0; y0 < height_; y0 += block) {
            for (std::uint64_t x = 0; x < count; ++x) {
              const auto end = std::min(height_, y0 + block);
              gather.poll(8 * (end - y0));
              for (auto y = y0; y < end; ++y) {
                new (transposed + x * target_pitch + y)
                    Complex{data[y * pitch_ + x0 + x]};
              }
            }
          }
        }
      }
      apply(transposed, 1, count, target_pitch, workspace, columns_, local);
      {
        const TransformBlock scatter{local, 8 * height_ * count};
        if constexpr (std::is_same_v<Scalar, float>) {
          TransformIO<Scalar>::template transpose_four<false>(
              data + x0, pitch_, transposed, target_pitch, count, height_,
              scatter);
        } else {
          for (std::uint64_t y0 = 0; y0 < height_; y0 += block) {
            for (std::uint64_t x = 0; x < count; ++x) {
              const auto end = std::min(height_, y0 + block);
              scatter.poll(8 * (end - y0));
              for (auto y = y0; y < end; ++y) {
                data[y * pitch_ + x0 + x] = transposed[x * target_pitch + y];
              }
            }
          }
        }
      }
    };
    transform_phase(work, [&] {
      if (workers == 1) {
        for (std::uint64_t i = 0; i < bands; ++i) {
          run_band(i, 0);
        }
      } else {
        transform_pool().run(bands, workers, run_band);
      }
    });
  }

  // Conservative numerical admission for an independent convolution block.
  // It covers every column strategy; unused allowance remains local.
  std::uint64_t arithmetic_bound(std::uint64_t real_width = 0) const {
    auto column = columns_.cost + 16 * height_;
    if constexpr (std::is_same_v<Scalar, float>) {
      if (columns_.fixed) {
        column = std::max(column, FixedTransform::column_cost(height_));
      }
    } else {
      if (columns_.short_roots) {
        column = std::max(column, ShortDoubleTransform::column_cost(height_));
      }
    }
    if (columns_.mixed) {
      column = std::max(column, Engine::line_cost(*columns_.mixed));
    }
    const auto real = real_width ? height_ * (32 + 64 * (real_width / 4)) : 0;
    return rows_.cost * height_ + column * pitch_ + real;
  }

  // A lower bound of the parent Work charges for a convolution transform.
  // Unlike arithmetic_bound(), this may discard work, never invent it. The
  // caller supplies its actual active forward rows or complete inverse rows.
  unsigned __int128 convolution_arithmetic_floor(
      std::uint64_t active_rows, std::uint64_t real_width = 0) const {
    using Wide = unsigned __int128;
    Wide column = Wide(columns_.cost) * pitch_;
    if constexpr (std::is_same_v<Scalar, double>) {
      if (columns_.short_roots && pitch_ > 1) {
        const auto tail =
            columns_.mixed ? Engine::line_cost(*columns_.mixed) : 0;
        column = Wide(ShortDoubleTransform::column_cost(height_)) *
                     (pitch_ - pitch_ % 2) +
                 Wide(tail) * (pitch_ % 2);
      }
    } else {
      if (columns_.fixed && height_ <= native_column_limit &&
          (pitch_ % 4 == 0 || column_tail_)) {
        const auto tail =
            columns_.mixed ? Engine::line_cost(*columns_.mixed) : 0;
        column =
            Wide(FixedTransform::column_cost(height_)) * (pitch_ - pitch_ % 4) +
            Wide(tail) * (pitch_ % 4);
      }
    }
    if (columns_.power) {
      // Native packet columns omit the generic power permutation charge;
      // transposed columns pay at least the ordinary Axis::cost instead.
      const auto packet = columns_.cost -
                          height_ * (2 * columns_.power->levels + 8) +
                          12 * height_;
      column = Wide(std::min(columns_.cost, packet)) * pitch_;
    }
    if (columns_.single.active() && pitch_ == 1) {
      column = 0;
    }
    const Wide rows = rows_.single.active() && active_rows == 1
                          ? 0
                          : Wide(rows_.cost) * active_rows;
    const Wide real =
        real_width ? Wide(active_rows) * (32 + 64 * (real_width / 4)) : 0;
    return rows + column + real;
  }

  // DCT and convolution adapters use the same ISA-selected one-axis kernels.
  // Scratch remains caller-owned; all immutable plans stay in the caches.
  static Axis plan_line(std::uint64_t n, bool inverse,
                        typename Engine::Cache& plans,
                        TransformCache<Scalar>& tables,
                        FixedTransform::Cache& fixed, const Work& work,
                        bool single_axis = false) {
    return prepare(n, inverse, plans, tables, fixed, work, true, false, false,
                   single_axis);
  }
  static void apply_line(Complex* data, std::uint64_t lines,
                         std::uint64_t pitch, Complex* scratch,
                         const Axis& axis, const Work& work,
                         std::uint64_t parallel_scratch_capacity = 0) {
    if (parallel_scratch_capacity && lines == 1 && axis.single.active()) {
      axis.single.execute(data, scratch, parallel_scratch_capacity, work);
      return;
    }
    apply(data, 1, lines, pitch, scratch, axis, work);
  }

 private:
  static constexpr std::uint64_t minimum_compute_work = UINT64_C(1) << 19;
  PS_FRQ_VECTOR_TARGET void short_columns(Complex* data, Complex* scratch,
                                          const Work& work) const {
    if constexpr (std::is_same_v<Scalar, double>) {
      const auto& roots = *columns_.short_roots;
      const auto cost = ShortDoubleTransform::column_cost(height_);
      const auto tail_cost =
          columns_.mixed ? Engine::line_cost(*columns_.mixed) : 0;
      const auto native_cost = cost * (pitch_ - pitch_ % 2);
      const auto total = native_cost + (pitch_ % 2) * tail_cost;
      const auto workers = native_cost <= 2 * minimum_compute_work
                               ? 1U
                               : column_participants(native_cost);
      check(work(total));
      const TransformAdmission admission(work);
      constexpr std::uint64_t band = 8;
      require(band * height_ <= stride_, "short column scratch capacity");
      const auto bands = (pitch_ + band - 1) / band;
      const auto run_band = [&](std::uint64_t index,
                                unsigned id) PS_FRQ_VECTOR_TARGET {
        const auto x0 = band * index, count = std::min(band, pitch_ - x0);
        const auto paired = count - count % 2;
        auto local = admission.local(paired * cost + (count % 2) * tail_cost);
        if (paired) {
          const TransformBlock arithmetic{local, paired * cost};
          if (columns_.inverse) {
            ShortDoubleTransform::columns<true>(data + x0, pitch_,
                                                scratch + id * stride_, paired,
                                                roots, arithmetic);
          } else {
            ShortDoubleTransform::columns<false>(data + x0, pitch_,
                                                 scratch + id * stride_, paired,
                                                 roots, arithmetic);
          }
        }
        if (count % 2) {
          Engine::axis(data + x0 + paired, pitch_, 1, 1, scratch + id * stride_,
                       *columns_.mixed, local);
        }
      };
      transform_phase(work, [&] {
        if (workers == 1) {
          for (std::uint64_t i = 0; i < bands; ++i) {
            run_band(i, 0);
          }
        } else {
          transform_pool().run(bands, workers, run_band);
        }
      });
    }
  }
  PS_FRQ_VECTOR_TARGET void fixed_columns(Complex* data, Complex* scratch,
                                          const Work& work) const {
    if constexpr (std::is_same_v<Scalar, float>) {
      const auto& plan = *columns_.fixed;
      const auto cost = FixedTransform::column_cost(height_);
      const auto tail_cost =
          columns_.mixed ? Engine::line_cost(*columns_.mixed) : 0;
      const auto vector_count = pitch_ - pitch_ % 4;
      const auto total = cost * vector_count + tail_cost * (pitch_ % 4);
      const auto workers = column_participants(total);
      check(work(total));
      const TransformAdmission admission(work);
#if defined(__APPLE__) && defined(__aarch64__)
      const std::uint64_t band =
          height_ >= 256 && (!column_tail_ || pitch_ >= 8) ? 16 : 8;
#else
      constexpr std::uint64_t band = 8;
#endif
      require(band * height_ <= stride_, "fixed column scratch capacity");
      const auto bands = (pitch_ + band - 1) / band;
      const auto run_band = [&](std::uint64_t index,
                                unsigned id) PS_FRQ_VECTOR_TARGET {
        const auto x0 = band * index, count = std::min(band, pitch_ - x0);
        const auto paired = count - count % 4;
        auto local = admission.local(paired * cost + (count % 4) * tail_cost);
        if (paired) {
          const TransformBlock arithmetic{local, paired * cost};
          if (columns_.inverse) {
            FixedTransform::columns<true>(data + x0, pitch_,
                                          scratch + id * stride_, paired, plan,
                                          arithmetic);
          } else {
            FixedTransform::columns<false>(data + x0, pitch_,
                                           scratch + id * stride_, paired, plan,
                                           arithmetic);
          }
        }
        if (count % 4) {
          Engine::axis(data + x0 + paired, pitch_, count % 4, 1,
                       scratch + id * stride_, *columns_.mixed, local);
        }
      };
      transform_phase(work, [&] {
        if (workers == 1) {
          for (std::uint64_t i = 0; i < bands; ++i) {
            run_band(i, 0);
          }
        } else {
          transform_pool().run(bands, workers, run_band);
        }
      });
    }
  }
  PS_FRQ_VECTOR_TARGET void power_columns(Complex* data, Complex* scratch,
                                          const Work& work) const {
    constexpr std::uint64_t band = 8;
    const auto& plan = *columns_.power;
    const auto cost =
        columns_.cost - height_ * (2 * plan.levels + 8) + 12 * height_;
    const auto workers = column_participants(cost * pitch_);
    check(work(cost * pitch_));
    const TransformAdmission admission(work);
    const auto bands = (pitch_ + band - 1) / band;
    const auto run_band = [&](std::uint64_t index, unsigned id) {
      const auto x0 = index * band, count = std::min(band, pitch_ - x0);
      auto local = admission.local(cost * count);
      auto* buffer = scratch + id * stride_;
      {
        const TransformBlock gather{local, 8 * height_ * count};
        for (std::uint64_t y = 0; y < height_; ++y) {
          gather.poll(8 * count);
          for (std::uint64_t x = 0; x < count; ++x) {
            new (buffer + plan.reversal[y] * count + x)
                Complex{data[y * pitch_ + x0 + x]};
          }
        }
      }
      if (columns_.inverse) {
        Base::template packet_columns<true>(buffer, height_, count, plan,
                                            local);
      } else {
        Base::template packet_columns<false>(buffer, height_, count, plan,
                                             local);
      }
      {
        const TransformBlock scatter{local, 4 * height_ * count};
        for (std::uint64_t y = 0; y < height_; ++y) {
          scatter.poll(4 * count);
          for (std::uint64_t x = 0; x < count; ++x) {
            data[y * pitch_ + x0 + x] = buffer[y * count + x];
          }
        }
      }
    };
    transform_phase(work, [&] {
      if (workers == 1) {
        for (std::uint64_t i = 0; i < bands; ++i) {
          run_band(i, 0);
        }
      } else {
        transform_pool().run(bands, workers, run_band);
      }
    });
  }
  static Axis prepare(std::uint64_t n, bool inverse,
                      typename Engine::Cache& plans,
                      TransformCache<Scalar>& tables,
                      FixedTransform::Cache& fixed, const Work& work,
                      bool small_power, bool column_axis = false,
                      bool contiguous_column = false,
                      bool single_axis = false) {
    Axis result;
    result.n = n;
    result.inverse = inverse;
    if (n == 1) {
      return result;
    }
    const bool long_line = SingleTransform<Scalar>::supports(n);
    bool balanced_rows = false;
#if defined(__APPLE__) && defined(__aarch64__)
    if constexpr (std::is_same_v<Scalar, float>) {
      balanced_rows = !column_axis && !single_axis &&
                      SingleTransform<Scalar>::supports_row(n);
    } else {
      balanced_rows = !column_axis && long_line;
    }
#elif defined(__x86_64__)
    if constexpr (std::is_same_v<Scalar, double>) {
      balanced_rows =
          !column_axis && long_line && transform_pool().count() == 1;
    }
#endif
    if ((single_axis && long_line) || balanced_rows) {
      result.single.prepare(n, inverse, tables, fixed, work);
      result.cost = result.single.cost();
      return result;
    }
    if constexpr (std::is_same_v<Scalar, float>) {
      bool fixed_power =
          small_power && n <= (column_axis ? native_column_limit : 128);
#if defined(__x86_64__)
      // The large balanced kernel wins on Intel at 16K and above. Smaller
      // powers retain the lower-overhead radix-four column strategy.
      fixed_power |= n >= 16384 || (!column_axis && n == 1024 &&
                                    transform_pool().count() == 1);
#endif
      if (fixed_power && FixedTransform::supports(n)) {
        result.fixed = &FixedTransform::get(
            fixed, n, inverse, work,
            column_axis && small_power && n <= native_column_limit);
        result.cost = FixedTransform::cost(n);
        return result;
      }
    }
    if constexpr (std::is_same_v<Scalar, double>) {
      const bool line_mode = !column_axis || contiguous_column;
      const bool selected =
          line_mode ? n <= 128 || n == 1024 : transform_double_columns(n);
      const bool supported = line_mode
                                 ? ShortDoubleTransform::row_supports(n)
                                 : ShortDoubleTransform::column_supports(n);
      if (selected && supported) {
        result.short_roots = &Base::root_table(tables, n, inverse, work);
        result.cost = ShortDoubleTransform::cost(n);
        if (!line_mode && !small_power) {
          result.mixed = &Engine::get(plans, n, inverse, work);
        }
        return result;
      }
    }
    if (Base::power2(n) && n >= 256) {
      result.power = &Base::power_table(tables, n, inverse, work);
      result.cost = Base::power_cost(n, result.power->levels);
    } else {
      result.mixed = &Engine::get(plans, n, inverse, work);
      result.cost = Engine::line_cost(*result.mixed);
    }
    return result;
  }
  template <unsigned Index = 0>
  static PS_FRQ_PACKET_INLINE void crt_gather48(Complex* scratch,
                                                const Complex* row) {
    constexpr auto j = Index / 16, k = Index % 16;
    new (scratch + Index) Complex{row[(16 * j + 3 * k) % 48]};
    if constexpr (Index + 1 < 48) {
      crt_gather48<Index + 1>(scratch, row);
    }
  }
  // Good-Thomas 3x16: no twiddle lies between the two coprime factors.
  // Native 16-point leaves operate on contiguous complex values; the final
  // radix-three layer writes the original natural frequency ordering.
  template <bool Inverse>
  static PS_FRQ_VECTOR_TARGET void crt_rows48(
      Complex* data, std::uint64_t lines, std::uint64_t step, Complex* scratch,
      const Axis& axis, const Work& work) {
    using Ops = typename Engine::ScalarOps;
    constexpr std::uint64_t m = 16, n = 48;
    const TransformBlock fuel{work, axis.cost * lines};
    for (std::uint64_t line = 0; line < lines; ++line) {
      auto* row = data + line * step;
      fuel.poll(8 * n);
      crt_gather48(scratch, row);
      for (unsigned j = 0; j < 3; ++j) {
        if constexpr (std::is_same_v<Scalar, float>) {
          FixedTransform::template line<16, Inverse>(
              scratch + j * m, scratch + n, *axis.crt_fixed16, fuel);
        } else {
          ShortDoubleTransform::template line<16, Inverse>(
              scratch + j * m, scratch + n, *axis.crt_roots16, fuel);
        }
      }
      for (std::uint64_t p = 0; p + 3 <= m; p += 3) {
        Engine::template coprime_three_output<Ops, Inverse, 0, 1>(
            row, 1, scratch, m, p, p, fuel);
        Engine::template coprime_three_output<Ops, Inverse, 1, 1>(
            row, 1, scratch, m, p + 1, p + 1, fuel);
        Engine::template coprime_three_output<Ops, Inverse, 2, 1>(
            row, 1, scratch, m, p + 2, p + 2, fuel);
      }
      Engine::template coprime_three_output<Ops, Inverse, 0, 1>(
          row, 1, scratch, m, 15, 15, fuel);
    }
    check(work(0));
  }
  static PS_FRQ_VECTOR_TARGET void apply(Complex* data, std::uint64_t stride,
                                         std::uint64_t lines,
                                         std::uint64_t step, Complex* scratch,
                                         const Axis& axis, const Work& work) {
    if (axis.n == 1) {
      return;
    }
    if (axis.crt_fixed16 || axis.crt_roots16) {
      require(stride == 1 && axis.n == 48, "CRT native contiguous rows");
      if (axis.inverse) {
        crt_rows48<true>(data, lines, step, scratch, axis, work);
      } else {
        crt_rows48<false>(data, lines, step, scratch, axis, work);
      }
      return;
    }
    if (axis.single.active()) {
      require(stride == 1, "serial FFT contiguous rows");
      for (std::uint64_t i = 0; i < lines; ++i) {
        axis.single.execute_serial(data + i * step, scratch, 2 * axis.n, work);
      }
      return;
    }
    if constexpr (std::is_same_v<Scalar, float>) {
      if (axis.fixed) {
        const TransformBlock arithmetic{work, axis.cost * lines};
        for (std::uint64_t i = 0; i < lines; ++i) {
          if (axis.inverse) {
            FixedTransform::execute<true>(data + i * step, scratch, *axis.fixed,
                                          arithmetic);
          } else {
            FixedTransform::execute<false>(data + i * step, scratch,
                                           *axis.fixed, arithmetic);
          }
        }
        return;
      }
    }
    if constexpr (std::is_same_v<Scalar, double>) {
      if (axis.short_roots) {
        const TransformBlock arithmetic{work, axis.cost * lines};
        for (std::uint64_t i = 0; i < lines; ++i) {
          if (axis.inverse) {
            ShortDoubleTransform::execute<true>(data + i * step, scratch,
                                                *axis.short_roots, arithmetic);
          } else {
            ShortDoubleTransform::execute<false>(data + i * step, scratch,
                                                 *axis.short_roots, arithmetic);
          }
        }
        return;
      }
    }
    if (axis.power) {
      for (std::uint64_t i = 0; i < lines; ++i) {
        if (axis.inverse) {
          Base::template packet_line<true>(data + i * step, axis.n, *axis.power,
                                           work);
        } else {
          Base::template packet_line<false>(data + i * step, axis.n,
                                            *axis.power, work);
        }
      }
    } else {
      if (axis.coprime_columns) {
        Engine::template axis<true>(data, stride, lines, step, scratch,
                                    *axis.mixed, work);
      } else {
        Engine::axis(data, stride, lines, step, scratch, *axis.mixed, work);
      }
    }
  }
  unsigned column_participants(std::uint64_t cost) const {
    auto count = participants(cost);
#if defined(__x86_64__)
    if constexpr (std::is_same_v<Scalar, double>) {
      // Large native bands exceed the shared E-core cache with more workers.
      // Keep the independent row phase free to use the full arithmetic pool.
      if (height_ >= 8192 && columns_.short_roots) {
        count = std::min(count, 8U);
      }
    }
#endif
    return count;
  }
  unsigned participants(std::uint64_t cost) const {
    // Below this amount, wake-up and synchronization dominate arithmetic.
    constexpr auto minimum = minimum_compute_work;
    if (capacity_ == 1 || cost < 2 * minimum) {
      return 1;
    }
    auto workers = static_cast<unsigned>(std::min<std::uint64_t>(
        {capacity_, transform_pool().count(), cost / minimum}));
#if defined(__APPLE__) && defined(__aarch64__)
    // Short arithmetic phases do not amortize waking the whole hybrid pool.
    if (cost < (UINT64_C(1) << 24) || height_ * pitch_ < (UINT64_C(1) << 17)) {
      workers = std::min(workers, 4U);
    }
#endif
    return workers;
  }
  void execute(Complex* data, std::uint64_t stride, std::uint64_t lines,
               std::uint64_t step, Complex* scratch, const Axis& axis,
               const Work& work) const {
    if (axis.n == 1) {
      return;
    }
    if (axis.single.active() && lines == 1) {
      require(stride == 1, "single FFT contiguous axis");
      axis.single.execute(data, scratch, stride_, work);
      return;
    }
    const auto cost = axis.cost * lines;
    const auto workers = participants(cost);
    check(work(cost));
    const TransformAdmission admission(work);
    transform_phase(work, [&] {
      if (workers == 1) {
        auto local = admission.local(cost);
        apply(data, stride, lines, step, scratch, axis, local);
      } else {
        const std::uint64_t packet = axis.single.active() ? 1 : 8;
        const auto grain = std::max<std::uint64_t>(
            packet, (lines / (workers * 8) + packet - 1) / packet * packet);
        const auto tasks = (lines + grain - 1) / grain;
        transform_pool().run(
            tasks, workers, [&](std::uint64_t task, unsigned id) {
              const auto begin = task * grain;
              const auto count = std::min(grain, lines - begin);
              auto local = admission.local(axis.cost * count);
              apply(data + begin * step, stride, count, step,
                    scratch + id * stride_, axis, local);
            });
      }
    });
  }
  std::uint64_t height_, pitch_, row_, stride_, capacity_;
  Axis rows_, columns_;
  bool column_tail_ = false;
};
#endif
}  // namespace ps::plugin_internal::frequency
