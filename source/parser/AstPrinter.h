//
// Created by Vladimir Arsenijevic on 31.5.2026.
//

#ifndef GG_ASTPRINTER_H
#define GG_ASTPRINTER_H

#include "Ast.h"
#include <ostream>
#include <string>

class AstPrinter {
public:
    void print(const Program& program, std::ostream& stream);

private:
    int           indent_  = 0;
    std::ostream* stream_  = nullptr;

    void printExpr(const Expr& expr);
    void printStmt(const Stmt& stmt);
    void printBlock(const BlockStmt& block);

    void out(const std::string& text);
};

#endif //GG_ASTPRINTER_H
