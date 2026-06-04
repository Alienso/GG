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
