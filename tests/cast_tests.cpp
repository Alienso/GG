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
        fn main() {
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
        fn main() {
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
        fn main() {
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
        fn main() {
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
        fn main() {
            i64 big = 1000;
            i32 small = big as i32;
        }
    )");
    REQUIRE_FALSE(result.hadError);
}

TEST_CASE("Cast - float to int is valid (no error)", "[cast][semantic]") {
    auto result = analyzeString(R"(
        fn main() {
            f64 f = 3.14;
            i32 i = f as i32;
        }
    )");
    REQUIRE_FALSE(result.hadError);
}

TEST_CASE("Cast - int to float is valid (no error)", "[cast][semantic]") {
    auto result = analyzeString(R"(
        fn main() {
            i32 i = 5;
            f32 f = i as f32;
        }
    )");
    REQUIRE_FALSE(result.hadError);
}

TEST_CASE("Cast - int to bool is valid", "[cast][semantic]") {
    auto result = analyzeString(R"(
        fn main() {
            i32 x = 5;
            bool b = x as bool;
        }
    )");
    REQUIRE_FALSE(result.hadError);
}

TEST_CASE("Cast - bool to int is valid", "[cast][semantic]") {
    auto result = analyzeString(R"(
        fn main() {
            bool b = true;
            i32 x = b as i32;
        }
    )");
    REQUIRE_FALSE(result.hadError);
}

TEST_CASE("Cast - int to ptr is valid", "[cast][semantic]") {
    auto result = analyzeString(R"(
        fn main() {
            u64 addr = 0;
            ptr p = addr as ptr;
        }
    )");
    REQUIRE_FALSE(result.hadError);
}

TEST_CASE("Cast - ptr to int is valid", "[cast][semantic]") {
    auto result = analyzeString(R"(
        fn main() {
            ptr p = "hello";
            u64 addr = p as u64;
        }
    )");
    REQUIRE_FALSE(result.hadError);
}

TEST_CASE("Cast - Object to ptr is valid", "[cast][semantic]") {
    auto result = analyzeString(R"(
        class Box { i32 val; }
        fn main() {
            Box b;
            ptr p = b as ptr;
        }
    )");
    REQUIRE_FALSE(result.hadError);
}

TEST_CASE("Cast - Array to ptr is valid", "[cast][semantic]") {
    auto result = analyzeString(R"(
        fn main() {
            i32[4] arr;
            ptr p = arr as ptr;
        }
    )");
    REQUIRE_FALSE(result.hadError);
}

TEST_CASE("Cast - same type identity cast is valid", "[cast][semantic]") {
    auto result = analyzeString(R"(
        fn main() {
            i32 x = 5;
            i32 y = x as i32;
        }
    )");
    REQUIRE_FALSE(result.hadError);
}

TEST_CASE("Cast - narrowing emits no warning (explicit suppresses it)", "[cast][semantic]") {
    StderrCapture cap;
    auto result = analyzeString(R"(
        fn main() {
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
        fn main() {
            i32 x = 5;
            x as void;
        }
    )");
    REQUIRE(result.hadError);
}

TEST_CASE("Cast - cannot cast Object to numeric", "[cast][semantic]") {
    auto result = analyzeString(R"(
        class Box { i32 val; }
        fn main() {
            Box b;
            i32 x = b as i32;
        }
    )");
    REQUIRE(result.hadError);
}

TEST_CASE("Cast - cannot cast float to ptr", "[cast][semantic]") {
    auto result = analyzeString(R"(
        fn main() {
            f32 f = 1.0;
            ptr p = f as ptr;
        }
    )");
    REQUIRE(result.hadError);
}

TEST_CASE("Cast - cannot cast ptr to float", "[cast][semantic]") {
    auto result = analyzeString(R"(
        fn main() {
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
        fn main() {
            i64 big = 1000;
            i32 small = big as i32;
        }
    )");
    REQUIRE(ir.find("trunc i64") != std::string::npos);
    REQUIRE(ir.find("to i32")    != std::string::npos);
}

TEST_CASE("Cast - i32 as i64 emits sext", "[cast][codegen]") {
    auto ir = codegenString(R"(
        fn main() {
            i32 x = 5;
            i64 y = x as i64;
        }
    )");
    REQUIRE(ir.find("sext i32") != std::string::npos);
    REQUIRE(ir.find("to i64")   != std::string::npos);
}

TEST_CASE("Cast - u32 as u64 emits zext", "[cast][codegen]") {
    auto ir = codegenString(R"(
        fn main() {
            u32 x = 5;
            u64 y = x as u64;
        }
    )");
    REQUIRE(ir.find("zext i32") != std::string::npos);
    REQUIRE(ir.find("to i64")   != std::string::npos);
}

TEST_CASE("Cast - f64 as i32 emits fptosi", "[cast][codegen]") {
    auto ir = codegenString(R"(
        fn main() {
            f64 f = 3.14;
            i32 i = f as i32;
        }
    )");
    REQUIRE(ir.find("fptosi double") != std::string::npos);
    REQUIRE(ir.find("to i32")        != std::string::npos);
}

TEST_CASE("Cast - i32 as f32 emits sitofp", "[cast][codegen]") {
    auto ir = codegenString(R"(
        fn main() {
            i32 i = 5;
            f32 f = i as f32;
        }
    )");
    REQUIRE(ir.find("sitofp i32") != std::string::npos);
    REQUIRE(ir.find("to float")   != std::string::npos);
}

TEST_CASE("Cast - f32 as f64 emits fpext", "[cast][codegen]") {
    auto ir = codegenString(R"(
        fn main() {
            f32 f = 1.0;
            f64 d = f as f64;
        }
    )");
    REQUIRE(ir.find("fpext float") != std::string::npos);
    REQUIRE(ir.find("to double")   != std::string::npos);
}

TEST_CASE("Cast - i32 as bool emits icmp ne", "[cast][codegen]") {
    auto ir = codegenString(R"(
        fn main() {
            i32 x = 5;
            bool b = x as bool;
        }
    )");
    REQUIRE(ir.find("icmp ne i32") != std::string::npos);
}

TEST_CASE("Cast - bool as i32 emits zext i1", "[cast][codegen]") {
    auto ir = codegenString(R"(
        fn main() {
            bool b = true;
            i32 x = b as i32;
        }
    )");
    REQUIRE(ir.find("zext i1") != std::string::npos);
    REQUIRE(ir.find("to i32")  != std::string::npos);
}

TEST_CASE("Cast - u64 as ptr emits inttoptr", "[cast][codegen]") {
    auto ir = codegenString(R"(
        fn main() {
            u64 addr = 0;
            ptr p = addr as ptr;
        }
    )");
    REQUIRE(ir.find("inttoptr i64") != std::string::npos);
    REQUIRE(ir.find("to ptr")       != std::string::npos);
}

TEST_CASE("Cast - ptr as u64 emits ptrtoint", "[cast][codegen]") {
    auto ir = codegenString(R"(
        fn main() {
            ptr p = "hello";
            u64 addr = p as u64;
        }
    )");
    REQUIRE(ir.find("ptrtoint ptr") != std::string::npos);
    REQUIRE(ir.find("to i64")       != std::string::npos);
}

TEST_CASE("Cast - i32 as u32 emits no instruction (same IR type)", "[cast][codegen]") {
    auto ir = codegenString(R"(
        fn main() {
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
        class Box { i32 val; }
        fn main() {
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
        fn main() {
            i32[4] arr;
            ptr p = arr as ptr;
        }
    )");
    REQUIRE(ir.find("getelementptr [4 x i32]") != std::string::npos);
    REQUIRE(ir.find("i32 0, i32 0")            != std::string::npos);
}

// ============================================================
// Numeric-literal type adoption (untyped constants)
// ============================================================
// A bare numeric literal is untyped: it adopts the contextual expected type (a numeric target)
// and only falls back to its default (i32 for integers, f64 for decimals) when there is no
// numeric context. So `i64 y = 5;` / `f32 f = 1.0;` / `u32 n = 5;` no longer warn. Adoption is
// confined to *direct* literal bindings — operands of a subexpression keep their default type.

TEST_CASE("Numlit - i64 = literal does not warn (adopts i64)", "[numlit][semantic]") {
    StderrCapture cap;
    auto result = analyzeString("fn main() -> i32 { i64 y = 5; return 0; }");
    REQUIRE_FALSE(result.hadError);
    REQUIRE_FALSE(cap.contains("Warning"));
}

TEST_CASE("Numlit - small/unsigned integer literals no longer warn or error", "[numlit][semantic]") {
    for (const char* src : { "fn main() -> i32 { i8  b = 5;  return 0; }",
                             "fn main() -> i32 { i16 s = 5;  return 0; }",
                             "fn main() -> i32 { u8  c = 5;  return 0; }",   // was an ERROR before
                             "fn main() -> i32 { u16 h = 5;  return 0; }",
                             "fn main() -> i32 { u32 n = 5;  return 0; }",
                             "fn main() -> i32 { u64 m = 5;  return 0; }" }) {
        StderrCapture cap;
        auto result = analyzeString(src);
        REQUIRE_FALSE(result.hadError);
        REQUIRE_FALSE(cap.contains("Warning"));
    }
}

TEST_CASE("Numlit - integer literal adopts char (a 32-bit code point) without warning", "[numlit][semantic]") {
    // `char` is a u32-ranged integer kind, so an in-range integer literal adopts it silently.
    // (Regression: `integerLiteralFits` used to lack a Char case and warned for every value.)
    for (const char* src : { "fn main() -> i32 { char a = 65;  return 0; }",
                             "fn main() -> i32 { char z = 300; return 0; }" }) {   // 300 is a valid code point
        StderrCapture cap;
        auto result = analyzeString(src);
        REQUIRE_FALSE(result.hadError);
        REQUIRE_FALSE(cap.contains("Warning"));
    }
}

TEST_CASE("Numlit - f32 = decimal literal does not warn (adopts f32)", "[numlit][semantic]") {
    StderrCapture cap;
    auto result = analyzeString("fn main() -> i32 { f32 x = 1.0; return 0; }");
    REQUIRE_FALSE(result.hadError);
    REQUIRE_FALSE(cap.contains("Warning"));
}

TEST_CASE("Numlit - a negated literal adopts the target incl. the boundary", "[numlit][semantic]") {
    StderrCapture cap;
    auto result = analyzeString(R"(
        fn main() -> i32 {
            i8 lo = -128;   // boundary min — must not warn
            i8 hi = -5;
            return 0;
        }
    )");
    REQUIRE_FALSE(result.hadError);
    REQUIRE_FALSE(cap.contains("Warning"));
}

TEST_CASE("Numlit - an out-of-range integer literal warns", "[numlit][semantic]") {
    StderrCapture cap;
    auto result = analyzeString("fn main() -> i32 { i8 b = 300; return 0; }");
    REQUIRE_FALSE(result.hadError);   // warning, not error (still adopts, truncating)
    REQUIRE(cap.contains("does not fit"));
}

TEST_CASE("Numlit - an out-of-range literal still emits VALID (wrapped) IR", "[numlit][codegen]") {
    // Regression: `i8 b = 300;` must not emit `store i8 300` (an out-of-range, malformed iN
    // constant that a strict LLVM rejects). It is masked to the target width → 300 & 0xFF = 44.
    auto ir = codegenString("fn main() -> i32 { i8 b = 300; return 0; }");
    REQUIRE(ir.find("store i8 44")  != std::string::npos);
    REQUIRE(ir.find("store i8 300") == std::string::npos);
}

TEST_CASE("Numlit - a literal overflowing even u64 emits valid IR (0)", "[numlit][codegen]") {
    // std::stoull can't parse it → the value is meaningless (already warned) → emit a valid 0
    // rather than a >64-bit constant that would be malformed IR.
    auto ir = codegenString("fn main() -> i32 { u64 x = 99999999999999999999999; return 0; }");
    REQUIRE(ir.find("store i64 0") != std::string::npos);
}

TEST_CASE("Numlit - u64 max is representable and emits as-is", "[numlit][codegen]") {
    auto ir = codegenString("fn main() -> i32 { u64 m = 18446744073709551615; return 0; }");
    REQUIRE(ir.find("store i64 18446744073709551615") != std::string::npos);
}

TEST_CASE("Numlit - a literal with no numeric context defaults to i32", "[numlit][semantic]") {
    // `var` has no target type → the default kicks in.
    auto ir = codegenString("fn main() -> i32 { var z = 5; return z; }");
    REQUIRE(ir.find("alloca i32") != std::string::npos);
}

TEST_CASE("Numlit - a huge literal overflowing the i32 default warns", "[numlit][semantic]") {
    StderrCapture cap;
    auto result = analyzeString("fn main() -> i32 { var z = 9000000000; return 0; }");
    REQUIRE(cap.contains("overflows the default type 'i32'"));
}

TEST_CASE("Numlit - i64 literal lowers to an i64 store with no widening", "[numlit][codegen]") {
    auto ir = codegenString("fn main() -> i32 { i64 big = 9000000000; return 0; }");
    REQUIRE(ir.find("store i64 9000000000") != std::string::npos);  // no overflow to i32
    REQUIRE(ir.find("sext") == std::string::npos);                  // literal was typed i64 directly
}

TEST_CASE("Numlit - f32 literal lowers to an LLVM hex-float constant", "[numlit][codegen]") {
    auto ir = codegenString("fn main() -> i32 { f32 x = 0.5; return 0; }");
    REQUIRE(ir.find("store float 0x") != std::string::npos);   // hex-encoded, exactly representable
    REQUIRE(ir.find("fptrunc") == std::string::npos);          // no f64→f32 truncation needed
}

TEST_CASE("Numlit - adoption is confined to direct literals, not subexpressions", "[numlit][codegen]") {
    // `1 / 2` in an f64 context must stay INTEGER division (operands keep i32), then widen — so the
    // result is 0.0, not 0.5. If the operands wrongly adopted f64 this would be `fdiv`.
    auto ir = codegenString("fn main() -> i32 { f64 d = 1 / 2; return 0; }");
    REQUIRE(ir.find("sdiv i32") != std::string::npos);
    REQUIRE(ir.find("fdiv")     == std::string::npos);
}

TEST_CASE("Numlit - a function-argument literal still widens silently (no adoption)", "[numlit][semantic]") {
    // Arguments are resolved with the expected type cleared, so a literal keeps its default i32 and
    // reaches an i64 parameter via silent widening — unchanged behavior.
    StderrCapture cap;
    auto result = analyzeString(R"(
        fn take(i64 v) -> i64 { return v; }
        fn main() -> i32 { take(5); return 0; }
    )");
    REQUIRE_FALSE(result.hadError);
    REQUIRE_FALSE(cap.contains("Warning"));
}

TEST_CASE("Numlit - adopts in a return context", "[numlit][codegen]") {
    StderrCapture cap;
    auto ir = codegenString("fn f() -> i64 { return 5; }");
    REQUIRE_FALSE(cap.contains("Warning"));
    REQUIRE(ir.find("ret i64 5") != std::string::npos);   // literal typed i64, no sext
}

TEST_CASE("Numlit - adopts on the RHS of an assignment", "[numlit][codegen]") {
    StderrCapture cap;
    auto ir = codegenString(R"(
        fn main() -> i32 {
            mut i64 y = 0;
            y = 5000000000;
            return 0;
        }
    )");
    REQUIRE_FALSE(cap.contains("Warning"));
    REQUIRE(ir.find("store i64 5000000000") != std::string::npos);
}

TEST_CASE("Numlit - adopts the inner type of a nullable target", "[numlit][semantic][nullable]") {
    StderrCapture cap;
    auto result = analyzeString(R"(
        fn main() -> i32 {
            i64? maybe = 5;   // 5 adopts i64, then wraps to i64?
            return 0;
        }
    )");
    REQUIRE_FALSE(result.hadError);
    REQUIRE_FALSE(cap.contains("Warning"));
}

TEST_CASE("Numlit - an integer literal in an f32 slot emits a hex float", "[numlit][codegen]") {
    auto ir = codegenString("fn main() -> i32 { f32 f = 5; return 0; }");
    REQUIRE(ir.find("store float 0x") != std::string::npos);
    REQUIRE(ir.find("sitofp")         == std::string::npos);   // no runtime int→float conversion
}

// ============================================================
// Overflow checks (--overflow-checks; Rust-style, opt-in)
// ============================================================
// When enabled, integer +/-/* (and +=/-=/*=) trap on overflow via
// llvm.{s,u}{add,sub,mul}.with.overflow.iN, and out-of-range narrowing/sign-changing conversions
// trap after a range check. Explicit `as` casts are NOT checked (intentional truncation). Off by
// default the emitted IR is byte-identical to before.

static CompilerOptions checkedOpts() {
    CompilerOptions o;
    o.allowRawPtr    = true;   // keep ptr/string usable, like defaultTestOptions()
    o.overflowChecks = true;
    return o;
}

// The runtime-panic path references @gg_stderr, whose definition is target-specific: Windows/UCRT
// obtains the FILE* via __acrt_iob_func; glibc/musl load the external @stderr global. The triple is
// configurable so GG can target both.
TEST_CASE("Target - Windows triple defines gg_stderr via __acrt_iob_func", "[target][codegen]") {
    CompilerOptions o = checkedOpts();
    o.targetTriple = "x86_64-w64-windows-gnu";
    auto ir = codegenString("fn f(i32 a) -> i32 { return a + a; } fn main() -> i32 { return f(2); }", o);
    REQUIRE(ir.find("target triple = \"x86_64-w64-windows-gnu\"") != std::string::npos);
    REQUIRE(ir.find("call ptr @gg_stderr()")            != std::string::npos);
    REQUIRE(ir.find("define ptr @gg_stderr()")          != std::string::npos);
    REQUIRE(ir.find("@__acrt_iob_func(i32 2)")          != std::string::npos);
    REQUIRE(ir.find("@stderr = external global")        == std::string::npos);
}

TEST_CASE("Target - Linux triple defines gg_stderr via the @stderr global (no __acrt_iob_func)",
          "[target][codegen]") {
    CompilerOptions o = checkedOpts();
    o.targetTriple = "x86_64-pc-linux-gnu";
    auto ir = codegenString("fn f(i32 a) -> i32 { return a + a; } fn main() -> i32 { return f(2); }", o);
    REQUIRE(ir.find("target triple = \"x86_64-pc-linux-gnu\"") != std::string::npos);
    REQUIRE(ir.find("@stderr = external global ptr")     != std::string::npos);
    REQUIRE(ir.find("load ptr, ptr @stderr")             != std::string::npos);
    REQUIRE(ir.find("__acrt_iob_func")                   == std::string::npos);
}

TEST_CASE("Overflow - off by default: plain add, no intrinsic", "[overflow][codegen]") {
    auto ir = codegenString(R"(
        fn add(i32 a, i32 b) -> i32 { return a + b; }
        fn main() -> i32 { return add(1, 2); }
    )");
    REQUIRE(ir.find("add i32")        != std::string::npos);
    REQUIRE(ir.find("with.overflow")  == std::string::npos);
}

TEST_CASE("Overflow - signed + emits sadd.with.overflow and a trap", "[overflow][codegen]") {
    auto ir = codegenString(R"(
        fn add(i32 a, i32 b) -> i32 { return a + b; }
        fn main() -> i32 { return add(1, 2); }
    )", checkedOpts());
    REQUIRE(ir.find("llvm.sadd.with.overflow.i32") != std::string::npos);
    REQUIRE(ir.find("call void @abort()")          != std::string::npos);
}

TEST_CASE("Overflow - unsigned + emits uadd.with.overflow", "[overflow][codegen]") {
    auto ir = codegenString(R"(
        fn add(u32 a, u32 b) -> u32 { return a + b; }
        fn main() -> i32 { return 0; }
    )", checkedOpts());
    REQUIRE(ir.find("llvm.uadd.with.overflow.i32") != std::string::npos);
}

TEST_CASE("Overflow - the trap prints a diagnostic before aborting", "[overflow][codegen]") {
    // The trap block writes "GG runtime error: integer overflow …" to stderr (fputs + __acrt_iob_func)
    // before abort, so a panic explains itself instead of dying silently.
    auto ir = codegenString(R"(
        fn add(i32 a, i32 b) -> i32 { return a + b; }
        fn main() -> i32 { return add(1, 2); }
    )", checkedOpts());
    REQUIRE(ir.find("GG runtime error: integer overflow") != std::string::npos);
    REQUIRE(ir.find("@__acrt_iob_func(i32 2)") != std::string::npos);   // stderr
    REQUIRE(ir.find("@fputs(ptr")              != std::string::npos);
    REQUIRE(ir.find("call void @abort()")      != std::string::npos);   // still aborts after
}

TEST_CASE("Overflow - a narrowing conversion trap has its own message", "[overflow][codegen]") {
    auto ir = codegenString(R"(
        fn narrow(i64 v) -> i32 { return v; }   // implicit i64 -> i32 narrowing, checked
        fn main() -> i32 { return narrow(5); }
    )", checkedOpts());
    REQUIRE(ir.find("GG runtime error: value out of range in narrowing conversion") != std::string::npos);
}

TEST_CASE("Overflow - subtraction and multiplication are checked", "[overflow][codegen]") {
    auto ir = codegenString(R"(
        fn f(i64 a, i64 b) -> i64 { return a - b; }
        fn g(i64 a, i64 b) -> i64 { return a * b; }
        fn main() -> i32 { return 0; }
    )", checkedOpts());
    REQUIRE(ir.find("llvm.ssub.with.overflow.i64") != std::string::npos);
    REQUIRE(ir.find("llvm.smul.with.overflow.i64") != std::string::npos);
}

TEST_CASE("Overflow - compound += is checked", "[overflow][codegen]") {
    auto ir = codegenString(R"(
        fn main() -> i32 {
            mut i32 x = 1;
            x += 2;
            return 0;
        }
    )", checkedOpts());
    REQUIRE(ir.find("llvm.sadd.with.overflow.i32") != std::string::npos);
}

TEST_CASE("Overflow - division is NOT checked (plain sdiv)", "[overflow][codegen]") {
    auto ir = codegenString(R"(
        fn d(i32 a, i32 b) -> i32 { return a / b; }
        fn main() -> i32 { return 0; }
    )", checkedOpts());
    REQUIRE(ir.find("sdiv i32")     != std::string::npos);
    REQUIRE(ir.find("with.overflow") == std::string::npos);
}

TEST_CASE("Overflow - float arithmetic is NOT checked", "[overflow][codegen]") {
    auto ir = codegenString(R"(
        fn add(f64 a, f64 b) -> f64 { return a + b; }
        fn main() -> i32 { return 0; }
    )", checkedOpts());
    REQUIRE(ir.find("fadd")          != std::string::npos);
    REQUIRE(ir.find("with.overflow") == std::string::npos);
}

TEST_CASE("Overflow - implicit narrowing is range-checked", "[overflow][codegen]") {
    // A dynamic i32 value narrowed to i8: trunc, sext back, compare, trap on mismatch.
    auto ir = codegenString(R"(
        fn wide() -> i32 { return 5; }
        fn main() -> i32 {
            i8 x = wide();
            return 0;
        }
    )", checkedOpts());
    REQUIRE(ir.find("trunc i32")           != std::string::npos);
    REQUIRE(ir.find("sext i8")             != std::string::npos);   // extend back to compare
    REQUIRE(ir.find("call void @abort()")  != std::string::npos);
}

TEST_CASE("Overflow - an explicit `as` cast is NOT checked", "[overflow][codegen]") {
    // `as` is an intentional truncation → no trap, even with checks on.
    auto ir = codegenString(R"(
        fn wide() -> i32 { return 300; }
        fn main() -> i32 {
            i8 x = wide() as i8;
            return 0;
        }
    )", checkedOpts());
    REQUIRE(ir.find("trunc i32")          != std::string::npos);
    REQUIRE(ir.find("call void @abort()") == std::string::npos);   // no conversion trap
}

TEST_CASE("Overflow - widening conversion is not checked", "[overflow][codegen]") {
    auto ir = codegenString(R"(
        fn small() -> i32 { return 5; }
        fn main() -> i32 {
            i64 x = small();   // widening i32 → i64 can't lose data → no check
            return 0;
        }
    )", checkedOpts());
    REQUIRE(ir.find("sext i32")           != std::string::npos);
    REQUIRE(ir.find("call void @abort()") == std::string::npos);
}

// ---- Literal adoption from a non-literal sibling in a binary op (numlit refinement) ----

TEST_CASE("Numlit - a literal adopts a non-literal sibling's type in a comparison", "[numlit][semantic]") {
    // `big == 6000000000` — the >i32 literal adopts i64 from `big`, so it does not wrap/warn.
    StderrCapture cap;
    auto result = analyzeString(R"(
        fn main() -> i32 {
            mut i64 big = 3000000000;
            big = big + big;
            if (big == 6000000000) { return 0; }
            return 1;
        }
    )");
    REQUIRE_FALSE(result.hadError);
    REQUIRE_FALSE(cap.contains("overflows"));
}

TEST_CASE("Numlit - a literal adopts a non-literal sibling's type in arithmetic", "[numlit][semantic]") {
    StderrCapture cap;
    auto result = analyzeString(R"(
        fn main() -> i32 {
            mut i64 big = 3000000000;
            var s = big + 3000000000;   // literal adopts i64 → 6e9, no wrap/warn
            return 0;
        }
    )");
    REQUIRE_FALSE(result.hadError);
    REQUIRE_FALSE(cap.contains("overflows"));
}

TEST_CASE("Numlit - a decimal literal does NOT adopt an integer sibling (float division kept)", "[numlit][codegen]") {
    // `1.0 / count` (count i32) must stay float division — a decimal literal never becomes an int.
    auto ir = codegenString(R"(
        fn main() -> i32 {
            mut i32 count = 2;
            f64 half = 1.0 / count;
            return 0;
        }
    )");
    REQUIRE(ir.find("fdiv")     != std::string::npos);
    REQUIRE(ir.find("sdiv i32") == std::string::npos);
}

TEST_CASE("Numlit - two literals in a subexpression still keep the default (no sibling)", "[numlit][codegen]") {
    // `1 / 2` has no non-literal sibling → both stay i32 → integer division, even in an f64 slot.
    auto ir = codegenString("fn main() -> i32 { f64 d = 1 / 2; return 0; }");
    REQUIRE(ir.find("sdiv i32") != std::string::npos);
    REQUIRE(ir.find("fdiv")     == std::string::npos);
}

TEST_CASE("Numlit - a compound-assignment RHS literal adopts the target type", "[numlit][semantic]") {
    // `u64Var *= 10;` types the literal 10 as u64 — no spurious i32->u64 narrowing warning.
    StderrCapture cap;
    auto r = analyzeString(R"(
        fn main() -> i32 { mut u64 x = 100; x *= 10; x += 5; x -= 1; return 0; }
    )");
    REQUIRE_FALSE(r.hadError);
    REQUIRE_FALSE(cap.contains("may lose data"));
}

TEST_CASE("Numlit - a non-literal compound-assignment RHS still warns on a lossy conversion",
          "[numlit][semantic]") {
    // The adoption is literal-only: an i32 variable on the RHS of a u8 `+=` still warns.
    StderrCapture cap;
    auto r = analyzeString(R"(
        fn main() -> i32 { mut u8 b = 0; i32 n = 5; b += n; return 0; }
    )");
    REQUIRE(cap.contains("may lose data"));
}

// ---- Overflow checks on ++/-- and unary negation (parity with += / -=) ----

TEST_CASE("Overflow - postfix ++ is checked", "[overflow][codegen]") {
    auto ir = codegenString(R"(
        fn main() -> i32 { mut i8 c = 0; c++; return 0; }
    )", checkedOpts());
    REQUIRE(ir.find("llvm.sadd.with.overflow.i8") != std::string::npos);
}

TEST_CASE("Overflow - prefix -- is checked", "[overflow][codegen]") {
    auto ir = codegenString(R"(
        fn main() -> i32 { mut i8 c = 0; --c; return 0; }
    )", checkedOpts());
    REQUIRE(ir.find("llvm.ssub.with.overflow.i8") != std::string::npos);
}

TEST_CASE("Overflow - unsigned ++ uses the unsigned intrinsic", "[overflow][codegen]") {
    auto ir = codegenString(R"(
        fn main() -> i32 { mut u8 c = 0; c++; return 0; }
    )", checkedOpts());
    REQUIRE(ir.find("llvm.uadd.with.overflow.i8") != std::string::npos);
}

TEST_CASE("Overflow - signed unary negation is checked (catches -INT_MIN)", "[overflow][codegen]") {
    auto ir = codegenString(R"(
        fn neg(i32 x) -> i32 { return -x; }
        fn main() -> i32 { return 0; }
    )", checkedOpts());
    REQUIRE(ir.find("llvm.ssub.with.overflow.i32") != std::string::npos);
}

TEST_CASE("Overflow - ++/-- and unary - are NOT checked when the flag is off", "[overflow][codegen]") {
    auto ir = codegenString(R"(
        fn neg(i32 x) -> i32 { return -x; }
        fn main() -> i32 { mut i8 c = 0; c++; --c; return 0; }
    )");   // default options: overflowChecks = false
    REQUIRE(ir.find("with.overflow") == std::string::npos);
    REQUIRE(ir.find("add i8")        != std::string::npos);   // plain ++
}

TEST_CASE("Overflow - a narrowing at a return is checked", "[overflow][codegen]") {
    auto ir = codegenString(R"(
        fn narrow(i32 v) -> i8 { return v; }
        fn main() -> i32 { return 0; }
    )", checkedOpts());
    REQUIRE(ir.find("trunc i32")          != std::string::npos);
    REQUIRE(ir.find("call void @abort()") != std::string::npos);
}

TEST_CASE("Overflow - a narrowing at a call argument is checked", "[overflow][codegen]") {
    auto ir = codegenString(R"(
        fn take(i8 v) -> i8 { return v; }
        fn main() -> i32 {
            mut i32 big = 200;
            i8 r = take(big);
            return 0;
        }
    )", checkedOpts());
    REQUIRE(ir.find("trunc i32")          != std::string::npos);
    REQUIRE(ir.find("call void @abort()") != std::string::npos);
}

// ============================================================
// Mixed signed/unsigned arithmetic — SIGNED wins (GG departs from C)
// ============================================================
// The common type of a mixed signed/unsigned operation is a SIGNED integer of the wider width,
// so `-6 / 3u == -2` (not a huge unsigned result) and `-1 < 1u` is true. The unsigned operand is
// reinterpreted as signed.

TEST_CASE("Arith - mixed signed/unsigned division is SIGNED division", "[arith]") {
    auto ir = codegenString(R"(
        fn main() -> i32 {
            mut i32 s = 0 - 6;
            mut u32 u = 3;
            var q = s / u;      // common type i32 → sdiv → -2
            return q;
        }
    )");
    REQUIRE(ir.find("sdiv i32") != std::string::npos);
    REQUIRE(ir.find("udiv")     == std::string::npos);
}

TEST_CASE("Arith - mixed signed/unsigned comparison is a SIGNED compare", "[arith]") {
    auto ir = codegenString(R"(
        fn main() -> i32 {
            mut i32 s = 0 - 1;
            mut u32 u = 1;
            if (s < u) { return 1; }   // -1 < 1 → true via signed slt, not unsigned ult
            return 0;
        }
    )");
    REQUIRE(ir.find("icmp slt") != std::string::npos);
    REQUIRE(ir.find("icmp ult") == std::string::npos);
}

TEST_CASE("Arith - mixed widths widen to a signed type of the larger width", "[arith]") {
    auto ir = codegenString(R"(
        fn main() -> i32 {
            mut i32 s = 0 - 6;
            mut u64 u = 3;
            var q = s / u;      // → i64 sdiv (signed, widened to 64)
            return 0;
        }
    )");
    REQUIRE(ir.find("sdiv i64") != std::string::npos);
    REQUIRE(ir.find("udiv")     == std::string::npos);
}

TEST_CASE("Arith - pure unsigned division is still unsigned", "[arith]") {
    auto ir = codegenString(R"(
        fn main() -> i32 {
            mut u32 a = 10;
            mut u32 b = 3;
            var q = a / b;      // both unsigned → udiv
            return 0;
        }
    )");
    REQUIRE(ir.find("udiv i32") != std::string::npos);
}

TEST_CASE("Arith - both-int-literal division stays integer even in an f64 slot", "[arith]") {
    auto ir = codegenString("fn main() -> i32 { f64 d = 7 / 2; return 0; }");
    REQUIRE(ir.find("sdiv i32") != std::string::npos);   // 7/2 == 3, then widened to 3.0
    REQUIRE(ir.find("fdiv")     == std::string::npos);
}

// ---- Shifts follow the LEFT operand, not the symmetric common type ----

TEST_CASE("Arith - right-shift is LOGICAL for an unsigned left operand", "[arith]") {
    // u32 >> count must be lshr even when the count is signed (which used to force ashr and
    // corrupt the high bit).
    auto ir = codegenString(R"(
        fn main() -> i32 {
            mut u32 x = 16;
            mut i32 sh = 1;
            var r = x >> sh;
            return 0;
        }
    )");
    REQUIRE(ir.find("lshr") != std::string::npos);
    REQUIRE(ir.find("ashr") == std::string::npos);
}

TEST_CASE("Arith - right-shift is ARITHMETIC for a signed left operand", "[arith]") {
    // i32 >> count must be ashr even when the count is unsigned (sign must be preserved).
    auto ir = codegenString(R"(
        fn main() -> i32 {
            mut i32 x = 0 - 8;
            mut u32 sh = 1;
            var r = x >> sh;
            return 0;
        }
    )");
    REQUIRE(ir.find("ashr") != std::string::npos);
    REQUIRE(ir.find("lshr") == std::string::npos);
}

TEST_CASE("Arith - a shift's result type follows the left operand's width", "[arith]") {
    auto ir = codegenString(R"(
        fn main() -> i32 {
            mut u8  x = 200;
            mut i32 c = 1;
            var r = x >> c;      // result u8 → lshr i8, count coerced to i8
            return 0;
        }
    )");
    REQUIRE(ir.find("lshr i8") != std::string::npos);
}
