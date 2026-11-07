#pragma once

#include <memory>
#include <optional>
#include <string>
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
    Window
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

// FROM (SELECT ...) AS alias
struct DerivedTableRef : TableRef
{
    StmtPtr query;
    std::string alias;
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
    DropView
};

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
    StmtPtr query;
    CreateViewStatement() { type = StmtType::CreateView; }
};

struct DropViewStatement : Statement
{
    std::string name;
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
    RenameTable
};

struct AlterTableStatement : Statement
{
    std::string table_name;
    AlterAction action;
    ColumnDef column; // ADD COLUMN
    std::string target_name; // DROP COLUMN: column name; RENAME TO: new table name
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
    std::string var_name;
    std::string value;
    SetVarStatement() { type = StmtType::SetVar; }
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
    std::string var_name;
    SelectSystemVarStatement() { type = StmtType::SelectSystemVar; }
};

} // namespace popo::parser
