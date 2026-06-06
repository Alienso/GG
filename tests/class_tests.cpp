#include <catch2/catch_test_macros.hpp>
#include "helpers.h"

// ============================================================
// Parser tests
// ============================================================

TEST_CASE("Class - basic class parses to ClassDeclStmt", "[class][parser]") {
    auto prog = parseString(R"(
        class Point {
            public f32 x;
            public f32 y;
        }
    )");
    REQUIRE(prog.declarations.size() == 1);
    const auto& cls = asStmt<ClassDeclStmt>(prog.declarations[0]);
    REQUIRE(cls.name.lexeme == "Point");
    REQUIRE(cls.fields.size() == 2);
    REQUIRE(cls.fields[0].name.lexeme == "x");
    REQUIRE(cls.fields[1].name.lexeme == "y");
    REQUIRE(cls.fields[0].isPublic);
    REQUIRE(cls.fields[1].isPublic);
}

TEST_CASE("Class - constructor is parsed as MethodDecl with isConstructor=true", "[class][parser]") {
    auto prog = parseString(R"(
        class Vec {
            public f32 x;
            public Vec(f32 x) { this.x = x; }
        }
    )");
    REQUIRE(prog.declarations.size() == 1);
    const auto& cls = asStmt<ClassDeclStmt>(prog.declarations[0]);
    REQUIRE(cls.methods.size() == 1);
    REQUIRE(cls.methods[0].isConstructor);
    REQUIRE(cls.methods[0].name.lexeme == "Vec");
    REQUIRE(cls.methods[0].params.size() == 1);
}

TEST_CASE("Class - regular method is parsed as MethodDecl with isConstructor=false", "[class][parser]") {
    auto prog = parseString(R"(
        class Counter {
            public i32 value;
            public i32 get() { return this.value; }
        }
    )");
    REQUIRE(prog.declarations.size() == 1);
    const auto& cls = asStmt<ClassDeclStmt>(prog.declarations[0]);
    REQUIRE(cls.methods.size() == 1);
    REQUIRE_FALSE(cls.methods[0].isConstructor);
    REQUIRE(cls.methods[0].name.lexeme == "get");
}

TEST_CASE("Class - private member parses with isPublic=false", "[class][parser]") {
    auto prog = parseString(R"(
        class Foo {
            private i32 secret;
            public i32 visible;
        }
    )");
    const auto& cls = asStmt<ClassDeclStmt>(prog.declarations[0]);
    REQUIRE_FALSE(cls.fields[0].isPublic);  // secret
    REQUIRE(cls.fields[1].isPublic);        // visible
}

TEST_CASE("Class - field access parses to MemberAccessExpr", "[class][parser]") {
    auto prog = parseString(R"(
        class P { public f32 x; }
        f32 foo() {
            P p;
            return p.x;
        }
    )");
    // second declaration is the function
    const auto& func = asStmt<FunctionDeclStmt>(prog.declarations[1]);
    // second statement is return p.x
    const auto& ret = asStmt<ReturnStmt>(*func.body.body[1]);
    const auto& ma  = asExpr<MemberAccessExpr>(*ret.value);
    REQUIRE(ma.field.lexeme == "x");
    const auto& obj = asExpr<IdentifierExpr>(*ma.object);
    REQUIRE(obj.name.lexeme == "p");
}

TEST_CASE("Class - method call parses to MethodCallExpr", "[class][parser]") {
    auto prog = parseString(R"(
        class C { public i32 val() { return 0; } }
        i32 main() {
            C c;
            return c.val();
        }
    )");
    const auto& func = asStmt<FunctionDeclStmt>(prog.declarations[1]);
    const auto& ret  = asStmt<ReturnStmt>(*func.body.body[1]);
    const auto& mc   = asExpr<MethodCallExpr>(*ret.value);
    REQUIRE(mc.method.lexeme == "val");
}

TEST_CASE("Class - field assignment parses to MemberAssignExpr", "[class][parser]") {
    auto prog = parseString(R"(
        class P { public f32 x; }
        void foo() {
            P p;
            p.x = 1.0;
        }
    )");
    const auto& func  = asStmt<FunctionDeclStmt>(prog.declarations[1]);
    const auto& exprS = asStmt<ExprStmt>(*func.body.body[1]);
    const auto& ma    = asExpr<MemberAssignExpr>(exprS.expression);
    REQUIRE(ma.field.lexeme == "x");
}

