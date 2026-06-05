#include <catch2/catch_test_macros.hpp>
#include "helpers.h"
#include "../source/parser/AstPrinter.h"

// ============================================================
// Lexer
// ============================================================

TEST_CASE("Cast - 'as' is a keyword token", "[cast][lexer]") {
    auto tokens = lexString("x as i32");
    // x  as  i32  EOF  (indices 0-3)
    REQUIRE(tokens.size() == 4);
    REQUIRE(tokens[1].type    == TokenType::AS);
    REQUIRE(tokens[1].lexeme  == "as");
}

// ============================================================
// Parser
// ============================================================

TEST_CASE("Cast - simple 'as' produces CastExpr with correct target type", "[cast][parser]") {
    auto ast = parseString(R"(
        void main() {
            i64 x = 1;
            i32 y = x as i32;
        }
    )");
    REQUIRE(ast.declarations.size() == 1);
    const auto& fn = asStmt<FunctionDeclStmt>(ast.declarations[0]);
    // second statement:  i32 y = x as i32
    const auto& varDeclExpr = asExpr<VarDeclExpr>(
        asStmt<ExprStmt>(*fn.body.body[1]).expression);
    REQUIRE(varDeclExpr.initializer != nullptr);
    const auto& cast = asExpr<CastExpr>(*varDeclExpr.initializer);
    REQUIRE(cast.targetType.type   == TokenType::I32);
    REQUIRE(cast.targetType.lexeme == "i32");
    REQUIRE(std::holds_alternative<IdentifierExpr>(*cast.operand->node));
}

TEST_CASE("Cast - chained 'as' is left-associative", "[cast][parser]") {
    auto ast = parseString(R"(
        void main() {
            i64 x = 1;
            f64 z = x as i32 as f64;
        }
    )");
    const auto& fn = asStmt<FunctionDeclStmt>(ast.declarations[0]);
    const auto& varDeclExpr = asExpr<VarDeclExpr>(
        asStmt<ExprStmt>(*fn.body.body[1]).expression);
    // outer cast:  (x as i32) as f64
    const auto& outerCast = asExpr<CastExpr>(*varDeclExpr.initializer);
    REQUIRE(outerCast.targetType.type == TokenType::F64);
    // inner cast:  x as i32
    const auto& innerCast = asExpr<CastExpr>(*outerCast.operand);
    REQUIRE(innerCast.targetType.type == TokenType::I32);
    REQUIRE(std::holds_alternative<IdentifierExpr>(*innerCast.operand->node));
}

TEST_CASE("Cast - 'as' binds tighter than '*'", "[cast][parser]") {
    // a * b as i32  should parse as  a * (b as i32)
    auto ast = parseString(R"(
        void main() {
            i64 a = 1;
            i64 b = 2;
            i64 r = a * b as i64;
        }
    )");
    const auto& fn = asStmt<FunctionDeclStmt>(ast.declarations[0]);
    const auto& varDeclExpr = asExpr<VarDeclExpr>(
        asStmt<ExprStmt>(*fn.body.body[2]).expression);
    // Should be Binary(*) with right = CastExpr
    const auto& binary = asExpr<BinaryExpr>(*varDeclExpr.initializer);
    REQUIRE(binary.operatorToken.type == TokenType::STAR);
    REQUIRE(std::holds_alternative<CastExpr>(*binary.right->node));
}

TEST_CASE("Cast - AstPrinter emits Cast node", "[cast][parser]") {
    auto ast = parseString(R"(
        void main() {
            i64 x = 1;
            i32 y = x as i32;
        }
    )");
    std::ostringstream oss;
    AstPrinter printer;
    printer.print(ast, oss);
    REQUIRE(oss.str().find("Cast as 'i32'") != std::string::npos);
}

// ============================================================
// Semantic — valid casts (no error)
// ============================================================

TEST_CASE("Cast - numeric narrowing is valid (no error)", "[cast][semantic]") {
    auto result = analyzeString(R"(
        void main() {
            i64 big = 1000;
            i32 small = big as i32;
        }
    )");
    REQUIRE_FALSE(result.hadError);
}

TEST_CASE("Cast - float to int is valid (no error)", "[cast][semantic]") {
    auto result = analyzeString(R"(
        void main() {
            f64 f = 3.14;
            i32 i = f as i32;
        }
    )");
    REQUIRE_FALSE(result.hadError);
}

