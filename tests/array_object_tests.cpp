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
