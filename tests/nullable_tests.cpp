#include <catch2/catch_test_macros.hpp>
#include "helpers.h"

// ============================================================
// Nullable references (Phase 1) — `T?`, null, `== null`, `!!`, `?:`, smart-casts.
// ============================================================
// A reference-like type may be made nullable with a postfix `?` (`Class&?`). It shares the `ptr`
// representation (null = machine null). `null` is assignable only to a nullable type; `T → T?` is
// an implicit widening; narrowing back needs `!!`, `?:`, or a smart-cast. Value objects and (in
// Phase 1) primitives may not be nullable. `?.` is deferred.

// ---- Parser ----

TEST_CASE("Nullable - `T&?` parses and `null` is a literal", "[nullable][parser]") {
    auto prog = parseString(R"(
        class C { i32 x; }
        fn main() -> i32 { C&? a = null; return 0; }
    )");
    REQUIRE(prog.declarations.size() == 2);
}

TEST_CASE("Nullable - nested `??` is a parse error", "[nullable][parser]") {
    StderrCapture cap;
    parseString("class C { i32 x; } fn f(C&?? a) { }");
    REQUIRE(cap.contains("nested '?\?'"));
}

// ---- Semantic: accepted ----

TEST_CASE("Nullable - a nullable reference accepts null and a non-null value", "[nullable][semantic]") {
    auto r = analyzeString(R"(
        class C { i32 x; C(i32 v) { x = v; } }
        fn main() -> i32 { mut C&? a = null; a = new C(1); return 0; }
    )");
    REQUIRE_FALSE(r.hadError);
}

TEST_CASE("Nullable - `== null` / `!= null` type-check to bool", "[nullable][semantic]") {
    auto r = analyzeString(R"(
        class C { i32 x; C(i32 v) { x = v; } }
        fn main() -> i32 {
            C&? a = null;
            if (a == null) { return 1; }
            if (a != null) { return 2; }
            return 0;
        }
    )");
    REQUIRE_FALSE(r.hadError);
}

TEST_CASE("Nullable - `!!` unwraps to the non-null type", "[nullable][semantic]") {
    auto r = analyzeString(R"(
        class C { mut i32 x; C(i32 v) { x = v; } }
        fn main() -> i32 { C&? a = new C(3); C& b = a!!; return b.x; }
    )");
    REQUIRE_FALSE(r.hadError);
}

TEST_CASE("Nullable - Elvis `?:` yields the non-null type", "[nullable][semantic]") {
    auto r = analyzeString(R"(
        class C { mut i32 x; C(i32 v) { x = v; } }
        fn main() -> i32 { C& fallback = new C(1); C&? a = null; C& b = a ?: fallback; return b.x; }
    )");
    REQUIRE_FALSE(r.hadError);
}

TEST_CASE("Nullable - a nullable return type is allowed", "[nullable][semantic]") {
    auto r = analyzeString(R"(
        class C { i32 x; C(i32 v) { x = v; } }
        fn pick(bool b) -> C&? { if (b) { return new C(1); } return null; }
        fn main() -> i32 { return 0; }
    )");
    REQUIRE_FALSE(r.hadError);
}

// ---- Smart-casts ----

TEST_CASE("Nullable - `if (x != null)` narrows the binding in the block", "[nullable][smartcast][semantic]") {
    auto r = analyzeString(R"(
        class C { mut i32 x; C(i32 v) { x = v; } fn get() -> i32 { return x; } }
        fn use(C&? a) -> i32 { if (a != null) { return a.get(); } return -1; }
    )");
    REQUIRE_FALSE(r.hadError);
}

TEST_CASE("Nullable - guard clause narrows after an early return", "[nullable][smartcast][semantic]") {
    auto r = analyzeString(R"(
        class C { mut i32 x; C(i32 v) { x = v; } }
        fn use(C&? a) -> i32 { if (a == null) { return 0; } return a.x; }
    )");
    REQUIRE_FALSE(r.hadError);
}

TEST_CASE("Nullable - reassigning a narrowed binding to a non-null value keeps the narrowing", "[nullable][smartcast][semantic]") {
    auto r = analyzeString(R"(
        class C { mut i32 x; C(i32 v) { x = v; } }
        fn main() -> i32 {
            mut C&? a = new C(1);
            if (a != null) { a = new C(2); return a.x; }
            return 0;
        }
    )");
    REQUIRE_FALSE(r.hadError);
}

