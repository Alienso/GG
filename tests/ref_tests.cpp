#include <catch2/catch_test_macros.hpp>
#include "helpers.h"

// ============================================================
// `ref T` — non-owning borrow reference.
//   - `ref T` / `mut ref T` are shared / mutable borrows of a class value.
//   - Non-owning: no refcount traffic (never retained/released, never `+1` on return,
//     never in a dtor scope). Lowers to a plain LLVM `ptr`.
//   - Coercions INTO a borrow are allowed: owning `Class&` → `ref`, value object → `ref`,
//     `ref` → `ref`. The reverse (a borrow → an owning `Class&`) is forbidden.
//   - A `ref` may not be a class field (nothing keeps the borrowee alive).
//   - Escape analysis still applies: passing a *stack value object* to a `ref` parameter
//     that escapes (returned / stored) is rejected — only heap references may escape.
// ============================================================

// ---- accepted ----

TEST_CASE("Ref - borrow a heap reference and mutate through it", "[ref][semantic]") {
    auto r = analyzeString(R"(
        class Point { mut i32 x; Point(i32 v) { x = v; } }
        fn main() -> i32 {
            Point& owner = new Point(5);
            mut ref Point b = owner;
            b.x = 9;
            return owner.x;
        }
    )");
    REQUIRE_FALSE(r.hadError);
}

TEST_CASE("Ref - borrow a stack value object", "[ref][semantic]") {
    auto r = analyzeString(R"(
        class Point { mut i32 x; Point(i32 v) { x = v; } }
        fn main() -> i32 {
            mut Point v(20);
            mut ref Point b = v;
            b.x = 9;
            return v.x;
        }
    )");
    REQUIRE_FALSE(r.hadError);
}

TEST_CASE("Ref - a ref parameter borrows both owning refs and value objects", "[ref][semantic]") {
    auto r = analyzeString(R"(
        class Point { mut i32 x; Point(i32 v) { x = v; } }
        fn bump(mut ref Point p) { p.x = p.x + 1; }
        fn main() -> i32 {
            Point& owner = new Point(10);
            bump(owner);
            mut Point local(20);
            bump(local);
            return owner.x + local.x;
        }
    )");
    REQUIRE_FALSE(r.hadError);
}

TEST_CASE("Ref - a function may return a borrow of an owning-reference argument", "[ref][semantic]") {
    auto r = analyzeString(R"(
        class Point { mut i32 x; Point(i32 v) { x = v; } }
        fn pick(ref Point a, ref Point b, bool first) -> ref Point {
            if (first) { return a; }
            return b;
        }
        fn main() -> i32 {
            Point& p = new Point(3);
            Point& q = new Point(4);
            ref Point c = pick(p, q, false);
            return c.x;
        }
    )");
    REQUIRE_FALSE(r.hadError);
}

TEST_CASE("Ref - generic `ref T` survives monomorphization", "[ref][generic][semantic]") {
    auto r = analyzeString(R"(
        class Point { mut i32 x; Point(i32 v) { x = v; } }
        fn borrow<T>(ref T item) -> ref T { return item; }
        fn main() -> i32 {
            Point& p = new Point(42);
            ref Point b = borrow<Point>(p);
            return b.x;
        }
    )");
    REQUIRE_FALSE(r.hadError);
}

TEST_CASE("Ref - a method dispatches through a borrow", "[ref][semantic]") {
    auto r = analyzeString(R"(
        class Point { mut i32 x; Point(i32 v) { x = v; } fn getX() -> i32 { return x; } }
        fn main() -> i32 {
            Point& owner = new Point(7);
            ref Point b = owner;
            return b.getX();
        }
    )");
    REQUIRE_FALSE(r.hadError);
}

TEST_CASE("Ref - a borrow may be passed where a ref parameter is expected", "[ref][semantic]") {
    auto r = analyzeString(R"(
        class Point { mut i32 x; Point(i32 v) { x = v; } }
        fn read(ref Point p) -> i32 { return p.x; }
        fn main() -> i32 {
            Point& owner = new Point(4);
            ref Point b = owner;
            return read(b);
        }
    )");
    REQUIRE_FALSE(r.hadError);
}

TEST_CASE("Ref - `ref Self` in an impl block resolves to the implementing type", "[ref][semantic]") {
    auto r = analyzeString(R"(
        trait Identity { fn me() -> ref Self; }
        class Node { mut i32 v; Node(i32 x) { v = x; } }
        impl Identity for Node { fn me() -> ref Self { return this; } }
        fn main() -> i32 {
            Node& n = new Node(11);
            ref Node r = n.me();
            return r.v;
        }
    )");
    REQUIRE_FALSE(r.hadError);
}

