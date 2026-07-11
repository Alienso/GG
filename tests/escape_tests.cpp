#include <catch2/catch_test_macros.hpp>
#include "helpers.h"

// ============================================================
// Escape analysis (v1). Passing a *stack value object* where a `Class&` parameter is expected is a
// safe borrow only if the callee doesn't let the reference outlive the call. If the parameter (or
// the implicit receiver `this`) is returned or stored into a reference field, it escapes → error.
// The check is gated on the argument being a value object (Object) coerced to a Reference, so
// reference→reference passing, value-field copies, and pure-borrow readers are never flagged.
// ============================================================

// ---- rejected: the reference escapes ----

TEST_CASE("Escape - storing a borrowed value object into a reference field is rejected", "[escape][semantic]") {
    StderrCapture cap;
    auto r = analyzeString(R"(
        class Point { mut i32 x; Point(i32 v) { x = v; } }
        class Box { mut Point& p; Box() { } fn keep(Point& q) mut { p = q; } }
        fn main() -> i32 { Point v(3); mut Box b; b.keep(v); return 0; }
    )");
    REQUIRE(r.hadError);
    REQUIRE(cap.contains("escapes"));
}

TEST_CASE("Escape - `this.field = q` (explicit) is also rejected", "[escape][semantic]") {
    StderrCapture cap;
    auto r = analyzeString(R"(
        class Point { mut i32 x; Point(i32 v) { x = v; } }
        class Box { mut Point& p; Box() { } fn keep(Point& q) mut { this.p = q; } }
        fn main() -> i32 { Point v(3); mut Box b; b.keep(v); return 0; }
    )");
    REQUIRE(r.hadError);
    REQUIRE(cap.contains("escapes"));
}

TEST_CASE("Escape - returning a borrowed value object is rejected", "[escape][semantic]") {
    StderrCapture cap;
    auto r = analyzeString(R"(
        class Point { mut i32 x; Point(i32 v) { x = v; } }
        fn id(Point& q) -> Point& { return q; }
        fn main() -> i32 { Point v(3); Point& r = id(v); return 0; }
    )");
    REQUIRE(r.hadError);
    REQUIRE(cap.contains("escapes"));
}

// Note: a method's `this` cannot itself escape in current GG — `this` is Object-typed and
// Object→Reference is already rejected in return/store positions, so `return this;` /
// `field = this;` never compile. The `thisEscapes` machinery exists for that future case; there
// is no valid program that triggers it today.

// ---- accepted: no escape ----

TEST_CASE("Escape - a pure-borrow reader accepts a value object", "[escape][semantic]") {
    auto r = analyzeString(R"(
        class Point { mut i32 x; Point(i32 v) { x = v; } fn getX() -> i32 { return x; } }
        fn readX(Point& q) -> i32 { return q.x; }
        fn main() -> i32 { Point v(3); i32 a = readX(v); i32 b = v.getX(); return a + b; }
    )");
    REQUIRE_FALSE(r.hadError);
}

TEST_CASE("Escape - passing a heap reference to a storing parameter is allowed", "[escape][semantic]") {
    // A real reference co-owns its target, so escaping is fine — the check is gated on value objects.
    auto r = analyzeString(R"(
        class Point { mut i32 x; Point(i32 v) { x = v; } }
        class Box { mut Point& p; Box() { } fn keep(Point& q) mut { p = q; } }
        fn main() -> i32 { Point& r = new Point(3); mut Box b; b.keep(r); return 0; }
    )");
    REQUIRE_FALSE(r.hadError);
}

TEST_CASE("Escape - storing into a value-object field is a copy, not an escape", "[escape][semantic]") {
    auto r = analyzeString(R"(
        class Money  { mut i32 d; Money(i32 a) { d = a; } }
        class Wallet { mut Money m; Wallet(Money& mm) { m = mm; } }
        fn main() -> i32 { Money a(1); Wallet w(a); return 0; }
    )");
    REQUIRE_FALSE(r.hadError);
}

TEST_CASE("Escape - an operator borrowing its value operand is allowed", "[escape][semantic]") {
    // `a + b` borrows both operands as `Self&`; add() only reads them → no escape.
    auto r = analyzeString(R"(
        class V { mut i32 x; V(i32 a) { x = a; } }
        impl Add for V { fn add(V& rhs) -> V& { return new V(x + rhs.x); } }
        fn main() -> i32 { V a(1); V b(2); V& c = a + b; return 0; }
    )");
    REQUIRE_FALSE(r.hadError);
}

