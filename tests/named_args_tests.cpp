#include <catch2/catch_test_macros.hpp>
#include "helpers.h"

// ============================================================
// Named arguments — `f(x: 1, y: 2)`
// ============================================================
// A call may pass arguments by parameter name. Mixed calls are allowed with named args following
// a positional prefix. Names participate in overload selection (an arg naming a parameter the
// candidate lacks makes it non-viable). Defaults may now sit on any parameter, and a named call
// can skip a defaulted middle param. The reordering is a semantic→codegen handoff; a purely
// positional call is unaffected.

// ---- Parser ----

TEST_CASE("Named - a named argument records its parameter name", "[named][parser]") {
    auto prog = parseStringRaw(R"(
        fn f(i32 a, i32 b) -> i32 { return a; }
        fn main() -> i32 { return f(b: 2, a: 1); }
    )");
    REQUIRE(prog.declarations.size() == 2);
    const auto& main = asStmt<FunctionDeclStmt>(prog.declarations[1]);
    const auto& ret  = std::get<ReturnStmt>(*main.body.body[0]->node);
    const auto& call = asExpr<CallExpr>(*ret.value);
    REQUIRE(call.argNames.size() == 2);
    REQUIRE(call.argNames[0].lexeme == "b");
    REQUIRE(call.argNames[1].lexeme == "a");
}

TEST_CASE("Named - a positional argument after a named one is a parse error", "[named][parser]") {
    StderrCapture cap;
    parseString("fn f(i32 a, i32 b) -> i32 { return a; } fn main() -> i32 { return f(a: 1, 2); }");
    REQUIRE(cap.contains("positional argument cannot follow a named argument"));
}

// ---- Semantic: accepted ----

TEST_CASE("Named - all-named reordered call type-checks", "[named][semantic]") {
    auto r = analyzeString(R"(
        fn sub(i32 a, i32 b) -> i32 { return a - b; }
        fn main() -> i32 { return sub(b: 3, a: 10); }
    )");
    REQUIRE_FALSE(r.hadError);
}

TEST_CASE("Named - mixed positional + named call type-checks", "[named][semantic]") {
    auto r = analyzeString(R"(
        fn sub(i32 a, i32 b) -> i32 { return a - b; }
        fn main() -> i32 { return sub(10, b: 4); }
    )");
    REQUIRE_FALSE(r.hadError);
}

TEST_CASE("Named - a named arg can skip a defaulted middle parameter", "[named][semantic]") {
    auto r = analyzeString(R"(
        fn span(i32 lo, i32 step = 10, i32 hi) -> i32 { return (hi - lo) * step; }
        fn main() -> i32 { return span(1, hi: 3); }
    )");
    REQUIRE_FALSE(r.hadError);
}

TEST_CASE("Named - names participate in overload selection", "[named][semantic]") {
    auto r = analyzeString(R"(
        fn show(i32 count) -> i32 { return count; }
        fn show(bool flag) -> i32 { return 0; }
        fn main() -> i32 { return show(flag: true) + show(count: 3); }
    )");
    REQUIRE_FALSE(r.hadError);
}

TEST_CASE("Named - a method call accepts named arguments", "[named][semantic]") {
    auto r = analyzeString(R"(
        class Box { mut i32 w; mut i32 h; Box() { w = 0; h = 0; }
                    fn set(i32 width, i32 height) mut { w = width; h = height; } }
        fn main() -> i32 {
            mut Box b();
            b.set(height: 4, width: 3);
            return b.w;
        }
    )");
    REQUIRE_FALSE(r.hadError);
}

TEST_CASE("Named - a constructor via new accepts named arguments", "[named][semantic]") {
    auto r = analyzeString(R"(
        class P { mut i32 x; mut i32 y; P(i32 a, i32 b) { x = a; y = b; } }
        fn main() -> i32 {
            P& p = new P(b: 2, a: 1);
            return p.x;
        }
    )");
    REQUIRE_FALSE(r.hadError);
}

// ---- Semantic: rejected ----

TEST_CASE("Named - an unknown parameter name is an error", "[named][semantic]") {
    StderrCapture cap;
    auto r = analyzeString(R"(
        fn f(i32 a, i32 b) -> i32 { return a; }
        fn main() -> i32 { return f(a: 1, z: 2); }
    )");
    REQUIRE(r.hadError);
    REQUIRE(cap.contains("unknown parameter name 'z'"));
}

TEST_CASE("Named - a duplicated parameter name is an error", "[named][semantic]") {
    StderrCapture cap;
    auto r = analyzeString(R"(
        fn f(i32 a, i32 b) -> i32 { return a; }
        fn main() -> i32 { return f(a: 1, a: 2); }
    )");
    REQUIRE(r.hadError);
    REQUIRE(cap.contains("'a'"));
}

TEST_CASE("Named - naming a parameter already given positionally is an error", "[named][semantic]") {
    StderrCapture cap;
    auto r = analyzeString(R"(
        fn f(i32 a, i32 b) -> i32 { return a; }
        fn main() -> i32 { return f(1, a: 2); }
    )");
    REQUIRE(r.hadError);
    REQUIRE(cap.contains("'a'"));
}

