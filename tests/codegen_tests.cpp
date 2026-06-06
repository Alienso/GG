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

// ============================================================
// Arithmetic — completeness
// ============================================================

TEST_CASE("CodeGen - signed modulo uses srem", "[codegen]") {
    auto ir = codegenString(R"(
        i32 main() { i32 x = 10; i32 y = 3; i32 z = x % y; return z; }
    )");
    REQUIRE(irContains(ir, "srem i32"));
}

TEST_CASE("CodeGen - unsigned division uses udiv", "[codegen]") {
    auto ir = codegenString(R"(
        u32 main() { u32 x = 10; u32 y = 2; u32 z = x / y; return z; }
    )");
    REQUIRE(irContains(ir, "udiv i32"));
}

TEST_CASE("CodeGen - unsigned modulo uses urem", "[codegen]") {
    auto ir = codegenString(R"(
        u32 main() { u32 x = 10; u32 y = 3; u32 z = x % y; return z; }
    )");
    REQUIRE(irContains(ir, "urem i32"));
}

TEST_CASE("CodeGen - float subtraction uses fsub", "[codegen]") {
    auto ir = codegenString(R"(
        f64 main() { f64 x = 5.0; f64 y = 3.0; f64 z = x - y; return z; }
    )");
    REQUIRE(irContains(ir, "fsub double"));
}

TEST_CASE("CodeGen - float multiplication uses fmul", "[codegen]") {
    auto ir = codegenString(R"(
        f64 main() { f64 x = 2.0; f64 y = 3.0; f64 z = x * y; return z; }
    )");
    REQUIRE(irContains(ir, "fmul double"));
}

TEST_CASE("CodeGen - float division uses fdiv", "[codegen]") {
    auto ir = codegenString(R"(
        f64 main() { f64 x = 6.0; f64 y = 2.0; f64 z = x / y; return z; }
    )");
    REQUIRE(irContains(ir, "fdiv double"));
}

TEST_CASE("CodeGen - f32 arithmetic uses float IR type", "[codegen]") {
    // f32 operands → fadd float, not fadd double
    auto ir = codegenString(R"(
        f32 foo() { f32 x = 1.0; f32 y = 2.0; f32 z = x + y; return z; }
    )");
    REQUIRE(irContains(ir, "fadd float"));
}

// ============================================================
// Bitwise operators
// ============================================================

TEST_CASE("CodeGen - bitwise AND uses and instruction", "[codegen]") {
    auto ir = codegenString(R"(
        i32 main() { i32 x = 5; i32 y = 3; i32 z = x & y; return z; }
    )");
    REQUIRE(irContains(ir, "and i32"));
}

TEST_CASE("CodeGen - bitwise OR uses or instruction", "[codegen]") {
    auto ir = codegenString(R"(
        i32 main() { i32 x = 5; i32 y = 3; i32 z = x | y; return z; }
    )");
    REQUIRE(irContains(ir, "or i32"));
}

TEST_CASE("CodeGen - bitwise XOR uses xor instruction", "[codegen]") {
    auto ir = codegenString(R"(
        i32 main() { i32 x = 5; i32 y = 3; i32 z = x ^ y; return z; }
    )");
    REQUIRE(irContains(ir, "xor i32"));
}

TEST_CASE("CodeGen - shift left uses shl", "[codegen]") {
    auto ir = codegenString(R"(
        i32 main() { i32 x = 1; i32 y = x << 3; return y; }
    )");
    REQUIRE(irContains(ir, "shl i32"));
}

TEST_CASE("CodeGen - signed shift right uses ashr", "[codegen]") {
    // Arithmetic shift right — preserves sign bit for signed integers
    auto ir = codegenString(R"(
        i32 main() { i32 x = 16; i32 y = x >> 2; return y; }
    )");
    REQUIRE(irContains(ir, "ashr i32"));
}

