#include <catch2/catch_test_macros.hpp>
#include "helpers.h"

// ============================================================
// `for (DECL : ITERABLE)` — the Java-style range loop. A PARSER-ONLY desugar to
//   { mut var __forit = (ITERABLE).iter(); while (__forit.hasNext()) { DECL = __forit.next(); BODY } }
// so it adds no AST / semantic / codegen. `next()` returns a `T*` borrow: a primitive loop variable
// (`i32 x`) loads the value, an object loop variable must be a borrow (`Point* p`). Iterability is by
// convention — the collection needs an `iter()` returning a cursor with `hasNext()`/`next()`, and a
// cursor conforms to the built-in `Iterator` trait.
// ============================================================

// A minimal iterable collection + its cursor. The cursor implements the built-in Iterator trait.
static const char* ITER_FIXTURE = R"(
    extern malloc(u64 size) -> ptr;
    class IntIter {
        mut ptr<i32> data; mut u64 len; mut u64 pos;
        IntIter(ptr<i32> b, u64 n) { data = b; len = n; pos = 0; }
    }
    impl Iterator for IntIter {
        fn hasNext() -> bool { return pos < len; }
        fn next() mut -> i32* { i32* e = data[pos]; pos = pos + 1; return e; }
    }
    class IntVec {
        mut ptr<i32> data; mut u64 count;
        IntVec() { data = malloc(16 * sizeof(i32)); count = 0; }
        fn add(i32 v) mut { data[count] = v; count = count + 1; }
        fn iter() -> IntIter it { it = IntIter(data, count); return it; }
    }
)";

TEST_CASE("foreach - desugars to iter()/hasNext()/next() over a while loop", "[foreach][codegen]") {
    std::string ir = codegenString(std::string(ITER_FIXTURE) + R"(
        fn main() -> i32 {
            mut IntVec v = IntVec();
            v.add(7);
            mut i32 s = 0;
            for (i32 x : v) { s = s + x; }
            return s;
        }
    )");
    REQUIRE(ir.find("call void @IntVec_iter(") != std::string::npos);      // the auto-inserted .iter()
    REQUIRE(ir.find("@IntIter_hasNext(")       != std::string::npos);      // loop condition
    REQUIRE(ir.find("@IntIter_next(")          != std::string::npos);      // element fetch
    REQUIRE(ir.find("while.cond")              != std::string::npos);      // lowered to a while loop
}

TEST_CASE("foreach - a primitive loop variable iterates by value", "[foreach][codegen]") {
    // `i32 x = __forit.next()` — next() returns i32*, decayed (loaded) into the value `x`.
    std::string ir = codegenString(std::string(ITER_FIXTURE) + R"(
        fn main() -> i32 {
            mut IntVec v = IntVec();
            v.add(1); v.add(2);
            mut i32 s = 0;
            for (i32 x : v) { s = s + x; }
            return s;
        }
    )");
    REQUIRE(ir.find("@IntIter_next(") != std::string::npos);
    // the loaded i32 flows into the accumulation
    REQUIRE(ir.find("load i32") != std::string::npos);
}

// An object collection + borrow cursor.
static const char* OBJ_FIXTURE = R"(
    extern malloc(u64 size) -> ptr;
    class Point { mut i32 x; mut i32 y; Point(i32 a, i32 b) { x = a; y = b; } fn sum() -> i32 { return x + y; } }
    class PIter {
        mut ptr<Point> data; mut u64 len; mut u64 pos;
        PIter(ptr<Point> b, u64 n) { data = b; len = n; pos = 0; }
    }
    impl Iterator for PIter {
        fn hasNext() -> bool { return pos < len; }
        fn next() mut -> Point* { Point* e = data[pos]; pos = pos + 1; return e; }
    }
    class PVec {
        mut ptr<Point> data; mut u64 count;
        PVec() { data = malloc(16 * sizeof(Point)); count = 0; }
        fn add(i32 a, i32 b) mut { data[count] = Point(a, b); count = count + 1; }
        fn iter() -> PIter it { it = PIter(data, count); return it; }
    }
)";

