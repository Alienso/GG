#include <catch2/catch_test_macros.hpp>
#include "helpers.h"

// ============================================================
// `match` — destructuring pattern matching (statement + expression). Patterns: wildcard `_`,
// binding, literal (compared via ==), tuple `(p0, p1)`, struct `Class{ field: sub, field }`, nested.
// Reuses the switch arm-lowering skeleton: each arm is a pattern test-tree (field GEPs + emitEquality)
// + bindings. `switch` is untouched. Exhaustiveness (expression form) + reachability are enforced.
// ============================================================

TEST_CASE("match - a tuple pattern destructures via field GEPs and binds", "[match][codegen]") {
    std::string ir = codegenString(R"(
        fn add((i32, i32)* p) -> i32 {
            match p {
                (a, b) -> { return a + b; }
            }
            return 0;
        }
        fn main() -> i32 { (i32, i32) t = (2, 3); return add(t) - 5; }
    )");
    // The tuple pattern GEPs into the tuple struct to reach _0/_1.
    REQUIRE(ir.find("getelementptr %Tuple$i32$i32") != std::string::npos);
    // An irrefutable tuple pattern needs no equality test — the match branch is unconditional.
    REQUIRE(ir.find("mt.arm") != std::string::npos);
}

TEST_CASE("match - a struct pattern with a literal sub-pattern tests the field", "[match][codegen]") {
    std::string ir = codegenString(R"(
        class Point { i32 x; i32 y; Point(i32 a, i32 b) { x = a; y = b; } }
        fn classify(Point* p) -> i32 {
            match p {
                Point{ x: 0, y } -> { return 100 + y; }
                Point{ x, y }    -> { return x + y; }
            }
            return -1;
        }
        fn main() -> i32 { Point a(0, 7); return classify(a) - 107; }
    )");
    // The literal sub-pattern `x: 0` compares the field to 0.
    REQUIRE(ir.find("icmp eq i32") != std::string::npos);
    REQUIRE(ir.find("getelementptr %Point") != std::string::npos);
}

TEST_CASE("match - a match expression selects and yields a value", "[match][codegen]") {
    std::string ir = codegenString(R"(
        fn pick(i32 n) -> i32 {
            return match n {
                0 -> 10;
                _ -> 20;
            };
        }
        fn main() -> i32 { return pick(0) - 10; }
    )");
    // A literal arm compares the scrutinee; a result slot is stored and loaded.
    REQUIRE(ir.find("icmp eq i32") != std::string::npos);
    REQUIRE(ir.find("mt.merge") != std::string::npos);
}

TEST_CASE("match - nested tuple pattern chains GEPs", "[match][codegen]") {
    std::string ir = codegenString(R"(
        fn f(((i32, i32), i32)* p) -> i32 {
            match p {
                ((a, b), c) -> { return a + b + c; }
            }
            return 0;
        }
        fn main() -> i32 { ((i32,i32),i32) n = ((1,2),3); return f(n) - 6; }
    )");
    REQUIRE(ir.find("getelementptr %Tuple$Tuple$i32$i32$i32") != std::string::npos);
    REQUIRE(ir.find("getelementptr %Tuple$i32$i32") != std::string::npos);
}

TEST_CASE("match - tuple pattern arity mismatch is rejected", "[match][semantic]") {
    StderrCapture cap;
    auto result = analyzeString(R"(
        fn f((i32, i32)* p) -> i32 { match p { (a, b, c) -> { return a; } } return 0; }
        fn main() -> i32 { return 0; }
    )");
    REQUIRE(result.hadError);
    REQUIRE(cap.contains("tuple pattern has 3 element"));
}

TEST_CASE("match - struct pattern with an unknown field is rejected", "[match][semantic]") {
    StderrCapture cap;
    auto result = analyzeString(R"(
        class Point { i32 x; i32 y; Point(i32 a, i32 b) { x = a; y = b; } }
        fn f(Point* p) -> i32 { match p { Point{ z } -> { return 1; } } return 0; }
        fn main() -> i32 { return 0; }
    )");
    REQUIRE(result.hadError);
    REQUIRE(cap.contains("has no field 'z'"));
}

TEST_CASE("match - an arm after an irrefutable pattern is unreachable", "[match][semantic]") {
    StderrCapture cap;
    auto result = analyzeString(R"(
        fn f(i32 n) -> i32 { match n { x -> { return x; } 0 -> { return 1; } } return 0; }
        fn main() -> i32 { return 0; }
    )");
    REQUIRE(result.hadError);
    REQUIRE(cap.contains("unreachable"));
}

TEST_CASE("match - a non-exhaustive match expression is rejected", "[match][semantic]") {
    StderrCapture cap;
    auto result = analyzeString(R"(
        fn f(i32 n) -> i32 { i32 r = match n { 0 -> 1; }; return r; }
        fn main() -> i32 { return 0; }
    )");
    REQUIRE(result.hadError);
    REQUIRE(cap.contains("exhaustive"));
}

TEST_CASE("match - a binding pattern binds the whole scrutinee", "[match][codegen]") {
    std::string ir = codegenString(R"(
        fn f(i32 n) -> i32 { match n { x -> { return x + 1; } } return 0; }
        fn main() -> i32 { return f(41) - 42; }
    )");
    // A lone binding is irrefutable → the arm runs unconditionally (no equality test emitted for it).
    REQUIRE(ir.find("mt.arm") != std::string::npos);
}
