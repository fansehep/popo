# PopoDB

[![C++23](https://img.shields.io/badge/C%2B%2B-23-00599C.svg?logo=cplusplus)](https://en.cppreference.com/w/cpp/23)
[![Platform](https://img.shields.io/badge/platform-Linux%20(io__uring)-FCC624.svg?logo=linux&logoColor=black)](https://kernel.dk/io_uring.pdf)
[![Protocol](https://img.shields.io/badge/protocol-MySQL%20v10-4479A1.svg?logo=mysql&logoColor=white)](https://dev.mysql.com/doc/dev/mysql-server/latest/PAGE_PROTOCOL.html)
[![License: MIT](https://img.shields.io/badge/license-MIT-2ea44f.svg)](https://opensource.org/licenses/MIT)

**中文** | [English](README_EN.md)

**PopoDB** 是一个使用现代 **C++23** 从零构建的单机关系型数据库，面向 Linux 平台设计。它提供原生 **MySQL Wire Protocol v10** 网络服务，内部融合了**三层混合 SQL 执行引擎**（主键点查快路径、自定义字节码虚拟机、支持外部落盘的分析型计划解释器）、**四级隔离行级 MVCC**、**ARIES 风格逻辑 WAL 恢复**，以及基于 **`io_uring` + `zstd` 1 MB 追加块**的聚簇 B+Tree 存储引擎。

> **状态说明**：本项目主要用于数据库内核研发实践、架构探索与 OLTP/OLAP 基准测试，暂不建议直接承载生产环境核心业务数据。

---

## 目录

- [核心特性](#核心特性)
- [系统架构](#系统架构)
- [快速开始](#快速开始)
- [SQL 示例与支持矩阵](#sql-示例与支持矩阵)
- [配置说明](#配置说明)
- [测试、代码质量与基准评估](#测试代码质量与基准评估)
- [目录结构](#目录结构)
- [许可证](#许可证)

---

## 核心特性

### 1. 网络协议与安全鉴权 (`server`)
- **MySQL 协议兼容**：基于独立 `Asio` 实现 MySQL Wire Protocol v10，可直接通过 `mysql` CLI、MariaDB 客户端、JDBC、DBeaver、Navicat 等标准工具连接。
- **多认证插件支持**：完整支持 `caching_sha2_password`（快速认证与 RSA 公钥加密交换）、`mysql_native_password` 与 `sha256_password`。
- **持久化账号与 RBAC 权限体系**：支持 `CREATE/ALTER/DROP USER`、`SET PASSWORD`、`GRANT`、`REVOKE`、`SHOW GRANTS`，支持按 `'user'@'host'`（`%`、`127.0.0.1` 等）匹配及库表级权限校验。

### 2. 三层混合 SQL 执行引擎 (`parser` · `planner` · `bytecode` · `executor`)
- **Tier 1 — 主键点查快路径 (`try_point_select`)**：针对 `SELECT col FROM tbl WHERE pk = const` 跳过完整词法/语法/编译流程，直达 MVCC / 自适应哈希索引 / B+Tree，自提交只读查询零 WAL 开销。
- **Tier 2 — 字节码编译器与栈式 VM (`Compiler` + `VirtualMachine`)**：将高频 OLTP 增删改查、主键/二级索引查找、单表排序与聚合编译为紧凑的自定义操作码高效执行。
- **Tier 3 — 分析型计划解释器与外部落盘 (`Executor` + `Spill`)**：覆盖完整 **SQL:2006** 与 **TPC-H Q1–Q22**（含 RF1/RF2），内置谓词下推、连接重排、Hash Equi-Join（`INNER`/`LEFT`/`RIGHT`/`FULL`/`CROSS`）、相关/非相关子查询结果缓存、递归 CTE、视图、窗口函数与 SQL/XML 发布函数；同时通过 `MemoryTracker` 与 `TempFileManager` 实现受内存预算约束的**外部归并排序**与 **Hash 分区落盘连接/聚合**。

### 3. 行级 MVCC 事务与并发隔离 (`trancation`)
- **No-Steal 多版本并发控制 (`MvccManager`)**：以聚簇 B+Tree 为已提交基线，维护增量 CSN 版本链与事务私有写缓冲层（Overlay），通过流式归并游标按主键顺序输出快照可见行，并自动回收过期历史版本。
- **完整四级 SQL 隔离级别**：支持 `READ UNCOMMITTED`、`READ COMMITTED`、`REPEATABLE READ` 与 `SERIALIZABLE`（采用 First-Committer-Wins 写冲突检测与读区间可串行化校验）。
- **会话级事务控制**：支持显式 `BEGIN` / `START TRANSACTION [ISOLATION LEVEL ...]`、`COMMIT`、`ROLLBACK`、`SET [GLOBAL|SESSION] TRANSACTION ISOLATION LEVEL` 以及 `SET autocommit`。

### 4. 追加压缩存储与崩溃恢复 (`storage`)
- **聚簇 B+Tree 与二级索引 (`TableHeap`)**：采用 16 KB 逻辑页存储主键与行数据，支持单列/复合主键、普通/唯一（`UNIQUE`）二级索引，以及仿 InnoDB 的热页**自适应哈希索引 (`AdaptiveHashIndex`)**。
- **1 MB Zstd 追加写块与自动 GC (`DiskManager`)**：每张表对应独立的 `.ndb` 数据文件，16 KB 逻辑页经 `zstd` 压缩后以 Copy-on-Write 方式追加写入 1 MB 物理块（`BlockMeta` + 压缩页载荷），后台根据死空间阈值自动触发块级垃圾回收（GC）。
- **`io_uring` 批量 I/O 与双层持久化恢复**：底层通过 `IoRing` 批量提交读写并使用 `IOSQE_IO_LINK` 串联写与 `fsync`；跨表脏页发布由 `commit.wal` 提供单次持久化屏障，事务级持久性与崩溃恢复由 **ARIES 风格逻辑日志 (`redo.log`) + `RecoveryManager`**（Analysis、Redo、CLR 补偿记录、DDL 元数据恢复与 Checkpoint）保障。

### 5. 现代 C++23 工程实践 (`base`)
- **零异常可恢复错误模型**：全项目统一采用 `popo::base::Result<T>`（`std::expected<T, Error>`）与 `POPO_TRY` 传递错误，并通过 CMake 静态门禁目标 `check_expected_errors` 严格拦截 `throw`/`try`/`catch`、`std::sto*` 与未检查的 `.value()`。
- **运行时基础设施**：默认链接 `jemalloc` 替换全局分配器，集成 `cpptrace` 栈回溯与 `spdlog` 日志系统。

---

## 系统架构

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

### 数据目录文件说明

| 文件 | 作用 |
| :--- | :--- |
| `<table>.ndb` | 单表数据文件：`4 KB FileHeader` + 若干 `1 MB Block`（`4 KB BlockMeta` + `zstd` 压缩逻辑页追加区）。 |
| `commit.wal` | 块级原子发布日志：单次提交跨多表刷盘时只执行一次持久化屏障，保障页映像代际（Generation）一致性。 |
| `redo.log` / `redo.log.lsn` | ARIES 逻辑预写日志与持久化水位：记录 DML、DDL、事务状态、CLR 与 Checkpoint，用于崩溃恢复重放。 |
| `popo_accounts` | 账号、认证插件、凭证与库表授权元数据文件。 |

---

## 快速开始

### 环境要求

当前实现面向 **Linux**（依赖内核 `io_uring` 支持），构建需要：

- 支持 **C++23** 的编译器（建议 GCC 14+ 或 Clang 18+）
- **CMake 3.20+**
- 系统库：`liburing`、`jemalloc`、`OpenSSL`、`zstd`、`SQLite3`（基准对比与测试使用）、`pthread`
- 可选：`mysql` / `mariadb` 客户端及开发库（用于命令行连接与 MySQL 对比集成测试）

以 Debian / Ubuntu 为例安装依赖：

```bash
sudo apt update
sudo apt install -y build-essential cmake pkg-config \
  liburing-dev libjemalloc-dev libssl-dev libzstd-dev libsqlite3-dev \
  default-mysql-client libmariadb-dev
```

### 编译构建

```bash
git clone https://github.com/fansehep/neon.git
cd neon

cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j"$(nproc)"
```

> **提示**：构建阶段会通过 CMake `FetchContent` 拉取 `cpptrace`、`spdlog`、`googletest` 与 `asio`。若处于受限网络环境，可直接导出 `HTTP_PROXY` / `HTTPS_PROXY` 环境变量，或在 `CMakeLists.txt` 中配置 `GH_PROXY`。

### 启动服务与客户端连接

默认情况下，`popodb-server` 会自动查找当前工作目录下的 `deploy/popodb.cnf`（或 `popodb.cnf`），默认监听 `0.0.0.0:3307`：

```bash
# 启动服务端
./build/src/popodb-server

# 使用标准 MySQL 客户端连接
mysql -h 127.0.0.1 -P 3307 -u root
```

常见启动参数与环境变量覆盖方式：

```bash
# 显式指定配置文件
./build/src/popodb-server --defaults-file=deploy/popodb.cnf

# 命令行直接覆盖监听端口
./build/src/popodb-server 13306

# 首次初始化账号文件时注入 root 密码与默认认证插件
POPO_PASSWORD=secret POPO_AUTH_PLUGIN=caching_sha2_password \
  ./build/src/popodb-server
```

仓库内的 `deploy/popodb.service` 提供了 `systemd` 托管示例，`deploy/init.sql` 提供了业务账号与最小权限初始化脚本。

---

## SQL 示例与支持矩阵

### 基础建表、索引与事务操作

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

### 分析查询（CTE + 窗口函数 + 日期区间）

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

### 账号与权限管理

```sql
CREATE USER IF NOT EXISTS 'app'@'%' IDENTIFIED BY 'app_password';
GRANT SELECT, INSERT, UPDATE, DELETE ON popo.* TO 'app'@'%';
SHOW GRANTS FOR 'app'@'%';
REVOKE DELETE ON popo.* FROM 'app'@'%';
```

### SQL 支持矩阵

| 分类 | 支持语法与能力 |
| :--- | :--- |
| **DDL** | `CREATE TABLE`（单列/复合 `PRIMARY KEY`、`NOT NULL`、`UNIQUE`、`DEFAULT`）、`ALTER TABLE`（`ADD/DROP COLUMN`、`RENAME TO`、`ADD PRIMARY KEY`）、`DROP TABLE [IF EXISTS]`、`CREATE [UNIQUE] INDEX`、`CREATE [OR REPLACE] VIEW`、`DROP VIEW [IF EXISTS]` |
| **DML** | `INSERT INTO ... VALUES`（单行/多行批量插入、可选列清单）、`UPDATE ... SET ... [WHERE ...]`、`DELETE FROM ... [WHERE ...]` |
| **基础查询** | `SELECT [DISTINCT]`、`WHERE`、`GROUP BY`、`HAVING`、`ORDER BY [ASC\|DESC]`、`LIMIT` / `OFFSET`、表别名与列别名 |
| **多表连接** | `INNER JOIN`、`LEFT [OUTER] JOIN`、`RIGHT [OUTER] JOIN`、`FULL [OUTER] JOIN`、`CROSS JOIN`、隐式逗号连接 |
| **高级分析 (SQL:2006)** | `WITH` / `WITH RECURSIVE` CTE、派生表、标量/`IN`/`EXISTS`/`ANY`/`ALL` 相关与非相关子查询、`UNION [ALL]` / `INTERSECT [ALL]` / `EXCEPT [ALL]`、窗口函数（`ROW_NUMBER`、`RANK`、`DENSE_RANK` 及聚合 `OVER`） |
| **表达式与函数** | `CASE WHEN`、`CAST`、`COALESCE`、`NULLIF`、`SUBSTRING`/`SUBSTR`、`UPPER`/`LOWER`、`TRIM`、`EXTRACT`、`INTERVAL`（`YEAR`/`MONTH`/`DAY`）、`[NOT] LIKE`、`[NOT] BETWEEN`、`IS [NOT] NULL`、`IS [NOT] DISTINCT FROM`、`COUNT`/`SUM`/`AVG`/`MIN`/`MAX`、SQL/XML 函数（`XMLELEMENT`/`XMLFOREST`/`XMLAGG` 等） |
| **事务控制** | `BEGIN` / `START TRANSACTION [ISOLATION LEVEL ...]`、`COMMIT`、`ROLLBACK`、`SET [GLOBAL\|SESSION] TRANSACTION ISOLATION LEVEL`、`SET autocommit = 0\|1\|ON\|OFF` |
| **账号与权限** | `CREATE USER [IF NOT EXISTS]`、`ALTER USER`、`SET PASSWORD`、`DROP USER [IF EXISTS]`、`GRANT`、`REVOKE`、`SHOW GRANTS` |
| **元数据与会话** | `SHOW TABLES`、`SHOW DATABASES`、`DESCRIBE` / `DESC`、`SHOW VARIABLES`、`SHOW STATUS`、`SELECT DATABASE()`、`SELECT @@var` |
| **数据类型** | `TINYINT` / `SMALLINT` / `INT` / `BIGINT`（含无符号内部表示）、`FLOAT` / `DOUBLE` / `DECIMAL`、`CHAR` / `VARCHAR` / `TEXT`、`BOOL`、`DATE`、`NULL` |

---

## 配置说明

`deploy/popodb.cnf` 采用 MySQL 风格的 INI 格式（未知配置项自动忽略）：

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

| 配置项 | 默认值 | 对应环境变量 | 说明 |
| :--- | :--- | :--- | :--- |
| `port` | `3307` | 命令行位置参数 | TCP 监听端口（如 `./popodb-server 13306` 优先级最高）。 |
| `bind-address` | `0.0.0.0` | — | 服务端绑定地址。 |
| `datadir` | `data` | — | 表数据、WAL 与账号文件的存储目录（相对工作目录或绝对路径）。 |
| `database` | `popo` | — | 默认逻辑数据库名。 |
| `default-auth-plugin` | `caching_sha2_password` | `POPO_AUTH_PLUGIN` | 握手默认认证插件（可选 `mysql_native_password`、`sha256_password`）。 |
| `root-password` | 空 | `POPO_PASSWORD` | **仅首次**创建 `{datadir}/popo_accounts` 时用于初始化 `root@'%'` 密码，不会覆盖已有账号文件。 |
| `require-full-auth` | `0` | `POPO_AUTH_FULL` | 设为 `1`（或设置环境变量）时强制 `caching_sha2_password` 走完整公钥加密校验。 |

---

## 测试、代码质量与基准评估

### 单元与集成测试

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j"$(nproc)"

# 运行全量测试套件（存储、字节码、SQL:2006、TPC-H Q1-Q22、MVCC 隔离、崩溃恢复、鉴权与权限）
ctest --test-dir build --output-on-failure -j"$(nproc)"

# 检查项目源码是否遵守 Result<T> 无异常错误处理规范
cmake --build build --target check_expected_errors
```

> 本机若未启动可用的 MySQL 8.0 实例，MySQL 差分对比集成测试（`sql_integration_test`）会自动跳过，不影响其余测试运行。代码风格与静态分析规则位于 `.clang-format` 与 `.clang-tidy`。

### 基准测试（OLTP / OLAP / 官方 TPC-H）

项目内置多套性能与正确性基准程序（建议在 `Release` 模式下构建）：

- **微观与工作负载基准**：
  - `./build/tests/sql/sql_bench`：单表点查、扫描、过滤与更新吞吐测试
  - `./build/tests/sql/oltp_bench`：多套经典 OLTP 负载（YCSB、TPC-C、TPC-E、SmallBank、TATP、Sysbench、pgbench、AuctionMark、Wikipedia）
  - `./build/tests/sql/olap_bench`：列裁剪、过滤聚合与连接分析吞吐测试
- **官方 TPC-H 双引擎对比基准 (`tpch_bench`)**：
  - 基于官方 `dbgen` / `qgen` 按任意 Scale Factor 生成数据与查询，支持 PopoDB 与 SQLite 同口径装载、Q1–Q22 执行、结果数值容差比对、RSS 采样及执行器内存/外部落盘统计：

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

完整参数与校验口径请参考 [`bench/tpch/README.md`](bench/tpch/README.md)。

---

## 目录结构

```text
.
├── src/
│   ├── base/         # Result<T> (std::expected) 错误处理、数值解析与基础宏
│   ├── type/         # Value / Row 内存表示与类型转换
│   ├── catalog/      # 表、列、视图、索引元数据管理与持久化序列化
│   ├── parser/       # SQL 词法分析器、递归下降语法分析器与 AST 定义
│   ├── expression/   # 通用表达式结构与求值辅助
│   ├── planner/      # 逻辑执行计划生成与结构化节点定义
│   ├── bytecode/     # 主键点查快路径、字节码编译器、操作码与栈式虚拟机
│   ├── executor/     # 分析型计划解释器、连接/子查询优化与外部落盘 (Spill)
│   ├── storage/      # BufferPool、聚簇 B+Tree、自适应哈希、1MB Zstd 块、io_uring、ARIES WAL 与恢复
│   ├── trancation/   # 四级隔离行级 MVCC、快照游标、写冲突检测与事务管理
│   └── server/       # Asio TCP 服务、MySQL Wire Protocol v10、多插件认证与 RBAC 账号存储
├── bench/
│   └── tpch/         # 基于官方 dbgen/qgen 的 PopoDB vs SQLite TPC-H 基准框架
├── deploy/           # 默认配置文件 popodb.cnf、初始化 SQL 与 systemd 服务单元示例
├── cmake/            # 构建期代码规范检查脚本 (CheckExpectedErrors.cmake)
└── tests/            # 存储、字节码、表达式、SQL:2006、TPC-H、隔离级别、崩溃恢复与服务端测试
```

---

## 许可证

本项目基于 [MIT License](https://opensource.org/licenses/MIT) 开源。
