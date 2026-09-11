#pragma once

/** @brief Bounded rectangle-request emitter for trusted Metal discovery code.
 * @note Table has a 16-byte zero-initialized header (uint32 attempted count,
 * overflow, zero, zero), followed by capacity 144-byte records. Each record
 * stores uint32 port/roles/rank/zero, uint64 offsets[8], uint64 extents[8], all
 * little-endian. The host declares at most UINT32_MAX total emit attempts;
 * shader code must respect that bound. Excess capacity sets overflow and never
 * writes outside the table. No numerical output may be published on overflow.
 */
// NOLINTBEGIN(whitespace/indent_namespace)
#define PS_GPU_DISCOVERY_MSL_V9                                           \
  "#include <metal_stdlib>\nusing namespace metal;\n"                     \
  "bool ps_discovery_emit(device atomic_uint* table, uint capacity,"      \
  " uint port, uint roles, uint rank, thread const ulong* offsets,"       \
  " thread const ulong* extents) {"                                       \
  " uint index=atomic_fetch_add_explicit(table,1u,memory_order_relaxed);" \
  " if(index>=capacity) {atomic_store_explicit(table+1,1u,"               \
  " memory_order_relaxed);return false;}"                                 \
  " device uint* row=reinterpret_cast<device uint*>(table)+4+index*36;"   \
  " row[0]=port;row[1]=roles;row[2]=rank;row[3]=0;"                       \
  " for(uint a=0;a<8;++a) {ulong o=a<rank?offsets[a]:0;"                  \
  " ulong n=a<rank?extents[a]:0;row[4+a*2]=uint(o);"                      \
  " row[5+a*2]=uint(o>>32);row[20+a*2]=uint(n);"                          \
  " row[21+a*2]=uint(n>>32);}return true;}\n"
// NOLINTEND