// ---- rejected ----

TEST_CASE("Ref - a shared (non-mut) borrow cannot mutate through a field", "[ref][semantic]") {
    // The core distinction from `mut ref`: a plain `ref` is a read-only view.
    StderrCapture cap;
    auto r = analyzeString(R"(
        class Point { mut i32 x; Point(i32 v) { x = v; } }
        fn main() -> i32 {
            Point& owner = new Point(5);
            ref Point b = owner;
            b.x = 9;
            return 0;
        }
    )");
    REQUIRE(r.hadError);
}

TEST_CASE("Ref - a non-mut borrow cannot be rebound", "[ref][semantic]") {
    StderrCapture cap;
    auto r = analyzeString(R"(
        class Point { mut i32 x; Point(i32 v) { x = v; } }
        fn main() -> i32 {
            Point& a = new Point(1);
            Point& b = new Point(2);
            ref Point r = a;
            r = b;
            return 0;
        }
    )");
    REQUIRE(r.hadError);
}

TEST_CASE("Ref - a borrow cannot be converted to an owning reference", "[ref][semantic]") {
    StderrCapture cap;
    auto r = analyzeString(R"(
        class Point { mut i32 x; Point(i32 v) { x = v; } }
        fn main() -> i32 {
            Point& owner = new Point(5);
            ref Point b = owner;
            Point& back = b;
            return back.x;
        }
    )");
    REQUIRE(r.hadError);
}

TEST_CASE("Ref - a class field cannot be a borrow", "[ref][semantic]") {
    StderrCapture cap;
    auto r = analyzeString(R"(
        class Point { i32 x; Point(i32 v) { x = v; } }
        class Holder { ref Point r; }
        fn main() -> i32 { return 0; }
    )");
    REQUIRE(r.hadError);
    REQUIRE(cap.contains("borrow"));
}

TEST_CASE("Ref - a primitive borrow reads and writes through the referent", "[ref][semantic]") {
    // `ref i32` is an lvalue reference to a primitive (like C++ `int&`): reads deref, writes
    // store through, and a `mut ref` may mutate the borrowed variable.
    auto r = analyzeString(R"(
        fn main() -> i32 {
            mut i32 n = 5;
            mut ref i32 b = n;
            b = b + 10;
            return n;
        }
    )");
    REQUIRE_FALSE(r.hadError);
}

TEST_CASE("Ref - `ref` of ptr/void is a parse error", "[ref][parser]") {
    // A parse error is caught by parseString and printed to stderr (Program comes back empty),
    // so assert on the captured message rather than the semantic hadError flag.
    StderrCapture cap;
    parseString(R"(
        fn main() -> i32 {
            ptr p = 0;
            ref void b = p;
            return 0;
        }
    )");
    REQUIRE(cap.contains("cannot be borrowed"));
}

TEST_CASE("Ref - a shared primitive borrow cannot be written through", "[ref][semantic]") {
    StderrCapture cap;
    auto r = analyzeString(R"(
        fn main() -> i32 {
            mut i32 n = 5;
            ref i32 b = n;
            b = 9;
            return n;
        }
    )");
    REQUIRE(r.hadError);
}

TEST_CASE("Ref - a primitive borrow cannot bind a temporary", "[ref][semantic]") {
    StderrCapture cap;
    auto r = analyzeString(R"(
        fn main() -> i32 {
            i32 a = 1;
            i32 b = 2;
            ref i32 x = a + b;
            return 0;
        }
    )");
    REQUIRE(r.hadError);
    REQUIRE(cap.contains("addressable"));
}

TEST_CASE("Ref - assign directly to a returned primitive borrow (v.at(i) = x)", "[ref][semantic]") {
    auto r = analyzeString(R"(
        class Counter {
            mut i32 n;
            Counter(i32 v) { n = v; }
            fn slot() -> ref i32 { return n; }
        }
        fn main() -> i32 {
            mut Counter c(0);
            c.slot() = 42;
            return c.n;
        }
    )");
    REQUIRE_FALSE(r.hadError);
}

TEST_CASE("Ref - assigning to a non-reference call result is rejected", "[ref][semantic]") {
    StderrCapture cap;
    auto r = analyzeString(R"(
        fn val() -> i32 { return 5; }
        fn main() -> i32 {
            val() = 3;
            return 0;
        }
    )");
    REQUIRE(r.hadError);
    REQUIRE(cap.contains("not assignable"));
}

TEST_CASE("Ref - a primitive borrow parameter can be written through", "[ref][semantic]") {
    auto r = analyzeString(R"(
        fn bump(mut ref i32 p) { p = p + 1; }
        fn main() -> i32 { mut i32 n = 41; bump(n); return n; }
    )");
    REQUIRE_FALSE(r.hadError);
}

