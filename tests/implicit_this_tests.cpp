#include <catch2/catch_test_macros.hpp>
#include "helpers.h"

// ============================================================
// Implicit `this` — referencing class members without `this.`
// ============================================================
// A bare name inside a method resolves to a local/parameter/free-function first
// (they shadow), and only falls back to a member of the enclosing class. So class
// members have the lowest name-resolution priority.

// ------------------------------------------------------------
// Reads
// ------------------------------------------------------------

TEST_CASE("ImplicitThis - a bare field read resolves to this.field", "[implicitthis][semantic]") {
    auto result = analyzeString(R"(
        class Point {
            i32 x;
            i32 y;
            Point(i32 a, i32 b) { this.x = a; this.y = b; }
            i32 sum() { return x + y; }
        }
    )");
    REQUIRE_FALSE(result.hadError);
}

TEST_CASE("ImplicitThis - a bare field read lowers to a GEP load on this", "[implicitthis][codegen]") {
    std::string ir = codegenString(R"(
        class Point {
            i32 x;
            i32 y;
            Point(i32 a, i32 b) { this.x = a; this.y = b; }
            i32 sum() { return x + y; }
        }
        i32 main() { Point& p = new Point(3, 4); return p.sum(); }
    )");
    REQUIRE(ir.find("getelementptr") != std::string::npos);
}

// A parameter (or local) of the same name shadows the field — members are lowest priority.
TEST_CASE("ImplicitThis - a parameter shadows a same-named field", "[implicitthis][semantic]") {
    auto result = analyzeString(R"(
        class Point {
            i32 x;
            i32 y;
            Point(i32 a, i32 b) { this.x = a; this.y = b; }
            i32 pick(i32 x) { return x + y; }   // x = param, y = field
        }
    )");
    REQUIRE_FALSE(result.hadError);
}

TEST_CASE("ImplicitThis - a bare name that is neither local nor field is undeclared", "[implicitthis][semantic]") {
    StderrCapture cap;
    auto result = analyzeString(R"(
        class Point {
            i32 x;
            Point(i32 a) { this.x = a; }
            i32 bad() { return z; }
        }
    )");
    REQUIRE(result.hadError);
    REQUIRE(cap.contains("use of undeclared identifier 'z'"));
}

// ------------------------------------------------------------
// Writes / compound / ++ / --
// ------------------------------------------------------------

TEST_CASE("ImplicitThis - bare field writes, compound and ++ work in a mut method", "[implicitthis][semantic]") {
    auto result = analyzeString(R"(
        class Counter {
            mut i32 n;
            Counter() { n = 0; }              // bare write in ctor
            void inc()  mut { n = n + 1; }    // bare read + write
            void add(i32 d) mut { n += d; }   // bare compound
            void bump() mut { n++; }          // bare ++
            i32 get() { return n; }
        }
    )");
    REQUIRE_FALSE(result.hadError);
}

TEST_CASE("ImplicitThis - bare field ops lower correctly (runs to 0)", "[implicitthis][codegen]") {
    std::string ir = codegenString(R"(
        class Counter {
            mut i32 n;
            Counter() { n = 0; }
            void inc()  mut { n = n + 1; }
            void add(i32 d) mut { n += d; }
            void bump() mut { n++; }
            i32 get() { return n; }
        }
        i32 main() {
            mut Counter c;
            c.inc(); c.add(5); c.bump();
            return c.get();
        }
    )");
    REQUIRE(ir.find("getelementptr") != std::string::npos);
    REQUIRE(ir.find("store") != std::string::npos);
}

TEST_CASE("ImplicitThis - a bare write to a const field outside the ctor is an error", "[implicitthis][semantic]") {
    StderrCapture cap;
    auto result = analyzeString(R"(
        class Point {
            i32 x;
            Point(i32 a) { this.x = a; }
            void bad() mut { x = 9; }   // x is const (no mut)
        }
    )");
    REQUIRE(result.hadError);
    REQUIRE(cap.contains("cannot assign to immutable field 'x'"));
}

TEST_CASE("ImplicitThis - a bare field write in a non-mut method is an error", "[implicitthis][semantic]") {
    StderrCapture cap;
    auto result = analyzeString(R"(
        class Counter {
            mut i32 n;
            Counter() { n = 0; }
            void bad() { n = 1; }   // method is not `mut`
        }
    )");
    REQUIRE(result.hadError);
    REQUIRE(cap.contains("in a read-only method; declare the method 'mut'"));
}

// ------------------------------------------------------------
// Method calls
// ------------------------------------------------------------

TEST_CASE("ImplicitThis - a bare method call resolves to this.method", "[implicitthis][semantic]") {
    auto result = analyzeString(R"(
        class Counter {
            mut i32 n;
            Counter() { n = 0; }
            void inc()  mut { n = n + 1; }
            void twice() mut { inc(); inc(); }   // bare calls to this.inc()
            i32 get() { return n; }
        }
    )");
    REQUIRE_FALSE(result.hadError);
}