// ---- Semantic: rejected ----

TEST_CASE("Nullable - member access on a possibly-null value is an error", "[nullable][semantic]") {
    StderrCapture cap;
    auto r = analyzeString(R"(
        class C { mut i32 x; C(i32 v) { x = v; } }
        fn use(C&? a) -> i32 { return a.x; }
    )");
    REQUIRE(r.hadError);
    REQUIRE(cap.contains("possibly-null"));
}

TEST_CASE("Nullable - reassigning a narrowed binding to null invalidates the narrowing", "[nullable][smartcast][semantic]") {
    StderrCapture cap;
    auto r = analyzeString(R"(
        class C { mut i32 x; C(i32 v) { x = v; } }
        fn main() -> i32 {
            mut C&? a = new C(1);
            if (a != null) { a = null; return a.x; }
            return 0;
        }
    )");
    REQUIRE(r.hadError);
    REQUIRE(cap.contains("possibly-null"));
}

TEST_CASE("Nullable - `null` cannot initialize a non-nullable reference", "[nullable][semantic]") {
    StderrCapture cap;
    auto r = analyzeString(R"(
        class C { i32 x; C(i32 v) { x = v; } }
        fn main() -> i32 { C& a = null; return 0; }
    )");
    REQUIRE(r.hadError);
    REQUIRE(cap.contains("cannot implicitly convert"));
}

TEST_CASE("Nullable - a nullable value cannot implicitly narrow to non-nullable", "[nullable][semantic]") {
    StderrCapture cap;
    auto r = analyzeString(R"(
        class C { i32 x; C(i32 v) { x = v; } }
        fn main() -> i32 { C&? a = new C(1); C& b = a; return 0; }
    )");
    REQUIRE(r.hadError);
    REQUIRE(cap.contains("cannot implicitly convert"));
}

TEST_CASE("Nullable - a value object cannot be nullable", "[nullable][semantic]") {
    StderrCapture cap;
    auto r = analyzeString(R"(
        class C { i32 x; C(i32 v) { x = v; } }
        fn main() -> i32 { C? a = null; return 0; }
    )");
    REQUIRE(r.hadError);
    REQUIRE(cap.contains("value object cannot be nullable"));
}

TEST_CASE("Nullable - a nullable primitive (`i32?`) is supported", "[nullable][primitive][semantic]") {
    auto r = analyzeString(R"(
        fn main() -> i32 {
            i32? a = 5;
            i32? b = null;
            if (a == null) { return 1; }
            i32 av = a!!;
            i32 bv = b ?: 37;
            return av + bv;
        }
    )");
    REQUIRE_FALSE(r.hadError);
}

TEST_CASE("Nullable - `ptr?` / `void?` are rejected", "[nullable][primitive][semantic]") {
    StderrCapture cap;
    auto r = analyzeString("fn main() -> i32 { ptr? a = null; return 0; }");
    REQUIRE(r.hadError);
    REQUIRE(cap.contains("cannot be nullable"));
}

TEST_CASE("Nullable - `?.` safe-call yields a nullable result", "[nullable][safecall][semantic]") {
    // `a?.get()` on a `C&?` returns `i32?` (the method returns i32, made nullable by `?.`).
    auto r = analyzeString(R"(
        class C { mut i32 x; C(i32 v) { x = v; } fn get() -> i32 { return x; } }
        fn use(C&? a) -> i32 { i32? g = a?.get(); return g ?: -1; }
    )");
    REQUIRE_FALSE(r.hadError);
}

TEST_CASE("Nullable - `?.` field access yields a nullable result", "[nullable][safecall][semantic]") {
    auto r = analyzeString(R"(
        class C { mut i32 x; C(i32 v) { x = v; } }
        fn use(C&? a) -> i32 { i32? v = a?.x; return v ?: -1; }
    )");
    REQUIRE_FALSE(r.hadError);
}

TEST_CASE("Nullable - `?.` cannot yield a value object", "[nullable][safecall][semantic]") {
    StderrCapture cap;
    auto r = analyzeString(R"(
        class P { mut i32 x; }
        class C { fn make() -> P p { p.x = 1; } }
        fn use(C&? a) -> i32 { P q = a?.make(); return 0; }
    )");
    REQUIRE(r.hadError);
}