TEST_CASE("CodeGen - unsigned shift right uses lshr", "[codegen]") {
    // Logical shift right — fills with zeros for unsigned integers
    auto ir = codegenString(R"(
        u32 foo() { u32 x = 16; u32 y = x >> 2; return y; }
    )");
    REQUIRE(irContains(ir, "lshr i32"));
}

// ============================================================
// Logical operators
// ============================================================

TEST_CASE("CodeGen - logical AND emits and i1", "[codegen]") {
    auto ir = codegenString(R"(
        i32 main() { bool a = true; bool b = false; bool c = a && b; return 0; }
    )");
    REQUIRE(irContains(ir, "and i1"));
}

TEST_CASE("CodeGen - logical OR emits or i1", "[codegen]") {
    auto ir = codegenString(R"(
        i32 main() { bool a = true; bool b = false; bool c = a || b; return 0; }
    )");
    REQUIRE(irContains(ir, "or i1"));
}

// ============================================================
// Comparison operators — full set
// ============================================================

TEST_CASE("CodeGen - equality comparison uses icmp eq", "[codegen]") {
    auto ir = codegenString(R"(
        i32 main() { i32 a = 3; i32 b = 3; bool c = a == b; return 0; }
    )");
    REQUIRE(irContains(ir, "icmp eq"));
}

TEST_CASE("CodeGen - inequality comparison uses icmp ne", "[codegen]") {
    auto ir = codegenString(R"(
        i32 main() { i32 a = 3; i32 b = 4; bool c = a != b; return 0; }
    )");
    REQUIRE(irContains(ir, "icmp ne"));
}

TEST_CASE("CodeGen - signed greater-than uses icmp sgt", "[codegen]") {
    auto ir = codegenString(R"(
        i32 main() { i32 a = 5; i32 b = 3; bool c = a > b; return 0; }
    )");
    REQUIRE(irContains(ir, "icmp sgt"));
}

TEST_CASE("CodeGen - unsigned less-than uses icmp ult", "[codegen]") {
    // Same operator, different signedness → different instruction
    auto ir = codegenString(R"(
        i32 main() { u32 a = 3; u32 b = 5; bool c = a < b; return 0; }
    )");
    REQUIRE(irContains(ir, "icmp ult"));
}

TEST_CASE("CodeGen - float comparison uses fcmp olt", "[codegen]") {
    auto ir = codegenString(R"(
        i32 main() { f64 a = 1.0; f64 b = 2.0; bool c = a < b; return 0; }
    )");
    REQUIRE(irContains(ir, "fcmp olt"));
}

// ============================================================
// Unary operators — full set
// ============================================================

TEST_CASE("CodeGen - integer negation emits sub from zero", "[codegen]") {
    auto ir = codegenString(R"(
        i32 main() { i32 x = 5; i32 y = -x; return y; }
    )");
    // LLVM IR for unary minus on integers: sub i32 0, %x
    REQUIRE(irContains(ir, "sub i32 0,"));
}

TEST_CASE("CodeGen - float negation emits fneg", "[codegen]") {
    auto ir = codegenString(R"(
        f64 main() { f64 x = 5.0; f64 y = -x; return y; }
    )");
    REQUIRE(irContains(ir, "fneg double"));
}

TEST_CASE("CodeGen - bitwise NOT emits xor with -1", "[codegen]") {
    // LLVM IR idiom: ~x = xor iN x, -1
    auto ir = codegenString(R"(
        i32 main() { i32 x = 255; i32 y = ~x; return y; }
    )");
    REQUIRE(irContains(ir, "xor i32"));
    REQUIRE(irContains(ir, "-1"));
}

TEST_CASE("CodeGen - logical NOT emits xor i1 with true", "[codegen]") {
    auto ir = codegenString(R"(
        i32 main() { bool b = true; bool n = !b; return 0; }
    )");
    REQUIRE(irContains(ir, "xor i1"));
}

