#include <catch2/catch_test_macros.hpp>
#include "helpers.h"

// ============================================================
// Overloading — free functions, constructors, and methods, distinguished by
// parameter signature and by return type. Enums are excluded.
// ============================================================

// ------------------------------------------------------------
// Semantic — accepted
// ------------------------------------------------------------

TEST_CASE("Overload - free functions by arity and by type", "[overload][semantic]") {
    auto result = analyzeString(R"(
        fn add(i32 a, i32 b) -> i32        { return a + b; }
        fn add(i32 a, i32 b, i32 c) -> i32 { return a + b + c; }
        fn add(f64 a, f64 b) -> f64        { return a + b; }
        fn main() -> i32 {
            i32 x = add(1, 2);
            i32 y = add(1, 2, 3);
            f64 z = add(1.0, 2.0);
            return 0;
        }
    )");
    REQUIRE_FALSE(result.hadError);
}

TEST_CASE("Overload - best match prefers widening over narrowing", "[overload][semantic]") {
    auto result = analyzeString(R"(
        fn f(i32 a, i32 b) -> i32 { return a + b; }
        fn f(f64 a, f64 b) -> f64 { return a + b; }
        fn main() -> i32 { f64 r = f(1, 2.0); return 0; }   // picks f(f64,f64)
    )");
    REQUIRE_FALSE(result.hadError);
}

TEST_CASE("Overload - constructor overloading (new and stack)", "[overload][semantic]") {
    auto result = analyzeString(R"(
        class Vec {
            mut i32 x; mut i32 y;
            Vec()             { x = 0; y = 0; }
            Vec(i32 v)        { x = v; y = v; }
            Vec(i32 a, i32 b) { x = a; y = b; }
            fn sum() -> i32 { return x + y; }
        }
        fn main() -> i32 {
            Vec& a = new Vec();
            Vec& b = new Vec(5);
            mut Vec c(1, 2);
            return c.sum();
        }
    )");
    REQUIRE_FALSE(result.hadError);
}

TEST_CASE("Overload - instance and static method overloading", "[overload][semantic]") {
    auto result = analyzeString(R"(
        class C {
            fn combine(i32 k) -> i32        { return k; }
            fn combine(i32 k, i32 m) -> i32 { return k + m; }
            fn static mk() -> i32           { return 0; }
            fn static mk(i32 b) -> i32      { return b; }
        }
        fn main() -> i32 {
            C& c = new C();
            i32 a = c.combine(1);
            i32 b = c.combine(1, 2);
            i32 d = C::mk();
            i32 e = C::mk(9);
            return 0;
        }
    )");
    REQUIRE_FALSE(result.hadError);
}

TEST_CASE("Overload - return-type overloading selected by context", "[overload][semantic]") {
    auto result = analyzeString(R"(
        fn make() -> i32 { return 7; }
        fn make() -> f64 { return 2.5; }
        fn main() -> i32 {
            i32 a = make();          // context → i32 overload
            f64 b = make();          // context → f64 overload
            i32 c = make() as i32;   // cast → i32 overload
            return 0;
        }
    )");
    REQUIRE_FALSE(result.hadError);
}

TEST_CASE("Overload - resolution by object/reference parameter type", "[overload][semantic]") {
    auto result = analyzeString(R"(
        class Point { i32 x; Point(i32 x) { this.x = x; } fn get() -> i32 { return this.x; } }
        fn describe(Point& p) -> i32 { return p.get(); }
        fn describe(i32 n) -> i32    { return n; }
        fn main() -> i32 {
            Point& p = new Point(5);
            return describe(p) + describe(9);
        }
    )");
    REQUIRE_FALSE(result.hadError);
}

TEST_CASE("Overload - overloaded method call via implicit this", "[overload][semantic]") {
    auto result = analyzeString(R"(
        class C {
            fn f(i32 a) -> i32        { return a; }
            fn f(i32 a, i32 b) -> i32 { return a + b; }
            fn g() -> i32             { return f(1) + f(2, 3); }   // bare overloaded calls on this
        }
    )");
    REQUIRE_FALSE(result.hadError);
}

TEST_CASE("Overload - return-type selected by assignment and return context", "[overload][semantic]") {
    auto result = analyzeString(R"(
        fn pick() -> i32 { return 3; }
        fn pick() -> f64 { return 9.5; }
        fn viaReturn() -> f64 { return pick(); }        // return context → f64
        fn main() -> i32 {
            mut i32 a;
            a = pick();                           // assignment context → i32
            return a;
        }
    )");
    REQUIRE_FALSE(result.hadError);
}

TEST_CASE("Overload - a nested overloaded call as an argument resolves", "[overload][semantic]") {
    auto result = analyzeString(R"(
        fn id(i32 x) -> i32 { return x; }
        fn id(f64 x) -> i32 { return 0; }
        fn main() -> i32 { return id(id(7)); }
    )");
    REQUIRE_FALSE(result.hadError);
}

// ------------------------------------------------------------
// Semantic — errors
// ------------------------------------------------------------

