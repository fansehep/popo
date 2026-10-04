#pragma once
#include <optional>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>
#include "base/error.h"
#include "bytecode/program.h"
#include "catalog/schema_mgr.h"
#include "planner/plan_node.h"

namespace neon::bytecode
{

using neon::base::Error;

class Compiler
{
public:
    explicit Compiler(neon::catalog::SchemaManager & schema_mgr);
    neon::base::Result<Program> compile(const neon::planner::PlanNodePtr & plan);

private:
    neon::catalog::SchemaManager & schema_mgr_;
    Program program_;
    int32_t next_cursor_id_ = 0;
    int32_t next_sorter_id_ = 0;
    std::unordered_map<std::string, int32_t> table_to_cursor_;
    std::unordered_map<int32_t, neon::catalog::SchemaRef> cursor_schemas_;
    std::unordered_map<const neon::parser::Expr *, int32_t> agg_expr_to_index_;
    bool in_agg_step_context_ = false;

    void compile_node(const neon::planner::PlanNodePtr & node);
    void compile_scan(const neon::planner::ScanNode & node);
    void compile_filter(const neon::planner::FilterNode & node);
    void compile_project(const neon::planner::ProjectNode & node);
    void compile_join(const neon::planner::JoinNode & node);
    void compile_agg(const neon::planner::AggNode & node);
    void compile_sort(const neon::planner::SortNode & node);
    void compile_limit(const neon::planner::LimitNode & node);
    void compile_insert(const neon::planner::InsertNode & node);
    void compile_update(const neon::planner::UpdateNode & node);
    void compile_delete(const neon::planner::DeleteNode & node);
    void compile_create_table(const neon::planner::CreateTableNode & node);
    void compile_create_index(const neon::planner::CreateIndexNode & node);
    void compile_drop_table(const neon::planner::DropTableNode & node);
    void compile_show_tables(const neon::planner::ShowTablesNode & node);
    void compile_describe(const neon::planner::DescribeNode & node);
    void compile_show_databases(const neon::planner::ShowDatabasesNode & node);
    void compile_set_var(const neon::planner::SetVarNode & node);
    void compile_show_variables(const neon::planner::ShowVariablesNode & node);
    void compile_show_status(const neon::planner::ShowStatusNode & node);
    void compile_select_database(const neon::planner::SelectDatabaseNode & node);
    void compile_select_system_var(const neon::planner::SelectSystemVarNode & node);
    void compile_expr(const neon::parser::ExprPtr & expr);

    // Detect "pk_column = literal_constant" pattern.
    // Returns {cursor_id, pk_col_idx, literal_value} if matched, nullopt otherwise.
    std::optional<std::tuple<int32_t, int32_t, neon::type::Value>>
    detect_pk_equality(const neon::parser::ExprPtr & where, int32_t cursor_id);

    struct KeyBounds
    {
        std::optional<neon::type::Value> lower;
        bool lower_inclusive = true;
        std::optional<neon::type::Value> upper;
        bool upper_inclusive = true;
    };
    std::optional<KeyBounds> detect_pk_bounds(const neon::parser::ExprPtr & where, int32_t cursor_id);
    // Secondary index equality. pair is (index ordinal, literal).
    std::optional<std::pair<int32_t, neon::type::Value>> detect_index_equality(const neon::parser::ExprPtr & where, int32_t cursor_id);

    int32_t alloc_cursor() { return next_cursor_id_++; }
    int32_t alloc_sorter() { return next_sorter_id_++; }
    std::pair<int32_t, int32_t> resolve_column(const std::string & table_name, const std::string & col_name);
    int32_t find_column_index(int32_t cursor_id, const std::string & col_name);
    void emit_all_columns(int32_t cursor_id);
    void open_table_cursor(const std::string & table_name, bool for_write);

    struct SelectPipeline
    {
        const neon::planner::ScanNode * scan = nullptr;
        const neon::planner::JoinNode * join = nullptr;
        std::vector<const neon::planner::FilterNode *> where_filters;
        const neon::planner::AggNode * agg = nullptr;
        const neon::planner::FilterNode * having = nullptr;
        const neon::planner::SortNode * sort = nullptr;
        const neon::planner::LimitNode * limit = nullptr;
        const neon::planner::ProjectNode * project = nullptr;
    };

    void compile_select(const neon::planner::ProjectNode & project);
    void analyze_select_pipeline(const neon::planner::PlanNodePtr & node, SelectPipeline & pipe);
    void compile_select_pipeline(const SelectPipeline & pipe);
    void compile_select_simple(const SelectPipeline & pipe);
    void compile_select_with_sort(const SelectPipeline & pipe);
    void compile_select_with_agg(const SelectPipeline & pipe);
    void compile_select_with_join(const SelectPipeline & pipe);

    // Resolve project expressions: expand StarExpr, return actual column count and names
    int32_t resolve_project_columns(const std::vector<neon::parser::ExprPtr> & exprs, std::vector<std::string> & out_names);
};

} // namespace neon::bytecode
