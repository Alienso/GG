#include <catch2/catch_test_macros.hpp>
#include "helpers.h"

// ============================================================
// `T*` — non-owning borrow reference.
//   - `T*` / `mut T*` are shared / mutable borrows of a class value.
//   - Non-owning: no refcount traffic (never retained/released, never `+1` on return,
//     never in a dtor scope). Lowers to a plain LLVM `ptr`.
//   - Coercions INTO a borrow are allowed: owning `Class&` → `Class*`, value object → `Class*`,
//     `Class*` → `Class*`. The reverse (a borrow → an owning `Class&`) is forbidden.
//   - A borrow may not be a class field (nothing keeps the borrowee alive).
//   - Escape analysis still applies: passing a *stack value object* to a `T*` parameter
//     that escapes (returned / stored) is rejected — only heap references may escape.
// ============================================================

// ---- accepted ----

TEST_CASE("Ref - borrow a heap reference and mutate through it", "[ref][semantic]") {
    auto r = analyzeString(R"(
        class Point { mut i32 x; Point(i32 v) { x = v; } }
        fn main() -> i32 {
            Point& owner = new Point(5);
            mut Point* b = owner;
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
            mut Point* b = v;
            b.x = 9;
            return v.x;
        }
    )");
    REQUIRE_FALSE(r.hadError);
}

TEST_CASE("Ref - a borrow parameter borrows both owning refs and value objects", "[ref][semantic]") {
    auto r = analyzeString(R"(
        class Point { mut i32 x; Point(i32 v) { x = v; } }
        fn bump(mut Point* p) { p.x = p.x + 1; }
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
        fn pick(Point* a, Point* b, bool first) -> Point* {
            if (first) { return a; }
            return b;
        }
        fn main() -> i32 {
            Point& p = new Point(3);
            Point& q = new Point(4);
            Point* c = pick(p, q, false);
            return c.x;
        }
    )");
    REQUIRE_FALSE(r.hadError);
}

TEST_CASE("Ref - a generic borrow `T*` survives monomorphization", "[ref][generic][semantic]") {
    auto r = analyzeString(R"(
        class Point { mut i32 x; Point(i32 v) { x = v; } }
        fn borrow<T>(T* item) -> T* { return item; }
        fn main() -> i32 {
            Point& p = new Point(42);
            Point* b = borrow<Point>(p);
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
            Point* b = owner;
            return b.getX();
        }
    )");
    REQUIRE_FALSE(r.hadError);
}

TEST_CASE("Ref - a borrow may be passed where a borrow parameter is expected", "[ref][semantic]") {
    auto r = analyzeString(R"(
        class Point { mut i32 x; Point(i32 v) { x = v; } }
        fn read(Point* p) -> i32 { return p.x; }
        fn main() -> i32 {
            Point& owner = new Point(4);
            Point* b = owner;
            return read(b);
        }
    )");
    REQUIRE_FALSE(r.hadError);
}

TEST_CASE("Ref - `Self*` in an impl block resolves to the implementing type", "[ref][semantic]") {
    auto r = analyzeString(R"(
        trait Identity { fn me() -> Self*; }
        class Node { mut i32 v; Node(i32 x) { v = x; } }
        impl Identity for Node { fn me() -> Self* { return this; } }
        fn main() -> i32 {
            Node& n = new Node(11);
            Node* r = n.me();
            return r.v;
        }
    )");
    REQUIRE_FALSE(r.hadError);
}

// ---- rejected ----

TEST_CASE("Ref - a shared (non-mut) borrow cannot mutate through a field", "[ref][semantic]") {
    // The core distinction from `mut Point*`: a plain `Point*` is a read-only view.
    StderrCapture cap;
    auto r = analyzeString(R"(
        class Point { mut i32 x; Point(i32 v) { x = v; } }
        fn main() -> i32 {
            Point& owner = new Point(5);
            Point* b = owner;
            b.x = 9;
            return 0;
        }
    )");
    REQUIRE(r.hadError);
    REQUIRE(cap.contains("immutable binding"));
}

TEST_CASE("Ref - a non-mut borrow cannot be rebound", "[ref][semantic]") {
    StderrCapture cap;
    auto r = analyzeString(R"(
        class Point { mut i32 x; Point(i32 v) { x = v; } }
        fn main() -> i32 {
            Point& a = new Point(1);
            Point& b = new Point(2);
            Point* r = a;
            r = b;
            return 0;
        }
    )");
    REQUIRE(r.hadError);
    REQUIRE(cap.contains("reassign immutable"));
}

TEST_CASE("Ref - a borrow cannot be converted to an owning reference", "[ref][semantic]") {
    StderrCapture cap;
    auto r = analyzeString(R"(
        class Point { mut i32 x; Point(i32 v) { x = v; } }
        fn main() -> i32 {
            Point& owner = new Point(5);
            Point* b = owner;
            Point& back = b;
            return back.x;
        }
    )");
    REQUIRE(r.hadError);
    REQUIRE(cap.contains("cannot implicitly convert"));
}

