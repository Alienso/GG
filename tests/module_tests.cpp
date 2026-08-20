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

// ------------------------------------------------------------
// Dotted (multi-segment) module names and import paths
// ------------------------------------------------------------
// `module std.utility;` / `import std.utility.Pair;` generalize the single-segment grammar to a
// dot-separated chain: the module-name read and the import-path split both accept `IDENT
// ('.' IDENT)*`, and an import's LAST segment is always the symbol (unambiguous — GG top-level
// decls are always flat, never nested).

TEST_CASE("Module - a dotted module name qualifies its class type in IR", "[module]") {
    auto ir = codegenString(R"(
        module std.utility;
        class Pair { mut i32 a; Pair(i32 v) { this.a = v; } fn get() -> i32 { return this.a; } }
        fn main() -> i32 { Pair p(5); return p.get(); }
    )");
    REQUIRE(ir.find("%std.utility.Pair = type") != std::string::npos);
    REQUIRE(ir.find("@std.utility.Pair_get(")   != std::string::npos);
}

TEST_CASE("Module - a dotted import path binds only the last segment (not a prefix segment)", "[module]") {
    // Regression guard: the import-scanner used to pattern-match exactly one dot (`IDENT.IDENT`),
    // so `import a.b.Widget;` wrongly registered a binding for the simple name `b` -> "a.b" (a
    // dangling, meaningless target) while `Widget` (the actually-wanted symbol) was never imported
    // at all — surfacing later as an unrelated "expected expression"/"not a known type" error.
    writeIn("gg_mod_dotted_import", "lib.gg",
            "module a.b;\nclass Widget { mut i32 x; Widget(i32 v) { this.x = v; } }");
    std::string root = writeIn("gg_mod_dotted_import", "main.gg", R"(
        import "lib.gg";
        import a.b.Widget;
        fn main() -> i32 { Widget w(9); return w.x - 9; }
    )");

    ImportResolver resolver;
    Program program = resolver.resolve(root);
    SemanticAnalyzer analyzer;
    SemanticResult sem = analyzer.analyze(program, root, defaultTestOptions());
    REQUIRE_FALSE(sem.hadError);

    CodeGen cg;
    IRModule ir = cg.generate(program, sem, defaultTestOptions());
    std::ostringstream out; IRPrinter{}.print(ir, out);
    REQUIRE(out.str().find("%a.b.Widget = type") != std::string::npos);
}

TEST_CASE("Module - an inline fully-qualified dotted reference folds without any import line", "[module]") {
    writeIn("gg_mod_dotted_fqn", "lib.gg",
            "module a.b;\nclass Widget { mut i32 x; Widget(i32 v) { this.x = v; } }");
    std::string root = writeIn("gg_mod_dotted_fqn", "main.gg", R"(
        import "lib.gg";
        fn main() -> i32 { a.b.Widget w(4); return w.x - 4; }
    )");

    ImportResolver resolver;
    Program program = resolver.resolve(root);
    SemanticAnalyzer analyzer;
    SemanticResult sem = analyzer.analyze(program, root, defaultTestOptions());
    REQUIRE_FALSE(sem.hadError);

    CodeGen cg;
    IRModule ir = cg.generate(program, sem, defaultTestOptions());
    std::ostringstream out; IRPrinter{}.print(ir, out);
    REQUIRE(out.str().find("%a.b.Widget = type") != std::string::npos);
}

TEST_CASE("Module - longest-prefix FQN fold picks a longer registered module over a shorter overlapping one",
          "[module]") {
    writeIn("gg_mod_longest_prefix", "a.gg", "module a;\nfn x() -> i32 { return 1; }");
    writeIn("gg_mod_longest_prefix", "ab.gg", "module a.b;\nfn y() -> i32 { return 42; }");
    std::string root = writeIn("gg_mod_longest_prefix", "main.gg", R"(
        import "a.gg";
        import "ab.gg";
        fn main() -> i32 { return a.x() + a.b.y(); }
    )");

    ImportResolver resolver;
    Program program = resolver.resolve(root);
    SemanticAnalyzer analyzer;
    SemanticResult sem = analyzer.analyze(program, root, defaultTestOptions());
    REQUIRE_FALSE(sem.hadError);

    CodeGen cg;
    IRModule ir = cg.generate(program, sem, defaultTestOptions());
    std::ostringstream out; IRPrinter{}.print(ir, out);
    std::string s = out.str();
    REQUIRE(s.find("@a.x(")   != std::string::npos);
    REQUIRE(s.find("@a.b.y(") != std::string::npos);   // proves "a.b.y" folds as module a.b + name y,
                                                        // not module a + a dangling ".b.y" chain
}