TEST_CASE("foreach - an object loop variable iterates by borrow (no clone)", "[foreach][codegen]") {
    std::string ir = codegenString(std::string(OBJ_FIXTURE) + R"(
        fn main() -> i32 {
            mut PVec v = PVec();
            v.add(3, 4);
            mut i32 s = 0;
            for (Point* p : v) { s = s + p.sum(); }
            return s;
        }
    )");
    REQUIRE(ir.find("@PIter_next(")   != std::string::npos);
    REQUIRE(ir.find("@Point_sum(")    != std::string::npos);
    REQUIRE(ir.find("@Point_clone(")  == std::string::npos);   // borrowed, never copied
}

TEST_CASE("foreach - a bare object loop variable is rejected (must borrow)", "[foreach][semantic]") {
    StderrCapture cap;
    (void)parseString(std::string(OBJ_FIXTURE) + R"(
        fn main() -> i32 {
            mut PVec v = PVec();
            for (Point p : v) { }
            return 0;
        }
    )");
    REQUIRE(cap.contains("iterate 'Point' by borrow"));
    REQUIRE(cap.contains("Point* p"));
}

TEST_CASE("foreach - a bare enum loop variable is rejected (must borrow)", "[foreach][semantic]") {
    // Enums are reference-like objects, not primitives, so they follow the object rule: borrow them.
    StderrCapture cap;
    (void)parseString(std::string(ITER_FIXTURE) + R"(
        enum Color { RED, GREEN }
        fn main() -> i32 {
            mut IntVec v = IntVec();
            for (Color c : v) { }
            return 0;
        }
    )");
    REQUIRE(cap.contains("by borrow"));
}

TEST_CASE("foreach - a mutable borrow writes through to the element", "[foreach][codegen]") {
    // `for (mut Point* q : v) { q.x = ...; }` — a mutable borrow of each element; the store lands in
    // the buffer (a store into a GEP of the element address, not a copy).
    std::string ir = codegenString(std::string(OBJ_FIXTURE) + R"(
        fn main() -> i32 {
            mut PVec v = PVec();
            v.add(1, 1);
            for (mut Point* q : v) { q.x = q.x + 1; }
            return 0;
        }
    )");
    REQUIRE(ir.find("@PIter_next(")  != std::string::npos);
    REQUIRE(ir.find("@Point_clone(") == std::string::npos);
}

TEST_CASE("foreach - nested loops get independent cursors over the same collection", "[foreach][codegen]") {
    // Each `for` mints a fresh iterator via a fresh `iter()` call, so the two cursors are independent.
    std::string ir = codegenString(std::string(ITER_FIXTURE) + R"(
        fn main() -> i32 {
            mut IntVec v = IntVec();
            v.add(1); v.add(2);
            mut i32 n = 0;
            for (i32 a : v) { for (i32 b : v) { n = n + 1; } }
            return n;
        }
    )");
    // Two distinct cursor allocas (the desugar names them __forit_0 / __forit_1).
    REQUIRE(ir.find("__forit_0") != std::string::npos);
    REQUIRE(ir.find("__forit_1") != std::string::npos);
}

TEST_CASE("foreach - a C-style for is still parsed as a C-style for (no false range detection)", "[foreach][codegen]") {
    // The range detection keys on a ':' at paren-depth 1 that is not a ternary colon; a classic
    // three-clause for (with a ternary in the CONDITION) must stay C-style.
    std::string ir = codegenString(R"(
        fn main() -> i32 {
            mut i32 s = 0;
            for (mut i32 i = 0; i < 3; i = i + 1) { s = s + (i < 2 ? 10 : 20); }
            return s;
        }
    )");
    // The ternary colon in the CONDITION must not be mistaken for a range ':' — so no iter()/hasNext
    // desugar is emitted; it stays an ordinary counting loop.
    REQUIRE(ir.find(".iter(")  == std::string::npos);
    REQUIRE(ir.find("hasNext") == std::string::npos);
    REQUIRE(ir.find("@main")   != std::string::npos);
}

