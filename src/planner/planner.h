#pragma once
#include <expected>
#include <utility>
#include "base/error.h"
#include "catalog/schema_mgr.h"
#include "parser/ast.h"
#include "planner/plan_node.h"

namespace popo::planner
{

class Planner
{
public:
    explicit Planner(popo::catalog::SchemaManager & schema_mgr);

    popo::base::Result<PlanNodePtr> plan(const popo::parser::StmtPtr & stmt);

private:
    popo::catalog::SchemaManager & schema_mgr_;

    PlanNodePtr plan_select(const popo::parser::SelectStatement & stmt);
    PlanNodePtr plan_insert(const popo::parser::InsertStatement & stmt);
    PlanNodePtr plan_create_table(const popo::parser::CreateTableStatement & stmt);
    PlanNodePtr plan_create_index(const popo::parser::CreateIndexStatement & stmt);
    PlanNodePtr plan_update(const popo::parser::UpdateStatement & stmt);
    PlanNodePtr plan_delete(const popo::parser::DeleteStatement & stmt);
    PlanNodePtr plan_drop_table(const popo::parser::DropTableStatement & stmt);
    PlanNodePtr plan_show_tables(const popo::parser::ShowTablesStatement & stmt);
    PlanNodePtr plan_describe(const popo::parser::DescribeStatement & stmt);
    PlanNodePtr plan_show_databases(const popo::parser::ShowDatabasesStatement & stmt);
    PlanNodePtr plan_set_var(const popo::parser::SetVarStatement & stmt);
    PlanNodePtr plan_show_variables(const popo::parser::ShowVariablesStatement & stmt);
    PlanNodePtr plan_show_status(const popo::parser::ShowStatusStatement & stmt);
    PlanNodePtr plan_select_database(const popo::parser::SelectDatabaseStatement & stmt);
    PlanNodePtr plan_select_system_var(const popo::parser::SelectSystemVarStatement & stmt);

    // Build the FROM clause into plan nodes (handles joins)
    PlanNodePtr plan_table_ref(const popo::parser::TableRefPtr & ref);

    // Resolve table columns for scan nodes
    void resolve_scan_columns(ScanNode & scan);
};

} // namespace popo::planner
