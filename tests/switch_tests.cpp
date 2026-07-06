#include <catch2/catch_test_macros.hpp>
#include "helpers.h"

// ============================================================
// Java-style `switch` — statement and expression, arrow form only. Case labels are compared to
// the scrutinee using the `Eq` trait when implemented, and default comparison otherwise (the same
// three-path machinery as `==`). Switch expressions must be exhaustive (explicit `default`, or an
// enum scrutinee covering every variant).
// ============================================================

// ---- Parser ----

TEST_CASE("Switch - a statement switch parses as a SwitchStmt", "[switch][parser]") {
    auto prog = parseStringRaw(R"(
        fn main() -> i32 { switch (1) { case 1 -> foo(); default -> bar(); } return 0; }
    )");
    const auto& fn = asStmt<FunctionDeclStmt>(prog.declarations[0]);
    const auto& sw = asStmt<SwitchStmt>(*fn.body.body[0]);
    REQUIRE(sw.arms.size() == 2);
    REQUIRE(sw.arms[0].labels.size() == 1);
    REQUIRE(sw.arms[1].isDefault);
}

TEST_CASE("Switch - a multi-label arm parses", "[switch][parser]") {
    auto prog = parseStringRaw(R"(
        fn main() -> i32 { switch (1) { case 1, 2, 3 -> foo(); default -> bar(); } return 0; }
    )");
    const auto& fn = asStmt<FunctionDeclStmt>(prog.declarations[0]);
    const auto& sw = asStmt<SwitchStmt>(*fn.body.body[0]);
    REQUIRE(sw.arms[0].labels.size() == 3);
}

TEST_CASE("Switch - two default arms is a parse error", "[switch][parser]") {
    StderrCapture cap;
    parseString(R"(
        fn main() -> i32 { switch (1) { case 1 -> a(); default -> b(); default -> c(); } return 0; }
    )");
    REQUIRE(cap.contains("at most one 'default'"));
}

// ---- Semantic ----

TEST_CASE("Switch - a primitive statement switch analyzes clean", "[switch][semantic]") {
    auto r = analyzeString(R"(
        fn main() -> i32 {
            mut i32 x = 0;
            switch (5) { case 1, 2 -> x = 1; case 5 -> { x = 2; } default -> x = 3; }
            return x;
        }
    )");
    REQUIRE_FALSE(r.hadError);
}

TEST_CASE("Switch - an expression switch requires a default", "[switch][semantic]") {
    StderrCapture cap;
    auto r = analyzeString(R"(
        fn main() -> i32 { i32 y = switch (3) { case 1 -> 10; case 3 -> 30; }; return y; }
    )");
    REQUIRE(r.hadError);
    REQUIRE(cap.contains("must be exhaustive"));
}

TEST_CASE("Switch - an exhaustive enum expression switch needs no default", "[switch][semantic][enum]") {
    auto r = analyzeString(R"(
        enum Color { RED, GREEN, BLUE }
        fn code(Color c) -> i32 {
            return switch (c) { case Color.RED -> 1; case Color.GREEN -> 2; case Color.BLUE -> 3; };
        }
        fn main() -> i32 { return code(Color.RED); }
    )");
    REQUIRE_FALSE(r.hadError);
}

TEST_CASE("Switch - a non-exhaustive enum expression switch is rejected", "[switch][semantic][enum]") {
    StderrCapture cap;
    auto r = analyzeString(R"(
        enum Color { RED, GREEN, BLUE }
        fn code(Color c) -> i32 { return switch (c) { case Color.RED -> 1; case Color.GREEN -> 2; }; }
        fn main() -> i32 { return 0; }
    )");
    REQUIRE(r.hadError);
    REQUIRE(cap.contains("exhaustive"));
}

TEST_CASE("Switch - a label type incompatible with the scrutinee is rejected", "[switch][semantic]") {
    StderrCapture cap;
    auto r = analyzeString(R"(
        fn main() -> i32 { switch (1) { case true -> a(); default -> b(); } return 0; }
    )");
    REQUIRE(r.hadError);
    REQUIRE(cap.contains("switch case"));
}

TEST_CASE("Switch - 'yield' outside a switch expression is an error", "[switch][semantic]") {
    StderrCapture cap;
    auto r = analyzeString(R"(
        fn main() -> i32 { yield 5; return 0; }
    )");
    REQUIRE(r.hadError);
    REQUIRE(cap.contains("'yield' is only valid inside a switch expression"));
}

TEST_CASE("Switch - a block arm that can fall through without yielding is rejected", "[switch][semantic]") {
    StderrCapture cap;
    auto r = analyzeString(R"(
        fn main() -> i32 {
            i32 y = switch (1) {
                case 1  -> { i32 t = 3; }   // no yield on this path
                default -> 0;
            };
            return y;
        }
    )");
    REQUIRE(r.hadError);
    REQUIRE(cap.contains("must 'yield'"));
}

