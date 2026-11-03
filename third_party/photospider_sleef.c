/* Private adapter for SLEEF 3.9.0. Upstream sources remain unmodified. */
#include <math.h>
#include <stddef.h>

#include "misc.h"  // NOLINT(build/include_subdir): private SLEEF include path.
#undef EXPORT
#define EXPORT static
#define Sleef_rempitabdp photospider_sleef_rempitabdp
#define Sleef_rempitabsp photospider_sleef_rempitabsp
#if defined(__aarch64__)
#define ENABLE_ADVSIMD
#elif defined(__x86_64__)
#define ENABLE_AVX2
#else
#define ENABLE_PUREC_SCALAR
#endif
#include "rempitab.c"  // NOLINT(build/include): compile private upstream adapter.
#include "sleefsimddp.c"  // NOLINT(build/include): compile private upstream adapter.

/* Native exp2 for operator-owned bounded batches. Same ISA admission and
   duplicated-tail rules as the evaluator; no change to its kind vocabulary. */
void photospider_sleef_exp2(const double* input, double* output, size_t count) {
  for (size_t offset = 0; offset < count; offset += VECTLENDP) {
    double lanes[VECTLENDP], result[VECTLENDP];
    size_t n = count - offset < VECTLENDP ? count - offset : VECTLENDP;
    for (size_t lane = 0; lane < VECTLENDP; ++lane) {
      lanes[lane] = input[offset + (lane < n ? lane : 0)];
    }
    vstoreu_v_p_vd(result, xexp2(vloadu_vd_p(lanes)));
    for (size_t lane = 0; lane < n; ++lane) {
      output[offset + lane] = result[lane];
    }
  }
}

static const double photospider_log_inverse[97] = {
    0x1.5555555555555p+0, 0x1.51d07eae2f815p+0, 0x1.4e5e0a72f0539p+0,
    0x1.4afd6a052bf5bp+0, 0x1.47ae147ae147bp+0, 0x1.446f86562d9fbp+0,
    0x1.4141414141414p+0, 0x1.3e22cbce4a902p+0, 0x1.3b13b13b13b14p+0,
    0x1.3813813813814p+0, 0x1.3521cfb2b78c1p+0, 0x1.323e34a2b10bfp+0,
    0x1.2f684bda12f68p+0, 0x1.2c9fb4d812ca0p+0, 0x1.29e4129e4129ep+0,
    0x1.27350b8812735p+0, 0x1.2492492492492p+0, 0x1.21fb78121fb78p+0,
    0x1.1f7047dc11f70p+0, 0x1.1cf06ada2811dp+0, 0x1.1a7b9611a7b96p+0,
    0x1.1811811811812p+0, 0x1.15b1e5f75270dp+0, 0x1.135c81135c811p+0,
    0x1.1111111111111p+0, 0x1.0ecf56be69c90p+0, 0x1.0c9714fbcda3bp+0,
    0x1.0a6810a6810a7p+0, 0x1.0842108421084p+0, 0x1.0624dd2f1a9fcp+0,
    0x1.0410410410410p+0, 0x1.0204081020408p+0, 0x1.0000000000000p+0,
    0x1.fc07f01fc07f0p-1, 0x1.f81f81f81f820p-1, 0x1.f44659e4a4271p-1,
    0x1.f07c1f07c1f08p-1, 0x1.ecc07b301ecc0p-1, 0x1.e9131abf0b767p-1,
    0x1.e573ac901e574p-1, 0x1.e1e1e1e1e1e1ep-1, 0x1.de5d6e3f8868ap-1,
    0x1.dae6076b981dbp-1, 0x1.d77b654b82c34p-1, 0x1.d41d41d41d41dp-1,
    0x1.d0cb58f6ec074p-1, 0x1.cd85689039b0bp-1, 0x1.ca4b3055ee191p-1,
    0x1.c71c71c71c71cp-1, 0x1.c3f8f01c3f8f0p-1, 0x1.c0e070381c0e0p-1,
    0x1.bdd2b899406f7p-1, 0x1.bacf914c1bad0p-1, 0x1.b7d6c3dda338bp-1,
    0x1.b4e81b4e81b4fp-1, 0x1.b2036406c80d9p-1, 0x1.af286bca1af28p-1,
    0x1.ac5701ac5701bp-1, 0x1.a98ef606a63bep-1, 0x1.a6d01a6d01a6dp-1,
    0x1.a41a41a41a41ap-1, 0x1.a16d3f97a4b02p-1, 0x1.9ec8e951033d9p-1,
    0x1.9c2d14ee4a102p-1, 0x1.999999999999ap-1, 0x1.970e4f80cb872p-1,
    0x1.948b0fcd6e9e0p-1, 0x1.920fb49d0e229p-1, 0x1.8f9c18f9c18fap-1,
    0x1.8d3018d3018d3p-1, 0x1.8acb90f6bf3aap-1, 0x1.886e5f0abb04ap-1,
    0x1.8618618618618p-1, 0x1.83c977ab2beddp-1, 0x1.8181818181818p-1,
    0x1.7f405fd017f40p-1, 0x1.7d05f417d05f4p-1, 0x1.7ad2208e0ecc3p-1,
    0x1.78a4c8178a4c8p-1, 0x1.767dce434a9b1p-1, 0x1.745d1745d1746p-1,
    0x1.724287f46debcp-1, 0x1.702e05c0b8170p-1, 0x1.6e1f76b4337c7p-1,
    0x1.6c16c16c16c17p-1, 0x1.6a13cd1537290p-1, 0x1.6816816816817p-1,
    0x1.661ec6a5122f9p-1, 0x1.642c8590b2164p-1, 0x1.623fa77016240p-1,
    0x1.6058160581606p-1, 0x1.5e75bb8d015e7p-1, 0x1.5c9882b931057p-1,
    0x1.5ac056b015ac0p-1, 0x1.58ed2308158edp-1, 0x1.571ed3c506b3ap-1,
    0x1.5555555555555p-1};
