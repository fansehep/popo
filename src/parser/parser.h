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
    // Parse a SQL string, returns a list of statements
    popo::base::Result<std::vector<StmtPtr>> parse(const std::string & sql);

private:
    std::vector<Token> tokens_;
    size_t pos_ = 0;

    // Token access
    const Token & current() const;
    const Token & peek_token() const; // look ahead
    const Token & advance_token();
    bool check(TokenType type) const;
    bool match(TokenType type); // consume if match
    Token expect(TokenType type); // consume or throw
    bool is_at_end() const;

    // Statement parsing
    StmtPtr parse_statement();
    StmtPtr parse_select();
    StmtPtr parse_insert();
    StmtPtr parse_create_table();
    StmtPtr parse_create_index(bool unique);
    StmtPtr parse_update();
    StmtPtr parse_delete();
    StmtPtr parse_drop();
    StmtPtr parse_show();
    StmtPtr parse_describe();
    StmtPtr parse_set();
    StmtPtr parse_select_database();
    StmtPtr parse_select_system_var();

    // SELECT clause parsing
    std::vector<ExprPtr> parse_select_list();
    TableRefPtr parse_table_ref();
    TableRefPtr parse_join(TableRefPtr left);
    ExprPtr parse_where();
    std::vector<ExprPtr> parse_group_by();
    ExprPtr parse_having();
    std::vector<OrderByItem> parse_order_by();
    std::optional<int64_t> parse_limit();

    // Expression parsing (precedence climbing)
    ExprPtr parse_expr();
    ExprPtr parse_or_expr();
    ExprPtr parse_and_expr();
    ExprPtr parse_not_expr();
    ExprPtr parse_comparison();
    ExprPtr parse_addition();
    ExprPtr parse_multiplication();
    ExprPtr parse_unary();
    ExprPtr parse_primary();
    ExprPtr parse_function_call(const std::string & name);

    // Type parsing for CREATE TABLE
    ColumnDef parse_column_def();
    popo::type::ValueType parse_data_type(int32_t & type_len);
};

} // namespace popo::parser
