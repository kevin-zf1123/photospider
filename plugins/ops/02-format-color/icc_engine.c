#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "engine_memory.h"   // NOLINT(build/include_subdir)
#include "lcms2_internal.h"  // NOLINT(build/include_subdir)

typedef struct ps_icc_engine {
  cmsContext context;
  cmsHTRANSFORM transform;
  cmsPipeline* gamut;
  cmsHPROFILE opened[255], lab;
  ps_color_host_v1 host;
  unsigned inputs, outputs;
  char* trace;
  size_t trace_used, trace_capacity;
} ps_icc_engine;
static void trace(ps_icc_engine* e, const char* format, ...) {
  char text[512];
  va_list args;
  int count;
  size_t needed, capacity;
  va_start(args, format);
  count = vsnprintf(text, sizeof(text), format, args);
  va_end(args);
  if (count < 0 || (size_t)count >= sizeof(text)) {
    ps_engine_error("ICC trace event limit");
    return;
  }
  needed = e->trace_used + (size_t)count + 1;
  if (needed > 1024 * 1024) {
    ps_engine_error("ICC construction trace limit");
    return;
  }
  if (needed > e->trace_capacity) {
    char* replacement;
    capacity = e->trace_capacity ? e->trace_capacity : 1024;
    while (capacity < needed) {
      capacity *= 2;
    }
    replacement = ps_engine_realloc(e->trace, capacity);
    if (!replacement) {
      ps_engine_error("ICC trace allocation failed");
      return;
    }
    e->trace = replacement;
    e->trace_capacity = capacity;
  }
  memcpy(e->trace + e->trace_used, text, (size_t)count + 1);
  e->trace_used += (size_t)count;
}
void ps_engine_icc_route(void* profile, const char* route, unsigned intent) {
  ps_icc_engine* e = cmsGetContextUserData(cmsGetProfileContextID(profile));
  unsigned index;
  if (!e) {
    return;
  }
  for (index = 0; index < 255 && e->opened[index] != profile; ++index) {
  }
  if (index < 255) {
    trace(e, "resource_stage=%u route=%s intent=%u; ", index, route, intent);
  } else {
    trace(e, "generated_profile route=%s intent=%u; ", route, intent);
  }
}
void* ps_engine_icc_read_tag(void* profile, unsigned signature) {
  void* result = cmsReadTag(profile, (cmsTagSignature)signature);
  ps_icc_engine* e = cmsGetContextUserData(cmsGetProfileContextID(profile));
  if (e && result) {
    char name[5] = {(char)(signature >> 24), (char)(signature >> 16),
                    (char)(signature >> 8), (char)signature, 0};
    trace(e, "selected_tag=%s; ", name);
  }
  return result;
}
static const cmsColorSpaceSignature spaces[] = {cmsSigGrayData, cmsSigRgbData,
                                                cmsSigCmykData, cmsSigXYZData,
                                                cmsSigLabData};
static const cmsUInt32Number formats[] = {
    TYPE_GRAY_DBL, TYPE_RGB_DBL, TYPE_CMYK_DBL, TYPE_XYZ_DBL, TYPE_Lab_DBL};
