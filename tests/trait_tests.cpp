#include <catch2/catch_test_macros.hpp>
#include "helpers.h"

// ============================================================
// Traits / interfaces + operator overloading.
//
// A `trait` declares a contract (method signatures, optionally with a trailing
// `mut`). An `impl Trait for Type { ... }` block provides the bodies; its
// methods become methods on `Type` (mangled `@Type_method`). `Self` in a
// signature resolves to the implementing type. Dispatch is static — no vtables.
//
// Operators desugar to named trait methods: `a + b` -> `a.add(b)`, `a == b` ->
// `a.eq(b)`, `a < b` -> `a.cmp(b) < 0`, `-a` -> `a.neg()`, `a[i]` -> `a.get(i)`,
// `a[i] = v` -> `a.set(i, v)`. The overload is only active when the operand type
// implements the corresponding built-in trait (Add/Sub/Mul/Div/Rem/Eq/Ord/Neg/Index).
// ============================================================

// ------------------------------------------------------------
// Parser
// ------------------------------------------------------------

TEST_CASE("Trait - declaration parses with required (signature-only) methods", "[trait][parser]") {
    auto prog = parseString(R"(
        trait Shape {
            f64 area();
            f64 scale(f64 factor);
        }
    )");
    REQUIRE(prog.declarations.size() == 1);
    const auto& t = asStmt<TraitDeclStmt>(prog.declarations[0]);
    REQUIRE(t.name.lexeme == "Shape");
    REQUIRE(t.methods.size() == 2);
    REQUIRE(t.methods[0].name.lexeme == "area");
    REQUIRE_FALSE(t.methods[0].hasBody);        // signature-only
    REQUIRE_FALSE(t.methods[1].hasBody);
}

TEST_CASE("Trait - impl block parses trait + target + methods", "[trait][parser]") {
    auto prog = parseString(R"(
        trait Named { i32 tag(); }
        class Box { i32 v; Box(i32 x) { v = x; } }
        impl Named for Box {
            i32 tag() { return 7; }
        }
    )");
    REQUIRE(prog.declarations.size() == 3);
    const auto& impl = asStmt<ImplDeclStmt>(prog.declarations[2]);
    REQUIRE(impl.traitName.lexeme == "Named");
    REQUIRE(impl.typeName.lexeme == "Box");
    REQUIRE(impl.methods.size() == 1);
    REQUIRE(impl.methods[0].name.lexeme == "tag");
    REQUIRE(impl.methods[0].hasBody);
}

TEST_CASE("Trait - Self type parses in a trait signature", "[trait][parser]") {
    auto prog = parseString(R"(
        trait Combine { Self& merge(Self& other); }
    )");
    REQUIRE(prog.declarations.size() == 1);
    const auto& t = asStmt<TraitDeclStmt>(prog.declarations[0]);
    REQUIRE(t.methods.size() == 1);
    REQUIRE(t.methods[0].params.size() == 1);
}

// ------------------------------------------------------------
// Semantic — accepted
// ------------------------------------------------------------

TEST_CASE("Trait - impl satisfying a user trait is accepted", "[trait][semantic]") {
    auto result = analyzeString(R"(
        trait Named { i32 tag(); }
        class Box { i32 v; Box(i32 x) { v = x; } }
        impl Named for Box { i32 tag() { return 7; } }
        i32 main() {
            Box& b = new Box(3);
            return b.tag();
        }
    )");
    REQUIRE_FALSE(result.hadError);
}

TEST_CASE("Trait - Self in impl signature resolves to the target type", "[trait][semantic]") {
    auto result = analyzeString(R"(
        trait Combine { Self& merge(Self& other); }
        class Acc {
            mut i32 n;
            Acc(i32 x) { n = x; }
            i32 get() { return n; }
        }
        impl Combine for Acc {
            Acc& merge(Acc& other) { return new Acc(n + other.n); }
        }
        i32 main() {
            Acc& a = new Acc(2);
            Acc& b = new Acc(5);
            Acc& c = a.merge(b);
            return c.get();
        }
    )");
    REQUIRE_FALSE(result.hadError);
}

