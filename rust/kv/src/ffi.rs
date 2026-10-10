// Copyright (c) 2026 OceanBase.
// SPDX-License-Identifier: Apache-2.0

//! C adapter. Handles own Rust objects; cursor handles retain their snapshot.
//! A transaction handle is borrowed only for the synchronous write callback,
//! on its calling thread. Visitors borrow bytes only until they return.
//! Callbacks must not unwind or close their active handle. A scan visitor must
//! not mutate its source transaction or advance the same cursor recursively.
//! `kv_write` must not recursively write the same store. A get visitor may use
//! its owned value to perform further reads or mutations in the write callback.

use super::*;
use std::ffi::c_void;
use std::ptr;

#[repr(C)]
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum KvCode {
    Ok,
    NotFound,
    End,
    Io,
    Corrupt,
    Busy,
    InvalidArgument,
    KeyTooLarge,
    ValueTooLarge,
    TransactionTooLarge,
    TooManySnapshots,
    Aborted,
    Poisoned,
}

#[repr(C)]
#[derive(Clone, Copy, Debug)]
pub struct KvStatus {
    pub code: KvCode,
    pub os_error: i32,
}

impl KvStatus {
    fn new(code: KvCode) -> Self {
        Self { code, os_error: 0 }
    }
}

impl From<Error> for KvStatus {
    fn from(error: Error) -> Self {
        let mut status = Self::new(match &error {
            Error::Io(_) => KvCode::Io,
            Error::Corrupt(_) => KvCode::Corrupt,
            Error::Busy => KvCode::Busy,
            Error::KeyTooLarge => KvCode::KeyTooLarge,
            Error::ValueTooLarge => KvCode::ValueTooLarge,
            Error::TransactionTooLarge => KvCode::TransactionTooLarge,
            Error::TooManySnapshots => KvCode::TooManySnapshots,
            Error::TransactionAborted => KvCode::Aborted,
            Error::Poisoned => KvCode::Poisoned,
            Error::InvalidOptions => KvCode::InvalidArgument,
        });
        if let Error::Io(error) = error {
            status.os_error = error.raw_os_error().unwrap_or(0);
        }
        status
    }
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct KvBytes {
    pub data: *const u8,
    pub length: usize,
}

impl KvBytes {
    fn view(bytes: &[u8]) -> Self {
        Self {
            data: if bytes.is_empty() {
                ptr::null()
            } else {
                bytes.as_ptr()
            },
            length: bytes.len(),
        }
    }

