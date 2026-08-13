#include <catch2/catch_test_macros.hpp>
#include "helpers.h"

// ============================================================
// Local variable type inference — the `var` keyword
// ============================================================
// `var name = expr;` deduces the variable's type from its initializer. It is
// const by default (single-assignment), exactly like a typed local; `mut var`
// (or `var mut`) opts into mutability. An initializer is required — there is
// nothing to infer from otherwise. The parser leaves a `var` sentinel type
// token; the semantic analyzer deduces the type and records a synthesized type
// token that codegen swaps in, so every existing declaration path is reused.

// ------------------------------------------------------------
// Parser
// ------------------------------------------------------------

TEST_CASE("var - parses to a VarDeclExpr with a VAR sentinel type token", "[var][parser]") {
    auto prog = parseString(R"(
        fn main() -> i32 {
            var x = 5;
            return x;
        }
    )");
    REQUIRE(prog.declarations.size() == 1);
    const auto& fn = asStmt<FunctionDeclStmt>(prog.declarations[0]);
    const auto& d  = asExpr<VarDeclExpr>(std::get<ExprStmt>(*fn.body.body[0]->node).expression);
    REQUIRE(d.typeName.type == TokenType::VAR);
    REQUIRE(d.name.lexeme == "x");
    REQUIRE(d.initializer != nullptr);
    REQUIRE_FALSE(d.isMut);
}

TEST_CASE("var - 'mut var' and 'var mut' both set isMut", "[var][parser]") {
    for (const char* src : { "fn f() -> i32 { mut var c = 0; c = 1; return c; }",
                             "fn f() -> i32 { var mut c = 0; c = 1; return c; }" }) {
        auto prog = parseString(src);
        REQUIRE(prog.declarations.size() == 1);
        const auto& fn = asStmt<FunctionDeclStmt>(prog.declarations[0]);
        const auto& d  = asExpr<VarDeclExpr>(std::get<ExprStmt>(*fn.body.body[0]->node).expression);
        REQUIRE(d.typeName.type == TokenType::VAR);
        REQUIRE(d.isMut);
    }
}

TEST_CASE("var - without an initializer is a parse error", "[var][parser]") {
    StderrCapture cap;
    auto prog = parseString("fn f() -> i32 { var x; return 0; }");
    REQUIRE(cap.contains("requires an initializer"));
}

// ------------------------------------------------------------
// Semantic — inference + const-by-default
// ------------------------------------------------------------

TEST_CASE("var - records an inferred type token per var-decl node", "[var][semantic]") {
    auto result = analyzeString(R"(
        fn main() -> i32 {
            var x = 5;
            var y = 2.5;
            return x;
        }
    )");
    REQUIRE_FALSE(result.hadError);
    REQUIRE(result.inferredVarType.size() == 2);
}

TEST_CASE("var - is const by default: reassignment is an error", "[var][semantic]") {
    StderrCapture cap;
    auto result = analyzeString(R"(
        fn main() -> i32 {
            var x = 5;
            x = 6;
            return x;
        }
    )");
    REQUIRE(result.hadError);
    REQUIRE(cap.contains("cannot reassign immutable variable 'x'"));
}

TEST_CASE("var - 'mut var' permits reassignment", "[var][semantic]") {
    auto result = analyzeString(R"(
        fn main() -> i32 {
            mut var x = 5;
            x = 6;
            return x;
        }
    )");
    REQUIRE_FALSE(result.hadError);
}

TEST_CASE("var - the inferred type is used for type checking", "[var][semantic]") {
    // x is inferred i32; calling a method on it (a primitive) must be rejected —
    // proving the binding really carries the deduced type.
    StderrCapture cap;
    auto result = analyzeString(R"(
        fn main() -> i32 {
            var x = 5;
            return x.foo();
        }
    )");
    REQUIRE(result.hadError);
}

TEST_CASE("var - redeclaration in the same scope is an error", "[var][semantic]") {
    StderrCapture cap;
    auto result = analyzeString(R"(
        fn main() -> i32 {
            var x = 1;
            var x = 2;
            return x;
        }
    )");
    REQUIRE(result.hadError);
    REQUIRE(cap.contains("already declared in this scope"));
}

TEST_CASE("var - cannot infer from a 'void' initializer", "[var][semantic]") {
    StderrCapture cap;
    auto result = analyzeString(R"(
        fn nothing() { }
        fn main() -> i32 {
            var x = nothing();
            return 0;
        }
    )");
    REQUIRE(result.hadError);
    REQUIRE(cap.contains("void"));
}

TEST_CASE("var - cannot infer from a bare 'null'", "[var][semantic]") {
    StderrCapture cap;
    auto result = analyzeString(R"(
        fn main() -> i32 {
            var x = null;
            return 0;
        }
    )");
    REQUIRE(result.hadError);
    REQUIRE(cap.contains("null"));
}

