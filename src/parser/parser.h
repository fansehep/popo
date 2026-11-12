#pragma once

#include <expected>
#include <string>
#include <utility>
#include <vector>
#include "base/error.h"
#include "parser/ast.h"
#include "parser/lexer.h"

namespace popo::parser
{

class Parser
{
public:
    popo::base::Result<std::vector<StmtPtr>> parse(const std::string & sql);

private:
    std::vector<Token> tokens_;
    size_t pos_ = 0;

    // Token access
    const Token & current() const;
    const Token & peek_token() const;
    const Token & advance_token();
    bool check(TokenType type) const;
    bool match(TokenType type);
    popo::base::Result<Token> expect(TokenType type);
    bool is_at_end() const;

    // Statement parsing
    popo::base::Result<StmtPtr> parse_statement();
    popo::base::Result<StmtPtr> parse_select();
    popo::base::Result<void> parse_select_core(const std::shared_ptr<SelectStatement> & stmt);
    popo::base::Result<std::vector<CteDef>> parse_with_clause();
    popo::base::Result<StmtPtr> parse_alter_table();
    popo::base::Result<StmtPtr> parse_insert();
    popo::base::Result<StmtPtr> parse_create_table();
    popo::base::Result<StmtPtr> parse_create_index(bool unique);
    popo::base::Result<StmtPtr> parse_update();
    popo::base::Result<StmtPtr> parse_delete();
    popo::base::Result<StmtPtr> parse_drop();
    popo::base::Result<StmtPtr> parse_create_view(bool or_replace = false);
    popo::base::Result<StmtPtr> parse_show();
    popo::base::Result<StmtPtr> parse_describe();
    popo::base::Result<StmtPtr> parse_set();
    popo::base::Result<StmtPtr> parse_set_assignment();
    popo::base::Result<StmtPtr> parse_begin();
    popo::base::Result<StmtPtr> parse_start_transaction();
    popo::base::Result<StmtPtr> parse_commit();
    popo::base::Result<StmtPtr> parse_rollback();
    popo::base::Result<IsolationLevel> parse_isolation_level();
    // Transaction keywords stay non-reserved so they remain usable as identifiers.
    bool check_keyword(const char * word) const;
    bool match_keyword(const char * word);
    popo::base::Result<void> expect_keyword(const char * word);
    popo::base::Result<StmtPtr> parse_create_user();
    popo::base::Result<StmtPtr> parse_alter_user();
    popo::base::Result<StmtPtr> parse_set_password();
    popo::base::Result<StmtPtr> parse_grant();
    popo::base::Result<StmtPtr> parse_revoke();
    popo::base::Result<uint32_t> parse_privilege_list();
    popo::base::Result<void> parse_privilege_level(std::string & database, std::string & table);
    popo::base::Result<AccountName> parse_account_name();
    popo::base::Result<std::string> parse_user_token();
    popo::base::Result<StmtPtr> parse_select_database();
    popo::base::Result<StmtPtr> parse_select_system_var();

    // SELECT clause parsing
    popo::base::Result<std::vector<ExprPtr>> parse_select_list();
    popo::base::Result<TableRefPtr> parse_table_ref();
    popo::base::Result<TableRefPtr> parse_table_primary();
    popo::base::Result<TableRefPtr> parse_join(TableRefPtr left);
    popo::base::Result<ExprPtr> parse_where();
    popo::base::Result<std::vector<ExprPtr>> parse_group_by();
    popo::base::Result<ExprPtr> parse_having();
    popo::base::Result<std::vector<OrderByItem>> parse_order_by();
    popo::base::Result<int64_t> parse_limit();

    // Expression parsing (precedence climbing)
    popo::base::Result<ExprPtr> parse_expr();
    popo::base::Result<ExprPtr> parse_or_expr();
    popo::base::Result<ExprPtr> parse_and_expr();
    popo::base::Result<ExprPtr> parse_not_expr();
    popo::base::Result<ExprPtr> parse_comparison();
    popo::base::Result<ExprPtr> parse_addition();
    popo::base::Result<ExprPtr> parse_multiplication();
    popo::base::Result<ExprPtr> parse_unary();
    popo::base::Result<ExprPtr> parse_primary();
    popo::base::Result<ExprPtr> parse_case_expr();
    popo::base::Result<ExprPtr> parse_function_call(const std::string & name);
    popo::base::Result<ExprPtr> maybe_window(ExprPtr func);
    popo::base::Result<ExprPtr> parse_comparison_rhs(BinaryOpType op, const ExprPtr & left);
    popo::base::Result<ExprPtr> parse_interval_expr();
    popo::base::Result<popo::type::ValueType> parse_cast_target();
    popo::base::Result<void> skip_parenthesized();
    popo::base::Result<void> parse_foreign_key_tail();

    // Type parsing for CREATE TABLE
    popo::base::Result<ColumnDef> parse_column_def();
    popo::base::Result<popo::type::ValueType> parse_data_type(int32_t & type_len);
};

} // namespace popo::parser