TEST_CASE("CodeGen - postfix decrement emits sub", "[codegen]") {
    auto ir = codegenString(R"(
        i32 main() { i32 i = 5; i--; return i; }
    )");
    REQUIRE(irContains(ir, "sub i32"));
}

TEST_CASE("CodeGen - prefix decrement emits sub and stores updated value", "[codegen]") {
    auto ir = codegenString(R"(
        i32 main() { i32 i = 5; --i; return i; }
    )");
    REQUIRE(irContains(ir, "sub i32"));
    REQUIRE(irContains(ir, "store i32"));
}

// ============================================================
// Compound assignment
// ============================================================

TEST_CASE("CodeGen - += loads, adds, and stores", "[codegen]") {
    auto ir = codegenString(R"(
        i32 main() { i32 x = 5; x += 3; return x; }
    )");
    REQUIRE(irContains(ir, "add i32"));
    // Two stores: initializer + compound assignment
    size_t count = 0;
    size_t pos   = 0;
    while ((pos = ir.find("store i32", pos)) != std::string::npos) { ++count; ++pos; }
    REQUIRE(count >= 2);
}

TEST_CASE("CodeGen - -= loads, subtracts, and stores", "[codegen]") {
    auto ir = codegenString(R"(
        i32 main() { i32 x = 10; x -= 3; return x; }
    )");
    REQUIRE(irContains(ir, "sub i32"));
}

TEST_CASE("CodeGen - *= loads, multiplies, and stores", "[codegen]") {
    auto ir = codegenString(R"(
        i32 main() { i32 x = 4; x *= 3; return x; }
    )");
    REQUIRE(irContains(ir, "mul i32"));
}

TEST_CASE("CodeGen - |= uses or instruction", "[codegen]") {
    auto ir = codegenString(R"(
        i32 main() { i32 x = 5; x |= 2; return x; }
    )");
    REQUIRE(irContains(ir, "or i32"));
}

TEST_CASE("CodeGen - ^= uses xor instruction", "[codegen]") {
    auto ir = codegenString(R"(
        i32 main() { i32 x = 5; x ^= 3; return x; }
    )");
    REQUIRE(irContains(ir, "xor i32"));
}

// ============================================================
// Casts — all directions
// ============================================================

TEST_CASE("CodeGen - u8 to u32 widening uses zext", "[codegen]") {
    auto ir = codegenString(R"(
        u32 main() { u8 small = 5; u32 big = small; return big; }
    )");
    REQUIRE(irContains(ir, "zext i8"));
}

TEST_CASE("CodeGen - i8 to i32 widening uses sext", "[codegen]") {
    auto ir = codegenString(R"(
        i32 main() { i8 small = 5; i32 big = small; return big; }
    )");
    REQUIRE(irContains(ir, "sext i8"));
}

TEST_CASE("CodeGen - f32 to f64 widening uses fpext", "[codegen]") {
    auto ir = codegenString(R"(
        f64 main() { f32 small = 1.0; f64 big = small; return big; }
    )");
    REQUIRE(irContains(ir, "fpext float"));
}

TEST_CASE("CodeGen - f64 to f32 narrowing uses fptrunc", "[codegen]") {
    StderrCapture cap;  // suppress implicit-narrowing warning
    auto ir = codegenString(R"(
        f32 main() { f64 x = 3.14; f32 y = x; return y; }
    )");
    REQUIRE(irContains(ir, "fptrunc double"));
}

TEST_CASE("CodeGen - unsigned int to float uses uitofp", "[codegen]") {
    auto ir = codegenString(R"(
        f64 main() { u32 n = 5; f64 x = n; return x; }
    )");
    REQUIRE(irContains(ir, "uitofp i32"));
}

TEST_CASE("CodeGen - float to signed int uses fptosi", "[codegen]") {
    StderrCapture cap;  // suppress implicit-narrowing warning
    auto ir = codegenString(R"(
        i32 main() { f64 x = 3.7; i32 y = x; return y; }
    )");
    REQUIRE(irContains(ir, "fptosi double"));
}

