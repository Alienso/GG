#include <catch2/catch_test_macros.hpp>
#include "helpers.h"

#include <algorithm>   // std::replace (for the cross-file private-function tests)

// ============================================================
// Valid programs — should produce no errors
// ============================================================

TEST_CASE("Semantic - valid function produces no error", "[semantic]") {
    auto result = analyzeString(R"(
        fn add(i32 a, i32 b) -> i32 {
            i32 sum = a + b;
            return sum;
        }
    )");
    REQUIRE_FALSE(result.hadError);
}

TEST_CASE("Semantic - variable shadowing in inner scope is allowed", "[semantic]") {
    // 'x' is declared in the outer block and again in an inner block.
    // The inner declaration should shadow without error.
    StderrCapture cap;
    auto result = analyzeString(R"(
        fn main() -> i32 {
            i32 x = 1;
            {
                i32 x = 2;
            }
            return x;
        }
    )");
    REQUIRE_FALSE(result.hadError);
}

TEST_CASE("Semantic - mutual recursion is allowed", "[semantic]") {
    // Both functions are hoisted in pass 1, so they can call each other.
    auto result = analyzeString(R"(
        fn isEven(i32 n) -> i32 { return isOdd(n); }
        fn isOdd(i32 n) -> i32  { return isEven(n); }
    )");
    REQUIRE_FALSE(result.hadError);
}

TEST_CASE("Semantic - silent widening cast produces no error or warning", "[semantic]") {
    // i32 → i64 is a silent signed widening — no error, no warning.
    // Using i32 as the source because integer literals already default to i32.
    StderrCapture cap;
    auto result = analyzeString(R"(
        fn main() -> i32 {
            i32 small = 5;
            i64 big   = small;
            return 0;
        }
    )");
    REQUIRE_FALSE(result.hadError);
    REQUIRE_FALSE(cap.contains("Warning"));
    REQUIRE_FALSE(cap.contains("Error"));
}

// ============================================================
// Scoping errors
// ============================================================

TEST_CASE("Semantic - use of undeclared identifier is an error", "[semantic]") {
    StderrCapture cap;
    auto result = analyzeString(R"(
        fn main() -> i32 {
            return x;
        }
    )");
    REQUIRE(result.hadError);
    REQUIRE(cap.contains("undeclared"));
}

TEST_CASE("Semantic - redeclaration in the same scope is an error", "[semantic]") {
    StderrCapture cap;
    auto result = analyzeString(R"(
        fn main() -> i32 {
            i32 x = 1;
            i32 x = 2;
            return 0;
        }
    )");
    REQUIRE(result.hadError);
    REQUIRE(cap.contains("already declared"));
}

// ============================================================
// Type errors
// ============================================================

TEST_CASE("Semantic - assigning string to i32 is an error", "[semantic]") {
    StderrCapture cap;
    auto result = analyzeString(R"(
        fn main() -> i32 {
            i32 x = "hello";
            return 0;
        }
    )");
    REQUIRE(result.hadError);
    REQUIRE(cap.contains("cannot implicitly convert"));
}

TEST_CASE("Semantic - return type mismatch is an error", "[semantic]") {
    StderrCapture cap;
    auto result = analyzeString(R"(
        fn main() -> i32 {
            return "oops";
        }
    )");
    REQUIRE(result.hadError);
    REQUIRE(cap.contains("cannot implicitly convert"));
}

TEST_CASE("Semantic - calling function with wrong argument count is an error", "[semantic]") {
    StderrCapture cap;
    auto result = analyzeString(R"(
        fn add(i32 a, i32 b) -> i32 { return 0; }
        fn main() -> i32 {
            add(1);
            return 0;
        }
    )");
    REQUIRE(result.hadError);
    REQUIRE(cap.contains("expects"));
}

// ============================================================
// Warning casts (allowed but flagged)
// ============================================================

