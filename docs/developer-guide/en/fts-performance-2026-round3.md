# FTS third optimization: synchronization in token aggregation

This is a local candidate on top of the second delivery `d719f55ec7b7507d3e726a0e665422f41566d30d`. Release compilation and five native correctness tests pass. Full SQL regressions and the official nine-metric benchmark have not run for this candidate: the current execution sandbox rejects `socket.socket()` with `EPERM` before a dedicated database instance can start. This candidate has not been published or officially scored. The latest previously verified submission remains the second delivery.

## Ownership and implementation

The word-frequency maps are local variables in TOKENIZE, index-row generation, optimizer query tokenization, and retrieval query tokenization. A worker does not share its map or its node allocator with another worker. `ObFTWordMap` now uses the existing `NoPthreadDefendMode` for both buckets and its node allocator, removing a bucket latch and allocator spinlocks from the per-token path. The three direct uses of the former template type also use this alias, including the retained retrieval implementation outside the active composition source inventory.

The built-in stopword set is populated during `ObFTParseData::init_global()` before parser users start, read without mutation during service, and destroyed after parser users stop. It now uses `NoPthreadDefendMode`, so concurrent readers no longer acquire per-bucket pthread read locks. Hash computation and charset conversion remain unchanged, including the dictionary's case/accent-insensitive collation for binary input. This depends on the existing startup/publication and shutdown ordering; runtime stopword mutation would require revisiting the synchronization policy.

The bucket count, hash/equality functions, node-block capacity, iteration order, last spelling of equivalent words, token frequencies and document lengths are preserved. The patch does not change the index format or scoring rules.

## Native correctness

`unittest/storage/fts/test_fts_word_map.cpp` contains five tests. The exact same test source passed against the unchanged second-round libraries first and against the candidate libraries after the complete Release rebuild:

- Forced collisions in two buckets, more than 320 distinct words across allocator blocks, binary and case/accent-insensitive collations, embedded NUL, Unicode, trailing spaces, last spelling, iteration order against the former synchronized map, clear/reuse and destroy/recreate.
- Grouped frequencies and document length with case/accent-equivalent tokens.
- Stopword collation, binary-to-UTF-8 conversion, invalid/empty input, repeated initialization and destruction.
- Eight concurrent readers perform 40,000 stopword checks after publication and join before destruction.
- Actual BENG parsing reuses one map across 100 alternating document pairs, checking exact counts, frequencies and empty stopword-only output.

The initial baseline test attempted Latin-1 conversion, which this competition branch rejects because it supports only binary and three UTF-8 collations. Before the production changes, the new test was corrected to assert that existing error and exercise supported binary conversion. The original failed result is retained. No upstream tests or assertions were changed.

The first candidate build caught direct declarations using the former map template in the optimizer and active retrieval composition. All three such declarations, including the retained retrieval implementation, were migrated; the subsequent full Release build returned zero. No product code was changed after that successful build and candidate test run.

## Diagnostic native timings

After compilation finished, the saved old and new native test executables ran in the fixed sequence old/new/new/old/old/new, pinned to CPUs 4,5,6,7. Every trial passed and is retained. The table uses arithmetic means of the three observations for each binary; values are whole-process seconds, including startup, assertions and thread creation.

| Workload | Old mean seconds | Candidate mean seconds | Time reduction |
| --- | ---: | ---: | ---: |
| `stopword_reads` | 1.018060 | 0.650482 | 36.11% |
| `beng_documents` | 1.051387 | 0.750540 | 28.61% |

`stopword_reads` repeats the eight-reader test 100 times (4,000,000 checks per process). `beng_documents` repeats the actual parser test 1,000 times (200,000 documents per process). There is no exclusive CPU reservation or fixed CPU frequency. An earlier pilot overlapping compilation is preserved separately and was not included in this post-build comparison.

These are targeted native measurements, not SQL TOKENIZE latency, index-build timings, MATCH timings, or a competition-score forecast. They do not establish an overall FTS improvement. The [JSON artifact](fts-performance-2026-round3.json) retains all samples, commands, binary/test/patch hashes, build metadata and the runtime limitation. The previous [second-round full benchmark](fts-performance-2026-round2.md) remains separate.

## Reproduction and remaining validation

With the existing repository Release configuration and fixed toolchain, run from the repository root:

```bash
./build.sh release --make -j24
python3 unittest/storage/fts/run_word_map_test.py --label candidate
```

The runner reuses the actual CMake compile and link commands and bundled gtest. It writes commands, logs, XML results and binary identity under `build_release/fts-word-map-tests/`; `--output-dir` selects another location. It neither starts a server nor changes the production build graph. The competition checkout has no native unittest build definitions, so the test runner is included alongside the C++ test.

Once execution permits a dedicated local server, run the complete 19-case FTS SQL suite and repeated, interleaved official benchmarks against the saved second-round binary. Retain the two previously reproduced upstream failures unless evidence changes. Check all nine timing metrics and exact query results before publishing this candidate. The five official benchmark assets have been compared byte-for-byte with competition upstream and remain unchanged.
