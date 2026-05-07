# Neon 数据库

[![C++23](https://img.shields.io/badge/C%2B%2B-23-blue.svg)](https://en.cppreference.com/w/cpp/23)
[![License](https://img.shields.io/badge/license-MIT-green.svg)](LICENSE)

**Neon** 是一个用现代 C++23 从零构建的轻量级关系型数据库，兼容 MySQL 有线协议（MySQL Wire Protocol），支持通过标准 MySQL 客户端（`mysql` CLI、JDBC 等）直接连接。

---

## 特性

- **MySQL 协议兼容** — 原生支持 MySQL Wire Protocol v10，任何 MySQL 客户端可直接连接
- **完整 SQL 支持** — SELECT / INSERT / UPDATE / DELETE / CREATE TABLE / DROP TABLE
- **高级查询** — WHERE 过滤、ORDER BY 排序、LIMIT 限制、聚合函数（COUNT / SUM / AVG / MIN / MAX）、多表 JOIN
- **B+Tree 聚簇索引** — 主键数据与索引存储于同一 B+Tree，查询高效
- **Undo 空间与事务** — 支持行级 MVCC 和多版本回滚
- **字节码虚拟机** — SQL 经过 解析 → 规划 → 编译 生成自定义字节码，在 VM 中执行
- **C++23 `std::expected<>` 错误处理** — 全链路统一错误处理，零异常开销
- **类型系统** — INT8 / INT16 / INT32 / INT64 / FLOAT32 / FLOAT64 / STRING / BOOL / NULL

---

## 演示

在终端中通过 MySQL 客户端连接 Neon，执行建表、插入、查询、聚合、更新、删除全流程：

![Neon Demo](demo.svg)

---

## 架构

```
┌─────────────────────────────────────┐
│          MySQL 客户端                │
│    (mysql CLI / JDBC / Navicat)     │
└──────────────┬──────────────────────┘
               │ MySQL Wire Protocol (TCP)
┌──────────────▼──────────────────────┐
│         neon-server                 │
│  ┌──────────────────────────────┐   │
│  │       Session (连接管理)      │   │
│  └──────────┬───────────────────┘   │
│             │ SQL String            │
│  ┌──────────▼───────────────────┐   │
│  │    Parser (词法/语法分析)     │   │
│  │         → AST                 │   │
│  └──────────┬───────────────────┘   │
│             │ AST Nodes             │
│  ┌──────────▼───────────────────┐   │
│  │    Planner (逻辑计划生成)     │   │
│  │         → Plan Tree           │   │
│  └──────────┬───────────────────┘   │
│             │ Plan Tree             │
│  ┌──────────▼───────────────────┐   │
│  │    Compiler (字节码编译)      │   │
│  │         → Program             │   │
│  └──────────┬───────────────────┘   │
│             │ Bytecode Program      │
│  ┌──────────▼───────────────────┐   │
│  │  VirtualMachine (虚拟机执行)  │   │
│  │         → ExecuteSuccess      │   │
│  └──────────┬───────────────────┘   │
│             │ Result Rows           │
│  ┌──────────▼───────────────────┐   │
│  │   StorageEngine (存储引擎)    │   │
│  │  ┌───────┐ ┌──────────────┐  │   │
│  │  │B+Tree │ │  Undo Space  │  │   │
│  │  │Index  │ │  (MVCC)      │  │   │
│  │  └───────┘ └──────────────┘  │   │
│  │  ┌────────────────────────┐  │   │
│  │  │   BufferPool (LRU)     │  │   │
│  │  └───────────┬────────────┘  │   │
│  │  ┌───────────▼────────────┐  │   │
│  │  │  DiskManager (磁盘IO)  │  │   │
│  │  └────────────────────────┘  │   │
│  └──────────────────────────────┘   │
└─────────────────────────────────────┘
```

---

## 快速开始

### 依赖

- C++23 编译器（GCC 15+ 或 Clang 19+）
- CMake 3.20+
- `mysql` 客户端（用于连接测试）

### 编译

```bash
git clone https://github.com/your/neon.git
cd neon
mkdir -p build2 && cd build2
cmake .. -DCMAKE_BUILD_TYPE=Release
cmake --build . -j$(nproc)
```

### 启动服务

```bash
./neon-server
# 默认监听 127.0.0.1:3307
```

自定义端口：
```bash
./neon-server 13306
```

### 连接

```bash
mysql -h 127.0.0.1 -P 3307 -u root
```

---

## SQL 示例

```sql
-- 建表
CREATE TABLE users (
    id INT PRIMARY KEY,
    name VARCHAR(255),
    age INT
);

-- 插入
INSERT INTO users (id, name, age) VALUES (1, 'Alice', 25);
INSERT INTO users VALUES (2, 'Bob', 30), (3, 'Charlie', 35);

-- 查询
SELECT * FROM users;
SELECT name, age FROM users WHERE age > 26;
SELECT COUNT(*), AVG(age) FROM users;

-- 排序 & 限制
SELECT * FROM users ORDER BY age DESC LIMIT 2;

-- 更新 & 删除
UPDATE users SET age = age + 1 WHERE name = 'Alice';
DELETE FROM users WHERE id = 3;

-- 删表
DROP TABLE users;
```

---

## 支持的 SQL

| 语句 | 说明 |
|------|------|
| `CREATE TABLE` | 创建表（支持 PRIMARY KEY） |
| `DROP TABLE [IF EXISTS]` | 删除表 |
| `INSERT INTO ... VALUES` | 单行/多行插入 |
| `SELECT ... FROM ...` | 查询 |
| `SELECT ... WHERE ...` | 条件过滤（=, <>, <, <=, >, >=, AND, OR） |
| `SELECT ... ORDER BY ...` | 排序（ASC / DESC） |
| `SELECT ... LIMIT n` | 结果限制 |
| `SELECT ... JOIN ... ON ...` | 嵌套循环连接 |
| `SELECT COUNT/SUM/AVG/MIN/MAX(...)` | 聚合函数 |
| `UPDATE ... SET ... WHERE ...` | 更新行 |
| `DELETE FROM ... WHERE ...` | 删除行 |
| `SHOW TABLES` | 显示所有表 |
| `DESCRIBE table` | 显示表结构 |

### 数据类型

`TINYINT` / `SMALLINT` / `INT` / `BIGINT` / `FLOAT` / `DOUBLE` / `VARCHAR(n)` / `TEXT` / `BOOL`

---

## 项目结构

```
src/
├── base/           # 基础设施（错误处理、宏）
├── parser/         # SQL 词法分析 & 语法分析
├── planner/        # 逻辑计划生成
├── bytecode/       # 字节码定义、编译器、虚拟机
├── catalog/        # 元数据管理（Schema、Column）
├── expression/     # 表达式求值
├── storage/        # 存储引擎（B+Tree、BufferPool、DiskManager、Undo）
├── type/           # 类型系统（Value、Tuple）
├── server/         # 网络服务（MySQL 协议、Session）
└── trancation/     # 事务管理（MVCC）
tests/
├── bytecode/       # 编译器 & VM 单元测试
├── storage/        # 存储引擎单元测试 & 压力测试
├── catalog/        # Schema 管理测试
└── integration/    # SQL 集成测试（对比 MySQL 8.0）
```

---

## 许可证

MIT License