TEST_CASE("Module - a declaration's own name is never hijacked by an import of the same bare name",
          "[module]") {
    // Regression guard for a pre-existing bug in the qualifier (not introduced by dotted-module
    // support): module `a` declares `fn f()`; module `b` ALSO declares its own `fn f()` AND imports
    // `a.f` under the same bare name `f`. Before the fix, `qualifyTokens` resolved a declaration's
    // own name the same way as any call site (import binding first) — so `b`'s own `fn f()` would
    // get silently renamed/collided into `a.f` instead of declaring `b.f`. A declaration always
    // belongs to its own module; only ordinary call sites (like `callImported`'s bare `f()`) should
    // resolve through the import binding.
    writeIn("gg_mod_decl_collision", "a.gg", "module a;\nfn f() -> i32 { return 1; }");
    std::string root = writeIn("gg_mod_decl_collision", "main.gg", R"(
        module b;
        import "a.gg";
        import a.f;
        fn f() -> i32 { return 2; }
        fn callImported() -> i32 { return f(); }
        fn main() -> i32 {
            if (b.f() != 2) { return 1; }
            if (b.callImported() != 1) { return 2; }
            return 0;
        }
    )");

    ImportResolver resolver;
    Program program = resolver.resolve(root);
    SemanticAnalyzer analyzer;
    SemanticResult sem = analyzer.analyze(program, root, defaultTestOptions());
    REQUIRE_FALSE(sem.hadError);

    CodeGen cg;
    IRModule ir = cg.generate(program, sem, defaultTestOptions());
    std::ostringstream out; IRPrinter{}.print(ir, out);
    std::string s = out.str();
    REQUIRE(s.find("@a.f(") != std::string::npos);
    REQUIRE(s.find("@b.f(") != std::string::npos);   // b's own decl kept its own identity
    // b's internal bare f() call resolves through the import to a.f, not to b's own f.
    auto callImportedPos = s.find("@b.callImported(");
    REQUIRE(callImportedPos != std::string::npos);
    auto bodyEnd = s.find("\n}", callImportedPos);
    REQUIRE(bodyEnd != std::string::npos);
    std::string body = s.substr(callImportedPos, bodyEnd - callImportedPos);
    REQUIRE(body.find("@a.f(") != std::string::npos);
    REQUIRE(body.find("@b.f(") == std::string::npos);
}

// ------------------------------------------------------------
// Wildcard imports — `import a.b.*;`
// ------------------------------------------------------------
// Java-style: binds every top-level member (types + free functions) declared DIRECTLY in that
// module, exactly as if each had its own `import a.b.Name;` line. Non-recursive — a submodule
// `a.b.c` is a distinct registry key and is never pulled in by a wildcard on `a.b`.

TEST_CASE("Module - a wildcard import binds every member of the module", "[module]") {
    writeIn("gg_mod_wildcard", "lib.gg", R"(
        module a.b;
        fn twice(i32 n) -> i32 { return n * 2; }
        class Widget { mut i32 x; Widget(i32 v) { this.x = v; } fn get() -> i32 { return this.x; } }
    )");
    std::string root = writeIn("gg_mod_wildcard", "main.gg", R"(
        import "lib.gg";
        import a.b.*;
        fn main() -> i32 {
            Widget w(4);
            return twice(w.get()) - 8;
        }
    )");

    ImportResolver resolver;
    Program program = resolver.resolve(root);
    SemanticAnalyzer analyzer;
    SemanticResult sem = analyzer.analyze(program, root, defaultTestOptions());
    REQUIRE_FALSE(sem.hadError);

    CodeGen cg;
    IRModule ir = cg.generate(program, sem, defaultTestOptions());
    std::ostringstream out; IRPrinter{}.print(ir, out);
    std::string s = out.str();
    REQUIRE(s.find("@a.b.twice(")      != std::string::npos);
    REQUIRE(s.find("%a.b.Widget = type") != std::string::npos);
}

TEST_CASE("Module - a wildcard import does not pull in a submodule's members", "[module]") {
    // `a.b.*` must bind only a.b's OWN members, never anything declared in a.b.c — a submodule is
    // a distinct registry key, not a nested namespace.
    writeIn("gg_mod_wildcard_nonrecursive", "sub.gg", "module a.b.c;\nfn deep() -> i32 { return 99; }");
    writeIn("gg_mod_wildcard_nonrecursive", "lib.gg", "module a.b;\nfn shallow() -> i32 { return 1; }");
    std::string root = writeIn("gg_mod_wildcard_nonrecursive", "main.gg", R"(
        import "sub.gg";
        import "lib.gg";
        import a.b.*;
        fn main() -> i32 { return shallow() + deep(); }   // `deep` is not bound by `a.b.*`
    )");

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
    REQUIRE(errored);   // `deep` is an unbound bare name — a.b.c is not pulled in by a.b.*
}

TEST_CASE("Module - a wildcard import colliding with another import's name is ambiguous", "[module][semantic]") {
    writeIn("gg_mod_wildcard_ambig", "a.gg", "module a;\nfn f() -> i32 { return 1; }");
    writeIn("gg_mod_wildcard_ambig", "b.gg", "module b;\nfn f() -> i32 { return 2; }");
    std::string root = writeIn("gg_mod_wildcard_ambig", "main.gg", R"(
        import "a.gg";
        import "b.gg";
        import a.*;
        import b.f;
        fn main() -> i32 { return f(); }   // ambiguous: a.* also bound `f`
    )");

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