TEST_CASE("Trait - operator + via Add is accepted and typed", "[trait][operator][semantic]") {
    auto result = analyzeString(R"(
        class V { mut i32 x; V(i32 a) { x = a; } i32 get() { return x; } }
        impl Add for V { V& add(V& rhs) { return new V(x + rhs.x); } }
        i32 main() {
            V& a = new V(1);
            V& b = new V(2);
            V& c = a + b;
            return c.get();
        }
    )");
    REQUIRE_FALSE(result.hadError);
}

TEST_CASE("Trait - == / != via Eq yields bool", "[trait][operator][semantic]") {
    auto result = analyzeString(R"(
        class V { mut i32 x; V(i32 a) { x = a; } }
        impl Eq for V { bool eq(V& rhs) { return x == rhs.x; } }
        i32 main() {
            V& a = new V(1);
            V& b = new V(1);
            if (a == b) { return 0; }
            if (a != b) { return 1; }
            return 2;
        }
    )");
    REQUIRE_FALSE(result.hadError);
}

TEST_CASE("Trait - ordering via Ord yields bool for < <= > >=", "[trait][operator][semantic]") {
    auto result = analyzeString(R"(
        class V { mut i32 x; V(i32 a) { x = a; } }
        impl Ord for V { i32 cmp(V& rhs) { return x - rhs.x; } }
        i32 main() {
            V& a = new V(1);
            V& b = new V(2);
            bool r = (a < b) && (a <= b) && (b > a) && (b >= a);
            if (r) { return 0; }
            return 1;
        }
    )");
    REQUIRE_FALSE(result.hadError);
}

TEST_CASE("Trait - unary minus via Neg", "[trait][operator][semantic]") {
    auto result = analyzeString(R"(
        class V { mut i32 x; V(i32 a) { x = a; } i32 get() { return x; } }
        impl Neg for V { V& neg() { return new V(0 - x); } }
        i32 main() {
            V& a = new V(5);
            V& b = -a;
            return b.get();
        }
    )");
    REQUIRE_FALSE(result.hadError);
}

TEST_CASE("Trait - index get/set via Index", "[trait][operator][semantic]") {
    auto result = analyzeString(R"(
        class Pair {
            mut i32 a; mut i32 b;
            Pair(i32 x, i32 y) { a = x; b = y; }
        }
        impl Index for Pair {
            i32 get(i32 i) { if (i == 0) { return a; } return b; }
            void set(i32 i, i32 v) mut { if (i == 0) { a = v; } else { b = v; } }
        }
        i32 main() {
            mut Pair& p = new Pair(1, 2);
            p[0] = 9;
            return p[0];
        }
    )");
    REQUIRE_FALSE(result.hadError);
}

// ------------------------------------------------------------
// Semantic — rejected
// ------------------------------------------------------------

TEST_CASE("Trait - missing required method is an error", "[trait][semantic]") {
    StderrCapture cap;
    auto result = analyzeString(R"(
        trait Named { i32 tag(); }
        class Box { i32 v; Box(i32 x) { v = x; } }
        impl Named for Box { }
    )");
    REQUIRE(result.hadError);
}

TEST_CASE("Trait - impl for an unknown trait is an error", "[trait][semantic]") {
    StderrCapture cap;
    auto result = analyzeString(R"(
        class Box { i32 v; Box(i32 x) { v = x; } }
        impl Nonexistent for Box { i32 tag() { return 1; } }
    )");
    REQUIRE(result.hadError);
}

TEST_CASE("Trait - impl targeting a non-class type is an error", "[trait][semantic]") {
    StderrCapture cap;
    auto result = analyzeString(R"(
        trait Named { i32 tag(); }
        impl Named for i32 { i32 tag() { return 1; } }
    )");
    REQUIRE(result.hadError);
}

TEST_CASE("Trait - operator on a type with no matching impl is an error", "[trait][operator][semantic]") {
    StderrCapture cap;
    auto result = analyzeString(R"(
        class V { mut i32 x; V(i32 a) { x = a; } }
        i32 main() {
            V& a = new V(1);
            V& b = new V(2);
            V& c = a + b;
            return 0;
        }
    )");
    REQUIRE(result.hadError);
}

