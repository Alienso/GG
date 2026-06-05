#include <catch2/catch_test_macros.hpp>
#include "helpers.h"

// ============================================================
// Parser — array declaration syntax
// ============================================================

TEST_CASE("Array - parser produces VarDeclExpr with arraySize", "[array][parser]") {
    Program program = parseStringRaw("void main() { i32[5] arr; }");
    REQUIRE(program.declarations.size() == 1);
    const auto& fn = asStmt<FunctionDeclStmt>(program.declarations[0]);
    REQUIRE(fn.body.body.size() == 1);
    const auto& es = asStmt<ExprStmt>(*fn.body.body[0]);
    const auto& vd = asExpr<VarDeclExpr>(es.expression);
    REQUIRE(vd.arraySize == 5);
    REQUIRE(vd.name.lexeme == "arr");
}

// ============================================================
// Semantic — array type checks
// ============================================================

TEST_CASE("Array - semantic analysis passes for valid declaration", "[array][semantic]") {
    SemanticResult result = analyzeString("void main() { i32[5] arr; }");
    REQUIRE_FALSE(result.hadError);
}

TEST_CASE("Array - semantic: out-of-bounds constant index is an error", "[array][semantic]") {
    StderrCapture capture;
    SemanticResult result = analyzeString(
        "void main() { i32[3] arr; arr[5] = 1; }"
    );
    REQUIRE(result.hadError);
    REQUIRE(capture.contains("out of bounds"));
}

TEST_CASE("Array - semantic: in-bounds constant index is accepted", "[array][semantic]") {
    SemanticResult result = analyzeString(
        "void main() { i32[3] arr; arr[2] = 1; }"
    );
    REQUIRE_FALSE(result.hadError);
}

TEST_CASE("Array - semantic: non-integer index is an error", "[array][semantic]") {
    SemanticResult result = analyzeString(
        "void main() { i32[3] arr; i32 x = arr[1]; }"
    );
    // 1 is a valid integer index — should pass
    REQUIRE_FALSE(result.hadError);
}

TEST_CASE("Array - semantic: float index is an error", "[array][semantic]") {
    SemanticResult result = analyzeString(
        "void main() { f64[3] arr; f64 x = arr[1]; }"
    );
    REQUIRE_FALSE(result.hadError);
}

TEST_CASE("Array - semantic: subscript on non-array is an error", "[array][semantic]") {
    SemanticResult result = analyzeString(
        "void main() { i32 x = 0; i32 y = x[0]; }"
    );
    REQUIRE(result.hadError);
}

// ============================================================
// Codegen — array IR output
// ============================================================

TEST_CASE("Array - codegen: alloca emitted for array declaration", "[array][codegen]") {
    std::string ir = codegenString("void main() { i32[5] arr; }");
    REQUIRE(ir.find("alloca [5 x i32]") != std::string::npos);
}

TEST_CASE("Array - codegen: zero-initialiser emitted", "[array][codegen]") {
    std::string ir = codegenString("void main() { i32[5] arr; }");
    REQUIRE(ir.find("zeroinitializer") != std::string::npos);
}

TEST_CASE("Array - codegen: getelementptr emitted on element access", "[array][codegen]") {
    std::string ir = codegenString(
        "void main() { i32[5] arr; i32 x = arr[2]; }"
    );
    REQUIRE(ir.find("getelementptr [5 x i32]") != std::string::npos);
    REQUIRE(ir.find("load i32") != std::string::npos);
}

TEST_CASE("Array - codegen: store emitted on element assignment", "[array][codegen]") {
    std::string ir = codegenString(
        "void main() { i32[5] arr; arr[2] = 42; }"
    );
    REQUIRE(ir.find("getelementptr [5 x i32]") != std::string::npos);
    REQUIRE(ir.find("store i32 42") != std::string::npos);
}

TEST_CASE("Array - codegen: bounds check emitted by default", "[array][codegen]") {
    std::string ir = codegenString(
        "void main() { i32[5] arr; arr[2] = 1; }"
    );
    REQUIRE(ir.find("icmp ult i64") != std::string::npos);
    REQUIRE(ir.find("@abort") != std::string::npos);
    REQUIRE(ir.find("unreachable") != std::string::npos);
}

TEST_CASE("Array - codegen: no bounds check with --no-bounds-check", "[array][codegen]") {
    CompilerOptions opts;
    opts.boundsCheck = false;
    std::string ir = codegenString(
        "void main() { i32[5] arr; arr[2] = 1; }",
        opts
    );
    REQUIRE(ir.find("icmp ult") == std::string::npos);
    REQUIRE(ir.find("@abort") == std::string::npos);
}

TEST_CASE("Array - codegen: multiple element types", "[array][codegen]") {
    std::string ir = codegenString(
        "void main() {\n"
        "    f64[4] floats;\n"
        "    floats[0] = 3.14;\n"
        "    bool[2] flags;\n"
        "    flags[1] = true;\n"
        "}"
    );
    REQUIRE(ir.find("alloca [4 x double]") != std::string::npos);
    REQUIRE(ir.find("alloca [2 x i1]") != std::string::npos);
}

TEST_CASE("Array - codegen: variable index access", "[array][codegen]") {
    std::string ir = codegenString(
        "void main() {\n"
        "    i32[10] arr;\n"
        "    i32 i = 3;\n"
        "    arr[i] = 99;\n"
        "    i32 x = arr[i];\n"
        "}"
    );
    REQUIRE(ir.find("getelementptr [10 x i32]") != std::string::npos);
    REQUIRE(ir.find("icmp ult i64") != std::string::npos);
}

TEST_CASE("Array - codegen: abort is auto-declared when bounds checks are on", "[array][codegen]") {
    std::string ir = codegenString(
        "void main() { i32[3] arr; arr[0] = 1; }"
    );
    REQUIRE(ir.find("declare void @abort()") != std::string::npos);
}

TEST_CASE("Array - codegen: abort not duplicated when process.gg already declares it", "[array][codegen]") {
    // Inline both abort declaration and array access; @abort should appear exactly once in declares
    std::string ir = codegenString(
        "extern void abort();\n"
        "void main() { i32[3] arr; arr[0] = 1; }"
    );
    size_t first  = ir.find("declare void @abort()");
    size_t second = ir.find("declare void @abort()", first + 1);
    REQUIRE(first != std::string::npos);
    REQUIRE(second == std::string::npos);  // appears exactly once
}
