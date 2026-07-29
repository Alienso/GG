#include <catch2/catch_test_macros.hpp>
#include "helpers.h"

// ============================================================
// Compile-time reflection: @-sigil builtins + `inline for`
// ============================================================
// `inline for (f in @fields(T)) { … }` unrolls at compile time (in the parser, after
// monomorphization) — one copy of the body per field, with `f.name` -> the field-name string and
// `@field(v, f.name)` -> ordinary member access `v.<field>`. Scalar queries (@typeName / @fieldCount
// / @hasField) fold to constants like `sizeof`; @compileError aborts compilation. Everything is
// expanded/folded before semantic analysis and codegen, at zero runtime cost.

// ------------------------------------------------------------
// inline for + @field (the field-iteration core)
// ------------------------------------------------------------

TEST_CASE("Reflect - inline for over @fields unrolls to per-field member access", "[reflect]") {
    auto ir = codegenString(R"(
        class Point { i32 x; i32 y; Point(i32 a, i32 b) { this.x = a; this.y = b; } }
        fn eqAll(Point& a, Point& b) -> bool {
            inline for (f in @fields(Point)) {
                if (@field(a, f.name) != @field(b, f.name)) { return false; }
            }
            return true;
        }
        fn main() -> i32 { return 0; }
    )");
    // Two fields -> per-field GEP + i32 compare, and no reflection node survives.
    REQUIRE(ir.find("getelementptr %Point") != std::string::npos);
    REQUIRE(ir.find("i32 0, i32 1") != std::string::npos);   // second field GEP
    REQUIRE(ir.find("icmp ne i32")  != std::string::npos);   // per-field compare
}

TEST_CASE("Reflect - @field is an lvalue (assignment target)", "[reflect]") {
    auto ir = codegenString(R"(
        class P { mut i32 x; mut i32 y; }
        fn zeroAll(mut P& v) {
            inline for (f in @fields(P)) { @field(v, f.name) = 0; }
        }
        fn main() -> i32 { return 0; }
    )");
    REQUIRE(ir.find("store i32 0") != std::string::npos);   // each field zeroed via member assign
}

TEST_CASE("Reflect - inline for inside a generic function monomorphizes + expands", "[reflect][generic]") {
    auto ir = codegenString(R"(
        class Point { i32 x; i32 y; Point(i32 a, i32 b) { this.x = a; this.y = b; } }
        fn eqAll<T>(T& a, T& b) -> bool {
            inline for (f in @fields(T)) {
                if (@field(a, f.name) != @field(b, f.name)) { return false; }
            }
            return true;
        }
        fn main() -> i32 {
            Point p = Point(1, 2);
            Point q = Point(1, 2);
            if (eqAll<Point>(p, q)) { return 0; }
            return 1;
        }
    )");
    REQUIRE(ir.find("eqAll$Point") != std::string::npos);       // the instantiation
    REQUIRE(ir.find("icmp ne i32") != std::string::npos);       // expanded per-field i32 compare
}

TEST_CASE("Reflect - nested inline for expands to a fixpoint", "[reflect]") {
    auto ir = codegenString(R"(
        class A { mut i32 a; mut i32 b; }
        fn touch(mut A& v) {
            inline for (f in @fields(A)) {
                inline for (g in @fields(A)) { @field(v, f.name) = @field(v, g.name); }
            }
        }
        fn main() -> i32 { return 0; }
    )");
    // 2x2 unroll -> at least four field GEPs, no reflection node left.
    REQUIRE(ir.find("i32 0, i32 0") != std::string::npos);
    REQUIRE(ir.find("i32 0, i32 1") != std::string::npos);
}

// ------------------------------------------------------------
// Scalar queries (fold to constants, like sizeof)
// ------------------------------------------------------------

TEST_CASE("Reflect - @typeName folds to a string constant of the type name", "[reflect]") {
    auto ir = codegenString(R"(
        class Point { i32 x; i32 y; }
        fn main() -> i32 { var s = @typeName(Point); return 0; }
    )");
    REQUIRE(ir.find("c\"Point\\00\"") != std::string::npos);
}

