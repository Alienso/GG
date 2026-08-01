#include <catch2/catch_test_macros.hpp>
#include "helpers.h"

#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

// ============================================================
// Module namespacing — `module NAME;` + Java-style `import a.B;`
// ============================================================
// A file's `module` prefixes its top-level names (class/enum/trait/annotation/free-fn), so two
// modules may each declare `Foo`. A bare reference resolves via `import a.B;` bindings then the
// current module's own members; a fully-qualified `a.Foo` always works. Qualification happens as a
// token rewrite in the parser (like generic mangling), so the qualified name flows through as an
// ordinary className string — codegen emits `%geo.Point`, `@geo.Point_method`, etc.

// Write `source` into a fresh subdirectory of the temp dir; returns the file's absolute path.
static std::string writeIn(const std::string& subdir, const std::string& filename,
                           const std::string& source) {
    fs::path dir = fs::temp_directory_path() / subdir;
    fs::create_directories(dir);
    std::string path = (dir / filename).string();
    std::ofstream(path) << source;
    return path;
}

// ------------------------------------------------------------
// Single-file qualification (via codegenString/analyzeString → ImportResolver)
// ------------------------------------------------------------

TEST_CASE("Module - a module qualifies its class type and methods in IR", "[module]") {
    auto ir = codegenString(R"(
        module geo;
        class Point { mut i32 x; Point(i32 a) { this.x = a; } fn get() -> i32 { return this.x; } }
        fn main() -> i32 { Point p(5); return p.get(); }
    )");
    REQUIRE(ir.find("%geo.Point = type") != std::string::npos);
    REQUIRE(ir.find("@geo.Point_get(")   != std::string::npos);
    REQUIRE(ir.find("@geo.Point_geo.Point(") != std::string::npos);   // constructor is qualified too
}

TEST_CASE("Module - `main` and its body resolve; main stays unqualified", "[module]") {
    auto ir = codegenString(R"(
        module app;
        fn helper() -> i32 { return 42; }
        fn main() -> i32 { return helper(); }
    )");
    REQUIRE(ir.find("define i32 @main(") != std::string::npos);   // entry symbol NOT qualified
    REQUIRE(ir.find("@app.helper(")      != std::string::npos);   // free fn IS qualified
}

TEST_CASE("Module - a fully-qualified reference resolves within the same file", "[module]") {
    // `geo.Point` (FQN) folds to the same qualified name as the bare `Point`.
    auto ir = codegenString(R"(
        module geo;
        class Point { mut i32 x; Point(i32 a) { this.x = a; } fn get() -> i32 { return this.x; } }
        fn main() -> i32 { geo.Point p(9); return p.get(); }
    )");
    REQUIRE(ir.find("%geo.Point = type") != std::string::npos);
    REQUIRE(ir.find("@geo.Point_get(")   != std::string::npos);
}

TEST_CASE("Module - a generic class in a module composes with `$` mangling", "[module]") {
    auto ir = codegenString(R"(
        module coll;
        class Box<T> { mut T v; Box(T x) { this.v = x; } fn get() -> T { return this.v; } }
        fn main() -> i32 { Box<i32> b(7); return b.get(); }
    )");
    REQUIRE(ir.find("%coll.Box$i32 = type") != std::string::npos);   // module prefix + generic mangle
}

TEST_CASE("Module - duplicate type in one module is a clear error", "[module][semantic]") {
    StderrCapture cap;
    auto result = analyzeString(R"(
        module geo;
        class Foo { mut i32 a; }
        class Foo { mut i32 b; }
        fn main() -> i32 { return 0; }
    )");
    REQUIRE(result.hadError);
    REQUIRE(cap.contains("already defined"));
}

TEST_CASE("Module - a module-less program is unaffected (bare names, no prefix)", "[module]") {
    auto ir = codegenString(R"(
        class Point { mut i32 x; Point(i32 a) { this.x = a; } fn get() -> i32 { return this.x; } }
        fn main() -> i32 { Point p(5); return p.get(); }
    )");
    REQUIRE(ir.find("%Point = type")  != std::string::npos);   // unqualified, exactly as before
    REQUIRE(ir.find("%.Point")        == std::string::npos);   // no stray/empty-module prefix
}

// ------------------------------------------------------------
// Name collisions with a like-named module member (context-aware qualification)
// ------------------------------------------------------------
// A TYPE name is qualified in any position; a FUNCTION name only in call/generic position; a
// method / member decl name is never qualified. So a field / method / local named like a module
// free function is NOT mistaken for that function.

TEST_CASE("Module - a class field named like a module free function is fine", "[module]") {
    auto ir = codegenString(R"(
        module m;
        fn size() -> i32 { return 99; }
        class C { mut i32 size; C(i32 s) { this.size = s; } fn get() -> i32 { return this.size; } }
        fn main() -> i32 { C c(7); return c.get(); }
    )");
    REQUIRE(ir.find("@m.C_get(")  != std::string::npos);   // compiled — field `size` not mis-qualified
    REQUIRE(ir.find("@m.size(")   != std::string::npos);   // the free function is still namespaced
}

