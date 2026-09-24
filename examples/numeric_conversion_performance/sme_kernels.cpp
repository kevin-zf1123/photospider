// Experimental streaming-SVE kernels. Only this file enables SME instructions.
#include <arm_sme.h>

#include <cstdint>

__arm_locally_streaming std::uint64_t sme_u8_f32(const std::uint8_t* source,
                                                 std::uint8_t* target,
                                                 std::uint64_t count) {
  for (std::uint64_t i = 0; i < count; i += svcntw()) {
    const auto pg = svwhilelt_b32_u64(i, count);
    const auto codes = svld1ub_u32(pg, source + i);
    const auto values = svcvt_f32_u32_x(pg, codes);
    svst1_f32(pg, reinterpret_cast<float*>(target) + i,
              svdiv_n_f32_x(pg, values, 255.0f));
  }
  return count;
}

__arm_locally_streaming std::uint64_t sme_f32_u8(const std::uint8_t* source,
                                                 std::uint8_t* target,
                                                 std::uint64_t count) {
  for (std::uint64_t i = 0; i < count; i += svcntd()) {
    const auto pg = svwhilelt_b64_u64(i, count);
    // Widen raw words into the even float lanes consumed by FCVT to double.
    const auto words =
        svld1uw_u64(pg, reinterpret_cast<const std::uint32_t*>(source) + i);
    const auto values = svcvt_f64_f32_x(pg, svreinterpret_f32_u64(words));
    const auto valid = svand_b_z(pg, svcmpge_n_f64(pg, values, 0.0),
                                 svcmple_n_f64(pg, values, 1.0));
    if (svptest_any(pg, svnot_b_z(pg, valid)))
      return i;
    const auto exact = svmul_n_f64_x(pg, values, 255.0);
    const auto rounded = svrintn_f64_x(pg, exact);
    svst1b_u64(pg, target + i, svcvt_u64_f64_x(pg, rounded));
  }
  return count;
}

__arm_locally_streaming std::uint64_t sme_f64_f32(const std::uint8_t* source,
                                                  std::uint8_t* target,
                                                  std::uint64_t count) {
  for (std::uint64_t i = 0; i < count; i += svcntd()) {
    const auto pg = svwhilelt_b64_u64(i, count);
    const auto values =
        svld1_f64(pg, reinterpret_cast<const double*>(source) + i);
    const auto exponent = svand_n_u64_x(
        pg, svlsr_n_u64_x(pg, svreinterpret_u64_f64(values), 52), 2047);
    const auto valid = svand_b_z(pg, svcmpge_n_u64(pg, exponent, 897),
                                 svcmplt_n_u64(pg, exponent, 2047));
    if (svptest_any(pg, svnot_b_z(pg, valid)))
      return i;
    const auto result = svcvt_f32_f64_x(pg, values);
    const auto bits = svreinterpret_u64_f32(result);
    const auto result_exponent = svand_n_u64_x(pg, bits, 0x7f800000);
    if (svptest_any(pg, svcmpeq_n_u64(pg, result_exponent, 0x7f800000)))
      return i;
    svst1w_u64(pg, reinterpret_cast<std::uint32_t*>(target) + i, bits);
  }
  return count;
}

// One predicate-to-scalar decision per 64 samples. Source admission is
// complete before any output store, so a rejected block leaves no writes.
__arm_locally_streaming std::uint64_t sme_f32_u8_batched(
    const std::uint8_t* source, std::uint8_t* target, std::uint64_t count) {
  for (std::uint64_t base = 0; base < count; base += 64) {
    const auto end = count - base < 64 ? count : base + 64;
    auto bad = svpfalse_b();
    for (auto i = base; i < end; i += svcntw()) {
      const auto pg = svwhilelt_b32_u64(i, end);
      const auto words =
          svld1_u32(pg, reinterpret_cast<const std::uint32_t*>(source) + i);
      // Accept +[0,1] and -0. Nonfinite/negative/out-of-range words fail.
      const auto magnitude = svand_n_u32_x(pg, words, 0x7fffffff);
      const auto invalid = svand_b_z(pg, svcmpgt_n_u32(pg, words, 0x3f800000),
                                     svcmpne_n_u32(pg, magnitude, 0));
      bad = svorr_b_z(svptrue_b32(), bad, invalid);
    }
    if (svptest_any(svptrue_b32(), bad))
      return base;
    for (auto i = base; i < end; i += svcntd()) {
      const auto pg = svwhilelt_b64_u64(i, end);
      const auto words =
          svld1uw_u64(pg, reinterpret_cast<const std::uint32_t*>(source) + i);
      const auto values = svcvt_f64_f32_x(pg, svreinterpret_f32_u64(words));
      const auto exact = svmul_n_f64_x(pg, values, 255.0);
      svst1b_u64(pg, target + i, svcvt_u64_f64_x(pg, svrintn_f64_x(pg, exact)));
    }
  }
  return count;
}

__arm_locally_streaming std::uint64_t sme_f64_f32_batched(
    const std::uint8_t* source, std::uint8_t* target, std::uint64_t count) {
  for (std::uint64_t base = 0; base < count; base += 64) {
    const auto end = count - base < 64 ? count : base + 64;
    auto bad = svpfalse_b();
    for (auto i = base; i < end; i += svcntd()) {
      const auto pg = svwhilelt_b64_u64(i, end);
      const auto words =
          svld1_u64(pg, reinterpret_cast<const std::uint64_t*>(source) + i);
      const auto magnitude =
          svand_n_u64_x(pg, words, UINT64_C(0x7fffffffffffffff));
      // These source bounds prove a normal finite binary32 result under RN.
      // The upper bound is the midpoint that rounds to infinity (exclusive).
      const auto invalid = svorr_b_z(
          pg, svcmplt_n_u64(pg, magnitude, UINT64_C(0x3810000000000000)),
          svcmpge_n_u64(pg, magnitude, UINT64_C(0x47effffff0000000)));
      bad = svorr_b_z(svptrue_b64(), bad, invalid);
    }
    if (svptest_any(svptrue_b64(), bad))
      return base;
    for (auto i = base; i < end; i += svcntd()) {
      const auto pg = svwhilelt_b64_u64(i, end);
      const auto values =
          svld1_f64(pg, reinterpret_cast<const double*>(source) + i);
      const auto result = svcvt_f32_f64_x(pg, values);
      svst1w_u64(pg, reinterpret_cast<std::uint32_t*>(target) + i,
                 svreinterpret_u64_f32(result));
    }
  }
  return count;
}

