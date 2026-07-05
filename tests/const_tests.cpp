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

TEST_CASE("Cast - an explicit 'as mut T' silences the const→mut warning", "[mut][cast][semantic]") {
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
    auto result = analyzeString(R"(
        class Counter { static i32 total; }
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
            Counter c;
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
            mut Counter c;
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
            Counter c;
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
