//
// Created by Vladimir Arsenijevic on 27.5.2026.
//

#include "Token.h"

#include <utility>

Token::Token(TokenType tokenType, std::string lexeme, int line) : type(tokenType), lexeme(std::move(lexeme)), line(line) {

}
