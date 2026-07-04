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
            out("Index");
            indent++;
            printExpr(*indexExpr.object);
            printExpr(*indexExpr.index);
            indent--;
        },

        [&](const IndexAssignExpr& indexAssign) {
            out("IndexAssign");
            indent++;
            out("object:");
            indent++;
            printExpr(*indexAssign.object);
            indent--;
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

        [&](const ThisExpr&) {
            out("This");
        },

        [&](const MemberAccessExpr& ma) {
            out("MemberAccess '." + ma.field.lexeme + "'");
            indent++;
            printExpr(*ma.object);
            indent--;
        },

        [&](const MemberAssignExpr& ma) {
            out("MemberAssign '." + ma.field.lexeme + "'");
            indent++;
            printExpr(*ma.object);
            printExpr(*ma.value);
            indent--;
        },

        [&](const MethodCallExpr& mc) {
            out("MethodCall '." + mc.method.lexeme + "'");
            indent++;
            printExpr(*mc.object);
            for (const auto& arg : mc.args) printExpr(*arg);
            if (mc.args.empty()) out("(no args)");
            indent--;
        },

        [&](const CastExpr& castExpr) {
            out("Cast as '" + castExpr.targetType.lexeme + "'");
            indent++;
            printExpr(*castExpr.operand);
            indent--;
        },

        [&](const NewExpr& newExpr) {
            out("New '" + newExpr.className.lexeme + "'");
            indent++;
            for (const auto& arg : newExpr.args) printExpr(*arg);
            if (newExpr.args.empty()) out("(no args)");
            indent--;
        },

        [&](const SizeofExpr& sizeofExpr) {
            out("Sizeof '" + sizeofExpr.typeName.lexeme + "'");
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

        [&](const ClassDeclStmt& classDecl) {
            out("ClassDecl '" + classDecl.name.lexeme + "'");
            indent++;
            for (const FieldDecl& fd : classDecl.fields) {
                out(std::string(fd.isPublic ? "" : "private ")
                    + "field " + fd.typeName.lexeme + " '" + fd.name.lexeme + "'");
            }
            for (const MethodDecl& md : classDecl.methods) {
                std::string params;
                bool first = true;
                for (const auto& param : md.params) {
                    if (!first) params += ", ";
                    first = false;
                    params += param.typeName.lexeme + " " + param.name.lexeme;
                }
                std::string prefix = std::string(md.isPublic ? "" : "private ")
                                   + (md.isConstructor ? "ctor " : "method ");
                out(prefix + "'" + md.name.lexeme + "' (" + params + ")");
                indent++;
                printBlock(md.body);
                indent--;
            }
            indent--;
        },

        [&](const EnumDeclStmt& enumDecl) {
            out("EnumDecl '" + enumDecl.name.lexeme + "'");
            indent++;
            for (const EnumVariant& v : enumDecl.variants) {
                out("variant '" + v.name.lexeme + "' (" + std::to_string(v.args.size()) + " args)");
                indent++;
                for (const auto& arg : v.args) printExpr(*arg);
                indent--;
            }
            for (const FieldDecl& fd : enumDecl.fields) {
                out(std::string(fd.isPublic ? "" : "private ")
                    + "field " + fd.typeName.lexeme + " '" + fd.name.lexeme + "'");
            }
            for (const MethodDecl& md : enumDecl.methods) {
                std::string params;
                bool first = true;
                for (const auto& param : md.params) {
                    if (!first) params += ", ";
                    first = false;
                    params += param.typeName.lexeme + " " + param.name.lexeme;
                }
                std::string prefix = std::string(md.isPublic ? "" : "private ")
                                   + (md.isConstructor ? "ctor " : "method ");
                out(prefix + "'" + md.name.lexeme + "' (" + params + ")");
                indent++;
                printBlock(md.body);
                indent--;
            }
            indent--;
        },
        [&](const TraitDeclStmt& tr) {
            out("TraitDecl '" + tr.name.lexeme + "'");
            indent++;
            for (const MethodDecl& md : tr.methods)
                out(std::string(md.hasBody ? "default method '" : "required method '")
                    + md.name.lexeme + "'");
            indent--;
        },
        [&](const ImplDeclStmt& impl) {
            out("ImplDecl '" + impl.traitName.lexeme + "' for '" + impl.typeName.lexeme + "'");
            indent++;
            for (const MethodDecl& md : impl.methods) {
                out("method '" + md.name.lexeme + "'");
                indent++;
                printBlock(md.body);
                indent--;
            }
            indent--;
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
