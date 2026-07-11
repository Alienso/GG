#include <catch2/catch_test_macros.hpp>
#include "helpers.h"

// ============================================================
// Brace construction `Type{args}` — an alternate delimiter for a positional constructor call
// (same overload resolution). Two forms:
//   - typed    `Point{1, 2}`  ≡  `Point(1, 2)`        (parses to an ordinary constructor CallExpr)
//   - untyped  `{1, 2}`       — class deduced from the expected type at the use site
//                               (constructor argument, var initializer, or return)
// Value objects, `new`, and embedded/nested value fields. Enums are not constructable.
// ============================================================

// ---- typed `Type{...}` ----

TEST_CASE("BraceInit - typed value-object local constructs via the constructor", "[brace][codegen]") {
    std::string ir = codegenString(R"(
        class Point { mut i32 x; mut i32 y; Point(i32 a, i32 b) { x = a; y = b; } }
        fn main() -> i32 { Point p{3, 4}; return p.x + p.y; }
    )");
    // Same lowering as Point p(3,4): the constructor runs on the local's storage.
    REQUIRE(ir.find("call void @Point_Point(ptr %p.addr, i32 3, i32 4)") != std::string::npos);
}

TEST_CASE("BraceInit - `new Point{...}` constructs on the heap", "[brace][codegen]") {
    std::string ir = codegenString(R"(
        class Point { mut i32 x; mut i32 y; Point(i32 a, i32 b) { x = a; y = b; } }
        fn main() -> i32 { Point& r = new Point{5, 6}; return r.x; }
    )");
    REQUIRE(ir.find("@gg_alloc") != std::string::npos);
    REQUIRE(ir.find("@Point_Point(") != std::string::npos);
}

TEST_CASE("BraceInit - typed brace as a function argument", "[brace][semantic]") {
    auto r = analyzeString(R"(
        class Point { mut i32 x; mut i32 y; Point(i32 a, i32 b) { x = a; y = b; } }
        fn use(Point& p) -> i32 { return p.x; }
        fn main() -> i32 { return use(Point{1, 2}); }
    )");
    REQUIRE_FALSE(r.hadError);
}

// ---- untyped `{...}` deduced from context ----

TEST_CASE("BraceInit - untyped nested braces deduce the embedded class", "[brace][semantic]") {
    auto r = analyzeString(R"(
        class Point { mut i32 x; mut i32 y; Point(i32 a, i32 b) { x = a; y = b; } }
        class Line { mut Point start; mut Point end; Line(Point& a, Point& b) { start = a; end = b; } }
        fn main() -> i32 {
            Line l{ {1, 2}, {10, 20} };
            return l.start.x + l.end.y;
        }
    )");
    REQUIRE_FALSE(r.hadError);
}

TEST_CASE("BraceInit - untyped brace argument deduces from the parameter type", "[brace][codegen]") {
    std::string ir = codegenString(R"(
        class Point { mut i32 x; mut i32 y; Point(i32 a, i32 b) { x = a; y = b; } }
        fn use(Point& p) -> i32 { return p.x; }
        fn main() -> i32 { return use({7, 8}); }
    )");
    // The untyped brace lowered to a real Point construction.
    REQUIRE(ir.find("call void @Point_Point(") != std::string::npos);
}

TEST_CASE("BraceInit - untyped brace in a var initializer deduces from the declared type", "[brace][semantic]") {
    auto r = analyzeString(R"(
        class Point { mut i32 x; mut i32 y; Point(i32 a, i32 b) { x = a; y = b; } }
        fn main() -> i32 { Point p = {7, 8}; return p.x + p.y; }
    )");
    REQUIRE_FALSE(r.hadError);
}

TEST_CASE("BraceInit - typed braces select the right constructor overload", "[brace][semantic]") {
    auto r = analyzeString(R"(
        class P { mut i32 x; mut i32 y; P(i32 a) { x = a; y = 0; } P(i32 a, i32 b) { x = a; y = b; } }
        fn main() -> i32 { P p{7}; P q{3, 4}; return p.x + q.y; }
    )");
    REQUIRE_FALSE(r.hadError);
}