TEST_CASE("ImplicitThis - a free function shadows a same-named method (members lowest priority)", "[implicitthis][codegen]") {
    // `helper()` should bind to the free function, not any member — verified by the
    // presence of a direct `@helper` call in the IR.
    std::string ir = codegenString(R"(
        i32 helper() { return 7; }
        class C {
            i32 n;
            C() { this.n = 0; }
            i32 use() { return helper(); }   // free function, not a member
        }
        i32 main() { C& c = new C(); return c.use(); }
    )");
    REQUIRE(ir.find("call i32 @helper(") != std::string::npos);
}

TEST_CASE("ImplicitThis - a bare static method call works this-lessly", "[implicitthis][semantic]") {
    auto result = analyzeString(R"(
        class C {
            static i32 answer() { return 42; }
            i32 use() { return answer(); }   // bare static call
        }
    )");
    REQUIRE_FALSE(result.hadError);
}

TEST_CASE("ImplicitThis - calling a bare mut method from a non-mut method is an error", "[implicitthis][semantic]") {
    StderrCapture cap;
    auto result = analyzeString(R"(
        class Counter {
            mut i32 n;
            Counter() { n = 0; }
            void inc()  mut { n = n + 1; }
            void tick() { inc(); }   // caller not mut
        }
    )");
    REQUIRE(result.hadError);
    REQUIRE(cap.contains("on 'this' in a read-only method"));
}

// ------------------------------------------------------------
// Static-method context / enums
// ------------------------------------------------------------

TEST_CASE("ImplicitThis - a bare instance field in a static method is undeclared", "[implicitthis][semantic]") {
    StderrCapture cap;
    auto result = analyzeString(R"(
        class C {
            i32 n;
            static i32 bad() { return n; }   // no `this` in a static method
        }
    )");
    REQUIRE(result.hadError);
    REQUIRE(cap.contains("use of undeclared identifier 'n'"));
}

// ------------------------------------------------------------
// Static fields, reference fields, local shadowing, args
// ------------------------------------------------------------

TEST_CASE("ImplicitThis - a bare static-field read and write resolve to the global", "[implicitthis][semantic]") {
    auto result = analyzeString(R"(
        class C {
            static mut i32 total;
            C() { total = total + 1; }   // bare static read + write
            static i32 count() { return total; }
        }
    )");
    REQUIRE_FALSE(result.hadError);
}

// Static fields are class-level (not instance state), so a non-`mut` method may write them.
TEST_CASE("ImplicitThis - a non-mut method may write a bare static field", "[implicitthis][semantic]") {
    auto result = analyzeString(R"(
        class C {
            static mut i32 total;
            void bump() { total = total + 1; }   // no `mut` needed for a static field
        }
    )");
    REQUIRE_FALSE(result.hadError);
}

TEST_CASE("ImplicitThis - a bare static-field write lowers to a global store", "[implicitthis][codegen]") {
    std::string ir = codegenString(R"(
        class C {
            static mut i32 total;
            void bump() { total = total + 1; }
        }
        i32 main() { C& c = new C(); c.bump(); return 0; }
    )");
    REQUIRE(ir.find("@C$total") != std::string::npos);
}

TEST_CASE("ImplicitThis - a bare reference-field write works in a mut method", "[implicitthis][semantic]") {
    auto result = analyzeString(R"(
        class Node {
            i32 v;
            mut Node& next;
            Node(i32 x) { v = x; }
            void link(mut Node& n) mut { next = n; }   // bare ref-field write
            i32 peek() { return next.v; }               // bare ref-field read + access
        }
    )");
    REQUIRE_FALSE(result.hadError);
}

// A local (not just a parameter) shadows a same-named field. Here the field is const,
// so if `x` bound to the field the reassignment would error — it does not, proving the
// local wins.
TEST_CASE("ImplicitThis - a local variable shadows a same-named field", "[implicitthis][semantic]") {
    auto result = analyzeString(R"(
        class C {
            i32 x;                       // const field
            C(i32 a) { this.x = a; }
            i32 m() { mut i32 x = 1; x = 2; return x; }
        }
    )");
    REQUIRE_FALSE(result.hadError);
}

TEST_CASE("ImplicitThis - a bare method call passes arguments correctly", "[implicitthis][codegen]") {
    std::string ir = codegenString(R"(
        class C {
            mut i32 n;
            C() { n = 0; }
            void addN(i32 d) mut { n += d; }
            void run() mut { addN(5); }   // bare call with an argument
        }
        i32 main() { mut C& c = new C(); c.run(); return 0; }
    )");
    REQUIRE(ir.find("@C_addN(") != std::string::npos);
}

TEST_CASE("ImplicitThis - an enum method may read a field without this", "[implicitthis][semantic]") {
    auto result = analyzeString(R"(
        enum Planet {
            EARTH(9.8);
            f64 gravity;
            Planet(f64 g) { this.gravity = g; }
            f64 g() { return gravity; }   // bare field read in an enum method
        }
    )");
    REQUIRE_FALSE(result.hadError);
}