TEST_CASE("Overload - equal-cost implicit conversions are ambiguous", "[overload][semantic]") {
    StderrCapture cap;
    auto result = analyzeString(R"(
        fn f(i32 a) -> i32 { return a; }
        fn f(f64 a) -> f64 { return 0.0; }
        fn main() -> i32 {
            i8 s = 3;
            f(s);   // i8→i32 and i8→f64 are both silent widenings → tie → ambiguous
            return 0;
        }
    )");
    REQUIRE(result.hadError);
    REQUIRE(cap.contains("ambiguous"));
}

TEST_CASE("Overload - no matching overload is an error", "[overload][semantic]") {
    StderrCapture cap;
    auto result = analyzeString(R"(
        class C { i32 z; }
        fn k(i32 a) -> i32 { return a; }
        fn main() -> i32 { C& c = new C(); return k(c); }
    )");
    REQUIRE(result.hadError);
    REQUIRE(cap.contains("no matching overload"));
}

TEST_CASE("Overload - ambiguous return-type call with no context is an error", "[overload][semantic]") {
    StderrCapture cap;
    auto result = analyzeString(R"(
        fn make() -> i32 { return 0; }
        fn make() -> f64 { return 1.0; }
        fn main() -> i32 { make(); return 0; }   // no expected type → ambiguous
    )");
    REQUIRE(result.hadError);
    REQUIRE(cap.contains("ambiguous"));
}

TEST_CASE("Overload - identical signature is a redefinition error", "[overload][semantic]") {
    StderrCapture cap;
    auto result = analyzeString(R"(
        fn g(i32 a) -> i32 { return a; }
        fn g(i32 b) -> i32 { return b; }
    )");
    REQUIRE(result.hadError);
    REQUIRE(cap.contains("already defined with the same signature"));
}

TEST_CASE("Overload - extern cannot be overloaded", "[overload][semantic]") {
    StderrCapture cap;
    auto result = analyzeString(R"(
        extern ext(ptr s) -> i32;
        extern ext(i32 x) -> i32;
    )");
    REQUIRE(result.hadError);
    REQUIRE(cap.contains("cannot be overloaded"));
}

TEST_CASE("Overload - main cannot be overloaded", "[overload][semantic]") {
    StderrCapture cap;
    auto result = analyzeString(R"(
        fn main() -> i32 { return 0; }
        fn main(i32 x) -> i32 { return x; }
    )");
    REQUIRE(result.hadError);
    REQUIRE(cap.contains("'main' cannot be overloaded"));
}

TEST_CASE("Overload - enums cannot overload constructors", "[overload][semantic]") {
    StderrCapture cap;
    auto result = analyzeString(R"(
        enum E {
            A(1);
            i32 v;
            E(i32 x)        { this.v = x; }
            E(i32 a, i32 b) { this.v = a; }
        }
    )");
    REQUIRE(result.hadError);
    REQUIRE(cap.contains("cannot overload"));
}

// ------------------------------------------------------------
// Codegen — mangling only when overloaded
// ------------------------------------------------------------

TEST_CASE("Overload - a non-overloaded function keeps its plain symbol name", "[overload][codegen]") {
    std::string ir = codegenString(R"(
        fn solo(i32 a) -> i32 { return a; }
        fn main() -> i32 { return solo(3); }
    )");
    REQUIRE(ir.find("@solo(") != std::string::npos);        // plain, no mangling
    REQUIRE(ir.find("@solo$") == std::string::npos);
}

TEST_CASE("Overload - overloaded functions emit distinct mangled symbols", "[overload][codegen]") {
    std::string ir = codegenString(R"(
        fn f(i32 a) -> i32 { return a; }
        fn f(f64 a) -> i32 { return 0; }
        fn main() -> i32 { return f(1) + (f(2.0)); }
    )");
    REQUIRE(ir.find("@f$i32$ret$i32(") != std::string::npos);
    REQUIRE(ir.find("@f$f64$ret$i32(") != std::string::npos);
    // No un-mangled @f( definition/call remains.
    REQUIRE(ir.find("@f(") == std::string::npos);
}

TEST_CASE("Overload - overloaded constructors emit distinct mangled symbols", "[overload][codegen]") {
    std::string ir = codegenString(R"(
        class Vec {
            mut i32 x;
            Vec()      { x = 0; }
            Vec(i32 v) { x = v; }
        }
        fn main() -> i32 { Vec& a = new Vec(); Vec& b = new Vec(5); return 0; }
    )");
    REQUIRE(ir.find("@Vec_Vec$ret$void(") != std::string::npos);       // zero-arg ctor
    REQUIRE(ir.find("@Vec_Vec$i32$ret$void(") != std::string::npos);   // one-arg ctor
}

TEST_CASE("Overload - reference-parameter overload mangles with .ref", "[overload][codegen]") {
    std::string ir = codegenString(R"(
        class Point { i32 x; Point(i32 x) { this.x = x; } fn get() -> i32 { return this.x; } }
        fn d(Point& p) -> i32 { return p.get(); }
        fn d(i32 n) -> i32    { return n; }
        fn main() -> i32 { Point& p = new Point(1); return d(p) + d(2); }
    )");
    REQUIRE(ir.find("@d$Point.ref$ret$i32(") != std::string::npos);
    REQUIRE(ir.find("@d$i32$ret$i32(") != std::string::npos);
}