TEST_CASE("Class - constructor call syntax parses to VarDecl with CallExpr initializer", "[class][parser]") {
    auto prog = parseString(R"(
        class P { public f32 x; public P(f32 x) { this.x = x; } }
        void main() {
            P p(1.0);
        }
    )");
    const auto& func  = asStmt<FunctionDeclStmt>(prog.declarations[1]);
    const auto& exprS = asStmt<ExprStmt>(*func.body.body[0]);
    const auto& vd    = asExpr<VarDeclExpr>(exprS.expression);
    REQUIRE(vd.typeName.lexeme == "P");
    REQUIRE(vd.name.lexeme == "p");
    REQUIRE(vd.initializer != nullptr);
    const auto& ctor = asExpr<CallExpr>(*vd.initializer);
    REQUIRE(ctor.callee.lexeme == "P");
    REQUIRE(ctor.args.size() == 1);
}

// ============================================================
// Semantic tests
// ============================================================

TEST_CASE("Class - valid class with constructor and method passes", "[class][semantic]") {
    auto result = analyzeString(R"(
        class Point {
            public f32 x;
            public f32 y;
            public Point(f32 x, f32 y) {
                this.x = x;
                this.y = y;
            }
            public f32 sum() {
                return this.x + this.y;
            }
        }
        void main() {
            Point p(1.0, 2.0);
        }
    )");
    REQUIRE_FALSE(result.hadError);
}

TEST_CASE("Class - accessing undeclared member is an error", "[class][semantic]") {
    StderrCapture cap;
    auto result = analyzeString(R"(
        class Foo { public i32 a; }
        void main() {
            Foo f;
            i32 x = f.noSuchField;
        }
    )");
    REQUIRE(result.hadError);
}

TEST_CASE("Class - accessing private field from outside class is an error", "[class][semantic]") {
    StderrCapture cap;
    auto result = analyzeString(R"(
        class Foo { private i32 secret; }
        void main() {
            Foo f;
            i32 x = f.secret;
        }
    )");
    REQUIRE(result.hadError);
    REQUIRE(cap.contains("private"));
}

TEST_CASE("Class - accessing private field from inside class is valid", "[class][semantic]") {
    auto result = analyzeString(R"(
        class Foo {
            private i32 secret;
            public i32 get() { return this.secret; }
        }
    )");
    REQUIRE_FALSE(result.hadError);
}

TEST_CASE("Class - constructor argument count mismatch is an error", "[class][semantic]") {
    StderrCapture cap;
    auto result = analyzeString(R"(
        class Vec {
            public f32 x;
            public Vec(f32 x) { this.x = x; }
        }
        void main() {
            Vec v(1.0, 2.0);
        }
    )");
    REQUIRE(result.hadError);
    REQUIRE(cap.contains("expects"));
}

TEST_CASE("Class - method argument count mismatch is an error", "[class][semantic]") {
    StderrCapture cap;
    auto result = analyzeString(R"(
        class Calc {
            public i32 add(i32 a, i32 b) { return a + b; }
        }
        void main() {
            Calc c;
            c.add(1);
        }
    )");
    REQUIRE(result.hadError);
    REQUIRE(cap.contains("expects"));
}

TEST_CASE("Class - 'this' outside class method is an error", "[class][semantic]") {
    StderrCapture cap;
    auto result = analyzeString(R"(
        void foo() { i32 x = this.y; }
    )");
    REQUIRE(result.hadError);
    REQUIRE(cap.contains("outside of a class"));
}

TEST_CASE("Class - zero-argument constructor call is valid when no constructor defined", "[class][semantic]") {
    auto result = analyzeString(R"(
        class Empty { public i32 x; }
        void main() { Empty e; }
    )");
    REQUIRE_FALSE(result.hadError);
}

TEST_CASE("Class - field type mismatch in assignment is an error", "[class][semantic]") {
    StderrCapture cap;
    auto result = analyzeString(R"(
        class P { public i32 x; }
        void main() {
            P p;
            p.x = "hello";
        }
    )");
    REQUIRE(result.hadError);
    REQUIRE(cap.contains("cannot implicitly convert"));
}

// ============================================================
// CodeGen tests
// ============================================================

