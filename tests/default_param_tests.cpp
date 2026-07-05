#include <catch2/catch_test_macros.hpp>
#include "helpers.h"

// ============================================================
// Default parameter values (C++-style). Defaults must form a contiguous trailing run; an omitted
// trailing argument is filled from the callee's default, evaluated at the call site. Applies to
// free functions, methods, and constructors; not to `extern`; a default may not reference the
// function's own parameters.
// ============================================================

// ---- Parser / rule enforcement ----

TEST_CASE("Default - a trailing default parses", "[default][parser]") {
    auto prog = parseStringRaw("fn f(i32 a, i32 b = 0) -> i32 { return a + b; }");
    REQUIRE(prog.declarations.size() == 1);
    const auto& fn = asStmt<FunctionDeclStmt>(prog.declarations[0]);
    REQUIRE(fn.params.size() == 2);
    REQUIRE(fn.params[0].defaultValue == nullptr);
    REQUIRE(fn.params[1].defaultValue != nullptr);
}

TEST_CASE("Default - a non-trailing default is rejected", "[default][parser]") {
    StderrCapture cap;
    auto prog = parseString("fn f(i32 a = 0, i32 b, i32 c = 0) -> i32 { return a; }");
    REQUIRE(cap.contains("must have a default value because an earlier parameter has one"));
}

TEST_CASE("Default - defaults are rejected on extern", "[default][parser]") {
    StderrCapture cap;
    auto prog = parseString("extern puts(ptr s = 0) -> i32;");
    REQUIRE(cap.contains("not allowed on 'extern'"));
}

TEST_CASE("Default - the contiguous rule fires on a constructor too", "[default][parser]") {
    // Constructors route through the same parseParamList, so the trailing-run rule must apply.
    StderrCapture cap;
    auto prog = parseString("class C { C(i32 a = 0, i32 b) { } }");
    REQUIRE(cap.contains("must have a default value because an earlier parameter has one"));
}

// ---- Semantic ----

TEST_CASE("Default - a default whose type mismatches the parameter is rejected", "[default][semantic]") {
    StderrCapture cap;
    auto r = analyzeString(R"(
        class C { }
        fn f(i32 a = C()) -> i32 { return a; }
        fn main() -> i32 { return 0; }
    )");
    REQUIRE(r.hadError);
    REQUIRE(cap.contains("default value of parameter 'a'"));
}

TEST_CASE("Default - a default may not reference the function's parameters", "[default][semantic]") {
    StderrCapture cap;
    auto r = analyzeString(R"(
        fn f(i32 a, i32 b = a) -> i32 { return a + b; }
        fn main() -> i32 { return 0; }
    )");
    REQUIRE(r.hadError);
    REQUIRE(cap.contains("undeclared identifier 'a'"));
}

TEST_CASE("Default - omitting trailing args is accepted", "[default][semantic]") {
    auto r = analyzeString(R"(
        fn add(i32 a, i32 b = 100, i32 c = 1000) -> i32 { return a + b + c; }
        fn main() -> i32 { i32 x = add(1); i32 y = add(1, 2); i32 z = add(1, 2, 3); return 0; }
    )");
    REQUIRE_FALSE(r.hadError);
}

TEST_CASE("Default - fewer args than the required minimum is an arity error", "[default][semantic]") {
    StderrCapture cap;
    auto r = analyzeString(R"(
        fn f(i32 a, i32 b = 0) -> i32 { return a + b; }
        fn main() -> i32 { return f(); }
    )");
    REQUIRE(r.hadError);
    REQUIRE(cap.contains("expects 1 to 2 argument(s), got 0"));
}

TEST_CASE("Default - more args than parameters is still an arity error", "[default][semantic]") {
    // Defaults widen the *lower* arity bound; the upper bound (total params) is unchanged.
    StderrCapture cap;
    auto r = analyzeString(R"(
        fn f(i32 a, i32 b = 0) -> i32 { return a + b; }
        fn main() -> i32 { return f(1, 2, 3); }
    )");
    REQUIRE(r.hadError);
    REQUIRE(cap.contains("expects 1 to 2 argument(s), got 3"));
}

TEST_CASE("Default - a tie is resolved by the contextual return type", "[default][semantic][overload]") {
    // Both overloads are viable at arity 1 (the second via its default); the expected type breaks
    // the tie without an ambiguity error.
    auto r = analyzeString(R"(
        fn g(i32 a) -> i32 { return a; }
        fn g(i32 a, i32 b = 0) -> f64 { return 1.0; }
        fn main() -> i32 { f64 x = g(5); i32 y = g(5); return 0; }
    )");
    REQUIRE_FALSE(r.hadError);
}

TEST_CASE("Default - constructor and method defaults are accepted", "[default][semantic]") {
    auto r = analyzeString(R"(
        class Point {
            mut i32 x; mut i32 y;
            Point(i32 a = 1, i32 b = 2) { x = a; y = b; }
            fn shift(i32 d = 5) mut -> i32 { x = x + d; return x; }
        }
        fn main() -> i32 {
            mut Point& p = new Point();     // both defaulted
            Point& q = new Point(9);        // one defaulted
            i32 r = p.shift();              // method default
            return 0;
        }
    )");
    REQUIRE_FALSE(r.hadError);
}

TEST_CASE("Default - a call ambiguous between an overload and a defaulted one is rejected",
          "[default][semantic][overload]") {
    StderrCapture cap;
    auto r = analyzeString(R"(
        fn f(i32 a) -> i32 { return a; }
        fn f(i32 a, i32 b = 0) -> i32 { return a + b; }
        fn main() -> i32 { return f(1); }   // matches both (arity 1 fits each)
    )");
    REQUIRE(r.hadError);
    REQUIRE(cap.contains("ambiguous"));
}

// ---- Codegen ----

TEST_CASE("Default - an omitted argument is filled with the default at the call site", "[default][codegen]") {
    std::string ir = codegenString(R"(
        fn add(i32 a, i32 b = 7) -> i32 { return a + b; }
        fn main() -> i32 { return add(1); }
    )");
    // The call supplies both operands — the second from the default literal 7.
    REQUIRE(ir.find("call i32 @add(i32 1, i32 7)") != std::string::npos);
}

TEST_CASE("Default - an object-value (sret) return fills its defaults at the call site", "[default][codegen]") {
    std::string ir = codegenString(R"(
        class Point { mut i32 x; mut i32 y; }
        fn make(i32 a = 3, i32 b = 4) -> Point p { p.x = a; p.y = b; }
        fn main() -> i32 { Point d = make(); return 0; }
    )");
    // sret lowering: hidden slot ptr first, then both operands — the second from the default.
    REQUIRE(ir.find("call void @make(ptr") != std::string::npos);
    REQUIRE(ir.find(", i32 3, i32 4)") != std::string::npos);
}
