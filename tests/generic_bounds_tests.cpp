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

// ------------------------------------------------------------
// Generic BODY checking against bounds (definition-time, bounded-only)
// ------------------------------------------------------------

TEST_CASE("BodyCheck - bounded body using exactly its bounds is accepted", "[bounds][bodycheck][semantic]") {
    auto result = analyzeString(R"(
        trait Comparable { fn compareTo(Self& other) -> i32; }
        trait Show { fn tag() -> i32; }
        class M { mut i32 v; M(i32 x) { v = x; } }
        impl Comparable for M { fn compareTo(M& o) -> i32 { return v - o.v; } }
        impl Ord  for M { fn cmp(M& r) -> i32 { return v - r.v; } }
        impl Show for M { fn tag() -> i32 { return v; } }
        fn maxOf<T: Comparable>(T& a, T& b) -> T& { if (a.compareTo(b) >= 0) { return a; } return b; }
        fn biggest<T: Ord>(T& a, T& b) -> T& { if (a < b) { return b; } return a; }
        fn label<T: Show>(T& a) -> i32 { return a.tag(); }
        fn main() -> i32 {
            M& x = new M(1); M& y = new M(2);
            M& p = maxOf<M>(x, y); M& q = biggest<M>(x, y); i32 t = label<M>(x);
            return 0;
        }
    )");
    REQUIRE_FALSE(result.hadError);
}

TEST_CASE("BodyCheck - calling a method not provided by the bounds is an error", "[bounds][bodycheck][semantic]") {
    StderrCapture cap;
    auto result = analyzeString(R"(
        trait Show { fn tag() -> i32; }
        fn bad<T: Show>(T& a, T& b) -> i32 { return a.compareTo(b); }
        fn main() -> i32 { return 0; }
    )");
    REQUIRE(result.hadError);
    REQUIRE(cap.contains("no method 'compareTo' provided by the bounds"));
}

TEST_CASE("BodyCheck - using an operator whose trait isn't bound is an error", "[bounds][bodycheck][semantic]") {
    StderrCapture cap;
    auto result = analyzeString(R"(
        trait Show { fn tag() -> i32; }
        fn bad<T: Show>(T& a, T& b) -> bool { return a < b; }
        fn main() -> i32 { return 0; }
    )");
    REQUIRE(result.hadError);
    REQUIRE(cap.contains("requires bound 'Ord'"));
}

TEST_CASE("BodyCheck - accessing a field of a type parameter is an error", "[bounds][bodycheck][semantic]") {
    StderrCapture cap;
    auto result = analyzeString(R"(
        trait Show { fn tag() -> i32; }
        fn bad<T: Show>(T& a) -> i32 { return a.x; }
        fn main() -> i32 { return 0; }
    )");
    REQUIRE(result.hadError);
    REQUIRE(cap.contains("type parameter 'T'"));
}

TEST_CASE("BodyCheck - a built-in operator-trait bound permits its operator", "[bounds][bodycheck][semantic]") {
    auto result = analyzeString(R"(
        class M { mut i32 v; M(i32 x) { v = x; } }
        impl Add for M { fn add(M& r) -> M out { out.v = v + r.v; return out; } }
        fn sum<T: Add>(T& a, T& b) -> T { return a + b; }
        fn main() -> i32 { M& x = new M(1); M& y = new M(2); return 0; }
    )");
    REQUIRE_FALSE(result.hadError);
}

TEST_CASE("BodyCheck - a bounded generic class method body is checked", "[bounds][bodycheck][semantic]") {
    // Wrapper<T: Show>.label() calls inner.tag() — provided by Show → accepted.
    auto ok = analyzeString(R"(
        trait Show { fn tag() -> i32; }
        class N { mut i32 v; N(i32 x) { v = x; } }
        impl Show for N { fn tag() -> i32 { return v; } }
        class Wrapper<T: Show> { T& inner; Wrapper(T& x) { this.inner = x; } fn label() -> i32 { return this.inner.tag(); } }
        fn main() -> i32 { N& n = new N(3); Wrapper<N>& w = new Wrapper<N>(n); return w.label(); }
    )");
    REQUIRE_FALSE(ok.hadError);

    // The key win: a class method calls a method the CONCRETE type happens to have (`extra`) but
    // the bound (Show) does not. The instantiation itself is clean, so only the definition-time
    // body-check catches it — exactly the gap this feature closes.
    StderrCapture cap;
    auto bad = analyzeString(R"(
        trait Show { fn tag() -> i32; }
        class N { mut i32 v; N(i32 x) { v = x; } fn extra() -> i32 { return v; } }
        impl Show for N { fn tag() -> i32 { return v; } }
        class Wrapper<T: Show> { T& inner; Wrapper(T& x) { this.inner = x; } fn go() -> i32 { return this.inner.extra(); } }
        fn main() -> i32 { N& n = new N(3); Wrapper<N>& w = new Wrapper<N>(n); return w.go(); }
    )");
    REQUIRE(bad.hadError);
    REQUIRE(cap.contains("no method 'extra' provided by the bounds"));
}