TEST_CASE("Class - struct type declaration emitted in IR", "[class][codegen]") {
    std::string ir = codegenString(R"(
        class Point {
            public f32 x;
            public f32 y;
        }
    )");
    REQUIRE(ir.find("%Point = type { float, float }") != std::string::npos);
}

TEST_CASE("Class - method emitted with mangled name and ptr self parameter", "[class][codegen]") {
    std::string ir = codegenString(R"(
        class Counter {
            public i32 value;
            public i32 get() { return this.value; }
        }
    )");
    REQUIRE(ir.find("@Counter_get(ptr %self)") != std::string::npos);
}

TEST_CASE("Class - constructor emitted as void mangled function", "[class][codegen]") {
    std::string ir = codegenString(R"(
        class Vec {
            public f32 x;
            public Vec(f32 x) { this.x = x; }
        }
    )");
    REQUIRE(ir.find("define void @Vec_Vec(ptr %self") != std::string::npos);
}

TEST_CASE("Class - stack alloca emitted for object variable", "[class][codegen]") {
    std::string ir = codegenString(R"(
        class Point { public f32 x; public f32 y; }
        void main() { Point p; }
    )");
    REQUIRE(ir.find("%p.addr = alloca %Point") != std::string::npos);
}

TEST_CASE("Class - zeroinitializer emitted for object variable", "[class][codegen]") {
    std::string ir = codegenString(R"(
        class Point { public f32 x; public f32 y; }
        void main() { Point p; }
    )");
    REQUIRE(ir.find("store %Point zeroinitializer, ptr %p.addr") != std::string::npos);
}

TEST_CASE("Class - constructor call emitted after alloca + zeroinit", "[class][codegen]") {
    std::string ir = codegenString(R"(
        class Point {
            public f32 x;
            public f32 y;
            public Point(f32 x, f32 y) { this.x = x; this.y = y; }
        }
        void main() { Point p(1.0, 2.0); }
    )");
    REQUIRE(ir.find("call void @Point_Point(ptr %p.addr") != std::string::npos);
}

TEST_CASE("Class - field read produces getelementptr + load", "[class][codegen]") {
    std::string ir = codegenString(R"(
        class Point { public f32 x; public f32 y; }
        f32 getX(Point& p) {
            return p.x;
        }
    )");
    REQUIRE(ir.find("getelementptr %Point") != std::string::npos);
    REQUIRE(ir.find("load float") != std::string::npos);
}

TEST_CASE("Class - method call emitted with self pointer as first argument", "[class][codegen]") {
    std::string ir = codegenString(R"(
        class Counter {
            public i32 value;
            public i32 get() { return this.value; }
        }
        i32 main() {
            Counter c;
            return c.get();
        }
    )");
    REQUIRE(ir.find("call i32 @Counter_get(ptr %c.addr)") != std::string::npos);
}

// ============================================================
// Destructor tests
// ============================================================

TEST_CASE("Destructor - parser sets isDestructor=true on MethodDecl", "[class][destructor][parser]") {
    auto prog = parseString(R"(
        class Box {
            public i32 value;
            public Box(i32 v) { this.value = v; }
            public ~Box() { }
        }
    )");
    REQUIRE(prog.declarations.size() == 1);
    const auto& cls = asStmt<ClassDeclStmt>(prog.declarations[0]);
    // Should have 2 methods: constructor + destructor
    REQUIRE(cls.methods.size() == 2);
    bool foundDtor = false;
    for (const auto& m : cls.methods) {
        if (m.isDestructor) {
            foundDtor = true;
            REQUIRE_FALSE(m.isConstructor);
            REQUIRE(m.params.empty());
            REQUIRE(m.name.lexeme == "Box");
        }
    }
    REQUIRE(foundDtor);
}

TEST_CASE("Destructor - class without destructor has no _dtor function in IR", "[class][destructor][codegen]") {
    std::string ir = codegenString(R"(
        class Counter {
            public i32 value;
            public i32 get() { return this.value; }
        }
        i32 main() {
            Counter c;
            return c.get();
        }
    )");
    REQUIRE(ir.find("_dtor") == std::string::npos);
}