static const double photospider_log_center[97] = {
    -0x1.a8ff971810a5ep-2, -0x1.99b072a96c6b2p-2, -0x1.8a8980abfbd32p-2,
    -0x1.7b89f02cf2aadp-2, -0x1.6cb0f6865c8eap-2, -0x1.5dfdcf1eeae0ep-2,
    -0x1.4f6fbb2cec598p-2, -0x1.4106017c3eca3p-2, -0x1.32bfee370ee68p-2,
    -0x1.249cd2b13cd6cp-2, -0x1.169c05363f158p-2, -0x1.08bce0d95fa38p-2,
    -0x1.f5fd8a9063e35p-3, -0x1.dac22d3e441d3p-3, -0x1.bfc67a7fff4ccp-3,
    -0x1.a5094b54d2828p-3, -0x1.8a8980abfbd32p-3, -0x1.7046031c79f85p-3,
    -0x1.563dc29ffacb2p-3, -0x1.3c6fb650cde51p-3, -0x1.22dadc2ab3497p-3,
    -0x1.097e38ce60649p-3, -0x1.e0b1ae8f2fd56p-4, -0x1.aed391ab6674ep-4,
    -0x1.7d60496cfbb4cp-4, -0x1.4c560fe68af88p-4, -0x1.1bb32a600549dp-4,
    -0x1.d6ebd1f1febfep-5, -0x1.77394c9d958d5p-5, -0x1.184b8e4c56af8p-5,
    -0x1.743ee861f3556p-6, -0x1.72c7ba20f7327p-7, 0x0.0p+0,
    0x1.6fe50b6ef0851p-7,  0x1.6e79685c2d22ap-6,  0x1.11cd1d5133413p-5,
    0x1.6bad3758efd87p-5,  0x1.c4dfab90aab5fp-5,  0x1.0eb389fa29f9bp-4,
    0x1.3aa2fdd27f1c3p-4,  0x1.663f6fac91316p-4,  0x1.918a16e46335bp-4,
    0x1.bc84240adabbap-4,  0x1.e72ec117fa5b2p-4,  0x1.08c588cda79e4p-3,
    0x1.1dcd197552b7bp-3,  0x1.32ae9e278ae1ap-3,  0x1.476a9f983f74dp-3,
    0x1.5c01a39fbd688p-3,  0x1.70742d4ef027fp-3,  0x1.84c2bd02f03b3p-3,
    0x1.98edd077e70dfp-3,  0x1.acf5e2db4ec94p-3,  0x1.c0db6cdd94deep-3,
    0x1.d49ee4c325970p-3,  0x1.e840be74e6a4dp-3,  0x1.fbc16b902680ap-3,
    0x1.0790adbb03009p-2,  0x1.11307dad30b76p-2,  0x1.1ac05b291f070p-2,
    0x1.24407ab0e073ap-2,  0x1.2db10fc4d9aafp-2,  0x1.37124cea4cdedp-2,
    0x1.406463b1b0449p-2,  0x1.49a784bcd1b8bp-2,  0x1.52dbdfc4c96b3p-2,
    0x1.5c01a39fbd688p-2,  0x1.6518fe4677ba7p-2,  0x1.6e221cd9d0cdep-2,
    0x1.771d2ba7efb3cp-2,  0x1.800a563161c54p-2,  0x1.88e9c72e0b226p-2,
    0x1.91bba891f1709p-2,  0x1.9a802391e232fp-2,  0x1.a33760a7f6051p-2,
    0x1.abe18797f1f49p-2,  0x1.b47ebf73882a1p-2,  0x1.bd0f2e9e79031p-2,
    0x1.c592fad295b56p-2,  0x1.ce0a4923a587dp-2,  0x1.d6753e032ea0fp-2,
    0x1.ded3fd442364cp-2,  0x1.e726aa1e754d2p-2,  0x1.ef6d67328e220p-2,
    0x1.f7a8568cb06cfp-2,  0x1.ffd799a83ff9bp-2,  0x1.03fda8b97997fp-1,
    0x1.0809cf27f703dp-1,  0x1.0c10500d63aa6p-1,  0x1.10113b153c8eap-1,
    0x1.140c9faa1e544p-1,  0x1.18028cf72976ap-1,  0x1.1bf311e95d00ep-1,
    0x1.1fde3d30e8126p-1,  0x1.23c41d42727c8p-1,  0x1.27a4c0585cbf8p-1,
    0x1.2b803473f7ad1p-1};

