#include <catch2/catch_test_macros.hpp>
#include "helpers.h"

// ============================================================
// Const-by-default variables and the `mut` keyword
// ============================================================
// Bindings (locals, parameters) and instance fields are immutable by default.
// A `mut` on the declaration makes them reassignable. A const local permits
// exactly one defining assignment (deferred init OK); any later reassignment,
// compound-assignment, or ++/-- is an error. A const instance field is writable
// only via `this.field = ...` inside the class's own constructor.

// ------------------------------------------------------------
// Parser tests
// ------------------------------------------------------------

TEST_CASE("Mut - local carries isMut flag", "[mut][parser]") {
    auto prog = parseString(R"(
        fn main() -> i32 {
            mut i32 a = 0;
            i32 b = 1;
            return b;
        }
    )");
    REQUIRE(prog.declarations.size() == 1);
    const auto& fn = asStmt<FunctionDeclStmt>(prog.declarations[0]);
    const auto& aDecl = asExpr<VarDeclExpr>(std::get<ExprStmt>(*fn.body.body[0]->node).expression);
    const auto& bDecl = asExpr<VarDeclExpr>(std::get<ExprStmt>(*fn.body.body[1]->node).expression);
    REQUIRE(aDecl.isMut);
    REQUIRE_FALSE(bDecl.isMut);
}

TEST_CASE("Mut - 'mut static' and 'static mut' both parse", "[mut][parser]") {
    for (const char* src : { "fn f() -> i32 { mut static i32 c = 0; return c; }",
                             "fn f() -> i32 { static mut i32 c = 0; return c; }" }) {
        auto prog = parseString(src);
        REQUIRE(prog.declarations.size() == 1);
        const auto& fn = asStmt<FunctionDeclStmt>(prog.declarations[0]);
        const auto& d = asExpr<VarDeclExpr>(std::get<ExprStmt>(*fn.body.body[0]->node).expression);
        REQUIRE(d.isMut);
        REQUIRE(d.isStatic);
    }
}

TEST_CASE("Mut - parameter carries isMut flag", "[mut][parser]") {
    auto prog = parseString(R"(
        fn f(mut i32 n, i32 m) -> i32 { return m; }
    )");
    REQUIRE(prog.declarations.size() == 1);
    const auto& fn = asStmt<FunctionDeclStmt>(prog.declarations[0]);
    REQUIRE(fn.params.size() == 2);
    REQUIRE(fn.params[0].isMut);
    REQUIRE_FALSE(fn.params[1].isMut);
}

TEST_CASE("Mut - field carries isMut flag", "[mut][parser]") {
    auto prog = parseString(R"(
        class C {
            mut i32 counter;
            i32 fixed;
        }
    )");
    REQUIRE(prog.declarations.size() == 1);
    const auto& cls = asStmt<ClassDeclStmt>(prog.declarations[0]);
    REQUIRE(cls.fields.size() == 2);
    REQUIRE(cls.fields[0].isMut);
    REQUIRE_FALSE(cls.fields[1].isMut);
}

TEST_CASE("Mut - 'private mut' and 'mut private' field orders both parse", "[mut][parser]") {
    for (const char* src : { "class C { private mut i32 x; }",
                             "class C { mut private i32 x; }" }) {
        auto prog = parseString(src);
        REQUIRE(prog.declarations.size() == 1);
        const auto& cls = asStmt<ClassDeclStmt>(prog.declarations[0]);
        REQUIRE(cls.fields.size() == 1);
        REQUIRE(cls.fields[0].isMut);
        REQUIRE_FALSE(cls.fields[0].isPublic);
    }
}

TEST_CASE("Mut - a method declared without 'fn' is a parse error", "[mut][parser]") {
    StderrCapture cap;
    auto prog = parseString(R"(
        class C { i32 get() { return 0; } }
    )");
    REQUIRE(cap.contains("methods must be declared with 'fn'"));
}

TEST_CASE("Mut - trailing 'mut' sets the method's isMut flag", "[mut][parser]") {
    auto prog = parseString(R"(
        class C {
            fn set() mut { }
            fn get() -> i32     { return 0; }
        }
    )");
    REQUIRE(prog.declarations.size() == 1);
    const auto& cls = asStmt<ClassDeclStmt>(prog.declarations[0]);
    REQUIRE(cls.methods.size() == 2);
    REQUIRE(cls.methods[0].name.lexeme == "set");
    REQUIRE(cls.methods[0].isMut);
    REQUIRE_FALSE(cls.methods[1].isMut);   // get() is read-only
}

TEST_CASE("Mut - trailing 'mut' on a static method is a parse error", "[mut][parser]") {
    StderrCapture cap;
    auto prog = parseString(R"(
        class C { fn static f() mut { } }
    )");
    REQUIRE(cap.contains("static methods cannot be 'mut'"));
}

TEST_CASE("Mut - trailing 'mut' on an enum method is a parse error", "[mut][parser]") {
    StderrCapture cap;
    auto prog = parseString(R"(
        enum E { A; fn v() mut -> i32 { return 0; } }
    )");
    REQUIRE(cap.contains("enum methods cannot be 'mut'"));
}

TEST_CASE("Mut - 'mut' on an enum field is a parse error", "[mut][parser]") {
    StderrCapture cap;
    auto prog = parseString(R"(
        enum E { A; mut i32 x; E() { this.x = 0; } }
    )");
    REQUIRE(cap.contains("enum fields are always immutable"));
}

// ------------------------------------------------------------
// Semantic — errors
// ------------------------------------------------------------

TEST_CASE("Const - reassigning a const local is an error", "[mut][const][semantic]") {
    StderrCapture cap;
    auto result = analyzeString(R"(
        fn main() -> i32 {
            i32 x = 1;
            x = 2;
            return x;
        }
    )");
    REQUIRE(result.hadError);
    REQUIRE(cap.contains("cannot reassign immutable variable 'x'"));
}

TEST_CASE("Const - compound-assigning a const local is an error", "[mut][const][semantic]") {
    StderrCapture cap;
    auto result = analyzeString(R"(
        fn main() -> i32 {
            i32 x = 1;
            x += 2;
            return x;
        }
    )");
    REQUIRE(result.hadError);
    REQUIRE(cap.contains("cannot mutate immutable variable 'x'"));
}

