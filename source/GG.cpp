//
// Created by Vladimir Arsenijevic on 27.5.2026.
//

#include "GG.h"
#include "parser/AstPrinter.h"


GG::GG(std::vector<std::string>& paths) : lexer(paths) {

}

void GG::run() {
    lexer.lex();
    AstPrinter printer;
    for (const auto& fileTokens : lexer.tokensForFiles) {
        Program ast = parser.parse(fileTokens);
        printer.print(ast);
    }
}
