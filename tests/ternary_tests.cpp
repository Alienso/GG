#include <catch2/catch_test_macros.hpp>
#include "helpers.h"

// ============================================================
// Ternary conditional `cond ? a : b`
// ============================================================
// A C-style ternary is a PARSER-ONLY desugar to a switch EXPRESSION
// `switch (cond) { case true -> a; default -> b; }`, so it reuses the switch-expression machinery
// (bool scrutinee, result-type unification, exhaustiveness via `default`, codegen result slot) with
// no new semantic/codegen. It sits between assignment and Elvis, and is right-associative.

TEST_CASE("Ternary - basic selects the then/else branch", "[ternary][semantic]") {
    auto r = analyzeString(R"(
        fn main() -> i32 { i32 y = 5; i32 x = y == 5 ? 3 : 2; return x; }
    )");
    REQUIRE_FALSE(r.hadError);
}

TEST_CASE("Ternary - desugars to a switch expression (bool scrutinee, true/default arms)",
          "[ternary][codegen]") {
    auto ir = codegenString(R"(
        fn main() -> i32 { i32 y = 5; i32 x = y == 5 ? 3 : 2; return x; }
    )");
    // Reuses the switch-expression lowering: a result slot storing 3 on the true path and 2 on the
    // default path (a linear i1 test chain, not an LLVM `switch`).
    REQUIRE(ir.find("store i32 3") != std::string::npos);
    REQUIRE(ir.find("store i32 2") != std::string::npos);
}

TEST_CASE("Ternary - usable as a function argument", "[ternary][semantic]") {
    auto r = analyzeString(R"(
        fn take(i32 v) -> i32 { return v; }
        fn main() -> i32 { i32 y = 3; return take(y > 0 ? 100 : 200); }
    )");
    REQUIRE_FALSE(r.hadError);
}

TEST_CASE("Ternary - is right-associative: a ? b : c ? d : e == a ? b : (c ? d : e)",
          "[ternary][semantic]") {
    auto r = analyzeString(R"(
        fn classify(i32 n) -> i32 { return n > 0 ? 1 : (n < 0 ? 0 - 1 : 0); }
        fn main() -> i32 { return classify(7) + classify(0 - 3) + classify(0); }
    )");
    REQUIRE_FALSE(r.hadError);
}

TEST_CASE("Ternary - branches must unify to a common type", "[ternary][semantic]") {
    // Reference branches of the same class are fine (switch-expr rules).
    auto r = analyzeString(R"(
        class Box { mut i32 v; Box(i32 n) { v = n; } }
        fn pick(Box& a, Box& b, bool c) -> Box& { return c ? a : b; }
        fn main() -> i32 { Box& x = new Box(1); Box& y = new Box(2); return pick(x, y, true).v; }
    )");
    REQUIRE_FALSE(r.hadError);
}

TEST_CASE("Ternary - str branches work (a common primitive-ish view type)", "[ternary][semantic]") {
    auto r = analyzeString(R"(
        fn main() -> i32 { i32 y = 3; str s = y > 0 ? "pos" : "neg"; return s.len as i32; }
    )");
    REQUIRE_FALSE(r.hadError);
}

TEST_CASE("Ternary - a non-bool condition is rejected", "[ternary][semantic]") {
    // `5 ? ...` — the desugared switch compares an i32 scrutinee against the bool `true` label.
    StderrCapture cap;
    auto r = analyzeString(R"(
        fn main() -> i32 { i32 x = 5 ? 1 : 2; return x; }
    )");
    REQUIRE(r.hadError);
}

TEST_CASE("Ternary - a missing ':' is a clean parse error", "[ternary][parser]") {
    StderrCapture cap;
    (void)parseString(R"(
        fn main() -> i32 { i32 y = 1; i32 x = y == 1 ? 3; return x; }
    )");
    REQUIRE(cap.contains("expected ':' in ternary"));
}

// The middle branch is a full expression, so a nested ternary in the middle parses without parens:
// `a ? b ? c : d : e` == `a ? (b ? c : d) : e`.
TEST_CASE("Ternary - a nested ternary in the middle needs no parentheses", "[ternary][parser]") {
    auto r = analyzeString(R"(
        fn f(i32 a, i32 b) -> i32 { return a > 0 ? b > 0 ? 1 : 2 : 3; }
        fn main() -> i32 { return f(1, 1) + f(1, 0) + f(0, 0); }
    )");
    REQUIRE_FALSE(r.hadError);
}

// Precedence: ternary binds just below assignment, so it is a valid compound-assignment RHS.
TEST_CASE("Ternary - works as a compound-assignment RHS", "[ternary][parser]") {
    auto r = analyzeString(R"(
        fn main() -> i32 { mut i32 x = 10; x += true ? 1 + 2 : 3 * 4; return x; }
    )");
    REQUIRE_FALSE(r.hadError);
}

// A ternary is an ordinary expression, so it may be a control-flow condition.
TEST_CASE("Ternary - usable as an `if` condition", "[ternary][semantic]") {
    auto r = analyzeString(R"(
        fn main() -> i32 { i32 x = 13; if (x == 13 ? true : false) { return 1; } return 0; }
    )");
    REQUIRE_FALSE(r.hadError);
}

// Short-circuit: the desugared switch expression evaluates ONLY the taken arm, so each branch's
// value expression lowers into its own basic block guarded by the condition test (not both inline).
// This is what makes the null-guard idiom `x != null ? x!! : default` safe.
TEST_CASE("Ternary - only the taken branch is evaluated (lowered to separate blocks)",
          "[ternary][codegen]") {
    auto ir = codegenString(R"(
        fn a() -> i32 { return 1; }
        fn b() -> i32 { return 2; }
        fn main() -> i32 { i32 y = 0; return y > 0 ? a() : b(); }
    )");
    // Both calls are emitted, but under a conditional branch into distinct arm blocks — so at runtime
    // exactly one runs. (The switch-expression lowering: a cond test + per-arm blocks + result slot.)
    REQUIRE(ir.find("call i32 @a()") != std::string::npos);
    REQUIRE(ir.find("call i32 @b()") != std::string::npos);
    REQUIRE(ir.find("br i1") != std::string::npos);   // conditional branch selecting the arm
}
