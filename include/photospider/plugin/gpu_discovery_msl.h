#pragma once

/** @brief Bounded rectangle-request emitters for trusted Metal discovery code.
 * The zero-initialized table has a 16-byte header (attempted-record count,
 * overflow flag, and two reserved zero words), followed by `capacity` 144-byte
 * records. Each record contains four uint32 words, then `uint64_t offsets[8]`
 * and `uint64_t extents[8]`, all little-endian. The Result v2 helper writes
 * input, roles, rank, and tensor slot in those first four words. The legacy
 * v11 helper writes input, roles, rank, and zero in the fourth word. The host
 * bounds and charges total emit attempts; callback code must configure GPU
 * work so the emitter does not exceed the candidate bound. An emit beyond
 * table capacity sets overflow and writes no record. The host rejects
 * overflow before attaching requests or publishing numerical output.
 */
// NOLINTBEGIN(whitespace/indent_namespace)
#define PS_GPU_DISCOVERY_MSL_BODY(signature, slot_expression)             \
  "#include <metal_stdlib>\nusing namespace metal;\n" signature           \
  " {"                                                                    \
  " uint index=atomic_fetch_add_explicit(table,1u,memory_order_relaxed);" \
  " if(index>=capacity) {atomic_store_explicit(table+1,1u,"               \
  " memory_order_relaxed);return false;}"                                 \
  " device uint* row=reinterpret_cast<device uint*>(table)+4+index*36;"   \
  " row[0]=port;row[1]=roles;row[2]=rank;row[3]=" slot_expression         \
  ";"                                                                     \
  " for(uint a=0;a<8;++a) {ulong o=a<rank?offsets[a]:0;"                  \
  " ulong n=a<rank?extents[a]:0;row[4+a*2]=uint(o);"                      \
  " row[5+a*2]=uint(o>>32);row[20+a*2]=uint(n);"                          \
  " row[21+a*2]=uint(n>>32);}return true;}\n"
#define PS_GPU_DISCOVERY_MSL_V11                                         \
  PS_GPU_DISCOVERY_MSL_BODY(                                             \
      "bool ps_discovery_emit(device atomic_uint* table, uint capacity," \
      " uint port, uint roles, uint rank, thread const ulong* offsets,"  \
      " thread const ulong* extents)",                                   \
      "0")
#define PS_RESULT_GPU_DISCOVERY_MSL_V2                                     \
  PS_GPU_DISCOVERY_MSL_BODY(                                               \
      "bool ps_result_discovery_emit(device atomic_uint* table, uint "     \
      "capacity,"                                                          \
      " uint port, uint slot, uint roles, uint rank, thread const ulong* " \
      "offsets,"                                                           \
      " thread const ulong* extents)",                                     \
      "slot")
// NOLINTEND