// ---- more edge cases ----

TEST_CASE("Escape - a constructor storing a ref param rejects a value object (new)", "[escape][semantic]") {
    StderrCapture cap;
    auto r = analyzeString(R"(
        class Point { mut i32 x; Point(i32 v) { x = v; } }
        class Box { mut Point& p; Box(Point& q) { p = q; } }
        fn main() -> i32 { Point v(3); Box& b = new Box(v); return 0; }
    )");
    REQUIRE(r.hadError);
    REQUIRE(cap.contains("escapes"));
}

TEST_CASE("Escape - a constructor storing a ref param rejects a value object (stack)", "[escape][semantic]") {
    StderrCapture cap;
    auto r = analyzeString(R"(
        class Point { mut i32 x; Point(i32 v) { x = v; } }
        class Box { mut Point& p; Box(Point& q) { p = q; } }
        fn main() -> i32 { Point v(3); Box b(v); return 0; }
    )");
    REQUIRE(r.hadError);
}

TEST_CASE("Escape - escape through a local reference alias is tracked", "[escape][semantic]") {
    StderrCapture cap;
    auto r = analyzeString(R"(
        class Point { mut i32 x; Point(i32 v) { x = v; } }
        fn f(Point& q) -> Point& { Point& r = q; return r; }
        fn main() -> i32 { Point v(3); Point& r = f(v); return 0; }
    )");
    REQUIRE(r.hadError);
}

TEST_CASE("Escape - a store nested inside control flow still escapes", "[escape][semantic]") {
    StderrCapture cap;
    auto r = analyzeString(R"(
        class Point { mut i32 x; Point(i32 v) { x = v; } }
        class Box { mut Point& p; Box() { } fn keep(Point& q, bool c) mut { if (c) { p = q; } } }
        fn main() -> i32 { Point v(3); mut Box b; b.keep(v, true); return 0; }
    )");
    REQUIRE(r.hadError);
}

TEST_CASE("Escape - only the escaping parameter is rejected (per-parameter)", "[escape][semantic]") {
    // set2 stores `a` but not `b`. Value object as `a` → error; as `b` → OK.
    auto bad = analyzeString(R"(
        class Point { mut i32 x; Point(i32 v) { x = v; } }
        class Box { mut Point& p; Box() { } fn set2(Point& a, Point& b) mut { p = a; } }
        fn main() -> i32 { Point v(3); Point& r = new Point(9); mut Box b; b.set2(v, r); return 0; }
    )");
    REQUIRE(bad.hadError);

    auto ok = analyzeString(R"(
        class Point { mut i32 x; Point(i32 v) { x = v; } }
        class Box { mut Point& p; Box() { } fn set2(Point& a, Point& b) mut { p = a; } }
        fn main() -> i32 { Point v(3); Point& r = new Point(9); mut Box b; b.set2(r, v); return 0; }
    )");
    REQUIRE_FALSE(ok.hadError);
}

TEST_CASE("Escape - a local shadowing a reference field is a rebind, not a field store", "[escape][semantic]") {
    // `p` here is a local that shadows the field `p`; `p = q` rebinds the local → not an escape.
    auto r = analyzeString(R"(
        class Point { mut i32 x; Point(i32 v) { x = v; } }
        class Box {
            mut Point& p;
            Box() { }
            fn use(Point& q) mut { mut Point& p = new Point(0); p = q; }
        }
        fn main() -> i32 { Point v(3); mut Box b; b.use(v); return 0; }
    )");
    REQUIRE_FALSE(r.hadError);
}

TEST_CASE("Escape - returning a field (not the parameter) is not an escape of the parameter", "[escape][semantic]") {
    auto r = analyzeString(R"(
        class Point { mut i32 x; Point(i32 v) { x = v; } }
        class Box { mut Point& p; Box(Point& q) { p = q; } fn getP() -> Point& { return p; } }
        fn readVal(Box& b) -> i32 { return b.getP().x; }
        fn main() -> i32 { Point& pt = new Point(3); Box b2(pt); return readVal(b2); }
    )");
    REQUIRE_FALSE(r.hadError);
}
