# PopoDB

[![C++23](https://img.shields.io/badge/C%2B%2B-23-00599C.svg?logo=cplusplus)](https://en.cppreference.com/w/cpp/23)
[![Platform](https://img.shields.io/badge/platform-Linux%20(io__uring)-FCC624.svg?logo=linux&logoColor=black)](https://kernel.dk/io_uring.pdf)
[![Protocol](https://img.shields.io/badge/protocol-MySQL%20v10-4479A1.svg?logo=mysql&logoColor=white)](https://dev.mysql.com/doc/dev/mysql-server/latest/PAGE_PROTOCOL.html)
[![License: MIT](https://img.shields.io/badge/license-MIT-2ea44f.svg)](https://opensource.org/licenses/MIT)

[中文](README.md) | **English**

**PopoDB** is a single-node relational database management system written from scratch in modern **C++23** for Linux. It exposes a native **MySQL Wire Protocol v10** server backed by a **three-tier hybrid SQL execution engine** (primary-key fast path, custom bytecode VM, and spill-capable analytical plan interpreter), **row-level MVCC** across all four standard SQL isolation levels, **ARIES-style logical WAL recovery**, and an **append-only `zstd`-compressed B+Tree storage engine** powered by **`io_uring`**.

> **Status**: PopoDB is built for database internals exploration, correctness verification, and OLTP/OLAP performance experimentation. It is not yet intended for production-critical data.

---

## Table of Contents

- [Key Highlights](#key-highlights)
- [Architecture](#architecture)
- [Quick Start](#quick-start)
- [SQL Tour & Feature Matrix](#sql-tour--feature-matrix)
- [Configuration](#configuration)
- [Testing, Code Quality & Benchmarks](#testing-code-quality--benchmarks)
- [Project Layout](#project-layout)
- [License](#license)

---

## Key Highlights

### 1. Wire Protocol & Security (`server`)
- **MySQL Protocol v10 Compatibility**: Standalone `Asio` TCP server directly usable with `mysql` CLI, MariaDB client, JDBC drivers, DBeaver, Navicat, and standard MySQL tooling.
- **Pluggable Authentication**: Supports `caching_sha2_password` (both fast-auth and RSA public-key exchange), `mysql_native_password`, and `sha256_password`.
- **Persistent Accounts & RBAC**: Full support for `CREATE/ALTER/DROP USER`, `SET PASSWORD`, `GRANT`, `REVOKE`, and `SHOW GRANTS` with `'user'@'host'` matching (`%`, `127.0.0.1`, etc.) and database/table-level privilege enforcement.

### 2. Three-Tier Hybrid SQL Engine (`parser` · `planner` · `bytecode` · `executor`)
- **Tier 1 — Primary-Key Point-Select Fast Path (`try_point_select`)**: Short-circuits lexing, parsing, and planning for `SELECT col FROM tbl WHERE pk = const`, reading directly through MVCC / Adaptive Hash Index / B+Tree with zero WAL traffic on autocommit reads.
- **Tier 2 — Bytecode Compiler & Stack VM (`Compiler` + `VirtualMachine`)**: Compiles common OLTP CRUD, primary/secondary index seeks, single-table sorts, and aggregations into compact custom opcodes.
- **Tier 3 — Analytical Plan Interpreter & External Spilling (`Executor` + `Spill`)**: Executes **SQL:2006** and **TPC-H Q1–Q22** (plus RF1/RF2), featuring predicate pushdown, join reordering, Hash Equi-Join (`INNER`/`LEFT`/`RIGHT`/`FULL`/`CROSS`), correlated & uncorrelated subquery caching, recursive CTEs, views, window functions, and SQL/XML publishing—paired with `MemoryTracker` and `TempFileManager` for budget-bounded **external merge sort** and **hash-partitioned join/aggregation spilling**.

### 3. Row-Level MVCC & Isolation (`trancation`)
- **No-Steal Multi-Version Concurrency Control (`MvccManager`)**: Uses the clustered B+Tree as the committed baseline while maintaining incremental CSN version chains and per-transaction write overlays, streaming snapshot-visible rows via ordered merge cursors and garbage-collecting obsolete versions.
- **All Four SQL Isolation Levels**: Supports `READ UNCOMMITTED`, `READ COMMITTED`, `REPEATABLE READ`, and `SERIALIZABLE` (First-Committer-Wins write-conflict detection + read-range validation).
- **Session Transaction Control**: Supports `BEGIN` / `START TRANSACTION [ISOLATION LEVEL ...]`, `COMMIT`, `ROLLBACK`, `SET [GLOBAL|SESSION] TRANSACTION ISOLATION LEVEL`, and `SET autocommit`.

### 4. Compressed Append-Only Storage & Crash Recovery (`storage`)
- **Clustered B+Tree & Secondary Indexes (`TableHeap`)**: Stores primary keys and rows together in 16 KB logical pages with single/composite primary keys, secondary & `UNIQUE` indexes, and an InnoDB-inspired **Adaptive Hash Index (`AdaptiveHashIndex`)** for hot point lookups.
- **1 MB Zstd Append-Only Blocks & GC (`DiskManager`)**: Each table is stored in `<table>.ndb` as 1 MB blocks (`BlockMeta` + `zstd`-compressed page payloads) written copy-on-write with automatic dead-space **Garbage Collection (GC)**.
- **`io_uring` Batched I/O & Dual-Layer Durability**: Submits batched page I/O and `IOSQE_IO_LINK` write+`fsync` chains through Linux `io_uring` (`IoRing`); `commit.wal` provides a single durability barrier across multi-table page flushes, while **ARIES-style logical WAL (`redo.log`) + `RecoveryManager`** handles Analysis, Redo, CLR compensation, DDL catalog recovery, and checkpoints.

### 5. Modern C++23 Engineering (`base`)
- **Exception-Free Recoverable Error Model**: Uses `popo::base::Result<T>` (`std::expected<T, Error>`) and `POPO_TRY` across all project code, enforced at build time by `check_expected_errors` (rejecting `throw`/`try`/`catch`, `std::sto*`, and unchecked `.value()`).
- **Runtime Infrastructure**: Links `jemalloc` as the global allocator alongside `cpptrace` diagnostics and `spdlog` logging.

---

## Architecture

```text
                           MySQL Clients (mysql CLI / JDBC / DBeaver)
                                               │
                                    MySQL Wire Protocol v10
                                               ▼
┌──────────────────────────────────────────────────────────────────────────────────────────────┐
│  Server Layer (src/server)                                                                   │
│  Asio TCP Server ──► Handshake & Auth Plugins ──► AccountStore (RBAC) ──► Session / TxnCtrl  │
└──────────────────────────────┬───────────────────────────────────────┬───────────────────────┘
                               │ PK Point Lookup                       │ General SQL
                               ▼                                       ▼
                ┌─────────────────────────────┐         ┌─────────────────────────────┐
                │   Point-Select Fast Path    │         │  Lexer ──► Parser ──► AST   │
                │  (src/bytecode/fast_path.h) │         │            │                │
                └──────────────┬──────────────┘         │            ▼                │
                               │                        │     Planner (PlanTree)      │
                               │                        └──────┬───────────────┬──────┘
                               │                  Simple OLTP  │               │ Analytical / SQL:2006
                               │                               ▼               ▼
                               │                    ┌────────────────┐ ┌────────────────────────────┐
                               │                    │    Bytecode    │ │      Plan Interpreter      │
                               │                    │ Compiler & VM  │ │ HashJoin / Subquery Cache  │
                               │                    │ (src/bytecode) │ │ External Sort & Hash Spill │
                               │                    └───────┬────────┘ └─────────────┬──────────────┘
                               └────────────────────────────┼──────────┬─────────────┘
                                                            ▼          ▼
┌──────────────────────────────────────────────────────────────────────────────────────────────┐
│  Transaction & Catalog Layer (src/trancation · src/catalog)                                  │
│  MvccManager (RU / RC / RR / Serializable, CSN Version Chains, Per-Txn Overlay) · SchemaMgr  │
└───────────────────────────────────────────────┬──────────────────────────────────────────────┘
                                                ▼
┌──────────────────────────────────────────────────────────────────────────────────────────────┐
│  Storage & Recovery Layer (src/storage)                                                      │
│  BufferPool (16KB Pages, LRU) ──► TableHeap (Clustered B+Tree, Secondary Idx, Adaptive Hash) │
│               │                                                     │                        │
│               ▼                                                     ▼                        │
│  DiskManager (1MB Zstd Append-Only Blocks + GC + commit.wal)   RedoLogManager & ARIES Recov  │
│               └──────────────────────────┬──────────────────────────┘                        │
│                                          ▼                                                   │
│                            IoRing (Linux io_uring + fsync)                                   │
└──────────────────────────────────────────┬───────────────────────────────────────────────────┘
                                           ▼
                 Data Directory: <table>.ndb │ commit.wal │ redo.log │ popo_accounts
```

### On-Disk Data Directory Layout

| File | Purpose |
| :--- | :--- |
| `<table>.ndb` | Per-table storage file: `4 KB FileHeader` + `1 MB Blocks` (`4 KB BlockMeta` + appended `zstd`-compressed 16 KB page images). |
| `commit.wal` | Block-generation commit barrier ensuring atomic multi-table page publication with a single `fsync`. |
| `redo.log` / `redo.log.lsn` | ARIES logical write-ahead log and durable LSN watermark for DML, DDL, transaction states, CLRs, and checkpoints. |
| `popo_accounts` | Persistent user accounts, authentication plugins, credentials, and privilege grants. |

---

## Quick Start

### Prerequisites

PopoDB targets **Linux** (using kernel `io_uring`) and requires:

- A **C++23**-capable compiler (GCC 14+ or Clang 18+ recommended)
- **CMake 3.20+**
- System libraries: `liburing`, `jemalloc`, `OpenSSL`, `zstd`, `SQLite3` (used by comparison benchmarks), and `pthread`
- Optional: `mysql` / `mariadb` CLI and client development headers (for client connections and differential MySQL integration tests)

On Debian / Ubuntu:

```bash
sudo apt update
sudo apt install -y build-essential cmake pkg-config \
  liburing-dev libjemalloc-dev libssl-dev libzstd-dev libsqlite3-dev \
  default-mysql-client libmariadb-dev
```

### Build

```bash
git clone https://github.com/fansehep/popo.git
cd popo

cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j"$(nproc)"
```

> **Note**: CMake `FetchContent` downloads `cpptrace`, `spdlog`, `googletest`, and `asio` during configuration. In restricted network environments, export `HTTP_PROXY` / `HTTPS_PROXY` or set `GH_PROXY` in `CMakeLists.txt`.

### Start & Connect

By default, `popodb-server` loads `deploy/popodb.cnf` (or `popodb.cnf` in the working directory) and listens on `0.0.0.0:3307`:

```bash
# Start the server
./build/src/popodb-server

# Connect with the standard MySQL client
mysql -h 127.0.0.1 -P 3307 -u root
```

Common CLI and environment overrides:

```bash
# Specify an explicit configuration file
./build/src/popodb-server --defaults-file=deploy/popodb.cnf

# Override listening port via positional argument
./build/src/popodb-server 13306

# Bootstrap initial root password and auth plugin on first startup
POPO_PASSWORD=secret POPO_AUTH_PLUGIN=caching_sha2_password \
  ./build/src/popodb-server
```

See `deploy/popodb.service` for a `systemd` unit example and `deploy/init.sql` for sample account provisioning statements.

---

## SQL Tour & Feature Matrix

### DDL, Indexes & Transactions

```sql
CREATE TABLE users (
    id         BIGINT PRIMARY KEY,
    name       VARCHAR(255) NOT NULL,
    dept       VARCHAR(64)  DEFAULT 'eng',
    salary     DECIMAL(10, 2),
    created_at DATE
);

CREATE INDEX idx_users_dept ON users(dept);

INSERT INTO users VALUES
    (1, 'Alice',   'eng',   18000.00, DATE '2026-01-10'),
    (2, 'Bob',     'eng',   22000.00, DATE '2026-02-15'),
    (3, 'Charlie', 'sales', 19500.00, DATE '2026-03-01');

BEGIN ISOLATION LEVEL REPEATABLE READ;
UPDATE users SET salary = salary + 1500.00 WHERE id = 1;
COMMIT;
```

### Analytical Queries (CTE + Window Functions + Intervals)

```sql
WITH recent_staff AS (
    SELECT id, name, dept, salary
    FROM users
    WHERE created_at >= DATE '2026-01-01' + INTERVAL '15' DAY
)
SELECT
    name,
    dept,
    salary,
    RANK() OVER (PARTITION BY dept ORDER BY salary DESC) AS dept_rank
FROM recent_staff
ORDER BY dept, dept_rank;
```

### User Accounts & Privilege Management

```sql
CREATE USER IF NOT EXISTS 'app'@'%' IDENTIFIED BY 'app_password';
GRANT SELECT, INSERT, UPDATE, DELETE ON popo.* TO 'app'@'%';
SHOW GRANTS FOR 'app'@'%';
REVOKE DELETE ON popo.* FROM 'app'@'%';
```

### SQL Coverage Matrix

| Category | Supported Statements & Features |
| :--- | :--- |
| **DDL** | `CREATE TABLE` (single/composite `PRIMARY KEY`, `NOT NULL`, `UNIQUE`, `DEFAULT`), `ALTER TABLE` (`ADD/DROP COLUMN`, `RENAME TO`, `ADD PRIMARY KEY`), `DROP TABLE [IF EXISTS]`, `CREATE [UNIQUE] INDEX`, `CREATE [OR REPLACE] VIEW`, `DROP VIEW [IF EXISTS]` |
| **DML** | `INSERT INTO ... VALUES` (single/multi-row, optional column list), `UPDATE ... SET ... [WHERE ...]`, `DELETE FROM ... [WHERE ...]` |
| **Core Queries** | `SELECT [DISTINCT]`, `WHERE`, `GROUP BY`, `HAVING`, `ORDER BY [ASC\|DESC]`, `LIMIT` / `OFFSET`, table & column aliases |
| **Joins** | `INNER JOIN`, `LEFT [OUTER] JOIN`, `RIGHT [OUTER] JOIN`, `FULL [OUTER] JOIN`, `CROSS JOIN`, implicit comma joins |
| **Analytical (SQL:2006)** | `WITH` / `WITH RECURSIVE` CTEs, derived tables, scalar/`IN`/`EXISTS`/`ANY`/`ALL` correlated & uncorrelated subqueries, `UNION [ALL]` / `INTERSECT [ALL]` / `EXCEPT [ALL]`, window functions (`ROW_NUMBER`, `RANK`, `DENSE_RANK`, and aggregate `OVER`) |
| **Expressions & Functions** | `CASE WHEN`, `CAST`, `COALESCE`, `NULLIF`, `SUBSTRING`/`SUBSTR`, `UPPER`/`LOWER`, `TRIM`, `EXTRACT`, `INTERVAL` (`YEAR`/`MONTH`/`DAY`), `[NOT] LIKE`, `[NOT] BETWEEN`, `IS [NOT] NULL`, `IS [NOT] DISTINCT FROM`, `COUNT`/`SUM`/`AVG`/`MIN`/`MAX`, SQL/XML publishing (`XMLELEMENT`/`XMLFOREST`/`XMLAGG`, etc.) |
| **Transactions** | `BEGIN` / `START TRANSACTION [ISOLATION LEVEL ...]`, `COMMIT`, `ROLLBACK`, `SET [GLOBAL\|SESSION] TRANSACTION ISOLATION LEVEL`, `SET autocommit = 0\|1\|ON\|OFF` |
| **Accounts & RBAC** | `CREATE USER [IF NOT EXISTS]`, `ALTER USER`, `SET PASSWORD`, `DROP USER [IF EXISTS]`, `GRANT`, `REVOKE`, `SHOW GRANTS` |
| **Metadata & Session** | `SHOW TABLES`, `SHOW DATABASES`, `DESCRIBE` / `DESC`, `SHOW VARIABLES`, `SHOW STATUS`, `SELECT DATABASE()`, `SELECT @@var` |
| **Data Types** | `TINYINT` / `SMALLINT` / `INT` / `BIGINT` (plus unsigned internal representations), `FLOAT` / `DOUBLE` / `DECIMAL`, `CHAR` / `VARCHAR` / `TEXT`, `BOOL`, `DATE`, `NULL` |

---

## Configuration

`deploy/popodb.cnf` uses a MySQL-style INI format (unknown keys are ignored):

```ini
[popodb]
port = 3307
bind-address = 0.0.0.0
datadir = data
database = popo
default-auth-plugin = caching_sha2_password
root-password =
require-full-auth = 0
```

| Option | Default | Env / CLI Override | Description |
| :--- | :--- | :--- | :--- |
| `port` | `3307` | Positional CLI arg | TCP listening port (`./popodb-server 13306` takes highest precedence). |
| `bind-address` | `0.0.0.0` | — | Local network interface address to bind. |
| `datadir` | `data` | — | Directory storing `.ndb` table files, WAL logs, and `popo_accounts`. |
| `database` | `popo` | — | Default logical database name. |
| `default-auth-plugin` | `caching_sha2_password` | `POPO_AUTH_PLUGIN` | Default authentication plugin advertised during handshake (`mysql_native_password`, `sha256_password`). |
| `root-password` | *(empty)* | `POPO_PASSWORD` | Initial password for `root@'%'` **only** when creating `{datadir}/popo_accounts` for the first time. |
| `require-full-auth` | `0` | `POPO_AUTH_FULL` | Set to `1` (or define `POPO_AUTH_FULL`) to force RSA full authentication in `caching_sha2_password`. |

---

## Testing, Code Quality & Benchmarks

### Unit & Integration Tests

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j"$(nproc)"

# Run all test suites (storage, bytecode VM, SQL:2006, TPC-H Q1-Q22, MVCC isolation, crash recovery, auth/RBAC)
ctest --test-dir build --output-on-failure -j"$(nproc)"

# Verify that project sources strictly follow the Result<T> exception-free policy
cmake --build build --target check_expected_errors
```

> Differential MySQL integration tests (`sql_integration_test`) automatically skip when a local MySQL 8.0 server is unavailable. Formatting and static analysis rules are configured in `.clang-format` and `.clang-tidy`.

### Benchmarks (OLTP / OLAP / Official TPC-H)

Build in `Release` mode before running performance benchmarks:

- **Built-in Micro & Workload Suites**:
  - `./build/tests/sql/sql_bench`: single-table point lookup, scan, filter, and update throughput
  - `./build/tests/sql/oltp_bench`: classic OLTP workloads (YCSB, TPC-C, TPC-E, SmallBank, TATP, Sysbench, pgbench, AuctionMark, Wikipedia)
  - `./build/tests/sql/olap_bench`: analytical scan, projection pruning, filtered aggregation, and join benchmarks
- **Official TPC-H Dual-Engine Harness (`tpch_bench`)**:
  - Generates scale-specific data and queries via official `dbgen` / `qgen`, runs PopoDB alongside SQLite, validates ordered result cells within numeric tolerance, and records RSS plus executor memory/spill statistics:

```bash
./build/bench/tpch/tpch_bench \
  --tpch-home /path/to/tpch-kit \
  --scale 1 \
  --data-dir /tmp/tpch/sf1/tbl \
  --db-dir /tmp/tpch/sf1/db \
  --result-dir /tmp/tpch/sf1/results \
  --temp-dir /tmp/tpch/sf1/tmp \
  --memory-budget 1073741824 \
  --buffer-pool-pages 131072 \
  --engine both --phase all
```

See [`bench/tpch/README.md`](bench/tpch/README.md) for full CLI options and verification details.

---

## Project Layout

```text
.
├── src/
│   ├── base/         # Result<T> (std::expected) error handling, numeric parsers, and base macros
│   ├── type/         # Value / Row representations and type conversions
│   ├── catalog/      # Table, column, view, and index metadata management & persistence
│   ├── parser/       # SQL lexer, recursive-descent parser, and AST definitions
│   ├── expression/   # Expression representations and evaluation helpers
│   ├── planner/      # Logical query planner and plan node definitions
│   ├── bytecode/     # PK point-select fast path, bytecode compiler, opcodes, and stack VM
│   ├── executor/     # Analytical plan interpreter, join/subquery optimizations, and external spill
│   ├── storage/      # BufferPool, clustered B+Tree, adaptive hash, 1MB Zstd blocks, io_uring, ARIES WAL
│   ├── trancation/   # Row-level MVCC, snapshot merge cursors, conflict validation, and txn manager
│   └── server/       # Asio TCP server, MySQL Wire Protocol v10, auth plugins, and RBAC AccountStore
├── bench/
│   └── tpch/         # Official dbgen/qgen TPC-H benchmark harness (PopoDB vs SQLite)
├── deploy/           # Sample popodb.cnf configuration, init.sql, and systemd service unit
├── cmake/            # Build-time policy checks (CheckExpectedErrors.cmake)
└── tests/            # Storage, bytecode, expression, SQL:2006, TPC-H, isolation, recovery & server tests
```

---

## License

Released under the [MIT License](https://opensource.org/licenses/MIT).