TEST_CASE("Cast - int to float is valid (no error)", "[cast][semantic]") {
    auto result = analyzeString(R"(
        void main() {
            i32 i = 5;
            f32 f = i as f32;
        }
    )");
    REQUIRE_FALSE(result.hadError);
}

TEST_CASE("Cast - int to bool is valid", "[cast][semantic]") {
    auto result = analyzeString(R"(
        void main() {
            i32 x = 5;
            bool b = x as bool;
        }
    )");
    REQUIRE_FALSE(result.hadError);
}

TEST_CASE("Cast - bool to int is valid", "[cast][semantic]") {
    auto result = analyzeString(R"(
        void main() {
            bool b = true;
            i32 x = b as i32;
        }
    )");
    REQUIRE_FALSE(result.hadError);
}

TEST_CASE("Cast - int to ptr is valid", "[cast][semantic]") {
    auto result = analyzeString(R"(
        void main() {
            u64 addr = 0;
            ptr p = addr as ptr;
        }
    )");
    REQUIRE_FALSE(result.hadError);
}

TEST_CASE("Cast - ptr to int is valid", "[cast][semantic]") {
    auto result = analyzeString(R"(
        void main() {
            ptr p = "hello";
            u64 addr = p as u64;
        }
    )");
    REQUIRE_FALSE(result.hadError);
}

TEST_CASE("Cast - Object to ptr is valid", "[cast][semantic]") {
    auto result = analyzeString(R"(
        class Box { public i32 val; }
        void main() {
            Box b;
            ptr p = b as ptr;
        }
    )");
    REQUIRE_FALSE(result.hadError);
}

TEST_CASE("Cast - Array to ptr is valid", "[cast][semantic]") {
    auto result = analyzeString(R"(
        void main() {
            i32[4] arr;
            ptr p = arr as ptr;
        }
    )");
    REQUIRE_FALSE(result.hadError);
}

TEST_CASE("Cast - same type identity cast is valid", "[cast][semantic]") {
    auto result = analyzeString(R"(
        void main() {
            i32 x = 5;
            i32 y = x as i32;
        }
    )");
    REQUIRE_FALSE(result.hadError);
}

TEST_CASE("Cast - narrowing emits no warning (explicit suppresses it)", "[cast][semantic]") {
    StderrCapture cap;
    auto result = analyzeString(R"(
        void main() {
            i64 big = 1000;
            i32 small = big as i32;
        }
    )");
    REQUIRE_FALSE(result.hadError);
    REQUIRE_FALSE(cap.contains("Warning"));
}

// ============================================================
// Semantic — invalid casts (error)
// ============================================================

TEST_CASE("Cast - cannot cast to void", "[cast][semantic]") {
    auto result = analyzeString(R"(
        void main() {
            i32 x = 5;
            x as void;
        }
    )");
    REQUIRE(result.hadError);
}

TEST_CASE("Cast - cannot cast Object to numeric", "[cast][semantic]") {
    auto result = analyzeString(R"(
        class Box { public i32 val; }
        void main() {
            Box b;
            i32 x = b as i32;
        }
    )");
    REQUIRE(result.hadError);
}

TEST_CASE("Cast - cannot cast float to ptr", "[cast][semantic]") {
    auto result = analyzeString(R"(
        void main() {
            f32 f = 1.0;
            ptr p = f as ptr;
        }
    )");
    REQUIRE(result.hadError);
}

TEST_CASE("Cast - cannot cast ptr to float", "[cast][semantic]") {
    auto result = analyzeString(R"(
        void main() {
            ptr p = "hello";
            f32 f = p as f32;
        }
    )");
    REQUIRE(result.hadError);
}

// ============================================================
// Codegen — IR instructions
// ============================================================

TEST_CASE("Cast - i64 as i32 emits trunc", "[cast][codegen]") {
    auto ir = codegenString(R"(
        void main() {
            i64 big = 1000;
            i32 small = big as i32;
        }
    )");
    REQUIRE(ir.find("trunc i64") != std::string::npos);
    REQUIRE(ir.find("to i32")    != std::string::npos);
}

TEST_CASE("Cast - i32 as i64 emits sext", "[cast][codegen]") {
    auto ir = codegenString(R"(
        void main() {
            i32 x = 5;
            i64 y = x as i64;
        }
    )");
    REQUIRE(ir.find("sext i32") != std::string::npos);
    REQUIRE(ir.find("to i64")   != std::string::npos);
}