__arm_locally_streaming std::uint64_t sme_u8_f32_unrolled(
    const std::uint8_t* source, std::uint8_t* target, std::uint64_t count) {
  const auto width = svcntw();
  std::uint64_t i = 0;
  for (; count - i >= 4 * width; i += 4 * width) {
    const auto p0 = svwhilelt_b32_u64(i, count);
    const auto p1 = svwhilelt_b32_u64(i + width, count);
    const auto p2 = svwhilelt_b32_u64(i + 2 * width, count);
    const auto p3 = svwhilelt_b32_u64(i + 3 * width, count);
    const auto a = svcvt_f32_u32_x(p0, svld1ub_u32(p0, source + i));
    const auto b = svcvt_f32_u32_x(p1, svld1ub_u32(p1, source + i + width));
    const auto c = svcvt_f32_u32_x(p2, svld1ub_u32(p2, source + i + 2 * width));
    const auto d = svcvt_f32_u32_x(p3, svld1ub_u32(p3, source + i + 3 * width));
    const auto r0 = svdiv_n_f32_x(p0, a, 255.0f);
    const auto r1 = svdiv_n_f32_x(p1, b, 255.0f);
    const auto r2 = svdiv_n_f32_x(p2, c, 255.0f);
    const auto r3 = svdiv_n_f32_x(p3, d, 255.0f);
    auto* output = reinterpret_cast<float*>(target) + i;
    svst1_f32(p0, output, r0);
    svst1_f32(p1, output + width, r1);
    svst1_f32(p2, output + 2 * width, r2);
    svst1_f32(p3, output + 3 * width, r3);
  }
  for (; i < count; i += width) {
    const auto pg = svwhilelt_b32_u64(i, count);
    const auto values = svcvt_f32_u32_x(pg, svld1ub_u32(pg, source + i));
    svst1_f32(pg, reinterpret_cast<float*>(target) + i,
              svdiv_n_f32_x(pg, values, 255.0f));
  }
  return count;
}

__arm_locally_streaming std::uint64_t sme_f32_u8_integer_flags(
    const std::uint8_t* source, std::uint8_t* target, std::uint64_t count) {
  for (std::uint64_t base = 0; base < count; base += 64) {
    const auto end = count - base < 64 ? count : base + 64;
    auto bad = svdup_u32(0);
    for (auto i = base; i < end; i += svcntw()) {
      const auto pg = svwhilelt_b32_u64(i, end);
      const auto words =
          svld1_u32(pg, reinterpret_cast<const std::uint32_t*>(source) + i);
      // A sign bit or unsigned underflow marks values outside +[0,1].
      const auto difference = svsub_u32_z(pg, svdup_u32(0x3f800000), words);
      const auto invalid = svorr_u32_z(pg, words, difference);
      bad = svorr_u32_x(svptrue_b32(), bad, invalid);
    }
    if (svptest_any(
            svptrue_b32(),
            svcmplt_n_s32(svptrue_b32(), svreinterpret_s32_u32(bad), 0)))
      return base;
    for (auto i = base; i < end; i += svcntd()) {
      const auto pg = svwhilelt_b64_u64(i, end);
      const auto words =
          svld1uw_u64(pg, reinterpret_cast<const std::uint32_t*>(source) + i);
      const auto values = svcvt_f64_f32_x(pg, svreinterpret_f32_u64(words));
      const auto exact = svmul_n_f64_x(pg, values, 255.0);
      svst1b_u64(pg, target + i, svcvt_u64_f64_x(pg, svrintn_f64_x(pg, exact)));
    }
  }
  return count;
}

__arm_locally_streaming std::uint64_t sme_f64_f32_integer_flags(
    const std::uint8_t* source, std::uint8_t* target, std::uint64_t count) {
  for (std::uint64_t base = 0; base < count; base += 64) {
    const auto end = count - base < 64 ? count : base + 64;
    auto bad = svdup_u64(0);
    for (auto i = base; i < end; i += svcntd()) {
      const auto pg = svwhilelt_b64_u64(i, end);
      const auto words =
          svld1_u64(pg, reinterpret_cast<const std::uint64_t*>(source) + i);
      const auto magnitude =
          svand_n_u64_x(pg, words, UINT64_C(0x7fffffffffffffff));
      // The signed subtraction ranges fit int64: a high bit in either
      // difference identifies a source outside the certified finite domain.
      const auto lower =
          svsub_n_u64_z(pg, magnitude, UINT64_C(0x3810000000000000));
      const auto upper =
          svsub_u64_z(pg, svdup_u64(UINT64_C(0x47efffffefffffff)), magnitude);
      bad = svorr_u64_x(svptrue_b64(), bad, svorr_u64_z(pg, lower, upper));
    }
    if (svptest_any(
            svptrue_b64(),
            svcmplt_n_s64(svptrue_b64(), svreinterpret_s64_u64(bad), 0)))
      return base;
    for (auto i = base; i < end; i += svcntd()) {
      const auto pg = svwhilelt_b64_u64(i, end);
      const auto values =
          svld1_f64(pg, reinterpret_cast<const double*>(source) + i);
      const auto result = svcvt_f32_f64_x(pg, values);
      svst1w_u64(pg, reinterpret_cast<std::uint32_t*>(target) + i,
                 svreinterpret_u64_f32(result));
    }
  }
  return count;
}

__arm_locally_streaming std::uint64_t sme_vector_bytes() {
  return svcntb();
}

// Admitted +[0,1] binary32 inputs. Exact M*255 fits uint32; variable shifts
// implement ties-even without floating intermediates or predicate feedback.
__arm_locally_streaming std::uint64_t sme_f32_u8_integer_unchecked(
    const std::uint8_t* source, std::uint8_t* target, std::uint64_t count) {
  for (std::uint64_t i = 0; i < count; i += svcntw()) {
    const auto pg = svwhilelt_b32_u64(i, count);
    const auto bits =
        svld1_u32(pg, reinterpret_cast<const std::uint32_t*>(source) + i);
    const auto exponent = svlsr_n_u32_x(pg, bits, 23);
    const auto significand =
        svorr_n_u32_x(pg, svand_n_u32_x(pg, bits, 0x7fffff), 0x800000);
    const auto product =
        svsub_u32_x(pg, svlsl_n_u32_x(pg, significand, 8), significand);
    const auto shift = svsub_u32_x(pg, svdup_u32(149), exponent);
    const auto head = svlsr_u32_x(pg, product, shift);
    const auto quotient = svlsr_n_u32_x(pg, head, 1);
    const auto mask =
        svsub_n_u32_x(pg, svlsl_u32_x(pg, svdup_u32(1), shift), 1);
    const auto tail = svand_u32_x(pg, product, mask);
    const auto sticky = svlsr_n_u32_x(
        pg, svorr_u32_x(pg, tail, svsub_u32_x(pg, svdup_u32(0), tail)), 31);
    const auto increment = svand_n_u32_x(
        pg, svand_u32_x(pg, head, svorr_u32_x(pg, sticky, quotient)), 1);
    svst1b_u32(pg, target + i, svadd_u32_x(pg, quotient, increment));
  }
  return count;
}

