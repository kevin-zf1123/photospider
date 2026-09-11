#pragma once

/** @brief MSL helper for the 80-byte FragmentAtlasPlan directory slot format.
 * @note Concatenate with a trusted kernel using ps_atlas_address. The directory
 * binding contains at least slots*80 bytes; payload contains payload_bytes.
 * shape/tile/coordinate have rank entries. All sampling boundary mapping is
 * performed in global coordinates before lookup. False is an explicit missing
 * sample; callers must not silently substitute it for a present source value.
 */
#define PS_FRAGMENT_ATLAS_MSL_V9                                               \
  "#include <metal_stdlib>\n"                                                  \
  "using namespace metal;\n"                                                   \
  "inline bool ps_atlas_address(device const ulong* directory, ulong slots,\n" \
  " constant ulong* shape, constant ulong* tile, uint rank,\n"                 \
  " thread const ulong* at, ulong payload_bytes, uint width,\n"                \
  " thread ulong& address) {\n"                                                \
  " if (!rank || rank>8 || !slots || (slots&(slots-1)) ||\n"                   \
  "     (width!=1 && width!=4 && width!=8)) return false;\n"                   \
  " ulong key[8]={0}, bit=0, volume=1;\n"                                      \
  " ulong hash=14695981039346656037ul;\n"                                      \
  " for (uint axis=0; axis<rank; ++axis) {\n"                                  \
  "  if (!tile[axis] || tile[axis]>64/volume || at[axis]>=shape[axis])\n"      \
  "   return false;\n"                                                         \
  "  volume*=tile[axis]; key[axis]=at[axis]/tile[axis];\n"                     \
  "  bit=bit*tile[axis]+at[axis]%tile[axis];\n"                                \
  "  for (uint byte=0; byte<8; ++byte) {\n"                                    \
  "   hash^=(key[axis]>>(byte*8))&255ul; hash*=1099511628211ul;\n"             \
  "  }\n"                                                                      \
  " }\n"                                                                       \
  " ulong slot=hash&(slots-1);\n"                                              \
  " for (ulong probe=0; probe<slots; ++probe) {\n"                             \
  "  device const ulong* entry=directory+slot*10;\n"                           \
  "  ulong mask=entry[8]; if (!mask) return false;\n"                          \
  "  bool same=true; for (uint axis=0; axis<8; ++axis)\n"                      \
  "   same=same && entry[axis]==key[axis];\n"                                  \
  "  if (same) {\n"                                                            \
  "   if (!(mask&(1ul<<bit))) return false;\n"                                 \
  "   ulong prior=mask&((1ul<<bit)-1), count=0;\n"                             \
  "   while (prior) { prior&=prior-1; ++count; }\n"                            \
  "   ulong offset=entry[9], extra=count*width;\n"                             \
  "   if (offset>payload_bytes || extra>payload_bytes-offset ||\n"             \
  "       width>payload_bytes-offset-extra) return false;\n"                   \
  "   address=offset+extra; return true;\n"                                    \
  "  }\n"                                                                      \
  "  slot=(slot+1)&(slots-1);\n"                                               \
  " }\n"                                                                       \
  " return false;\n"                                                           \
  "}\n"