TEST_CASE("Named - a missing required parameter is an error", "[named][semantic]") {
    StderrCapture cap;
    auto r = analyzeString(R"(
        fn f(i32 a, i32 b) -> i32 { return a; }
        fn main() -> i32 { return f(a: 1); }
    )");
    REQUIRE(r.hadError);
    REQUIRE(cap.contains("required parameter"));
}

// ---- Codegen: reordering ----

TEST_CASE("Named - reordered arguments are emitted in parameter order", "[named][codegen]") {
    // sub(b: 3, a: 10) must call @sub with a=10 first, then b=3 — the parameter order, not the
    // written order.
    std::string ir = codegenString(R"(
        fn sub(i32 a, i32 b) -> i32 { return a - b; }
        fn main() -> i32 { return sub(b: 3, a: 10); }
    )");
    REQUIRE(ir.find("call i32 @sub(i32 10, i32 3)") != std::string::npos);
}

TEST_CASE("Named - a skipped defaulted middle param is filled by its default in IR", "[named][codegen]") {
    // span(1, hi: 3) → lo=1, step defaults to 10, hi=3 → @span(i32 1, i32 10, i32 3).
    std::string ir = codegenString(R"(
        fn span(i32 lo, i32 step = 10, i32 hi) -> i32 { return (hi - lo) * step; }
        fn main() -> i32 { return span(1, hi: 3); }
    )");
    REQUIRE(ir.find("call i32 @span(i32 1, i32 10, i32 3)") != std::string::npos);
}

TEST_CASE("Named - a purely positional call is unchanged (no reordering)", "[named][codegen]") {
    std::string ir = codegenString(R"(
        fn sub(i32 a, i32 b) -> i32 { return a - b; }
        fn main() -> i32 { return sub(10, 3); }
    )");
    REQUIRE(ir.find("call i32 @sub(i32 10, i32 3)") != std::string::npos);
}

// ---- Edge cases ----

TEST_CASE("Named - a generic function accepts and reorders named arguments", "[named][generic][codegen]") {
    // The monomorphized `pick$i32` gets its param names from the re-parsed concrete decl.
    std::string ir = codegenString(R"(
        fn pick<T>(T first, T second) -> T { return first; }
        fn main() -> i32 { return pick<i32>(second: 2, first: 9); }
    )");
    REQUIRE(ir.find("@pick$i32(i32 9, i32 2)") != std::string::npos);
}

TEST_CASE("Named - a static method accepts named arguments (reordered in IR)", "[named][codegen]") {
    std::string ir = codegenString(R"(
        class Calc { fn static combine(i32 a, i32 b) -> i32 { return a * 10 + b; } }
        fn main() -> i32 { return Calc::combine(b: 3, a: 4); }
    )");
    REQUIRE(ir.find("@Calc_combine(i32 4, i32 3)") != std::string::npos);
}

TEST_CASE("Named - reference arguments reorder correctly", "[named][semantic]") {
    auto r = analyzeString(R"(
        class Point { mut i32 x; Point(i32 v) { x = v; } }
        fn diff(Point& p, Point& q) -> i32 { return p.x - q.x; }
        fn main() -> i32 {
            Point& a = new Point(7);
            Point& b = new Point(2);
            return diff(q: b, p: a);
        }
    )");
    REQUIRE_FALSE(r.hadError);
}

TEST_CASE("Named - an untyped brace initializer works as a named argument", "[named][brace][semantic]") {
    auto r = analyzeString(R"(
        class Point { mut i32 x; mut i32 y; Point(i32 a, i32 b) { x = a; y = b; } }
        fn takes(Point* p) -> i32 { return p.x; }
        fn main() -> i32 { return takes(p: {5, 6}); }
    )");
    REQUIRE_FALSE(r.hadError);
}

TEST_CASE("Named - an object-returning (sret) call accepts named arguments", "[named][semantic]") {
    auto r = analyzeString(R"(
        class Point { mut i32 x; mut i32 y; }
        fn build(i32 a, i32 b) -> Point p { p.x = a; p.y = b; }
        fn main() -> i32 { Point r = build(b: 2, a: 9); return r.x; }
    )");
    REQUIRE_FALSE(r.hadError);
}

TEST_CASE("Named - a var-decl constructor call accepts named arguments", "[named][semantic]") {
    auto r = analyzeString(R"(
        class Point { mut i32 x; mut i32 y; Point(i32 a, i32 b) { x = a; y = b; } }
        fn main() -> i32 { Point p(b: 2, a: 1); return p.x; }
    )");
    REQUIRE_FALSE(r.hadError);
}

