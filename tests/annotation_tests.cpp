#include <catch2/catch_test_macros.hpp>
#include "helpers.h"

// ============================================================
// Annotations — compile-time metadata (`annotation` + `@Name`)
// ============================================================
// An `annotation Name { <const fields> }` type carries no runtime representation. `@Name(args)`
// prefixes a class / field / method / enum-variant. Read at compile time via `inline for`:
//   f.has(Ann)          -> bool        (does the current member carry Ann)
//   f.get(Ann).field    -> const value (the annotation field, spliced arg tokens)
//   @hasAnnotation(T,A) -> bool        (does the type or any member carry A)
// Everything folds through the reflection expansion pass — no runtime cost, no new IR.

// ------------------------------------------------------------
// Declaration + application parsing
// ------------------------------------------------------------

TEST_CASE("Annotation - marker decl + application parse and produce no IR", "[annotation]") {
    auto ir = codegenString(R"(
        annotation Skip {}
        class C { @Skip i32 x; i32 y; }
        fn main() -> i32 { return 0; }
    )");
    // No struct/ctor for the annotation; the class is unaffected (annotation is metadata only).
    REQUIRE(ir.find("Skip") == std::string::npos);   // 'Skip' leaves no trace in the IR
    REQUIRE(ir.find("%C = type") != std::string::npos);
}

TEST_CASE("Annotation - `annotation` cannot declare methods", "[annotation][parser]") {
    StderrCapture cap;
    parseString("annotation Bad { fn f() -> i32 { return 0; } }");
    REQUIRE(cap.contains("cannot declare methods"));
}

// ------------------------------------------------------------
// f.has (markers)
// ------------------------------------------------------------

TEST_CASE("Annotation - f.has(Ann) folds to a bool per member", "[annotation]") {
    // A direct `if (f.has(Skip))` per field folds to a constant branch: the @Skip field -> br i1 1,
    // the others -> br i1 0. (A `!f.has(...)` would emit an `xor`, not a literal branch.)
    auto ir = codegenString(R"(
        annotation Skip {}
        class User { i32 id; @Skip i32 cache; i32 age; }
        fn skipCount() -> i32 {
            mut i32 n = 0;
            inline for (fld in @fields(User)) { if (fld.has(Skip)) { n = n + 1; } }
            return n;
        }
        fn main() -> i32 { return skipCount(); }
    )");
    REQUIRE(ir.find("br i1 1") != std::string::npos);   // the @Skip field (cache)
    REQUIRE(ir.find("br i1 0") != std::string::npos);   // id, age
}

TEST_CASE("Annotation - @hasAnnotation folds to a type-level bool", "[annotation]") {
    auto yes = codegenString(R"(
        annotation Skip {}
        class C { @Skip i32 x; }
        fn main() -> i32 { if (@hasAnnotation(C, Skip)) { return 1; } return 0; }
    )");
    REQUIRE(yes.find("br i1 1") != std::string::npos);
    auto no = codegenString(R"(
        annotation Skip {}
        class C { i32 x; }
        fn main() -> i32 { if (@hasAnnotation(C, Skip)) { return 1; } return 0; }
    )");
    REQUIRE(no.find("br i1 0") != std::string::npos);
}

// ------------------------------------------------------------
// f.get (data)
// ------------------------------------------------------------

TEST_CASE("Annotation - f.get(Ann).field splices an int const value", "[annotation]") {
    auto ir = codegenString(R"(
        annotation Range { i32 lo; i32 hi; }
        class C { @Range(0, 150) i32 age; }
        fn topOf<T>() -> i32 {
            inline for (f in @fields(T)) { if (f.has(Range)) { return f.get(Range).hi; } }
            return 0 - 1;
        }
        fn main() -> i32 { return topOf<C>(); }
    )");
    REQUIRE(ir.find("ret i32 150") != std::string::npos);   // the @Range(_, 150) high bound
}

TEST_CASE("Annotation - f.get(Ann).field splices a str const value", "[annotation]") {
    auto ir = codegenString(R"(
        annotation Rename { str key; }
        class C { @Rename("user_name") i32 id; }
        fn keyLen<T>() -> u64 {
            inline for (f in @fields(T)) { if (f.has(Rename)) { return f.get(Rename).key.len; } }
            return 0;
        }
        fn main() -> i32 { var n = keyLen<C>(); return 0; }
    )");
    REQUIRE(ir.find("c\"user_name\\00\"") != std::string::npos);   // the spliced str literal
}

