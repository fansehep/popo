# TPC-H benchmark harness

`tpch_bench` uses the official TPC-H `dbgen` and `qgen` binaries. Query text is
generated separately for every scale factor, so substitutions such as Q11's
threshold are never reused from SF1.

```sh
./tpch_bench \
  --tpch-home /opt/tpch-kit \
  --scale 10 \
  --data-dir /bench/tpch/sf10/tbl \
  --db-dir /bench/tpch/sf10/db \
  --result-dir /bench/tpch/sf10/results \
  --engine both --phase all --resume \
  --memory-budget 4294967296 \
  --buffer-pool-pages 524288 \
  --temp-dir /bench/tpch/sf10/tmp
```

`--dbgen` and `--qgen` may be supplied instead of the conventional
`TPCH_HOME/dbgen/{dbgen,qgen}` paths. The phases `generate`, `load`, and
`query` can be run independently. `--resume` keeps completed loads and skips
queries that already have per-query JSON files; `--force` is required before
an existing database is replaced.

Both engines use the same primary keys and no query-specific secondary
indexes. `.tbl` input is read one line at a time. SQLite uses a prepared
statement and PopoDB uses bounded transaction batches. Results include
per-query JSON, an appendable CSV, and run metadata. The load phase finishes
with a PopoDB flush/checkpoint or SQLite WAL checkpoint.

For `--engine both`, `comparison.json` verifies every ordered result cell with
an absolute numeric tolerance of `1e-6` plus a relative tolerance of `1e-8`.
Per-query JSON includes a tolerance-normalized FNV-1a digest, sampled peak
process RSS, executor-accounted peak bytes, and spill file/byte totals. Q15 is
run as one create-view/query/drop-view lifecycle; cleanup is attempted even
when its query fails.