/* Positive log2: nearest binary center in [.75,1.5], exact subtraction, then
   degree-seven log1p. Keeping a center at one avoids cancellation near log2(1).
   The tables were rounded from 90-decimal-digit arithmetic. */
static __attribute__((noinline, cold)) vdouble
photospider_log_fallback(vdouble d) {
  return xlog2_u35(d);
}
static INLINE vdouble photospider_grade_log2(vdouble d) {
  const vopmask valid =
      vand_vo_vo_vo(vgt_vo_vd_vd(d, vcast_vd_d(0)),
                    vle_vo_vd_vd(d, vcast_vd_d(0x1.fffffffffffffp+1023)));
  const vdouble original = d;
  const int full = vtestallones_i_vo64(valid);
  if (!full)
    d = vsel_vd_vo_vd_vd(valid, d, vcast_vd_d(1));
  const vopmask tiny = vlt_vo_vd_vd(d, vcast_vd_d(SLEEF_DBL_MIN));
  d = vsel_vd_vo_vd_vd(tiny, vmul_vd_vd_vd(d, vcast_vd_d(0x1p64)), d);
  vint e = vilogb2k_vi_vd(vmul_vd_vd_vd(d, vcast_vd_d(1.0 / .75)));
  const vdouble m = vldexp3_vd_vd_vi(d, vneg_vi_vi(e));
  e = vsel_vi_vo_vi_vi(vcast_vo32_vo64(tiny), vsub_vi_vi_vi(e, vcast_vi_i(64)),
                       e);
  const vint j = vrint_vi_vd(
      vmul_vd_vd_vd(vsub_vd_vd_vd(m, vcast_vd_d(.75)), vcast_vd_d(128)));
  const vdouble c =
      vfma_vd_vd_vd_vd(vcast_vd_vi(j), vcast_vd_d(1.0 / 128), vcast_vd_d(.75));
  const vdouble r = vmul_vd_vd_vd(vsub_vd_vd_vd(m, c),
                                  vgather_vd_p_vi(photospider_log_inverse, j));
  vdouble p = vcast_vd_d(1.0 / 7);
  p = vfma_vd_vd_vd_vd(p, r, vcast_vd_d(-1.0 / 6));
  p = vfma_vd_vd_vd_vd(p, r, vcast_vd_d(1.0 / 5));
  p = vfma_vd_vd_vd_vd(p, r, vcast_vd_d(-1.0 / 4));
  p = vfma_vd_vd_vd_vd(p, r, vcast_vd_d(1.0 / 3));
  p = vfma_vd_vd_vd_vd(p, r, vcast_vd_d(-.5));
  const vdouble correction = vfma_vd_vd_vd_vd(vmul_vd_vd_vd(r, r), p, r);
  const vdouble result = vfma_vd_vd_vd_vd(
      correction, vcast_vd_d(R_LN2),
      vadd_vd_vd_vd(vcast_vd_vi(e),
                    vgather_vd_p_vi(photospider_log_center, j)));
  /* Exceptional neighbors must not change a regular lane's approximation. */
  return full ? result
              : vsel_vd_vo_vd_vd(valid, result,
                                 photospider_log_fallback(original));
}

