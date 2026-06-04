//
// Created by Vladimir Arsenijevic on 4.6.2026.
//

#include <catch2/catch_test_macros.hpp>
#include "helpers.h"

// Helper: check that the IR string contains a substring.
static bool irContains(const std::string& ir, const std::string& sub) {
    return ir.find(sub) != std::string::npos;
}

// ============================================================
// Function signatures
// ============================================================

TEST_CASE("CodeGen - function signature is emitted", "[codegen]") {
    auto ir = codegenString("i32 add(i32 a, i32 b) { return 0; }");
    REQUIRE(irContains(ir, "define i32 @add"));
}

TEST_CASE("CodeGen - parameters appear in signature", "[codegen]") {
    auto ir = codegenString("i32 add(i32 a, i32 b) { return 0; }");
    REQUIRE(irContains(ir, "i32 %a"));
    REQUIRE(irContains(ir, "i32 %b"));
}

TEST_CASE("CodeGen - multiple functions produce multiple define blocks", "[codegen]") {
    auto ir = codegenString(R"(
        i32 foo() { return 1; }
        i32 bar() { return 2; }
    )");
    REQUIRE(irContains(ir, "define i32 @foo"));
    REQUIRE(irContains(ir, "define i32 @bar"));
}

// ============================================================
// Variables
// ============================================================

TEST_CASE("CodeGen - local variable produces alloca and store", "[codegen]") {
    auto ir = codegenString(R"(
        i32 main() {
            i32 x = 42;
            return 0;
        }
    )");
    REQUIRE(irContains(ir, "alloca i32"));
    REQUIRE(irContains(ir, "store i32"));
}

TEST_CASE("CodeGen - reading a variable produces load", "[codegen]") {
    auto ir = codegenString(R"(
        i32 main() {
            i32 x = 5;
            return x;
        }
    )");
    REQUIRE(irContains(ir, "load i32"));
}

TEST_CASE("CodeGen - assignment stores new value", "[codegen]") {
    auto ir = codegenString(R"(
        i32 main() {
            i32 x = 1;
            x = 2;
            return x;
        }
    )");
    // Two stores: one for init, one for assignment
    size_t count = 0;
    size_t pos   = 0;
    while ((pos = ir.find("store i32", pos)) != std::string::npos) {
        ++count; ++pos;
    }
    REQUIRE(count >= 2);
}

// ============================================================
// Return
// ============================================================

TEST_CASE("CodeGen - return integer literal", "[codegen]") {
    auto ir = codegenString("i32 main() { return 0; }");
    REQUIRE(irContains(ir, "ret i32"));
}

TEST_CASE("CodeGen - return expression result", "[codegen]") {
    auto ir = codegenString(R"(
        i32 add(i32 a, i32 b) {
            i32 sum = a + b;
            return sum;
        }
    )");
    REQUIRE(irContains(ir, "ret i32"));
    REQUIRE(irContains(ir, "add i32"));
}

// ============================================================
// Arithmetic
// ============================================================

TEST_CASE("CodeGen - integer addition", "[codegen]") {
    auto ir = codegenString(R"(
        i32 main() { i32 x = 1 + 2; return x; }
    )");
    REQUIRE(irContains(ir, "add i32"));
}

TEST_CASE("CodeGen - integer subtraction", "[codegen]") {
    auto ir = codegenString(R"(
        i32 main() { i32 x = 10 - 3; return x; }
    )");
    REQUIRE(irContains(ir, "sub i32"));
}

TEST_CASE("CodeGen - integer multiplication", "[codegen]") {
    auto ir = codegenString(R"(
        i32 main() { i32 x = 4 * 5; return x; }
    )");
    REQUIRE(irContains(ir, "mul i32"));
}

TEST_CASE("CodeGen - signed integer division", "[codegen]") {
    auto ir = codegenString(R"(
        i32 main() { i32 x = 10 / 2; return x; }
    )");
    REQUIRE(irContains(ir, "sdiv i32"));
}