TEST_CASE("Semantic - f64 VALUE assigned to f32 produces a warning but no error", "[semantic]") {
    StderrCapture cap;
    auto result = analyzeString(R"(
        fn main() -> i32 {
            f64 d = 1.5;
            f32 x = d;   // a genuine f64 value narrowed to f32 → warn
            return 0;
        }
    )");
    // A bare literal (`f32 x = 1.0;`) now *adopts* f32 (no warning — see the [numlit] tests);
    // this narrowing warning fires only for a real f64 value flowing into an f32.
    REQUIRE_FALSE(result.hadError);
    REQUIRE(cap.contains("Warning"));
    REQUIRE(cap.contains("f64"));
    REQUIRE(cap.contains("f32"));
}

TEST_CASE("Semantic - unsigned to signed of same size produces a warning but no error", "[semantic]") {
    // Declare u32 via a cast from i64 (silent widening path to avoid the
    // i32-literal → u32 warn from muddying the u32 → i32 warn we're testing).
    StderrCapture cap;
    auto result = analyzeString(R"(
        fn main() -> i32 {
            i64 tmp = 5;
            u32 a   = tmp;
            i32 b   = a;
            return 0;
        }
    )");
    // i64 → u32 and u32 → i32 are both Warn, not Error
    REQUIRE_FALSE(result.hadError);
    REQUIRE(cap.contains("Warning"));
}

// ============================================================
// void type misuse
// ============================================================

TEST_CASE("Semantic - void variable declaration is an error", "[semantic]") {
    StderrCapture cap;
    auto result = analyzeString(R"(
        fn foo() { void x; }
    )");
    REQUIRE(result.hadError);
    REQUIRE(cap.contains("void"));
}

TEST_CASE("Semantic - void variable with initializer is an error", "[semantic]") {
    StderrCapture cap;
    auto result = analyzeString(R"(
        fn foo() { void x = 0; }
    )");
    REQUIRE(result.hadError);
    REQUIRE(cap.contains("void"));
}

TEST_CASE("Semantic - void parameter type is an error", "[semantic]") {
    StderrCapture cap;
    auto result = analyzeString(R"(
        fn foo(void x) -> i32 { return 0; }
    )");
    REQUIRE(result.hadError);
    REQUIRE(cap.contains("void"));
}

TEST_CASE("Semantic - void function itself is valid", "[semantic]") {
    // 'void' as a *return* type is fine — the error only applies to variables/params.
    auto result = analyzeString(R"(
        fn doNothing() { }
    )");
    REQUIRE_FALSE(result.hadError);
}

// ============================================================
// Missing return
// ============================================================

TEST_CASE("Semantic - non-void function with no return produces a warning", "[semantic]") {
    StderrCapture cap;
    auto result = analyzeString(R"(
        fn foo() -> i32 { }
    )");
    REQUIRE_FALSE(result.hadError);   // warning, not error
    REQUIRE(cap.contains("Warning"));
    REQUIRE(cap.contains("does not always return"));
}

TEST_CASE("Semantic - non-void function with unconditional return is clean", "[semantic]") {
    StderrCapture cap;
    auto result = analyzeString(R"(
        fn foo() -> i32 { return 42; }
    )");
    REQUIRE_FALSE(result.hadError);
    REQUIRE_FALSE(cap.contains("Warning"));
}

TEST_CASE("Semantic - if/else with both branches returning is clean", "[semantic]") {
    StderrCapture cap;
    auto result = analyzeString(R"(
        fn sign(i32 x) -> i32 {
            if (x > 0) { return 1; } else { return 0; }
        }
    )");
    REQUIRE_FALSE(result.hadError);
    REQUIRE_FALSE(cap.contains("Warning"));
}

TEST_CASE("Semantic - if without else branch warns about missing return", "[semantic]") {
    StderrCapture cap;
    auto result = analyzeString(R"(
        fn foo(i32 x) -> i32 {
            if (x > 0) { return 1; }
        }
    )");
    REQUIRE_FALSE(result.hadError);
    REQUIRE(cap.contains("Warning"));
    REQUIRE(cap.contains("does not always return"));
}

