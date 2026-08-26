#include <catch2/catch_test_macros.hpp>
#include "helpers.h"
#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

// ============================================================
// Trait-name parameter sugar — `fn f(Trait a) -> ...` / `fn f(Trait& a) -> ...`.
//
// A bare (or `&`/`*`-sigiled) trait name used as a whole parameter TYPE is desugared, purely at the
// token level (Parser::desugarTraitParams), into a bounded generic before the ordinary
// tryCaptureFunctionTemplate capture ever runs: `fn f(Eq& a) -> ...` becomes
// `fn f<__T0: Eq>(__T0& a) -> ...`. So every check below is really exercising the EXISTING bounded-
// generic pipeline (instantiation, bound obligations, monomorphized body checking) — this file's job
// is to confirm the desugar itself fires correctly, preserves an explicit `&`/`*` sigil, keeps
// repeated trait params INDEPENDENT (Rust `impl Trait` semantics), and stays scoped to free
// functions with no explicit `<...>` of their own.
// ============================================================

static std::string writeTempFile(const std::string& filename, const std::string& source) {
    std::string path = (fs::temp_directory_path() / filename).string();
    std::ofstream(path) << source;
    return path;
}

// ------------------------------------------------------------
// Parser — the desugar produces a real generic instantiation + bound obligation
// ------------------------------------------------------------

TEST_CASE("TraitParamSugar - bare user-trait param records a bound obligation", "[traitparam][parser]") {
    auto prog = parseString(R"(
        trait Shape { fn area() -> i32; }
        class Square { mut i32 side; Square(i32 s) { side = s; } }
        impl Shape for Square { fn area() -> i32 { return side * side; } }
        fn totalArea(Shape a) -> i32 { return a.area(); }
        fn main() -> i32 { Square& s = new Square(4); return totalArea(s); }
    )");
    bool found = false;
    for (const auto& bc : prog.genericBoundChecks)
        if (bc.typeName == "Square" && bc.traitName == "Shape") found = true;
    REQUIRE(found);
}

TEST_CASE("TraitParamSugar - built-in operator trait (Eq) is a valid bare-param bound", "[traitparam][parser]") {
    auto prog = parseString(R"(
        class Vec2 { mut i32 x; Vec2(i32 v) { x = v; } }
        impl Eq for Vec2 { fn eq(Vec2& r) -> bool { return x == r.x; } }
        fn same(Eq& a, Eq& b) -> bool { return a == b; }
        fn main() -> i32 { Vec2& a = new Vec2(1); Vec2& b = new Vec2(1); if (same(a, b)) { return 0; } return 1; }
    )");
    bool found = false;
    for (const auto& bc : prog.genericBoundChecks)
        if (bc.typeName == "Vec2" && bc.traitName == "Eq") found = true;
    REQUIRE(found);
}

// ------------------------------------------------------------
// Semantic — accepted
// ------------------------------------------------------------

TEST_CASE("TraitParamSugar - satisfied bound, called with inference (no explicit <...>)", "[traitparam][semantic]") {
    auto result = analyzeString(R"(
        trait Shape { fn area() -> i32; }
        class Square { mut i32 side; Square(i32 s) { side = s; } }
        impl Shape for Square { fn area() -> i32 { return side * side; } }
        fn totalArea(Shape a) -> i32 { return a.area(); }
        fn main() -> i32 { Square& s = new Square(4); return totalArea(s); }
    )");
    REQUIRE_FALSE(result.hadError);
}

TEST_CASE("TraitParamSugar - `&` sigil lets an operator-trait method taking Self& be called", "[traitparam][semantic]") {
    auto result = analyzeString(R"(
        class Vec2 { mut i32 x; Vec2(i32 v) { x = v; } }
        impl Eq for Vec2 { fn eq(Vec2& r) -> bool { return x == r.x; } }
        fn same(Eq& a, Eq& b) -> bool { return a == b; }
        fn main() -> i32 { Vec2& a = new Vec2(1); Vec2& b = new Vec2(1); if (same(a, b)) { return 0; } return 1; }
    )");
    REQUIRE_FALSE(result.hadError);
}

TEST_CASE("TraitParamSugar - bare (borrow) param works for a Self-argument-free trait method", "[traitparam][semantic]") {
    auto result = analyzeString(R"(
        trait Sized { fn size() -> i32; }
        class Box { mut i32 v; Box(i32 x) { v = x; } }
        impl Sized for Box { fn size() -> i32 { return v; } }
        fn reportSize(Sized a) -> i32 { return a.size(); }
        fn main() -> i32 { Box& b = new Box(7); return reportSize(b); }
    )");
    REQUIRE_FALSE(result.hadError);
}