TEST_CASE("Ref - a primitive borrow parameter reads through as a value", "[ref][semantic]") {
    auto r = analyzeString(R"(
        fn get(ref i32 x) -> i32 { return x; }
        fn main() -> i32 { mut i32 n = 7; return get(n); }
    )");
    REQUIRE_FALSE(r.hadError);
}

TEST_CASE("Ref - generic `ref T` passthrough works for a primitive", "[ref][generic][semantic]") {
    auto r = analyzeString(R"(
        fn borrow<T>(ref T item) -> ref T { return item; }
        fn main() -> i32 { mut i32 n = 9; ref i32 r = borrow<i32>(n); return r; }
    )");
    REQUIRE_FALSE(r.hadError);
}

TEST_CASE("Ref - a primitive borrow works for f64 and bool", "[ref][semantic]") {
    auto r = analyzeString(R"(
        class C { mut f64 x; mut bool b; C() { x = 1.5; b = false; }
                  fn fx() -> ref f64 { return x; } fn fb() -> ref bool { return b; } }
        fn main() -> i32 {
            mut C c;
            mut ref f64 rx = c.fx(); rx = 3.5;
            mut ref bool rb = c.fb(); rb = true;
            return 0;
        }
    )");
    REQUIRE_FALSE(r.hadError);
}

TEST_CASE("Ref - passing a temporary to a primitive borrow parameter is rejected", "[ref][semantic]") {
    StderrCapture cap;
    auto r = analyzeString(R"(
        fn bump(mut ref i32 p) { p = p + 1; }
        fn main() -> i32 { mut i32 a = 1; mut i32 b = 2; bump(a + b); return 0; }
    )");
    REQUIRE(r.hadError);
    REQUIRE(cap.contains("temporary"));
}

TEST_CASE("Ref - compound assignment through a primitive borrow is rejected", "[ref][semantic]") {
    StderrCapture cap;
    auto r = analyzeString(R"(
        class C { mut i32 n; C(i32 v) { n = v; } fn slot() -> ref i32 { return n; } }
        fn main() -> i32 { mut C c(10); mut ref i32 r = c.slot(); r += 5; return c.n; }
    )");
    REQUIRE(r.hadError);
    REQUIRE(cap.contains("not supported"));
}

TEST_CASE("Ref - `++` through a primitive borrow is rejected", "[ref][semantic]") {
    StderrCapture cap;
    auto r = analyzeString(R"(
        class C { mut i32 n; C(i32 v) { n = v; } fn slot() -> ref i32 { return n; } }
        fn main() -> i32 { mut C c(10); mut ref i32 r = c.slot(); r++; return c.n; }
    )");
    REQUIRE(r.hadError);
    REQUIRE(cap.contains("not supported"));
}

TEST_CASE("Ref - a method may return a primitive borrow of a field", "[ref][semantic]") {
    auto r = analyzeString(R"(
        class Counter {
            mut i32 n;
            Counter(i32 v) { n = v; }
            fn slot() -> ref i32 { return n; }
        }
        fn main() -> i32 {
            mut Counter c(10);
            mut ref i32 s = c.slot();
            s = s + 5;
            return c.n;
        }
    )");
    REQUIRE_FALSE(r.hadError);
}

TEST_CASE("Ref - passing a stack value object to an escaping ref param is rejected", "[ref][escape][semantic]") {
    // The borrow is returned, so it would outlive the stack value object.
    StderrCapture cap;
    auto r = analyzeString(R"(
        class Point { mut i32 x; Point(i32 v) { x = v; } }
        fn keep(ref Point a) -> ref Point { return a; }
        fn main() -> i32 {
            Point v(3);
            ref Point b = keep(v);
            return 0;
        }
    )");
    REQUIRE(r.hadError);
    REQUIRE(cap.contains("escapes"));
}

// ---- codegen: no refcount traffic ----

TEST_CASE("Ref - borrow lowers to a plain ptr with no retain/release", "[ref][codegen]") {
    std::string ir = codegenString(R"(
        class Point { mut i32 x; Point(i32 v) { x = v; } }
        fn borrow(ref Point item) -> ref Point { return item; }
    )");
    // The borrow function body: load the arg, return it — no refcount calls of its own.
    auto pos = ir.find("@borrow");
    REQUIRE(pos != std::string::npos);
    auto end = ir.find("\n}", pos);
    REQUIRE(end != std::string::npos);
    std::string body = ir.substr(pos, end - pos);
    REQUIRE(body.find("gg_retain") == std::string::npos);
    REQUIRE(body.find("gg_release") == std::string::npos);
}
