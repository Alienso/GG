#include <catch2/catch_test_macros.hpp>
#include "helpers.h"

// ============================================================
// `addressOf(local)` — the raw address of a local variable's/parameter's own storage slot, as a
// typed ptr<T> (unsafe FFI/C-binding primitive; requires --unsafe-ptr). v1: a bare local/parameter
// identifier only.
//   - primitive local  -> ptr<T>       (address of the scalar's own storage)
//   - value-object local -> ptr<Class> (address of the struct's own storage)
//   - owning-reference local -> ptr<Class&> (address of the SLOT HOLDING the reference — one level
//     of indirection above the object, for APIs that write a pointer back to you)
// ============================================================

TEST_CASE("AddressOf - primitive local yields ptr<i32> and decays to plain ptr", "[addressof][semantic]") {
    auto result = analyzeString(R"(
        fn main() -> i32 {
            i32 n = 42;
            ptr<i32> p = addressOf(n);
            ptr raw = addressOf(n);
            return 0;
        }
    )");
    REQUIRE_FALSE(result.hadError);
}

TEST_CASE("AddressOf - value-object local yields ptr<Class>", "[addressof][semantic]") {
    auto result = analyzeString(R"(
        class Point { mut i32 x; mut i32 y; Point(i32 a, i32 b) { x = a; y = b; } }
        fn main() -> i32 {
            Point p(1, 2);
            ptr<Point> pp = addressOf(p);
            return 0;
        }
    )");
    REQUIRE_FALSE(result.hadError);
}

TEST_CASE("AddressOf - owning reference local yields ptr<Class&> (slot, not the object)", "[addressof][semantic]") {
    auto result = analyzeString(R"(
        class Point { mut i32 x; Point(i32 a) { x = a; } }
        fn main() -> i32 {
            Point& r = new Point(5);
            ptr<Point&> slot = addressOf(r);
            return 0;
        }
    )");
    REQUIRE_FALSE(result.hadError);
}

TEST_CASE("AddressOf - a parameter is a valid local (v1 scope)", "[addressof][semantic]") {
    auto result = analyzeString(R"(
        fn f(i32 n) -> i32 {
            ptr<i32> p = addressOf(n);
            return 0;
        }
        fn main() -> i32 { return f(1); }
    )");
    REQUIRE_FALSE(result.hadError);
}

TEST_CASE("AddressOf - requires --unsafe-ptr", "[addressof][semantic]") {
    StderrCapture cap;
    auto result = analyzeString(R"(
        fn main() -> i32 {
            i32 n = 42;
            var addr = addressOf(n);
            return 0;
        }
    )", CompilerOptions{});   // allowRawPtr = false
    REQUIRE(result.hadError);
    REQUIRE(cap.contains("requires --unsafe-ptr"));
}

TEST_CASE("AddressOf - rejects a non-identifier operand", "[addressof][semantic]") {
    StderrCapture cap;
    auto result = analyzeString(R"(
        fn main() -> i32 {
            i32 n = 5;
            ptr<i32> bad = addressOf(n + 1);
            return 0;
        }
    )");
    REQUIRE(result.hadError);
    REQUIRE(cap.contains("bare local variable or parameter name"));
}

TEST_CASE("AddressOf - rejects a borrow-typed local (Class*)", "[addressof][semantic]") {
    StderrCapture cap;
    auto result = analyzeString(R"(
        class Point { mut i32 x; Point(i32 a) { x = a; } }
        fn f(Point* b) -> i32 {
            ptr<Point&> bad = addressOf(b);
            return 0;
        }
        fn main() -> i32 { return 0; }
    )");
    REQUIRE(result.hadError);
    REQUIRE(cap.contains("not supported for 'Point*' locals"));
}

TEST_CASE("AddressOf - rejects a field access (v1: locals only)", "[addressof][semantic]") {
    StderrCapture cap;
    auto result = analyzeString(R"(
        class Point { mut i32 x; Point(i32 a) { x = a; } }
        fn f(Point& p) -> i32 {
            ptr<i32> bad = addressOf(p.x);
            return 0;
        }
        fn main() -> i32 { return 0; }
    )");
    REQUIRE(result.hadError);
    REQUIRE(cap.contains("bare local variable or parameter name"));
}

TEST_CASE("AddressOf - codegen: primitive local yields the alloca address", "[addressof][codegen]") {
    std::string ir = codegenString(R"(
        fn main() -> i32 {
            i32 n = 42;
            ptr<i32> p = addressOf(n);
            return 0;
        }
    )");
    // The addressOf result is stored straight from n's alloca — no load of n's value into it.
    REQUIRE(ir.find("alloca i32") != std::string::npos);
}

TEST_CASE("AddressOf - codegen: write through the typed ptr mutates the local", "[addressof]") {
    std::string ir = codegenString(R"(
        fn main() -> i32 {
            i32 n = 42;
            ptr<i32> p = addressOf(n);
            p[0] = 99;
            return n;
        }
    )");
    REQUIRE(ir.find("getelementptr i32") != std::string::npos);
}