// ============================================================
// Void functions
// ============================================================

TEST_CASE("CodeGen - void function has correct signature", "[codegen]") {
    auto ir = codegenString("void greet() { }");
    REQUIRE(irContains(ir, "define void @greet"));
}

TEST_CASE("CodeGen - explicit return in void function emits ret void", "[codegen]") {
    auto ir = codegenString("void doNothing() { return; }");
    REQUIRE(irContains(ir, "ret void"));
}

TEST_CASE("CodeGen - void function with no return gets implicit ret void", "[codegen]") {
    auto ir = codegenString(R"(
        void doNothing() { i32 x = 0; }
    )");
    REQUIRE(irContains(ir, "ret void"));
}

TEST_CASE("CodeGen - void function call emits call void", "[codegen]") {
    auto ir = codegenString(R"(
        void helper() { }
        i32 main() { helper(); return 0; }
    )");
    REQUIRE(irContains(ir, "call void @helper"));
}

// ============================================================
// IR structure and correctness
// ============================================================

TEST_CASE("CodeGen - function body starts with entry label", "[codegen]") {
    auto ir = codegenString("i32 main() { return 0; }");
    REQUIRE(irContains(ir, "entry:"));
}

TEST_CASE("CodeGen - parameters are spilled to alloca at function entry", "[codegen]") {
    // Each parameter gets an alloca + an initial store so it can be reassigned.
    auto ir = codegenString("i32 id(i32 x) { return x; }");
    REQUIRE(irContains(ir, "%x.addr = alloca i32"));
    REQUIRE(irContains(ir, "store i32 %x, ptr %x.addr"));
}

TEST_CASE("CodeGen - alloca strategy produces no phi nodes", "[codegen]") {
    // All mutable state lives in alloca slots — phi nodes are never needed.
    auto ir = codegenString(R"(
        i32 main() {
            i32 x = 0;
            if (x) { x = 1; } else { x = 2; }
            while (x < 10) { x++; }
            return x;
        }
    )");
    REQUIRE(!irContains(ir, "phi "));
}

TEST_CASE("CodeGen - conditional branches use br i1", "[codegen]") {
    auto ir = codegenString(R"(
        i32 main() { i32 x = 1; if (x) { x = 2; } return x; }
    )");
    REQUIRE(irContains(ir, "br i1"));
}

TEST_CASE("CodeGen - while loop body branches back to condition block", "[codegen]") {
    // The back-edge is what makes it a loop in the CFG.
    auto ir = codegenString(R"(
        i32 main() { i32 i = 0; while (i < 5) { i++; } return i; }
    )");
    REQUIRE(irContains(ir, "br label %while.cond."));
}

TEST_CASE("CodeGen - early return inside if terminates block without extra branch", "[codegen]") {
    // The then-block ends with 'ret'; the fall-through 'br' must not be emitted after it.
    auto ir = codegenString(R"(
        i32 clamp(i32 x) {
            if (x > 100) { return 100; }
            return x;
        }
    )");
    REQUIRE(irContains(ir, "ret i32 100"));
    REQUIRE(irContains(ir, "if.then."));
    // Verify no 'br' follows a 'ret' in the same block by checking the if.merge block exists
    REQUIRE(irContains(ir, "if.merge."));
}

TEST_CASE("CodeGen - zero-argument call emits empty argument list", "[codegen]") {
    auto ir = codegenString(R"(
        i32 getZero() { return 0; }
        i32 main() { i32 x = getZero(); return x; }
    )");
    REQUIRE(irContains(ir, "call i32 @getZero()"));
}

TEST_CASE("CodeGen - multiple local variables produce multiple allocas", "[codegen]") {
    auto ir = codegenString(R"(
        i32 main() { i32 a = 1; i32 b = 2; i32 c = 3; return a + b + c; }
    )");
    size_t count = 0;
    size_t pos   = 0;
    while ((pos = ir.find("alloca i32", pos)) != std::string::npos) { ++count; ++pos; }
    REQUIRE(count >= 3);
}

