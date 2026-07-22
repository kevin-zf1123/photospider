#ifndef INCLUDE_PHOTOSPIDER_POLICY_POLICY_PLUGIN_API_H_
#define INCLUDE_PHOTOSPIDER_POLICY_POLICY_PLUGIN_API_H_

/**
 * @file policy_plugin_api.h
 * @brief Pure C ABI v1 for Photospider policy-only plugins.
 *
 * A policy receives immutable, bounded ranking descriptors and returns one
 * opaque candidate identity. It never receives an executor, worker, device,
 * allocation service, resource grant, Run, Graph, completion route, logger,
 * or lifecycle callback. The Host retains every physical and lifecycle
 * capability and validates every returned byte before acting on a decision.
 *
 * The supported profile is an in-process natural-layout 64-bit C ABI with
 * eight-bit bytes, 32-bit `uint32_t`, 64-bit `uint64_t`, eight-byte pointers,
 * and eight-byte pointer/`uint64_t` alignment. Including this header while a
 * packing pragma or nondefault aggregate alignment is active fails a layout
 * assertion. A new record shape requires a new ABI generation; v1 accepts no
 * undersized, oversized, or tailed record.
 *
 * @note C++ inclusion adds C linkage and `noexcept` to every callback and
 * export. C11 inclusion uses the platform C calling convention directly.
 * @note A callback may run concurrently for independent contexts. A callback
 * may perform Host read-only policy inspection through an outer API, but it
 * must not retain borrowed snapshot memory or synchronously request policy
 * mutation from the same thread. The Host provides no timeout or forced
 * recovery when an honest in-process callback never returns.
 */

#include <limits.h>
#include <stddef.h>
#include <stdint.h>

#if defined(_WIN32)
#define PS_POLICY_CALL __cdecl
#define PS_POLICY_PLUGIN_EXPORT __declspec(dllexport)
#else
#define PS_POLICY_CALL
#define PS_POLICY_PLUGIN_EXPORT __attribute__((visibility("default")))
#endif

#if defined(__cplusplus)
#define PS_POLICY_NOEXCEPT noexcept
#define PS_POLICY_STATIC_ASSERT(condition, message) \
  static_assert(condition, message)