TEST_CASE("CodeGen - float addition uses fadd", "[codegen]") {
    auto ir = codegenString(R"(
        f64 main() { f64 x = 1.0 + 2.0; return x; }
    )");
    REQUIRE(irContains(ir, "fadd double"));
}

// ============================================================
// Comparison and logical
// ============================================================

TEST_CASE("CodeGen - less-than comparison", "[codegen]") {
    auto ir = codegenString(R"(
        i32 main() {
            i32 a = 3;
            i32 b = 5;
            bool c = a < b;
            return 0;
        }
    )");
    REQUIRE(irContains(ir, "icmp slt"));
}

// ============================================================
// Control flow
// ============================================================

TEST_CASE("CodeGen - if statement produces branch labels", "[codegen]") {
    auto ir = codegenString(R"(
        i32 main() {
            i32 x = 1;
            if (x) { x = 2; }
            return x;
        }
    )");
    REQUIRE(irContains(ir, "if.then."));
    REQUIRE(irContains(ir, "if.merge."));
}

TEST_CASE("CodeGen - if/else produces else label", "[codegen]") {
    auto ir = codegenString(R"(
        i32 main() {
            i32 x = 1;
            if (x) { x = 2; } else { x = 3; }
            return x;
        }
    )");
    REQUIRE(irContains(ir, "if.then."));
    REQUIRE(irContains(ir, "if.else."));
    REQUIRE(irContains(ir, "if.merge."));
}

TEST_CASE("CodeGen - while loop produces cond and body labels", "[codegen]") {
    auto ir = codegenString(R"(
        i32 main() {
            i32 i = 0;
            while (i < 10) { i = i + 1; }
            return i;
        }
    )");
    REQUIRE(irContains(ir, "while.cond."));
    REQUIRE(irContains(ir, "while.body."));
    REQUIRE(irContains(ir, "while.merge."));
}

TEST_CASE("CodeGen - for loop produces cond, body, and inc labels", "[codegen]") {
    auto ir = codegenString(R"(
        i32 main() {
            i32 s = 0;
            for (i32 i = 0; i < 5; i++) { s = s + i; }
            return s;
        }
    )");
    REQUIRE(irContains(ir, "for.cond."));
    REQUIRE(irContains(ir, "for.body."));
    REQUIRE(irContains(ir, "for.inc."));
    REQUIRE(irContains(ir, "for.merge."));
}

// ============================================================
// Function calls
// ============================================================

TEST_CASE("CodeGen - function call is emitted", "[codegen]") {
    auto ir = codegenString(R"(
        i32 square(i32 x) { return x * x; }
        i32 main() {
            i32 r = square(4);
            return r;
        }
    )");
    REQUIRE(irContains(ir, "call i32 @square"));
}

// ============================================================
// Casts
// ============================================================

TEST_CASE("CodeGen - widening cast i32 to i64 uses sext", "[codegen]") {
    auto ir = codegenString(R"(
        i32 main() {
            i32 small = 5;
            i64 big   = small;
            return 0;
        }
    )");
    REQUIRE(irContains(ir, "sext i32"));
}

TEST_CASE("CodeGen - integer to float uses sitofp", "[codegen]") {
    auto ir = codegenString(R"(
        i32 main() {
            i32 n = 3;
            f64 x = n;
            return 0;
        }
    )");
    REQUIRE(irContains(ir, "sitofp i32"));
}

// ============================================================
// Postfix and prefix operators
// ============================================================

TEST_CASE("CodeGen - postfix increment stores updated value", "[codegen]") {
    auto ir = codegenString(R"(
        i32 main() {
            i32 i = 0;
            i++;
            return i;
        }
    )");
    REQUIRE(irContains(ir, "add i32"));
    REQUIRE(irContains(ir, "store i32"));
}

TEST_CASE("CodeGen - prefix increment stores updated value", "[codegen]") {
    auto ir = codegenString(R"(
        i32 main() {
            i32 i = 0;
            ++i;
            return i;
        }
    )");
    REQUIRE(irContains(ir, "add i32"));
}
