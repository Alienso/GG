#include <catch2/catch_test_macros.hpp>
#include "helpers.h"

// ============================================================
// Lambdas & callable objects. `Call(P…)->R` is a signature-carrying trait bound; `obj(args)` on a
// Call-implementing class desugars to `obj.call(args)`. A generic `<F: Call(…)>` function accepts
// any matching callable (a hand-written impl or a lambda literal). Lambdas desugar in the parser
// to a hoisted value-object class implementing Call, captured by value.
// ============================================================

// ---- Phase A: callable objects + signature-carrying Call bound ----

TEST_CASE("Call - obj(args) desugars to obj.call(args)", "[lambda][call][codegen]") {
    std::string ir = codegenString(R"(
        class Adder { i32 n; Adder(i32 a) { n = a; } }
        impl Call for Adder { fn call(i32 x) -> i32 { return x + n; } }
        fn main() -> i32 { Adder a(5); return a(10); }
    )");
    REQUIRE(ir.find("@Adder_call(") != std::string::npos);
}

TEST_CASE("Call - a callable object flows through a Call-bounded generic", "[lambda][call][semantic]") {
    auto r = analyzeString(R"(
        class Adder { i32 n; Adder(i32 a) { n = a; } }
        impl Call for Adder { fn call(i32 x) -> i32 { return x + n; } }
        fn apply<F: Call(i32) -> i32>(F& f, i32 x) -> i32 { return f(x); }
        fn main() -> i32 { Adder a(5); return apply<Adder>(a, 10); }
    )");
    REQUIRE_FALSE(r.hadError);
}

TEST_CASE("Call - a signature mismatch is rejected by the bound", "[lambda][call][semantic]") {
    StderrCapture cap;
    auto r = analyzeString(R"(
        class Wrong { fn dummy() -> i32 { return 0; } }
        impl Call for Wrong { fn call(f64 x) -> f64 { return x; } }
        fn apply<F: Call(i32) -> i32>(F& f, i32 x) -> i32 { return f(x); }
        fn main() -> i32 { Wrong w; return apply<Wrong>(w, 5); }
    )");
    REQUIRE(r.hadError);
    REQUIRE(cap.contains("does not satisfy bound 'Call(i32) -> i32'"));
}

TEST_CASE("Call - calling a non-callable variable is still an error", "[lambda][call][semantic]") {
    StderrCapture cap;
    auto r = analyzeString(R"(
        fn main() -> i32 { i32 x = 3; return x(1); }
    )");
    REQUIRE(r.hadError);
    REQUIRE(cap.contains("is not a function"));
}

// ---- Phase B/C: lambda literals ----

TEST_CASE("Lambda - a non-capturing lambda through a Call-bounded generic analyzes clean", "[lambda][semantic]") {
    auto r = analyzeString(R"(
        fn apply<F: Call(i32) -> i32>(F& f, i32 x) -> i32 { return f(x); }
        fn main() -> i32 { return apply((i32 y) -> i32 { return y + 1; }, 41); }
    )");
    REQUIRE_FALSE(r.hadError);
}

TEST_CASE("Lambda - a capturing lambda (local) analyzes clean", "[lambda][semantic]") {
    auto r = analyzeString(R"(
        fn apply<F: Call(i32) -> i32>(F& f, i32 x) -> i32 { return f(x); }
        fn main() -> i32 { i32 base = 100; return apply((i32 y) -> i32 { return y + base; }, 5); }
    )");
    REQUIRE_FALSE(r.hadError);
}

TEST_CASE("Lambda - desugars to a value-object class with a call method", "[lambda][codegen]") {
    std::string ir = codegenString(R"(
        fn apply<F: Call(i32) -> i32>(F& f, i32 x) -> i32 { return f(x); }
        fn main() -> i32 { i32 base = 7; return apply((i32 y) -> i32 { return y + base; }, 3); }
    )");
    // The generated lambda class, its call method, and a captured field all appear.
    REQUIRE(ir.find("%__lambda_0 = type") != std::string::npos);
    REQUIRE(ir.find("@__lambda_0_call(") != std::string::npos);
}

