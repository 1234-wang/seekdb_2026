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

use super::*;
#[derive(Clone, Copy, PartialEq, Eq)]
enum LocalTable {
    MergeInfo,
    ChecksumError,
    Deadlock,
    TabletMeta,
    TabletChecksum,
    Job,
}
impl LocalTable {
    fn schema(self) -> Arc<Schema> {
        #[allow(non_upper_case_globals)]
        const Text: LocalKind = LocalKind::Text;
        #[allow(non_upper_case_globals)]
        const Blob: LocalKind = LocalKind::Blob;
        let tables: [Schema; 6] = [
            Schema {
                namespace_id: 0,
                kinds: vec![I, I, I, I, I, I, I, I, I, I],
                nullable: vec![],
                primary: vec![0],
                indexes: vec![],
            },
            Schema {
                namespace_id: 0,
                kinds: vec![I, I, I, I, I, I, I, I, I],
                nullable: vec![],
                primary: vec![0, 1, 2, 3, 4, 5],
                indexes: vec![],
            },
            Schema {
                namespace_id: 0,
                kinds: vec![
                    I, I, I, I, I, Text, Text, I, I, I, Text, Text, Text, Text, Text, Text, Text,
                    Text, Text,
                ],
                nullable: vec![5, 6, 10, 11, 12, 13, 14, 15, 16, 17, 18],
                primary: vec![0, 1],
                indexes: vec![2],
            },
            Schema {
                namespace_id: 0,
                kinds: vec![I, I, I, I, I, I, I, I],
                nullable: vec![0, 1],
                primary: vec![2],
                indexes: vec![3],
            },
            Schema {
                namespace_id: 0,
                kinds: vec![I, I, I, I, Text, Blob, I],
                nullable: vec![4, 5],
                primary: vec![0],
                indexes: vec![],
            },
            Schema {
                namespace_id: 0,
                kinds: vec![I, I, I, Text, Text, I],
                nullable: vec![1, 2, 5],
                primary: vec![0],
                indexes: vec![],
            },
        ];

        let mut schema = tables.into_iter().nth(self as usize).unwrap();
        schema.namespace_id = self as u8;
        schema.into()
    }
}