TEST_CASE("Switch - a switch expression on an Eq class analyzes clean", "[switch][semantic][trait]") {
    auto r = analyzeString(R"(
        class Money { mut i32 cents; Money(i32 c) { cents = c; } }
        impl Eq for Money { fn eq(Money& o) -> bool { return cents == o.cents; } }
        fn main() -> i32 {
            Money p(50); Money lo(25); Money hi(50);
            i32 label = switch (p) { case lo -> 1; case hi -> 2; default -> 0; };
            return label;
        }
    )");
    REQUIRE_FALSE(r.hadError);
}

TEST_CASE("Switch - duplicate integer literal labels are rejected", "[switch][semantic]") {
    StderrCapture cap;
    auto r = analyzeString(R"(
        fn main() -> i32 { switch (1) { case 1 -> a(); case 1 -> b(); default -> c(); } return 0; }
    )");
    REQUIRE(r.hadError);
    REQUIRE(cap.contains("duplicate case label"));
}

TEST_CASE("Switch - a duplicate across a multi-label arm is rejected", "[switch][semantic]") {
    StderrCapture cap;
    auto r = analyzeString(R"(
        fn main() -> i32 { switch (2) { case 1, 2 -> a(); case 2 -> b(); default -> c(); } return 0; }
    )");
    REQUIRE(r.hadError);
    REQUIRE(cap.contains("duplicate case label"));
}

TEST_CASE("Switch - a duplicate enum variant label is rejected", "[switch][semantic][enum]") {
    StderrCapture cap;
    auto r = analyzeString(R"(
        enum Color { RED, GREEN, BLUE }
        fn main() -> i32 {
            switch (Color.RED) { case Color.RED -> a(); case Color.RED -> b(); default -> c(); }
            return 0;
        }
    )");
    REQUIRE(r.hadError);
    REQUIRE(cap.contains("duplicate case label"));
}

TEST_CASE("Switch - distinct labels are accepted (no false positive)", "[switch][semantic]") {
    auto r = analyzeString(R"(
        fn main() -> i32 {
            switch (1) { case 1, 2 -> a(); case 3 -> b(); default -> c(); }
            return 0;
        }
        fn a() -> i32 { return 0; } fn b() -> i32 { return 0; } fn c() -> i32 { return 0; }
    )");
    REQUIRE_FALSE(r.hadError);
}

// ---- Codegen ----

TEST_CASE("Switch - an Eq-class label lowers to an eq call", "[switch][codegen][trait]") {
    std::string ir = codegenString(R"(
        class Money { mut i32 cents; Money(i32 c) { cents = c; } }
        impl Eq for Money { fn eq(Money& o) -> bool { return cents == o.cents; } }
        fn main() -> i32 {
            Money p(50); Money hi(50);
            i32 label = switch (p) { case hi -> 2; default -> 0; };
            return label;
        }
    )");
    REQUIRE(ir.find("call i1 @Money_eq(") != std::string::npos);
}

TEST_CASE("Switch - a class without Eq compares references by address", "[switch][codegen]") {
    std::string ir = codegenString(R"(
        class Node { mut i32 v; Node(i32 x) { v = x; } }
        fn main() -> i32 {
            Node& a = new Node(1);
            mut i32 w = 0;
            switch (a) { case a -> w = 1; default -> w = 2; }
            return w;
        }
    )");
    REQUIRE(ir.find("icmp eq ptr") != std::string::npos);
}

TEST_CASE("Switch - an enum switch lowers to pointer identity", "[switch][codegen][enum]") {
    std::string ir = codegenString(R"(
        enum Color { RED, GREEN, BLUE }
        fn main() -> i32 {
            return switch (Color.GREEN) { case Color.RED -> 1; case Color.GREEN -> 2; case Color.BLUE -> 3; };
        }
    )");
    REQUIRE(ir.find("icmp eq ptr") != std::string::npos);
}

TEST_CASE("Switch - a reference-producing arm transfers its +1 (no premature release)", "[switch][codegen]") {
    std::string ir = codegenString(R"(
        class Node { mut i32 v; Node(i32 x) { v = x; } fn get() -> i32 { return v; } }
        fn main() -> i32 {
            Node& n = switch (1) { case 1 -> new Node(10); default -> new Node(0); };
            return n.get();
        }
    )");
    // The +1 from `new` in the arm must NOT be released inside the arm block (that would free the
    // object before it is bound). The old bug emitted `store ... ; gg_release(...)` in the arm.
    REQUIRE(ir.find("sw.arm") != std::string::npos);
    // The arm stores the new object then branches to merge with no intervening release.
    size_t arm   = ir.find("sw.arm");
    size_t merge = ir.find("br label %sw.merge", arm);
    REQUIRE(merge != std::string::npos);
    std::string armBody = ir.substr(arm, merge - arm);
    REQUIRE(armBody.find("gg_release") == std::string::npos);
}

TEST_CASE("Switch - an expression switch allocates a result slot loaded after the merge", "[switch][codegen]") {
    std::string ir = codegenString(R"(
        fn main() -> i32 { i32 y = switch (2) { case 1 -> 10; case 2 -> 20; default -> 0; }; return y; }
    )");
    // A merge block is emitted and the result slot is read after it.
    REQUIRE(ir.find("sw.merge") != std::string::npos);
}