TEST_CASE("Module - a method named like a module free function is fine", "[module]") {
    auto ir = codegenString(R"(
        module m;
        fn area() -> i32 { return 9; }
        class C { mut i32 v; C(i32 a) { this.v = a; } fn area() -> i32 { return this.v; } }
        fn main() -> i32 { C c(7); return c.area(); }
    )");
    REQUIRE(ir.find("@m.C_area(") != std::string::npos);   // method decl name not qualified away
    REQUIRE(ir.find("@m.area(")   != std::string::npos);   // free function still namespaced
}

TEST_CASE("Module - a local named like a module free function shadows cleanly", "[module]") {
    auto ir = codegenString(R"(
        module m;
        fn count() -> i32 { return 3; }
        fn main() -> i32 { i32 count = 5; if (count > 2) { return count; } return 0; }
    )");
    REQUIRE(ir.find("@main(")    != std::string::npos);   // main is never qualified
    REQUIRE(ir.find("@m.count(") != std::string::npos);   // the free function is namespaced
}

// ------------------------------------------------------------
// Multi-file: two modules with the same class name coexist
// ------------------------------------------------------------

TEST_CASE("Module - two modules each defining `Foo` coexist", "[module]") {
    writeIn("gg_mod_coexist", "a.gg", "module a;\nclass Foo { mut i32 x; Foo(i32 v) { this.x = v; } fn get() -> i32 { return this.x; } }");
    writeIn("gg_mod_coexist", "b.gg", "module b;\nclass Foo { mut i32 y; Foo(i32 v) { this.y = v; } fn get() -> i32 { return this.y * 10; } }");
    std::string root = writeIn("gg_mod_coexist", "main.gg", R"(
        import "a.gg";
        import "b.gg";
        import a.Foo;
        fn main() -> i32 {
            Foo p(3);          // bare → a.Foo
            b.Foo q(4);        // FQN → b.Foo
            return p.get() + q.get();
        }
    )");

    ImportResolver resolver;
    Program program = resolver.resolve(root);
    SemanticAnalyzer analyzer;
    SemanticResult sem = analyzer.analyze(program);
    REQUIRE_FALSE(sem.hadError);

    CodeGen cg;
    IRModule ir = cg.generate(program, sem, defaultTestOptions());
    std::ostringstream out; IRPrinter{}.print(ir, out);
    std::string s = out.str();
    REQUIRE(s.find("%a.Foo = type") != std::string::npos);
    REQUIRE(s.find("%b.Foo = type") != std::string::npos);   // both, distinct
}

TEST_CASE("Module - an ambiguous bare name (imported from two modules) errors", "[module][semantic]") {
    writeIn("gg_mod_ambig", "a.gg", "module a;\nclass Foo { mut i32 x; Foo(i32 v) { this.x = v; } }");
    writeIn("gg_mod_ambig", "b.gg", "module b;\nclass Foo { mut i32 y; Foo(i32 v) { this.y = v; } }");
    std::string root = writeIn("gg_mod_ambig", "main.gg", R"(
        import "a.gg";
        import "b.gg";
        import a.Foo;
        import b.Foo;
        fn main() -> i32 { Foo p(3); return 0; }   // bare `Foo` is ambiguous → must use a.Foo / b.Foo
    )");

    // An ambiguous bare name is left unqualified, so it is not a known type → a hard compile error
    // (v1: the message is generic — the fix is to write `a.Foo` / `b.Foo`). It surfaces either as a
    // thrown parse error or a semantic error; both are acceptable — the point is it never compiles.
    StderrCapture cap;
    bool errored = false;
    try {
        ImportResolver resolver;
        Program program = resolver.resolve(root);
        SemanticAnalyzer analyzer;
        SemanticResult sem = analyzer.analyze(program, root, defaultTestOptions());
        errored = sem.hadError;
    } catch (const CompileError&) {
        errored = true;
    }
    REQUIRE(errored);
}

TEST_CASE("Module - a cross-module free function is callable by its qualified name", "[module]") {
    writeIn("gg_mod_call", "lib.gg", "module lib;\nfn twice(i32 n) -> i32 { return n * 2; }");
    std::string root = writeIn("gg_mod_call", "main.gg", R"(
        import "lib.gg";
        fn main() -> i32 { return lib.twice(21); }
    )");

    ImportResolver resolver;
    Program program = resolver.resolve(root);
    SemanticAnalyzer analyzer;
    SemanticResult sem = analyzer.analyze(program, root, defaultTestOptions());
    REQUIRE_FALSE(sem.hadError);

    CodeGen cg;
    IRModule ir = cg.generate(program, sem, defaultTestOptions());
    std::ostringstream out; IRPrinter{}.print(ir, out);
    REQUIRE(out.str().find("@lib.twice(") != std::string::npos);
}
