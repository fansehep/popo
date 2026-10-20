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

// Expression types
enum class ExprType
{
    Literal,
    ColumnRef,
    BinaryOp,
    UnaryOp,
    FunctionCall,
    Star
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
    Or
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
    Max
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

// Order by item
struct OrderByItem
{
    ExprPtr expr;
    bool ascending = true;
};

// Join type
enum class JoinType
{
    Inner,
    Left,
    Right,
    Cross
};

// Table reference (can be a simple table or a join)
struct TableRef;
using TableRefPtr = std::shared_ptr<TableRef>;

enum class TableRefType
{
    Simple,
    Join
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
    ShowTables,
    Describe,
    ShowDatabases,
    SetVar,
    ShowVariables,
    ShowStatus,
    SelectDatabase,
    SelectSystemVar
};

struct Statement
{
    StmtType type;
    virtual ~Statement() = default;
};
using StmtPtr = std::shared_ptr<Statement>;

struct SelectStatement : Statement
{
    std::vector<ExprPtr> select_list; // projection columns
    TableRefPtr from; // FROM clause
    ExprPtr where; // WHERE clause (optional)
    std::vector<ExprPtr> group_by; // GROUP BY (optional)
    ExprPtr having; // HAVING (optional)
    std::vector<OrderByItem> order_by; // ORDER BY (optional)
    std::optional<int64_t> limit; // LIMIT (optional)
    SelectStatement() { type = StmtType::Select; }
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