// ------------------------------------------------------------
// Errors / rulings
// ------------------------------------------------------------

TEST_CASE("Annotation - unknown annotation name is an error", "[annotation][semantic]") {
    StderrCapture cap;
    auto result = analyzeString(R"(
        class C { @Nope i32 x; }
        fn main() -> i32 { return 0; }
    )");
    REQUIRE(result.hadError);
    REQUIRE(cap.contains("unknown annotation"));
}

TEST_CASE("Annotation - using an annotation as a value type is rejected", "[annotation][parser]") {
    // `Skip` is an annotation, not a type name, so `Skip s;` doesn't parse as a declaration.
    StderrCapture cap;
    parseString(R"(
        annotation Skip {}
        fn usef() -> i32 { Skip s; return 0; }
    )");
    REQUIRE(cap.contains("expected"));   // rejected (a parse error, printed to stderr)
}

TEST_CASE("Annotation - duplicate application on one target is an error", "[annotation][semantic]") {
    StderrCapture cap;
    auto result = analyzeString(R"(
        annotation Skip {}
        class C { @Skip @Skip i32 x; }
        fn main() -> i32 { return 0; }
    )");
    REQUIRE(result.hadError);
    REQUIRE(cap.contains("duplicate annotation"));
}

TEST_CASE("Annotation - a non-constant argument is rejected", "[annotation][semantic]") {
    StderrCapture cap;
    auto result = analyzeString(R"(
        annotation Range { i32 lo; i32 hi; }
        fn side() -> i32 { return 5; }
        class C { @Range(0, side()) i32 x; }
        fn main() -> i32 { return 0; }
    )");
    REQUIRE(result.hadError);
    REQUIRE(cap.contains("compile-time constant"));
}

TEST_CASE("Annotation - wrong argument count is rejected", "[annotation][semantic]") {
    StderrCapture cap;
    auto result = analyzeString(R"(
        annotation Range { i32 lo; i32 hi; }
        class C { @Range(0) i32 x; }
        fn main() -> i32 { return 0; }
    )");
    REQUIRE(result.hadError);
    REQUIRE(cap.contains("expects 2 argument"));
}

TEST_CASE("Annotation - f.get with an unknown field is an error", "[annotation][semantic]") {
    StderrCapture cap;
    analyzeString(R"(
        annotation Rename { str key; }
        class C { @Rename("x") i32 id; }
        fn f<T>() { inline for (g in @fields(T)) { if (g.has(Rename)) { var q = g.get(Rename).nope; } } }
        fn main() -> i32 { f<C>(); return 0; }
    )");
    REQUIRE(cap.contains("no field 'nope'"));
}

// ------------------------------------------------------------
// Composition
// ------------------------------------------------------------

TEST_CASE("Annotation - works in a generic function (per-instantiation unroll)", "[annotation][generic]") {
    auto ir = codegenString(R"(
        annotation Skip {}
        class A { @Skip i32 a; i32 b; }
        class B { i32 x; i32 y; i32 z; }
        fn live<T>() -> i32 {
            mut i32 n = 0;
            inline for (f in @fields(T)) { if (!f.has(Skip)) { n = n + 1; } }
            return n;
        }
        fn main() -> i32 { return live<A>() + live<B>(); }   // 1 + 3
    )");
    REQUIRE(ir.find("live$A") != std::string::npos);
    REQUIRE(ir.find("live$B") != std::string::npos);
}

TEST_CASE("Annotation - applies to an enum variant", "[annotation]") {
    auto ir = codegenString(R"(
        annotation Hidden {}
        enum Color { RED, @Hidden GREEN, BLUE }
        fn countHidden() -> i32 {
            mut i32 n = 0;
            inline for (v in @variants(Color)) { if (v.has(Hidden)) { n = n + 1; } }
            return n;
        }
        fn main() -> i32 { return countHidden(); }   // only GREEN is @Hidden -> 1
    )");
    REQUIRE(ir.find("br i1 1") != std::string::npos);   // GREEN is @Hidden
    REQUIRE(ir.find("br i1 0") != std::string::npos);   // RED, BLUE are not
}

// ------------------------------------------------------------
// Edge cases — field-type coverage, target coverage, more errors
// ------------------------------------------------------------

