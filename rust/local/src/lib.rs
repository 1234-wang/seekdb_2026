// Copyright (c) 2026 OceanBase.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

//! Process-local persistent metadata. KV owns durability and concurrency;
//! records add typed fields, ordered keys and streaming selection, without SQL.
use std::path::{Path, PathBuf};
use std::sync::Mutex;

mod bridge;
mod records;

static PROCESS: Mutex<Option<(PathBuf, kv::Store)>> = Mutex::new(None);

pub fn open(path: &Path) -> kv::Result<()> {
    let mut current = PROCESS.lock().map_err(|_| kv::Error::Poisoned)?;
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
    PROCESS.lock().map_err(|_| kv::Error::Poisoned)?.take();
    Ok(())
}

pub fn store() -> kv::Result<kv::Store> {
    PROCESS
        .lock()
        .map_err(|_| kv::Error::Poisoned)?
        .as_ref()
        .map(|(_, store)| store.clone())
        .ok_or(kv::Error::InvalidOptions)
}
