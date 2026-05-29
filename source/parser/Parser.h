//
// Created by Vladimir Arsenijevic on 28.5.2026.
//

#ifndef GG_PARSER_H
#define GG_PARSER_H


#include "../lexer/Token.h"

#include <vector>

class Parser {
public:
    Parser();

    void parse(std::vector<Token> &tokens);
};


#endif //GG_PARSER_H