TEST_CASE("Annotation - f.get(Ann).field splices a char const value", "[annotation]") {
    // A char-valued annotation arg splices as its code point; the guarded compare folds to a
    // constant branch. (The dead branch on the un-annotated field must NOT warn — see the
    // char-placeholder regression below.)
    auto ir = codegenString(R"(
        annotation Mark { char c; }
        class C { @Mark('Z') i32 x; i32 y; }
        fn firstMark<T>() -> i32 {
            inline for (f in @fields(T)) {
                if (f.has(Mark)) { if (f.get(Mark).c == 'Z') { return 1; } }
            }
            return 0;
        }
        fn main() -> i32 { return firstMark<C>(); }
    )");
    REQUIRE(ir.find("90") != std::string::npos);   // 'Z' == code point 90
}

TEST_CASE("Annotation - f.get(Ann).field splices a bool const value", "[annotation]") {
    auto ir = codegenString(R"(
        annotation Flag { bool on; }
        class C { @Flag(true) i32 x; }
        fn firstFlag<T>() -> i32 {
            inline for (f in @fields(T)) {
                if (f.has(Flag)) { if (f.get(Flag).on) { return 1; } }
            }
            return 0;
        }
        fn main() -> i32 { return firstFlag<C>(); }
    )");
    REQUIRE(ir.find("br i1 1") != std::string::npos);   // the spliced `true` lowers to i1 1
}

TEST_CASE("Annotation - an unguarded f.get on an absent member does not warn (char placeholder)", "[annotation]") {
    // `inline for` folds `f.get` unconditionally (comptime-if pruning is deferred), so the
    // un-annotated field `y` still expands `f.get(Mark).c`. Its default placeholder must be a
    // char-typed value, not a numeric 0 that would warn "does not fit in 'char'".
    StderrCapture cap;
    codegenString(R"(
        annotation Mark { char c; }
        class C { @Mark('A') i32 x; i32 y; }
        fn walk<T>() -> i32 {
            inline for (f in @fields(T)) {
                if (f.has(Mark)) { if (f.get(Mark).c == 'A') { return 1; } }
            }
            return 0;
        }
        fn main() -> i32 { return walk<C>(); }
    )");
    REQUIRE_FALSE(cap.contains("does not fit"));
}

TEST_CASE("Annotation - @hasAnnotation sees a method-carried annotation", "[annotation]") {
    // Neither the type nor any field carries @Api — only a method does. @hasAnnotation's
    // "type or any member" clause must still report true.
    auto ir = codegenString(R"(
        annotation Api {}
        class C { i32 x; @Api fn ping() -> i32 { return 1; } }
        fn main() -> i32 { if (@hasAnnotation(C, Api)) { return 1; } return 0; }
    )");
    REQUIRE(ir.find("br i1 1") != std::string::npos);
}

TEST_CASE("Annotation - applies to the class declaration itself", "[annotation]") {
    // A class-level `@Marker class C` (no annotated members) is seen by @hasAnnotation.
    auto ir = codegenString(R"(
        annotation Entity {}
        @Entity class C { i32 x; }
        fn main() -> i32 { if (@hasAnnotation(C, Entity)) { return 1; } return 0; }
    )");
    REQUIRE(ir.find("br i1 1") != std::string::npos);
}

TEST_CASE("Annotation - a field of a non-comptime type is rejected", "[annotation][semantic]") {
    StderrCapture cap;
    auto result = analyzeString(R"(
        class Point { i32 x; i32 y; }
        annotation Bad { Point p; }
        fn main() -> i32 { return 0; }
    )");
    REQUIRE(result.hadError);
    REQUIRE(cap.contains("compile-time type"));
}

TEST_CASE("Annotation - name colliding with a class is rejected", "[annotation][semantic]") {
    StderrCapture cap;
    auto result = analyzeString(R"(
        class Foo { i32 x; }
        annotation Foo {}
        fn main() -> i32 { return 0; }
    )");
    REQUIRE(result.hadError);
    REQUIRE(cap.contains("already declared"));
}

TEST_CASE("Annotation - @hasAnnotation with an undeclared annotation is an error", "[annotation][semantic]") {
    StderrCapture cap;
    auto result = analyzeString(R"(
        class C { i32 x; }
        fn main() -> i32 { if (@hasAnnotation(C, Nope)) { return 1; } return 0; }
    )");
    REQUIRE(result.hadError);
    REQUIRE(cap.contains("unknown annotation"));
}