#define PS_POLICY_ALIGNOF(type) alignof(type)
extern "C" {
#else
#define PS_POLICY_NOEXCEPT
#define PS_POLICY_STATIC_ASSERT(condition, message) \
  _Static_assert(condition, message)
#define PS_POLICY_ALIGNOF(type) _Alignof(type)
#endif

/** @brief Numeric generation accepted by the two required v1 exports. */
#define PS_POLICY_PLUGIN_ABI_VERSION UINT32_C(1)

/** @brief Sentinel used when an Interactive candidate has no deadline. */
#define PS_POLICY_NO_DEADLINE_NS UINT64_MAX

/** @brief Successful callback completion. */
#define PS_POLICY_STATUS_OK UINT32_C(0)
/** @brief Plugin-detected invalid input or known-invalid output. */
#define PS_POLICY_STATUS_INVALID_ARGUMENT UINT32_C(1)
/** @brief Plugin-owned allocation failure. */
#define PS_POLICY_STATUS_OUT_OF_MEMORY UINT32_C(2)
/** @brief Requested type or class is unsupported by the plugin. */
#define PS_POLICY_STATUS_UNSUPPORTED UINT32_C(3)
/** @brief Plugin-internal failure with no more precise v1 category. */
#define PS_POLICY_STATUS_INTERNAL_ERROR UINT32_C(4)

/** @brief Interactive policy class numeric value. */
#define PS_POLICY_CLASS_INTERACTIVE UINT32_C(1)
/** @brief Throughput policy class numeric value. */
#define PS_POLICY_CLASS_THROUGHPUT UINT32_C(2)

/** @brief Metadata mask bit declaring Interactive support. */
#define PS_POLICY_CLASS_MASK_INTERACTIVE UINT32_C(0x1)
/** @brief Metadata mask bit declaring Throughput support. */
#define PS_POLICY_CLASS_MASK_THROUGHPUT UINT32_C(0x2)
/** @brief Complete set of class-mask bits recognized by ABI v1. */
#define PS_POLICY_CLASS_MASK_VALID UINT32_C(0x3)

/** @brief Decision kind selecting one candidate from the original snapshot. */
#define PS_POLICY_DECISION_SELECT UINT32_C(1)
/** @brief Decision kind explicitly declining to select a candidate. */
#define PS_POLICY_DECISION_ABSTAIN UINT32_C(2)

/** @brief Structure-kind value for `ps_policy_type_metadata_v1`. */
#define PS_POLICY_STRUCT_TYPE_METADATA UINT32_C(1)
/** @brief Structure-kind value for `ps_policy_create_args_v1`. */
#define PS_POLICY_STRUCT_CREATE_ARGS UINT32_C(2)
/** @brief Structure-kind value for `ps_policy_candidate_v1`. */
#define PS_POLICY_STRUCT_CANDIDATE UINT32_C(3)
/** @brief Structure-kind value for `ps_policy_selection_snapshot_v1`. */
#define PS_POLICY_STRUCT_SELECTION_SNAPSHOT UINT32_C(4)
/** @brief Structure-kind value for `ps_policy_decision_v1`. */
#define PS_POLICY_STRUCT_DECISION UINT32_C(5)
/** @brief Structure-kind value for `ps_policy_plugin_api_v1`. */
#define PS_POLICY_STRUCT_PLUGIN_API UINT32_C(6)

/** @brief Candidate originated from a private high-priority ready hint. */
#define PS_POLICY_CANDIDATE_FLAG_HIGH_PRIORITY_HINT UINT32_C(0x1)
/** @brief Candidate carries a finite monotonic deadline. */
#define PS_POLICY_CANDIDATE_FLAG_DEADLINE_PRESENT UINT32_C(0x2)
/** @brief Complete set of candidate flag bits recognized by ABI v1. */
#define PS_POLICY_CANDIDATE_FLAG_VALID UINT32_C(0x3)

/** @brief Fixed-width callback status domain; only five v1 values are valid. */
typedef uint32_t ps_policy_status_v1;
/** @brief Fixed-width policy class domain. */
typedef uint32_t ps_policy_class_v1;
/** @brief Fixed-width supported-class bit mask. */
typedef uint32_t ps_policy_class_mask_v1;
/** @brief Fixed-width decision-kind domain. */
typedef uint32_t ps_policy_decision_kind_v1;
/** @brief Fixed-width record-kind domain. */
typedef uint32_t ps_policy_struct_kind_v1;
/** @brief Fixed-width candidate-flags domain. */
typedef uint32_t ps_policy_candidate_flags_v1;

/**
 * @brief Borrowed immutable byte range used only for plugin metadata.
 *
 * Empty strings may use a null pointer. Nonempty ranges must be addressable
 * for the complete size and remain immutable until the candidate DSO unloads.
 * The Host copies metadata immediately while holding the DSO lease.
 */
typedef struct ps_policy_string_view_v1 {
  /** @brief Borrowed bytes, nullable only when `size == 0`. */
  const char* data;
  /** @brief Byte count, excluding any optional terminator. */
  uint64_t size;
} ps_policy_string_view_v1;

/**
 * @brief Copied type metadata returned for one API-table row.
 *
 * Names are 1..128 lowercase ASCII bytes matching
 * `[a-z][a-z0-9_.-]*`. Description and implementation version are valid UTF-8
 * of at most 4,096 bytes. `interactive` and `throughput` are Host-reserved.
 */
typedef struct ps_policy_type_metadata_v1 {
  /** @brief Exact value `sizeof(ps_policy_type_metadata_v1)`. */
  uint32_t struct_size;
  /** @brief Exact value `PS_POLICY_STRUCT_TYPE_METADATA`. */
  ps_policy_struct_kind_v1 struct_kind;
  /** @brief Canonical policy type name. */
  ps_policy_string_view_v1 name;
  /** @brief Reader-facing type description. */
  ps_policy_string_view_v1 description;
  /** @brief Diagnostic implementation version, not an ABI selector. */
  ps_policy_string_view_v1 implementation_version;
  /** @brief Nonzero subset of `PS_POLICY_CLASS_MASK_VALID`. */
  ps_policy_class_mask_v1 supported_class_mask;
  /** @brief Must remain zero in ABI v1. */
  uint32_t reserved0;
  /** @brief Must remain entirely zero in ABI v1. */
  uint64_t reserved[2];
} ps_policy_type_metadata_v1;

/**
 * @brief Immutable arguments for one policy-class binding context.
 *
 * The Host creates separate Interactive and Throughput contexts even when
 * both bindings use the same type. No field grants resource or execution
 * authority.
 */
typedef struct ps_policy_create_args_v1 {
  /** @brief Exact value `sizeof(ps_policy_create_args_v1)`. */
  uint32_t struct_size;
  /** @brief Exact value `PS_POLICY_STRUCT_CREATE_ARGS`. */
  ps_policy_struct_kind_v1 struct_kind;
  /** @brief One valid `PS_POLICY_CLASS_*` value. */
  ps_policy_class_v1 policy_class;
  /** @brief Must remain zero in ABI v1. */
  uint32_t reserved0;
  /** @brief Nonzero generation owned and checked by the Host. */
  uint64_t binding_generation;
  /** @brief Must remain entirely zero in ABI v1. */
  uint64_t reserved[2];
} ps_policy_create_args_v1;

/**
 * @brief Immutable authority-free descriptor for one admissible ready entry.
 *
 * Every identity, weight, work value, byte charge, age, and enqueue sequence
 * is nonzero where the Host contract requires it. Scores and flags are
 * recomputable Host metadata; the record contains no pointer or capability.
 */
typedef struct ps_policy_candidate_v1 {
  /** @brief Exact value `sizeof(ps_policy_candidate_v1)`. */
  uint32_t struct_size;
  /** @brief Exact value `PS_POLICY_STRUCT_CANDIDATE`. */
  ps_policy_struct_kind_v1 struct_kind;
  /** @brief Nonzero opaque identity unique for the entry lifetime. */
  uint64_t candidate_id;
  /** @brief Nonzero opaque Graph identity. */
  uint64_t graph_id;
  /** @brief Nonzero opaque Run identity. */
  uint64_t run_id;
  /** @brief Finite monotonic deadline or `PS_POLICY_NO_DEADLINE_NS`. */
  uint64_t deadline_ns;
  /** @brief Positive Host-authored service weight. */
  uint64_t weight;
  /** @brief Positive trusted work estimate. */
  uint64_t work_units;
  /** @brief Positive complete ready-store byte charge. */
  uint64_t ready_bytes;
  /** @brief Saturating Host-computed Graph service score. */
  uint64_t graph_service_score;
  /** @brief Saturating Host-computed Run service score. */
  uint64_t run_service_score;
  /** @brief Saturating same-class dispatch age. */
  uint64_t dispatch_age;
  /** @brief Nonzero stable enqueue sequence. */
  uint64_t enqueue_sequence;
  /** @brief Subset of `PS_POLICY_CANDIDATE_FLAG_VALID`. */
  ps_policy_candidate_flags_v1 flags;
  /** @brief Must remain zero in ABI v1. */
  uint32_t reserved0;
  /** @brief Must remain entirely zero in ABI v1. */
  uint64_t reserved[2];
} ps_policy_candidate_v1;

/**
 * @brief Borrowed exact-size selection input for one policy callback.
 *
 * `candidates` points to exactly `candidate_count` naturally aligned records
 * at the exact 120-byte stride. The entire array is borrowed only until
 * `select` returns and must not be retained by the plugin.
 */
typedef struct ps_policy_selection_snapshot_v1 {
  /** @brief Exact value `sizeof(ps_policy_selection_snapshot_v1)`. */
  uint32_t struct_size;
  /** @brief Exact value `PS_POLICY_STRUCT_SELECTION_SNAPSHOT`. */
  ps_policy_struct_kind_v1 struct_kind;
  /** @brief One valid `PS_POLICY_CLASS_*` value. */
  ps_policy_class_v1 policy_class;
  /** @brief Number of records in `candidates`, in `1..4096`. */
  uint32_t candidate_count;
  /** @brief Nonzero binding generation copied into the decision. */
  uint64_t binding_generation;
  /** @brief Nonzero immutable original-call snapshot generation. */
  uint64_t snapshot_generation;
  /** @brief Nonzero Host selection sequence. */
  uint64_t selection_sequence;
  /** @brief Exact value `sizeof(ps_policy_candidate_v1)` (120). */
  uint32_t candidate_stride;
  /** @brief Must remain zero in ABI v1. */
  uint32_t reserved0;
  /** @brief Borrowed nonnull naturally aligned candidate array. */
  const ps_policy_candidate_v1* candidates;
  /** @brief Must remain zero in ABI v1. */
  uint64_t reserved[1];
} ps_policy_selection_snapshot_v1;

/**
 * @brief Exact-size selection output initialized by the Host.
 *
 * `SELECT` returns one nonzero candidate id from the original snapshot;
 * `ABSTAIN` returns zero. Both forms echo the exact binding and snapshot
 * generations supplied by the Host.
 */
typedef struct ps_policy_decision_v1 {
  /** @brief Exact value `sizeof(ps_policy_decision_v1)`. */
  uint32_t struct_size;
  /** @brief Exact value `PS_POLICY_STRUCT_DECISION`. */
  ps_policy_struct_kind_v1 struct_kind;
  /** @brief `PS_POLICY_DECISION_SELECT` or `PS_POLICY_DECISION_ABSTAIN`. */
  ps_policy_decision_kind_v1 decision_kind;
  /** @brief Must remain zero in ABI v1. */
  uint32_t reserved0;
  /** @brief Exact nonzero binding generation from the call. */
  uint64_t binding_generation;
  /** @brief Exact nonzero snapshot generation from the call. */
  uint64_t snapshot_generation;
  /** @brief Selected nonzero identity, or zero only for `ABSTAIN`. */
  uint64_t candidate_id;
  /** @brief Must remain zero in ABI v1. */
  uint64_t reserved[1];
} ps_policy_decision_v1;

/** @brief Returns copied metadata for one zero-based type table row. */
typedef ps_policy_status_v1(PS_POLICY_CALL* ps_policy_get_metadata_fn_v1)(
    uint32_t type_index,
    ps_policy_type_metadata_v1* out_metadata) PS_POLICY_NOEXCEPT;

/**
 * @brief Creates one class-specific logical instance.
 *
 * The Host initializes `*out_context` to null. Successful null is a valid
 * stateless instance and still receives exactly one `destroy` call. Non-OK
 * must leave null and reclaim every partial plugin allocation.
 */
typedef ps_policy_status_v1(PS_POLICY_CALL* ps_policy_create_fn_v1)(
    uint32_t type_index, const ps_policy_create_args_v1* args,
    void** out_context) PS_POLICY_NOEXCEPT;

/** @brief Selects or abstains from one immutable original snapshot. */
typedef ps_policy_status_v1(PS_POLICY_CALL* ps_policy_select_fn_v1)(
    void* context, const ps_policy_selection_snapshot_v1* snapshot,
    ps_policy_decision_v1* out_decision) PS_POLICY_NOEXCEPT;

/**
 * @brief Destroys one successful logical instance, including null context.
 * @note The Host invokes this once without retry while the DSO remains mapped.
 */
typedef ps_policy_status_v1(PS_POLICY_CALL* ps_policy_destroy_fn_v1)(
    void* context) PS_POLICY_NOEXCEPT;

/**
 * @brief Exact ABI v1 function table returned by the compatible export.
 *
 * `type_count` is in `1..256`; all four callbacks are mandatory. The Host
 * initializes the complete output, including prefix and ABI generation,
 * before the plugin call and rejects any changed prefix or reserved byte.
 */
typedef struct ps_policy_plugin_api_v1 {
  /** @brief Exact value `sizeof(ps_policy_plugin_api_v1)`. */
  uint32_t struct_size;
  /** @brief Exact value `PS_POLICY_STRUCT_PLUGIN_API`. */
  ps_policy_struct_kind_v1 struct_kind;
  /** @brief Exact value `PS_POLICY_PLUGIN_ABI_VERSION`. */
  uint32_t abi_version;
  /** @brief Number of type rows, in `1..256`. */
  uint32_t type_count;
  /** @brief Mandatory metadata callback. */
  ps_policy_get_metadata_fn_v1 get_metadata;
  /** @brief Mandatory context-creation callback. */
  ps_policy_create_fn_v1 create;
  /** @brief Mandatory authority-free selection callback. */
  ps_policy_select_fn_v1 select;
  /** @brief Mandatory exact-once context destruction callback. */
  ps_policy_destroy_fn_v1 destroy;
  /** @brief Must remain entirely zero in ABI v1. */
  uint64_t reserved[4];
} ps_policy_plugin_api_v1;

/**
 * @brief Reports the only ABI generation implemented by this DSO.
 * @return Exact `PS_POLICY_PLUGIN_ABI_VERSION` for a compatible plugin.
 * @throws Nothing. C++ exports are `noexcept`; C exports must not unwind.
 * @note The loader resolves and calls only this symbol before exact equality.
 */
PS_POLICY_PLUGIN_EXPORT uint32_t PS_POLICY_CALL
ps_policy_plugin_get_abi_version(void) PS_POLICY_NOEXCEPT;

/**
 * @brief Returns the complete exact-size API table after ABI equality.
 * @param out_api Nonnull Host-initialized exact v1 output record.
 * @return One valid `PS_POLICY_STATUS_*` value.
 * @throws Nothing. C++ exports are `noexcept`; C exports must not unwind.
 * @note On non-OK the output is ignored. On OK every prefix, callback, count,
 * and reserved field is validated before metadata ranges are dereferenced.
 */
PS_POLICY_PLUGIN_EXPORT ps_policy_status_v1 PS_POLICY_CALL
ps_policy_plugin_get_api_v1(ps_policy_plugin_api_v1* out_api)
    PS_POLICY_NOEXCEPT;

#if defined(__cplusplus)
} /* extern "C" */
#endif