TEST_CASE("Semantic - return after if without else is clean", "[semantic]") {
    // The unconditional return at the bottom satisfies all paths.
    StderrCapture cap;
    auto result = analyzeString(R"(
        fn clamp(i32 x) -> i32 {
            if (x > 100) { return 100; }
            return x;
        }
    )");
    REQUIRE_FALSE(result.hadError);
    REQUIRE_FALSE(cap.contains("Warning"));
}

TEST_CASE("Semantic - void function never triggers missing-return warning", "[semantic]") {
    StderrCapture cap;
    auto result = analyzeString(R"(
        fn doNothing() { }
    )");
    REQUIRE_FALSE(result.hadError);
    REQUIRE_FALSE(cap.contains("does not always return"));
}

// ============================================================
// return value in wrong context
// ============================================================

TEST_CASE("Semantic - return with no value in non-void function is an error", "[semantic]") {
    StderrCapture cap;
    auto result = analyzeString(R"(
        fn foo() -> i32 { return; }
    )");
    REQUIRE(result.hadError);
    REQUIRE(cap.contains("no value"));
}

TEST_CASE("Semantic - return with value in void function is an error", "[semantic]") {
    StderrCapture cap;
    auto result = analyzeString(R"(
        fn foo() { return 42; }
    )");
    REQUIRE(result.hadError);
}

// ============================================================
// break / continue — valid uses
// ============================================================

TEST_CASE("Semantic - break inside while loop is valid", "[semantic]") {
    auto result = analyzeString(R"(
        fn foo() { while (1) { break; } }
    )");
    REQUIRE_FALSE(result.hadError);
}

TEST_CASE("Semantic - continue inside while loop is valid", "[semantic]") {
    auto result = analyzeString(R"(
        fn foo() { while (1) { continue; } }
    )");
    REQUIRE_FALSE(result.hadError);
}

TEST_CASE("Semantic - break inside for loop is valid", "[semantic]") {
    auto result = analyzeString(R"(
        fn foo() { for (;;) { break; } }
    )");
    REQUIRE_FALSE(result.hadError);
}

TEST_CASE("Semantic - continue inside for loop is valid", "[semantic]") {
    auto result = analyzeString(R"(
        fn foo() { for (;;) { continue; } }
    )");
    REQUIRE_FALSE(result.hadError);
}

TEST_CASE("Semantic - break inside if inside loop is valid", "[semantic]") {
    // The if is still lexically inside the loop.
    auto result = analyzeString(R"(
        fn foo() {
            while (1) {
                if (1) { break; }
            }
        }
    )");
    REQUIRE_FALSE(result.hadError);
}

TEST_CASE("Semantic - break inside inner loop does not affect outer loop", "[semantic]") {
    auto result = analyzeString(R"(
        fn foo() {
            while (1) {
                while (1) { break; }
            }
        }
    )");
    REQUIRE_FALSE(result.hadError);
}

// ============================================================
// break / continue — invalid uses
// ============================================================

TEST_CASE("Semantic - break outside any loop is an error", "[semantic]") {
    StderrCapture cap;
    auto result = analyzeString(R"(
        fn foo() { break; }
    )");
    REQUIRE(result.hadError);
    REQUIRE(cap.contains("outside of a loop"));
}

TEST_CASE("Semantic - continue outside any loop is an error", "[semantic]") {
    StderrCapture cap;
    auto result = analyzeString(R"(
        fn foo() { continue; }
    )");
    REQUIRE(result.hadError);
    REQUIRE(cap.contains("outside of a loop"));
}

TEST_CASE("Semantic - break inside function nested in loop is an error", "[semantic]") {
    // The inner function creates a new context; loop depth resets to 0 for it.
    StderrCapture cap;
    auto result = analyzeString(R"(
        fn outer() {
            while (1) {
                fn inner() { break; }
            }
        }
    )");
    REQUIRE(result.hadError);
    REQUIRE(cap.contains("outside of a loop"));
}

TEST_CASE("Semantic - break at top level is an error", "[semantic]") {
    StderrCapture cap;
    // Top-level break (no enclosing function or loop)
    auto result = analyzeString("fn foo() -> i32 { break; return 0; }");
    REQUIRE(result.hadError);
}