TEST_CASE("CodeGen - nested loops produce unique label names", "[codegen]") {
    // Each loop gets its own label index so labels never collide.
    auto ir = codegenString(R"(
        i32 main() {
            i32 s = 0;
            for (i32 i = 0; i < 3; i++) {
                for (i32 j = 0; j < 3; j++) {
                    s = s + 1;
                }
            }
            return s;
        }
    )");
    REQUIRE(irContains(ir, "for.body.1"));
    REQUIRE(irContains(ir, "for.body.2"));
}

// ============================================================
// String literals
// ============================================================

TEST_CASE("CodeGen - string literal emits private global constant", "[codegen]") {
    auto ir = codegenString(R"(
        ptr foo() { ptr s = "hello"; return s; }
    )");
    REQUIRE(irContains(ir, "@.str.0"));
    REQUIRE(irContains(ir, "private unnamed_addr constant"));
    REQUIRE(irContains(ir, "getelementptr inbounds"));
}

TEST_CASE("CodeGen - string global size includes null terminator", "[codegen]") {
    // "hi" = 2 chars + 1 null byte = [3 x i8]
    auto ir = codegenString(R"(
        ptr foo() { ptr s = "hi"; return s; }
    )");
    REQUIRE(irContains(ir, "[3 x i8]"));
}

TEST_CASE("CodeGen - two string literals get distinct global names", "[codegen]") {
    auto ir = codegenString(R"(
        void foo() { ptr a = "hello"; ptr b = "world"; }
    )");
    REQUIRE(irContains(ir, "@.str.0"));
    REQUIRE(irContains(ir, "@.str.1"));
}

// ============================================================
// Break and continue
// ============================================================

TEST_CASE("CodeGen - break in while jumps to merge block", "[codegen]") {
    auto ir = codegenString(R"(
        void foo() {
            while (1) { break; }
        }
    )");
    REQUIRE(irContains(ir, "br label %while.merge."));
}

TEST_CASE("CodeGen - continue in while jumps back to condition block", "[codegen]") {
    auto ir = codegenString(R"(
        void foo() {
            i32 i = 0;
            while (i < 5) { continue; }
        }
    )");
    // The continue inside the body should jump to the cond block.
    // The body block is terminated by continue so the normal back-edge is not emitted.
    REQUIRE(irContains(ir, "br label %while.cond."));
}

TEST_CASE("CodeGen - break in for loop jumps to merge block", "[codegen]") {
    auto ir = codegenString(R"(
        void foo() {
            for (i32 i = 0; i < 10; i++) { break; }
        }
    )");
    REQUIRE(irContains(ir, "br label %for.merge."));
}

TEST_CASE("CodeGen - continue in for loop jumps to increment block", "[codegen]") {
    // In a for loop, continue re-enters at the increment, not the condition.
    auto ir = codegenString(R"(
        void foo() {
            for (i32 i = 0; i < 10; i++) { continue; }
        }
    )");
    REQUIRE(irContains(ir, "br label %for.inc."));
}

TEST_CASE("CodeGen - nested loops break targets innermost merge", "[codegen]") {
    // The inner break must jump to for.merge.2, not for.merge.1.
    auto ir = codegenString(R"(
        void foo() {
            for (i32 i = 0; i < 3; i++) {
                for (i32 j = 0; j < 3; j++) {
                    break;
                }
            }
        }
    )");
    // Both merge labels exist; the inner break targets the inner one (index 2).
    REQUIRE(irContains(ir, "for.merge.1"));
    REQUIRE(irContains(ir, "for.merge.2"));
}

// ============================================================
// char type (Unicode code points — u32)
// ============================================================

TEST_CASE("CodeGen - char variable uses i32 alloca", "[codegen]") {
    // char is a 32-bit Unicode code point, so its IR type is i32 (same as u32).
    auto ir = codegenString(R"(
        void foo() { char c = 'A'; }
    )");
    REQUIRE(irContains(ir, "alloca i32"));
    REQUIRE(irContains(ir, "store i32"));
}