__arm_locally_streaming std::uint64_t sme_f64_f32_unchecked(
    const std::uint8_t* source, std::uint8_t* target, std::uint64_t count) {
  for (std::uint64_t i = 0; i < count; i += svcntd()) {
    const auto pg = svwhilelt_b64_u64(i, count);
    const auto values =
        svld1_f64(pg, reinterpret_cast<const double*>(source) + i);
    const auto result = svcvt_f32_f64_x(pg, values);
    svst1w_u64(pg, reinterpret_cast<std::uint32_t*>(target) + i,
               svreinterpret_u64_f32(result));
  }
  return count;
}

__arm_locally_streaming std::uint64_t sme_f32_u8_scheduled(
    const std::uint8_t* source, std::uint8_t* target, std::uint64_t count) {
  const auto lanes = svcntd();
  std::uint64_t base = 0;
  const auto pg = svptrue_b64();
  for (; count - base >= lanes * 8; base += lanes * 8) {
    const auto w0 = svld1uw_u64(
        pg, reinterpret_cast<const std::uint32_t*>(source) + base + 0 * lanes);
    const auto v0 = svcvt_f64_f32_x(pg, svreinterpret_f32_u64(w0));
    const auto flag0 =
        svorr_u64_x(pg, w0, svsub_u64_x(pg, svdup_u64(0x3f800000), w0));
    const auto w1 = svld1uw_u64(
        pg, reinterpret_cast<const std::uint32_t*>(source) + base + 1 * lanes);
    const auto v1 = svcvt_f64_f32_x(pg, svreinterpret_f32_u64(w1));
    const auto flag1 =
        svorr_u64_x(pg, w1, svsub_u64_x(pg, svdup_u64(0x3f800000), w1));
    const auto w2 = svld1uw_u64(
        pg, reinterpret_cast<const std::uint32_t*>(source) + base + 2 * lanes);
    const auto v2 = svcvt_f64_f32_x(pg, svreinterpret_f32_u64(w2));
    const auto flag2 =
        svorr_u64_x(pg, w2, svsub_u64_x(pg, svdup_u64(0x3f800000), w2));
    const auto w3 = svld1uw_u64(
        pg, reinterpret_cast<const std::uint32_t*>(source) + base + 3 * lanes);
    const auto v3 = svcvt_f64_f32_x(pg, svreinterpret_f32_u64(w3));
    const auto flag3 =
        svorr_u64_x(pg, w3, svsub_u64_x(pg, svdup_u64(0x3f800000), w3));
    const auto w4 = svld1uw_u64(
        pg, reinterpret_cast<const std::uint32_t*>(source) + base + 4 * lanes);
    const auto v4 = svcvt_f64_f32_x(pg, svreinterpret_f32_u64(w4));
    const auto flag4 =
        svorr_u64_x(pg, w4, svsub_u64_x(pg, svdup_u64(0x3f800000), w4));
    const auto w5 = svld1uw_u64(
        pg, reinterpret_cast<const std::uint32_t*>(source) + base + 5 * lanes);
    const auto v5 = svcvt_f64_f32_x(pg, svreinterpret_f32_u64(w5));
    const auto flag5 =
        svorr_u64_x(pg, w5, svsub_u64_x(pg, svdup_u64(0x3f800000), w5));
    const auto w6 = svld1uw_u64(
        pg, reinterpret_cast<const std::uint32_t*>(source) + base + 6 * lanes);
    const auto v6 = svcvt_f64_f32_x(pg, svreinterpret_f32_u64(w6));
    const auto flag6 =
        svorr_u64_x(pg, w6, svsub_u64_x(pg, svdup_u64(0x3f800000), w6));
    const auto w7 = svld1uw_u64(
        pg, reinterpret_cast<const std::uint32_t*>(source) + base + 7 * lanes);
    const auto v7 = svcvt_f64_f32_x(pg, svreinterpret_f32_u64(w7));
    const auto flag7 =
        svorr_u64_x(pg, w7, svsub_u64_x(pg, svdup_u64(0x3f800000), w7));
    const auto bad = svorr_u64_x(pg,
                                 svorr_u64_x(pg, svorr_u64_x(pg, flag0, flag1),
                                             svorr_u64_x(pg, flag2, flag3)),
                                 svorr_u64_x(pg, svorr_u64_x(pg, flag4, flag5),
                                             svorr_u64_x(pg, flag6, flag7)));
    const auto r0 =
        svcvt_u64_f64_x(pg, svrintn_f64_x(pg, svmul_n_f64_x(pg, v0, 255.0)));
    const auto r1 =
        svcvt_u64_f64_x(pg, svrintn_f64_x(pg, svmul_n_f64_x(pg, v1, 255.0)));
    const auto r2 =
        svcvt_u64_f64_x(pg, svrintn_f64_x(pg, svmul_n_f64_x(pg, v2, 255.0)));
    const auto r3 =
        svcvt_u64_f64_x(pg, svrintn_f64_x(pg, svmul_n_f64_x(pg, v3, 255.0)));
    const auto r4 =
        svcvt_u64_f64_x(pg, svrintn_f64_x(pg, svmul_n_f64_x(pg, v4, 255.0)));
    const auto r5 =
        svcvt_u64_f64_x(pg, svrintn_f64_x(pg, svmul_n_f64_x(pg, v5, 255.0)));
    const auto r6 =
        svcvt_u64_f64_x(pg, svrintn_f64_x(pg, svmul_n_f64_x(pg, v6, 255.0)));
    const auto r7 =
        svcvt_u64_f64_x(pg, svrintn_f64_x(pg, svmul_n_f64_x(pg, v7, 255.0)));
    if (svptest_any(pg, svcmplt_n_s64(pg, svreinterpret_s64_u64(bad), 0)))
      return base;
    svst1b_u64(pg, target + base + 0 * lanes, r0);
    svst1b_u64(pg, target + base + 1 * lanes, r1);
    svst1b_u64(pg, target + base + 2 * lanes, r2);
    svst1b_u64(pg, target + base + 3 * lanes, r3);
    svst1b_u64(pg, target + base + 4 * lanes, r4);
    svst1b_u64(pg, target + base + 5 * lanes, r5);
    svst1b_u64(pg, target + base + 6 * lanes, r6);
    svst1b_u64(pg, target + base + 7 * lanes, r7);
  }
  return base;
}

