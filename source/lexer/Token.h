//
// Created by Vladimir Arsenijevic on 27.5.2026.
//

#ifndef GG_TOKEN_H
#define GG_TOKEN_H

#include <string>

enum class TokenType {
    // Single-character tokens.
    LEFT_PAREN, RIGHT_PAREN, LEFT_BRACE, RIGHT_BRACE,
    LEFT_BRACKET, RIGHT_BRACKET,
    COMMA, DOT, SEMICOLON,
    TILDE, QUESTION, COLON,

    // One or two character tokens.
    BANG, BANG_EQUAL,
    EQUAL, EQUAL_EQUAL,
    PLUS, INCREMENT, PLUS_EQUAL,
    MINUS, DECREMENT, MINUS_EQUAL, ARROW,
    STAR, STAR_EQUAL,
    SLASH, SLASH_EQUAL,
    PERCENT, PERCENT_EQUAL,
    CARET, CARET_EQUAL,
    AMPERSAND, AND, AMPERSAND_EQUAL,
    PIPE, OR, PIPE_EQUAL,
    LESS, LESS_EQUAL, SHIFT_LEFT,
    GREATER, GREATER_EQUAL, SHIFT_RIGHT,

    // Literals.
    IDENTIFIER, STRING, CHAR, NUMBER,

    // Keywords.
    CLASS, ELSE, EXTERN, FALSE, FOR, IF, IMPORT,
    RETURN, BREAK, CONTINUE, SUPER, THIS, TRUE, VAR, WHILE,
    PRIVATE, AS, NEW, SIZEOF,

    // Type keywords — signed integers.
    I8, I16, I32, I64,

    // Type keywords — unsigned integers.
    U8, U16, U32, U64,

    // Type keywords — floating point.
    F32, F64,

    // Type keywords — other.
    BOOL, CHAR_TYPE, VOID, PTR,

    END_OF_FILE
};

[[nodiscard]] TokenType lookupKeyword(const std::string& text);

class Token {
public:
    Token(TokenType tokenType, std::string lexeme, int line);

    const TokenType type;
    const std::string lexeme;
    const int line;
};


#endif //GG_TOKEN_H