TEST_CASE("Destructor - @ClassName_dtor function is emitted", "[class][destructor][codegen]") {
    std::string ir = codegenString(R"(
        class Box {
            public i32 value;
            public ~Box() { }
        }
        i32 main() {
            Box b;
            return 0;
        }
    )");
    REQUIRE(ir.find("define void @Box_dtor(ptr %self)") != std::string::npos);
}

TEST_CASE("Destructor - called at end of enclosing block", "[class][destructor][codegen]") {
    std::string ir = codegenString(R"(
        class Box {
            public i32 value;
            public ~Box() { }
        }
        i32 main() {
            Box b;
            return 0;
        }
    )");
    // The dtor call must appear before the final ret
    auto dtorPos = ir.find("call void @Box_dtor(ptr %b.addr)");
    auto retPos  = ir.find("ret i32");
    REQUIRE(dtorPos != std::string::npos);
    REQUIRE(retPos  != std::string::npos);
    REQUIRE(dtorPos < retPos);
}

TEST_CASE("Destructor - called before early return", "[class][destructor][codegen]") {
    std::string ir = codegenString(R"(
        class Box {
            public i32 value;
            public ~Box() { }
        }
        i32 main() {
            Box b;
            return 1;
        }
    )");
    auto dtorPos = ir.find("call void @Box_dtor(ptr %b.addr)");
    auto retPos  = ir.find("ret i32 1");
    REQUIRE(dtorPos != std::string::npos);
    REQUIRE(retPos  != std::string::npos);
    REQUIRE(dtorPos < retPos);
}

TEST_CASE("Destructor - multiple objects destroyed in reverse order", "[class][destructor][codegen]") {
    std::string ir = codegenString(R"(
        class A {
            public i32 v;
            public ~A() { }
        }
        class B {
            public i32 v;
            public ~B() { }
        }
        i32 main() {
            A a;
            B b;
            return 0;
        }
    )");
    auto dtorA = ir.find("call void @A_dtor(ptr %a.addr)");
    auto dtorB = ir.find("call void @B_dtor(ptr %b.addr)");
    REQUIRE(dtorA != std::string::npos);
    REQUIRE(dtorB != std::string::npos);
    // B declared after A, so B must be destroyed first (dtorB < dtorA)
    REQUIRE(dtorB < dtorA);
}

TEST_CASE("Destructor - semantic error: duplicate destructor", "[class][destructor][semantic]") {
    auto result = analyzeString(R"(
        class Box {
            public i32 value;
            public ~Box() { }
            public ~Box() { }
        }
    )");
    REQUIRE(result.hadError);
}

TEST_CASE("Destructor - parser: destructor with params produces no destructor in class", "[class][destructor][parser]") {
    // The parser enforces no-params for destructors via a ParseError that is then
    // synchronised over.  The resulting ClassDeclStmt will therefore have no
    // destructor MethodDecl (the partial parse is discarded during synchronize()).
    auto prog = parseString(R"(
        class Box {
            public i32 value;
            public ~Box(i32 x) { }
        }
    )");
    // The class may not even be in the declarations (ParseError clears it).
    // What matters is that no destructor with params made it through.
    bool dtorWithParamsFound = false;
    for (const auto& decl : prog.declarations) {
        if (!std::holds_alternative<ClassDeclStmt>(*decl.node)) continue;
        const auto& cls = std::get<ClassDeclStmt>(*decl.node);
        for (const auto& m : cls.methods)
            if (m.isDestructor && !m.params.empty()) dtorWithParamsFound = true;
    }
    REQUIRE_FALSE(dtorWithParamsFound);
}

// ============================================================
// Object parameters must be references (raw value objects rejected);
// primitives are still passed by value.
// ============================================================

TEST_CASE("ObjectParam - a raw value-object parameter is rejected", "[class][semantic][byref]") {
    auto result = analyzeString(R"(
        class Point { public f32 x; public f32 y; }
        void use(Point p) { }
    )");
    REQUIRE(result.hadError);   // must be declared 'Point&'
}

TEST_CASE("ObjectParam - a reference parameter is accepted", "[class][semantic][byref]") {
    auto result = analyzeString(R"(
        class Point { public f32 x; public f32 y; }
        void use(Point& p) { p.x = 1.0; }
    )");
    REQUIRE_FALSE(result.hadError);
}