__arm_locally_streaming std::uint64_t sme_f64_f32_scheduled(
    const std::uint8_t* source, std::uint8_t* target, std::uint64_t count) {
  const auto lanes = svcntd();
  std::uint64_t base = 0;
  const auto pg = svptrue_b64();
  for (; count - base >= lanes * 8; base += lanes * 8) {
    const auto v0 = svld1_f64(
        pg, reinterpret_cast<const double*>(source) + base + 0 * lanes);
    const auto m0 = svand_n_u64_x(pg, svreinterpret_u64_f64(v0),
                                  UINT64_C(0x7fffffffffffffff));
    const auto flag0 = svorr_u64_x(
        pg, svsub_n_u64_x(pg, m0, UINT64_C(0x3810000000000000)),
        svsub_u64_x(pg, svdup_u64(UINT64_C(0x47efffffefffffff)), m0));
    const auto v1 = svld1_f64(
        pg, reinterpret_cast<const double*>(source) + base + 1 * lanes);
    const auto m1 = svand_n_u64_x(pg, svreinterpret_u64_f64(v1),
                                  UINT64_C(0x7fffffffffffffff));
    const auto flag1 = svorr_u64_x(
        pg, svsub_n_u64_x(pg, m1, UINT64_C(0x3810000000000000)),
        svsub_u64_x(pg, svdup_u64(UINT64_C(0x47efffffefffffff)), m1));
    const auto v2 = svld1_f64(
        pg, reinterpret_cast<const double*>(source) + base + 2 * lanes);
    const auto m2 = svand_n_u64_x(pg, svreinterpret_u64_f64(v2),
                                  UINT64_C(0x7fffffffffffffff));
    const auto flag2 = svorr_u64_x(
        pg, svsub_n_u64_x(pg, m2, UINT64_C(0x3810000000000000)),
        svsub_u64_x(pg, svdup_u64(UINT64_C(0x47efffffefffffff)), m2));
    const auto v3 = svld1_f64(
        pg, reinterpret_cast<const double*>(source) + base + 3 * lanes);
    const auto m3 = svand_n_u64_x(pg, svreinterpret_u64_f64(v3),
                                  UINT64_C(0x7fffffffffffffff));
    const auto flag3 = svorr_u64_x(
        pg, svsub_n_u64_x(pg, m3, UINT64_C(0x3810000000000000)),
        svsub_u64_x(pg, svdup_u64(UINT64_C(0x47efffffefffffff)), m3));
    const auto v4 = svld1_f64(
        pg, reinterpret_cast<const double*>(source) + base + 4 * lanes);
    const auto m4 = svand_n_u64_x(pg, svreinterpret_u64_f64(v4),
                                  UINT64_C(0x7fffffffffffffff));
    const auto flag4 = svorr_u64_x(
        pg, svsub_n_u64_x(pg, m4, UINT64_C(0x3810000000000000)),
        svsub_u64_x(pg, svdup_u64(UINT64_C(0x47efffffefffffff)), m4));
    const auto v5 = svld1_f64(
        pg, reinterpret_cast<const double*>(source) + base + 5 * lanes);
    const auto m5 = svand_n_u64_x(pg, svreinterpret_u64_f64(v5),
                                  UINT64_C(0x7fffffffffffffff));
    const auto flag5 = svorr_u64_x(
        pg, svsub_n_u64_x(pg, m5, UINT64_C(0x3810000000000000)),
        svsub_u64_x(pg, svdup_u64(UINT64_C(0x47efffffefffffff)), m5));
    const auto v6 = svld1_f64(
        pg, reinterpret_cast<const double*>(source) + base + 6 * lanes);
    const auto m6 = svand_n_u64_x(pg, svreinterpret_u64_f64(v6),
                                  UINT64_C(0x7fffffffffffffff));
    const auto flag6 = svorr_u64_x(
        pg, svsub_n_u64_x(pg, m6, UINT64_C(0x3810000000000000)),
        svsub_u64_x(pg, svdup_u64(UINT64_C(0x47efffffefffffff)), m6));
    const auto v7 = svld1_f64(
        pg, reinterpret_cast<const double*>(source) + base + 7 * lanes);
    const auto m7 = svand_n_u64_x(pg, svreinterpret_u64_f64(v7),
                                  UINT64_C(0x7fffffffffffffff));
    const auto flag7 = svorr_u64_x(
        pg, svsub_n_u64_x(pg, m7, UINT64_C(0x3810000000000000)),
        svsub_u64_x(pg, svdup_u64(UINT64_C(0x47efffffefffffff)), m7));
    const auto bad = svorr_u64_x(pg,
                                 svorr_u64_x(pg, svorr_u64_x(pg, flag0, flag1),
                                             svorr_u64_x(pg, flag2, flag3)),
                                 svorr_u64_x(pg, svorr_u64_x(pg, flag4, flag5),
                                             svorr_u64_x(pg, flag6, flag7)));
    const auto r0 = svreinterpret_u64_f32(svcvt_f32_f64_x(pg, v0));
    const auto r1 = svreinterpret_u64_f32(svcvt_f32_f64_x(pg, v1));
    const auto r2 = svreinterpret_u64_f32(svcvt_f32_f64_x(pg, v2));
    const auto r3 = svreinterpret_u64_f32(svcvt_f32_f64_x(pg, v3));
    const auto r4 = svreinterpret_u64_f32(svcvt_f32_f64_x(pg, v4));
    const auto r5 = svreinterpret_u64_f32(svcvt_f32_f64_x(pg, v5));
    const auto r6 = svreinterpret_u64_f32(svcvt_f32_f64_x(pg, v6));
    const auto r7 = svreinterpret_u64_f32(svcvt_f32_f64_x(pg, v7));
    if (svptest_any(pg, svcmplt_n_s64(pg, svreinterpret_s64_u64(bad), 0)))
      return base;
    svst1w_u64(pg, reinterpret_cast<std::uint32_t*>(target) + base + 0 * lanes,
               r0);
    svst1w_u64(pg, reinterpret_cast<std::uint32_t*>(target) + base + 1 * lanes,
               r1);
    svst1w_u64(pg, reinterpret_cast<std::uint32_t*>(target) + base + 2 * lanes,
               r2);
    svst1w_u64(pg, reinterpret_cast<std::uint32_t*>(target) + base + 3 * lanes,
               r3);
    svst1w_u64(pg, reinterpret_cast<std::uint32_t*>(target) + base + 4 * lanes,
               r4);
    svst1w_u64(pg, reinterpret_cast<std::uint32_t*>(target) + base + 5 * lanes,
               r5);
    svst1w_u64(pg, reinterpret_cast<std::uint32_t*>(target) + base + 6 * lanes,
               r6);
    svst1w_u64(pg, reinterpret_cast<std::uint32_t*>(target) + base + 7 * lanes,
               r7);
  }
  return base;
}
__arm_locally_streaming std::uint64_t sme_f32_u8_span_flags(
    const std::uint8_t* source, std::uint8_t* target, std::uint64_t count) {
  if (count) {
    const std::uint64_t base = 0, end = count;
    auto bad = svdup_u32(0);
    for (auto i = base; i < end; i += svcntw()) {
      const auto pg = svwhilelt_b32_u64(i, end);
      const auto words =
          svld1_u32(pg, reinterpret_cast<const std::uint32_t*>(source) + i);
      // A sign bit or unsigned underflow marks values outside +[0,1].
      const auto difference = svsub_u32_z(pg, svdup_u32(0x3f800000), words);
      const auto invalid = svorr_u32_z(pg, words, difference);
      bad = svorr_u32_x(svptrue_b32(), bad, invalid);
    }
    if (svptest_any(
            svptrue_b32(),
            svcmplt_n_s32(svptrue_b32(), svreinterpret_s32_u32(bad), 0)))
      return base;
    for (auto i = base; i < end; i += svcntd()) {
      const auto pg = svwhilelt_b64_u64(i, end);
      const auto words =
          svld1uw_u64(pg, reinterpret_cast<const std::uint32_t*>(source) + i);
      const auto values = svcvt_f64_f32_x(pg, svreinterpret_f32_u64(words));
      const auto exact = svmul_n_f64_x(pg, values, 255.0);
      svst1b_u64(pg, target + i, svcvt_u64_f64_x(pg, svrintn_f64_x(pg, exact)));
    }
  }
  return count;
}