// ============================================================
// Extern function declarations
// ============================================================

TEST_CASE("Semantic - extern declaration is valid", "[semantic]") {
    auto result = analyzeString("extern printf(i8 fmt) -> i32;");
    REQUIRE_FALSE(result.hadError);
}

TEST_CASE("Semantic - extern function can be called", "[semantic]") {
    auto result = analyzeString(R"(
        extern add(i32 a, i32 b) -> i32;
        fn main() -> i32 { return add(1, 2); }
    )");
    REQUIRE_FALSE(result.hadError);
}

TEST_CASE("Semantic - duplicate extern declaration is an error", "[semantic]") {
    StderrCapture cap;
    auto result = analyzeString(R"(
        extern exit(i32 code);
        extern exit(i32 code);
    )");
    REQUIRE(result.hadError);
}

TEST_CASE("Semantic - extern and function with same name is an error", "[semantic]") {
    StderrCapture cap;
    auto result = analyzeString(R"(
        extern foo();
        fn foo() { }
    )");
    REQUIRE(result.hadError);
}

TEST_CASE("Semantic - extern call with wrong arg count is an error", "[semantic]") {
    StderrCapture cap;
    auto result = analyzeString(R"(
        extern exit(i32 code);
        fn main() { exit(1, 2); }
    )");
    REQUIRE(result.hadError);
}

TEST_CASE("Semantic - extern call with wrong arg type is an error", "[semantic]") {
    // Passing a string where i32 is expected — no implicit conversion exists.
    StderrCapture cap;
    auto result = analyzeString(R"(
        extern exit(i32 code);
        fn main() { exit("hello"); }
    )");
    REQUIRE(result.hadError);
}

// ============================================================
// ptr type
// ============================================================

TEST_CASE("Semantic - ptr variable is valid", "[semantic]") {
    auto result = analyzeString(R"(
        extern malloc(u64 size) -> ptr;
        fn main() { ptr p = malloc(64); }
    )");
    REQUIRE_FALSE(result.hadError);
}

TEST_CASE("Semantic - string literal has ptr type and passes ptr parameter", "[semantic]") {
    // String literals are typed as ptr (pointer to null-terminated char data).
    auto result = analyzeString(R"(
        extern puts(ptr s) -> i32;
        fn main() { puts("hello"); }
    )");
    REQUIRE_FALSE(result.hadError);
}

TEST_CASE("Semantic - string is no longer a reserved keyword", "[semantic]") {
    // 'string' was formerly a type keyword; it is now a plain identifier and may be used as a
    // function name. If it were still reserved this would be a PARSE error — which analyzeString
    // swallows (returning hadError == false), so a plain REQUIRE_FALSE would pass vacuously. Prove
    // it really parsed and lowered by checking the emitted function symbol.
    std::string ir = codegenString("fn string() -> i32 { return 42; }");
    REQUIRE(ir.find("define i32 @string(") != std::string::npos);
}

TEST_CASE("Semantic - ptr function parameter is valid", "[semantic]") {
    auto result = analyzeString(R"(
        fn process(ptr data) { }
    )");
    REQUIRE_FALSE(result.hadError);
}

TEST_CASE("Semantic - ptr return type is valid", "[semantic]") {
    auto result = analyzeString(R"(
        extern malloc(u64 n) -> ptr;
        fn alloc(u64 size) -> ptr { return malloc(size); }
    )");
    REQUIRE_FALSE(result.hadError);
}

// ============================================================
// Definite assignment — use-before-initialization errors
// ============================================================

TEST_CASE("Uninit - primitive used before any assignment is an error", "[uninit]") {
    StderrCapture cap;
    auto result = analyzeString(R"(
        fn main() -> i32 {
            i32 x;
            return x;
        }
    )");
    REQUIRE(result.hadError);
    REQUIRE(cap.contains("before it has been assigned"));
}

TEST_CASE("Uninit - primitive with initializer is valid", "[uninit]") {
    auto result = analyzeString(R"(
        fn main() -> i32 {
            i32 x = 5;
            return x;
        }
    )");
    REQUIRE_FALSE(result.hadError);
}