TEST_CASE("ObjectParam - a method with a raw object parameter is rejected", "[class][semantic][byref]") {
    auto result = analyzeString(R"(
        class Point { public f32 x; }
        class Line { public f32 len; public void from(Point a) { this.len = a.x; } }
    )");
    REQUIRE(result.hadError);
}

TEST_CASE("ObjectParam - passing a raw value object as an argument is rejected", "[class][semantic][byref]") {
    auto result = analyzeString(R"(
        class Point { public f32 x; public Point(f32 v) { this.x = v; } }
        void use(Point& p) { }
        void main() { Point p(1.0); use(p); }
    )");
    REQUIRE(result.hadError);   // p is a value object; pass a reference instead
}

TEST_CASE("ObjectParam - basic-type parameter is still passed by value", "[class][codegen][byref]") {
    std::string ir = codegenString(R"(
        void use(i32 n) { n = n + 1; }
    )");
    REQUIRE(ir.find("define void @use(i32 %n)") != std::string::npos);
    REQUIRE(ir.find("%n.addr = alloca i32")      != std::string::npos);
    REQUIRE(ir.find("store i32 %n, ptr %n.addr") != std::string::npos);
}

TEST_CASE("ObjectParam - reassigning a basic-type parameter is still allowed", "[class][semantic][byref]") {
    auto result = analyzeString(R"(
        void use(i32 n) { n = 5; }
    )");
    REQUIRE_FALSE(result.hadError);
}

// ============================================================
// Reference parameters (Point&) — Phase 0 of new/refcount
// (Parsing + type resolution + signature lowering. Dereferencing
//  through a reference and `new` come in later steps.)
// ============================================================

TEST_CASE("Reference param - parses as a '<Class>&' type token", "[reference][parser]") {
    auto ast = parseString(R"(
        class Point { public i32 x; }
        void take(Point& p) { }
    )");
    REQUIRE(ast.declarations.size() == 2);
    const auto& fn = asStmt<FunctionDeclStmt>(ast.declarations[1]);
    REQUIRE(fn.params.size() == 1);
    REQUIRE(fn.params[0].typeName.type   == TokenType::IDENTIFIER);
    REQUIRE(fn.params[0].typeName.lexeme == "Point&");
}

TEST_CASE("Reference param - type-checks without error", "[reference][semantic]") {
    auto result = analyzeString(R"(
        class Point { public i32 x; }
        void take(Point& p) { }
    )");
    REQUIRE_FALSE(result.hadError);
}

TEST_CASE("Reference param - lowers to ptr in the signature", "[reference][codegen]") {
    std::string ir = codegenString(R"(
        class Point { public i32 x; public i32 y; }
        void take(Point& p) { }
    )");
    REQUIRE(ir.find("define void @take(ptr %p)") != std::string::npos);
}

TEST_CASE("Reference param - method taking a reference lowers to ptr", "[reference][codegen]") {
    std::string ir = codegenString(R"(
        class Point { public i32 x; }
        class Line {
            public i32 n;
            public void use(Point& p) { }
        }
    )");
    REQUIRE(ir.find("define void @Line_use(ptr %self, ptr %p)") != std::string::npos);
}

TEST_CASE("Reference param - '&' on a primitive type is rejected", "[reference][parser]") {
    StderrCapture cap;
    auto ast = parseString("void f(i32& x) { }");
    REQUIRE(ast.declarations.empty());   // parse error clears the program
}

// ============================================================
// new operator + heap references (refcounted)
// ============================================================

TEST_CASE("New - parses to a NewExpr with class name and args", "[new][parser]") {
    auto ast = parseString(R"(
        class Point { public i32 x; public i32 y; public Point(i32 a, i32 b){ this.x=a; this.y=b; } }
        void main() { Point& p = new Point(1, 2); }
    )");
    const auto& fn = asStmt<FunctionDeclStmt>(ast.declarations[1]);
    const auto& vd = asExpr<VarDeclExpr>(asStmt<ExprStmt>(*fn.body.body[0]).expression);
    REQUIRE(vd.typeName.lexeme == "Point&");
    REQUIRE(vd.initializer != nullptr);
    const auto& ne = asExpr<NewExpr>(*vd.initializer);
    REQUIRE(ne.className.lexeme == "Point");
    REQUIRE(ne.args.size() == 2);
}