/* Positive Float64 log2 for explicitly published operator feature stages. */
void photospider_sleef_log2(const double* input, double* output, size_t count) {
  size_t i = 0;
  for (; i + VECTLENDP <= count; i += VECTLENDP)
    vstoreu_v_p_vd(output + i, photospider_grade_log2(vloadu_vd_p(input + i)));
  if (i < count) {
    double tail[VECTLENDP], result[VECTLENDP];
    for (size_t lane = 0; lane < VECTLENDP; ++lane)
      tail[lane] = input[i + (lane < count - i ? lane : 0)];
    vstoreu_v_p_vd(result, photospider_grade_log2(vloadu_vd_p(tail)));
    for (size_t lane = 0; lane < count - i; ++lane)
      output[i + lane] = result[lane];
  }
}

void photospider_sleef_log2_clamped(const double* input, double* output,
                                    double epsilon, size_t count) {
  const vdouble floor = vcast_vd_d(epsilon);
  size_t i = 0;
  for (; i + VECTLENDP <= count; i += VECTLENDP) {
    const vdouble d = vloadu_vd_p(input + i);
    vstoreu_v_p_vd(output + i, photospider_grade_log2(vsel_vd_vo_vd_vd(
                                   vlt_vo_vd_vd(d, floor), floor, d)));
  }
  if (i < count) {
    double in[VECTLENDP], out[VECTLENDP];
    for (size_t j = 0; j < VECTLENDP; ++j)
      in[j] = input[i + (j < count - i ? j : 0)];
    const vdouble d = vloadu_vd_p(in);
    vstoreu_v_p_vd(out, photospider_grade_log2(vsel_vd_vo_vd_vd(
                            vlt_vo_vd_vd(d, floor), floor, d)));
    for (size_t j = 0; j < count - i; ++j)
      output[i + j] = out[j];
  }
}

/* Rounded 2^(j/64), generated at 90 decimal digits. This is an exp argument
   reduction table, not a quantized bilateral range lookup. For |r|<=ln(2)/128,
   the degree-five Taylor remainder is below 3.6e-17. All other arguments retain
   the full-domain SLEEF implementation, including subnormal output. */
static const double photospider_exp_table[64] = {
    0x1.0000000000000p+0, 0x1.02c9a3e778061p+0, 0x1.059b0d3158574p+0,
    0x1.0874518759bc8p+0, 0x1.0b5586cf9890fp+0, 0x1.0e3ec32d3d1a2p+0,
    0x1.11301d0125b51p+0, 0x1.1429aaea92de0p+0, 0x1.172b83c7d517bp+0,
    0x1.1a35beb6fcb75p+0, 0x1.1d4873168b9aap+0, 0x1.2063b88628cd6p+0,
    0x1.2387a6e756238p+0, 0x1.26b4565e27cddp+0, 0x1.29e9df51fdee1p+0,
    0x1.2d285a6e4030bp+0, 0x1.306fe0a31b715p+0, 0x1.33c08b26416ffp+0,
    0x1.371a7373aa9cbp+0, 0x1.3a7db34e59ff7p+0, 0x1.3dea64c123422p+0,
    0x1.4160a21f72e2ap+0, 0x1.44e086061892dp+0, 0x1.486a2b5c13cd0p+0,
    0x1.4bfdad5362a27p+0, 0x1.4f9b2769d2ca7p+0, 0x1.5342b569d4f82p+0,
    0x1.56f4736b527dap+0, 0x1.5ab07dd485429p+0, 0x1.5e76f15ad2148p+0,
    0x1.6247eb03a5585p+0, 0x1.6623882552225p+0, 0x1.6a09e667f3bcdp+0,
    0x1.6dfb23c651a2fp+0, 0x1.71f75e8ec5f74p+0, 0x1.75feb564267c9p+0,
    0x1.7a11473eb0187p+0, 0x1.7e2f336cf4e62p+0, 0x1.82589994cce13p+0,
    0x1.868d99b4492edp+0, 0x1.8ace5422aa0dbp+0, 0x1.8f1ae99157736p+0,
    0x1.93737b0cdc5e5p+0, 0x1.97d829fde4e50p+0, 0x1.9c49182a3f090p+0,
    0x1.a0c667b5de565p+0, 0x1.a5503b23e255dp+0, 0x1.a9e6b5579fdbfp+0,
    0x1.ae89f995ad3adp+0, 0x1.b33a2b84f15fbp+0, 0x1.b7f76f2fb5e47p+0,
    0x1.bcc1e904bc1d2p+0, 0x1.c199bdd85529cp+0, 0x1.c67f12e57d14bp+0,
    0x1.cb720dcef9069p+0, 0x1.d072d4a07897cp+0, 0x1.d5818dcfba487p+0,
    0x1.da9e603db3285p+0, 0x1.dfc97337b9b5fp+0, 0x1.e502ee78b3ff6p+0,
    0x1.ea4afa2a490dap+0, 0x1.efa1bee615a27p+0, 0x1.f50765b6e4540p+0,
    0x1.fa7c1819e90d8p+0};
