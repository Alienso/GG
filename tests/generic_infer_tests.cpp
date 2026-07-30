#include <catch2/catch_test_macros.hpp>
#include "helpers.h"

// ============================================================
// Generic type-argument DEDUCTION — `f(x)` inferring `f<T>(x)`
// ============================================================
// A generic free function called WITHOUT explicit `<…>` deduces its type parameters from the
// argument types. v1: a type parameter appearing as a top-level parameter type (`T` / `T&` / `T*`
// / `T?`) is matched against a positional argument whose type is syntactically known — an in-scope
// identifier, a `Class(...)` constructor call, or `new Class(...)`. Deduction is all-or-nothing;
// an un-inferable call is a clear parse-time error, not a confusing later failure. Inference runs
// in the parser (monomorphization), so the resolved call reaches the same mangled instantiation as
// the explicit form.

// ------------------------------------------------------------
// Successful deduction
// ------------------------------------------------------------

TEST_CASE("GenInfer - type argument inferred from an object identifier", "[geninfer]") {
    auto ir = codegenString(R"(
        class Box { mut i32 v; Box(i32 x) { this.v = x; } }
        fn unwrap<T>(T& b) -> i32 { return b.v; }
        fn main() -> i32 { mut Box b = Box(7); return unwrap(b); }
    )");
    REQUIRE(ir.find("unwrap$Box") != std::string::npos);   // same instantiation as unwrap<Box>(b)
}

TEST_CASE("GenInfer - inferred call matches the explicit call", "[geninfer]") {
    // `unwrap(b)` and `unwrap<Box>(b)` must resolve to the SAME mangled function (one instantiation).
    auto ir = codegenString(R"(
        class Box { mut i32 v; Box(i32 x) { this.v = x; } }
        fn unwrap<T>(T& b) -> i32 { return b.v; }
        fn main() -> i32 { mut Box b = Box(1); return unwrap(b) + unwrap<Box>(b); }
    )");
    // Exactly one definition of unwrap$Box is emitted (deduped by the instantiation set).
    size_t first = ir.find("define i32 @unwrap$Box(");
    REQUIRE(first != std::string::npos);
    REQUIRE(ir.find("define i32 @unwrap$Box(", first + 1) == std::string::npos);
}

TEST_CASE("GenInfer - inferred from a constructor rvalue and from `new`", "[geninfer]") {
    auto ir = codegenString(R"(
        class Box { mut i32 v; Box(i32 x) { this.v = x; } }
        fn take<T>(T& b) -> i32 { return b.v; }
        fn main() -> i32 { return take(Box(3)) + take(new Box(4)); }
    )");
    REQUIRE(ir.find("take$Box") != std::string::npos);
}

TEST_CASE("GenInfer - two type parameters inferred from two arguments", "[geninfer]") {
    auto ir = codegenString(R"(
        class P { mut i32 v; P(i32 x) { this.v = x; } }
        class Q { mut i32 w; Q(i32 x) { this.w = x; } }
        fn combine<A, B>(A& a, B& b) -> i32 { return a.v + b.w; }
        fn main() -> i32 {
            mut P p = P(2); mut Q q = Q(5);
            return combine(p, q);
        }
    )");
    REQUIRE(ir.find("combine$P$Q") != std::string::npos);
}

TEST_CASE("GenInfer - explicit type arguments still work unchanged", "[geninfer]") {
    auto ir = codegenString(R"(
        class Box { mut i32 v; Box(i32 x) { this.v = x; } }
        fn unwrap<T>(T& b) -> i32 { return b.v; }
        fn main() -> i32 { mut Box b = Box(9); return unwrap<Box>(b); }
    )");
    REQUIRE(ir.find("unwrap$Box") != std::string::npos);
}

// ------------------------------------------------------------
// Un-inferable calls → a clear error (all-or-nothing)
// ------------------------------------------------------------

TEST_CASE("GenInfer - a type parameter only in the return type cannot be inferred", "[geninfer]") {
    StderrCapture cap;
    analyzeString(R"(
        class Box { mut i32 v; }
        fn zero<T>() -> T out { }
        fn main() -> i32 { var b = zero(); return 0; }
    )");
    REQUIRE(cap.contains("cannot infer"));
}

TEST_CASE("GenInfer - a bare literal argument is not inferable in v1", "[geninfer]") {
    // T appears as a top-level param type, but the argument is a literal (no syntactic type name).
    StderrCapture cap;
    analyzeString(R"(
        fn echo<T>(T x) -> i32 { return 0; }
        fn main() -> i32 { return echo(5); }
    )");
    REQUIRE(cap.contains("cannot infer"));
}