TEST_CASE("New - 'new' on a non-class name is rejected at parse time", "[new][parser]") {
    StderrCapture cap;
    auto ast = parseString("void main() { new Nope(); }");
    REQUIRE(ast.declarations.empty());
}

TEST_CASE("New - 'Class& r = new Class(..)' type-checks", "[new][semantic]") {
    auto r = analyzeString(R"(
        class Point { public i32 x; public Point(i32 a){ this.x=a; } }
        void main() { Point& p = new Point(5); }
    )");
    REQUIRE_FALSE(r.hadError);
}

TEST_CASE("New - assigning a mismatched reference class is an error", "[new][semantic]") {
    auto r = analyzeString(R"(
        class A { public i32 x; public A(i32 a){ this.x=a; } }
        class B { public i32 y; public B(i32 a){ this.y=a; } }
        void main() { A& a = new B(1); }
    )");
    REQUIRE(r.hadError);
}

TEST_CASE("New - wrong constructor argument count is an error", "[new][semantic]") {
    auto r = analyzeString(R"(
        class Point { public i32 x; public Point(i32 a){ this.x=a; } }
        void main() { Point& p = new Point(1, 2); }
    )");
    REQUIRE(r.hadError);
}

TEST_CASE("New - emits gg_alloc, sizeof, and the refcount runtime", "[new][codegen]") {
    auto ir = codegenString(R"(
        class Point { public i32 x; public Point(i32 a){ this.x=a; } }
        void main() { Point& p = new Point(5); }
    )");
    REQUIRE(ir.find("getelementptr %Point, ptr null, i32 1") != std::string::npos);  // sizeof
    REQUIRE(ir.find("call ptr @gg_alloc(")    != std::string::npos);
    REQUIRE(ir.find("define ptr @gg_alloc(")  != std::string::npos);
    REQUIRE(ir.find("define void @gg_retain(")  != std::string::npos);
    REQUIRE(ir.find("define void @gg_release(") != std::string::npos);
}

TEST_CASE("New - reference owner is released at scope exit", "[new][codegen]") {
    auto ir = codegenString(R"(
        class Point { public i32 x; public Point(i32 a){ this.x=a; } }
        void main() { Point& p = new Point(5); }
    )");
    REQUIRE(ir.find("call void @gg_release(") != std::string::npos);
    REQUIRE(ir.find(", ptr null)")            != std::string::npos);  // no dtor → null
}

TEST_CASE("New - class with a destructor passes its dtor to gg_release", "[new][codegen]") {
    auto ir = codegenString(R"(
        class Res { public i32 x; public Res(i32 a){ this.x=a; } public ~Res(){ } }
        void main() { Res& r = new Res(5); }
    )");
    REQUIRE(ir.find(", ptr @Res_dtor)") != std::string::npos);
}

TEST_CASE("New - no 'new' means no refcount runtime is emitted", "[new][codegen]") {
    auto ir = codegenString(R"(
        void main() { i32 x = 1; }
    )");
    REQUIRE(ir.find("@gg_alloc")   == std::string::npos);
    REQUIRE(ir.find("@gg_release") == std::string::npos);
}

TEST_CASE("New - member access through a reference loads the pointer then GEPs", "[new][codegen]") {
    auto ir = codegenString(R"(
        class Point { public i32 x; public Point(i32 a){ this.x=a; } }
        i32 main() { Point& p = new Point(5); return p.x; }
    )");
    REQUIRE(ir.find("load ptr, ptr %p.addr")        != std::string::npos);
    REQUIRE(ir.find("getelementptr %Point, ptr %t") != std::string::npos);
}

TEST_CASE("New - method call through a reference dispatches to the mangled method", "[new][codegen]") {
    auto ir = codegenString(R"(
        class Point {
            public i32 x;
            public Point(i32 a){ this.x=a; }
            public i32 get() { return this.x; }
        }
        i32 main() { Point& p = new Point(9); return p.get(); }
    )");
    REQUIRE(ir.find("call i32 @Point_get(ptr %t") != std::string::npos);
}

// ============================================================
// Reference co-ownership (retain on copy, release+retain on rebind)
// ============================================================

TEST_CASE("Refcount - copying a reference retains it", "[new][refcount][codegen]") {
    auto ir = codegenString(R"(
        class Point { public i32 x; public Point(i32 a){ this.x=a; } }
        void main() { Point& a = new Point(1); Point& b = a; }
    )");
    REQUIRE(ir.find("call void @gg_retain(") != std::string::npos);
}

