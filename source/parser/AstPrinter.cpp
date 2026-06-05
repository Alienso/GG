//
// Created by Vladimir Arsenijevic on 31.5.2026.
//

#include "AstPrinter.h"

void AstPrinter::out(const std::string& text) {
    *stream << std::string(indent * 2, ' ') << text << '\n';
}

void AstPrinter::print(const Program& program, std::ostream& outputStream) {
    stream = &outputStream;
    indent = 0;
    out("Program");
    indent++;
    for (const Stmt& stmt : program.declarations)
        printStmt(stmt);
    indent--;
}

void AstPrinter::printExpr(const Expr& expr) {
    std::visit(overloaded{

        [&](const LiteralExpr& literal) {
            out("Literal '" + literal.token.lexeme + "'");
        },

        [&](const IdentifierExpr& identifier) {
            out("Identifier '" + identifier.name.lexeme + "'");
        },

        [&](const UnaryExpr& unary) {
            out("Unary '" + unary.operatorToken.lexeme + "'");
            indent++;
            printExpr(*unary.operand);
            indent--;
        },

        [&](const BinaryExpr& binary) {
            out("Binary '" + binary.operatorToken.lexeme + "'");
            indent++;
            printExpr(*binary.left);
            printExpr(*binary.right);
            indent--;
        },

        [&](const AssignExpr& assign) {
            out("Assign '" + assign.name.lexeme + "'");
            indent++;
            printExpr(*assign.value);
            indent--;
        },

        [&](const CompoundAssignExpr& compoundAssign) {
            out("CompoundAssign '" + compoundAssign.name.lexeme + "' '" + compoundAssign.operatorToken.lexeme + "'");
            indent++;
            printExpr(*compoundAssign.value);
            indent--;
        },

        [&](const PostfixExpr& postfix) {
            out("Postfix '" + postfix.operatorToken.lexeme + "'");
            indent++;
            printExpr(*postfix.operand);
            indent--;
        },

        [&](const CallExpr& call) {
            out("Call '" + call.callee.lexeme + "'");
            indent++;
            for (const auto& arg : call.args)
                printExpr(*arg);
            if (call.args.empty())
                out("(no args)");
            indent--;
        },

        [&](const VarDeclExpr& varDecl) {
            std::string typeStr = varDecl.arraySize > 0
                ? varDecl.typeName.lexeme + "[" + std::to_string(varDecl.arraySize) + "]"
                : varDecl.typeName.lexeme;
            out("VarDecl " + typeStr + " '" + varDecl.name.lexeme + "'");
            indent++;
            if (varDecl.initializer) printExpr(*varDecl.initializer);
            else                     out("(no initializer)");
            indent--;
        },

        [&](const IndexExpr& indexExpr) {
            out("Index '" + indexExpr.name.lexeme + "'");
            indent++;
            printExpr(*indexExpr.index);
            indent--;
        },

        [&](const IndexAssignExpr& indexAssign) {
            out("IndexAssign '" + indexAssign.name.lexeme + "'");
            indent++;
            out("index:");
            indent++;
            printExpr(*indexAssign.index);
            indent--;
            out("value:");
            indent++;
            printExpr(*indexAssign.value);
            indent--;
            indent--;
        },

    }, *expr.node);
}

void AstPrinter::printStmt(const Stmt& stmt) {
    std::visit(overloaded{

        [&](const ExprStmt& exprStmt) {
            out("ExprStmt");
            indent++;
            printExpr(exprStmt.expression);
            indent--;
        },

        [&](const BlockStmt& blockStmt) {
            printBlock(blockStmt);
        },

        [&](const IfStmt& ifStmt) {
            out("If");
            indent++;
            out("condition:");
            indent++;
            printExpr(ifStmt.condition);
            indent--;
            out("then:");
            indent++;
            printStmt(*ifStmt.thenBranch);
            indent--;
            if (ifStmt.elseBranch) {
                out("else:");
                indent++;
                printStmt(*ifStmt.elseBranch);
                indent--;
            }
            indent--;
        },

        [&](const WhileStmt& whileStmt) {
            out("While");
            indent++;
            out("condition:");
            indent++;
            printExpr(whileStmt.condition);
            indent--;
            out("body:");
            indent++;
            printStmt(*whileStmt.body);
            indent--;
            indent--;
        },

        [&](const ForStmt& forStmt) {
            out("For");
            indent++;
            out("init:");
            indent++;
            if (forStmt.init) printStmt(*forStmt.init);
            else              out("(empty)");
            indent--;
            out("condition:");
            indent++;
            if (forStmt.condition) printExpr(*forStmt.condition);
            else                   out("(empty)");
            indent--;
            out("increment:");
            indent++;
            if (forStmt.increment) printExpr(*forStmt.increment);
            else                   out("(empty)");
            indent--;
            out("body:");
            indent++;
            printStmt(*forStmt.body);
            indent--;
            indent--;
        },

        [&](const BreakStmt&) {
            out("Break");
        },

        [&](const ContinueStmt&) {
            out("Continue");
        },

        [&](const ReturnStmt& returnStmt) {
            out("Return");
            if (returnStmt.value) {
                indent++;
                printExpr(*returnStmt.value);
                indent--;
            }
        },

        [&](const FunctionDeclStmt& functionDecl) {
            std::string params;
            bool first = true;
            for (const auto& param : functionDecl.params) {
                if (!first) params += ", ";
                first = false;
                params += param.typeName.lexeme + " " + param.name.lexeme;
            }
            out("FunctionDecl " + functionDecl.returnType.lexeme + " '" + functionDecl.name.lexeme + "' (" + params + ")");
            indent++;
            printBlock(functionDecl.body);
            indent--;
        },

        [&](const ImportStmt& importStmt) {
            out("Import " + importStmt.path.lexeme);
        },

        [&](const ExternFuncDeclStmt& externDecl) {
            std::string params;
            bool first = true;
            for (const auto& param : externDecl.params) {
                if (!first) params += ", ";
                first = false;
                params += param.typeName.lexeme + " " + param.name.lexeme;
            }
            out("ExternFuncDecl " + externDecl.returnType.lexeme + " '" + externDecl.name.lexeme + "' (" + params + ")");
        },

    }, *stmt.node);
}

void AstPrinter::printBlock(const BlockStmt& block) {
    out("Block");
    indent++;
    for (const auto& statement : block.body)
        printStmt(*statement);
    if (block.body.empty()) out("(empty)");
    indent--;
}
