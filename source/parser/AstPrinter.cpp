//
// Created by Vladimir Arsenijevic on 31.5.2026.
//

#include "AstPrinter.h"
#include <iostream>

void AstPrinter::out(const std::string& text) {
    std::cout << std::string(indent_ * 2, ' ') << text << '\n';
}

void AstPrinter::print(const Program& program) {
    out("Program");
    indent_++;
    for (const Stmt& stmt : program.declarations) {
        printStmt(stmt);
    }
    indent_--;
}

void AstPrinter::printExpr(const Expr& expr) {
    std::visit(overloaded{

        [&](const LiteralExpr& e) {
            out("Literal '" + e.token.lexeme + "'");
        },

        [&](const IdentifierExpr& e) {
            out("Identifier '" + e.name.lexeme + "'");
        },

        [&](const UnaryExpr& e) {
            out("Unary '" + e.op.lexeme + "'");
            indent_++;
            printExpr(*e.operand);
            indent_--;
        },

        [&](const BinaryExpr& e) {
            out("Binary '" + e.op.lexeme + "'");
            indent_++;
            printExpr(*e.left);
            printExpr(*e.right);
            indent_--;
        },

        [&](const AssignExpr& e) {
            out("Assign '" + e.name.lexeme + "'");
            indent_++;
            printExpr(*e.value);
            indent_--;
        },

        [&](const CompoundAssignExpr& e) {
            out("CompoundAssign '" + e.name.lexeme + "' '" + e.op.lexeme + "'");
            indent_++;
            printExpr(*e.value);
            indent_--;
        },

        [&](const PostfixExpr& e) {
            out("Postfix '" + e.op.lexeme + "'");
            indent_++;
            printExpr(*e.operand);
            indent_--;
        },

        [&](const CallExpr& e) {
            out("Call '" + e.callee.lexeme + "'");
            indent_++;
            for (const auto& arg : e.args) {
                printExpr(*arg);
            }
            if (e.args.empty()) out("(no args)");
            indent_--;
        },

        [&](const VarDeclExpr& e) {
            out("VarDecl " + e.typeName.lexeme + " '" + e.name.lexeme + "'");
            indent_++;
            if (e.initializer) printExpr(*e.initializer);
            else               out("(no initializer)");
            indent_--;
        },

    }, *expr.node);
}

void AstPrinter::printStmt(const Stmt& stmt) {
    std::visit(overloaded{

        [&](const ExprStmt& s) {
            out("ExprStmt");
            indent_++;
            printExpr(s.expression);
            indent_--;
        },

        [&](const BlockStmt& s) {
            printBlock(s);
        },

        [&](const IfStmt& s) {
            out("If");
            indent_++;
            out("condition:");
            indent_++;
            printExpr(s.condition);
            indent_--;
            out("then:");
            indent_++;
            printStmt(*s.thenBranch);
            indent_--;
            if (s.elseBranch) {
                out("else:");
                indent_++;
                printStmt(*s.elseBranch);
                indent_--;
            }
            indent_--;
        },

        [&](const WhileStmt& s) {
            out("While");
            indent_++;
            out("condition:");
            indent_++;
            printExpr(s.condition);
            indent_--;
            out("body:");
            indent_++;
            printStmt(*s.body);
            indent_--;
            indent_--;
        },

        [&](const ForStmt& s) {
            out("For");
            indent_++;
            out("init:");
            indent_++;
            if (s.init) printStmt(*s.init);
            else        out("(empty)");
            indent_--;
            out("condition:");
            indent_++;
            if (s.condition) printExpr(*s.condition);
            else             out("(empty)");
            indent_--;
            out("increment:");
            indent_++;
            if (s.increment) printExpr(*s.increment);
            else             out("(empty)");
            indent_--;
            out("body:");
            indent_++;
            printStmt(*s.body);
            indent_--;
            indent_--;
        },

        [&](const ReturnStmt& s) {
            out("Return");
            if (s.value) {
                indent_++;
                printExpr(*s.value);
                indent_--;
            }
        },

        [&](const FunctionDeclStmt& s) {
            std::string params;
            for (size_t i = 0; i < s.params.size(); i++) {
                if (i > 0) params += ", ";
                params += s.params[i].typeName.lexeme + " " + s.params[i].name.lexeme;
            }
            out("FunctionDecl " + s.returnType.lexeme + " '" + s.name.lexeme + "' (" + params + ")");
            indent_++;
            printBlock(s.body);
            indent_--;
        },

    }, *stmt.node);
}

void AstPrinter::printBlock(const BlockStmt& block) {
    out("Block");
    indent_++;
    for (const auto& stmt : block.body) {
        printStmt(*stmt);
    }
    if (block.body.empty()) out("(empty)");
    indent_--;
}