TEST_CASE("var - a value object is inferred and copied", "[var][semantic]") {
    auto result = analyzeString(R"(
        class Point { i32 x; i32 y; Point(i32 a, i32 b) { this.x = a; this.y = b; }
                      fn sum() -> i32 { return this.x + this.y; } }
        fn main() -> i32 {
            var p = Point(3, 4);
            return p.sum();
        }
    )");
    REQUIRE_FALSE(result.hadError);
}

TEST_CASE("var - a nullable initializer infers a nullable binding", "[var][semantic][nullable]") {
    // np is inferred Point&? ; using it without a null-check/unwrap must be rejected,
    // proving the inferred type retained its nullability.
    StderrCapture cap;
    auto result = analyzeString(R"(
        class Point { i32 x; Point(i32 a) { this.x = a; } fn get() -> i32 { return this.x; } }
        fn maybe(bool b) -> Point&? p { if (b) { p = new Point(1); } return p; }
        fn main() -> i32 {
            var np = maybe(true);
            return np.get();
        }
    )");
    REQUIRE(result.hadError);   // must unwrap / null-check a Point&? before member access
}

// ------------------------------------------------------------
// Codegen — the deduced type drives the same lowering as an explicit one
// ------------------------------------------------------------

TEST_CASE("var - i32 inference lowers to an i32 alloca", "[var][codegen]") {
    auto ir = codegenString(R"(
        fn main() -> i32 {
            var x = 5;
            return x;
        }
    )");
    REQUIRE(ir.find("alloca i32") != std::string::npos);
}

TEST_CASE("var - f64 inference lowers to a double alloca", "[var][codegen]") {
    auto ir = codegenString(R"(
        fn main() -> i32 {
            var f = 2.5;
            return 0;
        }
    )");
    REQUIRE(ir.find("alloca double") != std::string::npos);
}

TEST_CASE("var - value-object inference lowers to the class struct alloca", "[var][codegen]") {
    auto ir = codegenString(R"(
        class Point { i32 x; i32 y; Point(i32 a, i32 b) { this.x = a; this.y = b; } }
        fn main() -> i32 {
            var p = Point(3, 4);
            return 0;
        }
    )");
    REQUIRE(ir.find("alloca %Point") != std::string::npos);
    REQUIRE(ir.find("@Point_Point") != std::string::npos);
}

TEST_CASE("var - heap-reference inference is released at scope exit", "[var][codegen]") {
    // `var r = new Point(...)` must behave exactly like `Point& r = new Point(...)`:
    // the +1 is co-owned and released on the way out.
    auto ir = codegenString(R"(
        class Point { i32 x; Point(i32 a) { this.x = a; } }
        fn main() -> i32 {
            var r = new Point(7);
            return 0;
        }
    )");
    REQUIRE(ir.find("@gg_release") != std::string::npos);
}

TEST_CASE("var - enum inference lowers to a ptr singleton binding", "[var][codegen]") {
    auto ir = codegenString(R"(
        enum Color { RED, GREEN, BLUE }
        fn main() -> i32 {
            var c = Color::GREEN;
            if (c == Color::GREEN) { return 1; }
            return 0;
        }
    )");
    REQUIRE(ir.find("@Color$GREEN") != std::string::npos);
}

// ------------------------------------------------------------
// Edge cases — scope, control flow, static, borrows, generics
// ------------------------------------------------------------

TEST_CASE("var - usable in a for-loop initializer", "[var][semantic]") {
    auto result = analyzeString(R"(
        fn main() -> i32 {
            mut var total = 0;
            for (mut var i = 0; i < 5; i++) { total = total + i; }
            return total;
        }
    )");
    REQUIRE_FALSE(result.hadError);
}

TEST_CASE("var - a nested-scope var shadows an outer one", "[var][semantic]") {
    auto result = analyzeString(R"(
        fn main() -> i32 {
            var x = 10;
            { var x = 20; return x; }
        }
    )");
    REQUIRE_FALSE(result.hadError);
}

TEST_CASE("var - self-reference in the initializer is undeclared", "[var][semantic]") {
    StderrCapture cap;
    auto result = analyzeString(R"(
        fn main() -> i32 {
            var x = x + 1;
            return x;
        }
    )");
    REQUIRE(result.hadError);
    REQUIRE(cap.contains("undeclared identifier 'x'"));
}

TEST_CASE("var - a borrow-returning call infers a borrow (auto-deref on read)", "[var][semantic][ref]") {
    auto result = analyzeString(R"(
        fn pick(i32* a) -> i32* { return a; }
        fn main() -> i32 {
            mut i32 n = 42;
            var r = pick(n);   // r : i32* — reads deref to i32
            return r + 0;
        }
    )");
    REQUIRE_FALSE(result.hadError);
}

