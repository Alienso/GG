#include <catch2/catch_test_macros.hpp>
#include "helpers.h"

// ============================================================
// Generic trait bounds — `T maxOf<T: Comparable>(...)`.
//
// Bounds are enforced at each instantiation site (static dispatch): when a
// template is monomorphized the parser records a GenericBoundCheck obligation on
// the Program, and the semantic analyzer verifies the concrete type argument
// implements the named trait (via `implementedTraits`). The type parameter itself
// never reaches semantics/codegen.
// ============================================================

// ------------------------------------------------------------
// Parser — obligations recorded on the Program
// ------------------------------------------------------------

TEST_CASE("Bounds - instantiation records a bound obligation", "[bounds][parser]") {
    auto prog = parseString(R"(
        trait Cmp { fn c(Self& o) -> i32; }
        class M { mut i32 v; M(i32 x) { v = x; } }
        impl Cmp for M { fn c(M& o) -> i32 { return v - o.v; } }
        fn mx<T: Cmp>(T& a, T& b) -> T& { if (a.c(b) >= 0) { return a; } return b; }
        fn main() -> i32 {
            M& a = new M(1);
            M& b = new M(2);
            M& r = mx<M>(a, b);
            return 0;
        }
    )");
    bool found = false;
    for (const auto& bc : prog.genericBoundChecks)
        if (bc.typeName == "M" && bc.traitName == "Cmp") found = true;
    REQUIRE(found);
}

TEST_CASE("Bounds - unbounded type params record no obligations", "[bounds][parser]") {
    auto prog = parseString(R"(
        fn idOf<T>(T& a) -> T& { return a; }
        class M { mut i32 v; M(i32 x) { v = x; } }
        fn main() -> i32 { M& a = new M(1); M& r = idOf<M>(a); return 0; }
    )");
    REQUIRE(prog.genericBoundChecks.empty());
}

// ------------------------------------------------------------
// Semantic — accepted
// ------------------------------------------------------------

TEST_CASE("Bounds - satisfied user-trait bound is accepted", "[bounds][semantic]") {
    auto result = analyzeString(R"(
        trait Comparable { fn compareTo(Self& other) -> i32; }
        class Money {
            mut i32 cents;
            Money(i32 c) { cents = c; }
        }
        impl Comparable for Money { fn compareTo(Money& o) -> i32 { return cents - o.cents; } }
        fn maxOf<T: Comparable>(T& a, T& b) -> T& {
            if (a.compareTo(b) >= 0) { return a; }
            return b;
        }
        fn main() -> i32 {
            Money& x = new Money(5);
            Money& y = new Money(9);
            Money& m = maxOf<Money>(x, y);
            return 0;
        }
    )");
    REQUIRE_FALSE(result.hadError);
}

TEST_CASE("Bounds - satisfied built-in operator-trait bound (Ord)", "[bounds][semantic]") {
    auto result = analyzeString(R"(
        class N { mut i32 v; N(i32 x) { v = x; } }
        impl Ord for N { fn cmp(N& r) -> i32 { return v - r.v; } }
        fn biggest<T: Ord>(T& a, T& b) -> T& { if (a < b) { return b; } return a; }
        fn main() -> i32 {
            N& a = new N(1);
            N& b = new N(2);
            N& r = biggest<N>(a, b);
            return 0;
        }
    )");
    REQUIRE_FALSE(result.hadError);
}

TEST_CASE("Bounds - multiple bounds on one param are all checked", "[bounds][semantic]") {
    auto result = analyzeString(R"(
        trait Show { fn tag() -> i32; }
        class N { mut i32 v; N(i32 x) { v = x; } }
        impl Ord  for N { fn cmp(N& r) -> i32 { return v - r.v; } }
        impl Show for N { fn tag() -> i32 { return 7; } }
        fn pick<T: Ord + Show>(T& a, T& b) -> T& { if (a < b) { return b; } return a; }
        fn main() -> i32 {
            N& a = new N(1);
            N& b = new N(2);
            N& r = pick<N>(a, b);
            return 0;
        }
    )");
    REQUIRE_FALSE(result.hadError);
}

TEST_CASE("Bounds - generic class with a bound is accepted", "[bounds][semantic]") {
    auto result = analyzeString(R"(
        trait Show { fn tag() -> i32; }
        class N { mut i32 v; N(i32 x) { v = x; } }
        impl Show for N { fn tag() -> i32 { return 7; } }
        class Wrapper<T: Show> {
            T& inner;
            Wrapper(T& x) { this.inner = x; }
            fn label() -> i32 { return this.inner.tag(); }
        }
        fn main() -> i32 {
            N& a = new N(1);
            Wrapper<N>& w = new Wrapper<N>(a);
            return w.label();
        }
    )");
    REQUIRE_FALSE(result.hadError);
}

// ------------------------------------------------------------
// Semantic — rejected
// ------------------------------------------------------------

TEST_CASE("Bounds - class not implementing the trait is rejected", "[bounds][semantic]") {
    StderrCapture cap;
    auto result = analyzeString(R"(
        trait Comparable { fn compareTo(Self& other) -> i32; }
        class Plain { mut i32 v; Plain(i32 x) { v = x; } }
        fn maxOf<T: Comparable>(T& a, T& b) -> T& { if (a.compareTo(b) >= 0) { return a; } return b; }
        fn main() -> i32 {
            Plain& p = new Plain(1);
            Plain& q = new Plain(2);
            Plain& m = maxOf<Plain>(p, q);
            return 0;
        }
    )");
    REQUIRE(result.hadError);
    REQUIRE(cap.contains("does not satisfy bound"));
    REQUIRE(cap.contains("Comparable"));
}

TEST_CASE("Bounds - primitive type argument does not satisfy a bound", "[bounds][semantic]") {
    StderrCapture cap;
    auto result = analyzeString(R"(
        trait Comparable { fn compareTo(Self& other) -> i32; }
        fn pickFirst<T: Comparable>(T a, T b) -> T { return a; }
        fn main() -> i32 { i32 r = pickFirst<i32>(1, 2); return 0; }
    )");
    REQUIRE(result.hadError);
    REQUIRE(cap.contains("does not satisfy bound"));
}

TEST_CASE("Bounds - unknown trait in a bound is an error", "[bounds][semantic]") {
    StderrCapture cap;
    auto result = analyzeString(R"(
        class C { mut i32 v; C(i32 x) { v = x; } }
        fn pick<T: Bogus>(T& a, T& b) -> T& { return a; }
        fn main() -> i32 { C& a = new C(1); C& b = new C(2); C& r = pick<C>(a, b); return 0; }
    )");
    REQUIRE(result.hadError);
    REQUIRE(cap.contains("unknown trait"));
}
