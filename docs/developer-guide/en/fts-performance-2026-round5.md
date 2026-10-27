# FTS fifth optimization: allocation and operation dispatch

This candidate builds on `6f2ee5fcd3d54479201585ae3687fc8099af319c`, the fourth-round submission context. The user reports latest official raw score **43.384/100**, equivalent to **21.692/50** by the task weight (computed). The platform supplies only the total; task ID/SHA and per-metric timings were not provided. Revision attribution follows conversation context and has not been independently verified against a task record.

The published formula maps that score to 21.692% equal-category mean time reduction against the official baseline. Full score requires 50%, leaving 28.308 percentage points of further aggregate improvement. Component timings below do not establish that threshold or predict the next official score.

## Implementation

- Private word maps allocate buckets and nodes through the existing caller allocator in TOKENIZE, index-row generation, optimizer tokenization and retrieval tokenization. Their bucket counts, hash values, key equality and iteration order are preserved. The original allocator-label API remains available; failure and explicit destruction clear the parent binding, and a repeated create cannot switch the owner of a live map. Arena users release this temporary memory with their enclosing operation. Token bytes and generated index rows retain their original ownership and lifetime.
- Each private map retains the hash and comparison function selected from current metadata. Every token still passes through the same scalar operation; changing its type/collation refreshes dispatch. This removes repeated metadata dispatch without enlarging each token. The shared read-only stopword hash set retains its stateless hash/equality operations.
- The built-in stopword dictionary generates a sorted array of short alphabetic ASCII keys at initialization. ASCII case folding and trailing-space padding match the dictionary's existing general_ci rules. Unicode, controls, punctuation and unsupported encodings follow the existing conversion/hash path. A future non-ASCII dictionary entry disables this shortcut for the entire dictionary. Keys derive from the dictionary, and each input word is still checked.
- Posting-ID comparison specializes unsigned integers and unpadded binary VARCHAR IDs after resolving their metadata, preserving NULL-first behavior. Other types and collations use the previous datum comparator. The operation is available for heap inlining. Posting visibility, score accumulation, ordering, filtering and retrieval ranges are unchanged.

Index rows already contain inline capacity for four cells; no speculative row-buffer allocation change was made. No index-format migration is needed.

## Correctness and build

The expanded **11 common native tests passed on the unchanged fourth-round libraries first**. The final candidate passed those same test sources plus **four new allocator/dispatch tests: 15 total**. The driver compiles the additional test translation unit alongside the existing tests; it retains source hashes for each. The Release build completed with the fixed toolchain and `./build.sh release --make -j24`, exit code zero.

The new semantic checks compare exact index-row order, all four cells, frequencies, document lengths and both inverted/doc-word layouts against segmentation results in 36 combinations. All 26^3 lowercase triples, mixed-case forms of every built-in stopword, all byte boundaries, accents, Unicode, controls, NUL and padding are compared with the original collation-aware dictionary under four collations. The posting comparator matches the original datum-function ordering in 100,352 pairs including NULL, unsigned extremes, embedded NUL, varying lengths and padded/nonbinary fallback cases.

Allocator tests cover 400 nodes, forced collisions, reuse, rebinding after destruction, legacy API recreation, refusal to rebind a live map, retry after failed creation, and cleanup at **25 allocation failure points**. Hash/comparison dispatch is checked against the old scalar functions while repeatedly changing type and collation. The previous parser lifetime, malformed-input and concurrent stopword regressions remain passing.

Two reference-harness mistakes were fixed before product changes: using an unavailable integer setter name, then providing no backing storage for numeric `ObDatum` setters. The corrected backing vector follows the documented datum contract. Original compile/crash results remain in local logs; they were not product regressions or modified upstream assertions. Debugger ptrace was unavailable, so a diagnostic own-process stack trace identified the erroneous setter. The successful reference/candidate checks and all timed trials ran without diagnostic instrumentation.

## Complete native observations

Both saved executables ran old/new/new/old/old/new for each workload on CPUs 4,5,6,7 after compilation. The table is the arithmetic mean of all three observations per version, in whole-process seconds. All trials passed and every sample is retained.

| Native workload | Fourth-round mean seconds | Candidate mean seconds | Time reduction |
| --- | ---: | ---: | ---: |
| `index_word_rows` | 2.888106 | 2.871414 | 0.58% |
| `stopword_reads` | 6.395360 | 3.187609 | 50.16% |
| `id_comparisons` | 5.844822 | 5.310006 | 9.15% |
| `beng_documents` | 3.572531 | 2.186219 | 38.80% |
| `legacy_allocator_map` | 3.939990 | 3.439342 | 12.71% |

The final index-row change is only about 0.58% amid substantial host variation: this does not establish a stable index-build improvement. Stopword and BENG component work shows reductions, while the ID comparison workload also includes the former out-of-line call boundary. The legacy allocator-map workload exercises changed adapters and dispatch; it is not an unchanged control. None of these observations is a SQL TOKENIZE, complete index-build, MATCH, nine-metric benchmark, or competition-score measurement. No aggregate score is calculated from them.

The index-row test repeats 1,500 times per process, the eight-reader stopword test 1,000 times, ID comparisons 1,500 times, the 100-pair BENG test 10,000 times (2,000,000 documents), and the collision/legacy allocator test 1,000 times. Fixtures, reference oracles, assertions and process startup are included. CPUs are not exclusive and frequency is not fixed. There is no subtraction, timing adjustment or outlier removal.

An earlier implementation without per-map dispatch caching had a 4.46% legacy allocator-map regression. Its full measurements and binary identities remain in the [JSON artifact](fts-performance-2026-round5.json), alongside the complete final observations. These are different source patches, not selected repetitions of one version. Earlier-round improvements are not added together.

## Reproduction and remaining validation

```bash
./build.sh release --make -j24
python3 unittest/storage/fts/run_word_map_test.py --label round5
```

The execution environment still rejects local socket creation with EPERM, so no dedicated SQL server, full SQL suite or official benchmark ran for this candidate. All five official benchmark files are byte-identical to competition upstream. The next required steps are a full native SQL FTS regression, complete repeated official benchmarks, and a formal platform evaluation. The original two upstream SQL failures remain documented separately; they have not been changed or hidden.

The user performs repository pushes. This report does not assert the candidate is already on GitHub. The updated workspace submission document supplies the exact candidate SHA/branch and an offline bundle after the local commit is created.