__arm_locally_streaming std::uint64_t sme_f64_f32_span_flags(
    const std::uint8_t* source, std::uint8_t* target, std::uint64_t count) {
  if (count) {
    const std::uint64_t base = 0, end = count;
    auto bad = svdup_u64(0);
    for (auto i = base; i < end; i += svcntd()) {
      const auto pg = svwhilelt_b64_u64(i, end);
      const auto words =
          svld1_u64(pg, reinterpret_cast<const std::uint64_t*>(source) + i);
      const auto magnitude =
          svand_n_u64_x(pg, words, UINT64_C(0x7fffffffffffffff));
      // The signed subtraction ranges fit int64: a high bit in either
      // difference identifies a source outside the certified finite domain.
      const auto lower =
          svsub_n_u64_z(pg, magnitude, UINT64_C(0x3810000000000000));
      const auto upper =
          svsub_u64_z(pg, svdup_u64(UINT64_C(0x47efffffefffffff)), magnitude);
      bad = svorr_u64_x(svptrue_b64(), bad, svorr_u64_z(pg, lower, upper));
    }
    if (svptest_any(
            svptrue_b64(),
            svcmplt_n_s64(svptrue_b64(), svreinterpret_s64_u64(bad), 0)))
      return base;
    for (auto i = base; i < end; i += svcntd()) {
      const auto pg = svwhilelt_b64_u64(i, end);
      const auto values =
          svld1_f64(pg, reinterpret_cast<const double*>(source) + i);
      const auto result = svcvt_f32_f64_x(pg, values);
      svst1w_u64(pg, reinterpret_cast<std::uint32_t*>(target) + i,
                 svreinterpret_u64_f32(result));
    }
  }
  return count;
}

__arm_locally_streaming std::uint64_t sme_u8_f32_fma(const std::uint8_t* source,
                                                     std::uint8_t* target,
                                                     std::uint64_t count) {
  for (std::uint64_t i = 0; i < count; i += svcntw()) {
    const auto pg = svwhilelt_b32_u64(i, count);
    const auto value = svcvt_f32_u32_x(pg, svld1ub_u32(pg, source + i));
    // Rounded reciprocal plus its binary32 residual. Exhaustively checked
    // against exact i/255 rounding for the complete 256-code domain.
    const auto residual = svmul_n_f32_x(pg, value, -0x1.fdfdfe0000000p-33f);
    const auto result =
        svmla_n_f32_x(pg, residual, value, 0x1.0101020000000p-8f);
    svst1_f32(pg, reinterpret_cast<float*>(target) + i, result);
  }
  return count;
}