TEST_CASE("var - a nullable-primitive inference requires unwrap before use as non-null", "[var][semantic][nullable]") {
    // m is inferred i32?; `?:` supplies the non-null fallback. This must analyze cleanly, and the
    // Elvis result must itself be usable as i32.
    auto result = analyzeString(R"(
        fn maybe(bool b) -> i32? { if (b) { return 7; } return null; }
        fn main() -> i32 {
            var m = maybe(true);
            var got = m ?: 0;
            return got;
        }
    )");
    REQUIRE_FALSE(result.hadError);
}

TEST_CASE("var - inside a generic body survives monomorphization", "[var][generic]") {
    auto ir = codegenString(R"(
        fn dup<T>(T v) -> T { var copy = v; return copy; }
        fn main() -> i32 {
            var d = dup<i32>(35);
            return d;
        }
    )");
    // The i32 instantiation is emitted and the inferred local lowers to an i32 alloca.
    REQUIRE(ir.find("dup$i32") != std::string::npos);
    REQUIRE(ir.find("alloca i32") != std::string::npos);
}

// ------------------------------------------------------------
// static var
// ------------------------------------------------------------

TEST_CASE("var - 'static var' lowers to a persistent internal global", "[var][static]") {
    auto ir = codegenString(R"(
        fn counter() -> i32 {
            mut static var n = 0;
            n = n + 1;
            return n;
        }
        fn main() -> i32 { counter(); return counter(); }
    )");
    REQUIRE(ir.find("internal global i32") != std::string::npos);
}

TEST_CASE("var - 'static var' rejects a non-primitive inferred type", "[var][static]") {
    StderrCapture cap;
    auto result = analyzeString(R"(
        class Point { i32 x; Point(i32 a) { this.x = a; } }
        fn f() -> i32 {
            static var p = Point(1);
            return 0;
        }
    )");
    REQUIRE(result.hadError);
    REQUIRE(cap.contains("must have a primitive type"));
}

// ------------------------------------------------------------
// Raw-pointer gate — an inferred ptr obeys --unsafe-ptr
// ------------------------------------------------------------

TEST_CASE("var - a string literal infers `str` (safe, not gated by --unsafe-ptr)", "[var][semantic]") {
    // A string literal is a `str` view, not a raw `ptr`, so `var s = "..."` is allowed even
    // without --unsafe-ptr. (Before the `str` type, a string literal inferred a raw `ptr` and this
    // was gated behind --unsafe-ptr; the `str` type makes string literals safe by default.)
    auto result = analyzeString(R"(
        fn main() -> i32 {
            var s = "literal";
            return 0;
        }
    )", CompilerOptions{});   // allowRawPtr = false
    REQUIRE_FALSE(result.hadError);
}

// ------------------------------------------------------------
// Inferring from a value-returning (sret) method call
// ------------------------------------------------------------

TEST_CASE("var - infers from a method returning its OWN class by value (sret)", "[var][semantic]") {
    // Regression: `fn empty() -> Vec e` returns its own class by value. Its return type was resolved
    // during buildClassInfo *before* Vec was registered in classRegistry, so resolveTypeToken (which
    // only consulted classRegistry) yielded Error — leaving the method's stored returnType Error and
    // silently breaking `var e = v.empty()` inference (the symbol was never declared → "use of
    // undeclared identifier"). The explicit-typed path `Vec e = v.empty()` masked it (it uses the
    // declared type). Fixed by falling back to declaredClassNames_ (a name pre-pass) in
    // resolveTypeToken. See SemanticAnalyzer::resolveTypeToken.
    auto result = analyzeString(R"(
        class Vec {
            mut i32 x;
            Vec() { x = 0; }
            fn empty() -> Vec e { e.x = 42; return e; }
        }
        fn main() -> i32 {
            mut Vec v;
            var e = v.empty();
            return e.x;
        }
    )");
    REQUIRE_FALSE(result.hadError);
}

TEST_CASE("var - inferring from an sret method call yields the correct object type in IR", "[var][codegen]") {
    auto ir = codegenString(R"(
        class Vec {
            mut i32 x;
            Vec() { x = 0; }
            fn empty() -> Vec e { e.x = 42; return e; }
        }
        fn main() -> i32 {
            mut Vec v;
            var e = v.empty();
            return e.x;
        }
    )");
    REQUIRE(ir.find("%Vec") != std::string::npos);          // e is a Vec value object
    REQUIRE(ir.find("@Vec_empty(") != std::string::npos);   // the sret call is emitted
}

TEST_CASE("var - a method return type that forward-references a later class resolves", "[var][semantic]") {
    // The same root cause, exercised through a forward reference (Maker declared before Widget) —
    // resolveTypeToken must resolve `Widget` via the name pre-pass even though Widget's ClassInfo is
    // built after Maker's.
    auto result = analyzeString(R"(
        class Maker {
            fn build() -> Widget w { return w; }
        }
        class Widget {
            mut i32 v;
            Widget() { v = 7; }
        }
        fn main() -> i32 {
            mut Maker m;
            var w = m.build();
            return w.v;
        }
    )");
    REQUIRE_FALSE(result.hadError);
}