use std::path::PathBuf;
use std::sync::atomic::{AtomicU64, Ordering};
static NEXT: AtomicU64 = AtomicU64::new(0);
struct Directory(PathBuf);
impl Directory {
    fn new() -> Self {
        let path = std::env::temp_dir().join(format!(
            "local-{}-{}",
            std::process::id(),
            NEXT.fetch_add(1, Ordering::Relaxed)
        ));
        std::fs::create_dir(&path).unwrap();
        Self(path)
    }
}
impl Drop for Directory {
    fn drop(&mut self) {
        std::fs::remove_dir_all(&self.0).unwrap();
    }
}
fn record(table: LocalTable, id: i64) -> Vec<Cell> {
    let mut row: Vec<_> = table
        .schema()
        .kinds
        .iter()
        .map(|&kind| match kind {
            LocalKind::Integer => Cell::Integer(0),
            LocalKind::Text => Cell::Text(vec![]),
            LocalKind::Blob => Cell::Blob(vec![]),
            _ => unreachable!(),
        })
        .collect();
    row[table.schema().primary[0]] = Cell::Integer(id);
    row
}
fn insert(table: LocalTable, row: Vec<Cell>, upsert: bool) -> Mutation {
    Mutation {
        action: if upsert {
            LocalAction::Upsert
        } else {
            LocalAction::Insert
        },
        query: Query::all(table.schema()),
        assignments: row
            .into_iter()
            .enumerate()
            .map(|(column, value)| Assignment {
                column,
                value,
                clamp_column: None,
            })
            .collect(),
    }
}
fn all(store: &kv::Store, query: Query) -> Vec<Vec<Cell>> {
    let mut cursor = RowCursor::new(store, query).unwrap();
    let mut rows = vec![];
    while let Some(row) = cursor.next_row().unwrap() {
        rows.push(row);
    }
    rows
}
fn predicate(column: usize, compare: LocalCompare, value: i64) -> Predicate {
    Predicate {
        column,
        compare,
        values: vec![Cell::Integer(value)],
    }
}
#[test]
fn every_table_persists_with_binary_fields_and_preserves_other_namespaces() {
    let directory = Directory::new();
    let store = kv::Store::open(&directory.0).unwrap();
    let mut tx = store.begin().unwrap();
    tx.put(b"config/cpu_count", b"4").unwrap();
    for table in [
        LocalTable::MergeInfo,
        LocalTable::ChecksumError,
        LocalTable::Deadlock,
        LocalTable::TabletMeta,
        LocalTable::TabletChecksum,
        LocalTable::Job,
    ] {
        let mut row = record(table, 12);
        if table == LocalTable::TabletChecksum {
            row[5] = Cell::Blob(vec![0, 1, 255, 0, 2]);
        }
        if table == LocalTable::Job {
            row[3] = Cell::Text(b"job'\0type".to_vec());
        }
        insert(table, row, false).execute(&mut tx).unwrap();
    }
    tx.commit().unwrap();
    drop(store);
    let store = kv::Store::open(&directory.0).unwrap();
    for table in [
        LocalTable::MergeInfo,
        LocalTable::ChecksumError,
        LocalTable::Deadlock,
        LocalTable::TabletMeta,
        LocalTable::TabletChecksum,
        LocalTable::Job,
    ] {
        let rows = all(&store, Query::all(table.schema()));
        assert_eq!(rows.len(), 1);
        if table == LocalTable::TabletChecksum {
            assert_eq!(rows[0][5], Cell::Blob(vec![0, 1, 255, 0, 2]));
        }
    }
    assert_eq!(store.get(b"config/cpu_count").unwrap().unwrap(), b"4");
}
#[test]
fn cross_table_transaction_rolls_back_on_duplicate_and_preserves_primary_identity() {
    let directory = Directory::new();
    let store = kv::Store::open(&directory.0).unwrap();
    let mut tx = store.begin().unwrap();
    insert(
        LocalTable::ChecksumError,
        record(LocalTable::ChecksumError, 9),
        false,
    )
    .execute(&mut tx)
    .unwrap();
    tx.commit().unwrap();
    let mut tx = store.begin().unwrap();
    insert(
        LocalTable::TabletMeta,
        record(LocalTable::TabletMeta, 22),
        true,
    )
    .execute(&mut tx)
    .unwrap();
    insert(
        LocalTable::TabletChecksum,
        record(LocalTable::TabletChecksum, 22),
        true,
    )
    .execute(&mut tx)
    .unwrap();
    let mut duplicate = record(LocalTable::ChecksumError, 9);
    duplicate[6] = Cell::Integer(77);
    assert!(matches!(
        insert(LocalTable::ChecksumError, duplicate, false).execute(&mut tx),
        Err(Error::Duplicate)
    ));
    tx.rollback();
    assert!(all(&store, Query::all(LocalTable::TabletMeta.schema())).is_empty());
    assert!(all(&store, Query::all(LocalTable::TabletChecksum.schema())).is_empty());
    let errors = all(&store, Query::all(LocalTable::ChecksumError.schema()));
    assert_eq!(errors[0][6], Cell::Integer(0));
}
#[test]
fn cleanup_uses_time_order_and_upserts_remove_old_index_keys() {
    let directory = Directory::new();
    let store = kv::Store::open(&directory.0).unwrap();
    let mut tx = store.begin().unwrap();
    for (id, time) in [(1, 300), (2, 100), (3, 200)] {
        let mut row = record(LocalTable::Deadlock, id);
        row[2] = Cell::Integer(time);
        insert(LocalTable::Deadlock, row, false)
            .execute(&mut tx)
            .unwrap();
    }
    tx.commit().unwrap();
    let mut query = Query::all(LocalTable::Deadlock.schema());
    query.order = Some(2);
    query.limit = Some(1);
    query.predicates.push(predicate(2, LocalCompare::Less, 250));
    let mut tx = store.begin().unwrap();
    assert_eq!(
        Mutation {
            action: LocalAction::Delete,
            query,
            assignments: vec![]
        }
        .execute(&mut tx)
        .unwrap(),
        1
    );
    let mut updated = record(LocalTable::Deadlock, 3);
    updated[2] = Cell::Integer(400);
    insert(LocalTable::Deadlock, updated, true)
        .execute(&mut tx)
        .unwrap();
    tx.commit().unwrap();
    let mut query = Query::all(LocalTable::Deadlock.schema());
    query.order = Some(2);
    let rows = all(&store, query);
    assert_eq!(rows.len(), 2);
    assert_eq!(rows[0][0], Cell::Integer(1));
    assert_eq!(rows[1][0], Cell::Integer(3));
}
#[test]
fn long_cursor_keeps_its_view_while_other_tables_are_modified() {
    let directory = Directory::new();
    let store = kv::Store::open(&directory.0).unwrap();
    let mut tx = store.begin().unwrap();
    for id in 0..5 {
        insert(
            LocalTable::TabletMeta,
            record(LocalTable::TabletMeta, id),
            true,
        )
        .execute(&mut tx)
        .unwrap();
    }
    tx.commit().unwrap();
    let mut cursor = RowCursor::new(&store, Query::all(LocalTable::TabletMeta.schema())).unwrap();
    assert_eq!(cursor.next_row().unwrap().unwrap()[2], Cell::Integer(0));
    let mut tx = store.begin().unwrap();
    Mutation {
        action: LocalAction::Delete,
        query: Query::all(LocalTable::TabletMeta.schema()),
        assignments: vec![],
    }
    .execute(&mut tx)
    .unwrap();
    insert(
        LocalTable::TabletMeta,
        record(LocalTable::TabletMeta, 99),
        true,
    )
    .execute(&mut tx)
    .unwrap();
    tx.commit().unwrap();
    for id in 1..5 {
        assert_eq!(cursor.next_row().unwrap().unwrap()[2], Cell::Integer(id));
    }
    assert!(cursor.next_row().unwrap().is_none());
    assert_eq!(
        all(&store, Query::all(LocalTable::TabletMeta.schema())).len(),
        1
    );
}
#[test]
fn reporting_is_conditional_counts_affected_rows_and_clamps_to_compaction() {
    let directory = Directory::new();
    let store = kv::Store::open(&directory.0).unwrap();
    let mut tx = store.begin().unwrap();
    for (id, scn, status) in [(1, 50, 0), (2, 80, 1), (3, 120, 0)] {
        let mut row = record(LocalTable::TabletMeta, id);
        row[3] = Cell::Integer(scn);
        row[7] = Cell::Integer(status);
        insert(LocalTable::TabletMeta, row, true)
            .execute(&mut tx)
            .unwrap();
    }
    tx.commit().unwrap();
    let mut query = Query::all(LocalTable::TabletMeta.schema());
    query.predicates = vec![
        predicate(3, LocalCompare::GreaterEqual, 60),
        predicate(6, LocalCompare::Less, 100),
        predicate(7, LocalCompare::NotEqual, 1),
    ];
    let mut tx = store.begin().unwrap();
    assert_eq!(
        Mutation {
            action: LocalAction::Update,
            query,
            assignments: vec![Assignment {
                column: 6,
                value: Cell::Integer(100),
                clamp_column: Some(3)
            }]
        }
        .execute(&mut tx)
        .unwrap(),
        1
    );
    tx.commit().unwrap();
    let rows = all(&store, Query::all(LocalTable::TabletMeta.schema()));
    assert_eq!(
        rows.iter().map(|r| r[6].clone()).collect::<Vec<_>>(),
        vec![Cell::Integer(0), Cell::Integer(0), Cell::Integer(100)]
    );
    let mut query = Query::all(LocalTable::TabletMeta.schema());
    query.order = Some(3);
    query.columns = vec![3];
    query.limit = Some(1);
    assert_eq!(all(&store, query), vec![vec![Cell::Integer(50)]]);
}
#[test]
fn aggregates_and_paginated_queries_preserve_empty_and_null_semantics() {
    let directory = Directory::new();
    let store = kv::Store::open(&directory.0).unwrap();
    let mut query = Query::all(LocalTable::Job.schema());
    query.columns = vec![0];
    query.aggregate = LocalAggregate::Max;
    assert_eq!(all(&store, query.clone()), vec![vec![Cell::Null]]);
    let mut tx = store.begin().unwrap();
    for id in [9, -3, 0, 2] {
        insert(LocalTable::Job, record(LocalTable::Job, id), false)
            .execute(&mut tx)
            .unwrap();
    }
    tx.commit().unwrap();
    assert_eq!(all(&store, query), vec![vec![Cell::Integer(9)]]);
    let mut query = Query::all(LocalTable::Job.schema());
    query.columns = vec![0];
    query.order = Some(0);
    query.offset = 1;
    query.limit = Some(2);
    assert_eq!(
        all(&store, query),
        vec![vec![Cell::Integer(0)], vec![Cell::Integer(2)]]
    );
    let mut query = Query::all(LocalTable::Job.schema());
    query.aggregate = LocalAggregate::Count;
    query.predicates = vec![predicate(0, LocalCompare::Greater, 100)];
    assert_eq!(all(&store, query), vec![vec![Cell::Integer(0)]]);
}
#[test]
fn primary_ranges_and_in_lists_handle_signed_boundaries_without_duplicates() {
    let directory = Directory::new();
    let store = kv::Store::open(&directory.0).unwrap();
    let mut tx = store.begin().unwrap();
    for id in [i64::MIN, -3, 0, 2, 9, i64::MAX] {
        insert(LocalTable::Job, record(LocalTable::Job, id), false)
            .execute(&mut tx)
            .unwrap();
    }
    tx.commit().unwrap();
    let mut query = Query::all(LocalTable::Job.schema());
    query.columns = vec![0];
    query.predicates = vec![
        predicate(0, LocalCompare::Greater, -3),
        predicate(0, LocalCompare::LessEqual, 9),
    ];
    assert_eq!(
        all(&store, query),
        vec![
            vec![Cell::Integer(0)],
            vec![Cell::Integer(2)],
            vec![Cell::Integer(9)]
        ]
    );
    let mut query = Query::all(LocalTable::Job.schema());
    query.columns = vec![0];
    query.predicates = vec![predicate(0, LocalCompare::Greater, i64::MAX)];
    assert!(all(&store, query).is_empty());
    let mut query = Query::all(LocalTable::Job.schema());
    query.columns = vec![0];
    query.predicates = vec![Predicate {
        column: 0,
        compare: LocalCompare::In,
        values: vec![
            Cell::Integer(9),
            Cell::Integer(-3),
            Cell::Integer(9),
            Cell::Integer(5),
        ],
    }];
    assert_eq!(
        all(&store, query.clone()),
        vec![vec![Cell::Integer(-3)], vec![Cell::Integer(9)]]
    );
    let mut tx = store.begin().unwrap();
    assert_eq!(
        Mutation {
            action: LocalAction::Delete,
            query,
            assignments: vec![]
        }
        .execute(&mut tx)
        .unwrap(),
        2
    );
    tx.commit().unwrap();
    assert_eq!(all(&store, Query::all(LocalTable::Job.schema())).len(), 4);
}
#[test]
fn transaction_reads_its_writes_and_can_compute_a_following_update() {
    let directory = Directory::new();
    let store = kv::Store::open(&directory.0).unwrap();
    let mut tx = store.begin().unwrap();
    insert(LocalTable::Job, record(LocalTable::Job, 22), false)
        .execute(&mut tx)
        .unwrap();
    let mut query = Query::all(LocalTable::Job.schema());
    query.columns = vec![5];
    query.predicates = vec![predicate(0, LocalCompare::Equal, 22)];
    let mut old = None;
    query
        .visit_transaction(&tx, |row| {
            old = Some(row[0].clone());
            true
        })
        .unwrap();
    assert_eq!(old, Some(Cell::Integer(0)));
    Mutation {
        action: LocalAction::Update,
        query: query.clone(),
        assignments: vec![Assignment {
            column: 5,
            value: Cell::Integer(73),
            clamp_column: None,
        }],
    }
    .execute(&mut tx)
    .unwrap();
    query
        .visit_transaction(&tx, |row| {
            assert_eq!(row[0], Cell::Integer(73));
            true
        })
        .unwrap();
    tx.rollback();
    assert!(all(&store, Query::all(LocalTable::Job.schema())).is_empty());
}
#[test]
fn maximum_column_checksum_batch_fits_the_process_transaction_budget() {
    let directory = Directory::new();
    let store = kv::Store::open_with_options(
        &directory.0,
        kv::Options {
            max_transaction_pages: 16384,
            ..kv::Options::default()
        },
    )
    .unwrap();
    let mut tx = store.begin().unwrap();
    for id in 1..=128 {
        insert(
            LocalTable::TabletMeta,
            record(LocalTable::TabletMeta, id),
            true,
        )
        .execute(&mut tx)
        .unwrap();
        let mut row = record(LocalTable::TabletChecksum, id);
        // MAX_OCCUPIED_BYTES = 4000 * 8 + 11; visible signed checksums can
        // occupy 21 characters each, including their comma separator.
        row[4] = Cell::Text(vec![b'9'; 4000 * 21]);
        row[5] = Cell::Blob(vec![37; 4000 * 8 + 11]);
        insert(LocalTable::TabletChecksum, row, true)
            .execute(&mut tx)
            .unwrap();
    }
    let commit = tx.commit().unwrap();
    assert!(commit.pages_written < 16384);
    assert_eq!(
        all(&store, Query::all(LocalTable::TabletMeta.schema())).len(),
        128
    );
    assert_eq!(
        all(&store, Query::all(LocalTable::TabletChecksum.schema())).len(),
        128
    );
}