TEST_CASE("Cast - u32 as u64 emits zext", "[cast][codegen]") {
    auto ir = codegenString(R"(
        void main() {
            u32 x = 5;
            u64 y = x as u64;
        }
    )");
    REQUIRE(ir.find("zext i32") != std::string::npos);
    REQUIRE(ir.find("to i64")   != std::string::npos);
}

TEST_CASE("Cast - f64 as i32 emits fptosi", "[cast][codegen]") {
    auto ir = codegenString(R"(
        void main() {
            f64 f = 3.14;
            i32 i = f as i32;
        }
    )");
    REQUIRE(ir.find("fptosi double") != std::string::npos);
    REQUIRE(ir.find("to i32")        != std::string::npos);
}

TEST_CASE("Cast - i32 as f32 emits sitofp", "[cast][codegen]") {
    auto ir = codegenString(R"(
        void main() {
            i32 i = 5;
            f32 f = i as f32;
        }
    )");
    REQUIRE(ir.find("sitofp i32") != std::string::npos);
    REQUIRE(ir.find("to float")   != std::string::npos);
}

TEST_CASE("Cast - f32 as f64 emits fpext", "[cast][codegen]") {
    auto ir = codegenString(R"(
        void main() {
            f32 f = 1.0;
            f64 d = f as f64;
        }
    )");
    REQUIRE(ir.find("fpext float") != std::string::npos);
    REQUIRE(ir.find("to double")   != std::string::npos);
}

TEST_CASE("Cast - i32 as bool emits icmp ne", "[cast][codegen]") {
    auto ir = codegenString(R"(
        void main() {
            i32 x = 5;
            bool b = x as bool;
        }
    )");
    REQUIRE(ir.find("icmp ne i32") != std::string::npos);
}

TEST_CASE("Cast - bool as i32 emits zext i1", "[cast][codegen]") {
    auto ir = codegenString(R"(
        void main() {
            bool b = true;
            i32 x = b as i32;
        }
    )");
    REQUIRE(ir.find("zext i1") != std::string::npos);
    REQUIRE(ir.find("to i32")  != std::string::npos);
}

TEST_CASE("Cast - u64 as ptr emits inttoptr", "[cast][codegen]") {
    auto ir = codegenString(R"(
        void main() {
            u64 addr = 0;
            ptr p = addr as ptr;
        }
    )");
    REQUIRE(ir.find("inttoptr i64") != std::string::npos);
    REQUIRE(ir.find("to ptr")       != std::string::npos);
}

TEST_CASE("Cast - ptr as u64 emits ptrtoint", "[cast][codegen]") {
    auto ir = codegenString(R"(
        void main() {
            ptr p = "hello";
            u64 addr = p as u64;
        }
    )");
    REQUIRE(ir.find("ptrtoint ptr") != std::string::npos);
    REQUIRE(ir.find("to i64")       != std::string::npos);
}

TEST_CASE("Cast - i32 as u32 emits no instruction (same IR type)", "[cast][codegen]") {
    auto ir = codegenString(R"(
        void main() {
            i32 x = 5;
            u32 y = x as u32;
        }
    )");
    // No trunc/sext/zext/bitcast needed — i32 and u32 are both i32 in LLVM
    REQUIRE(ir.find("trunc") == std::string::npos);
    REQUIRE(ir.find("sext")  == std::string::npos);
    REQUIRE(ir.find("zext")  == std::string::npos);
}

TEST_CASE("Cast - Object as ptr returns alloca pointer (no extra instruction)", "[cast][codegen]") {
    auto ir = codegenString(R"(
        class Box { public i32 val; }
        void main() {
            Box b;
            ptr p = b as ptr;
        }
    )");
    // The object's alloca IS the ptr — no ptrtoint/inttoptr/GEP
    REQUIRE(ir.find("ptrtoint") == std::string::npos);
    REQUIRE(ir.find("inttoptr") == std::string::npos);
    // The alloca for b must exist
    REQUIRE(ir.find("%b.addr = alloca %Box") != std::string::npos);
    // ptr p should be assigned %b.addr (store ptr %b.addr, ptr %p.addr)
    REQUIRE(ir.find("store ptr %b.addr, ptr %p.addr") != std::string::npos);
}

TEST_CASE("Cast - Array as ptr emits GEP to first element", "[cast][codegen]") {
    auto ir = codegenString(R"(
        void main() {
            i32[4] arr;
            ptr p = arr as ptr;
        }
    )");
    REQUIRE(ir.find("getelementptr [4 x i32]") != std::string::npos);
    REQUIRE(ir.find("i32 0, i32 0")            != std::string::npos);
}