__arm_locally_streaming std::uint64_t sme_f32_u8_throughput(
    const std::uint8_t* source, std::uint8_t* target, std::uint64_t count) {
  const auto pg = svptrue_b32();
  const auto width = svcntw();
  auto bad0 = svdup_u32(0), bad1 = svdup_u32(0), bad2 = svdup_u32(0),
       bad3 = svdup_u32(0);
  std::uint64_t i = 0;
  for (; count - i >= 4 * width; i += 4 * width) {
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

__arm_locally_streaming std::uint64_t sme_f64_f32_throughput(
    const std::uint8_t* source, std::uint8_t* target, std::uint64_t count) {
  const auto pg = svptrue_b64();
  const auto width = svcntd();
  auto bad0 = svdup_u64(0), bad1 = svdup_u64(0), bad2 = svdup_u64(0),
       bad3 = svdup_u64(0);
  std::uint64_t i = 0;
  for (; count - i >= 4 * width; i += 4 * width) {
    const auto w0 = svld1_u64(
        pg, reinterpret_cast<const std::uint64_t*>(source) + i + 0 * width);
    const auto m0 = svand_n_u64_x(pg, w0, UINT64_C(0x7fffffffffffffff));
    const auto flag0 = svorr_u64_x(
        pg, svsub_n_u64_x(pg, m0, UINT64_C(0x3810000000000000)),
        svsub_u64_x(pg, svdup_u64(UINT64_C(0x47efffffefffffff)), m0));
    bad0 = svorr_u64_x(pg, bad0, flag0);
    const auto w1 = svld1_u64(
        pg, reinterpret_cast<const std::uint64_t*>(source) + i + 1 * width);
    const auto m1 = svand_n_u64_x(pg, w1, UINT64_C(0x7fffffffffffffff));
    const auto flag1 = svorr_u64_x(
        pg, svsub_n_u64_x(pg, m1, UINT64_C(0x3810000000000000)),
        svsub_u64_x(pg, svdup_u64(UINT64_C(0x47efffffefffffff)), m1));
    bad1 = svorr_u64_x(pg, bad1, flag1);
    const auto w2 = svld1_u64(
        pg, reinterpret_cast<const std::uint64_t*>(source) + i + 2 * width);
    const auto m2 = svand_n_u64_x(pg, w2, UINT64_C(0x7fffffffffffffff));
    const auto flag2 = svorr_u64_x(
        pg, svsub_n_u64_x(pg, m2, UINT64_C(0x3810000000000000)),
        svsub_u64_x(pg, svdup_u64(UINT64_C(0x47efffffefffffff)), m2));
    bad2 = svorr_u64_x(pg, bad2, flag2);
    const auto w3 = svld1_u64(
        pg, reinterpret_cast<const std::uint64_t*>(source) + i + 3 * width);
    const auto m3 = svand_n_u64_x(pg, w3, UINT64_C(0x7fffffffffffffff));
    const auto flag3 = svorr_u64_x(
        pg, svsub_n_u64_x(pg, m3, UINT64_C(0x3810000000000000)),
        svsub_u64_x(pg, svdup_u64(UINT64_C(0x47efffffefffffff)), m3));
    bad3 = svorr_u64_x(pg, bad3, flag3);
  }
  for (; i < count; i += width) {
    const auto tail = svwhilelt_b64_u64(i, count);
    const auto w0 =
        svld1_u64(tail, reinterpret_cast<const std::uint64_t*>(source) + i);
    const auto m0 = svand_n_u64_x(
        pg, svsel_u64(tail, w0, svdup_u64(UINT64_C(0x3810000000000000))),
        UINT64_C(0x7fffffffffffffff));
    const auto flag0 = svorr_u64_x(
        pg, svsub_n_u64_x(pg, m0, UINT64_C(0x3810000000000000)),
        svsub_u64_x(pg, svdup_u64(UINT64_C(0x47efffffefffffff)), m0));
    bad0 = svorr_u64_x(pg, bad0, flag0);
  }
  const auto bad =
      svorr_u64_x(pg, svorr_u64_x(pg, bad0, bad1), svorr_u64_x(pg, bad2, bad3));
  if (svptest_any(pg, svcmplt_n_s64(pg, svreinterpret_s64_u64(bad), 0)))
    return 0;
  const auto pd = svptrue_b64();
  const auto lanes = svcntd();
  i = 0;
  for (; count - i >= 4 * lanes; i += 4 * lanes) {
    const auto r0 = svreinterpret_u64_f32(svcvt_f32_f64_x(
        pd, svld1_f64(
                pd, reinterpret_cast<const double*>(source) + i + 0 * lanes)));
    const auto r1 = svreinterpret_u64_f32(svcvt_f32_f64_x(
        pd, svld1_f64(
                pd, reinterpret_cast<const double*>(source) + i + 1 * lanes)));
    const auto r2 = svreinterpret_u64_f32(svcvt_f32_f64_x(
        pd, svld1_f64(
                pd, reinterpret_cast<const double*>(source) + i + 2 * lanes)));
    const auto r3 = svreinterpret_u64_f32(svcvt_f32_f64_x(
        pd, svld1_f64(
                pd, reinterpret_cast<const double*>(source) + i + 3 * lanes)));
    svst1w_u64(pd, reinterpret_cast<std::uint32_t*>(target) + i + 0 * lanes,
               r0);
    svst1w_u64(pd, reinterpret_cast<std::uint32_t*>(target) + i + 1 * lanes,
               r1);
    svst1w_u64(pd, reinterpret_cast<std::uint32_t*>(target) + i + 2 * lanes,
               r2);
    svst1w_u64(pd, reinterpret_cast<std::uint32_t*>(target) + i + 3 * lanes,
               r3);
  }
  for (; i < count; i += lanes) {
    const auto tail = svwhilelt_b64_u64(i, count);
    const auto value =
        svld1_f64(tail, reinterpret_cast<const double*>(source) + i);
    svst1w_u64(tail, reinterpret_cast<std::uint32_t*>(target) + i,
               svreinterpret_u64_f32(svcvt_f32_f64_x(tail, value)));
  }
  return count;
}
__arm_locally_streaming std::uint64_t sme_f32_u8_bits(
    const std::uint8_t* source, std::uint8_t* target, std::uint64_t count) {
  const auto pg = svptrue_b32();
  const auto width = svcntw();
  auto bad0 = svdup_u32(0), bad1 = svdup_u32(0), bad2 = svdup_u32(0),
       bad3 = svdup_u32(0);
  std::uint64_t i = 0;
  for (; count - i >= 4 * width; i += 4 * width) {
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
  i = 0;
  for (; i < count; i += width) {
    const auto tail = svwhilelt_b32_u64(i, count);
    const auto words =
        svld1_u32(tail, reinterpret_cast<const std::uint32_t*>(source) + i);
    const auto exponent = svlsr_n_u32_x(pg, words, 23);
    const auto significand =
        svorr_n_u32_x(pg, svand_n_u32_x(pg, words, 0x7fffff), 0x800000);
    const auto product =
        svsub_u32_x(pg, svlsl_n_u32_x(pg, significand, 8), significand);
    const auto shift = svsub_u32_x(pg, svdup_u32(149), exponent);
    const auto head = svlsr_u32_x(pg, product, shift);
    const auto quotient = svlsr_n_u32_x(pg, head, 1);
    const auto mask =
        svsub_n_u32_x(pg, svlsl_u32_x(pg, svdup_u32(1), shift), 1);
    const auto low = svand_u32_x(pg, product, mask);
    const auto sticky = svlsr_n_u32_x(
        pg, svorr_u32_x(pg, low, svsub_u32_x(pg, svdup_u32(0), low)), 31);
    const auto increment = svand_n_u32_x(
        pg, svand_u32_x(pg, head, svorr_u32_x(pg, sticky, quotient)), 1);
    svst1b_u32(tail, target + i, svadd_u32_x(pg, quotient, increment));
  }
  return count;
}
__arm_locally_streaming std::uint64_t sme_f64_f32_bits(
    const std::uint8_t* source, std::uint8_t* target, std::uint64_t count) {
  const auto pg = svptrue_b64();
  const auto width = svcntd();
  auto bad0 = svdup_u64(0), bad1 = svdup_u64(0), bad2 = svdup_u64(0),
       bad3 = svdup_u64(0);
  std::uint64_t i = 0;
  for (; count - i >= 4 * width; i += 4 * width) {
    const auto w0 = svld1_u64(
        pg, reinterpret_cast<const std::uint64_t*>(source) + i + 0 * width);
    const auto m0 = svand_n_u64_x(pg, w0, UINT64_C(0x7fffffffffffffff));
    const auto flag0 = svorr_u64_x(
        pg, svsub_n_u64_x(pg, m0, UINT64_C(0x3810000000000000)),
        svsub_u64_x(pg, svdup_u64(UINT64_C(0x47efffffefffffff)), m0));
    bad0 = svorr_u64_x(pg, bad0, flag0);
    const auto w1 = svld1_u64(
        pg, reinterpret_cast<const std::uint64_t*>(source) + i + 1 * width);
    const auto m1 = svand_n_u64_x(pg, w1, UINT64_C(0x7fffffffffffffff));
    const auto flag1 = svorr_u64_x(
        pg, svsub_n_u64_x(pg, m1, UINT64_C(0x3810000000000000)),
        svsub_u64_x(pg, svdup_u64(UINT64_C(0x47efffffefffffff)), m1));
    bad1 = svorr_u64_x(pg, bad1, flag1);
    const auto w2 = svld1_u64(
        pg, reinterpret_cast<const std::uint64_t*>(source) + i + 2 * width);
    const auto m2 = svand_n_u64_x(pg, w2, UINT64_C(0x7fffffffffffffff));
    const auto flag2 = svorr_u64_x(
        pg, svsub_n_u64_x(pg, m2, UINT64_C(0x3810000000000000)),
        svsub_u64_x(pg, svdup_u64(UINT64_C(0x47efffffefffffff)), m2));
    bad2 = svorr_u64_x(pg, bad2, flag2);
    const auto w3 = svld1_u64(
        pg, reinterpret_cast<const std::uint64_t*>(source) + i + 3 * width);
    const auto m3 = svand_n_u64_x(pg, w3, UINT64_C(0x7fffffffffffffff));
    const auto flag3 = svorr_u64_x(
        pg, svsub_n_u64_x(pg, m3, UINT64_C(0x3810000000000000)),
        svsub_u64_x(pg, svdup_u64(UINT64_C(0x47efffffefffffff)), m3));
    bad3 = svorr_u64_x(pg, bad3, flag3);
  }
  for (; i < count; i += width) {
    const auto tail = svwhilelt_b64_u64(i, count);
    const auto w0 =
        svld1_u64(tail, reinterpret_cast<const std::uint64_t*>(source) + i);
    const auto m0 = svand_n_u64_x(
        pg, svsel_u64(tail, w0, svdup_u64(UINT64_C(0x3810000000000000))),
        UINT64_C(0x7fffffffffffffff));
    const auto flag0 = svorr_u64_x(
        pg, svsub_n_u64_x(pg, m0, UINT64_C(0x3810000000000000)),
        svsub_u64_x(pg, svdup_u64(UINT64_C(0x47efffffefffffff)), m0));
    bad0 = svorr_u64_x(pg, bad0, flag0);
  }
  const auto bad =
      svorr_u64_x(pg, svorr_u64_x(pg, bad0, bad1), svorr_u64_x(pg, bad2, bad3));
  if (svptest_any(pg, svcmplt_n_s64(pg, svreinterpret_s64_u64(bad), 0)))
    return 0;
  i = 0;
  for (; i < count; i += width) {
    const auto tail = svwhilelt_b64_u64(i, count);
    const auto words =
        svld1_u64(tail, reinterpret_cast<const std::uint64_t*>(source) + i);
    const auto magnitude =
        svand_n_u64_x(pg, words, UINT64_C(0x7fffffffffffffff));
    const auto odd = svand_n_u64_x(pg, svlsr_n_u64_x(pg, magnitude, 29), 1);
    const auto biased = svadd_u64_x(
        pg, magnitude, svadd_n_u64_x(pg, odd, (UINT64_C(1) << 28) - 1));
    const auto narrowed =
        svsub_n_u64_x(pg, svlsr_n_u64_x(pg, biased, 29), UINT64_C(896) << 23);
    const auto sign =
        svand_n_u64_x(pg, svlsr_n_u64_x(pg, words, 32), UINT64_C(0x80000000));
    svst1w_u64(tail, reinterpret_cast<std::uint32_t*>(target) + i,
               svorr_u64_x(pg, narrowed, sign));
  }
  return count;
}
__arm_locally_streaming std::uint64_t sme_f32_u8_bits_unrolled(
    const std::uint8_t* source, std::uint8_t* target, std::uint64_t count) {
  const auto pg = svptrue_b32();
  const auto width = svcntw();
  auto bad0 = svdup_u32(0), bad1 = svdup_u32(0), bad2 = svdup_u32(0),
       bad3 = svdup_u32(0);
  std::uint64_t i = 0;
  for (; count - i >= 4 * width; i += 4 * width) {
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
  i = 0;
  for (; count - i >= 4 * width; i += 4 * width) {
    const auto words0 = svld1_u32(
        pg, reinterpret_cast<const std::uint32_t*>(source) + i + 0 * width);
    const auto words1 = svld1_u32(
        pg, reinterpret_cast<const std::uint32_t*>(source) + i + 1 * width);
    const auto words2 = svld1_u32(
        pg, reinterpret_cast<const std::uint32_t*>(source) + i + 2 * width);
    const auto words3 = svld1_u32(
        pg, reinterpret_cast<const std::uint32_t*>(source) + i + 3 * width);
    const auto exponent0 = svlsr_n_u32_x(pg, words0, 23);
    const auto exponent1 = svlsr_n_u32_x(pg, words1, 23);
    const auto exponent2 = svlsr_n_u32_x(pg, words2, 23);
    const auto exponent3 = svlsr_n_u32_x(pg, words3, 23);
    const auto significand0 =
        svorr_n_u32_x(pg, svand_n_u32_x(pg, words0, 0x7fffff), 0x800000);
    const auto significand1 =
        svorr_n_u32_x(pg, svand_n_u32_x(pg, words1, 0x7fffff), 0x800000);
    const auto significand2 =
        svorr_n_u32_x(pg, svand_n_u32_x(pg, words2, 0x7fffff), 0x800000);
    const auto significand3 =
        svorr_n_u32_x(pg, svand_n_u32_x(pg, words3, 0x7fffff), 0x800000);
    const auto product0 =
        svsub_u32_x(pg, svlsl_n_u32_x(pg, significand0, 8), significand0);
    const auto product1 =
        svsub_u32_x(pg, svlsl_n_u32_x(pg, significand1, 8), significand1);
    const auto product2 =
        svsub_u32_x(pg, svlsl_n_u32_x(pg, significand2, 8), significand2);
    const auto product3 =
        svsub_u32_x(pg, svlsl_n_u32_x(pg, significand3, 8), significand3);
    const auto shift0 = svsub_u32_x(pg, svdup_u32(149), exponent0);
    const auto shift1 = svsub_u32_x(pg, svdup_u32(149), exponent1);
    const auto shift2 = svsub_u32_x(pg, svdup_u32(149), exponent2);
    const auto shift3 = svsub_u32_x(pg, svdup_u32(149), exponent3);
    const auto head0 = svlsr_u32_x(pg, product0, shift0);
    const auto head1 = svlsr_u32_x(pg, product1, shift1);
    const auto head2 = svlsr_u32_x(pg, product2, shift2);
    const auto head3 = svlsr_u32_x(pg, product3, shift3);
    const auto quotient0 = svlsr_n_u32_x(pg, head0, 1);
    const auto quotient1 = svlsr_n_u32_x(pg, head1, 1);
    const auto quotient2 = svlsr_n_u32_x(pg, head2, 1);
    const auto quotient3 = svlsr_n_u32_x(pg, head3, 1);
    const auto mask0 =
        svsub_n_u32_x(pg, svlsl_u32_x(pg, svdup_u32(1), shift0), 1);
    const auto mask1 =
        svsub_n_u32_x(pg, svlsl_u32_x(pg, svdup_u32(1), shift1), 1);
    const auto mask2 =
        svsub_n_u32_x(pg, svlsl_u32_x(pg, svdup_u32(1), shift2), 1);
    const auto mask3 =
        svsub_n_u32_x(pg, svlsl_u32_x(pg, svdup_u32(1), shift3), 1);
    const auto low0 = svand_u32_x(pg, product0, mask0);
    const auto low1 = svand_u32_x(pg, product1, mask1);
    const auto low2 = svand_u32_x(pg, product2, mask2);
    const auto low3 = svand_u32_x(pg, product3, mask3);
    const auto sticky0 = svlsr_n_u32_x(
        pg, svorr_u32_x(pg, low0, svsub_u32_x(pg, svdup_u32(0), low0)), 31);
    const auto sticky1 = svlsr_n_u32_x(
        pg, svorr_u32_x(pg, low1, svsub_u32_x(pg, svdup_u32(0), low1)), 31);
    const auto sticky2 = svlsr_n_u32_x(
        pg, svorr_u32_x(pg, low2, svsub_u32_x(pg, svdup_u32(0), low2)), 31);
    const auto sticky3 = svlsr_n_u32_x(
        pg, svorr_u32_x(pg, low3, svsub_u32_x(pg, svdup_u32(0), low3)), 31);
    const auto increment0 = svand_n_u32_x(
        pg, svand_u32_x(pg, head0, svorr_u32_x(pg, sticky0, quotient0)), 1);
    const auto increment1 = svand_n_u32_x(
        pg, svand_u32_x(pg, head1, svorr_u32_x(pg, sticky1, quotient1)), 1);
    const auto increment2 = svand_n_u32_x(
        pg, svand_u32_x(pg, head2, svorr_u32_x(pg, sticky2, quotient2)), 1);
    const auto increment3 = svand_n_u32_x(
        pg, svand_u32_x(pg, head3, svorr_u32_x(pg, sticky3, quotient3)), 1);
    svst1b_u32(pg, target + i + 0 * width,
               svadd_u32_x(pg, quotient0, increment0));
    svst1b_u32(pg, target + i + 1 * width,
               svadd_u32_x(pg, quotient1, increment1));
    svst1b_u32(pg, target + i + 2 * width,
               svadd_u32_x(pg, quotient2, increment2));
    svst1b_u32(pg, target + i + 3 * width,
               svadd_u32_x(pg, quotient3, increment3));
  }
  for (; i < count; i += width) {
    const auto tail = svwhilelt_b32_u64(i, count);
    const auto words =
        svld1_u32(tail, reinterpret_cast<const std::uint32_t*>(source) + i);
    const auto exponent = svlsr_n_u32_x(pg, words, 23);
    const auto significand =
        svorr_n_u32_x(pg, svand_n_u32_x(pg, words, 0x7fffff), 0x800000);
    const auto product =
        svsub_u32_x(pg, svlsl_n_u32_x(pg, significand, 8), significand);
    const auto shift = svsub_u32_x(pg, svdup_u32(149), exponent);
    const auto head = svlsr_u32_x(pg, product, shift);
    const auto quotient = svlsr_n_u32_x(pg, head, 1);
    const auto mask =
        svsub_n_u32_x(pg, svlsl_u32_x(pg, svdup_u32(1), shift), 1);
    const auto low = svand_u32_x(pg, product, mask);
    const auto sticky = svlsr_n_u32_x(
        pg, svorr_u32_x(pg, low, svsub_u32_x(pg, svdup_u32(0), low)), 31);
    const auto increment = svand_n_u32_x(
        pg, svand_u32_x(pg, head, svorr_u32_x(pg, sticky, quotient)), 1);
    svst1b_u32(tail, target + i, svadd_u32_x(pg, quotient, increment));
  }
  return count;
}