TEST_CASE("Reflect - @fieldCount folds to the field count", "[reflect]") {
    auto ir = codegenString(R"(
        class Point { i32 x; i32 y; i32 z; }
        fn main() -> i32 { var n = @fieldCount(Point); return 0; }
    )");
    REQUIRE(ir.find("store i64 3") != std::string::npos);   // 3 fields, u64 constant
}

TEST_CASE("Reflect - @hasField folds to a bool", "[reflect]") {
    auto yes = codegenString(R"(
        class Point { i32 x; i32 y; }
        fn main() -> i32 { if (@hasField(Point, "x")) { return 1; } return 0; }
    )");
    // present -> the condition is a constant true (i1 1); absent -> 0.
    REQUIRE(yes.find("br i1 1") != std::string::npos);
    auto no = codegenString(R"(
        class Point { i32 x; i32 y; }
        fn main() -> i32 { if (@hasField(Point, "nope")) { return 1; } return 0; }
    )");
    REQUIRE(no.find("br i1 0") != std::string::npos);
}

// ------------------------------------------------------------
// Errors
// ------------------------------------------------------------

TEST_CASE("Reflect - @compileError aborts with its message", "[reflect]") {
    StderrCapture cap;
    auto result = analyzeString(R"(
        fn main() -> i32 { @compileError("boom"); return 0; }
    )");
    REQUIRE(result.hadError);
    REQUIRE(cap.contains("boom"));
}

TEST_CASE("Reflect - @field outside inline for is an error", "[reflect]") {
    StderrCapture cap;
    analyzeString(R"(
        class P { i32 x; }
        fn main() -> i32 { P v; return @field(v, "x"); }
    )");
    REQUIRE(cap.contains("@field"));
}

TEST_CASE("Reflect - break directly inside inline for is an error", "[reflect]") {
    StderrCapture cap;
    analyzeString(R"(
        class P { i32 x; i32 y; }
        fn f(P& v) { inline for (g in @fields(P)) { break; } }
        fn main() -> i32 { return 0; }
    )");
    REQUIRE(cap.contains("not allowed directly inside"));
}

TEST_CASE("Reflect - bare binding use is an error (only f.name / @field)", "[reflect]") {
    StderrCapture cap;
    analyzeString(R"(
        class P { i32 x; }
        fn f(P& v) { inline for (g in @fields(P)) { var q = g; } }
        fn main() -> i32 { return 0; }
    )");
    REQUIRE(cap.contains("reflection binding"));
}

TEST_CASE("Reflect - @fields over a non-class type is an error", "[reflect]") {
    StderrCapture cap;
    analyzeString(R"(
        fn f() { inline for (g in @fields(i32)) { } }
        fn main() -> i32 { return 0; }
    )");
    REQUIRE(cap.contains("not a class type"));
}

TEST_CASE("Reflect - @fields is not valid as a standalone expression", "[reflect]") {
    StderrCapture cap;
    analyzeString(R"(
        class P { i32 x; }
        fn main() -> i32 { var s = @fields(P); return 0; }
    )");
    REQUIRE(cap.contains("@fields"));
}

// ------------------------------------------------------------
// Edge cases: heterogeneous fields, statics, methods, empty, non-class @typeName
// ------------------------------------------------------------

TEST_CASE("Reflect - heterogeneous field types resolve per field (icmp + fcmp)", "[reflect]") {
    // The decisive property: each unrolled `@field(...) != @field(...)` resolves to ITS field's
    // type independently — an i32 field -> icmp, an f64 field -> fcmp.
    auto ir = codegenString(R"(
        class M { i32 a; f64 b; M(i32 x, f64 y) { this.a = x; this.b = y; } }
        fn eq(M& p, M& q) -> bool {
            inline for (f in @fields(M)) {
                if (@field(p, f.name) != @field(q, f.name)) { return false; }
            }
            return true;
        }
        fn main() -> i32 { return 0; }
    )");
    REQUIRE(ir.find("icmp ne i32")  != std::string::npos);   // the i32 field
    REQUIRE(ir.find("fcmp")         != std::string::npos);   // the f64 field
}

TEST_CASE("Reflect - static fields are excluded from @fields / @fieldCount", "[reflect]") {
    auto ir = codegenString(R"(
        class C { i32 x; i32 y; static i32 shared; }
        fn main() -> i32 { var n = @fieldCount(C); return 0; }
    )");
    REQUIRE(ir.find("store i64 2") != std::string::npos);   // 2 instance fields, static excluded
}