TEST_CASE("foreach - break and continue bind to the loop", "[foreach][codegen]") {
    std::string ir = codegenString(std::string(ITER_FIXTURE) + R"(
        fn main() -> i32 {
            mut IntVec v = IntVec(); v.add(1); v.add(2); v.add(3);
            mut i32 s = 0;
            for (i32 x : v) {
                if (x == 3) { break; }
                if (x == 1) { continue; }
                s = s + x;
            }
            return s;
        }
    )");
    // break/continue lower to branches back to the while cond / out to the merge — the loop compiles.
    REQUIRE(ir.find("@IntVec_iter(") != std::string::npos);
    REQUIRE(ir.find("while.cond") != std::string::npos);
}

TEST_CASE("foreach - a single-statement (unbraced) body works", "[foreach][codegen]") {
    std::string ir = codegenString(std::string(ITER_FIXTURE) + R"(
        fn main() -> i32 {
            mut IntVec v = IntVec(); v.add(4);
            mut i32 s = 0;
            for (i32 x : v) s = s + x;
            return s;
        }
    )");
    REQUIRE(ir.find("@IntIter_next(") != std::string::npos);
}

TEST_CASE("foreach - a primitive-borrow loop variable (i32*) is allowed", "[foreach][codegen]") {
    std::string ir = codegenString(std::string(ITER_FIXTURE) + R"(
        fn main() -> i32 {
            mut IntVec v = IntVec(); v.add(4); v.add(5);
            mut i32 s = 0;
            for (i32* x : v) { s = s + x; }
            return s;
        }
    )");
    REQUIRE(ir.find("@IntIter_next(") != std::string::npos);
}

TEST_CASE("foreach - a bare identifier iterable is iterated directly (no source binding)", "[foreach][codegen]") {
    // A place (local/field/index/this) is iterated in place — no `__forsrc` temp, no copy.
    std::string ir = codegenString(std::string(ITER_FIXTURE) + R"(
        fn main() -> i32 {
            mut IntVec v = IntVec(); v.add(1);
            mut i32 s = 0;
            for (i32 x : v) { s = s + x; }
            return s;
        }
    )");
    REQUIRE(ir.find("__forsrc") == std::string::npos);
}

TEST_CASE("foreach - a call-result iterable is bound to a source local (kept alive for the loop)", "[foreach][codegen]") {
    // A reference-returning call as the iterable must be bound to a `__forsrc` local so the collection
    // lives for the whole loop — otherwise its temp is released (dtor frees the buffer) at the
    // `.iter()` statement, before the loop runs (a use-after-free). See parseForInStmt.
    std::string ir = codegenString(std::string(ITER_FIXTURE) + R"(
        fn make() -> IntVec& { IntVec& r = new IntVec(); return r; }
        fn main() -> i32 {
            mut i32 s = 0;
            for (i32 x : make()) { s = s + x; }
            return s;
        }
    )");
    REQUIRE(ir.find("__forsrc") != std::string::npos);   // the collection is bound, not iterated as a bare temp
}

TEST_CASE("foreach - Iterable is a built-in trait; a class with iter() conforms", "[foreach][iterable]") {
    // A collection formally declares iterability via `impl Iterable` (conformance = "has an iter()
    // method", the built-in convention). Here iter() lives in the class body; the empty impl block
    // conforms because the method exists. `@implements` then reflects the membership.
    std::string ir = codegenString(std::string(ITER_FIXTURE) + R"(
        impl Iterable for IntVec { }
        fn main() -> i32 {
            if (@implements(IntVec, Iterable)) { return 0; }
            return 1;
        }
    )");
    REQUIRE(ir.find("@main") != std::string::npos);   // compiles: Iterable recognized + conformance ok
}

TEST_CASE("foreach - impl Iterable without an iter() method is a conformance error", "[foreach][iterable]") {
    StderrCapture cap;
    (void)analyzeString(R"(
        class C { mut i32 n; C() { n = 0; } }
        impl Iterable for C { }
        fn main() -> i32 { return 0; }
    )");
    REQUIRE(cap.contains("built-in trait 'Iterable'"));
    REQUIRE(cap.contains("must define method 'iter'"));
}

