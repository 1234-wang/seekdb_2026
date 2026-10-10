/* Copyright (c) 2026 OceanBase. SPDX-License-Identifier: Apache-2.0 */
/* Generated from the kv crate. DO NOT EDIT. */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum KvCode {
  KvCode_Ok,
  KvCode_NotFound,
  KvCode_End,
  KvCode_Io,
  KvCode_Corrupt,
  KvCode_Busy,
  KvCode_InvalidArgument,
  KvCode_KeyTooLarge,
  KvCode_ValueTooLarge,
  KvCode_TransactionTooLarge,
  KvCode_TooManySnapshots,
  KvCode_Aborted,
  KvCode_Poisoned,
} KvCode;

typedef enum KvBoundKind {
  KvBoundKind_Unbounded,
  KvBoundKind_Included,
  KvBoundKind_Excluded,
} KvBoundKind;

typedef struct KvCursor KvCursor;

typedef struct KvSnapshot KvSnapshot;

typedef struct KvStore KvStore;

typedef struct KvStatus {
  enum KvCode code;
  int32_t os_error;
} KvStatus;

typedef struct KvBytes {
  const uint8_t *data;
  size_t length;
} KvBytes;

typedef struct KvLimits {
  size_t max_transaction_pages;
  size_t max_snapshots;
} KvLimits;

typedef struct KvStatus (*KvValueVisitor)(struct KvBytes, void*);

typedef struct KvRange {
  enum KvBoundKind start_kind;
  struct KvBytes start;
  enum KvBoundKind end_kind;
  struct KvBytes end;
} KvRange;

typedef struct KvStatus (*KvRowVisitor)(struct KvBytes, struct KvBytes, void*);

typedef struct KvStatus (*KvWriteCallback)(void*, void*);

typedef struct KvCommit {
  uint64_t generation;
  uint64_t pages_written;
  uint64_t bytes_written;
} KvCommit;

#ifdef __cplusplus
extern "C" {
#endif // __cplusplus

/**
 * Open a store directory; path is UTF-8. Null limits select Rust defaults.
 * # Safety
 * Input slices/limits must be readable; output must be writable and empty.
 */
struct KvStatus kv_open(struct KvBytes path,
                        const struct KvLimits *limits,
                        struct KvStore **output);

/**
 * # Safety
 * Handle must be null or owned, live, and not used by an active call.
 */
void kv_close(struct KvStore *store);

/**
 * # Safety
 * Store/key must remain valid during the call; visitor follows module contract.
 */
struct KvStatus kv_get(const struct KvStore *store,
                       struct KvBytes key,
                       KvValueVisitor visitor,
                       void *context);

/**
 * # Safety
 * Store must be live and output must be writable and empty.
 */
struct KvStatus kv_snapshot(const struct KvStore *store, struct KvSnapshot **output);

/**
 * # Safety
 * Handle must be null or owned and live. Open cursors retain their own reference.
 */
void kv_snapshot_close(struct KvSnapshot *snapshot);

/**
 * # Safety
 * Snapshot/key must remain valid; visitor follows module contract.
 */
struct KvStatus kv_snapshot_get(const struct KvSnapshot *snapshot,
                                struct KvBytes key,
                                KvValueVisitor visitor,
                                void *context);

/**
 * # Safety
 * Snapshot/range must remain valid; output must be writable and empty.
 */
struct KvStatus kv_scan(const struct KvSnapshot *snapshot,
                        struct KvRange range,
                        struct KvCursor **output);

/**
 * # Safety
 * Snapshot/prefix must remain valid; output must be writable and empty.
 */
struct KvStatus kv_scan_prefix(const struct KvSnapshot *snapshot,
                               struct KvBytes prefix,
                               struct KvCursor **output);

/**
 * Advances one row, even if its visitor fails. End is a distinct status.
 * # Safety
 * Cursor must be live and exclusively used; visitor follows module contract.
 */
struct KvStatus kv_cursor_next(struct KvCursor *cursor, KvRowVisitor visitor, void *context);

/**
 * # Safety
 * Cursor must be null or owned, live, and not used by an active call.
 */
void kv_cursor_close(struct KvCursor *cursor);

/**
 * Callback Ok commits; any other callback status rolls the transaction back.
 * # Safety
 * Store must remain live. Callback follows module contract; transaction pointer
 * is usable only on this thread within this callback. Commit output may be null.
 */
struct KvStatus kv_write(const struct KvStore *store,
                         KvWriteCallback callback,
                         void *context,
                         struct KvCommit *output);

/**
 * # Safety
 * Transaction is borrowed from the active write callback on this thread;
 * key is readable, and the value visitor follows module contract.
 */
struct KvStatus kv_tx_get(void *transaction,
                          struct KvBytes key,
                          KvValueVisitor visitor,
                          void *context);

/**
 * # Safety
 * Transaction is borrowed from the active write callback on this thread;
 * key/value slices must be readable for the call.
 */
struct KvStatus kv_tx_put(void *transaction, struct KvBytes key, struct KvBytes value);

/**
 * # Safety
 * Transaction is borrowed from the active write callback on this thread;
 * key must be readable and optional deleted output writable.
 */
struct KvStatus kv_tx_delete(void *transaction, struct KvBytes key, bool *deleted);

/**
 * A row visitor can return End to stop the scan successfully.
 * # Safety
 * Transaction is borrowed from the active write callback on this thread;
 * range must be readable. Visitor cannot mutate the same transaction.
 */
struct KvStatus kv_tx_scan(void *transaction,
                           struct KvRange range,
                           KvRowVisitor visitor,
                           void *context);

#ifdef __cplusplus
}  // extern "C"
#endif  // __cplusplus