TEST_CASE("Reflect - inline for works inside a method using this", "[reflect]") {
    auto ir = codegenString(R"(
        class C {
            i32 x; i32 y;
            fn allZero() -> bool {
                inline for (f in @fields(C)) { if (@field(this, f.name) != 0) { return false; } }
                return true;
            }
        }
        fn main() -> i32 { return 0; }
    )");
    REQUIRE(ir.find("getelementptr %C") != std::string::npos);   // per-field GEP on self
    REQUIRE(ir.find("icmp ne i32")      != std::string::npos);
}

TEST_CASE("Reflect - inline for over an empty class expands to nothing", "[reflect]") {
    auto ir = codegenString(R"(
        class E { E() {} }
        fn f() -> i32 { mut i32 c = 0; inline for (g in @fields(E)) { c = c + 1; } return c; }
        fn main() -> i32 { return 0; }
    )");
    REQUIRE(ir.find("getelementptr %E") == std::string::npos);   // no field access emitted
}

TEST_CASE("Reflect - @typeName yields a `str` view (has .len / .data)", "[reflect][str]") {
    // Phase 2: @typeName returns `str` (was a bare ptr), so `.len` and `.data` work on it.
    auto ir = codegenString(R"(
        class Point { i32 x; i32 y; }
        fn main() -> i32 { str s = @typeName(Point); u64 n = s.len; return 0; }
    )");
    REQUIRE(ir.find("c\"Point\\00\"")            != std::string::npos);
    REQUIRE(ir.find("insertvalue { ptr, i64 }")  != std::string::npos);   // the str view
    REQUIRE(ir.find("extractvalue { ptr, i64 }") != std::string::npos);   // s.len read
}

TEST_CASE("Reflect - @typeName works on primitives and enums", "[reflect]") {
    auto ir = codegenString(R"(
        enum Color { RED, GREEN }
        fn main() -> i32 { var a = @typeName(i32); var b = @typeName(Color); return 0; }
    )");
    REQUIRE(ir.find("c\"i32\\00\"")   != std::string::npos);
    REQUIRE(ir.find("c\"Color\\00\"") != std::string::npos);
}

// ============================================================
// Phase 2 — enum @variants, layout, predicates, @implements
// ============================================================

// ------------------------------------------------------------
// inline for over @variants (the enum-iteration core)
// ------------------------------------------------------------

TEST_CASE("Reflect - inline for over @variants unrolls to per-variant statements", "[reflect]") {
    auto ir = codegenString(R"(
        enum Color { RED, GREEN, BLUE }
        fn count(Color c) -> i32 {
            mut i32 n = 0;
            inline for (v in @variants(Color)) { if (c == v) { n = n + 1; } }
            return n;
        }
        fn main() -> i32 { return 0; }
    )");
    // Each variant bare `v` lowered to the singleton global @Color$VARIANT; identity `icmp` per arm.
    REQUIRE(ir.find("@Color$RED")   != std::string::npos);
    REQUIRE(ir.find("@Color$GREEN") != std::string::npos);
    REQUIRE(ir.find("@Color$BLUE")  != std::string::npos);
    REQUIRE(ir.find("icmp eq ptr")  != std::string::npos);
}

TEST_CASE("Reflect - v.name inside @variants inline for is the variant name string", "[reflect]") {
    auto ir = codegenString(R"(
        enum Color { RED, GREEN }
        fn f() { inline for (v in @variants(Color)) { var s = v.name; } }
        fn main() -> i32 { return 0; }
    )");
    REQUIRE(ir.find("c\"RED\\00\"")   != std::string::npos);
    REQUIRE(ir.find("c\"GREEN\\00\"") != std::string::npos);
}

TEST_CASE("Reflect - @variants over a non-enum type is an error", "[reflect]") {
    StderrCapture cap;
    analyzeString(R"(
        class P { i32 x; }
        fn f() { inline for (v in @variants(P)) { } }
        fn main() -> i32 { return 0; }
    )");
    REQUIRE(cap.contains("not an enum type"));
}