TEST_CASE("Nullable - `?.` safe-call reorders/short-circuits on null (codegen)", "[nullable][safecall][codegen]") {
    // The receiver is null-checked (icmp ne ptr) and the result flows through a slot.
    std::string ir = codegenString(R"(
        class C { mut i32 x; C(i32 v) { x = v; } fn get() -> i32 { return x; } }
        fn use(C&? a) -> i32 { i32? g = a?.get(); return g ?: -1; }
    )");
    REQUIRE(ir.find("icmp ne ptr") != std::string::npos);
}

// ---- Codegen ----

TEST_CASE("Nullable - a nullable reference lowers to a ptr and null is a null ptr", "[nullable][codegen]") {
    std::string ir = codegenString(R"(
        class C { i32 x; C(i32 v) { x = v; } }
        fn main() -> i32 {
            C&? a = null;
            if (a == null) { return 1; }
            return 0;
        }
    )");
    // The nullable local is a `ptr` alloca, initialised to null, and `== null` is a ptr icmp.
    REQUIRE(ir.find("alloca ptr") != std::string::npos);
    REQUIRE(ir.find("icmp eq ptr") != std::string::npos);
}

TEST_CASE("Nullable - a nullable primitive lowers to a `{ i1, iN }` tagged value", "[nullable][primitive][codegen]") {
    std::string ir = codegenString(R"(
        fn main() -> i32 { i32? a = 5; if (a == null) { return 1; } return a!!; }
    )");
    // The tagged optional appears (alloca of the struct), and `== null` tests the presence tag.
    REQUIRE(ir.find("{ i1, i32 }") != std::string::npos);
    REQUIRE(ir.find("extractvalue") != std::string::npos);
}

TEST_CASE("Nullable - `!!` emits a null check that aborts", "[nullable][codegen]") {
    std::string ir = codegenString(R"(
        class C { mut i32 x; C(i32 v) { x = v; } }
        fn main() -> i32 { C&? a = new C(1); C& b = a!!; return b.x; }
    )");
    REQUIRE(ir.find("icmp eq ptr") != std::string::npos);
    REQUIRE(ir.find("call void @abort()") != std::string::npos);
}

// ============================================================
// EXPANDED COVERAGE (added alongside the Phase-1/2/safe-call baseline above).
// ============================================================

// ---- Type surface: nullable across the reference-like kinds ----

TEST_CASE("Nullable - a nullable class borrow `Class*?` is supported", "[nullable][semantic]") {
    // A `*` borrow made nullable. As a parameter it needs no lvalue source.
    auto r = analyzeString(R"(
        class C { mut i32 x; C(i32 v) { x = v; } }
        fn use(C*? p) -> i32 { if (p == null) { return 0; } return p!!.x; }
    )");
    REQUIRE_FALSE(r.hadError);
}

TEST_CASE("Nullable - a nullable enum `Color?` accepts null and `== null`", "[nullable][semantic]") {
    auto r = analyzeString(R"(
        enum Color { RED, GREEN, BLUE }
        fn main() -> i32 {
            Color? c = null;
            if (c == null) { return 1; }
            return 0;
        }
    )");
    REQUIRE_FALSE(r.hadError);
}

TEST_CASE("Nullable - a nullable enum lowers to a ptr (null = null ptr)", "[nullable][codegen]") {
    std::string ir = codegenString(R"(
        enum Color { RED, GREEN, BLUE }
        fn main() -> i32 { Color? c = null; if (c == null) { return 1; } return 0; }
    )");
    REQUIRE(ir.find("alloca ptr") != std::string::npos);
    REQUIRE(ir.find("icmp eq ptr") != std::string::npos);
}

TEST_CASE("Nullable - several nullable primitive kinds are accepted", "[nullable][primitive][semantic]") {
    // i64/f64/bool/char/u8 all form `{ i1, iN }` optionals; null is valid for each.
    auto r = analyzeString(R"(
        fn main() -> i32 {
            i64? a = 5;
            f64? b = 1.5;
            bool? c = true;
            char? d = null;
            u8? e = null;
            if (a == null) { return 1; }
            if (b == null) { return 2; }
            if (c == null) { return 3; }
            if (d != null) { return 4; }
            if (e != null) { return 5; }
            return 0;
        }
    )");
    REQUIRE_FALSE(r.hadError);
}