TEST_CASE("TraitParamSugar - explicit <...> call form resolves to the same instantiation", "[traitparam][semantic]") {
    auto result = analyzeString(R"(
        class Vec2 { mut i32 x; Vec2(i32 v) { x = v; } }
        impl Eq for Vec2 { fn eq(Vec2& r) -> bool { return x == r.x; } }
        fn same(Eq& a, Eq& b) -> bool { return a == b; }
        fn main() -> i32 {
            Vec2& a = new Vec2(1);
            Vec2& b = new Vec2(1);
            if (same<Vec2, Vec2>(a, b)) { return 0; }
            return 1;
        }
    )");
    REQUIRE_FALSE(result.hadError);
}

TEST_CASE("TraitParamSugar - two bare-trait params bind independently to different concrete types",
          "[traitparam][semantic]") {
    // Rust `impl Trait`-in-argument-position semantics: each occurrence is its own type parameter,
    // so `Money` and `Vec2` (unrelated classes, both implementing Eq) may fill the two slots at once.
    auto result = analyzeString(R"(
        class Vec2  { mut i32 x; Vec2(i32 v)  { x = v; } }
        class Money { mut i32 c; Money(i32 v) { c = v; } }
        impl Eq for Vec2  { fn eq(Vec2& r)  -> bool { return x == r.x; } }
        impl Eq for Money { fn eq(Money& r) -> bool { return c == r.c; } }
        fn bothEqualSelf(Eq& a, Eq& b) -> bool { return (a == a) && (b == b); }
        fn main() -> i32 {
            Vec2&  v = new Vec2(1);
            Money& m = new Money(2);
            if (bothEqualSelf(v, m)) { return 0; }
            return 1;
        }
    )");
    REQUIRE_FALSE(result.hadError);
}

TEST_CASE("TraitParamSugar - `mut Trait&` gives a mutable owning-reference param", "[traitparam][semantic]") {
    // The `mut` precedes the trait name; the desugar must still recognise the trait (preceded-by-`mut`
    // case) and preserve both `mut` and the `&` sigil around the synthesized type param.
    auto result = analyzeString(R"(
        trait Sized { fn size() -> i32; }
        class Box { mut i32 v; Box(i32 x) { v = x; } }
        impl Sized for Box { fn size() -> i32 { return v; } }
        fn touch(mut Sized& a) -> i32 { return a.size(); }
        fn main() -> i32 { mut Box& b = new Box(7); return touch(b); }
    )");
    REQUIRE_FALSE(result.hadError);
}

TEST_CASE("TraitParamSugar - `Trait*` explicit-borrow sigil is preserved", "[traitparam][semantic]") {
    auto result = analyzeString(R"(
        trait Sized { fn size() -> i32; }
        class Box { mut i32 v; Box(i32 x) { v = x; } }
        impl Sized for Box { fn size() -> i32 { return v; } }
        fn borrowSize(Sized* a) -> i32 { return a.size(); }
        fn main() -> i32 { Box& b = new Box(7); return borrowSize(b); }
    )");
    REQUIRE_FALSE(result.hadError);
}

TEST_CASE("TraitParamSugar - a `private` free function may use trait-param sugar", "[traitparam][semantic]") {
    // The desugar must skip the optional `private` between `fn` and the name when locating the
    // parameter list (nameIdx advances past PRIVATE).
    auto result = analyzeString(R"(
        class Vec2 { mut i32 x; Vec2(i32 v) { x = v; } }
        impl Eq for Vec2 { fn eq(Vec2& r) -> bool { return x == r.x; } }
        fn private same(Eq& a, Eq& b) -> bool { return a == b; }
        fn main() -> i32 { Vec2& a = new Vec2(1); Vec2& b = new Vec2(1); if (same(a, b)) { return 0; } return 1; }
    )");
    REQUIRE_FALSE(result.hadError);
}

TEST_CASE("TraitParamSugar - a parameter NAMED like a trait is not rewritten", "[traitparam][semantic]") {
    // False-positive guard: `i32 Eq` is an ordinary param named `Eq` (preceded by the `i32` type
    // keyword, not `(`/`,`/`mut`), so the function stays a plain non-generic `f(i32)` — NOT a
    // bounded generic. If it were wrongly rewritten, `Eq` would vanish as a usable value.
    auto result = analyzeString(R"(
        fn namedLikeTrait(i32 Eq) -> i32 { return Eq + 1; }
        fn main() -> i32 { if (namedLikeTrait(41) != 42) { return 1; } return 0; }
    )");
    REQUIRE_FALSE(result.hadError);
}