TEST_CASE("Uninit - assigned before read is valid", "[uninit]") {
    auto result = analyzeString(R"(
        fn main() -> i32 {
            i32 x;
            x = 5;
            return x;
        }
    )");
    REQUIRE_FALSE(result.hadError);
}

TEST_CASE("Uninit - function parameters are always initialized", "[uninit]") {
    auto result = analyzeString(R"(
        fn double_it(i32 n) -> i32 { return n + n; }
        fn main() -> i32 { return 0; }
    )");
    REQUIRE_FALSE(result.hadError);
}

TEST_CASE("Uninit - value-object declaration is zero-initialized (no error)", "[uninit]") {
    // Class values are zero-initialized by the runtime; reading them is safe.
    auto result = analyzeString(R"(
        class Point { i32 x; i32 y; }
        fn main() -> i32 {
            Point p;
            return p.x;
        }
    )");
    REQUIRE_FALSE(result.hadError);
}

TEST_CASE("Uninit - reference declared without init is an error when read", "[uninit]") {
    StderrCapture cap;
    auto result = analyzeString(R"(
        class Counter { i32 n; Counter(i32 v) { this.n = v; } }
        fn main() -> i32 {
            Counter& c;
            return c.n;
        }
    )");
    REQUIRE(result.hadError);
    REQUIRE(cap.contains("before it has been assigned"));
}

TEST_CASE("Uninit - compound-assign on uninitialized variable is an error", "[uninit]") {
    StderrCapture cap;
    auto result = analyzeString(R"(
        fn main() -> i32 {
            mut i32 x;
            x += 1;
            return x;
        }
    )");
    REQUIRE(result.hadError);
    REQUIRE(cap.contains("before it has been assigned"));
}

TEST_CASE("Uninit - if-else both branches assign: initialized after", "[uninit]") {
    auto result = analyzeString(R"(
        fn pick(bool cond) -> i32 {
            i32 x;
            if (cond) { x = 1; } else { x = 2; }
            return x;
        }
    )");
    REQUIRE_FALSE(result.hadError);
}

TEST_CASE("Uninit - if without else does not guarantee initialization", "[uninit]") {
    StderrCapture cap;
    auto result = analyzeString(R"(
        fn main() -> i32 {
            i32 x;
            if (true) { x = 1; }
            return x;
        }
    )");
    REQUIRE(result.hadError);
    REQUIRE(cap.contains("before it has been assigned"));
}

TEST_CASE("Uninit - if-else only one branch assigns: error after", "[uninit]") {
    StderrCapture cap;
    auto result = analyzeString(R"(
        fn main() -> i32 {
            i32 x;
            if (true) { x = 1; } else { }
            return x;
        }
    )");
    REQUIRE(result.hadError);
    REQUIRE(cap.contains("before it has been assigned"));
}

TEST_CASE("Uninit - while body does not guarantee initialization", "[uninit]") {
    StderrCapture cap;
    auto result = analyzeString(R"(
        fn main() -> i32 {
            i32 x;
            while (false) { x = 1; }
            return x;
        }
    )");
    REQUIRE(result.hadError);
    REQUIRE(cap.contains("before it has been assigned"));
}

TEST_CASE("Uninit - for body does not guarantee initialization", "[uninit]") {
    StderrCapture cap;
    auto result = analyzeString(R"(
        fn main() -> i32 {
            mut i32 x;
            for (mut i32 i = 0; i < 10; i++) { x = i; }
            return x;
        }
    )");
    REQUIRE(result.hadError);
    REQUIRE(cap.contains("before it has been assigned"));
}

TEST_CASE("Uninit - nested if-else: both paths cover all branches is valid", "[uninit]") {
    auto result = analyzeString(R"(
        fn clamp(i32 v, i32 lo, i32 hi) -> i32 {
            i32 result;
            if (v < lo) {
                result = lo;
            } else if (v > hi) {
                result = hi;
            } else {
                result = v;
            }
            return result;
        }
    )");
    REQUIRE_FALSE(result.hadError);
}