// ---- Nullable parameters & returns ----

TEST_CASE("Nullable - a nullable primitive parameter and return type work", "[nullable][primitive][semantic]") {
    auto r = analyzeString(R"(
        fn addOr(i32? x) -> i32 { return x ?: 0; }
        fn maybe(bool b) -> i32? { if (b) { return 5; } return null; }
        fn main() -> i32 { i32? m = maybe(true); return addOr(m); }
    )");
    REQUIRE_FALSE(r.hadError);
}

TEST_CASE("Nullable - a non-null argument implicitly wraps to a nullable parameter", "[nullable][semantic]") {
    auto r = analyzeString(R"(
        class C { i32 x; C(i32 v) { x = v; } }
        fn takesNullable(C&? c) -> i32 { return 0; }
        fn main() -> i32 { C& a = new C(1); return takesNullable(a); }
    )");
    REQUIRE_FALSE(r.hadError);
}

TEST_CASE("Nullable - a non-null value implicitly wraps in a nullable return", "[nullable][semantic]") {
    auto r = analyzeString(R"(
        class C { i32 x; C(i32 v) { x = v; } }
        fn f(C& a) -> C&? { return a; }
        fn main() -> i32 { return 0; }
    )");
    REQUIRE_FALSE(r.hadError);
}

TEST_CASE("Nullable - passing a nullable argument to a non-null parameter is rejected", "[nullable][semantic]") {
    StderrCapture cap;
    auto r = analyzeString(R"(
        class C { i32 x; C(i32 v) { x = v; } }
        fn takesNonNull(C& c) -> i32 { return 0; }
        fn use(C&? a) -> i32 { return takesNonNull(a); }
    )");
    REQUIRE(r.hadError);
}

TEST_CASE("Nullable - returning a nullable where a non-null return is declared is rejected", "[nullable][semantic]") {
    StderrCapture cap;
    auto r = analyzeString(R"(
        class C { i32 x; C(i32 v) { x = v; } }
        fn f(C&? a) -> C& { return a; }
    )");
    REQUIRE(r.hadError);
    REQUIRE(cap.contains("cannot implicitly convert"));
}

TEST_CASE("Nullable - a nullable primitive cannot implicitly narrow on assignment", "[nullable][primitive][semantic]") {
    StderrCapture cap;
    auto r = analyzeString(R"(
        fn main() -> i32 { i32? a = 5; i32 b = a; return b; }
    )");
    REQUIRE(r.hadError);
    REQUIRE(cap.contains("cannot implicitly convert"));
}

// ---- Nullable fields ----

TEST_CASE("Nullable - a class may declare nullable reference and primitive fields", "[nullable][semantic]") {
    auto r = analyzeString(R"(
        class N { mut i32 v; N&? next; i32? tag; N(i32 x) { v = x; } }
        fn main() -> i32 { return 0; }
    )");
    REQUIRE_FALSE(r.hadError);
}

TEST_CASE("Nullable - a nullable field can be compared with null", "[nullable][semantic]") {
    auto r = analyzeString(R"(
        class N { mut i32 v; N&? next; N(i32 x) { v = x; } }
        fn main() -> i32 {
            N& a = new N(1);
            if (a.next == null) { return 1; }
            return 0;
        }
    )");
    REQUIRE_FALSE(r.hadError);
}

TEST_CASE("Nullable - a nullable field is readable after `!!`", "[nullable][semantic]") {
    auto r = analyzeString(R"(
        class N { mut i32 v; N&? next; N(i32 x) { v = x; } }
        fn read(N& a) -> i32 { return a.next!!.v; }
    )");
    REQUIRE_FALSE(r.hadError);
}

TEST_CASE("Nullable - a field chain is NOT smart-cast narrowed", "[nullable][smartcast][semantic]") {
    // `if (a.next != null)` must NOT narrow `a.next` — only bare local/param bindings narrow.
    StderrCapture cap;
    auto r = analyzeString(R"(
        class N { mut i32 v; N&? next; N(i32 x) { v = x; } }
        fn read(N& a) -> i32 { if (a.next != null) { return a.next.v; } return -1; }
    )");
    REQUIRE(r.hadError);
    REQUIRE(cap.contains("possibly-null"));
}

// ---- null literal placement ----

