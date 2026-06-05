#include <catch2/catch_test_macros.hpp>
#include "helpers.h"

// ============================================================
// Valid programs — should produce no errors
// ============================================================

TEST_CASE("Semantic - valid function produces no error", "[semantic]") {
    auto result = analyzeString(R"(
        i32 add(i32 a, i32 b) {
            i32 sum = a + b;
            return sum;
        }
    )");
    REQUIRE_FALSE(result.hadError);
}

TEST_CASE("Semantic - variable shadowing in inner scope is allowed", "[semantic]") {
    // 'x' is declared in the outer block and again in an inner block.
    // The inner declaration should shadow without error.
    StderrCapture cap;
    auto result = analyzeString(R"(
        i32 main() {
            i32 x = 1;
            {
                i32 x = 2;
            }
            return x;
        }
    )");
    REQUIRE_FALSE(result.hadError);
}

TEST_CASE("Semantic - mutual recursion is allowed", "[semantic]") {
    // Both functions are hoisted in pass 1, so they can call each other.
    auto result = analyzeString(R"(
        i32 isEven(i32 n) { return isOdd(n); }
        i32 isOdd(i32 n)  { return isEven(n); }
    )");
    REQUIRE_FALSE(result.hadError);
}

TEST_CASE("Semantic - silent widening cast produces no error or warning", "[semantic]") {
    // i32 → i64 is a silent signed widening — no error, no warning.
    // Using i32 as the source because integer literals already default to i32.
    StderrCapture cap;
    auto result = analyzeString(R"(
        i32 main() {
            i32 small = 5;
            i64 big   = small;
            return 0;
        }
    )");
    REQUIRE_FALSE(result.hadError);
    REQUIRE_FALSE(cap.contains("Warning"));
    REQUIRE_FALSE(cap.contains("Error"));
}

// ============================================================
// Scoping errors
// ============================================================

TEST_CASE("Semantic - use of undeclared identifier is an error", "[semantic]") {
    StderrCapture cap;
    auto result = analyzeString(R"(
        i32 main() {
            return x;
        }
    )");
    REQUIRE(result.hadError);
    REQUIRE(cap.contains("undeclared"));
}

TEST_CASE("Semantic - redeclaration in the same scope is an error", "[semantic]") {
    StderrCapture cap;
    auto result = analyzeString(R"(
        i32 main() {
            i32 x = 1;
            i32 x = 2;
            return 0;
        }
    )");
    REQUIRE(result.hadError);
    REQUIRE(cap.contains("already declared"));
}

// ============================================================
// Type errors
// ============================================================

TEST_CASE("Semantic - assigning string to i32 is an error", "[semantic]") {
    StderrCapture cap;
    auto result = analyzeString(R"(
        i32 main() {
            i32 x = "hello";
            return 0;
        }
    )");
    REQUIRE(result.hadError);
    REQUIRE(cap.contains("cannot implicitly convert"));
}

TEST_CASE("Semantic - return type mismatch is an error", "[semantic]") {
    StderrCapture cap;
    auto result = analyzeString(R"(
        i32 main() {
            return "oops";
        }
    )");
    REQUIRE(result.hadError);
    REQUIRE(cap.contains("cannot implicitly convert"));
}

TEST_CASE("Semantic - calling function with wrong argument count is an error", "[semantic]") {
    StderrCapture cap;
    auto result = analyzeString(R"(
        i32 add(i32 a, i32 b) { return 0; }
        i32 main() {
            add(1);
            return 0;
        }
    )");
    REQUIRE(result.hadError);
    REQUIRE(cap.contains("expects"));
}

// ============================================================
// Warning casts (allowed but flagged)
// ============================================================

TEST_CASE("Semantic - f64 assigned to f32 produces a warning but no error", "[semantic]") {
    StderrCapture cap;
    auto result = analyzeString(R"(
        i32 main() {
            f32 x = 1.0;
            return 0;
        }
    )");
    // 1.0 is an f64 literal → f64 → f32 is Warn, not an error
    REQUIRE_FALSE(result.hadError);
    REQUIRE(cap.contains("Warning"));
    REQUIRE(cap.contains("f64"));
    REQUIRE(cap.contains("f32"));
}

TEST_CASE("Semantic - unsigned to signed of same size produces a warning but no error", "[semantic]") {
    // Declare u32 via a cast from i64 (silent widening path to avoid the
    // i32-literal → u32 warn from muddying the u32 → i32 warn we're testing).
    StderrCapture cap;
    auto result = analyzeString(R"(
        i32 main() {
            i64 tmp = 5;
            u32 a   = tmp;
            i32 b   = a;
            return 0;
        }
    )");
    // i64 → u32 and u32 → i32 are both Warn, not Error
    REQUIRE_FALSE(result.hadError);
    REQUIRE(cap.contains("Warning"));
}

// ============================================================
// void type misuse
// ============================================================

