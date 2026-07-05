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
            fn area() -> f64;
            fn scale(f64 factor) -> f64;
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
        trait Named { fn tag() -> i32; }
        class Box { i32 v; Box(i32 x) { v = x; } }
        impl Named for Box {
            fn tag() -> i32 { return 7; }
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
        trait Combine { fn merge(Self& other) -> Self&; }
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
        trait Named { fn tag() -> i32; }
        class Box { i32 v; Box(i32 x) { v = x; } }
        impl Named for Box { fn tag() -> i32 { return 7; } }
        fn main() -> i32 {
            Box& b = new Box(3);
            return b.tag();
        }
    )");
    REQUIRE_FALSE(result.hadError);
}

TEST_CASE("Trait - Self in impl signature resolves to the target type", "[trait][semantic]") {
    auto result = analyzeString(R"(
        trait Combine { fn merge(Self& other) -> Self&; }
        class Acc {
            mut i32 n;
            Acc(i32 x) { n = x; }
            fn get() -> i32 { return n; }
        }
        impl Combine for Acc {
            fn merge(Acc& other) -> Acc& { return new Acc(n + other.n); }
        }
        fn main() -> i32 {
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
        class V { mut i32 x; V(i32 a) { x = a; } fn get() -> i32 { return x; } }
        impl Add for V { fn add(V& rhs) -> V& { return new V(x + rhs.x); } }
        fn main() -> i32 {
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
        impl Eq for V { fn eq(V& rhs) -> bool { return x == rhs.x; } }
        fn main() -> i32 {
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
        impl Ord for V { fn cmp(V& rhs) -> i32 { return x - rhs.x; } }
        fn main() -> i32 {
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
        class V { mut i32 x; V(i32 a) { x = a; } fn get() -> i32 { return x; } }
        impl Neg for V { fn neg() -> V& { return new V(0 - x); } }
        fn main() -> i32 {
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
            fn get(i32 i) -> i32 { if (i == 0) { return a; } return b; }
            fn set(i32 i, i32 v) mut { if (i == 0) { a = v; } else { b = v; } }
        }
        fn main() -> i32 {
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
        trait Named { fn tag() -> i32; }
        class Box { i32 v; Box(i32 x) { v = x; } }
        impl Named for Box { }
    )");
    REQUIRE(result.hadError);
}

TEST_CASE("Trait - impl for an unknown trait is an error", "[trait][semantic]") {
    StderrCapture cap;
    auto result = analyzeString(R"(
        class Box { i32 v; Box(i32 x) { v = x; } }
        impl Nonexistent for Box { fn tag() -> i32 { return 1; } }
    )");
    REQUIRE(result.hadError);
}

TEST_CASE("Trait - impl targeting a non-class type is an error", "[trait][semantic]") {
    StderrCapture cap;
    auto result = analyzeString(R"(
        trait Named { fn tag() -> i32; }
        impl Named for i32 { fn tag() -> i32 { return 1; } }
    )");
    REQUIRE(result.hadError);
}

TEST_CASE("Trait - operator on a type with no matching impl is an error", "[trait][operator][semantic]") {
    StderrCapture cap;
    auto result = analyzeString(R"(
        class V { mut i32 x; V(i32 a) { x = a; } }
        fn main() -> i32 {
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
        trait Greet { fn hello() -> i32 { return 1; } }
    )");
    REQUIRE(result.hadError);
}

TEST_CASE("Trait - operator whose trait is not implemented errors even if another is", "[trait][operator][semantic]") {
    StderrCapture cap;
    // V implements Add but NOT Ord, so '<' must be rejected.
    auto result = analyzeString(R"(
        class V { mut i32 x; V(i32 a) { x = a; } }
        impl Add for V { fn add(V& r) -> V& { return new V(x + r.x); } }
        fn main() -> i32 {
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
            fn add(i32 d) -> i32 { return x + d; }
        }
        impl Add for V { fn add(V& r) -> V& { return new V(x + r.x); } }
        fn main() -> i32 {
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
            fn add(i32 d) -> i32 { return x + d; }
        }
        impl Add for V { fn add(V& r) -> V& { return new V(x + r.x); } }
        fn main() -> i32 {
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
        trait Named { fn tag() -> i32; }
        class Box { i32 v; Box(i32 x) { v = x; } }
        impl Named for Box { fn tag() -> i32 { return 7; } }
    )");
    REQUIRE(ir.find("@Box_tag") != std::string::npos);
}

TEST_CASE("Trait - a + b lowers to a call of the add method", "[trait][operator][codegen]") {
    std::string ir = codegenString(R"(
        class V { mut i32 x; V(i32 a) { x = a; } }
        impl Add for V { fn add(V& rhs) -> V& { return new V(x + rhs.x); } }
        fn main() -> i32 {
            V& a = new V(1);
            V& b = new V(2);
            V& c = a + b;
            return 0;
        }
    )");
    REQUIRE(ir.find("call ptr @V_add") != std::string::npos);
}

TEST_CASE("Trait - an operator returning an object value uses the sret slot convention", "[trait][operator][codegen]") {
    // Regression: an operator whose impl method returns an object *by value* must be called
    // through the sret convention (hidden slot ptr first, `void` return) — not as a by-value
    // struct return, which produced malformed IR (`%t = call %V @V_add` fed where a ptr is
    // expected). No heap allocation is involved.
    std::string ir = codegenString(R"(
        class V { mut i32 x; V(i32 a) { x = a; } }
        impl Add for V { fn add(V& rhs) -> V out { out.x = x + rhs.x; return out; } }
        fn main() -> i32 {
            V a(1);
            V b(2);
            V c = a + b;
            return 0;
        }
    )");
    REQUIRE(ir.find("call void @V_add(ptr ") != std::string::npos);   // sret call
    REQUIRE(ir.find("= call %V @V_add") == std::string::npos);        // NOT a by-value return
    REQUIRE(ir.find("@gg_alloc") == std::string::npos);               // no heap allocation
}

TEST_CASE("Trait - operator operands may be value objects (borrowed as references)", "[trait][operator][semantic][byref]") {
    // `a + b` with stack-value operands: the receiver and the `Self&` argument are borrowed
    // (address-of). Previously rejected with "no matching 'add' method".
    auto result = analyzeString(R"(
        class V { mut i32 x; V(i32 a) { x = a; } }
        impl Add for V { fn add(V& rhs) -> V out { out.x = x + rhs.x; return out; } }
        fn main() -> i32 {
            V a(1);
            V b(2);
            V c = a + b;
            return 0;
        }
    )");
    REQUIRE_FALSE(result.hadError);
}

TEST_CASE("Trait - a < b lowers to cmp call plus icmp against zero", "[trait][operator][codegen]") {
    std::string ir = codegenString(R"(
        class V { mut i32 x; V(i32 a) { x = a; } }
        impl Ord for V { fn cmp(V& rhs) -> i32 { return x - rhs.x; } }
        fn main() -> i32 {
            V& a = new V(1);
            V& b = new V(2);
            bool r = a < b;
            return 0;
        }
    )");
    REQUIRE(ir.find("call i32 @V_cmp") != std::string::npos);
    REQUIRE(ir.find("icmp slt") != std::string::npos);
}