TEST_CASE("GenInfer - a nested generic parameter type is not inferred in v1", "[geninfer]") {
    // `Array<T>& a` — T is not a *top-level* parameter type, so it isn't deduced from the argument.
    StderrCapture cap;
    analyzeString(R"(
        class Array<T> { mut i32 n; }
        fn size<T>(Array<T>& a) -> i32 { return a.n; }
        fn main() -> i32 { mut Array<i32> a; return size(a); }
    )");
    REQUIRE(cap.contains("cannot infer"));
}

// ------------------------------------------------------------
// More edge cases (found while double-checking)
// ------------------------------------------------------------

TEST_CASE("GenInfer - a repeated type parameter infers from the first argument", "[geninfer]") {
    auto ir = codegenString(R"(
        class Box { mut i32 v; Box(i32 x) { this.v = x; } }
        fn eqv<T>(T& a, T& b) -> i32 { return a.v + b.v; }
        fn main() -> i32 { mut Box a = Box(1); mut Box b = Box(2); return eqv(a, b); }
    )");
    REQUIRE(ir.find("eqv$Box") != std::string::npos);
}

TEST_CASE("GenInfer - mismatched arguments for a repeated type parameter error cleanly", "[geninfer][semantic]") {
    // T is inferred from the FIRST argument (Box); the second (Bag) then fails to match Box& — a
    // clean semantic error, never a miscompile.
    StderrCapture cap;
    auto result = analyzeString(R"(
        class Box { mut i32 v; Box(i32 x) { this.v = x; } }
        class Bag { mut i32 w; Bag(i32 x) { this.w = x; } }
        fn eqv<T>(T& a, T& b) -> i32 { return a.v; }
        fn main() -> i32 { mut Box a = Box(1); mut Bag c = Bag(2); return eqv(a, c); }
    )");
    REQUIRE(result.hadError);
    REQUIRE(cap.contains("no matching overload"));
}

TEST_CASE("GenInfer - inferred from a bare instance field inside a method", "[geninfer]") {
    // The argument `item` is an implicit-`this` field reference — resolved via the parser's
    // class-field scope, not a local.
    auto ir = codegenString(R"(
        class Box { mut i32 v; Box(i32 x) { this.v = x; } }
        fn unwrap<T>(T& b) -> i32 { return b.v; }
        class Holder {
            Box item;
            Holder(i32 x) { this.item = Box(x); }
            fn total() -> i32 { return unwrap(item); }
        }
        fn main() -> i32 { Holder& h = new Holder(5); return h.total(); }
    )");
    REQUIRE(ir.find("unwrap$Box") != std::string::npos);
}

TEST_CASE("GenInfer - a named-argument call is not inference-matched", "[geninfer][semantic]") {
    StderrCapture cap;
    analyzeString(R"(
        class Box { mut i32 v; Box(i32 x) { this.v = x; } }
        fn unwrap<T>(T& b) -> i32 { return b.v; }
        fn main() -> i32 { mut Box b = Box(1); return unwrap(b: b); }
    )");
    REQUIRE(cap.contains("cannot infer"));
}

TEST_CASE("GenInfer - a `var`-typed local argument is not inferable (clean error)", "[geninfer][semantic]") {
    // A `var` local records only the `var` sentinel at parse time (its real type is deduced later,
    // in semantics), so it cannot drive parser-side inference — must be a clean error, NOT a bogus
    // `f$var` instantiation. Same root cause as the var-in-lambda-capture limitation.
    StderrCapture cap;
    analyzeString(R"(
        class Box { mut i32 v; Box(i32 x) { this.v = x; } }
        fn unwrap<T>(T& b) -> i32 { return b.v; }
        fn main() -> i32 { var b = Box(7); return unwrap(b); }
    )");
    REQUIRE(cap.contains("cannot infer"));
}

TEST_CASE("GenInfer - a named callable object infers a Call-bounded type parameter", "[geninfer][trait]") {
    // Passing a named callable-object variable (not a lambda) to a single-`Call`-bounded generic
    // without `<…>` infers the parameter as that object's class — the bound check + `obj(args)`
    // desugar still apply.
    auto ir = codegenString(R"(
        class Adder { mut i32 base; Adder(i32 b) { this.base = b; } }
        impl Call for Adder { fn call(i32 x) -> i32 { return this.base + x; } }
        fn apply<F: Call(i32) -> i32>(F& f, i32 x) -> i32 { return f(x); }
        fn main() -> i32 { Adder& a = new Adder(10); return apply(a, 5); }
    )");
    REQUIRE(ir.find("apply$Adder") != std::string::npos);
}