TEST_CASE("Semantic - void variable declaration is an error", "[semantic]") {
    StderrCapture cap;
    auto result = analyzeString(R"(
        void foo() { void x; }
    )");
    REQUIRE(result.hadError);
    REQUIRE(cap.contains("void"));
}

TEST_CASE("Semantic - void variable with initializer is an error", "[semantic]") {
    StderrCapture cap;
    auto result = analyzeString(R"(
        void foo() { void x = 0; }
    )");
    REQUIRE(result.hadError);
    REQUIRE(cap.contains("void"));
}

TEST_CASE("Semantic - void parameter type is an error", "[semantic]") {
    StderrCapture cap;
    auto result = analyzeString(R"(
        i32 foo(void x) { return 0; }
    )");
    REQUIRE(result.hadError);
    REQUIRE(cap.contains("void"));
}

TEST_CASE("Semantic - void function itself is valid", "[semantic]") {
    // 'void' as a *return* type is fine — the error only applies to variables/params.
    auto result = analyzeString(R"(
        void doNothing() { }
    )");
    REQUIRE_FALSE(result.hadError);
}

// ============================================================
// Missing return
// ============================================================

TEST_CASE("Semantic - non-void function with no return produces a warning", "[semantic]") {
    StderrCapture cap;
    auto result = analyzeString(R"(
        i32 foo() { }
    )");
    REQUIRE_FALSE(result.hadError);   // warning, not error
    REQUIRE(cap.contains("Warning"));
    REQUIRE(cap.contains("does not always return"));
}

TEST_CASE("Semantic - non-void function with unconditional return is clean", "[semantic]") {
    StderrCapture cap;
    auto result = analyzeString(R"(
        i32 foo() { return 42; }
    )");
    REQUIRE_FALSE(result.hadError);
    REQUIRE_FALSE(cap.contains("Warning"));
}

TEST_CASE("Semantic - if/else with both branches returning is clean", "[semantic]") {
    StderrCapture cap;
    auto result = analyzeString(R"(
        i32 sign(i32 x) {
            if (x > 0) { return 1; } else { return 0; }
        }
    )");
    REQUIRE_FALSE(result.hadError);
    REQUIRE_FALSE(cap.contains("Warning"));
}

TEST_CASE("Semantic - if without else branch warns about missing return", "[semantic]") {
    StderrCapture cap;
    auto result = analyzeString(R"(
        i32 foo(i32 x) {
            if (x > 0) { return 1; }
        }
    )");
    REQUIRE_FALSE(result.hadError);
    REQUIRE(cap.contains("Warning"));
    REQUIRE(cap.contains("does not always return"));
}

TEST_CASE("Semantic - return after if without else is clean", "[semantic]") {
    // The unconditional return at the bottom satisfies all paths.
    StderrCapture cap;
    auto result = analyzeString(R"(
        i32 clamp(i32 x) {
            if (x > 100) { return 100; }
            return x;
        }
    )");
    REQUIRE_FALSE(result.hadError);
    REQUIRE_FALSE(cap.contains("Warning"));
}

TEST_CASE("Semantic - void function never triggers missing-return warning", "[semantic]") {
    StderrCapture cap;
    auto result = analyzeString(R"(
        void doNothing() { }
    )");
    REQUIRE_FALSE(result.hadError);
    REQUIRE_FALSE(cap.contains("does not always return"));
}

// ============================================================
// return value in wrong context
// ============================================================

TEST_CASE("Semantic - return with no value in non-void function is an error", "[semantic]") {
    StderrCapture cap;
    auto result = analyzeString(R"(
        i32 foo() { return; }
    )");
    REQUIRE(result.hadError);
    REQUIRE(cap.contains("no value"));
}

TEST_CASE("Semantic - return with value in void function is an error", "[semantic]") {
    StderrCapture cap;
    auto result = analyzeString(R"(
        void foo() { return 42; }
    )");
    REQUIRE(result.hadError);
}

// ============================================================
// break / continue — valid uses
// ============================================================

TEST_CASE("Semantic - break inside while loop is valid", "[semantic]") {
    auto result = analyzeString(R"(
        void foo() { while (1) { break; } }
    )");
    REQUIRE_FALSE(result.hadError);
}

TEST_CASE("Semantic - continue inside while loop is valid", "[semantic]") {
    auto result = analyzeString(R"(
        void foo() { while (1) { continue; } }
    )");
    REQUIRE_FALSE(result.hadError);
}

TEST_CASE("Semantic - break inside for loop is valid", "[semantic]") {
    auto result = analyzeString(R"(
        void foo() { for (;;) { break; } }
    )");
    REQUIRE_FALSE(result.hadError);
}

TEST_CASE("Semantic - continue inside for loop is valid", "[semantic]") {
    auto result = analyzeString(R"(
        void foo() { for (;;) { continue; } }
    )");
    REQUIRE_FALSE(result.hadError);
}

