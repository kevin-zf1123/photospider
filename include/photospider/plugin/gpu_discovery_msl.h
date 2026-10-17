#pragma once

/** @brief Bounded tile-request emitter for trusted Metal discovery code.
 * A zero-initialized table has a 16-byte header: uint32 attempted count,
 * uint32 overflow flag, and two reserved zero words. Each 32-byte record is
 * uint32 input, kind, tensor_slot, reserved followed by uint64 begin and end,
 * all little-endian. Interval=0 selects [begin,end) of one tensor member.
 * Descriptor=1 requires tensor_slot, begin and end zero. Reserved is zero.
 * An emit beyond capacity sets overflow and writes no record. The caller
 * must bound total attempts by the declared candidate count. The host checks
 * every record before attaching any Need, including member and interval bounds.
 */
// NOLINTBEGIN(whitespace/indent_namespace)
#define PS_RESULT_GPU_DISCOVERY_MSL_V6                                     \
  "#include <metal_stdlib>\nusing namespace metal;\n"                      \
  "bool ps_result_discovery_emit(device atomic_uint* table,uint capacity," \
  "uint port,uint kind,uint slot,ulong begin,ulong end) {"                 \
  "uint index=atomic_fetch_add_explicit(table,1u,memory_order_relaxed);"   \
  "if(index>=capacity){atomic_store_explicit(table+1,1u,"                  \
  "memory_order_relaxed);return false;}"                                   \
  "device uint* row=reinterpret_cast<device uint*>(table)+4+index*8;"      \
  "row[0]=port;row[1]=kind;row[2]=slot;row[3]=0;"                          \
  "row[4]=uint(begin);row[5]=uint(begin>>32);"                             \
  "row[6]=uint(end);row[7]=uint(end>>32);"                                 \
  "return true;}\n"
// NOLINTEND