PS_POLICY_STATIC_ASSERT(CHAR_BIT == 8, "policy ABI requires 8-bit bytes");
PS_POLICY_STATIC_ASSERT(sizeof(uint32_t) == 4,
                        "policy ABI requires 32-bit uint32_t");
PS_POLICY_STATIC_ASSERT(sizeof(uint64_t) == 8,
                        "policy ABI requires 64-bit uint64_t");
PS_POLICY_STATIC_ASSERT(sizeof(void*) == 8,
                        "policy ABI requires 64-bit pointers");
PS_POLICY_STATIC_ASSERT(PS_POLICY_ALIGNOF(uint64_t) == 8,
                        "policy ABI requires 8-byte uint64_t alignment");
PS_POLICY_STATIC_ASSERT(PS_POLICY_ALIGNOF(void*) == 8,
                        "policy ABI requires 8-byte pointer alignment");

#define PS_POLICY_ASSERT_LAYOUT(type, expected_size, expected_alignment)   \
  PS_POLICY_STATIC_ASSERT(sizeof(type) == (expected_size),                 \
                          #type " has an unsupported size");               \
  PS_POLICY_STATIC_ASSERT(PS_POLICY_ALIGNOF(type) == (expected_alignment), \
                          #type " has an unsupported alignment")
#define PS_POLICY_ASSERT_OFFSET(type, field, expected_offset)         \
  PS_POLICY_STATIC_ASSERT(offsetof(type, field) == (expected_offset), \
                          #type "." #field " has an unsupported offset")

PS_POLICY_ASSERT_LAYOUT(ps_policy_string_view_v1, 16, 8);
PS_POLICY_ASSERT_OFFSET(ps_policy_string_view_v1, data, 0);
PS_POLICY_ASSERT_OFFSET(ps_policy_string_view_v1, size, 8);

PS_POLICY_ASSERT_LAYOUT(ps_policy_type_metadata_v1, 80, 8);
PS_POLICY_ASSERT_OFFSET(ps_policy_type_metadata_v1, struct_size, 0);
PS_POLICY_ASSERT_OFFSET(ps_policy_type_metadata_v1, struct_kind, 4);
PS_POLICY_ASSERT_OFFSET(ps_policy_type_metadata_v1, name, 8);
PS_POLICY_ASSERT_OFFSET(ps_policy_type_metadata_v1, description, 24);
PS_POLICY_ASSERT_OFFSET(ps_policy_type_metadata_v1, implementation_version, 40);
PS_POLICY_ASSERT_OFFSET(ps_policy_type_metadata_v1, supported_class_mask, 56);
PS_POLICY_ASSERT_OFFSET(ps_policy_type_metadata_v1, reserved0, 60);
PS_POLICY_ASSERT_OFFSET(ps_policy_type_metadata_v1, reserved, 64);

PS_POLICY_ASSERT_LAYOUT(ps_policy_create_args_v1, 40, 8);
PS_POLICY_ASSERT_OFFSET(ps_policy_create_args_v1, struct_size, 0);
PS_POLICY_ASSERT_OFFSET(ps_policy_create_args_v1, struct_kind, 4);
PS_POLICY_ASSERT_OFFSET(ps_policy_create_args_v1, policy_class, 8);
PS_POLICY_ASSERT_OFFSET(ps_policy_create_args_v1, reserved0, 12);
PS_POLICY_ASSERT_OFFSET(ps_policy_create_args_v1, binding_generation, 16);
PS_POLICY_ASSERT_OFFSET(ps_policy_create_args_v1, reserved, 24);

PS_POLICY_ASSERT_LAYOUT(ps_policy_candidate_v1, 120, 8);
PS_POLICY_ASSERT_OFFSET(ps_policy_candidate_v1, struct_size, 0);
PS_POLICY_ASSERT_OFFSET(ps_policy_candidate_v1, struct_kind, 4);
PS_POLICY_ASSERT_OFFSET(ps_policy_candidate_v1, candidate_id, 8);
PS_POLICY_ASSERT_OFFSET(ps_policy_candidate_v1, graph_id, 16);
PS_POLICY_ASSERT_OFFSET(ps_policy_candidate_v1, run_id, 24);
PS_POLICY_ASSERT_OFFSET(ps_policy_candidate_v1, deadline_ns, 32);
PS_POLICY_ASSERT_OFFSET(ps_policy_candidate_v1, weight, 40);
PS_POLICY_ASSERT_OFFSET(ps_policy_candidate_v1, work_units, 48);
PS_POLICY_ASSERT_OFFSET(ps_policy_candidate_v1, ready_bytes, 56);
PS_POLICY_ASSERT_OFFSET(ps_policy_candidate_v1, graph_service_score, 64);
PS_POLICY_ASSERT_OFFSET(ps_policy_candidate_v1, run_service_score, 72);
PS_POLICY_ASSERT_OFFSET(ps_policy_candidate_v1, dispatch_age, 80);
PS_POLICY_ASSERT_OFFSET(ps_policy_candidate_v1, enqueue_sequence, 88);
PS_POLICY_ASSERT_OFFSET(ps_policy_candidate_v1, flags, 96);
PS_POLICY_ASSERT_OFFSET(ps_policy_candidate_v1, reserved0, 100);
PS_POLICY_ASSERT_OFFSET(ps_policy_candidate_v1, reserved, 104);

PS_POLICY_ASSERT_LAYOUT(ps_policy_selection_snapshot_v1, 64, 8);
PS_POLICY_ASSERT_OFFSET(ps_policy_selection_snapshot_v1, struct_size, 0);
PS_POLICY_ASSERT_OFFSET(ps_policy_selection_snapshot_v1, struct_kind, 4);
PS_POLICY_ASSERT_OFFSET(ps_policy_selection_snapshot_v1, policy_class, 8);
PS_POLICY_ASSERT_OFFSET(ps_policy_selection_snapshot_v1, candidate_count, 12);
PS_POLICY_ASSERT_OFFSET(ps_policy_selection_snapshot_v1, binding_generation,
                        16);
PS_POLICY_ASSERT_OFFSET(ps_policy_selection_snapshot_v1, snapshot_generation,
                        24);
PS_POLICY_ASSERT_OFFSET(ps_policy_selection_snapshot_v1, selection_sequence,
                        32);
PS_POLICY_ASSERT_OFFSET(ps_policy_selection_snapshot_v1, candidate_stride, 40);
PS_POLICY_ASSERT_OFFSET(ps_policy_selection_snapshot_v1, reserved0, 44);
PS_POLICY_ASSERT_OFFSET(ps_policy_selection_snapshot_v1, candidates, 48);
PS_POLICY_ASSERT_OFFSET(ps_policy_selection_snapshot_v1, reserved, 56);

PS_POLICY_ASSERT_LAYOUT(ps_policy_decision_v1, 48, 8);
PS_POLICY_ASSERT_OFFSET(ps_policy_decision_v1, struct_size, 0);
PS_POLICY_ASSERT_OFFSET(ps_policy_decision_v1, struct_kind, 4);
PS_POLICY_ASSERT_OFFSET(ps_policy_decision_v1, decision_kind, 8);
PS_POLICY_ASSERT_OFFSET(ps_policy_decision_v1, reserved0, 12);
PS_POLICY_ASSERT_OFFSET(ps_policy_decision_v1, binding_generation, 16);
PS_POLICY_ASSERT_OFFSET(ps_policy_decision_v1, snapshot_generation, 24);
PS_POLICY_ASSERT_OFFSET(ps_policy_decision_v1, candidate_id, 32);
PS_POLICY_ASSERT_OFFSET(ps_policy_decision_v1, reserved, 40);

PS_POLICY_ASSERT_LAYOUT(ps_policy_plugin_api_v1, 80, 8);
PS_POLICY_ASSERT_OFFSET(ps_policy_plugin_api_v1, struct_size, 0);
PS_POLICY_ASSERT_OFFSET(ps_policy_plugin_api_v1, struct_kind, 4);
PS_POLICY_ASSERT_OFFSET(ps_policy_plugin_api_v1, abi_version, 8);
PS_POLICY_ASSERT_OFFSET(ps_policy_plugin_api_v1, type_count, 12);
PS_POLICY_ASSERT_OFFSET(ps_policy_plugin_api_v1, get_metadata, 16);
PS_POLICY_ASSERT_OFFSET(ps_policy_plugin_api_v1, create, 24);
PS_POLICY_ASSERT_OFFSET(ps_policy_plugin_api_v1, select, 32);
PS_POLICY_ASSERT_OFFSET(ps_policy_plugin_api_v1, destroy, 40);
PS_POLICY_ASSERT_OFFSET(ps_policy_plugin_api_v1, reserved, 48);

#undef PS_POLICY_ASSERT_OFFSET
#undef PS_POLICY_ASSERT_LAYOUT
#undef PS_POLICY_ALIGNOF
#undef PS_POLICY_STATIC_ASSERT

#endif  // INCLUDE_PHOTOSPIDER_POLICY_POLICY_PLUGIN_API_H_