static __attribute__((noinline, cold)) vdouble
photospider_exp_fallback(vdouble x) {
  return xexp(x);
}
static __attribute__((noinline, cold)) vdouble
photospider_exp2_fallback(vdouble x) {
  return xexp2(x);
}
static INLINE vdouble photospider_exp_negative(vdouble x) {
  const vopmask regular = vand_vo_vo_vo(vge_vo_vd_vd(x, vcast_vd_d(-700)),
                                        vle_vo_vd_vd(x, vcast_vd_d(0)));
  const vdouble original = x;
  const int full = vtestallones_i_vo64(regular);
  if (!full)
    x = vsel_vd_vo_vd_vd(regular, x, vcast_vd_d(0));
  const vdouble u = vrint_vd_vd(vmul_vd_vd_vd(x, vcast_vd_d(64 * R_LN2)));
  const vint q = vrint_vi_vd(u);
  vdouble r = vfma_vd_vd_vd_vd(u, vcast_vd_d(-L2U / 64), x);
  r = vfma_vd_vd_vd_vd(u, vcast_vd_d(-L2L / 64), r);
  vdouble p = vcast_vd_d(1.0 / 120);
  p = vfma_vd_vd_vd_vd(p, r, vcast_vd_d(1.0 / 24));
  p = vfma_vd_vd_vd_vd(p, r, vcast_vd_d(1.0 / 6));
  p = vfma_vd_vd_vd_vd(p, r, vcast_vd_d(.5));
  p = vfma_vd_vd_vd_vd(p, r, vcast_vd_d(1));
  p = vfma_vd_vd_vd_vd(p, r, vcast_vd_d(1));
  const vdouble scale =
      vgather_vd_p_vi(photospider_exp_table, vand_vi_vi_vi(q, vcast_vi_i(63)));
  const vdouble result =
      vmul_vd_vd_vd(p, vldexp3_vd_vd_vi(scale, vsra_vi_vi_i(q, 6)));
  return full ? result
              : vsel_vd_vo_vd_vd(regular, result,
                                 photospider_exp_fallback(original));
}

static INLINE void photospider_exp_pair(vdouble x0, vdouble x1, vdouble* w0,
                                        vdouble* w1) {
  const vopmask valid0 = vand_vo_vo_vo(vge_vo_vd_vd(x0, vcast_vd_d(-700)),
                                       vle_vo_vd_vd(x0, vcast_vd_d(0)));
  const vopmask valid1 = vand_vo_vo_vo(vge_vo_vd_vd(x1, vcast_vd_d(-700)),
                                       vle_vo_vd_vd(x1, vcast_vd_d(0)));
  if (!vtestallones_i_vo64(vand_vo_vo_vo(valid0, valid1))) {
    *w0 = photospider_exp_negative(x0);
    *w1 = photospider_exp_negative(x1);
    return;
  }
  const vdouble u0 = vrint_vd_vd(vmul_vd_vd_vd(x0, vcast_vd_d(64 * R_LN2)));
  const vdouble u1 = vrint_vd_vd(vmul_vd_vd_vd(x1, vcast_vd_d(64 * R_LN2)));
  const vint q0 = vrint_vi_vd(u0), q1 = vrint_vi_vd(u1);
  vdouble r0 = vfma_vd_vd_vd_vd(u0, vcast_vd_d(-L2U / 64), x0);
  vdouble r1 = vfma_vd_vd_vd_vd(u1, vcast_vd_d(-L2U / 64), x1);
  r0 = vfma_vd_vd_vd_vd(u0, vcast_vd_d(-L2L / 64), r0);
  r1 = vfma_vd_vd_vd_vd(u1, vcast_vd_d(-L2L / 64), r1);
  const vdouble s0 = vldexp3_vd_vd_vi(
      vgather_vd_p_vi(photospider_exp_table, vand_vi_vi_vi(q0, vcast_vi_i(63))),
      vsra_vi_vi_i(q0, 6));
  const vdouble s1 = vldexp3_vd_vd_vi(
      vgather_vd_p_vi(photospider_exp_table, vand_vi_vi_vi(q1, vcast_vi_i(63))),
      vsra_vi_vi_i(q1, 6));
  vdouble p0 = vcast_vd_d(1.0 / 120), p1 = p0;
  p0 = vfma_vd_vd_vd_vd(p0, r0, vcast_vd_d(1.0 / 24));
  p1 = vfma_vd_vd_vd_vd(p1, r1, vcast_vd_d(1.0 / 24));
  p0 = vfma_vd_vd_vd_vd(p0, r0, vcast_vd_d(1.0 / 6));
  p1 = vfma_vd_vd_vd_vd(p1, r1, vcast_vd_d(1.0 / 6));
  p0 = vfma_vd_vd_vd_vd(p0, r0, vcast_vd_d(.5));
  p1 = vfma_vd_vd_vd_vd(p1, r1, vcast_vd_d(.5));
  p0 = vfma_vd_vd_vd_vd(p0, r0, vcast_vd_d(1));
  p1 = vfma_vd_vd_vd_vd(p1, r1, vcast_vd_d(1));
  p0 = vfma_vd_vd_vd_vd(p0, r0, vcast_vd_d(1));
  p1 = vfma_vd_vd_vd_vd(p1, r1, vcast_vd_d(1));
  *w0 = vmul_vd_vd_vd(p0, s0);
  *w1 = vmul_vd_vd_vd(p1, s1);
}

