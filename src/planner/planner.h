#pragma once
#include "planner/plan_node.h"
#include "parser/ast.h"
#include "catalog/schema_mgr.h"
#include "base/error.h"
#include <utility>

namespace neon::planner {

class Planner {
public:
    explicit Planner(neon::catalog::SchemaManager& schema_mgr);
    
    std::pair<PlanNodePtr, neon::base::Error> plan(const neon::parser::StmtPtr& stmt);

private:
    neon::catalog::SchemaManager& schema_mgr_;
    
    PlanNodePtr plan_select(const neon::parser::SelectStatement& stmt);
    PlanNodePtr plan_insert(const neon::parser::InsertStatement& stmt);
    PlanNodePtr plan_create_table(const neon::parser::CreateTableStatement& stmt);
    PlanNodePtr plan_update(const neon::parser::UpdateStatement& stmt);
    PlanNodePtr plan_delete(const neon::parser::DeleteStatement& stmt);
    PlanNodePtr plan_drop_table(const neon::parser::DropTableStatement& stmt);
    
    // Build the FROM clause into plan nodes (handles joins)
    PlanNodePtr plan_table_ref(const neon::parser::TableRefPtr& ref);
    
    // Resolve table columns for scan nodes
    void resolve_scan_columns(ScanNode& scan);
};

} // namespace neon::planner