TEST_CASE("Refcount - a 'new'-initialised reference is not retained", "[new][refcount][codegen]") {
    auto ir = codegenString(R"(
        class Point { public i32 x; public Point(i32 a){ this.x=a; } }
        void main() { Point& a = new Point(1); }
    )");
    REQUIRE(ir.find("call void @gg_retain(")  == std::string::npos);
    REQUIRE(ir.find("call void @gg_release(") != std::string::npos);
}

TEST_CASE("Refcount - reference reassignment releases old and retains new", "[new][refcount][codegen]") {
    auto ir = codegenString(R"(
        class Point { public i32 x; public Point(i32 a){ this.x=a; } }
        void main() {
            Point& a = new Point(1);
            Point& b = new Point(2);
            a = b;
        }
    )");
    REQUIRE(ir.find("call void @gg_retain(")  != std::string::npos);   // retain b on rebind
    REQUIRE(ir.find("call void @gg_release(") != std::string::npos);   // release a's old target
}

TEST_CASE("Refcount - reassigning a reference to 'new' does not retain", "[new][refcount][codegen]") {
    auto ir = codegenString(R"(
        class Point { public i32 x; public Point(i32 a){ this.x=a; } }
        void main() {
            Point& a = new Point(1);
            a = new Point(2);
        }
    )");
    REQUIRE(ir.find("call void @gg_retain(")  == std::string::npos);
    REQUIRE(ir.find("call void @gg_release(") != std::string::npos);
}

TEST_CASE("Refcount - reassigning a reference parameter is an error", "[new][reference][semantic]") {
    auto r = analyzeString(R"(
        class Point { public i32 x; public Point(i32 a){ this.x=a; } }
        void f(Point& p) { p = new Point(9); }
    )");
    REQUIRE(r.hadError);
}

TEST_CASE("Refcount - retain/release runtime is null-safe", "[new][refcount][codegen]") {
    auto ir = codegenString(R"(
        class Point { public i32 x; public Point(i32 a){ this.x=a; } }
        void main() { Point& a = new Point(1); }
    )");
    // null guards present in both helpers
    REQUIRE(ir.find("icmp eq ptr %obj, null") != std::string::npos);
}

// ============================================================
// Reference fields (Class& field) + recursive release
// ============================================================

TEST_CASE("RefField - field declared as Class& parses with '&' type", "[reffield][parser]") {
    auto ast = parseString(R"(
        class Node { public i32 v; public Node& next; }
    )");
    const auto& cls = asStmt<ClassDeclStmt>(ast.declarations[0]);
    REQUIRE(cls.fields.size() == 2);
    REQUIRE(cls.fields[1].typeName.lexeme == "Node&");
}

TEST_CASE("RefField - a class with a reference field type-checks", "[reffield][semantic]") {
    auto r = analyzeString(R"(
        class Node { public i32 v; public Node& next; public Node(i32 x){ this.v=x; } }
    )");
    REQUIRE_FALSE(r.hadError);
}

TEST_CASE("RefField - a value-object field is rejected", "[reffield][semantic]") {
    auto r = analyzeString(R"(
        class Point { public i32 x; }
        class Bad   { public Point p; }
    )");
    REQUIRE(r.hadError);
}

TEST_CASE("RefField - reference field lowers to a ptr slot in the struct", "[reffield][codegen]") {
    auto ir = codegenString(R"(
        class Node { public i32 v; public Node& next; public Node(i32 x){ this.v=x; } }
        void main() { Node& a = new Node(1); }
    )");
    REQUIRE(ir.find("%Node = type { i32, ptr }") != std::string::npos);
}

TEST_CASE("RefField - assigning a reference field retains new and releases old", "[reffield][codegen]") {
    auto ir = codegenString(R"(
        class Node { public i32 v; public Node& next; public Node(i32 x){ this.v=x; } }
        void main() {
            Node& a = new Node(1);
            Node& b = new Node(2);
            a.next = b;
        }
    )");
    REQUIRE(ir.find("call void @gg_retain(")  != std::string::npos);
    REQUIRE(ir.find("call void @gg_release(") != std::string::npos);
}