TEST_CASE("Trait - default (bodied) trait methods are rejected in v1", "[trait][semantic]") {
    StderrCapture cap;
    auto result = analyzeString(R"(
        trait Greet { i32 hello() { return 1; } }
    )");
    REQUIRE(result.hadError);
}

TEST_CASE("Trait - operator whose trait is not implemented errors even if another is", "[trait][operator][semantic]") {
    StderrCapture cap;
    // V implements Add but NOT Ord, so '<' must be rejected.
    auto result = analyzeString(R"(
        class V { mut i32 x; V(i32 a) { x = a; } }
        impl Add for V { V& add(V& r) { return new V(x + r.x); } }
        i32 main() {
            V& a = new V(1);
            V& b = new V(2);
            bool r = a < b;
            return 0;
        }
    )");
    REQUIRE(result.hadError);
    REQUIRE(cap.contains("Ord"));
}

// ------------------------------------------------------------
// Interaction with overloading
// ------------------------------------------------------------

TEST_CASE("Trait - operator method coexists with a same-named non-operator overload", "[trait][operator][overload][semantic]") {
    // `add(i32)` and the operator's `add(V&)` are two overloads of `add`; `a + b`
    // must resolve to add(V&), while a.add(1) resolves to add(i32).
    auto result = analyzeString(R"(
        class V {
            mut i32 x;
            V(i32 a) { x = a; }
            i32 add(i32 d) { return x + d; }
        }
        impl Add for V { V& add(V& r) { return new V(x + r.x); } }
        i32 main() {
            V& a = new V(10);
            V& b = new V(5);
            V& c = a + b;
            i32 n = a.add(3);
            return 0;
        }
    )");
    REQUIRE_FALSE(result.hadError);
}

TEST_CASE("Trait - overloaded operator method emits mangled add for a + b", "[trait][operator][overload][codegen]") {
    std::string ir = codegenString(R"(
        class V {
            mut i32 x;
            V(i32 a) { x = a; }
            i32 add(i32 d) { return x + d; }
        }
        impl Add for V { V& add(V& r) { return new V(x + r.x); } }
        i32 main() {
            V& a = new V(10);
            V& b = new V(5);
            V& c = a + b;
            return 0;
        }
    )");
    // The '+' desugars to the mangled add(V&) overload (base V_add is overloaded).
    REQUIRE(ir.find("@V_add$") != std::string::npos);
}

// ------------------------------------------------------------
// CodeGen
// ------------------------------------------------------------

TEST_CASE("Trait - impl method emits as @Type_method", "[trait][codegen]") {
    std::string ir = codegenString(R"(
        trait Named { i32 tag(); }
        class Box { i32 v; Box(i32 x) { v = x; } }
        impl Named for Box { i32 tag() { return 7; } }
    )");
    REQUIRE(ir.find("@Box_tag") != std::string::npos);
}

TEST_CASE("Trait - a + b lowers to a call of the add method", "[trait][operator][codegen]") {
    std::string ir = codegenString(R"(
        class V { mut i32 x; V(i32 a) { x = a; } }
        impl Add for V { V& add(V& rhs) { return new V(x + rhs.x); } }
        i32 main() {
            V& a = new V(1);
            V& b = new V(2);
            V& c = a + b;
            return 0;
        }
    )");
    REQUIRE(ir.find("call ptr @V_add") != std::string::npos);
}

TEST_CASE("Trait - a < b lowers to cmp call plus icmp against zero", "[trait][operator][codegen]") {
    std::string ir = codegenString(R"(
        class V { mut i32 x; V(i32 a) { x = a; } }
        impl Ord for V { i32 cmp(V& rhs) { return x - rhs.x; } }
        i32 main() {
            V& a = new V(1);
            V& b = new V(2);
            bool r = a < b;
            return 0;
        }
    )");
    REQUIRE(ir.find("call i32 @V_cmp") != std::string::npos);
    REQUIRE(ir.find("icmp slt") != std::string::npos);
}
