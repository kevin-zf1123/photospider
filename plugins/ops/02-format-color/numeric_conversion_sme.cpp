// Isolated SME target. The host checks ISA availability before this call.
#include <arm_sme.h>

#include <cstdint>

#include "execution/cancellation_poll.hpp"

namespace ps::plugin_internal::format_numeric {
__arm_locally_streaming std::uint64_t sme_conversion_vector_bytes() {
  return svcntb();
}

// Admit all samples before storing any result. Unsigned words outside +[0,1]
// set the high bit in words | (0x3f800000 - words). Four independent integer
// chains avoid per-vector predicate feedback; only the final admission
// branches. Binary32 * 255 is exactly representable in binary64. FRINTN gives
// ties-even before truncating the already integral result. The host owns the FP
// environment.
__arm_locally_streaming std::uint64_t sme_f32_u8_tile(
    const std::uint8_t* source, std::uint8_t* target, std::uint64_t count,
    const execution_internal::CancellationPoll& cancellation) {
  const auto flag_count = cancellation.size;
  const auto* flags = cancellation.flags.data();
  const auto cancelled = [&]() {
    for (std::size_t k = 0; k < flag_count; ++k)
      if (flags[k]->load(std::memory_order_acquire))
        return true;
    return false;
  };
  // Restrict dispatch to a 64-byte streaming vector: four word vectors admit
  // exactly 64 samples; four double vectors convert 32 samples per poll.
  if (svcntb() != 64)
    return 0;
  const auto pg = svptrue_b32();
  const auto width = svcntw();
  auto bad0 = svdup_u32(0), bad1 = svdup_u32(0), bad2 = svdup_u32(0),
       bad3 = svdup_u32(0);
  std::uint64_t i = 0;
  for (; count - i >= 4 * width; i += 4 * width) {
    if (cancelled())
      return 0;
    const auto w0 = svld1_u32(
        pg, reinterpret_cast<const std::uint32_t*>(source) + i + 0 * width);
    const auto flag0 =
        svorr_u32_x(pg, w0, svsub_u32_x(pg, svdup_u32(0x3f800000), w0));
    bad0 = svorr_u32_x(pg, bad0, flag0);
    const auto w1 = svld1_u32(
        pg, reinterpret_cast<const std::uint32_t*>(source) + i + 1 * width);
    const auto flag1 =
        svorr_u32_x(pg, w1, svsub_u32_x(pg, svdup_u32(0x3f800000), w1));
    bad1 = svorr_u32_x(pg, bad1, flag1);
    const auto w2 = svld1_u32(
        pg, reinterpret_cast<const std::uint32_t*>(source) + i + 2 * width);
    const auto flag2 =
        svorr_u32_x(pg, w2, svsub_u32_x(pg, svdup_u32(0x3f800000), w2));
    bad2 = svorr_u32_x(pg, bad2, flag2);
    const auto w3 = svld1_u32(
        pg, reinterpret_cast<const std::uint32_t*>(source) + i + 3 * width);
    const auto flag3 =
        svorr_u32_x(pg, w3, svsub_u32_x(pg, svdup_u32(0x3f800000), w3));
    bad3 = svorr_u32_x(pg, bad3, flag3);
  }
  for (; i < count; i += width) {
    if (cancelled())
      return 0;
    const auto tail = svwhilelt_b32_u64(i, count);
    const auto w0 =
        svld1_u32(tail, reinterpret_cast<const std::uint32_t*>(source) + i);
    const auto flag0 =
        svorr_u32_x(pg, w0, svsub_u32_x(pg, svdup_u32(0x3f800000), w0));
    bad0 = svorr_u32_x(pg, bad0, flag0);
  }
  const auto bad =
      svorr_u32_x(pg, svorr_u32_x(pg, bad0, bad1), svorr_u32_x(pg, bad2, bad3));
  if (svptest_any(pg, svcmplt_n_s32(pg, svreinterpret_s32_u32(bad), 0)))
    return 0;
  const auto pd = svptrue_b64();
  const auto lanes = svcntd();
  i = 0;
  for (; count - i >= 4 * lanes; i += 4 * lanes) {
    if (cancelled())
      return 0;
    const auto v0 = svcvt_f64_f32_x(
        pd, svreinterpret_f32_u64(
                svld1uw_u64(pd, reinterpret_cast<const std::uint32_t*>(source) +
                                    i + 0 * lanes)));
    const auto r0 =
        svcvt_u64_f64_x(pd, svrintn_f64_x(pd, svmul_n_f64_x(pd, v0, 255.0)));
    const auto v1 = svcvt_f64_f32_x(
        pd, svreinterpret_f32_u64(
                svld1uw_u64(pd, reinterpret_cast<const std::uint32_t*>(source) +
                                    i + 1 * lanes)));
    const auto r1 =
        svcvt_u64_f64_x(pd, svrintn_f64_x(pd, svmul_n_f64_x(pd, v1, 255.0)));
    const auto v2 = svcvt_f64_f32_x(
        pd, svreinterpret_f32_u64(
                svld1uw_u64(pd, reinterpret_cast<const std::uint32_t*>(source) +
                                    i + 2 * lanes)));
    const auto r2 =
        svcvt_u64_f64_x(pd, svrintn_f64_x(pd, svmul_n_f64_x(pd, v2, 255.0)));
    const auto v3 = svcvt_f64_f32_x(
        pd, svreinterpret_f32_u64(
                svld1uw_u64(pd, reinterpret_cast<const std::uint32_t*>(source) +
                                    i + 3 * lanes)));
    const auto r3 =
        svcvt_u64_f64_x(pd, svrintn_f64_x(pd, svmul_n_f64_x(pd, v3, 255.0)));
    svst1b_u64(pd, target + i + 0 * lanes, r0);
    svst1b_u64(pd, target + i + 1 * lanes, r1);
    svst1b_u64(pd, target + i + 2 * lanes, r2);
    svst1b_u64(pd, target + i + 3 * lanes, r3);
  }
  for (; i < count; i += lanes) {
    if (cancelled())
      return 0;
    const auto tail = svwhilelt_b64_u64(i, count);
    const auto value = svcvt_f64_f32_x(
        tail, svreinterpret_f32_u64(svld1uw_u64(
                  tail, reinterpret_cast<const std::uint32_t*>(source) + i)));
    svst1b_u64(
        tail, target + i,
        svcvt_u64_f64_x(
            tail, svrintn_f64_x(tail, svmul_n_f64_x(tail, value, 255.0))));
  }
  return count;
}

}  // namespace ps::plugin_internal::format_numeric
