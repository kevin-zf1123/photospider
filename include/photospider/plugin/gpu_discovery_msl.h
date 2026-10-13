#pragma once

/** @brief Bounded tile-request emitter for trusted Metal discovery code.
 * A zero-initialized table has a 16-byte header: uint32 attempted count,
 * uint32 overflow flag, and two reserved zero words. Each 16-byte record is
 * uint32 input, uint32 kind and uint64 tile, all little-endian. Kinds are
 * Tile=0, Whole=1 and Descriptor=2; Whole/Descriptor require tile zero.
 * An emit beyond capacity sets overflow and writes no record. The caller
 * must bound total attempts by the declared candidate count. The host checks
 * every record before attaching any Need, including tile bounds when another
 * record for the same input requests Whole.
 */
// NOLINTBEGIN(whitespace/indent_namespace)
#define PS_RESULT_GPU_DISCOVERY_MSL_V3                                     \
  "#include <metal_stdlib>\nusing namespace metal;\n"                      \
  "bool ps_result_discovery_emit(device atomic_uint* table,uint capacity," \
  "uint port,uint kind,ulong tile) {"                                      \
  "uint index=atomic_fetch_add_explicit(table,1u,memory_order_relaxed);"   \
  "if(index>=capacity){atomic_store_explicit(table+1,1u,"                  \
  "memory_order_relaxed);return false;}"                                   \
  "device uint* row=reinterpret_cast<device uint*>(table)+4+index*4;"      \
  "row[0]=port;row[1]=kind;row[2]=uint(tile);row[3]=uint(tile>>32);"       \
  "return true;}\n"
// NOLINTEND