TEST_CASE("Named - a const->mut coercion still warns for a named mut-reference argument", "[named][cast][semantic]") {
    // The per-argument diagnostics are keyed by parameter slot, so the warning must still fire
    // when the argument is passed by name.
    StderrCapture cap;
    auto r = analyzeString(R"(
        class Point { mut i32 x; Point(i32 v) { x = v; } }
        fn mutate(mut Point& p, i32 n) { p.x = n; }
        fn main() -> i32 {
            Point& b = new Point(1);
            mutate(n: 5, p: b);
            return 0;
        }
    )");
    REQUIRE_FALSE(r.hadError);
    REQUIRE(cap.contains("read-only (const) reference into a 'mut' binding"));
}

TEST_CASE("Named - a named call may omit a trailing default too", "[named][semantic]") {
    auto r = analyzeString(R"(
        fn f(i32 a, i32 b = 5, i32 c = 6) -> i32 { return a + b + c; }
        fn main() -> i32 { return f(b: 2, a: 1); }
    )");
    REQUIRE_FALSE(r.hadError);
}

TEST_CASE("Named - escape analysis still fires for a named value-object argument", "[named][escape][semantic]") {
    // Passing a stack value object to an escaping reference parameter must be rejected even when
    // the argument is named (the escape check is keyed by parameter slot).
    StderrCapture cap;
    auto r = analyzeString(R"(
        class Point { mut i32 x; Point(i32 v) { x = v; } }
        fn keep(i32 tag, Point& a) -> Point& { return a; }
        fn main() -> i32 {
            Point v(3);
            Point& b = keep(a: v, tag: 1);
            return 0;
        }
    )");
    REQUIRE(r.hadError);
    REQUIRE(cap.contains("escapes"));
}

TEST_CASE("Named - named arguments are rejected on a callable-object sugar call", "[named][semantic]") {
    StderrCapture cap;
    auto r = analyzeString(R"(
        class Adder { i32 n; Adder(i32 v) { n = v; } }
        impl Call for Adder { fn call(i32 x, i32 y) -> i32 { return x - y + n; } }
        fn main() -> i32 {
            Adder add(100);
            return add(y: 3, x: 10);
        }
    )");
    REQUIRE(r.hadError);
    REQUIRE(cap.contains("named arguments are not supported on a callable-object call"));
}

TEST_CASE("Named - a genuinely ambiguous named call is an error", "[named][semantic]") {
    // Equal conversion cost on both overloads, same return type → no tiebreak → ambiguous.
    StderrCapture cap;
    auto r = analyzeString(R"(
        fn amb(i32 a, f64 b) -> i32 { return 1; }
        fn amb(f64 a, i32 b) -> i32 { return 2; }
        fn main() -> i32 { return amb(a: 1, b: 2); }
    )");
    REQUIRE(r.hadError);
    REQUIRE(cap.contains("ambiguous"));
}

TEST_CASE("Named - return-type overloads are disambiguated by expected type through a named arg", "[named][codegen]") {
    // `i64 b = conv(x: 5)` must select the i64-returning overload (chosen by contextual type).
    std::string ir = codegenString(R"(
        fn conv(i32 x) -> i32 { return x + 1; }
        fn conv(i32 x) -> i64 { return (x as i64) + 100; }
        fn main() -> i32 { i64 b = conv(x: 5); return b as i32; }
    )");
    REQUIRE(ir.find("call i64 @conv$i32$ret$i64") != std::string::npos);
}

TEST_CASE("Named - per-slot casting follows the reordered argument to its parameter", "[named][codegen]") {
    // widthed(b: 7, a: 5): the emitted call is `@widthed(i64 <..>, i8 <..>)` — the argument
    // operands are typed in PARAMETER order (i64 then i8), so each cast tracked its own slot
    // despite the reversed written order. (Runtime value-correctness — 5007 — is covered in
    // e2e/named_args_test.gg check 8, which can't be asserted from IR because the operands are
    // cast temps, not inlined literals.)
    std::string ir = codegenString(R"(
        fn widthed(i64 a, i8 b) -> i64 { return a; }
        fn main() -> i64 { return widthed(b: 7, a: 5); }
    )");
    auto call = ir.find("call i64 @widthed(i64 ");
    REQUIRE(call != std::string::npos);
    REQUIRE(ir.find(", i8 ", call) != std::string::npos);   // second operand typed i8 (slot 1)
}

TEST_CASE("Named - an implicit-this method call accepts named arguments", "[named][semantic]") {
    auto r = analyzeString(R"(
        class Counter {
            mut i32 n;
            Counter() { n = 0; }
            fn setTo(i32 base, i32 step) mut { n = base + step; }
            fn bump() mut { this.setTo(step: 2, base: 100); }
        }
        fn main() -> i32 { mut Counter c(); c.bump(); return c.n; }
    )");
    REQUIRE_FALSE(r.hadError);
}

TEST_CASE("Named - a callable object accepts named arguments via an explicit .call", "[named][semantic]") {
    auto r = analyzeString(R"(
        class Adder { i32 n; Adder(i32 v) { n = v; } }
        impl Call for Adder { fn call(i32 x, i32 y) -> i32 { return x - y + n; } }
        fn main() -> i32 {
            Adder add(100);
            return add.call(y: 3, x: 10);
        }
    )");
    REQUIRE_FALSE(r.hadError);
}