static int pcs(cmsColorSpaceSignature s) {
  return s == cmsSigXYZData || s == cmsSigLabData;
}
static int ordinary(cmsProfileClassSignature c) {
  return c == cmsSigInputClass || c == cmsSigDisplayClass ||
         c == cmsSigOutputClass || c == cmsSigColorSpaceClass;
}
static void error_handler(cmsContext c, cmsUInt32Number code,
                          const char* message) {
  (void)c;
  (void)code;
  ps_engine_error(message);
}
static int construct(const ps_icc_request_v1* q, ps_icc_engine* e) {
  cmsHPROFILE profiles[255] = {0};
  cmsBool bpc[255] = {0};
  cmsUInt32Number intents[255] = {0};
  cmsFloat64Number adaptation[255];

  cmsColorSpaceSignature current;
  cmsUInt32Number flags = cmsFLAGS_NOCACHE;
  unsigned n = (unsigned)q->count, i;

  cmsSetLogErrorHandlerTHR(e->context, error_handler);
  cmsSetAdaptationStateTHR(e->context, 1.0);
  if (q->reference) {
    flags |= cmsFLAGS_NOOPTIMIZE;
  }
  for (i = 0; i < n; ++i) {
    if (q->stages[i].profile.size > UINT32_MAX) {
      goto fail;
    }
    profiles[i] = e->opened[i] =
        cmsOpenProfileFromMemTHR(e->context, q->stages[i].profile.data,
                                 (cmsUInt32Number)q->stages[i].profile.size);
    if (!profiles[i]) {
      goto fail;
    }
    adaptation[i] = 1.0;
    intents[i] = q->stages[i].intent < 0
                     ? cmsGetHeaderRenderingIntent(profiles[i])
                     : (cmsUInt32Number)q->stages[i].intent;
    bpc[i] = q->stages[i].bpc > 0;
  }
  if (q->kind == 3 || q->kind == 4) {
    if (n != (q->kind == 3 ? 3U : 2U)) {
      goto fail;
    }
    if (!ordinary(cmsGetDeviceClass(profiles[0])) ||
        (cmsGetDeviceClass(profiles[1]) != cmsSigDisplayClass &&
         cmsGetDeviceClass(profiles[1]) != cmsSigOutputClass) ||
        (cmsGetColorSpace(profiles[1]) != cmsSigGrayData &&
         cmsGetColorSpace(profiles[1]) != cmsSigRgbData &&
         cmsGetColorSpace(profiles[1]) != cmsSigCmykData)) {
      goto fail;
    }
    if (q->kind == 3 && (cmsGetDeviceClass(profiles[2]) != cmsSigDisplayClass ||
                         (cmsGetColorSpace(profiles[2]) != cmsSigRgbData &&
                          cmsGetColorSpace(profiles[2]) != cmsSigGrayData))) {
      goto fail;
    }
    profiles[3] = profiles[2];
    profiles[2] = profiles[1];
    if (q->kind == 4) {
      e->lab = cmsCreateLab4ProfileTHR(e->context, NULL);
      if (!e->lab) {
        goto fail;
      }
      profiles[3] = e->lab;
      flags = cmsFLAGS_GAMUTCHECK | cmsFLAGS_NOCACHE;
    } else {
      flags |= cmsFLAGS_SOFTPROOFING;
    }
    n = 4;
    intents[0] = q->rendering_intent;
    intents[1] =
        q->kind == 4 ? INTENT_RELATIVE_COLORIMETRIC : q->rendering_intent;
    intents[2] = INTENT_RELATIVE_COLORIMETRIC;
    intents[3] =
        q->kind == 4 ? INTENT_RELATIVE_COLORIMETRIC : q->proofing_intent;
    bpc[0] = bpc[1] = q->kind == 3 && q->bpc;
    bpc[2] = bpc[3] = FALSE;
    for (i = 0; i < 4; ++i) {
      adaptation[i] = 1.0;
    }
  }
  current = cmsGetColorSpace(profiles[0]);
  if (current != spaces[q->input_model]) {
    goto fail;
  }
  for (i = 0; i < n; ++i) {
    cmsProfileClassSignature cls = cmsGetDeviceClass(profiles[i]);
    int link = cls == cmsSigLinkClass || cls == cmsSigAbstractClass;
    int input = i == 0 && !link ? 1 : !pcs(current);
    cmsColorSpaceSignature in =
        input || link ? cmsGetColorSpace(profiles[i]) : cmsGetPCS(profiles[i]);
    cmsColorSpaceSignature out =
        input || link ? cmsGetPCS(profiles[i]) : cmsGetColorSpace(profiles[i]);
    int declared = link ? 2 : input ? 0 : 1;
    if ((!ordinary(cls) && !link) ||
        (in != current && !(pcs(in) && pcs(current)))) {
      goto fail;
    }
    if (cls == cmsSigAbstractClass && (i == 0 || i + 1 == n) &&
        !(q->kind == 4 && i == 3)) {
      goto fail;
    }
    if (q->kind == 0 && !ordinary(cls)) {
      goto fail;
    }
    if (q->kind == 1 && cls != cmsSigLinkClass) {
      goto fail;
    }
    if (q->kind < 3 && q->stages[i].direction != declared) {
      goto fail;
    }
    if (q->kind == 2 && cls == cmsSigLinkClass &&
        (q->stages[i].intent >= 0 || q->stages[i].bpc >= 0)) {
      goto fail;
    }
    if (q->kind == 2 && !link && input && q->stages[i].bpc >= 0) {
      goto fail;
    }
    if (intents[i] > 3 ||
        (intents[i] == INTENT_ABSOLUTE_COLORIMETRIC && bpc[i])) {
      goto fail;
    }
    trace(e,
          "stage=%u direction=%s intent=%u requested_bpc=%d effective_bpc=%s "
          "bridge=%s; ",
          i,
          link    ? "forward"
          : input ? "device_to_pcs"
                  : "pcs_to_device",
          intents[i], bpc[i],
          !link && input ? "not_applicable"
          : cmsGetProfileVersion(profiles[i]) >= 4.0 &&
                  (intents[i] == 0 || intents[i] == 2)
              ? "v4_forced_on"
          : bpc[i] ? "on"
                   : "off",
          in == current ? "none" : "XYZ_Lab");
    current = out;
  }
  if (current != spaces[q->kind == 4 ? 4 : q->output_model]) {
    goto fail;
  }
  e->inputs = (unsigned)cmsChannelsOfColorSpace(spaces[q->input_model]);
  e->outputs = q->kind == 4
                   ? 1U
                   : (unsigned)cmsChannelsOfColorSpace(spaces[q->output_model]);
  e->transform = cmsCreateExtendedTransform(
      e->context, n, profiles, bpc, intents, adaptation,
      q->kind == 4 ? profiles[1] : NULL, q->kind == 4 ? 1 : 0,
      formats[q->input_model] | PLANAR_SH(1),
      formats[q->kind == 4 ? 4 : q->output_model] | PLANAR_SH(1), flags);
  if (!e->transform) {
    goto fail;
  }
  if (q->kind == 4) {
    e->gamut = cmsGetTransformGamutCheckPipeline(e->transform);
    if (!e->gamut || cmsPipelineInputChannels(e->gamut) != e->inputs ||
        cmsPipelineOutputChannels(e->gamut) != 1) {
      goto fail;
    }
  }
  return 1;
fail:
  ps_engine_error(
      "ICC profile, declared direction, endpoint or transform construction is "
      "invalid");
  return 0;
}
static void close_profiles(ps_icc_engine* e) {
  unsigned i;
  for (i = 0; i < 255; ++i) {
    if (e->opened[i]) {
      cmsCloseProfile(e->opened[i]);
      e->opened[i] = NULL;
    }
  }
  if (e->lab) {
    cmsCloseProfile(e->lab);
    e->lab = NULL;
  }
}
void* ps_color_icc_create_v1(const ps_icc_request_v1* q,
                             const ps_color_host_v1* host, char* error,
                             size_t size) {
  ps_engine_scope_v1 scope;
  ps_engine_scope_v1* previous = ps_engine_scope;
  ps_icc_engine* e;
  int success = 0;
  memset(&scope, 0, sizeof(scope));
  scope.host = *host;
  scope.error = error;
  scope.error_size = size;
  if (size) {
    error[0] = 0;
  }
  ps_engine_scope = &scope;
  // Heap-owned cleanup state is not made indeterminate by setjmp/longjmp.
  e = (ps_icc_engine*)ps_engine_calloc(1, sizeof(ps_icc_engine));
  if (e) {
    e->host = *host;
    // Registration and teardown never run with the work jump armed.
    e->context = cmsCreateContext(NULL, e);
    if (e->context) {
      // Every profile is private to this externally serialized engine. Keep
      // the global context registry synchronized, but use no profile locks.
      _cmsRegisterMutexPlugin(e->context, NULL);
      scope.armed = 1;
      if (setjmp(scope.jump) == 0 && q && q->count > 0 && q->count <= 255 &&
          q->input_model >= 0 && q->input_model <= 4 && q->output_model >= 0 &&
          q->output_model <= 4) {
        success = construct(q, e);
      }
      scope.armed = 0;
      close_profiles(e);
      if (!success) {
        if (e->transform) {
          cmsDeleteTransform(e->transform);
        }
        cmsDeleteContext(e->context);
      }
    }
    if (!success) {
      ps_engine_free(e->trace);
      ps_engine_free(e);
      e = NULL;
    }
  }
  ps_engine_scope = previous;
  return e;
}
void ps_color_icc_release_v1(void* pointer) {
  ps_icc_engine* e = (ps_icc_engine*)pointer;
  if (!e) {
    return;
  }
  cmsDeleteTransform(e->transform);
  cmsDeleteContext(e->context);
  ps_engine_free(e->trace);
  ps_engine_free(e);
}
static int apply(ps_icc_engine* e, const double* input, double* output,
                 unsigned count) {
  _cmsTRANSFORM* transform = (_cmsTRANSFORM*)e->transform;
  cmsFloat32Number normalized[cmsMAXCHANNELS];  // NOLINT(runtime/arrays)
  if (e->gamut) {
    unsigned p;
    for (p = 0; p < count; ++p) {
      cmsFloat32Number score;
      transform->FromInputFloat(transform, normalized,
                                (cmsUInt8Number*)(input + p),
                                count * sizeof(double));
      cmsPipelineEvalFloat(normalized, &score, e->gamut);
      output[p] = score > 0 ? 1.0 : 0.0;
    }
  } else {
    cmsDoTransformLineStride(e->transform, input, output, count, 1, 0, 0,
                             count * sizeof(double), count * sizeof(double));
  }
  return 1;
}
int ps_color_icc_apply_v1(void* pointer, const double* input, double* output,
                          unsigned count) {
  ps_icc_engine* e = (ps_icc_engine*)pointer;
  ps_engine_scope_v1 scope;
  ps_engine_scope_v1* previous = ps_engine_scope;
  char error[512] = {0};
  int ok;
  memset(&scope, 0, sizeof(scope));
  scope.host = e->host;
  scope.error = error;
  scope.error_size = sizeof(error);
  ps_engine_scope = &scope;
  ok = apply(e, input, output, count);
  ps_engine_scope = previous;
  return ok && !error[0];
}
const char* ps_color_icc_trace_v1(const void* p) {
  return ((const ps_icc_engine*)p)->trace ? ((const ps_icc_engine*)p)->trace
                                          : "";
}
const char* ps_color_engine_identity_v1(void) {
  return "ps-color-2;lcms=2.19.1;ocio=2.5.2;build=" PS_COLOR_BUILD_HASH
         ";RN;no-contract;closed";
}
