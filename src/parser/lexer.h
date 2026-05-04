#pragma once

#include <string>
#include <vector>
#include <cstdint>

namespace neon::parser {

enum class TokenType {
    // Literals
    Integer,
    Float,
    String,

    // Identifiers & Keywords
    Identifier,

    // Keywords (SQL)
    Select,
    From,
    Where,
    Insert,
    Into,
    Values,
    Create,
    Table,
    Drop,
    Update,
    Set,
    Delete,
    Alter,
    And,
    Or,
    Not,
    Null,
    True,
    False,
    Join,
    Inner,
    Left,
    Right,
    Cross,
    On,
    Group,
    By,
    Having,
    Order,
    Asc,
    Desc,
    Limit,
    As,
    Primary,
    Key,
    Int,
    Integer_,
    Bigint,
    Smallint,
    Tinyint,
    Float_,
    Double,
    Varchar,
    Char,
    Text,
    Boolean,
    Bool_,
    Unique,
    Default,
    If,
    Exists,
    Count,
    Sum,
    Avg,
    Min,
    Max,
    Distinct,

    // Operators
    Plus,
    Minus,
    Star,
    Slash,
    Eq,
    Ne,
    Lt,
    Le,
    Gt,
    Ge,

    // Delimiters
    LParen,
    RParen,
    Comma,
    Semicolon,
    Dot,

    // Special
    Eof,
    Invalid
};

struct Token {
    TokenType type;
    std::string value;
    int line;
    int col;
};

class Lexer {
public:
    explicit Lexer(const std::string& input);
    std::vector<Token> tokenize();

private:
    std::string input_;
    size_t pos_ = 0;
    int line_ = 1;
    int col_ = 1;

    [[nodiscard]] char peek() const;
    char advance();
    void skip_whitespace();
    void skip_line_comment();
    void skip_block_comment();
    Token read_number();
    Token read_string();
    Token read_identifier_or_keyword();
    TokenType keyword_lookup(const std::string& word);
};

} // namespace neon::parser