TEST_CASE("TraitParamSugar - trait declared in a separate imported file is recognised", "[traitparam][semantic]") {
    writeTempFile("gg_traitsugar_shape.gg", R"(
        trait Shape { fn area() -> i32; }
        class Square { mut i32 side; Square(i32 s) { side = s; } }
        impl Shape for Square { fn area() -> i32 { return side * side; } }
    )");
    std::string mainPath = writeTempFile("gg_traitsugar_main.gg", R"(
        import "gg_traitsugar_shape.gg";
        fn totalArea(Shape a) -> i32 { return a.area(); }
        fn main() -> i32 { Square& s = new Square(4); return totalArea(s); }
    )");
    ImportResolver resolver;
    Program ast = resolver.resolve(mainPath);
    SemanticAnalyzer analyzer;
    SemanticResult result = analyzer.analyze(ast, mainPath, defaultTestOptions());
    REQUIRE_FALSE(result.hadError);
}

// ------------------------------------------------------------
// Semantic — rejected
// ------------------------------------------------------------

TEST_CASE("TraitParamSugar - unsatisfied bound is a clean error", "[traitparam][semantic]") {
    StderrCapture cap;
    auto result = analyzeString(R"(
        class Plain { mut i32 x; Plain(i32 v) { x = v; } }
        fn same(Eq& a, Eq& b) -> bool { return a == b; }
        fn main() -> i32 {
            Plain& a = new Plain(1);
            Plain& b = new Plain(2);
            if (same(a, b)) { return 1; }
            return 0;
        }
    )");
    REQUIRE(result.hadError);
    REQUIRE(cap.contains("does not satisfy bound"));
}

TEST_CASE("TraitParamSugar - a class method with a bare-trait param is out of v1 scope (clean error)",
          "[traitparam][semantic]") {
    // v1: free functions only. A method (inside a class body) sees the ordinary pre-existing
    // "not a known type" error — the desugar never touches it (braceDepth > 0). A non-generic
    // method's params are checked immediately at PARSE time, not semantic time — parseString's
    // CompileError catch swallows this into an empty (trivially error-free) Program, so this must
    // be asserted via stderr rather than SemanticResult::hadError (see analyzeString/parseString).
    StderrCapture cap;
    parseString(R"(
        trait Shape { fn area() -> i32; }
        class Holder {
            fn combine(Shape a) -> i32 { return a.area(); }
        }
        fn main() -> i32 { return 0; }
    )");
    REQUIRE(cap.contains("not a known type"));
}

TEST_CASE("TraitParamSugar - mixing sugar with an explicit <...> on the same function is not applied",
          "[traitparam][semantic]") {
    // v1: a declaration with its OWN explicit `<...>` is left untouched by the desugar, so a bare
    // trait name used as a param type there hits the ordinary "not a known type" error — but only once
    // the template is actually instantiated (an uninstantiated generic's body is never parsed at all).
    // This is a monomorphization-time PARSE error (re-parsing the substituted instantiation), so it
    // must be asserted via stderr — see the note on the test above.
    StderrCapture cap;
    parseString(R"(
        trait Shape { fn area() -> i32; }
        fn f<T>(Shape a, T b) -> i32 { return a.area(); }
        fn main() -> i32 { return f<i32>(1, 2); }
    )");
    REQUIRE(cap.contains("not a known type"));
}

// ------------------------------------------------------------
// Codegen — sanity (compiles down to IR with no crash, mangled per-instantiation)
// ------------------------------------------------------------

TEST_CASE("TraitParamSugar - codegen emits a mangled per-instantiation function", "[traitparam][codegen]") {
    std::string ir = codegenString(R"(
        trait Shape { fn area() -> i32; }
        class Square { mut i32 side; Square(i32 s) { side = s; } }
        impl Shape for Square { fn area() -> i32 { return side * side; } }
        fn totalArea(Shape a) -> i32 { return a.area(); }
        fn main() -> i32 { Square& s = new Square(4); return totalArea(s); }
    )");
    REQUIRE(ir.find("totalArea") != std::string::npos);
    REQUIRE(ir.find("Square") != std::string::npos);
}
