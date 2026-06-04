#include <catch2/catch_test_macros.hpp>
#include "helpers.h"

// ============================================================
// Function declarations
// ============================================================

TEST_CASE("Parser - minimal function declaration", "[parser]") {
    Program p = parseString("i32 main() { return 0; }");

    REQUIRE(p.declarations.size() == 1);
    const auto& f = asStmt<FunctionDeclStmt>(p.declarations[0]);

    REQUIRE(f.name.lexeme       == "main");
    REQUIRE(f.returnType.type   == TokenType::I32);
    REQUIRE(f.params.empty());
    REQUIRE(f.body.body.size()  == 1);
}

TEST_CASE("Parser - function with parameters", "[parser]") {
    Program p = parseString("i32 add(i32 a, i32 b) { return 0; }");
    const auto& f = asStmt<FunctionDeclStmt>(p.declarations[0]);

    REQUIRE(f.params.size()        == 2);
    REQUIRE(f.params[0].name.lexeme == "a");
    REQUIRE(f.params[0].typeName.type == TokenType::I32);
    REQUIRE(f.params[1].name.lexeme == "b");
}

// ============================================================
// Variable declarations
// ============================================================

TEST_CASE("Parser - variable declaration without initializer", "[parser]") {
    Program p = parseString("i32 main() { i32 x; }");
    const auto& f    = asStmt<FunctionDeclStmt>(p.declarations[0]);
    const auto& stmt = asStmt<ExprStmt>(*f.body.body[0]);
    const auto& decl = asExpr<VarDeclExpr>(stmt.expression);

    REQUIRE(decl.typeName.type == TokenType::I32);
    REQUIRE(decl.name.lexeme   == "x");
    REQUIRE(decl.initializer   == nullptr);
}

TEST_CASE("Parser - variable declaration with initializer", "[parser]") {
    Program p = parseString("i32 main() { i32 x = 42; }");
    const auto& f    = asStmt<FunctionDeclStmt>(p.declarations[0]);
    const auto& stmt = asStmt<ExprStmt>(*f.body.body[0]);
    const auto& decl = asExpr<VarDeclExpr>(stmt.expression);

    REQUIRE(decl.name.lexeme != "");
    REQUIRE(decl.initializer != nullptr);

    const auto& lit = asExpr<LiteralExpr>(*decl.initializer);
    REQUIRE(lit.token.lexeme == "42");
}

// ============================================================
// Control flow
// ============================================================

TEST_CASE("Parser - if/else structure", "[parser]") {
    Program p = parseString("i32 main() { if (x) { } else { } }");
    const auto& f    = asStmt<FunctionDeclStmt>(p.declarations[0]);
    const auto& stmt = asStmt<IfStmt>(*f.body.body[0]);

    // Condition is an identifier
    const auto& cond = asExpr<IdentifierExpr>(stmt.condition);
    REQUIRE(cond.name.lexeme == "x");

    REQUIRE(stmt.thenBranch != nullptr);
    REQUIRE(stmt.elseBranch != nullptr);
}

TEST_CASE("Parser - while loop structure", "[parser]") {
    Program p = parseString("i32 main() { while (1) { } }");
    const auto& f    = asStmt<FunctionDeclStmt>(p.declarations[0]);
    const auto& stmt = asStmt<WhileStmt>(*f.body.body[0]);

    const auto& cond = asExpr<LiteralExpr>(stmt.condition);
    REQUIRE(cond.token.lexeme == "1");
    REQUIRE(stmt.body != nullptr);
}

TEST_CASE("Parser - for loop with all clauses", "[parser]") {
    Program p = parseString("i32 main() { for (i32 i = 0; i < 10; i++) { } }");
    const auto& f    = asStmt<FunctionDeclStmt>(p.declarations[0]);
    const auto& stmt = asStmt<ForStmt>(*f.body.body[0]);

    // Init is an ExprStmt wrapping a VarDeclExpr
    REQUIRE(stmt.init != nullptr);
    const auto& initStmt = asStmt<ExprStmt>(*stmt.init);
    const auto& initDecl = asExpr<VarDeclExpr>(initStmt.expression);
    REQUIRE(initDecl.name.lexeme == "i");

    REQUIRE(stmt.condition.has_value());
    REQUIRE(stmt.increment.has_value());
}

// ============================================================
// Expressions
// ============================================================

TEST_CASE("Parser - return statement with literal", "[parser]") {
    Program p = parseString("i32 main() { return 7; }");
    const auto& f    = asStmt<FunctionDeclStmt>(p.declarations[0]);
    const auto& stmt = asStmt<ReturnStmt>(*f.body.body[0]);

    REQUIRE(stmt.value.has_value());
    const auto& lit = asExpr<LiteralExpr>(*stmt.value);
    REQUIRE(lit.token.lexeme == "7");
}

TEST_CASE("Parser - binary expression operator is captured", "[parser]") {
    Program p = parseString("i32 main() { i32 x = 1 + 2; }");
    const auto& f    = asStmt<FunctionDeclStmt>(p.declarations[0]);
    const auto& stmt = asStmt<ExprStmt>(*f.body.body[0]);
    const auto& decl = asExpr<VarDeclExpr>(stmt.expression);

    const auto& bin = asExpr<BinaryExpr>(*decl.initializer);
    REQUIRE(bin.op.type    == TokenType::PLUS);
    REQUIRE(bin.op.lexeme  == "+");
}

TEST_CASE("Parser - function call with arguments", "[parser]") {
    Program p = parseString("i32 main() { foo(1, 2); }");
    const auto& f    = asStmt<FunctionDeclStmt>(p.declarations[0]);
    const auto& stmt = asStmt<ExprStmt>(*f.body.body[0]);
    const auto& call = asExpr<CallExpr>(stmt.expression);

    REQUIRE(call.callee.lexeme == "foo");
    REQUIRE(call.args.size()   == 2);
}

TEST_CASE("Parser - assignment expression", "[parser]") {
    Program p = parseString("i32 main() { x = 5; }");
    const auto& f    = asStmt<FunctionDeclStmt>(p.declarations[0]);
    const auto& stmt = asStmt<ExprStmt>(*f.body.body[0]);
    const auto& asgn = asExpr<AssignExpr>(stmt.expression);

    REQUIRE(asgn.name.lexeme == "x");
    const auto& val = asExpr<LiteralExpr>(*asgn.value);
    REQUIRE(val.token.lexeme == "5");
}