    unsafe fn slice<'a>(self) -> std::result::Result<&'a [u8], KvStatus> {
        if self.length == 0 {
            return Ok(&[]);
        }
        if self.data.is_null() || self.length > isize::MAX as usize {
            return Err(KvStatus::new(KvCode::InvalidArgument));
        }
        Ok(unsafe { std::slice::from_raw_parts(self.data, self.length) })
    }
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct KvLimits {
    pub max_transaction_pages: usize,
    pub max_snapshots: usize,
}

#[repr(C)]
pub struct KvCommit {
    pub generation: u64,
    pub pages_written: u64,
    pub bytes_written: u64,
}

#[repr(C)]
#[derive(Clone, Copy)]
pub enum KvBoundKind {
    Unbounded,
    Included,
    Excluded,
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct KvRange {
    pub start_kind: KvBoundKind,
    pub start: KvBytes,
    pub end_kind: KvBoundKind,
    pub end: KvBytes,
}

impl KvRange {
    unsafe fn decode(self) -> std::result::Result<Range, KvStatus> {
        unsafe fn bound(
            kind: KvBoundKind,
            bytes: KvBytes,
        ) -> std::result::Result<Bound<Vec<u8>>, KvStatus> {
            if matches!(kind, KvBoundKind::Unbounded) {
                return Ok(Bound::Unbounded);
            }
            if bytes.length > MAX_KEY_SIZE {
                return Err(KvStatus::new(KvCode::KeyTooLarge));
            }
            let key = unsafe { bytes.slice()? }.to_vec();
            Ok(match kind {
                KvBoundKind::Included => Bound::Included(key),
                KvBoundKind::Excluded => Bound::Excluded(key),
                KvBoundKind::Unbounded => unreachable!(),
            })
        }
        Ok(Range {
            start: unsafe { bound(self.start_kind, self.start)? },
            end: unsafe { bound(self.end_kind, self.end)? },
        })
    }
}

pub struct KvStore {
    store: Store,
}
pub struct KvSnapshot {
    snapshot: Arc<Snapshot>,
}
pub struct KvCursor {
    snapshot: Arc<Snapshot>,
    state: CursorState,
}

pub type KvValueVisitor = Option<unsafe extern "C" fn(KvBytes, *mut c_void) -> KvStatus>;
pub type KvRowVisitor = Option<unsafe extern "C" fn(KvBytes, KvBytes, *mut c_void) -> KvStatus>;
pub type KvWriteCallback = Option<unsafe extern "C" fn(*mut c_void, *mut c_void) -> KvStatus>;

macro_rules! attempt {
    ($expression:expr) => {
        match $expression {
            Ok(value) => value,
            Err(error) => return error.into(),
        }
    };
}

/// Open a store directory; path is UTF-8. Null limits select Rust defaults.
/// # Safety
/// Input slices/limits must be readable; output must be writable and empty.
#[no_mangle]
pub unsafe extern "C" fn kv_open(
    path: KvBytes,
    limits: *const KvLimits,
    output: *mut *mut KvStore,
) -> KvStatus {
    if output.is_null() {
        return KvStatus::new(KvCode::InvalidArgument);
    }
    unsafe {
        *output = ptr::null_mut();
    }
    let bytes = attempt!(unsafe { path.slice() });
    let Ok(path) = std::str::from_utf8(bytes) else {
        return KvStatus::new(KvCode::InvalidArgument);
    };
    if path.is_empty() {
        return KvStatus::new(KvCode::InvalidArgument);
    }
    let options = unsafe { limits.as_ref() }.map_or_else(Options::default, |limits| Options {
        max_transaction_pages: limits.max_transaction_pages,
        max_snapshots: limits.max_snapshots,
    });
    let store = attempt!(Store::open_with_options(path, options));
    unsafe {
        *output = Box::into_raw(Box::new(KvStore { store }));
    }
    KvStatus::new(KvCode::Ok)
}

/// # Safety
/// Handle must be null or owned, live, and not used by an active call.
#[no_mangle]
pub unsafe extern "C" fn kv_close(store: *mut KvStore) {
    if !store.is_null() {
        drop(unsafe { Box::from_raw(store) });
    }
}

unsafe fn visit_value(
    value: Result<Option<Vec<u8>>>,
    visitor: KvValueVisitor,
    context: *mut c_void,
) -> KvStatus {
    let Some(visitor) = visitor else {
        return KvStatus::new(KvCode::InvalidArgument);
    };
    match value {
        Ok(Some(value)) => unsafe { visitor(KvBytes::view(&value), context) },
        Ok(None) => KvStatus::new(KvCode::NotFound),
        Err(error) => error.into(),
    }
}

/// # Safety
/// Store/key must remain valid during the call; visitor follows module contract.
#[no_mangle]
pub unsafe extern "C" fn kv_get(
    store: *const KvStore,
    key: KvBytes,
    visitor: KvValueVisitor,
    context: *mut c_void,
) -> KvStatus {
    let Some(store) = (unsafe { store.as_ref() }) else {
        return KvStatus::new(KvCode::InvalidArgument);
    };
    if key.length > MAX_KEY_SIZE {
        return KvStatus::new(KvCode::KeyTooLarge);
    }
    let key = attempt!(unsafe { key.slice() });
    unsafe { visit_value(store.store.get(key), visitor, context) }
}

/// # Safety
/// Store must be live and output must be writable and empty.
#[no_mangle]
pub unsafe extern "C" fn kv_snapshot(
    store: *const KvStore,
    output: *mut *mut KvSnapshot,
) -> KvStatus {
    if output.is_null() {
        return KvStatus::new(KvCode::InvalidArgument);
    }
    unsafe {
        *output = ptr::null_mut();
    }
    let Some(store) = (unsafe { store.as_ref() }) else {
        return KvStatus::new(KvCode::InvalidArgument);
    };
    let snapshot = attempt!(store.store.snapshot());
    unsafe {
        *output = Box::into_raw(Box::new(KvSnapshot {
            snapshot: Arc::new(snapshot),
        }));
    }
    KvStatus::new(KvCode::Ok)
}

/// # Safety
/// Handle must be null or owned and live. Open cursors retain their own reference.
#[no_mangle]
pub unsafe extern "C" fn kv_snapshot_close(snapshot: *mut KvSnapshot) {
    if !snapshot.is_null() {
        drop(unsafe { Box::from_raw(snapshot) });
    }
}

/// # Safety
/// Snapshot/key must remain valid; visitor follows module contract.
#[no_mangle]
pub unsafe extern "C" fn kv_snapshot_get(
    snapshot: *const KvSnapshot,
    key: KvBytes,
    visitor: KvValueVisitor,
    context: *mut c_void,
) -> KvStatus {
    let Some(snapshot) = (unsafe { snapshot.as_ref() }) else {
        return KvStatus::new(KvCode::InvalidArgument);
    };
    if key.length > MAX_KEY_SIZE {
        return KvStatus::new(KvCode::KeyTooLarge);
    }
    let key = attempt!(unsafe { key.slice() });
    unsafe { visit_value(snapshot.snapshot.get(key), visitor, context) }
}

fn open_cursor(snapshot: &KvSnapshot, range: Range, output: &mut *mut KvCursor) -> KvStatus {
    if let Err(error) = snapshot.snapshot.shared.check() {
        return error.into();
    }
    let root = snapshot.snapshot.root;
    let state = attempt!(CursorState::new(
        &snapshot.snapshot.shared,
        root.page,
        root.pages,
        range
    ));
    *output = Box::into_raw(Box::new(KvCursor {
        snapshot: Arc::clone(&snapshot.snapshot),
        state,
    }));
    KvStatus::new(KvCode::Ok)
}

/// # Safety
/// Snapshot/range must remain valid; output must be writable and empty.
#[no_mangle]
pub unsafe extern "C" fn kv_scan(
    snapshot: *const KvSnapshot,
    range: KvRange,
    output: *mut *mut KvCursor,
) -> KvStatus {
    if output.is_null() {
        return KvStatus::new(KvCode::InvalidArgument);
    }
    unsafe {
        *output = ptr::null_mut();
    }
    let Some(snapshot) = (unsafe { snapshot.as_ref() }) else {
        return KvStatus::new(KvCode::InvalidArgument);
    };
    let range = attempt!(unsafe { range.decode() });
    open_cursor(snapshot, range, unsafe { &mut *output })
}

/// # Safety
/// Snapshot/prefix must remain valid; output must be writable and empty.
#[no_mangle]
pub unsafe extern "C" fn kv_scan_prefix(
    snapshot: *const KvSnapshot,
    prefix: KvBytes,
    output: *mut *mut KvCursor,
) -> KvStatus {
    if output.is_null() {
        return KvStatus::new(KvCode::InvalidArgument);
    }
    unsafe {
        *output = ptr::null_mut();
    }
    let Some(snapshot) = (unsafe { snapshot.as_ref() }) else {
        return KvStatus::new(KvCode::InvalidArgument);
    };
    if prefix.length > MAX_KEY_SIZE {
        return KvStatus::new(KvCode::KeyTooLarge);
    }
    let prefix = attempt!(unsafe { prefix.slice() });
    open_cursor(snapshot, Range::prefix(prefix), unsafe { &mut *output })
}

/// Advances one row, even if its visitor fails. End is a distinct status.
/// # Safety
/// Cursor must be live and exclusively used; visitor follows module contract.
#[no_mangle]
pub unsafe extern "C" fn kv_cursor_next(
    cursor: *mut KvCursor,
    visitor: KvRowVisitor,
    context: *mut c_void,
) -> KvStatus {
    let Some(cursor) = (unsafe { cursor.as_mut() }) else {
        return KvStatus::new(KvCode::InvalidArgument);
    };
    let Some(visitor) = visitor else {
        return KvStatus::new(KvCode::InvalidArgument);
    };
    match cursor.state.next(&cursor.snapshot.shared) {
        Some(Ok((key, value))) => unsafe {
            visitor(KvBytes::view(&key), KvBytes::view(&value), context)
        },
        Some(Err(error)) => error.into(),
        None => KvStatus::new(KvCode::End),
    }
}

/// # Safety
/// Cursor must be null or owned, live, and not used by an active call.
#[no_mangle]
pub unsafe extern "C" fn kv_cursor_close(cursor: *mut KvCursor) {
    if !cursor.is_null() {
        drop(unsafe { Box::from_raw(cursor) });
    }
}

/// Callback Ok commits; any other callback status rolls the transaction back.
/// # Safety
/// Store must remain live. Callback follows module contract; transaction pointer
/// is usable only on this thread within this callback. Commit output may be null.
#[no_mangle]
pub unsafe extern "C" fn kv_write(
    store: *const KvStore,
    callback: KvWriteCallback,
    context: *mut c_void,
    output: *mut KvCommit,
) -> KvStatus {
    let Some(store) = (unsafe { store.as_ref() }) else {
        return KvStatus::new(KvCode::InvalidArgument);
    };
    let Some(callback) = callback else {
        return KvStatus::new(KvCode::InvalidArgument);
    };
    // The scoped owner keeps the writer and files alive for the whole callback.
    let scoped_store = store.store.clone();
    let mut transaction = attempt!(scoped_store.begin());
    let borrowed = ptr::from_mut(&mut transaction).cast::<c_void>();
    let status = unsafe { callback(borrowed, context) };
    if status.code != KvCode::Ok {
        return status;
    }
    let info = attempt!(transaction.commit());
    if let Some(output) = unsafe { output.as_mut() } {
        *output = KvCommit {
            generation: info.generation,
            pages_written: info.pages_written,
            bytes_written: info.bytes_written,
        };
    }
    KvStatus::new(KvCode::Ok)
}

/// # Safety
/// Transaction is borrowed from the active write callback on this thread;
/// key is readable, and the value visitor follows module contract.
#[no_mangle]
pub unsafe extern "C" fn kv_tx_get(
    transaction: *mut c_void,
    key: KvBytes,
    visitor: KvValueVisitor,
    context: *mut c_void,
) -> KvStatus {
    let Some(transaction) = (unsafe { transaction.cast::<Transaction<'_>>().as_ref() }) else {
        return KvStatus::new(KvCode::InvalidArgument);
    };
    if key.length > MAX_KEY_SIZE {
        return KvStatus::new(KvCode::KeyTooLarge);
    }
    let key = attempt!(unsafe { key.slice() });
    unsafe { visit_value(transaction.get(key), visitor, context) }
}

unsafe fn mutation_bytes<'a>(
    transaction: &mut Transaction<'_>,
    bytes: KvBytes,
    limit: usize,
    code: KvCode,
) -> std::result::Result<&'a [u8], KvStatus> {
    let result = if bytes.length > limit {
        Err(KvStatus::new(code))
    } else {
        unsafe { bytes.slice() }
    };
    if result.is_err() {
        transaction.failed = true;
    }
    result
}

/// # Safety
/// Transaction is borrowed from the active write callback on this thread;
/// key/value slices must be readable for the call.
#[no_mangle]
pub unsafe extern "C" fn kv_tx_put(
    transaction: *mut c_void,
    key: KvBytes,
    value: KvBytes,
) -> KvStatus {
    let Some(transaction) = (unsafe { transaction.cast::<Transaction<'_>>().as_mut() }) else {
        return KvStatus::new(KvCode::InvalidArgument);
    };
    let key =
        attempt!(unsafe { mutation_bytes(transaction, key, MAX_KEY_SIZE, KvCode::KeyTooLarge) });
    let value = attempt!(unsafe {
        mutation_bytes(transaction, value, MAX_VALUE_SIZE, KvCode::ValueTooLarge)
    });
    attempt!(transaction.put(key, value));
    KvStatus::new(KvCode::Ok)
}

/// # Safety
/// Transaction is borrowed from the active write callback on this thread;
/// key must be readable and optional deleted output writable.
#[no_mangle]
pub unsafe extern "C" fn kv_tx_delete(
    transaction: *mut c_void,
    key: KvBytes,
    deleted: *mut bool,
) -> KvStatus {
    let Some(transaction) = (unsafe { transaction.cast::<Transaction<'_>>().as_mut() }) else {
        return KvStatus::new(KvCode::InvalidArgument);
    };
    let key =
        attempt!(unsafe { mutation_bytes(transaction, key, MAX_KEY_SIZE, KvCode::KeyTooLarge) });
    let removed = attempt!(transaction.delete(key));
    if let Some(output) = unsafe { deleted.as_mut() } {
        *output = removed;
    }
    KvStatus::new(KvCode::Ok)
}

/// A row visitor can return End to stop the scan successfully.
/// # Safety
/// Transaction is borrowed from the active write callback on this thread;
/// range must be readable. Visitor cannot mutate the same transaction.
#[no_mangle]
pub unsafe extern "C" fn kv_tx_scan(
    transaction: *mut c_void,
    range: KvRange,
    visitor: KvRowVisitor,
    context: *mut c_void,
) -> KvStatus {
    let Some(transaction) = (unsafe { transaction.cast::<Transaction<'_>>().as_ref() }) else {
        return KvStatus::new(KvCode::InvalidArgument);
    };
    let Some(visitor) = visitor else {
        return KvStatus::new(KvCode::InvalidArgument);
    };
    let range = attempt!(unsafe { range.decode() });
    let cursor = attempt!(transaction.scan(range));
    for record in cursor {
        let (key, value) = attempt!(record);
        let status = unsafe { visitor(KvBytes::view(&key), KvBytes::view(&value), context) };
        match status.code {
            KvCode::Ok => (),
            KvCode::End => break,
            _ => return status,
        }
    }
    KvStatus::new(KvCode::Ok)
}