TEST_CASE("Lambda - a wrong-arity lambda is rejected by the bound", "[lambda][semantic]") {
    StderrCapture cap;
    auto r = analyzeString(R"(
        fn apply<F: Call(i32) -> i32>(F& f, i32 x) -> i32 { return f(x); }
        fn main() -> i32 { return apply((i32 a, i32 b) -> i32 { return a + b; }, 5); }
    )");
    REQUIRE(r.hadError);
    REQUIRE(cap.contains("does not satisfy bound 'Call(i32) -> i32'"));
}

TEST_CASE("Lambda - a multi-parameter signature analyzes clean", "[lambda][semantic]") {
    auto r = analyzeString(R"(
        fn apply2<F: Call(i32, i32) -> i32>(F& f, i32 a, i32 b) -> i32 { return f(a, b); }
        fn main() -> i32 { return apply2((i32 p, i32 q) -> i32 { return p * q; }, 6, 7); }
    )");
    REQUIRE_FALSE(r.hadError);
}

TEST_CASE("Lambda - a value-object parameter in a Call signature is rejected", "[lambda][semantic]") {
    StderrCapture cap;
    auto r = analyzeString(R"(
        class Point { mut i32 x; Point(i32 v) { x = v; } }
        fn apply<F: Call(Point) -> i32>(F& f, Point& p) -> i32 { return f(p); }
        fn main() -> i32 { Point p(3); return apply((Point q) -> i32 { return q.x; }, p); }
    )");
    REQUIRE(r.hadError);
    REQUIRE(cap.contains("must be passed by reference"));
}

TEST_CASE("Lambda - untyped parameters infer from the Call bound", "[lambda][semantic]") {
    auto r = analyzeString(R"(
        fn apply<F: Call(i32) -> i32>(F& f, i32 x) -> i32 { return f(x); }
        fn main() -> i32 {
            i32 base = 100;
            i32 a = apply(y -> { return y + base; }, 7);      // bare single untyped param
            i32 b = apply((y) -> { return y * 2; }, 3);       // parenthesized single
            return a + b;
        }
    )");
    REQUIRE_FALSE(r.hadError);
}

TEST_CASE("Lambda - untyped multi-parameter infers from the Call bound", "[lambda][semantic]") {
    auto r = analyzeString(R"(
        fn apply2<F: Call(i32, i32) -> i32>(F& f, i32 a, i32 b) -> i32 { return f(a, b); }
        fn main() -> i32 { return apply2((p, q) -> { return p + q; }, 6, 7); }
    )");
    REQUIRE_FALSE(r.hadError);
}

TEST_CASE("Lambda - an untyped lambda with no inferable signature is rejected", "[lambda][parser]") {
    StderrCapture cap;
    parseString(R"(
        fn main() -> i32 { i32 z = (y) -> { return y; }; return z; }
    )");
    REQUIRE(cap.contains("cannot infer this lambda's parameter types"));
}

TEST_CASE("Lambda - a bare-identifier case label is not mistaken for a lambda", "[lambda][switch]") {
    // `case hi -> …` must parse `hi` as the label, not a lambda `hi -> …`.
    auto r = analyzeString(R"(
        class Money { mut i32 c; Money(i32 v) { c = v; } }
        impl Eq for Money { fn eq(Money& o) -> bool { return c == o.c; } }
        fn main() -> i32 {
            Money p(5); Money hi(5);
            i32 label = switch (p) { case hi -> 2; default -> 0; };
            return label;
        }
    )");
    REQUIRE_FALSE(r.hadError);
}

TEST_CASE("Lambda - a nested lambda is rejected", "[lambda][parser]") {
    StderrCapture cap;
    parseString(R"(
        fn apply<F: Call(i32) -> i32>(F& f, i32 x) -> i32 { return f(x); }
        fn main() -> i32 {
            return apply((i32 y) -> i32 { return apply((i32 z) -> i32 { return z; }, y); }, 1);
        }
    )");
    REQUIRE(cap.contains("nested lambdas are not supported"));
}
