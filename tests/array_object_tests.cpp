#include <catch2/catch_test_macros.hpp>
#include "helpers.h"

// ============================================================
// Owned value-object elements in a `ptr<T>` buffer (the machinery behind stdlib Array<T>):
//   • object element WRITE `data[i] = v` → zero-init the slot + deep clone (safe construct)
//   • object element READ `data[i]`      → the slot ADDRESS (not a load), so it borrows/clones
//   • `destroy(place)`                    → the class's dtor in place (no-op for primitive/no-dtor)
//   • temp-materialization                → a primitive rvalue binds to a `T*` param
//   • owning-`ptr` value objects (String / nested containers) are rejected
// ============================================================

static const char* MINI_VEC = R"(
    extern malloc(u64 size) -> ptr;
    extern free(ptr p);
    class Point { mut i32 x; mut i32 y; Point(i32 a, i32 b) { x = a; y = b; } }
    class Vec<T> {
        private mut ptr<T> data;
        private mut u64 count;
        Vec() { count = 0; data = malloc(64 * sizeof(T)); }
        fn push(T* value) mut { data[count] = value; count = count + 1; }
        fn at(i32 i) -> T* { return data[i]; }
    }
)";

TEST_CASE("ArrayObj - object element write zero-inits the slot then deep-clones", "[array-object][codegen]") {
    std::string ir = codegenString(std::string(MINI_VEC) + R"(
        fn main() -> i32 { mut Vec<Point>& v = new Vec<Point>(); Point p(3, 4); v.push(p); return 0; }
    )");
    REQUIRE(ir.find("store %Point zeroinitializer") != std::string::npos);
    REQUIRE(ir.find("call void @Point_clone(ptr ") != std::string::npos);
    // The old malformed scalar store of a struct value must be gone.
    REQUIRE(ir.find("store %Point %") == std::string::npos);
}

TEST_CASE("ArrayObj - primitive element write stays a scalar store (no clone)", "[array-object][codegen]") {
    std::string ir = codegenString(R"(
        extern malloc(u64 size) -> ptr;
        class Vec<T> {
            private mut ptr<T> data;
            private mut u64 count;
            Vec() { count = 0; data = malloc(64 * sizeof(T)); }
            fn push(T* value) mut { data[count] = value; count = count + 1; }
        }
        fn main() -> i32 { mut Vec<i32>& v = new Vec<i32>(); v.push(7); return 0; }
    )");
    REQUIRE(ir.find("store i32 ") != std::string::npos);
    REQUIRE(ir.find("_clone(") == std::string::npos);   // no clone for a primitive element
}

TEST_CASE("ArrayObj - destroy(data[i]) on a dtor-needing class emits @Class_dtor", "[array-object][codegen]") {
    std::string ir = codegenString(R"(
        extern malloc(u64 size) -> ptr;
        extern free(ptr p);
        class Node { mut i32 v; Node(i32 x) { v = x; } ~Node() { v = 0; } }
        class Vec<T> {
            private mut ptr<T> data;
            private mut u64 count;
            Vec() { count = 0; data = malloc(64 * sizeof(T)); }
            ~Vec() { mut u64 i = 0; while (i < count) { destroy(data[i]); i = i + 1; } free(data); }
        }
        fn main() -> i32 { mut Vec<Node>& v = new Vec<Node>(); return 0; }
    )");
    REQUIRE(ir.find("call void @Node_dtor(ptr ") != std::string::npos);
}

TEST_CASE("ArrayObj - destroy on a primitive element is a no-op (no dtor call)", "[array-object][codegen]") {
    // Non-generic container so its own dtor name (`@IntVec_dtor`) doesn't contain "i32_dtor".
    std::string ir = codegenString(R"(
        extern malloc(u64 size) -> ptr;
        extern free(ptr p);
        class IntVec {
            private mut ptr<i32> d;
            private mut u64 c;
            IntVec() { c = 0; d = malloc(64 * sizeof(i32)); }
            ~IntVec() { mut u64 i = 0; while (i < c) { destroy(d[i]); i = i + 1; } free(d); }
        }
        fn main() -> i32 { mut IntVec& v = new IntVec(); return 0; }
    )");
    REQUIRE(ir.find("i32_dtor") == std::string::npos);   // destroy(i32) emits no dtor
}