TEST_CASE("Nullable - `null` cannot initialize a non-nullable primitive", "[nullable][primitive][semantic]") {
    StderrCapture cap;
    auto r = analyzeString("fn main() -> i32 { i32 x = null; return x; }");
    REQUIRE(r.hadError);
    REQUIRE(cap.contains("cannot implicitly convert"));
}

// ---- `!!` ----

TEST_CASE("Nullable - `!!` on a non-nullable value warns that it is unnecessary", "[nullable][semantic]") {
    StderrCapture cap;
    auto r = analyzeString(R"(
        fn main() -> i32 { i32 x = 4; return x!!; }
    )");
    REQUIRE_FALSE(r.hadError);
    REQUIRE(cap.contains("the assertion is unnecessary"));
}

TEST_CASE("Nullable - `!!` on a nullable primitive extracts the payload and aborts on absence", "[nullable][primitive][codegen]") {
    std::string ir = codegenString(R"(
        fn main() -> i32 { i32? a = 5; return a!!; }
    )");
    REQUIRE(ir.find("extractvalue { i1, i32 }") != std::string::npos);
    REQUIRE(ir.find("call void @abort()") != std::string::npos);
}

// ---- `?:` Elvis ----

TEST_CASE("Nullable - Elvis with a primitive default yields the non-null primitive", "[nullable][primitive][semantic]") {
    auto r = analyzeString(R"(
        fn main() -> i32 { i32? a = null; i32 b = a ?: 42; return b; }
    )");
    REQUIRE_FALSE(r.hadError);
}

TEST_CASE("Nullable - Elvis is right-associative and chains", "[nullable][semantic]") {
    auto r = analyzeString(R"(
        class C { i32 x; C(i32 v) { x = v; } }
        fn pick(C&? a, C&? b, C& c) -> i32 { C& r = a ?: b ?: c; return r.x; }
    )");
    REQUIRE_FALSE(r.hadError);
}

TEST_CASE("Nullable - Elvis with a nullable right operand keeps the result nullable", "[nullable][semantic]") {
    // `a ?: b` where both sides are nullable → the result is still nullable (needs a further unwrap).
    auto r = analyzeString(R"(
        class C { i32 x; C(i32 v) { x = v; } }
        fn pick(C&? a, C&? b) -> i32 { C&? r = a ?: b; if (r == null) { return 0; } return r!!.x; }
    )");
    REQUIRE_FALSE(r.hadError);
}

TEST_CASE("Nullable - Elvis on a non-nullable left operand warns the default is never used", "[nullable][semantic]") {
    StderrCapture cap;
    auto r = analyzeString(R"(
        class C { i32 x; C(i32 v) { x = v; } }
        fn main() -> i32 { C& a = new C(1); C& b = a ?: new C(2); return b.x; }
    )");
    REQUIRE_FALSE(r.hadError);
    REQUIRE(cap.contains("is not nullable"));
}

// ---- Smart-casts (soundness-critical) ----

TEST_CASE("Nullable - else-branch narrows when the then-branch is the null case", "[nullable][smartcast][semantic]") {
    auto r = analyzeString(R"(
        class C { mut i32 x; C(i32 v) { x = v; } }
        fn use(C&? a) -> i32 { if (a == null) { return 0; } else { return a.x; } }
    )");
    REQUIRE_FALSE(r.hadError);
}

TEST_CASE("Nullable - narrowing on a primitive lets it be used as a plain value", "[nullable][primitive][smartcast][semantic]") {
    auto r = analyzeString(R"(
        fn use(i32? a) -> i32 { if (a == null) { return 0; } return a + 1; }
    )");
    REQUIRE_FALSE(r.hadError);
}

TEST_CASE("Nullable - primitive smart-cast extracts payload with no runtime null check", "[nullable][primitive][smartcast][codegen]") {
    // After `if (a == null) return`, the use `a + 1` extracts payload (field 1) directly — no abort.
    std::string ir = codegenString(R"(
        fn use(i32? a) -> i32 { if (a == null) { return 0; } return a + 1; }
    )");
    REQUIRE(ir.find("extractvalue { i1, i32 }") != std::string::npos);
    // The narrowed read must NOT emit an abort (that would be `!!`'s job, not a smart-cast).
    REQUIRE(ir.find("call void @abort()") == std::string::npos);
}

