// Copyright (c) 2026 OceanBase. SPDX-License-Identifier: Apache-2.0
//! Process-local persistent metadata. KV owns durability and concurrency;
//! records add typed fields, ordered keys and streaming selection, without SQL.
use std::path::{Path, PathBuf};
use std::sync::{Mutex, OnceLock};

mod bridge;
mod records;
pub use records::*;

static PROCESS: OnceLock<Mutex<Option<(PathBuf, kv::Store)>>> = OnceLock::new();

pub fn open(path: &Path) -> kv::Result<()> {
    let mut current = PROCESS
        .get_or_init(|| Mutex::new(None))
        .lock()
        .map_err(|_| kv::Error::Poisoned)?;
    if let Some((directory, store)) = current.as_ref() {
        if directory != path {
            return Err(kv::Error::Busy);
        }
        store.snapshot()?;
        return Ok(());
    }
    *current = Some((
        path.to_owned(),
        kv::Store::open_with_options(
            path,
            kv::Options {
                // Cover a full tablet queue batch with the maximum column checksum
                // payload, including its visible text and both table indexes.
                max_transaction_pages: 16384,
                ..kv::Options::default()
            },
        )?,
    ));
    Ok(())
}

pub fn close() -> kv::Result<()> {
    PROCESS
        .get_or_init(|| Mutex::new(None))
        .lock()
        .map_err(|_| kv::Error::Poisoned)?
        .take();
    Ok(())
}

pub fn store() -> kv::Result<kv::Store> {
    PROCESS
        .get_or_init(|| Mutex::new(None))
        .lock()
        .map_err(|_| kv::Error::Poisoned)?
        .as_ref()
        .map(|(_, store)| store.clone())
        .ok_or(kv::Error::InvalidOptions)
}
