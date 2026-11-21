#pragma once
#include <memory>
#include <optional>
#include <string>
#include <vector>
#include "parser/ast.h"
#include "type/value.h"

namespace popo::planner
{

enum class PlanNodeType
{
    Scan,
    Filter,
    Project,
    Join,
    Agg,
    Sort,
    Limit,
    Insert,
    Update,
    Delete,
    CreateTable,
    CreateIndex,
    DropTable,
    AlterTable,
    Distinct,
    SetOp,
    SubqueryScan,
    RecursiveScan,
    Window,
    Dual,
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

struct PlanNode;
using PlanNodePtr = std::shared_ptr<PlanNode>;

struct PlanNode
{
    PlanNodeType type;
    virtual ~PlanNode() = default;
};

// Table scan: read rows from a table (optionally projected to needed physical columns)
struct ScanNode : PlanNode
{
    std::string table_name;
    std::string alias; // optional alias for the table
    std::vector<std::string> column_names; // resolved column names
    std::vector<popo::type::ValueType> column_types; // resolved column types
    std::vector<uint16_t> projected_columns; // physical schema column indices when has_projection is true
    bool has_projection = false;
    ScanNode() { type = PlanNodeType::Scan; }
};

// Filter: WHERE or HAVING condition
struct FilterNode : PlanNode
{
    PlanNodePtr child;
    popo::parser::ExprPtr condition;
    FilterNode() { type = PlanNodeType::Filter; }
};

// Project: SELECT column list
struct ProjectNode : PlanNode
{
    PlanNodePtr child;
    std::vector<popo::parser::ExprPtr> expressions; // output expressions
    std::vector<std::string> output_names; // output column names/aliases
    ProjectNode() { type = PlanNodeType::Project; }
};

// Join two inputs
struct JoinNode : PlanNode
{
    popo::parser::JoinType join_type;
    PlanNodePtr left;
    PlanNodePtr right;
    popo::parser::ExprPtr condition; // ON condition
    JoinNode() { type = PlanNodeType::Join; }
};

// Aggregation: GROUP BY + aggregate functions
struct AggNode : PlanNode
{
    PlanNodePtr child;
    std::vector<popo::parser::ExprPtr> group_by; // GROUP BY expressions
    std::vector<popo::parser::ExprPtr> agg_exprs; // aggregate expressions (COUNT, SUM, etc.)
    std::vector<std::string> output_names; // output column names
    AggNode() { type = PlanNodeType::Agg; }
};

// Sort: ORDER BY
struct SortNode : PlanNode
{
    PlanNodePtr child;
    std::vector<popo::parser::OrderByItem> order_by;
    SortNode() { type = PlanNodeType::Sort; }
};

// Limit: LIMIT N OFFSET M
struct LimitNode : PlanNode
{
    PlanNodePtr child;
    int64_t limit_count = -1; // -1: no limit (offset only)
    int64_t offset_count = 0;
    LimitNode() { type = PlanNodeType::Limit; }
};

// Distinct: deduplicate rows
struct DistinctNode : PlanNode
{
    PlanNodePtr child;
    DistinctNode() { type = PlanNodeType::Distinct; }
};

// Set operation: UNION / INTERSECT / EXCEPT
struct SetOpNode : PlanNode
{
    PlanNodePtr left;
    PlanNodePtr right;
    popo::parser::SetOpKind kind = popo::parser::SetOpKind::Union;
    bool is_all = false;
    SetOpNode() { type = PlanNodeType::SetOp; }
};

// Scan over a materialized subquery (CTE reference or derived table)
struct SubqueryScanNode : PlanNode
{
    std::string alias;
    std::string cte_name;
    bool recursive = false;
    std::vector<std::string> column_names;
    PlanNodePtr child;
    SubqueryScanNode() { type = PlanNodeType::SubqueryScan; }
};

// Self-reference inside a WITH RECURSIVE body. Reads the previous iteration.
struct RecursiveScanNode : PlanNode
{
    std::string cte_name;
    RecursiveScanNode() { type = PlanNodeType::RecursiveScan; }
};

// Window functions computed over the child rows.
struct WindowNode : PlanNode
{
    PlanNodePtr child;
    std::vector<popo::parser::ExprPtr> windows;
    WindowNode() { type = PlanNodeType::Window; }
};

// Single empty row: SELECT without FROM
struct DualNode : PlanNode
{
    DualNode() { type = PlanNodeType::Dual; }
};

// Insert rows into a table
struct InsertNode : PlanNode
{
    std::string table_name;
    std::vector<std::string> columns;
    std::vector<std::vector<popo::parser::ExprPtr>> values;
    InsertNode() { type = PlanNodeType::Insert; }
};

// Update rows
struct UpdateNode : PlanNode
{
    std::string table_name;
    std::vector<popo::parser::UpdateAssignment> assignments;
    popo::parser::ExprPtr where_clause; // optional
    UpdateNode() { type = PlanNodeType::Update; }
};

// Delete rows
struct DeleteNode : PlanNode
{
    std::string table_name;
    popo::parser::ExprPtr where_clause; // optional
    DeleteNode() { type = PlanNodeType::Delete; }
};

// Create table DDL
struct CreateTableNode : PlanNode
{
    std::string table_name;
    std::vector<popo::parser::ColumnDef> columns;
    CreateTableNode() { type = PlanNodeType::CreateTable; }
};

struct CreateIndexNode : PlanNode
{
    bool unique = false;
    std::string index_name;
    std::string table_name;
    std::string column_name;
    CreateIndexNode() { type = PlanNodeType::CreateIndex; }
};

// Drop table DDL
struct DropTableNode : PlanNode
{
    std::string table_name;
    bool if_exists = false;
    DropTableNode() { type = PlanNodeType::DropTable; }
};

// Alter table DDL
struct AlterTableNode : PlanNode
{
    std::string table_name;
    popo::parser::AlterAction action;
    popo::parser::ColumnDef column;
    std::string target_name;
    std::vector<std::string> key_columns;
    AlterTableNode() { type = PlanNodeType::AlterTable; }
};

// Show tables
struct ShowTablesNode : PlanNode
{
    ShowTablesNode() { type = PlanNodeType::ShowTables; }
};

// Describe table
struct DescribeNode : PlanNode
{
    std::string table_name;
    DescribeNode() { type = PlanNodeType::Describe; }
};

// Show databases
struct ShowDatabasesNode : PlanNode
{
    ShowDatabasesNode() { type = PlanNodeType::ShowDatabases; }
};

// Set variable
struct SetVarNode : PlanNode
{
    SetVarNode() { type = PlanNodeType::SetVar; }
};

// Show variables
struct ShowVariablesNode : PlanNode
{
    ShowVariablesNode() { type = PlanNodeType::ShowVariables; }
};

// Show status
struct ShowStatusNode : PlanNode
{
    ShowStatusNode() { type = PlanNodeType::ShowStatus; }
};

// SELECT DATABASE()
struct SelectDatabaseNode : PlanNode
{
    SelectDatabaseNode() { type = PlanNodeType::SelectDatabase; }
};

// SELECT @@xxx
struct SelectSystemVarNode : PlanNode
{
    std::vector<std::string> var_names;
    std::vector<std::string> column_names;
    SelectSystemVarNode() { type = PlanNodeType::SelectSystemVar; }
};

struct CreateViewNode : PlanNode
{
    std::string name;
    bool or_replace = false;
    std::vector<std::string> column_names;
    popo::parser::StmtPtr query;
    CreateViewNode() { type = PlanNodeType::CreateView; }
};

struct DropViewNode : PlanNode
{
    std::string name;
    bool if_exists = false;
    DropViewNode() { type = PlanNodeType::DropView; }
};

} // namespace popo::planner
