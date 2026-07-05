#include <catch2/catch_test_macros.hpp>
#include "helpers.h"

namespace {
    // The body of `@main` only — excludes the emitted refcount runtime (whose gg_retain/release
    // null checks contain their own `icmp eq ptr`), so identity-comparison assertions are precise.
    std::string mainBody(const std::string& ir) {
        size_t start = ir.find("define i32 @main(");
        if (start == std::string::npos) return "";
        size_t end = ir.find("\n}", start);
        return end == std::string::npos ? ir.substr(start) : ir.substr(start, end - start);
    }
    // The body of the first function whose definition line contains `signature`.
    std::string functionBody(const std::string& ir, const std::string& signature) {
        size_t start = ir.find(signature);
        if (start == std::string::npos) return "";
        size_t end = ir.find("\n}", start);
        return end == std::string::npos ? ir.substr(start) : ir.substr(start, end - start);
    }
}

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

TEST_CASE("Trait - reference == / != without Eq compares by address (identity)",
          "[trait][operator][semantic][identity]") {
    // References default to address identity for ==/!= when the class does not implement Eq.
    auto result = analyzeString(R"(
        class N { mut i32 v; N(i32 x) { v = x; } }
        fn main() -> i32 {
            N& a = new N(1);
            N& b = new N(2);
            if (a == b) { return 1; }
            if (a != b) { return 0; }
            return 2;
        }
    )");
    REQUIRE_FALSE(result.hadError);
}

TEST_CASE("Trait - reference == without Eq lowers to icmp eq ptr", "[trait][operator][codegen][identity]") {
    std::string ir = codegenString(R"(
        class N { mut i32 v; N(i32 x) { v = x; } }
        fn main() -> i32 { N& a = new N(1); N& b = new N(2); if (a == b) { return 1; } return 0; }
    )");
    REQUIRE(mainBody(ir).find("icmp eq ptr") != std::string::npos);
    REQUIRE(ir.find("@N_eq") == std::string::npos);   // no structural dispatch
}

TEST_CASE("Trait - reference != without Eq lowers to icmp ne ptr", "[trait][operator][codegen][identity]") {
    std::string ir = codegenString(R"(
        class N { mut i32 v; N(i32 x) { v = x; } }
        fn main() -> i32 { N& a = new N(1); N& b = new N(2); if (a != b) { return 1; } return 0; }
    )");
    REQUIRE(mainBody(ir).find("icmp ne ptr") != std::string::npos);
}

TEST_CASE("Trait - an Eq impl overrides reference identity (structural)", "[trait][operator][codegen][identity]") {
    // With Eq implemented, `==` on references dispatches to `eq` — NOT a pointer compare.
    std::string ir = codegenString(R"(
        class V { mut i32 x; V(i32 a) { x = a; } }
        impl Eq for V { fn eq(V& rhs) -> bool { return x == rhs.x; } }
        fn main() -> i32 { V& a = new V(1); V& b = new V(1); if (a == b) { return 0; } return 1; }
    )");
    // Structural dispatch to eq (the comparison is a call, not a pointer compare). Note: the
    // emitted refcount runtime contains its own `icmp eq ptr` null checks, so a blanket negative
    // on that string is unreliable — the positive `@V_eq` call is the definitive signal.
    REQUIRE(ir.find("call i1 @V_eq") != std::string::npos);
}

TEST_CASE("Trait - a value-object == without Eq compares memberwise (structural)",
          "[trait][operator][semantic][identity]") {
    // Value objects have no address identity, so `==`/`!=` default to memberwise structural
    // equality (generated) rather than erroring.
    auto result = analyzeString(R"(
        class N { mut i32 v; N(i32 x) { v = x; } }
        fn dot(i32 a) -> i32 { N x(a); N y(a); if (x == y) { return 1; } return 0; }
        fn main() -> i32 { return 0; }
    )");
    REQUIRE_FALSE(result.hadError);
}