/* Native grade gains use the same small-residual polynomial. The restricted
   exponent range keeps every scale normal; exceptional gains retain xexp2. */
static INLINE vdouble photospider_grade_exp2(vdouble x) {
  const vopmask normal = vand_vo_vo_vo(vge_vo_vd_vd(x, vcast_vd_d(-1000)),
                                       vle_vo_vd_vd(x, vcast_vd_d(1000)));
  const vdouble original = x;
  const int full = vtestallones_i_vo64(normal);
  if (!full)
    x = vsel_vd_vo_vd_vd(normal, x, vcast_vd_d(0));
  const vdouble u = vrint_vd_vd(vmul_vd_vd_vd(x, vcast_vd_d(64)));
  const vint q = vrint_vi_vd(u);
  const vdouble r =
      vmul_vd_vd_vd(vsub_vd_vd_vd(x, vmul_vd_vd_vd(u, vcast_vd_d(1.0 / 64))),
                    vcast_vd_d(0.693147180559945309417232121458));
  vdouble p = vcast_vd_d(1.0 / 120);
  p = vfma_vd_vd_vd_vd(p, r, vcast_vd_d(1.0 / 24));
  p = vfma_vd_vd_vd_vd(p, r, vcast_vd_d(1.0 / 6));
  p = vfma_vd_vd_vd_vd(p, r, vcast_vd_d(.5));
  p = vfma_vd_vd_vd_vd(p, r, vcast_vd_d(1));
  p = vfma_vd_vd_vd_vd(p, r, vcast_vd_d(1));
  const vdouble scale =
      vgather_vd_p_vi(photospider_exp_table, vand_vi_vi_vi(q, vcast_vi_i(63)));
  const vdouble result =
      vmul_vd_vd_vd(p, vldexp3_vd_vd_vi(scale, vsra_vi_vi_i(q, 6)));
  return full ? result
              : vsel_vd_vo_vd_vd(normal, result,
                                 photospider_exp2_fallback(original));
}
void photospider_sleef_grade_exp2(const double* input, double* output,
                                  size_t count) {
  size_t i = 0;
  for (; i + VECTLENDP <= count; i += VECTLENDP)
    vstoreu_v_p_vd(output + i, photospider_grade_exp2(vloadu_vd_p(input + i)));
  if (i < count) {
    double tail[VECTLENDP], result[VECTLENDP];
    for (size_t lane = 0; lane < VECTLENDP; ++lane)
      tail[lane] = input[i + (lane < count - i ? lane : 0)];
    vstoreu_v_p_vd(result, photospider_grade_exp2(vloadu_vd_p(tail)));
    for (size_t lane = 0; lane < count - i; ++lane)
      output[i + lane] = result[lane];
  }
}

/* Equal published log coordinates have an exact zero difference, including
   equal infinities generated by finite source RGB luminance overflow. */
