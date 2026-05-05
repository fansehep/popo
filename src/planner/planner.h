#pragma once
#include <expected>
#include <utility>
#include "base/error.h"
#include "catalog/schema_mgr.h"
#include "parser/ast.h"
#include "planner/plan_node.h"

namespace neon::planner
{

class Planner
{
public:
    explicit Planner(neon::catalog::SchemaManager & schema_mgr);

    neon::base::Result<PlanNodePtr> plan(const neon::parser::StmtPtr & stmt);

private:
    neon::catalog::SchemaManager & schema_mgr_;

    PlanNodePtr plan_select(const neon::parser::SelectStatement & stmt);
    PlanNodePtr plan_insert(const neon::parser::InsertStatement & stmt);
    PlanNodePtr plan_create_table(const neon::parser::CreateTableStatement & stmt);
    PlanNodePtr plan_update(const neon::parser::UpdateStatement & stmt);
    PlanNodePtr plan_delete(const neon::parser::DeleteStatement & stmt);
    PlanNodePtr plan_drop_table(const neon::parser::DropTableStatement & stmt);
    PlanNodePtr plan_show_tables(const neon::parser::ShowTablesStatement & stmt);
    PlanNodePtr plan_describe(const neon::parser::DescribeStatement & stmt);
    PlanNodePtr plan_show_databases(const neon::parser::ShowDatabasesStatement & stmt);
    PlanNodePtr plan_set_var(const neon::parser::SetVarStatement & stmt);
    PlanNodePtr plan_show_variables(const neon::parser::ShowVariablesStatement & stmt);
    PlanNodePtr plan_show_status(const neon::parser::ShowStatusStatement & stmt);
    PlanNodePtr plan_select_database(const neon::parser::SelectDatabaseStatement & stmt);
    PlanNodePtr plan_select_system_var(const neon::parser::SelectSystemVarStatement & stmt);

    // Build the FROM clause into plan nodes (handles joins)
    PlanNodePtr plan_table_ref(const neon::parser::TableRefPtr & ref);

    // Resolve table columns for scan nodes
    void resolve_scan_columns(ScanNode & scan);
};

} // namespace neon::planner