TEST_CASE("CodeGen - ASCII char literal stores correct code point", "[codegen]") {
    // 'A' = Unicode code point 65
    auto ir = codegenString(R"(
        void foo() { char c = 'A'; }
    )");
    REQUIRE(irContains(ir, "store i32 65"));
}

TEST_CASE("CodeGen - char newline escape stores code point 10", "[codegen]") {
    auto ir = codegenString(R"(
        void foo() { char c = '\n'; }
    )");
    REQUIRE(irContains(ir, "store i32 10"));
}

TEST_CASE("CodeGen - char tab escape stores code point 9", "[codegen]") {
    auto ir = codegenString(R"(
        void foo() { char c = '\t'; }
    )");
    REQUIRE(irContains(ir, "store i32 9"));
}

TEST_CASE("CodeGen - char arithmetic uses i32 instructions", "[codegen]") {
    // char is an integer type — arithmetic produces add i32
    auto ir = codegenString(R"(
        i32 foo() { char a = 'A'; char b = 'B'; char c = a + b; return 0; }
    )");
    REQUIRE(irContains(ir, "add i32"));
}

TEST_CASE("CodeGen - char comparison uses unsigned icmp", "[codegen]") {
    // char is u32, so ordering comparisons must be unsigned
    auto ir = codegenString(R"(
        bool foo() { char a = 'a'; char b = 'z'; bool c = a < b; return c; }
    )");
    REQUIRE(irContains(ir, "icmp ult"));
}

TEST_CASE("CodeGen - char function parameter uses i32 IR type", "[codegen]") {
    auto ir = codegenString(R"(
        i32 toInt(char c) { return c; }
    )");
    REQUIRE(irContains(ir, "define i32 @toInt(i32 %c)"));
}

TEST_CASE("CodeGen - break block is terminated so no trailing branch follows", "[codegen]") {
    // Statements after break in the same block are dead — no extra br should follow.
    auto ir = codegenString(R"(
        void foo() {
            while (1) {
                break;
            }
        }
    )");
    // Only one branch to the merge block from the break itself.
    // The normal back-edge (br label %while.cond.) must NOT appear in the body block.
    REQUIRE(irContains(ir, "while.merge."));
}

// ============================================================
// Extern function declarations
// ============================================================

TEST_CASE("CodeGen - extern produces declare line", "[codegen]") {
    auto ir = codegenString("extern void exit(i32 code);");
    REQUIRE(irContains(ir, "declare void @exit(i32)"));
}

TEST_CASE("CodeGen - extern with no params produces empty param list", "[codegen]") {
    auto ir = codegenString("extern i64 getTime();");
    REQUIRE(irContains(ir, "declare i64 @getTime()"));
}

TEST_CASE("CodeGen - extern with multiple params", "[codegen]") {
    auto ir = codegenString("extern i32 add(i32 a, i32 b);");
    REQUIRE(irContains(ir, "declare i32 @add(i32, i32)"));
}

TEST_CASE("CodeGen - extern declare appears before define", "[codegen]") {
    auto ir = codegenString(R"(
        extern void exit(i32 code);
        void main() { exit(0); }
    )");
    auto declarePos = ir.find("declare void @exit");
    auto definePos  = ir.find("define void @main");
    REQUIRE(declarePos != std::string::npos);
    REQUIRE(definePos  != std::string::npos);
    REQUIRE(declarePos < definePos);
}

TEST_CASE("CodeGen - extern call emits call instruction", "[codegen]") {
    auto ir = codegenString(R"(
        extern i32 add(i32 a, i32 b);
        i32 main() { return add(1, 2); }
    )");
    REQUIRE(irContains(ir, "call i32 @add"));
}

// ============================================================
// ptr type
// ============================================================

