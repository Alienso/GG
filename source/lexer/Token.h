//
// Created by Vladimir Arsenijevic on 27.5.2026.
//

#ifndef GG_TOKEN_H
#define GG_TOKEN_H

#include <string>

enum class TokenType {
    // Single-character tokens.
    LEFT_PAREN, RIGHT_PAREN, LEFT_BRACE, RIGHT_BRACE,
    COMMA, DOT, MINUS, PLUS, SEMICOLON, SLASH, STAR,

    // One or two character tokens.
    BANG, BANG_EQUAL,
    EQUAL, EQUAL_EQUAL,
    GREATER, GREATER_EQUAL,
    LESS, LESS_EQUAL,

    // Literals.
    IDENTIFIER, STRING, NUMBER,

    // Keywords.
    CLASS, ELSE, FALSE, FOR, IF,
    RETURN, SUPER, THIS, TRUE, VAR, WHILE,

    END_OF_FILE
};

class Token {
public:
    Token(TokenType tokenType, std::string lexeme, int line);

private:
    const TokenType type;
    const std::string lexeme;
    const int line;
};


#endif //GG_TOKEN_H
