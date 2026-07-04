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
        trait Cmp { i32 c(Self& o); }
        class M { mut i32 v; M(i32 x) { v = x; } }
        impl Cmp for M { i32 c(M& o) { return v - o.v; } }
        T& mx<T: Cmp>(T& a, T& b) { if (a.c(b) >= 0) { return a; } return b; }
        i32 main() {
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
        T& idOf<T>(T& a) { return a; }
        class M { mut i32 v; M(i32 x) { v = x; } }
        i32 main() { M& a = new M(1); M& r = idOf<M>(a); return 0; }
    )");
    REQUIRE(prog.genericBoundChecks.empty());
}

// ------------------------------------------------------------
// Semantic — accepted
// ------------------------------------------------------------

TEST_CASE("Bounds - satisfied user-trait bound is accepted", "[bounds][semantic]") {
    auto result = analyzeString(R"(
        trait Comparable { i32 compareTo(Self& other); }
        class Money {
            mut i32 cents;
            Money(i32 c) { cents = c; }
        }
        impl Comparable for Money { i32 compareTo(Money& o) { return cents - o.cents; } }
        T& maxOf<T: Comparable>(T& a, T& b) {
            if (a.compareTo(b) >= 0) { return a; }
            return b;
        }
        i32 main() {
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
        impl Ord for N { i32 cmp(N& r) { return v - r.v; } }
        T& biggest<T: Ord>(T& a, T& b) { if (a < b) { return b; } return a; }
        i32 main() {
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
        trait Show { i32 tag(); }
        class N { mut i32 v; N(i32 x) { v = x; } }
        impl Ord  for N { i32 cmp(N& r) { return v - r.v; } }
        impl Show for N { i32 tag() { return 7; } }
        T& pick<T: Ord + Show>(T& a, T& b) { if (a < b) { return b; } return a; }
        i32 main() {
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
        trait Show { i32 tag(); }
        class N { mut i32 v; N(i32 x) { v = x; } }
        impl Show for N { i32 tag() { return 7; } }
        class Wrapper<T: Show> {
            T& inner;
            Wrapper(T& x) { this.inner = x; }
            i32 label() { return this.inner.tag(); }
        }
        i32 main() {
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
        trait Comparable { i32 compareTo(Self& other); }
        class Plain { mut i32 v; Plain(i32 x) { v = x; } }
        T& maxOf<T: Comparable>(T& a, T& b) { if (a.compareTo(b) >= 0) { return a; } return b; }
        i32 main() {
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
        trait Comparable { i32 compareTo(Self& other); }
        T pickFirst<T: Comparable>(T a, T b) { return a; }
        i32 main() { i32 r = pickFirst<i32>(1, 2); return 0; }
    )");
    REQUIRE(result.hadError);
    REQUIRE(cap.contains("does not satisfy bound"));
}

TEST_CASE("Bounds - unknown trait in a bound is an error", "[bounds][semantic]") {
    StderrCapture cap;
    auto result = analyzeString(R"(
        class C { mut i32 v; C(i32 x) { v = x; } }
        T& pick<T: Bogus>(T& a, T& b) { return a; }
        i32 main() { C& a = new C(1); C& b = new C(2); C& r = pick<C>(a, b); return 0; }
    )");
    REQUIRE(result.hadError);
    REQUIRE(cap.contains("unknown trait"));
}