TEST_CASE("Semantic - break inside if inside loop is valid", "[semantic]") {
    // The if is still lexically inside the loop.
    auto result = analyzeString(R"(
        void foo() {
            while (1) {
                if (1) { break; }
            }
        }
    )");
    REQUIRE_FALSE(result.hadError);
}

TEST_CASE("Semantic - break inside inner loop does not affect outer loop", "[semantic]") {
    auto result = analyzeString(R"(
        void foo() {
            while (1) {
                while (1) { break; }
            }
        }
    )");
    REQUIRE_FALSE(result.hadError);
}

// ============================================================
// break / continue — invalid uses
// ============================================================

TEST_CASE("Semantic - break outside any loop is an error", "[semantic]") {
    StderrCapture cap;
    auto result = analyzeString(R"(
        void foo() { break; }
    )");
    REQUIRE(result.hadError);
    REQUIRE(cap.contains("outside of a loop"));
}

TEST_CASE("Semantic - continue outside any loop is an error", "[semantic]") {
    StderrCapture cap;
    auto result = analyzeString(R"(
        void foo() { continue; }
    )");
    REQUIRE(result.hadError);
    REQUIRE(cap.contains("outside of a loop"));
}

TEST_CASE("Semantic - break inside function nested in loop is an error", "[semantic]") {
    // The inner function creates a new context; loop depth resets to 0 for it.
    StderrCapture cap;
    auto result = analyzeString(R"(
        void outer() {
            while (1) {
                void inner() { break; }
            }
        }
    )");
    REQUIRE(result.hadError);
    REQUIRE(cap.contains("outside of a loop"));
}

TEST_CASE("Semantic - break at top level is an error", "[semantic]") {
    StderrCapture cap;
    // Top-level break (no enclosing function or loop)
    auto result = analyzeString("i32 foo() { break; return 0; }");
    REQUIRE(result.hadError);
}

// ============================================================
// Extern function declarations
// ============================================================

TEST_CASE("Semantic - extern declaration is valid", "[semantic]") {
    auto result = analyzeString("extern i32 printf(i8 fmt);");
    REQUIRE_FALSE(result.hadError);
}

TEST_CASE("Semantic - extern function can be called", "[semantic]") {
    auto result = analyzeString(R"(
        extern i32 add(i32 a, i32 b);
        i32 main() { return add(1, 2); }
    )");
    REQUIRE_FALSE(result.hadError);
}

TEST_CASE("Semantic - duplicate extern declaration is an error", "[semantic]") {
    StderrCapture cap;
    auto result = analyzeString(R"(
        extern void exit(i32 code);
        extern void exit(i32 code);
    )");
    REQUIRE(result.hadError);
}

TEST_CASE("Semantic - extern and function with same name is an error", "[semantic]") {
    StderrCapture cap;
    auto result = analyzeString(R"(
        extern void foo();
        void foo() { }
    )");
    REQUIRE(result.hadError);
}

TEST_CASE("Semantic - extern call with wrong arg count is an error", "[semantic]") {
    StderrCapture cap;
    auto result = analyzeString(R"(
        extern void exit(i32 code);
        void main() { exit(1, 2); }
    )");
    REQUIRE(result.hadError);
}

TEST_CASE("Semantic - extern call with wrong arg type is an error", "[semantic]") {
    // Passing a string where i32 is expected — no implicit conversion exists.
    StderrCapture cap;
    auto result = analyzeString(R"(
        extern void exit(i32 code);
        void main() { exit("hello"); }
    )");
    REQUIRE(result.hadError);
}

// ============================================================
// ptr type
// ============================================================

TEST_CASE("Semantic - ptr variable is valid", "[semantic]") {
    auto result = analyzeString(R"(
        extern ptr malloc(u64 size);
        void main() { ptr p = malloc(64); }
    )");
    REQUIRE_FALSE(result.hadError);
}

TEST_CASE("Semantic - string literal has ptr type and passes ptr parameter", "[semantic]") {
    // String literals are typed as ptr (pointer to null-terminated char data).
    auto result = analyzeString(R"(
        extern i32 puts(ptr s);
        void main() { puts("hello"); }
    )");
    REQUIRE_FALSE(result.hadError);
}

TEST_CASE("Semantic - string is no longer a reserved keyword", "[semantic]") {
    // 'string' was formerly a type keyword; it is now a plain identifier.
    // It can therefore be used as a function name without error.
    auto result = analyzeString("i32 string() { return 42; }");
    REQUIRE_FALSE(result.hadError);
}

TEST_CASE("Semantic - ptr function parameter is valid", "[semantic]") {
    auto result = analyzeString(R"(
        void process(ptr data) { }
    )");
    REQUIRE_FALSE(result.hadError);
}

TEST_CASE("Semantic - ptr return type is valid", "[semantic]") {
    auto result = analyzeString(R"(
        extern ptr malloc(u64 n);
        ptr alloc(u64 size) { return malloc(size); }
    )");
    REQUIRE_FALSE(result.hadError);
}