TEST_CASE("Uninit - variable initialized before loop body always read is valid", "[uninit]") {
    auto result = analyzeString(R"(
        fn main() -> i32 {
            mut i32 sum = 0;
            for (mut i32 i = 0; i < 10; i++) { sum = sum + i; }
            return sum;
        }
    )");
    REQUIRE_FALSE(result.hadError);
}

// ============================================================
// Access control — `private` on free functions
// ============================================================
// A free function may be declared `fn private name(...)`. Like a private field, this is
// file-local: calling it from a DIFFERENT source file is a warning (not an error); calling
// it from the same file is silent. Cross-file needs two real files, so these tests write a
// library file to a temp path and import it by absolute path from an inline source string.

namespace {
    // Write `libSource` to a per-process temp file and return an importing source string
    // (absolute import path, forward-slashed for the GG string literal) that runs `body`.
    std::string importingSource(const std::string& libSource, const std::string& body) {
        auto dir  = std::filesystem::temp_directory_path();
        auto name = "gg_priv_lib_" +
#ifdef _WIN32
            std::to_string(_getpid())
#else
            std::to_string(getpid())
#endif
            + ".gg";
        auto libPath = (dir / name).string();
        std::ofstream(libPath) << libSource;
        std::string importPath = libPath;
        std::replace(importPath.begin(), importPath.end(), '\\', '/');
        return "import \"" + importPath + "\";\n" + body;
    }
}

TEST_CASE("Private fn - parser sets isPublic on the FunctionDeclStmt", "[private][parser]") {
    auto prog = parseStringRaw(R"(
        fn private secret() -> i32 { return 1; }
        fn open() -> i32 { return 2; }
    )");
    REQUIRE(prog.declarations.size() == 2);
    const auto& secret = asStmt<FunctionDeclStmt>(prog.declarations[0]);
    const auto& open   = asStmt<FunctionDeclStmt>(prog.declarations[1]);
    REQUIRE(secret.name.lexeme == "secret");
    REQUIRE_FALSE(secret.isPublic);
    REQUIRE(open.isPublic);
}

TEST_CASE("Private fn - a private function is callable within its own file", "[private][semantic]") {
    StderrCapture cap;
    auto result = analyzeString(R"(
        fn private secret() -> i32 { return 42; }
        fn main() -> i32 { return secret(); }
    )");
    REQUIRE_FALSE(result.hadError);
    REQUIRE_FALSE(cap.contains("is private to its source file"));
}

TEST_CASE("Private fn - calling a private function cross-file warns (not errors)", "[private][semantic]") {
    StderrCapture cap;
    auto result = analyzeString(importingSource(
        "fn private secret() -> i32 { return 42; }\n",
        "fn main() -> i32 { return secret(); }\n"));
    REQUIRE_FALSE(result.hadError);                         // warning, not error
    REQUIRE(cap.contains("function 'secret' is private to its source file"));
}

TEST_CASE("Private fn - a public function called cross-file does not warn", "[private][semantic]") {
    StderrCapture cap;
    auto result = analyzeString(importingSource(
        "fn open() -> i32 { return 42; }\n",
        "fn main() -> i32 { return open(); }\n"));
    REQUIRE_FALSE(result.hadError);
    REQUIRE_FALSE(cap.contains("is private to its source file"));
}

TEST_CASE("Private fn - a private function used internally then imported warns only at the cross-file site",
          "[private][semantic]") {
    StderrCapture cap;
    // The library calls its own private `secret` (same file → silent); the importer calls it
    // directly (cross-file → one warning).
    auto result = analyzeString(importingSource(
        "fn private secret() -> i32 { return 42; }\n"
        "fn helper() -> i32 { return secret(); }\n",
        "fn main() -> i32 { return helper() + secret(); }\n"));
    REQUIRE_FALSE(result.hadError);
    const std::string& out = cap.str();
    // Exactly one occurrence of the warning (the importer's direct call), none for helper().
    auto first = out.find("is private to its source file");
    REQUIRE(first != std::string::npos);
    REQUIRE(out.find("is private to its source file", first + 1) == std::string::npos);
}