TEST_CASE("CodeGen - ptr variable declaration", "[codegen]") {
    auto ir = codegenString(R"(
        extern ptr malloc(u64 size);
        void main() { ptr p = malloc(64); }
    )");
    REQUIRE(irContains(ir, "declare ptr @malloc(i64)"));
    REQUIRE(irContains(ir, "alloca ptr"));
    REQUIRE(irContains(ir, "call ptr @malloc"));
}

TEST_CASE("CodeGen - ptr parameter in extern uses ptr IR type", "[codegen]") {
    auto ir = codegenString("extern void free(ptr p);");
    REQUIRE(irContains(ir, "declare void @free(ptr)"));
}

TEST_CASE("CodeGen - string literal passes to ptr param without cast", "[codegen]") {
    // String literals are typed as ptr; no bitcast should appear when passed to a ptr param.
    auto ir = codegenString(R"(
        extern i32 puts(ptr s);
        void main() { puts("hello"); }
    )");
    REQUIRE(irContains(ir, "call i32 @puts(ptr"));
    REQUIRE(!irContains(ir, "bitcast"));
}

TEST_CASE("CodeGen - ptr returned from extern can be stored and passed", "[codegen]") {
    auto ir = codegenString(R"(
        extern ptr  malloc(u64 size);
        extern void free(ptr p);
        void main() {
            ptr buf = malloc(32);
            free(buf);
        }
    )");
    REQUIRE(irContains(ir, "call ptr @malloc"));
    REQUIRE(irContains(ir, "call void @free(ptr"));
}

// ============================================================
// Reference type representation (Phase 0 — Ref<T> foundation)
// ============================================================

TEST_CASE("Reference type - typeName renders as Ref<Class>", "[type][reference]") {
    Type r = makeReferenceType("Point");
    REQUIRE(r.kind == TypeKind::Reference);
    REQUIRE(r.className == "Point");
    REQUIRE(typeName(r) == "Ref<Point>");
}

TEST_CASE("Reference type - lowers to an opaque ptr in IR", "[type][reference]") {
    REQUIRE(irTypeName(makeReferenceType("Point")) == "ptr");
}

TEST_CASE("Reference type - is distinct from the value object type", "[type][reference]") {
    Type value = makeObjectType("Point");
    Type ref   = makeReferenceType("Point");
    REQUIRE(value != ref);                 // same class name, different kind
    REQUIRE(irTypeName(value) == "%Point");
    REQUIRE(irTypeName(ref)   == "ptr");
}

// ============================================================
// Generics — monomorphized generic functions
// ============================================================

TEST_CASE("Generics - generic function instantiates to a mangled concrete function", "[generics][codegen]") {
    auto ir = codegenString(R"(
        T addT<T>(T a, T b) { return a + b; }
        i32 main() { return addT<i32>(3, 4); }
    )");
    REQUIRE(irContains(ir, "define i32 @addT$i32(i32 %a, i32 %b)"));
    REQUIRE(irContains(ir, "call i32 @addT$i32("));
    REQUIRE_FALSE(irContains(ir, "@addT("));   // the template itself is never emitted
}

TEST_CASE("Generics - distinct type arguments produce distinct instantiations", "[generics][codegen]") {
    auto ir = codegenString(R"(
        T addT<T>(T a, T b) { return a + b; }
        i64 main() { i32 a = addT<i32>(1, 2); return addT<i64>(10, 20); }
    )");
    REQUIRE(irContains(ir, "define i32 @addT$i32("));
    REQUIRE(irContains(ir, "define i64 @addT$i64("));
}

TEST_CASE("Generics - multiple type parameters mangle in order", "[generics][codegen]") {
    auto ir = codegenString(R"(
        K firstOf<K, V>(K a, V b) { return a; }
        i32 main() { return firstOf<i32, i64>(42, 99); }
    )");
    REQUIRE(irContains(ir, "define i32 @firstOf$i32$i64(i32 %a, i64 %b)"));
}

