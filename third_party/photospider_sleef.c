/* Private adapter for SLEEF 3.9.0. Upstream sources remain unmodified. */
#include <stddef.h>
#include "misc.h"
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
#include "sleefsimddp.c"
#include "rempitab.c"

/* Call only after ISA admission. Tail lanes duplicate a requested operand;
   they cannot introduce a source read, exception, or logical evaluation. */
void photospider_sleef_evaluate(unsigned kind, const double *a,
                               const double *b, double *out, size_t count) {
  for (size_t offset = 0; offset < count; offset += VECTLENDP) {
    double left[VECTLENDP], right[VECTLENDP], result[VECTLENDP];
    size_t n = count - offset < VECTLENDP ? count - offset : VECTLENDP;
    for (size_t lane = 0; lane < VECTLENDP; ++lane) {
      left[lane] = a[offset + (lane < n ? lane : 0)];
      right[lane] = b ? b[offset + (lane < n ? lane : 0)] : 1;
    }
    vdouble x = vloadu_vd_p(left), y = vloadu_vd_p(right), z;
    switch (kind) {
      case 0: z = xexp(x); break;
      case 1: z = xlog_u1(x); break;
      case 2: z = xsin_u1(x); break;
      case 3: z = xcos_u1(x); break;
      case 4: z = xtan_u1(x); break;
      case 10: z = xpow(x, y); break;
      case 11: z = xatan2_u1(x, y); break;
      default: z = x; break;
    }
    vstoreu_v_p_vd(result, z);
    for (size_t lane = 0; lane < n; ++lane) out[offset + lane] = result[lane];
  }
}
