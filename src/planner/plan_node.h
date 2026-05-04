#pragma once
#include <string>
#include <vector>
#include <memory>
#include <optional>
#include "parser/ast.h"
#include "type/value.h"

namespace neon::planner {

enum class PlanNodeType {
    Scan, Filter, Project, Join, Agg, Sort, Limit,
    Insert, Update, Delete, CreateTable, DropTable
};

struct PlanNode;
using PlanNodePtr = std::shared_ptr<PlanNode>;

struct PlanNode {
    PlanNodeType type;
    virtual ~PlanNode() = default;
};

// Table scan: read all rows from a table
struct ScanNode : PlanNode {
    std::string table_name;
    std::string alias;  // optional alias for the table
    std::vector<std::string> column_names; // resolved column names
    std::vector<neon::type::ValueType> column_types; // resolved column types
    ScanNode() { type = PlanNodeType::Scan; }
};

// Filter: WHERE or HAVING condition
struct FilterNode : PlanNode {
    PlanNodePtr child;
    neon::parser::ExprPtr condition;
    FilterNode() { type = PlanNodeType::Filter; }
};

// Project: SELECT column list
struct ProjectNode : PlanNode {
    PlanNodePtr child;
    std::vector<neon::parser::ExprPtr> expressions; // output expressions
    std::vector<std::string> output_names;           // output column names/aliases
    ProjectNode() { type = PlanNodeType::Project; }
};

// Join two inputs
struct JoinNode : PlanNode {
    neon::parser::JoinType join_type;
    PlanNodePtr left;
    PlanNodePtr right;
    neon::parser::ExprPtr condition;  // ON condition
    JoinNode() { type = PlanNodeType::Join; }
};

// Aggregation: GROUP BY + aggregate functions
struct AggNode : PlanNode {
    PlanNodePtr child;
    std::vector<neon::parser::ExprPtr> group_by;      // GROUP BY expressions
    std::vector<neon::parser::ExprPtr> agg_exprs;     // aggregate expressions (COUNT, SUM, etc.)
    std::vector<std::string> output_names;             // output column names
    AggNode() { type = PlanNodeType::Agg; }
};

// Sort: ORDER BY
struct SortNode : PlanNode {
    PlanNodePtr child;
    std::vector<neon::parser::OrderByItem> order_by;
    SortNode() { type = PlanNodeType::Sort; }
};

// Limit: LIMIT N
struct LimitNode : PlanNode {
    PlanNodePtr child;
    int64_t limit_count;
    LimitNode() { type = PlanNodeType::Limit; }
};

// Insert rows into a table
struct InsertNode : PlanNode {
    std::string table_name;
    std::vector<std::string> columns;
    std::vector<std::vector<neon::parser::ExprPtr>> values;
    InsertNode() { type = PlanNodeType::Insert; }
};

// Update rows
struct UpdateNode : PlanNode {
    std::string table_name;
    std::vector<neon::parser::UpdateAssignment> assignments;
    neon::parser::ExprPtr where_clause;  // optional
    UpdateNode() { type = PlanNodeType::Update; }
};

// Delete rows
struct DeleteNode : PlanNode {
    std::string table_name;
    neon::parser::ExprPtr where_clause;  // optional
    DeleteNode() { type = PlanNodeType::Delete; }
};

// Create table DDL
struct CreateTableNode : PlanNode {
    std::string table_name;
    std::vector<neon::parser::ColumnDef> columns;
    CreateTableNode() { type = PlanNodeType::CreateTable; }
};

// Drop table DDL
struct DropTableNode : PlanNode {
    std::string table_name;
    bool if_exists = false;
    DropTableNode() { type = PlanNodeType::DropTable; }
};

} // namespace neon::planner