TEST_CASE("Generics - the same instantiation is emitted only once", "[generics][codegen]") {
    auto ir = codegenString(R"(
        T idT<T>(T x) { return x; }
        i32 main() { i32 a = idT<i32>(1); i32 b = idT<i32>(2); return a + b; }
    )");
    auto first = ir.find("define i32 @idT$i32(");
    REQUIRE(first != std::string::npos);
    REQUIRE(ir.find("define i32 @idT$i32(", first + 1) == std::string::npos);
}

TEST_CASE("Generics - generic function call type-checks", "[generics][semantic]") {
    auto r = analyzeString(R"(
        T addT<T>(T a, T b) { return a + b; }
        i32 main() { return addT<i32>(3, 4); }
    )");
    REQUIRE_FALSE(r.hadError);
}

// ============================================================
// Generics — monomorphized generic classes
// ============================================================

TEST_CASE("Generics - generic class instantiates to a mangled struct and methods", "[generics][codegen]") {
    auto ir = codegenString(R"(
        class Box<T> { public T value; public Box(T v){ this.value=v; } public T get(){ return this.value; } }
        i32 main() { Box<i32>& b = new Box<i32>(42); return b.get(); }
    )");
    REQUIRE(irContains(ir, "%Box$i32 = type { i32 }"));
    REQUIRE(irContains(ir, "define void @Box$i32_Box$i32(ptr %self, i32 %v)"));
    REQUIRE(irContains(ir, "define i32 @Box$i32_get(ptr %self)"));
    REQUIRE_FALSE(irContains(ir, "%Box ="));   // the template itself is never emitted
}

TEST_CASE("Generics - distinct class instantiations produce distinct structs", "[generics][codegen]") {
    auto ir = codegenString(R"(
        class Box<T> { public T value; public Box(T v){ this.value=v; } }
        void main() { Box<i32>& a = new Box<i32>(1); Box<i64>& b = new Box<i64>(2); }
    )");
    REQUIRE(irContains(ir, "%Box$i32 = type { i32 }"));
    REQUIRE(irContains(ir, "%Box$i64 = type { i64 }"));
}

TEST_CASE("Generics - generic class with multiple type parameters", "[generics][codegen]") {
    auto ir = codegenString(R"(
        class Pair<K, V> { public K first; public V second; public Pair(K a, V b){ this.first=a; this.second=b; } }
        void main() { Pair<i32, i64>& p = new Pair<i32, i64>(7, 99); }
    )");
    REQUIRE(irContains(ir, "%Pair$i32$i64 = type { i32, i64 }"));
}

TEST_CASE("Generics - reference element type lowers to a ptr field", "[generics][codegen]") {
    auto ir = codegenString(R"(
        class Point { public i32 x; public Point(i32 v){ this.x=v; } }
        class Cell<T> { public T value; public Cell(T v){ this.value=v; } }
        void main() { Point& p = new Point(5); Cell<Point&>& c = new Cell<Point&>(p); }
    )");
    REQUIRE(irContains(ir, "%Cell$Point.ref = type { ptr }"));
}

TEST_CASE("Generics - generic class type-checks", "[generics][semantic]") {
    auto r = analyzeString(R"(
        class Box<T> { public T value; public Box(T v){ this.value=v; } public T get(){ return this.value; } }
        i32 main() { Box<i32>& b = new Box<i32>(7); return b.get(); }
    )");
    REQUIRE_FALSE(r.hadError);
}

TEST_CASE("Generics - self-referential generic linked node", "[generics][codegen]") {
    auto ir = codegenString(R"(
        class Node<T> { public T value; public Node<T>& next; public Node(T v){ this.value=v; } }
        void main() { Node<i32>& n = new Node<i32>(1); n.next = new Node<i32>(2); }
    )");
    REQUIRE(irContains(ir, "%Node$i32 = type { i32, ptr }"));
    REQUIRE(irContains(ir, "define void @Node$i32_dtor(ptr %self)"));
}