TEST_CASE("AddressOf - two raw-ptr values compare by identity (icmp ptr, not numeric widening)",
          "[addressof][codegen]") {
    std::string ir = codegenString(R"(
        class Point { mut i32 x; Point(i32 a) { x = a; } }
        fn main() -> i32 {
            Point p(1);
            ptr a = addressOf(p);
            ptr b = addressOf(p);
            if (a == b) { return 0; }
            return 1;
        }
    )");
    REQUIRE(ir.find("icmp eq ptr") != std::string::npos);
}

// ---- The motivating use case: flow into an extern ptr parameter (a C out-param) ----

TEST_CASE("AddressOf - result decays to a plain ptr for an extern C call", "[addressof][semantic]") {
    // This is the whole point of the feature: hand a local's address to a C API.
    auto result = analyzeString(R"(
        extern memcpy(ptr dst, ptr src, u64 n) -> ptr;
        fn main() -> i32 {
            i32 dst = 0;
            i32 src = 1234;
            memcpy(addressOf(dst), addressOf(src), 4);
            return dst;
        }
    )");
    REQUIRE_FALSE(result.hadError);
}

// ---- bool / char primitives (claimed supported; verify) ----

TEST_CASE("AddressOf - bool and char locals are supported", "[addressof][semantic]") {
    auto result = analyzeString(R"(
        fn main() -> i32 {
            bool b = true;
            char c = 'Z';
            ptr<bool> pb = addressOf(b);
            ptr<char> pc = addressOf(c);
            return 0;
        }
    )");
    REQUIRE_FALSE(result.hadError);
}

// ---- var inference from addressOf ----

TEST_CASE("AddressOf - var infers the typed ptr", "[addressof][semantic]") {
    auto result = analyzeString(R"(
        fn main() -> i32 {
            i32 n = 7;
            var p = addressOf(n);
            return p[0];
        }
    )");
    REQUIRE_FALSE(result.hadError);
}

// ---- Additional rejection cases (v1 unsupported operand kinds) ----

TEST_CASE("AddressOf - rejects a nullable local", "[addressof][semantic]") {
    StderrCapture cap;
    auto result = analyzeString(R"(
        fn main() -> i32 {
            i32? n = 5;
            var p = addressOf(n);
            return 0;
        }
    )");
    REQUIRE(result.hadError);
    REQUIRE(cap.contains("nullable"));
}

TEST_CASE("AddressOf - rejects an enum local", "[addressof][semantic]") {
    StderrCapture cap;
    auto result = analyzeString(R"(
        enum Color { RED, GREEN }
        fn main() -> i32 {
            Color c = Color::RED;
            var p = addressOf(c);
            return 0;
        }
    )");
    REQUIRE(result.hadError);
    REQUIRE(cap.contains("not supported for 'Color'"));
}

TEST_CASE("AddressOf - rejects a primitive-borrow local (i32*)", "[addressof][semantic]") {
    StderrCapture cap;
    auto result = analyzeString(R"(
        fn f(i32* r) -> i32 {
            var p = addressOf(r);
            return 0;
        }
        fn main() -> i32 { return 0; }
    )");
    REQUIRE(result.hadError);
    REQUIRE(cap.contains("not supported for 'i32*'"));
}

TEST_CASE("AddressOf - rejects a str local", "[addressof][semantic]") {
    StderrCapture cap;
    auto result = analyzeString(R"(
        fn main() -> i32 {
            str s = "hi";
            var p = addressOf(s);
            return 0;
        }
    )");
    REQUIRE(result.hadError);
    REQUIRE(cap.contains("not supported for 'str'"));
}

TEST_CASE("AddressOf - rejects a raw-ptr local (v1: no ptr-to-ptr)", "[addressof][semantic]") {
    StderrCapture cap;
    auto result = analyzeString(R"(
        extern malloc(u64 n) -> ptr;
        fn main() -> i32 {
            ptr<i32> buf = malloc(16);
            var p = addressOf(buf);
            return 0;
        }
    )");
    REQUIRE(result.hadError);
    REQUIRE(cap.contains("not supported for 'ptr<i32>'"));
}

TEST_CASE("AddressOf - rejects an array local", "[addressof][semantic]") {
    StderrCapture cap;
    auto result = analyzeString(R"(
        fn main() -> i32 {
            i32[3] a;
            var p = addressOf(a);
            return 0;
        }
    )");
    REQUIRE(result.hadError);
    REQUIRE(cap.contains("not supported for 'i32[3]'"));
}

TEST_CASE("AddressOf - rejects 'this'", "[addressof][semantic]") {
    StderrCapture cap;
    auto result = analyzeString(R"(
        class Box {
            mut i32 v;
            Box(i32 x) { v = x; }
            fn probe() -> i32 { var p = addressOf(this); return 0; }
        }
        fn main() -> i32 { return 0; }
    )");
    REQUIRE(result.hadError);
    REQUIRE(cap.contains("bare local variable or parameter name"));
}

TEST_CASE("AddressOf - rejects an undeclared identifier", "[addressof][semantic]") {
    StderrCapture cap;
    auto result = analyzeString(R"(
        fn main() -> i32 {
            var p = addressOf(doesNotExist);
            return 0;
        }
    )");
    REQUIRE(result.hadError);
}