TEST_CASE("Nullable - narrowing merges across nested if/else paths", "[nullable][smartcast][semantic]") {
    // `a` is non-null after the if because every path either returns or proves it non-null.
    auto r = analyzeString(R"(
        class C { mut i32 x; C(i32 v) { x = v; } }
        fn use(C&? a, bool b) -> i32 {
            if (a == null) {
                if (b) { return 1; } else { return 2; }
            }
            return a.x;
        }
    )");
    REQUIRE_FALSE(r.hadError);
}

TEST_CASE("Nullable - a one-branch narrowing does not leak past the if", "[nullable][smartcast][semantic]") {
    // Narrowing set only in the then-branch must not survive the merge (else path leaves it nullable).
    StderrCapture cap;
    auto r = analyzeString(R"(
        class C { mut i32 x; C(i32 v) { x = v; } }
        fn use(C&? a) -> i32 { if (a != null) { } return a.x; }
    )");
    REQUIRE(r.hadError);
    REQUIRE(cap.contains("possibly-null"));
}

TEST_CASE("Nullable - a loop body reassigning to non-null does not leak narrowing past the loop", "[nullable][smartcast][semantic]") {
    // The body may run zero times, so the in-body non-null reassignment cannot narrow after the loop.
    StderrCapture cap;
    auto r = analyzeString(R"(
        class C { mut i32 x; C(i32 v) { x = v; } }
        fn use(bool cond) -> i32 {
            mut C&? a = null;
            while (cond) { a = new C(1); }
            return a.x;
        }
    )");
    REQUIRE(r.hadError);
    REQUIRE(cap.contains("possibly-null"));
}

TEST_CASE("Nullable - a loop body that nulls a pre-narrowed binding drops the narrowing after", "[nullable][smartcast][semantic]") {
    // `a` is non-null before the loop; the body may null it, so it is possibly-null afterwards.
    StderrCapture cap;
    auto r = analyzeString(R"(
        class C { mut i32 x; C(i32 v) { x = v; } }
        fn use(bool cond) -> i32 {
            mut C&? a = new C(1);
            if (a != null) {
                while (cond) { a = null; }
                return a.x;
            }
            return 0;
        }
    )");
    REQUIRE(r.hadError);
    REQUIRE(cap.contains("possibly-null"));
}

// ---- `?.` safe access / call ----

TEST_CASE("Nullable - `?.` void-returning method call yields a nullable result", "[nullable][safecall][semantic]") {
    auto r = analyzeString(R"(
        class C { mut i32 x; C(i32 v) { x = v; } fn bump() mut { x = x + 1; } }
        fn use(mut C&? a) -> i32 { a?.bump(); return 0; }
    )");
    REQUIRE_FALSE(r.hadError);
}

TEST_CASE("Nullable - `?.` with a `mut` method on a `mut` binding is accepted", "[nullable][safecall][semantic]") {
    auto r = analyzeString(R"(
        class C { mut i32 x; C(i32 v) { x = v; } fn bump() mut -> i32 { x = x + 1; return x; } }
        fn main() -> i32 { mut C&? a = new C(1); i32? r = a?.bump(); return r ?: 0; }
    )");
    REQUIRE_FALSE(r.hadError);
}

TEST_CASE("Nullable - `?.` result must be unwrapped before use as non-null", "[nullable][safecall][semantic]") {
    // `a?.get()` is `i32?`; assigning it to a plain `i32` without `!!`/`?:` is an error.
    StderrCapture cap;
    auto r = analyzeString(R"(
        class C { mut i32 x; C(i32 v) { x = v; } fn get() -> i32 { return x; } }
        fn use(C&? a) -> i32 { i32 g = a?.get(); return g; }
    )");
    REQUIRE(r.hadError);
    REQUIRE(cap.contains("cannot implicitly convert"));
}

TEST_CASE("Nullable - `?.` field access short-circuits through a result slot (codegen)", "[nullable][safecall][codegen]") {
    std::string ir = codegenString(R"(
        class C { mut i32 x; C(i32 v) { x = v; } }
        fn use(C&? a) -> i32 { i32? v = a?.x; return v ?: -1; }
    )");
    REQUIRE(ir.find("icmp ne ptr") != std::string::npos);
}

// ---- Rejections ----