TEST_CASE("Const - ++ on a const local is an error", "[mut][const][semantic]") {
    StderrCapture cap;
    auto result = analyzeString(R"(
        fn main() -> i32 {
            i32 x = 1;
            x++;
            return x;
        }
    )");
    REQUIRE(result.hadError);
    REQUIRE(cap.contains("cannot mutate immutable variable 'x'"));
}

TEST_CASE("Const - prefix ++ on a const local is an error", "[mut][const][semantic]") {
    StderrCapture cap;
    auto result = analyzeString(R"(
        fn main() -> i32 {
            i32 x = 1;
            ++x;
            return x;
        }
    )");
    REQUIRE(result.hadError);
    REQUIRE(cap.contains("cannot mutate immutable variable 'x'"));
}

TEST_CASE("Const - reassigning a const parameter is an error", "[mut][const][semantic]") {
    StderrCapture cap;
    auto result = analyzeString(R"(
        fn f(i32 n) -> i32 {
            n = n + 1;
            return n;
        }
    )");
    REQUIRE(result.hadError);
    REQUIRE(cap.contains("cannot reassign immutable variable 'n'"));
}

TEST_CASE("Mut - a mut reference parameter is a mutable borrow (writes allowed)", "[mut][semantic]") {
    auto result = analyzeString(R"(
        class Point { mut i32 x; Point(i32 x) { this.x = x; } }
        fn f(mut Point& p) { p.x = 5; }
    )");
    REQUIRE_FALSE(result.hadError);
}

TEST_CASE("Const - writing a field through a const reference parameter is an error", "[mut][const][semantic]") {
    StderrCapture cap;
    auto result = analyzeString(R"(
        class Point { mut i32 x; Point(i32 x) { this.x = x; } }
        fn f(Point& p) { p.x = 5; }
    )");
    REQUIRE(result.hadError);
    REQUIRE(cap.contains("through an immutable binding"));
}

TEST_CASE("Const - assigning a const instance field outside the ctor is an error", "[mut][const][semantic]") {
    StderrCapture cap;
    auto result = analyzeString(R"(
        class Point {
            i32 x;
            Point(i32 x) { this.x = x; }
            fn bump() { this.x = this.x + 1; }
        }
    )");
    REQUIRE(result.hadError);
    REQUIRE(cap.contains("cannot assign to immutable field 'x'"));
}

TEST_CASE("Const - assigning a const field through an instance is an error", "[mut][const][semantic]") {
    StderrCapture cap;
    auto result = analyzeString(R"(
        class Point { i32 x; Point(i32 x) { this.x = x; } }
        fn main() -> i32 {
            Point& p = new Point(1);
            p.x = 5;
            return 0;
        }
    )");
    REQUIRE(result.hadError);
    REQUIRE(cap.contains("cannot assign to immutable field 'x'"));
}

// ------------------------------------------------------------
// Semantic — accepted
// ------------------------------------------------------------

TEST_CASE("Const - single deferred defining assignment is allowed", "[mut][const][semantic]") {
    auto result = analyzeString(R"(
        fn main() -> i32 {
            i32 x;
            x = 5;
            return x;
        }
    )");
    REQUIRE_FALSE(result.hadError);
}

TEST_CASE("Const - if/else split initialization of a const is allowed", "[mut][const][semantic]") {
    auto result = analyzeString(R"(
        fn pick(i32 c) -> i32 {
            i32 x;
            if (c > 0) { x = 1; } else { x = 2; }
            return x;
        }
    )");
    REQUIRE_FALSE(result.hadError);
}

// Once a const is definitely initialized by BOTH branches of an if/else, it is fully
// initialized afterwards — so a further write is a reassignment, not a defining
// assignment, and must be rejected. This pins that the branch-merge marks the binding
// initialized for const-enforcement, not merely for read-safety.
TEST_CASE("Const - a const assigned in both branches cannot be reassigned after", "[mut][const][semantic]") {
    StderrCapture cap;
    auto result = analyzeString(R"(
        fn main() -> i32 {
            i32 x;
            if (true) { x = 1; } else { x = 2; }
            x = 3;
            return x;
        }
    )");
    REQUIRE(result.hadError);
    REQUIRE(cap.contains("cannot reassign immutable variable 'x'"));
}

// The dual: with only one branch assigning, the merge conservatively resets the binding
// to uninitialized, so the post-if write IS the single defining assignment — allowed.
TEST_CASE("Const - a const assigned in only one branch may still be defined after", "[mut][const][semantic]") {
    auto result = analyzeString(R"(
        fn main() -> i32 {
            i32 x;
            if (true) { x = 1; }
            x = 3;
            return x;
        }
    )");
    REQUIRE_FALSE(result.hadError);
}

