#pragma once

#include <cctype>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>
#include "type/value.h"

namespace popo::parser
{

// Forward declarations
struct Expr;
using ExprPtr = std::shared_ptr<Expr>;
struct Statement;
using StmtPtr = std::shared_ptr<Statement>;
struct SelectStatement;

// Expression types
enum class ExprType
{
    Literal,
    ColumnRef,
    BinaryOp,
    UnaryOp,
    FunctionCall,
    Star,
    In,
    Between,
    Like,
    IsNull,
    Case,
    Subquery,
    InSubquery,
    Exists,
    Alias,
    DistinctFrom,
    Quantified,
    ScalarCall,
    Cast,
    Window,
    Interval
};

enum class IntervalUnit : uint8_t
{
    Year,
    Month,
    Day
};

enum class BinaryOpType
{
    // Arithmetic
    Add,
    Sub,
    Mul,
    Div,
    // Comparison
    Eq,
    Ne,
    Lt,
    Le,
    Gt,
    Ge,
    // Logical
    And,
    Or,
    // String
    Concat
};

enum class UnaryOpType
{
    Not,
    Neg
};

enum class AggFuncType
{
    Count,
    Sum,
    Avg,
    Min,
    Max,
    XmlAgg
};

struct Expr
{
    ExprType type;
    virtual ~Expr() = default;
};

struct LiteralExpr : Expr
{
    popo::type::Value value;
    LiteralExpr() { type = ExprType::Literal; }
};

struct ColumnRefExpr : Expr
{
    std::string table_name; // optional, for t.col
    std::string column_name;
    ColumnRefExpr() { type = ExprType::ColumnRef; }
};

struct StarExpr : Expr
{
    std::string table_name; // optional, for t.*
    StarExpr() { type = ExprType::Star; }
};

struct BinaryOpExpr : Expr
{
    BinaryOpType op;
    ExprPtr left;
    ExprPtr right;
    BinaryOpExpr() { type = ExprType::BinaryOp; }
};

struct UnaryOpExpr : Expr
{
    UnaryOpType op;
    ExprPtr operand;
    UnaryOpExpr() { type = ExprType::UnaryOp; }
};

struct FunctionCallExpr : Expr
{
    AggFuncType func;
    std::vector<ExprPtr> args;
    bool is_distinct = false;
    FunctionCallExpr() { type = ExprType::FunctionCall; }
};

struct InExpr : Expr
{
    ExprPtr operand;
    std::vector<ExprPtr> list;
    bool negated = false;
    InExpr() { type = ExprType::In; }
};

struct BetweenExpr : Expr
{
    ExprPtr operand;
    ExprPtr low;
    ExprPtr high;
    bool negated = false;
    BetweenExpr() { type = ExprType::Between; }
};

struct LikeExpr : Expr
{
    ExprPtr operand;
    ExprPtr pattern;
    bool negated = false;
    LikeExpr() { type = ExprType::Like; }
};

struct IsNullExpr : Expr
{
    ExprPtr operand;
    bool negated = false; // IS NOT NULL
    IsNullExpr() { type = ExprType::IsNull; }
};

struct CaseExpr : Expr
{
    ExprPtr operand; // simple CASE operand, null for searched CASE
    std::vector<std::pair<ExprPtr, ExprPtr>> whens; // condition/value -> result
    ExprPtr else_expr; // optional
    CaseExpr() { type = ExprType::Case; }
};

// Scalar subquery: (SELECT ...)
struct SubqueryExpr : Expr
{
    StmtPtr query;
    SubqueryExpr() { type = ExprType::Subquery; }
};

// operand [NOT] IN (SELECT ...)
struct InSubqueryExpr : Expr
{
    ExprPtr operand;
    StmtPtr query;
    bool negated = false;
    InSubqueryExpr() { type = ExprType::InSubquery; }
};

// EXISTS (SELECT ...)
struct ExistsExpr : Expr
{
    StmtPtr query;
    ExistsExpr() { type = ExprType::Exists; }
};

// expr AS alias (select list only)
struct AliasExpr : Expr
{
    ExprPtr expr;
    std::string alias;
    AliasExpr() { type = ExprType::Alias; }
};

// expr IS [NOT] DISTINCT FROM expr. NULL matches NULL.
struct DistinctFromExpr : Expr
{
    ExprPtr left;
    ExprPtr right;
    bool negated = false; // IS NOT DISTINCT FROM
    DistinctFromExpr() { type = ExprType::DistinctFrom; }
};

// expr op ANY|ALL (SELECT ...)
struct QuantifiedExpr : Expr
{
    ExprPtr operand;
    BinaryOpType op = BinaryOpType::Eq;
    bool all = false;
    StmtPtr query;
    QuantifiedExpr() { type = ExprType::Quantified; }
};

// COALESCE, NULLIF, UPPER, XML publishing functions, and other scalar calls.
struct ScalarCallExpr : Expr
{
    std::string name;
    std::vector<ExprPtr> args;
    ScalarCallExpr() { type = ExprType::ScalarCall; }
};

struct CastExpr : Expr
{
    ExprPtr operand;
    popo::type::ValueType target = popo::type::ValueType::Int64;
    CastExpr() { type = ExprType::Cast; }
};

// Order by item
struct OrderByItem
{
    ExprPtr expr;
    bool ascending = true;
};

// function OVER (PARTITION BY ... ORDER BY ...)
struct WindowExpr : Expr
{
    ExprPtr function;
    std::vector<ExprPtr> partition_by;
    std::vector<OrderByItem> order_by;
    WindowExpr() { type = ExprType::Window; }
};

// INTERVAL '90' DAY / INTERVAL '3' MONTH / INTERVAL '1' YEAR
struct IntervalExpr : Expr
{
    ExprPtr value;
    IntervalUnit unit = IntervalUnit::Day;
    IntervalExpr() { type = ExprType::Interval; }
};

// Join type
enum class JoinType
{
    Inner,
    Left,
    Right,
    Full,
    Cross
};

// Table reference (can be a simple table or a join)
struct TableRef;
using TableRefPtr = std::shared_ptr<TableRef>;

enum class TableRefType
{
    Simple,
    Join,
    Derived
};

struct TableRef
{
    TableRefType type;
    virtual ~TableRef() = default;
};

struct SimpleTableRef : TableRef
{
    std::string table_name;
    std::string alias;
    std::vector<std::string> column_aliases;
    SimpleTableRef() { type = TableRefType::Simple; }
};

struct JoinTableRef : TableRef
{
    JoinType join_type;
    TableRefPtr left;
    TableRefPtr right;
    ExprPtr condition;
    JoinTableRef() { type = TableRefType::Join; }
};

// FROM (SELECT ...) AS alias [(col1, col2, ...)]
struct DerivedTableRef : TableRef
{
    StmtPtr query;
    std::string alias;
    std::vector<std::string> column_aliases;
    DerivedTableRef() { type = TableRefType::Derived; }
};

// Column definition for CREATE TABLE
struct ColumnDef
{
    std::string name;
    popo::type::ValueType data_type;
    int32_t type_len = 0;
    bool is_primary_key = false;
    bool is_nullable = true;
    bool is_unique = false;
    std::optional<popo::type::Value> default_value;
};

// Update assignment: SET col = expr
struct UpdateAssignment
{
    std::string column_name;
    ExprPtr value;
};

// Statement types
enum class StmtType
{
    Select,
    Insert,
    CreateTable,
    CreateIndex,
    Update,
    Delete,
    DropTable,
    AlterTable,
    ShowTables,
    Describe,
    ShowDatabases,
    SetVar,
    ShowVariables,
    ShowStatus,
    SelectDatabase,
    SelectSystemVar,
    CreateView,
    DropView,
    CreateUser,
    DropUser,
    AlterUser,
    Grant,
    Revoke,
    ShowGrants,
    Begin,
    Commit,
    Rollback,
    SetTransaction
};

// SQL standard isolation levels, named as the standard spells them. The storage
// layer has its own enum; the session maps between the two.
enum class IsolationLevel : uint8_t
{
    ReadUncommitted,
    ReadCommitted,
    RepeatableRead,
    Serializable
};

// SET TRANSACTION without a scope applies to the next transaction only.
enum class TxnScope : uint8_t
{
    Next,
    Session,
    Global
};

// MySQL spells these with dashes in @@transaction_isolation.
inline const char * isolation_level_name(IsolationLevel level)
{
    switch (level)
    {
        case IsolationLevel::ReadUncommitted:
            return "READ-UNCOMMITTED";
        case IsolationLevel::ReadCommitted:
            return "READ-COMMITTED";
        case IsolationLevel::RepeatableRead:
            return "REPEATABLE-READ";
        case IsolationLevel::Serializable:
            return "SERIALIZABLE";
    }
    return "READ-COMMITTED";
}

// Accepts READ COMMITTED, READ-COMMITTED, read_committed, ...
inline std::optional<IsolationLevel> isolation_level_from_name(std::string_view text)
{
    std::string norm;
    norm.reserve(text.size());
    for (char ch : text)
    {
        if (ch == '-' || ch == '_' || std::isspace(static_cast<unsigned char>(ch)) != 0)
        {
            continue;
        }
        norm.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
    }
    if (norm == "readuncommitted")
        return IsolationLevel::ReadUncommitted;
    if (norm == "readcommitted")
        return IsolationLevel::ReadCommitted;
    if (norm == "repeatableread")
        return IsolationLevel::RepeatableRead;
    if (norm == "serializable")
        return IsolationLevel::Serializable;
    return std::nullopt;
}

// Privilege bits stored on GRANT. GRANT OPTION is separate from ALL PRIVILEGES.
inline constexpr uint32_t kPrivSelect = 1u << 0;
inline constexpr uint32_t kPrivInsert = 1u << 1;
inline constexpr uint32_t kPrivUpdate = 1u << 2;
inline constexpr uint32_t kPrivDelete = 1u << 3;
inline constexpr uint32_t kPrivCreate = 1u << 4;
inline constexpr uint32_t kPrivDrop = 1u << 5;
inline constexpr uint32_t kPrivAlter = 1u << 6;
inline constexpr uint32_t kPrivIndex = 1u << 7;
inline constexpr uint32_t kPrivCreateUser = 1u << 8;
inline constexpr uint32_t kPrivGrantOption = 1u << 9;
inline constexpr uint32_t kPrivAll = kPrivSelect | kPrivInsert | kPrivUpdate | kPrivDelete | kPrivCreate | kPrivDrop | kPrivAlter
    | kPrivIndex | kPrivCreateUser;

struct Statement
{
    StmtType type;
    virtual ~Statement() = default;
};

// WITH name AS (SELECT ...)
enum class SetOpKind
{
    Union,
    Intersect,
    Except
};

struct SetTerm
{
    SetOpKind kind = SetOpKind::Union;
    bool is_all = false;
    std::shared_ptr<SelectStatement> query;
};

struct CteDef
{
    std::string name;
    std::vector<std::string> column_names;
    std::shared_ptr<SelectStatement> query;
    bool recursive = false;
};

struct SelectStatement : Statement
{
    std::vector<CteDef> with_clause; // WITH (optional)
    bool distinct = false;
    std::vector<ExprPtr> select_list; // projection columns
    TableRefPtr from; // FROM clause (optional)
    ExprPtr where; // WHERE clause (optional)
    std::vector<ExprPtr> group_by; // GROUP BY (optional)
    ExprPtr having; // HAVING (optional)
    std::vector<OrderByItem> order_by; // ORDER BY (optional)
    std::optional<int64_t> limit; // LIMIT (optional)
    std::optional<int64_t> offset; // OFFSET (optional)
    // UNION / INTERSECT / EXCEPT. INTERSECT and EXCEPT bind tighter than UNION.
    std::vector<SetTerm> set_ops;
    SelectStatement() { type = StmtType::Select; }
};

struct CreateViewStatement : Statement
{
    std::string name;
    bool or_replace = false;
    std::vector<std::string> column_names;
    StmtPtr query;
    CreateViewStatement() { type = StmtType::CreateView; }
};

struct DropViewStatement : Statement
{
    std::string name;
    bool if_exists = false;
    DropViewStatement() { type = StmtType::DropView; }
};

struct InsertStatement : Statement
{
    std::string table_name;
    std::vector<std::string> columns; // optional column list
    std::vector<std::vector<ExprPtr>> values; // VALUES rows
    InsertStatement() { type = StmtType::Insert; }
};

struct CreateTableStatement : Statement
{
    std::string table_name;
    std::vector<ColumnDef> columns;
    CreateTableStatement() { type = StmtType::CreateTable; }
};

struct CreateIndexStatement : Statement
{
    bool unique = false;
    std::string index_name;
    std::string table_name;
    std::vector<std::string> columns;
    CreateIndexStatement() { type = StmtType::CreateIndex; }
};

struct UpdateStatement : Statement
{
    std::string table_name;
    std::vector<UpdateAssignment> assignments;
    ExprPtr where; // optional
    UpdateStatement() { type = StmtType::Update; }
};

struct DeleteStatement : Statement
{
    std::string table_name;
    ExprPtr where; // optional
    DeleteStatement() { type = StmtType::Delete; }
};

struct DropTableStatement : Statement
{
    std::string table_name;
    bool if_exists = false;
    DropTableStatement() { type = StmtType::DropTable; }
};

enum class AlterAction
{
    AddColumn,
    DropColumn,
    RenameTable,
    AddPrimaryKey,
    NoOp
};

struct AlterTableStatement : Statement
{
    std::string table_name;
    AlterAction action;
    ColumnDef column; // ADD COLUMN
    std::string target_name; // DROP COLUMN: column name; RENAME TO: new table name
    std::vector<std::string> key_columns; // ADD PRIMARY KEY (...)
    AlterTableStatement() { type = StmtType::AlterTable; }
};

struct ShowTablesStatement : Statement
{
    ShowTablesStatement() { type = StmtType::ShowTables; }
};

struct DescribeStatement : Statement
{
    std::string table_name;
    DescribeStatement() { type = StmtType::Describe; }
};

struct ShowDatabasesStatement : Statement
{
    ShowDatabasesStatement() { type = StmtType::ShowDatabases; }
};

struct SetVarStatement : Statement
{
    std::string var_name; // lowercased, scope prefix stripped
    std::string value;
    TxnScope scope = TxnScope::Session;
    SetVarStatement() { type = StmtType::SetVar; }
};

struct BeginStatement : Statement
{
    // BEGIN ISOLATION LEVEL ... / START TRANSACTION ISOLATION LEVEL ...
    std::optional<IsolationLevel> isolation;
    BeginStatement() { type = StmtType::Begin; }
};

struct CommitStatement : Statement
{
    CommitStatement() { type = StmtType::Commit; }
};

struct RollbackStatement : Statement
{
    RollbackStatement() { type = StmtType::Rollback; }
};

struct SetTransactionStatement : Statement
{
    TxnScope scope = TxnScope::Next;
    IsolationLevel isolation = IsolationLevel::ReadCommitted;
    SetTransactionStatement() { type = StmtType::SetTransaction; }
};

struct ShowVariablesStatement : Statement
{
    ShowVariablesStatement() { type = StmtType::ShowVariables; }
};

struct ShowStatusStatement : Statement
{
    ShowStatusStatement() { type = StmtType::ShowStatus; }
};

struct SelectDatabaseStatement : Statement
{
    SelectDatabaseStatement() { type = StmtType::SelectDatabase; }
};

struct SelectSystemVarStatement : Statement
{
    std::vector<std::string> var_names;
    std::vector<std::string> column_names;
    SelectSystemVarStatement() { type = StmtType::SelectSystemVar; }
};

// 'user'@'host' plus an optional password from IDENTIFIED BY.
struct AccountName
{
    std::string user;
    std::string host = "%";
    std::string password;
    bool has_password = false;
    std::string plugin;
};

struct CreateUserStatement : Statement
{
    bool if_not_exists = false;
    std::vector<AccountName> users;
    CreateUserStatement() { type = StmtType::CreateUser; }
};

struct DropUserStatement : Statement
{
    bool if_exists = false;
    std::vector<AccountName> users;
    DropUserStatement() { type = StmtType::DropUser; }
};

struct AlterUserStatement : Statement
{
    AccountName user;
    AlterUserStatement() { type = StmtType::AlterUser; }
};

struct GrantStatement : Statement
{
    uint32_t privs = 0;
    std::string database = "*";
    std::string table = "*";
    std::vector<AccountName> users;
    GrantStatement() { type = StmtType::Grant; }
};

struct RevokeStatement : Statement
{
    uint32_t privs = 0;
    std::string database = "*";
    std::string table = "*";
    std::vector<AccountName> users;
    RevokeStatement() { type = StmtType::Revoke; }
};

struct ShowGrantsStatement : Statement
{
    bool current_user = true;
    AccountName user;
    ShowGrantsStatement() { type = StmtType::ShowGrants; }
};

} // namespace popo::parser