TEST_CASE("BodyCheck - catches use-beyond-bounds even when the concrete type satisfies it (function)",
          "[bounds][bodycheck][semantic]") {
    // `T: Show` body calls `a.extra()`; the instantiation type N has `extra`, so the monomorphized
    // code is fine — but the body relies on more than Show declares, which the body-check flags.
    StderrCapture cap;
    auto result = analyzeString(R"(
        trait Show { fn tag() -> i32; }
        class N { mut i32 v; N(i32 x) { v = x; } fn extra() -> i32 { return v; } }
        impl Show for N { fn tag() -> i32 { return v; } }
        fn use<T: Show>(T& a) -> i32 { return a.extra(); }
        fn main() -> i32 { N& n = new N(1); return use<N>(n); }
    )");
    REQUIRE(result.hadError);
    REQUIRE(cap.contains("no method 'extra' provided by the bounds"));
}

TEST_CASE("BodyCheck - unbounded type params stay permissive (no body check)", "[bounds][bodycheck][semantic]") {
    // `addT`/`maxT` use +/> on an UNbounded T — must still be accepted (duck-typed at instantiation).
    auto result = analyzeString(R"(
        fn addT<T>(T a, T b) -> T { return a + b; }
        fn maxT<T>(T a, T b) -> T { if (a > b) { return a; } return b; }
        fn main() -> i32 { i32 r = addT<i32>(1, 2); i32 m = maxT<i32>(3, 4); return 0; }
    )");
    REQUIRE_FALSE(result.hadError);
}

TEST_CASE("BodyCheck - Self-returning bound methods chain, and multiple/mixed bounds resolve",
          "[bounds][bodycheck][semantic]") {
    auto result = analyzeString(R"(
        trait Combine { fn merge(Self& o) -> Self&; }
        trait Show    { fn tag() -> i32; }
        fn chain<T: Combine>(T& a, T& b, T& c) -> T& { return a.merge(b).merge(c); }   // Self-return chaining
        fn both<T: Show + Combine>(T& a, T& b) -> i32 { T& m = a.merge(b); return m.tag(); }  // one method per bound
        fn mixed<T: Show, U>(T& a, U b) -> i32 { return a.tag(); }                      // U unbounded/permissive
        fn main() -> i32 { return 0; }
    )");
    REQUIRE_FALSE(result.hadError);
}

TEST_CASE("BodyCheck - a built-in operator-trait bound permits its conventional method + chaining",
          "[bounds][bodycheck][semantic]") {
    // `T: Ord` ⇒ `a.cmp(b)` (i32); `T: Add` ⇒ `a.add(b)` and `a + b + c`. A type parameter and a
    // reference to it are interchangeable, so a Self-returning result fits a `T&` return.
    auto result = analyzeString(R"(
        fn byName<T: Ord>(T& a, T& b) -> i32 { return a.cmp(b); }
        fn addName<T: Add>(T& a, T& b) -> T& { return a.add(b); }
        fn chainOp<T: Add>(T& a, T& b, T& c) -> T { return a + b + c; }
        fn main() -> i32 { return 0; }
    )");
    REQUIRE_FALSE(result.hadError);
}

TEST_CASE("BodyCheck - Ord bound does not provide Eq (== requires its own bound)", "[bounds][bodycheck][semantic]") {
    // GG's Ord and Eq are distinct traits — a `T: Ord` body using `==` needs an Eq bound.
    StderrCapture cap;
    auto result = analyzeString(R"(
        fn bad<T: Ord>(T& a, T& b) -> bool { return a == b; }
        fn main() -> i32 { return 0; }
    )");
    REQUIRE(result.hadError);
    REQUIRE(cap.contains("requires bound 'Eq'"));

    // Using `<` (which Ord does provide) is accepted.
    auto ok = analyzeString(R"(
        fn good<T: Ord>(T& a, T& b) -> bool { if (a < b) { return true; } return false; }
        fn main() -> i32 { return 0; }
    )");
    REQUIRE_FALSE(ok.hadError);
}

TEST_CASE("BodyCheck - a bound-method call with the wrong arity is not matched", "[bounds][bodycheck][semantic]") {
    StderrCapture cap;
    auto result = analyzeString(R"(
        trait Comparable { fn compareTo(Self& o) -> i32; }
        fn bad<T: Comparable>(T& a) -> i32 { return a.compareTo(); }
        fn main() -> i32 { return 0; }
    )");
    REQUIRE(result.hadError);
    REQUIRE(cap.contains("no method 'compareTo' provided by the bounds"));
}
