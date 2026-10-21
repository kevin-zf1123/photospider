#pragma once
#include <stddef.h>
#include <stdint.h>
#if defined(_WIN32)
#define PS_COLOR_API __declspec(dllexport)
#else
#define PS_COLOR_API __attribute__((visibility("default")))
#endif
#ifdef __cplusplus
extern "C" {
#endif
/* Private, versioned engine boundary. No C++ object crosses the shared module.
 */
typedef struct ps_color_host_v1 {
  void* context;
  void* (*allocate)(void*, size_t, size_t);
  void (*release)(void*);
  int (*work)(void*, uint64_t);
} ps_color_host_v1;
typedef struct ps_color_bytes_v1 {
  const unsigned char* data;
  size_t size;
} ps_color_bytes_v1;
typedef struct ps_icc_stage_v1 {
  ps_color_bytes_v1 profile;
  int direction; /* 0 device->PCS, 1 PCS->device, 2 forward */
  int intent;    /* -1 means DeviceLink header */
  int bpc;       /* -1 means not applicable */
} ps_icc_stage_v1;
typedef struct ps_icc_request_v1 {
  int kind; /* 0 profiles, 1 DeviceLink, 2 chain, 3 proof, 4 gamut */
  int input_model, output_model; /* 0 gray, 1 rgb, 2 cmyk, 3 xyz, 4 Lab */
  int reference;
  int rendering_intent, proofing_intent, bpc;
  const ps_icc_stage_v1* stages;
  size_t count;
} ps_icc_request_v1;
PS_COLOR_API void* ps_color_icc_create_v1(const ps_icc_request_v1*,
                                          const ps_color_host_v1*, char*,
                                          size_t);
PS_COLOR_API void ps_color_icc_release_v1(void*);
PS_COLOR_API int ps_color_icc_apply_v1(void*, const double*, double*, unsigned);
PS_COLOR_API const char* ps_color_icc_trace_v1(const void*);
PS_COLOR_API const char* ps_color_engine_identity_v1(void);
typedef struct ps_ocio_request_v1 {
  int kind, inverse, reference, data_bypass, looks_bypass, allow_bridge,
      semantic;
  int interpolation;
  ps_color_bytes_v1 snapshot, file_data, tree;
  const char *identity, *source, *destination, *display, *view, *looks, *named;
  const char *file, *ccc_id, *expected_input, *expected_output;
} ps_ocio_request_v1;
typedef struct ps_ocio_info_v1 {
  char input[256], output[256];
  int input_reference, output_reference;
} ps_ocio_info_v1;
PS_COLOR_API void* ps_color_ocio_create_v1(const ps_ocio_request_v1*,
                                           const ps_color_host_v1*,
                                           ps_ocio_info_v1*, char*, size_t);
PS_COLOR_API void ps_color_ocio_release_v1(void*);
PS_COLOR_API int ps_color_ocio_apply_v1(void*, float*, unsigned);
PS_COLOR_API const char* ps_color_ocio_trace_v1(const void*);
PS_COLOR_API void ps_color_release_allocations_v1(void*);
#ifdef __cplusplus
}
#endif