TEST_CASE("ArrayObj - destroy requires --unsafe-ptr", "[array-object][semantic]") {
    StderrCapture cap;
    auto result = analyzeString(R"(
        class Point { mut i32 x; Point(i32 a) { x = a; } ~Point() { x = 0; } }
        fn f(Point& p) { destroy(p); }
    )", CompilerOptions{});   // allowRawPtr = false
    REQUIRE(result.hadError);
    REQUIRE(cap.contains("requires --unsafe-ptr"));
}

TEST_CASE("ArrayObj - a value object owning a raw ptr is rejected as a ptr<T> element", "[array-object][semantic]") {
    StderrCapture cap;
    auto result = analyzeString(R"(
        extern malloc(u64 size) -> ptr;
        class Buf { mut ptr<i32> raw; Buf() { raw = malloc(16); } }
        class Vec<T> {
            private mut ptr<T> data;
            private mut u64 count;
            Vec() { count = 0; data = malloc(64 * sizeof(T)); }
            fn push(T* value) mut { data[count] = value; count = count + 1; }
        }
        fn main() -> i32 { mut Vec<Buf>& v = new Vec<Buf>(); Buf b; v.push(b); return 0; }
    )");
    REQUIRE(result.hadError);
    REQUIRE(cap.contains("owns a raw buffer"));
}