TEST_CASE("Reflect - @variants is not valid as a standalone expression", "[reflect]") {
    StderrCapture cap;
    analyzeString(R"(
        enum Color { RED }
        fn main() -> i32 { var s = @variants(Color); return 0; }
    )");
    REQUIRE(cap.contains("@variants"));
}

// ------------------------------------------------------------
// @variantCount / @alignOf / @offsetOf (fold to constants)
// ------------------------------------------------------------

TEST_CASE("Reflect - @variantCount folds to the variant count", "[reflect]") {
    auto ir = codegenString(R"(
        enum Color { RED, GREEN, BLUE }
        fn main() -> i32 { var n = @variantCount(Color); return 0; }
    )");
    REQUIRE(ir.find("store i64 3") != std::string::npos);
}

TEST_CASE("Reflect - @variantCount over a non-enum is an error", "[reflect]") {
    StderrCapture cap;
    analyzeString(R"(
        class P { i32 x; }
        fn main() -> i32 { var n = @variantCount(P); return 0; }
    )");
    REQUIRE(cap.contains("not an enum type"));
}

TEST_CASE("Reflect - @alignOf folds to the natural alignment", "[reflect]") {
    auto ir = codegenString(R"(
        class Big { i8 a; f64 b; }   // aligned to 8 (widest member)
        fn main() -> i32 {
            var a = @alignOf(i32);   // 4
            var b = @alignOf(f64);   // 8
            var c = @alignOf(Big);   // 8
            return 0;
        }
    )");
    REQUIRE(ir.find("store i64 4") != std::string::npos);
    REQUIRE(ir.find("store i64 8") != std::string::npos);
}

TEST_CASE("Reflect - @offsetOf folds to a field's byte offset", "[reflect]") {
    auto ir = codegenString(R"(
        class R { i32 a; i32 b; f64 c; }
        fn main() -> i32 {
            var oa = @offsetOf(R, "a");   // 0
            var ob = @offsetOf(R, "b");   // 4
            var oc = @offsetOf(R, "c");   // 8 (aligned)
            return 0;
        }
    )");
    REQUIRE(ir.find("store i64 0") != std::string::npos);
    REQUIRE(ir.find("store i64 4") != std::string::npos);
    REQUIRE(ir.find("store i64 8") != std::string::npos);
}

TEST_CASE("Reflect - @offsetOf on an unknown field is an error", "[reflect]") {
    StderrCapture cap;
    analyzeString(R"(
        class R { i32 a; }
        fn main() -> i32 { var o = @offsetOf(R, "nope"); return 0; }
    )");
    REQUIRE(cap.contains("no instance field"));
}

// ------------------------------------------------------------
// Type predicates (@isInteger / @isFloat / @isClass / @isEnum / @isPrimitive)
// ------------------------------------------------------------

TEST_CASE("Reflect - type predicates fold to bool constants", "[reflect]") {
    auto ir = codegenString(R"(
        class C { i32 x; }
        enum E { A, B }
        fn main() -> i32 {
            if (@isInteger(i32))   { return 0; }
            if (@isFloat(f64))     { return 0; }
            if (@isClass(C))       { return 0; }
            if (@isEnum(E))        { return 0; }
            if (@isPrimitive(bool)){ return 0; }
            return 1;
        }
    )");
    REQUIRE(ir.find("br i1 1") != std::string::npos);
}

TEST_CASE("Reflect - type predicates answer false for the wrong category", "[reflect]") {
    auto ir = codegenString(R"(
        class C { i32 x; }
        enum E { A }
        fn main() -> i32 {
            if (@isInteger(f64)) { return 1; }   // false
            if (@isFloat(i32))   { return 1; }   // false
            if (@isClass(E))     { return 1; }   // enum is not a class -> false
            if (@isEnum(C))      { return 1; }   // class is not an enum -> false
            if (@isPrimitive(C)) { return 1; }   // class is not primitive -> false
            return 0;
        }
    )");
    REQUIRE(ir.find("br i1 0") != std::string::npos);
    REQUIRE(ir.find("br i1 1") == std::string::npos);   // no predicate came out true
}

// ------------------------------------------------------------
// @implements (needs the trait table surfaced to codegen)
// ------------------------------------------------------------