// A loop body may run zero times, so an assignment inside it never establishes
// initialization for the code after the loop.
TEST_CASE("Const - a loop-body assignment does not initialize a const for use after the loop", "[mut][const][semantic]") {
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

// Deferred initialization applies to `mut` bindings too — definite-assignment is about
// read-safety, independent of mutability. Assigning in only one branch leaves it possibly
// unassigned on read.
TEST_CASE("Const - a mut binding assigned in only one branch is unassigned on a later read", "[mut][const][semantic]") {
    StderrCapture cap;
    auto result = analyzeString(R"(
        fn main() -> i32 {
            mut i32 x;
            if (true) { x = 1; } else { }
            return x;
        }
    )");
    REQUIRE(result.hadError);
    REQUIRE(cap.contains("before it has been assigned"));
}

// The branch-merge recurses: nested if/else that assigns on every path fully initializes.
TEST_CASE("Const - nested if/else assigning on every path initializes a const", "[mut][const][semantic]") {
    auto result = analyzeString(R"(
        fn pick(i32 a, i32 b) -> i32 {
            i32 x;
            if (a > 0) {
                if (b > 0) { x = 1; } else { x = 2; }
            } else {
                x = 3;
            }
            return x;
        }
    )");
    REQUIRE_FALSE(result.hadError);
}

TEST_CASE("Mut - reassigning a mut local is allowed", "[mut][semantic]") {
    auto result = analyzeString(R"(
        fn main() -> i32 {
            mut i32 x = 0;
            x = 1;
            x += 2;
            x++;
            return x;
        }
    )");
    REQUIRE_FALSE(result.hadError);
}

TEST_CASE("Mut - reassigning a mut parameter is allowed", "[mut][semantic]") {
    auto result = analyzeString(R"(
        fn f(mut i32 n) -> i32 {
            n = n + 1;
            return n;
        }
    )");
    REQUIRE_FALSE(result.hadError);
}

TEST_CASE("Mut - a mut method may write a mut field outside the ctor", "[mut][semantic]") {
    auto result = analyzeString(R"(
        class Counter {
            mut i32 n;
            Counter() { this.n = 0; }
            fn inc() mut { this.n = this.n + 1; }
        }
    )");
    REQUIRE_FALSE(result.hadError);
}

TEST_CASE("Mut - a non-mut method may not write a field", "[mut][semantic]") {
    StderrCapture cap;
    auto result = analyzeString(R"(
        class Counter {
            mut i32 n;
            Counter() { this.n = 0; }
            fn inc() { this.n = this.n + 1; }
        }
    )");
    REQUIRE(result.hadError);
    REQUIRE(cap.contains("in a read-only method; declare the method 'mut'"));
}

TEST_CASE("Const - assigning a const field via this in the ctor is allowed", "[mut][const][semantic]") {
    auto result = analyzeString(R"(
        class Point {
            i32 x;
            i32 y;
            Point(i32 x, i32 y) { this.x = x; this.y = y; }
        }
    )");
    REQUIRE_FALSE(result.hadError);
}

// ------------------------------------------------------------
// Semantic — value objects, references, statics, arrays, decrement
// ------------------------------------------------------------

TEST_CASE("Const - -- on a const local is an error", "[mut][const][semantic]") {
    StderrCapture cap;
    auto result = analyzeString(R"(
        fn main() -> i32 {
            i32 x = 5;
            x--;
            return x;
        }
    )");
    REQUIRE(result.hadError);
    REQUIRE(cap.contains("cannot mutate immutable variable 'x'"));
}

TEST_CASE("Const - reassigning a const value-object variable is an error", "[mut][const][semantic]") {
    StderrCapture cap;
    auto result = analyzeString(R"(
        class Point { mut i32 x; Point(i32 x) { this.x = x; } }
        fn main() -> i32 {
            Point a(1);
            Point b(2);
            a = b;
            return 0;
        }
    )");
    REQUIRE(result.hadError);
    REQUIRE(cap.contains("cannot reassign immutable variable 'a'"));
}

TEST_CASE("Const - rebinding a const reference local is an error", "[mut][const][semantic]") {
    StderrCapture cap;
    auto result = analyzeString(R"(
        class Point { mut i32 x; Point(i32 x) { this.x = x; } }
        fn main() -> i32 {
            Point& a = new Point(1);
            Point& b = new Point(2);
            a = b;
            return 0;
        }
    )");
    REQUIRE(result.hadError);
    REQUIRE(cap.contains("cannot reassign immutable variable 'a'"));
}

TEST_CASE("Mut - rebinding a mut reference local is allowed", "[mut][semantic]") {
    auto result = analyzeString(R"(
        class Point { mut i32 x; Point(i32 x) { this.x = x; } }
        fn main() -> i32 {
            mut Point& a = new Point(1);
            Point& b = new Point(2);
            a = b;
            return 0;
        }
    )");
    REQUIRE_FALSE(result.hadError);
}

TEST_CASE("Mut - writing a mut field through a mut reference is allowed", "[mut][semantic]") {
    auto result = analyzeString(R"(
        class Counter { mut i32 n; Counter() { this.n = 0; } }
        fn main() -> i32 {
            mut Counter& c = new Counter();
            c.n = 9;
            return 0;
        }
    )");
    REQUIRE_FALSE(result.hadError);
}

// Transitive const: a const object binding forbids writes to its fields too, even `mut`
// ones. The whole binding must be `mut`.
TEST_CASE("Const - writing a mut field through a const object binding is an error", "[mut][const][semantic]") {
    StderrCapture cap;
    auto result = analyzeString(R"(
        class Point { mut i32 x; Point(i32 x) { this.x = x; } }
        fn main() -> i32 {
            Point p(1);
            p.x = 5;
            return p.x;
        }
    )");
    REQUIRE(result.hadError);
    REQUIRE(cap.contains("through an immutable binding"));
}

TEST_CASE("Mut - writing a mut field through a mut object binding is allowed", "[mut][semantic]") {
    auto result = analyzeString(R"(
        class Point { mut i32 x; Point(i32 x) { this.x = x; } }
        fn main() -> i32 {
            mut Point p(1);
            p.x = 5;
            return p.x;
        }
    )");
    REQUIRE_FALSE(result.hadError);
}

// Array/pointer *element* writes are not gated by `mut` — only the binding is.
TEST_CASE("Const - array element write does not require mut", "[mut][const][semantic]") {
    auto result = analyzeString(R"(
        fn main() -> i32 {
            i32[4] a;
            a[0] = 5;
            a[1] = 7;
            return a[0] + a[1];
        }
    )");
    REQUIRE_FALSE(result.hadError);
}

TEST_CASE("Const - reassigning a const static local is an error", "[mut][const][semantic]") {
    StderrCapture cap;
    auto result = analyzeString(R"(
        fn tick() -> i32 {
            static i32 n = 0;
            n = n + 1;
            return n;
        }
    )");
    REQUIRE(result.hadError);
    REQUIRE(cap.contains("cannot reassign immutable variable 'n'"));
}

TEST_CASE("Mut - a mut static local can be reassigned", "[mut][semantic]") {
    auto result = analyzeString(R"(
        fn tick() -> i32 {
            static mut i32 n = 0;
            n = n + 1;
            return n;
        }
    )");
    REQUIRE_FALSE(result.hadError);
}

// ------------------------------------------------------------
// Transitive const through references + const→mut cast warning
// ------------------------------------------------------------

TEST_CASE("Const - writing a field through a const reference local is an error", "[mut][const][semantic]") {
    StderrCapture cap;
    auto result = analyzeString(R"(
        class Point { mut i32 x; Point(i32 x) { this.x = x; } }
        fn main() -> i32 {
            Point& p = new Point(1);
            p.x = 5;
            return 0;
        }
    )");
    REQUIRE(result.hadError);
    REQUIRE(cap.contains("through an immutable binding"));
}

TEST_CASE("Cast - coercing a const reference into a mut binding warns", "[mut][cast][semantic]") {
    StderrCapture cap;
    auto result = analyzeString(R"(
        class Point { mut i32 x; Point(i32 x) { this.x = x; } }
        fn main() -> i32 {
            Point& b = new Point(1);
            mut Point& a = b;
            return 0;
        }
    )");
    REQUIRE_FALSE(result.hadError);                 // it is a warning, not an error
    REQUIRE(cap.contains("read-only (const) reference into a 'mut' binding"));
}

TEST_CASE("Cast - an explicit 'as mut T' silences the const-to-mut warning", "[mut][cast][semantic]") {
    StderrCapture cap;
    auto result = analyzeString(R"(
        class Point { mut i32 x; Point(i32 x) { this.x = x; } }
        fn main() -> i32 {
            Point& b = new Point(1);
            mut Point& a = b as mut Point&;
            return 0;
        }
    )");
    REQUIRE_FALSE(result.hadError);
    REQUIRE_FALSE(cap.contains("read-only (const) reference"));
}

TEST_CASE("Cast - initialising a mut ref from new (owned) does not warn", "[mut][cast][semantic]") {
    StderrCapture cap;
    auto result = analyzeString(R"(
        class Point { mut i32 x; Point(i32 x) { this.x = x; } }
        fn main() -> i32 {
            mut Point& a = new Point(1);
            return 0;
        }
    )");
    REQUIRE_FALSE(result.hadError);
    REQUIRE_FALSE(cap.contains("read-only (const) reference"));
}

TEST_CASE("Cast - passing a const reference to a mut ref parameter warns", "[mut][cast][semantic]") {
    StderrCapture cap;
    auto result = analyzeString(R"(
        class Point { mut i32 x; Point(i32 x) { this.x = x; } }
        fn mutate(mut Point& p) { p.x = 1; }
        fn main() -> i32 {
            Point& b = new Point(1);
            mutate(b);
            return 0;
        }
    )");
    REQUIRE_FALSE(result.hadError);
    REQUIRE(cap.contains("read-only (const) reference into a 'mut' binding"));
}

TEST_CASE("Cast - passing a mut reference to a mut ref parameter does not warn", "[mut][cast][semantic]") {
    StderrCapture cap;
    auto result = analyzeString(R"(
        class Point { mut i32 x; Point(i32 x) { this.x = x; } }
        fn mutate(mut Point& p) { p.x = 1; }
        fn main() -> i32 {
            mut Point& b = new Point(1);
            mutate(b);
            return 0;
        }
    )");
    REQUIRE_FALSE(result.hadError);
    REQUIRE_FALSE(cap.contains("read-only (const) reference"));
}

TEST_CASE("Cast - rebinding a mut ref local from a const ref warns", "[mut][cast][semantic]") {
    StderrCapture cap;
    auto result = analyzeString(R"(
        class Point { mut i32 x; Point(i32 x) { this.x = x; } }
        fn main() -> i32 {
            mut Point& a = new Point(1);
            Point& b = new Point(2);
            a = b;
            return 0;
        }
    )");
    REQUIRE_FALSE(result.hadError);
    REQUIRE(cap.contains("read-only (const) reference into a 'mut' binding"));
}

TEST_CASE("Cast - passing a mut ref to a read-only parameter is silent", "[mut][cast][semantic]") {
    StderrCapture cap;
    auto result = analyzeString(R"(
        class Point { mut i32 x; Point(i32 x) { this.x = x; } }
        fn readX(Point& p) -> i32 { return p.x; }
        fn main() -> i32 {
            mut Point& b = new Point(1);
            return readX(b);
        }
    )");
    REQUIRE_FALSE(result.hadError);
    REQUIRE_FALSE(cap.contains("read-only (const) reference"));
}

TEST_CASE("Mut - a mut ref parameter still may not be rebound", "[mut][semantic]") {
    StderrCapture cap;
    auto result = analyzeString(R"(
        class Point { mut i32 x; Point(i32 x) { this.x = x; } }
        fn f(mut Point& p, Point& other) { p = other; }
    )");
    REQUIRE(result.hadError);
    REQUIRE(cap.contains("cannot rebind reference parameter"));
}

// Transitive const recurses along a field-access chain: every link must be mutable.
TEST_CASE("Const - nested field write through a fully-mut chain is allowed", "[mut][const][semantic]") {
    auto result = analyzeString(R"(
        class Inner { mut i32 x; Inner(i32 v) { this.x = v; } }
        class Box   { mut Inner& inner; Box(Inner& i) { this.inner = i; } }
        fn main() -> i32 {
            mut Box& o = new Box(new Inner(1));
            o.inner.x = 5;
            return 0;
        }
    )");
    REQUIRE_FALSE(result.hadError);
}

TEST_CASE("Const - nested field write through a const root is an error", "[mut][const][semantic]") {
    StderrCapture cap;
    auto result = analyzeString(R"(
        class Inner { mut i32 x; Inner(i32 v) { this.x = v; } }
        class Box   { mut Inner& inner; Box(Inner& i) { this.inner = i; } }
        fn main() -> i32 {
            Box& o = new Box(new Inner(1));
            o.inner.x = 5;
            return 0;
        }
    )");
    REQUIRE(result.hadError);
    REQUIRE(cap.contains("through an immutable binding"));
}

// Static fields are class-level, not part of the instance — a write through an instance
// is NOT subject to the transitive-const receiver check.
TEST_CASE("Const - static field write through a const instance is allowed", "[mut][const][semantic]") {
    // A `mut static` is writable through even a const instance binding: static fields are
    // class-level, so the transitive-const receiver check does not apply to them.
    auto result = analyzeString(R"(
        class Counter { mut static i32 total; }
        fn main() -> i32 {
            Counter c;
            c.total = 5;
            return Counter::total;
        }
    )");
    REQUIRE_FALSE(result.hadError);
}

// A `mut` method mutates the receiver, so it needs a mutable binding (Rust &mut self).
TEST_CASE("Mut - calling a mut method on a const object is an error", "[mut][const][semantic]") {
    StderrCapture cap;
    auto result = analyzeString(R"(
        class Counter {
            mut i32 n;
            Counter() { this.n = 0; }
            fn inc() mut { this.n = this.n + 1; }
        }
        fn main() -> i32 {
            Counter c();
            c.inc();
            return 0;
        }
    )");
    REQUIRE(result.hadError);
    REQUIRE(cap.contains("cannot call mutating method 'inc' through an immutable binding"));
}

TEST_CASE("Mut - calling a mut method on a mut object is allowed", "[mut][semantic]") {
    auto result = analyzeString(R"(
        class Counter {
            mut i32 n;
            Counter() { this.n = 0; }
            fn inc() mut { this.n = this.n + 1; }
        }
        fn main() -> i32 {
            mut Counter c();
            c.inc();
            return 0;
        }
    )");
    REQUIRE_FALSE(result.hadError);
}

// A read-only method may still be called on a const object (getters etc.).
TEST_CASE("Const - calling a non-mut method on a const object is allowed", "[mut][const][semantic]") {
    auto result = analyzeString(R"(
        class Counter {
            mut i32 n;
            Counter() { this.n = 0; }
            fn get() -> i32 { return this.n; }
        }
        fn main() -> i32 {
            Counter c();
            return c.get();
        }
    )");
    REQUIRE_FALSE(result.hadError);
}

// A non-mut method may not call a mut method on `this`.
TEST_CASE("Mut - a non-mut method cannot call a mut method on this", "[mut][semantic]") {
    StderrCapture cap;
    auto result = analyzeString(R"(
        class Counter {
            mut i32 n;
            Counter() { this.n = 0; }
            fn inc() mut { this.n = this.n + 1; }
            fn tick() { this.inc(); }
        }
    )");
    REQUIRE(result.hadError);
    REQUIRE(cap.contains("on 'this' in a read-only method"));
}

// A mut method may call another mut method on `this`.
TEST_CASE("Mut - a mut method may call a mut method on this", "[mut][semantic]") {
    auto result = analyzeString(R"(
        class Counter {
            mut i32 n;
            Counter() { this.n = 0; }
            fn inc()  mut { this.n = this.n + 1; }
            fn tick() mut { this.inc(); }
        }
    )");
    REQUIRE_FALSE(result.hadError);
}

// ------------------------------------------------------------
// Codegen — mutability is a compile-time check; IR is unaffected
// ------------------------------------------------------------

TEST_CASE("Mut - a mutated loop lowers to alloca + store + arithmetic", "[mut][codegen]") {
    std::string ir = codegenString(R"(
        fn main() -> i32 {
            mut i32 total = 0;
            for (mut i32 i = 1; i <= 3; i++) {
                total = total + i;
            }
            return total;
        }
    )");
    REQUIRE(ir.find("alloca i32") != std::string::npos);
    REQUIRE(ir.find("store i32") != std::string::npos);
    REQUIRE(ir.find("add i32") != std::string::npos);
}

TEST_CASE("Const - single defining assignment lowers to a store", "[mut][const][codegen]") {
    std::string ir = codegenString(R"(
        fn main() -> i32 {
            i32 x;
            x = 7;
            return x;
        }
    )");
    REQUIRE(ir.find("store i32 7") != std::string::npos);
}

// ============================================================
// Definite initialization for Object (class) locals — a bare `ClassName name;` used to be
// unconditionally treated as definitely-initialized the moment its struct was zero-initialized,
// regardless of whether the class even has a constructor. That let a method call run on an object
// whose constructor never executed (e.g. Array<T>'s ctor, which sets up cap/data) — a real
// heap-corruption bug. Now: a class with NO constructor at all is unaffected (zero-init is its
// complete valid state); a class with ANY constructor — including a zero-arg-callable one — is
// ALWAYS deferred: a bare declaration never implicitly calls anything, whether the class happens to
// have a zero-arg overload or not. This is a deliberate no-magic rule (whether a bare declaration is
// safe to use immediately must never depend on the invisible detail of the class's constructor
// shape) — an explicit `ClassName name();`/`ClassName name{}` (an ordinary initializer, unaffected
// by this rule) or a later assignment on every reachable path is required, which incidentally also
// enables picking a different constructor per branch with no default-construction overhead.
// ============================================================

TEST_CASE("Const - a ctor-less class's bare declaration is unaffected", "[mut][const][semantic]") {
    auto result = analyzeString(R"(
        class Point { mut i32 x; mut i32 y; }
        fn main() -> i32 {
            mut Point p;
            p.x = 5;
            return p.x;
        }
    )");
    REQUIRE_FALSE(result.hadError);
}

TEST_CASE("Const - a bare declaration never implicitly calls a zero-arg constructor",
          "[mut][const][semantic]") {
    StderrCapture cap;
    auto result = analyzeString(R"(
        class Widget { mut i32 v; Widget() { v = 7; } }
        fn main() -> i32 { Widget w; return w.v; }
    )");
    REQUIRE(result.hadError);
    REQUIRE(cap.contains("'w' is used before it has been assigned a value"));
}

TEST_CASE("Const - an explicit empty-arg call to a zero-arg constructor is usable immediately",
          "[mut][const][semantic]") {
    auto result = analyzeString(R"(
        class Widget { mut i32 v; Widget() { v = 7; } }
        fn main() -> i32 { Widget w(); return w.v; }
    )");
    REQUIRE_FALSE(result.hadError);
}

TEST_CASE("Const - a class with ONLY parameterized constructors requires assignment before use",
          "[mut][const][semantic]") {
    StderrCapture cap;
    auto result = analyzeString(R"(
        class Widget { mut i32 v; Widget(i32 x) { v = x; } }
        fn main() -> i32 { Widget w; return w.v; }
    )");
    REQUIRE(result.hadError);
    REQUIRE(cap.contains("'w' is used before it has been assigned a value"));
}

TEST_CASE("Const - an all-defaulted constructor is still deferred on a bare declaration",
          "[mut][const][semantic]") {
    // An all-defaulted ctor is callable with zero arguments (`Widget()` would work), but that does
    // NOT make a BARE declaration special-cased anymore — only an explicit call does.
    StderrCapture cap;
    auto result = analyzeString(R"(
        class Widget { mut i32 v; Widget(i32 x = 3) { v = x; } }
        fn main() -> i32 { Widget w; return w.v; }
    )");
    REQUIRE(result.hadError);
    REQUIRE(cap.contains("'w' is used before it has been assigned a value"));
}

TEST_CASE("Const - a MIXED ctor set (zero-arg overload alongside parameterized ones) is still deferred",
          "[mut][const][semantic]") {
    // A zero-arg-callable overload coexisting with other overloads must not special-case the bare
    // declaration either — every existing test used a PURE zero-arg or PURE parameterized ctor set;
    // this pins down the mixed-overload-set shape specifically.
    StderrCapture cap;
    auto result = analyzeString(R"(
        class Widget {
            mut i32 v;
            Widget() { v = 0; }
            Widget(i32 x) { v = x; }
        }
        fn main() -> i32 { mut Widget w; w.v = 1; return w.v; }
    )");
    REQUIRE(result.hadError);
    REQUIRE(cap.contains("'w' is used before it has been assigned a value"));
}

TEST_CASE("Const - passing a deferred-init object as a call argument before assignment is an error",
          "[mut][const][semantic]") {
    // Every other deferred-init test reads the variable via a field access or method call; this
    // pins down the argument-passing path specifically (goes through the same identifier-use check).
    StderrCapture cap;
    auto result = analyzeString(R"(
        class Widget { mut i32 v; Widget() { v = 0; } }
        fn take(Widget& w) -> i32 { return w.v; }
        fn main() -> i32 { mut Widget w; return take(w); }
    )");
    REQUIRE(result.hadError);
    REQUIRE(cap.contains("'w' is used before it has been assigned a value"));
}

TEST_CASE("Const - the direct-construct fast path works when a branch picks the zero-arg overload "
          "of a mixed ctor set", "[mut][const][codegen]") {
    std::string ir = codegenString(R"(
        class Widget {
            mut i32 v;
            Widget() { v = 0; }
            Widget(i32 x) { v = x; }
        }
        fn pick(bool useDefault) -> i32 {
            mut Widget w;
            if (useDefault) { w = Widget(); } else { w = Widget(9); }
            return w.v;
        }
        fn main() -> i32 { return pick(true); }
    )");
    REQUIRE(ir.find("call void @Widget_Widget$ret$void(ptr ") != std::string::npos);
    REQUIRE(ir.find("call void @Widget_Widget$i32$ret$void(ptr ") != std::string::npos);
    REQUIRE(ir.find("@Widget_clone") == std::string::npos);
}

TEST_CASE("Const - deferred object init allows picking a different constructor per branch",
          "[mut][const][semantic]") {
    auto result = analyzeString(R"(
        class Widget {
            mut i32 v;
            Widget(i32 x) { v = x; }
            Widget(char c) { v = c as i32; }
        }
        fn pick(i32 cond) -> i32 {
            mut Widget w;
            if (cond > 0) { w = Widget(1); } else { w = Widget('a'); }
            return w.v;
        }
        fn main() -> i32 { return pick(1); }
    )");
    REQUIRE_FALSE(result.hadError);
}

TEST_CASE("Const - a genuine copy on an object's defining assignment is still a clean Clone error",
          "[mut][const][semantic]") {
    // Even though `b`'s defining assignment has no OLD value to leak, copying FROM an existing
    // raw-ptr-owning object still aliases the two independently-destroyed buffers.
    StderrCapture cap;
    auto result = analyzeString(R"(
        extern malloc(u64 size) -> ptr;
        extern free(ptr p);
        class Buf {
            mut ptr<i32> data;
            Buf(i32 s) { data = malloc(16); }
            ~Buf() { free(data); }
        }
        fn main() -> i32 {
            mut Buf a(1);
            mut Buf b;
            b = a;
            return 0;
        }
    )");
    REQUIRE(result.hadError);
    REQUIRE(cap.contains("impl Clone for Buf"));
}

TEST_CASE("Const - an explicit empty-arg constructor call invokes the zero-arg constructor",
          "[mut][const][codegen]") {
    // A bare `Widget w;` no longer calls anything (see the semantic tests above) — only an explicit
    // `Widget w();` (an ordinary initializer, unaffected by the deferred-init rule) does.
    std::string ir = codegenString(R"(
        class Widget { mut i32 v; Widget() { v = 7; } }
        fn main() -> i32 { Widget w(); return w.v; }
    )");
    REQUIRE(ir.find("call void @Widget_Widget(ptr ") != std::string::npos);
}

TEST_CASE("Const - a ctor-less class's bare declaration emits no constructor call",
          "[mut][const][codegen]") {
    std::string ir = codegenString(R"(
        class Point { mut i32 x; }
        fn main() -> i32 { mut Point p; p.x = 5; return p.x; }
    )");
    REQUIRE(ir.find("call void @Point_Point(") == std::string::npos);
}

TEST_CASE("Const - the defining assignment of a deferred-init object constructs directly, no clone",
          "[mut][const][codegen]") {
    std::string ir = codegenString(R"(
        class Widget {
            mut i32 v;
            Widget(i32 x) { v = x; }
            Widget(char c) { v = c as i32; }
        }
        fn pick(i32 cond) -> i32 {
            mut Widget w;
            if (cond > 0) { w = Widget(1); } else { w = Widget('a'); }
            return w.v;
        }
        fn main() -> i32 { return pick(1); }
    )");
    REQUIRE(ir.find("call void @Widget_Widget$i32$ret$void(ptr ") != std::string::npos);
    REQUIRE(ir.find("call void @Widget_Widget$char$ret$void(ptr ") != std::string::npos);
    REQUIRE(ir.find("@Widget_clone") == std::string::npos);
}

TEST_CASE("Const - a reassignment of an already-initialized deferred-init object still clones",
          "[mut][const][codegen]") {
    // Only the DEFINING (first) assignment gets the direct-construct fast path; a subsequent
    // reassignment of the same mut binding must go through the ordinary clone lowering (there is a
    // live value there that a direct construct would silently leak).
    std::string ir = codegenString(R"(
        class Widget { mut i32 v; Widget(i32 x) { v = x; } }
        fn main() -> i32 {
            mut Widget w;
            w = Widget(1);
            w = Widget(2);
            return w.v;
        }
    )");
    REQUIRE(ir.find("@Widget_clone") != std::string::npos);
}

// ============================================================
// Field default initializers (`= expr` / `{args}`) + constructor field-initialization enforcement.
// Every instance field must be definitely assigned in a constructor, unless it has a default
// initializer, is nullable, or embeds a ctor-less class — see the CLAUDE.md invariant of the
// same name.
// ============================================================

TEST_CASE("FieldInit - a primitive '=' default initializer is accepted", "[fieldinit][semantic]") {
    auto result = analyzeString(R"(
        class Widget { mut i32 v; mut i32 count = 0; Widget(i32 x) { v = x; } }
        fn main() -> i32 { Widget w(5); return w.count; }
    )");
    REQUIRE_FALSE(result.hadError);
}

TEST_CASE("FieldInit - a '{args}' default initializer brace-constructs an object field",
          "[fieldinit][semantic]") {
    auto result = analyzeString(R"(
        class Slot { mut i32 v; Slot(i32 x) { v = x; } }
        class Bag { mut Slot s{7}; Bag() { } }
        fn main() -> i32 { Bag b(); return b.s.v; }
    )");
    REQUIRE_FALSE(result.hadError);
}

TEST_CASE("FieldInit - a class with ONLY field initializers gets a synthesized empty constructor",
          "[fieldinit][semantic]") {
    // No explicit constructor at all — the flagship shape (e.g. HashMap<K,V>).
    auto result = analyzeString(R"(
        class Slot { mut i32 v; Slot(i32 x) { v = x; } }
        class Bag { mut Slot s{7}; mut i32 count = 0; }
        fn main() -> i32 { Bag b(); return b.count + b.s.v; }
    )");
    REQUIRE_FALSE(result.hadError);
}

TEST_CASE("FieldInit - the synthesized constructor makes a bare declaration deferred-init too",
          "[fieldinit][semantic]") {
    // Consistent with the no-magic rule: a bare declaration never implicitly calls ANY
    // constructor, including a synthesized one.
    StderrCapture cap;
    auto result = analyzeString(R"(
        class Bag { mut i32 count = 0; }
        fn main() -> i32 { Bag b; return b.count; }
    )");
    REQUIRE(result.hadError);
    REQUIRE(cap.contains("'b' is used before it has been assigned a value"));
}

TEST_CASE("FieldInit - a default initializer cannot reference another field", "[fieldinit][semantic]") {
    StderrCapture cap;
    auto result = analyzeString(R"(
        class Widget { mut i32 a = 1; mut i32 b = a + 1; }
        fn main() -> i32 { return 0; }
    )");
    REQUIRE(result.hadError);
    REQUIRE(cap.contains("use of undeclared identifier 'a'"));
}

TEST_CASE("FieldInit - a default initializer cannot reference 'this'", "[fieldinit][semantic]") {
    StderrCapture cap;
    auto result = analyzeString(R"(
        class Widget {
            mut i32 v;
            Widget() { v = 1; }
            fn getV() -> i32 { return v; }
            mut i32 cached = this.getV();
        }
        fn main() -> i32 { return 0; }
    )");
    REQUIRE(result.hadError);
    REQUIRE(cap.contains("'this' used outside of a class method"));
}

TEST_CASE("FieldInit - enum fields still reject a default initializer", "[fieldinit][semantic]") {
    // Enums have their own, separate, already-correct model (fields set via variant ctor args) —
    // unaffected by (excluded from) this feature entirely.
    StderrCapture cap;
    (void)parseString(R"(
        enum Color { RED, GREEN; i32 code = 0; }
        fn main() -> i32 { return 0; }
    )");
    REQUIRE(cap.contains("expected ';' after field declaration"));
}

TEST_CASE("FieldInit - a constructor missing a non-defaulted field is a clean error",
          "[fieldinit][semantic]") {
    StderrCapture cap;
    auto result = analyzeString(R"(
        class Widget { mut i32 v; mut i32 w; Widget(i32 x) { v = x; } }
        fn main() -> i32 { return 0; }
    )");
    REQUIRE(result.hadError);
    REQUIRE(cap.contains("field 'w' is not initialized in constructor 'Widget'"));
}

TEST_CASE("FieldInit - a field assigned in both if/else branches satisfies the check",
          "[fieldinit][semantic]") {
    auto result = analyzeString(R"(
        class Widget { mut i32 v; Widget(bool c) { if (c) { v = 1; } else { v = 2; } } }
        fn main() -> i32 { return 0; }
    )");
    REQUIRE_FALSE(result.hadError);
}

TEST_CASE("FieldInit - a field assigned in only ONE if branch is still an error",
          "[fieldinit][semantic]") {
    StderrCapture cap;
    auto result = analyzeString(R"(
        class Widget { mut i32 v; Widget(bool c) { if (c) { v = 1; } } }
        fn main() -> i32 { return 0; }
    )");
    REQUIRE(result.hadError);
    REQUIRE(cap.contains("field 'v' is not initialized in constructor 'Widget'"));
}

TEST_CASE("FieldInit - an early 'return;' that skips a field is caught at the return site",
          "[fieldinit][semantic]") {
    StderrCapture cap;
    auto result = analyzeString(R"(
        class Widget {
            mut i32 v;
            Widget(bool skip) {
                if (skip) { return; }
                v = 1;
            }
        }
        fn main() -> i32 { return 0; }
    )");
    REQUIRE(result.hadError);
    REQUIRE(cap.contains("field 'v' is not initialized in constructor 'Widget'"));
}

TEST_CASE("FieldInit - a nullable field is exempt from the must-initialize check",
          "[fieldinit][semantic]") {
    auto result = analyzeString(R"(
        class N { mut i32 v; N&? next; i32? tag; N(i32 x) { v = x; } }
        fn main() -> i32 { return 0; }
    )");
    REQUIRE_FALSE(result.hadError);
}

TEST_CASE("FieldInit - an embedded field whose class has no constructor is exempt",
          "[fieldinit][semantic]") {
    auto result = analyzeString(R"(
        class Point { mut i32 x; mut i32 y; }
        class Line { mut Point p; Line() { } }
        fn main() -> i32 { Line l(); return l.p.x; }
    )");
    REQUIRE_FALSE(result.hadError);
}

TEST_CASE("FieldInit - a mixed zero-arg-and-parameterized ctor set on the field's class is NOT exempt",
          "[fieldinit][semantic]") {
    // The field's class HAS a constructor (even if one overload is zero-arg-callable) — it is not
    // ctor-less, so the field must still be explicitly initialized.
    StderrCapture cap;
    auto result = analyzeString(R"(
        class Slot { mut i32 v; Slot() { v = 0; } Slot(i32 x) { v = x; } }
        class Bag { mut Slot s; Bag() { } }
        fn main() -> i32 { return 0; }
    )");
    REQUIRE(result.hadError);
    REQUIRE(cap.contains("field 's' is not initialized in constructor 'Bag'"));
}

TEST_CASE("FieldInit - a '{args}' initializer direct-constructs with no temp or clone",
          "[fieldinit][codegen]") {
    std::string ir = codegenString(R"(
        class Slot { mut i32 v; Slot(i32 x) { v = x; } }
        class Bag { mut Slot s{7}; mut i32 count = 0; }
        fn main() -> i32 { Bag b(); return b.count + b.s.v; }
    )");
    // The synthesized Bag() constructor GEPs to `s` and calls Slot's ctor directly into it.
    REQUIRE(ir.find("call void @Slot_Slot(ptr ") != std::string::npos);
    REQUIRE(ir.find("@Slot_clone") == std::string::npos);
    REQUIRE(ir.find("define void @Bag_Bag(ptr %self)") != std::string::npos);
}

TEST_CASE("FieldInit - default initializers run before the constructor's own body",
          "[fieldinit][codegen]") {
    // Field defaults are stores that must appear, in order, before the body's own override.
    std::string ir = codegenString(R"(
        class Widget {
            mut i32 v = 10;
            mut i32 extra = 0;
            Widget(i32 x) { v = x; }
        }
        fn main() -> i32 { Widget w(5); return w.v + w.extra; }
    )");
    size_t ctorStart = ir.find("define void @Widget_Widget(");
    REQUIRE(ctorStart != std::string::npos);
    size_t posDefaultV      = ir.find("store i32 10,", ctorStart);
    size_t posDefaultExtra  = ir.find("store i32 0,", ctorStart);
    size_t posLoadParam     = ir.find("load i32, ptr %x.addr", ctorStart);
    REQUIRE(posDefaultV != std::string::npos);
    REQUIRE(posDefaultExtra != std::string::npos);
    REQUIRE(posLoadParam != std::string::npos);
    // Both defaults land before the body reads/stores the constructor's own parameter.
    REQUIRE(posDefaultV < posLoadParam);
    REQUIRE(posDefaultExtra < posLoadParam);
}

TEST_CASE("FieldInit - each constructor overload is checked independently",
          "[fieldinit][semantic]") {
    // Widget(i32) sets both fields; Widget(char) leaves 'w' unset. Only the second should error —
    // per-constructor tracking must not leak between overloads of the same class.
    StderrCapture cap;
    auto result = analyzeString(R"(
        class Widget {
            mut i32 v;
            mut i32 w;
            Widget(i32 x) { v = x; w = 0; }
            Widget(char c) { v = c as i32; }
        }
        fn main() -> i32 { return 0; }
    )");
    REQUIRE(result.hadError);
    REQUIRE(cap.contains("field 'w' is not initialized in constructor 'Widget'"));
}

TEST_CASE("FieldInit - a correctly-initializing overload does not error even when a sibling does",
          "[fieldinit][semantic]") {
    // Same class as above, isolated: the i32 overload alone must type-check clean.
    auto result = analyzeString(R"(
        class Widget { mut i32 v; mut i32 w; Widget(i32 x) { v = x; w = 0; } }
        fn main() -> i32 { Widget wi(1); return wi.v + wi.w; }
    )");
    REQUIRE_FALSE(result.hadError);
}

TEST_CASE("FieldInit - a synthesized constructor coexists with a user-written destructor",
          "[fieldinit][codegen]") {
    std::string ir = codegenString(R"(
        class Tracked { mut i32 count = 0; ~Tracked() { count = -1; } }
        fn main() -> i32 { Tracked t(); return t.count; }
    )");
    REQUIRE(ir.find("define void @Tracked_Tracked(ptr %self)") != std::string::npos);
    REQUIRE(ir.find("define void @Tracked_dtor(ptr %self)") != std::string::npos);
}

TEST_CASE("FieldInit - a generic class's field default is independent per instantiation",
          "[fieldinit][generic][codegen]") {
    std::string ir = codegenString(R"(
        class Box<T> { mut i32 tag = 99; }
        fn main() -> i32 { Box<i32> a(); Box<char> b(); return a.tag + b.tag; }
    )");
    REQUIRE(ir.find("define void @Box$i32_Box$i32(ptr %self)") != std::string::npos);
    REQUIRE(ir.find("define void @Box$char_Box$char(ptr %self)") != std::string::npos);
}

TEST_CASE("FieldInit - a field default may use the class's own type parameter",
          "[fieldinit][generic][semantic]") {
    auto result = analyzeString(R"(
        class Box<T> { mut T val = 0; }
        fn main() -> i32 { Box<i32> a(); return a.val; }
    )");
    REQUIRE_FALSE(result.hadError);
}

TEST_CASE("FieldInit - a default initializer MAY reference the class's own static field "
          "via an explicit qualified name", "[fieldinit][semantic]") {
    // Static access resolves via the type name, independent of the currentClassName-clearing guard
    // that blocks implicit this/field/param references — so this is allowed, unlike a bare name.
    auto result = analyzeString(R"(
        class Config { mut static i32 DEFAULT = 42; mut i32 v = Config::DEFAULT; }
        fn main() -> i32 { Config c(); return c.v; }
    )");
    REQUIRE_FALSE(result.hadError);
}

TEST_CASE("FieldInit - a bare (unqualified) reference to the class's own static field is "
          "rejected, matching analyzeParamDefaults' identical pre-existing limitation",
          "[fieldinit][semantic]") {
    // Asymmetric but INTENTIONALLY consistent: analyzeParamDefaults has the exact same bare-name
    // gap (also clears currentClassName), so a field default inherits it rather than introducing a
    // new inconsistency. The qualified form (above) is always the working escape hatch.
    StderrCapture cap;
    auto result = analyzeString(R"(
        class Config { mut static i32 DEFAULT = 42; mut i32 v = DEFAULT; }
        fn main() -> i32 { return 0; }
    )");
    REQUIRE(result.hadError);
    REQUIRE(cap.contains("use of undeclared identifier 'DEFAULT'"));
}

TEST_CASE("FieldInit - a ctor-less field class works when declared AFTER the class that embeds it",
          "[fieldinit][semantic]") {
    // The ctor-less exemption looks up the field's class in classRegistry — must not depend on
    // declaration order (collectClasses populates the whole registry before any body is analyzed).
    auto result = analyzeString(R"(
        class Line { mut Point p; Line() { } }
        class Point { mut i32 x; mut i32 y; }
        fn main() -> i32 { Line l(); return l.p.x; }
    )");
    REQUIRE_FALSE(result.hadError);
}