TEST_CASE("BraceInit - empty braces on a class with no constructor zero-initialize", "[brace][codegen]") {
    // Regression: `C c{}` (or `C c()`) on a ctor-less class must NOT emit a call to a
    // non-existent @C_C; it zero-initializes like `C c;`.
    std::string ir = codegenString(R"(
        class C { mut i32 n; }
        fn main() -> i32 { C c{}; return c.n; }
    )");
    REQUIRE(ir.find("store %C zeroinitializer") != std::string::npos);
    REQUIRE(ir.find("@C_C(") == std::string::npos);
}

TEST_CASE("BraceInit - untyped brace in a return (object alias) deduces the return type", "[brace][semantic]") {
    auto r = analyzeString(R"(
        class P { mut i32 x; mut i32 y; P(i32 a, i32 b) { x = a; y = b; } }
        fn make() -> P out { out = {6, 7}; return out; }
        fn main() -> i32 { P p = make(); return p.x + p.y; }
    )");
    REQUIRE_FALSE(r.hadError);
}

TEST_CASE("BraceInit - untyped brace deduces a `ref` (borrow) parameter type", "[brace][semantic]") {
    auto r = analyzeString(R"(
        class P { mut i32 x; P(i32 a) { x = a; } }
        fn peek(ref P p) -> i32 { return p.x; }
        fn main() -> i32 { return peek({99}); }
    )");
    REQUIRE_FALSE(r.hadError);
}

// ---- rejected ----

TEST_CASE("BraceInit - untyped brace to an overloaded function is rejected (ambiguous target)", "[brace][semantic]") {
    // The class can't be deduced because the enclosing call is overloaded — spell the type.
    StderrCapture cap;
    auto r = analyzeString(R"(
        class P { mut i32 x; P(i32 a) { x = a; } }
        class Q { mut i32 y; Q(i32 a) { y = a; } }
        fn f(P& p) -> i32 { return p.x; }
        fn f(Q& q) -> i32 { return q.y; }
        fn main() -> i32 { return f({5}); }
    )");
    REQUIRE(r.hadError);
    REQUIRE(cap.contains("cannot infer the type"));
}

TEST_CASE("BraceInit - nested untyped brace with wrong arity is rejected", "[brace][semantic]") {
    StderrCapture cap;
    auto r = analyzeString(R"(
        class Point { mut i32 x; mut i32 y; Point(i32 a, i32 b) { x = a; y = b; } }
        class Line { mut Point s; mut Point e; Line(Point& a, Point& b) { s = a; e = b; } }
        fn main() -> i32 { Line l{ {1}, {2, 3} }; return 0; }
    )");
    REQUIRE(r.hadError);
    REQUIRE(cap.contains("argument"));
}

// ---- previously present rejected cases ----

TEST_CASE("BraceInit - untyped brace with no type context is rejected", "[brace][semantic]") {
    StderrCapture cap;
    auto r = analyzeString(R"(
        fn main() -> i32 { i32 y = {1, 2}; return y; }
    )");
    REQUIRE(r.hadError);
    REQUIRE(cap.contains("cannot infer the type"));
}

TEST_CASE("BraceInit - brace construction of an enum is rejected", "[brace][semantic]") {
    StderrCapture cap;
    auto r = analyzeString(R"(
        enum Color { RED, GREEN }
        fn use(Color c) -> i32 { return 0; }
        fn main() -> i32 { return use({1}); }
    )");
    REQUIRE(r.hadError);
    REQUIRE(cap.contains("enum"));
}

TEST_CASE("BraceInit - wrong argument count to a brace constructor is rejected", "[brace][semantic]") {
    StderrCapture cap;
    auto r = analyzeString(R"(
        class Point { mut i32 x; mut i32 y; Point(i32 a, i32 b) { x = a; y = b; } }
        fn main() -> i32 { Point p{1}; return p.x; }
    )");
    REQUIRE(r.hadError);
}