TEST_CASE("RefField - class with a ref field but no user dtor gets a synthesized destructor", "[reffield][codegen]") {
    auto ir = codegenString(R"(
        class Leaf   { public i32 v; public Leaf(i32 x){ this.v=x; } }
        class Holder { public Leaf& leaf; }
        void main() { Holder& h = new Holder(); }
    )");
    REQUIRE(ir.find("define void @Holder_dtor(ptr %self)") != std::string::npos);
    REQUIRE(ir.find("call void @gg_release(") != std::string::npos);
}

TEST_CASE("RefField - destructor releases reference fields by GEP index", "[reffield][codegen]") {
    auto ir = codegenString(R"(
        class Node { public i32 v; public Node& next; public Node(i32 x){ this.v=x; } public ~Node(){ } }
        void main() { Node& a = new Node(1); }
    )");
    auto dtorPos = ir.find("define void @Node_dtor(ptr %self)");
    REQUIRE(dtorPos != std::string::npos);
    auto body = ir.substr(dtorPos);
    REQUIRE(body.find("getelementptr %Node, ptr %self, i32 0, i32 1") != std::string::npos);
    REQUIRE(body.find("call void @gg_release(") != std::string::npos);
}

// ============================================================
// clone / deep copy (copy constructor + value-copy assignment)
// ============================================================

TEST_CASE("Clone - 'new Class(value)' copy-constructs via @Class_clone", "[clone][codegen]") {
    auto ir = codegenString(R"(
        class Point { public i32 x; public i32 y; public Point(i32 a, i32 b){ this.x=a; this.y=b; } }
        void main() { Point p(1, 2); Point& r = new Point(p); }
    )");
    REQUIRE(ir.find("call void @Point_clone(") != std::string::npos);
    REQUIRE(ir.find("define void @Point_clone(ptr %dest, ptr %src)") != std::string::npos);
}

TEST_CASE("Clone - copy constructor type-checks", "[clone][semantic]") {
    auto r = analyzeString(R"(
        class Point { public i32 x; public Point(i32 a){ this.x=a; } }
        void main() { Point p(1); Point& q = new Point(p); }
    )");
    REQUIRE_FALSE(r.hadError);
}

TEST_CASE("Clone - value-object assignment deep-copies via clone", "[clone][codegen]") {
    auto ir = codegenString(R"(
        class Point { public i32 x; public Point(i32 a){ this.x=a; } }
        void main() { Point p(1); Point q(2); q = p; }
    )");
    REQUIRE(ir.find("call void @Point_clone(") != std::string::npos);
}

TEST_CASE("Clone - value copy-initialisation uses clone", "[clone][codegen]") {
    auto ir = codegenString(R"(
        class Point { public i32 x; public Point(i32 a){ this.x=a; } }
        void main() { Point p(1); Point q = p; }
    )");
    REQUIRE(ir.find("call void @Point_clone(") != std::string::npos);
}

TEST_CASE("Clone - value = reference derefs and clones", "[clone][codegen]") {
    auto ir = codegenString(R"(
        class Point { public i32 x; public Point(i32 a){ this.x=a; } }
        void main() { Point& r = new Point(5); Point v(0); v = r; }
    )");
    REQUIRE(ir.find("call void @Point_clone(") != std::string::npos);
}

TEST_CASE("Clone - clone retains reference fields (option 3)", "[clone][codegen]") {
    auto ir = codegenString(R"(
        class Leaf   { public i32 v; public Leaf(i32 x){ this.v=x; } }
        class Holder { public Leaf& leaf; }
        void main() { Holder& a = new Holder(); Holder& b = new Holder(a); }
    )");
    auto clonePos = ir.find("define void @Holder_clone(ptr %dest, ptr %src)");
    REQUIRE(clonePos != std::string::npos);
    auto body = ir.substr(clonePos);
    REQUIRE(body.find("call void @gg_retain(") != std::string::npos);
}

TEST_CASE("Clone - assigning a value to a reference variable is rejected", "[clone][semantic]") {
    auto r = analyzeString(R"(
        class Point { public i32 x; public Point(i32 a){ this.x=a; } }
        void main() { Point p(1); Point& q = new Point(2); q = p; }
    )");
    REQUIRE(r.hadError);   // Object -> Reference is not implicit; use new Point(p)
}