TEST_CASE("ArrayObj - a primitive rvalue binds to a T* param via a materialized temp", "[array-object][codegen]") {
    std::string ir = codegenString(std::string(R"(
        extern malloc(u64 size) -> ptr;
        class Vec<T> {
            private mut ptr<T> data;
            private mut u64 count;
            Vec() { count = 0; data = malloc(64 * sizeof(T)); }
            fn push(T* value) mut { data[count] = value; count = count + 1; }
        }
        fn main() -> i32 { mut Vec<i32>& v = new Vec<i32>(); v.push(5); return 0; }
    )"));
    // A hidden slot holds the literal and its address is passed (no bogus `ptr 5`).
    REQUIRE(ir.find("borrowtmp") != std::string::npos);
    REQUIRE(ir.find("(ptr 5") == std::string::npos);
}

TEST_CASE("ArrayObj - destroy of a scope-managed local object is rejected (double-free guard)", "[array-object][semantic]") {
    StderrCapture cap;
    auto result = analyzeString(R"(
        class Node { mut i32 v; Node(i32 x) { v = x; } ~Node() { v = 0; } }
        fn main() -> i32 { mut Node n(5); destroy(n); return 0; }
    )");
    REQUIRE(result.hadError);
    REQUIRE(cap.contains("double-free"));
}

TEST_CASE("ArrayObj - an embedded value-object element deep-clones recursively", "[array-object][codegen]") {
    // Array<Line> where Line embeds two Point value fields: Line_clone must recurse into Point_clone,
    // and the top-level zeroinitializer zeroes the whole aggregate so each nested clone is safe.
    std::string ir = codegenString(R"(
        extern malloc(u64 size) -> ptr;
        class Point { mut i32 x; mut i32 y; Point(i32 a, i32 b) { x = a; y = b; } }
        class Line { mut Point a; mut Point b; }
        class Vec<T> {
            private mut ptr<T> data;
            private mut u64 count;
            Vec() { count = 0; data = malloc(64 * sizeof(T)); }
            fn push(T* value) mut { data[count] = value; count = count + 1; }
        }
        fn main() -> i32 { mut Vec<Line>& v = new Vec<Line>(); mut Line ln; v.push(ln); return 0; }
    )");
    REQUIRE(ir.find("store %Line zeroinitializer") != std::string::npos);
    REQUIRE(ir.find("call void @Line_clone(") != std::string::npos);
    REQUIRE(ir.find("call void @Point_clone(") != std::string::npos);   // recursion into embedded fields
}

TEST_CASE("ArrayObj - a mut T* param still rejects a temporary", "[array-object][semantic]") {
    StderrCapture cap;
    auto result = analyzeString(R"(
        class C { mut i32 v; C() { v = 0; } fn take(mut i32* r) mut { r = 9; } }
        fn main() -> i32 { mut C c; c.take(5); return 0; }
    )");
    REQUIRE(result.hadError);
    REQUIRE(cap.contains("write through it would be lost"));
}

// ============================================================
// Direct-construct: `data[i] = Class(args)` on a `ptr<Class>` element constructs straight into the
// slot's address (no temp, no clone) — the same "result location" treatment a local `Point p(2,3);`
// already gets. Non-constructor RHS shapes must keep going through the existing clone path.
// ============================================================

TEST_CASE("ArrayObj - data[i] = Class(args) constructs directly, no temp or clone", "[array-object][codegen]") {
    std::string ir = codegenString(R"(
        extern malloc(u64 size) -> ptr;
        class Point { mut i32 x; mut i32 y; Point(i32 a, i32 b) { x = a; y = b; } }
        class Vec<T> {
            private mut ptr<T> data;
            Vec() { data = malloc(64 * sizeof(T)); }
            fn setAt(i32 i) mut { data[i] = Point(2, 3); }
        }
        fn main() -> i32 { mut Vec<Point>& v = new Vec<Point>(); v.setAt(0); return 0; }
    )");
    REQUIRE(ir.find("store %Point zeroinitializer") != std::string::npos);
    REQUIRE(ir.find("call void @Point_Point(ptr ") != std::string::npos);
    REQUIRE(ir.find("call void @Point_clone(") == std::string::npos);   // no clone — constructed in place
    REQUIRE(ir.find("objtmp") == std::string::npos);                    // no temporary object built
}

TEST_CASE("ArrayObj - data[i] = data[j] (non-ctor RHS) still deep-clones", "[array-object][codegen]") {
    // The RHS is an IndexExpr (an existing element's address), not a CallExpr — proves the new
    // direct-construct branch doesn't over-fire on a non-constructor value. (The T*-borrow-param
    // case, `push(value)`, is already covered by the very first test in this file.)
    std::string ir = codegenString(R"(
        extern malloc(u64 size) -> ptr;
        class Point { mut i32 x; mut i32 y; Point(i32 a, i32 b) { x = a; y = b; } }
        class Vec<T> {
            private mut ptr<T> data;
            Vec() { data = malloc(64 * sizeof(T)); }
            fn copyWithin(i32 i, i32 j) mut { data[i] = data[j]; }
        }
        fn main() -> i32 { mut Vec<Point>& v = new Vec<Point>(); v.copyWithin(0, 1); return 0; }
    )");
    REQUIRE(ir.find("call void @Point_clone(ptr ") != std::string::npos);
}

TEST_CASE("ArrayObj - data[i] = freeFn() (CallExpr, non-class callee) falls through to clone", "[array-object][codegen]") {
    // The RHS is a CallExpr but its callee `makePoint` is a FUNCTION, not a class — so the
    // `cgClasses_.count(callee)` guard must reject the direct-construct fast path and fall back to
    // the object-returning-call sret temp + clone. (This same guard is what excludes the callable-
    // object sugar `obj(args)`, whose callee is also a non-class name.)
    std::string ir = codegenString(R"(
        extern malloc(u64 size) -> ptr;
        class Point { mut i32 x; mut i32 y; Point(i32 a, i32 b) { x = a; y = b; } }
        fn makePoint() -> Point p { p = Point(7, 8); }
        class Vec<T> {
            private mut ptr<T> data;
            Vec() { data = malloc(64 * sizeof(T)); }
            fn setAt(i32 i) mut { data[i] = makePoint(); }
        }
        fn main() -> i32 { mut Vec<Point>& v = new Vec<Point>(); v.setAt(0); return 0; }
    )");
    REQUIRE(ir.find("call void @makePoint(ptr ") != std::string::npos);   // sret temp materialized
    REQUIRE(ir.find("call void @Point_clone(ptr ") != std::string::npos); // then cloned in
    // The callee is NOT mistaken for a constructor `@makePoint_makePoint`.
    REQUIRE(ir.find("makePoint_makePoint") == std::string::npos);
}

TEST_CASE("ArrayObj - direct-construct fills a defaulted constructor parameter", "[array-object][codegen]") {
    // `data[i] = Point(5)` on a ctor `Point(i32 a, i32 b = 99)` must fill the default (b=99) through
    // the direct-construct path — proving `defaultsFor(mangledCtor)` is threaded into buildArgString.
    std::string ir = codegenString(R"(
        extern malloc(u64 size) -> ptr;
        class Point { mut i32 x; mut i32 y; Point(i32 a, i32 b = 99) { x = a; y = b; } }
        class Vec<T> {
            private mut ptr<T> data;
            Vec() { data = malloc(64 * sizeof(T)); }
            fn setAt(i32 i) mut { data[i] = Point(5); }
        }
        fn main() -> i32 { mut Vec<Point>& v = new Vec<Point>(); v.setAt(0); return 0; }
    )");
    REQUIRE(ir.find("call void @Point_Point(ptr ") != std::string::npos);
    REQUIRE(ir.find("i32 5, i32 99") != std::string::npos);   // default b=99 filled at the call site
    REQUIRE(ir.find("call void @Point_clone(") == std::string::npos);
}

TEST_CASE("ArrayObj - direct-construct reorders named constructor arguments", "[array-object][codegen]") {
    // `data[i] = Point(b: 2, a: 1)` must reorder the written args (b, a) into parameter order (a, b)
    // through the direct-construct path — proving `orderFor(&ctorCall)` is threaded into buildArgString.
    std::string ir = codegenString(R"(
        extern malloc(u64 size) -> ptr;
        class Point { mut i32 x; mut i32 y; Point(i32 a, i32 b) { x = a; y = b; } }
        class Vec<T> {
            private mut ptr<T> data;
            Vec() { data = malloc(64 * sizeof(T)); }
            fn setAt(i32 i) mut { data[i] = Point(b: 2, a: 1); }
        }
        fn main() -> i32 { mut Vec<Point>& v = new Vec<Point>(); v.setAt(0); return 0; }
    )");
    REQUIRE(ir.find("call void @Point_Point(ptr ") != std::string::npos);
    REQUIRE(ir.find("i32 1, i32 2") != std::string::npos);   // reordered to positional (a=1, b=2)
    REQUIRE(ir.find("call void @Point_clone(") == std::string::npos);
}

TEST_CASE("ArrayObj - data[i] = Class() with no user constructor is a zero-init no-op", "[array-object][codegen]") {
    std::string ir = codegenString(R"(
        extern malloc(u64 size) -> ptr;
        class Empty { mut i32 pad; }
        class Vec<T> {
            private mut ptr<T> data;
            Vec() { data = malloc(64 * sizeof(T)); }
            fn setAt(i32 i) mut { data[i] = Empty(); }
        }
        fn main() -> i32 { mut Vec<Empty>& v = new Vec<Empty>(); v.setAt(0); return 0; }
    )");
    REQUIRE(ir.find("store %Empty zeroinitializer") != std::string::npos);
    REQUIRE(ir.find("@Empty_Empty") == std::string::npos);
    REQUIRE(ir.find("@Empty_clone") == std::string::npos);
}

TEST_CASE("ArrayObj - data[i] = Class(args) resolves an overloaded constructor directly", "[array-object][codegen]") {
    std::string ir = codegenString(R"(
        extern malloc(u64 size) -> ptr;
        class Point {
            mut i32 x; mut i32 y;
            Point(i32 a, i32 b) { x = a; y = b; }
            Point(f64 a, f64 b) { x = a as i32; y = b as i32; }
        }
        class Vec<T> {
            private mut ptr<T> data;
            Vec() { data = malloc(64 * sizeof(T)); }
            fn setAt(i32 i) mut { data[i] = Point(1.0, 2.0); }
        }
        fn main() -> i32 { mut Vec<Point>& v = new Vec<Point>(); v.setAt(0); return 0; }
    )");
    REQUIRE(ir.find("call void @Point_Point$f64$f64$ret$void(ptr ") != std::string::npos);
    REQUIRE(ir.find("call void @Point_clone(") == std::string::npos);
}

TEST_CASE("ArrayObj - direct-construct args are read before the slot is zero-inited", "[array-object][codegen]") {
    // `data[i] = Point(data[i].x, data[i].y + 1)` must evaluate the constructor's arguments (which
    // read the OLD field values) BEFORE the slot is zero-initialized — reordering this the other way
    // would silently read zeros. The distinctive marker for "argument evaluation happened" is the
    // `add i32 ..., 1` from `data[i].y + 1`: it is unambiguously part of arg evaluation, unlike a
    // bare `load i32` (the LHS index load is emitted before this branch even runs, so it would
    // precede the zero-init regardless of the bug and can't isolate it). The field-access GEP
    // (`i32 0, i32 1`, distinct from the element-address GEP's `i64` index) is asserted too.
    std::string ir = codegenString(R"(
        extern malloc(u64 size) -> ptr;
        class Point { mut i32 x; mut i32 y; Point(i32 a, i32 b) { x = a; y = b; } }
        class Vec<T> {
            private mut ptr<T> data;
            Vec() { data = malloc(64 * sizeof(T)); }
            fn bump(i32 i) mut { data[i] = Point(data[i].x, data[i].y + 1); }
        }
        fn main() -> i32 { mut Vec<Point>& v = new Vec<Point>(); v.bump(0); return 0; }
    )");
    size_t fnPos   = ir.find("_bump(");
    REQUIRE(fnPos != std::string::npos);
    size_t addPos   = ir.find("add i32", fnPos);                     // the `+ 1` — part of arg eval
    size_t fieldGep = ir.find(", i32 0, i32 1", fnPos);              // the `.y` field GEP (not the i64 index GEP)
    size_t zeroPos  = ir.find("store %Point zeroinitializer", fnPos);
    REQUIRE(addPos   != std::string::npos);
    REQUIRE(fieldGep != std::string::npos);
    REQUIRE(zeroPos  != std::string::npos);
    REQUIRE(fieldGep < zeroPos);   // old field is read (GEP'd + loaded) before the slot is zeroed
    REQUIRE(addPos   < zeroPos);   // the argument expression is fully evaluated before the zero-init
}

TEST_CASE("ArrayObj - direct-construct zero-inits the whole aggregate before embedded-field ctor writes", "[array-object][codegen]") {
    // Line's ctor assigns two embedded Point fields (this.a = Point(...), this.b = Point(...)) —
    // those field writes route through genMemberAssign's clone path and rely on the field already
    // being zeroed (a null "release old reference" no-op). The outer zero-init must cover the whole
    // Line aggregate, and Line itself must be constructed directly (no top-level Line clone).
    std::string ir = codegenString(R"(
        extern malloc(u64 size) -> ptr;
        class Point { mut i32 x; mut i32 y; Point(i32 a, i32 b) { x = a; y = b; } }
        class Line {
            mut Point a; mut Point b;
            Line(i32 x1, i32 y1, i32 x2, i32 y2) { a = Point(x1, y1); b = Point(x2, y2); }
        }
        class Vec<T> {
            private mut ptr<T> data;
            Vec() { data = malloc(64 * sizeof(T)); }
            fn setAt(i32 i) mut { data[i] = Line(0, 0, 1, 1); }
        }
        fn main() -> i32 { mut Vec<Line>& v = new Vec<Line>(); v.setAt(0); return 0; }
    )");
    REQUIRE(ir.find("store %Line zeroinitializer") != std::string::npos);
    REQUIRE(ir.find("call void @Line_Line(ptr ") != std::string::npos);
    REQUIRE(ir.find("call void @Point_clone(") != std::string::npos);   // embedded fields, inside Line's own ctor
    REQUIRE(ir.find("call void @Line_clone(") == std::string::npos);    // Line itself: constructed, not cloned
}
