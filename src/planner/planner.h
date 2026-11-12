#pragma once
#include <expected>
#include <unordered_map>
#include <unordered_set>
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
    std::unordered_map<std::string, PlanNodePtr> cte_plans_;
    std::unordered_map<std::string, bool> cte_recursive_;
    std::unordered_map<std::string, std::vector<std::string>> cte_columns_;
    std::unordered_set<std::string> expanding_views_;

    popo::base::Result<PlanNodePtr> plan_select(const popo::parser::SelectStatement & stmt);
    popo::base::Result<PlanNodePtr> plan_select_body(const popo::parser::SelectStatement & stmt);
    popo::base::Result<PlanNodePtr> plan_alter_table(const popo::parser::AlterTableStatement & stmt);
    popo::base::Result<PlanNodePtr> plan_insert(const popo::parser::InsertStatement & stmt);
    popo::base::Result<PlanNodePtr> plan_create_table(const popo::parser::CreateTableStatement & stmt);
    popo::base::Result<PlanNodePtr> plan_create_index(const popo::parser::CreateIndexStatement & stmt);
    popo::base::Result<PlanNodePtr> plan_update(const popo::parser::UpdateStatement & stmt);
    popo::base::Result<PlanNodePtr> plan_delete(const popo::parser::DeleteStatement & stmt);
    popo::base::Result<PlanNodePtr> plan_drop_table(const popo::parser::DropTableStatement & stmt);
    popo::base::Result<PlanNodePtr> plan_show_tables(const popo::parser::ShowTablesStatement & stmt);
    popo::base::Result<PlanNodePtr> plan_describe(const popo::parser::DescribeStatement & stmt);
    popo::base::Result<PlanNodePtr> plan_show_databases(const popo::parser::ShowDatabasesStatement & stmt);
    popo::base::Result<PlanNodePtr> plan_set_var(const popo::parser::SetVarStatement & stmt);
    popo::base::Result<PlanNodePtr> plan_show_variables(const popo::parser::ShowVariablesStatement & stmt);
    popo::base::Result<PlanNodePtr> plan_show_status(const popo::parser::ShowStatusStatement & stmt);
    popo::base::Result<PlanNodePtr> plan_select_database(const popo::parser::SelectDatabaseStatement & stmt);
    popo::base::Result<PlanNodePtr> plan_select_system_var(const popo::parser::SelectSystemVarStatement & stmt);
    popo::base::Result<PlanNodePtr> plan_create_view(const popo::parser::CreateViewStatement & stmt);
    popo::base::Result<PlanNodePtr> plan_drop_view(const popo::parser::DropViewStatement & stmt);

    popo::base::Result<PlanNodePtr> plan_table_ref(const popo::parser::TableRefPtr & ref);

    popo::base::Result<void> resolve_scan_columns(ScanNode & scan);

    static std::vector<std::string> plan_output_names(const PlanNodePtr & node);
};

} // namespace popo::planner