TEST_CASE("Ref - a class field borrow is rejected without --unsafe-ptr", "[ref][semantic]") {
    // A borrow field can outlive its source, which escape analysis can't prove — so it is rejected by
    // default. (Under --unsafe-ptr it is ALLOWED: see the next case.)
    StderrCapture cap;
    auto r = analyzeString(R"(
        class Point { i32 x; Point(i32 v) { x = v; } }
        class Holder { Point* r; }
        fn main() -> i32 { return 0; }
    )", CompilerOptions{});   // allowRawPtr defaults to false
    REQUIRE(r.hadError);
    REQUIRE(cap.contains("borrow"));
}

TEST_CASE("Ref - a class field borrow is ALLOWED under --unsafe-ptr", "[ref][semantic]") {
    // The canonical use is a non-owning cursor viewing its collection (e.g. an iterator). A borrow
    // field lowers to a plain `ptr` and is never retained/released.
    auto r = analyzeString(R"(
        class Point { mut i32 x; Point(i32 v) { x = v; } }
        class Holder { mut Point* r; }
        fn main() -> i32 { return 0; }
    )");   // defaultTestOptions(): allowRawPtr = true
    REQUIRE_FALSE(r.hadError);
}

TEST_CASE("Ref - a PRIMITIVE borrow field is always rejected (even under --unsafe-ptr)", "[ref][semantic]") {
    // Only CLASS borrows (`Point*`) are permitted as fields; a primitive borrow (`i32*`) has no
    // struct/clone codegen (it would be mistaken for the scalar value and wrongly retained).
    StderrCapture cap;
    auto r = analyzeString(R"(
        class Holder { mut i32* r; }
        fn main() -> i32 { return 0; }
    )");   // allowRawPtr = true — still rejected
    REQUIRE(r.hadError);
    REQUIRE(cap.contains("primitive borrow"));
}

TEST_CASE("Ref - assigning a class borrow field emits no refcount traffic (non-owning)", "[ref][codegen]") {
    // A borrow field is a plain pointer: `holder.r = obj` stores the address with NO gg_retain of the
    // new target and NO gg_release of the old — otherwise the borrowed object would leak (the holder
    // never releases a borrow). Regression for a leak that silently retained the borrowed collection.
    std::string ir = codegenString(R"(
        class Point { mut i32 x; Point(i32 v) { x = v; } }
        class Holder { mut Point* r; }
        fn main() -> i32 {
            mut Point& p = new Point(5);
            mut Holder h;
            h.r = p;
            return h.r.x;
        }
    )");
    // The Holder has no destructor (a borrow field doesn't own anything), and the borrow store `h.r =
    // p` emits NO retain — `new Point` claims its own `+1` (no retain call), so the whole program has
    // zero gg_retain calls. (Before the fix, the borrow store retained `p`, leaking it.)
    REQUIRE(ir.find("@Holder_dtor(")        == std::string::npos);
    REQUIRE(ir.find("call void @gg_retain") == std::string::npos);
}

TEST_CASE("Ref - a primitive borrow reads and writes through the referent", "[ref][semantic]") {
    // `i32*` is an lvalue reference to a primitive (like C++ `int&`): reads deref, writes
    // store through, and a `mut i32*` may mutate the borrowed variable.
    auto r = analyzeString(R"(
        fn main() -> i32 {
            mut i32 n = 5;
            mut i32* b = n;
            b = b + 10;
            return n;
        }
    )");
    REQUIRE_FALSE(r.hadError);
}