TEST_CASE("Nullable - `void?` is rejected", "[nullable][semantic]") {
    StderrCapture cap;
    auto r = analyzeString("fn main() -> i32 { void? a = null; return 0; }");
    REQUIRE(r.hadError);
    REQUIRE(cap.contains("cannot be nullable"));
}

TEST_CASE("Nullable - a nullable value-object parameter is rejected", "[nullable][semantic]") {
    StderrCapture cap;
    auto r = analyzeString(R"(
        class C { i32 x; C(i32 v) { x = v; } }
        fn f(C? a) -> i32 { return 0; }
    )");
    REQUIRE(r.hadError);
    REQUIRE(cap.contains("value object cannot be nullable"));
}

// ---- Overload interaction ----

TEST_CASE("Nullable - overloading selects `i32` for a non-null arg and `i32?` for null", "[nullable][semantic]") {
    auto r = analyzeString(R"(
        fn f(i32 x) -> i32 { return 1; }
        fn f(i32? x) -> i32 { return 2; }
        fn main() -> i32 { return f(5) + f(null); }
    )");
    REQUIRE_FALSE(r.hadError);
}

// ---- Smart-cast boundaries (v1 limitations) & chaining ----

TEST_CASE("Nullable - narrowing does NOT reach inside `&&` (v1 limitation)", "[nullable][smartcast][semantic]") {
    // `if (x != null && x.get() > 0)` — the RHS of `&&` is not narrowed in v1, so `x.get()` errors.
    StderrCapture cap;
    auto r = analyzeString(R"(
        class C { mut i32 x; C(i32 v) { x = v; } fn get() -> i32 { return x; } }
        fn use(C&? x) -> i32 { if (x != null && x.get() > 0) { return 1; } return 0; }
    )");
    REQUIRE(r.hadError);
    REQUIRE(cap.contains("possibly-null"));
}

TEST_CASE("Nullable - reassigning a narrowed mut binding to a nullable expression drops the narrowing",
          "[nullable][smartcast][semantic]") {
    // Distinct from re-assigning the literal `null`: any assignment of a *nullable* value invalidates.
    StderrCapture cap;
    auto r = analyzeString(R"(
        class C { mut i32 x; C(i32 v) { x = v; } }
        fn pick() -> C&? { return null; }
        fn main() -> i32 {
            mut C&? a = new C(1);
            if (a != null) { a = pick(); return a.x; }
            return 0;
        }
    )");
    REQUIRE(r.hadError);
    REQUIRE(cap.contains("possibly-null"));
}

TEST_CASE("Nullable - `?.` chains and short-circuits through a null link", "[nullable][safecall][semantic]") {
    // `a?.next?.x` — a chained safe access; the whole chain is nullable and stops at the first null.
    auto r = analyzeString(R"(
        class C { mut i32 x; mut C&? next; C(i32 v) { x = v; next = null; } }
        fn use(C&? a) -> i32 { i32? v = a?.next?.x; return v ?: -1; }
    )");
    REQUIRE_FALSE(r.hadError);
}

// ---- Composition with other features ----

TEST_CASE("Nullable - `T?` in a generic function survives monomorphization", "[nullable][generic][semantic]") {
    // `T?` substitutes to `i32?` at instantiation; the `?:` inside works on the concrete type.
    auto r = analyzeString(R"(
        fn firstOr<T>(T? a, T b) -> T { return a ?: b; }
        fn main() -> i32 { return firstOr<i32>(null, 7) + firstOr<i32>(3, 99); }
    )");
    REQUIRE_FALSE(r.hadError);
}

TEST_CASE("Nullable - a generic class may hold a nullable field", "[nullable][generic][semantic]") {
    auto r = analyzeString(R"(
        class Box<T> { mut T? item; Box() { item = null; }
                       fn set(T v) mut { item = v; } fn has() -> bool { return item != null; } }
        fn main() -> i32 { mut Box<i32> b{}; if (b.has()) { return 1; } b.set(5); return 0; }
    )");
    REQUIRE_FALSE(r.hadError);
}

TEST_CASE("Nullable - a nullable enum works as a switch scrutinee with a `null` case", "[nullable][semantic]") {
    auto r = analyzeString(R"(
        enum Color { RED, GREEN }
        fn main() -> i32 { Color? c = null; return switch (c) { case null -> 1; default -> 0; }; }
    )");
    REQUIRE_FALSE(r.hadError);
}
