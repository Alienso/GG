//
// Created by Vladimir Arsenijevic on 27.5.2026.
//

#ifndef GG_LEXER_H
#define GG_LEXER_H

#include <fstream>
#include <vector>
#include "Token.h"

class Lexer {
public:
    explicit Lexer(std::vector<std::string> &paths);

    void lex();

private:
    std::vector<std::ifstream> inputFiles{};
    std::vector<std::vector<Token>> tokensForFiles{};

    static void processFile(std::ifstream &file, std::vector<Token>& tokens);
};


#endif //GG_LEXER_H