TEST_CASE("foreach - iter() may live in the impl Iterable block", "[foreach][iterable]") {
    // The idiomatic form: the method satisfying the trait lives in the impl (mirrors the stdlib Array).
    std::string ir = codegenString(R"(
        extern malloc(u64 size) -> ptr;
        class It { mut ptr<i32> data; mut u64 len; mut u64 pos; It(ptr<i32> b, u64 n){data=b;len=n;pos=0;} }
        impl Iterator for It { fn hasNext() -> bool { return pos<len; } fn next() mut -> i32* { i32* e = data[pos]; pos=pos+1; return e; } }
        class Bag {
            mut ptr<i32> data; mut u64 count;
            Bag() { data = malloc(64 * sizeof(i32)); count = 0; }
            fn add(i32 v) mut { data[count] = v; count = count + 1; }
        }
        impl Iterable for Bag { fn iter() -> It it { it = It(data, count); return it; } }
        fn main() -> i32 {
            mut Bag b = Bag(); b.add(7);
            mut i32 s = 0;
            for (i32 x : b) { s = s + x; }
            return s;
        }
    )");
    REQUIRE(ir.find("@Bag_iter(") != std::string::npos);   // iter() attached to Bag from the impl
    REQUIRE(ir.find("@It_next(")  != std::string::npos);
}

TEST_CASE("foreach - a cursor may hold a borrow of the collection and read it live (realloc-safe)", "[foreach][codegen]") {
    // The realloc-safe cursor: hold `Coll*` + index and read size()/get() LIVE through the borrow,
    // rather than snapshotting the buffer pointer. Requires --unsafe-ptr (borrow field). Mirrors the
    // stdlib Array iterator — a mid-iteration realloc is harmless because next() goes through the
    // collection's current buffer.
    std::string ir = codegenString(R"(
        extern malloc(u64 size) -> ptr;
        class Vec {
            private mut ptr<i32> data; private mut u64 count;
            Vec() { data = malloc(64 * sizeof(i32)); count = 0; }
            fn add(i32 v) mut { data[count] = v; count = count + 1; }
            fn at(u64 i) -> i32* { return data[i]; }
            fn size() -> u64 { return count; }
        }
        class VecIter { mut Vec* v; mut u64 pos; }
        impl Iterator for VecIter {
            fn hasNext() -> bool { return pos < v.size(); }
            fn next() mut -> i32* { i32* e = v.at(pos); pos = pos + 1; return e; }
        }
        impl Iterable for Vec { fn iter() -> VecIter it { it.v = this; it.pos = 0; return it; } }
        fn main() -> i32 {
            mut Vec& a = new Vec(); a.add(7);
            mut i32 s = 0;
            for (i32 x : a) { s = s + x; }
            return s;
        }
    )");
    // next()/hasNext() read LIVE through the borrow — they call the collection's methods, not a snapshot.
    REQUIRE(ir.find("@Vec_size(") != std::string::npos);
    REQUIRE(ir.find("@Vec_at(")   != std::string::npos);
    // A borrow field is a plain ptr, never retained/released — so the cursor needs no destructor.
    REQUIRE(ir.find("@VecIter_dtor(") == std::string::npos);
}

TEST_CASE("foreach - a member-access iterable works and is not source-bound", "[foreach][codegen]") {
    std::string ir = codegenString(std::string(ITER_FIXTURE) + R"(
        class Holder { mut IntVec v; Holder() { v = IntVec(); } }
        fn main() -> i32 {
            mut Holder h = Holder(); h.v.add(6);
            mut i32 s = 0;
            for (i32 x : h.v) { s = s + x; }
            return s;
        }
    )");
    REQUIRE(ir.find("@IntVec_iter(") != std::string::npos);
    REQUIRE(ir.find("__forsrc")   == std::string::npos);   // a member place is iterated directly
}
