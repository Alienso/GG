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

TEST_CASE("Ref - `ref` of a primitive is a parse error", "[ref][parser]") {
    // A parse error is caught by parseString and printed to stderr (Program comes back empty),
    // so assert on the captured message rather than the semantic hadError flag.
    StderrCapture cap;
    parseString(R"(
        fn main() -> i32 {
            i32 n = 5;
            ref i32 b = n;
            return 0;
        }
    )");
    REQUIRE(cap.contains("class name after 'ref'"));
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