TEST_CASE("Trait - value-object == without Eq lowers to a generated structeq call",
          "[trait][operator][codegen][identity]") {
    std::string ir = codegenString(R"(
        class N { mut i32 v; N(i32 x) { v = x; } }
        fn main() -> i32 { N x(1); N y(1); if (x == y) { return 0; } return 1; }
    )");
    REQUIRE(ir.find("define i1 @N_structeq(ptr %a, ptr %b)") != std::string::npos);
    REQUIRE(mainBody(ir).find("call i1 @N_structeq") != std::string::npos);
}

TEST_CASE("Trait - structeq uses ordered float equality for float fields", "[trait][operator][codegen][identity]") {
    std::string ir = codegenString(R"(
        class FP { mut f64 a; mut f32 b; FP(f64 x, f32 y) { a = x; b = y; } }
        fn main() -> i32 { FP p(1.0, 2.0); FP q(1.0, 2.0); if (p == q) { return 0; } return 1; }
    )");
    std::string body = functionBody(ir, "define i1 @FP_structeq(");
    REQUIRE_FALSE(body.empty());
    REQUIRE(body.find("fcmp oeq double") != std::string::npos);
    REQUIRE(body.find("fcmp oeq float")  != std::string::npos);
}

TEST_CASE("Trait - structeq of a fieldless class is always equal", "[trait][operator][codegen][identity]") {
    std::string ir = codegenString(R"(
        class Empty { }
        fn main() -> i32 { Empty a; Empty b; if (a == b) { return 0; } return 1; }
    )");
    std::string body = functionBody(ir, "define i1 @Empty_structeq(");
    REQUIRE_FALSE(body.empty());
    REQUIRE(body.find("ret i1 1") != std::string::npos);
}

TEST_CASE("Trait - a value/reference mix without Eq compares structurally", "[trait][operator][semantic][identity]") {
    auto result = analyzeString(R"(
        class N { mut i32 v; N(i32 x) { v = x; } }
        fn main() -> i32 { N a(1); N& b = new N(1); if (a == b) { return 0; } return 1; }
    )");
    REQUIRE_FALSE(result.hadError);
}

TEST_CASE("Trait - structeq dispatches an embedded value field with an Eq impl to that eq",
          "[trait][operator][codegen][identity][valuefield]") {
    // Wallet has no Eq (memberwise), but its embedded Money field does — the parent's structeq
    // must call @Money_eq for that field, not compare it memberwise.
    std::string ir = codegenString(R"(
        class Money  { mut i32 d; mut i32 note; Money(i32 a, i32 b) { d = a; note = b; } }
        impl Eq for Money { fn eq(Money& o) -> bool { return d == o.d; } }
        class Wallet { mut Money m; mut i32 id; Wallet(Money& mm, i32 i) { m = mm; id = i; } }
        fn main() -> i32 { Money a(1,1); Wallet x(a, 0); Wallet y(a, 0); if (x == y) { return 0; } return 1; }
    )");
    std::string body = functionBody(ir, "define i1 @Wallet_structeq(");
    REQUIRE_FALSE(body.empty());
    REQUIRE(body.find("call i1 @Money_eq") != std::string::npos);         // field dispatched to its Eq
    REQUIRE(body.find("call i1 @Money_structeq") == std::string::npos);   // not memberwise
}

TEST_CASE("Trait - structeq compares an embedded value field recursively and a reference field by address",
          "[trait][operator][codegen][identity][valuefield]") {
    std::string ir = codegenString(R"(
        class P { mut i32 x; P(i32 a) { x = a; } }
        class N { mut i32 v; N(i32 x) { v = x; } }
        class L { mut P p; mut N& r; L(P& a, N& b) { p = a; r = b; } }
        fn main() -> i32 {
            P a(1); N& n = new N(2); L l1(a, n); L l2(a, n);
            if (l1 == l2) { return 0; } return 1;
        }
    )");
    std::string body = functionBody(ir, "define i1 @L_structeq(");
    REQUIRE_FALSE(body.empty());
    REQUIRE(body.find("call i1 @P_structeq") != std::string::npos);   // embedded value → recurse
    REQUIRE(body.find("icmp eq ptr") != std::string::npos);           // reference field → identity
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