static INLINE vdouble photospider_log_delta(vdouble neighbor, vdouble center) {
  return vsel_vd_vo_vd_vd(veq_vo_vd_vd(neighbor, center), vcast_vd_d(0),
                          vsub_vd_vd_vd(neighbor, center));
}

void photospider_sleef_bilateral_weights(const double* restrict center,
                                         const double* restrict neighbor,
                                         double* restrict weights,
                                         double spatial, double sigma_range,
                                         size_t count) {
  const double inverse = 1 / sigma_range;
  const int regular = isfinite(inverse) && inverse != 0;
  const vdouble scale = vcast_vd_d(regular ? inverse : sigma_range);
  const vdouble space = vcast_vd_d(spatial), half = vcast_vd_d(-.5);
  size_t i = 0;
  for (; i + VECTLENDP <= count; i += VECTLENDP) {
    const vdouble d = photospider_log_delta(vloadu_vd_p(neighbor + i),
                                            vloadu_vd_p(center + i));
    const vdouble q =
        regular ? vmul_vd_vd_vd(d, scale) : vdiv_vd_vd_vd(d, scale);
    const vdouble a =
        vadd_vd_vd_vd(space, vmul_vd_vd_vd(vmul_vd_vd_vd(half, q), q));
    vstoreu_v_p_vd(weights + i, photospider_exp_negative(a));
  }
  if (i < count) {
    double d[VECTLENDP], out[VECTLENDP];
    for (size_t j = 0; j < VECTLENDP; ++j) {
      const size_t k = i + (j < count - i ? j : 0);
      d[j] = neighbor[k] == center[k] ? 0 : neighbor[k] - center[k];
    }
    const vdouble delta = vloadu_vd_p(d);
    const vdouble q =
        regular ? vmul_vd_vd_vd(delta, scale) : vdiv_vd_vd_vd(delta, scale);
    vstoreu_v_p_vd(out, photospider_exp_negative(vadd_vd_vd_vd(
                            space, vmul_vd_vd_vd(vmul_vd_vd_vd(half, q), q))));
    for (size_t j = 0; j < count - i; ++j)
      weights[i + j] = out[j];
  }
}
void photospider_sleef_weighted_step(const double* restrict center,
                                     const double* restrict neighbor,
                                     const double* restrict weights,
                                     double* restrict numerator,
                                     double* restrict denominator,
                                     size_t count) {
  size_t i = 0;
  for (; i + VECTLENDP <= count; i += VECTLENDP) {
    const vdouble d = photospider_log_delta(vloadu_vd_p(neighbor + i),
                                            vloadu_vd_p(center + i));
    const vdouble w = vloadu_vd_p(weights + i);
    vstoreu_v_p_vd(numerator + i, vadd_vd_vd_vd(vloadu_vd_p(numerator + i),
                                                vmul_vd_vd_vd(w, d)));
    vstoreu_v_p_vd(denominator + i,
                   vadd_vd_vd_vd(vloadu_vd_p(denominator + i), w));
  }
  for (; i < count; ++i) {
    const double d = neighbor[i] == center[i] ? 0 : neighbor[i] - center[i];
    numerator[i] += weights[i] * d;
    denominator[i] += weights[i];
  }
}

/* Fused neighbor accumulation for the finite bilateral mean. Every lane keeps
   its own Float64 left fold. The caller handles support, storage and polling;
   sigma_range is positive. Equal published log coordinates have zero delta. */
