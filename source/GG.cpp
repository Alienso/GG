//
// Created by Vladimir Arsenijevic on 27.5.2026.
//

#include "GG.h"


GG::GG(std::vector<std::string>& paths) : lexer(paths) {

}

void GG::run() {
    lexer.lex();
}