TEST_CASE("Ref - a borrow of ptr/void is a parse error", "[ref][parser]") {
    // A parse error is caught by parseString and printed to stderr (Program comes back empty),
    // so assert on the captured message rather than the semantic hadError flag.
    StderrCapture cap;
    parseString(R"(
        fn main() -> i32 {
            ptr p = 0;
            void* b = p;
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
            i32* b = n;
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
            i32* x = a + b;
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
            fn slot() -> i32* { return n; }
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
        fn bump(mut i32* p) { p = p + 1; }
        fn main() -> i32 { mut i32 n = 41; bump(n); return n; }
    )");
    REQUIRE_FALSE(r.hadError);
}

TEST_CASE("Ref - a primitive borrow parameter reads through as a value", "[ref][semantic]") {
    auto r = analyzeString(R"(
        fn get(i32* x) -> i32 { return x; }
        fn main() -> i32 { mut i32 n = 7; return get(n); }
    )");
    REQUIRE_FALSE(r.hadError);
}

TEST_CASE("Ref - a generic borrow `T*` passthrough works for a primitive", "[ref][generic][semantic]") {
    auto r = analyzeString(R"(
        fn borrow<T>(T* item) -> T* { return item; }
        fn main() -> i32 { mut i32 n = 9; i32* r = borrow<i32>(n); return r; }
    )");
    REQUIRE_FALSE(r.hadError);
}

TEST_CASE("Ref - a primitive borrow works for f64 and bool", "[ref][semantic]") {
    auto r = analyzeString(R"(
        class C { mut f64 x; mut bool b; C() { x = 1.5; b = false; }
                  fn fx() -> f64* { return x; } fn fb() -> bool* { return b; } }
        fn main() -> i32 {
            mut C c;
            mut f64* rx = c.fx(); rx = 3.5;
            mut bool* rb = c.fb(); rb = true;
            return 0;
        }
    )");
    REQUIRE_FALSE(r.hadError);
}

TEST_CASE("Ref - passing a temporary to a primitive borrow parameter is rejected", "[ref][semantic]") {
    StderrCapture cap;
    auto r = analyzeString(R"(
        fn bump(mut i32* p) { p = p + 1; }
        fn main() -> i32 { mut i32 a = 1; mut i32 b = 2; bump(a + b); return 0; }
    )");
    REQUIRE(r.hadError);
    REQUIRE(cap.contains("temporary"));
}

TEST_CASE("Ref - compound assignment through a primitive borrow is rejected", "[ref][semantic]") {
    StderrCapture cap;
    auto r = analyzeString(R"(
        class C { mut i32 n; C(i32 v) { n = v; } fn slot() -> i32* { return n; } }
        fn main() -> i32 { mut C c(10); mut i32* r = c.slot(); r += 5; return c.n; }
    )");
    REQUIRE(r.hadError);
    REQUIRE(cap.contains("through a borrow"));
}

TEST_CASE("Ref - `++` through a primitive borrow is rejected", "[ref][semantic]") {
    StderrCapture cap;
    auto r = analyzeString(R"(
        class C { mut i32 n; C(i32 v) { n = v; } fn slot() -> i32* { return n; } }
        fn main() -> i32 { mut C c(10); mut i32* r = c.slot(); r++; return c.n; }
    )");
    REQUIRE(r.hadError);
    REQUIRE(cap.contains("through a borrow"));
}

TEST_CASE("Ref - a method may return a primitive borrow of a field", "[ref][semantic]") {
    auto r = analyzeString(R"(
        class Counter {
            mut i32 n;
            Counter(i32 v) { n = v; }
            fn slot() -> i32* { return n; }
        }
        fn main() -> i32 {
            mut Counter c(10);
            mut i32* s = c.slot();
            s = s + 5;
            return c.n;
        }
    )");
    REQUIRE_FALSE(r.hadError);
}

TEST_CASE("Ref - passing a stack value object to an escaping borrow param is rejected", "[ref][escape][semantic]") {
    // The borrow is returned, so it would outlive the stack value object.
    StderrCapture cap;
    auto r = analyzeString(R"(
        class Point { mut i32 x; Point(i32 v) { x = v; } }
        fn keep(Point* a) -> Point* { return a; }
        fn main() -> i32 {
            Point v(3);
            Point* b = keep(v);
            return 0;
        }
    )");
    REQUIRE(r.hadError);
    REQUIRE(cap.contains("escapes"));
}

TEST_CASE("Ref - a primitive borrow can bind an explicit class-field member access", "[ref][semantic]") {
    // `genBorrowSource` previously handled only a bare identifier / index expression as an
    // addressable source — an explicit `obj.field` member access fell through to "not
    // addressable" and produced malformed IR (a `store` with a missing operand). This exercises
    // the fix: both a plain reference receiver and a `!!`-unwrapped nullable receiver.
    auto r = analyzeString(R"(
        class Node { mut i32 v; Node(i32 x) { v = x; } }
        fn main() -> i32 {
            Node& n = new Node(5);
            i32* a = n.v;
            Node&? m = new Node(7);
            i32* b = m!!.v;
            return a + b;
        }
    )");
    REQUIRE_FALSE(r.hadError);
}

TEST_CASE("Ref - a primitive borrow from a class-field member access emits a valid GEP, not a malformed store",
          "[ref][codegen]") {
    std::string ir = codegenString(R"(
        class Node { mut i32 v; Node(i32 x) { v = x; } }
        fn grab(Node& n) -> i32 { i32* a = n.v; return a; }
    )");
    REQUIRE(ir.find("store ptr , ") == std::string::npos);
    auto pos = ir.find("@grab");
    REQUIRE(pos != std::string::npos);
    auto end = ir.find("\n}", pos);
    REQUIRE(end != std::string::npos);
    std::string body = ir.substr(pos, end - pos);
    REQUIRE(body.find("getelementptr") != std::string::npos);
}

// ---- codegen: no refcount traffic ----

TEST_CASE("Ref - borrow lowers to a plain ptr with no retain/release", "[ref][codegen]") {
    std::string ir = codegenString(R"(
        class Point { mut i32 x; Point(i32 v) { x = v; } }
        fn borrow(Point* item) -> Point* { return item; }
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