void photospider_sleef_bilateral_step(const double* restrict center,
                                      const double* restrict neighbor,
                                      double* restrict numerator,
                                      double* restrict denominator,
                                      double spatial, double sigma_range,
                                      size_t count) {
  const double inverse = 1 / sigma_range;
  const int regular = isfinite(inverse) && inverse != 0;
  const vdouble scale = vcast_vd_d(regular ? inverse : sigma_range);
  const vdouble space = vcast_vd_d(spatial), half = vcast_vd_d(-.5);
  size_t i = 0;
  if (regular) {
    for (; i + 2 * VECTLENDP <= count; i += 2 * VECTLENDP) {
      const vdouble d0 = photospider_log_delta(vloadu_vd_p(neighbor + i),
                                               vloadu_vd_p(center + i));
      const vdouble d1 =
          photospider_log_delta(vloadu_vd_p(neighbor + i + VECTLENDP),
                                vloadu_vd_p(center + i + VECTLENDP));
      const vdouble q0 = vmul_vd_vd_vd(d0, scale),
                    q1 = vmul_vd_vd_vd(d1, scale);
      const vdouble a0 =
          vadd_vd_vd_vd(space, vmul_vd_vd_vd(vmul_vd_vd_vd(half, q0), q0));
      const vdouble a1 =
          vadd_vd_vd_vd(space, vmul_vd_vd_vd(vmul_vd_vd_vd(half, q1), q1));
      vdouble w0, w1;
      photospider_exp_pair(a0, a1, &w0, &w1);
      vstoreu_v_p_vd(numerator + i, vadd_vd_vd_vd(vloadu_vd_p(numerator + i),
                                                  vmul_vd_vd_vd(w0, d0)));
      vstoreu_v_p_vd(numerator + i + VECTLENDP,
                     vadd_vd_vd_vd(vloadu_vd_p(numerator + i + VECTLENDP),
                                   vmul_vd_vd_vd(w1, d1)));
      vstoreu_v_p_vd(denominator + i,
                     vadd_vd_vd_vd(vloadu_vd_p(denominator + i), w0));
      vstoreu_v_p_vd(
          denominator + i + VECTLENDP,
          vadd_vd_vd_vd(vloadu_vd_p(denominator + i + VECTLENDP), w1));
    }
  }
  for (; i + VECTLENDP <= count; i += VECTLENDP) {
    const vdouble d = photospider_log_delta(vloadu_vd_p(neighbor + i),
                                            vloadu_vd_p(center + i));
    const vdouble q =
        regular ? vmul_vd_vd_vd(d, scale) : vdiv_vd_vd_vd(d, scale);
    const vdouble arg =
        vadd_vd_vd_vd(space, vmul_vd_vd_vd(vmul_vd_vd_vd(half, q), q));
    const vdouble w = photospider_exp_negative(arg);
    vstoreu_v_p_vd(numerator + i, vadd_vd_vd_vd(vloadu_vd_p(numerator + i),
                                                vmul_vd_vd_vd(w, d)));
    vstoreu_v_p_vd(denominator + i,
                   vadd_vd_vd_vd(vloadu_vd_p(denominator + i), w));
  }
  if (i < count) {
    double delta[VECTLENDP], weights[VECTLENDP];
    for (size_t lane = 0; lane < VECTLENDP; ++lane) {
      const size_t j = i + (lane < count - i ? lane : 0);
      delta[lane] = neighbor[j] == center[j] ? 0 : neighbor[j] - center[j];
    }
    const vdouble d = vloadu_vd_p(delta);
    const vdouble q =
        regular ? vmul_vd_vd_vd(d, scale) : vdiv_vd_vd_vd(d, scale);
    vstoreu_v_p_vd(weights,
                   photospider_exp_negative(vadd_vd_vd_vd(
                       space, vmul_vd_vd_vd(vmul_vd_vd_vd(half, q), q))));
    for (size_t lane = 0; lane < count - i; ++lane) {
      numerator[i + lane] += weights[lane] * delta[lane];
      denominator[i + lane] += weights[lane];
    }
  }
}

/* Call only after ISA admission. Tail lanes duplicate a requested operand;
   they cannot introduce a source read, exception, or logical evaluation. */
void photospider_sleef_evaluate(unsigned kind, const double* a, const double* b,
                                double* out, size_t count) {
  for (size_t offset = 0; offset < count; offset += VECTLENDP) {
    double left[VECTLENDP], right[VECTLENDP], result[VECTLENDP];
    size_t n = count - offset < VECTLENDP ? count - offset : VECTLENDP;
    for (size_t lane = 0; lane < VECTLENDP; ++lane) {
      left[lane] = a[offset + (lane < n ? lane : 0)];
      right[lane] = b ? b[offset + (lane < n ? lane : 0)] : 1;
    }
    vdouble x = vloadu_vd_p(left), y = vloadu_vd_p(right), z;
    switch (kind) {
      case 0:
        z = xexp(x);
        break;
      case 1:
        z = xlog_u1(x);
        break;
      case 2:
        z = xsin_u1(x);
        break;
      case 3:
        z = xcos_u1(x);
        break;
      case 4:
        z = xtan_u1(x);
        break;
      case 10:
        z = xpow(x, y);
        break;
      case 11:
        z = xatan2_u1(x, y);
        break;
      case 12:
        z = xcbrt_u1(x);
        break;
      default:
        z = x;
        break;
    }
    vstoreu_v_p_vd(result, z);
    for (size_t lane = 0; lane < n; ++lane)
      out[offset + lane] = result[lane];
  }
}
