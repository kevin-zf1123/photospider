/* Independent Little CMS 2.19.1 oracle. Uses only unmodified upstream
 * headers/library, including its private transform struct for gamut scoring.
 * Serialize/reopen every fixture before computing. Both public dtypes use
 * double formatters; CMYK and Lab L use explicit public unit adaptation.
 * Synthetic CMYK is an interface fixture, not a printer model. */
#include <fenv.h>  // NOLINT(build/c++11)
#include <inttypes.h>
#include <lcms2.h>
#include <lcms2_internal.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define ORACLE_REQUIRE(x)                             \
  do {                                                \
    if (!(x)) {                                       \
      fprintf(stderr, "line %d: %s\n", __LINE__, #x); \
      exit(1);                                        \
    }                                                 \
  } while (0)
typedef struct {
  cmsHPROFILE h;
  unsigned char* bytes;
  cmsUInt32Number size;
} Profile;
static cmsContext ctx;
static const char* directory;
static FILE* table;
static const cmsUInt32Number flags[2] = {cmsFLAGS_NOCACHE | cmsFLAGS_NOOPTIMIZE,
                                         cmsFLAGS_NOCACHE};
static const char* mode[2] = {"reference", "optimized"};
static void error(cmsContext c, cmsUInt32Number n, const char* text) {
  (void)c;
  fprintf(stderr, "LCMS %" PRIu32 ": %s\n", (uint32_t)n, text);
  exit(1);
}
static void path(char* out, size_t capacity, const char* file) {
  ORACLE_REQUIRE(snprintf(out, capacity, "%s/%s", directory, file) > 0);
}
static Profile freeze(cmsHPROFILE h, const char* name) {
  Profile p = {0};
  char filename[4096];
  FILE* f;
  cmsMLU* m = cmsMLUalloc(ctx, 1);
  const unsigned char date[12] = {7, 234, 0, 1, 0, 1, 0, 0, 0, 0, 0, 0};
  ORACLE_REQUIRE(h && m);
  ORACLE_REQUIRE(cmsMLUsetASCII(m, "en", "US", name));
  ORACLE_REQUIRE(cmsWriteTag(h, cmsSigProfileDescriptionTag, m));
  cmsMLUfree(m);
  m = cmsMLUalloc(ctx, 1);
  ORACLE_REQUIRE(m);
  ORACLE_REQUIRE(
      cmsMLUsetASCII(m, "en", "US", "Photospider synthetic oracle fixture"));
  ORACLE_REQUIRE(cmsWriteTag(h, cmsSigCopyrightTag, m));
  cmsMLUfree(m);
  if (!cmsReadTag(h, cmsSigMediaWhitePointTag)) {
    ORACLE_REQUIRE(cmsWriteTag(h, cmsSigMediaWhitePointTag, cmsD50_XYZ()));
  }
  ORACLE_REQUIRE(cmsSaveProfileToMem(h, NULL, &p.size));
  p.bytes = malloc(p.size);
  ORACLE_REQUIRE(p.bytes);
  ORACLE_REQUIRE(cmsSaveProfileToMem(h, p.bytes, &p.size));
  memcpy(p.bytes + 24, date, 12);
  memset(p.bytes + 84, 0, 16);
  path(filename, sizeof(filename), name);
  f = fopen(filename, "wb");
  ORACLE_REQUIRE(f && fwrite(p.bytes, 1, p.size, f) == p.size);
  ORACLE_REQUIRE(!fclose(f));
  p.h = cmsOpenProfileFromMemTHR(ctx, p.bytes, p.size);
  ORACLE_REQUIRE(p.h);
  ORACLE_REQUIRE(cmsCloseProfile(h));
  return p;
}
static cmsHPROFILE rgb(double gamma, int narrow) {
  cmsCIExyY w = {.3127, .329, 1};
  cmsCIExyYTRIPLE p = {{.64, .33, 1}, {.30, .60, 1}, {.15, .06, 1}};
  cmsToneCurve* g = cmsBuildGamma(ctx, gamma);
  cmsToneCurve* curves[3] = {g, g, g};
  cmsHPROFILE h;
  ORACLE_REQUIRE(g);
  if (narrow) {
    p.Red.x = .60;
    p.Green.y = .57;
    p.Blue.y = .10;
  }
  h = cmsCreateRGBProfileTHR(ctx, &w, &p, curves);
  cmsFreeToneCurve(g);
  ORACLE_REQUIRE(h);
  return h;
}
static int cmyk_lab(const cmsUInt16Number in[], cmsUInt16Number out[],
                    void* unused) {
  double k = in[3] / 65535., r = (1 - in[0] / 65535.) * (1 - k),
         g = (1 - in[1] / 65535.) * (1 - k), b = (1 - in[2] / 65535.) * (1 - k);
  cmsCIELab lab = {100 * (r + g + b) / 3, 80 * (r - g), 80 * ((r + g) / 2 - b)};
  (void)unused;
  cmsFloat2LabEncoded(out, &lab);
  return 1;
}
static int lab_cmyk(const cmsUInt16Number in[], cmsUInt16Number out[],
                    void* unused) {
  cmsCIELab lab;
  double y, a, b, rgb[3];
  unsigned c;
  (void)unused;
  cmsLabEncoded2Float(&lab, in);
  y = lab.L / 100;
  a = lab.a / 80;
  b = lab.b / 80;
  rgb[0] = y + b / 3 + a / 2;
  rgb[1] = y + b / 3 - a / 2;
  rgb[2] = y - 2 * b / 3;
  for (c = 0; c < 3; ++c) {
    out[c] = (cmsUInt16Number)floor(fmin(1, fmax(0, 1 - rgb[c])) * 65535 + .5);
  }
  out[3] = 0;
  return 1;
}
static cmsPipeline* lut(unsigned ni, unsigned no, cmsSAMPLER16 sampler) {
  cmsPipeline* p = cmsPipelineAlloc(ctx, ni, no);
  cmsStage* clut = cmsStageAllocCLut16bit(ctx, 9, ni, no, NULL);
  cmsToneCurve* g = cmsBuildGamma(ctx, 1);
  cmsToneCurve* curves[4] = {g, g, g, g};
  cmsStage* pre;
  cmsStage* post;
  ORACLE_REQUIRE(p && clut && g);
  pre = cmsStageAllocToneCurves(ctx, ni, curves);
  post = cmsStageAllocToneCurves(ctx, no, curves);
  cmsFreeToneCurve(g);
  ORACLE_REQUIRE(pre && post);
  ORACLE_REQUIRE(cmsStageSampleCLut16bit(clut, sampler, NULL, 0));
  ORACLE_REQUIRE(cmsPipelineInsertStage(p, cmsAT_END, pre));
  ORACLE_REQUIRE(cmsPipelineInsertStage(p, cmsAT_END, clut));
  ORACLE_REQUIRE(cmsPipelineInsertStage(p, cmsAT_END, post));
  return p;
}
static cmsHPROFILE cmyk(void) {
  cmsHPROFILE h = cmsCreateProfilePlaceholder(ctx);
  cmsPipeline* a = lut(4, 3, cmyk_lab);
  cmsPipeline* b = lut(3, 4, lab_cmyk);
  const cmsTagSignature at[3] = {cmsSigAToB0Tag, cmsSigAToB1Tag,
                                 cmsSigAToB2Tag},
                        bt[3] = {cmsSigBToA0Tag, cmsSigBToA1Tag,
                                 cmsSigBToA2Tag};
  unsigned i;
  ORACLE_REQUIRE(h);
  cmsSetProfileVersion(h, 4.3);
  cmsSetDeviceClass(h, cmsSigOutputClass);
  cmsSetColorSpace(h, cmsSigCmykData);
  cmsSetPCS(h, cmsSigLabData);
  cmsSetHeaderRenderingIntent(h, 1);
  ORACLE_REQUIRE(cmsWriteTag(h, cmsSigMediaWhitePointTag, cmsD50_XYZ()));
  for (i = 0; i < 3; ++i) {
    ORACLE_REQUIRE(cmsWriteTag(h, at[i], a));
    ORACLE_REQUIRE(cmsWriteTag(h, bt[i], b));
  }
  cmsPipelineFree(a);
  cmsPipelineFree(b);
  return h;
}
static cmsHPROFILE ordinary_pcs(cmsHPROFILE h) {
  cmsPipeline* p = cmsReadTag(h, cmsSigAToB0Tag);
  ORACLE_REQUIRE(p);
  ORACLE_REQUIRE(cmsWriteTag(h, cmsSigBToA0Tag, p));
  cmsSetDeviceClass(h, cmsSigColorSpaceClass);
  return h;
}
static cmsUInt32Number format(cmsColorSpaceSignature s) {
  switch (s) {
    case cmsSigRgbData:
      return TYPE_RGB_DBL;
    case cmsSigGrayData:
      return TYPE_GRAY_DBL;
    case cmsSigCmykData:
      return TYPE_CMYK_DBL;
    case cmsSigLabData:
      return TYPE_Lab_DBL;
    case cmsSigXYZData:
      return TYPE_XYZ_DBL;
    default:
      ORACLE_REQUIRE(0);
      return 0;
  }
}
static double rn(double v, int wide) {
  if (wide) {
    return v;
  }
  {
    volatile float f = (float)v;
    return f;
  }
}
static double scale(cmsColorSpaceSignature s, unsigned c) {
  return s == cmsSigCmykData || (s == cmsSigLabData && c == 0) ? 100. : 1.;
}
static void bits(const double* p, unsigned n, int wide) {
  unsigned c;
  for (c = 0; c < n; ++c) {
    if (c) {
      fputc(',', table);
    }
    if (wide) {
      uint64_t u;
      memcpy(&u, p + c, 8);
      fprintf(table, "%016" PRIx64, u);
    } else {
      float f = (float)p[c];
      uint32_t u;
      memcpy(&u, &f, 4);
      fprintf(table, "%08" PRIx32, u);
    }
  }
}
static void emit(unsigned v, const char* name, int wide, unsigned row,
                 unsigned ni, unsigned no, const double* in,
                 const double* out) {
  unsigned c;
  int finite = 1;
  for (c = 0; c < no; ++c) {
    finite &= isfinite(out[c]);
  }
  fprintf(table, "%s\t%s\tf%d\t%u\t%u\t%u\t%s\t", mode[v], name, wide ? 64 : 32,
          row, ni, no, finite ? "ok" : "nonfinite");
  bits(in, ni, wide);
  fputc('\t', table);
  bits(out, no, wide);
  fputc('\n', table);
}
static void run(const char* name, cmsHPROFILE* profiles, unsigned count,
                cmsColorSpaceSignature si, cmsColorSpaceSignature so,
                unsigned ni, unsigned no, const double* samples,
                unsigned rows) {
  cmsBool bpc[4] = {0};
  cmsUInt32Number intents[4] = {1, 1, 1, 1};
  cmsFloat64Number adaptation[4] = {1, 1, 1, 1};
  unsigned v, row, c;
  int wide;
  for (v = 0; v < 2; ++v) {
    cmsHTRANSFORM t = cmsCreateExtendedTransform(
        ctx, count, profiles, bpc, intents, adaptation, NULL, 0, format(si),
        format(so), flags[v]);
    ORACLE_REQUIRE(t);
    for (wide = 0; wide < 2; ++wide) {
      for (row = 0; row < rows; ++row) {
        double in[4] = {0}, ei[4] = {0}, eo[4] = {0}, out[4] = {0};
        for (c = 0; c < ni; ++c) {
          in[c] = rn(samples[row * ni + c], wide);
          ei[c] = in[c] * scale(si, c);
        }
        cmsDoTransform(t, ei, eo, 1);
        for (c = 0; c < no; ++c) {
          out[c] = rn(eo[c] / scale(so, c), wide);
        }
        emit(v, name, wide, row, ni, no, in, out);
      }
    }
    cmsDeleteTransform(t);
  }
}
static void gamut(cmsHPROFILE source, cmsHPROFILE proof, const double* samples,
                  unsigned rows) {
  cmsHPROFILE lab = cmsCreateLab4ProfileTHR(ctx, NULL);
  cmsHPROFILE profiles[4] = {source, proof, proof, lab};
  cmsBool bpc[4] = {0};
  cmsUInt32Number intents[4] = {1, 1, 1, 1};
  cmsFloat64Number adaptation[4] = {1, 1, 1, 1};
  /* Source/proof RGB is the intersection with stock's original grid recipe.
   * Gamut construction deliberately has no NOOPTIMIZE flag in either mode. */
  cmsHTRANSFORM owner = cmsCreateExtendedTransform(
      ctx, 4, profiles, bpc, intents, adaptation, proof, 1, TYPE_RGB_DBL,
      TYPE_Lab_DBL, cmsFLAGS_GAMUTCHECK | cmsFLAGS_NOCACHE);
  _cmsTRANSFORM* t = (_cmsTRANSFORM*)owner;
  unsigned row, c, v;
  int wide;
  ORACLE_REQUIRE(t && t->GamutCheck && t->FromInputFloat);
  ORACLE_REQUIRE(cmsPipelineInputChannels(t->GamutCheck) == 3 &&
                 cmsPipelineOutputChannels(t->GamutCheck) == 1);
  for (v = 0; v < 2; ++v) {
    for (wide = 0; wide < 2; ++wide) {
      for (row = 0; row < rows; ++row) {
        double in[3], ei[3], out[1];
        float normalized[cmsMAXCHANNELS] = {0}, score[1] = {0};
        for (c = 0; c < 3; ++c) {
          ei[c] = in[c] = rn(samples[3 * row + c], wide);
        }
        t->FromInputFloat(t, normalized, (cmsUInt8Number*)ei, 0);
        cmsPipelineEvalFloat(normalized, score, t->GamutCheck);
        ORACLE_REQUIRE(isfinite(score[0]));
        out[0] = score[0] > 0 ? 1 : 0;
        emit(v, "gamut_rgb", wide, row, 3, 1, in, out);
      }
    }
  }
  cmsDeleteTransform(owner);
  cmsCloseProfile(lab);
}

/* Independently expressed source-domain hull builder. The stock RGB/RGB
 * owning pipeline above remains a second cross-check of this construction. */
typedef struct {
  cmsHTRANSFORM source, forward, reverse;
  double threshold;
} Hull;
static int hull_sample(const cmsUInt16Number in[], cmsUInt16Number out[],
                       void* cargo) {
  Hull* h = cargo;
  cmsCIELab original, first, second;
  cmsUInt16Number device[cmsMAXCHANNELS];  // NOLINT(runtime/arrays)
  cmsDoTransform(h->source, in, &original, 1);
  cmsDoTransform(h->forward, &original, device, 1);
  cmsDoTransform(h->reverse, device, &first, 1);
  cmsDoTransform(h->forward, &first, device, 1);
  cmsDoTransform(h->reverse, device, &second, 1);
  double a = cmsDeltaE(&original, &first), b = cmsDeltaE(&first, &second),
         value = 0;
  ORACLE_REQUIRE(isfinite(a) && isfinite(b));
  if (a < h->threshold && (b < h->threshold || b > h->threshold)) {
    value = 0;
  } else if (a > h->threshold && b < h->threshold) {
    value = a - h->threshold;
  } else {
    double ratio = b == 0 ? a : a / b;
    ORACLE_REQUIRE(isfinite(ratio));
    if (ratio > h->threshold) {
      value = ratio - h->threshold;
    }
  }
  ORACLE_REQUIRE(value >= 0 && value < 65535.5);
  out[0] = (cmsUInt16Number)floor(value + .5);
  return 1;
}
static void source_gamut(const char* name, cmsHPROFILE source,
                         cmsHPROFILE proof, const double* samples,
                         unsigned rows) {
  const cmsColorSpaceSignature space = cmsGetColorSpace(source);
  const unsigned n = cmsChannelsOfColorSpace(space),
                 np = cmsChannelsOfColorSpace(cmsGetColorSpace(proof)),
                 grid = n == 4 ? 23 : 49;
  const cmsUInt32Number input16 = CHANNELS_SH(n) | BYTES_SH(2),
                        proof16 = CHANNELS_SH(np) | BYTES_SH(2);
  cmsHPROFILE lab = cmsCreateLab4ProfileTHR(ctx, NULL),
              chain[2] = {source, lab};
  cmsBool bpc[2] = {0, 0};
  cmsUInt32Number intents[2] = {1, 1};
  cmsFloat64Number adaptation[2] = {1, 1};
  Hull h = {0};
  h.threshold = cmsIsMatrixShaper(proof) ? 1. : 5.;
  h.source =
      cmsCreateExtendedTransform(ctx, 2, chain, bpc, intents, adaptation, NULL,
                                 0, input16, TYPE_Lab_DBL, cmsFLAGS_NOCACHE);
  h.forward = cmsCreateTransformTHR(ctx, lab, TYPE_Lab_DBL, proof, proof16, 1,
                                    cmsFLAGS_NOCACHE);
  h.reverse = cmsCreateTransformTHR(ctx, proof, proof16, lab, TYPE_Lab_DBL, 1,
                                    cmsFLAGS_NOCACHE);
  ORACLE_REQUIRE(h.source && h.forward && h.reverse);
  cmsPipeline* table = cmsPipelineAlloc(ctx, n, 1);
  cmsStage* clut = cmsStageAllocCLut16bit(ctx, grid, n, 1, NULL);
  ORACLE_REQUIRE(table && clut &&
                 cmsPipelineInsertStage(table, cmsAT_END, clut));
  ORACLE_REQUIRE(cmsStageSampleCLut16bit(clut, hull_sample, &h, 0));
  cmsHTRANSFORM formatter_owner = cmsCreateTransformTHR(
      ctx, source, format(space), lab, TYPE_Lab_DBL, 1, cmsFLAGS_NOCACHE);
  _cmsTRANSFORM* formatter = (_cmsTRANSFORM*)formatter_owner;
  ORACLE_REQUIRE(formatter && formatter->FromInputFloat);
  for (unsigned v = 0; v < 2; ++v) {
    for (int wide = 0; wide < 2; ++wide) {
      for (unsigned row = 0; row < rows; ++row) {
        double in[4] = {0}, adapted[4] = {0}, out[1];
        float normalized[cmsMAXCHANNELS] = {0}, score = 0;
        for (unsigned c = 0; c < n; ++c) {
          in[c] = rn(samples[row * n + c], wide);
          adapted[c] = in[c] * scale(space, c);
        }
        formatter->FromInputFloat(formatter, normalized,
                                  (cmsUInt8Number*)adapted, 0);
        cmsPipelineEvalFloat(normalized, &score, table);
        ORACLE_REQUIRE(isfinite(score));
        out[0] = score > 0 ? 1 : 0;
        emit(v, name, wide, row, n, 1, in, out);
      }
    }
  }
  cmsDeleteTransform(formatter_owner);
  cmsPipelineFree(table);
  cmsDeleteTransform(h.source);
  cmsDeleteTransform(h.forward);
  cmsDeleteTransform(h.reverse);
  cmsCloseProfile(lab);
}
static void settings(const char* name, cmsHPROFILE source, cmsHPROFILE proof,
                     cmsHPROFILE dest, unsigned intent, unsigned proof_intent,
                     int black_point, const double* samples, unsigned rows) {
  cmsHPROFILE profiles[4] = {source, proof ? proof : dest, proof, dest};
  cmsBool bpc[4] = {0, black_point, 0, 0};
  cmsUInt32Number intents[4] = {intent, intent, 1, proof_intent};
  cmsFloat64Number adaptation[4] = {1, 1, 1, 1};
  unsigned n = proof ? 4 : 2;
  if (proof) {
    bpc[0] = black_point;
  }
  for (unsigned v = 0; v < 2; ++v) {
    cmsHTRANSFORM t = cmsCreateExtendedTransform(
        ctx, n, profiles, bpc, intents, adaptation, NULL, 0, TYPE_RGB_DBL,
        TYPE_RGB_DBL, flags[v] | (proof ? cmsFLAGS_SOFTPROOFING : 0));
    ORACLE_REQUIRE(t);
    for (int wide = 0; wide < 2; ++wide) {
      for (unsigned row = 0; row < rows; ++row) {
        double in[3], out[3];
        for (unsigned c = 0; c < 3; ++c) {
          in[c] = rn(samples[3 * row + c], wide);
        }
        cmsDoTransform(t, in, out, 1);
        for (unsigned c = 0; c < 3; ++c) {
          out[c] = rn(out[c], wide);
        }
        emit(v, name, wide, row, 3, 3, in, out);
      }
    }
    cmsDeleteTransform(t);
  }
}

static const double rv[] = {0,
                            0,
                            0,
                            1,
                            1,
                            1,
                            1,
                            0,
                            0,
                            0,
                            1,
                            0,
                            0,
                            0,
                            1,
                            .2,
                            .4,
                            .7,
                            -.25,
                            .1,
                            1.2,
                            1.5,
                            -.5,
                            3,
                            -0.,
                            0,
                            -0.,
                            .5,
                            .5,
                            .5,
                            .01,
                            .99,
                            .2,
                            0x1.000001p-1,
                            0x1.fffffep-1,
                            0x1p-149};
static const double gv[] = {-.1, -0., .18, .5, 1, 1.5};
static const double cv[] = {0,  0,  0,   0,  1,  0, 0,   0, 0, 1,  0,
                            0,  0,  0,   1,  0,  0, 0,   0, 1, .2, .4,
                            .6, .1, -.1, .2, .3, 0, 1.5, 0, 0, 0};
static const double lv[] = {0, 0,  0,   1,   0,   0,   .5, 0,
                            0, .5, 80,  -70, .75, -40, 60, -.1,
                            0, 0,  1.2, 10,  -10, -0., 0,  -0.};
static const double xv[] = {0,   0,  0,  .9642, 1, .8249, .2,  .3, .4,
                            -.1, .2, .3, 1.4,   2, .8,    -0., 0,  -0.};
enum { S, L, P, G, LAB, XYZ, C, LINK, LAB_ABS, XYZ_ABS, N };
int main(int argc, char** argv) {
  Profile p[N] = {{0}};
  cmsToneCurve* gamma;
  cmsHTRANSFORM t;
  char filename[4096];
  unsigned i;
  ORACLE_REQUIRE(argc == 2);
  directory = argv[1];
  ORACLE_REQUIRE(!fesetround(FE_TONEAREST));
  ORACLE_REQUIRE(LCMS_VERSION == 2190);
  ctx = cmsCreateContext(NULL, NULL);
  ORACLE_REQUIRE(ctx);
  cmsSetLogErrorHandlerTHR(ctx, error);
  cmsSetAdaptationStateTHR(ctx, 1);
  p[S] = freeze(cmsCreate_sRGBProfileTHR(ctx), "srgb.icc");
  p[L] = freeze(rgb(1, 0), "linear-rgb.icc");
  p[P] = freeze(rgb(2.2, 1), "narrow-rgb.icc");
  gamma = cmsBuildGamma(ctx, 2.2);
  p[G] = freeze(cmsCreateGrayProfileTHR(ctx, cmsD50_xyY(), gamma),
                "gray-gamma22.icc");
  cmsFreeToneCurve(gamma);
  p[LAB] = freeze(ordinary_pcs(cmsCreateLab4ProfileTHR(ctx, NULL)),
                  "lab-identity.icc");
  p[XYZ] =
      freeze(ordinary_pcs(cmsCreateXYZProfileTHR(ctx)), "xyz-identity.icc");
  p[LAB_ABS] = freeze(cmsCreateLab4ProfileTHR(ctx, NULL), "lab-abstract.icc");
  p[XYZ_ABS] = freeze(cmsCreateXYZProfileTHR(ctx), "xyz-abstract.icc");
  p[C] = freeze(cmyk(), "synthetic-cmyk.icc");
  t = cmsCreateTransformTHR(ctx, p[S].h, TYPE_RGB_DBL, p[L].h, TYPE_RGB_DBL, 1,
                            flags[0]);
  ORACLE_REQUIRE(t);
  p[LINK] = freeze(cmsTransform2DeviceLink(t, 4.3, cmsFLAGS_KEEP_SEQUENCE),
                   "rgb-devicelink.icc");
  cmsDeleteTransform(t);
  path(filename, sizeof(filename), "reference.tsv");
  table = fopen(filename, "wb");
  ORACLE_REQUIRE(table);
  fprintf(table,
          "# LCMS %d; serialized/reopened; double formatters; relative intent; "
          "BPC off; adaptation 1\n",
          LCMS_VERSION);
  fputs(
      "variant\tcase\tdtype\trow\tnin\tnout\tstatus\tinput_bits\toutput_bits\n",
      table);
#define RUN(name, pi, po, si, so, ni, no, samples)                         \
  run(name, (cmsHPROFILE[]){p[pi].h, p[po].h}, 2, si, so, ni, no, samples, \
      sizeof(samples) / sizeof(double) / ni)
  RUN("profiles", S, L, cmsSigRgbData, cmsSigRgbData, 3, 3, rv);
  run("devicelink", (cmsHPROFILE[]){p[LINK].h}, 1, cmsSigRgbData, cmsSigRgbData,
      3, 3, rv, 12);
  run("chain3", (cmsHPROFILE[]){p[S].h, p[LAB_ABS].h, p[L].h}, 3, cmsSigRgbData,
      cmsSigRgbData, 3, 3, rv, 12);
  run("chain4", (cmsHPROFILE[]){p[S].h, p[LAB_ABS].h, p[XYZ_ABS].h, p[L].h}, 4,
      cmsSigRgbData, cmsSigRgbData, 3, 3, rv, 12);
  run("proof_rgb", (cmsHPROFILE[]){p[S].h, p[P].h, p[P].h, p[S].h}, 4,
      cmsSigRgbData, cmsSigRgbData, 3, 3, rv, 12);
  run("proof_cmyk", (cmsHPROFILE[]){p[S].h, p[C].h, p[C].h, p[S].h}, 4,
      cmsSigRgbData, cmsSigRgbData, 3, 3, rv, 12);
  RUN("gray_to_rgb", G, S, cmsSigGrayData, cmsSigRgbData, 1, 3, gv);
  RUN("cmyk_to_rgb", C, S, cmsSigCmykData, cmsSigRgbData, 4, 3, cv);
  RUN("rgb_to_cmyk", S, C, cmsSigRgbData, cmsSigCmykData, 3, 4, rv);
  RUN("rgb_to_lab", S, LAB, cmsSigRgbData, cmsSigLabData, 3, 3, rv);
  RUN("lab_to_rgb", LAB, S, cmsSigLabData, cmsSigRgbData, 3, 3, lv);
  RUN("rgb_to_xyz", S, XYZ, cmsSigRgbData, cmsSigXYZData, 3, 3, rv);
  RUN("xyz_to_rgb", XYZ, S, cmsSigXYZData, cmsSigRgbData, 3, 3, xv);
  gamut(p[S].h, p[P].h, rv, 12);
  source_gamut("gamut_gray_rgb", p[G].h, p[P].h, gv, 6);
  source_gamut("gamut_cmyk_rgb", p[C].h, p[P].h, cv, 8);
  source_gamut("gamut_rgb_cmyk", p[S].h, p[C].h, rv, 12);
  source_gamut("gamut_rgb_independent", p[S].h, p[P].h, rv, 12);
  for (unsigned intent = 0; intent < 4; ++intent) {
    for (int bpc = 0; bpc < 2; ++bpc) {
      if (intent == 3 && bpc) {
        continue;
      }
      char name[80];
      snprintf(name, sizeof(name), "settings_%u_%d", intent, bpc);
      settings(name, p[S].h, NULL, p[L].h, intent, 1, bpc, rv, 12);
      for (unsigned proof_intent = 1; proof_intent < 4; proof_intent += 2) {
        snprintf(name, sizeof(name), "proofsettings_%u_%u_%d", intent,
                 proof_intent, bpc);
        settings(name, p[S].h, p[C].h, p[S].h, intent, proof_intent, bpc, rv,
                 12);
      }
    }
  }
  ORACLE_REQUIRE(!fclose(table));
  for (i = 0; i < N; ++i) {
    cmsCloseProfile(p[i].h);
    free(p[i].bytes);
  }
  cmsDeleteContext(ctx);
  return 0;
}
