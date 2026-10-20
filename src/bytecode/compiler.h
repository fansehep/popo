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

namespace popo::bytecode
{

using popo::base::Error;

class Compiler
{
public:
    explicit Compiler(popo::catalog::SchemaManager & schema_mgr);
    popo::base::Result<Program> compile(const popo::planner::PlanNodePtr & plan);

private:
    popo::catalog::SchemaManager & schema_mgr_;
    Program program_;
    int32_t next_cursor_id_ = 0;
    int32_t next_sorter_id_ = 0;
    std::unordered_map<std::string, int32_t> table_to_cursor_;
    std::unordered_map<int32_t, popo::catalog::SchemaRef> cursor_schemas_;
    std::unordered_map<const popo::parser::Expr *, int32_t> agg_expr_to_index_;
    bool in_agg_step_context_ = false;

    void compile_node(const popo::planner::PlanNodePtr & node);
    void compile_scan(const popo::planner::ScanNode & node);
    void compile_filter(const popo::planner::FilterNode & node);
    void compile_project(const popo::planner::ProjectNode & node);
    void compile_join(const popo::planner::JoinNode & node);
    void compile_agg(const popo::planner::AggNode & node);
    void compile_sort(const popo::planner::SortNode & node);
    void compile_limit(const popo::planner::LimitNode & node);
    void compile_insert(const popo::planner::InsertNode & node);
    void compile_update(const popo::planner::UpdateNode & node);
    void compile_delete(const popo::planner::DeleteNode & node);
    void compile_create_table(const popo::planner::CreateTableNode & node);
    void compile_create_index(const popo::planner::CreateIndexNode & node);
    void compile_drop_table(const popo::planner::DropTableNode & node);
    void compile_show_tables(const popo::planner::ShowTablesNode & node);
    void compile_describe(const popo::planner::DescribeNode & node);
    void compile_show_databases(const popo::planner::ShowDatabasesNode & node);
    void compile_set_var(const popo::planner::SetVarNode & node);
    void compile_show_variables(const popo::planner::ShowVariablesNode & node);
    void compile_show_status(const popo::planner::ShowStatusNode & node);
    void compile_select_database(const popo::planner::SelectDatabaseNode & node);
    void compile_select_system_var(const popo::planner::SelectSystemVarNode & node);
    void compile_expr(const popo::parser::ExprPtr & expr);

    // Detect "pk_column = literal_constant" pattern.
    // Returns {cursor_id, pk_col_idx, literal_value} if matched, nullopt otherwise.
    std::optional<std::tuple<int32_t, int32_t, popo::type::Value>>
    detect_pk_equality(const popo::parser::ExprPtr & where, int32_t cursor_id);

    struct KeyBounds
    {
        std::optional<popo::type::Value> lower;
        bool lower_inclusive = true;
        std::optional<popo::type::Value> upper;
        bool upper_inclusive = true;
    };
    std::optional<KeyBounds> detect_pk_bounds(const popo::parser::ExprPtr & where, int32_t cursor_id);
    // Secondary index equality. pair is (index ordinal, literal).
    std::optional<std::pair<int32_t, popo::type::Value>> detect_index_equality(const popo::parser::ExprPtr & where, int32_t cursor_id);

    int32_t alloc_cursor() { return next_cursor_id_++; }
    int32_t alloc_sorter() { return next_sorter_id_++; }
    std::pair<int32_t, int32_t> resolve_column(const std::string & table_name, const std::string & col_name);
    int32_t find_column_index(int32_t cursor_id, const std::string & col_name);
    void emit_all_columns(int32_t cursor_id);
    void open_table_cursor(const std::string & table_name, bool for_write);

    struct SelectPipeline
    {
        const popo::planner::ScanNode * scan = nullptr;
        const popo::planner::JoinNode * join = nullptr;
        std::vector<const popo::planner::FilterNode *> where_filters;
        const popo::planner::AggNode * agg = nullptr;
        const popo::planner::FilterNode * having = nullptr;
        const popo::planner::SortNode * sort = nullptr;
        const popo::planner::LimitNode * limit = nullptr;
        const popo::planner::ProjectNode * project = nullptr;
    };

    void compile_select(const popo::planner::ProjectNode & project);
    void analyze_select_pipeline(const popo::planner::PlanNodePtr & node, SelectPipeline & pipe);
    void compile_select_pipeline(const SelectPipeline & pipe);
    void compile_select_simple(const SelectPipeline & pipe);
    void compile_select_with_sort(const SelectPipeline & pipe);
    void compile_select_with_agg(const SelectPipeline & pipe);
    void compile_select_with_join(const SelectPipeline & pipe);

    // Resolve project expressions: expand StarExpr, return actual column count and names
    int32_t resolve_project_columns(const std::vector<popo::parser::ExprPtr> & exprs, std::vector<std::string> & out_names);
};

} // namespace popo::bytecode