TEST_CASE("Reflect - @implements is true for an implemented trait", "[reflect]") {
    auto ir = codegenString(R"(
        trait Greet { fn hi() -> i32; }
        class C { i32 x; }
        impl Greet for C { fn hi() -> i32 { return 1; } }
        fn main() -> i32 { if (@implements(C, Greet)) { return 0; } return 1; }
    )");
    REQUIRE(ir.find("br i1 1") != std::string::npos);
}

TEST_CASE("Reflect - @implements is false for an unimplemented trait", "[reflect]") {
    auto ir = codegenString(R"(
        trait Greet { fn hi() -> i32; }
        class C { i32 x; }
        fn main() -> i32 { if (@implements(C, Greet)) { return 1; } return 0; }
    )");
    REQUIRE(ir.find("br i1 0") != std::string::npos);
}

TEST_CASE("Reflect - @implements recognises a built-in operator trait", "[reflect]") {
    // An `Eq` impl makes @implements(T, Eq) true (built-in traits populate the table too).
    auto ir = codegenString(R"(
        class V { i32 x; }
        impl Eq for V { fn eq(V& o) -> bool { return this.x == o.x; } }
        fn main() -> i32 { if (@implements(V, Eq)) { return 0; } return 1; }
    )");
    REQUIRE(ir.find("br i1 1") != std::string::npos);
}

TEST_CASE("Reflect - a variant binding is a real value (method call / field access)", "[reflect]") {
    // Inside @variants, `v` is the singleton `Enum::VARIANT`, so `v.method()` / `v.field` work
    // (they lower to `Enum::VARIANT.method()`), not just `v.name`.
    auto ir = codegenString(R"(
        enum Planet {
            EARTH(1), MARS(2);
            i32 order;
            Planet(i32 o) { this.order = o; }
            fn getOrder() -> i32 { return this.order; }
        }
        fn total() -> i32 {
            mut i32 s = 0;
            inline for (v in @variants(Planet)) { s = s + v.getOrder(); }
            return s;
        }
        fn main() -> i32 { return 0; }
    )");
    REQUIRE(ir.find("@Planet$EARTH") != std::string::npos);
    REQUIRE(ir.find("@Planet$MARS")  != std::string::npos);
    REQUIRE(ir.find("@Planet_getOrder") != std::string::npos);   // the method is actually called
}

TEST_CASE("Reflect - @offsetOf skips past an embedded value-object field", "[reflect]") {
    // Line { Point start; Point end; } — end's offset is sizeof(Point) = 8, not 4.
    auto ir = codegenString(R"(
        class Point { i32 x; i32 y; }
        class Line { Point start; Point end; }
        fn main() -> i32 {
            var a = @offsetOf(Line, "start");   // 0
            var b = @offsetOf(Line, "end");     // 8 (past a whole Point)
            return 0;
        }
    )");
    REQUIRE(ir.find("store i64 0") != std::string::npos);
    REQUIRE(ir.find("store i64 8") != std::string::npos);
}

TEST_CASE("Reflect - @alignOf on a reference / borrow is pointer-sized", "[reflect]") {
    auto ir = codegenString(R"(
        class C { i32 x; }
        fn main() -> i32 {
            var a = @alignOf(C&);   // owning reference -> 8
            var b = @alignOf(C*);   // borrow          -> 8
            return 0;
        }
    )");
    REQUIRE(ir.find("store i64 8") != std::string::npos);
}

TEST_CASE("Reflect - @fieldCount on an enum is a (class-only) error", "[reflect]") {
    // @fields/@fieldCount are class-only; @variants/@variantCount are enum-only. Cross-use errors.
    StderrCapture cap;
    analyzeString(R"(
        enum E { A, B }
        fn main() -> i32 { var n = @fieldCount(E); return 0; }
    )");
    REQUIRE(cap.contains("not a class type"));
}

TEST_CASE("Reflect - @variants composes with a generic function", "[reflect][generic]") {
    auto ir = codegenString(R"(
        enum Color { RED, GREEN, BLUE }
        fn nameCount<T>() -> i32 {
            mut i32 n = 0;
            inline for (v in @variants(T)) { n = n + 1; }
            return n;
        }
        fn main() -> i32 { return nameCount<Color>() - 3; }
    )");
    // Three variants -> three `n = n + 1` increments after monomorphization + expansion.
    REQUIRE(ir.find("nameCount$Color") != std::string::npos);
}
